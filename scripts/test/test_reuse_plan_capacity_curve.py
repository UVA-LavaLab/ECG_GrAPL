import csv
from pathlib import Path

import pytest

from scripts.experiments.ecg.analysis.reuse_plan_capacity_curve import analyze


POLICIES = (
    "LRU", "SRRIP", "GRASP", "HAWKEYE_PROXY", "POPT",
    "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE",
    "ECG_REUSE_PLAN_FLOWTHROUGH", "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH",
)


def write_matrix(path: Path, reuse_plan_ways: int, multiplier: float) -> None:
    fields = [
        "final_graph", "benchmark", "policy_label", "status",
        "l3_effective_ways", "l3_misses", "l3_misses_with_overhead",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for graph in ("g1", "g2", "g3"):
            for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
                for policy in POLICIES:
                    base = 1000
                    misses = (
                        round(base * multiplier)
                        if policy.startswith("ECG_REUSE_PLAN") else base)
                    ways = (
                        reuse_plan_ways if policy.startswith("ECG_REUSE_PLAN")
                        else 15 if policy == "POPT" else 16)
                    writer.writerow({
                        "final_graph": graph,
                        "benchmark": benchmark,
                        "policy_label": policy,
                        "status": "ok",
                        "l3_effective_ways": ways,
                        "l3_misses": misses,
                        "l3_misses_with_overhead": misses,
                    })


def test_capacity_curve_requires_matched_baselines(tmp_path: Path):
    p16, p15, p14 = (
        tmp_path / "16.csv", tmp_path / "15.csv", tmp_path / "14.csv")
    write_matrix(p16, 16, 0.9)
    write_matrix(p15, 15, 0.95)
    write_matrix(p14, 14, 1.0)
    summary, cells = analyze(p16, p15, p14)
    assert summary["status"] == "passed"
    assert len(cells) == 60
    policy = summary["policies"]["ECG_REUSE_PLAN"]
    assert policy["geomean_miss_ratio_vs_lru"]["16"] == pytest.approx(0.9)
    assert policy["capacity_penalty_percent_vs_16"]["14"] == pytest.approx(
        100 * (1 / 0.9 - 1))

    rows = list(csv.DictReader(p14.open()))
    rows[0]["l3_misses"] = "1001"
    rows[0]["l3_misses_with_overhead"] = "1001"
    with p14.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    with pytest.raises(ValueError, match="baselines drift"):
        analyze(p16, p15, p14)
