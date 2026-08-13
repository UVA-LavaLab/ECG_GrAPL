# Sniper Prefetcher Overlays

This directory documents GraphBrew Sniper graph-prefetcher overlays. Sniper's
active prefetcher factory lives in `parametric_dram_directory_msi`, so tracked
prefetcher implementations follow that upstream path even though this README
keeps the prefetch work grouped conceptually.

Tracked implementations:

```text
common/core/memory_subsystem/parametric_dram_directory_msi/droplet_prefetcher.{h,cc}
common/core/memory_subsystem/parametric_dram_directory_msi/ecg_pfx_prefetcher.{h,cc}
```

The Sniper DROPLET path must match the active gem5 DROPLET semantics:

- use actual CSR edge shadow data,
- detect edge-stream cache lines,
- resolve future neighbor IDs to property-region prefetch addresses,
- expose issued/useful/late/unused counters when possible.

Reference audit against the old public DROPLET Sniper-6.1 repo
(Basak et al., 2019):

- the repo is a partial Sniper tree (`common/` and `config/` only), not a clean
	runnable baseline by itself;
- `README.md` identifies the source work as "Analysis and Optimization of the Memory
	Hierarchy for Graph Processing Workloads" and says the workloads were GAPBS;
- `dropletL1.{h,cc}` documents the intended split between structure-demand
	streaming and property-address training, but `trainPrefetcherForProperty()` is
	empty and `prefetcher.cc` does not instantiate `DROPLETL1`;
- `prefetcher.cfg` lists `simple`, `ghb`, `stream`, `graph_stream`,
	`baseline_stream`, and `address_prefetch`, but the prefetcher selection lines
	are commented out in the checked-in config;
- `AddressPrefetcher` and `MemoryManagerBase` use `StrucAddress.txt` and
	`propAddress.txt` to identify CSR structure and property ranges, then compute
	property prefetch addresses from neighbor IDs read out of structure cache
	lines;
- `DramCntlrInterface` contains a memory-side property-prefetch queue for
	structure-prefetch responses, while `CacheCntlr::trainL1PropertyPrefetcher()`
	has a cache-side path that manually maps structure IDs to property lines.
- some old property-address paths use bitmap-style indexing (`vertex / 64`) for
	property arrays, while GraphBrew sidebands describe each exported property
	region with a concrete element size and address `base + vertex * elem_size`.

GraphBrew's tracked `droplet_prefetcher` intentionally ports the usable part of
that design into the modern overlay style: sideband metadata replaces the old
text files, edge shadow data replaces direct simulator dereferences of guest
addresses, and the active prefetcher emits both edge-stream and indirect
property prefetches through Sniper's normal prefetcher API. Treat this as a
DROPLET-style port until the original parameters and memory-side queue behavior
are fully matched or explicitly documented as modeling differences.

Artifact-informed defaults used by `roi_matrix.py`, gem5, and Sniper overlays:

```text
droplet_prefetch_degree = 1       # one edge-stream line per trigger
droplet_indirect_degree = 16      # one 64B cache line of 4B neighbor IDs
droplet_stride_table_size = 64    # artifact config stream count
```

The gem5 and Sniper ports both issue indirect property prefetches from the
current edge cache line and from predicted future edge lines. This matches the
artifact intent that structure cache-line data trains property prefetches. The
remaining explicit modeling difference is that GraphBrew issues these through
the normal prefetcher API, while the old Sniper artifact also had a partial
memory-side property-prefetch queue in `DramCntlrInterface`.

Current decision: do not port the old `DramCntlrInterface` changes for the
claim-oriented baseline yet. They are invasive, tied to the old Sniper-6.1
message flow and ad hoc `StrucAddress.txt`/`propAddress.txt` files, and mainly
model memory-side scheduling of property prefetches after structure-prefetch
responses. The core DROPLET mechanism we need for comparison is already covered
by the modern prefetcher API path: edge-stream detection, neighbor-ID lookup,
property-address generation, queue insertion, and useful/unused prefetch
counters. Revisit a DRAM-side mode only if we decide to reproduce the old
artifact's memory-controller scheduling exactly rather than running a
DROPLET-style graph prefetch baseline.

The old DRAM-side path would add overhead beyond the property prefetches
themselves:

- per-core memory-side property-prefetch queues and a priority list,
- queue eviction bookkeeping when `MAX_PREFETCH_LIST_SIZE` is exceeded,
- extra `DRAM_PREFETCH_REQ`, `TAG_CHECK`, `TAG_CHECK_REP`, and
	`DRAM_PREFETCH_REP` messages,
- tag checks before issuing a memory-side property prefetch,
- DRAM read latency and bandwidth pressure for every accepted property prefetch,
- explicit staggered scheduling with `DRAM_PREFETCH_INTERVAL`,
- simulator-side pointer dereferences of structure cache-line contents and
	property-address calculations,
- possible queue interference across cores when the priority list is non-empty.

Those costs are useful if the goal is to reproduce the old artifact's memory-side
prefetch engine. They are not required to evaluate the cleaner hardware-facing
DROPLET mechanism, and mixing them into the current normal-prefetcher path would
make the baseline less comparable to gem5 and harder to debug.

The Sniper ECG_PFX path is separate from DROPLET:

- benchmarks emit `SNIPER_ECG_PFX_TARGET(vertex)` hints,
- `scripts/setup_sniper.py --apply-overlays` patches `magic_server.cc` so the
	`GPFX` SimUser command stores per-core prefetch targets,
- `ecg_pfx_prefetcher` consumes those stored targets,
- targets are resolved to the exported property-region sideband,
- recent cache lines are deduplicated,
- `ecg-pfx-prefetcher.*` counters report sideband load, hints seen, requests
	issued, duplicate skips, and invalid/no-sideband cases.

Timing caveat: current Sniper ECG_PFX uses explicit benchmark-emitted
`SNIPER_ECG_PFX_TARGET` hints. That is a prototype delivery path for validating
precision and cache behavior; it is not the final instruction-carried metadata
model. `roi_matrix.py` marks these rows as
`timing_model=prototype_explicit_hint_delivery` and
`timing_valid_for_speedup=0`, and `aggregate_results.py` suppresses speedup metrics
for them while preserving cache/prefetch metrics.

To reduce prototype overcharge, the Sniper harness filters recent duplicate PFX
targets before calling `SimUser`. Configure this with
`--ecg-pfx-hint-filter` / `SNIPER_ECG_PFX_HINT_FILTER`; the default capacity is
`16`, and `0` disables filtering. This is still an explicit-hint prototype, but
it is closer to a hardware prefetch-target filter than forwarding every rejected
candidate to the simulator.

Do not use this path for final claims until those counters prove the prefetcher
is active on the relevant graph/benchmark rows.

Current validated status:

- `scripts/setup_sniper.py --apply-overlays` wires `prefetcher = droplet` and
	`prefetcher = ecg_pfx` into `Prefetcher::createPrefetcher()`.
- `roi_matrix.py --suite sniper --prefetcher DROPLET` and
	`--prefetcher ECG_PFX` attach the selected prefetcher to `l2` by default or
	`l1d` when requested.
- `sniper_sift_ecg_pfx_smoke` validates synthetic PR/BFS/SSSP ECG_PFX rows.
- `sniper_sift_file_ecg_pfx_smoke` validates email-Eu-core PR/BFS/SSSP ECG_PFX
	rows using lookahead 4 for PR/BFS and lookahead 0 for SSSP.
