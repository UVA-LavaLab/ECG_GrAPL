# RISC-V Instruction Path

ECG implements an experimental custom-0 RISC-V family in gem5. Matching guest
kernels emit the encodings with `.insn`. This is a research ISA extension, not
a ratified RISC-V extension, an upstream gem5 feature, or fabricated hardware.

## 1. Instruction roles

### Figure 1 — Experimental RISC-V instruction roles and operand contracts

![Three-band instruction-role plate covering ECG control CSRs, record-load variants, computed and indexed property loads, and the explicit register dependency](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg)

**Figure 1.** Record acquisition and property access remain two dynamic loads.

| Family | Inputs | Result | Request effect |
|---|---|---|---|
| `ecg.plan.load` | general record address | canonical 64-bit ReusePlan in integer `rd` | ordinary cacheable placement |
| `ecg_plan_weighted_load` | sidecar address in `rs1`, destination in `rs2` | 32-bit weighted sidecar in `rd` | ordinary cacheable placement |
| `ecg.flow.load` | general record address | canonical 64-bit ReusePlan in integer `rd` | sets record FlowThrough |
| `ecg.flow.load.compact` | compact record address | canonical widened ReusePlan in integer `rd` | sets record FlowThrough |
| `ecg_flow_weighted_load` | weighted sidecar address | 32-bit weighted sidecar in `rd` | sets record FlowThrough |
| `ecg.bind.load.*` | computed property address in `rs1`, plan in `rs2` | typed property value | attaches ReuseBind |
| `ecg.bind.iload.*` | property base in `rs1`, plan/destination in `rs2` | typed property value | indexed EA plus ReuseBind |

There is no compact Plan-load encoding; compact unweighted acquisition is
FlowThrough-only. The record-format CSR supplies compact `id_bits` and
`epoch_bits`. The record-load execution helper consumes that format state and
widens the returned compact value into the canonical
destination/tier/two-epoch layout; frontend Decode only identifies the
custom-0 role.
The current-epoch CSR changes only at quantized traversal boundaries. A
nonzero context CSR distinguishes overlapping executions.

The request flags are distinct: `ECG_FLOWTHROUGH` is emitted by the
request-bound record-load family, while `STRUCTURAL_FLOWTHROUGH` is the
policy-independent fairness control applied to a validated structural range.
Neither flag is attached to the governed property Request.

## 2. Out-of-order request path

### Figure 2 — Concrete ReusePlan instruction pair in the gem5 O3 pipeline

![Architecture schematic tracing adjacency entry 4 to 7, mapped internally to 8 to 18, through the FlowThrough record instruction, gem5 Fetch Decode Rename IEW Commit pipeline, LSQ Request, caches, LLC line, writeback, and retirement](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg)

**Figure 2.** Checked adjacency entry `4 -> 7` maps to internal entry
`8 -> 18` and supplies the fixture-derived operands and addresses.

The top-level grouping follows gem5 O3CPU's documented **Fetch, Decode,
Rename, IEW, Commit** pipeline. The IEW block is expanded into the issue queue,
physical-register read, AGU, and LSQ Request path used by a load. This is the
gem5 simulator architecture, not a claim about a fabricated ECG core.

The six numbered stages are record load, rename/dependency, property address
generation, LSQ Request construction, cache/MSHR/LLC traversal, and
completion/retirement.

### Fetch, decode, and rename

The frontend follows the normal custom-0 decode path. A record load allocates a
ROB entry, load-queue entry, and renamed integer destination. The property
instruction reads that renamed destination as `rs2`; issue therefore waits for
both the property address/base and the ReusePlan.

No shared metadata mailbox is used by the O3 path. TimingSimpleCPU can use a
serialized mailbox-equivalent diagnostic because its loads cannot overlap, but
that path is not evidence of out-of-order request binding.

### Execute and LSQ Request construction

The AGU forms:

- record address plus immediate for `plan.load` and `flow.load`;
- the software-computed address for `bind.load`; or
- property base plus destination times element size for `bind.iload`.

The LSQ applies ordinary memory-dependence, ordering, and replay rules. For a
ReuseBind property load it then attaches:

```text
destination, tier, epoch1, epoch2, epoch_count,
current_epoch, context_id, dynamic_sequence, conflicted
```

to that dynamic Request. The property Request never receives FlowThrough.

### Cache and retirement

Private-cache and LLC hits return data normally. On a miss, compatible MSHR
targets preserve the selected extension. The LLC accepts a live stamp only
after validating context and destination line. The integer or floating result
writes back normally and the ROB retires precisely in order.

## 3. MSHR metadata lifecycle

### Figure 3 — ReuseBind across MSHR merge, fill, and invalidation

![Four-band MSHR lifecycle showing typed request fields, compatible and conflicting merges, independent allocOnFill aggregation, LLC acceptance, refresh, and invalidation](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg)

**Figure 3.** ReuseBind validity and FlowThrough allocation are independent
state machines.

The MSHR rebuilds its ECG state whenever active targets change:

- compatible governed targets require the same requestor and nonzero context;
- the newest sequence supplies the selected payload;
- equal sequences must carry identical payloads;
- governed/ungoverned mixing, context or requestor disagreement, invalid
  context, or equal-sequence payload disagreement marks a conflict.

The selected extension is copied to the downstream response. A conflict marker
propagates, so the LLC cannot mistake a merged request for valid metadata.
MSHR deallocation resets this state.

`allocOnFill` is independent and combines with OR. A FlowThrough target cannot
suppress an ordinary target's required LLC allocation.

## 4. Implementation sources

| Layer | Source |
|---|---|
| instruction roles and memory semantics | `bench/include/gem5_sim/overlays/arch/riscv/isa/decoder_ecg_extract.isa` |
| decoder fields | `bench/include/gem5_sim/overlays/arch/riscv/isa/formats/ecg.isa` |
| guest `.insn` emitters | `bench/include/gem5_sim/gem5_harness.h` |
| Request extension and merge rules | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh` |
| MSHR integration | `bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch` |
| O3 attachment | `bench/include/gem5_sim/overlays/cpu/o3/lsq_ecg_producer.patch` |
| graph-kernel call sites | `bench/src_gem5/` |
