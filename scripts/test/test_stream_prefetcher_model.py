#!/usr/bin/env python3
"""The structure-stream prefetcher must be able to mispredict.

The prefetcher behind the withdrawn STRIDE8 lead decided what to prefetch by
asking the graph context whether an address was property data. It therefore
never mispredicted the distinction the experiment turned on, and it issued with
no MSHR, queue, lateness or bandwidth backpressure. A mechanism built so that
it cannot be wrong cannot confirm a hypothesis.

The default is now an address-only stream detector: confirmation-gated, capable
of training on the wrong stream, and bounded by a finite in-flight budget. The
oracle survives only as an explicitly labelled upper bound.
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


def run(**overrides) -> dict:
    env = dict(os.environ)
    env.update({
        "OMP_NUM_THREADS": "1",
        "CACHE_ULTRAFAST": "0",
        "CACHE_POLICY": "LRU",
        "CACHE_L1_SIZE": "1024",
        "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192",
        "CACHE_L3_WAYS": "16",
        "CACHE_STREAM_PREFETCH_DEGREE": "8",
    })
    env.update({k: str(v) for k, v in overrides.items()})
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        return json.loads(out.read_text())


def test_default_model_is_the_honest_one():
    stats = run()
    assert stats["stream_prefetch_model"] == "stride", (
        "the oracle prefetcher must not be the default; results depending on "
        "it are ineligible for performance claims")


def test_oracle_is_still_selectable_as_an_upper_bound():
    stats = run(CACHE_STREAM_PREFETCH_MODEL="oracle")
    assert stats["stream_prefetch_model"] == "oracle"


def test_stride_model_requires_confirmation_before_issuing():
    """A detector that issues on first sight is not a detector."""
    stats = run()
    assert stats["stream_prefetch_untrained"] > 0, (
        "no access was ever declined for lack of a confirmed stream, so the "
        "confirmation gate is not doing anything")


def test_stride_model_is_bounded_in_flight():
    """A tight budget must actually throttle; unbounded issue is the defect."""
    tight = run(CACHE_STREAM_PREFETCH_MAX_INFLIGHT="1")
    loose = run(CACHE_STREAM_PREFETCH_MAX_INFLIGHT="64")
    assert tight["stream_prefetch_throttled"] > 0, (
        "a 1-entry in-flight budget throttled nothing")
    assert tight["stream_prefetch_issued"] < loose["stream_prefetch_issued"], (
        "the in-flight budget did not restrict issue")


def test_oracle_issues_far_more_freely_than_the_honest_model():
    """Quantifies what the oracle was worth: unbounded, cost-free issue."""
    oracle = run(CACHE_STREAM_PREFETCH_MODEL="oracle")
    stride = run(CACHE_STREAM_PREFETCH_MODEL="stride")
    assert oracle["stream_prefetch_issued"] > stride["stream_prefetch_issued"]


def test_prefetcher_is_off_by_default():
    stats = run(CACHE_STREAM_PREFETCH_DEGREE="0")
    assert stats["prefetch_fills"] == 0
    assert stats["stream_prefetch_issued"] == 0


def test_non_temporal_path_uses_the_same_detector():
    """K2 records and P-OPT matrix columns must not keep oracle coverage.

    accessNonTemporal() carries both metadata streams. It previously issued
    prefetches unconditionally regardless of the selected model, so the honest
    detector never reached the streams the K2-versus-P-OPT comparison turns on.
    With a 1-entry in-flight budget the detector must throttle that path too.
    """
    stats = run(POPT_MATRIX_STREAM_SIM=1, CACHE_POLICY="POPT",
                CACHE_STREAM_PREFETCH_MAX_INFLIGHT="1")
    assert stats["popt_matrix_stream_lines_simulated"] > 0, (
        "matrix stream did not run; this test would be vacuous")
    assert stats["stream_prefetch_throttled"] > 0, (
        "the matrix-stream path issued without consulting the in-flight budget, "
        "so it is still on an unconditional issue loop")


def test_counters_are_roi_scoped():
    """Counters must reset with the others, not span the pre-ROI warm replay."""
    stats = run(CACHE_STREAM_PREFETCH_DEGREE="0")
    # With the prefetcher off inside the ROI, a nonzero count could only come
    # from accesses outside it.
    assert stats["stream_prefetch_issued"] == 0
    assert stats["stream_prefetch_untrained"] == 0
