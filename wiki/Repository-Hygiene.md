# Repository Hygiene

The tracked repository should contain source code, experiment definitions,
tests, and public documentation only.

## Track these files

- cache and simulator integration source under `bench/include/`;
- graph kernels under `bench/src_sim/`, `bench/src_gem5/`, and
  `bench/src_sniper/`;
- synthesizable ReusePlan cost-model RTL under `bench/src_rtl/`;
- setup, experiment, analysis, and verification scripts under `scripts/`;
- versioned experiment configurations under
  `scripts/experiments/ecg/configs/`;
- tests under `scripts/test/`;
- README, wiki pages, generated SVG figures, editable Draw.io mirrors,
  `fig/ecg-figure-fixture.json`, figure generators/validators, Makefile, and
  contribution guidance.

## Leave these paths untracked

- `research/` drafts, notes, literature copies, and private working material;
- `results/` simulator output and aggregate tables;
- graph datasets (`*.el`, `*.wel`, `*.sg`, `*.mtx`);
- simulator checkouts under `bench/include/gem5_sim/gem5/` and
  `bench/include/sniper_sim/snipersim/`;
- compiled binaries under `bench/bin*`;
- `build/`, `m5out/`, `sim.out/`, `sniper.out/`, logs, and virtual
  environments.

The setup scripts recreate simulator checkouts:

```bash
make setup-gem5
make setup-gem5-guest-tools
make setup-sniper
```

The build and experiment commands are in [Reproduction](Reproduction).

The public `wiki/` files are the documentation source; generated SVGs and
their editable mirrors are produced together. Keep the shared example tied
to the actual encoders and victim helper. Downloaded architecture papers and
temporary render previews are reference material, not assets to copy into
the tracked figure set. The separate paper collection retains its own layout
and implementation scope.

## Before updating the public repository

```bash
python3 -m pytest -q \
  scripts/test/test_repository_hygiene.py \
  scripts/test/test_public_docs.py \
  scripts/test/test_wiki_figures.py
python3 scripts/docs/generate_ecg_figures.py --check
python3 scripts/docs/check_wiki_figures.py
git diff --check
git status --short
```

`make clean-all` removes generated binaries, build trees, simulator scratch
output, and pytest cache. It deliberately preserves both local `research/`
material and experiment data under `results/`.
