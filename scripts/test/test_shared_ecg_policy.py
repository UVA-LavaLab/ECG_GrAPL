"""Verify that every simulator uses the same ECG eviction policy.

The ECG_GRASP_POPT victim-selection logic lives in one header,
``bench/include/ecg_victim_policy.h``, which cache_sim, gem5 and Sniper all call.
To keep "nothing is ported/mirrored" true, every simulator's co-located copy of
that header must be byte-identical to the canonical one. If they ever drift, the
decision logic could differ between backends — this test fails loudly.
"""
from __future__ import annotations

import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CANONICAL = ROOT / "bench" / "include" / "ecg_victim_policy.h"
# Tracked co-located copies (gem5 uses the .hh convention; content is identical).
COPIES = [
    ROOT / "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_victim_policy.hh",
    ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/ecg_victim_policy.h",
]
MODE_CANONICAL = ROOT / "bench/include/ecg_mode.h"
MODE_COPIES = [
    ROOT / "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_mode.hh",
    ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/ecg_mode.h",
]


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def test_canonical_exists():
    assert CANONICAL.is_file(), f"canonical ECG policy header missing: {CANONICAL}"


def test_all_copies_byte_identical():
    want = _sha(CANONICAL)
    for c in COPIES:
        assert c.is_file(), f"overlay ECG policy copy missing: {c}"
        assert _sha(c) == want, (
            f"ECG policy header drift: {c} differs from canonical {CANONICAL}.\n"
            f"All simulators must share the identical eviction decision; re-copy "
            f"bench/include/ecg_victim_policy.h into the overlay trees."
        )


def test_ecg_mode_is_one_byte_identical_definition():
    want = _sha(MODE_CANONICAL)
    for copy in MODE_COPIES:
        assert _sha(copy) == want, (
            f"ECG mode drift: {copy} differs from {MODE_CANONICAL}")
    text = MODE_CANONICAL.read_text()
    expected = {
        "DBG_PRIMARY": 0,
        "POPT_PRIMARY": 1,
        "POPT_TIE": 2,
        "DBG_ONLY": 3,
        "ECG_EMBEDDED": 4,
        "ECG_EPOCH_EMBEDDED": 5,
        "ECG_COMBINED": 6,
        "ECG_EXACT": 7,
        "ECG_EXACT_STORED": 8,
        "ECG_EXACT_MASK": 9,
        "ECG_GRASP_POPT": 10,
        "ECG_REF32": 11,
    }
    for name, value in expected.items():
        assert re.search(rf"\b{name}\s*=\s*{value}\b", text)


def test_calls_present_in_each_simulator():
    """Each simulator's policy source must actually call the shared function."""
    callers = {
        "bench/include/cache_sim/cache_sim.h": 'ecg_policy::selectVictim',
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_rp.cc": 'ecg_policy::selectVictim',
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/cache_set_ecg.cc": 'ecg_policy::selectVictim',
    }
    for rel, token in callers.items():
        text = (ROOT / rel).read_text(errors="ignore")
        assert token in text, f"{rel} does not call the shared {token}"
        assert "ecg_policy::parseVariant" in text, (
            f"{rel} does not use the shared fail-closed variant parser")


def test_epoch_stamp_defaults_are_undelivered_across_backends():
    cache_context = (
        ROOT / "bench/include/cache_sim/graph_cache_context.h").read_text()
    gem5_header = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.hh").read_text()
    sniper_source = (
        ROOT / "bench/include/sniper_sim/overlays/common/core/"
        "memory_subsystem/cache/cache_set_ecg.cc").read_text()
    assert "bool     edge_epoch_valid = false;" in cache_context
    assert "ecg_epoch_valid(false)" in gem5_header
    assert "m_ecg_epoch_valid[way] = false;" in sniper_source


def test_reuse_admission_mapping_is_shared_across_backends():
    callers = (
        "bench/include/cache_sim/cache_sim.h",
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_rp.cc",
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/cache_set_ecg.cc",
    )
    for rel in callers:
        text = (ROOT / rel).read_text(errors="ignore")
        assert text.count("ecg_policy::reuseAdmissionRRPV") >= 2, (
            f"{rel} must apply shared future-distance admission on fill and hit")
        assert "ecg_policy::combinedReuseAdmissionRRPV" in text
        assert "ECG_REUSE_ADMISSION" in text
    builder = (ROOT / "bench/include/ecg_reuse_plan_builder.h").read_text()
    assert builder.count("quantizedFutureEpoch(") >= 3


def test_combined_insertion_mapping_is_shared_across_backends():
    callers = (
        "bench/include/cache_sim/cache_sim.h",
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_rp.cc",
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/cache_set_ecg.cc",
    )
    for rel in callers:
        assert "ecg_policy::combinedInsertionRRPV" in (
            ROOT / rel).read_text(errors="ignore")


def test_grasp_insertion_classifier_is_shared():
    """The GRASP insertion tier is shared with the eviction policy:
    each simulator's graph context must call ecg_policy::classifyGraspTier rather
    than duplicate the per-region boundary math (which previously drifted — e.g.
    cache_sim classified [upper,upper+8) as MODERATE while gem5/Sniper did not)."""
    callers = {
        "bench/include/cache_sim/graph_cache_context.h": 'ecg_policy::classifyGraspTier',
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_cache_context_gem5.hh": 'ecg_policy::classifyGraspTier',
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.cc": 'ecg_policy::classifyGraspTier',
    }
    for rel, token in callers.items():
        text = (ROOT / rel).read_text(errors="ignore")
        assert token in text, f"{rel} does not call the shared {token} (GRASP tier drift risk)"


def test_prefetch_target_is_shared():
    """ECG prefetch-target selection is a single shared header
    (bench/include/ecg_mode6_builder.h, compiled into every kernel). The cache_sim
    mask builder must call it rather than duplicate the lookahead logic, so the
    one prefetch-target unit test covers all three simulators."""
    builder = ROOT / "bench/include/ecg_mode6_builder.h"
    assert builder.is_file(), f"shared mask builder missing: {builder}"
    assert "selectPrefetchTarget" in builder.read_text(errors="ignore")
    cc_ctx = ROOT / "bench/include/cache_sim/graph_cache_context.h"
    assert "ecg_mode6::selectPrefetchTarget" in cc_ctx.read_text(errors="ignore"), (
        "cache_sim graph_cache_context.h must call the shared "
        "ecg_mode6::selectPrefetchTarget, not duplicate the lookahead logic"
    )


# The overlays are the tracked home; bench/include/gem5_sim/gem5 and the Sniper
# checkout are GENERATED and gitignored. Nothing previously noticed when a change
# was made directly in the generated tree, which is easy to do because that is
# where the build reads from and where a compiler error points you. Such a change
# builds, runs, and measures correctly on this machine and does not exist at all
# on any other -- the worst possible failure, because every local check passes.
GEM5_APPLIED = ROOT / "bench/include/gem5_sim/gem5/src"
GEM5_OVERLAY = ROOT / "bench/include/gem5_sim/overlays"


def test_applied_gem5_tree_matches_the_tracked_sources():
    """Files the build copies in must be byte-identical where it reads them.

    The pair list is derived from setup_gem5.OVERLAY_FILE_MAP rather than from a
    directory walk, because the map is the authority on what gets copied and it
    includes sources from OUTSIDE the overlays directory. A walk missed
    ../../hawkeye_policy.h, leaving a BASELINE replacement policy -- one that
    every comparison is measured against -- with no drift guard at all.
    """
    if not GEM5_APPLIED.is_dir():
        import pytest
        pytest.skip("gem5 checkout not present")
    import importlib.util
    import sys
    spec = importlib.util.spec_from_file_location(
        "setup_gem5_for_test", ROOT / "scripts/setup_gem5.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["setup_gem5_for_test"] = mod
    spec.loader.exec_module(mod)

    checked, drifted, missing = 0, [], []
    for src_rel, dst_rel in mod.OVERLAY_FILE_MAP.items():
        src = (GEM5_OVERLAY / src_rel).resolve()
        dst = (GEM5_APPLIED / dst_rel).resolve()
        if not src.is_file():
            missing.append(f"source {src_rel}")
            continue
        if not dst.is_file():
            missing.append(f"installed {dst_rel}")
            continue
        checked += 1
        if _sha(src) != _sha(dst):
            drifted.append(dst_rel)
    assert checked > 0, "no copied pairs compared; the check is vacuous"
    assert not missing, (
        f"copy map entries with no file on one side: {missing}")
    assert not drifted, (
        f"{drifted} differ from their tracked sources. The gem5 checkout is "
        "generated and gitignored, so these edits exist only on this machine; "
        "move them into the tracked source and re-apply.")


def test_every_ecg_instruction_in_the_built_decoder_is_tracked():
    """An opcode added straight into the generated decoder would be lost.

    This is not hypothetical: ecg_extract2c was added to the gem5 checkout,
    built, and measured, while the tracked overlay knew nothing about it. A
    fresh clone would have produced a guest that emits the instruction and a
    simulator that cannot decode it.
    """
    applied = GEM5_APPLIED / "arch/riscv/isa/decoder.isa"
    overlay = (GEM5_OVERLAY / "arch/riscv/isa/decoder_ecg_extract.isa")
    if not applied.exists():
        import pytest
        pytest.skip("gem5 checkout not present")
    import re
    names = lambda t: set(re.findall(r"\b(ecg_[a-z0-9_]+)\(\{\{", t))
    built, tracked = names(applied.read_text()), names(overlay.read_text())
    untracked = sorted(built - tracked)
    assert not untracked, (
        f"{untracked} exist only in the generated gem5 decoder, so they are "
        "not in version control and will vanish on a fresh checkout; add them "
        f"to {overlay.relative_to(ROOT)}")
    # The other direction matters too: a tracked instruction absent from the
    # build means the overlay was edited without reinstalling, so the simulator
    # being measured is not the one in version control.
    uninstalled = sorted(tracked - built)
    assert not uninstalled, (
        f"{uninstalled} are tracked in the overlay but absent from the built "
        "decoder; re-apply the overlays, or the measured simulator is not the "
        "one under review")


def test_the_executing_victim_variant_is_attested_at_runtime():
    """The runner recorded the variant it ASKED for, not the one that ran.

    Every decomposition in this work turns on epoch_first versus lru_only being
    the only difference between two arms. gem5 emitted the executing variant
    only through a gated trace that is off in measurement runs, so no archived
    artifact proved which rule executed and the claim rested on the request.

    An ungated one-line receipt, printed once, closes that. Verified end to end:
    ECG_VARIANT=lru_only produces
    "[ECG-VARIANT-RECEIPT sim=gem5 requested=lru_only effective=6 dueling=0]".
    """
    overlay = (ROOT / "bench/include/gem5_sim/overlays/mem/cache"
               / "replacement_policies/ecg_rp.cc").read_text()
    assert "ECG-VARIANT-RECEIPT" in overlay, (
        "nothing attests the executing victim rule, so a replacement-rule "
        "comparison cites its own configuration as evidence")
    i = overlay.index("ECG-VARIANT-RECEIPT")
    window = overlay[i - 800:i + 500]
    assert "requested=" in window and "effective=" in window, (
        "the receipt must show BOTH what was asked for and what was resolved, "
        "or it cannot catch a silent fallback")
    assert "dueling=" in window, (
        "set-dueling overrides the configured variant per set; a receipt that "
        "hides it would attest a rule that is not uniformly in force")


def test_online_dueling_has_roi_activity_statistics():
    policy = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.cc").read_text()
    header = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.hh").read_text()
    selector = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_victim_policy.hh").read_text()
    runner = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()

    assert "sampledMisses() const" in selector
    assert "completedWindows() const" in selector
    assert "OnlineDuelingStats" in header
    for field in (
            "requestBoundVictims", "leaderSamples", "followerSelections",
            "completedWindows", "winnerChanges",
            "followerVariantOverrides"):
        assert f"ADD_STAT(\n        {field}," in policy
    assert "++onlineDuelingStats.requestBoundVictims" in policy
    assert "++onlineDuelingStats.leaderSamples" in policy
    assert "++onlineDuelingStats.followerSelections" in policy
    assert "gem5_reuse_plan_dueling_completed_windows" in runner
    assert "ONLINE_DUELING_WINDOW_MISSES" in runner


SNIPER_CACHE_SET_ECG = (
    ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem"
    "/cache/cache_set_ecg.cc")


def test_sniper_variant_is_attested_at_runtime():
    """Sniper analog of test_the_executing_victim_variant_is_attested_at_runtime.

    Verified end to end: ECG_VARIANT=lru_only produces
    "[ECG-VARIANT-RECEIPT sim=sniper requested=lru_only effective=6
    dueling=0]" -- an ungated, print-once receipt gated only on
    context.loaded, mirroring gem5's own gating.
    """
    overlay = SNIPER_CACHE_SET_ECG.read_text()
    assert "ECG-VARIANT-RECEIPT" in overlay, (
        "nothing attests the Sniper-executing victim rule, so a "
        "replacement-rule comparison cites its own configuration as evidence")
    i = overlay.index("ECG-VARIANT-RECEIPT")
    window = overlay[i - 800:i + 600]
    assert "sim=sniper" in window
    assert "requested=" in window and "effective=" in window, (
        "the receipt must show BOTH what was asked for and what was "
        "resolved, or it cannot catch a silent fallback")
    assert "dueling=" in window, (
        "set-dueling overrides the configured variant per set; a receipt "
        "that hides it would attest a rule that is not uniformly in force")


def test_sniper_online_dueling_has_roi_activity_statistics():
    """Sniper analog of test_online_dueling_has_roi_activity_statistics.

    Sniper has no gem5-style statistics::Group, so the counters are exposed
    via registerStatsMetric under the "ecg-online-dueling" namespace, the
    SAME mechanism nuca-cache's flowthrough-reads/writes already relies
    on. Sniper's --roi wrapper does NOT reset registered stats at ROI start
    (StatsManager::recordStats() only snapshots current values under a named
    prefix); each counter is monotonic, registered no later than its first
    increment, and reported by the caller (sniper_lib.py/parse_stats) as a
    roi-begin -> roi-end snapshot delta.
    """
    overlay = SNIPER_CACHE_SET_ECG.read_text()
    runner = (ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()

    # Multicore-safe evidence: recordMiss() now returns a MissRecordEvent
    # describing only what THAT call did (no racy before/after diffing of
    # the selector's own sampledMisses()/completedWindows() across shared
    # core threads), and every evidence increment goes through the atomic
    # incrementEvidenceCounter() wrapper (__sync_fetch_and_add) rather than
    # a plain "++" that would race across Sniper's per-core OS threads. See
    # the MULTICORE SAFETY comment block in cache_set_ecg.cc.
    assert "ecg_policy::MissRecordEvent" in overlay
    assert "incrementEvidenceCounter" in overlay
    assert "__sync_fetch_and_add" in overlay
    assert "struct OnlineDuelingEvidence" in overlay
    assert "ensureOnlineDuelingStatsRegistered" in overlay
    for field in (
            "governed_victims", "leader_samples", "follower_selections",
            "completed_windows", "winner_changes",
            "follower_variant_overrides"):
        assert f"&evidence.{field}" in overlay, (
            f"OnlineDuelingEvidence.{field} is never registered via "
            "registerStatsMetric")
        increment_pattern = re.compile(
            r"incrementEvidenceCounter\(\s*"
            rf"(evidence|onlineDuelingEvidence\(\))\.{re.escape(field)}\s*\)")
        assert increment_pattern.search(overlay), (
            f"OnlineDuelingEvidence.{field} is never atomically incremented "
            "via incrementEvidenceCounter()")
        # Guard against a regression back to a racy plain "++" increment.
        assert f"++evidence.{field}" not in overlay, (
            f"OnlineDuelingEvidence.{field} is incremented with a plain "
            "'++' -- this races across Sniper's per-core OS threads; use "
            "incrementEvidenceCounter() instead")
        assert f"++onlineDuelingEvidence().{field}" not in overlay, (
            f"OnlineDuelingEvidence.{field} is incremented with a plain "
            "'++' -- this races across Sniper's per-core OS threads; use "
            "incrementEvidenceCounter() instead")
    assert "sniper_reuse_plan_dueling_completed_windows" in runner
    assert "SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS" in runner


def test_sniper_online_dueling_naming_avoids_gem5_request_binding_claim():
    """Sniper must never claim gem5's O3 Request/MSHR-attested victim binding.

    gem5's first online-dueling counter is named "requestBoundVictims"
    because it is gated on a genuine per-packet Request/MSHR binding
    (GraphEcgRP::setVictimRequest) that only exists on gem5's O3 CPU.
    Sniper's marker/sideband-governed population must use a different name
    (governed_victims) so a reader of the evidence cannot mistake Sniper's
    population for that gem5-specific HW binding.
    """
    overlay = SNIPER_CACHE_SET_ECG.read_text()
    assert "struct OnlineDuelingEvidence" in overlay
    assert "UInt64 governed_victims" in overlay
    # The evidence struct itself must not (re-)introduce gem5's field name.
    struct_start = overlay.index("struct OnlineDuelingEvidence")
    struct_end = overlay.index("};", struct_start)
    struct_body = overlay[struct_start:struct_end]
    assert "request_bound_victims" not in struct_body
    assert "requestBoundVictims" not in struct_body
