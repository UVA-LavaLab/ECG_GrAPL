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

## Patents result

`results/ecg_experiments/runs/bfs_phases_patents_aaba1978` contains the
three completed cells at source `aaba1978`. All roles return depth digest
`3865ba0b18ab9fe2`, execute 438,095,550 kernel requests and complete the
same seventeen BFS levels. Both GRASP arms have identical setup counters
and 2,877,493 setup transfers.

| Role | Kernel memory misses | Kernel transfers | Setup + kernel transfers |
|---|---:|---:|---:|
| GRASP throughout | 13,052,296 | 13,416,757 | 16,294,250 |
| GRASP in passes, LRU outside | 11,027,736 | 11,391,656 | 14,269,149 |
| Favorable full-capacity P-OPT | 10,617,749 | 10,852,949 | 20,146,460 |

Phase scoping reduces misses by 15.51%, kernel traffic by 15.09%, and
setup-inclusive modeled traffic by 12.43% versus ordinary GRASP.
It closes 78.99% of GRASP's kernel-traffic gap to P-OPT. It still uses
4.96% more kernel traffic than P-OPT; the 29.17% lower setup-inclusive
traffic is a single-query result with P-OPT construction counted, not
a claim about amortized multi-query use or CPU performance.

### The missing locality was mostly in frontier sorting

| Phase | GRASP misses | Scoped GRASP misses | P-OPT misses |
|---|---:|---:|---:|
| Edge probe | 10,716,023 | 10,559,487 | 10,148,424 |
| Frontier build | 234,574 | 234,452 | 234,909 |
| Frontier sort | 2,101,699 | 233,797 | 234,416 |

Sorting performs the same 353,228,007 frontier-array requests in every
role, with no depth-array requests. Scoped GRASP reduces its sorting
misses by 88.88%, nearly matching P-OPT's outside-pass LRU behavior.
Sorting accounts for 76.70% of the original P-OPT/GRASP demand-miss gap.
The experiment therefore supports the auxiliary-phase policy hypothesis
on this graph/source: graph-property priority was persisting through a
phase that was using only frontier data.

State changes also affect later graph passes; the result is not merely
the sum of isolated phase simulations. No cache contents are reset.
For example, scoped GRASP's depth misses decrease from 4,881,206 to
4,721,503 even though its in-pass policy remains GRASP.

The residual miss gap to P-OPT is now 435,477 depth misses, offset by
25,490 fewer other-data misses under scoped GRASP. P-OPT additionally
has 128,720 fewer kernel writebacks. This is a much narrower remaining
problem than the original aggregate non-depth gap.

### Costs and interpretation

The scoped run reports one configuration and 34 begin/close transitions:
560 functional control steps and 1,680 logical control bytes.
The recency/RRPV state is maintained throughout, and no data/tag/metadata
walk is performed at transitions. These are explicit cache-model
control costs, not measured native instruction, latency or area costs.

Writebacks move between phases: sorting triggers 61,880 under scoped
GRASP versus 33,313 under ordinary GRASP, while the overall kernel
writeback count falls slightly, from 364,461 to 363,920. Phase-local
writeback movement must not be mistaken for the total traffic outcome.

**Decision:** retain phase-scoped GRASP as a stronger, explicitly named
baseline for this workload. This is not an ECG mask win. Future ECG
comparisons should receive the same applicable phase treatment and
compare against this stronger baseline and unchanged P-OPT. The
window candidate remains unpromoted; its benefit under the stronger
baseline is not established by this experiment.
