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
figure is its whole figure and it has no setup-inclusive counterpart.

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

- **Not a competitiveness claim, and the setup line is why.** The gate is
  arm-to-arm within one build. Context is given in the next section with both
  halves of the accounting; the kernel-transfer half is favourable and the
  setup-inclusive half is not, and neither is a preregistered claim.
- **Nothing about BFS, SSSP or BC.** They were excluded from the study because
  they lack a live bound at the moment of choice and an ordering rule cannot
  supply one. Governed-first may still pay there for the same reuse-free-stream
  reason, but that is an untested hypothesis and extending the claim would need
  its own study and its own gate.
- **Nothing about insertion.** The record bound is still not used to place an
  incoming line. That remains the open candidate for the dense kernels.
- **Nothing about timing.** These are functional cache results.

## Context against the named baselines, with setup shown

This context was **not** part of the frozen gate, and no threshold here was set
before the result existed. It is reported because omitting it would be
selective, and it is reported with its unfavourable half attached.

The SpMV control reproduces the `ECG_REPLACEMENT_BASE_GRASP_PAPER` cell of the
42-cell `competitive_8mb_patents_074cbf75` study **exactly on both phases** —
kernel 14,916,849, setup 19,195,101, `result_digest` `55e3fc26a1027ddb`. A
shared cell that reproduces byte-identically demonstrates a common footing for
SpMV at this capacity rather than assuming one, which is what makes the
surrounding rows worth printing at all.

| SpMV, 8 MiB/16-way | Kernel | Setup | Setup+kernel |
|---|---:|---:|---:|
| LRU | 24,143,623 | 2,877,491 | 27,021,114 |
| GRASP_PAPER | 15,456,930 | 2,877,491 | 18,334,421 |
| `POPT:UNCHARGED` | 12,398,416 | 9,293,507 | 21,691,923 |
| ECG-R/G base-first | 14,916,849 | 19,195,101 | 34,111,950 |
| **ECG-R/G governed-first** | **12,289,362** | 19,195,101 | 31,484,463 |

On **kernel transfers** the governed-first arm crosses `POPT:UNCHARGED` by
109,054, or 0.88%, and closes `(14,916,849 - 12,289,362) / (14,916,849 -
12,398,416) = 1.0433` of the ECG-to-P-OPT kernel gap. `POPT:UNCHARGED` is the
favourable P-OPT control: full data capacity, no ways reserved for its rank
matrix, construction counted but the matrix stream not replayed.

On **setup-inclusive traffic the arm loses to every baseline in the table** — by
45.14% against P-OPT and 71.72% against GRASP — because record construction
costs 19,195,101 transfers where GRASP pays 2,877,491 and P-OPT pays 9,293,507.
Construction would have to fall by 51.0%, to below 9,402,561 setup transfers,
merely to tie `POPT:UNCHARGED` on the setup-inclusive total for a single SpMV
pass. Against GRASP's 2,877,491 no plausible reduction closes it on one pass;
that comparison requires reuse across passes or queries, which is measured
separately in [reusable SpMV queries](Reusable-SpMV-Queries) and must not be
obtained by relabelling a longer single run as independent work.

The honest reading is therefore narrow and specific. **Once the victim rule is
right, the mechanism is competitive on the traffic it is meant to improve, and
its remaining cost is in producing the carrier.** Confirming even that much
needs `POPT:UNCHARGED` and `GRASP_PAPER` cells in this same profile and build,
which have not been run.

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
