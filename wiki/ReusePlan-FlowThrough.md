# ReusePlan and FlowThrough: Architecture Guide

Graph kernels stream structural records and then use each adjacency entry to
read a vertex property such as score, parent, distance, depth, or component
label. The structural stream is regular; the property address is irregular.
ReusePlan carries graph-derived guidance from the structural access to the
consuming property Request, ReuseBind is the Request extension that transports
that metadata, and FlowThrough is a separate LLC fill-allocation decision.

This page specifies construction, record format, victim selection, and
placement semantics. Instruction and MSHR details are specified in the
[RISC-V instruction path](RISC-V-Instruction-Path).
It contains no experimental results.

## Graph terminology

Let `G = (V,E)` be directed, with out-neighbors `N_out(u)`, in-neighbors
`N_in(u)`, out-degree `d_out(u) = |N_out(u)|`, and in-degree
`d_in(u) = |N_in(u)|`. The **outer vertex** is the vertex indexed by the outer
loop and therefore the owner of the active CSR row.

- In an out-neighbor traversal, outer vertex `u` iterates over CSR row `u`,
  whose `row_ptr[u]..row_ptr[u+1]` entries in `col_idx` enumerate `N_out(u)`.
  Each adjacency entry `(u,v)` reads property value `p[v]`. For a fixed
  property vertex `v`, the consuming outer vertices are `N_in(v)`, so the
  property-request count is `d_in(v)` and the access count is `d_in(v)`.
- In an in-neighbor (pull) traversal, outer vertex `u` iterates over CSR row
  `u`, whose entries enumerate `N_in(u)`. Each adjacency entry `(u,v)` reads
  property value `p[v]`. For a fixed property vertex `v`, the consuming outer
  vertices are `N_out(v)`, so the property-request count is `d_out(v)` and the
  access count is `d_out(v)`.

For each property vertex, the offline builder materializes the sorted outer
vertices that read it. The figure fixture is undirected, so its in-neighbor and
out-neighbor sets are equal; the worked example uses an out-neighbor traversal
orientation.

## 1. Offline construction and the ROI boundary

### Figure 1 — Constructing an edge-aligned ReusePlan

![Three-panel derivation of a fixture graph row, degree-derived property tier, subsequent property-line accesses, and compact ReusePlan packing](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg)

**Figure 1.** Every displayed value is derived from
`fig/ecg-figure-fixture.json`.

The builder is kernel-phase aware:

- PageRank records follow the in-neighbor traversal.
- BFS, SSSP, BC forward, and CC records follow the out-neighbor traversal.
- BC backward uses its runtime successor DAG rather than a static edge-aligned
  record stream.
- Edge-aligned record offsets must match the corresponding canonical CSR row
  offsets.

Vertices are ranked by property-request count: `d_in(v)` for an out-neighbor
traversal and `d_out(v)` for an in-neighbor traversal. With the default hot
fraction `f = 0.15`, the first `floor(f|V|)` vertices in stable descending
count order receive tier 1, the next equally sized group tier 2, and the rest
tier 3. Vertex ID breaks equal-count ties. A property block receives the
hottest tier among its vertices.

For each adjacency entry in outer vertex row `u`, the builder searches the
sorted outer vertex lists strictly after `u`. It selects the next two
accesses to any property vertex in the same property block, wraps to the next
ID-ordered sweep when necessary, preserves additional same-row accesses to that
block, and quantizes the selected outer vertex IDs into the configured epoch
range.

Record construction is preprocessing, not measured graph execution. Sidecar
headers bind the graph, traversal, runtime width configuration, ordering,
record count, and payload. The guest validates these facts before the ROI and
aborts on disagreement.

## 2. Record formats and traffic

### Figure 2 — ReusePlan record formats and structural traffic

![Bit-level general and compact ReusePlan layouts plus the two weighted SSSP transport choices and their simulated byte footprints](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg)

**Figure 2.** Format selection changes the materialized structural data
footprint and must agree with the runtime width configuration receipt.

The general unweighted record is fixed:

```text
destination[32] | tier[2] | epoch1[15] | epoch2[15]
```

The compact record is graph-specific:

```text
id_bits + 2 + 2*epoch_bits <= 32
```

Unused upper compact bits are zero/reserved; they are not alignment bits. The
record-load execution helper widens a compact value into the canonical 64-bit
metadata layout using the configured record-format CSR.

The exploratory finite-horizon successor uses a different compact record:

```text
destination[id_bits] | absolute-next-use[8] | state[2]
```

For an n18 graph this consumes 28 bits; an n22 graph uses exactly 32. The state
distinguishes unknown, finite in the current sweep, known dead, and wrap to the
next sweep. A wrap is expanded to the following iteration at runtime and
becomes dead only in the final iteration. The record replaces the ordinary
four-byte destination entry; it is not a sidecar and does not require
FlowThrough.

Because dead/wrap state is defined against a finite iteration horizon,
next-use-record runs require convergence stopping to be disabled (`-t 0`).
Otherwise the executed final iteration could differ from the horizon encoded
offline, so both cache_sim and gem5 fail before the ROI.

Weighted SSSP has two supported transport formats:

- a compact 64-bit substitute with 24-bit destination, 8-bit positive weight,
  tier, and two 15-bit epochs; or
- the ordinary weighted edge plus a 32-bit tier/epoch sidecar.

The latter costs edge bytes plus four metadata bytes. A comparison must not
erase the weight or sidecar traffic.

## 3. Future distance

### Figure 3 — Quantized next-reference distance for one property line

![Fixture property-line schedule showing current outer vertex 8, subsequent outer vertices 11 and 15, circular distance, and RRIP-first interpretation](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg)

**Figure 3.** In the checked out-neighbor traversal, the property block
containing internal vertices 18 and 20 is accessed from outer vertices
`1, 6, 8, 11, 15, 18, 20`, in ID order.

For epoch `e`, current epoch `c`, and epoch count `N`:

```text
distance(e,c) = (e + N - (c mod N)) mod N
ReusePlan distance = min(distance(epoch1,c), distance(epoch2,c))
```

Malformed or out-of-range epoch values are clamped before use. An unstamped
property line has effective distance zero, so only a live stamp participates in
future-distance ranking. A payload count of one preserves single-epoch
behavior; the minimum of two distances is used only when two epochs are live.

For the checked adjacency entry at outer vertex/current epoch 8,
`epoch1=11` and `epoch2=15`. The two distances are 3 and 7, so the line's
effective ReusePlan distance is 3.

## 4. LLC state and victim selection

### Figure 4 — ReuseBind acceptance and RRIP-first victim selection

![Three-panel LLC architecture showing ReuseBind acceptance, line-local metadata, RRIP eligibility, structural preference, and epoch ranking](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg)

**Figure 4.** Request acceptance, metadata lifetime, insertion/refresh, and
victim selection are distinct state transitions.

An LLC hit or fill accepts ReuseBind only when the extension is valid, the
execution context is nonzero, and the destination vertex maps to the accessed
property block. Conflicted or mismatched metadata never becomes a live line
stamp. Invalidation clears the tier, epochs, context, count, and stamp-valid
state.

The default static rule is RRIP-first (`rrip_first` in configuration):

1. age until at least one way reaches `rrpvMax`;
2. within that eligible set, select the oldest structural/non-property line;
3. if only property lines remain, select the farthest effective ReusePlan
   distance; and
4. use stable set order for a remaining tie.

This is the shared `ecg_policy::selectVictim` decision used by the cache_sim,
gem5, and Sniper adapters. Delivery, cache populations, timing, and native line
state still differ across the simulators.

`grasp_only`, `epoch_first`, `degree_first`, `shortcircuit`, `lru_only`, and
the no-epoch controls are explicit ablations. The two online-selector
generations did not satisfy the retained representativeness and regret checks.
They remain admission diagnostics and are not used as gem5 or Sniper
performance policies. Admission diagnostics are separate from victim selection.

`next_use_lru` is an exploratory successor implemented in cache_sim and as a
gem5 request-bound mechanism. It evicts a known-dead governed property line
first. Otherwise it preserves global LRU for non-property and unknown lines,
and refines the victim only when global LRU already selected a finite-use
governed property line. A finite bucket that has passed becomes dead only when
every property access is guaranteed to refresh LLC metadata; without that
guarantee it becomes unknown, preventing stale private-cache hits from creating
false-dead evictions. The initial gem5 path software-decodes the compact record
before the existing ReuseBind property load, so its cache metrics are useful
but its timing is not admissible.

## 5. FlowThrough cache behavior

### Figure 5 — FlowThrough lookup, service, and LLC fill allocation

![Detailed FlowThrough hierarchy showing ordinary hits, all-no-allocate misses, mixed MSHR targets, derived-prefetch classification, and normal property requests](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-flowthrough-outcomes.svg)

**Figure 5.** The no-allocate decision is target-level and combines safely
when several requests share an MSHR.

FlowThrough preserves:

- normal address translation and load ordering;
- L1D, L2, and LLC lookup;
- all cache hits;
- memory miss service and response;
- private-cache fills; and
- architectural writeback and in-order retirement.

On an LLC miss, a FlowThrough MSHR target contributes `allocOnFill=false`.
gem5's MSHR target list combines allocation requirements with logical OR.
Therefore:

- an MSHR containing only no-allocate targets skips the LLC fill; but
- a mixed MSHR still allocates when any coalesced target requires the line.

A derived prefetch receives structural FlowThrough only when its own target
address remains inside the active structural-carrier region. The property
Request is never assigned FlowThrough.

## 6. Design mechanism and matched structural-array control

### Figure 6 — FlowThrough mechanism and matched structural-array control

![Comparison of request-specific ReusePlan FlowThrough and the policy-independent structural fairness control across baseline CSR and packed substitute carriers](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-structural-fairness.svg)

**Figure 6.** The two switches answer different experimental questions.

`ECG_FLOWTHROUGH` is the design mechanism attached by `ecg.flow.load*` to a
ReusePlan record Request. It may also use the separate adaptive placement
diagnostic.

`--flowthrough all` is the matched control. It enables
`STRUCTURAL_FLOWTHROUGH` and records the active structural-array region for
every policy:

- CSR for LRU, GRASP, and P-OPT;
- the packed substitute when ReusePlan replaces CSR at runtime; or
- CSR when a candidate record path falls back.

cache_sim requires positive structural accesses, gem5 positive no-allocate miss
targets, and Sniper positive structural read/fill events. Sniper rejects
translated-address mode because the current public ranges are virtual.

Neither switch changes structural data footprint, hides latency, or proves a
better victim policy.

## 7. Instruction crosswalk

The cache mechanisms are reached through four experimental instruction roles:

- `ecg.plan.load` and the weighted Plan form load general/sidecar records
  with ordinary placement; there is no compact Plan-load encoding;
- `ecg.flow.load*` loads general, compact, or weighted record data with
  FlowThrough placement;
- `ecg.bind.load.*` binds a plan to a computed-address property load; and
- `ecg.bind.iload.*` combines indexed address generation with binding.

Their operands, O3 stages, Request extension, and MSHR rules are specified in
[RISC-V instruction path](RISC-V-Instruction-Path).

## 8. Hardware and evidence boundaries

ReusePlan does not reserve LLC data ways, but it adds metadata storage and
preprocessing cost. Property lines that carry ReuseBind state store tier,
epochs, count/validity, context, and ordinary replacement state. Requests and
MSHRs carry additional control state. Physical area/energy, metadata overhead,
and preprocessing cost must be reported separately from structural data
footprint and capacity.

Only gem5 O3 timing is architectural speedup evidence. cache_sim supplies
functional cache/traffic evidence; Sniper supplies matched-work modeled
cache/traffic evidence. Analytic P-OPT timing is an optimistic lower bound
because target-time lookup latency, matrix-stream latency, bandwidth, queueing,
and contention are omitted.

Continue with [Evaluation methodology](Evaluation-Methodology) and
[Build and reproduction](Reproduction).
