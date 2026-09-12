# Frontier-conditioned consumer-cohort masks

This is the cache-only BFS candidate B following the
[P-OPT information ablation](P-OPT-Rank-Attribution). It is not the failed
[potential-window codec](Potential-Window-Prototype), a learned predictor,
or a demonstrated performance win. The default next-reference mechanism
and the window negative control remain available with their original
semantics.

## Immutable graph information

For `N` vertices, set `C = nextpow2(max(8, ceil(N/256)))`. This gives at
most 256 source-row cohorts. A record describes later potential consumer
rows of its U32 target cache line, not a predicted BFS execution:

| Bits | Meaning |
|---|---|
| 9 | Known graph-derived information |
| 8 | At least one consumer beyond the eight represented cohorts |
| 7:0 | Presence in the emitting cohort and the following seven cohorts |

Bit 0 includes strictly later rows only; the emitting row does not
validate itself. A reverse scan freezes the emitted token before
inserting that row once. Same-row duplicates reuse the frozen token.
The temporary state is row32, internal presence8, beyond1, frozen
token10 and valid1: 52 logical bits in one 64-bit word per target line.
Shifted-out bits set beyond-horizon and never wrap into nearer cohorts.

ID inspection and reverse construction are O(V+E), without running BFS
or reconstructing a graph for each frontier. The carrier is genuinely
4 or 8 bytes per edge, selected from actual encoded-ID headroom, and the
original CSR remains allocated beside it. A constructor word digest and
layout/allocation receipts identify the immutable stream.

The ten-bit carrier layout and bounded transport are shared infrastructure,
but window and frontier streams have distinct C++ types and decoders.
For example, B's `0x224` means consumer cohorts 2 and 5 relative to the
anchor; it must not enter WINDOW's gap/strength/endpoint decoder.
Known-empty `0x200`, beyond-only `0x300`, and UNKNOWN `0x000` are all
unrankable, not DEAD. Known-record counts include empty/beyond-only words;
they are not accuracy or usable-prediction coverage.

## Causal remaining-frontier context

Two fixed banks of 256 U32 counts occupy **2 KiB of modeled ordinary
memory**. Binding initializes them and accounts for the already-executed
single-source frontier seed. Normal successful next-frontier appends
increment the next bank; source visits decrement the current bank before
that row's property accesses. Counts do not clear a cohort while another
unprocessed current-frontier vertex remains there.

Before a pass's data accesses, a bounded scan of the cohort counts forms
a 256-bit snapshot and verifies the current-frontier total. It scans at
most 256 counters, not graph edges or a reconstructed frontier. The
snapshot is acknowledged before use. Next-frontier discoveries never
enter current eligibility before the actual frontier-buffer swap.

A zero-count transition sends an acknowledged cohort-clear update,
sharing its message with a source-watermark advance when both occur.
Counter accesses preceding that acknowledgment use the last delivered
context; the model does not grant instantaneous knowledge to the cache.
The real frontier swap verifies the next length and reuses the exhausted
zero bank without a reset walk. Invalid totals, underflow, skipped active
cohorts, premature closure and wrong-pass updates are errors.

This remains potential reuse: an active row and a consumer can be
different vertices in the same cohort. Finite horizon and coarse-cohort
false positives remain limitations.

## Publication, action and costs

The existing checked record/property/store lineage is retained: an
associated discovery store publishes the immutable read hint under its
newer event order. Delayed updates require matching resident PENDING
state; eviction, newer observations and pass expiry cannot revive an old
hint. The raw mask stays anchored at emission, not delivery.

Published line state uses the existing **67 logical bits**: three state
bits plus a 64-bit union containing either pending event order or a
54-bit absolute emitting-cohort anchor and ten token bits. The pass
range and source watermark are global. Overflow fails rather than
wrapping; no extra per-line cutoff or runtime graph-sized matrix is added.

Start from the actual GRASP victim once. If its hint is unrankable, retain
it. Otherwise compare only rankable RRPV-7 candidates by their first
remaining represented cohort, selecting a strictly farther alternative.
Ties retain the existing order. No density tie-break, RRPV-6 widening,
insertion change, bypass, prefetch or learner is present.

GRASP is used during the acknowledged graph pass, with LRU outside it.
Counter-snapshot preparation occurs before BEGIN installation; frontier
sorting remains outside the pass. Cache contents and insertion/hit history
are not reset. B's own paid controls supply phase switching, without a
second CSR phase-control charge.

| Resource or operation | Accounted prototype contract |
|---|---|
| Counter initialization | 512 U32 writes, then the seed's read/write/update |
| Append or consume | One U32 read, one U32 write, one extra local functional step |
| Cohort scan | One U32 read and one local step per cohort per pass |
| Frontier swap | One local step, no graph/reset walk |
| Configuration, source/clear, CLOSE | 16 functional steps and 48 logical control bytes |
| BEGIN with current bitmap | 24 functional steps and 80 logical control bytes |
| Metadata queue/port | 16 uncoalesced entries, eight-step latency, one observation/delivery port |
| Source packet staging | 32 transient bytes |
| LLC context | 32-byte bitmap; a 64-byte budget includes its pointer/control bookkeeping |
| Proxy controller | Includes the queue, lineage and source counter banks; object plus staging/LLC budget must fit 4 KiB |

The raw ledger closes counter memory, local steps, queue outcomes and
control bytes. Counter initialization precedes the kernel boundary:
runtime memory steps equal kernel memory requests plus 514 initialization
requests. Controller steps are **not CPU cycles**. A native interface,
spills, physical SRAM/ports/ECC, area, energy and execution time remain
unqualified.

## Cost-matched ablation and frozen gate

Use `--record-model frontier --record-base-policy GRASP_PAPER
--grasp-scope graph-passes` with `--frontier-gating enabled|ignored`.
Both values construct and load the same records, maintain both counter
banks, send the same context and pay all source/control costs.
`ignored` withholds only the frontier intersection from victim ranking;
it still honors source/pass progress. It is explicitly marked as a policy
ablation. The two arm names and launch strings have equal lengths to avoid
gratuitous argument/output-path layout differences.

Only unweighted, nonempty, single-query, single-source sorted TD BFS is
admitted. Native backends, other algorithms, DO BFS, weighted graphs,
prefetch and incompatible observer/preprocessing modes are rejected.
Transport mode exists for bounded mechanism qualification; the following
full-graph arms both use replacement.

The `ecg_frontier_mask_cache` profile contains exactly four fresh,
same-build Patents/source-0 cells at 8 MiB/16-way LLC, 32 KiB L1D and
256 KiB L2 (both eight-way), with no prefetch:

| Cell | Purpose |
|---|---|
| Phase-scoped CSR GRASP | Strong common baseline |
| Intact favorable `POPT:UNCHARGED` | Full-capacity, free-runtime-lookup quality baseline; construction counted |
| B, gating ignored | Cost-matched structural-mask control |
| B, gating enabled | Complete frontier-conditioned candidate |

The information gate is **at least 2% fewer kernel transfers** than the
ignored arm, using ignored traffic as denominator. A competitive kernel
win additionally requires fewer transfers than both fresh intact
baselines. A cold traffic win requires lower setup-plus-kernel traffic
than both; kernel-only success does not establish it.

```bash
ulimit -c 0
make -j1 bench/bin_sim/algorithms
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_frontier_mask_cache \
  --run-dir results/ecg_experiments/runs/frontier_mask_case \
  --no-build --no-resume
```

Use a clean committed tree. Each policy has one simulator thread, a
2 GiB process-tree RSS guard and a thirty-minute wall limit. These four
runs exhaust the remaining twelve-run development budget. Failure freezes
this candidate; it does not authorize new cohorts, wider eligibility,
more graphs, or an A/B combination. No novelty or deployment claim is
made by this prototype.
