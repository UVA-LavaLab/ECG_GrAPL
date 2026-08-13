# Sniper Cache Policy Overlays

This directory will hold GraphBrew Sniper cache-set implementations.

Planned files:

```text
cache_set_grasp.{h,cc}
cache_set_popt.{h,cc}
cache_set_ecg.{h,cc}
graph_cache_context_sniper.{h,cc}
```

`graph_cache_context_sniper.{h,cc}` now exists as a tracked overlay scaffold. It
loads the current GraphBrew sideband JSON and P-OPT matrix format, tracks
per-core current-vertex hints, classifies property/edge ranges, and exposes the
same helper surface that the first GRASP/POPT/ECG cache-set ports will need.

`cache_set_grasp.{h,cc}` now exists as the first policy scaffold. It keeps SRRIP
victim aging, adds GRASP high/moderate/low insertion and hot-hit promotion, and
exposes `prepareInsertion(addr)`. Sniper's current `CacheSet::insert()` does not
pass the fill address, so the factory/insertion wiring must call that hook from
`Cache::insertSingleLine()` before this policy matches the reference behavior.

Integration target discovered in Sniper main:

```text
snipersim/common/core/memory_subsystem/cache/cache_set.h
snipersim/common/core/memory_subsystem/cache/cache_set_srrip.{h,cc}
snipersim/common/core/memory_subsystem/cache/cache_base.{h,cc}
```

The first integration patch should wire `cache_set_grasp` into `CacheSet` and
feed `prepareInsertion(addr)`, then use `ECG_DBG_ONLY` as the parity check.

`cache_set_popt.{h,cc}` is also scaffolded. It loads the same sideband and
P-OPT matrix files as gem5, uses P-OPT distance for all-property candidate sets,
evicts non-property data first for mixed sets to match upstream P-OPT, and falls
back to SRRIP when the matrix is absent. It is not yet wired into the Sniper
factory.

`cache_set_ecg.{h,cc}` supports `SNIPER_ECG_MODE=DBG_ONLY`, `DBG_PRIMARY`,
`POPT_PRIMARY`, `ECG_EMBEDDED`, and `ECG_COMBINED`. The active smoke coverage
targets DBG-only GRASP parity and P-OPT-primary oracle parity first.

`roi_matrix.py --suite sniper` currently aliases exact parity labels to the
already validated policies: `ECG_DBG_ONLY` runs Sniper `grasp`, and
`ECG_POPT_PRIMARY` runs Sniper `popt`. The `ecg` policy is used for hybrid modes
such as `ECG_DBG_PRIMARY`.
