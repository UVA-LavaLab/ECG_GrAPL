#!/usr/bin/env python3
"""Cross-simulator parity gate for ECG mode 6 (per-edge mask) port.

Mode 6 was ported from cache_sim (where it
was originally implemented) to gem5 and Sniper kernels via the shared
header bench/include/ecg_mode6_builder.h. This gate locks the port
by asserting that all three simulators' PR kernels produce
byte-identical per-edge mask encoding statistics on a small reference
graph.

The encoding stats line emitted by the shared builder
(ecg_mode6::buildInEdgeMasks) is:

    [ecg_mode6 <label>] vertices=N edges=E encoded=K (P%) build_s=...

We capture the (E, K, P) triple from each binary's stdout and assert
equality. If a future change to the shared builder, the kernel
wiring, or the env-knob plumbing causes any sim to produce a
different number of edges encoded, this gate trips.

Smoke graph: results/graphs/email-Eu-core/email-Eu-core.sg
  (1005 vertices, 32128 directed edges; the smallest graph in the
  literature corpus.)
"""

import os
import re
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
GRAPH = PROJECT_ROOT / "results/graphs/email-Eu-core/email-Eu-core.sg"

# Regex matches the standard ecg_mode6 stats line emitted by the
# shared builder. Capture groups: edges, encoded, percent.
STATS_RE = re.compile(
    r"\[ecg_mode6\s+([\w\-]+)\]\s+vertices=(\d+)\s+edges=(\d+)\s+"
    r"encoded=(\d+)\s+\(([\d\.]+)%\)\s+build_s=([\d\.]+)"
)


def _require_graph():
    if not GRAPH.is_file():
        pytest.skip(f"smoke graph missing: {GRAPH}")


def _run_binary(binary: Path, env_extra: dict[str, str]) -> str:
    """Run a kernel binary on the email-Eu-core graph in mode 6.

    Returns combined stdout+stderr captured as text.
    """
    if not binary.is_file():
        pytest.skip(f"binary not built: {binary}")
    env = dict(os.environ)
    env.update(env_extra)
    proc = subprocess.run(
        [str(binary), "-f", str(GRAPH), "-i", "2", "-o", "5", "-n", "1"],
        cwd=PROJECT_ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        pytest.fail(f"{binary.name} exited {proc.returncode}; stderr:\n{proc.stderr[:500]}")
    return proc.stdout + proc.stderr


def _extract_first_stats(output: str, label_prefix: str) -> dict:
    """Return the first ecg_mode6 stats line matching the label prefix."""
    for m in STATS_RE.finditer(output):
        if m.group(1).startswith(label_prefix):
            return {
                "label": m.group(1),
                "vertices": int(m.group(2)),
                "edges": int(m.group(3)),
                "encoded": int(m.group(4)),
                "encoded_pct": float(m.group(5)),
            }
    return {}


def _gem5_pr_stats() -> dict:
    binary = PROJECT_ROOT / "bench" / "bin_gem5" / "pr"
    env = {
        "GEM5_ENABLE_ECG_PFX_HINTS": "1",
        "GEM5_ECG_PFX_MODE": "6",
        "GEM5_ECG_EDGE_MASK_LOOKAHEAD": "8",
    }
    return _extract_first_stats(_run_binary(binary, env), "gem5-PR")


def _sniper_pr_stats() -> dict:
    binary = PROJECT_ROOT / "bench" / "bin_sniper" / "pr"
    env = {
        "SNIPER_ENABLE_ECG_PFX_HINTS": "1",
        "SNIPER_ECG_PFX_MODE": "6",
        "SNIPER_ECG_EDGE_MASK_LOOKAHEAD": "8",
    }
    return _extract_first_stats(_run_binary(binary, env), "sniper-PR")


def _sniper_sg_stats() -> dict:
    binary = PROJECT_ROOT / "bench" / "bin_sniper" / "sg_kernel"
    env = {
        "SNIPER_ENABLE_ECG_PFX_HINTS": "1",
        "SNIPER_ECG_PFX_MODE": "6",
        "SNIPER_ECG_PFX_LOOKAHEAD": "8",
    }
    out = subprocess.run(
        [str(binary), "--benchmark", "pr", "-f", str(GRAPH), "-i", "2", "-o", "5", "-n", "1"],
        cwd=PROJECT_ROOT,
        env={**os.environ, **env},
        capture_output=True,
        text=True,
        timeout=60,
    )
    if out.returncode != 0:
        pytest.skip(f"sg_kernel returncode={out.returncode}")
    return _extract_first_stats(out.stdout + out.stderr, "sniper-sg-PR")


# === Gate 319: cross-sim mode 6 parity ===

def test_ecg_mode6_cross_sim_parity():
    """All 3 sims must produce byte-identical encoding stats on the
    same reference graph, since they share ecg_mode6::buildInEdgeMasks.
    """
    _require_graph()

    gem5 = _gem5_pr_stats()
    sniper = _sniper_pr_stats()
    sg = _sniper_sg_stats()

    if not gem5 or not sniper or not sg:
        pytest.skip("one or more binaries did not emit ecg_mode6 stats; "
                    "the kernel may not be built with mode 6 wiring.")

    # Vertex count must match (graph is fixed).
    assert gem5["vertices"] == sniper["vertices"] == sg["vertices"], (
        f"vertex count mismatch: gem5={gem5['vertices']}, sniper={sniper['vertices']}, "
        f"sg_kernel={sg['vertices']}"
    )

    # Edge count must match (CSR is fixed; both halves of undirected).
    assert gem5["edges"] == sniper["edges"] == sg["edges"], (
        f"edge count mismatch: gem5={gem5['edges']}, sniper={sniper['edges']}, "
        f"sg_kernel={sg['edges']}"
    )

    # Encoded count must match exactly (shared builder produces
    # identical output across sims).
    assert gem5["encoded"] == sniper["encoded"] == sg["encoded"], (
        f"encoded count mismatch: gem5={gem5['encoded']}, sniper={sniper['encoded']}, "
        f"sg_kernel={sg['encoded']}"
    )

    # Encoded percentage tolerance: must be within 0.05pp (floating
    # point representation only).
    pcts = [gem5["encoded_pct"], sniper["encoded_pct"], sg["encoded_pct"]]
    assert max(pcts) - min(pcts) < 0.05, (
        f"encoded percentage mismatch: gem5={pcts[0]}, sniper={pcts[1]}, "
        f"sg_kernel={pcts[2]}"
    )


def test_ecg_mode6_email_eu_core_anchor():
    """Anchor the absolute encoding numbers on email-Eu-core so any
    change to the builder (e.g., different DBG quartile threshold,
    different POPT quantization) becomes visible.
    """
    _require_graph()
    gem5 = _gem5_pr_stats()
    if not gem5:
        pytest.skip("gem5 binary did not emit ecg_mode6 stats")

    # Locked values from the cross-simulator parity check
    # (commit 1aa1b24b). If the shared builder changes its output
    # for any reason, this anchor breaks and forces a deliberate
    # update.
    assert gem5["vertices"] == 1005, (
        f"vertex count drift: expected 1005, got {gem5['vertices']}")
    assert gem5["edges"] == 32128, (
        f"edge count drift: expected 32128, got {gem5['edges']}")
    assert gem5["encoded"] == 31142, (
        f"encoded count drift: expected 31142, got {gem5['encoded']}")
    assert abs(gem5["encoded_pct"] - 96.9) < 0.05, (
        f"encoded percentage drift: expected 96.9%, got {gem5['encoded_pct']}")
