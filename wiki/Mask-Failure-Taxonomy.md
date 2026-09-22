# Why the record mask loses: a measured taxonomy

The record mechanism gives the last-level cache a decoded upper bound on when
each governed property line is next potentially read, and `selectVictim` uses
that bound to choose a victim. It still loses traffic to GRASP and to P-OPT.
This page records **why**, measured rather than argued, and it splits the five
algorithms into two groups with different causes and different remedies.

The short form: the mask is not the weak part. For the dense kernels the
information is present and current and the **victim rule was spending capacity
badly**, which is fixable and has been
[fixed and measured](Governed-First-Eviction). For the filtered kernels the
information is **structurally stale by the time it is consulted**, which no
ordering rule can repair.

All measurements here are functional cache results. No receipt on this page has
`timing_valid_for_speedup` set, and none of it is a CPU speedup.

## What was ruled out first

Four explanations were tested and eliminated before the taxonomy was written,
which is why the remaining two are worth taking seriously.

**Encoding precision, quantization and delivery.** The
[reference-consumer diagnostic](GRASP-Reference-Consumer) handed a GRASP-based
consumer the actual canonical P-OPT matrix — the same 60,396,288 bytes, 235,923
lines and 256 epochs that intact P-OPT consumes, digest
`3683564403557561320`. It closed 29.42% of the ordinary-GRASP-to-P-OPT kernel
gap and stayed 17.41% above intact P-OPT on byte-identical information. A
residual that survives perfect information is not an information problem.

**Candidate ordering.** The [consumer-architecture study](Consumer-Architecture)
gave that consumer P-OPT's rank-first selection with RRIP aging confined to the
maximum-rank tie set. The arm **lost**: `share = -0.0569`. Ordering discipline
alone does not explain the residual.

**Construction quality.** Records leave construction 97–100% FINITE on every
filtered kernel, so nothing is lost at build time.

**Pass-end clamping.** `BuildScope::end` defaults to `UINT64_MAX`, and no
measured cell was clamped. The suspicion that bounds were being truncated at a
pass boundary was a non-problem.

## Dense kernels: SpMV and PageRank had an action problem

For SpMV and PageRank the mask is in excellent condition at the moment of use.
Live bound coverage is 99.9% and 98.5%, and the strictly-farther override fires
on 44.1% and 60.8% of evictions. The cache is consulting a current bound
constantly and still losing.

The attribution census located the leak. On 39.1% of SpMV evictions and 34.7%
of PageRank evictions, the base victim handed to `selectVictim` was **not a
governed line at all** — it was structural data, CSR indices or the edge array.
The record rule then had nothing to say about it and passed it through. Meanwhile
governed property lines, the ones with real reuse and a real predicted next
read, were being displaced by a structural stream that gains nothing from
residency at this capacity.

That is a defect in how the masks are *used*, not in the masks. It is entry
**C3** in the taxonomy, and giving ungoverned lines strict eviction precedence
recovers 17.61% of SpMV kernel transfers and 23.29% of PageRank kernel
transfers with no new storage. The full result, the decision split and the
hardware accounting are on the
[governed-first eviction page](Governed-First-Eviction).

One candidate cause remains open for the dense kernels: **insertion**. The
record bound is used to choose a victim but not to place an incoming line, and
that has not been measured.

## Filtered kernels: BFS, SSSP and BC have an information problem

For the frontier-filtered kernels the diagnosis is the opposite, and it is
worse, because it is structural rather than a policy oversight.

Records are built well and then lose almost all of their value before they are
read. Two independent losses compound:

| | BFS | BC | SSSP |
|---|---:|---:|---:|
| Governed ways holding a stored bound | 49.6% | 63.4% | 27.0% |
| Of those, expired against skipped progress | **72.3%** | **74.4%** | **71.1%** |
| Live bound at eviction | 23.1% | 26.6% | 8.9% |

The derived live share, stored coverage times one minus the expired fraction,
reproduces the independently measured per-eviction coverage exactly, so the two
censuses corroborate each other rather than repeating one instrument.

By contrast **SpMV expires exactly 0.0%** of the bounds it holds. Expiry is
specific to filtered traversal, not a general property of the encoding.

### Expiry is not a clock artifact

A filtered pass advances two counters. `watermark()` returns `progress_`, which
advances over skipped positions as the traversal declines to visit them;
`deliveredWatermark()` returns `watermark_`, which advances only on delivered
commits. If expiry were an artifact of comparing deadlines against the faster
counter, the slower one would rescue it.

It does not. An opt-in delivered-clock comparison, wired so that one accessor
feeds both the victim-state collapse and the sequence handed to `selectVictim`
and the two therefore cannot disagree, moves expiry by **0.7 points**: 72.3% to
71.6%. The code explains the size. The gap between the two counters is bounded
by the commit queue — sixteen slots — and that lag is negligible against
graph-scale sequences and shrinks proportionally as the graph grows.

### Nor is it rescuable by a grace window

The natural next move is to keep a slightly expired bound rather than discard
it. A passive instrument measured how far past its deadline an expired bound
sits, normalised by the length of the record stream so the quantity is
scale-free:

| Kernel | Mean distance behind, as a share of the record stream |
|---|---:|
| BFS | 39.8% |
| BC | 17.5% |
| SSSP | 37.7% |

Expired bounds are 17–40% of the whole traversal in the past. No grace window
or clock adjustment recovers a prediction that stale, and a bound that stale is
not predictive of anything.

The cause is visible in the mechanism rather than inferred. **The predicted next
potential read simply does not happen.** A filtered kernel declines to visit
most of the vertices whose reads the bound anticipated, the line sits resident
while the stream sweeps far past its deadline, and the bound the cache is
holding describes an event that was cancelled. This is entry **C2a**.

Frontier conditioning was the design intended to fix exactly this, and it was
[measured inert](Frontier-Cohort-Mask).

## What each group implies

The two groups do not share a remedy, and the honest consequence is that they
should not share a claim either.

For **SpMV and PageRank** the mechanism is sound and the action was wrong. That
has been corrected and measured, and insertion remains an untested candidate
for more.

For **BFS, SSSP and BC** the bound is the wrong quantity to publish under
filtering. A useful mask would have to predict the next *actually visited*
read rather than the next potential one, which is a different producer and not a
tuning of this one. Until such a producer exists, the per-algorithm claim for
these three stays where the
[per-algorithm table](Evaluation-Methodology#per-algorithm-performance) puts it,
and nothing on this page should be read as a competitiveness claim for them.

## Reproducing the readings

The censuses are emitted by every `cache_sim` receipt as passive counters that
no policy consults: `ecg_record_victim_*` for the decision split and
`ecg_record_ways_*`, `ecg_record_state_*` and `ecg_record_expired_*` for the
coverage and staleness censuses. The profiles that produced the tables above are
`ecg_victim_attribution_cache` and `ecg_expiry_attribution_cache` in
`scripts/experiments/ecg/experiment_manifest.json`.

```bash
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_victim_attribution_cache --run-dir <run-dir>
```

The instrument is passive by construction and by test: `testVictimTraceIsPassive`
in `bench/src_sim/test_ecg_record.cc` drives 20,000 randomized selections and
requires the chosen victim to be identical whether or not a trace is attached.
An earlier version of the expiry counter read the collapsed way state rather
than the stored record metadata and reported exactly zero; two censuses that
disagreed caught it. Counters on this page are read from the side of the
transformation they are meant to observe.
