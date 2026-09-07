# Build and Reproduction

Datasets and experiment output live under `results/`. They, compiled binaries,
simulator checkouts and traces are untracked.

Choose the implementation path before launching a campaign:

| Goal | Current recipe |
|---|---|
| bounded cross-backend semantics | `ecg_current_equivalence`, the default profile; prepare its small corpus first |
| large functional exploration | `ecg_large_cache`; accurate current mechanisms, no final authorization |
| detailed final evaluation | `ecg_detailed_final`, explicit `--final-stage` and current `--equivalence-receipt` |
| functional cache behavior | `ECG`, `ECG:replacement`, `ECG:prefetch`, or `ECG:transport` with graph-derived layout |
| record-width control | `--ecg-record-bytes 0`, `4`, or `8`; optional `--ecg-record-minimum-mantissa-bits` |
| native record/retirement/LLC path | `--ecg-native --ecg-mechanism ... --ecg-record-bytes ...`; serial fixed-iteration PageRank only |
| historical one-column baseline | [single-epoch P-OPT comparison](#single-epoch-p-opt-comparison) |
| architecture explanation | regenerate the fixture-backed SVG/Draw.io pairs in [Section 3](#3-test) |

The current codec uses the maximum encoded vertex ID, record count, requested
width, and minimum mantissa precision. Width zero tries four bytes and then
the explicit eight-byte escape. Receipts must report all layout fields,
64-bit sequence/deadline widths, source/carrier/auxiliary storage, active
mechanisms, queue/traffic accounting, and matching semantic work.

## 1. Prepare graph data

Start with the small current corpus, without downloading or converting a large
graph:

```bash
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py
python3 scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py --check
```

The checked figure fixture retains all 32 vertices, including isolated vertices,
and its 34 adjacency records. The deterministic directed spread graph has
512 vertices and 2,048 records. Their serialized `.sg` bytes, recipe inputs,
and corpus receipt live under `results/graphs/ecg-current-equivalence/`.
Preparation refuses to overwrite changed artifacts; `--force` is an explicit
regeneration request, not an automatic recovery path.

### Larger retained datasets

Prepare the literature-scale corpus only for a separately selected larger run:

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

Build only the backend needed by the selected recipe. The bounded equivalence
profile needs all three current PageRank guests. On an already configured host:

```bash
make -j1 sim-pr sniper-sg_kernel gem5-riscv-m5ops-pr
```

Do not run simulator builds or simulations concurrently. For an unconfigured
host, use the repository setup flows for the missing backends; install Python
dependencies only if they are not already available.

```bash
python3 -m pip install -r scripts/requirements.txt

make -j1 all-sim
make setup-gem5-guest-tools
timeout 7200 python3 scripts/setup_gem5.py --isa RISCV --jobs 1
timeout 1800 make -j1 gem5-riscv-m5ops-pr
```

Sniper setup and its current PageRank guest use:

```bash
make setup-sniper PARALLEL=1
make -j1 sniper-sg_kernel
```

Other kernels remain separately identified earlier controls, not additional
current native ECG implementations.

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
`fig/wiki_src/**/*.drawio` files from one declaration. Fixture-backed values
come from `fig/ecg-figure-fixture.json`: the graph view, cache-line reuse
timeline, adaptive layout, `0x20000052` record, unchanged F32 value, A/B
next-use order, and worked cache decision. C++ checks compile against the
actual current codec, record-window selector, and victim helper:

```bash
python3 -m pytest -q scripts/test/test_wiki_figures.py
```

The separate paper collection is not regenerated by this command. The
`ecg-public/v1` SVG schema is an asset-format contract, not a research-method
version.

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

## 5. Historical campaign runner

This retained profile belongs to the earlier ReusePlan study, not the current
adaptive ECG method. Use the current commands in section 6 for the new record path.

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

## 6. Current qualification and historical campaign recipes

### Default bounded cross-backend workflow

After corpus preparation and guest builds:

```bash
ulimit -c 0
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_current_equivalence \
  --run-dir results/ecg_experiments/runs/ecg_current_equivalence \
  --no-build

python3 scripts/experiments/ecg/record_equivalence_gate.py \
  --validate-receipt \
  results/ecg_experiments/runs/ecg_current_equivalence/current_ecg_equivalence.complete.json
```

The profile is the runner's default. It executes 12 sequential matrix jobs,
36 rows in total: fixture32 uses transport, replacement, prefetch, and combined
mechanisms; spread512 uses transport and combined. Both use four- and
eight-byte records on cache_sim, RV64 O3 gem5, and translated SIFT Sniper.
Every backend consumes the same prepared graph at `-o 0 -n 1 -i 2 -t 0`.
The small 1 KiB cache geometry is intentional mechanism pressure, not a paper
performance configuration. The spread rows must produce real positive
prefetch candidates, requests, and completions in each backend and width.

Per-cell wall limits are 30-120 seconds. Each complete matrix process tree also
has a wall bound and a 2,048 MiB sampled RSS ceiling, covering graph inspection,
native input sealing, simulator descendants, and their child watchdogs. Jobs
use repository-local locks and `<matrix-output>/scratch`, not shared temporary
trace directories. Run from a clean worktree; successful unchanged jobs can
resume, but a filtered invocation cannot issue full equivalence authorization.

The reference-host run at `106c1227` completed all 36 rows in 604.264 seconds.
An additional whole-workflow watchdog measured a peak sampled RSS of
910.945 MiB, below its 2,048 MiB limit, and exited with no surviving descendants.
These are host execution/resource observations for the instrumented semantic
workflow, not target speedup measurements or guarantees for other machines.
After the native associativity-forwarding correction, the fresh `81e7e08f`
run is retained separately as `ecg_current_equivalence_preliminary`; it
completed in 630.590 seconds with 910.742 MiB peak sampled RSS. Its receipt
matches the current code; the earlier `106c1227` run is preserved as prior
evidence rather than relabeled.

The dedicated receipt re-expands the canonical roster, replays current receipt
validators against raw backend logs, and binds graph recipes, layouts, semantic
digests, source/configuration files, binaries, and outputs. A changed method or
runtime requires a fresh small run. A generic `run.complete.json`, historical
screen receipt, or manually copied `valid` flag is insufficient.

### Large exploration and explicit final runs

The local release gate adds ordinary CSR/reference policies to all four
current ECG mechanisms on the six retained, preordered full core graphs:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_local_release_cache \
  --run-dir results/ecg_experiments/runs/local_release_cache --no-build
```

This is a serial 60-row cache_sim matrix: two fixed PageRank iterations,
32 KiB L1D / 256 KiB L2 (eight ways each), and an 8 MiB / 16-way LLC.
It includes LRU, SRRIP, GRASP_PAPER, size-correct P-OPT, both disclosed
P-OPT-SE reconstructions, and transport/replacement/prefetch/combined ECG.
P-OPT matrix streams are simulated rather than added as free-latency traffic.
The per-matrix RSS cap is 8,192 MiB and each policy has a 3,600-second wall
limit. Successful complete graph matrices can resume; an incomplete matrix
must be retried as a complete comparison group. Twitter is a separate scale
stress case, not silently included in this core gate.
The completed `8d50ec7c` run contains all 60 rows and is retained at that source
revision. It took 6,734.789 host seconds and peaked at 1,994.219 MiB sampled
RSS. See the [full-core results](Evaluation-Methodology#43-full-core-local-release-results),
including the web-Google regression against GRASP_PAPER.

`--current-pr-baselines` selects the common fixed PageRank workload for the
ordinary cache_sim policies. It uses the same non-fused F32 arithmetic,
warm-up and explicit incoming/outgoing CSR-index reads as the current record
path, but reads ordinary four-byte source IDs instead of allocating a record
carrier. Mixed current/legacy workloads cannot be certified as a comparison.
Current PageRank build targets explicitly disable FP contraction; native guest
builds retain their strict compiler/configuration receipts. Only requested
RISC-V guest dependency files are loaded by a targeted build.

The prepared large graph set is declared separately in the manifest. The
functional tier retains accurate current record/window/traffic behavior; it
does not switch to a reduced-behavior fast path:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_large_cache \
  --run-dir results/ecg_experiments/runs/ecg_large_cache --no-build
```

This tier is independent and non-authorizing. The final detailed tier is never
selected automatically; it requires both an explicit request and the freshly
revalidated small-run receipt:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_detailed_final --final-stage \
  --equivalence-receipt \
  results/ecg_experiments/runs/ecg_current_equivalence/current_ecg_equivalence.complete.json \
  --run-dir results/ecg_experiments/runs/ecg_detailed_final --no-build
```

Inspect a larger profile with `--list` before executing it. Its declared host,
carrier, auxiliary, and native target-memory budgets are distinct; adjust them
deliberately rather than disabling guards. Small equivalence establishes
semantic confidence, not a guarantee of large-graph runtime, memory fit,
performance benefit, or complete final-paper baseline coverage.

The separate `ecg_current_equivalence_extended` profile optionally repeats
transport/combined, eight-byte real-Orkut-4096 rows on all three backends.
It is not part of the quick roster and cannot replace its authorization.

The uninstrumented current sample pairs are archived in
`results/ecg_experiments/runs/current_preliminary/`, grouped by backend and
sample. Their common controls are `--policies ECG:transport ECG`,
`--ecg-record-bytes 0`, `-o 0 -n 1 -i 2 -t 0`, L1D 4 KiB / eight ways,
L2 8 KiB / eight ways, and LLC 16 KiB / 16 ways. Native cells use RV64 O3;
Sniper cells use translated SIFT. Do not add `--ecg-equivalence` when
reproducing their timing numbers. See the
[results and limitations](Evaluation-Methodology#41-current-sampled-preliminary-results).

### Current Twitter archive reproduction

The explicit `ecg_twitter_reproduction` profile reruns the historical
one-iteration Twitter controls with the current implementation:

```bash
python3 -I scripts/experiments/ecg/flows/experiment_run.py \
  --profile ecg_twitter_reproduction \
  --run-dir results/ecg_experiments/runs/current_twitter_reproduction --no-build
```

It consumes the unchanged directed `twitter-2010-dbg.sg` graph (41,652,230
vertices and 1,468,364,884 records) with `-o 0 -n 1 -i 1 -t 0`.
L1D is 32 KiB / eight ways, L2 is 128 KiB / eight ways, and the LLC settings
are 8 and 24 MiB / 16 ways, matching the archive. The nine historical roles
are retained with current ECG replacement/combined implementations, plus the
required current transport control: 20 cells in total. Transport runs first
to exercise full-scale record allocation before the remaining controls.

P-OPT traffic is **analytic in this reproduction**, matching the historical
archive rather than the simulated-column mode of `ecg_local_release_cache`.
Current fixed-CSR access accounting and non-fused F32 arithmetic remain
enabled. Changed-model counts are compared with the archive, not forced to
equal it; this profile is not native timing or final-run authorization.

The full graph resolves automatically to ID26/M6/H31/m0 and four-byte records.
Its carrier alone needs 5,873,459,536 bytes; the initial preflight reservation
was 19,795,600,521 host bytes. That heuristic is not a peak-allocation bound:
the completed run measured 25,020.707 MiB peak process-tree RSS. Use the
measured peak and headroom when sizing the next run. The profile declares a
6 GiB carrier limit, 1 GiB construction-scratch limit, 32 GiB process-tree RSS
cap, two hours per policy, and twelve hours for the whole matrix. Run serially
and retain host memory headroom. A Slurm allocation must exceed the profile
RSS cap rather than relying on the wrapper's default 32 GiB allocation.
The old `twitter_popt_se_d9ae0a6c` archive is never overwritten.

The current reproduction completed all 20 cells in 23,681.348 host seconds.
All rows match the current full-work checksum; relative improvements over
full-capacity P-OPT persist, but the changed implementation/accounting does
not reproduce identical historical counts. See the
[current and archived comparison](Evaluation-Methodology#44-current-twitter-reproduction).

### Lab-node handoff

Rebuild the selected guests on the lab node rather than assuming a workstation
`-march=native` binary is portable. Keep compiler jobs low (`make -j1` and
`PARALLEL=1` when building prerequisite libraries). The lab must create its own
complete equivalence receipt: paths, code, binaries, graphs, and output hashes
are bound to the executing checkout.

From the repository root, the current whole-profile Slurm mode is:

```bash
mkdir -p results/slurm_logs
sbatch --export=ALL,GRAPHBREW_SLURM_MODE=current-whole-profile,GRAPHBREW_CURRENT_PROFILE=ecg_current_equivalence,GRAPHBREW_CURRENT_RUN_TAG=lab_equivalence \
  scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch
```

This runs one serial whole profile, not an array or a filtered shard. The
wrapper first executes `lab_runtime_preflight.py` under a 512 MiB / 60-second
watchdog on the allocated node. It checks the required tool/runtime files,
ASLR control and, for gem5 profiles, an actual tiny FUSE mount/read/unmount.
Missing prerequisites stop the job before experiment execution. File
availability is not proof of CPU compatibility or SDE correctness; the
following complete small run is still required.

The same mode accepts `ecg_local_release_cache`, `ecg_large_cache`,
`ecg_twitter_reproduction`, and `ecg_detailed_final`. A final launch additionally requires
`GRAPHBREW_FINAL_STAGE=1` and `GRAPHBREW_EQUIVALENCE_RECEIPT` pointing to the
lab-generated `current_ecg_equivalence.complete.json`. The experiment runner
remains the authoritative validator. Size the allocation's wall/RSS budgets
for the selected whole profile; the wrapper does not increase or disable the
profile's limits.

The experiment lock now covers manifest/preflight writes, job execution, and
completion-receipt publication. A duplicate run, list, dry-run or graph-check
invocation cannot rewrite an active run's artifacts or erase its lock-owner
record. Use `--status` for read-only monitoring. Resume eligibility still
requires unchanged material inputs; changing the runner does not relabel an
older completed matrix as evidence from the new revision.

Historical TSV shards remain a separate mode. The legacy shard generator
rejects current whole profiles rather than emitting jobs that would fail
current authorization. Do not add `--only`, graph/policy filters, or an old
screen receipt to bypass this distinction.

### Current mechanism controls

Current public policy names are `ECG` (replacement-prefetch),
`ECG:replacement`, `ECG:prefetch`, and `ECG:transport`, producing
`ECG`, `ECG_REPLACEMENT`, `ECG_PREFETCH`, and `ECG_TRANSPORT`.
Use the same graph/order, one thread, positive fixed iteration count, `-t 0`,
and FlowThrough off. Native modeled-time comparisons require a successful
matching `ECG_TRANSPORT` row in the same group. The runners enforce layout,
work, queue, traffic, and completion receipts; do not bypass those checks.

### Shared input diagnostics and resource budgets

Add `--ecg-equivalence` to `roi_matrix.py` only for semantic diagnostics.
It requires a prepared unweighted `.sg` with at most 4,096 vertices and
65,536 adjacency records. Every backend consumes the same bytes with explicit
`-o 0 -n 1 -i 2 -t 0`; isolated vertices are part of the graph and must not be
discarded during preparation. All current file-backed runs require `-o 0`
so resource preflight and execution use the same encoded IDs.

The observer reports complete actual-load fingerprints, not a reconstructed
host-side execution trace. Compare layout, carrier, consumed semantics, and
reference-window digests within each width. Compare source order, destination
stream, property-read count, and PageRank checksum across widths. Never require
equal aggregate misses, cycles, or actual prefetch issue counts across models.
Diagnostic rows have `timing_valid_for_speedup=0`; a standalone matrix
completion is not final-experiment authorization.

The current runner exposes these independent limits:

| Control | Default | Meaning |
|---|---|---|
| `--ecg-record-max-carrier-bytes` | 268435456 | carrier allocation cap |
| `--ecg-record-max-auxiliary-bytes` | 268435456 | construction scratch cap |
| `--cache-record-rss-mib` | 2048 | functional child process-tree RSS budget |
| `--gem5-record-rss-mib` | 2048 | native child process-tree RSS budget |
| `--sniper-record-rss-mib` | 2048 | modeled child process-tree RSS budget |
| `--gem5-mem-size` | `4GB` | native simulated physical memory |

Preflight scans the serialized graph in bounded chunks, validates CSR domains,
and resolves the width from actual pull-source IDs. Its conservative memory
reservation includes the retained graph, carrier, configured auxiliary cap,
and process margin; gem5 also reserves a sealed graph copy. This is not a
measured peak or a whole-host memory guarantee. Direct `roi_matrix.py` calls
bound the launched child tree; the tiered orchestrator additionally bounds the
whole matrix job, including native input sealing. Larger graphs require prepared
inputs and appropriately sized explicit limits, not an unbounded generator.

Direct gem5 graph runs use:

```text
--ecg-native
--ecg-mechanism transport|replacement|prefetch|replacement-prefetch
--ecg-record-bytes 0|4|8
--ecg-minimum-mantissa-bits N
```

The native path supports serial fixed-iteration PageRank. Its completed focused
qualification covers raw4/raw8 and all four mechanisms, plus real Patents at
exact 24 MiB/16 ways and an Orkut wide-record pressure case. These runs do not
constitute the final paper campaign or a silicon-area result.

The bounded native VID/address probes use only a few backed properties even
when the logical ID domain reaches unsigned 32 bits:

```bash
make -j1 gem5-riscv-m5ops-record_isa_smoke
python3 -m pytest -q scripts/test/test_gem5_record_cache.py \
  -k 'native_raw_data or native_adaptive_vid'
```

They cover automatic four/eight-byte selection and actual high-ID property
loads, not a full-size graph-loader run. Current native matrix runs also
forward explicit `--l1d-ways` and `--l2-ways`; keep these equal across matched
controls and inspect the emitted cache configuration when defining a new cell.

Current Sniper rows must use:

```text
--suite sniper
--sniper-workload sg_kernel
--sniper-cores 1
--sniper-record-rss-mib 2048
--ecg-record-bytes 0|4|8
--ecg-record-minimum-mantissa-bits N
```

The workload must be uncapped fixed PageRank (`-n 1`, positive `-i`, `-t 0`)
with FlowThrough off and true modulo LLC indexing. No unsafe or allow-drop
flag is admissible. Current strict admission has passed for all four Sniper
mechanisms, including 4-byte live virtual transport and 8-byte SIFT operation
with actual translation under 4 KiB and exact 24 MiB/16-way geometries.
Receipts require matching work/checksum, closed transport and prefetch
accounting, positive traffic when prefetch is required, zero required loss,
and empty final queues.

Sniper retains `source_stream_bytes` as `retained_source_bytes`. The guest
software window is always 16 `uint64_t` values (1,024 data bits), plus
1,024 index bits and 16 validity bits, even when records are four bytes.
The separate runtime bank is `16 * 257` bits. Its update link is
`bounded-completion-corroboration`, not architectural retirement.

The managed Sniper frontend uses these narrow environment controls:

```text
SNIPER_ECG_RECORD_MECHANISM
SNIPER_ECG_RECORD_BYTES
SNIPER_ECG_RECORD_MINIMUM_MANTISSA_BITS
SNIPER_ECG_RECORD_UPDATE_LATENCY
SNIPER_ECG_RECORD_CAPTURE_WIDTH
SNIPER_ECG_RECORD_PREFETCH_QUEUE
SNIPER_ECG_RECORD_PREFETCH_LATENCY
SNIPER_ECG_RECORD_LOOKUP_LATENCY
SNIPER_ECG_RECORD_DRAIN_MAX_CYCLES
SNIPER_ECG_RECORD_LINE_BYTES=64
SNIPER_ECG_RECORD_WARM_PROPERTIES
```

Native timing is admissible only when the same run group contains a successful
matching `ECG_TRANSPORT` control with identical layout and work. The sealed
24 MiB 4-byte transport/combined pair and wide real-Orkut production pair
both completed successfully. Sniper's translated Patents and Orkut production
pairs also completed at both record widths. These establish the implementation
scope, not a final campaign conclusion.

For example, run the current functional mechanisms together:

```bash
make -j1 sim-pr
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite cache-sim --benchmark pr \
  --policies ECG:transport ECG:replacement ECG:prefetch ECG \
  --options "-g 8 -k 2 -o 5 -n 1 -i 2 -t 0" \
  --l3-sizes 24MB --l3-ways 16 --ecg-record-bytes 0 \
  --out-dir results/ecg_experiments/runs/current_functional --no-build
```

Select the native ISA and its immutable guest for an architectural pair:

```bash
make -j1 gem5-riscv-m5ops-pr
GEM5_OPT="$PWD/bench/include/gem5_sim/gem5/build/RISCV/gem5.opt" \
GEM5_KERNEL_SUFFIX=_riscv_m5ops \
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite gem5 --benchmark pr --gem5-cpu-type O3 \
  --policies ECG:transport ECG \
  --options "-g 8 -k 2 -o 5 -n 1 -i 2 -t 0" \
  --l3-sizes 24MB --l3-ways 16 --ecg-record-bytes 0 \
  --out-dir results/ecg_experiments/runs/current_native --no-build
```

The corresponding bounded Sniper model uses the canonical workload:

```bash
make -j1 sniper-sg_kernel
python3 scripts/experiments/ecg/roi_matrix.py \
  --suite sniper --benchmark pr --sniper-workload sg_kernel \
  --sniper-cores 1 --sniper-address-domain translated \
  --sniper-queue-model windowed_mg1 --sniper-record-rss-mib 2048 \
  --policies ECG:transport ECG \
  --options "-g 8 -k 2 -o 5 -n 1 -i 2 -t 0" \
  --l3-sizes 24MB --l3-ways 16 --ecg-record-bytes 0 \
  --out-dir results/ecg_experiments/runs/current_sniper --no-build
```

The remaining recipes in this section preserve historical campaigns. Their
old policy labels, fixed encodings, commits, hashes, measurements and
limitations are retained for reproduction and are not renamed as current
adaptive results.

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

### Historical fixed-26+6 native replacement qualification

This recipe is retained for the earlier fixed-layout implementation. It does
not exercise the current adaptive raw4/raw8 ISA.

Build and exercise the native operand/retirement/replacement path
sequentially, with one compiler job and bounded execution:

```bash
ulimit -c 0
timeout 7200 python3 scripts/setup_gem5.py --isa RISCV --jobs 1
timeout 1800 make -j1 gem5-riscv-m5ops-pr
timeout 420 python3 -m pytest -q \
  scripts/test/test_gem5_ref32_cache.py::test_native_ref32_retirement_path_matches_isa_lru
```

This historical diagnostic paired `--ref32-native --policy LRU` with
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
See [native integration](RISC-V-Instruction-Path) for the current ISA,
prefetch, and evidence boundaries.

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

### Historical fixed-format cache-quality probe

The commands and expected results below remain tied to the recorded
`ECG_REF32_*` implementation and provenance.

Historical REF32 requires a certified preordered `*-dbg.sg` graph, `-o 0`, a fixed
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
