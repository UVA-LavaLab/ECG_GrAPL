# gem5 ECG Backend

This directory contains the tracked ECG gem5 artifact:

- `configs/graphbrew/` — SE-mode cache hierarchy and policy configuration;
- `overlays/` — ECG/GRASP/P-OPT policies, request extensions, prefetchers, and
  RISC-V custom instruction definitions;
- `gem5_harness.h` — benchmark-side sideband and custom-instruction wrappers.

The gem5 source checkout is ignored and installed by:

```bash
python3 scripts/setup_gem5.py --isa X86 RISCV
```

Build a PageRank wrapper:

```bash
make gem5-m5ops-pr
make gem5-riscv-m5ops-pr
```

Run experiment cells through `scripts/experiments/ecg/roi_matrix.py` or the
manifest-driven `scripts/experiments/ecg/flows/experiment_run.py`.
