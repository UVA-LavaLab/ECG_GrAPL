#!/usr/bin/env python3
"""Fail-closed regret gate for the online admission selector."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


GRAPHS = (
    "web-Google-n16",
    "soc-pokec-n16",
    "cit-Patents-n18-sym",
    "temporal-spread-directed-n16",
)
CONTROL = "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH"
FUTURE = "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH"
ONLINE = "ECG_REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH"
POLICIES = ("LRU", "GRASP", CONTROL, FUTURE, ONLINE)
METRICS = ("l3_misses", "total_offchip_traffic")
REGRET_LIMIT = 1.02


def integer(row: dict[str, str], field: str) -> int:
    try:
        return int(float(row[field]))
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(
            f"{row.get('final_graph')}/{row.get('policy_label')}: "
            f"missing integer {field}") from exc


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
        if (
                row.get("status") != "ok" or
                row.get("simulator") != "cache_sim" or
                row.get("benchmark") != "pr" or
                row.get("flowthrough") != "all"):
            raise ValueError(f"invalid row contract for {key}")
        for metric in METRICS:
            if integer(row, metric) <= 0:
                raise ValueError(f"nonpositive {metric} for {key}")
        observed[key] = row
    missing = sorted(expected - set(observed))
    if missing:
        raise ValueError(f"missing rows: {missing}")

    per_graph: dict[str, object] = {}
    all_pass = True
    grasp_followers = 0
    future_followers = 0
    for graph in GRAPHS:
        online = observed[(graph, ONLINE)]
        accesses = [
            integer(online, "ecg_admission_leader_accesses_grasp"),
            integer(online, "ecg_admission_leader_accesses_future"),
        ]
        misses = [
            integer(online, "ecg_admission_leader_misses_grasp"),
            integer(online, "ecg_admission_leader_misses_future"),
        ]
        windows = integer(online, "ecg_admission_completed_windows")
        if min(accesses) <= 0 or min(misses) <= 0 or windows <= 0:
            raise ValueError(f"selector not exercised for {graph}")
        access_balance = max(accesses) / min(accesses)
        balance_pass = access_balance <= 2.0
        grasp_followers += integer(
            online, "ecg_admission_follower_selections_grasp")
        future_followers += integer(
            online, "ecg_admission_follower_selections_future")

        ratios: dict[str, float] = {}
        for metric in METRICS:
            best = min(
                integer(observed[(graph, CONTROL)], metric),
                integer(observed[(graph, FUTURE)], metric))
            ratios[metric] = integer(online, metric) / best
        passed = (
            balance_pass and
            all(ratio <= REGRET_LIMIT for ratio in ratios.values()))
        all_pass = all_pass and passed
        per_graph[graph] = {
            "ratios_to_best_static": ratios,
            "leader_accesses": {
                "grasp": accesses[0], "future": accesses[1]},
            "leader_misses": {
                "grasp": misses[0], "future": misses[1]},
            "completed_windows": windows,
            "leader_access_balance": access_balance,
            "leader_access_balance_pass": balance_pass,
            "winner_changes": integer(
                online, "ecg_admission_winner_changes"),
            "final_winner_arm": integer(
                online, "ecg_admission_final_winner_arm"),
            "pass": passed,
        }
    if grasp_followers <= 0 or future_followers <= 0:
        all_pass = False
    return {
        "schema": 1,
        "input": str(path),
        "regret_limit": REGRET_LIMIT,
        "per_graph": per_graph,
        "follower_selections": {
            "grasp": grasp_followers,
            "future": future_followers,
        },
        "promote_to_gem5": all_pass,
        "decision": "PROMOTE" if all_pass else "STOP",
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
