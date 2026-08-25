#!/usr/bin/env python3
"""Run the frozen cache_sim color sweep for admission-selector generation 2."""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
ROI_MATRIX = ROOT / "scripts/experiments/ecg/roi_matrix.py"
OFFSETS = tuple(range(8))
CONTROL = "ECG:REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH"
FUTURE = "ECG:REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH"
ONLINE = "ECG:REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH"
GRAPHS = (
    ("web-Google-n16",
     "results/graphs/web-Google-n16/web-Google-n16.sg",
     "5", "16kB", "64kB", "128kB"),
    ("soc-pokec-n16",
     "results/graphs/soc-pokec-n16/soc-pokec-n16.sg",
     "5", "16kB", "64kB", "128kB"),
    ("cit-Patents-n18-sym",
     "results/graphs/cit-Patents-n18/cit-Patents-n18-sym.sg",
     "5", "32kB", "128kB", "512kB"),
    ("temporal-spread-directed-n16",
     "results/graphs/temporal-reuse/spread-n16.sg",
     "0", "16kB", "64kB", "128kB"),
)


def run_rows(
        graph: tuple[str, str, str, str, str, str],
        policies: tuple[str, ...],
        out_dir: Path,
        offset: int | None) -> list[dict[str, str]]:
    name, graph_path, order, l1, l2, l3 = graph
    explicit = {
        "ECG_RECORD_TIER_BITS": "2",
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
    }
    if offset is not None:
        explicit["CACHE_ECG_ADMISSION_SET_OFFSET"] = str(offset)
    command = [
        sys.executable, str(ROI_MATRIX),
        "--suite", "cache-sim",
        "--benchmark", "pr",
        "--options", f"-f {graph_path} -o {order} -n 1 -i 1",
        "--policies", *policies,
        "--prefetcher", "none",
        "--l1d-size", l1,
        "--l2-size", l2,
        "--l1d-ways", "8",
        "--l2-ways", "8",
        "--l3-sizes", l3,
        "--l3-ways", "16",
        "--line-size", "64",
        "--out-dir", str(out_dir),
        "--ecg-charged", "1",
        "--ecg-epochs", "32",
        "--cache-sim-omp-threads", "1",
        "--flowthrough", "all",
        "--require-cache-sim-aslr-disable",
        "--no-build",
    ]
    env = dict(os.environ)
    env["GRAPHBREW_EXPLICIT_CELL_ENV"] = json.dumps(
        explicit, sort_keys=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    with (out_dir / "roi_matrix.csv").open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != len(policies):
        raise RuntimeError(
            f"{name}/offset={offset}: expected {len(policies)} rows, "
            f"got {len(rows)}")
    for row in rows:
        row["final_graph"] = name
        row["sweep_offset"] = "static" if offset is None else str(offset)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    out_dir = args.out_dir.resolve()
    if out_dir.exists():
        parser.error(f"output directory already exists: {out_dir}")
    out_dir.mkdir(parents=True)

    subprocess.run(
        ["make", "-s", "sim-pr"], cwd=ROOT, check=True)
    rows: list[dict[str, str]] = []
    for graph in GRAPHS:
        name = graph[0]
        rows.extend(run_rows(
            graph, (CONTROL, FUTURE),
            out_dir / f"{name}_static", None))
        for offset in OFFSETS:
            rows.extend(run_rows(
                graph, (ONLINE,),
                out_dir / f"{name}_offset_{offset}", offset))

    fields = sorted({field for row in rows for field in row})
    combined = out_dir / "online_admission_color_sweep.csv"
    with combined.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    receipt = {
        "schema": 1,
        "graphs": [graph[0] for graph in GRAPHS],
        "offsets": list(OFFSETS),
        "static_policies": [CONTROL, FUTURE],
        "online_policy": ONLINE,
        "row_count": len(rows),
        "combined_csv": str(combined),
    }
    (out_dir / "sweep_receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(combined)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
