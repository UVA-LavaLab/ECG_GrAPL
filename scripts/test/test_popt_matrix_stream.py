#!/usr/bin/env python3
"""cache_sim must charge P-OPT's matrix stream once per sweep, not once per run.

The first version of the simulated column stream only charged forward epoch
progress. PageRank sweeps epochs 0..N-1 once per iteration, so every sweep after
the first was silently free: the stream cost was identical at -i 1, -i 2 and
-i 4. That reproduced the same undercharge as the flat analytic count, which is
also a single sweep, and it undercharged P-OPT by the iteration count.

The residency model replaces it: an epoch whose column is still one of the two
resident columns costs nothing, anything else streams a fresh column. These
tests pin that behaviour against the real binary.
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


def run_pr(iterations: int, extra_env: dict | None = None) -> dict:
    """Run PageRank on a small synthetic graph and return the stats JSON."""
    env = dict(os.environ)
    env.pop("POPT_SE_POSTFINAL", None)
    env.update({
        "OMP_NUM_THREADS": "1",
        "CACHE_ULTRAFAST": "0",
        "CACHE_POLICY": "POPT",
        # Small enough to stay fast, small enough that the property array does
        # not fit the LLC, so the cell is not degenerate.
        "CACHE_L1_SIZE": "1024",
        "CACHE_L2_SIZE": "2048",
        "CACHE_L3_SIZE": "8192",
        "CACHE_L3_WAYS": "16",
        "POPT_MATRIX_STREAM_SIM": "1",
    })
    env.update(extra_env or {})
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        subprocess.run(
            [str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1",
             "-i", str(iterations)],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        return json.loads(out.read_text())


@pytest.mark.parametrize("postfinal", [None, "later_lower_bound", "distant"])
def test_stream_is_charged_once_per_sweep(postfinal):
    """Columns must scale with iterations, not be fixed at one sweep."""
    env = {"POPT_SE_POSTFINAL": postfinal} if postfinal else {}
    one = run_pr(1, env)
    two = run_pr(2, env)
    c1 = one["popt_matrix_stream_columns_simulated"]
    c2 = two["popt_matrix_stream_columns_simulated"]
    assert c1 > 0, "matrix stream never fired"
    # Exactly one column per epoch per sweep.
    assert c2 == 2 * c1, (
        f"expected two sweeps to stream twice as many columns, got {c1} then {c2}; "
        "a fixed count means later sweeps are silently free")


def test_stream_lines_track_columns():
    stats = run_pr(1)
    columns = stats["popt_matrix_stream_columns_simulated"]
    lines = stats["popt_matrix_stream_lines_simulated"]
    assert columns > 0 and lines > 0
    assert lines % columns == 0, "every column must stream the same line count"


def test_stream_is_off_by_default():
    """The stream must never appear unless explicitly requested."""
    stats = run_pr(1, {"POPT_MATRIX_STREAM_SIM": "0"})
    assert stats["popt_matrix_stream_columns_simulated"] == 0
    assert stats["popt_matrix_stream_lines_simulated"] == 0


@pytest.mark.parametrize("postfinal", [None, "later_lower_bound", "distant"])
def test_stream_adds_traffic_without_a_prefetcher(postfinal):
    """A cold sequential stream must cost real memory traffic."""
    env = {"POPT_SE_POSTFINAL": postfinal} if postfinal else {}
    off = run_pr(1, {**env, "POPT_MATRIX_STREAM_SIM": "0"})
    on = run_pr(1, env)
    added = on["total_memory_traffic"] - off["total_memory_traffic"]
    lines = on["popt_matrix_stream_lines_simulated"]
    # Nearly every line of a cold stream misses; allow slack for the few served
    # by the private caches.
    assert added >= 0.9 * lines, (
        f"stream of {lines} lines added only {added} traffic")


def test_gem5_popt_matrix_stream_is_disclosed_as_analytic():
    """gem5 P-OPT does not pay for its own matrix, so it must say so.

    The rereference matrix reaches the gem5 replacement policy through a
    sideband file, so streaming columns costs no simulated traffic, no latency
    and no instructions -- while ReusePlan pays for its records in full. Left
    undisclosed, that asymmetry reads as P-OPT beating everything.

    The runner must record the omitted stream, and the analysis must surface it
    before any ratio is shown.
    """
    runner = (ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()
    assert "popt_matrix_stream_mode" in runner
    assert "popt_matrix_stream_bytes" in runner

    analysis = (ROOT / "scripts/experiments/ecg/analysis"
                / "record_width_timing.py").read_text()
    assert "popt_matrix_stream_mode" in analysis, (
        "the analysis must read the charging mode, or an idealised P-OPT row "
        "is presented as a comparable baseline")
    assert "optimistic lower bound" in analysis, (
        "the report must disclose that analytic stream latency favors P-OPT")
    marker = analysis.index("popt_matrix_stream_mode")
    ratios = analysis.index("2. TIME AND TRAFFIC versus LRU")
    assert marker < ratios, (
        "the disclosure must come BEFORE the ratios, not after them")


def test_decode_matrix_report_refuses_to_present_times_as_speedup():
    """The decode stages are instruction and traffic evidence, not speedup.

    The runner marks the packed+ecg.extract2(c) delivery family
    timing_valid_for_speedup=0 because the property load is still a separate
    instruction rather than a fused request-bound one. An analysis that prints
    times without surfacing that flag invites the reader to treat them as a
    speedup claim, which is the failure the flag exists to prevent.

    It must also refuse to call the FUSED 4b/8b pair a width contrast: the
    fused load family takes only the 64-bit record, so its compact arm still
    widens in software.
    """
    src = (ROOT / "scripts/experiments/ecg/analysis"
           / "record_width_timing.py").read_text()
    assert "timing_valid_for_speedup" in src, (
        "the decode report does not read the admissibility flag, so an "
        "inadmissible time can be printed as though it were a result")
    assert "NOT SPEEDUP EVIDENCE" in src
    assert "NOT a width-only contrast" in src, (
        "the fused pair must be labelled width PLUS decode, or it reads as "
        "the width contrast it cannot be")
    # The width contrast must be the pair where BOTH arms decode in one
    # instruction, not the pair that merely differs in declared bytes.
    i_width = src.index("WIDTH: compact versus wide")
    assert "43_isa_plain_4b_hardware" in src[i_width - 400:i_width]
    assert "44_isa_plain_8b" in src[i_width - 400:i_width]


def test_idealised_mechanism_warning_is_reachable_on_every_path():
    """It was printed inline in main() and the decode report returned first.

    So the moment a decode matrix was analysed, P-OPT rows were displayed with
    the caveat that makes them readable silently skipped. A warning that only
    fires on the path nobody takes is worse than no warning, because the report
    looks complete.
    """
    src = (ROOT / "scripts/experiments/ecg/analysis"
           / "record_width_timing.py").read_text()
    assert "def report_idealised_mechanisms(" in src, (
        "the charging disclosure must be a function, or it cannot be reached "
        "from more than one report path")
    # It must be invoked BEFORE the decode report, not after the numbers.
    i_call = src.index("report_idealised_mechanisms(rows)\n        report_decode_matrix")
    assert i_call > 0, "the disclosure must precede the decode ratios"


def test_partial_matrix_announces_itself():
    """A stage that has only produced its LRU cell must not look like a result.

    The loader drops rows whose status is not ok, so an unfinished stage is
    indistinguishable from a finished one with nothing to report, and a
    single-graph number printed under "geomean" reads as a graph-set result.
    """
    src = (ROOT / "scripts/experiments/ecg/analysis"
           / "record_width_timing.py").read_text()
    assert "INCOMPLETE" in src
    assert "def report_coverage(" in src
    assert 'f"geomean n={len(pairs)}"' in src, (
        "every geomean must carry the number of cells behind it")


def test_decode_report_does_not_claim_traffic_agreement_is_proof():
    """Decode changes instruction fetch, spills and ordering, so it CAN move bytes.

    Traffic near 1.0 is consistent with a decode-only difference; it does not
    establish one. The earlier wording asserted a pure decode difference
    "cannot move bytes", which is false and would survive a superficial
    until they asked about instruction-cache behaviour.
    """
    src = (ROOT / "scripts/experiments/ecg/analysis"
           / "record_width_timing.py").read_text()
    assert "cannot move bytes" not in src
    assert "does NOT prove one" in src
