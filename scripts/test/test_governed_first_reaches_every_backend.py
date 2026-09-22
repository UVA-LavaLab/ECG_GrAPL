"""The governed-first arm must reach all three backends, not just cache_sim.

Governed-first was measured in cache_sim and confirmed on both dense kernels,
which is the trigger for promoting it to the native paths. The promotion is
exactly the failure shape this repository has already paid for twice: one option
needs many plumbing sites, each site is taught separately, and a site that is
missed leaves the arm silently off while every receipt honestly reports the
default. In cache_sim that cost several simulation cells before a test pinned
the sites to each other.

Native cells are far more expensive than cache_sim cells, so the sites are
pinned here before any native run rather than after one is wasted. Every
assertion below fails if its site is reverted.

Two classes of defect are covered. First, the plumbing: a declaration, a
constructor, a forwarder, a command line and an environment variable, in two
backends. Second, and less obvious, **installed-output staleness**: the Sniper
checkout carried a copy of the shared record header predating `VictimOptions`
entirely, so an overlay that passed options would not have compiled. The
overlays under `bench/include/{gem5,sniper}_sim/` are source and the checkouts
are installed output, so the two must agree.
"""
from pathlib import Path
import hashlib
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

SHARED_HEADER = ROOT / "bench/include/ecg_record_runtime.h"
CACHE_SIM = ROOT / "bench/include/cache_sim/cache_sim.h"
GEM5_RP_CC = ROOT / "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_ecg_record_rp.cc"
GEM5_RP_HH = ROOT / "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_ecg_record_rp.hh"
GEM5_PARAMS = ROOT / "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/GraphReplacementPolicies.py"
GEM5_CACHE_CONFIG = ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_cache_config.py"
GEM5_SE = ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py"
SNIPER_SET = ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/cache_set_ecg.cc"
SNIPER_REC_CC = ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/ecg_record_sniper.cc"
SNIPER_REC_H = ROOT / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/ecg_record_sniper.h"
RUNNER = ROOT / "scripts/experiments/ecg/roi_matrix.py"


def test_the_option_is_off_by_default_in_the_shared_rule():
    """A default-on arm would silently change every result ever recorded."""
    text = SHARED_HEADER.read_text()
    assert "struct VictimOptions" in text
    assert "bool governed_first = false;" in text, (
        "governed_first must default to false in the shared rule, or every "
        "backend changes behaviour the moment it is wired")


@pytest.mark.parametrize("path,label", [
    (CACHE_SIM, "cache_sim"),
    (GEM5_RP_CC, "gem5"),
    (SNIPER_SET, "sniper"),
])
def test_every_production_call_site_passes_options(path, label):
    """All three backends must hand selectVictim an options argument.

    `selectVictim` takes options as a trailing defaulted parameter, so a call
    site that omits it compiles cleanly and silently runs the default arm. That
    is precisely why this cannot be left to inspection.
    """
    text = path.read_text()
    assert "ecg_record::selectVictim(" in text, f"{label} no longer calls the shared rule"
    assert "VictimOptions" in text or "record_victim_options_" in text, (
        f"{label} calls ecg_record::selectVictim without an options argument, so "
        "it silently runs the default arm regardless of configuration")


@pytest.mark.parametrize("path,label", [
    (CACHE_SIM, "cache_sim"),
    (GEM5_RP_CC, "gem5"),
    (SNIPER_REC_CC, "sniper"),
])
def test_every_backend_compares_against_the_same_clock(path, label):
    """One accessor must feed the sequence handed to the rule.

    `comparisonWatermark()` collapses the progress and delivered clocks into a
    single choice. If one backend reads `watermark()` directly the three agree
    only while the default clock is selected, and diverge silently otherwise.
    """
    assert "comparisonWatermark()" in path.read_text(), (
        f"{label} does not take its comparison sequence from "
        "comparisonWatermark(), so the backends can disagree about expiry")


def test_gem5_plumbs_the_parameter_end_to_end():
    params = GEM5_PARAMS.read_text()
    assert "governed_first = Param.Bool(" in params
    assert "Param.Bool(\n        False," in params or "Param.Bool(False" in params, (
        "the gem5 parameter must default to False")
    assert "const bool governedFirst;" in GEM5_RP_HH.read_text(), (
        "the gem5 policy has no member to hold the parameter")
    rp = GEM5_RP_CC.read_text()
    assert "governedFirst(params.governed_first)" in rp, (
        "the gem5 constructor ignores its own parameter")
    assert "options.governed_first = governedFirst;" in rp, (
        "the gem5 policy reads the parameter but never applies it")
    assert "governed_first=" in rp, (
        "the gem5 announce line must state the arm, so a receipt records which ran")
    assert 'governed_first=kwargs.get("governed_first", False)' in GEM5_CACHE_CONFIG.read_text(), (
        "graph_cache_config does not forward the kwarg to the SimObject")
    se = GEM5_SE.read_text()
    assert '"--ecg-record-governed-first"' in se, "graph_se.py has no flag for the arm"
    assert 'l3_policy_kwargs["governed_first"]' in se, (
        "graph_se.py parses the flag but never forwards it, the exact defect "
        "that cost cells in cache_sim")


def test_sniper_plumbs_the_environment_variable_end_to_end():
    cc = SNIPER_REC_CC.read_text()
    assert 'envUnsigned("SNIPER_ECG_RECORD_GOVERNED_FIRST", 0, 0, 1)' in cc, (
        "Sniper must read the arm fail-closed, rejecting anything but 0 or 1")
    assert "recordGovernedFirst()" in cc
    assert "bool recordGovernedFirst();" in SNIPER_REC_H.read_text(), (
        "the accessor is defined but not declared, so the call site cannot see it")
    assert "recordGovernedFirst()" in SNIPER_SET.read_text(), (
        "the Sniper victim site never consults the arm")


def test_the_runner_names_the_arm_for_both_native_backends():
    """Sniper reads SNIPER_-prefixed variables, so one name is not enough."""
    text = RUNNER.read_text()
    assert '"--ecg-record-governed-first", "on"' in text, (
        "the runner never passes the arm to gem5")
    assert '"SNIPER_ECG_RECORD_GOVERNED_FIRST"' in text, (
        "the runner sets ECG_RECORD_GOVERNED_FIRST but not the SNIPER_ variant, "
        "so Sniper silently runs the default arm")
    assert '"ECG_RECORD_GOVERNED_FIRST"' in text


def test_the_gem5_flag_is_absent_at_its_default():
    """An unconditional flag changes every existing command's configuration hash."""
    text = RUNNER.read_text()
    marker = '"--ecg-record-governed-first", "on"'
    index = text.index(marker)
    preceding = text[max(0, index - 400):index]
    assert 'record_governed_first", "no") == "on"' in preceding, (
        "the gem5 flag must be emitted only when the arm is on, or every "
        "previously recorded command and hash changes")


@pytest.mark.parametrize("overlay,installed", [
    (
        ROOT / "bench/include/ecg_record_runtime.h",
        ROOT / "bench/include/sniper_sim/snipersim/common/core/memory_subsystem/cache/ecg_record_runtime.h",
    ),
    (
        ROOT / "bench/include/ecg_record_runtime.h",
        ROOT / "bench/include/gem5_sim/gem5/src/mem/cache/replacement_policies/ecg_record_runtime.h",
    ),
    (SNIPER_SET, ROOT / "bench/include/sniper_sim/snipersim/common/core/memory_subsystem/cache/cache_set_ecg.cc"),
    (SNIPER_REC_CC, ROOT / "bench/include/sniper_sim/snipersim/common/core/memory_subsystem/cache/ecg_record_sniper.cc"),
    (GEM5_RP_CC, ROOT / "bench/include/gem5_sim/gem5/src/mem/cache/replacement_policies/graph_ecg_record_rp.cc"),
])
def test_installed_checkouts_match_their_source(overlay, installed):
    """Installed output must not drift from the overlay that owns it.

    The Sniper checkout was found holding a copy of the shared record header
    from before `VictimOptions` existed. Nothing reported that: the checkout is
    gitignored build output, so neither `git status` nor the source tests could
    see it, and it would have surfaced as a compile error during an expensive
    native build or, worse, as a silently old rule.

    Reinstall rather than edit a checkout: `scripts/setup_gem5.py --isa RISCV
    --skip-build` and `scripts/setup_sniper.py --skip-build --apply-overlays`.
    """
    if not installed.exists():
        pytest.skip(f"{installed.name} is not installed in this environment")
    source_digest = hashlib.sha256(overlay.read_bytes()).hexdigest()
    installed_digest = hashlib.sha256(installed.read_bytes()).hexdigest()
    assert source_digest == installed_digest, (
        f"{installed.relative_to(ROOT)} is stale against "
        f"{overlay.relative_to(ROOT)}; reinstall the checkout rather than "
        "patching it in place")
