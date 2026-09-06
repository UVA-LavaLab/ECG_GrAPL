#!/usr/bin/env python3
"""Generate ECG's example-led architecture plates and Draw.io mirrors."""

from __future__ import annotations

import argparse
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from ecg_figure_lib import (
    AMBER, BLUE, BORDER, GRAY, GREEN, INK, PURPLE, RED, WHITE,
    BLUE_MATTE, GREEN_MATTE, RED_MATTE, NEUTRAL,
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
    metadata_bits: int
    horizon_bits: int
    exponent_bits: int
    mantissa_bits: int
    quantized_code: int
    token: int
    upper: int
    record: int
    property_base: int
    property_address: int
    property_line: int
    id_bits: int
    property_value: float
    property_value_bits: int
    previous_position: int
    previous_vertex: int
    previous_next_position: int
    previous_upper: int

    @property
    def sequence(self) -> int:
        return self.position + 1

    @property
    def deadline(self) -> int:
        return self.sequence + self.upper

    @property
    def id_mask(self) -> int:
        return (1 << self.id_bits) - 1

    @property
    def metadata_mask(self) -> int:
        return self.record & ~self.id_mask

    @property
    def native_operand(self) -> int:
        return self.record

    @property
    def previous_sequence(self) -> int:
        return self.previous_position + 1

    @property
    def previous_deadline(self) -> int:
        return self.previous_sequence + self.previous_upper


def precision_for(metadata_bits: int, horizon_bits: int) -> tuple[int, int]:
    levels = ((1 << metadata_bits) - 2) // (2 * horizon_bits)
    if levels <= 0:
        raise ValueError("metadata cannot encode the required horizon")
    mantissa_bits = levels.bit_length() - 1
    return mantissa_bits, horizon_bits << mantissa_bits


def distance_bounds(distance: int, mantissa_bits: int) -> tuple[int, int]:
    exponent = distance.bit_length() - 1
    base = 1 << exponent
    fraction = distance - base
    mantissa = (
        fraction >> (exponent - mantissa_bits)
        if exponent >= mantissa_bits
        else fraction << (mantissa_bits - exponent)
    )
    code = (exponent << mantissa_bits) | mantissa
    if exponent >= mantissa_bits:
        upper = (mantissa + 1) << (exponent - mantissa_bits)
    else:
        shift = mantissa_bits - exponent
        upper = (mantissa + 1 + (1 << shift) - 1) >> shift
    return code, base + max(1, upper) - 1


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
    base = int(raw["property_base"])
    address = base + dest * element
    id_bits = max(1, (n - 1).bit_length())
    metadata_bits = 32 - id_bits
    horizon_bits = len(stream).bit_length()
    exponent_bits = max(0, (horizon_bits - 1).bit_length())
    mantissa_bits, _codes_per_state = precision_for(
        metadata_bits, horizon_bits)
    quantized_code, upper = distance_bounds(distance, mantissa_bits)
    token = 2 + quantized_code
    record = dest | (token << id_bits)
    if dest <= outer:
        raise ValueError("the illustrated property must not have been updated yet")
    property_value = 1.0 / (n * len(rows[dest]))
    property_value_bits = struct.unpack("<I", struct.pack("<f", property_value))[0]
    previous_position = position - 1
    previous_vertex = stream[previous_position]
    previous_next = next(
        index for index in range(previous_position + 1, len(stream))
        if stream[index] // vertices_per_line == previous_vertex // vertices_per_line
    )
    previous_distance = previous_next - previous_position
    _, previous_upper = distance_bounds(previous_distance, mantissa_bits)
    return CheckedFixture(
        num_vertices=n, mapping=mapping, edges=edges,
        rows=tuple(tuple(row) for row in rows), offsets=tuple(offsets),
        stream=stream, tracked_reader=outer, tracked_dest=dest,
        position=position, next_position=next_position, distance=distance,
        metadata_bits=metadata_bits, horizon_bits=horizon_bits,
        exponent_bits=exponent_bits, mantissa_bits=mantissa_bits,
        quantized_code=quantized_code, token=token, upper=upper, record=record,
        property_base=base, property_address=address,
        property_line=address & ~(line_bytes - 1), id_bits=id_bits,
        property_value=property_value, property_value_bits=property_value_bits,
        previous_position=previous_position, previous_vertex=previous_vertex,
        previous_next_position=previous_next,
        previous_upper=previous_upper,
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


def example_graph(f, fx, x, y, scale=1):
    positions = {
        0: (50, 40), 1: (50, 215), 2: (210, 125),
        3: (375, 20), 4: (375, 245), 5: (550, 65),
        6: (705, 20), 7: (705, 250), 8: (850, 135),
    }
    points = {vertex: (x + px * scale, y + py * scale)
              for vertex, (px, py) in positions.items()}
    for left, right, _weight in fx.edges:
        f.line(points[left], points[right], color=GRAY, width=1.5)
    first, last = points[4], points[7]
    length = math.dist(first, last)
    dx, dy = (last[0] - first[0]) / length, (last[1] - first[1]) / length
    f.arrow(((first[0] + 24 * dx, first[1] + 24 * dy),
             (last[0] - 25 * dx, last[1] - 25 * dy)),
            kind="model-edge", color=BLUE)
    for source, (px, py) in points.items():
        vertex = fx.mapping[source]
        fill = GREEN_MATTE if vertex == fx.tracked_reader else (
            BLUE_MATTE if vertex >= 16 else NEUTRAL)
        stroke = GREEN if vertex == fx.tracked_reader else BLUE
        f.circle(px, py, 23, fill=fill, stroke=stroke)
        f.text(px, py + 6, str(vertex), size=17, bold=True, anchor="middle")


def csr_strip(f, fx, x, y, width, positions, height=86):
    cell = width / len(positions)
    for index, position in enumerate(positions):
        left = x + index * cell
        vertex = fx.stream[position]
        tracked_line = vertex // 16 == fx.tracked_dest // 16
        f.text(left + cell / 2, y - 12, f"j={position}", mono=True,
               anchor="middle", color=BLUE if tracked_line else BORDER)
        f.rect(left, y, cell, height, role="data" if tracked_line else "neutral",
               radius=0, stroke=BLUE if position == fx.position else BORDER)
        f.text(left + cell / 2, y + 31, str(vertex), size=18, bold=True,
               anchor="middle")
        f.text(left + cell / 2, y + 64, "B" if tracked_line else "A",
               anchor="middle", color=BLUE if tracked_line else BORDER)


def example_ribbon(f, fx, y=130):
    f.rect(40, y, 1120, 52, role="data", radius=0)
    f.text(58, y + 32,
           f"Same access: u={fx.tracked_reader} | CSR j={fx.position} | "
           f"request s={fx.sequence} | property v={fx.tracked_dest} | line B",
           size=17, bold=True, max_width=1084)


def system_overview(root, fx):
    f = plate(
        root, "home", "01", "system-overview",
        "ECG: graph knowledge in the edge stream",
        "Graph-derived joint distance state travels with a 4- or 8-byte edge record.",
        "The same graph access is followed from vertex eight and CSR position "
        "eighteen through one adaptive layout, normal property addressing, "
        "retirement transport, and a resident-line update. The property value "
        "is unchanged. A teaching cache contrasts recency with carried future "
        "reuse; wider graph IDs reduce precision and can require eight-byte records.",
        1180,
    )
    f.section("1", "GRAPH -> CSR -> ENCODED MASK",
              "one access; choose a format that fits", 138, role="data")
    f.text(40, 205, "Graph: edge excerpt", size=17, bold=True)
    for x, vertex, fill in ((90, 8, GREEN_MATTE), (280, 18, BLUE_MATTE)):
        f.circle(x, 273, 28, fill=fill, stroke=BLUE)
        f.text(x, 279, str(vertex), size=18, bold=True, anchor="middle")
    f.arrow(((118, 273), (251, 273)), kind="model-edge", color=BLUE)
    f.text(185, 246, "read p[18]", anchor="middle", color=BLUE)
    f.text(40, 338, "outer u=8; property v=18", size=16)
    f.table(420, 214, 280, 130, 3, role="data")
    f.text(435, 243, "Incoming CSR", size=17, bold=True)
    f.text(435, 286, "row_ptr[8:10] = [14,19]", size=16)
    f.text(435, 329, "in_ids[18] = 18", mono=True, color=BLUE)
    part(f, 860, 214, 300, 130, "Adaptive edge record",
         (f"b_ID={fx.id_bits}; M={fx.metadata_bits}; H={fx.horizon_bits}; m={fx.mantissa_bits}",
          f"R = 0x{fx.record:08x}"), "state")
    f.arrow(((308, 273), (420, 273)), kind="control", label="row order",
            label_at=(370, 372), color=GREEN)
    f.arrow(((700, 273), (860, 273)), kind="control", label="encode reuse",
            label_at=(780, 198), color=PURPLE)
    f.arrow(((1010, 344), (1010, 393), (15, 393), (15, 620), (40, 620)),
            kind="transfer", label="read the encoded word", cadence="4 bytes",
            label_at=(562, 380), color=BLUE)
    note(f, 436, "Bit-granular layout: 18/19/20 ID bits leave M=14/13/12 and, at H=31, m=8/7/6.")

    f.section("2", "DECODE -> ADDRESS -> VALUE",
              "the graph and property values do not change", 490, role="compute")
    part(f, 40, 551, 300, 154, "Current-layout decode",
         ("low ID field -> vertex 18", "state + distance -> future",
          "same 4-byte record access"), "compute")
    part(f, 455, 551, 280, 154, "Property load",
         ("base + 18 x 4", "VA = 0x80000048",
          "prediction stays with load"), "data")
    part(f, 875, 551, 285, 154, "L1D / L2 / memory",
         ("ordinary address translation", "ordinary coherent data service",
          "p[18] = 1/128"), "data")
    f.arrow(((340, 585), (455, 585)), kind="dependency", label="vertex",
            label_at=(397, 534), color=BLUE)
    f.arrow(((340, 665), (455, 665)), kind="dependency", label="future",
            label_at=(397, 735), color=PURPLE)
    f.arrow(((735, 603), (875, 603)), kind="transfer", label="load",
            cadence="4 bytes", label_at=(805, 534), color=BLUE)
    f.arrow(((875, 674), (735, 674)), kind="transfer", label="return value",
            cadence="F32", label_at=(805, 735), color=BLUE)

    f.section("3", "CHANGE THE CACHE DECISION",
              "resident metadata, not a property-value rewrite", 806, role="state")
    f.lines(40, 884, ("Recency alone: evict A.",
                      "A was used at 18; B at 19.",
                      "But A is needed at 20,",
                      "before B is needed at 23."), step=32, max_width=350)
    fifo(f, 455, 917, 280, 16, "16-entry update transport")
    f.arrow(((595, 705), (595, 860), (425, 860), (425, 938), (455, 938)), kind="control",
            label="retired prediction", label_at=(744, 785), color=PURPLE)
    part(f, 875, 880, 285, 132, "LLC future ranking",
         ("A: remaining 2 -> score 0", "B: remaining 7 -> score 1",
          "evict B; retain sooner-use A"), "state")
    f.arrow(((735, 938), (875, 938)), kind="control", label="ordered update",
            label_at=(805, 1044), color=PURPLE)
    note(f, 1088, "The cache comparison is a worked two-way example, not a benchmark result. Data bytes stay unchanged.")
    note(f, 1120, "Functional, gem5 and Sniper paths use the same codec and record-window selector.")
    note(f, 1152, "Native evidence is serial fixed-iteration PR; modeled Sniper evidence and physical cost remain separate.", BORDER)
    return f


def offline_construction(root, fx):
    f = plate(
        root, MECHANISM, "01", "offline-construction",
        "From one graph edge to its reuse mask",
        "Vertex identity, CSR position and cache-line identity are different quantities.",
        "An undirected PageRank example has thirty-two vertices with nine "
        "non-isolated vertices shown. Outer vertex eight reads property eighteen "
        "from CSR position eighteen. Positions eighteen and twenty-two touch "
        "the same property line. Their distance four produces joint FINITE "
        "token 16777218, metadata mask 0x20000040 and record 0x20000052.",
        1160,
    )
    f.section("1", "THE GRAPH DEFINES THE ACCESSES",
              "32 vertices; 9 non-isolated vertices shown", 138, role="data")
    example_graph(f, fx, 48, 212, 0.82)
    f.lines(836, 217, (
        "PageRank pull at u=8",
        "Green: current outer vertex",
        "Blue fill: line B properties",
        "p[18] and p[20] share B",
        "4-byte values; 64-byte lines",
        "Weights are not used here",
    ), step=35, bold_first=True, max_width=324)
    note(f, 488, "All labels are the internal vertex IDs used by the CSR. The other 23 vertices are isolated.")

    f.section("2", "CSR MAKES THE ORDER EXPLICIT",
              "in-neighbor rows, visited in increasing outer ID", 548, role="data")
    f.table(40, 616, 230, 132, 3, role="data")
    f.text(55, 645, "row_ptr", size=17, bold=True)
    f.text(55, 689, "row 8 starts at 14", size=16)
    f.text(55, 733, "row 9 starts at 19", size=16)
    f.text(330, 590, "in_ids: row 8 plus the following accesses", size=17, bold=True)
    positions = tuple(range(14, 26))
    csr_strip(f, fx, 330, 636, 830, positions)
    cell = 830 / len(positions)
    first = 330 + (fx.position - 14 + 0.5) * cell
    following = 330 + (fx.next_position - 14 + 0.5) * cell
    f.arrow(((first, 722), (first, 772), (following, 772), (following, 722)),
            kind="dependency", label="next B use: 22 - 18 = 4",
            label_at=((first + following) / 2, 807), color=PURPLE)

    f.section("3", "ENCODE REUSE, PRESERVE THE ID",
              "current layout: b_ID=5, M=27, H=6, m=23", 862, role="state")
    part(f, 40, 921, 300, 127, "Line-reference construction",
         ("current s=19; next use s=23", "distance 4 -> q = 2 << 23"), "compute")
    part(f, 455, 921, 300, 127, f"Mask = 0x{fx.metadata_mask:08x}",
         ("FINITE token = 2 + q", "ID width=5; no action field"), "state")
    part(f, 895, 921, 265, 127, f"R = 0x{fx.record:08x}",
         ("R = vertex 18 | M", "still one 4-byte word"), "data")
    f.arrow(((340, 978), (455, 978)), kind="control", label="encode",
            label_at=(397, 899), color=PURPLE)
    f.arrow(((755, 978), (895, 978)), kind="control", label="OR with ID",
            label_at=(825, 1084), color=BLUE)
    note(f, 1112, "Position j=18 happens to contain vertex 18; j=22 contains it again. A position is not a vertex ID.")
    note(f, 1144, "Preprocessing finishes before the measured traversal. The next plate shows adaptive width selection.")
    return f


def record_formats(root, fx):
    f = plate(
        root, MECHANISM, "02", "record-formats",
        "Choose one adaptive layout for the graph",
        "ID width, record count and requested precision determine every record field.",
        "A record uses the actual maximum encoded vertex ID, not a rounded "
        "format family. Metadata consumes every remaining bit. H is the bit "
        "width of record_count; the largest mantissa m satisfying "
        "2 + 2*H*2^m <= 2^M is selected. Four bytes are tried before the "
        "explicit eight-byte escape unless width is forced.",
        1320,
    )
    f.section("1", "GRAPH SIZE SETS THE BIT BUDGET",
              "b_ID = max(1, ceil(log2 |V|))", 138, role="data")
    tabular(f, 40, 199, (330, 170, 190, 430),
            ("Numeric case", "ID bits", "Metadata M", "Precision at H=31"),
            (("Running fixture (actual max ID 31)", "5", "M=27", "H=6 here; m=23"),
             ("18-bit IDs", "18", "M=14", "m=8"),
             ("19-bit IDs", "19", "M=13", "m=7"),
             ("20-bit IDs", "20", "M=12", "m=6"),
             ("26-bit IDs", "26", "M=6", "m=0")),
            row_height=44)
    note(f, 490, "These are numeric layouts of one method, not public fixed-format families.")

    f.section("2", "THE RUNNING 32-BIT LAYOUT",
              "actual five-bit IDs; all 27 metadata bits are used", 530, role="state")
    for x, label in ((512.5, "[31:5]"), (1065, "[4:0]")):
        f.text(x, 554, label, mono=True, anchor="middle")
    f.bitfield(40, 577, 1120, 88,
               (("joint state/distance token (M=27; H=6, e=3, m=23)", 27, "state"),
                ("vertex", 5, "data")), total_bits=32, minimum_field_width=0)
    note(f, 706, f"q = 2 << 23; FINITE token = 2 + q; mask 0x{fx.metadata_mask:08x}; R = 0x{fx.record:08x}.")
    tabular(f, 40, 747, (245, 245, 300, 330),
            ("Token range", "State", "Payload", "Meaning"),
            (("0", "UNKNOWN", "--", "no finite bound"),
             ("1", "DEAD", "--", "no use in requested horizon"),
             ("2 .. 1+K", "FINITE", "q=(e<<m)|mantissa", "same-traversal upper bound"),
             ("2+K .. 1+2K", "WRAP", "q=(e<<m)|mantissa", "next-traversal upper bound")),
            row_height=44)
    note(f, 990, "K = H x 2^m. Invalid tokens, horizons, addresses and sequences fail closed.")

    f.section("3", "THE EXPLICIT EIGHT-BYTE ESCAPE",
              "width changes; method semantics do not", 1040, role="state")
    f.bitfield(40, 1095, 1120, 80,
               (("joint metadata [63:32]", 32, "state"),
                ("vertex ID [31:0]", 32, "data")),
               total_bits=64, minimum_field_width=0)
    note(f, 1190, "A full 32-bit vertex ID leaves no metadata in a 32-bit word, so the record becomes 8 bytes with M=32.")
    note(f, 1222, "The loader may force 4 or 8 bytes, or try 4 then 8 while enforcing minimum mantissa precision.")
    note(f, 1254, "The 26-ID/M6/H31/m0 case reproduces the old compact bytes only as a numeric configuration.")
    note(f, 1286, "An eight-byte record alone is not evidence of exascale graph-loader support.", BORDER)
    return f


def future_distance(root, fx):
    f = plate(
        root, MECHANISM, "03", "future-distance",
        "Graph-derived metadata sharpens the future bound",
        "The same joint grammar adapts its mantissa precision to the available bit budget.",
        "CSR position seventeen accesses line A and position eighteen accesses "
        "line B. Their next uses are at positions nineteen and twenty-two. "
        "The fixture's M27/H6/m23 layout represents both distances exactly. "
        "A separate H31 distance-one-hundred probe compares numeric metadata "
        "budgets M14, M13, M12 and M6. Passed predictions become "
        "UNKNOWN, not DEAD; the timeline deliberately holds one prediction fixed.",
        1150,
    )
    f.section("1", "FOLLOW CACHE-LINE REUSE",
              "A contains vertices 0..15; B contains 16..31", 138, role="data")
    csr_strip(f, fx, 40, 282, 1120, tuple(range(17, 23)), height=82)
    f.arrow(((60, 282), (60, 207), (433, 207), (433, 282)),
            kind="dependency", label="A: s18 -> s20, distance 2",
            label_at=(247, 192), color=PURPLE)
    f.arrow(((320, 364), (320, 410), (1067, 410), (1067, 364)),
            kind="dependency", label="B: s19 -> s23, true distance = 4",
            label_at=(694, 449), color=PURPLE)

    f.section("2", "QUANTIZE WITHOUT CHANGING THE METHOD",
              "table entries are decoded upper bounds at H=31", 500, role="state")
    rows = []
    for distance, label in ((4, "4 (running distance)"), (100, "100 (probe)")):
        bounds = []
        for metadata in (14, 13, 12, 6):
            mantissa, _ = precision_for(metadata, 31)
            bounds.append(str(distance_bounds(distance, mantissa)[1]))
        rows.append((label, *bounds))
    tabular(f, 40, 568, (220, 225, 225, 225, 225),
            ("True distance", "M14 / m8", "M13 / m7",
             "M12 / m6", "M6 / m0"), rows, row_height=48)
    note(f, 753, "M14, M13 and M12 retain bit-granular precision; M6/H31/m0 is the old compact numeric instance.")
    note(f, 785, "Distance 100 is a precision probe, not another access in the 32-vertex fixture.", BORDER)

    f.section("3", "EXPIRY IS NOT DEATH",
              "hold the s19 prediction fixed for this comparison", 840, role="verify")
    f.text(40, 910, "Fixture", size=17, bold=True, color=GREEN)
    f.rect(180, 880, 400, 42, role="compute", stroke=GREEN, radius=0)
    f.text(380, 908, "FINITE through deadline 23", anchor="middle", color=GREEN)
    f.text(680, 908, "UNKNOWN from 24", color=RED)
    f.text(40, 981, "Coarse M6", size=17, bold=True, color=PURPLE)
    f.rect(180, 951, 700, 42, role="state", stroke=PURPLE, radius=0)
    f.text(530, 979, "FINITE through upper-bound deadline 26", anchor="middle", color=PURPLE)
    f.text(980, 979, "UNKNOWN at 27", color=RED)
    f.line((180, 1022), (1080, 1022), width=2)
    for sequence in (19, 23, 24, 26, 27):
        x = 180 + 100 * (sequence - 19)
        f.line((x, 1012), (x, 1031), width=2)
        f.text(x, 1058, str(sequence), mono=True, anchor="middle")
    f.text(1160, 1058, "semantic s", anchor="end", color=BORDER)
    note(f, 1104, "The actual next B use is at s23 and can refresh its prediction. Only explicit no-future-use knowledge is DEAD.")
    note(f, 1136, "WRAP means next-traversal reuse; it normalizes to FINITE if another pass remains, otherwise DEAD.", BORDER)
    return f


def llc_policy(root, fx):
    f = plate(
        root, MECHANISM, "04", "llc-policy-pipeline",
        "Why the encoded future changes an eviction",
        "A recent line is not necessarily the line needed soonest. The same graph gives a concrete counterexample.",
        "A teaching cache with one set and two ways contains property lines A "
        "and B after semantic request nineteen. A was touched at eighteen and "
        "is needed at twenty; B was touched at nineteen and is needed at "
        "twenty-three. Current-layout deadlines twenty and twenty-three give "
        "remaining distances one and four, hence victim scores zero and one. LRU evicts A "
        "for an incoming scores line C; ECG evicts B. This is a worked algorithm "
        "example, not measured benchmark performance.",
        1310,
    )
    example_ribbon(f, fx)
    f.section("1", "THE CACHE SNAPSHOT AFTER s19",
              "teaching set 0: two resident property ways", 233, role="data")
    tabular(f, 40, 295, (180, 180, 210, 230, 180, 140),
            ("Way / line", "Last touch", "Actual next use", "Decoded deadline", "Remaining", "Score"),
            (("0 / A: p[0..15]", "s18: p[11]", "s20: p[7]", "18 + 2 = 20", "remaining 1", "0"),
             ("1 / B: p[16..31]", "s19: p[18]", "s23: p[18]", "19 + 4 = 23", "remaining 4", "1")),
            row_height=50)
    note(f, 488, "The score is distanceRRPV(remaining) = min(7, floor(log2(remaining)) / 2), rounded down.")
    note(f, 520, "The fixture's M27/H6/m23 layout decodes both reuse distances exactly.")

    f.section("2", "ONE MISS, DIFFERENT VICTIMS",
              "the same new scores-line C needs a way", 579, role="compute")
    for x, width, title in ((40, 520, "Plain IDs + LRU"),
                             (640, 520, "Encoded records + ECG")):
        f.rect(x, 642, width, 240, role="neutral", radius=0)
        f.text(x + 20, 676, title, size=18, bold=True)
    f.text(60, 714, "Oldest touch: A at 18 -> evict A", color=RED)
    f.text(660, 714, "Largest score: B at 1 -> evict B", color=GREEN)
    for x, name, detail, role in (
        (70, "way 0: C", "new scores line", "neutral"),
        (330, "way 1: B", "next use at s23", "data"),
        (670, "way 0: A", "next use at s20", "data"),
        (930, "way 1: C", "new scores line", "neutral"),
    ):
        f.rect(x, 747, 185, 80, role=role, radius=0)
        f.text(x + 92.5, 777, name, size=17, bold=True, anchor="middle")
        f.text(x + 92.5, 806, detail, anchor="middle")
    f.text(60, 854, "Next request p[7] at s20: miss", color=RED)
    f.text(660, 854, "Next request p[7] at s20: hit", color=GREEN)
    note(f, 925, "Only the replacement choice differs. The graph, p[18] = 1/128, and dirty-data handling remain unchanged.")

    f.section("3", "GET FRESH KNOWLEDGE TO THE LLC",
              "private hits still need a metadata route", 979, role="state")
    part(f, 40, 1040, 270, 109, "Retired property load",
         ("line P_B, s19, deadline 23", "retain context and identity"), "compute")
    fifo(f, 450, 1072, 285, 16, "16-entry commit transport")
    part(f, 895, 1040, 265, 109, "Resident metadata",
         ("prediction update only", "no fill or dirtying",
          "RRPV / recency unchanged"), "state")
    f.arrow(((310, 1093), (450, 1093)), kind="control", label="capture",
            label_at=(380, 1024), color=PURPLE)
    f.arrow(((735, 1093), (895, 1093)), kind="control", label="resident update",
            label_at=(815, 1172), color=PURPLE)
    f.text(40, 1180, "Current line payload: 64-bit prediction value + 2-bit state + 1-bit origin.", size=16)
    f.bitfield(40, 1200, 1120, 72,
               (("prediction value", 64, "state"), ("state", 2, "state"),
                ("origin", 1, "transfer")), total_bits=67)
    note(f, 1298, "General victim order: DEAD, non-property, then scored properties. Unknown uses max(RRPV, local GRASP).")
    return f


def lookahead_prefetch(root, fx):
    f = plate(
        root, MECHANISM, "05", "lookahead-prefetch",
        "One real-record window selects a prefetch",
        "Reuse distance ranks candidates; the selected future record supplies the property ID.",
        "The running record's sixteen-word window contains only property lines "
        "A and B. B is current and A first appears at lead one, so the current "
        "uniform selector issues no prefetch. For every graph layout it examines "
        "real records at leads eight through fifteen, keeps the first occurrence "
        "of each distinct line, and ranks finite cyclic bounds before lead-ten "
        "proximity and lower lead. UNKNOWN and DEAD rank as infinity.",
        1340,
    )
    f.section("1", "THE EXAMPLE NEEDS NO PREFETCH",
              "lookahead starts at the same CSR position j18", 138, role="data")
    window = fx.stream[fx.position:fx.position + 16]
    for index, vertex in enumerate(window):
        x = 40 + 70 * index
        f.text(x + 35, 219, "now" if index == 0 else f"+{index}",
               size=16, anchor="middle")
        line = "B" if vertex // 16 == fx.tracked_dest // 16 else "A"
        f.rect(x, 244, 70, 81, role="transfer" if index >= 8 else "neutral",
               radius=0)
        f.text(x + 35, 275, line, size=18, bold=True, anchor="middle")
        f.text(x + 35, 308, str(vertex), size=16, anchor="middle")
    f.line((600, 350), (1158, 350), color=AMBER, width=3)
    f.text(880, 385, "candidate leads 8..15", size=17, bold=True,
           anchor="middle", color=AMBER)
    note(f, 426, "B is the current line. A first appears at +1, before the eligible window. Later A/B entries are not new candidates.")
    f.rect(40, 458, 1120, 51, role="compute", radius=0)
    f.text(600, 490, "Current selector: no eligible new line, so no prefetch request.",
           size=17, bold=True, anchor="middle", color=GREEN)

    f.section("2", "ONE SELECTOR FOR EVERY LAYOUT",
              "no action field and no host future oracle", 566, role="state")
    part(f, 40, 632, 500, 165, "Acquire real record bytes",
         ("16 actual records, not decoded IDs alone",
          "4-byte records span up to 2 lines",
          "unaligned 8-byte records span up to 3 lines"), "data")
    part(f, 660, 632, 500, 165, "Rank eligible distinct lines",
         ("first distinct line at a lead in 8..15",
          "smallest decoded future bound wins",
          "tie: closest to lead 10, then lower lead"), "compute")
    part(f, 415, 871, 370, 130, "Read target ID, then admit",
         ("target = R[j + lead].vertex",
          "reject resident / pending / denied",
          "reuse distance is not the target ID"), "compute")
    f.arrow(((290, 797), (290, 933), (415, 933)), kind="control", label="ready records",
            label_at=(199, 841), color=BLUE)
    f.arrow(((910, 797), (910, 933), (785, 933)), kind="control", label="selected lead",
            label_at=(1009, 841), color=GREEN)

    f.section("3", "PREFETCH FILLS ONLY THE LLC",
              "native path uses acknowledged ReadReq traffic", 1057, role="transfer")
    fifo(f, 40, 1139, 270, 8, "bounded prefetch queues", "transfer")
    f.cylinder(480, 1102, 210, 118, role="data")
    f.lines(503, 1144, ("Memory", "64-byte data line"), bold_first=True, step=30)
    part(f, 890, 1107, 270, 111, "LLC fill",
         ("no L1D / L2 allocation", "count reads and writebacks"), "data")
    f.arrow(((600, 1001), (600, 1030), (15, 1030), (15, 1160), (40, 1160)),
            kind="control", label="accepted target", label_at=(217, 1017), color=AMBER)
    f.arrow(((310, 1160), (480, 1160)), kind="control", label="read request",
            label_at=(395, 1237), color=AMBER)
    f.arrow(((690, 1160), (890, 1160)), kind="transfer", label="prefetch fill",
            cadence="one line", label_at=(790, 1258), color=AMBER)
    note(f, 1294, "Default native model: 12-cycle lookup + 8-cycle prefetch pipeline; one lookup input/cycle. FlowThrough is OFF.")
    note(f, 1326, "Issue and completion check L1/L2/LLC duplicates and shared admission; all traffic and drain time are charged.", BORDER)
    return f


def capacity_accounting(root, fx):
    f = plate(
        root, MECHANISM, "06", "capacity-accounting",
        "Graph-sized matrices and cache-sized state",
        "P-OPT reserves active columns; ECG uses the edge word and additional per-cache-line state.",
        "For full Twitter, one one-byte-per-property-line column is 2,603,265 "
        "bytes. Full P-OPT keeps two columns and SE keeps one, while both retain "
        "256 backing columns. Sixteen-way bars show the full and single-epoch "
        "reservations at 8, 16 and 24 MiB. Current ECG stores a 64-bit "
        "prediction value, two state bits and one origin bit per resident line. "
        "Neither logical model count is a silicon-area ratio.",
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
    note(f, 933, "SE uses a distinct historical baseline encoding. A two-way undercharge of full P-OPT is not SE.", RED)

    f.section("3", "ECG KEEPS THE DATA WAYS",
              "prediction state is additional storage", 976, role="verify")
    note(f, 1032, "Current prediction payload: 67 bits/line, separate from tags, data, classification, recency, queues and ports.")
    note(f, 1064, "8 MiB: 8,781,824 bits = 1,097,728 bytes. 24 MiB: 26,345,472 bits = 3,293,184 bytes.")
    note(f, 1097, "These are payload totals, not silicon area. P-OPT backing matrices, active columns and reserved ways remain distinct.", BORDER)
    return f


def instruction_family(root, fx):
    f = plate(
        root, ISA, "01", "instruction-family",
        "Two native loads, two different results",
        "The record produces a renamed integer operand; the dependent property load produces the unchanged F32 value.",
        "The native example uses the graph-derived 5-ID/27-metadata layout. "
        "Record load I0 reads four real bytes and writes raw word "
        "0x0000000020000052 to illustrative register P17. Property load I1 "
        "takes property base, raw word and the real record address as separate "
        "sources, derives semantic sequence nineteen, and returns 1/128. Its "
        "prediction stays on the dynamic instruction for retirement.",
        1270,
    )
    example_ribbon(f, fx)
    f.section("1", "CONFIGURE THE RECORD CONTEXT",
              "custom-1 opcode 0x2b; graph-derived layout descriptor", 238, role="compute")
    tabular(f, 40, 300, (360, 400, 360),
            ("Configuration", "Running example", "Role"),
            (("CSRs 0x803 / 0x801", "record base R; context 1", "address and identity"),
             ("CSRs 0x805..0x80B", "layout/count/V/Pbase/base/control/gen", "checked configuration + pending query")),
            row_height=47)
    note(f, 480, "Configure funct3=2 and pending-query funct3=3 are non-speculative; generation is an identity, not a method version.")

    f.section("2", "I0: READ AND ASSEMBLE THE OPERAND",
              "custom-1 / funct3 0 / funct7 0 for 4 B, 1 for 8 B", 535, role="data")
    f.table(40, 598, 310, 160, 3, role="data")
    f.text(55, 628, "Record array in memory", size=17, bold=True)
    f.text(55, 680, "RA = 0x60000000 + 4 x 18", mono=True)
    f.text(55, 734, f"Mem[RA] = 0x{fx.record:08x}", mono=True)
    part(f, 480, 598, 320, 160, "Record result assembly",
         ("validate RA, count and layout",
          "load raw32 or raw64",
          "sequence is tracked separately"), "compute")
    f.rect(930, 598, 230, 160, role="state", radius=0)
    f.text(945, 627, "P17: 64-bit operand", size=17, bold=True)
    f.text(945, 683, f"0x{fx.native_operand:016x}", mono=True)
    f.text(945, 728, "zero-extended raw record", size=16)
    f.arrow(((350, 674), (480, 674)), kind="transfer", label="raw word",
            cadence="4 bytes", label_at=(415, 579), color=BLUE)
    f.arrow(((800, 674), (930, 674)), kind="dependency", label="rename result",
            label_at=(865, 792), color=PURPLE)

    f.section("3", "I1: KEEP VALUE AND HINT SEPARATE",
              "custom-1 / funct3 1 / FUNCT2 0; three explicit sources", 849, role="compute")
    part(f, 40, 912, 325, 182, "Consume renamed P17",
         ("low 5 bits -> v=18", "FINITE token -> distance 4",
          "propertyBase + rawWord + realRA", "deadline = s19 + 4 = 23"), "compute")
    part(f, 475, 912, 330, 182, "Ordinary data service",
         ("VA 0x80000048 -> translated PA",
          "L1D / L2 / LLC as required",
          "read the contribution, not the mask",
          "p[18] is still 1/128"), "data")
    part(f, 915, 912, 245, 80, "F9: unchanged value",
         (f"F32 bits 0x{fx.property_value_bits:08X}",), "data")
    part(f, 915, 1030, 245, 122, "DynInst hint for I1",
         ("v18, s19, deadline 23",
          "ctx1; own translated PA",
          "export only at retirement"), "state")
    f.arrow(((1160, 674), (1183, 674), (1183, 819), (15, 819), (15, 1001), (40, 1001)),
            kind="dependency", label="I1 waits for its own P17", label_at=(600, 807), color=PURPLE)
    f.arrow(((365, 1001), (475, 1001)), kind="transfer", label="property address",
            cadence="one load", label_at=(420, 891), color=BLUE)
    f.arrow(((805, 952), (915, 952)), kind="transfer", label="F32 value",
            cadence="4 bytes", label_at=(860, 891), color=BLUE)
    f.arrow(((202, 1094), (202, 1190), (1037, 1190), (1037, 1152)),
            kind="dependency", label="prediction and dynamic association",
            label_at=(620, 1226), color=PURPLE)
    note(f, 1256, "P17/F9 are illustrative rename tags. This is an experimental custom-1 extension, not a ratified RISC-V ISA.")
    return f


def o3_pipeline(root, fx):
    f = plate(
        root, ISA, "02", "o3-request-pipeline",
        "The mask follows the load through the core",
        "Blue carries addresses and values; purple carries dependencies and reuse state; green controls execution.",
        "A detailed native RISC-V O3 datapath follows the same record and "
        "property loads. Fetch, decode, rename and dispatch feed an issue queue. "
        "I1 waits for I0's renamed P17 operand. Both use the AGU, LSQ, address "
        "translation and private caches; returned bytes are assembled or written "
        "back to the appropriate register. Per-instruction hints stay in the "
        "ROB. Private misses expose only observations at the LLC, while a separate "
        "bounded retirement channel can update resident metadata even for "
        "private hits. No host-side future lookup supplies the operand.",
        1540,
    )
    example_ribbon(f, fx)
    f.text(40, 212, f"Native I0 reads raw 0x{fx.record:08x}; I1 returns p[18] = 1/128. P_B denotes its translated physical line.",
           size=16, max_width=1120)
    f.rect(40, 235, 745, 855, role="neutral", radius=0)
    f.text(60, 268, "RISC-V O3 core: two dependent loads", size=18, bold=True)
    f.text(65, 291, "instruction flow", color=GREEN)
    for x, width, title in ((65, 150, "Fetch"), (265, 150, "Decode"),
                            (465, 150, "Rename"), (645, 110, "Dispatch")):
        part(f, x, 305, width, 60, title, role="compute")
    for first, last in ((215, 265), (415, 465), (615, 645)):
        f.arrow(((first, 335), (last, 335)), kind="control",
                label="instruction flow", color=GREEN)
    f.arrow(((700, 365), (700, 405), (205, 405), (205, 440)),
            kind="control", label="dispatch I0 / I1", label_at=(420, 390), color=GREEN)

    f.table(65, 440, 280, 160, 3, role="compute")
    f.text(80, 470, "Issue / select", size=17, bold=True)
    f.text(80, 521, "I0: record address ready", size=16)
    f.text(80, 575, "I1: wait for P17", size=16, color=PURPLE)
    f.table(465, 440, 290, 160, 3, role="state")
    f.text(480, 470, "Physical registers", size=17, bold=True)
    f.text(480, 521, f"P17 = 0x{fx.native_operand:016x}", size=16)
    f.text(480, 575, "F9 = 1/128 (F32 0x3C000000)", size=16)

    part(f, 65, 661, 280, 134, "AGU / payload decode",
         ("I0: RA=R+4j; raw word", "I1: Pbase + raw + real RA",
          "I1 hint: s19, deadline 23"), "compute")
    part(f, 455, 661, 300, 134, "LSQ + translation",
         ("ordering, replay and faults",
          "VA 0x80000048 -> P_B+8",
          "I1 observation: s19, ctx1"), "data")
    f.arrow(((205, 600), (205, 661)), kind="control", label="issue",
            label_at=(150, 639), color=GREEN)
    f.arrow(((465, 521), (405, 521), (405, 699), (345, 699)),
            kind="dependency", label="P17 dependency",
            label_at=(430, 633), label_anchor="start", color=PURPLE)
    f.arrow(((345, 729), (455, 729)), kind="transfer", label="load request",
            cadence="I0 / I1", label_at=(370, 824), color=BLUE)
    f.arrow(((600, 661), (600, 600)), kind="transfer",
            label="writeback", cadence="per load", color=BLUE)
    f.text(625, 623, "writeback", color=BLUE)
    f.text(625, 647, "per load", color=BLUE)

    part(f, 900, 620, 260, 175, "Private data hierarchy",
         ("record and property loads", "L1D / L2 hit: return value",
          "miss: fetch a cache line", "no automatic LLC refresh"), "data")
    f.arrow(((755, 710), (900, 710)), kind="transfer",
            label="request", cadence="4 bytes", color=BLUE)
    f.arrow(((900, 747), (755, 747)), kind="transfer",
            label="response", cadence="4 bytes", color=BLUE)
    f.text(826, 666, "4 bytes", anchor="middle", color=BLUE)
    f.text(826, 687, "request", anchor="middle", color=BLUE)
    f.text(826, 780, "response", anchor="middle", color=BLUE)
    f.lines(886, 457, ("I0: real record bytes", "I1: real property bytes",
                       "Payload decode is not", "frontend opcode decode."),
            step=29, max_width=274)

    tabular(f, 65, 853, (120, 130, 440),
            ("ROB", "Complete?", "Retained dynamic state"),
            (("I0", "P17 ready", "RECORD: own position s19"),
             ("I1", "F9 ready", "PROPERTY: s19, D23, ctx1, own PA")),
            row_height=44)
    f.arrow(((600, 795), (600, 853)), kind="control", label="load completed",
            label_at=(671, 831), color=GREEN)
    part(f, 65, 1010, 690, 58,
         "Commit: oldest completed, non-squashed instruction", role="compute")
    f.arrow(((210, 985), (210, 1010)), kind="control",
            label="Commit", color=GREEN)

    f.rect(875, 910, 305, 505, role="neutral", radius=0)
    f.text(895, 942, "Shared LLC", size=18, bold=True)
    part(f, 900, 970, 255, 140, "Data and ordinary tags",
         ("physical line P_B", "p[18] = 1/128 (unchanged)",
          "fills / dirty writebacks"), "data")
    part(f, 900, 1260, 255, 125, "Replacement metadata",
         ("observe: PENDING s19", "commit: FINITE, D23",
          "non-touching tag lookup"), "state")
    f.arrow(((1010, 795), (1010, 970)), kind="transfer",
            label="miss", cadence="64 bytes", color=BLUE)
    f.arrow(((1120, 970), (1120, 795)), kind="transfer",
            label="fill", cadence="64 bytes", color=BLUE)
    f.text(972, 859, "miss", anchor="end", color=BLUE)
    f.text(1140, 859, "fill", color=BLUE)
    f.text(1065, 887, "64 bytes", anchor="middle", color=BLUE)
    f.arrow(((1035, 1110), (1035, 1260)), kind="control",
            label="I1: observe s19", color=PURPLE)
    f.text(902, 1160, "I1: observe s19", color=PURPLE)
    f.text(902, 1187, "not D23", color=PURPLE)

    f.rect(40, 1137, 745, 195, role="state", radius=0)
    f.text(60, 1170, "Retirement metadata transport", size=18, bold=True, color=PURPLE)
    f.text(65, 1201, "{P_B, s19, D23, ctx1}", mono=True)
    fifo(f, 65, 1240, 420, 16, "16 physical message slots")
    f.lines(525, 1202, ("capture: commitWidth (8)",
                       "minimum delay: 8 cycles",
                       "output: 1 update / cycle",
                       "oldest version protected"),
            step=29, max_width=235)
    f.arrow(((405, 1068), (405, 1137)), kind="control",
            label="successful retirement", label_at=(614, 1121), color=PURPLE)
    f.arrow(((785, 1298), (900, 1298)), kind="control",
            label="update", color=PURPLE)
    f.text(843, 1357, "update", anchor="middle", color=PURPLE)
    f.text(65, 1393, "Receiver position advances only when a timed update arrives.", size=16)
    note(f, 1461, "A private hit can bypass LLC data traffic, but still exports its own committed prediction. Squashed loads export none.")
    note(f, 1493, "Observations never install FINITE/DEAD. Metadata updates never allocate or alter a data line.")
    note(f, 1525, "Native prefetch uses separate acknowledged traffic and paid presence ports; physical-area qualification remains separate.", BORDER)
    return f


def mshr_lifecycle(root, fx):
    f = plate(
        root, ISA, "03", "mshr-metadata-lifecycle",
        "Completion is not permission to install a prediction",
        "Separate request observation, architectural retirement and timed metadata delivery.",
        "For the same property line B, a request observation installs a pending "
        "sequence but no future prediction. A completed load can still be "
        "squashed. Retirement alone authorizes a delayed update. A newer pending "
        "observation at sequence twenty-three rejects an older committed update "
        "at nineteen; a coalesced update at twenty-five can install its newer "
        "prediction. The final table distinguishes CPU ready cycles from semantic "
        "request positions and protects the oldest of two physical same-line slots.",
        1430,
    )
    example_ribbon(f, fx)
    f.section("1", "THREE EVENTS, THREE PERMISSIONS",
              "top row shows the private-miss case", 236, role="compute")
    part(f, 40, 301, 300, 151, "Observed at the LLC",
         ("I1 request carries s19 / ctx1",
          "set PENDING, value=19",
          "do not install deadline 23"), "state")
    part(f, 450, 301, 300, 151, "Data completes",
         ("F9 receives 1/128",
          "I1 may still be speculative",
          "no commit update yet"), "data")
    part(f, 860, 301, 300, 151, "I1 retires",
         ("its own PA and captured hint",
          "enqueue {P_B,s19,D23,ctx1}",
          "start the transport latency"), "compute")
    f.arrow(((340, 376), (450, 376)), kind="control", label="completion",
            label_at=(395, 280), color=BLUE)
    f.arrow(((750, 376), (860, 376)), kind="control", label="ROB permission",
            label_at=(805, 486), color=GREEN)
    part(f, 455, 510, 290, 87, "Squashed / faulted",
         ("discard; do not enqueue",), "verify")
    f.arrow(((600, 452), (600, 510)), kind="control", label="squash",
            label_at=(706, 492), color=RED)
    note(f, 638, "A private hit skips the LLC observation, not retirement. MSHRs merge observations; they do not commit predictions.")

    f.section("2", "A NEWER OBSERVATION IS A GUARD",
              "same physical line P_B; other lines omitted", 699, role="state")
    part(f, 40, 765, 300, 147, "Pending observation s23",
         ("state=PENDING; value=23",
          "replacement treats it as UNKNOWN",
          "no free watermark advance"), "state")
    part(f, 450, 765, 300, 147, "Delivered update s19",
         ("19 is older than pending 23",
          "count STALE; retain PENDING",
          "received watermark can advance"), "verify")
    part(f, 860, 765, 300, 147, "Delivered update s25",
         ("25 is newer than pending 23",
          "install FINITE prediction",
          "data / recency unchanged"), "state")
    f.arrow(((340, 833), (450, 833)), kind="control", label="reject older",
            label_at=(395, 951), color=RED)
    f.arrow(((750, 833), (860, 833)), kind="control", label="accept newer",
            label_at=(805, 951), color=GREEN)

    f.section("3", "COALESCE WITHOUT STARVING THE OLDEST",
              "queue-only timing illustration; not an O3 trace", 1015, role="state")
    tabular(f, 40, 1074, (135, 350, 315, 320),
            ("CPU cycle", "Same-line event", "Physical slot 0", "Physical slot 1"),
            (("100", "enqueue s19", "s19, ready 108", "empty"),
             ("102", "enqueue s23", "s19, ready 108", "s23, ready 110"),
             ("104", "coalesce secondary with s25", "s19, ready 108", "s25, ready 112"),
             ("108", "deliver oldest s19", "empty", "s25 is now protected"),
             ("112", "deliver s25", "empty", "empty")),
            row_height=44)
    note(f, 1377, "Two same-line versions consume two of the 16 slots. A replacement secondary gets its own full latency.")
    note(f, 1409, "Output is at most one update per CPU cycle; absent lines never allocate data for a metadata message.", BORDER)
    return f


def checked_walkthrough(root, fx):
    f = plate(
        root, WALK, "01", "checked-request",
        "One edge, one adaptive record, unchanged data",
        "The mask occupies unused edge-ID bits. It is not applied to the floating-point property value.",
        "The running graph needs five vertex bits. Vertex eighteen is ORed with "
        "joint metadata mask 0x20000040 to form record 0x20000052. The "
        "layout uses M=27, H=6 and m=23, recovers vertex eighteen and address "
        "0x80000048, and returns the exact initial contribution 1/128 with F32 "
        "bits 0x3C000000. The decoded distance is the upper bound four, giving "
        "deadline twenty-three.",
        1330,
    )
    example_ribbon(f, fx)
    f.section("1", "BUILD THE METADATA MASK",
              "current layout: b_ID=5, M=27, H=6, m=23", 236, role="state")
    part(f, 40, 309, 390, 138, "Ordinary edge ID",
         ("v = 18 = 0x00000012", "ID extraction mask = 0x0000001F"), "data")
    part(f, 40, 495, 390, 138, f"Metadata mask = 0x{fx.metadata_mask:08x}",
         ("q = 2 << 23 = 16777216",
          "FINITE token = 2 + q"), "state")
    f.circle(610, 472, 36, fill=GREEN_MATTE, stroke=GREEN)
    f.text(610, 479, "OR", size=18, bold=True, anchor="middle")
    part(f, 860, 411, 300, 123, "Packed record R",
         (f"0x{fx.record:08x}", "one 32-bit word, not a sidecar"), "data")
    f.arrow(((430, 378), (520, 378), (520, 454), (579, 454)),
            kind="dependency", label="preserve vertex", label_at=(596, 344), color=BLUE)
    f.arrow(((430, 564), (520, 564), (520, 490), (579, 490)),
            kind="dependency", label="insert metadata", label_at=(597, 611), color=PURPLE)
    f.arrow(((646, 472), (860, 472)), kind="control", label="R = v | M",
            label_at=(753, 389), color=GREEN)
    note(f, 679, "The word changes from 0x00000012 to 0x20000052; masking with 0x1F still recovers vertex 18.")

    f.section("2", "DECODE THE SELECTED LAYOUT",
              "one grammar; graph-derived precision", 736, role="compute")
    tabular(f, 40, 799, (260, 260, 280, 320),
            ("Field", "Value", "Check", "Result"),
            (("destination", "18", "R & 0x1F", "property vertex 18"),
             ("joint token", "16777218", "2 + q; q=2<<23", "FINITE, distance <=4")),
            row_height=48)

    f.section("3", "THE PROPERTY LOAD IS UNCHANGED",
              "line B contains p[16..31]; p[18] is byte offset 8", 1004, role="data")
    part(f, 40, 1066, 320, 137, "VA = 0x80000048",
         ("0x80000000 + 18 x 4",
          "virtual line 0x80000040",
          "translate to physical line P_B"), "data")
    part(f, 475, 1066, 300, 137, "Ordinary memory hierarchy",
         ("read the addressed F32 value",
          "no property-value masking",
          "same coherent data semantics"), "data")
    part(f, 910, 1066, 250, 137, "F32 value",
         ("p[18] = 1/128",
          f"bits 0x{fx.property_value_bits:08X}",
          "not the encoded edge word"), "data")
    f.arrow(((360, 1135), (475, 1135)), kind="transfer", label="load",
            cadence="4 bytes", label_at=(417, 1048), color=BLUE)
    f.arrow(((775, 1135), (910, 1135)), kind="transfer", label="return",
            cadence="4 bytes", label_at=(843, 1236), color=BLUE)
    note(f, 1279, "The different future bounds feed cache ranking; the graph index, address and algorithm data remain the same.")
    note(f, 1311, "Native P17 is raw 0x0000000020000052. Sequence s19 is checked separately from the real record address.", BORDER)
    return f


def architecture_state_map(root, fx):
    f = plate(
        root, WALK, "02", "architecture-state-map",
        "Where the mask and its decoded state live",
        "Edge storage, per-instruction association and per-cache-line predictions have different lifetimes.",
        "The encoded edge record and ordinary property data reside in separate "
        "graph-memory arrays. Current builders retain the source graph, allocate "
        "a vector<uint32_t> or vector<uint64_t> carrier, and use a sparse "
        "line-indexed first/next map rather than edge-sized action arrays. "
        "Native per-instruction state is bounded by the CPU window. Resident "
        "prediction payload is 67 logical bits per cache line.",
        1430,
    )
    f.section("1", "GRAPH MEMORY AND BUILD-TIME SCRATCH",
              "no runtime P-OPT rereference matrix", 138, role="data")
    f.rect(40, 194, 720, 232, role="neutral", radius=0)
    f.text(60, 225, "Graph memory: records are not property values", size=17, bold=True, color=BLUE)
    f.table(60, 254, 680, 138, 3, role="data")
    for row, text in enumerate(("CSR offsets: row boundaries are unchanged",
                                "R[j]: 4-byte encoded edge record replaces the ID read",
                                "p[v]: ordinary F32 algorithm data, not an encoded mask")):
        f.text(78, 284 + row * 46, text, size=16)
    part(f, 830, 224, 330, 168, "Builder scratch",
         ("sparse line first / next map",
          "no E-sized action/future arrays",
          "source graph remains allocated",
          "temporary, not silicon state"), "neutral")
    note(f, 467, "Receipts separate source_stream_bytes, retained_source_bytes, carrier payload/allocation and auxiliary peak.")
    f.rect(40, 499, 1120, 115, role="compute", radius=0)
    f.text(60, 530, "Per-in-flight native access, not per graph edge", size=17, bold=True)
    f.text(60, 574, "P17: raw 32/64-bit record operand", size=16)
    f.text(660, 574, "I1: captured hint + its translated address", size=16)

    f.section("2", "BOUNDED CONTROL AND LINE STATE",
              "D is the stored semantic-deadline width", 672, role="state")
    f.rect(40, 728, 1120, 504, role="neutral", radius=0)
    f.arrow(((600, 614), (600, 728)), kind="control", label="committed information",
            label_at=(768, 716), color=PURPLE)
    fifo(f, 64, 807, 290, 16, "Commit: 16 entries")
    fifo(f, 440, 807, 310, 8, "Bounded trigger/property queues", "transfer")
    f.table(860, 759, 275, 130, 3, role="data")
    f.text(876, 789, "Sniper guest window", size=17, bold=True)
    f.text(876, 832, "16 x uint64 = 1024 data bits", size=16)
    f.text(876, 875, "+ 1024 index + 16 valid bits", size=16)
    f.text(64, 909, "semantic sequence/deadline: checked 64-bit", size=16, color=PURPLE)
    f.text(440, 909, "bounded queues and paid ports", size=16, color=AMBER)
    f.text(64, 966, "Current payload: 67 bits/line; 64-bit prediction value", size=17, bold=True)
    f.text(630, 966, "Separate from baseline cache structures", size=17, bold=True)
    f.bitfield(64, 988, 1071, 76,
               (("prediction value", 64, "state"), ("state", 2, "state"),
                ("origin", 1, "transfer")), total_bits=67)
    tabular(f, 64, 1100, (190, 210, 340, 330),
            ("LLC", "Lines", "Prediction payload", "Bytes"),
            (("8 MiB", "131,072", "8,781,824 bits", "1,097,728"),
             ("24 MiB", "393,216", "26,345,472 bits", "3,293,184")),
            row_height=38)

    f.section("3", "DO NOT TURN STATE INTO AN AREA CLAIM",
              "payload count only; queues and ports are additional", 1286, role="verify")
    note(f, 1350, "Baseline tags, data, property/tier classification, recency and RRPV remain separate.")
    note(f, 1382, "Runtime word bank is 16 x 257 bits; native 4/8-byte acquisition pins 2/3 real line banks.", BORDER)
    note(f, 1414, "These logical bits are not a synthesized area, energy or timing result.", BORDER)
    return f


def evidence_boundary(root, fx):
    f = plate(
        root, "evaluation-methodology", "01", "evidence-boundary",
        "What each implementation can establish",
        "Separate encoding, cache behavior, native execution and physical cost before making a claim.",
        "A support matrix distinguishes one graph-adaptive codec from backend "
        "evidence. Functional and native replacement/prefetch paths are working. "
        "Sniper's four mechanisms pass strict admission but remain modeled corroboration. "
        "Demand misses, total traffic and runtime remain distinct, and physical "
        "qualification is not implied. Historical P-OPT-SE stays a disclosed reconstruction.",
        1270,
    )
    f.section("1", "FORMAT SUPPORT IS NOT BACKEND SUPPORT",
              "current implementation, not an aspirational diagram", 138, role="verify")
    tabular(f, 40, 201, (240, 235, 335, 310),
            ("Surface", "Current record", "Implemented mechanism", "Evidence limit"),
            (("cache_sim", "graph-derived 4/8 B", "admitted T / R / P / R+P", "both widths + exact 24 MiB"),
             ("gem5 / RV64 O3", "raw32 / raw64", "real loads, retirement, R / P", "serial fixed-iteration PR"),
             ("Sniper", "actual 4/8 B loads", "admitted T / R / P / R+P", "modeled corroboration only"),
             ("RTL / physical cost", "67-bit line payload", "earlier components only", "no complete silicon result"),
             ("Historical results", "old fixed encodings", "preserved provenance", "not relabeled as current")),
            row_height=49)
    note(f, 538, "Sniper: one-core sg_kernel, bounded RSS, modulo LLC; completion link is not native retirement.")

    f.section("2", "COUNT TRAFFIC, THEN INTERPRET IT",
              "a demand-miss count is not a speedup result", 601, role="data")
    part(f, 40, 665, 310, 180, "Off-chip transfer accounting",
         ("demand line reads", "prefetch line reads",
          "dirty writebacks", "baseline matrix-stream charge"), "transfer")
    part(f, 510, 665, 650, 79, "Fewer demand misses may still mean more traffic",
         ("A prefetch can replace a demand miss with another memory read.",), "data")
    part(f, 510, 787, 650, 79, "Cache behavior is not runtime or silicon area",
         ("Native time and physical cost need their own complete implementation.",), "state")
    f.arrow(((350, 707), (510, 707)), kind="control", label="all reads / writes",
            label_at=(430, 644), color=AMBER)
    f.arrow(((350, 819), (510, 819)), kind="control", label="separate metrics",
            label_at=(430, 908), color=PURPLE)

    f.section("3", "MATCH WORK AND ACTIVE MECHANISMS",
              "fail closed rather than compare unlike runs", 971, role="compute")
    part(f, 40, 1032, 300, 126, "Same computation",
         ("graph / order / iterations",
          "edge count + score checksum",
          "same cache geometry"), "data")
    part(f, 450, 1032, 300, 126, "Active requested path",
         ("selected format and policy",
          "bounded queues / traffic",
          "complete final receipts"), "state")
    part(f, 860, 1032, 300, 126, "Scoped conclusion",
         ("separate R / P / R+P",
          "charge baseline overhead",
          "no unsupported timing rows"), "compute")
    f.arrow(((340, 1095), (450, 1095)), kind="control", label="same work",
            label_at=(395, 1011), color=GREEN)
    f.arrow(((750, 1095), (860, 1095)), kind="control", label="active path",
            label_at=(805, 1011), color=GREEN)
    note(f, 1205, "popt_target_time_charged = 0: matrix traffic is charged analytically, not as target-time stream latency.")
    note(f, 1237, "POPT_SE and POPT_SE_DISTANT are paper-constrained reconstructions; keep both interpretations visible.", BORDER)
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
