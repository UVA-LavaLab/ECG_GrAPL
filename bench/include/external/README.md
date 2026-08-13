# bench/include/external/ — Bundled External Libraries

Third-party libraries vendored into the project (header-only).

| Directory | Library | Purpose |
|-----------|---------|---------|
| `gapbs/` | [GAP Benchmark Suite](http://gap.cs.berkeley.edu/benchmark.html) | Core runtime — graph representation, builder, CLI, benchmark harness |
| `rabbit/` | [RabbitOrder](https://github.com/araij/rabbit_order) | Community-based graph ordering via hierarchical clustering |
| `gorder/` | [GOrder](https://github.com/datourat/Gorder) | GoGraph baseline — graph ordering by sliding window scoring |
| `corder/` | COrder | Cache-aware graph ordering |
| `leiden/` | [GVE-Leiden](https://github.com/puzzlef/leiden-communities-openmp) | Leiden community detection (OpenMP) |

GraphBrew's CSR-native reimplementations of RabbitOrder, GOrder, and RCM live in
`bench/include/graphbrew/reorder/` — these external copies are the original reference implementations.
