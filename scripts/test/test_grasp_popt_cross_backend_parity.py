"""Cross-backend GRASP/P-OPT parity checks.

These checks intentionally run NO Sniper or gem5 simulation. They pin the
shared, byte-identical ecg_victim_policy.h GRASP tier classifier at the
current 0.15 vertex-space fraction by exercising cache_sim's compiled copy
(proven byte-identical to gem5/Sniper's copies by test_shared_ecg_policy
.py's test_all_copies_byte_identical), and guard several related invariants:

* the fixed 0.15 GRASP hot-fraction boundary math (classifyGraspTier),
* removal of the dead legacy "GRASP 0.5 of LLC" auto-tuning code path,
* Sniper's required sniper_context_loaded/sniper_rereference_loaded check
  for GRASP/P-OPT staying intact,
* the documented frontier-kernel (BFS/SSSP) P-OPT rereference-matrix scope
  caveat remaining an explicit, honest limitation rather than something a
  future edit silently reframes as "fixed".

BINARY-DEPENDENT SKIP LIMITATION: the two tests that actually execute the
shared classifier/selector code
(test_grasp_tier_classification_pins_015_vertex_space_fraction and
test_online_dueling_five_arm_selection_pins_shared_selector) require the
pre-built cache_sim `bench/bin_sim/test_ecg_victim` binary and are marked
`pytest.mark.skipif` -- SKIPPED, not failed -- whenever that binary is
missing (e.g. a fresh checkout, or `make sim-test_ecg_victim` was never
run). A skip in either of those two tests is silent: it does NOT mean the
0.15 GRASP-tier boundary or the five-arm selector were verified, only that
they were not checked this run. Run `make sim-test_ecg_victim` first (or
check this module's output for "SKIPPED") before treating a green run of
this file as confirming those two invariants. The remaining three tests
(dead-code removal, the Sniper context/rereference gate, and the frontier
caveat wording) are pure static/source checks with no binary dependency and
always run.
"""
from __future__ import annotations

import ast
import os
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TEST_ECG_VICTIM = ROOT / "bench/bin_sim/test_ecg_victim"
GRAPH_CACHE_CONTEXT = ROOT / "bench/include/cache_sim/graph_cache_context.h"
ROI_MATRIX = ROOT / "scripts/experiments/ecg/roi_matrix.py"
FRONTIER_CAVEAT_DOC = ROOT / "wiki/Evaluation-Methodology.md"


def read(path: Path) -> str:
    return path.read_text(errors="ignore")


# NOTE (binary-dependent skip): this mark SKIPS (does not fail) the two
# tests below when bench/bin_sim/test_ecg_victim has not been built; see
# the module docstring's "BINARY-DEPENDENT SKIP LIMITATION" section.
pytestmark_skip_binary = pytest.mark.skipif(
    not TEST_ECG_VICTIM.exists(),
    reason="cache_sim test_ecg_victim binary not built "
           "(make sim-test_ecg_victim); this SKIPS rather than fails, so a "
           "green run of this module does not by itself confirm the 0.15 "
           "GRASP-tier boundary or five-arm selector invariants -- see the "
           "module docstring")


@pytestmark_skip_binary
def test_grasp_tier_classification_pins_015_vertex_space_fraction():
    """Runs the shared ecg_policy::classifyGraspTier/graspTierRRPV harness.

    cache_sim, gem5 and Sniper all call this SAME header function (verified
    byte-identical by test_shared_ecg_policy.py), so pinning cache_sim's
    compiled behavior at hot_fraction=0.15 is a cross-backend pin without
    requiring a gem5/Sniper simulation run. A regression in the shared
    header's boundary math (e.g. a reversion to the historical fixed-0.50-
    of-LLC GRASP default) would fail this test on any of the three sims.
    """
    env = dict(os.environ)
    env["ECG_VARIANT"] = "tier"
    result = subprocess.run(
        [str(TEST_ECG_VICTIM)], env=env,
        capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, (
        f"GRASP tier classification regressed:\n{result.stdout}\n"
        f"{result.stderr}")
    assert "RESULT[tier]: 16 passed, 0 failed" in result.stdout, result.stdout


@pytestmark_skip_binary
def test_online_dueling_five_arm_selection_pins_shared_selector():
    """Runs the shared OnlineDuelingSelector five-arm leader/follower harness.

    This is the SAME ecg_policy::OnlineDuelingSelector both gem5's
    GraphEcgRP and Sniper's CacheSetECG call (variantForSet/recordMiss/
    winnerArm/duelingLeaderArm), so pinning it here cross-checks the
    population semantics both sims' new evidence counters rely on.
    """
    env = dict(os.environ)
    env["ECG_VARIANT"] = "dueling"
    result = subprocess.run(
        [str(TEST_ECG_VICTIM)], env=env,
        capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, (
        f"online-dueling five-arm selection regressed:\n{result.stdout}\n"
        f"{result.stderr}")
    assert "RESULT[dueling]: 7 passed, 0 failed" in result.stdout, (
        result.stdout)


@pytestmark_skip_binary
def test_online_admission_selector_uses_access_normalized_rates():
    env = dict(os.environ)
    env["ECG_VARIANT"] = "admission_dueling"
    result = subprocess.run(
        [str(TEST_ECG_VICTIM)], env=env,
        capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "RESULT[admission_dueling]: 7 passed, 0 failed" in result.stdout


def test_dead_legacy_grasp_auto_tune_hot_fraction_is_removed():
    """Guard against reintroducing the dead legacy GRASP 0.5-of-LLC default.

    ``autoComputeHotFraction`` was an unused private method (repo-wide grep
    found zero callers) implementing a superseded auto-tuning approach whose
    sanity clamp topped out at 0.50 -- the historical "fixed 0.50-of-LLC"
    GRASP default that under-protected large graphs, before the current
    switched to the fixed 0.15 vertex-space fraction used everywhere today
    (cache_sim/gem5/Sniper). It has been removed as dead code; this test
    fails if it (or an equivalent auto-tuning entry point) is reintroduced
    without also being wired into a real caller.
    """
    text = read(GRAPH_CACHE_CONTEXT)
    assert "autoComputeHotFraction" not in text, (
        "the dead legacy GRASP auto-hot-fraction method (0.50-of-LLC "
        "clamp) has reappeared; either wire it into a real caller and "
        "cover it with a test, or keep it removed")
    # The fixed reference-compatible default must remain 0.15 everywhere it is
    # declared as a literal default (not a clamp/sanity bound elsewhere).
    assert "grasp_hot_fraction = 0.15" in text
    assert "grasp_hot_percent = 15" in text


def test_sniper_grasp_popt_context_gate_remains_fail_closed():
    """Sniper's GRASP/P-OPT context+rereference gate must still fail closed.

    Regression guard for the roi_matrix.py integration: the
    pre-existing sniper_context_loaded / sniper_rereference_loaded checks
    (which already fail the row when Sniper completes without a loaded
    graph context, or when P-OPT completes without a loaded rereference
    matrix) must remain intact and unconditional -- they run before any of
    the new Sniper ReusePlan online-dueling / variant-receipt / geometry-receipt
    related evidence code.
    """
    text = read(ROI_MATRIX)
    assert (
        "Sniper graph policy completed without a loaded graph context"
        in text)
    assert "Sniper P-OPT completed without a loaded rereference matrix" in text
    assert 'row["sniper_context_loaded"] = int(context_loaded)' in text
    assert 'row["sniper_rereference_loaded"] = reref_loaded' in text
    # The is_reuse_plan_ecg-gated receipt/evidence code must sit
    # AFTER the fail-closed context gate, not before it (an early return on
    # a missing context must short-circuit before any evidence is trusted).
    #
    # This is checked semantically via the AST rather than by matching an
    # exact, indentation/line-wrap-coupled source substring: comparing the
    # line numbers of the parsed AST nodes survives any reformatting
    # (reindentation, black/autopep8 reflow, argument-wrapping changes)
    # that preserves the underlying statement order, whereas a literal
    # multi-line string match (e.g. containing a hardcoded newline +
    # indentation width) would spuriously fail on a purely cosmetic reflow.
    tree = ast.parse(text, filename=str(ROI_MATRIX))

    def _first_string_literal_lineno(needle: str) -> int:
        matches = [
            node.lineno for node in ast.walk(tree)
            if isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and needle in node.value
        ]
        assert matches, f"no string literal containing {needle!r} found"
        return min(matches)

    def _first_call_lineno(func_name: str) -> int:
        matches = [
            node.lineno for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and (
                (isinstance(node.func, ast.Name) and node.func.id == func_name)
                or (isinstance(node.func, ast.Attribute)
                    and node.func.attr == func_name)
            )
        ]
        assert matches, f"no call to {func_name}() found"
        return min(matches)

    context_gate_line = _first_string_literal_lineno(
        "Sniper graph policy completed without a loaded graph context")
    variant_receipt_line = _first_call_lineno("apply_sniper_variant_receipt")
    assert context_gate_line < variant_receipt_line, (
        "the Sniper ReusePlan variant-receipt call must be parsed AFTER (i.e. at a "
        "later source line than) the fail-closed missing-graph-context gate")


def test_frontier_kernel_popt_scope_caveat_is_preserved_as_a_design_limit():
    """The P-OPT rereference-matrix scope caveat on frontier kernels (BFS/
    SSSP) must remain documented as an inherent, as-designed limitation of
    P-OPT's monotonic sweep-order epoch assumption -- not something a later
    edit should silently reframe as a "fixed implementation
    bug". This guards the wording so a future edit cannot quietly claim the
    limitation was resolved without an explicit, reviewed decision.
    """
    assert FRONTIER_CAVEAT_DOC.exists(), (
        "the frontier-kernel P-OPT scope caveat evidence doc is missing: "
        f"{FRONTIER_CAVEAT_DOC}")
    text = " ".join(read(FRONTIER_CAVEAT_DOC).split())
    assert "BFS and SSSP comparisons are project extensions" in text
    assert "monotonic sweep-order epoch assumption" in text
