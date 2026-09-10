# BFS phase and data-role attribution

The costed window experiment leaves a substantial P-OPT/GRASP gap:
1,839,367 of P-OPT's 2,434,547 fewer kernel demand misses (75.55%) are
outside the depth array. Aggregate property counters do not identify
which auxiliary phase or structure causes that difference.

This experiment tests a specific explanation without changing masks,
graph order, source or BFS traversal. The existing P-OPT control uses
its graph-derived matrix during graph passes and LRU when the graph
clock is unavailable outside them. Ordinary GRASP continues its
region-based policy during frontier sorting.

## Exact phase-policy variant

`--grasp-scope graph-passes` is an explicit CSR-BFS GRASP variant:

| Interval | Victim selection |
|---|---|
| Setup before first completed property binding | Original GRASP |
| Graph pass, including discovery and frontier appends | Original GRASP |
| Outside graph passes, including frontier sorting | LRU |

The default is `--grasp-scope all`. Both variants maintain the same
recency and GRASP insertion/hit-promotion metadata on every access;
only victim selection is switched. This preserves useful history
when GRASP resumes, without a bulk reset or an uncharged cache walk.
Data, dirty state and tags are never flushed at a phase boundary.

Initial configuration and each begin/close transition are charged in
the reported control ledger at sixteen functional steps and 48
logical control bytes. These are not measured CPU cycles or DRAM
traffic. Native state/port costs and timing remain unqualified.

This is a common-baseline optimization candidate, not an ECG mask
contribution. P-OPT's encoding, region coverage and policy remain
unchanged in the comparison.

## Attribution contract

`--bfs-traffic-phases on` records aggregate counters without changing
cache decisions or adding modeled data requests. It is cache-only,
limited to CSR top-down BFS, and cannot be combined with a record
model or window observer.

| Phase | Included work |
|---|---|
| Setup | Model-visible graph validation, initialization and policy preprocessing |
| Edge probe | Current-frontier reads, CSR offsets/IDs, depth probes and discovery writes |
| Frontier build | Stores appending discovered vertices to the next frontier |
| Frontier sort | In-place sorting of the next frontier, outside the graph pass |
| Between passes | Any remaining modeled accesses; required to be empty for this kernel |

Access roles are CSR index, CSR edge, weight, depth, frontier/worklist
and construction. BFS does not consume weights. Sorting and frontier
roles follow actual operations, not physical allocation names that
change meaning when frontier buffers are swapped.

Miss/hit counters belong to the requested data. Writebacks are
attributed to the **triggering access**, not to the owner of the
evicted dirty line. Phase writeback totals are valid, but a depth-triggered
writeback must not be described as necessarily writing back depth data.
Every role sum closes to its phase; setup/kernel sums close to the
unchanged whole-run counters.

Serialized input loading and host-runner hashing are outside these
cache-model counters. Instrumented host duration is not native speedup.

## Matched run

The `ecg_bfs_phase_cache` profile runs three serial full-Patents/source-0
cells at an 8 MiB/16-way LLC, 32 KiB L1D and 256 KiB L2, both eight-way:
ordinary GRASP, unchanged favorable full-capacity P-OPT, and phase-scoped
GRASP. Both GRASP arms have the same setup policy. P-OPT retains its
declared, separately counted matrix construction and free runtime lookup.

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_bfs_phase_cache \
  --run-dir results/ecg_experiments/runs/bfs_phase_case \
  --no-build --no-resume
```

Source/binary fingerprints, identical algorithm output/work, phase
closure, control accounting and equal GRASP setup counters are required.
If auxiliary-phase policy explains the gap, the stronger baseline must
also be available in future ECG comparisons. If it does not, the
hypothesis must be rejected rather than relabeled as a mask improvement.
