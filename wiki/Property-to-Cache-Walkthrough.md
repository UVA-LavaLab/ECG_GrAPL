# One edge from graph structure to a cache decision

This page follows one access rather than introducing a new example at each
hardware boundary. The topology comes from `fig/ecg-figure-fixture.json`:
32 vertices, nine non-isolated vertices shown in the diagrams, and 34 directed
adjacency entries after expanding the undirected edges. The original fixture's
weights are irrelevant to PageRank and are omitted from the drawings.

## 1. Start with the operation the kernel executes

PageRank pull visits the in-neighbors of outer vertex `u=8`:

```text
row_ptr[8] = 14
row_ptr[9] = 19
in_ids[14:19] = [3, 6, 7, 11, 18]
```

The fifth edge is at CSR position `j=18` and names property vertex `v=18`.
Its semantic request number is `s=19` in the first iteration. With sixteen
four-byte properties per cache line, `p[18]` and `p[20]` share line B,
covering vertices `16..31`.

The next B use occurs at `j=22`, request `s=23`, so the true reference distance
is `22-18=4`. The reference is line-granular even when the future vertex
differs. Position is not identity: another entry, `j=22`, also contains ID 18.

## 2. Pack metadata into the ID headroom

### Figure 1 — One edge, two encodings, unchanged data

![The ordinary ID and Full14 metadata mask combined into a single word, contrasted with the Scale6 representation of the same edge, followed by identical property addressing and F32 data](../fig/wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f01-checked-request.svg)

**Figure 1.** The example needs five ID bits, so its ID extraction mask is
`0x1F`. Full14 uses its default eight reference bits, two state bits and four
action bits above that ID field. Encoding distance four gives reference
`0x10`; FINITE is state `1`. No eligible distinct-line prefetch exists in
this record's lookahead, so its action is zero.

| Transformation | Result |
|---|---|
| ordinary edge ID | `18 = 0x00000012` |
| metadata mask | `(0x10 << 5) \| (1 << 13) = 0x00002200` |
| encoded Full14 word | `18 \| 0x00002200 = 0x00002212` |
| decoded vertex | `0x00002212 & 0x1F = 18` |
| decoded distance / deadline | `4` / `19+4=23` |

The word has 13 unused high bits: the current Full14 implementation does not
automatically expand its metadata beyond fourteen bits. That is distinct from
the **available** 27-bit budget.

For the native pipeline drawing, encode the same edge with its implemented
26+6 ABI. Scale6's distance-four token is `4`, giving
`(4 << 26) | 18 = 0x10000012`. The vertex remains 18, but the decoded upper
bound is `7`, hence deadline `26`. A small graph can be used to explain or
exercise that ABI without claiming that it needs 26 ID bits.

## 3. Recover the address, not a different property value

The drawing chooses virtual property base `0x80000000`:

```text
property VA = 0x80000000 + 18 * 4 = 0x80000048
virtual line = 0x80000040
offset within the 64-byte line = 8
```

Normal translation produces a physical line, labeled `P_B` in the pipeline.
The VA is not assumed to equal the PA. The actual guest can allocate different
addresses; the diagram's base makes the transformation easy to follow.

At this point in the first iteration, vertex 18 has not yet been updated:
`18 > u=8`. Its out-degree is four, and the kernel initialized scores to
`1/32`. The contribution returned by the load is therefore
`(1/32)/4 = 1/128`, with F32 bits **`0x3C000000`**.
The mask is never applied to those floating-point data bits.

## 4. Follow the native operand into its own instruction

The native record operation performs a real four-byte load. It derives the
semantic position from the record address and iteration descriptor, normalizes
WRAP if necessary, and produces:

```text
canonical operand = (s19 << 32) | 0x10000012
                  = 0x0000001310000012
```

P17 is an illustrative rename tag for that 64-bit integer result. This does
**not** turn the memory record into eight bytes. The dependent property
operation consumes that exact renamed operand, forms the ordinary address,
and returns the F32 value to an illustrative floating-point tag F9.

The property instruction also retains its own context, semantic position and
prediction. Completion is not retirement. The request can expose an
observation at the LLC, but only successful retirement can authorize a timed
prediction update. A private hit returns data without a new LLC demand while
still producing that retirement update.

The [processor pipeline](RISC-V-Instruction-Path) shows both data loads, the
register dependency, AGU/LSQ/translation, ROB association and separate
retirement channel.

## 5. See why the cache may choose a different line

The preceding access, `j=17`, reads `p[11]` in line A. Its next A use is
`j=19`, so Scale6 gives it bound `3` and deadline `18+3=21`.
At semantic position 19:

| Resident line | Last touch | Actual next use | Scale6 remaining bound | Shared victim score |
|---|---|---|---|---|
| A, vertices `0..15` | 18 | 20 | `21-19=2` | 0 |
| B, vertices `16..31` | 19 | 23 | `26-19=7` | 1 |

In the teaching two-way snapshot, an incoming scores-line C needs a way.
LRU selects older A. ECG's shared score selects B, retaining A for the next
governed request. The [cache-decision figure](ReusePlan-FlowThrough#4-use-the-prediction-to-choose-a-victim)
draws the two resulting states.

This snapshot isolates the policy decision; it is not a complete cache trace
or a benchmark result. In a real hierarchy, observations, delayed updates,
private hits and other traffic determine which predictions are available.
Holding a prediction fixed beyond its bound yields UNKNOWN, never an inferred
DEAD state.

## 6. Account for where each representation lives

### Figure 2 — Where the mask and its decoded state live

![Memory, CPU and cache ownership map separating encoded edge words from property values, temporary preprocessing, in-flight native associations, bounded queues and the different Full14 and Scale6 line-state budgets](../fig/wiki/property-to-cache-walkthrough/property-to-cache-walkthrough-f02-architecture-state-map.svg)

**Figure 2.** The record, renamed operand and LLC prediction are not three
copies of the same data structure with the same lifetime.

| State | Owner and lifetime |
|---|---|
| encoded edge record | graph traversal input; four bytes replace the measured ordinary ID read |
| construction scratch | preprocessing; Full14 includes edge-sized arrays, while bounded in-place Scale6 uses line-indexed first/next positions |
| canonical operand and dynamic hint | CPU register/in-flight instruction lifetime; native control cost, not an edge-sized future table |
| commit/prefetch entries | bounded queues, independently accounted |
| future deadline/state/origin | resident LLC-line lifetime; `D+3` logical bits per line |

The functional Full14 path retains a separate encoded carrier; the in-place
Scale6 path avoids another edge array. The native borrowed carrier restores
ordinary IDs after the ROI. “No metadata sidecar” does not mean all builders
have identical allocation or preprocessing costs.

With an 8 MiB, 64-byte-line LLC, there are 131,072 lines. With both functional
queues and lookahead enabled, the accounting is:

| Configuration | Line payload | Buffers/control | Total |
|---|---:|---:|---:|
| Full14, default `D=21` | 3,145,728 bits | 2,624 bits | 3,148,352 bits |
| Scale6, `D=32` | 4,587,520 bits | 3,064 bits | 4,590,584 bits |

These are logical model-state totals, not synthesized area. Ordinary tags,
data, RRPV and recency are separate. Native capture lanes, address/classification
validation, port logic and the proposed two-line, 128-byte lookahead buffer
need their own accounting. The functional 512-bit window is not evidence of
an implemented native prefetch buffer.
