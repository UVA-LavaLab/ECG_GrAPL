# Cache Simulation (bench/include/cache_sim)

Purpose: L1/L2/L3 cache simulation core used by `bench/bin_sim/*`.

Key headers:
- `cache_sim.h` — Cache simulator core, eviction policies
- `graph_sim.h` — Graph wrappers/instrumentation for cache simulation

Replaces legacy aliases:
- `bench/include/cache/cache_sim.h` (removed)
- `bench/include/cache/graph_sim.h` (removed)
