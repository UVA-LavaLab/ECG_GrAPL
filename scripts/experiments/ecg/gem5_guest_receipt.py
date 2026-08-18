#!/usr/bin/env python3
"""Atomically build and verify material-input-bound gem5 guest kernels."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import errno
import fcntl
import hashlib
import importlib.util
import json
import multiprocessing
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[3]
GUEST_STRACE = Path("/usr/bin/strace")
GUEST_PROOT = PROJECT_ROOT / "bench/include/gem5_sim/.tools/proot"
GUEST_PROOT_LOADER = Path(
    "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2")
GUEST_PROOT_LIBC = Path("/usr/lib/x86_64-linux-gnu/libc.so.6")
GUEST_PROOT_TALLOC = Path("/usr/lib/x86_64-linux-gnu/libtalloc.so.2")
GUEST_FUSEPY = PROJECT_ROOT / "bench/include/gem5_sim/.tools/fusepy.py"
GUEST_LIBFUSE = PROJECT_ROOT / "bench/include/gem5_sim/.tools/libfuse.so.2"
GUEST_FUSERMOUNT = Path("/usr/bin/fusermount3")
GUEST_PYTHON = Path("/usr/bin/python3.12")
MATERIAL_COMPILER_ENV = (
    "PATH",
    "COMPILER_PATH",
    "GCC_EXEC_PREFIX",
    "LIBRARY_PATH",
    "CPATH",
    "CPLUS_INCLUDE_PATH",
)
LEGACY_ORCHESTRATION_DEPENDENCIES = frozenset({
    "Makefile",
    "scripts/experiments/ecg/gem5_guest_receipt.py",
})
TOOL_PATH_FIELDS = (
    "STRACE",
    "PROOT",
    "PROOT_LOADER",
    "PROOT_LIBC",
    "PROOT_TALLOC",
    "FUSEPY",
    "LIBFUSE",
    "FUSERMOUNT",
    "PYTHON",
)
LEGACY_BUILD_HASH_FIELDS = frozenset({
    "RISCV_CXX_SHA256",
    *(f"{name}_SHA256" for name in TOOL_PATH_FIELDS if name != "STRACE"),
})


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_receipt(path: Path) -> dict[str, str]:
    return {
        "path": str(path),
        "sha256": sha256(path),
    }


def stable_receipt_payload(payload: dict) -> dict:
    """Select reproducible guest-build fields, excluding trace churn."""
    return {
        key: payload.get(key)
        for key in (
            "schema_version",
            "binary",
            "canonical_command",
            "compiler",
            "flags",
            "includes",
            "link_inputs",
            "source",
            "build_config",
            "build_config_values",
            "make_target",
        )
    }


def stable_receipt_fingerprint(path: Path) -> str:
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid gem5 guest receipt {path}: {error}") from error
    return hashlib.sha256(json.dumps(
        stable_receipt_payload(payload),
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def material_input_fingerprint(
        path: Path, root: Path = PROJECT_ROOT) -> str:
    """Fingerprint current receipt dependencies and reject stale receipts."""
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid gem5 guest receipt {path}: {error}") from error
    dependencies = payload.get("dependencies")
    if not isinstance(dependencies, dict) or not dependencies:
        raise ValueError("gem5 guest receipt has no material dependencies")
    current = {}
    for name, expected in sorted(dependencies.items()):
        if payload.get("schema_version") == 2 and \
                name in LEGACY_ORCHESTRATION_DEPENDENCIES:
            continue
        dependency = Path(name)
        if not dependency.is_absolute():
            dependency = root / dependency
        dependency = dependency.resolve()
        if not dependency.is_file():
            raise ValueError(
                f"gem5 guest material input is missing: {name}")
        actual = sha256(dependency)
        if actual != expected:
            raise ValueError(
                f"gem5 guest material input changed: {name}")
        current[name] = actual
    return hashlib.sha256(json.dumps(
        current, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def resolve_compiler(compiler_text: str) -> Path:
    parts = shlex.split(compiler_text)
    if len(parts) != 1:
        raise ValueError(
            "RISCV_CXX must name one compiler executable; wrappers and "
            "embedded arguments are not supported")
    driver = shutil.which(parts[0])
    if not driver:
        raise ValueError(f"compiler not found: {parts[0]}")
    driver_path = Path(driver).resolve()
    if driver_path.read_bytes()[:4] != b"\x7fELF":
        raise ValueError("RISCV_CXX must be an ELF compiler, not a wrapper")
    return driver_path


def compiler_component(driver: Path, *arguments: str) -> dict[str, str]:
    output = subprocess.run(
        [str(driver), *arguments], capture_output=True, text=True,
        env=execution_environment(), check=True).stdout.strip()
    path = Path(output)
    if not path.is_absolute():
        candidate = shutil.which(output)
        path = Path(candidate) if candidate else path
    path = path.resolve()
    if not path.is_file():
        raise ValueError(
            f"compiler component is missing: {' '.join(arguments)} -> {path}")
    return {"path": str(path), "sha256": sha256(path)}


def compiler_receipt(compiler_text: str) -> dict:
    driver = resolve_compiler(compiler_text)
    version = subprocess.run(
        [str(driver), "--version"], capture_output=True, text=True,
        env=execution_environment(), check=True).stdout.splitlines()[0]
    dumpmachine = subprocess.run(
        [str(driver), "-dumpmachine"], capture_output=True, text=True,
        env=execution_environment(), check=True).stdout.strip()
    if dumpmachine != "riscv64-linux-gnu":
        raise ValueError(f"unexpected RISC-V compiler target: {dumpmachine}")
    return {
        "invoked": compiler_text,
        "driver": str(driver),
        "driver_sha256": sha256(driver),
        "version": version,
        "dumpmachine": dumpmachine,
        "cc1plus": compiler_component(
            driver, "-print-prog-name=cc1plus"),
        "collect2": compiler_component(
            driver, "-print-prog-name=collect2"),
        "libgcc": compiler_component(
            driver, "-print-libgcc-file-name"),
        "libstdcxx": compiler_component(
            driver, "-print-file-name=libstdc++.a"),
        "assembler": compiler_component(
            driver, "-print-prog-name=as"),
        "linker": compiler_component(
            driver, "-print-prog-name=ld"),
        "crt1": compiler_component(
            driver, "-print-file-name=crt1.o"),
        "crti": compiler_component(
            driver, "-print-file-name=crti.o"),
        "crtn": compiler_component(
            driver, "-print-file-name=crtn.o"),
        "crtbegin": compiler_component(
            driver, "-print-file-name=crtbeginT.o"),
        "crtend": compiler_component(
            driver, "-print-file-name=crtend.o"),
        "libgcc_eh": compiler_component(
            driver, "-print-file-name=libgcc_eh.a"),
        "libgomp": compiler_component(
            driver, "-print-file-name=libgomp.a"),
        "libc": compiler_component(
            driver, "-print-file-name=libc.a"),
        "libpthread": compiler_component(
            driver, "-print-file-name=libpthread.a"),
        "specs_sha256": hashlib.sha256(subprocess.run(
            [str(driver), "-dumpspecs"], capture_output=True,
            env=execution_environment(), check=True).stdout).hexdigest(),
        "search_dirs_sha256": hashlib.sha256(subprocess.run(
            [str(driver), "-print-search-dirs"], capture_output=True,
            env=execution_environment(), check=True).stdout).hexdigest(),
    }


def material_environment() -> dict[str, str]:
    return {
        "PATH": "/usr/bin:/bin",
        "COMPILER_PATH": "",
        "GCC_EXEC_PREFIX": "",
        "LIBRARY_PATH": "",
        "CPATH": "",
        "CPLUS_INCLUDE_PATH": "",
    }


def execution_environment() -> dict[str, str]:
    return {
        **{
            key: value for key, value in material_environment().items()
            if value
        },
        "TMPDIR": "/tmp",
        "HOME": "/tmp",
        "LC_ALL": "C",
        "LANG": "C",
    }


def parse_build_config(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text().splitlines():
        if not line or "=" not in line:
            raise ValueError(f"invalid RISC-V build config line: {line!r}")
        key, value = line.split("=", 1)
        if key in values:
            raise ValueError(f"duplicate RISC-V build config key: {key}")
        values[key] = value
    required = {
        "RISCV_CXX",
        "RISCV_CXX_RESOLVED",
        "CXXFLAGS_GEM5_RISCV",
        "INCLUDES",
        *TOOL_PATH_FIELDS,
        "HOME",
        "TMPDIR",
        "LC_ALL",
        "LANG",
        *MATERIAL_COMPILER_ENV,
    }
    fields = set(values)
    if not (required - {"STRACE"}) <= fields or fields - required - \
            LEGACY_BUILD_HASH_FIELDS:
        raise ValueError("RISC-V build config fields do not match schema")
    values.setdefault("STRACE", str(GUEST_STRACE))
    return values


def normalize_build_config_values(values: dict[str, str]) -> dict[str, str]:
    normalized = {
        key: value for key, value in values.items()
        if key not in LEGACY_BUILD_HASH_FIELDS
    }
    normalized.setdefault("STRACE", str(GUEST_STRACE))
    for field in ("RISCV_CXX_RESOLVED", *TOOL_PATH_FIELDS):
        if field not in normalized:
            raise ValueError(f"RISC-V build config lacks {field}")
        normalized[field] = str(Path(normalized[field]).resolve())
    return normalized


def validate_build_config(
        path: Path, compiler: str, flags: str, includes: str) -> dict[str, str]:
    values = normalize_build_config_values(parse_build_config(path))
    driver = resolve_compiler(compiler)
    expected = {
        "RISCV_CXX": compiler,
        "RISCV_CXX_RESOLVED": str(driver),
        "CXXFLAGS_GEM5_RISCV": flags,
        "INCLUDES": includes,
        "HOME": "/tmp",
        "TMPDIR": "/tmp",
        "LC_ALL": "C",
        "LANG": "C",
        **material_environment(),
    }
    if any(values.get(key) != value for key, value in expected.items()):
        raise ValueError(
            "requested compiler, flags, includes, or environment do not "
            "match .riscv_build_config")
    for field in TOOL_PATH_FIELDS:
        if not Path(values[field]).is_file():
            raise ValueError(
                f"RISC-V build tool is missing: {field}={values[field]}")
    return values


def tool_paths(values: dict[str, str]) -> dict[str, Path]:
    return {
        field: Path(values[field]).resolve()
        for field in TOOL_PATH_FIELDS
    }


def default_tool_paths() -> dict[str, Path]:
    return {
        "STRACE": GUEST_STRACE,
        "PROOT": GUEST_PROOT,
        "PROOT_LOADER": GUEST_PROOT_LOADER,
        "PROOT_LIBC": GUEST_PROOT_LIBC,
        "PROOT_TALLOC": GUEST_PROOT_TALLOC,
        "FUSEPY": GUEST_FUSEPY,
        "LIBFUSE": GUEST_LIBFUSE,
        "FUSERMOUNT": GUEST_FUSERMOUNT,
        "PYTHON": GUEST_PYTHON,
    }


def require_tool(path: Path, label: str) -> Path:
    if not path.is_file():
        raise ValueError(f"{label} is missing: {path}")
    return path


def require_trace_tool(tools: dict[str, Path]) -> Path:
    return require_tool(tools["STRACE"], "strace build tracer")


def require_proot(tools: dict[str, Path]) -> Path:
    for field in ("PROOT", "PROOT_LOADER", "PROOT_LIBC", "PROOT_TALLOC"):
        require_tool(tools[field], field.lower().replace("_", " "))
    return tools["PROOT"]


@contextmanager
def sealed_proot_command(
        tools: dict[str, Path]) -> tuple[list[str], tuple[int, ...]]:
    require_proot(tools)
    runtime = (
        tools["PROOT_LOADER"],
        tools["PROOT_LIBC"],
        tools["PROOT_TALLOC"],
        tools["PROOT"],
    )
    fds = []
    try:
        for path in runtime:
            fd, _name = open_sealed_guest(path, sha256(path))
            fds.append(fd)
        paths = [f"/proc/self/fd/{fd}" for fd in fds]
        command = [
            paths[0], "--preload", f"{paths[1]}:{paths[2]}", paths[3],
        ]
        yield command, tuple(fds)
    finally:
        for fd in fds:
            os.close(fd)


def require_fuse_tools(tools: dict[str, Path]) -> None:
    for field in ("FUSEPY", "LIBFUSE", "FUSERMOUNT"):
        require_tool(tools[field], field.lower().replace("_", " "))


def require_isolated_python(expected: Path) -> None:
    executable = Path(sys.executable).resolve()
    if executable != expected.resolve() or not sys.flags.isolated:
        raise ValueError(
            f"guest evidence builds require {expected} -I")


def run_traced_command(
        command: list[str], trace_path: Path, tracer: Path,
        cwd: Path = PROJECT_ROOT) -> None:
    require_tool(tracer, "strace build tracer")
    subprocess.run(
        [
            str(tracer), "-qq", "-f", "-yy", "-e", "trace=%file",
            "-o", str(trace_path), *command,
        ],
        cwd=cwd, env=execution_environment(), check=True)


def decode_trace_path(value: str) -> str:
    return json.loads(f'"{value}"')


def traced_input_paths(
        trace_path: Path, excluded_root: Path,
        root: Path = PROJECT_ROOT) -> dict[Path, Path]:
    paths = {}
    quoted = re.compile(r'"((?:\\.|[^"\\])*)"')
    relative_at = re.compile(
        r'(?:openat|newfstatat)\([^<]*<([^>]+)>[^,]*,\s*'
        r'"((?:\\.|[^"\\])*)"')
    for line in trace_path.read_text(errors="ignore").splitlines():
        at_match = relative_at.search(line)
        if at_match:
            base = Path(decode_trace_path(at_match.group(1)))
            value = Path(decode_trace_path(at_match.group(2)))
            path = value if value.is_absolute() else base / value
        else:
            match = quoted.search(line)
            if not match:
                continue
            path = Path(decode_trace_path(match.group(1)))
            if not path.is_absolute():
                path = root / path
        virtual_path = path.absolute()
        source_path = virtual_path.resolve()
        try:
            source_path.relative_to(excluded_root)
            continue
        except ValueError:
            pass
        if source_path.is_file():
            paths[virtual_path] = source_path
    return paths


def executable_interpreter_path(path: Path) -> Path | None:
    data = path.read_bytes()
    if data.startswith(b"#!"):
        first_line = data.splitlines()[0][2:].decode(
            errors="ignore").strip()
        if not first_line:
            return None
        return Path(shlex.split(first_line)[0])
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return None
    byteorder = "little" if data[5] == 1 else "big"
    if data[4] == 2:
        phoff = int.from_bytes(data[32:40], byteorder)
        phentsize = int.from_bytes(data[54:56], byteorder)
        phnum = int.from_bytes(data[56:58], byteorder)
        offset_field, offset_size = 8, 8
        filesz_field, filesz_size = 32, 8
    elif data[4] == 1:
        phoff = int.from_bytes(data[28:32], byteorder)
        phentsize = int.from_bytes(data[42:44], byteorder)
        phnum = int.from_bytes(data[44:46], byteorder)
        offset_field, offset_size = 4, 4
        filesz_field, filesz_size = 16, 4
    else:
        return None
    for index in range(phnum):
        start = phoff + index * phentsize
        header = data[start:start + phentsize]
        if len(header) < phentsize or \
                int.from_bytes(header[:4], byteorder) != 3:
            continue
        offset = int.from_bytes(
            header[offset_field:offset_field + offset_size], byteorder)
        size = int.from_bytes(
            header[filesz_field:filesz_field + filesz_size], byteorder)
        interpreter = Path(
            data[offset:offset + size].rstrip(b"\0").decode())
        return interpreter
    return None


def expand_interpreter_inputs(
        paths: dict[Path, Path]) -> dict[Path, Path]:
    expanded = dict(paths)
    for source in set(paths.values()):
        interpreter = executable_interpreter_path(source)
        if interpreter is not None and interpreter.is_file():
            expanded[interpreter] = interpreter.resolve()
    return expanded


def snapshot_path(root: Path, virtual_path: Path) -> Path:
    if not virtual_path.is_absolute():
        raise ValueError(f"snapshot path must be absolute: {virtual_path}")
    return root / virtual_path.relative_to("/")


def add_snapshot_symlinks(
        snapshot_root: Path, input_paths: Iterable[Path]) -> None:
    for alias, target in (
            (Path("/bin"), Path("/usr/bin")),
            (Path("/sbin"), Path("/usr/sbin")),
            (Path("/lib"), Path("/usr/lib"))):
        destination = snapshot_path(snapshot_root, alias)
        if not destination.exists() and not destination.is_symlink():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.symlink_to(target)
    for executable in input_paths:
        interpreter = executable_interpreter_path(executable)
        if interpreter is None or not interpreter.is_absolute():
            continue
        resolved = interpreter.resolve()
        if resolved == interpreter:
            continue
        destination = snapshot_path(snapshot_root, interpreter)
        if destination.exists() or destination.is_symlink():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.symlink_to(resolved)


def serve_immutable_fuse(
        mountpoint: str, files: dict[str, tuple[bytes, int]],
        fusepy_source: bytes, libfuse_fd: int) -> None:
    os.environ.clear()
    os.environ.update({
        "FUSE_LIBRARY_PATH": f"/proc/self/fd/{libfuse_fd}",
        "PATH": "/usr/bin:/bin",
        "LC_ALL": "C",
    })
    module = importlib.util.module_from_spec(
        importlib.util.spec_from_loader("graphbrew_fusepy", loader=None))
    module.__file__ = "receipt-fusepy.py"
    exec(compile(
        fusepy_source, module.__file__, "exec"), module.__dict__)

    class ImmutableFiles(module.Operations):
        def getattr(self, path, fh=None):
            if path == "/":
                return {
                    "st_mode": stat.S_IFDIR | 0o555,
                    "st_nlink": 2,
                }
            name = path.removeprefix("/")
            if name not in files:
                raise module.FuseOSError(errno.ENOENT)
            data, mode = files[name]
            return {
                "st_mode": stat.S_IFREG | mode,
                "st_nlink": 1,
                "st_size": len(data),
            }

        def readdir(self, path, fh):
            return [".", "..", *sorted(files)]

        def open(self, path, flags):
            if flags & (os.O_WRONLY | os.O_RDWR):
                raise module.FuseOSError(errno.EROFS)
            if path.removeprefix("/") not in files:
                raise module.FuseOSError(errno.ENOENT)
            return 0

        def read(self, path, size, offset, fh):
            data = files[path.removeprefix("/")][0]
            return data[offset:offset + size]

        def access(self, path, mode):
            if mode & os.W_OK:
                raise module.FuseOSError(errno.EROFS)
            if path != "/" and path.removeprefix("/") not in files:
                raise module.FuseOSError(errno.ENOENT)
            return 0

    module.FUSE(
        ImmutableFiles(), mountpoint,
        foreground=True, nothreads=True, ro=True,
        fsname="graphbrew-immutable-build")


@contextmanager
def immutable_fuse_files(
        files: dict[str, tuple[bytes, int]], mountpoint: Path,
        tools: dict[str, Path] | None = None):
    if tools is None:
        tools = default_tool_paths()
    require_fuse_tools(tools)
    if os.path.ismount(mountpoint):
        raise ValueError(f"immutable FUSE path is already mounted: {mountpoint}")
    fusepy_source = tools["FUSEPY"].read_bytes()
    fusepy_hash = hashlib.sha256(fusepy_source).hexdigest()
    if sha256(tools["FUSEPY"]) != fusepy_hash:
        raise ValueError("fusepy changed while loading")
    libfuse_fd, _libfuse_path = open_sealed_guest(
        tools["LIBFUSE"], sha256(tools["LIBFUSE"]))
    process = None
    try:
        mountpoint.mkdir(parents=True, exist_ok=True)
        process = multiprocessing.get_context("fork").Process(
            target=serve_immutable_fuse,
            args=(str(mountpoint), files, fusepy_source, libfuse_fd),
            daemon=True)
        process.start()
        for _ in range(100):
            if os.path.ismount(mountpoint):
                break
            if not process.is_alive():
                raise ValueError("immutable FUSE snapshot process exited")
            time.sleep(0.05)
        else:
            raise ValueError("immutable FUSE snapshot did not mount")
        yield
    finally:
        if os.path.ismount(mountpoint):
            subprocess.run(
                [str(tools["FUSERMOUNT"]), "-u", str(mountpoint)],
                capture_output=True, check=False)
        if os.path.ismount(mountpoint):
            subprocess.run(
                [str(tools["FUSERMOUNT"]), "-uz", str(mountpoint)],
                capture_output=True, check=False)
        if process is not None:
            process.join(5)
            if process.is_alive():
                process.terminate()
                process.join(5)
        os.close(libfuse_fd)
        if os.path.ismount(mountpoint):
            raise RuntimeError(
                f"immutable FUSE mount did not detach: {mountpoint}")
        try:
            mountpoint.rmdir()
        except OSError:
            pass


def run_sealed_snapshot_compile(
        command: list[str],
        input_bindings: dict[Path, tuple[Path, str]],
        snapshot_root: Path, workdir: Path,
        output_paths: list[Path],
        tools: dict[str, Path]) -> list[Path]:
    require_proot(tools)
    mountpoint = snapshot_root.parent / "immutable-inputs"
    immutable_files = {}
    bindings = []
    for index, (virtual_path, (source, expected_hash)) in enumerate(sorted(
            (
                (virtual.absolute(), (source.resolve(), digest))
                for virtual, (source, digest) in input_bindings.items()
            ),
            key=lambda item: str(item[0]))):
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected_hash or \
                sha256(source) != expected_hash:
            raise ValueError(
                f"input changed while creating immutable snapshot: {source}")
        name = f"{index:06d}"
        mode = 0o555 if source.stat().st_mode & 0o111 else 0o444
        immutable_files[name] = (data, mode)
        destination = snapshot_path(snapshot_root, virtual_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.touch()
        bindings.extend(["-b", f"{mountpoint / name}:{virtual_path}"])
    for output in output_paths:
        snapshot_path(snapshot_root, output).parent.mkdir(
            parents=True, exist_ok=True)
    snapshot_path(snapshot_root, workdir).mkdir(
        parents=True, exist_ok=True)
    (snapshot_root / "tmp").mkdir(parents=True, exist_ok=True)
    (snapshot_root / "dev").mkdir(parents=True, exist_ok=True)
    add_snapshot_symlinks(
        snapshot_root, (source for source, _digest in input_bindings.values()))
    for device in ("/dev/null", "/dev/zero", "/dev/urandom"):
        snapshot_path(snapshot_root, Path(device)).touch()
        bindings.extend(["-b", f"{device}:{device}"])
    with immutable_fuse_files(immutable_files, mountpoint, tools):
        with sealed_proot_command(tools) as (proot_command, proot_fds):
            subprocess.run(
                [
                    *proot_command,
                    "-r", str(snapshot_root), "-w", str(workdir),
                    *bindings, *command,
                ],
                cwd=PROJECT_ROOT, env=execution_environment(),
                pass_fds=proot_fds, check=True)
        return [snapshot_path(snapshot_root, path) for path in output_paths]


def parse_depfile_text(
        text: str, root: Path = PROJECT_ROOT) -> tuple[str, Path, list[Path]]:
    flattened = text.replace("\\\n", " ")
    if ":" not in flattened:
        raise ValueError("compiler depfile has no target")
    target_text, dependency_text = flattened.split(":", 1)
    targets = shlex.split(target_text)
    if len(targets) != 1:
        raise ValueError("compiler depfile must have exactly one target")
    target_text = targets[0]
    target = Path(target_text)
    if not target.is_absolute():
        target = root / target
    dependencies = []
    for token in shlex.split(dependency_text):
        dependency = Path(token)
        if not dependency.is_absolute():
            dependency = root / dependency
        dependency = dependency.resolve()
        if not dependency.is_file():
            raise ValueError(f"dependency is missing: {dependency}")
        dependencies.append(dependency)
    return target_text, target.resolve(), sorted(set(dependencies))


def parse_depfile(
        path: Path, root: Path = PROJECT_ROOT
        ) -> tuple[str, Path, list[Path]]:
    return parse_depfile_text(path.read_text(), root)


def dependency_key(path: Path, root: Path = PROJECT_ROOT) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def make_escape(path: Path) -> str:
    return str(path).replace("\\", "\\\\").replace(" ", "\\ ")


def write_complete_depfile(
        path: Path, make_target: str,
        dependencies: Iterable[Path]) -> None:
    rows = [f"{make_target}: \\"]
    ordered = sorted(set(item.absolute() for item in dependencies))
    for index, dependency in enumerate(ordered):
        suffix = " \\" if index + 1 < len(ordered) else ""
        rows.append(f"  {make_escape(dependency)}{suffix}")
    path.write_text("\n".join(rows) + "\n")


def snapshot(paths: Iterable[Path]) -> dict[str, str]:
    return {
        dependency_key(path): sha256(path)
        for path in sorted(set(item.resolve() for item in paths))
    }


def compile_command(
        driver: Path, flags: str, includes: str, depfile: Path,
        dep_target: Path, source: Path, link_inputs: Iterable[Path],
        binary: Path) -> list[str]:
    return [
        str(driver),
        *shlex.split(flags),
        *shlex.split(includes),
        "-MD", "-MF", str(depfile), "-MT", str(dep_target),
        str(source),
        *(str(path) for path in link_inputs),
        "-o", str(binary),
    ]


def dependency_scan_command(
        driver: Path, flags: str, includes: str, depfile: Path,
        dep_target: Path, source: Path) -> list[str]:
    return [
        str(driver),
        *shlex.split(flags),
        *shlex.split(includes),
        "-M", "-MF", str(depfile), "-MT", str(dep_target),
        str(source),
    ]


def normalize_output_paths(command: list[str]) -> list[str]:
    normalized = list(command)
    for option, marker in (("-MF", "<DEPFILE>"), ("-o", "<BINARY>")):
        if option in normalized:
            normalized[normalized.index(option) + 1] = marker
    return normalized


def require_riscv_elf(path: Path) -> None:
    header = path.read_bytes()[:20]
    if len(header) < 20 or header[:4] != b"\x7fELF":
        raise ValueError("guest compiler output is not ELF")
    byteorder = "little" if header[5] == 1 else "big"
    if int.from_bytes(header[18:20], byteorder) != 243:
        raise ValueError("guest compiler output is not RISC-V ELF")


def build_guest(
        receipt_path: Path, binary: Path, depfile: Path, compiler: str,
        flags: str, includes: str, source: Path,
        link_inputs: list[Path], build_config: Path,
        make_target: str) -> dict:
    binary = binary.resolve()
    depfile = depfile.resolve()
    receipt_path = receipt_path.resolve()
    source = source.resolve()
    link_inputs = [path.resolve() for path in link_inputs]
    build_config = build_config.resolve()
    expected_receipt = Path(str(binary) + ".build.json")
    expected_depfile = Path(str(binary) + ".d")
    if receipt_path != expected_receipt or depfile != expected_depfile:
        raise ValueError("receipt and depfile must be adjacent to the guest")
    if make_target != dependency_key(binary):
        raise ValueError("Make target does not match requested guest binary")
    for path in (source, build_config, *link_inputs):
        if not path.is_file():
            raise ValueError(f"build input is missing: {path}")

    driver = resolve_compiler(compiler)
    build_config_values = validate_build_config(
        build_config, compiler, flags, includes)
    tools = tool_paths(build_config_values)
    compiler_before = compiler_receipt(compiler)
    fixed_dependencies = {
        require_trace_tool(tools),
        require_proot(tools),
        *tools.values(),
        build_config,
        *link_inputs,
    }
    fixed_dependencies = {
        path.resolve() for path in fixed_dependencies
    }
    binary.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix=f".{binary.name}.build.", dir=binary.parent) as temp_text:
        temp = Path(temp_text)
        pre_depfile = temp / "pre.d"
        discovery_depfile = temp / "discovery.d"
        discovery_binary = temp / "discovery.bin"
        discovery_trace = temp / "discovery.strace"
        temp_depfile = temp / "final.d"
        temp_binary = temp / binary.name
        snapshot_root = temp / "root"
        scan_command = dependency_scan_command(
            driver, flags, includes, pre_depfile,
            Path(make_target), source)
        subprocess.run(
            scan_command, cwd=PROJECT_ROOT,
            env=execution_environment(), check=True)
        pre_target_text, pre_target, pre_dependencies = parse_depfile(
            pre_depfile)
        if pre_target_text != make_target or pre_target != binary or \
                source not in pre_dependencies:
            raise ValueError("dependency scan does not describe requested guest")
        discovery_command = compile_command(
            driver, flags, includes, discovery_depfile, Path(make_target),
            source, link_inputs, discovery_binary)
        run_traced_command(
            discovery_command, discovery_trace, tools["STRACE"])
        discovery_target_text, discovery_target, discovery_dependencies = (
            parse_depfile(discovery_depfile))
        if discovery_target_text != make_target or \
                discovery_target != binary or \
                source not in discovery_dependencies:
            raise ValueError(
                "discovery depfile does not describe requested guest")
        if set(pre_dependencies) != set(discovery_dependencies):
            raise ValueError(
                "pre-scan dependency set differs from discovery build")
        discovery_inputs = expand_interpreter_inputs(
            traced_input_paths(discovery_trace, temp))
        before_paths = (
            set(discovery_dependencies) | set(discovery_inputs.values()) |
            fixed_dependencies)
        before_hashes = snapshot(before_paths)
        sealed_inputs = {
            virtual: (
                source,
                before_hashes[dependency_key(source)])
            for virtual, source in discovery_inputs.items()
        }
        for path in set(discovery_dependencies) | fixed_dependencies:
            sealed_inputs.setdefault(
                path, (path, before_hashes[dependency_key(path)]))

        command = compile_command(
            driver, flags, includes, temp_depfile, Path(make_target),
            source, link_inputs, temp_binary)
        built_binary, built_depfile = run_sealed_snapshot_compile(
            command, sealed_inputs, snapshot_root, PROJECT_ROOT,
            [temp_binary, temp_depfile], tools)
        post_target_text, post_target, post_dependencies = parse_depfile(
            built_depfile)
        if post_target_text != make_target or post_target != binary or \
                source not in post_dependencies:
            raise ValueError("compile depfile does not describe requested guest")
        after_hashes = snapshot(before_paths)
        if set(discovery_dependencies) != set(post_dependencies):
            raise ValueError("dependency set changed during guest compilation")
        if before_hashes != after_hashes:
            raise ValueError("build input changed during guest compilation")
        if compiler_before != compiler_receipt(compiler):
            raise ValueError("compiler changed during guest compilation")
        if not built_binary.is_file():
            raise ValueError("compiler produced no guest binary")
        require_riscv_elf(built_binary)
        write_complete_depfile(
            built_depfile, make_target,
            before_paths | set(discovery_inputs.keys()))

        canonical_command = compile_command(
            driver, flags, includes, depfile, Path(make_target),
            source, link_inputs, binary)
        payload = {
            "schema_version": 3,
            "compiler": compiler_before,
            "compiler_environment": material_environment(),
            "build_config_values": build_config_values,
            "flags": flags,
            "includes": includes,
            "make_target": make_target,
            "source": dependency_key(source),
            "link_inputs": [
                dependency_key(path) for path in link_inputs
            ],
            "build_config": dependency_key(build_config),
            "dependency_scan_command": scan_command,
            "discovery_compile_command": discovery_command,
            "compile_command": command,
            "canonical_command": canonical_command,
            "trace_tool": file_receipt(tools["STRACE"]),
            "snapshot_runner": {
                "path": str(tools["PROOT"]),
                "sha256": sha256(tools["PROOT"]),
                "loader_sha256": sha256(tools["PROOT_LOADER"]),
                "libc_sha256": sha256(tools["PROOT_LIBC"]),
                "talloc_sha256": sha256(tools["PROOT_TALLOC"]),
                "immutable_fuse_inputs": True,
                "fusepy_sha256": sha256(tools["FUSEPY"]),
                "libfuse_sha256": sha256(tools["LIBFUSE"]),
                "fusermount_sha256": sha256(tools["FUSERMOUNT"]),
            },
            "builder_runtime": {
                "python": str(tools["PYTHON"]),
                "python_sha256": sha256(tools["PYTHON"]),
                "isolated": True,
            },
            "traced_inputs": sorted(
                (
                    {
                        "virtual_path": str(virtual),
                        "source_path": dependency_key(source),
                    }
                    for virtual, source in discovery_inputs.items()
                ),
                key=lambda row: row["virtual_path"]),
            "binary": {
                "path": dependency_key(binary),
                "sha256": sha256(built_binary),
            },
            "depfile": {
                "path": dependency_key(depfile),
                "sha256": sha256(built_depfile),
                "target": post_target_text,
            },
            "dependencies": after_hashes,
        }
        temp_receipt = temp / receipt_path.name
        temp_receipt.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n")
        os.replace(built_binary, binary)
        os.replace(built_depfile, depfile)
        os.replace(temp_receipt, receipt_path)
    return payload


def validate_receipt(
        receipt_path: Path, binary: Path, source: Path,
        link_inputs: list[Path], build_config: Path,
        root: Path = PROJECT_ROOT,
        payload: dict | None = None) -> list[str]:
    errors = []
    binary = binary.resolve()
    source = source.resolve()
    link_inputs = [path.resolve() for path in link_inputs]
    build_config = build_config.resolve()
    expected_receipt = Path(str(binary) + ".build.json")
    expected_depfile = Path(str(binary) + ".d")
    make_target = dependency_key(binary, root)
    if receipt_path.resolve() != expected_receipt:
        errors.append("guest receipt path does not match requested binary")
    if payload is None and not receipt_path.is_file():
        return errors + [f"guest build receipt is missing: {receipt_path}"]
    if payload is None:
        try:
            payload = json.loads(receipt_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            return errors + [f"guest build receipt is unreadable: {error}"]
    if payload.get("schema_version") not in (2, 3):
        errors.append("unsupported guest build receipt schema")
    target_rows = {
        "binary": dependency_key(binary, root),
        "source": dependency_key(source, root),
        "build_config": dependency_key(build_config, root),
    }
    binary_row = payload.get("binary", {})
    if binary_row.get("path") != target_rows["binary"]:
        errors.append("guest receipt names a different binary target")
    if payload.get("source") != target_rows["source"]:
        errors.append("guest receipt names a different kernel source")
    if payload.get("build_config") != target_rows["build_config"]:
        errors.append("guest receipt names a different build configuration")
    if payload.get("make_target") != make_target:
        errors.append("guest receipt names a different Make target")
    expected_links = [dependency_key(path, root) for path in link_inputs]
    if payload.get("link_inputs") != expected_links:
        errors.append("guest receipt names different link inputs")
    if not binary.is_file() or binary_row.get("sha256") != sha256(binary):
        errors.append("guest binary hash does not match build receipt")
    else:
        try:
            require_riscv_elf(binary)
        except ValueError as error:
            errors.append(str(error))
    depfile_row = payload.get("depfile", {})
    if depfile_row.get("path") != dependency_key(expected_depfile, root):
        errors.append("guest receipt names a different depfile")
    if not expected_depfile.is_file() or \
            depfile_row.get("sha256") != sha256(expected_depfile):
        errors.append("guest depfile hash does not match build receipt")
        dep_dependencies = []
    else:
        try:
            dep_target_text, dep_target, dep_dependencies = parse_depfile(
                expected_depfile, root)
            if dep_target_text != make_target or dep_target != binary or \
                    depfile_row.get("target") != make_target:
                errors.append("guest depfile target does not match binary")
        except ValueError as error:
            errors.append(str(error))
            dep_dependencies = []
    try:
        build_values = parse_build_config(build_config)
        compiler_text = build_values["RISCV_CXX"]
        flags = build_values["CXXFLAGS_GEM5_RISCV"]
        includes = build_values["INCLUDES"]
        current_build_values = validate_build_config(
            build_config, compiler_text, flags, includes)
        recorded_build_values = normalize_build_config_values(
            payload.get("build_config_values", {}))
        if recorded_build_values != current_build_values:
            errors.append("guest receipt build configuration is inconsistent")
        if payload.get("compiler_environment") != material_environment():
            errors.append("guest compiler environment changed")
        tools = tool_paths(current_build_values)
    except (OSError, ValueError) as error:
        errors.append(f"guest build configuration cannot be verified: {error}")
        compiler_text, flags, includes = "", "", ""
        tools = {}
    compiler = payload.get("compiler", {})
    try:
        current_compiler = compiler_receipt(compiler_text)
        if compiler != current_compiler:
            errors.append("guest compiler does not match build receipt")
    except (ValueError, subprocess.CalledProcessError) as error:
        errors.append(f"guest compiler cannot be verified: {error}")
        current_compiler = {}
    fixed_dependencies = {build_config, *link_inputs, *tools.values()}
    fixed_dependencies = {
        path.resolve() for path in fixed_dependencies
    }
    trace_tool = payload.get("trace_tool", {})
    if tools and trace_tool != file_receipt(tools["STRACE"]):
        errors.append("guest trace tool does not match build receipt")
    snapshot_runner = payload.get("snapshot_runner", {})
    if tools and snapshot_runner != {
            "path": str(tools["PROOT"]),
            "sha256": sha256(tools["PROOT"]),
            "loader_sha256": sha256(tools["PROOT_LOADER"]),
            "libc_sha256": sha256(tools["PROOT_LIBC"]),
            "talloc_sha256": sha256(tools["PROOT_TALLOC"]),
            "immutable_fuse_inputs": True,
            "fusepy_sha256": sha256(tools["FUSEPY"]),
            "libfuse_sha256": sha256(tools["LIBFUSE"]),
            "fusermount_sha256": sha256(tools["FUSERMOUNT"])}:
        errors.append("guest snapshot runner does not match build receipt")
    if tools and payload.get("builder_runtime") != {
            "python": str(tools["PYTHON"]),
            "python_sha256": sha256(tools["PYTHON"]),
            "isolated": True}:
        errors.append("guest builder Python runtime does not match receipt")
    traced_rows = payload.get("traced_inputs")
    traced_inputs = set()
    if not isinstance(traced_rows, list) or not traced_rows:
        errors.append("guest receipt has no traced compiler/linker inputs")
    else:
        for row in traced_rows:
            if not isinstance(row, dict):
                errors.append("invalid traced compiler input row")
                continue
            virtual_path = Path(str(row.get("virtual_path", "")))
            if not virtual_path.is_absolute():
                errors.append("traced compiler virtual path is not absolute")
            path = Path(str(row.get("source_path", "")))
            if not path.is_absolute():
                path = root / path
            path = path.resolve()
            if not path.is_file():
                errors.append(
                    f"traced compiler input is missing: "
                    f"{row.get('source_path')}")
            else:
                traced_inputs.add(path)
                if not virtual_path.is_file() or \
                        virtual_path.resolve() != path:
                    errors.append(
                        "traced compiler virtual alias changed: "
                        f"{virtual_path}")
    dependency_paths = set(dep_dependencies) | traced_inputs | fixed_dependencies
    recorded_dependencies = dict(payload.get("dependencies", {}))
    if payload.get("schema_version") == 2:
        dependency_paths = {
            path for path in dependency_paths
            if dependency_key(path, root) not in
            LEGACY_ORCHESTRATION_DEPENDENCIES
        }
        for name in LEGACY_ORCHESTRATION_DEPENDENCIES:
            recorded_dependencies.pop(name, None)
    expected_dependencies = snapshot(dependency_paths)
    if recorded_dependencies != expected_dependencies:
        errors.append("guest dependency hashes do not match build receipt")
    if source not in dep_dependencies:
        errors.append("guest depfile does not include requested kernel source")
    if current_compiler:
        expected_command = compile_command(
            Path(current_compiler["driver"]),
            flags, includes, expected_depfile, Path(make_target),
            source, link_inputs, binary)
        if payload.get("flags") != flags or \
                payload.get("includes") != includes:
            errors.append("guest compiler options do not match build config")
        if payload.get("canonical_command") != expected_command:
            errors.append("guest canonical compiler command is inconsistent")
        compile_row = payload.get("compile_command")
        if not isinstance(compile_row, list) or \
                normalize_output_paths(compile_row) != \
                normalize_output_paths(expected_command):
            errors.append("guest executed compiler command is inconsistent")
    return errors


def stage_validated_guest(
        receipt_path: Path, binary: Path, source: Path,
        link_inputs: list[Path], build_config: Path,
        staging_dir: Path, root: Path = PROJECT_ROOT) -> tuple[Path, str]:
    try:
        receipt_bytes = receipt_path.read_bytes()
        payload = json.loads(receipt_bytes)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"guest build receipt is unreadable: {error}") from error
    errors = validate_receipt(
        receipt_path, binary, source, link_inputs, build_config, root,
        payload)
    if errors:
        raise ValueError("\n".join(errors))
    expected_hash = str(payload["binary"]["sha256"])
    if sha256(binary) != expected_hash:
        raise ValueError("guest changed after receipt validation")
    staging_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(staging_dir, 0o700)
    destination = staging_dir / f"{binary.name}.{expected_hash}"
    if not destination.is_file():
        temporary = staging_dir / f".{destination.name}.tmp"
        temporary.unlink(missing_ok=True)
        with binary.open("rb") as source_handle, temporary.open("xb") as out:
            shutil.copyfileobj(source_handle, out)
            out.flush()
            os.fsync(out.fileno())
        if sha256(binary) != expected_hash or \
                sha256(temporary) != expected_hash:
            temporary.unlink(missing_ok=True)
            raise ValueError("guest changed while staging validated input")
        os.chmod(temporary, 0o555)
        os.replace(temporary, destination)
    if sha256(destination) != expected_hash:
        raise ValueError("staged guest hash mismatch")
    os.chmod(destination, 0o555)
    os.chmod(staging_dir, 0o555)
    return destination, expected_hash


def verify_staged_guest(path: Path, expected_hash: str) -> None:
    if not path.is_file() or sha256(path) != expected_hash:
        raise ValueError("validated staged guest changed before execution")


def open_sealed_guest(path: Path, expected_hash: str) -> tuple[int, str]:
    fd = os.memfd_create(
        f"graphbrew-{path.name}",
        os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
                view = memoryview(chunk)
                while view:
                    written = os.write(fd, view)
                    view = view[written:]
        if digest.hexdigest() != expected_hash:
            raise ValueError("staged guest changed while opening sealed input")
        os.fchmod(fd, 0o555)
        seals = (
            fcntl.F_SEAL_SEAL |
            fcntl.F_SEAL_SHRINK |
            fcntl.F_SEAL_GROW |
            fcntl.F_SEAL_WRITE)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, seals)
        os.lseek(fd, 0, os.SEEK_SET)
        return fd, f"/proc/self/fd/{fd}"
    except Exception:
        os.close(fd)
        raise


def remove_outputs(binary: Path, depfile: Path, receipt: Path) -> None:
    for path in (binary, depfile, receipt):
        path.unlink(missing_ok=True)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="action", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("--receipt", type=Path, required=True)
    build.add_argument("--binary", type=Path, required=True)
    build.add_argument("--depfile", type=Path, required=True)
    build.add_argument("--compiler", required=True)
    build.add_argument("--flags", default="")
    build.add_argument("--includes", default="")
    build.add_argument("--source", type=Path, required=True)
    build.add_argument(
        "--link-input", type=Path, action="append", default=[])
    build.add_argument("--build-config", type=Path, required=True)
    build.add_argument("--make-target", required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("--receipt", type=Path, required=True)
    verify.add_argument("--binary", type=Path, required=True)
    verify.add_argument("--source", type=Path, required=True)
    verify.add_argument(
        "--link-input", type=Path, action="append", default=[])
    verify.add_argument("--build-config", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.action == "build":
        try:
            build_values = parse_build_config(args.build_config)
            require_isolated_python(Path(build_values["PYTHON"]))
            build_guest(
                args.receipt, args.binary, args.depfile, args.compiler,
                args.flags, args.includes, args.source, args.link_input,
                args.build_config, args.make_target)
        except Exception:
            remove_outputs(args.binary, args.depfile, args.receipt)
            raise
        return 0
    errors = validate_receipt(
        args.receipt, args.binary, args.source,
        args.link_input, args.build_config)
    for error in errors:
        print(f"[FAIL] {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
