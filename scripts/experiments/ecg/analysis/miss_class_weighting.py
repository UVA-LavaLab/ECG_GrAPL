#!/usr/bin/env python3
"""Weight irregular property misses against sequential structural misses.

The default cache_sim metric counts LLC misses, which implicitly assumes every
miss costs the same. That is not true for graph workloads:

  * structural (edge-stream) misses are sequential. A stride prefetcher predicts
    them, they sustain high memory-level parallelism, and they tend to hit an
    open DRAM row. Their exposed latency is largely hidden; what remains is
    bandwidth.
  * property misses are irregular and data dependent. They cannot be prefetched,
    they frequently sit on a dependent chain, and they tend to cause DRAM row
    conflicts. Their latency is largely exposed.

So a single miss count understates policies that trade irregular misses for
sequential ones -- which is exactly what K2 does when it spends edge-record
bandwidth to protect property lines.

This tool re-scores a matrix under

    cost = w * property_misses + structural_misses

and reports, for each policy pair, the crossover w at which the challenger
overtakes the incumbent. It does not assert a value for w: w is a property of
the machine (prefetcher effectiveness, MLP, DRAM row locality, and above all
whether the workload is bandwidth saturated). w -> 1 when bandwidth is the
binding constraint, and grows as the regime becomes latency bound. Use it to
turn "is the structural stream cheap?" into a concrete, testable threshold that
a timing simulator can confirm or refute.
"""

from __future__ import annotations

import argparse
import csv
import glob
import math
import re
from collections import defaultdict
from pathlib import Path

DEFAULT_POLICIES = (
    "GRASP", "HAWKEYE_PROXY", "SRRIP", "POPT",
    "ECG_K2", "ECG_K2_ONLINE",
    "ECG_K2_STREAMSHIELD", "ECG_K2_ONLINE_STREAMSHIELD",
)
FLOOR = 1e-9


def graph_of(options: str) -> str:
    match = re.search(r"/graphs/([^/]+)/", options or "")
    return match.group(1) if match else "?"


def load(root: Path):
    prop: dict = defaultdict(dict)
    struct: dict = defaultdict(dict)
    for path in glob.glob(str(root / "matrices/*/*/*/roi_matrix.csv")):
        for row in csv.DictReader(open(path)):
            if row.get("status") != "ok":
                continue
            cell = (graph_of(row.get("options")), row.get("benchmark"))
            policy = row.get("policy_label")
            prop[cell][policy] = float(row.get("l3_prop_misses") or 0)
            struct[cell][policy] = float(row.get("l3_struct_misses") or 0)
    return prop, struct


def weighted_cost(prop, struct, policy: str, w: float, baseline: str) -> float:
    ratios = []
    for cell in prop:
        if policy not in prop[cell] or baseline not in prop[cell]:
            continue
        num = w * prop[cell][policy] + struct[cell][policy]
        den = w * prop[cell][baseline] + struct[cell][baseline]
        if den > 0:
            ratios.append(max(num / den, FLOOR))
    if not ratios:
        return float("nan")
    return math.exp(sum(math.log(r) for r in ratios) / len(ratios))


def crossover(prop, struct, challenger: str, incumbent: str,
              baseline: str, limit: float = 200.0) -> float | None:
    """Smallest w at which challenger costs less than incumbent, else None."""
    lo, hi = 1.0, limit
    if weighted_cost(prop, struct, challenger, lo, baseline) < \
            weighted_cost(prop, struct, incumbent, lo, baseline):
        return 1.0
    if weighted_cost(prop, struct, challenger, hi, baseline) >= \
            weighted_cost(prop, struct, incumbent, hi, baseline):
        return None
    for _ in range(60):
        mid = (lo + hi) / 2
        if weighted_cost(prop, struct, challenger, mid, baseline) < \
                weighted_cost(prop, struct, incumbent, mid, baseline):
            hi = mid
        else:
            lo = mid
    return (lo + hi) / 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", default="LRU")
    parser.add_argument("--policies", nargs="+", default=list(DEFAULT_POLICIES))
    parser.add_argument("--challengers", nargs="+",
                        default=["ECG_K2", "ECG_K2_ONLINE_STREAMSHIELD"])
    parser.add_argument("--weights", nargs="+", type=float,
                        default=[1, 2, 3, 5, 8, 12, 20])
    args = parser.parse_args()

    prop, struct = load(args.root)
    if not prop:
        print(f"[FAIL] no ok rows under {args.root}")
        return 1
    print(f"cells: {len(prop)}   baseline: {args.baseline}")
    print("\nw = cost(irregular property miss) / cost(sequential structural miss)")
    header = f"{'w':>6s}  " + "".join(f"{p[:12]:>13s}" for p in args.policies)
    print(header)
    for w in args.weights:
        costs = {p: weighted_cost(prop, struct, p, w, args.baseline)
                 for p in args.policies}
        print(f"{w:6.1f}  " + "".join(f"{costs[p]:13.3f}" for p in args.policies))

    print("\ncrossover w (challenger overtakes incumbent):")
    for challenger in args.challengers:
        for incumbent in args.policies:
            if incumbent == challenger:
                continue
            w = crossover(prop, struct, challenger, incumbent, args.baseline)
            if w is None:
                print(f"  {challenger:28s} never overtakes {incumbent}")
            elif w <= 1.0:
                print(f"  {challenger:28s} already beats {incumbent}")
            else:
                print(f"  {challenger:28s} overtakes {incumbent:16s} at w = {w:6.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
