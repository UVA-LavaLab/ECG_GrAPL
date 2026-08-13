import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
ROI_MATRIX = ROOT / "scripts/experiments/ecg/roi_matrix.py"
spec = importlib.util.spec_from_file_location(
    "online_dueling_roi_matrix", ROI_MATRIX)
roi_matrix = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["online_dueling_roi_matrix"] = roi_matrix
spec.loader.exec_module(roi_matrix)


def valid_row():
    return {
        "timing_valid_for_speedup": "1",
        "gem5_k2_dueling_request_bound_victims": 20000,
        "gem5_k2_dueling_leader_samples": 2048,
        "gem5_k2_dueling_follower_selections": 18000,
        "gem5_k2_dueling_completed_windows": 2,
        "gem5_k2_dueling_winner_changes": 0,
        "gem5_k2_dueling_follower_variant_overrides": 0,
    }


def test_online_dueling_activity_accepts_full_roi_window():
    row = valid_row()
    assert roi_matrix.validate_online_dueling_activity(row, required=True)
    assert "error" not in row


def test_online_dueling_activity_rejects_partial_window():
    row = valid_row()
    row["gem5_k2_dueling_leader_samples"] = 1023
    assert not roi_matrix.validate_online_dueling_activity(
        row, required=True)
    assert row["timing_valid_for_speedup"] == "0"
    assert "leader_samples<1024" in row["error"]


def test_online_dueling_activity_is_optional_for_static_k2():
    row = {}
    assert roi_matrix.validate_online_dueling_activity(
        row, required=False)
    assert row == {}
