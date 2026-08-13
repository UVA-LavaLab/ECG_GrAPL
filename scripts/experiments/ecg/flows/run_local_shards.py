#!/usr/bin/env python3
"""Run manifest shards concurrently on one prebuilt local machine."""

from __future__ import annotations

import argparse
import csv
import fcntl
import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import experiment_run  # noqa: E402


PROJECT_ROOT = Path(__file__).resolve().parents[4]


@dataclass(frozen=True)
class Shard:
    profile: str
    stage: str
    graph: str
    benchmark: str
    policy: str
    run_tag: str
    suite: str

    @property
    def key(self) -> str:
        return "_".join(
            experiment_run.sanitize(value)
            for value in (
                self.profile, self.stage, self.graph,
                self.benchmark, self.policy)
        )


def stage_suites(manifest: dict) -> dict[str, str]:
    return {
        str(stage["name"]): str(stage["suite"])
        for stage in manifest.get("stages", [])
        if stage.get("kind") == "roi_matrix"
    }


def read_shards(path: Path, suites: dict[str, str]) -> list[Shard]:
    rows: list[Shard] = []
    seen: set[tuple[str, ...]] = set()
    with path.open(newline="") as handle:
        for line_number, fields in enumerate(
                csv.reader(handle, delimiter="\t"), start=1):
            if len(fields) != 6 or any(not field for field in fields):
                raise SystemExit(
                    f"invalid shard row {line_number}: expected six "
                    "non-empty tab-separated fields")
            profile, stage, graph, benchmark, policy, run_tag = fields
            if stage not in suites:
                raise SystemExit(
                    f"invalid shard row {line_number}: unknown stage={stage!r}")
            identity = tuple(fields)
            if identity in seen:
                raise SystemExit(
                    f"duplicate shard row {line_number}: {fields!r}")
            seen.add(identity)
            rows.append(Shard(
                profile, stage, graph, benchmark, policy,
                experiment_run.sanitize(run_tag), suites[stage]))
    if not rows:
        raise SystemExit("shard file contains no rows")
    run_keys = [(row.run_tag, row.key) for row in rows]
    if len(run_keys) != len(set(run_keys)):
        raise SystemExit("shard rows collide after path sanitization")
    return rows


def shard_command(
        shard: Shard, run_root: Path, manifest_path: Path,
        args: argparse.Namespace) -> tuple[list[str], Path]:
    run_dir = run_root / shard.run_tag / shard.key
    command = [
        sys.executable,
        str(experiment_run.__file__),
        "--manifest", str(manifest_path),
        "--graph-dir", str(args.graph_dir),
        "--profile", shard.profile,
        "--run-dir", str(run_dir),
        "--only", shard.stage,
        "--graph", shard.graph,
        "--benchmark", shard.benchmark,
        "--no-build",
        "--lock-path", str(run_dir / ".experiment_run.lock"),
    ]
    if shard.policy != "__whole__":
        command.extend(["--policy", shard.policy])
    if args.force:
        command.append("--force")
    if args.allow_missing_graphs:
        command.append("--allow-missing-graphs")
    return command, run_dir


def run_shard(
        shard: Shard, run_root: Path, manifest_path: Path,
        args: argparse.Namespace,
        semaphore: threading.BoundedSemaphore) -> tuple[Shard, int, Path]:
    command, run_dir = shard_command(shard, run_root, manifest_path, args)
    if args.dry_run:
        print("[dry-run] " + experiment_run.command_text(command))
        return shard, 0, run_dir
    run_dir.mkdir(parents=True, exist_ok=True)
    log_path = run_dir / "local_launcher.log"
    launcher_lock = run_dir / ".local_shard.lock"
    env = dict(os.environ)
    env["GRAPHBREW_SHARD_GROUP"] = shard.run_tag
    with semaphore, launcher_lock.open("w") as lock, log_path.open("w") as log:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log.write("another local launcher owns this shard\n")
            return shard, 2, run_dir
        result = subprocess.run(
            command, cwd=PROJECT_ROOT, env=env,
            stdout=log, stderr=subprocess.STDOUT, check=False)
        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
    return shard, result.returncode, run_dir


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run GraphBrew experiment shards concurrently.")
    parser.add_argument("--shards", required=True)
    parser.add_argument(
        "--manifest", default=str(experiment_run.DEFAULT_MANIFEST))
    parser.add_argument(
        "--graph-dir",
        default=str(PROJECT_ROOT / "results" / "graphs"))
    parser.add_argument(
        "--run-root",
        default="results/ecg_experiments/runs/local")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument(
        "--cache-sim-jobs", type=int, default=0,
        help="cache_sim concurrency; 0 inherits --jobs")
    parser.add_argument("--gem5-jobs", type=int, default=1)
    parser.add_argument("--sniper-jobs", type=int, default=1)
    parser.add_argument(
        "--stop-on-error", action="store_true",
        help="Do not launch later shards after the first failure. Enabled "
             "automatically for serial k2_final_campaign runs.")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--allow-missing-graphs", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.jobs < 1:
        raise SystemExit("--jobs must be >= 1")
    if args.stop_on_error and args.jobs != 1:
        raise SystemExit("--stop-on-error requires --jobs 1")
    for name in ("cache_sim_jobs", "gem5_jobs", "sniper_jobs"):
        if getattr(args, name) < 0:
            raise SystemExit(f"--{name.replace('_', '-')} must be >= 0")

    manifest_path = experiment_run.resolve_path(args.manifest)
    args.graph_dir = experiment_run.resolve_path(args.graph_dir)
    manifest = experiment_run.load_manifest(manifest_path)
    shards = read_shards(
        experiment_run.resolve_path(args.shards), stage_suites(manifest))
    run_root = experiment_run.resolve_path(args.run_root)
    sniper_limit = args.sniper_jobs or args.jobs
    if (sniper_limit > 1 and any(
            shard.suite == "sniper" and
            shard.profile == "k2_final_campaign"
            for shard in shards)):
        raise SystemExit(
            "k2_final_campaign Sniper shards must run serially; "
            "use --sniper-jobs 1")
    limits = {
        "cache-sim": args.cache_sim_jobs or args.jobs,
        "gem5": args.gem5_jobs or args.jobs,
        "sniper": sniper_limit,
    }
    unknown_suites = sorted({
        shard.suite for shard in shards if shard.suite not in limits})
    if unknown_suites:
        raise SystemExit(
            "no concurrency cap configured for suite(s): " +
            ", ".join(unknown_suites))
    shards.sort(key=lambda shard: (-limits[shard.suite], shard.key))
    semaphores = {
        suite: threading.BoundedSemaphore(max(1, limit))
        for suite, limit in limits.items()
    }
    print(
        f"[local-shards] shards={len(shards)} jobs={args.jobs} "
        f"caps={limits} run_root={run_root}")

    stop_on_error = args.stop_on_error or (
        args.jobs == 1 and
        any(shard.profile == "k2_final_campaign" for shard in shards))
    if stop_on_error and args.jobs == 1:
        semaphore = threading.BoundedSemaphore(1)
        for shard in shards:
            try:
                shard, code, run_dir = run_shard(
                    shard, run_root, manifest_path, args, semaphore)
            except Exception as error:
                print(
                    f"[FAIL] local shard launcher error: {error}",
                    file=sys.stderr)
                return 1
            print(
                f"[{'ok' if code == 0 else 'FAIL'}] "
                f"{shard.suite} {shard.key} -> {run_dir}")
            if code != 0:
                print(
                    "[local-shards] stopped after first failed shard",
                    file=sys.stderr)
                return 1
        print("[local-shards] all shards completed")
        return 0

    failures = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [
            executor.submit(
                run_shard, shard, run_root, manifest_path, args,
                semaphores[shard.suite])
            for shard in shards
        ]
        for future in as_completed(futures):
            try:
                shard, code, run_dir = future.result()
            except Exception as error:
                failures += 1
                print(
                    f"[FAIL] local shard launcher error: {error}",
                    file=sys.stderr)
                continue
            print(
                f"[{'ok' if code == 0 else 'FAIL'}] "
                f"{shard.suite} {shard.key} -> {run_dir}")
            failures += code != 0
    if failures:
        print(f"[local-shards] {failures} shard(s) failed", file=sys.stderr)
        return 1
    print("[local-shards] all shards completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
