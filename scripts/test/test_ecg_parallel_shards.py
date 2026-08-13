import importlib.util
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_local_parallel_shards_are_isolated(tmp_path):
    shards = tmp_path / "shards.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_smoke",
            "--run-tag", "local_parallel_test",
            "--out", str(shards),
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    assert len(shards.read_text().splitlines()) == 7

    run_root = tmp_path / "runs"
    launched = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/run_local_shards.py",
            "--shards", str(shards),
            "--run-root", str(run_root),
            "--jobs", "4",
            "--cache-sim-jobs", "4",
            "--dry-run",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert launched.returncode == 0, launched.stdout + launched.stderr
    assert launched.stdout.count("[dry-run]") == 7
    assert launched.stdout.count("--lock-path") == 7
    assert launched.stdout.count("--graph-dir") == 7
    assert launched.stdout.count("--no-build") == 7
    assert "caps={'cache-sim': 4, 'gem5': 1, 'sniper': 1}" in launched.stdout


def test_parallel_shard_reader_rejects_duplicates(tmp_path):
    module = load_module(
        "run_local_shards_test",
        ROOT / "scripts/experiments/ecg/flows/run_local_shards.py")
    shard_file = tmp_path / "duplicate.tsv"
    row = "ecg_smoke\t01_ecg_cache_sim_smoke\tsynthetic_g12\tpr\tLRU\ttag\n"
    shard_file.write_text(row + row)
    suites = {"01_ecg_cache_sim_smoke": "cache-sim"}
    try:
        module.read_shards(shard_file, suites)
    except SystemExit as error:
        assert "duplicate shard row" in str(error)
    else:
        raise AssertionError("duplicate shards were accepted")


def test_serial_final_shards_stop_after_first_failure(
        tmp_path, monkeypatch):
    module = load_module(
        "run_local_shards_fail_fast",
        ROOT / "scripts/experiments/ecg/flows/run_local_shards.py")
    manifest = tmp_path / "manifest.json"
    manifest.write_text(
        '{"stages":[{"name":"81_final","kind":"roi_matrix",'
        '"suite":"sniper"}]}')
    shards = tmp_path / "shards.tsv"
    shards.write_text(
        "k2_final_campaign\t81_final\tg1\tpr\t__whole__\ttag\n"
        "k2_final_campaign\t81_final\tg2\tpr\t__whole__\ttag\n")
    calls = []

    def fail_first(shard, run_root, manifest_path, args, semaphore):
        calls.append(shard.key)
        return shard, 1, run_root / shard.run_tag / shard.key

    monkeypatch.setattr(module, "run_shard", fail_first)
    assert module.main([
        "--manifest", str(manifest),
        "--shards", str(shards),
        "--run-root", str(tmp_path / "runs"),
        "--jobs", "1",
    ]) == 1
    assert len(calls) == 1


def test_threaded_stop_on_error_is_rejected(tmp_path):
    module = load_module(
        "run_local_shards_threaded_fail_fast",
        ROOT / "scripts/experiments/ecg/flows/run_local_shards.py")
    try:
        module.main([
            "--shards", str(tmp_path / "unused.tsv"),
            "--jobs", "2",
            "--stop-on-error",
        ])
    except SystemExit as error:
        assert "--stop-on-error requires --jobs 1" in str(error)
    else:
        raise AssertionError("threaded fail-fast request was accepted")


def test_final_campaign_expands_to_whole_cell_shards(tmp_path):
    shards = tmp_path / "final_whole_cells.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "k2_final_campaign",
            "--run-tag", "final-whole",
            "--out", str(shards),
            "--whole-cell",
            "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 76
    assert {row[4] for row in rows} == {"__whole__"}

    run_root = tmp_path / "runs"
    rejected = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/run_local_shards.py",
            "--shards", str(shards),
            "--run-root", str(run_root),
            "--jobs", "8",
            "--cache-sim-jobs", "4",
            "--gem5-jobs", "1",
            "--sniper-jobs", "2",
            "--dry-run",
            "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert rejected.returncode != 0
    assert "Sniper shards must run serially" in (
        rejected.stdout + rejected.stderr)

    launched = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/run_local_shards.py",
            "--shards", str(shards),
            "--run-root", str(run_root),
            "--jobs", "8",
            "--cache-sim-jobs", "4",
            "--gem5-jobs", "1",
            "--sniper-jobs", "1",
            "--dry-run",
            "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert launched.returncode == 0, launched.stdout + launched.stderr
    assert launched.stdout.count("[dry-run]") == 76
    assert "--policy __whole__" not in launched.stdout

    sbatch = (
        ROOT /
        "scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch"
    ).read_text()
    assert 'if [[ "$policy" != "__whole__" ]]' in sbatch
    assert '"${policy_args[@]}"' in sbatch


def test_full_3sim_smoke_expands_to_120_shards(tmp_path):
    shards = tmp_path / "three_sim.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_3sim_allalg_smoke",
            "--run-tag", "three_sim_smoke",
            "--out", str(shards),
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 120
    assert {row[3] for row in rows} == {"pr", "bfs", "sssp", "bc", "cc"}
    assert len({row[4] for row in rows}) == 8


def test_full_3sim_realgraph_expands_to_360_shards(tmp_path):
    shards = tmp_path / "three_sim_realgraph.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_3sim_realgraph_allalg",
            "--run-tag", "three_sim_realgraph",
            "--out", str(shards),
            "--allow-missing-graphs",
            "--allow-blocked",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 360
    assert {row[2] for row in rows} == {
        "web-Google", "soc-pokec", "cit-Patents"}
    assert {row[3] for row in rows} == {"pr", "bfs", "sssp", "bc", "cc"}
    assert len({row[4] for row in rows}) == 8


def test_capped_3sim_realgraph_expands_to_360_shards(tmp_path):
    shards = tmp_path / "three_sim_realgraph_1b.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_3sim_realgraph_allalg_1b",
            "--run-tag", "three_sim_realgraph_1b",
            "--out", str(shards),
            "--allow-missing-graphs",
            "--allow-blocked",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 360
    assert {row[1].split("_", 1)[0] for row in rows} == {"22", "25", "26"}


def test_sampled_3sim_realgraph_expands_to_360_shards(tmp_path):
    shards = tmp_path / "three_sim_sampled.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_3sim_sampled_allalg",
            "--run-tag", "three_sim_sampled",
            "--out", str(shards),
            "--allow-missing-graphs",
            "--allow-blocked",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 360
    assert {row[1].split("_", 1)[0] for row in rows} == {"27", "28", "29"}
    assert {row[2] for row in rows} == {
        "web-Google-n16", "soc-pokec-n16", "cit-Patents-n18-sym"}


def test_sniper_600m_expands_to_120_shards(tmp_path):
    shards = tmp_path / "sniper_600m.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "ecg_sniper_realgraph_600m",
            "--run-tag", "sniper_600m",
            "--out", str(shards),
            "--allow-missing-graphs",
        ],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 120
    assert {row[1] for row in rows} == {"32_sniper_realgraph_600m"}
    assert {row[2] for row in rows} == {
        "web-Google", "soc-pokec", "cit-Patents"}
    assert {row[3] for row in rows} == {"pr", "bfs", "sssp", "bc", "cc"}
    assert len({row[4] for row in rows}) == 8


def test_slurm_shards_use_per_run_lock():
    source = (
        ROOT / "scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch"
    ).read_text()
    assert '--lock-path "$run_dir/.experiment_run.lock"' in source
    assert '--graph-dir "${GRAPHBREW_GRAPH_DIR:-results/graphs}"' in source
    local = (
        ROOT / "scripts/experiments/ecg/flows/run_local_shards.py"
    ).read_text()
    assert '".local_shard.lock"' in local
    assert "fcntl.LOCK_NB" in local
