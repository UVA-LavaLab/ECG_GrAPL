#!/usr/bin/env python3
"""Fail-closed color-stability gate for admission-selector generation 2."""

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
    "temporal-spread-directed-n16",
)
OFFSETS = tuple(range(8))
CONTROL = "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH"
FUTURE = "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH"
ONLINE = "ECG_REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH"
STATIC_ARMS = (CONTROL, FUTURE)
METRICS = ("l3_misses", "total_offchip_traffic")
REGRET_LIMIT = 1.02
SAMPLES_PER_ARM = 64


def integer(row: dict[str, str], field: str) -> int:
    try:
        numeric = float(row[field])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(
            f"{row.get('final_graph')}/{row.get('policy_label')}: "
            f"missing integer {field}") from exc
    value = int(numeric)
    if not math.isfinite(numeric) or numeric != value:
        raise ValueError(
            f"{row.get('final_graph')}/{row.get('policy_label')}: "
            f"non-integral {field}={row.get(field)!r}")
    return value


def evaluate(path: Path) -> dict[str, object]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))

    static: dict[tuple[str, str], dict[str, str]] = {}
    online: dict[tuple[str, int], dict[str, str]] = {}
    for row in rows:
        graph = row.get("final_graph", "")
        policy = row.get("policy_label", "")
        if graph not in GRAPHS:
            raise ValueError(f"unexpected graph {graph!r}")
        if (
                row.get("status") != "ok" or
                row.get("simulator") != "cache_sim" or
                row.get("benchmark") != "pr" or
                row.get("flowthrough") != "all"):
            raise ValueError(f"invalid row contract for {graph}/{policy}")
        for metric in METRICS:
            if integer(row, metric) <= 0:
                raise ValueError(f"nonpositive {metric} for {graph}/{policy}")

        sweep_offset = row.get("sweep_offset", "")
        if policy in STATIC_ARMS and sweep_offset == "static":
            key = (graph, policy)
            if key in static:
                raise ValueError(f"duplicate static row {key}")
            static[key] = row
        elif policy == ONLINE:
            try:
                offset = int(sweep_offset)
            except ValueError as exc:
                raise ValueError(
                    f"invalid online offset {sweep_offset!r}") from exc
            if offset not in OFFSETS:
                raise ValueError(f"unexpected online offset {offset}")
            key = (graph, offset)
            if key in online:
                raise ValueError(f"duplicate online row {key}")
            online[key] = row
        else:
            raise ValueError(
                f"unexpected policy/offset {policy!r}/{sweep_offset!r}")

    expected_static = {
        (graph, policy) for graph in GRAPHS for policy in STATIC_ARMS
    }
    expected_online = {
        (graph, offset) for graph in GRAPHS for offset in OFFSETS
    }
    if set(static) != expected_static:
        raise ValueError(
            f"static roster mismatch: missing={sorted(expected_static-set(static))} "
            f"extra={sorted(set(static)-expected_static)}")
    if set(online) != expected_online:
        raise ValueError(
            f"online roster mismatch: missing={sorted(expected_online-set(online))} "
            f"extra={sorted(set(online)-expected_online)}")

    per_graph: dict[str, object] = {}
    promote = True
    for graph in GRAPHS:
        control = static[(graph, CONTROL)]
        future = static[(graph, FUTURE)]
        static_misses = [integer(control, "l3_misses"),
                         integer(future, "l3_misses")]
        static_winner = 0 if static_misses[0] <= static_misses[1] else 1
        offsets: dict[str, object] = {}
        graph_pass = True
        for offset in OFFSETS:
            row = online[(graph, offset)]
            accesses = [
                integer(row, "ecg_admission_leader_accesses_grasp"),
                integer(row, "ecg_admission_leader_accesses_future"),
            ]
            misses = [
                integer(row, "ecg_admission_leader_misses_grasp"),
                integer(row, "ecg_admission_leader_misses_future"),
            ]
            winner = integer(row, "ecg_admission_final_winner_arm")
            sample_winner = 1 if misses[1] < misses[0] else 0
            winner_changes = integer(
                row, "ecg_admission_winner_changes")
            exact_window = (
                accesses == [SAMPLES_PER_ARM, SAMPLES_PER_ARM] and
                min(misses) > 0 and
                all(miss <= access for miss, access in zip(misses, accesses)) and
                integer(row, "ecg_admission_completed_windows") == 1)
            sample_receipt = (
                winner == sample_winner and
                winner_changes == int(sample_winner == 1))
            offset_receipt = (
                integer(row, "ecg_admission_set_offset") == offset)
            representative = winner == static_winner
            ratios: dict[str, float] = {}
            for metric in METRICS:
                best = min(integer(control, metric), integer(future, metric))
                ratios[metric] = integer(row, metric) / best
            regret_pass = all(
                ratio <= REGRET_LIMIT for ratio in ratios.values())
            passed = (
                exact_window and sample_receipt and offset_receipt and
                representative and regret_pass)
            graph_pass = graph_pass and passed
            offsets[str(offset)] = {
                "leader_accesses": {
                    "grasp": accesses[0], "future": accesses[1]},
                "leader_misses": {
                    "grasp": misses[0], "future": misses[1]},
                "leader_miss_rates": {
                    "grasp": misses[0] / accesses[0],
                    "future": misses[1] / accesses[1],
                },
                "final_winner_arm": winner,
                "sample_winner_arm": sample_winner,
                "sample_winner_receipt": sample_receipt,
                "winner_changes": winner_changes,
                "winner_matches_full_static": representative,
                "ratios_to_best_static": ratios,
                "exact_window": exact_window,
                "offset_receipt": offset_receipt,
                "regret_pass": regret_pass,
                "pass": passed,
            }
        promote = promote and graph_pass
        per_graph[graph] = {
            "static_l3_misses": {
                "grasp": static_misses[0], "future": static_misses[1]},
            "static_winner_arm": static_winner,
            "offsets": offsets,
            "stable_winner": all(
                entry["final_winner_arm"] == static_winner
                for entry in offsets.values()),
            "worst_regret": {
                metric: max(
                    entry["ratios_to_best_static"][metric]
                    for entry in offsets.values())
                for metric in METRICS
            },
            "pass": graph_pass,
        }

    return {
        "schema": 2,
        "generation": "online-admission-color-stable-v2",
        "input": str(path),
        "offsets": list(OFFSETS),
        "samples_per_arm": SAMPLES_PER_ARM,
        "regret_limit": REGRET_LIMIT,
        "per_graph": per_graph,
        "promote_to_detailed_simulators": promote,
        "decision": "PROMOTE" if promote else "STOP",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = evaluate(args.input)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
