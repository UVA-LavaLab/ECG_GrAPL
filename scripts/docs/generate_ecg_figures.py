#!/usr/bin/env python3
"""Generate the ECG public wiki figure set and editable Draw.io mirrors."""

from __future__ import annotations

import argparse
import bisect
import json
import math
import shutil
from dataclasses import dataclass
from pathlib import Path

from ecg_figure_lib import (
    AMBER,
    AMBER_MATTE,
    BLUE,
    BORDER,
    GRAY,
    GREEN,
    GREEN_MATTE,
    INK,
    PURPLE,
    PURPLE_MATTE,
    RED,
    ROLE_COLORS,
    WHITE,
    Figure,
    FigureTarget,
    clean_generated_roots,
)


SOURCE_ROOT = Path(__file__).resolve().parents[2]
ROOT = SOURCE_ROOT
FIXTURE_PATH = SOURCE_ROOT / "fig" / "ecg-figure-fixture.json"
CHECK_ROOT = SOURCE_ROOT / ".figure-check"


@dataclass(frozen=True)
class CheckedFixture:
    num_vertices: int
    epoch_count: int
    hot_fraction: float
    property_base: int
    property_element_bytes: int
    cache_line_bytes: int
    source_to_internal: tuple[int, ...]
    weighted_edges: tuple[tuple[int, int, int], ...]
    rows: tuple[tuple[int, ...], ...]
    row_ptr: tuple[int, ...]
    reader_counts: tuple[int, ...]
    tiers: tuple[int, ...]
    tracked_source_reader: int
    tracked_source_dest: int
    tracked_reader: int
    tracked_dest: int
    tracked_weight: int
    first_reader: int
    second_reader: int
    first_epoch: int
    second_epoch: int
    line_tier: int
    line_begin: int
    line_end: int
    line_reader_ids: tuple[int, ...]
    property_address: int
    property_line: int
    id_bits: int
    epoch_bits: int


def load_fixture() -> CheckedFixture:
    raw = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    n = int(raw["num_vertices"])
    epoch_count = int(raw["epoch_count"])
    hot_fraction = float(raw["hot_fraction"])
    source_to_internal = tuple(int(value) for value in raw["source_to_internal"])
    weighted_edges = tuple(
        (int(left), int(right), int(weight))
        for left, right, weight in raw["weighted_undirected_edges"]
    )
    rows: list[list[int]] = [[] for _ in range(n)]
    readers: list[list[int]] = [[] for _ in range(n)]
    for source_left, source_right, _weight in weighted_edges:
        left = source_to_internal[source_left]
        right = source_to_internal[source_right]
        rows[left].append(right)
        rows[right].append(left)
        readers[right].append(left)
        readers[left].append(right)
    for values in rows:
        values.sort()
    for values in readers:
        values.sort()

    order = sorted(range(n), key=lambda vertex: (-len(readers[vertex]), vertex))
    hot_count = int(hot_fraction * n)
    if hot_fraction > 0 and hot_count == 0:
        hot_count = 1
    moderate_end = min(n, hot_count * 2)
    tiers = [3] * n
    for rank, vertex in enumerate(order):
        tiers[vertex] = 1 if rank < hot_count else 2 if rank < moderate_end else 3

    tracked_source_reader = int(raw["tracked_edge"]["source_vertex"])
    tracked_source_dest = int(raw["tracked_edge"]["destination_vertex"])
    tracked_reader = source_to_internal[tracked_source_reader]
    tracked_dest = source_to_internal[tracked_source_dest]
    tracked_weight = next(
        weight
        for left, right, weight in weighted_edges
        if {left, right} == {tracked_source_reader, tracked_source_dest}
    )

    vertices_per_line = int(raw["cache_line_bytes"]) // int(
        raw["property_element_bytes"]
    )
    line_begin = (tracked_dest // vertices_per_line) * vertices_per_line
    line_end = min(n, line_begin + vertices_per_line)
    line_reader_ids = sorted({
        reader
        for vertex in range(line_begin, line_end)
        for reader in readers[vertex]
    })

    def quantized_future_epoch(reader: int, current: int, wrapped: bool) -> int:
        epoch = reader * epoch_count // n
        if epoch >= epoch_count:
            epoch = epoch_count - 1
        current_epoch = current * epoch_count // n
        if wrapped and epoch == current_epoch:
            epoch = epoch_count - 1 if current_epoch == 0 else current_epoch - 1
        return epoch

    candidates: list[tuple[int, int, int]] = []
    for vertex in range(line_begin, line_end):
        vertex_readers = readers[vertex]
        if not vertex_readers:
            continue
        index = bisect.bisect_right(vertex_readers, tracked_reader)
        completed_cycles = 0
        for _ in range(2):
            if index == len(vertex_readers):
                index = 0
                completed_cycles += 1
            selected = vertex_readers[index]
            absolute = selected + completed_cycles * n
            candidates.append((
                absolute - tracked_reader,
                quantized_future_epoch(
                    selected, tracked_reader, completed_cycles > 0
                ),
                selected,
            ))
            index += 1
    candidates.sort(key=lambda item: item[0])
    first_epoch = candidates[0][1]
    second_epoch = candidates[1][1] if len(candidates) > 1 else first_epoch
    first_reader = candidates[0][2]
    second_reader = candidates[1][2] if len(candidates) > 1 else first_reader

    base = int(raw["property_base"])
    element = int(raw["property_element_bytes"])
    line_bytes = int(raw["cache_line_bytes"])
    property_address = base + tracked_dest * element
    property_line = property_address & ~(line_bytes - 1)
    id_bits = max(1, math.ceil(math.log2(n)))
    epoch_bits = max(1, math.ceil(math.log2(epoch_count)))
    row_ptr = [0]
    for row in rows:
        row_ptr.append(row_ptr[-1] + len(row))

    return CheckedFixture(
        num_vertices=n,
        epoch_count=epoch_count,
        hot_fraction=hot_fraction,
        property_base=base,
        property_element_bytes=element,
        cache_line_bytes=line_bytes,
        source_to_internal=source_to_internal,
        weighted_edges=weighted_edges,
        rows=tuple(tuple(row) for row in rows),
        row_ptr=tuple(row_ptr),
        reader_counts=tuple(len(values) for values in readers),
        tiers=tuple(tiers),
        tracked_source_reader=tracked_source_reader,
        tracked_source_dest=tracked_source_dest,
        tracked_reader=tracked_reader,
        tracked_dest=tracked_dest,
        tracked_weight=tracked_weight,
        first_reader=first_reader,
        second_reader=second_reader,
        first_epoch=first_epoch,
        second_epoch=second_epoch,
        line_tier=min(tiers[line_begin:line_end]),
        line_begin=line_begin,
        line_end=line_end,
        line_reader_ids=tuple(line_reader_ids),
        property_address=property_address,
        property_line=property_line,
        id_bits=id_bits,
        epoch_bits=epoch_bits,
    )


def save(figure: Figure, generated: list[tuple[Path, Path]]) -> None:
    generated.append(figure.save())


def panel_box(
    figure: Figure,
    label: str,
    title: str,
    x: float,
    y: float,
    width: float,
    height: float,
    *,
    role: str = "neutral",
    stroke: str = BORDER,
    stroke_width: float = 1.5,
) -> tuple[float, float, float, float]:
    strong, _ = ROLE_COLORS[role]
    figure.rect(
        x, y, width, height, role=role, stroke=stroke,
        stroke_width=stroke_width, radius=0,
    )
    figure.text(
        x + 16, y + 28, f"({label})", size=18, bold=True,
        color=strong, max_width=36,
    )
    figure.text(
        x + 58, y + 28, title, size=20, bold=True,
        color=INK, max_width=width - 74,
    )
    figure.line((x + 16, y + 46), (x + width - 16, y + 46), color=GRAY, width=1)
    return x + 16, y + 72, width - 32, height - 88


def box_title(
    figure: Figure,
    x: float,
    y: float,
    width: float,
    title: str,
    *,
    color: str = INK,
    mono: bool = False,
) -> None:
    figure.text(
        x + width / 2, y + 26, title, size=18, bold=True,
        color=color, mono=mono, anchor="middle", max_width=width - 18,
    )


def tracked_sources_for_dest(fx: CheckedFixture) -> tuple[int, ...]:
    return tuple(sorted({
        right if left == fx.tracked_source_dest else left
        for left, right, _weight in fx.weighted_edges
        if fx.tracked_source_dest in {left, right}
    }))


def tracked_fixture_neighbors(fx: CheckedFixture) -> tuple[tuple[int, int], ...]:
    pairs = []
    for left, right, weight in fx.weighted_edges:
        if fx.tracked_source_reader in {left, right}:
            other = right if left == fx.tracked_source_reader else left
            pairs.append((other, weight))
    return tuple(sorted(pairs))


def tier_name(tier: int) -> str:
    return {1: "hot", 2: "moderate", 3: "cold"}[tier]


def system_overview(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("home", "01", "system-overview"),
        "ECG dataflow from graph preprocessing to LLC replacement",
        "The diagram separates offline graph analysis, runtime requests, cache policy, and evaluation scope.",
        "The figure links offline graph analysis, the validated "
        "ReusePlan stream, the two-load runtime path, LLC metadata, and the "
        "simulator evidence boundary for the fixture edge 4->7.",
        560,
    )

    panel_box(figure, "a", "Offline record path", 24, 24, 1152, 200, role="neutral")
    figure.rect(58, 100, 170, 82, role="data", radius=0)
    box_title(figure, 58, 100, 170, "Graph storage", color=BLUE)
    figure.text(143, 154, "CSR + property arrays", anchor="middle")

    figure.rect(261, 95, 138, 92, role="compute", radius=0)
    figure.text(330, 136, "ReusePlan", size=18, bold=True, color=GREEN, anchor="middle")
    figure.text(330, 160, "tier + e1/e2", color=GREEN, anchor="middle")

    figure.rect(430, 100, 210, 82, role="state", radius=0)
    box_title(figure, 430, 100, 210, "Record array", color=PURPLE)
    figure.text(535, 154, "dest18 | T1 | e11/e15", mono=True, anchor="middle")

    figure.line((710, 92), (710, 188), color=RED, width=2)
    figure.text(724, 112, "ROI", size=18, bold=True, color=RED)

    figure.rect(762, 100, 182, 82, role="state", radius=0)
    box_title(figure, 762, 100, 182, "O3 load pair", color=PURPLE)
    figure.text(853, 154, "I0 record | I1 property", anchor="middle")

    figure.rect(984, 100, 154, 82, role="state", radius=0)
    box_title(figure, 984, 100, 154, "LLC state", color=PURPLE)
    figure.text(1061, 154, "tier/e1/e2 + RRPV", anchor="middle")

    for start, end, label, color, x in (
        ((228, 141), (261, 141), "build", BLUE, 244),
        ((399, 141), (430, 141), "pack", GREEN, 414),
        ((640, 141), (762, 141), "stream", AMBER, 702),
        ((944, 141), (984, 141), "bind", BLUE, 964),
    ):
        figure.arrow((start, end), kind="control", label=label, color=color, label_at=(x, 76))
    figure.text(
        600, 204,
        f"tracked 4->7 -> internal 8->18 -> 0x{fx.property_address:08X} -> line 0x{fx.property_line:08X}",
        mono=True, color=RED, anchor="middle", max_width=1040,
    )

    panel_box(figure, "b", "Runtime requests", 24, 248, 556, 286, role="neutral")
    runtime_nodes = ((100, "LSQ"), (236, "private"), (372, "MSHR"), (508, "LLC"))
    for x, label in runtime_nodes:
        figure.line((x, 332), (x, 492), color=GRAY, width=1)
        figure.text(x, 314, label, bold=True, anchor="middle")
    figure.arrow(
        ((70, 366), (532, 366)),
        kind="transfer",
        label="record Request",
        cadence="per edge",
        color=AMBER,
        label_at=(176, 350),
    )
    figure.arrow(
        ((70, 438), (532, 438)),
        kind="transfer",
        label="property Request",
        cadence="per ReuseBind load",
        color=BLUE,
        label_at=(196, 422),
    )
    for x in (100, 236, 372, 508):
        figure.circle(x, 366, 8, fill=WHITE, stroke=AMBER)
        figure.circle(x, 438, 8, fill=WHITE, stroke=BLUE)
    figure.text(100, 396, "FlowThrough=1", color=AMBER, anchor="middle")
    figure.text(372, 396, "alloc OR", anchor="middle")
    figure.text(508, 396, "lookup ok", anchor="middle")
    figure.text(100, 468, "ReuseBind", color=BLUE, anchor="middle")
    figure.text(372, 468, "merge newest", anchor="middle")
    figure.text(508, 468, "guard/stamp", anchor="middle")

    panel_box(figure, "c", "Evidence boundary", 596, 248, 580, 286, role="neutral")
    figure.table(620, 316, 532, 150, 2, cols=4, role="neutral")
    for col, title in enumerate(("gem5 O3", "cache_sim", "Sniper", "P-OPT")):
        figure.text(686 + col * 133, 346, title, size=18, bold=True, anchor="middle")
    top = ("timing", "traffic", "direction", "bound")
    bottom = ("exact binding", "no cycles", "diagnostic", "omit latency")
    for col, value in enumerate(top):
        figure.text(686 + col * 133, 392, value, anchor="middle")
    for col, value in enumerate(bottom):
        figure.text(686 + col * 133, 440, value, anchor="middle")
    figure.text(
        886, 494, "same-simulator baselines; receipts gate publication",
        color=RED, anchor="middle", max_width=520,
    )
    save(figure, generated)


def offline_construction(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "01", "offline-construction"),
        "Constructing an edge-aligned ReusePlan",
        "The concrete values come from fig/ecg-figure-fixture.json and its executable test.",
        "The figure shows the fixture edge 4->7, the derived internal row "
        "8->18, the line tier, the next two line accesses, and the packed record "
        "that becomes immutable runtime input.",
        706,
    )

    panel_box(figure, "a", "Fixture graph and mapped row", 24, 24, 1152, 320, role="neutral")
    center = (146, 194)
    neighbors = tracked_fixture_neighbors(fx)
    coords = {
        1: (62, 114),
        2: (128, 88),
        3: (212, 106),
        5: (238, 202),
        7: (262, 278),
    }
    figure.circle(center[0], center[1], 24, fill=AMBER_MATTE, stroke=AMBER)
    figure.text(center[0], center[1] + 6, "4", bold=True, anchor="middle")
    figure.text(146, 288, "fixture 9-node graph", color=BLUE, anchor="middle")
    for vertex, weight in neighbors:
        x, y = coords[vertex]
        if vertex == fx.tracked_source_dest:
            figure.arrow(
                ((center[0] + 20, center[1] + 18), (x - 20, y - 16)),
                kind="model-edge",
                color=RED,
                width=3,
            )
        else:
            figure.line(center, (x, y), color=GRAY, width=2)
        figure.circle(
            x, y, 22,
            fill=PURPLE_MATTE if vertex == 7 else WHITE,
            stroke=RED if vertex == 7 else BLUE,
        )
        figure.text(x, y + 6, str(vertex), bold=True, anchor="middle")
        figure.text((center[0] + x) / 2, (center[1] + y) / 2 - 10, f"w{weight}", color=GRAY, anchor="middle")
    figure.text(56, 314, "tracked 4->7, w5", color=RED, max_width=270)

    figure.rect(350, 94, 360, 210, role="data", radius=0)
    box_title(figure, 350, 94, 360, "Internal CSR row u=8", color=BLUE)
    figure.text(530, 148, "row_ptr[8]=14; row_ptr[9]=19", mono=True, anchor="middle")
    figure.table(380, 176, 300, 92, 2, cols=5, role="neutral")
    for index, edge_pos in enumerate(range(fx.row_ptr[8], fx.row_ptr[9])):
        x = 410 + index * 60
        figure.text(x, 204, str(edge_pos), color=GRAY, anchor="middle")
    for index, value in enumerate(fx.rows[fx.tracked_reader]):
        x = 410 + index * 60
        figure.text(x, 250, str(value), mono=True, anchor="middle")
    figure.text(530, 292, "edge_pos 18 -> dest 18", color=RED, anchor="middle")

    figure.rect(754, 94, 390, 210, role="state", radius=0)
    box_title(figure, 754, 94, 390, "Tier from access counts", color=PURPLE)
    figure.table(784, 148, 330, 116, 4, cols=4, role="neutral")
    headers = ("vertex", "d_in", "rank", "tier")
    for col, header in enumerate(headers):
        figure.text(825 + col * 82.5, 176, header, bold=True, mono=col == 0, anchor="middle")
    rows = (
        ("v2/int6", fx.reader_counts[6], "0", "T1"),
        ("v4/int8", fx.reader_counts[8], "1", "T1"),
        ("v7/int18", fx.reader_counts[18], "3", "T1"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 176 + row_index * 29
        for col, value in enumerate(values):
            figure.text(825 + col * 82.5, y, str(value), mono=col < 3, anchor="middle")
    figure.text(
        949, 292,
        f"min(T18=1, T20=2) = T{fx.line_tier} ({tier_name(fx.line_tier)})",
        mono=True, color=GREEN, anchor="middle", max_width=360,
    )

    panel_box(figure, "b", "Next two line accesses", 24, 432, 556, 246, role="neutral")
    figure.line((74, 560), (538, 560), color=INK, width=3)
    tick_values = (0, fx.tracked_reader, fx.first_reader, fx.second_reader, fx.epoch_count - 1)
    for value in tick_values:
        x = 74 + 464 * value / (fx.epoch_count - 1)
        color = AMBER if value == fx.tracked_reader else GREEN if value == fx.first_reader else PURPLE if value == fx.second_reader else GRAY
        figure.line((x, 542), (x, 578), color=color, width=2)
        figure.circle(x, 560, 10, fill=WHITE, stroke=color)
        figure.text(x, 522 if value != fx.second_reader else 600, str(value), bold=True, color=color, anchor="middle")
    figure.text(
        302, 628,
        f"0x{fx.property_line:08X} sources: {list(fx.line_reader_ids)}",
        mono=True, color=PURPLE, anchor="middle", max_width=520,
    )
    figure.text(
        302, 654,
        f"current 8 -> e1 {fx.first_epoch} via 11 -> e2 {fx.second_epoch} via 15",
        mono=True, color=RED, anchor="middle", max_width=520,
    )

    panel_box(figure, "c", "Packed record and validation", 604, 432, 572, 246, role="neutral")
    figure.bitfield(
        630, 514, 520, 92,
        (
            ("dest 18", fx.id_bits, "data"),
            ("T1", 2, "transfer"),
            ("e1 11", fx.epoch_bits, "compute"),
            ("e2 15", fx.epoch_bits, "state"),
        ),
        total_bits=fx.id_bits + 2 + 2 * fx.epoch_bits,
    )
    figure.text(
        890, 642,
        f"compact width = {fx.id_bits} + 2 + 2*{fx.epoch_bits} = 17 bits",
        mono=True, color=AMBER, anchor="middle", max_width=500,
    )
    figure.text(680, 666, "header", bold=True, color=RED, anchor="middle")
    figure.text(890, 666, "offsets", bold=True, color=RED, anchor="middle")
    figure.text(1100, 666, "hash/width", bold=True, color=RED, anchor="middle")
    figure.line((730, 656), (840, 656), color=RED, width=2)
    figure.line((940, 656), (1050, 656), color=RED, width=2)
    save(figure, generated)


def record_formats(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "02", "record-formats"),
        "ReusePlan record formats and structural traffic",
        "General, compact, and weighted layouts are separate transport choices.",
        "The figure shows the canonical 64-bit record, the fixture's 32-bit "
        "compact layout, and the two weighted SSSP transports with their byte "
        "accounting constraints.",
        620,
    )

    panel_box(figure, "a", "Canonical 64-bit layout", 24, 24, 1152, 166, role="neutral")
    figure.bitfield(
        60, 92, 1080, 74,
        (
            ("destination", 32, "data"),
            ("tier", 2, "transfer"),
            ("epoch 1", 15, "compute"),
            ("epoch 2", 15, "state"),
        ),
        total_bits=64,
    )
    figure.text(
        600, 184,
        "tier 0 invalid; epoch count <= 2^15",
        mono=True, color=PURPLE, anchor="middle", max_width=880,
    )

    panel_box(figure, "b", "Compact 32-bit layout", 24, 214, 1152, 174, role="neutral")
    figure.bitfield(
        60, 284, 1080, 74,
        (
            ("dest 5", fx.id_bits, "data"),
            ("tier 2", 2, "transfer"),
            ("e1 5", fx.epoch_bits, "compute"),
            ("e2 5", fx.epoch_bits, "state"),
            ("zero/reserved", 32 - fx.id_bits - 2 - 2 * fx.epoch_bits, "neutral"),
        ),
        total_bits=32,
    )
    figure.text(
        312, 382,
        "unused high bits are zero/reserved",
        color=RED, anchor="middle", max_width=360,
    )
    figure.text(
        874, 382,
        "width receipt must match the array",
        color=PURPLE, anchor="middle", max_width=430,
    )

    panel_box(figure, "c", "Weighted SSSP transports", 24, 412, 1152, 184, role="neutral")
    figure.bitfield(
        60, 480, 420, 74,
        (
            ("d24", 24, "data"),
            ("w8", 8, "neutral"),
            ("T", 2, "transfer"),
            ("e1", 15, "compute"),
            ("e2", 15, "state"),
        ),
        total_bits=64,
    )
    figure.text(310, 578, "compact substitute: 8 bytes", color=BLUE, anchor="middle")
    figure.bitfield(
        560, 480, 220, 74,
        (
            ("d32", 32, "data"),
            ("w32", 32, "neutral"),
        ),
        total_bits=64,
    )
    figure.text(820, 518, "+", size=20, bold=True, anchor="middle")
    figure.bitfield(
        860, 480, 280, 74,
        (
            ("T", 2, "transfer"),
            ("e1", 15, "compute"),
            ("e2", 15, "state"),
        ),
        total_bits=32,
    )
    figure.text(850, 578, "edge + 4-byte sidecar", color=PURPLE, anchor="middle")
    save(figure, generated)


def future_distance(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    current = fx.tracked_reader
    d1 = (fx.first_epoch + fx.epoch_count - current) % fx.epoch_count
    d2 = (fx.second_epoch + fx.epoch_count - current) % fx.epoch_count
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "03", "future-distance"),
        "Quantized next-reference distance for one property line",
        f"The tracked property line is subsequently accessed from outer vertices {fx.first_reader} and {fx.second_reader} after {fx.tracked_reader}.",
        "The figure contains one fixture timeline, the circular "
        "distance calculation, and the RRIP-first ordering rule.",
        620,
    )

    panel_box(figure, "a", "Fixture property-line timeline", 24, 24, 1152, 206, role="neutral")
    figure.line((74, 142), (1126, 142), color=INK, width=3)
    for value in (0, current, fx.first_reader, fx.second_reader, fx.epoch_count - 1):
        x = 74 + 1052 * value / (fx.epoch_count - 1)
        color = AMBER if value == current else GREEN if value == fx.first_reader else PURPLE if value == fx.second_reader else GRAY
        figure.line((x, 122), (x, 162), color=color, width=2)
        figure.circle(x, 142, 10, fill=WHITE, stroke=color)
        figure.text(x, 104 if value != fx.second_reader else 188, str(value), bold=True, color=color, anchor="middle")
    figure.text(
        292, 204,
        f"0x{fx.property_line:08X} sources: {list(fx.line_reader_ids)}",
        mono=True, color=PURPLE, anchor="middle", max_width=516,
    )
    figure.text(
        878, 204,
        f"fixture 4->7 / internal 8->18: e1={fx.first_epoch}, e2={fx.second_epoch}",
        mono=True, color=RED, anchor="middle", max_width=500,
    )

    panel_box(figure, "b", "Circular distance", 24, 254, 556, 342, role="neutral")
    ring_cx, ring_cy, ring_r = 150, 414, 82
    figure.circle(ring_cx, ring_cy, ring_r, fill=PURPLE_MATTE, stroke=PURPLE)
    figure.circle(ring_cx, ring_cy, 56, fill=WHITE, stroke=WHITE)
    figure.text(ring_cx, ring_cy - 8, "epoch", bold=True, color=PURPLE, anchor="middle")
    figure.text(ring_cx, ring_cy + 20, "mod 32", color=PURPLE, anchor="middle")
    for epoch, color, label in (
        (current, AMBER, f"c={current}"),
        (fx.first_epoch, GREEN, f"e1={fx.first_epoch}"),
        (fx.second_epoch, PURPLE, f"e2={fx.second_epoch}"),
    ):
        angle = -math.pi / 2 + 2 * math.pi * epoch / fx.epoch_count
        x = ring_cx + ring_r * math.cos(angle)
        y = ring_cy + ring_r * math.sin(angle)
        figure.circle(x, y, 10, fill=WHITE, stroke=color)
        figure.text(
            ring_cx + (ring_r + 34) * math.cos(angle),
            ring_cy + (ring_r + 34) * math.sin(angle) + 6,
            label,
            bold=True,
            color=color,
            anchor="middle",
        )
    figure.lines(
        286, 344,
        (
            f"e1={fx.first_epoch} -> d1={d1}",
            f"e2={fx.second_epoch} -> d2={d2}",
            f"nearest = {min(d1, d2)}",
            "epochs stay absolute",
            "unstamped = 0",
        ),
        mono=True,
        color=GREEN,
        max_width=250,
    )

    panel_box(figure, "c", "RRIP-first ordering", 604, 254, 572, 342, role="neutral")
    figure.rect(642, 346, 188, 108, role="compute", radius=0)
    figure.text(736, 392, "eligible set", bold=True, color=GREEN, anchor="middle")
    figure.text(736, 420, "at max RRPV?", anchor="middle")
    figure.rect(874, 346, 188, 108, role="transfer", radius=0)
    figure.text(968, 392, "structural", bold=True, color=AMBER, anchor="middle")
    figure.text(968, 420, "candidate?", anchor="middle")
    figure.rect(664, 490, 162, 80, role="transfer", radius=0)
    box_title(figure, 664, 490, 162, "select oldest", color=AMBER)
    figure.text(745, 544, "structural line", anchor="middle")
    figure.rect(920, 490, 220, 80, role="state", radius=0)
    box_title(figure, 920, 490, 220, "select farthest", color=PURPLE)
    figure.text(1030, 544, "stamped distance", anchor="middle")
    figure.arrow(((830, 400), (874, 400)), kind="control", label="eligible", color=GREEN, label_at=(852, 330))
    figure.arrow(((1048, 454), (1048, 490)), kind="control", label="property", color=PURPLE, label_at=(1094, 446))
    figure.arrow(((968, 454), (968, 490), (826, 490)), kind="control", label="structural", color=AMBER, label_at=(880, 472))
    save(figure, generated)


def llc_policy(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "04", "llc-policy-pipeline"),
        "ReuseBind acceptance and RRIP-first victim selection",
        "Line updates, eligibility, structural preference, and epoch ranking are distinct decisions.",
        "The figure separates three steps: accept or reject the Request metadata, "
        "store line-local ECG state, and apply RRIP-first only after native RRIP "
        "eligibility is reached.",
        720,
    )

    panel_box(figure, "a", "Accept or reject ReuseBind", 24, 24, 1152, 188, role="neutral")
    figure.rect(52, 92, 222, 82, role="state", radius=0)
    box_title(figure, 52, 92, 222, "ReuseBind", color=PURPLE)
    figure.text(163, 146, "dest | tier | e1 | e2", mono=True, anchor="middle")
    gates = (
        (420, "context != 0?", "verify", RED),
        (664, "conflict == 0?", "verify", RED),
        (910, "dest line match?", "compute", GREEN),
    )
    for cx, label, role, color in gates:
        figure.rect(cx - 89, 87, 178, 92, role=role, radius=0)
        first, second = label.split(" ", 1)
        figure.text(cx, 125, first, bold=True, color=color, anchor="middle")
        figure.text(cx, 149, second, anchor="middle")
    figure.rect(1046, 92, 100, 82, role="compute", radius=0)
    box_title(figure, 1046, 92, 100, "stamp", color=GREEN)
    figure.text(1096, 146, "hit/fill", anchor="middle")
    for start, end, color in (
        ((274, 133), (331, 133), PURPLE),
        ((509, 133), (575, 133), RED),
        ((753, 133), (821, 133), RED),
        ((999, 133), (1046, 133), GREEN),
    ):
        figure.line(start, end, color=color, width=3)
    figure.text(600, 188, "reject on mixed payload, invalid count, or line mismatch", color=RED, anchor="middle", max_width=1040)

    panel_box(figure, "b", "Line-local ECG state", 24, 236, 584, 460, role="neutral")
    figure.table(52, 304, 528, 252, 5, cols=5, role="neutral")
    for col, label in enumerate(("role", "RRPV", "tier", "epochs", "context")):
        figure.text(104 + col * 105.6, 334, label, bold=True, anchor="middle")
    rows = (
        ("structural", "3", "-", "-", "-"),
        ("property", "3", "T1", "11 / 15", "k"),
        ("property", "2", "T2", "20 / 20", "k"),
        ("invalid", "-", "T0", "0 / 0", "0"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 334 + row_index * 50
        for col, value in enumerate(values):
            figure.text(104 + col * 105.6, y, value, mono=col > 0, anchor="middle")
    figure.lines(
        52, 590,
        (
            "ReuseBind hit/fill refreshes tier, epochs, context",
            "ordinary hits keep native RRPV and recency",
            "invalidate clears every ECG field",
        ),
        color=PURPLE,
        max_width=500,
    )

    panel_box(figure, "c", "RRIP-first victim order", 632, 236, 544, 460, role="neutral")
    figure.rect(678, 312, 200, 88, role="compute", radius=0)
    box_title(figure, 678, 312, 200, "RRIP-eligible?", color=GREEN)
    figure.text(778, 366, "any way at max RRPV", anchor="middle")
    figure.rect(936, 312, 172, 88, role="transfer", radius=0)
    box_title(figure, 936, 312, 172, "structural?", color=AMBER)
    figure.text(1022, 366, "structural candidate", anchor="middle")
    figure.line((878, 356), (936, 356), color=GREEN, width=3)
    figure.text(908, 332, "eligible", bold=True, color=GREEN, anchor="middle")
    figure.line((1022, 400), (1022, 446), color=AMBER, width=3)
    figure.text(1058, 430, "yes", bold=True, color=AMBER)
    figure.line((936, 510), (878, 510), color=PURPLE, width=3)
    figure.text(908, 486, "no", bold=True, color=PURPLE, anchor="middle")
    figure.rect(938, 446, 170, 92, role="transfer", radius=0)
    box_title(figure, 938, 446, 170, "oldest struct", color=AMBER)
    figure.text(1023, 500, "normalized recency", anchor="middle")
    figure.rect(676, 466, 208, 108, role="state", radius=0)
    box_title(figure, 676, 466, 208, "farthest prop", color=PURPLE)
    figure.text(780, 520, "min(d(e1), d(e2))", mono=True, anchor="middle")
    figure.text(780, 548, "unstamped distance = 0", anchor="middle")
    figure.text(904, 620, "primary: RRIP-first -> structural -> epoch", color=RED, anchor="middle", max_width=460)
    save(figure, generated)


def flowthrough_outcomes(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "05", "flowthrough-outcomes"),
        "FlowThrough lookup, service, and LLC fill allocation",
        "The important corner case is an MSHR shared with an allocating target.",
        "The figure separates the unchanged lookup and service path, the shared "
        "MSHR allocOnFill corner case, and the distinction between derived "
        "structural arrays and bound property loads.",
        620,
    )

    panel_box(figure, "a", "Unchanged front half", 24, 24, 1152, 166, role="neutral")
    nodes = (
        (58, 92, 156, "Load queue", "order/replay", "state"),
        (244, 92, 126, "D-TLB", "translation", "data"),
        (400, 92, 126, "L1D", "tag + data", "data"),
        (556, 92, 126, "L2", "tag + data", "data"),
        (712, 92, 160, "LLC tags", "hit or MSHR", "compute"),
        (902, 92, 214, "Memory", "ordinary service", "verify"),
    )
    figure.arrow(
        ((58, 84), (1116, 84)),
        kind="transfer",
        label="record Request",
        cadence="per record load",
        color=AMBER,
        label_at=(588, 72),
    )
    for x, y, width, title, body, role in nodes:
        figure.rect(x, y, width, 84, role=role, radius=0)
        box_title(figure, x, y, width, title, color=BLUE if role == "data" else INK)
        figure.text(x + width / 2, y + 58, body, anchor="middle")
    figure.text(588, 186, "translation, private fills, response, and writeback stay ordinary", color=BLUE, anchor="middle", max_width=980)

    panel_box(figure, "b", "LLC miss corner case", 24, 214, 560, 382, role="neutral")
    figure.rect(88, 274, 432, 84, role="compute", radius=0)
    box_title(figure, 88, 274, 432, "LLC miss path", color=GREEN)
    figure.lines(
        168, 334,
        ("hit returns record", "miss reaches fill gate"),
        max_width=280,
    )
    figure.rect(88, 382, 432, 92, role="state", radius=0)
    box_title(figure, 88, 382, 432, "MSHR targets", color=PURPLE)
    figure.text(304, 434, "target A: allocOnFill=false", mono=True, anchor="middle")
    figure.text(304, 460, "target B: false or true", mono=True, anchor="middle")
    figure.line((304, 358), (304, 382), color=PURPLE, width=3)
    figure.rect(166, 478, 276, 64, role="transfer", radius=0)
    figure.text(304, 506, "allocOnFill OR", bold=True, color=AMBER, anchor="middle", max_width=260)
    figure.text(304, 532, "allocOnFill combines with OR", mono=True, bold=True, anchor="middle", max_width=320)
    figure.line((304, 474), (304, 478), color=PURPLE, width=3)
    figure.line((304, 542), (304, 548), color=AMBER, width=3)
    figure.line((159, 548), (429, 548), color=AMBER, width=3)
    figure.line((159, 548), (159, 556), color=AMBER, width=3)
    figure.line((429, 548), (429, 556), color=RED, width=3)
    figure.rect(34, 556, 250, 40, role="transfer", radius=0)
    figure.rect(304, 556, 250, 40, role="verify", radius=0)
    figure.text(159, 574, "all targets no-allocate", anchor="middle", max_width=236)
    figure.text(159, 594, "-> skip LLC fill", anchor="middle", max_width=236)
    figure.text(429, 574, "any allocating target", anchor="middle", max_width=236)
    figure.text(429, 594, "-> allocate LLC fill", anchor="middle", max_width=236)

    panel_box(figure, "c", "Derived arrays vs property loads", 608, 214, 568, 382, role="neutral")
    for x, label in ((706, "candidate"), (892, "flag/ext"), (1078, "LLC result")):
        figure.line((x, 328), (x, 560), color=GRAY, width=1)
        figure.text(x, 312, label, bold=True, anchor="middle")
    figure.arrow(
        ((636, 356), (1148, 356)),
        kind="transfer",
        label="derived prefetch",
        cadence="per candidate",
        color=PURPLE,
        label_at=(760, 338),
    )
    figure.arrow(
        ((636, 452), (1148, 452)),
        kind="transfer",
        label="property Request",
        cadence="per access",
        color=BLUE,
        label_at=(760, 434),
    )
    for x in (706, 892, 1078):
        figure.circle(x, 356, 8, fill=WHITE, stroke=PURPLE)
        figure.circle(x, 452, 8, fill=WHITE, stroke=BLUE)
    figure.text(706, 388, "range check", anchor="middle")
    figure.text(892, 388, "STRUCT_FLOW bit", anchor="middle")
    figure.text(1078, 388, "bit stays clear", anchor="middle")
    figure.text(706, 486, "property line guard", anchor="middle")
    figure.text(892, 484, "ReuseBind", anchor="middle")
    figure.text(1078, 484, "stamps allowed", anchor="middle")
    save(figure, generated)


def structural_fairness(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "06", "structural-fairness"),
        "Request-specific FlowThrough and matched structural-array control",
        "The two switches answer different experimental questions.",
        "The figure separates request-specific FlowThrough from the "
        "policy-independent matched control that equalizes no-allocate "
        "opportunity across compared policies.",
        520,
    )

    panel_box(figure, "a", "Design mechanism", 24, 24, 560, 472, role="neutral")
    figure.rect(56, 98, 150, 86, role="state", radius=0)
    box_title(figure, 56, 98, 150, "ReusePlan", color=PURPLE)
    figure.text(131, 152, "compact / wide / sidecar", anchor="middle")
    figure.rect(240, 98, 152, 86, role="transfer", radius=0)
    box_title(figure, 240, 98, 152, "flow.load*", color=AMBER)
    figure.text(316, 152, "record Request", anchor="middle")
    figure.rect(426, 98, 126, 86, role="transfer", radius=0)
    box_title(figure, 426, 98, 126, "FLOW", color=AMBER)
    figure.text(489, 152, "request bit", anchor="middle")
    for start, end, label, x in (
        ((206, 141), (240, 141), "record", 223),
        ((392, 141), (426, 141), "flag", 409),
    ):
        figure.arrow((start, end), kind="control", label=label, color=AMBER, label_at=(x, 106))
    figure.rect(86, 244, 438, 104, role="data", radius=0)
    box_title(figure, 86, 244, 438, "Normal hierarchy response", color=BLUE)
    figure.lines(
        108, 300,
        (
            "hit returns normally",
            "miss reaches the LLC allocation gate",
            "not bypass",
            "data footprint and latency retained",
        ),
        color=BLUE,
        max_width=400,
    )

    panel_box(figure, "b", "Matched structural-array control", 608, 24, 568, 472, role="neutral")
    figure.table(636, 98, 512, 262, 5, cols=4, role="neutral")
    for col, label in enumerate(("policy", "array", "receipt", "gate")):
        figure.text(700 + col * 128, 128, label, bold=True, anchor="middle")
    rows = (
        ("LRU", "CSR edges", "struct hits", "reject zero"),
        ("GRASP", "CSR edges", "no-alloc", "same cell"),
        ("P-OPT", "CSR + matrix", "struct event", "keep matrix"),
        ("ReusePlan", "record array", "event count", "matched array"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 128 + row_index * 52
        for col, value in enumerate(values):
            figure.text(700 + col * 128, y, value, mono=col == 0, anchor="middle", max_width=126)
    figure.lines(
        690, 372,
        (
            "cache_sim: access count",
            "gem5: no-allocate targets",
            "Sniper: read/fill counts",
        ),
        color=PURPLE,
        max_width=400,
    )
    figure.text(894, 458, "compare only after structural arrays are matched", color=RED, anchor="middle", max_width=460)
    save(figure, generated)


def instruction_family(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "01", "instruction-family"),
        "RISC-V record-load and property-load instruction roles",
        "Record acquisition and property access remain separate dynamic loads.",
        "The figure contains the control CSRs, the record-load "
        "family, and the property-load family with the explicit rd->rs2 "
        "dependency that connects them.",
        620,
    )

    panel_box(figure, "a", "Execution-control CSRs", 24, 24, 1152, 200, role="neutral")
    figure.rect(58, 92, 1084, 132, role="neutral", radius=0)
    for x in (329, 600, 871):
        figure.line((x, 92), (x, 224), color=INK, width=1)
    for col, label in enumerate(("CSR", "fields", "software update", "consumer")):
        figure.text(193 + col * 271, 116, label, bold=True, anchor="middle")
    rows = (
        ("record format", "id_bits | epoch_bits", "before ROI", "compact record load"),
        ("current epoch", "quantized traversal", "epoch boundary", "ReuseBind Request"),
        ("context", "nonzero execution ID", "execution boundary", "MSHR + LLC validation"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 126 + row_index * 28
        for col, value in enumerate(values):
            figure.text(193 + col * 271, y, value, mono=col == 1, anchor="middle", max_width=248)

    panel_box(figure, "b", "Record-load family", 24, 248, 560, 348, role="neutral")
    figure.rect(58, 308, 126, 82, role="data", radius=0)
    box_title(figure, 58, 308, 126, "custom-0", color=BLUE)
    figure.text(121, 362, "record address in rs1", anchor="middle")
    figure.rect(203, 303, 142, 92, role="compute", radius=0)
    figure.text(274, 341, "record-load", bold=True, color=GREEN, anchor="middle")
    figure.text(274, 365, "role decode", anchor="middle")
    figure.rect(372, 268, 178, 74, role="data", radius=0)
    box_title(figure, 372, 268, 178, "ecg.plan.load*", color=BLUE)
    figure.text(461, 318, "ordinary placement", anchor="middle")
    figure.rect(372, 356, 178, 74, role="transfer", radius=0)
    box_title(figure, 372, 356, 178, "ecg.flow.load*", color=AMBER)
    figure.text(461, 406, "FlowThrough placement", anchor="middle")
    figure.line((184, 349), (203, 349), color=BLUE, width=3)
    figure.line((345, 320), (372, 320), color=BLUE, width=3)
    figure.line((345, 394), (372, 394), color=AMBER, width=3)
    figure.text(358, 290, "plan", bold=True, color=BLUE, anchor="middle")
    figure.text(358, 448, "flow", bold=True, color=AMBER, anchor="middle")
    figure.lines(
        58, 472,
        (
            "general: 64-bit ReusePlan",
            "compact: 4-byte load widened to canonical rd",
            "weighted: sidecar32 + destination",
        ),
        max_width=492,
    )

    panel_box(figure, "c", "Property-load family", 608, 248, 568, 348, role="neutral")
    figure.rect(644, 300, 162, 82, role="state", radius=0)
    box_title(figure, 644, 300, 162, "physical rd", color=PURPLE)
    figure.text(725, 354, "canonical ReusePlan", anchor="middle")
    figure.rect(809, 295, 142, 92, role="compute", radius=0)
    figure.text(880, 333, "property load", bold=True, color=GREEN, anchor="middle")
    figure.text(880, 357, "address form", anchor="middle")
    figure.line((806, 341), (830, 341), color=PURPLE, width=3)
    figure.text(818, 306, "rd->rs2", bold=True, color=PURPLE, anchor="middle")
    figure.rect(980, 268, 150, 74, role="compute", radius=0)
    box_title(figure, 980, 268, 150, "bind.load.*", color=GREEN)
    figure.text(1055, 318, "rs1=EA; rs2=plan", anchor="middle")
    figure.rect(980, 356, 150, 74, role="compute", radius=0)
    box_title(figure, 980, 356, 150, "bind.iload.*", color=GREEN)
    figure.text(1055, 406, "base + dest*size", anchor="middle")
    figure.lines(
        644, 472,
        (
            "one ordinary property Request",
            "typed ReuseBind on Request",
            "FlowThrough is never attached",
            "native order / replay / retire",
        ),
        max_width=476,
    )
    save(figure, generated)


def o3_pipeline(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "02", "o3-request-pipeline"),
        "ReusePlan loads in an out-of-order core",
        f"Adjacency entry {fx.tracked_source_reader}->{fx.tracked_source_dest} maps to internal {fx.tracked_reader}->{fx.tracked_dest}, property 0x{fx.property_address:08X}, and LLC line 0x{fx.property_line:08X}.",
        "The figure follows the cross-layer dependency: graph and CSR data "
        "create a ReusePlan record, I0 produces "
        "P17, I1 consumes it, and the two Requests retain ordinary O3 ordering.",
        696,
    )

    panel_box(figure, "a", "I0 and I1 on the O3 datapath", 24, 24, 1152, 220, role="neutral")
    figure.circle(70, 104, 15, fill=AMBER, stroke=AMBER)
    figure.text(70, 110, "I0", bold=True, color=WHITE, anchor="middle")
    figure.token_line(
        96, 110,
        (
            ("flow.load.compact", AMBER, True),
            (": ", INK, False),
            (f"record[{fx.tracked_reader}->{fx.tracked_dest}]", BLUE, False),
            (" -> ", INK, False),
            ("P17", PURPLE, True),
        ),
        max_width=470,
    )
    figure.circle(628, 104, 15, fill=GREEN, stroke=GREEN)
    figure.text(628, 110, "I1", bold=True, color=WHITE, anchor="middle")
    figure.token_line(
        654, 110,
        (
            ("bind.load.u32", GREEN, True),
            (": ", INK, False),
            ("rs1", PURPLE, True),
            ("=", INK, False),
            (f"0x{fx.property_address:08X}", BLUE, False),
            (", rs2=P17 -> P21", PURPLE, True),
        ),
        mono=False,
        max_width=470,
    )
    stage_boxes = (
        (74, "Fetch"), (230, "Decode"), (386, "Rename"), (542, "Issue / select"),
        (742, "Physical regs"), (916, "AGU/LSQ"), (1072, "L1D"),
    )
    for x, label in stage_boxes:
        figure.rect(x - 58, 154, 116, 62, role="data" if label in {"Fetch", "Decode", "L1D"} else "state", radius=0)
        figure.text(x, 192, label, bold=True, anchor="middle")
    for left, right in (
        (132, 172), (288, 328), (444, 484), (600, 684), (800, 858), (974, 1014)
    ):
        figure.line((left, 185), (right, 185), color=AMBER, width=3)
    figure.arrow(((742, 216), (742, 236), (542, 236), (542, 216)), kind="dependency", label="P17 wakes I1", color=PURPLE, label_at=(642, 254))
    figure.text(586, 224, "I0 rd->P17 | I1 rs2=P17 | in-order commit", mono=True, color=RED, anchor="middle", max_width=980)

    panel_box(figure, "b", "Graph/CSR alignment", 24, 268, 1152, 150, role="neutral")
    graph_center = (124, 370)
    figure.circle(graph_center[0], graph_center[1], 24, fill=AMBER_MATTE, stroke=AMBER)
    figure.text(graph_center[0], graph_center[1] + 6, "4", bold=True, anchor="middle")
    neighbor_coords = {
        1: (58, 334),
        2: (118, 322),
        3: (188, 334),
        5: (244, 384),
        7: (214, 330),
    }
    for vertex, weight in tracked_fixture_neighbors(fx):
        x, y = neighbor_coords[vertex]
        if vertex == 7:
            figure.arrow(((graph_center[0] + 22, graph_center[1] + 18), (x - 20, y - 18)), kind="model-edge", color=RED, width=3)
        else:
            figure.line(graph_center, (x, y), color=GRAY, width=2)
        figure.circle(x, y, 22, fill=WHITE, stroke=RED if vertex == 7 else BLUE)
        figure.text(x, y + 6, str(vertex), bold=True, anchor="middle")
    figure.text(150, 410, "N_out(4) = {1,2,3,5,7}", mono=True, color=BLUE, anchor="middle", max_width=300)

    figure.rect(352, 302, 792, 106, role="neutral", radius=0)
    box_title(figure, 352, 302, 792, "Internal CSR row u=8 + ReusePlan", color=BLUE)
    figure.text(748, 354, "row_ptr[8]=14; row_ptr[9]=19", mono=True, anchor="middle")
    figure.table(396, 374, 704, 22, 1, cols=5, role="neutral")
    for index, value in enumerate((3, 6, 7, 11, 18)):
        figure.text(466 + index * 140.8, 390, str(value), mono=True, anchor="middle")
    figure.text(748, 412, "edge_pos18 -> (4,7) / (8,18) / dest18 / T1 / e11 / e15", mono=True, color=RED, anchor="middle", max_width=760)

    panel_box(figure, "c", "Two Requests from the LSQ", 24, 432, 560, 240, role="neutral")
    for x, label in ((104, "LSQ"), (246, "private"), (394, "record MSHR"), (530, "LLC")):
        figure.line((x, 510), (x, 640), color=GRAY, width=1)
        figure.text(x, 492, label, bold=True, anchor="middle", max_width=126)
    figure.arrow(((76, 538), (548, 538)), kind="transfer", label="I0 record Request", cadence="per edge", color=AMBER, label_at=(182, 522))
    figure.arrow(((76, 594), (548, 594)), kind="transfer", label="I1 property Request", cadence="per load", color=BLUE, label_at=(186, 610))
    for x in (104, 246, 394, 530):
        figure.circle(x, 538, 8, fill=WHITE, stroke=AMBER)
        figure.circle(x, 594, 8, fill=WHITE, stroke=BLUE)
    figure.text(104, 564, "4-byte record", color=AMBER, anchor="middle")
    figure.text(394, 564, "FlowThrough=1", color=AMBER, anchor="middle")
    figure.text(530, 564, "write P17", anchor="middle")
    figure.text(104, 646, "4-byte U32", color=GREEN, anchor="middle")
    figure.text(394, 646, "ReuseBind", color=BLUE, anchor="middle")
    figure.text(530, 646, f"stamp 0x{fx.property_line:08X}", mono=True, anchor="middle")

    panel_box(figure, "d", "Writeback and commit", 608, 432, 568, 240, role="neutral")
    figure.table(636, 500, 512, 98, 3, cols=3, role="neutral")
    for col, label in enumerate(("lane", "completion", "commit")):
        figure.text(722 + col * 170.6, 526, label, bold=True, anchor="middle")
    rows = (
        ("I0", "P17 ready", "ROB0 oldest"),
        ("I1", "P21 ready", "commit after I0"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 526 + row_index * 32
        for col, value in enumerate(values):
            figure.text(722 + col * 170.6, y, value, mono=col == 0, anchor="middle", max_width=156)
    figure.lines(
        652, 630,
        (
            "I0 affects record-miss LLC allocation only",
            "I1 affects matching property-line metadata only",
        ),
        color=PURPLE,
        max_width=480,
    )
    save(figure, generated)


def mshr_lifecycle(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "03", "mshr-metadata-lifecycle"),
        "ReuseBind merge, response, and line-metadata lifetime",
        "MSHR merge validity and FlowThrough allocation are separate state machines.",
        "The figure contains the typed Request extension, compatible versus "
        "conflicting MSHR merges, downstream LLC acceptance, and the advisory "
        "lifetime of the resulting line metadata.",
        578,
    )

    panel_box(figure, "a", "Typed Request extension", 24, 24, 1152, 148, role="neutral")
    widths = (108, 86, 108, 108, 92, 98, 102, 110, 104)
    labels = ("dest", "tier", "epoch1", "epoch2", "count", "current", "context", "sequence", "conflict")
    cursor = 56
    for width, label in zip(widths, labels):
        figure.rect(cursor, 92, width, 54, role="state" if label in {"dest", "tier", "epoch1", "epoch2"} else "neutral", stroke_width=1, radius=0)
        figure.text(cursor + width / 2, 125, label, mono=True, bold=True, anchor="middle", max_width=width - 8)
        cursor += width
    figure.text(600, 166, "valid iff context != 0 and conflict == 0", mono=True, color=RED, anchor="middle", max_width=900)

    panel_box(figure, "b", "MSHR target-list merge", 24, 196, 560, 358, role="neutral")
    figure.rect(52, 254, 504, 128, role="neutral", radius=0)
    for x in (154, 278, 402):
        figure.line((x, 254), (x, 382), color=INK, width=1)
    for col, label in enumerate(("target", "ReuseBind", "context", "seq/alloc")):
        figure.text(103 + col * 124, 282, label, bold=True, anchor="middle")
    rows = (
        ("A", "yes", "cpu0 / k", "17 / false"),
        ("B", "yes", "cpu0 / k", "18 / true"),
        ("C", "yes/no", "cpu? / k?", "19 / false"),
    )
    for row_index, values in enumerate(rows, start=1):
        y = 314 + (row_index - 1) * 28
        for col, value in enumerate(values):
            figure.text(103 + col * 124, y, value, mono=True, anchor="middle", max_width=116)
    figure.rect(84, 398, 188, 68, role="compute", radius=0)
    box_title(figure, 84, 398, 188, "compatible?", color=GREEN)
    figure.rect(320, 398, 216, 56, role="state", radius=0)
    box_title(figure, 320, 398, 216, "selected ext", color=PURPLE)
    figure.text(428, 470, "equal seq requires same payload", anchor="middle")
    figure.rect(320, 482, 216, 56, role="verify", radius=0)
    box_title(figure, 320, 482, 216, "conflict state", color=RED)
    figure.text(428, 530, "mixed / mismatch / invalid", anchor="middle")
    figure.text(304, 548, "allocOnFill = OR(target allocOnFill)", mono=True, color=AMBER, anchor="middle", max_width=480)

    panel_box(figure, "c", "Response, acceptance, and lifetime", 608, 196, 568, 358, role="neutral")
    figure.rect(636, 250, 166, 52, role="state", radius=0)
    box_title(figure, 636, 250, 166, "response", color=PURPLE)
    figure.rect(828, 250, 132, 52, role="verify", radius=0)
    box_title(figure, 828, 250, 132, "conflicted?", color=RED)
    figure.rect(996, 250, 144, 52, role="compute", radius=0)
    box_title(figure, 996, 250, 144, "line match?", color=GREEN)
    figure.line((802, 276), (828, 276), color=PURPLE, width=3)
    figure.line((960, 276), (996, 276), color=GREEN, width=3)
    figure.text(719, 320, "selected ext + conflict", anchor="middle")
    figure.text(1070, 322, "stamp", bold=True, color=GREEN)
    figure.rect(676, 354, 430, 86, role="state", radius=0)
    box_title(figure, 676, 354, 430, "LLC line metadata", color=PURPLE)
    figure.text(891, 414, "tier | e1 | e2 | context | valid", mono=True, anchor="middle")
    figure.line((1070, 302), (1070, 354), color=GREEN, width=3)
    for x, title, body, color in (
        (692, "accept", "hit/fill", GREEN),
        (826, "refresh", "ReuseBind hit", BLUE),
        (958, "evolve", "native RRPV", PURPLE),
        (1088, "clear", "invalidate", RED),
    ):
        figure.circle(x, 492, 11, fill=WHITE, stroke=color)
        figure.text(x, 526, title, bold=True, color=color, anchor="middle")
        figure.text(x, 550, body, anchor="middle")
    figure.line((692, 492), (1088, 492), color=PURPLE, width=3)
    figure.text(888, 470, "line metadata lifetime", bold=True, color=PURPLE, anchor="middle")
    save(figure, generated)


def checked_walkthrough(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    current = fx.tracked_reader
    d1 = (fx.first_epoch + fx.epoch_count - current) % fx.epoch_count
    d2 = (fx.second_epoch + fx.epoch_count - current) % fx.epoch_count
    figure = Figure(
        ROOT,
        FigureTarget("property-to-cache-walkthrough", "01", "checked-request"),
        "From adjacency entry 4 -> 7 to LLC line 0x80000040",
        "Every number is derived from fig/ecg-figure-fixture.json.",
        "The figure follows fixture adjacency 4->7 into the compact "
        "ReusePlan record, the explicit rd->rs2 dependency, the property Request "
        "with ReuseBind, and the later victim-time interpretation of e1/e2.",
        850,
    )

    panel_box(figure, "a", "Tracked entry and compact record", 24, 24, 560, 230, role="neutral")
    readers = tracked_sources_for_dest(fx)
    line_x, line_y = 412, 156
    figure.rect(336, 96, 176, 118, role="state", radius=0)
    box_title(figure, 336, 96, 176, "64-byte line", color=PURPLE)
    figure.text(424, 146, "vertices 16..31", anchor="middle")
    figure.circle(424, 184, 26, fill=PURPLE_MATTE, stroke=RED)
    figure.text(424, 191, "18", bold=True, anchor="middle")
    for index, reader in enumerate(readers):
        x = 94
        y = 102 + index * 32
        figure.circle(
            x, y, 15,
            fill=AMBER_MATTE if reader == fx.tracked_source_reader else WHITE,
            stroke=AMBER if reader == fx.tracked_source_reader else BLUE,
        )
        figure.text(x, y + 6, str(reader), bold=True, anchor="middle")
        figure.line((x + 15, y), (336, line_y), color=RED if reader == fx.tracked_source_reader else GRAY, width=2)
    figure.text(44, 232, "in-neighbors of vertex 7", color=BLUE, max_width=240)
    figure.bitfield(
        300, 196, 250, 44,
        (
            ("d18", fx.id_bits, "data"),
            ("T1", 2, "transfer"),
            ("e1", fx.epoch_bits, "compute"),
            ("e2", fx.epoch_bits, "state"),
        ),
        total_bits=fx.id_bits + 2 + 2 * fx.epoch_bits,
    )

    panel_box(figure, "b", "Software loop and identifier correlation", 608, 24, 568, 230, role="neutral")
    pseudocode = (
        ("L1", (("for ", RED, True), ("u", INK, False), (" in active:", BLUE, False))),
        ("L2", (("  for e in ", RED, True), ("row(u):", BLUE, False))),
        ("L3", (("    plan = ", INK, False), ("record load", AMBER, True))),
        ("L4", (("    value = ", INK, False), ("bind load", GREEN, True), (" [C,D]", PURPLE, True))),
    )
    for index, (line_no, tokens) in enumerate(pseudocode):
        y = 106 + index * 32
        figure.text(632, y, line_no, mono=True, color=GRAY)
        figure.token_line(676, y, tokens, max_width=340)
    figure.lines(
        942, 132,
        (
            "A: 4->7 / 8->18",
            "B: d18 | T1 | 11/15",
            "flow.load.compact",
            "bind.load.u32",
        ),
        color=PURPLE,
        max_width=200,
    )
    figure.text(892, 228, "tracked execution: 4->7 maps to 8->18", color=RED, anchor="middle", max_width=500)

    panel_box(figure, "c", "Dependency and Requests", 24, 276, 1152, 250, role="neutral")
    figure.rect(52, 332, 150, 60, role="transfer", radius=0)
    box_title(figure, 52, 332, 150, "record array", color=AMBER)
    figure.text(127, 376, "record[8->18]", mono=True, anchor="middle")
    figure.rect(228, 332, 136, 60, role="transfer", radius=0)
    box_title(figure, 228, 332, 136, "record load", color=AMBER)
    figure.text(296, 376, "widen compact", anchor="middle")
    figure.rect(390, 332, 136, 60, role="state", radius=0)
    box_title(figure, 390, 332, 136, "P17 reg", color=PURPLE)
    figure.text(458, 376, "ReusePlan", mono=True, anchor="middle")
    figure.rect(548, 332, 158, 60, role="compute", radius=0)
    box_title(figure, 548, 332, 158, "property load", color=GREEN)
    figure.text(627, 376, f"EA=0x{fx.property_address:08X}", mono=True, color=BLUE, anchor="middle")
    figure.rect(730, 316, 396, 92, role="state", radius=0)
    box_title(figure, 730, 320, 396, "LSQ Request", color=PURPLE)
    figure.text(928, 368, "dest18 | T1 | e11/e15", mono=True, anchor="middle")
    figure.text(928, 394, "current8 | context k | sequence s", mono=True, anchor="middle")
    for start, end, color in (
        ((202, 362), (228, 362), AMBER),
        ((364, 362), (390, 362), PURPLE),
        ((526, 362), (548, 362), BLUE),
        ((706, 362), (730, 362), PURPLE),
    ):
        figure.line(start, end, color=color, width=3)

    for x, label in ((120, "LSQ"), (356, "private"), (592, "MSHR"), (1008, "LLC")):
        figure.line((x, 430), (x, 514), color=GRAY, width=1)
        figure.text(x, 416, label, bold=True, anchor="middle")
    figure.arrow(((80, 458), (1112, 458)), kind="transfer", label="record Request", cadence="per edge", color=AMBER, label_at=(182, 442))
    figure.arrow(((80, 494), (1112, 494)), kind="transfer", label="property Request", cadence="per edge", color=BLUE, label_at=(188, 514))
    figure.text(356, 474, "record block; no alloc", anchor="middle")
    figure.text(1008, 474, "miss may skip fill", anchor="middle")
    figure.text(592, 514, "property block; merge ext", anchor="middle")
    figure.text(1008, 514, "guard/stamp T1/e11/e15", anchor="middle")

    panel_box(figure, "d", "Line stamp and later victim rule", 24, 544, 1152, 282, role="neutral")
    figure.rect(52, 652, 648, 170, role="state", radius=0)
    box_title(figure, 52, 652, 648, f"LLC line 0x{fx.property_line:08X}", color=PURPLE)
    axis_y = 734
    figure.line((104, axis_y), (648, axis_y), color=INK, width=3)
    for epoch, color, label in (
        (current, AMBER, f"current {current}"),
        (fx.first_epoch, GREEN, f"e1 {fx.first_epoch}"),
        (fx.second_epoch, PURPLE, f"e2 {fx.second_epoch}"),
    ):
        x = 104 + 544 * epoch / (fx.epoch_count - 1)
        figure.circle(x, axis_y, 10, fill=WHITE, stroke=color)
        figure.text(x, axis_y - 28 if epoch != fx.first_epoch else axis_y + 36, label, bold=True, color=color, anchor="middle")
    figure.text(376, 796, f"nearest = min({d1}, {d2}) = {min(d1, d2)}", mono=True, color=PURPLE, anchor="middle")
    figure.text(376, 814, "property value unchanged; metadata is advisory", color=RED, anchor="middle")
    figure.rect(764, 666, 164, 74, role="verify", radius=0)
    box_title(figure, 764, 666, 164, "max-RRPV?", color=RED)
    figure.text(846, 712, "eligible victim set", anchor="middle")
    figure.rect(970, 666, 154, 74, role="transfer", radius=0)
    box_title(figure, 970, 666, 154, "structural?", color=AMBER)
    figure.text(1047, 712, "structural line present", anchor="middle")
    figure.line((928, 702), (970, 702), color=RED, width=3)
    figure.text(948, 678, "eligible", bold=True, color=RED, anchor="middle")
    figure.text(944, 790, "yes -> oldest structural", color=AMBER)
    figure.text(944, 818, "no -> farthest property", color=PURPLE)
    save(figure, generated)


def architecture_state_map(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("property-to-cache-walkthrough", "02", "architecture-state-map"),
        "ReusePlan state placement across software, core, and LLC",
        "Containment denotes storage; arrows denote architectural operand or Request transfer.",
        "The figure uses one software-to-core-to-cache containment path so the "
        "paper can identify where immutable records stop and where live execution "
        "state begins inside O3 and the shared cache hierarchy.",
        620,
    )

    panel_box(figure, "a", "Software and architectural inputs", 24, 24, 560, 250, role="neutral")
    figure.table(52, 96, 504, 150, 4, role="neutral")
    figure.text(304, 126, "Memory image before ROI", bold=True, color=BLUE, anchor="middle")
    figure.text(304, 162, "CSR / weighted structural arrays", anchor="middle")
    figure.text(304, 198, "edge-aligned ReusePlan / sidecar arrays", anchor="middle")
    figure.text(304, 234, "property arrays + graph/payload identity", anchor="middle")

    panel_box(figure, "b", "Out-of-order core", 608, 24, 568, 250, role="neutral")
    core_boxes = (
        (628, 96, 122, "Decode", "custom-0"),
        (774, 96, 154, "Rename / ROB", "I0 rd->P17"),
        (952, 96, 114, "Issue", "wait P17"),
        (1068, 96, 92, "Req ext", "typed"),
    )
    for x, y, width, title, body in core_boxes:
        figure.rect(x, y, width, 96, role="state" if title != "Decode" else "data", radius=0)
        box_title(figure, x, y, width, title, color=PURPLE if title != "Decode" else BLUE)
        figure.text(x + width / 2, y + 62, body, mono="P17" in body, anchor="middle", max_width=width - 10)
    figure.line((928, 144), (952, 144), color=PURPLE, width=3)
    figure.text(940, 206, "rd->rs2", bold=True, color=PURPLE, anchor="middle")

    panel_box(figure, "c", "Cache hierarchy", 24, 298, 1152, 298, role="neutral")
    cache_boxes = (
        (52, 378, 156, "L1D", "ordinary tags", "data"),
        (244, 378, 156, "L2", "ordinary tags", "data"),
        (436, 378, 222, "MSHR target list", "allocOnFill + merge", "state"),
        (694, 364, 430, "LLC replacement entry", "valid | property | RRPV | recency\ntier | e1 | e2 | count\ncontext | stamp", "state"),
    )
    for x, y, width, title, body, role in cache_boxes:
        figure.rect(x, y, width, 138 if role == "data" else 166 if x >= 694 else 138, role=role, radius=0)
        box_title(figure, x, y, width, title, color=PURPLE if role == "state" else BLUE)
        for index, line in enumerate(body.split("\n")):
            figure.text(x + width / 2, y + 68 + index * 28, line, mono=x >= 694, anchor="middle", max_width=width - 18)
    figure.arrow(((84, 536), (1088, 536)), kind="transfer", label="property Request", cadence="per load", color=BLUE, label_at=(586, 518))
    figure.text(586, 570, "property bytes return normally; metadata stays advisory at the LLC", color=BLUE, anchor="middle", max_width=1000)
    save(figure, generated)


def evidence_boundary(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("evaluation-methodology", "01", "evidence-boundary"),
        "Evaluation evidence and admissible claims",
        "Rows are compared only with matching baselines inside the same simulator.",
        "The figure separates simulator roles, row-acceptance receipts, and "
        "reporting limits that keep analytic "
        "P-OPT timing clearly separated from architectural speedup claims.",
        540,
    )

    panel_box(figure, "a", "Simulator roles", 24, 24, 1152, 210, role="neutral")
    figure.table(52, 96, 1096, 122, 3, cols=4, role="neutral")
    for col, label in enumerate(("gem5 O3", "cache_sim", "Sniper", "Reported evidence")):
        figure.text(189 + col * 274, 126, label, bold=True, color=(GREEN, BLUE, PURPLE, RED)[col], anchor="middle")
    top = ("architectural time", "functional victim", "cache direction", "timing / traffic / checks")
    bottom = ("exact Request binding", "off-chip traffic", "computed sideband", "same-simulator baseline")
    note = ("native LSQ/MSHR/cache", "no cycle model", "diagnostic only", "exclude invalid rows")
    for col, value in enumerate(top):
        figure.text(189 + col * 274, 162, value, anchor="middle", max_width=248)
    for col, value in enumerate(bottom):
        figure.text(189 + col * 274, 190, value, anchor="middle", max_width=248)
    for col, value in enumerate(note):
        figure.text(189 + col * 274, 218, value, anchor="middle", max_width=248)

    panel_box(figure, "b", "Row acceptance", 24, 258, 556, 258, role="neutral")
    figure.rect(48, 324, 190, 126, role="state", radius=0)
    box_title(figure, 48, 324, 190, "Mechanism checks", color=PURPLE)
    figure.lines(68, 378, ("req == eff", "events +", "widths agree"), max_width=156)
    figure.rect(252, 324, 190, 126, role="state", radius=0)
    box_title(figure, 252, 324, 190, "Semantic checks", color=PURPLE)
    figure.lines(272, 378, ("kernel budget", "rows agree", "peer reject"), max_width=156)
    figure.rect(458, 336, 98, 102, role="verify", radius=0)
    figure.text(507, 378, "AND", bold=True, color=RED, anchor="middle")
    figure.text(507, 404, "gate", anchor="middle")
    figure.line((238, 388), (252, 388), color=PURPLE, width=3)
    figure.line((442, 388), (458, 388), color=PURPLE, width=3)
    figure.text(304, 484, "accepted row only after both vectors pass", color=GREEN, anchor="middle", max_width=480)

    panel_box(figure, "c", "Claim limits", 604, 258, 572, 258, role="neutral")
    figure.rect(632, 330, 234, 114, role="neutral", radius=0)
    figure.rect(914, 330, 234, 114, role="neutral", radius=0)
    figure.text(749, 358, "Analytic P-OPT boundary", bold=True, color=AMBER, anchor="middle")
    figure.text(1031, 358, "Reported quantities", bold=True, color=RED, anchor="middle")
    figure.text(749, 392, "charged: ways + bytes", anchor="middle", max_width=206)
    figure.text(749, 420, "omit target latency", anchor="middle", max_width=206)
    figure.text(1031, 392, "time + traffic", anchor="middle", max_width=206)
    figure.text(1031, 420, "exclude invalid rows", anchor="middle", max_width=206)
    figure.text(890, 472, "timing_valid_for_speedup=0 rows excluded", mono=True, color=RED, anchor="middle", max_width=500)
    figure.text(890, 498, "popt_target_time_charged=0 -> optimistic", mono=True, color=RED, anchor="middle", max_width=500)
    save(figure, generated)


def generate(output_root: Path = SOURCE_ROOT) -> list[tuple[Path, Path]]:
    global ROOT
    previous_root = ROOT
    ROOT = output_root
    fixture = load_fixture()
    try:
        clean_generated_roots(output_root)
        generated: list[tuple[Path, Path]] = []
        system_overview(fixture, generated)
        offline_construction(fixture, generated)
        record_formats(fixture, generated)
        future_distance(fixture, generated)
        llc_policy(generated)
        flowthrough_outcomes(generated)
        structural_fairness(generated)
        instruction_family(generated)
        o3_pipeline(fixture, generated)
        mshr_lifecycle(generated)
        checked_walkthrough(fixture, generated)
        architecture_state_map(generated)
        evidence_boundary(generated)
        return generated
    finally:
        ROOT = previous_root


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="Generate in a private repository directory and compare outputs.",
    )
    args = parser.parse_args()
    if args.check:
        before: dict[Path, bytes] = {}
        for root, suffix in (
            (SOURCE_ROOT / "fig" / "wiki", "*.svg"),
            (SOURCE_ROOT / "fig" / "wiki_src", "*.drawio"),
        ):
            if root.exists():
                before.update({
                    path.relative_to(SOURCE_ROOT): path.read_bytes()
                    for path in root.rglob(suffix)
                })
        if CHECK_ROOT.exists():
            shutil.rmtree(CHECK_ROOT)
        try:
            generated = generate(CHECK_ROOT)
            after = {
                path.relative_to(CHECK_ROOT): path.read_bytes()
                for pair in generated for path in pair
            }
        finally:
            if CHECK_ROOT.exists():
                shutil.rmtree(CHECK_ROOT)
        if before != after:
            missing = sorted(str(path) for path in before.keys() - after.keys())
            added = sorted(str(path) for path in after.keys() - before.keys())
            changed = sorted(
                str(path)
                for path in before.keys() & after.keys()
                if before[path] != after[path]
            )
            raise SystemExit(
                "generated figures differ: "
                f"missing={missing} added={added} changed={changed}"
            )
        return 0
    generated = generate(SOURCE_ROOT)
    for svg, drawio in generated:
        print(svg.relative_to(SOURCE_ROOT), drawio.relative_to(SOURCE_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
