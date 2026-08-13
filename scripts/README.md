# ECG Scripts

## Canonical entry points

| Script | Purpose |
|---|---|
| `setup_gem5.py` | Install ECG gem5 overlays and build simulator targets |
| `setup_sniper.py` | Install ECG Sniper overlays and build simulator targets |
| `experiments/ecg/roi_matrix.py` | Execute one cache_sim, gem5, or Sniper matrix |
| `experiments/ecg/flows/experiment_run.py` | Expand and resume manifest-defined experiment jobs |
| `experiments/ecg/flows/aggregate_results.py` | Aggregate complete jobs into tables and figures |
| `experiments/ecg/slurm/make_slurm_shards.py` | Generate policy-isolated Slurm rows |
| `experiments/ecg/verify/` | Exact policy and cross-simulator correctness gates |

New behavior belongs in `experiments/ecg/flows/`, `policy_specs.py`, `verify/`,
or `slurm/`; do not add duplicate top-level wrappers.
