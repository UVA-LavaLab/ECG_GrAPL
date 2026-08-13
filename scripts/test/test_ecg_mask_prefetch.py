#!/usr/bin/env python3
"""Regression tests for ECG mask prefetch bit mapping."""

import shutil
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_ecg_mask_prefetch_table_decode_and_dedup_clamp(tmp_path):
    """Hot-table prefetch targets are 1-based and dedup capacity is bounded."""
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available")

    source = tmp_path / "ecg_mask_prefetch_check.cc"
    binary = tmp_path / "ecg_mask_prefetch_check"

    source.write_text(
        r'''
#include <climits>
#include <cstdlib>
#include <vector>

#include "bench/include/cache_sim/graph_cache_context.h"

int main() {
    using cache_sim::GraphCacheContext;
    using cache_sim::MaskConfig;

    setenv("ECG_DBG_BITS", "2", 1);
    setenv("ECG_POPT_BITS", "2", 1);
    setenv("ECG_PFX_BITS", "2", 1);
    MaskConfig cfg;
    cfg.autoAllocate(1024);  // table mode: 2 PFX bits cannot encode vertex IDs directly
    unsetenv("ECG_DBG_BITS");
    unsetenv("ECG_POPT_BITS");
    unsetenv("ECG_PFX_BITS");

    if (cfg.prefetch_direct) return 1;
    if (cfg.hot_table_size != 3) return 2;

    GraphCacheContext ctx;
    ctx.mask_config = cfg;
    ctx.hot_table = {11, 22, 33};

    if (ctx.resolvePrefetchTarget(0) != UINT32_MAX) return 3;
    if (ctx.resolvePrefetchTarget(ctx.mask_config.encode(1, 0, 1)) != 11) return 4;
    if (ctx.resolvePrefetchTarget(ctx.mask_config.encode(1, 0, 2)) != 22) return 5;
    if (ctx.resolvePrefetchTarget(ctx.mask_config.encode(1, 0, 3)) != 33) return 6;

    GraphCacheContext::PrefetchDedupWindow window;
    window.capacity = 16;
    for (uint32_t i = 1; i <= 16; ++i) window.push(i);
    if (!window.contains(1) || !window.contains(16)) return 7;
    window.push(17);
    if (window.contains(1) || !window.contains(17)) return 8;

    setenv("ECG_PREFETCH_WINDOW", "64", 1);
    MaskConfig env_cfg;
    env_cfg.initFromEnv();
    if (env_cfg.prefetch_window != 16) return 9;
    unsetenv("ECG_PREFETCH_WINDOW");

    setenv("ECG_PREFETCH_WINDOW", "12", 1);
    GraphCacheContext runtime_ctx;
    std::vector<uint32_t> degrees = {1, 2, 3, 4};
    runtime_ctx.initTopology(degrees.data(), degrees.size(), 0, false);
    runtime_ctx.initMaskConfig();
    if (runtime_ctx.dedup_for_thread().capacity != 12) return 10;
    unsetenv("ECG_PREFETCH_WINDOW");

    return 0;
}
'''
    )

    compile_cmd = [
        compiler,
        "-std=c++17",
        f"-I{PROJECT_ROOT}",
        f"-I{PROJECT_ROOT / 'bench/include/external/gapbs'}",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        str(source),
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, cwd=str(PROJECT_ROOT), check=True)
    subprocess.run([str(binary)], cwd=str(PROJECT_ROOT), check=True)
