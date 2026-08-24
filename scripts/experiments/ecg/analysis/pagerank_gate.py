#!/usr/bin/env python3
"""Evaluate the deterministic ReusePlan PageRank study."""

from __future__ import annotations

import argparse
import csv
import json
import math
import shlex
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
ECG_DIR = ROOT / "scripts" / "experiments" / "ecg"
sys.path.insert(0, str(ECG_DIR))

from policy_specs import (  # noqa: E402
    ONLINE_DUELING_REPORTED_FIELDS,
    ONLINE_DUELING_WINDOW_MISSES,
    policy_output_label,
)


DEFAULT_CONFIG = (
    ROOT / "scripts" / "experiments" / "ecg" / "configs" /
    "pagerank_study.json")


def number(value: Any, field: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid numeric field {field}: {value!r}") from error
    if not math.isfinite(result):
        raise ValueError(f"non-finite numeric field {field}: {value!r}")
    return result


def integer(value: Any, field: str) -> int:
    result = number(value, field)
    if result != int(result):
        raise ValueError(f"non-integral field {field}: {value!r}")
    return int(result)


def geomean(values: list[float]) -> float:
    if not values or any(value <= 0 for value in values):
        raise ValueError("geomean requires positive values")
    return math.exp(sum(math.log(value) for value in values) / len(values))


def iteration_from_options(options: str) -> int:
    tokens = shlex.split(options)
    for index, token in enumerate(tokens):
        if token == "-i" and index + 1 < len(tokens):
            return int(tokens[index + 1])
        if token.startswith("-i") and token[2:].isdigit():
            return int(token[2:])
    raise ValueError(f"PageRank iteration missing from options: {options!r}")


def canonical_policy(value: str) -> str:
    return policy_output_label(value)


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def require_text(row: dict[str, Any], field: str, expected: Any) -> None:
    if str(row.get(field, "")).lower() != str(expected).lower():
        raise ValueError(
            f"{canonical_policy(str(row.get('policy_label', '')))} "
            f"failed {field}: {row.get(field)!r} != {expected!r}")


def require_positive(row: dict[str, Any], field: str) -> None:
    if number(row.get(field), field) <= 0:
        raise ValueError(f"{field} must be positive")


def require_fields(
        row: dict[str, Any], fields: tuple[str, ...], scope: str) -> None:
    missing = [field for field in fields if row.get(field) in (None, "")]
    if missing:
        raise ValueError(f"{scope} row is missing fields: {missing}")


def require_size(row: dict[str, Any], field: str, expected: Any) -> None:
    actual = str(row.get(field, ""))
    if parse_size(actual) != parse_size(str(expected)):
        raise ValueError(
            f"{canonical_policy(str(row.get('policy_label', '')))} "
            f"failed {field}: {actual!r} != {expected!r}")


def graph_map(config: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {str(graph["name"]): graph for graph in config["graphs"]}


def policy_roles(config: dict[str, Any]) -> dict[str, Any]:
    policies = config["policies"]
    return {
        "all": [canonical_policy(value) for value in policies["all"]],
        "sanity": canonical_policy(policies["sanity_baseline"]),
        "serious": [
            canonical_policy(value)
            for value in policies["serious_baselines"]
        ],
        "popt": canonical_policy(policies["charged_popt_baseline"]),
        "oracle": canonical_policy(policies["oracle_control"]),
        "transport": canonical_policy(policies["transport_control"]),
        "primary": canonical_policy(policies["primary_candidate"]),
        "characterization": [
            canonical_policy(value)
            for value in policies["characterization_only"]
        ],
    }


def validate_common_row(
        row: dict[str, Any], config: dict[str, Any],
        graph: dict[str, Any], iteration: int) -> None:
    require_fields(
        row,
        (
            "status", "final_output_status", "timing_valid_for_speedup",
            "simulator", "gem5_cpu_type", "benchmark", "prefetcher",
            "pr_result_matched", "pr_result_group_rows_ok", "l3_exercised",
            "l1d_size", "l1d_ways", "l2_size", "l2_ways",
            "l3_size", "l3_ways", "line_size",
            "gem5_l3_size_actual", "gem5_l3_ways_actual",
            "final_job_id", "options", "pr_iterations", "pr_semantic_edges",
            "pr_score_checksum", *config["metrics"]["primary"],
            *config["metrics"]["secondary"],
        ),
        "common")
    for field, expected in (
            ("status", "ok"),
            ("final_output_status", "ok"),
            ("timing_valid_for_speedup", "1"),
            ("simulator", config["simulator"]),
            ("gem5_cpu_type", config["cpu_type"]),
            ("benchmark", config["benchmark"]),
            ("prefetcher", config["prefetcher"]),
            ("pr_result_matched", "1"),
            ("pr_result_group_rows_ok", "1"),
            ("l3_exercised", "True")):
        require_text(row, field, expected)

    for field, expected in (
            ("l1d_size", graph["l1d_size"]),
            ("l1d_ways", graph["l1d_ways"]),
            ("l2_size", graph["l2_size"]),
            ("l2_ways", graph["l2_ways"]),
            ("l3_size", graph["l3_size"]),
            ("l3_ways", graph["l3_ways"]),
            ("line_size", graph["line_size"])):
        require_text(row, field, expected)

    expected_options = config["options_template"].format(
        graph_path=str((ROOT / graph["path"]).resolve()),
        iterations=iteration)
    actual_tokens = shlex.split(str(row.get("options", "")))
    expected_tokens = shlex.split(expected_options)
    if (
            len(actual_tokens) != len(expected_tokens) or
            actual_tokens[0] != "-f"):
        raise ValueError("row does not use the frozen PageRank argv")
    actual_graph = Path(actual_tokens[1])
    expected_graph = Path(graph["path"])
    if (
            len(actual_graph.parts) < len(expected_graph.parts) or
            tuple(actual_graph.parts[-len(expected_graph.parts):]) !=
            tuple(expected_graph.parts)):
        raise ValueError("row uses the wrong graph file")
    if actual_tokens[2:] != expected_tokens[2:]:
        raise ValueError("row does not use the frozen PageRank argv")

    receipt = graph["semantic_receipts"][str(iteration)]
    if integer(row.get("pr_iterations"), "pr_iterations") != iteration:
        raise ValueError("executed PageRank iterations differ from requested")
    if integer(row.get("pr_semantic_edges"), "pr_semantic_edges") != int(
            receipt["edges"]):
        raise ValueError("PageRank semantic edge count mismatch")
    require_text(row, "pr_score_checksum", receipt["checksum"])

    for metric in config["metrics"]["primary"]:
        require_positive(row, metric)
    for metric in config["metrics"]["secondary"]:
        value = number(row.get(metric), metric)
        if value < 0 or (metric in {"roi_insts", "ipc"} and value <= 0):
            raise ValueError(f"invalid secondary metric {metric}")


def expected_popt(config: dict[str, Any], graph: dict[str, Any],
                  iteration: int) -> dict[str, int]:
    model = config["popt_model"]
    line_size = int(graph["line_size"])
    if int(graph["vertices"]) * int(model["property_bytes"]) % line_size:
        raise ValueError(
            f"{graph['name']} P-OPT column is not line-aligned")
    if parse_size(graph["l3_size"]) % (
            int(graph["l3_ways"]) * line_size):
        raise ValueError(
            f"{graph['name']} LLC geometry is not set-aligned")
    lines = (
        int(graph["vertices"]) * int(model["property_bytes"]) +
        line_size - 1) // line_size
    column_bytes = lines
    matrix_bytes = int(model["reserved_column_slots"]) * column_bytes
    bytes_per_way = (
        parse_size(graph["l3_size"]) //
        int(graph["l3_ways"]))
    reserved_ways = (matrix_bytes + bytes_per_way - 1) // bytes_per_way
    if reserved_ways != int(model["expected_reserved_ways_in_screen"]):
        raise ValueError(
            f"{graph['name']} P-OPT reservation differs from the screen")
    effective_ways = int(graph["l3_ways"]) - reserved_ways
    if effective_ways != int(
            model["expected_effective_data_ways_in_screen"]):
        raise ValueError(
            f"{graph['name']} P-OPT effective ways differ from the screen")
    stream_bytes_per_iteration = int(model["epochs"]) * column_bytes
    stream_requests_per_iteration = (
        stream_bytes_per_iteration + line_size - 1) // line_size
    stream_requests = stream_requests_per_iteration * iteration
    cumulative_stream_bytes = stream_requests * line_size
    return {
        "matrix_column_bytes": column_bytes,
        "matrix_bytes": matrix_bytes,
        "reserved_ways": reserved_ways,
        "effective_ways": effective_ways,
        "stream_bytes_per_iteration": stream_bytes_per_iteration,
        "cumulative_stream_bytes": cumulative_stream_bytes,
        "stream_requests": stream_requests,
    }


def parse_size(value: str) -> int:
    text = value.strip().lower()
    units = {"gb": 1024 ** 3, "mb": 1024 ** 2, "kb": 1024, "b": 1}
    for unit, multiplier in units.items():
        if text.endswith(unit):
            return int(text[:-len(unit)]) * multiplier
    return int(text)


def validate_grasp(row: dict[str, Any]) -> None:
    require_fields(
        row,
        (
            "grasp_context_loaded", "grasp_regions_loaded",
            "grasp_hot_property_accesses",
        ),
        "GRASP")
    require_text(row, "grasp_context_loaded", "1")
    require_positive(row, "grasp_regions_loaded")
    require_positive(row, "grasp_hot_property_accesses")


def validate_baseline(
        row: dict[str, Any], graph: dict[str, Any], policy: str) -> None:
    require_fields(
        row,
        (
            "proposal_path_active", "ecg_reuse_plan_depth", "graph_edge_bytes",
            "edge_stream_bytes_per_edge", "ecg_record_replaces_edge",
            "popt_reserved_ways", "popt_effective_l3_ways",
            "popt_effective_l3_size", "l3_effective_ways",
            "l3_effective_size",
        ),
        policy)
    for field, value in (
            ("proposal_path_active", "0"),
            ("ecg_reuse_plan_depth", "0"),
            ("graph_edge_bytes", "4"),
            ("edge_stream_bytes_per_edge", "4"),
            ("ecg_record_replaces_edge", "0")):
        require_text(row, field, value)
    if policy != "POPT":
        require_text(row, "popt_reserved_ways", "0")
        require_text(row, "popt_effective_l3_ways", graph["l3_ways"])
        require_size(
            row, "popt_effective_l3_size", graph["l3_size"])
        require_text(row, "l3_effective_ways", graph["l3_ways"])
        require_size(row, "l3_effective_size", graph["l3_size"])
        require_text(row, "gem5_l3_ways_actual", graph["l3_ways"])
        require_size(row, "gem5_l3_size_actual", graph["l3_size"])


def validate_popt(row: dict[str, Any], config: dict[str, Any],
                  graph: dict[str, Any], iteration: int) -> None:
    require_fields(
        row,
        (
            "popt_overhead_charged", "popt_reserve_model",
            "popt_matrix_fits", "popt_property_bytes",
            "popt_matrix_active_columns", "popt_num_epochs",
            "popt_min_data_ways", "popt_target_time_charged",
            "popt_matrix_stream_mode", "popt_offchip_includes_matrix_stream",
            "popt_policy_active", "popt_context_loaded",
            "popt_rereference_loaded", "popt_runtime_epochs",
            "popt_runtime_cache_lines", "popt_roi_rereference_queries",
            "popt_matrix_bytes",
            "popt_reserved_ways", "popt_effective_l3_ways",
            "popt_matrix_stream_bytes", "popt_cumulative_stream_bytes",
            "popt_matrix_stream_iterations",
            "popt_matrix_stream_requests",
            "popt_dram_offchip_bytes_without_matrix_stream",
            "popt_matrix_stream_dram_bytes",
            "popt_stream_requestor_dram_bytes",
            "popt_reload_each_iteration",
            "popt_initial_columns_charged",
            "popt_timing_optimistic", "timing_model", "timing_caveat",
        ),
        "POPT")
    model = config["popt_model"]
    expected = expected_popt(config, graph, iteration)
    for field, value in (
            ("popt_overhead_charged", "1"),
            ("popt_reserve_model", "size_correct"),
            ("popt_matrix_fits", "1"),
            ("popt_property_bytes", model["property_bytes"]),
            ("popt_matrix_active_columns", model["reserved_column_slots"]),
            ("popt_num_epochs", model["epochs"]),
            ("popt_min_data_ways", model["minimum_data_ways"]),
            ("popt_target_time_charged", "0"),
            ("popt_matrix_stream_mode", "analytic_cumulative"),
            ("popt_offchip_includes_matrix_stream", "1"),
            ("popt_policy_active", "1"),
            ("popt_context_loaded", "1"),
            ("popt_rereference_loaded", "1"),
            ("popt_reload_each_iteration",
             int(model["reload_each_iteration"])),
            ("popt_initial_columns_charged",
             int(model["charge_initial_columns_in_roi"])),
            ("popt_timing_optimistic", "1"),
            ("timing_model", "optimistic_popt_analytic_stream")):
        require_text(row, field, value)
    if "favors P-OPT" not in str(row.get("timing_caveat", "")):
        raise ValueError("P-OPT row does not disclose optimistic timing")
    require_positive(row, "popt_roi_rereference_queries")

    without_stream = number(
        row.get("popt_dram_offchip_bytes_without_matrix_stream"),
        "popt_dram_offchip_bytes_without_matrix_stream")
    stream = number(
        row.get("popt_matrix_stream_dram_bytes"),
        "popt_matrix_stream_dram_bytes")
    total = number(row.get("dram_offchip_bytes"), "dram_offchip_bytes")
    if without_stream < 0 or stream <= 0 or total < stream:
        raise ValueError("P-OPT off-chip traffic components are invalid")
    if abs((without_stream + stream) - total) > 0.5:
        raise ValueError("P-OPT off-chip traffic decomposition is invalid")
    requestor_stream = number(
        row.get("popt_stream_requestor_dram_bytes"),
        "popt_stream_requestor_dram_bytes")
    if (
            stream != requestor_stream or
            stream != expected["cumulative_stream_bytes"]):
        raise ValueError("P-OPT stream bytes are not fully charged")

    for field, value in (
            ("popt_matrix_bytes", expected["matrix_bytes"]),
            ("popt_runtime_epochs", model["epochs"]),
            ("popt_runtime_cache_lines",
             expected["matrix_column_bytes"]),
            ("popt_reserved_ways", expected["reserved_ways"]),
            ("popt_effective_l3_ways", expected["effective_ways"]),
            ("popt_matrix_stream_bytes",
             expected["stream_bytes_per_iteration"]),
            ("popt_cumulative_stream_bytes",
             expected["cumulative_stream_bytes"]),
            ("popt_matrix_stream_iterations", iteration),
            ("popt_matrix_stream_requests", expected["stream_requests"])):
        if integer(row.get(field), field) != value:
            raise ValueError(f"P-OPT {field} differs from the frozen model")
    effective_size = (
        parse_size(graph["l3_size"]) * expected["effective_ways"] //
        int(graph["l3_ways"]))
    require_size(row, "popt_effective_l3_size", effective_size)
    require_size(row, "l3_effective_size", effective_size)
    require_text(row, "gem5_l3_ways_actual", expected["effective_ways"])
    require_size(row, "gem5_l3_size_actual", effective_size)


def validate_oracle(
        row: dict[str, Any], config: dict[str, Any],
        graph: dict[str, Any]) -> None:
    require_fields(
        row,
        (
            "popt_overhead_charged", "popt_reserved_ways",
            "popt_target_time_charged", "popt_matrix_stream_mode",
            "popt_matrix_stream_bytes", "popt_matrix_stream_requests",
            "popt_cumulative_stream_bytes",
            "popt_matrix_stream_dram_bytes",
            "popt_stream_requestor_dram_bytes",
            "popt_dram_offchip_bytes_without_matrix_stream",
            "popt_nonstream_requestor_dram_bytes",
            "popt_effective_l3_ways", "popt_effective_l3_size",
            "popt_policy_active", "popt_context_loaded",
            "popt_rereference_loaded", "popt_runtime_epochs",
            "popt_runtime_cache_lines", "popt_roi_rereference_queries",
        ),
        "POPT_UNCHARGED")
    for field, value in (
            ("popt_overhead_charged", "0"),
            ("popt_reserved_ways", "0"),
            ("popt_target_time_charged", "0"),
            ("popt_matrix_stream_mode", "none"),
            ("popt_matrix_stream_bytes", "0"),
            ("popt_matrix_stream_requests", "0"),
            ("popt_cumulative_stream_bytes", "0"),
            ("popt_matrix_stream_dram_bytes", "0"),
            ("popt_stream_requestor_dram_bytes", "0")):
        require_text(row, field, value)
    require_text(row, "popt_effective_l3_ways", graph["l3_ways"])
    require_size(row, "popt_effective_l3_size", graph["l3_size"])
    require_text(row, "gem5_l3_ways_actual", graph["l3_ways"])
    require_size(row, "gem5_l3_size_actual", graph["l3_size"])
    require_text(row, "popt_policy_active", "1")
    require_text(row, "popt_context_loaded", "1")
    require_text(row, "popt_rereference_loaded", "1")
    require_positive(row, "popt_roi_rereference_queries")
    expected = expected_popt(config, graph, 1)
    require_text(
        row, "popt_runtime_epochs", config["popt_model"]["epochs"])
    require_text(
        row, "popt_runtime_cache_lines",
        expected["matrix_column_bytes"])
    total = number(row.get("dram_offchip_bytes"), "dram_offchip_bytes")
    without_stream = number(
        row.get("popt_dram_offchip_bytes_without_matrix_stream"),
        "popt_dram_offchip_bytes_without_matrix_stream")
    nonstream = number(
        row.get("popt_nonstream_requestor_dram_bytes"),
        "popt_nonstream_requestor_dram_bytes")
    if without_stream < 0 or nonstream < 0 or (
            without_stream != total or nonstream != total):
        raise ValueError("uncharged P-OPT traffic accounting is invalid")


def validate_reuse_plan(row: dict[str, Any], config: dict[str, Any],
                graph: dict[str, Any], policy: str) -> None:
    require_fields(
        row,
        (
            "proposal_path_active", "proposal_performance_mode_active",
            "gem5_compact_reuse_bind_flowthrough_active",
            "gem5_compact_reuse_bind_performance_requested",
            "gem5_ecg_delivery", "gem5_reuse_bind_model",
            "ecg_record_bytes", "ecg_record_replaces_edge",
            "edge_stream_bytes_per_edge", "reuse_plan_metadata_bits_per_line",
            "l3_effective_ways", "l3_effective_size",
            "ecg_isa_variant", "ecg_epochs",
            "gem5_reuse_bind_trace_limit",
            "gem5_flowthrough_trace_limit",
            "proposal_compact_id_bits", "proposal_compact_epoch_bits",
            "proposal_compact_tier_bits",
            "gem5_variant_requested_receipt",
            "gem5_variant_effective_receipt",
            "gem5_variant_dueling_receipt",
        ),
        policy)
    for field, value in (
            ("proposal_path_active", "1"),
            ("proposal_performance_mode_active", "1"),
            ("gem5_compact_reuse_bind_flowthrough_active", "1"),
            ("gem5_compact_reuse_bind_performance_requested", "1"),
            ("gem5_ecg_delivery",
             "ecg.flow.load.compact+ecg.bind.load.f32"),
            ("gem5_reuse_bind_model", "request"),
            ("ecg_record_bytes", "4"),
            ("ecg_record_replaces_edge", "1"),
            ("edge_stream_bytes_per_edge", "4"),
            ("reuse_plan_metadata_bits_per_line",
             config["resource_scope"]["reuse_plan_metadata_bits_per_line"]),
            ("ecg_isa_variant", config["isa_variant"]),
            ("ecg_epochs", config["reuse_plan_epochs"]),
            ("gem5_reuse_bind_trace_limit", "0"),
            ("gem5_flowthrough_trace_limit", "0")):
        require_text(row, field, value)
    require_text(row, "proposal_compact_id_bits", graph["compact_id_bits"])
    require_text(
        row, "proposal_compact_epoch_bits", graph["compact_epoch_bits"])
    require_text(
        row, "proposal_compact_tier_bits", config["compact_tier_bits"])
    require_text(row, "l3_effective_ways", graph["l3_ways"])
    require_text(row, "l3_effective_size", graph["l3_size"])
    require_text(row, "gem5_l3_ways_actual", graph["l3_ways"])
    require_size(row, "gem5_l3_size_actual", graph["l3_size"])

    receipt = config["variant_receipts"][policy]
    require_text(row, "gem5_variant_requested_receipt", receipt["requested"])
    require_text(row, "gem5_variant_effective_receipt", receipt["effective"])
    require_text(row, "gem5_variant_dueling_receipt", receipt["dueling"])
    if int(receipt["dueling"]) == 1:
        require_fields(
            row,
            ONLINE_DUELING_REPORTED_FIELDS,
            policy)
        for field in (
                "gem5_reuse_plan_dueling_request_bound_victims",
                "gem5_reuse_plan_dueling_follower_selections",
                "gem5_reuse_plan_dueling_completed_windows"):
            require_positive(row, field)
        if integer(
                row.get("gem5_reuse_plan_dueling_leader_samples"),
                "gem5_reuse_plan_dueling_leader_samples") < (
                    ONLINE_DUELING_WINDOW_MISSES):
            raise ValueError(
                "online ReusePlan did not collect a full leader-sample window")
        for field in (
                "gem5_reuse_plan_dueling_winner_changes",
                "gem5_reuse_plan_dueling_follower_variant_overrides"):
            if number(row.get(field), field) < 0:
                raise ValueError(f"{field} must be nonnegative")


def build_cells(rows: list[dict[str, str]], config: dict[str, Any]) -> dict[
        tuple[str, int], dict[str, dict[str, str]]]:
    graphs = graph_map(config)
    for graph in graphs.values():
        epoch_bits = int(graph["compact_epoch_bits"])
        if 1 << epoch_bits < int(config["reuse_plan_epochs"]):
            raise ValueError(
                f"{graph['name']} has too few compact epoch bits")
        bits = (
            int(graph["compact_id_bits"]) +
            int(config["compact_tier_bits"]) +
            2 * epoch_bits)
        if bits > 32:
            raise ValueError(
                f"{graph['name']} compact record needs {bits} bits")
    roles = policy_roles(config)
    expected_policies = set(roles["all"])
    expected_cells = {
        (graph, iteration)
        for graph in graphs
        for iteration in [int(value) for value in config["iterations"]]
    }
    cells: dict[tuple[str, int], dict[str, dict[str, str]]] = {}

    for row in rows:
        graph_name = str(row.get("final_graph", ""))
        if graph_name not in graphs:
            raise ValueError(f"row outside screen graph set: {graph_name!r}")
        iteration = iteration_from_options(str(row.get("options", "")))
        key = (graph_name, iteration)
        if key not in expected_cells:
            raise ValueError(f"row outside screen cell set: {key}")
        policy = canonical_policy(str(row.get("policy_label", "")))
        if policy not in expected_policies:
            raise ValueError(f"unexpected policy {policy} in {key}")

        validate_common_row(row, config, graphs[graph_name], iteration)
        if not policy.startswith("ECG_REUSE_PLAN_"):
            validate_baseline(row, graphs[graph_name], policy)
        if policy == "GRASP":
            validate_grasp(row)
        elif policy == roles["popt"]:
            validate_popt(row, config, graphs[graph_name], iteration)
        elif policy == "POPT_UNCHARGED":
            validate_oracle(row, config, graphs[graph_name])
        elif policy.startswith("ECG_REUSE_PLAN_"):
            validate_reuse_plan(row, config, graphs[graph_name], policy)

        per_policy = cells.setdefault(key, {})
        if policy in per_policy:
            raise ValueError(f"duplicate policy {policy} in {key}")
        per_policy[policy] = row

    if set(cells) != expected_cells:
        missing = sorted(expected_cells - set(cells))
        extra = sorted(set(cells) - expected_cells)
        raise ValueError(f"incomplete screen cells missing={missing} extra={extra}")
    for key, per_policy in cells.items():
        if set(per_policy) != expected_policies:
            raise ValueError(
                f"incomplete policy roster for {key}: "
                f"{sorted(per_policy)}")
        if len({
                row.get("final_job_id", "")
                for row in per_policy.values()
            }) != 1:
            raise ValueError(f"cell {key} mixes job ids")
    return cells


def classify(ratio: float, tie_band: float) -> str:
    if ratio < 1.0 - tie_band:
        return "win"
    if ratio > 1.0 + tie_band:
        return "loss"
    return "tie"


def compare(
        cells: dict[tuple[str, int], dict[str, dict[str, str]]],
        policy: str, baseline: str, config: dict[str, Any]) -> dict[str, Any]:
    tie_band = float(config["metrics"]["tie_band"])
    entries = []
    for (graph, iteration), per_policy in sorted(cells.items()):
        policy_row = per_policy[policy]
        baseline_row = per_policy[baseline]
        time_ratio = (
            number(policy_row["sim_ticks"], "sim_ticks") /
            number(baseline_row["sim_ticks"], "sim_ticks"))
        traffic_ratio = (
            number(policy_row["dram_offchip_bytes"], "dram_offchip_bytes") /
            number(baseline_row["dram_offchip_bytes"], "dram_offchip_bytes"))
        entries.append({
            "graph": graph,
            "iterations": iteration,
            "time_ratio": time_ratio,
            "traffic_ratio": traffic_ratio,
            "time_class": classify(time_ratio, tie_band),
            "traffic_class": classify(traffic_ratio, tie_band),
        })

    per_graph = {}
    for graph in graph_map(config):
        graph_entries = [entry for entry in entries if entry["graph"] == graph]
        per_graph[graph] = {
            "time_ratio": geomean([
                entry["time_ratio"] for entry in graph_entries]),
            "traffic_ratio": geomean([
                entry["traffic_ratio"] for entry in graph_entries]),
        }
    per_iteration = {}
    for iteration in config["iterations"]:
        iteration_entries = [
            entry for entry in entries
            if entry["iterations"] == int(iteration)]
        per_iteration[str(iteration)] = {
            "time_ratio": geomean([
                entry["time_ratio"] for entry in iteration_entries]),
            "traffic_ratio": geomean([
                entry["traffic_ratio"] for entry in iteration_entries]),
        }
    leave_one_out = {}
    for omitted in graph_map(config):
        retained = [entry for entry in entries if entry["graph"] != omitted]
        leave_one_out[omitted] = {
            "time_ratio": geomean([
                entry["time_ratio"] for entry in retained]),
            "traffic_ratio": geomean([
                entry["traffic_ratio"] for entry in retained]),
        }
    return {
        "aggregate_time_ratio": geomean([
            entry["time_ratio"] for entry in entries]),
        "aggregate_traffic_ratio": geomean([
            entry["traffic_ratio"] for entry in entries]),
        "worst_time_cell": max(entries, key=lambda entry: entry["time_ratio"]),
        "worst_traffic_cell": max(
            entries, key=lambda entry: entry["traffic_ratio"]),
        "wins": sum(entry["time_class"] == "win" for entry in entries),
        "ties": sum(entry["time_class"] == "tie" for entry in entries),
        "losses": sum(entry["time_class"] == "loss" for entry in entries),
        "per_graph": per_graph,
        "per_iteration": per_iteration,
        "leave_one_graph_out": leave_one_out,
        "cells": entries,
    }


def within(value: float, limits: list[float]) -> bool:
    return float(limits[0]) <= value <= float(limits[1])


def instruction_parity(
        cells: dict[tuple[str, int], dict[str, dict[str, str]]],
        policy: str, baseline: str) -> dict[str, Any]:
    entries = []
    for (graph, iteration), per_policy in sorted(cells.items()):
        policy_insts = integer(
            per_policy[policy]["roi_insts"], "roi_insts")
        baseline_insts = integer(
            per_policy[baseline]["roi_insts"], "roi_insts")
        entries.append({
            "graph": graph,
            "iterations": iteration,
            "policy_roi_insts": policy_insts,
            "baseline_roi_insts": baseline_insts,
            "matched": policy_insts == baseline_insts,
        })
    return {
        "passes": all(entry["matched"] for entry in entries),
        "cells": entries,
    }


def evaluate(rows: list[dict[str, str]], config: dict[str, Any]) -> dict[str, Any]:
    cells = build_cells(rows, config)
    roles = policy_roles(config)
    decision = config["decision"]
    if decision.get(
            "replacement_claim_requires_exact_roi_instruction_parity") is not True:
        raise ValueError(
            "replacement claims require exact ROI instruction parity")

    baseline_sanity = {}
    sanity_passes = True
    for baseline in roles["serious"]:
        result = compare(cells, baseline, roles["sanity"], config)
        time_range = (
            decision["charged_popt_sanity_time_range"]
            if baseline == roles["popt"] else
            decision["baseline_sanity_time_range"])
        passed = (
            within(
                result["aggregate_time_ratio"],
                time_range) and
            within(
                result["aggregate_traffic_ratio"],
                decision["baseline_sanity_traffic_range"]) and
            all(
                within(
                    cell["time_ratio"],
                    time_range) and
                within(
                    cell["traffic_ratio"],
                    decision["baseline_sanity_traffic_range"])
                for cell in result["cells"]))
        result["passes"] = passed
        baseline_sanity[baseline] = result
        sanity_passes = sanity_passes and passed

    oracle_sanity = compare(
        cells, roles["oracle"], roles["popt"], config)
    oracle_limit = float(
        decision["max_oracle_ratio_vs_charged"])
    oracle_sanity_passes = not (
            oracle_sanity["aggregate_time_ratio"] > oracle_limit or
            oracle_sanity["aggregate_traffic_ratio"] > oracle_limit or
            any(
                cell["time_ratio"] > oracle_limit or
                cell["traffic_ratio"] > oracle_limit
                for cell in oracle_sanity["cells"]))
    oracle_sanity["passes"] = oracle_sanity_passes
    screen_valid = sanity_passes and oracle_sanity_passes

    popt_stream_accounting = []
    for (graph, iteration), per_policy in sorted(cells.items()):
        charged = per_policy[roles["popt"]]
        oracle = per_policy[roles["oracle"]]
        stream_bytes = number(
            charged["popt_matrix_stream_dram_bytes"],
            "popt_matrix_stream_dram_bytes")
        total_bytes = number(
            charged["dram_offchip_bytes"], "dram_offchip_bytes")
        popt_stream_accounting.append({
            "graph": graph,
            "iterations": iteration,
            "charged_stream_bytes": stream_bytes,
            "charged_total_offchip_bytes": total_bytes,
            "charged_stream_share": stream_bytes / total_bytes,
            "uncharged_time_ratio": (
                number(oracle["sim_ticks"], "sim_ticks") /
                number(charged["sim_ticks"], "sim_ticks")),
            "uncharged_traffic_ratio": (
                number(oracle["dram_offchip_bytes"], "dram_offchip_bytes") /
                total_bytes),
        })

    candidates = {}
    for candidate in [roles["primary"], *roles["characterization"]]:
        comparisons = {
            baseline: compare(cells, candidate, baseline, config)
            for baseline in [
                roles["sanity"], *roles["serious"],
                roles["transport"], roles["oracle"]]
        }
        guard_baselines = [roles["sanity"], *roles["serious"]]
        longest_iteration = str(max(int(value) for value in config["iterations"]))
        performance_guards_pass = True
        for baseline in guard_baselines:
            result = comparisons[baseline]
            time_limit = (
                decision["max_time_ratio_vs_lru"]
                if baseline == roles["sanity"] else
                decision["max_time_ratio_vs_serious_baselines"])
            traffic_limit = (
                decision["max_traffic_ratio_vs_lru"]
                if baseline == roles["sanity"] else
                decision["max_traffic_ratio_vs_serious_baselines"])
            performance_guards_pass = performance_guards_pass and (
                result["aggregate_time_ratio"] <= float(time_limit) and
                result["aggregate_traffic_ratio"] <= float(traffic_limit) and
                all(
                    cell["time_ratio"] <=
                    float(decision["max_time_ratio_per_cell"]) and
                    cell["traffic_ratio"] <=
                    float(decision["max_traffic_ratio_per_cell"])
                    for cell in result["cells"]) and
                all(
                    graph["time_ratio"] <=
                    float(decision["max_time_ratio_per_graph"])
                    for graph in result["per_graph"].values()) and
                result["per_iteration"][longest_iteration]["time_ratio"] <=
                float(decision["max_time_ratio_i8"]) and
                all(
                    logo["time_ratio"] <= float(
                        decision["max_time_ratio_leave_one_graph_out"]) and
                    logo["traffic_ratio"] <= float(
                        decision["max_traffic_ratio_leave_one_graph_out"])
                    for logo in result["leave_one_graph_out"].values()))

        transport_comparison = comparisons[roles["transport"]]
        transport_instruction_parity = instruction_parity(
            cells, candidate, roles["transport"])
        transport_pass = (
            transport_instruction_parity["passes"] and
            transport_comparison["aggregate_time_ratio"] <= float(
                decision[
                    "max_time_ratio_vs_transport_control_for_policy_claim"]) and
            all(
                result["time_ratio"] <= float(
                    decision[
                        "max_time_ratio_vs_transport_control_"
                        "leave_one_graph_out"])
                for result in
                transport_comparison["leave_one_graph_out"].values()))
        passes = screen_valid and performance_guards_pass
        candidates[candidate] = {
            "decision_role": (
                "primary" if candidate == roles["primary"]
                else "characterization_only"),
            "passes": passes,
            "performance_guards_pass": performance_guards_pass,
            "comparisons": comparisons,
            "replacement_instruction_parity":
                transport_instruction_parity,
            "replacement_policy_contribution": transport_pass,
            "claim_classification": (
                "characterization_only"
                if candidate != roles["primary"] else
                "inconclusive_invalid_baselines"
                if not sanity_passes else
                "inconclusive_invalid_oracle"
                if not oracle_sanity_passes else
                "replacement_policy_supported_complete_design_failed"
                if not performance_guards_pass and transport_pass else
                "no_claim_screen_failed"
                if not performance_guards_pass else
                "complete_design_and_replacement_policy"
                if transport_pass else
                "complete_design_transport_or_layout_only"),
        }

    primary_passes = candidates[roles["primary"]]["passes"]
    primary_performance_passes = candidates[roles["primary"]][
        "performance_guards_pass"]
    oracle_comparison = candidates[roles["primary"]][
        "comparisons"][roles["oracle"]]
    screen_result = (
        "inconclusive_invalid_baselines"
        if not sanity_passes else
        "inconclusive_invalid_oracle"
        if not oracle_sanity_passes else
        "go"
        if primary_performance_passes else
        "stop")
    return {
        "screen_id": config["id"],
        "cell_count": len(cells),
        "row_count": sum(len(value) for value in cells.values()),
        "screen_valid": screen_valid,
        "screen_result": screen_result,
        "baseline_sanity_passes": sanity_passes,
        "baseline_sanity": baseline_sanity,
        "oracle_sanity_passes": oracle_sanity_passes,
        "oracle_sanity": oracle_sanity,
        "popt_stream_accounting": popt_stream_accounting,
        "primary_candidate": roles["primary"],
        "candidates": candidates,
        "screen_passes": primary_passes,
        "stop_broad_campaign": bool(
            decision["stop_if_primary_fails"] and
            screen_valid and not primary_performance_passes),
        "replacement_policy_claim_allowed": bool(
            screen_valid and
            candidates[roles["primary"]][
                "replacement_policy_contribution"]),
        "primary_vs_oracle_time_ratio":
            oracle_comparison["aggregate_time_ratio"],
        "primary_within_oracle_robustness_band": bool(
            oracle_comparison["aggregate_time_ratio"] <= float(
                decision["max_ratio_vs_oracle_for_robustness"]) and
            oracle_comparison["aggregate_traffic_ratio"] <= float(
                decision["max_ratio_vs_oracle_for_robustness"])),
        "resource_scope": config["resource_scope"],
        "sampling_caveat": config["sampling_caveat"],
        "decision": decision,
        "popt_model": config["popt_model"],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--output", default="")
    args = parser.parse_args(argv)

    config = json.loads(Path(args.config).read_text())
    result = evaluate(load_rows(Path(args.input)), config)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
