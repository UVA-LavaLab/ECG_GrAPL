from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_fixed_order_actual_record_evidence(tmp_path):
    binary = tmp_path / "record_evidence"
    built = subprocess.run([
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "bench/include"),
        str(ROOT / "bench/src_sim/test_ecg_record_evidence.cc"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "[SUMMARY] failures=0" in ran.stdout
