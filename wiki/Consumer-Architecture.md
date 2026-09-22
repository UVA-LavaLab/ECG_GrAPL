# Consumer architecture: rank-first selection

This cache-only study asks whether the way a GRASP-based consumer **orders**
its candidates, rather than the information it receives, explains the traffic
it still loses to intact P-OPT. It follows the
[reference-consumer diagnostic](GRASP-Reference-Consumer), which established
that graph ranks help that consumer but leave it well short of P-OPT.

It is not a new edge encoding, a deployable ECG policy, or a native
performance result. **The rank-first arm lost**, as reported
[below](#measured-patents-result). The explanation that did hold
for these kernels was eviction precedence, not candidate order; see the
[mask failure taxonomy](Mask-Failure-Taxonomy) and
[governed-first eviction](Governed-First-Eviction).
The default next-reference mechanism and
the base-first diagnostic keep their original semantics.

## Why the residual could not be an information problem

The reference diagnostic gave a GRASP-based consumer the actual canonical
P-OPT FULL matrix, with ideal availability at victim selection and no RRPV
candidate restriction. It closed 29.42% of the ordinary-GRASP-to-P-OPT kernel
gap and remained 17.41% above intact P-OPT.

That residual cannot be attributed to encoding precision, quantization,
availability or delivery, because the diagnostic and intact P-OPT consume the
**same matrix**: 60,396,288 bytes, 235,923 lines, 256 epochs, digest
`3683564403557561320`. Whatever remains is a property of the consumer.

The residual is also narrow in kind. Against intact P-OPT the diagnostic has
2,159,195 more registered-property demand misses and 610 **fewer** writebacks.
Dirty traffic is already at P-OPT quality; the loss is entirely in which
property lines survive.

## The one difference under test

The base-first consumer obtains the GRASP victim once and replaces it only
with a **strictly farther** covered rank, leaving equal ranks in the existing
order. Intact P-OPT instead ranks first and confines RRIP aging to the
maximum-rank candidates.

`--grasp-reference rank` adds `DIAG_GRASP_REFERENCE_RANK`, which differs from
`DIAG_GRASP_REFERENCE_FULL` in exactly one respect: rank-first ordering among
covered property ways, with RRIP aging confined to the maximum-rank tie set.
Both arms share one engagement boundary in literally the same code. Each takes
the GRASP victim, applies the same matrix and source checks, and returns that
victim unchanged when it is not covered property data. Only then do they
diverge.

The arm deliberately does **not** adopt P-OPT's non-property eviction
precedence. Adding it would confound the ordering question with the region
rule and make the measured quantity uninterpretable. Non-covered ways keep
ordinary GRASP handling, and insertion, hit promotion, recency, dirty state
and invalid-way priority are unchanged. Ranks are compared raw, as the
base-first arm compares them, rather than clamped to the width P-OPT uses for
its own table; adopting that clamp would have been a second difference.

Both arms build and look up the same complete matrix, so their setup traffic
and initial state are identical.

## Frozen four-cell study

The `ecg_consumer_architecture_cache` profile runs exactly four cells on the
fixed full DBG Patents graph, SpMV two sweeps, 32 KiB L1D and 256 KiB L2 (both
eight-way), an 8 MiB/16-way LLC and 64-byte lines, with no prefetch: fresh
ordinary GRASP, fresh intact favorable `POPT:UNCHARGED`, the base-first
diagnostic, and the rank-first diagnostic. One simulator thread, a 2 GiB
process-tree RSS limit, a 512 MiB algorithm workspace and a thirty-minute wall
limit apply to each cell. The two diagnostic launch strings have equal length,
including their output directory names.

Let `R` be the base-first-to-intact-P-OPT kernel residual measured in the same
build. The preregistered quantity is the fraction of `R` that rank-first alone
recovers:

```text
share = (basefirst_kernel - rankfirst_kernel) / (basefirst_kernel - popt_kernel)
```

Written before any implementation or run, and unchanged since:

- `share >= 0.50` — selection architecture is the dominant cause, and the
  rank-first premise is supported.
- `0.20 <= share < 0.50` — partial; insertion isolation becomes the next
  question rather than a candidate build.
- `share < 0.20` — rank-first ordering does not explain the residual, and a
  pass-local rank-first design should be rejected rather than widened.

Two limits were also recorded in advance. The `basefirst_divergence` counter
understates the treatment, because rank-first additionally ages only the
maximum-rank tie set while base-first ages every line, so the arms can differ
even where the counter reads zero. And because the arms share an engagement
boundary, a low `share` refutes "ordering and tie discipline alone explain the
residual", which is narrower than refuting every possible rank-first design.

```bash
ulimit -c 0
make -j1 bench/bin_sim/algorithms
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_consumer_architecture_cache \
  --run-dir results/ecg_experiments/runs/consumer_architecture_patents \
  --no-build --no-resume
```

Use a clean committed tree. This is a separate four-run budget; the earlier
twelve-run campaign and the four-run reference budget remain closed. Failure
rejects this ordering direction, not all possible consumer designs.

## Measured Patents result

The four cells completed at implementation commit `2b00f213` in
`results/ecg_experiments/runs/consumer_architecture_patents_2b00f213`. All four
produce result digest `55e3fc26a1027ddb`; the three matrix-bearing arms share
digest `3683564403557561320`; the two diagnostics have identical setup
counters.

| Policy | Property misses (`x`+`y`) | Other misses | Writebacks | Kernel transfers |
|---|---:|---:|---:|---:|
| Ordinary GRASP | 9,847,988 | 5,073,432 | 535,510 | 15,456,930 |
| Intact favorable P-OPT | 6,855,000 | 5,073,432 | 469,984 | 12,398,416 |
| Base-first diagnostic | 9,014,195 | 5,073,432 | 469,374 | 14,557,001 |
| Rank-first diagnostic | 9,137,458 | 5,073,432 | 469,038 | 14,679,928 |

```text
share = (14,557,001 - 14,679,928) / (14,557,001 - 12,398,416) = -0.0569
```

Rank-first uses **122,927 more kernel transfers than base-first**, or 0.844%:
123,263 additional property misses against 336 fewer writebacks. It remains
5.03% better than ordinary GRASP and 18.40% worse than intact P-OPT, closing
25.40% of the ordinary-GRASP-to-P-OPT gap where base-first closes 29.42%.

This was not an inactive arm. Rank-first made 14,210,890 victim decisions,
8,545,695 of them on a covered base, with 5,430,330 overrides and a
maximum-rank tie population of 15,467,196, about 1.81 tied ways per covered
decision. In 762,013 decisions it selected a way the base-first rule would not
have selected on the same set. As recorded in advance, that count understates
the treatment, because the tie-set aging differs even where the selected way
agrees.

### Setup and total traffic

| Policy | Setup transfers | Setup + kernel transfers |
|---|---:|---:|
| Ordinary GRASP | 2,877,491 | 18,334,421 |
| Intact favorable P-OPT | 9,293,507 | 21,691,923 |
| Base-first diagnostic | 9,373,437 | 23,930,438 |
| Rank-first diagnostic | 9,373,437 | 24,053,365 |

### Decision and provenance

**Decision: the ordering hypothesis is refuted.** With ideal information and no
candidate restriction, rank-first selection with RRIP tie aging is slightly
worse than strictly-farther refinement on this workload. A pass-local
rank-first replacement design is therefore **rejected rather than rescued**:
its defining action is what this arm tested. Do not respond by widening
eligibility, adding metadata bits, or tuning the tie rule after this result.

The measured residual remains unexplained by ordering. Because both arms share
the engagement boundary, the path where the base victim is not covered property
data was never exercised by either. That path, and insertion, remain the open
explanations; neither is established here.

| Artifact | SHA256 |
|---|---|
| `combined_roi_matrix.csv` | `b4e8e7ce4fb4e984069fbf45eff31b6c2f534549a0e0a9acddef75142137067d` |

### Execution record

The first execution of the rank-first cell completed its simulation normally
and was then recorded `status=error` by the runner's receipt validation, which
required `strictly_farther_only` to be true for every reference mode. The
rank-first arm truthfully reports it as false. The validation was corrected in
commit `2b00f213` to expect the mechanics each mode declares, and a test now
drives real base-first and rank-first receipts through that path.

All four cells were then rerun fresh at the corrected commit and reproduced
every value above exactly, so the corrected execution changed no measurement.
Eight executions were used against a four-execution budget: four in the first
run, one of which was lost to the validation defect, and four in the
deterministic rerun. No execution selected a parameter, graph, source or
capacity, and no value differed between the two runs. The earlier run directory
is retained unchanged as the record of that defect.
