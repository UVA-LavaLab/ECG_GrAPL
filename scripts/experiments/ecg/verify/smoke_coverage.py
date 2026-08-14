#!/usr/bin/env python3
"""Validate the final 3-simulator/all-algorithm smoke aggregate."""

from __future__ import annotations

import argparse
import csv
import math
from collections import Counter, defaultdict
from pathlib import Path


SIMULATORS = ("cache_sim", "gem5", "sniper")
BENCHMARKS = ("pr", "bfs", "sssp", "bc", "cc")
DEFAULT_GRAPHS = ("kron_s12_k4",)
POLICIES = (
    "LRU", "SRRIP", "GRASP", "POPT",
    "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE",
    "ECG_REUSE_PLAN_FLOWTHROUGH", "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH",
)
REUSE_PLAN_POLICIES = set(POLICIES[4:])
FLOWTHROUGH_POLICIES = {"ECG_REUSE_PLAN_FLOWTHROUGH", "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH"}


def row_name(row: dict[str, str]) -> str:
    return (
        f"{row.get('simulator')}/{row.get('final_graph')}/"
        f"{row.get('benchmark')}/{row.get('policy_label')}")


def number(row: dict[str, str], field: str) -> float | None:
    value = row.get(field, "")
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def require_number(
        errors: list[str], row: dict[str, str], field: str,
        *, positive: bool = False) -> None:
    value = number(row, field)
    if value is None or (positive and value <= 0):
        errors.append(f"{row_name(row)}: missing {field}")


def validate(
        rows: list[dict[str, str]],
        graphs: tuple[str, ...] = DEFAULT_GRAPHS,
        instruction_cap: int = 0,
        simulators: tuple[str, ...] = SIMULATORS,
        require_fused_receipts: bool = True) -> list[str]:
    errors: list[str] = []
    expected_cells = {
        (simulator, graph, benchmark)
        for simulator in simulators
        for graph in graphs
        for benchmark in BENCHMARKS
    }
    grouped: dict[tuple[str, str, str], set[str]] = defaultdict(set)

    for row in rows:
        simulator = row.get("simulator", "")
        graph = row.get("final_graph", "")
        benchmark = row.get("benchmark", "")
        policy = row.get("policy_label", "")
        if simulator not in simulators:
            errors.append(f"unexpected simulator={simulator!r}")
            continue
        if benchmark not in BENCHMARKS:
            errors.append(f"unexpected benchmark={benchmark!r}")
            continue
        if policy not in POLICIES:
            errors.append(f"unexpected policy={policy!r}")
            continue
        if graph not in graphs:
            errors.append(f"{row_name(row)}: unexpected graph={graph!r}")
        grouped[(simulator, graph, benchmark)].add(policy)
        for field in ("l3_size", "l3_ways", "timing_model"):
            if not row.get(field):
                errors.append(f"{row_name(row)}: missing {field}")
        if row.get("status") != "ok":
            errors.append(f"{row_name(row)}: status={row.get('status')}")
        if row.get("final_output_status", "ok") != "ok":
            errors.append(
                f"{row_name(row)}: "
                f"final_output_status={row.get('final_output_status')}")
        if str(row.get("l3_exercised", "")).lower() not in ("1", "true"):
            errors.append(f"{row_name(row)}: L3 not exercised")
        require_number(errors, row, "l3_misses", positive=True)
        require_number(errors, row, "l3_miss_rate")
        require_number(errors, row, "timing_valid_for_speedup")

        if simulator == "cache_sim":
            require_number(errors, row, "total_accesses", positive=True)
            require_number(
                errors, row, "total_memory_traffic_with_overhead",
                positive=True)
            require_number(errors, row, "l3_hits")
            require_number(errors, row, "l3_prop_misses")
        elif simulator == "gem5":
            require_number(errors, row, "l3_accesses", positive=True)
            require_number(errors, row, "dram_read_bytes", positive=True)
            require_number(errors, row, "dram_write_bytes")
            require_number(errors, row, "sim_ticks", positive=True)
            require_number(errors, row, "ipc")
        else:
            require_number(errors, row, "l3_accesses", positive=True)
            require_number(errors, row, "instructions", positive=True)
            require_number(errors, row, "sim_ticks", positive=True)
            require_number(errors, row, "ipc")
            for field in (
                    "sniper_cpi_base", "sniper_cpi_data_cache",
                    "sniper_cpi_data_llc", "sniper_cpi_data_dram"):
                require_number(errors, row, field)

        if instruction_cap > 0 and simulator in ("gem5", "sniper"):
            cap_field = (
                "gem5_max_insts"
                if simulator == "gem5"
                else "sniper_roi_icount")
            if number(row, cap_field) != instruction_cap:
                errors.append(
                    f"{row_name(row)}: {cap_field} != {instruction_cap}")
            if number(row, "timing_valid_for_speedup") != 0:
                errors.append(
                    f"{row_name(row)}: capped timing marked speed-valid")

        if policy in REUSE_PLAN_POLICIES:
            if number(row, "ecg_reuse_plan_depth") != 2:
                errors.append(f"{row_name(row)}: reuse_plan_depth != 2")
            if number(row, "ecg_epochs_effective") != 32768:
                errors.append(f"{row_name(row)}: effective epochs != 32768")
            if simulator == "gem5":
                isa_name = (
                    "load"
                    if row.get("ecg_isa_variant") == "computed"
                    else "iload")
                expected = (
                    f"ecg.flow.weighted+ecg.bind.{isa_name}.cw24"
                    if policy in FLOWTHROUGH_POLICIES and
                    row.get("benchmark") == "sssp"
                    else f"ecg.flow.load+ecg.bind.{isa_name}"
                    if policy in FLOWTHROUGH_POLICIES
                    else f"ecg.plan.weighted+ecg.bind.{isa_name}.cw24"
                    if row.get("benchmark") == "sssp"
                    else f"ecg.bind.{isa_name}")
                if row.get("gem5_ecg_delivery") != expected:
                    errors.append(
                        f"{row_name(row)}: delivery="
                        f"{row.get('gem5_ecg_delivery')!r}, expected={expected!r}")
                if (policy in FLOWTHROUGH_POLICIES and
                        not (number(
                            row, "gem5_flowthrough_trace_events") or 0) > 0):
                    errors.append(
                        f"{row_name(row)}: FlowThrough trace missing")
            if simulator == "sniper":
                if row.get("sniper_ecg_delivery") not in {
                        "fused-reuse_plan-model",
                        "fused-reuse_plan-weighted64-model",
                        "fused-reuse_plan-weighted32-model"}:
                    errors.append(f"{row_name(row)}: fused delivery missing")
                bad_receipts = number(row, "sniper_fused_reuse_plan_bad_receipts")
                if require_fused_receipts:
                    require_number(
                        errors, row, "sniper_fused_reuse_plan_receipts", positive=True)
                    if bad_receipts is None:
                        errors.append(
                            f"{row_name(row)}: "
                            "missing sniper_fused_reuse_plan_bad_receipts")
                if bad_receipts not in (None, 0):
                    errors.append(
                        f"{row_name(row)}: "
                        f"bad fused receipts={bad_receipts:g}")
                if policy in FLOWTHROUGH_POLICIES:
                    require_number(
                        errors, row, "sniper_flowthrough_reads",
                        positive=True)
                    require_number(
                        errors, row, "sniper_flowthrough_writes",
                        positive=True)

    expected_rows = (
        len(simulators) * len(graphs) * len(BENCHMARKS) * len(POLICIES))
    if len(rows) != expected_rows:
        errors.append(f"expected {expected_rows} rows, found {len(rows)}")
    if set(grouped) != expected_cells:
        errors.append(
            f"expected {len(expected_cells)} simulator/graph/benchmark cells, "
            f"found {len(grouped)}")
    for cell in sorted(expected_cells):
        actual = grouped.get(cell, set())
        if actual != set(POLICIES):
            errors.append(
                f"{cell[0]}/{cell[1]}/{cell[2]} policy set mismatch: "
                f"missing={sorted(set(POLICIES) - actual)} "
                f"extra={sorted(actual - set(POLICIES))}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate final ECG 3-sim/all-alg smoke coverage.")
    parser.add_argument("--csv", required=True)
    parser.add_argument(
        "--graph", nargs="+", default=list(DEFAULT_GRAPHS),
        help="Expected graph names. Defaults to the bounded smoke graph.")
    parser.add_argument(
        "--instruction-cap", type=int, default=0,
        help="Expected gem5/Sniper detailed-ROI instruction cap; 0 means full work.")
    parser.add_argument(
        "--simulator", nargs="+", choices=SIMULATORS,
        default=list(SIMULATORS),
        help="Expected simulator set. Defaults to all three backends.")
    parser.add_argument(
        "--allow-unvalidated-fused-receipts", action="store_true",
        help="Accept fused ReusePlan rows without per-row receipt traces when a "
             "separate mechanism gate validates the transport.")
    args = parser.parse_args()
    path = Path(args.csv)
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    graphs = tuple(dict.fromkeys(args.graph))
    simulators = tuple(dict.fromkeys(args.simulator))
    errors = validate(
        rows, graphs, args.instruction_cap, simulators,
        not args.allow_unvalidated_fused_receipts)
    counts = Counter(row.get("simulator", "") for row in rows)
    print(
        f"[smoke-coverage] rows={len(rows)} "
        f"cache_sim={counts['cache_sim']} gem5={counts['gem5']} "
        f"sniper={counts['sniper']} graphs={','.join(graphs)}")
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    print(
        f"[smoke-coverage] PASS: all {len(rows)} required rows and metrics "
        "are present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
