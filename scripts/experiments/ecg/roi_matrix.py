#!/usr/bin/env python3
"""
ROI-scoped cache_sim/gem5 policy matrix for ECG validation.

This runner is intentionally small and explicit. It compares the fast,
accurate cache simulator against ROI-scoped gem5 runs at the same policy scope:
L1/L2 stay LRU and the tested replacement policy is applied to L3.

Default workload is the synthetic PR stress point used during validation:
    -g 10 -k 16 -o 5 -n 1 -i 5

Examples:
    python3 scripts/experiments/ecg/roi_matrix.py --dry-run

    python3 scripts/experiments/ecg/roi_matrix.py \
        --suite cache-sim --policies LRU GRASP POPT ECG:POPT_PRIMARY

    python3 scripts/experiments/ecg/roi_matrix.py \
        --suite gem5 --policies LRU GRASP POPT ECG:POPT_PRIMARY \
        --l3-sizes 32kB
"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import csv
import fcntl
import functools
import hashlib
import json
import math
import mmap
import os
import platform
import re
import signal
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[3]
RESULTS_ROOT = PROJECT_ROOT / "results" / "ecg_experiments" / "roi_matrix"
REUSE_PLAN_SIDECAR_ROOT = (
    PROJECT_ROOT / "results" / "ecg_experiments" /
    "reuse_plan_sidecars")
REUSE_PLAN_SIDECAR_TOOL = (
    PROJECT_ROOT / "bench" / "bin_sim" / "reuse_plan_sidecar")
REUSE_PLAN_PR_VERTICES_PER_LINE = 16  # 64-byte line / 4-byte float.
MIN_SNIPER_REUSE_PLAN_CERT_RECEIPTS = 32
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gem5_guest_receipt import (  # noqa: E402
    immutable_fuse_files,
    open_sealed_guest,
    stage_validated_guest,
    validate_receipt as validate_gem5_guest_receipt,
    verify_staged_guest,
)
from path_fingerprints import hash_path as hash_input_path  # noqa: E402
from policy_specs import (  # noqa: E402
    ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    ONLINE_DUELING_WINDOW_MISSES,
    SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    PolicySpec,
    parse_policy_spec,
)

_GEM5_OPT = Path(os.environ.get(
    "GEM5_OPT",
    PROJECT_ROOT / "bench" / "include" / "gem5_sim" / "gem5" / "build" / "X86" / "gem5.opt",
))
GEM5_OPT = (
    _GEM5_OPT if _GEM5_OPT.is_absolute()
    else (PROJECT_ROOT / _GEM5_OPT).resolve())
# Override the kernel binary suffix to select RISC-V guests. The default
# remains the native X86 m5ops build.
GEM5_KERNEL_SUFFIX = os.environ.get("GEM5_KERNEL_SUFFIX", "_m5ops")
GEM5_CONFIG = PROJECT_ROOT / "bench" / "include" / "gem5_sim" / "configs" / "graphbrew" / "graph_se.py"
VALIDATED_GEM5_GUEST: Path | None = None
VALIDATED_GEM5_GUEST_SHA256 = ""
PLANNING_MISSING_GEM5_GUEST_SHA256 = (
    "planning-missing-gem5-guest-sha256")


def fixed_runtime_mount_name(
        kind: str, pid: int | None = None,
        timestamp_ns: int | None = None) -> str:
    if kind not in ("runtime", "config"):
        raise ValueError(f"unsupported mount kind: {kind}")
    actual_pid = os.getpid() if pid is None else pid
    actual_ns = time.time_ns() if timestamp_ns is None else timestamp_ns
    return (
        f".gem5-{kind}-{actual_pid:010d}-{actual_ns:019d}")


@functools.lru_cache(maxsize=None)
def cached_file_sha256(path_text: str) -> str:
    path = Path(path_text)
    if not path.exists() or not path.is_file():
        return ""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reuse_plan_sidecar_options(options: str) -> list[str]:
    parts = shlex.split(options)
    filtered: list[str] = []
    index = 0
    while index < len(parts):
        part = parts[index]
        if part in ("-i", "-t") and index + 1 < len(parts):
            index += 2
            continue
        if ((part.startswith("-i") or part.startswith("-t")) and
                part[2:].replace(".", "", 1).isdigit()):
            index += 1
            continue
        filtered.append(part)
        index += 1
    return filtered


def ensure_reuse_plan_sidecar(
        args: argparse.Namespace, env: dict[str, str],
        record_bytes: int) -> Path:
    if args.benchmark != "pr":
        raise RuntimeError(
            "host-generated ReusePlan sidecars are currently implemented "
            "only for gem5 PageRank")
    if record_bytes not in (4, 8):
        raise RuntimeError(
            f"unsupported ReusePlan sidecar width: {record_bytes}")
    if not REUSE_PLAN_SIDECAR_TOOL.is_file():
        raise RuntimeError(
            f"missing ReusePlan sidecar generator: "
            f"{REUSE_PLAN_SIDECAR_TOOL}; run make sim-reuse_plan_sidecar")
    graph_fingerprint = str(args.expected_graph_sha256 or "")
    if not graph_fingerprint:
        options = shlex.split(args.options)
        try:
            graph_path = Path(options[options.index("-f") + 1])
        except (ValueError, IndexError):
            graph_path = Path()
        graph_fingerprint = (
            hash_input_path(graph_path)
            if graph_path and graph_path.is_file()
            else hashlib.sha256(
                json.dumps(
                    reuse_plan_sidecar_options(args.options),
                    separators=(",", ":")).encode()).hexdigest())
    hot_fraction = env.get("GRASP_HOT_FRACTION", "0.15")
    key = {
        "schema": 1,
        "generator_sha256": cached_file_sha256(
            str(REUSE_PLAN_SIDECAR_TOOL)),
        "graph": graph_fingerprint,
        "options": reuse_plan_sidecar_options(args.options),
        "record_bytes": record_bytes,
        "epochs": int(args.ecg_epochs),
        "vertices_per_line": REUSE_PLAN_PR_VERTICES_PER_LINE,
        "linemin": 1,
        "push_out_edges": 0,
        "hot_fraction": hot_fraction,
    }
    digest = hashlib.sha256(json.dumps(
        key, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    sidecar = REUSE_PLAN_SIDECAR_ROOT / f"{digest}.bin"
    lock_path = REUSE_PLAN_SIDECAR_ROOT / f"{digest}.lock"
    if args.dry_run:
        return sidecar
    REUSE_PLAN_SIDECAR_ROOT.mkdir(parents=True, exist_ok=True)
    generator_env = dict(env)
    generator_env.update({
        "ECG_REUSE_PLAN_SIDECAR": str(sidecar),
        "ECG_REUSE_PLAN_SIDECAR_RECORD_BYTES": str(record_bytes),
        "ECG_REUSE_PLAN_SIDECAR_EPOCHS": str(int(args.ecg_epochs)),
        "ECG_REUSE_PLAN_SIDECAR_VPL":
            str(REUSE_PLAN_PR_VERTICES_PER_LINE),
        "ECG_REUSE_PLAN_SIDECAR_LINEMIN": "1",
        "ECG_REUSE_PLAN_SIDECAR_PUSH": "0",
    })
    command = [
        str(REUSE_PLAN_SIDECAR_TOOL),
        *reuse_plan_sidecar_options(args.options),
    ]
    with lock_path.open("w") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        if sidecar.is_file():
            return sidecar
        result = subprocess.run(
            command, cwd=PROJECT_ROOT, env=generator_env,
            capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(
                "ReusePlan sidecar generation failed:\n" +
                (result.stdout + result.stderr)[-4000:])
    return sidecar


def gem5_sideband_paths(gem5_out: Path) -> dict[str, Path]:
    # FIXED-LENGTH sideband directory, independent of the policy-named gem5_out.
    # The sideband file paths are read by the benchmark as env strings and written
    # into the ctx JSON (data_path), so they live in the benchmark's heap. If the
    # path length varies by policy (because gem5_out embeds the policy name), the
    # heap allocation shifts and changes the cache-line/set alignment of the graph
    # and property arrays -- which, at the tiny ROI cache sizes, swings L1 misses
    # and IPC by up to ~30% and confounds the per-policy comparison. A constant-
    # length hashed directory keeps per-cell isolation while making every policy's
    # sideband paths identical length (only the hex characters differ, never the
    # length), so the benchmark heap layout is policy-independent.
    digest = hashlib.sha1(str(gem5_out).encode("utf-8")).hexdigest()[:16]
    sideband_dir = Path(tempfile.gettempdir()) / f"gbsb_{digest}"
    return {
        "context": sideband_dir / "gem5_graphbrew_ctx.json",
        "popt_matrix": sideband_dir / "gem5_popt_matrix.bin",
        "out_edges": sideband_dir / "gem5_graphbrew_out_edges.bin",
        "in_edges": sideband_dir / "gem5_graphbrew_in_edges.bin",
    }


DEFAULT_SNIPER_ROOT = Path("bench") / "include" / "sniper_sim" / "snipersim"
SNIPER_OVERLAY_STATUS = PROJECT_ROOT / "bench" / "include" / "sniper_sim" / ".sniper_overlays.json"
PINNED_SNIPER_HEAD = "56505e42fd98bca863fac181e769bd3c98d2bb33"
SNIPER_STATS_DIR = PROJECT_ROOT / "bench" / "include" / "sniper_sim" / "scripts"
if str(SNIPER_STATS_DIR) not in sys.path:
    sys.path.insert(0, str(SNIPER_STATS_DIR))
from parse_stats import extract_graphbrew_metrics, read_sniper_stats


def project_relative_path(value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else PROJECT_ROOT / path


def sniper_root_path(args: argparse.Namespace) -> Path:
    return project_relative_path(args.sniper_root)


def sniper_runner_path(args: argparse.Namespace) -> Path:
    return sniper_root_path(args) / "run-sniper"

DEFAULT_POLICIES = [
    "LRU", "SRRIP", "GRASP", "POPT", "POPT:UNCHARGED",
    "ECG:DBG_ONLY", "ECG:DBG_PRIMARY_CHARGED", "ECG:DBG_PRIMARY",
    "ECG:POPT_PRIMARY", "ECG:ECG_GRASP_POPT",
]
SNIPER_DEFAULT_POLICIES = ["LRU", "SRRIP"]
SNIPER_POLICY_MAP = {
    "LRU": "lru",
    "SRRIP": "srrip",
}
SNIPER_GRAPH_POLICY_MAP = {
    "GRASP": "grasp",
    "POPT": "popt",
    "ECG": "ecg",
}
ALL_POLICIES = [
    "LRU",
    "SRRIP",
    "GRASP",
    "POPT",
    "POPT:UNCHARGED",
    "ECG:DBG_ONLY",
    "ECG:DBG_PRIMARY_CHARGED",
    "ECG:DBG_PRIMARY",
    "ECG:POPT_TIE",
    "ECG:POPT_PRIMARY",
    "ECG:ECG_GRASP_POPT",
    "ECG:ECG_EMBEDDED",
    "ECG:ECG_EPOCH_EMBEDDED",
    "ECG:ECG_COMBINED",
]

GEM5_STAT_KEYS = {
    "sim_ticks": "simTicks",
    "ipc": "system.cpu.ipc",
    "l1_miss_rate": "system.cpu.dcache.overallMissRate::total",
    "l2_miss_rate": "system.l2cache.overallMissRate::total",
    "l3_miss_rate": "system.l3cache.overallMissRate::total",
    "l1_misses": "system.cpu.dcache.overallMisses::total",
    "l2_misses": "system.l2cache.overallMisses::total",
    "l3_misses": "system.l3cache.overallMisses::total",
    "l1_accesses": "system.cpu.dcache.overallAccesses::total",
    "l2_accesses": "system.l2cache.overallAccesses::total",
    "l3_accesses": "system.l3cache.overallAccesses::total",
    "grasp_hot_property_accesses":
        "system.l3cache.replacement_policy.hotPropertyAccesses",
    "popt_roi_rereference_queries":
        "system.l3cache.replacement_policy.rereferenceQueries",
    "gem5_reuse_plan_dueling_request_bound_victims":
        "system.l3cache.replacement_policy.requestBoundVictims",
    "gem5_reuse_plan_dueling_leader_samples":
        "system.l3cache.replacement_policy.leaderSamples",
    "gem5_reuse_plan_dueling_follower_selections":
        "system.l3cache.replacement_policy.followerSelections",
    "gem5_reuse_plan_dueling_completed_windows":
        "system.l3cache.replacement_policy.completedWindows",
    "gem5_reuse_plan_dueling_winner_changes":
        "system.l3cache.replacement_policy.winnerChanges",
    "gem5_reuse_plan_dueling_follower_variant_overrides":
        "system.l3cache.replacement_policy.followerVariantOverrides",
    # Demand-load (cpu.data) L3 stats EXCLUDING prefetcher fills. The L2 stream
    # prefetcher otherwise dominates overall::total (>>demand). Sniper's NUCA
    # counters do not provide this split, so the pipeline treats its prefetched
    # rows as total LLC-read traffic rather than demand-miss evidence.
    "l3_data_misses": "system.l3cache.overallMisses::cpu.data",
    "l3_data_hits": "system.l3cache.overallHits::cpu.data",
    "l3_prefetch_misses":
        "system.l3cache.overallMisses::l2cache.prefetcher",
    "l3_prefetch_hits":
        "system.l3cache.overallHits::l2cache.prefetcher",
    "l3_prefetch_accesses":
        "system.l3cache.overallAccesses::l2cache.prefetcher",
    "dram_read_bytes": "system.mem_ctrl.dram.bytesRead::total",
    "dram_write_bytes": "system.mem_ctrl.dram.bytesWritten::total",
    "dram_read_requests": "system.mem_ctrl.dram.numReads::total",
    "dram_write_requests": "system.mem_ctrl.dram.numWrites::total",
    "dram_prefetch_read_bytes":
        "system.mem_ctrl.dram.bytesRead::l2cache.prefetcher",
    # Bandwidth SATURATION. ReusePlan trades bandwidth for exposed latency: it can use
    # more total traffic while exposing far fewer demand misses to full DRAM
    # latency. Which side binds depends entirely on whether the memory system is
    # saturated, so utilisation must be reported alongside execution time rather
    # than inferred. cache_sim cannot produce these at all.
    "dram_bus_util_pct": "system.mem_ctrl.dram.busUtil",
    "dram_bus_util_read_pct": "system.mem_ctrl.dram.busUtilRead",
    "dram_bus_util_write_pct": "system.mem_ctrl.dram.busUtilWrite",
    "dram_peak_bw_mibs": "system.mem_ctrl.dram.peakBW",
    "dram_avg_read_bw_mibs": "system.mem_ctrl.dram.avgRdBW",
    "dram_avg_write_bw_mibs": "system.mem_ctrl.dram.avgWrBW",
    "dram_bw_total_bytes_per_s": "system.mem_ctrl.dram.bwTotal::total",
    # ROI-SCOPED instruction count. simInsts is NOT cleared by m5_reset_stats
    # (it keeps counting from boot, so it includes graph loading and metadata
    # construction), which makes it useless for attributing ROI cost -- and
    # actively misleading, since it implies an IPC above 1 on an in-order
    # TimingSimpleCPU. commitStats0.numInsts IS reset, so it measures the
    # reported region. Needed because a software-decoded record can
    # cost more instructions than the memory traffic it saves.
    "roi_insts": "system.cpu.commitStats0.numInsts",
    "roi_cycles": "system.cpu.numCycles",
    "roi_cpi": "system.cpu.commitStats0.cpi",
}

GEM5_PREFETCH_STAT_KEYS = {
    "pf_issued": "pfIssued",
    "pf_useful": "pfUseful",
    "pf_useful_but_miss": "pfUsefulButMiss",
    "pf_unused": "pfUnused",
    "pf_late": "pfLate",
    "pf_identified": "pfIdentified",
    "pf_hit_in_cache": "pfHitInCache",
    "pf_hit_in_mshr": "pfHitInMSHR",
    "pf_hit_in_wb": "pfHitInWB",
    "pf_in_cache": "pfInCache",
    "pf_removed_demand": "pfRemovedDemand",
    "pf_removed_full": "pfRemovedFull",
    "pf_span_page": "pfSpanPage",
    "pf_useful_span_page": "pfUsefulSpanPage",
}

ECG_PFX_MODE_VALUES = {
    "degree": "1",
    "popt": "2",
    "droplet": "3",  # DROPLET-style: sequential prefetch (no target selection)
    "far_future": "4",  # FAR-FUTURE: target from global hot_table (not v.s neighbors)
    "per_edge": "6",
    "cross_iter": "7",
    "1": "1",
    "2": "2",
    "3": "3",
    "4": "4",
    "6": "6",
    "7": "7",
}


def ecg_pfx_env(args: argparse.Namespace) -> dict[str, str]:
    # For cache_sim, DROPLET maps to ECG_PREFETCH_MODE=3 (sequential
    # lookahead, no target selection — faithful comparator for the
    # ECG_PFX claim). The Sniper/gem5 DROPLET overlays use a separate
    # path (perf_model/.../prefetcher/droplet), so this only affects
    # cache_sim env.
    if args.prefetcher == "DROPLET":
        return {
            "ECG_PREFETCH_MODE": "3",
            "ECG_PREFETCH_WINDOW": str(args.ecg_pfx_window),
            "ECG_PREFETCH_LOOKAHEAD": str(args.ecg_pfx_lookahead),
        }
    if args.prefetcher != "ECG_PFX":
        return {}
    return {
        "ECG_PREFETCH_MODE": ECG_PFX_MODE_VALUES[str(args.ecg_pfx_mode)],
        "ECG_PREFETCH_WINDOW": str(args.ecg_pfx_window),
        "ECG_PREFETCH_LOOKAHEAD": str(args.ecg_pfx_lookahead),
    }


def effective_ecg_pfx_value(args: argparse.Namespace, name: str) -> str:
    return ecg_pfx_env(args).get(name, os.environ.get(name, ""))


def parse_gem5_number(text: str) -> int | float:
    return float(text) if "." in text else int(text)


def parse_size_bytes(size: str | int) -> int:
    if isinstance(size, int):
        return size
    text = str(size).strip()
    match = re.fullmatch(r"([0-9]+)\s*([A-Za-z]*)", text)
    if not match:
        raise ValueError(f"invalid size: {size!r}")
    value = int(match.group(1))
    suffix = match.group(2).lower()
    if suffix in ("", "b"):
        return value
    if suffix in ("k", "kb", "kib"):
        return value * 1024
    if suffix in ("m", "mb", "mib"):
        return value * 1024 * 1024
    if suffix in ("g", "gb", "gib"):
        return value * 1024 * 1024 * 1024
    raise ValueError(f"invalid size suffix in {size!r}")


def format_size_bytes(size_bytes: int) -> str:
    return f"{int(size_bytes)}B"


def format_sniper_kb(size: str | int) -> int:
    size_bytes = parse_size_bytes(size)
    if size_bytes % 1024 != 0:
        raise ValueError(f"Sniper cache sizes must be whole KiB values, got {size!r}")
    return max(size_bytes // 1024, 1)


def sniper_l3_geometry(args: argparse.Namespace, l3_size: str, charge: dict[str, Any]) -> tuple[int, str, int]:
    line_size = parse_size_bytes(args.line_size)
    requested_bytes = parse_size_bytes(l3_size)
    requested_ways = max(int(args.l3_ways), 1)
    requested_sets = max(requested_bytes // (requested_ways * line_size), 1)
    desired_ways = max(int(charge["popt_effective_l3_ways"]), 1)

    # Sniper's cache_size is in integer KiB and its cache constructor requires
    # size == sets * ways * line_size. Charged P-OPT can produce fractional-KiB
    # effective sizes on tiny LLCs, so round data ways down to the nearest valid
    # geometry. This is conservative for charged policies and leaves uncharged
    # whole-KiB configurations unchanged.
    configured_ways = desired_ways
    while configured_ways > 1:
        configured_bytes = requested_sets * configured_ways * line_size
        if configured_bytes % 1024 == 0:
            return configured_bytes // 1024, str(configured_ways), configured_bytes
        configured_ways -= 1
    configured_bytes = requested_sets * configured_ways * line_size
    configured_kb = max(configured_bytes // 1024, 1)
    return configured_kb, str(configured_ways), configured_bytes


def numeric(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def miss_rate(misses: Any, accesses: Any) -> float | None:
    miss_count = numeric(misses)
    access_count = numeric(accesses)
    if miss_count is None or not access_count:
        return None
    return miss_count / access_count


def estimate_num_iterations(options: str) -> int | None:
    parts = shlex.split(options)
    for index, part in enumerate(parts):
        if part == "-i" and index + 1 < len(parts):
            return int(parts[index + 1])
        if part.startswith("-i") and part[2:].isdigit():
            return int(part[2:])
    return None


def apply_overhead_metrics(row: dict[str, Any]) -> None:
    """Charge P-OPT's rereference-matrix stream without double-counting.

    Two accounting modes exist and must never both apply:

    * ``simulated`` -- cache_sim issued the column stream as real accesses
      (``POPT_MATRIX_STREAM_SIM=1``), so it is already inside ``l3_misses`` and
      ``total_memory_traffic``. Adding the flat charge here would double-count.
    * ``analytic`` -- the stream was not simulated, so each PageRank iteration
      is charged post hoc while target-time stream latency remains omitted.
    * ``analytic_prefetch_upper_bound`` -- the same byte charge under a common
      prefetcher, explicitly treating P-OPT's matrix latency as perfectly
      hidden. This is a P-OPT-favorable sensitivity, not stream simulation.

    The analytic mode is only symmetric with ReusePlan when no prefetcher is active.
    ReusePlan's edge records are simulated accesses a structure prefetcher covers,
    while a flat charge can never be covered, so under a prefetcher the analytic
    mode penalises P-OPT with demand misses a real prefetcher removes. Measured
    on web-Google PageRank under STRIDE8, simulating the stream costs 0 extra
    demand misses and ~230k extra prefetch fills, against the flat charge's
    229,108 demand misses. Traffic is materially identical either way.
    """
    simulated = int(row.get("popt_matrix_stream_lines_simulated") or 0)
    charged = row.get("popt_overhead_charged") in (1, "1", True, "true")
    requested = row.get("popt_matrix_stream_requested") or "analytic"
    if charged and requested == "simulated" and simulated <= 0:
        # Fail closed. Silently falling back to the analytic charge here would
        # reintroduce exactly the asymmetry this mode exists to remove, and the
        # row would look legitimate. Only cache_sim implements the stream.
        row["status"] = "error"
        row["error"] = (
            "--popt-matrix-stream simulated was requested but no matrix-stream "
            "lines were observed; the stream is implemented in cache_sim only")
        return
    iteration_value = (
        row.get("pr_iterations") or
        estimate_num_iterations(str(row.get("options", ""))))
    if charged and iteration_value in (None, "") and simulated <= 0:
        mark_row_error(
            row,
            "charged P-OPT row has no PageRank iteration count")
    iterations = max(int(iteration_value or 1), 1)
    row["popt_matrix_stream_iterations"] = iterations if charged else 0
    if simulated > 0:
        row["popt_matrix_stream_mode"] = "simulated"
        stream_lines = 0
    else:
        if charged and requested == "analytic_prefetch_upper_bound":
            row["popt_matrix_stream_mode"] = (
                "analytic_cumulative_prefetch_upper_bound")
        else:
            row["popt_matrix_stream_mode"] = (
                "analytic_cumulative" if charged else "none")
        stream_lines = (
            int(row.get("popt_matrix_stream_cache_lines") or 0) * iterations
            if charged else 0)
    line_size = parse_size_bytes(str(row.get("line_size") or "64"))
    stream_bytes = stream_lines * line_size
    row["popt_cumulative_stream_bytes"] = stream_bytes
    row["popt_matrix_stream_requests"] = stream_lines
    row["popt_matrix_stream_dram_bytes"] = stream_bytes
    row["popt_stream_requestor_dram_bytes"] = stream_bytes
    row["popt_target_time_charged"] = 0
    row["popt_timing_optimistic"] = int(charged and simulated <= 0)
    row["popt_prefetch_upper_bound"] = int(
        charged and requested == "analytic_prefetch_upper_bound")
    row["popt_reload_each_iteration"] = int(charged)
    row["popt_initial_columns_charged"] = int(charged)
    # The frozen primary metric on a timing backend: memory-controller bytes in
    # BOTH directions. gem5 reports reads and writes separately; combine them
    # once here so no downstream consumer has to remember to.
    rd = row.get("dram_read_bytes")
    wr = row.get("dram_write_bytes")
    if rd not in (None, "") and wr not in (None, ""):
        try:
            without_stream = int(float(rd)) + int(float(wr))
            row["popt_dram_offchip_bytes_without_matrix_stream"] = (
                without_stream)
            row["popt_nonstream_requestor_dram_bytes"] = without_stream
            row["dram_offchip_bytes"] = without_stream + stream_bytes
            row["popt_offchip_includes_matrix_stream"] = int(charged)
        except (TypeError, ValueError):
            pass
    l3_misses = row.get("l3_misses")
    if l3_misses not in (None, ""):
        row["l3_misses_with_overhead"] = int(l3_misses) + stream_lines
        if charged:
            row["popt_charged_l3_misses_plus_matrix_stream"] = (
                int(l3_misses) + stream_lines)
    traffic = row.get("total_memory_traffic")
    if traffic not in (None, ""):
        row["total_memory_traffic_with_overhead"] = (
            int(traffic) + stream_lines)
        if charged:
            row["popt_charged_total_memory_traffic"] = (
                int(traffic) + stream_lines)


def annotate_l3_pressure(row: dict[str, Any]) -> dict[str, Any]:
    """Flag cells where the L3 is not meaningfully exercised.

    When the property working set fits entirely in L2, the L3 sees only the
    cold-miss stream: every L3 access misses (miss_rate == 1.0, misses ==
    accesses) and ALL replacement policies produce identical L3 numbers. Such
    a cell carries no L3-policy signal, yet a naive reading of "L3 miss-rate =
    1.0000" looks like catastrophic thrash. Mark these so they are not mistaken
    for a real policy comparison. Suite-agnostic: works on any row that carries
    l3_misses / l3_accesses.
    """
    if str(row.get("status", "")) not in ("ok", "", "0"):
        return row
    misses = numeric(row.get("l3_misses"))
    accesses = numeric(row.get("l3_accesses"))
    rate = row.get("l3_miss_rate")
    positive_activity = (
        accesses is not None and math.isfinite(accesses) and accesses > 0)
    cold_only = (
        misses is not None
        and accesses is not None
        and accesses > 0
        and misses >= accesses
    ) or (rate is not None and float(rate) >= 0.9995 and accesses not in (None, 0))
    # cache_sim rows expose hits/misses but may leave l3_accesses None.
    if accesses in (None, 0):
        hits = numeric(row.get("l3_hits"))
        m = numeric(row.get("l3_misses"))
        if hits is not None and m is not None and (hits + m) > 0:
            positive_activity = True
            cold_only = hits == 0
    row["l3_exercised"] = bool(positive_activity and not cold_only)
    if cold_only:
        row["l3_pressure_note"] = (
            "L3 inert (cold-only: every access misses); property working set "
            "fits in L2 so the L3 replacement policy is not exercised at this "
            "cache geometry -- not a meaningful policy comparison."
        )
    elif not positive_activity:
        row["l3_pressure_note"] = (
            "L3 unobserved: no finite positive LLC activity was reported.")
    return row


def graph_vertices_from_sg(path: Path) -> int | None:
    try:
        data = path.read_bytes()[:17]
    except OSError:
        return None
    if len(data) < 17:
        return None
    # GAPBS serialized graph header: bool directed, int64 edges, int64 vertices.
    return int(struct.unpack_from("<q", data, 1 + 8)[0])


def graph_vertices_from_mtx(path: Path) -> int | None:
    try:
        with path.open("r", errors="ignore") as fh:
            for line in fh:
                stripped = line.strip()
                if not stripped or stripped.startswith("%"):
                    continue
                parts = stripped.split()
                if len(parts) >= 2:
                    return max(int(parts[0]), int(parts[1]))
    except (OSError, ValueError):
        return None
    return None


def estimate_num_vertices(options: str) -> int | None:
    parts = shlex.split(options)
    for index, part in enumerate(parts):
        if part == "-g" and index + 1 < len(parts):
            return 1 << int(parts[index + 1])
        match = re.fullmatch(r"-g([0-9]+)", part)
        if match:
            return 1 << int(match.group(1))
        if part == "-f" and index + 1 < len(parts):
            path = Path(parts[index + 1])
            if not path.is_absolute():
                path = PROJECT_ROOT / path
            suffix = path.suffix.lower()
            if suffix in (".sg", ".wsg"):
                return graph_vertices_from_sg(path)
            if suffix == ".mtx":
                return graph_vertices_from_mtx(path)
    return None


def popt_charge_metadata(args: argparse.Namespace, spec: PolicySpec, l3_size: str) -> dict[str, Any]:
    requested_bytes = parse_size_bytes(l3_size)
    metadata: dict[str, Any] = {
        "popt_overhead_charged": int(spec.charge_popt_overhead),
        "popt_reserve_model": getattr(args, "popt_reserve_model", "fixed_one"),
        "popt_requested_l3_size": l3_size,
        "popt_effective_l3_size": l3_size,
        "popt_effective_l3_ways": args.l3_ways,
        "popt_reserved_ways": 0,
        "popt_reserved_bytes": 0,
        "popt_matrix_bytes": 0,
        "popt_matrix_fits": 1,
        "popt_matrix_active_columns": 0,
        "popt_property_bytes": max(
            int(getattr(args, "popt_property_bytes", 4)), 1),
        "popt_num_epochs": max(
            int(getattr(args, "popt_num_epochs", 256)), 1),
        "popt_min_data_ways": max(
            int(getattr(args, "popt_min_data_ways", 1)), 1),
        "popt_matrix_column_bytes": 0,
        "popt_matrix_stream_bytes": 0,
        "popt_matrix_stream_cache_lines": 0,
        "popt_estimated_vertices": "",
    }
    if not spec.charge_popt_overhead:
        return metadata

    vertices = estimate_num_vertices(args.options)
    if not vertices:
        metadata["popt_charge_warning"] = "could_not_estimate_vertices"
        return metadata

    line_size = parse_size_bytes(args.line_size)
    assoc = max(int(args.l3_ways), 1)
    property_bytes = max(int(args.popt_property_bytes), 1)
    active_columns = max(int(args.popt_active_columns), 1)
    num_epochs = max(int(args.popt_num_epochs), 1)
    min_data_ways = max(min(int(args.popt_min_data_ways), assoc), 1)

    num_cache_lines = (vertices * property_bytes + line_size - 1) // line_size
    column_bytes = num_cache_lines
    matrix_bytes = active_columns * column_bytes
    sets = max(requested_bytes // (assoc * line_size), 1)
    bytes_per_way = sets * line_size
    reserve_model = getattr(args, "popt_reserve_model", "fixed_one")
    matrix_fits = True
    if reserve_model == "size_correct":
        # Reference-compatible charge: P-OPT keeps
        # `active_columns` Rereference-Matrix columns RESIDENT in reserved LLC
        # ways -- "enough ways need to be reserved as to be able to store
        # 2 * numLines * 1B"; "P-OPT never evicts Rereference Matrix data". The
        # reserved-way count therefore scales with the graph (|V|/elemsPerLine),
        # NOT a fixed one. matrix_bytes = active_columns * numLines (1B/entry).
        needed_ways = (matrix_bytes + bytes_per_way - 1) // bytes_per_way
        max_reservable = max(assoc - min_data_ways, 0)
        if needed_ways > max_reservable:
            # The two resident columns cannot fit while leaving min_data_ways of
            # data: the configured design point is infeasible at this graph/LLC.
            # We still emit a clamped number (data = min_data_ways) as a labeled
            # P-OPT-favorable sensitivity, but flag the cell as infeasible.
            matrix_fits = False
            reserved_ways = max_reservable
        else:
            reserved_ways = needed_ways
    else:
        # LEGACY / P-OPT-FAVORABLE sensitivity ("fixed_one", the historical
        # default): charge a single reserved streaming-buffer way regardless of
        # |V|. This UNDER-charges large graphs (the resident columns span many
        # ways) and is retained only for comparison; it is not size-correct.
        reserved_ways = 1 if (assoc - min_data_ways) >= 1 else 0
    reserved_bytes = reserved_ways * bytes_per_way
    effective_ways = max(assoc - reserved_ways, min_data_ways)
    effective_bytes = sets * effective_ways * line_size
    stream_bytes = num_epochs * column_bytes
    stream_cache_lines = (stream_bytes + line_size - 1) // line_size

    metadata.update({
        "popt_reserve_model": reserve_model,
        "popt_effective_l3_size": format_size_bytes(effective_bytes),
        "popt_effective_l3_ways": str(effective_ways),
        "popt_reserved_ways": reserved_ways,
        "popt_reserved_bytes": reserved_bytes,
        "popt_matrix_bytes": matrix_bytes,
        "popt_bytes_per_way": bytes_per_way,
        "popt_matrix_fits": int(matrix_fits),
        "popt_matrix_active_columns": active_columns,
        "popt_matrix_column_bytes": column_bytes,
        "popt_matrix_stream_bytes": stream_bytes,
        "popt_matrix_stream_cache_lines": stream_cache_lines,
        "popt_estimated_vertices": vertices,
    })
    if not matrix_fits:
        metadata["popt_infeasible"] = 1
        metadata["popt_charge_warning"] = (
            f"matrix_exceeds_llc: needs {(matrix_bytes + bytes_per_way - 1) // bytes_per_way} "
            f"of {assoc} ways for {matrix_bytes}B resident columns; clamped to "
            f"{reserved_ways} reserved / {effective_ways} data way(s)")
    return metadata


def policy_cache_geometry(
        args: argparse.Namespace, spec: PolicySpec,
        l3_size: str) -> dict[str, Any]:
    metadata = popt_charge_metadata(args, spec, l3_size)
    baseline_ways = max(int(args.l3_ways), 1)
    override_ways = max(int(getattr(args, "reuse_plan_l3_ways", 0)), 0)
    metadata.update({
        "reuse_plan_l3_ways_requested": override_ways,
        "reuse_plan_baseline_l3_ways": baseline_ways,
        "reuse_plan_effective_l3_ways": metadata["popt_effective_l3_ways"],
        "reuse_plan_area_mode": (
            "equal_capacity" if override_ways == 0
            else "baseline_equal_silicon_reference"),
    })
    transport = ecg_transport_for(spec, args.benchmark)
    is_reuse_plan = (
        spec.policy == "ECG" and
        spec.ecg_mode == "ECG_GRASP_POPT" and
        transport.reuse_plan_depth == 2)
    if is_reuse_plan:
        metadata["reuse_plan_metadata_bits_per_line"] = 49
    if override_ways == 0:
        return metadata
    if override_ways > baseline_ways:
        raise ValueError(
            f"ReusePlan L3 ways ({override_ways}) cannot exceed baseline "
            f"ways ({baseline_ways})")

    if not is_reuse_plan:
        return metadata

    line_size = parse_size_bytes(args.line_size)
    requested_bytes = parse_size_bytes(l3_size)
    sets = max(requested_bytes // (baseline_ways * line_size), 1)
    effective_bytes = sets * override_ways * line_size
    metadata.update({
        "popt_effective_l3_size": format_size_bytes(effective_bytes),
        "popt_effective_l3_ways": str(override_ways),
        "reuse_plan_effective_l3_ways": str(override_ways),
        "reuse_plan_effective_l3_size": format_size_bytes(effective_bytes),
        "reuse_plan_area_mode": "equal_silicon_sensitivity",
        "reuse_plan_metadata_bits_per_line": 49,
    })
    return metadata


def now_tag() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def sanitize_subprocess_environment(
        env: dict[str, str] | None) -> dict[str, str]:
    clean = dict(env or {})
    for key in list(clean):
        if key.startswith((
                "LD_", "PROOT_", "PYTHON", "FUSE_")):
            clean.pop(key, None)
    clean.update({
        "PATH": "/usr/bin:/bin",
        "HOME": os.environ.get("HOME", "/tmp"),
        "TMPDIR": "/tmp",
        "LC_ALL": "C",
        "LANG": "C",
    })
    return clean


def run_command(
    cmd: list[str],
    cwd: Path,
    env: dict[str, str] | None,
    timeout: int,
    stdout_path: Path,
    dry_run: bool,
    pass_fds: tuple[int, ...] = (),
) -> subprocess.CompletedProcess[str] | None:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    command_text = " ".join(shlex.quote(part) for part in cmd)
    stdout_path.with_suffix(stdout_path.suffix + ".cmd").write_text(command_text + "\n")
    material_env = {
        key: value for key, value in sanitize_subprocess_environment(
            env).items()
        if key.startswith(("ECG_", "GEM5_", "GRAPHBREW_", "OMP_"))
    }
    stdout_path.with_suffix(
        stdout_path.suffix + ".env.json").write_text(
            json.dumps(material_env, indent=2, sort_keys=True) + "\n")

    if dry_run:
        print(f"[dry-run] {command_text}")
        return None

    start = time.time()
    with stdout_path.open("w") as out:
        out.write(f"$ {command_text}\n")
        out.flush()
        process = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            env=sanitize_subprocess_environment(env),
            stdout=out,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            pass_fds=pass_fds,
        )
        try:
            process.communicate(timeout=timeout)
            returncode = process.returncode
        except subprocess.TimeoutExpired:
            elapsed = time.time() - start
            out.write(f"\n[timeout_s] {timeout}\n")
            out.write(f"[elapsed_s] {elapsed:.3f}\n")
            out.write("[timeout_action] SIGTERM process group\n")
            out.flush()
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                out.write("[timeout_action] SIGKILL process group\n")
                out.flush()
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)
            returncode = 124
        result = subprocess.CompletedProcess(cmd, returncode)
        out.write(f"\n[exit_code] {result.returncode}\n")
        out.write(f"[elapsed_s] {time.time() - start:.3f}\n")
    return result


def memory_limited_command(cmd: list[str], memory_limit_gb: float) -> list[str]:
    if memory_limit_gb <= 0.0:
        return cmd
    prlimit = shutil.which("prlimit")
    if not prlimit:
        raise RuntimeError("prlimit not found; cannot enforce Sniper unsafe workload memory limit")
    limit_bytes = int(memory_limit_gb * 1024 * 1024 * 1024)
    return [prlimit, f"--as={limit_bytes}", "--", *cmd]


def clear_sideband_files(paths: dict[str, Path]) -> None:
    for path in paths.values():
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def clear_sniper_reuse_plan_sidebands(paths: dict[str, Path]) -> None:
    for key in ("reuse_plan_offsets", "reuse_plan_records"):
        try:
            paths[key].unlink()
        except (KeyError, FileNotFoundError):
            pass


def validate_sniper_fused_receipts(
        log_path: Path, paths: dict[str, Path]) -> tuple[int, int]:
    receipt_re = re.compile(
        r"\[ECG-ReusePlan-FUSED-RECV sim=sniper seq=(\d+) src=(\d+) "
        r"line=(\d+) addr_line=0x([0-9a-fA-F]+) vpl=(\d+) "
        r"index=(\d+) begin=(\d+) end=(\d+) "
        r"dest=(\d+) tier=(\d+) epoch1=(\d+) epoch2=(\d+)\]")
    if not log_path.exists():
        return 0, 0
    receipts = []
    with log_path.open(errors="ignore") as log:
        for line in log:
            match = receipt_re.search(line)
            if match:
                groups = match.groups()
                receipts.append((
                    int(groups[0]), int(groups[1]), int(groups[2]),
                    int(groups[3], 16), *map(int, groups[4:]),
                ))
    if not receipts:
        return 0, 0
    try:
        offsets_file = paths["reuse_plan_offsets"].open("rb")
        records_file = paths["reuse_plan_records"].open("rb")
    except (KeyError, FileNotFoundError, OSError):
        return len(receipts), len(receipts)
    try:
        offsets_bytes = offsets_file.seek(0, os.SEEK_END)
        records_bytes = records_file.seek(0, os.SEEK_END)
        offsets_file.seek(0)
        records_file.seek(0)
        if (offsets_bytes < 16 or offsets_bytes % 8 != 0 or
                records_bytes < 8 or records_bytes % 8 != 0):
            return len(receipts), len(receipts)
        offset_count = offsets_bytes // 8
        record_count = records_bytes // 8
        with (
            mmap.mmap(offsets_file.fileno(), 0, access=mmap.ACCESS_READ) as offsets,
            mmap.mmap(records_file.fileno(), 0, access=mmap.ACCESS_READ) as records,
        ):
            def offset_at(index: int) -> int:
                return struct.unpack_from("<Q", offsets, index * 8)[0]

            def record_at(index: int) -> int:
                return struct.unpack_from("<Q", records, index * 8)[0]

            previous = offset_at(0)
            sideband_valid = previous == 0
            for offset_index in range(1, offset_count):
                current = offset_at(offset_index)
                if current < previous:
                    sideband_valid = False
                    break
                previous = current
            sideband_valid = sideband_valid and previous == record_count
            bad = 0 if sideband_valid else max(1, len(receipts))
            for (_seq, src, line_id, _addr_line, vpl, index, begin, end,
                 dest, tier, first, second) in receipts:
                valid = (
                    src + 1 < offset_count and
                    offset_at(src) == begin and
                    offset_at(src + 1) == end and
                    begin <= index < end and
                    index < record_count and vpl > 0
                )
                if valid:
                    record = record_at(index)
                    valid = (
                        (record & 0xFFFFFFFF) == dest and
                        1 <= tier <= 3 and
                        ((record >> 32) & 0x3) == tier and
                        ((record >> 34) & 0x7FFF) == first and
                        ((record >> 49) & 0x7FFF) == second and
                        dest // vpl == line_id
                    )
                bad += not valid
    except (OSError, ValueError, struct.error):
        return len(receipts), len(receipts)
    finally:
        offsets_file.close()
        records_file.close()
    with log_path.open("a") as out:
        out.write(
            f"\n[ECG-ReusePlan-FUSED-VALID count={len(receipts)} bad={bad}]\n")
    return len(receipts), bad


def validate_sniper_exact_bind_trace(
        log_path: Path, expected_count: int = 0) -> tuple[int, int]:
    bind_re = re.compile(
        r"\[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=(\d+) core=(\d+) "
        r"bound=0x([0-9a-fA-F]+) line=0x([0-9a-fA-F]+) size=(\d+) "
        r"current=(\d+) context=(\d+)\]")
    receipt_re = re.compile(
        r"\[ECG-ReusePlan-FUSED-RECV sim=sniper seq=(\d+) src=(\d+) "
        r"line=(\d+) addr_line=0x([0-9a-fA-F]+)")
    if not log_path.exists():
        return 0, 0
    binds = {}
    receipts = {}
    duplicate_binds = set()
    duplicate_receipts = set()
    with log_path.open(errors="ignore") as log:
        for line in log:
            bind = bind_re.search(line)
            if bind:
                groups = bind.groups()
                sequence = int(groups[0])
                if sequence in binds:
                    duplicate_binds.add(sequence)
                binds[sequence] = (
                    int(groups[1]), int(groups[2], 16),
                    int(groups[3], 16), int(groups[4]),
                    int(groups[5]), int(groups[6]),
                )
            receipt = receipt_re.search(line)
            if receipt:
                sequence = int(receipt.group(1))
                if sequence in receipts:
                    duplicate_receipts.add(sequence)
                receipts[sequence] = int(receipt.group(4), 16)
    if not binds and not receipts:
        return 0, 0
    bad = (
        len(set(binds) ^ set(receipts)) +
        len(duplicate_binds) + len(duplicate_receipts)
    )
    # A certification run requests a fixed trace budget; anything short of the
    # full contiguous sequence means the proof covers fewer transactions than
    # it claims, so one lucky pairing cannot certify the whole trace.
    if expected_count > 0 and set(binds) != set(range(expected_count)):
        bad += max(1, expected_count - len(set(binds) & set(receipts)))
    for sequence in set(binds) & set(receipts):
        _core, bound, line, size, current, context = binds[sequence]
        valid = (
            size > 0 and (size & (size - 1)) == 0 and
            (bound & ~(size - 1)) == line and
            receipts[sequence] == line and
            0 <= current <= 0x7FFF and context > 0
        )
        bad += not valid
    return len(binds), bad


def sniper_sideband_paths(sniper_out: Path) -> dict[str, Path]:
    # FIXED-LENGTH sideband directory, independent of the policy-named sniper_out
    # (mirrors gem5_sideband_paths). The sideband file paths are read by the
    # benchmark as env strings; if their length varies by policy (because
    # sniper_out embeds the policy name) the benchmark heap shifts and changes
    # array cache-line alignment, swinging per-policy L1/L3 numbers at the tiny
    # ROI cache sizes. A constant-length hashed dir keeps per-cell isolation
    # while making every policy's paths identical length.
    digest = hashlib.sha1(str(sniper_out).encode("utf-8")).hexdigest()[:16]
    sideband_dir = Path(tempfile.gettempdir()) / f"snsb_{digest}"
    context = sideband_dir / "sniper_graphbrew_ctx.json"
    return {
        "context": context,
        "popt_matrix": sideband_dir / "sniper_popt_matrix.bin",
        "out_edges": sideband_dir / "sniper_graphbrew_out_edges.bin",
        "in_edges": sideband_dir / "sniper_graphbrew_in_edges.bin",
        "reuse_plan_offsets": Path(str(context) + ".reuse_plan_offsets.bin"),
        "reuse_plan_records": Path(str(context) + ".reuse_plan_records.bin"),
    }


def build_targets(args: argparse.Namespace) -> None:
    if args.no_build or args.dry_run:
        return

    targets = []
    if args.suite in ("cache-sim", "both"):
        targets.append(f"sim-{args.benchmark}")
    if args.suite in ("gem5", "both"):
        target_prefix = (
            "gem5-riscv-m5ops"
            if selected_gem5_isa() == "riscv" else "gem5-m5ops")
        targets.append(f"{target_prefix}-{args.benchmark}")
    if args.suite == "sniper":
        if args.sniper_workload == "pr_kernel_smoke":
            targets.append("sniper-pr_kernel_smoke")
        elif args.sniper_workload == "sg_kernel" and args.allow_sniper_sg_kernel_workload:
            targets.append("sniper-sg_kernel")
        elif args.allow_sniper_benchmark_workload:
            targets.append(f"sniper-{args.benchmark}")

    for target in targets:
        print(f"[build] make {target}")
        subprocess.run(["make", target], cwd=str(PROJECT_ROOT), check=True)


def selected_gem5_isa() -> str:
    suffix_isa = {
        "_m5ops": "x86",
        "_riscv_m5ops": "riscv",
    }.get(GEM5_KERNEL_SUFFIX)
    if suffix_isa is None:
        raise SystemExit(
            f"unsupported GEM5_KERNEL_SUFFIX={GEM5_KERNEL_SUFFIX!r}; "
            "use _m5ops or _riscv_m5ops")
    build_isa = "riscv" if "RISCV" in GEM5_OPT.parts else (
        "x86" if "X86" in GEM5_OPT.parts else "")
    if build_isa != suffix_isa:
        raise SystemExit(
            f"inconsistent gem5 ISA selection: GEM5_OPT={GEM5_OPT} "
            f"but GEM5_KERNEL_SUFFIX={GEM5_KERNEL_SUFFIX}")
    return suffix_isa


def selected_gem5_guest_paths(
        benchmark: str) -> tuple[Path, Path, Path, list[Path], Path]:
    binary = PROJECT_ROOT / "bench" / "bin_gem5" / (
        f"{benchmark}{GEM5_KERNEL_SUFFIX}")
    receipt = Path(str(binary) + ".build.json")
    source = PROJECT_ROOT / "bench" / "src_gem5" / f"{benchmark}.cc"
    build_config = PROJECT_ROOT / "bench" / "bin_gem5" / (
        ".riscv_build_config")
    link_inputs = [
        PROJECT_ROOT / "bench" / "include" / "gem5_sim" / "gem5" /
        "util" / "m5" / "build" / "riscv" / "out" / "libm5.a",
    ]
    return binary, receipt, source, link_inputs, build_config


def validate_selected_gem5_guest(
        args: argparse.Namespace, out_dir: Path) -> None:
    global VALIDATED_GEM5_GUEST, VALIDATED_GEM5_GUEST_SHA256
    if args.suite not in ("gem5", "both"):
        VALIDATED_GEM5_GUEST = None
        VALIDATED_GEM5_GUEST_SHA256 = ""
        return
    binary, receipt, source, link_inputs, build_config = (
        selected_gem5_guest_paths(args.benchmark))
    expected = str(args.expected_gem5_guest_sha256)
    if expected == PLANNING_MISSING_GEM5_GUEST_SHA256:
        if not args.dry_run:
            raise SystemExit(
                "planning-only missing gem5 guest hash cannot execute")
        expected = ""
    if selected_gem5_isa() != "riscv":
        actual = hash_input_path(binary)
        if actual == "missing":
            raise SystemExit(f"gem5 guest binary is missing: {binary}")
        if expected and actual != expected:
            raise SystemExit(
                "gem5 guest does not match experiment-run expected hash: "
                f"{actual} != {expected}")
        VALIDATED_GEM5_GUEST = binary
        VALIDATED_GEM5_GUEST_SHA256 = actual
        return
    receipt_errors = validate_gem5_guest_receipt(
        receipt, binary, source, link_inputs, build_config, PROJECT_ROOT)
    if receipt_errors:
        raise SystemExit(
            "prebuilt gem5 guest provenance failed:\n  " +
            "\n  ".join(receipt_errors))
    try:
        VALIDATED_GEM5_GUEST, VALIDATED_GEM5_GUEST_SHA256 = (
            stage_validated_guest(
                receipt, binary, source, link_inputs, build_config,
                out_dir / ".validated_inputs", PROJECT_ROOT))
    except ValueError as error:
        raise SystemExit(
            f"cannot stage validated gem5 guest: {error}") from error
    if expected and VALIDATED_GEM5_GUEST_SHA256 != expected:
        raise SystemExit(
            "validated gem5 guest does not match experiment-run expected hash: "
            f"{VALIDATED_GEM5_GUEST_SHA256} != {expected}")


def graph_path_from_options(options: str) -> Path | None:
    parts = shlex.split(options)
    if "-f" not in parts:
        return None
    index = parts.index("-f")
    if index + 1 >= len(parts):
        return None
    path = Path(parts[index + 1])
    return path if path.is_absolute() else (PROJECT_ROOT / path).resolve()


def validate_expected_gem5_inputs(args: argparse.Namespace) -> None:
    if args.suite not in ("gem5", "both"):
        return
    checks = [
        ("gem5 executable", GEM5_OPT, args.expected_gem5_opt_sha256),
        (
            "gem5 config tree", GEM5_CONFIG.parent,
            args.expected_gem5_config_sha256),
    ]
    graph = graph_path_from_options(args.options)
    if graph is not None:
        checks.append(("graph input", graph, args.expected_graph_sha256))
    for label, path, expected in checks:
        actual = hash_input_path(path)
        if expected and actual != expected:
            raise SystemExit(
                f"{label} does not match experiment-run expected hash: "
                f"{actual} != {expected}")


def parse_ecg_log_stats(
        log_path: Path, combined_text: str | None = None
        ) -> dict[str, Any]:
    if combined_text is None and not log_path.exists():
        return {}
    stats: dict[str, Any] = {}
    pattern = re.compile(r"(build_s|vertices|pfx_candidates|pfx_encoded|pfx_no_candidate|pfx_table_miss|pfx_dedup_skips|runtime_no_target|runtime_duplicate|runtime_issued)=([0-9.]+)")
    text = (
        combined_text if combined_text is not None
        else log_path.read_text(errors="ignore"))
    for line in text.splitlines():
        if not line.startswith("ECG Mask Stats:"):
            continue
        for key, value in pattern.findall(line):
            out_key = f"ecg_{key}"
            stats[out_key] = float(value) if "." in value else int(value)
    sidecar = re.search(
        r"\[ReusePlan-SIDECAR sim=gem5 active=1 "
        r"record_bytes=(\d+) records=(\d+) graph_hash=(\d+) "
        r"payload_hash=(\d+)\]",
        text)
    if sidecar:
        stats.update({
            "gem5_reuse_plan_sidecar_active": 1,
            "gem5_reuse_plan_sidecar_record_bytes":
                int(sidecar.group(1)),
            "gem5_reuse_plan_sidecar_records":
                int(sidecar.group(2)),
            "gem5_reuse_plan_sidecar_graph_hash":
                sidecar.group(3),
            "gem5_reuse_plan_sidecar_payload_hash":
                sidecar.group(4),
        })
    return stats


def cache_sim_env(args: argparse.Namespace, spec: PolicySpec, effective_l3_size: str,
                  effective_l3_ways: str, json_path: Path) -> dict[str, str]:
    env = dict(os.environ)
    scrub_cell_mechanism_env(env)
    apply_explicit_cell_mechanism_env(env, spec)
    transport = ecg_transport_for(spec, args.benchmark)
    apply_ecg_transport_env(env, transport)
    env.update({
        "CACHE_ULTRAFAST": "0",
        "CACHE_FAST": "0",
        "CACHE_SAMPLED": "0",
        "CACHE_MULTICORE": "0",
        "CACHE_POLICY": spec.policy,
        "CACHE_L1_POLICY": "LRU",
        "CACHE_L2_POLICY": "LRU",
        "CACHE_L3_POLICY": spec.policy,
        "CACHE_L1_SIZE": args.l1d_size,
        "CACHE_L1_WAYS": args.l1d_ways,
        "CACHE_L2_SIZE": args.l2_size,
        "CACHE_L2_WAYS": args.l2_ways,
        "CACHE_L3_SIZE": effective_l3_size,
        "CACHE_L3_WAYS": effective_l3_ways,
        "CACHE_LINE_SIZE": args.line_size,
        "CACHE_OUTPUT_JSON": str(json_path),
        # Simulate P-OPT's rereference-matrix column stream as real accesses so
        # the structure prefetcher covers it exactly as it covers ReusePlan's per-edge
        # records. Only meaningful for policies that carry the overhead charge.
        "POPT_MATRIX_STREAM_SIM": (
            "1" if (spec.charge_popt_overhead and
                    getattr(args, "popt_matrix_stream", "analytic") == "simulated")
            else "0"),
        # Structural-FlowThrough, offered to every policy so ReusePlan's FlowThrough
        # is not a mechanism its competitors are denied.
        "CACHE_STREAM_PREFETCH_MODEL": getattr(
            args, "stream_prefetch_model", "stride"),
        "FLOWTHROUGH": (
            "1" if getattr(args, "flowthrough", "off") == "all" else "0"),
        # Structure-stream prefetcher degree, applied to ALL policies (0 = off).
        # This is the cross-sim LEVELING control: --prefetcher STRIDE is the switch
        # that turns on each simulator's native generic stream/stride prefetcher
        # (gem5 StridePrefetcher, Sniper "simple"); for cache_sim the equivalent is
        # this next-line model, so when STRIDE is selected we honor the SAME
        # --structure-prefetch-degree here. NOTE: the three prefetchers are NOT
        # algorithm-identical (cache_sim = idealized next-line on non-property
        # regions; gem5 = learned stride; Sniper = n-flow next-line) -- treat the
        # leveled comparison as a sensitivity/control, not exact prefetch equivalence.
        "CACHE_STREAM_PREFETCH_DEGREE": str(
            args.structure_prefetch_degree if args.prefetcher == "STRIDE"
            else args.cache_stream_prefetch_degree),
        # cache_sim MUST run single-threaded for deterministic/reproducible
        # results: the OpenMP-parallel kernel records cache accesses in
        # nondeterministic interleaved order, so >1 thread yields
        # non-reproducible, thread-count-dependent miss counts.
        "OMP_NUM_THREADS": str(args.cache_sim_omp_threads),
        "CACHE_ECG_EPOCH_REGION_INDICES":
            cache_sim_ecg_epoch_region_indices(args.benchmark),
    })
    env.update(ecg_pfx_env(args))
    if spec.ecg_mode:
        env["ECG_MODE"] = spec.ecg_mode
        env["ECG_VARIANT"] = effective_ecg_variant(
            args, transport.reuse_plan_depth, spec)
        if spec.ecg_mode == "ECG_GRASP_POPT":
            env.update({
                "ECG_EXACT_REREF": "1",
                "ECG_PREFETCH_MODE": "6",
                "ECG_EDGE_MASK_EPOCH": "1",
                "ECG_EDGE_MASK_LINEMIN": "1",
                "ECG_EDGE_MASK_EPOCHS": str(args.ecg_epochs),
                "ECG_EDGE_MASK_LEAN": "1",
                "ECG_EDGE_MASK_PACK": "1",
                "ECG_EDGE_MASK_PACK_BITS": str(args.ecg_epoch_pack_bits),
                "ECG_EDGE_MASK_CHARGED": str(args.ecg_charged),
            })
            # ECG_STORED_REFRESH / ECG_REFRESH_LLC_ONLY gates are PRESENCE-based in
            # cache_sim.h (getenv != null), so only inject each key when enabled; never
            # set "0" (still enables). Refresh defaults OFF: the aggressive form is an
            # IDEALIZED CEILING (uncharged per-access LLC metadata write on L1/L2 hits),
            # NOT hardware-free — its feasible piggybacked form recovers ~0. Enable it
            # only as a labelled ceiling, optionally with --ecg-refresh-llc-only for the
            # feasible measurement.
            if getattr(args, "ecg_stored_refresh", 0):
                env["ECG_STORED_REFRESH"] = "1"
                if getattr(args, "ecg_refresh_llc_only", 0):
                    env["ECG_REFRESH_LLC_ONLY"] = "1"
                else:
                    env.pop("ECG_REFRESH_LLC_ONLY", None)
            else:
                env.pop("ECG_STORED_REFRESH", None)
                env.pop("ECG_REFRESH_LLC_ONLY", None)
    return env


def requested_ecg_reuse_plan_depth() -> int:
    raw = os.environ.get("ECG_REUSE_PLAN_DEPTH", "0") or "0"
    try:
        return int(raw)
    except ValueError as exc:
        raise RuntimeError(
            f"ECG_REUSE_PLAN_DEPTH must be an integer, got {raw!r}") from exc


@dataclass(frozen=True)
class EcgTransport:
    reuse_plan_depth: int = 0
    flowthrough: bool = False
    trace_enabled: bool = True
    set_dueling: bool = False
    edge_masks: bool = False
    flowthrough_adaptive: bool = False


def ecg_transport_for(spec: PolicySpec, benchmark: str) -> EcgTransport:
    if spec.policy != "ECG" or spec.ecg_mode != "ECG_GRASP_POPT":
        return EcgTransport()

    explicit = spec.ecg_transport_pinned
    if (explicit and spec.ecg_reuse_plan_depth == 2 and
            benchmark not in ("pr", "bfs", "sssp", "bc", "cc")):
        raise RuntimeError(
            f"ECG ReusePlan delivery is not implemented for benchmark {benchmark!r}.")
    reuse_plan_depth = (
        spec.ecg_reuse_plan_depth if explicit else requested_ecg_reuse_plan_depth())
    flowthrough = (
        spec.ecg_flowthrough if explicit
        else os.environ.get("ECG_FLOWTHROUGH") == "1")
    flowthrough_adaptive = (
        spec.ecg_flowthrough_adaptive if explicit
        else os.environ.get("ECG_FLOWTHROUGH_ADAPTIVE") == "1")
    return EcgTransport(
        reuse_plan_depth=reuse_plan_depth,
        flowthrough=flowthrough,
        set_dueling=spec.ecg_set_dueling,
        trace_enabled=not explicit,
        edge_masks=explicit or reuse_plan_depth > 0,
        flowthrough_adaptive=flowthrough_adaptive,
    )


def apply_ecg_transport_env(
        env: dict[str, str], transport: EcgTransport) -> None:
    explicit_reuse_plan_trace = env.get("ECG_REUSE_PLAN_DELIVERY_TRACE")
    explicit_flowthrough_trace = env.get("ECG_FLOWTHROUGH_TRACE")
    for key in (
        "ECG_REUSE_PLAN_DEPTH",
        "ECG_EDGE_MASKS",
        "ECG_REUSE_PLAN_DELIVERY_TRACE",
        "ECG_FLOWTHROUGH",
        "ECG_FLOWTHROUGH_TRACE",
        "ECG_FLOWTHROUGH_ADAPTIVE",
        "ECG_SET_DUELING",
    ):
        env.pop(key, None)
    if transport.edge_masks:
        env["ECG_EDGE_MASKS"] = "1"
    if transport.reuse_plan_depth:
        env["ECG_REUSE_PLAN_DEPTH"] = str(transport.reuse_plan_depth)
        trace = explicit_reuse_plan_trace
        if trace is None and transport.trace_enabled:
            trace = os.environ.get("ECG_REUSE_PLAN_DELIVERY_TRACE")
        if trace:
            env["ECG_REUSE_PLAN_DELIVERY_TRACE"] = trace
    if transport.flowthrough:
        env["ECG_FLOWTHROUGH"] = "1"
        if transport.flowthrough_adaptive:
            env["ECG_FLOWTHROUGH_ADAPTIVE"] = "1"
        trace = explicit_flowthrough_trace
        if trace is None and transport.trace_enabled:
            trace = os.environ.get("ECG_FLOWTHROUGH_TRACE")
        if trace:
            env["ECG_FLOWTHROUGH_TRACE"] = trace
    if transport.set_dueling:
        env["ECG_SET_DUELING"] = "1"


def scrub_cell_mechanism_env(env: dict[str, str]) -> None:
    diagnostic_keys = {
        "ECG_DEBUG",
        "ECG_EVICT_TRACE",
        "ECG_EVICT_TRACE_ROI",
        "GEM5_ECG_EXT_TRACE",
    }
    diagnostics = {
        key: os.environ[key]
        for key in diagnostic_keys
        if key in os.environ
    }
    prefixes = (
        "ECG_",
        "GEM5_ECG_",
        "GEM5_FORCE_ECG_",
        "SNIPER_ECG_",
        "SNIPER_ENABLE_ECG_",
        "CACHE_ECG_",
        "GRASP_",
        "POPT_",
    )
    for key in list(env):
        if key.startswith(prefixes):
            env.pop(key, None)
    env.update(diagnostics)


def disable_gem5_event_traces(env: dict[str, str]) -> None:
    for key in (
            "ECG_REUSE_PLAN_DELIVERY_TRACE",
            "ECG_FLOWTHROUGH_TRACE",
            "GEM5_ECG_EXT_TRACE",
            "ECG_EVICT_TRACE",
            "ECG_EVICT_TRACE_ROI"):
        env.pop(key, None)


def explicit_cell_mechanism_env() -> dict[str, Any]:
    raw = os.environ.get("GRAPHBREW_EXPLICIT_CELL_ENV", "{}")
    try:
        explicit = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "GRAPHBREW_EXPLICIT_CELL_ENV must be valid JSON") from exc
    if not isinstance(explicit, dict):
        raise RuntimeError(
            "GRAPHBREW_EXPLICIT_CELL_ENV must encode an object")
    return explicit


def explicit_ecg_record_bytes(default: int) -> int:
    explicit = explicit_cell_mechanism_env()
    raw = explicit.get(
        "ECG_EXPECT_BYTES_PER_EDGE",
        explicit.get("ECG_EDGE_RECORD_BYTES"))
    if raw is None:
        return default
    try:
        width = int(float(str(raw)))
    except ValueError as exc:
        raise RuntimeError(
            f"ECG record width must be numeric, got {raw!r}") from exc
    if width <= 0:
        raise RuntimeError(
            f"ECG record width must be positive, got {width}")
    return width


def require_sniper_reuse_plan_certification_budget(
        env: dict[str, str]) -> int:
    raw = env.get("ECG_REUSE_PLAN_DELIVERY_TRACE", "0") or "0"
    try:
        budget = int(raw)
    except ValueError as exc:
        raise RuntimeError(
            f"ECG_REUSE_PLAN_DELIVERY_TRACE must be an integer, got {raw!r}") from exc
    if budget < MIN_SNIPER_REUSE_PLAN_CERT_RECEIPTS:
        raise RuntimeError(
            "Sniper ReuseBind exact-bind certification requires "
            f"ECG_REUSE_PLAN_DELIVERY_TRACE >= {MIN_SNIPER_REUSE_PLAN_CERT_RECEIPTS}; "
            f"got {budget}")
    return budget


def apply_sniper_transport_cell_env(env: dict[str, str]) -> None:
    explicit = explicit_cell_mechanism_env()
    for key in (
            "ECG_RECORD_VARIABLE_WIDTH",
            "ECG_EXPECT_BYTES_PER_EDGE",
            "ECG_EDGE_RECORD_BYTES"):
        if key in explicit:
            env[key] = str(explicit[key])


def apply_explicit_cell_mechanism_env(
        env: dict[str, str], spec: PolicySpec) -> None:
    explicit = explicit_cell_mechanism_env()
    for key, value in explicit.items():
        key = str(key)
        allowed = (
            (key.startswith((
                "ECG_", "GEM5_ECG_", "GEM5_FORCE_ECG_",
                "SNIPER_ECG_", "SNIPER_ENABLE_ECG_", "CACHE_ECG_"))
             and spec.policy == "ECG") or
            (key.startswith("GRASP_") and spec.policy in ("GRASP", "ECG")) or
            (key.startswith("POPT_") and spec.policy in ("POPT", "ECG"))
        )
        if allowed:
            env[str(key)] = str(value)


def apply_gem5_compact_fused_receipt(
        row: dict[str, Any], log_text: str, requested: bool) -> bool:
    """Attest fused-compact activation from the guest, never from the request."""
    active = "[ECG_REUSE_BIND_ILOAD_C]" in log_text
    row["gem5_compact_fused_active"] = int(active)
    if active:
        row["gem5_ecg_delivery"] = "ecg.bind.iload.compact"
    if requested and not active:
        mark_row_error(row, (
            "fused compact ReusePlan was requested but the guest emitted no "
            "ECG_REUSE_BIND_ILOAD_C activation receipt"))
    return active


def apply_gem5_popt_receipt(
        row: dict[str, Any], log_text: str, required: bool) -> bool:
    match = re.search(
        r"\[POPT-ACTIVE sim=gem5 context=1 reref=1 phase2=1 "
        r"epochs=(\d+) cache_lines=(\d+)\]",
        log_text)
    active = match is not None
    row["popt_policy_active"] = int(active)
    row["popt_context_loaded"] = int(active)
    row["popt_rereference_loaded"] = int(active)
    if match:
        row["popt_runtime_epochs"] = int(match.group(1))
        row["popt_runtime_cache_lines"] = int(match.group(2))
    if required and not active:
        mark_row_error(
            row,
            "P-OPT completed without entering the rereference victim path")
    return active


def apply_gem5_grasp_receipt(
        row: dict[str, Any], log_text: str, required: bool) -> bool:
    match = re.search(
        r"\[GRASP-ACTIVE sim=gem5 context=1 regions=(\d+)\]",
        log_text)
    active = match is not None
    row["grasp_context_loaded"] = int(active)
    row["grasp_regions_loaded"] = (
        int(match.group(1)) if match else 0)
    if required and not active:
        mark_row_error(
            row,
            "GRASP completed without loading graph context")
    return active


def apply_gem5_geometry_receipt(
        row: dict[str, Any], config_path: Path,
        expected_size: str, expected_ways: str) -> bool:
    try:
        config = json.loads(config_path.read_text())
        l3 = config["system"]["l3cache"]
        actual_size = parse_size_bytes(str(l3["size"]))
        actual_ways = int(l3["assoc"])
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
        mark_row_error(row, "gem5 config is missing the realized LLC geometry")
        return False
    row["gem5_l3_size_actual"] = actual_size
    row["gem5_l3_ways_actual"] = actual_ways
    valid = (
        actual_size == parse_size_bytes(str(expected_size)) and
        actual_ways == int(expected_ways))
    if not valid:
        mark_row_error(
            row,
            "gem5 realized LLC geometry differs from the charged geometry")
    return valid


def apply_sniper_geometry_receipt(
        row: dict[str, Any], sniper_out: Path,
        expected_kb: int, expected_ways: str) -> bool:
    """Attest Sniper's actually-applied LLC (NUCA) geometry.

    Sniper's CacheParameters constructor computes
    ``num_sets = size_kb * 1024 / (associativity * block_size)`` and
    LOG_ASSERT_ERRORs (a fatal, run-aborting check) if that does not divide
    evenly, so a *completed* run's geometry is guaranteed internally
    self-consistent. That does not, however, prove our requested
    ``-g perf_model/nuca/cache_size=...``/``associativity=...`` overrides
    actually reached the merged config (a wrong key path or a later config
    file could silently leave a stale default in place). Verify against
    Sniper's OWN emitted ``sim.cfg`` (the merged, as-applied config dump)
    rather than re-asserting our own already-computed request as ground
    truth -- mirroring apply_gem5_geometry_receipt's use of gem5's
    config.json instead of gem5's requested/charged geometry.

    Also require ``[perf_model/nuca] enabled = true``: Sniper's base.cfg
    ships ``[perf_model/nuca] enabled = false`` by default (NUCA is opt-in
    via nuca-cache.cfg), and a merged config that left ``enabled = false``
    would make the ``cache_size``/``associativity`` keys checked above
    inert -- the run would then be modeling the LLC through a different,
    unattested mechanism with a possibly different real geometry, even
    though those two keys happen to read back as the requested values.
    """
    sim_cfg_path = sniper_out / "simulation" / "sim.cfg"
    if not sim_cfg_path.exists():
        sim_cfg_path = sniper_out / "sim.cfg"
    try:
        cfg_text = sim_cfg_path.read_text(errors="ignore")
    except OSError:
        mark_row_error(
            row,
            "Sniper sim.cfg is missing; cannot verify realized LLC geometry")
        return False
    section_match = re.search(
        r"\[perf_model/nuca\](.*?)(?:\n\[|\Z)", cfg_text, re.DOTALL)
    if not section_match:
        mark_row_error(row, "Sniper sim.cfg has no [perf_model/nuca] section")
        return False
    section = section_match.group(1)
    enabled_match = re.search(
        r'^enabled(?:\[\])?\s*=\s*"?([A-Za-z0-9]+)"?',
        section, re.MULTILINE)
    if not enabled_match:
        mark_row_error(
            row,
            "Sniper sim.cfg [perf_model/nuca] has no 'enabled' key; cannot "
            "verify NUCA is actually driving the realized LLC geometry")
        return False
    nuca_enabled = enabled_match.group(1).strip().lower() in ("true", "1")
    row["sniper_l3_nuca_enabled"] = int(nuca_enabled)
    if not nuca_enabled:
        mark_row_error(
            row,
            "Sniper sim.cfg reports [perf_model/nuca] enabled = false; the "
            "realized LLC geometry cannot be attested through a disabled "
            "NUCA cache")
        return False
    size_match = re.search(
        r"^cache_size(?:\[\])?\s*=\s*([0-9,]+)", section, re.MULTILINE)
    assoc_match = re.search(
        r"^associativity(?:\[\])?\s*=\s*([0-9,]+)", section, re.MULTILINE)
    if not size_match or not assoc_match:
        mark_row_error(
            row, "Sniper sim.cfg is missing the realized LLC geometry")
        return False
    actual_size_values = {
        int(v) for v in size_match.group(1).split(",") if v.strip()}
    actual_assoc_values = {
        int(v) for v in assoc_match.group(1).split(",") if v.strip()}
    if len(actual_size_values) != 1 or len(actual_assoc_values) != 1:
        mark_row_error(
            row, "Sniper realized LLC geometry differs across cores")
        return False
    actual_size = next(iter(actual_size_values))
    actual_ways = next(iter(actual_assoc_values))
    row["sniper_l3_size_actual_kb"] = actual_size
    row["sniper_l3_ways_actual"] = actual_ways
    valid = actual_size == int(expected_kb) and actual_ways == int(expected_ways)
    if not valid:
        mark_row_error(
            row,
            "Sniper realized LLC geometry differs from the charged geometry")
    return valid



def validate_online_dueling_activity(
        row: dict[str, Any], required: bool,
        positive_fields: tuple[str, ...] = ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
        leader_samples_field: str = "gem5_reuse_plan_dueling_leader_samples") -> bool:
    """Fail-closed check that online set-dueling actually ran in the ROI.

    Shared by gem5 (default fields) and Sniper (``sniper_*`` fields passed by
    the caller) -- the gem5 field names/semantics are never renamed; Sniper
    passes its own ``sniper_reuse_plan_dueling_*`` fields explicitly instead.
    """
    if not required:
        return True
    missing = [
        field for field in positive_fields
        if int(row.get(field) or 0) <= 0
    ]
    leader_samples = int(row.get(leader_samples_field) or 0)
    if leader_samples < ONLINE_DUELING_WINDOW_MISSES:
        missing.append(
            f"{leader_samples_field}<{ONLINE_DUELING_WINDOW_MISSES}")
    if missing:
        mark_row_error(
            row,
            "online ReusePlan set-dueling was not exercised in the ROI: "
            f"{missing}")
        return False
    return True


def mark_row_error(row: dict[str, Any], message: str) -> None:
    """Preserve every failing gate instead of replacing the first cause."""
    existing = str(row.get("error") or "").strip()
    if not existing:
        row["error"] = message
    elif message not in existing:
        row["error"] = f"{existing} | {message}"
    row["status"] = "error"
    row["timing_valid_for_speedup"] = "0"


def apply_gem5_compact_reuse_bind_flowthrough_receipt(
        row: dict[str, Any], log_text: str, requested: bool,
        require_trace_receipts: bool = True,
        performance_requested: bool = False) -> bool:
    """Validate the proposal path, with traces required only for mechanism rows."""
    format_receipt = re.search(
        r"\[ECG_REUSE_BIND_LOAD_C_FLOW\][^\n]*"
        r"id_bits=(\d+) epoch_bits=(\d+)", log_text)
    active = "[ECG_REUSE_BIND_LOAD_C_FLOW]" in log_text
    row["gem5_compact_reuse_bind_flowthrough_active"] = int(active)
    if active:
        row["gem5_ecg_delivery"] = (
            "ecg.flow.load.compact+ecg.bind.load.f32")
        row["proposal_path_active"] = 1
    else:
        row["proposal_path_active"] = 0
    all_flowthrough_lines = re.findall(
        r"\[ECG-FLOWTHROUGH sim=gem5 [^\n]*allocate=0\]",
        log_text)
    range_lines = [
        line for line in all_flowthrough_lines
        if "source=range" in line
    ]
    request_flag_lines = re.findall(
        r"\[ECG-FLOWTHROUGH sim=gem5 [^\n]*"
        r"source=request-flag [^\n]*allocate=0\]",
        log_text)
    request_flag_sizes = []
    for line in request_flag_lines:
        match = re.search(r"\bsize=(\d+)\b", line)
        request_flag_sizes.append(
            int(match.group(1)) if match else -1)
    request_flag_events = len(request_flag_lines)
    size4_events = sum(size == 4 for size in request_flag_sizes)
    bad_size_events = sum(size != 4 for size in request_flag_sizes)
    row["gem5_flowthrough_request_flag_events"] = request_flag_events
    row["gem5_flowthrough_request_flag_size4_events"] = size4_events
    row["gem5_flowthrough_request_flag_bad_size_events"] = bad_size_events
    row["gem5_flowthrough_all_events"] = len(all_flowthrough_lines)
    row["gem5_flowthrough_range_events"] = len(range_lines)
    if format_receipt:
        row["proposal_compact_id_bits"] = int(format_receipt.group(1))
        row["proposal_compact_epoch_bits"] = int(format_receipt.group(2))
        row["proposal_compact_tier_bits"] = 2
    row["proposal_performance_mode_active"] = int(
        performance_requested and active and format_receipt is not None)
    if requested and not active:
        mark_row_error(row, (
            "proposal compact ReuseBind+FlowThrough was requested but "
            "the guest emitted no ECG_REUSE_BIND_LOAD_C_FLOW activation receipt"))
    elif performance_requested and format_receipt is None:
        mark_row_error(row, (
            "trace-free proposal timing was requested but the guest emitted "
            "no compact field-width receipt"))
    elif require_trace_receipts and requested and (
            request_flag_events == 0 or
            size4_events == 0 or bad_size_events != 0 or
            len(all_flowthrough_lines) != request_flag_events or range_lines):
        mark_row_error(row, (
            "proposal compact ReuseBind+FlowThrough was requested but "
            + ("the LLC emitted no request-flag FlowThrough receipt"
               if request_flag_events == 0 else
               "the request-flag FlowThrough receipts did not attest "
               "only 4-byte request-flag record requests")))
    return active


def apply_gem5_reuse_bind_receipt(
        row: dict[str, Any], log_text: str, requested: bool,
        line_bytes: int = 64,
        require_discriminating: bool = False,
        trace_limit: int = 0) -> bool:
    request_re = re.compile(
        r"\[ECG-ReuseBind-REQUEST sim=gem5 seq=(\d+) request_seq=(\d+) "
        r"dest=(\d+) tier=(\d+) epoch1=(\d+) epoch2=(\d+) "
        r"current=(\d+) context=(\d+)\]")
    accept_re = re.compile(
        r"\[ECG-ReuseBind-ACCEPT sim=gem5 seq=(\d+) request_seq=(\d+) "
        r"request_dest=(\d+) fill_dest=(\d+) source=(\w+) "
        r"tier=(\d+) epoch1=(\d+) epoch2=(\d+) current=(\d+) "
        r"context=(\d+) (?:property_elem_bytes|width)=(\d+)\]")
    requests = {}
    request_conflicts = 0
    raw_request_receipts = 0
    request_trace_max_seq = -1
    for match in request_re.finditer(log_text):
        groups = tuple(map(int, match.groups()))
        raw_request_receipts += 1
        request_trace_max_seq = max(request_trace_max_seq, groups[0])
        request_sequence = groups[1]
        payload = groups[2:]
        previous = requests.setdefault(request_sequence, payload)
        request_conflicts += previous != payload
    accepts = []
    accepted_metadata = set()
    accepted_epoch_states = set()
    accepted_plan_epochs = set()
    accept_sequences = set()
    duplicate_accepts = 0
    mailbox_accepts = 0
    exact_vertex_accepts = 0
    coalesced_line_accepts = 0
    nonzero_epoch_accepts = 0
    bad = 0
    for match in accept_re.finditer(log_text):
        groups = match.groups()
        request_seq = int(groups[1])
        request_dest = int(groups[2])
        fill_dest = int(groups[3])
        source = groups[4]
        payload = tuple(map(int, groups[5:10]))
        property_elem_bytes = int(groups[10])
        expected = requests.get(request_seq)
        same_line = (
            property_elem_bytes > 0 and line_bytes > 0 and
            (request_dest * property_elem_bytes) // line_bytes ==
            (fill_dest * property_elem_bytes) // line_bytes)
        valid = (
            expected is not None and source == "request" and
            request_dest == expected[0] and same_line and
            payload == expected[1:] and property_elem_bytes == 4)
        if valid:
            accepted_metadata.add(payload)
            accepted_epoch_states.add(payload[1:4])
            accepted_plan_epochs.add(payload[1:3])
            if request_dest == fill_dest:
                exact_vertex_accepts += 1
            else:
                coalesced_line_accepts += 1
            nonzero_epoch_accepts += payload[1] != 0 or payload[2] != 0
        mailbox_accepts += source == "mailbox"
        bad += not valid
        duplicate_accepts += request_seq in accept_sequences
        accept_sequences.add(request_seq)
        accepts.append(request_seq)
    request_metadata = {
        payload[1:] for payload in requests.values()
    }
    request_epoch_states = {
        (payload[2], payload[3], payload[4])
        for payload in requests.values()
    }
    request_plan_epochs = {
        (payload[2], payload[3])
        for payload in requests.values()
    }
    payload_discriminating = (
        len(request_epoch_states) > 1 and
        len(accepted_epoch_states) > 1 and
        len(request_plan_epochs) > 1 and
        len(accepted_plan_epochs) > 1 and
        any(
            epoch1 != 0 or epoch2 != 0
            for (epoch1, epoch2) in accepted_plan_epochs
        )
    )
    exact_bind = (
        bool(requests) and bool(accepts) and
        request_conflicts == 0 and duplicate_accepts == 0 and bad == 0)
    trace_saturated = (
        trace_limit > 0 and
        (
            raw_request_receipts >= trace_limit or
            request_trace_max_seq >= trace_limit - 1
        )
    )
    row.update({
        "gem5_reuse_bind_request_receipts": len(requests),
        "gem5_reuse_bind_request_trace_events": raw_request_receipts,
        "gem5_reuse_bind_request_trace_max_seq": request_trace_max_seq,
        "gem5_reuse_bind_duplicate_request_receipts": (
            raw_request_receipts - len(requests)),
        "gem5_reuse_bind_request_accepts": len(accepts),
        "gem5_reuse_bind_request_bad_receipts": bad,
        "gem5_reuse_bind_request_conflicts": request_conflicts,
        "gem5_reuse_bind_duplicate_accepts": duplicate_accepts,
        "gem5_reuse_bind_mailbox_accepts": mailbox_accepts,
        "gem5_reuse_bind_exact_vertex_accepts": exact_vertex_accepts,
        "gem5_reuse_bind_coalesced_line_accepts": coalesced_line_accepts,
        "gem5_reuse_bind_nonzero_epoch_accepts": nonzero_epoch_accepts,
        "gem5_reuse_bind_request_line_bytes": line_bytes,
        "gem5_reuse_bind_request_metadata_values": len(request_metadata),
        "gem5_reuse_bind_accept_metadata_values": len(accepted_metadata),
        "gem5_reuse_bind_request_epoch_states": len(request_epoch_states),
        "gem5_reuse_bind_accept_epoch_states": len(accepted_epoch_states),
        "gem5_reuse_bind_request_plan_epochs": len(
            request_plan_epochs),
        "gem5_reuse_bind_accept_plan_epochs": len(
            accepted_plan_epochs),
        "gem5_reuse_bind_payload_discriminating": int(payload_discriminating),
        "gem5_reuse_plan_exact_bind": int(exact_bind),
        "gem5_reuse_bind_trace_saturated": int(trace_saturated),
    })
    valid = exact_bind and (
        not require_discriminating or payload_discriminating)
    if requested and not valid:
        mark_row_error(row, (
            "proposal ReuseBind exact Request binding was not attested "
            f"(requests={len(requests)} accepts={len(accepts)} "
            f"conflicts={request_conflicts} "
            f"duplicate_accepts={duplicate_accepts} "
            f"mailbox={mailbox_accepts} "
            f"bad={bad} request_metadata={len(request_metadata)} "
            f"accept_metadata={len(accepted_metadata)} "
            f"request_epoch_states={len(request_epoch_states)} "
            f"accept_epoch_states={len(accepted_epoch_states)} "
            f"request_record_epochs={len(request_plan_epochs)} "
            f"accept_record_epochs={len(accepted_plan_epochs)} "
            f"discriminating={int(payload_discriminating)})"))
    return valid


def validate_gem5_compact_reuse_bind_flowthrough_rows(
        rows: list[dict[str, Any]], args: argparse.Namespace,
        policies: list[PolicySpec]) -> None:
    """Require every requested proposal cell to attest the complete mechanism."""
    if not args.gem5_compact_reuse_bind_flowthrough or args.dry_run:
        return
    proposal_rows = [
        row for row in rows
        if str(row.get(
            "gem5_compact_reuse_bind_flowthrough_requested", "0")) == "1"
    ]
    target_labels = {
        spec.label for spec in policies
        if (
            spec.policy == "ECG" and
            spec.ecg_mode == "ECG_GRASP_POPT" and
            args.ecg_isa_variant == "computed" and
            ecg_transport_for(
                spec, args.benchmark).reuse_plan_depth == 2 and
            ecg_transport_for(
                spec, args.benchmark).flowthrough
        )
    }
    expected_keys = {
        (label, str(l3_size))
        for label in target_labels
        for l3_size in args.l3_sizes
    }
    observed_keys = {
        (str(row.get("policy_label")), str(row.get("l3_size")))
        for row in proposal_rows
    }
    failures = [
        row for row in proposal_rows
        if (
            row.get("status") != "ok" or
            str(row.get("proposal_path_active", "0")) != "1" or
            str(row.get("gem5_reuse_plan_exact_bind", "0")) != "1" or
            str(row.get("gem5_reuse_bind_payload_discriminating", "0")) != "1" or
            int(row.get("gem5_reuse_bind_nonzero_epoch_accepts") or 0) < 8 or
            int(row.get("gem5_reuse_bind_coalesced_line_accepts") or 0) <= 0 or
            int(row.get(
                "gem5_flowthrough_request_flag_size4_events") or 0) <= 0 or
            int(row.get(
                "gem5_flowthrough_request_flag_bad_size_events") or 0) != 0 or
            int(row.get("gem5_flowthrough_range_events") or 0) != 0 or
            int(row.get("gem5_flowthrough_trace_saturated") or 0) != 0 or
            int(row.get("gem5_flowthrough_all_events") or 0) !=
            int(row.get("gem5_flowthrough_request_flag_events") or 0)
        )
    ]
    if (
            observed_keys != expected_keys or
            len(proposal_rows) != len(expected_keys) or failures):
        details = [
            {
                "policy": row.get("policy_label"),
                "l3_size": row.get("l3_size"),
                "status": row.get("status"),
                "active": row.get("proposal_path_active"),
                "request_bound": row.get(
                    "gem5_reuse_plan_exact_bind"),
                "payload_discriminating": row.get(
                    "gem5_reuse_bind_payload_discriminating"),
                "coalesced_accepts": row.get(
                    "gem5_reuse_bind_coalesced_line_accepts"),
                "nonzero_epoch_accepts": row.get(
                    "gem5_reuse_bind_nonzero_epoch_accepts"),
                "stream_size4": row.get(
                    "gem5_flowthrough_request_flag_size4_events"),
                "stream_bad_size": row.get(
                    "gem5_flowthrough_request_flag_bad_size_events"),
                "error": row.get("error"),
            }
            for row in failures
        ]
        raise SystemExit(
            "proposal compact ReuseBind+FlowThrough gate failed: "
            f"expected={sorted(expected_keys)} "
            f"observed={sorted(observed_keys)} failures={details}")


def apply_gem5_variant_receipt(
        row: dict[str, Any], log_text: str,
        requested: str, required: bool,
        expected_dueling: int = 0) -> bool:
    """Attest the executing ECG victim variant rather than trusting config."""
    match = re.search(
        r"\[ECG-VARIANT-RECEIPT sim=gem5 requested=([^ ]+) "
        r"effective=(\d+) dueling=(\d+)\]", log_text)
    if not match:
        if required:
            mark_row_error(
                row, "ECG victim variant receipt missing from gem5 output")
        return False
    actual_requested = match.group(1)
    effective = int(match.group(2))
    dueling = int(match.group(3))
    expected_effective = {
        "grasp_only": 0, "epoch_first": 1, "rrip_first": 2,
        "epoch_only": 3, "shortcircuit": 4, "legacy": 4,
        "degree_first": 5, "traversal": 5, "lru_only": 6,
    }.get(requested)
    row["gem5_variant_requested_receipt"] = actual_requested
    row["gem5_variant_effective_receipt"] = effective
    row["gem5_variant_dueling_receipt"] = dueling
    valid = (
        actual_requested == requested and
        expected_effective == effective and
        dueling == expected_dueling)
    if required and not valid:
        mark_row_error(row, (
            "ECG victim variant receipt mismatch: "
            f"expected {requested}/{expected_effective}/"
            f"dueling={expected_dueling}, got "
            f"{actual_requested}/{effective}/dueling={dueling}"))
    return valid


def apply_sniper_variant_receipt(
        row: dict[str, Any], log_text: str,
        requested: str, required: bool,
        expected_dueling: int = 0) -> bool:
    """Attest the executing Sniper ECG victim variant rather than trusting
    config, mirroring apply_gem5_variant_receipt's [ECG-VARIANT-RECEIPT]
    contract but matching Sniper's own receipt (``sim=sniper``).

    Sniper's variant/dueling decision is driven by the SAME shared
    ecg_policy::OnlineDuelingSelector as gem5 (a marker/sideband-governed
    per-set decision), but Sniper has no O3 Request/MSHR to bind a victim
    to. This function -- and the ``sniper_variant_*`` fields it writes --
    must never be conflated with gem5's Request-bound attestation; see
    ``sniper_reuse_bind_dueling_model`` for the explicit distinction.
    """
    match = re.search(
        r"\[ECG-VARIANT-RECEIPT sim=sniper requested=([^ ]+) "
        r"effective=(\d+) dueling=(\d+)\]", log_text)
    if not match:
        if required:
            mark_row_error(
                row, "ECG victim variant receipt missing from Sniper output")
        return False
    actual_requested = match.group(1)
    effective = int(match.group(2))
    dueling = int(match.group(3))
    expected_effective = {
        "grasp_only": 0, "epoch_first": 1, "rrip_first": 2,
        "epoch_only": 3, "shortcircuit": 4, "legacy": 4,
        "degree_first": 5, "traversal": 5, "lru_only": 6,
    }.get(requested)
    row["sniper_variant_requested_receipt"] = actual_requested
    row["sniper_variant_effective_receipt"] = effective
    row["sniper_variant_dueling_receipt"] = dueling
    valid = (
        actual_requested == requested and
        expected_effective == effective and
        dueling == expected_dueling)
    if required and not valid:
        mark_row_error(row, (
            "ECG victim variant receipt mismatch: "
            f"expected {requested}/{expected_effective}/"
            f"dueling={expected_dueling}, got "
            f"{actual_requested}/{effective}/dueling={dueling}"))
    return valid


def ecg_epoch_region(benchmark: str) -> str:
    return {
        "pr": "contrib", "bfs": "parent", "sssp": "dist",
        "bc": "depth,path_counts", "cc": "comp",
    }.get(benchmark, "")


def property_regions(benchmark: str) -> str:
    return {
        "pr": "scores,contrib",
        "bfs": "parent",
        "sssp": "dist",
        "bc": "scores,depth,path_counts,deltas",
        "cc": "comp",
    }.get(benchmark, "")


def gem5_ecg_epoch_region_indices(benchmark: str) -> str:
    return {
        "pr": "1",
        "bc": "1,2",
    }.get(benchmark, "0")


def cache_sim_ecg_epoch_region_indices(benchmark: str) -> str:
    return {
        "pr": "1",
        "bc": "0,1",
    }.get(benchmark, "0")


def effective_ecg_variant(
        args: argparse.Namespace, reuse_plan_depth: int | None = None,
        spec: PolicySpec | None = None) -> str:
    requested = spec.ecg_variant if spec else None
    if requested is None:
        requested = os.environ.get("ECG_VARIANT")
    if requested is None:
        if reuse_plan_depth is None:
            reuse_plan_depth = requested_ecg_reuse_plan_depth()
        requested = "adaptive" if reuse_plan_depth == 2 else "rrip_first"
    if requested != "adaptive":
        return requested
    benchmark = str(args.benchmark).lower()
    if benchmark in ("bfs", "sssp"):
        return "degree_first"
    if benchmark == "pr":
        return "epoch_first"
    # BC/CC mix frontier work with backward/pointer-chasing phases; rrip_first
    # is the measured do-no-harm arm and remains the safe adaptive fallback.
    return "rrip_first"


def sniper_mask_mode_ecg_variant(
        args: argparse.Namespace, reuse_plan_depth: int | None,
        spec: PolicySpec) -> str:
    """Compute the ECG_VARIANT Sniper's mask-mode (ReuseBind / ``--ecg-isa-variant
    mask``) transport exports, called from run_sniper's mask branch.

    Every ``ECG:REUSE_PLAN_*`` PolicySpec pins its own ``ecg_variant`` (e.g.
    ``ECG:REUSE_PLAN_LRU_FLOWTHROUGH`` -> "lru_only", ``ECG:REUSE_PLAN_DEGREE`` ->
    "degree_first", ``ECG:REUSE_PLAN_RRIP_FLOWTHROUGH`` -> "rrip_first",
    ``ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH`` -> "rrip_first" with dueling enabled) and
    that pin MUST reach the child Sniper process unchanged and MUST be what
    the receipt validator (apply_sniper_variant_receipt) checks against --
    anything else would let the runner silently execute (and certify) a
    different variant than the one requested. Only a spec with genuinely no
    pinned variant (``spec.ecg_variant is None``, i.e. it reached
    ECG_GRASP_POPT mode through the generic "ECG:<mode>" parser path rather
    than a named ``REUSE_PLAN_*`` spec) falls back to the generic ECG:REUSE_PLAN
    adaptive-benchmark mapping, matching gem5/cache_sim's own default.
    """
    if spec.ecg_variant is None:
        return effective_ecg_variant(
            args, reuse_plan_depth=2, spec=parse_policy_spec("ECG:REUSE_PLAN"))
    return effective_ecg_variant(args, reuse_plan_depth, spec)


def run_cache_sim(args: argparse.Namespace, out_dir: Path, spec: PolicySpec, l3_size: str) -> list[dict[str, Any]]:
    if spec.policy == "HAWKEYE" and spec.label != "HAWKEYE_PROXY":
        raise RuntimeError(
            "cache_sim has no instruction PC; use HAWKEYE:PROXY or run "
            "the faithful HAWKEYE policy in gem5.")
    binary = PROJECT_ROOT / "bench" / "bin_sim" / args.benchmark
    label = f"cache_sim_{args.benchmark}_{spec.safe_label}_L3{sanitize(l3_size)}"
    json_path = out_dir / "cache_sim" / f"{label}.json"
    log_path = out_dir / "logs" / f"{label}.log"
    json_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(binary)] + shlex.split(args.options)
    setarch = shutil.which("setarch")
    if setarch:
        cmd = [setarch, platform.machine(), "-R", *cmd]
    elif args.require_cache_sim_aslr_disable:
        raise RuntimeError(
            "Controlled cache_sim runs require setarch -R, but setarch is unavailable.")
    charge = policy_cache_geometry(args, spec, l3_size)
    effective_l3_size = str(charge["popt_effective_l3_size"])
    effective_l3_ways = str(charge["popt_effective_l3_ways"])
    env = cache_sim_env(args, spec, effective_l3_size, effective_l3_ways, json_path)

    result = run_command(cmd, PROJECT_ROOT, env, args.timeout_cache, log_path, args.dry_run)
    if args.dry_run:
        return []

    row = base_row("cache_sim", args, spec, l3_size, charge)
    row.update({"section": 0, "log_path": str(log_path), "json_path": str(json_path)})
    if result is None or result.returncode != 0:
        row.update({"status": "error", "error": f"exit_code={result.returncode if result else 'unknown'}"})
        return [row]
    if not json_path.exists():
        row.update({"status": "error", "error": "missing cache_sim json"})
        return [row]

    data = json.loads(json_path.read_text())
    log_text = log_path.read_text(errors="ignore")
    metadata_receipt = re.search(
        r"\[ECG-METADATA [^\]]*record_bytes=(\d+)"
        r"[^\]]*bytes_per_edge=([0-9.]+)[^\]]*\]",
        log_text)
    if metadata_receipt:
        record_bytes = int(metadata_receipt.group(1))
        row["ecg_receipt_bytes_per_edge"] = float(
            metadata_receipt.group(2))
        row["ecg_record_bytes"] = record_bytes
        if "[ECG_COMPACT_REUSE_PLAN_WEIGHTED64]" in log_text:
            row["ecg_record_replaces_edge"] = 1
            row["edge_stream_bytes_per_edge"] = 8
        elif int(row.get("ecg_record_replaces_edge") or 0):
            row["edge_stream_bytes_per_edge"] = record_bytes
        else:
            row["edge_stream_bytes_per_edge"] = (
                int(row.get("graph_edge_bytes") or 0) + record_bytes)
    row.update({
        "status": "ok",
        "total_accesses": data.get("total_accesses"),
        "memory_accesses": data.get("memory_accesses"),
    })
    for key in (
        "prefetch_requests",
        "prefetch_cache_hits",
        "prefetch_fills",
        "prefetch_useful",
        "prefetch_evicted_before_use",
        "prefetch_pending",
        "total_memory_traffic",
        "prefetch_distinct_pages_4k",
        "prefetch_distinct_pages_2m",
        "prefetch_mtlb_entries",
        "prefetch_mtlb_misses",
        # Off-chip traffic in BOTH directions. total_memory_traffic counts
        # reads only; a policy that changes which dirty lines are resident
        # changes writebacks independently, which can reorder write-heavy
        # kernels such as PageRank and CC.
        "llc_writebacks",
        "total_offchip_traffic",
        # Prefetcher provenance: the model actually used plus its issue,
        # throttle and training counts, so an oracle row cannot be mistaken
        # for an honest one and an unbounded issue rate is visible.
        "stream_prefetch_model",
        "stream_prefetch_issued",
        "stream_prefetch_throttled",
        "stream_prefetch_untrained",
        # P-OPT matrix-stream provenance.
        "popt_matrix_stream_lines_simulated",
        "popt_matrix_stream_columns_simulated",
    ):
        row[key] = data.get(key)
    fills = row.get("prefetch_fills") or 0
    requests = row.get("prefetch_requests") or 0
    if fills:
        row["prefetch_fill_useful_rate"] = (row.get("prefetch_useful") or 0) / fills
    if requests:
        row["prefetch_request_fill_rate"] = fills / requests
        row["prefetch_request_cache_hit_rate"] = (row.get("prefetch_cache_hits") or 0) / requests
    for level in ("L1", "L2", "L3"):
        stats = data.get(level, {})
        prefix = level.lower()
        hit_rate = stats.get("hit_rate")
        row[f"{prefix}_hit_rate"] = hit_rate
        row[f"{prefix}_miss_rate"] = None if hit_rate is None else 1.0 - float(hit_rate)
        row[f"{prefix}_hits"] = stats.get("hits")
        row[f"{prefix}_misses"] = stats.get("misses")
        row[f"{prefix}_policy"] = stats.get("policy")
        # PROPERTY (irregular, latency-critical) vs STRUCTURE (read-once streamed)
        # miss split — the honest replacement-policy metric (structure is bandwidth,
        # hidden by the stream prefetcher; property is what the policy governs).
        ph, pm = stats.get("prop_hits"), stats.get("prop_misses")
        row[f"{prefix}_prop_hits"], row[f"{prefix}_prop_misses"] = ph, pm
        if ph is not None and pm is not None and (ph + pm) > 0:
            row[f"{prefix}_prop_miss_rate"] = pm / (ph + pm)
            tm = stats.get("misses")
            row[f"{prefix}_struct_misses"] = None if tm is None else tm - pm
        else:
            row[f"{prefix}_prop_miss_rate"] = None
            row[f"{prefix}_struct_misses"] = None
    transport = ecg_transport_for(spec, args.benchmark)
    log_text = log_path.read_text(errors="ignore")
    if "[ECG_COMPACT_REUSE_PLAN_WEIGHTED64]" in log_text:
        row.update({
            "graph_edge_bytes": 8,
            "ecg_record_bytes": 8,
            "edge_stream_bytes_per_edge": 8,
            "ecg_record_replaces_edge": 1,
        })
    if transport.flowthrough:
        expected = (
            "[ECG-FLOWTHROUGH sim=cache_sim active=1 adaptive=1]"
            if transport.flowthrough_adaptive else
            "[ECG-FLOWTHROUGH sim=cache_sim active=1")
        if expected not in log_text:
            row["status"] = "error"
            row["error"] = "FlowThrough requested but cache_sim FlowThrough path was inactive"
    apply_overhead_metrics(row)
    row.update(parse_ecg_log_stats(log_path))
    return [row]


def run_gem5(args: argparse.Namespace, out_dir: Path, spec: PolicySpec, l3_size: str) -> list[dict[str, Any]]:
    label = f"gem5_{args.benchmark}_{spec.safe_label}_L3{sanitize(l3_size)}"
    gem5_out = out_dir / "gem5" / label
    log_path = out_dir / "logs" / f"{label}.log"
    sidebands = gem5_sideband_paths(gem5_out)
    charge = policy_cache_geometry(args, spec, l3_size)
    if args.prefetcher == "ECG_PFX" and not args.allow_gem5_ecg_pfx:
        row = base_row("gem5", args, spec, l3_size, charge)
        row.update({
            "section": 0,
            "log_path": str(log_path),
            "gem5_out": str(gem5_out),
            "status": "unsupported",
            "error": "ECG_PFX gem5 timing path is experimental; pass --allow-gem5-ecg-pfx only after rebuilding gem5 with the ECG_PFX SimObject scaffold.",
        })
        return [row]
    binary = VALIDATED_GEM5_GUEST
    if binary is None:
        if args.dry_run:
            binary = PROJECT_ROOT / "bench" / "bin_gem5" / (
                f"{args.benchmark}{GEM5_KERNEL_SUFFIX}")
        else:
            raise RuntimeError("gem5 guest was not validated")
    if (selected_gem5_isa() == "riscv" and
            not (args.dry_run and not VALIDATED_GEM5_GUEST_SHA256)):
        verify_staged_guest(binary, VALIDATED_GEM5_GUEST_SHA256)
    effective_l3_size = str(charge["popt_effective_l3_size"])
    effective_l3_ways = str(charge["popt_effective_l3_ways"])

    cmd = [
        str(GEM5_OPT),
        f"--outdir={gem5_out}",
        str(GEM5_CONFIG),
        "--binary", str(binary),
        "--options", args.options,
        "--policy", spec.policy,
        "--prefetcher", args.prefetcher,
        "--prefetcher-level", args.prefetcher_level,
        "--structure-prefetch-degree", str(args.structure_prefetch_degree),
        "--l1d-size", args.l1d_size,
        "--l2-size", args.l2_size,
        "--l3-size", effective_l3_size,
        "--l3-ways", effective_l3_ways,
        "--cpu-type", args.gem5_cpu_type,
    ]
    if int(args.gem5_max_insts) > 0:
        cmd.extend(["--max-insts", str(int(args.gem5_max_insts))])
    if args.prefetcher == "DROPLET":
        cmd.extend([
            "--droplet-prefetch-degree", str(args.droplet_prefetch_degree),
            "--droplet-indirect-degree", str(args.droplet_indirect_degree),
            "--droplet-stride-table-size", str(args.droplet_stride_table_size),
        ])
    if args.prefetcher == "ECG_PFX":
        cmd.extend([
            "--ecg-pfx-lookahead", str(args.ecg_pfx_lookahead),
            "--ecg-pfx-hint-filter", str(args.ecg_pfx_hint_filter),
            "--ecg-pfx-delivery", str(args.ecg_pfx_delivery),
        ])
    if spec.ecg_mode:
        cmd.extend(["--ecg-mode", spec.ecg_mode])

    if not args.dry_run:
        if gem5_out.exists():
            shutil.rmtree(gem5_out)
        sidebands["context"].parent.mkdir(parents=True, exist_ok=True)
        clear_sideband_files(sidebands)

    gem5_ecg_delivery = ""
    gem5_ecg_epoch_channel = ""
    gem5_ecg_context_id = ""
    ecg_variant = ""
    env = dict(os.environ)
    scrub_cell_mechanism_env(env)
    apply_explicit_cell_mechanism_env(env, spec)
    transport = ecg_transport_for(spec, args.benchmark)
    apply_ecg_transport_env(env, transport)
    is_reuse_plan_ecg = (
        spec.policy == "ECG" and
        spec.ecg_mode == "ECG_GRASP_POPT" and
        transport.reuse_plan_depth == 2)
    compact_fused_requested = (
        bool(args.gem5_compact_fused) or
        env.get("GEM5_ECG_COMPACT_FUSED") == "1")
    compact_reuse_bind_verify_requested = (
        bool(getattr(args, "gem5_compact_reuse_bind_flowthrough", False)))
    compact_reuse_bind_performance_requested = (
        bool(getattr(args, "gem5_compact_reuse_bind_performance", False)))
    compact_reuse_bind_flowthrough_requested = (
        compact_reuse_bind_verify_requested or
        compact_reuse_bind_performance_requested)
    if compact_fused_requested and args.benchmark != "pr":
        raise RuntimeError(
            "fused compact ReusePlan is implemented only for gem5 PageRank; "
            f"benchmark={args.benchmark!r} would run a wide load while being "
            "labelled compact")
    compact_fused_cell_requested = (
        is_reuse_plan_ecg and compact_fused_requested)
    if compact_fused_cell_requested:
        env["GEM5_ECG_COMPACT_FUSED"] = "1"
    else:
        env.pop("GEM5_ECG_COMPACT_FUSED", None)
    compact_reuse_bind_flowthrough_cell_requested = (
        is_reuse_plan_ecg and compact_reuse_bind_flowthrough_requested and
        transport.flowthrough and args.ecg_isa_variant == "computed")
    if compact_reuse_bind_flowthrough_cell_requested:
        env.update({
            "GEM5_ECG_COMPACT_REUSE_BIND_FLOW": "1",
            "ECG_RECORD_VARIABLE_WIDTH": "1",
            "ECG_EXPECT_BYTES_PER_EDGE": "4",
        })
        if compact_reuse_bind_verify_requested:
            env["ECG_FLOWTHROUGH_TRACE"] = "2048"
            env["ECG_REUSE_PLAN_DELIVERY_TRACE"] = "2048"
            if transport.set_dueling:
                env["ECG_FLOWTHROUGH_TRACE"] = "131072"
                env["ECG_REUSE_PLAN_DELIVERY_TRACE"] = "131072"
        else:
            disable_gem5_event_traces(env)
    else:
        env.pop("GEM5_ECG_COMPACT_REUSE_BIND_FLOW", None)
    reuse_bind_isa_name = (
        "load" if args.ecg_isa_variant == "computed" else "iload")
    if is_reuse_plan_ecg:
        env["GEM5_ECG_ISA_VARIANT"] = args.ecg_isa_variant
        env["GEM5_ECG_EPOCH_CSR"] = "1"
        gem5_ecg_epoch_channel = "csr"
        gem5_ecg_context_id = "runtime-monotonic"
    requested_ecg_load = os.environ.get("GEM5_FORCE_ECG_LOAD") == "1"
    env.pop("GEM5_FORCE_ECG_LOAD", None)
    env.pop("GEM5_FORCE_ECG_PLOAD", None)
    env.pop("GEM5_FORCE_ECG_FLOW_LOAD", None)
    env.pop("GEM5_FORCE_ECG_PLAN_LOAD", None)
    env.pop("GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND", None)
    env["GEM5_GRAPHBREW_CTX"] = str(sidebands["context"])
    env["GEM5_POPT_MATRIX"] = str(sidebands["popt_matrix"])
    env["GEM5_GRAPHBREW_OUT_EDGES"] = str(sidebands["out_edges"])
    env["GEM5_GRAPHBREW_IN_EDGES"] = str(sidebands["in_edges"])
    epoch_region = ecg_epoch_region(args.benchmark)
    if epoch_region:
        env["GEM5_ECG_EPOCH_REGION_INDICES"] = (
            gem5_ecg_epoch_region_indices(args.benchmark))
    if spec.ecg_mode == "ECG_GRASP_POPT":
        env.update({
            "GEM5_ECG_PFX_MODE": "6",
            "ECG_PREFETCH_MODE": "6",
            "ECG_EDGE_MASK_EPOCH": "1",
            "ECG_EDGE_MASK_LINEMIN": "1",
            "ECG_EDGE_MASK_EPOCHS": str(args.ecg_epochs),
            "ECG_EDGE_MASK_PACK_BITS": str(args.ecg_epoch_pack_bits),
        })
        riscv_delivery = (
            "RISCV" in str(GEM5_OPT).upper()
            or "_riscv" in str(GEM5_KERNEL_SUFFIX).lower()
        )
        if is_reuse_plan_ecg and args.ecg_isa_variant == "computed" and not riscv_delivery:
            raise RuntimeError(
                "gem5 ReuseBind requires the RISC-V custom load path; "
                "X86 packed/extract fallback cannot be labeled computed-address.")
        ecg_variant = effective_ecg_variant(
            args, transport.reuse_plan_depth, spec)
        env["ECG_VARIANT"] = ecg_variant
        reuse_plan_depth = transport.reuse_plan_depth if is_reuse_plan_ecg else 0
        if reuse_plan_depth not in (0, 2):
            raise RuntimeError(
                "gem5 Schedule-K delivery currently supports only "
                "ECG_REUSE_PLAN_DEPTH=2.")
        if reuse_plan_depth == 2 and (
                args.benchmark not in ("pr", "bfs", "sssp", "bc", "cc")
                or args.prefetcher not in ("none", "STRIDE")):
            raise RuntimeError(
                "gem5 two-epoch ReusePlan is implemented for PR/BFS/SSSP/BC/CC with "
                "prefetcher none or STRIDE.")
        force_delivery = os.environ.get("ECG_FORCE_DELIVERY") == "1"
        if reuse_plan_depth == 2 and args.benchmark in (
                "pr", "bfs", "sssp", "bc", "cc"):
            reuse_bind_iload = (
                riscv_delivery and
                args.benchmark in ("pr", "bfs", "sssp", "bc", "cc"))
            # Explicit ablation of the fused computed-address property load (ReuseBind-Indexed). Every
            # fused delivery -- ecg.bind.iload, ecg.plan.load, ecg.flow.load --
            # carries the CANONICAL 64-bit record and has no 32-bit variant, so
            # a compact record must be widened in software before it can be
            # used. That software widen is precisely the cost under study, so
            # isolating width from decode requires turning the whole fused
            # family off and putting every arm on the plain
            # packed+ecg.extract2 delivery.
            fused_record_load_allowed = (
                os.environ.get("GRAPHBREW_REUSE_PLAN_FUSED_LOAD") != "0")
            if not fused_record_load_allowed:
                reuse_bind_iload = False
            if args.gem5_cpu_type == "O3" and not reuse_bind_iload:
                raise RuntimeError(
                    "two-epoch ReusePlan O3 requires the RISC-V masked property-load "
                    "path with its request-bound ReusePlan producer.")
            env.pop("GEM5_FORCE_ECG_LOAD", None)
            if reuse_bind_iload:
                env["GEM5_FORCE_ECG_PLOAD"] = "1"
                if args.gem5_cpu_type == "O3":
                    env["GEM5_ECG_PRODUCER"] = "1"
            else:
                env.pop("GEM5_FORCE_ECG_PLOAD", None)
            if (riscv_delivery and
                    env.get("ECG_FLOWTHROUGH") == "1"):
                env["GEM5_FORCE_ECG_FLOW_LOAD"] = "1"
                env["GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND"] = "1"
                env.pop("GEM5_FORCE_ECG_PLAN_LOAD", None)
                gem5_ecg_delivery = (
                    f"ecg.flow.wload+ecg.bind.{reuse_bind_isa_name}"
                    if reuse_bind_iload and args.benchmark == "sssp"
                    else f"ecg.flow.load+ecg.bind.{reuse_bind_isa_name}"
                    if reuse_bind_iload
                    else "ecg.flow.wload" if args.benchmark == "sssp"
                    else "ecg.flow.load")
            elif reuse_bind_iload:
                env.pop("GEM5_FORCE_ECG_PLAN_LOAD", None)
                env.pop("GEM5_FORCE_ECG_FLOW_LOAD", None)
                env.pop("GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND", None)
                gem5_ecg_delivery = f"ecg.bind.{reuse_bind_isa_name}"
            elif riscv_delivery and fused_record_load_allowed:
                env["GEM5_FORCE_ECG_PLAN_LOAD"] = "1"
                env.pop("GEM5_FORCE_ECG_FLOW_LOAD", None)
                env.pop("GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND", None)
                gem5_ecg_delivery = (
                    "ecg.wplan_load" if args.benchmark == "sssp"
                    else "ecg.plan.load")
            else:
                env.pop("GEM5_FORCE_ECG_PLAN_LOAD", None)
                env.pop("GEM5_FORCE_ECG_FLOW_LOAD", None)
                env.pop("GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND", None)
                gem5_ecg_delivery = "packed8+reuse_plan+ecg.extract2"
        elif riscv_delivery and (ecg_variant != "grasp_only" or force_delivery):
            if args.benchmark == "pr":
                # PR already has a 4-byte packed edge-record path (dest+epoch)
                # followed by register-only ecg.extract. Do NOT force the 8-byte
                # WIDE ecg.load record in performance runs; keep it as an
                # explicit GEM5_FORCE_ECG_LOAD=1 ISA-cost ablation.
                if args.gem5_cpu_type == "O3":
                    env["GEM5_FORCE_ECG_PLOAD"] = "1"
                    env["GEM5_ECG_PRODUCER"] = "1"
                    env["GEM5_ECG_EPOCH_CSR"] = "1"
                    gem5_ecg_delivery = "ecg.pload-request-bound"
                elif requested_ecg_load:
                    gem5_ecg_delivery = "ecg.load"
                    env["GEM5_FORCE_ECG_LOAD"] = "1"
                elif args.prefetcher == "ECG_PFX":
                    path_a = int(effective_ecg_pfx_value(
                        args, "ECG_EDGE_MASK_PREFETCH"
                    )) > 0
                    gem5_ecg_delivery = (
                        "packed4+ecg.extract+pathA"
                        if path_a
                        else "wide64+ecg.extract+pathB"
                    )
                else:
                    gem5_ecg_delivery = "packed4+ecg.extract"
            elif args.benchmark in ("bfs", "bc", "cc", "sssp"):
                env["GEM5_FORCE_ECG_PLOAD"] = "1"
                gem5_ecg_delivery = "ecg.pload"
    if args.prefetcher == "ECG_PFX":
        env.update(ecg_pfx_env(args))
        env["GEM5_ENABLE_ECG_PFX_HINTS"] = "1"
        env["GEM5_ECG_PFX_LOOKAHEAD"] = effective_ecg_pfx_value(args, "ECG_PREFETCH_LOOKAHEAD")

    reuse_plan_sidecar: Path | None = None
    reuse_plan_sidecar_hash = ""
    if is_reuse_plan_ecg and args.benchmark == "pr":
        if (
                env.get("ECG_RECORD_VARIABLE_WIDTH") == "1" and
                "ECG_EXPECT_BYTES_PER_EDGE" not in env and
                "ECG_EDGE_RECORD_BYTES" not in env):
            raise RuntimeError(
                "ECG_RECORD_VARIABLE_WIDTH=1 requires an explicit "
                "ECG_EXPECT_BYTES_PER_EDGE or ECG_EDGE_RECORD_BYTES value")
        record_bytes = int(
            env.get(
                "ECG_EXPECT_BYTES_PER_EDGE",
                env.get("ECG_EDGE_RECORD_BYTES", "8")))
        reuse_plan_sidecar = ensure_reuse_plan_sidecar(
            args, env, record_bytes)
        env["GEM5_REUSE_PLAN_SIDECAR"] = str(reuse_plan_sidecar)
        env["GEM5_REUSE_PLAN_SIDECAR_REQUIRED"] = "1"
        if not args.dry_run:
            reuse_plan_sidecar_hash = hash_input_path(
                reuse_plan_sidecar)

    pass_fds: list[int] = []
    with ExitStack() as runtime:
        if not args.dry_run:
            gem5_hash = (
                args.expected_gem5_opt_sha256 or hash_input_path(GEM5_OPT))
            gem5_fd, sealed_gem5 = open_sealed_guest(GEM5_OPT, gem5_hash)
            runtime.callback(os.close, gem5_fd)
            pass_fds.append(gem5_fd)
            cmd[0] = sealed_gem5

            runtime_files = {}
            guest_data = binary.read_bytes()
            if hashlib.sha256(guest_data).hexdigest() != \
                    VALIDATED_GEM5_GUEST_SHA256:
                raise RuntimeError("gem5 guest changed while sealing inputs")
            runtime_files[binary.name] = (guest_data, 0o555)
            graph = graph_path_from_options(args.options)
            if graph is not None:
                graph_hash = (
                    args.expected_graph_sha256 or hash_input_path(graph))
                graph_data = graph.read_bytes()
                if hashlib.sha256(graph_data).hexdigest() != graph_hash:
                    raise RuntimeError("graph changed while sealing inputs")
                runtime_files[graph.name] = (graph_data, 0o444)
            if reuse_plan_sidecar is not None:
                sidecar_data = reuse_plan_sidecar.read_bytes()
                if hashlib.sha256(sidecar_data).hexdigest() != \
                        reuse_plan_sidecar_hash:
                    raise RuntimeError(
                        "ReusePlan sidecar changed while sealing inputs")
                runtime_files[reuse_plan_sidecar.name] = (
                    sidecar_data, 0o444)

            runtime_mount = out_dir / fixed_runtime_mount_name("runtime")
            runtime.enter_context(
                immutable_fuse_files(runtime_files, runtime_mount))
            sealed_guest = runtime_mount / binary.name
            if hash_input_path(sealed_guest) != VALIDATED_GEM5_GUEST_SHA256:
                raise RuntimeError("served guest hash mismatch")
            cmd[cmd.index("--binary") + 1] = str(sealed_guest)
            if graph is not None:
                sealed_graph = runtime_mount / graph.name
                if hash_input_path(sealed_graph) != graph_hash:
                    raise RuntimeError("served graph hash mismatch")
                options = shlex.split(args.options)
                options[options.index("-f") + 1] = str(sealed_graph)
                cmd[cmd.index("--options") + 1] = shlex.join(options)
            if reuse_plan_sidecar is not None:
                sealed_sidecar = runtime_mount / reuse_plan_sidecar.name
                if hash_input_path(sealed_sidecar) != \
                        reuse_plan_sidecar_hash:
                    raise RuntimeError(
                        "served ReusePlan sidecar hash mismatch")
                env["GEM5_REUSE_PLAN_SIDECAR"] = str(sealed_sidecar)

            config_hash = (
                args.expected_gem5_config_sha256 or
                hash_input_path(GEM5_CONFIG.parent))
            config_files = {}
            config_paths = sorted(
                path for path in GEM5_CONFIG.parent.rglob("*")
                if path.is_file() and
                "__pycache__" not in path.parts and
                path.suffix not in {".pyc", ".log"})
            if any(
                    path.parent != GEM5_CONFIG.parent or path.suffix != ".py"
                    for path in config_paths):
                raise RuntimeError(
                    "gem5 config sealing supports only the current flat "
                    "Python module set")
            for path in config_paths:
                data = path.read_bytes()
                config_files[path.name] = (data, 0o444)
            if hash_input_path(GEM5_CONFIG.parent) != config_hash:
                raise RuntimeError("gem5 config changed while sealing inputs")
            config_mount = out_dir / fixed_runtime_mount_name("config")
            runtime.enter_context(
                immutable_fuse_files(config_files, config_mount))
            if hash_input_path(config_mount) != config_hash:
                raise RuntimeError("served gem5 config hash mismatch")
            cmd[2] = str(config_mount / GEM5_CONFIG.name)

        result = run_command(
            cmd, PROJECT_ROOT, env, args.timeout_gem5, log_path,
            args.dry_run, tuple(pass_fds))
    if (not args.dry_run and
            hash_input_path(binary) != VALIDATED_GEM5_GUEST_SHA256):
        raise RuntimeError("gem5 guest changed after execution")
    if (not args.dry_run and reuse_plan_sidecar is not None and
            hash_input_path(reuse_plan_sidecar) !=
            reuse_plan_sidecar_hash):
        raise RuntimeError("ReusePlan sidecar changed after execution")
    if args.dry_run:
        return []

    base = base_row("gem5", args, spec, l3_size, charge)
    base.update({
        "gem5_guest_staged_path": str(binary),
        "gem5_guest_staged_sha256": VALIDATED_GEM5_GUEST_SHA256,
        "gem5_guest_expected_sha256":
            str(args.expected_gem5_guest_sha256),
        "gem5_compact_reuse_bind_flowthrough_requested": int(
            compact_reuse_bind_flowthrough_cell_requested),
        "gem5_compact_reuse_bind_performance_requested": int(
            compact_reuse_bind_flowthrough_cell_requested and
            compact_reuse_bind_performance_requested),
        "gem5_cpu_type": args.gem5_cpu_type,
        "gem5_reuse_bind_trace_limit": int(
            env.get("ECG_REUSE_PLAN_DELIVERY_TRACE", "0") or 0),
        "gem5_flowthrough_trace_limit": int(
            env.get("ECG_FLOWTHROUGH_TRACE", "0") or 0),
    })
    base.update({
        "gem5_opt_expected_sha256": str(args.expected_gem5_opt_sha256),
        "gem5_config_expected_sha256":
            str(args.expected_gem5_config_sha256),
        "graph_expected_sha256": str(args.expected_graph_sha256),
    })
    if gem5_ecg_epoch_channel:
        base["gem5_ecg_epoch_channel"] = gem5_ecg_epoch_channel
        base["gem5_ecg_context_id"] = gem5_ecg_context_id
    if gem5_ecg_delivery:
        base["gem5_ecg_delivery"] = gem5_ecg_delivery
    if (
            compact_reuse_bind_flowthrough_cell_requested and
            compact_reuse_bind_verify_requested):
        base["timing_model"] = "mechanism_probe_exact_request"
        base["timing_valid_for_speedup"] = "0"
        base["timing_caveat"] = (
            "Synthetic compact FlowThrough plus ReuseBind O3 correctness gate; "
            "this row is not performance evidence.")
    elif (
            compact_reuse_bind_flowthrough_cell_requested and
            compact_reuse_bind_performance_requested and
            args.has_lru_baseline and
            str(base.get("timing_valid_for_speedup")) == "1"):
        base["timing_model"] = "architectural_compact_reuse_bind_flowthrough"
        base["timing_valid_for_speedup"] = "1"
        base["timing_caveat"] = ""
    elif (
            compact_reuse_bind_flowthrough_requested and
            is_reuse_plan_ecg and not transport.flowthrough):
        base["timing_model"] = "mechanism_semantic_anchor"
        base["timing_valid_for_speedup"] = "0"
        base["timing_caveat"] = (
            "Non-FlowThrough wide-record ReusePlan semantic anchor; it is "
            "width-unmatched and is not a FlowThrough control or "
            "performance evidence.")
    if str(gem5_ecg_delivery).startswith("packed8+reuse_plan+ecg.extract2"):
        # Deliberately fail-closed and deliberately NOT relaxed for the compact
        # ISA arm. ecg.extract2c removes the software widen, but the property
        # load is still a separate instruction rather than a fused,
        # request-bound one, so this delivery remains a prototype and its
        # execution time is not speedup evidence. Its instruction counts and
        # traffic are.
        base["timing_model"] = "prototype_instruction_delivery"
        base["timing_valid_for_speedup"] = "0"
        base["timing_caveat"] = (
            "This kernel uses a packed record load followed by "
            "ecg.extract2/ecg.extract2c; use instruction counts and cache "
            "metrics, not speedup, until request-bound fused delivery carries "
            "the compact record.")
    apply_instruction_cap_provenance(base, "gem5", args)
    base.update({
        "log_path": str(log_path),
        "gem5_out": str(gem5_out),
        "gem5_sideband_dir": str(sidebands["context"].parent),
        "gem5_context_path": str(sidebands["context"]),
        "gem5_popt_matrix_path": str(sidebands["popt_matrix"]),
        "gem5_out_edges_path": str(sidebands["out_edges"]),
        "gem5_in_edges_path": str(sidebands["in_edges"]),
        "gem5_ecg_pfx_experimental": int(args.prefetcher == "ECG_PFX" and args.allow_gem5_ecg_pfx),
    })
    if log_path.exists():
        log_text = log_path.read_text(errors="ignore")
        for benchmark_log in gem5_out.rglob("benchmark_stderr.txt"):
            log_text += "\n" + benchmark_log.read_text(errors="ignore")
        base.update(parse_ecg_log_stats(log_path, log_text))
        if (
                is_reuse_plan_ecg and args.benchmark == "pr" and
                int(base.get("gem5_reuse_plan_sidecar_active") or 0) != 1):
            mark_row_error(
                base, "gem5 ReusePlan run did not consume a validated sidecar")
        compact_weighted_markers = (
            "[ECG_REUSE_PLAN_WEIGHTED64]",
            "[ECG_REUSE_BIND_LOAD_CW24]",
            "[ECG_REUSE_BIND_ILOAD_CW24]",
        )
        if any(marker in log_text for marker in compact_weighted_markers):
            compact_isa_name = (
                "load" if "[ECG_REUSE_BIND_LOAD_CW24]" in log_text else "iload")
            base.update({
                "gem5_ecg_delivery": (
                    f"ecg.flow.weighted+ecg.bind.{compact_isa_name}.cw24"
                    if transport.flowthrough
                    else f"ecg.plan.weighted+ecg.bind.{compact_isa_name}.cw24"),
                "graph_edge_bytes": 8,
                "ecg_record_bytes": 8,
                "edge_stream_bytes_per_edge": 8,
                "ecg_record_replaces_edge": 1,
            })
        if "[ECG_REUSE_BIND_LOAD" in log_text:
            base["ecg_isa_variant"] = "computed"
        elif "[ECG_REUSE_BIND_ILOAD" in log_text:
            base["ecg_isa_variant"] = "indexed"
        apply_gem5_grasp_receipt(
            base, log_text, required=spec.policy == "GRASP")
        apply_gem5_popt_receipt(
            base, log_text, required=spec.policy == "POPT")
        apply_gem5_geometry_receipt(
            base, gem5_out / "config.json",
            effective_l3_size, effective_l3_ways)
        apply_gem5_compact_fused_receipt(
            base, log_text, compact_fused_cell_requested)
        apply_gem5_compact_reuse_bind_flowthrough_receipt(
            base, log_text, compact_reuse_bind_flowthrough_cell_requested,
            require_trace_receipts=compact_reuse_bind_verify_requested,
            performance_requested=(
                compact_reuse_bind_flowthrough_cell_requested and
                compact_reuse_bind_performance_requested))
        apply_gem5_reuse_bind_receipt(
            base, log_text, (
                compact_reuse_bind_flowthrough_cell_requested and
                compact_reuse_bind_verify_requested),
            64, require_discriminating=(
                compact_reuse_bind_flowthrough_cell_requested and
                compact_reuse_bind_verify_requested),
            trace_limit=int(
                base.get("gem5_reuse_bind_trace_limit") or 0))
        if (
                compact_reuse_bind_flowthrough_cell_requested and
                compact_reuse_bind_performance_requested):
            trace_free = (
                int(base.get("gem5_reuse_bind_trace_limit") or 0) == 0 and
                int(base.get("gem5_flowthrough_trace_limit") or 0) == 0 and
                int(base.get("gem5_reuse_bind_request_trace_events") or 0) == 0 and
                int(base.get("gem5_flowthrough_all_events") or 0) == 0)
            if not trace_free:
                mark_row_error(
                    base,
                    "trace-free proposal timing emitted per-event traces")
            base["proposal_performance_mode_active"] = int(
                trace_free and
                int(base.get("proposal_performance_mode_active") or 0) == 1)
        base["gem5_flowthrough_trace_saturated"] = int(
            int(base.get("gem5_flowthrough_trace_limit") or 0) > 0 and
            int(base.get("gem5_flowthrough_all_events") or 0) >=
            int(base.get("gem5_flowthrough_trace_limit") or 0))
        if (
                compact_reuse_bind_flowthrough_cell_requested and
                int(base.get("gem5_reuse_bind_trace_saturated") or 0)):
            caveat = str(base.get("timing_caveat") or "").strip()
            trace_limit = int(
                base.get("gem5_reuse_bind_trace_limit") or 0)
            trace_events = int(
                base.get("gem5_reuse_bind_request_trace_events") or 0)
            accepts = int(base.get("gem5_reuse_bind_request_accepts") or 0)
            exact_accepts = int(
                base.get("gem5_reuse_bind_exact_vertex_accepts") or 0)
            coalesced_accepts = int(
                base.get("gem5_reuse_bind_coalesced_line_accepts") or 0)
            base["gem5_reuse_bind_accepts_per_traced_request"] = (
                accepts / trace_events if trace_events else 0.0)
            trace_caveat = (
                f"Exact binding is attested for {accepts} accepted LLC "
                f"deliveries ({exact_accepts} exact-vertex, "
                f"{coalesced_accepts} same-line coalesced) observed within "
                f"the first {trace_limit} traced ReusePlan requests; request-count "
                "coverage is not claimed. Accept traces are emitted only "
                "after the simulator dest-line guard; non-accepted traced "
                "requests are unclassified (inner-cache hit or guard "
                "rejection).")
            base["timing_caveat"] = " ".join(
                part for part in (caveat, trace_caveat) if part)
        apply_gem5_variant_receipt(
            base, log_text, ecg_variant, required=is_reuse_plan_ecg,
            expected_dueling=int(transport.set_dueling))
        # ecg_record_bytes above is a NOMINAL value derived from the schedule,
        # so it read 8 for every two-epoch ReusePlan row even when the guest streamed a
        # compact 4-byte record. Anyone re-parsing the combined CSV would have
        # concluded both width stages streamed 8 bytes. The guest receipt is the
        # only source of truth for what was actually streamed, so promote it.
        receipt = re.search(
            r"\[ECG-METADATA [^\]]*bytes_per_edge=([0-9.]+)[^\]]*\]", log_text)
        if receipt:
            width = int(float(receipt.group(1)))
            base["ecg_receipt_bytes_per_edge"] = float(receipt.group(1))
            base["ecg_record_bytes"] = width
            # edge_stream_bytes_per_edge is DERIVED from the record width, and
            # was computed earlier from the nominal one. Left alone it produced
            # rows asserting ecg_record_bytes=4, ecg_record_replaces_edge=1 and
            # edge_stream_bytes_per_edge=8 simultaneously -- a row that
            # contradicts itself, and the field a reader would most likely trust
            # when computing bytes per edge.
            if int(base.get("ecg_record_replaces_edge", 0)):
                base["edge_stream_bytes_per_edge"] = width
            elif base.get("ecg_charged") and width:
                base["edge_stream_bytes_per_edge"] = (
                    int(base.get("graph_edge_bytes", 4)) + width)
        base["ecg_compact_isa_active"] = int("[ECG_EXTRACT2C]" in log_text)
        # The delivery label was hardcoded from the env, so a cell that streamed
        # a 4-byte record and decoded it in the ISA still reported
        # "packed8+reuse_plan+ecg.extract2". Derive it from what the guest reported.
        current = str(base.get("gem5_ecg_delivery", ""))
        if current.startswith("packed8+reuse_plan+ecg.extract2"):
            width = base.get("ecg_receipt_bytes_per_edge")
            stem = "packed4" if width == 4.0 else "packed8"
            op = ("ecg.extract2c" if base.get("ecg_compact_isa_active")
                  else "ecg.extract2")
            base["gem5_ecg_delivery"] = f"{stem}+reuse_plan+{op}"
        base["gem5_metadata_fatal"] = log_text.count("[ECG-METADATA-FATAL")
        base["gem5_flowthrough_trace_events"] = log_text.count(
            "[ECG-FLOWTHROUGH sim=gem5")
        base["gem5_flowthrough_adaptive_active"] = int(
            "[ECG-FLOWTHROUGH-ADAPTIVE sim=gem5 active=1]" in log_text)
        pr_result = re.search(
            r"\[ECG-PR-RESULT iterations=(\d+) semantic_edges=(\d+) "
            r"score_checksum=([0-9a-fA-F]+)\]", log_text)
        if pr_result:
            base["pr_iterations"] = int(pr_result.group(1))
            base["pr_semantic_edges"] = int(pr_result.group(2))
            base["pr_score_checksum"] = pr_result.group(3).lower()
        if is_reuse_plan_ecg:
            base["gem5_reuse_bind_model"] = (
                "request" if args.gem5_cpu_type == "O3"
                else "serialized_mailbox")
            if args.gem5_cpu_type != "O3":
                caveat = str(base.get("timing_caveat") or "").strip()
                binding_caveat = (
                    "TimingSimpleCPU uses serialized mailbox-equivalent ReusePlan "
                    "delivery; exact request binding is proven separately by "
                    "the O3 mechanism probe.")
                base["timing_caveat"] = " ".join(
                    part for part in (caveat, binding_caveat) if part)
    if result is None or result.returncode != 0:
        base["section"] = 0
        mark_row_error(
            base,
            f"exit_code={result.returncode if result else 'unknown'}")
        return [base]

    stats_path = gem5_out / "stats.txt"
    if not stats_path.exists():
        base["section"] = 0
        mark_row_error(base, "missing stats.txt")
        return [base]

    sections = parse_gem5_sections(stats_path)
    if not sections:
        base["section"] = 0
        mark_row_error(base, "no stats sections")
        return [base]

    # The benchmark emits the first stats block at the ROI boundary. gem5 then
    # emits a second exit dump with teardown/post-ROI activity; reporting both
    # double-weights gem5 and contaminates the matrix. Keep the benchmark ROI.
    row = dict(base)
    row.update({
        "section": 1,
        "stats_path": str(stats_path),
        "gem5_stats_sections_seen": len(sections),
    })
    row.setdefault("status", "ok")
    row.update(sections[0])
    if spec.policy == "GRASP" and int(
            row.get("grasp_hot_property_accesses") or 0) <= 0:
        mark_row_error(
            row,
            "GRASP made no hot-tier property classifications in the ROI")
    validate_online_dueling_activity(row, spec.ecg_set_dueling)
    if spec.policy == "POPT" and int(
            row.get("popt_roi_rereference_queries") or 0) <= 0:
        mark_row_error(
            row,
            "P-OPT performed no phase-two rereference queries in the ROI")
    apply_overhead_metrics(row)
    if (transport.flowthrough_adaptive and
            not int(row.get("gem5_flowthrough_adaptive_active") or 0)):
        mark_row_error(
            row, "adaptive FlowThrough was requested but not active")
    return [row]


def sniper_graph_policies_enabled(args: argparse.Namespace) -> bool:
    if getattr(args, "sniper_enable_graph_policies", False):
        return True
    if not SNIPER_OVERLAY_STATUS.exists():
        return False
    try:
        status = json.loads(SNIPER_OVERLAY_STATUS.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    if status.get("sniper_head") != PINNED_SNIPER_HEAD:
        return False
    if not {"grasp", "popt", "ecg"}.issubset(
            set(status.get("policies", []))):
        return False

    sniper_root = sniper_root_path(args)
    try:
        actual_head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=sniper_root, capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return False
    if actual_head != status.get("sniper_head"):
        return False
    overlay_root = (
        PROJECT_ROOT / "bench" / "include" / "sniper_sim" / "overlays")
    copied_files = status.get("copied_files", [])
    if not isinstance(copied_files, list):
        return False
    expected_copied = sorted(
        str(path.relative_to(overlay_root))
        for path in overlay_root.rglob("*")
        if path.is_file() and
        path.suffix.lower() in {".h", ".hh", ".cc", ".cpp"}
    )
    if sorted(str(value) for value in copied_files) != expected_copied:
        return False
    file_hashes = status.get("file_hashes")
    if not isinstance(file_hashes, dict):
        return False
    for relative in copied_files:
        source = overlay_root / relative
        installed = sniper_root / relative
        if (not source.exists() or not installed.exists() or
                source.read_bytes() != installed.read_bytes() or
                hash_input_path(source) != file_hashes.get(relative) or
                hash_input_path(installed) != file_hashes.get(relative)):
            return False
    for relative in status.get("patched_files", []):
        installed = sniper_root / relative
        if (not installed.exists() or
                hash_input_path(installed) != file_hashes.get(relative)):
            return False
    binary_info = status.get("binary")
    if not isinstance(binary_info, dict):
        return False
    binary = sniper_root / str(binary_info.get("path", ""))
    if (not binary.exists() or
            binary.stat().st_size != binary_info.get("size") or
            hash_input_path(binary) != binary_info.get("sha256")):
        return False
    return True


def sniper_policy_name(args: argparse.Namespace, spec: PolicySpec) -> str | None:
    if spec.ecg_mode and spec.policy != "ECG":
        return None
    if spec.charge_popt_overhead and spec.policy not in ("POPT", "ECG"):
        return None
    if spec.policy in SNIPER_POLICY_MAP:
        return SNIPER_POLICY_MAP[spec.policy]
    if spec.policy == "ECG" and sniper_graph_policies_enabled(args):
        if spec.ecg_mode == "DBG_ONLY":
            return "grasp"
        if spec.ecg_mode == "POPT_PRIMARY":
            return "popt"
        return "ecg"
    if sniper_graph_policies_enabled(args):
        return SNIPER_GRAPH_POLICY_MAP.get(spec.policy)
    return None


def sniper_binary_and_options(args: argparse.Namespace) -> tuple[Path, list[str]]:
    if args.sniper_workload == "pr_kernel_smoke":
        if args.benchmark != "pr":
            raise SystemExit("--suite sniper --sniper-workload pr_kernel_smoke is only valid with --benchmark pr")
        return PROJECT_ROOT / "bench" / "bin_sniper" / "pr_kernel_smoke", []
    if args.sniper_workload == "kernel_smoke":
        supported = {"pr", "bfs", "sssp"}
        if args.benchmark not in supported:
            raise SystemExit(f"--suite sniper --sniper-workload kernel_smoke supports only {sorted(supported)}")
        return PROJECT_ROOT / "bench" / "bin_sniper" / f"{args.benchmark}_kernel_smoke", []
    if args.sniper_workload == "sg_kernel":
        options = shlex.split(args.options)
        if "-f" not in options and "-g" not in options:
            raise SystemExit(
                "--suite sniper --sniper-workload sg_kernel requires "
                "--options with -f graph.sg or -g scale")
        sg_binary_override = getattr(args, "sniper_sg_binary", "")
        binary = (
            Path(sg_binary_override)
            if sg_binary_override
            else PROJECT_ROOT / "bench" / "bin_sniper" / "sg_kernel")
        if not binary.is_absolute():
            binary = PROJECT_ROOT / binary
        return binary, ["--benchmark", args.benchmark, *options]
    return PROJECT_ROOT / "bench" / "bin_sniper" / args.benchmark, shlex.split(args.options)


def run_sniper(args: argparse.Namespace, out_dir: Path, spec: PolicySpec, l3_size: str) -> list[dict[str, Any]]:
    label = f"sniper_{args.benchmark}_{spec.safe_label}_L3{sanitize(l3_size)}"
    if getattr(args, "_sniper_thread_sweep", False):
        label += f"_T{sanitize(str(args.sniper_cores))}"
    sniper_out = out_dir / "sniper" / label
    log_path = out_dir / "logs" / f"{label}.log"
    sidebands = sniper_sideband_paths(sniper_out)
    charge = policy_cache_geometry(args, spec, l3_size)
    row = base_row("sniper", args, spec, l3_size, charge)
    sniper_root = sniper_root_path(args)
    sniper_runner = sniper_runner_path(args)
    unsafe_sniper_workload = args.sniper_workload in ("benchmark", "sg_kernel")
    binary, binary_options = sniper_binary_and_options(args)
    sniper_binary = sniper_root / "lib" / "sniper"
    row.update({
        "section": 0,
        "log_path": str(log_path),
        "sniper_out": str(sniper_out),
        "sniper_root": str(sniper_root),
        "sniper_runner": str(sniper_runner),
        "sniper_sideband_dir": str(sidebands["context"].parent),
        "sniper_context_path": str(sidebands["context"]),
        "sniper_popt_matrix_path": str(sidebands["popt_matrix"]),
        "sniper_out_edges_path": str(sidebands["out_edges"]),
        "sniper_in_edges_path": str(sidebands["in_edges"]),
        "sniper_workload": args.sniper_workload,
        "sniper_workload_binary": str(binary),
        "sniper_workload_sha256": cached_file_sha256(str(binary.resolve())),
        "sniper_simulator_binary": str(sniper_binary),
        "sniper_simulator_sha256": cached_file_sha256(
            str(sniper_binary.resolve())),
        "sniper_cores": args.sniper_cores,
        "sniper_frontend": args.sniper_frontend,
        "sniper_omp_wait_policy": args.sniper_omp_wait_policy,
        "sniper_base_config": args.sniper_base_config,
        "sniper_extra_configs": " ".join(args.sniper_config),
        "sniper_queue_model": args.sniper_queue_model,
        "sniper_address_domain": args.sniper_address_domain,
        "sniper_mimicos_memory_mb": args.sniper_mimicos_memory_mb,
        "sniper_mimicos_kernel_mb": args.sniper_mimicos_kernel_mb,
        "threads": args.sniper_cores,
        "sniper_metric_scope": "loads_only_cache_stats",
        "sniper_overlays_enabled": int(sniper_graph_policies_enabled(args)),
    })
    if unsafe_sniper_workload:
        row["sniper_memory_limit_gb"] = args.sniper_memory_limit_gb

    if spec.policy == "ECG" and spec.ecg_mode in ("DBG_ONLY", "POPT_PRIMARY"):
        row["sniper_policy_alias_for"] = spec.ecg_mode

    if args.sniper_workload == "benchmark" and not args.allow_sniper_benchmark_workload:
        row.update({
            "status": "unsupported",
            "error": "Full bench/bin_sniper wrappers are disabled by default after the tiny PR SDE/SIFT probe consumed about 53 GiB RSS; pass --allow-sniper-benchmark-workload only for bounded run-mode debugging.",
        })
        return [row]

    if args.prefetcher == "ECG_PFX":
        if not sniper_graph_policies_enabled(args):
            row.update({
                "status": "unsupported",
                "error": "Sniper ECG_PFX requires overlays from scripts/setup_sniper.py --apply-overlays.",
            })
            return [row]

    if args.sniper_workload == "sg_kernel" and not args.allow_sniper_sg_kernel_workload:
        row.update({
            "status": "unsupported",
            "error": "bench/bin_sniper/sg_kernel is native-clean for .sg load+ROI diagnostics, but under Sniper/SDE it repeated the ~50 GiB runaway child-process behavior; pass --allow-sniper-sg-kernel-workload only for tightly bounded run-mode debugging.",
        })
        return [row]

    if args.prefetcher == "DROPLET" and not sniper_graph_policies_enabled(args):
        row.update({
            "status": "unsupported",
            "error": "Sniper DROPLET requires overlays from scripts/setup_sniper.py --apply-overlays.",
        })
        return [row]

    policy_name = sniper_policy_name(args, spec)
    if policy_name is None:
        supported = "LRU/SRRIP"
        if not sniper_graph_policies_enabled(args):
            supported += "; apply overlays with scripts/setup_sniper.py --apply-overlays for GRASP/POPT"
        row.update({
            "status": "unsupported",
            "error": f"Sniper runner currently supports {supported}; POPT/ECG overlays are still Phase 3 work.",
        })
        return [row]

    if not args.dry_run:
        if not sniper_runner.exists():
            row.update({"status": "error", "error": f"missing run-sniper: {sniper_runner}"})
            return [row]
        if not binary.exists():
            row.update({"status": "error", "error": f"missing Sniper benchmark binary: {binary}"})
            return [row]
        sidebands["context"].parent.mkdir(parents=True, exist_ok=True)
        clear_sideband_files(sidebands)

    l1_kb = format_sniper_kb(args.l1d_size)
    l2_kb = format_sniper_kb(args.l2_size)
    line_size = parse_size_bytes(args.line_size)
    l3_kb, sniper_l3_ways, sniper_l3_bytes = sniper_l3_geometry(args, l3_size, charge)
    row.update({
        "sniper_l3_config_kb": l3_kb,
        "sniper_l3_config_ways": sniper_l3_ways,
        "sniper_l3_config_bytes": sniper_l3_bytes,
    })

    cmd = [
        str(sniper_runner),
        "--roi",
    ]
    # Keep Sniper's cache-warming fast-forward enabled so the ROI starts from the
    # same warmed-but-stat-reset state as cache_sim and gem5. The detailed region
    # is still bounded by a fixed instruction budget.
    if int(args.sniper_roi_icount) > 0:
        cmd.extend(["-s", f"stop-by-icount:{int(args.sniper_roi_icount)}"])
    if args.sniper_frontend == "sift":
        cmd.append("--sift")
    cmd.extend([
        "-n", str(args.sniper_cores),
        "-d", str(sniper_out),
        "-c", args.sniper_base_config,
    ])
    for config_name in args.sniper_config:
        cmd.extend(["-c", config_name])
    sniper_config_values = {
        "general/total_cores": args.sniper_cores,
        # Disable the periodic barrier clock-skew scheme via a -g override (config
        # files do not reliably override base.cfg's scheme=barrier here). The
        # barrier client/server are the source of both the multicore OpenMP
        # deadlock and an intermittent SIGSEGV; cache miss rate is capacity/reuse
        # dominated and is unchanged by the scheme (validated within noise).
        "clock_skew_minimization/scheme": "none",
        "perf_model/l1_icache/cache_block_size": line_size,
        "perf_model/l1_dcache/cache_block_size": line_size,
        "perf_model/l2_cache/cache_block_size": line_size,
        "perf_model/l1_dcache/cache_size": l1_kb,
        "perf_model/l1_dcache/associativity": args.l1d_ways,
        "perf_model/l1_dcache/replacement_policy": "lru",
        "perf_model/l2_cache/cache_size": l2_kb,
        "perf_model/l2_cache/associativity": args.l2_ways,
        "perf_model/l2_cache/replacement_policy": "lru",
        "perf_model/nuca/cache_size": l3_kb,
        "perf_model/nuca/associativity": sniper_l3_ways,
        "perf_model/nuca/replacement_policy": policy_name,
        "perf_model/nuca/queue_model/type": args.sniper_queue_model,
        "perf_model/dram/queue_model/type": args.sniper_queue_model,
        "perf_model/dram/cache/queue_model/type": args.sniper_queue_model,
        "network/emesh_hop_by_hop/queue_model/type":
            args.sniper_queue_model,
        "network/bus/queue_model/type": args.sniper_queue_model,
        "perf_model/reserve_thp/memory_size": args.sniper_mimicos_memory_mb,
        "perf_model/reserve_thp/kernel_size": args.sniper_mimicos_kernel_mb,
    }
    sniper_config_values["general/translation_enabled"] = "false" if args.sniper_address_domain == "virtual" else "true"
    if args.prefetcher == "DROPLET":
        prefetch_config = "l1_dcache" if args.prefetcher_level == "l1d" else "l2_cache"
        sniper_config_values[f"perf_model/{prefetch_config}/prefetcher"] = "droplet"
        sniper_config_values[f"perf_model/{prefetch_config}/prefetcher/droplet/prefetch_degree"] = args.droplet_prefetch_degree
        sniper_config_values[f"perf_model/{prefetch_config}/prefetcher/droplet/indirect_degree"] = args.droplet_indirect_degree
        sniper_config_values[f"perf_model/{prefetch_config}/prefetcher/droplet/stride_table_size"] = args.droplet_stride_table_size
    elif args.prefetcher == "ECG_PFX":
        prefetch_config = "l1_dcache" if args.prefetcher_level == "l1d" else "l2_cache"
        sniper_config_values[f"perf_model/{prefetch_config}/prefetcher"] = "ecg_pfx"
    elif args.prefetcher == "STRIDE":
        # Uniform structure-stream prefetcher (leveling, ALL policies):
        # Sniper's built-in "simple" next-line/stream prefetcher. Mirrors
        # cache_sim CACHE_STREAM_PREFETCH_DEGREE and the gem5 StridePrefetcher,
        # so the sequential structure stream is hidden identically on all three.
        prefetch_config = "l1_dcache" if args.prefetcher_level == "l1d" else "l2_cache"
        pfx = f"perf_model/{prefetch_config}/prefetcher"
        sniper_config_values[pfx] = "simple"
        sniper_config_values[f"{pfx}/simple/flows"] = 16
        sniper_config_values[f"{pfx}/simple/flows_per_core"] = "false"
        sniper_config_values[f"{pfx}/simple/num_prefetches"] = args.structure_prefetch_degree
        sniper_config_values[f"{pfx}/simple/stop_at_page_boundary"] = "false"
    for key, value in sniper_config_values.items():
        cmd.extend(["-g", f"{key}={value}"])
    cmd.extend(["--", str(binary), *binary_options])

    if unsafe_sniper_workload:
        try:
            cmd = memory_limited_command(cmd, float(args.sniper_memory_limit_gb))
        except RuntimeError as exc:
            row.update({"status": "error", "error": str(exc)})
            return [row]

    # Disable ASLR so the simulated workload's heap arrays land at fixed
    # addresses every run. Sniper models physical cache set-indexing on those
    # addresses, so ASLR alone produces run-to-run miss-rate swings (the graph
    # property/edge arrays randomly collide in sets). cache_sim/gem5 use fixed
    # addresses; setarch -R gives Sniper the same determinism. No-op if absent.
    setarch = shutil.which("setarch")
    if setarch:
        cmd = [setarch, platform.machine(), "-R", *cmd]
        row["sniper_aslr_disabled"] = 1
    elif args.require_sniper_aslr_disable:
        raise RuntimeError(
            "Controlled Sniper runs require setarch -R, but setarch is unavailable.")

    env = dict(os.environ)
    scrub_cell_mechanism_env(env)
    apply_explicit_cell_mechanism_env(env, spec)
    if args.ecg_isa_variant == "computed":
        apply_sniper_transport_cell_env(env)
    semantic_edge_limit = int(args.sniper_semantic_edge_limit)
    if semantic_edge_limit > 0:
        env["SNIPER_SEMANTIC_EDGE_LIMIT"] = str(semantic_edge_limit)
    else:
        env.pop("SNIPER_SEMANTIC_EDGE_LIMIT", None)
    env.pop("SNIPER_ECG_FUSED_REUSE_PLAN", None)
    env.pop("SNIPER_ECG_FUSED_VALIDATE", None)
    env.pop("SNIPER_REUSE_PLAN_TRANSPORT_MATCHED", None)
    env.pop("SNIPER_REUSE_PLAN_EXACT_BIND", None)
    transport = ecg_transport_for(spec, args.benchmark)
    apply_ecg_transport_env(env, transport)
    is_reuse_plan_ecg = policy_name == "ecg" and spec.ecg_mode == "ECG_GRASP_POPT"
    if args.ecg_isa_variant == "computed":
        env["SNIPER_REUSE_PLAN_TRANSPORT_MATCHED"] = "1"
        env["SNIPER_REUSE_PLAN_EXACT_BIND"] = "1"
        env["SNIPER_ENABLE_ECG_EXTRACT"] = "1"
        env["SNIPER_ECG_FUSED_REUSE_PLAN"] = "1"
        env["ECG_REUSE_PLAN_DEPTH"] = "2"
        env["ECG_EDGE_MASK_EPOCHS"] = str(args.ecg_epochs)
        env["ECG_REUSE_PLAN_VALIDATE"] = "1"
        row["sniper_transport_matched"] = 1
        row["sniper_reuse_bind_exact"] = 1
        row["sniper_reuse_plan_epoch_context_bound"] = 1
        row["sniper_transport_receipts_validated"] = 0
        row["sniper_reuse_bind_exact_validated"] = 0
        row["sniper_reuse_plan_epoch_context_validated"] = 0
        transport_record_bytes = explicit_ecg_record_bytes(8)
        row["sniper_transport_record_bytes"] = transport_record_bytes
        row["sniper_transport_bytes_per_edge"] = transport_record_bytes
        row["timing_model"] = "transport_matched_diagnostic"
        row["timing_valid_for_speedup"] = "0"
        row["timing_caveat"] = (
            "Transport-matched ReuseBind certification row; timing remains "
            "diagnostic because Sniper models rather than executes the "
            "architectural epoch/context CSR channel.")
    if (is_reuse_plan_ecg and args.ecg_isa_variant == "computed" and
            args.sniper_require_fused_receipts):
        require_sniper_reuse_plan_certification_budget(env)
    if policy_name == "popt":
        popt_fast = (
            "0" if os.environ.get("SNIPER_POPT_FAST") == "0" else "1")
        env["SNIPER_POPT_FAST"] = popt_fast
        row["sniper_popt_fast"] = int(popt_fast == "1")
    env["OMP_NUM_THREADS"] = str(args.sniper_cores)
    # Multi-core OpenMP under Sniper deadlocks with PASSIVE waits: idle threads
    # call futex_wait with no timeout, so when every core is sleeping at a barrier
    # at once Sniper's barrier_sync_server aborts ("No threads running, no
    # timeout. Application has deadlocked"). ACTIVE spin-waits keep the threads
    # runnable, so the deadlock never fires. Passive is kept for single-core runs
    # (one thread never blocks at a barrier, and passive avoids spin cache-noise).
    wait_policy = args.sniper_omp_wait_policy
    try:
        _sniper_core_count = int(args.sniper_cores)
    except (TypeError, ValueError):
        _sniper_core_count = 1
    if _sniper_core_count > 1 and wait_policy != "active":
        print(f"[sniper] cores={_sniper_core_count} > 1: forcing OMP_WAIT_POLICY=active "
              f"(passive deadlocks multi-core OpenMP under Sniper)")
        wait_policy = "active"
    if wait_policy != "unset":
        env["OMP_WAIT_POLICY"] = wait_policy
    else:
        env.pop("OMP_WAIT_POLICY", None)
    env["SNIPER_GRAPHBREW_CTX"] = str(sidebands["context"])
    env["SNIPER_POPT_MATRIX"] = str(sidebands["popt_matrix"])
    requires_popt_matrix = policy_name == "popt"
    row["sniper_popt_matrix_required"] = int(requires_popt_matrix)
    if requires_popt_matrix:
        env["SNIPER_REQUIRE_POPT_MATRIX"] = "1"
    else:
        env.pop("SNIPER_REQUIRE_POPT_MATRIX", None)
    env["SNIPER_GRAPHBREW_OUT_EDGES"] = str(sidebands["out_edges"])
    env["SNIPER_GRAPHBREW_IN_EDGES"] = str(sidebands["in_edges"])
    env["SNIPER_GRAPHBREW_PREFETCHER"] = str(args.prefetcher)
    env["SNIPER_CACHE_LINE_SIZE"] = str(args.line_size)
    epoch_region = ecg_epoch_region(args.benchmark)
    if epoch_region:
        env["SNIPER_ECG_EPOCH_REGION"] = epoch_region
    env["SNIPER_ECG_VERTICES_PER_LINE"] = str(
        max(1, int(args.line_size) // 4))
    # Level the simulated instruction stream across every policy while exposing
    # the real outer-vertex clock required by P-OPT/ECG. Without this, the
    # SNIPER_SET_VERTEX calls are no-ops and graph policies fall back to a
    # property-address-derived pseudo-clock.
    env["SNIPER_ENABLE_VERTEX_HINTS"] = "1"
    row["sniper_vertex_clock"] = "outer-vertex"
    if args.prefetcher == "ECG_PFX":
        env.update(ecg_pfx_env(args))
        env["SNIPER_ENABLE_ECG_PFX_HINTS"] = "1"
        env["SNIPER_ECG_PFX_LOOKAHEAD"] = effective_ecg_pfx_value(args, "ECG_PREFETCH_LOOKAHEAD")
        env["SNIPER_ECG_PFX_MODE"] = effective_ecg_pfx_value(args, "ECG_PREFETCH_MODE")
        env["SNIPER_ECG_PFX_HINT_FILTER"] = str(args.ecg_pfx_hint_filter)
        env["SNIPER_ECG_PFX_FILTER_ELEM_SIZE"] = "4"
        env["SNIPER_ECG_PFX_FILTER_LINE_SIZE"] = str(args.line_size)
    if spec.ecg_mode and policy_name == "ecg":
        env["SNIPER_ECG_MODE"] = spec.ecg_mode
    ecg_variant = effective_ecg_variant(
        args, transport.reuse_plan_depth, spec)
    env["ECG_VARIANT"] = ecg_variant
    if args.ecg_isa_variant == "computed":
        env["SNIPER_ECG_MODE"] = "ECG_GRASP_POPT"
        env["ECG_MODE"] = "ECG_GRASP_POPT"
        # Preserve any spec-pinned variant (e.g. ECG:REUSE_PLAN_LRU_FLOWTHROUGH ->
        # "lru_only") instead of unconditionally overwriting it with the
        # generic ECG:REUSE_PLAN adaptive mapping; see sniper_mask_mode_ecg_variant.
        ecg_variant = sniper_mask_mode_ecg_variant(
            args, transport.reuse_plan_depth, spec)
        env["ECG_VARIANT"] = ecg_variant
        env["ECG_EDGE_MASKS"] = "1"
        env["SNIPER_POPT_FAST"] = "1"
    reuse_plan_depth = transport.reuse_plan_depth if is_reuse_plan_ecg else 0
    if reuse_plan_depth not in (0, 2):
        raise RuntimeError(
            "Sniper Schedule-K delivery currently supports only "
            "ECG_REUSE_PLAN_DEPTH=2.")
    if reuse_plan_depth == 2 and (
            args.benchmark not in ("pr", "bfs", "sssp", "bc", "cc")
            or args.prefetcher not in ("none", "STRIDE")):
        raise RuntimeError(
            "Sniper two-epoch ReusePlan is implemented for PR/BFS/SSSP/BC/CC with "
            "prefetcher none or STRIDE.")
    if reuse_plan_depth == 2 and args.sniper_workload != "sg_kernel":
        raise RuntimeError(
            "Sniper two-epoch ReusePlan requires --sniper-workload sg_kernel; "
            "the smoke/full-wrapper workloads do not emit extract2 pairs.")
    if (env.get("ECG_FLOWTHROUGH") == "1" and
            args.sniper_workload != "sg_kernel"):
        raise RuntimeError(
            "Sniper FlowThrough requires --sniper-workload sg_kernel; "
            "the smoke/full-wrapper workloads do not export packed-stream ranges.")
    force_delivery = os.environ.get("ECG_FORCE_DELIVERY") == "1"
    fused_reuse_plan = False
    fused_validation = False
    reuse_plan_trace_requested = (
        env.get("ECG_REUSE_PLAN_DELIVERY_TRACE", "0") not in ("", "0"))
    cold_mechanism_proof = (
        args.sniper_require_fused_receipts or
        (reuse_plan_depth == 2 and reuse_plan_trace_requested)
    )
    if cold_mechanism_proof:
        cmd.insert(cmd.index("--roi") + 1, "--no-cache-warming")
        row["sniper_cache_warming"] = 0
    else:
        row["sniper_cache_warming"] = 1
    if (spec.ecg_mode == "ECG_GRASP_POPT" and policy_name == "ecg"
            and (reuse_plan_depth == 2 or ecg_variant != "grasp_only"
                 or force_delivery)):
        # Performance-equivalent to gem5/cache_sim: consume the delivered
        # per-edge epoch, not Sniper's stronger live findNextRef oracle.
        env["SNIPER_ENABLE_ECG_EXTRACT"] = "1"
        env["ECG_EDGE_MASK_EPOCHS"] = str(args.ecg_epochs)
        fused_reuse_plan = (
            reuse_plan_depth == 2 and args.sniper_workload == "sg_kernel"
        )
        if fused_reuse_plan:
            env["SNIPER_ECG_FUSED_REUSE_PLAN"] = "1"
            fused_validation = cold_mechanism_proof
            if fused_validation:
                env["SNIPER_ECG_FUSED_VALIDATE"] = "1"
        row["sniper_ecg_delivery"] = (
            "matched-reuse_bind-sideband-model"
            if fused_reuse_plan and args.ecg_isa_variant == "computed"
            else "fused-reuse_plan-weighted32-model"
            if fused_reuse_plan and args.benchmark == "sssp"
            else "fused-reuse_plan-model" if fused_reuse_plan
            else "per-edge-extract2-reuse_plan" if reuse_plan_depth == 2
            else "per-edge-extract")
        if fused_reuse_plan:
            if args.ecg_isa_variant == "computed":
                row["timing_model"] = "matched_computed_address_sideband_model"
                row["timing_valid_for_speedup"] = "0"
                row["timing_caveat"] = (
                    "All policies use transport-matched, runtime-receipted "
                    "record loops, and exact governed-load binding is "
                    "validated; the "
                    "architectural epoch/context CSR remains modeled, so use "
                    "this row for instruction-parity validation.")
            else:
                row["timing_model"] = "fused_record_load_sideband_model"
                row["timing_valid_for_speedup"] = "0"
                row["timing_caveat"] = (
                    "The packed record load is the Sniper fused-delivery event; "
                    "non-tracing runs execute no per-edge SimMagic or "
                    "software-only delivery call. Sniper remains "
                    "scale/direction corroboration, not an architectural "
                    "ReuseBind speedup authority.")
        elif reuse_plan_depth == 2:
            row["timing_model"] = "prototype_explicit_magic_delivery"
            row["timing_valid_for_speedup"] = "0"
            row["timing_caveat"] = (
                "This kernel still emits per-edge SimMagic for ReusePlan delivery; "
                "use cache metrics, not speedup.")
    elif args.ecg_isa_variant != "computed":
        env.pop("SNIPER_ENABLE_ECG_EXTRACT", None)
    apply_instruction_cap_provenance(row, "sniper", args)
    apply_semantic_cap_provenance(row, "sniper", args)
    result = run_command(cmd, PROJECT_ROOT, env, args.timeout_sniper, log_path, args.dry_run)
    if args.dry_run:
        return []

    if result is None or result.returncode != 0:
        clear_sniper_reuse_plan_sidebands(sidebands)
        row.update({"status": "error", "error": f"exit_code={result.returncode if result else 'unknown'}"})
        return [row]
    log_text = log_path.read_text(errors="ignore")
    semantic_patterns = {
        "pr": r"GraphBrew Sniper SG PR checksum:\s*(.+)",
        "bfs": r"GraphBrew Sniper SG BFS reached:\s*(.+)",
        "sssp": r"GraphBrew Sniper SG SSSP reached/checksum:\s*(.+)",
        "bc": r"GraphBrew Sniper SG BC checksum:\s*(.+)",
        "cc": r"GraphBrew Sniper SG CC components:\s*(.+)",
    }
    semantic_match = re.search(
        semantic_patterns.get(args.benchmark, r"$^"), log_text)
    row["sniper_semantic_result"] = (
        semantic_match.group(1).strip() if semantic_match else "")
    semantic_limit = int(args.sniper_semantic_edge_limit)
    if semantic_limit > 0:
        work_matches = re.findall(
            r"\[SEMANTIC-ROI benchmark=([a-z]+) "
            r"edge_visits=(\d+) limit=(\d+) truncated=([01])\]",
            log_text)
        if not work_matches:
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": "Sniper semantic edge-limit marker missing",
            })
            return [row]
        if len(work_matches) != 1:
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": (
                    "Sniper semantic edge-limit marker must appear "
                    "exactly once"),
            })
            return [row]
        marker_benchmark, visits_text, marker_limit_text, truncated_text = (
            work_matches[0])
        visits = int(visits_text)
        marker_limit = int(marker_limit_text)
        truncated = int(truncated_text)
        row.update({
            "sniper_semantic_edge_visits": visits,
            "sniper_semantic_truncated": truncated,
        })
        if (marker_benchmark != args.benchmark or
                marker_limit != semantic_limit or
                visits > semantic_limit or
                (truncated and visits != semantic_limit)):
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": "Sniper semantic edge-limit marker mismatch",
            })
            return [row]
    metadata_receipt = re.search(
        r"\[ECG-METADATA [^\]]*record_bytes=(\d+)"
        r"[^\]]*bytes_per_edge=([0-9.]+)[^\]]*\]",
        log_text)
    if metadata_receipt:
        runtime_record_bytes = int(metadata_receipt.group(1))
        row["ecg_receipt_bytes_per_edge"] = float(
            metadata_receipt.group(2))
        row["ecg_record_bytes"] = runtime_record_bytes
        if (args.ecg_isa_variant == "computed" and
                args.benchmark != "sssp"):
            row["ecg_record_replaces_edge"] = 1
            row["edge_stream_bytes_per_edge"] = runtime_record_bytes
        elif int(row.get("ecg_record_replaces_edge") or 0):
            row["edge_stream_bytes_per_edge"] = runtime_record_bytes
    if args.ecg_isa_variant == "computed":
        if "[REUSE_PLAN_TRANSPORT_MATCHED] SSSP general 12B" in log_text:
            transport_record_bytes = 12
        elif metadata_receipt:
            transport_record_bytes = int(metadata_receipt.group(1))
        else:
            transport_record_bytes = 8
        row["sniper_transport_record_bytes"] = transport_record_bytes
        row["sniper_transport_bytes_per_edge"] = transport_record_bytes
    if args.ecg_isa_variant == "computed" and args.benchmark == "sssp":
        if "[ECG_FUSED_REUSE_PLAN_WEIGHTED64]" in log_text:
            row.update({
                "sniper_ecg_delivery": "fused-reuse_plan-weighted64-model",
                "graph_edge_bytes": 8,
                "ecg_record_bytes": 8,
                "edge_stream_bytes_per_edge": 8,
                "ecg_record_replaces_edge": 1,
            })
        elif "[ECG_FUSED_REUSE_PLAN_WEIGHTED32]" in log_text:
            row.update({
                "sniper_ecg_delivery": "fused-reuse_plan-weighted32-model",
                "graph_edge_bytes": 8,
                "ecg_record_bytes": 12,
                "edge_stream_bytes_per_edge": 12,
                "ecg_record_replaces_edge": 1,
            })
    if policy_name in ("grasp", "popt", "ecg"):
        context_marker = re.search(
            r"\[ECG-CONTEXT-READY sim=sniper loaded=1 "
            r"regions=(\d+) reref=(\d+)\]",
            log_text,
        )
        context_loaded = context_marker is not None
        row["sniper_context_loaded"] = int(context_loaded)
        if not context_loaded:
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": "Sniper graph policy completed without a loaded graph context",
            })
            return [row]
        reref_loaded = int(context_marker.group(2))
        row["sniper_rereference_loaded"] = reref_loaded
        if policy_name == "popt" and reref_loaded != 1:
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": "Sniper P-OPT completed without a loaded rereference matrix",
            })
            return [row]
        if (args.ecg_isa_variant == "computed" and policy_name != "popt"
                and reref_loaded != 0):
            clear_sniper_reuse_plan_sidebands(sidebands)
            row.update({
                "status": "error",
                "error": (
                    "Matrix-free ReuseBind row unexpectedly loaded the P-OPT "
                    "rereference matrix"),
            })
            return [row]
        if is_reuse_plan_ecg:
            apply_sniper_variant_receipt(
                row, log_text, ecg_variant, required=True,
                expected_dueling=int(transport.set_dueling))
            row["sniper_reuse_bind_dueling_model"] = "marker_population"

    raw_stats = read_sniper_stats(sniper_out)
    if not raw_stats.get("success"):
        clear_sniper_reuse_plan_sidebands(sidebands)
        row.update({"status": "error", "error": raw_stats.get("error", "missing Sniper stats")})
        return [row]

    metrics = extract_graphbrew_metrics(raw_stats)
    l1_accesses = metrics.get("l1d_loads", 0)
    l1_misses = metrics.get("l1d_load_misses", 0)
    l2_accesses = metrics.get("l2_loads", 0)
    l2_misses = metrics.get("l2_load_misses", 0)
    l3_accesses = metrics.get("llc_loads", 0)
    l3_misses = metrics.get("llc_load_misses", 0)
    row.update({
        "section": 1,
        "stats_path": metrics.get("stats_path", ""),
        "sniper_policy_config": policy_name,
        "sim_ticks": metrics.get("cycles_or_time", 0),
        "instructions": metrics.get("instructions", 0),
        "ipc": metrics.get("ipc_raw", 0.0),
        "l1_accesses": l1_accesses,
        "l1_misses": l1_misses,
        "l1_miss_rate": miss_rate(l1_misses, l1_accesses),
        "l2_accesses": l2_accesses,
        "l2_misses": l2_misses,
        "l2_miss_rate": miss_rate(l2_misses, l2_accesses),
        "l3_accesses": l3_accesses,
        "l3_misses": l3_misses,
        "l3_miss_rate": miss_rate(l3_misses, l3_accesses),
        "l1_policy": "LRU",
        "l2_policy": "LRU",
        "l3_policy": policy_name.upper(),
    })
    # Metrics population must never resurrect a row that an earlier gate
    # (e.g. apply_sniper_variant_receipt) already failed via mark_row_error;
    # only claim "ok" here if nothing upstream already marked "error".
    if row.get("status") != "error":
        row["status"] = "ok"
    apply_sniper_geometry_receipt(row, sniper_out, l3_kb, sniper_l3_ways)
    apply_overhead_metrics(row)
    # In certification mode the runner asks for a fixed ReusePlan delivery-trace
    # budget; require that full budget so a single paired transaction cannot
    # stand in for the whole trace.
    try:
        reuse_plan_trace_budget = int(env.get("ECG_REUSE_PLAN_DELIVERY_TRACE", "0") or 0)
    except ValueError:
        reuse_plan_trace_budget = 0
    fused_count, fused_bad = validate_sniper_fused_receipts(
        log_path, sidebands)
    bind_count, bind_bad = validate_sniper_exact_bind_trace(
        log_path, reuse_plan_trace_budget)
    row["sniper_reuse_bind_trace_budget"] = reuse_plan_trace_budget
    row["sniper_fused_reuse_plan_receipts"] = fused_count
    row["sniper_fused_reuse_plan_bad_receipts"] = fused_bad
    row["sniper_reuse_bind_consumes"] = bind_count
    row["sniper_reuse_bind_bad_consumes"] = bind_bad
    if (fused_count > 0 and fused_bad == 0 and
            fused_count >= reuse_plan_trace_budget):
        row["sniper_transport_receipts_validated"] = 1
    if (bind_count > 0 and bind_bad == 0 and
            bind_count >= reuse_plan_trace_budget):
        row["sniper_reuse_bind_exact_validated"] = 1
        row["sniper_reuse_plan_epoch_context_validated"] = 1
    if fused_validation and (fused_count == 0 or fused_bad != 0):
        row["timing_valid_for_speedup"] = "0"
        row["timing_caveat"] = (
            row.get("timing_caveat", "") +
            " Fused ReusePlan receipt validation failed.")
    if (fused_validation and args.ecg_isa_variant == "computed" and
            (bind_count == 0 or bind_bad != 0)):
        row["status"] = "error"
        row["error"] = (
            "exact ReusePlan bind validation failed: "
            f"count={bind_count} bad={bind_bad}")
        row["timing_valid_for_speedup"] = "0"
        row["timing_caveat"] = (
            row.get("timing_caveat", "") +
            " Exact ReusePlan bind validation failed.")
    if fused_validation and (fused_count == 0 or fused_bad != 0):
        row["status"] = "error"
        row["error"] = (
            "fused ReusePlan receipt validation failed: "
            f"count={fused_count} bad={fused_bad}")
        row["timing_valid_for_speedup"] = "0"
    stats_path = Path(str(metrics.get("stats_path", "")))
    if stats_path.exists():
        stats_text = stats_path.read_text(errors="ignore")
        for field, metric in (
            ("sniper_flowthrough_reads", "flowthrough-reads"),
            ("sniper_flowthrough_writes", "flowthrough-writes"),
        ):
            match = re.search(
                rf"nuca-cache\.{re.escape(metric)}\s*=\s*(\d+)",
                stats_text)
            row[field] = int(match.group(1)) if match else 0
        # Sniper analog of gem5's OnlineDuelingStats (ecg_rp.hh): registered via
        # registerStatsMetric("ecg-online-dueling", 0, ...) in cache_set_ecg.cc,
        # only when the ReusePlan online-dueling selector was actually exercised.
        # "governed_victims" (not "request_bound_victims") because Sniper has
        # no O3 Request/MSHR to bind a victim to -- see
        # sniper_reuse_bind_dueling_model.
        for field, metric in (
            ("sniper_reuse_plan_dueling_governed_victims", "governed-victims"),
            ("sniper_reuse_plan_dueling_leader_samples", "leader-samples"),
            ("sniper_reuse_plan_dueling_follower_selections", "follower-selections"),
            ("sniper_reuse_plan_dueling_completed_windows", "completed-windows"),
            ("sniper_reuse_plan_dueling_winner_changes", "winner-changes"),
            ("sniper_reuse_plan_dueling_follower_variant_overrides",
             "follower-variant-overrides"),
        ):
            match = re.search(
                rf"ecg-online-dueling\.{re.escape(metric)}\s*=\s*(\d+)",
                stats_text)
            row[field] = int(match.group(1)) if match else 0
    if is_reuse_plan_ecg:
        validate_online_dueling_activity(
            row, transport.set_dueling,
            positive_fields=SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
            leader_samples_field="sniper_reuse_plan_dueling_leader_samples")
    if transport.flowthrough:
        flowthrough_reads = int(row.get("sniper_flowthrough_reads") or 0)
        flowthrough_writes = int(row.get("sniper_flowthrough_writes") or 0)
        log_text = log_path.read_text(errors="ignore")
        adaptive_active = (
            "[ECG-FLOWTHROUGH-ADAPTIVE sim=sniper active=1]" in log_text)
        if transport.flowthrough_adaptive and not adaptive_active:
            row["status"] = "error"
            row["error"] = (
                "adaptive FlowThrough was requested but not active")
            row["timing_valid_for_speedup"] = "0"
        elif (not transport.flowthrough_adaptive and
              (flowthrough_reads <= 0 or flowthrough_writes <= 0)):
            row["status"] = "error"
            row["error"] = (
                "FlowThrough inactive: expected positive NUCA FlowThrough "
                f"reads/writes, got {flowthrough_reads}/{flowthrough_writes}")
            row["timing_valid_for_speedup"] = "0"
    for key in (
        "pf_issued",
        "pf_fillups",
        "pf_useful",
        "pf_evicted_before_use",
        "pf_invalidated_before_use",
        "droplet_sideband_loaded",
        "droplet_edge_accesses",
        "droplet_stride_issued",
        "droplet_indirect_issued",
        "droplet_duplicate_skips",
        "ecg_pfx_sideband_loaded",
        "ecg_pfx_target_hints_seen",
        "ecg_pfx_issued",
        "ecg_pfx_duplicate_skips",
        "ecg_pfx_no_sideband",
        "ecg_pfx_invalid_target",
        "sniper_cpi_base",
        "sniper_cpi_branch",
        "sniper_cpi_data_cache",
        "sniper_cpi_data_l1",
        "sniper_cpi_data_l2",
        "sniper_cpi_data_llc",
        "sniper_cpi_data_dram",
        "sniper_cpi_sync",
        "sniper_cpi_unknown",
        "sniper_nonidle_elapsed_time",
        "sniper_idle_elapsed_time",
        "sniper_elapsed_time",
    ):
        row[key] = metrics.get(key, 0)
    if args.prefetcher == "DROPLET":
        indirect_issued = int(row.get("droplet_indirect_issued") or 0)
        prefetch_issued = int(row.get("pf_issued") or 0)
        prefetch_useful = int(row.get("pf_useful") or 0)
        if indirect_issued == 0:
            error = "DROPLET sideband loaded but no edge accesses/prefetches issued."
            if args.sniper_address_domain == "translated":
                error += " Sniper cache addresses are translated while current GraphBrew sidebands are virtual."
            row.update({
                "status": "inactive",
                "droplet_activity": "inactive",
                "droplet_useful_activity": "inactive",
                "error": error,
            })
        elif prefetch_issued == 0:
            row.update({
                "status": "active_no_fill",
                "droplet_activity": "requested_no_fill",
                "droplet_useful_activity": "no_fill",
                "error": "DROPLET saw edge accesses and generated indirect requests, but Sniper did not enqueue cache prefetch fills.",
            })
        else:
            row["droplet_activity"] = "issued"
            row["droplet_useful_activity"] = "useful" if prefetch_useful > 0 else "issued_no_useful"
    if args.prefetcher == "ECG_PFX":
        hints_seen = int(row.get("ecg_pfx_target_hints_seen") or 0)
        pfx_issued = int(row.get("ecg_pfx_issued") or 0)
        prefetch_issued = int(row.get("pf_issued") or 0)
        if hints_seen == 0:
            row.update({
                "status": "inactive",
                "ecg_pfx_activity": "inactive",
                "error": "ECG_PFX prefetcher was configured but consumed no target hints.",
            })
        elif pfx_issued == 0:
            row.update({
                "status": "active_no_fill",
                "ecg_pfx_activity": "consumed_no_prefetch",
                "error": "ECG_PFX consumed target hints but issued no cache prefetch requests.",
            })
        elif prefetch_issued == 0:
            row.update({
                "status": "active_no_fill",
                "ecg_pfx_activity": "requested_no_fill",
                "error": "ECG_PFX consumed target hints and generated prefetch requests, but Sniper did not enqueue cache prefetch fills.",
            })
        else:
            row["ecg_pfx_activity"] = "issued"
    clear_sniper_reuse_plan_sidebands(sidebands)
    return [row]


def parse_gem5_sections(stats_path: Path) -> list[dict[str, Any]]:
    text = stats_path.read_text(errors="replace")
    raw_sections = text.split("---------- Begin Simulation Statistics ----------")[1:]
    parsed = []
    for section in raw_sections:
        stats: dict[str, Any] = {}
        for out_key, gem5_key in GEM5_STAT_KEYS.items():
            match = re.search(rf"{re.escape(gem5_key)}\s+([0-9.]+)", section)
            if not match:
                continue
            value = match.group(1)
            stats[out_key] = parse_gem5_number(value)
        for out_key, stat_name in GEM5_PREFETCH_STAT_KEYS.items():
            match = re.search(rf"system\.(?:l2cache|cpu\.dcache)\.prefetcher\.{re.escape(stat_name)}\s+([0-9.]+)", section)
            if not match:
                continue
            value = match.group(1)
            stats[out_key] = parse_gem5_number(value)
        # Override L3 miss rate with DEMAND-LOAD (cpu.data) only, excluding L2
        # stream-prefetcher fills. Sniper's NUCA aggregate is handled separately.
        dm = stats.get("l3_data_misses")
        dh = stats.get("l3_data_hits")
        if dm is not None and dh is not None and (dm + dh) > 0:
            stats["l3_miss_rate"] = dm / (dm + dh)
            stats["l3_misses"] = dm
            stats["l3_accesses"] = dm + dh
        parsed.append(stats)
    return parsed


def effective_ecg_epoch_count(requested: int, reuse_plan_depth: int) -> int:
    upper = 32768 if reuse_plan_depth == 2 else 65535
    return min(max(int(requested), 2), upper)


def apply_instruction_cap_provenance(
        row: dict[str, Any], simulator: str,
        args: argparse.Namespace) -> None:
    cap = (
        int(args.gem5_max_insts)
        if simulator == "gem5"
        else int(args.sniper_roi_icount)
        if simulator == "sniper"
        else 0
    )
    row["instruction_cap"] = cap
    row["gem5_max_insts"] = (
        int(args.gem5_max_insts) if simulator == "gem5" else 0)
    row["sniper_roi_icount"] = (
        int(args.sniper_roi_icount) if simulator == "sniper" else 0)
    if cap <= 0:
        return
    row["timing_model"] = "instruction_capped_diagnostic"
    row["timing_valid_for_speedup"] = "0"
    row["timing_caveat"] = (
        f"{simulator} stops after {cap} committed detailed-ROI instructions. "
        "Policies may reach different graph progress; use cache metrics only "
        "as an instruction-capped diagnostic.")


def apply_semantic_cap_provenance(
        row: dict[str, Any], simulator: str,
        args: argparse.Namespace) -> None:
    limit = (
        int(args.sniper_semantic_edge_limit)
        if simulator == "sniper" else 0)
    row["sniper_semantic_edge_limit"] = limit
    if limit <= 0:
        return
    row["semantic_work_unit"] = "static_graph_edge_visits"
    row["semantic_work_matched"] = 0
    row["timing_model"] = "semantic_edge_capped_diagnostic"
    row["timing_valid_for_speedup"] = "0"
    existing = str(row.get("timing_caveat") or "").strip()
    caveat = (
        f"Sniper stops after {limit} static graph edge visits, or earlier "
        "if the semantic ROI completes. Compare rows only when reported "
        "edge visits and truncation state match.")
    row["timing_caveat"] = f"{existing} {caveat}".strip()


def certify_sniper_semantic_work(
        rows: list[dict[str, Any]], args: argparse.Namespace,
        policies: list[PolicySpec]) -> None:
    limit = int(args.sniper_semantic_edge_limit)
    if args.suite != "sniper" or limit <= 0:
        return

    local_policies = {spec.label for spec in policies}
    try:
        expected_from_env = json.loads(os.environ.get(
            "GRAPHBREW_EXPECTED_POLICY_LABELS", "[]"))
    except json.JSONDecodeError:
        expected_from_env = []
    expected_policies = (
        {str(label) for label in expected_from_env}
        if isinstance(expected_from_env, list) and expected_from_env
        else local_policies)
    if len(expected_policies) < 2 or local_policies != expected_policies:
        return
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in rows:
        if row.get("simulator") != "sniper":
            continue
        row["semantic_work_matched"] = 0
        key = (
            row.get("benchmark"), row.get("options"), row.get("l3_size"),
            row.get("l3_ways"), row.get("threads"),
            row.get("sniper_cores"),
        )
        groups.setdefault(key, []).append(row)

    for group_rows in groups.values():
        policy_labels = {
            str(row.get("policy_label") or row.get("policy"))
            for row in group_rows
        }
        statuses_ok = all(row.get("status") == "ok" for row in group_rows)
        work = {
            (
                int(row.get("sniper_semantic_edge_limit") or 0),
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
        policies_match = (
            policy_labels == expected_policies and
            len(group_rows) == len(expected_policies))
        work_matches = len(work) == 1 and next(iter(work))[0] == limit
        results_match = (
            len(semantic_results) == 1 and "" not in semantic_results)
        transport_matches = (
            len(transport_widths) == 1 and
            next(iter(transport_widths))[0] > 0)
        matched = (
            statuses_ok and policies_match and work_matches and
            results_match and transport_matches)
        if matched:
            for row in group_rows:
                row["semantic_work_matched"] = 1
            continue
        if not statuses_ok:
            error = "Sniper semantic policy group contains a failed row"
        elif not policies_match:
            error = "Sniper semantic policy group is incomplete"
        elif not work_matches:
            error = "Sniper semantic work differs across policy rows"
        elif not transport_matches:
            error = "Sniper transport width differs across policy rows"
        else:
            error = "Sniper semantic result differs across policy rows"
        for row in group_rows:
            if row.get("status") == "ok":
                row["status"] = "error"
                row["error"] = error


def base_row(simulator: str, args: argparse.Namespace, spec: PolicySpec, l3_size: str,
             charge: dict[str, Any] | None = None) -> dict[str, Any]:
    transport = ecg_transport_for(spec, args.benchmark)
    if simulator == "gem5" and args.gem5_cpu_type == "O3":
        timing_model = "simulated_target_time"
        timing_valid_for_speedup = "1"
        timing_caveat = ""
    elif simulator == "gem5":
        timing_model = "gem5_non_o3_diagnostic"
        timing_valid_for_speedup = "0"
        timing_caveat = (
            "Only gem5 O3 rows are architectural timing evidence; this "
            f"{args.gem5_cpu_type} row is diagnostic only.")
    elif simulator == "sniper":
        timing_model = "sniper_scale_direction_model"
        timing_valid_for_speedup = "0"
        timing_caveat = (
            "Sniper provides scale/direction corroboration only, not an "
            "architectural ReuseBind speedup result.")
    elif simulator == "cache_sim":
        timing_model = "cache_mechanism_model"
        timing_valid_for_speedup = "0"
        timing_caveat = (
            "cache_sim reports functional, replacement, and traffic evidence "
            "without architectural timing.")
    else:
        timing_model = "unknown_timing_diagnostic"
        timing_valid_for_speedup = "0"
        timing_caveat = "Unknown simulator timing is not speedup evidence."
    timing_comparison_bound = (
        "measured"
        if timing_valid_for_speedup == "1"
        else "not_speedup_evidence")
    offchip_comparison_bound = "measured"
    l3_miss_comparison_valid = 1
    if args.prefetcher == "ECG_PFX" and simulator in ("gem5", "sniper"):
        timing_model = (
            "prototype_instruction_delivery"
            if simulator == "gem5" and args.ecg_pfx_delivery == "instruction"
            else "prototype_explicit_hint_delivery"
        )
        timing_valid_for_speedup = "0"
        timing_caveat = " ".join(
            part for part in (
                timing_caveat,
                "ECG_PFX timing includes prototype benchmark-emitted hint "
                "delivery; use cache and prefetch metrics for mechanism "
                "evidence until PFX is validated as instruction-carried "
                "metadata.")
            if part)

    if not getattr(args, "has_lru_baseline", False):
        timing_model = "mechanism_probe_no_baseline"
        timing_valid_for_speedup = "0"
        timing_caveat = " ".join(
            part for part in (
                timing_caveat,
                "This invocation has no within-run LRU cell. Under the frozen "
                "comparison rules it is mechanism/correctness evidence only "
                "and cannot support a speedup claim.")
            if part)

    if (
            spec.policy == "POPT" and charge and
            int(charge.get("popt_overhead_charged", 0)) == 1 and
            simulator == "gem5" and
            getattr(args, "popt_matrix_stream", "analytic") in (
                "analytic", "analytic_prefetch_upper_bound")):
        if timing_valid_for_speedup == "1":
            timing_model = (
                "optimistic_popt_prefetch_upper_bound"
                if args.popt_matrix_stream ==
                "analytic_prefetch_upper_bound"
                else "optimistic_popt_analytic_stream")
        if timing_valid_for_speedup == "1":
            timing_comparison_bound = "popt_favorable_lower_bound"
        if args.popt_matrix_stream == "analytic_prefetch_upper_bound":
            offchip_comparison_bound = "popt_favorable_lower_bound"
            l3_miss_comparison_valid = 0
        prefetch_disclosure = (
            " The common prefetcher does not issue accesses for the analytic "
            "matrix sideband, so this sensitivity assumes perfect matrix "
            "latency hiding while still charging all matrix bytes."
            if args.popt_matrix_stream ==
            "analytic_prefetch_upper_bound" else "")
        timing_caveat = " ".join(
            part for part in (
                timing_caveat,
                "P-OPT replacement and reduced LLC capacity are simulated, "
                "while cumulative matrix-stream traffic is charged "
                "analytically and its latency is omitted; timing therefore "
                f"favors P-OPT.{prefetch_disclosure}")
            if part)

    is_reuse_plan = (
        spec.policy == "ECG" and
        spec.ecg_mode == "ECG_GRASP_POPT" and
        transport.reuse_plan_depth == 2)
    trace_free_gem5_reuse_bind = (
        simulator == "gem5" and
        bool(getattr(args, "gem5_compact_reuse_bind_performance", False)) and
        transport.flowthrough)
    if (is_reuse_plan and args.ecg_isa_variant == "computed" and
            simulator in ("gem5", "sniper") and
            not trace_free_gem5_reuse_bind):
        timing_model = "prototype_computed_address_load"
        timing_valid_for_speedup = "0"
        timing_caveat = " ".join(
            part for part in (
                timing_caveat,
                "ReuseBind timing is diagnostic unless gem5 executes the "
                "architectural compact FlowThrough record load and "
                "request-bound property load with per-event tracing disabled.")
            if part)

    effective_ecg_epochs = effective_ecg_epoch_count(
        args.ecg_epochs, transport.reuse_plan_depth)
    edge_bytes = 8 if args.benchmark == "sssp" else 4
    ecg_record_bytes = (
        explicit_ecg_record_bytes(8)
        if transport.reuse_plan_depth == 2 else 0)
    row = {
        "simulator": simulator,
        "benchmark": args.benchmark,
        "options": args.options,
        "prefetcher": args.prefetcher,
        "prefetcher_level": args.prefetcher_level,
        "timing_model": timing_model,
        "timing_valid_for_speedup": timing_valid_for_speedup,
        "timing_caveat": timing_caveat,
        "timing_comparison_bound": timing_comparison_bound,
        "offchip_comparison_bound": offchip_comparison_bound,
        "l3_miss_comparison_valid": l3_miss_comparison_valid,
        "droplet_prefetch_degree": args.droplet_prefetch_degree,
        "droplet_indirect_degree": args.droplet_indirect_degree,
        "droplet_stride_table_size": args.droplet_stride_table_size,
        "ecg_prefetch_mode": effective_ecg_pfx_value(args, "ECG_PREFETCH_MODE"),
        "ecg_prefetch_window": effective_ecg_pfx_value(args, "ECG_PREFETCH_WINDOW"),
        "ecg_prefetch_lookahead": effective_ecg_pfx_value(args, "ECG_PREFETCH_LOOKAHEAD"),
        "ecg_pfx_hint_filter": args.ecg_pfx_hint_filter,
        "ecg_pfx_delivery": args.ecg_pfx_delivery,
        # Experiment configuration recorded in every row.
        "cache_stream_prefetch_degree": args.cache_stream_prefetch_degree,
        "structure_prefetch_degree": (
            args.structure_prefetch_degree
            if args.prefetcher == "STRIDE" else 0),
        "ecg_epoch_pack_bits": args.ecg_epoch_pack_bits,
        "ecg_epochs": effective_ecg_epochs,
        "ecg_epochs_requested": args.ecg_epochs,
        "ecg_epochs_effective": effective_ecg_epochs,
        "property_regions": property_regions(args.benchmark),
        "ecg_epoch_regions": ecg_epoch_region(args.benchmark),
        "ecg_isa_variant": (
            args.ecg_isa_variant
            if is_reuse_plan
            else "baseline"),
        "ecg_isa_variant_requested": (
            args.ecg_isa_variant if is_reuse_plan else "baseline"),
        "ecg_charged": args.ecg_charged,
        "ecg_reuse_plan_depth": transport.reuse_plan_depth,
        "graph_edge_bytes": edge_bytes,
        "ecg_record_bytes": ecg_record_bytes,
        "edge_stream_bytes_per_edge": (
            edge_bytes + ecg_record_bytes
            if args.ecg_charged and ecg_record_bytes and
               args.benchmark == "sssp"
            else ecg_record_bytes
            if args.ecg_charged and ecg_record_bytes
            else edge_bytes),
        "ecg_record_replaces_edge": int(
            bool(args.ecg_charged and ecg_record_bytes and
                 args.benchmark != "sssp")),
        "ecg_flowthrough": int(transport.flowthrough),
        "ecg_flowthrough_adaptive": int(transport.flowthrough_adaptive),
        "popt_reserve_model": args.popt_reserve_model,
        "policy_label": spec.label,
        "policy": spec.policy,
        "ecg_mode": spec.ecg_mode or "",
        "ecg_variant_requested": (
            spec.ecg_variant or os.environ.get(
                "ECG_VARIANT",
                "adaptive" if transport.reuse_plan_depth == 2 else "rrip_first")
            if spec.ecg_mode else ""
        ),
        "ecg_variant_effective": (
            effective_ecg_variant(args, transport.reuse_plan_depth, spec)
            if spec.ecg_mode else ""
        ),
        "l1d_size": args.l1d_size,
        "l1d_ways": args.l1d_ways,
        "l2_size": args.l2_size,
        "l2_ways": args.l2_ways,
        "l3_size": l3_size,
        "l3_ways": args.l3_ways,
        "line_size": args.line_size,
        "l3_effective_size": (
            charge.get("popt_effective_l3_size", l3_size)
            if charge else l3_size),
        "l3_effective_ways": (
            charge.get("popt_effective_l3_ways", args.l3_ways)
            if charge else args.l3_ways),
        "l1_l2_policy": "LRU",
        # Recorded on every row: a comparison is only valid if all policies in
        # the matrix had the same FlowThrough option available. Only cache_sim
        # implements it, so other backends must say so rather than echo the
        # request and imply an equalisation that never happened.
        "stream_prefetch_model_requested": (
            getattr(args, "stream_prefetch_model", "stride")
            if simulator == "cache_sim" else "n/a"),
        "flowthrough": (
            getattr(args, "flowthrough", "off")
            if simulator == "cache_sim" else "unsupported"),
    }
    if charge:
        row.update(charge)
        row["popt_matrix_stream_requested"] = getattr(
            args, "popt_matrix_stream", "analytic")
    if spec.policy == "HAWKEYE":
        proxy = spec.label == "HAWKEYE_PROXY"
        row.update({
            "hawkeye_pc_source": (
                "static_access_site_proxy" if proxy
                else "request_instruction_pc"),
            "hawkeye_faithfulness": (
                "proxy_not_real_instruction_pc" if proxy
                else "faithful_real_instruction_pc"),
            "hawkeye_optgen_quanta": 128,
            "hawkeye_sampled_sets": 64,
        })
    return row


def sanitize(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "", text)


def certify_gem5_pr_results(
        rows: list[dict[str, Any]], args: argparse.Namespace) -> None:
    """Fail closed unless every gem5 PR policy produced the same one-sweep state."""
    if args.benchmark != "pr" or args.suite not in ("gem5", "both"):
        return
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in rows:
        if row.get("simulator") != "gem5":
            continue
        key = (
            row.get("options"), row.get("l3_size"), row.get("l3_ways"),
            row.get("prefetcher"))
        groups.setdefault(key, []).append(row)
    for group_rows in groups.values():
        ok_rows = [row for row in group_rows if row.get("status") == "ok"]
        receipts = {
            (
                row.get("pr_iterations"),
                row.get("pr_semantic_edges"),
                row.get("pr_score_checksum"),
            )
            for row in ok_rows
        }
        receipt = next(iter(receipts), (None, None, None))
        valid = (
            len(ok_rows) == len(group_rows) and len(receipts) == 1 and
            all(value is not None for value in receipt))
        for row in group_rows:
            row["pr_result_matched"] = int(valid)
        if valid:
            continue
        detail = sorted(str(value) for value in receipts)
        for row in group_rows:
            mark_row_error(row, (
                "gem5 PageRank semantic receipt mismatch or missing: "
                f"{detail}"))


def write_outputs(out_dir: Path, rows: list[dict[str, Any]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "roi_matrix.json"
    csv_path = out_dir / "roi_matrix.csv"
    json_path.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n")

    fieldnames = sorted({key for row in rows for key in row.keys()})
    with csv_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    print(f"[write] {json_path}")
    print(f"[write] {csv_path}")


def standalone_matrix_config_hash(
        args: argparse.Namespace, policies: list[PolicySpec]) -> str:
    paths: dict[str, Path] = {
        "roi_matrix": Path(__file__).resolve(),
        "policy_specs": Path(__file__).resolve().parent / "policy_specs.py",
    }
    option_parts = shlex.split(args.options)
    if "-f" in option_parts:
        index = option_parts.index("-f")
        if index + 1 < len(option_parts):
            graph = Path(option_parts[index + 1])
            paths["graph"] = (
                graph if graph.is_absolute() else PROJECT_ROOT / graph)
    if args.suite in ("cache-sim", "both"):
        setarch = shutil.which("setarch")
        paths["cache_sim_setarch"] = (
            Path(setarch) if setarch else
            PROJECT_ROOT / ".missing-cache-sim-setarch")
    if args.suite == "sniper":
        root = sniper_root_path(args)
        workload = args.sniper_workload
        binary_name = (
            "sg_kernel" if workload == "sg_kernel"
            else "pr_kernel_smoke" if workload == "pr_kernel_smoke"
            else f"{args.benchmark}_kernel_smoke"
            if workload == "kernel_smoke" else args.benchmark)
        sg_binary_override = getattr(args, "sniper_sg_binary", "")
        benchmark_binary = (
            Path(sg_binary_override)
            if workload == "sg_kernel" and sg_binary_override
            else PROJECT_ROOT / "bench" / "bin_sniper" / binary_name)
        if not benchmark_binary.is_absolute():
            benchmark_binary = PROJECT_ROOT / benchmark_binary
        paths.update({
            "sniper_runner": root / "run-sniper",
            "sniper_record_trace": root / "record-trace",
            "sniper_binary": root / "lib" / "sniper",
            "sniper_config": root / "config",
            "sniper_runtime_scripts": root / "scripts",
            "sniper_tools": root / "tools",
            "sniper_sde": root / "sde_kit" / "sde64",
            "sniper_sift_recorder": root / "sift" / "recorder" /
            "obj-intel64" / "sde_sift_recorder.so",
            "benchmark_binary": benchmark_binary,
        })
        setarch = shutil.which("setarch")
        paths["setarch"] = (
            Path(setarch) if setarch else PROJECT_ROOT / ".missing-setarch")
    if args.suite in ("gem5", "both"):
        guest_binary = PROJECT_ROOT / "bench" / "bin_gem5" / (
            f"{args.benchmark}{GEM5_KERNEL_SUFFIX}")
        paths.update({
            "gem5_binary": GEM5_OPT,
            "gem5_config": GEM5_CONFIG.parent,
            "gem5_benchmark_binary": guest_binary,
        })
        if selected_gem5_isa() == "riscv":
            paths["gem5_guest_build_receipt"] = Path(
                str(guest_binary) + ".build.json")
    if args.suite in ("cache-sim", "both"):
        paths["cache_sim_benchmark_binary"] = (
            PROJECT_ROOT / "bench" / "bin_sim" / args.benchmark)

    config = {
        key: value for key, value in vars(args).items()
        if key not in {"out_dir", "dry_run"}
    }
    material_env = {
        key: value for key, value in os.environ.items()
        if key.startswith((
            "CACHE_", "ECG_", "GEM5_", "SNIPER_", "OMP_")) and
        not key.startswith("GRAPHBREW_MATRIX_")
    }
    payload = {
        "config": config,
        "policy_labels": [spec.label for spec in policies],
        "env": material_env,
        "inputs": {
            name: hash_input_path(path.resolve())
            for name, path in paths.items()
        },
    }
    return hashlib.sha256(json.dumps(
        payload, sort_keys=True, default=str,
        separators=(",", ":")).encode()).hexdigest()


def write_completion_marker(
        out_dir: Path, args: argparse.Namespace,
        policies: list[PolicySpec], rows: list[dict[str, Any]]) -> None:
    marker = out_dir / "roi_matrix.complete.json"
    temp = out_dir / "roi_matrix.complete.json.tmp"
    try:
        expected_policy_labels = json.loads(os.environ.get(
            "GRAPHBREW_EXPECTED_POLICY_LABELS", "[]"))
    except json.JSONDecodeError:
        expected_policy_labels = []
    if not isinstance(expected_policy_labels, list):
        expected_policy_labels = []
    local_hash = standalone_matrix_config_hash(args, policies)
    config_hash = os.environ.get(
        "GRAPHBREW_MATRIX_CONFIG_HASH", local_hash)
    matrix_config_hash = os.environ.get(
        "GRAPHBREW_MATRIX_GROUP_HASH", local_hash)
    outputs = {}
    for name in ("roi_matrix.csv", "roi_matrix.json"):
        path = out_dir / name
        outputs[name] = {
            "sha256": hash_input_path(path),
            "size": path.stat().st_size,
            "rows": len(rows),
        }
    payload = {
        "complete": True,
        "all_rows_ok": bool(rows) and all(
            row.get("status") == "ok" for row in rows),
        "benchmark": args.benchmark,
        "matrix_id": os.environ.get(
            "GRAPHBREW_MATRIX_ID",
            f"{args.benchmark}_{sanitize(str(out_dir))}"),
        "shard_group": os.environ.get(
            "GRAPHBREW_SHARD_GROUP", out_dir.parent.name),
        "config_hash": os.environ.get(
            "GRAPHBREW_MATRIX_CONFIG_HASH", config_hash),
        "matrix_config_hash": matrix_config_hash,
        "comparison_config_hash": os.environ.get(
            "GRAPHBREW_COMPARISON_CONFIG_HASH", ""),
        "policy_labels": [spec.label for spec in policies],
        "expected_policy_labels": (
            expected_policy_labels or [spec.label for spec in policies]),
        "l3_sizes": [str(size) for size in args.l3_sizes],
        "threads": [
            str(value) for value in (args.threads or [args.sniper_cores])
        ] if args.suite == "sniper" else [],
        "prefetcher": args.prefetcher,
        "structure_prefetch_degree": (
            args.structure_prefetch_degree
            if args.prefetcher == "STRIDE" else 0),
        "rows": len(rows),
        "outputs": outputs,
    }
    temp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    temp.replace(marker)
    print(f"[write] {marker}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run matched cache_sim/gem5/Sniper ROI policy matrix for ECG validation."
    )
    parser.add_argument("--suite", choices=["cache-sim", "gem5", "sniper", "both"], default="both")
    parser.add_argument("--benchmark", default="pr")
    parser.add_argument("--options", default="-g 10 -k 16 -o 5 -n 1 -i 5")
    parser.add_argument("--policies", nargs="+", default=None)
    parser.add_argument("--all-policies", action="store_true", help="Use the full ECG validation policy set.")
    parser.add_argument("--gem5-cpu-type", choices=["timing", "O3", "minor"], default="timing",
                        help="gem5 CPU model (graph_se.py --cpu-type). 'timing'=TimingSimpleCPU "
                             "(in-order, default, all ECG validation used this); 'O3'=DerivO3CPU "
                             "(out-of-order) for the ecg.load OoO-pipeline evaluation. NOTE: under "
                             "O3 the ecg.load epoch must ride the per-request EcgReusePlanExtension "
                             "sideband (race-free) rather than the single-slot mailbox, which "
                             "assumes in-order load serialization.")
    parser.add_argument("--gem5-max-insts", default="0",
                        help="Cap gem5 at this many committed instructions (0 = run to completion). "
                             "Bounds gem5 on large graphs like Sniper's --sniper-roi-icount. The "
                             "benchmark's m5_reset_stats/m5_dump_stats scope the recorded window to "
                             "the ROI. Pair with GEM5_OPT=.../build/RISCV/gem5.opt + "
                             "GEM5_KERNEL_SUFFIX=_riscv_m5ops + GEM5_FORCE_ECG_LOAD=1 (host var; "
                             "maps to the benchmark's GEM5_ENABLE_ECG_LOAD=1) to exercise the "
                             "RISC-V ecg.load instruction at scale.")
    parser.add_argument("--prefetcher", choices=["none", "DROPLET", "ECG_PFX", "STRIDE"], default="none",
                        help="Prefetcher to attach. ECG_PFX is supported by cache_sim and experimental gem5/Sniper hint paths. STRIDE = uniform structure-stream prefetcher (all policies) to level the structure-prefetch axis across sims.")
    parser.add_argument("--structure-prefetch-degree", type=int, default=4,
                        help="Degree for the STRIDE structure-stream prefetcher on gem5/Sniper; mirrors cache_sim --cache-stream-prefetch-degree.")
    parser.add_argument("--prefetcher-level", choices=["l1d", "l2"], default="l2",
                        help="gem5/Sniper cache level for --prefetcher; ignored by cache_sim ECG_PFX.")
    parser.add_argument("--droplet-prefetch-degree", type=int, default=1,
                        help="DROPLET edge-stream cache lines to prefetch per trigger (artifact default: 1).")
    parser.add_argument("--droplet-indirect-degree", type=int, default=16,
                        help="DROPLET neighbor IDs to translate into property prefetches per edge line (artifact default: one 64B line of 4B IDs).")
    parser.add_argument("--droplet-stride-table-size", type=int, default=64,
                        help="DROPLET stream table entries (artifact config streams default: 64).")
    parser.add_argument("--ecg-pfx-mode", choices=sorted(ECG_PFX_MODE_VALUES), default="popt",
                        help="ECG_PFX target selection: degree/hot-neighbor mode or P-OPT-ranked mode.")
    parser.add_argument("--ecg-pfx-window", default="16",
                        help="Runtime/construction dedup window for ECG_PFX.")
    parser.add_argument("--ecg-pfx-lookahead", default="4",
                        help="Algorithm lookahead distance for ECG_PFX temporal prefetch probes.")
    parser.add_argument("--ecg-pfx-hint-filter", default="16",
                        help="Recent-target filter capacity before emitting ECG_PFX hints; 0 disables filtering.")
    parser.add_argument("--ecg-pfx-delivery", choices=["explicit-hint", "instruction"], default="explicit-hint",
                        help="ECG_PFX detailed-sim delivery path. instruction uses gem5 RISC-V ecg.extract or x86 pseudo-op scaffolds.")
    parser.add_argument("--l1d-size", default="1kB")
    parser.add_argument("--l1d-ways", default="8")
    parser.add_argument("--l2-size", default="2kB")
    parser.add_argument("--l2-ways", default="4")
    parser.add_argument("--l3-sizes", nargs="+", default=["32kB"])
    parser.add_argument("--cache-sim-omp-threads", type=int, default=1,
                        help="OMP threads for cache_sim. MUST be 1 for deterministic/reproducible "
                             "results: the parallel kernel records cache accesses in nondeterministic "
                             "interleaved order, so >1 thread gives non-reproducible, "
                             "thread-count-dependent miss counts.")
    parser.add_argument("--l3-ways", default="16")
    parser.add_argument(
        "--reuse-plan-l3-ways", type=int, default=0,
        help="Optional ReusePlan-only LLC associativity override for "
             "equal-silicon sensitivity. Baselines retain --l3-ways; "
             "0 keeps equal data capacity.")
    parser.add_argument("--line-size", default="64")
    parser.add_argument("--cache-stream-prefetch-degree", type=int, default=0,
                        help="Uniform structure-stream (next-line) prefetcher degree for the "
                             "cache_sim, applied to ALL policies (0=off, default). Faithful to "
                             "the HW stride prefetchers in GRASP/P-OPT/DROPLET; hides the read-once "
                             "structure stream so total LLC mr reflects the irregular property "
                             "accesses. NOTE: an optimistic next-line model (hides ~93-99%%); sweep "
                             "{0,1,2,4} and report prefetch_fills/total_memory_traffic for honesty.")
    parser.add_argument("--popt-property-bytes", default="4",
                        help="Vertex property bytes used to estimate P-OPT matrix column size for *_CHARGED policies.")
    parser.add_argument("--popt-active-columns", default="2",
                        help="Active rereference matrix columns charged for *_CHARGED policies (default: current+next).")
    parser.add_argument("--popt-num-epochs", default="256",
                        help="P-OPT epoch count used to estimate matrix streaming traffic for *_CHARGED policies.")
    parser.add_argument("--popt-min-data-ways", default="1",
                        help="Minimum LLC data ways kept after reserving P-OPT matrix ways.")
    parser.add_argument("--popt-reserve-model", choices=["fixed_one", "size_correct"],
                        default="fixed_one",
                        help="P-OPT reserved-LLC-way charge model for *_CHARGED policies. "
                             "'fixed_one' (legacy default, P-OPT-favorable): one streaming-buffer "
                             "way regardless of |V|. 'size_correct' (reference-compatible "
                             "P-OPT Section V.D): reserve ceil(active_columns*numLines / bytes_per_way) "
                             "ways for the resident rereference-matrix columns (scales with |V|; "
                             "marks cells popt_matrix_fits=0 when the columns cannot fit).")
    parser.add_argument("--stream-prefetch-model", choices=["stride", "oracle"],
                        default="stride",
                        help="cache_sim structure-stream prefetcher model. 'stride' "
                             "(default) detects streams from addresses alone, requires "
                             "confirmation, can mispredict, and is bounded by a finite "
                             "in-flight budget. 'oracle' asks the graph context whether an "
                             "address is property data and issues without limit; it never "
                             "mispredicts the distinction the experiment turns on, so it is "
                             "an UPPER BOUND and results depending on it are ineligible for "
                             "performance claims under the frozen metrics.")
    parser.add_argument("--flowthrough", choices=["off", "all"],
                        default="off",
                        help="Offer FlowThrough to EVERY policy, not just ReusePlan. "
                             "The CSR edge stream is "
                             "sequential and read-once for every policy, so allowing only "
                             "ReusePlan to decline to allocate it confounds 'ReusePlan replaces better' "
                             "with 'ReusePlan is the only policy allowed to use FlowThrough'. "
                             "Measured on web-Google PageRank, FlowThrough changes traffic "
                             "by -20.0%% for LRU, -5.1%% for GRASP, -2.0%% for P-OPT, "
                             "and -2.4%% for ReusePlan.")
    parser.add_argument(
        "--popt-matrix-stream",
        choices=[
            "analytic", "simulated",
            "analytic_prefetch_upper_bound",
        ],
                        default="analytic",
                        help="How P-OPT's rereference-matrix column stream is charged. "
                             "'analytic' (legacy): add a flat per-run line count to the miss "
                             "and traffic totals after the run. 'simulated': cache_sim issues "
                             "the column stream as real non-temporal accesses at each epoch "
                             "boundary, so a structure prefetcher can cover it exactly as it "
                             "covers ReusePlan's per-edge records. The analytic mode is only "
                             "symmetric with ReusePlan when no prefetcher is active; with a "
                             "prefetcher it charges P-OPT demand misses that real hardware "
                             "removes, so 'simulated' is REQUIRED for any prefetch-enabled "
                             "ReusePlan-versus-P-OPT comparison. "
                             "'analytic_prefetch_upper_bound' explicitly keeps "
                             "the analytic byte charge under a common "
                             "prefetcher while assuming perfect matrix latency "
                             "hiding; it is a P-OPT-favorable sensitivity.")
    parser.add_argument("--ecg-charged", type=int, choices=[0, 1], default=1,
                        help="ECG per-edge record DELIVERY charge. 1 (default) = software "
                             "delivery: the 8B packed record is read from memory per edge "
                             "(real bandwidth, competes for cache). 0 = ISA delivery "
                             "(ecg.extract): the record rides the demand with no extra traffic "
                             "(idealized upper bound; isolates the eviction quality from the "
                             "delivery cost).")
    parser.add_argument("--ecg-epochs", type=int, default=65535,
                        help="ECG_GRASP_POPT number of absolute epochs the per-edge mask "
                             "quantizes to (eviction-epoch resolution). Default 65535 (committed). "
                             "EFFECTIVE count = min(this, pack-bits cap). Eviction quality saturates "
                             "near ne~1024-4096; values above the sweet spot over-resolve and can "
                             "worsen the miss rate, so pair a sweet-spot ne with --ecg-epoch-pack-bits "
                             "64 to MAINTAIN it at scale (instead of collapsing to 2^(32-id_bits)).")
    parser.add_argument("--ecg-epoch-pack-bits", type=int, choices=[32, 64], default=32,
                        help="ECG per-edge epoch packed-record container width. 32 (default) = "
                             "4B fat-CSR edge word: epoch caps at 2^(32-id_bits), collapsing at "
                             "scale (committed reproductions unchanged). 64 = ISA-faithful 64-bit "
                             "packed record: full epoch resolution at any scale; the wider (8B) "
                             "record stream is honestly charged by ecgRecordBytes under CHARGED=1.")
    parser.add_argument(
        "--ecg-isa-variant",
        choices=["indexed", "computed"],
        default="indexed",
        help="ReusePlan detailed-simulator ISA: indexed = fused base+record ReuseBind-Indexed; "
             "mask = computed-address ReuseBind. Sniper uses a transport-matched "
             "diagnostic model until exact request binding lands.")
    parser.add_argument(
        "--gem5-compact-fused", action="store_true",
        help="Use PR's fused compact ReuseBind-Indexed load. Implemented only for gem5 "
             "PageRank; unsupported kernels fail instead of falling back.")
    parser.add_argument(
        "--gem5-compact-reuse-bind-flowthrough", action="store_true",
        help="Run PR's traced correctness gate: a 4-byte "
             "FlowThrough record load followed by a one-for-one "
             "computed-address ReuseBind property load.")
    parser.add_argument(
        "--gem5-compact-reuse-bind-performance", action="store_true",
        help="Run the same architectural compact ReuseBind+FlowThrough path with "
             "per-event traces disabled so gem5 target time is admissible.")
    parser.add_argument(
        "--expected-gem5-guest-sha256", default="",
        help="Require the staged RISC-V guest to match this experiment-run hash.")
    parser.add_argument("--expected-gem5-opt-sha256", default="")
    parser.add_argument("--expected-gem5-config-sha256", default="")
    parser.add_argument("--expected-graph-sha256", default="")
    parser.add_argument("--ecg-stored-refresh", type=int, choices=[0, 1], default=0,
                        help="ECG_STORED_REFRESH: re-stamp a resident LLC line's next-ref "
                             "epoch from the per-edge hint on EVERY access, INCLUDING L1/L2 "
                             "hits (an aggressive per-access LLC metadata broadcast). This is "
                             "an IDEALIZED EVICTION CEILING, NOT hardware-free: the HW-feasible "
                             "piggybacked form (--ecg-refresh-llc-only, write only when the "
                             "access actually reaches L3) recovers ~ZERO of its benefit "
                             "(== no-refresh). It closes ~2.4pp only because it does uncharged "
                             "L3 tag-writes on inner-cache hits, which cache_sim does not model. "
                             "0 (default) = feasible stale-stamp behaviour; 1 = idealized ceiling "
                             "(pair with --ecg-refresh-llc-only for the feasible measurement).")
    parser.add_argument("--ecg-refresh-llc-only", type=int, choices=[0, 1], default=0,
                        help="ECG_REFRESH_LLC_ONLY: with --ecg-stored-refresh 1, restrict the "
                             "epoch re-stamp to accesses that actually REACH L3 (miss L1+L2), so "
                             "the metadata write piggybacks a real L3 access (HW-free). This is "
                             "the FEASIBLE refresh; empirically it recovers ~0 of the aggressive "
                             "form's benefit, i.e. feasible ECG does not beat P-OPT on eviction.")
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--timeout-cache", type=int, default=600)
    parser.add_argument("--timeout-gem5", type=int, default=900)
    parser.add_argument("--timeout-sniper", type=int, default=600)
    parser.add_argument("--allow-gem5-ecg-pfx", action="store_true",
                        help="Run experimental gem5 ECG_PFX timing path. Requires rebuilt gem5 overlays; default is an explicit unsupported row.")
    parser.add_argument("--sniper-workload", choices=["pr_kernel_smoke", "kernel_smoke", "sg_kernel", "benchmark"], default="pr_kernel_smoke",
                        help="Use a fast fixed kernel smoke, file-backed .sg kernel, or the full bench/bin_sniper/<benchmark> wrapper.")
    parser.add_argument(
        "--sniper-sg-binary", default="",
        help="Optional sg_kernel binary override for isolated validation without "
             "replacing the canonical bench/bin_sniper/sg_kernel.")
    parser.add_argument("--allow-sniper-benchmark-workload", action="store_true",
                        help="Allow full bench/bin_sniper/<benchmark> under Sniper. Unsafe until SDE/SIFT run mode is fixed; guarded by --sniper-memory-limit-gb.")
    parser.add_argument("--allow-sniper-sg-kernel-workload", action="store_true",
                        help="Allow file-backed bench/bin_sniper/sg_kernel under Sniper. Native .sg runs are clean, but Sniper/SDE sg_kernel repeated the high-memory runaway; use only for bounded run-mode debugging guarded by --sniper-memory-limit-gb.")
    parser.add_argument("--sniper-memory-limit-gb", type=float, default=16.0,
                        help="Address-space limit applied with prlimit to explicitly allowed unsafe Sniper benchmark/sg_kernel workloads. Set 0 to disable only for manual debugging.")
    parser.add_argument("--sniper-mimicos-memory-mb", default="4096",
                        help="Override perf_model/reserve_thp/memory_size for GraphBrew Sniper runs. The upstream baseline default is 131072 MB, which is excessive for these workloads.")
    parser.add_argument("--sniper-mimicos-kernel-mb", default="128",
                        help="Override perf_model/reserve_thp/kernel_size for GraphBrew Sniper runs. The upstream baseline default is 32768 MB, which is excessive for these workloads.")
    parser.add_argument("--sniper-enable-graph-policies", action="store_true",
                        help="Enable tracked Sniper graph-policy overlays even if .sniper_overlays.json is absent.")
    parser.add_argument("--sniper-cores", default="1", help="Core count passed to run-sniper -n and OMP_NUM_THREADS.")
    parser.add_argument("--threads", nargs="+", default=[],
                        help="Sniper thread/core counts to sweep. Alias for repeated --sniper-cores values.")
    parser.add_argument("--sniper-base-config", default="graphbrew/graph_sniper",
                        help="Base Sniper -c config for GraphBrew runs. Installed by scripts/setup_sniper.py from bench/include/sniper_sim/configs/.")
    parser.add_argument("--sniper-root", default=str(DEFAULT_SNIPER_ROOT),
                        help="Sniper checkout/install root containing run-sniper. Relative paths are resolved from the GraphBrew repository root.")
    parser.add_argument("--sniper-frontend", choices=["live", "sift"], default="live",
                        help="Sniper frontend mode. 'live' is the proven default; 'sift' inserts --sift for bounded trace-frontend probes.")
    parser.add_argument("--sniper-roi-icount", default="0",
                        help="Cap the Sniper DETAILED ROI at this many instructions (aggregated over cores) via '-s stop-by-icount:N'. "
                             "0 disables the cap (full ROI). Bounds simulation time on large graphs regardless of size, matching the "
                             "the reference 600000000-instruction cap and P-OPT iteration sampling; cache_sim runs the full ROI.")
    parser.add_argument(
        "--sniper-semantic-edge-limit", default="0",
        help="Policy-independent cap on static graph edge visits in sg_kernel "
             "(0 = full semantic ROI). Mutually exclusive with "
             "--sniper-roi-icount.")
    parser.add_argument("--sniper-omp-wait-policy", choices=["passive", "active", "unset"], default="passive",
                        help="OMP_WAIT_POLICY for Sniper benchmark processes. Passive avoids SIFT/OpenMP barrier deadlocks observed with full wrappers.")
    parser.add_argument("--sniper-config", nargs="*", default=[], help="Additional Sniper -c config names after --sniper-base-config.")
    parser.add_argument(
        "--sniper-queue-model",
        choices=["history_list", "windowed_mg1"],
        default="history_list",
        help="Queue model used by Sniper NUCA, DRAM, DRAM-cache, and network "
             "queues. Final scale runs use windowed_mg1 because Sniper "
             "documents history_list as unsafe with interval/OoO timing.")
    parser.add_argument("--sniper-address-domain", choices=["virtual", "translated"], default="virtual",
                        help="Address domain for Sniper cache-side GraphBrew sidebands. 'virtual' disables Sniper translation so exported virtual regions match cache callbacks; 'translated' keeps the baseline MMU path and requires translated/physical sidebands.")
    parser.add_argument(
        "--require-sniper-aslr-disable", action="store_true",
        help="Fail controlled Sniper cells unless setarch -R is available and used.")
    parser.add_argument(
        "--sniper-require-fused-receipts", action="store_true",
        help="Require live fused-ReusePlan receipts and disable cache warming for this mechanism-proof cell.")
    parser.add_argument(
        "--require-cache-sim-aslr-disable", action="store_true",
        help="Fail controlled cache_sim cells unless setarch -R is available and used.")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    semantic_edge_limit = int(args.sniper_semantic_edge_limit)
    if int(args.sniper_roi_icount) > 0 and semantic_edge_limit > 0:
        raise SystemExit(
            "--sniper-roi-icount and --sniper-semantic-edge-limit "
            "are mutually exclusive")
    if semantic_edge_limit > 0 and args.sniper_workload != "sg_kernel":
        raise SystemExit(
            "--sniper-semantic-edge-limit requires "
            "--sniper-workload sg_kernel")
    if semantic_edge_limit > 0 and args.ecg_isa_variant != "computed":
        raise SystemExit(
            "--sniper-semantic-edge-limit requires "
            "--ecg-isa-variant computed for transport-matched execution")
    if semantic_edge_limit > 0 and int(args.sniper_cores) != 1:
        raise SystemExit(
            "--sniper-semantic-edge-limit requires --sniper-cores 1 "
            "for deterministic edge order")
    if (semantic_edge_limit > 0 and args.threads and
            any(int(value) != 1 for value in args.threads)):
        raise SystemExit(
            "--sniper-semantic-edge-limit requires every --threads value "
            "to equal 1")
    if args.threads and args.suite != "sniper":
        raise SystemExit("--threads is currently supported only with --suite sniper")
    compact_reuse_bind_requested = bool(
        args.gem5_compact_reuse_bind_flowthrough or
        args.gem5_compact_reuse_bind_performance)
    if (
            args.gem5_compact_reuse_bind_flowthrough and
            args.gem5_compact_reuse_bind_performance):
        raise SystemExit(
            "choose either --gem5-compact-reuse-bind-flowthrough for traced "
            "correctness or --gem5-compact-reuse-bind-performance for timing")
    if args.gem5_compact_fused and args.suite not in ("gem5", "both"):
        raise SystemExit(
            "--gem5-compact-fused requires --suite gem5 or both")
    if compact_reuse_bind_requested and args.suite not in ("gem5", "both"):
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires --suite gem5 or both")
    if args.suite in ("gem5", "both"):
        gem5_isa = selected_gem5_isa()
    else:
        gem5_isa = ""
    if args.gem5_compact_fused and args.benchmark != "pr":
        raise SystemExit(
            "--gem5-compact-fused is implemented only for --benchmark pr")
    if compact_reuse_bind_requested and args.benchmark != "pr":
        raise SystemExit(
            "compact ReuseBind+FlowThrough is implemented only for "
            "--benchmark pr")
    if compact_reuse_bind_requested and args.ecg_isa_variant != "computed":
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires "
            "--ecg-isa-variant computed")
    if compact_reuse_bind_requested and args.gem5_cpu_type != "O3":
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires --gem5-cpu-type O3 "
            "for exact Request-bound delivery")
    if (
            compact_reuse_bind_requested and
            parse_size_bytes(str(args.line_size)) != 64):
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires --line-size 64 "
            "because the gem5 ECG request/fill guard is cache-line based")
    if args.gem5_compact_fused and gem5_isa != "riscv":
        raise SystemExit("--gem5-compact-fused requires RISC-V gem5")
    if compact_reuse_bind_requested and gem5_isa != "riscv":
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires RISC-V gem5")
    if (getattr(args, "flowthrough", "off") != "off" and
            args.suite not in ("cache-sim", "both")):
        raise SystemExit(
            "--flowthrough is implemented in cache_sim only; other "
            "backends would record an equalisation that never happened")
    if (getattr(args, "popt_matrix_stream", "analytic") == "simulated" and
            args.suite not in ("cache-sim", "both")):
        raise SystemExit(
            "--popt-matrix-stream simulated is implemented in cache_sim only; "
            "gem5 and Sniper would silently fall back to the analytic charge")
    if (
            getattr(args, "popt_matrix_stream", "analytic") ==
            "analytic_prefetch_upper_bound" and
            args.suite != "gem5"):
        raise SystemExit(
            "--popt-matrix-stream analytic_prefetch_upper_bound is a "
            "gem5-only sensitivity; cache_sim must use simulated streaming")
    # A flat analytic matrix charge cannot be covered by a prefetcher, while
    # ReusePlan's per-edge records are simulated accesses that can. Combining the
    # analytic charge with an active prefetcher therefore prices the two
    # metadata streams differently and produces an invalid comparison; the
    # The reporting rules in wiki/Evaluation-Methodology.md forbid it.
    prefetch_active = (
        args.prefetcher != "none" or
        int(getattr(args, "cache_stream_prefetch_degree", 0) or 0) > 0)
    if (prefetch_active and
            getattr(args, "popt_matrix_stream", "analytic") == "analytic" and
            any(spec.charge_popt_overhead for spec in
                [parse_policy_spec(p) for p in (
                    args.policies or
                    (ALL_POLICIES if args.all_policies else
                     SNIPER_DEFAULT_POLICIES if args.suite == "sniper"
                     else DEFAULT_POLICIES))])):
        raise SystemExit(
            "charged P-OPT with an active prefetcher requires "
            "--popt-matrix-stream simulated, or the explicit "
            "analytic_prefetch_upper_bound sensitivity: a flat analytic "
            "matrix charge cannot be prefetch-covered while ReusePlan's records can")
    if args.all_policies:
        policy_texts = ALL_POLICIES
    elif args.policies is not None:
        policy_texts = args.policies
    elif args.suite == "sniper":
        policy_texts = SNIPER_DEFAULT_POLICIES
    else:
        policy_texts = DEFAULT_POLICIES
    policies = [parse_policy_spec(p) for p in policy_texts]
    if (
            compact_reuse_bind_requested and
            not any(
                spec.policy == "ECG" and spec.ecg_flowthrough and
                spec.ecg_reuse_plan_depth == 2
                for spec in policies)):
        raise SystemExit(
            "compact ReuseBind+FlowThrough requires at least one "
            "two-epoch ReusePlan ECG FlowThrough policy")
    args.has_lru_baseline = any(spec.label == "LRU" for spec in policies)
    out_dir = Path(args.out_dir) if args.out_dir else RESULTS_ROOT / now_tag()
    if not out_dir.is_absolute():
        out_dir = PROJECT_ROOT / out_dir

    print(f"[roi-matrix] output: {out_dir}")
    print(f"[roi-matrix] suite={args.suite} benchmark={args.benchmark} options={args.options!r}")
    print(f"[roi-matrix] policies={', '.join(p.label for p in policies)}")
    print(f"[roi-matrix] l3_sizes={', '.join(args.l3_sizes)}")

    completion_marker = out_dir / "roi_matrix.complete.json"
    if not args.dry_run:
        completion_marker.unlink(missing_ok=True)
    if (
            args.expected_gem5_guest_sha256 ==
            PLANNING_MISSING_GEM5_GUEST_SHA256 and
            not args.dry_run):
        raise SystemExit(
            "planning-only missing gem5 guest hash cannot execute")
    build_targets(args)
    validate_selected_gem5_guest(args, out_dir)
    validate_expected_gem5_inputs(args)

    rows: list[dict[str, Any]] = []
    for l3_size in args.l3_sizes:
        for spec in policies:
            if args.suite in ("cache-sim", "both"):
                print(f"[cache_sim] {spec.label} L3={l3_size}")
                rows.extend(run_cache_sim(args, out_dir, spec, l3_size))
            if args.suite in ("gem5", "both"):
                print(f"[gem5] {spec.label} L3={l3_size}")
                rows.extend(run_gem5(args, out_dir, spec, l3_size))
            if args.suite == "sniper":
                original_cores = str(args.sniper_cores)
                thread_values = [str(value) for value in (args.threads or [args.sniper_cores])]
                args._sniper_thread_sweep = bool(args.threads)
                for thread_count in thread_values:
                    args.sniper_cores = thread_count
                    print(f"[sniper] {spec.label} L3={l3_size} T={thread_count}")
                    rows.extend(run_sniper(args, out_dir, spec, l3_size))
                args.sniper_cores = original_cores
                args._sniper_thread_sweep = False
            if not args.dry_run:
                write_outputs(out_dir, rows)

    certify_sniper_semantic_work(rows, args, policies)
    certify_gem5_pr_results(rows, args)
    if not args.dry_run:
        # Persist layered certification failures before any fail-closed
        # run-level validator raises. Do not emit a completion marker here.
        write_outputs(out_dir, rows)
    validate_gem5_compact_reuse_bind_flowthrough_rows(rows, args, policies)

    inert_cells = set()
    for row in rows:
        annotate_l3_pressure(row)
        if row.get("l3_exercised") is False:
            inert_cells.add((row.get("benchmark"), str(row.get("l3_size"))))
    for benchmark, l3_size in sorted(c for c in inert_cells if all(c)):
        print(
            f"[warn] L3 inert for {benchmark} @ L3={l3_size}: property working set "
            f"fits in L2, so the L3 policy is not exercised (every access cold-misses). "
            f"Use a larger graph (property bytes > LLC) or smaller caches for an L3 comparison."
        )

    if not args.dry_run:
        write_outputs(out_dir, rows)
        write_completion_marker(out_dir, args, policies, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))