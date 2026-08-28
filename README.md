<p align="center"><img src="wiki/assets/logo.png" alt="ECG graph logo" width="140"></p>

# ECG Next

**ReusePlan, ReuseBind, and FlowThrough cache mechanisms for irregular graph
analytics**

ECG Next derives line-level reuse guidance from the adjacency traversal used by
each graph kernel. **ReusePlan** is the offline edge-aligned metadata record,
**ReuseBind** is the Request extension attached to the consuming property load,
and **FlowThrough** is the no-allocate decision for eligible structural misses.
These mechanisms do not change graph results or property values.

For an out-neighbor traversal, property `p[v]` is read once for each source
vertex in `N_in(v)`; its property-request count is therefore `d_in(v)`. For an
in-neighbor (pull) traversal, `p[v]` is read once for each destination vertex in
`N_out(v)`; its property-request count is therefore `d_out(v)`.

## Architecture overview

![System overview showing ECG offline record construction, the two-load RISC-V instruction path, LLC replacement state, and simulator evidence boundaries](fig/wiki/home/home-f01-system-overview.svg)

The architecture keeps four boundaries explicit:

1. **Offline construction:** a kernel-direction-aware graph pass emits immutable
   ReusePlan records in canonical CSR order.
2. **Two dynamic loads:** a record load produces an explicit register operand;
   a dependent property load issues the consuming memory Request.
3. **Request-bound state:** gem5 O3 attaches destination, tier, two epochs,
   current epoch, context, and sequence to that property Request through
   ReuseBind.
4. **LLC policy:** RRIP-first forms the eligible set, selects the oldest
   eligible non-property line when one exists, and otherwise selects the
   property line with the largest valid reuse distance.

FlowThrough preserves translation, private-cache behavior, LLC hits, miss
service, and response. It changes only whether an eligible returning structural
miss receives fill allocation in the LLC. If a coalesced MSHR also contains an
allocating target, the shared fill still allocates.

The request-bound `ECG_FLOWTHROUGH` flag is the ReusePlan design mechanism.
For controlled comparisons, `STRUCTURAL_FLOWTHROUGH` / `--flowthrough all`
applies the same no-allocate rule to the active structural array used by each
policy, either CSR or the packed ReusePlan array.

## Evidence boundary

| Backend | Role |
|---|---|
| **gem5 O3** | architectural timing evidence and dynamic Request binding |
| **cache_sim** | functional cache behavior and off-chip traffic evidence |
| **Sniper** | matched-work modeled cache and traffic evidence |
| **RTL models** | synthesizable metadata and physical-cost components |

Only gem5 O3 time is architectural speedup evidence. Analytic P-OPT time is an
optimistic lower bound because target-time lookup latency, matrix-stream
latency, bandwidth, queueing, and contention are omitted.

Sniper includes per-edge markers in its indexed path and computed fused-sideband
diagnostics; neither path is architectural ReuseBind speedup evidence.

The gem5 mechanism is an experimental RISC-V custom-0 implementation. It is
not a ratified RISC-V extension or an upstream gem5 feature.

## Documentation

- [ReusePlan and FlowThrough](wiki/ReusePlan-FlowThrough.md)
- [RISC-V instruction path](wiki/RISC-V-Instruction-Path.md)
- [End-to-end property Request example](wiki/Property-to-Cache-Walkthrough.md)
- [Evaluation methodology](wiki/Evaluation-Methodology.md)
- [Related work](wiki/Related-Work.md)
- [Build and reproduction](wiki/Reproduction.md)
- [Repository hygiene](wiki/Repository-Hygiene.md)

## Repository layout

| Path | Purpose |
|---|---|
| `bench/include/` | shared policy, metadata, ISA, and simulator integration |
| `bench/src_sim/` | functional cache-simulator graph kernels |
| `bench/src_gem5/` | gem5 graph kernels |
| `bench/src_sniper/` | Sniper graph workload |
| `bench/src_rtl/` | synthesizable ReusePlan cost models |
| `scripts/experiments/ecg/` | experiment runners and fail-closed gates |
| `scripts/docs/` and `fig/` | deterministic public figure generation |
| `wiki/` | architecture, methodology, and reproduction guides |

Generated experiment output remains under `results/` and is not tracked.
Performance tables are published only after the frozen evaluation completes.
