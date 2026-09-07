import importlib.util
import os
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
SBATCH = (
    ROOT / "scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch"
)
SHARD_GENERATOR = (
    ROOT / "scripts/experiments/ecg/slurm/make_slurm_shards.py"
)
PREFLIGHT = (
    ROOT / "scripts/experiments/ecg/flows/lab_runtime_preflight.py"
)
CURRENT_PROFILES = (
    "ecg_current_equivalence",
    "ecg_local_release_cache",
    "ecg_twitter_reproduction",
    "ecg_large_cache",
    "ecg_detailed_final",
)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def fake_project(tmp_path: Path) -> tuple[Path, Path]:
    root = tmp_path / "project"
    fake_bin = tmp_path / "bin"
    capture = tmp_path / "argv.txt"
    (root / "scripts/experiments/ecg/flows").mkdir(parents=True)
    fake_bin.mkdir()
    python = fake_bin / "python3"
    python.write_text(
        '#!/bin/sh\nprintf "%s\\n" "$@" > "$GRAPHBREW_TEST_CAPTURE"\n'
        'printf "%s\\n" "$*" >> "$GRAPHBREW_TEST_CAPTURE.commands"\n'
        'if [ "${GRAPHBREW_TEST_FAIL_PREFLIGHT:-0}" = 1 ]; then exit 9; fi\n'
    )
    python.chmod(0o755)
    return root, capture


def run_wrapper(
        tmp_path: Path, extra_env: dict[str, str]
) -> tuple[subprocess.CompletedProcess[str], list[str]]:
    source = SBATCH.read_text()
    assert "${!" not in source
    assert "eval " not in source
    root, capture = fake_project(tmp_path)
    env = {
        "PATH": f"{tmp_path / 'bin'}:/usr/bin:/bin",
        "GRAPHBREW_ROOT": str(root),
        "GRAPHBREW_TEST_CAPTURE": str(capture),
        **extra_env,
    }
    result = subprocess.run(
        ["/bin/bash", str(SBATCH)],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    argv = capture.read_text().splitlines() if capture.exists() else []
    return result, argv


def test_slurm_wrapper_has_no_indirect_or_eval_expansion():
    source = SBATCH.read_text()
    assert "${!" not in source
    assert "eval " not in source


@pytest.mark.parametrize("profile", CURRENT_PROFILES)
def test_generator_rejects_current_whole_profiles(tmp_path, profile):
    out = tmp_path / "must-not-exist.tsv"
    result = subprocess.run(
        [
            sys.executable,
            str(SHARD_GENERATOR),
            "--profile",
            profile,
            "--manifest",
            str(tmp_path / "not-needed.json"),
            "--out",
            str(out),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "CURRENT WHOLE-PROFILE mode" in result.stderr
    assert profile in result.stderr
    assert not out.exists()


@pytest.mark.parametrize(
    "profile",
    (
        "ecg_current_equivalence",
        "ecg_local_release_cache",
        "ecg_twitter_reproduction",
        "ecg_large_cache",
    ),
)
def test_current_nonfinal_launch_has_no_selection_filters(tmp_path, profile):
    result, argv = run_wrapper(
        tmp_path,
        {
            "GRAPHBREW_SLURM_MODE": "current-whole-profile",
            "GRAPHBREW_CURRENT_PROFILE": profile,
            "GRAPHBREW_CURRENT_RUN_TAG": "lab_nonfinal",
        },
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert argv[:2] == ["-I", "scripts/experiments/ecg/flows/experiment_run.py"]
    assert argv[argv.index("--profile") + 1] == profile
    assert "--no-build" in argv
    for option in (
        "--only", "--skip", "--graph", "--benchmark", "--policy",
        "--job", "--from-job", "--limit", "--screen-gate",
    ):
        assert option not in argv
    commands = (tmp_path / "argv.txt.commands").read_text()
    assert "lab_runtime_preflight.py" in commands and "--seconds 60" in commands


@pytest.mark.parametrize("run_tag", [".", ".."])
def test_current_mode_rejects_directory_navigation_tags(tmp_path, run_tag):
    completed, argv = run_wrapper(tmp_path, {
        "GRAPHBREW_SLURM_MODE": "current-whole-profile",
        "GRAPHBREW_CURRENT_PROFILE": "ecg_local_release_cache",
        "GRAPHBREW_CURRENT_RUN_TAG": run_tag,
    })
    assert completed.returncode != 0
    assert not argv


def test_current_preflight_failure_stops_before_experiment_execution(tmp_path):
    completed, argv = run_wrapper(tmp_path, {
        "GRAPHBREW_SLURM_MODE": "current-whole-profile",
        "GRAPHBREW_CURRENT_PROFILE": "ecg_local_release_cache",
        "GRAPHBREW_CURRENT_RUN_TAG": "blocked",
        "GRAPHBREW_TEST_FAIL_PREFLIGHT": "1",
    })
    assert completed.returncode == 9
    assert "scripts/test/sniper_rss_watch.py" in argv
    assert "scripts/experiments/ecg/flows/experiment_run.py" not in argv


@pytest.mark.parametrize(
    ("extra_env", "message"),
    [
        ({}, "GRAPHBREW_FINAL_STAGE=1"),
        ({"GRAPHBREW_FINAL_STAGE": "1"}, "GRAPHBREW_EQUIVALENCE_RECEIPT"),
    ],
)
def test_current_final_requires_intent_and_receipt(
        tmp_path, extra_env, message):
    result, argv = run_wrapper(
        tmp_path,
        {
            "GRAPHBREW_SLURM_MODE": "current-whole-profile",
            "GRAPHBREW_CURRENT_PROFILE": "ecg_detailed_final",
            "GRAPHBREW_CURRENT_RUN_TAG": "lab_final",
            **extra_env,
        },
    )
    assert result.returncode != 0
    assert message in result.stderr
    assert not argv


def test_current_final_forwards_only_authorization_controls(tmp_path):
    receipt = tmp_path / "current.complete.json"
    receipt.write_text("{}\n")
    result, argv = run_wrapper(
        tmp_path,
        {
            "GRAPHBREW_SLURM_MODE": "current-whole-profile",
            "GRAPHBREW_CURRENT_PROFILE": "ecg_detailed_final",
            "GRAPHBREW_CURRENT_RUN_TAG": "lab_final",
            "GRAPHBREW_FINAL_STAGE": "1",
            "GRAPHBREW_EQUIVALENCE_RECEIPT": str(receipt),
        },
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "--final-stage" in argv
    assert argv[argv.index("--equivalence-receipt") + 1] == str(receipt)
    for option in ("--only", "--graph", "--benchmark", "--policy"):
        assert option not in argv


def test_current_mode_rejects_historical_screen_receipt(tmp_path):
    result, argv = run_wrapper(
        tmp_path,
        {
            "GRAPHBREW_SLURM_MODE": "current-whole-profile",
            "GRAPHBREW_CURRENT_PROFILE": "ecg_large_cache",
            "GRAPHBREW_CURRENT_RUN_TAG": "lab_cache",
            "GRAPHBREW_SCREEN_GATE": str(tmp_path / "screen.json"),
        },
    )
    assert result.returncode != 0
    assert "historical screen receipt" in result.stderr
    assert not argv


def test_current_mode_rejects_array_launch(tmp_path):
    result, argv = run_wrapper(
        tmp_path,
        {
            "GRAPHBREW_SLURM_MODE": "current-whole-profile",
            "GRAPHBREW_CURRENT_PROFILE": "ecg_large_cache",
            "GRAPHBREW_CURRENT_RUN_TAG": "lab_cache",
            "SLURM_ARRAY_TASK_ID": "0",
        },
    )
    assert result.returncode != 0
    assert "serial single job" in result.stderr
    assert not argv


def test_legacy_shard_mode_preserves_filters_and_screen_gate(tmp_path):
    shards = tmp_path / "legacy.tsv"
    shards.write_text(
        "ecg_smoke\t01_ecg_cache_sim_smoke\tsynthetic_g12\tpr\t"
        "ECG:REUSE_PLAN\tlegacy_tag\n"
    )
    screen = tmp_path / "screen.json"
    result, argv = run_wrapper(
        tmp_path,
        {
            "SHARDS": str(shards),
            "SLURM_ARRAY_TASK_ID": "0",
            "GRAPHBREW_SCREEN_GATE": str(screen),
        },
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert argv[argv.index("--profile") + 1] == "ecg_smoke"
    assert argv[argv.index("--only") + 1] == "01_ecg_cache_sim_smoke"
    assert argv[argv.index("--graph") + 1] == "synthetic_g12"
    assert argv[argv.index("--benchmark") + 1] == "pr"
    assert argv[argv.index("--policy") + 1] == "ECG:REUSE_PLAN"
    assert argv[argv.index("--screen-gate") + 1] == str(screen)


def test_legacy_shard_mode_rejects_current_profile_rows(tmp_path):
    shards = tmp_path / "legacy.tsv"
    shards.write_text(
        "ecg_large_cache\t130_current_large_cache\tgraph\tpr\t"
        "__whole__\tlegacy_tag\n"
    )
    result, argv = run_wrapper(
        tmp_path,
        {
            "SHARDS": str(shards),
            "SLURM_ARRAY_TASK_ID": "0",
        },
    )
    assert result.returncode != 0
    assert "current-whole-profile" in result.stderr
    assert not argv


@pytest.mark.parametrize(
    ("task_id", "row", "message"),
    [
        ("not-a-number", "a\tb\tc\td\te\tf\n", "non-negative integer"),
        ("0", "a\tb\tc\td\te\n", "expected 6 tab-separated fields"),
        ("0", "a\tb\tbad/graph\td\te\tf\n", "invalid graph"),
        ("0", "a\tb\tc\td\te\tf\textra\n", "expected 6 tab-separated fields"),
    ],
)
def test_legacy_shard_mode_rejects_malformed_input(
        tmp_path, task_id, row, message):
    shards = tmp_path / "malformed.tsv"
    shards.write_text(row)
    result, argv = run_wrapper(
        tmp_path,
        {
            "SHARDS": str(shards),
            "SLURM_ARRAY_TASK_ID": task_id,
        },
    )
    assert result.returncode != 0
    assert message in result.stderr
    assert not argv


def test_preflight_reports_missing_requirements_and_rebuild_warning(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            str(PREFLIGHT),
            "--project-root",
            str(tmp_path / "empty-project"),
            "--profile",
            "ecg_detailed_final",
            "--equivalence-receipt",
            str(tmp_path / "missing.complete.json"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    output = result.stdout + result.stderr
    assert "host architecture" in output
    assert "node-local rebuild" in output
    assert "full small ecg_current_equivalence run" in output
    assert "does not establish -march=native portability" in output


def test_preflight_default_root_is_repository():
    module = load_module("lab_runtime_preflight_root_test", PREFLIGHT)
    assert module.DEFAULT_ROOT == ROOT


@pytest.mark.parametrize(
    "profile", ("ecg_local_release_cache", "ecg_large_cache", "ecg_twitter_reproduction")
)
def test_cache_only_preflight_can_pass_with_fixture_tools(
        tmp_path, monkeypatch, profile):
    module = load_module("lab_runtime_preflight_test", PREFLIGHT)
    root = tmp_path / "project"
    binary = root / "bench/bin_sim/pr"
    binary.parent.mkdir(parents=True)
    binary.write_text("fixture\n")
    binary.chmod(0o755)

    monkeypatch.setattr(
        module.shutil, "which", lambda name: f"/usr/bin/{name}"
    )
    monkeypatch.setattr(module.platform, "machine", lambda: "x86_64")
    monkeypatch.setattr(
        module.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            returncode=0, stdout="", stderr=""
        ),
    )
    report = module.collect_preflight(
        root, [profile], equivalence_receipt=None,
        probe_fuse=False,
    )
    assert report["ok"] is True
    assert all(check["status"] == "pass" for check in report["checks"])


def test_setarch_probe_failure_is_reported(monkeypatch):
    module = load_module("lab_runtime_preflight_setarch_test", PREFLIGHT)
    monkeypatch.setattr(
        module.shutil, "which", lambda name: "/usr/bin/setarch"
    )
    monkeypatch.setattr(
        module.subprocess,
        "run",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            subprocess.TimeoutExpired(args[0], 5)
        ),
    )
    check = module.check_setarch("x86_64")
    assert check["status"] == "fail"
    assert "could not verify" in check["detail"]


@pytest.mark.parametrize("helper_removes_directory", [False, True])
def test_fuse_probe_is_explicit_and_uses_helper(
        tmp_path, monkeypatch, helper_removes_directory):
    module = load_module("lab_runtime_preflight_fuse_test", PREFLIGHT)
    root = tmp_path / "project"
    observed = []

    @contextmanager
    def fake_fuse_context(project_root, mountpoint):
        observed.append((project_root, mountpoint))
        mountpoint.mkdir(parents=True)
        probe = mountpoint / "probe.txt"
        probe.write_bytes(b"graphbrew-fuse-preflight\n")
        try:
            yield
        finally:
            probe.unlink()
            if helper_removes_directory:
                mountpoint.rmdir()

    monkeypatch.setattr(module, "_fuse_probe_context", fake_fuse_context)
    monkeypatch.setattr(module.os, "getpid", lambda: 4242)
    check = module.probe_fuse_mount(root)
    expected = (
        root / "results/ecg_experiments/preflight/fuse_probe_4242"
    ).resolve()
    assert check["status"] == "pass"
    assert observed == [(root, expected)]
    assert not expected.exists()


def test_fuse_probe_reports_incomplete_cleanup(tmp_path, monkeypatch):
    module = load_module("lab_runtime_preflight_cleanup_test", PREFLIGHT)

    @contextmanager
    def broken_context(_root, mountpoint):
        mountpoint.mkdir(parents=True)
        (mountpoint / "leftover.txt").write_text("retain failure evidence")
        raise RuntimeError("probe setup failed")
        yield

    monkeypatch.setattr(module, "_fuse_probe_context", broken_context)
    check = module.probe_fuse_mount(tmp_path)
    assert check["status"] == "fail"
    assert "probe setup failed" in check["detail"] and "cleanup" in check["detail"]
