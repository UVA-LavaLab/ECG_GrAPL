#!/usr/bin/env python3
"""
gem5 cache hierarchy configuration matching GraphBrew/ECG defaults.

Provides factory functions to create cache hierarchy objects with parameters
matching the standalone cache_sim defaults:

    L1D: 32KB, 8-way, 64B lines
    L1I: 32KB, 8-way, 64B lines
    L2:  256KB, 4-way, 64B lines  (private per core)
    L3:  8MB, 16-way, 64B lines   (shared)

These match:
    - bench/include/cache_sim/cache_sim.h CacheHierarchy defaults
    - scripts/experiments/ecg_config.py DEFAULT_CACHE
    - Makefile CACHE_L1_SIZE/CACHE_L2_SIZE/CACHE_L3_SIZE

Reference: scripts/experiments/ecg_config.py lines 72-78
"""

import m5
import os
from m5.objects import *
from m5.util import convert


# =============================================================================
# Default cache parameters (matching ECG standalone simulator)
# =============================================================================
DEFAULTS = {
    "l1d_size":  "32kB",
    "l1d_assoc": 8,
    "l1i_size":  "32kB",
    "l1i_assoc": 8,
    "l2_size":   "256kB",
    "l2_assoc":  8,
    "l3_size":   "8MB",
    "l3_assoc":  16,
    "line_size":  64,
}


# =============================================================================
# Replacement Policy Factory
# =============================================================================
POLICY_MAP = {
    "LRU":   lambda: LRURP(),
    "FIFO":  lambda: FIFORP(),
    # gem5 BRRIP uses btp as the probability of LONG (max-1) insertion.
    # btp=0 therefore means always DISTANT/max insertion (BRRIP), not SRRIP.
    # RRIPRP pins btp=100; use 3 bits to match GRASP/POPT/ECG and Sniper.
    "SRRIP": lambda: RRIPRP(num_bits=3),
    "BRRIP": lambda: BRRIPRP(),
    "RANDOM": lambda: RandomRP(),
}


def size_to_bytes(size):
    """Return a cache-size value as bytes for replacement-policy parameters."""
    if isinstance(size, str):
        return int(convert.toMemorySize(size))
    return int(size)


def make_replacement_policy(name, **kwargs):
    """Create a replacement policy SimObject by name.

    For graph-aware policies (GRASP, POPT, ECG), imports from the
    GraphBrew overlay SimObjects.

    Args:
        name: Policy name (LRU, FIFO, SRRIP, GRASP, POPT, ECG, etc.)
        **kwargs: Extra parameters passed to the policy constructor.

    Returns:
        gem5 SimObject for the replacement policy.
    """
    upper = name.upper()
    sideband_path = os.environ.get("GEM5_GRAPHBREW_CTX", "/tmp/gem5_graphbrew_ctx.json")
    popt_matrix_path = os.environ.get("GEM5_POPT_MATRIX", "/tmp/gem5_popt_matrix.bin")

    if upper in POLICY_MAP:
        return POLICY_MAP[upper]()

    if upper == "HAWKEYE":
        return GraphHawkeyeRP(
            num_sets=kwargs.get("num_sets", 8192),
            num_ways=kwargs.get("num_ways", 16),
            line_size=kwargs.get("line_size", 64),
        )
    elif upper == "GRASP":
        return GraphGraspRP(
            max_rrpv=kwargs.get("max_rrpv", 7),
            num_buckets=kwargs.get("num_buckets", 11),
            hot_fraction=kwargs.get("hot_fraction", 0.15),
            llc_size_bytes=kwargs.get("llc_size_bytes", 8388608),
            sideband_path=sideband_path,
        )
    elif upper in ("POPT", "P-OPT"):
        return GraphPoptRP(
            max_rrpv=kwargs.get("max_rrpv", 7),
            sideband_path=sideband_path,
            popt_matrix_path=popt_matrix_path,
        )
    elif upper == "ECG":
        if kwargs.get("ecg_mode") == "ECG_REF32":
            if not kwargs.get("native_ref32", False):
                raise ValueError("ECG_REF32 requires a native commit transport")
            return GraphRef32RP(
                llc_size_bytes=kwargs.get("llc_size_bytes", 8388608),
                sideband_path=sideband_path,
                required_context=1,
            )
        return GraphEcgRP(
            rrpv_max=kwargs.get("rrpv_max", 7),
            num_buckets=kwargs.get("num_buckets", 11),
            ecg_mode=kwargs.get("ecg_mode", "DBG_PRIMARY"),
            llc_size_bytes=kwargs.get("llc_size_bytes", 8388608),
            sideband_path=sideband_path,
            popt_matrix_path=popt_matrix_path,
        )
    else:
        print(f"Warning: Unknown policy '{name}', defaulting to LRU")
        return LRURP()


# =============================================================================
# Cache Hierarchy Builder
# =============================================================================
def make_l1d_cache(policy="LRU", size=DEFAULTS["l1d_size"],
                   assoc=DEFAULTS["l1d_assoc"], **policy_kwargs):
    """Create L1 data cache matching ECG defaults."""
    if str(policy).upper() == "HAWKEYE":
        raise ValueError("Hawkeye is an LLC-only replacement policy")
    return Cache(
        size=size,
        assoc=assoc,
        tag_latency=2,
        data_latency=2,
        response_latency=2,
        mshrs=4,
        tgts_per_mshr=20,
        replacement_policy=make_replacement_policy(policy, **policy_kwargs),
    )


def make_l1i_cache(policy="LRU", size=DEFAULTS["l1i_size"],
                   assoc=DEFAULTS["l1i_assoc"], **policy_kwargs):
    """Create L1 instruction cache."""
    if str(policy).upper() == "HAWKEYE":
        raise ValueError("Hawkeye is an LLC-only replacement policy")
    return Cache(
        size=size,
        assoc=assoc,
        tag_latency=2,
        data_latency=2,
        response_latency=2,
        mshrs=4,
        tgts_per_mshr=20,
        replacement_policy=make_replacement_policy(policy, **policy_kwargs),
    )


def make_l2_cache(policy="LRU", size=DEFAULTS["l2_size"],
                  assoc=DEFAULTS["l2_assoc"], **policy_kwargs):
    """Create L2 private cache matching ECG defaults."""
    if str(policy).upper() == "HAWKEYE":
        raise ValueError("Hawkeye is an LLC-only replacement policy")
    return Cache(
        size=size,
        assoc=assoc,
        tag_latency=10,
        data_latency=10,
        response_latency=10,
        mshrs=20,
        tgts_per_mshr=12,
        replacement_policy=make_replacement_policy(policy, **policy_kwargs),
    )


def make_l3_cache(policy="LRU", size=DEFAULTS["l3_size"],
                  assoc=DEFAULTS["l3_assoc"], **policy_kwargs):
    """Create L3 shared cache matching ECG defaults (8MB, 16-way)."""
    policy_kwargs.setdefault("llc_size_bytes", size_to_bytes(size))
    policy_kwargs.setdefault("num_ways", int(assoc))
    policy_kwargs.setdefault(
        "num_sets", max(size_to_bytes(size) // (int(assoc) * 64), 1))
    policy_kwargs.setdefault("line_size", 64)
    # Diagnostic: gem5 L3 defaults to mostly_incl (inclusive -> back-invalidates
    # L1/L2 on L3 eviction). cache_sim has no back-invalidation, so this is a
    # candidate source of gem5-vs-cache_sim divergence for ECG. Allow override.
    l3_clusivity = os.environ.get("GEM5_L3_CLUSIVITY", "mostly_incl")
    return Cache(
        size=size,
        assoc=assoc,
        tag_latency=20,
        data_latency=20,
        response_latency=20,
        mshrs=32,
        tgts_per_mshr=16,
        clusivity=l3_clusivity,
        replacement_policy=make_replacement_policy(policy, **policy_kwargs),
    )


def make_droplet_prefetcher(**kwargs):
    """Create DROPLET indirect graph prefetcher."""
    return GraphDropletPrefetcher(
        prefetch_degree=kwargs.get("prefetch_degree", 1),
        indirect_degree=kwargs.get("indirect_degree", 16),
        stride_table_size=kwargs.get("stride_table_size", 64),
        use_virtual_addresses=True,
        prefetch_on_access=True,
        on_inst=False,
    )


def make_ecg_pfx_prefetcher(**kwargs):
    """Create ECG_PFX hint-driven graph prefetcher."""
    return GraphEcgPfxPrefetcher(
        recent_filter_size=kwargs.get("recent_filter_size", 256),
        use_virtual_addresses=True,
        prefetch_on_access=True,
        on_inst=False,
    )


def make_stride_prefetcher(**kwargs):
    """Create a generic stride/stream prefetcher for the sequential
    structure (edge-list) stream. Attached UNIFORMLY to every policy to
    LEVEL the structure-prefetch axis across the three simulators: it is
    gem5's faithful analogue of cache_sim's CACHE_STREAM_PREFETCH_DEGREE
    and Sniper's stride prefetcher. The structure stream is sequential, so
    a real HW stride prefetcher captures it while the irregular property
    stream is left to the replacement policy. This is NOT a graph-aware
    prefetcher (unlike DROPLET / ECG_PFX); it carries no graph semantics."""
    return StridePrefetcher(
        degree=kwargs.get("degree", 4),
        use_virtual_addresses=True,
        prefetch_on_access=True,
        on_inst=False,
    )
