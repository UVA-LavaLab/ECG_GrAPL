import importlib.util
import json
import pytest
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/experiments/ecg/flows/sample_realgraph.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "ecg_sample_realgraph", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_edge_list_sample_is_deterministic(tmp_path):
    module = load_module()
    source = tmp_path / "graph.el"
    source.write_text(
        "# graph\n"
        "10 11\n"
        "11 12\n"
        "12 10\n"
        "12 13\n"
        "13 10\n"
        "99 100\n")
    output = tmp_path / "sample.el"
    vertices = tmp_path / "vertices.tsv"
    metadata = tmp_path / "sample.json"

    result = module.write_sample(
        source, output, vertices, metadata, target_vertices=4)

    assert output.read_text() == (
        "1\t2\n"
        "2\t0\n"
        "0\t1\n"
        "0\t3\n"
        "3\t1\n")
    assert vertices.read_text() == "0\t12\n1\t10\n2\t11\n3\t13\n"
    assert result["vertices"] == 4
    assert result["edges"] == 5
    assert result["root_original_vertex"] == 12
    assert result["root_out_degree"] == 2


def test_matrix_market_header_is_skipped(tmp_path):
    module = load_module()
    source = tmp_path / "graph.mtx"
    source.write_text(
        "%%MatrixMarket matrix coordinate pattern general\n"
        "% comment\n"
        "5 5 4\n"
        "1 2\n"
        "2 3\n"
        "3 1\n"
        "4 5\n")
    output = tmp_path / "sample.el"
    vertices = tmp_path / "vertices.tsv"
    metadata = tmp_path / "sample.json"

    module.write_sample(
        source, output, vertices, metadata, target_vertices=3)

    assert output.read_text() == "0\t1\n1\t2\n2\t0\n"
    assert json.loads(metadata.read_text())["edges"] == 3


def test_sample_rejects_too_few_vertices(tmp_path):
    module = load_module()
    source = tmp_path / "small.el"
    source.write_text("0 1\n1 2\n")
    with pytest.raises(RuntimeError, match="requested 4"):
        module.select_vertices(source, 4)
