# Reusable SpMV query interface

The existing cache-only algorithm executable and ROI runner accept
`--queries N` (1 through 64) for unweighted CSR SpMV baselines.
This is the reusable-query foundation, **not a PASS_RANK implementation
or a new performance study**. No full-graph campaign is added.

`--repeat` remains the number of sweeps inside EACH query. Every query
executes the real shared kernel with fresh query-local `x` and `y` arrays:
`x[v]=float(v+1)` is initialized again, and each sweep writes all of `y`.
The inputs are independently initialized but have the same fixed values;
this interface does not yet accept a stream of different vectors.

The loaded CSR owner remains alive for the entire synchronous batch.
No extra edge buffer is allocated. The API borrows that immutable view
only for the call; it is not a background graph-lease service. Data-cache
contents, dirty state and normal replacement history persist between
queries without flushing or counter resets.

## Actual P-OPT reuse

`PreparedSpmvMatrix` owns the real canonical FULL matrix across query-local
`AlgorithmBackend` instances. The first query constructs it through the
existing counted builder. Later queries bind their new aligned F32 `x`
region to the same matrix without reconstructing it. Streamed `y` remains
outside P-OPT matrix priority. Source/pass progress and context are fresh
for each query; each completed backend detaches before its property storage
is released.

Graph identity, property-bank mapping and workspace/auxiliary limits are
checked on reuse. Overlapping borrows are rejected. An interrupted matrix
query detaches its context and poisons that preparation; it cannot silently
resume. A failed batch aborts rather than continuing with partial state.
The resource plan reserves another 512 bytes for shared preparation
ownership, separately from the matrix and existing builder scratch.
This is a conservative host reservation, not a physical hardware estimate.

GRASP, LRU and SRRIP retain their normal policies and do not acquire a
matrix. P-OPT retains its existing favorable full-data-capacity and free
runtime-matrix-traffic convention, not an equal-area or native-time claim.

## Cost and provenance receipts

For `N>1`, the executable emits `ecg.spmv-queries.v1`, containing:

- Each complete query workload/result and query-local LLC/off-chip metrics.
- Per-query setup/kernel counters and the measured cumulative prefix.
- One-time `graph_preparation` traffic, `query_setup` totals and
  `query_kernel` totals, which sum to actual batch traffic.
- Matrix construction count, storage, digest and per-query reuse status.

Matrix traffic is attributed by snapshots around actual construction
inside the first query's setup. It is not moved earlier, repeated, or
subtracted from the cost of query 1. Every query's ordinary graph validation
and property initialization remain in query setup. Serialized input
loading is still excluded, exactly as in existing single-query controls.
`graph_preparation` does not claim to measure all graph-file/IO preparation.
The API requires a fresh hierarchy on entry; only intra-batch cache state
is preserved.

Per-query metrics cover LLC/off-chip deltas, not per-query L1/L2 statistics.
The top-level metrics retain the hierarchy's actual cumulative counters.
Host duration includes instrumented simulator work and is not native speed.

The ROI runner emits one aggregate row per policy with an explicit suffix,
for example `POPT_UNCHARGED_QUERIES3`. Work counts and traffic are summed;
result/work digests describe each identical query and are verified for
every entry. Row labels, configuration hashes and completion expectations
include the query count. Receipts reject repeated preparation, changed
matrix identity, missing/reordered queries and non-closing costs.

## Bounded reproduction

On an already prepared small fixture:

```bash
ulimit -c 0
make -j1 bench/bin_sim/algorithms
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --current-algorithms --benchmark spmv \
  --options "--graph results/graphs/ecg-algorithm-equivalence/pressure512.sg --repeat 2 --queries 3" \
  --policies LRU GRASP_PAPER POPT:UNCHARGED --ecg-equivalence \
  --l1d-size 128B --l1d-ways 2 --l2-size 256B --l2-ways 2 \
  --l3-sizes 1024B --l3-ways 2 --cache-record-rss-mib 512 \
  --no-build --out-dir results/ecg_prepared_spmv_fixture
```

The default and explicit `--queries 1` retain the original single-query
schema and behavior. Multi-query records, weighted inputs, other kernels,
reference diagnostics, phase/observer controls and native backends are
not admitted. The [earlier tiny ownership qualifier](GRASP-Reference-Consumer#bounded-prepared-graph-ownership-follow-on)
remains useful for canonical tagged storage, but tagged carriers and
PASS_RANK prediction/publication are not enabled by this raw-CSR interface.

No full-graph query count is selected here, and both prior measurement
budgets remain closed. Future evaluation must predeclare the workload,
include the first-query prefix and full cumulative costs, and compare
intact baselines without converting a storage/reuse feature into a
kernel-quality claim.
