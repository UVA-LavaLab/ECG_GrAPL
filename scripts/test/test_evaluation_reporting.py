"""Checks for the public evaluation and reporting methodology."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
METHODOLOGY = ROOT / "wiki/Evaluation-Methodology.md"


def methodology() -> str:
    return METHODOLOGY.read_text(errors="ignore")


def evaluation_section() -> str:
    return " ".join(methodology().lower().split())


def test_primary_quantities_are_time_and_traffic():
    section = evaluation_section()
    assert "gem5 o3 execution time" in section
    assert "total off-chip traffic" in section
    assert "always reported together" in section
    assert "demand, prefetch, metadata, writeback" in section


def test_aggregation_and_matching_baselines_are_explicit():
    section = evaluation_section()
    assert "geometric mean" in section
    assert "+/-2%" in section
    assert "matching baseline from the same invocation and build" in section


def test_non_architectural_rows_are_excluded_from_timing_ratios():
    section = evaluation_section()
    assert "timing_valid_for_speedup=0" in section
    assert "excluded from timing ratios" in section


def test_prefetch_and_idealized_models_are_interpreted_correctly():
    section = evaluation_section()
    assert "demand misses alone are not performance evidence" in section
    assert "execution time and total off-chip traffic" in section
    assert "upper bound rather than as measured hardware performance" in (
        section)
    assert "mshr" in section


def test_instruction_count_separates_complete_design_from_replacement():
    section = evaluation_section()
    assert "complete-design comparisons" in section
    assert "replacement-only attribution" in section
    assert "exact per-cell instruction equality" in section
    assert "ipc is derived" in section
    assert "counterfactual instruction normalization" in section


def test_popt_analytic_timing_is_described_as_an_optimistic_bound():
    section = evaluation_section()
    assert "popt_target_time_charged=0" in section
    assert "matrix-stream latency is omitted" in section
    assert "optimistic p-opt bound" in section
    assert "realistic target-time implementation" in section


def test_preliminary_results_are_not_published_in_public_documents():
    for path in (
            ROOT / "README.md",
            ROOT / "wiki/K2-StreamShield.md",
            ROOT / "wiki/Evaluation-Methodology.md",
            ROOT / "wiki/Reproduction.md"):
        text = path.read_text(errors="ignore")
        for phrase in (
                "Result: STOP",
                "Current STOP",
                "proposal_sota_v2_",
                "0.9061",
                "0.9835",
                "1.0235"):
            assert phrase not in text, f"{path} publishes {phrase!r}"
