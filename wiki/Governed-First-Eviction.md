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

Two later studies on the same graph and capacities are reported here too:

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

The [hardware cost](#hardware-cost) of the rule as first measured has been
**corrected**. It assumed the cache already keeps a recency order, and a GRASP
cache, which is SpMV's base here, does not. The RRPV order removes that
dependence.

Every gated result is about `selectVictim` against its own previous ordering,
and the matched comparisons are functional cache results on one graph. None is
a timing result: `timing_valid_for_speedup` is `false` in every receipt quoted
here.

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
  not close it.

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
  avoidance. These receipts do not separate the two.
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

Why SpMV's residual persists is open. One untested candidate is the cache state
the kernel inherits from construction. The boundary between construction and the
kernel keeps the cache's contents, and at 20 MiB a record policy's setup moves
18.04 million transfers through the cache against GRASP's 2.77 million. The
receipts carry no per-pass split, so this cannot yet be separated from the
kernel's own decisions.

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
checkouts have not drifted from the overlays that own them.

The two later options are narrower. The RRPV order, `--record-rrpv-order`, and
the pressure gate, `--record-pressure-gate` with `on` for the per-set counter
and `duel` for set dueling, exist in cache_sim only. Both are off by default,
and the gem5 and Sniper harnesses refuse them rather than run a cell under a
label whose behaviour they do not implement.

## Reproducing

```bash
make -j1 PARALLEL=1 sim-algorithms sim-pr
python3 -m pytest -q scripts/test/test_ecg_record.py \
  scripts/test/test_ecg_record_cache.py \
  scripts/test/test_record_victim_arms_reach_the_kernel.py \
  scripts/test/test_resolved_labels_match_the_runner.py
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_governed_first_cache --run-dir <run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_pressure_duel_cache --run-dir <duel-run-dir>
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_rrpv_order_cache --run-dir <rrpv-run-dir>
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
