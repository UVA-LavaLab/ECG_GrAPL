#!/usr/bin/env python3
"""
gem5 stats.txt parser for GraphBrew.

Parses gem5 output statistics to extract cache miss rates and other
performance metrics, converting them to the same CacheResult format
used by the standalone cache simulator pipeline.

Usage:
    from parse_stats import parse_gem5_stats

    result = parse_gem5_stats("m5out/stats.txt")
    print(f"L1 miss rate: {result['l1_miss_rate']:.4f}")
    print(f"L3 miss rate: {result['l3_miss_rate']:.4f}")
    print(f"IPC: {result['ipc']:.2f}")
"""

import re
from pathlib import Path
from typing import Dict, Optional


def parse_gem5_stats(stats_path: str) -> Dict:
    """Parse gem5 stats.txt file for cache and CPU metrics.

    Extracts:
        - L1 data cache: overall miss rate, total misses, total accesses
        - L2 cache: overall miss rate, total misses, total accesses
        - L3 cache: overall miss rate, total misses, total accesses
        - CPU: IPC, total cycles, total instructions

    Args:
        stats_path: Path to gem5 stats.txt file.

    Returns:
        Dict with extracted metrics:
        {
            "l1_miss_rate": float,
            "l2_miss_rate": float,
            "l3_miss_rate": float,
            "l1_misses": int,
            "l2_misses": int,
            "l3_misses": int,
            "l1_accesses": int,
            "l2_accesses": int,
            "l3_accesses": int,
            "ipc": float,
            "cycles": int,
            "instructions": int,
            "success": bool,
            "error": str,
        }
    """
    result = {
        "l1_miss_rate": 0.0, "l2_miss_rate": 0.0, "l3_miss_rate": 0.0,
        "l1_misses": 0, "l2_misses": 0, "l3_misses": 0,
        "l1_accesses": 0, "l2_accesses": 0, "l3_accesses": 0,
        "ipc": 0.0, "cycles": 0, "instructions": 0,
        "success": False, "error": "",
    }

    path = Path(stats_path)
    if not path.exists():
        result["error"] = f"Stats file not found: {stats_path}"
        return result

    content = path.read_text()

    # ── Cache miss rates ──
    # gem5 format: system.cpu.dcache.overallMissRate::total    0.123456
    patterns = {
        "l1_miss_rate": [
            r"system\.cpu\.dcache\.overallMissRate::total\s+([0-9.]+)",
            r"system\.cpu\.dcache\.overall_miss_rate::total\s+([0-9.]+)",
        ],
        "l2_miss_rate": [
            r"system\.l2cache\.overallMissRate::total\s+([0-9.]+)",
            r"system\.l2cache\.overall_miss_rate::total\s+([0-9.]+)",
        ],
        "l3_miss_rate": [
            r"system\.l3cache\.overallMissRate::total\s+([0-9.]+)",
            r"system\.l3cache\.overall_miss_rate::total\s+([0-9.]+)",
        ],
    }

    for key, pats in patterns.items():
        for pat in pats:
            m = re.search(pat, content)
            if m:
                result[key] = float(m.group(1))
                break

    # ── Cache miss counts ──
    miss_patterns = {
        "l1_misses": [
            r"system\.cpu\.dcache\.overallMisses::total\s+(\d+)",
            r"system\.cpu\.dcache\.overall_misses::total\s+(\d+)",
        ],
        "l2_misses": [
            r"system\.l2cache\.overallMisses::total\s+(\d+)",
            r"system\.l2cache\.overall_misses::total\s+(\d+)",
        ],
        "l3_misses": [
            r"system\.l3cache\.overallMisses::total\s+(\d+)",
            r"system\.l3cache\.overall_misses::total\s+(\d+)",
        ],
    }

    for key, pats in miss_patterns.items():
        for pat in pats:
            m = re.search(pat, content)
            if m:
                result[key] = int(m.group(1))
                break

    # ── Cache access counts ──
    access_patterns = {
        "l1_accesses": [
            r"system\.cpu\.dcache\.overallAccesses::total\s+(\d+)",
            r"system\.cpu\.dcache\.overall_accesses::total\s+(\d+)",
        ],
        "l2_accesses": [
            r"system\.l2cache\.overallAccesses::total\s+(\d+)",
            r"system\.l2cache\.overall_accesses::total\s+(\d+)",
        ],
        "l3_accesses": [
            r"system\.l3cache\.overallAccesses::total\s+(\d+)",
            r"system\.l3cache\.overall_accesses::total\s+(\d+)",
        ],
    }

    for key, pats in access_patterns.items():
        for pat in pats:
            m = re.search(pat, content)
            if m:
                result[key] = int(m.group(1))
                break

    # ── CPU IPC ──
    ipc_patterns = [
        r"system\.cpu\.ipc\s+([0-9.]+)",
        r"system\.cpu\.ipc_total\s+([0-9.]+)",
    ]
    for pat in ipc_patterns:
        m = re.search(pat, content)
        if m:
            result["ipc"] = float(m.group(1))
            break

    # ── CPU cycles and instructions ──
    cycle_m = re.search(r"system\.cpu\.numCycles\s+(\d+)", content)
    if cycle_m:
        result["cycles"] = int(cycle_m.group(1))

    inst_m = re.search(r"system\.cpu\.committedInsts\s+(\d+)", content)
    if inst_m:
        result["instructions"] = int(inst_m.group(1))

    # Validate: at least some data was found
    if any(result[k] > 0 for k in ["l1_accesses", "l2_accesses", "l3_accesses",
                                     "cycles", "instructions"]):
        result["success"] = True
    else:
        result["error"] = "No cache or CPU statistics found in stats.txt"

    return result


def stats_summary(stats: Dict) -> str:
    """Format parsed stats as human-readable summary."""
    lines = [
        "gem5 Simulation Results:",
        f"  L1D miss rate: {stats['l1_miss_rate']*100:.2f}%"
        f"  ({stats['l1_misses']:,} misses / {stats['l1_accesses']:,} accesses)",
        f"  L2  miss rate: {stats['l2_miss_rate']*100:.2f}%"
        f"  ({stats['l2_misses']:,} misses / {stats['l2_accesses']:,} accesses)",
        f"  L3  miss rate: {stats['l3_miss_rate']*100:.2f}%"
        f"  ({stats['l3_misses']:,} misses / {stats['l3_accesses']:,} accesses)",
        f"  IPC: {stats['ipc']:.4f}"
        f"  ({stats['instructions']:,} insts / {stats['cycles']:,} cycles)",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: parse_stats.py <stats.txt>")
        sys.exit(1)

    stats = parse_gem5_stats(sys.argv[1])
    if stats["success"]:
        print(stats_summary(stats))
    else:
        print(f"Error: {stats['error']}")
        sys.exit(1)
