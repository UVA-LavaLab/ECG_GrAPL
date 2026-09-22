# Governed-first eviction: giving reuse-free lines strict precedence

This cache-only study fixes one defect in `ecg_record::selectVictim` that the
[mask failure taxonomy](Mask-Failure-Taxonomy) located by measurement: on 39.1%
of SpMV evictions and 34.7% of PageRank evictions the victim the record rule was
handed was not a governed line at all, so the rule passed it through while
governed property lines were displaced by a structural stream.

**The arm won on both kernels**, which is unusual enough on this page's
neighbours to state plainly up front. It is a result about `selectVictim`
against its own previous ordering. It is **not** a competitiveness claim against
GRASP or P-OPT, and it is not a timing result — `timing_valid_for_speedup` is
`false` in every receipt quoted here.

The rule is off by default, as `--record-governed-first no`.

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
    std::size_t ungoverned = count;
    for (std::size_t index = 0; index < count; ++index) {
        if (!ways[index].property &&
            (ungoverned == count || ways[index].recency < ways[ungoverned].recency))
            ungoverned = index;
    }
    if (ungoverned != count) {
        victim = ungoverned;
        if (trace) trace->path = VictimPath::UNGOVERNED_FIRST;
        return Status::OK;
    }
}
```

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

One **governed bit per line**, which is already present as `WayState::property`
and is already read for the existing record rule — no new storage.

One **comparison across the ways of the selected set** to find the
least-recently-used ungoverned way. This is the same scan shape the cache
already performs to select an LRU victim, over a set that has already been read
out for the replacement decision, and it can share that comparator tree. The
rule is a reordering of an existing priority list, evaluated after the DEAD scan
and before the base victim.

No new ports, no new metadata traffic, no change to the record codec, no change
to the window rule, and no change to what the producer publishes.

## What this result does not claim

- **The frozen gate is arm-to-arm.** The matched baseline comparison in the
  next section was authorized and gated separately, as a reproduction check; it
  is reported as a measured ROI comparison on SpMV at one capacity, not as a
  general competitiveness claim for the complete design.
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

| SpMV, 8 MiB/16-way, 2 passes | Common setup | Mechanism prep | ROI kernel | ECG's ROI margin |
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

| PageRank, 8 MiB/16-way, 2 iterations | ROI kernel | ECG's ROI margin |
|---|---:|---:|
| GRASP_PAPER | 15,975,818 | **+18.56%** |
| `POPT:UNCHARGED` | 13,880,532 | **+6.26%** |
| ECG base-first (no governed-first) | 16,961,478 | −5.79% |
| **ECG governed-first** | **13,011,200** | — |

**On PageRank the rule is the difference between losing and leading.** Without
it, ECG is 6.17% worse than GRASP and 22.20% worse than the favourable P-OPT
control. With it, ECG leads both, and the P-OPT margin of 6.26% is seven times
SpMV's.

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

On the ROI the governed-first arm leads both baselines, including the
**favourable** P-OPT control — full data capacity, no ways reserved for the rank
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

## Status and default

The rule is implemented, tested and off by default. Turning it on by default
would change every existing ECG result in this repository, so it is a deliberate
decision rather than a consequence of this measurement, and it has not been
taken. The native gem5 and Sniper backends carry the same shared header and the
same option, but no native cell has been run for it.

## Reproducing

```bash
make -j1 PARALLEL=1 sim-algorithms sim-pr
python3 -m pytest -q scripts/test/test_ecg_record.py \
  scripts/test/test_record_victim_arms_reach_the_kernel.py \
  scripts/test/test_resolved_labels_match_the_runner.py
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_governed_first_cache --run-dir <run-dir>
```

Both kernels must be rebuilt: `pr` is a separate executable configured by
environment rather than by the algorithm CLI, and a shared `DEP_ECG` header
feeds both. `testGovernedFirstEviction` in `bench/src_sim/test_ecg_record.cc`
pins the rule itself, and the two script tests pin the option and its output
label across every site that computes one, because a correctly executed cell
that is labelled as its own control is indistinguishable from a missing policy.
