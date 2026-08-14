import csv
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.experiments.ecg.analysis.reuse_plan_cacti_packet import (  # noqa: E402
    REUSE_PLAN_METADATA_BYTES,
    emit_packet,
    parse_cacti_csv,
    physical_input_partial,
    validate_cacti_profile,
)


def test_packet_emits_hashed_baseline_and_metadata_configs(tmp_path: Path):
    manifest = emit_packet(tmp_path)
    assert manifest["status"] == "inputs_only_unmeasured"
    assert manifest["baseline"]["ways"] == 16
    assert manifest["metadata"]["logical_bits_per_entry"] == 49
    assert manifest["metadata"]["secded_bits_per_entry"] == 7
    assert manifest["metadata"]["macro_bits_per_entry"] == 64
    assert manifest["metadata"]["set_row_bits"] == 1024
    assert manifest["metadata"]["sets"] == 8192
    assert manifest["metadata"]["macro_bytes"] == REUSE_PLAN_METADATA_BYTES
    assert len(manifest["configs"]) == 3
    for config in manifest["configs"].values():
        path = tmp_path / config["path"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == config["sha256"]

    baseline = (tmp_path / "baseline_llc_8mib_16way.cfg").read_text()
    assert "\n-size (bytes) 8388608\n" in baseline
    assert "\n-associativity 16\n" in baseline
    assert '\n-cache type "cache"\n' in baseline
    metadata = (tmp_path / "reuse_plan_metadata_1rw.cfg").read_text()
    assert "\n-size (bytes) 1048576\n" in metadata
    assert "\n-block size (bytes) 128\n" in metadata
    assert "\n-output/input bus width 1024\n" in metadata
    assert '\n-cache type "ram"\n' in metadata
    assert '\n-Add ECC - "false"\n' in metadata


def test_cacti_csv_parser_maps_physical_metrics(tmp_path: Path):
    path = tmp_path / "out.csv"
    headers = [
        "Tech node (nm)", "Capacity (bytes)", "Number of banks",
        "Associativity", "Output width (bits)", "Access time (ns)",
        "Dynamic read energy (nJ)", "Dynamic write energy (nJ)",
        "Standby leakage per bank(mW)", "Area (mm2)",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=headers)
        writer.writeheader()
        writer.writerow({
            "Tech node (nm)": "32",
            "Capacity (bytes)": "8388608",
            "Number of banks": "1",
            "Associativity": "16",
            "Output width (bits)": "512",
            "Access time (ns)": "1.25",
            "Dynamic read energy (nJ)": "0.5",
            "Dynamic write energy (nJ)": "0.6",
            "Standby leakage per bank(mW)": "3.0",
            "Area (mm2)": "2.5",
        })
    assert parse_cacti_csv(path) == {
        "area_mm2": 2.5,
        "read_energy_nj": 0.5,
        "write_energy_nj": 0.6,
        "leakage_mw": 3.0,
        "delay_ns": 1.25,
        "technology_nm": 32,
        "capacity_bytes": 8388608,
        "banks": 1,
        "associativity": 16,
        "output_width_bits": 512,
    }
    directives = {
        "-technology (u)": "0.032",
        "-size (bytes)": "8388608",
        "-UCA bank count": "1",
        "-associativity": "16",
        "-output/input bus width": "512",
    }
    validate_cacti_profile("baseline", parse_cacti_csv(path), directives)
    directives["-associativity"] = "8"
    try:
        validate_cacti_profile("baseline", parse_cacti_csv(path), directives)
    except ValueError as exc:
        assert "geometry mismatch" in str(exc)
    else:
        raise AssertionError("mismatched CACTI geometry was accepted")


def test_manifest_is_json_serializable(tmp_path: Path):
    emit_packet(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    assert "14-way" in " ".join(manifest["limitations"])


def test_measurement_bridge_maps_hashes_to_physical_schema(tmp_path: Path):
    manifest = emit_packet(tmp_path)
    metric = {
        "area_mm2": 1.0,
        "read_energy_nj": 2.0,
        "write_energy_nj": 3.0,
        "leakage_mw": 4.0,
        "delay_ns": 5.0,
        "config_sha256": "a" * 64,
        "report_sha256": "b" * 64,
        "stdout_sha256": "c" * 64,
    }
    payload = {
        "manifest_sha256": "d" * 64,
        "cacti_binary_sha256": "e" * 64,
        "measurements": {
            "baseline_llc_8mib_16way": dict(metric),
            "reuse_plan_metadata_1rw": dict(metric),
            "reuse_plan_metadata_1r1w": dict(metric),
        },
    }
    partial = physical_input_partial(manifest, payload)
    assert partial["technology_nm"] == 32
    assert partial["metadata_port_model"] == "1rw"
    assert partial["baseline_cache"]["area_mm2"] == 1.0
    assert partial["provenance"]["cacti_binary_sha256"] == "e" * 64
    assert partial["reuse_plan_ecc_logic"]["area_mm2"] is None
    sensitivity = physical_input_partial(
        manifest, payload, metadata_name="reuse_plan_metadata_1r1w")
    assert sensitivity["metadata_port_model"] == "1r1w"
