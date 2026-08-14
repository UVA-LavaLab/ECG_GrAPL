"""Validate the public README and wiki surface."""

from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_PAGES = (
    ROOT / "README.md",
    ROOT / "CONTRIBUTING.md",
    ROOT / "wiki/Home.md",
    ROOT / "wiki/ReusePlan-FlowThrough.md",
    ROOT / "wiki/Evaluation-Methodology.md",
    ROOT / "wiki/Reproduction.md",
    ROOT / "wiki/Repository-Hygiene.md",
)


def test_public_pages_use_direct_project_language():
    forbidden = (
        "hpca",
        "go/no-go",
        "claim gate",
    )
    for path in PUBLIC_PAGES:
        text = path.read_text(errors="ignore").lower()
        for term in forbidden:
            assert term not in text, f"{path} contains {term!r}"


def test_public_links_resolve():
    tracked = set(subprocess.run(
        ["git", "ls-files"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines())
    missing = []
    untracked = []
    for page in PUBLIC_PAGES:
        text = page.read_text(errors="ignore")
        for target in re.findall(
                r"!?(?:\[[^\]]*\])\(([^)]+)\)", text):
            target = target.split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            resolved = (page.parent / target).resolve()
            if (
                    not resolved.exists() and
                    page.parent == ROOT / "wiki" and
                    not Path(target).suffix):
                resolved = resolved.with_suffix(".md")
            if not resolved.exists():
                missing.append((str(page), target))
                continue
            relative = str(resolved.relative_to(ROOT))
            if relative not in tracked:
                untracked.append((str(page), target))
    assert missing == []
    assert untracked == []


def test_wiki_page_links_use_rendered_slugs():
    for page in (ROOT / "wiki").glob("*.md"):
        text = page.read_text(errors="ignore")
        for target in re.findall(
                r"!?(?:\[[^\]]*\])\(([^)]+)\)", text):
            target = target.split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            if (page.parent / target).suffix == ".md":
                assert not (page.parent / target).exists(), (
                    f"{page} links to raw wiki source {target!r}")


def test_design_guide_uses_aligned_instruction_family():
    guide = (ROOT / "wiki/ReusePlan-FlowThrough.md").read_text()
    for mnemonic in (
            "ecg.plan.load",
            "ecg.flow.load",
            "ecg.bind.load",
            "ecg.bind.iload"):
        assert mnemonic in guide


def test_svg_figures_are_valid_and_use_straight_connectors():
    figures = sorted((ROOT / "wiki/assets").glob("*.svg"))
    assert len(figures) >= 7
    for path in figures:
        ET.parse(path)
        if path.name == "logo.svg":
            continue
        text = path.read_text(errors="ignore")
        path_data = re.findall(
            r"<path\b[^>]*\sd=\"([^\"]+)\"", text)
        assert all(
            not re.search(r"[CcSsQqTtAa]", data)
            for data in path_data)
        assert "markerUnits=\"userSpaceOnUse\"" in text or (
            "marker-end" not in text)


def test_public_tree_has_no_tracked_research_directory():
    tracked = subprocess.run(
        ["git", "ls-files", "research"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert tracked == ""
