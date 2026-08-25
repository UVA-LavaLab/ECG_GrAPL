#!/usr/bin/env python3
"""Fail-closed signal gate for combined-mask admission and eviction policies."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


GRAPHS = (
    "web-Google-n16",
    "soc-pokec-n16",
    "cit-Patents-n18-sym",
)
POLICIES = (
    "LRU",
    "GRASP",
    "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
    "ECG_REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH",
    "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
    "ECG_REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
    "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH",
    "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
)
ADMISSION_POLICIES = {
    "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
    "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
    "ECG_REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
}
CONTRASTS = {
    "future_tier_eviction": (
        "ECG_REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
    ),
    "future_admission_recency": (
        "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
    ),
    "combined_admission_recency": (
        "ECG_REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
    ),
    "future_admission_epoch": (
        "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH",
    ),
}
METRICS = ("l3_misses", "total_offchip_traffic")
AGGREGATE_LIMIT = 0.97
WORST_GRAPH_LIMIT = 1.02


def require_int(row: dict[str, str], field: str) -> int:
    try:
        value = int(float(row[field]))
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(
            f"{row.get('final_graph')}/{row.get('policy_label')}: "
            f"missing integer {field}") from exc
    if value < 0:
        raise ValueError(f"{field} must be nonnegative, got {value}")
    return value


def geometric_mean(values: list[float]) -> float:
    if not values or any(value <= 0 for value in values):
        raise ValueError("geometric mean requires positive values")
    return math.exp(sum(math.log(value) for value in values) / len(values))


def evaluate(path: Path) -> dict[str, object]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    expected = {(graph, policy) for graph in GRAPHS for policy in POLICIES}
    observed: dict[tuple[str, str], dict[str, str]] = {}
    for row in rows:
        key = (row.get("final_graph", ""), row.get("policy_label", ""))
        if key not in expected:
            raise ValueError(f"unexpected row {key}")
        if key in observed:
            raise ValueError(f"duplicate row {key}")
        if row.get("status") != "ok" or row.get("simulator") != "cache_sim":
            raise ValueError(f"invalid row status/simulator for {key}")
        if row.get("benchmark") != "pr" or row.get("flowthrough") != "all":
            raise ValueError(f"wrong benchmark/FlowThrough mode for {key}")
        for metric in METRICS:
            if require_int(row, metric) <= 0:
                raise ValueError(f"{metric} must be positive for {key}")
        updates = require_int(row, "ecg_reuse_admission_updates")
        if (key[1] in ADMISSION_POLICIES) != (updates > 0):
            raise ValueError(
                f"admission activity mismatch for {key}: updates={updates}")
        if key[1].startswith("ECG_") and require_int(
                row, "ecg_record_bytes") != 4:
            raise ValueError(f"non-compact ReusePlan row {key}")
        observed[key] = row
    missing = sorted(expected - set(observed))
    if missing:
        raise ValueError(f"missing rows: {missing}")

    results: dict[str, object] = {}
    promote = False
    for name, (candidate, baseline) in CONTRASTS.items():
        per_graph: dict[str, dict[str, float]] = {}
        aggregate: dict[str, float] = {}
        for metric in METRICS:
            ratios = []
            for graph in GRAPHS:
                ratio = (
                    require_int(observed[(graph, candidate)], metric) /
                    require_int(observed[(graph, baseline)], metric)
                )
                ratios.append(ratio)
                per_graph.setdefault(graph, {})[metric] = ratio
            aggregate[metric] = geometric_mean(ratios)
        passed = (
            all(aggregate[metric] <= AGGREGATE_LIMIT for metric in METRICS)
            and all(
                values[metric] <= WORST_GRAPH_LIMIT
                for values in per_graph.values()
                for metric in METRICS
            )
        )
        promote = promote or passed
        results[name] = {
            "candidate": candidate,
            "baseline": baseline,
            "per_graph": per_graph,
            "geomean": aggregate,
            "pass": passed,
        }
    return {
        "schema": 1,
        "input": str(path),
        "graphs": list(GRAPHS),
        "metrics": list(METRICS),
        "aggregate": "unweighted_geometric_mean_of_per_graph_ratios",
        "aggregate_limit": AGGREGATE_LIMIT,
        "worst_graph_limit": WORST_GRAPH_LIMIT,
        "contrasts": results,
        "promote_to_gem5": promote,
        "decision": "PROMOTE" if promote else "STOP",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        decision = evaluate(args.input)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    text = json.dumps(decision, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
