import json
import subprocess
from pathlib import Path

import pytest

from scripts.experiments.ecg.analysis.three_costs import (
    GraphInfo,
    cost_rows,
    parse_size,
    read_sg,
)


ROOT = Path(__file__).resolve().parents[2]


def test_three_cost_formulas():
    graph = GraphInfo(
        name="g", path="g.sg", directed=False,
        vertices=1024, serialized_edges=4096)
    rows = cost_rows(graph, 128 * 1024, ways=16, line_bytes=64)
    by_transport = {row["transport"]: row for row in rows}

    assert by_transport["unweighted_k2"]["k2_extra_bytes_per_edge"] == 4
    assert by_transport["unweighted_k2"][
        "k2_extra_active_stream_bytes"] == 16384
    assert by_transport["weighted_compact_k2"][
        "k2_extra_bytes_per_edge"] == 0
    assert by_transport["weighted_fallback_k2"][
        "k2_extra_bytes_per_edge"] == 4
    assert by_transport["unweighted_k2"][
        "k2_contextual_metadata_bits_per_line"] == 49
    assert by_transport["unweighted_k2"][
        "k2_contextual_way_equivalent"] == pytest.approx(1.53125)
    assert by_transport["unweighted_k2"]["popt_matrix_bytes"] == 128
    assert by_transport["unweighted_k2"]["popt_reserved_ways"] == 1
    assert "added metadata SRAM" in by_transport[
        "unweighted_k2"]["k2_cost_unit"]
    assert "capacity loss" in by_transport[
        "unweighted_k2"]["popt_cost_unit"]


def test_real_graph_headers_and_default_cli(tmp_path: Path):
    graph = read_sg(
        ROOT / "results/graphs/web-Google/web-Google.sg",
        "web-Google")
    assert graph.vertices == 916428
    assert graph.serialized_edges == 8644102
    assert not graph.directed
    assert parse_size("8MB") == 8 * 1024 * 1024

    script = ROOT / "scripts/experiments/ecg/analysis/three_costs.py"
    out_json = tmp_path / "three-costs.json"
    out_csv = tmp_path / "three-costs.csv"
    result = subprocess.run(
        [
            "python3", str(script),
            "--cache-sizes", "2MB", "8MB",
            "--out-json", str(out_json),
            "--out-csv", str(out_csv),
        ],
        check=True, capture_output=True, text=True)
    rows = json.loads(out_json.read_text())
    assert len(rows) == 18
    assert "| Graph | LLC | Transport |" in result.stdout
    assert "Extra active-stream MiB" in result.stdout
    assert out_csv.read_text().count("\n") == 19
