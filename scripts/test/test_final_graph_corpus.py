import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONFIG = (
    ROOT / "scripts/experiments/ecg/configs/"
    "final_graph_corpus.json")
SCRIPT = (
    ROOT / "scripts/experiments/ecg/flows/"
    "prepare_final_graph_corpus.py")


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
