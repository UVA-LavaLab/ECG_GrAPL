#!/usr/bin/env python3
"""Report lab-node prerequisites for current ECG whole-profile launches."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from typing import Any, Iterator


DEFAULT_ROOT = Path(__file__).resolve().parents[4]
CURRENT_PROFILES = (
    "ecg_current_equivalence",
    "ecg_local_release_cache",
    "ecg_large_cache",
    "ecg_detailed_final",
)


def result(
        name: str, status: str, detail: str, required: bool = True
) -> dict[str, Any]:
    return {
        "name": name,
        "status": status,
        "required": required,
        "detail": detail,
    }


def check_command(name: str) -> dict[str, Any]:
    path = shutil.which(name)
    if path:
        return result(f"command:{name}", "pass", path)
    return result(f"command:{name}", "fail", "not found on PATH")


def check_configured_command(label: str, command: str) -> dict[str, Any]:
    if not command or any(character.isspace() for character in command):
        return result(
            f"command:{label}",
            "fail",
            f"{label} must name one executable, not a shell command",
        )
    path = shutil.which(command)
    if path:
        return result(f"command:{label}", "pass", path)
    return result(
        f"command:{label}", "fail", f"{command} not found on PATH"
    )


def check_path(
        name: str, path: Path, *, executable: bool = False
) -> dict[str, Any]:
    if not path.is_file():
        return result(name, "fail", f"missing file: {path}")
    if executable and not os.access(path, os.X_OK):
        return result(name, "fail", f"not executable: {path}")
    return result(name, "pass", str(path))


def check_device(name: str, path: Path) -> dict[str, Any]:
    if not path.exists():
        return result(name, "fail", f"missing device: {path}")
    if not os.access(path, os.R_OK | os.W_OK):
        return result(
            name, "fail", f"device is not readable and writable: {path}"
        )
    return result(name, "pass", str(path))


def check_setarch(architecture: str) -> dict[str, Any]:
    setarch = shutil.which("setarch")
    if not setarch:
        return result(
            "setarch/aslr-control", "fail", "setarch not found on PATH"
        )
    try:
        completed = subprocess.run(
            [setarch, architecture, "-R", "/usr/bin/true"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return result(
            "setarch/aslr-control",
            "fail",
            f"could not verify setarch -R for this job: {error}",
        )
    if completed.returncode:
        detail = (completed.stderr or completed.stdout).strip()
        return result(
            "setarch/aslr-control",
            "fail",
            f"setarch -R is unavailable to this job"
            f"{': ' + detail if detail else ''}",
        )
    return result(
        "setarch/aslr-control",
        "pass",
        f"{setarch} can run {architecture} -R",
    )


def check_fuse_availability(root: Path) -> list[dict[str, Any]]:
    return [
        check_device("fuse:device", Path("/dev/fuse")),
        check_path("fuse:fusermount3", Path("/usr/bin/fusermount3"),
                   executable=True),
        check_path(
            "fuse:fusepy",
            root / "bench/include/gem5_sim/.tools/fusepy.py",
        ),
        check_path(
            "fuse:libfuse",
            root / "bench/include/gem5_sim/.tools/libfuse.so.2",
        ),
    ]


@contextmanager
def _fuse_probe_context(root: Path, mountpoint: Path) -> Iterator[None]:
    ecg_dir = root / "scripts/experiments/ecg"
    sys.path.insert(0, str(ecg_dir))
    try:
        from gem5_guest_receipt import immutable_fuse_files

        with immutable_fuse_files(
                {"probe.txt": (b"graphbrew-fuse-preflight\n", 0o444)},
                mountpoint):
            yield
    finally:
        if sys.path and sys.path[0] == str(ecg_dir):
            sys.path.pop(0)


def probe_fuse_mount(root: Path) -> dict[str, Any]:
    scratch_root = (
        root / "results/ecg_experiments/preflight"
    ).resolve()
    mountpoint = scratch_root / f"fuse_probe_{os.getpid()}"
    if mountpoint.exists():
        return result(
            "fuse:mount-probe", "fail",
            f"bounded probe path already exists: {mountpoint}",
        )
    try:
        with _fuse_probe_context(root, mountpoint):
            payload = (mountpoint / "probe.txt").read_bytes()
            if payload != b"graphbrew-fuse-preflight\n":
                raise RuntimeError("mounted probe content mismatch")
        if mountpoint.exists():
            mountpoint.rmdir()
    except (OSError, RuntimeError, ValueError, ImportError) as error:
        detail = str(error)
        if mountpoint.exists():
            try:
                mountpoint.rmdir()
            except OSError as cleanup_error:
                detail += f"; cleanup failed for {mountpoint}: {cleanup_error}"
        return result("fuse:mount-probe", "fail", detail)
    return result(
        "fuse:mount-probe", "pass",
        f"immutable_fuse_files mounted and unmounted {mountpoint}",
    )


def collect_preflight(
        root: Path,
        profiles: list[str],
        *,
        equivalence_receipt: Path | None,
        probe_fuse: bool,
) -> dict[str, Any]:
    root = root.resolve()
    selected = set(profiles)
    needs_cache = bool(selected & {
        "ecg_current_equivalence", "ecg_local_release_cache",
        "ecg_large_cache",
    })
    needs_gem5 = bool(selected & {
        "ecg_current_equivalence", "ecg_detailed_final",
    })
    needs_sniper = needs_gem5
    architecture = platform.machine()
    checks = [
        result("host architecture", "pass", architecture),
        check_command("python3"),
        check_command("git"),
        check_command("make"),
        check_configured_command(
            "CXX", os.environ.get("CXX", "g++")
        ),
        check_setarch(architecture),
    ]

    if needs_cache:
        checks.append(check_path(
            "cache-sim:pr", root / "bench/bin_sim/pr", executable=True
        ))

    if needs_gem5:
        checks.extend([
            check_configured_command(
                "RISCV_CXX",
                os.environ.get("RISCV_CXX", "riscv64-linux-gnu-g++"),
            ),
            check_command("scons"),
            check_path(
                "gem5:riscv-gem5.opt",
                root / "bench/include/gem5_sim/gem5/build/RISCV/gem5.opt",
                executable=True,
            ),
            check_path(
                "gem5:pr-riscv-guest",
                root / "bench/bin_gem5/pr_riscv_m5ops",
                executable=True,
            ),
            check_path(
                "gem5:graph-se-config",
                root / "bench/include/gem5_sim/configs/graphbrew/graph_se.py",
            ),
            check_path(
                "guest-build:python3.12",
                Path("/usr/bin/python3.12"),
                executable=True,
            ),
            check_path(
                "guest-build:strace",
                Path("/usr/bin/strace"),
                executable=True,
            ),
            check_path(
                "guest-build:proot",
                root / "bench/include/gem5_sim/.tools/proot",
                executable=True,
            ),
            check_path(
                "guest-build:proot-loader",
                Path("/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
            ),
            check_path(
                "guest-build:proot-libc",
                Path("/usr/lib/x86_64-linux-gnu/libc.so.6"),
            ),
            check_path(
                "guest-build:proot-talloc",
                Path("/usr/lib/x86_64-linux-gnu/libtalloc.so.2"),
            ),
        ])
        checks.extend(check_fuse_availability(root))

    if needs_sniper:
        sniper = root / "bench/include/sniper_sim/snipersim"
        if architecture not in {"x86_64", "amd64"}:
            checks.append(result(
                "sniper:host-architecture",
                "fail",
                f"Sniper SDE/SIFT requires an x86_64 host, got {architecture}",
            ))
        else:
            checks.append(result(
                "sniper:host-architecture", "pass", architecture
            ))
        for name, relative, executable in (
            ("sniper:run-sniper", "run-sniper", True),
            ("sniper:record-trace", "record-trace", True),
            ("sniper:binary", "lib/sniper", True),
            ("sniper:sde64", "sde_kit/sde64", True),
            (
                "sniper:sde-sift-recorder",
                "sift/recorder/obj-intel64/sde_sift_recorder.so",
                False,
            ),
        ):
            checks.append(check_path(
                name, sniper / relative, executable=executable
            ))
        checks.append(check_path(
            "sniper:sg-kernel",
            root / "bench/bin_sniper/sg_kernel",
            executable=True,
        ))

    if "ecg_detailed_final" in selected:
        if equivalence_receipt is None:
            checks.append(result(
                "final:equivalence-receipt",
                "fail",
                "--equivalence-receipt is required for ecg_detailed_final",
            ))
        else:
            receipt_path = equivalence_receipt
            if not receipt_path.is_absolute():
                receipt_path = root / receipt_path
            checks.append(check_path(
                "final:equivalence-receipt",
                receipt_path.resolve(),
            ))

    if probe_fuse:
        checks.append(probe_fuse_mount(root))

    return {
        "ok": all(check["status"] == "pass" for check in checks),
        "project_root": str(root),
        "profiles": profiles,
        "checks": checks,
        "notes": [
            (
                "Existing executable/ELF presence is only an availability "
                "check and does not establish -march=native portability."
            ),
            (
                "A node-local rebuild is still required before release "
                "qualification."
            ),
            (
                "A full small ecg_current_equivalence run must be completed "
                "on the rebuilt lab node; its path/binary/source-bound receipt "
                "must be regenerated there before ecg_detailed_final."
            ),
            (
                "This preflight checks launch prerequisites only and does not "
                "guarantee lab performance."
            ),
        ],
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project-root", type=Path, default=DEFAULT_ROOT
    )
    parser.add_argument(
        "--profile",
        action="append",
        choices=CURRENT_PROFILES,
        help=(
            "Current profile to check; repeat as needed "
            "(default: ecg_current_equivalence)."
        ),
    )
    parser.add_argument(
        "--equivalence-receipt",
        type=Path,
        help=(
            "Receipt path to check for ecg_detailed_final. The experiment "
            "runner remains the authoritative validator."
        ),
    )
    parser.add_argument(
        "--probe-fuse",
        action="store_true",
        help=(
            "Explicitly perform a tiny immutable_fuse_files mount under results/ecg_experiments/preflight. "
            "The whole-profile launcher supplies the enclosing RSS/time watchdog."
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit the report as JSON.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    profiles = args.profile or ["ecg_current_equivalence"]
    report = collect_preflight(
        args.project_root,
        profiles,
        equivalence_receipt=args.equivalence_receipt,
        probe_fuse=args.probe_fuse,
    )
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"[ecg-lab-preflight] project_root={report['project_root']}")
        print(f"[ecg-lab-preflight] profiles={','.join(report['profiles'])}")
        for check in report["checks"]:
            marker = "PASS" if check["status"] == "pass" else "FAIL"
            print(
                f"[{marker}] {check['name']}: {check['detail']}",
                file=sys.stdout if marker == "PASS" else sys.stderr,
            )
        for note in report["notes"]:
            print(f"[NOTE] {note}")
        print(
            "[ecg-lab-preflight] "
            + ("ready for rebuild/equivalence work" if report["ok"]
               else "missing or unusable prerequisites"),
        )
    return 0 if report["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
