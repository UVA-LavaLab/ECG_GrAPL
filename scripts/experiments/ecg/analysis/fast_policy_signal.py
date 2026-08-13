#!/usr/bin/env python3
"""Fast policy signal: the frozen decision rule, in minutes instead of a week.

The gem5 timing matrix costs days, but the question "is K2 ahead or behind?"
is answered by cache_sim traffic in minutes. This runs the full
graph x kernel x policy matrix directly against the cache_sim kernels and
applies the reporting method from wiki/Evaluation-Methodology.md:
geometric mean of per-cell ratios, a +/-2% tie band, win/tie/loss counts, and
the worst cell always reported.

It deliberately reuses roi_matrix's own environment construction, so the signal
cannot drift from the experiment runner. What it skips is orchestration:
locking,
evidence archiving, provenance capture, gem5/Sniper. It is a fast direction
check, NOT a source of publishable numbers.

Corrected accounting is the default:
  * P-OPT's rereference-matrix column stream is simulated, not charged flat
  * the structural bypass is offered to every policy, not just K2
  * the stream prefetcher is address-only rather than oracle-guided

Tiers:
  fast  sampled graphs (n16/n18), whole matrix in a few minutes
  full  full graphs, ~2 hours because K2's mask preprocessing dominates

Usage:
  python3 scripts/experiments/ecg/analysis/fast_policy_signal.py --tier fast
  python3 scripts/experiments/ecg/analysis/fast_policy_signal.py --tier fast \
      --kernels pr,bfs --policies LRU,GRASP,ECG:K2
"""
from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BIN = ROOT / "bench/bin_sim"


def load_roi_matrix():
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_signal", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["roi_matrix_signal"] = mod
    spec.loader.exec_module(mod)
    return mod


# Sampled graphs are ratio-matched to full-graph LLC pressure where possible.
# LLC sizes give each cell a property working set about 2x the LLC, so the
# replacement policy is actually exercised. Sampled vertex counts are 65,536 /
# 65,536 / 262,144, i.e. 256 kB / 256 kB / 1 MB of 4-byte property.
#
# This is deliberately NOT the "ratio-matched to full-graph pressure" sizing.
# Matching web-Google's full-graph 8 MB pressure (0.44x) makes the property
# array FIT the LLC, and every policy then returns byte-identical traffic: a
# cell with no signal at all. Ratio matching is right for a timing matrix that
# must reproduce full-graph behaviour; it is wrong for a direction check, which
# needs cells that can distinguish policies.
TIERS = {
    "fast": [
        ("web-Google-n16", "results/graphs/web-Google-n16/web-Google-n16.sg", "128kB"),
        ("soc-pokec-n16", "results/graphs/soc-pokec-n16/soc-pokec-n16.sg", "128kB"),
        ("cit-Patents-n18", "results/graphs/cit-Patents-n18/cit-Patents-n18.sg", "512kB"),
    ],
    "full": [
        ("web-Google", "results/graphs/web-Google/web-Google.sg", "2MB"),
        ("soc-pokec", "results/graphs/soc-pokec/soc-pokec.sg", "4MB"),
        ("cit-Patents", "results/graphs/cit-Patents/cit-Patents.sg", "8MB"),
    ],
}

DEFAULT_KERNELS = ["pr", "bfs", "sssp", "bc", "cc"]
DEFAULT_POLICIES = [
    "LRU", "SRRIP", "GRASP", "POPT", "ECG:K2", "ECG:K2_STREAMSHIELD",
]

# The frozen decision rule.
TIE_BAND = 0.02


def size_bytes(text: str) -> str:
    t = text.strip().lower()
    mult = 1
    if t.endswith("kb"):
        mult, t = 1024, t[:-2]
    elif t.endswith("mb"):
        mult, t = 1024 * 1024, t[:-2]
    return str(int(float(t) * mult))


def build_env(rm, policy_text: str, kernel: str, l3: str, args) -> dict:
    """Reuse the experiment runner's environment construction.

    The namespace comes from roi_matrix's own parser, so every default matches
    the experiment runner and a new option cannot silently diverge here.
    """
    spec = rm.parse_policy_spec(policy_text)
    # The private-cache sizes MUST be set explicitly. roi_matrix's bare defaults
    # are 1 kB L1 and 2 kB L2, sized for smoke tests, and with a hierarchy that
    # small almost every access reaches the LLC: web-Google-n16 PageRank LRU
    # traffic was 297,710 against 128,970 with a realistic 32 kB / 256 kB
    # hierarchy, a 2.3x inflation that changes what the LLC policy is even being
    # asked to do.
    ns = rm.parse_args([
        "--suite", "cache-sim",
        "--benchmark", kernel,
        "--l1d-size", args.l1d_size,
        "--l2-size", args.l2_size,
        "--prefetcher", args.prefetcher,
        "--stream-prefetch-model", args.stream_prefetch_model,
        "--popt-matrix-stream", args.popt_matrix_stream,
        "--structural-bypass", args.structural_bypass,
    ])
    if args.prefetch_degree:
        ns.structure_prefetch_degree = args.prefetch_degree
        ns.cache_stream_prefetch_degree = args.prefetch_degree
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        json_path = Path(tmp.name)
    env = rm.cache_sim_env(ns, spec, size_bytes(l3), "16", json_path)
    env["OMP_NUM_THREADS"] = "1"
    # Epoch resolution decides whether the per-edge record packs into 4 bytes or
    # spills to 8, and that single bit of configuration dominates every K2
    # result. The pinned specs hardcode 65535 epochs, which always spills.
    if args.ecg_epochs and "ECG_EDGE_MASK_EPOCHS" in env:
        env["ECG_EDGE_MASK_EPOCHS"] = str(args.ecg_epochs)
    # Schedule-2 historically returned 8 bytes unconditionally instead of
    # computing its width, which doubled K2's modelled transport whenever its
    # fields would in fact have fitted in 4.
    if args.variable_record_width:
        env["ECG_RECORD_VARIABLE_WIDTH"] = "1"
    if args.tier_bits is not None:
        env["ECG_RECORD_TIER_BITS"] = str(args.tier_bits)
    if args.prefetch_degree:
        env["CACHE_STREAM_PREFETCH_DEGREE"] = str(args.prefetch_degree)
    return env, json_path, spec


def run_cell(rm, kernel, graph_path, l3, policy_text, args):
    binary = BIN / kernel
    if not binary.exists():
        return None, f"missing binary {binary}"
    env, json_path, spec = build_env(rm, policy_text, kernel, l3, args)
    # Disable ASLR, exactly as roi_matrix does. cache_sim tracks REAL pointers
    # for the CSR and property arrays, so address-space randomisation changes
    # cache set mapping and makes results vary run to run by ~0.07%. That is the
    # same order as several differences reported in this study, so without this
    # the harness cannot distinguish a small effect from placement noise.
    cmd = ["/usr/bin/setarch", "x86_64", "-R",
           str(binary), "-f", str(ROOT / graph_path), "-n", "1"]
    if kernel == "pr":
        cmd += ["-i", str(args.iterations)]
    if args.reorder:
        cmd += ["-o", str(args.reorder)]
    started = time.time()
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=args.timeout)
    except subprocess.TimeoutExpired:
        json_path.unlink(missing_ok=True)
        return None, "timeout"
    elapsed = time.time() - started
    if proc.returncode != 0:
        json_path.unlink(missing_ok=True)
        tail = (proc.stderr or "").strip().splitlines()[-1:] or [""]
        return None, f"exit {proc.returncode}: {tail[0][:120]}"
    if not json_path.exists():
        return None, "no stats json"
    data = json.loads(json_path.read_text())
    json_path.unlink(missing_ok=True)
    return {
        "policy": spec.label,
        "traffic": data.get("total_memory_traffic"),
        "demand": (data.get("L3") or {}).get("misses"),
        "fills": data.get("prefetch_fills"),
        "prefetch_model": data.get("stream_prefetch_model"),
        "popt_stream_columns": data.get("popt_matrix_stream_columns_simulated"),
        "seconds": round(elapsed, 2),
    }, None


def geomean(values):
    vals = [v for v in values if v and v > 0]
    if not vals:
        return None
    return math.exp(sum(math.log(v) for v in vals) / len(vals))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tier", choices=sorted(TIERS), default="fast")
    ap.add_argument("--kernels", default=",".join(DEFAULT_KERNELS))
    ap.add_argument("--policies", default=",".join(DEFAULT_POLICIES))
    ap.add_argument("--baseline", default="LRU",
                    help="policy every ratio is taken against")
    ap.add_argument("--metric", choices=["traffic", "demand"], default="traffic",
                    help="frozen primary is traffic; demand is secondary only")
    ap.add_argument("--iterations", type=int, default=2)
    ap.add_argument("--reorder", type=int, default=5)
    ap.add_argument("--prefetcher", default="none")
    ap.add_argument("--prefetch-degree", type=int, default=0)
    ap.add_argument("--stream-prefetch-model", choices=["stride", "oracle"],
                    default="stride")
    ap.add_argument("--popt-matrix-stream", choices=["analytic", "simulated"],
                    default="simulated")
    ap.add_argument("--structural-bypass", choices=["off", "all"], default="all")
    ap.add_argument("--ecg-epochs", type=int, default=0,
                    help="override ECG_EDGE_MASK_EPOCHS for every ECG policy. "
                         "The pinned specs use 65535, which forces a 16-bit "
                         "epoch field and an 8-byte record; 4096 or fewer packs "
                         "the record into 4 bytes on a 16-bit-id graph.")
    ap.add_argument("--l1d-size", default="32kB")
    ap.add_argument("--l2-size", default="256kB")
    ap.add_argument("--variable-record-width", action="store_true",
                    help="compute the Schedule-2 record width from the bit "
                         "budget instead of returning 8 bytes unconditionally")
    ap.add_argument("--tier-bits", type=int, default=None,
                    help="override ECG_RECORD_TIER_BITS (transport width only; "
                         "does NOT disable the GRASP tier mechanism)")
    ap.add_argument("--min-activity", type=int, default=10000,
                    help="minimum baseline metric value for a cell to count; "
                         "below this, ratios are noise and one cell can "
                         "dominate the geometric mean")
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--out", default="")
    args = ap.parse_args(argv)

    if (args.metric == "demand" and
            (args.prefetcher != "none" or args.prefetch_degree)):
        raise SystemExit(
            "demand misses may not carry a comparison while a prefetcher is "
            "active (see wiki/Evaluation-Methodology.md)")

    rm = load_roi_matrix()
    kernels = [k.strip() for k in args.kernels.split(",") if k.strip()]
    policies = [p.strip() for p in args.policies.split(",") if p.strip()]
    graphs = TIERS[args.tier]

    rows = []
    total = len(graphs) * len(kernels) * len(policies)
    done = 0
    print(f"[signal] tier={args.tier} cells={total} metric={args.metric} "
          f"bypass={args.structural_bypass} pf={args.stream_prefetch_model}",
          flush=True)
    for gname, gpath, l3 in graphs:
        if not (ROOT / gpath).exists():
            print(f"[skip] {gname}: missing {gpath}", flush=True)
            continue
        for kernel in kernels:
            for policy in policies:
                done += 1
                res, err = run_cell(rm, kernel, gpath, l3, policy, args)
                if err:
                    print(f"  [{done}/{total}] {gname}/{kernel}/{policy}: {err}",
                          flush=True)
                    continue
                res.update(graph=gname, kernel=kernel, l3=l3,
                           policy_text=policy)
                rows.append(res)
                print(f"  [{done}/{total}] {gname}/{kernel}/{res['policy']}: "
                      f"{args.metric}={res[args.metric]} ({res['seconds']}s)",
                      flush=True)

    if not rows:
        raise SystemExit("no cells completed")

    base_label = rm.parse_policy_spec(args.baseline).label
    by_cell = {}
    for r in rows:
        by_cell.setdefault((r["graph"], r["kernel"]), {})[r["policy"]] = r

    # Degenerate-cell guard. When the property working set fits the LLC, every
    # policy returns byte-identical traffic. Such a cell carries no policy
    # signal, but it silently pulls every geomean towards 1.000 and inflates the
    # tie count, which would make a real difference look smaller than it is.
    ratios = {}
    degenerate = []
    for cell, per_policy in sorted(by_cell.items()):
        base = per_policy.get(base_label)
        if not base or not base[args.metric]:
            continue
        values = {r[args.metric] for r in per_policy.values() if r[args.metric]}
        if len(per_policy) > 1 and len(values) == 1:
            degenerate.append((cell, f"identical {args.metric} across policies"))
            continue
        # Minimum-activity guard. A cell where the baseline moves only a
        # handful of lines produces ratios like 5/1 that are noise, not signal,
        # and a single such cell can dominate a geometric mean. Observed:
        # cit-Patents-n18 has average degree 1, so its BFS cell had an LRU
        # traffic of ONE line and a 5.000 ratio that moved GRASP's geomean from
        # a clear win to an apparent loss.
        if base[args.metric] < args.min_activity:
            degenerate.append(
                (cell, f"baseline {args.metric}={base[args.metric]} below "
                       f"--min-activity {args.min_activity}"))
            continue
        for label, r in per_policy.items():
            if not r[args.metric]:
                continue
            ratios.setdefault(label, []).append(
                (cell, r[args.metric] / base[args.metric]))

    if degenerate:
        print()
        print(f"EXCLUDED {len(degenerate)} cell(s) carrying no usable policy signal:")
        for (g, k), why in degenerate:
            print(f"  - {g}/{k}: {why}")
    if not ratios:
        raise SystemExit(
            "every cell was degenerate; shrink the LLC so the property working "
            "set does not fit")

    print()
    print(f"=== frozen decision rule: {args.metric} vs {base_label} "
          f"(geomean, +/-{TIE_BAND:.0%} tie band) ===")
    print(f"{'policy':<28} {'geomean':>8} {'W':>4} {'T':>4} {'L':>4}  worst cell")
    summary = []
    for label, pairs in sorted(ratios.items(),
                               key=lambda kv: geomean([r for _, r in kv[1]]) or 9):
        vals = [r for _, r in pairs]
        gm = geomean(vals)
        wins = sum(1 for v in vals if v < 1 - TIE_BAND)
        losses = sum(1 for v in vals if v > 1 + TIE_BAND)
        ties = len(vals) - wins - losses
        worst_cell, worst = max(pairs, key=lambda p: p[1])
        summary.append((label, gm, wins, ties, losses, worst_cell, worst))
        print(f"{label:<28} {gm:>8.3f} {wins:>4} {ties:>4} {losses:>4}  "
              f"{worst_cell[0]}/{worst_cell[1]} {worst:.3f}")

    best = min(summary, key=lambda s: s[1])
    k2 = [s for s in summary if s[0].startswith("ECG_K2")]
    print()
    print(f"best policy: {best[0]} at {best[1]:.3f}")
    if k2:
        best_k2 = min(k2, key=lambda s: s[1])
        if best_k2[0] == best[0]:
            print(f"K2 LEADS: {best_k2[0]} is the best policy on this matrix.")
        else:
            gap = (best_k2[1] / best[1] - 1) * 100
            print(f"K2 TRAILS: best K2 variant {best_k2[0]} at {best_k2[1]:.3f} "
                  f"is {gap:+.1f}% behind {best[0]}.")

    out = Path(args.out) if args.out else (
        ROOT / f"results/ecg_experiments/fast_signal_{args.tier}_{int(time.time())}.csv")
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=sorted(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"\nrows: {out}")
    print("NOTE: direction check only. Not publishable numbers: no gem5 timing, "
          "no evidence archive, cache_sim prefetch fills are synchronous.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
