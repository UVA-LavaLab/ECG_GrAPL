#!/usr/bin/env python3
"""S2 (narrow sidecar) must be semantically identical to S1 (packed record).

The implementation uses a 15-cell conformance gate: the eviction decision in
`ecg_victim_policy.h` is kernel-agnostic and byte-identical across cache_sim,
gem5 and Sniper. Introducing a second metadata delivery structure threatens
that gate unless the structure is provably transport-only.

It is. The gate verifies victim decisions given the epochs, not how the epochs
reached the policy. So if S2 delivers the same stamps as S1, every victim
decision is unchanged and conformance is preserved by construction.

These tests prove the antecedent the cheap way: charge nothing for the metadata
in either structure, which removes transport from both, and require the two to
produce byte-identical cache behaviour. Any divergence means S2 is delivering
different stamps and is therefore not a drop-in structure.
"""
from __future__ import annotations

import json
import re
import difflib
import os
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PR = ROOT / "bench/bin_sim/pr"

pytestmark = pytest.mark.skipif(
    not PR.exists(), reason="cache_sim pr binary not built")

COMMON = {
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
    "ECG_EDGE_MASK_EPOCH": "1",
    "ECG_EDGE_MASK_LEAN": "1",
    "ECG_EDGE_MASK_PACK": "1",
    "ECG_EDGE_MASK_LINEMIN": "1",
    "ECG_EXACT_REREF": "1",
    "ECG_PREFETCH_MODE": "6",
    "ECG_EDGE_MASK_EPOCHS": "32",
    "ECG_VARIANT": "epoch_first",
}


def run(**overrides) -> dict:
    env = dict(os.environ)
    env.update(COMMON)
    env.update({k: str(v) for k, v in overrides.items()})
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "stats.json"
        env["CACHE_OUTPUT_JSON"] = str(out)
        # ASLR must be off: cache_sim tracks real pointers, so address-space
        # randomisation perturbs cache set mapping by ~0.07% run to run, which
        # is larger than the differences these gates assert on.
        subprocess.run(
            ["/usr/bin/setarch", "x86_64", "-R",
             str(PR), "-g", "12", "-k", "8", "-o", "5", "-n", "1", "-i", "2"],
            env=env, capture_output=True, text=True, check=True, timeout=900)
        return json.loads(out.read_text())


def signature(stats: dict) -> dict:
    return {
        "offchip": stats["total_offchip_traffic"],
        "reads": stats["total_memory_traffic"],
        "writebacks": stats["llc_writebacks"],
        "l1_misses": stats["L1"]["misses"],
        "l2_misses": stats["L2"]["misses"],
        "l3_misses": stats["L3"]["misses"],
    }


def test_results_are_deterministic():
    """Everything below assumes exact equality, so prove runs are repeatable."""
    a = signature(run(ECG_EDGE_MASK_CHARGED=1))
    b = signature(run(ECG_EDGE_MASK_CHARGED=1))
    assert a == b, f"cache_sim is not deterministic under setarch -R:\n{a}\n{b}"


def test_sidecar_is_transport_only():
    """Uncharged S1 and S2 must be byte-identical at every cache level."""
    s1 = signature(run(ECG_EDGE_MASK_CHARGED=0))
    s2 = signature(run(ECG_EDGE_MASK_CHARGED=0, ECG_DELIVERY="sidecar"))
    assert s1 == s2, (
        "S2 changed cache behaviour with transport removed, so it is NOT "
        f"delivering the same stamps as S1:\nS1={s1}\nS2={s2}")


def test_sidecar_costs_traffic_when_charged():
    """The gate above must not pass by the sidecar simply doing nothing."""
    free = run(ECG_EDGE_MASK_CHARGED=0, ECG_DELIVERY="sidecar")["total_offchip_traffic"]
    charged = run(ECG_EDGE_MASK_CHARGED=1, ECG_DELIVERY="sidecar")["total_offchip_traffic"]
    assert charged > free, (
        "charging the sidecar did not add traffic, so it is not being "
        "simulated and the equivalence test above is vacuous")


def test_sidecar_payload_width_changes_cost_monotonically():
    """A wider payload must cost more; width must actually reach the model."""
    narrow = run(ECG_EDGE_MASK_CHARGED=1, ECG_DELIVERY="sidecar",
                 ECG_SIDECAR_PAYLOAD_BITS=6)["total_offchip_traffic"]
    wide = run(ECG_EDGE_MASK_CHARGED=1, ECG_DELIVERY="sidecar",
               ECG_SIDECAR_PAYLOAD_BITS=24)["total_offchip_traffic"]
    assert wide > narrow, (
        f"payload width did not affect cost: 6b={narrow} 24b={wide}")


def test_sidecar_width_is_independent_of_graph_size():
    """The whole point of S2: payload must not depend on vertex count.

    The packed record's width is id_bits + stamps*epoch_bits + tier_bits, so it
    grows with the graph. The sidecar carries no destination id, so two graphs
    of different size at the same payload setting must charge the same bits per
    edge.
    """
    header = ROOT / "bench/include/ecg_metadata.h"
    text = header.read_text()
    assert "payload_bits" in text
    start = text.index("const int forced_payload")
    body = text[start:text.index("const int needed", start)]
    assert "num_vertices" not in body, (
        "the sidecar payload consults the vertex count, so its width is not "
        "graph-size independent")
    assert "id_bits" not in body, (
        "the sidecar payload includes destination id bits it does not need; "
        "the CSR edge already carries the destination")


def test_every_cache_sim_kernel_uses_the_shared_metadata():
    """All five algorithms must share one delivery site, on every simulator.

    The implementation depends on a 15-cell conformance gate, so a kernel that
    delivers metadata its own way is a correctness risk, not just untidy. These
    checks fail if any kernel drifts back to a private chain.
    """
    for kernel in ("pr", "bfs", "cc", "bc", "sssp"):
        src = (ROOT / f"bench/src_sim/{kernel}.cc").read_text()
        assert "::ecg_metadata::configure(" in src, (
            f"{kernel} does not configure delivery from the shared implementation")
        assert "SIM_ECG_EDGE(" in src, (
            f"{kernel} does not use the single delivery site")
        assert "::ecg_metadata::announce(" in src, (
            f"{kernel} emits no configuration receipt")
        for dead in ("SIM_CACHE_READ_EDGE_RECORD(",
                     "SIM_CACHE_READ_EDGE_RECORD_FLOWTHROUGH(",
                     "GraphSimEcgRecordBytes("):
            assert dead not in src, (
                f"{kernel} still carries the superseded {dead}")


def test_all_three_simulators_share_the_shared_metadata():
    """cache_sim, gem5 and Sniper must derive width and structure from one header.

    ecg_victim_policy.h owns the eviction DECISION and is kept identical across
    the three by copying it into each overlay and hash-checking the copies.
    ecg_metadata.h owns TRANSPORT, and because it is consumed by the guest
    kernels rather than by simulator internals, all three can include the
    canonical file directly via -I bench/include. There is therefore nothing to
    copy and nothing that can drift -- but only as long as each simulator
    actually uses it, which is what this asserts.
    """
    canonical = ROOT / "bench/include/ecg_metadata.h"
    assert canonical.is_file(), "shared metadata implementation header is missing"

    consumers = {
        "cache_sim": [ROOT / f"bench/src_sim/{k}.cc"
                      for k in ("pr", "bfs", "cc", "bc", "sssp")],
        "gem5": [ROOT / f"bench/src_gem5/{k}.cc"
                 for k in ("pr", "bfs", "cc", "bc", "sssp")],
        "sniper": [ROOT / "bench/src_sniper/sg_kernel.cc"],
    }
    for sim, paths in consumers.items():
        for path in paths:
            src = path.read_text()
            assert "ecg_metadata.h" in src or "::ecg_metadata::" in src, (
                f"{sim} source {path.name} does not use the shared metadata implementation")

    # No simulator may keep a private width rule.
    for path in [p for paths in consumers.values() for p in paths]:
        src = path.read_text()
        assert "GraphSimEcgRecordBytes(" not in src, (
            f"{path.name} still computes record width locally")


def test_shared_metadata_has_no_simulator_dependencies():
    """It must stay includable by guest kernels on every backend.

    Checks code, not prose: the header names the simulators in its own
    documentation, which is fine. What must not appear is an include of, or a
    type from, any one backend.
    """
    lines = (ROOT / "bench/include/ecg_metadata.h").read_text().splitlines()
    code = "\n".join(
        l for l in lines if not l.lstrip().startswith("//"))
    for forbidden in ("cache_sim.h", "graph_sim.h", "CacheHierarchy",
                      "SimArray", "m5op", "sift"):
        assert forbidden not in code, (
            f"shared metadata implementation depends on {forbidden}, so it is no longer "
            "backend-neutral")
    # Only standard headers.
    includes = [l for l in lines if l.lstrip().startswith("#include")]
    assert includes, "header includes nothing at all"
    for inc in includes:
        assert "<" in inc, f"non-standard include in the shared implementation: {inc.strip()}"


# ---------------------------------------------------------------------------
# Cross-simulator width agreement
# ---------------------------------------------------------------------------

GEM5_PR = ROOT / "bench/bin_gem5/pr"
GRAPH = ROOT / "results/graphs/web-Google-n16/web-Google-n16.sg"

RECEIPT = re.compile(
    r"stamps=\d+ epoch_bits=\d+ tier_bits=\d+ id_bits=\d+ "
    r"record_bytes=\d+ payload_bits=\d+")


def _receipt(cmd, env):
    e = dict(os.environ); e.update({k: str(v) for k, v in env.items()})
    out = subprocess.run(cmd, env=e, capture_output=True, text=True,
                         timeout=900)
    m = RECEIPT.search(out.stdout + out.stderr)
    return m.group(0) if m else None


@pytest.mark.skipif(not (GEM5_PR.exists() and GRAPH.exists()),
                    reason="gem5 pr binary or graph fixture missing")
@pytest.mark.parametrize("stamps,variable,expect_bytes",
                         [(1, False, 4), (2, False, 8), (2, True, 4)])
def test_cache_sim_and_gem5_derive_identical_width(stamps, variable,
                                                   expect_bytes):
    """The whole point of the shared implementation: no backend may compute its own width.

    Both simulators independently call ecg_metadata::configure and print a
    receipt. Identical configuration must produce byte-identical receipts. A
    mismatch means one backend has drifted back to a private width rule, which
    is exactly the defect that made the two- versus single-epoch comparison a
    comparison of record widths.

    The variable-width two-epoch ReusePlan case is the one that matters most: the shared implementation
    computes a 4-byte BUDGET, and a backend that materialises the record wider
    must declare the container it really streams. gem5 and Sniper both printed
    the budget while building 64-bit arrays, so the receipt agreed while the
    memory traffic did not.
    """
    shared = {
        "ECG_EDGE_MASK_EPOCH": 1, "ECG_EDGE_MASK_LINEMIN": 1,
        "ECG_EDGE_MASK_EPOCHS": 32,
    }
    if stamps == 2:
        shared["ECG_REUSE_PLAN_DEPTH"] = 2
    if variable:
        shared["ECG_RECORD_VARIABLE_WIDTH"] = 1

    cs_env = dict(shared)
    cs_env.update({
        "ECG_MODE": "ECG_GRASP_POPT", "ECG_EDGE_MASKS": 1,
        "ECG_EDGE_MASK_LEAN": 1, "ECG_EDGE_MASK_PACK": 1,
        "ECG_EXACT_REREF": 1, "ECG_PREFETCH_MODE": 6,
        "OMP_NUM_THREADS": 1, "CACHE_ULTRAFAST": 0,
        "CACHE_POLICY": "ECG", "CACHE_L3_SIZE": 131072,
    })
    cs = _receipt(
        ["/usr/bin/setarch", "x86_64", "-R", str(PR),
         "-f", str(GRAPH), "-o", "5", "-n", "1", "-i", "1"], cs_env)

    g5_env = dict(shared)
    g5_env.update({"GEM5_ENABLE_ECG_PFX_HINTS": 1, "GEM5_ECG_PFX_MODE": 6})
    g5 = _receipt([str(GEM5_PR), "-f", str(GRAPH), "-n", "1", "-i", "1"],
                  g5_env)

    assert cs is not None, "cache_sim emitted no metadata receipt"
    assert g5 is not None, "gem5 emitted no metadata receipt"
    assert cs == g5, (
        f"backends disagree on record width at stamps={stamps} "
        f"variable={variable}:\n  cache_sim: {cs}\n  gem5     : {g5}")
    assert f"record_bytes={expect_bytes} " in cs + " ", (
        f"expected a {expect_bytes}-byte record at stamps={stamps} "
        f"variable={variable}, got: {cs}")

    # Sniper's workload is a third independent consumer of the same header.
    sniper = ROOT / "bench/bin_sniper/sg_kernel"
    if sniper.exists():
        sn_env = dict(shared)
        sn_env["SNIPER_ENABLE_ECG_EXTRACT"] = 1
        sn = _receipt(
            [str(sniper), "--benchmark", "pr", "-f", str(GRAPH), "-i", "1"],
            sn_env)
        assert sn is not None, "Sniper emitted no metadata receipt"
        assert sn == cs, (
            f"Sniper disagrees on record width at stamps={stamps} "
            f"variable={variable}:\n  cache_sim: {cs}\n  sniper   : {sn}")


def test_declared_gem5_timing_stages_are_honestly_scoped():
    """The gem5 width contrast must vary width and nothing else.

    gem5 once built pvector<uint64_t> unconditionally, so both arms of a
    4-versus-8-byte contrast would have streamed 8 bytes and the comparison
    would have been vacuous. It now has a compact 32-bit two-epoch ReusePlan record, so
    the contrast is real -- but only if the 4-byte arm actually asks for a
    computed width and the 8-byte arm actually forces one.

    Guards the two mistakes already made: forcing a width in both arms
    (vacuous), and nesting the explicit-cell channel inside itself (silently
    dropped, since experiment_run already wraps the stage env).
    """
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stages = [s for s in manifest["stages"]
              if str(s.get("name", "")).startswith("31_gem5_record_width")]
    assert stages, "the declared gem5 timing stages are missing"

    for stage in stages:
        env = stage.get("env", {})
        assert "GRAPHBREW_EXPLICIT_CELL_ENV" not in env, (
            f"{stage['name']} nests the explicit channel inside itself; "
            "experiment_run already wraps the stage env, so this double-encodes")
        assert env.get("ECG_RECORD_VARIABLE_WIDTH") == "1", (
            f"{stage['name']} must request variable width so the receipt "
            "reports a computed width rather than a hardcoded default")
        if stage["name"].endswith("_8b"):
            assert env.get("ECG_EDGE_RECORD_BYTES") == "8", (
                "the 8-byte arm must force its width, or both arms measure the "
                "same thing")
        else:
            assert "ECG_EDGE_RECORD_BYTES" not in env, (
                f"{stage['name']} forces a width, so it is not the compact arm")
        assert int(stage.get("ecg_epochs", 0)) <= 4096, (
            f"{stage['name']} uses too many epochs for the record to pack")


def test_gem5_forwards_metadata_knobs_into_the_simulated_guest():
    """gem5 SE mode does not inherit the host environment.

    graph_se.py builds an explicit allowlist of variables to hand the simulated
    process. The shared metadata implementation knobs were absent from it, so a stage asking for
    a 4-byte record silently got the two-epoch ReusePlan default of 8: the run looked
    correct at every layer above, and only the guest's own receipt disagreed.

    This is the third distinct layer of env plumbing between a manifest stage
    and the guest, after roi_matrix's scrub and experiment_run's explicit-cell
    channel, and the only one that is invisible from the host side.
    """
    config = (ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py").read_text()
    required = [
        "ECG_RECORD_VARIABLE_WIDTH",
        "ECG_EDGE_RECORD_BYTES",
        "ECG_DELIVERY",
        "ECG_SIDECAR_PAYLOAD_BITS",
        "ECG_RECORD_TIER_BITS",
        "ECG_VIRTUAL_ID_BITS",
        # The enforcement knob was omitted from this list for its entire
        # existence, so the guest could never abort on a width mismatch and the
        # guard silently degraded to a printed receipt on gem5.
        "ECG_EXPECT_BYTES_PER_EDGE",
    ]
    for name in required:
        assert f'"{name}"' in config, (
            f"graph_se.py does not forward {name} to the simulated guest, so "
            "gem5 cells cannot honour it and will silently use the default")

    # Every knob the shared metadata implementation reads must be forwarded, or a future knob
    # repeats the same failure. Derive the list from the shared implementation itself.
    shared_text = (ROOT / "bench/include/ecg_metadata.h").read_text()
    knobs = set(re.findall(r'"(ECG_[A-Z0-9_]+)"', shared_text))
    # Guest-side mechanism knobs live in gem5_harness.h and are GEM5_ECG_*
    # prefixed, so the shared implementation regex alone cannot see them. They need forwarding
    # for exactly the same reason, and were being hand-maintained.
    harness = (ROOT / "bench/include/gem5_sim/gem5_harness.h").read_text()
    knobs |= set(re.findall(r'getenv\("(GEM5_ECG_[A-Z0-9_]+)"\)', harness))
    # graph_se.py forwards through TWO mechanisms: an f-string block that
    # always emits a value, and a pass-through allowlist that forwards only when
    # the host sets one. Requiring the quoted form would flag knobs handled by
    # the first mechanism, so accept a mention by either.
    missing = sorted(k for k in knobs if k not in config)
    assert not missing, (
        f"the guest reads {missing} but graph_se.py does not forward "
        "them into the gem5 guest, so those knobs are silently inert there")


def test_compact_two_stamp_record_packs_and_round_trips():
    """The 32-bit two-stamp format must be exact, and honest about its limits.

    gem5 and Sniper previously had only a 64-bit two-epoch ReusePlan record, so they
    streamed 8 bytes per edge and DOUBLED the structural stream against a 4-byte
    CSR edge, while cache_sim modelled the record as substituting for that edge.
    That produced a direction reversal between simulators: 0.557 against LRU in
    cache_sim versus 1.189 in gem5 at identical geometry.

    The compact format closes it, but only where the fields genuinely fit.
    """
    header = (ROOT / "bench/include/ecg_reuse_plan_builder.h").read_text()
    for fn in ("canPackReusePlan32", "packReusePlanRecord32",
               "extractReusePlan32Dest", "extractReusePlan32Tier",
               "extractReusePlan32First", "extractReusePlan32Second",
               "widenReusePlan32", "buildInEdgeReusePlanRecords32"):
        assert fn in header, f"compact two-epoch ReusePlan helper {fn} is missing"

    # The compact builder must reuse the SAME epoch computation as the 64-bit
    # one, or the two widths would mean different policies.
    start = header.index("bool buildInEdgeReusePlanRecords32")
    body = header[start:start + 3000]
    assert "nextReusePlanForLine" in body, (
        "the compact builder computes epochs its own way, so a width change "
        "would silently change the policy")

    # And it must refuse rather than truncate when the fields do not fit.
    assert "if (!canPackReusePlan32(n, ne)) return false;" in body, (
        "the compact builder does not check feasibility, so it could silently "
        "truncate destinations or epochs")


def test_gem5_prefers_the_compact_record_and_declares_its_width():
    src = (ROOT / "bench/src_gem5/pr.cc").read_text()
    assert "buildInEdgeReusePlanRecords32" in src, (
        "gem5 does not try the compact record, so it always streams 8 bytes")
    assert "widenReusePlan32" in src, (
        "gem5 does not widen the compact record for the ISA helpers")
    assert "declareContainerBytes" in src, (
        "gem5 does not declare the container it actually streams, so its "
        "receipt can claim a width the guest does not deliver")
    assert "canPackReusePlan32" in src, (
        "gem5 declares a fixed container instead of the one feasibility allows")


def test_riscv_gem5_binaries_are_not_stale_against_the_compact_record():
    """gem5 runs the RISC-V kernels, not the native ones.

    experiment_run passes --no-build, so a rebuilt native binary proves nothing about
    what gem5 executes. This was measured the hard way: the receipt from the
    native binary read 4 bytes while the RISC-V guest still printed the 8-byte
    banner, and the timing arm silently reproduced the 8-byte result.
    """
    riscv = ROOT / "bench/bin_gem5/pr_riscv_m5ops"
    if not riscv.exists():
        pytest.skip("RISC-V gem5 kernel not built")
    blob = riscv.read_bytes()
    assert b"COMPACT record ON" in blob, (
        "the RISC-V gem5 kernel predates the compact two-stamp record, so "
        "gem5 cells will stream 8 bytes whatever the receipt claims; rebuild "
        "with make gem5-riscv-m5ops-pr")
    # The compact ISA path emits an unknown opcode on a gem5 that predates it,
    # which faults inside the guest rather than reporting a configuration
    # error. Binary and simulator must move together.
    guest_has_isa = b"ECG_EXTRACT2C" in blob
    # Compare against the BUILT simulator. Comparing against decoder.isa would
    # pass whenever the source has been edited but gem5 not rebuilt, which is
    # precisely the staleness this test exists to catch.
    gem5_opt = ROOT / "bench/include/gem5_sim/gem5/build/RISCV/gem5.opt"
    if not gem5_opt.exists():
        pytest.skip("RISCV gem5 not built")
    sim_has_isa = b"ecg_extract2c" in gem5_opt.read_bytes()
    assert guest_has_isa == sim_has_isa, (
        "the compact-decode instruction is present in "
        f"{'the guest binary' if guest_has_isa else 'the gem5 decoder'} but "
        f"not {'the gem5 decoder' if guest_has_isa else 'the guest binary'}; "
        "rebuild both (make gem5-riscv-m5ops-pr and the RISCV gem5 build) or "
        "neither, otherwise GEM5_ECG_COMPACT_ISA=1 traps on an unknown opcode")
    guest_has_proposal = b"ECG_REUSE_BIND_LOAD_C_FLOW" in blob
    sim_has_proposal = b"ecg_flow_load_compact" in gem5_opt.read_bytes()
    assert guest_has_proposal == sim_has_proposal, (
        "the compact FlowThrough record-load proposal is present in "
        f"{'the guest binary' if guest_has_proposal else 'the gem5 decoder'} "
        "but not both; rebuild gem5 and the RISC-V guest together")


def test_guest_enforces_the_width_the_runner_intended():
    """Receipts must be enforced, not merely printed.

    Four independent layers of env plumbing silently defeated the same setting
    during this work, and each was invisible except in the guest's own receipt.
    A receipt only helps if something reads it, so the guest now aborts before
    the ROI when what it derived disagrees with ECG_EXPECT_BYTES_PER_EDGE.
    """
    meta = (ROOT / "bench/include/ecg_metadata.h").read_text()
    assert "enforceExpectedBytesPerEdge" in meta
    assert "ECG_EXPECT_BYTES_PER_EDGE" in meta
    assert "std::abort()" in meta, (
        "a mismatch must abort; warning is what let four separate "
        "misconfigurations reach a result")

    # Every kernel on every backend must call it, or the gap is where the next
    # silent misconfiguration lands.
    for rel in ([f"bench/src_sim/{k}.cc" for k in ("pr", "bfs", "cc", "bc", "sssp")] +
                [f"bench/src_gem5/{k}.cc" for k in ("pr", "bfs", "cc", "bc", "sssp")] +
                ["bench/src_sniper/sg_kernel.cc"]):
        src = (ROOT / rel).read_text()
        assert "enforceExpectedBytesPerEdge" in src, (
            f"{rel} prints a receipt but does not enforce it")


def test_width_contrast_stages_are_scoped_to_what_gem5_implements():
    """Only gem5 PR has a compact record, and FlowThrough is 8-byte only."""
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stages = [s for s in manifest["stages"]
              if str(s.get("name", "")).startswith("31_gem5_record_width")]
    assert stages
    for stage in stages:
        assert stage.get("benchmarks") == ["pr"], (
            f"{stage['name']} includes kernels with no compact path, whose "
            "receipts would claim a width they do not stream")
        assert not any("FLOWTHROUGH" in p.upper() for p in stage["policies"]), (
            f"{stage['name']} includes a FlowThrough policy; the stream-load "
            "instruction is 8-byte only, so the arm would change width AND "
            "allocation together")
        want = "8" if stage["name"].endswith("_8b") else "4"
        assert stage["env"].get("ECG_EXPECT_BYTES_PER_EDGE") == want, (
            f"{stage['name']} does not assert the width it claims")


def test_compact_records_decode_identically_to_the_64_bit_form():
    """Per-record proof, not an output hash.

    Semantic transparency was first argued from identical top-20 PageRank
    scores, which is weak: replacement metadata cannot change PageRank values
    even if every epoch is wrong. This compares every compact record against the
    64-bit record built from the same graph, field by field, across several
    epoch counts.
    """
    binary = ROOT / "bench/bin_sim/test_ecg_reuse_plan32"
    graph = ROOT / "results/graphs/web-Google-n16/web-Google-n16.sg"
    if not (binary.exists() and graph.exists()):
        pytest.skip("ReusePlan equivalence harness or graph fixture missing")
    env = dict(os.environ, OMP_NUM_THREADS="4")
    proc = subprocess.run([str(binary), "-f", str(graph)],
                          env=env, capture_output=True, text=True, timeout=900)
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, f"compact/64-bit records diverge:\n{out[-2000:]}"
    assert "ALL EQUIVALENT" in out, out[-2000:]
    # Guard against a vacuous pass if the builder silently refused every size.
    assert out.count("records checked") >= 3, (
        "too few epoch counts exercised; the compact builder may be refusing "
        f"to pack:\n{out[-1000:]}")


def test_reuse_plan_sidecar_is_deterministic_and_fail_closed(tmp_path):
    tool = ROOT / "bench/bin_sim/reuse_plan_sidecar"
    if not tool.exists():
        pytest.skip("ReusePlan sidecar generator not built")
    first = tmp_path / "first.bin"
    second = tmp_path / "second.bin"
    base_env = {
        **os.environ,
        "ECG_REUSE_PLAN_SIDECAR_RECORD_BYTES": "4",
        "ECG_REUSE_PLAN_SIDECAR_EPOCHS": "16",
        "ECG_REUSE_PLAN_SIDECAR_VPL": "16",
        "ECG_REUSE_PLAN_SIDECAR_LINEMIN": "1",
        "ECG_REUSE_PLAN_SIDECAR_PUSH": "0",
        "OMP_NUM_THREADS": "4",
    }
    command = [
        str(tool), "-g", "10", "-k", "4",
        "-o", "5", "-n", "1", "-i", "1",
    ]
    for output in (first, second):
        env = {
            **base_env,
            "ECG_REUSE_PLAN_SIDECAR": str(output),
        }
        result = subprocess.run(
            command, env=env, cwd=ROOT,
            capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "[ReusePlan-SIDECAR-OK" in result.stdout
    assert first.read_bytes() == second.read_bytes()

    verify_env = {
        **base_env,
        "ECG_REUSE_PLAN_SIDECAR": str(first),
        "ECG_REUSE_PLAN_SIDECAR_VERIFY_ONLY": "1",
    }
    verified = subprocess.run(
        command, env=verify_env, cwd=ROOT,
        capture_output=True, text=True, timeout=300)
    assert verified.returncode == 0, verified.stdout + verified.stderr

    corrupted = bytearray(first.read_bytes())
    corrupted[-1] ^= 0x01
    first.write_bytes(corrupted)
    rejected = subprocess.run(
        command, env=verify_env, cwd=ROOT,
        capture_output=True, text=True, timeout=300)
    assert rejected.returncode != 0
    assert "payload hash mismatch" in (
        rejected.stdout + rejected.stderr)


@pytest.mark.skipif(not (GEM5_PR.exists() and GRAPH.exists()),
                    reason="gem5 pr binary or graph fixture missing")
def test_a_four_byte_receipt_means_a_four_byte_array_was_built():
    """A receipt that agrees across backends can still be wrong in all of them.

    The shared source computes the width a record COULD occupy. gem5 and Sniper both
    printed that budget while building 64-bit arrays, so the cross-backend
    receipt comparison passed while the memory traffic silently doubled. Only
    the container the backend actually allocates settles it.

    Both backends announce the compact array when they build it, so a 4-byte
    receipt without that announcement is the exact defect this guards.
    """
    env = {"ECG_EDGE_MASK_EPOCH": 1, "ECG_EDGE_MASK_LINEMIN": 1,
           "ECG_EDGE_MASK_EPOCHS": 32, "ECG_REUSE_PLAN_DEPTH": 2,
           "ECG_RECORD_VARIABLE_WIDTH": 1}

    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    e.update({"GEM5_ENABLE_ECG_PFX_HINTS": "1", "GEM5_ECG_PFX_MODE": "6"})
    g5 = subprocess.run([str(GEM5_PR), "-f", str(GRAPH), "-n", "1", "-i", "1"],
                        env=e, capture_output=True, text=True, timeout=900)
    g5_out = g5.stdout + g5.stderr
    assert "record_bytes=4 " in g5_out, "gem5 did not take the 4-byte budget"
    assert "two-epoch ReusePlan COMPACT record ON" in g5_out, (
        "gem5 announced a 4-byte record but did not build the compact array; "
        "it is streaming 8 bytes per edge while claiming 4")

    sniper = ROOT / "bench/bin_sniper/sg_kernel"
    if not sniper.exists():
        pytest.skip("Sniper workload not built")
    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    e["SNIPER_ENABLE_ECG_EXTRACT"] = "1"
    sn = subprocess.run(
        [str(sniper), "--benchmark", "pr", "-f", str(GRAPH), "-i", "1"],
        env=e, capture_output=True, text=True, timeout=900)
    sn_out = sn.stdout + sn.stderr
    assert "record_bytes=4 " in sn_out, "Sniper did not take the 4-byte budget"
    assert "ECG-PAIR32 sim=sniper" in sn_out, (
        "Sniper announced a 4-byte record but did not build the compact array; "
        "it is streaming 8 bytes per edge while claiming 4")


def test_the_compact_format_has_one_definition_in_three_places():
    """dest[id_bits] | tier[2] | first[eb] | second[eb], transcribed once too often.

    The compact record is now decoded by the builder's own helpers, by
    widenReusePlan32 in the guest, and by ecg_extract2c in the gem5 decoder.
    The first two are proven equal per record by test_ecg_reuse_plan32; the
    decoder is a third transcription that no unit test can reach, so guard its
    shifts against the layout they are supposed to implement.

    A drifting decoder would not crash. It would deliver plausible-looking
    epochs and quietly change every eviction decision.
    """
    decoder = (ROOT / "bench/include/gem5_sim/gem5/src/arch/riscv/isa"
               / "decoder.isa").read_text()
    start = decoder.index("0x02: ecg_extract2c")
    body = decoder[start:start + 2600]
    # Same field order and offsets as packReusePlanRecord32.
    assert "record & id_mask" in body, "dest must occupy the low id_bits"
    assert "(record >> id_bits) & 0x3U" in body, "tier sits directly above dest"
    assert "(record >> (id_bits + 2)) & ep_mask" in body, (
        "the first stamp sits above the 2 tier bits")
    assert "(record >> (id_bits + 2 + epoch_bits)) & ep_mask" in body, (
        "the second stamp sits above the first")
    # Must deliver through the same path as the 64-bit instruction, or the two
    # widths would mean different policies.
    assert "setDecodedEcgExtractHint2" in body
    assert "storeEcgMetadataByVertex" in body

    builder = (ROOT / "bench/include/ecg_reuse_plan_builder.h").read_text()
    assert "(static_cast<uint32_t>(tier & 0x3u) << id_bits)" in builder, (
        "the packer's layout changed; the gem5 decoder still implements the "
        "old one and will deliver wrong epochs without failing")


def test_built_kernels_are_newer_than_the_headers_they_embed():
    """make reported success while leaving every binary stale.

    The gem5/Sniper/cache_sim build rules listed gapbs, graphbrew and external
    headers as prerequisites but not the ECG headers, so editing
    ecg_metadata.h or gem5_harness.h did not rebuild anything and make printed
    "Built ...". A traced run then showed the guest silently missing the change
    under test while the simulator had it.

    Timestamps are the only thing that can catch this, because the stale binary
    is otherwise perfectly valid.
    """
    headers = sorted(
        list((ROOT / "bench/include").glob("ecg_*.h"))
        + list((ROOT / "bench/include/gem5_sim").glob("*.h")))
    if not headers:
        pytest.skip("ECG headers not found")
    changed = set(subprocess.run(
        ["git", "diff", "--name-only"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.splitlines())
    changed.update(subprocess.run(
        ["git", "diff", "--cached", "--name-only"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.splitlines())
    material_headers = []
    for header in headers:
        relative = str(header.relative_to(ROOT))
        if relative not in changed:
            continue
        baseline = subprocess.run(
            ["git", "show", f"HEAD:{relative}"],
            cwd=ROOT, capture_output=True, text=True)
        if baseline.returncode != 0:
            material_headers.append(header)
            continue
        diff = difflib.unified_diff(
            baseline.stdout.splitlines(),
            header.read_text(errors="ignore").splitlines(),
        )
        if any(
                line.startswith(("+", "-")) and
                not line.startswith(("+++", "---")) and
                not line[1:].lstrip().startswith(("//", "/*", "*", "#"))
                for line in diff):
            material_headers.append(header)
    if not material_headers:
        pytest.skip("no edited ECG header requires a rebuild")
    newest = max(h.stat().st_mtime for h in material_headers)
    newest_name = max(
        material_headers, key=lambda h: h.stat().st_mtime).name

    stale, present = [], 0
    for binary in (ROOT / "bench/bin_gem5" / "pr_riscv_m5ops",
                   ROOT / "bench/bin_gem5" / "pr",
                   ROOT / "bench/bin_sniper" / "sg_kernel"):
        if not binary.exists():
            continue
        present += 1
        if binary.stat().st_mtime < newest:
            stale.append(binary.name)
    if present == 0:
        pytest.skip("no measurement binaries built")
    assert not stale, (
        f"{stale} predate {newest_name}; the build rules now list the ECG "
        "headers as prerequisites, so rebuild rather than trusting a binary "
        "that cannot contain the change being measured")


def test_row_cannot_contradict_itself_about_stream_width():
    """A row said 4 bytes per record and 8 bytes per edge at the same time.

    edge_stream_bytes_per_edge is derived from the record width and was computed
    from the NOMINAL width before the guest receipt corrected it, so every
    compact ReusePlan row asserted ecg_record_bytes=4, ecg_record_replaces_edge=1 and
    edge_stream_bytes_per_edge=8 together. That is the field a reader is most
    likely to trust when computing bytes per edge, so the contradiction is worse
    than a missing column.
    """
    runner = (ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()
    i = runner.index('base["ecg_receipt_bytes_per_edge"]')
    window = runner[i:i + 1400]
    assert "edge_stream_bytes_per_edge" in window, (
        "the derived stream width must be recomputed where the receipt "
        "corrects the record width, or the two disagree")
    assert "ecg_record_replaces_edge" in window
