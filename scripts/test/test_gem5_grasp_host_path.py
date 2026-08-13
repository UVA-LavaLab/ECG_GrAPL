import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
ROI_MATRIX = ROOT / "scripts/experiments/ecg/roi_matrix.py"
spec = importlib.util.spec_from_file_location(
    "grasp_host_roi_matrix", ROI_MATRIX)
roi_matrix = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["grasp_host_roi_matrix"] = roi_matrix
spec.loader.exec_module(roi_matrix)


def test_gem5_grasp_has_roi_hot_property_stat():
    source = (
        ROOT /
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/grasp_rp.cc"
    ).read_text()
    header = (
        ROOT /
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/grasp_rp.hh"
    ).read_text()

    assert "[GRASP-ACTIVE sim=gem5 context=1 regions=%u]" in source
    assert source.count("++graspStats.hotPropertyAccesses") == 2
    assert "ADD_STAT(" in source
    assert "hotPropertyAccesses" in header
    assert source.count("data->line_addr = 0;") >= 2
    assert "data->line_addr != 0" in source
    assert roi_matrix.GEM5_STAT_KEYS["grasp_hot_property_accesses"] == (
        "system.l3cache.replacement_policy.hotPropertyAccesses")


def test_gem5_grasp_context_receipt_is_fail_closed():
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_grasp_receipt(
        row,
        "[GRASP-ACTIVE sim=gem5 context=1 regions=2]",
        required=True)
    assert row["grasp_context_loaded"] == 1
    assert row["grasp_regions_loaded"] == 2

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_grasp_receipt(
        missing, "", required=True)
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"
