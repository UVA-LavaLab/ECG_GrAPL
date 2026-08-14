import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.experiments.ecg.analysis.reuse_plan_rtl_packet import emit  # noqa: E402


def test_rtl_packet_hashes_synthesis_inputs(tmp_path: Path):
    payload = emit(tmp_path)
    assert payload["status"] == "inputs_only_unmeasured"
    assert payload["technology_nm_required"] == 32
    replacement = payload["replacement_ranking_subcomponent"]
    assert replacement["parameters"]["WAYS"] == 16
    assert "Ranking and RRIP aging only" in replacement["scope"]
    assert payload["replacement_path"]["top"] == "reuse_plan_replacement_path"
    assert payload["replacement_path"]["parameters"]["EPOCH_BITS"] == 15
    assert payload["request_path_units"]["payload_bits"] == 95
    assert payload["request_path_units"]["machine_wide_counts_status"] == (
        "parameterized_unfrozen")
    assert payload["request_path_units"]["tops"]["mshr_slot"] == (
        "reuse_bind_request_state_slot")
    assert payload["ecc"]["area_instances"] == {
        "encoders": 16,
        "decoders": 16,
    }
    for entry in (
            replacement["source"],
            replacement["policy_source"],
            *payload["replacement_path"]["sources"],
            *payload["request_path_units"]["sources"],
            payload["ecc"]["source"],
            payload["verification"]["testbench"],
            payload["verification"]["replacement_testbench"],
            payload["verification"]["request_testbench"]):
        path = ROOT / entry["path"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"]
    serialized = json.loads(
        (tmp_path / "reuse_plan_rtl_manifest.json").read_text())
    assert serialized == payload
