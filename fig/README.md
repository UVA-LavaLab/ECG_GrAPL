# ECG figures

This directory contains ECG's public SVG figures and editable Draw.io mirrors.
The wiki collection describes the current Scale6 model and its pending native
integration. The separate paper collection preserves the earlier ReusePlan
layouts; it is not a completed Scale6 paper figure set.

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
generator derives canonical request positions, next property-line use,
logarithmic token, packed word, address and deadline. It forces the 26-bit
Scale6 layout. Independent fixture derivation and the actual C++ token and
lookup helpers pin those values.

Architecture vocabulary and values trace to:

- `bench/include/ecg_ref32.h` for current records, quantization, builders,
  prefetch selection, expiry and victim ranking;
- `bench/include/cache_sim/cache_sim.h` for bounded update/prefetch channels,
  line state and resource accounting;
- `bench/include/cache_sim/graph_cache_context.h` and `bench/src_sim/pr.cc`
  for record consumption, traversal position and context;
- `bench/include/gem5_sim/overlays/arch/riscv/` and
  `bench/include/gem5_sim/gem5_harness.h` for existing legacy instruction
  roles, not a completed native Scale6 port;
- `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh`
  and `bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch` for
  Request and MSHR state;
- `bench/include/gem5_sim/overlays/mem/cache/base_flowthrough.patch` for
  legacy allocation controls, which remain off in primary Scale6 runs; and
- `scripts/experiments/ecg/roi_matrix.py` for simulator roles and fail-closed
  row validation.

## Figure fixture

The construction, instruction, and cache figures use one common fixture:

| Quantity | Value |
|---|---|
| adjacency entry | fixture `4 -> 7`; internal `8 -> 18` |
| current / next property-line edge position | `18` / `22` |
| governed request sequence | `19`, one-based |
| true / decoded next-use distance | `4` / `7` requests |
| Scale6 token and word | token `4`; `0x10000012` |
| property address | `0x80000048` |
| property line | `0x80000040`, containing vertices `16..31` |
| deadline / expiry | deadline `26`; UNKNOWN at `27` if not refreshed |

The figures do not report speedup measurements. cache_sim implements Scale6
and supplies cache/traffic evidence. The native gem5 Scale6 request,
retirement and prefetch path remains pending; existing ReuseBind support does
not establish it. Sniper Scale6 rows remain unsupported. State-bit accounting
is not silicon area, and analytic P-OPT matrix traffic does not establish
target-time stream latency.

The prefetch window is explicitly illustrative: first appearances of lines
D, E and F at leads 8, 10 and 13 have future bounds 31, 7 and 15. The actual
C++ selector chooses E at lead 10. Commit FIFOs show 16 slots; prefetch FIFOs
show eight.

## Figure register

| Figure | Visible title | Embedding page |
|---|---|---|
| [`home/home-f01-system-overview.svg`](wiki/home/home-f01-system-overview.svg) | Scale6: future reuse in the existing edge word | [`Home.md`](../wiki/Home.md) Figure 1; repository README overview |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg) | Building Scale6 records in traversal order | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 1 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg) | One 32-bit record, including Twitter-sized IDs | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 2 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg) | From the next use to a conservative deadline | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 3 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg) | Fresh LLC state and Scale6 victim selection | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 4 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f05-lookahead-prefetch.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-lookahead-prefetch.svg) | Selective prefetch from the record stream | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 5 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f06-capacity-accounting.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-capacity-accounting.svg) | P-OPT columns and the LLC capacity budget | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 6 |
| [`risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg) | RISC-V integration: existing path and next step | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 1 |
| [`risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg) | Target Scale6 path through an out-of-order core | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 2 |
| [`risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg) | Request lifetime is not metadata lifetime | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 3 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg) | One edge word, one property line, one update | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 1 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg) | Where Scale6 metadata lives | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 2 |
| [`evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg`](wiki/evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg) | Cache evidence is not a timing or area result | [`Evaluation-Methodology.md`](../wiki/Evaluation-Methodology.md) Figure 1 |

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
