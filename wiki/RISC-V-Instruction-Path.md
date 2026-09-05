# RISC-V integration: existing support and Scale6 target

The repository contains an experimental custom-0 RISC-V ReusePlan/ReuseBind
implementation and a separate native Scale6 path in gem5. Scale6 record/F32
operations, a retirement-only transport and `GraphRef32RP` are implemented
in RISC-V O3 and **under replacement-only qualification**. Native prefetch
and production timing admission remain closed; this is not a complete
Scale6 timing or physical-area result.

## 1. Reuse the execution discipline, not the old bit layout

### Figure 1 — RISC-V integration: existing path and next step

![Existing ReuseBind record-load and dependent property-load roles separated from the pending Scale6 contract for a 26-plus-six-bit record, translated dynamic request state, retirement-only refresh and real lookahead traffic](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg)

**Figure 1.** Existing record acquisition and property access remain two
dynamic operations with an explicit renamed dependency. That foundation does
not automatically implement the Scale6 token or its commit/prefetch channels.
The lower panel separates the native operand/retirement contract from the
still-unimplemented prefetch boundary.

| Existing family | Role in the legacy implementation |
|---|---|
| `ecg.plan.load` | ordinary-placement general ReusePlan acquisition |
| `ecg.flow.load` / `.compact` | record acquisition with request-specific FlowThrough |
| weighted record helpers | acquire the existing weighted sidecar formats |
| `ecg.bind.load.*` | computed-address typed property load with ReuseBind |
| `ecg.bind.iload.*` | indexed property load with ReuseBind |

The old current-epoch/context/format CSRs and tier/two-epoch payload belong to
that implementation. Scale6 uses a request-sequence future bound, not two
outer-vertex epochs. The research instruction family is neither a ratified
RISC-V extension nor an upstream gem5 feature.

The shared Scale6 codec defines the following RV64 operand contract. The
record and property raw custom-0 operations implement it:

| Value | Bit layout |
|---|---|
| configuration | record count `[30:0]`, vertex count minus one `[56:31]`, enable `[57]`, version `[59:58]`, reserved `[63:60]` |
| iteration descriptor | sequence base `[31:0]`, another-iteration flag `[32]`, remaining bits zero |
| canonical record operand | semantic sequence `[63:32]`, runtime-normalized Scale6 record `[31:0]` |

Version 1 requires positive record count below `2^31`, at most `2^26`
vertices, aligned addresses and non-overflowing configured ranges. Encoding
the vertex count minus one preserves the exact `2^26`-vertex boundary.
The record address determines the one-based edge position; the iteration
descriptor adds the sequence base modulo `2^32`. WRAP becomes FINITE when
another traversal remains, otherwise DEAD.

Sequence zero and even an all-zero canonical operand can be valid after
counter wrap. Consumers must use the codec's explicit validity result, not
a zero-word sentinel. Modular ordering treats an exact half-range difference
as ambiguous. These helpers live in `bench/include/ecg_ref32.h`; they do not
open the native gem5 experiment gate on their own.

The experimental record operation uses raw funct7 `0x30`, the F32 property
operation uses `0x34`, and both use custom-0 / funct3 `0x2`. Their reserved
width subcodes and RV32 forms remain invalid. Record-base and configuration
CSRs are `0x803` and `0x804`; the existing context CSR is `0x801`. The guest
emits `.insn`, not a claimed standard assembler mnemonic. A dedicated native
probe covers the maximum n26 vertex, final-WRAP normalization, signed zero,
NaN payload preservation, zero canonical operands and invalid record bounds.

## 2. Native Scale6 replacement integration

### Figure 2 — Native Scale6 path through an out-of-order core

![Native out-of-order core containing fetch, decode, rename, issue, physical register P17, the record-to-property dependency, AGU, LSQ, translation and private caches, plus the separately timed retirement-to-LLC metadata channel](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg)

**Figure 2.** The checked word `0x10000012` names property vertex `18` and
token `4`. Standard O3 mechanisms must preserve the association with its own
property access. A private hit returns data without reaching the LLC, but
the eventual committed access may still need to refresh resident LLC metadata.

Each dynamic property instruction retains its own normalized token,
semantic position and context. Retirement reads its translated physical
address, not the O3 instruction sequence number or a shared last-request
mailbox. The associated record/property loads execute through ordinary
memory dependence, replay, exception and squash handling.

Data completion and architectural retirement are different events. Ordinary
data requests can be speculative; a retirement-only commit refresh must not
be enqueued by a squashed load. `EcgRef32CommitTransport` listens only to
successful O3 `Commit` events, validates record/property pairing and
contiguous semantic positions, and includes private-cache hits.

The guest encodes its borrowed in-edge CSR in place and restores the IDs
after the ROI. It exports static region descriptions, not edge-data files
or a runtime future table. The LRU control uses the same native instruction
pair and fixed-iteration loop, but validates retirement without applying
updates. Comparing against the older software-address-generation loop
would confound replacement with different executed work.

## 3. MSHR lifetime versus metadata lifetime

### Figure 3 — Request lifetime is not metadata lifetime

![Existing ReuseBind MSHR compatibility and conflict propagation above a target Scale6 state machine distinguishing issue, completion, squash, retirement, commit-queue coalescing and independent LLC expiry](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg)

**Figure 3.** Existing MSHR merge rules select compatible per-request
metadata. Commit-update coalescing instead carries later committed knowledge
about a resident line. These are not interchangeable mechanisms.

Existing ReuseBind compatibility requires matching requestor and nonzero
context. The newest sequence supplies the payload; equal sequences require
identical payloads. Mixed ordinary/ReuseBind targets, incompatible contexts or
payload conflicts propagate a conflict marker. The MSHR clears this state
on release.

Native Scale6 uses a separate observation extension. A governed request
observed at the LLC marks the line PENDING with its semantic position;
it does not install FINITE/DEAD or advance the receiver watermark.
MSHR merge/rebuild/response handling retains the newest compatible
observation and clears obsolete extensions. An older delivered update
cannot overwrite a newer pending observation.

`allocOnFill` is independent and combines with OR. The primary Scale6
comparison does not enable FlowThrough. Metadata delivery performs a
non-touching tag lookup: it cannot allocate a data line, alter its contents
or dirty state, or count as an ordinary cache hit. Nonresident and expired
updates are accounted separately.

Address binding and property classification have separate validity. A
physical-only writeback can acquire a governed VA later, and a line installed
before static context registration can be classified without losing its
established VA/PA identity. Classification uses the cache line, including a
partially occupied final property line. Conflicting established aliases or
known classifications still fail closed.

## 4. Bounded capture is not link bandwidth

The native queue has **16 physical slots**, a minimum **eight CPU-cycle**
delay and **one delivered update per CPU cycle**. For a given physical
line/context, the oldest snapshot is protected; a second slot can hold
a newer version, and only that secondary can be coalesced. Replacing it
starts the new version's own latency.

Coalescing can skip several short traversals. Delivered updates must remain
newer in modular sequence order, but cannot be limited to one traversal's
sequence gap. The one-traversal horizon guard applies to speculative
observations, not to this already validated retirement stream.

O3 retirement can emit several property loads in one cycle.
`--ref32-capture-width 0` therefore selects the configured CPU commit width
(currently eight); explicit widths from 1 to 16 are supported. It does
not assign a later, fictitious generation cycle to excess arrivals.
Equal-ready updates follow capture-lane order even after physical-slot
reuse. At width eight this adds 48 logical slot-order bits, in addition to
capture control and the multi-lane selection/write logic. This is not a
measured hardware-area estimate.

The 35-bit per-line figure describes prediction value/state/origin, not a
complete native implementation cost. Address/classification validation,
capture control, port logic and timing realization remain separate from
that logical payload count.

The receipt reports `captureWidth`, `captureOrderBits`, `outputWidth`,
`maxRetirementBurst`, latency and occupancy separately. Admission requires
zero drops/errors, no pending entries and all four accounting identities:

```text
generated = accepted + fullDrops + ingressDrops + degradedDrops
accepted = enqueued + coalesced
enqueued = delivered + cancelled + pending
delivered = applied + stale + expired + notResident + invalidDelivery
```

After guest exit, only the bounded transport is finished; the configuration
does not globally drain an exited SE CPU. `--ref32-allow-drops` is a
fail-visible diagnostic mode, never a way to admit a result.

## 5. What remains before timing claims

`ref32_isa_smoke_riscv_m5ops` is operand/exception evidence only. It does not
exercise a native replacement policy or produce an admissible speedup row.
The separate native PageRank integration uses real retirement and timed
metadata delivery, but broader graph qualification and strict runner
admission remain necessary. Request-count latency in cache_sim is not
native CPU-cycle latency.

The current native path assumes a dedicated metadata link and dedicated
tag lookup port. It does not establish ordinary tag-port, data-array or
coherent-interconnect contention, or silicon area. Unlike cache_sim,
native demand observations cannot precommit FINITE/DEAD predictions and
there is no speculative known-dead miss bypass.

Native lookahead must consume real carrier bytes and pay for acquisition,
translation and prefetch traffic. The proposed two-line buffer is 128
bytes plus tags/control, not the functional model's 512-bit window.
Native prefetch is not implemented. REF32 gem5 and Sniper production rows
remain unsupported at the runner.

## Implementation sources

| Existing surface | Source |
|---|---|
| custom instruction roles | `bench/include/gem5_sim/overlays/arch/riscv/isa/decoder_ecg_extract.isa` |
| decoder fields | `bench/include/gem5_sim/overlays/arch/riscv/isa/formats/ecg.isa` |
| guest emitters | `bench/include/gem5_sim/gem5_harness.h` |
| dynamic producer state | `bench/include/gem5_sim/overlays/cpu/o3/dyn_inst_ecg_producer.patch` |
| LSQ attachment | `bench/include/gem5_sim/overlays/cpu/o3/lsq_ecg_producer.patch` |
| Request extension and merge rules | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh` |
| MSHR integration | `bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch` |
| native dynamic operand state | `bench/include/gem5_sim/overlays/cpu/ecg_ref32_producer.patch` |
| native observation/MSHR state | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_observation.hh` |
| native retirement transport | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_commit_transport.cc` |
| bounded two-version queue | `bench/include/ecg_ref32_commit.h` |
| native replacement receiver | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_ref32_rp.cc` |
| supported-backend gates | `scripts/experiments/ecg/roi_matrix.py` |
