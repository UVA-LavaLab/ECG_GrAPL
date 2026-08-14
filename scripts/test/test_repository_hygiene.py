"""Prevent local data, simulator checkouts, and binaries from being tracked."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def tracked_files() -> list[str]:
    return subprocess.run(
        ["git", "ls-files"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()


def test_local_only_paths_are_not_tracked():
    forbidden_prefixes = (
        "research/",
        "results/",
        "build/",
        "m5out/",
        "bench/bin/",
        "bench/bin_sim/",
        "bench/bin_gem5/",
        "bench/bin_sniper/",
        "bench/include/gem5_sim/gem5/",
        "bench/include/sniper_sim/snipersim/",
    )
    offenders = [
        path for path in tracked_files()
        if path.startswith(forbidden_prefixes)
    ]
    assert offenders == []


def test_retired_process_files_are_not_tracked():
    tracked = set(tracked_files())
    retired = {
        "wiki/ECG-HPCA-Paper.md",
        "scripts/experiments/ecg/flows/freeze_proposal_reuse_bind.py",
        "scripts/test/test_ecg_paper_ssot.py",
        "scripts/test/test_frozen_metrics.py",
    }
    assert tracked.isdisjoint(retired)


def test_prompt_and_tool_instruction_artifacts_are_not_tracked():
    offenders = [
        path for path in tracked_files()
        if path.startswith(".github/prompts/") or
        path.endswith(".prompt.md") or
        (
            path.startswith(".github/") and
            "instructions" in Path(path).name.lower()
        )
    ]
    assert offenders == []


def test_public_documentation_and_figures_are_tracked():
    tracked = set(tracked_files())
    required = {
        "README.md",
        "wiki/Home.md",
        "wiki/ReusePlan-FlowThrough.md",
        "wiki/RISC-V-Instruction-Path.md",
        "wiki/Evaluation-Methodology.md",
        "wiki/Reproduction.md",
        "wiki/Repository-Hygiene.md",
        "scripts/experiments/ecg/configs/pagerank_study.json",
    }
    required.update(
        str(path.relative_to(ROOT))
        for path in (ROOT / "wiki/assets").glob("*.svg"))
    assert required <= tracked


def test_generated_data_extensions_are_not_tracked():
    forbidden_suffixes = (".el", ".wel", ".sg", ".mtx", ".log")
    offenders = [
        path for path in tracked_files()
        if path.endswith(forbidden_suffixes) and
        not path.startswith("scripts/test/data/")
    ]
    assert offenders == []


def test_no_large_binary_blob_is_tracked():
    limit = 20 * 1024 * 1024
    offenders = []
    for row in subprocess.run(
            ["git", "ls-files", "-s"],
            cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.splitlines():
        _, object_id, _, relative = row.split(maxsplit=3)
        size = int(subprocess.run(
            ["git", "cat-file", "-s", object_id],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout)
        if size > limit:
            offenders.append((relative, size))
    assert offenders == []


def test_research_is_ignored_and_clean_all_preserves_it():
    gitignore = (ROOT / ".gitignore").read_text().splitlines()
    assert "research/" in gitignore
    makefile = (ROOT / "Makefile").read_text()
    clean_all = makefile.split("clean-all:", 1)[1]
    removal_lines = [
        line for line in clean_all.splitlines()
        if line.lstrip().startswith("rm ")]
    assert all("research/" not in line for line in removal_lines)
    assert all("results/" not in line for line in removal_lines)
    assert "research/ and results/ preserved" in clean_all


def test_public_setup_documentation_names_both_simulators():
    text = (ROOT / "wiki/Reproduction.md").read_text()
    assert "make setup-gem5" in text
    assert "make setup-sniper" in text


def test_authored_evaluation_code_avoids_process_jargon():
    roots = (
        "scripts/experiments/ecg/",
        "wiki/",
        "bench/src_sim/",
        "bench/src_gem5/",
        "bench/src_sniper/",
        "bench/include/cache_sim/",
        "bench/include/gem5_sim/",
        "bench/include/sniper_sim/",
        "bench/include/graphbrew/reorder/",
    )
    root_files = {"README.md", "CONTRIBUTING.md"}
    forbidden = (
        "ssot",
        "sprint ",
        "headline",
        "paper-run",
        "rubber-duck",
        "go/no-go",
        "claim gate",
        "submission",
        "reviewer",
        "reviewer-facing",
        "recovery plan",
        "findings ",
    )
    offenders = []
    for relative in tracked_files():
        if relative not in root_files and not relative.startswith(roots):
            continue
        text = (ROOT / relative).read_text(errors="ignore").lower()
        if any(term in text for term in forbidden):
            offenders.append(relative)
    assert offenders == []


def test_retired_mechanism_names_are_absent():
    forbidden_content = (
        "K" + "2",
        "Stream" + "Shield",
        "stream" + "shield",
        "ecg.k" + "2",
        "ecg.stream.load" + "2",
        "m" + "load",
        "load" + "2",
        "stream-" + "bypass",
        "structural-" + "bypass",
        "--schedule-" + "k",
        "PLACE_" + "SHIELD",
        "--ecg-isa-variant " + "mask",
    )
    forbidden_paths = (
        "k" + "2",
        "stream" + "shield",
        "stream_" + "bypass",
        "epoch_" + "pair",
    )
    offenders = []
    for relative in tracked_files():
        if relative == "wiki/assets/logo.svg" or relative.startswith(
                ("bench/include/external/", "bench/include/graphbrew/")):
            continue
        if any(term in relative.lower() for term in forbidden_paths):
            offenders.append(relative)
            continue
        text = (ROOT / relative).read_text(errors="ignore")
        if any(term in text for term in forbidden_content):
            offenders.append(relative)
    assert offenders == []
