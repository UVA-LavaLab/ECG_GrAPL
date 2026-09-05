<p align="center"><img src="wiki/assets/logo.png" alt="ECG graph logo" width="180"></p>

# ECG

**Edge-carried reuse: from graph structure to cache decisions**

For a fixed graph traversal, the edge stream describes both **which property
to read** and **when its cache line will be needed again**. Ordinary replacement
policies see accesses but not this graph-derived future. ECG carries a compact
reuse description with the edge, avoiding a runtime P-OPT rereference-matrix
lookup on the current REF32 path.

The record remains an ordinary **32-bit edge word**. Its low bits identify the
property vertex; available high bits carry a metadata mask. The mask changes
how the cache treats a line, not the graph or the property's value.

## Choose the encoding for the graph

**Scale6 is the compact large-ID format, not the whole ECG design.** If
`b = max(1, ceil(log2 |V|))` bits identify a vertex, the word has `32 - b`
spare bits. Smaller graphs can therefore use richer metadata.

| Current format | ID-width limit | Metadata | Implementation |
|---|---|---|---|
| **Full14** | up to 18 bits | 14 bits total; default 8 reference + 2 state + 4 prefetch-action bits | cache_sim; configurable reference/action splits |
| **Scale6** | up to 26 bits | 6-bit state/distance token; no separate action field | cache_sim, plus the current fixed 26+6 native RISC-V ABI |

Twitter-2010 needs 26 ID bits, leaving six for Scale6. The small example below
needs only five ID bits: Full14 uses 14 metadata bits and leaves 13 unused.
The implemented codecs are explicit choices, **not an automatic allocator of
every spare bit**. Native rich-format decoding is not implemented merely
because a smaller graph would fit it.

## Follow one edge through the design

![ECG example connecting graph vertex 8 and property vertex 18 to CSR position 18, Full14 or Scale6 encoding, ordinary property loading, retirement metadata, and a changed cache victim](fig/wiki/home/home-f01-system-overview.svg)

1. **Graph to CSR.** PageRank pull at outer vertex `u=8` reads its in-neighbors.
   CSR entry `j=18` names property vertex `v=18`; the next use of that property
   cache line is at `j=22`, four governed requests later.
2. **CSR to mask.** Full14 encodes the distance and state above the five ID bits:
   metadata `M=0x00002200` turns ordinary ID `0x00000012` into record
   `R=0x00002212`. Both the ID and the reuse fields remain recoverable.
3. **Mask to property load.** Decoding still selects `p[18]`, at example address
   `0x80000048`. Its first-iteration value is still `1/128`, not the mask.
   The native figures use the same edge encoded as Scale6, `0x10000012`,
   because that is the implemented native ABI.
4. **Load to cache metadata.** The native path retains the prediction on the
   dynamic instruction and exports it only after retirement. A bounded channel
   includes private-cache hits, which otherwise leave no new demand at the LLC.
5. **Metadata to eviction.** In the worked two-way cache, LRU would evict line
   A because it is older than B. The graph says A is needed at request 20,
   before B at 23; ECG's decoded bounds instead select B. This is an
   explanation of the decision, not a benchmark speedup claim.

The [walkthrough](wiki/Property-to-Cache-Walkthrough.md) derives the numbers.
The [native pipeline](wiki/RISC-V-Instruction-Path.md) distinguishes record
bytes, renamed operands, returned data, request observations and committed
predictions. Prefetch is a separate mechanism: Full14 carries a selected
forward-record lead, while Scale6 selects from a bounded record window.

## What is implemented, and what the results mean

| Surface | Current role and limitation |
|---|---|
| **cache_sim** | Full14 and Scale6 replacement/commit/prefetch mechanisms; functional cache and traffic results, including full Twitter with Scale6 |
| **gem5 RV64 O3** | Real Scale6 record/F32 loads, retirement transport and LLC replacement; native prefetch and production timing admission remain closed |
| **Sniper** | Earlier matched-work modeled controls; REF32 rows remain unsupported |
| **RTL / physical cost** | Earlier components, not a complete REF32 physical implementation |

The native path is an experimental RISC-V custom-0 implementation,
not a ratified RISC-V extension.

The [evaluation page](wiki/Evaluation-Methodology.md) reports demand misses
alongside total off-chip reads and writebacks, including P-OPT's matrix
charge. These are not interchangeable with processor time or silicon area.
Keeping the LLC data ways does not make ECG's metadata and ports free.

The primary REF32 comparisons keep **FlowThrough off**. Earlier two-epoch
ReusePlan/ReuseBind and FlowThrough paths remain separate controls. Metadata
must describe the actual traversal. Pull visits in-neighbors `N_in(u)`, and
property `p[v]` is read for destinations in `N_out(v)`.
The property-request count is therefore `d_out(v)`.
For a traversal over out-neighbors `N_out(u)`,
property `p[v]` is read once for each source in `N_in(v)`, giving `d_in(v)`
accesses. Dynamic frontier traversals do not automatically inherit a fixed
PageRank sweep's future-use description.

## Documentation

- [Edge-carried records and cache control](wiki/ReusePlan-FlowThrough.md)
- [Native record-to-cache pipeline](wiki/RISC-V-Instruction-Path.md)
- [A checked edge-to-cache example](wiki/Property-to-Cache-Walkthrough.md)
- [Evaluation methodology and results](wiki/Evaluation-Methodology.md)
- [Related work](wiki/Related-Work.md)
- [Build and reproduction](wiki/Reproduction.md)
- [Repository hygiene](wiki/Repository-Hygiene.md)

## Repository layout

| Path | Purpose |
|---|---|
| `bench/include/` | shared record, policy, ISA and simulator integration |
| `bench/src_sim/` | functional cache-simulator graph kernels |
| `bench/src_gem5/` | native record/property execution and earlier gem5 graph kernels |
| `bench/src_sniper/` | Sniper graph workload |
| `bench/src_rtl/` | existing ReusePlan cost models |
| `scripts/experiments/ecg/` | experiment runners and fail-closed gates |
| `scripts/docs/` and `fig/` | deterministic SVG figures and editable Draw.io mirrors |
| `wiki/` | architecture, evidence, and reproduction guides |

Experiment output remains under `results/` and is not tracked. Wiki and
conference-paper figures have separate generators and layouts; changing a
wiki plate never compresses or overwrites its paper counterpart.
