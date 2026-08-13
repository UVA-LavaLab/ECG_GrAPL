#!/usr/bin/env python3
"""Combine external CACTI/synthesis measurements for K2.

This module does not estimate physical values. It validates explicit tool
outputs and derives reproducible overhead/equal-area ratios from them.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any


REQUIRED_METRICS = (
    "area_mm2",
    "read_energy_nj",
    "write_energy_nj",
    "leakage_mw",
    "delay_ns",
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REQUEST_UNIT_SCALING = {
    "mshr_slot": False,
    "csr_per_hart": False,
    "sequence_allocator": False,
    "pipeline_copy": False,
    "recency_rank_per_set": True,
}
OPTIONAL_REQUEST_UNITS = {"sequence_allocator", "recency_rank_per_set"}


def template() -> dict[str, Any]:
    return {
        "technology_nm": None,
        "cache_bytes": 8 * 1024 * 1024,
        "baseline_ways": 16,
        "synthesis_technology_nm": None,
        "metadata_port_model": None,
        "metadata_access_fraction": 1.0,
        "replacement_access_fraction": 1.0,
        "request_path_access_fraction": 1.0,
        "baseline_cache": {key: None for key in REQUIRED_METRICS},
        "k2_metadata_sram": {key: None for key in REQUIRED_METRICS},
        "k2_ecc_logic": {key: None for key in REQUIRED_METRICS},
        "k2_replacement_logic": {key: None for key in REQUIRED_METRICS},
        "k2_request_path": {
            "critical_delay_ns": None,
            "units": {
                name: {
                    "instances": None,
                    "activations_per_access": None,
                    "scales_with_ways": scales,
                    **{key: None for key in REQUIRED_METRICS},
                }
                for name, scales in REQUEST_UNIT_SCALING.items()
            },
        },
        "provenance": {
            "cacti_version": None,
            "cacti_source_sha256": None,
            "cacti_binary_sha256": None,
            "cacti_packet_manifest_sha256": None,
            "synthesis_tool": None,
            "technology_library": None,
            "technology_library_sha256": None,
            "baseline_config_sha256": None,
            "baseline_report_sha256": None,
            "metadata_config_sha256": None,
            "metadata_report_sha256": None,
            "ecc_logic_input_sha256": None,
            "ecc_logic_report_sha256": None,
            "replacement_logic_input_sha256": None,
            "replacement_logic_report_sha256": None,
            "request_path_logic_input_sha256": None,
            "request_path_logic_report_sha256": None,
        },
    }


def _positive_number(value: Any, field: str, *, allow_zero: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{field} must be finite")
    if number < 0 or (number == 0 and not allow_zero):
        comparator = "non-negative" if allow_zero else "positive"
        raise ValueError(f"{field} must be {comparator}")
    return number


def _positive_int(value: Any, field: str) -> int:
    number = _positive_number(value, field)
    if not float(number).is_integer():
        raise ValueError(f"{field} must be an integer")
    return int(number)


def _nonnegative_int(value: Any, field: str) -> int:
    number = _positive_number(value, field, allow_zero=True)
    if not float(number).is_integer():
        raise ValueError(f"{field} must be an integer")
    return int(number)


def _provenance(data: dict[str, Any]) -> dict[str, str]:
    value = data.get("provenance")
    if not isinstance(value, dict):
        raise ValueError("missing provenance")
    required = (
        "cacti_version",
        "cacti_source_sha256",
        "cacti_binary_sha256",
        "cacti_packet_manifest_sha256",
        "synthesis_tool",
        "technology_library",
        "technology_library_sha256",
        "baseline_config_sha256",
        "baseline_report_sha256",
        "metadata_config_sha256",
        "metadata_report_sha256",
        "ecc_logic_input_sha256",
        "ecc_logic_report_sha256",
        "replacement_logic_input_sha256",
        "replacement_logic_report_sha256",
        "request_path_logic_input_sha256",
        "request_path_logic_report_sha256",
    )
    result: dict[str, str] = {}
    for key in required:
        entry = value.get(key)
        if not isinstance(entry, str) or not entry.strip():
            raise ValueError(f"provenance.{key} must be a non-empty string")
        normalized = entry.strip()
        if key.endswith("_sha256") and not SHA256_RE.fullmatch(normalized):
            raise ValueError(
                f"provenance.{key} must be a lowercase SHA-256 digest")
        result[key] = normalized
    return result


def _component(data: dict[str, Any], name: str) -> dict[str, float]:
    value = data.get(name)
    if not isinstance(value, dict):
        raise ValueError(f"missing component: {name}")
    return {
        key: _positive_number(
            value.get(key), f"{name}.{key}",
            allow_zero=key in ("leakage_mw",))
        for key in REQUIRED_METRICS
    }


def _request_path(data: dict[str, Any]) -> dict[str, Any]:
    value = data.get("k2_request_path")
    if not isinstance(value, dict):
        raise ValueError("missing component: k2_request_path")
    critical_delay = _positive_number(
        value.get("critical_delay_ns"),
        "k2_request_path.critical_delay_ns")
    units = value.get("units")
    if not isinstance(units, dict):
        raise ValueError("k2_request_path.units must be an object")

    normalized: dict[str, Any] = {}
    fixed_area = 0.0
    scaled_area = 0.0
    read_energy = 0.0
    write_energy = 0.0
    leakage = 0.0
    for name, expected_scaling in REQUEST_UNIT_SCALING.items():
        unit = units.get(name)
        if not isinstance(unit, dict):
            raise ValueError(f"missing request-path unit: {name}")
        instance_field = f"k2_request_path.units.{name}.instances"
        instances = (
            _nonnegative_int(unit.get("instances"), instance_field)
            if name in OPTIONAL_REQUEST_UNITS
            else _positive_int(unit.get("instances"), instance_field))
        activations = _positive_number(
            unit.get("activations_per_access"),
            f"k2_request_path.units.{name}.activations_per_access",
            allow_zero=True)
        if activations > instances:
            raise ValueError(
                f"k2_request_path.units.{name}.activations_per_access "
                "must be <= instances")
        scaling = unit.get("scales_with_ways")
        if scaling is not expected_scaling:
            raise ValueError(
                f"k2_request_path.units.{name}.scales_with_ways "
                f"must be {expected_scaling}")
        metrics = {
            key: _positive_number(
                unit.get(key),
                f"k2_request_path.units.{name}.{key}",
                allow_zero=key in ("leakage_mw",))
            for key in REQUIRED_METRICS
        }
        area = instances * metrics["area_mm2"]
        if scaling:
            scaled_area += area
        else:
            fixed_area += area
        read_energy += activations * metrics["read_energy_nj"]
        write_energy += activations * metrics["write_energy_nj"]
        leakage += instances * metrics["leakage_mw"]
        normalized[name] = {
            "instances": instances,
            "activations_per_access": activations,
            "scales_with_ways": scaling,
            **metrics,
        }
    extra = sorted(set(units) - set(REQUEST_UNIT_SCALING))
    if extra:
        raise ValueError(
            "unknown request-path units: " + ", ".join(extra))
    return {
        "critical_delay_ns": critical_delay,
        "units": normalized,
        "fixed_area_mm2": fixed_area,
        "way_scaled_area_mm2": scaled_area,
        "total_area_mm2": fixed_area + scaled_area,
        "read_energy_nj": read_energy,
        "write_energy_nj": write_energy,
        "leakage_mw": leakage,
    }


def characterize(data: dict[str, Any]) -> dict[str, Any]:
    technology_nm = _positive_number(
        data.get("technology_nm"), "technology_nm")
    synthesis_technology_nm = _positive_number(
        data.get("synthesis_technology_nm"), "synthesis_technology_nm")
    if synthesis_technology_nm != technology_nm:
        raise ValueError(
            "synthesis_technology_nm must match technology_nm")
    metadata_port_model = data.get("metadata_port_model")
    if metadata_port_model not in {"1rw", "1r1w"}:
        raise ValueError("metadata_port_model must be 1rw or 1r1w")
    cache_bytes = _positive_int(data.get("cache_bytes"), "cache_bytes")
    baseline_ways = _positive_int(
        data.get("baseline_ways"), "baseline_ways")
    fractions = {}
    for field in (
            "metadata_access_fraction", "replacement_access_fraction",
            "request_path_access_fraction"):
        fraction = _positive_number(
            data.get(field, 1.0), field, allow_zero=True)
        if fraction > 1:
            raise ValueError(f"{field} must be <= 1")
        fractions[field] = fraction

    baseline = _component(data, "baseline_cache")
    metadata = _component(data, "k2_metadata_sram")
    ecc_logic = _component(data, "k2_ecc_logic")
    logic = _component(data, "k2_replacement_logic")
    request_path = _request_path(data)
    provenance = _provenance(data)

    k2_area = (
        baseline["area_mm2"] + metadata["area_mm2"] +
        ecc_logic["area_mm2"] + logic["area_mm2"] +
        request_path["total_area_mm2"])
    area_overhead = k2_area / baseline["area_mm2"] - 1.0
    k2_read_energy = (
        baseline["read_energy_nj"] +
        fractions["metadata_access_fraction"] *
        metadata["read_energy_nj"] +
        fractions["metadata_access_fraction"] *
        ecc_logic["read_energy_nj"] +
        fractions["replacement_access_fraction"] *
        logic["read_energy_nj"] +
        fractions["request_path_access_fraction"] *
        request_path["read_energy_nj"])
    k2_write_energy = (
        baseline["write_energy_nj"] +
        fractions["metadata_access_fraction"] *
        metadata["write_energy_nj"] +
        fractions["metadata_access_fraction"] *
        ecc_logic["write_energy_nj"] +
        fractions["replacement_access_fraction"] *
        logic["write_energy_nj"] +
        fractions["request_path_access_fraction"] *
        request_path["write_energy_nj"])
    k2_leakage = (
        baseline["leakage_mw"] +
        metadata["leakage_mw"] +
        ecc_logic["leakage_mw"] +
        logic["leakage_mw"] +
        request_path["leakage_mw"])

    metadata_read_delay = metadata["delay_ns"] + ecc_logic["delay_ns"]
    parallel_delay = max(baseline["delay_ns"], metadata_read_delay)
    request_to_data_delay = (
        request_path["critical_delay_ns"] + parallel_delay)
    eviction_selection_delay = metadata_read_delay + logic["delay_ns"]
    serialized_request_to_data_delay = (
        request_path["critical_delay_ns"] + baseline["delay_ns"] +
        metadata_read_delay)
    all_components_upper_bound = (
        serialized_request_to_data_delay + logic["delay_ns"])

    # Linearized sensitivity: data/tag, line metadata, and parallel per-way
    # SECDED logic scale with ways; replacement/request logic remains fixed.
    way_scaled_logic_area = (
        ecc_logic["area_mm2"] + request_path["way_scaled_area_mm2"])
    fixed_logic_area = (
        logic["area_mm2"] + request_path["fixed_area_mm2"])
    available_scaling_area = baseline["area_mm2"] - fixed_logic_area
    physical_fractional_ways = (
        baseline_ways * max(available_scaling_area, 0.0) /
        (baseline["area_mm2"] + metadata["area_mm2"] +
         way_scaled_logic_area))
    physical_integral_ways = math.floor(physical_fractional_ways)
    fractional_effective_bytes = math.floor(
        cache_bytes * physical_fractional_ways / baseline_ways)
    integral_effective_bytes = (
        cache_bytes * physical_integral_ways // baseline_ways)

    return {
        "technology_nm": technology_nm,
        "synthesis_technology_nm": synthesis_technology_nm,
        "cache_bytes": cache_bytes,
        "baseline_ways": baseline_ways,
        "metadata_port_model": metadata_port_model,
        **fractions,
        "baseline_cache": baseline,
        "k2_metadata_sram": metadata,
        "k2_ecc_logic": ecc_logic,
        "k2_replacement_logic": logic,
        "k2_request_path": request_path,
        "k2_way_scaled_logic_area_mm2": way_scaled_logic_area,
        "k2_fixed_logic_area_mm2": fixed_logic_area,
        "k2_total_area_mm2": k2_area,
        "k2_area_overhead_ratio": area_overhead,
        "k2_area_overhead_percent": 100.0 * area_overhead,
        "k2_read_energy_nj": k2_read_energy,
        "k2_read_energy_overhead_ratio":
            k2_read_energy / baseline["read_energy_nj"] - 1.0,
        "k2_write_energy_nj": k2_write_energy,
        "k2_write_energy_overhead_ratio":
            k2_write_energy / baseline["write_energy_nj"] - 1.0,
        "k2_total_leakage_mw": k2_leakage,
        "k2_leakage_overhead_ratio":
            k2_leakage / baseline["leakage_mw"] - 1.0
            if baseline["leakage_mw"] > 0 else None,
        "parallel_lookup_delay_ns": parallel_delay,
        "parallel_lookup_delay_overhead_ratio":
            parallel_delay / baseline["delay_ns"] - 1.0,
        "metadata_read_with_ecc_delay_ns": metadata_read_delay,
        "request_to_data_parallel_delay_ns": request_to_data_delay,
        "eviction_selection_delay_ns": eviction_selection_delay,
        "serialized_request_to_data_delay_ns":
            serialized_request_to_data_delay,
        "serialized_all_components_upper_bound_ns":
            all_components_upper_bound,
        "linear_equal_area_fractional_ways": physical_fractional_ways,
        "linear_equal_area_integral_ways": physical_integral_ways,
        "linear_equal_area_fractional_effective_bytes":
            fractional_effective_bytes,
        "linear_equal_area_integral_effective_bytes":
            integral_effective_bytes,
        "linear_equal_area_model":
            "linear data+metadata+parallel-ECC scaling with fixed "
            "replacement/request logic",
        "provenance": provenance,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Combine measured CACTI/synthesis values for K2.")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--template", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.template:
        print(json.dumps(template(), indent=2, sort_keys=True))
        return 0
    if args.input is None:
        raise SystemExit("--input is required unless --template is used")
    result = characterize(json.loads(args.input.read_text()))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
