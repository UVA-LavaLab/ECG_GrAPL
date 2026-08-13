#!/usr/bin/env python3
"""Sniper sim.stats parser for GraphBrew.

Sniper writes key/value stats as `name = value` under `<out>/simulation/sim.stats`.
This parser extracts a small common subset for early GraphBrew smoke tests and
future `roi_matrix.py --suite sniper` integration.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any


def parse_value(text: str) -> int | float | str:
    stripped = text.strip()
    # Multi-core Sniper writes per-core (and per-NUCA-slice) counters as a
    # comma-separated list, e.g. "5169547, 3395879". Aggregate cache/instruction
    # counters are summed across cores so the whole-run miss rate is well-defined
    # (single-core values contain no comma and are unaffected).
    if "," in stripped:
        parts = [part.strip() for part in stripped.split(",") if part.strip() != ""]
        try:
            if any(any(ch in part for ch in (".", "e", "E")) for part in parts):
                return sum(float(part) for part in parts)
            return sum(int(part) for part in parts)
        except ValueError:
            return stripped
    try:
        if any(char in stripped for char in (".", "e", "E")):
            return float(stripped)
        return int(stripped)
    except ValueError:
        return stripped


def read_sniper_stats(path: str | Path) -> dict[str, Any]:
    stats_path = Path(path)
    if stats_path.is_dir():
        stats_path = stats_path / "simulation" / "sim.stats"
    rows: dict[str, Any] = {}
    if not stats_path.exists():
        return {"success": False, "error": f"stats file not found: {stats_path}"}
    for line in stats_path.read_text(errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        rows[key.strip()] = parse_value(value)
    rows["success"] = True
    rows["stats_path"] = str(stats_path)
    return rows


def extract_graphbrew_metrics(rows: dict[str, Any]) -> dict[str, Any]:
    instructions = rows.get("core.instructions", 0)
    # barrier.global_time is the whole-run wall clock reported once (on core 0; the
    # other cores report 0), so its summed value stays correct under multi-core.
    # performance_model.elapsed_time is replicated per core and would inflate the
    # denominator. IPC = total instructions (summed) / global wall clock = a
    # system-wide throughput smoke field.
    cycles = rows.get("barrier.global_time", rows.get("performance_model.elapsed_time", 0))
    ipc = 0.0
    try:
        if cycles:
            # Sniper times are femtoseconds in many stats; use raw ratio only as a smoke field.
            ipc = float(instructions) / float(cycles)
    except (TypeError, ValueError):
        ipc = 0.0

    def first_value(*keys: str) -> Any:
        for key in keys:
            value = rows.get(key)
            if value not in (None, ""):
                return value
        return 0

    def numeric_value(key: str) -> float:
        value = rows.get(key, 0)
        try:
            return float(value)
        except (TypeError, ValueError):
            return 0.0

    def sum_values(*keys: str) -> float:
        return sum(numeric_value(key) for key in keys)

    def sum_prefix(prefix: str) -> float:
        return sum(
            float(value)
            for key, value in rows.items()
            if key.startswith(prefix) and isinstance(value, (int, float))
        )

    return {
        "success": bool(rows.get("success")),
        "stats_path": rows.get("stats_path", ""),
        "instructions": instructions,
        "cycles_or_time": cycles,
        "ipc_raw": ipc,
        "l1d_loads": first_value("L1-D.loads-data", "L1-D.loads", "L1-D.load"),
        "l1d_load_misses": first_value("L1-D.load-misses-data", "L1-D.load-misses", "L1-D.misses"),
        "l2_loads": first_value("L2.loads-data", "L2.loads", "L2.load"),
        "l2_load_misses": first_value("L2.load-misses-data", "L2.load-misses", "L2.misses"),
        "llc_loads": first_value("nuca-cache.reads", "nuca-cache.loads-data", "L3.loads-data", "L3.loads"),
        "llc_load_misses": first_value("nuca-cache.read-misses", "nuca-cache.load-misses-data", "L3.load-misses-data", "L3.misses"),
        "pf_issued": first_value("L2.prefetches", "L1-D.prefetches"),
        "pf_fillups": first_value("L2.prefetches-fillup", "L1-D.prefetches-fillup"),
        "pf_useful": first_value("L2.hits-prefetch", "L1-D.hits-prefetch"),
        "pf_evicted_before_use": first_value("L2.evict-prefetch", "L1-D.evict-prefetch"),
        "pf_invalidated_before_use": first_value("L2.invalidate-prefetch", "L1-D.invalidate-prefetch"),
        # DRAM-level demand request count.
        # l3_miss_rate is unsafe with prefetchers + mask-charged
        # accesses; demand-DRAM traffic is the metric §4.3 of the
        # reference model reports. Aggregated across all dram-bank-group-*.
        "dram_demand_requests": sum(
            float(value) for key, value in rows.items()
            if key.endswith(".num-requests")
            and key.startswith("dram-bank-group-")
            and isinstance(value, (int, float))
        ),
        "droplet_sideband_loaded": first_value("droplet-prefetcher.sideband-loaded"),
        "droplet_edge_accesses": first_value("droplet-prefetcher.edge-accesses"),
        "droplet_stride_issued": first_value("droplet-prefetcher.stride-issued"),
        "droplet_indirect_issued": first_value("droplet-prefetcher.indirect-issued"),
        "droplet_duplicate_skips": first_value("droplet-prefetcher.duplicate-skips"),
        "ecg_pfx_sideband_loaded": first_value("ecg-pfx-prefetcher.sideband-loaded"),
        "ecg_pfx_target_hints_seen": first_value("ecg-pfx-prefetcher.target-hints-seen"),
        "ecg_pfx_issued": first_value("ecg-pfx-prefetcher.issued"),
        "ecg_pfx_duplicate_skips": first_value("ecg-pfx-prefetcher.duplicate-skips"),
        "ecg_pfx_no_sideband": first_value("ecg-pfx-prefetcher.no-sideband"),
        "ecg_pfx_invalid_target": first_value("ecg-pfx-prefetcher.invalid-target"),
        "sniper_cpi_base": first_value("rob_timer.cpiBase"),
        "sniper_cpi_branch": first_value("rob_timer.cpiBranchPredictor"),
        "sniper_cpi_data_cache": sum_prefix("rob_timer.cpiDataCache"),
        "sniper_cpi_data_l1": sum_values("rob_timer.cpiDataCacheL1", "rob_timer.cpiDataCacheL1_S", "rob_timer.cpiDataCacheL1I"),
        "sniper_cpi_data_l2": sum_values("rob_timer.cpiDataCacheL2", "rob_timer.cpiDataCacheL2_S"),
        "sniper_cpi_data_llc": sum_values("rob_timer.cpiDataCacheL3", "rob_timer.cpiDataCacheL3_S", "rob_timer.cpiDataCacheL4", "rob_timer.cpiDataCacheL4_S", "rob_timer.cpiDataCachenuca-cache"),
        "sniper_cpi_data_dram": sum_values("rob_timer.cpiDataCachedram", "rob_timer.cpiDataCachedram-local", "rob_timer.cpiDataCachedram-remote"),
        "sniper_cpi_sync": sum_prefix("performance_model.cpiSync"),
        "sniper_cpi_unknown": sum_values("performance_model.cpiUnknown", "rob_timer.cpiDataCacheunknown", "rob_timer.cpiInstructionCacheunknown"),
        "sniper_nonidle_elapsed_time": first_value("performance_model.nonidle_elapsed_time"),
        "sniper_idle_elapsed_time": first_value("performance_model.idle_elapsed_time"),
        "sniper_elapsed_time": first_value("performance_model.elapsed_time"),
    }


def stats_summary(metrics: dict[str, Any]) -> str:
    return "\n".join([
        "Sniper Simulation Results:",
        f"  stats: {metrics.get('stats_path', '')}",
        f"  instructions: {metrics.get('instructions', 0)}",
        f"  cycles_or_time: {metrics.get('cycles_or_time', 0)}",
        f"  ipc_raw: {metrics.get('ipc_raw', 0.0):.6f}",
        f"  L1D loads/misses: {metrics.get('l1d_loads', 0)} / {metrics.get('l1d_load_misses', 0)}",
        f"  L2 loads/misses: {metrics.get('l2_loads', 0)} / {metrics.get('l2_load_misses', 0)}",
        f"  LLC loads/misses: {metrics.get('llc_loads', 0)} / {metrics.get('llc_load_misses', 0)}",
        f"  prefetch issued/fill/useful: {metrics.get('pf_issued', 0)} / {metrics.get('pf_fillups', 0)} / {metrics.get('pf_useful', 0)}",
        f"  DROPLET sideband/edge/indirect: {metrics.get('droplet_sideband_loaded', 0)} / {metrics.get('droplet_edge_accesses', 0)} / {metrics.get('droplet_indirect_issued', 0)}",
        f"  CPI base/data-cache/branch: {metrics.get('sniper_cpi_base', 0)} / {metrics.get('sniper_cpi_data_cache', 0)} / {metrics.get('sniper_cpi_branch', 0)}",
    ])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Parse Sniper sim.stats for GraphBrew smoke metrics.")
    parser.add_argument("path", help="Sniper output directory or simulation/sim.stats path.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = read_sniper_stats(args.path)
    if not rows.get("success"):
        print(f"Error: {rows.get('error')}")
        return 1
    print(stats_summary(extract_graphbrew_metrics(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
