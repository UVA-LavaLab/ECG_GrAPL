#!/usr/bin/env python3
"""Regression test for the standalone ECG preprocessing benchmark."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_ecg_preprocess_benchmark_outputs_overhead_json(tmp_path):
    """The no-simulation utility reports preprocessing substep timings."""
    if shutil.which("g++") is None:
        pytest.skip("g++ not available")

    subprocess.run(
        ["make", "RABBIT_ENABLE=0", "bench/bin_sim/ecg_preprocess"],
        cwd=PROJECT_ROOT,
        check=True,
    )

    output = tmp_path / "ecg_preprocess.json"
    env = os.environ.copy()
    env.update(
        {
            "ECG_PREFETCH_MODE": "2",
            "ECG_PREPROCESS_REPEATS": "1",
            "ECG_PREPROCESS_OUTPUT_JSON": str(output),
            "OMP_NUM_THREADS": "2",
        }
    )
    subprocess.run(
        [str(PROJECT_ROOT / "bench/bin_sim/ecg_preprocess"), "-g", "6", "-k", "4", "-o", "0", "-n", "1"],
        cwd=PROJECT_ROOT,
        env=env,
        check=True,
    )

    data = json.loads(output.read_text())
    assert data["nodes"] > 0
    assert data["edges_directed"] > 0
    assert data["popt_matrix_enabled"] == 1
    assert data["ecg_prefetch_mode"] == 2
    assert data["degree_scan_s_mean"] >= 0
    assert data["popt_matrix_s_mean"] >= 0
    assert data["mask_build_s_mean"] >= 0
    assert data["total_preprocess_s_mean"] >= data["mask_build_s_mean"]
    assert data["mask_vertices"] == data["nodes"]
    assert data["pfx_candidates"] >= data["pfx_encoded"]