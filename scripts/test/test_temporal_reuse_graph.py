import importlib.util
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
PATH = (
    ROOT /
    "scripts/experiments/ecg/flows/generate_temporal_reuse_graph.py")
SPEC = importlib.util.spec_from_file_location(
    "generate_temporal_reuse_graph_test", PATH)
GEN = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)


def read_edges(path: Path):
    return [tuple(map(int, line.split())) for line in path.read_text().splitlines()]


def test_temporal_graphs_have_identical_degree_sequences(tmp_path):
    outputs = {}
    for mode in ("clustered", "spread"):
        graph = tmp_path / f"{mode}.el"
        metadata = tmp_path / f"{mode}.json"
        receipt = GEN.generate(graph, metadata, 32, 4, mode)
        assert receipt["directed_edges"] == 128
        assert json.loads(metadata.read_text()) == receipt
        outputs[mode] = read_edges(graph)

    for mode, graph_edges in outputs.items():
        outdegree = [0] * 32
        indegree = [0] * 32
        for source, destination in graph_edges:
            outdegree[source] += 1
            indegree[destination] += 1
        assert outdegree == [4] * 32, mode
        assert indegree == [4] * 32, mode

    clustered_readers = [
        destination for source, destination in outputs["clustered"]
        if source == 0
    ]
    spread_readers = [
        destination for source, destination in outputs["spread"]
        if source == 0
    ]
    assert clustered_readers == [1, 2, 3, 4]
    assert spread_readers == [2, 6, 10, 14]
