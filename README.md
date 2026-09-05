<p align="center"><img src="wiki/assets/logo.png" alt="ECG graph logo" width="180"></p>

# ECG

**Scale6: edge-carried future reuse for graph-cache management**

ECG studies whether compact, graph-derived reuse information can improve cache
behavior without a runtime P-OPT rereference matrix. The current candidate is
**REF32 Scale6**: a 26-bit property vertex and a six-bit future token packed into
the existing four-byte edge word. It combines line-local LLC predictions,
bounded commit updates that include private-cache hits, and selective LLC-only
prefetching.

The current evidence is **functional cache behavior and traffic in cache_sim**,
including full Twitter-2010. The native Scale6 gem5 path and physical
area/timing qualification are not complete. Earlier ReusePlan, ReuseBind and
FlowThrough implementations remain in the repository as separate mechanisms
and controls; their implementation status is not evidence for a completed
Scale6 port.

## Architecture overview

![Scale6 architecture separating in-place four-byte record construction, ordinary property accesses, private-hit commit refresh, bounded LLC prefetching, and the unfinished native timing path](fig/wiki/home/home-f01-system-overview.svg)

The current design keeps four boundaries explicit:

1. **Offline construction:** a traversal-specific pass encodes the next
   property-line use in the in-edge CSR. The directed in-place builder uses
   two arrays indexed by property line rather than extra arrays indexed by edge.
2. **One ordinary-width record:** destination and future token share 32 bits.
   The token distinguishes unknown, dead, finite-current and next-traversal
   reuse. It carries a logarithmic distance class, not an exact future position.
3. **Fresh, bounded LLC state:** a 16-entry commit-update channel closes the
   private-hit freshness gap. A 16-record lookahead feeds an eight-entry,
   LLC-only prefetch queue. Model latency is in governed requests, not cycles.
4. **Explicit cost:** Scale6 keeps the LLC data ways, but its added
   per-line/controller state is not free. On-chip state, reserved LLC capacity,
   and a backing matrix in DRAM are separate cost domains.

For PageRank pull, the **outer vertex** `u` traverses in-neighbors `N_in(u)`;
the **property vertex** `v` is read for destinations in `N_out(v)`. Its access
count is `d_out(v)`. An out-neighbor traversal over `N_out(u)` instead has
access count `d_in(v)`. Reuse metadata must follow the traversal actually used.

The primary Scale6 comparisons use **FlowThrough off**. Optional structural
no-allocation controls must be applied symmetrically; they are not silently
included in the replacement or prefetch claim.

## Evidence boundary

| Surface | Current role and limitation |
|---|---|
| **cache_sim** | Scale6 functional results, demand LLC misses, and off-chip reads plus writes |
| **gem5 O3** | Native timing backend; existing legacy ReuseBind path, but Scale6 delivery/commit/prefetch is pending |
| **Sniper** | Legacy matched-work modeled corroboration; Scale6 rows remain unsupported |
| **RTL models** | Earlier metadata/cost components; not a completed Scale6 physical implementation |

Only a supported native timing path can establish architectural speedup.
Request-count latency in cache_sim cannot be reinterpreted as processor cycles.
Likewise, total metadata-footprint ratios against P-OPT's complete DRAM matrix
are not silicon-area reductions.

Charged P-OPT reserves enough ways for its active columns at each graph/cache
size. `POPT_SE` and `POPT_SE_DISTANT` reconstruct the paper's one-column format
under two disclosed interpretations of an unspecified case. Both retain the
full backing-matrix stream charge; neither is a fixed-two-way undercharge of
ordinary P-OPT.

## Documentation

- [Scale6 records and cache control](wiki/ReusePlan-FlowThrough.md)
- [RISC-V integration: existing support and Scale6 target](wiki/RISC-V-Instruction-Path.md)
- [A checked edge-to-cache example](wiki/Property-to-Cache-Walkthrough.md)
- [Evaluation methodology and results](wiki/Evaluation-Methodology.md)
- [Related work](wiki/Related-Work.md)
- [Build and reproduction](wiki/Reproduction.md)
- [Repository hygiene](wiki/Repository-Hygiene.md)

## Repository layout

| Path | Purpose |
|---|---|
| `bench/include/` | shared record, policy, ISA and simulator integration |
| `bench/src_sim/` | functional cache-simulator graph kernels |
| `bench/src_gem5/` | gem5 graph kernels and legacy request-bound delivery |
| `bench/src_sniper/` | Sniper graph workload |
| `bench/src_rtl/` | existing ReusePlan cost models |
| `scripts/experiments/ecg/` | experiment runners and fail-closed gates |
| `scripts/docs/` and `fig/` | deterministic SVG figures and editable Draw.io mirrors |
| `wiki/` | architecture, evidence, and reproduction guides |

Experiment output remains under `results/` and is not tracked. Wiki and
conference-paper figures have separate generators and layouts; changing a
wiki plate never compresses or overwrites its paper counterpart.
