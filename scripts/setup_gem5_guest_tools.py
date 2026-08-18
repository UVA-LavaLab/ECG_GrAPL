#!/usr/bin/env python3
"""Install unprivileged tools for material-input-bound gem5 guest builds."""

from __future__ import annotations

import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "bench/include/gem5_sim/.tools"
PACKAGES = {
    "proot": {
        "source": "usr/bin/proot",
        "target": "proot",
        "mode": 0o755,
        "kind": "elf",
    },
    "python3-fusepy": {
        "source": "usr/lib/python3/dist-packages/fusepy.py",
        "target": "fusepy.py",
        "mode": 0o644,
        "kind": "python",
    },
    "libfuse2t64": {
        "source_glob": "lib/x86_64-linux-gnu/libfuse.so.2*",
        "target": "libfuse.so.2",
        "mode": 0o755,
        "kind": "elf",
    },
}
def validate_tool(path: Path, kind: str) -> str | None:
    if not path.is_file():
        return f"tool is missing: {path}"
    if path.stat().st_size == 0:
        return f"tool is empty: {path}"
    if kind == "elf" and path.read_bytes()[:4] != b"\x7fELF":
        return f"tool is not ELF: {path}"
    if kind == "python":
        try:
            compile(path.read_text(), str(path), "exec")
        except (OSError, SyntaxError) as error:
            return f"tool is not valid Python: {path}: {error}"
    return None


def verify(out_dir: Path) -> list[str]:
    errors = []
    for package, values in PACKAGES.items():
        path = out_dir / str(values["target"])
        error = validate_tool(path, str(values["kind"]))
        if error:
            errors.append(f"{package}: {error}")
    return errors


def install(out_dir: Path) -> None:
    if platform.machine() != "x86_64":
        raise SystemExit("gem5 guest build tools require x86_64")
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix="graphbrew-gem5-tools-") as temp_text:
        temp = Path(temp_text)
        for package, values in PACKAGES.items():
            subprocess.run(
                ["apt-get", "download", package],
                cwd=temp, check=True, stdout=subprocess.DEVNULL)
            archives = list(temp.glob(f"{package}_*.deb"))
            if len(archives) != 1:
                raise SystemExit(
                    f"expected one downloaded archive for {package}")
            archive = archives[0]
            extracted = temp / f"extract-{package}"
            subprocess.run(
                ["dpkg-deb", "-x", str(archive), str(extracted)],
                check=True)
            if "source" in values:
                source = extracted / str(values["source"])
            else:
                candidates = sorted(extracted.glob(str(values["source_glob"])))
                regular = [path for path in candidates if path.is_file()]
                if not regular:
                    raise SystemExit(
                        f"package has no matching tool: {package}")
                source = regular[-1]
            target = out_dir / str(values["target"])
            temporary = target.with_suffix(target.suffix + ".tmp")
            shutil.copyfile(source, temporary)
            error = validate_tool(temporary, str(values["kind"]))
            if error:
                temporary.unlink(missing_ok=True)
                raise SystemExit(error)
            temporary.chmod(int(values["mode"]))
            temporary.replace(target)
            archive.unlink()
    errors = verify(out_dir)
    if errors:
        raise SystemExit("\n".join(errors))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    out_dir = args.out_dir.resolve()
    if not args.verify_only:
        install(out_dir)
    errors = verify(out_dir)
    for error in errors:
        print(f"[FAIL] {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
