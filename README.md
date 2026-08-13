<p align="center">
  <img src="wiki/assets/ecg-logo.png" alt="ECG graph logo" width="190">
</p>

<h1 align="center">ECG Next</h1>

<p align="center">
  <strong>K2 and StreamShield cache architecture for irregular graph analytics</strong>
</p>

ECG Next carries compact reuse metadata with streamed graph records, binds that
metadata to the corresponding property request, and uses it to guide
last-level-cache replacement and placement.

## Architecture

- **K2 records** encode a reuse tier and the next two property-reuse epochs.
- **K2-M** binds K2 metadata to the exact property request that consumes it.
- **StreamShield** prevents one-touch edge records from occupying the shared
  LLC after a miss while preserving private-cache fills and LLC hits.

The shared implementation covers PageRank, BFS, SSSP, Betweenness Centrality,
and Connected Components.

## Evaluation backends

| Backend | Role |
|---|---|
| **gem5 O3** | Architectural timing and request-bound K2-M behavior |
| **cache_sim** | Functional replacement behavior and total memory traffic |
| **Sniper** | Equal-work, larger-scale cache and traffic trends |
| **RTL models** | Synthesizable K2 metadata and physical-cost components |

Architectural timing is reported from gem5 O3 only. Each backend is compared
with its own matching baseline; absolute miss rates are not compared across
simulators.

## Documentation

- [K2 and StreamShield design](wiki/K2-StreamShield.md) explains the record,
  request path, replacement policy, and worked examples.
- [Evaluation methodology](wiki/Evaluation-Methodology.md) defines workloads,
  baselines, metrics, and simulator roles.
- [Build and reproduction](wiki/Reproduction.md) covers datasets, simulator
  setup, tests, and experiment commands.
- [Repository hygiene](wiki/Repository-Hygiene.md) lists tracked and local-only
  artifacts.

## Repository layout

| Path | Purpose |
|---|---|
| `bench/include/` | Shared policies, metadata, and simulator integration |
| `bench/src_sim/` | Functional cache-simulator graph kernels |
| `bench/src_gem5/` | gem5 graph kernels |
| `bench/src_sniper/` | Sniper graph workload |
| `bench/src_rtl/` | Synthesizable K2 cost models and testbenches |
| `scripts/experiments/ecg/` | Experiment runners, gates, and analysis tools |
| `scripts/test/` | Unit, integration, and public-surface checks |
| `wiki/` | Design, methodology, reproduction guide, and figures |

Generated graphs, binaries, traces, and experiment output are kept under
`results/` and are not tracked. Performance tables will be published only
after the final evaluation is complete.
