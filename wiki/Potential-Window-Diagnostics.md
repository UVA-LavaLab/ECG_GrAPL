# Observation-only potential-window diagnostics

The window observer evaluates information availability at actual GRASP
evictions without changing the active policy. It is not an implementation
of window replacement, a native interface, or a performance result.
It is restricted to unweighted, sorted top-down BFS with one U32 depth
array and a 64-byte-aligned base.

## Fixed graph-only profile

Source rows are grouped into power-of-two cohorts:
`C = next_power_of_two(max(8, ceil(vertices/256)))`, with `Q=C/8` rows
per endpoint bin. Patents uses C=16,384, Q=2,048 and 1,844 bins per pass.

One ten-bit logical token describes the nearest future occupied cohort:
known bit, three-bit exact cohort gap, three-bit distinct-row strength
(1..7 or 8+), and a three-bit rounded exclusive endpoint. Gap overflow
becomes UNKNOWN, never a nearer fabricated cohort. Same-row, same-line
duplicates reuse the token frozen before the row is inserted into the
reverse summary. Counts refer to structural potential, not actual frontiers.

The observer computes these tokens from the immutable graph, without
executing BFS first. Its diagnostic side table has two bytes per edge,
with eight bytes of reverse scratch per target line. This is not a
deployed extra metadata stream: its reads and construction are explicitly
excluded from active cache accounting to preserve GRASP's original history.
No current FINITE/WRAP token or native record is reinterpreted as a window.

## Runtime observation and timing limits

The observer receives the actual pass, sorted source row and designated
depth access. The source/index association is checked. A const cache
callback observes the actual victim and final RRIP candidate set after
GRASP has aged it exactly once. The callback cannot return a replacement
victim or modify any cache-line state.

The current diagnostic retains five cache-sized shadow views. The first two
preserve the original observation as a reference:

| View | Meaning |
|---|---|
| Immediate | Window available immediately after an actual designated access, if its line is resident in the LLC |
| Delivered | Eight access-step update delay, one output per step, sixteen bounded entries, no coalescing; newer observations hide pending/older information |
| Preserved immediate | Zero-delay reference in which a depth write does not discard existing graph-static potential |
| Preserved delivered | Retain only already-published potential across a depth write; pending hints are cancelled, never restored |
| Forwarded delivered | The immutable hint from an explicitly matched depth read is published under its conditional store's newer event order |

Ordinary depth reads and writes invalidate the first two views. Ordinary
reads still invalidate the preserved views. Every write advances the
event-order cutoff in all views: a delayed older hint cannot overwrite or
revive a newer observation. Keeping a published window does not refresh its
endpoint, recency or strength, and does not revive an expired or old-pass
window. Filling or evicting
a real line updates shadow residency, so metadata cannot survive a lost
residency or apply to a different line occupying the same way. Expiry and
pass changes are checked lazily; unknown, invalidated, pending, expired
and old-pass states are reported separately.

The forwarded view uses an explicit checked-write hook in the BFS edge
operation. It must match the read's structural index, exact element address,
U32 binding/width, source row and pass, with no intervening modeled memory
access. Matching just a cache line is insufficient. An unmatched or
unrelated write keeps the original invalidation behavior; a supported BFS
run fails its receipt gate if a requested association is rejected.

The old load update is still superseded. Only the matching store event
carries the captured, absolute window payload under its newer order.
Another observation, including UNKNOWN, can supersede that store update
before delivery. Source/pass changes clear the one outstanding read
association. The prototype has one immutable depth binding, so cross-binding
associations are rejected rather than assuming a free generation change.
No-store edges retain their ordinary read publication.

There is still one queue event per actual property access: the store event
is a write barrier in the reference views and a paired publication only in
the forwarded view. This does not replay an old event, refresh an endpoint,
or change any algorithm value, memory request or GRASP decision.

Source-bin markers drain the shadow queue and add eight shadow service
steps; begin/close and jumps are counted. They never stall or change the
real cache model. Marker traffic is reported as 48 logical control bytes
per exchange, not DRAM traffic. This is a timing diagnostic, not a
claim to model native retirement, overlap, interconnect delay or CPU cycles.

## Sampling and subsequent-read outcomes

Sampling is fixed at every 256th valid-line LLC eviction during graph
passes, not the first few evictions. It never consults future accesses.
The observer asks whether compatible live windows would distinguish the
actual base victim from another property way already at GRASP's eviction
RRPV. Farther cohorts and then weaker structural strength are poorer
retention candidates; ties and unrankable base victims retain GRASP's choice.
For a live base victim, the diagnostic separates absence of another eligible
depth way, eligible ways lacking live hints, equal window ranks, an already
worse-ranked base victim, and a strictly worse eligible alternative.
It also counts a worse-ranked live way outside the eligible set without
turning that way into a legal victim.

For a delivered-view disagreement, the observer records only that pair of
line addresses and its sampled endpoints. Each delivered view has its own
256-pair capacity, so adding the preserved view cannot displace reference
pairs. The first later
read to either line is observed during normal execution, with an explicit
131,072-memory-request horizon and pass-end censoring. Capacity drops,
censoring and unresolved outcomes are not counted as victories.
Private-cache reads, LLC-reaching reads and memory misses are distinguished.
Writes still invalidate window state, but do not resolve a read-order pair.
The preserved views use their explicit write rule instead. Counts of reads
before the sampled endpoint(s) do not imply that the same hint remained
live throughout the intervening execution.

These outcomes are not counterfactual miss savings. Changing a victim would
change later cache history, and a later read can hit a private cache. No
pair state feeds back into the policy, token generator or parameter choice.
There is no full address trace or future-derived mask artifact.

## Guarded execution and noninterference

`--window-observer control` records real demand/eviction digests without
window tables. `--window-observer window` adds the passive shadow analysis.
The default is `off`; incompatible algorithms, policies, records, weighted
inputs and native backends are rejected rather than silently downgraded.

The `ecg_window_observer` profile runs one full-Patents/source-0/8-MiB
condition with these two roles serially, under process-tree memory and
wall limits. It requires identical algorithm output/work, complete
cache counters, setup/kernel snapshots and demand/victim digests.
Input and binary hashes must also match. A changed placement or history
fails that gate; equal aggregate miss counts alone are insufficient.

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_window_observer \
  --run-dir results/ecg_experiments/runs/window_observer_case \
  --no-build --no-resume
```

Both roles declare `diagnostic_only=true`,
`measurement_scope=observation-only-unchanged-grasp` and distinct `OBS`
policy labels. Diagnostic storage is separately bounded at 128 MiB
by default, inside the algorithm's workspace reservation. The raw report
contains aggregate and per-pass sample counts, state histograms, bounded
queue/trial accounting and immutable-stream digests.
Schema `ecg.window-eviction-observer.v2` adds write-survival and candidate
attribution; v3 adds checked store publication and a third independent
256-pair book. The earlier runs retain their original receipts.
The extra shadow values and ordering state are diagnostic storage, not
proof that simultaneous hint retention and an order cutoff fit the proposed
67-bit native payload. That hardware contract remains unqualified.
Likewise, the one-slot read/store association is a diagnostic software
contract, not proof of native speculative-execution, translation, retirement
or store-buffer association. Those costs and failure paths need a separate
interface qualification before a real implementation is admitted.

Passing noninterference establishes trustworthy observation of this one
baseline history. It does not authorize enabling the window policy,
choosing parameters from held-out results, or claiming superiority over
[the existing GRASP/P-OPT controls](Traversal-Metadata-Taxonomy#matched-grasp-and-p-opt-comparison).

## Full Patents observation

The run `results/ecg_experiments/runs/window_observer_patents_7cd262d5`
uses the original v1 observer at source `7cd262d5`, stored BFS source 0, and the existing 8 MiB/16-way
LLC with 32 KiB L1D and 256 KiB L2, both eight-way. Both roles execute
all seventeen levels and return depth digest `3865ba0b18ab9fe2`.

The paired audit gate passes: identical output/work, complete cache
counters, setup/kernel snapshots, demand digest `f687a6da1055b468` and
victim digest `2d0587da55b70715`. The kernel remains at 438,095,550
requests, 13,052,296 memory misses and 13,416,757 transfers. These are
unchanged GRASP counts, not window-policy gains.

Known tokens accompany 31,499,996 of 33,023,480 actual designated depth
reads (95.39%). At evictions, the situation is different. The fixed sample
contains 42,775 decisions from 10,950,597 graph-pass evictions; the
remainder of the kernel's evictions occurs outside those passes.

| Actual base-victim state, delayed view | Samples | Share of sampled decisions |
|---|---:|---:|
| Not depth data | 24,275 | 56.75% |
| Invalidated by ordinary depth accesses | 7,119 | 16.64% |
| Live compatible window | 6,431 | 15.03% |
| UNKNOWN token | 2,425 | 5.67% |
| Expired | 1,967 | 4.60% |
| Previous pass | 442 | 1.03% |
| No observed hint | 115 | 0.27% |
| Pending update | 1 | 0.002% |

Among the 18,500 sampled depth victims, 34.76% have a live window and
38.48% are invalidated. There are no ordinary depth reads in this scoped
run; the invalidations originate from 3,764,116 discovery writes.
That identifies a validity-rule question, not permission to remove
ordering or invalidation safeguards without a new contract.

Only **204 sampled decisions (0.477%)** have a strictly different
eligible victim under the reference window ranking. Both immediate and
delayed views find the same 204; delayed delivery removes just one live
eligible candidate across the sample. The queue peaks at five entries
and drains without loss. Under this shadow timing model, update latency
is not the main limiter at the sampled decision points.

| Subsequent-read observation for the 204 pairs | Count |
|---|---:|
| GRASP's evicted line read first | 54 |
| Proposed alternative read first | 13 |
| Neither read before the fixed horizon | 137 |
| Capacity drops / pass-end censoring | 0 / 0 |

All 67 resolved first reads reach the LLC. The 54 base-line reads miss in
memory; two of the thirteen alternative-line reads do. This is read-order
evidence under unchanged GRASP, not an accuracy percentage or an estimate
of saved misses. In particular, 67.16% of pairs remain unresolved at the
declared horizon, and subsequent reads are not required to occur before
the annotation's endpoint. Do not count censored pairs as successes or
extend the horizon after seeing the outcome.

The observer reserves 69.88 MiB of diagnostic host state, including the
66,075,788-byte token side table and 5,242,880-byte dual shadow. It counts
15,507 source-control exchanges, or 744,336 logical control bytes, only
in the shadow model. These costs are excluded from active cache counters
by design and cannot support an end-to-end performance or hardware claim.

**Decision:** the producer passes the bounded functional and structural
checks, but this consumer exposes little actionable separation on the
observed history. Keep the window policy disabled. The next design review
should distinguish preservation of graph-static potential after depth
updates from restrictions in the base candidate set. Expanding property
priority indiscriminately or claiming that more metadata bits solve the
problem is not justified by this result.

## Write-validity and candidate attribution

The follow-up run
`results/ecg_experiments/runs/window_validity_patents_30b9c31d` uses the same
graph, source and cache condition with the v2 diagnostic. Both new roles
pass exact noninterference. The original views also reproduce the prior
sample counts and read-order outcomes under the same demand/victim
digests. No active policy, token parameters, sample interval or read horizon
was changed.

### Discovery writes arrive before publication

The conservative preserved-delivery view retains only an already-published
window. The actual write observations are:

| Discovery-write situation | Count |
|---|---:|
| Published live window available to retain | 0 |
| Pending update cancelled by the newer write | 3,763,624 |
| No live/pending resident window | 492 |
| Total | 3,764,116 |

Thus 99.987% of discovery writes cancel a pending hint. In this BFS loop,
the depth probe is followed by its conditional discovery store before the
eight-step shadow delivery can publish the hint. The write advances the
ordering cutoff, so the older queued observation is rejected.
Pending updates can carry UNKNOWN as well as known tokens; this is not a
count of lost useful windows. Also, a new designated read replaces usable
delivered state with PENDING in this conservative model. A line may have had
an earlier published window, but the pilot does not keep a second copy
available through PENDING.

This clarifies the initial latency finding: almost no pending state is
visible *at eviction*, but an earlier load/store interaction has already
removed the information. Low pending occupancy at eviction does not rule
out timing-dependent information loss earlier in the operation.

| View | Live base victims / 42,775 samples | Hypothetical different victims |
|---|---:|---:|
| Original delivered | 6,431 (15.03%) | 204 (0.477%) |
| Preserve published, delayed | 6,431 (15.03%) | 204 (0.477%) |
| Preserve potential, zero-delay diagnostic | 11,405 (26.66%) | 409 (0.956%) |

Preserving already-delivered state cannot help when no such state exists
at the write. The zero-delay view is an information-availability
diagnostic, not an implementable latency assumption or a measured cache
gain. Even it changes fewer than 1% of sampled decisions under the fixed
tie-only consumer.

### Candidate eligibility is more restrictive than equal ranks

The 6,431 live base victims in the delayed view partition as follows:

| Reason for the reference choice | Samples | Share of live base victims |
|---|---:|---:|
| No other eligible depth way | 4,470 | 69.51% |
| Other eligible depth ways lack live hints | 1,379 | 21.44% |
| All live eligible alternatives have the same rank | 78 | 1.21% |
| Base already has the poorest retention rank | 300 | 4.66% |
| Strictly worse-ranked eligible alternative exists | 204 | 3.17% |

In 2,539 no-change decisions, a worse-ranked live depth way exists outside
GRASP's final eligible set. This overlaps the no-change categories above;
it is not another set of safe swaps or a savings estimate. Those protected
ways were never selected by the observer. Only six of 9,585 live eligible
candidate instances have saturated structural strength, so count saturation
is not the dominant explanation for lost choices in this sample.

Both delayed views have the same 204 read-order pairs: 54 base-first,
13 alternative-first and 137 horizon-censored, with no capacity drops.
All 67 resolved first reads precede that line's sampled endpoint; 52
base-first and seven alternative-first reads precede both endpoints.
These endpoint comparisons do not prove that the same hint remained valid
through all intervening accesses or predict counterfactual cache behavior.

### Next design decision

The next publication contract to examine is a single checked edge operation
covering the depth probe and its conditional write, or equivalent
store-associated forwarding of the immutable graph hint under the store's
new event identity. It must prove matching record, target, profile, pass and
generation, preserve later-event precedence, and never resurrect an
arbitrary old queued hint. The forwarded passive view models this checked
association; no native publication interface is implemented by this study.

Publication and victim eligibility are separate problems: fixing the former
alone does not remove the tie-only bottleneck. Any broader candidate rule
must be evaluated explicitly rather than silently weakening GRASP's
protection. Window replacement remains disabled, and the joint native
hint/ordering storage contract remains unqualified.

## Checked publication observation

`results/ecg_experiments/runs/window_publication_patents_7939207d` evaluates
the forwarded view at source `7939207d` on the same Patents/source-0/8-MiB
condition. Both roles pass exact noninterference. The original four views,
two pair books, cache counters and demand/victim digests also reproduce
the preceding observation; only the additional view changes interpretation.

All 3,764,116 discovery stores match their checked read, with no rejected
associations. The observer forwards the captured absolute window under the
store event, not by accepting the superseded load update.

| Store-associated publication | Updates |
|---|---:|
| Issued | 3,764,116 |
| Issued with a known token | 3,432,888 |
| Applied while resident and current | 3,762,587 |
| Applied with a known token | 3,431,536 |
| Superseded by a newer observation | 654 |
| Target not resident at delivery | 875 |

Applied, superseded and absent updates sum to issued updates; the known-token
rows are subsets, not additional dispositions. UNKNOWN publications remain
UNKNOWN. There are still 36,787,596 queue events and a peak occupancy of
five: the new view changes the meaning of the existing write event rather
than adding another event. Native association/tag transport is not thereby
free or already implemented.

The delayed views retain eight-step per-update latency; comparison with
the zero-delay reference is:

| View | Live base victims / 42,775 samples | Hypothetical different victims |
|---|---:|---:|
| Original delivered | 6,431 (15.03%) | 204 (0.477%) |
| Preserve published, delayed | 6,431 (15.03%) | 204 (0.477%) |
| Checked store publication, delayed | 11,405 (26.66%) | 409 (0.956%) |
| Preserve potential, zero-delay diagnostic | 11,405 (26.66%) | 409 (0.956%) |

Checked publication restores the sampled base-window availability and
choices of the zero-delay reference without removing delivery latency or
later-event precedence. It does not establish a cache-performance gain:
GRASP still executes every actual victim choice.

The 409 forwarded-view read-order pairs yield 96 base-first reads, 21
alternative-first reads, and 292 horizon-censored pairs (71.39%), with no
capacity drops and a peak of eight pending pairs. All resolved reads reach
the LLC; the 96 base-first reads and three alternative-first reads miss in
memory. Of these, 94 base-first reads precede their own sampled endpoint
and 89 precede both endpoints; all 21 alternative-first reads precede
their own endpoint and twelve precede both. These remain retrospective
observations, not 96 saved misses or an accuracy percentage.

Candidate eligibility still limits this consumer. Of the 11,405 live base
victims, 7,792 have no other eligible depth way and 2,287 have eligible depth
ways without live hints. Only 154 are blocked solely by equal ranks; 763
already have the poorest retention rank, leaving the 409 strict alternatives.
Another 4,245 no-change decisions have a worse-ranked live way outside the
eligible set. Those ways were not selected and must not be counted as safe
evictions.

**Decision:** the checked-publication semantics pass this passive gate.
The next question is the risk and usefulness of a separately specified
candidate rule, not additional token bits or accepting old updates.
Keep active window replacement disabled until that rule and its costs are
qualified. The 64-byte, single-read association slot is diagnostic software
state; it does not prove a native pipeline implementation or compliance
with the proposed hardware budget.
