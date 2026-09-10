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
