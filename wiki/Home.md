<p align="center">
  <img src="assets/logo.png" alt="ECG graph logo" width="180">
</p>

# ECG Next Documentation

ECG Next is an experimental cache architecture for irregular graph-property
loads. **ReusePlan** is the offline edge-aligned record derived from graph
structure, **ReuseBind** attaches that metadata to the consuming property
Request, and **FlowThrough** suppresses LLC fill allocation for eligible
structural misses. These mechanisms do not change graph results or property
values.

### Figure 1 — ECG dataflow from graph preprocessing to LLC replacement

![System overview tracing immutable graph-derived records through the two-load RISC-V path into validated LLC replacement state and bounded simulator evidence](../fig/wiki/home/home-f01-system-overview.svg)

**Figure 1.** The diagram separates offline graph analysis, dynamic
instructions, Request and cache state, victim selection, and evaluation scope.
Graph direction is kernel-phase specific: PageRank uses in-neighbor rows;
BFS, SSSP, BC forward, and CC use out-neighbor rows. BC backward traverses its
runtime successor DAG rather than a static edge-aligned record array.

## Documentation structure

1. [ReusePlan and FlowThrough](ReusePlan-FlowThrough) defines construction,
   record formats, victim selection, FlowThrough outcomes, and the
   matched structural-array control.
2. [RISC-V instruction path](RISC-V-Instruction-Path) follows the record and
   property instructions through frontend decode, rename, LSQ Request
   construction, MSHR target merge, writeback, and in-order retirement.
3. [End-to-end property Request example](Property-to-Cache-Walkthrough) derives
   one fixture adjacency entry and identifies its processor and cache state.
4. [Evaluation methodology](Evaluation-Methodology) states what gem5,
   cache_sim, Sniper, and analytic P-OPT can establish.
5. [Build and reproduction](Reproduction) provides graph preparation, build,
   test, and experiment commands.

The figures describe implemented or explicitly modeled architecture. They do
not report performance measurements.
