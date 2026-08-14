import json
import hashlib
import struct
from pathlib import Path

from scripts.experiments.ecg.analysis import final_campaign_gate


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads(
    (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
SCREEN = json.loads(
    (ROOT / "scripts/experiments/ecg/configs/pagerank_study.json").read_text())


def test_final_gate_derives_all_cells_from_manifests():
    expected = final_campaign_gate.expected_cells(MANIFEST, SCREEN)
    assert len(expected) == 76
    stage_cells = {}
    for stage, _, _ in expected:
        stage_cells[stage] = stage_cells.get(stage, 0) + 1
    assert stage_cells == {
        "60_gem5_proposal_reuse_bind_o3": 1,
        "70_gem5_pagerank_i1": 3,
        "71_gem5_pagerank_i2": 3,
        "72_gem5_pagerank_i4": 3,
        "73_gem5_pagerank_i8": 3,
        "80_cache_sim_final_fullgraph": 12,
        "81_sniper_final_semantic": 12,
        "82_cache_sim_final_wide16": 15,
        "83_cache_sim_final_wide256": 15,
        "84_cache_sim_final_popt": 6,
        "85_sniper_final_sssp_wide": 3,
    }


def test_final_gate_discovers_nested_run_directories(tmp_path):
    direct = tmp_path / "direct"
    nested = tmp_path / "root" / "cell"
    direct.mkdir()
    nested.mkdir(parents=True)
    (direct / "resolved_manifest.json").write_text("{}")
    (nested / "resolved_manifest.json").write_text("{}")
    assert final_campaign_gate.discover_run_dirs(
        [direct, tmp_path / "root"]) == [direct.resolve(), nested.resolve()]


def test_final_gate_annotates_selected_timing_matrix(tmp_path):
    run_dir = tmp_path / "run"
    matrix_dir = (
        run_dir / "matrices" / "70_gem5_pagerank_i1" /
        "web-Google-n16" / "pr")
    matrix_dir.mkdir(parents=True)
    (run_dir / "resolved_manifest.json").write_text("{}")
    matrix = matrix_dir / "roi_matrix.csv"
    matrix.write_text("status,policy_label\nok,LRU\n")
    marker = {
        "complete": True,
        "all_rows_ok": True,
        "rows": 1,
        "matrix_id": "70_gem5_pagerank_i1_web-Google-n16_pr",
        "matrix_config_hash": "matrix-hash",
        "comparison_config_hash": "comparison-hash",
        "expected_policy_labels": ["LRU"],
        "shard_group": "timing-repair",
        "outputs": {
            "roi_matrix.csv": {
                "rows": 1,
                "sha256": hashlib.sha256(
                    matrix.read_bytes()).hexdigest(),
                "size": matrix.stat().st_size,
            },
        },
    }
    (matrix_dir / "roi_matrix.complete.json").write_text(
        json.dumps(marker))
    rows = [{
        "status": "ok",
        "policy_label": "LRU",
        "pipeline_source_csv": str(matrix),
    }]
    assert final_campaign_gate.annotate_selected_timing_rows(
        rows, [matrix]) == [run_dir.resolve()]
    assert rows[0]["final_stage"] == "70_gem5_pagerank_i1"
    assert rows[0]["final_graph"] == "web-Google-n16"
    assert rows[0]["final_job_id"] == marker["matrix_id"]
    assert rows[0]["final_matrix_config_hash"] == "matrix-hash"


def test_final_gate_rejects_missing_policy():
    expected = {
        ("stage", "graph", "pr"): ("LRU", "ECG_REUSE_PLAN"),
    }
    groups = {
        ("stage", "graph", "pr"): [
            {"policy_label": "LRU"},
        ],
    }
    errors = final_campaign_gate.validate_rosters(groups, expected)
    assert "policy roster mismatch" in errors[0]


def test_final_gate_detects_cache_baseline_drift():
    groups = {}
    for stage in final_campaign_gate.CACHE_CONTROL_STAGES:
        row = {"policy_label": "LRU"}
        for field in final_campaign_gate.BASELINE_DRIFT_FIELDS:
            row[field] = "1"
        if stage == "83_cache_sim_final_wide256":
            row["l3_misses"] = "2"
        groups[(stage, "graph", "pr")] = [row]
    assert final_campaign_gate.validate_cache_baselines(groups) == [
        "('graph', 'pr') baseline drift across width/epoch controls"
    ]


def test_final_gate_accepts_natural_sniper_completion():
    manifest = {
        "graph_sets": {
            "factorial_graphs_uniform_8mb": [{
                "name": "graph",
                "sniper_semantic_edge_limit": 100,
            }],
        },
    }
    row = {
        "status": "ok",
        "policy_label": "LRU",
        "final_matrix_config_hash": "hash",
        "simulator": "sniper",
        "timing_valid_for_speedup": "0",
        "sniper_queue_model": "windowed_mg1",
        "sniper_transport_record_bytes": "4",
        "edge_stream_bytes_per_edge": "4",
        "sniper_semantic_edge_limit": "100",
        "sniper_semantic_edge_visits": "60",
        "sniper_semantic_truncated": "0",
        "semantic_work_matched": "1",
        "l3_accesses": "1",
        "l3_misses": "1",
    }
    groups = {
        ("81_sniper_final_semantic", "graph", "bfs"): [row],
    }
    assert final_campaign_gate.validate_role_rows(
        groups, manifest) == []
    row["sniper_semantic_truncated"] = "1"
    assert "semantic work mismatch" in (
        final_campaign_gate.validate_role_rows(
            groups, manifest)[0])


def test_final_gate_allows_only_documented_untracked_checkouts(tmp_path):
    allowed = tmp_path / "allowed"
    unexpected = tmp_path / "unexpected"
    for run_dir, status in (
            (allowed, "?? bench/include/sniper_sim/snipersim\n"),
            (unexpected, "?? scratch.txt\n")):
        preflight = run_dir / "preflight"
        preflight.mkdir(parents=True)
        (preflight / "git_diff_stat.txt").write_text("")
        (preflight / "git_status.txt").write_text(status)
    assert final_campaign_gate.validate_run_preflight([allowed]) == []
    errors = final_campaign_gate.validate_run_preflight([unexpected])
    assert "unexpected worktree state" in errors[0]


def test_final_gate_validates_graph_header_receipts(tmp_path):
    graph_path = tmp_path / "graph.sg"
    graph_path.write_bytes(
        bytes([0]) + struct.pack("<q", 12) + struct.pack("<q", 8))
    manifest = {
        "graph_sets": {
            "factorial_graphs_uniform_8mb": [{
                "name": "graph",
                "path": "graph.sg",
                "compact_id_bits": 3,
                "compact_epoch_bits": 4,
                "compact_total_bits": 13,
                "sniper_semantic_edge_limit": 12,
                "sniper_semantic_edge_source":
                    "symmetrized .sg serialized edge count",
            }],
        },
    }
    assert final_campaign_gate.validate_final_graph_receipts(
        manifest, 2, tmp_path) == []
    manifest["graph_sets"]["factorial_graphs_uniform_8mb"][0][
        "sniper_semantic_edge_limit"] = 11
    assert "serialized edges mismatch" in (
        final_campaign_gate.validate_final_graph_receipts(
            manifest, 2, tmp_path)[0])


def test_final_graph_receipt_rejects_wrong_shape(tmp_path):
    graph_path = tmp_path / "graph.sg"
    graph_path.write_bytes(
        bytes([1]) + struct.pack("<q", 12) + struct.pack("<q", 8))
    graph = {
        "name": "graph",
        "path": "graph.sg",
        "compact_id_bits": 4,
        "compact_epoch_bits": 4,
        "compact_total_bits": 99,
        "sniper_semantic_edge_limit": 12,
    }
    manifest = {
        "graph_sets": {
            "factorial_graphs_uniform_8mb": [graph],
        },
    }
    errors = final_campaign_gate.validate_final_graph_receipts(
        manifest, 2, tmp_path)
    assert any("semantic edge source is not declared" in error
               for error in errors)
    assert any("final graph is not symmetrized" in error
               for error in errors)
    assert any("compact id bits mismatch" in error for error in errors)
    assert any("compact total bits mismatch" in error for error in errors)
