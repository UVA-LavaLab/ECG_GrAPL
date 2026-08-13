#!/usr/bin/env python3
"""Generate one-policy Slurm shards from the experiment manifest."""

from __future__ import annotations

import argparse
import csv
import sys
from datetime import datetime
from pathlib import Path


ECG_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ECG_DIR.parents[2]
sys.path.insert(0, str(ECG_DIR))

from flows import experiment_run  # noqa: E402


def build_rows(args: argparse.Namespace) -> list[tuple[str, ...]]:
    manifest = experiment_run.load_manifest(Path(args.manifest))
    graph_sets = manifest.get("graph_sets", {})
    rows: list[tuple[str, ...]] = []

    for profile in args.profile:
        for stage in manifest.get("stages", []):
            if stage.get("kind") != "roi_matrix":
                continue
            if profile not in stage.get("profiles", []):
                continue
            if args.only and not any(
                    token in str(stage["name"]) for token in args.only):
                continue
            settings = experiment_run.merged_defaults(manifest, stage)
            settings, screen_graphs = experiment_run.apply_screen_config(
                settings)
            policy_sharding_allowed = bool(
                settings.get("policy_sharding_allowed", True))
            if screen_graphs is not None:
                screen = settings["_screen_config_data"]
                policy_sharding_allowed = bool(
                    screen.get("execution", {}).get(
                        "policy_sharding_allowed",
                        policy_sharding_allowed))
                if not policy_sharding_allowed and not args.whole_cell:
                    raise SystemExit(
                        f"stage {stage['name']} requires whole-cell jobs; "
                        "use --whole-cell")
            if not policy_sharding_allowed and not args.whole_cell:
                raise SystemExit(
                    f"stage {stage['name']} requires whole-cell jobs; "
                    "use --whole-cell")
            blocked_reason = str(settings.get("blocked_reason", ""))
            if blocked_reason and not args.allow_blocked:
                raise SystemExit(
                    f"stage {stage['name']} is blocked: {blocked_reason}")

            if screen_graphs is not None:
                graphs = screen_graphs
            else:
                graph_set_name = str(stage["graph_set"])
                if graph_set_name not in graph_sets:
                    raise SystemExit(
                        f"unknown graph_set={graph_set_name!r} "
                        f"in stage {stage['name']}")
                graphs = graph_sets[graph_set_name]
            policies = (
                ["__whole__"] if args.whole_cell else
                experiment_run.filter_policy_specs(
                    [str(policy) for policy in settings.get("policies", [])],
                    args.policy,
                ))
            for graph in graphs:
                graph_name = str(graph["name"])
                if not experiment_run.token_matches(graph_name, args.graph):
                    continue
                if not experiment_run.graph_uses_synthetic_options(graph):
                    experiment_run.find_graph_path(
                        graph, Path(args.graph_dir),
                        args.allow_missing_graphs)
                for benchmark in settings.get("benchmarks", []):
                    if not experiment_run.token_matches(
                            str(benchmark), args.benchmark):
                        continue
                    for policy in policies:
                        rows.append((
                            profile,
                            str(stage["name"]),
                            graph_name,
                            str(benchmark),
                            policy,
                            args.run_tag,
                        ))

    if args.smoke and rows:
        return rows[:1]
    return rows


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Slurm shard TSV rows from experiment_manifest.json")
    parser.add_argument("--profile", nargs="+", default=["ecg_smoke"])
    parser.add_argument("--manifest", default=str(experiment_run.DEFAULT_MANIFEST))
    parser.add_argument(
        "--graph-dir", default=str(PROJECT_ROOT / "results" / "graphs"))
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--graph", nargs="*", default=[])
    parser.add_argument("--benchmark", nargs="*", default=[])
    parser.add_argument("--policy", nargs="*", default=[])
    parser.add_argument(
        "--run-tag",
        default=f"ecg_{datetime.now().strftime('%Y%m%d_%H%M%S')}")
    parser.add_argument("--out", required=True)
    parser.add_argument("--allow-missing-graphs", action="store_true")
    parser.add_argument(
        "--allow-blocked", action="store_true",
        help="Generate inspection-only rows for a manifest-blocked stage.")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument(
        "--whole-cell", action="store_true",
        help="Emit one shard per graph/benchmark with the complete policy roster.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.run_tag = experiment_run.sanitize(args.run_tag)
    rows = build_rows(args)
    if not rows:
        raise SystemExit("no shard rows matched the requested filters")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as handle:
        csv.writer(handle, delimiter="\t", lineterminator="\n").writerows(rows)
    print(f"[slurm-shards] wrote {len(rows)} rows to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
