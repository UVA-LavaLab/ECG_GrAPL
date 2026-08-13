import subprocess
from pathlib import Path

from scripts.experiments.ecg.analysis.k2_isa_decomposition import (
    analyze,
    assemble,
    disassemble,
)


ROOT = Path(__file__).resolve().parents[2]


def test_canonical_k2_isa_decomposition(tmp_path: Path):
    source = ROOT / "bench/src_gem5/k2_isa_decomposition.S"
    object_path = tmp_path / "k2_isa.o"
    assemble(source, object_path)
    result = analyze(disassemble(object_path))
    baseline = result["rows"]["baseline_u32_d32"]
    k2_m = result["rows"]["k2_m_u32_d32"]
    k2_i = result["rows"]["k2_i_u32_d32"]

    assert baseline["body_instructions"] == 6
    assert baseline["ordinary_property_load"] == 1
    assert baseline["destination_extract"] == 2
    assert baseline["address_generation"] == 2

    assert k2_m["body_instructions"] == 6
    assert k2_m["k2_mload"] == 1
    assert k2_m["instructions_vs_baseline"] == 0
    assert k2_m["destination_extract"] == 2
    assert k2_m["address_generation"] == 2
    assert k2_m["custom_instruction_word"] == "3053250b"

    assert k2_i["body_instructions"] == 2
    assert k2_i["k2_iload"] == 1
    assert k2_i["destination_extract"] == 0
    assert k2_i["address_generation"] == 0
    assert k2_i["instructions_vs_baseline"] == -4
    assert k2_i["custom_instruction_word"] == "1855a50b"

    harness = (ROOT / "bench/include/gem5_sim/gem5_harness.h").read_text()
    assert '".insn r 0x0b, 0x2, 0x0c' in harness
    assert '".insn r 0x0b, 0x2, 0x18' in harness


def test_k2_isa_cli():
    script = (
        ROOT / "scripts/experiments/ecg/analysis/"
        "k2_isa_decomposition.py")
    result = subprocess.run(
        ["python3", str(script)],
        check=True, capture_output=True, text=True)
    assert "| Baseline | 6 |" in result.stdout
    assert "| K2-M | 6 |" in result.stdout
    assert "| K2-I | 2 |" in result.stdout
