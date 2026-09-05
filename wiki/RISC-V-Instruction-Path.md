# Native record-to-cache pipeline

The native port implements **one current REF32 encoding: the fixed 26+6
Scale6 ABI**. This page follows its real record/property loads and
retirement-driven replacement path in RV64 gem5 O3. Full14's richer
small-graph encoding is implemented in cache_sim, not automatically selected
by the native decoder. Native prefetch and production timing admission remain
unfinished.

## 1. Two instructions consume different data

### Figure 1 — Two native loads, two different results

![The current native configuration and record load producing a canonical renamed integer operand, followed by the dependent property load with distinct F32 data and per-instruction prediction outputs](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg)

**Figure 1.** The running edge is still `u=8`, `j=18`, `v=18`. In this ABI,
its memory word is `0x10000012`. I0 loads that real four-byte record and
assembles the canonical integer operand; I1 uses it to load the ordinary
property value. Their results are not interchangeable.

| Operation | Memory access | Architectural result | Retained association |
|---|---|---|---|
| **I0: record load**, raw funct7 `0x30` | 32-bit word at `record_base + 4*j` | RV64 operand containing semantic sequence and normalized record | record identity used to validate the pair |
| **I1: F32 property load**, raw funct7 `0x34` | ordinary F32 at `property_base + 4*v` | unchanged property value, with normal FPU boxing | this instruction's sequence, prediction, context and translated address |

Both operations use custom-0 opcode `0x0b`, funct3 `0x2`; only RV64 and the
defined width subcode are accepted. Guest wrappers emit `.insn`. These are
experimental encodings, not ratified RISC-V instructions or claimed standard
assembler mnemonics.

The record-base CSR is `0x803`, configuration is `0x804`, and context is
`0x801`. Configuration is established with the existing serialized,
non-speculative CSR discipline. The individual record and property loads
still use the ordinary speculative memory pipeline.

| Configured/derived value | Implemented layout |
|---|---|
| configuration | record count `[30:0]`, vertex count minus one `[56:31]`, enable `[57]`, version `[59:58]`, reserved `[63:60]` |
| iteration descriptor operand | sequence base `[31:0]`, another-iteration flag `[32]`, remaining bits zero |
| canonical record operand | semantic sequence `[63:32]`, normalized Scale6 record `[31:0]` |

For the example, record address determines `j=18`; first-iteration base zero
gives `s=19`. I0 writes `0x0000001310000012` to illustrative rename tag P17.
I1 extracts vertex 18 and bound seven, forms virtual address `0x80000048`,
and returns `1/128` with F32 bits `0x3C000000` to illustrative tag F9.
The 64-bit register operand does not imply an eight-byte edge-memory load.

## 2. Preserve the dependency through the real processor

### Figure 2 — The mask follows the load through the core

![Actual O3 stage containment and dataflow for both loads through rename, issue, physical registers, AGU, LSQ, translation and private caches, with a separate ROB-to-LLC retirement channel and distinct data and replacement fields](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg)

**Figure 2.** The frontend decodes an opcode, not a future memory word.
Rename gives I0's integer result its own physical destination, and I1 waits
for that exact operand. Both loads use the AGU, LSQ, translation and normal
cache hierarchy. Returned record bytes are assembled into `sequence|record`;
returned property bytes remain F32 data.

Dispatch, issue and execute are expanded functional roles within gem5's IEW
machinery, not additional independently timed stages introduced by the drawing.
The distinction between writeback and ROB-head retirement also follows the
standard O3 terminology illustrated in the
[BOOM pipeline](https://docs.boom-core.org/en/latest/sections/intro-overview/boom-pipeline.html)
and [ROB](https://docs.boom-core.org/en/latest/sections/reorder-buffer.html)
documentation; those references do not establish this gem5 port's timing.

I1's prediction is captured on the **dynamic instruction**. It is not
reconstructed from a shared “last request” mailbox, a per-vertex future table,
or the O3 instruction sequence number. At retirement, the transport uses
I1's own translated physical address; it does not depend on a Request object
whose lifetime may already have ended.

There are two routes to the LLC with different permissions:

1. **Ordinary demand route.** A private miss carries a request observation
   containing sequence/context/destination. An LLC touch or fill can mark the
   line PENDING. It does not install the future deadline.
2. **Retirement metadata route.** Successful O3 `Commit` authorizes a delayed
   update for the matching physical line and context. This route also covers
   private hits, for which no new demand reaches the LLC.

The receiver's semantic position advances only when a timed update arrives.
There is no free issue-time or CPU-retirement watermark callback. The native
model explicitly assumes a dedicated metadata link and tag-lookup port;
ordinary tag/data-port and interconnect contention are not established by
this implementation.

## 3. Give observations and committed predictions different lifetimes

### Figure 3 — Completion is not permission to install a prediction

![Native request observation, data completion and retirement permissions, a pending-sequence guard rejecting an older update, and a bounded two-version coalescing example with its own ready cycles](../fig/wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg)

**Figure 3.** I1 can complete and later be squashed. Only successful retirement
can enqueue its prediction. The top row shows the private-miss case; a
private hit skips the LLC observation while retaining retirement delivery.
The final table is a queue-only timing illustration, not the measured
schedule of the PageRank example.

Native MSHR state keeps the newest compatible observation, propagates
conflicts and removes obsolete extensions when targets are rebuilt or
released. PENDING reuses the per-line value field for the newest observed
semantic position and ranks as UNKNOWN. It is not a speculative FINITE/DEAD
prediction.

If the line has observed `s23`, a delivered `s19` update cannot overwrite that
newer knowledge. It is counted STALE while the received watermark can advance.
A later coalesced `s25` update can install the prediction. In this particular
trace both updates can carry deadline 26; equal deadlines do not make their
semantic versions equivalent.

Address identity and classification also have separate validity. A
physical-only writeback can acquire a validated governed VA later; a line
installed before context registration can be classified without discarding
its established VA/PA identity. Known conflicting aliases remain rejected.
Classification is line-granular, including partially occupied final property
lines.

## 4. Capture bursts without inventing link bandwidth

The queue has **16 physical slots**, minimum **eight CPU-cycle latency**, and
**one output update per CPU cycle**. The oldest snapshot for a
`{physical_line, secure, context}` key is protected. A second same-line version
uses another real slot; further coalescing replaces only that secondary and
starts the replacement's own full latency. After the oldest leaves, the
remaining version becomes protected.

O3 retirement can produce more than one governed load in a cycle.
`--ref32-capture-width 0` selects the configured CPU commit width, currently
eight; explicit widths 1 through 16 are supported. Capture and delivery
bandwidth are reported separately. Same-ready updates preserve capture-lane
order even when physical slots are reused. Eight-wide capture adds 48 logical
slot-order bits, plus control and multi-lane selection/write logic.

Coalescing can skip several short traversals. Delivered updates must remain
newer in modular sequence order, but their gap is not restricted to one
traversal. The one-traversal horizon guard applies to speculative observations,
not to the validated retirement stream.

Metadata delivery performs a non-touching resident tag lookup. It cannot
allocate a data line or modify data, dirty state, ordinary hit statistics or
recency. Prediction and RRPV updates are intentional. Nonresident, stale and
expired outcomes remain separate in the receipt:

```text
generated = accepted + fullDrops + ingressDrops + degradedDrops
accepted  = enqueued + coalesced
enqueued  = delivered + cancelled + pending
delivered = applied + stale + expired + notResident + invalidDelivery
```

The default is fail-fast on errors or drops. Diagnostic
`--ref32-allow-drops` disables prediction use after degradation and cannot
make a result admissible. After guest exit, only the metadata transport is
finished within a finite budget; the configuration does not globally drain
an exited SE CPU.

## 5. Keep the native port distinct from the functional model

| Boundary | cache_sim REF32 | Current native RV64 path |
|---|---|---|
| encoding | Full14 and Scale6 | fixed 26+6 Scale6 ABI |
| property-load association | modeled request hints | real renamed operand and per-DynInst state |
| demand observation | can stamp request-bound predictions | PENDING only; prediction installs on timed retirement delivery |
| update latency unit | governed requests | CPU cycles |
| known-dead governed-miss bypass | implemented | no speculative native bypass |
| LLC-only prefetch | functional mechanism | not implemented |

The native PageRank guest borrows the in-edge CSR, encodes it in place and
restores ordinary IDs after the ROI, including an aliased undirected CSR.
Only static region descriptions are exported; no edge-data files or runtime
future table supply the instruction operands. The LRU control executes the
same native instructions and fixed-iteration loop, but validates retirement
without applying metadata. The older software-address-generation loop is
not an ISA-matched replacement control.

Version 1 accepts positive record counts below `2^31`, at most `2^26`
vertices, aligned addresses and non-overflowing ranges. The vertex-count-minus-
one field preserves the exact `2^26` boundary. Sequence zero and an all-zero
canonical operand can be valid after wrap; explicit validity, not a zero
sentinel, decides acceptance. Exact half-range sequence differences are
ambiguous and rejected.

The supported native workload is serial fixed-iteration PageRank, one trial,
FlowThrough off, and O3 with one hardware thread. Native rich-format decoding,
real-byte lookahead/prefetch, production timing admission and physical-area
qualification remain separate work. The proposed native lookahead holds two
real cache lines, 128 bytes plus tags/control, not an assumed free 512-bit
functional window.

## Earlier instruction families

The repository also retains the earlier ReusePlan/ReuseBind `plan.load`,
`flow.load`, weighted helpers, and `bind.load` / `bind.iload` families. Their
tier/two-epoch payloads, format CSRs and request-specific FlowThrough behavior
must not be mistaken for the REF32 instruction pair described above.

## Implementation sources

| Surface | Source |
|---|---|
| operand/configuration codecs | `bench/include/ecg_ref32.h` |
| native instruction bodies | `bench/include/gem5_sim/overlays/arch/riscv/isa/decoder_ecg_extract.isa` |
| guest emitters and native PageRank | `bench/include/gem5_sim/gem5_harness.h`, `bench/src_gem5/pr.cc` |
| dynamic instruction capture | `bench/include/gem5_sim/overlays/cpu/ecg_ref32_producer.patch` |
| request observation attachment | `bench/include/gem5_sim/overlays/cpu/o3/ecg_ref32_observation.patch` |
| observation/MSHR state | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_observation.hh` |
| timed retirement channel | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_commit_transport.cc` |
| bounded queue and native receiver | `bench/include/ecg_ref32_commit.h`, `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_native_state.hh` |
| resident-line replacement policy | `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_ref32_rp.cc` |
| reproducible commands and evidence boundaries | [Reproduction](Reproduction), [Evaluation methodology](Evaluation-Methodology) |
