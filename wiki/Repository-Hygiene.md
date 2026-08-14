# Repository Hygiene

The tracked repository should contain source code, experiment definitions,
tests, and public documentation only.

## Push these files

- cache and simulator integration source under `bench/include/`;
- graph kernels under `bench/src_sim/`, `bench/src_gem5/`, and
  `bench/src_sniper/`;
- synthesizable ReusePlan cost-model RTL under `bench/src_rtl/`;
- setup, experiment, analysis, and verification scripts under `scripts/`;
- versioned experiment configurations under
  `scripts/experiments/ecg/configs/`;
- tests under `scripts/test/`;
- README, wiki pages, SVG figures, Makefile, and contribution guidance.

## Keep these files local

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

## Before pushing

```bash
python3 -m pytest -q \
  scripts/test/test_repository_hygiene.py \
  scripts/test/test_public_docs.py
git diff --check
git status --short
```

`make clean-all` removes generated binaries, build trees, simulator scratch
output, and pytest cache. It deliberately preserves both local `research/`
material and experiment data under `results/`.
