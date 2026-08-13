"""Concurrency regression tests for shared online-dueling evidence.

Sniper simulates each core as a real OS thread sharing one LLC set/selector
(unlike gem5's single-threaded discrete-event model), so
``ecg_policy::OnlineDuelingSelector`` (bench/include/ecg_victim_policy.h --
byte-identical across cache_sim/gem5/Sniper, see
test_shared_ecg_policy.py) used to be driven by Sniper's
cache_set_ecg.cc overlay via separate before/after reads of its counters
bracketing a ``recordMiss()`` call. That before/after diff races against
every OTHER core thread's concurrent ``recordMiss()`` call on the SAME
selector, and can misattribute (double-count or drop) window completions /
winner changes.

The fix changed ``recordMiss()`` to return a ``MissRecordEvent`` describing
only what THAT call itself did, derived from the return values of its own
atomic fetch_add/exchange operations (which are unique per calling thread by
construction), eliminating the race without changing the dueling decision
logic itself.

This module does NOT launch any Sniper (or gem5) simulation. It compiles and
runs bench/src_sim/test_ecg_online_dueling_concurrency.cc -- a small,
self-contained C++ program that drives the shared header directly with real
``std::thread`` concurrency (8 threads hammering one shared
``OnlineDuelingSelector``, plus a second 128-thread oversubscribed stress
scenario) and asserts exact accounting invariants a racy before/after-diff
implementation would violate under contention (see that file's header
comment for the full list, including the transition-chain conservation
invariant that specifically catches a stale/duplicated ``winner_before``
under overlapping window completions). It is compiled fresh by this test
module (not merely checked for a pre-built binary), so a clean checkout with
no prior ``make`` run still exercises the regression.

A second test additionally builds and runs the same program instrumented
with ``-fsanitize=thread`` (ThreadSanitizer) for an independent, tool-backed
proof that no data race exists on the shared atomics under concurrent
``recordMiss()`` calls. That test is skipped (not failed) if this
environment's compiler/libc combination cannot produce a runnable
ThreadSanitizer binary (checked at compile *and* run time), since
ThreadSanitizer availability is environment-dependent and unrelated to the
correctness of the fix itself.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "bench/src_sim/test_ecg_online_dueling_concurrency.cc"
INCLUDE_DIR = ROOT / "bench/include"
BIN_DIR = ROOT / "bench/bin_sim"

EXPECTED_PASS_LINE = "== 14 passed, 0 failed =="


def _compile(output: Path, extra_flags: list) -> subprocess.CompletedProcess:
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [
        "g++", "-std=c++17", "-I", str(INCLUDE_DIR),
        *extra_flags, str(SRC), "-o", str(output),
    ]
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=120, check=False)


def test_online_dueling_concurrency_invariants_hold_under_real_threads():
    """Compiles and runs the multicore concurrency regression program.

    Drives 8 real OS threads concurrently calling the shared
    ``OnlineDuelingSelector::recordMiss()`` on one shared selector instance
    and asserts every window/winner-change is counted exactly once (never
    double-counted or dropped), matching the selector's own atomic
    ``completedWindows()`` counter -- the precise defect class the
    before/after-diff implementation was vulnerable to.
    """
    binary = BIN_DIR / "test_ecg_online_dueling_concurrency"
    compiled = _compile(binary, ["-O2", "-pthread"])
    assert compiled.returncode == 0, (
        f"failed to compile the online-dueling concurrency regression:\n"
        f"{compiled.stdout}\n{compiled.stderr}")

    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60,
        check=False)
    assert result.returncode == 0, (
        f"online-dueling concurrency regression FAILED:\n{result.stdout}\n"
        f"{result.stderr}")
    assert EXPECTED_PASS_LINE in result.stdout, result.stdout
    assert "[FAIL]" not in result.stdout, result.stdout


def test_online_dueling_concurrency_is_threadsanitizer_clean():
    """ThreadSanitizer-instrumented run: independent proof of no data race.

    Skipped (not failed) if this environment cannot produce a runnable TSan
    binary (missing libtsan, unsupported sandbox memory layout, etc.) --
    that is an environment limitation, not evidence about the fix.
    """
    binary = BIN_DIR / "test_ecg_online_dueling_concurrency_tsan"
    compiled = _compile(
        binary, ["-O1", "-g", "-fsanitize=thread", "-pthread", "-no-pie"])
    if compiled.returncode != 0:
        pytest.skip(
            "ThreadSanitizer toolchain unavailable in this environment: "
            f"{compiled.stderr.strip()[-500:]}")

    env = dict(os.environ)
    env["TSAN_OPTIONS"] = "halt_on_error=0"
    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60,
        check=False, env=env)
    if result.returncode != 0 and "ThreadSanitizer: unexpected memory" in (
            result.stdout + result.stderr):
        pytest.skip(
            "ThreadSanitizer could not map memory in this sandboxed "
            f"environment: {(result.stdout + result.stderr)[-500:]}")

    assert "WARNING: ThreadSanitizer: data race" not in result.stdout, (
        f"ThreadSanitizer detected a DATA RACE in the online-dueling "
        f"evidence refactor:\n{result.stdout}\n{result.stderr}")
    assert result.returncode == 0, (
        f"ThreadSanitizer run failed unexpectedly:\n{result.stdout}\n"
        f"{result.stderr}")
    assert EXPECTED_PASS_LINE in result.stdout, result.stdout
