# Evaluation Methodology

This page defines how K2 and StreamShield are evaluated. It contains no
performance results.

## Simulator roles

| Simulator | Use |
|---|---|
| **gem5 O3** | architectural execution time and request-bound K2-M behavior |
| **cache_sim** | functional replacement behavior and total memory traffic |
| **Sniper** | larger-scale cache and traffic trends |

Only gem5 O3 execution time is used for architectural speedup. cache_sim does
not model cycles or instructions. Sniper uses a coarser delivery model than
gem5's request-bound O3 path, so its time is not used as a K2-M speedup.

Absolute miss rates are not compared across simulators. Each simulator is
compared with its own matching baseline.

## PageRank study

The deterministic PageRank study uses samples of web-Google, soc-pokec, and
cit-Patents. Iteration counts are 1, 2, 4, and 8. Exact graph sizes, checksums,
cache geometries, and command lines are specified in
[`pagerank_study.json`](https://github.com/UVA-LavaLab/ECG_GrAPL/blob/main/scripts/experiments/ecg/configs/pagerank_study.json).

The policy set contains:

- LRU;
- GRASP;
- capacity- and traffic-charged P-OPT;
- an uncharged P-OPT control;
- K2 with an LRU replacement control;
- static RRIP-first K2 with StreamShield; and
- online K2 with StreamShield.

## Primary quantities

The primary quantities are always reported together:

1. gem5 O3 execution time; and
2. total off-chip traffic, including demand, prefetch, metadata, writeback,
   and modeled reference-structure traffic.

Per-cell ratios are aggregated with the geometric mean. A +/-2% interval is
used when classifying a per-cell ratio as approximately equal.

Every comparison uses a matching baseline from the same invocation and build.
Rows marked `timing_valid_for_speedup=0` are excluded from timing ratios.

## Prefetching

With a prefetcher enabled, demand misses alone are not performance evidence:
prefetching may move traffic from demand requests to prefetch requests.
Execution time and total off-chip traffic remain the primary quantities.

An idealized mechanism with perfect prediction or unlimited latency,
bandwidth, queue, or MSHR resources is reported as an upper bound rather than
as measured hardware performance.

## Instruction-count interpretation

Complete-design comparisons include record layout, transport, ISA, placement,
and replacement. Their instruction counts may differ, so time, traffic, and
retired instructions must be reported together.

Replacement-only attribution uses K2 RRIP-first plus StreamShield versus K2
LRU plus StreamShield. These configurations share the record layout, delivery
path, ISA, and instruction count. Exact per-cell instruction equality is
required for this attribution.

IPC is derived from instruction count and execution time; it is not an
independent corroborating quantity. Counterfactual instruction normalization
is a sensitivity study, not a measured result.

## P-OPT accounting

P-OPT is charged for reserved LLC capacity and cumulative matrix traffic. The
current analytic mode sets `popt_target_time_charged=0`, so matrix-stream
latency is omitted. Timing from this mode is an optimistic P-OPT bound and
must not be presented as a realistic target-time implementation.

Final P-OPT reference comparisons are limited to PageRank and Connected
Components. BFS and SSSP comparisons are project extensions: their
frontier-driven traversal does not satisfy the monotonic sweep-order epoch
assumption used by the reference rereference matrix, so they remain
diagnostic rather than final reference rows.

## Final campaign roles

The final campaign separates simulator responsibilities:

- **gem5 O3:** compact 4-byte, 32-epoch PageRank timing only;
- **cache_sim:** full-graph all-kernel replacement and traffic;
- **Sniper:** full-graph equal-semantic-work cache/traffic corroboration.

The full-graph cache_sim primary uses a 4-byte record with 16 epochs, which
fits all three graphs for PR, BFS, BC, and CC. Weighted SSSP uses its
implemented 8-byte replacement record. Two wide-record controls isolate the
cost of record width and the effect of increasing K2 resolution from 16 to
256 epochs.

P-OPT reference rows are limited to PageRank and Connected Components. They
compare directly with the compact 4-byte/16-epoch K2 primary in the same cell
and pin property width, resident columns, P-OPT's 256 epochs, minimum data
ways, and simulated matrix streaming. A separate wide K2/256-epoch control
shows the sensitivity to epoch resolution. gem5 P-OPT time still omits
matrix-stream latency and therefore remains an optimistic bound.

Sniper runs are bounded by one full serialized edge sweep per graph, use equal
semantic edge visits across policies, and are excluded from speedup reporting.

The compact cit-Patents encoding uses all 32 available bits. Any wider
identifier or additional record field requires the 8-byte fallback.

## Publication policy

Preliminary numbers and intermediate experiment decisions are not published
in the README or wiki. Tables and result figures will be added only after the
final evaluation campaign is complete.
