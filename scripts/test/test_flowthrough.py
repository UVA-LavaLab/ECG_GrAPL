#!/usr/bin/env python3
"""FlowThrough must be available to every policy, not just ReusePlan.

FlowThrough lets ReusePlan read its one-touch per-edge records without allocating them
in the LLC. The same argument applies to any policy's CSR edge stream: it is
sequential and read-once, so allocating it evicts reusable property lines.
Offering the option only to ReusePlan confounds "ReusePlan replaces better"
with "ReusePlan is the only policy allowed to use FlowThrough."

Measured on web-Google PageRank at 2 MiB 16-way, FlowThrough changes traffic
by -20.0% for LRU, -5.1% for GRASP, -2.0% for P-OPT, and -2.4% for ReusePlan.
It therefore mattered which policies were allowed to use it.
"""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PR = ROOT / "bench/bin_sim/pr"

pytestmark = pytest.mark.skipif(
    not PR.exists(), reason="cache_sim pr binary not built")


def run(policy: str, flowthrough: bool) -> dict:
    env = dict(os.environ)
    env.update({
        "OMP_NUM_THREADS": "1",
        "CACHE_ULTRAFAST": "0",
        "CACHE_POLICY": policy,
        "CACHE_L1_SIZE": "1024",
        "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192",
        "CACHE_L3_WAYS": "16",
        "FLOWTHROUGH": "1" if flowthrough else "0",
    })
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        return json.loads(out.read_text())


@pytest.mark.parametrize("policy", ["LRU", "GRASP", "POPT"])
def test_flowthrough_is_available_to_baseline_policies(policy):
    """A baseline policy must be able to decline to allocate the edge stream.

    Only availability is asserted, not benefit. FlowThrough is a sensitivity, not
    a free win: on the large web-Google cell it helps every policy (LRU -20.0%,
    GRASP -5.1%, P-OPT -2.0%), but on this small synthetic cell it *increases*
    P-OPT traffic, because declining to allocate a stream that still has reuse
    costs more than it saves. That is precisely why the option has to be offered
    to every policy and reported, rather than assumed beneficial and granted to
    ReusePlan alone. If FlowThrough were still ReusePlan-only, these would be
    identical.
    """
    off = run(policy, flowthrough=False)["total_memory_traffic"]
    on = run(policy, flowthrough=True)["total_memory_traffic"]
    assert on != off, (
        f"{policy} traffic unchanged by FLOWTHROUGH ({off}); FlowThrough "
        "is not reaching baseline policies")


def test_flowthrough_is_off_by_default():
    """The default must not silently change every baseline number."""
    env = dict(os.environ)
    env.pop("FLOWTHROUGH", None)
    env.update({
        "OMP_NUM_THREADS": "1", "CACHE_ULTRAFAST": "0", "CACHE_POLICY": "LRU",
        "CACHE_L1_SIZE": "1024", "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192", "CACHE_L3_WAYS": "16",
    })
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        default = json.loads(out.read_text())["total_memory_traffic"]
    assert default == run("LRU", flowthrough=False)["total_memory_traffic"]


# ---------------------------------------------------------------------------
# ReusePlan and ReusePlan+FlowThrough must stay distinct rows
# ---------------------------------------------------------------------------
# FlowThrough is an independent placement mechanism, so "ReusePlan" and
# "ReusePlan+FlowThrough" are two policies, not two settings of one policy.
# Collapsing them lets the better variant's number be quoted under the plainer
# name.

def _policy_specs():
    import importlib.util
    import sys
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_flowthrough", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["roi_matrix_flowthrough"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_reuse_plan_variants_are_distinct_policies():
    mod = _policy_specs()
    names = ["ECG:REUSE_PLAN", "ECG:REUSE_PLAN_FLOWTHROUGH",
             "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
             "ECG:REUSE_PLAN_ONLINE", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH"]
    specs = {n: mod.parse_policy_spec(n) for n in names}
    labels = {n: s.label for n, s in specs.items()}
    assert len(set(labels.values())) == len(names), (
        f"ReusePlan variants collapsed to shared labels: {labels}")
    # FlowThrough variants must carry the placement flag; plain ReusePlan must not.
    assert specs["ECG:REUSE_PLAN"].ecg_flowthrough is False
    assert specs["ECG:REUSE_PLAN_FLOWTHROUGH"].ecg_flowthrough is True
    assert specs["ECG:REUSE_PLAN_RRIP_FLOWTHROUGH"].ecg_flowthrough is True
    assert specs["ECG:REUSE_PLAN_RRIP_FLOWTHROUGH"].ecg_variant == "rrip_first"
    assert specs["ECG:REUSE_PLAN_ONLINE"].ecg_flowthrough is False
    assert specs["ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH"].ecg_flowthrough is True
    # Online selection is the other independent axis.
    assert specs["ECG:REUSE_PLAN"].ecg_set_dueling is False
    assert specs["ECG:REUSE_PLAN_RRIP_FLOWTHROUGH"].ecg_set_dueling is False
    assert specs["ECG:REUSE_PLAN_ONLINE"].ecg_set_dueling is True


def test_cache_sim_online_dueling_reports_arm_accounting():
    env = dict(os.environ)
    env.update({
        "OMP_NUM_THREADS": "1",
        "CACHE_ULTRAFAST": "0",
        "CACHE_POLICY": "ECG",
        "CACHE_L1_SIZE": "1024",
        "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192",
        "CACHE_L3_WAYS": "16",
        "ECG_MODE": "ECG_GRASP_POPT",
        "ECG_EDGE_MASKS": "1",
        "ECG_REUSE_PLAN_DEPTH": "2",
        "ECG_EDGE_MASK_EPOCHS": "16",
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
        "ECG_SET_DUELING": "1",
        "CACHE_ECG_DUELING_SET_OFFSET": "7",
    })
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        stats = json.loads(out.read_text())

    arms = ("rrip", "grasp", "epoch", "degree", "lru")
    assert stats["ecg_dueling_set_offset"] == 7
    assert 0 <= stats["ecg_dueling_final_winner_arm"] < len(arms)
    assert sum(
        stats[f"ecg_dueling_leader_samples_{arm}"] for arm in arms) > 0
    assert sum(
        stats[f"ecg_dueling_follower_selections_{arm}"] for arm in arms) > 0
    assert stats["ecg_dueling_completed_windows"] == sum(
        stats[f"ecg_dueling_winner_windows_{arm}"] for arm in arms)
