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
    token: int
    upper: int
    record: int
    property_base: int
    property_address: int
    property_line: int
    id_bits: int
    full_code: int
    full_upper: int
    full_record: int
    property_value: float
    property_value_bits: int
    previous_position: int
    previous_vertex: int
    previous_next_position: int
    previous_full_upper: int
    previous_scale_upper: int

    @property
    def sequence(self) -> int:
        return self.position + 1

    @property
    def deadline(self) -> int:
        return self.sequence + self.upper

    @property
    def full_deadline(self) -> int:
        return self.sequence + self.full_upper

    @property
    def id_mask(self) -> int:
        return (1 << self.id_bits) - 1

    @property
    def full_mask(self) -> int:
        return self.full_record & ~self.id_mask

    @property
    def native_operand(self) -> int:
        return (self.sequence << 32) | self.record

    @property
    def previous_sequence(self) -> int:
        return self.previous_position + 1

    @property
    def previous_deadline(self) -> int:
        return self.previous_sequence + self.previous_scale_upper


def full_distance_bounds(distance: int, reference_bits: int = 8) -> tuple[int, int]:
    exponent = distance.bit_length() - 1
    base = 1 << exponent
    levels = 1 << (reference_bits - 5)
    mantissa = (distance - base) * levels // base
    upper = base + max(1, ((mantissa + 1) * base + levels - 1) // levels) - 1
    return exponent * levels + mantissa, min(upper, 0x7FFFFFFF)


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
    id_bits = max(1, (n - 1).bit_length())
    full_code, full_upper = full_distance_bounds(distance)
    full_record = dest | (full_code << id_bits) | (1 << (id_bits + 8))
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
    _, previous_full_upper = full_distance_bounds(previous_distance)
    previous_scale_upper = (1 << previous_distance.bit_length()) - 1
    return CheckedFixture(
        num_vertices=n, mapping=mapping, edges=edges,
        rows=tuple(tuple(row) for row in rows), offsets=tuple(offsets),
        stream=stream, tracked_reader=outer, tracked_dest=dest,
        position=position, next_position=next_position, distance=distance,
        token=token, upper=upper, record=(token << 26) | dest,
        property_base=base, property_address=address,
        property_line=address & ~(line_bytes - 1), id_bits=id_bits,
        full_code=full_code, full_upper=full_upper, full_record=full_record,
        property_value=property_value, property_value_bits=property_value_bits,
        previous_position=previous_position, previous_vertex=previous_vertex,
        previous_next_position=previous_next,
        previous_full_upper=previous_full_upper,
        previous_scale_upper=previous_scale_upper,
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
        "Reuse information travels with an ordinary-width edge record, then guides a cache decision.",
        "The same graph access is followed from vertex eight and CSR position "
        "eighteen to a richer Full14 mask or a compact Scale6 token, normal "
        "property addressing, and a resident-line update. The property value "
        "is unchanged. A teaching cache contrasts recency with carried future "
        "reuse. Scale6 is the large-ID format, not the complete ECG design.",
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
    part(f, 860, 214, 300, 130, "32-bit edge record",
         (f"Full14  0x{fx.full_record:08x}",
          f"Scale6  0x{fx.record:08x}"), "state")
    f.arrow(((308, 273), (420, 273)), kind="control", label="row order",
            label_at=(370, 372), color=GREEN)
    f.arrow(((700, 273), (860, 273)), kind="control", label="encode reuse",
            label_at=(780, 198), color=PURPLE)
    f.arrow(((1010, 344), (1010, 393), (15, 393), (15, 620), (40, 620)),
            kind="transfer", label="read the encoded word", cadence="4 bytes",
            label_at=(562, 380), color=BLUE)
    note(f, 436, "Small IDs leave more metadata space: Full14 uses 14 bits; Twitter's 26-bit IDs leave 6 for Scale6.")

    f.section("2", "DECODE -> ADDRESS -> VALUE",
              "the graph and property values do not change", 490, role="compute")
    part(f, 40, 551, 300, 154, "Selected-format decode",
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
    note(f, 1120, "cache_sim implements both formats; the native RISC-V record/property pair currently uses Scale6.")
    note(f, 1152, "Prefetch is a separate path. Its native implementation and full timing/area qualification remain open.", BORDER)
    return f


def offline_construction(root, fx):
    f = plate(
        root, MECHANISM, "01", "offline-construction",
        "From one graph edge to its reuse mask",
        "Vertex identity, CSR position and cache-line identity are different quantities.",
        "An undirected PageRank example has thirty-two vertices with nine "
        "non-isolated vertices shown. Outer vertex eight reads property eighteen "
        "from CSR position eighteen. Positions eighteen and twenty-two touch "
        "the same property line. Their distance four produces the Full14 "
        "metadata mask 0x00002200 and record 0x00002212 with five ID bits.",
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
              "Full14 example: 5 ID bits + 14 metadata bits", 862, role="state")
    part(f, 40, 921, 300, 127, "Line-reference construction",
         ("current s=19; next use s=23", "distance 4 -> reference 0x10"), "compute")
    part(f, 455, 921, 300, 127, f"Mask M = 0x{fx.full_mask:08x}",
         ("FINITE=1; reference=16", "action=0; ID width=5"), "state")
    part(f, 895, 921, 265, 127, f"R = 0x{fx.full_record:08x}",
         ("R = vertex 18 | M", "still one 4-byte word"), "data")
    f.arrow(((340, 978), (455, 978)), kind="control", label="encode",
            label_at=(397, 899), color=PURPLE)
    f.arrow(((755, 978), (895, 978)), kind="control", label="OR with ID",
            label_at=(825, 1084), color=BLUE)
    note(f, 1112, "Position j=18 happens to contain vertex 18; j=22 contains it again. A position is not a vertex ID.")
    note(f, 1144, "Preprocessing finishes before the measured traversal. The next plate compares the two supported formats.")
    return f


def record_formats(root, fx):
    f = plate(
        root, MECHANISM, "02", "record-formats",
        "Choose the mask to fit the graph",
        "Scale6 is the compact large-ID mode. Smaller graphs can use richer reference and action fields.",
        "A thirty-two-bit edge word has 32 minus the required ID width spare "
        "bits. The running thirty-two-vertex graph needs five ID bits and its "
        "Full14 example uses fourteen metadata bits, leaving thirteen unused. "
        "Full14 supports configurable reference/state/action splits totaling "
        "fourteen bits. Twitter requires twenty-six ID bits and uses Scale6. "
        "The current native ABI is explicitly fixed to twenty-six plus six.",
        1320,
    )
    f.section("1", "GRAPH SIZE SETS THE BIT BUDGET",
              "b_ID = max(1, ceil(log2 |V|))", 138, role="data")
    tabular(f, 40, 199, (340, 180, 210, 390),
            ("Graph", "ID bits", "Spare bits", "Implemented encoding"),
            (("Running example: 32 vertices", "5", "27", "Full14 uses 14; 13 remain unused"),
             ("262,144 vertices (n18)", "18", "14", "Full14 fills the available budget"),
             ("Twitter: 41,652,230 vertices", "26", "6", "Scale6 fills the available budget")),
            row_height=49)
    note(f, 433, "The budget is 32 - b_ID, not always six bits. Format selection is explicit, not an automatic bit allocator.")

    f.section("2", "FULL14: A RICHER SMALL-GRAPH MASK",
              "actual five-bit IDs in the running example", 492, role="state")
    for x, label in ((267.5, "[31:19]"), (565, "[18:15]"), (670, "[14:13]"),
                     (845, "[12:5]"), (1072.5, "[4:0]")):
        f.text(x, 554, label, mono=True, anchor="middle")
    f.bitfield(40, 577, 1120, 88,
               (("unused", 13, "neutral"), ("action", 4, "transfer"),
                ("state", 2, "state"), ("reference", 8, "state"),
                ("vertex", 5, "data")), total_bits=32, minimum_field_width=0)
    note(f, 706, f"0x{fx.full_record:08x}: vertex 18, reference 0x10, FINITE state 1, action 0. The ID is still recoverable.")
    tabular(f, 40, 747, (260, 260, 320, 280),
            ("Ref / state / action", "Reference precision", "Action interpretation", "Metadata total"),
            (("8 / 2 / 4 (default)", "5 exponent + 3 mantissa", "direct forward record lead", "14 bits"),
             ("10 / 2 / 2", "5 exponent + 5 mantissa", "codes for {none, 8, 12, 15}", "14 bits"),
             ("12 / 2 / 0", "5 exponent + 7 mantissa", "no encoded prefetch action", "14 bits")),
            row_height=44)
    note(f, 958, "More reference precision trades against action bits within Full14; it does not automatically consume all spare bits.")

    f.section("3", "SCALE6: THE TWITTER-SCALE CHOICE",
              "also the current fixed-width native ABI", 1012, role="state")
    f.bitfield(40, 1070, 1120, 80,
               (("token [31:26]", 6, "state"), ("vertex ID [25:0]", 26, "data")),
               total_bits=32, minimum_field_width=0)
    note(f, 1190, "Token: 0 UNKNOWN | 1 DEAD | 2..32 FINITE | 33..63 WRAP. No separate action field.")
    note(f, 1222, f"The same edge is 0x{fx.record:08x} in the 26+6 ABI; its distance-four hint decodes to seven.")
    note(f, 1254, "Full14 supports ID widths through 18; Scale6 through 26. Unused headroom is not silently reassigned.")
    note(f, 1286, "Native rich-format decode is not implemented. The functional model supports both encodings.", BORDER)
    return f


def future_distance(root, fx):
    f = plate(
        root, MECHANISM, "03", "future-distance",
        "More metadata bits sharpen the future bound",
        "Reference precision is an encoding choice; the underlying graph access order is unchanged.",
        "CSR position seventeen accesses line A and position eighteen accesses "
        "line B. Their next uses are at positions nineteen and twenty-two. "
        "Full14 represents the tracked distance four exactly, while Scale6 "
        "decodes it to seven. A separate distance-one-hundred quantizer probe "
        "shows the effect of additional mantissa bits. Passed predictions become "
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

    f.section("2", "QUANTIZE WITHOUT CHANGING THE ID",
              "table entries are decoded upper bounds", 500, role="state")
    rows = []
    for distance, label in ((4, "4 (running edge)"), (100, "100 (probe)")):
        rich = tuple(str(full_distance_bounds(distance, bits)[1])
                     for bits in (8, 10, 12))
        rows.append((label, *rich, str((1 << distance.bit_length()) - 1)))
    tabular(f, 40, 568, (220, 225, 225, 225, 225),
            ("True distance", "Full14: 8/2/4", "Full14: 10/2/2",
             "Full14: 12/2/0", "Scale6"), rows, row_height=48)
    note(f, 753, "Full14 keeps mantissa precision. Scale6 uses a compact state/distance token; its distance-four upper bound is 7.")
    note(f, 785, "Distance 100 is a separate precision probe, not a request in the running 32-vertex graph.", BORDER)

    f.section("3", "EXPIRY IS NOT DEATH",
              "hold the s19 prediction fixed for this comparison", 840, role="verify")
    f.text(40, 910, "Full14", size=17, bold=True, color=GREEN)
    f.rect(180, 880, 400, 42, role="compute", stroke=GREEN, radius=0)
    f.text(380, 908, "FINITE through deadline 23", anchor="middle", color=GREEN)
    f.text(680, 908, "UNKNOWN from 24", color=RED)
    f.text(40, 981, "Scale6", size=17, bold=True, color=PURPLE)
    f.rect(180, 951, 700, 42, role="state", stroke=PURPLE, radius=0)
    f.text(530, 979, "FINITE through deadline 26", anchor="middle", color=PURPLE)
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
        "twenty-three. Scale6 deadlines twenty-one and twenty-six give remaining "
        "distances two and seven, hence victim scores zero and one. LRU evicts A "
        "for an incoming scores line C; ECG evicts B. This is a worked algorithm "
        "example, not measured benchmark performance.",
        1310,
    )
    example_ribbon(f, fx)
    f.section("1", "THE CACHE SNAPSHOT AFTER s19",
              "teaching set 0: two resident property ways", 233, role="data")
    tabular(f, 40, 295, (180, 180, 210, 230, 180, 140),
            ("Way / line", "Last touch", "Actual next use", "Decoded deadline", "Remaining", "Score"),
            (("0 / A: p[0..15]", "s18: p[11]", "s20: p[7]", "18 + 3 = 21", "21 - 19 = 2", "0"),
             ("1 / B: p[16..31]", "s19: p[18]", "s23: p[18]", "19 + 7 = 26", "26 - 19 = 7", "1")),
            row_height=50)
    note(f, 488, "The score is distanceRRPV(remaining) = min(7, floor(log2(remaining)) / 2), rounded down.")
    note(f, 520, "Full14 yields tighter deadlines 20 and 23 for these two accesses, with the same score ordering.")

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
         ("line P_B, s19, deadline 26", "retain context and identity"), "compute")
    fifo(f, 450, 1072, 285, 16, "16-entry commit transport")
    part(f, 895, 1040, 265, 109, "Resident metadata",
         ("prediction / RRPV update", "no fill / recency touch"), "state")
    f.arrow(((310, 1093), (450, 1093)), kind="control", label="capture",
            label_at=(380, 1024), color=PURPLE)
    f.arrow(((735, 1093), (895, 1093)), kind="control", label="resident update",
            label_at=(815, 1172), color=PURPLE)
    f.text(40, 1180, "Native line payload: D=32. Full14's default model uses D=21.", size=16)
    f.bitfield(40, 1200, 1120, 72,
               (("deadline", 32, "state"), ("state", 2, "state"),
                ("origin", 1, "transfer")), total_bits=35)
    note(f, 1298, "General victim order: DEAD, non-property, then scored properties. Unknown uses max(RRPV, local GRASP).")
    return f


def lookahead_prefetch(root, fx):
    f = plate(
        root, MECHANISM, "05", "lookahead-prefetch",
        "Prefetch actions name a future record",
        "Reference distance predicts this line's reuse; a prefetch lead selects another record's property.",
        "The running record's sixteen-word window contains only property lines "
        "A and B. B is current and A first appears at lead one, so neither "
        "Full14 nor Scale6 issues a prefetch for this record. A second flow "
        "contrasts Full14's offline encoded lead with Scale6's runtime selection "
        "from record bytes. Both decode the target vertex from the selected "
        "future record and apply resident, pending and admission filters.",
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
    f.text(600, 490, "Full14 action = 0; Scale6 selected lead = 0. No extra read for this record.",
           size=17, bold=True, anchor="middle", color=GREEN)

    f.section("2", "TWO WAYS TO SELECT A LEAD",
              "choose a format, not an additional edge stream", 566, role="state")
    part(f, 40, 632, 500, 165, "Full14: encode the choice offline",
         ("prioritize the largest backward gap",
          "then next-use distance, then proximity to lead 10",
          "4-bit action: lead; 2-bit action: {0,8,12,15}"), "state")
    part(f, 660, 632, 500, 165, "Scale6: inspect the record window",
         ("first distinct line at a lead in 8..15",
          "smallest decoded future bound wins",
          "tie: closest to lead 10; no action field"), "compute")
    part(f, 415, 871, 370, 130, "Read target ID, then admit",
         ("target = R[j + lead].vertex",
          "reject resident / pending / denied",
          "reuse distance is not the target ID"), "compute")
    f.arrow(((290, 797), (290, 933), (415, 933)), kind="control", label="decoded action",
            label_at=(199, 841), color=PURPLE)
    f.arrow(((910, 797), (910, 933), (785, 933)), kind="control", label="selected lead",
            label_at=(1009, 841), color=GREEN)

    f.section("3", "PREFETCH FILLS ONLY THE LLC",
              "functional model; native delivery is not implemented", 1057, role="transfer")
    fifo(f, 40, 1139, 270, 8, "8-entry prefetch queue", "transfer")
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
    note(f, 1294, "Primary model: 8-request latency, at most 1 issue per 8 governed requests. FlowThrough is OFF.")
    note(f, 1326, "Useful prefetches can reduce demand misses; only total reads plus writebacks establish the traffic cost.", BORDER)
    return f


def capacity_accounting(root, fx):
    f = plate(
        root, MECHANISM, "06", "capacity-accounting",
        "Graph-sized matrices and cache-sized state",
        "P-OPT reserves active columns; ECG uses the edge word and additional per-cache-line state.",
        "For full Twitter, one one-byte-per-property-line column is 2,603,265 "
        "bytes. Full P-OPT keeps two columns and SE keeps one, while both retain "
        "256 backing columns. Sixteen-way bars show the full and single-epoch "
        "reservations at 8, 16 and 24 MiB. Full14's default twenty-one-bit "
        "deadline and Scale6's thirty-two-bit deadline produce different "
        "cache-sized metadata budgets. Neither model count is a silicon-area ratio.",
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

    f.section("3", "ECG KEEPS THE DATA WAYS",
              "prediction state is additional storage", 976, role="verify")
    note(f, 1032, "Full14 with D=21 adds 24 bits/line; Scale6 with D=32 adds 35. Both retain the ordinary data ways.")
    note(f, 1064, "At 8 MiB: about 384 KiB (Full14 model) or 560 KiB (Scale6 model), including both queues and lookahead.")
    note(f, 1097, "This is not yet an equal-area comparison. Backing matrices, active payloads, cache state and ports are distinct.", BORDER)
    return f


def instruction_family(root, fx):
    f = plate(
        root, ISA, "01", "instruction-family",
        "Two native loads, two different results",
        "The record produces a renamed integer operand; the dependent property load produces the unchanged F32 value.",
        "The native example uses the fixed Scale6 ABI. Record load I0 reads "
        "four real bytes, derives semantic position nineteen from the record "
        "address and iteration descriptor, and writes canonical operand "
        "0x0000001310000012 to an illustrative renamed register P17. "
        "Property load I1 extracts vertex eighteen, forms the ordinary property "
        "address and returns 1/128. Its prediction stays on the dynamic "
        "instruction for retirement; it is not the floating-point result.",
        1270,
    )
    example_ribbon(f, fx)
    f.section("1", "CONFIGURE THE RECORD CONTEXT",
              "implemented RV64 ABI: 26 ID bits + 6 token bits", 238, role="compute")
    tabular(f, 40, 300, (360, 400, 360),
            ("Configuration", "Running example", "Role"),
            (("CSRs 0x803 / 0x804 / 0x801", "record base R; 34 words; V=32; ctx=1", "bounds and context, set before ROI"),
             ("Iteration descriptor operand", "sequence base 0; no following pass", "same semantic coordinate across loads")),
            row_height=47)
    note(f, 480, "A small graph does not switch the native decoder to Full14. The wider small-graph encoding is currently in cache_sim.")

    f.section("2", "I0: READ AND ASSEMBLE THE OPERAND",
              "custom-0 / funct3 2 / raw funct7 0x30", 535, role="data")
    f.table(40, 598, 310, 160, 3, role="data")
    f.text(55, 628, "Record array in memory", size=17, bold=True)
    f.text(55, 680, "RA = R + 4 x 18", mono=True)
    f.text(55, 734, f"Mem[RA] = 0x{fx.record:08x}", mono=True)
    part(f, 480, 598, 320, 160, "Record result assembly",
         ("s = 0 + (RA-R)/4 + 1 = 19",
          "range checks; normalize WRAP",
          "join sequence with 32-bit word"), "compute")
    f.rect(930, 598, 230, 160, role="state", radius=0)
    f.text(945, 627, "P17: 64-bit operand", size=17, bold=True)
    f.text(945, 683, f"0x{fx.native_operand:016x}", mono=True)
    f.text(945, 728, "sequence | record", size=16)
    f.arrow(((350, 674), (480, 674)), kind="transfer", label="raw word",
            cadence="4 bytes", label_at=(415, 579), color=BLUE)
    f.arrow(((800, 674), (930, 674)), kind="dependency", label="rename result",
            label_at=(865, 792), color=PURPLE)

    f.section("3", "I1: KEEP VALUE AND HINT SEPARATE",
              "custom-0 / funct3 2 / raw funct7 0x34", 849, role="compute")
    part(f, 40, 912, 325, 182, "Consume renamed P17",
         ("low 26 bits -> v=18", "token 4 -> distance bound 7",
          "EA = property base + 18 x 4", "deadline = s19 + 7 = 26"), "compute")
    part(f, 475, 912, 330, 182, "Ordinary data service",
         ("VA 0x80000048 -> translated PA",
          "L1D / L2 / LLC as required",
          "read the contribution, not the mask",
          "p[18] is still 1/128"), "data")
    part(f, 915, 912, 245, 80, "F9: unchanged value",
         (f"F32 bits 0x{fx.property_value_bits:08X}",), "data")
    part(f, 915, 1030, 245, 122, "DynInst hint for I1",
         ("v18, s19, deadline 26",
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
    note(f, 1256, "P17/F9 are illustrative rename tags. This is an experimental custom-0 extension, not a ratified RISC-V ISA.")
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
    f.text(40, 212, f"Native I0 reads 0x{fx.record:08x}; I1 returns p[18] = 1/128. P_B denotes its translated physical line.",
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
         ("I0: RA=R+4j; s=j+1", "I1: VA=Pbase+18x4",
          "I1 hint: s19, deadline 26"), "compute")
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
             ("I1", "F9 ready", "PROPERTY: s19, D26, ctx1, own PA")),
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
         ("observe: PENDING s19", "commit: FINITE, D26",
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
    f.text(902, 1187, "not D26", color=PURPLE)

    f.rect(40, 1137, 745, 195, role="state", radius=0)
    f.text(60, 1170, "Retirement metadata transport", size=18, bold=True, color=PURPLE)
    f.text(65, 1201, "{P_B, s19, D26, ctx1}", mono=True)
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
    note(f, 1525, "The native model uses a dedicated metadata link and tag port; prefetch and physical-cost qualification remain separate.", BORDER)
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
        "at nineteen; a coalesced update at twenty-five can install deadline "
        "twenty-six. The final table distinguishes CPU ready cycles from semantic "
        "request positions and protects the oldest of two physical same-line slots.",
        1430,
    )
    example_ribbon(f, fx)
    f.section("1", "THREE EVENTS, THREE PERMISSIONS",
              "top row shows the private-miss case", 236, role="compute")
    part(f, 40, 301, 300, 151, "Observed at the LLC",
         ("I1 request carries s19 / ctx1",
          "set PENDING, value=19",
          "do not install deadline 26"), "state")
    part(f, 450, 301, 300, 151, "Data completes",
         ("F9 receives 1/128",
          "I1 may still be speculative",
          "no commit update yet"), "data")
    part(f, 860, 301, 300, 151, "I1 retires",
         ("its own PA and captured hint",
          "enqueue {P_B,s19,D26,ctx1}",
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
          "install FINITE, deadline=26",
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
        "One edge, two encodings, unchanged data",
        "The mask occupies unused edge-ID bits. It is not applied to the floating-point property value.",
        "The running graph needs five vertex bits. Vertex eighteen is ORed with "
        "Full14 metadata mask 0x00002200 to form record 0x00002212. The same "
        "access has Scale6 record 0x10000012 in the fixed native ABI. Both "
        "recover vertex eighteen and address 0x80000048, returning the exact "
        "initial contribution 1/128 with F32 bits 0x3C000000. Only the decoded "
        "future bounds differ: four versus seven, giving deadlines twenty-three "
        "and twenty-six.",
        1330,
    )
    example_ribbon(f, fx)
    f.section("1", "BUILD THE METADATA MASK",
              "Full14 example; b_ID=5, not 26", 236, role="state")
    part(f, 40, 309, 390, 138, "Ordinary edge ID",
         ("v = 18 = 0x00000012", "ID extraction mask = 0x0000001F"), "data")
    part(f, 40, 495, 390, 138, f"Metadata M = 0x{fx.full_mask:08x}",
         ("(reference 0x10 << 5) | (FINITE 1 << 13)",
          "prefetch action = 0"), "state")
    f.circle(610, 472, 36, fill=GREEN_MATTE, stroke=GREEN)
    f.text(610, 479, "OR", size=18, bold=True, anchor="middle")
    part(f, 860, 411, 300, 123, "Packed record R",
         (f"0x{fx.full_record:08x}", "one 32-bit word, not a sidecar"), "data")
    f.arrow(((430, 378), (520, 378), (520, 454), (579, 454)),
            kind="dependency", label="preserve vertex", label_at=(596, 344), color=BLUE)
    f.arrow(((430, 564), (520, 564), (520, 490), (579, 490)),
            kind="dependency", label="insert metadata", label_at=(597, 611), color=PURPLE)
    f.arrow(((646, 472), (860, 472)), kind="control", label="R = v | M",
            label_at=(753, 389), color=GREEN)
    note(f, 679, "The full word changes from 0x00000012 to 0x00002212; masking off metadata still recovers exactly vertex 18.")

    f.section("2", "DECODE THE SELECTED FORMAT",
              "same address; different precision", 736, role="compute")
    tabular(f, 40, 799, (260, 260, 280, 320),
            ("Encoding", "Record", "Recovered vertex", "Candidate deadline"),
            (("Full14, 5 ID bits", f"0x{fx.full_record:08x}", "R & 0x1F = 18", "s19 + 4 = 23"),
             ("Scale6, native 26+6", f"0x{fx.record:08x}", "R & 0x03FFFFFF = 18", "s19 + 7 = 26")),
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
    note(f, 1311, "Native P17 adds s19: 0x0000001310000012. That is a register operand, not an eight-byte edge-memory load.", BORDER)
    return f


def architecture_state_map(root, fx):
    f = plate(
        root, WALK, "02", "architecture-state-map",
        "Where the mask and its decoded state live",
        "Edge storage, per-instruction association and per-cache-line predictions have different lifetimes.",
        "The encoded edge record and ordinary property data reside in graph "
        "memory. Full14's functional path keeps a separate carrier; in-place "
        "Scale6 avoids another edge array. Native per-instruction state is "
        "bounded by the CPU's in-flight window. Functional runtime accounting "
        "includes bounded commit and prefetch queues, a sixteen-word lookahead "
        "and D plus three prediction bits per cache line. D=21 and D=32 are "
        "shown separately rather than treating the Scale6 cost as universal.",
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
         ("Full14: edge-sized arrays",
          "Scale6 in-place: first / next",
          "Twitter: about 39.7 MiB",
          "temporary, not silicon state"), "neutral")
    note(f, 467, "Full14's functional carrier is separate; the in-place Scale6 path avoids a second edge array.")
    f.rect(40, 499, 1120, 115, role="compute", radius=0)
    f.text(60, 530, "Per-in-flight native access, not per graph edge", size=17, bold=True)
    f.text(60, 574, "P17: 64-bit sequence | record operand", size=16)
    f.text(660, 574, "I1: captured hint + its translated address", size=16)

    f.section("2", "BOUNDED CONTROL AND LINE STATE",
              "D is the stored semantic-deadline width", 672, role="state")
    f.rect(40, 728, 1120, 504, role="neutral", radius=0)
    f.arrow(((600, 614), (600, 728)), kind="control", label="committed information",
            label_at=(768, 716), color=PURPLE)
    fifo(f, 64, 807, 290, 16, "Commit: 16 entries")
    fifo(f, 440, 807, 310, 8, "Prefetch: 8 entries", "transfer")
    f.table(860, 759, 275, 130, 3, role="data")
    f.text(876, 789, "Functional lookahead", size=17, bold=True)
    f.text(876, 832, "16 x 32-bit records", size=16)
    f.text(876, 875, "512 bits", size=16)
    f.text(64, 909, "entry = 51 + 2D bits", size=16, color=PURPLE)
    f.text(440, 909, "entry = 49 + D bits", size=16, color=AMBER)
    f.text(64, 966, "Full14 default: D=21", size=17, bold=True)
    f.text(630, 966, "Scale6 / native payload: D=32", size=17, bold=True)
    for x, width, bits in ((64, 480, 21), (630, 505, 32)):
        f.bitfield(x, 988, width, 76,
                   (("deadline", bits, "state"), ("state", 2, "state"),
                    ("origin", 1, "transfer")), total_bits=bits + 3)
    tabular(f, 64, 1100, (190, 210, 340, 330),
            ("Format", "Per line", "Line payload at 8 MiB", "Buffers + control"),
            (("Full14, D=21", "24 bits", "3,145,728 bits", "2,624 bits"),
             ("Scale6, D=32", "35 bits", "4,587,520 bits", "3,064 bits")),
            row_height=38)

    f.section("3", "DO NOT TURN STATE INTO AN AREA CLAIM",
              "both queues and lookahead enabled in these totals", 1286, role="verify")
    note(f, 1350, "8 MiB model totals: Full14 3,148,352 bits; Scale6 4,590,584 bits. Baseline cache data/tags remain separate.")
    note(f, 1382, "Native capture, validation, port logic and planned 128-byte lookahead require their own cost accounting.", BORDER)
    note(f, 1414, "These bit counts are not synthesized area, and the 512-bit functional window is not a native buffer implementation.", BORDER)
    return f


def evidence_boundary(root, fx):
    f = plate(
        root, "evaluation-methodology", "01", "evidence-boundary",
        "What each implementation can establish",
        "Separate encoding, cache behavior, native execution and physical cost before making a claim.",
        "A support matrix distinguishes the richer Full14 functional format, "
        "the Scale6 large-graph format, and the current fixed-ABI native "
        "replacement path. Native prefetch and physical qualification are not "
        "implied by functional cache results. Demand misses, total traffic and "
        "runtime are kept distinct, and every comparison must match graph work "
        "and active mechanisms. P-OPT-SE remains a disclosed reconstruction.",
        1270,
    )
    f.section("1", "FORMAT SUPPORT IS NOT BACKEND SUPPORT",
              "current implementation, not an aspirational diagram", 138, role="verify")
    tabular(f, 40, 201, (240, 235, 335, 310),
            ("Surface", "Encoding", "Implemented mechanism", "Evidence limit"),
            (("cache_sim / small graphs", "Full14; ID width <=18", "replacement + commit + prefetch", "functional cache / traffic"),
             ("cache_sim / Twitter", "Scale6; 26+6", "bounded in-place large-graph path", "full Twitter cache / traffic"),
             ("gem5 / RV64 O3", "fixed native 26+6", "real loads + retirement + R-only", "production timing gate closed"),
             ("Sniper", "legacy paths", "earlier modeled controls", "REF32 rows unsupported"),
             ("RTL / physical cost", "earlier components", "not a complete REF32 realization", "no complete area / timing result")),
            row_height=49)
    note(f, 538, "A small graph can use Full14 in cache_sim; it does not automatically change the current native instruction ABI.")

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
