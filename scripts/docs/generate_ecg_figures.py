#!/usr/bin/env python3
"""Generate the ECG public wiki figure set and editable Draw.io mirrors."""

from __future__ import annotations

import argparse
import bisect
import json
import math
from tempfile import TemporaryDirectory
from dataclasses import dataclass
from pathlib import Path

from ecg_figure_lib import (
    AMBER,
    AMBER_MATTE,
    BORDER,
    BLUE,
    GRAY,
    GREEN,
    GREEN_MATTE,
    INK,
    PURPLE,
    PURPLE_MATTE,
    RED,
    WHITE,
    Figure,
    FigureTarget,
    clean_generated_roots,
)


SOURCE_ROOT = Path(__file__).resolve().parents[2]
ROOT = SOURCE_ROOT
FIXTURE_PATH = SOURCE_ROOT / "fig" / "ecg-figure-fixture.json"


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
    reader_counts: tuple[int, ...]
    tiers: tuple[int, ...]
    tracked_source_reader: int
    tracked_source_dest: int
    tracked_reader: int
    tracked_dest: int
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
    ne = int(raw["epoch_count"])
    hot_fraction = float(raw["hot_fraction"])
    rows: list[list[int]] = [[] for _ in range(n)]
    readers: list[list[int]] = [[] for _ in range(n)]
    source_to_internal = tuple(int(value) for value in raw["source_to_internal"])
    weighted_edges = tuple(
        (int(left), int(right), int(weight))
        for left, right, weight in raw["weighted_undirected_edges"]
    )
    for source_left, source_right, _weight in weighted_edges:
        left = source_to_internal[source_left]
        right = source_to_internal[source_right]
        rows[left].append(right)
        readers[right].append(left)
        rows[right].append(left)
        readers[left].append(right)
    for row in rows:
        row.sort()
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
    if tracked_dest not in rows[tracked_reader]:
        raise ValueError("tracked adjacency entry is absent from the checked graph")
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
    candidates: list[tuple[int, int, int]] = []

    def quantized_future_epoch(reader: int, current: int, wrapped: bool) -> int:
        epoch = reader * ne // n
        if epoch >= ne:
            epoch = ne - 1
        current_epoch = current * ne // n
        if wrapped and epoch == current_epoch:
            epoch = ne - 1 if current_epoch == 0 else current_epoch - 1
        return epoch

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
    if not candidates:
        raise ValueError("tracked property line has no access-source vertex")
    candidates.sort(key=lambda item: item[0])
    first_epoch = candidates[0][1]
    second_epoch = candidates[1][1] if len(candidates) > 1 else first_epoch
    first_reader = candidates[0][2]
    second_reader = candidates[1][2] if len(candidates) > 1 else first_reader
    line_tier = min(tiers[line_begin:line_end])
    base = int(raw["property_base"])
    element = int(raw["property_element_bytes"])
    line_bytes = int(raw["cache_line_bytes"])
    address = base + tracked_dest * element
    line = address & ~(line_bytes - 1)
    id_bits = max(1, math.ceil(math.log2(n)))
    epoch_bits = max(1, math.ceil(math.log2(ne)))
    return CheckedFixture(
        num_vertices=n,
        epoch_count=ne,
        hot_fraction=hot_fraction,
        property_base=base,
        property_element_bytes=element,
        cache_line_bytes=line_bytes,
        source_to_internal=source_to_internal,
        weighted_edges=weighted_edges,
        rows=tuple(tuple(row) for row in rows),
        reader_counts=tuple(len(values) for values in readers),
        tiers=tuple(tiers),
        tracked_source_reader=tracked_source_reader,
        tracked_source_dest=tracked_source_dest,
        tracked_reader=tracked_reader,
        tracked_dest=tracked_dest,
        first_reader=first_reader,
        second_reader=second_reader,
        first_epoch=first_epoch,
        second_epoch=second_epoch,
        line_tier=line_tier,
        line_begin=line_begin,
        line_end=line_end,
        line_reader_ids=tuple(line_reader_ids),
        property_address=address,
        property_line=line,
        id_bits=id_bits,
        epoch_bits=epoch_bits,
    )


def save(figure: Figure, generated: list[tuple[Path, Path]]) -> None:
    generated.append(figure.save())


def system_overview(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("home", "01", "system-overview"),
        "ECG dataflow from graph preprocessing to LLC replacement",
        "The diagram separates offline graph analysis, runtime requests, cache policy, and evaluation scope.",
        "Three numbered bands show ECG Next's offline graph analysis, the two-load "
        "RISC-V runtime path, line-local LLC replacement state, and the evidence "
        "boundary across gem5 O3, cache_sim, and Sniper. The tracked checked-fixture "
        f"adjacency entry has outer vertex {fx.tracked_reader} and property vertex "
        f"{fx.tracked_dest}.",
        1060,
    )
    figure.section(
        "1", "CROSS-LAYER DATAFLOW", "offline construction ends before the measured ROI",
        138, role="data",
    )
    figure.table(35, 195, 180, 170, 4, role="data")
    figure.text(125, 225, "Graph storage", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(50, 270, "row_ptr / col_idx", size=16, mono=True)
    figure.text(50, 312, "weight / property", size=16, mono=True)
    figure.text(50, 352, "kernel direction", size=16)
    figure.diamond(315, 280, 150, 130, role="compute")
    figure.text(315, 275, "ReusePlan", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(315, 305, "builder", size=16, anchor="middle")
    figure.text(315, 370, "rank d_in / d_out", size=16,
                color=GREEN, anchor="middle")
    figure.table(430, 195, 190, 170, 4, role="state")
    figure.text(525, 225, "Record array", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(445, 270, "destination", size=16, mono=True)
    figure.text(445, 312, "tier | e1 | e2", size=16, mono=True)
    figure.text(445, 352, "hash + width receipt", size=16)
    figure.line((660, 180), (660, 410), color=RED, width=3)
    figure.text(675, 205, "measured ROI", size=16, bold=True, color=RED)
    figure.queue(700, 210, 150, 135, role="state")
    figure.text(775, 240, "O3 issue", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(775, 278, "I0 record load", size=16, anchor="middle")
    figure.text(775, 315, "I1 property load", size=16, anchor="middle")
    figure.table(900, 195, 120, 170, 3, role="transfer")
    figure.text(960, 225, "MSHR", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(960, 282, "targets", size=16, anchor="middle")
    figure.text(960, 338, "merge", size=16, anchor="middle")
    figure.table(1060, 195, 110, 170, 3, role="state")
    figure.text(1115, 225, "LLC", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(1115, 282, "RRPV", size=16, anchor="middle")
    figure.text(1115, 338, "tier/e1/e2", size=16, anchor="middle")
    for start, end, label, color in (
        ((215, 280), (240, 280), "Graph storage", BLUE),
        ((390, 280), (430, 280), "ReusePlan", PURPLE),
        ((620, 280), (700, 280), "Record array", AMBER),
        ((850, 280), (900, 280), "O3 issue", GREEN),
        ((1020, 280), (1060, 280), "MSHR", PURPLE),
    ):
        figure.arrow(
            (start, end), kind="control", label=label, color=color,
        )
    figure.text(
        675, 385,
        f"tracked: fixture 4->7 -> internal {fx.tracked_reader}->"
        f"{fx.tracked_dest}",
        size=16, mono=True, color=RED, max_width=480,
    )
    figure.text(
        675, 415,
        f"property address 0x{fx.property_address:08X}",
        size=16, mono=True, color=RED, max_width=480,
    )

    figure.section(
        "2", "REQUEST AND LLC DECISION PATH",
        "FlowThrough placement and ReuseBind state are independent",
        450, role="compute",
    )
    figure.arrow(
        ((60, 560), (1130, 560)),
        kind="transfer", label="I0 record Request",
        cadence="per adjacency entry", color=AMBER, width=3,
        label_at=(300, 545),
    )
    figure.arrow(
        ((60, 680), (1130, 680)),
        kind="transfer", label="I1 property Request + ReuseBind",
        cadence="per governed load", color=BLUE, width=3,
        label_at=(340, 665),
    )
    nodes = (
        (100, "LSQ"),
        (315, "D-TLB / L1 / L2"),
        (530, "MSHR"),
        (745, "LLC hit/fill"),
        (960, "RRIP-first"),
    )
    for x, label in nodes:
        figure.line((x, 520), (x, 735), color=GRAY, width=1)
        figure.circle(x, 560, 10, fill=WHITE, stroke=AMBER)
        figure.circle(x, 680, 10, fill=WHITE, stroke=BLUE)
        figure.text(x, 505, label, size=16, bold=True, anchor="middle")
    figure.text(100, 595, "FlowThrough=1", size=16,
                color=AMBER, anchor="middle")
    figure.text(530, 595, "allocOnFill OR", size=16, anchor="middle")
    figure.text(745, 595, "hit normal; miss may bypass fill",
                size=16, anchor="middle")
    figure.text(100, 715, "ReuseBind", size=16,
                color=BLUE, anchor="middle")
    figure.text(530, 715, "newest compatible target", size=16,
                anchor="middle")
    figure.text(745, 715, "validate + stamp", size=16,
                anchor="middle")
    figure.text(960, 715, "structural -> distance", size=16,
                anchor="middle")

    figure.section(
        "3", "EVIDENCE BOUNDARY", "mechanism activity is not itself a speedup claim",
        780, role="verify",
    )
    figure.rect(40, 830, 1120, 150, role="neutral", radius=0)
    for x in (320, 600, 880):
        figure.line((x, 830), (x, 980), color=INK, width=1)
    figure.line((40, 880), (1160, 880), color=INK, width=1)
    for x, label in (
        (180, "gem5 O3"),
        (460, "cache_sim"),
        (740, "Sniper"),
        (1020, "Analytic P-OPT"),
    ):
        figure.text(x, 862, label, size=17, bold=True, anchor="middle")
    evidence = (
        ("architectural time", "exact Request binding", "instructions + traffic"),
        ("functional victim", "large-graph traffic", "no cycles/instructions"),
        ("modeled cache direction", "equal semantic work", "time not speedup"),
        ("reserved ways + bytes", "target-time costs omitted", "optimistic bound"),
    )
    for col, lines in enumerate(evidence):
        figure.lines(56 + col * 280, 915, lines, max_width=248)
    figure.arrow(
        ((110, 1025), (1090, 1025)),
        kind="dependency", label="semantic receipts gate every published row",
        color=RED, label_at=(600, 1010),
    )
    save(figure, generated)


def offline_construction(
    fx: CheckedFixture, generated: list[tuple[Path, Path]]
) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "01", "offline-construction"),
        "Constructing an edge-aligned ReusePlan",
        "The concrete values come from fig/ecg-figure-fixture.json and its executable test.",
        "Four numbered bands derive a ReusePlan for the checked adjacency entry "
        f"adjacency entry {fx.tracked_source_reader}->{fx.tracked_source_dest} "
        f"(internal {fx.tracked_reader}->{fx.tracked_dest}). The figure "
        "shows selected CSR rows, degree-derived tiering, subsequent property-line "
        "accesses, compact packing, and the offline/measured-runtime boundary.",
        1725,
    )
    figure.section(
        "1", "CHECKED WEIGHTED GRAPH",
        "9 vertices, 17 undirected edges; unused internal IDs omitted",
        138, role="data",
    )
    figure.rect(24, 180, 780, 390, role="neutral", stroke=INK, stroke_width=3)
    figure.text(42, 211, "Checked nine-node weighted graph",
                size=17, bold=True, color=BLUE, max_width=735)
    figure.text(42, 238, "fixture IDs 0..8; node color shows internal property tier",
                size=16, color=GRAY, max_width=735)
    coords = {
        0: (95, 280),
        1: (95, 425),
        2: (220, 335),
        3: (365, 260),
        4: (350, 440),
        5: (500, 375),
        6: (625, 270),
        7: (620, 450),
        8: (740, 335),
    }
    for index, (left, right, weight) in enumerate(fx.weighted_edges):
        x1, y1 = coords[left]
        x2, y2 = coords[right]
        tracked = {left, right} == {
            fx.tracked_source_reader, fx.tracked_source_dest
        }
        figure.line(
            (x1, y1), (x2, y2),
            color=RED if tracked else GRAY,
            width=3 if tracked else 2,
        )
        dx, dy = x2 - x1, y2 - y1
        length = max(1.0, math.hypot(dx, dy))
        sign = -1 if index % 2 else 1
        weight_x = (x1 + x2) / 2 + sign * (-dy / length) * 10
        weight_y = (y1 + y2) / 2 + sign * (dx / length) * 10
        figure.text(
            weight_x, weight_y, str(weight), size=16, color=GRAY,
            anchor="middle",
        )
    x1, y1 = coords[fx.tracked_source_reader]
    x2, y2 = coords[fx.tracked_source_dest]
    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    figure.arrow(
        (
            (x1 + dx * 25 / length, y1 + dy * 25 / length),
            (x2 - dx * 28 / length, y2 - dy * 28 / length),
        ),
        kind="model-edge",
        color=RED,
        width=3,
    )
    figure.text(52, 552, "T1 hot (green)", size=16, color=GREEN)
    figure.text(215, 552, "T2 moderate (amber)", size=16, color=AMBER)
    figure.text(410, 552, "T3 cold (purple)", size=16, color=PURPLE)
    figure.text(585, 552, "tracked 4 -> 7 (red)", size=16, color=RED)
    for source_vertex in range(9):
        x, y = coords[source_vertex]
        internal = fx.source_to_internal[source_vertex]
        tier = fx.tiers[internal]
        fill = (
            GREEN_MATTE if tier == 1
            else AMBER_MATTE if tier == 2
            else PURPLE_MATTE
        )
        stroke = (
            RED if source_vertex in {
                fx.tracked_source_reader, fx.tracked_source_dest
            }
            else GREEN if tier == 1
            else AMBER if tier == 2
            else PURPLE
        )
        figure.circle(
            x, y, 24,
            fill=fill,
            stroke=stroke,
        )
        figure.text(
            x, y + 6, str(source_vertex), size=16, bold=True, anchor="middle"
        )
    figure.circle(55, 500, 18, fill=RED, stroke=RED)
    figure.text(55, 506, "A", size=17, bold=True, color=WHITE, anchor="middle")
    figure.text(
        86, 506,
        f"tracked adjacency {fx.tracked_source_reader} -> "
        f"{fx.tracked_source_dest}",
        size=17, bold=True, color=RED, max_width=300,
    )
    figure.rect(830, 180, 346, 390, role="data", radius=0)
    figure.text(846, 212, "Outgoing-CSR crosswalk", size=17,
                bold=True, color=BLUE)
    figure.line((830, 235), (1176, 235), color=INK, width=1)
    figure.line((970, 235), (970, 475), color=INK, width=1)
    for y in (295, 355, 415, 475):
        figure.line((830, y), (1176, y), color=INK, width=1)
    csr_rows = (
        ("0 -> 1", str(list(fx.rows[1]))),
        ("2 -> 6", str(list(fx.rows[6]))),
        ("4 -> 8", str(list(fx.rows[8]))),
        ("7 -> 18", str(list(fx.rows[18]))),
    )
    for row, (mapping, values) in enumerate(csr_rows):
        y = 272 + row * 60
        figure.text(846, y, mapping, size=16, mono=True)
        figure.text(986, y, values, size=16, mono=True)
    figure.text(
        846, 515,
        f"tracked: 4->7 = internal {fx.tracked_reader}->{fx.tracked_dest}",
        size=16, bold=True, mono=True, color=RED,
    )
    figure.text(846, 535, "PageRank: incoming CSR",
                size=16, color=PURPLE, max_width=314)
    figure.text(846, 562, "frontier kernels: outgoing CSR",
                size=16, color=PURPLE, max_width=314)

    figure.section(
        "2", "DEGREE-DERIVED REUSE TIER",
        "outgoing traversal: sort by d_in, then vertex ID",
        615, role="state",
    )
    tier_name = {1: "hot", 2: "moderate", 3: "cold"}[fx.line_tier]
    figure.rect(24, 657, 550, 245, role="state", radius=0)
    figure.text(40, 688, "Stable degree rank (hot fraction = 0.15)",
                size=17, bold=True, color=PURPLE)
    figure.line((24, 710), (574, 710), color=INK, width=1)
    for x in (210, 345, 455):
        figure.line((x, 710), (x, 902), color=INK, width=1)
    for x, label in ((110, "fixture / internal"), (277, "d_in"),
                     (400, "rank"), (515, "tier")):
        figure.text(x, 735, label, size=16, bold=True, anchor="middle")
    rank_rows = (
        ("v2 / int6", fx.reader_counts[6], 0, fx.tiers[6]),
        ("v4 / int8", fx.reader_counts[8], 1, fx.tiers[8]),
        ("v5 / int11", fx.reader_counts[11], 2, fx.tiers[11]),
        ("v7 / int18", fx.reader_counts[18], 3, fx.tiers[18]),
        ("v8 / int20", fx.reader_counts[20], 4, fx.tiers[20]),
    )
    for row, values in enumerate(rank_rows):
        y = 770 + row * 30
        for x, value in zip((40, 277, 400, 515), values):
            figure.text(x, y, str(value), size=16, mono=True,
                        anchor="middle" if x != 40 else "start")

    figure.rect(600, 657, 576, 245, role="neutral", radius=0)
    figure.text(616, 688, "64-byte property line: 16 x 4-byte vertices",
                size=17, bold=True)
    cell_width = 34
    for index, vertex in enumerate(range(fx.line_begin, fx.line_end)):
        x = 616 + index * cell_width
        role = "compute" if vertex == fx.tracked_dest else (
            "state" if vertex == 20 else "neutral"
        )
        figure.rect(x, 725, cell_width, 62, role=role,
                    stroke_width=1, radius=0)
        figure.text(x + cell_width / 2, 763, str(vertex),
                    size=16, mono=True, anchor="middle")
    figure.text(616, 825, "line tier = min(vertex tiers)",
                size=16, mono=True)
    figure.text(
        616, 855,
        f"min(T18={fx.tiers[18]}, T20={fx.tiers[20]}) = "
        f"T{fx.line_tier} ({tier_name})",
        size=16, bold=True, mono=True, color=GREEN,
    )
    figure.text(616, 885, "tier 0 is reserved for invalid metadata",
                size=16, color=RED)

    figure.section(
        "3", "TWO SUBSEQUENT LINE ACCESSES",
        "search begins after the current outer vertex",
        947, role="compute",
    )
    figure.rect(24, 989, 1152, 245, role="neutral", radius=0)
    access_x0, access_x1, access_y = 80, 1120, 1080
    figure.line((access_x0, access_y), (access_x1, access_y),
                color=INK, width=3)
    for vertex in fx.line_reader_ids:
        x = access_x0 + (access_x1 - access_x0) * vertex / (fx.epoch_count - 1)
        color = (
            AMBER if vertex == fx.tracked_reader
            else GREEN if vertex == fx.first_reader
            else PURPLE if vertex == fx.second_reader
            else GRAY
        )
        figure.circle(x, access_y, 10, fill=WHITE, stroke=color)
        figure.text(x, access_y - 28, str(vertex), size=16,
                    bold=vertex in {
                        fx.tracked_reader, fx.first_reader, fx.second_reader
                    }, color=color, anchor="middle")
    figure.text(
        40, 1020,
        f"access-source vertices = {list(fx.line_reader_ids)}",
        size=16, bold=True, mono=True, color=PURPLE,
    )
    figure.text(250, 1140, f"current u={fx.tracked_reader}",
                size=16, bold=True, color=AMBER, anchor="middle")
    figure.text(540, 1140, f"next u={fx.first_reader} -> e1={fx.first_epoch}",
                size=16, bold=True, color=GREEN, anchor="middle")
    figure.text(870, 1140, f"second u={fx.second_reader} -> e2={fx.second_epoch}",
                size=16, bold=True, color=PURPLE, anchor="middle")
    figure.text(
        600, 1190,
        f"epoch = floor(u * {fx.epoch_count} / {fx.num_vertices}); "
        "same-row line accesses are preserved; ID order wraps at |V|",
        size=16, mono=True, anchor="middle", max_width=1080,
    )

    figure.section(
        "4", "RECORD PACKING AND VALIDATION",
        "preprocessing produces an immutable runtime input",
        1279, role="transfer",
    )
    figure.bitfield(
        40, 1340, 520, 90,
        (
            (f"dest {fx.tracked_dest}", fx.id_bits, "data"),
            (f"T{fx.line_tier}", 2, "transfer"),
            (f"e1 {fx.first_epoch}", fx.epoch_bits, "compute"),
            (f"e2 {fx.second_epoch}", fx.epoch_bits, "state"),
        ),
        total_bits=fx.id_bits + 2 + 2 * fx.epoch_bits,
    )
    figure.text(
        40, 1470,
        f"compact width = {fx.id_bits} + 2 + 2*{fx.epoch_bits} = "
        f"{fx.id_bits + 2 + 2 * fx.epoch_bits} bits",
        size=16, bold=True, mono=True, color=AMBER,
    )
    validation_nodes = (
        (700, "header", "graph + config"),
        (900, "offsets", "record count"),
        (1100, "hash/width", "payload identity"),
    )
    for x, title, body in validation_nodes:
        figure.diamond(x, 1390, 140, 100, role="verify")
        figure.text(x, 1396, title, size=16, bold=True,
                    color=RED, anchor="middle")
        figure.text(x, 1460, body, size=16, anchor="middle")
    figure.arrow(
        ((560, 1390), (630, 1390)),
        kind="control", label="compact width", color=AMBER,
    )
    figure.arrow(
        ((770, 1390), (830, 1390)),
        kind="control", label="header", color=RED,
    )
    figure.arrow(
        ((970, 1390), (1030, 1390)),
        kind="control", label="offsets", color=RED,
    )
    figure.lines(
        650, 1480,
        (
            "builder executes outside the measured ROI",
            "guest aborts on header/offset/hash/width mismatch",
            "runtime streams validated edge order",
        ),
        color=RED, max_width=500,
    )
    figure.arrow(
        ((190, 1665), (1010, 1665)),
        kind="transfer",
        label="validated edge-aligned record stream",
        cadence="one record per governed edge access",
        color=AMBER,
        label_at=(600, 1650),
    )
    save(figure, generated)


def record_formats(fx: CheckedFixture, generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "02", "record-formats"),
        "ReusePlan record formats and structural traffic",
        "General, compact, and weighted layouts are separate transport choices.",
        "The figure gives the exact unweighted 64-bit layout, the graph-dependent "
        "32-bit compact rule instantiated by the checked fixture, and both weighted "
        "SSSP transports: one compact 64-bit edge record or an ordinary weighted edge "
        "plus a 32-bit metadata sidecar.",
        1190,
    )
    figure.section(
        "1", "GENERAL UNWEIGHTED RECORD", "canonical in-memory metadata layout",
        138, role="state",
    )
    figure.bitfield(
        40,
        190,
        1120,
        96,
        (
            ("destination", 32, "data"),
            ("tier", 2, "transfer"),
            ("epoch 1", 15, "compute"),
            ("epoch 2", 15, "state"),
        ),
        total_bits=64,
    )
    figure.lines(
        40,
        330,
        (
            "Bits 0..31 destination | bits 32..33 tier | bits 34..48 epoch 1 | bits 49..63 epoch 2",
            "Tier 1/2/3 means hot/moderate/cold; tier 0 is invalid. Epoch count is at most 2^15.",
        ),
        max_width=1120,
    )

    figure.section(
        "2", "COMPACT 32-BIT RECORD", "width is derived from graph and epoch configuration",
        405, role="transfer",
    )
    figure.bitfield(
        40, 455, 1120, 88,
        (
            (f"dest {fx.id_bits}", fx.id_bits, "data"),
            ("tier 2", 2, "transfer"),
            (f"e1 {fx.epoch_bits}", fx.epoch_bits, "compute"),
            (f"e2 {fx.epoch_bits}", fx.epoch_bits, "state"),
            ("zero / reserved", 32 - fx.id_bits - 2 - 2 * fx.epoch_bits,
             "neutral"),
        ),
        total_bits=32,
    )
    figure.text(
        40, 580,
        f"id_bits + 2 + 2*epoch_bits = {fx.id_bits} + 2 + "
        f"2*{fx.epoch_bits} = {fx.id_bits + 2 + 2 * fx.epoch_bits} <= 32",
        size=16, bold=True, mono=True, color=AMBER, max_width=650,
    )
    figure.arrow(
        ((90, 625), (430, 625)),
        kind="transfer", label="4-byte compact record",
        cadence="per governed adjacency", color=AMBER,
        label_at=(260, 612),
    )
    figure.arrow(
        ((500, 625), (1110, 625)),
        kind="dependency", label="record-load widening",
        color=PURPLE, label_at=(805, 612),
    )
    figure.text(90, 660, "substitutes for one 4-byte edge ID",
                size=16, color=AMBER)
    figure.text(500, 660, "format CSR -> canonical destination/tier/e1/e2",
                size=16, color=PURPLE)
    figure.text(40, 687, "unused high bits are zero/reserved",
                size=16, color=RED)
    figure.text(500, 687, "width receipt must match the materialized array",
                size=16, color=RED)

    figure.section(
        "3", "WEIGHTED SSSP TRANSPORTS", "weight bytes cannot disappear from the comparison",
        735, role="data",
    )
    figure.text(40, 790, "A. compact weighted substitute: 8 bytes",
                size=17, bold=True, color=BLUE)
    figure.bitfield(
        40, 815, 1120, 82,
        (
            ("destination 24", 24, "data"),
            ("weight 8", 8, "neutral"),
            ("tier 2", 2, "transfer"),
            ("epoch 1: 15", 15, "compute"),
            ("epoch 2: 15", 15, "state"),
        ),
        total_bits=64,
    )
    figure.text(
        40, 930,
        "constraint: |V| < 2^24 and 0 < weight <= 255; record replaces the "
        "ordinary weighted edge",
        size=16, mono=True, max_width=1120,
    )
    figure.text(40, 985, "B. ordinary weighted edge + 4-byte sidecar",
                size=17, bold=True, color=PURPLE)
    figure.bitfield(
        40, 1010, 700, 82,
        (
            ("destination 32", 32, "data"),
            ("weight 32", 32, "neutral"),
        ),
        total_bits=64,
    )
    figure.text(765, 1045, "+", size=22, bold=True, anchor="middle")
    figure.bitfield(
        800, 1010, 360, 82,
        (
            ("tier 2", 2, "transfer"),
            ("epoch 1: 15", 15, "compute"),
            ("epoch 2: 15", 15, "state"),
        ),
        total_bits=32,
    )
    figure.text(
        40, 1130,
        "traffic = weighted-edge bytes + 4 sidecar bytes; edge and sidecar "
        "FlowThrough roles are accounted separately",
        size=16, bold=True, color=RED, max_width=1120,
    )
    save(figure, generated)


def future_distance(
    fx: CheckedFixture, generated: list[tuple[Path, Path]]
) -> None:
    current = fx.tracked_reader
    epoch_count = fx.epoch_count
    first_distance = (
        fx.first_epoch + epoch_count - current
    ) % epoch_count
    second_distance = (
        fx.second_epoch + epoch_count - current
    ) % epoch_count
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "03", "future-distance"),
        "Quantized next-reference distance for one property line",
        f"The tracked property line is subsequently accessed from outer vertices "
        f"{fx.first_reader} and {fx.second_reader} after {fx.tracked_reader}.",
        f"A horizontal schedule follows property line 0x{fx.property_line:08X} "
        f"from current outer vertex {current} to subsequent access-source vertices "
        f"{fx.first_reader} and {fx.second_reader}. The ReusePlan stores their "
        "quantized epochs, and RRIP-first consults the nearer "
        "circular distance only after the line becomes RRIP eligible.",
        1140,
    )
    figure.section(
        "1", "CHECKED CACHE-LINE TIMELINE", "one 64-byte line contains property vertices 16..31",
        138, role="state",
    )
    figure.rect(24, 180, 1152, 340, role="neutral", stroke=INK, stroke_width=3)
    axis_y = 340
    axis_x0 = 85
    axis_x1 = 1115
    figure.line((axis_x0, axis_y), (axis_x1, axis_y), color=INK, width=3)
    ticks = (0, current, fx.first_reader, fx.second_reader, epoch_count - 1)
    for index, epoch in enumerate(ticks):
        x = axis_x0 + (axis_x1 - axis_x0) * epoch / (epoch_count - 1)
        color = (
            AMBER if epoch == current
            else GREEN if epoch == fx.first_reader
            else PURPLE if epoch == fx.second_reader
            else GRAY
        )
        figure.line((x, axis_y - 18), (x, axis_y + 18), color=color, width=3)
        figure.circle(x, axis_y, 11, fill=WHITE, stroke=color)
        label_y = 292 if index % 2 == 0 else 395
        figure.text(x, label_y, str(epoch), size=17, bold=True,
                    color=color, anchor="middle")
    figure.text(
        axis_x0, 235,
        f"line 0x{fx.property_line:08X} sources: "
        f"{list(fx.line_reader_ids)}",
        size=17, bold=True, color=PURPLE, max_width=520,
    )
    figure.text(
        1115, 235,
        f"fixture {fx.tracked_source_reader}->{fx.tracked_source_dest} / "
        f"internal {fx.tracked_reader}->{fx.tracked_dest}: "
        f"e1={fx.first_epoch}, e2={fx.second_epoch}",
        size=17, bold=True, color=AMBER, anchor="end", max_width=520,
    )
    figure.text(260, 450, "current outer vertex", size=16, color=AMBER)
    figure.text(505, 450, "next line access", size=16, color=GREEN)
    figure.text(860, 450, "second line access", size=16, color=PURPLE)
    figure.text(
        600, 492,
        "the record encodes the next two scheduled accesses to this property line",
        size=16, color=GRAY, anchor="middle", max_width=900,
    )
    figure.section(
        "2", "CIRCULAR DISTANCE AT THE LLC", "the current victim-time epoch can advance after fill",
        565, role="compute",
    )
    ring_cx, ring_cy, ring_r = 190, 690, 80
    figure.circle(ring_cx, ring_cy, ring_r,
                  fill=PURPLE_MATTE, stroke=PURPLE)
    figure.circle(ring_cx, ring_cy, 54, fill=WHITE, stroke=WHITE)
    figure.text(ring_cx, ring_cy - 8, "epoch", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(ring_cx, ring_cy + 20, "mod 32", size=16,
                color=PURPLE, anchor="middle")
    for epoch, color, label in (
        (current, AMBER, f"c={current}"),
        (fx.first_epoch, GREEN, f"e1={fx.first_epoch}"),
        (fx.second_epoch, PURPLE, f"e2={fx.second_epoch}"),
    ):
        angle = -math.pi / 2 + 2 * math.pi * epoch / epoch_count
        x = ring_cx + ring_r * math.cos(angle)
        y = ring_cy + ring_r * math.sin(angle)
        figure.circle(x, y, 11, fill=WHITE, stroke=color)
        figure.text(
            ring_cx + (ring_r + 30) * math.cos(angle),
            ring_cy + (ring_r + 30) * math.sin(angle) + 6,
            label, size=16, bold=True, color=color, anchor="middle",
        )

    figure.rect(380, 607, 796, 205, role="neutral", radius=0)
    figure.line((380, 652), (1176, 652), color=INK, width=1)
    figure.line((380, 697), (1176, 697), color=INK, width=1)
    figure.line((380, 742), (1176, 742), color=INK, width=1)
    figure.text(400, 637, f"current epoch c = {current}",
                size=16, bold=True, mono=True)
    figure.text(
        400, 682,
        f"d1 = ({fx.first_epoch} + {epoch_count} - {current}) mod "
        f"{epoch_count} = {first_distance}",
        size=16, mono=True,
    )
    figure.text(
        400, 727,
        f"d2 = ({fx.second_epoch} + {epoch_count} - {current}) mod "
        f"{epoch_count} = {second_distance}",
        size=16, mono=True,
    )
    figure.text(
        400, 772,
        f"nearest = min({first_distance}, {second_distance}) = "
        f"{min(first_distance, second_distance)}",
        size=16, bold=True, mono=True, color=GREEN,
    )
    figure.text(790, 637, "line state stores absolute epochs",
                size=16, color=PURPLE)
    figure.text(790, 682, "victim-time c may advance after fill",
                size=16)
    figure.text(790, 727, "unstamped distance = 0",
                size=16)
    figure.text(790, 772, "malformed epochs are clamped",
                size=16)
    figure.section(
        "3", "RRIP-FIRST DECISION",
        "property-line ranking follows RRIP eligibility",
        857, role="verify",
    )
    figure.diamond(220, 970, 210, 115, role="compute")
    figure.text(220, 958, "max-RRPV", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(220, 985, "candidate?", size=16, anchor="middle")
    figure.diamond(555, 970, 210, 115, role="transfer")
    figure.text(555, 958, "structural", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(555, 985, "candidate?", size=16, anchor="middle")
    figure.rect(760, 910, 190, 120, role="transfer", radius=0)
    figure.text(855, 947, "select oldest", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(855, 980, "structural line", size=16, anchor="middle")
    figure.rect(980, 910, 196, 120, role="verify", radius=0)
    figure.text(1078, 943, "select farthest", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(1078, 973, "stamped distance", size=16, anchor="middle")
    figure.text(1078, 1003, "stable set-order tie", size=16, anchor="middle")
    figure.arrow(
        ((325, 970), (450, 970)),
        kind="control", label="eligible set formed", color=GREEN,
        label_at=(388, 900),
    )
    figure.arrow(
        ((660, 970), (760, 970)),
        kind="control", label="structural", color=AMBER,
    )
    figure.arrow(
        ((660, 970), (690, 970), (690, 1050), (965, 1050),
         (965, 970), (980, 970)),
        kind="control", label="no: property only", color=RED,
        label_at=(815, 1085),
    )
    figure.arrow(
        ((220, 1028), (220, 1085), (80, 1085), (80, 970), (115, 970)),
        kind="loop", label="none: age RRPV and retry", color=PURPLE,
        label_at=(265, 1075), label_anchor="start",
    )
    save(figure, generated)


def llc_policy(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "04", "llc-policy-pipeline"),
        "ReuseBind acceptance and RRIP-first victim selection",
        "Line updates, eligibility, structural preference, and epoch ranking are distinct decisions.",
        "Four bands show how a validated ReuseBind updates line-local metadata on "
        "an LLC hit or fill, how invalidation clears it, and how the shared victim "
        "policy ages RRPV, prefers an old structural line, and otherwise selects "
        "the farthest stamped property among eligible ways.",
        1380,
    )
    figure.section(
        "1", "ACCEPT OR REJECT REQUEST METADATA", "destination line and execution context must agree",
        138, role="verify",
    )
    figure.rect(40, 200, 245, 130, role="state", radius=0)
    figure.text(162, 232, "ReuseBind extension", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(162, 267, "dest | tier | e1 | e2", size=16, mono=True,
                anchor="middle")
    figure.text(162, 298, "count | context | conflict", size=16, mono=True,
                anchor="middle")
    gates = (
        (405, "context != 0?", "verify"),
        (635, "conflict == 0?", "verify"),
        (865, "dest line match?", "compute"),
    )
    for x, label, role in gates:
        figure.diamond(x, 265, 180, 120, role=role)
        first, second = label.split(" ", 1)
        figure.text(x, 252, first, size=17, bold=True,
                    anchor="middle")
        figure.text(x, 282, second, size=16, anchor="middle")
    figure.rect(1010, 205, 150, 105, role="compute", radius=0)
    figure.text(1085, 240, "accept", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(1085, 275, "hit/fill stamp", size=16, anchor="middle")
    for start, end, label in (
        ((285, 265), (315, 265), "ReuseBind extension"),
        ((495, 265), (545, 265), "context"),
        ((725, 265), (775, 265), "conflict"),
        ((955, 265), (1010, 265), "dest"),
    ):
        figure.arrow((start, end), kind="control", label=label,
                     color=GREEN)
    figure.text(
        600, 365,
        "reject: governed/ungoverned mix | requestor/context mismatch | "
        "invalid count",
        size=16, bold=True, color=RED, anchor="middle", max_width=1080,
    )
    figure.text(
        600, 397,
        "reject: equal-sequence payload mismatch | destination-line mismatch",
        size=16, bold=True, color=RED, anchor="middle", max_width=1080,
    )

    figure.section(
        "2", "LINE-LOCAL STATE", "data value and replacement metadata have different lifetimes",
        450, role="state",
    )
    figure.rect(40, 500, 1120, 205, role="neutral", radius=0)
    columns = (40, 120, 260, 350, 455, 560, 665, 770, 900, 1040, 1160)
    for x in columns[1:-1]:
        figure.line((x, 500), (x, 705), color=INK, width=1)
    for y in (545, 585, 625, 665):
        figure.line((40, y), (1160, y), color=INK, width=1)
    headers = ("way", "role/data", "RRPV", "recency", "tier",
               "e1", "e2", "count", "context", "stamp")
    centers = tuple((a + b) / 2 for a, b in zip(columns, columns[1:]))
    for x, label in zip(centers, headers):
        figure.text(x, 530, label, size=16, bold=True, anchor="middle")
    rows = (
        ("0", "structural", "3", "91", "-", "-", "-", "0", "-", "0"),
        ("1", "property", "3", "77", "T1", "11", "15", "2", "k", "1"),
        ("2", "property", "2", "63", "T2", "20", "20", "1", "k", "1"),
        ("3", "invalid", "-", "-", "T0", "0", "0", "0", "0", "0"),
    )
    for row, values in enumerate(rows):
        y = 572 + row * 40
        for x, value in zip(centers, values):
            figure.text(x, y, value, size=16, mono=True, anchor="middle")
    figure.text(40, 742, "hit/fill: refresh tier/epochs/context",
                size=16, color=GREEN)
    figure.text(420, 742, "ordinary hit: native RRPV/recency",
                size=16, color=BLUE)
    figure.text(790, 742, "invalidate -> clear every ECG field",
                size=16, color=RED)

    figure.section(
        "3", "RRIP ELIGIBILITY", "the default variant ages until a way reaches rrpvMax",
        762, role="compute",
    )
    figure.diamond(190, 900, 220, 130, role="compute")
    figure.text(190, 884, "any way at", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(190, 915, "RRPV == max?", size=16, anchor="middle")
    figure.diamond(500, 900, 220, 130, role="transfer")
    figure.text(500, 884, "structural", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(500, 915, "candidate?", size=16, anchor="middle")
    figure.rect(680, 830, 210, 120, role="transfer", radius=0)
    figure.text(785, 868, "oldest structural", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(785, 905, "normalized recency", size=16, anchor="middle")
    figure.rect(930, 830, 230, 150, role="state", radius=0)
    figure.text(1045, 865, "farthest property", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(1045, 898, "min(d(e1), d(e2))", size=16,
                mono=True, anchor="middle")
    figure.text(1045, 932, "unstamped distance=0", size=16,
                anchor="middle")
    figure.text(1045, 962, "stable set-order tie", size=16,
                anchor="middle")
    figure.arrow(
        ((300, 900), (390, 900)), kind="control",
        label="RRPV == max?", color=GREEN,
    )
    figure.arrow(
        ((610, 900), (645, 900), (645, 890), (680, 890)),
        kind="control",
        label="structural", color=AMBER,
    )
    figure.arrow(
        ((610, 900), (650, 900), (650, 1005), (910, 1005),
         (910, 905), (930, 905)),
        kind="control",
        label="property candidates", color=PURPLE,
        label_at=(780, 1035),
    )
    figure.arrow(
        ((190, 965), (190, 1035), (70, 1035), (70, 900), (80, 900)),
        kind="loop", label="age RRPV and retry", color=RED,
        label_at=(230, 1025), label_anchor="start",
    )

    figure.section(
        "4", "VARIANTS ARE CONTROLLED ABLATIONS", "do not blend their ordering into the primary claim",
        1074, role="verify",
    )
    figure.rect(40, 1120, 1120, 175, role="neutral", radius=0)
    figure.line((40, 1165), (1160, 1165), color=INK, width=1)
    for x in (315, 590, 865):
        figure.line((x, 1120), (x, 1295), color=INK, width=1)
    variants = (
        ("rrip_first", "RRIP->struct->epoch", "primary"),
        ("grasp_only", "pure RRIP", "neutral"),
        ("rrip_no_epoch", "fixed property tie", "ablation"),
        ("rrip_no_epoch_recency", "property LRU tie", "ablation"),
        ("epoch_first / degree_first", "alternate order", "diagnostic"),
        ("future_tier_first", "future/tier/LRU", "diagnostic"),
        ("shortcircuit / lru_only", "baseline ctrl", "diagnostic"),
        ("online selectors", "failed gates", "not promoted"),
    )
    for index, (name, order, status) in enumerate(variants):
        col = index % 4
        row = index // 4
        x = 56 + col * 275
        y = 1195 + row * 55
        figure.text(x, y, name, size=16, bold=True, mono=True,
                    color=RED if status == "not promoted" else INK)
        figure.text(x, y + 25, f"{order} | {status}", size=16,
                    max_width=250)
    save(figure, generated)


def flowthrough_outcomes(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "05", "flowthrough-outcomes"),
        "FlowThrough lookup, service, and LLC fill allocation",
        "The important corner case is an MSHR shared with an allocating target.",
        "A FlowThrough record request uses normal translation, private caches, "
        "LLC tag lookup, miss service, and response. An LLC hit remains a hit. "
        "For an LLC miss, an all-no-allocate MSHR skips the LLC fill while mixed "
        "targets retain allocation because gem5 combines allocOnFill with OR.",
        1280,
    )
    figure.section(
        "1", "UNCHANGED FRONT HALF", "record request uses the normal memory hierarchy",
        138, role="data",
    )
    pipeline_nodes = (
        (35, 205, 145, "Load queue", "order / replay", "state"),
        (220, 205, 145, "D-TLB", "translation", "data"),
        (405, 205, 145, "L1D", "tag + data", "data"),
        (590, 205, 145, "L2", "tag + data", "data"),
        (775, 205, 145, "LLC tags", "hit or MSHR", "compute"),
        (960, 205, 180, "Memory", "ordinary service", "verify"),
    )
    figure.arrow(
        ((45, 263), (1130, 263)),
        kind="transfer", label="record Request + FlowThrough",
        cadence="per record load", color=AMBER, width=3,
        label_at=(600, 190), underlay=True,
    )
    for x, y, width, title, subtitle, role in pipeline_nodes:
        figure.rect(x, y, width, 115, role=role, radius=0)
        figure.text(x + width / 2, y + 38, title, size=17, bold=True,
                    anchor="middle")
        figure.text(x + width / 2, y + 75, subtitle, size=16,
                    anchor="middle")
    figure.text(
        600, 370,
        "translation, private hits/fills, LLC lookup, memory response, "
        "writeback: unchanged",
        size=16, bold=True, color=BLUE, anchor="middle", max_width=1050,
    )

    figure.section(
        "2", "THREE LLC OUTCOMES", "only the returning miss fill reaches the allocation gate",
        430, role="transfer",
    )
    figure.diamond(170, 585, 210, 120, role="compute")
    figure.text(170, 575, "LLC tag", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(170, 605, "hit?", size=16, anchor="middle")
    figure.rect(330, 505, 230, 105, role="compute", radius=0)
    figure.text(445, 540, "hit: return record", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(445, 575, "no allocation decision", size=16,
                anchor="middle")
    figure.arrow(
        ((275, 560), (330, 560)),
        kind="control", label="hit: return record", color=GREEN,
    )

    figure.table(330, 635, 350, 150, 4, role="state")
    figure.text(505, 665, "MSHR target list", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(346, 705, "target A: allocOnFill=false", size=16, mono=True)
    figure.text(346, 742, "target B: false or true", size=16, mono=True)
    figure.text(346, 778, "allocOnFill combines with OR", size=16,
                bold=True, mono=True)
    figure.arrow(
        ((170, 645), (170, 710), (330, 710)),
        kind="control", label="LLC miss", color=AMBER,
        label_at=(235, 695),
    )
    figure.diamond(805, 710, 210, 120, role="transfer")
    figure.text(805, 700, "allocOnFill", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(805, 730, "aggregate?", size=16, anchor="middle")
    figure.arrow(
        ((680, 710), (700, 710)),
        kind="control", label="allocOnFill combines with OR",
        color=PURPLE,
    )
    figure.rect(960, 635, 190, 105, role="verify", radius=0)
    figure.text(1055, 670, "true: insert LLC", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(1055, 705, "mixed target wins", size=16, anchor="middle")
    figure.rect(960, 755, 190, 90, role="transfer", radius=0)
    figure.text(1055, 790, "false: skip fill", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(1055, 820, "all targets false", size=16, anchor="middle")
    figure.arrow(
        ((910, 710), (935, 710), (935, 688), (960, 688)),
        kind="control", label="true: insert LLC", color=RED,
    )
    figure.arrow(
        ((910, 710), (935, 710), (935, 800), (960, 800)),
        kind="control", label="false: skip fill", color=AMBER,
    )

    figure.section(
        "3", "DERIVED PREFETCHES AND PROPERTY LOADS", "classification remains target-range exact",
        890, role="state",
    )
    figure.arrow(
        ((60, 980), (1130, 980)),
        kind="transfer", label="derived prefetch Request",
        cadence="per generated candidate", color=PURPLE, width=3,
        label_at=(300, 965),
    )
    figure.arrow(
        ((60, 1100), (1130, 1100)),
        kind="transfer", label="governed property Request",
        cadence="per property access", color=BLUE, width=3,
        label_at=(300, 1085),
    )
    for x, label in (
        (140, "candidate address"),
        (430, "range check"),
        (720, "Request flag/ext"),
        (1010, "LLC behavior"),
    ):
        figure.line((x, 940), (x, 1160), color=GRAY, width=1)
        figure.text(x, 930, label, size=16, bold=True, anchor="middle")
        figure.circle(x, 980, 9, fill=WHITE, stroke=PURPLE)
        figure.circle(x, 1100, 9, fill=WHITE, stroke=BLUE)
    figure.text(430, 1015, "in active carrier?", size=16, anchor="middle")
    figure.text(720, 1015, "in-range -> STRUCTURAL_FLOWTHROUGH",
                size=16, anchor="middle")
    figure.text(1010, 1015, "out-of-range bit stays clear",
                size=16, anchor="middle")
    figure.text(430, 1135, "destination-line guard", size=16,
                anchor="middle")
    figure.text(720, 1135, "ReuseBind; FlowThrough=0", size=16,
                anchor="middle")
    figure.text(1010, 1135, "allocatable; hit/fill may stamp",
                size=16, anchor="middle")
    figure.arrow(
        ((145, 1215), (1055, 1215)),
        kind="control",
        label="suppress LLC insertion only when every coalesced target permits it",
        color=AMBER,
        label_at=(600, 1200),
    )
    save(figure, generated)


def structural_fairness(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "06", "structural-fairness"),
        "FlowThrough mechanism and matched structural-array control",
        "The two switches answer different experimental questions.",
        "The design flag belongs to ReusePlan record requests. The symmetric "
        "--flowthrough all control gives LRU, GRASP, P-OPT, and ReusePlan the same "
        "no-allocate opportunity on the structural stream each workload actually "
        "consumes, with source-specific receipts in all three simulators.",
        1000,
    )
    figure.section(
        "1", "DESIGN MECHANISM", "ECG_FLOWTHROUGH is attached by ecg.flow.load*",
        138, role="state",
    )
    figure.rect(40, 200, 180, 105, role="state", radius=0)
    figure.text(130, 232, "ReusePlan array", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(130, 263, "compact / wide", size=16, anchor="middle")
    figure.text(130, 289, "weighted sidecar", size=16, anchor="middle")
    figure.rect(300, 200, 180, 105, role="transfer", radius=0)
    figure.text(390, 237, "ecg.flow.load*", size=17, bold=True,
                mono=True, color=AMBER, anchor="middle")
    figure.text(390, 275, "record Request", size=16, anchor="middle")
    figure.rect(560, 200, 190, 105, role="transfer", radius=0)
    figure.text(655, 237, "ECG_FLOWTHROUGH", size=16, bold=True,
                mono=True, color=AMBER, anchor="middle")
    figure.text(655, 275, "request-specific bit", size=16, anchor="middle")
    figure.rect(830, 200, 300, 105, role="data", radius=0)
    figure.text(980, 232, "normal TLB / private / LLC lookup",
                size=16, bold=True, color=BLUE, anchor="middle")
    figure.text(980, 265, "hit returns normally", size=16,
                anchor="middle")
    figure.text(980, 292, "miss reaches LLC allocation gate", size=16,
                anchor="middle")
    for start, end, label in (
        ((220, 252), (300, 252), "record load"),
        ((480, 252), (560, 252), "Request flag"),
        ((750, 252), (830, 252), "normal hierarchy"),
    ):
        figure.arrow(
            (start, end), kind="control", label=label,
            color=AMBER, label_at=((start[0] + end[0]) / 2, 185),
        )
    figure.line((40, 350), (1130, 350), color=RED, width=2)
    figure.text(
        50, 385,
        "not bypass | not zero bytes | not zero latency | not a victim-policy result",
        size=16, bold=True, color=RED, max_width=1080,
    )

    figure.section(
        "2", "SYMMETRIC FAIRNESS CONTROL", "--flowthrough all is policy-independent",
        440, role="transfer",
    )
    figure.rect(40, 490, 1120, 275, role="neutral", radius=0)
    for y in (535, 590, 645, 700):
        figure.line((40, y), (1160, y), color=INK, width=1)
    for x in (220, 480, 740, 960):
        figure.line((x, 490), (x, 765), color=INK, width=1)
    for x, label in (
        (130, "Policy row"),
        (350, "Active structural carrier"),
        (610, "Fairness flag"),
        (850, "Required receipt"),
        (1060, "Fail-closed rule"),
    ):
        figure.text(x, 520, label, size=16, bold=True, anchor="middle")
    rows = (
        ("LRU", "CSR edge array", "STRUCTURAL_FLOWTHROUGH",
         "positive structural access", "reject zero activity"),
        ("GRASP", "CSR edge array", "STRUCTURAL_FLOWTHROUGH",
         "positive no-allocate", "same workload cell"),
        ("P-OPT", "CSR + matrix traffic", "STRUCTURAL_FLOWTHROUGH",
         "positive structural event", "matrix bytes retained"),
        ("ReusePlan", "record array; CSR on fallback",
         "STRUCTURAL_FLOWTHROUGH", "backend-specific counter",
         "carrier receipt must match"),
    )
    for row, values in enumerate(rows):
        y = 572 + row * 55
        colors = (INK, BLUE, AMBER, PURPLE, RED)
        for x, value, color in zip((56, 236, 496, 756, 976), values, colors):
            figure.text(x, y, value, size=16, color=color,
                        mono=value == "STRUCTURAL_FLOWTHROUGH")
    figure.text(
        40, 805,
        "cache_sim: access count | gem5: no-allocate targets | "
        "Sniper: read/fill counts; translated mode rejected",
        size=16, color=PURPLE, max_width=1120,
    )
    figure.arrow(
        ((110, 880), (1090, 880)),
        kind="dependency",
        label="compare policies only after active structural carriers are matched",
        color=RED,
        label_at=(600, 865),
    )
    save(figure, generated)


def instruction_family(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "01", "instruction-family"),
        "RISC-V record-load and property-load instruction roles",
        "Record acquisition and property access remain separate dynamic loads.",
        "Four numbered roles distinguish ordinary and FlowThrough record loads "
        "from computed-address and indexed ReuseBind property loads. Compact "
        "decode uses the record-format CSR and widens into the canonical metadata "
        "layout before the property instruction consumes the plan.",
        1120,
    )
    figure.section(
        "1", "CONFIGURE EXECUTION CONTEXT", "software writes format, current epoch, and context CSRs",
        138, role="state",
    )
    figure.rect(40, 180, 1120, 190, role="neutral", radius=0)
    for y in (225, 273, 321):
        figure.line((40, y), (1160, y), color=INK, width=1)
    for x in (270, 570, 860):
        figure.line((x, 180), (x, 370), color=INK, width=1)
    for x, label in (
        (155, "Architectural CSR"),
        (420, "Fields"),
        (715, "Software update"),
        (1010, "Consumer"),
    ):
        figure.text(x, 210, label, size=16, bold=True, anchor="middle")
    csr_rows = (
        ("record format", "id_bits | epoch_bits", "before ROI",
         "compact record-load execution"),
        ("current epoch", "quantized traversal position", "epoch boundary",
         "ReuseBind Request"),
        ("context", "nonzero execution identity", "execution boundary",
         "MSHR merge + LLC validation"),
    )
    for row, values in enumerate(csr_rows):
        y = 257 + row * 48
        for x, value in zip((56, 286, 586, 876), values):
            figure.text(x, y, value, size=16, mono=row == 0)

    figure.section(
        "2", "RECORD-LOAD FAMILY", "rs1 is a record address; result depends on the form",
        430, role="transfer",
    )
    figure.rect(40, 490, 180, 120, role="data", radius=0)
    figure.text(130, 525, "custom-0", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(130, 557, "record address", size=16, anchor="middle")
    figure.text(130, 585, "in rs1", size=16, mono=True, anchor="middle")
    figure.diamond(350, 550, 190, 125, role="compute")
    figure.text(350, 540, "record-load", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(350, 570, "role decode", size=16, anchor="middle")
    figure.arrow(
        ((220, 550), (255, 550)),
        kind="control", label="custom-0", color=BLUE,
    )

    figure.rect(500, 475, 290, 90, role="data", radius=0)
    figure.text(515, 507, "ecg.plan.load*", size=17, bold=True,
                mono=True, color=BLUE)
    figure.text(515, 538, "ordinary placement -> canonical rd", size=16)
    figure.rect(500, 585, 290, 90, role="transfer", radius=0)
    figure.text(515, 617, "ecg.flow.load*", size=17, bold=True,
                mono=True, color=AMBER)
    figure.text(515, 648, "ECG_FLOWTHROUGH -> canonical rd", size=16)
    figure.rect(835, 475, 325, 200, role="state", radius=0)
    figure.text(852, 507, "Transport forms", size=17, bold=True,
                color=PURPLE)
    figure.lines(
        852, 540,
        (
            "general: 64-bit ReusePlan",
            "compact: 4-byte load, widened",
            "weighted: sidecar32 + dest",
            "compact: FlowThrough only",
            "no compact Plan-load",
        ),
        mono=True, max_width=292,
    )
    figure.line((445, 550), (470, 550), color=BORDER, width=2)
    figure.line((470, 520), (470, 630), color=BORDER, width=2)
    figure.circle(470, 550, 5, fill=WHITE, stroke=BORDER)
    figure.arrow(
        ((470, 520), (500, 520)),
        kind="control", label="ecg.plan.load*", color=BLUE,
    )
    figure.arrow(
        ((470, 630), (500, 630)),
        kind="control", label="ecg.flow.load*", color=AMBER,
    )

    figure.section(
        "3", "PROPERTY-LOAD FAMILY", "the ReusePlan result is an explicit source operand",
        722, role="compute",
    )
    figure.rect(40, 790, 210, 120, role="state", radius=0)
    figure.text(145, 825, "physical rd", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(145, 857, "canonical ReusePlan", size=16,
                anchor="middle")
    figure.text(145, 885, "becomes property rs2", size=16,
                mono=True, anchor="middle")
    figure.diamond(390, 850, 190, 125, role="compute")
    figure.text(390, 840, "property", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(390, 870, "address form", size=16, anchor="middle")
    figure.arrow(
        ((250, 850), (295, 850)),
        kind="dependency", label="physical rd",
        color=PURPLE,
    )

    figure.rect(535, 775, 285, 115, role="compute", radius=0)
    figure.text(552, 808, "ecg.bind.load.*", size=17, bold=True,
                mono=True, color=GREEN)
    figure.token_line(
        552, 840,
        (("rs1", PURPLE, True), (" = ", INK, False),
         ("computed address", BLUE, False)),
        mono=False, max_width=250,
    )
    figure.token_line(
        552, 870,
        (("rs2", PURPLE, True), (" = ", INK, False),
         ("ReusePlan", PURPLE, False)),
        mono=True, max_width=250,
    )
    figure.rect(535, 915, 285, 115, role="compute", radius=0)
    figure.text(552, 948, "ecg.bind.iload.*", size=17, bold=True,
                mono=True, color=GREEN)
    figure.token_line(
        552, 980,
        (("rs1", PURPLE, True), (" = ", INK, False),
         ("property base", BLUE, False)),
        mono=False, max_width=250,
    )
    figure.token_line(
        552, 1010,
        (("EA", GREEN, True), (" = ", INK, False),
         ("base", BLUE, False), (" + dest*size", INK, False)),
        mono=False, max_width=250,
    )
    figure.rect(865, 790, 295, 225, role="data", radius=0)
    figure.text(882, 824, "Shared memory semantics", size=17,
                bold=True, color=BLUE)
    figure.lines(
        882, 858,
        (
            "one ordinary property Request",
            "typed ReuseBind on Request",
            "result: U32 / S32 / U64 / F32",
            "FlowThrough is never attached",
            "native order / replay / retire",
        ),
        max_width=260,
    )
    figure.line((485, 850), (510, 850), color=BORDER, width=2)
    figure.line((510, 833), (510, 973), color=BORDER, width=2)
    figure.circle(510, 850, 5, fill=WHITE, stroke=BORDER)
    figure.arrow(
        ((510, 833), (535, 833)),
        kind="control", label="ecg.bind.load.*", color=GREEN,
    )
    figure.arrow(
        ((510, 973), (535, 973)),
        kind="control", label="ecg.bind.iload.*", color=GREEN,
    )
    figure.arrow(
        ((160, 1055), (1040, 1055)),
        kind="dependency",
        label="record rd becomes property rs2 through normal rename and issue",
        color=PURPLE,
        label_at=(600, 1040),
    )
    save(figure, generated)


def o3_pipeline(
    fx: CheckedFixture, generated: list[tuple[Path, Path]]
) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "02", "o3-request-pipeline"),
        "ReusePlan loads in an out-of-order core",
        f"Adjacency entry {fx.tracked_source_reader}->{fx.tracked_source_dest} "
        f"maps to internal {fx.tracked_reader}->{fx.tracked_dest}, property "
        f"0x{fx.property_address:08X}, and LLC line 0x{fx.property_line:08X}.",
        "The diagram connects an outgoing graph adjacency and its CSR/ReusePlan "
        "arrays to the O3 load datapath: Fetch, Decode, Rename, ROB, issue "
        "queue, physical register file, AGU, LSQ, L1D, load-data writeback, "
        "dependency wakeup, request-specific cache state, and in-order commit.",
        1900,
    )

    figure.section(
        "1", "GEM5 O3 LOAD DATAPATH",
        "shared hardware; amber=I0, green=I1",
        138, role="transfer",
    )
    figure.circle(45, 200, 16, fill=AMBER, stroke=AMBER)
    figure.text(45, 206, "I0", size=16, bold=True,
                color=WHITE, anchor="middle")
    figure.token_line(
        72, 206,
        (
            ("flow.load.compact", AMBER, True),
            (": ", INK, False),
            (f"record[{fx.tracked_reader}->{fx.tracked_dest}]", BLUE, False),
            (" -> ", INK, False),
            ("P17", PURPLE, True),
        ),
        max_width=500,
    )
    figure.circle(650, 200, 16, fill=GREEN, stroke=GREEN)
    figure.text(650, 206, "I1", size=16, bold=True,
                color=WHITE, anchor="middle")
    figure.token_line(
        677, 206,
        (
            ("bind.load.u32", GREEN, True),
            (": ", INK, False),
            ("rs1", PURPLE, True),
            ("=", INK, False),
            (f"0x{fx.property_address:08X}", BLUE, False),
            (", ", INK, False),
            ("rs2", PURPLE, True),
            ("=", INK, False),
            ("P17", PURPLE, True),
            (" -> ", INK, False),
            ("P21", PURPLE, True),
        ),
        max_width=475,
    )

    figure.rect(24, 230, 1152, 555, role="neutral", radius=0)

    # Dynamic instructions use the same physical pipeline. Draw their paths
    # before the stage symbols so only the inter-stage segments remain visible.
    figure.arrow(
        ((40, 500), (1160, 500)),
        kind="control", label="I0", color=AMBER, width=3,
        underlay=True,
    )
    figure.arrow(
        ((40, 535), (1160, 535)),
        kind="control", label="I1", color=GREEN, width=3,
        underlay=True,
    )

    # ROB occupancy: neutral entries plus the two live loads at the commit end.
    figure.text(590, 252, "ROB entries (oldest at right)", size=17,
                bold=True, color=RED, anchor="middle")
    for index in range(13):
        role = "transfer" if index == 12 else "compute" if index == 11 else "neutral"
        figure.rect(330 + index * 40, 270, 40, 70, role=role,
                    stroke_width=1, radius=0)
    figure.text(790, 312, "I1", size=16, bold=True,
                color=GREEN, anchor="middle")
    figure.text(830, 312, "I0", size=16, bold=True,
                color=AMBER, anchor="middle")
    figure.text(830, 362, "head", size=16, color=RED, anchor="middle")
    figure.rect(950, 270, 180, 65, role="verify", radius=0)
    figure.text(1040, 298, "Commit", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(1040, 323, "I0 before I1", size=16, anchor="middle")

    # Main load datapath.
    figure.rect(40, 440, 95, 110, role="data", radius=0)
    figure.text(87, 468, "Fetch", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(87, 500, "I0", size=16, bold=True,
                color=AMBER, anchor="middle")
    figure.text(87, 535, "I1", size=16, bold=True,
                color=GREEN, anchor="middle")

    figure.rect(165, 440, 100, 110, role="data", radius=0)
    figure.text(215, 468, "Decode", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(215, 500, "flow", size=16, anchor="middle")
    figure.text(215, 535, "bind", size=16, anchor="middle")

    figure.rect(295, 425, 125, 140, role="state", radius=0)
    figure.text(357, 456, "Rename", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(357, 490, "I0 rd->P17", size=16, mono=True,
                anchor="middle")
    figure.text(357, 522, "I1 rs2=P17", size=16, mono=True,
                anchor="middle")
    figure.text(357, 552, "I1 rd->P21", size=16, mono=True,
                anchor="middle")

    figure.queue(455, 435, 140, 120, role="state")
    figure.text(525, 463, "Issue / select", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(525, 500, "I0 request=1", size=16, anchor="middle")
    figure.text(525, 535, "I1 waits P17", size=16, anchor="middle")

    figure.table(635, 415, 150, 160, 3, role="data")
    figure.text(710, 446, "Physical regs", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(710, 500, "P17 ReusePlan", size=16, mono=True,
                anchor="middle")
    figure.text(710, 553, "P21 property", size=16, mono=True,
                anchor="middle")

    figure.diamond(850, 495, 100, 110, role="compute")
    figure.text(850, 490, "AGU", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(850, 520, "EA", size=16, anchor="middle")

    figure.table(925, 400, 145, 190, 4, role="state")
    for x in range(943, 1070, 18):
        figure.line((x, 400), (x, 438), color=PURPLE, width=1)
    figure.text(997, 429, "LSQ", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(997, 476, "LQ0 record", size=16, anchor="middle")
    figure.text(997, 523, "LQ1 property", size=16, anchor="middle")
    figure.text(997, 570, "order / replay", size=16, anchor="middle")

    figure.rect(1100, 440, 70, 110, role="data", radius=0)
    figure.text(1135, 478, "L1D", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(1135, 515, "tag", size=16, anchor="middle")
    figure.text(1135, 540, "data", size=16, anchor="middle")

    # Allocation, completion, response, and dependency feedback.
    figure.arrow(
        ((357, 425), (357, 375), (790, 375), (790, 340)),
        kind="control", label="ROB allocation", color=PURPLE,
        label_at=(555, 368),
    )
    figure.arrow(
        ((710, 415), (710, 360), (830, 360), (830, 340)),
        kind="control", label="ROB completion", color=BLUE,
        label_at=(765, 390),
    )
    figure.arrow(
        ((850, 302), (950, 302)),
        kind="control", label="Commit", color=RED,
    )
    figure.arrow(
        ((1135, 550), (1135, 700), (710, 700), (710, 575)),
        kind="transfer", label="load-data response",
        cadence="per completed load", color=BLUE,
        label_at=(960, 690),
    )
    figure.arrow(
        ((635, 548), (615, 548), (615, 650), (525, 650), (525, 555)),
        kind="dependency", label="P17 wakes I1", color=PURPLE,
        label_at=(570, 640),
    )
    figure.text(
        600, 750,
        "Fetch -> Decode -> Rename -> IEW {issue, register read, AGU, LSQ, "
        "writeback} -> Commit",
        size=16, bold=True, color=INK, anchor="middle", max_width=1080,
    )

    figure.section(
        "2", "GRAPH, CSR, AND EDGE-ALIGNED REUSEPLAN",
        "fixture row u=4 maps to internal CSR row u=8",
        825, role="data",
    )
    figure.rect(24, 867, 1152, 328, role="neutral", radius=0)
    figure.text(42, 897, "Outgoing row u=4",
                size=17, bold=True, color=BLUE)

    graph_center = (140, 1040)
    graph_neighbors = {
        1: (55, 945, 4),
        2: (115, 920, 3),
        3: (185, 920, 2),
        5: (255, 950, 1),
        7: (300, 1040, 5),
    }
    for vertex, (x, y, weight) in graph_neighbors.items():
        if vertex == 7:
            figure.arrow(
                ((graph_center[0] + 24, graph_center[1]), (x - 24, y)),
                kind="model-edge", color=RED, width=3,
            )
        else:
            figure.line(graph_center, (x, y), color=GRAY, width=2)
        figure.circle(x, y, 22, fill=WHITE,
                      stroke=RED if vertex == 7 else BLUE)
        figure.text(x, y + 6, str(vertex), size=16, bold=True,
                    anchor="middle")
        wx = (graph_center[0] + x) / 2
        wy = (graph_center[1] + y) / 2 - 8
        figure.text(wx, wy, f"w{weight}", size=16, color=GRAY,
                    anchor="middle")
    figure.circle(graph_center[0], graph_center[1], 24,
                  fill=AMBER_MATTE, stroke=AMBER)
    figure.text(graph_center[0], graph_center[1] + 6, "4",
                size=16, bold=True, anchor="middle")
    figure.text(185, 1090, "tracked adjacency (4,7), weight 5",
                size=16, bold=True, color=RED, anchor="middle")
    figure.text(42, 1168, "N_out_fixture(4) = {1, 2, 3, 5, 7}",
                size=16, mono=True, color=BLUE)

    figure.text(390, 897, "Internal CSR row u=8 and aligned ReusePlan",
                size=17, bold=True, color=BLUE)
    figure.text(500, 925, "row_ptr[8]=14; row_ptr[9]=19",
                size=16, mono=True)
    cell_x = (500, 620, 740, 860, 980)
    cell_centers = tuple(x + 60 for x in cell_x)
    for center, edge_pos in zip(cell_centers, range(14, 19)):
        figure.text(center, 954, str(edge_pos), size=16, color=GRAY,
                    anchor="middle")
    for row_y, role in ((968, "data"), (1013, "neutral"), (1058, "state")):
        for index, x in enumerate(cell_x):
            figure.rect(
                x, row_y, 120, 45 if row_y < 1058 else 70,
                role="transfer" if index == 4 else role,
                stroke_width=1, radius=0,
            )
    figure.text(390, 996, "col_idx", size=16, bold=True, mono=True)
    figure.text(390, 1041, "weight", size=16, bold=True, mono=True)
    figure.text(390, 1097, "ReusePlan", size=16, bold=True, mono=True)
    for center, value in zip(cell_centers, (3, 6, 7, 11, 18)):
        figure.text(center, 996, str(value), size=16, mono=True,
                    anchor="middle")
    for center, value in zip(cell_centers, (4, 3, 2, 1, 5)):
        figure.text(center, 1041, str(value), size=16, mono=True,
                    anchor="middle")
    for center, edge_pos in zip(cell_centers[:-1], range(14, 18)):
        figure.text(center, 1097, f"RP{edge_pos}", size=16, mono=True,
                    color=GRAY, anchor="middle")
    figure.text(cell_centers[-1], 1085, "dest18 | T1", size=16,
                bold=True, mono=True, color=AMBER, anchor="middle")
    figure.text(cell_centers[-1], 1115, "e11 | e15", size=16,
                bold=True, mono=True, color=AMBER, anchor="middle")
    figure.text(
        390, 1148,
        "edge_pos 18: fixture (4,7) -> internal (8,18)",
        size=16, mono=True, color=RED,
    )
    figure.text(
        390, 1178,
        f"I0 loads ReusePlan[18]; I1 address = "
        f"0x{fx.property_address - fx.tracked_dest * fx.property_element_bytes:08X} "
        f"+ 18*4 = 0x{fx.property_address:08X}",
        size=16, mono=True, color=PURPLE, max_width=755,
    )

    figure.section(
        "3", "TWO REQUESTS FROM THE LSQ",
        "I0 completes before P17 wakes I1",
        1235, role="compute",
    )
    figure.rect(24, 1277, 1152, 340, role="neutral", radius=0)
    request_columns = (
        (100, "LSQ"),
        (320, "D-TLB + private caches"),
        (540, "MSHR"),
        (760, "LLC"),
        (1000, "Response / WB"),
    )
    for x, label in request_columns:
        figure.text(x, 1310, label, size=16, bold=True, anchor="middle")
        figure.line((x, 1325), (x, 1585), color=GRAY, width=1)

    figure.arrow(
        ((70, 1360), (1120, 1360)),
        kind="transfer", label="I0 record Request",
        cadence="per adjacency entry", color=AMBER, width=3,
        label_at=(600, 1348),
    )
    figure.arrow(
        ((70, 1500), (1120, 1500)),
        kind="transfer", label="I1 property Request + ReuseBind",
        cadence="per governed property load", color=BLUE, width=3,
        label_at=(600, 1488),
    )
    for x in (100, 320, 540, 760, 1000):
        figure.circle(x, 1360, 9, fill=WHITE, stroke=AMBER)
        figure.circle(x, 1500, 9, fill=WHITE, stroke=BLUE)

    figure.text(45, 1366, "I0", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(100, 1395, "4-byte record", size=16,
                color=AMBER, anchor="middle")
    figure.text(100, 1422, "FlowThrough=1", size=16,
                color=AMBER, anchor="middle")
    figure.text(320, 1395, "normal lookup/hit", size=16, anchor="middle")
    figure.text(540, 1395, "record-block MSHR", size=16, anchor="middle")
    figure.text(760, 1395, "hit normal; miss may", size=16, anchor="middle")
    figure.text(760, 1422, "skip LLC allocation", size=16, anchor="middle")
    figure.text(1000, 1395, "widen compact record", size=16, anchor="middle")
    figure.text(1000, 1422, "writeback P17", size=16,
                color=AMBER, anchor="middle")

    figure.arrow(
        ((1000, 1369), (1000, 1455), (100, 1455), (100, 1500)),
        kind="dependency", label="P17 wakeup", color=PURPLE,
        label_at=(550, 1445),
    )

    figure.text(45, 1506, "I1", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(100, 1535, "4-byte U32", size=16,
                color=GREEN, anchor="middle")
    figure.text(100, 1562, "ReuseBind", size=16,
                color=GREEN, anchor="middle")
    figure.text(320, 1535, "normal lookup/hit", size=16, anchor="middle")
    figure.text(540, 1535, "property MSHR", size=16, anchor="middle")
    figure.text(760, 1535, "guard + valid hit/fill", size=16, anchor="middle")
    figure.text(
        760, 1562, f"stamp 0x{fx.property_line:08X}",
        size=16, mono=True, anchor="middle",
    )
    figure.text(
        1000, 1535, f"property[{fx.tracked_dest}] -> P21",
        size=16, mono=True, anchor="middle",
    )
    figure.text(1000, 1562, "FlowThrough=0", size=16, anchor="middle")
    figure.text(
        600, 1602,
        f"ReuseBind = dest{fx.tracked_dest} | T{fx.line_tier} | "
        f"e{fx.first_epoch}/e{fx.second_epoch} | current{fx.tracked_reader} "
        "| context k | sequence s",
        size=16, mono=True, color=PURPLE, anchor="middle", max_width=1080,
    )

    figure.section(
        "4", "WRITEBACK, COMMIT, AND ARCHITECTURAL EFFECT",
        "both loads retain native O3 ordering and fault behavior",
        1655, role="verify",
    )
    figure.rect(24, 1697, 650, 165, role="verify", radius=0)
    figure.line((24, 1742), (674, 1742), color=INK, width=1)
    figure.line((24, 1797), (674, 1797), color=INK, width=1)
    figure.line((94, 1697), (94, 1862), color=INK, width=1)
    figure.line((390, 1697), (390, 1862), color=INK, width=1)
    for x, label in (
        (59, "Lane"),
        (242, "Completion"),
        (532, "Commit"),
    ):
        figure.text(x, 1727, label, size=16, bold=True, anchor="middle")
    figure.text(59, 1778, "I0", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(110, 1778, "P17 ready; LQ0 complete", size=16)
    figure.text(406, 1778, "commit when ROB0 is oldest", size=16)
    figure.text(59, 1833, "I1", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(110, 1833, "P21 ready; LQ1 complete", size=16)
    figure.text(406, 1833, "commit after I0", size=16)

    figure.rect(700, 1697, 476, 165, role="neutral", radius=0)
    figure.text(716, 1727, "Request-specific effects", size=16,
                bold=True, color=PURPLE)
    figure.lines(
        716, 1760,
        (
            "I0 FlowThrough: record-miss LLC allocation only",
            "I1 ReuseBind: matching property-line metadata only",
            "native replay, squash, fault, and retirement rules",
            "neither mechanism changes the loaded property value",
        ),
        max_width=444,
    )
    save(figure, generated)


def mshr_lifecycle(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "03", "mshr-metadata-lifecycle"),
        "ReuseBind merge, response, and line-metadata lifetime",
        "MSHR merge validity and FlowThrough allocation are separate state machines.",
        "The figure traces a typed ReuseBind extension into an MSHR target list, "
        "shows the exact conflict rules and newest-sequence rule, applies the "
        "selected extension to the downstream fill, validates the destination line "
        "at the LLC, and separates this from allocOnFill aggregation.",
        1370,
    )
    figure.section(
        "1", "REQUEST EXTENSION", "one dynamic governed property load",
        138, role="state",
    )
    ext_fields = (
        ("dest", 130, "data"),
        ("tier", 90, "transfer"),
        ("epoch1", 120, "compute"),
        ("epoch2", 120, "state"),
        ("count", 90, "state"),
        ("current", 110, "compute"),
        ("context", 110, "verify"),
        ("sequence", 120, "neutral"),
        ("conflict", 110, "verify"),
    )
    x = 40
    for label, width, role in ext_fields:
        figure.rect(x, 205, width, 85, role=role, stroke_width=1, radius=0)
        figure.text(x + width / 2, 255, label, size=16, bold=True,
                    mono=True, anchor="middle")
        x += width
    figure.text(40, 330, "EcgReusePlanExtension cloned with Request",
                size=17, bold=True, color=PURPLE)
    figure.text(
        560, 330,
        "valid iff context != 0 and conflict == 0",
        size=16, mono=True, color=RED,
    )

    figure.section(
        "2", "MSHR TARGET-LIST MERGE", "rebuild state whenever active targets change",
        410, role="compute",
    )
    figure.rect(40, 465, 530, 235, role="neutral", radius=0)
    for y in (510, 558, 606, 654):
        figure.line((40, y), (570, y), color=INK, width=1)
    for x in (125, 245, 350, 455):
        figure.line((x, 465), (x, 700), color=INK, width=1)
    headers = ("target", "governed", "requestor", "context", "sequence", "alloc")
    centers = (82, 185, 297, 402, 512)
    figure.text(82, 495, "target", size=16, bold=True, anchor="middle")
    figure.text(185, 495, "governed", size=16, bold=True, anchor="middle")
    figure.text(297, 495, "requestor", size=16, bold=True, anchor="middle")
    figure.text(402, 495, "context", size=16, bold=True, anchor="middle")
    figure.text(512, 495, "seq / alloc", size=16, bold=True, anchor="middle")
    rows = (
        ("A", "yes", "cpu0", "k", "17 / false"),
        ("B", "yes", "cpu0", "k", "18 / true"),
        ("C", "yes/no", "cpu?", "k?", "19 / false"),
        ("state", "compat?", "same?", "same?", "newest / OR"),
    )
    for row, values in enumerate(rows):
        y = 544 + row * 48
        for x, value in zip((82, 185, 297, 402, 512), values):
            figure.text(x, y, value, size=16, mono=True, anchor="middle")

    figure.diamond(700, 575, 210, 135, role="compute")
    figure.text(700, 582, "compatible?", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.arrow(
        ((570, 575), (595, 575)),
        kind="control", label="target", color=PURPLE,
    )
    figure.rect(845, 465, 315, 105, role="state", radius=0)
    figure.text(862, 500, "selected extension", size=17, bold=True,
                color=PURPLE)
    figure.text(862, 535, "newest compatible sequence", size=16)
    figure.text(862, 562, "equal seq requires same payload", size=16)
    figure.rect(845, 595, 315, 105, role="verify", radius=0)
    figure.text(862, 630, "conflict state", size=17, bold=True,
                color=RED)
    figure.text(862, 665, "mixed / mismatch / invalid", size=16)
    figure.arrow(
        ((805, 575), (825, 575), (825, 518), (845, 518)),
        kind="control", label="selected extension", color=GREEN,
    )
    figure.arrow(
        ((805, 575), (825, 575), (825, 648), (845, 648)),
        kind="control", label="conflict state", color=RED,
    )
    figure.text(
        600, 710,
        "allocOnFill = OR(target allocOnFill); independent of ReuseBind validity",
        size=16, bold=True, mono=True, color=AMBER, anchor="middle",
        max_width=1080,
    )

    figure.section(
        "3", "RESPONSE AND LLC ACCEPTANCE", "conflict never becomes a valid line stamp",
        742, role="verify",
    )
    figure.rect(40, 800, 220, 105, role="state", radius=0)
    figure.text(150, 835, "response Request", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(150, 870, "selected ext + conflict", size=16,
                anchor="middle")
    figure.diamond(390, 852, 190, 125, role="verify")
    figure.text(390, 840, "conflicted?", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(390, 872, "reject if yes", size=16, anchor="middle")
    figure.diamond(650, 852, 210, 125, role="compute")
    figure.text(650, 858, "line match?", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.rect(820, 800, 330, 105, role="state", radius=0)
    figure.text(985, 832, "LLC line metadata", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(985, 865, "tier | e1 | e2 | context | valid",
                size=16, mono=True, anchor="middle")
    figure.arrow(
        ((260, 852), (295, 852)),
        kind="control", label="response Request", color=PURPLE,
    )
    figure.arrow(
        ((485, 852), (545, 852)),
        kind="control", label="conflicted?", color=GREEN,
    )
    figure.arrow(
        ((755, 852), (820, 852)),
        kind="control", label="line match?", color=GREEN,
    )
    figure.text(
        600, 950,
        "serviceable/deferred target changes rebuild merge state; "
        "deallocation resets MSHR ECG state",
        size=16, color=RED, anchor="middle", max_width=1080,
    )

    figure.section(
        "4", "LINE LIFETIME", "metadata is advisory; data correctness stays architectural",
        1074, role="neutral",
    )
    lifetime_y = 1180
    figure.arrow(
        ((90, lifetime_y), (1110, lifetime_y)),
        kind="control", label="line metadata lifetime", color=PURPLE,
        label_at=(600, 1165),
    )
    for x, title, body, color in (
        (140, "accept", "valid hit/fill", GREEN),
        (430, "refresh", "later governed hit", BLUE),
        (720, "evolve", "native RRPV/recency", PURPLE),
        (1010, "clear", "invalidate or evict", RED),
    ):
        figure.circle(x, lifetime_y, 14, fill=WHITE, stroke=color)
        figure.text(x, 1225, title, size=17, bold=True,
                    color=color, anchor="middle")
        figure.text(x, 1255, body, size=16, anchor="middle")
    figure.text(
        600, 1315,
        "property bytes are never modified by ReuseBind metadata",
        size=16, bold=True, color=RED, anchor="middle",
    )
    save(figure, generated)


def checked_walkthrough(
    fx: CheckedFixture, generated: list[tuple[Path, Path]]
) -> None:
    current = fx.tracked_reader
    d1 = (fx.first_epoch + fx.epoch_count - current) % fx.epoch_count
    d2 = (fx.second_epoch + fx.epoch_count - current) % fx.epoch_count
    figure = Figure(
        ROOT,
        FigureTarget("property-to-cache-walkthrough", "01", "checked-request"),
        "From adjacency entry 4 -> 7 to LLC line 0x80000040",
        "Every number is derived from fig/ecg-figure-fixture.json.",
        "A single checked-fixture edge is followed from its edge-aligned compact "
        "record through the record load, explicit register dependency, computed "
        "property address, typed Request extension, LLC line stamp, circular "
        "distance, normal data completion, and later victim selection.",
        2070,
    )
    figure.section(
        "1", "TRACKED ADJACENCY ENTRY AND RECORD",
        "one fixture-backed entry; not a measured workload",
        138, role="data",
    )
    figure.rect(24, 180, 500, 280, role="neutral", stroke=INK, stroke_width=3)
    figure.text(42, 211, "In-neighbors of vertex 7", size=17, bold=True,
                color=BLUE, max_width=330)
    source_readers = sorted({
        right if left == fx.tracked_source_dest else left
        for left, right, _weight in fx.weighted_edges
        if fx.tracked_source_dest in {left, right}
    })
    reader_positions = tuple(
        (reader, 100, 250 + index * 58)
        for index, reader in enumerate(source_readers)
    )
    for reader, x, y in reader_positions:
        figure.arrow(
            ((x + 24, y), (388, 340)),
            kind="model-edge",
            color=RED if reader == fx.tracked_source_reader else GRAY,
            width=3 if reader == fx.tracked_source_reader else 2,
        )
    figure.rect(350, 225, 150, 215, role="state", stroke=PURPLE, stroke_width=2)
    figure.text(425, 254, "64-byte line", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(425, 280, "vertices 16..31", size=16, anchor="middle")
    figure.circle(425, 340, 30, fill=PURPLE_MATTE, stroke=RED)
    figure.text(425, 347, "18", size=17, bold=True, anchor="middle")
    for reader, x, y in reader_positions:
        figure.circle(
            x, y, 24,
            fill=AMBER_MATTE
            if reader == fx.tracked_source_reader else WHITE,
            stroke=AMBER if reader == fx.tracked_source_reader else BLUE,
        )
        figure.text(x, y + 6, str(reader), size=16, bold=True, anchor="middle")
    figure.circle(350, 205, 18, fill=RED, stroke=RED)
    figure.text(350, 211, "A", size=17, bold=True, color=WHITE, anchor="middle")
    figure.text(
        380, 211,
        f"adjacency {fx.tracked_source_reader} -> "
        f"{fx.tracked_source_dest}",
                size=17, bold=True, color=RED, max_width=310)

    figure.rect(550, 180, 626, 280, role="transfer", stroke=INK, stroke_width=3)
    figure.text(570, 211, "B  edge-aligned compact ReusePlan",
                size=17, bold=True, color=AMBER, max_width=560)
    figure.lines(
        570, 244,
        (
            f"outer vertex {fx.tracked_source_reader} -> "
            f"internal outer vertex {fx.tracked_reader}",
            f"outgoing CSR row accesses property {fx.tracked_dest}",
            f"line tier = {fx.line_tier}; next access sources = "
            f"{fx.first_reader}, {fx.second_reader}",
            f"property address = 0x{fx.property_address:08X}; "
            f"line = 0x{fx.property_line:08X}",
        ),
        mono=True,
        max_width=580,
    )
    figure.bitfield(
        575, 335, 576, 90,
        (
            (f"dest {fx.tracked_dest}", fx.id_bits, "data"),
            (f"T{fx.line_tier}", 2, "transfer"),
            (f"e1 {fx.first_epoch}", fx.epoch_bits, "compute"),
            (f"e2 {fx.second_epoch}", fx.epoch_bits, "state"),
        ),
        total_bits=fx.id_bits + 2 + 2 * fx.epoch_bits,
    )

    figure.section(
        "2", "SOFTWARE LOOP AND HARDWARE CORRELATION",
        "outgoing example; PageRank uses incoming rows",
        505, role="state",
    )
    figure.rect(24, 547, 650, 300, role="neutral", stroke=INK, stroke_width=3)
    figure.text(44, 578, "Representative property-access pseudocode",
                size=17, bold=True, color=PURPLE, max_width=580)
    pseudocode = (
        ("L1", (("for ", RED, True), ("u", INK, False),
                (" in ", RED, True), ("active_vertices", BLUE, False),
                (":", INK, False))),
        ("L2", (("  for ", RED, True), ("edge_pos", INK, False),
                (" in ", RED, True), ("out_csr_row", BLUE, False),
                ("(u):", INK, False))),
        ("L3", (("    plan", PURPLE, True), (" = ", INK, False),
                ("ecg.flow.load.compact", AMBER, True),
                ("(record[edge_pos])  ", INK, False), ("[B]", AMBER, True))),
        ("L4", (("    v", INK, False), (" = ", INK, False),
                ("plan", PURPLE, True), (".destination", BLUE, False))),
        ("L5", (("    addr", BLUE, True), (" = ", INK, False),
                ("property_base", BLUE, False), (" + v * 4", INK, False))),
        ("L6", (("    value", BLUE, True), (" = ", INK, False),
                ("ecg.bind.load.u32", GREEN, True),
                ("(addr, ", INK, False), ("plan", PURPLE, True),
                (")  ", INK, False), ("[C,D]", PURPLE, True))),
        ("L7", (("    proposal", INK, False), (" = ", INK, False),
                ("update", BLUE, False), ("(u, v, value)", INK, False))),
    )
    for index, (line_no, tokens) in enumerate(pseudocode):
        y = 615 + index * 30
        figure.text(44, y, line_no, size=16, color=GRAY, mono=True)
        figure.token_line(88, y, tokens, max_width=560)
    figure.text(
        44, 830,
        f"tracked execution: adjacency {fx.tracked_source_reader}->"
        f"{fx.tracked_source_dest} maps to internal "
        f"{fx.tracked_reader}->{fx.tracked_dest}",
        size=16, color=RED, max_width=600,
    )

    figure.rect(700, 547, 476, 300, role="state", stroke=INK, stroke_width=3)
    figure.text(720, 578, "Cross-layer identifiers",
                size=17, bold=True, color=PURPLE, max_width=430)
    callouts = (
        (
            "A",
            f"adjacency ({fx.tracked_source_reader},{fx.tracked_source_dest}) "
            f"/ internal ({fx.tracked_reader},{fx.tracked_dest})",
            RED,
        ),
        (
            "B",
            f"record: dest{fx.tracked_dest} | T{fx.line_tier} | "
            f"e{fx.first_epoch} | e{fx.second_epoch}",
            AMBER,
        ),
        ("C", "record rd becomes bind-load rs2", PURPLE),
        ("D", "property Request carries ReuseBind", BLUE),
        ("E", f"LLC line 0x{fx.property_line:08X} stores the stamp", GREEN),
    )
    for index, (letter, text, color) in enumerate(callouts):
        y = 625 + index * 43
        figure.circle(730, y - 6, 15, fill=color, stroke=color)
        figure.text(730, y, letter, size=16, bold=True,
                    color=WHITE, anchor="middle")
        figure.text(758, y, text, size=16, max_width=390)

    figure.section(
        "3", "TWO-INSTRUCTION DATA DEPENDENCY",
        "the compact record becomes an explicit rs2 operand",
        885, role="compute",
    )
    figure.arrow(
        ((55, 1030), (1145, 1030)),
        kind="dependency", label="instruction operand / Request state",
        color=PURPLE, width=3, underlay=True,
    )
    figure.table(40, 950, 190, 160, 3, role="transfer")
    figure.text(135, 982, "Record array", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(135, 1030, f"record[{fx.tracked_reader}->{fx.tracked_dest}]",
                size=16, mono=True, anchor="middle")
    figure.text(135, 1080, "4-byte compact", size=16, anchor="middle")
    figure.diamond(330, 1030, 150, 130, role="transfer")
    figure.text(330, 1036, "record load", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(330, 1125, "widen compact ReusePlan", size=16,
                color=AMBER, anchor="middle")
    figure.table(455, 950, 180, 160, 3, role="state")
    figure.text(545, 982, "Physical reg", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(545, 1030, "P17 ReusePlan", size=16, mono=True,
                anchor="middle")
    figure.text(545, 1080, "I1 rs2 dependency", size=16,
                anchor="middle")
    figure.diamond(745, 1030, 160, 130, role="compute")
    figure.text(745, 945, "ecg.bind.load.u32", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(745, 1036, "property load", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(745, 1125, f"EA=0x{fx.property_address:08X}",
                size=16, mono=True, color=BLUE, anchor="middle")
    figure.table(880, 935, 270, 190, 4, role="state")
    figure.text(1015, 967, "LSQ Request", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(1015, 1012, f"dest={fx.tracked_dest} T{fx.line_tier}",
                size=16, mono=True, anchor="middle")
    figure.text(1015, 1060,
                f"e{fx.first_epoch}/e{fx.second_epoch} current={current}",
                size=16, mono=True, anchor="middle")
    figure.text(1015, 1105, "context=k sequence=s", size=16,
                mono=True, anchor="middle")
    figure.text(
        600, 1205, "C  instruction operand / Request state",
        size=16, bold=True, color=PURPLE, anchor="middle",
    )

    figure.section(
        "4", "TWO REQUEST LANES THROUGH THE CACHE", "record placement and property metadata remain distinct",
        1245, role="data",
    )
    lane_nodes = ((100, "LSQ"), (360, "Private caches"),
                  (620, "MSHR"), (900, "LLC"))
    figure.arrow(
        ((70, 1350), (1130, 1350)),
        kind="transfer", label="record Request",
        cadence="per tracked edge", color=AMBER, width=3,
        label_at=(600, 1338),
    )
    figure.arrow(
        ((70, 1495), (1130, 1495)),
        kind="transfer", label="property Request + ReuseBind",
        cadence="per tracked edge", color=BLUE, width=3,
        label_at=(600, 1483),
    )
    for x, label in lane_nodes:
        figure.line((x, 1305), (x, 1555), color=GRAY, width=1)
        figure.text(x, 1295, label, size=16, bold=True, anchor="middle")
        figure.circle(x, 1350, 9, fill=WHITE, stroke=AMBER)
        figure.circle(x, 1495, 9, fill=WHITE, stroke=BLUE)
    figure.text(100, 1390, "ECG_FLOWTHROUGH", size=16,
                mono=True, color=AMBER, anchor="middle")
    figure.text(360, 1390, "normal lookup/fill", size=16, anchor="middle")
    figure.text(620, 1390, "record block; alloc=false",
                size=16, anchor="middle")
    figure.text(900, 1390, "hit normal; miss may skip fill",
                size=16, anchor="middle")
    figure.text(100, 1535, "ReuseBind; alloc=true", size=16,
                color=BLUE, anchor="middle")
    figure.text(360, 1535, "normal lookup/fill", size=16, anchor="middle")
    figure.text(620, 1535, "property block; merge ext",
                size=16, anchor="middle")
    figure.text(
        900, 1535,
        f"guard + stamp T{fx.line_tier}/e{fx.first_epoch}/e{fx.second_epoch}",
        size=16, anchor="middle",
    )
    figure.text(
        600, 1580,
        "D  record Request | property Request + ReuseBind | per tracked edge",
        size=16, bold=True, color=BLUE, anchor="middle", max_width=850,
    )

    figure.section(
        "5", "LINE STAMP, REUSE TIMELINE, AND LATER VICTIM", "the property value is already complete",
        1625, role="state",
    )
    figure.rect(24, 1667, 720, 250, role="state", stroke=INK, stroke_width=2)
    figure.text(44, 1698, f"E  LLC line 0x{fx.property_line:08X}",
                size=17, bold=True, color=PURPLE, max_width=420)
    axis_y = 1805
    figure.line((80, axis_y), (700, axis_y), color=INK, width=3)
    for epoch, color, label in (
        (current, AMBER, f"current {current}"),
        (fx.first_epoch, GREEN, f"e1 {fx.first_epoch}"),
        (fx.second_epoch, PURPLE, f"e2 {fx.second_epoch}"),
    ):
        x = 80 + 620 * epoch / (fx.epoch_count - 1)
        figure.circle(x, axis_y, 12, fill=WHITE, stroke=color)
        label_y = axis_y + 50 if epoch == fx.first_epoch else axis_y - 32
        figure.text(x, label_y, label, size=16, bold=True,
                    color=color, anchor="middle")
    figure.text(
        384, 1875,
        f"nearest = min({d1}, {d2}) = {min(d1, d2)}; stored epochs remain absolute",
        size=16, mono=True, color=PURPLE, anchor="middle", max_width=650,
    )
    figure.diamond(860, 1745, 180, 120, role="verify")
    figure.text(860, 1732, "max-RRPV", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(860, 1762, "eligible?", size=16, anchor="middle")
    figure.diamond(1060, 1745, 180, 120, role="transfer")
    figure.text(1060, 1732, "structural", size=17, bold=True,
                color=AMBER, anchor="middle")
    figure.text(1060, 1762, "candidate?", size=16, anchor="middle")
    figure.arrow(
        ((950, 1745), (970, 1745)),
        kind="control", label="max-RRPV", color=RED,
    )
    figure.text(790, 1840, "yes -> oldest structural",
                size=16, color=AMBER)
    figure.text(790, 1870, "else -> farthest stamped property",
                size=16, color=PURPLE)
    figure.text(790, 1900, "property value unchanged; no speedup claim",
                size=16, color=RED)
    save(figure, generated)


def architecture_state_map(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("property-to-cache-walkthrough", "02", "architecture-state-map"),
        "ReusePlan state placement across software, core, and LLC",
        "Containment denotes storage; arrows denote architectural operand or Request transfer.",
        "The architecture map places offline records, architectural format/current/context "
        "CSRs, renamed ReusePlan operands, load-queue state, typed Request extensions, "
        "MSHR merge state, and line-local LLC metadata in their actual controlling "
        "structures. It distinguishes exact gem5 O3 binding from diagnostic models.",
        1240,
    )
    figure.section(
        "1", "SOFTWARE AND ARCHITECTURAL INPUTS", "immutable graph data is separate from live execution state",
        138, role="data",
    )
    figure.table(40, 190, 520, 190, 4, role="data")
    figure.text(300, 220, "Memory image before ROI", size=17,
                bold=True, color=BLUE, anchor="middle")
    figure.text(56, 265, "CSR / weighted structural arrays", size=16)
    figure.text(56, 312, "edge-aligned ReusePlan / sidecar arrays", size=16)
    figure.text(56, 360, "property arrays + graph/payload identity", size=16)
    figure.table(640, 190, 520, 190, 4, role="state")
    figure.text(900, 220, "Architectural ECG CSR bank", size=17,
                bold=True, color=PURPLE, anchor="middle")
    figure.text(656, 265, "format: id_bits | epoch_bits", size=16,
                mono=True)
    figure.text(656, 312, "current: quantized traversal epoch", size=16)
    figure.text(656, 360, "context: nonzero execution identity", size=16)

    figure.section(
        "2", "OUT-OF-ORDER CORE", "standard O3 structures carry one explicit dependency",
        450, role="compute",
    )
    figure.arrow(
        ((50, 600), (1135, 600)),
        kind="control", label="dynamic instruction state", color=GREEN,
        label_at=(600, 735), underlay=True,
    )
    figure.rect(40, 530, 150, 140, role="data", radius=0)
    figure.text(115, 562, "Decode", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(115, 602, "custom-0", size=16, anchor="middle")
    figure.text(115, 642, "role + width", size=16, anchor="middle")
    figure.rect(230, 510, 170, 180, role="state", radius=0)
    figure.text(315, 542, "Rename / ROB", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(315, 585, "I0 rd -> P17", size=16, mono=True,
                anchor="middle")
    figure.text(315, 625, "I1 rs2 -> P17", size=16, mono=True,
                anchor="middle")
    figure.text(315, 665, "ROB + LQ allocate", size=16, anchor="middle")
    figure.queue(440, 530, 170, 140, role="compute")
    figure.text(525, 562, "Issue / select", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(525, 605, "I0 ready", size=16, anchor="middle")
    figure.text(525, 645, "I1 waits P17", size=16, anchor="middle")
    figure.diamond(700, 600, 150, 150, role="compute")
    figure.text(700, 590, "AGU", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(700, 625, "EA", size=16, anchor="middle")
    figure.text(700, 695, "record or property address", size=16,
                color=GREEN, anchor="middle")
    figure.table(820, 505, 180, 190, 4, role="state")
    figure.text(910, 537, "Load queue", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(910, 580, "order / replay", size=16, anchor="middle")
    figure.text(910, 625, "role + size", size=16, anchor="middle")
    figure.text(910, 672, "ReuseBind ext", size=16, anchor="middle")
    figure.rect(1040, 530, 120, 140, role="state", radius=0)
    figure.text(1100, 562, "Request", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(1100, 602, "address", size=16, anchor="middle")
    figure.text(1100, 642, "typed ext", size=16, anchor="middle")

    figure.section(
        "3", "CACHE HIERARCHY", "private caches stay ordinary; LLC state is extended",
        782, role="state",
    )
    figure.arrow(
        ((60, 920), (1130, 920)),
        kind="transfer", label="property Request + ReuseBind",
        cadence="per governed load", color=BLUE, width=3,
        label_at=(600, 1070), underlay=True,
    )
    cache_nodes = (
        (40, 850, 180, "L1D", "ordinary tags/data"),
        (280, 850, 180, "L2", "ordinary tags/data"),
        (520, 850, 210, "MSHR target list", "allocOnFill + merge"),
        (790, 830, 360, "LLC replacement entry",
         "valid | property | RRPV | recency\n"
         "tier | e1 | e2 | count\n"
         "context | stamp"),
    )
    for x, y, width, title, body in cache_nodes:
        height = 180 if x >= 790 else 140
        role = "state" if x >= 520 else "data"
        figure.rect(x, y, width, height, role=role, radius=0)
        figure.text(x + width / 2, y + 35, title, size=17, bold=True,
                    color=PURPLE if role == "state" else BLUE,
                    anchor="middle")
        body_lines = body.split("\n")
        for index, line in enumerate(body_lines):
            figure.text(x + width / 2, y + 78 + index * 36, line,
                        size=16, mono=x >= 790, anchor="middle")
    figure.text(625, 1015, "newest compatible sequence; conflict propagates",
                size=16, color=AMBER, anchor="middle")
    figure.arrow(
        ((130, 1145), (1070, 1145)),
        kind="transfer",
        label="property bytes return normally; metadata remains advisory at the LLC",
        cadence="per governed hit or fill",
        color=BLUE,
        label_at=(600, 1130),
    )
    save(figure, generated)


def evidence_boundary(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("evaluation-methodology", "01", "evidence-boundary"),
        "Evaluation evidence and admissible claims",
        "Rows are compared only with matching baselines inside the same simulator.",
        "The figure separates the questions answered by gem5 O3, cache_sim, and "
        "Sniper; identifies semantic receipts and activity counters as acceptance "
        "gates; and marks analytic P-OPT timing as an optimistic bound because "
        "matrix-stream latency, queueing, and bandwidth are not target-time charged.",
        1120,
    )
    figure.section(
        "1", "SIMULATOR ROLES", "do not compare absolute rates or time across simulators",
        138, role="data",
    )
    figure.rect(40, 190, 1120, 260, role="neutral", radius=0)
    for x in (320, 600, 880):
        figure.line((x, 190), (x, 450), color=INK, width=1)
    figure.line((40, 240), (1160, 240), color=INK, width=1)
    for x, label, color in (
        (180, "gem5 O3", GREEN),
        (460, "cache_sim", BLUE),
        (740, "Sniper", PURPLE),
        (1020, "Claim output", RED),
    ):
        figure.text(x, 222, label, size=17, bold=True,
                    color=color, anchor="middle")
    simulator_columns = (
        ("architectural time", "exact Request binding", "native LSQ/MSHR/cache"),
        ("functional victim logic", "off-chip traffic", "no cycle/instruction model"),
        ("modeled cache direction", "exact indexed markers",
         "computed fused sideband", "is diagnostic",
         "inconsistent hints", "fail closed"),
        ("gem5: timing speedup", "cache_sim: traffic", "Sniper: corroboration"),
    )
    for col, values in enumerate(simulator_columns):
        figure.lines(56 + col * 280, 280, values, step=30, max_width=248)

    figure.section(
        "2", "ROW ACCEPTANCE", "a success-shaped fallback is not a valid experiment row",
        510, role="verify",
    )
    figure.rect(40, 565, 300, 170, role="state", radius=0)
    figure.text(190, 598, "Mechanism receipt vector", size=17,
                bold=True, color=PURPLE, anchor="middle")
    figure.lines(
        58, 632,
        (
            "requested == effective",
            "carrier + events positive",
            "width/substitution agree",
            "P-OPT matrix active if required",
        ),
        max_width=264,
    )
    figure.rect(420, 565, 300, 170, role="state", radius=0)
    figure.text(570, 598, "Semantic receipt vector", size=17,
                bold=True, color=PURPLE, anchor="middle")
    figure.lines(
        438, 632,
        (
            "kernel output / edge budget",
            "all matched policy rows agree",
            "failed peer invalidates timing",
            "not gated by speculation counts",
        ),
        max_width=264,
    )
    figure.diamond(825, 650, 170, 125, role="verify")
    figure.text(825, 637, "mechanism", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(825, 668, "AND semantic", size=16, anchor="middle")
    figure.rect(965, 595, 195, 110, role="compute", radius=0)
    figure.text(1062, 632, "accepted row", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(1062, 668, "eligible for claims", size=16,
                anchor="middle")
    figure.arrow(
        ((340, 650), (420, 650)),
        kind="control", label="Mechanism receipt vector",
        color=PURPLE,
    )
    figure.arrow(
        ((720, 650), (740, 650)),
        kind="control", label="Semantic receipt vector",
        color=PURPLE,
    )
    figure.arrow(
        ((910, 650), (965, 650)),
        kind="control", label="accepted row", color=GREEN,
    )

    figure.section(
        "3", "CLAIM LIMITS", "state what is omitted before interpreting a ratio",
        842, role="transfer",
    )
    figure.rect(40, 895, 1120, 165, role="neutral", radius=0)
    figure.line((600, 895), (600, 1060), color=INK, width=1)
    figure.text(56, 928, "Analytic P-OPT boundary", size=17,
                bold=True, color=AMBER)
    figure.text(56, 962, "charged: reserved ways + cumulative matrix bytes",
                size=16)
    figure.text(56, 992, "omitted: target-time latency / bandwidth / queueing",
                size=16)
    figure.text(56, 1022, "popt_target_time_charged = 0 -> optimistic timing bound",
                size=16, bold=True, mono=True, color=RED)
    figure.text(616, 928, "Publication vector", size=17,
                bold=True, color=RED)
    figure.text(616, 962, "time + total off-chip traffic + instructions",
                size=16)
    figure.text(616, 992, "same-build, same-cell baseline; geometric mean",
                size=16)
    figure.text(616, 1022, "exclude timing_valid_for_speedup = 0",
                size=16, mono=True)
    figure.arrow(
        ((120, 1095), (1080, 1095)),
        kind="dependency", label="publish only after the frozen campaign",
        color=RED, label_at=(600, 1080),
    )
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
        help="Generate in a private temporary directory and compare outputs.",
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
        with TemporaryDirectory(prefix="ecg-figure-check-") as temporary:
            temporary_root = Path(temporary)
            generated = generate(temporary_root)
            after = {
                path.relative_to(temporary_root): path.read_bytes()
                for pair in generated for path in pair
            }
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
