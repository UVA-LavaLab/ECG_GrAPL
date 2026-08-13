# ECG Tests

Run all retained cache-policy, simulator-scaffold, and experiment-workflow tests:

```bash
pytest -q scripts/test
```

The suite covers shared ECG victim policy, K2/StreamShield isolation, gem5 and
Sniper scaffolds, charged P-OPT accounting, prefetch behavior, and manifest/run
completion safety.
