"""Semantic and geometry locks for ECG's example-led public figures."""

from __future__ import annotations

import json
import math
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SVG_NS = "{http://www.w3.org/2000/svg}"


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


def test_fixture_derives_both_words_and_unchanged_property_data():
    fixture, rows, stream = checked_stream()
    assert rows[8] == [3, 6, 7, 11, 18]
    position = sum(len(row) for row in rows[:8]) + rows[8].index(18)
    assert position == 18 and len(stream) == 34
    next_position = next(
        index for index in range(position + 1, len(stream))
        if stream[index] // 16 == 18 // 16)
    assert next_position == 22
    distance = next_position - position
    assert distance == 4
    bits = (fixture["num_vertices"] - 1).bit_length()
    assert bits == 5 and 32 - bits - 14 == 13
    reference = (distance.bit_length() - 1) << 3
    mask = (reference << bits) | (1 << (bits + 8))
    assert reference == 16 and mask == 0x2200
    assert (mask | 18) == 0x2212 and (0x2212 & ((1 << bits) - 1)) == 18
    token = 2 + distance.bit_length() - 1
    upper = (1 << distance.bit_length()) - 1
    assert token == 4 and upper == 7
    assert (token << 26) | 18 == 0x10000012
    assert position + 1 + distance == 23
    assert position + 1 + upper == 26
    address = fixture["property_base"] + 18 * fixture["property_element_bytes"]
    assert address == 0x80000048
    assert address & ~(fixture["cache_line_bytes"] - 1) == 0x80000040
    assert len(rows[18]) == 4
    contribution = 1.0 / (fixture["num_vertices"] * len(rows[18]))
    assert contribution == 1 / 128
    assert struct.unpack("<I", struct.pack("<f", contribution))[0] == 0x3C000000
    assert stream[17] == 11 and stream[19] == 7
    assert stream[17] // 16 == stream[19] // 16 == 0
    assert stream[25] == 20 and stream[25] // 16 == stream[position] // 16


def test_examples_match_actual_builders_codecs_and_victim_helper(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    _, rows, stream = checked_stream()
    offsets = [0]
    for row in rows:
        offsets.append(offsets[-1] + len(row))
    source = tmp_path / "figure_fixture.cc"
    binary = tmp_path / "figure_fixture"
    code = r'''
#include "ecg_ref32.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
int main() {
    using namespace ecg_ref32;
    int failures = 0;
    const auto check = [&failures](bool condition, const char* label) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    const std::vector<uint64_t> offsets = {@OFFSETS@};
    const std::vector<uint32_t> destinations = {@DESTINATIONS@};
    FlatRecords rich, scale;
    if (!buildFlatRecordsFromDestinations(32, 16, offsets, destinations, rich) ||
        !buildFlatScaleRecordsFromDestinations(32, 16, offsets, destinations, scale, 26) ||
        rich.records.size() != 34 || scale.records.size() != 34) {
        std::fprintf(stderr, "fixture build failed\n");
        return 1;
    }
    check(bitsForVertices(32) == 5 && kMetadataBits == 14, "small-graph bit budget");
    check(canPackRecord32(1u << 18) && !canPackRecord32((1u << 18) + 1),
          "Full14 ID boundary");
    check(canPackScaleRecord32(1u << 26, 26) &&
          !canPackScaleRecord32((1u << 26) + 1, 26), "Scale6 ID boundary");
    check(rich.records[18] == 0x2212 && (rich.records[18] >> 19) == 0,
          "actual five-bit-ID Full14 word with thirteen unused bits");
    check(rich.exact_distances[18] == 4, "graph-derived next B distance");
    const auto full = decodeRecord32(rich.records[18], 5);
    const auto compact = decodeScaleRecord32(scale.records[18], 26);
    check(full.destination == 18 && full.distance == 4 &&
          full.state == State::FINITE && full.action == 0, "Full14 decode");
    check(scale.records[18] == 0x10000012 && compact.destination == 18 &&
          compact.distance == 7 && compact.state == State::FINITE, "Scale6 decode");
    for (const auto& fields : std::array<std::pair<unsigned, unsigned>, 3>{
             {{8, 4}, {10, 2}, {12, 0}}}) {
        FlatRecords variant;
        const bool built = buildFlatRecordsFromDestinations(
            32, 16, offsets, destinations, variant, fields.first, fields.second);
        check(built && variant.records.size() == 34, "implemented Full14 field split");
        if (!built || variant.records.size() != 34)
            continue;
        const auto decoded = decodeRecord32(
            variant.records[18], 5, fields.first, fields.second);
        check(decoded.destination == 18 && decoded.distance == 4 &&
              decoded.state == State::FINITE && decoded.action == 0,
              "field split preserves the running edge");
    }
    check(!validFieldWidths(8, 5), "metadata does not silently grow beyond fourteen");
    check(decodeDistanceUpper(encodeDistance(100, 8), 8) == 103 &&
          decodeDistanceUpper(encodeDistance(100, 10), 10) == 101 &&
          decodeDistanceUpper(encodeDistance(100, 12), 12) == 100 &&
          decodeScaleDistance(encodeScaleToken(100, State::FINITE)) == 127,
          "precision table");
    check(resolveQuantizedFuture(State::FINITE, 23, 23, 21).state == State::FINITE &&
          resolveQuantizedFuture(State::FINITE, 23, 24, 21).state == State::UNKNOWN &&
          resolveQuantizedFuture(State::FINITE, 26, 26, 32).state == State::FINITE &&
          resolveQuantizedFuture(State::FINITE, 26, 27, 32).state == State::UNKNOWN,
          "expiry is UNKNOWN, not DEAD");
    uint64_t config = 0, iteration = 0, canonical = 0;
    NativeAccess access;
    check(packNativeConfig(32, 34, config) &&
          packNativeIteration(0, 34, 1, iteration) &&
          canonicalScaleRecord(scale.records[18], 0x40000048, 0x40000000,
                               config, iteration, canonical) &&
          nativePropertyAccess(canonical, 0x80000000, config, access),
          "actual native operand helpers");
    check(canonical == 0x0000001310000012ULL && access.destination == 18 &&
          access.sequence == 19 && access.deadline == 26 &&
          access.address == 0x80000048 && access.state == State::FINITE,
          "native register, address and prediction values");
    const float contribution = (1.0f / 32.0f) / 4.0f;
    uint32_t data = 0;
    std::memcpy(&data, &contribution, sizeof(data));
    check(data == 0x3c000000, "unchanged F32 data is not the encoded mask");
    WayState ways[2];
    ways[0].property = ways[1].property = true;
    ways[0].state = ways[1].state = State::FINITE;
    ways[0].grasp_tier = ways[1].grasp_tier = 1;
    ways[0].recency = 18;
    ways[1].recency = 19;
    ways[0].quantized_deadline =
        18 + decodeScaleRecord32(scale.records[17], 26).distance;
    ways[1].quantized_deadline = 19 + compact.distance;
    check(ways[0].quantized_deadline == 21 && ways[1].quantized_deadline == 26 &&
          distanceRRPV(2) == 0 && distanceRRPV(7) == 1, "cache snapshot scores");
    check(ways[0].recency < ways[1].recency &&
          selectVictim(ways, 2, 19, false, nullptr, 32) == 1,
          "LRU chooses A, actual ECG helper chooses B");
    ways[0].quantized_deadline = 18 + decodeRecord32(rich.records[17], 5).distance;
    ways[1].quantized_deadline = 19 + full.distance;
    check(ways[0].quantized_deadline == 20 && ways[1].quantized_deadline == 23 &&
          selectVictim(ways, 2, 19, false, nullptr, 21) == 1,
          "richer encoding gives the same worked victim ordering");
    const std::array<int, 2> after_lru = {2, 1};
    const std::array<int, 2> after_ecg = {0, 2};
    const int next_line = destinations[19] / 16;
    check(std::find(after_lru.begin(), after_lru.end(), next_line) == after_lru.end() &&
          std::find(after_ecg.begin(), after_ecg.end(), next_line) != after_ecg.end(),
          "the next A access is a miss versus a hit in the teaching snapshot");
    ways[0].quantized_deadline = 19 + 64;
    ways[1].state = State::UNKNOWN;
    ways[1].rrpv = 0;
    check(selectVictim(ways, 2, 19, false, nullptr, 32) == 0,
          "unknown does not unconditionally precede finite");
    ways[1].property = false;
    check(selectVictim(ways, 2, 19, false, nullptr, 32) == 1,
          "non-property precedes ordinary property candidates");
    ways[0].state = State::DEAD;
    check(selectVictim(ways, 2, 19, false, nullptr, 32) == 0,
          "explicit DEAD precedes non-property");
    check(extractAction(rich.records[18], 5) == 0 &&
          selectScalePrefetchDelta(scale.records, 18, 26) == 0,
          "the actual running graph supplies no prefetch candidate here");
    const unsigned lines[16] = {0,0,1,1,2,2,0,2,3,3,4,3,4,5,5,5};
    uint32_t records[16];
    for (unsigned i = 0; i < 16; ++i)
        records[i] = packScaleRecord32(lines[i] * 16, 0, 26);
    records[8] = packScaleRecord32(48, encodeScaleToken(31, State::FINITE), 26);
    records[10] = packScaleRecord32(64, encodeScaleToken(7, State::FINITE), 26);
    records[13] = packScaleRecord32(80, encodeScaleToken(15, State::FINITE), 26);
    check(selectScalePrefetchDelta(records, 16, 0, 26) == 10,
          "separate positive selector case retains lead-ten tie preference");
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures != 0;
}
'''
    source.write_text(code.replace("@OFFSETS@", ",".join(map(str, offsets))).replace(
        "@DESTINATIONS@", ",".join(map(str, stream))))
    built = subprocess.run(
        [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
         f"-I{ROOT / 'bench/include'}", str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr


def test_graph_edges_do_not_cross_unrelated_vertices(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "scripts/docs"))
    import generate_ecg_figures as generator
    fixture = generator.load_fixture()
    svg, _ = generator.offline_construction(tmp_path, fixture).save()
    document = ET.parse(svg).getroot()
    circles = [
        (float(node.get("cx")), float(node.get("cy")), float(node.get("r")))
        for node in document.iter(f"{SVG_NS}circle") if node.get("r") == "23"
    ]
    edges = [
        node for node in document.iter(f"{SVG_NS}line")
        if node.get("stroke") == "#98A2B3"
    ]
    assert len(circles) == 9 and len(edges) == len(fixture.edges)
    for edge in edges:
        first = (float(edge.get("x1")), float(edge.get("y1")))
        last = (float(edge.get("x2")), float(edge.get("y2")))
        dx, dy = last[0] - first[0], last[1] - first[1]
        for x, y, radius in circles:
            if min(math.dist(first, (x, y)), math.dist(last, (x, y))) < 0.01:
                continue
            along = max(0, min(1, ((x - first[0]) * dx + (y - first[1]) * dy) /
                               (dx * dx + dy * dy)))
            distance = math.dist((x, y), (first[0] + along * dx, first[1] + along * dy))
            assert distance >= radius + 2, (
                f"edge {first}->{last} visually connects an unrelated vertex at {(x, y)}")


def test_native_dependency_leaves_p17_not_the_float_result(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "scripts/docs"))
    import generate_ecg_figures as generator
    svg, _ = generator.o3_pipeline(tmp_path, generator.load_fixture()).save()
    document = ET.parse(svg).getroot()
    register = next(node for node in document.iter(f"{SVG_NS}text")
                    if (node.text or "").startswith("P17 = "))
    paths = {
        node.get("data-flow-label"): node
        for node in document.iter(f"{SVG_NS}path")
        if node.get("data-flow-label")
    }
    def points(label):
        return [(float(x), float(y)) for x, y in re.findall(
            r"[ML]\s*([\d.]+)\s+([\d.]+)", paths[label].get("d"))]
    dependency = points("P17 dependency")
    request = points("load request")
    assert dependency[0][1] == float(register.get("y"))
    for start, end in zip(dependency, dependency[1:]):
        if start[1] == end[1] == request[0][1] == request[-1][1]:
            overlap = min(max(start[0], end[0]), max(request[0][0], request[-1][0])) - max(
                min(start[0], end[0]), min(request[0][0], request[-1][0]))
            assert overlap <= 0, "operand and address arrows must not share an opposing segment"


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
    return {
        path.name: " ".join(node.text or "" for node in
                           ET.parse(path).getroot().iter(f"{SVG_NS}text"))
        for path in (ROOT / "fig/wiki").rglob("*.svg")
    }


def test_graph_budget_and_both_encodings_are_visible():
    rendered = figures()
    word = rendered["reuse-plan-flowthrough-f02-record-formats.svg"]
    for token in ("Full14", "Scale6", "13 remain unused", "[31:19]", "[4:0]",
                  "0x00002212", "0x10000012", "8 / 2 / 4", "10 / 2 / 2",
                  "12 / 2 / 0", "2..32 FINITE", "33..63 WRAP",
                  "Native rich-format decode is not implemented"):
        assert token in word
    timeline = rendered["reuse-plan-flowthrough-f03-future-distance.svg"]
    for token in ("true distance = 4", "100 (probe)", "103", "101", "127",
                  "deadline 23", "deadline 26", "UNKNOWN from 24",
                  "UNKNOWN at 27", "EXPIRY IS NOT DEATH"):
        assert token in timeline
    walkthrough = rendered["property-to-cache-walkthrough-f01-checked-request.svg"]
    for token in ("0x00002200", "0x00002212", "0x10000012", "0x80000048",
                  "0x80000040", "0x3C000000", "1/128", "0x0000001310000012"):
        assert token in walkthrough


def test_cache_decision_and_prefetch_follow_the_same_graph():
    rendered = figures()
    policy = rendered["reuse-plan-flowthrough-f04-llc-policy-pipeline.svg"]
    for token in ("Oldest touch: A at 18 -> evict A",
                  "Largest score: B at 1 -> evict B",
                  "Next request p[7] at s20: miss", "Next request p[7] at s20: hit",
                  "D=21", "D=32", "Unknown uses max(RRPV, local GRASP)"):
        assert token in policy
    assert "retain until" not in policy, "a future-use prediction does not pin a line until that use"
    prefetch = rendered["reuse-plan-flowthrough-f05-lookahead-prefetch.svg"]
    for token in ("candidate leads 8..15", "Full14 action = 0",
                  "Scale6 selected lead = 0", "largest backward gap",
                  "smallest decoded future bound", "target = R[j + lead].vertex",
                  "8-entry prefetch queue", "no L1D / L2 allocation",
                  "at most 1 issue per 8", "FlowThrough is OFF"):
        assert token in prefetch
    assert "the example chooses E" not in prefetch


def test_native_views_separate_data_observations_and_retirement():
    rendered = figures()
    family = rendered["risc-v-instruction-path-f01-instruction-family.svg"]
    for token in ("26 ID bits + 6 token bits", "raw funct7 0x30", "raw funct7 0x34",
                  "0x0000001310000012", "F32 bits 0x3C000000",
                  "I1 waits for its own P17"):
        assert token in family
    pipeline = rendered["risc-v-instruction-path-f02-o3-request-pipeline.svg"]
    for token in ("Fetch", "Decode", "Rename", "Dispatch", "Issue / select",
                  "Physical registers", "AGU / payload decode", "LSQ + translation",
                  "ROB", "P17 dependency", "per load", "16 physical message slots",
                  "minimum delay: 8 cycles", "output: 1 update / cycle",
                  "I1: observe s19", "not D26", "non-touching tag lookup"):
        assert token in pipeline
    lifetime = rendered["risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg"]
    for token in ("PENDING", "discard; do not enqueue", "count STALE",
                  "install FINITE, deadline=26", "s19, ready 108",
                  "s25, ready 112", "not an O3 trace"):
        assert token in lifetime


def test_storage_domains_and_backend_limits_are_explicit():
    rendered = figures()
    budget = rendered["reuse-plan-flowthrough-f06-capacity-accounting.svg"]
    for token in ("2,603,265", "635.6 MiB", "4.97 MiB", "2.48 MiB",
                  "10 reserved / 6 data", "5 reserved / 11 data",
                  "4 reserved / 12 data", "2 reserved / 14 data",
                  "Full14", "384 KiB", "560 KiB", "not yet an equal-area comparison"):
        assert token in budget
    state = rendered["property-to-cache-walkthrough-f02-architecture-state-map.svg"]
    for token in ("Full14 default: D=21", "D=32", "24 bits", "35 bits",
                  "3,145,728", "2,624", "3,148,352", "4,587,520",
                  "3,064", "4,590,584", "not synthesized area"):
        assert token in state
    evidence = rendered["evaluation-methodology-f01-evidence-boundary.svg"]
    for token in ("Full14; ID width <=18", "fixed native 26+6",
                  "REF32 rows unsupported", "production timing gate closed",
                  "popt_target_time_charged = 0", "dirty writebacks",
                  "POPT_SE_DISTANT", "reconstructions", "fail closed"):
        assert token in evidence
    assert "1 bit" in state and re.search(r"\b1 bits\b", state) is None


def test_retired_figures_and_text_stroke_halos_do_not_return():
    for retired in ("f05-flowthrough-outcomes", "f06-structural-fairness"):
        assert not (ROOT / "fig/wiki/reuse-plan-flowthrough" /
                    f"reuse-plan-flowthrough-{retired}.svg").exists()
    assert not list((ROOT / "wiki/assets").glob("*.svg"))
    path = ROOT / "fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg"
    document = ET.parse(path).getroot()
    assert all(node.get("stroke") is None for node in document.iter(f"{SVG_NS}text"))


def test_public_graph_terminology_and_scope_are_explicit():
    text = " ".join(" ".join((ROOT / path).read_text().split()) for path in (
        "README.md", "wiki/Home.md", "wiki/ReusePlan-FlowThrough.md",
        "wiki/Property-to-Cache-Walkthrough.md"))
    for token in ("out-neighbors", "in-neighbors", "`N_out(u)`", "`N_in(u)`",
                  "`d_in(v)`", "`d_out(v)`", "outer vertex", "property vertex",
                  "Full14", "Scale6", "not an automatic"):
        assert token in text
    for imprecise in ("reader graph", "current reader", "future readers",
                      "honest traffic", "reading spine"):
        assert imprecise not in text
