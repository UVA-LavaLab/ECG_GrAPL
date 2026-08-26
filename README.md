<p align="center"><img src="wiki/assets/logo.png" alt="ECG graph logo" width="140"></p>

# ECG Next

**ReusePlan and FlowThrough cache architecture for irregular graph analytics**

ECG Next derives line-level reuse guidance from the adjacency traversal used by
each graph kernel, stores it in an edge-aligned ReusePlan, binds it to the exact
property request, and uses it to refine last-level-cache replacement.
FlowThrough is a separate placement mechanism for low-reuse structural
records.

For an outgoing-neighbor traversal, accesses to property `p[v]` originate from
the in-neighbors of `v`, so the property-access count is the in-degree
`d_in(v)`. For an incoming-neighbor traversal, they originate from the
out-neighbors of `v`, so the count is the out-degree `d_out(v)`.

## ECG Next: offline guidance to request-bound LLC state

![System overview showing ECG offline record construction, the two-load RISC-V request path, LLC replacement state, and simulator evidence boundaries](fig/wiki/home/home-f01-system-overview.svg)

The architecture keeps four boundaries explicit:

1. **Offline construction:** a kernel-direction-aware graph pass emits immutable
   ReusePlan records in canonical CSR order.
2. **Two dynamic loads:** a record load produces an explicit register operand;
   a dependent property load issues the architectural data request.
3. **Request-bound state:** gem5 O3 attaches destination, tier, two epochs,
   current epoch, context, and sequence to that exact property Request.
4. **LLC policy:** RRIP first forms the eligible set; an old structural line is
   preferred, otherwise the farthest stamped property line is selected.

FlowThrough preserves translation, private-cache behavior, LLC hits, miss
service, and response. It changes only whether an eligible returning miss
allocates in the LLC. If a coalesced MSHR also contains an allocating target,
the shared fill still allocates.

The request-bound `ECG_FLOWTHROUGH` flag is the ReusePlan design mechanism.
The separate `STRUCTURAL_FLOWTHROUGH` / `--flowthrough all` switch is a
policy-independent fairness control for each workload's active structural
carrier.

## Evidence boundary

| Backend | Role |
|---|---|
| **gem5 O3** | architectural timing and exact dynamic Request binding |
| **cache_sim** | functional victim-policy behavior and off-chip traffic |
| **Sniper** | modeled equal-work cache and traffic direction |
| **RTL models** | synthesizable metadata and physical-cost components |

Only gem5 O3 time is architectural speedup evidence. Analytic P-OPT time is an
optimistic bound because target-time matrix latency, bandwidth, and queueing
are not charged.

Sniper includes exact per-edge marker and computed fused-sideband diagnostics;
neither path is architectural ReuseBind speedup evidence.

The gem5 mechanism is an experimental RISC-V custom-0 implementation. It is
not a ratified RISC-V extension or a claim of fabricated processor support.

## Documentation

- [ReusePlan and FlowThrough](wiki/ReusePlan-FlowThrough.md)
- [RISC-V instruction path](wiki/RISC-V-Instruction-Path.md)
- [End-to-end property-access example](wiki/Property-to-Cache-Walkthrough.md)
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
