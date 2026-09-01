"""Preregistration and gate tests for the ReusePlan transport campaign."""

import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

from scripts.experiments.ecg.analysis import transport_scale_gate


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "scripts/experiments/ecg/experiment_manifest.json"
CONFIG_PATH = (
    ROOT / "scripts/experiments/ecg/configs/transport_literature_scale.json")
PAGERANK_CONFIG_PATH = (
    ROOT / "scripts/experiments/ecg/configs/pagerank_literature_scale.json")
EXPERIMENT_RUN_PATH = (
    ROOT / "scripts/experiments/ecg/flows/experiment_run.py")
MANIFEST = json.loads(MANIFEST_PATH.read_text())
CONFIG = json.loads(CONFIG_PATH.read_text())
PROFILE = "reuse_plan_transport_campaign"
BASELINE = "LRU"
CANDIDATE = "ECG_REUSE_PLAN_LRU_FLOWTHROUGH"
MECHANISM_ROSTER = (
    "ECG_REUSE_PLAN",
    "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
    "ECG_REUSE_PLAN_FLOWTHROUGH",
)


def load_experiment_run():
    spec = importlib.util.spec_from_file_location(
        "experiment_run_transport_test", EXPERIMENT_RUN_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def timing_row(graph, stage, iterations, policy, ticks, traffic):
    receipt = graph["semantic_receipts"][str(iterations)]
    row = {
        "final_stage": stage,
        "final_graph": graph["name"],
        "final_job_id": f"{stage}-{graph['name']}",
        "final_matrix_config_hash": "hash",
        "final_output_status": "ok",
        "status": "ok",
        "benchmark": "pr",
        "policy_label": policy,
        "simulator": "gem5",
        "gem5_cpu_type": "O3",
        "prefetcher": "none",
        "timing_valid_for_speedup": "1",
        "pr_result_matched": "1",
        "pr_result_group_rows_ok": "1",
        "l3_exercised": "True",
        "l1d_size": graph["l1d_size"],
        "l1d_ways": str(graph["l1d_ways"]),
        "l2_size": graph["l2_size"],
        "l2_ways": str(graph["l2_ways"]),
        "l3_size": graph["l3_size"],
        "l3_ways": str(graph["l3_ways"]),
        "line_size": str(graph["line_size"]),
        "l3_effective_size": graph["l3_size"],
        "l3_effective_ways": str(graph["l3_ways"]),
        "gem5_l3_size_actual": graph["l3_size"],
        "gem5_l3_ways_actual": str(graph["l3_ways"]),
        "options": CONFIG["options_template"].format(
            graph_path=str((ROOT / graph["path"]).resolve()),
            iterations=iterations),
        "pr_iterations": str(iterations),
        "pr_semantic_edges": str(receipt["edges"]),
        "pr_score_checksum": receipt["checksum"],
        "sim_ticks": str(ticks),
        "dram_offchip_bytes": str(traffic),
        "l3_misses": "100",
        "roi_insts": "1000",
        "ipc": "1.0",
        "dram_bus_util_pct": "1.0",
        "gem5_structural_flowthrough_receipt": "1",
        "gem5_structural_flowthrough_miss_targets": "500",
    }
    if policy == CANDIDATE:
        variant = CONFIG["variant_receipts"][CANDIDATE]
        row.update({
            "proposal_path_active": "1",
            "proposal_performance_mode_active": "1",
            "gem5_compact_reuse_bind_flowthrough_active": "1",
            "gem5_compact_reuse_bind_performance_requested": "1",
            "gem5_ecg_delivery":
                "ecg.flow.load.compact+ecg.bind.load.f32",
            "gem5_reuse_bind_model": "request",
            "ecg_reuse_plan_depth": "2",
            "ecg_record_bytes": "4",
            "ecg_record_replaces_edge": "1",
            "edge_stream_bytes_per_edge": "4",
            "reuse_plan_metadata_bits_per_line": "49",
            "ecg_isa_variant": CONFIG["isa_variant"],
            "ecg_epochs": str(CONFIG["reuse_plan_epochs"]),
            "gem5_reuse_bind_trace_limit": "0",
            "gem5_flowthrough_trace_limit": "0",
            "proposal_compact_id_bits": str(graph["compact_id_bits"]),
            "proposal_compact_epoch_bits": str(graph["compact_epoch_bits"]),
            "proposal_compact_tier_bits": str(CONFIG["compact_tier_bits"]),
            "gem5_variant_requested_receipt": variant["requested"],
            "gem5_variant_effective_receipt": str(variant["effective"]),
            "gem5_variant_dueling_receipt": str(variant["dueling"]),
            "gem5_reuse_plan_sidecar_active": "1",
            "gem5_reuse_plan_sidecar_record_bytes": "4",
            "gem5_reuse_plan_sidecar_tier_bits": str(
                CONFIG["compact_tier_bits"]),
            "gem5_reuse_plan_sidecar_records": str(
                graph["directed_edges"]),
            "gem5_reuse_plan_coverage_validated": "1",
            "gem5_reuse_plan_victim_selections": "100",
            "gem5_reuse_plan_victim_stamped_ways": "80",
        })
    else:
        row.update({
            "proposal_path_active": "0",
            "ecg_reuse_plan_depth": "0",
            "graph_edge_bytes": "4",
            "edge_stream_bytes_per_edge": "4",
            "ecg_record_replaces_edge": "0",
            "popt_reserved_ways": "0",
            "popt_effective_l3_ways": str(graph["l3_ways"]),
            "popt_effective_l3_size": graph["l3_size"],
        })
    return row


def mechanism_rows():
    return [
        {
            "final_stage": "60_gem5_proposal_reuse_bind_o3",
            "final_graph": "kron_s12_k4",
            "final_matrix_config_hash": "hash",
            "benchmark": "pr",
            "status": "ok",
            "policy_label": policy,
            "simulator": "gem5",
            "timing_valid_for_speedup": "0",
        }
        for policy in MECHANISM_ROSTER
    ]


def cache_row(stage, graph, benchmark, policy, traffic, misses):
    compact = stage == transport_scale_gate.COMPACT_STAGE
    row = {
        "final_stage": stage,
        "final_graph": graph,
        "final_matrix_config_hash": "hash",
        "status": "ok",
        "benchmark": benchmark,
        "policy_label": policy,
        "simulator": "cache_sim",
        "timing_valid_for_speedup": "0",
        "flowthrough": "all",
        "structural_flowthrough_accesses": "1000",
        "ecg_epochs_effective": "16",
        "options": f"-f {graph}.sg -o 0 -n 1",
        "l1d_size": "32kB",
        "l1d_ways": "8",
        "l2_size": "256kB",
        "l2_ways": "8",
        "l3_size": "8MB",
        "l3_ways": "16",
        "l3_effective_ways": "16",
        "line_size": "64",
        "prefetcher": "none",
        "ecg_vertices": "1000",
        "total_offchip_traffic": str(traffic),
        "l3_misses": str(misses),
        "total_accesses": "5000",
        "l1_hits": "10",
        "l1_misses": "10",
        "l2_hits": "10",
        "l2_misses": "10",
        "l3_hits": "10",
        "llc_writebacks": "10",
        "memory_accesses": "10",
    }
    if policy == CANDIDATE:
        row.update({
            "ecg_record_bytes": "4" if compact else "8",
            "edge_stream_bytes_per_edge": "4" if compact else "8",
            "ecg_record_replaces_edge": "1",
            "ecg_variant_effective": "lru_only",
        })
    return row


def sniper_row(graph, benchmark, policy, limit, misses):
    row = {
        "final_stage": transport_scale_gate.SNIPER_STAGE,
        "final_graph": graph,
        "final_matrix_config_hash": "hash",
        "status": "ok",
        "benchmark": benchmark,
        "policy_label": policy,
        "simulator": "sniper",
        "timing_valid_for_speedup": "0",
        "sniper_queue_model": "windowed_mg1",
        "sniper_semantic_edge_limit": str(limit),
        "sniper_semantic_edge_visits": str(limit),
        "semantic_work_matched": "1",
        "sniper_structural_flowthrough_receipt": "1",
        "l3_misses": str(misses),
    }
    if policy == CANDIDATE:
        row.update({
            "sniper_transport_record_bytes": "8",
            "edge_stream_bytes_per_edge": "8",
            "sniper_transport_receipts_validated": "1",
            "sniper_reuse_plan_epoch_context_validated": "1",
            "sniper_reuse_bind_exact_validated": "1",
            "sniper_reuse_bind_bad_consumes": "0",
            "ecg_variant_effective": "lru_only",
        })
    return row


def screen_rows(time_ratio=0.95, traffic_ratio=0.99, overrides=None):
    overrides = overrides or {}
    rows = mechanism_rows()
    for graph in CONFIG["graphs"]:
        ratio = overrides.get(graph["name"], (time_ratio, traffic_ratio))
        rows.append(timing_row(
            graph, transport_scale_gate.SCREEN_TIMING_STAGE, 1,
            BASELINE, 100.0, 1000.0))
        rows.append(timing_row(
            graph, transport_scale_gate.SCREEN_TIMING_STAGE, 1,
            CANDIDATE, 100.0 * ratio[0], 1000.0 * ratio[1]))
    return rows


def complete_rows(
        compact_traffic_ratio=0.95, sniper_llc_miss_ratio=1.0,
        compact_miss_ratio=1.0):
    rows = screen_rows()
    for graph in CONFIG["graphs"]:
        rows.append(timing_row(
            graph, transport_scale_gate.ROBUSTNESS_TIMING_STAGE, 8,
            BASELINE, 800.0, 8000.0))
        rows.append(timing_row(
            graph, transport_scale_gate.ROBUSTNESS_TIMING_STAGE, 8,
            CANDIDATE, 800.0 * 0.95, 8000.0 * 0.99))
    stage = {stage["name"]: stage for stage in MANIFEST["stages"]}
    cache_graphs = [
        str(graph["name"])
        for graph in MANIFEST["graph_sets"]["literature_scale_compact16"]
    ]
    for graph in cache_graphs:
        for benchmark in stage[
                transport_scale_gate.WIDE_STAGE]["benchmarks"]:
            for policy in (BASELINE, CANDIDATE):
                rows.append(cache_row(
                    transport_scale_gate.WIDE_STAGE, graph, benchmark,
                    policy, 2000, 200))
                compact_traffic = (
                    2000 * compact_traffic_ratio
                    if policy == CANDIDATE else 2000)
                compact_misses = (
                    200 * compact_miss_ratio
                    if policy == CANDIDATE else 200)
                rows.append(cache_row(
                    transport_scale_gate.COMPACT_STAGE, graph, benchmark,
                    policy, compact_traffic, compact_misses))
    for graph in MANIFEST["graph_sets"]["literature_scale_sniper"]:
        limit = int(graph["sniper_semantic_edge_limit"])
        for benchmark in stage[
                transport_scale_gate.SNIPER_STAGE]["benchmarks"]:
            rows.append(sniper_row(
                str(graph["name"]), benchmark, BASELINE, limit, 400))
            rows.append(sniper_row(
                str(graph["name"]), benchmark, CANDIDATE, limit,
                400 * sniper_llc_miss_ratio))
    ordered = []
    for stage_name in (
            "60_gem5_proposal_reuse_bind_o3",
            transport_scale_gate.SCREEN_TIMING_STAGE,
            transport_scale_gate.ROBUSTNESS_TIMING_STAGE,
            transport_scale_gate.WIDE_STAGE,
            transport_scale_gate.COMPACT_STAGE,
            transport_scale_gate.SNIPER_STAGE):
        ordered.extend(
            row for row in rows if row["final_stage"] == stage_name)
    return ordered


def test_transport_config_is_frozen_and_replacement_free():
    assert CONFIG["id"] == "transport_literature_scale"
    assert CONFIG["policies"]["all"] == [
        "LRU", "ECG:REUSE_PLAN_LRU_FLOWTHROUGH"]
    assert CONFIG["policies"]["victim_policy_of_record"] == "LRU"
    assert "popt_model" not in CONFIG
    assert CONFIG["iterations"] == [1, 8]
    assert CONFIG["reuse_plan_epochs"] == 16
    assert CONFIG["compact_tier_bits"] == 2
    assert CONFIG["prefetcher"] == "none"
    assert CONFIG["isa_variant"] == "computed"
    assert CONFIG["reuse_plan_timing_mode"] == "compact_trace_free"
    assert CONFIG["record_format"]["gem5_timing_record_bytes"] == 4
    assert CONFIG["record_format"]["cache_compact_record_bytes"] == 4
    assert CONFIG["record_format"]["cache_wide_record_bytes"] == 8
    assert CONFIG["record_format"]["sniper_transport_record_bytes"] == 8
    screen = CONFIG["decision"]["screen"]
    complete = CONFIG["decision"]["complete"]
    assert screen == {
        "max_time_ratio_vs_lru": 0.98,
        "max_traffic_ratio_vs_lru": 1.02,
        "max_time_ratio_per_cell": 1.02,
        "max_traffic_ratio_per_cell": 1.02,
        "stop_if_screen_fails": True,
    }
    assert complete["max_time_ratio_vs_lru_i8"] == 0.98
    assert complete["max_traffic_ratio_vs_lru_i8"] == 1.02
    assert complete["max_time_ratio_per_cell"] == 1.02
    assert complete["max_traffic_ratio_per_cell"] == 1.02
    assert complete["max_compact_wide_traffic_ratio_aggregate"] == 0.98
    assert complete["max_compact_wide_traffic_ratio_per_cell"] == 1.02
    assert complete["max_compact_wide_llc_miss_ratio_per_cell"] == 1.02
    assert complete["max_sniper_llc_miss_ratio_aggregate"] == 1.02
    assert complete["max_sniper_llc_miss_ratio_per_cell"] == 1.02
    assert complete["sniper_timing_admissible"] is False
    assert complete["mechanism_stage_timing_admissible"] is False
    assert CONFIG["decision"][
        "thresholds_frozen_before_this_campaign_runs"] is True
    assert CONFIG["decision"][
        "thresholds_informed_by_prior_transport_control_rows"] is True
    assert "+/-2% tie band" in CONFIG["decision"]["threshold_basis"]
    assert CONFIG["prior_evidence"]["git_commit"].startswith("14b82753")
    assert CONFIG["prior_evidence"]["aggregate_time_ratio"] == pytest.approx(
        0.8050035925960729)
    disallowed = " ".join(CONFIG["disallowed_claims"]).lower()
    for term in ("replacement", "srrip", "grasp", "p-opt"):
        assert term in disallowed
    assert CONFIG["execution"]["policy_sharding_allowed"] is False
    assert CONFIG["record_format"][
        "full_graph_compact_eligible_graphs"] == [
            "web-Google", "soc-pokec", "cit-Patents",
            "roadNet-CA", "com-Orkut"]
    assert "soc-LiveJournal1" in CONFIG["record_format"][
        "full_graph_compact_exclusions"]


def test_transport_config_copies_graph_identity_and_receipts():
    pagerank = json.loads(PAGERANK_CONFIG_PATH.read_text())
    assert CONFIG["graphs"] == pagerank["graphs"]
    assert CONFIG["options_template"] == pagerank["options_template"]
    assert len(CONFIG["graphs"]) == 6


def test_transport_manifest_stages_are_frozen():
    control = MANIFEST["profile_controls"][PROFILE]
    assert control["require_clean_worktree"] is True
    assert control["status"] == "ready"
    assert PROFILE in MANIFEST["profiles"]
    stages = {
        stage["name"]: stage for stage in MANIFEST["stages"]
        if PROFILE in stage.get("profiles", [])
    }
    assert set(stages) == {
        "60_gem5_proposal_reuse_bind_o3",
        "96_gem5_transport_i1",
        "97_gem5_transport_i8",
        "98_cache_sim_transport_wide16",
        "99_cache_sim_transport_compact16",
        "100_sniper_transport_matched",
    }
    for name in (
            "96_gem5_transport_i1",
            "97_gem5_transport_i8",
            "98_cache_sim_transport_wide16",
            "99_cache_sim_transport_compact16",
            "100_sniper_transport_matched"):
        assert stages[name]["flowthrough"] == "all"
    for name, iteration in (
            ("96_gem5_transport_i1", 1), ("97_gem5_transport_i8", 8)):
        assert stages[name]["suite"] == "gem5"
        assert stages[name]["screen_iteration"] == iteration
        assert stages[name]["screen_config"] == (
            "scripts/experiments/ecg/configs/"
            "transport_literature_scale.json")
        assert stages[name]["env"][
            "GEM5_REUSE_PLAN_COVERAGE_REQUIRED"] == "1"
    for name in (
            "98_cache_sim_transport_wide16",
            "99_cache_sim_transport_compact16"):
        stage = stages[name]
        assert stage["suite"] == "cache-sim"
        assert stage["graph_set"] == "literature_scale_compact16"
        assert stage["benchmarks"] == ["pr", "bfs", "bc", "cc"]
        assert stage["policies"] == [
            "LRU", "ECG:REUSE_PLAN_LRU_FLOWTHROUGH"]
        assert stage["prefetcher"] == "none"
        assert stage["ecg_epochs"] == 16
        assert stage["ecg_isa_variant"] == "computed"
        assert stage["policy_sharding_allowed"] is False
    assert stages["98_cache_sim_transport_wide16"]["env"] == {
        "ECG_EDGE_RECORD_BYTES": "8",
        "ECG_EXPECT_BYTES_PER_EDGE": "8",
    }
    assert stages["99_cache_sim_transport_compact16"]["env"] == {
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
    }
    sniper = stages["100_sniper_transport_matched"]
    assert sniper["suite"] == "sniper"
    assert sniper["graph_set"] == "literature_scale_sniper"
    assert sniper["benchmarks"] == ["pr", "cc"]
    assert sniper["policies"] == ["LRU", "ECG:REUSE_PLAN_LRU_FLOWTHROUGH"]
    assert sniper["sniper_workload"] == "sg_kernel"
    assert sniper["sniper_frontend"] == "sift"
    assert sniper["sniper_queue_model"] == "windowed_mg1"
    assert sniper["sniper_address_domain"] == "virtual"
    assert sniper["sniper_require_fused_receipts"] is True
    assert sniper["env"]["ECG_EDGE_RECORD_BYTES"] == "8"


def test_transport_expected_cell_shapes():
    screen = transport_scale_gate.expected_cells(
        MANIFEST, CONFIG, transport_scale_gate.SCREEN_STAGES)
    complete = transport_scale_gate.expected_cells(
        MANIFEST, CONFIG, transport_scale_gate.COMPLETE_STAGES)
    assert len(screen) == 7
    assert sum(len(roster) for roster in screen.values()) == 15
    assert len(complete) == 59
    assert sum(len(roster) for roster in complete.values()) == 119


def test_screen_gate_reports_go_on_complete_evidence():
    result = transport_scale_gate.evaluate(
        screen_rows(), MANIFEST, CONFIG, [], "screen")
    assert result["errors"] == []
    assert result["valid"] is True
    assert result["decision"] == "GO"
    assert result["campaign"] == PROFILE
    assert result["cell_count"] == 7
    assert result["row_count"] == 15
    assert result["stage_rows"] == {
        "60_gem5_proposal_reuse_bind_o3": 3,
        "96_gem5_transport_i1": 12,
    }
    assert result["full_roles_authorized"] is True
    assert result["replacement_claim_allowed"] is False
    assert result["screen_timing"]["aggregate_time_ratio"] == pytest.approx(
        0.95)


def test_screen_gate_stops_on_aggregate_threshold_failure():
    result = transport_scale_gate.evaluate(
        screen_rows(time_ratio=0.99), MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is True
    assert result["decision"] == "STOP"
    assert result["screen_timing"]["passes"] is False
    assert result["screen_timing"]["per_cell_violations"] == []


def test_screen_gate_stops_on_single_cell_regression():
    overrides = {"com-Orkut-final-n18": (1.10, 0.99)}
    result = transport_scale_gate.evaluate(
        screen_rows(overrides=overrides), MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is True
    assert result["decision"] == "STOP"
    violations = result["screen_timing"]["per_cell_violations"]
    assert [entry["graph"] for entry in violations] == [
        "com-Orkut-final-n18"]


def test_screen_gate_stops_on_traffic_regression():
    overrides = {"cit-Patents-final-n18": (0.95, 1.10)}
    result = transport_scale_gate.evaluate(
        screen_rows(overrides=overrides), MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is True
    assert result["decision"] == "STOP"
    assert [
        entry["graph"]
        for entry in result["screen_timing"]["per_cell_violations"]
    ] == ["cit-Patents-final-n18"]


def test_screen_gate_is_invalid_without_symmetric_flowthrough():
    rows = screen_rows()
    for row in rows:
        if (row["final_stage"] == transport_scale_gate.SCREEN_TIMING_STAGE
                and row["policy_label"] == BASELINE):
            row["gem5_structural_flowthrough_receipt"] = "0"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is False
    assert result["decision"] == "INVALID"
    assert any(
        "structural FlowThrough receipt" in error
        for error in result["errors"])


def test_screen_gate_is_invalid_on_malformed_and_incomplete_rows():
    empty = transport_scale_gate.evaluate([], MANIFEST, CONFIG, [], "screen")
    assert empty["valid"] is False
    assert empty["decision"] == "INVALID"
    assert any("missing transport cells" in error for error in empty["errors"])
    assert any(
        "timing rows incomplete" in error for error in empty["errors"])

    rows = screen_rows()
    for row in rows:
        if row["policy_label"] == CANDIDATE and row["final_stage"] == (
                transport_scale_gate.SCREEN_TIMING_STAGE):
            row["ecg_record_bytes"] = "not-a-number"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is False
    assert result["decision"] == "INVALID"
    assert any("is malformed" in error for error in result["errors"])


def test_screen_gate_rejects_wrong_semantic_receipt():
    rows = screen_rows()
    for row in rows:
        if row["final_stage"] == transport_scale_gate.SCREEN_TIMING_STAGE:
            row["pr_score_checksum"] = "0000000000000000"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is False
    assert any("timing gate failed" in error for error in result["errors"])


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("gem5_reuse_plan_sidecar_tier_bits", "0"),
        ("gem5_reuse_plan_sidecar_records", "1"),
        ("gem5_reuse_plan_coverage_validated", "0"),
        ("gem5_reuse_plan_victim_selections", "0"),
        ("gem5_reuse_plan_victim_stamped_ways", "0"),
    ),
)
def test_screen_gate_requires_bound_sidecar_and_stamp_coverage(field, value):
    rows = screen_rows()
    for row in rows:
        if (
                row["final_stage"] ==
                transport_scale_gate.SCREEN_TIMING_STAGE and
                row["policy_label"] == CANDIDATE):
            row[field] = value
            break
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "screen")
    assert result["valid"] is False
    assert result["decision"] == "INVALID"
    assert any(
        "validated record sidecar" in error or
        "positive ReusePlan stamp coverage" in error
        for error in result["errors"])


def test_transport_corpus_binds_timing_fullgraph_and_sniper_inputs(tmp_path):
    receipt = (
        ROOT / "results/graphs/literature_scale_corpus.receipt.json")
    assert transport_scale_gate.validate_transport_corpus(
        MANIFEST, CONFIG, receipt) == []

    modified = json.loads(receipt.read_text())
    for graph in modified["graphs"]:
        if graph["name"] == "roadNet-CA":
            graph["dbg_serialized_edges"] += 1
            break
    bad = tmp_path / "corpus.json"
    bad.write_text(json.dumps(modified))
    errors = transport_scale_gate.validate_transport_corpus(
        MANIFEST, CONFIG, bad)
    assert any(
        "Sniper semantic limit differs" in error or
        "preordered graph header mismatch" in error
        for error in errors)


def test_complete_gate_passes_with_matched_roles():
    result = transport_scale_gate.evaluate(
        complete_rows(), MANIFEST, CONFIG, [], "complete")
    assert result["errors"] == []
    assert result["decision"] == "PASS"
    assert result["cell_count"] == 59
    assert result["row_count"] == 119
    assert result["compact_vs_wide"]["passes"] is True
    assert result["compact_vs_wide"][
        "aggregate_traffic_ratio"] == pytest.approx(0.95)
    assert result["sniper_llc_misses"]["passes"] is True
    assert result["sniper_llc_misses"]["timing_admissible"] is False
    assert result["sniper_llc_misses"][
        "aggregate_llc_miss_ratio"] == pytest.approx(1.0)
    assert result["iteration_8_timing"]["passes"] is True


def test_complete_gate_fails_when_compact_saves_too_little():
    result = transport_scale_gate.evaluate(
        complete_rows(compact_traffic_ratio=0.99),
        MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is True
    assert result["decision"] == "FAIL"
    assert result["compact_vs_wide"]["passes"] is False


def test_complete_gate_fails_on_compact_llc_miss_regression():
    result = transport_scale_gate.evaluate(
        complete_rows(compact_miss_ratio=1.05),
        MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is True
    assert result["decision"] == "FAIL"
    assert result["compact_vs_wide"]["per_cell_violations"]


def test_complete_gate_rejects_mismatched_record_widths():
    rows = complete_rows()
    for row in rows:
        if (row["final_stage"] == transport_scale_gate.COMPACT_STAGE and
                row["policy_label"] == CANDIDATE):
            row["ecg_record_bytes"] = "8"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is False
    assert any(
        "record width is not 4 bytes" in error for error in result["errors"])


def test_complete_gate_rejects_cache_baseline_drift():
    rows = complete_rows()
    for row in rows:
        if (row["final_stage"] == transport_scale_gate.COMPACT_STAGE and
                row["policy_label"] == BASELINE):
            row["total_accesses"] = "4999"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is False
    assert any(
        "baseline drift across record widths" in error
        for error in result["errors"])


def test_complete_gate_requires_the_lru_victim_variant():
    rows = complete_rows()
    for row in rows:
        if (row["final_stage"] in (
                transport_scale_gate.WIDE_STAGE,
                transport_scale_gate.COMPACT_STAGE) and
                row["policy_label"] == CANDIDATE):
            row["ecg_variant_effective"] = "rrip_first"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is False
    assert any(
        "does not use the LRU victim variant" in error
        for error in result["errors"])


def test_complete_gate_fails_on_sniper_llc_miss_regression():
    result = transport_scale_gate.evaluate(
        complete_rows(sniper_llc_miss_ratio=1.05),
        MANIFEST, CONFIG, [], "complete")
    assert result["valid"] is True
    assert result["decision"] == "FAIL"
    assert result["sniper_llc_misses"]["passes"] is False


def test_sniper_rows_never_contribute_timing():
    rows = complete_rows()
    sniper_rows = [
        row for row in rows
        if row["final_stage"] == transport_scale_gate.SNIPER_STAGE
    ]
    assert sniper_rows
    for row in sniper_rows:
        assert row["timing_valid_for_speedup"] == "0"
    result = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "complete")
    timing_stages = {
        result["screen_timing"]["stage"],
        result["iteration_8_timing"]["stage"],
    }
    assert transport_scale_gate.SNIPER_STAGE not in timing_stages
    assert result["sniper_timing_admissible"] is False
    assert result["mechanism_stage_timing_admissible"] is False

    for row in rows:
        if row["final_stage"] == transport_scale_gate.SNIPER_STAGE:
            row["timing_valid_for_speedup"] = "1"
    invalid = transport_scale_gate.evaluate(
        rows, MANIFEST, CONFIG, [], "complete")
    assert invalid["valid"] is False
    assert any(
        "Sniper role is malformed" in error for error in invalid["errors"])


def test_full_role_authorization_must_bind_commit_and_hashes(tmp_path):
    manifest = tmp_path / "manifest.json"
    config = tmp_path / "transport.json"
    manifest.write_text("{}")
    config.write_text("{}")
    gate = tmp_path / "screen_gate.json"
    gate.write_text(json.dumps({
        "valid": True,
        "phase": "screen",
        "campaign": PROFILE,
        "decision": "GO",
    }))
    run_dir = tmp_path / "full"
    run_dir.mkdir()
    snapshot = {
        "jobs": [{"stage": "98_cache_sim_transport_wide16"}],
        "screen_authorization": {
            "path": str(gate),
            "sha256": transport_scale_gate.sha256(gate),
            "git_head": "abc",
            "manifest_sha256": transport_scale_gate.sha256(manifest),
            "screen_config_sha256": transport_scale_gate.sha256(config),
        },
    }
    (run_dir / "resolved_manifest.json").write_text(json.dumps(snapshot))
    assert transport_scale_gate.validate_full_role_authorizations(
        [run_dir], manifest, config, "abc") == []
    assert "transport authorization is stale" in (
        transport_scale_gate.validate_full_role_authorizations(
            [run_dir], manifest, config, "def")[0])

    gate.write_text(json.dumps({
        "valid": True,
        "phase": "screen",
        "campaign": PROFILE,
        "decision": "STOP",
    }))
    snapshot["screen_authorization"]["sha256"] = (
        transport_scale_gate.sha256(gate))
    (run_dir / "resolved_manifest.json").write_text(json.dumps(snapshot))
    assert "transport authorization is stale" in (
        transport_scale_gate.validate_full_role_authorizations(
            [run_dir], manifest, config, "abc")[0])


def transport_gate_payload(module):
    return {
        "valid": True,
        "phase": "screen",
        "campaign": PROFILE,
        "decision": "GO",
        "cell_count": 7,
        "row_count": 15,
        "stage_rows": {
            "60_gem5_proposal_reuse_bind_o3": 3,
            "96_gem5_transport_i1": 12,
        },
        "run_dirs": ["results/ecg_experiments/runs/transport_screen"],
        "git_head": "abc",
        "manifest_sha256": module.file_sha256(MANIFEST_PATH),
        "screen_config_sha256": module.file_sha256(CONFIG_PATH),
    }


def transport_job(module, tmp_path, stage="98_cache_sim_transport_wide16"):
    return module.Job(
        job_id="full", stage=stage, kind="roi_matrix", command=[],
        out_dir=tmp_path / "out", log_path=tmp_path / "log")


def test_transport_full_roles_require_valid_screen_receipt(
        tmp_path, monkeypatch):
    module = load_experiment_run()
    payload = transport_gate_payload(module)
    gate = tmp_path / "transport_gate.json"
    gate.write_text(json.dumps(payload))
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False,
        screen_gate=str(gate))
    monkeypatch.setattr(module, "current_git_head", lambda: "abc")
    monkeypatch.setattr(
        module, "recompute_screen_gate",
        lambda payload, manifest_path, screen_path, **kwargs: dict(payload))
    authorization = module.validate_screen_authorization(
        args, [transport_job(module, tmp_path)], MANIFEST_PATH)
    assert authorization["sha256"] == module.file_sha256(gate)
    assert authorization["screen_config_sha256"] == module.file_sha256(
        CONFIG_PATH)

    for mutation in (
            {"decision": "STOP"},
            {"valid": False},
            {"campaign": "reuse_plan_literature_scale_campaign"},
            {"git_head": "stale"},
            {"manifest_sha256": "stale"},
            {"screen_config_sha256": "stale"},
            {"stage_rows": {"96_gem5_transport_i1": 12}},
            {"row_count": 14},
    ):
        stale = dict(payload)
        stale.update(mutation)
        gate.write_text(json.dumps(stale))
        with pytest.raises(SystemExit, match="stale or not GO"):
            module.validate_screen_authorization(
                args, [transport_job(module, tmp_path)], MANIFEST_PATH)


def test_transport_full_roles_reject_stale_recomputation(
        tmp_path, monkeypatch):
    module = load_experiment_run()
    payload = transport_gate_payload(module)
    gate = tmp_path / "transport_gate.json"
    gate.write_text(json.dumps(payload))
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False,
        screen_gate=str(gate))
    monkeypatch.setattr(module, "current_git_head", lambda: "abc")

    def recomputed(payload, manifest_path, screen_path, **kwargs):
        stale = dict(payload)
        stale["decision"] = "STOP"
        return stale

    monkeypatch.setattr(module, "recompute_screen_gate", recomputed)
    with pytest.raises(SystemExit, match="stale or not GO"):
        module.validate_screen_authorization(
            args, [transport_job(module, tmp_path)], MANIFEST_PATH)


def test_transport_full_roles_require_a_screen_gate_argument(tmp_path):
    module = load_experiment_run()
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False, screen_gate="")
    with pytest.raises(SystemExit, match="require --screen-gate"):
        module.validate_screen_authorization(
            args, [transport_job(module, tmp_path)], MANIFEST_PATH)


def test_transport_and_literature_full_roles_cannot_share_a_run(tmp_path):
    module = load_experiment_run()
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False, screen_gate="")
    jobs = [
        transport_job(module, tmp_path),
        transport_job(
            module, tmp_path,
            stage="92_cache_sim_literature_scale_wide16"),
    ]
    with pytest.raises(SystemExit, match="cannot share one run"):
        module.validate_screen_authorization(args, jobs, MANIFEST_PATH)


def test_transport_screen_stages_need_no_authorization(tmp_path):
    module = load_experiment_run()
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False, screen_gate="")
    jobs = [transport_job(module, tmp_path, stage="96_gem5_transport_i1")]
    assert module.validate_screen_authorization(
        args, jobs, MANIFEST_PATH) is None


def test_expected_transport_screen_rows_follow_the_manifest():
    module = load_experiment_run()
    assert module.expected_transport_screen_rows(MANIFEST, CONFIG) == {
        "60_gem5_proposal_reuse_bind_o3": 3,
        "96_gem5_transport_i1": 12,
    }


def test_transport_screen_config_carries_no_popt_settings():
    module = load_experiment_run()
    settings, graphs = module.apply_screen_config({
        "screen_config":
            "scripts/experiments/ecg/configs/"
            "transport_literature_scale.json",
        "screen_iteration": 1,
        "timeout_gem5": 100,
    })
    assert graphs is not None and len(graphs) == 6
    assert settings["policies"] == [
        "LRU", "ECG:REUSE_PLAN_LRU_FLOWTHROUGH"]
    assert settings["ecg_epochs"] == 16
    assert settings["gem5_cpu_type"] == "O3"
    assert settings["env"]["ECG_RECORD_TIER_BITS"] == "2"
    assert not any(key.startswith("popt_") for key in settings)


def test_literature_screen_config_still_carries_popt_settings():
    module = load_experiment_run()
    settings, graphs = module.apply_screen_config({
        "screen_config":
            "scripts/experiments/ecg/configs/"
            "pagerank_literature_scale.json",
        "screen_iteration": 1,
        "timeout_gem5": 100,
    })
    pagerank = json.loads(PAGERANK_CONFIG_PATH.read_text())
    assert graphs is not None and len(graphs) == 6
    assert settings["popt_reserve_model"] == "size_correct"
    assert settings["popt_property_bytes"] == (
        pagerank["popt_model"]["property_bytes"])
    assert settings["popt_active_columns"] == (
        pagerank["popt_model"]["reserved_column_slots"])
    assert settings["popt_num_epochs"] == pagerank["popt_model"]["epochs"]
    assert settings["popt_min_data_ways"] == (
        pagerank["popt_model"]["minimum_data_ways"])
    assert settings["popt_matrix_stream"] == "analytic"
