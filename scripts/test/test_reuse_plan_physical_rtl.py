import shutil
import re
from pathlib import Path

import pytest

from scripts.experiments.ecg.analysis.reuse_plan_rtl_verify import verify


ROOT = Path(__file__).resolve().parents[2]
RTL = ROOT / "bench/src_rtl"


def test_reuse_plan_physical_rtl_matches_policy_and_ecc(tmp_path: Path):
    if not shutil.which("verilator") or not shutil.which("yosys"):
        pytest.skip("generic RTL verification tools are unavailable")
    result = verify(tmp_path)
    assert result["status"] == "passed"


def test_rtl_variant_numbers_match_cpp_policy():
    cpp = (ROOT / "bench/include/ecg_victim_policy.h").read_text()
    rtl = (RTL / "reuse_plan_victim_select.sv").read_text()
    variants = {
        "GRASP_ONLY": 0,
        "EPOCH_FIRST": 1,
        "RRIP_FIRST": 2,
        "EPOCH_ONLY": 3,
        "SHORTCIRCUIT": 4,
        "DEGREE_FIRST": 5,
        "LRU_ONLY": 6,
    }
    for name, value in variants.items():
        assert re.search(rf"\b{name}\s*=\s*{value}\b", cpp)
        assert f"{name} = 3'd{value}" in rtl
