# Evaluation Methodology

The full-paper successor evaluates graph-adaptive records, request-bound loads,
bounded resident updates and real-record prefetching. This page defines current
workloads, comparisons, measured results and evidence limits.

The mechanism is explained in [Adaptive records and cache control](ReusePlan-FlowThrough),
the [worked graph-to-cache example](Property-to-Cache-Walkthrough), and the
[native RISC-V pipeline](RISC-V-Instruction-Path).

## 1. Algorithm scope

**Single-core, fixed-iteration PageRank pull-GS** is implemented in cache_sim,
gem5 RV64 O3, and Sniper.
The record stream follows the exact prepared graph order. Each experiment
uses one trial, a positive fixed iteration count, and zero convergence
tolerance: `-o 0 -n 1 -i N -t 0`.

SpMV, BFS, SSSP, CC, BC and TC share current kernels and CSR/record adapters
across cache_sim, gem5 RV64 O3 and Sniper. Their detailed paths are admitted for
bounded semantic qualification, not final-workload CPU speedup.
The legacy algorithm executables are not substitutes for these current paths.

### Per-algorithm performance

| Algorithm | Current implementation | 8 MiB Patents kernel ECG/LRU | 1 KiB diagnostic total ECG/LRU | Native CPU speedup |
|---|---|---:|---:|---|
| PageRank (`pr`) | cache_sim, gem5 RV64 O3, Sniper | 0.5191 | Not measured | Not reported for final workloads |
| SpMV (CSR) | All three (bounded) | 0.5091 | 2.7754 | Not measured |
| BFS (`bfs`) | All three (bounded) | 1.4033 | 3.0956 | Not measured |
| SSSP (`sssp`) | All three (bounded) | 1.7235 | 2.2321 | Not measured |
| CC (`cc`) | All three (bounded) | Not measured | 2.5008 | Not measured |
| BC (`bc`) | All three (bounded) | Not measured | 2.0792 | Not measured |
| TC (`tc`) | All three (bounded) | Not measured | 2.3309 | Not measured |

Ratios are modeled off-chip traffic, **not CPU speedup**; lower is better. Patents uses 32 KiB L1D/256 KiB L2.
PR/SpMV use two traversals and 14.40 MiB F32 arrays; their GRASP/LRU kernel ratios are 0.6446/0.6402.
BFS/SSSP converge from source 0; SSSP uses declared synthetic weights 1-32 and delta 8. Both regress in the kernel.
Including setup, BFS/SSSP totals are **2.1670/2.0173**, and SpMV is **1.1613** after two repeats; PR setup is unmeasured.
The 1 KiB pressure512 totals include setup and are not comparable to the 8 MiB kernel column.

### Algorithm and prediction contracts

| Algorithm | Declared variant | Designated property read |
|---|---|---|
| SpMV | Fixed-input CSR `y=A*x`, ordered non-fused F32 accumulation | Read-only F32 `x[v]`; dense exact |
| BFS | Level-synchronous top-down with sorted frontiers | U32 depth bits; ordered-filtered |
| SSSP | Deterministic serial delta-stepping, sorted light closures and heavy phases | U64 distance; ordered-filtered |
| CC | Two-round deterministic Afforest, minimum-ID roots and a deterministic largest-component tie break | Initial U32 neighbor component; ordered-filtered |
| BC | Level-synchronous Brandes over an explicit source list, sorted forward/reverse levels | U32 depth, then F32 dependency; ordered-filtered |
| TC | Degree-order orientation and node-iterator intersections | Dedicated read-only U64 target-row start; dense exact |

Ordered-filtered metadata means **next potential designated read**, not next actual reference.
Skipped positions, ordinary-access invalidation and charged phase transitions follow the
[filtered contract](ReusePlan-FlowThrough#ordered-filtered-algorithm-passes).

New-algorithm totals include initialization, sorting, buckets, orientation and record construction;
nonintrusive first-binding snapshots also report kernel traffic without resetting cache contents.
P-OPT has not been ported to this roster; an uncharged dynamic oracle is not a substitute.

## 2. Stable design and simulator roles

Layout selection uses the maximum VID actually encoded, the record count,
the requested four/eight-byte width and the minimum mantissa precision.
The joint UNKNOWN/DEAD/FINITE/WRAP token is the same for every graph.
Four-byte records are preferred; any required eight-byte carrier is charged.
Report the resolved VID, metadata, horizon and mantissa bits, not a fixed
graph-specific mask width.

| Mode | Purpose |
|---|---|
| `ECG:transport` | Same encoded-record/property loop with replacement and prefetch application disabled |
| `ECG:replacement` | Apply the resident reuse prediction to replacement |
| `ECG:prefetch` | Apply the bounded record-window prefetcher |
| `ECG` | Combined replacement and prefetch |

All modes retain the actual graph and property values. Metadata delivery
does not allocate or dirty a line or refresh ordinary recency/RRPV. Required
updates and optional prefetches have separate bounded accounting.

| Backend | Evidence provided | Limit |
|---|---|---|
| cache_sim | Complete modeled cache accesses, replacement, record acquisition and traffic | Access-step model, not architectural time |
| gem5 RV64 O3 | Actual raw records and typed F32/U32/U64 loads, retirement transport and LLC prefetch | PR timing path plus bounded current-algorithm qualification |
| Sniper | Actual record loads and modeled transport/replacement/prefetch corroboration | Its `bounded-completion-corroboration` link is not native retirement |

Only gem5 O3 execution time is used for architectural speedup.
cache_sim does not model cycles or instructions. Sniper time is not used as
native RISC-V speedup. Compare each backend with its own same-build,
same-input baseline; absolute misses and cycles need not match between models.

## 3. Workloads and fair comparisons

Prepare graph ordering once and reuse the identical `.sg` bytes for every
policy. Preserve isolated vertices and disclose directed versus symmetrized
inputs. A record annotation is valid only for the traversal that consumes it.
Current graph loaders use signed-32 `NodeID`; an unsigned-32 ISA probe is not
a full-size graph-loader result.

| Current workload set | Iterations | L1D / L2 | LLC | P-OPT traffic treatment |
|---|---:|---|---|---|
| Six full core graphs: web-Google, roadNet-CA, cit-Patents, soc-pokec, soc-LiveJournal1, com-Orkut | 2 | 32 KiB / 256 KiB, eight-way | 8 MiB, 16-way | Simulated column stream |
| Directed Twitter-2010 | 1 | 32 KiB / 128 KiB, eight-way | 8 and 24 MiB, 16-way | Analytic column charge, explicitly disclosed |

The core corpus retains its prepared symmetrization and DBG ordering.
FlowThrough and extra structure prefetchers are off. ECG's own prefetcher is
enabled only in the corresponding mode and its traffic is included.

Ordinary CSR comparisons use `--current-pr-baselines`: the same fixed pull-GS
loop, non-fused F32 arithmetic, warm-up, and explicit incoming/outgoing
CSR-index accesses as the record path. CSR reads ordinary source IDs;
ECG reads its encoded carrier. Compare LRU, SRRIP, GRASP_PAPER, P-OPT controls
and the four ECG modes without changing algorithmic work or input order.

Native transport-matched speedup isolates the cache mechanism inside the
record/property instruction loop. It must not be presented as end-to-end
speedup over ordinary CSR unless that separate comparison is implemented
and measured.

### P-OPT and P-OPT-SE

Keep full backing-matrix storage, active-column storage, reserved LLC ways
and matrix traffic separate. The size-correct reservation is:

```text
column_bytes = ceil(vertices * property_bytes / line_bytes)
reserved_ways = ceil(active_columns * column_bytes / bytes_per_way)
```

Charged P-OPT pays this reservation. `POPT:UNCHARGED` retains full data
capacity as a P-OPT-favorable replacement-quality control; it is not an
equal-area comparison. Simulated and analytic matrix streams are separate
modeling choices and are not pooled as the same experiment.

Any detailed row with `popt_target_time_charged=0` omits native matrix
lookup/stream latency and is an optimistic P-OPT bound, not a fully costed
P-OPT timing implementation.

`POPT_SE` and `POPT_SE_DISTANT` are disclosed one-column reconstructions:
the public P-OPT artifact does not implement SE, and the paper leaves a
post-final-use case unspecified. Report both interpretations. One-column
residency does not halve the full matrix or its cumulative stream.

## 4. Measurement and acceptance

Report execution time, instructions, demand misses, total off-chip traffic,
and mechanism activity together where the backend supplies them. Total
traffic includes demand, prefetch, writeback and the declared metadata stream.
A miss reduction alone is not evidence of speedup or lower traffic.

Use ROI-scoped gem5 `system.cpu.commitStats0.numInsts`, not unreset
`simInsts`. Form ratios only within the same input/configuration/build and
aggregate with a geometric mean. A +/-2% interval is a tie; retain losses and
worst cases rather than selecting favorable cells.

An accepted comparison requires:

1. the requested mechanism, record width and cache geometry to match execution;
2. complete fixed work and bitwise-equal current PageRank checksums;
3. complete CSR/source/carrier accounting and charged construction storage;
4. closed update, prefetch and traffic counts, with no required-update loss;
5. empty final queues and respected latency/resource bounds; and
6. graph, source, binary, configuration and output fingerprints.

Run the complete small cross-backend profile before detailed evaluation.
Its actual-load fingerprints compare decoded semantics across backends and
both widths. They do not require identical miss counts or issued-prefetch
identities. `--ecg-equivalence` adds diagnostic work, so those rows always
have `timing_valid_for_speedup=0`.

Large cache_sim runs are independent functional evidence. Detailed final
profiles require a freshly validated equivalence receipt and explicit
`--final-stage`. A partial or generic completion marker is not authorization.

## 5. Current PageRank results

These are current-model **functional cache/traffic measurements**, not a
cross-algorithm or native-speedup claim. Final CPU timing cells remain
unreported until their workload matrix is complete.

### Full core graphs

Combined ECG total off-chip transfer reduction at the primary 8 MiB LLC:

| Graph | Versus CSR LRU | Versus GRASP_PAPER | Versus charged P-OPT |
|---|---:|---:|---:|
| web-Google | 3.97% | -19.18% | 12.27% |
| roadNet-CA | 28.48% | 16.38% | 40.80% |
| cit-Patents | 48.08% | 19.48% | 22.59% |
| soc-pokec | 25.23% | 0.80% | 20.01% |
| soc-LiveJournal1 | 39.08% | 15.87% | 20.46% |
| com-Orkut | 38.39% | 24.15% | 11.58% |

All six select four-byte records, with mantissas from three through six bits.
ECG/CSR-LRU traffic has geometric-mean ratio 0.6814. Against GRASP_PAPER,
the +/-2% rule gives four wins, one tie and one loss; web-Google's regression
is part of the result.

### Twitter-scale

The 1,468,364,884-record graph selects ID26/M6/H31/m0 and four-byte records.
P-OPT in this table is the uncharged, full-capacity control. All compared
rows retain 16 data ways.

| LLC | Replacement-only miss reduction | Combined miss reduction | Replacement-only traffic reduction | Combined traffic reduction |
|---|---:|---:|---:|---:|
| 8 MiB | 11.75% | 25.01% | 11.64% | 11.59% |
| 24 MiB | 8.53% | 19.95% | 8.42% | 8.42% |

The replacement-only benefit survives without a P-OPT reserved-way penalty
or ECG prefetching. Combined ECG reduces demand misses further with nearly
unchanged total traffic. This does not isolate encoding precision from every
replacement/admission choice, nor establish equal-area hardware superiority.

The complete current result matrices and provenance are under
`results/ecg_experiments/runs/local_release_cache/` and
`results/ecg_experiments/runs/current_twitter_reproduction/`.
Only these current scoped measurements are summarized here; superseded
experiment outputs remain preserved outside the publication narrative.

## 6. Overhead and claim boundaries

Report preprocessing time, peak construction memory, `retained_source_bytes`,
carrier payload/allocation and auxiliary storage separately. A four-byte
carrier preserves edge-stream width, but the current builder retains the
source graph and allocates that carrier separately. Eight-byte records pay
their actual storage and traffic cost.

The resident prediction payload is 67 bits per LLC line, or 13.1% of the
512 data bits before other metadata, ports, queues and ECC. Current-design
physical area, energy and critical-path overhead are not yet established.
Include RF/AGU integration, dedicated lookup/update resources and real
saturation/backpressure behavior before making a low-overhead hardware claim.

The paper extends the published ECG workshop work with the current model and
native realization. Cite that lineage and the closest comparators; neither
passing workload gates nor a working simulator proves universal novelty or
bug freedom. See [Related Work](Related-Work), the
[hardware boundary](RISC-V-Instruction-Path#5-state-and-evidence-boundaries),
and [Reproduction](Reproduction).
