# Evaluation Methodology

This page defines what each ECG experiment can establish. It contains no
performance results.

### Figure 1 — Evaluation evidence and admissible claims

![Evidence hierarchy separating gem5 O3 architectural timing, cache_sim functional cache and traffic evidence, Sniper matched-work modeled cache and traffic evidence, row acceptance receipts, and optimistic P-OPT limits](../fig/wiki/evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg)

**Figure 1.** A result row is accepted only after mechanism activity and
semantic output are both verified.

## 1. Simulator roles

| Simulator | Valid use | Explicit limit |
|---|---|---|
| **gem5 O3** | architectural execution time, decoded ISA path, dynamic Request binding, native LSQ/MSHR/cache behavior | sampled graphs and bounded detailed simulation |
| **cache_sim** | shared victim logic, functional cache behavior, prefetch/traffic accounting, large graph sweeps | no cycle or instruction model; native runtime Requests are abstracted |
| **Sniper** | matched-work modeled cache and traffic evidence at larger scale | time is not ReuseBind speedup evidence; delivery and pipeline behavior are modeled |

Only gem5 O3 execution time is used for architectural speedup. cache_sim does
not model cycles or instructions. Sniper time is not used as a ReuseBind
speedup metric. Every simulator is compared with its own same-build, same-cell
baseline; absolute miss rates and timing are not compared across simulators.

The default indexed Sniper ReusePlan path uses per-edge delivery markers.
The computed fused sideband remains diagnostic and rejects source/line cases
whose per-edge hints cannot be represented consistently.

## 2. Fail-closed row acceptance

An experiment row must establish:

1. requested and effective policy/mode agree;
2. the active structural carrier is the one the workload actually consumes;
3. record width, substitution, and traffic fields agree;
4. FlowThrough activity is positive when requested;
5. P-OPT context, matrix, and phase-two queries are active when required; and
6. semantic output agrees across every policy row in the matched group.

If any matched row fails, group timing is invalid. Memory-order violations,
dependency conflicts, and squashes are O3 diagnostics; semantic receipts decide
architectural correctness.

## 3. Structural FlowThrough fairness

The `--flowthrough all` control gives LRU, GRASP, P-OPT, and ReusePlan the same
no-allocate opportunity on their actual structural carrier. It is distinct
from request-specific `ECG_FLOWTHROUGH`.

Receipts are backend-specific:

- cache_sim: positive structural accesses;
- gem5: positive structural no-allocate miss targets; and
- Sniper: positive structural read and fill-write counts.

This control removes a policy-specific structural allocation advantage; it does
not equalize record width, matrix traffic, instruction count, or victim
quality.

## 4. Primary quantities

The primary quantities are always reported together:

1. gem5 O3 execution time;
2. total off-chip traffic, including demand, prefetch, metadata, writeback,
   and modeled reference-structure traffic;
3. retired instructions for complete-design comparisons; and
4. policy/mechanism activity receipts.

Per-cell ratios use a matching baseline from the same invocation and build and
are aggregated with a geometric mean. A +/-2% interval classifies a per-cell
ratio as approximately equal. Rows with `timing_valid_for_speedup=0` are
excluded from timing ratios.

### Prefetching and idealized models

With a prefetcher enabled, demand misses alone are not performance evidence:
prefetching may move traffic from demand to prefetch requests. Execution time
and total off-chip traffic remain primary, and MSHR pressure, bandwidth,
queueing, and overfetch must remain visible.

A mechanism with perfect prediction or unlimited latency, bandwidth, queue, or
MSHR resources is reported as an upper bound rather than as measured hardware
performance.

### Instruction-count interpretation

Complete-design comparisons include record layout, transport, ISA, placement,
and replacement, so time, traffic, and retired instructions are interpreted
together. Replacement-only attribution compares transport-matched ReusePlan
policies and requires exact per-cell instruction equality.

IPC is derived from instructions and time; it is not independent evidence.
Counterfactual instruction normalization is a sensitivity, not a measurement.

## 5. P-OPT accounting

Analytic P-OPT charges reserved LLC capacity and cumulative matrix traffic. It
sets `popt_target_time_charged=0`, so matrix-stream latency is omitted, as are
target-time lookup latency, target-time bandwidth, queueing, and contention.
Its timing is therefore an optimistic P-OPT bound, specifically a lower bound
on time, not a realistic target-time implementation.

The reference matrix assumes an ordered sweep. Final reference rows are
limited to PageRank and Connected Components. BFS and SSSP comparisons are
project extensions: frontier order does not satisfy P-OPT's monotonic
sweep-order epoch assumption, so those rows remain diagnostic.

The two resident columns are current and next. Initial loading belongs to
cumulative stream traffic, not a third resident column.

## 6. Workloads and campaign roles

The literature-scale PageRank screen uses fixed 262,144-vertex samples of
web-Google, Pokec, Patents, roadNet-CA, LiveJournal, and Orkut at iteration
counts 1 and 8. Its hashes, geometries, policy roles, and decision thresholds
are in `pagerank_literature_scale.json`.

The earlier three-graph sensitivity study uses web-Google, soc-pokec, and
cit-Patents. Iteration counts are 1, 2, 4, and 8. Its configuration is
[`pagerank_study.json`](https://github.com/UVA-LavaLab/ECG_GrAPL/blob/main/scripts/experiments/ecg/configs/pagerank_study.json).

The publication corpus must include web-Google, Pokec, Patents, roadNet-CA,
LiveJournal, and Orkut; Twitter-2010 is the cache-only stress case.

- gem5 O3 supplies compact PageRank architectural timing.
- cache_sim supplies full-graph all-kernel replacement and traffic.
- Sniper supplies bounded matched-work cache/traffic corroboration.

Selector generations 1 and 2 did not satisfy the retained representativeness
and regret checks. They are retained as negative diagnostics and must not be
presented as detailed-simulator performance policies.

## 7. Two separate campaigns

The replacement campaign and the transport campaign are preregistered
separately, run separately, and gated separately. Evidence and receipts are
never shared between them.

### 7.1 Replacement campaign

The literature-scale replacement campaign compares ReusePlan victim selection
against SRRIP, GRASP, and P-OPT under `pagerank_literature_scale.json`. Its
screen decision stands at STOP, so it supports no ReusePlan replacement claim
against those baselines. That decision remains the authoritative record for
replacement, and its configuration, thresholds, and stages are frozen. Rows and
receipts produced under earlier commits are not admissible evidence for either
campaign.

### 7.2 Transport campaign

The transport campaign isolates compact ReusePlan record transport and
structural FlowThrough while replacement stays pure LRU. Its configuration is
`transport_literature_scale.json`; its manifest profile is
`reuse_plan_transport_campaign`.

- The comparison is `LRU` against `ECG_REUSE_PLAN_LRU_FLOWTHROUGH`. Both arms
  select victims with LRU and both run `--flowthrough all`, so structural
  FlowThrough is symmetric and only graph representation, request path, and
  record transport differ.
- Timing uses the same six 262,144-vertex PageRank samples, geometries, and
  iteration-1/iteration-8 semantic receipts as the replacement screen, with
  gem5 O3, the computed-address compact trace-free path, 16 epochs, 2 tier
  bits, 4-byte records, and no prefetcher.
- Full-graph 8-byte and 4-byte cache_sim roles are matched cell for cell over
  `pr`, `bfs`, `bc`, and `cc` on the five compact-eligible graphs.
  `soc-LiveJournal1` is excluded from this width comparison because its full
  graph needs 23 destination bits, and `23 + 2 + 4 + 4 = 33` does not fit a
  32-bit record.
- Bounded matched-work Sniper rows with 8-byte records corroborate demand LLC
  load misses only. Sniper does not expose byte-level off-chip traffic for
  this role, and its LLC load-miss count excludes writeback bytes.

Admissible claims are limited to transport and placement: PageRank execution
time and off-chip traffic against an identical LRU cell, full-graph record
substitution at 4 bytes against the matched 8-byte control on the five
compact-eligible graphs, and Sniper demand-LLC-miss non-regression. No
replacement-policy claim, no comparison against SRRIP, GRASP, or P-OPT, no
Sniper byte-traffic claim, no hardware-cost claim, and no timing claim from
Sniper or from the mechanism stage may be drawn from it.

This is a confirmatory campaign, not a first observation. Earlier symmetric
iteration-1 rows at commit `14b82753` already contained the same LRU transport
control; they informed the transport-only scope but are excluded from the new
campaign evidence. The configuration records those prior ratios explicitly.
The new thresholds use the existing frozen +/-2% tie band rather than being
selected from the confirmatory rows: an aggregate ratio of at most 0.98 is
required for a positive timing or compact-width claim, while 1.02 is the
maximum material-regression bound.

The screen therefore requires an aggregate geometric-mean time ratio of at
most 0.98 and an aggregate traffic ratio of at most 1.02, with every per-cell
time and traffic ratio at most 1.02. The complete phase repeats those limits at
iteration 8, requires an aggregate compact/wide off-chip traffic ratio of at
most 0.98 with per-cell traffic and LLC-miss ratios at most 1.02, and requires
Sniper aggregate and per-cell demand-LLC-miss ratios at most 1.02.

Configuration version 2 records one validation amendment discovered before
any full cache role was accepted. When `--flowthrough all` is active, the
common structural no-allocate path intentionally supersedes the candidate's
duplicate static record no-allocate path. cache_sim now records that
subsumption explicitly and accepts it only when structural FlowThrough is
active with positive accesses. The first screen receipt and attempted full
cache run are invalidated by the configuration amendment; no threshold,
policy, stage, or claim changed.

Configuration version 3 records a second validation amendment discovered in
the first Sniper role. Distinct per-edge records may target different vertices
within one property cache line and legitimately carry different future hints.
The Sniper fused sideband now preserves all destination records and binds the
certified prefix by current source plus the original bound property address,
rather than collapsing records by source and cache line. The marker-free
post-prefix lookup remains line-granular, but the Sniper role uses pure LRU
replacement and supplies no admissible timing, so those hints cannot affect
the campaign's victim choice. All version-2 screen and full-role evidence is
invalidated; thresholds, policies, stage roster, and admissible claims remain
unchanged.

### 7.3 REF32 original-goal recovery

`ECG_REF32_RP_COMMIT` is a separate candidate that returns to the original ECG
goal: improve cache behavior relative to GRASP and P-OPT without retaining
P-OPT's runtime rereference matrix.

For certified n18 DBG-ordered PageRank graphs, each 32-bit edge record contains
an 18-bit destination, an 8-bit forward property-line reference, a 2-bit
finite/dead/wrap/unknown state, and a 4-bit forward-record prefetch action. The
reference uses a five-bit exponent and three-bit mantissa. A 21-bit per-LLC-line
deadline covers the complete recorded iteration while preserving safe modular
expiry; a passed prediction becomes unknown, never dead.

Private-cache hits update LLC metadata through a bounded commit-only channel:
16 entries, eight governed requests of latency, one update per governed request,
and cache-line coalescing. The selective prefetch path has eight pending entries,
the same eight-request latency, one issue per eight governed requests, and an
LLC-only fill. It reads the selected destination from a 16-record lookahead
buffer, rejects resident or pending duplicates, and checks replacement
admission before issuing.

The record substitutes for the ordinary 4-byte CSR destination and has no
sidecar. At a 512 KiB, 64-byte-line LLC, the accounted added state is 24 bits per
line plus the two bounded queues, 16-record lookahead buffer, and control state:
199,232 bits total. The corresponding n18, 256-epoch P-OPT matrix is 33,554,432
bits, a 168.4x reduction before counting P-OPT's reserved LLC capacity.

REF32 rows are accepted only when the graph filename certifies DBG order, the
record/commit/prefetch/resource receipts validate, no runtime P-OPT matrix is
present, semantic output matches, both queues drain, and the record remains four
bytes. Cache-simulator LLC misses, governed-property misses, and off-chip
traffic are admissible. Timing is not: detailed-simulator request, commit, and
prefetch implementations must be validated before any speedup claim.

## 8. Publication policy

Preliminary numbers and intermediate choices remain local. Tables and measured
figures are published only after the final frozen campaign, preprocessing
costs, record data footprints, traffic decomposition, and physical
metadata/control costs are complete.
