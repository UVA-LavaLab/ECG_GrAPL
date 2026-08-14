#!/usr/bin/env python3
"""Emit and optionally run reproducible CACTI inputs for ReusePlan SRAM costs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import shutil
import subprocess
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
CACTI_ROOT = (
    PROJECT_ROOT / "bench/include/gem5_sim/gem5/ext/mcpat/cacti")
BASE_CONFIG = CACTI_ROOT / "cache.cfg"
CACTI_README = CACTI_ROOT / "README"

LLC_LINES = (8 * 1024 * 1024) // 64
LLC_WAYS = 16
LLC_SETS = LLC_LINES // LLC_WAYS
REUSE_PLAN_LOGICAL_LINE_BITS = 49
REUSE_PLAN_SECDED_BITS = 7
REUSE_PLAN_PROTECTED_LINE_BITS = REUSE_PLAN_LOGICAL_LINE_BITS + REUSE_PLAN_SECDED_BITS
REUSE_PLAN_MACRO_LINE_BITS = 64
REUSE_PLAN_SET_ROW_BITS = LLC_WAYS * REUSE_PLAN_MACRO_LINE_BITS
REUSE_PLAN_METADATA_BYTES = LLC_LINES * (REUSE_PLAN_MACRO_LINE_BITS // 8)

COMMON_DIRECTIVES = {
    "-UCA bank count": "1",
    "-technology (u)": "0.032",
    "-operating temperature (K)": "360",
    "-Cache model (NUCA, UCA)  -": '"UCA"',
    "-Print level (DETAILED, CONCISE) -": '"DETAILED"',
    "-Print input parameters -": '"true"',
}

PROFILES = {
    "baseline_llc_8mib_16way": {
        **COMMON_DIRECTIVES,
        "-size (bytes)": str(8 * 1024 * 1024),
        "-block size (bytes)": "64",
        "-associativity": "16",
        "-read-write port": "1",
        "-exclusive read port": "0",
        "-exclusive write port": "0",
        "-single ended read ports": "0",
        "-output/input bus width": "512",
        "-cache type": '"cache"',
        "-Add ECC -": '"true"',
    },
    "reuse_plan_metadata_1rw": {
        **COMMON_DIRECTIVES,
        "-size (bytes)": str(REUSE_PLAN_METADATA_BYTES),
        "-block size (bytes)": str(REUSE_PLAN_SET_ROW_BITS // 8),
        "-associativity": "1",
        "-read-write port": "1",
        "-exclusive read port": "0",
        "-exclusive write port": "0",
        "-single ended read ports": "0",
        "-output/input bus width": str(REUSE_PLAN_SET_ROW_BITS),
        "-cache type": '"ram"',
        # The 64-bit macro row already includes 49 data, 7 SECDED, and 8 pad bits.
        "-Add ECC -": '"false"',
    },
    "reuse_plan_metadata_1r1w": {
        **COMMON_DIRECTIVES,
        "-size (bytes)": str(REUSE_PLAN_METADATA_BYTES),
        "-block size (bytes)": str(REUSE_PLAN_SET_ROW_BITS // 8),
        "-associativity": "1",
        "-read-write port": "0",
        "-exclusive read port": "1",
        "-exclusive write port": "1",
        "-single ended read ports": "0",
        "-output/input bus width": str(REUSE_PLAN_SET_ROW_BITS),
        "-cache type": '"ram"',
        "-Add ECC -": '"false"',
    },
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_tree(root: Path) -> str:
    digest = hashlib.sha256()
    inputs = sorted(
        path for path in root.iterdir()
        if path.is_file() and
        (path.suffix in {".cc", ".h", ".mk"} or
         path.name in {"README", "makefile"}))
    for path in inputs:
        digest.update(path.name.encode())
        digest.update(sha256_file(path).encode())
    return digest.hexdigest()


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    temp.replace(path)


def cacti_version() -> str:
    for line in CACTI_README.read_text().splitlines():
        if line.startswith("Version ") and "new c++ code base" in line:
            return line.split(" has ", 1)[0]
    raise RuntimeError("unable to identify vendored CACTI version")


def render_config(directives: dict[str, str]) -> str:
    lines = BASE_CONFIG.read_text().splitlines()
    replaced = {key: 0 for key in directives}
    output: list[str] = []
    ordered_keys = sorted(directives, key=len, reverse=True)
    for line in lines:
        stripped = line.strip()
        if stripped.startswith(("//", "#")):
            output.append(line)
            continue
        matched = next(
            (key for key in ordered_keys if stripped.startswith(key)), None)
        if matched is None:
            output.append(line)
            continue
        output.append(f"{matched} {directives[matched]}")
        replaced[matched] += 1
    missing = [key for key, count in replaced.items() if count != 1]
    if missing:
        raise RuntimeError(
            "CACTI template directives were not uniquely replaced: "
            + ", ".join(missing))
    return "\n".join(output) + "\n"


def parse_cacti_csv(path: Path) -> dict[str, float | int]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle, skipinitialspace=True)
        rows = [
            {str(key).strip(): str(value).strip()
             for key, value in row.items() if key is not None}
            for row in reader
        ]
    if not rows:
        raise ValueError(f"CACTI CSV has no measurement rows: {path}")
    row = rows[-1]
    fields = {
        "area_mm2": "Area (mm2)",
        "read_energy_nj": "Dynamic read energy (nJ)",
        "write_energy_nj": "Dynamic write energy (nJ)",
        "leakage_mw": "Standby leakage per bank(mW)",
        "delay_ns": "Access time (ns)",
    }
    result: dict[str, float | int] = {}
    for target, source in fields.items():
        value = row.get(source)
        try:
            result[target] = float(value) if value is not None else float("nan")
        except ValueError as exc:
            raise ValueError(
                f"invalid CACTI metric {source!r}: {value!r}") from exc
        if not math.isfinite(float(result[target])) or result[target] < 0:
            raise ValueError(f"missing or negative CACTI metric: {source}")
    integer_fields = {
        "technology_nm": "Tech node (nm)",
        "capacity_bytes": "Capacity (bytes)",
        "banks": "Number of banks",
        "associativity": "Associativity",
        "output_width_bits": "Output width (bits)",
    }
    for target, source in integer_fields.items():
        value = row.get(source)
        try:
            number = float(value) if value is not None else float("nan")
        except ValueError as exc:
            raise ValueError(
                f"invalid CACTI geometry {source!r}: {value!r}") from exc
        if not math.isfinite(number) or not number.is_integer() or number < 0:
            raise ValueError(f"missing or invalid CACTI geometry: {source}")
        result[target] = int(number)
    return result


def validate_cacti_profile(
        name: str, measurement: dict[str, float | int],
        directives: dict[str, str]) -> None:
    expected = {
        "technology_nm": round(float(directives["-technology (u)"]) * 1000),
        "capacity_bytes": int(directives["-size (bytes)"]),
        "banks": int(directives["-UCA bank count"]),
        "associativity": int(directives["-associativity"]),
        "output_width_bits": int(directives["-output/input bus width"]),
    }
    mismatches = [
        f"{field}={measurement.get(field)} expected={value}"
        for field, value in expected.items()
        if measurement.get(field) != value
    ]
    if mismatches:
        raise ValueError(
            f"CACTI report geometry mismatch for {name}: "
            + ", ".join(mismatches))


def emit_packet(out_dir: Path) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    configs: dict[str, Any] = {}
    for name, directives in PROFILES.items():
        path = out_dir / f"{name}.cfg"
        path.write_text(render_config(directives))
        configs[name] = {
            "path": path.name,
            "sha256": sha256_file(path),
            "directives": directives,
        }

    manifest = {
        "version": 1,
        "status": "inputs_only_unmeasured",
        "cacti": {
            "version": cacti_version(),
            "source_path": str(CACTI_ROOT.relative_to(PROJECT_ROOT)),
            "source_sha256": sha256_tree(CACTI_ROOT),
            "template_path": str(BASE_CONFIG.relative_to(PROJECT_ROOT)),
            "template_sha256": sha256_file(BASE_CONFIG),
            "build_command": (
                "make -C bench/include/gem5_sim/gem5/ext/mcpat/cacti "
                "CXX=g++ CC=gcc"),
            "run_command": "cacti -infile <config>",
        },
        "technology_nm": 32,
        "temperature_k": 360,
        "baseline": {
            "cache_bytes": 8 * 1024 * 1024,
            "line_bytes": 64,
            "ways": 16,
            "banks": 1,
            "port_model": "1rw",
            "cacti_ecc_storage": True,
        },
        "metadata": {
            "sets": LLC_SETS,
            "ways": LLC_WAYS,
            "logical_bits_per_entry": REUSE_PLAN_LOGICAL_LINE_BITS,
            "secded_bits_per_entry": REUSE_PLAN_SECDED_BITS,
            "protected_bits_per_entry": REUSE_PLAN_PROTECTED_LINE_BITS,
            "macro_bits_per_entry": REUSE_PLAN_MACRO_LINE_BITS,
            "set_row_bits": REUSE_PLAN_SET_ROW_BITS,
            "padding_bits_per_entry": (
                REUSE_PLAN_MACRO_LINE_BITS - REUSE_PLAN_PROTECTED_LINE_BITS),
            "macro_bytes": REUSE_PLAN_METADATA_BYTES,
            "primary_port_model": "1rw",
            "port_sensitivity": "1r1w",
            "cacti_ecc_storage": False,
            "ecc_note": (
                "The 64-bit macro row explicitly includes seven SECDED bits; "
                "CACTI ECC is disabled to avoid double charging storage."),
            "organization_note": (
                "Each CACTI row contains all 16 ways in one 1024-bit set row "
                "so miss victim selection can inspect every way in parallel."),
        },
        "limitations": [
            "CACTI 6.5 requires power-of-two associativity, so 14-way and "
            "15-way LLCs are not represented by these inputs.",
            "CACTI models SRAM storage but not SECDED encoder/decoder logic.",
            "The wide-row model charges full-set read/write energy and does not "
            "model per-way write masking.",
            "No physical result is valid until reports and hashes are recorded.",
        ],
        "configs": configs,
    }
    write_json_atomic(out_dir / "manifest.json", manifest)
    return manifest


def physical_input_partial(
        manifest: dict[str, Any], payload: dict[str, Any],
        metadata_name: str = "reuse_plan_metadata_1rw") -> dict[str, Any]:
    from scripts.experiments.ecg.analysis.reuse_plan_physical import template

    measurements = payload["measurements"]
    baseline_name = "baseline_llc_8mib_16way"
    port_models = {
        "reuse_plan_metadata_1rw": "1rw",
        "reuse_plan_metadata_1r1w": "1r1w",
    }
    if metadata_name not in port_models:
        raise ValueError(f"unsupported metadata profile: {metadata_name}")
    result = template()
    result.update({
        "technology_nm": manifest["technology_nm"],
        "cache_bytes": manifest["baseline"]["cache_bytes"],
        "baseline_ways": manifest["baseline"]["ways"],
        "metadata_port_model": port_models[metadata_name],
        "baseline_cache": {
            key: value for key, value in measurements[baseline_name].items()
            if key in {
                "area_mm2", "read_energy_nj", "write_energy_nj",
                "leakage_mw", "delay_ns",
            }
        },
        "reuse_plan_metadata_sram": {
            key: value for key, value in measurements[metadata_name].items()
            if key in {
                "area_mm2", "read_energy_nj", "write_energy_nj",
                "leakage_mw", "delay_ns",
            }
        },
    })
    result["provenance"].update({
        "cacti_version": manifest["cacti"]["version"],
        "cacti_source_sha256": manifest["cacti"]["source_sha256"],
        "cacti_binary_sha256": payload["cacti_binary_sha256"],
        "cacti_packet_manifest_sha256": payload["manifest_sha256"],
        "baseline_config_sha256":
            measurements[baseline_name]["config_sha256"],
        "baseline_report_sha256":
            measurements[baseline_name]["report_sha256"],
        "metadata_config_sha256":
            measurements[metadata_name]["config_sha256"],
        "metadata_report_sha256":
            measurements[metadata_name]["report_sha256"],
    })
    return result


def run_packet(out_dir: Path, cacti_binary: Path) -> dict[str, Any]:
    if not cacti_binary.is_file():
        raise ValueError(f"CACTI binary does not exist: {cacti_binary}")
    for name in (
            "cacti_measurements.json",
            "physical_input.1rw.partial.json",
            "physical_input.1r1w.partial.json"):
        (out_dir / name).unlink(missing_ok=True)
    manifest = emit_packet(out_dir)
    measurements: dict[str, Any] = {}
    for name, config in manifest["configs"].items():
        run_dir = out_dir / f"{name}_run"
        run_dir.mkdir(exist_ok=True)
        report = run_dir / "out.csv"
        report.unlink(missing_ok=True)
        result = subprocess.run(
            [str(cacti_binary.resolve()), "-infile",
             str((out_dir / config["path"]).resolve())],
            cwd=run_dir, capture_output=True, text=True)
        (run_dir / "stdout.log").write_text(
            result.stdout + result.stderr)
        if result.returncode != 0 or not report.exists():
            raise RuntimeError(
                f"CACTI failed for {name}; see {run_dir / 'stdout.log'}")
        parsed = parse_cacti_csv(report)
        validate_cacti_profile(name, parsed, config["directives"])
        measurements[name] = {
            **parsed,
            "config_sha256": config["sha256"],
            "report_sha256": sha256_file(report),
            "stdout_sha256": sha256_file(run_dir / "stdout.log"),
        }
    payload = {
        "status": "measured_cacti_only",
        "manifest_sha256": sha256_file(out_dir / "manifest.json"),
        "cacti_binary_sha256": sha256_file(cacti_binary),
        "measurements": measurements,
    }
    write_json_atomic(out_dir / "cacti_measurements.json", payload)
    for metadata_name, suffix in (
            ("reuse_plan_metadata_1rw", "1rw"),
            ("reuse_plan_metadata_1r1w", "1r1w")):
        partial = physical_input_partial(
            manifest, payload, metadata_name=metadata_name)
        write_json_atomic(
            out_dir / f"physical_input.{suffix}.partial.json", partial)
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit reproducible CACTI inputs for ReusePlan physical costs.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--cacti-binary", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.run:
        if args.cacti_binary is None:
            raise SystemExit("--cacti-binary is required with --run")
        result = run_packet(args.out_dir, args.cacti_binary)
    else:
        result = emit_packet(args.out_dir)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
