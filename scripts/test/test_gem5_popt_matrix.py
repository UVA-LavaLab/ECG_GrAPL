#!/usr/bin/env python3
"""Regression tests for gem5 P-OPT rereference matrix export metadata."""

import shutil
import struct
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def test_gem5_popt_matrix_export_uses_ceil_sub_epoch(tmp_path):
    """gem5 export header must match makeOffsetMatrix/cache_sim ceil sizing."""
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available")

    source = tmp_path / "popt_export_check.cc"
    binary = tmp_path / "popt_export_check"
    matrix_path = tmp_path / "popt_matrix.bin"

    source.write_text(
        r'''
#include <cstdint>
#include <vector>

#include "bench/include/gem5_sim/gem5_harness.h"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    GEM5_ECG_PFX_TARGET(7);
    constexpr uint32_t num_epochs = 256;
    constexpr uint32_t num_cache_lines = 3;
    constexpr uint32_t num_vertices = 33025;  // ceil(33025/256)=130, ceil(130/128)=2
    std::vector<uint8_t> matrix(num_epochs * num_cache_lines, 0);
    return gem5_export_popt_matrix(matrix.data(), num_cache_lines,
                                   num_epochs, num_vertices, 64,
                                   argv[1]) ? 0 : 1;
}
'''
    )

    compile_cmd = [
        compiler,
        "-std=c++17",
        "-DNO_M5OPS",
        f"-I{PROJECT_ROOT}",
        f"-I{PROJECT_ROOT / 'bench/include/external/gapbs'}",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        str(source),
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, cwd=str(PROJECT_ROOT), check=True)
    subprocess.run([str(binary), str(matrix_path)], cwd=str(PROJECT_ROOT), check=True)

    data = matrix_path.read_bytes()
    assert len(data) == 16 + 256 * 3
    assert struct.unpack("<IIII", data[:16]) == (256, 3, 130, 2)