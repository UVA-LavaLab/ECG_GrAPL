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

    tracked_source_reader = int(raw["tracked_edge"]["source_reader"])
    tracked_source_dest = int(raw["tracked_edge"]["source_destination"])
    tracked_reader = source_to_internal[tracked_source_reader]
    tracked_dest = source_to_internal[tracked_source_dest]
    if tracked_dest not in rows[tracked_reader]:
        raise ValueError("tracked edge is absent from the checked reader graph")
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
        raise ValueError("tracked property line has no reader")
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
        "ECG Next: offline guidance to request-bound LLC state",
        "One reading spine separates graph preprocessing, runtime requests, cache policy, and evidence.",
        "Three numbered bands show ECG Next's offline graph analysis, the two-load "
        "RISC-V runtime path, line-local LLC replacement state, and the evidence "
        "boundary across gem5 O3, cache_sim, and Sniper. The tracked checked-fixture "
        f"edge is reader {fx.tracked_reader} to property {fx.tracked_dest}.",
        1160,
    )
    figure.section(
        "1", "OFFLINE GRAPH ANALYSIS", "immutable records; excluded from the ROI",
        138, role="data",
    )
    figure.card(
        24, 180, 360, 205, "Kernel-specific reader direction",
        (
            "PageRank: pull over in-neighbors",
            "BFS / BC / CC / SSSP: push over",
            "the implemented out-neighbor schedule",
            "runtime never infers reader order",
        ),
        role="data",
    )
    figure.card(
        420, 180, 360, 205, "Per-line ReusePlan",
        (
            "rank reader counts -> tier 1 / 2 / 3",
            "find two future readers after current",
            "merge vertices in one 64-byte line",
            "pack destination + tier + two epochs",
        ),
        role="state",
    )
    figure.card(
        816, 180, 360, 205, "Sealed runtime input",
        (
            "record order follows canonical CSR",
            "compact record substitutes for edge ID",
            "otherwise use edge plus sidecar",
            "guest validates graph + payload hashes",
        ),
        role="verify",
    )

    figure.section(
        "2", "RUNTIME REQUEST PATH", "two loads and one explicit register dependency",
        430, role="compute",
    )
    figure.card(
        24, 472, 270, 205, "Record request",
        (
            "ecg.plan.load*",
            "or ecg.flow.load*",
            "returns ReusePlan in rd",
            "FlowThrough marks request",
            "normal translation + lookup",
        ),
        role="transfer",
    )
    figure.card(
        318, 472, 270, 205, "ReusePlan operand",
        (
            "rename maps record rd",
            "property reads it as rs2",
            "issue waits for both sources",
            "no shared O3 mailbox",
        ),
        role="state",
    )
    figure.card(
        612, 472, 270, 205, "Property request",
        (
            "ecg.bind.load.*",
            "computed address",
            "ecg.bind.iload.*",
            "indexed address",
            "typed ReuseBind extension",
            "property: no FlowThrough",
        ),
        role="compute",
    )
    figure.card(
        906, 472, 270, 205, "LLC line update",
        (
            "valid hit/fill",
            "stamps tier + epochs",
            "line guard must match",
            "MSHR conflict rejects hint",
            "data completion is unchanged",
        ),
        role="verify",
    )

    figure.section(
        "3", "POLICY AND EVIDENCE", "mechanism state is not itself a speedup claim",
        722, role="verify",
    )
    figure.card(
        24, 764, 360, 205, "Default rrip_first victim rule",
        (
            "age until a way reaches max RRPV",
            "old structural line wins in that set",
            "else evict farthest stamped property",
            "unstamped property distance is zero",
        ),
        role="state",
    )
    figure.card(
        420, 764, 360, 205, "Placement is a separate decision",
        (
            "record FlowThrough: design mechanism",
            "structural FlowThrough fairness",
            "LLC hits and private fills stay normal",
            "mixed MSHR targets may still allocate",
        ),
        role="transfer",
    )
    figure.card(
        816, 764, 360, 205, "Claim boundary",
        (
            "gem5 O3: timing authority",
            "cache_sim: functional policy + traffic",
            "Sniper: modeled cache/traffic",
            "analytic P-OPT time is optimistic",
        ),
        role="verify",
    )
    figure.arrow(
        ((105, 1015), (405, 1015)),
        kind="transfer",
        label="immutable ReusePlan records",
        cadence="once before the ROI",
        color=AMBER,
        label_at=(255, 1000),
    )
    figure.arrow(
        ((455, 1080), (745, 1080)),
        kind="transfer",
        label="property request + ReuseBind",
        cadence="per governed load",
        color=PURPLE,
        label_at=(600, 1065),
    )
    figure.arrow(
        ((795, 1015), (1095, 1015)),
        kind="transfer",
        label="semantic receipt and counters",
        cadence="per experiment row",
        color=RED,
        label_at=(945, 1000),
    )
    save(figure, generated)


def offline_construction(
    fx: CheckedFixture, generated: list[tuple[Path, Path]]
) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "01", "offline-construction"),
        "From a checked reader graph to one edge-aligned ReusePlan",
        "The concrete values come from fig/ecg-figure-fixture.json and its executable test.",
        "Four numbered bands derive a ReusePlan for the checked-fixture edge from "
        f"source edge {fx.tracked_source_reader}->{fx.tracked_source_dest} "
        f"(internal {fx.tracked_reader}->{fx.tracked_dest}). The figure "
        "shows selected CSR rows, reader-count tiering, line-level future readers, "
        "compact packing, and the boundary between preprocessing and the measured ROI.",
        1725,
    )
    figure.section(
        "1", "CHECKED READER GRAPH", "nine nodes, seventeen edges; other vertex IDs are empty",
        138, role="data",
    )
    figure.rect(24, 180, 780, 390, role="neutral", stroke=INK, stroke_width=3)
    figure.text(42, 211, "Checked nine-node weighted graph",
                size=17, bold=True, color=BLUE, max_width=735)
    figure.text(42, 238, "source IDs 0..8; node color shows internal property tier",
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
    figure.text(52, 552, "green: tier 1", size=16, color=GREEN)
    figure.text(215, 552, "amber: tier 2", size=16, color=AMBER)
    figure.text(375, 552, "purple: tier 3", size=16, color=PURPLE)
    figure.text(555, 552, "red: tracked access 4 -> 7", size=16, color=RED)
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
        f"tracked source edge {fx.tracked_source_reader} -> "
        f"{fx.tracked_source_dest}",
        size=17, bold=True, color=RED, max_width=300,
    )
    figure.card(
        830, 180, 346, 390, "CSR crosswalk",
        (
            f"src 0 -> int 1",
            f"row = {list(fx.rows[1])}",
            f"src 2 -> int 6",
            f"row = {list(fx.rows[6])}",
            f"src 4 -> int 8",
            f"row = {list(fx.rows[8])}",
            f"src 7 -> int 18",
            f"row = {list(fx.rows[18])}",
            f"tracked 4->7 = int {fx.tracked_reader}->{fx.tracked_dest}",
            "PageRank uses incoming CSR.",
            "frontier paths use outgoing CSR",
        ),
        role="data",
        mono_body=True,
    )

    figure.section(
        "2", "READER COUNTS TO REUSE TIER", "stable rank: count descending, vertex ID ascending",
        615, role="state",
    )
    counts = (
        f"src2/int6: {fx.reader_counts[6]} readers -> tier {fx.tiers[6]}",
        f"src4/int8: {fx.reader_counts[8]} readers -> tier {fx.tiers[8]}",
        f"src5/int11: {fx.reader_counts[11]} readers -> tier {fx.tiers[11]}",
        f"src7/int18: {fx.reader_counts[18]} readers -> tier {fx.tiers[18]}",
        f"src8/int20: {fx.reader_counts[20]} readers -> tier {fx.tiers[20]}",
        f"src0/int1: {fx.reader_counts[1]} readers -> tier {fx.tiers[1]}",
    )
    tier_name = {1: "hot", 2: "moderate", 3: "cold"}[fx.line_tier]
    figure.card(
        24, 657, 550, 245, "Default hot fraction = 0.15",
        counts,
        role="state",
        mono_body=True,
    )
    figure.card(
        600, 657, 576, 245, "Cache-line aggregation",
        (
            "4-byte properties -> 16 vertices per 64-byte line",
            f"property {fx.tracked_dest} is in line vertices "
            f"{fx.line_begin}..{fx.line_end - 1}",
            "line tier is the hottest tier among those vertices",
            f"tracked line tier = {fx.line_tier} ({tier_name})",
            "tier 0 is reserved for invalid metadata",
        ),
        role="neutral",
    )

    figure.section(
        "3", "TWO FUTURE LINE READERS", "reader CSR is searched strictly after the current reader",
        947, role="compute",
    )
    figure.card(
        24, 989, 550, 245, "Tracked line reader order",
        (
            f"line {fx.line_begin}..{fx.line_end - 1} readers = "
            f"{list(fx.line_reader_ids)}",
            f"current reader = {fx.tracked_reader}",
            f"first future reader = {fx.first_reader} -> epoch {fx.first_epoch}",
            f"second future reader = {fx.second_reader} -> epoch {fx.second_epoch}",
            "search wraps into the next traversal if needed",
        ),
        role="compute",
        mono_body=True,
    )
    figure.card(
        600, 989, 576, 245, "Quantization and same-reader correction",
        (
            "epoch = floor(reader * epoch_count / vertex_count)",
            f"here epoch_count = vertex_count = {fx.epoch_count}",
            "same-source accesses to the same line are preserved",
            "the pair describes the line, not only one vertex",
            "malformed epochs are clamped before distance use",
        ),
        role="neutral",
    )

    figure.section(
        "4", "PACK, SEAL, THEN STREAM", "preprocessing produces an immutable runtime input",
        1279, role="transfer",
    )
    figure.card(
        24, 1321, 550, 245, "Checked logical record",
        (
            f"destination = {fx.tracked_dest}",
            f"tier = {fx.line_tier}",
            f"epoch1 = {fx.first_epoch}",
            f"epoch2 = {fx.second_epoch}",
            f"compact bits = {fx.id_bits} + 2 + 2*{fx.epoch_bits} = "
            f"{fx.id_bits + 2 + 2 * fx.epoch_bits}",
        ),
        role="transfer",
        mono_body=True,
    )
    figure.card(
        600, 1321, 576, 245, "Offline / runtime boundary",
        (
            "builder output is outside the measured ROI",
            "sidecar header binds graph, configuration, and payload",
            "guest verifies offsets, hashes, width, and record count",
            "runtime streams the validated record in edge order",
            "no detailed simulator recomputes the graph pass",
        ),
        role="verify",
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
        "ReusePlan wire formats and honest traffic footprints",
        "General, compact, and weighted layouts are separate transport choices.",
        "The figure gives the exact unweighted 64-bit layout, the graph-dependent "
        "32-bit compact rule instantiated by the checked fixture, and both weighted "
        "SSSP transports: one compact 64-bit edge record or an ordinary weighted edge "
        "plus a 32-bit metadata sidecar.",
        1150,
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
    figure.card(
        24, 447, 550, 210, "Packing condition",
        (
            "id_bits + 2 tier bits + 2*epoch_bits <= 32",
            f"checked fixture: {fx.id_bits} + 2 + 2*{fx.epoch_bits} = "
            f"{fx.id_bits + 2 + 2 * fx.epoch_bits}",
            "unused high bits are zero/reserved",
            "decode widens to the canonical 64-bit value",
        ),
        role="transfer",
        mono_body=True,
    )
    figure.card(
        600, 447, 576, 210, "Traffic meaning",
        (
            "compact record substitutes for one ordinary edge ID",
            "the simulated stream is 4 bytes per edge",
            "if fields do not fit, use the wide record or sidecar",
            "a width receipt must match the materialized container",
        ),
        role="verify",
    )

    figure.section(
        "3", "WEIGHTED SSSP TRANSPORTS", "weight bytes cannot disappear from the comparison",
        702, role="data",
    )
    figure.card(
        24, 744, 550, 285, "Compact weighted 64-bit substitute",
        (
            "destination: 24 bits",
            "positive weight: 8 bits",
            "tier: 2 bits",
            "epoch 1 + epoch 2: 15 bits each",
            "valid only for < 2^24 vertices and weight <= 255",
            "one 8-byte record replaces the weighted edge",
        ),
        role="data",
        mono_body=True,
    )
    figure.card(
        600, 744, 576, 285, "General weighted edge + sidecar",
        (
            "ordinary weighted edge remains in the structural stream",
            "parallel 32-bit sidecar stores tier + two 15-bit epochs",
            "combined logical value still uses canonical ReusePlan fields",
            "simulated footprint is edge bytes plus 4 sidecar bytes",
            "FlowThrough roles for edge and sidecar are accounted separately",
        ),
        role="state",
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
        "Checked cache-line reuse timeline and circular distance",
        f"The tracked property line is revisited at internal readers "
        f"{fx.first_reader} and {fx.second_reader} after {fx.tracked_reader}.",
        f"A horizontal timeline follows the checked property line "
        f"0x{fx.property_line:08X} from current reader {current} to future "
        f"readers {fx.first_reader} and {fx.second_reader}. The ReusePlan stores "
        "the first two quantized epochs, and rrip_first consults their nearer "
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
        f"line 0x{fx.property_line:08X}: readers {list(fx.line_reader_ids)}",
        size=17, bold=True, color=PURPLE, max_width=520,
    )
    figure.text(
        1115, 235,
        f"source {fx.tracked_source_reader}->{fx.tracked_source_dest} / "
        f"internal {fx.tracked_reader}->{fx.tracked_dest}: "
        f"e1={fx.first_epoch}, e2={fx.second_epoch}",
        size=17, bold=True, color=AMBER, anchor="end", max_width=520,
    )
    figure.text(260, 450, "current reader", size=16, color=AMBER)
    figure.text(505, 450, "first future line use", size=16, color=GREEN)
    figure.text(860, 450, "second future line use", size=16, color=PURPLE)
    figure.text(
        600, 492,
        "the two-epoch record captures both future uses of this checked property line",
        size=16, color=GRAY, anchor="middle", max_width=900,
    )
    figure.section(
        "2", "CIRCULAR DISTANCE AT THE LLC", "the current victim-time epoch can advance after fill",
        565, role="compute",
    )
    figure.card(
        24, 607, 550, 205, "Checked arithmetic at current epoch 6",
        (
            f"d1 = ({fx.first_epoch} + {epoch_count} - {current}) mod "
            f"{epoch_count} = {first_distance}",
            f"d2 = ({fx.second_epoch} + {epoch_count} - {current}) mod "
            f"{epoch_count} = {second_distance}",
            f"nearest = min({first_distance}, {second_distance}) = "
            f"{min(first_distance, second_distance)}",
            "payload count 2 activates both epochs",
        ),
        role="compute",
        mono_body=True,
    )
    figure.card(
        600, 607, 576, 205, "Line-state interpretation",
        (
            "epochs remain absolute in the cache line",
            "victim-time current epoch may be later than 6",
            "unstamped property has effective distance zero",
            "malformed epochs are clamped before use",
        ),
        role="state",
    )
    figure.section(
        "3", "RRIP-FIRST DECISION", "timeline ranks properties only after RRIP eligibility",
        857, role="verify",
    )
    figure.card(
        24, 899, 550, 170, "Eligible structural line exists",
        (
            "select the oldest max-RRPV structural line",
            "property future distance is not consulted",
            "this keeps stream pollution separate from property ranking",
        ),
        role="transfer",
    )
    figure.card(
        600, 899, 576, 170, "Only eligible property lines remain",
        (
            f"tracked line contributes nearest distance "
            f"{min(first_distance, second_distance)}",
            "select the farthest effective stamped distance",
            "stable set order resolves an exact remaining tie",
        ),
        role="verify",
    )
    save(figure, generated)


def llc_policy(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "04", "llc-policy-pipeline"),
        "LLC metadata lifecycle and rrip_first victim pipeline",
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
    figure.card(
        24, 180, 550, 225, "Accepted ReuseBind",
        (
            "typed Request extension has two epochs",
            "context ID is nonzero and not conflicted",
            "destination maps to the accessed property line",
            "hit or fill may refresh line metadata",
        ),
        role="compute",
    )
    figure.card(
        600, 180, 576, 225, "Rejected metadata",
        (
            "mixed governed / ungoverned MSHR targets",
            "requestor or context disagreement",
            "equal-sequence payload disagreement",
            "destination-line mismatch or invalid count",
        ),
        role="verify",
    )

    figure.section(
        "2", "LINE-LOCAL STATE", "data value and replacement metadata have different lifetimes",
        450, role="state",
    )
    figure.card(
        24, 492, 550, 225, "Governed property line",
        (
            "valid | property | RRPV | recency",
            "tier | epoch1 | epoch2 | epoch_count",
            "context ID and live-stamp validity",
            "property bytes remain architecturally unchanged",
        ),
        role="state",
        mono_body=True,
    )
    figure.card(
        600, 492, 576, 225, "Refresh and invalidation",
        (
            "accepted hit/fill updates tier and epochs",
            "ordinary hit still updates native recency/RRIP state",
            "conflicted metadata does not stamp the line",
            "invalidation clears tier, epochs, context, and validity",
        ),
        role="neutral",
    )

    figure.section(
        "3", "RRIP ELIGIBILITY", "the default variant ages until a way reaches rrpvMax",
        762, role="compute",
    )
    figure.card(
        24, 804, 360, 225, "A. Form candidate set",
        (
            "keep only ways with RRPV >= rrpvMax",
            "if empty: increment lower RRPVs",
            "repeat until a candidate exists",
            "recency remains an independent field",
        ),
        role="compute",
    )
    figure.card(
        420, 804, 360, 225, "B. Prefer structural candidate",
        (
            "among eligible non-property ways",
            "select oldest by normalized recency",
            "records and CSR are structural",
            "property epochs do not rank these ways",
        ),
        role="transfer",
    )
    figure.card(
        816, 804, 360, 225, "C. Rank property candidates",
        (
            "effective distance = zero if unstamped",
            "else min(d(epoch1), d(epoch2))",
            "select the farthest effective distance",
            "set order resolves an exact tie",
        ),
        role="state",
    )

    figure.section(
        "4", "VARIANTS ARE CONTROLLED ABLATIONS", "do not blend their ordering into the primary claim",
        1074, role="verify",
    )
    figure.card(
        24, 1116, 550, 185, "Primary and neutral controls",
        (
            "rrip_first: eligibility -> structural -> farthest epoch",
            "grasp_only: pure RRIP",
            "rrip_no_epoch: same gate, fixed property tie",
            "rrip_no_epoch_recency: same gate, property LRU",
        ),
        role="verify",
    )
    figure.card(
        600, 1116, 576, 185, "Other experiment variants",
        (
            "epoch_first | degree_first | shortcircuit | lru_only",
            "future_tier_first combines future, tier, then recency",
            "online selectors are failed diagnostics, not primary policy",
            "admission diagnostics are separate from victim selection",
        ),
        role="neutral",
    )
    save(figure, generated)


def flowthrough_outcomes(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "05", "flowthrough-outcomes"),
        "FlowThrough changes LLC allocation, not lookup or service",
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
    for x, title, body, role in (
        (24, "Load queue", ("ordering + replay", "size and request role", "FlowThrough flag"), "state"),
        (318, "D-TLB + L1D", ("normal translation", "private hit is normal", "private fill is normal"), "data"),
        (612, "L2 + LLC tags", ("normal lookup", "LLC hit returns normally", "miss allocates an MSHR"), "compute"),
        (906, "Memory response", ("ordinary line fetch", "ordinary response path", "destination writes back"), "verify"),
    ):
        figure.card(x, 180, 270, 205, title, body, role=role)

    figure.section(
        "2", "THREE LLC OUTCOMES", "only the returning miss fill reaches the allocation gate",
        430, role="transfer",
    )
    figure.card(
        24, 472, 360, 245, "LLC hit",
        (
            "existing line supplies the record",
            "no memory request",
            "no fill allocation decision",
            "FlowThrough does not bypass the hit",
        ),
        role="compute",
    )
    figure.card(
        420, 472, 360, 245, "LLC miss: all targets no-allocate",
        (
            "memory fetch completes normally",
            "private caches receive the line",
            "MSHR allocOnFill remains false",
            "returning line is not inserted in LLC",
        ),
        role="transfer",
    )
    figure.card(
        816, 472, 360, 245, "LLC miss: mixed MSHR targets",
        (
            "one target requires allocation",
            "allocOnFill combines with OR",
            "the shared returning line is allocated",
            "no target suppresses a required fill",
        ),
        role="verify",
    )

    figure.section(
        "3", "DERIVED PREFETCHES AND PROPERTY LOADS", "classification remains target-range exact",
        762, role="state",
    )
    figure.card(
        24, 804, 550, 245, "Derived structural prefetch",
        (
            "candidate address is checked against active carrier range",
            "in-range prefetch receives STRUCTURAL_FLOWTHROUGH",
            "out-of-range candidate does not inherit the bit",
            "translated prefetch Request carries the exact result",
        ),
        role="state",
    )
    figure.card(
        600, 804, 576, 245, "Governed property request",
        (
            "ReuseBind extension is independent of FlowThrough",
            "property line must remain allocatable",
            "LLC hit/fill may update tier and epochs",
            "property data completes normally",
        ),
        role="compute",
    )
    figure.arrow(
        ((145, 1130), (1055, 1130)),
        kind="control",
        label="suppress LLC insertion only when every coalesced target permits it",
        color=AMBER,
        label_at=(600, 1115),
    )
    save(figure, generated)


def structural_fairness(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("reuse-plan-flowthrough", "06", "structural-fairness"),
        "Design FlowThrough and symmetric structural fairness",
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
    figure.card(
        24, 180, 550, 245, "ReusePlan record request",
        (
            "record can be compact, wide, or weighted sidecar",
            "request-specific flag is attached by the ISA path",
            "optional adaptive placement applies only to this stream",
            "purpose: avoid retaining one-touch metadata records",
        ),
        role="state",
    )
    figure.card(
        600, 180, 576, 245, "What it does not grant",
        (
            "it does not bypass the governed property request",
            "it does not remove record bytes or memory latency",
            "it does not make P-OPT matrix traffic disappear",
            "it is not evidence that the victim rule is better",
        ),
        role="verify",
    )

    figure.section(
        "2", "SYMMETRIC FAIRNESS CONTROL", "--flowthrough all is policy-independent",
        470, role="transfer",
    )
    figure.card(
        24, 512, 360, 245, "Baseline policies",
        (
            "LRU / GRASP / P-OPT consume CSR",
            "active carrier range = real edge array",
            "STRUCTURAL_FLOWTHROUGH no-allocate",
            "positive activity receipt is required",
        ),
        role="data",
    )
    figure.card(
        420, 512, 360, 245, "Packed-substitute ReusePlan",
        (
            "runtime uses record, not edge ID",
            "active carrier range = record array",
            "fallback paths publish CSR instead",
            "carrier selection is fail-closed",
        ),
        role="state",
    )
    figure.card(
        816, 512, 360, 245, "Backend evidence",
        (
            "cache_sim: structural access count",
            "gem5: no-allocate miss targets",
            "Sniper: structural read/fill counts",
            "translated Sniper mode is rejected",
        ),
        role="verify",
    )
    figure.arrow(
        ((110, 845), (1090, 845)),
        kind="dependency",
        label="compare policies only after active structural carriers are matched",
        color=RED,
        label_at=(600, 830),
    )
    save(figure, generated)


def instruction_family(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "01", "instruction-family"),
        "Experimental RISC-V instruction roles and operand contracts",
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
    figure.card(
        24, 180, 360, 205, "Record-format CSR",
        ("id bits", "epoch bits", "compact decode widths", "configured before ROI"),
        role="state",
        mono_body=True,
    )
    figure.card(
        420, 180, 360, 205, "Current-epoch CSR",
        ("quantized traversal position", "updated at epoch boundaries", "reused inside one epoch", "carried on property Request"),
        role="compute",
    )
    figure.card(
        816, 180, 360, 205, "Context CSR",
        ("nonzero execution identity", "separates overlapping contexts", "required for valid ReuseBind", "checked during MSHR merge"),
        role="verify",
    )

    figure.section(
        "2", "RECORD-LOAD FAMILY", "rs1 is a record address; result depends on the form",
        430, role="transfer",
    )
    figure.card(
        24, 472, 550, 205, "ecg.plan.load*",
        (
            "ordinary cacheable placement",
            "general: rd = canonical 64-bit plan",
            "weighted: rs2 = destination",
            "weighted: rd = 32-bit sidecar",
            "no compact Plan-load encoding",
        ),
        role="data",
        mono_body=True,
    )
    figure.card(
        600, 472, 576, 205, "ecg.flow.load*",
        (
            "sets ECG_FLOWTHROUGH on record Request",
            "general unweighted: canonical plan in rd",
            "compact unweighted: 4-byte load, widened",
            "weighted: rd = 32-bit sidecar",
            "compact unweighted is FlowThrough-only",
        ),
        role="transfer",
        mono_body=True,
    )

    figure.section(
        "3", "PROPERTY-LOAD FAMILY", "the ReusePlan result is an explicit source operand",
        722, role="compute",
    )
    figure.card(
        24, 764, 550, 225, "ecg.bind.load.*",
        ("rs1 = software-computed property address", "rs2 = canonical ReusePlan", "result = typed U32 / S32 / U64 / F32 value", "one ordinary property memory request"),
        role="compute",
        mono_body=True,
    )
    figure.card(
        600, 764, 576, 225, "ecg.bind.iload.*",
        ("rs1 = property-array base", "rs2 carries destination + ReusePlan", "EA = base + destination * element size", "indexed address generation and binding are fused"),
        role="compute",
        mono_body=True,
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
        "A real ReusePlan instruction pair in the gem5 O3 pipeline",
        f"Source edge {fx.tracked_source_reader}->{fx.tracked_source_dest} "
        f"maps to internal {fx.tracked_reader}->{fx.tracked_dest}, property "
        f"0x{fx.property_address:08X}, and LLC line 0x{fx.property_line:08X}.",
        "An architecture-style schematic follows the checked compact record "
        "through the actual record instruction, renamed register dependency, "
        "property instruction, frontend and execution stages, LSQ Request "
        "extension, private caches, MSHR, LLC line state, writeback, and ROB "
        "retirement.",
        1490,
    )
    figure.section(
        "1", "RECORD LOAD + DEPENDENT PROPERTY INSTRUCTION", "record result is a true rs2 dependency",
        138, role="transfer",
    )
    figure.card(
        24, 180, 520, 180, "ecg.flow.load.compact",
        (
            f"rs1 = record for source {fx.tracked_source_reader}->"
            f"{fx.tracked_source_dest} / int {fx.tracked_reader}->"
            f"{fx.tracked_dest}",
            "memory width = 4 bytes",
            "Request flag = ECG_FLOWTHROUGH",
            f"rd = dest {fx.tracked_dest} | T{fx.line_tier} | "
            f"e1 {fx.first_epoch} | e2 {fx.second_epoch}",
            "execute reads format CSR and widens the result",
        ),
        role="transfer",
        mono_body=True,
    )
    figure.card(
        656, 180, 520, 180, "ecg.bind.load.f32",
        (
            f"rs1 = computed property address 0x{fx.property_address:08X}",
            "rs2 = renamed ReusePlan physical register",
            "result = ordinary binary32 property value",
            "property Request carries ReuseBind",
            "never FlowThrough",
        ),
        role="compute",
        mono_body=True,
    )
    figure.arrow(
        ((284, 360), (284, 405), (916, 405), (916, 360)),
        kind="dependency",
        label="canonical ReusePlan register",
        color=PURPLE,
        label_at=(600, 392),
    )

    figure.section(
        "2-4", "GEM5 O3 PIPELINE", "Fetch | Decode | Rename | IEW | Commit",
        450, role="compute",
    )
    figure.arrow(
        ((105, 650), (1090, 650)),
        kind="control",
        label="dynamic instruction state",
        color=GREEN,
        width=2.5,
    )
    figure.rect(40, 560, 140, 150, role="compute", stroke=INK, stroke_width=2)
    figure.text(110, 595, "Fetch", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(110, 630, "DynInst", size=16, anchor="middle")
    figure.text(110, 665, "branch state", size=16, anchor="middle")

    figure.rect(220, 560, 140, 150, role="compute", stroke=INK, stroke_width=2)
    figure.text(290, 595, "Decode", size=17, bold=True,
                color=GREEN, anchor="middle")
    figure.text(290, 630, "custom-0 role", size=16, anchor="middle")
    figure.text(290, 665, "width + dst", size=16, anchor="middle")

    figure.table(400, 540, 170, 190, 3, role="state")
    figure.text(485, 575, "Rename", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(485, 632, "2 plan dependency", size=16, anchor="middle")
    figure.text(485, 695, "map + free list", size=16, anchor="middle")

    figure.rect(610, 520, 360, 245, role="neutral", stroke=GREEN, stroke_width=3)
    figure.text(790, 548, "IEW: issue / execute / writeback", size=17,
                bold=True, color=GREEN, anchor="middle")
    figure.queue(625, 568, 125, 78, role="state")
    figure.text(688, 595, "Issue queue", size=16, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(688, 625, "rs1 + rs2 ready", size=16, anchor="middle")
    figure.table(770, 568, 90, 78, 2, role="data")
    figure.text(815, 595, "Phys regs", size=16, bold=True,
                color=BLUE, anchor="middle")
    figure.text(815, 625, "read", size=16, anchor="middle")
    figure.diamond(915, 607, 95, 78, role="compute")
    figure.text(915, 602, "3 AGU", size=16, bold=True,
                color=GREEN, anchor="middle")
    figure.text(915, 627, "EA", size=16, anchor="middle")
    figure.table(650, 674, 285, 70, 2, role="state")
    figure.text(792, 702, "4 LSQ Request", size=17, bold=True,
                color=PURPLE, anchor="middle")
    figure.text(792, 734, "ordering | replay | ReuseBind extension", size=16,
                anchor="middle")

    figure.table(1010, 540, 150, 190, 3, role="verify")
    figure.text(1085, 575, "Commit", size=17, bold=True,
                color=RED, anchor="middle")
    figure.text(1085, 632, "ROB head", size=16, anchor="middle")
    figure.text(1085, 695, "fault / redirect", size=16, anchor="middle")
    figure.text(
        600, 802, "dynamic instruction state",
        size=16, bold=True, color=GREEN, anchor="middle",
    )
    figure.text(
        600, 832,
        f"LSQ extension: dest={fx.tracked_dest} tier={fx.line_tier} "
        f"epochs=({fx.first_epoch},{fx.second_epoch}) current={fx.tracked_reader} "
        "context=k sequence=s",
        size=16, mono=True, color=PURPLE, anchor="middle", max_width=1080,
    )

    figure.section(
        "5", "CACHE AND MISS PATH", "the same property Request crosses every cache boundary",
        880, role="data",
    )
    for start, end in (
        ((270, 1075), (330, 1075)),
        ((560, 1075), (620, 1075)),
        ((850, 1075), (930, 1075)),
    ):
        figure.arrow(
            (start, end),
            kind="transfer",
            label="property Request + ReuseBind",
            cadence="per governed load",
            color=BLUE,
            width=2.5,
        )
    figure.table(40, 950, 230, 215, 4, role="data")
    figure.text(56, 980, "Private hierarchy", size=17, bold=True, color=BLUE)
    figure.text(56, 1035, "D-TLB: translation", size=16)
    figure.text(56, 1088, "L1D: normal hit/fill", size=16)
    figure.text(56, 1142, "L2: extension preserved", size=16)

    figure.table(330, 950, 230, 215, 4, role="state")
    figure.text(346, 980, "MSHR target table", size=17, bold=True, color=PURPLE)
    figure.text(346, 1020, "same property block", size=16)
    figure.text(346, 1075, "newest compatible seq", size=16)
    figure.text(346, 1128, "conflict propagation", size=16)

    figure.table(620, 950, 230, 215, 4, role="state")
    figure.text(636, 980, "LLC set + metadata", size=17, bold=True, color=PURPLE)
    figure.text(636, 1020, f"line 0x{fx.property_line:08X}", size=16, mono=True)
    figure.text(636, 1075, "destination-line guard", size=16)
    figure.text(636, 1128, "stamp tier + epochs", size=16)

    figure.cylinder(930, 950, 190, 215, role="data")
    figure.text(1025, 995, "Memory", size=17, bold=True,
                color=BLUE, anchor="middle")
    figure.text(1025, 1045, "ordinary line fetch", size=16, anchor="middle")
    figure.text(1025, 1095, "allocOnFill applies", size=16, anchor="middle")
    figure.text(1025, 1140, "normal response", size=16, anchor="middle")
    figure.text(
        600, 1203, "property Request + ReuseBind | per governed load",
        size=16, bold=True, color=BLUE, anchor="middle",
    )

    figure.section(
        "6", "COMPLETION AND RETIREMENT", "metadata is advisory; data uses normal O3 completion",
        1250, role="verify",
    )
    figure.table(100, 1292, 470, 145, 3, role="data")
    figure.text(116, 1322, "Writeback bus + physical register", size=17,
                bold=True, color=BLUE)
    figure.text(116, 1365, "binary32 value -> floating destination", size=16)
    figure.text(116, 1412, "load queue marks the dynamic load complete", size=16)
    figure.table(630, 1292, 470, 145, 3, role="verify")
    figure.text(646, 1322, "ROB commit", size=17, bold=True, color=RED)
    figure.text(646, 1365, "precise in-order retirement", size=16)
    figure.text(646, 1412, "squash/replay stays native gem5", size=16)
    save(figure, generated)


def mshr_lifecycle(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("risc-v-instruction-path", "03", "mshr-metadata-lifecycle"),
        "ReuseBind across MSHR merge, fill, and invalidation",
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
    figure.card(
        100, 180, 1000, 185, "EcgReusePlanExtension",
        (
            "destination | tier | epoch1 | epoch2 | epoch_count",
            "current_epoch | context_id | dynamic sequence | conflicted",
            "valid context requires nonzero context ID and no conflict",
            "the extension clones with the gem5 Request",
        ),
        role="state",
        mono_body=True,
    )

    figure.section(
        "2", "MSHR TARGET-LIST MERGE", "rebuild state whenever active targets change",
        410, role="compute",
    )
    figure.card(
        24, 452, 360, 245, "Compatible governed targets",
        (
            "same requestor",
            "same nonzero context",
            "newer sequence replaces payload",
            "equal sequence: payload must match",
        ),
        role="compute",
    )
    figure.card(
        420, 452, 360, 245, "Conflict cases",
        (
            "governed mixed with ungoverned target",
            "requestor or context mismatch",
            "invalid context",
            "equal sequence with different payload",
        ),
        role="verify",
    )
    figure.card(
        816, 452, 360, 245, "Independent allocation state",
        (
            "each target carries allocOnFill",
            "FlowThrough target contributes false",
            "ordinary target contributes true",
            "TargetList combines them with OR",
        ),
        role="transfer",
    )

    figure.section(
        "3", "RESPONSE AND LLC ACCEPTANCE", "conflict never becomes a valid line stamp",
        742, role="verify",
    )
    figure.card(
        24, 784, 550, 245, "Downstream response",
        (
            "selected newest extension is copied to response Request",
            "conflict marker propagates if any merge rule failed",
            "serviceable/deferred target changes rebuild merge state",
            "deallocation resets MSHR metadata state",
        ),
        role="state",
    )
    figure.card(
        600, 784, 576, 245, "LLC hit or fill",
        (
            "reject conflicted extension",
            "map destination through property region and element size",
            "accept only when destination line equals accessed line",
            "store tier, epochs, context, and live-stamp validity",
        ),
        role="verify",
    )

    figure.section(
        "4", "LINE LIFETIME", "metadata is advisory; data correctness stays architectural",
        1074, role="neutral",
    )
    figure.card(
        100, 1116, 1000, 170, "After acceptance",
        (
            "a later governed hit may refresh the metadata",
            "ordinary recency/RRIP state continues to evolve",
            "invalidation clears every ECG field and stamp-valid bit",
            "eviction discards metadata with the line; property bytes are never modified",
        ),
        role="neutral",
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
        "Checked request: source edge 4 -> 7 maps to property 18",
        "Every number is derived from fig/ecg-figure-fixture.json.",
        "A single checked-fixture edge is followed from its edge-aligned compact "
        "record through the record load, explicit register dependency, computed "
        "property address, typed Request extension, LLC line stamp, circular "
        "distance, normal data completion, and later victim selection.",
        1610,
    )
    figure.section(
        "1", "TRACKED GRAPH EDGE AND RECORD", "one concrete checked edge; not a measured workload",
        138, role="data",
    )
    figure.rect(24, 180, 500, 280, role="neutral", stroke=INK, stroke_width=3)
    figure.text(42, 211, "Source readers of vertex 7", size=17, bold=True,
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
        f"track {fx.tracked_source_reader} -> "
        f"{fx.tracked_source_dest}",
                size=17, bold=True, color=RED, max_width=310)

    figure.rect(550, 180, 626, 280, role="transfer", stroke=INK, stroke_width=3)
    figure.text(570, 211, "B  edge-aligned compact ReusePlan",
                size=17, bold=True, color=AMBER, max_width=560)
    figure.lines(
        570, 244,
        (
            f"source {fx.tracked_source_reader} -> "
            f"internal reader {fx.tracked_reader}",
            f"CSR row contains destination {fx.tracked_dest}",
            f"line tier = {fx.line_tier}; future readers = "
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
        "2", "REAL TWO-INSTRUCTION PIPELINE", "the compact record becomes an explicit rs2 operand",
        505, role="compute",
    )
    pipeline_x = (40, 330, 620, 910)
    pipeline_cards = (
        ("Record load", ("ecg.flow.load", ".compact", f"record[int {fx.tracked_reader}->{fx.tracked_dest}]", "rd = canonical plan"), "transfer"),
        ("Rename + issue", ("rd -> physical reg", "property reads rs2", "wait for rs1 + rs2"), "state"),
        ("ecg.bind.load.f32", (f"rs1 = 0x{fx.property_address:08X}", "rs2 = ReusePlan", "binary32 result"), "compute"),
        ("LSQ Request", (f"dest={fx.tracked_dest} tier={fx.line_tier}", f"epochs={fx.first_epoch},{fx.second_epoch}", f"current={current} context=k", "sequence=s"), "state"),
    )
    for x1, x2 in zip(pipeline_x, pipeline_x[1:]):
        figure.arrow(
            ((x1 + 230, 675), (x2, 675)),
            kind="dependency",
            label="instruction operand / Request state",
            color=PURPLE,
            width=2.5,
        )
    for x, (title, body, role) in zip(pipeline_x, pipeline_cards):
        figure.card(x, 565, 230, 220, title, body, role=role, mono_body=True)
    figure.text(
        600, 825, "C  instruction operand / Request state",
        size=16, bold=True, color=PURPLE, anchor="middle",
    )

    figure.section(
        "3", "TWO REQUEST LANES THROUGH THE CACHE", "record placement and property metadata remain distinct",
        870, role="data",
    )
    lane_x = (40, 320, 600, 880)
    for x1, x2 in zip(lane_x, lane_x[1:]):
        figure.arrow(
            ((x1 + 204 if x1 < 880 else x1 + 260, 982), (x2, 982)),
            kind="transfer",
            label="record Request",
            cadence="per tracked edge",
            color=AMBER,
            width=2.5,
        )
    record_lane = (
        ("Record Request", ("ECG_FLOWTHROUGH", "record address"), "transfer", 204),
        ("Private caches", ("normal lookup", "private fill"), "data", 204),
        ("Record-block MSHR", ("same block only", "allocOnFill=false"), "transfer", 204),
        ("LLC record outcome", ("hit returns normally", "miss may skip LLC fill"), "verify", 260),
    )
    for x, (title, body, role, width) in zip(lane_x, record_lane):
        figure.card(x, 930, width, 115, title, body, role=role, mono_body=True)
    for x1, x2 in zip(lane_x, lane_x[1:]):
        figure.arrow(
            ((x1 + 204 if x1 < 880 else x1 + 260, 1127), (x2, 1127)),
            kind="transfer",
            label="property Request + ReuseBind",
            cadence="per tracked edge",
            color=BLUE,
            width=2.5,
        )
    property_lane = (
        ("Property Request", ("property address", "ReuseBind ext", "allocOnFill=true"), "state", 204),
        ("Private caches", ("normal lookup", "private fill"), "data", 204),
        ("Property MSHR", ("same block only", "ReuseBind merge"), "state", 204),
        (
            "LLC property outcome",
            (
                "destination-line guard",
                f"stamp T{fx.line_tier} / e{fx.first_epoch} / "
                f"e{fx.second_epoch}",
            ),
            "verify",
            260,
        ),
    )
    for x, (title, body, role, width) in zip(lane_x, property_lane):
        figure.card(x, 1075, width, 115, title, body, role=role, mono_body=True)
    figure.text(
        600, 1208,
        "D  record Request | property Request + ReuseBind | per tracked edge",
        size=16, bold=True, color=BLUE, anchor="middle", max_width=850,
    )

    figure.section(
        "4", "LINE STAMP, REUSE TIMELINE, AND LATER VICTIM", "the property value is already complete",
        1235, role="state",
    )
    figure.rect(24, 1277, 720, 250, role="state", stroke=INK, stroke_width=2)
    figure.text(44, 1308, f"E  LLC line 0x{fx.property_line:08X}",
                size=17, bold=True, color=PURPLE, max_width=420)
    axis_y = 1415
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
        384, 1485,
        f"nearest = min({d1}, {d2}) = {min(d1, d2)}; stored epochs remain absolute",
        size=16, mono=True, color=PURPLE, anchor="middle", max_width=650,
    )
    figure.card(
        770, 1277, 406, 250, "Later rrip_first decision",
        (
            "line must first be max-RRPV eligible",
            "eligible structural line wins first",
            "otherwise farthest stamped property wins",
            "ReusePlan never changes the property value",
            "this checked path is not a speedup claim",
        ),
        role="verify",
    )
    save(figure, generated)


def architecture_state_map(generated: list[tuple[Path, Path]]) -> None:
    figure = Figure(
        ROOT,
        FigureTarget("property-to-cache-walkthrough", "02", "architecture-state-map"),
        "Where ECG state lives in the processor and cache hierarchy",
        "Containment shows storage; arrows are reserved for real operands and requests.",
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
    figure.card(
        24, 180, 550, 225, "Memory before the ROI",
        (
            "CSR or weighted structural arrays",
            "edge-aligned ReusePlan records / sidecars",
            "property arrays",
            "sealed graph and payload identity",
        ),
        role="data",
    )
    figure.card(
        600, 180, 576, 225, "Architectural ECG CSRs",
        (
            "record format: id bits + epoch bits",
            "current epoch: quantized traversal position",
            "context: nonzero execution identity",
            "updated by software at controlled boundaries",
        ),
        role="state",
    )

    figure.section(
        "2", "OUT-OF-ORDER CORE", "standard O3 structures carry one explicit dependency",
        450, role="compute",
    )
    figure.card(
        24, 492, 270, 245, "Frontend + rename",
        ("custom-0 decode", "ROB allocation", "physical rd mapping", "property rs2 mapping"),
        role="compute",
    )
    figure.card(
        318, 492, 270, 245, "Issue queue",
        ("record waits for address", "property waits for rs1 + rs2", "normal wakeup", "normal speculation"),
        role="compute",
    )
    figure.card(
        612, 492, 270, 245, "AGU + load queue",
        ("record EA or property EA", "ordering + replay", "request role + size", "FlowThrough only on record"),
        role="state",
    )
    figure.card(
        906, 492, 270, 245, "Request object",
        ("ordinary address fields", "optional ReuseBind extension", "destination + tier + epochs", "current + context + sequence"),
        role="state",
    )

    figure.section(
        "3", "CACHE HIERARCHY", "private caches stay ordinary; LLC state is extended",
        782, role="state",
    )
    figure.card(
        24, 824, 360, 245, "L1D and L2",
        (
            "ordinary tag/data lookup",
            "ordinary private fills",
            "record or property may hit here",
            "no line-local ECG state needed",
        ),
        role="data",
    )
    figure.card(
        420, 824, 360, 245, "MSHR",
        (
            "target-level allocOnFill",
            "ReuseBind merge state",
            "newest compatible sequence",
            "conflict propagation",
        ),
        role="transfer",
    )
    figure.card(
        816, 824, 360, 245, "LLC replacement entry",
        (
            "valid / property / RRPV / recency",
            "tier / two epochs / count / context",
            "stamp-valid bit",
            "shared victim-policy adapter",
        ),
        role="state",
    )
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
        "Evidence boundary for ECG architecture claims",
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
    figure.card(
        24, 180, 360, 285, "gem5 O3",
        (
            "architectural execution time authority",
            "decoded experimental RISC-V operations",
            "exact dynamic Request extension",
            "native LSQ, MSHR, cache, speculation",
            "semantic output matches policy peers",
            "guest build receipt seals binary",
        ),
        role="compute",
    )
    figure.card(
        420, 180, 360, 285, "cache_sim",
        (
            "shared victim-policy behavior",
            "kernel-declared graph-data accesses",
            "functional LLC and off-chip traffic",
            "no cycle or instruction model",
            "selector result is only a hypothesis",
            "structural FlowThrough access receipt",
        ),
        role="data",
    )
    figure.card(
        816, 180, 360, 285, "Sniper",
        (
            "modeled cache and traffic direction",
            "equal semantic work for scale rows",
            "indexed path: exact per-edge markers",
            "computed fused sideband is diagnostic",
            "inconsistent fused hints fail closed",
            "time is not ReuseBind speedup evidence",
        ),
        role="state",
    )

    figure.section(
        "2", "ROW ACCEPTANCE", "a success-shaped fallback is not a valid experiment row",
        510, role="verify",
    )
    figure.card(
        24, 552, 550, 245, "Mechanism receipts",
        (
            "requested and effective policy/mode match",
            "active structural carrier and positive events",
            "record width and replacement/substitution agree",
            "P-OPT matrix and phase-two queries are active when required",
        ),
        role="verify",
    )
    figure.card(
        600, 552, 576, 245, "Semantic receipts",
        (
            "full kernel output or fixed semantic edge budget",
            "all policy rows in one matched group agree",
            "one failed row invalidates group timing",
            "semantic correctness is independent of speculation counters",
        ),
        role="verify",
    )

    figure.section(
        "3", "CLAIM LIMITS", "state what is omitted before interpreting a ratio",
        842, role="transfer",
    )
    figure.card(
        24, 884, 550, 185, "P-OPT analytic mode",
        (
            "reserved data ways and cumulative matrix bytes are charged",
            "target-time matrix latency / bandwidth / queueing are omitted",
            "popt_target_time_charged = 0 -> optimistic timing bound",
            "frontier-kernel P-OPT rows remain diagnostic",
        ),
        role="transfer",
    )
    figure.card(
        600, 884, 576, 185, "Publication rule",
        (
            "report time, total off-chip traffic, and instructions together",
            "use same-build, same-cell baselines",
            "exclude timing_valid_for_speedup = 0",
            "publish final tables only after the frozen campaign completes",
        ),
        role="verify",
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
