import csv
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import pytest

from scripts.experiments.ecg import roi_matrix


ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = (
    ROOT / "scripts/experiments/ecg/configs/pagerank_study.json")
GATE_PATH = (
    ROOT / "scripts/experiments/ecg/analysis/pagerank_gate.py")
EXPERIMENT_RUN_PATH = (
    ROOT / "scripts/experiments/ecg/flows/experiment_run.py")
MANIFEST_PATH = (
    ROOT / "scripts/experiments/ecg/experiment_manifest.json")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def config(path=CONFIG_PATH):
    return json.loads(path.read_text())


def gate():
    return load_module("pagerank_gate_test", GATE_PATH)


def expected_popt(cfg, graph, iterations):
    model = cfg["popt_model"]
    line_size = int(graph["line_size"])
    lines = (
        int(graph["vertices"]) * int(model["property_bytes"]) +
        line_size - 1) // line_size
    matrix_bytes = int(model["reserved_column_slots"]) * lines
    bytes_per_way = (
        gate().parse_size(graph["l3_size"]) //
        int(graph["l3_ways"]))
    reserved_ways = (matrix_bytes + bytes_per_way - 1) // bytes_per_way
    stream_bytes_per_iteration = int(model["epochs"]) * lines
    stream_requests_per_iteration = (
        stream_bytes_per_iteration + line_size - 1) // line_size
    target_stream_bytes = (
        stream_requests_per_iteration * iterations * line_size)
    return (
        matrix_bytes, reserved_ways,
        stream_bytes_per_iteration, target_stream_bytes)


def synthetic_rows(primary_ratio=0.94, cfg=None):
    cfg = cfg or config()
    mod = gate()
    roles = mod.policy_roles(cfg)
    rows = []
    for graph in cfg["graphs"]:
        for iterations in cfg["iterations"]:
            (matrix_bytes, reserved_ways, stream_bytes_per_iteration,
             target_stream_bytes) = expected_popt(
                cfg, graph, iterations)
            ordinary = target_stream_bytes * 2 + 1000
            metrics = {
                "LRU": (110.0, ordinary),
                "GRASP": (100.0, ordinary),
                "POPT": (90.0, ordinary + target_stream_bytes),
                "POPT_UNCHARGED": (85.0, ordinary),
                roles["transport"]: (
                    105.0, ordinary * 1.01),
                roles["primary"]: (
                    90.0 * primary_ratio, ordinary * 1.01),
                roles["characterization"][0]: (
                    90.0 * (primary_ratio + 0.01), ordinary * 1.015),
            }
            for policy, (ticks, traffic) in metrics.items():
                row = {
                    "status": "ok",
                    "final_output_status": "ok",
                    "final_graph": graph["name"],
                    "final_job_id": (
                        f"job-{graph['name']}-i{iterations}"),
                    "benchmark": "pr",
                    "options": cfg["options_template"].format(
                        graph_path=str((ROOT / graph["path"]).resolve()),
                        iterations=iterations),
                    "policy_label": policy,
                    "timing_valid_for_speedup": "1",
                    "timing_model": "simulated_target_time",
                    "timing_caveat": "",
                    "simulator": "gem5",
                    "gem5_cpu_type": "O3",
                    "prefetcher": "none",
                    "pr_result_matched": "1",
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
                    "popt_effective_l3_size": graph["l3_size"],
                    "popt_effective_l3_ways": str(graph["l3_ways"]),
                    "popt_reserved_ways": "0",
                    "proposal_path_active": "0",
                    "ecg_reuse_plan_depth": "0",
                    "graph_edge_bytes": "4",
                    "edge_stream_bytes_per_edge": "4",
                    "ecg_record_replaces_edge": "0",
                    "pr_iterations": str(iterations),
                    "pr_semantic_edges": str(
                        graph["semantic_receipts"][str(iterations)]["edges"]),
                    "pr_score_checksum":
                        graph["semantic_receipts"][str(iterations)]["checksum"],
                    "sim_ticks": str(ticks),
                    "dram_offchip_bytes": str(traffic),
                    "l3_misses": "100",
                    "roi_insts": "1000",
                    "ipc": "1.0",
                    "dram_bus_util_pct": "1.0",
                }
                if policy == "GRASP":
                    row.update({
                        "grasp_context_loaded": "1",
                        "grasp_regions_loaded": "2",
                        "grasp_hot_property_accesses": "100",
                    })
                elif policy == "POPT":
                    row.update({
                        "popt_overhead_charged": "1",
                        "popt_reserve_model": "size_correct",
                        "popt_matrix_fits": "1",
                        "popt_property_bytes":
                            str(cfg["popt_model"]["property_bytes"]),
                        "popt_matrix_active_columns":
                            str(cfg["popt_model"]["reserved_column_slots"]),
                        "popt_num_epochs":
                            str(cfg["popt_model"]["epochs"]),
                        "popt_min_data_ways":
                            str(cfg["popt_model"]["minimum_data_ways"]),
                        "popt_reload_each_iteration": "1",
                        "popt_initial_columns_charged": "1",
                        "popt_target_time_charged": "0",
                        "popt_timing_optimistic": "1",
                        "timing_model":
                            "optimistic_popt_analytic_stream",
                        "timing_caveat":
                            "Matrix-stream latency is omitted; timing "
                            "therefore favors P-OPT.",
                        "popt_matrix_stream_mode": "analytic_cumulative",
                        "popt_offchip_includes_matrix_stream": "1",
                        "popt_policy_active": "1",
                        "popt_context_loaded": "1",
                        "popt_rereference_loaded": "1",
                        "popt_runtime_epochs":
                            str(cfg["popt_model"]["epochs"]),
                        "popt_runtime_cache_lines": str(
                            stream_bytes_per_iteration //
                            int(cfg["popt_model"]["epochs"])),
                        "popt_roi_rereference_queries": "100",
                        "popt_matrix_bytes": str(matrix_bytes),
                        "popt_reserved_ways": str(reserved_ways),
                        "popt_effective_l3_ways": str(
                            int(graph["l3_ways"]) - reserved_ways),
                        "popt_effective_l3_size": str(
                            mod.parse_size(graph["l3_size"]) *
                            (int(graph["l3_ways"]) - reserved_ways) //
                            int(graph["l3_ways"])),
                        "l3_effective_ways": str(
                            int(graph["l3_ways"]) - reserved_ways),
                        "l3_effective_size": str(
                            mod.parse_size(graph["l3_size"]) *
                            (int(graph["l3_ways"]) - reserved_ways) //
                            int(graph["l3_ways"])),
                        "gem5_l3_ways_actual": str(
                            int(graph["l3_ways"]) - reserved_ways),
                        "gem5_l3_size_actual": str(
                            mod.parse_size(graph["l3_size"]) *
                            (int(graph["l3_ways"]) - reserved_ways) //
                            int(graph["l3_ways"])),
                        "popt_matrix_stream_bytes":
                            str(stream_bytes_per_iteration),
                        "popt_cumulative_stream_bytes":
                            str(target_stream_bytes),
                        "popt_matrix_stream_iterations": str(iterations),
                        "popt_matrix_stream_requests": str(
                            target_stream_bytes // int(graph["line_size"])),
                        "popt_dram_offchip_bytes_without_matrix_stream":
                            str(ordinary),
                        "popt_matrix_stream_dram_bytes":
                            str(target_stream_bytes),
                        "popt_stream_requestor_dram_bytes":
                            str(target_stream_bytes),
                    })
                elif policy == roles["oracle"]:
                    row.update({
                        "popt_overhead_charged": "0",
                        "popt_reserved_ways": "0",
                        "popt_target_time_charged": "0",
                        "popt_matrix_stream_mode": "none",
                        "popt_matrix_stream_bytes": "0",
                        "popt_matrix_stream_requests": "0",
                        "popt_cumulative_stream_bytes": "0",
                        "popt_matrix_stream_dram_bytes": "0",
                        "popt_stream_requestor_dram_bytes": "0",
                        "popt_dram_offchip_bytes_without_matrix_stream":
                            str(ordinary),
                        "popt_nonstream_requestor_dram_bytes": str(ordinary),
                        "popt_effective_l3_ways": str(graph["l3_ways"]),
                        "popt_effective_l3_size": graph["l3_size"],
                        "popt_policy_active": "1",
                        "popt_context_loaded": "1",
                        "popt_rereference_loaded": "1",
                        "popt_runtime_epochs":
                            str(cfg["popt_model"]["epochs"]),
                        "popt_runtime_cache_lines": str(
                            stream_bytes_per_iteration //
                            int(cfg["popt_model"]["epochs"])),
                        "popt_roi_rereference_queries": "100",
                    })
                elif policy.startswith("ECG_REUSE_PLAN_"):
                    receipt = cfg["variant_receipts"][policy]
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
                        "l3_effective_ways": str(graph["l3_ways"]),
                        "l3_effective_size": graph["l3_size"],
                        "ecg_isa_variant": cfg["isa_variant"],
                        "ecg_epochs": str(cfg["reuse_plan_epochs"]),
                        "gem5_reuse_bind_trace_limit": "0",
                        "gem5_flowthrough_trace_limit": "0",
                        "proposal_compact_id_bits":
                            str(graph["compact_id_bits"]),
                        "proposal_compact_epoch_bits":
                            str(graph["compact_epoch_bits"]),
                        "proposal_compact_tier_bits":
                            str(cfg["compact_tier_bits"]),
                        "gem5_variant_requested_receipt":
                            receipt["requested"],
                        "gem5_variant_effective_receipt":
                            str(receipt["effective"]),
                        "gem5_variant_dueling_receipt":
                            str(receipt["dueling"]),
                    })
                    if int(receipt["dueling"]) == 1:
                        row.update({
                            "gem5_reuse_plan_dueling_request_bound_victims": "100",
                            "gem5_reuse_plan_dueling_leader_samples": "2048",
                            "gem5_reuse_plan_dueling_follower_selections": "90",
                            "gem5_reuse_plan_dueling_completed_windows": "2",
                            "gem5_reuse_plan_dueling_winner_changes": "0",
                            "gem5_reuse_plan_dueling_follower_variant_overrides": "0",
                        })
                rows.append(row)
    return rows


def test_preregistration_is_compact_and_has_no_hash_qualification():
    cfg = config()
    text = CONFIG_PATH.read_text()
    assert len(text.splitlines()) < 300
    assert "sha256" not in text.lower()
    assert len(cfg["graphs"]) == 3
    assert cfg["iterations"] == [1, 2, 4, 8]
    assert len(cfg["policies"]["all"]) == 7
    assert cfg["policies"]["primary_candidate"] == (
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH")
    assert cfg["compact_tier_bits"] == 2
    assert "outcome" not in cfg["execution"]
    assert "superseded_screens" not in cfg
    # No v1/v2-suffixed id: this is the single active preregistration.
    assert cfg["id"] == "pagerank_study"
    assert "_v1" not in cfg["id"] and "_v2" not in cfg["id"]


def test_public_methodology_matches_experiment_configuration():
    cfg = config()
    text = (
        ROOT / "wiki/Evaluation-Methodology.md"
    ).read_text()
    flat = " ".join(text.split())
    by_name = {graph["name"]: graph for graph in cfg["graphs"]}
    assert "Iteration counts are 1, 2, 4, and 8" in text
    assert "pagerank_study.json" in text
    for name, vertices in (
            ("web-Google-n16", 65536),
            ("soc-pokec-n16", 65536),
            ("cit-Patents-n18-sym", 262144)):
        assert by_name[name]["vertices"] == vertices
    assert "web-Google, soc-pokec, and cit-Patents" in flat


def test_pagerank_configurations_are_explicitly_scoped():
    cfg = config()
    assert "superseded_screens" not in cfg
    assert "outcome" not in cfg["execution"]
    config_dir = ROOT / "scripts/experiments/ecg/configs"
    files = sorted(p.name for p in config_dir.glob("*pagerank*.json"))
    assert files == [
        "pagerank_literature_scale.json",
        "pagerank_study.json",
    ]
    for name in files:
        assert "_v1" not in name and "_v2" not in name
    result = gate().evaluate(synthetic_rows(cfg=cfg), cfg)
    assert result["primary_candidate"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"
    assert result["screen_passes"] is True


def test_csr_transport_calibration_is_one_complete_bounded_cell():
    manifest = json.loads(MANIFEST_PATH.read_text())
    stages = [
        stage for stage in manifest["stages"]
        if "reuse_plan_csr_transport_calibration" in
        stage.get("profiles", [])
    ]
    assert len(stages) == 1
    stage = stages[0]
    assert stage["name"] == "61_gem5_csr_transport_cit_i1"
    assert stage["suite"] == "gem5"
    assert stage["graph_set"] == "cit_patents_n18_transport_calibration"
    assert stage["benchmarks"] == ["pr"]
    assert stage["policies"] == [
        "LRU",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
    ]
    assert stage["policy_sharding_allowed"] is False
    assert stage["gem5_cpu_type"] == "O3"
    assert stage["gem5_compact_reuse_bind_performance"] is True
    assert stage["prefetcher"] == "none"
    assert stage["ecg_epochs"] == 32
    assert stage["env"]["GEM5_GRAPH_ARRAY_STATS"] == "1"
    assert stage["env"]["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
    assert "at or below 2%" in stage["notes"]
    assert roi_matrix.GEM5_ARRAY_OTHER_MISS_SHARE_LIMIT == 0.02

    graphs = manifest["graph_sets"][
        "cit_patents_n18_transport_calibration"]
    assert graphs == [{
        "name": "cit-Patents-n18-sym",
        "path": "results/graphs/cit-Patents-n18/cit-Patents-n18-sym.sg",
        "options_key": "file_pr_i1_dbg",
        "l1d_size": "32kB",
        "l2_size": "128kB",
        "l3_ways": "16",
        "l3_sizes": ["512kB"],
        "structure_prefetch_degree": 0,
    }]
    assert manifest["benchmark_options"]["file_pr_i1_dbg"]["pr"].endswith(
        "-o 5 -n 1 -i 1")


def test_transport_matched_topt_trace_is_fail_closed(tmp_path):
    log = tmp_path / "trace.log"
    log.write_text(
        "[T_OPT] L3 FlowThrough-aware Belady "
        "(warm-start ROI window): accesses=100 "
        "hits=70 misses=30 sets=16 ways=8 miss_rate=0.3\n"
        "[T_OPT-TRACE accesses=100 property_accesses=60 "
        "hash=abc123 line_hash=def456 sets=16 ways=8]\n")
    parsed = roi_matrix.parse_ecg_log_stats(log)
    assert parsed == {
        "topt_trace_accesses": 100,
        "topt_trace_property_accesses": 60,
        "topt_trace_hash": "abc123",
        "topt_trace_line_hash": "def456",
        "topt_trace_sets": 16,
        "topt_trace_ways": 8,
        "topt_hits": 70,
        "topt_misses": 30,
        "topt_miss_rate": 0.3,
    }

    common = {
        "simulator": "cache_sim",
        "benchmark": "pr",
        "options": "-g 8 -i 1",
        "l3_size": "32kB",
        "l3_ways": "8",
        "ecg_reuse_plan_depth": 2,
        "ecg_flowthrough": 1,
        "ecg_record_replaces_edge": 1,
        "ecg_record_bytes": 4,
        "prefetcher": "none",
        "topt_trace_requested": 1,
        "topt_trace_accesses": 100,
        "topt_trace_property_accesses": 60,
        "topt_trace_hash": "abc123",
        "topt_trace_line_hash": "def456",
        "topt_trace_sets": 16,
        "topt_trace_ways": 8,
        "topt_misses": 30,
        "l3_hits": 70,
        "l3_misses": 30,
        "timing_valid_for_speedup": "1",
    }
    policy_texts = [
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_GRASP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_DEGREE_FLOWTHROUGH",
        "ECG:REUSE_PLAN_EPOCH_FLOWTHROUGH",
        "ECG:REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH",
    ]
    policies = [
        roi_matrix.parse_policy_spec(text) for text in policy_texts]
    rows = [
        {**common, "policy_label": policy.label}
        for policy in policies
    ]
    roi_matrix.certify_cache_sim_trace_identity(
        rows, SimpleNamespace(suite="cache-sim"), policies)
    assert all(row["topt_trace_matched"] == 1 for row in rows)

    rows[1]["topt_trace_hash"] = "different"
    roi_matrix.certify_cache_sim_trace_identity(
        rows, SimpleNamespace(suite="cache-sim"), policies)
    assert all(row["status"] == "error" for row in rows)

    missing = [
        {
            **common,
            "policy_label": policy.label,
            "topt_trace_hash": "",
        }
        for policy in policies
    ]
    roi_matrix.certify_cache_sim_trace_identity(
        missing, SimpleNamespace(suite="cache-sim"), policies)
    assert all(row["status"] == "error" for row in missing)
    assert all(
        "receipts are missing" in row["error"] for row in missing)

    partial_rows = rows[:2]
    partial_policies = policies[:2]
    for row in partial_rows:
        row.pop("status", None)
        row.pop("error", None)
        row["topt_trace_hash"] = "abc123"
    roi_matrix.certify_cache_sim_trace_identity(
        partial_rows, SimpleNamespace(suite="cache-sim"),
        partial_policies)
    assert all(row["status"] == "error" for row in partial_rows)


def test_matched_policy_replay_is_one_unsharded_cell():
    manifest = json.loads(MANIFEST_PATH.read_text())
    stages = [
        stage for stage in manifest["stages"]
        if "reuse_plan_matched_policy_replay" in
        stage.get("profiles", [])
    ]
    assert len(stages) == 1
    stage = stages[0]
    assert stage["name"] == "62_cache_sim_matched_policy_replay"
    assert stage["suite"] == "cache-sim"
    assert stage["graph_set"] == "cit_patents_n18_transport_calibration"
    assert stage["benchmarks"] == ["pr"]
    assert stage["policy_sharding_allowed"] is False
    assert stage["prefetcher"] == "none"
    assert stage["ecg_epochs"] == 32
    assert stage["env"]["T_OPT"] == "1"
    assert stage["env"]["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
    assert stage["policies"] == [
        "LRU",
        "GRASP",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_GRASP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_DEGREE_FLOWTHROUGH",
        "ECG:REUSE_PLAN_EPOCH_FLOWTHROUGH",
        "ECG:REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH",
    ]


def test_shortcircuit_timing_is_one_replay_promoted_cell():
    manifest = json.loads(MANIFEST_PATH.read_text())
    stages = [
        stage for stage in manifest["stages"]
        if "reuse_plan_shortcircuit_timing" in
        stage.get("profiles", [])
    ]
    assert len(stages) == 1
    stage = stages[0]
    assert stage["name"] == "63_gem5_shortcircuit_cit_i1"
    assert stage["suite"] == "gem5"
    assert stage["graph_set"] == "cit_patents_n18_transport_calibration"
    assert stage["benchmarks"] == ["pr"]
    assert stage["policy_sharding_allowed"] is False
    assert stage["gem5_cpu_type"] == "O3"
    assert stage["gem5_compact_reuse_bind_performance"] is True
    assert stage["policies"] == [
        "LRU",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH",
    ]
    assert stage["env"]["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
    assert stage["env"]["GEM5_GRAPH_ARRAY_STATS"] == "1"


def test_cache_sim_mode_receipt_survives_graph_context_lifetime():
    cache = (
        ROOT / "bench/include/cache_sim/cache_sim.h").read_text()
    assert "ECGMode ecg_mode_snapshot_" in cache
    assert "ecg_mode_snapshot_ = ctx->mask_config.ecg_mode" in cache
    assert "return ECGModeToString(ecg_mode_snapshot_);" in cache
    for kernel in (
            "pr", "pr_spmv", "bfs", "bc", "cc", "cc_sv", "sssp", "tc"):
        source = (ROOT / f"bench/src_sim/{kernel}.cc").read_text()
        assert source.count("cache.initGraphContext(&graph_ctx);") >= 2
        assert source.index("graph_ctx.initMaskConfig();") < source.rindex(
            "cache.initGraphContext(&graph_ctx);")


def test_final_campaign_is_role_separated():
    manifest = json.loads(MANIFEST_PATH.read_text())
    assert "reuse_plan_final_campaign" in manifest["profiles"]
    stages = [
        stage for stage in manifest["stages"]
        if "reuse_plan_final_campaign" in stage.get("profiles", [])
    ]
    assert len(stages) == 11
    by_name = {stage["name"]: stage for stage in stages}

    mechanism = by_name["60_gem5_proposal_reuse_bind_o3"]
    assert mechanism["gem5_cpu_type"] == "O3"
    assert mechanism["ecg_isa_variant"] == "computed"

    timing = [
        by_name[f"{number}_gem5_pagerank_i{iteration}"]
        for number, iteration in ((70, 1), (71, 2), (72, 4), (73, 8))
    ]
    assert all(
        stage["screen_config"] ==
        "scripts/experiments/ecg/configs/pagerank_study.json"
        for stage in timing)

    functional = by_name["80_cache_sim_final_fullgraph"]
    assert functional["suite"] == "cache-sim"
    assert functional["graph_set"] == "factorial_graphs_uniform_8mb"
    assert functional["benchmarks"] == ["pr", "bfs", "bc", "cc"]
    assert functional["ecg_epochs"] == 16
    assert functional["ecg_isa_variant"] == "computed"
    assert functional["policy_sharding_allowed"] is False
    assert functional["env"] == {
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
    }
    assert functional["policies"] == [
        "LRU", "GRASP",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ]

    scale = by_name["81_sniper_final_semantic"]
    assert scale["suite"] == "sniper"
    assert scale["graph_set"] == "factorial_graphs_uniform_8mb"
    assert scale["benchmarks"] == ["pr", "bfs", "bc", "cc"]
    assert scale["ecg_isa_variant"] == "computed"
    assert scale["ecg_epochs"] == 16
    assert scale["sniper_queue_model"] == "windowed_mg1"
    assert scale["policy_sharding_allowed"] is False
    assert scale["env"] == {
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
        "ECG_REUSE_PLAN_DELIVERY_TRACE": "32",
    }
    assert "POPT" not in scale["policies"]
    assert scale["policies"] == [
        "LRU", "GRASP",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ]
    final_graphs = manifest["graph_sets"][
        "factorial_graphs_uniform_8mb"]
    assert {
        graph["name"]: (
            graph["compact_id_bits"],
            graph["compact_epoch_bits"],
            graph["compact_total_bits"],
            graph["sniper_semantic_edge_limit"],
        )
        for graph in final_graphs
    } == {
        "web-Google": (20, 4, 30, 8644102),
        "soc-pokec": (21, 4, 31, 44603928),
        "cit-Patents": (22, 4, 32, 33037894),
    }
    assert all(
        graph["compact_total_bits"] ==
        graph["compact_id_bits"] +
        2 * graph["compact_epoch_bits"] + 2
        for graph in final_graphs)
    assert all(
        graph["compact_total_bits"] <= 32
        for graph in final_graphs)
    assert all(
        graph["sniper_semantic_edge_source"] ==
        "symmetrized .sg serialized edge count"
        for graph in final_graphs)

    wide16 = by_name["82_cache_sim_final_wide16"]
    assert wide16["ecg_epochs"] == 16
    assert wide16["env"] == {
        "ECG_EDGE_RECORD_BYTES": "8",
        "ECG_EXPECT_BYTES_PER_EDGE": "8",
    }
    assert wide16["policies"] == [
        "LRU",
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
    ]

    wide256 = by_name["83_cache_sim_final_wide256"]
    assert wide256["ecg_epochs"] == 256
    assert wide256["env"] == wide16["env"]
    assert wide256["policies"] == wide16["policies"]

    popt = by_name["84_cache_sim_final_popt"]
    assert popt["benchmarks"] == ["pr", "cc"]
    assert popt["popt_matrix_stream"] == "simulated"
    assert popt["popt_property_bytes"] == 4
    assert popt["popt_active_columns"] == 2
    assert popt["popt_num_epochs"] == 256
    assert popt["popt_min_data_ways"] == 1
    assert popt["ecg_epochs"] == 16
    assert popt["env"] == functional["env"]

    sniper_sssp = by_name["85_sniper_final_sssp_wide"]
    assert sniper_sssp["benchmarks"] == ["sssp"]
    assert sniper_sssp["ecg_epochs"] == 16
    assert sniper_sssp["sniper_queue_model"] == "windowed_mg1"
    assert sniper_sssp["env"] == {
        **wide16["env"],
        "ECG_REUSE_PLAN_DELIVERY_TRACE": "32",
    }
    assert sniper_sssp["policies"] == scale["policies"]

    assert all(
        stage.get("policy_sharding_allowed", False) is False
        for stage in (
            mechanism, functional, scale, wide16, wide256, popt,
            sniper_sssp))

    for name in (
            "23_gem5_3sim_realgraph_allalg",
            "28_gem5_3sim_sampled_allalg"):
        assert "Legacy non-O3" in next(
            stage for stage in manifest["stages"]
            if stage["name"] == name)["blocked_reason"]


def test_weighted_sssp_is_excluded_from_compact_four_byte_stages():
    manifest = json.loads(MANIFEST_PATH.read_text())
    by_name = {stage["name"]: stage for stage in manifest["stages"]}
    for name in (
            "80_cache_sim_final_fullgraph",
            "81_sniper_final_semantic"):
        assert "sssp" not in by_name[name]["benchmarks"]
        assert by_name[name]["env"]["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
    assert by_name["85_sniper_final_sssp_wide"]["benchmarks"] == ["sssp"]
    assert (
        by_name["85_sniper_final_sssp_wide"]["env"]
        ["ECG_EXPECT_BYTES_PER_EDGE"] == "8")

    source = (ROOT / "bench/src_sim/sssp.cc").read_text()
    declare = source.index(
        "::ecg_metadata::declareContainerBytes(ecg_meta, 8)")
    enforce = source.index(
        "::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, \"sssp\")")
    assert declare < enforce


def test_sniper_unweighted_reuse_plan_uses_variable_width_pair_streams():
    source = (ROOT / "bench/src_sniper/sg_kernel.cc").read_text()
    assert "struct ReusePlanPairStream" in source
    for kernel in ("bfs", "bc", "cc"):
        assert (
            f'/*push_out_edges=*/true, "{kernel}", ' in source)
    assert source.count("pairs.record(pos)") >= 4
    assert "bfs_pairs.record(pos)" in source
    assert "runtime-receipted" in (
        ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()


def test_final_campaign_expands_to_76_jobs(tmp_path):
    listed = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "reuse_plan_final_campaign",
            "--run-dir", str(tmp_path / "final-campaign"),
            "--list", "--dry-run", "--no-build", "--no-resume",
            "--allow-missing-graphs",
            "--allow-missing-runtime-inputs",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert listed.returncode == 0, listed.stdout + listed.stderr
    jobs = [
        line for line in listed.stdout.splitlines()
        if re.match(r"^\d{3} ", line)
    ]
    assert len(jobs) == 76
    assert sum("80_cache_sim_final_fullgraph" in line for line in jobs) == 12
    assert sum("81_sniper_final_semantic" in line for line in jobs) == 12
    assert sum("82_cache_sim_final_wide16" in line for line in jobs) == 15
    assert sum("83_cache_sim_final_wide256" in line for line in jobs) == 15
    assert sum("84_cache_sim_final_popt" in line for line in jobs) == 6
    assert sum("85_sniper_final_sssp_wide" in line for line in jobs) == 3
    assert listed.stdout.count("--gem5-compact-reuse-bind-performance") == 12
    assert listed.stdout.count("--popt-matrix-stream simulated") == 6
    assert listed.stdout.count(
        "--sniper-semantic-edge-limit 8644102") == 5
    assert listed.stdout.count(
        "--sniper-semantic-edge-limit 44603928") == 5
    assert listed.stdout.count(
        "--sniper-semantic-edge-limit 33037894") == 5
    assert listed.stdout.count("--ecg-epochs 16") == 48
    assert listed.stdout.count("--ecg-epochs 256") == 15


def test_final_campaign_rejects_policy_sharding(tmp_path):
    filtered = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "reuse_plan_final_campaign",
            "--run-dir", str(tmp_path / "filtered"),
            "--only", "80_cache_sim_final_fullgraph",
            "--policy", "LRU",
            "--dry-run", "--no-build", "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert filtered.returncode != 0
    assert "complete policy roster" in (
        filtered.stdout + filtered.stderr)

    shards = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "reuse_plan_final_campaign",
            "--run-tag", "final-test",
            "--out", str(tmp_path / "shards.tsv"),
            "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert shards.returncode != 0
    assert "whole-cell jobs" in (shards.stdout + shards.stderr)


def test_legacy_diagnostic_requires_explicit_allow_blocked(tmp_path):
    base = [
        sys.executable,
        "scripts/experiments/ecg/flows/experiment_run.py",
        "--profile", "ecg_3sim_sampled_allalg",
        "--run-dir", str(tmp_path / "legacy"),
        "--only", "28_gem5_3sim_sampled_allalg",
        "--no-build", "--allow-missing-graphs",
        "--allow-missing-runtime-inputs",
    ]
    blocked = subprocess.run(
        base, cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert blocked.returncode != 0
    assert "Legacy non-O3" in (blocked.stdout + blocked.stderr)

    allowed = subprocess.run(
        [*base, "--allow-blocked", "--dry-run"],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert allowed.returncode == 0, allowed.stdout + allowed.stderr


def test_underscaled_final_profile_is_execution_blocked(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "reuse_plan_final_campaign",
            "--run-dir", str(tmp_path / "blocked-final"),
            "--no-build",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert result.returncode != 0
    output = result.stdout + result.stderr
    assert "profile reuse_plan_final_campaign is blocked" in output
    assert "GRASP/P-OPT evaluation scale" in output


def test_profile_expands_to_twelve_whole_cells(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "reuse_plan_pagerank_study",
            "--run-dir", str(tmp_path / "run"),
            "--list", "--dry-run", "--no-build",
            "--allow-missing-graphs",
            "--allow-missing-runtime-inputs",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=120)
    text = result.stdout + result.stderr
    assert result.returncode == 0, text
    assert "jobs=12" in text
    for iterations in (1, 2, 4, 8):
        assert text.count(f"-i {iterations} -t 0'") == 3
    assert text.count(
        "--policies LRU GRASP POPT POPT:UNCHARGED "
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH ECG:REUSE_PLAN_RRIP_FLOWTHROUGH "
        "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH") == 12
    assert text.count("--popt-active-columns 2") == 12
    assert text.count("--popt-matrix-stream analytic") == 12
    assert text.count("--timeout-gem5 86400") == 12
    assert text.count("--gem5-compact-reuse-bind-performance") == 12

    runner = load_module(
        "proposal_sota_experiment_run_test", EXPERIMENT_RUN_PATH)
    manifest = json.loads(MANIFEST_PATH.read_text())
    stage = next(
        value for value in manifest["stages"]
        if value["name"] == "70_gem5_pagerank_i1")
    settings, _ = runner.apply_screen_config(
        runner.merged_defaults(manifest, stage))
    assert settings["env"]["ECG_RECORD_VARIABLE_WIDTH"] == "1"
    assert settings["env"]["ECG_RECORD_TIER_BITS"] == str(
        config()["compact_tier_bits"])


def test_no_v1_v2_screen_profile_or_stage_is_active():
    manifest = json.loads(MANIFEST_PATH.read_text())
    profile_names = set(manifest["profiles"])
    stage_names = {stage["name"] for stage in manifest["stages"]}
    assert "reuse_plan_pagerank_study" in profile_names
    for name in profile_names | stage_names:
        assert "proposal_reuse_bind_sota_pr_screen" not in name
        assert not name.endswith("_v1") and not name.endswith("_v2")
    screen_stages = [
        stage for stage in manifest["stages"]
        if "reuse_plan_pagerank_study" in stage.get("profiles", [])
    ]
    assert len(screen_stages) == 4
    for stage in screen_stages:
        assert stage["screen_config"] == (
            "scripts/experiments/ecg/configs/pagerank_study.json")
    # Exactly one active proposal-screen profile.
    screen_profiles = [
        name for name in profile_names if "pagerank_study" in name
    ]
    assert screen_profiles == ["reuse_plan_pagerank_study"]


def test_popt_model_matches_roi_matrix_producer():
    cfg = config()
    mod = gate()
    roi = load_module(
        "proposal_sota_roi_matrix_test",
        ROOT / "scripts/experiments/ecg/roi_matrix.py")
    spec = roi.parse_policy_spec("POPT")
    for graph in cfg["graphs"]:
        vertices = int(graph["vertices"])
        scale = vertices.bit_length() - 1
        assert 1 << scale == vertices
        args = SimpleNamespace(
            options=f"-g {scale} -i 1",
            line_size=str(graph["line_size"]),
            l3_ways=str(graph["l3_ways"]),
            popt_property_bytes=str(cfg["popt_model"]["property_bytes"]),
            popt_active_columns=str(
                cfg["popt_model"]["reserved_column_slots"]),
            popt_num_epochs=str(cfg["popt_model"]["epochs"]),
            popt_min_data_ways=str(
                cfg["popt_model"]["minimum_data_ways"]),
            popt_reserve_model="size_correct",
        )
        charge = roi.popt_charge_metadata(args, spec, graph["l3_size"])
        expected = mod.expected_popt(cfg, graph, 1)
        assert expected["reserved_ways"] == cfg["popt_model"][
            "expected_reserved_ways_in_screen"]
        assert expected["effective_ways"] == cfg["popt_model"][
            "expected_effective_data_ways_in_screen"]
        assert charge["popt_matrix_bytes"] == expected["matrix_bytes"]
        assert charge["popt_reserved_ways"] == expected["reserved_ways"]
        assert charge["popt_matrix_stream_bytes"] == (
            expected["stream_bytes_per_iteration"])


def test_policy_sharding_is_rejected(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "reuse_plan_pagerank_study",
            "--run-tag", "screen",
            "--out", str(tmp_path / "shards.tsv"),
            "--allow-blocked",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert result.returncode != 0
    assert "whole-cell jobs" in (result.stdout + result.stderr)

    local = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "reuse_plan_pagerank_study",
            "--policy", "LRU",
            "--run-dir", str(tmp_path / "policy-filter"),
            "--dry-run", "--no-build", "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert local.returncode != 0
    assert "complete policy roster" in (local.stdout + local.stderr)


def test_valid_screen_passes_and_reports_attribution():
    result = gate().evaluate(synthetic_rows(), config())
    primary = result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]
    online = result["candidates"]["ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH"]
    assert result["cell_count"] == 12
    assert result["row_count"] == 84
    assert result["screen_valid"] is True
    assert result["screen_result"] == "go"
    assert result["screen_passes"] is True
    assert result["stop_broad_campaign"] is False
    assert primary["passes"] is True
    assert primary["replacement_policy_contribution"] is True
    assert online["decision_role"] == "characterization_only"
    assert "POPT_UNCHARGED" in primary["comparisons"]
    assert len(result["popt_stream_accounting"]) == 12
    assert result["decision"] == config()["decision"]
    assert result["popt_model"] == config()["popt_model"]


def test_online_characterization_cannot_pass_screen_alone():
    rows = synthetic_rows()
    for row in rows:
        if row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH":
            row["sim_ticks"] = "100"
    result = gate().evaluate(rows, config())
    assert result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]["passes"] is False
    assert result["candidates"]["ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH"]["passes"] is True
    assert result["screen_passes"] is False


def test_baseline_activity_and_popt_accounting_fail_closed():
    rows = synthetic_rows()
    grasp = next(row for row in rows if row["policy_label"] == "GRASP")
    grasp["grasp_hot_property_accesses"] = "0"
    with pytest.raises(ValueError, match="must be positive"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    grasp = next(row for row in rows if row["policy_label"] == "GRASP")
    grasp["grasp_regions_loaded"] = "0"
    with pytest.raises(ValueError, match="must be positive"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    popt = next(row for row in rows if row["policy_label"] == "POPT")
    popt["popt_matrix_stream_dram_bytes"] = "0"
    with pytest.raises(
            ValueError, match="components|decomposition"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    popt = next(row for row in rows if row["policy_label"] == "POPT")
    popt["popt_matrix_stream_iterations"] = "99"
    with pytest.raises(ValueError, match="popt_matrix_stream_iterations"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    popt = next(row for row in rows if row["policy_label"] == "POPT")
    popt["popt_roi_rereference_queries"] = "0"
    with pytest.raises(ValueError, match="must be positive"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    oracle = next(
        row for row in rows
        if row["policy_label"] == "POPT_UNCHARGED")
    oracle["popt_policy_active"] = "0"
    with pytest.raises(ValueError, match="popt_policy_active"):
        gate().evaluate(rows, config())


def test_reuse_plan_performance_mode_and_online_dueling_fail_closed():
    rows = synthetic_rows()
    primary = next(
        row for row in rows
        if row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH")
    primary["proposal_performance_mode_active"] = "0"
    with pytest.raises(ValueError, match="proposal_performance_mode_active"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    online = next(
        row for row in rows
        if row["policy_label"] == "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH")
    online["gem5_variant_dueling_receipt"] = "0"
    with pytest.raises(ValueError, match="gem5_variant_dueling_receipt"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    online = next(
        row for row in rows
        if row["policy_label"] == "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH")
    online["gem5_reuse_plan_dueling_completed_windows"] = "0"
    with pytest.raises(ValueError, match="must be positive"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    online = next(
        row for row in rows
        if row["policy_label"] == "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH")
    online["gem5_reuse_plan_dueling_leader_samples"] = "1023"
    with pytest.raises(ValueError, match="full leader-sample window"):
        gate().evaluate(rows, config())


def test_incomplete_duplicate_and_extra_cells_are_rejected():
    rows = synthetic_rows()
    with pytest.raises(ValueError, match="incomplete policy roster"):
        gate().evaluate(rows[:-1], config())

    rows = synthetic_rows()
    rows.append(dict(rows[0]))
    with pytest.raises(ValueError, match="duplicate policy"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    rows[0]["final_graph"] = "other-graph"
    with pytest.raises(ValueError, match="outside screen graph set"):
        gate().evaluate(rows, config())


def test_per_cell_guard_prevents_masking():
    rows = synthetic_rows(primary_ratio=0.80)
    for row in rows:
        if (
                row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH" and
                row["final_graph"] == "web-Google-n16" and
                "-i 1" in row["options"]):
            row["sim_ticks"] = str(90.0 * 1.021)
    result = gate().evaluate(rows, config())
    assert result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]["passes"] is False
    assert result["screen_passes"] is False


def test_i8_guard_prevents_short_run_masking():
    rows = synthetic_rows(primary_ratio=0.80)
    for row in rows:
        if (
                row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH" and
                "-i 8" in row["options"]):
            row["sim_ticks"] = str(90.0 * 0.98)
    result = gate().evaluate(rows, config())
    assert result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]["passes"] is False


def test_leave_one_graph_out_guard_prevents_one_graph_masking():
    rows = synthetic_rows()
    for row in rows:
        if row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH":
            ratio = (
                0.70 if row["final_graph"] == "web-Google-n16"
                else 0.99)
            row["sim_ticks"] = str(90.0 * ratio)
    result = gate().evaluate(rows, config())
    assert result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]["passes"] is False


def test_oracle_sanity_is_checked_per_cell():
    rows = synthetic_rows()
    oracle = next(
        row for row in rows
        if (
            row["policy_label"] == "POPT_UNCHARGED" and
            row["final_graph"] == "web-Google-n16" and
            "-i 1" in row["options"]
        ))
    oracle["sim_ticks"] = "1000"
    result = gate().evaluate(rows, config())
    assert result["oracle_sanity_passes"] is False
    assert result["screen_valid"] is False
    assert result["screen_result"] == "inconclusive_invalid_oracle"
    assert result["screen_passes"] is False
    assert result["stop_broad_campaign"] is False


def test_invalid_baseline_is_inconclusive_not_stop():
    rows = synthetic_rows(primary_ratio=0.80)
    for row in rows:
        if row["policy_label"] == "GRASP":
            row["sim_ticks"] = "176"
    result = gate().evaluate(rows, config())
    assert result["baseline_sanity_passes"] is False
    assert result["screen_valid"] is False
    assert result["screen_result"] == "inconclusive_invalid_baselines"
    assert result["screen_passes"] is False
    assert result["stop_broad_campaign"] is False
    assert result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"][
        "performance_guards_pass"] is True


def test_faithfully_expensive_charged_popt_is_not_invalid():
    rows = synthetic_rows(primary_ratio=0.80)
    for row in rows:
        if row["policy_label"] == "POPT":
            row["sim_ticks"] = "176"
    result = gate().evaluate(rows, config())
    assert result["baseline_sanity"]["POPT"]["passes"] is True
    assert result["screen_valid"] is True


def test_transport_claim_has_leave_one_graph_out_guard():
    rows = synthetic_rows()
    candidate_ticks = 90.0 * 0.94
    ratios = {
        "web-Google-n16": 1.10,
        "soc-pokec-n16": 0.90,
        "cit-Patents-n18-sym": 0.90,
    }
    for row in rows:
        if row["policy_label"] == "ECG_REUSE_PLAN_LRU_FLOWTHROUGH":
            row["sim_ticks"] = str(
                candidate_ticks / ratios[row["final_graph"]])
    result = gate().evaluate(rows, config())
    primary = result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]
    assert primary["passes"] is True
    assert primary["comparisons"]["ECG_REUSE_PLAN_LRU_FLOWTHROUGH"][
        "aggregate_time_ratio"] <= 0.98
    assert primary["replacement_policy_contribution"] is False
    assert result["replacement_policy_claim_allowed"] is False


def test_transport_only_win_does_not_authorize_policy_claim():
    rows = synthetic_rows()
    for row in rows:
        if row["policy_label"] == "ECG_REUSE_PLAN_LRU_FLOWTHROUGH":
            row["sim_ticks"] = str(90.0 * 0.94)
    result = gate().evaluate(rows, config())
    primary = result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]
    assert result["screen_passes"] is True
    assert primary["claim_classification"] == (
        "complete_design_transport_or_layout_only")
    assert result["replacement_policy_claim_allowed"] is False


def test_replacement_claim_requires_per_cell_instruction_parity():
    rows = synthetic_rows()
    primary = next(
        row for row in rows
        if (
            row["policy_label"] == "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH" and
            row["final_graph"] == "web-Google-n16" and
            "-i 1" in row["options"]))
    primary["roi_insts"] = "1001"
    result = gate().evaluate(rows, config())
    candidate = result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]
    assert result["screen_passes"] is True
    assert candidate["replacement_instruction_parity"]["passes"] is False
    assert candidate["replacement_policy_contribution"] is False
    assert candidate["claim_classification"] == (
        "complete_design_transport_or_layout_only")
    assert result["replacement_policy_claim_allowed"] is False


def test_replacement_instruction_parity_rule_is_mandatory():
    cfg = config()
    cfg["decision"][
        "replacement_claim_requires_exact_roi_instruction_parity"] = False
    with pytest.raises(
            ValueError, match="exact ROI instruction parity"):
        gate().evaluate(synthetic_rows(cfg=cfg), cfg)


def test_stop_does_not_suppress_valid_replacement_attribution():
    result = gate().evaluate(
        synthetic_rows(primary_ratio=1.01), config())
    candidate = result["candidates"]["ECG_REUSE_PLAN_RRIP_FLOWTHROUGH"]
    assert result["screen_valid"] is True
    assert result["screen_result"] == "stop"
    assert result["screen_passes"] is False
    assert candidate["replacement_policy_contribution"] is True
    assert candidate["claim_classification"] == (
        "replacement_policy_supported_complete_design_failed")
    assert result["replacement_policy_claim_allowed"] is True


def test_wrong_semantics_or_geometry_is_rejected():
    rows = synthetic_rows()
    rows[0]["pr_score_checksum"] = "wrong"
    with pytest.raises(ValueError, match="checksum"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    rows[0]["l2_ways"] = "4"
    with pytest.raises(ValueError, match="failed l2_ways"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    lru = next(row for row in rows if row["policy_label"] == "LRU")
    lru["l3_effective_ways"] = "8"
    with pytest.raises(ValueError, match="l3_effective_ways"):
        gate().evaluate(rows, config())

    rows = synthetic_rows()
    popt = next(row for row in rows if row["policy_label"] == "POPT")
    popt["popt_effective_l3_size"] = "64kB"
    with pytest.raises(ValueError, match="popt_effective_l3_size"):
        gate().evaluate(rows, config())

    bad_config = config()
    bad_config["graphs"][0]["compact_epoch_bits"] = 4
    with pytest.raises(ValueError, match="too few compact epoch bits"):
        gate().evaluate(synthetic_rows(), bad_config)

    bad_config = config()
    bad_config["popt_model"]["expected_reserved_ways_in_screen"] = 2
    with pytest.raises(ValueError, match="reservation differs"):
        gate().evaluate(synthetic_rows(), bad_config)
