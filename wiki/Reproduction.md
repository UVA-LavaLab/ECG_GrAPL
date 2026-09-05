# Build and Reproduction

Datasets and experiment output live under `results/`. They, compiled binaries,
simulator checkouts and traces are untracked.

Choose the implementation path before launching a campaign:

| Goal | Format and recipe |
|---|---|
| small-ID functional cache behavior | Full14, default `8 reference / 2 state / 4 action`; [REF32 cache-quality probe](#ref32-cache-quality-probe) |
| large-ID / Twitter functional behavior | explicitly selected Scale6; use the large-graph recipe under the same probe section |
| actual record/retirement/LLC path | fixed native 26+6 ABI; [native replacement qualification](#native-scale6-replacement-qualification) |
| completed one-column baseline comparison | [single-epoch P-OPT comparison](#single-epoch-p-opt-comparison) |
| architecture explanation | regenerate the fixture-backed SVG/Draw.io pairs in [Section 3](#3-test) |

The ID headroom is graph-dependent, but the codecs do not automatically
allocate all unused bits. Named profiles pin their format and field widths.
Full14's default deadline width also limits the supported traversal length;
ID fit alone is insufficient. Native execution does not gain Full14 support
by running a smaller graph.

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

Build only the backend needed by the selected recipe. The following commands
keep the cache simulator and native PageRank path explicit and use one
compiler job; do not run simulator builds or large simulations concurrently.
Install Python dependencies only if they are not already available.

```bash
python3 -m pip install -r scripts/requirements.txt

make -j1 all-sim
make setup-gem5-guest-tools
timeout 7200 python3 scripts/setup_gem5.py --isa RISCV --jobs 1
timeout 1800 make -j1 gem5-riscv-m5ops-pr
```

The other kernels and Sniper recipes are for separately identified earlier
controls, not additional native REF32 implementations:

```bash
make -j1 gem5-riscv-m5ops-bfs \
  gem5-riscv-m5ops-sssp gem5-riscv-m5ops-bc gem5-riscv-m5ops-cc
make setup-sniper
make -j1 sniper-sg_kernel
```

`make all-sim` also builds `bench/bin_sim/reuse_plan_sidecar`. The earlier
ReusePlan gem5 flow caches generated sidecars under
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
`fig/ecg-figure-fixture.json`. The same fixture supplies the Full14 mask,
Scale6 word, returned F32 value, A/B next-use order and worked cache decision.
The C++ example checks use the actual encoder and victim-selection helpers:

```bash
python3 -m pytest -q scripts/test/test_wiki_figures.py
```

The separate paper collection is not regenerated by this command. Full14's
14-bit budget and the native fixed 26+6 ABI are intentionally shown separately.

Use the [architecture guide](ReusePlan-FlowThrough), the
[RISC-V instruction path](RISC-V-Instruction-Path), and the
[evidence boundary](Evaluation-Methodology) to interpret the generated figures.

## 4. Inspect the earlier PageRank study

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

## 6. Campaign recipes and current REF32 paths

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

### Native Scale6 replacement qualification

Build and exercise the native operand/retirement/replacement path
sequentially, with one compiler job and bounded execution:

```bash
ulimit -c 0
timeout 7200 python3 scripts/setup_gem5.py --isa RISCV --jobs 1
timeout 1800 make -j1 gem5-riscv-m5ops-pr
timeout 420 python3 -m pytest -q \
  scripts/test/test_gem5_ref32_cache.py::test_native_ref32_retirement_path_matches_isa_lru
```

This diagnostic pairs `--ref32-native --policy LRU` with
`--ref32-native --policy ECG --ecg-mode ECG_REF32`. Both execute the
same Scale6 instructions and fixed-iteration PageRank loop; only ECG
applies retirement metadata. The fixture is deliberately small, with
no generic or native prefetcher and FlowThrough off. It is not a
production timing-matrix row.

Capture defaults to the CPU's configured commit width, not one update:
`--ref32-capture-width 0` selects that default, while 1 through 16
select an explicit width. The link still delivers at most one update
per CPU cycle after at least eight cycles. Final receipts must expose
capture/output widths, exact work, accounting identities, zero drops
and an empty queue. Diagnostic `--ref32-allow-drops` is not admissible.
See [native integration](RISC-V-Instruction-Path) for the dedicated-port
assumptions and remaining prefetch/timing limits.

The one-edge, four-iteration regression deliberately uses a long but bounded
4096-cycle link delay. It forces two secondary updates to coalesce across
traversals, without increasing the 16-slot queue:

```bash
timeout 420 python3 -m pytest -q \
  scripts/test/test_gem5_ref32_cache.py::test_native_ref32_coalesces_across_traversals
```

Two optional file-backed cases cover directed Patents at 8 MiB for one
iteration and undirected Orkut at 64 KiB for three iterations. Starting
from the prepared final-n18 edge lists, create the small inputs:

```bash
for graph in cit-Patents com-Orkut; do
  sample="${graph}-native-n12"
  directory="results/graphs/${sample}"
  python3 scripts/experiments/ecg/flows/sample_realgraph.py \
    --input "results/graphs/${graph}-final-n18/${graph}-final-n18.el" \
    --output "${directory}/${sample}.el" \
    --vertices "${directory}/${sample}.vertices.tsv" \
    --metadata "${directory}/${sample}.sample.json" \
    --target-vertices 4096 --target-edges 16384
done
OMP_NUM_THREADS=1 bench/bin/converter \
  -f results/graphs/cit-Patents-native-n12/cit-Patents-native-n12.el \
  -m -o 5 -b results/graphs/cit-Patents-native-n12/cit-Patents-native-n12-dbg
OMP_NUM_THREADS=1 bench/bin/converter \
  -f results/graphs/com-Orkut-native-n12/com-Orkut-native-n12.el \
  -s -m -o 5 -b results/graphs/com-Orkut-native-n12/com-Orkut-native-n12-dbg
timeout 780 python3 -m pytest -q \
  scripts/test/test_gem5_ref32_cache.py::test_native_ref32_real_graph_pair
```

Every native pair retains `simulator.log`, guest receipts and `stats.txt`
in its pytest output directory. The comparison uses the first ROI stats
block and `system.cpu.commitStats0.numInsts`, not the unreset cumulative
`simInsts` value. A unique `--basetemp` under `results/` keeps these artifacts
outside pytest's rotating temporary directories. These are small mechanism
probes, not full-graph timing results or evidence that the LLC is capacity
stressed.

### REF32 cache-quality probe

REF32 requires a certified preordered `*-dbg.sg` graph, `-o 0`, a fixed
iteration horizon (`-t 0`), the accurate single-core cache simulator, and no
generic prefetcher. A focused Patents comparison is:

```bash
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --benchmark pr \
  --options \
    '-f results/graphs/cit-Patents-final-n18/cit-Patents-final-n18-dbg.sg -o 0 -n 1 -i 1 -t 0' \
  --policies LRU GRASP POPT:UNCHARGED POPT \
    ECG:REF32_R_COMMIT ECG:REF32_RP_COMMIT \
  --l1d-size 32kB --l1d-ways 8 \
  --l2-size 128kB --l2-ways 8 \
  --l3-sizes 512kB --l3-ways 16 \
  --cache-sim-omp-threads 1 --prefetcher none --flowthrough off \
  --out-dir results/ecg_experiments/probes/ref32_patents
```

Accept only rows with validated REF32 record, commit-channel, prefetch,
resource, DBG-order, geometry, policy, and semantic receipts. These runs
provide cache and traffic evidence only; they do not provide a speedup claim.

To reproduce the official GRASP PageRank example after cloning commit
`6e3814430265fc4f2513c95ef131a6522bc9d389`, add the missing `return 0;` to
`trace-based-simulators/common.h::add_border_boundry`, build the upstream LRU
and GRASP simulators, then compare with:

```bash
bench/bin_sim/grasp_trace_replay \
  results/external/grasp-upstream/datasets/\
PageRankOpt.web-Google.cvgr.dbg.lru.llc.trace 1 LRU

bench/bin_sim/grasp_trace_replay \
  results/external/grasp-upstream/datasets/\
PageRankOpt.web-Google.cvgr.dbg.lru.llc.trace 1 GRASP
```

Expected misses are 8,687,691 for LRU and 6,397,965 for GRASP.

Twitter-scale encoding is screened before converting the billion-edge graph by
using `ECG:REF32_SCALE_R_COMMIT` and
`ECG:REF32_SCALE_RP_COMMIT`. These policies force a 26-bit destination and the
six-bit scale token while retaining a four-byte edge record. On the full
directed Twitter graph, the runner enables the in-place two-pass builder, which
uses O(property-lines) auxiliary memory and emits progress receipts rather
than allocating O(edges) destination, distance, and lookahead arrays.

Run the full directed Twitter proof with:

```bash
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --benchmark pr \
  --options \
    '-f results/graphs/twitter-2010/twitter-2010-dbg.sg -o 0 -n 1 -i 1 -t 0' \
  --policies LRU SRRIP GRASP:PAPER POPT:UNCHARGED POPT \
    ECG:REF32_SCALE_R_COMMIT ECG:REF32_SCALE_RP_COMMIT \
  --l1d-size 32kB --l1d-ways 8 \
  --l2-size 128kB --l2-ways 8 \
  --l3-sizes 8MB --l3-ways 16 \
  --cache-sim-omp-threads 1 \
  --popt-reserve-model size_correct \
  --popt-property-bytes 4 --popt-active-columns 2 \
  --popt-num-epochs 256 --popt-matrix-stream analytic \
  --prefetcher none --flowthrough off \
  --out-dir results/ecg_experiments/runs/twitter_ref32
```

This 8 MiB run is the primary target configuration. `size_correct` reserves
enough ways for the two complete resident P-OPT columns; on Twitter that is 10
of 16 ways, not a fixed two-way reservation. `POPT:UNCHARGED` remains in the
matrix to separate replacement quality from that graph-scaled storage cost.

Require all rows to report one iteration, 1,468,364,884 semantic edges, and
score checksum `df4fdaf1e3957ce9`.

For the charged-P-OPT-positive comparison point, rerun the same command with:

```text
--l3-sizes 16MB
--out-dir results/ecg_experiments/runs/twitter_ref32_16mb_2dbb6680
```

At 16 MiB, size-correct P-OPT reserves five of 16 ways and leaves eleven data
ways. The expected `roi_matrix.json` SHA-256 is
`608370f0d2a9dd72d8319bcadfee2837c1a58bc34da734dc520d90d418f0a0e5`.

For the P-OPT paper's 24 MiB, 16-way LLC geometry, rerun the seven-policy
command with:

```text
--l3-sizes 24MB
--out-dir results/ecg_experiments/runs/twitter_ref32_24mb_6a1b9f29
```

The cache simulator uses the paper's modulo set mapping for this
non-power-of-two set count. Size-correct full P-OPT reserves four ways for
Twitter's current and next columns. The expected `roi_matrix.json` SHA-256 is
`a145ba982e8fcfaa198899382f7c026606a58647aa0d5b642b20d2d75a708d0d`.

The two-way result is a deliberately infeasible sensitivity, not a P-OPT
baseline. It is reproduced by preserving the 24 MiB cache's 24,576 sets while
exposing 14 data ways:

```bash
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --benchmark pr \
  --options \
    '-f results/graphs/twitter-2010/twitter-2010-dbg.sg -o 0 -n 1 -i 1 -t 0' \
  --policies POPT:UNCHARGED \
  --l1d-size 32kB --l1d-ways 8 \
  --l2-size 128kB --l2-ways 8 \
  --l3-sizes 21MB --l3-ways 14 \
  --cache-sim-omp-threads 1 \
  --prefetcher none --flowthrough off \
  --out-dir \
    results/ecg_experiments/runs/twitter_popt_24mb_fixed2_sensitivity \
  --timeout-cache 172800 --no-build
```

Add 10,413,060 matrix-stream transfers when comparing this diagnostic against
charged policies. Its expected `roi_matrix.json` SHA-256 is
`7dfbc7c7ff2c9104a6bc095694842a88023a86c78e804f13896eb364b0a77a53`.

### Single-epoch P-OPT comparison

`POPT_SE` and `POPT_SE_DISTANT` implement the paper's one-column format with
two explicitly disclosed interpretations of its unspecified post-final-use
case. The pinned public artifact does not include an SE implementation.
Keep both reconstructions in the roster and ordinary P-OPT as a separate baseline:

```bash
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --benchmark pr \
  --options \
    '-f results/graphs/twitter-2010/twitter-2010-dbg.sg -o 0 -n 1 -i 1 -t 0' \
  --policies LRU SRRIP GRASP:PAPER POPT:UNCHARGED POPT \
    POPT_SE POPT_SE_DISTANT \
    ECG:REF32_SCALE_R_COMMIT ECG:REF32_SCALE_RP_COMMIT \
  --l1d-size 32kB --l1d-ways 8 \
  --l2-size 128kB --l2-ways 8 \
  --l3-sizes 8MB 24MB --l3-ways 16 \
  --cache-sim-omp-threads 1 \
  --popt-reserve-model size_correct \
  --popt-property-bytes 4 --popt-active-columns 2 \
  --popt-num-epochs 256 --popt-matrix-stream analytic \
  --prefetcher none --flowthrough off \
  --out-dir results/ecg_experiments/runs/twitter_popt_se \
  --timeout-cache 5400 --no-build
```

The active-column setting above applies to ordinary P-OPT. SE always reserves
one column, yielding five ways at 8 MiB and two at 24 MiB. All SE rows must
report `popt_se_validated=1`, `popt_runtime_active_columns=1`, and the requested
`popt_se_postfinal`; the full-roster PageRank checksum must agree. The
complete matrix still streams once per iteration. Compare
`total_offchip_traffic_with_overhead` for reads plus writes plus analytic
matrix traffic, not `l3_misses` against a matrix-inclusive traffic total.

The completed 18-row run from implementation commit `d9ae0a6c` is archived at
`results/ecg_experiments/runs/twitter_popt_se_d9ae0a6c/roi_matrix.json`.
Its SHA-256 is
`6ee0e0c21bf582f55b0ef6a4c1d8c7544348558eaccc7b4cb9454c6352b2e124`,
also recorded in `roi_matrix.complete.json`. This identifies the archived
file; a fresh run has different path and timing fields. Its semantic work,
policy configuration and cache counters are the reproducible comparison.
All rows report one iteration, 1,468,364,884 semantic edges and checksum
`df4fdaf1e3957ce9`. Both Scale6 queues finish drained with zero capacity drops.

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

Configuration version 2 records a validation-only amendment. Under
`--flowthrough all`, symmetric structural FlowThrough supersedes the
candidate's duplicate static record FlowThrough path. The cache_sim row must
report this subsumption explicitly. The version-1 screen receipt and first
full-cache attempt are invalid and must not authorize or populate version 2;
the thresholds, policies, stage roster, and admissible claims are unchanged.

Configuration version 3 records the Sniper fused-binding correction. Distinct
vertices in one property cache line may carry different per-edge hints, so the
certified prefix now indexes the sideband by current source plus exact bound
property address and preserves every destination record. Marker-free fallback
remains line-granular under pure LRU replacement. All version-2 screen, cache,
iteration-8, and Sniper evidence is invalid and must not authorize or populate
version 3; thresholds, policies, stage roster, and admissible claims are
unchanged.

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
