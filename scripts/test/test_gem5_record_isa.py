"""Adaptive record operations use explicit raw-word and real-address operands."""

from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_record_overlay_patch_syntax_and_applicability():
    gem5 = ROOT / "bench/include/gem5_sim/gem5"
    overlays = ROOT / "bench/include/gem5_sim/overlays"
    for relative in (
        "arch/riscv/ecg_record_csr.patch",
        "arch/riscv/ecg_record_operand.patch",
        "cpu/ecg_record_producer.patch",
    ):
        patch = overlays / relative
        parsed = subprocess.run(
            ["git", "apply", "--numstat", str(patch)],
            cwd=ROOT, capture_output=True, text=True, timeout=10)
        assert parsed.returncode == 0, parsed.stderr
        if not (gem5 / "src/cpu/o3/dyn_inst.hh").exists():
            continue
        applied = subprocess.run(
            ["git", "apply", "--check", str(patch)],
            cwd=gem5, capture_output=True, text=True, timeout=10)
        if applied.returncode:
            applied = subprocess.run(
                ["git", "apply", "--reverse", "--check", str(patch)],
                cwd=gem5, capture_output=True, text=True, timeout=10)
        assert applied.returncode == 0, applied.stderr


def test_record_atomic_execute_fragments_compile(tmp_path):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("g++ is unavailable")
    source = (ROOT / "bench/include/gem5_sim/overlays/arch/riscv/isa/"
              "decoder_ecg_record.isa").read_text()
    fragments = re.findall(
        r"(ecg_record_word|ecg_record_doubleword|ecg_record_property_f32)"
        r"\(\{\{(.*?)\}\}, ea_code=\{\{(.*?)\}\}", source, re.DOTALL)
    assert len(fragments) == 3
    scaffold = r'''
#include <cstdint>
#include <memory>
#include "ecg_record_native.h"
struct IllegalInstFault { IllegalInstFault(const char*, uint32_t) {} };
using Fault = std::shared_ptr<IllegalInstFault>;
enum class FPUStatus { OFF, DIRTY };
struct STATUS {
    FPUStatus fs = FPUStatus::DIRTY;
    STATUS(uint64_t) {}
    operator uint64_t() const { return 0; }
};
enum { MISCREG_STATUS };
struct Exec {
    uint64_t readMiscReg(int) { return 0; }
    void setMiscReg(int, uint64_t) {}
    void setEcgRecordLoadHint(ecg_record::InstructionKind,
                              const ecg_record::NativeLoadResult&) {}
};
namespace RiscvISA {
ecg_record::NativeConfiguration readEcgRecordConfiguration(Exec*) { return {}; }
}
uint64_t rvZext(uint64_t value) { return value; }
uint32_t f32(uint32_t value) { return value; }
struct freg_t { uint64_t v; };
freg_t freg(uint32_t value) { return {value}; }
'''
    for name, access, ea in fragments:
        scaffold += (
            f"Fault {name}() {{\n"
            "Exec exec; auto* xc = &exec;\n"
            "uint64_t Rs1=0, Rs2=0, Rs3=0, EA=0, Rd=0, Fd_bits=0, Mem_ud=0;\n"
            "uint32_t Mem_uw=0, machInst=0;\n"
            f"{ea}\n{access}\nreturn {{}};\n}}\n")
    path = tmp_path / "atomic_fragments.cc"
    path.write_text(scaffold)
    result = subprocess.run(
        [compiler, "-std=c++17", "-fsyntax-only",
         "-I", str(ROOT / "bench/include"), str(path)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr


def test_record_instruction_encodings_and_three_source_operand(tmp_path):
    assembler = shutil.which("riscv64-linux-gnu-as")
    objdump = shutil.which("riscv64-linux-gnu-objdump")
    if not assembler or not objdump:
        pytest.skip("RISC-V binutils are unavailable")
    source = tmp_path / "record.S"
    source.write_text(
        ".text\n"
        ".insn r 0x2b, 0x0, 0x00, a0, a1, zero\n"
        ".insn r 0x2b, 0x0, 0x01, a0, a1, zero\n"
        ".insn r4 0x2b, 0x1, 0x0, fa0, a1, a2, a3\n"
        ".insn r 0x2b, 0x2, 0x0, zero, zero, zero\n"
        ".insn r 0x2b, 0x3, 0x0, a0, zero, zero\n")
    obj = tmp_path / "record.o"
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
    assert instructions == [
        0x0005852B, 0x0205852B, 0x68C5952B, 0x0000202B, 0x0000352B], decoded.stdout
    assert all(instruction & 0x7F == 0x2B for instruction in instructions)
    assert (instructions[2] >> 27) & 0x1F == 13


def test_record_inline_assembly_accepts_real_address_third_source(tmp_path):
    compiler = shutil.which("riscv64-linux-gnu-g++")
    if compiler is None:
        pytest.skip("RISC-V C++ compiler is unavailable")
    source = tmp_path / "record.cc"
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
         "-c", str(source), "-o", str(tmp_path / "record.o")],
        capture_output=True, text=True, timeout=30, check=False)
    assert compiled.returncode == 0, compiled.stderr


def test_record_host_data_path_and_bad_address(tmp_path):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("g++ is unavailable")
    binary = tmp_path / "record_isa_smoke"
    result = subprocess.run(
        [compiler, "-std=c++17", "-O2", "-DNO_M5OPS",
         "-I", str(ROOT / "bench/include"),
         "-I", str(ROOT / "bench/include/external/gapbs"),
         str(ROOT / "bench/src_gem5/record_isa_smoke.cc"), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    for width in ("4", "8"):
        ran = subprocess.run(
            [str(binary), width], capture_output=True, text=True, timeout=10)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        assert f"native=0 record_bytes={width} cases=8" in ran.stdout
        assert "result=PASS" in ran.stdout
    bad = subprocess.run(
        [str(binary), "bad-address"], capture_output=True, text=True, timeout=10)
    assert bad.returncode == 6
    assert "Invalid native ECG record address" in bad.stderr
