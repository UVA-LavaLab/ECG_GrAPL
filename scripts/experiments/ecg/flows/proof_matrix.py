#!/usr/bin/env python3
"""ECG proof matrix for component ablations before ISA work.

This script runs cache_sim ablations that separate replacement-only effects
from PFX prefetch effects:

- cache-alone baselines: LRU, SRRIP, GRASP, POPT
- ECG replacement modes with PFX disabled
- PFX-only under LRU replacement
- DBG/POPT/PFX combined modes
- offline adaptive selector rows synthesized from the replacement-only modes

gem5/DROPLET comparisons are handled through roi_matrix.py after the cache_sim
component story is stable.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
RESULTS_ROOT = PROJECT_ROOT / "results" / "ecg_experiments" / "proof_matrix"
ROI_MATRIX = PROJECT_ROOT / "scripts" / "experiments" / "ecg" / "roi_matrix.py"


DEFAULT_OPTIONS = {
    "pr": "-g 12 -k 16 -o 5 -n 1 -i 2",
    "bfs": "-g 12 -k 16 -o 5 -n 1 -r 0",
    "sssp": "-g 12 -k 16 -o 5 -n 1 -r 0 -d 1",
}

FILE_OPTIONS = {
    "pr": "-f {graph_path} -s -o 5 -n 1 -i 2",
    "bfs": "-f {graph_path} -s -o 5 -n 1 -r 0",
    "sssp": "-f {graph_path} -s -o 5 -n 1 -r 0 -d 1",
}


@dataclass(frozen=True)
class Ablation:
    label: str
    group: str
    policy: str
    pfx_mode: int = 0
    pfx_lookahead: int = 0
    note: str = ""


ABLATIONS = [
    Ablation("LRU_cache_only", "cache_alone", "LRU", note="No graph-aware replacement or PFX."),
    Ablation("SRRIP_cache_only", "cache_alone", "SRRIP", note="Prior generic replacement baseline."),
    Ablation("GRASP_DBG_only", "cache_alone", "GRASP", note="Prior DBG/degree-aware replacement."),
    Ablation("POPT_only", "cache_alone", "POPT", note="Prior P-OPT replacement."),
    Ablation("ECG_DBG_only", "ecg_replacement", "ECG:DBG_ONLY", note="ECG DBG replacement only."),
    Ablation("ECG_POPT_primary", "ecg_replacement", "ECG:POPT_PRIMARY", note="ECG POPT replacement plus DBG tiebreak."),
    Ablation("ECG_DBG_POPT", "ecg_replacement", "ECG:DBG_PRIMARY", note="DBG primary plus POPT tiebreak."),
    Ablation("ECG_POPT_TIE", "ecg_replacement", "ECG:POPT_TIE", note="SRRIP candidates plus dynamic POPT tiebreak and DBG fallback."),
    Ablation("ECG_EMBEDDED", "ecg_replacement", "ECG:ECG_EMBEDDED", note="Stored P-OPT hint plus DBG tiebreak; zero dynamic P-OPT lookup."),
    Ablation("ECG_EPOCH_EMBEDDED", "ecg_replacement", "ECG:ECG_EPOCH_EMBEDDED", note="Current-epoch compact P-OPT hint plus DBG tiebreak."),
    Ablation("ECG_COMBINED", "ecg_replacement", "ECG:ECG_COMBINED", note="Combined DBG and stored P-OPT hint insertion priority."),
    Ablation("PFX_degree_only", "pfx_only", "LRU", pfx_mode=1, pfx_lookahead=4,
             note="Degree-ranked PFX with LRU replacement."),
    Ablation("PFX_POPT_only", "pfx_only", "LRU", pfx_mode=2, pfx_lookahead=4,
             note="POPT-ranked PFX with LRU replacement."),
    Ablation("DBG_PFX", "combined", "ECG:DBG_ONLY", pfx_mode=2, pfx_lookahead=4,
             note="DBG replacement plus POPT-ranked PFX."),
    Ablation("POPT_PFX", "combined", "ECG:POPT_PRIMARY", pfx_mode=2, pfx_lookahead=4,
             note="POPT-primary replacement plus PFX."),
    Ablation("DBG_POPT_PFX", "combined", "ECG:DBG_PRIMARY", pfx_mode=2, pfx_lookahead=4,
             note="DBG+POPT replacement plus PFX."),
]


@dataclass(frozen=True)
class AdaptiveSelector:
    label: str
    candidates: tuple[str, ...]
    note: str


ADAPTIVE_METRIC = "memory_accesses"
ADAPTIVE_GROUP_FIELDS = (
    "benchmark",
    "simulator",
    "options",
    "l1d_size",
    "l2_size",
    "l3_size",
    "l3_ways",
    "line_size",
    "prefetcher",
    "prefetcher_level",
    "section",
)
ADAPTIVE_SELECTORS = [
    AdaptiveSelector(
        "ECG_ADAPTIVE_ORACLE",
        (
            "ECG_DBG_only",
            "ECG_POPT_primary",
            "ECG_DBG_POPT",
            "ECG_POPT_TIE",
            "ECG_EMBEDDED",
            "ECG_EPOCH_EMBEDDED",
            "ECG_COMBINED",
        ),
        "Offline selector over all ECG replacement modes; includes dynamic P-OPT primary.",
    ),
    AdaptiveSelector(
        "ECG_ADAPTIVE_NO_FULL_POPT",
        (
            "ECG_DBG_only",
            "ECG_DBG_POPT",
            "ECG_POPT_TIE",
            "ECG_EMBEDDED",
            "ECG_EPOCH_EMBEDDED",
            "ECG_COMBINED",
        ),
        "Offline selector over ECG modes that exclude full dynamic P-OPT primary.",
    ),
]


def now_tag() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def sanitize(text: str) -> str:
    return "".join(ch for ch in text if ch.isalnum() or ch in "_.-")


def number(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def benchmark_options(args: argparse.Namespace, benchmark: str) -> str:
    explicit = {
        "pr": args.pr_options,
        "bfs": args.bfs_options,
        "sssp": args.sssp_options,
    }
    if explicit.get(benchmark):
        return explicit[benchmark]
    if args.graph_path:
        graph_path = Path(args.graph_path)
        if not graph_path.is_absolute():
            graph_path = PROJECT_ROOT / graph_path
        return FILE_OPTIONS[benchmark].format(graph_path=graph_path)
    if benchmark in DEFAULT_OPTIONS:
        return DEFAULT_OPTIONS[benchmark]
    raise ValueError(f"unsupported benchmark: {benchmark}")


def build_targets(args: argparse.Namespace) -> None:
    if args.no_build or args.dry_run:
        return
    targets = [f"sim-{benchmark}" for benchmark in args.benchmarks]
    print(f"[build] make {' '.join(targets)}")
    subprocess.run(["make"] + targets, cwd=str(PROJECT_ROOT), check=True)


def selected_ablations(args: argparse.Namespace) -> list[Ablation]:
    if not args.ablations:
        return ABLATIONS
    wanted = set(args.ablations)
    known = {ablation.label for ablation in ABLATIONS}
    missing = sorted(wanted - known)
    if missing:
        raise SystemExit(f"unknown ablations: {', '.join(missing)}")
    return [ablation for ablation in ABLATIONS if ablation.label in wanted]


def run_ablation(
    args: argparse.Namespace,
    out_dir: Path,
    benchmark: str,
    ablation: Ablation,
) -> list[dict[str, Any]]:
    options = benchmark_options(args, benchmark)
    run_dir = out_dir / benchmark / sanitize(ablation.label)
    env = dict(os.environ)
    env.update({
        "OMP_NUM_THREADS": str(args.omp_threads),
    })

    cmd = [
        sys.executable,
        str(ROI_MATRIX),
        "--suite", "cache-sim",
        "--benchmark", benchmark,
        "--options", options,
        "--policies", ablation.policy,
        "--l1d-size", args.l1d_size,
        "--l2-size", args.l2_size,
        "--l3-sizes", *args.l3_sizes,
        "--l3-ways", args.l3_ways,
        "--line-size", args.line_size,
        "--out-dir", str(run_dir),
        "--timeout-cache", str(args.timeout_cache),
        "--no-build",
    ]
    if ablation.pfx_mode:
        pfx_mode = {1: "degree", 2: "popt"}.get(ablation.pfx_mode)
        if pfx_mode is None:
            raise RuntimeError(
                f"Unsupported proof-matrix PFX mode: {ablation.pfx_mode}")
        cmd.extend([
            "--prefetcher", "ECG_PFX",
            "--ecg-pfx-mode", pfx_mode,
            "--ecg-pfx-window", str(args.pfx_window),
            "--ecg-pfx-lookahead", str(ablation.pfx_lookahead),
        ])
    if args.dry_run:
        cmd.append("--dry-run")

    print(f"[proof] {benchmark} {ablation.label}: {' '.join(shlex.quote(part) for part in cmd)}")
    if args.dry_run:
        return []

    log_path = out_dir / "logs" / f"{benchmark}_{sanitize(ablation.label)}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w") as log:
        result = subprocess.run(
            cmd,
            cwd=str(PROJECT_ROOT),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.timeout_cache + 120,
        )

    rows: list[dict[str, Any]] = []
    matrix_path = run_dir / "roi_matrix.csv"
    if result.returncode != 0 or not matrix_path.exists():
        rows.append({
            "benchmark": benchmark,
            "ablation": ablation.label,
            "ablation_group": ablation.group,
            "policy_spec": ablation.policy,
            "status": "error",
            "error": f"exit_code={result.returncode}",
            "log_path": str(log_path),
        })
        return rows

    with matrix_path.open(newline="") as fh:
        for row in csv.DictReader(fh):
            row.update({
                "ablation": ablation.label,
                "ablation_group": ablation.group,
                "ablation_note": ablation.note,
                "policy_spec": ablation.policy,
                "proof_pfx_mode": ablation.pfx_mode,
                "proof_pfx_window": args.pfx_window,
                "proof_pfx_lookahead": ablation.pfx_lookahead,
                "proof_log_path": str(log_path),
                "proof_run_dir": str(run_dir),
            })
            rows.append(row)
    return rows


def write_outputs(out_dir: Path, rows: list[dict[str, Any]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "proof_matrix.json"
    csv_path = out_dir / "proof_matrix.csv"
    json_path.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n")
    fields = sorted({key for row in rows for key in row.keys()})
    with csv_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    print(f"[write] {json_path}")
    print(f"[write] {csv_path}")


def write_completion_marker(
        out_dir: Path, rows: list[dict[str, Any]]) -> None:
    marker = out_dir / "proof_matrix.complete.json"
    temp = out_dir / "proof_matrix.complete.json.tmp"
    outputs = {}
    for name in ("proof_matrix.csv", "proof_matrix.json"):
        path = out_dir / name
        outputs[name] = {
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "size": path.stat().st_size,
            "rows": len(rows),
        }
    payload = {
        "complete": True,
        "all_rows_ok": bool(rows) and all(
            row.get("status") == "ok" for row in rows),
        "matrix_id": os.environ.get(
            "GRAPHBREW_MATRIX_ID", out_dir.name),
        "shard_group": os.environ.get(
            "GRAPHBREW_SHARD_GROUP", out_dir.parent.name),
        "config_hash": os.environ.get(
            "GRAPHBREW_MATRIX_CONFIG_HASH", ""),
        "rows": len(rows),
        "outputs": outputs,
    }
    temp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    temp.replace(marker)


def adaptive_group_key(row: dict[str, Any]) -> tuple[str, ...]:
    return tuple(str(row.get(field, "")) for field in ADAPTIVE_GROUP_FIELDS)


def synthesize_adaptive_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, ...], dict[str, dict[str, Any]]] = {}
    for row in rows:
        if row.get("status") != "ok" or row.get("ablation_group") != "ecg_replacement":
            continue
        grouped.setdefault(adaptive_group_key(row), {})[str(row.get("ablation", ""))] = row

    synthetic: list[dict[str, Any]] = []
    for key in sorted(grouped):
        table = grouped[key]
        for selector in ADAPTIVE_SELECTORS:
            if any(label not in table for label in selector.candidates):
                continue
            candidate_rows = []
            for rank, label in enumerate(selector.candidates):
                value = number(table[label].get(ADAPTIVE_METRIC))
                if value is not None:
                    candidate_rows.append((value, rank, label, table[label]))
            if not candidate_rows:
                continue
            selected_value, _, selected_label, selected_row = min(candidate_rows)
            row = dict(selected_row)
            row.update({
                "ablation": selector.label,
                "ablation_group": "adaptive_selector",
                "ablation_note": selector.note,
                "policy": "ECG_ADAPTIVE",
                "policy_label": selector.label,
                "policy_spec": selector.label,
                "adaptive_selection_metric": ADAPTIVE_METRIC,
                "adaptive_selected_ablation": selected_label,
                "adaptive_selected_policy_spec": selected_row.get("policy_spec", ""),
                "adaptive_selected_value": selected_value,
                "adaptive_candidates": ",".join(selector.candidates),
            })
            synthetic.append(row)
    return synthetic


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run ECG cache_sim proof ablation matrix.")
    parser.add_argument("--benchmarks", nargs="+", default=["pr", "bfs", "sssp"],
                        choices=["pr", "bfs", "sssp"])
    parser.add_argument("--ablations", nargs="+", default=[],
                        help="Optional ablation labels to run. Defaults to all.")
    parser.add_argument("--graph-path", default="", help="Optional .sg/.mtx graph path used to build deterministic file-backed benchmark options.")
    parser.add_argument("--pr-options", default="", help="Override PR benchmark options. Defaults to --graph-path or synthetic g12.")
    parser.add_argument("--bfs-options", default="", help="Override BFS benchmark options. Defaults to --graph-path or synthetic g12.")
    parser.add_argument("--sssp-options", default="", help="Override SSSP benchmark options. Defaults to --graph-path or synthetic g12.")
    parser.add_argument("--l1d-size", default="1kB")
    parser.add_argument("--l2-size", default="2kB")
    parser.add_argument("--l3-sizes", nargs="+", default=["4kB"])
    parser.add_argument("--l3-ways", default="16")
    parser.add_argument("--line-size", default="64")
    parser.add_argument("--pfx-window", default="16")
    parser.add_argument("--omp-threads", type=int, default=1)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--timeout-cache", type=int, default=900)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    out_dir = Path(args.out_dir) if args.out_dir else RESULTS_ROOT / now_tag()
    if not out_dir.is_absolute():
        out_dir = PROJECT_ROOT / out_dir

    if not args.dry_run:
        (out_dir / "proof_matrix.complete.json").unlink(missing_ok=True)
    build_targets(args)
    rows: list[dict[str, Any]] = []
    for benchmark in args.benchmarks:
        for ablation in selected_ablations(args):
            rows.extend(run_ablation(args, out_dir, benchmark, ablation))

    if not args.dry_run:
        rows.extend(synthesize_adaptive_rows(rows))
        write_outputs(out_dir, rows)
        write_completion_marker(out_dir, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))