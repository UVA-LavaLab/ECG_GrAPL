# Cagra / P-OPT Partition Helpers (bench/include/graphbrew/partition/cagra)

Purpose: Cagra/GraphIT-style CSR slicing and partitioning used by `builder.h` when `-j` specifies type `0`.

Key headers:
- `popt.h`
  - `graphSlicer` — slice CSR by vertex range
  - `MakeCagraPartitionedGraph` — build p_n × p_m partitions (honors `outDegree`, CLI `-z`)

Replaces legacy aliases:
- `bench/include/graphbrew/cache/popt.h` (removed)
- `bench/include/graphbrew/partitioning/popt.h` (now here)
