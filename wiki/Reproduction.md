# Build and Reproduction

This runbook covers the current adaptive ECG design only. Use a clean,
committed checkout and keep builds and simulations serial. Graphs, binaries,
simulator checkouts and results are untracked; stage them separately on the
machine that will execute the experiments.

Current kernels cover PR, SpMV, BFS, SSSP, CC, BC and TC. The
[per-algorithm performance table](Evaluation-Methodology#per-algorithm-performance)
distinguishes bounded qualification, measured cache behavior and unmeasured CPU speedups.

## 1. Prepare inputs

Create the deterministic small corpus first:

```bash
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py --check
```

This preserves all 32 vertices and 34 records in the worked fixture, plus a
512-vertex, 2,048-record pressure graph. Graph bytes and construction recipes
are checked; changed artifacts are not silently regenerated.

For the full core corpus, stage the prepared `.sg` files or use the existing
preparation flow when downloads/conversion are required:

```bash
make -j1 PARALLEL=1 converter
python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py
python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py \
  --weighted-source results/graphs/cit-Patents/cit-Patents-dbg.sg \
  --weighted-output results/graphs/cit-Patents/cit-Patents-dbg-w32.wsg --weight-maximum 32
```

The core set is web-Google, roadNet-CA, cit-Patents, soc-pokec,
soc-LiveJournal1 and com-Orkut. Prepare Twitter only with sufficient space
and memory:

```bash
python3 scripts/experiments/ecg/flows/prepare_final_graph_corpus.py \
  --graphs twitter-2010 --include-scale-stress
```

Current jobs consume the prepared `*-dbg.sg` files with `-o 0`; graph ordering
is not regenerated per policy. Preserve isolated vertices and the declared
directed/symmetrized graph semantics. Input hashes must match the manifest.

## 2. Build the current backends

On an unconfigured host, install only missing Python dependencies and set up
the required backends using the repository flows:

```bash
python3 -m pip install -r scripts/requirements.txt
make setup-gem5-guest-tools
timeout 7200 python3 scripts/setup_gem5.py --isa RISCV --jobs 1
make setup-sniper PARALLEL=1
```

Build the current guests with one compiler job:

```bash
make -j1 PARALLEL=1 sim-pr sniper-sg_kernel gem5-riscv-m5ops-pr
make -j1 PARALLEL=1 gem5-riscv-m5ops-record_isa_smoke
make -j1 PARALLEL=1 sim-algorithms gem5-riscv-m5ops-algorithms sniper-algorithms
```

PageRank builds use explicit non-fused F32 arithmetic. Native build receipts
bind source/includes, compiler, link inputs and configuration. Do not edit
receipt hashes manually. Rebuild when material inputs change; do not assume a
workstation `-march=native` executable is portable to the lab CPU.

## 3. Qualify the shared semantics

After preparation and builds, run the complete small profile:

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_current_equivalence \
  --run-dir results/ecg_experiments/runs/ecg_current_equivalence \
  --no-build --no-resume

python3 scripts/experiments/ecg/record_equivalence_gate.py \
  --validate-receipt \
  results/ecg_experiments/runs/ecg_current_equivalence/current_ecg_equivalence.complete.json
```

This is the default profile: 12 sequential jobs and 36 rows across cache_sim,
gem5 RV64 O3 and translated SIFT Sniper, with both record widths.
Every row uses the same prepared graph and `-o 0 -n 1 -i 2 -t 0`.
The pressure graph must produce real prefetch activity. The small cache
geometry is for mechanism coverage, not final-paper performance.

Per-cell wall limits are 30-120 seconds; each whole matrix process tree is
also bounded at 2,048 MiB RSS and an explicit wall limit. The receipt
recomputes the complete roster, raw semantics, inputs and outputs.
Filtered runs cannot issue full authorization. Diagnostic fingerprints never
enter CPU speedup results.

On the reference host, use `/usr/bin/python3.12 -I` and add
`--require-reference-python` for the pinned interpreter contract.

## 4. Run current cache experiments

The additional algorithms use the shared current executable, not the legacy
`bfs`, `sssp`, `cc`, `bc`, `tc` or `pr_spmv` paths:

```bash
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py --algorithms

ulimit -c 0
for backend in cache gem5 sniper; do
  python3 -I scripts/experiments/ecg/flows/experiment_run.py \
    --profile "ecg_algorithm_equivalence_${backend}" \
    --run-dir "results/ecg_experiments/runs/algorithm_equivalence_${backend}" \
    --no-build --no-resume
done

python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_algorithm_cache \
  --run-dir results/ecg_experiments/runs/ecg_algorithm_cache \
  --no-build --no-resume
```

Each qualification profile contains 84 rows: six algorithms, seven policies and
both record widths, with independent diamond/clique answers and actual-record
semantic digests. The cache performance profile
contains 42 auto-width rows on a 512-vertex pressure graph with two nontrivial
components and 16 isolates. SpMV/SSSP use the explicit weighted graph (integer weights 0-31);
the other kernels use its identical unweighted topology. All run serially with
32 MiB algorithm workspace, 1 GiB cache-process or 2 GiB detailed-process RSS
guards. The tiny cache geometry is diagnostic, not a final-paper machine.

`--current-algorithms` selects this path; changing only `--benchmark` does not.
Detailed algorithm runs currently require bounded `--ecg-equivalence`; their
instrumented work is not CPU speedup. The profiles cannot substitute for the
PR authorization or authorize unmeasured detailed final workloads.

Recorded semantic runs are under `results/ecg_experiments/runs/`:
`algorithm_equivalence_cache_afda774e`, `algorithm_equivalence_gem5_complete`
and `algorithm_equivalence_sniper_final`. Every raw workload/transport receipt
must replay, and corresponding output, work, position and metadata digests must match.

The current pressure table uses `algorithm_cache_current_final` at `bf730824`:
42 rows, 10.525 s summed guarded job time, and 50.449 MiB peak sampled RSS.
These are local runner/resource measurements, not simulated CPU execution times.

| Profile | Workload | Resource scope |
|---|---|---|
| `ecg_matched_8mb_cache` | Full Patents PR/SpMV, two traversals, common LRU/GRASP/T/R/RP controls, 8 MiB/16-way LLC | Serial; 4 GiB RSS; 30 minutes per policy |
| `ecg_dynamic_8mb_cache` | Full Patents BFS/weighted SSSP, source 0, delta 8, LRU/T/RP, 8 MiB/16-way LLC | Six serial cells; 4 GiB RSS; 30 minutes per policy |
| `ecg_local_release_cache` | Six full core graphs, ten CSR/reference/ECG roles, two iterations, 8 MiB LLC | 8 GiB RSS; one hour per policy |
| `ecg_large_cache` | Focused current-mechanism capacity exploration | Explicit manifest limits; no final authorization |
| `ecg_twitter_reproduction` | Directed Twitter, ten roles, one iteration, 8/24 MiB LLC | 32 GiB RSS; two hours per policy; twelve hours per matrix |

The executable Twitter profile name is retained as an interface; its current
measurements are reported as Twitter-scale evaluation.

The earlier `matched_8mb_pr_spmv` and `dynamic_8mb_bfs_sssp` runs predate the LRU-neutral repair.
Corrected focused rows are in `results/ecg_experiments/roi_matrix/pr_lru_neutral_policy`
and `bfs_lru_neutral_policy`; their `.log.cmd` receipts preserve the exact commands.
`traffic_phases.setup/kernel` close to the unchanged total; compare kernel-only scopes.
Weighted-input receipts bind the topology-preserving synthetic 1-32 weight recipe and both file hashes.

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_local_release_cache \
  --run-dir results/ecg_experiments/runs/local_release_cache --no-build

python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_twitter_reproduction \
  --run-dir results/ecg_experiments/runs/current_twitter_reproduction --no-build
```

The full-core profile uses `--current-pr-baselines` and simulated P-OPT
column traffic. Twitter uses explicitly disclosed analytic P-OPT traffic.
Do not pool those traffic models as one configuration. The ordinary CSR path
and current record path use the same fixed PageRank work and CSR-index
accounting. Neither profile authorizes detailed execution.

The current Twitter carrier needs about 5.47 GiB in addition to the retained
graph. Measured whole-process peak RSS was about 24.4 GiB, above the initial
planning estimate. Keep the 32 GiB guard and host headroom; a scheduler
allocation must exceed that cap. Resource estimates are not allocation bounds.

Current cache/traffic results are summarized in
[Evaluation Methodology](Evaluation-Methodology#5-pre-correction-pagerank-evidence).

## 5. Lab handoff and detailed execution

Rebuild on the executing lab node and generate its own complete equivalence
receipt. Workstation receipts are path-, binary-, source- and input-bound;
copying one is not a replacement for lab qualification.

For Slurm, submit from the repository root:

```bash
mkdir -p results/slurm_logs
sbatch --export=ALL,GRAPHBREW_SLURM_MODE=current-whole-profile,GRAPHBREW_CURRENT_PROFILE=ecg_current_equivalence,GRAPHBREW_CURRENT_RUN_TAG=lab_equivalence \
  scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch
```

The wrapper runs one serial whole profile. Its on-node preflight has a
512 MiB / 60-second watchdog and checks required tools/runtime files,
ASLR control and a tiny FUSE mount/read/unmount for gem5 profiles.
Availability checks do not prove CPU compatibility or SDE correctness;
the complete small workload run is still required.

After the lab receipt exists, the direct detailed command is:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_detailed_final --final-stage \
  --equivalence-receipt \
  results/ecg_experiments/runs/ecg_current_equivalence/current_ecg_equivalence.complete.json \
  --run-dir results/ecg_experiments/runs/ecg_detailed_final --no-build
```

Use the actual lab receipt path; the Slurm example places it under
`results/ecg_experiments/runs/slurm/lab_equivalence/ecg_current_equivalence/`.
For a Slurm final launch, set `GRAPHBREW_CURRENT_PROFILE=ecg_detailed_final`,
`GRAPHBREW_FINAL_STAGE=1` and `GRAPHBREW_EQUIVALENCE_RECEIPT` to that receipt.
Current profiles do not use array shards or partial policy/graph filters.

Inspect `--list` before submission and size the allocation for the complete
profile. At present, `ecg_detailed_final` covers PageRank on Patents and Orkut
with transport/combined pairs; it is not yet the complete per-algorithm paper
matrix. Its 24-hour per-policy limits do not guarantee that every full-graph
simulation fits the lab allocation.

## 6. Runtime contracts, budgets and recovery

Current policy names are `ECG:transport`, `ECG:replacement`, `ECG:prefetch`
and `ECG`. Keep the graph/order, arithmetic, iteration count, private-cache
geometry and line size equal across each matched comparison.
Use `--ecg-record-bytes 0` for automatic layout, or `4`/`8` for a declared
width study; `--ecg-record-minimum-mantissa-bits` sets a precision floor.

Direct Sniper runs require `--sniper-workload sg_kernel`, one core,
`--sniper-record-rss-mib 2048` or an explicitly larger bound, and translated
SIFT for the canonical comparison. Native runs require RV64 O3 and their
validated guest. No instruction or semantic-work cap may masquerade as a
complete workload.

| Control | Default | Meaning |
|---|---|---|
| `--ecg-record-max-carrier-bytes` | 268435456 | Record allocation cap |
| `--ecg-record-max-auxiliary-bytes` | 268435456 | Record-construction scratch cap |
| `--cache-record-rss-mib` | 2048 | Functional child-tree RSS limit |
| `--gem5-record-rss-mib` | 2048 | Native child-tree RSS limit |
| `--sniper-record-rss-mib` | 2048 | Modeled child-tree RSS limit |
| `--gem5-mem-size` | `4GB` | Native simulated physical memory |

Profiles override these defaults explicitly. The orchestrator additionally
bounds the complete matrix process tree, including native input sealing.
Use prepared `.sg` inputs and actual measured peaks for larger jobs.

The run lock protects manifest/preflight writes, execution and completion
publication. Use `--status --run-dir ...` for read-only monitoring.
Unchanged complete matrices resume by default; incomplete comparison groups
are retried together. Changed inputs invalidate resume eligibility.
Never repair a receipt by editing its hashes or bypassing its failed fields.

## 7. Focused checks and figures

The native VID/horizon probes use sparse backing rather than allocating
their logical graph domains:

```bash
python3 -m pytest -q scripts/test/test_gem5_record_cache.py \
  -k 'native_raw_data or native_adaptive_vid or large_horizon'
```

These are instruction-boundary checks, not large-graph performance.
Regenerate the public architecture figures only when their declarations
change:

```bash
python3 scripts/docs/generate_ecg_figures.py
python3 scripts/docs/generate_ecg_figures.py --check
python3 scripts/docs/check_wiki_figures.py
```

The shared fixture supplies the graph, CSR position, adaptive record and
cache decision for both SVG and Draw.io outputs. Paper figures keep their
separate geometry. The `ecg-public/v1` label is an asset schema, not a
research-method version.
