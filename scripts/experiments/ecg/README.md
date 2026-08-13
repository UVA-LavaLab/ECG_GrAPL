# K2 Experiment Package

## Main components

| Path | Responsibility |
|---|---|
| `configs/` | Versioned experiment specifications |
| `experiment_manifest.json` | Experiment profiles and stage definitions |
| `flows/experiment_run.py` | Resumable profile orchestration |
| `roi_matrix.py` | Simulator cell execution and metrics |
| `policy_specs.py` | Policy parsing and output labels |
| `flows/aggregate_results.py` | Complete-matrix aggregation |
| `analysis/final_campaign_gate.py` | Final role/count/provenance validation |
| `verify/` | Functional and cross-simulator checks |
| `slurm/` | Manifest-derived shard generation |

Public methodology and setup instructions are in:

- [`wiki/Evaluation-Methodology.md`](../../../wiki/Evaluation-Methodology.md)
- [`wiki/Reproduction.md`](../../../wiki/Reproduction.md)

Generated output belongs under `results/` and is not tracked.
