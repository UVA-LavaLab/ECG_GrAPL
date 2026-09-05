"""Adaptive record operations use a distinct, explicitly bound v2 ISA space."""

from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_v2_instruction_encodings_and_three_source_operand(tmp_path):
    assembler = shutil.which("riscv64-linux-gnu-as")
    objdump = shutil.which("riscv64-linux-gnu-objdump")
    if not assembler or not objdump:
        pytest.skip("RISC-V binutils are unavailable")
    source = tmp_path / "record_v2.S"
    source.write_text(
        ".text\n"
        ".insn r 0x2b, 0x0, 0x00, a0, a1, zero\n"
        ".insn r 0x2b, 0x0, 0x01, a0, a1, zero\n"
        ".insn r4 0x2b, 0x1, 0x0, fa0, a1, a2, a3\n")
    obj = tmp_path / "record_v2.o"
    assembled = subprocess.run(
        [assembler, "-march=rv64gc", "-o", str(obj), str(source)],
        capture_output=True, text=True, timeout=30, check=False)
    assert assembled.returncode == 0, assembled.stderr
    decoded = subprocess.run(
        [objdump, "-d", str(obj)],
        capture_output=True, text=True, timeout=30, check=False)
    assert decoded.returncode == 0, decoded.stderr
    instructions = [
        int(word, 16) for word in re.findall(
            r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s", decoded.stdout, re.MULTILINE)
    ]
    assert instructions == [0x0005852B, 0x0205852B, 0x68C5952B], decoded.stdout
    assert all(instruction & 0x7F == 0x2B for instruction in instructions)
    assert (instructions[-1] >> 27) & 0x1F == 13


def test_v2_inline_assembly_accepts_real_address_third_source(tmp_path):
    compiler = shutil.which("riscv64-linux-gnu-g++")
    if compiler is None:
        pytest.skip("RISC-V C++ compiler is unavailable")
    source = tmp_path / "record_v2.cc"
    source.write_text(r'''
#include <cstdint>
float load_property(uint64_t base, uint64_t record, uint64_t record_address) {
    float result;
    asm volatile(".insn r4 0x2b, 0x1, 0x0, %0, %1, %2, %3"
                 : "=f"(result)
                 : "r"(base), "r"(record), "r"(record_address)
                 : "memory");
    return result;
}
''')
    compiled = subprocess.run(
        [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
         "-c", str(source), "-o", str(tmp_path / "record_v2.o")],
        capture_output=True, text=True, timeout=30, check=False)
    assert compiled.returncode == 0, compiled.stderr
