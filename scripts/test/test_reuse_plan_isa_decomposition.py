import subprocess
from pathlib import Path

from scripts.experiments.ecg.analysis.reuse_plan_isa_decomposition import (
    analyze,
    assemble,
    disassemble,
)


ROOT = Path(__file__).resolve().parents[2]


def test_canonical_reuse_plan_isa_decomposition(tmp_path: Path):
    source = ROOT / "bench/src_gem5/reuse_plan_isa_decomposition.S"
    object_path = tmp_path / "reuse_plan_isa.o"
    assemble(source, object_path)
    result = analyze(disassemble(object_path))
    baseline = result["rows"]["baseline_u32_d32"]
    reuse_plan_m = result["rows"]["reuse_plan_m_u32_d32"]
    reuse_plan_i = result["rows"]["reuse_plan_i_u32_d32"]

    assert baseline["body_instructions"] == 6
    assert baseline["ordinary_property_load"] == 1
    assert baseline["destination_extract"] == 2
    assert baseline["address_generation"] == 2

    assert reuse_plan_m["body_instructions"] == 6
    assert reuse_plan_m["reuse_bind_load"] == 1
    assert reuse_plan_m["instructions_vs_baseline"] == 0
    assert reuse_plan_m["destination_extract"] == 2
    assert reuse_plan_m["address_generation"] == 2
    assert reuse_plan_m["custom_instruction_word"] == "3053250b"

    assert reuse_plan_i["body_instructions"] == 2
    assert reuse_plan_i["reuse_bind_iload"] == 1
    assert reuse_plan_i["destination_extract"] == 0
    assert reuse_plan_i["address_generation"] == 0
    assert reuse_plan_i["instructions_vs_baseline"] == -4
    assert reuse_plan_i["custom_instruction_word"] == "1855a50b"

    harness = (ROOT / "bench/include/gem5_sim/gem5_harness.h").read_text()
    assert '".insn r 0x0b, 0x2, 0x0c' in harness
    assert '".insn r 0x0b, 0x2, 0x18' in harness


def test_reuse_plan_isa_cli():
    script = (
        ROOT / "scripts/experiments/ecg/analysis/"
        "reuse_plan_isa_decomposition.py")
    result = subprocess.run(
        ["python3", str(script)],
        check=True, capture_output=True, text=True)
    assert "| Baseline | 6 |" in result.stdout
    assert "| ReuseBind | 6 |" in result.stdout
    assert "| ReuseBind-Indexed | 2 |" in result.stdout
