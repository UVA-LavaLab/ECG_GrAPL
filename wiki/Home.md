<p align="center">
  <img src="assets/logo.png" alt="ECG graph logo" width="180">
</p>

# ECG: edge-carried reuse

A fixed graph traversal exposes future property-line uses that a cache cannot
infer from recency alone. ECG puts a compact description of that reuse into
the spare bits of the edge word, carries it with the corresponding load, and
uses it to rank cache lines or select prefetch candidates. The current
**REF32** family keeps the structural record at four bytes and leaves the
algorithm's property values unchanged.

**Scale6 is one encoding, not the architecture's name.** Twitter needs 26
vertex-ID bits and leaves six metadata bits. Smaller graphs can use the
richer **Full14** encoding: default eight reference bits, two state bits and
four action bits. The implementation supports explicit encoding choices,
not automatic use of every spare bit; the native decoder currently implements
only the fixed 26+6 ABI.

### Figure 1 — ECG: graph knowledge in the edge stream

![One ECG access traced from graph vertex 8 and CSR position 18 through alternative Full14 and Scale6 records, unchanged property data, retirement metadata delivery, and a different cache victim](../fig/wiki/home/home-f01-system-overview.svg)

**Figure 1.** Outer vertex `u=8` reads property `v=18` from CSR position `j=18`.
The next use of that property's cache line is at `j=22`. A richer mask and a
compact token encode the same distance with different precision. Both recover
the same address and value. The cache example then shows why retaining the
most recently touched line is not always the right choice.

The example is PageRank pull: the **outer vertex** traverses in-neighbors
`N_in(u)`, and the **property vertex** contributes to destinations in
`N_out(v)`. Its governed access count is `d_out(v)`. Other kernels need metadata
for their actual request order; changing to out-neighbors `N_out(u)` changes
that count to `d_in(v)`. A dynamic frontier is not the same stream as a fixed
PageRank sweep.

## Read the design as a sequence of transformations

1. [Records and cache control](ReusePlan-FlowThrough) starts with the graph,
   constructs CSR-aligned masks, compares bit budgets, and works through an
   actual victim-ranking example.
2. [One edge, end to end](Property-to-Cache-Walkthrough) derives the hex words,
   address, returned F32 value, future bounds and storage ownership.
3. [Native processor pipeline](RISC-V-Instruction-Path) follows both real loads
   through rename, issue, translation, private caches, the ROB, and the
   retirement-only metadata channel.
4. [Evaluation methodology](Evaluation-Methodology) separates functional
   cache results, total traffic, native execution and physical cost.
5. [Reproduction](Reproduction) provides the corresponding build, experiment
   and bounded native-probe commands.

## Implementation and evidence

cache_sim implements Full14 and Scale6 replacement, commit refresh and
LLC-only prefetching. Full Twitter-2010 results use Scale6; they are cache and
traffic evidence, not processor-speedup measurements. The 8 MiB LLC remains
the primary target, with larger capacity points reported separately.

The native RV64 O3 path implements real Scale6 record/F32 loads and
retirement-driven LLC replacement. Native prefetch, production timing
admission and physical-area qualification remain unfinished. Earlier
ReusePlan/ReuseBind and FlowThrough mechanisms are retained as separate
controls, not silently included in the current result.
