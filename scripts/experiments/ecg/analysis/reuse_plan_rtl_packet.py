#!/usr/bin/env python3
"""Emit hashed ReusePlan replacement and SECDED synthesis inputs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from scripts.experiments.ecg.analysis.reuse_plan_cacti_packet import (
    PROJECT_ROOT,
    sha256_file,
    write_json_atomic,
)


RTL_ROOT = PROJECT_ROOT / "bench/src_rtl"
VICTIM_RTL = RTL_ROOT / "reuse_plan_victim_select.sv"
ECC_RTL = RTL_ROOT / "reuse_plan_secded_49.sv"
ONLINE_RTL = RTL_ROOT / "reuse_plan_online_selector.sv"
REPLACEMENT_RTL = RTL_ROOT / "reuse_plan_replacement_path.sv"
REQUEST_RTL = RTL_ROOT / "reuse_bind_request_path.sv"
RECENCY_RTL = RTL_ROOT / "reuse_plan_recency_rank.sv"
TESTBENCH = RTL_ROOT / "tb_reuse_plan_physical_logic.sv"
REPLACEMENT_TESTBENCH = RTL_ROOT / "tb_reuse_plan_replacement_path.sv"
REQUEST_TESTBENCH = RTL_ROOT / "tb_reuse_bind_request_path.sv"
POLICY_SOURCE = PROJECT_ROOT / "bench/include/ecg_victim_policy.h"


def source_entry(path: Path) -> dict[str, str]:
    return {
        "path": str(path.relative_to(PROJECT_ROOT)),
        "sha256": sha256_file(path),
    }


def manifest() -> dict[str, Any]:
    return {
        "version": 1,
        "status": "inputs_only_unmeasured",
        "technology_nm_required": 32,
        "replacement_ranking_subcomponent": {
            "top": "reuse_plan_victim_select",
            "source": source_entry(VICTIM_RTL),
            "policy_source": source_entry(POLICY_SOURCE),
            "parameters": {
                "WAYS": 16,
                "RRPV_BITS": 3,
                "RECENCY_BITS": 4,
                "TIER_BITS": 2,
                "DIST_BITS": 15,
            },
            "variants": {
                "GRASP_ONLY": 0,
                "EPOCH_FIRST": 1,
                "RRIP_FIRST": 2,
                "EPOCH_ONLY": 3,
                "SHORTCIRCUIT": 4,
                "DEGREE_FIRST": 5,
                "LRU_ONLY": 6,
            },
            "scope": (
                "Ranking and RRIP aging only. Final replacement synthesis must "
                "also include ReusePlan distance, context/property "
                "qualification, variant/online selection, and any non-baseline "
                "recency-rank maintenance."),
        },
        "replacement_path": {
            "top": "reuse_plan_replacement_path",
            "sources": [
                source_entry(VICTIM_RTL),
                source_entry(ONLINE_RTL),
                source_entry(REPLACEMENT_RTL),
            ],
            "parameters": {
                "WAYS": 16,
                "ADDR_BITS": 48,
                "EPOCH_REGIONS": 2,
                "EPOCH_BITS": 15,
                "CONTEXT_BITS": 16,
                "RRPV_BITS": 3,
                "RECENCY_BITS": 4,
                "SET_INDEX_BITS": 13,
            },
            "scope": (
                "Static and five-arm online replacement path including "
                "property-region comparisons, context qualification, "
                "two-epoch circular distance, ranking, and winner state. "
                "The two descriptors must be prefiltered to the benchmark's "
                "epoch-governed arrays. EPOCH_BITS=15 fixes the physical point "
                "at 32768 epochs; singleton records repeat epoch1 in epoch2."),
        },
        "ecc": {
            "area_top": "reuse_plan_secded_49_parallel16",
            "read_delay_top": "reuse_plan_secded_49_decode",
            "source": source_entry(ECC_RTL),
            "data_bits_per_way": 49,
            "secded_bits_per_way": 7,
            "ways": 16,
            "area_instances": {
                "encoders": 16,
                "decoders": 16,
            },
        },
        "request_path_units": {
            "machine_wide_counts_status": "parameterized_unfrozen",
            "sources": [
                source_entry(REQUEST_RTL),
                source_entry(RECENCY_RTL),
            ],
            "tops": {
                "mshr_slot": "reuse_bind_request_state_slot",
                "csr_per_hart": "reuse_plan_csr_state",
                "optional_sequence_allocator": "reuse_plan_sequence_allocator",
                "pipeline_copy": "reuse_bind_request_pipeline_stage",
                "optional_recency_rank_per_set": "reuse_plan_recency_rank_state",
            },
            "payload_bits": 95,
            "payload_layout": {
                "tier": "1:0",
                "epoch1": "16:2",
                "epoch2": "31:17",
                "current_epoch": "46:32",
                "context": "62:47",
                "sequence": "94:63",
            },
            "scaling": (
                "Synthesize per-unit tops, then scale by actual MSHR slots, "
                "harts, and request-sideband copies. The baseline MSHR supplies "
                "address match, allocation, and slot arbitration; these tops "
                "measure only incremental ReusePlan state/merge logic. O3 may reuse "
                "existing dynamic-instruction sequence tags; otherwise the "
                "8-lane 32-bit allocator is the pinned fallback. Scale "
                "registered recency state by LLC sets only when the baseline "
                "does not provide age rank."),
        },
        "verification": {
            "testbench": source_entry(TESTBENCH),
            "replacement_testbench": source_entry(REPLACEMENT_TESTBENCH),
            "request_testbench": source_entry(REQUEST_TESTBENCH),
            "commands": [
                "python3 -m "
                "scripts.experiments.ecg.analysis.reuse_plan_rtl_verify",
            ],
        },
        "limitations": [
            "No technology area, power, or delay result is embedded.",
            "Per-unit request-state RTL must be scaled using the target "
            "microarchitecture's actual harts, MSHR slots, and sideband copies.",
            "Pipeline copies include ReusePlan payload flops only; baseline queue "
            "head/tail/occupancy control is not charged to ReusePlan.",
            "The 4-bit recency input is baseline-provided 16-way age rank, not "
            "additional ReusePlan line metadata.",
        ],
    }


def emit(out_dir: Path) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    payload = manifest()
    write_json_atomic(out_dir / "reuse_plan_rtl_manifest.json", payload)
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit hashed ReusePlan synthesis input provenance.")
    parser.add_argument("--out-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    payload = emit(parse_args().out_dir)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
