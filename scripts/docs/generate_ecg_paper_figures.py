#!/usr/bin/env python3
"""Generate the compact ECG conference-paper figure set and Draw.io mirrors.

The wiki plates in ``fig/wiki`` stay untouched: this generator writes the
separate ``fig/paper`` and ``fig/paper_src`` collections. Each figure carries
exactly one concept, uses a landscape 1200 px canvas, and keeps live text at
17 px or larger so a two-column page reduction stays readable.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Sequence

from ecg_figure_lib import (
    AMBER,
    BLUE,
    GRAY,
    GREEN,
    INK,
    PURPLE,
    RED,
    ROLE_COLORS,
    Figure,
    FigureTarget,
    clean_generated_roots,
)


SOURCE_ROOT = Path(__file__).resolve().parents[2]
ROOT = SOURCE_ROOT
COLLECTION = "paper"
SLUG = "ecg-paper"

BODY = 18
LABEL = 17
HEADING = 22


def target(index: str, topic: str) -> FigureTarget:
    return FigureTarget(SLUG, index, topic)


def paper_figure(
    index: str,
    topic: str,
    title: str,
    subtitle: str,
    description: str,
    height: int,
) -> Figure:
    return Figure(
        ROOT,
        target(index, topic),
        title,
        subtitle,
        description,
        height,
        collection=COLLECTION,
    )


def band_label(figure: Figure, x: float, y: float, text: str) -> None:
    figure.text(x, y, text, size=LABEL, bold=True, color=GRAY, max_width=900)


def panel(
    figure: Figure,
    x: float,
    y: float,
    width: float,
    height: float,
    title: str,
    body: Sequence[str],
    *,
    role: str,
    title_color: str | None = None,
    mono_body: bool = False,
    body_color: str = INK,
) -> None:
    """One titled plate. Panels are used sparingly; there is no card grid."""
    figure.rect(x, y, width, height, role=role, radius=0)
    strong, _ = ROLE_COLORS[role]
    figure.text(
        x + 14, y + 32, title, size=LABEL, bold=True,
        color=title_color or strong, max_width=width - 28,
    )
    figure.lines(
        x + 14, y + 64, body, size=BODY, step=26,
        mono=mono_body, color=body_color, max_width=width - 28,
    )


def bit_row(
    figure: Figure,
    x: float,
    y: float,
    width: float,
    height: float,
    fields: Sequence[tuple[str, int, str]],
) -> None:
    """Proportional bit layout with 17/18 px live text."""
    minimums = [110.0 if bits <= 4 else 0.0 for _, bits, _ in fields]
    flexible_bits = sum(
        bits for (_, bits, _), minimum in zip(fields, minimums)
        if minimum == 0
    )
    flexible_width = width - sum(minimums)
    widths = [
        minimum if minimum else flexible_width * bits / flexible_bits
        for (_, bits, _), minimum in zip(fields, minimums)
    ]
    cursor = x
    for (label, bits, role), field_width in zip(fields, widths):
        figure.rect(
            cursor, y, field_width, height, role=role,
            stroke=INK, stroke_width=2, radius=0,
        )
        figure.text(
            cursor + field_width / 2, y + 36, label, size=LABEL, bold=True,
            anchor="middle", max_width=field_width - 14,
        )
        figure.text(
            cursor + field_width / 2, y + 68, f"{bits} bits", size=BODY,
            anchor="middle", max_width=field_width - 14,
        )
        cursor += field_width


def save(figure: Figure, generated: list[tuple[Path, Path]]) -> None:
    generated.append(figure.save())


def offline_plan(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "01", "offline-plan",
        "Offline ReusePlan construction and the measured-ROI boundary",
        "One deterministic preprocessing pass turns CSR rows into an "
        "edge-aligned record stream.",
        "A left-to-right band shows CSR rows entering a deterministic "
        "future-use pass that ranks property vertices by request count, "
        "selects the next two accesses to the same property block, and "
        "quantizes them into epochs. The resulting edge-aligned ReusePlan "
        "crosses an explicit measured-ROI boundary: construction happens "
        "before the region of interest, and the kernel only streams "
        "validated records inside it.",
        560,
    )
    band_label(
        figure, 32, 133,
        "OFFLINE PREPROCESSING: deterministic, outside the measured region",
    )

    panel(
        figure, 30, 155, 300, 216,
        "Graph and CSR rows",
        (
            "outer vertex u owns row u",
            "row_ptr[u] .. row_ptr[u+1]",
            "col_idx enumerates N_in(u)",
            "or N_out(u), by traversal",
            "entry (u,v) reads p[v]",
        ),
        role="data",
    )
    panel(
        figure, 450, 155, 300, 216,
        "Future-use construction",
        (
            "rank by property requests",
            "pull d_out(v); push d_in(v)",
            "hot fraction 0.15 sets tiers",
            "next two block accesses",
            "after outer vertex u",
            "quantize outer IDs to epochs",
        ),
        role="compute",
    )
    panel(
        figure, 870, 155, 300, 216,
        "Edge-aligned ReusePlan",
        (
            "one record per CSR entry",
            "destination | tier | e1 | e2",
            "record offset == CSR offset",
            "immutable runtime input",
        ),
        role="transfer",
    )
    figure.arrow(
        ((330, 250), (450, 250)),
        kind="control", label="CSR rows", color=BLUE,
        label_at=(390, 232), label_size=LABEL,
    )
    figure.arrow(
        ((750, 250), (870, 250)),
        kind="control", label="records", color=GREEN,
        label_at=(810, 232), label_size=LABEL,
    )

    figure.line((30, 400), (1170, 400), color=RED, width=2)
    figure.text(
        30, 392, "measured ROI boundary", size=LABEL, bold=True, color=RED,
        max_width=300,
    )
    figure.arrow(
        ((1020, 371), (1020, 416)),
        kind="transfer",
        label="validated record stream",
        cadence="one record per governed edge access",
        color=AMBER,
        label_at=(990, 392),
        label_anchor="end",
        label_size=LABEL,
    )
    panel(
        figure, 30, 416, 1140, 110,
        "Measured ROI",
        (
            "header, offsets, record width, and payload hash are checked "
            "before the ROI; disagreement aborts",
            "the kernel streams validated records in CSR order; no record "
            "is built inside the measured region",
        ),
        role="neutral",
        title_color=INK,
    )
    save(figure, generated)


def compact_record(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "02", "compact-record",
        "Compact ReusePlan record: 32-bit budget and edge substitution",
        "The compact record substitutes for the structural edge word; an "
        "unsupported width fails closed.",
        "Two bit layouts show the compact ReusePlan record. The tiered "
        "layout packs a 16-bit destination, two tier bits, and two 5-bit "
        "epochs; the tierless layout drops the carried tier so an 18-bit "
        "destination and two 7-bit epochs still fit in 32 bits. Side panels "
        "give the field meanings, the format-word and sidecar binding that "
        "fails closed on an unsupported or mismatched width, and the "
        "substitution against the structural edge word.",
        640,
    )
    band_label(figure, 32, 133, "COMPACT RECORD FIELDS AND 32-BIT BUDGET")

    bit_row(
        figure, 32, 152, 690, 88,
        (
            ("destination", 16, "data"),
            ("tier", 2, "transfer"),
            ("epoch1", 5, "compute"),
            ("epoch2", 5, "state"),
        ),
    )
    figure.text(
        32, 270,
        "16 + 2 + 5 + 5 = 28 bits: 65,536 vertices, 32 epochs",
        size=BODY, mono=True, max_width=690,
    )
    bit_row(
        figure, 32, 296, 690, 88,
        (
            ("destination", 18, "data"),
            ("epoch1", 7, "compute"),
            ("epoch2", 7, "state"),
        ),
    )
    figure.text(
        32, 414,
        "18 + 0 + 7 + 7 = 32 bits: 262,144 vertices, 128 epochs",
        size=BODY, mono=True, max_width=690,
    )
    figure.text(
        32, 440,
        "18 + 2 + 7 + 7 = 34 bits: rejected, no decoder implements it",
        size=BODY, mono=True, color=RED, max_width=690,
    )
    panel(
        figure, 32, 466, 690, 134,
        "Substitution for the structural edge word",
        (
            "4-byte compact record replaces the 4-byte CSR edge word",
            "8-byte general record instead adds a word per edge",
            "weighted SSSP: 64-bit substitute, or edge plus 32-bit sidecar",
        ),
        role="transfer",
    )

    panel(
        figure, 756, 152, 412, 188,
        "Field meaning",
        (
            "destination: property vertex",
            "tier: optional carried hint",
            "epoch1, epoch2: two future",
            "epochs for the same block",
            "unused high bits stay zero",
        ),
        role="data",
    )
    panel(
        figure, 756, 380, 412, 220,
        "Fail-closed format and sidecar binding",
        (
            "format word carries id_bits,",
            "epoch_bits, tier_bits, presence",
            "only tier_bits 0 or 2 decode",
            "sidecar v2 binds the tier width",
            "width mismatch aborts the run",
        ),
        role="verify",
    )
    figure.arrow(
        ((962, 380), (962, 340)),
        kind="control", label="bound record width", color=RED,
        label_at=(940, 366), label_anchor="end", label_size=LABEL,
    )
    save(figure, generated)


def request_path(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "03", "request-path",
        "Record load and property load through the RISC-V request path",
        "Two dynamic loads share the datapath; only the record load carries "
        "FlowThrough.",
        "The shared front end decodes the custom-0 role, generates the "
        "effective address in the AGU, and builds the Request in the LSQ. "
        "The path then splits into a record lane that sets the FlowThrough "
        "Request extension and reaches the LLC through a record-block MSHR, "
        "and a property lane whose ReuseBind Request extension consumes the "
        "renamed record operand and reaches the LLC through a property "
        "MSHR that merges compatible targets.",
        648,
    )
    band_label(figure, 32, 133, "SHARED FRONT END")

    panel(
        figure, 32, 140, 336, 130,
        "Decode (custom-0 role)",
        (
            "record load or property load",
            "format control/status register",
            "supplies compact field widths",
        ),
        role="neutral",
        title_color=INK,
    )
    panel(
        figure, 432, 140, 336, 130,
        "AGU (address generation)",
        (
            "forms the effective address",
            "I0: record base + offset",
            "I1: base + dest * elem size",
        ),
        role="compute",
    )
    panel(
        figure, 832, 140, 336, 130,
        "LSQ (load/store queue)",
        (
            "ordinary ordering and replay",
            "attaches the Request extension",
        ),
        role="state",
    )
    figure.arrow(
        ((368, 205), (432, 205)),
        kind="control", label="custom-0 role", color=INK,
    )
    figure.arrow(
        ((768, 205), (832, 205)),
        kind="control", label="effective address", color=GREEN,
    )

    figure.arrow(
        ((860, 270), (860, 290), (420, 290), (420, 330)),
        kind="control", label="record Request", color=AMBER,
        label_at=(432, 310), label_anchor="start", label_size=LABEL,
    )
    figure.arrow(
        ((980, 270), (980, 330)),
        kind="control", label="property Request", color=BLUE,
        label_at=(995, 310), label_anchor="start", label_size=LABEL,
    )

    figure.text(
        60, 310, "I0  ecg.flow.load.compact", size=BODY, mono=True,
        color=AMBER, max_width=420,
    )
    figure.text(
        620, 310, "I1  ecg.bind.load.u32", size=BODY, mono=True,
        color=BLUE, max_width=420,
    )
    lanes = (
        (
            60, AMBER, "transfer",
            (
                ("Request extension: ECG_FLOWTHROUGH",
                 "record block; allocOnFill = false"),
                ("record-block MSHR",
                 "a no-allocate target joins the target list"),
                ("LLC fill decision",
                 "the structural line need not be allocated"),
            ),
        ),
        (
            620, BLUE, "data",
            (
                ("Request extension: ReuseBind",
                 "rs2 = renamed record operand"),
                ("property MSHR",
                 "merges compatible ReuseBind targets"),
                ("LLC stamp decision",
                 "validates context and property block"),
            ),
        ),
    )
    for x, color, role, boxes in lanes:
        for step, (title, line) in enumerate(boxes):
            y = 330 + step * 100
            panel(figure, x, y, 520, 76, title, (line,), role=role)
        for step in range(2):
            top = 330 + step * 100 + 76
            figure.arrow(
                ((x + 260, top), (x + 260, top + 24)),
                kind="control", label=boxes[step + 1][0], color=color,
            )
    figure.arrow(
        ((580, 368), (620, 368)),
        kind="dependency", label="renamed record operand", color=PURPLE,
    )
    figure.text(
        32, 634,
        "The property Request never receives FlowThrough, and the record "
        "Request never carries ReuseBind.",
        size=BODY, max_width=1136,
    )
    save(figure, generated)


def llc_decision(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "04", "llc-decision",
        "LLC ReuseBind acceptance and victim selection",
        "Line-local stamp state, RRIP eligibility, and effective future "
        "distance.",
        "The upper band validates an incoming property Request: the "
        "ReuseBind extension must be valid, the execution context nonzero, "
        "and the destination must map to the accessed property block, after "
        "which the line-local stamp holds tier, two epochs, count, context, "
        "and validity. The lower band orders victim selection: RRIP "
        "eligibility, the oldest non-property line, the farthest effective "
        "future distance across property candidates with zero distance for "
        "an unstamped line, and a stable set-order tie-break.",
        648,
    )
    band_label(figure, 32, 133, "REQUEST AND CONTEXT VALIDATION")

    panel(
        figure, 32, 150, 300, 140,
        "Property Request at the LLC",
        (
            "hit or fill with ReuseBind",
            "execution context id",
            "destination vertex",
        ),
        role="data",
    )
    figure.diamond(560, 220, 340, 140, role="verify")
    figure.text(
        560, 210, "Accept ReuseBind?", size=LABEL, bold=True, color=RED,
        anchor="middle", max_width=220,
    )
    figure.text(
        560, 240, "context and block match", size=BODY, anchor="middle",
        max_width=240,
    )
    panel(
        figure, 790, 150, 378, 140,
        "Line-local metadata",
        (
            "accepted stamp: tier, epochs",
            "count, context, stamp-valid",
            "cleared on invalidation",
        ),
        role="state",
    )
    figure.arrow(
        ((332, 220), (390, 220)),
        kind="control", label="hit or fill with ReuseBind", color=BLUE,
    )
    figure.arrow(
        ((730, 220), (790, 220)),
        kind="control", label="accepted stamp", color=PURPLE,
    )
    figure.arrow(
        ((560, 290), (560, 318)),
        kind="control", label="no stamp", color=RED,
    )
    figure.text(
        32, 336,
        "conflicted, invalid context, or block mismatch -> no stamp; an "
        "unstamped line has effective distance 0",
        size=BODY, color=RED, max_width=1136,
    )

    band_label(figure, 32, 372, "VICTIM SELECTION IN SET ORDER (rrip_first)")
    steps = (
        (
            32, "1. RRIP eligibility", "compute",
            ("age the set until a", "way reaches rrpvMax"),
        ),
        (
            322, "2. Recency choice", "state",
            ("eligible ways only:", "oldest non-property", "line is taken"),
        ),
        (
            612, "3. Future distance", "transfer",
            ("no structural line:", "farthest effective",
             "distance over property", "candidates;",
             "unstamped distance = 0"),
        ),
        (
            902, "4. Stable set order", "neutral",
            ("a remaining tie uses", "deterministic set order"),
        ),
    )
    for x, title, role, body in steps:
        panel(
            figure, x, 390, 254, 190, title, body,
            role=role, title_color=INK if role == "neutral" else None,
        )
    for x, label, color in (
        (286, "eligible ways", GREEN),
        (576, "no structural line", PURPLE),
        (866, "a remaining tie", AMBER),
    ):
        figure.arrow(
            ((x, 485), (x + 36, 485)), kind="control", label=label,
            color=color,
        )
    figure.text(
        32, 624,
        "distance(e,c) = (e + N - (c mod N)) mod N ; effective = "
        "min(d1, d2) ; unstamped = 0",
        size=BODY, mono=True, max_width=1136,
    )
    save(figure, generated)


def flowthrough(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "05", "flowthrough",
        "FlowThrough separates service from LLC fill allocation",
        "Allocation is decided per MSHR target and combined with a logical "
        "OR.",
        "Lookup and service stay unchanged: translation, load ordering, "
        "private and last-level lookup, hits, miss service, responses, and "
        "writeback. A hit bypasses fill allocation. On a miss, each MSHR "
        "target contributes an allocation bit; an all-no-allocate target "
        "list skips the LLC fill, while any allocating merge target forces "
        "the fill.",
        640,
    )
    band_label(figure, 32, 133, "LOOKUP AND SERVICE ARE UNCHANGED")

    panel(
        figure, 32, 145, 360, 260,
        "Unchanged lookup and service",
        (
            "translation and load ordering",
            "L1D, L2, and LLC lookup",
            "all cache hits",
            "miss service and response",
            "private-cache fills",
            "writeback and retirement",
        ),
        role="neutral",
        title_color=INK,
    )
    panel(
        figure, 440, 285, 300, 130,
        "MSHR target allocation",
        (
            "target carries allocOnFill",
            "FlowThrough sets false",
            "combine target bits with OR",
        ),
        role="transfer",
    )
    panel(
        figure, 800, 145, 368, 120,
        "Hit: the line is returned",
        ("a cache hit takes no fill decision",),
        role="compute",
    )
    panel(
        figure, 800, 285, 368, 120,
        "Miss: all targets no-allocate",
        ("the LLC fill is skipped",),
        role="state",
    )
    panel(
        figure, 800, 435, 368, 120,
        "Miss: mixed MSHR merge",
        ("an allocating target forces a fill",),
        role="verify",
    )
    figure.arrow(
        ((392, 205), (800, 205)),
        kind="control", label="cache hit", color=GREEN,
        label_at=(596, 190), label_size=LABEL,
    )
    figure.arrow(
        ((392, 345), (440, 345)),
        kind="control", label="MSHR target allocation", color=INK,
    )
    figure.arrow(
        ((740, 345), (800, 345)),
        kind="control", label="the LLC fill is skipped", color=PURPLE,
    )
    panel(
        figure, 32, 435, 708, 120,
        "Corner case: an allocating merge target",
        (
            "a no-allocate FlowThrough target cannot suppress the fill",
            "another target still requires the same block",
        ),
        role="verify",
    )
    figure.arrow(
        ((590, 415), (590, 435)),
        kind="control",
        label="Corner case: an allocating merge target",
        color=RED,
    )
    figure.arrow(
        ((740, 495), (800, 495)),
        kind="control",
        label="an allocating target forces a fill",
        color=RED,
    )
    figure.text(
        32, 584,
        "A derived prefetch keeps structural FlowThrough only inside the "
        "active structural-carrier region.",
        size=BODY, max_width=1136,
    )
    figure.text(
        32, 612,
        "The property Request never receives FlowThrough.",
        size=BODY, max_width=1136,
    )
    save(figure, generated)


def evidence_boundary(generated: list[tuple[Path, Path]]) -> None:
    figure = paper_figure(
        "06", "evidence-boundary",
        "Evidence boundary for ECG evaluation claims",
        "Each source supports a different claim, and both receipt gates must "
        "pass.",
        "Four evidence sources are separated: gem5 O3 supplies architectural "
        "timing, cache_sim supplies functional cache and traffic evidence, "
        "Sniper supplies matched-work modeled cache and traffic evidence, "
        "and analytic P-OPT supplies an optimistic timing bound because "
        "target-time latency, bandwidth, queueing, and contention are "
        "omitted. A candidate result row is admissible only after the "
        "mechanism receipt gate and the semantic receipt gate both pass.",
        644,
    )
    band_label(figure, 32, 133, "EVIDENCE SOURCES AND THEIR LIMITS")

    sources = (
        (
            32, "gem5 O3 timing", "compute",
            ("architectural execution", "time and ISA path",
             "sampled, bounded runs"),
        ),
        (
            320, "cache_sim traffic", "data",
            ("functional cache and", "traffic accounting",
             "no cycle or instruction", "model"),
        ),
        (
            608, "Sniper matched work", "state",
            ("modeled cache and traffic", "at larger scale",
             "time is not speedup", "evidence"),
        ),
        (
            896, "Analytic P-OPT bound", "transfer",
            ("charges reserved ways", "and matrix bytes",
             "target-time cost omitted"),
        ),
    )
    for x, title, role, body in sources:
        panel(figure, x, 150, 272, 152, title, body, role=role)
    for x, label, color in (
        (168, "gem5 O3 timing", GREEN),
        (456, "cache_sim traffic", BLUE),
        (744, "Sniper matched work", PURPLE),
        (1032, "Analytic P-OPT bound", AMBER),
    ):
        figure.arrow(
            ((x, 302), (x, 340)), kind="control", label=label, color=color,
        )
    figure.rect(32, 340, 1136, 54, role="neutral", radius=0)
    figure.text(
        46, 374,
        "Candidate result row: time, total off-chip traffic, retired "
        "instructions, activity receipts",
        size=BODY, max_width=1108,
    )
    figure.arrow(
        ((212, 394), (212, 424)),
        kind="control", label="Candidate result row", color=INK,
    )

    panel(
        figure, 32, 424, 360, 136,
        "Mechanism receipt gate",
        (
            "requested and effective agree",
            "activity counters are positive",
            "width and substitution agree",
        ),
        role="verify",
    )
    panel(
        figure, 420, 424, 360, 136,
        "Semantic receipt gate",
        (
            "kernel output agrees across",
            "every matched policy row",
            "a failed peer invalidates timing",
        ),
        role="verify",
    )
    panel(
        figure, 808, 424, 360, 136,
        "Admissible row",
        (
            "both gates passed",
            "eligible for a reported claim",
            "no result is claimed here",
        ),
        role="compute",
    )
    figure.arrow(
        ((392, 492), (420, 492)),
        kind="control", label="Mechanism receipt", color=RED,
    )
    figure.arrow(
        ((780, 492), (808, 492)),
        kind="control", label="Semantic receipt", color=RED,
    )
    figure.text(
        32, 590,
        "popt_target_time_charged = 0, so analytic P-OPT timing is an "
        "optimistic bound rather than a target-time implementation.",
        size=BODY, max_width=1136,
    )
    figure.text(
        32, 618,
        "The figure states evidence scope and admissibility; it reports no "
        "measurements.",
        size=BODY, max_width=1136,
    )
    save(figure, generated)


def generate(output_root: Path = SOURCE_ROOT) -> list[tuple[Path, Path]]:
    global ROOT
    previous_root = ROOT
    ROOT = output_root
    try:
        clean_generated_roots(output_root, COLLECTION)
        generated: list[tuple[Path, Path]] = []
        offline_plan(generated)
        compact_record(generated)
        request_path(generated)
        llc_decision(generated)
        flowthrough(generated)
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
            (SOURCE_ROOT / "fig" / COLLECTION, "*.svg"),
            (SOURCE_ROOT / "fig" / f"{COLLECTION}_src", "*.drawio"),
        ):
            if root.exists():
                before.update({
                    path.relative_to(SOURCE_ROOT): path.read_bytes()
                    for path in root.rglob(suffix)
                })
        with TemporaryDirectory(prefix="ecg-paper-figure-check-") as temporary:
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
                "generated paper figures differ: "
                f"missing={missing} added={added} changed={changed}"
            )
        return 0
    generated = generate(SOURCE_ROOT)
    for svg, drawio in generated:
        print(svg.relative_to(SOURCE_ROOT), drawio.relative_to(SOURCE_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
