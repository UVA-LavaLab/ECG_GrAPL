"""Semantic and structural locks for the current Scale6 public figures."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]


def checked_stream():
    fixture = json.loads((ROOT / "fig/ecg-figure-fixture.json").read_text())
    rows = [[] for _ in range(fixture["num_vertices"])]
    mapping = fixture["source_to_internal"]
    for left, right, _weight in fixture["weighted_undirected_edges"]:
        rows[mapping[left]].append(mapping[right])
        rows[mapping[right]].append(mapping[left])
    for row in rows:
        row.sort()
    return fixture, rows, [vertex for row in rows for vertex in row]


def test_checked_fixture_derives_scale6_word_and_deadline():
    fixture, rows, stream = checked_stream()
    assert rows[8] == [3, 6, 7, 11, 18]
    position = sum(len(row) for row in rows[:8]) + rows[8].index(18)
    assert position == 18
    next_position = next(
        index for index in range(position + 1, len(stream))
        if stream[index] // 16 == 18 // 16)
    assert next_position == 22
    distance = next_position - position
    bucket = distance.bit_length() - 1
    token = bucket + 2
    upper = (1 << (bucket + 1)) - 1
    assert (distance, token, upper) == (4, 4, 7)
    assert (token << 26) | 18 == 0x10000012
    assert position + 1 + upper == 26
    address = fixture["property_base"] + 18 * fixture["property_element_bytes"]
    assert address == 0x80000048
    assert address & ~(fixture["cache_line_bytes"] - 1) == 0x80000040
    assert stream[25] == 20
    assert stream[25] // 16 == stream[position] // 16


def test_figure_examples_match_actual_scale6_helpers(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    source = tmp_path / "figure_fixture.cc"
    binary = tmp_path / "figure_fixture"
    source.write_text(r'''
#include "ecg_ref32.h"
#include <cstdio>
int main() {
    using namespace ecg_ref32;
    const auto token = encodeScaleToken(4, State::FINITE);
    const auto record = packScaleRecord32(18, token, 26);
    const auto decoded = decodeScaleRecord32(record, 26);
    if (token != 4 || record != 0x10000012 || decoded.destination != 18 ||
        decoded.distance != 7 || decoded.state != State::FINITE) return 1;
    if (resolveQuantizedFuture(State::FINITE, 26, 26, 32).state != State::FINITE ||
        resolveQuantizedFuture(State::FINITE, 26, 27, 32).state != State::UNKNOWN)
        return 2;
    const unsigned lines[16] = {0,0,1,1,2,2,0,2,3,3,4,3,4,5,5,5};
    uint32_t records[16];
    for (unsigned i = 0; i < 16; ++i)
        records[i] = packScaleRecord32(lines[i] * 16, 0, 26);
    records[8] = packScaleRecord32(48, encodeScaleToken(31, State::FINITE), 26);
    records[10] = packScaleRecord32(64, encodeScaleToken(7, State::FINITE), 26);
    records[13] = packScaleRecord32(80, encodeScaleToken(15, State::FINITE), 26);
    if (selectScalePrefetchDelta(records, 16, 0, 26) != 10) return 3;
    std::puts("checked Scale6 figure examples");
}
''')
    built = subprocess.run(
        [compiler, "-std=c++17", "-O2", f"-I{ROOT / 'bench/include'}",
         str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr


def test_generated_figure_contract_and_determinism():
    result = subprocess.run(
        [sys.executable, "scripts/docs/check_wiki_figures.py"],
        cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "validated 13 ECG wiki figures" in result.stdout


def test_architecture_figures_do_not_regress_to_card_grids():
    generator = (ROOT / "scripts/docs/generate_ecg_figures.py").read_text()
    assert ".card(" not in generator
    assert "f.table(" in generator and "f.bitfield(" in generator
    assert 'kind="model-edge"' in generator


def figures():
    return {path.name: path.read_text()
            for path in (ROOT / "fig/wiki").rglob("*.svg")}


def test_scale6_figures_lock_record_and_expiry_semantics():
    rendered = figures()
    word = rendered["reuse-plan-flowthrough-f02-record-formats.svg"]
    for token in ("[31:26]", "[25:0]", "0x10000012", "2..32", "33..63",
                  "no usable future prediction", "prefetch", "Larger IDs fail closed"):
        assert token in word
    timeline = rendered["reuse-plan-flowthrough-f03-future-distance.svg"]
    for token in ("j=18, seq=19", "j=22, seq=23", "true distance = 4",
                  "decoded upper bound = 7", "UNKNOWN", "EXPIRY IS NOT DEATH"):
        assert token in timeline
    walkthrough = rendered["property-to-cache-walkthrough-f01-checked-request.svg"]
    for token in ("0x10000012", "0x80000048", "0x80000040", "deadline = 26",
                  "8-request update latency", "expired update is discarded",
                  "not a measured gem5 execution"):
        assert token in walkthrough


def test_current_policy_and_prefetch_are_not_legacy_shortcuts():
    rendered = figures()
    policy = rendered["reuse-plan-flowthrough-f04-llc-policy-pipeline.svg"]
    for token in ("16-entry", "35 added bits", "distanceRRPV(remaining)",
                  "max(RRPV, local GRASP)", "score is compared first",
                  "never allocate for an update"):
        assert token in policy
    prefetch = rendered["reuse-plan-flowthrough-f05-lookahead-prefetch.svg"]
    for token in ("candidate leads 8..15", "lead +10", "Future bound",
                  "8-entry prefetch queue", "at most 1 issue",
                  "no prefetch allocation", "FlowThrough is OFF"):
        assert token.lower() in prefetch.lower()
    for retired in ("f05-flowthrough-outcomes", "f06-structural-fairness"):
        assert not (ROOT / "fig/wiki/reuse-plan-flowthrough" /
                    f"reuse-plan-flowthrough-{retired}.svg").exists()
    assert not list((ROOT / "wiki/assets").glob("*.svg"))


def test_native_path_keeps_qualification_and_prefetch_limits_explicit():
    rendered = figures()
    family = rendered["risc-v-instruction-path-f01-instruction-family.svg"]
    assert "under qualification; prefetch remains pending" in family
    assert "native replacement under qualification; prefetch pending" in family
    pipeline = rendered["risc-v-instruction-path-f02-o3-request-pipeline.svg"]
    for token in ("Fetch", "Decode", "Rename", "Issue / select", "Physical registers",
                  "I1 waits P17", "AGU", "LSQ", "ROB", "L1D / L2",
                  "commit-only update", "squashed load: no refresh",
                  "16 slots; &gt;=8 CPU cycles; 1 out/cycle; bounded capture",
                  "native prefetch is not implemented"):
        assert token in pipeline
    lifetime = rendered["risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg"]
    for token in ("equal seq requires same payload", "allocOnFill combines with OR",
                  "Completed load", "Retired load", "discard; do not enqueue",
                  "predictions install only at delivery"):
        assert token in lifetime


def test_storage_domains_and_backend_limits_are_explicit():
    rendered = figures()
    budget = rendered["reuse-plan-flowthrough-f06-capacity-accounting.svg"]
    for token in ("2,603,265", "635.6 MiB", "4.97 MiB", "2.48 MiB",
                  "10 reserved / 6 data", "5 reserved / 11 data",
                  "4 reserved / 12 data", "2 reserved / 14 data",
                  "not yet an equal-area comparison"):
        assert token in budget
    state = rendered["property-to-cache-walkthrough-f02-architecture-state-map.svg"]
    for token in ("4,587,520", "3,064", "4,590,584", "bit counts are not synthesized area"):
        assert token in state
    evidence = rendered["evaluation-methodology-f01-evidence-boundary.svg"]
    for token in ("popt_target_time_charged = 0", "Scale6 rows unsupported",
                  "Scale6 area / timing not established", "fail closed",
                  "dirty writebacks", "POPT_SE_DISTANT", "reconstructions"):
        assert token in evidence


def test_line_state_uses_singular_bit_unit():
    rendered = figures()
    for name in (
        "reuse-plan-flowthrough-f04-llc-policy-pipeline.svg",
        "property-to-cache-walkthrough-f02-architecture-state-map.svg",
    ):
        assert ">1 bit<" in rendered[name]
        assert ">1 bits<" not in rendered[name]


def test_graph_id_annotations_do_not_depend_on_text_stroke_paint_order():
    path = ROOT / (
        "fig/wiki/reuse-plan-flowthrough/"
        "reuse-plan-flowthrough-f01-offline-construction.svg")
    root = ET.parse(path).getroot()
    annotations = [
        node for node in root.iter("{http://www.w3.org/2000/svg}text")
        if (node.text or "").startswith("int ")
    ]
    assert len(annotations) == 9
    assert all(node.get("fill") == "#475467" for node in annotations)
    assert all(node.get("stroke") is None for node in annotations)


def test_public_graph_terminology_is_direction_explicit():
    text = " ".join(" ".join((ROOT / path).read_text().split()) for path in (
        "README.md", "wiki/Home.md", "wiki/ReusePlan-FlowThrough.md",
        "wiki/Property-to-Cache-Walkthrough.md"))
    for token in ("out-neighbors", "in-neighbors", "`N_out(u)`", "`N_in(u)`",
                  "`d_in(v)`", "`d_out(v)`", "outer vertex", "property vertex"):
        assert token in text
    for imprecise in ("reader graph", "current reader", "future readers",
                      "honest traffic", "reading spine"):
        assert imprecise not in text
