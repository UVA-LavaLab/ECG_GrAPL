# Build and Reproduction

Generated graphs, binaries, traces, and experiment output remain under
`results/` and are not tracked.

## 1. Prepare graph data

Prepare the literature-scale publication corpus with the resumable corpus
tool:

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

Downloads are resumable. Conversion and generated SHA-256 receipts are written
under `results/graphs`; researchers do not maintain checksum constants in the
repository.

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

Convert the symmetrized full graphs used by the final campaign:

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
  -b results/graphs/web-Google-n16/web-Google-n16.sg

bench/bin/converter \
  -f results/graphs/soc-pokec-n16/soc-pokec-n16.el \
  -b results/graphs/soc-pokec-n16/soc-pokec-n16.sg

bench/bin/converter \
  -f results/graphs/cit-Patents-n18/cit-Patents-n18.el \
  -s -b results/graphs/cit-Patents-n18/cit-Patents-n18-sym.sg
```

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

`make all-sim` builds `bench/bin_sim/reuse_plan_sidecar`. gem5 PageRank runs
cache generated sidecars under `results/ecg_experiments/reuse_plan_sidecars/`.
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

## 4. Inspect the PageRank study

```bash
python3 scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_pagerank_study \
  --run-dir results/ecg_experiments/runs/pagerank_dryrun \
  --list --dry-run --no-build \
  --allow-missing-graphs --allow-missing-runtime-inputs
```

The profile expands to 12 whole cells: three graphs and four iteration counts.
Policy sharding is disabled so each comparison retains its matching baseline.

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

> **Scale warning:** the checked-in `reuse_plan_final_campaign` profile is the
> stopped three-graph pilot. It is retained for auditability, not publication.
> A publication run must use the literature-scale corpus above and a revised
> scale campaign profile.

Inspect the complete campaign before launching:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_final_campaign \
  --run-dir results/ecg_experiments/runs/reuse_plan_final_dryrun \
  --list --dry-run --no-build --no-resume
```

The list command is authoritative. It expands the campaign into:

- one synthetic ReuseBind mechanism preflight;
- 12 gem5 O3 PageRank timing cells;
- 12 full-graph cache_sim compact-record primary cells;
- 15 matched wide-record cache_sim controls;
- 15 matched 256-epoch cache_sim controls;
- six PR/CC P-OPT reference cells with simulated matrix traffic; and
- 12 full-graph Sniper compact-record corroboration cells; and
- three wide-record Sniper SSSP cells.

The full-graph compact primary and the P-OPT comparison use a 4-byte ReusePlan record
with 16 epochs for PR, BFS, BC, and CC. Weighted SSSP uses its implemented
8-byte replacement record and is evaluated only in the wide-record stages.
Wide controls isolate record width and raise ReusePlan to 256 epochs for an
epoch-resolution sensitivity. Sniper runs one full serialized edge sweep per
graph so the working set turns over an 8 MiB LLC. Sniper rows support
cache/traffic direction only, never architectural speedup.

Launch the three roles into separate resumable directories:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_final_campaign \
  --run-dir results/ecg_experiments/runs/reuse_plan_final_timing \
  --only 60 70 71 72 73 \
  --no-build

python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_final_campaign \
  --run-dir results/ecg_experiments/runs/reuse_plan_final_popt \
  --only 84 \
  --no-build

python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_final_campaign \
  --run-dir results/ecg_experiments/runs/reuse_plan_final_cache \
  --only 80 82 83 \
  --no-build

python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile reuse_plan_final_campaign \
  --run-dir results/ecg_experiments/runs/reuse_plan_final_sniper \
  --only 81 85 \
  --no-build
```

The first cache command is the P-OPT validation. It is the first end-to-end use
of simulated matrix streaming and must report non-zero simulated stream lines
for every charged P-OPT row before the remaining cache_sim stages begin.

For parallel execution, generate whole-cell shards so every shard retains its
complete policy roster:

```bash
python3 scripts/experiments/ecg/slurm/make_slurm_shards.py \
  --profile reuse_plan_final_campaign \
  --run-tag reuse_plan_final \
  --whole-cell \
  --out results/ecg_experiments/slurm/reuse_plan_final.tsv

python3 scripts/experiments/ecg/flows/run_local_shards.py \
  --shards results/ecg_experiments/slurm/reuse_plan_final.tsv \
  --run-root results/ecg_experiments/runs/local \
  --jobs 8 --cache-sim-jobs 4 --gem5-jobs 1 --sniper-jobs 1
```

Use `--only 84` when generating the first validation shard set. Generate the
remaining stages only after the P-OPT rows pass.

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
