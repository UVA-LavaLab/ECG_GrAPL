# ECG figures

This directory contains ECG's public SVG figures and editable Draw.io mirrors.
The wiki collection follows one graph access through the current REF32 design:
graph, CSR, metadata mask, processor pipeline and cache decision. It treats
Full14 and Scale6 as distinct encodings and labels the fixed native ABI
separately. The paper collection preserves earlier ReusePlan layouts; it is
not a completed REF32 paper figure set.

Two independent collections are generated from the same primitives:

- the wiki plates in `fig/wiki` and `fig/wiki_src`; and
- the compact conference-paper set in `fig/paper` and `fig/paper_src`.

Each collection has its own generator, register, and validator. Generating one
collection never rewrites the other.

## Generated-mode contract

The set is generated deterministically from
`scripts/docs/generate_ecg_figures.py`, `scripts/docs/ecg_figure_lib.py`, and
`fig/ecg-figure-fixture.json`.

```text
fig/wiki/<page-slug>/<page-slug>-fNN-<topic>.svg
fig/wiki_src/<page-slug>/<page-slug>-fNN-<topic>.drawio
```

Run:

```bash
python3 scripts/docs/generate_ecg_figures.py
python3 scripts/docs/generate_ecg_figures.py --check
python3 scripts/docs/check_wiki_figures.py
```

Every published figure uses a 1200 px canvas, live text of at least 16 px, the
`ecg-public/v1` schema, accessible title and description metadata, a fully
opaque background, the shared light/dark role palette, and semantic arrow
metadata. SVG and Draw.io files are emitted from the same operations and have
matching canvas, title, description, ordered labels, and arrow kinds.

Connector geometry is standardized across the set: arrows attach at symbol
boundaries, use rounded joins and compact arrowheads, route around non-endpoint
symbols, and place labels off the signal path with a light/dark-aware halo.
Transfer arrows state cadence when relevant. Semantic connectors are
orthogonal; only graph/model edges may be diagonal.

## Visual roles

| Role | Strong | Matte |
|---|---|---|
| ink / primary text | `#182230` | `#FFFFFF` |
| border / structure | `#475467` | `#F8FAFC` |
| neutral / annotation | `#98A2B3` | `#F8FAFC` |
| graph data / request data | `#2563EB` | `#EFF6FF` |
| local compute / accepted state | `#0F8A72` | `#ECFDF5` |
| transfer / prefetch / controls | `#C56A13` | `#FFF7ED` |
| verification / rejection | `#C63C4A` | `#FFF1F2` |
| policy / metadata state | `#6558C5` | `#F5F3FF` |

Visual styling uses dark navy headings, thin slate borders, mostly white or
near-white surfaces, and one or two semantic accents per local flow. Full-panel
fills are not the primary hierarchy cue.

Demand and graph-data paths are blue; future state, dependencies and commit
refresh are purple; local computation is green; prefetch and transfer
accounting are amber; invalid or unsupported states are red. Wiki annotation
text uses higher-contrast slate, reserving pale gray for background graph
edges. SVG and Draw.io mirrors retain the same labels and semantic roles.

## Sources and evidence scope

The fixture-backed mechanism example is `fig/ecg-figure-fixture.json`. The
generator derives request positions, next property-line uses, Full14 masks,
Scale6 tokens, packed words, addresses, F32 data and deadlines. The small graph
uses five actual ID bits for Full14. The native illustration uses the same
edge in its fixed 26+6 ABI. Independent fixture derivation and the actual C++
builders, decoders and victim helper pin these transformations.

## Story and visual invariants

The graph's internal IDs are the IDs in its CSR, not a second unlabeled
numbering system. Vertex `v=18`, record position `j=18`, semantic request
`s=19`, physical rename tag P17, and cache line B have different roles.
They remain explicitly labeled when the view changes.

Blue carries addresses and data; purple carries prediction state and operand
dependencies; green carries execution/decision control; amber carries
prefetch/traffic activity. Registers, queues, cache sets and arrays use
recognizable structure rather than interchangeable prose cards. Metadata
arrows do not silently become data writes.

The two-way eviction comparison is a teaching snapshot, not a production LLC
geometry or a performance measurement. Queue-cycle illustrations are not
measured schedules. Native diagrams distinguish PENDING observations from
retirement-authorized predictions and show both real memory loads, not a
host-supplied future lookup.

Architecture vocabulary and values trace to:

- `bench/include/ecg_ref32.h` for current records, quantization, builders,
  prefetch selection, expiry and victim ranking;
- `bench/include/cache_sim/cache_sim.h` for bounded update/prefetch channels,
  line state and resource accounting;
- `bench/include/cache_sim/graph_cache_context.h` and `bench/src_sim/pr.cc`
  for record consumption, traversal position and context;
- `bench/include/gem5_sim/overlays/arch/riscv/` and
  `bench/include/gem5_sim/gem5_harness.h` for legacy and native Scale6
  instruction roles;
- `bench/include/ecg_ref32_commit.h` and the native
  `ecg_ref32_commit_transport.cc` / `graph_ref32_rp.cc` overlays for the
  bounded retirement path and separate replacement receiver;
- `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_observation.hh`
  and `bench/include/gem5_sim/overlays/mem/cache/ecg_ref32_mshr_observation.patch`
  for native Request and MSHR state;
- `bench/include/gem5_sim/overlays/mem/cache/base_flowthrough.patch` for
  legacy allocation controls, which remain off in primary REF32 runs; and
- `scripts/experiments/ecg/roi_matrix.py` for simulator roles and fail-closed
  row validation.

## Architecture-paper presentation references

These are original ECG illustrations, not reproductions of source artwork.
The explanatory sequence draws on the following primary-paper devices while
using the repository's own graph, code-derived values and established palette:

| Reference | Useful presentation device | Applied here |
|---|---|---|
| [P-OPT, published manuscript](https://d1qx31qr3h6wln.cloudfront.net/publications/HPCA_2021_Cache_Replacement.pdf), Figs. 1, 3, 5, 8–9 | One graph remains recognizable through sparse representation, access order, a concrete cache decision, compressed metadata and hardware placement. | One internal-ID fixture is followed through CSR, masks, native operands and explicitly named cache ways. |
| [GRASP](https://arxiv.org/pdf/2001.09783), Figs. 1, 3–4 and Table II | Separate software transformation, interface, hardware classification and the exact policy action. | Encoding is outside the core; data, request observations and retirement updates have distinct paths; the victim score is explicit. |
| [Graphicionado](http://mrmgroup.cs.princeton.edu/papers/taejun_micro16.pdf), Figs. 4–6 | Map operations to stable pipeline roles and preserve the base/optimized frame so the changed mechanism is visible. | Real AGU/LSQ/register/ROB roles are labeled, and equal-sized LRU/ECG cache panels keep way positions fixed. |
| [ECG 2024](https://www.cs.virginia.edu/~rgq5aw/files/ecg.pdf), Figs. 1–3 | Connect graph storage, encoded information and processor/cache handling. | The current format, data transformation, native transport and cache consequences are unpacked into separate readable plates rather than reusing the historical bit allocation. |

[BOOM's pipeline](https://docs.boom-core.org/en/latest/sections/intro-overview/boom-pipeline.html)
and [ROB documentation](https://docs.boom-core.org/en/latest/sections/reorder-buffer.html)
are complementary terminology references for completion versus retirement.
The implementation claims come from the gem5 overlays listed above, not from
assuming that a published accelerator or another core has the same timing.

## Figure fixture

The construction, instruction, and cache figures use one common fixture:

| Quantity | Value |
|---|---|
| graph | 32 vertices; nine non-isolated vertices shown; weights unused |
| adjacency entry | internal `8 -> 18` (source fixture `4 -> 7`) |
| current / next property-line edge position | `18` / `22` |
| governed request sequence | `19`, one-based |
| Full14 bit budget | 5 ID + 8 reference + 2 state + 4 action; 13 unused bits |
| Full14 mask / word | `0x00002200` / `0x00002212` |
| true / decoded next-use distance | true `4`; Full14 default `4`; Scale6 `7` |
| Scale6 token and word | token `4`; `0x10000012` |
| property address | `0x80000048` |
| property line | `0x80000040`, containing vertices `16..31` |
| returned contribution | `1/128`, F32 bits `0x3C000000` |
| native canonical operand | `0x0000001310000012`, held in illustrative P17 |
| Full14 deadline / expiry | `23` / UNKNOWN at `24` if held unchanged |
| Scale6 deadline / expiry | `26` / UNKNOWN at `27` if held unchanged |
| preceding line A | last `s18`, actual next `s20`, Scale6 deadline `21` |
| worked victim choice at `s19` | LRU chooses older A; ECG chooses later-use B |

The running record's actual sixteen-word window has only A and B. B is
current and A appears at lead one, so both implemented selectors request no
prefetch for this word. The positive selection rules are explained separately;
the drawing does not invent a candidate that the graph does not supply.

cache_sim implements both formats and supplies functional cache/traffic
evidence. Native Scale6 operands and retirement/replacement are implemented;
native prefetch and production timing admission remain closed. State-bit
accounting is not silicon area. The Full14 and Scale6 functional state totals
are shown separately, rather than applying the large-graph cost to every format.

## Figure register

| Figure | Visible title | Embedding page |
|---|---|---|
| [`home/home-f01-system-overview.svg`](wiki/home/home-f01-system-overview.svg) | ECG: graph knowledge in the edge stream | [`Home.md`](../wiki/Home.md) Figure 1; repository README overview |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg) | From one graph edge to its reuse mask | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 1 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg) | Choose the mask to fit the graph | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 2 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg) | More metadata bits sharpen the future bound | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 3 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg) | Why the encoded future changes an eviction | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 4 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f05-lookahead-prefetch.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-lookahead-prefetch.svg) | Prefetch actions name a future record | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 5 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f06-capacity-accounting.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-capacity-accounting.svg) | Graph-sized matrices and cache-sized state | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 6 |
| [`risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg) | Two native loads, two different results | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 1 |
| [`risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg) | The mask follows the load through the core | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 2 |
| [`risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg) | Completion is not permission to install a prediction | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 3 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg) | One edge, two encodings, unchanged data | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 1 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg) | Where the mask and its decoded state live | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 2 |
| [`evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg`](wiki/evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg) | What each implementation can establish | [`Evaluation-Methodology.md`](../wiki/Evaluation-Methodology.md) Figure 1 |

## Conference-paper figure set

The paper set is generated deterministically from
`scripts/docs/generate_ecg_paper_figures.py` and the same
`scripts/docs/ecg_figure_lib.py` primitives.

```text
fig/paper/ecg-paper/ecg-paper-fNN-<topic>.svg
fig/paper_src/ecg-paper/ecg-paper-fNN-<topic>.drawio
```

Run:

```bash
python3 scripts/docs/generate_ecg_paper_figures.py
python3 scripts/docs/export_ecg_paper_pdfs.py
python3 scripts/docs/generate_ecg_paper_figures.py --check
python3 scripts/docs/export_ecg_paper_pdfs.py --check
python3 scripts/docs/check_ecg_paper_figures.py
```

The PDF exporter uses headless Chrome for vector rendering and Ghostscript for
stable metadata. Each PDF page is cropped to the SVG canvas, embeds the source
SVG SHA-256, and is suitable for direct `\includegraphics` use in LaTeX.

The paper contract adds page-oriented constraints on top of the shared
schema, palette, accessibility, parity, and determinism rules:

- exactly six figures, each carrying one concept;
- a landscape 1200 px canvas between 420 px and 650 px tall;
- live text of at least 17 px, so a two-column reduction stays readable;
- separated plates instead of card grids or dense multi-panel plates; and
- no owning wiki page, so the set is validated without an embedding check.

The paper figures describe implemented and modeled architecture only. They
state no measured results, and the validator rejects marketing language and
percent/speedup claims in live text.

## Paper figure register

| SVG | PDF | Draw.io mirror | Visible title | Concept |
|---|---|---|---|---|
| `paper/ecg-paper/ecg-paper-f01-offline-plan.svg` | `paper/ecg-paper/ecg-paper-f01-offline-plan.pdf` | `paper_src/ecg-paper/ecg-paper-f01-offline-plan.drawio` | Offline ReusePlan construction and the measured-ROI boundary | traversal-selected CSR rows to edge-aligned records; construction ends before the ROI |
| `paper/ecg-paper/ecg-paper-f02-compact-record.svg` | `paper/ecg-paper/ecg-paper-f02-compact-record.pdf` | `paper_src/ecg-paper/ecg-paper-f02-compact-record.drawio` | Compact ReusePlan record: 32-bit budget and edge substitution | destination, optional carried tier, two epochs, and fail-closed width binding |
| `paper/ecg-paper/ecg-paper-f03-request-path.svg` | `paper/ecg-paper/ecg-paper-f03-request-path.pdf` | `paper_src/ecg-paper/ecg-paper-f03-request-path.drawio` | Record load and property load through the RISC-V request path | decode, AGU, LSQ, Request extension, MSHR, and LLC for both loads |
| `paper/ecg-paper/ecg-paper-f04-llc-decision.svg` | `paper/ecg-paper/ecg-paper-f04-llc-decision.pdf` | `paper_src/ecg-paper/ecg-paper-f04-llc-decision.drawio` | LLC ReuseBind acceptance and victim selection | validation, line-local metadata, RRIP eligibility, and future distance |
| `paper/ecg-paper/ecg-paper-f05-flowthrough.svg` | `paper/ecg-paper/ecg-paper-f05-flowthrough.pdf` | `paper_src/ecg-paper/ecg-paper-f05-flowthrough.drawio` | FlowThrough separates service from LLC fill allocation | hit bypass, all-no-allocate miss, and the allocating merge-target corner case |
| `paper/ecg-paper/ecg-paper-f06-evidence-boundary.svg` | `paper/ecg-paper/ecg-paper-f06-evidence-boundary.pdf` | `paper_src/ecg-paper/ecg-paper-f06-evidence-boundary.drawio` | Evidence boundary for ECG evaluation claims | simulator roles, receipt gates, and the optimistic analytic P-OPT bound |
