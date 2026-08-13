#!/usr/bin/env python3
"""Compare matched 16-, 15-, and 14-way K2 full-graph matrices."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


K2_POLICIES = (
    "ECG_K2",
    "ECG_K2_ONLINE",
    "ECG_K2_STREAMSHIELD",
    "ECG_K2_ONLINE_STREAMSHIELD",
)
BASELINE_POLICIES = ("LRU", "SRRIP", "GRASP", "HAWKEYE_PROXY", "POPT")


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError(f"matrix is empty: {path}")
    bad = [
        f"{row.get('final_graph')}/{row.get('benchmark')}/"
        f"{row.get('policy_label')}={row.get('status')}"
        for row in rows if row.get("status") != "ok"
    ]
    if bad:
        raise ValueError(f"matrix has non-ok rows: {', '.join(bad[:5])}")
    return rows


def row_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        row.get("final_graph", ""),
        row.get("benchmark", ""),
        row.get("policy_label", ""),
    )


def effective_misses(row: dict[str, str]) -> int:
    value = row.get("l3_misses_with_overhead") or row.get("l3_misses")
    if value in (None, ""):
        raise ValueError(f"missing L3 misses for {row_key(row)}")
    return int(value)


def load_matrix(path: Path, expected_k2_ways: int) -> dict[tuple[str, str, str], dict[str, str]]:
    indexed = {row_key(row): row for row in read_rows(path)}
    if len(indexed) != 135:
        raise ValueError(
            f"expected 135 unique rows in {path}, got {len(indexed)}")
    for key, row in indexed.items():
        if key[2] in K2_POLICIES:
            actual = int(row.get("l3_effective_ways") or 0)
            if actual != expected_k2_ways:
                raise ValueError(
                    f"{key}: K2 ways={actual}, expected {expected_k2_ways}")
        elif key[2] != "POPT":
            actual = int(row.get("l3_effective_ways") or 0)
            if actual != 16:
                raise ValueError(
                    f"{key}: baseline ways={actual}, expected 16")
    return indexed


def geomean(values: list[float]) -> float:
    if not values or any(value <= 0 for value in values):
        raise ValueError("geomean requires positive values")
    return math.prod(values) ** (1.0 / len(values))


def analyze(
        matrix16: Path, matrix15: Path,
        matrix14: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    matrices = {
        16: load_matrix(matrix16, 16),
        15: load_matrix(matrix15, 15),
        14: load_matrix(matrix14, 14),
    }
    keys = set(matrices[16])
    for ways in (15, 14):
        if set(matrices[ways]) != keys:
            raise ValueError(f"{ways}-way matrix cells/policies differ")

    baseline_mismatches = []
    for key in sorted(keys):
        if key[2] not in BASELINE_POLICIES:
            continue
        reference = effective_misses(matrices[16][key])
        for ways in (15, 14):
            measured = effective_misses(matrices[ways][key])
            if measured != reference:
                baseline_mismatches.append({
                    "graph": key[0],
                    "benchmark": key[1],
                    "policy": key[2],
                    "ways": ways,
                    "reference_misses": reference,
                    "measured_misses": measured,
                })
    if baseline_mismatches:
        raise ValueError(
            f"non-K2 baselines drift across matrices: "
            f"{baseline_mismatches[:3]}")

    cells = []
    summary: dict[str, Any] = {
        "status": "passed",
        "cells": 15,
        "rows_per_matrix": 135,
        "baseline_mismatches": 0,
        "policies": {},
    }
    for policy in K2_POLICIES:
        vs_lru: dict[int, list[float]] = {16: [], 15: [], 14: []}
        vs_16: dict[int, list[float]] = {15: [], 14: []}
        for graph, benchmark, row_policy in sorted(keys):
            if row_policy != policy:
                continue
            lru_key = (graph, benchmark, "LRU")
            misses = {
                ways: effective_misses(matrices[ways][
                    (graph, benchmark, policy)])
                for ways in (16, 15, 14)
            }
            lru_misses = effective_misses(matrices[16][lru_key])
            for ways in (16, 15, 14):
                vs_lru[ways].append(misses[ways] / lru_misses)
            for ways in (15, 14):
                vs_16[ways].append(misses[ways] / misses[16])
            cells.append({
                "graph": graph,
                "benchmark": benchmark,
                "policy": policy,
                "misses_16": misses[16],
                "misses_15": misses[15],
                "misses_14": misses[14],
                "ratio_15_over_16": misses[15] / misses[16],
                "ratio_14_over_16": misses[14] / misses[16],
            })
        summary["policies"][policy] = {
            "geomean_miss_ratio_vs_lru": {
                str(ways): geomean(vs_lru[ways])
                for ways in (16, 15, 14)
            },
            "geomean_capacity_penalty_vs_16": {
                str(ways): geomean(vs_16[ways])
                for ways in (15, 14)
            },
            "capacity_penalty_percent_vs_16": {
                str(ways): 100.0 * (geomean(vs_16[ways]) - 1.0)
                for ways in (15, 14)
            },
        }
    return summary, cells


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze matched 16/15/14-way K2 matrices.")
    parser.add_argument("--matrix-16", type=Path, required=True)
    parser.add_argument("--matrix-15", type=Path, required=True)
    parser.add_argument("--matrix-14", type=Path, required=True)
    parser.add_argument("--out-json", type=Path, required=True)
    parser.add_argument("--out-csv", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary, cells = analyze(
        args.matrix_16, args.matrix_15, args.matrix_14)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")
    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.out_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(cells[0]))
        writer.writeheader()
        writer.writerows(cells)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
