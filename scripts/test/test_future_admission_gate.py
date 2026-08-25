import csv
import importlib.util
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
PATH = (
    ROOT / "scripts/experiments/ecg/analysis/future_admission_gate.py")
SPEC = importlib.util.spec_from_file_location("future_admission_gate_test", PATH)
GATE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


def write_rows(path: Path, admission_ratio: float) -> None:
    rows = []
    control = "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH"
    treatment = "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH"
    for graph in GATE.GRAPHS:
        for policy in GATE.POLICIES:
            ratio = admission_ratio if policy == treatment else 1.0
            rows.append({
                "final_graph": graph,
                "policy_label": policy,
                "status": "ok",
                "simulator": "cache_sim",
                "benchmark": "pr",
                "flowthrough": "all",
                "l3_misses": str(int(100000 * ratio)),
                "total_offchip_traffic": str(int(120000 * ratio)),
                "ecg_reuse_admission_updates":
                    "100" if policy in GATE.ADMISSION_POLICIES else "0",
                "ecg_record_bytes":
                    "4" if policy.startswith("ECG_") else "0",
            })
    assert any(row["policy_label"] == control for row in rows)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def test_future_admission_gate_promotes_only_above_threshold(tmp_path):
    passing = tmp_path / "pass.csv"
    write_rows(passing, 0.94)
    decision = GATE.evaluate(passing)
    assert decision["decision"] == "PROMOTE"
    assert decision["contrasts"]["future_admission_recency"]["pass"] is True
    assert decision["aggregate"] == (
        "unweighted_geometric_mean_of_per_graph_ratios")

    tied = tmp_path / "tie.csv"
    write_rows(tied, 0.98)
    decision = GATE.evaluate(tied)
    assert decision["decision"] == "STOP"
    assert decision["promote_to_gem5"] is False


def test_future_admission_gate_rejects_incomplete_roster(tmp_path):
    path = tmp_path / "missing.csv"
    write_rows(path, 0.94)
    rows = list(csv.DictReader(path.open()))
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows[:-1])
    with pytest.raises(ValueError, match="missing rows"):
        GATE.evaluate(path)
