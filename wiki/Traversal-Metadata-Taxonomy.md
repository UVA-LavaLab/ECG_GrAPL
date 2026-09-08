# Traversal-aware metadata preprocessing

The objective is to improve what graph preprocessing learns so ECG can help
more than PageRank. Metadata requirements depend on the **algorithm phase and
accessed data**, not just the algorithm name. A common delivery format need
not force every phase to use the same prediction algorithm.

This page analyzes the implemented kernels and identifies candidate
preprocessing capabilities. The opt-in experiment below implements a bounded
subset with the existing record grammar; bitmap/list-body coverage and new
mask fields remain separate design questions.

## Preprocessing is allowed; algorithm replay is not

A bounded graph-preprocessing step, comparable in purpose to P-OPT's
graph-derived analysis, is allowed. A fixed small number of graph scans with
explicit workspace limits can inspect adjacency order, weights, row boundaries
and target cache-line mappings. The resulting metadata should be reusable
across compatible traversals rather than reconstructed for each frontier.

The excluded approach is running the graph algorithm in advance to discover
its future accesses. This includes computing BFS levels or SSSP relaxations
just to manufacture masks, replaying a recorded execution, and rerunning
triangle intersections to predict their accesses. Extra per-frontier graph
scans and runtime per-edge metadata rewrite streams are also outside this
target. Ordinary algorithm work can supply its already-known phase, direction
and frontier state; these are not advance knowledge of later frontiers.

P-OPT prepares a graph-derived rereference matrix; ECG prepares annotations
carried by edge records. This is a shared preprocessing principle, not identical
metadata, cost or accuracy. See [Related Work](Related-Work#direct-lineage-and-graph-specific-baselines).

### What the current implementation does

The current builder already performs graph-only preprocessing:

1. Scan the chosen adjacency stream forward to find each target property
   cache line's first occurrence.
2. Scan backward to find its next occurrence, or its wraparound distance.
3. Quantize that distance and pack the token beside the vertex ID.

The shared kernels also scan for the maximum encoded ID before selecting the
layout. Thus the two mask-construction scans are not the entire preparation
cost. With `E` records and `L` possible property cache lines, construction has
expected `O(E)` time, `O(min(E, L))` first/next workspace, and a separate
`O(E)` encoded carrier. The original graph remains allocated.

`Engine::bind` builds a carrier on first use and reuses it for compatible
adjacency storage, graph sizes, property stride and within-line offset.
This is in-process preparation, not a persistent preprocessed artifact reused
automatically across program launches. A changed traversal stream or line
grouping can require another carrier; property value changes alone do not.

With the default `--record-preprocess csr`, fixed PR/SpMV sweeps have a known
designated property-reference order. For filtered BFS/SSSP/CC/BC passes, the
builder knows only the next **potential
designated read in full CSR order**. It does not know which later rows will
execute. Improving preprocessing therefore means improving this analysis, not
merely making the PR distance encoding more precise.

## Taxonomy of the implemented phases

Here, **governed** means the access currently receives an ECG record/property
annotation. Other accesses still use the cache and can invalidate a prediction
on the same line. Their interference cannot be omitted from the analysis.

| Phase | Actual access pattern and current coverage | Useful preprocessing information and its limit |
|---|---|---|
| PR pull-GS | Fixed full IN-CSR sweep; governed F32 contributions. Scores and contributions are written per outer vertex; degree/index traffic is ordinary. | Next property-line occurrence follows the fixed structural order. Writes and other accesses remain part of the cache behavior; this is not an oracle for the whole memory trace. |
| SpMV | Fixed full OUT-row sweep, `y=A*x`; governed read-only F32 `x`, streamed `y` writes and weight reads where supplied. | Line-next-use in the actual row order is appropriate. Property stride, line sharing and the competing output/weight streams still matter. |
| BFS top-down | Only sorted active-frontier rows execute. Governed U32 depth probes are followed by discovery writes; frontier construction and sorting are ordinary. | OUT-traversal line occurrences and concentration across consumer rows/blocks are static potential-reuse information. Source-dependent future frontiers are not. A visited vertex's depth can still be queried. |
| BFS bottom-up | Unvisited vertices scan IN-CSR until the first current-frontier neighbor. Membership uses packed 64-bit bitmap words; current bitmap reads, next bitmap writes and depth/CSR traffic are ordinary. No current BU annotation. | Preprocessing can map neighbors to bitmap lines and summarize structural coverage. It cannot know the future level's membership or early-exit position. Scalar depth reuse is the wrong target, and a small bitmap may already fit. |
| SSSP light/heavy | Sorted delta-stepping repeatedly closes light work, then processes heavy edges. U64 destination distance is governed; source-distance reads, heap comparisons/updates and work arrays are ordinary. | For a fixed delta, `weight <= delta` versus `weight > delta` is known offline. Separate phase-eligible potential-reference analyses can exclude impossible references from each phase. Future buckets, repeated relaxations and their actual row sets remain value-dependent. |
| CC Afforest | Two sampled-neighbor rounds, compression, largest-component selection, remaining-edge work and final compression. The initial U32 neighbor-component load is governed; root chasing, path halving and union writes are ordinary. | Sampled positions and remaining structural references can be distinguished. The evolving roots and dynamically skipped largest component cannot be inferred from the edge VID alone. Original graph hubs need not be root-access hotspots. |
| BC forward | Per-source sorted BFS levels; governed U32 depth. Checked U64 path-count reads/updates, frontiers, order and level arrays are ordinary. | Forward structural potential and array-specific line mappings are available. The source's levels and shortest-path counts are results of execution, not offline metadata inputs. |
| BC backward | Reverse the discovered BFS levels. Ordinary CSR/depth tests select DAG successors before governed F32 dependency loads; U64 path-count inputs and score updates are ordinary. | The data role and phase differ from forward BFS. Graph structure can describe potential dependencies, but reverse discovered-level order is not simply reverse full-CSR order. Computing that schedule beforehand would replay the source traversal. |
| TC | Degree/ID orientation followed by nested sorted adjacency-list intersections. Only a duplicate read-only U64 target-row-start header is governed; adjacency comparisons are ordinary. | Oriented row ranges, lengths and structural consumer counts are available without doing intersections. List/range reuse is the relevant target; predicting a row header does not predict or protect the list body. |

Direction matters even for a cheap frequency statistic. PR pull reads `p[v]`
once for each destination in `N_out(v)`, so the structural consumer count is
`d_out(v)`. OUT-row SpMV reads `x[v]` once for each source in `N_in(v)`, giving
`d_in(v)`. Cache-line aggregation combines several vertices. Neither vertex
degree nor a line's total reference count describes the temporal distribution
of references or the active rows of a particular BFS source.

## What the preprocessing scan should be able to specialize

The useful common abstraction is **stream order, static eligibility and target
cache-line mapping**. These describe an analysis, not a proposed record layout.
They let a preprocessing implementation share its scan/storage machinery
without assuming that every edge predicts a four-byte PR contribution.

| Required capability | Information available without replay | Boundary |
|---|---|---|
| Traversal-specific order | IN versus OUT adjacency, reordered row sequence, orientation and fixed dense repetitions. | Future frontier/bucket order and discovered BFS levels are not static orders. |
| Phase-specific eligibility | SSSP light/heavy weight predicates for fixed delta; CC sampled positions; algorithm-defined structural subsets. | Dynamic row activation and CC component skipping cannot be treated as known offline. |
| Correct target and granularity | Scalar type/stride/alignment, bitmap word/line mapping, or adjacency-list start and extent. | The current typed scalar path does not automatically support bitmap or list-body annotations. Some improvements require coverage changes, not just a different scan. |
| Reuse distribution, not only one next position | First/next structural occurrences, distinct consumer rows, and concentration within row/block windows can be collected from graph scans. | These are candidate potential-reuse summaries, not proven predictors of active-frontier reuse. Arbitrary pairwise graph analysis is not a simple linear scan. |
| Phase and lifetime validity | The executing algorithm already knows its current phase, direction, property binding and pass boundary. | Preprocessing must not turn an unknown future into a guaranteed deadline. Phase changes and ordinary accesses must invalidate or expire incompatible state. |
| Explicit cost and reuse | Actual scan work, carrier bytes, temporary storage, retained input, binding work and compatible-carrier reuse. | No hidden preprocessing pass, side table, extra runtime graph fetch or metadata update stream is free. |

The first concrete specialization is SSSP's static light/heavy
predicate: it can remove references that cannot be governed in the current
phase without solving SSSP first. That is an analysis opportunity, not a claim
that it will improve caching. TD BFS instead needs a useful approximation of
frontier-conditioned reuse; changing full-CSR precision alone cannot provide
that missing information.

BU BFS and TC expose a different limitation: their important targets are
bitmap lines and adjacency ranges, respectively. A better scan that still
annotates only depth values or row headers would leave those accesses
uncovered. CC and BC further require distinguishing the designated edge-driven
load from other, often value-dependent accesses to the same arrays.

## Opt-in preprocessing experiment

`--record-preprocess traversal` changes reference selection, not the algorithm,
record width, cache victim rule or native token grammar. The default remains
`csr`. The existing experiment runner admits this trial in cache_sim only.

| Kernel | Implemented selection | Construction state per allocated hash slot |
|---|---|---|
| SSSP | Next same-line occurrence within the edge's light/heavy weight class for the declared delta. | One line key and two first/next pairs: 40 bytes rather than 24. |
| CC | Separate first-neighbor, second-neighbor and remaining-edge occurrences. Runtime component skipping is still unknown. | One line key and three first/next pairs: 56 bytes rather than 24. |
| BFS/BC | Retain same-row next-potential-use predictions; emit UNKNOWN instead of predicting another row or pass. | The original 24-byte slot; a bounded forward/reverse row cursor reads CSR offsets. |
| SpMV/TC | Preserve the current dense scalar-property analysis as a control. | Unchanged; TC list bodies still have no annotation. |

Distances remain in original structural-index units, not a compressed
phase-specific coordinate. A row-local candidate is still quantized upward;
this changes candidate selection, not the runtime pass-expiry protocol.
Filtered WRAP handling, ordinary-access invalidation and LRU-neutral UNKNOWN
are unchanged. DO BFS applies the row restriction only to its TD depth loads;
BU still uses ordinary bitmap/CSR accesses.

There is no per-edge class array or runtime reconstruction. SSSP rereads the
already-declared weights during construction; row-based analyses walk existing
CSR offsets in both directions. These accesses, carrier writes and larger
phase-slot allocations are charged and bounded by the existing limits.
Empty hash slots occupy the same space; allocation remains a power of two
covering at least twice the maximum number of distinct target lines.
The work is expected `O(E)` for weight classes and `O(V+E)` for row-based
analysis. Compatible immutable carriers are still reused.

`ecg_preprocessing_8mb_cache` compares same-build full-Patents BFS/SSSP/CC/BC:
current LRU/T/R versus specialized T/R, twenty serial cells at 8 MiB.
New setup/kernel snapshots include LLC hits/misses and registered-property
hits/misses, so property gains can be separated from other-data losses without
mixing in changed construction traffic. The measurements do not claim a
bitmap/list-body implementation or CPU speedup.

## Semantics and cache decisions must stay honest

A structural next-reference bound, an estimated reuse rank and a frequency
class are different kinds of information. A future advisory analysis must not
silently store a hotness score as today's FINITE deadline or treat low
confidence as DEAD. The encoding and receiver contract would need to state
what the information means before allocating bits.

For filtered passes, the current implementation converts WRAP/DEAD to UNKNOWN,
caps FINITE predictions at pass end and expires them at closure. UNKNOWN is
LRU-neutral. **Visited, settled, updated or no longer active does not mean
never read again**, and a cache line can contain both active and inactive
vertices. Any stronger deadness claim must cover all relevant accesses, not
only the algorithm's work queue.

Replacement and prefetch also need different evidence. A line may be worth
retaining if requested but not likely enough to justify fetching in advance.
The current four-byte in-band record replaces a four-byte ID load; mask bits
do not themselves require another property-side demand. Construction,
eight-byte carriers, weighted-source overfetch, lookahead acquisition and data
prefetches have separate costs.

The existing [8 MiB results](Evaluation-Methodology#per-algorithm-performance)
show substantial PR replacement benefit, much smaller TD-BFS benefit and a
TD-BFS combined-prefetch traffic regression. The direction-optimizing BFS
trial's main improvement comes from changing traversal, not a new mask:
TD still uses the existing builder and BU is unannotated. These observations
motivate specialization; they do not establish which candidate summary works.

Evaluate a future preprocessing change against the **same algorithm variant,
source, graph order and cache geometry**, with transport and replacement-only
controls before attributing a prefetch gain. Report preprocessing time,
workspace and traffic, kernel and setup-inclusive traffic, affected-data
misses, and collateral misses in the rest of the LLC. Reusing preprocessing
across iterations or sources must reflect actual reuse, not assumed
amortization. Native runtime and hardware cost remain separate measurements.

## Code anchors

The shared kernel functions define the access patterns independently of the
selected preprocessing mode.

| Topic | Source |
|---|---|
| Current forward/reverse builders | `bench/include/ecg_record_stream.h`, `buildRecordsMapped` and scoped typed `buildRecords` |
| Carrier preparation and reuse | `bench/include/ecg_algorithms.h`, `Engine::bind` |
| PR fixed pull loop | `bench/src_sim/pr.cc:193-231`, `PageRankPullGSFixed_Sim` |
| SpMV | `bench/include/ecg_algorithms.h`, `spmv` |
| TD and direction-optimizing BFS | `bench/include/ecg_algorithms.h`, `bfsTopDownLevel`, `bfsDirectionOptimizing`, `bfs` |
| SSSP | `bench/include/ecg_algorithms.h`, `BucketHeap`, `sssp` |
| CC | `bench/include/ecg_algorithms.h`, `root`, `link`, `cc` |
| BC | `bench/include/ecg_algorithms.h`, `bc` |
| TC | `bench/include/ecg_algorithms.h`, `tc` |
| P-OPT preprocessing comparison | `bench/include/graphbrew/partition/cagra/popt.h:396-574`, transpose selection and `makeOffsetMatrix` |

For current token, invalidation and victim semantics, see
[Adaptive records and cache control](ReusePlan-FlowThrough).
