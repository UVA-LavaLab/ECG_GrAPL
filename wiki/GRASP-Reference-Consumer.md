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

## Measured Patents result

The four cells completed at implementation commit `a5b58c82` in
`results/ecg_experiments/runs/grasp_reference_patents_a5b58c82`.
All four workload/result receipts agree. FULL and FLAT have identical
complete setup counters and the same matrix as intact P-OPT:
60,396,288 bytes, 235,923 lines, digest `3683564403557561320`,
and 328,708,247 / 191,849,776 construction read/write bytes.

| Policy | Property misses (`x` + `y`) | Other misses | Writebacks | Kernel transfers |
|---|---:|---:|---:|---:|
| Ordinary GRASP | 9,847,988 | 5,073,432 | 535,510 | 15,456,930 |
| Intact favorable P-OPT | 6,855,000 | 5,073,432 | 469,984 | 12,398,416 |
| Reference FLAT | 9,955,163 | 5,073,432 | 470,370 | 15,498,965 |
| Reference FULL | 9,014,195 | 5,073,432 | 469,374 | 14,557,001 |

FULL saves **941,964 kernel transfers (6.08%) versus FLAT**:
940,968 registered-property misses and 996 writebacks. Other misses are
unchanged. It also saves **899,929 transfers (5.82%) versus ordinary
GRASP**. Both preregistered development requirements pass.

However, FULL still uses **2,158,585 more kernel transfers (17.41%) than
intact P-OPT**, despite ideal rank availability and no RRPV candidate
restriction. It closes only **29.42% of the ordinary-GRASP-to-P-OPT
kernel gap**. The experiment therefore does not establish that replacing
the matrix with a compact encoding is sufficient to compete with P-OPT
on kernel traffic.

The diagnostic was active: FULL made **5,388,956 victim refinements**,
including 4,502,489 selecting a way with post-base-aging RRPV below 7.
FLAT made zero refinements, as required. FULL/FLAT performed
117,675,386 / 145,813,708 real matrix lookups. These are observed
mechanism counts, not independent miss-savings attributions.

FLAT's kernel differs slightly from ordinary GRASP even though it never
refines a victim: FLAT still prepares the full matrix and enters the
kernel with that preparation's cache state. It is not interchangeable
with the no-matrix GRASP baseline. FULL versus FLAT is the clean
information comparison; neither is intact P-OPT.

### Setup and the tighter cost target

| Policy | Setup transfers | Setup + kernel transfers |
|---|---:|---:|
| Ordinary GRASP | 2,877,491 | 18,334,421 |
| Intact favorable P-OPT | 9,293,507 | 21,691,923 |
| Reference FLAT | 9,373,437 | 24,872,402 |
| Reference FULL | 9,373,437 | 23,930,438 |

FULL loses setup-inclusive traffic by **30.52% versus GRASP** and
**10.32% versus P-OPT**, even with free runtime lookup. Its matrix
construction uses the same byte operations as P-OPT, but retains its
declared GRASP setup rather than P-OPT's LRU setup.

At FULL's *measured* kernel quality, a candidate would need total setup
below **3,777,420 transfers** to beat the cold GRASP workload, or less
than **899,929 extra setup transfers** above ordinary GRASP. The earlier
3,058,514-transfer incremental budget assumed matching P-OPT's kernel;
this diagnostic does not attain that quality. These are conditional
traffic targets, not lower bounds or measured amortized/native results.

### Decision and provenance

**Useful graph information helps this fixed GRASP-based consumer, but
the combination is not competitive with P-OPT or on cold total traffic.**
The positive gate justifies a concrete cost/delivery-constrained design
discussion. It does not approve a large producer, claim that information
availability is the only remaining problem, or establish that compression
alone closes the residual quality gap.

Do not infer that any more compact or different predictor is mathematically
bounded by this one diagnostic. Conversely, do not assume it improves
past the reference result without a specific hypothesis. The comparison
does not isolate P-OPT's individual insertion, region-priority and
rank-before-aging contributions.

The audit replays three job markers, all four raw receipts, and 111 input
fingerprints across 34 distinct files. Its own completion marker binds the
raw JSON/log/watchdog files and the unchanged original CSV. No additional
full-graph executions or parameter choices were used. The new four-run
budget is closed; the previous twelve-run campaign remains closed, and
WINDOW/B stay negative controls. A is still unimplemented.

| Artifact | SHA256 |
|---|---|
| `combined_roi_matrix.csv` | `ce26b81e7b15eb2215d99cc4564cf008fac0bee413b42f033e701bab2fd6992c` |
| Shared executable | `6e9d9aa1c442d96a82167a2c2a9ad49806cd43d9a81bd808b93de2abb8cc991b` |
| `audit/audit.complete.json` | `b545a7ae6267d593dd58f13e97ffca8a50eccaa95b259013fe0af6c8599ac805` |

## Bounded prepared-graph ownership follow-on

`scripts/test/test_ecg_prepared_spmv.py` qualifies a **test-only** ownership
prototype on a 64-vertex, six-adjacency fixture. It moves the real CSR
owner, tags its existing ID32 words in place, and executes the unchanged
shared SpMV kernel twice with fresh `x` initialization. The tagged view
decodes IDs without a second edge buffer; raw signed decoding still rejects
the deliberately high-bit-set words. The internal `encoded_id_bits` view
is restricted to unweighted ID32 CSR SpMV, without existing record modes.
No serialized-format or CLI admission is added.

The fixture also retains one actual FULL matrix across two query bindings,
checking stable storage/digest and canonical reference lookups. Borrowed
ownership survives release of the outer handle; overlap, abandoned-query
reuse, unsafe closure and clock overflow are rejected. Constructed
old-FINITE/terminal-range and PENDING-order cases check stale-query
rejection. These are lifetime predicates and existing update primitives,
not an integrated asynchronous PASS_RANK receiver.

Preparation bytes and per-query shared-kernel bytes are separate. Each
query observes 3,208 read bytes and 768 write bytes, including 64 fresh
input stores and the existing per-query graph validation. Fixture loading,
verification reads and matrix binding probes are outside those counters.
They are **logical byte receipts, not cache traffic or native timings**.

The tags are opaque ownership fixtures, not generated PASS_RANK hints.
Neither the production batch runner, real PASS_RANK producer nor its
rank-first victim policy is implemented by this qualification. The existing
performance results and both closed full-graph budgets remain unchanged.
