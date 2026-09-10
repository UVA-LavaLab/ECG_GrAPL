# Costed cache-only potential-window prototype

This opt-in prototype evaluates the scalar top-down BFS window contract
with real encoded records and resident cache metadata. It is separate from
the [read-only diagnostic](Potential-Window-Diagnostics), whose side table
and shadow accesses cannot be treated as an active implementation.

The default remains `--record-model next`. The new interface is:

```text
--record-model window --record-base-policy GRASP_PAPER
--mode transport|replacement --window-candidate-rrpv 6|7
```

Only unweighted, nonempty `.sg` inputs and sorted top-down BFS are admitted.
Other algorithms, BU, prefetch modes, nonzero mantissa requirements,
simultaneous observers and native backends are rejected. This is not an
ISA extension or a CPU-speedup result.

## Real records and graph-only construction

`ecg_window::Profile` is shared with the diagnostic. Cohort size is the
next power of two of `max(8, ceil(vertices/256))`; eight endpoint bins
cover each cohort. The ten-bit token contains known state, exact gap
0-7, saturated distinct-row strength and a rounded exclusive endpoint.
No represented future is UNKNOWN, never DEAD.

A distinct window layout packs `VID | token << id_bits`, where `id_bits`
comes from the maximum ID actually encoded. Four bytes are used only when
the ID and all ten token bits fit; otherwise eight are required.
Unused upper bits, malformed tokens and mismatched property operands are
rejected. The next-reference FINITE/WRAP codec is not used to decode these
words.

The producer scans for the ID bound, then scans adjacency in reverse with
one eight-byte state word per target line. Same-row duplicates use the
frozen pre-insertion token. CSR/index reads, temporary-state accesses and
carrier initialization/writes are observed by the active cache model.
The original graph and a separate four/eight-byte carrier are retained and
reported separately. No algorithm replay or per-frontier reconstruction
is performed.

The kernel replaces its ordinary ID load with the actual record load.
The checked dependent read returns the unmodified U32 depth value.
There is no extra token table, free future lookup, synthetic record load,
or per-edge rewrite during execution.

## Lifetime, publication and candidate rules

Source progress and structural edge position remain different coordinates.
An actual source visit advances source-bin progress as needed. The normal
CSR-offset loads then supply checked row bounds to the producer; the
prototype does not fetch the graph again solely to recover those bounds.
Record indices must be inside that row and monotonically consumed within
the managed structural pass.

A read/store association checks the latest record index, exact element,
binding, source, pass and absence of intervening modeled memory accesses.
Its matching discovery store publishes the immutable hint under a newer
event order. Old read updates remain superseded, and a later UNKNOWN or
other access can supersede the store update. Unrelated property accesses
publish UNKNOWN. Markers drain outstanding work before progress changes.

The existing 67-bit line payload is reused under an explicit window model:
64 value bits, two state bits and the origin bit. PENDING stores the
expected order; a known window stores its absolute endpoint/strength;
UNKNOWN retains its cutoff. Delivery applies only to a resident line
whose pending order matches. It cannot allocate data or revive an earlier
residency. Expired and previous-pass windows are unrankable.

Both replacement arms use exact GRASP insertion, hit promotion and one
normal victim search/aging operation. Floor 7 refines only the final
RRPV-7 property candidates. Floor 6 subsequently considers RRPV-6 property
ways, accepting only a strictly poorer window rank and preserving the
existing choice on ties. RRPV 0-5, incompatible data and an unrankable base
victim remain protected by the base rule.

The window transport arm performs the same record, control and publication
work but never applies the metadata to victim selection. It is not the
older next-reference transport arm with a different name.

## Functional cost ledger

| Operation | Accounted cost |
|---|---|
| Encoded record and property accesses | Real data-cache requests and resulting fills/writebacks |
| Construction | Actual graph/index, scratch and carrier accesses before the kernel boundary |
| Required metadata updates | Sixteen-entry uncoalesced queue, minimum eight-step latency, one output per step |
| Observation lookup | One available metadata-port step; waits behind ready updates; a missing line can require a post-fill lookup |
| Source-row context | One local functional step per visited row, using already-loaded bounds |
| Checked store association | One local functional step per discovery store |
| Configuration and source-bin markers | Eight outgoing plus eight acknowledgement steps; 48 logical control bytes per exchange; prior queue work drained |

The total step counter closes over memory, local context/association,
metadata observation/wait, control and drain steps. Data-cache contents
and counters are not reset at the setup/kernel boundary.

These are **functional resource steps, not CPU cycles**. Control-link
bytes are separate from DRAM transfers. The controller object and logical
line payload are reported, but complete physical state, tags, ports, ECC,
area, energy, speculation and native retirement costs remain unqualified.
The prototype must not be described as a fully costed physical design.

## Matched experiment

`ecg_window_costed_cache` runs six serial Patents/source-0 cases at an
8 MiB, 16-way LLC: CSR LRU, GRASP, favorable full-capacity P-OPT, window
transport, floor-7 replacement and floor-6 replacement. L1D is 32 KiB and
L2 256 KiB, both eight-way. Every cell has process-tree RSS and wall bounds.

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_window_costed_cache \
  --run-dir results/ecg_experiments/runs/window_costed_case \
  --no-build --no-resume
```

Compare actual demand misses and dirty writebacks, kernel and
setup-inclusive traffic, and the explicit control ledger. Equal algorithm
output/work and complete queue/position accounting are prerequisites.
A passive opportunity count is not a substitute for these measurements;
no default or native admission changes follow automatically from the
prototype's existence.

## Full Patents result

`results/ecg_experiments/runs/window_costed_patents_97d07d5c` contains all
six completed cells at source `97d07d5c`. Every role returns the same BFS
depth digest `3865ba0b18ab9fe2` and completes the same source/frontier work.
All window arms select ID22 plus ten metadata bits in a real four-byte
record. Their setup counters and construction footprints are identical.

| Role | Kernel memory misses | Kernel transfers | Setup transfers | Setup + kernel transfers |
|---|---:|---:|---:|---:|
| CSR LRU | 16,452,278 | 16,817,818 | 2,877,495 | 19,695,313 |
| CSR GRASP | 13,052,296 | 13,416,757 | 2,877,493 | 16,294,250 |
| Favorable full-capacity P-OPT | 10,617,749 | 10,852,949 | 9,293,511 | 20,146,460 |
| Window transport / GRASP base | 13,129,087 | 13,457,393 | 15,936,412 | 29,393,805 |
| Window replacement, floor 7 | 13,134,704 | 13,462,971 | 15,936,412 | 29,399,383 |
| Window replacement, floor 6 | 13,149,532 | 13,477,759 | 15,936,412 | 29,414,171 |

Transfers count demand fills and dirty writebacks; prefetching is disabled.
For floor 6, kernel traffic is 0.151% above its transport control, 0.455%
above GRASP and 24.185% above favorable P-OPT. Including modeled setup,
it is 80.519% above GRASP and 46.002% above P-OPT.
Floor 7 also fails to improve its transport control, by 0.041%.

The reduction versus CSR LRU comes from the GRASP base, not a successful
window refinement: plain GRASP already uses less traffic than either
window replacement arm. No source-specific or post-hoc winning-arm
selection is presented as a deployable policy.

### The mechanism runs, but its choices do not save traffic

Floor 6 makes 261,890 actual victim overrides, including 170,904 RRPV-6
choices. Relative to its own transport, it incurs 27,065 more depth-array
misses and 6,620 fewer other-data misses: a net increase of 20,445 demand
misses. It saves only 79 dirty writebacks, leaving 20,366 more transfers.
This is not a disabled-mechanism result or solely a construction penalty.

The passive read-order pairs were never counterfactual miss estimates:
most were horizon-censored, changing a victim changes later residency,
and this implementation has real record placement and explicit control/
observation work. The active comparison is the deciding evidence.

### Measured cost ledger

Each window arm retains a 132,151,576-byte carrier and uses 1,887,384
bytes of construction scratch. Modeled construction performs 589,002,592
read bytes and 530,204,224 write bytes. The input graph remains allocated.
Serialized file loading and host runner hashing are not included in these
cache-model traffic counters.

The floor-6 run consumes 33,023,480 actual records and reads exactly
132,093,920 record bytes. It forwards 3,764,116 checked discovery stores.
All 36,787,596 metadata events drain, with peak queue occupancy three.
There are 15,507 source/pass markers, 248,112 marker steps and 744,384
configuration/control-link bytes including initial configuration.

The 491,900,413 total functional steps close over:
438,095,550 data-request steps, 3,764,117 row-context steps, 3,764,116
association steps, 41,771,558 observation steps, 4,152,378 observation
wait steps, 248,112 marker steps, sixteen configuration steps and
104,566 drain steps. These counts are not a CPU-time or energy result.
The controller object is 1,032 bytes in this functional implementation;
no graph-sized runtime metadata table or diagnostic shadow is used.

**Decision: the gain gate fails on this workload.** Retain the implementation
as an explicit experimental option and preserve the existing default.
Do not expand this candidate to more algorithms/native hardware or widen
the RRPV rule simply to manufacture a win. A new hypothesis about useful
reuse prediction or policy interaction must precede another candidate.
