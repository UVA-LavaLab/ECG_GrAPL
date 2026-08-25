import csv
import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
PATH = (
    ROOT /
    "scripts/experiments/ecg/analysis/online_admission_color_gate.py")
SPEC = importlib.util.spec_from_file_location(
    "online_admission_color_gate_test", PATH)
GATE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


def write_rows(
        path: Path, *, wrong_winner: bool = False,
        excessive_regret: bool = False) -> None:
    rows = []
    for graph_index, graph in enumerate(GATE.GRAPHS):
        future_wins = graph_index >= 2
        for policy in GATE.STATIC_ARMS:
            value = (
                80000 if
                (policy == GATE.FUTURE) == future_wins else 100000)
            rows.append({
                "final_graph": graph,
                "policy_label": policy,
                "sweep_offset": "static",
                "status": "ok",
                "simulator": "cache_sim",
                "benchmark": "pr",
                "flowthrough": "all",
                "l3_misses": value,
                "total_offchip_traffic": value,
            })
        for offset in GATE.OFFSETS:
            winner = int(future_wins)
            if wrong_winner and graph_index == 0 and offset == 0:
                winner = 1
            value = 80000
            if excessive_regret and graph_index == 0 and offset == 0:
                value = 81601
            rows.append({
                "final_graph": graph,
                "policy_label": GATE.ONLINE,
                "sweep_offset": offset,
                "status": "ok",
                "simulator": "cache_sim",
                "benchmark": "pr",
                "flowthrough": "all",
                "l3_misses": value,
                "total_offchip_traffic": value,
                "ecg_admission_set_offset": offset,
                "ecg_admission_leader_accesses_grasp": 64,
                "ecg_admission_leader_accesses_future": 64,
                "ecg_admission_leader_misses_grasp":
                    40 if future_wins else 20,
                "ecg_admission_leader_misses_future":
                    20 if future_wins else 40,
                "ecg_admission_completed_windows": 1,
                "ecg_admission_final_winner_arm": winner,
                "ecg_admission_winner_changes": int(winner == 1),
            })
    fields = sorted({field for row in rows for field in row})
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def test_color_gate_requires_stable_winner_and_bounded_regret(tmp_path):
    passing = tmp_path / "passing.csv"
    write_rows(passing)
    assert GATE.evaluate(passing)["decision"] == "PROMOTE"

    wrong = tmp_path / "wrong.csv"
    write_rows(wrong, wrong_winner=True)
    assert GATE.evaluate(wrong)["decision"] == "STOP"

    regret = tmp_path / "regret.csv"
    write_rows(regret, excessive_regret=True)
    assert GATE.evaluate(regret)["decision"] == "STOP"


def test_color_gate_recomputes_sample_winner(tmp_path):
    path = tmp_path / "corrupt.csv"
    write_rows(path)
    rows = list(csv.DictReader(path.open()))
    for row in rows:
        if (
                row["policy_label"] == GATE.ONLINE and
                row["final_graph"] == GATE.GRAPHS[0] and
                row["sweep_offset"] == "0"):
            row["ecg_admission_final_winner_arm"] = "1"
            row["ecg_admission_winner_changes"] = "1"
            break
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    result = GATE.evaluate(path)
    assert result["decision"] == "STOP"
    assert not result["per_graph"][GATE.GRAPHS[0]]["offsets"]["0"][
        "sample_winner_receipt"]
