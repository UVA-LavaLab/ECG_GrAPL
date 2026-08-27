#!/usr/bin/env python3
"""Validate ECG's generated SVG/Draw.io wiki figure contract."""

from __future__ import annotations

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from html import unescape
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SVG_ROOT = ROOT / "fig" / "wiki"
DRAWIO_ROOT = ROOT / "fig" / "wiki_src"
WIKI_ROOT = ROOT / "wiki"
REGISTER = ROOT / "fig" / "README.md"
SVG_NS = "http://www.w3.org/2000/svg"
SCHEMA = "ecg-public/v1"

NAME = re.compile(
    r"^(?P<slug>[a-z0-9]+(?:-[a-z0-9]+)*)-f(?P<index>\d{2})-"
    r"(?P<topic>[a-z0-9]+(?:-[a-z0-9]+)*)$"
)
IMAGE = re.compile(r"!\[(?P<alt>[^\]]+)\]\((?P<target>[^)\s]+)\)")
FONT = re.compile(r"font-size\s*:\s*(\d+(?:\.\d+)?)px")
HEX = re.compile(r"#[0-9A-Fa-f]{6}")
POINT = re.compile(r"[ML]\s*(-?[\d.]+)[\s,]+(-?[\d.]+)")
CSS_CLASS = re.compile(r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}")
CSS_SIZE = re.compile(r"font-size\s*:\s*(\d+(?:\.\d+)?)px")
HTML_TAG = re.compile(r"<[^>]+>")

ALLOWED_COLORS = {
    "#27313A", "#9AA3AD", "#1769C2", "#15803D", "#B45309",
    "#B42318", "#6D5BD0", "#FFFFFF", "#F8F6EC", "#EDF5FF",
    "#E7F7EA", "#FFF0D8", "#F7DEDC", "#EEE9FF",
    "#1E2327", "#252A2E", "#273846", "#24382A", "#3B3122",
    "#3D292A", "#302C3C", "#ECE7DD", "#747D86", "#63A8FF",
    "#63D68B", "#F0B35A", "#F09B95", "#A79BF0",
}
ARROW_KINDS = {"transfer", "control", "loop", "dependency", "model-edge"}
PAGE_BY_SLUG = {
    "home": WIKI_ROOT / "Home.md",
    "reuse-plan-flowthrough": WIKI_ROOT / "ReusePlan-FlowThrough.md",
    "risc-v-instruction-path": WIKI_ROOT / "RISC-V-Instruction-Path.md",
    "property-to-cache-walkthrough":
        WIKI_ROOT / "Property-to-Cache-Walkthrough.md",
    "evaluation-methodology": WIKI_ROOT / "Evaluation-Methodology.md",
}


@dataclass(frozen=True)
class Figure:
    svg: Path
    drawio: Path
    slug: str
    index: str
    topic: str


def local_name(element: ET.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def figure_title(root: ET.Element) -> str:
    node = root.find(f"{{{SVG_NS}}}title")
    return "".join(node.itertext()).strip() if node is not None else ""


def figure_description(root: ET.Element) -> str:
    node = root.find(f"{{{SVG_NS}}}desc")
    return "".join(node.itertext()).strip() if node is not None else ""


def svg_labels(root: ET.Element) -> list[str]:
    return [
        " ".join("".join(node.itertext()).split())
        for node in root.iter()
        if local_name(node) == "text" and "".join(node.itertext()).strip()
    ]


def drawio_labels(root: ET.Element) -> list[str]:
    labels: list[str] = []
    for cell in root.iter("mxCell"):
        if cell.get("vertex") != "1":
            continue
        value = HTML_TAG.sub("", unescape(cell.get("value", ""))).strip()
        if value:
            labels.append(" ".join(value.split()))
    return labels


def css_sizes(root: ET.Element) -> dict[str, float]:
    source = "\n".join(
        node.text or ""
        for node in root.iter()
        if local_name(node) == "style"
    )
    sizes: dict[str, float] = {}
    for name, body in CSS_CLASS.findall(source):
        match = CSS_SIZE.search(body)
        if match:
            sizes[name] = float(match.group(1))
    return sizes


def check_text_layout(
    figure: Figure, root: ET.Element, errors: list[str]
) -> None:
    sizes = css_sizes(root)
    boxes: list[tuple[float, float, float, float, str]] = []
    name = figure.svg.relative_to(ROOT)
    for node in root.iter():
        if local_name(node) != "text":
            continue
        value = " ".join("".join(node.itertext()).split())
        if not value:
            continue
        classes = node.get("class", "").split()
        size = next((sizes[item] for item in classes if item in sizes), 16.0)
        mono = "mono" in classes
        bold = (
            node.get("font-weight") == "700"
            or any(item in {"title", "heading", "label"} for item in classes)
        )
        ratio = 0.62 if mono else 0.57 if bold else 0.53
        width = len(value) * size * ratio
        x = float(node.get("x", "0"))
        y = float(node.get("y", "0"))
        anchor = node.get("text-anchor", "start")
        left = x - width / 2 if anchor == "middle" else x - width if anchor == "end" else x
        right = left + width
        top = y - size * 0.80
        bottom = y + size * 0.22
        if left < -1 or right > 1201 or top < -1 or bottom > float(root.get("height", "0")) + 1:
            errors.append(f"{name}: text '{value}' leaves the canvas")
        boxes.append((left, top, right, bottom, value))
    collisions: list[str] = []
    for index, first in enumerate(boxes):
        for second in boxes[index + 1:]:
            horizontal = min(first[2], second[2]) - max(first[0], second[0])
            vertical = min(first[3], second[3]) - max(first[1], second[1])
            if horizontal > 1 and vertical > 1:
                collisions.append(f"'{first[4]}' overlaps '{second[4]}'")
                if len(collisions) == 8:
                    break
        if len(collisions) == 8:
            break
    if collisions:
        errors.append(f"{name}: text collisions: {'; '.join(collisions)}")


def collect(errors: list[str]) -> list[Figure]:
    figures: list[Figure] = []
    expected: set[Path] = set()
    for svg in sorted(SVG_ROOT.rglob("*.svg")):
        relative = svg.relative_to(SVG_ROOT)
        if len(relative.parts) != 2:
            errors.append(f"{relative}: expected fig/wiki/<page-slug>/<asset>")
            continue
        slug = relative.parts[0]
        match = NAME.match(svg.stem)
        if not match or match.group("slug") != slug:
            errors.append(f"{relative}: invalid stable figure name")
            continue
        drawio = DRAWIO_ROOT / slug / f"{svg.stem}.drawio"
        expected.add(drawio)
        if not drawio.is_file():
            errors.append(f"{drawio.relative_to(ROOT)}: missing Draw.io mirror")
            continue
        figures.append(
            Figure(svg, drawio, slug, match.group("index"), match.group("topic"))
        )
    for drawio in sorted(DRAWIO_ROOT.rglob("*.drawio")):
        if drawio not in expected:
            errors.append(f"{drawio.relative_to(ROOT)}: orphaned mirror")
    if not figures:
        errors.append("no generated ECG figures found")
    return figures


def check_svg(figure: Figure, errors: list[str]) -> ET.Element | None:
    try:
        root = ET.parse(figure.svg).getroot()
    except ET.ParseError as exc:
        errors.append(f"{figure.svg.relative_to(ROOT)}: invalid SVG: {exc}")
        return None
    name = figure.svg.relative_to(ROOT)
    if root.get("width") != "1200":
        errors.append(f"{name}: canvas width must be exactly 1200")
    view_box = root.get("viewBox", "").split()
    if len(view_box) != 4 or view_box[:3] != ["0", "0", "1200"]:
        errors.append(f"{name}: viewBox must start '0 0 1200'")
    if root.get("role") != "img" or root.get("aria-labelledby") != "title desc":
        errors.append(f"{name}: missing role/ARIA contract")
    if root.get("data-figure-schema") != SCHEMA:
        errors.append(f"{name}: schema must be {SCHEMA}")
    title = root.find(f"{{{SVG_NS}}}title")
    desc = root.find(f"{{{SVG_NS}}}desc")
    if title is None or title.get("id") != "title" or not figure_title(root):
        errors.append(f"{name}: missing accessible title")
    if desc is None or desc.get("id") != "desc" or len(figure_description(root)) < 80:
        errors.append(f"{name}: description must replace the visible figure")
    source = figure.svg.read_text(encoding="utf-8")
    if "prefers-color-scheme: dark" not in source:
        errors.append(f"{name}: missing dark-mode mapping")
    if any(token in source for token in ("<foreignObject", "<script", "<image ")):
        errors.append(f"{name}: raster/HTML/script content is forbidden")
    fonts = [float(value) for value in FONT.findall(source)]
    if not fonts or min(fonts) < 16:
        errors.append(f"{name}: every used font style must be at least 16 px")
    colors = {value.upper() for value in HEX.findall(source)}
    unknown = sorted(colors - ALLOWED_COLORS)
    if unknown:
        errors.append(f"{name}: colors outside the role palette: {unknown}")
    backgrounds = [
        node for node in root
        if local_name(node) == "rect" and node.get("x") == "0"
        and node.get("y") == "0" and node.get("width") == "1200"
        and node.get("fill") == "#FFFFFF"
    ]
    if not backgrounds:
        errors.append(f"{name}: missing fully opaque publication background")
    check_text_layout(figure, root, errors)
    visible = " ".join(svg_labels(root))
    for node in root.iter():
        if not node.get("marker-end"):
            continue
        kind = node.get("data-flow-kind")
        label = node.get("data-flow-label")
        cadence = node.get("data-flow-cadence")
        if kind not in ARROW_KINDS:
            errors.append(f"{name}: arrow lacks an allowed data-flow-kind")
        if kind != "model-edge" and (not label or label not in visible):
            errors.append(f"{name}: arrow label is not nearby live text")
        if kind == "model-edge" and label:
            errors.append(f"{name}: model-edge must not carry a label")
        if kind == "transfer" and (not cadence or cadence not in visible):
            errors.append(f"{name}: transfer lacks live-text cadence")
        points = [
            (float(x), float(y))
            for x, y in POINT.findall(node.get("d", ""))
        ]
        if len(points) < 2:
            errors.append(f"{name}: arrow path must be an absolute polyline")
        elif max(
            max(abs(x2 - x1), abs(y2 - y1))
            for (x1, y1), (x2, y2) in zip(points, points[1:])
        ) < 20:
            errors.append(f"{name}: arrow has no run of at least 20 px")
    return root


def check_drawio(
    figure: Figure, svg_root: ET.Element, errors: list[str]
) -> None:
    name = figure.drawio.relative_to(ROOT)
    try:
        root = ET.parse(figure.drawio).getroot()
    except ET.ParseError as exc:
        errors.append(f"{name}: invalid Draw.io XML: {exc}")
        return
    if root.tag != "mxfile" or root.get("compressed") != "false":
        errors.append(f"{name}: mirror must be uncompressed mxfile XML")
        return
    diagram = root.find("diagram")
    model = diagram.find("mxGraphModel") if diagram is not None else None
    if diagram is None or model is None:
        errors.append(f"{name}: incomplete Draw.io model")
        return
    if diagram.get("name") != figure_title(svg_root):
        errors.append(f"{name}: diagram name differs from SVG title")
    if diagram.get("ecgTitle") != figure_title(svg_root):
        errors.append(f"{name}: ecgTitle differs from SVG title")
    if diagram.get("ecgDescription") != figure_description(svg_root):
        errors.append(f"{name}: ecgDescription differs from SVG description")
    if model.get("pageWidth") != "1200":
        errors.append(f"{name}: page width must be 1200")
    if model.get("pageHeight") != svg_root.get("height"):
        errors.append(f"{name}: page height differs from SVG canvas")
    cells = list(root.iter("mxCell"))
    identifiers: list[str | None] = []
    for cell in cells:
        parent = next(
            (
                wrapper for wrapper in root.iter()
                if cell in list(wrapper)
                and local_name(wrapper) in {"object", "UserObject"}
            ),
            None,
        )
        identifiers.append(cell.get("id") or (parent.get("id") if parent is not None else None))
        if cell.get("vertex") == "1" and cell.find("mxGeometry") is None:
            errors.append(f"{name}: editable vertex lacks explicit geometry")
    if None in identifiers or len(identifiers) != len(set(identifiers)):
        errors.append(f"{name}: duplicate or missing cell IDs")
    if svg_labels(svg_root) != drawio_labels(root):
        errors.append(f"{name}: ordered live labels differ from SVG")
    svg_kinds = [
        node.get("data-flow-kind")
        for node in svg_root.iter()
        if node.get("marker-end")
    ]
    drawio_kinds = [
        node.get("data-flow-kind")
        for node in root.iter()
        if local_name(node) in {"object", "UserObject"}
        and node.get("data-flow-kind")
    ]
    if svg_kinds != drawio_kinds:
        errors.append(f"{name}: arrow kinds differ from SVG")


def check_registration_and_embed(
    figure: Figure, svg_root: ET.Element, errors: list[str]
) -> None:
    target = f"fig/wiki/{figure.slug}/{figure.svg.name}"
    register_target = f"wiki/{figure.slug}/{figure.svg.name}"
    register = REGISTER.read_text(encoding="utf-8")
    if register_target not in register or figure_title(svg_root) not in register:
        errors.append(f"{target}: missing title/path in fig/README.md")
    page = PAGE_BY_SLUG.get(figure.slug)
    if page is None or not page.is_file():
        errors.append(f"{target}: no owning wiki page")
        return
    source = page.read_text(encoding="utf-8")
    relative_target = f"../{target}"
    images = [
        match for match in IMAGE.finditer(source)
        if match.group("target") == relative_target
    ]
    if len(images) != 1:
        errors.append(f"{page.relative_to(ROOT)}: expected one embed for {target}")
    elif len(images[0].group("alt")) < 40:
        errors.append(f"{page.relative_to(ROOT)}: figure alt text is too short")
    heading = re.compile(
        rf"^### Figure \d+(?:\.\d+)? — {re.escape(figure_title(svg_root))}$",
        re.MULTILINE,
    )
    if not heading.search(source):
        errors.append(f"{page.relative_to(ROOT)}: missing canonical figure heading")
    if not re.search(r"^\*\*Figure \d+(?:\.\d+)?\.\*\* \S", source, re.MULTILINE):
        errors.append(f"{page.relative_to(ROOT)}: missing visible figure caption")


def main() -> int:
    errors: list[str] = []
    figures = collect(errors)
    for figure in figures:
        svg_root = check_svg(figure, errors)
        if svg_root is None:
            continue
        check_drawio(figure, svg_root, errors)
        check_registration_and_embed(figure, svg_root, errors)
    regeneration = subprocess.run(
        [sys.executable, str(ROOT / "scripts/docs/generate_ecg_figures.py"), "--check"],
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
    print(f"validated {len(figures)} ECG wiki figures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
