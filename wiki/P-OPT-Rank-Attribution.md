# P-OPT future-rank attribution

The failed [costed window](Potential-Window-Prototype) remains a negative
control. [Phase-scoped GRASP](BFS-Phase-Attribution) is the stronger common
BFS baseline, not an ECG masking contribution. Before constructing another
metadata producer, this study isolates how much P-OPT's future ranks add
under its existing region and replacement mechanics.

## One isolated consumer ablation

The current cache-only SpMV and sorted TD-BFS executables accept
`--popt-rank-mode future|constant` with `--policy POPT_UNCHARGED`.
The default `future` retains the intact graph-derived FULL-rank control.
The constant arm is labeled **`POPT_UNCHARGED_CONST_RANK`**, with
`policy_ablation=true` and P-OPT receipt role `policy-ablation`; it must
never replace the real P-OPT baseline in a performance comparison.

Only the ranks passed to the all-property victim comparison become zero.
Invalid ways are still filled first; non-P-OPT-property data are still
preferred victims. Among maximum-rank candidates, the original RRIP
tie-breaking and aging are unchanged. Constant ranks deliberately make
every covered way part of that tie set. Insertion, hit promotion, recency,
dirty state, region coverage and outside-pass LRU remain unchanged.
Cache contents are never reset at a phase boundary.

Both arms build the same complete 256-epoch FULL matrix, including typed
banks and the kernel's declared source/neighbor references. Both retain all
nominal data ways and favorable **free runtime lookup traffic**; neither
is an equal-area or charged-lookup result. Construction and ordinary
algorithm requests remain charged. Nothing executes BFS or SpMV in
advance to manufacture future information.

The real matrix lookup is retained even in the constant arm. Its returned
value contributes to `original_rank_sum` before being withheld from victim
selection, preventing an unused lookup from disappearing. `lookup_calls`
and `constant_rank_lookups` show where this occurs. Their totals may differ
between arms because changed victims produce different later cache states.

`matrix_digest` fingerprints finalized bytes in the existing bounded
64-line-block, reverse-epoch, ascending-lane completion pass. This adds no
matrix allocation or graph scan. The digest and lookup sums are diagnostic
receipts, not new policy inputs or native timing measurements.

## Frozen six-cell development study

`ecg_popt_rank_attribution` fixes full prepared DBG Patents, 32 KiB L1D and
256 KiB L2 (both eight-way), and an 8 MiB/16-way LLC with 64-byte lines.
There is no prefetching, reordered input, source search or parameter sweep.
Five serial jobs contain exactly six policy cells:

| Kernel | Fixed work | Strong GRASP | Real P-OPT | Constant-rank diagnostic |
|---|---|---|---|---|
| SpMV | Two identical fixed-input sweeps | Ordinary GRASP | Yes | Yes |
| TD BFS | Sorted frontiers, stored source 0 | Graph-pass GRASP / outside-pass LRU | Yes | Yes |

All BFS arms enable passive phase/role traffic attribution. P-OPT uses its
existing source/pass clock and receives no GRASP-only phase switch.
The GRASP phase-control ledger is reported separately from data traffic.
Each cell has a 2 GiB process-tree RSS limit, a 512 MiB algorithm workspace,
one simulator thread and a thirty-minute timeout.

```bash
ulimit -c 0
make -j1 bench/bin_sim/algorithms
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_popt_rank_attribution \
  --run-dir results/ecg_experiments/runs/popt_rank_patents_case \
  --no-build --no-resume
```

Use a clean committed tree and retain the resolved manifest, raw JSON/logs,
binary/input fingerprints and completion markers. Compare work and result
receipts across all three policies, and matrix digests, construction bytes
and setup state between the two P-OPT arms. Report property and other-data
demand misses, writebacks, kernel transfers and setup-inclusive transfers.
Writebacks by BFS role belong to the triggering request, not necessarily
to the evicted data's owner.

The preregistered per-kernel information margin is
`(constant_rank_kernel_transfers - ranked_POPT_kernel_transfers) /
constant_rank_kernel_transfers`. A margin **>= 2%** warrants considering
further information-design work on that sentinel; a smaller or negative
margin prioritizes other policy/region/cost explanations. This is a
development-budget gate, not prediction accuracy, held-out evidence,
automatic authorization to build both proposed masks, or proof that a
different causal signal cannot work. The planned follow-on study remains
capped at twelve total cells, including these six.

Beating this intentionally weakened diagnostic would not establish an
ECG win over GRASP or intact P-OPT. Setup amortization, physical metadata
cost, native execution time and suite-wide competitiveness remain separate
requirements.

## Results: fixed Patents sentinels

The six cells completed at implementation commit `945ddf04` in
`results/ecg_experiments/runs/popt_rank_patents_945ddf04`.
All three policies execute identical work and produce identical results
within each kernel. The four P-OPT cells have the same 60,396,288-byte
matrix (235,923 lines, 256 epochs), digest `3683564403557561320`, and
construction reads/writes of 328,708,247 / 191,849,776 bytes.

The table uses the existing **registered-property** LLC counter.
For SpMV this includes both `x` and streamed `y`; only `x` receives P-OPT
matrix priority. These are not separately measured `x`-only misses.
For BFS the registered property is depth. Other misses exclude those
registered arrays. Transfers are demand misses plus writebacks; prefetch
fills are zero.

| Kernel | Policy | Property misses | Other misses | Writebacks | Kernel transfers |
|---|---|---:|---:|---:|---:|
| SpMV | Strong GRASP | 9,847,988 | 5,073,432 | 535,510 | 15,456,930 |
| SpMV | Intact favorable P-OPT | 6,855,000 | 5,073,432 | 469,984 | 12,398,416 |
| SpMV | Constant-rank diagnostic | 11,432,470 | 5,073,432 | 470,599 | 16,976,501 |
| BFS | Phase-scoped GRASP | 4,721,503 | 6,306,233 | 363,920 | 11,391,656 |
| BFS | Intact favorable P-OPT | 4,286,026 | 6,331,723 | 235,200 | 10,852,949 |
| BFS | Constant-rank diagnostic | 5,433,167 | 6,331,566 | 235,197 | 11,999,930 |

Keeping future ranking saves **4,578,085 kernel transfers (26.97%)** on
SpMV and **1,146,981 (9.56%)** on BFS relative to the corresponding
constant-rank arm. Both exceed the preregistered 2% development gate.
SpMV saves 4,577,470 registered-property misses and 615 writebacks, with
unchanged other misses. BFS saves 1,147,141 depth misses, offset by 157
additional other misses and three additional writebacks.

This is not merely P-OPT's region priority: the ablation retains that
priority but loses to the applicable GRASP baseline on both kernels.
Intact P-OPT instead saves 19.79% / 4.73% kernel transfers against
SpMV / phase-scoped BFS GRASP. It does not follow that either proposed
edge-carried producer can recover these benefits.

### Construction remains the practical target

| Kernel | Strong GRASP setup + kernel | Intact P-OPT setup + kernel | Constant-rank setup + kernel |
|---|---:|---:|---:|
| SpMV | 18,334,421 | 21,691,923 | 26,270,008 |
| BFS | 14,269,149 | 20,146,460 | 21,293,441 |

Both P-OPT arms pay identical setup traffic: 9,293,507 transfers for SpMV
and 9,293,511 for BFS, versus 2,877,491 / 2,877,493 under their GRASP
controls. Even with free runtime lookup, intact P-OPT therefore costs
18.31% / 41.19% more setup-inclusive traffic than strong GRASP in these
workloads. This does not measure multi-query amortization or native time.

### Retained receipts and launch-address confirmation

The original BFS constant arm has one extra modeled construction access
(`total_accesses` 353,278,647 versus 353,278,646), but every other setup
counter is equal. A subsequent serial pair used the same output path,
disabled ASLR and byte-balanced argument/environment strings. All setup
counters then matched, and **every original kernel counter and P-OPT
receipt field was reproduced exactly**. This launch-dependent setup
counter does not explain the measured gain. The original six rows were
retained unchanged; these two executions are confirmation, not new
algorithm/source/capacity choices or a best-of-policy search.

The run's `audit/` directory retains that pair's raw JSON/log/watchdog
receipts, the receipt-replay script, and a separately hashed audit marker.
The audit closes five job markers, six cells, and 180 recorded input
fingerprints across 33 distinct files.

| Artifact | SHA256 |
|---|---|
| `combined_roi_matrix.csv` | `6d209b88a0baa4629e04c0c3bd26f7311df31c2d28474a79f06e65285ac46263` |
| Shared executable | `2b76d3d8b3fc3b928e1468b8d8a1f3e9ba5c9c2858c1d46ce851cfb71b75222b` |
| `audit/audit.complete.json` | `08af3bae8edc533ace35a190fcb4dfa3529632150f34bcbb97e90d3375f7721d` |

**Decision:** continue a bounded information-plus-action design rather
than abandon future information for region/RRIP tuning alone. The failed
window stays a negative control. Neither the learned-signature candidate
nor the current-frontier-conditioned mask is implemented or validated by
this result; each still needs a frozen receiver/cost contract and its own
cost-matched ablation before any competitiveness claim.

For budget accounting, the two full-graph confirmation executions also
count against the twelve-run ceiling. Eight runs have been used, leaving
**at most four additional runs** without an explicit budget revision;
the original six-cell A/B follow-on roster is no longer an executable
default.
