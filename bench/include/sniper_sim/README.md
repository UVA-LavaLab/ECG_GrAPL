# Sniper ECG Backend

This directory contains the tracked Sniper ECG artifact:

- `configs/graphbrew/` — cache hierarchy and policy configuration;
- `overlays/` — ECG, GRASP, P-OPT, StreamShield, and prefetch support;
- `sniper_harness.h` — ROI, sideband, K2, and StreamShield benchmark support.

The Sniper checkout is ignored and pinned by `scripts/setup_sniper.py` to:

```text
56505e42fd98bca863fac181e769bd3c98d2bb33
```

```bash
python3 scripts/setup_sniper.py --apply-overlays
make sniper-sg_kernel
```

Run experiment matrices through
`scripts/experiments/ecg/flows/experiment_run.py`.
