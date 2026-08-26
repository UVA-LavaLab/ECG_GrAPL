#!/usr/bin/env python3
"""Deterministic SVG and Draw.io primitives for ECG wiki figures."""

from __future__ import annotations

from dataclasses import dataclass
from html import escape
from pathlib import Path
from typing import Iterable, Sequence


CANVAS_WIDTH = 1200
SCHEMA = "ecg-public/v1"

INK = "#27313A"
GRAY = "#9AA3AD"
BLUE = "#1769C2"
GREEN = "#15803D"
AMBER = "#B45309"
RED = "#B42318"
PURPLE = "#6D5BD0"

WHITE = "#FFFFFF"
NEUTRAL = "#F8F6EC"
BLUE_MATTE = "#EDF5FF"
GREEN_MATTE = "#E7F7EA"
AMBER_MATTE = "#FFF0D8"
RED_MATTE = "#F7DEDC"
PURPLE_MATTE = "#EEE9FF"

ROLE_COLORS = {
    "ink": (INK, WHITE),
    "neutral": (INK, NEUTRAL),
    "data": (BLUE, BLUE_MATTE),
    "compute": (GREEN, GREEN_MATTE),
    "transfer": (AMBER, AMBER_MATTE),
    "verify": (RED, RED_MATTE),
    "state": (PURPLE, PURPLE_MATTE),
}


def _attrs(values: dict[str, object]) -> str:
    return " ".join(
        f'{name}="{escape(str(value), quote=True)}"'
        for name, value in values.items()
        if value is not None
    )


def _estimate_text(text: str, size: float, mono: bool, bold: bool) -> float:
    ratio = 0.62 if mono else 0.57 if bold else 0.53
    return len(text) * size * ratio


@dataclass(frozen=True)
class FigureTarget:
    slug: str
    index: str
    topic: str

    @property
    def stem(self) -> str:
        return f"{self.slug}-f{self.index}-{self.topic}"


class Figure:
    """One generated figure and its editable Draw.io mirror."""

    def __init__(
        self,
        root: Path,
        target: FigureTarget,
        title: str,
        subtitle: str,
        description: str,
        height: int,
    ) -> None:
        if height < 300:
            raise ValueError("figure height must leave room for a real plate")
        self.root = root
        self.target = target
        self.title = title
        self.subtitle = subtitle
        self.description = description
        self.width = CANVAS_WIDTH
        self.height = height
        self._svg: list[str] = []
        self._cells: list[str] = []
        self._labels: list[str] = []
        self._next_id = 2
        self._arrow_kinds: list[str] = []
        self._add_drawio_rect(
            0, 0, self.width, self.height, WHITE, "none", 0, rounded=0,
            identifier="canvas",
        )
        self.text(42, 47, title, size=32, bold=True, max_width=1116)
        self.text(42, 75, subtitle, size=18, color=INK, max_width=1116)
        self.line((42, 99), (1158, 99), color=INK, width=2)

    def _id(self, prefix: str) -> str:
        identifier = f"{prefix}{self._next_id:03d}"
        self._next_id += 1
        return identifier

    def _check_bounds(
        self, x: float, y: float, width: float = 0, height: float = 0
    ) -> None:
        if x < 0 or y < 0 or x + width > self.width or y + height > self.height:
            raise ValueError(
                f"{self.target.stem}: geometry outside canvas: "
                f"{x},{y},{width},{height}"
            )

    def _add_drawio_rect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        fill: str,
        stroke: str,
        stroke_width: float,
        *,
        rounded: int = 1,
        identifier: str | None = None,
    ) -> None:
        cell_id = identifier or self._id("b")
        style = (
            f"rounded={rounded};html=0;fillColor={fill};"
            f"strokeColor={stroke};strokeWidth={stroke_width};"
        )
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" style="{style}" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" '
            f'width="{width}" height="{height}" as="geometry"/></mxCell>'
        )

    def rect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        role: str = "neutral",
        stroke: str = INK,
        stroke_width: float = 2,
        radius: float = 9,
    ) -> None:
        self._check_bounds(x, y, width, height)
        _, fill = ROLE_COLORS[role]
        attributes = {
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "rx": radius,
            "fill": fill,
            "stroke": stroke,
            "stroke-width": stroke_width,
        }
        self._svg.append(
            f'<rect {_attrs(attributes)}/>'
        )
        self._add_drawio_rect(
            x, y, width, height, fill, stroke, stroke_width,
            rounded=1 if radius else 0,
        )

    def circle(
        self,
        cx: float,
        cy: float,
        radius: float,
        *,
        fill: str = INK,
        stroke: str = INK,
    ) -> None:
        self._check_bounds(cx - radius, cy - radius, 2 * radius, 2 * radius)
        attributes = {
            "cx": cx,
            "cy": cy,
            "r": radius,
            "fill": fill,
            "stroke": stroke,
            "stroke-width": 1,
        }
        self._svg.append(
            f'<circle {_attrs(attributes)}/>'
        )
        cell_id = self._id("b")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" '
            f'style="ellipse;html=0;fillColor={fill};strokeColor={stroke};'
            f'strokeWidth=1;" vertex="1" parent="1"><mxGeometry '
            f'x="{cx - radius}" y="{cy - radius}" width="{2 * radius}" '
            f'height="{2 * radius}" as="geometry"/></mxCell>'
        )

    def diamond(
        self,
        cx: float,
        cy: float,
        width: float,
        height: float,
        *,
        role: str = "compute",
        stroke: str = INK,
        stroke_width: float = 2,
    ) -> None:
        x = cx - width / 2
        y = cy - height / 2
        self._check_bounds(x, y, width, height)
        _, fill = ROLE_COLORS[role]
        points = (
            f"{cx},{y} {x + width},{cy} "
            f"{cx},{y + height} {x},{cy}"
        )
        self._svg.append(
            f'<polygon points="{points}" fill="{fill}" stroke="{stroke}" '
            f'stroke-width="{stroke_width}"/>'
        )
        cell_id = self._id("b")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" '
            f'style="rhombus;html=0;fillColor={fill};strokeColor={stroke};'
            f'strokeWidth={stroke_width};" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{width}" height="{height}" '
            f'as="geometry"/></mxCell>'
        )

    def queue(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        role: str = "state",
        stroke: str = INK,
        stroke_width: float = 2,
    ) -> None:
        self._check_bounds(x, y, width, height)
        _, fill = ROLE_COLORS[role]
        inset = min(24.0, width * 0.12)
        points = (
            f"{x + inset},{y} {x + width},{y} "
            f"{x + width - inset},{y + height} {x},{y + height}"
        )
        self._svg.append(
            f'<polygon points="{points}" fill="{fill}" stroke="{stroke}" '
            f'stroke-width="{stroke_width}"/>'
        )
        cell_id = self._id("b")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" '
            f'style="shape=trapezoid;perimeter=trapezoidPerimeter;html=0;'
            f'fillColor={fill};strokeColor={stroke};strokeWidth={stroke_width};" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" '
            f'width="{width}" height="{height}" as="geometry"/></mxCell>'
        )

    def cylinder(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        role: str = "data",
        stroke: str = INK,
        stroke_width: float = 2,
    ) -> None:
        self._check_bounds(x, y, width, height)
        _, fill = ROLE_COLORS[role]
        cap = min(24.0, height * 0.18)
        self._svg.extend([
            f'<rect x="{x}" y="{y + cap / 2}" width="{width}" '
            f'height="{height - cap}" fill="{fill}" stroke="{stroke}" '
            f'stroke-width="{stroke_width}"/>',
            f'<ellipse cx="{x + width / 2}" cy="{y + cap / 2}" '
            f'rx="{width / 2}" ry="{cap / 2}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{stroke_width}"/>',
            f'<ellipse cx="{x + width / 2}" cy="{y + height - cap / 2}" '
            f'rx="{width / 2}" ry="{cap / 2}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{stroke_width}"/>',
        ])
        cell_id = self._id("b")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" '
            f'style="shape=cylinder3;html=0;boundedLbl=1;backgroundOutline=1;'
            f'fillColor={fill};strokeColor={stroke};strokeWidth={stroke_width};" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" '
            f'width="{width}" height="{height}" as="geometry"/></mxCell>'
        )

    def table(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        rows: int,
        *,
        role: str = "data",
        stroke: str = INK,
        stroke_width: float = 2,
    ) -> None:
        self.rect(
            x, y, width, height, role=role, stroke=stroke,
            stroke_width=stroke_width, radius=0,
        )
        for row in range(1, rows):
            row_y = y + height * row / rows
            self.line((x, row_y), (x + width, row_y), color=stroke, width=1)

    def text(
        self,
        x: float,
        y: float,
        value: str,
        *,
        size: float = 16,
        bold: bool = False,
        mono: bool = False,
        color: str = INK,
        anchor: str = "start",
        max_width: float | None = None,
    ) -> None:
        if size < 16:
            raise ValueError(f"{self.target.stem}: font below 16 px")
        estimated = _estimate_text(value, size, mono, bold)
        if max_width is not None and estimated > max_width:
            raise ValueError(
                f"{self.target.stem}: text '{value}' needs {estimated:.1f}px "
                f"but only {max_width:.1f}px is available"
            )
        left = (
            x - estimated / 2
            if anchor == "middle"
            else x - estimated
            if anchor == "end"
            else x
        )
        self._check_bounds(max(0, left), y - size, min(estimated, self.width), size * 1.25)
        classes = ["mono" if mono else "sans"]
        if size == 32:
            classes.append("title")
        elif size == 22:
            classes.append("heading")
        elif size == 18:
            classes.append("subtitle")
        elif size == 17:
            classes.append("label")
        else:
            classes.append("body")
        extra = ' font-weight="700"' if bold else ""
        attributes = {
            "x": x,
            "y": y,
            "text-anchor": anchor,
            "class": " ".join(classes),
            "fill": color,
        }
        self._svg.append(
            f'<text {_attrs(attributes)}{extra}>'
            f'{escape(value)}</text>'
        )
        self._labels.append(value)
        geometry_width = max(12.0, estimated)
        geometry_x = (
            x - geometry_width / 2
            if anchor == "middle"
            else x - geometry_width
            if anchor == "end"
            else x
        )
        geometry_y = y - size * 0.86
        family = "Courier New" if mono else "Helvetica"
        align = "center" if anchor == "middle" else "right" if anchor == "end" else "left"
        font_style = "1" if bold else "0"
        cell_id = self._id("t")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="{escape(value, quote=True)}" '
            f'style="text;html=0;strokeColor=none;fillColor=none;'
            f'align={align};verticalAlign=middle;fontFamily={family};'
            f'fontSize={size};fontColor={color};fontStyle={font_style};" '
            f'vertex="1" parent="1"><mxGeometry x="{geometry_x:.2f}" '
            f'y="{geometry_y:.2f}" width="{geometry_width:.2f}" '
            f'height="{size * 1.4:.2f}" as="geometry"/></mxCell>'
        )

    def lines(
        self,
        x: float,
        y: float,
        values: Sequence[str],
        *,
        size: float = 16,
        step: float = 24,
        bold_first: bool = False,
        mono: bool = False,
        color: str = INK,
        max_width: float | None = None,
    ) -> None:
        for index, value in enumerate(values):
            self.text(
                x,
                y + index * step,
                value,
                size=size,
                bold=bold_first and index == 0,
                mono=mono,
                color=color,
                max_width=max_width,
            )

    def line(
        self,
        start: tuple[float, float],
        end: tuple[float, float],
        *,
        color: str = INK,
        width: float = 2,
    ) -> None:
        self._check_bounds(min(start[0], end[0]), min(start[1], end[1]))
        attributes = {
            "x1": start[0],
            "y1": start[1],
            "x2": end[0],
            "y2": end[1],
            "stroke": color,
            "stroke-width": width,
        }
        self._svg.append(
            f'<line {_attrs(attributes)}/>'
        )
        cell_id = self._id("e")
        self._cells.append(
            f'<mxCell id="{cell_id}" value="" '
            f'style="edgeStyle=none;html=0;strokeColor={color};'
            f'strokeWidth={width};startArrow=none;endArrow=none;" '
            f'edge="1" parent="1"><mxGeometry relative="1" as="geometry">'
            f'<mxPoint x="{start[0]}" y="{start[1]}" as="sourcePoint"/>'
            f'<mxPoint x="{end[0]}" y="{end[1]}" as="targetPoint"/>'
            f'</mxGeometry></mxCell>'
        )

    def arrow(
        self,
        points: Sequence[tuple[float, float]],
        *,
        kind: str,
        label: str | None = None,
        cadence: str | None = None,
        color: str = AMBER,
        width: float = 3,
        label_at: tuple[float, float] | None = None,
        label_anchor: str = "middle",
    ) -> None:
        if kind not in {"transfer", "control", "loop", "dependency", "model-edge"}:
            raise ValueError(f"unsupported arrow kind: {kind}")
        if kind != "model-edge" and not label:
            raise ValueError(f"{kind} arrows require a semantic label")
        if kind == "transfer" and not cadence:
            raise ValueError("transfer arrows require cadence")
        if len(points) < 2:
            raise ValueError("an arrow needs at least two points")
        dominant = max(
            max(abs(x2 - x1), abs(y2 - y1))
            for (x1, y1), (x2, y2) in zip(points, points[1:])
        )
        if dominant < 60:
            raise ValueError("semantic arrows need a run of at least 60 px")
        path = " ".join(
            [f"M{points[0][0]} {points[0][1]}"]
            + [f"L{x} {y}" for x, y in points[1:]]
        )
        marker = {
            BLUE: "arrow-blue",
            GREEN: "arrow-green",
            AMBER: "arrow-amber",
            RED: "arrow-red",
            PURPLE: "arrow-purple",
            GRAY: "arrow-gray",
        }.get(color, "arrow-ink")
        attributes: dict[str, object] = {
            "d": path,
            "fill": "none",
            "stroke": color,
            "stroke-width": width,
            "marker-end": f"url(#{marker})",
            "data-flow-kind": kind,
        }
        if label:
            attributes["data-flow-label"] = label
        if cadence:
            attributes["data-flow-cadence"] = cadence
        self._svg.append(f'<path {_attrs(attributes)}/>')
        self._arrow_kinds.append(kind)
        edge_id = self._id("e")
        wrapper = {
            "id": edge_id,
            "data-flow-kind": kind,
            "data-flow-label": label,
            "data-flow-cadence": cadence,
        }
        point_xml = "".join(
            f'<mxPoint x="{x}" y="{y}"/>'
            for x, y in points[1:-1]
        )
        self._cells.append(
            f'<object {_attrs(wrapper)}><mxCell value="" '
            f'style="edgeStyle=none;html=0;strokeColor={color};'
            f'strokeWidth={width};startArrow=none;endArrow=block;endFill=1;" '
            f'edge="1" parent="1"><mxGeometry relative="1" as="geometry">'
            f'<mxPoint x="{points[0][0]}" y="{points[0][1]}" '
            f'as="sourcePoint"/><Array as="points">{point_xml}</Array>'
            f'<mxPoint x="{points[-1][0]}" y="{points[-1][1]}" '
            f'as="targetPoint"/></mxGeometry></mxCell></object>'
        )
        if label_at and label:
            visible = label if not cadence else f"{label} | {cadence}"
            self.text(
                label_at[0],
                label_at[1],
                visible,
                size=16,
                bold=True,
                color=color,
                anchor=label_anchor,
                max_width=900,
            )

    def section(
        self,
        number: str,
        title: str,
        subtitle: str,
        y: float,
        *,
        role: str,
    ) -> None:
        strong, _ = ROLE_COLORS[role]
        self.circle(44, y, 19, fill=INK, stroke=INK)
        self.text(44, y + 6, number, size=17, bold=True, color=WHITE, anchor="middle")
        self.text(74, y + 7, title, size=22, bold=True, color=strong, max_width=560)
        self.text(
            1158,
            y + 6,
            subtitle,
            size=16,
            color=GRAY,
            anchor="end",
            max_width=520,
        )
        self.line((24, y + 26), (1176, y + 26), color=strong, width=3)

    def card(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        title: str,
        body: Sequence[str],
        *,
        role: str,
        mono_body: bool = False,
        title_color: str | None = None,
    ) -> None:
        self.rect(x, y, width, height, role=role, stroke=INK, stroke_width=2)
        strong, _ = ROLE_COLORS[role]
        self.text(
            x + 16,
            y + 29,
            title,
            size=17,
            bold=True,
            color=title_color or strong,
            max_width=width - 32,
        )
        self.lines(
            x + 16,
            y + 59,
            body,
            size=16,
            step=24,
            mono=mono_body,
            max_width=width - 32,
        )

    def bitfield(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        fields: Sequence[tuple[str, int, str]],
        *,
        total_bits: int,
    ) -> None:
        cursor = x
        minimums = [90.0 if bits <= 4 else 0.0 for _, bits, _ in fields]
        flexible_bits = sum(
            bits for (_, bits, _), minimum in zip(fields, minimums)
            if minimum == 0
        )
        flexible_width = width - sum(minimums)
        widths = [
            minimum if minimum else flexible_width * bits / flexible_bits
            for (_, bits, _), minimum in zip(fields, minimums)
        ]
        for (label, bits, role), field_width in zip(fields, widths):
            self.rect(
                cursor,
                y,
                field_width,
                height,
                role=role,
                stroke=INK,
                stroke_width=2,
                radius=0,
            )
            self.text(
                cursor + field_width / 2,
                y + height / 2 - 2,
                label,
                size=16,
                bold=True,
                anchor="middle",
                max_width=max(20, field_width - 12),
            )
            self.text(
                cursor + field_width / 2,
                y + height / 2 + 22,
                f"{bits} bits",
                size=16,
                anchor="middle",
                max_width=max(20, field_width - 12),
            )
            cursor += field_width

    def save(self) -> tuple[Path, Path]:
        svg_path = (
            self.root / "fig" / "wiki" / self.target.slug
            / f"{self.target.stem}.svg"
        )
        drawio_path = (
            self.root / "fig" / "wiki_src" / self.target.slug
            / f"{self.target.stem}.drawio"
        )
        svg_path.parent.mkdir(parents=True, exist_ok=True)
        drawio_path.parent.mkdir(parents=True, exist_ok=True)
        markers = "\n".join(
            f'<marker id="arrow-{name}" viewBox="0 0 10 8" '
            f'markerWidth="10" markerHeight="8" refX="10" refY="4" '
            f'orient="auto" markerUnits="userSpaceOnUse">'
            f'<path d="M0 0 L10 4 L0 8 Z" fill="{color}"/></marker>'
            for name, color in (
                ("blue", BLUE),
                ("green", GREEN),
                ("amber", AMBER),
                ("red", RED),
                ("purple", PURPLE),
                ("gray", GRAY),
                ("ink", INK),
            )
        )
        style = """
      :root { color-scheme: light dark; }
      @media (prefers-color-scheme: dark) {
        [fill="#FFFFFF"]{fill:#1E2327}[stroke="#FFFFFF"]{stroke:#1E2327}
        [fill="#F8F6EC"]{fill:#252A2E}[stroke="#F8F6EC"]{stroke:#252A2E}
        [fill="#EDF5FF"]{fill:#273846}[stroke="#EDF5FF"]{stroke:#273846}
        [fill="#E7F7EA"]{fill:#24382A}[stroke="#E7F7EA"]{stroke:#24382A}
        [fill="#FFF0D8"]{fill:#3B3122}[stroke="#FFF0D8"]{stroke:#3B3122}
        [fill="#F7DEDC"]{fill:#3D292A}[stroke="#F7DEDC"]{stroke:#3D292A}
        [fill="#EEE9FF"]{fill:#302C3C}[stroke="#EEE9FF"]{stroke:#302C3C}
        [fill="#27313A"]{fill:#ECE7DD}[stroke="#27313A"]{stroke:#ECE7DD}
        [fill="#9AA3AD"]{fill:#747D86}[stroke="#9AA3AD"]{stroke:#747D86}
        [fill="#1769C2"]{fill:#63A8FF}[stroke="#1769C2"]{stroke:#63A8FF}
        [fill="#15803D"]{fill:#63D68B}[stroke="#15803D"]{stroke:#63D68B}
        [fill="#B45309"]{fill:#F0B35A}[stroke="#B45309"]{stroke:#F0B35A}
        [fill="#6D5BD0"]{fill:#A79BF0}[stroke="#6D5BD0"]{stroke:#A79BF0}
        [fill="#B42318"]{fill:#F09B95}[stroke="#B42318"]{stroke:#F09B95}
      }
      .sans{font-family:Arial,Helvetica,sans-serif}
      .mono{font-family:"SFMono-Regular",Consolas,monospace}
      .title{font-size:32px;font-weight:700}
      .subtitle{font-size:18px}
      .heading{font-size:22px;font-weight:700}
      .label{font-size:17px;font-weight:700}
      .body{font-size:16px}
"""
        svg = (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.width}" '
            f'height="{self.height}" viewBox="0 0 {self.width} {self.height}" '
            f'role="img" aria-labelledby="title desc" fill="{INK}" '
            f'data-figure-schema="{SCHEMA}">\n'
            f'  <title id="title">{escape(self.title)}</title>\n'
            f'  <desc id="desc">{escape(self.description)}</desc>\n'
            f'  <defs>{markers}<style>{style}</style></defs>\n'
            f'  <rect x="0" y="0" width="{self.width}" height="{self.height}" '
            f'fill="{WHITE}"/>\n  '
            + "\n  ".join(self._svg)
            + "\n</svg>\n"
        )
        drawio = (
            '<mxfile host="app.diagrams.net" agent="ECG deterministic figure '
            'generator" type="device" compressed="false">\n'
            f'  <diagram id="{self.target.stem}" '
            f'name="{escape(self.title, quote=True)}" '
            f'ecgTitle="{escape(self.title, quote=True)}" '
            f'ecgDescription="{escape(self.description, quote=True)}">\n'
            f'    <mxGraphModel dx="{self.width}" dy="{self.height}" grid="1" '
            f'gridSize="10" guides="1" tooltips="1" connect="1" arrows="1" '
            f'fold="1" page="1" pageScale="1" pageWidth="{self.width}" '
            f'pageHeight="{self.height}" math="0" shadow="0" '
            f'background="none"><root>\n'
            '      <mxCell id="0"/><mxCell id="1" parent="0"/>\n      '
            + "\n      ".join(self._cells)
            + "\n    </root></mxGraphModel>\n  </diagram>\n</mxfile>\n"
        )
        svg_path.write_text(svg, encoding="utf-8")
        drawio_path.write_text(drawio, encoding="utf-8")
        return svg_path, drawio_path


def clean_generated_roots(root: Path) -> None:
    """Remove only generated SVG/Draw.io files, retaining directory roots."""
    for base, suffix in (
        (root / "fig" / "wiki", ".svg"),
        (root / "fig" / "wiki_src", ".drawio"),
    ):
        if not base.exists():
            continue
        for path in sorted(base.rglob(f"*{suffix}")):
            path.unlink()
        for directory in sorted(
            (path for path in base.rglob("*") if path.is_dir()),
            reverse=True,
        ):
            if not any(directory.iterdir()):
                directory.rmdir()
