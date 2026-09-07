<p align="center"><img src="wiki/assets/logo.png" alt="ECG graph logo" width="180"></p>

# ECG

**Edge-carried graph reuse for cache replacement and prefetching**

For a fixed graph traversal, an edge identifies both the property to read and
the next use of that property's cache line. ECG encodes a conservative
graph-derived reuse bound in the edge record, carries it through the real load,
and uses it for replacement and bounded lookahead prefetching. Property values
and graph semantics remain unchanged.

This repository develops the full-paper successor to our
[ECG workshop paper (IPDPSW 2024)](https://doi.org/10.1109/IPDPSW63119.2024.00105),
with an improved graph-adaptive model and native RISC-V/gem5 implementation.

## One graph-adaptive record layout

ECG has one current record grammar rather than public versioned or fixed-width
format families. `Requirements` supplies `vertex_count`,
`max_vertex_id_known`, `max_vertex_id`, `record_count`, `traversal_count`,
`requested_record_bytes`, and `minimum_mantissa_bits`. Layout selection uses:

- the maximum vertex ID actually encoded (not `vertex_count - 1` when isolated
  vertices enlarge the property domain);
- `record_count` and `traversal_count`;
- requested record width `0`, `4`, or `8` bytes; and
- a minimum mantissa precision.

For a word of `W` bits:

```text
id_bits       = max(1, bit_width(max_encoded_vertex_id))
metadata_bits = W - id_bits
H             = bit_width(record_count)
K             = H * 2^mantissa_bits
```

The codec selects the largest `mantissa_bits` satisfying
`2 + 2*K <= 2^metadata_bits`. Width `0` tries four bytes and then the explicit
eight-byte escape. Tokens are `UNKNOWN=0`, `DEAD=1`,
`FINITE=2+q`, and `WRAP=2+K+q`, where
`q=(exponent<<mantissa_bits)|mantissa`. Decoding returns a conservative upper
bound. Invalid layouts, tokens, horizons, addresses, and 64-bit
sequence/deadline arithmetic fail closed.

This is bit-granular: with `H=31`, ID widths 18, 19, and 20 leave 14, 13, and
12 metadata bits and select mantissas 8, 7, and 6. The 26-ID/6-metadata/m0
case reproduces an older compact byte layout only as one numeric
configuration. A full 32-bit ID requires an eight-byte record with 32 metadata
bits; that width alone does **not** establish exascale graph-loader support.

## Follow one checked edge

![ECG example connecting graph vertex 8 and property vertex 18 to CSR position 18, the adaptive record, ordinary property loading, retirement metadata, and a changed cache victim](fig/wiki/home/home-f01-system-overview.svg)

The shared fixture has 32 vertices and 34 records. PageRank pull at outer
vertex `u=8` reads property vertex `v=18` from CSR position `j=18`; the next
use of that 64-byte property line is at `j=22`.

Its current layout is:

```text
record_bytes=4 id_bits=5 metadata_bits=27
horizon_bits=6 exponent_bits=3 mantissa_bits=23
distance=4 q=(2 << 23) token=2+q
metadata mask=0x20000040 record=0x20000052
sequence=19 deadline=23
```

Masking with `0x1f` recovers vertex 18. The property address is
`0x80000048`, its line is `0x80000040`, and the unchanged F32 contribution is
`1/128` (`0x3c000000`). In the teaching two-way set, line A has remaining
bound 1 and score 0 while B has remaining bound 4 and score 1. LRU evicts A;
ECG evicts B. This is a mechanism example, not a performance result.

The same sixteen-record window rule is used for every layout: among leads
8–15, consider the first occurrence of each distinct property line, rank the
smallest decoded cyclic reuse bound first, then proximity to lead 10 and lower
lead. `UNKNOWN` and `DEAD` rank as infinity. The fixture's A/B window has no
eligible new line. Missing required record bytes return `NOT_READY`; they do
not fabricate a no-candidate result.

## Implemented surfaces and evidence

| Surface | Current role | Evidence boundary |
|---|---|---|
| **cache_sim** | Shared codec, victim rule, real-record window and explicit access-step timing | strict admission passed for all four mechanisms, both widths, and exact 24 MiB; no CPU-cycle speedup |
| **gem5 RV64 O3** | Raw 4/8-byte record loads, dependent F32 property loads, retirement transport, replacement and acknowledged LLC-only prefetch | native architectural/timing evidence for serial fixed-iteration PageRank |
| **Sniper** | Actual 4/8-byte record loads and modeled transport/replacement/prefetch | all four mechanisms admitted, including 4-byte live and 8-byte SIFT translation cases; not native RISC-V timing |
| **RTL / physical cost** | Earlier component studies | not a complete current-method silicon-area result |

The native path is an experimental RISC-V custom-1 implementation using opcode
`0x2b`: record32 is
funct3 `0`/funct7 `0`, record64 uses funct7 `1`, and PropertyF32 is funct3
`1`/FUNCT2 `0`. The property instruction consumes property base, raw record
word, and the real record address. P17 in the fixture contains raw
`0x0000000020000052`; semantic sequence 19 is computed and checked separately.
This is not a ratified RISC-V extension.

Native updates pass from per-DynInst state through a 16-slot retirement queue
with at least eight CPU cycles of latency, configurable capture width, one
output per cycle, and two versions per key with the oldest protected.
Resident metadata updates never allocate, dirty, or alter ordinary recency or
RRPV. Native prefetch uses real record-line acquisition, MMU translation,
retry-capable ports, issue/completion duplicate and admission checks, and
acknowledged `ReadReq` traffic at the LLC input. Guests drain bounded pending
work before ROI end and before releasing the carrier.

Current builders retain the source graph and construct a separate
`vector<uint32_t>` or `vector<uint64_t>` carrier using sparse line first/next
scratch. Receipts distinguish `source_stream_bytes`, `retained_source_bytes`,
carrier payload/allocation, and auxiliary peak bytes.

Current Sniper rows require `--sniper-workload sg_kernel`, one core, an
uncapped fixed PageRank traversal, true modulo LLC indexing, and the mandatory
`--sniper-record-rss-mib` process-tree watchdog (default 2048 MiB). Its guest
window is always `16 * uint64_t`: 1,024 data bits plus 1,024 index bits and
16 valid bits, even for 4-byte records. The separate runtime word bank is
`16 * 257` bits. Its update link is bounded completion corroboration, not
architectural retirement.

For traversal direction, pull visits in-neighbors `N_in(u)` and property
`p[v]` is read for destinations in `N_out(v)`. The property-request count is therefore `d_out(v)`.
For traversal over out-neighbors `N_out(u)`, the
property `p[v]` is read once for each source in `N_in(v)`, giving `d_in(v)`. Metadata
must describe the exact order executed.

Historical Twitter, P-OPT, P-OPT-SE, ReusePlan, and FlowThrough results remain
available with their original names, revisions, encodings, receipts, and
limitations. They are not relabeled as results for the current adaptive
method.

## Run small before large

With all three backends built, start with the bounded current workflow:

```bash
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_current_equivalence \
  --run-dir results/ecg_experiments/runs/ecg_current_equivalence --no-build
```

This default profile runs 36 cells serially: the 32-vertex worked example and a
512-vertex pressure graph, both record widths, and all three backends. Actual
record-load semantics and PageRank results must agree; misses, cycles, and
issued prefetch totals need not. These instrumented rows are not speedup evidence.

Use `ecg_large_cache` for accurate, non-authorizing large-graph cache/traffic
exploration. `ecg_detailed_final` requires both `--final-stage` and a current
`--equivalence-receipt`; old or partial completions cannot authorize it.
`ecg_local_release_cache` adds the full six-graph, ten-role CSR/reference/ECG
comparison at the primary 8 MiB capacity before the lab handoff.
Construction, target-memory, wall-time, and process-tree RSS limits stay explicit.
See [reproduction](wiki/Reproduction.md#6-current-qualification-and-historical-campaign-recipes).

[Current sampled results](wiki/Evaluation-Methodology.md#41-current-sampled-preliminary-results)
include native transport-matched speedups, traffic, and all three backends'
miss counters, including the Sniper Patents regression. They are preliminary
4,096-vertex pressure cases, not full-graph paper results.
[Full-core local results](wiki/Evaluation-Methodology.md#43-full-core-local-release-results)
cover six full graphs and all ten roles at 8 MiB, with both wins and regressions retained.

## Documentation

- [Adaptive records and cache control](wiki/ReusePlan-FlowThrough.md)
- [Native record-to-cache pipeline](wiki/RISC-V-Instruction-Path.md)
- [Checked edge-to-cache example](wiki/Property-to-Cache-Walkthrough.md)
- [Evaluation methodology and historical results](wiki/Evaluation-Methodology.md)
- [Related work](wiki/Related-Work.md)
- [Build and reproduction](wiki/Reproduction.md)
- [Repository hygiene](wiki/Repository-Hygiene.md)

Generated wiki figures and editable Draw.io mirrors are under `fig/wiki/` and
`fig/wiki_src/`. The separate paper figure collection is not rewritten by the
wiki generator.
