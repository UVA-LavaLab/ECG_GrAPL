# Contributing to ECG Next

Changes should directly support ReusePlan, FlowThrough, their baselines, or
reproducibility.

## Rules

1. Keep experiment configuration in
   `scripts/experiments/ecg/experiment_manifest.json`.
2. Keep public design and methodology documentation under `wiki/`.
3. Use the policy roster from the versioned experiment configuration; do not
   add or remove policies silently.
4. Preserve policy isolation: ambient ECG variables must not contaminate
   baselines.
5. Do not commit generated `results/`, graph datasets, simulator checkouts, or
   binaries.
6. Do not publish preliminary performance tables.

## Validation

```bash
pytest -q scripts/test
python3 -m py_compile \
  scripts/experiments/ecg/roi_matrix.py \
  scripts/experiments/ecg/flows/experiment_run.py \
  scripts/experiments/ecg/flows/aggregate_results.py
git diff --check
```

For simulator or policy changes, also run the focused equivalence gates in
[`wiki/Reproduction.md`](wiki/Reproduction.md).
