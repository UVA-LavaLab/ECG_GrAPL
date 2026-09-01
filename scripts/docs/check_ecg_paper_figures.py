#!/usr/bin/env python3
"""Validate ECG's compact conference-paper figure collection.

The paper set is validated with the same structural contract as the wiki set
(bounds, text fit and collisions, connector routing, accessible title and
description, SVG/Draw.io parity, allowed colors, deterministic regeneration)
but it has no owning wiki page, it must stay compact and landscape, and its
live text must be at least 17 px so a two-column reduction stays readable.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_wiki_figures as shared


ROOT = shared.ROOT
SVG_ROOT = ROOT / "fig" / "paper"
DRAWIO_ROOT = ROOT / "fig" / "paper_src"
REGISTER = ROOT / "fig" / "README.md"
GENERATOR = ROOT / "scripts/docs/generate_ecg_paper_figures.py"

SLUG = "ecg-paper"
EXPECTED_STEMS = (
    "ecg-paper-f01-offline-plan",
    "ecg-paper-f02-compact-record",
    "ecg-paper-f03-request-path",
    "ecg-paper-f04-llc-decision",
    "ecg-paper-f05-flowthrough",
    "ecg-paper-f06-evidence-boundary",
)
MIN_HEIGHT = 420
MAX_HEIGHT = 650
MIN_FONT = 17
PDF_SOURCE = re.compile(rb"% ECG-SOURCE-SHA256:([0-9a-f]{64})")
PDF_CANVAS = re.compile(rb"% ECG-CANVAS:(\d+)x(\d+)")
PDF_MEDIA_BOX = re.compile(
    rb"/MediaBox\s*\[\s*0\s+0\s+([0-9.]+)\s+([0-9.]+)\s*\]"
)

MARKETING = (
    "faster", "outperform", "state-of-the-art", "novel", "significant",
    "dramatic", "seamless", "blazing", "best-in-class", "world-class",
    "revolutionary", "cutting-edge", "superior", "win over",
)
RESULT_NUMBER = re.compile(r"\d+(?:\.\d+)?\s*(?:%|x speedup|percent)")
REQUIRED_TERMS = {
    "ecg-paper-f01-offline-plan": (
        "outer vertex", "N_in(u)", "N_out(u)", "d_out(v)", "d_in(v)",
        "row_ptr", "col_idx", "ReusePlan", "measured ROI",
    ),
    "ecg-paper-f02-compact-record": (
        "destination", "tier", "epoch1", "epoch2", "sidecar", "32 bits",
    ),
    "ecg-paper-f03-request-path": (
        "AGU", "LSQ", "MSHR", "LLC", "ReuseBind", "Request extension",
        "FlowThrough",
    ),
    "ecg-paper-f04-llc-decision": (
        "ReuseBind", "rrpvMax", "Line-local metadata", "stamped",
        "non-property", "distance over property", "unstamped distance = 0",
    ),
    "ecg-paper-f05-flowthrough": (
        "allocOnFill", "MSHR", "FlowThrough", "LLC",
        "a cache hit takes no fill decision",
    ),
    "ecg-paper-f06-evidence-boundary": (
        "gem5 O3", "cache_sim", "Sniper", "P-OPT", "receipt",
    ),
}


def collect(errors: list[str]) -> list[shared.Figure]:
    figures: list[shared.Figure] = []
    expected_mirrors: set[Path] = set()
    expected_pdfs: set[Path] = set()
    for svg in sorted(SVG_ROOT.rglob("*.svg")):
        relative = svg.relative_to(SVG_ROOT)
        if len(relative.parts) != 2 or relative.parts[0] != SLUG:
            errors.append(f"{relative}: expected fig/paper/{SLUG}/<asset>")
            continue
        match = shared.NAME.match(svg.stem)
        if not match or match.group("slug") != SLUG:
            errors.append(f"{relative}: invalid stable figure name")
            continue
        drawio = DRAWIO_ROOT / SLUG / f"{svg.stem}.drawio"
        pdf = svg.with_suffix(".pdf")
        expected_mirrors.add(drawio)
        expected_pdfs.add(pdf)
        if not drawio.is_file():
            errors.append(f"{drawio.relative_to(ROOT)}: missing Draw.io mirror")
            continue
        if not pdf.is_file():
            errors.append(f"{pdf.relative_to(ROOT)}: missing vector PDF export")
            continue
        figures.append(
            shared.Figure(
                svg, drawio, SLUG, match.group("index"), match.group("topic")
            )
        )
    for drawio in sorted(DRAWIO_ROOT.rglob("*.drawio")):
        if drawio not in expected_mirrors:
            errors.append(f"{drawio.relative_to(ROOT)}: orphaned mirror")
    for pdf in sorted(SVG_ROOT.rglob("*.pdf")):
        if pdf not in expected_pdfs:
            errors.append(f"{pdf.relative_to(ROOT)}: orphaned PDF export")
    stems = tuple(figure.svg.stem for figure in figures)
    if stems != EXPECTED_STEMS:
        errors.append(
            f"paper set must be exactly {list(EXPECTED_STEMS)}, got "
            f"{list(stems)}"
        )
    return figures


def check_pdf(
    figure: shared.Figure, root: ET.Element, errors: list[str]
) -> None:
    pdf = figure.svg.with_suffix(".pdf")
    name = pdf.relative_to(ROOT)
    data = pdf.read_bytes()
    if not data.startswith(b"%PDF-"):
        errors.append(f"{name}: not a PDF file")
        return
    source = PDF_SOURCE.search(data)
    expected_digest = hashlib.sha256(figure.svg.read_bytes()).hexdigest()
    if not source or source.group(1).decode("ascii") != expected_digest:
        errors.append(f"{name}: source SVG digest is missing or stale")
    canvas = PDF_CANVAS.search(data)
    width = int(float(root.get("width", "0")))
    height = int(float(root.get("height", "0")))
    if not canvas or tuple(map(int, canvas.groups())) != (width, height):
        errors.append(f"{name}: source canvas marker is missing or stale")
    media_box = PDF_MEDIA_BOX.search(data)
    expected_points = (width * 0.75, height * 0.75)
    if not media_box:
        errors.append(f"{name}: PDF MediaBox is missing")
    else:
        actual_points = tuple(float(value) for value in media_box.groups())
        if any(
            abs(actual - expected) > 0.2
            for actual, expected in zip(actual_points, expected_points)
        ):
            errors.append(
                f"{name}: PDF page is {actual_points}, expected "
                f"{expected_points} points"
            )
    if b"/Font" not in data:
        errors.append(f"{name}: PDF has no vector text/font resources")
    mutool = shutil.which("mutool")
    if mutool:
        checked = subprocess.run(
            [mutool, "info", str(pdf)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if checked.returncode != 0:
            errors.append(
                f"{name}: mutool rejected PDF: "
                f"{(checked.stdout + checked.stderr).strip()}"
            )


def check_paper_shape(
    figure: shared.Figure, root: ET.Element, errors: list[str]
) -> None:
    name = figure.svg.relative_to(ROOT)
    height = float(root.get("height", "0"))
    if not MIN_HEIGHT <= height <= MAX_HEIGHT:
        errors.append(
            f"{name}: paper canvas height {height:g} is outside "
            f"{MIN_HEIGHT}-{MAX_HEIGHT} px"
        )
    if height >= float(root.get("width", "0")):
        errors.append(f"{name}: paper figures must stay landscape")
    sizes = shared.css_sizes(root)
    used: list[float] = []
    for node in root.iter():
        if shared.local_name(node) != "text":
            continue
        if not "".join(node.itertext()).strip():
            continue
        classes = node.get("class", "").split()
        used.append(
            next((sizes[item] for item in classes if item in sizes), 16.0)
        )
    if not used:
        errors.append(f"{name}: figure has no live text")
    elif min(used) < MIN_FONT:
        errors.append(
            f"{name}: live text at {min(used):g} px is below {MIN_FONT} px"
        )


def check_paper_language(
    figure: shared.Figure, root: ET.Element, errors: list[str]
) -> None:
    name = figure.svg.relative_to(ROOT)
    visible = " ".join(shared.svg_labels(root))
    lowered = visible.lower()
    offenders = sorted({term for term in MARKETING if term in lowered})
    if offenders:
        errors.append(f"{name}: marketing language: {offenders}")
    claims = RESULT_NUMBER.findall(visible)
    if claims:
        errors.append(f"{name}: figure states a measured result: {claims}")
    missing = [
        term for term in REQUIRED_TERMS.get(figure.svg.stem, ())
        if term not in visible
    ]
    if missing:
        errors.append(f"{name}: missing required terminology: {missing}")


def check_registration(
    figure: shared.Figure, root: ET.Element, errors: list[str]
) -> None:
    register = REGISTER.read_text(encoding="utf-8")
    target = f"paper/{SLUG}/{figure.svg.name}"
    mirror = f"paper_src/{SLUG}/{figure.svg.stem}.drawio"
    pdf = f"paper/{SLUG}/{figure.svg.stem}.pdf"
    if target not in register:
        errors.append(f"{target}: missing SVG path in fig/README.md")
    if mirror not in register:
        errors.append(f"{mirror}: missing Draw.io path in fig/README.md")
    if pdf not in register:
        errors.append(f"{pdf}: missing PDF path in fig/README.md")
    if shared.figure_title(root) not in register:
        errors.append(f"{target}: missing visible title in fig/README.md")


def main() -> int:
    errors: list[str] = []
    figures = collect(errors)
    for figure in figures:
        svg_root = shared.check_svg(figure, errors)
        if svg_root is None:
            continue
        shared.check_drawio(figure, svg_root, errors)
        check_pdf(figure, svg_root, errors)
        check_paper_shape(figure, svg_root, errors)
        check_paper_language(figure, svg_root, errors)
        check_registration(figure, svg_root, errors)
    regeneration = subprocess.run(
        [sys.executable, str(GENERATOR), "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if regeneration.returncode != 0:
        errors.append(
            "deterministic regeneration failed: "
            + (regeneration.stdout + regeneration.stderr).strip()
        )
    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"validated {len(figures)} ECG paper figures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
