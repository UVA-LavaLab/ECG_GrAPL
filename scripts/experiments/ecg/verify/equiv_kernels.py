#!/usr/bin/env python3
"""Multi-kernel 3-simulator K2 mechanism conformance + full debug.

The eviction DECISION (`ecg_victim_policy.h`) is kernel-AGNOSTIC and byte-identical across
cache_sim / gem5 / Sniper, so the ECG policy must obey the same eviction spec for EVERY
kernel in EVERY simulator — not just PageRank. This runs PR/BFS/SSSP/BC/CC on
each simulator with the eviction trace on, asserts every L3
eviction obeys the policy spec (reusing `verify_ecg.py`'s `verify_trace`), AND captures the
per-sim `[ECG-CONFIG …]` banner (full debug: each run proves the policy/mode/variant it ran).

This certifies DECISION and Schedule-2 delivery equivalence across kernels.
PR consumes IN-edge records; BFS/SSSP/BC consume transpose-correct OUT-edge
records; CC follows OUT-edge records under its undirected/symmetric contract.

Usage:
  python3 scripts/experiments/ecg/verify/equiv_kernels.py                 # cache_sim only (fast)
  python3 scripts/experiments/ecg/verify/equiv_kernels.py --gem5 --sniper # full 3-sim (slow)
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ecg  # noqa: E402  (reuse verify_trace, BASE_ENV, ECG_ENV, COV_ENV, GRAPH, GEM5_OPT, ROI_MATRIX, ROOT)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from roi_matrix import cache_sim_ecg_epoch_region_indices  # noqa: E402

BANNER_RE = re.compile(r"\[ECG-CONFIG[^\]]*\]")
# kernel -> simulators that can run it on the UNWEIGHTED eval graph (available binaries).
# SSSP runs on the unweighted email-Eu-core.sg via GAPBS WeightedBuilder (it deterministically
# synthesizes edge weights — no .wsg needed) and, at the cc small-cache geometry (L2 1kB/L3 2kB),
# its dist[] spills to the L3 so the epoch eviction is exercised (banner + nonzero stamped epochs).
# BC, CC, and SSSP have Sniper sg_kernel targets. WeightedBuilder deterministically
# synthesizes SSSP weights from the unweighted fixture, so all five property kernels
# are certified on all three simulators.
# tc (triangle counting) is intentionally EXCLUDED: it is a pure set-intersection over CSR neighbour
# lists with NO vertex-indexed property array (gem5 tc registers 0 property regions), so ECG's
# per-edge epoch has nothing to stamp and is outside the mechanism's scope.
KERNEL_SIMS = {
    "pr":   ["cache_sim", "gem5", "sniper"],
    "bfs":  ["cache_sim", "gem5", "sniper"],
    "bc":   ["cache_sim", "gem5", "sniper"],
    "cc":   ["cache_sim", "gem5", "sniper"],
    "sssp": ["cache_sim", "gem5", "sniper"],
}
# Reuse-sensitive kernels that must decisively exercise epoch eviction (epoch distance
# strictly decides >=1 victim) on EVERY sim. With the faithful per-edge OUT-direction masks delivered
# (ECG_EDGE_MASKS=1 on the cache_sim out-traversal legs; the gem5 ecg.load EVICT path already delivers
# them), BFS/BC/SSSP decisively exercise epoch ordering. PR is delivery+policy
# coverage: after region isolation, the bounded tiny cell is record-first/do-no-harm.
# cc is the sole DO-NO-HARM cell: undirected union-find with low property reuse and no OUT-edge-mask
# consumption in the cache_sim kernel -> decisive=0 on BOTH sims (epoch delivered + policy-compliant,
# but effective-distance ties dominate, consistent with ECG ~= GRASP on that access pattern).
EXPECTED_DECISIVE = {"bfs", "bc", "sssp"}
EXPECTED_EPOCH_REGIONS = {
    "pr": "contrib",
    "bfs": "parent",
    "sssp": "dist",
    "bc": "depth,path_counts",
    "cc": "comp",
}
GEM5_X86 = ecg.ROOT / "bench" / "include" / "gem5_sim" / "gem5" / "build" / "X86" / "gem5.opt"
GEM5_RISCV = ecg.ROOT / "bench" / "include" / "gem5_sim" / "gem5" / "build" / "RISCV" / "gem5.opt"
# Kernels whose gem5 leg runs on RISC-V via the validated fused ecg.load EVICT delivery
# (GEM5_FORCE_ECG_PLOAD). All ship a *_riscv_m5ops binary with real epoch delivery
# (pr: contrib; bfs: parent; bc: depth+path_counts; cc: comp; sssp: dist), so no equiv cell depends on the
# X86 fat-mask (BFS/SSSP/BC/CC have no X86 epoch delivery).
GEM5_RISCV_KERNELS = {"pr", "bfs", "bc", "cc", "sssp"}

# Optional cross-sim stream-prefetcher degree (--stream-prefetch-degree). 0 = off
# (the byte-identical-decisive baseline). >0 turns on each sim's native structure
# prefetcher (cache_sim next-line via CACHE_STREAM_PREFETCH_DEGREE; gem5 stride via
# roi_matrix --prefetcher STRIDE) so the run proves the policy spec still holds with
# the realistic prefetcher. The prefetchers are NOT algorithm-identical, so under
# prefetch the equivalence is SPEC-level (every eviction obeys the ECG spec), not
# byte-identical counts.
STREAM_PF_DEGREE = 0
SCHEDULE_K = 0
STREAM_BYPASS = False
ADAPTIVE_STREAM_BYPASS = False
GEM5_ISA_VARIANT = "indexed"
RUN_META = {}


def effective_variant(kernel):
    if SCHEDULE_K == 2:
        if kernel == "pr":
            return "epoch_first"
        if kernel in ("bfs", "sssp"):
            return "degree_first"
        return "rrip_first"
    return "rrip_first"


def expected_trace_policy(kernel):
    return f"ECG:{effective_variant(kernel)}"


def detailed_row_provenance(sim, kernel):
    row = RUN_META.get((sim, kernel), {}).get("row", {})
    checks = {
        "epoch_regions": (
            row.get("ecg_epoch_regions") ==
            EXPECTED_EPOCH_REGIONS[kernel]),
        "variant": (
            row.get("ecg_variant_effective") ==
            effective_variant(kernel)),
        "schedule_k": row.get("ecg_schedule_k") == str(SCHEDULE_K),
    }
    if GEM5_ISA_VARIANT == "mask":
        checks["isa_variant"] = row.get("ecg_isa_variant") == "mask"
    return all(checks.values()), checks


def sniper_policy():
    if SCHEDULE_K == 2 and ADAPTIVE_STREAM_BYPASS:
        return "ECG:K2_ADAPTIVE_STREAMSHIELD"
    if SCHEDULE_K == 2 and STREAM_BYPASS:
        return "ECG:K2_STREAMSHIELD"
    if SCHEDULE_K == 2:
        return "ECG:K2"
    return "ECG:ECG_GRASP_POPT"


def sniper_policy_label():
    return sniper_policy().replace(":", "_")


def _banner(text):
    m = BANNER_RE.search(text or "")
    return m.group(0) if m else "(no banner)"


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(path, relative_to=None):
    path = Path(path)
    resolved = path.resolve()
    recorded_path = resolved
    if relative_to is not None:
        try:
            recorded_path = resolved.relative_to(Path(relative_to).resolve())
        except ValueError:
            pass
    return {
        "path": str(recorded_path),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def write_json(path, payload):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n")
    temporary.replace(path)


def git_text(*args):
    result = subprocess.run(
        ["git", *args], cwd=ecg.ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def decoded_timeout_output(value):
    if value is None:
        return ""
    return value.decode(errors="replace") if isinstance(value, bytes) else value


def validate_roi_output(out, expected_policy):
    out = Path(out)
    csv_path = out / "roi_matrix.csv"
    json_path = out / "roi_matrix.json"
    marker_path = out / "roi_matrix.complete.json"
    errors = []
    rows = []
    if not csv_path.exists():
        errors.append("roi_matrix.csv missing")
    else:
        rows = list(csv.DictReader(csv_path.open()))
        if len(rows) != 1:
            errors.append(f"expected one ROI row, found {len(rows)}")
        elif rows[0].get("status") != "ok":
            errors.append(
                f"ROI row status is {rows[0].get('status')!r}")
        elif rows[0].get("policy_label") != expected_policy:
            errors.append(
                f"policy={rows[0].get('policy_label')!r}, "
                f"expected={expected_policy!r}")
    if not json_path.exists():
        errors.append("roi_matrix.json missing")
    if not marker_path.exists():
        errors.append("roi_matrix.complete.json missing")
    else:
        try:
            marker = json.loads(marker_path.read_text())
        except json.JSONDecodeError as exc:
            errors.append(f"completion marker is invalid JSON: {exc}")
        else:
            if marker.get("complete") is not True:
                errors.append("completion marker complete is not true")
            if marker.get("all_rows_ok") is not True:
                errors.append("completion marker all_rows_ok is not true")
            outputs = marker.get("outputs", {})
            for name, path in (
                    ("roi_matrix.csv", csv_path),
                    ("roi_matrix.json", json_path)):
                descriptor = outputs.get(name, {})
                if path.exists():
                    if descriptor.get("sha256") != sha256_file(path):
                        errors.append(f"{name} completion hash mismatch")
                    expected_rows = len(
                        rows if name.endswith(".csv")
                        else json.loads(path.read_text()))
                    if descriptor.get("rows") != expected_rows:
                        errors.append(f"{name} completion row mismatch")
    return errors, rows[0] if len(rows) == 1 else {}


def evidence_inputs(
        kernels, include_gem5, include_sniper, evidence_dir=None):
    paths = {
        "graph": ecg.GRAPH,
        "equiv_verifier": Path(__file__).resolve(),
        "trace_oracle": Path(ecg.__file__).resolve(),
        "roi_matrix": ecg.ROI_MATRIX,
        "policy_source": ecg.ROOT / "bench/include/ecg_victim_policy.h",
        "epoch_builder": ecg.ROOT / "bench/include/ecg_epoch_builder.h",
    }
    for kernel in kernels:
        paths[f"cache_sim_{kernel}"] = (
            ecg.ROOT / "bench/bin_sim" / kernel)
    if include_gem5:
        paths["gem5_riscv"] = GEM5_RISCV
        paths["gem5_config"] = (
            ecg.ROOT /
            "bench/include/gem5_sim/configs/graphbrew/graph_se.py")
        paths["gem5_ecg_policy"] = (
            ecg.ROOT /
            "bench/include/gem5_sim/overlays/mem/cache/"
            "replacement_policies/ecg_rp.cc")
        paths["gem5_policy_copy"] = (
            ecg.ROOT /
            "bench/include/gem5_sim/overlays/mem/cache/"
            "replacement_policies/ecg_victim_policy.hh")
        paths["gem5_patch_state"] = (
            ecg.ROOT / "bench/include/gem5_sim/.gem5_patch_state.json")
        paths["gem5_decoder"] = (
            ecg.ROOT /
            "bench/include/gem5_sim/overlays/arch/riscv/isa/"
            "decoder_ecg_extract.isa")
        for kernel in kernels:
            paths[f"gem5_guest_{kernel}"] = (
                ecg.ROOT / "bench/bin_gem5" /
                f"{kernel}_riscv_m5ops")
    if include_sniper:
        paths["sniper_binary"] = (
            ecg.ROOT / "bench/include/sniper_sim/snipersim/lib/sniper")
        paths["sniper_workload"] = (
            ecg.ROOT / "bench/bin_sniper/sg_kernel")
        paths["sniper_overlay_status"] = (
            ecg.ROOT / "bench/include/sniper_sim/.sniper_overlays.json")
        paths["sniper_ecg_policy"] = (
            ecg.ROOT /
            "bench/include/sniper_sim/overlays/common/core/"
            "memory_subsystem/cache/cache_set_ecg.cc")
        paths["sniper_policy_copy"] = (
            ecg.ROOT /
            "bench/include/sniper_sim/overlays/common/core/"
            "memory_subsystem/cache/ecg_victim_policy.h")
    missing = [str(path) for path in paths.values() if not path.exists()]
    if missing:
        raise RuntimeError(
            "evidence inputs are missing: " + ", ".join(missing))
    records = {}
    for name, path in sorted(paths.items()):
        if evidence_dir is not None and name in (
                "sniper_binary", "sniper_workload"):
            destination = Path(evidence_dir) / "inputs" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            records[name] = file_record(destination, evidence_dir)
        else:
            records[name] = file_record(path)
    return records


def archive_cell(
        evidence_dir, sim, kernel, text, banner, coverage,
        status, expected_policy, metadata):
    cell_dir = Path(evidence_dir) / "cells" / kernel / sim
    cell_dir.mkdir(parents=True, exist_ok=True)
    raw_path = cell_dir / "raw.log"
    raw_path.write_text(text)
    outputs = {
        "raw_log": file_record(raw_path, evidence_dir),
    }
    output_dir = metadata.get("output_dir")
    if output_dir:
        for name in (
                "roi_matrix.csv", "roi_matrix.json",
                "roi_matrix.complete.json"):
            source = Path(output_dir) / name
            if source.exists():
                destination = cell_dir / name
                shutil.copy2(source, destination)
                outputs[name] = file_record(destination, evidence_dir)
    record = {
        "simulator": sim,
        "kernel": kernel,
        "status": status,
        "expected_policy": expected_policy,
        "banner": banner,
        "coverage": coverage,
        "runner": metadata,
        "outputs": outputs,
    }
    write_json(cell_dir / "cell.json", record)
    return record


def _stale(binp, kernel):
    """Return a [STALE] note if bin_sim/<kernel> predates the ECG headers/policy/kernel source it
    is built from (the cc/sssp banner trap: a binary built before a header change silently runs old
    logic). Empty string when fresh. Guards the equiv against a stale-binary false pass/fail."""
    if not binp.exists():
        return ""
    bmt = binp.stat().st_mtime
    inc = ecg.ROOT / "bench" / "include"
    deps = [inc / "cache_sim" / "cache_sim.h", inc / "ecg_victim_policy.h",
            inc / "ecg_epoch_builder.h", inc / "ecg_mode6_builder.h",
            inc / "cache_sim" / "graph_cache_context.h",
            ecg.ROOT / "bench" / "src_sim" / f"{kernel}.cc"]
    newer = [d.name for d in deps if d.exists() and d.stat().st_mtime > bmt]
    return f"  [STALE] bin_sim/{kernel} older than {', '.join(newer)} — rebuild (make bench/bin_sim/{kernel})\n" if newer else ""


def stale_dependencies(binary, dependencies):
    binary = Path(binary)
    if not binary.exists():
        return [f"binary missing: {binary}"]
    binary_mtime = binary.stat().st_mtime
    return [
        str(Path(path))
        for path in dependencies
        if Path(path).exists() and Path(path).stat().st_mtime > binary_mtime
    ]


def run_cache(kernel):
    """cache_sim <kernel> with ECG_GRASP_POPT + coverage geometry (force property eviction)."""
    binp = ecg.ROOT / "bench" / "bin_sim" / kernel
    if not binp.exists():
        RUN_META[("cache_sim", kernel)] = {
            "returncode": None,
            "errors": ["binary missing"],
        }
        return ("", False), "(binary missing)"
    stale = _stale(binp, kernel)
    if stale:
        sys.stderr.write(stale)
        RUN_META[("cache_sim", kernel)] = {
            "returncode": None,
            "errors": [stale.strip()],
        }
        return ("", False), "(stale binary)"
    env = {**os.environ, **ecg.BASE_ENV, **ecg.ECG_ENV, **ecg.COV_ENV,
           "ECG_VARIANT": effective_variant(kernel), "ECG_DEBUG": "1"}
    env["CACHE_ECG_EPOCH_REGION_INDICES"] = (
        cache_sim_ecg_epoch_region_indices(kernel))
    if SCHEDULE_K:
        env["ECG_EDGE_MASK_SCHED"] = str(SCHEDULE_K)
        env["ECG_K2_DELIVERY_TRACE"] = "32"
    if STREAM_BYPASS:
        env["ECG_STREAM_BYPASS"] = "1"
        if ADAPTIVE_STREAM_BYPASS:
            env["ECG_STREAM_BYPASS_ADAPTIVE"] = "1"
    if kernel in ("bfs", "bc", "cc", "sssp"):
        # Out-traversal kernels read property[dest] over out_neigh(u); deliver the FAITHFUL per-edge
        # OUT-direction next-ref masks (ECG_EDGE_MASKS=1, epoch = next in_neigh(dest) > u) — the same
        # direction the gem5 ecg.load EVICT leg delivers. This makes the epoch STRICTLY DECIDE victims
        # on cache_sim (sssp 5, bfs 1, bc 123), matching the already-decisive gem5 legs, so the cell
        # proves epoch-equivalence (not just delivery). (Default PR mode-6 in-edge env stays for pr.)
        env["ECG_EDGE_MASKS"] = "1"
    if kernel in ("bfs", "cc", "sssp"):
        # These one-pass property arrays fit the default 1MB COV L2 after the
        # policy-independent warm replay, so they would never reach L3. Shrink
        # L2+L3 below the property footprint to exercise delivered epoch eviction.
        # (cc has no OUT-edge-mask consumption -> stays do-no-harm.)
        env["CACHE_L2_SIZE"] = "1kB"
        env["CACHE_L3_SIZE"] = "2kB"
    if STREAM_PF_DEGREE > 0:
        env["CACHE_STREAM_PREFETCH_DEGREE"] = str(STREAM_PF_DEGREE)
    command = [
        str(binp), "-f", str(ecg.GRAPH), "-o", "0", "-n", "1"]
    try:
        p = subprocess.run(
            command, env=env, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired as exc:
        text = (
            decoded_timeout_output(exc.stdout) +
            decoded_timeout_output(exc.stderr))
        RUN_META[("cache_sim", kernel)] = {
            "command": command,
            "returncode": None,
            "errors": ["timeout after 300 seconds"],
        }
        return (text, False), _banner(text)
    RUN_META[("cache_sim", kernel)] = {
        "command": command,
        "returncode": p.returncode,
        "errors": [] if p.returncode == 0 else [
            f"cache_sim exited {p.returncode}"],
    }
    return (p.stderr, p.returncode == 0), _banner(p.stderr)


def _roi_log(out):
    logs = sorted((out / "logs").glob("*.log")) if (out / "logs").exists() else []
    text = logs[0].read_text(errors="ignore") if logs else ""
    # gem5 redirects benchmark stdout/stderr away from the simulator log. Append
    # them so K2 EXPECT records from the guest can be matched against RECV records
    # emitted by the decoder/backend.
    for path in sorted(out.rglob("benchmark_stderr.txt")):
        text += "\n" + path.read_text(errors="ignore")
    for path in sorted(out.rglob("benchmark_stdout.txt")):
        text += "\n" + path.read_text(errors="ignore")
    for path in sorted(out.rglob("sim.stats")):
        text += "\n" + path.read_text(errors="ignore")
    return text, bool(text)


def run_gem5(kernel):
    """gem5 <kernel> with ECG_GRASP_POPT + coverage geometry. Schedule-2 runs
    all five kernels on RISC-V through the request-bound K2 property load. The
    record/sidecar stream remains separate and may carry StreamShield."""
    out = Path("/tmp") / f"equivk_gem5_{GEM5_ISA_VARIANT}_{kernel}"
    shutil.rmtree(out, ignore_errors=True)
    guest = (
        ecg.ROOT / "bench/bin_gem5" / f"{kernel}_riscv_m5ops")
    guest_stale = stale_dependencies(guest, (
        ecg.ROOT / "bench/src_gem5" / f"{kernel}.cc",
        ecg.ROOT / "bench/include/gem5_sim/gem5_harness.h",
        ecg.ROOT / "bench/include/ecg_epoch_builder.h",
    ))
    gem5_stale = stale_dependencies(GEM5_RISCV, (
        ecg.ROOT /
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.cc",
        ecg.ROOT /
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_victim_policy.hh",
        ecg.ROOT /
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_epoch_request_ext.hh",
        ecg.ROOT /
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa",
    ))
    stale = guest_stale + gem5_stale
    if stale:
        error = "stale gem5 inputs: " + ", ".join(stale)
        RUN_META[("gem5", kernel)] = {
            "returncode": None,
            "errors": [error],
        }
        return ("", False), "(stale binary)"
    if kernel in GEM5_RISCV_KERNELS:
        env = {**os.environ, "GEM5_OPT": str(GEM5_RISCV), "GEM5_KERNEL_SUFFIX": "_riscv_m5ops",
               "GEM5_FORCE_ECG_EXTRACT": "1",
               "GEM5_ECG_PFX_MODE": "6", "ECG_PREFETCH_MODE": "6",
               "ECG_VARIANT": effective_variant(kernel), "ECG_EVICT_TRACE": "4000",
               "ECG_EVICT_TRACE_ROI": "1", "ECG_STORED_REFRESH": "1",
               "ECG_DEBUG": "1"}
    else:
        env = {**os.environ, "GEM5_OPT": str(GEM5_X86), "GEM5_KERNEL_SUFFIX": "_m5ops",
               "GEM5_FORCE_ECG_EXTRACT": "1", "GEM5_ECG_PFX_MODE": "6", "ECG_PREFETCH_MODE": "6",
               "ECG_VARIANT": effective_variant(kernel), "ECG_EVICT_TRACE": "4000",
               "ECG_EVICT_TRACE_ROI": "1", "ECG_STORED_REFRESH": "1",
               "ECG_DEBUG": "1"}
    if SCHEDULE_K:
        env["ECG_EDGE_MASK_SCHED"] = str(SCHEDULE_K)
        env["ECG_K2_DELIVERY_TRACE"] = "32"
    if STREAM_BYPASS:
        env["ECG_STREAM_BYPASS"] = "1"
        env["ECG_STREAM_BYPASS_TRACE"] = "8"
        if ADAPTIVE_STREAM_BYPASS:
            env["ECG_STREAM_BYPASS_ADAPTIVE"] = "1"
    explicit = {"ECG_K2_DELIVERY_TRACE": "32"}
    if STREAM_BYPASS:
        explicit["ECG_STREAM_BYPASS_TRACE"] = "8"
    env["GRAPHBREW_EXPLICIT_CELL_ENV"] = json.dumps(
        explicit, sort_keys=True, separators=(",", ":"))
    cmd = [sys.executable, str(ecg.ROI_MATRIX), "--suite", "gem5", "--no-build",
           "--benchmark", kernel, "--policies", "ECG:ECG_GRASP_POPT",
           "--ecg-isa-variant", GEM5_ISA_VARIANT,
           "--options", f"-f {ecg.GRAPH} -o 5 -n 1", "--l3-sizes", "4kB", "--l3-ways", "8",
           "--l1d-size", "1kB", "--l2-size", "2kB", "--out-dir", str(out)]
    if STREAM_PF_DEGREE > 0:
        cmd += ["--prefetcher", "STRIDE", "--structure-prefetch-degree", str(STREAM_PF_DEGREE)]
    try:
        process = subprocess.run(
            cmd, env=env, cwd=str(ecg.ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=1200, check=False)
        returncode = process.returncode
        timeout_error = []
    except subprocess.TimeoutExpired:
        returncode = None
        timeout_error = ["timeout after 1200 seconds"]
    text, ran = _roi_log(out)
    output_errors, row = validate_roi_output(
        out, "ECG_ECG_GRASP_POPT")
    errors = timeout_error + (
        [] if returncode == 0 else [f"roi_matrix exited {returncode}"]) + \
        output_errors
    RUN_META[("gem5", kernel)] = {
        "command": cmd,
        "returncode": returncode,
        "output_dir": str(out),
        "row": row,
        "errors": errors,
    }
    return (text, ran and not errors), _banner(text)


def run_sniper(kernel):
    """Sniper sg_kernel --benchmark <kernel> with ECG_GRASP_POPT (memory-capped, guarded)."""
    out = Path("/tmp") / f"equivk_sniper_{GEM5_ISA_VARIANT}_{kernel}"
    shutil.rmtree(out, ignore_errors=True)
    workload = ecg.ROOT / "bench/bin_sniper/sg_kernel"
    sniper_binary = (
        ecg.ROOT / "bench/include/sniper_sim/snipersim/lib/sniper")
    workload_stale = stale_dependencies(workload, (
        ecg.ROOT / "bench/src_sniper/sg_kernel.cc",
        ecg.ROOT / "bench/include/sniper_sim/sniper_harness.h",
        ecg.ROOT / "bench/include/ecg_epoch_builder.h",
        ecg.ROOT / "bench/include/ecg_victim_policy.h",
    ))
    sniper_stale = stale_dependencies(sniper_binary, (
        ecg.ROOT /
        "bench/include/sniper_sim/overlays/common/core/"
        "memory_subsystem/cache/cache_set_ecg.cc",
        ecg.ROOT /
        "bench/include/sniper_sim/overlays/common/core/"
        "memory_subsystem/cache/graph_cache_context_sniper.cc",
        ecg.ROOT /
        "bench/include/sniper_sim/overlays/common/core/"
        "memory_subsystem/cache/ecg_victim_policy.h",
    ))
    stale = workload_stale + sniper_stale
    if stale:
        error = "stale Sniper inputs: " + ", ".join(stale)
        RUN_META[("sniper", kernel)] = {
            "returncode": None,
            "errors": [error],
        }
        return ("", False), "(stale binary)"
    env = {**os.environ, "SNIPER_ECG_MODE": "ECG_GRASP_POPT",
           "ECG_VARIANT": effective_variant(kernel),
           "ECG_EVICT_TRACE": "4000", "ECG_DEBUG": "1"}
    if SCHEDULE_K:
        env["ECG_EDGE_MASK_SCHED"] = str(SCHEDULE_K)
        env["ECG_K2_DELIVERY_TRACE"] = "32"
    if STREAM_BYPASS:
        env["ECG_STREAM_BYPASS"] = "1"
        env["ECG_STREAM_BYPASS_TRACE"] = "8"
        if ADAPTIVE_STREAM_BYPASS:
            env["ECG_STREAM_BYPASS_ADAPTIVE"] = "1"
    explicit = {"ECG_K2_DELIVERY_TRACE": "32"}
    if STREAM_BYPASS:
        explicit["ECG_STREAM_BYPASS_TRACE"] = "8"
    env["GRAPHBREW_EXPLICIT_CELL_ENV"] = json.dumps(
        explicit, sort_keys=True, separators=(",", ":"))
    # Per-kernel geometry: cc's comp[] (~4KB) and sssp's dist[] fit Sniper's inner
    # caches, and the L3 is NON-INCLUSIVE (sees only L2 evictions), so at the default
    # 2kB/4kB/16kB the property never reaches the L3 -> no epoch is stamped (vacuous).
    # Shrink L1d+L2 below the property footprint (and the L3 with it) so comp[]/dist[]
    # spill to and churn the L3, exercising the epoch delivery. Mirrors run_cache's
    # cc/sssp CACHE_L2=1kB/L3=2kB special-case. (cc stays do-no-harm: no OUT-edge mask.)
    if kernel in ("cc", "sssp"):
        l1d, l2, l3 = "1kB", "1kB", "2kB"
    else:
        l1d, l2, l3 = "2kB", "4kB", "16kB"
    policy = sniper_policy()
    cmd = [sys.executable, str(ecg.ROI_MATRIX), "--suite", "sniper",
           "--sniper-workload", "sg_kernel", "--allow-sniper-sg-kernel-workload",
           "--sniper-memory-limit-gb", "20", "--no-build",
           "--benchmark", kernel, "--policies", policy,
           "--ecg-isa-variant", GEM5_ISA_VARIANT,
           "--options", f"-f {ecg.GRAPH} -o 5 -n 1", "--l3-sizes", l3, "--l3-ways", "8",
           "--l1d-size", l1d, "--l2-size", l2, "--timeout-sniper", "540", "--out-dir", str(out)]
    try:
        process = subprocess.run(
            cmd, env=env, cwd=str(ecg.ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=900, check=False)
        returncode = process.returncode
        timeout_error = []
    except subprocess.TimeoutExpired:
        returncode = None
        timeout_error = ["timeout after 900 seconds"]
    text, ran = _roi_log(out)
    output_errors, row = validate_roi_output(
        out, sniper_policy_label())
    errors = timeout_error + (
        [] if returncode == 0 else [f"roi_matrix exited {returncode}"]) + \
        output_errors
    RUN_META[("sniper", kernel)] = {
        "command": cmd,
        "returncode": returncode,
        "output_dir": str(out),
        "row": row,
        "errors": errors,
    }
    return (text, ran and not errors), _banner(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Multi-kernel 3-sim ECG equivalence + debug")
    ap.add_argument("--gem5", action="store_true", help="also run gem5 (X86; slower)")
    ap.add_argument("--sniper", action="store_true", help="also run Sniper (guarded; slowest)")
    ap.add_argument("--kernels", nargs="+", default=list(KERNEL_SIMS),
                    choices=list(KERNEL_SIMS))
    ap.add_argument("--stream-prefetch-degree", type=int, default=0,
                    help="cross-sim structure stream-prefetcher degree (0=off, the byte-identical "
                         "baseline; >0 = spec-level equivalence under the realistic prefetcher).")
    ap.add_argument("--schedule-k", type=int, choices=[0, 2], default=0,
                    help="enable Schedule-2 delivery and require live K2 pair/distance coverage "
                         "for PR/BFS/SSSP/BC/CC.")
    ap.add_argument("--stream-bypass", action="store_true",
                    help="enable StreamShield and require a live LLC-bypass mechanism trace.")
    ap.add_argument("--adaptive-stream-bypass", action="store_true",
                    help="duel LLC allocation versus StreamShield for eligible "
                         "K2 records (requires --stream-bypass).")
    ap.add_argument(
        "--gem5-isa-variant", choices=["indexed", "mask"], default="indexed",
        help="K2 ISA/model variant for gem5 and Sniper; mask validates "
             "computed-address K2-M.")
    ap.add_argument(
        "--evidence-dir", type=Path,
        help="Archive a complete manifest, raw traces, ROI rows, and "
             "structured per-cell coverage.")
    ap.add_argument(
        "--overwrite-evidence", action="store_true",
        help="Replace an existing evidence directory.")
    ap.add_argument(
        "--allow-dirty", action="store_true",
        help="Allow evidence capture from a dirty git worktree.")
    args = ap.parse_args(argv)

    global STREAM_PF_DEGREE, SCHEDULE_K, STREAM_BYPASS
    global ADAPTIVE_STREAM_BYPASS, GEM5_ISA_VARIANT
    STREAM_PF_DEGREE = args.stream_prefetch_degree
    SCHEDULE_K = args.schedule_k
    STREAM_BYPASS = args.stream_bypass
    ADAPTIVE_STREAM_BYPASS = args.adaptive_stream_bypass
    GEM5_ISA_VARIANT = args.gem5_isa_variant
    RUN_META.clear()
    if STREAM_BYPASS and SCHEDULE_K != 2:
        ap.error("--stream-bypass requires --schedule-k 2")
    if ADAPTIVE_STREAM_BYPASS and not STREAM_BYPASS:
        ap.error("--adaptive-stream-bypass requires --stream-bypass")
    if (GEM5_ISA_VARIANT == "mask" and
            not (args.gem5 or args.sniper)):
        ap.error("--gem5-isa-variant mask requires --gem5 or --sniper")
    if GEM5_ISA_VARIANT == "mask" and SCHEDULE_K != 2:
        ap.error("--gem5-isa-variant mask requires --schedule-k 2")

    required = [ecg.GRAPH]
    required.extend(
        ecg.ROOT / "bench" / "bin_sim" / kernel
        for kernel in args.kernels
    )
    if args.gem5:
        required.extend(
            ecg.ROOT / "bench" / "bin_gem5" /
            f"{kernel}{'_riscv_m5ops' if kernel in GEM5_RISCV_KERNELS else '_m5ops'}"
            for kernel in args.kernels
        )
    if args.sniper:
        required.extend([
            ecg.ROOT / "bench" / "bin_sniper" / "sg_kernel",
            ecg.ROOT / "bench" / "include" / "sniper_sim" /
            "snipersim" / "lib" / "sniper",
        ])
    missing_inputs = [str(path) for path in required if not path.exists()]
    if missing_inputs:
        print("FAIL: missing equivalence inputs:")
        for path in missing_inputs:
            print(f"  - {path}")
        print("See wiki/Reproduction.md for graph staging and build commands.")
        return 2

    evidence_dir = args.evidence_dir
    if evidence_dir:
        evidence_dir = evidence_dir.resolve()
        if evidence_dir.exists() and any(evidence_dir.iterdir()):
            if not args.overwrite_evidence:
                print(
                    f"FAIL: evidence directory is not empty: {evidence_dir}")
                return 2
            shutil.rmtree(evidence_dir)
        evidence_dir.mkdir(parents=True, exist_ok=True)
        dirty = git_text("status", "--porcelain")
        if dirty and not args.allow_dirty:
            print("FAIL: evidence capture requires a clean git worktree")
            return 2
        manifest = {
            "schema_version": 1,
            "status": "running",
            "started_at_utc": datetime.datetime.now(
                datetime.timezone.utc).isoformat(),
            "command": [sys.executable, str(Path(__file__).resolve()), *(
                argv if argv is not None else sys.argv[1:])],
            "git_head": git_text("rev-parse", "HEAD"),
            "git_status_porcelain": dirty,
            "configuration": {
                "kernels": list(args.kernels),
                "simulators": [
                    sim for sim, enabled in (
                        ("cache_sim", True),
                        ("gem5", args.gem5),
                        ("sniper", args.sniper))
                    if enabled
                ],
                "schedule_k": SCHEDULE_K,
                "isa_variant": GEM5_ISA_VARIANT,
                "stream_prefetch_degree": STREAM_PF_DEGREE,
                "stream_bypass": STREAM_BYPASS,
                "adaptive_stream_bypass": ADAPTIVE_STREAM_BYPASS,
            },
            "inputs": evidence_inputs(
                args.kernels, args.gem5, args.sniper, evidence_dir),
        }
        write_json(evidence_dir / "manifest.json", manifest)

    print("== Independent mechanism preflight ==")
    preflight = {
        "exact_victim_unit": ecg.run_synthetic(),
        "field_layout_parity": ecg.run_field_parity(),
        "epoch_pair_unit": ecg.run_epoch_pair_unit(),
        "unknown_mode_hard_fail": ecg.verify_unknown_mode_hardfails(),
        "unknown_variant_hard_fail":
            ecg.verify_unknown_variant_hardfails(),
    }
    preflight_ok = all(preflight.values())
    if evidence_dir:
        write_json(evidence_dir / "preflight.json", preflight)
    if not preflight_ok:
        print("FAIL: independent mechanism preflight")
        if evidence_dir:
            manifest["status"] = "failed"
            manifest["finished_at_utc"] = datetime.datetime.now(
                datetime.timezone.utc).isoformat()
            write_json(evidence_dir / "manifest.json", manifest)
        return 1
    print()

    enabled = {"cache_sim"}
    if args.gem5:
        need = {(GEM5_RISCV if k in GEM5_RISCV_KERNELS else GEM5_X86) for k in args.kernels}
        missing = [str(p) for p in need if not p.exists()]
        if missing:
            print("FAIL: build gem5 first: " + ", ".join(missing)); return 2
        enabled.add("gem5")
    if args.sniper:
        enabled.add("sniper")
    RUNNERS = {"cache_sim": run_cache, "gem5": run_gem5, "sniper": run_sniper}
    sims_order = [s for s in ("cache_sim", "gem5", "sniper") if s in enabled]

    print("== Multi-kernel 3-sim K2 mechanism conformance ==")
    variant_label = (
        "PR=epoch_first,BFS/SSSP=degree_first,BC/CC=rrip_first"
        if SCHEDULE_K else "rrip_first")
    print(f"   graph={ecg.GRAPH.name}  policy=ECG:ECG_GRASP_POPT variant={variant_label}  "
          f"sims={sims_order}")
    print("   (SSSP weights are auto-synthesized by WeightedBuilder)\n")

    ok_all = True
    results = {}   # (sim, kernel) -> status: 'ok' / 'spec-FAIL' / 'banner-X' / 'n/a'
    cell_records = []
    for kernel in args.kernels:
        for sim in sims_order:
            if sim not in KERNEL_SIMS[kernel]:
                results[(sim, kernel)] = "n/a"
                continue
            result, banner = RUNNERS[sim](kernel)
            cov = {}
            fused_path_ok = True
            if SCHEDULE_K:
                spec_ok = ecg.verify_k2_trace(
                    f"{sim}/{kernel}", result,
                    ne=32768 if SCHEDULE_K == 2 else 65535,
                    coverage=cov,
                    expected_policy=expected_trace_policy(kernel),
                    require_exact_bind=(
                        sim == "sniper" and GEM5_ISA_VARIANT == "mask"))
                bc_dual_load_ok = True
                if kernel == "bc" and sim == "gem5":
                    bc_dual_load_ok = (
                        {4, 8}.issubset(set(
                            cov.get("k2_accept_widths", []))) and
                        cov.get("k2_accept_valid", False))
                elif kernel == "bc" and sim == "sniper":
                    bc_dual_load_ok = (
                        {8, 16}.issubset(set(
                            cov.get("k2_fused_vertices_per_line", []))))
                if kernel == "bc" and sim in ("gem5", "sniper"):
                    print(
                        "      BC depth/path_counts delivery: "
                        f"{'[OK]' if bc_dual_load_ok else '[FAIL]'}")
                spec_ok = spec_ok and bc_dual_load_ok
            else:
                spec_ok = ecg.verify_trace(
                    f"{sim}/{kernel}", result, coverage=cov,
                    expected_policy=expected_trace_policy(kernel))
            if SCHEDULE_K == 2:
                text, ran_ok = result
                if sim == "gem5":
                    if GEM5_ISA_VARIANT == "mask":
                        fused_marker = (
                            "[ECG_K2_MLOAD_CW24]"
                            if kernel == "sssp"
                            else "[ECG_K2_MLOAD]")
                        fused_label = "computed-address K2-M property load"
                    elif kernel == "sssp":
                        fused_marker = "[ECG_K2_ILOAD_CW24]"
                        fused_label = "indexed K2-I property load"
                    elif STREAM_BYPASS:
                        fused_marker = "[ECG_K2_ILOAD]"
                        fused_label = "indexed K2-I property load"
                    else:
                        fused_marker = "[ECG_K2_ILOAD]"
                        fused_label = "indexed K2-I property load"
                    fused_ok = fused_marker in text
                    fused_path_ok = ran_ok and fused_ok
                    print(f"      {fused_label}: "
                          f"{'[OK]' if ran_ok and fused_ok else '[FAIL]'}")
                    spec_ok &= ran_ok and fused_ok
                    if (kernel == "sssp" and
                            GEM5_ISA_VARIANT == "mask"):
                        rows_path = (
                            Path("/tmp") /
                            f"equivk_gem5_{GEM5_ISA_VARIANT}_{kernel}" /
                            "roi_matrix.csv")
                        provenance_ok = False
                        if rows_path.exists():
                            rows = list(csv.DictReader(rows_path.open()))
                            provenance_ok = len(rows) == 1 and all((
                                rows[0].get("ecg_isa_variant") == "mask",
                                rows[0].get("ecg_record_bytes") == "8",
                                rows[0].get("edge_stream_bytes_per_edge") == "8",
                                rows[0].get("ecg_record_replaces_edge") == "1",
                            ))
                        print("      compact K2-M provenance: "
                              f"{'[OK]' if provenance_ok else '[FAIL]'}")
                        spec_ok &= provenance_ok
                elif sim == "sniper":
                    if GEM5_ISA_VARIANT == "mask":
                        rows_path = (
                            Path("/tmp") /
                            f"equivk_sniper_{GEM5_ISA_VARIANT}_{kernel}" /
                            "roi_matrix.csv")
                        provenance_ok = False
                        if rows_path.exists():
                            rows = list(csv.DictReader(rows_path.open()))
                            marker_path = rows_path.with_name(
                                "roi_matrix.complete.json")
                            marker_ok = False
                            if marker_path.exists():
                                marker = json.loads(marker_path.read_text())
                                marker_ok = (
                                    marker.get("complete") is True and
                                    marker.get("all_rows_ok") is True)
                            provenance_ok = len(rows) == 1 and all((
                                rows[0].get("status") == "ok",
                                rows[0].get("ecg_isa_variant") == "mask",
                                rows[0].get(
                                    "policy_label") == sniper_policy_label(),
                                rows[0].get("sniper_transport_matched") == "1",
                                rows[0].get("sniper_k2_exact_bind") == "1",
                                rows[0].get(
                                    "sniper_k2_epoch_context_bound") == "1",
                                rows[0].get(
                                    "sniper_transport_receipts_validated") ==
                                    "1",
                                rows[0].get(
                                    "sniper_k2_exact_bind_validated") == "1",
                                rows[0].get(
                                    "sniper_k2_epoch_context_validated") == "1",
                                rows[0].get("sniper_context_loaded") == "1",
                                rows[0].get(
                                    "sniper_popt_matrix_required") == "0",
                                rows[0].get(
                                    "sniper_rereference_loaded") == "0",
                                marker_ok,
                            ))
                        fused_ok = (
                            provenance_ok and
                            "[K2_TRANSPORT_MATCHED]" in text and
                            "[K2_EXACT_BIND]" in text)
                        fused_label = "computed-address K2-M load binding"
                    else:
                        valid = ecg.K2_FUSED_VALID_RE.search(text)
                        fused_ok = (
                            valid is not None and
                            int(valid.group(1)) > 0 and
                            int(valid.group(2)) == 0)
                        fused_label = "fused K2 sideband"
                    fused_path_ok = ran_ok and fused_ok
                    print(f"      {fused_label}: "
                          f"{'[OK]' if ran_ok and fused_ok else '[FAIL]'}")
                    spec_ok &= ran_ok and fused_ok
                if sim in ("gem5", "sniper"):
                    provenance_ok, provenance_checks = (
                        detailed_row_provenance(sim, kernel))
                    print(
                        "      governed-region/variant provenance: "
                        f"{'[OK]' if provenance_ok else '[FAIL]'} "
                        f"{provenance_checks}")
                    spec_ok &= provenance_ok
            streamshield_ok = None
            if STREAM_BYPASS:
                text, ran_ok = result
                if ADAPTIVE_STREAM_BYPASS:
                    adaptive_marker = (
                        "[ECG-STREAM-BYPASS sim=cache_sim active=1 adaptive=1]"
                        if sim == "cache_sim" else
                        f"[ECG-STREAM-ADAPTIVE sim={sim} active=1]")
                    bypass_ok = adaptive_marker in text
                elif sim == "cache_sim":
                    bypass_ok = (
                        "[ECG-STREAM-BYPASS sim=cache_sim active=1" in text)
                elif sim == "gem5":
                    bypass_ok = "[ECG-STREAM-BYPASS sim=gem5" in text
                else:
                    reads = re.search(r"nuca-cache\.stream-bypass-reads = (\d+)", text)
                    writes = re.search(r"nuca-cache\.stream-bypass-writes = (\d+)", text)
                    bypass_ok = (
                        reads is not None and writes is not None and
                        int(reads.group(1)) > 0 and int(writes.group(1)) > 0
                    )
                print(f"      StreamShield LLC bypass: "
                      f"{'[OK]' if ran_ok and bypass_ok else '[FAIL]'}")
                streamshield_ok = (
                    ran_ok and fused_path_ok and bypass_ok and
                    bool(cov.get("k2_delivery_match", False)))
            ev, tv = cov.get("epoch_victims", 0), cov.get("victims", 0)
            dec = cov.get("epoch_decisive", 0)
            nz = cov.get("epoch_victims_nz", 0)   # stamped victims with a NON-ZERO delivered epoch
            k2_live = cov.get("k2_ways", 0) > 0
            delivery_ok = ev > 0 or (SCHEDULE_K == 2 and k2_live)
            # >=1 stamped property victim normally proves delivery. Under K2,
            # resident stamped K2 property ways + verified pair distances also
            # prove delivery even if a non-inclusive backend evicts records for
            # the whole bounded trace (Sniper PR's do-no-harm geometry).
            decisive_ok = dec > 0         # epoch DISTANCE strictly decided >=1 victim
            # collapse check: stamped property was evicted but EVERY delivered epoch was 0 -> the
            # epochs collapsed (a delivery-quality regression, not the benign tied-eff-dist case).
            collapsed = ev > 0 and nz == 0
            if STREAM_BYPASS:
                cell_ok = bool(streamshield_ok)
                label = (
                    ("adaptive allocate-vs-shield placement is live; "
                     if ADAPTIVE_STREAM_BYPASS else
                     "StreamShield removes post-delivery LLC churn; ") +
                    "K2 delivery is live and eviction is certified by the "
                    "separate no-bypass fused gate")
            elif STREAM_PF_DEGREE > 0:
                # Under the realistic stream prefetcher, the prefetched STRUCTURAL lines
                # change the cache contents (and carry no epoch), so the epoch may no
                # longer strictly DECIDE a victim -- decisive/nonzero are degree-0 metrics.
                # The equivalence here is SPEC-level: every eviction must still obey the
                # ECG policy spec (verify_trace -> spec_ok).
                cell_ok = spec_ok
                label = (f"prefetch(d{STREAM_PF_DEGREE}) spec-level: evictions obey spec; "
                         f"decisive {dec}x nonzero {nz} stamped {ev} (decisiveness is the degree-0 metric)")
            elif (kernel in EXPECTED_DECISIVE and sim != "sniper" and
                  not STREAM_BYPASS):
                cell_ok = decisive_ok
                label = ("decisive real-epoch reuse" if decisive_ok
                         else "FAIL: reuse-sensitive kernel has no decisive epoch eviction")
            elif kernel == "cc" and sim == "sniper":
                # cc DECISION-level certification on Sniper. cc is the do-no-harm cell
                # (union-find pointer-chases a SMALL, heavily-reused comp[]: most comp[]
                # accesses are undelivered chain hops, and the few property lines that
                # fill are mostly already-resident), and Sniper's L3 is NON-INCLUSIVE
                # (comp[] is protected + fits the inner caches), so property lines carry
                # a fresh delivered epoch only rarely -> stamped ~0 / epochs collapse to
                # 0. This mirrors the inclusive-vs-non-inclusive gap seen elsewhere; the
                # inclusive cache_sim/gem5 legs DO deliver+stamp cc (nonzero>0). What is
                # certifiable here is the byte-identical eviction DECISION: every eviction
                # obeys the ECG spec + the debug banner matches. Do NOT require delivery.
                cell_ok = spec_ok
                label = ("do-no-harm DECISION certified on Sniper (spec obeyed + banner); "
                         "epoch-delivery not exercised: union-find chases a small reused comp[] on "
                         f"a non-inclusive L3 -> stamped={ev} (delivery exercised on inclusive cache_sim/gem5)")
            elif not delivery_ok:
                cell_ok = False
                label = "FAIL: NO epoch delivered (vacuous)"
            elif collapsed:
                cell_ok = False
                label = f"FAIL: {ev} stamped victims but ALL epoch=0 (delivery COLLAPSED, not do-no-harm)"
            else:
                cell_ok = True            # do-no-harm: delivery + policy verified
                if SCHEDULE_K == 2 and k2_live and ev == 0:
                    label = (
                        f"K2 resident+distance verified ({cov.get('k2_ways', 0)} ways); "
                        "no property victim in bounded trace (record-first do-no-harm)")
                else:
                    label = (f"delivery+policy verified; epoch decisive {dec}x, nonzero {nz}/{ev}"
                             + ("" if decisive_ok else " (do-no-harm: tied eff-dist -> epoch seldom decisive)"))
            print(f"      epoch coverage: decisive={dec} nonzero={nz} stamped={ev} / {tv} total  [{label}]")
            expected_variant = effective_variant(kernel)
            banner_ok = all((
                "policy=ECG" in banner,
                "ECG_GRASP_POPT" in banner,
                f"variant={expected_variant}" in banner,
            ))
            print(f"      debug banner: {banner}  [{'OK' if banner_ok else 'MISSING'}]")
            if not spec_ok:
                status = "spec-FAIL"
            elif not banner_ok:
                status = "banner-X"
            elif not cell_ok:
                if kernel in EXPECTED_DECISIVE and sim != "sniper":
                    status = "FAIL-dec0"
                elif collapsed:
                    status = "FAIL-collapse"
                else:
                    status = "FAIL-nodeliv"
            else:
                status = "ok"
            results[(sim, kernel)] = status
            ok_all &= (status == "ok")
            if evidence_dir:
                text, _ran_ok = result
                coverage_record = {
                    **cov,
                    "strength": (
                        "decisive_epoch"
                        if dec > 0 else
                        "delivery_and_decision_conformance"),
                    "decisive_required": (
                        kernel in EXPECTED_DECISIVE and sim != "sniper"),
                    "stored_refresh_model": sim in ("cache_sim", "gem5"),
                }
                cell_records.append(archive_cell(
                    evidence_dir, sim, kernel, text, banner,
                    coverage_record, status,
                    expected_trace_policy(kernel),
                    RUN_META.get((sim, kernel), {})))

    print("\n## kernel x sim matrix (ok = spec PASS + banner + [bfs/bc/sssp on cache_sim+gem5: "
          ">=1 DECISIVE epoch victim] [cc on cache_sim/gem5: epoch DELIVERED, do-no-harm; "
          "cc on Sniper: DECISION-level (spec+banner) — union-find/non-inclusive L3 doesn't exercise delivery])")
    hdr = "kernel".ljust(8) + "".join(s.ljust(14) for s in sims_order)
    print(hdr)
    for kernel in args.kernels:
        row = kernel.ljust(8)
        for sim in sims_order:
            row += results[(sim, kernel)].ljust(14)
        print(row)
    bad = [
        f"{s}/{k}" for (s, k), value in results.items()
        if value not in ("ok", "n/a")
    ]
    if bad:
        print(f"\nFAIL: {', '.join(sorted(bad))}")
    result_text = (
        "ALL kernel x simulator cells CONFORM ✓"
        if ok_all else "see FAIL above")
    print(f"\nRESULT: {result_text}")
    if evidence_dir:
        summary = {
            "schema_version": 1,
            "status": "passed" if ok_all else "failed",
            "finished_at_utc": datetime.datetime.now(
                datetime.timezone.utc).isoformat(),
            "preflight": preflight,
            "git_head": manifest["git_head"],
            "configuration": manifest["configuration"],
            "proof_inputs": {
                name: manifest["inputs"][name]
                for name in ("sniper_binary", "sniper_workload")
                if name in manifest["inputs"]
            },
            "matrix": {
                kernel: {
                    sim: results[(sim, kernel)]
                    for sim in sims_order
                }
                for kernel in args.kernels
            },
            "cells": cell_records,
            "cell_count": len(cell_records),
            "passed_cells": sum(
                record["status"] == "ok" for record in cell_records),
            "decisive_epoch_cells": sum(
                record["coverage"]["strength"] == "decisive_epoch"
                for record in cell_records),
            "required_decisive_cells": sum(
                record["coverage"]["decisive_required"]
                for record in cell_records),
        }
        write_json(evidence_dir / "summary.json", summary)
        manifest["status"] = summary["status"]
        manifest["finished_at_utc"] = summary["finished_at_utc"]
        manifest["summary_sha256"] = sha256_file(
            evidence_dir / "summary.json")
        write_json(evidence_dir / "manifest.json", manifest)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
