"""The adaptive record contract preserves legacy bytes and fails closed."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_ecg_record_layout_and_codec(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    binary = tmp_path / "test_ecg_record"
    built = subprocess.run([
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "bench/include"),
        str(ROOT / "bench/src_sim/test_ecg_record.cc"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert built.returncode == 0, built.stdout + built.stderr
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=15, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "[SUMMARY] failures=0" in ran.stdout
