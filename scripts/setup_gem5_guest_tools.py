#!/usr/bin/env python3
"""Install pinned unprivileged tools for provenance-bound gem5 guest builds."""

from __future__ import annotations

import argparse
import hashlib
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
        "version": "5.1.0-1.3",
        "deb_sha256":
            "01a5d27c4ac16e184bdb356c9e69fa7d494325ac653c4cd64fae4c3fc63cdbbb",
        "source": "usr/bin/proot",
        "target": "proot",
        "target_sha256":
            "9f6fc9a29f9338aee2df61d16f84fb498d0c1c541c4e52bd648843108790853b",
    },
    "python3-fusepy": {
        "version": "3.0.1-5",
        "deb_sha256":
            "c35fc19d8d2fff7ce4c50fbf7f22dc407b20a0a997f0539c0f40511470076552",
        "source": "usr/lib/python3/dist-packages/fusepy.py",
        "target": "fusepy.py",
        "target_sha256":
            "7a7b60998bd459d5bbe6fcd8d4886fa4d3784d58bd8209db4f83ebee7299af87",
    },
    "libfuse2t64": {
        "version": "2.9.9-8.1build1",
        "deb_sha256":
            "42919b326576c6c5cde85f4748092351c13b1b365c8793adb926b9c11cd5b22d",
        "source": "lib/x86_64-linux-gnu/libfuse.so.2.9.9",
        "target": "libfuse.so.2",
        "target_sha256":
            "654ae57bdd98c3c85e7a592e4f73cc59dc19a12d545bce77a57b0c4e7af8f394",
    },
}
FUSERMOUNT = Path("/usr/bin/fusermount3")
FUSERMOUNT_SHA256 = (
    "d278775c1528dd32efc85c2cb322423ee93aa8dcf76aaa595f7022d427910704")
HOST_FILES = {
    Path("/usr/bin/fusermount3"):
        FUSERMOUNT_SHA256,
    Path("/usr/bin/strace"):
        "28f957c227012de0b18d1bd7fff2d396cb693ea60ed8013be68de071e84b5001",
    Path("/usr/bin/python3.12"):
        "1643dacd9feaedc58f3cc581e4d22577dfe25c09b10282936186ccf0f2e61118",
    Path("/usr/bin/riscv64-linux-gnu-g++-13"):
        "a675774e2afe01433771f6745de50870300833dc60ed5854662b414eff5fb7b6",
    Path("/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"):
        "cd4df4f3c7b83673d61189bf2eaebd33ca4f2853ab9772b8a25e025ef99b1e81",
    Path("/usr/lib/x86_64-linux-gnu/libc.so.6"):
        "8db37cf3f2169f59a0f07ef1fea308c35656668c64c8ff294e1860f4121eb161",
    Path("/usr/lib/x86_64-linux-gnu/libtalloc.so.2"):
        "5e4fb8691231a2431f5126f79c884bdc0678ef08b2c3d5f9c5017365589dbf4b",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(out_dir: Path) -> list[str]:
    errors = []
    for package, values in PACKAGES.items():
        path = out_dir / str(values["target"])
        if not path.is_file():
            errors.append(f"{package} target is missing: {path}")
        elif sha256(path) != values["target_sha256"]:
            errors.append(f"{package} target hash mismatch: {path}")
    for path, digest in HOST_FILES.items():
        if not path.is_file() or sha256(path) != digest:
            errors.append(f"pinned host runtime is missing or changed: {path}")
    return errors


def install(out_dir: Path) -> None:
    if platform.machine() != "x86_64":
        raise SystemExit("pinned guest build tools require x86_64")
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix="graphbrew-gem5-tools-") as temp_text:
        temp = Path(temp_text)
        for package, values in PACKAGES.items():
            spec = f"{package}={values['version']}"
            subprocess.run(
                ["apt-get", "download", spec],
                cwd=temp, check=True, stdout=subprocess.DEVNULL)
            archives = list(temp.glob(f"{package}_*.deb"))
            if len(archives) != 1:
                raise SystemExit(f"expected one downloaded archive for {spec}")
            archive = archives[0]
            if sha256(archive) != values["deb_sha256"]:
                raise SystemExit(f"package hash mismatch for {spec}")
            extracted = temp / f"extract-{package}"
            subprocess.run(
                ["dpkg-deb", "-x", str(archive), str(extracted)],
                check=True)
            source = extracted / str(values["source"])
            target = out_dir / str(values["target"])
            if target.is_file() and sha256(target) == values["target_sha256"]:
                archive.unlink()
                continue
            temporary = target.with_suffix(target.suffix + ".tmp")
            shutil.copyfile(source, temporary)
            if sha256(temporary) != values["target_sha256"]:
                temporary.unlink(missing_ok=True)
                raise SystemExit(f"installed hash mismatch for {package}")
            temporary.chmod(
                0o755 if package in ("proot", "libfuse2t64") else 0o644)
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
