#!/usr/bin/env python3
"""Run experiment profiles, collect complete CSVs, and generate summaries."""

from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from experiment_run import recover_roi_comparison_config_hash  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parents[4]
ECG_DIR = PROJECT_ROOT / "scripts" / "experiments" / "ecg"
EXPERIMENT_RUNNER = ECG_DIR / "flows" / "experiment_run.py"
REFERENCE_PYTHON = Path("/usr/bin/python3.12")


def execution_python(args: argparse.Namespace) -> Path:
    return (
        REFERENCE_PYTHON
        if getattr(args, "require_pinned_python", False)
        else Path(sys.executable))


def clean_profile_environment() -> dict[str, str]:
    return {
        "PATH": "/usr/bin:/bin",
        "HOME": os.environ.get("HOME", "/tmp"),
        "TMPDIR": "/tmp",
        "LC_ALL": "C",
        "LANG": "C",
    }
RESULTS_ROOT = PROJECT_ROOT / "results" / "ecg_experiments" / "aggregates"

POLICY_ORDER = [
    "LRU",
    "SRRIP",
    "GRASP",
    "POPT",
    "POPT_UNCHARGED",
    "ECG_DBG_ONLY",
    "ECG_DBG_PRIMARY_CHARGED",
    "ECG_DBG_PRIMARY",
    "ECG_POPT_PRIMARY",
    "ECG_REUSE_PLAN_GRASP",
    "ECG_REUSE_PLAN_GRASP_FLOWTHROUGH",
    "ECG_REUSE_PLAN_LRU",
    "ECG_REUSE_PLAN_RRIP",
    "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
    "ECG_REUSE_PLAN_DEGREE",
    "ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH",
    "ECG_REUSE_PLAN_EPOCH",
    "ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH",
    "ECG_REUSE_PLAN_SHORTCIRCUIT",
    "ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH",
    "ECG_REUSE_PLAN",
    "ECG_REUSE_PLAN_ONLINE",
    "ECG_REUSE_PLAN_FLOWTHROUGH",
    "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
    "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH",
    "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
]

BENCHMARK_ORDER = ["pr", "bfs", "sssp", "cc", "bc", "tc"]
BENCHMARK_LABELS = {
    "pr": "PR",
    "bfs": "BFS",
    "sssp": "SSSP",
    "cc": "CC",
    "bc": "BC",
    "tc": "TC",
}

POLICY_LABELS = {
    "LRU": "LRU",
    "SRRIP": "SRRIP",
    "GRASP": "GRASP",
    "POPT": "P-OPT",
    "POPT_UNCHARGED": "P-OPT oracle",
    "ECG_DBG_ONLY": "ECG-D",
    "ECG_DBG_PRIMARY_CHARGED": "ECG-H+C",
    "ECG_DBG_PRIMARY": "ECG-H",
    "ECG_POPT_PRIMARY": "ECG-P",
    "ECG_REUSE_PLAN_GRASP": "ReusePlan-GRASP",
    "ECG_REUSE_PLAN_GRASP_FLOWTHROUGH": "ReusePlan-GRASP+FlowThrough",
    "ECG_REUSE_PLAN_LRU": "ReusePlan-LRU",
    "ECG_REUSE_PLAN_RRIP": "ReusePlan-RRIP",
    "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH":
        "ReusePlan-epoch-admission+FlowThrough",
    "ECG_REUSE_PLAN_DEGREE": "ReusePlan-degree",
    "ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH": "ReusePlan-degree+FlowThrough",
    "ECG_REUSE_PLAN_EPOCH": "ReusePlan-epoch",
    "ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH": "ReusePlan-epoch+FlowThrough",
    "ECG_REUSE_PLAN_SHORTCIRCUIT": "ReusePlan-shortcircuit",
    "ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH":
        "ReusePlan-shortcircuit+FlowThrough",
    "ECG_REUSE_PLAN": "ReusePlan",
    "ECG_REUSE_PLAN_ONLINE": "ReusePlan-online",
    "ECG_REUSE_PLAN_FLOWTHROUGH": "ReusePlan+SS",
    "ECG_REUSE_PLAN_LRU_FLOWTHROUGH": "ReusePlan-LRU+SS",
    "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH": "ReusePlan-online+SS",
    "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH": "ReusePlan-online+adaptive-SS",
}

POLICY_DESCRIPTIONS = {
    "LRU": "Least recently used baseline",
    "SRRIP": "Static re-reference interval prediction baseline",
    "GRASP": "GRASP degree-aware replacement",
    "POPT": "P-OPT with matrix capacity and stream overhead charged",
    "POPT_UNCHARGED": "P-OPT oracle-capacity replacement",
    "ECG_DBG_ONLY": "ECG DBG-only mode, GRASP-equivalence check",
    "ECG_DBG_PRIMARY_CHARGED": "ECG hybrid mode with P-OPT overhead charged",
    "ECG_DBG_PRIMARY": "ECG DBG-primary hybrid mode",
    "ECG_POPT_PRIMARY": "ECG P-OPT-primary oracle-validation mode",
    "ECG_REUSE_PLAN_GRASP": "ReusePlan transport with GRASP-only victim selection",
    "ECG_REUSE_PLAN_GRASP_FLOWTHROUGH":
        "ReusePlan transport with GRASP-only victim selection and FlowThrough",
    "ECG_REUSE_PLAN_LRU": "ReusePlan transport with LRU-only victim selection",
    "ECG_REUSE_PLAN_LRU_FLOWTHROUGH":
        "ReusePlan transport with LRU-only victim selection and FlowThrough",
    "ECG_REUSE_PLAN_RRIP": "ReusePlan transport with RRIP-first victim selection",
    "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH":
        "ReusePlan future-distance admission with RRIP-first and FlowThrough",
    "ECG_REUSE_PLAN_DEGREE": "ReusePlan transport with degree-first victim selection",
    "ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH":
        "ReusePlan transport with degree-first victim selection and FlowThrough",
    "ECG_REUSE_PLAN_EPOCH": "ReusePlan transport with epoch-first victim selection",
    "ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH":
        "ReusePlan transport with epoch-first victim selection and FlowThrough",
    "ECG_REUSE_PLAN_SHORTCIRCUIT":
        "ReusePlan transport with legacy shortcircuit victim selection",
    "ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH":
        "ReusePlan transport with legacy shortcircuit selection and FlowThrough",
    "ECG_REUSE_PLAN": "Two-epoch ECG replacement",
    "ECG_REUSE_PLAN_ONLINE": "Online set-dueling across all ReusePlan victim arms",
    "ECG_REUSE_PLAN_FLOWTHROUGH": "Two-epoch ECG plus FlowThrough placement",
    "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH": "Online ReusePlan plus FlowThrough placement",
    "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH":
        "Online ReusePlan plus allocate-vs-FlowThrough placement dueling",
}

POLICY_COLORS = {
    "LRU": "#BDBDBD",
    "SRRIP": "#8E8E8E",
    "GRASP": "#4C78A8",
    "POPT": "#F58518",
    "POPT_UNCHARGED": "#F2B872",
    "ECG_DBG_ONLY": "#8CD17D",
    "ECG_DBG_PRIMARY_CHARGED": "#B79A20",
    "ECG_DBG_PRIMARY": "#54A24B",
    "ECG_POPT_PRIMARY": "#B279A2",
    "ECG_REUSE_PLAN_GRASP": "#8CD17D",
    "ECG_REUSE_PLAN_LRU": "#9C9C9C",
    "ECG_REUSE_PLAN_RRIP": "#59A14F",
    "ECG_REUSE_PLAN_DEGREE": "#2F8F9D",
    "ECG_REUSE_PLAN_EPOCH": "#1F9D55",
    "ECG_REUSE_PLAN": "#2CA02C",
    "ECG_REUSE_PLAN_ONLINE": "#007A3D",
    "ECG_REUSE_PLAN_FLOWTHROUGH": "#006D2C",
    "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH": "#00441B",
    "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH": "#00331F",
}

POLICY_HATCHES = {
    "POPT": "///",
    "ECG_DBG_PRIMARY_CHARGED": "///",
}

TABLE_HEADER_LABELS = {
    "avg_l3_miss_delta_pct": "avg LLC delta (\\%)",
    "avg_l3_miss_reduction_vs_lru_pct": "avg LLC red. (\\%)",
    "avg_popt_matrix_stream_cache_lines": "matrix stream lines",
    "avg_popt_reserved_bytes": "reserved B",
    "avg_popt_reserved_ways": "reserved ways",
    "avg_reserved_l3_pct": "reserved LLC (\\%)",
    "avg_sim_ticks": "avg ticks",
    "avg_speedup_vs_lru": "avg speedup",
    "candidate_short": "candidate",
    "charged_policy": "charged",
    "dynamic_popt_matrix": "P-OPT lookup",
    "l3_miss_delta_pct": "LLC delta (\\%)",
    "max_abs_l3_miss_delta_pct": "max LLC delta (\\%)",
    "max_abs_tick_delta_pct": "max tick delta (\\%)",
    "oracle_policy": "oracle",
    "passes_tolerance": "pass",
    "policy_short": "policy",
    "popt_reserved_ways": "reserved ways",
    "prefetch_fill_useful_pct": "useful/fills (\\%)",
    "prefetch_accuracy_pct": "useful/issued (\\%)",
    "prefetch_request_useful_pct": "useful/requests (\\%)",
    "prefetch_unused_pct": "unused/issued (\\%)",
    "reference_short": "reference",
    "tick_delta_pct": "tick delta (\\%)",
    "traffic_per_demand_access": "traffic/demand",
    "threads": "threads",
    "thread_speedup_vs_1t": "speedup vs 1T",
    "parallel_efficiency_vs_1t": "parallel eff.",
    "avg_sniper_cpi_data_cache_pct": r"data cache (\%)",
    "avg_sniper_cpi_data_dram_pct": r"DRAM (\%)",
    "avg_sniper_cpi_data_llc_pct": r"LLC (\%)",
}

ROI_COMPARE_KEYS = (
    "final_shard_group", "final_matrix_id", "final_matrix_config_hash",
    "simulator", "gem5_cpu_type", "final_graph", "benchmark", "prefetcher",
    "l3_size", "threads", "section",
)

FIGURE_WIDTH = 3.35
FIGURE_DPI = 300
LEGACY_FIGURES = (
    "avg_ticks_by_policy.png",
    "avg_l3_misses_by_policy.png",
    "charged_tick_overhead.png",
)
STALE_SPEEDUP_FIGURES = (
    "replacement_speedup_by_benchmark.svg",
    "replacement_speedup_by_benchmark.png",
    "replacement_speedup_vs_lru.svg",
    "replacement_speedup_vs_lru.png",
    "sniper_replacement_speedup_by_benchmark.svg",
    "sniper_replacement_speedup_by_benchmark.png",
)
STALE_SNIPER_TIMING_OUTPUTS = (
    ("aggregate", "sniper_relative_metrics.csv"),
    ("aggregate", "sniper_relative_policy_summary.csv"),
)

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def now_tag() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def resolve_path(path_text: str) -> Path:
    path = Path(path_text)
    return path if path.is_absolute() else PROJECT_ROOT / path


def command_text(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def read_csv(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def output_descriptor(path: Path) -> dict[str, Any]:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    rows = None
    if path.suffix == ".csv":
        with path.open(newline="") as handle:
            rows = max(sum(1 for _ in handle) - 1, 0)
    elif path.suffix == ".json":
        try:
            payload = json.loads(path.read_text())
            if isinstance(payload, list):
                rows = len(payload)
        except (OSError, json.JSONDecodeError):
            rows = None
    return {
        "sha256": digest.hexdigest(),
        "size": path.stat().st_size,
        "rows": rows,
    }


def bound_file(path: Path) -> dict[str, Any]:
    return {
        "path": str(path.resolve()),
        **output_descriptor(path),
    }


def marker_outputs_valid(payload: dict[str, Any], base_dir: Path) -> bool:
    expected = payload.get("outputs")
    if not isinstance(expected, dict) or not expected:
        return False
    for name, descriptor in expected.items():
        path = base_dir / name
        if not path.exists() or output_descriptor(path) != descriptor:
            return False
    return True


def discover_input_run_dirs(paths: list[Path]) -> list[Path]:
    discovered = set()
    for path in paths:
        path = path.resolve()
        if any((path / name).is_file() for name in (
                "resolved_manifest.json",
                "combined_roi_matrix.csv",
                "combined_proof_matrix.csv")):
            discovered.add(path)
            continue
        for manifest in path.glob("**/resolved_manifest.json"):
            discovered.add(manifest.parent.resolve())
    return sorted(discovered)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = sorted({key for row in rows for key in row.keys()})
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    print(f"[write] {path}")


def as_float(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def parse_size_bytes(value: Any) -> float | None:
    if value in (None, ""):
        return None
    text = str(value).strip()
    if not text:
        return None
    suffixes = {
        "kb": 1024.0,
        "kib": 1024.0,
        "mb": 1024.0 * 1024.0,
        "mib": 1024.0 * 1024.0,
        "gb": 1024.0 * 1024.0 * 1024.0,
        "gib": 1024.0 * 1024.0 * 1024.0,
        "b": 1.0,
    }
    lower = text.lower()
    for suffix, multiplier in sorted(suffixes.items(), key=lambda item: -len(item[0])):
        if lower.endswith(suffix):
            number = lower[: -len(suffix)].strip()
            try:
                return float(number) * multiplier
            except ValueError:
                return None
    try:
        return float(lower)
    except ValueError:
        return None


def pct_delta(value: float | None, baseline: float | None) -> float | None:
    if value is None or baseline in (None, 0.0):
        return None
    return ((value - baseline) / baseline) * 100.0


def safe_ratio(numerator: float | None, denominator: float | None) -> float | None:
    if numerator is None or denominator in (None, 0.0):
        return None
    return numerator / denominator


def thread_count(row: dict[str, Any]) -> int | None:
    value = as_float(row.get("threads"))
    if value is None or value <= 0.0:
        return None
    return int(value)


def metric_direction(value: float | None, reference: float, tolerance: float = 1.0e-9) -> str:
    if value is None:
        return "missing"
    delta = value - reference
    if abs(delta) <= tolerance:
        return "neutral"
    return "better" if delta > 0.0 else "worse"


def avg(values: Iterable[float]) -> float | None:
    data = list(values)
    return (sum(data) / len(data)) if data else None


def geo_mean(values: Iterable[float]) -> float | None:
    data = [value for value in values if value > 0.0]
    if not data:
        return None
    return math.exp(sum(math.log(value) for value in data) / len(data))


def policy_sort_key(policy: str) -> tuple[int, str]:
    try:
        return (POLICY_ORDER.index(policy), policy)
    except ValueError:
        return (len(POLICY_ORDER), policy)


def benchmark_sort_key(benchmark: str) -> tuple[int, str]:
    try:
        return (BENCHMARK_ORDER.index(benchmark), benchmark)
    except ValueError:
        return (len(BENCHMARK_ORDER), benchmark)


def benchmark_label(benchmark: str) -> str:
    return BENCHMARK_LABELS.get(benchmark, benchmark.upper())


def policy_label(policy: str) -> str:
    return POLICY_LABELS.get(policy, policy)


def policy_label_rows(policies: Iterable[str]) -> list[dict[str, Any]]:
    return [
        {
            "policy_label": policy,
            "figure_label": policy_label(policy),
            "description": POLICY_DESCRIPTIONS.get(policy, ""),
        }
        for policy in sorted(set(policies), key=policy_sort_key)
    ]


def effective_l3_misses(row: dict[str, Any]) -> float | None:
    uniform = as_float(row.get("l3_misses_with_overhead"))
    if uniform is not None:
        return uniform
    charged = as_float(row.get("popt_charged_l3_misses_plus_matrix_stream"))
    if charged is not None:
        return charged
    return as_float(row.get("l3_misses"))


def timing_valid_for_speedup(row: dict[str, Any]) -> bool:
    if (
            str(row.get("simulator", "")).strip().lower() != "gem5" or
            str(row.get("gem5_cpu_type", "")).strip() != "O3"):
        return False
    raw_value = row.get("timing_valid_for_speedup")
    if raw_value in (None, ""):
        return False
    value = str(raw_value).strip().lower()
    return value not in {"0", "false", "no", "invalid"}


def timing_valid_label(row: dict[str, Any]) -> str:
    return "1" if timing_valid_for_speedup(row) else "0"


def timing_model_label(row: dict[str, Any]) -> str:
    value = row.get("timing_model")
    if value not in (None, ""):
        return str(value)
    if row.get("prefetcher") == "ECG_PFX" and row.get("simulator") in ("gem5", "sniper"):
        return "prototype_explicit_hint_delivery"
    return ""


def compare_key(row: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(row.get(key, "") for key in ROI_COMPARE_KEYS)


def run_profile(args: argparse.Namespace, run_root: Path, profile: str) -> Path:
    run_dir = run_root / profile
    command = [
        str(execution_python(args)), "-I", str(EXPERIMENT_RUNNER),
        "--profile", profile, "--run-dir", str(run_dir)]
    if args.require_pinned_python:
        command.append("--require-reference-python")
    if args.dry_run:
        command.append("--dry-run")
    if args.no_build:
        command.append("--no-build")
    if args.allow_missing_graphs:
        command.append("--allow-missing-graphs")
    if args.force:
        command.append("--force")
    if not args.stop_on_error:
        command.append("--no-stop-on-error")

    log_dir = run_root / "pipeline_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{profile}.log"
    print(f"[profile] {profile}: {command_text(command)}")

    with log_path.open("w") as log:
        log.write(f"$ {command_text(command)}\n")
        log.flush()
        result = subprocess.run(
            command,
            cwd=str(PROJECT_ROOT),
            env=clean_profile_environment(),
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        log.write(f"\n[pipeline_profile_exit_code] {result.returncode}\n")
    if result.returncode != 0 and args.stop_on_error:
        raise SystemExit(f"profile failed: {profile}; see {log_path}")
    return run_dir


def parse_expected_policy_labels(row: dict[str, Any]) -> set[str]:
    raw = row.get("final_expected_policy_labels", "")
    if isinstance(raw, list):
        return {str(value) for value in raw}
    if not raw:
        return set()
    try:
        parsed = json.loads(str(raw))
    except json.JSONDecodeError:
        return set()
    if not isinstance(parsed, list):
        return set()
    return {str(value) for value in parsed}


def semantic_work_group_matches(group_rows: list[dict[str, Any]]) -> bool:
    limits = {
        int(row.get("sniper_semantic_edge_limit") or 0)
        for row in group_rows
    }
    if limits == {0}:
        return True
    if len(limits) != 1 or 0 in limits:
        return False
    work = {
        (
            int(row.get("sniper_semantic_edge_visits") or 0),
            int(row.get("sniper_semantic_truncated") or 0),
        )
        for row in group_rows
    }
    semantic_results = {
        str(row.get("sniper_semantic_result") or "")
        for row in group_rows
    }
    transport_widths = {
        (
            int(row.get("sniper_transport_record_bytes") or 0),
            int(float(row.get("edge_stream_bytes_per_edge") or 0)),
        )
        for row in group_rows
    }
    matched = (
        len(work) == 1 and len(semantic_results) == 1 and
        "" not in semantic_results and len(transport_widths) == 1 and
        next(iter(transport_widths))[0] > 0)
    if matched:
        for row in group_rows:
            row["semantic_work_matched"] = "1"
    return matched


def complete_matrix_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[compare_key(row)].append(row)

    complete: list[dict[str, Any]] = []
    for group_key, group_rows in groups.items():
        config_hashes = {
            str(row.get("final_matrix_config_hash", ""))
            for row in group_rows
        }
        expected: set[str] = set()
        for row in group_rows:
            expected.update(parse_expected_policy_labels(row))
        actual = {
            str(row.get("policy_label", "")) for row in group_rows
            if row.get("status") == "ok" and
            row.get("final_output_status", "ok") == "ok"
        }
        semantic_work_ok = semantic_work_group_matches(group_rows)
        if (config_hashes == {""} or len(config_hashes) != 1 or
                not expected or actual != expected or not semantic_work_ok):
            print(
                f"[skip] incomplete policy group={group_key} "
                f"hashes={sorted(config_hashes)} "
                f"expected={sorted(expected)} actual={sorted(actual)} "
                f"semantic_work_ok={semantic_work_ok}")
            continue
        complete.extend(group_rows)
    return complete


def collect_csvs(run_dirs: list[Path], input_csvs: list[Path]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    roi_rows: list[dict[str, Any]] = []
    proof_rows: list[dict[str, Any]] = []

    def add_rows(
            path: Path, kind: str, run_dir: Path | None = None,
            expected_job: dict[str, Any] | None = None,
            expected_jobs_by_id: dict[str, dict[str, Any]] | None = None
            ) -> None:
        rows = read_csv(path)
        marker_payload: dict[str, Any] = {}
        if kind != "proof" and path.name == "roi_matrix.csv":
            marker = path.parent / "roi_matrix.complete.json"
            try:
                marker_payload = (
                    json.loads(marker.read_text()) if marker.exists() else {})
                complete = (
                    marker_payload.get("complete") is True and
                    marker_payload.get("all_rows_ok") is True and
                    marker_outputs_valid(marker_payload, path.parent))
            except (OSError, json.JSONDecodeError):
                complete = False
            if (not complete or not rows or
                    any(row.get("status") != "ok" for row in rows)):
                print(f"[skip] incomplete ROI matrix: {path}")
                return
        if kind == "proof" and path.name == "proof_matrix.csv":
            marker = path.parent / "proof_matrix.complete.json"
            try:
                marker_payload = (
                    json.loads(marker.read_text()) if marker.exists() else {})
                complete = (
                    marker_payload.get("complete") is True and
                    marker_payload.get("all_rows_ok") is True and
                    marker_outputs_valid(marker_payload, path.parent))
            except (OSError, json.JSONDecodeError):
                complete = False
            if (not complete or not rows or
                    any(row.get("status") != "ok" for row in rows)):
                print(f"[skip] incomplete proof matrix: {path}")
                return
        if expected_job is not None and marker_payload:
            expected_hash = str(
                expected_job.get("metadata", {}).get("config_hash", ""))
            if (not expected_hash or
                    marker_payload.get("config_hash") != expected_hash):
                print(
                    f"[skip] stale matrix hash: {path} "
                    f"expected={expected_hash} "
                    f"actual={marker_payload.get('config_hash', '')}")
                return
            marker_comparison_hash = str(
                marker_payload.get("comparison_config_hash", ""))
            expected_comparison_hash = str(
                expected_job.get("metadata", {}).get(
                    "comparison_config_hash", ""))
            if (marker_comparison_hash and expected_comparison_hash and
                    marker_comparison_hash != expected_comparison_hash):
                print(
                    f"[skip] stale comparison hash: {path} "
                    f"expected={expected_comparison_hash} "
                    f"actual={marker_comparison_hash}")
                return
            if not marker_comparison_hash:
                marker_comparison_hash = (
                    expected_comparison_hash or
                    recover_roi_comparison_config_hash(expected_job))
                if marker_comparison_hash:
                    marker_payload["comparison_config_hash"] = (
                        marker_comparison_hash)
                    print(
                        "[metadata] recovered legacy comparison hash: "
                        f"{path}")
        if expected_jobs_by_id is not None:
            validated_rows = []
            for row in rows:
                job_id = str(row.get("final_job_id", ""))
                row_job = expected_jobs_by_id.get(job_id)
                if row_job is None:
                    print(
                        f"[skip] combined row not in resolved manifest: "
                        f"{path} job={job_id}")
                    continue
                metadata = row_job.get("metadata", {})
                expected_matrix_hash = str(
                    metadata.get("matrix_config_hash", ""))
                actual_matrix_hash = str(
                    row.get("final_matrix_config_hash", ""))
                expected_output = (
                    Path(str(row_job.get("out_dir", ""))) /
                    ("proof_matrix.csv"
                     if row_job.get("kind") == "proof_matrix"
                     else "roi_matrix.csv"))
                actual_output = Path(
                    str(row.get("final_output_csv", "")))
                if (not expected_matrix_hash or
                        actual_matrix_hash != expected_matrix_hash or
                        not actual_output.is_absolute() or
                        actual_output.resolve() != expected_output.resolve()):
                    print(
                        f"[skip] stale combined row: {path} job={job_id}")
                    continue
                comparison_hash = str(
                    row.get("final_comparison_config_hash", ""))
                expected_comparison_hash = str(
                    metadata.get("comparison_config_hash", ""))
                if (comparison_hash and expected_comparison_hash and
                        comparison_hash != expected_comparison_hash):
                    print(
                        f"[skip] stale combined comparison hash: "
                        f"{path} job={job_id}")
                    continue
                if not comparison_hash:
                    comparison_hash = (
                        expected_comparison_hash or
                        recover_roi_comparison_config_hash(row_job))
                    if comparison_hash:
                        row["final_comparison_config_hash"] = (
                            comparison_hash)
                        print(
                            "[metadata] recovered legacy combined "
                            f"comparison hash: {path} job={job_id}")
                validated_rows.append(row)
            rows = validated_rows
        rows = [
            row for row in rows
            if row.get("final_output_status", "ok") == "ok"
        ]
        source_run = run_dir or path.parent
        for row in rows:
            row["pipeline_source_csv"] = str(path)
            row["pipeline_run_dir"] = str(source_run)
            row["pipeline_run_name"] = source_run.name
            row.setdefault("final_shard_group", source_run.name)
            row.setdefault(
                "final_matrix_id", row.get("final_job_id", ""))
            row.setdefault("final_matrix_config_hash", "")
            row.setdefault("final_comparison_config_hash", "")
            row.setdefault(
                "final_scaling_series_id",
                row.get("final_matrix_id", row.get("final_job_id", "")))
            row.setdefault("per_core_l3_size", row.get("l3_size", ""))
            if marker_payload:
                row["final_matrix_id"] = str(
                    marker_payload.get("matrix_id", ""))
                row["final_shard_group"] = str(
                    marker_payload.get("shard_group", source_run.name))
                row["final_expected_policy_labels"] = json.dumps(
                    marker_payload.get("expected_policy_labels", []),
                    separators=(",", ":"))
                row["final_matrix_config_hash"] = str(
                    marker_payload.get(
                        "matrix_config_hash",
                        marker_payload.get("config_hash", "")))
                row["final_comparison_config_hash"] = str(
                    marker_payload.get("comparison_config_hash", ""))
                if (
                        path.name == "roi_matrix.csv" and
                        len(path.parents) > 3 and
                        path.parents[3].name == "matrices"):
                    stage = path.parents[2].name
                    graph = path.parents[1].name
                    benchmark = path.parent.name
                    matrix_id = str(
                        marker_payload.get("matrix_id", ""))
                    if not row.get("final_stage"):
                        row["final_stage"] = stage
                    if not row.get("final_graph"):
                        row["final_graph"] = graph
                    if not row.get("benchmark"):
                        row["benchmark"] = benchmark
                    if not row.get("final_job_id"):
                        row["final_job_id"] = matrix_id
                    if not row.get("final_kind"):
                        row["final_kind"] = "roi_matrix"
                    if not row.get("final_output_csv"):
                        row["final_output_csv"] = str(path.resolve())
                    if not row.get("final_output_status"):
                        row["final_output_status"] = "ok"
                    if not row.get("final_output_detail"):
                        row["final_output_detail"] = (
                            f"{len(rows)} ok rows")
        if kind == "proof":
            proof_rows.extend(rows)
        else:
            roi_rows.extend(rows)

    for run_dir in run_dirs:
        roi_path = run_dir / "combined_roi_matrix.csv"
        proof_path = run_dir / "combined_proof_matrix.csv"
        run_marker = run_dir / "run.complete.json"
        resolved_manifest = run_dir / "resolved_manifest.json"
        try:
            marker_payload = (
                json.loads(run_marker.read_text())
                if run_marker.exists() else {})
            resolved_payload = (
                json.loads(resolved_manifest.read_text())
                if resolved_manifest.exists() else {})
            run_complete = (
                marker_payload.get("complete") is True and
                marker_payload.get("run_config_hash") not in (None, "") and
                marker_payload.get("run_config_hash") ==
                resolved_payload.get("run_config_hash") and
                marker_outputs_valid(marker_payload, run_dir))
        except (OSError, json.JSONDecodeError):
            run_complete = False
            resolved_payload = {}
        jobs_by_out_dir = {
            str(resolve_path(str(job.get("out_dir", ""))).resolve()): job
            for job in resolved_payload.get("jobs", [])
            if job.get("out_dir")
        }
        jobs_by_id = {
            str(job.get("job_id", "")): job
            for job in resolved_payload.get("jobs", [])
            if job.get("job_id")
        }
        manifest_scoped = bool(jobs_by_out_dir)
        if roi_path.exists() and run_complete:
            add_rows(
                roi_path, "roi", run_dir,
                expected_jobs_by_id=jobs_by_id)
        else:
            for path in sorted((run_dir / "matrices").glob("**/roi_matrix.csv")):
                expected_job = jobs_by_out_dir.get(str(path.parent.resolve()))
                if manifest_scoped and expected_job is None:
                    print(f"[skip] matrix not in resolved manifest: {path}")
                    continue
                add_rows(
                    path, "roi", run_dir,
                    expected_job)
        if proof_path.exists() and run_complete:
            add_rows(
                proof_path, "proof", run_dir,
                expected_jobs_by_id=jobs_by_id)
        else:
            for path in sorted((run_dir / "matrices").glob("**/proof_matrix.csv")):
                expected_job = jobs_by_out_dir.get(str(path.parent.resolve()))
                if manifest_scoped and expected_job is None:
                    print(f"[skip] proof matrix not in resolved manifest: {path}")
                    continue
                add_rows(
                    path, "proof", run_dir,
                    expected_job)

    for path in input_csvs:
        kind = "proof" if path.name == "proof_matrix.csv" else "roi"
        add_rows(path, kind, path.parent)

    return complete_matrix_rows(roi_rows), proof_rows


def summarize_roi(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = ("simulator", "benchmark", "prefetcher", "l3_size", "threads", "policy_label")
    numeric_fields = (
        "sim_ticks", "l3_misses", "l3_miss_rate", "ipc",
        "pf_issued", "pf_useful", "popt_charged_l3_misses_plus_matrix_stream",
        "ecg_record_bytes", "edge_stream_bytes_per_edge",
    )
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row.get("status") == "ok":
            groups[tuple(row.get(key, "") for key in keys)].append(row)

    summary: list[dict[str, Any]] = []
    for group_key, group_rows in sorted(groups.items(), key=lambda item: policy_sort_key(item[0][-1])):
        out = {key: value for key, value in zip(keys, group_key)}
        out["policy_short"] = policy_label(str(out.get("policy_label", "")))
        out["rows"] = len(group_rows)
        timing_valid = all(
            timing_valid_for_speedup(row) for row in group_rows)
        out["timing_valid_for_speedup"] = "1" if timing_valid else "0"
        timing_models = sorted({
            timing_model_label(row) for row in group_rows
            if timing_model_label(row)
        })
        correctness_only = bool(timing_models) and all(
            model in {
                "mechanism_probe_exact_request",
                "mechanism_semantic_anchor",
            }
            for model in timing_models
        )
        out["mechanism_only_correctness"] = (
            "1" if correctness_only else "0")
        timing_caveats = sorted({
            str(row.get("timing_caveat") or "").strip()
            for row in group_rows
            if str(row.get("timing_caveat") or "").strip()
        })
        out["timing_model"] = " | ".join(timing_models)
        out["timing_caveat"] = " | ".join(timing_caveats)
        for field in numeric_fields:
            if field in ("sim_ticks", "ipc") and not timing_valid:
                continue
            if (
                    field in (
                        "l3_misses", "l3_miss_rate",
                        "popt_charged_l3_misses_plus_matrix_stream",
                    ) and correctness_only):
                continue
            value = avg(v for v in (as_float(row.get(field)) for row in group_rows) if v is not None)
            if value is not None:
                out[f"avg_{field}"] = value
        summary.append(out)
    return summary


def roi_summary_table_spec(
        relative: list[dict[str, Any]],
        relative_summary: list[dict[str, Any]],
        summary: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[str], str]:
    has_admissible_relative_timing = any(
        as_float(row.get("speedup_vs_lru")) is not None
        for row in relative
    )
    if has_admissible_relative_timing:
        return (
            relative_summary,
            [
                "policy_short", "benchmark", "prefetcher",
                "avg_speedup_vs_lru",
                "avg_l3_miss_reduction_vs_lru_pct",
            ],
            "ECG normalized ROI summary",
        )
    all_timing_admissible = bool(summary) and all(
        str(row.get("timing_valid_for_speedup", "0")) == "1"
        for row in summary
    )
    if all_timing_admissible:
        return (
            summary,
            [
                "policy_short", "benchmark", "prefetcher",
                "avg_sim_ticks", "avg_l3_misses",
            ],
            "ECG ROI policy summary",
        )
    return (
        summary,
        [
            "policy_short", "benchmark", "prefetcher",
            "avg_ecg_record_bytes", "avg_edge_stream_bytes_per_edge",
            "timing_valid_for_speedup",
            "timing_model", "timing_caveat",
        ],
        "ECG mechanism-only ROI summary",
    )


def roi_relative_metrics(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = complete_matrix_rows(rows)
    grouped: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        if (row.get("status") == "ok" and
                row.get("final_output_status", "ok") == "ok"):
            grouped[compare_key(row)][row.get("policy_label", "")] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_policy in grouped.items():
        lru = by_policy.get("LRU")
        if not lru:
            continue
        lru_ticks = as_float(lru.get("sim_ticks"))
        lru_misses = effective_l3_misses(lru)
        for policy, row in sorted(by_policy.items(), key=lambda item: policy_sort_key(item[0])):
            if timing_model_label(row) in {
                    "mechanism_probe_exact_request",
                    "mechanism_semantic_anchor"}:
                continue
            ticks = as_float(row.get("sim_ticks"))
            misses = effective_l3_misses(row)
            record = {key: value for key, value in zip(ROI_COMPARE_KEYS, group_key)}
            record.update({
                "policy_label": policy,
                "policy_short": policy_label(policy),
                "baseline_policy": "LRU",
                "sim_ticks": row.get("sim_ticks", ""),
                "timing_model": timing_model_label(row),
                "timing_valid_for_speedup": timing_valid_label(row),
                "timing_caveat": row.get("timing_caveat", ""),
                "l3_misses": row.get("l3_misses", ""),
                "l3_misses_with_overhead": row.get(
                    "l3_misses_with_overhead", ""),
                "effective_l3_misses": misses if misses is not None else "",
                "popt_overhead_charged": row.get("popt_overhead_charged", ""),
                "popt_charged_l3_misses_plus_matrix_stream": row.get(
                    "popt_charged_l3_misses_plus_matrix_stream", ""
                ),
            })
            if (
                    ticks is not None and lru_ticks and
                    timing_valid_for_speedup(row) and
                    timing_valid_for_speedup(lru)):
                record["speedup_vs_lru"] = lru_ticks / ticks
                record["normalized_ticks_vs_lru"] = ticks / lru_ticks
            if misses is not None and lru_misses:
                record["l3_miss_reduction_vs_lru_pct"] = ((lru_misses - misses) / lru_misses) * 100.0
                record["l3_miss_ratio_vs_lru"] = misses / lru_misses
            out_rows.append(record)
    return out_rows


def is_preliminary_5alg_row(row: dict[str, Any]) -> bool:
    return "preliminary_5alg" in str(row.get("final_matrix_id", ""))


def preliminary_policy_ranks(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    required = {
        "LRU", "SRRIP", "GRASP", "POPT", "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE",
    }
    grouped: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in complete_matrix_rows(rows):
        if (row.get("status") == "ok" and
                row.get("final_output_status", "ok") == "ok" and
                row.get("prefetcher") == "none" and
                is_preliminary_5alg_row(row)):
            grouped[compare_key(row)][str(row.get("policy_label", ""))] = row

    out: list[dict[str, Any]] = []
    for group_key, by_policy in grouped.items():
        if not required.issubset(by_policy):
            continue
        metrics = {
            policy: effective_l3_misses(by_policy[policy])
            for policy in required
        }
        if any(value is None or not math.isfinite(value)
               for value in metrics.values()):
            continue
        ranked = sorted(
            required,
            key=lambda policy: (float(metrics[policy]), policy_sort_key(policy)),
        )
        rank = {policy: index + 1 for index, policy in enumerate(ranked)}
        for policy in sorted(required, key=policy_sort_key):
            row = by_policy[policy]
            metric = float(metrics[policy])
            record = {
                key: value for key, value in zip(ROI_COMPARE_KEYS, group_key)
            }
            record.update({
                "policy_label": policy,
                "policy_short": policy_label(policy),
                "effective_l3_misses": metric,
                "rank_within_simulator": rank[policy],
                "best_policy": ranked[0],
                "record_transport_model": {
                    "cache_sim": "fused_wide_record",
                    "gem5": "explicit_record_delivery",
                    "sniper": "instrumented_record_delivery",
                }.get(str(row.get("simulator", "")), ""),
                "l3_miss_rate": row.get("l3_miss_rate", ""),
                "l3_prop_miss_rate": row.get("l3_prop_miss_rate", ""),
                "l3_prefetch_misses": row.get("l3_prefetch_misses", ""),
                "l3_prefetch_accesses": row.get("l3_prefetch_accesses", ""),
                "dram_read_bytes": row.get("dram_read_bytes", ""),
                "dram_write_bytes": row.get("dram_write_bytes", ""),
                "dram_prefetch_read_bytes": row.get(
                    "dram_prefetch_read_bytes", ""),
                "total_accesses": row.get("total_accesses", ""),
                "instructions": row.get("instructions", ""),
                "l3_exercised": row.get("l3_exercised", ""),
                "timing_model": timing_model_label(row),
                "timing_valid_for_speedup": timing_valid_label(row),
                "timing_caveat": row.get("timing_caveat", ""),
            })
            for baseline in ("LRU", "GRASP", "POPT"):
                baseline_metric = float(metrics[baseline])
                record[
                    f"effective_miss_reduction_vs_{baseline.lower()}_pct"
                ] = ((baseline_metric - metric) / baseline_metric) * 100.0
            out.append(record)
    return out


def preliminary_stride_sensitivity(
        rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = (
        "final_comparison_config_hash",
        "simulator", "final_graph", "benchmark", "l3_size",
        "l1d_size", "l2_size", "l3_ways", "options",
        "threads", "section", "policy_label",
    )
    logical_keys = keys[1:]
    required = {
        "LRU", "SRRIP", "GRASP", "POPT", "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE",
    }
    grouped: dict[
        tuple[Any, ...], dict[str, list[dict[str, Any]]]
    ] = defaultdict(lambda: defaultdict(list))
    hashes_by_logical: dict[
        tuple[Any, ...], dict[str, set[str]]
    ] = defaultdict(lambda: defaultdict(set))
    for row in complete_matrix_rows(rows):
        if (row.get("status") == "ok" and
                row.get("final_output_status", "ok") == "ok" and
                row.get("prefetcher") in ("none", "STRIDE") and
                row.get("policy_label") in required and
                is_preliminary_5alg_row(row)):
            if not row.get("final_comparison_config_hash"):
                raise RuntimeError(
                    "preliminary STRIDE sensitivity lacks comparison hash: "
                    f"matrix={row.get('final_matrix_id', '')}")
            full_key = tuple(row.get(key, "") for key in keys)
            prefetcher = str(row.get("prefetcher"))
            grouped[full_key][prefetcher].append(row)
            hashes_by_logical[
                tuple(row.get(key, "") for key in logical_keys)
            ][prefetcher].add(str(row["final_comparison_config_hash"]))

    for logical_key, by_prefetcher in hashes_by_logical.items():
        none_hashes = by_prefetcher.get("none", set())
        stride_hashes = by_prefetcher.get("STRIDE", set())
        if none_hashes and stride_hashes and none_hashes != stride_hashes:
            raise RuntimeError(
                "preliminary STRIDE sensitivity comparison hash mismatch: "
                f"key={logical_key} none={sorted(none_hashes)} "
                f"stride={sorted(stride_hashes)}")

    def demand_misses(
            row: dict[str, Any]) -> tuple[float | None, str]:
        total = effective_l3_misses(row)
        if total is None:
            return None, "missing_l3_misses"
        if (row.get("simulator") == "sniper" and
                row.get("prefetcher") != "none"):
            prefetch_misses = as_float(row.get("l3_prefetch_misses"))
            if prefetch_misses is None:
                return None, "sniper_lacks_prefetch_miss_split"
            return total - prefetch_misses, ""
        return total, ""

    def traffic(row: dict[str, Any]) -> tuple[float | None, str]:
        dram_read = as_float(row.get("dram_read_bytes"))
        dram_write = as_float(row.get("dram_write_bytes"))
        if dram_read is not None or dram_write is not None:
            return (dram_read or 0.0) + (dram_write or 0.0), "dram_bytes"
        total = as_float(row.get("total_memory_traffic_with_overhead"))
        if total is not None:
            return total, "memory_transactions"
        if row.get("simulator") == "sniper":
            misses = effective_l3_misses(row)
            if misses is not None:
                return misses, "llc_read_misses"
        return None, ""

    out: list[dict[str, Any]] = []
    for key, by_prefetcher in grouped.items():
        baseline_rows = by_prefetcher.get("none", [])
        stride_rows = by_prefetcher.get("STRIDE", [])
        if not baseline_rows or not stride_rows:
            continue
        if len(baseline_rows) != 1 or len(stride_rows) != 1:
            raise RuntimeError(
                "preliminary STRIDE sensitivity has duplicate inputs: "
                f"key={key} none={len(baseline_rows)} "
                f"stride={len(stride_rows)}")
        baseline = baseline_rows[0]
        stride = stride_rows[0]
        baseline_misses = effective_l3_misses(baseline)
        stride_misses = effective_l3_misses(stride)
        baseline_demand_misses, baseline_demand_reason = (
            demand_misses(baseline))
        stride_demand_misses, stride_demand_reason = (
            demand_misses(stride))
        baseline_traffic, traffic_unit = traffic(baseline)
        stride_traffic, stride_unit = traffic(stride)
        record = {name: value for name, value in zip(keys, key)}
        record.update({
            "baseline_prefetcher": "none",
            "sensitivity_prefetcher": "STRIDE",
            "structure_prefetch_degree": stride.get(
                "structure_prefetch_degree", ""),
            "baseline_effective_l3_misses":
                baseline_misses if baseline_misses is not None else "",
            "stride_effective_l3_misses":
                stride_misses if stride_misses is not None else "",
            "baseline_demand_l3_misses":
                baseline_demand_misses
                if baseline_demand_misses is not None else "",
            "stride_demand_l3_misses":
                stride_demand_misses
                if stride_demand_misses is not None else "",
            "demand_miss_metric_available": int(
                baseline_demand_misses is not None and
                stride_demand_misses is not None),
            "demand_miss_unavailable_reason":
                stride_demand_reason or baseline_demand_reason,
            "traffic_unit": traffic_unit if traffic_unit == stride_unit else "",
            "baseline_traffic":
                baseline_traffic if baseline_traffic is not None else "",
            "stride_traffic":
                stride_traffic if stride_traffic is not None else "",
            "timing_valid_for_speedup": int(
                timing_valid_for_speedup(baseline) and
                timing_valid_for_speedup(stride)),
        })
        if (baseline_demand_misses and
                stride_demand_misses is not None):
            record["demand_miss_reduction_pct"] = (
                (baseline_demand_misses - stride_demand_misses) /
                baseline_demand_misses) * 100.0
        if baseline_traffic and stride_traffic is not None:
            record["traffic_change_pct"] = (
                (stride_traffic - baseline_traffic) / baseline_traffic) * 100.0
        baseline_ticks = as_float(baseline.get("sim_ticks"))
        stride_ticks = as_float(stride.get("sim_ticks"))
        if (record["timing_valid_for_speedup"] and
                baseline_ticks and stride_ticks):
            record["speedup_none_to_stride"] = baseline_ticks / stride_ticks
        out.append(record)
    return out


def online_dueling_regret(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    static_policies = (
        "ECG_REUSE_PLAN_GRASP",
        "ECG_REUSE_PLAN_EPOCH",
        "ECG_REUSE_PLAN_RRIP",
        "ECG_REUSE_PLAN_DEGREE",
        "ECG_REUSE_PLAN_LRU",
    )
    grouped: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in complete_matrix_rows(rows):
        if (row.get("status") == "ok" and
                row.get("final_output_status", "ok") == "ok"):
            grouped[compare_key(row)][str(row.get("policy_label", ""))] = row

    out: list[dict[str, Any]] = []
    for group_key, by_policy in grouped.items():
        online = by_policy.get("ECG_REUSE_PLAN_ONLINE")
        static = [
            by_policy[policy] for policy in static_policies
            if policy in by_policy
        ]
        if online is None or len(static) != len(static_policies):
            continue
        demand_counts = set()
        for policy, row in by_policy.items():
            value = as_float(row.get("total_accesses"))
            if value is None or not math.isfinite(value):
                raise RuntimeError(
                    "online-dueling matrix lacks a finite demand count: "
                    f"matrix={group_key[1]} policy={policy}")
            exercised = str(row.get("l3_exercised", "")).lower()
            if exercised not in ("1", "true"):
                raise RuntimeError(
                    "online-dueling matrix has no exercised L3: "
                    f"matrix={group_key[1]} policy={policy}")
            demand_counts.add(value)
        if len(demand_counts) > 1:
            raise RuntimeError(
                "online-dueling matrix changes the demand stream: "
                f"matrix={group_key[1]} total_accesses={sorted(demand_counts)}")

        metric = next(
            (
                field for field in (
                    "l3_misses", "memory_accesses")
                if as_float(online.get(field)) is not None and
                all(as_float(row.get(field)) is not None for row in static)
            ),
            None,
        )
        if metric is None:
            continue
        best = min(
            static,
            key=lambda row: (
                as_float(row.get(metric))
                if as_float(row.get(metric)) is not None
                else math.inf
            ),
        )
        best_value = as_float(best.get(metric))
        online_value = as_float(online.get(metric))
        if not best_value or online_value is None:
            continue
        record = {
            key: value for key, value in zip(ROI_COMPARE_KEYS, group_key)
        }
        record.update({
            "metric": metric,
            "best_static_policy": best.get("policy_label", ""),
            "best_static_value": best_value,
            "online_value": online_value,
            "online_regret_pct": (
                (online_value - best_value) / best_value) * 100.0,
        })
        property_values = [
            (row, as_float(row.get("l3_prop_misses")))
            for row in static
        ]
        online_property = as_float(online.get("l3_prop_misses"))
        if (online_property is not None and all(
                value is not None for _row, value in property_values)):
            best_property_row, best_property = min(
                property_values,
                key=lambda item: (
                    item[1] if item[1] is not None else math.inf),
            )
            if best_property:
                record["best_static_property_policy"] = (
                    best_property_row.get("policy_label", ""))
                record["online_property_regret_pct"] = (
                    (online_property - best_property) /
                    best_property) * 100.0
        for policy in ("POPT_UNCHARGED", "POPT"):
            baseline = by_policy.get(policy)
            value = None
            if baseline:
                value = (
                    effective_l3_misses(baseline)
                    if policy == "POPT" and metric == "l3_misses"
                    else as_float(baseline.get(metric))
                )
            if value:
                record[f"online_delta_vs_{policy.lower()}_pct"] = (
                    (online_value - value) / value) * 100.0
        out.append(record)
    return out


def proof_relative_metrics(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        if (row.get("status") == "ok" and
                row.get("final_output_status", "ok") == "ok"):
            grouped[compare_key(row)][row.get("policy_label", "")] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_policy in grouped.items():
        lru = by_policy.get("LRU")
        if not lru:
            continue
        lru_traffic = as_float(lru.get("total_memory_traffic")) or as_float(lru.get("l3_misses"))
        lru_misses = as_float(lru.get("l3_misses"))
        for policy, row in sorted(by_policy.items(), key=lambda item: policy_sort_key(item[0])):
            traffic = as_float(row.get("total_memory_traffic")) or as_float(row.get("l3_misses"))
            misses = as_float(row.get("l3_misses"))
            record = {key: value for key, value in zip(ROI_COMPARE_KEYS, group_key)}
            record.update({
                "policy_label": policy,
                "policy_short": policy_label(policy),
                "baseline_policy": "LRU",
                "memory_traffic": traffic if traffic is not None else "",
                "l3_misses": misses if misses is not None else "",
            })
            if traffic is not None and lru_traffic:
                record["memory_traffic_reduction_vs_lru_pct"] = ((lru_traffic - traffic) / lru_traffic) * 100.0
                record["memory_traffic_ratio_vs_lru"] = traffic / lru_traffic
            if misses is not None and lru_misses:
                record["l3_miss_reduction_vs_lru_pct"] = ((lru_misses - misses) / lru_misses) * 100.0
                record["l3_miss_ratio_vs_lru"] = misses / lru_misses
            out_rows.append(record)
    return out_rows


def summarize_relative(rows: list[dict[str, Any]], metrics: tuple[str, ...]) -> list[dict[str, Any]]:
    keys = ("simulator", "benchmark", "prefetcher", "l3_size", "threads", "policy_label", "policy_short")
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(key, "") for key in keys)].append(row)

    summary: list[dict[str, Any]] = []
    for group_key, group_rows in sorted(groups.items(), key=lambda item: policy_sort_key(str(item[0][-2]))):
        out = {key: value for key, value in zip(keys, group_key)}
        out["rows"] = len(group_rows)
        for metric in metrics:
            value = avg(v for v in (as_float(row.get(metric)) for row in group_rows) if v is not None)
            if value is not None:
                out[f"avg_{metric}"] = value
        summary.append(out)
    return summary


def charged_overhead(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    pairs = [
        ("POPT", "POPT_UNCHARGED"),
        ("ECG_DBG_PRIMARY_CHARGED", "ECG_DBG_PRIMARY"),
    ]
    keys = (
        "final_shard_group", "final_matrix_id", "simulator", "benchmark",
        "prefetcher", "l3_size", "threads", "section")
    indexed: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        if row.get("status") != "ok":
            continue
        indexed[tuple(row.get(key, "") for key in keys)][row.get("policy_label", "")] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_policy in indexed.items():
        for charged, oracle in pairs:
            if charged not in by_policy or oracle not in by_policy:
                continue
            charged_row = by_policy[charged]
            oracle_row = by_policy[oracle]
            record = {key: value for key, value in zip(keys, group_key)}
            record.update({
                "charged_policy": charged,
                "oracle_policy": oracle,
                "popt_effective_l3_size": charged_row.get("popt_effective_l3_size", ""),
                "popt_effective_l3_ways": charged_row.get("popt_effective_l3_ways", ""),
                "popt_reserved_ways": charged_row.get("popt_reserved_ways", ""),
                "popt_reserved_bytes": charged_row.get("popt_reserved_bytes", ""),
                "popt_matrix_stream_cache_lines": charged_row.get("popt_matrix_stream_cache_lines", ""),
            })
            tick_charged = as_float(charged_row.get("sim_ticks"))
            tick_oracle = as_float(oracle_row.get("sim_ticks"))
            miss_charged = effective_l3_misses(charged_row)
            miss_oracle = effective_l3_misses(oracle_row)
            if tick_charged is not None and tick_oracle:
                record["tick_delta"] = tick_charged - tick_oracle
                record["tick_delta_pct"] = ((tick_charged - tick_oracle) / tick_oracle) * 100.0
            if miss_charged is not None and miss_oracle:
                record["l3_miss_delta"] = miss_charged - miss_oracle
                record["l3_miss_delta_pct"] = ((miss_charged - miss_oracle) / miss_oracle) * 100.0
            out_rows.append(record)
    return out_rows


def faithfulness_summary(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    pairs = [
        ("GRASP parity", "GRASP", "ECG_DBG_ONLY", 3.0),
        ("P-OPT parity", "POPT_UNCHARGED", "ECG_POPT_PRIMARY", 3.0),
    ]
    group_keys = (
        "final_shard_group", "final_matrix_id", "simulator", "benchmark",
        "prefetcher", "l3_size", "threads")
    indexed: dict[tuple[Any, ...], dict[str, dict[str, dict[str, Any]]]] = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        if row.get("status") != "ok":
            continue
        key = tuple(row.get(field, "") for field in group_keys)
        indexed[key][row.get("section", "")][row.get("policy_label", "")] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_section in indexed.items():
        for check_name, reference_policy, candidate_policy, tolerance_pct in pairs:
            tick_deltas: list[float] = []
            tick_delta_pcts: list[float] = []
            miss_deltas: list[float] = []
            miss_delta_pcts: list[float] = []
            sections: list[str] = []
            for section, by_policy in sorted(by_section.items(), key=lambda item: str(item[0])):
                reference = by_policy.get(reference_policy)
                candidate = by_policy.get(candidate_policy)
                if not reference or not candidate:
                    continue
                ref_ticks = as_float(reference.get("sim_ticks"))
                cand_ticks = as_float(candidate.get("sim_ticks"))
                ref_misses = as_float(reference.get("l3_misses"))
                cand_misses = as_float(candidate.get("l3_misses"))
                if ref_ticks is not None and cand_ticks is not None:
                    tick_deltas.append(cand_ticks - ref_ticks)
                    delta_pct = pct_delta(cand_ticks, ref_ticks)
                    if delta_pct is not None:
                        tick_delta_pcts.append(delta_pct)
                if ref_misses is not None and cand_misses is not None:
                    miss_deltas.append(cand_misses - ref_misses)
                    delta_pct = pct_delta(cand_misses, ref_misses)
                    if delta_pct is not None:
                        miss_delta_pcts.append(delta_pct)
                sections.append(section)
            if not sections:
                continue
            max_abs_tick_delta_pct = max((abs(value) for value in tick_delta_pcts), default=0.0)
            max_abs_miss_delta_pct = max((abs(value) for value in miss_delta_pcts), default=0.0)
            record = {field: value for field, value in zip(group_keys, group_key)}
            record.update({
                "check": check_name,
                "reference_policy": reference_policy,
                "candidate_policy": candidate_policy,
                "reference_short": policy_label(reference_policy),
                "candidate_short": policy_label(candidate_policy),
                "matched_sections": len(sections),
                "sections": ",".join(sections),
                "tolerance_pct": tolerance_pct,
                "avg_tick_delta": avg(tick_deltas) if tick_deltas else "",
                "avg_tick_delta_pct": avg(tick_delta_pcts) if tick_delta_pcts else "",
                "max_abs_tick_delta": max((abs(value) for value in tick_deltas), default=""),
                "max_abs_tick_delta_pct": max_abs_tick_delta_pct,
                "avg_l3_miss_delta": avg(miss_deltas) if miss_deltas else "",
                "avg_l3_miss_delta_pct": avg(miss_delta_pcts) if miss_delta_pcts else "",
                "max_abs_l3_miss_delta": max((abs(value) for value in miss_deltas), default=""),
                "max_abs_l3_miss_delta_pct": max_abs_miss_delta_pct,
                "passes_tolerance": "yes"
                if max_abs_tick_delta_pct <= tolerance_pct and max_abs_miss_delta_pct <= tolerance_pct
                else "no",
            })
            out_rows.append(record)
    return out_rows


def popt_storage_overhead_summary(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = ("simulator", "benchmark", "prefetcher", "l3_size", "threads", "policy_label")
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row.get("status") != "ok":
            continue
        if as_float(row.get("popt_reserved_bytes")) or as_float(row.get("popt_matrix_stream_bytes")):
            groups[tuple(row.get(key, "") for key in keys)].append(row)

    fields = (
        "popt_reserved_ways",
        "popt_reserved_bytes",
        "popt_matrix_stream_bytes",
        "popt_matrix_stream_cache_lines",
        "popt_matrix_active_columns",
        "popt_matrix_column_bytes",
    )
    out_rows: list[dict[str, Any]] = []
    for group_key, group_rows in sorted(groups.items(), key=lambda item: policy_sort_key(str(item[0][-1]))):
        record = {key: value for key, value in zip(keys, group_key)}
        record["policy_short"] = policy_label(str(record.get("policy_label", "")))
        record["rows"] = len(group_rows)
        requested_bytes = avg(
            value for value in (parse_size_bytes(row.get("popt_requested_l3_size") or row.get("l3_size")) for row in group_rows)
            if value is not None
        )
        for field in fields:
            value = avg(v for v in (as_float(row.get(field)) for row in group_rows) if v is not None)
            if value is not None:
                record[f"avg_{field}"] = value
        reserved = as_float(record.get("avg_popt_reserved_bytes"))
        if reserved is not None and requested_bytes:
            record["avg_reserved_l3_pct"] = (reserved / requested_bytes) * 100.0
        out_rows.append(record)
    return out_rows


def ecg_mode_overhead_rows() -> list[dict[str, Any]]:
    return [
        {
            "policy_label": "ECG_DBG_ONLY",
            "policy_short": policy_label("ECG_DBG_ONLY"),
            "dynamic_popt_matrix": "no",
            "charged_alias": "no",
            "popt_reserved_ways": 0,
            "role": "GRASP-equivalence / DBG-only low-overhead mode",
        },
        {
            "policy_label": "ECG_DBG_PRIMARY",
            "policy_short": policy_label("ECG_DBG_PRIMARY"),
            "dynamic_popt_matrix": "yes",
            "charged_alias": "no",
            "popt_reserved_ways": 0,
            "role": "Hybrid DBG-first ECG mode, oracle matrix capacity",
        },
        {
            "policy_label": "ECG_DBG_PRIMARY_CHARGED",
            "policy_short": policy_label("ECG_DBG_PRIMARY_CHARGED"),
            "dynamic_popt_matrix": "yes",
            "charged_alias": "yes",
            "popt_reserved_ways": "from run geometry",
            "role": "Hybrid DBG-first ECG mode with P-OPT overhead charged",
        },
        {
            "policy_label": "ECG_POPT_PRIMARY",
            "policy_short": policy_label("ECG_POPT_PRIMARY"),
            "dynamic_popt_matrix": "yes",
            "charged_alias": "no",
            "popt_reserved_ways": 0,
            "role": "P-OPT-equivalence / oracle-validation mode",
        },
    ]


def prefetch_quality_summary(roi_rows: list[dict[str, Any]], proof_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    out_rows: list[dict[str, Any]] = []

    roi_keys = ("simulator", "benchmark", "prefetcher", "l3_size", "threads", "policy_label")
    roi_groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in roi_rows:
        issued = as_float(row.get("pf_issued"))
        if row.get("status") == "ok" and issued is not None and issued > 0.0:
            roi_groups[tuple(row.get(key, "") for key in roi_keys)].append(row)
    for group_key, group_rows in sorted(roi_groups.items(), key=lambda item: policy_sort_key(str(item[0][-1]))):
        issued = avg(v for v in (as_float(row.get("pf_issued")) for row in group_rows) if v is not None)
        useful = avg(v for v in (as_float(row.get("pf_useful")) for row in group_rows) if v is not None)
        late = avg(v for v in (as_float(row.get("pf_late")) for row in group_rows) if v is not None)
        unused = avg(v for v in (as_float(row.get("pf_unused")) for row in group_rows) if v is not None)
        record = {key: value for key, value in zip(roi_keys, group_key)}
        record.update({
            "source": str(record.get("simulator") or "roi"),
            "policy_short": policy_label(str(record.get("policy_label", ""))),
            "rows": len(group_rows),
            "avg_prefetch_issued": issued if issued is not None else "",
            "avg_prefetch_useful": useful if useful is not None else "",
            "avg_prefetch_late": late if late is not None else "",
            "avg_prefetch_unused": unused if unused is not None else "",
        })
        accuracy = safe_ratio(useful, issued)
        late_rate = safe_ratio(late, issued)
        unused_rate = safe_ratio(unused, issued)
        if accuracy is not None:
            record["prefetch_accuracy_pct"] = accuracy * 100.0
        if late_rate is not None:
            record["prefetch_late_pct"] = late_rate * 100.0
        if unused_rate is not None:
            record["prefetch_unused_pct"] = unused_rate * 100.0
        out_rows.append(record)

    proof_keys = ("simulator", "benchmark", "prefetcher", "l3_size", "policy_label", "ablation")
    proof_groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in proof_rows:
        requests = as_float(row.get("prefetch_requests"))
        fills = as_float(row.get("prefetch_fills"))
        runtime_issued = as_float(row.get("ecg_runtime_issued"))
        has_prefetch = any(value and value > 0.0 for value in (requests, fills, runtime_issued))
        if row.get("status") == "ok" and has_prefetch:
            proof_groups[tuple(row.get(key, "") for key in proof_keys)].append(row)
    for group_key, group_rows in sorted(proof_groups.items(), key=lambda item: policy_sort_key(str(item[0][-2]))):
        requests = avg(v for v in (as_float(row.get("prefetch_requests")) for row in group_rows) if v is not None)
        fills = avg(v for v in (as_float(row.get("prefetch_fills")) for row in group_rows) if v is not None)
        useful = avg(v for v in (as_float(row.get("prefetch_useful")) for row in group_rows) if v is not None)
        evicted = avg(v for v in (as_float(row.get("prefetch_evicted_before_use")) for row in group_rows) if v is not None)
        traffic = avg(v for v in (as_float(row.get("total_memory_traffic")) for row in group_rows) if v is not None)
        demand = avg(v for v in (as_float(row.get("memory_accesses")) for row in group_rows) if v is not None)
        record = {key: value for key, value in zip(proof_keys, group_key)}
        record.update({
            "source": "cache_sim",
            "policy_short": policy_label(str(record.get("policy_label", ""))),
            "rows": len(group_rows),
            "avg_prefetch_requests": requests if requests is not None else "",
            "avg_prefetch_fills": fills if fills is not None else "",
            "avg_prefetch_useful": useful if useful is not None else "",
            "avg_prefetch_evicted_before_use": evicted if evicted is not None else "",
            "avg_total_memory_traffic": traffic if traffic is not None else "",
            "avg_demand_memory_accesses": demand if demand is not None else "",
        })
        request_accuracy = safe_ratio(useful, requests)
        fill_useful = safe_ratio(useful, fills)
        traffic_ratio = safe_ratio(traffic, demand)
        if request_accuracy is not None:
            record["prefetch_request_useful_pct"] = request_accuracy * 100.0
        if fill_useful is not None:
            record["prefetch_fill_useful_pct"] = fill_useful * 100.0
        if traffic_ratio is not None:
            record["traffic_per_demand_access"] = traffic_ratio
        out_rows.append(record)

    return out_rows


def thread_scaling_metrics(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = (
        "final_shard_group", "final_scaling_series_id", "simulator",
        "benchmark", "prefetcher", "per_core_l3_size", "policy_label",
        "section")
    groups: dict[tuple[Any, ...], dict[int, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        threads = thread_count(row)
        ticks = as_float(row.get("sim_ticks"))
        if row.get("status") == "ok" and threads is not None and ticks is not None:
            groups[tuple(row.get(key, "") for key in keys)][threads] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_thread in sorted(groups.items(), key=lambda item: policy_sort_key(str(item[0][6]))):
        if 1 not in by_thread or len(by_thread) < 2:
            continue
        baseline_ticks = as_float(by_thread[1].get("sim_ticks"))
        if not baseline_ticks:
            continue
        for threads, row in sorted(by_thread.items()):
            ticks = as_float(row.get("sim_ticks"))
            record = {key: value for key, value in zip(keys, group_key)}
            record.update({
                "threads": threads,
                "policy_short": policy_label(str(record.get("policy_label", ""))),
                "sim_ticks": row.get("sim_ticks", ""),
                "ipc": row.get("ipc", ""),
                "l3_misses": row.get("l3_misses", ""),
                "l3_miss_rate": row.get("l3_miss_rate", ""),
            })
            if ticks:
                speedup = baseline_ticks / ticks
                record["thread_speedup_vs_1t"] = speedup
                record["normalized_ticks_vs_1t"] = ticks / baseline_ticks
                record["parallel_efficiency_vs_1t"] = speedup / threads
            out_rows.append(record)
    return out_rows


def backend_direction_agreement(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = ("benchmark", "prefetcher", "l3_size", "threads", "policy_label", "section")
    metrics = (
        ("speedup_vs_lru", 1.0),
        ("l3_miss_reduction_vs_lru_pct", 0.0),
    )
    groups: dict[tuple[Any, ...], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        simulator = str(row.get("simulator", ""))
        if simulator:
            groups[tuple(row.get(key, "") for key in keys)][simulator] = row

    out_rows: list[dict[str, Any]] = []
    for group_key, by_simulator in sorted(groups.items(), key=lambda item: policy_sort_key(str(item[0][4]))):
        simulators = sorted(by_simulator)
        if len(simulators) < 2:
            continue
        for metric, reference in metrics:
            for left_index, left in enumerate(simulators):
                for right in simulators[left_index + 1:]:
                    left_value = as_float(by_simulator[left].get(metric))
                    right_value = as_float(by_simulator[right].get(metric))
                    left_direction = metric_direction(left_value, reference)
                    right_direction = metric_direction(right_value, reference)
                    record = {key: value for key, value in zip(keys, group_key)}
                    record.update({
                        "metric": metric,
                        "simulator_a": left,
                        "simulator_b": right,
                        "value_a": left_value if left_value is not None else "",
                        "value_b": right_value if right_value is not None else "",
                        "direction_a": left_direction,
                        "direction_b": right_direction,
                        "direction_agrees": "yes" if left_direction == right_direction else "no",
                    })
                    out_rows.append(record)
    return out_rows


def sniper_cpi_stack_summary(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = ("benchmark", "prefetcher", "l3_size", "threads", "policy_label")
    fields = (
        "sniper_cpi_base",
        "sniper_cpi_branch",
        "sniper_cpi_data_cache",
        "sniper_cpi_data_l1",
        "sniper_cpi_data_l2",
        "sniper_cpi_data_llc",
        "sniper_cpi_data_dram",
        "sniper_cpi_sync",
        "sniper_cpi_unknown",
    )
    stack_total_fields = {
        "sniper_cpi_base",
        "sniper_cpi_branch",
        "sniper_cpi_data_cache",
        "sniper_cpi_sync",
        "sniper_cpi_unknown",
    }
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row.get("status") == "ok" and row.get("simulator") == "sniper":
            if any(as_float(row.get(field)) is not None for field in fields):
                groups[tuple(row.get(key, "") for key in keys)].append(row)

    out_rows: list[dict[str, Any]] = []
    for group_key, group_rows in sorted(groups.items(), key=lambda item: policy_sort_key(str(item[0][-1]))):
        record = {key: value for key, value in zip(keys, group_key)}
        record["policy_short"] = policy_label(str(record.get("policy_label", "")))
        record["rows"] = len(group_rows)
        total = 0.0
        for field in fields:
            value = avg(v for v in (as_float(row.get(field)) for row in group_rows) if v is not None)
            if value is not None:
                record[f"avg_{field}"] = value
                if field in stack_total_fields:
                    total += value
        record["avg_sniper_cpi_stack_total"] = total
        if total > 0.0:
            for field in fields:
                value = as_float(record.get(f"avg_{field}"))
                if value is not None:
                    record[f"avg_{field}_pct"] = (value / total) * 100.0
        out_rows.append(record)
    return out_rows


def write_latex_table(path: Path, rows: list[dict[str, Any]], fields: list[str], caption: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as fh:
        fh.write("% Auto-generated by scripts/experiments/ecg/flows/aggregate_results.py\n")
        fh.write("\\begin{table}[t]\n\\centering\n")
        fh.write(f"\\caption{{{caption}}}\n")
        fh.write("\\begin{tabular}{" + "l" * len(fields) + "}\n")
        fh.write("\\toprule\n")
        headers = [TABLE_HEADER_LABELS.get(field, field.replace("_", "\\_")) for field in fields]
        fh.write(" & ".join(headers) + " \\\\\n")
        fh.write("\\midrule\n")
        for row in rows:
            values = []
            for field in fields:
                value = row.get(field, "")
                if isinstance(value, float):
                    value = f"{value:.3f}"
                values.append(str(value).replace("_", "\\_"))
            fh.write(" & ".join(values) + " \\\\\n")
        fh.write("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
    print(f"[write] {path}")


def set_plot_style() -> None:
    plt.rcParams.update({
        "font.size": 7,
        "axes.labelsize": 7,
        "axes.titlesize": 8,
        "xtick.labelsize": 6.5,
        "ytick.labelsize": 6.5,
        "legend.fontsize": 6.5,
        "svg.fonttype": "none",
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })


def save_figure(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(path, bbox_inches="tight")
    print(f"[write] {path}")
    if path.suffix.lower() == ".svg":
        png_path = path.with_suffix(".png")
        plt.savefig(png_path, dpi=FIGURE_DPI, bbox_inches="tight")
        print(f"[write] {png_path}")


def plot_metric_by_policy(
    path: Path,
    rows: list[dict[str, Any]],
    metric: str,
    xlabel: str,
    value_format: str,
    reference: float,
) -> None:
    if not HAS_MATPLOTLIB:
        print(f"[skip] matplotlib not available; not writing {path}")
        return
    values_by_policy: dict[str, list[float]] = defaultdict(list)
    for row in rows:
        value = as_float(row.get(metric))
        if value is not None:
            values_by_policy[row.get("policy_label", "")].append(value)
    if not values_by_policy:
        print(f"[skip] no data for {metric}")
        return
    policies = sorted(values_by_policy, key=policy_sort_key)
    values = [avg(values_by_policy[policy]) or 0.0 for policy in policies]
    labels = [policy_label(policy) for policy in policies]

    set_plot_style()
    fig_height = max(1.75, 0.23 * len(policies) + 0.65)
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, fig_height))
    bars = ax.barh(
        range(len(policies)),
        values,
        height=0.48,
        color=[POLICY_COLORS.get(policy, "#6B6B6B") for policy in policies],
        edgecolor="black",
        linewidth=0.55,
    )
    for bar, policy in zip(bars, policies):
        hatch = POLICY_HATCHES.get(policy)
        if hatch:
            bar.set_hatch(hatch)

    ax.set_yticks(range(len(policies)), labels)
    ax.invert_yaxis()
    ax.set_xlabel(xlabel)
    ax.grid(axis="x", alpha=0.25, linewidth=0.5)
    ax.axvline(reference, color="black", linewidth=0.7, linestyle="--")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    min_value = min(values + [reference])
    max_value = max(values + [reference])
    if reference == 1.0:
        left = min(0.85, min_value * 0.96)
        right = max(1.12, max_value * 1.08)
    else:
        span = max(1.0, max_value - min_value)
        left = min(0.0, min_value) - 0.12 * span
        right = max(0.0, max_value) + 0.18 * span
    ax.set_xlim(left, right)
    label_offset = (right - left) * 0.018
    for y_pos, value in enumerate(values):
        if reference == 0.0 and value < 0.0:
            x_pos = value + label_offset
            ha = "left"
        elif value >= reference:
            x_pos = value + label_offset
            ha = "left"
        else:
            x_pos = value - label_offset
            ha = "right"
        ax.text(x_pos, y_pos, value_format.format(value), va="center", ha=ha, fontsize=6.2)
    fig.tight_layout(pad=0.35)
    save_figure(path)
    plt.close()


def plot_grouped_metric_by_benchmark(
    path: Path,
    rows: list[dict[str, Any]],
    metric: str,
    ylabel: str,
    reference: float,
    summary_label: str,
    summary_mode: str,
) -> None:
    if not HAS_MATPLOTLIB:
        print(f"[skip] matplotlib not available; not writing {path}")
        return

    values_by_benchmark_policy: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    for row in rows:
        value = as_float(row.get(metric))
        benchmark = str(row.get("benchmark", ""))
        policy = str(row.get("policy_label", ""))
        if value is not None and benchmark and policy:
            values_by_benchmark_policy[benchmark][policy].append(value)
    if not values_by_benchmark_policy:
        print(f"[skip] no data for {metric}")
        return

    benchmarks = sorted(values_by_benchmark_policy, key=benchmark_sort_key)
    policies = sorted(
        {policy for by_policy in values_by_benchmark_policy.values() for policy in by_policy},
        key=policy_sort_key,
    )
    bench_policy_avg: dict[str, dict[str, float]] = defaultdict(dict)
    for benchmark in benchmarks:
        for policy in policies:
            value = avg(values_by_benchmark_policy[benchmark].get(policy, []))
            if value is not None:
                bench_policy_avg[benchmark][policy] = value

    labels = [benchmark_label(benchmark) for benchmark in benchmarks] + [summary_label]
    x_positions = list(range(len(labels)))
    policy_count = max(1, len(policies))
    group_width = 0.82
    bar_width = group_width / policy_count

    set_plot_style()
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, 2.15))
    for policy_index, policy in enumerate(policies):
        offset = (policy_index - (policy_count - 1) / 2.0) * bar_width
        values: list[float] = []
        for benchmark in benchmarks:
            values.append(bench_policy_avg.get(benchmark, {}).get(policy, 0.0))
        if summary_mode == "geomean":
            summary = geo_mean(value for value in values if value > 0.0)
        else:
            summary = avg(values)
        values.append(summary if summary is not None else 0.0)
        bars = ax.bar(
            [x + offset for x in x_positions],
            values,
            width=bar_width * 0.92,
            label=policy_label(policy),
            color=POLICY_COLORS.get(policy, "#6B6B6B"),
            edgecolor="black",
            linewidth=0.42,
        )
        hatch = POLICY_HATCHES.get(policy)
        if hatch:
            for bar in bars:
                bar.set_hatch(hatch)

    ax.set_xticks(x_positions, labels)
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", alpha=0.25, linewidth=0.5)
    ax.axhline(reference, color="black", linewidth=0.7, linestyle="--")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(
        frameon=False,
        loc="lower center",
        bbox_to_anchor=(0.5, 1.02),
        ncol=3,
        columnspacing=0.9,
        handlelength=1.3,
    )
    all_values = [
        value
        for benchmark in benchmarks
        for policy in policies
        for value in [bench_policy_avg.get(benchmark, {}).get(policy)]
        if value is not None
    ]
    if summary_mode == "geomean":
        lower = min(0.85, min(all_values + [reference]) * 0.97)
        upper = max(1.12, max(all_values + [reference]) * 1.05)
    else:
        lower = min(0.0, min(all_values + [reference]))
        upper = max(0.0, max(all_values + [reference]))
        span = max(1.0, upper - lower)
        lower -= span * 0.10
        upper += span * 0.12
    ax.set_ylim(lower, upper)
    fig.tight_layout(pad=0.35)
    save_figure(path)
    plt.close()


def plot_charged_overhead(path: Path, rows: list[dict[str, Any]]) -> None:
    if not HAS_MATPLOTLIB:
        print(f"[skip] matplotlib not available; not writing {path}")
        return
    tick_values: dict[tuple[str, str], list[float]] = defaultdict(list)
    miss_values: dict[tuple[str, str], list[float]] = defaultdict(list)
    for row in rows:
        pair = (row.get("charged_policy", ""), row.get("oracle_policy", ""))
        tick = as_float(row.get("tick_delta_pct"))
        miss = as_float(row.get("l3_miss_delta_pct"))
        if tick is not None:
            tick_values[pair].append(tick)
        if miss is not None:
            miss_values[pair].append(miss)
    pairs = sorted(set(tick_values) | set(miss_values), key=lambda pair: policy_sort_key(pair[0]))
    if not pairs:
        print("[skip] no charged overhead data")
        return

    labels = [f"{policy_label(charged)} / {policy_label(oracle)}" for charged, oracle in pairs]
    tick_avgs = [avg(tick_values[pair]) or 0.0 for pair in pairs]
    miss_avgs = [avg(miss_values[pair]) or 0.0 for pair in pairs]
    y_positions = list(range(len(pairs)))

    set_plot_style()
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, max(1.35, 0.34 * len(pairs) + 0.8)))
    height = 0.28
    ax.barh(
        [pos - height / 2 for pos in y_positions],
        tick_avgs,
        height=height,
        label="ticks",
        color="#F58518",
        edgecolor="black",
        linewidth=0.55,
    )
    ax.barh(
        [pos + height / 2 for pos in y_positions],
        miss_avgs,
        height=height,
        label="LLC misses",
        color="#4C78A8",
        edgecolor="black",
        linewidth=0.55,
    )
    ax.set_yticks(y_positions, labels)
    ax.invert_yaxis()
    ax.set_xlabel("Overhead vs oracle (%)")
    ax.grid(axis="x", alpha=0.25, linewidth=0.5)
    ax.axvline(0.0, color="black", linewidth=0.7)
    ax.legend(frameon=False, loc="lower right", bbox_to_anchor=(1.0, 1.02), ncol=2)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    max_value = max(tick_avgs + miss_avgs + [0.0])
    ax.set_xlim(0.0, max(1.0, max_value * 1.22))
    label_offset = ax.get_xlim()[1] * 0.018
    for y_pos, value in zip([pos - height / 2 for pos in y_positions], tick_avgs):
        ax.text(value + label_offset, y_pos, f"{value:.1f}%", va="center", ha="left", fontsize=6.2)
    for y_pos, value in zip([pos + height / 2 for pos in y_positions], miss_avgs):
        ax.text(value + label_offset, y_pos, f"{value:.1f}%", va="center", ha="left", fontsize=6.2)
    fig.tight_layout(pad=0.35)
    save_figure(path)
    plt.close()


def plot_sniper_thread_scaling(path: Path, rows: list[dict[str, Any]]) -> None:
    if not HAS_MATPLOTLIB:
        print(f"[skip] matplotlib not available; not writing {path}")
        return

    values: dict[tuple[str, str], dict[int, float]] = defaultdict(dict)
    for row in rows:
        if row.get("simulator") != "sniper":
            continue
        threads = thread_count(row)
        speedup = as_float(row.get("thread_speedup_vs_1t"))
        benchmark = str(row.get("benchmark", ""))
        policy = str(row.get("policy_label", ""))
        if threads is not None and speedup is not None and benchmark and policy:
            values[(benchmark, policy)][threads] = speedup
    if not values:
        print("[skip] no Sniper thread-scaling data")
        return

    set_plot_style()
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, 2.2))
    for benchmark, policy in sorted(values, key=lambda item: (benchmark_sort_key(item[0]), policy_sort_key(item[1]))):
        by_thread = values[(benchmark, policy)]
        x_values = sorted(by_thread)
        y_values = [by_thread[threads] for threads in x_values]
        ax.plot(
            x_values,
            y_values,
            marker="o",
            linewidth=1.0,
            markersize=3.0,
            color=POLICY_COLORS.get(policy, "#6B6B6B"),
            label=f"{benchmark_label(benchmark)} {policy_label(policy)}",
        )
    ax.set_xlabel("Threads")
    ax.set_ylabel("Speedup vs 1T")
    ax.grid(alpha=0.25, linewidth=0.5)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, 1.02), ncol=2, columnspacing=0.9)
    fig.tight_layout(pad=0.35)
    save_figure(path)
    plt.close()


def _graph_label(graph: str) -> str:
    return str(graph).replace("_", "-")


_GRAPH_OPTION_RE = re.compile(r"results/graphs/([^/\s]+)/")


def _graph_from_row(row: dict[str, Any]) -> str:
    """Best-effort graph-name extraction for cache_sim/gem5/sniper rows.

    Priority order:
    1. An explicit ``graph`` field if the row already carries one.
    2. The ``-f results/graphs/<name>/<name>.sg`` segment in ``options``.
    3. The ``pipeline_run_dir`` / ``pipeline_run_name`` set by collect_csvs
       (matrices/<stage>/<graph>/<app>/roi_matrix.csv layout).
    """
    explicit = str(row.get("graph") or "").strip()
    if explicit:
        return explicit
    options = str(row.get("options") or "")
    match = _GRAPH_OPTION_RE.search(options)
    if match:
        return match.group(1)
    src = str(row.get("pipeline_source_csv") or "")
    if "/matrices/" in src:
        parts = src.split("/matrices/", 1)[1].split("/")
        if len(parts) >= 3:
            return parts[1]
    return ""


def l_curve_rows(roi_rows: list[dict[str, Any]]) -> dict[tuple[str, str], list[dict[str, Any]]]:
    """Group cache_sim rows by (graph, app) for L-curve plotting.

    Only cache_sim rows with a parseable L3 size, a non-empty policy, and a
    finite l3_miss_rate are eligible. Returns groups with at least three
    distinct L3 sizes so the L-shape is meaningful.
    """
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in roi_rows:
        if row.get("simulator") != "cache_sim":
            continue
        # Skip degenerate cells the pressure guard flagged as inert: when the
        # property working set fits in the inner caches the L3 sees only the
        # cold-miss stream (l3_miss_rate == 1.0 for EVERY policy), which carries
        # no policy signal and would skew the averaged L-curve. annotate_l3_pressure
        # sets l3_exercised=False on these (roi_matrix.py). Only drop explicit
        # False so older rows without the flag are still included.
        if row.get("l3_exercised") is False:
            continue
        graph = _graph_from_row(row)
        app = str(row.get("benchmark") or "").strip()
        policy = str(row.get("policy_label") or row.get("policy") or "").strip()
        l3_bytes = parse_size_bytes(row.get("l3_size"))
        miss = as_float(row.get("l3_miss_rate"))
        if not graph or not app or not policy or l3_bytes is None or miss is None:
            continue
        grouped[(graph, app)].append(
            {
                "graph": graph,
                "app": app,
                "policy_label": policy,
                "l3_size": str(row.get("l3_size") or ""),
                "l3_size_bytes": l3_bytes,
                "l3_miss_rate": miss,
            }
        )
    return {
        key: rows
        for key, rows in grouped.items()
        if len({r["l3_size_bytes"] for r in rows}) >= 3
    }


def _l_curve_summary_rows(groups: dict[tuple[str, str], list[dict[str, Any]]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for (graph, app), entries in sorted(groups.items(), key=lambda item: (item[0][0], benchmark_sort_key(item[0][1]))):
        averaged: dict[tuple[str, float, str], list[float]] = defaultdict(list)
        for entry in entries:
            key = (entry["policy_label"], entry["l3_size_bytes"], entry["l3_size"])
            averaged[key].append(entry["l3_miss_rate"])
        for (policy, l3_bytes, l3_size), values in sorted(
            averaged.items(), key=lambda item: (policy_sort_key(item[0][0]), item[0][1])
        ):
            rows.append(
                {
                    "graph": graph,
                    "benchmark": app,
                    "policy_label": policy,
                    "policy_figure_label": policy_label(policy),
                    "l3_size": l3_size,
                    "l3_size_bytes": int(l3_bytes),
                    "l3_miss_rate": sum(values) / len(values),
                    "samples": len(values),
                }
            )
    return rows


def plot_l_curve(path: Path, group_key: tuple[str, str], entries: list[dict[str, Any]]) -> None:
    """Plot the GRASP L-curve for a single graph/application pair."""
    if not HAS_MATPLOTLIB:
        print(f"[skip] matplotlib not available; not writing {path}")
        return

    graph, app = group_key
    by_policy: dict[str, dict[float, list[float]]] = defaultdict(lambda: defaultdict(list))
    by_policy_label_size: dict[float, str] = {}
    for entry in entries:
        by_policy[entry["policy_label"]][entry["l3_size_bytes"]].append(entry["l3_miss_rate"])
        by_policy_label_size[entry["l3_size_bytes"]] = entry["l3_size"]
    if not by_policy:
        print(f"[skip] no L-curve data for {graph}/{app}")
        return

    policies = sorted(by_policy, key=policy_sort_key)
    all_sizes = sorted(by_policy_label_size)

    set_plot_style()
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, 2.2))
    for policy in policies:
        by_size = by_policy[policy]
        x_vals = [s for s in all_sizes if s in by_size]
        y_vals = [sum(by_size[s]) / len(by_size[s]) * 100.0 for s in x_vals]
        if not x_vals:
            continue
        ax.plot(
            x_vals,
            y_vals,
            marker="o",
            linewidth=1.0,
            markersize=3.0,
            color=POLICY_COLORS.get(policy, "#6B6B6B"),
            label=policy_label(policy),
        )
    ax.set_xscale("log", base=2)
    ax.set_xticks(all_sizes)
    ax.set_xticklabels([by_policy_label_size[s] for s in all_sizes], rotation=0, fontsize=6.2)
    ax.set_xlabel("L3 cache size")
    ax.set_ylabel("L3 miss rate (%)")
    ax.set_title(f"{_graph_label(graph)} / {benchmark_label(app)}", fontsize=7.5)
    ax.grid(alpha=0.25, linewidth=0.5)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(frameon=False, loc="upper right", fontsize=6.2)
    fig.tight_layout(pad=0.35)
    save_figure(path)
    plt.close()


def generate_outputs(
        out_dir: Path, roi_rows: list[dict[str, Any]],
        proof_rows: list[dict[str, Any]],
        input_run_dirs: list[Path] | None = None,
        input_csvs: list[Path] | None = None) -> None:
    aggregate_dir = out_dir / "aggregate"
    figures_dir = out_dir / "figures"
    tables_dir = out_dir / "tables"

    for legacy_name in LEGACY_FIGURES:
        legacy_path = figures_dir / legacy_name
        if legacy_path.exists():
            legacy_path.unlink()
            print(f"[remove] {legacy_path}")
    for name in STALE_SPEEDUP_FIGURES:
        stale_path = figures_dir / name
        if stale_path.exists():
            stale_path.unlink()
            print(f"[remove] {stale_path}")
    for directory, name in STALE_SNIPER_TIMING_OUTPUTS:
        stale_path = out_dir / directory / name
        if stale_path.exists():
            stale_path.unlink()
            print(f"[remove] {stale_path}")

    policy_map = policy_label_rows(row.get("policy_label", "") for row in roi_rows + proof_rows)
    if policy_map:
        write_csv(aggregate_dir / "policy_label_map.csv", policy_map)

    if roi_rows:
        write_csv(aggregate_dir / "roi_matrix_all.csv", roi_rows)
        summary = summarize_roi(roi_rows)
        write_csv(aggregate_dir / "roi_policy_summary.csv", summary)
        faithfulness = faithfulness_summary(roi_rows)
        if faithfulness:
            write_csv(aggregate_dir / "faithfulness_summary.csv", faithfulness)
            write_latex_table(
                tables_dir / "faithfulness_summary.tex",
                faithfulness[:24],
                [
                    "check", "benchmark", "prefetcher", "reference_short", "candidate_short",
                    "max_abs_tick_delta_pct", "max_abs_l3_miss_delta_pct", "passes_tolerance",
                ],
                "ECG faithfulness and parity checks",
            )
        storage_overhead = popt_storage_overhead_summary(roi_rows)
        if storage_overhead:
            write_csv(aggregate_dir / "popt_storage_overhead_summary.csv", storage_overhead)
            write_latex_table(
                tables_dir / "popt_storage_overhead_summary.tex",
                storage_overhead[:24],
                [
                    "policy_short", "benchmark", "prefetcher", "avg_popt_reserved_ways",
                    "avg_popt_reserved_bytes", "avg_reserved_l3_pct", "avg_popt_matrix_stream_cache_lines",
                ],
                "P-OPT storage and stream overhead summary",
            )
        sniper_cpi = sniper_cpi_stack_summary(roi_rows)
        if sniper_cpi:
            write_csv(aggregate_dir / "sniper_cpi_stack_summary.csv", sniper_cpi)
            write_latex_table(
                tables_dir / "sniper_cpi_stack_summary.tex",
                sniper_cpi[:24],
                [
                    "policy_short", "benchmark", "prefetcher", "threads",
                    "avg_sniper_cpi_data_cache_pct", "avg_sniper_cpi_data_llc_pct", "avg_sniper_cpi_data_dram_pct",
                ],
                "Sniper CPI/cache-stack summary",
            )
        mode_overhead = ecg_mode_overhead_rows()
        write_csv(aggregate_dir / "ecg_mode_overhead_summary.csv", mode_overhead)
        write_latex_table(
            tables_dir / "ecg_mode_overhead_summary.tex",
            mode_overhead,
            ["policy_short", "dynamic_popt_matrix", "charged_alias", "popt_reserved_ways"],
            "ECG mode overhead summary",
        )
        relative = roi_relative_metrics(roi_rows)
        if relative:
            write_csv(aggregate_dir / "roi_relative_metrics.csv", relative)
            relative_summary = summarize_relative(
                relative,
                (
                    "speedup_vs_lru",
                    "normalized_ticks_vs_lru",
                    "l3_miss_reduction_vs_lru_pct",
                    "l3_miss_ratio_vs_lru",
                ),
            )
            write_csv(aggregate_dir / "roi_relative_policy_summary.csv", relative_summary)
            direction_agreement = backend_direction_agreement(relative)
            if direction_agreement:
                write_csv(aggregate_dir / "backend_direction_agreement.csv", direction_agreement)
            sniper_relative = [row for row in relative if row.get("simulator") == "sniper"]
            if sniper_relative:
                write_csv(aggregate_dir / "sniper_relative_metrics.csv", sniper_relative)
                write_csv(
                    aggregate_dir / "sniper_relative_policy_summary.csv",
                    summarize_relative(
                        sniper_relative,
                        (
                            "speedup_vs_lru",
                            "normalized_ticks_vs_lru",
                            "l3_miss_reduction_vs_lru_pct",
                            "l3_miss_ratio_vs_lru",
                        ),
                    ),
                )
        preliminary_ranks = preliminary_policy_ranks(roi_rows)
        if preliminary_ranks:
            write_csv(
                aggregate_dir / "preliminary_5alg_policy_ranks.csv",
                preliminary_ranks)
        stride_sensitivity = preliminary_stride_sensitivity(roi_rows)
        if stride_sensitivity:
            write_csv(
                aggregate_dir / "preliminary_5alg_stride_sensitivity.csv",
                stride_sensitivity)
        regret_csv = aggregate_dir / "online_dueling_regret.csv"
        regret_tex = tables_dir / "online_dueling_regret.tex"
        for stale in (regret_csv, regret_tex):
            if stale.exists():
                stale.unlink()
                print(f"[remove] {stale}")
        dueling_regret = online_dueling_regret(roi_rows)
        if dueling_regret:
            write_csv(regret_csv, dueling_regret)
            write_latex_table(
                regret_tex,
                dueling_regret[:24],
                [
                    "final_matrix_id", "simulator", "benchmark", "l3_size",
                    "metric",
                    "best_static_policy", "online_regret_pct",
                    "best_static_property_policy",
                    "online_property_regret_pct",
                    "online_delta_vs_popt_uncharged_pct",
                    "online_delta_vs_popt_pct",
                ],
                "Online ReusePlan set-dueling regret versus the best static arm",
            )
        overhead = charged_overhead(roi_rows)
        if overhead:
            write_csv(aggregate_dir / "popt_charged_overhead.csv", overhead)
            write_latex_table(
                tables_dir / "popt_charged_overhead.tex",
                overhead[:20],
                ["charged_policy", "oracle_policy", "benchmark", "section", "tick_delta_pct", "l3_miss_delta_pct"],
                "P-OPT charged overhead summary",
            )
        table_rows, table_columns, table_title = roi_summary_table_spec(
            relative,
            relative_summary if relative else [],
            summary,
        )
        write_latex_table(
            tables_dir / "roi_policy_summary.tex",
            table_rows,
            table_columns,
            table_title,
        )
        if relative:
            replacement_relative = [row for row in relative if row.get("prefetcher") in ("", "none")]
            prefetch_relative = [row for row in relative if row.get("prefetcher") not in ("", "none")]
            if replacement_relative:
                plot_grouped_metric_by_benchmark(
                    figures_dir / "replacement_speedup_by_benchmark.svg",
                    replacement_relative,
                    "speedup_vs_lru",
                    "Speedup vs LRU",
                    1.0,
                    "gmean",
                    "geomean",
                )
                plot_grouped_metric_by_benchmark(
                    figures_dir / "replacement_l3_miss_reduction_by_benchmark.svg",
                    replacement_relative,
                    "l3_miss_reduction_vs_lru_pct",
                    "LLC miss reduction (%)",
                    0.0,
                    "avg",
                    "mean",
                )
                plot_metric_by_policy(
                    figures_dir / "replacement_speedup_vs_lru.svg",
                    replacement_relative,
                    "speedup_vs_lru",
                    "Speedup vs LRU",
                    "{:.2f}x",
                    1.0,
                )
                plot_metric_by_policy(
                    figures_dir / "replacement_l3_miss_reduction_vs_lru.svg",
                    replacement_relative,
                    "l3_miss_reduction_vs_lru_pct",
                    "LLC miss reduction vs LRU (%)",
                    "{:+.1f}%",
                    0.0,
                )
            if prefetch_relative:
                prefetchers = {row.get("prefetcher", "") for row in prefetch_relative}
                prefetch_prefix = "droplet" if prefetchers <= {"DROPLET"} else "prefetch"
                speedup_label = (
                    "Speedup vs DROPLET+LRU"
                    if prefetch_prefix == "droplet"
                    else "Speedup vs same-prefetcher LRU"
                )
                miss_label = (
                    "LLC miss reduction vs DROPLET+LRU (%)"
                    if prefetch_prefix == "droplet"
                    else "LLC miss reduction vs same-prefetcher LRU (%)"
                )
                plot_grouped_metric_by_benchmark(
                    figures_dir / f"{prefetch_prefix}_speedup_by_benchmark.svg",
                    prefetch_relative,
                    "speedup_vs_lru",
                    speedup_label,
                    1.0,
                    "gmean",
                    "geomean",
                )
                plot_grouped_metric_by_benchmark(
                    figures_dir / f"{prefetch_prefix}_l3_miss_reduction_by_benchmark.svg",
                    prefetch_relative,
                    "l3_miss_reduction_vs_lru_pct",
                    "LLC miss reduction (%)",
                    0.0,
                    "avg",
                    "mean",
                )
                plot_metric_by_policy(
                    figures_dir / f"{prefetch_prefix}_speedup_vs_lru.svg",
                    prefetch_relative,
                    "speedup_vs_lru",
                    speedup_label,
                    "{:.2f}x",
                    1.0,
                )
                plot_metric_by_policy(
                    figures_dir / f"{prefetch_prefix}_l3_miss_reduction_vs_lru.svg",
                    prefetch_relative,
                    "l3_miss_reduction_vs_lru_pct",
                    miss_label,
                    "{:+.1f}%",
                    0.0,
                )
        if overhead:
            plot_charged_overhead(figures_dir / "charged_overhead.svg", overhead)

        thread_scaling = thread_scaling_metrics(roi_rows)
        if thread_scaling:
            write_csv(aggregate_dir / "thread_scaling_metrics.csv", thread_scaling)
            sniper_thread_scaling = [row for row in thread_scaling if row.get("simulator") == "sniper"]
            if sniper_thread_scaling:
                write_csv(aggregate_dir / "sniper_thread_scaling_metrics.csv", sniper_thread_scaling)
                plot_sniper_thread_scaling(figures_dir / "sniper_thread_scaling.svg", sniper_thread_scaling)

        l_curve_groups = l_curve_rows(roi_rows)
        if l_curve_groups:
            l_curve_summary = _l_curve_summary_rows(l_curve_groups)
            write_csv(aggregate_dir / "l_curve_miss_rate_by_size.csv", l_curve_summary)
            for group_key, entries in sorted(l_curve_groups.items(), key=lambda item: (item[0][0], benchmark_sort_key(item[0][1]))):
                graph, app = group_key
                fig_path = figures_dir / f"l_curve_{graph}_{app}.svg"
                plot_l_curve(fig_path, group_key, entries)

    if proof_rows:
        write_csv(aggregate_dir / "proof_matrix_all.csv", proof_rows)
        proof_relative = proof_relative_metrics(proof_rows)
        if proof_relative:
            write_csv(aggregate_dir / "proof_relative_metrics.csv", proof_relative)
            proof_relative_summary = summarize_relative(
                proof_relative,
                (
                    "memory_traffic_reduction_vs_lru_pct",
                    "memory_traffic_ratio_vs_lru",
                    "l3_miss_reduction_vs_lru_pct",
                    "l3_miss_ratio_vs_lru",
                ),
            )
            write_csv(aggregate_dir / "proof_relative_policy_summary.csv", proof_relative_summary)
            plot_grouped_metric_by_benchmark(
                figures_dir / "component_memory_traffic_reduction_by_benchmark.svg",
                proof_relative,
                "memory_traffic_reduction_vs_lru_pct",
                "Memory traffic reduction (%)",
                0.0,
                "avg",
                "mean",
            )
            plot_metric_by_policy(
                figures_dir / "component_memory_traffic_reduction_vs_lru.svg",
                proof_relative,
                "memory_traffic_reduction_vs_lru_pct",
                "Memory traffic reduction vs LRU (%)",
                "{:+.1f}%",
                0.0,
            )

    prefetch_quality = prefetch_quality_summary(roi_rows, proof_rows)
    if prefetch_quality:
        write_csv(aggregate_dir / "prefetch_quality_summary.csv", prefetch_quality)
        roi_prefetch_quality = [row for row in prefetch_quality if row.get("source") in ("gem5", "sniper")]
        cache_sim_prefetch_quality = [row for row in prefetch_quality if row.get("source") == "cache_sim"]
        if roi_prefetch_quality:
            roi_prefetchers = {row.get("prefetcher", "") for row in roi_prefetch_quality}
            roi_prefetch_prefix = "droplet" if roi_prefetchers <= {"DROPLET"} else "prefetch"
            roi_prefetch_title = (
                "ROI DROPLET prefetch quality summary"
                if roi_prefetch_prefix == "droplet"
                else "ROI prefetch quality summary"
            )
            write_latex_table(
                tables_dir / "prefetch_quality_summary.tex",
                roi_prefetch_quality[:24],
                ["source", "benchmark", "prefetcher", "policy_short", "prefetch_accuracy_pct", "prefetch_unused_pct"],
                roi_prefetch_title,
            )
            plot_grouped_metric_by_benchmark(
                figures_dir / f"{roi_prefetch_prefix}_prefetch_accuracy_by_benchmark.svg",
                roi_prefetch_quality,
                "prefetch_accuracy_pct",
                "Useful prefetches / issued (%)",
                0.0,
                "avg",
                "mean",
            )
        if cache_sim_prefetch_quality:
            write_latex_table(
                tables_dir / "cache_sim_prefetch_quality_summary.tex",
                cache_sim_prefetch_quality[:24],
                [
                    "benchmark", "ablation", "policy_short", "prefetch_request_useful_pct",
                    "prefetch_fill_useful_pct", "traffic_per_demand_access",
                ],
                "cache_sim ECG prefetch quality summary",
            )

    bound_inputs: dict[str, Any] = {}
    for index, run_dir in enumerate(input_run_dirs or []):
        for name in (
                "run.complete.json", "combined_roi_matrix.csv",
                "resolved_manifest.json"):
            path = run_dir / name
            if path.exists():
                bound_inputs[f"run_{index}/{name}"] = bound_file(path)
    for index, path in enumerate(input_csvs or []):
        if path.exists():
            bound_inputs[f"csv_{index}/{path.name}"] = bound_file(path)

    bound_outputs: dict[str, Any] = {}
    for path in (
            aggregate_dir / "roi_matrix_all.csv",
            aggregate_dir / "roi_policy_summary.csv",
            tables_dir / "roi_policy_summary.tex"):
        if path.exists():
            bound_outputs[str(path.relative_to(out_dir))] = bound_file(path)

    manifest = {
        "created_utc": utc_now(),
        "roi_rows": len(roi_rows),
        "proof_rows": len(proof_rows),
        "has_matplotlib": HAS_MATPLOTLIB,
        "figure_format": "svg_primary_png_preview",
        "has_faithfulness_summary": bool(roi_rows),
        "has_prefetch_quality_summary": bool(prefetch_quality),
        "scripts": {
            "aggregate_results.py": bound_file(Path(__file__)),
            "experiment_run.py": bound_file(EXPERIMENT_RUNNER),
        },
        "inputs": bound_inputs,
        "outputs": bound_outputs,
    }
    (out_dir / "aggregation_manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run experiment profiles, aggregate data, and generate figures.")
    parser.add_argument("--profiles", nargs="+", default=["ecg_smoke"], help="experiment_run.py profiles to run.")
    parser.add_argument("--run-root", default="", help="Pipeline run root. Defaults to results/ecg_experiments/aggregates/<timestamp>.")
    parser.add_argument("--input-run-dirs", nargs="+", default=[], help="Existing final-run directories to aggregate.")
    parser.add_argument("--input-run-glob", nargs="+", default=[], help="Glob(s) for existing final-run directories to aggregate, useful for Slurm shards.")
    parser.add_argument("--input-csv", nargs="+", default=[], help="Additional roi_matrix.csv or proof_matrix.csv files to aggregate.")
    parser.add_argument("--input-csv-glob", nargs="+", default=[], help="Glob(s) for additional roi_matrix.csv or proof_matrix.csv files to aggregate.")
    parser.add_argument("--skip-run", action="store_true", help="Do not launch profiles; only aggregate inputs.")
    parser.add_argument("--dry-run", action="store_true", help="Pass --dry-run to flows/experiment_run.py.")
    parser.add_argument("--no-build", action="store_true", help="Pass --no-build to flows/experiment_run.py.")
    parser.add_argument("--allow-missing-graphs", action="store_true", help="Pass --allow-missing-graphs to flows/experiment_run.py.")
    parser.add_argument("--force", action="store_true", help="Pass --force to flows/experiment_run.py.")
    parser.add_argument(
        "--require-reference-python", "--require-pinned-python",
        dest="require_pinned_python", action="store_true",
        help="Use the repository reference Python for launched profiles.")
    parser.add_argument("--no-stop-on-error", action="store_false", dest="stop_on_error", help="Continue after failed profile.")
    parser.set_defaults(stop_on_error=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    run_root = Path(args.run_root) if args.run_root else RESULTS_ROOT / now_tag()
    if not run_root.is_absolute():
        run_root = PROJECT_ROOT / run_root
    run_root.mkdir(parents=True, exist_ok=True)

    run_dir_inputs = list(args.input_run_dirs)
    for pattern in args.input_run_glob:
        resolved_pattern = str(resolve_path(pattern)) if not Path(pattern).is_absolute() else pattern
        run_dir_inputs.extend(sorted(glob.glob(resolved_pattern)))
    run_dirs = discover_input_run_dirs(
        [resolve_path(path) for path in run_dir_inputs])
    if not args.skip_run:
        for profile in args.profiles:
            run_dirs.append(run_profile(args, run_root, profile))

    csv_inputs = list(args.input_csv)
    for pattern in args.input_csv_glob:
        resolved_pattern = str(resolve_path(pattern)) if not Path(pattern).is_absolute() else pattern
        csv_inputs.extend(sorted(glob.glob(resolved_pattern)))
    input_csvs = [resolve_path(path) for path in csv_inputs]
    roi_rows, proof_rows = collect_csvs(run_dirs, input_csvs)
    print(f"[aggregate] roi_rows={len(roi_rows)} proof_rows={len(proof_rows)}")
    if args.dry_run and not roi_rows and not proof_rows:
        print("[pipeline] dry-run complete; no result rows expected")
        return 0
    if not roi_rows and not proof_rows:
        raise SystemExit("no complete ROI or proof rows found")
    generate_outputs(
        run_root, roi_rows, proof_rows,
        input_run_dirs=run_dirs, input_csvs=input_csvs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))