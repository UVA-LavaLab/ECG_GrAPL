"""Run the shared REF32 codec/cache regressions from freshly compiled code."""

import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_ref32_cpp_suite(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    binary = tmp_path / "test_ecg_ref32"
    built = subprocess.run(
        [compiler, "-std=c++17", "-O2", "-fopenmp",
         f"-I{ROOT / 'bench/include'}",
         str(ROOT / "bench/src_sim/test_ecg_ref32.cc"), "-o", str(binary)],
        capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stderr
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("CACHE_", "ECG_", "POPT_", "TOPT_"))
        and key != "T_OPT"
    }
    env["OMP_NUM_THREADS"] = "1"
    ran = subprocess.run(
        [str(binary)], env=env, capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "REF32 TESTS: PASS" in ran.stdout
