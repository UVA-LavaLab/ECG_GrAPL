#!/usr/bin/env python3
"""Run one Sniper probe with wall-clock and process-tree RSS bounds."""

import argparse
import ctypes
from dataclasses import dataclass
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    start_time: int


def become_child_subreaper() -> None:
    pr_set_child_subreaper = 36
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(pr_set_child_subreaper, 1, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def process_stats(pid: int) -> tuple[int, int, int, int] | None:
    try:
        status = Path(f"/proc/{pid}/status").read_text()
        stat = Path(f"/proc/{pid}/stat").read_text()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None
    values = {}
    for line in status.splitlines():
        if line.startswith(("VmRSS:", "VmSize:")):
            key, value, _unit = line.split()
            values[key[:-1]] = int(value) * 1024
    fields = stat[stat.rfind(")") + 2:].split()
    return (
        int(fields[1]), values.get("VmRSS", 0),
        values.get("VmSize", 0), int(fields[19]))


def discover_process_tree(root: ProcessIdentity) -> dict[int, ProcessIdentity]:
    parents: dict[int, list[ProcessIdentity]] = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        stats = process_stats(int(entry.name))
        if stats is not None:
            identity = ProcessIdentity(int(entry.name), stats[3])
            parents.setdefault(stats[0], []).append(identity)
    found = {root.pid: root}
    pending = [root.pid]
    while pending:
        parent = pending.pop()
        children = parents.get(parent, [])
        for child in children:
            if child.pid not in found:
                found[child.pid] = child
                pending.append(child.pid)
    return found


def direct_children(pid: int) -> list[int]:
    path = Path(f"/proc/{pid}/task/{pid}/children")
    try:
        text = path.read_text().strip()
    except FileNotFoundError:
        return []
    return [int(value) for value in text.split()] if text else []


def capture_reparented(
        captured: dict[int, ProcessIdentity],
        watchdog_pid: int) -> None:
    for pid in direct_children(watchdog_pid):
        stats = process_stats(pid)
        if stats is not None:
            captured.setdefault(pid, ProcessIdentity(pid, stats[3]))


def identity_alive(identity: ProcessIdentity) -> bool:
    stats = process_stats(identity.pid)
    return stats is not None and stats[3] == identity.start_time


def terminate_captured(
        captured: dict[int, ProcessIdentity],
        root: ProcessIdentity) -> None:
    children = [
        identity for pid, identity in captured.items()
        if pid != root.pid
    ]
    for identity in reversed(children):
        if not identity_alive(identity):
            continue
        try:
            os.kill(identity.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    while True:
        try:
            waited, _status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            break
        if waited == 0:
            break
    if identity_alive(root):
        try:
            os.kill(root.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    time.sleep(1)
    for identity in reversed(children + [root]):
        if not identity_alive(identity):
            continue
        try:
            os.kill(identity.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rss-mib", type=int, required=True)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--sample-ms", type=int, default=100)
    parser.add_argument("--orphan-grace-ms", type=int, default=500)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("missing command")
    if args.rss_mib <= 0 or args.seconds <= 0 or args.sample_ms <= 0 or args.orphan_grace_ms < 0:
        parser.error("RSS, time and sampling limits must be positive; orphan grace must be nonnegative")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    become_child_subreaper()
    start = time.monotonic()
    peak_rss = 0
    peak_vms = 0
    reason = "exit"
    with args.log.open("w") as output:
        process = subprocess.Popen(
            command, stdout=output, stderr=subprocess.STDOUT, text=True)
        root_stats = process_stats(process.pid)
        if root_stats is None:
            raise RuntimeError("watchdog child disappeared before observation")
        root = ProcessIdentity(process.pid, root_stats[3])
        captured = {root.pid: root}
        while process.poll() is None:
            captured.update(discover_process_tree(root))
            capture_reparented(captured, os.getpid())
            rss = 0
            vms = 0
            for identity in captured.values():
                stats = process_stats(identity.pid)
                if stats is None or stats[3] != identity.start_time:
                    continue
                rss += stats[1]
                vms += stats[2]
            peak_rss = max(peak_rss, rss)
            peak_vms = max(peak_vms, vms)
            if rss > args.rss_mib * 1024 * 1024:
                reason = "rss-limit"
                terminate_captured(captured, root)
                break
            if time.monotonic() - start > args.seconds:
                reason = "timeout"
                terminate_captured(captured, root)
                break
            time.sleep(args.sample_ms / 1000)

        if reason == "exit":
            grace_end = time.monotonic() + args.orphan_grace_ms / 1000
            while time.monotonic() < grace_end:
                capture_reparented(captured, os.getpid())
                alive_children = [
                    identity for pid, identity in captured.items()
                    if pid != root.pid and identity_alive(identity)
                ]
                if not alive_children:
                    break
                time.sleep(args.sample_ms / 1000)
            capture_reparented(captured, os.getpid())
            if any(
                    identity_alive(identity)
                    for pid, identity in captured.items()
                    if pid != root.pid):
                reason = "orphan"
                terminate_captured(captured, root)

        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            terminate_captured(captured, root)
            process.wait(timeout=3)
    elapsed = time.monotonic() - start
    with args.log.open("a") as output:
        output.write(
            f"\n[watchdog_reason] {reason}\n"
            + f"[watchdog_returncode] {process.returncode}\n"
            + f"[watchdog_elapsed_s] {elapsed:.3f}\n"
            + f"[watchdog_peak_rss_mib] {peak_rss / 1024 / 1024:.3f}\n"
            + f"[watchdog_peak_vms_mib] {peak_vms / 1024 / 1024:.3f}\n")
    print(
        f"reason={reason} rc={process.returncode} elapsed={elapsed:.3f}s "
        f"peak_rss={peak_rss / 1024 / 1024:.1f}MiB "
        f"peak_vms={peak_vms / 1024 / 1024:.1f}MiB")
    return 0 if reason == "exit" and process.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
