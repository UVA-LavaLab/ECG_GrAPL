#!/usr/bin/env python3
"""Export deterministic, tightly cropped vector PDFs for ECG paper figures."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[2]
SVG_ROOT = ROOT / "fig" / "paper" / "ecg-paper"
ID_PATTERN = re.compile(
    rb"/ID\s*\[\s*<[0-9A-Fa-f]+>\s*<[0-9A-Fa-f]+>\s*\]"
)
EOF = b"%%EOF"


def required_tool(name: str, alternatives: tuple[str, ...] = ()) -> str:
    for candidate in (name, *alternatives):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    options = ", ".join((name, *alternatives))
    raise RuntimeError(f"missing required PDF export tool: {options}")


def svg_metadata(path: Path) -> tuple[int, int, str, str]:
    root = ET.parse(path).getroot()
    width = int(float(root.get("width", "0")))
    height = int(float(root.get("height", "0")))
    namespace = "{http://www.w3.org/2000/svg}"
    title_node = root.find(f"{namespace}title")
    title = (
        "".join(title_node.itertext()).strip()
        if title_node is not None else path.stem
    )
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if width <= 0 or height <= 0:
        raise RuntimeError(f"{path}: invalid SVG canvas")
    return width, height, title, digest


def html_url(svg: str, width: int, height: int) -> str:
    html = (
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"color-scheme\" content=\"light only\">"
        "<style>"
        f"@page{{size:{width}px {height}px;margin:0}}"
        f"html,body{{margin:0;width:{width}px;height:{height}px;"
        "overflow:hidden;color-scheme:only light}}"
        f"svg{{display:block;width:{width}px;height:{height}px}}"
        "</style></head><body>"
        f"{svg}</body></html>"
    )
    return "data:text/html;charset=utf-8," + urllib.parse.quote(html)


def pdfmark_literal(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace("(", "\\(")
        .replace(")", "\\)")
    )


def normalize_pdf(
    source: Path,
    destination: Path,
    *,
    title: str,
    digest: str,
    width: int,
    height: int,
) -> None:
    environment = dict(os.environ)
    environment.update({"SOURCE_DATE_EPOCH": "0", "TZ": "UTC"})
    pdfmark = (
        "[ "
        f"/Title ({pdfmark_literal(title)}) "
        "/Creator (ECG deterministic paper figure exporter) "
        f"/Subject (ecg-source-sha256:{digest}) "
        "/DOCINFO pdfmark"
    )
    subprocess.run(
        [
            required_tool("gs"),
            "-q",
            "-dBATCH",
            "-dNOPAUSE",
            "-dSAFER",
            "-sDEVICE=pdfwrite",
            "-dCompatibilityLevel=1.4",
            "-dAutoRotatePages=/None",
            f"-sOutputFile={destination}",
            "-f",
            str(source),
            "-c",
            pdfmark,
        ],
        cwd=ROOT,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    data = destination.read_bytes()
    identifier = digest[:32].upper().encode("ascii")
    replacement = b"/ID [<" + identifier + b"><" + identifier + b">]"
    data, replacements = ID_PATTERN.subn(replacement, data)
    if replacements != 1:
        raise RuntimeError(
            f"{destination}: expected one PDF identifier, found {replacements}"
        )
    marker = (
        f"% ECG-SOURCE-SHA256:{digest}\n"
        f"% ECG-CANVAS:{width}x{height}\n"
    ).encode("ascii")
    position = data.rfind(EOF)
    if position < 0:
        raise RuntimeError(f"{destination}: PDF end marker is missing")
    destination.write_bytes(data[:position] + marker + data[position:])


def export_one(svg_path: Path, pdf_path: Path) -> None:
    chrome = required_tool(
        "google-chrome", ("chromium", "chromium-browser"))
    width, height, title, digest = svg_metadata(svg_path)
    svg = svg_path.read_text(encoding="utf-8")
    with TemporaryDirectory(prefix="ecg-paper-pdf-") as temporary:
        temporary_root = Path(temporary)
        raw_pdf = temporary_root / "raw.pdf"
        normalized_pdf = temporary_root / "normalized.pdf"
        profile = temporary_root / "chrome-profile"
        subprocess.run(
            [
                chrome,
                "--headless",
                "--disable-background-networking",
                "--disable-dev-shm-usage",
                "--disable-extensions",
                "--disable-features=WebContentsForceDark",
                "--disable-gpu",
                "--disable-sync",
                "--force-color-profile=srgb",
                "--hide-scrollbars",
                "--metrics-recording-only",
                "--no-first-run",
                "--no-pdf-header-footer",
                "--no-sandbox",
                f"--user-data-dir={profile}",
                f"--print-to-pdf={raw_pdf}",
                html_url(svg, width, height),
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )
        normalize_pdf(
            raw_pdf,
            normalized_pdf,
            title=title,
            digest=digest,
            width=width,
            height=height,
        )
        pdf_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(normalized_pdf, pdf_path)


def expected_svgs() -> list[Path]:
    paths = sorted(SVG_ROOT.glob("ecg-paper-f*.svg"))
    if len(paths) != 6:
        raise RuntimeError(
            f"expected six paper SVGs under {SVG_ROOT}, found {len(paths)}"
        )
    return paths


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="Regenerate PDFs privately and compare them byte-for-byte.",
    )
    args = parser.parse_args()
    paths = expected_svgs()
    if args.check:
        with TemporaryDirectory(prefix="ecg-paper-pdf-check-") as temporary:
            temporary_root = Path(temporary)
            for svg_path in paths:
                generated = temporary_root / f"{svg_path.stem}.pdf"
                export_one(svg_path, generated)
                expected = svg_path.with_suffix(".pdf")
                if not expected.is_file():
                    raise SystemExit(f"missing generated PDF: {expected}")
                if generated.read_bytes() != expected.read_bytes():
                    raise SystemExit(
                        f"generated PDF differs: {expected.relative_to(ROOT)}"
                    )
        return 0

    for stale in SVG_ROOT.glob("*.pdf"):
        stale.unlink()
    for svg_path in paths:
        pdf_path = svg_path.with_suffix(".pdf")
        export_one(svg_path, pdf_path)
        print(pdf_path.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(f"PDF export failed: {error}", file=sys.stderr)
        raise SystemExit(1)
