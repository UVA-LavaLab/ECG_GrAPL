import importlib.util
import json
from pathlib import Path
import subprocess

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
CONVERTER = ROOT / "bench/bin/converter"


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
            assert graph["timing_sample_edges"] == 350000
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


def test_low_memory_flag_preserves_following_reorder_option(tmp_path):
    if not CONVERTER.is_file():
        pytest.skip("converter binary not built")
    edge_list = tmp_path / "varying-degree.el"
    edge_list.write_text(
        "0 1\n0 2\n0 3\n0 4\n1 0\n2 0\n3 0\n4 0\n4 1\n")
    original_base = tmp_path / "original"
    subprocess.run(
        [str(CONVERTER), "-f", str(edge_list), "-b", str(original_base)],
        cwd=ROOT, check=True, capture_output=True, text=True)
    reordered_base = tmp_path / "reordered"
    result = subprocess.run(
        [
            str(CONVERTER), "-f", str(original_base.with_suffix(".sg")),
            "-m", "-o", "5", "-b", str(reordered_base),
        ],
        cwd=ROOT, check=True, capture_output=True, text=True)
    assert "DBG Map Time" in result.stdout
    assert original_base.with_suffix(".sg").read_bytes() != \
        reordered_base.with_suffix(".sg").read_bytes()


def test_rebuilt_timing_sample_invalidates_semantic_receipt(
        tmp_path, monkeypatch):
    module = load_module()
    graph_root = tmp_path / "graphs"
    source_dir = graph_root / "test-graph"
    source_dir.mkdir(parents=True)
    (source_dir / "test.el").write_text("0 1\n1 2\n")
    sample_dir = graph_root / "test-final-n2"
    sample_dir.mkdir()
    semantic = sample_dir / "test-final-n2.semantic.json"
    semantic.write_text('{"stale": true}\n')
    observed = {}

    def fake_run(command, cwd, check):
        observed["command"] = command
        Path(command[command.index("--output") + 1]).write_text("0 1\n")
        Path(command[command.index("--vertices") + 1]).write_text(
            "0 0\n1 1\n")
        Path(command[command.index("--metadata") + 1]).write_text(
            json.dumps({"vertices": 2, "target_edges": 1}))

    converted = []
    monkeypatch.setattr(module.subprocess, "run", fake_run)
    monkeypatch.setattr(
        module, "convert",
        lambda source, output, symmetrize, reorder=0:
        converted.append((source, output, symmetrize, reorder)))

    module.prepare_timing_sample({
        "name": "test-graph",
        "edge_list": "test.el",
        "timing_sample_name": "test-final-n2",
        "timing_sample_vertices": 2,
        "timing_sample_edges": 1,
        "reorder": 5,
    }, graph_root, force=False)

    assert not semantic.exists()
    command = observed["command"]
    assert command[command.index("--target-edges") + 1] == "1"
    assert [entry[2:] for entry in converted] == [
        (True, 0),
        (False, 5),
    ]


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
    stage_by_name = {stage["name"]: stage for stage in stages}
    for name in (
            "90_gem5_literature_scale_i1",
            "91_gem5_literature_scale_i8",
            "92_cache_sim_literature_scale_wide16",
            "93_cache_sim_literature_scale_popt",
            "94_cache_sim_literature_scale_compact16",
            "95_sniper_literature_scale_matched"):
        assert stage_by_name[name]["flowthrough"] == "all"
    assert len(screen["graphs"]) == 6
    assert screen["iterations"] == [1, 8]
    assert len(screen["policies"]["all"]) == 8
    for graph in screen["graphs"]:
        assert graph["vertices"] == 262144
        assert graph["directed_edges"] <= 700000
        assert graph["semantic_receipts"]["1"]["edges"] == (
            graph["directed_edges"])
        assert graph["semantic_receipts"]["8"]["edges"] == (
            graph["directed_edges"] * 8)
        assert len(graph["semantic_receipts"]["1"]["checksum"]) == 16
        assert len(graph["semantic_receipts"]["8"]["checksum"]) == 16
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
