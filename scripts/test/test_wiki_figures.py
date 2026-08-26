"""Semantic and structural locks for ECG's generated public figures."""

from __future__ import annotations

import bisect
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def derive_next_two(
        readers: list[list[int]], line_begin: int, line_end: int,
        current: int, num_vertices: int, epoch_count: int) -> tuple[int, int]:
    def quantized(reader: int, wrapped: bool) -> int:
        epoch = min(reader * epoch_count // num_vertices, epoch_count - 1)
        current_epoch = current * epoch_count // num_vertices
        if wrapped and epoch == current_epoch:
            epoch = epoch_count - 1 if current_epoch == 0 else current_epoch - 1
        return epoch

    candidates = []
    for vertex in range(line_begin, line_end):
        values = sorted(readers[vertex])
        if not values:
            continue
        index = bisect.bisect_right(values, current)
        completed_cycles = 0
        for _ in range(2):
            if index == len(values):
                index = 0
                completed_cycles += 1
            reader = values[index]
            absolute = reader + completed_cycles * num_vertices
            candidates.append((
                absolute - current,
                quantized(reader, completed_cycles > 0),
            ))
            index += 1
    candidates.sort(key=lambda item: item[0])
    assert candidates
    return (
        candidates[0][1],
        candidates[1][1] if len(candidates) > 1 else candidates[0][1],
    )


def test_checked_figure_fixture_derives_published_values():
    fixture = json.loads(
        (ROOT / "fig/ecg-figure-fixture.json").read_text(encoding="utf-8")
    )
    n = fixture["num_vertices"]
    readers = [[] for _ in range(n)]
    rows = [[] for _ in range(n)]
    mapping = fixture["source_to_internal"]
    assert fixture["weighted_undirected_edges"] == [
        [0, 1, 2], [0, 2, 5], [0, 8, 9], [1, 2, 1], [1, 4, 4],
        [2, 3, 2], [2, 4, 3], [2, 8, 7], [3, 4, 2], [3, 5, 5],
        [4, 5, 1], [4, 7, 5], [5, 6, 2], [5, 7, 3], [6, 7, 2],
        [6, 8, 4], [7, 8, 1],
    ]
    for source_left, source_right, _weight in fixture[
            "weighted_undirected_edges"]:
        left = mapping[source_left]
        right = mapping[source_right]
        rows[left].append(right)
        readers[right].append(left)
        rows[right].append(left)
        readers[left].append(right)
    assert sorted(rows[8]) == [3, 6, 7, 11, 18]
    assert sorted(readers[18]) == [8, 11, 15, 20]

    order = sorted(range(n), key=lambda vertex: (-len(readers[vertex]), vertex))
    hot_count = int(fixture["hot_fraction"] * n)
    tiers = [3] * n
    for rank, vertex in enumerate(order):
        tiers[vertex] = 1 if rank < hot_count else 2 if rank < 2 * hot_count else 3
    assert tiers[6] == 1
    assert tiers[18] == 1
    assert tiers[20] == 2
    assert tiers[29] == 3

    vertices_per_line = (
        fixture["cache_line_bytes"] // fixture["property_element_bytes"]
    )
    line_begin = (18 // vertices_per_line) * vertices_per_line
    line_readers = sorted({
        reader
        for vertex in range(line_begin, line_begin + vertices_per_line)
        for reader in readers[vertex]
    })
    assert line_begin == 16
    assert line_readers == [1, 6, 8, 11, 15, 18, 20]
    assert derive_next_two(readers, 16, 32, 8, n, 32) == (11, 15)

    address = fixture["property_base"] + 18 * fixture["property_element_bytes"]
    assert address == 0x80000048
    assert address & ~(fixture["cache_line_bytes"] - 1) == 0x80000040
    id_bits = max(1, (n - 1).bit_length())
    epoch_bits = max(1, (fixture["epoch_count"] - 1).bit_length())
    assert id_bits + 2 + 2 * epoch_bits == 17


def test_fixture_derivation_matches_wrap_and_duplicate_reader_semantics():
    shared = [[] for _ in range(8)]
    shared[0] = [5]
    shared[1] = [5]
    assert derive_next_two(shared, 0, 2, 1, 8, 8) == (5, 5)

    wrapped = [[] for _ in range(8)]
    wrapped[0] = [2, 6]
    assert derive_next_two(wrapped, 0, 1, 6, 8, 8) == (2, 5)

    single = [[] for _ in range(8)]
    single[0] = [5]
    assert derive_next_two(single, 0, 1, 1, 8, 8) == (5, 5)


def test_generated_figure_contract_and_determinism():
    result = subprocess.run(
        [sys.executable, "scripts/docs/check_wiki_figures.py"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "validated 13 ECG wiki figures" in result.stdout


def test_retired_unmirrored_figures_are_removed():
    assert not list((ROOT / "wiki/assets").glob("*.svg"))


def test_architecture_figures_lock_critical_semantics():
    figures = {
        path.name: path.read_text(encoding="utf-8")
        for path in (ROOT / "fig/wiki").rglob("*.svg")
    }
    assert "unused high bits are zero/reserved" in figures[
        "reuse-plan-flowthrough-f02-record-formats.svg"
    ]
    flowthrough = figures[
        "reuse-plan-flowthrough-f05-flowthrough-outcomes.svg"
    ]
    assert "allocOnFill combines with OR" in flowthrough
    assert "out-of-range candidate does not inherit the bit" in flowthrough
    mshr = figures[
        "risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg"
    ]
    assert "governed mixed with ungoverned target" in mshr
    assert "equal sequence: payload must match" in mshr
    evidence = figures[
        "evaluation-methodology-f01-evidence-boundary.svg"
    ]
    assert "computed fused sideband is diagnostic" in evidence
    assert "inconsistent fused hints fail closed" in evidence
    assert "popt_target_time_charged = 0" in evidence


def test_pictorial_graph_timeline_and_pipeline_are_semantically_locked():
    figures = {
        path.name: path.read_text(encoding="utf-8")
        for path in (ROOT / "fig/wiki").rglob("*.svg")
    }
    graph = figures[
        "reuse-plan-flowthrough-f01-offline-construction.svg"
    ]
    assert "Checked nine-node weighted graph" in graph
    assert "tracked source edge 4 -&gt; 7" in graph
    assert graph.count('data-flow-kind="model-edge"') == 1

    timeline = figures[
        "reuse-plan-flowthrough-f03-future-distance.svg"
    ]
    for token in (
        "line 0x80000040: readers [1, 6, 8, 11, 15, 18, 20]",
        "current reader",
        "first future line use",
        "second future line use",
        "nearest = min(3, 7) = 3",
    ):
        assert token in timeline

    pipeline = figures[
        "risc-v-instruction-path-f02-o3-request-pipeline.svg"
    ]
    for token in (
        "ecg.flow.load.compact",
        "ecg.bind.load.u32",
        "rd = dest 18 | T1 | e1 11 | e2 15",
        "Fetch",
        "I0 flow.load",
        "I1 bind.load",
        "Decode",
        "I0 record op",
        "I1 u32 bind",
        "Rename",
        "I0 rd -&gt; P17",
        "I1 rs2 -&gt; P17",
        "IEW: issue / execute / writeback",
        "Issue queue",
        "I1 waits P17",
        "3 I1 AGU",
        "I0: record Request",
        "I1: property Request + ReuseBind",
        "Commit",
        "I0 ROB entry",
        "I1 ROB entry",
        "I0 commits before dependent I1",
        "execute reads format CSR and widens the result",
        "line 0x80000040",
    ):
        assert token in pipeline
    assert "record-format CSR" not in pipeline

    walkthrough = figures[
        "property-to-cache-walkthrough-f01-checked-request.svg"
    ]
    assert "Record-block MSHR" in walkthrough
    assert "Property MSHR" in walkthrough
    assert "Representative property-access pseudocode" in walkthrough
    assert "plan = ecg.flow.load.compact(record[edge_pos])  [B]" in walkthrough
    assert "value = ecg.bind.load.u32(addr, plan)  [C,D]" in walkthrough
    assert "tracked execution: source 4-&gt;7 maps to internal 8-&gt;18" in walkthrough
    assert "stamp T1 / e11 / e15" in walkthrough
    assert 'data-flow-label="record Request"' in walkthrough
    assert 'data-flow-label="property Request + ReuseBind"' in walkthrough
