# GRASP reference-consumer diagnostic

This cache-only study asks whether a specified GRASP-based consumer can
exploit graph-derived future ranks when those ranks are available at
eviction. It follows the [P-OPT rank ablation](P-OPT-Rank-Attribution) and
the failed [frontier-mask experiment](Frontier-Cohort-Mask).
It is not another edge encoding, a deployable ECG policy, or a native
performance result.

## Fixed consumer

`--grasp-reference off|full|flat` is opt-in for CSR SpMV with
`--policy GRASP_PAPER`. The default `off` preserves ordinary GRASP and
does not construct a reference matrix. Other algorithms, record modes,
native backends and conflicting phase/observer/rank controls are rejected.

| Arm | Comparison information | Output label |
|---|---|---|
| FULL | Actual canonical P-OPT FULL ranks | `DIAG_GRASP_REFERENCE_FULL` |
| FLAT | Zero ranks after computing the real lookup | `DIAG_GRASP_REFERENCE_FLAT` |

The selector fills invalid ways first, then obtains the actual GRASP
victim exactly once, including its usual RRIP aging. If the source clock
or matrix is inactive, or the base victim is not a matrix-covered
property line, it retains that victim.

Otherwise, all covered property ways are considered. Only a **strictly
farther** rank can replace the base/current selection. Equal ranks retain
the existing order. There is no RRPV-7 candidate filter and no extra RRIP
aging after the base decision. FLAT therefore never refines the victim.
Insertion, hit promotion, recency and dirty state remain ordinary GRASP.
Outside graph passes the diagnostic also retains GRASP, not P-OPT's LRU
fallback.

This is not intact P-OPT: it does not add non-property eviction priority or
rank-first selection with RRIP aging confined to maximum-rank candidates.
It is not an exact clone of current-next ECG either, whose explicit-DEAD
and equal-future recency handling differ. Current-next is already able
to compare live property futures across ways; this experiment must not
be misrepresented as simply removing its RRPV-7 restriction.

## Information and cost isolation

FULL and FLAT share the complete canonical 256-epoch matrix builder,
allocation, source progress and typed/property coverage. For SpMV the
matrix covers `x`; streamed output `y` does not get reference ranks.
They have identical GRASP setup, data capacity and initial algorithm
state. Matrix construction is counted before the kernel snapshot; cache
contents are not reset at that boundary.

Both arms actually look up the matrix and accumulate the original rank
sum. Only the comparison rank becomes zero in FLAT. Lookup totals can
differ later because different victim choices change cache history.
`grasp_reference` receipts distinguish active graph-pass decisions,
covered base victims, refinements and refinements selecting RRPV below 7.
Those counts are realized decisions, not counterfactual traffic savings.

The reference matrices have **ideal availability at victim selection**.
Runtime matrix traffic is deliberately uncharged, and all nominal data
ways remain available, matching the favorable P-OPT quality convention.
Backing storage, construction and ordinary algorithm traffic are still
reported. No algorithm is pre-executed to manufacture future accesses.
The reference is graph-derived and quantized, not an exact future oracle.

The matrix cannot be hidden inside a later low-overhead ECG candidate.
These diagnostics have no new real-record delivery path, so they do not
establish compact-codec fidelity, timely publication, physical area,
charged-lookup performance or native speed.

Ordinary GRASP does not have the diagnostic's matrix preparation, while
the intact current P-OPT baseline uses its original LRU setup. Only
FULL versus FLAT is the controlled information contrast; differences
against the intact baselines include their respective setup/cache state.

## Four-cell development gate

The `ecg_grasp_reference_cache` profile runs exactly four cells on the
fixed full DBG Patents graph, SpMV two sweeps, 32 KiB L1D and 256 KiB L2
(both eight-way), 8 MiB/16-way LLC and 64-byte lines:

1. Fresh ordinary GRASP.
2. Fresh intact favorable `POPT:UNCHARGED`.
3. `DIAG_GRASP_REFERENCE_FLAT`.
4. `DIAG_GRASP_REFERENCE_FULL`.

There is no prefetch, graph/order/geometry search or BFS follow-on.
The two diagnostic launch names and strings have equal lengths.
One simulator thread, a 2 GiB process-tree RSS limit, a 512 MiB algorithm
workspace and a thirty-minute timeout apply to each cell.

The preregistered information gate is
`(flat_kernel_transfers - full_kernel_transfers) / flat_kernel_transfers
>= 0.02`. FULL must also use strictly fewer kernel transfers than fresh
ordinary GRASP before a compact producer under this consumer is justified.
Compare intact P-OPT separately, without relabeling an ablation as P-OPT.
Report all setup/kernel/property/other/writeback totals, not only the gate.
SpMV's registered-property counter includes `x` and `y`, not `x` alone.

```bash
ulimit -c 0
make -j1 bench/bin_sim/algorithms
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_grasp_reference_cache \
  --run-dir results/ecg_experiments/runs/grasp_reference_case \
  --no-build --no-resume
```

Use a clean committed tree and retain the raw receipts, source/binary
fingerprints and completion markers. This is a separate four-run budget;
the preceding twelve-run campaign remains closed. Failure rejects this
reference-consumer direction, not all possible graph-aware policies.
Success would justify freezing one cost/delivery-constrained design,
not automatically implementing A or claiming competitiveness. No
additional full-graph campaign is authorized by either outcome.
