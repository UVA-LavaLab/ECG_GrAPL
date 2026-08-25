import csv
import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "scripts/experiments/ecg/analysis/online_admission_gate.py"
SPEC = importlib.util.spec_from_file_location("online_admission_gate_test", PATH)
GATE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


def write_rows(path: Path, online_ratio: float) -> None:
    rows = []
    for graph_index, graph in enumerate(GATE.GRAPHS):
        for policy in GATE.POLICIES:
            value = 100000
            if policy == GATE.FUTURE:
                value = 80000 if graph_index in (2, 3) else 120000
            elif policy == GATE.ONLINE:
                best = 80000 if graph_index in (2, 3) else 100000
                value = int(best * online_ratio)
            rows.append({
                "final_graph": graph,
                "policy_label": policy,
                "status": "ok",
                "simulator": "cache_sim",
                "benchmark": "pr",
                "flowthrough": "all",
                "l3_misses": value,
                "total_offchip_traffic": value,
                "ecg_admission_leader_accesses_grasp":
                    2048 if policy == GATE.ONLINE else 0,
                "ecg_admission_leader_accesses_future":
                    2048 if policy == GATE.ONLINE else 0,
                "ecg_admission_leader_misses_grasp":
                    500 if policy == GATE.ONLINE else 0,
                "ecg_admission_leader_misses_future":
                    300 if policy == GATE.ONLINE else 0,
                "ecg_admission_follower_selections_grasp":
                    1000 if policy == GATE.ONLINE and graph_index < 2 else 0,
                "ecg_admission_follower_selections_future":
                    1000 if policy == GATE.ONLINE and graph_index >= 2 else 0,
                "ecg_admission_completed_windows":
                    1 if policy == GATE.ONLINE else 0,
                "ecg_admission_winner_changes":
                    1 if policy == GATE.ONLINE else 0,
                "ecg_admission_final_winner_arm":
                    int(graph_index >= 2) if policy == GATE.ONLINE else 0,
            })
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def test_online_admission_gate_enforces_regret(tmp_path):
    passing = tmp_path / "passing.csv"
    write_rows(passing, 1.01)
    assert GATE.evaluate(passing)["decision"] == "PROMOTE"

    failing = tmp_path / "failing.csv"
    write_rows(failing, 1.03)
    assert GATE.evaluate(failing)["decision"] == "STOP"
