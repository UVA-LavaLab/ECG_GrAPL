#!/usr/bin/env python3
"""Regression tests for cache_sim demand/prefetch accounting."""

import shutil
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_accurate_cache_prefetch_fills_are_separate_from_demand_misses(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available")

    source = tmp_path / "cache_prefetch_accounting_check.cc"
    binary = tmp_path / "cache_prefetch_accounting_check"

    source.write_text(
        r'''
#include <cstdint>
#include <vector>

#include "bench/include/cache_sim/cache_sim.h"

int main() {
    using cache_sim::CacheHierarchy;
    using cache_sim::EvictionPolicy;

    CacheHierarchy cache(1024, 1, 1024, 1, 1024, 1, 64,
                         EvictionPolicy::LRU, EvictionPolicy::LRU, EvictionPolicy::LRU);
    std::vector<uint64_t> values(64, 0);

    cache.read(&values[0]);
    if (cache.getTotalAccesses() != 1) return 1;
    if (cache.getMemoryAccesses() != 1) return 2;

    cache.prefetch(reinterpret_cast<uint64_t>(&values[16]));
    if (cache.getTotalAccesses() != 1) return 3;
    if (cache.getMemoryAccesses() != 1) return 4;
    if (cache.getPrefetchRequests() != 1) return 5;
    if (cache.getPrefetchFills() != 1) return 6;
    if (cache.getTotalMemoryTraffic() != 2) return 7;
    if (cache.getPrefetchPending() != 1) return 8;

    cache.read(&values[16]);
    if (cache.getTotalAccesses() != 2) return 9;
    if (cache.getMemoryAccesses() != 1) return 10;
    if (cache.getPrefetchUseful() != 1) return 11;
    if (cache.getPrefetchPending() != 0) return 12;

    cache.prefetch(reinterpret_cast<uint64_t>(&values[16]));
    if (cache.getPrefetchRequests() != 2) return 13;
    if (cache.getPrefetchCacheHits() != 1) return 14;
    if (cache.getPrefetchFills() != 1) return 15;

    return 0;
}
'''
    )

    compile_cmd = [
        compiler,
        "-std=c++17",
        "-fopenmp",
        f"-I{PROJECT_ROOT}",
        f"-I{PROJECT_ROOT / 'bench/include/external/gapbs'}",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        str(source),
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, cwd=str(PROJECT_ROOT), check=True)
    subprocess.run([str(binary)], cwd=str(PROJECT_ROOT), check=True)