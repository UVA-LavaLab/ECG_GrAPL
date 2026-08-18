import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
CONFIG = (
    ROOT / "scripts/experiments/ecg/configs/"
    "final_graph_corpus.json")
SCRIPT = (
    ROOT / "scripts/experiments/ecg/flows/"
    "prepare_final_graph_corpus.py")
MANIFEST = (
    ROOT / "scripts/experiments/ecg/experiment_manifest.json")
SCREEN = (
    ROOT / "scripts/experiments/ecg/configs/"
    "pagerank_literature_scale.json")


def load_module():
    spec = importlib.util.spec_from_file_location(
        "prepare_final_graph_corpus", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_final_graph_corpus_matches_literature_scale():
    data = json.loads(CONFIG.read_text())
    assert data["version"] == 1
    graphs = {graph["name"]: graph for graph in data["graphs"]}
    assert {
        "web-Google",
        "soc-pokec",
        "cit-Patents",
        "roadNet-CA",
        "soc-LiveJournal1",
        "com-Orkut",
        "twitter-2010",
    } == set(graphs)
    assert sum(
        graph["role"] == "core" for graph in graphs.values()) == 6
    assert graphs["com-Orkut"]["source_edges"] > 100_000_000
    assert graphs["twitter-2010"]["source_edges"] > 1_000_000_000
    assert graphs["twitter-2010"]["role"] == "scale_stress"
    for graph in graphs.values():
        assert graph["source"] == "SNAP"
        assert graph["url"].startswith("https://snap.stanford.edu/")
        assert "sha256" not in graph
        assert graph["reorder"] == 5
        assert graph["dbg_sg"].endswith("-dbg.sg")
        if graph["role"] == "core":
            assert graph["timing_sample_vertices"] == 262144
            assert graph["timing_sample_name"].endswith("-final-n18")


def test_graph_corpus_selection_and_empty_receipt(tmp_path):
    module = load_module()
    config = module.load_config(CONFIG)
    selected = module.selected_graphs(config, [], False)
    assert len(selected) == 6
    assert all(graph["role"] == "core" for graph in selected)
    selected = module.selected_graphs(config, [], True)
    assert len(selected) == 7

    receipt = tmp_path / "receipt.json"
    assert module.main([
        "--config", str(CONFIG),
        "--graph-root", str(tmp_path / "graphs"),
        "--receipt", str(receipt),
        "--receipt-only",
    ]) == 0
    payload = json.loads(receipt.read_text())
    assert payload["corpus"] == "literature_scale_final_graph_corpus"
    assert len(payload["graphs"]) == 7
    assert not any(
        graph["sg_present"] for graph in payload["graphs"])
    assert all(
        not graph.get("timing_sample_present", False)
        for graph in payload["graphs"]
        if graph["role"] == "core")


def test_download_uses_partial_file_and_atomic_publish(
        tmp_path, monkeypatch):
    module = load_module()
    destination = tmp_path / "graph.txt.gz"
    observed = {}

    def fake_run(command, check):
        observed["command"] = command
        output = Path(command[command.index("--output") + 1])
        output.write_bytes(b"complete archive")

    monkeypatch.setattr(module.subprocess, "run", fake_run)
    module.download("https://example.test/graph.gz", destination)
    assert destination.read_bytes() == b"complete archive"
    assert not (tmp_path / "graph.txt.gz.part").exists()
    assert "--continue-at" in observed["command"]
    assert observed["command"][
        observed["command"].index("--output") + 1].endswith(".part")


def test_parse_pagerank_semantic_receipt():
    module = load_module()
    receipt = module.parse_pr_receipt(
        "[ECG-PR-RESULT iterations=8 semantic_edges=1234 "
        "score_checksum=deadbeef]")
    assert receipt == {
        "iterations": 8,
        "edges": 1234,
        "checksum": "deadbeef",
    }
    with pytest.raises(ValueError, match="receipt is missing"):
        module.parse_pr_receipt("no receipt")


def test_literature_scale_campaign_shape_is_frozen():
    manifest = json.loads(MANIFEST.read_text())
    screen = json.loads(SCREEN.read_text())
    profile = "reuse_plan_literature_scale_campaign"
    stages = [
        stage for stage in manifest["stages"]
        if profile in stage.get("profiles", [])
    ]
    assert {stage["name"] for stage in stages} == {
        "60_gem5_proposal_reuse_bind_o3",
        "90_gem5_literature_scale_i1",
        "91_gem5_literature_scale_i8",
        "92_cache_sim_literature_scale_wide16",
        "93_cache_sim_literature_scale_popt",
        "94_cache_sim_literature_scale_compact16",
        "95_sniper_literature_scale_matched",
    }
    assert len(screen["graphs"]) == 6
    assert screen["iterations"] == [1, 8]
    assert len(screen["policies"]["all"]) == 8
    cells = rows = 0
    for stage in stages:
        if stage["name"].startswith(("90_", "91_")):
            graph_count = len(screen["graphs"])
            benchmark_count = 1
            policy_count = len(screen["policies"]["all"])
        elif stage["name"].startswith("60_"):
            graph_count = benchmark_count = 1
            policy_count = len(stage["policies"])
        else:
            graph_count = len(
                manifest["graph_sets"][stage["graph_set"]])
            benchmark_count = len(stage["benchmarks"])
            policy_count = len(stage["policies"])
        stage_cells = graph_count * benchmark_count
        cells += stage_cells
        rows += stage_cells * policy_count
    assert cells == 81
    assert rows == 447
