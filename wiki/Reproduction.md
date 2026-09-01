# Build and Reproduction

Generated graphs, binaries, traces, and experiment output remain under
`results/` and are not tracked.

## 1. Prepare graph data

Prepare the literature-scale graph corpus:

```bash
make converter

python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py
```

The default core contains web-Google, soc-pokec, cit-Patents, roadNet-CA,
soc-LiveJournal1, and com-Orkut. Add the billion-edge Twitter stress graph
only when sufficient storage and conversion memory are available:

```bash
python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py \
  --graphs twitter-2010 --include-scale-stress
```

After the six core edge lists are present, generate the uniform 262,144-vertex
symmetrized gem5 timing samples. Each sample is additionally capped at 350,000
input arcs (at most 700,000 serialized edges after symmetrization), while a
deterministic coverage set keeps every selected vertex represented:

```bash
python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py \
  --samples-only

python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py \
  --semantics-only
```

Downloads resume when partial files already exist. Conversion receipts and
generated SHA-256 hashes are written under `results/graphs`; the repository
does not carry fixed checksum constants for these generated datasets. The tool
also writes a deterministic `*-dbg.sg` for each graph and timing sample. Final
simulator jobs consume those preordered files with `-o 0`; the reordered graph
is not regenerated per policy.

The commands below describe the earlier three-graph pilot inputs and remain
useful for smoke and sampled timing runs.

Download the three SNAP edge lists:

```bash
mkdir -p results/graphs/web-Google
curl -L https://snap.stanford.edu/data/web-Google.txt.gz |
  gzip -dc > results/graphs/web-Google/web-Google.el

mkdir -p results/graphs/soc-pokec
curl -L https://snap.stanford.edu/data/soc-pokec-relationships.txt.gz |
  gzip -dc > results/graphs/soc-pokec/soc-pokec.el

mkdir -p results/graphs/cit-Patents
curl -L https://snap.stanford.edu/data/cit-Patents.txt.gz |
  gzip -dc > results/graphs/cit-Patents/cit-Patents.el
```

Convert the three symmetrized pilot graphs:

```bash
bench/bin/converter \
  -f results/graphs/web-Google/web-Google.el \
  -s -b results/graphs/web-Google/web-Google.sg

bench/bin/converter \
  -f results/graphs/soc-pokec/soc-pokec.el \
  -s -b results/graphs/soc-pokec/soc-pokec.sg

bench/bin/converter \
  -f results/graphs/cit-Patents/cit-Patents.el \
  -s -b results/graphs/cit-Patents/cit-Patents.sg
```

Create deterministic samples:

```bash
python3 scripts/experiments/ecg/flows/sample_realgraph.py \
  --input results/graphs/web-Google/web-Google.el \
  --output results/graphs/web-Google-n16/web-Google-n16.el \
  --vertices results/graphs/web-Google-n16/web-Google-n16.vertices.tsv \
  --metadata results/graphs/web-Google-n16/web-Google-n16.sample.json \
  --target-vertices 65536

python3 scripts/experiments/ecg/flows/sample_realgraph.py \
  --input results/graphs/soc-pokec/soc-pokec.el \
  --output results/graphs/soc-pokec-n16/soc-pokec-n16.el \
  --vertices results/graphs/soc-pokec-n16/soc-pokec-n16.vertices.tsv \
  --metadata results/graphs/soc-pokec-n16/soc-pokec-n16.sample.json \
  --target-vertices 65536

python3 scripts/experiments/ecg/flows/sample_realgraph.py \
  --input results/graphs/cit-Patents/cit-Patents.el \
  --output results/graphs/cit-Patents-n18/cit-Patents-n18.el \
  --vertices results/graphs/cit-Patents-n18/cit-Patents-n18.vertices.tsv \
  --metadata results/graphs/cit-Patents-n18/cit-Patents-n18.sample.json \
  --target-vertices 262144
```

Convert the samples:

```bash
bench/bin/converter \
  -f results/graphs/web-Google-n16/web-Google-n16.el \
  -s -o 0 \
  -b results/graphs/web-Google-n16/web-Google-n16.sg

bench/bin/converter \
  -f results/graphs/soc-pokec-n16/soc-pokec-n16.el \
  -s -o 0 \
  -b results/graphs/soc-pokec-n16/soc-pokec-n16.sg

bench/bin/converter \
  -f results/graphs/cit-Patents-n18/cit-Patents-n18.el \
  -s -o 0 \
  -b results/graphs/cit-Patents-n18/cit-Patents-n18.sg
```

The publication workflow additionally applies reorder mode `5` and writes the
canonical `*-dbg.sg` files. Use
`prepare_final_graph_corpus.py --samples-only` rather than reproducing that
second conversion manually.

## 2. Build

```bash
python3 -m pip install -r scripts/requirements.txt

make setup-gem5
make setup-gem5-guest-tools
make setup-sniper
make all-sim
make gem5-riscv-m5ops-pr gem5-riscv-m5ops-bfs \
  gem5-riscv-m5ops-sssp gem5-riscv-m5ops-bc gem5-riscv-m5ops-cc
make sniper-sg_kernel
```

`make all-sim` builds `bench/bin_sim/reuse_plan_sidecar`. The gem5 PageRank
flow caches generated sidecars under
`results/ecg_experiments/reuse_plan_sidecars/`.
The cache key includes the graph, reorder options, record width, epoch count,
property-line width, tier fraction, and generator binary. Existing sidecars are
reused; gem5 validates and immutably seals them before execution.

Build receipts are generated from the guest binary's material inputs: source
and included files, compiler/toolchain, link inputs, and build configuration.
They do not encode the repository HEAD or an unrelated worktree diff. Do not
edit checksum fields manually; rerun the corresponding `make` target only when
one of those material inputs changes.

Dedicated third-party artifact flows may still name an upstream revision when
that revision defines the experiment being reproduced; those pins are separate
from the generic build and run receipts.

## 3. Test

```bash
python3 -m pytest -q scripts/test
```

Regenerate and validate the public architecture figures separately:

```bash
python3 scripts/docs/generate_ecg_figures.py
python3 scripts/docs/generate_ecg_figures.py --check
python3 scripts/docs/check_wiki_figures.py
```

The generator emits both `fig/wiki/**/*.svg` and matching uncompressed
`fig/wiki_src/**/*.drawio` files from one declaration. Fixture-backed mechanism
values, including the graph view and cache-line reuse timeline, come from
`fig/ecg-figure-fixture.json`.

Use the [architecture guide](ReusePlan-FlowThrough), the
[RISC-V instruction path](RISC-V-Instruction-Path), and the
[evidence boundary](Evaluation-Methodology) to interpret the generated figures.

## 4. Inspect the PageRank study

```bash
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_pagerank_study \
  --run-dir results/ecg_experiments/runs/pagerank_dryrun \
  --list --dry-run --no-build \
  --allow-missing-graphs --allow-missing-runtime-inputs
```

The profile expands to 12 experiment cells: three graphs and four iteration
counts. Policy sharding is disabled so each comparison retains its matching
baseline.

## 5. Run

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_pagerank_study \
  --run-dir results/ecg_experiments/runs/pagerank_final \
  --no-build --no-resume
```

For a provenance-locked rerun on the reference host, invoke
`/usr/bin/python3.12 -I` and add `--require-reference-python`.

Summarize a complete run:

```bash
python3 scripts/experiments/ecg/analysis/pagerank_gate.py \
  --input results/ecg_experiments/runs/pagerank_final/combined_roi_matrix.csv \
  --config scripts/experiments/ecg/configs/pagerank_study.json \
  --output results/ecg_experiments/runs/pagerank_final/decision.json
```

## 6. Final role-separated campaign

The literature-scale replacement campaign below is gated by its own screen
receipt, whose decision stands at STOP; replacement claims are therefore closed
and its configuration, thresholds, and stages are frozen. The separately
preregistered transport campaign later in this section is run and gated
independently and never reuses these receipts.

The checked-in `reuse_plan_final_campaign` profile is the stopped three-graph
pilot. It remains as a record of that pilot, not as a publication profile.
Publication runs must use the literature-scale corpus above and a revised scale
campaign profile.

Run the mechanism stage and iteration-1 cells first:

```bash
/usr/bin/python3.12 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_literature_scale_campaign \
  --run-dir results/ecg_experiments/runs/literature_scale_i1 \
  --only 60 90 --no-build --require-reference-python

python3 scripts/experiments/ecg/analysis/literature_scale_gate.py \
  --phase early-stop \
  --input-run-dirs \
    results/ecg_experiments/runs/literature_scale_i1 \
  --output \
    results/ecg_experiments/aggregates/literature_scale/early_stop_gate.json
```

Run iteration 8 only if the early-stop gate reports `"decision": "CONTINUE"`
and `"iteration_8_authorized": true`:

```bash
/usr/bin/python3.12 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_literature_scale_campaign \
  --run-dir results/ecg_experiments/runs/literature_scale_i8 \
  --only 91 --no-build --require-reference-python

python3 scripts/experiments/ecg/analysis/literature_scale_gate.py \
  --phase screen \
  --input-run-dirs \
    results/ecg_experiments/runs/literature_scale_i1 \
    results/ecg_experiments/runs/literature_scale_i8 \
  --output \
    results/ecg_experiments/aggregates/literature_scale/screen_gate.json
```

Launch the remaining full-graph roles only if the screen receipt reports
`"valid": true`, `"phase": "screen"`, and
`"pagerank_gate": {"screen_passes": true, ...}`:

```bash
/usr/bin/python3.12 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_literature_scale_campaign \
  --run-dir results/ecg_experiments/runs/literature_scale_full \
  --only 92 93 94 95 \
  --screen-gate \
    results/ecg_experiments/aggregates/literature_scale/screen_gate.json \
  --no-build

python3 scripts/experiments/ecg/analysis/literature_scale_gate.py \
  --phase complete \
  --input-run-dirs \
    results/ecg_experiments/runs/literature_scale_i1 \
    results/ecg_experiments/runs/literature_scale_i8 \
    results/ecg_experiments/runs/literature_scale_full \
  --output \
    results/ecg_experiments/aggregates/literature_scale/gate.json
```

For parallel local execution, generate cell-complete shards so every shard
retains its full policy roster. The same screen authorization is required by
every local shard:

```bash
python3 scripts/experiments/ecg/slurm/make_slurm_shards.py \
  --profile reuse_plan_literature_scale_campaign \
  --only 92 93 94 95 \
  --run-tag literature_scale_full \
  --whole-cell \
  --out results/ecg_experiments/slurm/literature_scale_full.tsv

python3 scripts/experiments/ecg/flows/run_local_shards.py \
  --shards results/ecg_experiments/slurm/literature_scale_full.tsv \
  --run-root results/ecg_experiments/runs/local \
  --screen-gate \
    results/ecg_experiments/aggregates/literature_scale/screen_gate.json \
  --jobs 8 --cache-sim-jobs 4 --gem5-jobs 1 --sniper-jobs 1
```

For Slurm arrays, export the same receipt as
`GRAPHBREW_SCREEN_GATE` before invoking
`slurm_experiment_shard.sbatch`. The stopped `reuse_plan_final_campaign`
profile remains available only for `--list --dry-run` manifest enumeration and
must not be executed.

### Separate transport campaign

The `reuse_plan_transport_campaign` profile holds replacement at pure LRU in
both arms and isolates compact ReusePlan record transport and structural
FlowThrough. It compares `LRU` against `ECG_REUSE_PLAN_LRU_FLOWTHROUGH` with
`--flowthrough all` on both arms, so structural FlowThrough is symmetric. Its
preregistration is
[`transport_literature_scale.json`](https://github.com/UVA-LavaLab/ECG_GrAPL/blob/main/scripts/experiments/ecg/configs/transport_literature_scale.json).
It makes no replacement-policy claim and no comparison against SRRIP, GRASP, or
P-OPT. The profile requires a clean worktree.

This is a confirmatory rerun. The configuration discloses the earlier
iteration-1 transport-control rows that informed this narrower scope; those
rows are not reused as evidence. The 0.98/1.02 limits retain the pre-existing
+/-2% tie band. Full-graph compact/wide cache_sim comparisons cover the five
graphs whose identifiers fit the 32-bit record. `soc-LiveJournal1` is excluded
because its `23 + 2 + 4 + 4 = 33` bit budget does not fit. Sniper contributes
demand LLC load-miss counts only, not byte-level off-chip traffic or timing.

Run the mechanism stage and the iteration-1 transport cells first, then
evaluate the screen:

```bash
/usr/bin/python3.12 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_transport_campaign \
  --run-dir results/ecg_experiments/runs/transport_screen \
  --only 60 96_gem5_transport_i1 --no-build --require-reference-python

python3 scripts/experiments/ecg/analysis/transport_scale_gate.py \
  --phase screen \
  --input-run-dirs \
    results/ecg_experiments/runs/transport_screen \
  --output \
    results/ecg_experiments/aggregates/transport_scale/screen_gate.json
```

The screen receipt is bound to the Git commit, the manifest hash, and the
transport configuration hash. Continue only when it reports `"valid": true`,
`"phase": "screen"`, and `"decision": "GO"`. A receipt with
`"decision": "STOP"` is a valid measured outcome and closes the campaign.

Run the iteration-8, full-graph, and matched-work roles only with that
receipt. Stages 97 through 100 refuse to start without it, and the receipt is
recomputed from its source run directories before any job is expanded:

```bash
/usr/bin/python3.12 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_transport_campaign \
  --run-dir results/ecg_experiments/runs/transport_full \
  --only 97 98 99 100 \
  --screen-gate \
    results/ecg_experiments/aggregates/transport_scale/screen_gate.json \
  --no-build

python3 scripts/experiments/ecg/analysis/transport_scale_gate.py \
  --phase complete \
  --input-run-dirs \
    results/ecg_experiments/runs/transport_screen \
    results/ecg_experiments/runs/transport_full \
  --corpus-receipt \
    results/graphs/literature_scale_corpus.receipt.json \
  --output \
    results/ecg_experiments/aggregates/transport_scale/gate.json
```

Receipts from a different commit, manifest, or transport configuration are
rejected, and the replacement campaign's receipts never authorize these
stages. Sniper rows and the mechanism stage carry no admissible timing.

## 7. Cross-simulator consistency

```bash
python3 -m pytest -q \
  scripts/test/test_grasp_sideband_registration.py \
  scripts/test/test_popt_permutation_equivalence.py

python3 scripts/experiments/ecg/verify/equiv_kernels.py \
  --gem5 --sniper --kernels pr bfs sssp bc cc --reuse-plan-depth 2
```

## 8. Validate and aggregate local output

Run the manifest-derived final gate before interpreting or aggregating rows:

```bash
python3 scripts/experiments/ecg/analysis/final_campaign_gate.py \
  --input-run-dirs \
    results/ecg_experiments/runs/reuse_plan_final_timing \
    results/ecg_experiments/runs/reuse_plan_final_popt \
    results/ecg_experiments/runs/reuse_plan_final_cache \
    results/ecg_experiments/runs/reuse_plan_final_sniper \
  --output results/ecg_experiments/aggregates/reuse_plan_final/gate.json
```

Only aggregate after the gate reports `"valid": true`.

```bash
python3 scripts/experiments/ecg/flows/aggregate_results.py \
  --skip-run \
  --input-run-dirs \
    results/ecg_experiments/runs/reuse_plan_final_timing \
    results/ecg_experiments/runs/reuse_plan_final_popt \
    results/ecg_experiments/runs/reuse_plan_final_cache \
    results/ecg_experiments/runs/reuse_plan_final_sniper \
  --run-root results/ecg_experiments/aggregates/reuse_plan_final
```
