"""Scale6 raw-opcode, host-reference and native ISA checks."""

import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
GEM5 = ROOT / "bench/include/gem5_sim/gem5/build/RISCV/gem5.opt"
GUEST = ROOT / "bench/bin_gem5/ref32_isa_smoke_riscv_m5ops"


def test_ref32_host_reference(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    binary = tmp_path / "ref32_isa_smoke"
    built = subprocess.run([
        compiler, "-std=c++17", "-O2", "-DNO_M5OPS",
        f"-I{ROOT / 'bench/include'}",
        f"-I{ROOT / 'bench/include/external/gapbs'}",
        str(ROOT / "bench/src_gem5/ref32_isa_smoke.cc"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stderr
    good = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert good.returncode == 0, good.stdout + good.stderr
    assert "native=0 cases=6 result=PASS" in good.stdout
    bad = subprocess.run(
        [str(binary), "bad-address"], capture_output=True, text=True, timeout=30)
    assert bad.returncode == 4
    assert "Invalid Scale6 record address" in bad.stderr


def test_ref32_instruction_encodings(tmp_path):
    assembler = shutil.which("riscv64-linux-gnu-as")
    objdump = shutil.which("riscv64-linux-gnu-objdump")
    if not assembler or not objdump:
        pytest.skip("RISC-V binutils are unavailable")
    source = tmp_path / "ref32.S"
    source.write_text(
        ".text\n"
        ".insn r 0x0b, 0x2, 0x30, a0, a1, a2\n"
        ".insn r 0x0b, 0x2, 0x34, fa0, a1, a2\n")
    obj = tmp_path / "ref32.o"
    subprocess.run(
        [assembler, "-march=rv64gc", "-o", str(obj), str(source)],
        check=True, capture_output=True, text=True)
    decoded = subprocess.run(
        [objdump, "-d", str(obj)], check=True, capture_output=True, text=True)
    assert "60c5a50b" in decoded.stdout
    assert "68c5a50b" in decoded.stdout


def test_ref32_decoder_is_independent_of_legacy_mailboxes():
    source = (
        ROOT / "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa").read_text()
    native = source[source.index("0x0C: decode RVTYPE"):]
    assert "0x0D: decode RVTYPE" in native
    assert native.count("static_cast<uint32_t>(Mem_uw)") == 1
    assert "Fd_bits = fd.v" in native
    assert "nativeRecordPosition" in native
    assert "canonicalScaleRecord" in native
    assert "nativePropertyAccess" in native
    assert "setEcgRef32RecordHint" in native
    assert "setEcgRef32LoadHint" in native
    assert "storeEcgMetadataByVertex" not in native
    assert "setDecodedEcgExtractHint" not in native
    assert "ECG_FLOWTHROUGH" not in native


def test_ref32_atomic_execute_fragments_compile(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    source = (
        ROOT / "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa").read_text()
    fragments = re.findall(
        r"(ecg_ref32_record|ecg_ref32_property_f32)\(\{\{(.*?)\}\}, "
        r"ea_code=\{\{(.*?)\}\}", source, re.DOTALL)
    assert len(fragments) == 2
    scaffold = r'''
#include <cstdint>
#include <memory>
#include <string>
#include "ecg_ref32.h"
struct IllegalInstFault {
    IllegalInstFault(const char*, uint32_t) {}
};
using Fault = std::shared_ptr<IllegalInstFault>;
enum class FPUStatus { OFF, DIRTY };
struct STATUS {
    FPUStatus fs = FPUStatus::DIRTY;
    STATUS(uint64_t) {}
    operator uint64_t() const { return 0; }
};
enum { MISCREG_ECG_CONTEXT, MISCREG_ECG_REF32_BASE,
       MISCREG_ECG_REF32_CONFIG, MISCREG_STATUS };
struct Exec {
    uint64_t readMiscReg(int) { return 0; }
    void setMiscReg(int, uint64_t) {}
    void setEcgRef32RecordHint(uint64_t, uint64_t, uint16_t) {}
    void setEcgRef32LoadHint(uint32_t, uint32_t, uint32_t, uint8_t, uint16_t) {}
};
uint64_t rvZext(uint64_t value) { return value; }
uint32_t f32(uint32_t value) { return value; }
struct freg_t { uint64_t v; };
freg_t freg(uint32_t value) { return {value}; }
'''
    for name, access, ea in fragments:
        scaffold += (
            f"Fault {name}() {{\n"
            "Exec exec; auto* xc = &exec;\n"
            "uint64_t Rs1=0, Rs2=0, EA=0, Rd=0, Fd_bits=0;\n"
            "uint32_t Mem_uw=0, machInst=0;\n"
            f"{ea}\n{access}\nreturn {{}};\n}}\n")
    path = tmp_path / "atomic_fragments.cc"
    path.write_text(scaffold)
    result = subprocess.run(
        [compiler, "-std=c++17", "-fsyntax-only",
         f"-I{ROOT / 'bench/include'}", str(path)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr


@pytest.mark.skipif(
    not GEM5.exists() or not GUEST.exists(),
    reason="native RISCV simulator/ISA smoke guest not built")
@pytest.mark.parametrize("bad", [False, True], ids=["data", "invalid-address"])
def test_ref32_native_isa(tmp_path, bad):
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))
    }
    env["OMP_NUM_THREADS"] = "1"
    command = [
        str(GEM5), "--outdir", str(tmp_path),
        str(ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py"),
        "--binary", str(GUEST), "--cpu-type", "O3",
        "--policy", "LRU", "--prefetcher", "none",
    ]
    if bad:
        command += ["--options", "bad-address"]
    result = subprocess.run(
        command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
    output = result.stdout + result.stderr
    for name in ("benchmark_stdout.txt", "benchmark_stderr.txt"):
        path = tmp_path / name
        if path.exists():
            output += path.read_text()
    if bad:
        assert result.returncode != 0
        assert "Invalid Scale6 record address" in output
    else:
        assert result.returncode == 0, output[-6000:]
        assert "native=1 cases=6 result=PASS" in output
