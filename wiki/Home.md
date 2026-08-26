<p align="center">
  <img src="assets/logo.png" alt="ECG graph logo" width="180">
</p>

# ECG Next Documentation

ECG Next is an experimental cache architecture for irregular graph-property
loads. It combines an offline ReusePlan, explicit request binding, and
FlowThrough placement without changing graph results or property values.

### Figure 1 — ECG Next: offline guidance to request-bound LLC state

![System overview tracing immutable graph-derived records through the two-load RISC-V path into validated LLC replacement state and bounded simulator evidence](../fig/wiki/home/home-f01-system-overview.svg)

**Figure 1.** The diagram separates offline graph analysis, dynamic
instructions, request and cache state, victim selection, and evaluation scope.
Graph direction is explicit: PageRank uses incoming-neighbor rows, whereas the
implemented BFS, BC, CC, and SSSP paths use outgoing-neighbor rows.

## Documentation structure

1. [ReusePlan and FlowThrough](ReusePlan-FlowThrough) defines construction,
   record formats, victim ordering, FlowThrough outcomes, and symmetric
   structural fairness.
2. [RISC-V instruction path](RISC-V-Instruction-Path) follows the record and
   property instructions through gem5 O3, Request construction, MSHR merging,
   completion, and retirement.
3. [End-to-end property-access example](Property-to-Cache-Walkthrough) derives
   one checked adjacency entry and identifies every associated processor and
   cache state field.
4. [Evaluation methodology](Evaluation-Methodology) states what gem5,
   cache_sim, Sniper, P-OPT, and semantic receipts can establish.
5. [Build and reproduction](Reproduction) provides graph preparation, build,
   test, and experiment commands.

The figures describe implemented or explicitly modeled architecture. They do
not report performance measurements.
