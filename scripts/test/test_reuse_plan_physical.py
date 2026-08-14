import json
import subprocess
from pathlib import Path

import pytest

from scripts.experiments.ecg.analysis.reuse_plan_physical import characterize, template


ROOT = Path(__file__).resolve().parents[2]


def measured_input():
    data = template()
    data.update({
        "technology_nm": 32,
        "synthesis_technology_nm": 32,
        "cache_bytes": 8 * 1024 * 1024,
        "baseline_ways": 16,
        "metadata_port_model": "1rw",
        "metadata_access_fraction": 1.0,
        "replacement_access_fraction": 1.0,
        "request_path_access_fraction": 1.0,
    })
    data["baseline_cache"] = {
        "area_mm2": 4.0,
        "read_energy_nj": 1.0,
        "write_energy_nj": 1.2,
        "leakage_mw": 20.0,
        "delay_ns": 2.0,
    }
    data["reuse_plan_metadata_sram"] = {
        "area_mm2": 0.4,
        "read_energy_nj": 0.1,
        "write_energy_nj": 0.12,
        "leakage_mw": 2.0,
        "delay_ns": 1.0,
    }
    data["reuse_plan_ecc_logic"] = {
        "area_mm2": 0.005,
        "read_energy_nj": 0.002,
        "write_energy_nj": 0.003,
        "leakage_mw": 0.02,
        "delay_ns": 0.02,
    }
    data["reuse_plan_replacement_logic"] = {
        "area_mm2": 0.04,
        "read_energy_nj": 0.02,
        "write_energy_nj": 0.02,
        "leakage_mw": 0.2,
        "delay_ns": 0.1,
    }
    data["reuse_bind_request_path"] = {
        "critical_delay_ns": 0.05,
        "units": {
            "mshr_slot": {
                "instances": 2,
                "activations_per_access": 1.0,
                "scales_with_ways": False,
                "area_mm2": 0.002,
                "read_energy_nj": 0.001,
                "write_energy_nj": 0.001,
                "leakage_mw": 0.005,
                "delay_ns": 0.02,
            },
            "csr_per_hart": {
                "instances": 1,
                "activations_per_access": 1.0,
                "scales_with_ways": False,
                "area_mm2": 0.001,
                "read_energy_nj": 0.001,
                "write_energy_nj": 0.001,
                "leakage_mw": 0.005,
                "delay_ns": 0.01,
            },
            "sequence_allocator": {
                "instances": 1,
                "activations_per_access": 1.0,
                "scales_with_ways": False,
                "area_mm2": 0.001,
                "read_energy_nj": 0.001,
                "write_energy_nj": 0.001,
                "leakage_mw": 0.005,
                "delay_ns": 0.01,
            },
            "pipeline_copy": {
                "instances": 4,
                "activations_per_access": 2.0,
                "scales_with_ways": False,
                "area_mm2": 0.001,
                "read_energy_nj": 0.001,
                "write_energy_nj": 0.001,
                "leakage_mw": 0.005,
                "delay_ns": 0.01,
            },
            "recency_rank_per_set": {
                "instances": 2,
                "activations_per_access": 0.0,
                "scales_with_ways": True,
                "area_mm2": 0.001,
                "read_energy_nj": 0.001,
                "write_energy_nj": 0.001,
                "leakage_mw": 0.005,
                "delay_ns": 0.01,
            },
        },
    }
    data["provenance"] = {
        "cacti_version": "test-cacti",
        "cacti_source_sha256": "a" * 64,
        "cacti_binary_sha256": "b" * 64,
        "cacti_packet_manifest_sha256": "c" * 64,
        "synthesis_tool": "test-synth",
        "technology_library": "test-lib",
        "technology_library_sha256": "d" * 64,
        "baseline_config_sha256": "e" * 64,
        "baseline_report_sha256": "f" * 64,
        "metadata_config_sha256": "1" * 64,
        "metadata_report_sha256": "2" * 64,
        "ecc_logic_input_sha256": "3" * 64,
        "ecc_logic_report_sha256": "4" * 64,
        "replacement_logic_input_sha256": "5" * 64,
        "replacement_logic_report_sha256": "6" * 64,
        "request_path_logic_input_sha256": "7" * 64,
        "request_path_logic_report_sha256": "8" * 64,
    }
    return data


def test_characterize_measured_physical_inputs():
    result = characterize(measured_input())
    assert result["reuse_plan_total_area_mm2"] == pytest.approx(4.457)
    assert result["reuse_plan_area_overhead_percent"] == pytest.approx(11.425)
    assert result["reuse_plan_read_energy_nj"] == pytest.approx(1.127)
    assert result["parallel_lookup_delay_ns"] == pytest.approx(2.0)
    assert result["request_to_data_parallel_delay_ns"] == pytest.approx(2.05)
    assert result["metadata_read_with_ecc_delay_ns"] == pytest.approx(1.02)
    assert result["eviction_selection_delay_ns"] == pytest.approx(1.12)
    assert result["serialized_request_to_data_delay_ns"] == pytest.approx(3.07)
    assert result["serialized_all_components_upper_bound_ns"] == pytest.approx(
        3.17)
    assert result["linear_equal_area_fractional_ways"] == pytest.approx(
        14.3408214205)
    assert result["linear_equal_area_integral_ways"] == 14
    assert result["linear_equal_area_integral_effective_bytes"] == 7340032


def test_characterize_rejects_missing_or_placeholder_values():
    with pytest.raises(ValueError, match="technology_nm"):
        characterize(template())
    data = measured_input()
    data["reuse_plan_metadata_sram"]["area_mm2"] = None
    with pytest.raises(ValueError, match="reuse_plan_metadata_sram.area_mm2"):
        characterize(data)
    data = measured_input()
    data["baseline_ways"] = 16.5
    with pytest.raises(ValueError, match="baseline_ways must be an integer"):
        characterize(data)
    data = measured_input()
    data["provenance"]["cacti_version"] = None
    with pytest.raises(ValueError, match="provenance.cacti_version"):
        characterize(data)
    data = measured_input()
    data["provenance"]["baseline_config_sha256"] = "not-a-hash"
    with pytest.raises(ValueError, match="lowercase SHA-256"):
        characterize(data)
    data = measured_input()
    data["synthesis_technology_nm"] = 45
    with pytest.raises(ValueError, match="must match technology_nm"):
        characterize(data)
    data = measured_input()
    data["reuse_bind_request_path"]["units"]["mshr_slot"]["instances"] = None
    with pytest.raises(ValueError, match="mshr_slot.instances"):
        characterize(data)
    data = measured_input()
    data["reuse_bind_request_path"]["units"]["pipeline_copy"][
        "activations_per_access"] = 5
    with pytest.raises(ValueError, match="must be <= instances"):
        characterize(data)
    data = measured_input()
    data["reuse_bind_request_path"]["units"]["mshr_slot"]["instances"] = 0
    with pytest.raises(ValueError, match="mshr_slot.instances"):
        characterize(data)
    data = measured_input()
    data["reuse_bind_request_path"]["units"]["sequence_allocator"]["instances"] = 0
    data["reuse_bind_request_path"]["units"]["sequence_allocator"][
        "activations_per_access"] = 0
    assert characterize(data)["reuse_bind_request_path"]["units"][
        "sequence_allocator"]["instances"] == 0


def test_cli_template_and_input(tmp_path: Path):
    script = ROOT / "scripts/experiments/ecg/analysis/reuse_plan_physical.py"
    template_result = subprocess.run(
        ["python3", str(script), "--template"],
        check=True, capture_output=True, text=True)
    assert json.loads(template_result.stdout)["baseline_cache"]["area_mm2"] is None

    input_path = tmp_path / "physical.json"
    input_path.write_text(json.dumps(measured_input()))
    measured_result = subprocess.run(
        ["python3", str(script), "--input", str(input_path)],
        check=True, capture_output=True, text=True)
    assert json.loads(measured_result.stdout)[
        "linear_equal_area_integral_ways"] == 14
