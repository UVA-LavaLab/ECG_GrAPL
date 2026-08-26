# ECG wiki figures

This directory contains ECG's published SVG figures and editable Draw.io
mirrors. This file records the repository's generated-mode contract, visual
tokens, provenance, and figure registration.

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

## Visual roles

| Role | Strong | Matte |
|---|---|---|
| ink / border | `#27313A` | `#FFFFFF` |
| neutral / annotation | `#9AA3AD` | `#F8F6EC` |
| graph data / request data | `#1769C2` | `#EDF5FF` |
| local compute / accepted state | `#15803D` | `#E7F7EA` |
| transfer / FlowThrough | `#B45309` | `#FFF0D8` |
| verification / rejection | `#B42318` | `#F7DEDC` |
| policy / metadata state | `#6D5BD0` | `#EEE9FF` |

## Value and claim ownership

The only fixture-backed concrete mechanism example is
`fig/ecg-figure-fixture.json`. The generator derives its adjacency rows,
degree-based property-access counts, tier, subsequent-access epochs, record
width, property address, cache line, and circular distances. Tests
independently recompute the same values.

Architecture vocabulary and values trace to:

- `bench/include/ecg_reuse_plan_builder.h` for records, tiers, epochs, and
  weighted formats;
- `bench/include/ecg_victim_policy.h` for line state, distance, insertion, and
  victim selection;
- `bench/include/gem5_sim/overlays/arch/riscv/` and
  `bench/include/gem5_sim/gem5_harness.h` for instruction roles;
- `bench/include/gem5_sim/overlays/mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh`
  and `bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch` for
  request and MSHR state;
- `bench/include/gem5_sim/overlays/mem/cache/base_flowthrough.patch` for
  allocation behavior; and
- `scripts/experiments/ecg/roi_matrix.py` for simulator roles and fail-closed
  row validation.

## Cross-layer callouts

The checked end-to-end plates keep one tracked entity and stable callout IDs:

| Callout | Meaning |
|---|---|
| `A` | tracked adjacency entry `4 -> 7`, mapped to internal entry `8 -> 18` |
| `B` | edge-aligned compact ReusePlan for that edge |
| `C` | renamed ReusePlan operand and dependent property instruction |
| `D` | record/property Request lanes through the cache hierarchy |
| `E` | governed LLC line `0x80000040` and its reuse timeline |

The figures describe implemented and modeled architecture. They do not report
performance measurements. gem5 O3 is the architectural timing authority;
cache_sim is functional/traffic evidence; Sniper is modeled cache/traffic
direction evidence. Analytic P-OPT timing remains an optimistic bound.

## Figure register

| Figure | Visible title | Embedding page |
|---|---|---|
| [`home/home-f01-system-overview.svg`](wiki/home/home-f01-system-overview.svg) | ECG Next: offline guidance to request-bound LLC state | [`Home.md`](../wiki/Home.md) Figure 1; repository README overview |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg) | Degree and traversal analysis for one edge-aligned ReusePlan | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 1 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg) | ReusePlan wire formats and traffic overhead | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 2 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg) | Cache-line access schedule and circular reuse distance | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 3 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg) | LLC metadata lifecycle and rrip_first victim pipeline | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 4 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f05-flowthrough-outcomes.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-flowthrough-outcomes.svg) | FlowThrough changes LLC allocation, not lookup or service | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 5 |
| [`reuse-plan-flowthrough/reuse-plan-flowthrough-f06-structural-fairness.svg`](wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-structural-fairness.svg) | Design FlowThrough and symmetric structural fairness | [`ReusePlan-FlowThrough.md`](../wiki/ReusePlan-FlowThrough.md) Figure 6 |
| [`risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f01-instruction-family.svg) | Experimental RISC-V instruction roles and operand contracts | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 1 |
| [`risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f02-o3-request-pipeline.svg) | Two ECG loads through gem5 O3 and the cache hierarchy | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 2 |
| [`risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg`](wiki/risc-v-instruction-path/risc-v-instruction-path-f03-mshr-metadata-lifecycle.svg) | ReuseBind across MSHR merge, fill, and invalidation | [`RISC-V-Instruction-Path.md`](../wiki/RISC-V-Instruction-Path.md) Figure 3 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg) | Checked request: adjacency 4 -> 7 maps to property 18 | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 1 |
| [`property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg`](wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg) | ECG state placement in the processor and cache hierarchy | [`Property-to-Cache-Walkthrough.md`](../wiki/Property-to-Cache-Walkthrough.md) Figure 2 |
| [`evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg`](wiki/evaluation-methodology/evaluation-methodology-f01-evidence-boundary.svg) | Evidence boundary for ECG architecture claims | [`Evaluation-Methodology.md`](../wiki/Evaluation-Methodology.md) Figure 1 |

## Legacy-asset disposition

Every prior hand-maintained SVG was replaced, merged, or retired:

| Prior asset | Disposition |
|---|---|
| `ecg-architecture-summary.svg` | replaced by the Home overview and architecture state map |
| `ecg-detailed-architecture.svg` | replaced by the O3 pipeline, MSHR lifecycle, and architecture state map |
| `flowthrough-path.svg` | replaced by Figure 5, including mixed-MSHR and derived-prefetch behavior |
| `graph-to-csr-reuseplan.svg` | merged into the checked offline-construction plate |
| `property-request-walkthrough.svg` | replaced by the checked source-4-to-7/internal-8-to-18 end-to-end example |
| `reuse-plan-cpu-pipeline.svg` | replaced by the 1200 px O3 request pipeline |
| `reuse-plan-example.svg` | replaced by the checked property-line reuse timeline |
| `reuse-plan-overview.svg` | replaced by the Home system overview |
| `reuse-plan-record.svg` | replaced by the unweighted, compact, and weighted format plate |
| `reuseplan-construction.svg` | merged into the checked offline-construction plate |
| `riscv-basic-flow.svg` | merged into the instruction-role and O3 pipeline plates |
| `riscv-instruction-family.svg` | replaced by the instruction-role plate |
| `logo.svg` | retired; the referenced `wiki/assets/logo.png` remains the logo |
