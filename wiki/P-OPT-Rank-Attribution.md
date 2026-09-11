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
