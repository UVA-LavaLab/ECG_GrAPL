#!/usr/bin/env python3
"""Fail-closed gate for the ReusePlan transport campaign.

This campaign holds replacement at pure LRU in both arms and isolates
compact ReusePlan record transport plus structural FlowThrough. It makes
no replacement-policy claim and never compares against SRRIP, GRASP, or
P-OPT.
"""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
ECG_DIR = ROOT / "scripts" / "experiments" / "ecg"
sys.path.insert(0, str(ECG_DIR))
sys.path.insert(0, str(ECG_DIR / "flows"))

from analysis import (  # noqa: E402
    final_campaign_gate,
    literature_scale_gate,
    pagerank_gate,
    three_costs,
)
from flows import aggregate_results, experiment_run  # noqa: E402
from policy_specs import policy_output_label  # noqa: E402


DEFAULT_MANIFEST = ECG_DIR / "experiment_manifest.json"
DEFAULT_CONFIG = ECG_DIR / "configs/transport_literature_scale.json"
DEFAULT_CORPUS_RECEIPT = (
    ROOT / "results/graphs/literature_scale_corpus.receipt.json")
PROFILE = "reuse_plan_transport_campaign"
MECHANISM_STAGE = "60_gem5_proposal_reuse_bind_o3"
SCREEN_TIMING_STAGE = "96_gem5_transport_i1"
ROBUSTNESS_TIMING_STAGE = "97_gem5_transport_i8"
WIDE_STAGE = "98_cache_sim_transport_wide16"
COMPACT_STAGE = "99_cache_sim_transport_compact16"
SNIPER_STAGE = "100_sniper_transport_matched"
TIMING_STAGES = {SCREEN_TIMING_STAGE, ROBUSTNESS_TIMING_STAGE}
CACHE_STAGES = {WIDE_STAGE, COMPACT_STAGE}
SCREEN_STAGES = {MECHANISM_STAGE, SCREEN_TIMING_STAGE}
FULL_STAGES = {
    ROBUSTNESS_TIMING_STAGE,
    WIDE_STAGE,
    COMPACT_STAGE,
    SNIPER_STAGE,
}
COMPLETE_STAGES = {*SCREEN_STAGES, *FULL_STAGES}
STAGE_ITERATION = {
    SCREEN_TIMING_STAGE: 1,
    ROBUSTNESS_TIMING_STAGE: 8,
}
CACHE_CELL_IDENTITY_FIELDS = (
    "benchmark",
    "options",
    "l1d_size",
    "l1d_ways",
    "l2_size",
    "l2_ways",
    "l3_size",
    "l3_ways",
    "l3_effective_ways",
    "line_size",
    "prefetcher",
    "flowthrough",
    "ecg_epochs_effective",
    "ecg_vertices",
)
CACHE_BASELINE_MATCH_FIELDS = (
    "total_accesses",
    "l1_hits",
    "l1_misses",
    "l2_hits",
    "l2_misses",
    "l3_hits",
    "l3_misses",
    "llc_writebacks",
    "memory_accesses",
    "total_offchip_traffic",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def geomean(values: list[float]) -> float:
    return pagerank_gate.geomean(values)


def policy_labels(config: dict[str, Any]) -> dict[str, str]:
    policies = config["policies"]
    return {
        "baseline": policy_output_label(policies["timing_baseline"]),
        "candidate": policy_output_label(policies["transport_candidate"]),
        "all": [
            policy_output_label(policy) for policy in policies["all"]],
    }


def stages(
        manifest: dict[str, Any],
        selected: set[str]) -> dict[str, dict[str, Any]]:
    result = {
        str(stage["name"]): experiment_run.merged_defaults(manifest, stage)
        for stage in manifest.get("stages", [])
        if PROFILE in stage.get("profiles", []) and
        str(stage["name"]) in selected
    }
    if set(result) != selected:
        raise ValueError(
            f"transport stage roster mismatch: "
            f"expected={sorted(selected)} actual={sorted(result)}")
    return result


def expected_cells(
        manifest: dict[str, Any],
        config: dict[str, Any],
        selected: set[str],
) -> dict[tuple[str, str, str], tuple[str, ...]]:
    expected = {}
    for name, stage in stages(manifest, selected).items():
        if name in TIMING_STAGES:
            graphs = config["graphs"]
            benchmarks = [config["benchmark"]]
            policies = config["policies"]["all"]
        else:
            graphs = manifest["graph_sets"][stage["graph_set"]]
            benchmarks = stage["benchmarks"]
            policies = stage["policies"]
        roster = tuple(policy_output_label(policy) for policy in policies)
        for graph in graphs:
            for benchmark in benchmarks:
                expected[(
                    name, str(graph["name"]), str(benchmark))] = roster
    return expected


def grouped_rows(
        rows: list[dict[str, Any]],
        selected: set[str],
) -> dict[tuple[str, str, str], list[dict[str, Any]]]:
    groups: dict[
        tuple[str, str, str], list[dict[str, Any]]
    ] = defaultdict(list)
    for row in rows:
        stage = str(row.get("final_stage", ""))
        if stage in selected:
            groups[(
                stage,
                str(row.get("final_graph", "")),
                str(row.get("benchmark", "")),
            )].append(row)
    return groups


def validate_rosters(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        expected: dict[tuple[str, str, str], tuple[str, ...]],
) -> list[str]:
    errors = []
    if set(groups) != set(expected):
        missing = sorted(set(expected) - set(groups))
        extra = sorted(set(groups) - set(expected))
        if missing:
            errors.append(f"missing transport cells: {missing}")
        if extra:
            errors.append(f"unexpected transport cells: {extra}")
    for key in sorted(set(groups) & set(expected)):
        actual = tuple(
            str(row.get("policy_label", ""))
            for row in groups[key])
        if actual != expected[key]:
            errors.append(
                f"{key} policy roster mismatch: "
                f"expected={expected[key]} actual={actual}")
    return errors


def validate_rows(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        manifest: dict[str, Any],
        config: dict[str, Any],
) -> list[str]:
    errors = []
    labels = policy_labels(config)
    timing_graphs = pagerank_gate.graph_map(config)
    sniper_graphs = {
        str(graph["name"]): graph
        for graph in manifest["graph_sets"]["literature_scale_sniper"]
    }
    for key, rows in sorted(groups.items()):
        stage, graph, _benchmark = key
        for row in rows:
            policy = str(row.get("policy_label", ""))
            if row.get("status") != "ok":
                errors.append(f"{key}/{policy} status={row.get('status')}")
            if not row.get("final_matrix_config_hash"):
                errors.append(f"{key}/{policy} missing matrix config hash")
            try:
                errors.extend(validate_stage_row(
                    stage, key, row, policy, labels,
                    sniper_graphs.get(graph, {}), config,
                    timing_graphs.get(graph, {})))
            except (KeyError, TypeError, ValueError) as error:
                errors.append(f"{key}/{policy} row is malformed: {error}")
    return errors


def validate_stage_row(
        stage: str, key: tuple[str, str, str], row: dict[str, Any],
        policy: str, labels: dict[str, Any],
        sniper_graph: dict[str, Any], config: dict[str, Any],
        timing_graph: dict[str, Any]) -> list[str]:
    if stage in TIMING_STAGES:
        return validate_timing_row(
            key, row, policy, labels, config, timing_graph)
    if stage == MECHANISM_STAGE:
        if (
                row.get("simulator") != "gem5" or
                str(row.get("timing_valid_for_speedup")) != "0"):
            return [f"{key}/{policy} mechanism row is malformed"]
        return []
    if stage in CACHE_STAGES:
        return validate_cache_row(key, row, policy, labels, stage)
    if stage == SNIPER_STAGE:
        return validate_sniper_row(key, row, policy, labels, sniper_graph)
    return [f"{key}/{policy} is outside the transport stage roster"]


def validate_timing_row(
        key: tuple[str, str, str], row: dict[str, Any], policy: str,
        labels: dict[str, Any], config: dict[str, Any],
        graph: dict[str, Any]) -> list[str]:
    errors = []
    if (
            row.get("simulator") != "gem5" or
            row.get("gem5_cpu_type") != "O3" or
            str(row.get("timing_valid_for_speedup")) != "1"):
        errors.append(f"{key}/{policy} is not valid gem5 O3 timing")
    if (
            str(row.get("gem5_structural_flowthrough_receipt")) != "1" or
            not final_campaign_gate.positive(
                row, "gem5_structural_flowthrough_miss_targets")):
        errors.append(
            f"{key}/{policy} lacks a structural FlowThrough receipt")
    if policy == labels["candidate"]:
        if (
                final_campaign_gate.integer(row, "ecg_record_bytes") != 4 or
                final_campaign_gate.integer(
                    row, "edge_stream_bytes_per_edge") != 4 or
                str(row.get("ecg_record_replaces_edge")) != "1"):
            errors.append(
                f"{key}/{policy} does not substitute a 4-byte record "
                f"for the destination array")
        if (
                str(row.get("gem5_reuse_plan_sidecar_active")) != "1" or
                final_campaign_gate.integer(
                    row, "gem5_reuse_plan_sidecar_record_bytes") != 4 or
                final_campaign_gate.integer(
                    row, "gem5_reuse_plan_sidecar_tier_bits") !=
                int(config["compact_tier_bits"]) or
                final_campaign_gate.integer(
                    row, "gem5_reuse_plan_sidecar_records") !=
                int(graph["directed_edges"])):
            errors.append(f"{key}/{policy} lacks a validated record sidecar")
        if (
                str(row.get("proposal_path_active")) != "1" or
                str(row.get("proposal_performance_mode_active")) != "1" or
                str(row.get(
                    "gem5_compact_reuse_bind_flowthrough_active")) != "1"):
            errors.append(
                f"{key}/{policy} lacks the compact performance path")
        if (
                str(row.get("gem5_reuse_plan_coverage_validated")) != "1" or
                not final_campaign_gate.positive(
                    row, "gem5_reuse_plan_victim_selections") or
                not final_campaign_gate.positive(
                    row, "gem5_reuse_plan_victim_stamped_ways")):
            errors.append(
                f"{key}/{policy} lacks positive ReusePlan stamp coverage")
    elif policy == labels["baseline"]:
        if (
                str(row.get("proposal_path_active")) != "0" or
                final_campaign_gate.integer(
                    row, "ecg_reuse_plan_depth") != 0):
            errors.append(f"{key}/{policy} is not a pure baseline row")
    else:
        errors.append(f"{key}/{policy} is outside the transport roster")
    return errors


def validate_cache_row(
        key: tuple[str, str, str], row: dict[str, Any], policy: str,
        labels: dict[str, Any], stage: str) -> list[str]:
    errors = []
    if (
            row.get("simulator") != "cache_sim" or
            str(row.get("timing_valid_for_speedup")) != "0"):
        errors.append(f"{key}/{policy} cache row is malformed")
    if str(row.get("flowthrough")) != "all" or not final_campaign_gate.positive(
            row, "structural_flowthrough_accesses"):
        errors.append(
            f"{key}/{policy} lacks symmetric structural FlowThrough")
    if final_campaign_gate.integer(row, "ecg_epochs_effective") != 16:
        errors.append(f"{key}/{policy} epoch count mismatch")
    if policy == labels["candidate"]:
        expected_width = 4 if stage == COMPACT_STAGE else 8
        if (
                final_campaign_gate.integer(
                    row, "ecg_record_bytes") != expected_width or
                final_campaign_gate.integer(
                    row, "edge_stream_bytes_per_edge") != expected_width or
                str(row.get("ecg_record_replaces_edge")) != "1"):
            errors.append(
                f"{key}/{policy} record width is not {expected_width} bytes")
        if str(row.get("ecg_variant_effective")) != "lru_only":
            errors.append(
                f"{key}/{policy} does not use the LRU victim variant")
    elif policy != labels["baseline"]:
        errors.append(f"{key}/{policy} is outside the transport roster")
    return errors


def validate_sniper_row(
        key: tuple[str, str, str], row: dict[str, Any], policy: str,
        labels: dict[str, Any], graph_spec: dict[str, Any]) -> list[str]:
    errors = []
    expected_limit = int(graph_spec.get("sniper_semantic_edge_limit", 0))
    if (
            row.get("simulator") != "sniper" or
            str(row.get("timing_valid_for_speedup")) != "0" or
            row.get("sniper_queue_model") != "windowed_mg1" or
            final_campaign_gate.integer(
                row, "sniper_semantic_edge_limit") != expected_limit or
            str(row.get("semantic_work_matched")) != "1"):
        errors.append(f"{key}/{policy} Sniper role is malformed")
    if str(row.get("sniper_structural_flowthrough_receipt")) != "1":
        errors.append(
            f"{key}/{policy} lacks symmetric structural FlowThrough")
    if not final_campaign_gate.positive(row, "l3_misses"):
        errors.append(f"{key}/{policy} lacks LLC turnover")
    if policy == labels["candidate"]:
        if (
                final_campaign_gate.integer(
                    row, "sniper_transport_record_bytes") != 8 or
                final_campaign_gate.integer(
                    row, "edge_stream_bytes_per_edge") != 8):
            errors.append(f"{key}/{policy} is not the 8-byte transport role")
        if str(row.get("ecg_variant_effective")) != "lru_only":
            errors.append(
                f"{key}/{policy} does not use the LRU victim variant")
        if (
                str(row.get(
                    "sniper_transport_receipts_validated")) != "1" or
                str(row.get(
                    "sniper_reuse_plan_epoch_context_validated")) != "1" or
                str(row.get("sniper_reuse_bind_exact_validated")) != "1" or
                final_campaign_gate.integer(
                    row, "sniper_reuse_bind_bad_consumes") != 0):
            errors.append(f"{key}/{policy} fused receipts failed")
    elif policy != labels["baseline"]:
        errors.append(f"{key}/{policy} is outside the transport roster")
    return errors


def timing_cells(
        rows: list[dict[str, Any]],
        config: dict[str, Any],
        stage: str,
) -> dict[str, dict[str, dict[str, Any]]]:
    """Validate and index one timing stage by graph, then policy label."""
    graphs = pagerank_gate.graph_map(config)
    labels = policy_labels(config)
    iteration = STAGE_ITERATION[stage]
    cells: dict[str, dict[str, dict[str, Any]]] = {}
    for row in rows:
        graph_name = str(row.get("final_graph", ""))
        if graph_name not in graphs:
            raise ValueError(f"row outside transport graph set: {graph_name}")
        policy = policy_output_label(str(row.get("policy_label", "")))
        if policy not in labels["all"]:
            raise ValueError(f"unexpected policy {policy} in {graph_name}")
        if pagerank_gate.iteration_from_options(
                str(row.get("options", ""))) != iteration:
            raise ValueError(
                f"{stage} row for {graph_name} does not use iteration "
                f"{iteration}")
        pagerank_gate.validate_common_row(
            row, config, graphs[graph_name], iteration)
        if policy == labels["candidate"]:
            pagerank_gate.validate_reuse_plan(
                row, config, graphs[graph_name], policy)
        else:
            pagerank_gate.validate_baseline(row, graphs[graph_name], policy)
        per_policy = cells.setdefault(graph_name, {})
        if policy in per_policy:
            raise ValueError(f"duplicate policy {policy} for {graph_name}")
        per_policy[policy] = row
    if set(cells) != set(graphs):
        raise ValueError(
            f"incomplete transport timing cells for {stage}: "
            f"{sorted(set(graphs) - set(cells))}")
    for graph_name, per_policy in cells.items():
        if set(per_policy) != set(labels["all"]):
            raise ValueError(
                f"incomplete policy roster for {graph_name}: "
                f"{sorted(per_policy)}")
        if len({
                row.get("final_job_id", "")
                for row in per_policy.values()}) != 1:
            raise ValueError(f"cell {graph_name} mixes job ids")
    return cells


def timing_comparison(
        rows: list[dict[str, Any]],
        config: dict[str, Any],
        stage: str,
        limits: dict[str, float],
) -> dict[str, Any]:
    cells = timing_cells(rows, config, stage)
    labels = policy_labels(config)
    fields = config["metrics"]["fields"]
    entries = []
    for graph_name in sorted(cells):
        candidate = cells[graph_name][labels["candidate"]]
        baseline = cells[graph_name][labels["baseline"]]
        time_ratio = (
            pagerank_gate.number(
                candidate[fields["gem5_time"]], "time") /
            pagerank_gate.number(baseline[fields["gem5_time"]], "time"))
        traffic_ratio = (
            pagerank_gate.number(
                candidate[fields["gem5_traffic"]], "traffic") /
            pagerank_gate.number(
                baseline[fields["gem5_traffic"]], "traffic"))
        entries.append({
            "graph": graph_name,
            "iterations": STAGE_ITERATION[stage],
            "time_ratio": time_ratio,
            "traffic_ratio": traffic_ratio,
        })
    aggregate_time = geomean([entry["time_ratio"] for entry in entries])
    aggregate_traffic = geomean([
        entry["traffic_ratio"] for entry in entries])
    violations = [
        entry for entry in entries
        if entry["time_ratio"] > limits["max_time_ratio_per_cell"] or
        entry["traffic_ratio"] > limits["max_traffic_ratio_per_cell"]
    ]
    passes = bool(
        aggregate_time <= limits["max_time_ratio_aggregate"] and
        aggregate_traffic <= limits["max_traffic_ratio_aggregate"] and
        not violations)
    return {
        "stage": stage,
        "iterations": STAGE_ITERATION[stage],
        "baseline": labels["baseline"],
        "candidate": labels["candidate"],
        "aggregate_time_ratio": aggregate_time,
        "aggregate_traffic_ratio": aggregate_traffic,
        "cells": entries,
        "per_cell_violations": violations,
        "limits": limits,
        "passes": passes,
    }


def compact_wide_comparison(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        config: dict[str, Any],
) -> dict[str, Any]:
    labels = policy_labels(config)
    fields = config["metrics"]["fields"]
    limits = config["decision"]["complete"]
    errors: list[str] = []
    by_stage: dict[str, dict[tuple[str, str, str], dict[str, Any]]] = {
        WIDE_STAGE: {},
        COMPACT_STAGE: {},
    }
    for (stage, graph, benchmark), rows in groups.items():
        if stage not in CACHE_STAGES:
            continue
        for row in rows:
            policy = str(row.get("policy_label", ""))
            by_stage[stage][(graph, benchmark, policy)] = row

    keys = sorted(set(by_stage[WIDE_STAGE]) & set(by_stage[COMPACT_STAGE]))
    unmatched = sorted(
        set(by_stage[WIDE_STAGE]) ^ set(by_stage[COMPACT_STAGE]))
    if unmatched:
        errors.append(f"compact/wide cells are unmatched: {unmatched}")
    entries = []
    for key in keys:
        graph, benchmark, policy = key
        wide = by_stage[WIDE_STAGE][key]
        compact = by_stage[COMPACT_STAGE][key]
        for field in CACHE_CELL_IDENTITY_FIELDS:
            if str(wide.get(field, "")) != str(compact.get(field, "")):
                errors.append(
                    f"{key} compact/wide {field} differs")
        if policy == labels["baseline"]:
            for field in CACHE_BASELINE_MATCH_FIELDS:
                if str(wide.get(field, "")) != str(compact.get(field, "")):
                    errors.append(
                        f"{key} baseline drift across record widths: {field}")
            continue
        if policy != labels["candidate"]:
            errors.append(f"{key} is outside the transport roster")
            continue
        traffic_ratio = (
            pagerank_gate.number(
                compact[fields["cache_traffic"]], "traffic") /
            pagerank_gate.number(wide[fields["cache_traffic"]], "traffic"))
        miss_ratio = (
            pagerank_gate.number(
                compact[fields["cache_llc_misses"]], "l3_misses") /
            pagerank_gate.number(
                wide[fields["cache_llc_misses"]], "l3_misses"))
        entries.append({
            "graph": graph,
            "benchmark": benchmark,
            "policy": policy,
            "traffic_ratio": traffic_ratio,
            "llc_miss_ratio": miss_ratio,
        })
    if not entries:
        errors.append("compact/wide comparison has no candidate cells")
        aggregate = None
        violations: list[dict[str, Any]] = []
        passes = False
    else:
        aggregate = geomean([entry["traffic_ratio"] for entry in entries])
        violations = [
            entry for entry in entries
            if entry["traffic_ratio"] > float(
                limits["max_compact_wide_traffic_ratio_per_cell"]) or
            entry["llc_miss_ratio"] > float(
                limits["max_compact_wide_llc_miss_ratio_per_cell"])
        ]
        passes = bool(
            aggregate <= float(
                limits["max_compact_wide_traffic_ratio_aggregate"]) and
            not violations)
    return {
        "aggregate_traffic_ratio": aggregate,
        "cells": entries,
        "per_cell_violations": violations,
        "errors": errors,
        "passes": passes and not errors,
    }


def sniper_comparison(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        config: dict[str, Any],
) -> dict[str, Any]:
    labels = policy_labels(config)
    fields = config["metrics"]["fields"]
    limits = config["decision"]["complete"]
    errors: list[str] = []
    entries = []
    for (stage, graph, benchmark), rows in sorted(groups.items()):
        if stage != SNIPER_STAGE:
            continue
        by_policy = {
            str(row.get("policy_label", "")): row for row in rows}
        candidate = by_policy.get(labels["candidate"])
        baseline = by_policy.get(labels["baseline"])
        if candidate is None or baseline is None:
            errors.append(f"({graph}, {benchmark}) Sniper cell is incomplete")
            continue
        if str(candidate.get("sniper_semantic_edge_visits", "")) != str(
                baseline.get("sniper_semantic_edge_visits", "")):
            errors.append(
                f"({graph}, {benchmark}) Sniper semantic work is unmatched")
        llc_miss_ratio = (
            pagerank_gate.number(
                candidate[fields["sniper_llc_misses"]],
                "sniper_llc_misses") /
            pagerank_gate.number(
                baseline[fields["sniper_llc_misses"]],
                "sniper_llc_misses"))
        entries.append({
            "graph": graph,
            "benchmark": benchmark,
            "llc_miss_ratio": llc_miss_ratio,
        })
    if not entries:
        errors.append("Sniper corroboration has no cells")
        aggregate = None
        violations: list[dict[str, Any]] = []
        passes = False
    else:
        aggregate = geomean([
            entry["llc_miss_ratio"] for entry in entries])
        violations = [
            entry for entry in entries
            if entry["llc_miss_ratio"] > float(
                limits["max_sniper_llc_miss_ratio_per_cell"])
        ]
        passes = bool(
            aggregate <= float(
                limits["max_sniper_llc_miss_ratio_aggregate"]) and
            not violations)
    return {
        "metric_field": fields["sniper_llc_misses"],
        "timing_admissible": False,
        "aggregate_llc_miss_ratio": aggregate,
        "cells": entries,
        "per_cell_violations": violations,
        "errors": errors,
        "passes": passes and not errors,
    }


def screen_limits(config: dict[str, Any]) -> dict[str, float]:
    decision = config["decision"]["screen"]
    return {
        "max_time_ratio_aggregate": float(decision["max_time_ratio_vs_lru"]),
        "max_traffic_ratio_aggregate": float(
            decision["max_traffic_ratio_vs_lru"]),
        "max_time_ratio_per_cell": float(
            decision["max_time_ratio_per_cell"]),
        "max_traffic_ratio_per_cell": float(
            decision["max_traffic_ratio_per_cell"]),
    }


def iteration8_limits(config: dict[str, Any]) -> dict[str, float]:
    decision = config["decision"]["complete"]
    return {
        "max_time_ratio_aggregate": float(
            decision["max_time_ratio_vs_lru_i8"]),
        "max_traffic_ratio_aggregate": float(
            decision["max_traffic_ratio_vs_lru_i8"]),
        "max_time_ratio_per_cell": float(
            decision["max_time_ratio_per_cell"]),
        "max_traffic_ratio_per_cell": float(
            decision["max_traffic_ratio_per_cell"]),
    }


def stage_rows(
        rows: list[dict[str, Any]], stage: str) -> list[dict[str, Any]]:
    return [row for row in rows if row.get("final_stage") == stage]


def validate_full_role_authorizations(
        run_dirs: list[Path], manifest_path: Path,
        config_path: Path, git_head: str) -> list[str]:
    errors = []
    for run_dir in run_dirs:
        snapshot_path = run_dir / "resolved_manifest.json"
        if not snapshot_path.is_file():
            continue
        try:
            snapshot = json.loads(snapshot_path.read_text())
        except (OSError, json.JSONDecodeError):
            errors.append(f"{run_dir} has unreadable resolved manifest")
            continue
        run_stages = {
            str(job.get("stage", ""))
            for job in snapshot.get("jobs", [])
        }
        if not run_stages.intersection(FULL_STAGES):
            continue
        authorization = snapshot.get("screen_authorization")
        if not isinstance(authorization, dict):
            errors.append(f"{run_dir} lacks transport screen authorization")
            continue
        gate_path = Path(str(authorization.get("path", "")))
        if not gate_path.is_file():
            errors.append(
                f"{run_dir} transport authorization file is missing")
            continue
        try:
            gate = json.loads(gate_path.read_text())
        except (OSError, json.JSONDecodeError):
            errors.append(
                f"{run_dir} transport authorization is unreadable")
            continue
        valid = (
            authorization.get("sha256") == sha256(gate_path) and
            authorization.get("git_head") == git_head and
            authorization.get("manifest_sha256") == sha256(manifest_path) and
            authorization.get("screen_config_sha256") ==
                sha256(config_path) and
            gate.get("valid") is True and
            gate.get("phase") == "screen" and
            gate.get("campaign") == PROFILE and
            gate.get("decision") == "GO")
        if not valid:
            errors.append(f"{run_dir} transport authorization is stale")
    return errors


def validate_transport_corpus(
        manifest: dict[str, Any], config: dict[str, Any],
        receipt_path: Path) -> list[str]:
    errors = literature_scale_gate.validate_corpus(
        manifest, receipt_path)
    try:
        receipt = json.loads(receipt_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return [
            *errors,
            f"transport corpus receipt is invalid: {error}",
        ]
    received = {
        str(graph["name"]): graph
        for graph in receipt.get("graphs", [])
    }
    timing_samples = {
        str(graph.get("timing_sample_name", "")): graph
        for graph in received.values()
        if graph.get("timing_sample_name")
    }
    for graph in config["graphs"]:
        name = str(graph["name"])
        source = timing_samples.get(name)
        if source is None:
            errors.append(f"{name} is absent from the corpus receipt")
            continue
        path = ROOT / str(graph["path"])
        if str(source.get("timing_sample_dbg_sg", "")) != str(
                path.relative_to(ROOT)):
            errors.append(f"{name} timing path differs from corpus receipt")
        elif not path.is_file() or source.get(
                "timing_sample_dbg_sha256") != sha256(path):
            errors.append(f"{name} timing graph hash mismatch")
        if (
                int(source.get("timing_sample_dbg_vertices", -1)) !=
                int(graph["vertices"]) or
                int(source.get("timing_sample_dbg_edges", -1)) !=
                int(graph["directed_edges"]) or
                source.get("timing_semantic_receipts") !=
                graph["semantic_receipts"]):
            errors.append(f"{name} timing receipt metadata mismatch")

    record = config["record_format"]
    eligible = set(record["full_graph_compact_eligible_graphs"])
    compact_graphs = {
        str(graph["name"])
        for graph in manifest["graph_sets"]["literature_scale_compact16"]
    }
    if compact_graphs != eligible:
        errors.append(
            "compact-eligible graph roster differs from the manifest: "
            f"config={sorted(eligible)} manifest={sorted(compact_graphs)}")
    exclusions = set(record["full_graph_compact_exclusions"])
    core_graphs = {
        str(graph["name"])
        for graph in manifest["graph_sets"]["literature_scale_core_8mb"]
    }
    if eligible | exclusions != core_graphs or eligible & exclusions:
        errors.append(
            "compact eligibility and exclusions do not partition the "
            "full-graph corpus")
    tier_bits = int(record["compact_tier_bits"])
    epoch_bits = int(record["compact_epoch_bits"])
    for name in sorted(core_graphs):
        source = received.get(name)
        if source is None:
            errors.append(f"{name} full graph is absent from corpus receipt")
            continue
        vertices = int(source.get("dbg_serialized_vertices", 0))
        id_bits = max(1, (vertices - 1).bit_length())
        total_bits = id_bits + tier_bits + 2 * epoch_bits
        if name in eligible and total_bits > 32:
            errors.append(
                f"{name} is declared compact but needs {total_bits} bits")
        if name in exclusions and total_bits <= 32:
            errors.append(
                f"{name} is excluded despite a {total_bits}-bit record")

    for graph in manifest["graph_sets"]["literature_scale_sniper"]:
        name = str(graph["name"])
        source = received.get(name)
        if source is None:
            errors.append(f"{name} Sniper graph is absent from corpus receipt")
            continue
        serialized_edges = int(source.get("dbg_serialized_edges", -1))
        semantic_limit = int(graph["sniper_semantic_edge_limit"])
        if semantic_limit <= 0 or semantic_limit > serialized_edges:
            errors.append(
                f"{name} Sniper semantic limit exceeds the corpus")
        path = ROOT / str(graph["path"])
        try:
            info = three_costs.read_sg(path, name)
        except (OSError, ValueError) as error:
            errors.append(f"{name} Sniper graph is invalid: {error}")
            continue
        if (
                info.directed or
                info.serialized_edges != serialized_edges):
            errors.append(f"{name} Sniper graph header mismatch")
    return errors


def evaluate(
        rows: list[dict[str, Any]],
        manifest: dict[str, Any],
        config: dict[str, Any],
        run_dirs: list[Path],
        phase: str,
        corpus_receipt: Path = DEFAULT_CORPUS_RECEIPT,
) -> dict[str, Any]:
    selected = SCREEN_STAGES if phase == "screen" else COMPLETE_STAGES
    groups = grouped_rows(rows, selected)
    errors: list[str] = []
    try:
        errors.extend(validate_rosters(
            groups, expected_cells(manifest, config, selected)))
    except (KeyError, ValueError) as error:
        errors.append(f"transport stage definitions are invalid: {error}")
    errors.extend(validate_rows(groups, manifest, config))
    errors.extend(final_campaign_gate.validate_run_preflight(run_dirs))

    expected_timing_rows = len(config["graphs"]) * len(
        config["policies"]["all"])
    screen_timing = None
    iteration8_timing = None
    timing_stage_names = (
        [SCREEN_TIMING_STAGE] if phase == "screen"
        else [SCREEN_TIMING_STAGE, ROBUSTNESS_TIMING_STAGE])
    results: dict[str, dict[str, Any] | None] = {}
    for stage in timing_stage_names:
        selected_rows = stage_rows(rows, stage)
        if len(selected_rows) != expected_timing_rows:
            errors.append(
                f"{stage} timing rows incomplete: "
                f"expected={expected_timing_rows} "
                f"actual={len(selected_rows)}")
            results[stage] = None
            continue
        limits = (
            screen_limits(config) if stage == SCREEN_TIMING_STAGE
            else iteration8_limits(config))
        try:
            results[stage] = timing_comparison(
                selected_rows, config, stage, limits)
        except ValueError as error:
            errors.append(f"{stage} timing gate failed: {error}")
            results[stage] = None
    screen_timing = results.get(SCREEN_TIMING_STAGE)
    iteration8_timing = results.get(ROBUSTNESS_TIMING_STAGE)

    compact_wide = None
    sniper = None
    if phase == "complete":
        errors.extend(validate_transport_corpus(
            manifest, config, corpus_receipt))
        compact_wide = compact_wide_comparison(groups, config)
        errors.extend(compact_wide["errors"])
        sniper = sniper_comparison(groups, config)
        errors.extend(sniper["errors"])

    valid = not errors
    if phase == "screen":
        passes = bool(screen_timing and screen_timing["passes"])
        decision = "INVALID" if not valid else "GO" if passes else "STOP"
    else:
        passes = bool(
            screen_timing and screen_timing["passes"] and
            iteration8_timing and iteration8_timing["passes"] and
            compact_wide and compact_wide["passes"] and
            sniper and sniper["passes"])
        decision = "INVALID" if not valid else "PASS" if passes else "FAIL"
    return {
        "campaign": PROFILE,
        "config_id": config["id"],
        "valid": valid,
        "phase": phase,
        "decision": decision,
        "errors": errors,
        "run_dirs": [str(path) for path in run_dirs],
        "cell_count": len(groups),
        "row_count": sum(len(group) for group in groups.values()),
        "stage_rows": {
            stage: sum(
                len(group) for key, group in groups.items()
                if key[0] == stage)
            for stage in sorted(selected)
        },
        "screen_timing": screen_timing,
        "iteration_8_timing": iteration8_timing,
        "compact_vs_wide": compact_wide,
        "sniper_llc_misses": sniper,
        "thresholds": config["decision"],
        "mechanism_stage_timing_admissible": False,
        "sniper_timing_admissible": False,
        "replacement_claim_allowed": False,
        "full_roles_authorized": bool(
            phase == "screen" and valid and decision == "GO"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-run-dirs", nargs="+", required=True)
    parser.add_argument(
        "--phase", choices=("screen", "complete"), default="complete")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--transport-config", default=str(DEFAULT_CONFIG))
    parser.add_argument(
        "--corpus-receipt", default=str(DEFAULT_CORPUS_RECEIPT))
    parser.add_argument("--output", default="")
    args = parser.parse_args(argv)

    run_dirs = final_campaign_gate.discover_run_dirs([
        experiment_run.resolve_path(path)
        for path in args.input_run_dirs
    ])
    with redirect_stdout(sys.stderr):
        rows, _ = aggregate_results.collect_csvs(run_dirs, [])
    manifest = json.loads(Path(args.manifest).read_text())
    config = json.loads(Path(args.transport_config).read_text())
    corpus_receipt = Path(args.corpus_receipt)
    result = evaluate(
        rows, manifest, config, run_dirs, args.phase, corpus_receipt)
    git_head = subprocess.run(
        ["/usr/bin/git", "rev-parse", "HEAD"],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    result.update({
        "git_head": git_head,
        "manifest_sha256": sha256(Path(args.manifest)),
        "screen_config_sha256": sha256(Path(args.transport_config)),
        "corpus_receipt_sha256": (
            sha256(corpus_receipt)
            if corpus_receipt.is_file() else ""),
    })
    if args.phase == "complete":
        result["errors"].extend(validate_full_role_authorizations(
            run_dirs, Path(args.manifest),
            Path(args.transport_config), git_head))
        if result["errors"]:
            result["valid"] = False
            result["decision"] = "INVALID"
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text, end="")
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
