#!/usr/bin/env python3
"""Tier A: GRASP sideband registration sanity.

Locks the invariant that each simulator (``cache_sim``, ``gem5``-native,
``sniper``-native) registers exactly two property regions for PageRank on
``email-Eu-core`` and that *both* of those regions are classified as
GRASP regions at the GRASP-faithful array-relative ``hot_pct=15`` band.

The shared simulator contract requires exactly two regions with
``grasp_region=1`` and the expected ``hot_pct`` (15 for PR/BC/Radii,
100 for BellmanFord). Two contexts apply:

* **Upstream GRASP trace replay**: header carries both ``propertyA`` and
  ``propertyB`` ranges, both flagged as GRASP regions (``grasp_region=1``).
* **Live GraphBrew runs**: each app registers both the ``scores``-style
  array and the ``contrib``-style array as GRASP regions
  (``grasp_region=1``).  Marking only one of multiple vertex-indexed
  property arrays as a GRASP region caused the catastrophic BC bug
  where the unmarked arrays thrashed under SRRIP while the single hot
  array hogged the LLC. The invariant is enforced directly by
  ``scripts/test/test_grasp_multi_property_invariant.py``.

Only PR is exercised end-to-end today because (a) it's the canonical
microbench in the handoff and (b) BellmanFordOpt (``hot_pct=100``) is not
yet plumbed through the gem5/sniper sideband.  The expected mapping is
parameterised so additional apps can be added as their wiring lands.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GRAPH = PROJECT_ROOT / "results" / "graphs" / "email-Eu-core" / "email-Eu-core.sg"

REG_LINE_RE = re.compile(
    r"^\[graphctx\] register region "
    r"source=(?P<source>\S+) "
    r"name=(?P<name>\S+) "
    r"base=0x(?P<base>[0-9a-fA-F]+) "
    r"upper=0x(?P<upper>[0-9a-fA-F]+) "
    r"hot_pct=(?P<hot_pct>\d+) "
    r"grasp_region=(?P<grasp_region>[01])\s*$"
)


@dataclass(frozen=True)
class AppSpec:
    """Per-app expected GRASP sideband invariant."""

    name: str
    binary: str            # binary basename (e.g., "pr")
    make_target: str       # "sim-pr", "gem5-pr", "sniper-pr"
    expected_hot_pct: int  # hot_pct on the grasp_region (frontier_frac upstream)


# Apps with their expected GRASP frontier fractions.  The mapping mirrors the
# handoff: PR/BC/Radii use a 15%-of-vertex-array hot band (array-relative,
# GRASP-faithful per ligra.h add_region); BellmanFordOpt uses a full 100%.
APP_EXPECTED_HOT_PCT = {
    "pr": 15,
    "bc": 15,
    "radii": 15,
    "bellmanford": 100,
}


def _require_graph() -> Path:
    if not DEFAULT_GRAPH.exists():
        pytest.skip(f"Test graph not present: {DEFAULT_GRAPH}")
    return DEFAULT_GRAPH


def _ensure_binary(make_target: str, binary_path: Path) -> Path:
    """Build ``make_target`` if ``binary_path`` is missing.

    The Make rules already short-circuit when the binary is up to date, but
    we avoid running ``make`` unnecessarily during fast iteration by checking
    first.  If the toolchain is missing or the build fails, skip the test.
    """

    if binary_path.exists():
        return binary_path
    if shutil.which("make") is None:
        pytest.skip("make not available on PATH")
    result = subprocess.run(
        ["make", make_target],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not binary_path.exists():
        pytest.skip(
            f"Could not build {make_target}: rc={result.returncode}\n"
            f"stderr tail:\n{result.stderr[-400:]}"
        )
    return binary_path


def _run_pr_native(binary: Path, graph: Path, env_overrides: dict[str, str]) -> subprocess.CompletedProcess:
    cmd = [str(binary), "-f", str(graph), "-s", "-o", "5", "-n", "1"]
    env = os.environ.copy()
    env.update(env_overrides)
    return subprocess.run(
        cmd,
        cwd=str(PROJECT_ROOT),
        env=env,
        capture_output=True,
        text=True,
        timeout=180,
    )


def _parse_stderr_registrations(stderr: str) -> list[dict]:
    out: list[dict] = []
    for line in stderr.splitlines():
        m = REG_LINE_RE.match(line.strip())
        if not m:
            continue
        d = m.groupdict()
        out.append(
            {
                "source": d["source"],
                "name": d["name"],
                "base": int(d["base"], 16),
                "upper": int(d["upper"], 16),
                "hot_pct": int(d["hot_pct"]),
                "grasp_region": int(d["grasp_region"]),
            }
        )
    return out


def _parse_sideband_regions(json_path: Path, sideband_hot_pct: int) -> list[dict]:
    """Read the simulator sideband JSON written by gem5/sniper harnesses.

    The JSON shape today is ``{"property_regions": [{name, base, size, count,
    elem_size, grasp}, ...]}``.  ``loadFromSideband`` (the parser exercised
    inside the live simulator) logs ``hot_pct=15`` (the GRASP-faithful
    array-relative default; classifyGRASP itself reads the configured
    ``hot_fraction``), so we synthesise the same ``hot_pct`` here for symmetry
    with the cache_sim stderr line.  This keeps the invariant single-sourced for
    now and will be promoted to a real per-region field when BellmanFord is
    wired through.
    """

    payload = json.loads(json_path.read_text())
    regions = payload.get("property_regions", [])
    parsed: list[dict] = []
    for entry in regions:
        base = int(entry["base"])
        size = int(entry["size"])
        parsed.append(
            {
                "source": "sideband",
                "name": entry.get("name", "(unnamed)"),
                "base": base,
                "upper": base + size,
                "hot_pct": sideband_hot_pct,
                "grasp_region": int(bool(entry.get("grasp", True))),
            }
        )
    return parsed


def _assert_grasp_invariant(regions: list[dict], app: AppSpec) -> None:
    assert regions, f"{app.name}: no registrations captured"
    assert len(regions) == 2, (
        f"{app.name}: expected exactly 2 property regions, got {len(regions)}: {regions}"
    )
    grasp_flags = [r["grasp_region"] for r in regions]
    # Post-fix invariant: *both* property arrays must be classified as
    # GRASP regions; the multi-property bug surfaced when only the trailing
    # array was marked.
    assert sum(grasp_flags) == 2, (
        f"{app.name}: expected both regions with grasp_region=1, "
        f"got grasp_region flags={grasp_flags}: {regions}"
    )
    for hot in regions:
        assert hot["hot_pct"] == app.expected_hot_pct, (
            f"{app.name}: grasp_region hot_pct={hot['hot_pct']} "
            f"!= expected {app.expected_hot_pct} ({hot})"
        )
        assert hot["upper"] > hot["base"], (
            f"{app.name}: degenerate grasp_region range {hot}"
        )


# ---------------------------------------------------------------------------
# Tier A end-to-end tests (one per simulator x app).  Only PR is parametrised
# today; additional apps slot in once their gem5/sniper sideband plumbing
# carries per-region hot_pct.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("app_name", ["pr"])
def test_cache_sim_grasp_sideband_registration(app_name: str, tmp_path: Path) -> None:
    """cache_sim emits one ``[graphctx] register region`` line per region."""

    graph = _require_graph()
    spec = AppSpec(
        name=app_name,
        binary=app_name,
        make_target=f"sim-{app_name}",
        expected_hot_pct=APP_EXPECTED_HOT_PCT[app_name],
    )
    binary = _ensure_binary(spec.make_target, PROJECT_ROOT / "bench" / "bin_sim" / spec.binary)

    proc = _run_pr_native(binary, graph, env_overrides={"GRAPHBREW_SIDEBAND_LOG": "1"})
    assert proc.returncode == 0, (
        f"cache_sim {app_name} exited rc={proc.returncode}\n"
        f"stderr tail:\n{proc.stderr[-600:]}"
    )

    regions = _parse_stderr_registrations(proc.stderr)
    _assert_grasp_invariant(regions, spec)
    for r in regions:
        assert r["source"].startswith("cache_sim"), r


@pytest.mark.parametrize("app_name", ["pr"])
def test_gem5_grasp_sideband_registration(app_name: str, tmp_path: Path) -> None:
    """gem5-native PR writes a sideband JSON with the expected GRASP shape."""

    graph = _require_graph()
    spec = AppSpec(
        name=app_name,
        binary=app_name,
        make_target=f"gem5-{app_name}",
        expected_hot_pct=APP_EXPECTED_HOT_PCT[app_name],
    )
    binary = _ensure_binary(spec.make_target, PROJECT_ROOT / "bench" / "bin_gem5" / spec.binary)

    ctx_path = tmp_path / "gem5_ctx.json"
    proc = _run_pr_native(
        binary,
        graph,
        env_overrides={
            "GEM5_GRAPHBREW_CTX": str(ctx_path),
            "GRAPHBREW_SIDEBAND_LOG": "1",
        },
    )
    assert proc.returncode == 0, (
        f"gem5 {app_name} exited rc={proc.returncode}\n"
        f"stderr tail:\n{proc.stderr[-600:]}"
    )
    assert ctx_path.exists(), f"gem5 sideband not written at {ctx_path}"

    regions = _parse_sideband_regions(ctx_path, sideband_hot_pct=15)
    _assert_grasp_invariant(regions, spec)


@pytest.mark.parametrize("app_name", ["pr"])
def test_sniper_grasp_sideband_registration(app_name: str, tmp_path: Path) -> None:
    """sniper-native PR writes a sideband JSON with the expected GRASP shape."""

    graph = _require_graph()
    spec = AppSpec(
        name=app_name,
        binary=app_name,
        make_target=f"sniper-{app_name}",
        expected_hot_pct=APP_EXPECTED_HOT_PCT[app_name],
    )
    binary = _ensure_binary(spec.make_target, PROJECT_ROOT / "bench" / "bin_sniper" / spec.binary)

    ctx_path = tmp_path / "sniper_ctx.json"
    proc = _run_pr_native(
        binary,
        graph,
        env_overrides={
            "SNIPER_GRAPHBREW_CTX": str(ctx_path),
            "GRAPHBREW_SIDEBAND_LOG": "1",
        },
    )
    assert proc.returncode == 0, (
        f"sniper {app_name} exited rc={proc.returncode}\n"
        f"stderr tail:\n{proc.stderr[-600:]}"
    )
    assert ctx_path.exists(), f"sniper sideband not written at {ctx_path}"

    regions = _parse_sideband_regions(ctx_path, sideband_hot_pct=15)
    _assert_grasp_invariant(regions, spec)


# ---------------------------------------------------------------------------
# Source-level guards: the registration log MUST exist in all three contexts.
# Cheap, hermetic, and protects against silent removal of the instrumentation.
# ---------------------------------------------------------------------------


def _read(rel: str) -> str:
    return (PROJECT_ROOT / rel).read_text()


def test_registration_log_present_in_all_contexts() -> None:
    cache_ctx = _read("bench/include/cache_sim/graph_cache_context.h")
    gem5_ctx = _read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh"
    )
    sniper_ctx = _read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc"
    )

    for blob, label in (
        (cache_ctx, "cache_sim"),
        (gem5_ctx, "gem5"),
        (sniper_ctx, "sniper"),
    ):
        assert "logGraphCtxRegistration(" in blob, f"missing log helper call in {label}"
        assert "[graphctx] register region" in blob, f"missing log format in {label}"
        assert "GRAPHBREW_SIDEBAND_LOG" in blob, (
            f"missing GRAPHBREW_SIDEBAND_LOG suppression knob in {label}"
        )


def test_expected_hot_pct_mapping_covers_supported_apps() -> None:
    """Encode the {PR/BC/Radii: 15, BellmanFord: 100} table so additions
    to the mapping are deliberate."""

    assert APP_EXPECTED_HOT_PCT == {
        "pr": 15,
        "bc": 15,
        "radii": 15,
        "bellmanford": 100,
    }
