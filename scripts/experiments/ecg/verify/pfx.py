#!/usr/bin/env python3
"""Reproducible correctness check for the PREFETCH path (DROPLET + ECG prefetch).

The prefetch analogue of verify_ecg.py. Two layers, no trust in aggregate
numbers required:

  1. SYNTHETIC (covers all 3 simulators).  Build + run test_ecg_prefetch, which
     drives ecg_mode6::selectPrefetchTarget — the SINGLE shared function (one
     header, compiled into the cache_sim, gem5 AND Sniper kernels) that decides
     the ECG prefetch target. Asserting its exact output therefore verifies the
     ECG prefetch decision for every simulator at once. Mutation-proven.

  2. LIVE BEHAVIOUR (cache_sim, authoritative).  Run PageRank with each
     prefetcher and assert each one's DEFINING, falsifiable property:
       none     - issues no prefetch; total traffic == demand traffic.
       DROPLET  - fires; cuts demand-to-mem (latency); CONSERVES total DRAM
                  traffic (a prefetcher only relocates demand->prefetch, it can
                  never reduce total traffic — only eviction can). This is the
                  honest DROPLET property and the reason bandwidth needs ECG
                  eviction, not a prefetcher.
       ECG_PFX  - fires; cuts demand-to-mem; is SELECTIVE (issues strictly fewer
                  prefetches than DROPLET at a comparable useful-rate) — the
                  shared selectPrefetchTarget picks POPT-best targets instead of
                  every next-K edge.

Exit code 0 iff the synthetic decision is exact AND every prefetcher obeys its
spec. Researcher-runnable artifact verification.

  python3 scripts/experiments/ecg/verify/pfx.py
"""
import json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
PR = ROOT / "bench" / "bin_sim" / "pr"
ROI = ROOT / "scripts" / "experiments" / "ecg" / "roi_matrix.py"
SYNTH_BIN = ROOT / "bench" / "bin_sim" / "test_ecg_prefetch"
SHARED_POLICY_TEST = ROOT / "scripts" / "test" / "test_shared_ecg_policy.py"
# Power-law cell with genuine L3 pressure so the prefetch path is exercised
# (email-Eu-core's property array fits the cache -> no pressure -> vacuous).
GRAPH = ROOT / "results" / "graphs" / "web-Google" / "web-Google.sg"
L3 = "512kB"
LOOKAHEAD = 8


def run_synthetic():
    """Build + run the synthetic exact-target test for the shared ECG prefetch
    decision (used by all three simulators)."""
    subprocess.run(["make", "bench/bin_sim/test_ecg_prefetch"], cwd=str(ROOT),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not SYNTH_BIN.exists():
        print("  [synthetic] FAIL: could not build test_ecg_prefetch")
        return False
    p = subprocess.run([str(SYNTH_BIN)], capture_output=True, text=True, timeout=60)
    for line in p.stdout.splitlines():
        if "expect=" in line or line.startswith("[test_ecg_prefetch]") or "RESULT" in line:
            print("  " + line.rstrip())
    return p.returncode == 0


def run_shared_policy_check():
    """Assert every simulator calls the shared selectPrefetchTarget."""
    p = subprocess.run([
        sys.executable, "-m", "pytest", str(SHARED_POLICY_TEST),
        "-q", "-k", "prefetch or shared"],
                       cwd=str(ROOT), capture_output=True, text=True, timeout=120)
    line = next((l for l in p.stdout.splitlines() if "passed" in l or "failed" in l), "")
    print(
        "  [shared-policy] selectPrefetchTarget across simulators: "
        f"{line.strip() or 'see pytest'}")
    return p.returncode == 0


def run_prefetcher(prefetcher):
    """Run cache_sim PR with the given prefetcher; return its prefetch metrics."""
    out = Path("/tmp") / f"verify_pfx_{prefetcher}"
    cmd = [sys.executable, str(ROI), "--suite", "cache-sim", "--no-build",
           "--benchmark", "pr", "--policies", "LRU",
           "--options", f"-f {GRAPH} -o 0 -n 1 -i 1",
           "--l3-sizes", L3, "--l3-ways", "16", "--l1d-size", "32kB", "--l2-size", "256kB",
           "--prefetcher", prefetcher, "--ecg-pfx-lookahead", str(LOOKAHEAD),
           "--out-dir", str(out)]
    subprocess.run(cmd, env=dict(os.environ), cwd=str(ROOT),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900, check=False)
    rows = json.load(open(out / "roi_matrix.json"))
    rows = rows if isinstance(rows, list) else [rows]
    r = rows[0]
    fills = r.get("prefetch_fills") or 0
    return {
        "demand": r.get("memory_accesses") or 0,
        "bw": r.get("total_memory_traffic") or 0,
        "fills": fills,
        "useful": (r.get("prefetch_fill_useful_rate") or 0.0) if fills else 0.0,
    }


def _check(ok, label):
    print(f"    [{'OK ' if ok else 'FAIL'}] {label}")
    return ok


def verify_live():
    """Run each prefetcher and assert its defining property against the
    no-prefetch baseline."""
    print(f"  cell: web-Google / L3={L3} / LRU eviction / lookahead={LOOKAHEAD}")
    m = {pf: run_prefetcher(pf) for pf in ("none", "DROPLET", "ECG_PFX")}
    base = m["none"]
    for pf in ("none", "DROPLET", "ECG_PFX"):
        x = m[pf]
        print(f"    {pf:8s} demand2mem={x['demand']/1e6:6.2f}M  bandwidth={x['bw']/1e6:6.2f}M  "
              f"fills={x['fills']/1e6:6.2f}M  useful={x['useful']*100:5.1f}%")
    ok = True
    # none: no prefetch issued; total traffic == demand traffic.
    ok &= _check(base["fills"] == 0 and base["bw"] == base["demand"],
                 "none: issues no prefetch, total traffic == demand traffic")
    # DROPLET: fires, cuts latency, CONSERVES traffic (cannot reduce total DRAM
    # traffic — only relocates demand->prefetch).
    d = m["DROPLET"]
    ok &= _check(d["fills"] > 0, "DROPLET: issues prefetches (path exercised)")
    ok &= _check(d["demand"] < base["demand"], "DROPLET: cuts demand-to-mem (latency) vs baseline")
    ok &= _check(d["bw"] >= base["bw"] * 0.99,
                 "DROPLET: does NOT reduce total traffic (conserves/relocates bandwidth)")
    # ECG_PFX: fires, cuts latency, is SELECTIVE (fewer fills than DROPLET).
    e = m["ECG_PFX"]
    ok &= _check(e["fills"] > 0, "ECG_PFX: issues prefetches (path exercised)")
    ok &= _check(e["demand"] < base["demand"], "ECG_PFX: cuts demand-to-mem (latency) vs baseline")
    ok &= _check(e["fills"] < d["fills"],
                 "ECG_PFX: issues FEWER prefetches than DROPLET (selective target)")
    ok &= _check(e["useful"] >= 0.5, "ECG_PFX: useful-rate >= 50% (accurate targets)")
    return ok


def main():
    if not PR.exists():
        print(f"FAIL: build cache_sim first (make sim-pr): {PR}")
        return 2
    ok = True
    print("== synthetic exact-target test (shared decision; covers cache_sim + gem5 + Sniper) ==")
    ok &= run_synthetic()
    ok &= run_shared_policy_check()
    print("\n-- live prefetch behaviour (cache_sim; each prefetcher obeys its spec) --")
    ok &= verify_live()
    print("\n" + ("ALL PREFETCH CHECKS PASSED" if ok else "PREFETCH VERIFICATION FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
