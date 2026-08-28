# Property-Load Request: End-to-End Example

This page derives one fixture-backed adjacency entry from graph structure,
then traces its ReusePlan through instruction execution, ReuseBind attachment,
cache access, and a later LLC victim-selection decision.

## 1. Figure fixture

`fig/ecg-figure-fixture.json` uses a nine-node, 17-edge weighted
topology with fixture IDs `0..8`. A declared fixture-to-internal map places
those vertices in a 32-entry property space so cache-line grouping remains
visible.
Here, the **outer vertex** is the vertex of the outer loop that owns the
current adjacency row. The selected source edge is `4 -> 7`, mapped to internal
source and destination vertices `8 -> 18`.

| Item | Derived value |
|---|---|
| source vertices in `N_in(18)` | internal IDs `8, 11, 15, 20` |
| in-degree of destination vertex 18 | `d_in(18) = 4` |
| stable in-degree-rank tier | `1` (hot) |
| subsequent property-block access epochs after outer vertex 8 | `11, 15` |
| property base | `0x8000_0000` |
| property address | `0x8000_0000 + 18*4 = 0x8000_0048` |
| 64-byte property line | `0x8000_0040`, vertices 16–31 |
| compact width | `5 + 2 + 2*5 = 17` bits |

### Figure 1 — From adjacency entry 4 -> 7 to LLC line 0x80000040

![End-to-end drawing of weighted graph adjacency entry 4 to 7, internal destination vertex 18, compact ReusePlan fields, the RISC-V instruction pair, separate record and property MSHRs, the LLC line schedule, and in-order retirement](../fig/wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg)

**Figure 1.** For the out-neighbor example, source vertex 4 processes adjacency
entry `(4,7)` and reads property value `p[7]`; the fixture-to-internal mapping
changes this to outer vertex 8 and destination vertex 18. Callouts `A` through
`E` identify the corresponding ReusePlan, renamed operand, property Request,
MSHR state, and LLC line state.
The record and property Requests use separate address lanes and therefore
separate MSHRs; only same-property-block targets can merge ReuseBind state.

At outer vertex 8, whose quantized current epoch is 8:

```text
d1 = (11 + 32 - 8) mod 32 = 3
d2 = (15 + 32 - 8) mod 32 = 7
nearest = 3
```

The current epoch used by a later victim decision may differ from the epoch at
fill or refresh; the line stores absolute future epochs, not a permanently
fixed distance.

## 2. Instruction and cache sequence

1. The offline builder emits the edge-aligned logical record
   `(destination=18, tier=1, epoch1=11, epoch2=15)`.
2. `ecg.flow.load.compact` reads the record and returns a canonical ReusePlan
   in an integer register. Its Request may use FlowThrough.
3. `ecg.bind.load.u32` reads the computed address `0x8000_0048` and names the
   ReusePlan register as `rs2`.
4. The LSQ attaches the typed ReuseBind extension to that property Request.
5. An LLC hit or fill accepts it only when context and destination line match.
6. The property value writes back and retires normally.
7. A later RRIP-first victim selection uses the stamp only if the line is
   max-RRPV eligible and no eligible structural line wins first.

FlowThrough changes record fill allocation. ReuseBind changes advisory
property-line metadata. Neither changes the property value.

The Request extension also carries epoch count, current epoch, context ID,
dynamic sequence, and a conflict bit. The two-epoch ReusePlan path uses the
tier and both epochs. Legacy compatibility fields remain in the
implementation for older single-epoch modes.

## 3. State placement

### Figure 2 — ReusePlan state placement across software, core, and LLC

![Architecture containment map placing immutable records, ECG control CSRs, renamed operands, load-queue state, typed Requests, MSHR merge state, and line-local LLC metadata](../fig/wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg)

**Figure 2.** Containment denotes storage; the retained arrow denotes the
property-response transfer and its per-hit/fill cadence.

| State | Owner and lifetime |
|---|---|
| record/sidecar bytes | immutable memory input, validated before ROI |
| record-format CSR | architectural context, configured before ROI |
| current-epoch CSR | architectural context, changed at epoch boundaries |
| context CSR | architectural execution identity |
| ReusePlan physical register | record completion through dependent property issue |
| ReuseBind Request extension | one dynamic property Request and compatible MSHR merge |
| MSHR ECG state | active miss target list |
| line tier/epochs/context | accepted LLC line until refresh, invalidation, or eviction |
| property value | ordinary destination-register and ROB lifetime |

## 4. Backend scope

The shared `selectVictim` function makes the final victim decision from native
per-way state, but the backends are not cycle-identical:

- **gem5 O3** executes the experimental RISC-V path and provides architectural
  timing evidence.
- **cache_sim** models declared graph-data accesses, replacement, prefetching,
  and traffic without cycles or instructions.
- **Sniper** provides matched-work modeled cache/traffic evidence. The default
  indexed ReusePlan path uses per-edge markers; computed fused sideband rows
  are diagnostic and fail closed when one source-vertex/cache-line combination
  requires inconsistent hints.

Absolute miss rates are not compared across simulators.

## 5. Implementation sources

| Layer | Source |
|---|---|
| fixture and generated figures | `fig/ecg-figure-fixture.json`, `scripts/docs/generate_ecg_figures.py` |
| construction and formats | `bench/include/ecg_reuse_plan_builder.h` |
| distance and victim selection | `bench/include/ecg_victim_policy.h` |
| gem5 Request/MSHR path | `bench/include/gem5_sim/overlays/mem/cache/` |
| gem5 replacement adapter | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_rp.cc` |
| Sniper replacement adapter | `bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/cache_set_ecg.cc` |
