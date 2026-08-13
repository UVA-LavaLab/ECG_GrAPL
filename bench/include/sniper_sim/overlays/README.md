# Sniper Overlay Area

GraphBrew Sniper policy/prefetcher overlays will live here before being copied
or patched into the ignored upstream checkout under `snipersim/`.

Expected future layout:

```text
overlays/
  common/core/memory_subsystem/cache/
    cache_set_grasp.*
    cache_set_popt.*
    cache_set_ecg.*
    graph_cache_context_sniper.*
  common/core/memory_subsystem/prefetcher/
    droplet_prefetcher.*
```

Do not edit `snipersim/` directly for tracked GraphBrew logic. Add source files
here and teach `scripts/setup_sniper.py` to copy/patch them when the Sniper cache
extension points are finalized.
