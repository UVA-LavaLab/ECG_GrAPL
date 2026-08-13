#!/usr/bin/env python3
"""The structural-stream bypass must be available to every policy, not just K2.

StreamShield lets K2 read its one-touch per-edge records without allocating them
in the LLC. The same argument applies to any policy's CSR edge stream: it is
sequential and read-once, so allocating it evicts reusable property lines.
Offering the option only to K2 confounds "K2 replaces better" with "K2 is the
only policy allowed to bypass".

Measured on web-Google PageRank at 2 MiB 16-way, the bypass is worth -20.0% to
LRU, -5.1% to GRASP and -2.0% to P-OPT, against StreamShield's -2.4% to K2. It
therefore mattered a great deal which policies were allowed to use it.
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


def run(policy: str, bypass: bool) -> dict:
    env = dict(os.environ)
    env.update({
        "OMP_NUM_THREADS": "1",
        "CACHE_ULTRAFAST": "0",
        "CACHE_POLICY": policy,
        "CACHE_L1_SIZE": "1024",
        "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192",
        "CACHE_L3_WAYS": "16",
        "STRUCTURAL_BYPASS": "1" if bypass else "0",
    })
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        return json.loads(out.read_text())


@pytest.mark.parametrize("policy", ["LRU", "GRASP", "POPT"])
def test_bypass_is_available_to_baseline_policies(policy):
    """A baseline policy must be able to decline to allocate the edge stream.

    Only availability is asserted, not benefit. The bypass is a sensitivity, not
    a free win: on the large web-Google cell it helps every policy (LRU -20.0%,
    GRASP -5.1%, P-OPT -2.0%), but on this small synthetic cell it *increases*
    P-OPT traffic, because declining to allocate a stream that still has reuse
    costs more than it saves. That is precisely why the option has to be offered
    to every policy and reported, rather than assumed beneficial and granted to
    K2 alone. If the bypass were still K2-only these would be identical.
    """
    off = run(policy, bypass=False)["total_memory_traffic"]
    on = run(policy, bypass=True)["total_memory_traffic"]
    assert on != off, (
        f"{policy} traffic unchanged by STRUCTURAL_BYPASS ({off}); the bypass "
        "is not reaching baseline policies")


def test_bypass_is_off_by_default():
    """The default must not silently change every baseline number."""
    env = dict(os.environ)
    env.pop("STRUCTURAL_BYPASS", None)
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
    assert default == run("LRU", bypass=False)["total_memory_traffic"]


# ---------------------------------------------------------------------------
# K2 and K2+StreamShield must stay distinct rows
# ---------------------------------------------------------------------------
# StreamShield is K2's structural bypass, so "K2" and "K2+StreamShield" are two
# policies, not two settings of one policy. Collapsing them lets the better
# variant's number be quoted under the plainer name.

def _policy_specs():
    import importlib.util
    import sys
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_bypass", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["roi_matrix_bypass"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_k2_variants_are_distinct_policies():
    mod = _policy_specs()
    names = ["ECG:K2", "ECG:K2_STREAMSHIELD",
             "ECG:K2_RRIP_STREAMSHIELD",
             "ECG:K2_ONLINE", "ECG:K2_ONLINE_STREAMSHIELD"]
    specs = {n: mod.parse_policy_spec(n) for n in names}
    labels = {n: s.label for n, s in specs.items()}
    assert len(set(labels.values())) == len(names), (
        f"K2 variants collapsed to shared labels: {labels}")
    # StreamShield variants must actually carry the bypass, and plain K2 must not.
    assert specs["ECG:K2"].ecg_stream_bypass is False
    assert specs["ECG:K2_STREAMSHIELD"].ecg_stream_bypass is True
    assert specs["ECG:K2_RRIP_STREAMSHIELD"].ecg_stream_bypass is True
    assert specs["ECG:K2_RRIP_STREAMSHIELD"].ecg_variant == "rrip_first"
    assert specs["ECG:K2_ONLINE"].ecg_stream_bypass is False
    assert specs["ECG:K2_ONLINE_STREAMSHIELD"].ecg_stream_bypass is True
    # Online selection is the other independent axis.
    assert specs["ECG:K2"].ecg_set_dueling is False
    assert specs["ECG:K2_RRIP_STREAMSHIELD"].ecg_set_dueling is False
    assert specs["ECG:K2_ONLINE"].ecg_set_dueling is True
