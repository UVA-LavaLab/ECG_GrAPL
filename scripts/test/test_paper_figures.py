"""Structural locks for ECG's compact conference-paper figure set."""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SVG_ROOT = ROOT / "fig/paper/ecg-paper"
DRAWIO_ROOT = ROOT / "fig/paper_src/ecg-paper"
SVG_NS = "http://www.w3.org/2000/svg"

EXPECTED_STEMS = (
    "ecg-paper-f01-offline-plan",
    "ecg-paper-f02-compact-record",
    "ecg-paper-f03-request-path",
    "ecg-paper-f04-llc-decision",
    "ecg-paper-f05-flowthrough",
    "ecg-paper-f06-evidence-boundary",
)
CSS_CLASS = re.compile(r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}")
CSS_SIZE = re.compile(r"font-size\s*:\s*(\d+(?:\.\d+)?)px")
RESULT_NUMBER = re.compile(r"\d+(?:\.\d+)?\s*(?:%|x speedup|percent)")


def wiki_asset_digest() -> dict[str, str]:
    return {
        str(path.relative_to(ROOT)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for base, pattern in (
            (ROOT / "fig/wiki", "*.svg"),
            (ROOT / "fig/wiki_src", "*.drawio"),
        )
        for path in sorted(base.rglob(pattern))
    }


def svg_roots() -> dict[str, ET.Element]:
    return {
        path.stem: ET.parse(path).getroot()
        for path in sorted(SVG_ROOT.glob("*.svg"))
    }


def visible_text(root: ET.Element) -> str:
    return " ".join(
        " ".join("".join(node.itertext()).split())
        for node in root.iter()
        if node.tag.rsplit("}", 1)[-1] == "text"
        and "".join(node.itertext()).strip()
    )


def test_paper_set_is_exactly_six_svgs_and_six_mirrors():
    svgs = sorted(path.stem for path in SVG_ROOT.glob("*.svg"))
    pdfs = sorted(path.stem for path in SVG_ROOT.glob("*.pdf"))
    mirrors = sorted(path.stem for path in DRAWIO_ROOT.glob("*.drawio"))
    assert tuple(svgs) == EXPECTED_STEMS
    assert tuple(pdfs) == EXPECTED_STEMS
    assert tuple(mirrors) == EXPECTED_STEMS
    assert not list((ROOT / "fig/paper").glob("*.svg"))
    assert sorted(
        path.name for path in (ROOT / "fig/paper").iterdir()
    ) == ["ecg-paper"]


def test_paper_figures_are_compact_landscape_plates():
    for stem, root in svg_roots().items():
        width = float(root.get("width", "0"))
        height = float(root.get("height", "0"))
        assert width == 1200, stem
        assert 420 <= height <= 650, (stem, height)
        assert height < width, stem
        assert root.get("data-figure-schema") == "ecg-public/v1", stem
        assert root.find(f"{{{SVG_NS}}}title") is not None, stem
        assert len(
            "".join(root.find(f"{{{SVG_NS}}}desc").itertext()).strip()
        ) >= 80, stem


def test_paper_live_text_never_drops_below_17px():
    for stem, root in svg_roots().items():
        source = "\n".join(
            node.text or ""
            for node in root.iter()
            if node.tag.rsplit("}", 1)[-1] == "style"
        )
        sizes = {
            name: float(CSS_SIZE.search(body).group(1))
            for name, body in CSS_CLASS.findall(source)
            if CSS_SIZE.search(body)
        }
        used = [
            next(
                (sizes[item] for item in node.get("class", "").split()
                 if item in sizes),
                16.0,
            )
            for node in root.iter()
            if node.tag.rsplit("}", 1)[-1] == "text"
            and "".join(node.itertext()).strip()
        ]
        assert used, stem
        assert min(used) >= 17, (stem, min(used))


def test_paper_figures_use_precise_architecture_terminology():
    figures = {stem: visible_text(root) for stem, root in svg_roots().items()}
    assert all(
        token in figures["ecg-paper-f01-offline-plan"]
        for token in (
            "outer vertex", "row_ptr[u]", "col_idx", "N_in(u)", "N_out(u)",
            "pull d_out(v); push d_in(v)",
            "Edge-aligned ReusePlan", "measured ROI boundary",
        )
    )
    compact = figures["ecg-paper-f02-compact-record"]
    for token in (
        "destination", "tier", "epoch1", "epoch2",
        "18 + 0 + 7 + 7 = 32 bits", "sidecar v2 binds the tier width",
        "only tier_bits 0 or 2 decode", "width mismatch aborts the run",
        "4-byte compact record replaces the 4-byte CSR edge word",
    ):
        assert token in compact
    request = figures["ecg-paper-f03-request-path"]
    for token in (
        "AGU (address generation)", "LSQ (load/store queue)",
        "record-block MSHR", "property MSHR", "LLC fill decision",
        "LLC stamp decision", "Request extension: ReuseBind",
        "ecg.flow.load.compact", "ecg.bind.load.u32",
    ):
        assert token in request
    llc = figures["ecg-paper-f04-llc-decision"]
    for token in (
        "Accept ReuseBind?", "Line-local metadata", "rrpvMax",
        "oldest non-property", "distance over property",
        "unstamped distance = 0",
        "distance(e,c) = (e + N - (c mod N)) mod N",
    ):
        assert token in llc
    flow = figures["ecg-paper-f05-flowthrough"]
    for token in (
        "Unchanged lookup and service", "MSHR target allocation",
        "target carries allocOnFill", "combine target bits with OR",
        "a cache hit takes no fill decision", "Miss: all targets no-allocate",
        "Corner case: an allocating merge target",
    ):
        assert token in flow
    evidence = figures["ecg-paper-f06-evidence-boundary"]
    for token in (
        "gem5 O3 timing", "cache_sim traffic", "Sniper matched work",
        "Analytic P-OPT bound", "Mechanism receipt gate",
        "Semantic receipt gate", "popt_target_time_charged = 0",
    ):
        assert token in evidence


def test_paper_figures_state_no_measured_result():
    for stem, root in svg_roots().items():
        text = visible_text(root)
        assert not RESULT_NUMBER.findall(text), stem
        lowered = text.lower()
        for term in (
            "faster", "outperform", "novel", "significant", "seamless",
            "state-of-the-art", "9.1", "7.3",
        ):
            assert term not in lowered, (stem, term)


def test_paper_generation_leaves_wiki_assets_untouched():
    before = wiki_asset_digest()
    assert before
    generation = subprocess.run(
        [sys.executable, "scripts/docs/generate_ecg_paper_figures.py"],
        cwd=ROOT, capture_output=True, text=True, timeout=180,
    )
    assert generation.returncode == 0, generation.stdout + generation.stderr
    assert wiki_asset_digest() == before
    wiki_check = subprocess.run(
        [sys.executable, "scripts/docs/generate_ecg_figures.py", "--check"],
        cwd=ROOT, capture_output=True, text=True, timeout=300,
    )
    assert wiki_check.returncode == 0, wiki_check.stdout + wiki_check.stderr


def test_paper_figure_contract_and_determinism():
    result = subprocess.run(
        [sys.executable, "scripts/docs/check_ecg_paper_figures.py"],
        cwd=ROOT, capture_output=True, text=True, timeout=300,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "validated 6 ECG paper figures" in result.stdout


def test_paper_pdfs_are_tight_vector_exports_of_the_svgs():
    source_pattern = re.compile(
        rb"% ECG-SOURCE-SHA256:([0-9a-f]{64})")
    canvas_pattern = re.compile(rb"% ECG-CANVAS:(\d+)x(\d+)")
    media_box_pattern = re.compile(
        rb"/MediaBox\s*\[\s*0\s+0\s+([0-9.]+)\s+([0-9.]+)\s*\]")
    for svg in sorted(SVG_ROOT.glob("*.svg")):
        pdf = svg.with_suffix(".pdf")
        data = pdf.read_bytes()
        assert data.startswith(b"%PDF-"), pdf
        source = source_pattern.search(data)
        assert source, pdf
        assert source.group(1).decode() == hashlib.sha256(
            svg.read_bytes()).hexdigest()
        root = ET.parse(svg).getroot()
        width = int(float(root.get("width", "0")))
        height = int(float(root.get("height", "0")))
        canvas = canvas_pattern.search(data)
        assert canvas and tuple(map(int, canvas.groups())) == (width, height)
        media_box = media_box_pattern.search(data)
        assert media_box, pdf
        points = tuple(float(value) for value in media_box.groups())
        assert abs(points[0] - width * 0.75) <= 0.2
        assert abs(points[1] - height * 0.75) <= 0.2
        assert b"/Font" in data


def test_paper_pdf_export_is_deterministic_when_tools_are_available():
    if not shutil.which("gs") or not (
        shutil.which("google-chrome")
        or shutil.which("chromium")
        or shutil.which("chromium-browser")
    ):
        return
    result = subprocess.run(
        [
            sys.executable,
            "scripts/docs/export_ecg_paper_pdfs.py",
            "--check",
        ],
        cwd=ROOT, capture_output=True, text=True, timeout=900,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_paper_register_documents_every_asset():
    register = (ROOT / "fig/README.md").read_text(encoding="utf-8")
    assert "## Conference-paper figure set" in register
    assert "scripts/docs/generate_ecg_paper_figures.py" in register
    assert "scripts/docs/export_ecg_paper_pdfs.py" in register
    assert "scripts/docs/check_ecg_paper_figures.py" in register
    for stem in EXPECTED_STEMS:
        assert f"paper/ecg-paper/{stem}.svg" in register
        assert f"paper/ecg-paper/{stem}.pdf" in register
        assert f"paper_src/ecg-paper/{stem}.drawio" in register
