#!/usr/bin/env python3
"""Generate Scale6 wiki plates and editable Draw.io mirrors."""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from ecg_figure_lib import (
    AMBER, BLUE, BORDER, GRAY, GREEN, INK, PURPLE, RED, WHITE,
    BLUE_MATTE, RED_MATTE,
    Figure, FigureTarget, clean_generated_roots,
)


SOURCE_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_PATH = SOURCE_ROOT / "fig/ecg-figure-fixture.json"
MECHANISM = "reuse-plan-flowthrough"
ISA = "risc-v-instruction-path"
WALK = "property-to-cache-walkthrough"


@dataclass(frozen=True)
class CheckedFixture:
    num_vertices: int
    mapping: tuple[int, ...]
    edges: tuple[tuple[int, int, int], ...]
    rows: tuple[tuple[int, ...], ...]
    offsets: tuple[int, ...]
    stream: tuple[int, ...]
    tracked_reader: int
    tracked_dest: int
    position: int
    next_position: int
    distance: int
    token: int
    upper: int
    record: int
    property_base: int
    property_address: int
    property_line: int

    @property
    def sequence(self) -> int:
        return self.position + 1

    @property
    def deadline(self) -> int:
        return self.sequence + self.upper


def load_fixture() -> CheckedFixture:
    raw = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    n = int(raw["num_vertices"])
    mapping = tuple(raw["source_to_internal"])
    edges = tuple(tuple(edge) for edge in raw["weighted_undirected_edges"])
    rows: list[list[int]] = [[] for _ in range(n)]
    for left, right, _weight in edges:
        rows[mapping[left]].append(mapping[right])
        rows[mapping[right]].append(mapping[left])
    for row in rows:
        row.sort()
    offsets = [0]
    for row in rows:
        offsets.append(offsets[-1] + len(row))
    stream = tuple(vertex for row in rows for vertex in row)
    tracked = raw["tracked_edge"]
    outer = mapping[tracked["source_vertex"]]
    dest = mapping[tracked["destination_vertex"]]
    position = offsets[outer] + rows[outer].index(dest)
    element = int(raw["property_element_bytes"])
    line_bytes = int(raw["cache_line_bytes"])
    vertices_per_line = line_bytes // element
    next_position = next(
        index for index in range(position + 1, len(stream))
        if stream[index] // vertices_per_line == dest // vertices_per_line
    )
    distance = next_position - position
    bucket = min(distance.bit_length() - 1, 30)
    token = 2 + bucket
    upper = (1 << (bucket + 1)) - 1
    base = int(raw["property_base"])
    address = base + dest * element
    return CheckedFixture(
        n, mapping, edges, tuple(tuple(row) for row in rows),
        tuple(offsets), stream, outer, dest, position, next_position,
        distance, token, upper, (token << 26) | dest,
        base, address, address & ~(line_bytes - 1),
    )


def plate(root, slug, index, topic, title, subtitle, description, height):
    return Figure(
        root, FigureTarget(slug, index, topic),
        title, subtitle, description, height,
    )


def part(f, x, y, width, height, title, lines=(), role="neutral"):
    """A component in a connected architecture, not a prose panel."""
    f.rect(x, y, width, height, role=role, radius=0)
    f.text(x + 14, y + 28, title, size=17, bold=True, max_width=width - 28)
    if lines:
        f.lines(x + 14, y + 57, lines, step=25, max_width=width - 28)


def note(f, y, text, color=INK):
    f.text(40, y, text, size=16,
           color=BORDER if color == GRAY else color, max_width=1120)


def tabular(f, x, y, widths, headers, rows, row_height=43):
    width = sum(widths)
    f.table(x, y, width, row_height * (len(rows) + 1),
            len(rows) + 1, role="neutral")
    cursor = x
    for column, column_width in enumerate(widths):
        if column:
            f.line((cursor, y), (cursor, y + row_height * (len(rows) + 1)),
                   color=BORDER, width=1)
        f.text(cursor + 12, y + 28, headers[column],
               size=17, bold=True, max_width=column_width - 24)
        for row, values in enumerate(rows):
            f.text(cursor + 12, y + (row + 1) * row_height + 28,
                   str(values[column]), size=16, max_width=column_width - 24)
        cursor += column_width


def fifo(f, x, y, width, count, title, role="state"):
    f.text(x, y - 14, title, size=17, bold=True, color=PURPLE, max_width=width)
    for index in range(count):
        w = width / count
        f.rect(x + index * w, y, w, 42, role=role, radius=0)
    f.text(x, y + 60, "head", size=16)
    f.text(x + width, y + 60, "tail", size=16, anchor="end")


def system_overview(root, fx):
    f = plate(
        root, "home", "01", "system-overview",
        "Scale6: future reuse in the existing edge word",
        "Current result: functional cache behavior. Native cycle-timed integration remains a separate gate.",
        "The current ECG candidate uses a four-byte destination-plus-token record. "
        "Offline line-indexed construction precedes PageRank. Demand accesses, "
        "private-hit commit updates and bounded LLC prefetching are modeled in "
        "cache_sim. The diagram explicitly distinguishes that evidence from the "
        "still-unimplemented Scale6 gem5 path and physical-cost qualification.",
        1060,
    )
    f.section("1", "OFFLINE: KEEP THE EDGE WORD SMALL",
              "directed PageRank pull; fixed traversal", 138, role="data")
    f.cylinder(50, 195, 245, 133, role="data")
    f.lines(72, 237, ("Directed CSR / CSC", "row offsets + neighbors",
                      "property-line reuse"), bold_first=True, step=28)
    part(f, 420, 195, 295, 133, "Two-pass construction",
         ("first + next position per line", "pack the in-edge CSR in place"), "compute")
    f.arrow(((295, 260), (420, 260)), kind="transfer", label="scan",
            cadence="offline", label_at=(357, 188), color=BLUE)
    f.bitfield(840, 207, 336, 100,
               (("token", 6, "state"), ("vertex", 26, "data")), total_bits=32)
    f.arrow(((715, 260), (840, 260)), kind="transfer", label="pack",
            cadence="per edge", label_at=(778, 188), color=PURPLE)
    note(f, 376, "4 bytes per edge; no runtime rereference matrix and no extra edge sidecar.")
    note(f, 404, "Twitter preprocessing scratch: about 39.7 MiB, indexed by property line, not by edge.", GRAY)

    f.section("2", "RUNTIME: KEEP LLC STATE FRESH",
              "cache_sim model, not a completed gem5 port", 465, role="compute")
    part(f, 50, 535, 240, 100, "Property access",
         ("vertex + future token",), "data")
    part(f, 430, 535, 260, 100, "L1D / L2",
         ("ordinary data service",), "data")
    part(f, 860, 535, 290, 100, "Shared LLC",
         ("data + line-local future state",), "state")
    f.arrow(((290, 575), (430, 575)), kind="transfer", label="demand",
            cadence="per edge", label_at=(360, 518), color=BLUE)
    f.arrow(((690, 575), (860, 575)), kind="transfer", label="miss stream",
            cadence="private miss", label_at=(775, 518), color=BLUE)
    fifo(f, 430, 701, 260, 16, "Commit update: 16 entries")
    f.arrow(((170, 635), (170, 722), (430, 722)), kind="control",
            label="including private hits", label_at=(300, 699), color=PURPLE)
    f.arrow(((690, 722), (1005, 722), (1005, 635)), kind="control",
            label="resident + live only", label_at=(843, 700), color=PURPLE)
    note(f, 783, "Selective prefetch: 16-record lookahead, 8 pending entries, LLC-only fills.", AMBER)
    note(f, 813, "The update and prefetch delays are measured in governed requests here, not CPU cycles.", GRAY)

    f.section("3", "WHAT THIS ESTABLISHES",
              "keep the implementation and evidence boundaries visible", 870, role="verify")
    f.text(50, 933, "ESTABLISHED", size=17, bold=True, color=GREEN)
    f.lines(50, 963, ("Full-graph PageRank results, LLC misses,",
                      "off-chip traffic, bounded model queues."), max_width=495)
    f.text(650, 933, "STILL REQUIRED", size=17, bold=True, color=AMBER)
    f.lines(650, 963, ("Scale6 gem5 request/retirement path,",
                       "cycle timing and physical area evidence."), max_width=500)
    return f


def offline_construction(root, fx):
    f = plate(
        root, MECHANISM, "01", "offline-construction",
        "Building Scale6 records in traversal order",
        "One checked graph connects adjacency order, property-line reuse, and the packed record.",
        "An undirected fixture is traversed as PageRank pull. Its source adjacency "
        "4 to 7 maps to internal row 8 and property vertex 18. Flattening the rows "
        "locates edge positions 18 and 22 as successive accesses to the same "
        "property line. Their distance four becomes token four, packed with vertex "
        "18 as 0x10000012. The production directed builder uses two line-indexed arrays.",
        1170,
    )
    f.section("1", "TOPOLOGY, NOT AN EPOCH LABEL",
              "fixture weights are omitted; PageRank reads topology", 138, role="data")
    coords = {
        0: (95, 235), 1: (90, 390), 2: (225, 310),
        3: (355, 220), 4: (365, 405), 5: (515, 325),
        6: (630, 220), 7: (645, 410), 8: (755, 315),
    }
    for left, right, _weight in fx.edges:
        f.line(coords[left], coords[right], color=GRAY, width=1.5)
    a, b = coords[4], coords[7]
    length = math.hypot(b[0] - a[0], b[1] - a[1])
    dx, dy = (b[0] - a[0]) / length, (b[1] - a[1]) / length
    f.arrow(((a[0] + 23 * dx, a[1] + 23 * dy),
             (b[0] - 25 * dx, b[1] - 25 * dy)),
            kind="model-edge", color=RED)
    for source, (x, y) in coords.items():
        f.circle(x, y, 23,
                 fill=RED_MATTE if source in (4, 7) else BLUE_MATTE,
                 stroke=RED if source in (4, 7) else BLUE)
        f.text(x, y + 6, str(source), size=17, bold=True, anchor="middle")
        f.rect(x - 36, y + 31, 72, 24, role="ink",
               stroke=WHITE, stroke_width=0, radius=3)
        f.text(x, y + 48, f"int {fx.mapping[source]}",
               size=16, anchor="middle", color=BORDER)
    f.lines(850, 217, (
        "PR pull example",
        "fixture adjacency 4 -> 7",
        "internal row 8 -> vertex 18",
        "row offsets [14, 19)",
        "edge position j = 18",
        "property line: vertices 16..31",
    ), step=35, bold_first=True, max_width=310)
    note(f, 492, "Sparse internal IDs preserve the existing checked fixture; Scale6 is forced to 26 ID bits.")

    f.section("2", "FLATTEN THE ACTUAL REQUEST ORDER",
              "same property line, even when the vertex changes", 553, role="state")
    for index, position in enumerate(range(14, 26)):
        x = 40 + index * 93
        vertex = fx.stream[position]
        role = "state" if vertex // 16 == fx.tracked_dest // 16 else "neutral"
        f.rect(x, 609, 93, 106, role=role, radius=0)
        f.text(x + 46, 636, f"j={position}", mono=True, anchor="middle")
        f.text(x + 46, 669, f"v={vertex}", mono=True, anchor="middle")
        f.text(x + 46, 698, f"line {vertex // 16}", anchor="middle")
    f.arrow(((458, 715), (458, 757), (830, 757), (830, 715)),
            kind="dependency", label="next use: 22 - 18 = 4 requests",
            label_at=(644, 792), color=PURPLE)

    f.section("3", "PACK WITHOUT AN EDGE-SIZED SIDECAR",
              "production builder: directed graphs with distinct in/out CSR", 853, role="compute")
    part(f, 40, 907, 310, 130, "Forward pass",
         ("first position for each line", "two 64-bit position arrays"), "data")
    part(f, 465, 907, 315, 130, "Reverse pass",
         ("next position -> distance", "distance -> token -> packed word"), "compute")
    f.arrow(((350, 968), (465, 968)), kind="control", label="positions",
            label_at=(408, 948), color=GREEN)
    part(f, 895, 907, 265, 130, f"0x{fx.record:08x}",
         (f"vertex {fx.tracked_dest} | token {fx.token}", "32 bits = 4 bytes"), "state")
    f.arrow(((780, 968), (895, 968)), kind="transfer", label="word",
            cadence="per edge", label_at=(838, 1074), color=PURPLE)
    note(f, 1106, "Twitter: 41,652,230 vertices, 1,468,364,884 directed edges; about 39.7 MiB auxiliary state.")
    note(f, 1139, "Construction finishes before the measured traversal. Preprocessing cost is reported separately.", GRAY)
    return f


def record_formats(root, fx):
    f = plate(
        root, MECHANISM, "02", "record-formats",
        "One 32-bit record, including Twitter-sized IDs",
        "The six-bit token encodes state and a logarithmic distance class; prefetch uses no record bits.",
        "The Scale6 word places the six-bit future token in bits 31 through 26 "
        "and the property vertex in bits 25 through zero. A token table explains "
        "unknown, dead, finite-current and next-traversal reuse. The plate contrasts "
        "this format with the retained n18 Full14 format without claiming identical precision.",
        1020,
    )
    f.section("1", "SCALE6: THE CURRENT SCALABLE FORMAT",
              "high bits on the left; ordinary edge width retained", 138, role="data")
    f.text(141, 206, "[31:26]", mono=True, anchor="middle", color=PURPLE)
    f.text(801, 206, "[25:0]", mono=True, anchor="middle", color=BLUE)
    f.bitfield(40, 227, 1120, 106,
               (("future token", 6, "state"), ("property vertex ID", 26, "data")),
               total_bits=32)
    note(f, 376, f"Checked word: 0x{fx.record:08x} = (token {fx.token} << 26) | vertex {fx.tracked_dest}.")
    note(f, 407, "Twitter needs 26 destination bits. The primary Scale6 configuration leaves exactly six token bits.", GRAY)

    f.section("2", "THE TOKEN ALPHABET",
              "one state code or one distance bucket", 460, role="state")
    tabular(f, 40, 517, (145, 205, 355, 415),
            ("Token", "Meaning", "Payload interpretation", "Runtime rule"),
            (
                ("0", "UNKNOWN", "no usable future prediction", "local GRASP / RRIP fallback"),
                ("1", "DEAD", "no remaining governed use", "bypass / prefer as victim"),
                ("2..32", "FINITE", "bucket = token - 2", "current-traversal distance"),
                ("33..63", "WRAP", "bucket = token - 33", "next-traversal distance"),
            ), row_height=48)
    note(f, 799, "bucket = floor(log2(distance)); decode upper bound = 2^(bucket + 1) - 1.")
    note(f, 829, "Prefetch candidates come from the record lookahead, not a separately encoded action.", AMBER)

    f.section("3", "DO NOT MIX FORMAT CLAIMS",
              "explicitly select the format; never truncate IDs", 888, role="verify")
    note(f, 947, "Full14 (n18): destination 18 + reference 8 + state 2 + action 4 = 32 bits.")
    note(f, 978, "Scale6 (through n26): destination 26 + token 6 = 32 bits. Larger IDs fail closed.", RED)
    return f


def future_distance(root, fx):
    f = plate(
        root, MECHANISM, "03", "future-distance",
        "From the next use to a conservative deadline",
        "The coordinate is the governed edge-request sequence, not an outer-vertex ID or a CPU cycle.",
        "The checked property-line use at sequence 19 is followed by a use at "
        "sequence 23. Distance four is represented by token four and decoded "
        "distance seven, producing deadline 26. Holding this prediction fixed, "
        "it becomes unknown at sequence 27, never dead. Separate panels explain "
        "bucket precision and traversal-wrap semantics.",
        1070,
    )
    f.section("1", "ONE LINE, TWO DIFFERENT POSITIONS",
              "line 0x80000040 contains vertices 16..31", 138, role="data")
    f.text(40, 208, f"j={fx.position}, seq={fx.sequence}: read p[{fx.tracked_dest}]",
           size=18, color=BLUE)
    f.text(390, 208, f"j={fx.next_position}, seq={fx.next_position + 1}: next line use",
           size=18, color=GREEN)
    f.rect(100, 315, 560, 50, role="compute", stroke=GREEN, radius=0)
    f.text(380, 347, "effective finite prediction", anchor="middle", color=GREEN)
    f.line((100, 283), (1100, 283), color=INK, width=2)
    for seq, x, label, y, color in (
        (19, 100, "current", 254, BLUE),
        (23, 420, "actual next use", 238, GREEN),
        (26, 660, "deadline", 254, PURPLE),
        (27, 740, "expired", 230, RED),
        (31, 1060, "sequence", 254, BORDER),
    ):
        f.line((x, 272), (x, 296), color=color, width=2)
        f.text(x, y, label, size=16, anchor="middle", color=color)
        f.text(x, 393, str(seq), mono=True, anchor="middle", color=color)
    f.arrow(((100, 428), (420, 428)), kind="dependency", label="true distance = 4",
            label_at=(260, 458), color=GREEN)
    f.arrow(((100, 493), (660, 493)), kind="dependency", label="decoded upper bound = 7",
            label_at=(380, 523), color=PURPLE)
    note(f, 567, "Holding this prediction fixed for illustration; real hits and commit updates can refresh it.", GRAY)

    f.section("2", "LOGARITHMIC PRECISION IS EXPLICIT",
              "no runtime logarithm is required", 620, role="state")
    tabular(f, 40, 675, (245, 265, 270, 340),
            ("True distance", "Bucket", "Finite token", "Decoded distance"),
            (("1", "0", "2", "1"),
             ("2..3", "1", "3", "3"),
             ("4..7", "2", "4", "7"),
             ("8..15", "3", "5", "15")))
    f.section("3", "EXPIRY IS NOT DEATH",
              "wrapping the sequence counter is handled separately", 939, role="verify")
    note(f, 996, "Passed bound -> UNKNOWN. Only explicit no-future-use knowledge produces DEAD.", RED)
    note(f, 1028, "WRAP identifies next-traversal reuse; the requested final traversal cannot promise another pass.")
    return f


def llc_policy(root, fx):
    f = plate(
        root, MECHANISM, "04", "llc-policy-pipeline",
        "Fresh LLC state and Scale6 victim selection",
        "Private hits must not leave LLC predictions stale; the functional update channel is bounded.",
        "A demand may hit privately while its committed reuse information still "
        "needs to reach an already-resident LLC line. The plate shows the "
        "16-entry coalescing queue and expiry checks, the 35 added bits per LLC "
        "line, and the actual ranked victim policy. Unknown and finite properties "
        "share a scored category; this is not the old RRIP-first ReusePlan policy.",
        1320,
    )
    f.section("1", "PRIVATE HITS STILL PRODUCE UPDATES",
              "LLC hit/fill stamps and commit refresh are distinct", 138, role="compute")
    part(f, 40, 200, 275, 115, "Governed property load",
         ("read value normally", "carry the per-edge prediction"), "data")
    part(f, 445, 200, 270, 115, "L1D / L2 hit",
         ("no demand reaches the LLC", "prediction still advances"), "data")
    f.arrow(((315, 250), (445, 250)), kind="transfer", label="data service",
            cadence="per access", label_at=(380, 186), color=BLUE)
    part(f, 860, 200, 300, 115, "Resident LLC line",
         ("update metadata, not data bytes", "never allocate for an update"), "state")
    fifo(f, 445, 387, 270, 16, "16-entry coalescing queue")
    f.arrow(((176, 315), (176, 408), (445, 408)), kind="control",
            label="commit refresh", label_at=(293, 387), color=PURPLE)
    f.arrow(((715, 408), (1010, 408), (1010, 315)), kind="control",
            label="resident + unexpired", label_at=(869, 388), color=PURPLE)
    note(f, 479, "Model: 8 governed requests of delay, at most 1 update per request, coalesce by property line.")
    note(f, 509, "Discard expired updates; an expired line prediction resolves to UNKNOWN, never DEAD.", RED)

    f.section("2", "LINE STATE IS CACHE-SIZED",
              "baseline tags, data, recency and RRPV remain separate", 567, role="state")
    f.bitfield(40, 625, 1120, 90,
               (("deadline", 32, "state"), ("state", 2, "state"),
                ("origin", 1, "transfer")), total_bits=35)
    note(f, 757, "35 added bits per LLC line = 32-bit deadline + 2-bit future state + 1 prefetch-origin bit.")
    note(f, 788, "Local GRASP tier is derived at the cache. It is not another field in the six-bit edge token.")

    f.section("3", "RANK THE ACTUAL CANDIDATE SET",
              "invalid ways fill first; known-dead governed misses bypass", 845, role="compute")
    tabular(f, 40, 902, (100, 230, 370, 420),
            ("Rank", "Candidate class", "Score inside the class", "Selection"),
            (
                ("first", "known-dead property", "explicit DEAD state", "prefer reclaiming the dead line"),
                ("next", "non-property line", "recency", "oldest non-property line"),
                ("then", "finite property", "distanceRRPV(remaining)", "shares the property category"),
                ("then", "unknown property", "max(RRPV, local GRASP)", "shares the property category"),
            ), row_height=47)
    note(f, 1187, "Within the property category: score -> unknown tie -> remaining distance -> colder tier -> oldest.")
    note(f, 1220, "Unknown does not always precede finite: the score is compared first.", PURPLE)
    note(f, 1264, "This is the Scale6 model. Native gem5 update transport and cycle timing remain unimplemented.", AMBER)
    return f


def lookahead_prefetch(root, fx):
    f = plate(
        root, MECHANISM, "05", "lookahead-prefetch",
        "Selective prefetch from the record stream",
        "No action bits are added to the edge word. All extra fills count as off-chip traffic.",
        "An illustrative 16-record window identifies the first occurrence of "
        "a distinct property line between leads eight and fifteen. Eligible "
        "candidates are ranked by decoded future distance, with ties favoring "
        "lead ten. Resident, pending and admission filters precede an eight-entry "
        "LLC-only queue. The request-based model is not a claim of native cycle timing.",
        1190,
    )
    f.section("1", "A BOUNDED WINDOW, NOT A FUTURE TABLE",
              "illustrative line IDs; separate from the checked fixture", 138, role="data")
    window = ("A", "A", "B", "B", "C", "C", "A", "C",
              "D", "D", "E", "D", "E", "F", "F", "F")
    for index, line in enumerate(window):
        x = 40 + 70 * index
        f.text(x + 35, 211, "now" if index == 0 else f"+{index}",
               size=16, anchor="middle")
        f.rect(x, 235, 70, 76, role="transfer" if index >= 8 else "neutral",
               stroke=GREEN if index == 10 else BORDER, radius=0)
        f.text(x + 35, 282, line, size=22, bold=True, anchor="middle",
               color=GREEN if index == 10 else INK)
    f.line((600, 331), (1158, 331), color=AMBER, width=3)
    f.text(880, 365, "candidate leads 8..15", size=17, bold=True,
           anchor="middle", color=AMBER)
    note(f, 411, "Reject the current line and any line already seen earlier in the lookahead.")
    note(f, 442, "Rank eligible lines by the smallest decoded future bound; break ties toward lead 10.")

    f.section("2", "SELECT, FILTER, THEN ENQUEUE",
              "the example chooses E at lead +10", 503, role="compute")
    tabular(f, 40, 563, (100, 80, 190), ("Lead", "Line", "Future bound"),
            (("+8", "D", "31"), ("+10", "E", "7 (selected)"), ("+13", "F", "15")))
    part(f, 510, 596, 265, 144, "Admission filters",
         ("already resident?", "already pending?", "admit this prefetch fill?"), "compute")
    f.arrow(((410, 650), (510, 650)), kind="control", label="candidate",
            label_at=(460, 628), color=GREEN)
    fifo(f, 875, 619, 280, 8, "8-entry prefetch queue", "transfer")
    f.arrow(((775, 650), (875, 650)), kind="control", label="accepted",
            label_at=(825, 690), color=AMBER)
    note(f, 792, "At most 1 issue per 8 governed requests; configured latency is 8 governed requests.", AMBER)

    f.section("3", "FILL THE LLC, NOT THE PRIVATE CACHES",
              "measure usefulness and total traffic separately", 851, role="transfer")
    f.cylinder(45, 916, 240, 126, role="data")
    f.lines(73, 960, ("Memory", "one property cache line"), bold_first=True, step=30)
    part(f, 500, 927, 290, 102, "LLC prefetch fill",
         ("can satisfy a later demand",), "state")
    part(f, 920, 927, 235, 102, "L1D / L2",
         ("no prefetch allocation",), "neutral")
    f.arrow(((285, 980), (500, 980)), kind="transfer", label="prefetch",
            cadence="per line", label_at=(392, 957), color=AMBER)
    f.text(852, 985, "no fill", size=17, bold=True, anchor="middle", color=RED)
    note(f, 1090, "Fewer demand misses can include prefetched traffic. Count reads + writebacks, not demand misses alone.")
    note(f, 1126, "FlowThrough is OFF in the primary Scale6 comparisons; this prefetch path is a separate mechanism.", RED)
    return f


def capacity_accounting(root, fx):
    f = plate(
        root, MECHANISM, "06", "capacity-accounting",
        "P-OPT columns and the LLC capacity budget",
        "A resident epoch column is graph-sized. Two columns do not mean two cache ways.",
        "For full Twitter, one one-byte-per-property-line column is 2,603,265 "
        "bytes. Full P-OPT keeps two columns and SE keeps one, while both retain "
        "256 backing columns. Sixteen-way bars show the full and single-epoch "
        "reservations at 8, 16 and 24 MiB. ECG's extra on-chip state is separately "
        "disclosed rather than compared to DRAM matrix storage as a silicon ratio.",
        1140,
    )
    f.section("1", "BACKING MATRIX IS NOT ALL IN THE LLC",
              "Twitter: 41,652,230 vertices; 4-byte properties", 138, role="data")
    f.cylinder(40, 197, 330, 154, role="data")
    f.lines(67, 239, ("256 backing columns", "635.6 MiB in memory",
                      "same backing size for SE"), bold_first=True, step=31)
    part(f, 565, 207, 275, 134, "Full P-OPT",
         ("current + next", "about 4.97 MiB payload"), "state")
    part(f, 915, 207, 245, 134, "P-OPT-SE",
         ("current only", "about 2.48 MiB payload"), "state")
    f.arrow(((370, 273), (565, 273)), kind="transfer", label="column",
            cadence="per epoch", label_at=(468, 247), color=AMBER)
    note(f, 393, "column bytes = ceil(vertices x 4 / 64) = 2,603,265; no fixed-way shortcut.")
    note(f, 424, "One-column residency does not halve the 256-column stream over a complete traversal.", AMBER)

    f.section("2", "RESERVE ENOUGH WHOLE WAYS",
              "purple = matrix; blue = application data", 487, role="state")
    f.text(250, 548, "FULL P-OPT: TWO COLUMNS", size=17, bold=True, color=PURPLE)
    f.text(750, 548, "P-OPT-SE: ONE COLUMN", size=17, bold=True, color=PURPLE)
    for row, (capacity, full, single) in enumerate(((8, 10, 5), (16, 5, 3), (24, 4, 2))):
        y = 590 + row * 108
        f.text(40, y + 30, f"{capacity} MiB", size=22, bold=True)
        for x, reserved in ((250, full), (750, single)):
            for way in range(16):
                f.rect(x + 25 * way, y, 25, 45,
                       role="state" if way < reserved else "data", radius=0)
                f.text(x + 25 * way + 12.5, y + 29,
                       "M" if way < reserved else "D",
                       size=16, anchor="middle")
            f.text(x, y + 77, f"{reserved} reserved / {16 - reserved} data ways", size=17, bold=True)
    note(f, 933, "SE uses a different six-bit-value encoding. A two-way undercharge of full P-OPT is not SE.", RED)

    f.section("3", "ECG COST IS NOT ZERO",
              "separate data capacity, on-chip state and backing storage", 976, role="verify")
    note(f, 1032, "At 8 MiB: Scale6 retains 16 data ways and adds about 560 KiB of line/controller state.")
    note(f, 1064, "That is not yet an equal-area comparison. Total metadata-footprint ratios are not silicon-area ratios.", RED)
    note(f, 1097, "The two SE post-final interpretations are disclosed reconstructions, not bit-exact paper reproductions.", GRAY)
    return f


def instruction_family(root, fx):
    f = plate(
        root, ISA, "01", "instruction-family",
        "RISC-V integration: existing path and next step",
        "The native operand pair exists; retirement and cache integration remain pending.",
        "The existing experimental RISC-V custom-0 instruction family supports "
        "record acquisition and a dependent property load with ReuseBind metadata. "
        "Scale6's separate raw record and property operations now implement the "
        "26-plus-six-bit operand format and traversal position. Retirement "
        "transport, cache integration and native prefetch remain pending. "
        "No end-to-end native timing result is asserted.",
        1050,
    )
    f.section("1", "EXISTING EXPERIMENTAL GEM5 SUPPORT",
              "legacy ReusePlan / ReuseBind, not Scale6", 138, role="compute")
    part(f, 40, 207, 320, 145, "Record-load family",
         ("plan.load / flow.load", "record -> integer register", "placement is explicit"), "data")
    part(f, 475, 207, 305, 145, "Renamed operand",
         ("per-dynamic dependency", "not a shared mailbox", "property waits for the record"), "state")
    part(f, 895, 207, 265, 145, "Property-load family",
         ("bind.load / bind.iload", "typed property result", "Request-bound ReuseBind"), "compute")
    f.arrow(((360, 277), (475, 277)), kind="dependency", label="record operand",
            label_at=(418, 188), color=PURPLE)
    f.arrow(((780, 277), (895, 277)), kind="dependency", label="rs2 dependency",
            label_at=(838, 188), color=PURPLE)
    note(f, 401, "custom-0 is a research extension, not a ratified RISC-V ISA feature.", GRAY)
    note(f, 432, "Scale6 raw operations now execute in O3; the retirement/cache path is not enabled.", AMBER)

    f.section("2", "SCALE6 TARGET CONTRACT",
              "operand codec exists; retirement/cache transport pending", 495, role="verify")
    f.bitfield(40, 556, 1120, 90,
               (("token", 6, "state"), ("vertex", 26, "data")), total_bits=32)
    tabular(f, 40, 704, (285, 410, 425),
            ("Boundary", "Required information", "Must preserve"),
            (
                ("record -> property", "vertex + six-bit token", "one exact dynamic association"),
                ("property -> retirement", "physical line + position + context", "squash, replay and translation"),
                ("retirement -> LLC", "committed line prediction", "bounded delay and bandwidth"),
                ("lookahead -> prefetch", "real record-stream candidates", "traffic, admission and LLC-only fill"),
            ), row_height=45)
    note(f, 977, "Raw funct7 0x30 / 0x34 are experimental encodings; no Scale6 speedup claim is enabled.", RED)
    return f


def o3_pipeline(root, fx):
    f = plate(
        root, ISA, "02", "o3-request-pipeline",
        "Target Scale6 path through an out-of-order core",
        "Native record/property operands execute; the retirement-to-LLC path remains a target.",
        "An explicit register dependency connects the Scale6 record load to the "
        "property access. Standard fetch, decode, rename, issue, AGU, LSQ, translation "
        "and cache structures retain ordinary execution semantics. A separate "
        "retirement-only update route must carry committed predictions to the LLC, "
        "including for private hits. Squashed operations cannot enqueue refreshes.",
        1380,
    )
    f.section("1", "A REAL WORD FROM THE EDGE STREAM",
              "checked native operand example; cache path pending", 138, role="data")
    part(f, 40, 195, 315, 128, "Incoming CSR row u=8",
         ("row_ptr[8]=14; row_ptr[9]=19", "neighbors: 3, 6, 7, 11, 18"), "data")
    f.bitfield(475, 205, 685, 104,
               (("token 4", 6, "state"), ("vertex 18", 26, "data")), total_bits=32)
    f.arrow(((355, 260), (475, 260)), kind="transfer", label="record[18]",
            cadence="one word", label_at=(415, 188), color=BLUE)
    note(f, 360, f"0x{fx.record:08x} -> p[18] at 0x{fx.property_address:08x}; sequence 19, decoded bound 7.")

    f.section("2", "ORDINARY O3 EXECUTION, EXPLICIT METADATA",
              "CPU stages are not a shortcut around memory ordering", 420, role="compute")
    f.rect(40, 475, 1120, 575, role="neutral", radius=0)
    for x, title in ((65, "Fetch"), (285, "Decode"), (505, "Rename"), (760, "Issue / select")):
        part(f, x, 511, 170 if x != 760 else 220, 70, title, role="compute")
    for start, end in ((235, 285), (455, 505), (675, 760)):
        f.arrow(((start, 545), (end, 545)), kind="control", label="instruction flow", color=GREEN)
    f.text(66, 616, "instruction flow", size=16, color=GREEN)
    part(f, 65, 668, 290, 105, "Record load I0",
         ("ordinary load -> physical P17",), "data")
    f.table(460, 668, 225, 130, 3, role="state")
    f.text(476, 695, "Physical registers", size=17, bold=True, color=PURPLE)
    f.text(476, 739, "P17 = record word", size=16)
    f.text(476, 782, "I1 reads P17", size=16)
    part(f, 790, 668, 335, 130, "Property load I1",
         ("AGU: base + vertex x 4", "LSQ: dynamic Request + metadata"), "compute")
    f.arrow(((955, 581), (955, 668)), kind="control", label="issue I1",
            label_at=(1035, 624), color=GREEN)
    f.arrow(((355, 729), (460, 729)), kind="dependency", label="load result",
            label_at=(407, 648), color=PURPLE)
    f.arrow(((685, 729), (790, 729)), kind="dependency", label="I1 waits P17",
            label_at=(738, 648), color=PURPLE)
    f.table(65, 867, 620, 131, 3, role="neutral")
    f.text(82, 895, "ROB: completion is not retirement", size=17, bold=True)
    f.text(82, 939, "I0 / I1 keep their ordinary replay and exception state", size=16)
    f.text(82, 982, "Only the oldest completed, non-squashed instruction retires", size=16)
    part(f, 790, 867, 335, 131, "Translation + L1D / L2",
         ("data response + register writeback", "private hits can bypass L3"), "data")
    f.arrow(((955, 798), (955, 867)), kind="transfer", label="property Request",
            cadence="one load", label_at=(791, 839), label_anchor="end", color=BLUE)
    f.arrow(((790, 934), (685, 934)), kind="control", label="completion",
            label_at=(738, 912), color=GREEN)

    f.section("3", "THE MISSING RETIREMENT-TO-LLC ROUTE",
              "Scale6 target: implement and measure, do not infer", 1110, role="verify")
    part(f, 40, 1169, 270, 105, "Retired property load",
         ("squashed load: no refresh",), "compute")
    fifo(f, 470, 1190, 260, 16, "Bounded commit transport")
    part(f, 900, 1169, 260, 105, "Resident LLC metadata",
         ("ordered, live updates only",), "state")
    f.arrow(((310, 1221), (470, 1221)), kind="control", label="commit-only update",
            label_at=(390, 1307), color=PURPLE)
    f.arrow(((730, 1221), (900, 1221)), kind="control", label="timed delivery",
            label_at=(815, 1307), color=AMBER)
    note(f, 1344, "Native latency, bandwidth, backpressure and drain behavior are not supplied by the cache_sim request clock.", RED)
    return f


def mshr_lifecycle(root, fx):
    f = plate(
        root, ISA, "03", "mshr-metadata-lifecycle",
        "Request lifetime is not metadata lifetime",
        "MSHR merging and commit-update coalescing solve different problems.",
        "The top flow summarizes existing ReuseBind MSHR compatibility and conflict "
        "handling. The lower state machine specifies the missing Scale6 retirement "
        "boundary: issued operations can complete or be squashed; only retirement "
        "can authorize commit refresh. Updates are coalesced separately and may be "
        "discarded when expired or when their line is no longer resident.",
        1140,
    )
    f.section("1", "EXISTING REUSEBIND MERGE RULES",
              "a reusable foundation, not a completed Scale6 extension", 138, role="data")
    part(f, 40, 209, 265, 144, "MSHR targets",
         ("requestor + context", "sequence + typed payload", "ordinary targets can mix"), "data")
    part(f, 450, 209, 305, 144, "Compatibility",
         ("newest compatible sequence", "equal seq requires same payload", "mismatch -> conflict marker"), "state")
    part(f, 900, 209, 260, 144, "Response metadata",
         ("copy chosen extension", "or propagate conflict", "clear on MSHR release"), "data")
    f.arrow(((305, 285), (450, 285)), kind="control", label="merge targets",
            label_at=(377, 189), color=PURPLE)
    f.arrow(((755, 285), (900, 285)), kind="control", label="one response",
            label_at=(827, 189), color=PURPLE)
    note(f, 404, "allocOnFill combines with OR. A non-allocating target cannot suppress an ordinary allocating target.")
    note(f, 435, "Primary Scale6 comparisons use FlowThrough OFF; these legacy allocation rules remain separate.", AMBER)

    f.section("2", "TARGET SCALE6 REFRESH LIFECYCLE",
              "required behavior; native gem5 transport is pending", 495, role="verify")
    part(f, 40, 557, 240, 94, "Issued load", ("retain dynamic metadata",), "data")
    part(f, 455, 557, 285, 94, "Completed load", ("may still be speculative",), "neutral")
    part(f, 920, 557, 240, 94, "Retired load", ("commit authorizes refresh",), "compute")
    f.arrow(((280, 603), (455, 603)), kind="control", label="normal completion",
            label_at=(368, 545), color=BLUE)
    f.arrow(((740, 603), (920, 603)), kind="control", label="oldest in ROB",
            label_at=(830, 545), color=GREEN)
    part(f, 455, 757, 285, 100, "Squashed / faulted",
         ("discard; do not enqueue",), "verify")
    f.arrow(((160, 651), (160, 807), (455, 807)), kind="control", label="squash",
            label_at=(305, 785), color=RED)
    f.arrow(((597, 651), (597, 757)), kind="control", label="squash",
            label_at=(649, 710), color=RED)
    fifo(f, 875, 779, 285, 16, "Coalesce commit updates")
    f.arrow(((1160, 603), (1180, 603), (1180, 800), (1160, 800)),
            kind="control", label="committed only",
            label_at=(1050, 725), color=PURPLE)
    f.section("3", "LLC STATE HAS ITS OWN EXPIRY",
              "no update is permission to allocate a data line", 931, role="state")
    note(f, 990, "Apply only to the matching resident line. Expired update -> discard; invalidated line -> no allocation.")
    note(f, 1025, "A stale finite prediction becomes UNKNOWN when resolved. It must never become falsely DEAD.", RED)
    note(f, 1060, "Native squash, translation, retry, queue-full and drain behavior must be demonstrated before timing claims.")
    return f


def checked_walkthrough(root, fx):
    f = plate(
        root, WALK, "01", "checked-request",
        "One edge word, one property line, one update",
        "A concrete Scale6 example; this is functional-model arithmetic, not a measured gem5 execution.",
        "The checked record 0x10000012 carries vertex 18 and finite token four. "
        "The property address is 0x80000048, within cache line 0x80000040. Sequence "
        "19 plus decoded distance seven gives deadline 26, while the true next "
        "use occurs at sequence 23. The plate distinguishes demand hit/fill state "
        "from delayed commit refresh and correctly discards stale updates.",
        1180,
    )
    f.section("1", "DECODE THE WORD ACTUALLY CONSUMED",
              "fixture row 8; edge position 18", 138, role="data")
    part(f, 40, 210, 295, 126, f"record = 0x{fx.record:08x}",
         ("32-bit substitute edge record", "no separate destination stream"), "data")
    part(f, 470, 210, 285, 126, "Decode",
         ("vertex = word & ((1<<26)-1)", "token = word >> 26"), "compute")
    part(f, 890, 210, 270, 126, "vertex 18 / token 4",
         ("FINITE, bucket 2", "upper-bound distance 7"), "state")
    f.arrow(((335, 272), (470, 272)), kind="control", label="split fields",
            label_at=(402, 188), color=PURPLE)
    f.arrow(((755, 272), (890, 272)), kind="control", label="decode token",
            label_at=(822, 188), color=PURPLE)

    f.section("2", "FORM THE ORDINARY PROPERTY ADDRESS",
              "neighbors 18 and 20 share the same 64-byte line", 410, role="data")
    note(f, 477, "property address = 0x80000000 + 18 x 4 = 0x80000048")
    note(f, 510, "line address = 0x80000040; byte offset = 8; property vertices 16..31", BLUE)
    for index in range(16):
        x = 40 + 70 * index
        f.rect(x, 548, 70, 68, role="state" if index == 2 else "data", radius=0)
        f.text(x + 35, 590, str(16 + index), size=17, bold=True, anchor="middle")
    note(f, 656, "Load p[18] through normal L1D / L2 / LLC data service. The property value is unchanged.")

    f.section("3", "STAMP, REFRESH, AND EXPIRE ARE DISTINCT",
              "current sequence 19; actual next use 23", 717, role="compute")
    part(f, 40, 782, 305, 151, "Demand LLC hit / fill",
         ("candidate deadline = 19 + 7", "deadline = 26", "metadata, not property values"), "state")
    part(f, 450, 782, 305, 151, "Private-hit commit refresh",
         ("same line, newer prediction", "16-entry bounded channel", "check age when it arrives"), "state")
    part(f, 860, 782, 300, 151, "Resolve at sequence 27",
         ("unrefreshed bound has passed", "effective state = UNKNOWN", "never infer DEAD from expiry"), "verify")
    f.arrow(((345, 872), (450, 872)), kind="control", label="later accesses",
            label_at=(399, 967), color=PURPLE)
    f.arrow(((755, 872), (860, 872)), kind="control", label="expiry check",
            label_at=(807, 967), color=RED)
    note(f, 1023, "The model's 8-request update latency may outlast this 7-request prediction; an expired update is discarded.")
    note(f, 1057, "An expired update cannot install the stale deadline; a newer coalesced prediction has its own bound.", RED)
    note(f, 1120, "Native request binding, retirement messages and cycle timing for Scale6 are still a separate implementation task.", AMBER)
    return f


def architecture_state_map(root, fx):
    f = plate(
        root, WALK, "02", "architecture-state-map",
        "Where Scale6 metadata lives",
        "Storage ownership, lifetime, and accounting domain are explicit.",
        "An outer memory region contains the CSR offsets, packed Scale6 records "
        "and ordinary property arrays. Preprocessing scratch is temporary. The "
        "runtime model contains bounded commit and prefetch queues plus a "
        "16-record lookahead, and each LLC line holds 35 added bits. The 8 MiB "
        "example totals 4,590,584 bits of added state, not a silicon-area measurement.",
        1200,
    )
    f.section("1", "GRAPH MEMORY AND BUILD-TIME SCRATCH",
              "no runtime P-OPT matrix in the Scale6 candidate", 138, role="data")
    f.rect(40, 194, 720, 232, role="neutral", radius=0)
    f.text(60, 225, "Persistent graph / property memory", size=17, bold=True, color=BLUE)
    f.table(60, 254, 680, 138, 3, role="data")
    for row, text in enumerate(("CSR offsets: row boundaries",
                                "Packed in-edge CSR: 4 bytes per edge, including token",
                                "Property arrays: ordinary algorithm values")):
        f.text(78, 284 + row * 46, text, size=16)
    part(f, 830, 224, 330, 168, "Preprocessing only",
         ("first + next arrays", "about 39.7 MiB for Twitter", "release after construction"), "neutral")
    note(f, 467, "The graph-scaled scratch is not resident hardware. The four-byte record stream remains after construction.")

    f.section("2", "BOUNDED RUNTIME MODEL STATE",
              "native placement and timing still require gem5 work", 525, role="state")
    f.rect(40, 579, 1120, 465, role="neutral", radius=0)
    fifo(f, 64, 653, 290, 16, "Commit: 16 entries")
    fifo(f, 440, 653, 310, 8, "Prefetch: 8 entries", "transfer")
    f.table(860, 610, 275, 130, 3, role="data")
    f.text(876, 639, "Lookahead", size=17, bold=True, color=BLUE)
    f.text(876, 683, "16 x 32-bit records", size=16)
    f.text(876, 726, "512 bits", size=16)
    f.text(64, 739, "1,840 bits", size=16, color=PURPLE)
    f.text(440, 739, "648 bits", size=16, color=AMBER)
    f.text(64, 790, "LLC: 8 MiB data capacity, 131,072 cache lines", size=17, bold=True)
    f.bitfield(64, 822, 1070, 86,
               (("deadline", 32, "state"), ("state", 2, "state"),
                ("origin", 1, "transfer")), total_bits=35)
    f.text(64, 949, "35 bits x 131,072 lines = 4,587,520 bits", size=17, bold=True, color=PURPLE)
    f.text(64, 991, "Queues + lookahead + 64 control bits = 3,064 additional bits", size=16)
    f.arrow(((750, 675), (800, 675), (800, 822)), kind="control",
            label="LLC-only effect", label_at=(954, 781), color=AMBER)

    f.section("3", "REPORT THE RIGHT COST DOMAIN",
              "bit counts are not synthesized area", 1100, role="verify")
    note(f, 1154, "Total added state: 4,590,584 bits, about 560 KiB. Charge this as well as the ordinary LLC data array.", RED)
    return f


def evidence_boundary(root, fx):
    f = plate(
        root, "evaluation-methodology", "01", "evidence-boundary",
        "Cache evidence is not a timing or area result",
        "Scale6 promotion needs matching work, active mechanisms, and a native implementation of the timed path.",
        "The plate separates current cache_sim results from the pending Scale6 "
        "gem5 implementation and physical-cost evidence. Demand misses, prefetch "
        "traffic, writebacks and P-OPT matrix traffic are distinct accounting "
        "terms. Acceptance requires matching PageRank results and bounded active "
        "mechanisms. The single-epoch baseline is labeled as a reconstruction.",
        1240,
    )
    f.section("1", "CURRENT EVIDENCE AND REMAINING WORK",
              "do not transfer a legacy backend's status to Scale6", 138, role="verify")
    tabular(f, 40, 205, (210, 440, 470),
            ("Surface", "What it can establish", "Scale6 status"),
            (
                ("cache_sim", "functional cache behavior and traffic", "implemented; full Twitter exercised"),
                ("gem5 O3", "architectural timing with native requests", "operands run; commit / cache / prefetch pending"),
                ("Sniper", "matched-work modeled corroboration", "Scale6 rows unsupported"),
                ("physical cost", "synthesized storage and control cost", "Scale6 area / timing not established"),
            ), row_height=51)
    note(f, 506, "The existing gem5 extension and RTL cost models are not evidence of a completed Scale6 port.")

    f.section("2", "KEEP THE ACCOUNTING TERMS SEPARATE",
              "all metrics refer to the same graph work and geometry", 568, role="data")
    part(f, 40, 633, 280, 149, "Demand LLC misses",
         ("not all memory transfers", "prefetch can hide a miss", "do not rename it speedup"), "data")
    part(f, 435, 633, 345, 149, "Off-chip traffic",
         ("demand + prefetch reads", "+ dirty writebacks", "+ analytic P-OPT matrix stream"), "transfer")
    part(f, 895, 633, 265, 149, "Metadata cost",
         ("DRAM backing matrix", "reserved LLC capacity", "extra on-chip state / logic"), "state")
    f.arrow(((320, 707), (435, 707)), kind="control", label="add all traffic",
            label_at=(378, 620), color=AMBER)
    f.arrow(((780, 707), (895, 707)), kind="control", label="separate domains",
            label_at=(838, 620), color=PURPLE)
    note(f, 826, "popt_target_time_charged = 0: analytic matrix charging does not model target-time stream latency.", RED)
    note(f, 858, "POPT_SE and POPT_SE_DISTANT are disclosed reconstructions; report both post-final interpretations.", AMBER)

    f.section("3", "FAIL CLOSED BEFORE COMPARING",
              "cache counters alone are not sufficient", 918, role="compute")
    part(f, 40, 977, 290, 139, "Same work",
         ("graph / order / iterations", "edge count + score checksum", "same requested cache geometry"), "data")
    part(f, 452, 977, 295, 139, "Active mechanism",
         ("record width and format", "commit / prefetch / state", "bounded queues and final drain"), "state")
    part(f, 870, 977, 290, 139, "Admissible comparison",
         ("all required policy rows", "no missing/conflicting receipt", "no unsupported timing claim"), "compute")
    f.arrow(((330, 1046), (452, 1046)), kind="control", label="same work",
            label_at=(391, 1149), color=GREEN)
    f.arrow(((747, 1046), (870, 1046)), kind="control", label="active mechanism",
            label_at=(808, 1149), color=GREEN)
    note(f, 1196, "Missing, mismatched or unsupported evidence must fail closed before any comparison is reported.", RED)
    return f


BUILDERS = (
    system_overview, offline_construction, record_formats, future_distance,
    llc_policy, lookahead_prefetch, capacity_accounting, instruction_family,
    o3_pipeline, mshr_lifecycle, checked_walkthrough, architecture_state_map,
    evidence_boundary,
)


def generate(output_root: Path = SOURCE_ROOT) -> list[tuple[Path, Path]]:
    fixture = load_fixture()
    figures = [builder(output_root, fixture) for builder in BUILDERS]
    clean_generated_roots(output_root)
    return [figure.save() for figure in figures]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="Generate privately and compare without changing the working set.")
    args = parser.parse_args()
    if args.check:
        before = {
            path.relative_to(SOURCE_ROOT): path.read_bytes()
            for collection, suffix in (("wiki", "*.svg"), ("wiki_src", "*.drawio"))
            for path in (SOURCE_ROOT / "fig" / collection).rglob(suffix)
        }
        with TemporaryDirectory(prefix="ecg-figure-check-") as temporary:
            root = Path(temporary)
            after = {
                path.relative_to(root): path.read_bytes()
                for pair in generate(root) for path in pair
            }
        if before != after:
            changed = sorted(str(path) for path in before.keys() | after.keys()
                             if before.get(path) != after.get(path))
            raise SystemExit(f"generated figures differ: {changed}")
        return 0
    for svg, drawio in generate():
        print(svg.relative_to(SOURCE_ROOT), drawio.relative_to(SOURCE_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
