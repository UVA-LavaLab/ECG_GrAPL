# Observation-only potential-window diagnostics

The window observer evaluates information availability at actual GRASP
evictions without changing the active policy. It is not an implementation
of window replacement, a native interface, or a performance result.
It is restricted to unweighted, sorted top-down BFS with one U32 depth
array and a 64-byte-aligned base.

## Fixed graph-only profile

Source rows are grouped into power-of-two cohorts:
`C = next_power_of_two(max(8, ceil(vertices/256)))`, with `Q=C/8` rows
per endpoint bin. Patents uses C=16,384, Q=2,048 and 1,844 bins per pass.

One ten-bit logical token describes the nearest future occupied cohort:
known bit, three-bit exact cohort gap, three-bit distinct-row strength
(1..7 or 8+), and a three-bit rounded exclusive endpoint. Gap overflow
becomes UNKNOWN, never a nearer fabricated cohort. Same-row, same-line
duplicates reuse the token frozen before the row is inserted into the
reverse summary. Counts refer to structural potential, not actual frontiers.

The observer computes these tokens from the immutable graph, without
executing BFS first. Its diagnostic side table has two bytes per edge,
with eight bytes of reverse scratch per target line. This is not a
deployed extra metadata stream: its reads and construction are explicitly
excluded from active cache accounting to preserve GRASP's original history.
No current FINITE/WRAP token or native record is reinterpreted as a window.

## Runtime observation and timing limits

The observer receives the actual pass, sorted source row and designated
depth access. The source/index association is checked. A const cache
callback observes the actual victim and final RRIP candidate set after
GRASP has aged it exactly once. The callback cannot return a replacement
victim or modify any cache-line state.

Two cache-sized shadow views are retained:

| View | Meaning |
|---|---|
| Immediate | Window available immediately after an actual designated access, if its line is resident in the LLC |
| Delivered | Eight access-step update delay, one output per step, sixteen bounded entries, no coalescing; newer observations hide pending/older information |

Ordinary depth reads and writes invalidate both views. Filling or evicting
a real line updates shadow residency, so metadata cannot survive a lost
residency or apply to a different line occupying the same way. Expiry and
pass changes are checked lazily; unknown, invalidated, pending, expired
and old-pass states are reported separately.

Source-bin markers drain the shadow queue and add eight shadow service
steps; begin/close and jumps are counted. They never stall or change the
real cache model. Marker traffic is reported as 48 logical control bytes
per exchange, not DRAM traffic. This is a timing diagnostic, not a
claim to model native retirement, overlap, interconnect delay or CPU cycles.

## Sampling and subsequent-read outcomes

Sampling is fixed at every 256th valid-line LLC eviction during graph
passes, not the first few evictions. It never consults future accesses.
The observer asks whether compatible live windows would distinguish the
actual base victim from another property way already at GRASP's eviction
RRPV. Farther cohorts and then weaker structural strength are poorer
retention candidates; ties and unrankable base victims retain GRASP's choice.

For a delivered-view disagreement, the observer records only that pair of
line addresses. There are at most 256 outstanding pairs. The first later
read to either line is observed during normal execution, with an explicit
131,072-memory-request horizon and pass-end censoring. Capacity drops,
censoring and unresolved outcomes are not counted as victories.
Private-cache reads, LLC-reaching reads and memory misses are distinguished.
Writes still invalidate window state, but do not resolve a read-order pair.

These outcomes are not counterfactual miss savings. Changing a victim would
change later cache history, and a later read can hit a private cache. No
pair state feeds back into the policy, token generator or parameter choice.
There is no full address trace or future-derived mask artifact.

## Guarded execution and noninterference

`--window-observer control` records real demand/eviction digests without
window tables. `--window-observer window` adds the passive shadow analysis.
The default is `off`; incompatible algorithms, policies, records, weighted
inputs and native backends are rejected rather than silently downgraded.

The `ecg_window_observer` profile runs one full-Patents/source-0/8-MiB
condition with these two roles serially, under process-tree memory and
wall limits. It requires identical algorithm output/work, complete
cache counters, setup/kernel snapshots and demand/victim digests.
Input and binary hashes must also match. A changed placement or history
fails that gate; equal aggregate miss counts alone are insufficient.

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_window_observer \
  --run-dir results/ecg_experiments/runs/window_observer_case \
  --no-build --no-resume
```

Both roles declare `diagnostic_only=true`,
`measurement_scope=observation-only-unchanged-grasp` and distinct `OBS`
policy labels. Diagnostic storage is separately bounded at 128 MiB
by default, inside the algorithm's workspace reservation. The raw report
contains aggregate and per-pass sample counts, state histograms, bounded
queue/trial accounting and immutable-stream digests.

Passing noninterference establishes trustworthy observation of this one
baseline history. It does not authorize enabling the window policy,
choosing parameters from held-out results, or claiming superiority over
[the existing GRASP/P-OPT controls](Traversal-Metadata-Taxonomy#matched-grasp-and-p-opt-comparison).
