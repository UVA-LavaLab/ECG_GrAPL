"""Validate the public README and wiki surface."""

from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_PAGES = (
    ROOT / "README.md",
    ROOT / "CONTRIBUTING.md",
    ROOT / "fig/README.md",
    ROOT / "wiki/Home.md",
    ROOT / "wiki/ReusePlan-FlowThrough.md",
    ROOT / "wiki/RISC-V-Instruction-Path.md",
    ROOT / "wiki/Property-to-Cache-Walkthrough.md",
    ROOT / "wiki/Evaluation-Methodology.md",
    ROOT / "wiki/Related-Work.md",
    ROOT / "wiki/Reproduction.md",
    ROOT / "wiki/Repository-Hygiene.md",
)


def test_public_pages_use_direct_project_language():
    direct_language_pages = (
        ROOT / "README.md",
        ROOT / "fig/README.md",
        *(ROOT / "wiki").glob("*.md"),
    )
    forbidden_phrases = (
        "reading spine",
        "reader graph",
        "future readers",
        "incoming-neighbor",
        "outgoing-neighbor",
        "access-source vertex",
        "paper-first",
        "publication vector",
        "whole cells",
        "audit inspection",
        "thin cache_sim",
        "closest generic",
        "closest design",
        "rrip_first victim",
    )
    for path in direct_language_pages:
        text = path.read_text(errors="ignore").lower()
        for phrase in forbidden_phrases:
            assert phrase not in text, f"{path} contains {phrase!r}"

    operational_forbidden = (
        "hpca",
        "go/no-go",
        "claim gate",
    )
    for path in (
            ROOT / "README.md",
            ROOT / "wiki/Home.md",
            ROOT / "wiki/ReusePlan-FlowThrough.md",
            ROOT / "wiki/RISC-V-Instruction-Path.md",
            ROOT / "wiki/Evaluation-Methodology.md",
            ROOT / "wiki/Reproduction.md",
            ROOT / "wiki/Repository-Hygiene.md"):
        text = path.read_text(errors="ignore").lower()
        for term in operational_forbidden:
            assert term not in text, f"{path} contains {term!r}"


def test_graph_direction_language_is_mathematically_explicit():
    readme = (ROOT / "README.md").read_text()
    guide = (ROOT / "wiki/ReusePlan-FlowThrough.md").read_text()
    for text in (readme, guide):
        assert "N_in(v)" in text
        assert "d_in(v)" in text
        assert "N_out(v)" in text
        assert "d_out(v)" in text
    assert "property `p[v]` is read once for each source" in readme
    assert "property-request count is therefore `d_out(v)`" in readme


def test_reproduction_requires_the_iteration_one_gate():
    reproduction = (ROOT / "wiki/Reproduction.md").read_text()
    assert "--phase early-stop" in reproduction
    assert '"iteration_8_authorized": true' in reproduction
    assert reproduction.index("--phase early-stop") < reproduction.index(
        "--only 91")


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


def test_readme_documents_experimental_riscv_support():
    readme = (ROOT / "README.md").read_text()
    flat = " ".join(readme.split())
    assert "experimental RISC-V custom-0 implementation" in flat
    assert "wiki/RISC-V-Instruction-Path.md" in readme
    assert "not a ratified RISC-V extension" in flat


def test_svg_figures_are_valid_and_use_straight_connectors():
    figures = sorted((ROOT / "fig/wiki").rglob("*.svg"))
    assert len(figures) == 13
    for path in figures:
        ET.parse(path)
        text = path.read_text(errors="ignore")
        path_data = re.findall(
            r"<path\b[^>]*\sd=\"([^\"]+)\"", text)
        assert all(
            not re.search(r"[CcSsQqTtAa]", data)
            for data in path_data)
        assert "markerUnits=\"userSpaceOnUse\"" in text or (
            "marker-end" not in text)
        assert 'data-figure-schema="ecg-public/v1"' in text
        assert 'role="img"' in text


def test_public_tree_has_no_tracked_research_directory():
    tracked = subprocess.run(
        ["git", "ls-files", "research"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert tracked == ""
