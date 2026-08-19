"""Executable cross-backend PageRank P-OPT ``findNextRef`` parity tests.

Unlike ``ecg_victim_policy.h`` (enforced byte-identical across cache_sim/
gem5/Sniper by ``test_shared_ecg_policy.py``), the P-OPT next-reference
lookup used by PageRank's rereference matrix has three independently
maintained copies:

* ``cache_sim::RereferenceConfig::findNextRef``
  (bench/include/cache_sim/graph_cache_context.h)
* ``gem5::replacement_policy::graph::RereferenceMatrix::findNextRef``
  (bench/include/gem5_sim/overlays/mem/cache/replacement_policies/
  graph_cache_context_gem5.hh)
* ``graphbrew::sniper::RereferenceMatrix::findNextRef``
  (bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/
  graph_cache_context_sniper.{h,cc})

A silent behavioral drift between these three copies would go undetected by
every other test in this repo. This module compiles and runs
``bench/src_sim/test_popt_findnextref_cross_backend_parity.cc``, which links
real object code from each backend's own header/source (via three tiny
extern "C" adapters, one per backend, each built under that backend's own
include path/namespace) and exercises all three with the SAME encoded
PageRank-representative rereference-matrix bytes and the SAME
(cache-line-id, current-vertex) query for every case, additionally checking
each result against an independently hand-derived expected value (from the
documented P-OPT Algorithm 2 MSB-encoding rules) so a bug shared by all
three copies -- not just a divergence between them -- is also caught. See
that .cc file's header comment for the full case list and rationale.

This test compiles the program fresh (it does not depend on, nor read from,
any pre-built binary), does not launch gem5 or Sniper, and requires no
graph/benchmark data -- only a working host C++ compiler. If compilation
ever fails, that failure is reported explicitly (not silently skipped),
since a real compiler is a baseline requirement of this repository already
exercised by many other tests in this same file/directory.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH_INCLUDE = ROOT / "bench/include"
CACHE_SIM_INCLUDE = BENCH_INCLUDE / "cache_sim"
GEM5_OVERLAY_INCLUDE = BENCH_INCLUDE / "gem5_sim/overlays"
SNIPER_OVERLAY_CACHE_DIR = (
    BENCH_INCLUDE /
    "sniper_sim/overlays/common/core/memory_subsystem/cache")
BIN_DIR = ROOT / "bench/bin_sim"

SOURCES = [
    ROOT / "bench/src_sim/popt_findnextref_adapter_cache_sim.cc",
    ROOT / "bench/src_sim/popt_findnextref_adapter_gem5.cc",
    ROOT / "bench/src_sim/popt_findnextref_adapter_sniper.cc",
    SNIPER_OVERLAY_CACHE_DIR / "graph_cache_context_sniper.cc",
    ROOT / "bench/src_sim/test_popt_findnextref_cross_backend_parity.cc",
]

EXPECTED_PASS_LINE = "== 12 passed, 0 failed =="


def test_popt_findnextref_cross_backend_parity_pins_pagerank_rereference_lookup():
    for src in SOURCES:
        assert src.is_file(), f"expected source file missing: {src}"

    binary = BIN_DIR / "test_popt_findnextref_cross_backend_parity"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    compile_cmd = [
        "g++", "-std=c++17", "-O2",
        "-I", str(BENCH_INCLUDE),
        "-I", str(CACHE_SIM_INCLUDE),
        "-I", str(GEM5_OVERLAY_INCLUDE),
        "-I", str(SNIPER_OVERLAY_CACHE_DIR),
        *[str(s) for s in SOURCES],
        "-o", str(binary),
    ]
    compiled = subprocess.run(
        compile_cmd, capture_output=True, text=True, timeout=120,
        check=False)
    assert compiled.returncode == 0, (
        "failed to compile the cross-backend P-OPT findNextRef parity "
        f"program:\ncmd: {' '.join(compile_cmd)}\n"
        f"stdout:\n{compiled.stdout}\nstderr:\n{compiled.stderr}")

    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60,
        check=False)
    assert result.returncode == 0, (
        f"cross-backend P-OPT findNextRef parity FAILED:\n{result.stdout}\n"
        f"{result.stderr}")
    assert EXPECTED_PASS_LINE in result.stdout, result.stdout
    assert "[FAIL]" not in result.stdout, result.stdout

    cache_context = (
        ROOT / "bench/include/cache_sim/graph_cache_context.h").read_text()
    cache = (ROOT / "bench/include/cache_sim/cache_sim.h").read_text()
    gem5_context = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/graph_cache_context_gem5.hh").read_text()
    assert "static thread_local PositionCache cache;" in cache_context
    assert "static thread_local PositionCache cache;" in cache
    assert "static thread_local PositionCache cache;" in gem5_context
    assert "candidate_distances[c] = dist;" in cache
    popt_tie = cache.split(
        "mode == ECGMode::POPT_TIE", 1)[1].split(
            "mode == ECGMode::ECG_EMBEDDED", 1)[0]
    assert popt_tie.count("graph_ctx_->findNextRef(") == 1
    assert "P-OPT and ECG support at most 64 cache ways" in cache
