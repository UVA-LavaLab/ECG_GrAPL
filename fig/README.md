# ECG wiki figures

This directory contains ECG's published SVG figures and matching Draw.io
mirrors. This file records the deterministic generation contract, visual roles,
provenance, and page registration.

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
| transfer / FlowThrough | `#C56A13` | `#FFF7ED` |
| verification / rejection | `#C63C4A` | `#FFF1F2` |
| policy / metadata state | `#6558C5` | `#F5F3FF` |

Visual styling uses dark navy headings, thin slate borders, mostly white or
near-white surfaces, and one or two semantic accents per local flow. Full-panel
fills are not the primary hierarchy cue.

Instruction and pseudocode text uses the same semantic colors: record and
FlowThrough mnemonics are amber, property-load mnemonics are green, register
names and ReusePlan operands are purple, addresses/data are blue, and rejected
or invalid state is red. SVG and Draw.io mirrors retain this token-level
highlighting.

## Sources and evidence scope

The fixture-backed mechanism example is `fig/ecg-figure-fixture.json`. The
generator derives its adjacency rows, degree-based property-request counts,
tier, subsequent-access epochs, record width, property address, cache line, and
circular distances. Tests independently recompute the same values.

Architecture vocabulary and values trace to:

- `bench/include/ecg_reuse_plan_builder.h` for records, tiers, epochs, and
  weighted formats;
- `bench/include/ecg_victim_policy.h` for line state, distance, insertion, and
  victim selection;
- `bench/include/gem5_sim/overlays/arch/riscv/` and
  `bench/include/gem5_sim/gem5_harness.h` for instruction roles;
- `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh`
  and `bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch` for
  Request and MSHR state;
- `bench/include/gem5_sim/overlays/mem/cache/base_flowthrough.patch` for
  allocation behavior; and
- `scripts/experiments/ecg/roi_matrix.py` for simulator roles and fail-closed
  row validation.

## Figure fixture

The construction, instruction, and cache figures use one common fixture:

| Quantity | Value |
|---|---|
| adjacency entry | fixture `4 -> 7`; internal `8 -> 18` |
| compact ReusePlan | destination `18`, tier `T1`, epochs `11` and `15` |
| property address | `0x80000048` |
| property line | `0x80000040`, containing vertices `16..31` |
| current and nearest reuse distance | epoch `8`; `min(3, 7) = 3` |

The figures describe implemented and modeled architecture. They do not report
performance measurements. gem5 O3 provides architectural timing evidence;
cache_sim provides functional cache/traffic evidence; Sniper provides
matched-work modeled cache/traffic evidence. Analytic P-OPT timing remains an
optimistic lower bound because target-time lookup latency, matrix-stream
latency, bandwidth, queueing, and contention are omitted.

## Figure register

| Figure | Visible title | Embedding page |
|---|---|---|
| [`home/home-f01-system-overview.svg`](wiki/home/home-f01-system-overview.svg) | ECG dataflow from graph preprocessing to LLC replacement | [`Home.md`](../wiki/Home.md) Figure 1; repository README overview |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg) | Constructing an edge-aligned ReusePlan | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 1 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg) | ReusePlan record formats and structural traffic | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 2 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg) | Quantized next-reference distance for one property line | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 3 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg) | ReuseBind acceptance and RRIP-first victim selection | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 4 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f05-flowthrough-outcomes.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-flowthrough-outcomes.svg) | FlowThrough lookup, service, and LLC fill allocation | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 5 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f06-structural-fairness.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-structural-fairness.svg) | FlowThrough mechanism and matched structural-array control | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 6 |
| [`risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg) | RISC-V record-load and property-load instruction roles | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 1 |
| [`risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg) | ReusePlan loads in an out-of-order core | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 2 |
| [`risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg) | ReuseBind merge, response, and line-metadata lifetime | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 3 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg) | From adjacency entry 4 -> 7 to LLC line 0x80000040 | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 1 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg) | ReusePlan state placement across software, core, and LLC | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 2 |
| [`evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg`](wiki/evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg) | Evaluation evidence and admissible claims | [`Evaluation-Methodology.md`](../wiki/Evaluation-Methodology.md) Figure 1 |
