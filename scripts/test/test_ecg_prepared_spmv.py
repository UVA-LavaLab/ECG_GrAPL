"""Bounded ownership/lifetime proof, not a new policy or performance runner."""

import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_prepared_spmv_ownership_and_query_lifetime(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import serialized_graph
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    graph = tmp_path / "prepared64.sg"
    graph.write_bytes(serialized_graph(
        64, [(0, 16), (16, 0), (0, 32), (32, 0), (16, 32), (32, 16)], False)[0])
    binary = tmp_path / "prepared-spmv"
    build = subprocess.run([
        compiler, "-std=c++17", "-O1", "-g", "-ffp-contract=off", "-fopenmp",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable",
        "-Wno-sign-compare", "-I", str(ROOT / "bench/include"),
        "-I", str(ROOT / "bench/include/external/gapbs"),
        str(ROOT / "bench/src_sim/test_ecg_prepared_spmv.cc"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=120, check=False)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run(
        [str(binary), str(graph)], capture_output=True, text=True, timeout=30, check=False,
        env={**os.environ, "OMP_NUM_THREADS": "1", "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
             "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
    assert run.returncode == 0, run.stdout + run.stderr
    receipt = json.loads(run.stdout.splitlines()[-1])
    assert receipt["schema"] == "ecg.prepared-spmv-qualification.v1"
    assert receipt["scope"] == "fixture-only-ownership-not-cache-performance"
    assert receipt["pass_rank_producer"] is receipt["pass_rank_policy"] is False
    assert receipt["storage_reused"] and receipt["stale_query_rejected"] and receipt["opaque_tags"]
    assert receipt["fresh_x_writes_per_query"] == 64
    cells = {cell["kind"]: cell for cell in receipt["cells"]}
    assert set(cells) == {"raw-owner", "matrix-owner", "opaque-tag-owner"}
    assert all(c["queries"] == 2 and c["edge_bytes"] == 24 and c["extra_edge_buffers"] == 0
               for c in cells.values())
    assert all(c["query_read_bytes"] == 3208 and c["query_write_bytes"] == 768 for c in cells.values())
    assert cells["matrix-owner"]["matrix_binding_probes_per_query"] == 1
    assert cells["raw-owner"]["preparation_read_bytes"] == cells["raw-owner"]["preparation_write_bytes"] == 0
    assert cells["matrix-owner"]["matrix_builds"] == 1 and cells["matrix-owner"]["matrix_bytes"] == 1024
    assert cells["matrix-owner"]["preparation_read_bytes"] > 0
    assert cells["matrix-owner"]["preparation_write_bytes"] >= 1024
    assert cells["opaque-tag-owner"]["conversions"] == 1
    assert cells["opaque-tag-owner"]["preparation_read_bytes"] == 48
    assert cells["opaque-tag-owner"]["preparation_write_bytes"] == 24
    (tmp_path / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
