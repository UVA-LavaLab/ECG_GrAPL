# gapbs/ — GAP Benchmark Suite Runtime

Core infrastructure for graph loading, construction, and benchmarking.

## Key Headers

| Header | Role |
|--------|------|
| `builder.h` | Graph construction, reordering dispatch (`GenerateMapping`), relabeling |
| `graph.h` | `CSRGraph` — compressed sparse row graph representation |
| `command_line.h` | CLI argument parsing (`-f`, `-o`, `-n`, `-j`, etc.) |
| `benchmark.h` | Trial runner, timing, verification harness |
| `reader.h` | Graph file I/O (`.sg`, `.el`, `.wel` formats) |
| `pvector.h` | Page-aligned vector (used for node ID arrays) |
| `generator.h` | Synthetic graph generation (RMAT, uniform random) |
| `platform_atomics.h` | Cross-platform atomic operations |
| `sliding_queue.h` | Lock-free sliding-window queue for BFS |

`builder.h` is the main integration point — it contains the top-level variant dispatch
that routes `-o 9:fast` → `GenerateGOrderFastMapping()`, etc.
