import json
from pathlib import Path

from scripts.experiments.ecg.analysis import literature_scale_gate


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads(
    (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
SCREEN = json.loads(
    (ROOT / "scripts/experiments/ecg/configs/"
     "pagerank_literature_scale.json").read_text())
CORPUS = ROOT / "results/graphs/literature_scale_corpus.receipt.json"


def test_literature_scale_gate_expected_shapes():
    screen = literature_scale_gate.expected_cells(
        MANIFEST, SCREEN, literature_scale_gate.SCREEN_STAGES)
    complete = literature_scale_gate.expected_cells(
        MANIFEST, SCREEN, literature_scale_gate.COMPLETE_STAGES)
    assert len(screen) == 13
    assert sum(len(roster) for roster in screen.values()) == 99
    assert len(complete) == 81
    assert sum(len(roster) for roster in complete.values()) == 447


def test_screen_gate_receipt_binds_commit_and_configs():
    source = (
        ROOT / "scripts/experiments/ecg/analysis/"
        "literature_scale_gate.py").read_text()
    assert '"git_head": git_head' in source
    assert '"manifest_sha256": sha256(Path(args.manifest))' in source
    assert '"screen_config_sha256": sha256(Path(args.screen_config))' in source


def test_literature_scale_corpus_receipt_is_complete():
    if not CORPUS.is_file():
        return
    assert literature_scale_gate.validate_corpus(
        MANIFEST, CORPUS) == []


def test_empty_screen_gate_fails_closed():
    result = literature_scale_gate.evaluate(
        [], MANIFEST, SCREEN, [], "screen", CORPUS)
    assert result["valid"] is False
    assert result["cell_count"] == 0
    assert result["row_count"] == 0
    assert any(
        "missing literature-scale cells" in error
        for error in result["errors"])
    assert any(
        "PageRank timing rows incomplete" in error
        for error in result["errors"])


def test_literature_gate_requires_certified_sniper_fallback():
    manifest = {
        "graph_sets": {
            "literature_scale_sniper": [{
                "name": "graph",
                "sniper_semantic_edge_limit": 100000,
            }],
        },
    }
    row = {
        "status": "ok",
        "policy_label": "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH",
        "final_matrix_config_hash": "hash",
        "simulator": "sniper",
        "timing_valid_for_speedup": "0",
        "sniper_queue_model": "windowed_mg1",
        "sniper_semantic_edge_limit": "100000",
        "semantic_work_matched": "1",
        "sniper_transport_record_bytes": "8",
        "sniper_reuse_bind_consumes": "32",
        "sniper_reuse_bind_bad_consumes": "0",
        "sniper_reuse_bind_certified_prefixes": "1",
        "sniper_reuse_bind_certified_fallbacks": "10",
        "sniper_transport_receipts_validated": "1",
        "sniper_reuse_plan_epoch_context_validated": "1",
        "sniper_reuse_bind_exact_validated": "1",
    }
    groups = {
        ("95_sniper_literature_scale_matched", "graph", "pr"): [row],
    }
    assert literature_scale_gate.validate_rows(
        groups, manifest) == []
    row["sniper_reuse_bind_certified_prefixes"] = "0"
    assert "exact-bind proof failed" in (
        literature_scale_gate.validate_rows(
            groups, manifest)[0])


def test_literature_gate_requires_two_column_charged_popt():
    row = {
        "status": "ok",
        "policy_label": "POPT",
        "final_matrix_config_hash": "hash",
        "simulator": "cache_sim",
        "timing_valid_for_speedup": "0",
        "popt_overhead_charged": "1",
        "popt_matrix_active_columns": "2",
        "popt_matrix_stream_mode": "simulated",
        "popt_matrix_stream_lines_simulated": "1",
    }
    groups = {
        ("93_cache_sim_literature_scale_popt", "graph", "pr"): [row],
    }
    manifest = {"graph_sets": {"literature_scale_sniper": []}}
    assert literature_scale_gate.validate_rows(
        groups, manifest) == []
    row["popt_overhead_charged"] = "0"
    assert "P-OPT stream is uncharged" in (
        literature_scale_gate.validate_rows(
            groups, manifest)[0])


def test_complete_gate_requires_persisted_screen_authorization(tmp_path):
    manifest = tmp_path / "manifest.json"
    screen = tmp_path / "screen.json"
    manifest.write_text("{}")
    screen.write_text("{}")
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({
        "valid": True,
        "phase": "screen",
        "pagerank_gate": {"screen_passes": True},
    }))
    run_dir = tmp_path / "full"
    run_dir.mkdir()
    snapshot = {
        "jobs": [{
            "stage": "92_cache_sim_literature_scale_wide16",
        }],
        "screen_authorization": {
            "path": str(gate),
            "sha256": literature_scale_gate.sha256(gate),
            "git_head": "abc",
            "manifest_sha256": literature_scale_gate.sha256(manifest),
            "screen_config_sha256": literature_scale_gate.sha256(screen),
        },
    }
    (run_dir / "resolved_manifest.json").write_text(
        json.dumps(snapshot))
    assert literature_scale_gate.validate_full_role_authorizations(
        [run_dir], manifest, screen, "abc") == []
    snapshot["screen_authorization"]["sha256"] = "stale"
    (run_dir / "resolved_manifest.json").write_text(
        json.dumps(snapshot))
    assert "screen authorization is stale" in (
        literature_scale_gate.validate_full_role_authorizations(
            [run_dir], manifest, screen, "abc")[0])
