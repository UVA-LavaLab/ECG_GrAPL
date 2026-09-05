import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_ecg_ref32_commit_queue_cpp(tmp_path: Path):
    source = ROOT / "bench/src_sim/test_ecg_ref32_commit.cc"
    binary = tmp_path / "test_ecg_ref32_commit"
    compile_result = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "bench/include"),
            str(source),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert compile_result.returncode == 0, (
        compile_result.stdout + compile_result.stderr)

    run_result = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert run_result.returncode == 0, (
        run_result.stdout + run_result.stderr)
    assert "[SUMMARY] failures=0" in run_result.stdout
