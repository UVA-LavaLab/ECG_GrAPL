# Governed-first eviction: giving reuse-free lines strict precedence

This cache-only study fixes one defect in `ecg_record::selectVictim` that the
[mask failure taxonomy](Mask-Failure-Taxonomy) located by measurement: on 39.1%
of SpMV evictions and 34.7% of PageRank evictions the victim the record rule was
handed was not a governed line at all, so the rule passed it through while
governed property lines were displaced by a structural stream.

**The arm won on both kernels at 8 MiB/16-way**, and the capacity qualifier is
part of the result rather than a hedge. A second reading at 24 MiB, reported in
[how the result depends on capacity](#how-the-result-depends-on-capacity),
**loses to GRASP on both kernels and regresses on SpMV**, and a four-point sweep
places the crossover between 16 and 20 MiB. The mitigation is measured to be
capacity-pressure dependent.

Five later studies on the same graph and capacities are reported here too:

- **A pressure signal from set dueling failed its frozen gate.** It was safe on
  SpMV at 8 and 16 MiB only, safe on PageRank at no capacity, and it regressed
  on PageRank at 20 and 24 MiB. See
  [the set-dueling result](#a-pressure-signal-from-set-dueling-failed).
- **An RRPV order passed its frozen gate on both kernels.** Every choice the
  record does not decide is taken from GRASP's RRPV bits instead of recency.
  With it, ECG leads the favourable P-OPT control at all four capacities on
  both kernels, thinly on SpMV at 8 MiB. It leads GRASP on SpMV at 8 and
  16 MiB and on PageRank at 8, 16 and 20 MiB, the last thinly. GRASP still
  leads SpMV above 16 MiB and, thinly, PageRank at 24 MiB. See
  [the RRPV order](#the-rrpv-order-no-choice-reads-recency).
- **A passive kernel census reran the RRPV-order rows.** GRASP's SpMV lead
  above 16 MiB comes from the state the kernel starts in, not from the second
  pass's decisions. Above 8 MiB the RRPV order's lift over governed-first is
  writebacks deferred past the kernel's end. On a boundary that charges them,
  that lift is 0.02% or less. See
  [the kernel census](#the-kernel-census-entry-state-and-deferred-writebacks).
- **Over eight passes after one construction, the settled passes tie GRASP at
  20 and 24 MiB.** Once the kernel has settled, the RRPV order ties GRASP there
  on both kernels, and no ECG row's mean per settled pass exceeds either
  baseline's. From 16 MiB up ECG settles on the same per-pass floor on both
  kernels. GRASP's remaining leads sit at the kernel's ends: SpMV's first pass,
  and PageRank's last, where wrapped bounds decode to DEAD. See
  [the steady state](#the-steady-state-eight-passes-after-one-construction).
- **PageRank's added last-pass misses are its own in-place writes.** A passive
  count charges each line that DEAD-first eviction retired and a later miss
  fetched again to the access that fetched it. From 16 MiB up the last pass's
  added property misses are the kernel's in-place write fetching back retired
  lines, and no gather fetches one back. A one-bit declaration that would stop
  the retirement is in the codec and is not measured. See
  [the re-fetch count](#the-re-fetch-count-pageranks-added-last-pass-misses-are-in-place-writes).

The [hardware cost](#hardware-cost) of the rule as first measured has been
**corrected**. It assumed the cache already keeps a recency order, and a GRASP
cache, which is SpMV's base here, does not. The RRPV order removes that
dependence.

Every gated result is about `selectVictim` against its own previous ordering,
and the matched comparisons are functional cache results on one graph. None is
a timing result: `timing_valid_for_speedup` is `false` in every receipt quoted
here.

> **Historical method.** Every comparison against GRASP_PAPER on this page,
> and the P-OPT contrasts beside it, was measured under a protocol that has
> since been corrected. Two things changed:
>
> - **Registration.** GRASP_PAPER registered every property array as a GRASP
>   region, so the never-reused destination arrays (SpMV's `y`, PageRank's
>   `scores`) held up to half the last level each. Upstream GRASP registers
>   only the arrays each phase gathers.
> - **Kernel entry.** The shared algorithm kernels began from whatever state
>   their setup left, which differs between a CSR row and a record row.
>
> The corrected protocol is opt-in, as `--grasp-registration declared` and
> `--kernel-entry cold`; see [status and default](#status-and-default). These
> margins have not been re-measured under it. Read them as the record of the
> historical protocol, not as current claims.

The rule is off by default, as `--record-governed-first no`. So is the RRPV
order, as `--record-rrpv-order no`.

## The one difference under test

`selectVictim` evaluates a priority list. Before this change it read:

1. an invalid or DEAD way, if one exists;
2. otherwise the base victim from the underlying policy — LRU or GRASP;
3. replace that victim only if some other governed way has a **strictly
   farther** decoded bound.

Step 2 is where the capacity was going. The base policy answers a question
about recency or region, not about whether a line is governed, so it regularly
returns a structural line. That is not wrong of it — but it means the record
rule never gets to express a preference between a governed line with a live
bound and an ungoverned line with no bound at all, because the base policy has
already chosen for both of them.

`--record-governed-first on` inserts one step after the DEAD scan: if any way
in the set is **ungoverned**, evict the least-recently-used ungoverned way and
stop. Only when every way is governed does the base victim and the
strictly-farther override run as before.

```cpp
if (options.governed_first) {
    const std::size_t ungoverned = options.rrpv_order
        ? grasp_scan(ungoverned_way) : pick(ungoverned_way, by_recency);
    if (ungoverned != count) {
        victim = ungoverned;
        if (trace) trace->path = VictimPath::UNGOVERNED_FIRST;
        return Status::OK;
    }
}
```

`pick(ungoverned_way, by_recency)` is the least-recently-used ungoverned way, and
it is the rule that the sections up to the dueling study measure. `grasp_scan`
belongs to [the RRPV order](#the-rrpv-order-no-choice-reads-recency), a later
option that is off by default.

Nothing else changes. The record grammar, the 4-byte and 8-byte layout
selection, the window rule, insertion, hit promotion, recency, dirty state and
the DEAD scan are untouched, and the option is a single trailing field on
`VictimOptions` so the default path is byte-identical to the previous rule.

## Frozen four-cell study

The `ecg_governed_first_cache` profile runs exactly four cells: SpMV and
PageRank, each with the arm off and on, everything else identical. Fixed
prepared DBG Patents graph, 32 KiB L1D and 256 KiB L2 both eight-way, 8 MiB
16-way LLC, 64-byte lines, no prefetch, one simulator thread, 512 MiB
workspace, 2 GiB process-tree RSS, 1800 s per cell.

The control is the **same-build `no` arm**, never an earlier study. The paired
arms differ in exactly one option.

The gate was written before any cell ran and applied once afterwards:
`lift = (base_first - governed_first) / base_first` on kernel transfers, with
`lift >= 0.02` required on **both** kernels to confirm the taxonomy entry, a
margin on one kernel only reported as partial and not generalised from the
winner, `lift < 0.02` on both recorded as a negative result rather than widened
to other kernels, and any negative lift reported as a regression.

The two kernels happen to test the rule against **two different base victims**.
SpMV's base is `GRASP_PAPER`; PageRank runs as a separate kernel whose base is
its default LRU. That was not designed, but it widens the result rather than
narrowing it.

## Measured Patents result

| Kernel | Base victim | `base_first` | `governed_first` | Lift |
|---|---|---:|---:|---:|
| SpMV | GRASP_PAPER | 14,916,849 | 12,289,362 | **+17.61%** |
| PageRank | LRU | 16,961,478 | 13,011,200 | **+23.29%** |

Both clear the 0.02 threshold, so by the frozen rule the C3 taxonomy entry is
confirmed: non-property eviction precedence was a real defect in the victim
rule and this is a real mitigation for it.

SpMV's setup-inclusive traffic is reported rather than gated: 34,111,950 to
31,484,463, **+7.70%**, with the setup phase **byte-identical** at 19,195,101 in
both arms, as it must be since the option only changes eviction during the
kernel. PageRank's carrier is prepared outside the measured run, so its kernel
figure is its whole figure, and no policy's preprocessing appears in its rows.
Its gate is arm-to-arm; the matched baselines are reported separately below, and
its number needs the context in
[what PageRank's margin is](#what-pageranks-margin-actually-is) to be read
correctly.

The arms compute the same answer. SpMV's `result_digest` is
`55e3fc26a1027ddb` in both; PageRank's `pr_score_checksum` is
`6249d06ef4cc2ed7` in both, with `pr_result_matched` set. A replacement policy
that changed the result would be a defect, not a win.

## Why it works: reuse-free lines cost nothing to evict early

Decomposing the kernel transfers explains the whole effect and is the finding
worth carrying forward.

| | SpMV base | SpMV arm | Change | PageRank base | PageRank arm | Change |
|---|---:|---:|---:|---:|---:|---:|
| Property misses | 9,367,843 | 6,739,182 | **−28.06%** | 10,978,616 | 7,038,256 | **−35.89%** |
| Other misses | 5,072,977 | 5,072,792 | −0.004% | 5,073,432 | 5,073,432 | **0.000%** |
| Writebacks | 476,029 | 477,388 | +0.29% | 909,430 | 899,512 | −1.09% |

PageRank's structural miss count is identical **to the digit** across the two
arms, and SpMV's moves by 185 out of 5.07 million. Evicting structural lines
first costs nothing measurable, because at 8 MiB and 16 ways the CSR index and
edge stream has no reuse the cache can capture in any case. The entire gain
lands on the property array, which does have reuse and was being displaced by a
stream that gained nothing from residency.

Stated as residency rather than traffic: governed ways per decision rise from
13.21 of 16 to 15.39 of 16 on SpMV, and from 12.66 to 15.52 on PageRank.

## What PageRank's margin actually is

PageRank's +23.29% is correctly gated against its same-build control, but it is
largely **not new benefit**, and reporting it without this would overstate it.

The same PageRank configuration — same graph, 33,037,894 records, 177,448,792
total accesses, `pr_score_checksum` `6249d06ef4cc2ed7`, identical structural
misses — measured 12,865,017 kernel transfers in September builds and 16,961,478
today. The cause is a deliberate correctness fix. Earlier, an `UNKNOWN` record
ranked as infinity and so made its line effectively immortal: the mechanism
protected lines it knew **nothing** about. That rule was removed, leaving the
override to compare only two live property futures, and the cost is measurable
at **+31.84%** on PageRank.

| PageRank, same graph and geometry | Kernel transfers |
|---|---:|
| With `UNKNOWN` pinning lines (removed) | 12,865,017 |
| With `UNKNOWN` LRU-neutral — today's control | 16,961,478 |
| **With `UNKNOWN` LRU-neutral plus governed-first** | **13,011,200** |

Governed-first recovers **96.4%** of what the fix cost. So the honest statement
is that it re-earns, through a rule that can be defended in hardware — evict a
line the mechanism does not govern before one it does — most of a benefit that
had previously come from a rule that cannot be defended at all.

The matched baselines sharpen this further, and in ECG's disfavour. The
`GRASP_PAPER` PageRank cell landed at 15,975,818 against 15,976,776 in the
earlier cross-build run, a drift of 0.006%, so that run's figures are sound and
the 12,865,017 above is a fair comparison point. It **already beat the matched
`POPT:UNCHARGED` by 7.32%.** ECG's PageRank advantage over P-OPT therefore
predates governed-first: the rule does not create that advantage, it *restores*
it, arriving 1.14% short of the old figure while earning it legitimately. That
is the claim this page makes, and it is smaller than the +23.29% arm-to-arm lift
on its own would suggest.

The census also explains why SpMV was almost untouched by that same fix, which
is why its figures reproduce byte-identically across builds while PageRank's
did not: SpMV inspects 0.00114 unknown ways per eviction decision and PageRank
inspects 0.18073, a factor of 159. The two observations corroborate rather than
conflict.

## Decision split

The passive attribution counters, which no policy consults, record which branch
of the priority list produced each victim.

| | SpMV base | SpMV arm | PageRank base | PageRank arm |
|---|---:|---:|---:|---:|
| Decisions | 14,357,536 | 11,741,508 | 15,954,097 | 12,043,490 |
| `dead_first` | 84,267 | 88,864 | 87,650 | 98,718 |
| `ungoverned_first` | 0 | 5,615,905 | 0 | 5,499,531 |
| `base_not_governed` | 5,607,869 | 0 | 5,541,309 | 0 |
| `base_no_future` | 5,379 | 717 | 244,553 | 261,492 |
| `base_kept` | 2,323,901 | 1,348,395 | 380,589 | 138,214 |
| `overridden` | 6,336,120 | 4,687,627 | 9,699,996 | 6,045,535 |

`ungoverned_first` is zero on both controls and positive on both treatments,
which is how the arms were confirmed to have taken effect before the gate was
read at all. The four rows also carry four distinct output labels, so no arm
overwrote its pair.

The split contradicts the naive reading of the mechanism, and the correction
matters. The **absolute** count of ungoverned evictions barely moves — 5.61M to
5.62M on SpMV, 5.54M to 5.50M on PageRank — because the base policy was already
choosing an ungoverned line about that often. What changes is the **share**,
since the total number of decisions falls by 2.6M and 3.9M: ungoverned evictions
rise from 39.06% to 47.83% of decisions on SpMV and from 34.73% to 45.66% on
PageRank. Fewer property misses mean fewer evictions, and each surviving
eviction is likelier to land on a structural line.

`overridden` falls as well, not because the strictly-farther override weakened
but because fewer decisions now reach it.

Expiry is **exactly zero** on both kernels in both arms, consistent with the
taxonomy: this mitigation and the filtered-kernel staleness problem are
disjoint, and neither is evidence about the other.

## Hardware cost

One **governed bit per line**. It is already present as `WayState::property`,
and the existing record rule already reads it.

A **recency order**. The rule as measured here reads it in three places: to
choose the least-recently-used ungoverned way, to break ties among DEAD ways,
and to break ties between equal bounds in the override. Its cost depends on the
base policy.

- **On an LRU cache** the order already exists. The ungoverned scan then has the
  shape of the LRU victim scan, over a set that has already been read out for
  the replacement decision, and it can share that comparator tree. That is
  PageRank's measured configuration, whose base victim is LRU.
- **On a GRASP cache** it does not exist. GRASP keeps a 3-bit RRPV per line, not
  a recency order, and SpMV's confirming cells ran on a `GRASP_PAPER` base.
  There the rule as measured needs **a second replacement state** beside the
  RRPV bits, for example a 4-bit age per line in a 16-way set, kept up to date on
  every access to the set.

**Correction.** An earlier version of this section said, without qualification,
that the rule needs no new storage and can share the LRU comparator tree. That
holds only on an LRU cache. It was not true of the configuration that produced
SpMV's result.

[The RRPV order](#the-rrpv-order-no-choice-reads-recency) removes the recency
order. It builds every choice the record does not decide from the RRPV bits a
GRASP cache already keeps, at the cost stated in that section.

Under either order: no new ports, no new metadata traffic, no change to the
record codec, no change to the window rule, and no change to what the producer
publishes. The rule is a reordering of an existing priority list, evaluated
after the DEAD scan and before the base victim.

## What this result does not claim

- **The frozen gate is arm-to-arm.** The matched baseline comparison in the
  next section was authorized and gated separately, as a reproduction check; it
  is reported as a measured ROI comparison at 8 MiB/16-way, not as a general
  competitiveness claim for the complete design.
- **Not capacity-general.** A preregistered reading at 24 MiB reverses the
  comparison against GRASP on both kernels. See below; it is a measured
  boundary, not an untested limit.
- **Nothing about BFS, SSSP or BC.** They were excluded from the study because
  they lack a live bound at the moment of choice and an ordering rule cannot
  supply one. Governed-first may still pay there for the same reuse-free-stream
  reason, but that is an untested hypothesis and extending the claim would need
  its own study and its own gate.
- **Nothing about insertion.** The record bound is still not used to place an
  incoming line. That remains the open candidate for the dense kernels.
- **No PageRank preprocessing accounting.** PageRank's rows omit it for every
  policy, so no break-even can be derived there as it can for SpMV. That
  accounting is an open item.
- **Nothing about another graph, another capacity, another iteration count or
  another base policy.** One configuration was measured.
- **Nothing about timing.** These are functional cache results.

## Matched comparison against the named baselines

The region of interest for this work is the **algorithm kernel**: the mask is
what the cache consults while the kernel runs, and building the mask is
preprocessing that precedes it. The receipts separate the two phases, the frozen
gate above is on kernel transfers, and so is the comparison here. Construction
is reported directly after, as a one-time cost with a break-even, because it is
a real cost and hiding it would be dishonest — not because it is the metric the
mechanism is judged on.

The `ecg_matched_baseline_cache` profile ran `GRASP_PAPER` and `POPT:UNCHARGED`
on the same graph, repeat count and geometry as the governed-first cells. Its
gate was a reproduction check frozen before the run — both baselines within
0.5% of their recorded figures — and both reproduced at **zero drift**:
15,456,930 and 12,398,416 exactly, setup 2,877,491 and 9,293,507 exactly,
`result_digest` `55e3fc26a1027ddb` throughout.

The comparison is therefore not a cross-study inference. `benchmark_binary_sha256`
is `33b3620a...` for the ECG arms and for both baselines, and `graph_sha256` is
`991199a8...` for every row: **the same executable read the same graph bytes.**

| SpMV, **8 MiB**/16-way, 2 passes | Common setup | Mechanism prep | ROI kernel | ECG's ROI margin |
|---|---:|---:|---:|---:|
| GRASP_PAPER | 2,877,491 | 0 | 15,456,930 | **+20.49%** |
| `POPT:UNCHARGED` | 2,877,491 | 6,416,016 | 12,398,416 | **+0.88%** |
| **ECG-R/G governed-first** | 2,877,491 | 16,317,610 | **12,289,362** | — |

Matched PageRank baselines were then run the same way, on the same prepared
graph with the same argv and the same unrebuilt kernel. PageRank receipts carry
no setup phase for **any** policy, so this comparison counts no preprocessing at
all — not the ECG carrier, and not P-OPT's matrix, which for PageRank reports
`popt_matrix_bytes` of 0 and an `analytic` stream mode. P-OPT therefore receives
its future information with no storage and no stream cost, the most favourable
form available, and the ECG carrier cost is real but uncounted here.

| PageRank, **8 MiB**/16-way, 2 iterations | ROI kernel | ECG's ROI margin |
|---|---:|---:|
| GRASP_PAPER | 15,975,818 | **+18.56%** |
| `POPT:UNCHARGED` | 13,880,532 | **+6.26%** |
| ECG base-first (no governed-first) | 16,961,478 | −5.79% |
| **ECG governed-first** | **13,011,200** | — |

**At this capacity, on PageRank, the rule is the difference between losing and
leading.** Without it, ECG is 6.17% worse than GRASP and 22.20% worse than the
favourable P-OPT control. With it, ECG leads both, and the P-OPT margin of 6.26%
is seven times SpMV's. At 24 MiB it leads neither.

Structural misses are **5,073,432 for every PageRank policy** — GRASP, P-OPT and
both ECG arms, identical to the digit. The structural stream is
policy-invariant, which is a direct corroboration of the reuse-free-stream
account above rather than a restatement of it: no replacement policy, however
informed, extracts anything from that stream.


Every policy pays a common 2,877,491 transfers before the kernel — loading the
CSR and initializing the property array — and LRU and GRASP pay nothing beyond
it. Above that floor sits each mechanism's own preprocessing: P-OPT's rank
matrix costs 6,416,016 and the ECG record carrier costs 16,317,610, which is
2.54 times P-OPT's.

On the ROI at this capacity the governed-first arm leads both baselines,
including the **favourable** P-OPT control — full data capacity, no ways reserved for the rank
matrix, matrix stream not replayed — which strengthens the reading rather than
weakening it. But the P-OPT margin is 109,054 transfers, or **0.88%**: that is a
crossing, not a comfortable lead, on one graph at one capacity, and it should be
read as a crossing.

### Construction as a one-time cost

Because the carrier is built once and consulted by every subsequent kernel
invocation, its cost amortizes, and the break-even is the honest way to state
it:

| | Prep deficit | ROI gain per measured kernel | Break-even |
|---|---:|---:|---|
| vs GRASP_PAPER | 16,317,610 | 3,167,568 | **~5 kernels** (~10 passes) |
| vs `POPT:UNCHARGED` | 9,901,594 | 109,054 | **~91 kernels** (~182 passes) |

ECG repays its carrier against GRASP in roughly ten SpMV passes; against P-OPT
it needs roughly 182, because the ROI margin there is thin. Both figures are
**linear extrapolations from a two-pass measurement, not measurements** — kernel
traffic is not guaranteed linear in passes, since the first pass is cold and
later ones warm. Measuring it is the job of
[reusable SpMV queries](Reusable-SpMV-Queries), and it must not be approximated
by enlarging `--repeat` and relabelling the result as independent queries.

Construction locality is where that deficit could shrink: construction reads the
graph 3.93 times over at a 21% hit rate, while its writebacks are already within
10% of the floor implied by the carrier's size. So the lever is the read path,
not a smaller carrier. No lower-cost construction mode is implemented today.

## How the result depends on capacity

The comparison was repeated at three further capacities, one change at a time,
on the same graph with the same argv. Gates and predictions were frozen before
each run, and the second and third capacities were chosen by a rule fixed before
the first was read. The result bounds the claim.

| Kernel transfers vs GRASP_PAPER | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| SpMV, ECG governed-first | **+20.49%** | **+8.22%** | −4.07% | −6.12% |
| PageRank, ECG governed-first | **+18.56%** | **+10.85%** | −1.07% | −2.90% |

**GRASP overtakes ECG between 16 and 20 MiB on both kernels.** That is reported
as an interval between measured capacities and not interpolated, because four
points do not license a fitted crossing. The competitive claim therefore holds
up to somewhere between 16 and 20 MiB on this graph.

The full comparison at the two ends:

| SpMV | 8 MiB | 24 MiB |
|---|---:|---:|
| GRASP_PAPER | 15,456,930 | 5,754,248 |
| `POPT:UNCHARGED` | 12,398,416 | 6,249,737 |
| ECG base-first | 14,916,849 | 6,035,369 |
| **ECG governed-first** | **12,289,362** | **6,106,480** |
| ECG vs GRASP | +20.49% | **−6.12%** |
| ECG vs P-OPT | +0.88% | +2.29% |
| governed-first over base-first | +17.61% | **−1.18%** |

| PageRank | 8 MiB | 24 MiB |
|---|---:|---:|
| GRASP_PAPER | 15,975,818 | 5,946,046 |
| `POPT:UNCHARGED` | 13,880,532 | 6,085,744 |
| ECG base-first | 16,961,478 | 7,174,510 |
| **ECG governed-first** | **13,011,200** | **6,118,682** |
| ECG vs GRASP | +18.56% | **−2.90%** |
| ECG vs P-OPT | +6.26% | **−0.54%** |
| governed-first over base-first | +23.29% | +14.72% |

**GRASP scales with capacity better than ECG does**, and that, rather than
P-OPT, is what ends the claim. ECG still leads the favourable P-OPT control on
SpMV at 24 MiB.

### ECG has a traffic floor; GRASP does not

| SpMV kernel transfers | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| ECG governed-first | 12,289,362 | 6,151,409 | 6,126,909 | **6,106,480** |
| GRASP_PAPER | 15,456,930 | 6,702,419 | 5,887,025 | **5,754,248** |

From 16 to 24 MiB, 50% more cache buys ECG **0.73%** and GRASP **14.15%**. ECG
is effectively flat from 16 MiB onward and GRASP falls straight through its
floor. Structural misses show where the floor comes from:

| SpMV structural misses | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| GRASP_PAPER | 5,073,432 | 5,063,595 | 5,007,113 | 4,945,273 |
| ECG governed-first | 5,072,792 | 5,072,792 | 5,064,601 | 5,072,792 |

GRASP converts capacity into structural hits, falling 128,159 monotonically
across the range. ECG's net change is **zero**, with a single 0.16% excursion, so
it converts essentially none. That is the rule working as designed — governed-first
evicts ungoverned lines first — and it is exactly right while the structural
stream has no capturable reuse and exactly wrong once it does.

### The mechanism holds; the ledger flips

The reuse-free account is not what breaks. Structural misses are identical **to
the digit** across arms at 24 MiB as well, and governed-first still cuts property
misses, by 9.6% on SpMV and 64.6% on PageRank. What changes is the other side:

| Governed-first vs base-first at 24 MiB | Property | Structural | Writebacks | Net |
|---|---:|---:|---:|---:|
| SpMV | −54,447 | 0 | **+125,558** | **+71,111** |
| PageRank | −911,816 | 0 | −144,012 | −1,055,828 |

Both reconcile to their totals exactly, so **the SpMV regression is entirely a
writeback effect**. Keeping property lines resident longer means more of them
are dirty when they finally leave, and SpMV writes `y[]` on each of its
11,324,304 property writes. That cost is invisible under pressure and dominant
without it:

| SpMV writebacks, base-first → governed-first | Change |
|---|---:|
| 8 MiB: 476,029 → 477,388 | +0.29% |
| 24 MiB: 396,970 → 522,528 | **+31.63%** |

The accurate statement of the mitigation is therefore narrower than a single
capacity suggests: **governed-first buys misses and pays writebacks, and the
trade is strongly favourable only while the cache is under capacity pressure.**
PageRank still beats its own base-first control at 24 MiB because its property
saving is an order of magnitude larger; SpMV does not.

Both effects have the same shape. Governed-first and the strictly-farther
override each trade residency against a predicted future, which is the right
trade when capacity is scarce and a pure loss when it is not. **They are
pressure heuristics applied without a pressure signal.** Two signals were then
built, each meant to relax the rule toward the base policy wherever the cache is
not pressured:

- **A 4-bit hit/miss counter per set.** Its study is **void**, not negative. The
  counter was keyed on a legacy property region, which for SpMV defaults to the
  output `y`. Nearly every kernel access to `y` misses, so every counter stayed
  saturated. The gate therefore read "pressured" on every decision, and the
  gated cell matched its control in every metric. The counter now keys on the
  lines the record rule governs. A test runs the real binary with the gate off
  and on and pins that behaviour. The study has not been rerun, so no result
  exists for the counter.
- **Set dueling.** It asks directly which rule moves fewer transfers. It was
  measured, and it **failed its frozen gate**; see
  [the next section](#a-pressure-signal-from-set-dueling-failed).

One further reading worth keeping. At 24 MiB, ECG base-first incurs **more**
property misses than plain GRASP — 565,607 against 472,501 — although GRASP is
its own base victim. The strictly-farther override is itself harmful at ample
capacity, independently of governed-first, which is a separate finding about the
record rule rather than about this mitigation.

## A pressure signal from set dueling failed

Set dueling makes one choice per cache, between the rule and a relaxed form of
it, by measuring both:

- **Leader sets.** Sets with `set & 63 == 16` always apply the rule. Sets with
  `set & 63 == 17` always relax: DEAD-first, then the base policy's own victim.
  Every other set follows a selector.
- **Selector.** One 10-bit saturating counter, starting at 511. A transfer in a
  rule leader adds one, a transfer in a relaxed leader subtracts one, and the
  followers relax while the counter reads 512 or more.
- **Transfer.** An LLC demand miss or a dirty victim, the same two components
  the kernel-transfer metric counts.

Hardware: the 10-bit counter, a 6-bit comparison on the set index, and one
increment or decrement per transfer the cache already makes. There is no
per-set or per-line state, no new port and no metadata traffic. `selectVictim`
stays stateless, because the signal only supplies its existing `pressured`
input.

The `ecg_pressure_duel_cache` study ran 32 rows, with one binary per kernel:
GRASP_PAPER, `POPT:UNCHARGED`, governed-first, and governed-first with the duel,
at 8, 16, 20 and 24 MiB on the same graph and geometry as the capacity sweep.
The gate was frozen before the run and applied once, on kernel transfers:

- `excess = (duel - best) / best`, where `best` is the lower of GRASP and
  governed-first;
- **capacity-safe** means `excess ≤ 0.01` at all four capacities;
- a duel above both GRASP and governed-first is a **regression**.

| Kernel | LLC | GRASP_PAPER | `POPT:UNCHARGED` | Governed-first | Duel | Best | Excess | Reading |
|---|---|---:|---:|---:|---:|---|---:|---|
| SpMV | 8 MiB | 15,456,930 | 12,398,416 | 12,289,362 | 12,339,443 | governed-first | +0.41% | safe |
| SpMV | 16 MiB | 6,702,419 | 6,249,737 | 6,151,409 | 6,162,935 | governed-first | +0.19% | safe |
| SpMV | 20 MiB | 5,887,025 | 6,252,319 | 6,126,909 | 6,048,250 | GRASP | +2.74% | fails |
| SpMV | 24 MiB | 5,754,248 | 6,249,737 | 6,106,480 | 5,970,105 | GRASP | +3.75% | fails |
| PageRank | 8 MiB | 15,975,818 | 13,880,532 | 13,011,200 | 13,193,583 | governed-first | +1.40% | fails |
| PageRank | 16 MiB | 6,900,452 | 8,263,074 | 6,151,504 | 6,213,698 | governed-first | +1.01% | fails |
| PageRank | 20 MiB | 6,068,180 | 7,178,381 | 6,132,881 | 6,163,017 | GRASP | +1.56% | **regression** |
| PageRank | 24 MiB | 5,946,046 | 6,085,744 | 6,118,682 | 6,133,975 | GRASP | +3.16% | **regression** |

**SpMV is partial.** It is safe at 8 and 16 MiB and fails at 20 and 24 MiB, and
the passing capacities are not generalised. **PageRank is not safe** at any
capacity. At 16 MiB it misses by 679 transfers, which is recorded as a miss and
not rounded, and it regresses at 20 and 24 MiB. The leader count, leader slots,
selector width, initial value and transfer definition have not been retuned.

Every GRASP, P-OPT and governed-first figure measured by an earlier study
reproduced exactly in this build, so the capacity tables above stand.

The receipts show why it failed:

1. **Where the followers stayed on the rule, the relaxed leaders account for the
   whole difference.** At SpMV 8 and 16 MiB and at every PageRank capacity, the
   duel's transfers above governed-first equal the relaxed leader sets' extra
   transfers over the rule leader sets to within 656. One set in 64 always runs
   the relaxed form, by design. On PageRank that form moves 1.9 and 1.6 times the
   rule's transfers in its sets at 8 and 16 MiB, so those sets alone put both
   cells past the 1% allowance.
2. **PageRank's relaxed form was DEAD-first plus LRU, not GRASP.** `pr`
   configures its record path without a base policy, so its base is the cache's
   default, LRU. Scaled by 64, its relaxed leader sets sit 19% to 55% above
   GRASP. Only 43 follower decisions ever went relaxed, all at 8 MiB. So the
   PageRank rows measure a duel against DEAD-first plus LRU, and they say
   nothing about a duel against a GRASP-based relaxed form.
3. **SpMV's relaxed sets do not behave like GRASP either.** They take DEAD-first
   and then GRASP's own victim, yet scaled by 64 they sit about 5% above GRASP at
   20 and 24 MiB. There the duel runs relaxed on 63% and 78% of decisions and
   recovers 32.8% and 38.7% of governed-first's gap to GRASP, all of it in
   writebacks.
4. **Above 8 MiB the record's bound is never compared.** At 16, 20 and 24 MiB on
   both kernels, every governed-first decision is DEAD-first or ungoverned-first.
   `base_kept`, `overridden` and `base_no_future` are all zero. There the rule
   reduces to two steps: evict a dead governed line if the set holds one, and
   otherwise the least-recently-used ungoverned line.

The counts in the fourth point are measured, and what follows from them is
inference. The crossover would then be set by the order among lines the record
does not describe, which is recency, and not by the record's information. A
signal that switches the rule off aims at the wrong lever. The next study tests
exactly that.

## The RRPV order: no choice reads recency

In record mode ECG already runs GRASP's insertion and hit update on every line,
so the cache holds an RRPV even for the lines the record is silent about.
`--record-rrpv-order on`, or `ECG_RECORD_RRPV_ORDER=1` for `pr`, takes every
choice the record does not decide from those bits:

1. **A DEAD governed way first**, unchanged. Ties go in GRASP's scan order: the
   highest RRPV, then the lowest way.
2. **Otherwise an ungoverned way, chosen by GRASP's scan** masked by the governed
   bit. GRASP's ageing applies to the ungoverned ways only.
3. **In a set holding only governed ways, GRASP's scan is the base on every
   kernel**, including PageRank, whose rule otherwise falls back to LRU. A
   strictly farther live bound still overrides it. Ties between equal bounds go
   to the higher RRPV, then the lower way.

In a set with no governed way, the victim and the set's RRPV state after the
decision equal GRASP's own. `testRrpvOrderIsGraspWithoutGovernedWays` pins
that, so at the level of a single decision ECG is at worst GRASP. The cache
throws if a decision under this order would reach an LRU scan, the base policy
or prefetch admission. On PageRank the arm registers its two property arrays
with GRASP_PAPER's tiers, a 0.50 hot fraction with the capacity boundary, which
are the tiers the GRASP_PAPER row uses.

Hardware:

- **State.** The 3-bit RRPV per line, the governed bit and GRASP's region
  registers. A GRASP cache already holds all three.
- **Removed.** The recency order.
- **Per eviction.** One max-RRPV search masked by the governed bit, plus GRASP's
  ageing, which is at most seven rounds. DEAD-first and the bound comparison
  are unchanged.
- **Interface.** No new port and no metadata traffic.

### Frozen study

The `ecg_rrpv_order_cache` study ran 32 rows, with one binary per kernel:
GRASP_PAPER, `POPT:UNCHARGED`, governed-first, and governed-first with the RRPV
order, at 8, 16, 20 and 24 MiB. The gate was frozen before the run and applied
once, on kernel transfers within one binary:

- `lift = (rule - arm) / rule`, where the rule is governed-first and the arm adds
  the RRPV order;
- **safe** where ECG already led means `lift ≥ -0.005` at 8 and 16 MiB;
- **improves** above the crossover means `lift ≥ +0.010` at 20 and 24 MiB;
- **confirmed** means both, per kernel;
- any `lift < -0.005` is a **regression**.

Integrity was checked first, and it was clean in all eight cells:

- four distinct labels;
- one binary and one result per kernel: `result_digest` `55e3fc26a1027ddb`, and
  `pr_score_checksum` `6249d06ef4cc2ed7`;
- identical access counts and no prefetch fills;
- every arm decision was RRPV-ordered and none reached an LRU base;
- no rule decision was RRPV-ordered.

| Kernel | LLC | GRASP_PAPER | `POPT:UNCHARGED` | Governed-first | RRPV order | Lift |
|---|---|---:|---:|---:|---:|---:|
| SpMV | 8 MiB | 15,456,930 | 12,398,416 | 12,289,362 | 12,289,813 | −0.004% |
| SpMV | 16 MiB | 6,702,419 | 6,249,737 | 6,151,409 | 6,123,151 | +0.46% |
| SpMV | 20 MiB | 5,887,025 | 6,252,319 | 6,126,909 | 6,052,874 | +1.21% |
| SpMV | 24 MiB | 5,754,248 | 6,249,737 | 6,106,480 | 5,977,208 | +2.12% |
| PageRank | 8 MiB | 15,975,743 | 13,880,986 | 13,011,137 | 12,872,632 | +1.06% |
| PageRank | 16 MiB | 6,900,691 | 8,262,963 | 6,151,543 | 6,125,111 | +0.43% |
| PageRank | 20 MiB | 6,068,167 | 7,178,487 | 6,132,857 | 6,055,044 | +1.27% |
| PageRank | 24 MiB | 5,946,039 | 6,085,585 | 6,118,746 | 5,996,115 | +2.00% |

**Confirmed on both kernels, with no regression.** The largest lift is +2.12%.
The [kernel census](#the-kernel-census-entry-state-and-deferred-writebacks)
later showed that above 8 MiB this lift is writebacks deferred past the
kernel's end. The verdict stands on its frozen metric. The lifts at 20 and
24 MiB that it rests on are boundary-sensitive on both kernels, between −0.01%
and +0.02% on the census's clean boundary.

SpMV's GRASP, P-OPT and governed-first rows reproduce the dueling study exactly.
On PageRank all three drift by at most 454 transfers, and the baselines move
alongside the rule. Rebuilding `pr` for the arm shifted its heap layout, which
moves every policy's rows, not only the arm's. The gate is applied within the
new binary. That is why the PageRank figures here differ slightly from those
earlier on this page.

### Against the named baselines

Margins are `(baseline - ECG) / baseline` on the same binary. A margin under 1%
is marked thin.

| Margin | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| SpMV, RRPV order vs GRASP_PAPER | **+20.49%** | **+8.64%** | −2.82% | −3.87% |
| SpMV, RRPV order vs `POPT:UNCHARGED` | +0.88% (thin) | **+2.03%** | **+3.19%** | **+4.36%** |
| PageRank, RRPV order vs GRASP_PAPER | **+19.42%** | **+11.24%** | +0.22% (thin) | −0.84% (thin) |
| PageRank, RRPV order vs `POPT:UNCHARGED` | **+7.26%** | **+25.87%** | **+15.65%** | **+1.47%** |
| SpMV, governed-first vs GRASP_PAPER | +20.49% | +8.22% | −4.07% | −6.12% |
| SpMV, governed-first vs `POPT:UNCHARGED` | +0.88% (thin) | +1.57% | +2.01% | +2.29% |
| PageRank, governed-first vs GRASP_PAPER | +18.56% | +10.86% | −1.07% | −2.90% |
| PageRank, governed-first vs `POPT:UNCHARGED` | +6.27% | +25.55% | +14.57% | −0.54% (thin) |

- **Against the favourable P-OPT control** the RRPV order leads in all eight
  cells, while governed-first alone trailed at PageRank 24 MiB. SpMV at 8 MiB
  remains a thin crossing.
- **Against GRASP** it leads in five of the eight cells, where governed-first led
  in four. PageRank at 20 MiB moves from −1.07% to +0.22%, which is thin and one
  cell on one graph.
- **GRASP still leads SpMV above 16 MiB**, by 2.82% and 3.87%, and leads
  PageRank thinly at 24 MiB, by 0.84%. The order narrows the crossover and does
  not close it. The
  [kernel census](#the-kernel-census-entry-state-and-deferred-writebacks) places
  SpMV's remaining deficit in the state the kernel starts in.

### Where the lift comes from

| Governed-first − RRPV order | LLC | Writebacks | Structural misses | Property misses |
|---|---|---:|---:|---:|
| SpMV | 8 MiB | −148 | −185 | −118 |
| SpMV | 16 MiB | +28,292 | −34 | 0 |
| SpMV | 20 MiB | +74,464 | −429 | 0 |
| SpMV | 24 MiB | +128,840 | 0 | +432 |
| PageRank | 8 MiB | −4,608 | 0 | +143,113 |
| PageRank | 16 MiB | +26,098 | 0 | +334 |
| PageRank | 20 MiB | +77,324 | 0 | +489 |
| PageRank | 24 MiB | +122,374 | 0 | +257 |

- **Above 8 MiB the lift is writebacks, on both kernels.** No other component
  moves by as many as 500 transfers. Neither arm reaches the base victim at
  these capacities, so the two differ only in the order among ungoverned lines
  and among DEAD lines. Writebacks are charged when a dirty line is evicted, and
  a dirty line still resident when the kernel ends is never charged, under any
  policy. Part of a writeback saving can therefore be deferral rather than
  avoidance. These receipts do not separate the two. The
  [kernel census](#the-kernel-census-entry-state-and-deferred-writebacks)
  does: above 8 MiB the saving is deferral.
- **PageRank at 8 MiB is the base switch.** Its lift is 143,113 fewer property
  misses, net of 4,608 more writebacks. With GRASP's scan as the base instead of
  LRU, the base's own victim stands, because no other line in the set has a
  strictly farther bound, on 1,381,382 decisions instead of 138,279: 11.6% of
  decisions instead of 1.1%. Overridden decisions fall from 50.2% to 39.8%. The
  preregistration named this cell as the safety risk. It came in on the
  favourable side, 0.06 points outside its predicted ±1% band.
- **SpMV at 8 MiB is a wash**, 451 transfers. Its rule already had a GRASP base,
  so only ties and the ungoverned order changed.
- The ungoverned-first share of decisions moves by at most 0.5 points, inside
  the 2 points the preregistration predicted.

### What is left against GRASP

This split of the RRPV order's gap to GRASP is arithmetic on the rows, not a
cause:

| RRPV order − GRASP_PAPER | Gap | Writebacks | Structural misses | Property misses |
|---|---:|---:|---:|---:|
| SpMV 20 MiB | +165,849 | +40,641 | +57,917 | +67,291 |
| SpMV 24 MiB | +222,960 | +57,214 | +127,519 | +38,227 |
| PageRank 20 MiB | −13,123 | +83,767 | 0 | −96,890 |
| PageRank 24 MiB | +50,076 | +87,739 | 0 | −37,663 |

On PageRank the record already beats GRASP on property misses at both
capacities, and what remains is writebacks. On SpMV at 24 MiB more than half of
the gap is structural. As [the traffic floor](#ecg-has-a-traffic-floor-grasp-does-not)
shows, GRASP converts added capacity into structural hits: its structural misses
fall from 5,073,432 at 8 MiB to 4,945,273 at 24 MiB. The RRPV order's are
5,072,792 at 24 MiB, identical to governed-first's, so it converts none.

The preregistration predicted that the masked scan, like GRASP's, would leave
some older structural lines resident and so recover part of that reuse. **It did
not**, and that prediction failed. So did the SpMV lift predictions at 20 and
24 MiB: about +2.0% and +2.8% were predicted, and +1.21% and +2.12% were
measured.

This study left open why SpMV's residual persisted, and named one candidate:
the cache state the kernel inherits from construction. The boundary between
construction and the kernel keeps the cache's contents, and at 20 MiB a record
policy's setup moves 18.04 million transfers through the cache against GRASP's
2.77 million. These receipts carry no per-pass split. The
[kernel census](#the-kernel-census-entry-state-and-deferred-writebacks) added
one and confirmed the candidate. The residual is the first pass's misses.
GRASP_PAPER enters the kernel with all of x resident, and the RRPV order with
69% of it at 20 MiB and 83% at 24 MiB.

### Setup, reported separately

SpMV's setup transfers are identical between governed-first and the RRPV order
at every capacity: 19,195,101, 18,340,263, 18,042,849 and 17,805,112. The order
adds no preprocessing, and the
[construction accounting](#construction-as-a-one-time-cost) is unchanged.
PageRank's rows carry no setup phase for any policy.

### What this result does not claim

- **Nothing about BFS, SSSP or BC.** SSSP is a
  [documented case](Traversal-Metadata-Taxonomy) in which an LRU base beat a
  GRASP base, so the order must not be carried over to the filtered kernels
  without evidence of its own.
- **Nothing about another graph, or about timing.** One graph was measured, in
  the functional cache model.
- **Nothing native.** Only cache_sim implements the order, and gem5 and Sniper
  refuse it rather than run the recency order under its label.
- **No default change.** Governed-first and the RRPV order are both off by
  default.
- **Beating governed-first is not a competitive claim.** The margins against
  GRASP and P-OPT come from one graph, and the thin ones are not generalised:
  SpMV against P-OPT at 8 MiB, and PageRank against GRASP at 20 and 24 MiB.

## The kernel census: entry state and deferred writebacks

The RRPV-order study left two questions that its receipts could not answer.
First, its lift above 8 MiB was writebacks, and a writeback saving can be
deferral rather than avoidance. Second, GRASP's SpMV lead above 16 MiB could
come from the cache state the kernel inherits from setup, rather than from the
kernel's own decisions. The `ecg_kernel_census_cache` study reran the same 32
rows with a passive census that answers both, on this graph.

**The census.** `cache_sim` arms it at the kernel boundary, and the receipt
reports:

- **the entry state:** the resident, dirty and property lines when the kernel
  begins;
- **the passes:** the kernel's transfers, misses and writebacks, split at its
  graph passes. SpMV ran two `--repeat` passes and PageRank two iterations.
  Every writeback of a line that setup left dirty is charged to the pass it
  fell in;
- **the exit residue:** the dirty lines still resident when the kernel ends,
  and how many of those setup dirtied.

The census adds receipt fields only. The modeled cache gains no bits, ports,
metadata traffic or per-eviction work. [The tests](#reproducing) require the
receipt of each of the study's four policies, less its census, to be
byte-identical to a receipt without one.

**Two boundaries.** The frozen metric is the kernel transfers that every study
on this page uses. It charges the kernel for writing back lines that setup left
dirty, but never for the dirty lines the kernel leaves resident at exit. The
census adds a clean boundary that does neither:

```text
clean = kernel transfers
      - writebacks, during the kernel, of lines that setup left dirty
      + dirty lines at exit that the kernel wrote
```

The clean figure needs no second run, because no roster decision reads the
dirty bit. A test runs each roster policy from a clean entry and finds the same
figure. A line's setup mark retires only when the line leaves the last level. So
if the kernel rewrote a setup-dirty line only in L1 or L2, the clean figure
would still charge its writeback to setup, as a clean-entry run of this model
would too.

Every clean figure is reported beside its frozen one, never in its place. A
figure is marked boundary-sensitive when its sign differs between the two, or
when its two values differ by a point or more.

### The census study

The study ran GRASP_PAPER, `POPT:UNCHARGED`, governed-first and the RRPV order
at 8, 16, 20 and 24 MiB, with one binary per kernel: the RRPV-order study's 32
rows. One question was gated. The gate and twelve predictions were frozen
before the run, and everything else was reported. The budget was 32 rows; all
32 ran once, and none was rerun.

Integrity was clean on all 32 rows:

- four distinct labels, one binary and one result per kernel and capacity;
- no prefetch fills, and identical access counts;
- every RRPV-order decision was RRPV-ordered, and none reached an LRU base;
- every census validates, with one entry and two passes of equal accesses, and
  no access fell outside the passes.

SpMV reproduces the RRPV-order study exactly on all 16 rows. PageRank drifts
on all 16, baselines included, by −916 to +529 transfers, as it drifted by up
to 454 at the RRPV-order study's rebuild. The census enlarged the simulator's
cache objects, and the cache model maps real addresses, so a rebuild can move
any row. PageRank is therefore compared within this binary. The prediction that
it would drift by at most 500 transfers missed.

### GRASP's SpMV lead is the state the kernel starts in

SpMV's setup ends by initialising x, the property array the kernel reads.
GRASP_PAPER goes from there straight into the kernel. ECG builds its records,
and P-OPT its rank matrix, between that initialisation and the kernel. The
policies therefore start the kernel with different amounts of x resident, and
every resident line of x is dirty:

| LLC | Lines | GRASP_PAPER | Governed-first and RRPV order | `POPT:UNCHARGED` |
|---|---:|---:|---:|---:|
| 8 MiB | 131,072 | 131,072 | 5,326 | 0 |
| 16 MiB | 262,144 | 235,923 | 126,228 | 0 |
| 20 MiB | 327,680 | 235,923 | 163,876 | 0 |
| 24 MiB | 393,216 | 235,923 | 196,609 | 0 |

x is 235,923 lines. From 16 MiB up GRASP_PAPER enters with all of it. At
8 MiB it enters with a cache that holds nothing else.

The gated question asked whether the RRPV order's deficit to GRASP_PAPER at 20
and 24 MiB reaches SpMV's second pass. It compares two deficits:

- `m_kernel`, the deficit over the whole kernel;
- `m_steady`, the deficit on the second pass once the setup writebacks that
  fell there are removed.

The deficit does not reach the second pass, class E, when
`m_steady ≥ −0.5%`. Below that, three classes compare `m_steady` with
`m_kernel`, using a one-point band. They separate a deficit partly confined to
the first pass, one that recurs at about its whole-kernel size, and one that the
second pass shows more of.

| LLC | m_kernel | m_steady | Class |
|---|---:|---:|---|
| 20 MiB | −2.82% | +1.69% | E |
| 24 MiB | −3.87% | +1.69% | E |

**The verdict is `entry_explained`,** class E at both capacities. The deficit
is a first-pass transient: the entry state, or the warm-up from it.

The +1.69% is not a lead. In the second pass the RRPV order writes back 49,472
and 50,443 fewer of the lines that the kernel dirtied, and leaves them dirty at
exit instead, which `m_steady` does not charge. Its second-pass misses are only
1,273 and 432 fewer, out of about 2.77 million. On misses the second pass is a
tie, so the class does not rest on the deferral.

On the clean boundary the whole gap is the first pass's misses:

| RRPV order − GRASP_PAPER | 20 MiB | 24 MiB |
|---|---:|---:|
| Clean gap | 125,208 | 165,314 |
| First-pass misses | +126,481 | +166,178 |
| of which property | +68,564 | +38,659 |
| of which structural | +57,917 | +127,519 |
| Second-pass misses | −1,273 | −432 |
| Lines the kernel dirtied | 0 | −432 |

The first-pass misses, the second-pass misses and the lines the kernel dirtied
sum to the gap exactly. The first pass's property part is close to the number
of x's lines that the RRPV order lacks at entry, 72,047 and 39,314.

GRASP_PAPER's first pass misses 66,319 and 128,159 fewer structural lines than
the 2,536,716 that every SpMV second pass misses, under every policy and at
every capacity. So structural lines that its setup left resident serve the
kernel too. That is inferred from the misses; the census does not say which
arrays those lines belong to. The RRPV order gets 8,402 lines of the same help
at 20 MiB and 640 at 24 MiB.

Two passes cannot separate a steady state from a slow warm-up. GRASP_PAPER
still holds 232,264 and 235,267 of x's setup-dirty lines at exit, so its entry
state lasts through both passes. What a kernel invoked repeatedly after one
construction sees is not measured. Eight passes after one construction were
measured later; see
[the steady state](#the-steady-state-eight-passes-after-one-construction).

### Above 8 MiB the RRPV order's lift is deferral

The frozen lift over governed-first splits exactly into three parts:

- the clean lift;
- the writebacks of setup's dirty lines that the RRPV order saves;
- the extra dirty lines that the kernel wrote and the RRPV order leaves
  resident at exit.

| Kernel | LLC | Frozen lift | Clean lift | Setup writebacks saved | Extra dirty lines at exit |
|---|---|---:|---:|---:|---:|
| SpMV | 16 MiB | 28,258 (+0.46%) | −34 | −10,156 | 38,448 |
| SpMV | 20 MiB | 74,035 (+1.21%) | −429 | −8,495 | 82,959 |
| SpMV | 24 MiB | 129,272 (+2.12%) | +864 | −4,682 | 133,090 |
| PageRank | 8 MiB | 138,483 (+1.06%) | +143,694 (+1.10%) | 0 | −5,211 |
| PageRank | 16 MiB | 26,422 (+0.43%) | +696 | 0 | 25,726 |
| PageRank | 20 MiB | 77,915 (+1.27%) | +970 | 0 | 76,945 |
| PageRank | 24 MiB | 122,525 (+2.00%) | +490 | 0 | 122,035 |

SpMV at 8 MiB is −451 frozen and −303 clean.

- **Above 8 MiB, on both kernels, the RRPV order moves writebacks past the
  kernel's end rather than removing them.** Its clean lift is between −0.01%
  and +0.02%. It also writes back more of setup's dirty lines than
  governed-first, not fewer, the opposite of what was predicted.
- **PageRank at 8 MiB keeps a real reduction,** +1.10% on the clean boundary.
  It comes from the property misses that the GRASP base recovers; see
  [where the lift comes from](#where-the-lift-comes-from).
- **The RRPV-order verdict stands on its frozen metric.** Its SpMV
  confirmation at 20 and 24 MiB is boundary-sensitive, −0.01% and +0.01% clean,
  and is not reversed. PageRank's lifts there are boundary-sensitive too,
  +0.02% and +0.01% clean.
- **What the RRPV order still offers** is that no ECG decision reads recency,
  at no clean cost. On the clean boundary it is never worse than governed-first
  by more than 0.01%. It makes no lift claim above 8 MiB.

### Against the named baselines, on both boundaries

The RRPV order's margins are `(baseline - ECG) / baseline`, frozen / clean. A
margin under 1% is marked thin. PageRank's figures come from the census binary,
so they can differ from the RRPV-order study's by a few hundredths of a point.

| RRPV order vs | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---|---|---|---|
| SpMV, GRASP_PAPER | +20.49 / +20.20 | +8.64 / +7.98 | −2.82 / −2.10 | −3.87 / −2.81 (sensitive) |
| SpMV, `POPT:UNCHARGED` | +0.88 / +0.94 (thin) | +2.03 / +2.03 | +3.19 / +2.76 | +4.36 / +3.17 (sensitive) |
| PageRank, GRASP_PAPER | +19.42 / +19.73 | +11.24 / +11.60 | +0.22 / +1.09 (thin) | −0.84 / +0.15 (sensitive, thin) |
| PageRank, `POPT:UNCHARGED` | +7.26 / +7.18 | +25.88 / +25.45 | +15.64 / +15.42 | +1.47 / +2.20 |

- **On the clean boundary ECG leads P-OPT at all eight points.** Only SpMV at
  8 MiB is thin.
- **Against GRASP_PAPER, SpMV keeps its deficit at 20 and 24 MiB** on the clean
  boundary, and the entry state explains it. PageRank at 24 MiB moves from
  −0.84% to +0.15%, which is thin.
- **The frozen boundary flatters whichever policy leaves more dirty lines
  resident at exit.** On PageRank at 20 and 24 MiB:
  - GRASP_PAPER leaves 306,908 and 368,399;
  - the RRPV order leaves 250,910 and 308,468;
  - governed-first leaves 173,965 and 186,433.
- **Governed-first's clean margins are within 0.02 points of the RRPV
  order's,** except on PageRank at 8 MiB: +18.84% against GRASP_PAPER and
  +6.15% against P-OPT. Two of its frozen deficits turn positive on the clean
  boundary: −1.07% against GRASP_PAPER at PageRank 20 MiB becomes +1.07%, and
  −0.54% against P-OPT at PageRank 24 MiB becomes +2.20%.

### PageRank: better in the first iteration, worse in the second

This is reported, not gated. Every PageRank row enters with no dirty line, so
its two boundaries differ only by the exit residue.

At 24 MiB the RRPV order's first iteration misses 65,593 fewer property lines
than GRASP_PAPER's. Its second iteration has 27,953 more property misses and
84,799 more writebacks, and moves 3.75% more transfers. At 20 MiB the second
iteration moves 3.81% more.

The prediction that each ECG row's second-iteration margin against GRASP_PAPER
would stay within a point of its whole-kernel margin held at 8 MiB and failed
at 16–24 MiB. There `d`, the difference between the two margins, ran from
−1.5% to −4.0%. Two iterations cannot say which one a longer run resembles.
A later run of eight iterations showed that the deficit belongs to the last
iteration, not to the second; see
[the last pass](#the-last-pass-wrapped-bounds-become-dead).

### Setup, reported separately

The kernel is the region of interest, and construction is a one-time cost
reported beside it. On SpMV every policy pays GRASP_PAPER's whole setup,
loading the CSR and initialising x. That common floor is 2,877,491 transfers at
8 MiB and 2,772,640 at 16–24 MiB. PageRank's rows carry no setup phase for any
policy. Each mechanism's own SpMV preprocessing costs this much above the
floor:

| LLC | `POPT:UNCHARGED` | Governed-first and RRPV order |
|---|---:|---:|
| 8 MiB | +6,416,016 | +16,317,610 |
| 16 MiB | +6,509,060 | +15,567,623 |
| 20 MiB | +6,498,712 | +15,270,209 |
| 24 MiB | +6,501,242 | +15,032,472 |

The number of kernel invocations after which the frozen kernel gain repays
that excess, governed-first / RRPV order:

| Against | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| GRASP_PAPER | 6 / 6 | 29 / 27 | never | never |
| `POPT:UNCHARGED` | 91 / 92 | 93 / 72 | 70 / 44 | 60 / 32 |

From 16 MiB up the RRPV order breaks even sooner only because of its deferred
writebacks. On the clean boundary the two have the same kernel figure, within
0.02%, and the same setup. Each break-even assumes that every invocation
repeats this run's frozen gain. This census shows that the gain depends on the
state the kernel starts in.

### Predictions

Twelve predictions were recorded before the run, and six held.

- **Held:**
  - governed-first and the RRPV order enter SpMV with setup-dirty lines, and
    P-OPT enters with none;
  - the RRPV order's clean SpMV lift at 20 and 24 MiB is under one point;
  - every PageRank row enters clean;
  - on PageRank the RRPV order's clean lift over governed-first stays positive
    at 16–24 MiB, although only by +0.01% to +0.02%;
  - SpMV reproduces the RRPV-order study exactly;
  - no access falls outside the passes.
- **Missed:**
  - GRASP_PAPER enters with x as predicted, but ECG's record build does not
    flush x. ECG enters with 5,326 to 196,609 of x's lines, 4–50% of the LLC,
    not the 1% at most that was predicted.
  - The RRPV order does not avoid setup writebacks. It writes back 4,682 to
    10,156 more than governed-first, and its lift is deferred kernel
    writebacks.
  - The verdict is `entry_explained`, not the predicted mix, in which part of
    the 24 MiB deficit would have recurred in the second pass.
  - Above 8 MiB most setup writebacks do not fall in the first pass. Only
    17–31% do, and the rest fall in the second.
  - On PageRank the second-iteration margins against GRASP_PAPER do not stay
    within a point of the whole-kernel margins at 16–24 MiB, as described
    above.
  - PageRank drifts by up to 916 transfers, not 500.

### What the census settles and what it does not claim

- **Settled** on this graph and these capacities, in the functional cache
  model, with two passes:
  - GRASP_PAPER's SpMV lead above 16 MiB is confined to the first pass, where
    the state the kernel starts in accounts for it. On misses the second pass
    is a tie;
  - the RRPV order's lift over governed-first above 8 MiB is writebacks
    deferred past the kernel's end.
- **Not settled** by the census, and answered for eight passes of one run by
  [the steady state](#the-steady-state-eight-passes-after-one-construction):
  - what a kernel invoked repeatedly after one construction sees; neither the
    frozen first pass nor this second pass is that;
  - whether PageRank's second-iteration deficit persists.
- **Not claimed:**
  - no new mechanism, and no default change;
  - nothing about BFS, SSSP or BC, another graph, timing, gem5 or Sniper;
  - neither boundary is a competitive claim, and a clean figure never replaces
    a frozen margin in a verdict.

## The steady state: eight passes after one construction

The census study left two questions that its two passes could not answer. Two
passes cannot separate a steady state from a slow warm-up. And on both kernels
the second pass was also the last, which matters for ECG alone: when no further
pass is requested, a wrapped bound decodes to DEAD rather than FINITE, as
[decoding a conservative future bound](ReusePlan-FlowThrough#3-decode-a-conservative-future-bound)
describes. SpMV requests a further pass on every `--repeat` pass but the last,
and PageRank on every iteration but the last. No baseline reads the request.

The `ecg_steady_state_cache` study reran the census study's 32 rows with eight
passes, SpMV with `--repeat 8` and PageRank with `-o 0 -n 1 -i 8 -t 0`, and
changed nothing else. One question per kernel was gated. The gate and six
predictions were frozen before the run, and everything else was reported. The
budget was 32 rows in 24 jobs; all ran once, and none was rerun.

**Per-pass detail.** The census now details each pass, up to 64: the pass's
counters, and the last level's valid, dirty, property and setup-dirty lines at
its end. A detailed pass is what the census would have reported had the kernel
ended there. It adds receipt fields only; the modeled cache gains no bits,
ports, metadata traffic or per-eviction work. Two figures are read per pass:

- its transfers, `T`: its misses and its writebacks. The eight passes sum to
  the frozen kernel transfers;
- its flow, `F`: its misses and the lines it turned from clean to dirty. A
  write is charged to the pass that makes it, not to the pass whose eviction
  writes the line back. Here the eight passes sum to the clean figure, because
  no kernel write at the last level hit a line that setup had left dirty.

**The window.** Pass 1 starts from the state that setup leaves, and passes 2
and 3 settle. Passes 4–7 are the window that the gate reads, and each of them
has a next pass. Pass 8 is the only pass without one, and is reported on its
own. A row is stationary when each of three quantities is at most 0.1% of the
corresponding window mean:

1. the difference between the mean of passes 4 and 5 and the mean of passes 6
   and 7, on transfers and on flow;
2. the difference between the row's mean transfers and its mean flow, which
   measures the net change in its resident dirty lines across the window;
3. the writebacks of setup's dirty lines per window pass.

**The gated question.** For each kernel, the RRPV order against GRASP_PAPER at
20 and 24 MiB. The margin is `(baseline - ECG) / baseline` on the two rows'
mean transfers per window pass. It is a lead at +0.5% or more, a tie strictly
between −0.5% and +0.5%, and a deficit at −0.5% or less; a pair with a row
that is not stationary is not classed. The verdict is `ecg_leads`, `tie` or
`grasp_leads` when both capacities agree, `mixed` when they differ, and
`unresolved` when either is not classed.

Integrity was clean on all 32 rows:

- four distinct labels, one binary and one result per kernel and capacity;
- no prefetch fills, and identical access counts;
- every RRPV-order decision was RRPV-ordered, and none reached an LRU base; no
  governed-first row made one;
- every census validates, with one entry and eight detailed passes, and no
  access fell outside the passes.

This rebuild moved no row. SpMV's setup, entry state and first pass reproduce
the census study's exactly on all 16 rows, and so do PageRank's entry state and
first pass. Each baseline's second pass reproduces the census study's second
pass exactly, on both kernels at every capacity. Earlier rebuilds had moved
PageRank rows by up to 454 and 916 transfers.

### Kernel transfers first

Over the whole kernel, the RRPV order's margins, frozen / clean:

| RRPV order vs | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---|---|---|---|
| SpMV, GRASP_PAPER | +20.77 / +20.70 | +9.42 / +9.25 | −0.64 / −0.48 (thin) | −0.94 / −0.69 (thin) |
| SpMV, `POPT:UNCHARGED` | +0.96 / +0.98 (thin) | +0.52 / +0.52 (thin) | +0.82 / +0.71 (thin) | +1.12 / +0.82 (thin) |
| PageRank, GRASP_PAPER | +19.13 / +19.20 | +11.17 / +11.27 | +0.11 / +0.34 (thin) | −0.22 / +0.04 (sensitive, thin) |
| PageRank, `POPT:UNCHARGED` | +6.83 / +6.81 | +27.12 / +27.01 | +15.80 / +15.75 | +0.77 / +0.98 (thin) |

- **Governed-first's clean margins are within 0.01 points of these,** except on
  PageRank at 8 MiB: +18.69% against GRASP_PAPER and +6.22% against P-OPT.
  Its frozen margins against GRASP_PAPER are lower: −0.95% and −1.48% on SpMV
  at 20 and 24 MiB, and −0.21% and −0.72% on PageRank, which are
  boundary-sensitive, +0.33% and +0.04% clean.
- **Above 8 MiB the RRPV order's lift over governed-first is still deferral.**
  Frozen, it is +0.11% to +0.54% at those six points; clean, it rounds to 0.00%
  at each. On PageRank at 8 MiB it is +0.62% frozen and +0.63% clean, and on
  SpMV at 8 MiB it rounds to 0.00% on both boundaries.
- **A whole-kernel margin depends on the pass count** when the gap sits at the
  kernel's ends. The RRPV order's deficit to GRASP_PAPER on SpMV at 20 and
  24 MiB is −2.82% and −3.87% over the census study's two passes, and −0.64%
  and −0.94% over eight. On PageRank at 24 MiB it is −0.84% over two and
  −0.22% over eight. Neither is a per-pass figure; the window is.

### In the window, ECG is never behind

**Both gated questions are ties:**

| RRPV order vs GRASP_PAPER | GRASP_PAPER per pass | RRPV order per pass | Margin, transfers / flow | Class |
|---|---:|---:|---:|---|
| SpMV, 20 MiB | 3,010,189.00 | 3,008,562.00 | +0.054% / +0.051% | tie |
| SpMV, 24 MiB | 3,008,562.50 | 3,008,561.50 | +0.000% / +0.000% | tie |
| PageRank, 20 MiB | 3,010,729.25 | 3,008,562.00 | +0.072% / +0.072% | tie |
| PageRank, 24 MiB | 3,008,562.00 | 3,008,562.00 | +0.000% / +0.000% | tie |

**The verdict is `tie` on both kernels.** Once the kernel has settled, the RRPV
order's mean transfers per pass are no higher than GRASP_PAPER's at 20 and
24 MiB, and at most 0.072% lower. So GRASP_PAPER's SpMV lead does not persist into the
settled passes, and PageRank's second-iteration deficit does not recur in a
pass that has a next pass.

Every pair, gated or reported, with its class:

| RRPV order, per pass | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---|---|---|---|
| SpMV, GRASP_PAPER | +20.86 lead | +9.67 lead | +0.05 tie | 0.00 tie |
| SpMV, `POPT:UNCHARGED` | +0.99 lead (thin) | 0.00 tie | 0.00 tie | 0.00 tie |
| PageRank, GRASP_PAPER | +19.03 lead | +11.15 lead | +0.07 tie | 0.00 tie |
| PageRank, `POPT:UNCHARGED` | +6.68 lead | +27.54 lead | +15.86 lead | +0.54 lead (thin) |

- Governed-first's classes are the RRPV order's at every point, and its margins
  differ only on PageRank at 8 MiB: +18.64% and +6.24%. There the RRPV order's
  per-pass lift over it is +0.47%; everywhere else it is 0.00%.
- Every class is the same on flow.
- No ECG row's window mean, on transfers or on flow, exceeds either baseline's
  at any capacity on either kernel. Pass by pass, from the second pass to the
  seventh, an ECG row moves more transfers than a baseline in only two passes,
  by 1 and by 4: governed-first against GRASP_PAPER at 24 MiB, in SpMV's fifth
  pass and PageRank's second.

**The per-pass floor.** From 16 MiB up, every ECG row's window mean is 3,008,562
transfers per pass on both kernels, to within half a transfer. That figure is:

- 2,536,716 structural misses, which every row on both kernels has in every
  pass from the second on;
- 235,923 property misses and 235,923 writebacks.

235,923 lines is one 4-byte array over the graph's 3,774,768 vertices. The
count fits a pass that misses and writes back each line of the output array
once, SpMV's y and PageRank's scores, and keeps the gathered array resident: x,
or PageRank's contributions. The census counts lines, not arrays, so that is
inferred. The floor is the level that a settled pass reaches here, not a proven
minimum: `POPT:UNCHARGED`'s PageRank row at 24 MiB writes back about 46,400
fewer lines per pass than the floor, though it misses about 62,700 more
property lines.

How far each row's window mean sits above the floor:

| Above the floor, per pass | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| SpMV, GRASP_PAPER | +155.75% | +10.70% | +0.054% | 0.00% |
| SpMV, `POPT:UNCHARGED` | +104.42% | 0.00% | 0.00% | 0.00% |
| SpMV, governed-first and RRPV order | +102.40% | 0.00% | 0.00% | 0.00% |
| PageRank, GRASP_PAPER | +164.45% | +12.54% | +0.072% | 0.00% |
| PageRank, `POPT:UNCHARGED` | +129.47% | +38.00% | +18.85% | +0.54% |
| PageRank, governed-first | +115.15% | 0.00% | 0.00% | 0.00% |
| PageRank, RRPV order | +114.14% | 0.00% | 0.00% | 0.00% |

On SpMV at 24 MiB GRASP_PAPER is half a transfer per pass above the floor and
the RRPV order half a transfer below it. Every other 0.00% is exact.

- **ECG settles on the floor from 16 MiB up on both kernels.** GRASP_PAPER
  reaches it only at 24 MiB, and `POPT:UNCHARGED` from 16 MiB on SpMV and at no
  capacity on PageRank.
- **At 8 MiB the gathered array cannot stay resident:** its 235,923 lines
  exceed the cache's 131,072. ECG is nearest the floor there on both kernels.

**Settled is not drained.** All 32 rows are stationary. The nearest to a bound
is GRASP_PAPER on SpMV at 20 MiB, whose window halves differ by 1,761 transfers
against a bound of 3,010. On SpMV, stationary does not mean that setup's dirty
lines have left. From the end of pass 1 to the end of pass 7, governed-first and
the RRPV order hold the same number of them: 2, 126,228, 163,876 and 196,609 at
8–24 MiB, from 16 MiB up as many as the lines of x they entered with.
GRASP_PAPER still holds 65,667, 159,149, 230,651 and 235,267 at the end of
pass 7, and P-OPT none. The window measures a state in which those lines stay:
at most 254 of them leave during any row's four window passes.

### Where GRASP_PAPER's remaining lead falls

The whole-kernel gap between GRASP_PAPER and the RRPV order, split by pass, as
transfers / flow. Positive means the RRPV order moves fewer:

| GRASP_PAPER − RRPV order | Pass 1 | Passes 2–7 | Pass 8 | Kernel |
|---|---:|---:|---:|---:|
| SpMV 8 MiB | +1,550,176 / +1,502,783 | +9,633,943 / +9,631,863 | +1,614,403 / +1,605,659 | +12,798,522 / +12,740,305 |
| SpMV 16 MiB | +243,801 / +205,242 | +1,939,979 / +1,935,194 | +328,854 / +323,097 | +2,512,634 / +2,463,533 |
| SpMV 20 MiB | −145,418 / −126,481 | +9,847 / +8,860 | −18,136 / +2,521 | −153,707 / −115,100 |
| SpMV 24 MiB | −183,788 / −166,178 | +8 / +7 | −38,858 / +958 | −222,638 / −165,213 |
| PageRank 8 MiB | +1,572,262 / +1,653,489 | +9,084,753 / +9,085,044 | +1,529,489 / +1,515,725 | +12,186,504 / +12,254,258 |
| PageRank 16 MiB | +514,299 / +507,907 | +2,269,482 / +2,269,958 | +257,195 / +315,297 | +3,040,976 / +3,093,162 |
| PageRank 20 MiB | +127,828 / +124,343 | +12,148 / +12,203 | −114,246 / −54,151 | +25,730 / +82,395 |
| PageRank 24 MiB | +62,511 / +65,596 | 0 / 0 | −115,419 / −55,890 | −52,908 / +9,706 |

- **At 8 and 16 MiB the RRPV order leads in every pass group,** and the six
  interior passes carry about three quarters of the lead.
- **On SpMV at 20 and 24 MiB, GRASP_PAPER's lead is the first pass.** Its flow
  part, 126,481 and 166,178, is exactly the census study's extra first-pass
  misses. After it the RRPV order leads on flow in every pass group. On
  transfers the last pass adds 18,136 and 38,858 to GRASP_PAPER's lead, but on
  flow the last pass favours the RRPV order: that is writeback timing, which
  [the last pass](#the-last-pass-wrapped-bounds-become-dead) explains.
- **On PageRank at 20 and 24 MiB, GRASP_PAPER leads only in the last pass.**
  The RRPV order leads the first pass by 127,828 and 62,511 transfers and ties
  or leads the interior, then loses the last pass by 114,246 and 115,419
  transfers, or 54,151 and 55,890 on flow. At 24 MiB the last pass outweighs
  the first, which is why the whole-kernel frozen margin is −0.22%.
- **Governed-first splits the same way.** Except on PageRank at 8 MiB, its flow
  gaps are within 1,000 of the RRPV order's in every pass group. Above 8 MiB
  its transfer gaps at both ends are worse, by the writebacks that the RRPV
  order defers.

### The last pass: wrapped bounds become DEAD

In the census study the second pass was the last. Here it has a next pass.
Every row reproduces the census study's first pass exactly, and each baseline
its second pass too. What differs for ECG is that its second pass now requests
a further one. Against the census study's second pass,
ECG's has:

- on PageRank at 16–24 MiB, 27,953 to 30,777 fewer property misses and 84,799
  to 105,608 fewer writebacks. That brings it to the floor, or 4 transfers
  below it for the RRPV order at 24 MiB;
- on SpMV at 16–24 MiB, the same misses but for 432 more by the RRPV order at
  24 MiB. It writes back 37,660 to 90,048 fewer of setup's dirty lines and
  25,063 to 53,928 more of the others. Its transfers change by −60,303 to
  +6,112, and bring it to the floor too;
- at 8 MiB, more: 9,495 and 9,346 more transfers on SpMV and 15,930 and 15,957
  on PageRank, for governed-first and the RRPV order. Most are writebacks, and
  584 to 2,234 are property misses.

And this run's last pass moves what the census study's last pass moved.
Governed-first's transfers are equal at 7 of 8 points and 2 apart at the
eighth, and the RRPV order's are within 2,671, 0.09%. So ECG's last pass costs
the same, to within 0.09%, whether it follows one pass or seven. The baselines
do not depend on the request. Their second pass is identical in both studies,
and their last pass differs from their seventh by at most 2,963 transfers,
0.10%. On 14 of their 16 rows that is within the largest step between
consecutive passes 2–7; GRASP_PAPER's SpMV rows at 20 and 24 MiB exceed it by
258 and 1.

**On PageRank the last pass adds misses.** Pass 8 against pass 7 on ECG's rows:

| PageRank, pass 8 − pass 7 | Transfers | Misses | Writebacks | Resident dirty lines | Flow |
|---|---:|---:|---:|---:|---:|
| 16 MiB, governed-first | +135,369 | +30,777 | +104,592 | −73,815 | +61,554 |
| 16 MiB, RRPV order | +118,757 | +30,429 | +88,328 | −57,899 | +60,858 |
| 20 MiB, governed-first | +134,555 | +28,947 | +105,608 | −76,661 | +57,894 |
| 20 MiB, RRPV order | +117,078 | +28,462 | +88,616 | −60,154 | +56,924 |
| 24 MiB, governed-first | +132,838 | +28,196 | +104,642 | −76,446 | +56,392 |
| 24 MiB, RRPV order | +115,419 | +27,945 | +87,474 | −59,529 | +55,890 |

The resident dirty lines did not change during pass 7 on these rows, so that
column is pass 8's own change. Every added miss is a property miss, and on
every row the flow rises by exactly twice the added misses, so the lines turned
dirty rise by as many as the misses. The added writebacks are those added dirty
lines plus the drop in resident dirty lines. The drop is writeback timing, and
it moves no flow.

Two readings fit, and the census cannot separate them. Neither is measured:

- **A write re-fetches a line the record has retired.** PageRank's kernel
  gathers contributions along in-edges throughout the pass and updates each
  vertex's own contribution in place, in vertex order. In the last pass a
  contribution line decodes DEAD after its last gather. If its vertices' writes
  are still to come, the line can be evicted and written back, and then missed
  and dirtied again by the write: one writeback, one miss and one line turned
  dirty. The bound stays conservative for the gathers it describes.
- **The bound is not conservative.** A line decoded DEAD is gathered again
  later in the same pass. That fits the doubled flow only if every such line's
  write was still to come.

A later count separated them: on these rows the added misses are writes
re-fetching retired lines, and no gather fetches one back; see
[the re-fetch count](#the-re-fetch-count-pageranks-added-last-pass-misses-are-in-place-writes).

**On SpMV the last pass changes writebacks and adds no flow.** The kernel only
reads x.
From 16 MiB up, the last pass writes back 37,660 to 85,366 of the setup-dirty
lines that governed-first had held since the first pass, and 47,816 to 90,049
of the RRPV order's. Against the seventh pass, its transfers change by −6,112
to +60,303. Its misses do not change, but for 479 fewer by the RRPV order at
24 MiB, and its flow, which charges those writebacks to setup, does not rise.

At 8 MiB the last pass is cheaper than the seventh on both kernels: by 9,492
and 9,336 transfers on SpMV and by 15,930 and 15,732 on PageRank, for
governed-first and the RRPV order.

### Setup, reported separately, and the break-even

SpMV's setup is the census study's, exactly: a common floor of 2,877,491
transfers at 8 MiB and 2,772,640 at 16–24 MiB, and the same excess above it for
each mechanism. PageRank's rows carry no setup phase for any policy.

SpMV's break-even counts passes of this run after one construction: the least
number whose frozen kernel gains, pass by pass, repay the setup excess. Beyond
seven passes it assumes that every further pass repeats the window's gain, and
it is `never` when that gain is not positive and no pass up to the seventh
repays. Pass 8 is excluded. Governed-first / RRPV order:

| Against | 8 MiB | 16 MiB | 20 MiB | 24 MiB |
|---|---:|---:|---:|---:|
| GRASP_PAPER | 11 / 11 | 49 / 49 | 9,512 / 9,476 | 30,648,801 / 15,216,259 |
| `POPT:UNCHARGED` | 164 / 164 | never / never | never / never | never / 16,439,057 |

- **Every figure is an extrapolation,** since none is seven passes or fewer.
- **From 20 MiB up there is little or no per-pass gain to repay with.** Against
  GRASP_PAPER it is 1,627 transfers per pass at 20 MiB and at most one at
  24 MiB. Against P-OPT it is zero from 16 MiB up, but for half a transfer by
  the RRPV order at 24 MiB.
- **Excluding pass 8 favours ECG wherever its last pass loses:** against both
  baselines at 20 and 24 MiB, and against P-OPT for governed-first at 16 MiB.
- **These counts are not comparable with the census study's break-even,**
  which assumed that every two-pass invocation repeats a gain measured from the
  construction's cache state. Here only the first pass starts from it, and a
  pass is not an independent query.

### Predictions

Six predictions were recorded before the run, and all six held:

- every row is stationary, including the one named as the likeliest
  exception, GRASP_PAPER on SpMV at 16 MiB;
- SpMV's gated question is a tie, and every SpMV row's window mean at 20 and
  24 MiB is within 0.1% of the floor;
- with low confidence, PageRank's gated question is a tie or an ECG lead at
  each of 20 and 24 MiB,
  most likely a tie at both, and the RRPV order's last-pass margin against
  GRASP_PAPER is −0.5% or worse at both. It was a tie at both, and the
  last-pass margins were −3.79% and −3.84%;
- SpMV's setup, entry state and first pass reproduce the census study's
  exactly, and so does GRASP_PAPER's SpMV second pass;
- at 8 and 16 MiB the RRPV order's window margin against GRASP_PAPER exceeds
  +5% on both kernels;
- the RRPV order's per-pass lift over governed-first is within 0.1% on SpMV at
  every capacity and on PageRank at 16–24 MiB, and positive on PageRank at
  8 MiB, where it is +0.47%.

### What the steady state settles and what it does not claim

- **Settled** on this graph and these capacities, in the functional cache
  model, over eight passes:
  - once the kernel has settled, the RRPV order ties GRASP_PAPER at 20 and
    24 MiB on both kernels, and no ECG row's window mean exceeds either
    baseline's at any capacity;
  - from 16 MiB up, ECG settles on the per-pass floor on both kernels;
  - where GRASP_PAPER still leads over the whole kernel, its lead is at one
    end: SpMV's first pass, which starts from the state setup leaves, and
    PageRank's last pass;
  - ECG's last pass differs because it requests no further pass. A second
    pass that requests none costs what an eighth does, and from 16 MiB up one
    that requests another sits on the floor, to within 4 transfers. On SpMV
    from 16 MiB up the difference is writebacks, and 479 misses for the RRPV
    order at 24 MiB. On PageRank from 16 MiB up the last pass moves 115,419 to
    135,369 more transfers than the seventh, including 27,945 to 30,777 added
    property misses, and every run has a last pass.
- **Not settled:**
  - whether PageRank's added last-pass misses are writes re-fetching retired
    lines or bounds that are not conservative. A later count answered it for
    these rows: they are writes; see [the re-fetch count](#the-re-fetch-count-pageranks-added-last-pass-misses-are-in-place-writes);
  - what a kernel invoked repeatedly with other work between invocations sees;
    the passes here share one run's cache state.
- **Not claimed:**
  - no new mechanism and no default change; the per-pass detail is census
    instrumentation;
  - nothing about another kernel or graph, timing, gem5 or Sniper;
  - a pass is not an independent query, and the break-even is not a query
    count;
  - no earlier verdict is revised;
  - a tie is not a lead, and neither boundary is a competitive claim.

## The re-fetch count: PageRank's added last-pass misses are in-place writes

[The last pass](#the-last-pass-wrapped-bounds-become-dead) left two readings of
PageRank's added last-pass misses, and the census could not separate them: a
write re-fetching a line the record has retired, or a bound that is not
conservative. A fixture, a passive count and a rerun of the steady state's
PageRank rows separate them. A one-bit declaration that would stop the
retirement was added beside the count, and it is not measured.

### A fixture that charges each added miss to one access

`exerciseLastPassInPlaceWrite`, in `bench/src_sim/test_ecg_record_cache.cc`,
replays PageRank's record-mode kernel access for access: three passes over a
64-vertex pull graph, with a last level of one 16-way set. Each case is
replayed twice, identically until the last pass. In one replay the last pass
requests no further pass, as the kernel's last pass does; in the other it
requests one, as if another pass followed. Each record is also decoded beside
the cache. So every last-pass access is tagged with whether a gather had
already retired its line DEAD in that pass, and every miss the final replay
adds is charged to one access.

- **With 16 values per line, as the kernel builds its records,** the one added
  miss is the in-place write, fetching again a line that a DEAD decode retired
  after its last gather. No gather reads a line after its DEAD decode. That
  holds at each of four record offsets within a line, under the LRU base and
  under governed-first.
- **A control built with 8 values per line** splits each real line across two
  builder lines, so a DEAD decode can precede the line's last gather: a bound
  that is not conservative, by construction. Its gathers do fetch retired lines
  again. Yet at three of the four offsets its census shows the write's
  signature, one line turned dirty per added miss. The doubled flow is
  therefore no evidence against a non-conservative bound; only a charge to
  each access separates the two readings.

On the kernel itself the second reading is excluded by construction. It builds
16 four-byte values per 64-byte line on a line-aligned array, and its records
run in the order of its gathers, so a DEAD decode is the pass's last gather of
that line. That is an argument, not a measurement; the count below measures it
on this graph.

### The written-in-place bit

A property region that the kernel also writes in place can now declare it.
`kNativeWrittenInPlace`, in `bench/include/ecg_record_native.h`, is one more
bit of the record control word, and it says that the kernel writes the region
by a reference no record describes. The decode ORs it into its has-next input,
so in a pass that requests no further one a wrapped bound decodes FINITE, with
the deadline it would carry before another pass, instead of DEAD. A line whose
write is still to come is then not retired.

- **Hardware cost:** one configuration bit per property region, in the existing
  control word, and one OR gate on the has-next input of the wrapped-bound
  decode. No per-line state, no port, no per-eviction work, no metadata traffic
  and no change to the record or its codec.
- **What it leaves alone:** the pass's own has-next result still comes from the
  has-next bit alone. cache_sim keeps the new bit when it rebuilds the control
  word for each pass, because a declared write belongs to the region, not to
  the pass.
- **With the bit clear nothing changes.** Before it existed, a control word
  that carried it failed closed.
- **It is coarse.** In the last pass it keeps every wrapped bound in the region
  from decoding DEAD, including those of lines whose write has already
  happened, so it is expected to give up the 8 MiB last-pass gain below. It
  would also hide a bound that is not conservative, since a split line would no
  longer be retired either. The count has to be read with the bit clear.
- **No kernel sets it.** No option or label carries it, and nothing on this
  page measures it.

`testNativeConfigurationAndBinding`, in `bench/src_sim/test_ecg_record.cc`, pins
the decode: with the enable bit alone a last-pass wrapped bound decodes DEAD,
and with the written-in-place bit added it decodes FINITE while the has-next
result stays false. In the fixture a last pass replayed with the bit retires no
line and repeats the ongoing replay miss for miss, whether each pass names its
base, as PageRank does, or opens through the managed cursor, as the shared
kernels do.

### The passive re-fetch count

cache_sim counts each DEAD-first victim that a later demand miss at the last
level fetches again. A victim counts once, under the access that fetched it:

- `ecg_record_dead_first_refetch_write`: a write;
- `ecg_record_dead_first_refetch_gather`: a gather that carries a record;
- `ecg_record_dead_first_refetch_read`: any other read.

A DEAD bound claims that no later reference exists, so each count is a
reference the bound did not describe. A write is the kernel's own in-place
update, which no record describes; a gather means the bound was not
conservative. The count runs from the statistics reset at kernel entry, with
no reset per pass, and a writeback from the level above is not a demand miss.
It is simulator bookkeeping, a set of victim line addresses. It reads no
decision state and changes no decision, and the modeled cache gains nothing.

`exerciseDeadFirstRefetchCount`, in the same file as the fixture, pins it on a
one-set, two-way cache: each kind of access counts under its own field, a
victim counts once, and the reset forgets every victim. In the fixture the
count equals the replay's charge to each access in every case. The combined
CSV does not carry the three fields, so they are read from the raw receipts.

### The study: the steady state's PageRank rows, rerun

The `ecg_refetch_count_cache` study reran the steady state's eight PageRank ECG
rows unchanged: governed-first and the RRPV order at 8, 16, 20 and 24 MiB, with
eight passes after one construction, `-o 0 -n 1 -i 8 -t 0`. It adds no label,
option or mechanism, and no row sets the bit. Its gate was frozen before the
run:

- **Integrity first,** as in the steady state, and two checks on each row. The
  read count must be 0, because in record mode the kernel touches its
  contributions only through gathers and the in-place write. The DEAD-first
  victim count must be positive, because a count that reads exactly zero is not
  a check.
- **The split falsifier:** the gather count must be 0 on all eight rows. A
  positive count would mean a bound that is not conservative, and would stop
  the study for a report.
- **The gated question,** at 16–24 MiB, where the last pass adds misses:
  `r = write / Δ`, where Δ is the row's pass-8 property misses less its pass-7
  property misses, both from its own per-pass detail. It is confirmed when
  0.9 ≤ r ≤ 1.1 on all six rows. The two 8 MiB rows, whose last pass gains,
  are reported only.

Every re-fetch is read as the last pass's, which
[where the re-fetches fall](#where-the-re-fetches-fall) checks.

Integrity was clean on all eight rows:

- two distinct labels, one binary and one PageRank result;
- every RRPV-order decision was RRPV-ordered and none reached an LRU base, and
  no governed-first row made one;
- every census validates, with eight detailed passes, and every row has a read
  count of 0 and a positive DEAD-first count.

**The change that added the bit and the count moved no row.** Each row's entry
state, all eight of its passes and its result reproduce the steady state's same
row exactly. So every figure below holds for the steady state's rows too.

| PageRank | DEAD-first victims | Write | Gather | Read | Property misses, pass 7 | Pass 8 | Δ | r |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 MiB, governed-first | 98,732 | 11,301 | 0 | 0 | 3,472,827 | 3,471,117 | −1,710 | reported only |
| 8 MiB, RRPV order | 96,943 | 11,837 | 0 | 0 | 3,442,763 | 3,440,851 | −1,912 | reported only |
| 16 MiB, governed-first | 129,874 | 30,777 | 0 | 0 | 235,923 | 266,700 | +30,777 | 1 |
| 16 MiB, RRPV order | 129,586 | 30,429 | 0 | 0 | 235,923 | 266,352 | +30,429 | 1 |
| 20 MiB, governed-first | 124,377 | 28,947 | 0 | 0 | 235,923 | 264,870 | +28,947 | 1 |
| 20 MiB, RRPV order | 123,888 | 28,462 | 0 | 0 | 235,923 | 264,385 | +28,462 | 1 |
| 24 MiB, governed-first | 125,156 | 28,196 | 0 | 0 | 235,923 | 264,119 | +28,196 | 1 |
| 24 MiB, RRPV order | 125,060 | 28,154 | 0 | 0 | 235,923 | 263,868 | +27,945 | 1.0075 |

- **The split falsifier holds.** On no row does a gather or any other read fetch
  a DEAD-first victim again. So on this graph no gather paid a miss for a DEAD
  bound, which is where a bound that is not conservative would cost one.
- **The gated question is confirmed.** r is exactly 1 on five of the six gated
  rows, and 1.0075 on the RRPV order at 24 MiB. From 16 MiB up the last pass's
  added property misses are the in-place write fetching again the lines that
  DEAD-first eviction retired: exactly on five rows, and to within 209 misses,
  0.75%, on the sixth.

**Each re-fetch adds two to the flow.** On all six rows at 16–24 MiB the added
writebacks and the change in resident dirty lines sum to Δ, and the flow rises
by 2Δ: each re-fetch adds one miss and one line turned dirty, which is written
back or is still dirty at the end. That is the doubled flow the steady state
found. The write accounts for 22.5–23.7% of the DEAD-first victims at
16–24 MiB and 11.4–12.2% at 8 MiB; no access fetches the rest again.

### Where the re-fetches fall

The DEAD-first and re-fetch counts are kernel totals; only the per-pass detail
is per pass. Reading the write count against the last pass rests on two
things:

- **The decode.** A bound decodes DEAD from a DEAD token, which reads the same
  in every pass, or from a wrapped bound in a pass that requests no further
  one, which only the last pass does.
- **Two passes against eight.** The census study's rows ran two passes from an
  older build. On seven of the eight rows they have the same DEAD-first victims
  as both eight-pass runs, and on the RRPV order at 8 MiB 28 more, although the
  eight-pass runs make about four times as many victim decisions. So passes 2–7
  add no DEAD-first victim. That the first pass adds none rests on the decode,
  not on a count.

On that reading the RRPV order's last pass at 24 MiB fetched 235,714 property
lines other than its re-fetches. That is 209 fewer than its seventh pass, and
207 fewer than its lowest of passes 2–7, 235,921 in pass 2. Every other row at
16–24 MiB fetched exactly 235,923 other property lines in its last pass, as in
each of its passes 2–7. So that row's last pass also carries a small saving,
with the sign of the 8 MiB gain below. The receipts do not say which stream it
comes from, and it is unattributed.

### At 8 MiB the last pass gains

- **Governed-first's** write fetches back 11,301 lines, and its other property
  misses fall by 13,011, against passes 2–7 that each have 3,472,827.
- **The RRPV order's** write fetches back 11,837 lines, and its other property
  misses fall by 13,749 against pass 7, or by 13,697 against its lowest of
  passes 2–7.

So at 8 MiB the last pass's DEAD decodes save more other property misses than
the write fetches back: evicting DEAD lines first keeps more of the other lines
resident. The written-in-place bit, which keeps every wrapped bound in the
region from decoding DEAD, is expected to give up both parts.

### Predictions

Four predictions were recorded before the run, with medium confidence, and one
held:

- the gather and read counts are 0 on every row: held;
- r is exactly 1 on each of the six gated rows: failed on the RRPV order at
  24 MiB, where it is 1.0075;
- each gated row's write count equals the steady state's Δ, provided its passes
  7 and 8 reproduce the steady state's: failed on the same row, 28,154 against
  27,945;
- the sharper form, r ≤ 1, since a re-fetch that evicts a line needed again
  adds a miss to Δ but not to the write count: failed on the same row.

The three failures are the same 209 misses. The gate reads r > 1.1 as
DEAD-first saving misses that the floor says do not exist, and 1.0075 lies
inside the band with that sign.

The budget was eight rows in eight jobs, to run once. All eight ran once, none
was void and none was rerun, and the budget is closed.

### What the re-fetch count settles and what it does not claim

- **Settled** for PageRank on this graph and these capacities, in the
  functional cache model, with the bit clear:
  - from 16 MiB up, the last pass's added property misses are the in-place
    write fetching again the lines that DEAD-first eviction retired: exactly on
    five of the six rows, and to within 209 misses on the sixth;
  - no gather or other read fetches a DEAD-first victim again, so no gather
    paid a miss for a DEAD bound;
  - at 8 MiB the last pass's DEAD decodes save more property misses than the
    write fetches back;
  - the bit and the count change nothing while the bit is clear.
- **Not settled:**
  - whether the written-in-place bit removes the re-fetches without other
    effects, and what it costs at 8 MiB; it is not measured;
  - the 209-miss saving in the RRPV order's last pass at 24 MiB;
  - whether the in-place write costs anything in a pass that has a next pass.
    It follows each line's last gather there too, while the line holds a bound
    for the next pass. From 16 MiB up those passes sit on the floor, so any
    such cost could show only at 8 MiB;
  - another kernel's record builder, and another graph.
- **Not claimed:**
  - no measured mechanism and no default change: the count is
    instrumentation, and no kernel sets the bit;
  - nothing about another kernel or graph, timing, gem5 or Sniper;
  - no competitive claim: the study ran no baseline;
  - no earlier verdict is revised.

## Status and default

The rule is implemented, tested and off by default. Turning it on by default
would change every existing ECG result in this repository, so it is a deliberate
decision rather than a consequence of this measurement, and it has not been
taken.

All three backends now reach the option through the one shared rule. cache_sim
takes `--record-governed-first on`, gem5 takes `--ecg-record-governed-first on`
through a `governed_first` policy parameter, and Sniper reads
`SNIPER_ECG_RECORD_GOVERNED_FIRST`, its record path being configured by
environment rather than by a command line. All three hand the same
`comparisonWatermark()` sequence to the rule, so they cannot disagree about
expiry if a non-default clock is ever selected.

**No native cell has been run**, and the native binaries are not rebuilt, so
nothing on this page is a timing result or a native measurement. The wiring
exists so that a native comparison is a build and a run rather than a
plumbing exercise, and it is pinned by
`scripts/test/test_governed_first_reaches_every_backend.py`, which fails if any
of the sites is reverted and which also checks that the installed simulator
checkouts have not drifted from the overlays that own them. For Sniper it
compares every shared record header on the installer's own copy list, not only
the runtime header.

The two later options are narrower. The RRPV order, `--record-rrpv-order`, and
the pressure gate, `--record-pressure-gate` with `on` for the per-set counter
and `duel` for set dueling, exist in cache_sim only. Both are off by default,
and the gem5 and Sniper harnesses refuse them rather than run a cell under a
label whose behaviour they do not implement.

The written-in-place bit is narrower still. It is part of the shared record
codec, and cache_sim keeps it from pass to pass, but no kernel sets it and no
option or label carries it. The shared algorithm kernels, gem5's guest harness
and Sniper's runtime and harness each rebuild the control word from a has-next
flag, so each would drop the bit, and Sniper's cache model reads only the
has-next bit. Carrying it to a native backend is a change across the backends
that has not been made.

The kernel census is cache_sim instrumentation, not an option. Every cache_sim
kernel arms it at the kernel boundary and adds its fields to the receipt. It
changes no decision, and gem5 and Sniper have no counterpart. Its per-pass
detail is the same instrumentation, and stops at 64 passes.

The fair-comparison contracts are cache_sim options, off by default. Native
runners refuse them.

- **`--grasp-registration declared`** makes each kernel phase designate the
  arrays upstream GRASP protects, at a declared fraction of the last level:
  - SpMV's `x`, top-down BFS's `depth` and SSSP's `distances` take the whole
    capacity, as upstream's BellmanFordOpt does.
  - PageRank's `contribution` takes half, as PageRankOpt does.
  - BC's `path_counts` (forward) and `dependency` (backward) take half each,
    as upstream's BC does. `depth`, which every BC edge reads, takes the other
    half, the share upstream gives its frontier bitmap.

  Every other property array stays a property region but not a GRASP region.
  Kernels with no declaration refuse GRASP tiers under the contract.
- **`--kernel-entry cold`** writes back every dirty line once, invalidates
  every level and charges that maintenance to setup, so every row starts its
  kernel empty.

PageRank takes both from the runner, together with a GRASP-based transport
control, `--record-base-policy GRASP_PAPER`. Every receipt states the
registration, the declarations, the kernel entry and the last level's hits and
misses per named property region.

## Reproducing

```bash
make -j1 PARALLEL=1 sim-algorithms sim-pr
python3 -m pytest -q scripts/test/test_ecg_record.py \
  scripts/test/test_ecg_record_cache.py \
  scripts/test/test_record_victim_arms_reach_the_kernel.py \
  scripts/test/test_resolved_labels_match_the_runner.py \
  scripts/test/test_ecg_algorithms.py \
  scripts/test/test_kernel_census.py
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_governed_first_cache --run-dir <run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_pressure_duel_cache --run-dir <duel-run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_rrpv_order_cache --run-dir <rrpv-run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_kernel_census_cache --run-dir <census-run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_steady_state_cache --run-dir <steady-run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_refetch_count_cache --run-dir <refetch-run-dir>
```

Both kernels must be rebuilt: `pr` is a separate executable configured by
environment rather than by the algorithm CLI, and a shared `DEP_ECG` header
feeds both. `pr` reads the two later options as `ECG_RECORD_PRESSURE_GATE` and
`ECG_RECORD_RRPV_ORDER`. `testGovernedFirstEviction` in
`bench/src_sim/test_ecg_record.cc` pins the rule itself, and the script tests
pin each option and its output label across every site that computes one,
because a correctly executed cell that is labelled as its own control is
indistinguishable from a missing policy.

The later options have their own tests:

- `testRrpvOrderNeverReadsRecency` and `testRrpvOrderIsGraspWithoutGovernedWays`,
  in the same file, pin the RRPV order. Each case of the first is built so that
  recency and the RRPV disagree. The second is the equality with GRASP's victim
  and ageing in a set with no governed way.
- `exerciseRecordPressureDuel` and `exerciseRrpvOrderWithoutLru`, in
  `bench/src_sim/test_ecg_record_cache.cc`, pin the selector's training and the
  cache's refusal of any decision under the RRPV order that would reach an LRU
  scan. The duel also refuses a set count that would give one leader slot more
  sets than the other.

The kernel census has its own tests:

- In `bench/src_sim/test_ecg_record_cache.cc`:
  - `exerciseKernelCensus` checks every count against a hand-traced stream;
  - `exerciseKernelCensusIsPassive` requires the receipt under LRU, FIFO,
    RANDOM, SRRIP and GRASP, less its census, to be byte-identical to a
    receipt without one;
  - `exerciseCleanEntryCounterfactual` cleans the last level at the kernel
    boundary. Every hit, miss and eviction must stay the same, and exactly the
    writebacks the census charged to setup must move;
  - `exerciseKernelCensusFailsClosed` refuses anything that would misplace a
    boundary, such as a kernel boundary inside a pass or a statistics reset
    after the boundary.
- `testKernelCensusUnderTheRoster`, in `bench/src_sim/test_ecg_algorithms.cc`,
  runs the study's four roster policies through the algorithm backend in fresh
  processes. It requires an exact repeat and a passive census. A clean entry
  and an all-dirty entry must move nothing but the writebacks, and the clean
  figure must equal a clean-entry run's.
- `scripts/test/test_kernel_census.py` holds the receipt validator to the
  census contract. It requires both executables to emit a census through the
  real runner.

The per-pass detail has its own tests:

- `exerciseKernelCensusPassDetail`, in
  `bench/src_sim/test_ecg_record_cache.cc`, requires each detailed pass to be
  what the census would have reported had the kernel ended with that pass.
  Under LRU, FIFO, RANDOM, SRRIP and GRASP, a replay cut right after pass `k`
  must detail the full replay's first `k` passes, and its exit must be the full
  replay's end of pass `k`.
- `exerciseKernelCensusPassDetailIsBounded`, in the same file, stops the detail
  at 64 passes. The passes beyond it are still counted and charged, but not
  detailed.
- In `scripts/test/test_kernel_census.py`,
  `test_a_census_details_each_pass_it_ran_up_to_its_limit` requires a census to
  detail each pass it ran, up to the limit, and
  `test_a_pass_detail_that_does_not_divide_its_passes_is_refused` refuses a
  detail that disagrees with its census. It must cover the passes, sum to the
  first-pass and later-pass segments, keep consistent line counts at each
  pass's end, never mark a setup-dirty line again after the mark retired, and
  end where the census's exit does.

The written-in-place bit and the re-fetch count have their own tests:

- `testNativeConfigurationAndBinding`, in `bench/src_sim/test_ecg_record.cc`,
  requires a last-pass wrapped bound to decode DEAD under the enable bit alone
  and FINITE with the written-in-place bit, with the has-next result unchanged.
- `exerciseLastPassInPlaceWrite`, in `bench/src_sim/test_ecg_record_cache.cc`,
  replays PageRank's kernel with its last pass final, ongoing and written in
  place, and charges each added miss to one access. The written-in-place
  replay must repeat the ongoing one under both pass interfaces, the split
  control must fetch a retired line back by a gather, and the count must equal
  the replay's charge.
- `exerciseDeadFirstRefetchCount`, in the same file, requires each kind of
  access to count under its own field, a victim to count once, and the
  statistics reset to forget every victim.
