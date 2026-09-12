# Frontier-conditioned consumer-cohort masks

This is the cache-only BFS candidate B following the
[P-OPT information ablation](P-OPT-Rank-Attribution). It is not the failed
[potential-window codec](Potential-Window-Prototype), a learned predictor,
or a demonstrated performance win: **B failed its frozen Patents gate**,
as reported [below](#measured-patents-result). The default next-reference mechanism
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

## Measured Patents result

The four cells completed at implementation commit `c69c44aa` in
`results/ecg_experiments/runs/frontier_mask_patents_c69c44aa`.
All use the same executable and prepared graph, source 0 and cache
geometry. Core BFS work and output agree across all four arms.
The two B arms have identical complete workload/carrier receipts, setup
counters and fixed source/context costs. No additional full-graph run
or parameter adjustment was used.

| Policy | Depth misses | Other misses | Writebacks | Kernel transfers |
|---|---:|---:|---:|---:|
| Phase-scoped GRASP | 4,721,503 | 6,306,233 | 363,920 | 11,391,656 |
| Intact favorable P-OPT | 4,286,026 | 6,331,723 | 235,200 | 10,852,949 |
| B, gating ignored | 4,814,475 | 6,308,127 | 327,697 | 11,450,299 |
| B, gating enabled | 4,814,471 | 6,308,127 | 327,697 | 11,450,295 |

Frontier gating saves **four depth misses/transfers, or 0.000035%**
relative to its cost-matched control, nowhere near the frozen 2% gate.
Enabled B has **0.515% more kernel traffic than phase-scoped GRASP**
and **5.504% more than intact P-OPT**. Relative to GRASP it adds 92,968
depth misses and 1,894 other misses while avoiding 36,223 writebacks:
the net result is 58,639 additional transfers.

This was not an inactive policy: enabled B made **151,947 actual victim
overrides**, versus 151,948 with gating ignored. Both applied 32,984,669
metadata updates and delivered all 36,787,596 queued events, with a queue
peak of three and no pending events at completion. Aggregate override
counts are not paired victim identities; their one-count difference must
not be presented as proof that only one individual decision changed.

### Cost and state receipts

| Policy | Setup transfers | Setup + kernel transfers |
|---|---:|---:|
| Phase-scoped GRASP | 2,877,493 | 14,269,149 |
| Intact favorable P-OPT | 9,293,511 | 20,146,460 |
| B, gating ignored | 15,936,444 | 27,386,743 |
| B, gating enabled | 15,936,444 | 27,386,739 |

Enabled B loses setup-inclusive traffic by **91.93% versus GRASP** and
**35.94% versus P-OPT**. The footprint and costs were not waived:

| B resource/work | Matched amount |
|---|---:|
| Actual carrier, 22 ID bits + 10 metadata bits | 132,151,576 bytes, four bytes per record |
| Temporary producer allocation | 1,887,384 bytes |
| Producer modeled reads / writes | 589,002,592 / 530,204,224 bytes |
| Constructor word digest | `22b6b4acf0b43138` |
| Counter modeled reads / writes, including initialization and scans | 30,128,644 / 30,114,984 bytes |
| Counter arithmetic / scan / swap steps | 7,528,234 / 3,927 / 17 |
| Source rows / successful appends | 3,764,117 / 3,764,116 |
| Cohort clears / total markers | 2,281 / 4,425 |
| Control bytes, including configuration and full snapshots | 212,992 |
| Proxy object, including its 2 KiB counter banks | 3,264 bytes |
| Proxy + source staging + conservative LLC context budget | 3,360 bytes, below the frozen 4 KiB ceiling |
| Logical resident metadata | 67 bits per line |

Modeled memory bytes are not necessarily off-chip bytes; they can hit
the private caches. Do not add the counter-byte totals to DRAM transfers.
The fixed costs match, while realized observation-port work can change
with cache residency: enabled/ignored total controller steps are
512,761,583 / 512,761,595, not native CPU cycles.

The audit replays four completion markers and 148 input fingerprints
across 34 distinct files. It binds raw JSON, logs, watchdogs and the
unchanged original result CSV in `audit/audit.complete.json`.
Successful execution markers do not mean a performance gate passed:
`audit/receipt.json` records all three performance gates as false.

| Artifact | SHA256 |
|---|---|
| `combined_roi_matrix.csv` | `d066e8e8339ebd483173e37e5799434e7a80ce8f98174b7a91f60798e98d5709` |
| Shared executable | `af6b7965eaa97dbd0ba01f9c814e2a576786ee8ccd772bed40aed5ffea98aac1` |
| `audit/audit.complete.json` | `bac798ef1a8b479b0469833af20a7e81fadd26f8342604f857f5771ff2b89617` |

**Decision: freeze B as a negative result.** Current-frontier conditioning
adds negligible traffic value to this coarse-cohort/RRPV-7 design.
This experiment does not establish which of granularity, false positives
or consumer restrictions is the dominant limitation, nor does it overturn
the earlier P-OPT result that useful future information can matter.
Do not rescue B by widening RRPV eligibility or tuning its cohorts after
this result. A remains unimplemented. All twelve development runs are now
used; further full-graph work requires an explicit regrouping and budget
revision.
