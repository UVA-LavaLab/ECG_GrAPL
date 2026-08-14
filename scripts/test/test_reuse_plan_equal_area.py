from types import SimpleNamespace
import json

import pytest

from scripts.experiments.ecg import roi_matrix


def args(reuse_plan_ways: int) -> SimpleNamespace:
    return SimpleNamespace(
        benchmark="pr",
        l3_ways="16",
        reuse_plan_l3_ways=reuse_plan_ways,
        line_size="64",
        popt_reserve_model="size_correct",
        options="-g 10 -k 16 -o 5 -n 1",
    )


def test_reuse_plan_14_way_override_preserves_16_way_baselines():
    reuse_plan = roi_matrix.policy_cache_geometry(
        args(14), roi_matrix.parse_policy_spec("ECG:REUSE_PLAN"), "8MB")
    lru = roi_matrix.policy_cache_geometry(
        args(14), roi_matrix.parse_policy_spec("LRU"), "8MB")

    assert reuse_plan["reuse_plan_area_mode"] == "equal_silicon_sensitivity"
    assert reuse_plan["reuse_plan_baseline_l3_ways"] == 16
    assert reuse_plan["reuse_plan_effective_l3_ways"] == "14"
    assert reuse_plan["popt_effective_l3_ways"] == "14"
    assert reuse_plan["reuse_plan_effective_l3_size"] == "7340032B"
    assert reuse_plan["reuse_plan_metadata_bits_per_line"] == 49

    assert lru["reuse_plan_area_mode"] == "baseline_equal_silicon_reference"
    assert lru["popt_effective_l3_ways"] == "16"
    assert lru["popt_effective_l3_size"] == "8MB"


def test_reuse_plan_15_way_and_equal_capacity_modes():
    sensitivity = roi_matrix.policy_cache_geometry(
        args(15), roi_matrix.parse_policy_spec("ECG:REUSE_PLAN_ONLINE"), "8MB")
    equal_capacity = roi_matrix.policy_cache_geometry(
        args(0), roi_matrix.parse_policy_spec("ECG:REUSE_PLAN"), "8MB")

    assert sensitivity["reuse_plan_effective_l3_ways"] == "15"
    assert sensitivity["reuse_plan_effective_l3_size"] == "7864320B"
    assert equal_capacity["reuse_plan_area_mode"] == "equal_capacity"
    assert equal_capacity["popt_effective_l3_ways"] == "16"


def test_reuse_plan_override_cannot_exceed_baseline_associativity():
    with pytest.raises(ValueError, match="cannot exceed baseline"):
        roi_matrix.policy_cache_geometry(
            args(17), roi_matrix.parse_policy_spec("ECG:REUSE_PLAN"), "8MB")


def test_experiment_runner_forwards_reuse_plan_way_override():
    text = (
        roi_matrix.PROJECT_ROOT /
        "scripts/experiments/ecg/flows/experiment_run.py").read_text()
    assert '"--reuse-plan-l3-ways"' in text
    assert 'settings.get("reuse_plan_l3_ways", 0)' in text


def test_manifest_defines_both_equal_area_sensitivities():
    manifest = json.loads(
        (roi_matrix.PROJECT_ROOT /
         "scripts/experiments/ecg/experiment_manifest.json").read_text())
    assert "ecg_equal_area_15" in manifest["profiles"]
    assert "ecg_equal_area_14" in manifest["profiles"]
    stages = {
        profile: stage
        for stage in manifest["stages"]
        for profile in stage.get("profiles", [])
        if profile in ("ecg_equal_area_15", "ecg_equal_area_14")
    }
    assert stages["ecg_equal_area_15"]["reuse_plan_l3_ways"] == 15
    assert stages["ecg_equal_area_14"]["reuse_plan_l3_ways"] == 14
    for stage in stages.values():
        assert stage.get("l3_ways", "16") == "16"
        assert stage["suite"] == "cache-sim"
        assert "LRU" in stage["policies"]
        assert "HAWKEYE:PROXY" in stage["policies"]
        assert "ECG:REUSE_PLAN_ONLINE" in stage["policies"]
