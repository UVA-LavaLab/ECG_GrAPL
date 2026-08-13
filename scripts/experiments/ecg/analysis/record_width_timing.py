#!/usr/bin/env python3
"""Read the record-width timing matrix and evaluate the width comparison.

The matrix reports execution time, off-chip bytes and DRAM bus utilisation
together, because K2's trade only resolves when all three are read at once: it
can spend more bandwidth while exposing far fewer demand misses to full DRAM
latency, and which side binds depends on saturation.

Specified before execution in the experiment configuration:
  low utilisation  -> the measured reductions in exposed demand misses should
                      appear as speedup at BOTH record widths
  high utilisation -> the 8-byte width should lose in proportion to its traffic

This deliberately reports the utilisation FIRST, then time against traffic, so
the timing numbers are interpreted rather than merely ranked.

Usage:
  python3 scripts/experiments/ecg/analysis/record_width_timing.py [RUN_DIR]
"""
from __future__ import annotations

import csv
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RUNS = ROOT / "results/ecg_experiments/runs"

# Utilisation below this is treated as "the memory system has headroom".
SATURATION_LOW = 20.0
SATURATION_HIGH = 70.0


def newest_run() -> Path | None:
    candidates = sorted(
        list(RUNS.glob("ecg_record_width_timing_*"))
        + list(RUNS.glob("ecg_isa_decode_matrix_*"))
        + list(RUNS.glob("ecg_fused_compact_matrix_*")),
        key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0] if candidates else None


def num(value):
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def load(run_dir: Path):
    rows = []
    for csv_path in run_dir.rglob("roi_matrix.csv"):
        stage = "?"
        for part in csv_path.parts:
            if part.startswith("31_gem5_record_width"):
                stage = part.rsplit("_", 1)[-1]
            elif re.match(r"^(?:4\d_isa_|5[0-2]_fused_)", part):
                stage = part
        for row in csv.DictReader(csv_path.open()):
            if row.get("status") != "ok":
                continue
            row["_stage"] = stage
            row["_graph"] = csv_path.parent.parent.name
            row["_kernel"] = csv_path.parent.name
            label = row.get("policy_label", "")
            matches = sorted((csv_path.parent / "gem5").glob(
                f"gem5_{row['_kernel']}_{label}_L3*/stats.txt"))
            row["_stats"] = matches[0] if matches else None
            rows.append(row)
    return rows


def geomean(values):
    vals = [v for v in values if v and v > 0]
    return math.exp(sum(math.log(v) for v in vals) / len(vals)) if vals else None


def roi_insts(row):
    """ROI-scoped committed instructions for a cell.

    Prefer the captured column, but fall back to the archived gem5 stats so
    cells produced before the column existed can still be attributed. Note this
    is deliberately NOT simInsts: that counter is not cleared by
    m5_reset_stats, so it includes graph loading and metadata construction and
    reports an impossible IPC above 1 on an in-order core.
    """
    direct = num(row.get("roi_insts"))
    if direct:
        return direct
    stats = row.get("_stats")
    if not stats or not stats.exists():
        return None
    first_dump = stats.read_text().split(
        "---------- Begin Simulation Statistics ----------")[1:2]
    if not first_dump:
        return None
    m = re.search(r"^system\.cpu\.commitStats0\.numInsts\s+(\d+)",
                  first_dump[0], re.M)
    return float(m.group(1)) if m else None



DECODE_STAGES = {
    "40_isa_fused_4b": "fused delivery, compact record",
    "41_isa_fused_8b": "fused delivery, wide record",
    "42_isa_plain_4b_software": "one instruction + software widen, compact",
    "43_isa_plain_4b_hardware": "one instruction (ecg.extract2c), compact",
    "44_isa_plain_8b": "one instruction (ecg.extract2), wide",
    "50_fused_compact_4b": "fused compact K2-I (serialized TimingSimple)",
    "51_fused_software_4b": "fused K2-I after software widening",
    "52_fused_wide_8b": "fused K2-I, wide record",
}


def expected_stage_graphs(run_dir: Path):
    jobs = run_dir / "jobs.csv"
    if not jobs.exists():
        return set()
    expected = set()
    for row in csv.DictReader(jobs.open()):
        out_dir = Path(row.get("out_dir", ""))
        if row.get("kind") != "roi_matrix" or len(out_dir.parts) < 3:
            continue
        expected.add((row.get("stage", "?"), out_dir.parts[-2]))
    return expected


def report_coverage(rows, expected) -> None:
    """A partial matrix must announce itself.

    The loader drops rows whose status is not ok, so a stage that has produced
    only its LRU cell looks like a stage with nothing to say rather than a stage
    still running. Printing a one-graph number under the heading "geomean" then
    invites it to be read as a result over the graph set.
    """
    missing = []
    for stage, graph in sorted(expected):
        got = {r.get("policy_label") for r in rows
               if r["_stage"] == stage and r["_graph"] == graph}
        if "LRU" not in got or "ECG_K2" not in got:
            missing.append(f"{stage}/{graph} (have: {sorted(got) or 'none'})")
    if missing:
        print()
        print("  INCOMPLETE -- these stage/graph cells lack LRU or ECG_K2:")
        for m in missing:
            print(f"    {m}")
        print("  Every figure below covers only the cells that finished.")


def report_decode_matrix(rows, expected=None) -> bool:
    """The decode matrix: what the record costs to MOVE versus to DECODE.

    Each stage carries its own LRU cell, so every ratio is normalised inside
    its own invocation. That matters because nominally identical cells in
    different invocations have been measured up to 1.7% apart in time.
    """
    stages = sorted({r["_stage"] for r in rows} & set(DECODE_STAGES))
    if not stages:
        return False
    print()
    print("=" * 72)
    print("DECODE MATRIX  (record width versus the cost of decoding it)")
    print("=" * 72)
    report_coverage(
        rows,
        expected or {
            (r["_stage"], r["_graph"]) for r in rows
            if r["_stage"] in DECODE_STAGES})

    norm = {}
    raw = {}
    for stage in stages:
        for graph in sorted({r["_graph"] for r in rows if r["_stage"] == stage}):
            cell = {r.get("policy_label"): r for r in rows
                    if r["_stage"] == stage and r["_graph"] == graph}
            base = cell.get("LRU")
            if not base:
                print(f"  {stage}/{graph}: no LRU cell, cannot normalise")
                continue
            bt, bb = num(base.get("sim_ticks")), num(base.get("dram_offchip_bytes"))
            bi = roi_insts(base)
            raw[(stage, graph, "LRU")] = (bi, bb, bt)
            for label, r in cell.items():
                t, b, i = (num(r.get("sim_ticks")),
                           num(r.get("dram_offchip_bytes")), roi_insts(r))
                if not (bt and bb and bi and t and b and i):
                    continue
                raw[(stage, graph, label)] = (i, b, t)
                norm[(stage, graph, label)] = (i / bi, b / bb, t / bt)

    print(f"\n  versus each stage's OWN LRU cell")
    print(f"    {'stage':<26}{'graph':<22}{'policy':<10}"
          f"{'insts':>9}{'traffic':>9}{'time':>8}")
    for (stage, graph, label), (i, b, t) in sorted(norm.items()):
        if label == "LRU":
            continue
        print(f"    {stage:<26}{graph:<22}{label:<10}"
              f"{i:>9.4f}{b:>9.4f}{t:>8.4f}")

    def contrast(a, b, title, note):
        pairs = [(g, norm[(a, g, "ECG_K2")], norm[(b, g, "ECG_K2")])
                 for g in sorted({k[1] for k in norm})
                 if (a, g, "ECG_K2") in norm and (b, g, "ECG_K2") in norm]
        if not pairs:
            return
        print(f"\n  {title}")
        print(f"    {note}")
        print("    estimator: (K2 / own-stage LRU)_A / "
              "(K2 / own-stage LRU)_B")
        drift = []
        for g, x, y in pairs:
            print(f"      {g:<24} insts x{x[0]/y[0]:.3f}  "
                  f"traffic x{x[1]/y[1]:.4f}  time x{x[2]/y[2]:.3f}")
            direct_a = raw[(a, g, "ECG_K2")]
            direct_b = raw[(b, g, "ECG_K2")]
            lru_a = raw[(a, g, "LRU")]
            lru_b = raw[(b, g, "LRU")]
            direct = tuple(
                direct_a[index] / direct_b[index]
                for index in range(3))
            lru_ratio = tuple(
                lru_a[index] / lru_b[index]
                for index in range(3))
            normalized = tuple(
                x[index] / y[index] for index in range(3))
            drift.append(tuple(
                abs(normalized[index] / direct[index] - 1.0)
                for index in range(3)))
            print(f"      {'direct audit':<24} insts x{direct[0]:.6f}  "
                  f"traffic x{direct[1]:.6f}  time x{direct[2]:.6f}; "
                  f"LRU A/B x{lru_ratio[0]:.6f}/"
                  f"{lru_ratio[1]:.6f}/{lru_ratio[2]:.6f}")
        gi = geomean([x[0] / y[0] for _, x, y in pairs])
        gb = geomean([x[1] / y[1] for _, x, y in pairs])
        gt = geomean([x[2] / y[2] for _, x, y in pairs])
        if gi and gb and gt:
            tag = f"geomean n={len(pairs)}"
            print(f"      {tag:<24} insts x{gi:.3f}  "
                  f"traffic x{gb:.4f}  time x{gt:.3f}")
            if len(pairs) < 3:
                print(f"      {'':<24} (partial: {', '.join(g for g, _, _ in pairs)})")
        if drift:
            print("      max estimator drift      "
                  f"insts {max(row[0] for row in drift) * 100:.4f}%  "
                  f"traffic {max(row[1] for row in drift) * 100:.4f}%  "
                  f"time {max(row[2] for row in drift) * 100:.4f}%")

    contrast("42_isa_plain_4b_software", "43_isa_plain_4b_hardware",
             "DECODE: software widen versus ecg.extract2c, identical record",
             "traffic near 1.0 is consistent with a decode-only difference but "
             "does NOT prove one:\n    decode also moves instruction-fetch, "
             "spills and memory ordering, so read the\n    instruction and L1 "
             "access counts alongside it")
    contrast("43_isa_plain_4b_hardware", "44_isa_plain_8b",
             "WIDTH: compact versus wide, BOTH delivered in one instruction",
             "the only contrast here that isolates the container")
    contrast("40_isa_fused_4b", "41_isa_fused_8b",
             "FUSED compact versus wide -- NOT a width-only contrast",
             "the fused load family takes only the 64-bit record, so the "
             "compact arm still widens in software: this is width PLUS decode")
    contrast("51_fused_software_4b", "50_fused_compact_4b",
             "FUSED DECODE: software widen versus compact K2-I",
             "same dedicated loop skeleton and 4-byte record; the compact "
             "instruction removes the guest widen")
    contrast("50_fused_compact_4b", "52_fused_wide_8b",
             "FUSED IMPLEMENTATION: compact K2-I versus wide K2-I",
             "dedicated matched loop skeletons in one build. Traffic prices "
             "the container; time also includes two different custom-op "
             "decoders, and compact decode is modeled as one instruction")

    invalid = {r["_stage"] for r in rows
               if r["_stage"] in DECODE_STAGES
               and str(r.get("timing_valid_for_speedup", "")) == "0"}
    if invalid:
        print(f"\n  NOT SPEEDUP EVIDENCE: {sorted(invalid)} carry")
        print("    timing_valid_for_speedup=0, because the property load is a "
              "separate\n    instruction rather than a fused request-bound "
              "one. Read the instruction\n    counts and the traffic; the "
              "times are context, not a claim.")
    fused_stages = {
        "50_fused_compact_4b", "51_fused_software_4b",
        "52_fused_wide_8b"}
    if set(stages) & fused_stages:
        print("\n  DETAILED-SIMULATOR SCOPE:")
        print("    Scale cells use single-core TimingSimpleCPU serialized mailbox")
        print("    equivalence. Exact per-Request binding is proven separately by")
        print("    the O3 micro-probe. The compact opcode's dynamic shifts/masks")
        print("    are charged as one custom memory instruction, so its latency is")
        print("    an idealized ISA implementation point, not a hardware timing proof.")
    return True


def report_idealised_mechanisms(rows) -> None:
    """Print BEFORE any ratio: an idealised arm cannot support a claim.

    This lived inline in main() and became unreachable the moment the
    decode report returned early, so P-OPT rows were being displayed with
    the caveat that makes them readable silently dropped.
    """
    print()
    print("=" * 72)
    print("1b. MECHANISM CHARGING  (an idealised arm cannot support a claim)")
    print("=" * 72)
    idealised = []
    seen = set()
    for r in rows:
        if r.get("policy_label") != "POPT":
            continue
        if r["_graph"] in seen:
            continue
        mode = r.get("popt_matrix_stream_mode")
        extra = num(r.get("popt_cumulative_stream_bytes")) or 0.0
        if str(mode).startswith("analytic") and extra > 0:
            seen.add(r["_graph"])
            idealised.append((r["_graph"], extra,
                              num(r.get(
                                  "popt_dram_offchip_bytes_without_matrix_stream"))
                              or 0.0))
    if idealised:
        print("  P-OPT: matrix-stream bytes are charged analytically.")
        print("  gem5 reads the matrix from a sideband file, so its column")
        print("  traffic is added to off-chip bytes for every iteration, but")
        print("  it does not contend for bandwidth or add target-time latency.")
        print("  P-OPT timing is therefore an optimistic lower bound.")
        print()
        print(f"    {'graph':<24}{'offchip':>12}{'+matrix':>12}{'understated':>13}")
        for graph, extra, base in idealised:
            if base:
                print(f"    {graph:<24}{base:>12,.0f}{base + extra:>12,.0f}"
                      f"{extra / base * 100:>12.1f}%")
        print()
        print("  => The P-OPT rows below are a conservative timing baseline")
        print("     for K2, not a target-time P-OPT performance claim.")
    else:
        print("  no analytic-only mechanism detected in these rows")


def main(argv):
    run_dir = Path(argv[0]) if argv else newest_run()
    if not run_dir or not run_dir.exists():
        raise SystemExit("no record-width timing run found")
    rows = load(run_dir)
    if not rows:
        raise SystemExit(f"no completed cells yet in {run_dir}")

    print(f"run: {run_dir.name}")
    print(f"completed cells: {len(rows)}")

    if {r["_stage"] for r in rows} & set(DECODE_STAGES):
        # Charging first, then ratios: an idealised arm has to be flagged before
        # its numbers are read, not after them.
        report_idealised_mechanisms(rows)
        report_decode_matrix(rows, expected_stage_graphs(run_dir))
        return 0

    # ---- 1. Saturation, read first -------------------------------------
    utils = [num(r.get("dram_bus_util_pct")) for r in rows]
    utils = [u for u in utils if u is not None]
    print()
    print("=" * 72)
    print("1. DRAM BUS UTILISATION  (read first: it decides how to read the rest)")
    print("=" * 72)
    if utils:
        print(f"  min {min(utils):.2f}%   median {statistics.median(utils):.2f}%"
              f"   max {max(utils):.2f}%")
        peak = num(rows[0].get("dram_peak_bw_mibs"))
        if peak:
            print(f"  peak bandwidth modelled: {peak:.0f} MiB/s")
        if max(utils) < SATURATION_LOW:
            verdict = ("LOW -- the memory system has headroom. Extra metadata "
                       "traffic is close to free; exposed latency is what "
                       "costs. Expected outcome: the exposed-miss "
                       "reduction should appear as speedup at BOTH widths.")
        elif min(utils) > SATURATION_HIGH:
            verdict = ("HIGH -- bandwidth binds. The wider record should lose "
                       "in proportion to its traffic increase.")
        else:
            verdict = ("MIXED -- neither regime dominates; report per-cell and "
                       "do not generalise.")
        print(f"  verdict: {verdict}")

    report_idealised_mechanisms(rows)

    # ---- 2. Time against traffic, per stage ----------------------------
    print()
    print("=" * 72)
    print("2. TIME AND TRAFFIC versus LRU, by record width")
    print("=" * 72)
    by_stage = defaultdict(list)
    for r in rows:
        by_stage[r["_stage"]].append(r)

    for stage in sorted(by_stage):
        cells = defaultdict(dict)
        for r in by_stage[stage]:
            cells[(r["_graph"], r["_kernel"])][r.get("policy_label", "?")] = r
        ratios = defaultdict(lambda: {"time": [], "traffic": [], "insts": []})
        for _, per_policy in cells.items():
            base = per_policy.get("LRU")
            if not base:
                continue
            bt, bb = num(base.get("sim_ticks")), num(base.get("dram_offchip_bytes"))
            bi = roi_insts(base)
            for label, r in per_policy.items():
                t, b = num(r.get("sim_ticks")), num(r.get("dram_offchip_bytes"))
                i = roi_insts(r)
                if bt and t:
                    ratios[label]["time"].append(t / bt)
                if bb and b:
                    ratios[label]["traffic"].append(b / bb)
                if bi and i:
                    ratios[label]["insts"].append(i / bi)
        print(f"\n  stage: {stage}   ({len(cells)} cell(s))")
        print(f"    {'policy':<24}{'time':>9}{'traffic':>10}{'ROI insts':>11}"
              f"{'cells':>7}")
        for label in sorted(ratios, key=lambda k: geomean(ratios[k]["time"]) or 9):
            gt = geomean(ratios[label]["time"])
            gb = geomean(ratios[label]["traffic"])
            gi = geomean(ratios[label]["insts"])
            print(f"    {label:<24}{gt if gt else float('nan'):>9.3f}"
                  f"{gb if gb else float('nan'):>10.3f}"
                  f"{gi if gi else float('nan'):>11.3f}"
                  f"{len(ratios[label]['time']):>7}")
        print("    (ROI insts versus LRU: a policy that executes more "
              "instructions per edge\n     can lose time while winning "
              "traffic, and at low utilisation it usually does.)")

    # ---- 3. The width contrast, matched --------------------------------
    print()
    print("=" * 72)
    print("3. THE WIDTH CONTRAST  (4b versus 8b, record-carrying policies only)")
    print("=" * 72)
    paired = defaultdict(dict)
    for r in rows:
        key = (r["_graph"], r["_kernel"], r.get("policy_label", "?"))
        paired[key][r["_stage"]] = r
    deltas_t, deltas_b, deltas_i = [], [], []
    for (graph, kernel, policy), stages in sorted(paired.items()):
        # Only policies that CARRY a record can show a width effect. LRU, GRASP
        # and P-OPT are identical in both arms by construction, so including
        # them drags every ratio towards 1.000 and hides the contrast.
        if not policy.startswith("ECG"):
            continue
        if "4b" in stages and "8b" in stages:
            t4, t8 = num(stages["4b"].get("sim_ticks")), num(stages["8b"].get("sim_ticks"))
            b4, b8 = (num(stages["4b"].get("dram_offchip_bytes")),
                      num(stages["8b"].get("dram_offchip_bytes")))
            i4, i8 = roi_insts(stages["4b"]), roi_insts(stages["8b"])
            if t4 and t8 and b4 and b8:
                deltas_t.append(t8 / t4)
                deltas_b.append(b8 / b4)
                extra = ""
                if i4 and i8:
                    deltas_i.append(i8 / i4)
                    extra = f"  ROI insts x{i8/i4:.3f}"
                print(f"  {graph}/{kernel}/{policy:<22} "
                      f"time x{t8/t4:.3f}  traffic x{b8/b4:.3f}{extra}")
    if deltas_t:
        gt, gb = geomean(deltas_t), geomean(deltas_b)
        gi = geomean(deltas_i)
        print(f"\n  geomean: widening the record costs x{gt:.3f} time "
              f"for x{gb:.3f} traffic")
        if gi:
            print(f"  the 8-byte arm executes x{gi:.3f} the ROI instructions "
                  "of the compact arm")
            if gi < 0.995:
                print("  -> the arms are NOT matched on work: the compact "
                      "record is decoded in\n     software, so this contrast "
                      "is width PLUS decode, not width alone. Report the\n"
                      "     decode cost explicitly or move it into the ISA.")
        if gb > 1.0:
            print(f"  -> time cost is {(gt-1)/(gb-1)*100:.0f}% of the traffic "
                  "cost, so the extra bytes are "
                  f"{'largely absorbed' if (gt-1) < (gb-1)*0.5 else 'not absorbed'}")
    else:
        print("  (no matched 4b/8b pairs completed yet)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
