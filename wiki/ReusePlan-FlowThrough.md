# Scale6 records and cache control

REF32 Scale6 is the current ECG cache-quality candidate. This page retains its
existing URL, but the main mechanism is no longer the earlier two-epoch
ReusePlan plus FlowThrough design. Historical mechanisms remain separate
controls; the primary Scale6 comparisons have FlowThrough off.

## 1. Construction follows the actual traversal

PageRank pull traverses the **in-neighbors** `N_in(u)` of an **outer vertex**.
Each **property vertex** `v` is therefore read for destinations in `N_out(v)`;
its access count is `d_out(v)`. A kernel traversing **out-neighbors**
`N_out(u)` has access count `d_in(v)` instead. The offline metadata must
describe the request stream the kernel actually executes.

Scale6 predicts the next access to a **property cache line**, not merely the
next access to the same vertex. With four-byte properties and 64-byte lines,
16 neighboring property values share one prediction domain.

### Figure 1 — Building Scale6 records in traversal order

![Checked graph and flattened PageRank request stream deriving edge positions 18 and 22, their shared property line, distance four, and the in-place Scale6 record built with two line-indexed position arrays](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f01-offline-construction.svg)

**Figure 1.** Fixture adjacency `4 -> 7` maps to internal row `8` and property
vertex `18`. The corresponding request is at edge position `18`; the next
access to its property line is at position `22`. The distance is four requests,
not the difference between two outer-vertex IDs.

The fixture is undirected, so its incoming and outgoing neighbor lists agree.
The production Twitter path is directed: its in-place builder requires
distinct in/out CSR arrays. A forward pass records the first position of each
property line; a reverse pass uses the next position to encode the edge word.
Two 64-bit position arrays cost about 39.7 MiB on Twitter. They are temporary
preprocessing state, not edge-sized runtime sidecars.

## 2. The record stays four bytes

### Figure 2 — One 32-bit record, including Twitter-sized IDs

![Scale6 bit layout with token in bits 31 through 26, property vertex in bits 25 through zero, the complete six-bit token alphabet, and the distinction from the retained n18 Full14 format](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f02-record-formats.svg)

**Figure 2.** Twitter requires 26 ID bits. The six remaining bits represent
state and a logarithmic distance class. No bits are reserved for a prefetch
action; candidates are derived from the record stream.

| Token | Interpretation |
|---|---|
| `0` | unknown prediction |
| `1` | known dead: no remaining governed use |
| `2..32` | finite-current distance bucket `token - 2` |
| `33..63` | next-traversal distance bucket `token - 33` |

The bucket is `floor(log2(distance))`; decoding uses its upper bound,
`2^(bucket + 1) - 1`, capped by the supported finite range. Encoding happens
offline. Hardware decoding needs range checks, a decoder/shift and an adder,
not a runtime logarithm.

The checked word is `0x10000012`: token `4` in the high six bits, vertex `18`
in the low 26. The figure forces the 26-bit format even though its tiny
fixture could use fewer ID bits. The retained n18 Full14 format instead
allocates 18 destination, 8 reference, 2 state and 4 action bits. Their
precision and prefetch delivery must not be conflated.

## 3. Quantization and expiry

### Figure 3 — From the next use to a conservative deadline

![Request-sequence timeline showing current sequence 19, actual next property-line use at 23, logarithmic upper-bound deadline 26, and safe transition to unknown at 27 rather than falsely declaring the line dead](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f03-future-distance.svg)

**Figure 3.** A true distance of four decodes to seven. With the one-based
request sequence `19`, the candidate deadline is `26`; the actual next line
access occurs at `23`. Holding that prediction fixed, it expires at `27`.
Actual accesses can refresh the prediction before then.

Expiry becomes **UNKNOWN**, never DEAD. WRAP tokens describe next-traversal
reuse; the requested final traversal cannot assume a further pass. The
32-bit deadline uses bounded modular arithmetic. These coordinates are
governed requests, not CPU cycles.

## 4. Keep resident predictions fresh

### Figure 4 — Fresh LLC state and Scale6 victim selection

![Private-hit freshness path with a 16-entry commit queue, resident and expiry checks, 35 bits of added LLC line state, and the Scale6 candidate classes and score-based tie ordering](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f04-llc-policy-pipeline.svg)

**Figure 4.** Demand LLC hits/fills can stamp the request's prediction. Private
hits also generate modeled commit refreshes, preventing the LLC from retaining
only the older miss-stream view. The channel has 16 entries, eight governed
requests of delay and at most one applied update per request. It coalesces by
property line and never allocates data for a metadata update.

An expired update is discarded; a nonresident target does not cause a fill.
Each LLC line has 35 added bits: a 32-bit deadline, two future-state bits and
one prefetch-origin bit. Ordinary tags, data, RRPV and recency remain separate.

On replacement, invalid ways fill first. Known-dead governed misses bypass
LLC allocation. Among resident victim candidates, known-dead properties
precede non-property lines, followed by a **shared property category**:
finite predictions use `distanceRRPV(remaining)`; unknown predictions use the
maximum of RRPV and the local GRASP fallback. Ties compare unknown status,
remaining distance, tier and recency. Unknown does not unconditionally
precede finite, and this is not the old RRIP-first ReusePlan ordering.

These are cache_sim rules. The native replacement-only path instead marks
request observations PENDING and installs predictions only on timed
retirement delivery. Its queue uses CPU cycles, explicit burst-capture
width and one-wide output; speculative known-dead miss bypass is not
implemented. See [native integration](RISC-V-Instruction-Path).

## 5. Selective prefetch is a separate mechanism

### Figure 5 — Selective prefetch from the record stream

![Sixteen-record lookahead selecting first appearances of distinct property lines at leads eight through fifteen, ranking their future bounds, filtering duplicates and admission, and filling only the LLC through an eight-entry queue](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f05-lookahead-prefetch.svg)

**Figure 5.** The illustrative window is separate from the checked graph.
Candidate lines must differ from the current line and must not have appeared
earlier in the lookahead. The best decoded future bound wins; ties favor
lead ten. Resident, pending and admission checks precede enqueueing.

The modeled queue has eight entries, an eight-request delay and at most one
issue per eight governed requests. Fills target the LLC only. A lower demand
miss count is not necessarily lower traffic: prefetch reads and dirty
writebacks must also be counted.

## 6. Capacity and hardware are different budgets

### Figure 6 — P-OPT columns and the LLC capacity budget

![Twitter P-OPT backing matrix and active columns, with sixteen-way capacity bars showing full and single-epoch reservations at 8, 16 and 24 MiB and a separate disclosure of Scale6's added on-chip metadata cost](../fig/wiki/reuse-plan-flowthrough/reuse-plan-flowthrough-f06-capacity-accounting.svg)

**Figure 6.** Twitter's current/next columns total about 4.97 MiB. Full P-OPT
therefore reserves 10, 5 and 4 ways at 8, 16 and 24 MiB; one-column SE
reserves 5, 3 and 2. Both keep the same complete 256-column backing matrix and
cumulative traversal stream. The two SE reconstructions are disclosed in
[Evaluation methodology](Evaluation-Methodology#52-single-epoch-p-opt-reconstruction).

Scale6 retains the LLC data ways but adds metadata/controller state: about
560 KiB at an 8 MiB LLC. That is not yet an equal-area result. Ratios against
P-OPT's complete DRAM matrix describe total metadata footprint, not silicon.

## Legacy FlowThrough controls

The primary runs use `--flowthrough off`. A separate `--flowthrough all`
control applies the same no-allocate opportunity to each policy's actual
structural carrier. Ordinary property requests are not FlowThrough requests.
MSHR `allocOnFill` combines with OR, so a non-allocating target cannot suppress
another target's required fill. These controls do not establish a Scale6
replacement or timing result.

## Implementation sources

| Surface | Source |
|---|---|
| token, quantizer, builders and victim rank | `bench/include/ecg_ref32.h` |
| record setup and runtime context | `bench/include/cache_sim/graph_cache_context.h` |
| update/prefetch queues and accounting | `bench/include/cache_sim/cache_sim.h` |
| PageRank record consumption | `bench/src_sim/pr.cc` |
| supported policies and evidence gates | `scripts/experiments/ecg/roi_matrix.py` |
