#!/usr/bin/env python3
"""Algorithmic-output correctness gate.

DISTINCT from equiv_kernels.py. That gate proves the three simulators apply the
SAME cache EVICTION decision. THIS gate proves each kernel computes the CORRECT
graph result -- i.e. the cache instrumentation and the ECG ISA hint delivery do
NOT corrupt the algorithm output. Every kernel is run with the GAPBS '-v'
verifier, which prints `Verification: PASS|FAIL`.

Lanes (a lane = one place the computation actually runs):
  cache_sim    bench/bin_sim/<k>             the cache_sim simulation itself
  gem5_host    bench/bin_gem5/<k>            host build of the gem5 kernel source
  sniper_host  bench/bin_sniper/<k>          host build of the sniper kernel source
  gem5         gem5.opt graph_se.py ... -v   IN-SIM, ECG_GRASP_POPT ISA delivery (--gem5; slow)

Sniper IN-SIM self-verify is intentionally absent: the measurement wrapper
(sg_kernel) is builder-free and verifier-less, and the full GAPBS binary inside
Sniper blows up to ~53 GiB RSS (roi_matrix.py guards it). Sniper computation
correctness is covered by the sniper_host lane (identical source) plus the gem5
in-sim PASS, which proves an ISA-delivery simulation preserves the computation.

Usage:
  python3 scripts/experiments/ecg/verify/correctness.py            # fast: host lanes
  python3 scripts/experiments/ecg/verify/correctness.py --gem5     # + gem5 in-sim (slow)
  python3 scripts/experiments/ecg/verify/correctness.py --gem5 --kernels pr bfs
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ecg  # noqa: E402  (ROOT, GRAPH, GEM5_OPT)

ROOT = ecg.ROOT
GRAPH = ecg.GRAPH
GEM5_OPT = ecg.GEM5_OPT
GEM5_CONFIG = ROOT / "bench" / "include" / "gem5_sim" / "configs" / "graphbrew" / "graph_se.py"

# Per-kernel GAPBS verify options ('-v' triggers the verifier).
# PR uses -i 20 so the PageRank residual converges below the verifier's 1e-4
# tolerance (-i 5 FAILs = under-convergence, NOT a correctness bug). bfs/sssp pin
# -r 0 so the kernel and its verifier draw the identical source.
KERNEL_OPTS = {
    "pr":   "-n 1 -i 20",
    "bfs":  "-n 1 -r 0",
    "sssp": "-n 1 -r 0 -d 1",
    "bc":   "-n 1",
    "cc":   "-n 1",
}

# Host lanes use a binary directory under bench/.
HOST_LANES = {
    "cache_sim":   "bin_sim",
    "gem5_host":   "bin_gem5",
    "sniper_host": "bin_sniper",
}

PASS, FAIL, NA, NOVERIFY = "PASS", "FAIL", "n/a", "no-verify"


def parse_verification(text: str) -> str | None:
    """'PASS'/'FAIL' from a GAPBS 'Verification:' line, else None (verifier never ran)."""
    for line in text.splitlines():
        if "Verification:" in line:
            return PASS if "PASS" in line else FAIL
    return None


def run_host(bin_dir: str, kernel: str) -> str:
    """Run a host GAPBS binary with -v and report PASS/FAIL/n/a/no-verify."""
    binary = ROOT / "bench" / bin_dir / kernel
    if not binary.exists():
        return NA
    cmd = [str(binary), "-f", str(GRAPH), *KERNEL_OPTS[kernel].split(), "-v"]
    env = {**os.environ, "OMP_NUM_THREADS": "1", "CACHE_ULTRAFAST": "1"}
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return NOVERIFY
    return parse_verification(p.stdout + p.stderr) or NOVERIFY


def run_gem5_insim(kernel: str) -> str:
    """Run <kernel>_riscv_m5ops inside gem5 SE with the ECG_GRASP_POPT ISA delivery."""
    binary = ROOT / "bench" / "bin_gem5" / f"{kernel}_riscv_m5ops"
    if not binary.exists() or not GEM5_OPT.exists():
        return NA
    out = Path("/tmp") / f"correctness_gem5_{kernel}"
    cmd = [str(GEM5_OPT), f"--outdir={out}", str(GEM5_CONFIG),
           "--binary", str(binary),
           "--options", f"-f {GRAPH} {KERNEL_OPTS[kernel]} -v",
           "--policy", "ECG", "--ecg-mode", "ECG_GRASP_POPT"]
    env = {**os.environ, "GEM5_FORCE_ECG_EXTRACT": "1"}
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1200)
    except subprocess.TimeoutExpired:
        return NOVERIFY
    return parse_verification(p.stdout + p.stderr) or NOVERIFY


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="Algorithmic-output correctness gate (GAPBS -v).")
    ap.add_argument("--gem5", action="store_true", help="also run the gem5 in-sim lane (slow).")
    ap.add_argument("--kernels", nargs="+", default=list(KERNEL_OPTS),
                    help="subset of kernels (default all).")
    args = ap.parse_args(argv)

    lanes = list(HOST_LANES) + (["gem5"] if args.gem5 else [])
    print(f"== Algorithmic correctness (GAPBS -v) | graph={GRAPH.name} | kernels={args.kernels} ==")
    print("   (proves the COMPUTATION is correct; equiv_kernels.py proves the cache DECISION)\n")

    results: dict[tuple[str, str], str] = {}
    for kernel in args.kernels:
        for lane in lanes:
            results[(kernel, lane)] = (run_gem5_insim(kernel) if lane == "gem5"
                                       else run_host(HOST_LANES[lane], kernel))

    col = 13
    print("kernel".ljust(8) + "".join(lane.ljust(col) for lane in lanes))
    bad: list[str] = []
    for kernel in args.kernels:
        row = kernel.ljust(8)
        for lane in lanes:
            status = results[(kernel, lane)]
            row += status.ljust(col)
            if status in (FAIL, NOVERIFY):
                bad.append(f"{kernel}/{lane}={status}")
        print(row)

    print()
    if bad:
        print("RESULT: CORRECTNESS FAIL -> " + ", ".join(bad))
        return 1
    print("RESULT: ALL algorithmic verifications PASS  (n/a = binary not built for that lane)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
