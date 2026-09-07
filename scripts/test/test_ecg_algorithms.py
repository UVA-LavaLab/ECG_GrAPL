"""Current shared algorithms agree with independent, nontrivial graph answers."""

from pathlib import Path
import importlib.util
import copy
import json
import os
import shutil
import struct
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_shared_current_algorithms(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    binary = tmp_path / "test_ecg_algorithms"
    built = subprocess.run([
        compiler, "-std=c++17", "-O2", "-ffp-contract=off", "-fopenmp", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-sign-compare",
        "-I", str(ROOT / "bench/include"),
        str(ROOT / "bench/src_sim/test_ecg_algorithms.cc"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=90, check=False)
    assert built.returncode == 0, built.stdout + built.stderr
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "[SUMMARY] failures=0" in ran.stdout


def test_current_algorithm_cli_and_independent_receipts(tmp_path):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    source = ROOT / "scripts/experiments/ecg/flows/prepare_record_equivalence_graphs.py"
    spec = importlib.util.spec_from_file_location("algorithm_corpus", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    config = json.loads((ROOT / "scripts/experiments/ecg/configs/algorithm_equivalence.json").read_text())
    assert module.algorithm_outputs()["diamond.sg"][1]["maximum_out_id"] == 7
    for filename, (data, _) in module.algorithm_outputs().items():
        (tmp_path / filename).write_bytes(data)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CACHE_", "ECG_", "GEM5_", "SNIPER_", "GRASP_", "POPT_"))}
    env.update(OMP_NUM_THREADS="1", GRAPHBREW_SIDEBAND_LOG="0")
    for algorithm, expected in config["references"].items():
        for width in (4, 8):
            for mode in ("csr", "replacement-prefetch"):
                output = tmp_path / f"{algorithm}-{width}-{mode}.json"
                ran = subprocess.run([
                    str(binary), "--algorithm", algorithm,
                    "--graph", str(tmp_path / expected["graph"]), "--delta", "2",
                    "--mode", mode, "--record-bytes", str(width), "--values", "--evidence",
                    "--l1-bytes", "128", "--l1-ways", "2",
                    "--l2-bytes", "256", "--l2-ways", "2",
                    "--llc-bytes", "512", "--llc-ways", "2",
                    "--output", str(output),
                ], env=env, capture_output=True, text=True, timeout=20, check=False)
                assert ran.returncode == 0, (algorithm, mode, ran.stdout, ran.stderr)
                payload = json.loads(output.read_text())
                workload = payload["workload"]
                assert payload["timing_valid_for_speedup"] is False
                assert payload["mode"] == mode
                assert workload["algorithm"] == algorithm
                assert workload["variant"] == config["algorithms"][algorithm]["variant"]
                for key, value in expected.items():
                    if key != "graph":
                        assert workload[key] == value, (algorithm, key)
                assert workload["actual_records"] + workload["skipped_positions"] == workload["structural_positions"]
                if mode != "csr":
                    assert workload["record_bytes"] == width
                    assert workload["construction_read_bytes"] > 0
                    assert workload["construction_write_bytes"] > 0


def test_checked_algorithm_graph_reader(tmp_path):
    binary = tmp_path / "reader"
    built = subprocess.run([
        "g++", "-std=c++17", "-O2", "-fopenmp",
        "-I", str(ROOT / "bench/include/external/gapbs"),
        str(ROOT / "bench/src_sim/test_ecg_algorithm_io.cc"), "-o", str(binary),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert built.returncode == 0, built.stdout + built.stderr
    header = struct.pack("<?qq", False, 2, 3)
    offsets = struct.pack("<4q", 0, 1, 2, 2)
    ids = struct.pack("<3i", 0, 1, 2)
    plain = header + offsets + struct.pack("<2i", 1, 0) + ids
    weighted = header + offsets + struct.pack("<4i", 1, -3, 0, -3) + ids
    cases = [
        ("valid.sg", plain, "plain", 0, ""),
        ("copy.sg", plain, "copy-failure", 0, ""),
        ("valid.wsg", weighted, "weighted", 0, "1:-3"),
        ("truncate.sg", plain[:-1], "plain", 3, "size"),
        ("trailing.sg", plain + b"x", "plain", 3, "size"),
        ("bad-offset.sg", header + struct.pack("<4q", 0, 9, 2, 2) + plain[len(header) + len(offsets):],
         "plain", 3, "offset"),
        ("bad-id.sg", header + offsets + struct.pack("<2i", 3, 0) + ids, "plain", 3, "neighbor"),
        ("bad-count.sg", struct.pack("<?qq", False, -2, 3) + plain[len(header):],
         "plain", 3, "count"),
        ("no-generated-weights.sg", plain, "weighted", 3, "weight"),
        ("no-dropped-weights.wsg", weighted, "plain", 3, "weight"),
    ]
    for filename, payload, kind, status, message in cases:
        path = tmp_path / filename
        path.write_bytes(payload)
        ran = subprocess.run(
            [str(binary), str(path), kind], capture_output=True, text=True, timeout=10, check=False)
        assert ran.returncode == status, (filename, ran.stdout, ran.stderr)
        assert message in ran.stdout + ran.stderr


def test_algorithm_resource_plans_cover_auxiliary_work():
    from scripts.experiments.ecg.record_resources import GraphInfo, RecordResourceError, plan_algorithm_resources
    graph = GraphInfo(False, 512, 3968, 495, 37913, "a" * 64, True, 0, 31)
    options = dict(records=True, requested_bytes=4, minimum_mantissa_bits=0,
                   traversals=2, sources=1, workspace_limit=1 << 20,
                   carrier_limit=1 << 20, auxiliary_limit=1 << 20, rss_mib=1024)
    for algorithm, array_bytes in (("sssp", 25 * 512), ("bc", 40 * 512 + 20),
                                   ("tc", 24 * 512 + 4 * 1984 + 8)):
        plan = plan_algorithm_resources(graph, algorithm=algorithm, **options)
        assert plan["array_bytes"] == array_bytes
        assert plan["construction_auxiliary_bytes_upper"] > 0
        assert plan["planned_host_bytes"] < 1024 << 20
    with pytest.raises(RecordResourceError, match="explicit limits"):
        plan_algorithm_resources(graph, algorithm="tc", **{**options, "workspace_limit": 4096})
    with pytest.raises(RecordResourceError, match="RSS budget"):
        plan_algorithm_resources(graph, algorithm="bfs", **{**options, "rss_mib": 64})


def test_algorithm_profiles_and_forged_work_are_rejected(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_algorithm_equivalence_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 12
    assert all("--current-algorithms" in job.command and "--ecg-equivalence" in job.command for job in jobs)
    assert {job.command[job.command.index("--benchmark") + 1] for job in jobs} == algorithm_matrix.ALGORITHMS
    assert all(Path(job.metadata["input_paths"]["cache_sim_benchmark_binary"]).name == "algorithms" for job in jobs)
    assert all(job.metadata["process_tree_rss_mib"] == 1024 for job in jobs)
    assert not experiment_run.can_write_current_equivalence_receipt(args, jobs)

    rows = [
        {"status": "ok", "simulator": "cache_sim", "benchmark": "bfs", "l3_size": "1kB",
         "graph_sha256": "a" * 64, "policy_label": policy, "total_offchip_traffic": 10,
         "algorithm_result_digest": "a" * 16, "algorithm_actual_records": count}
        for policy, count in (("LRU", 7), ("ECG", 8))
    ]
    algorithm_matrix.certify_rows(rows)
    assert all(row["status"] == "error" for row in rows)

    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph_path = ROOT / "results/graphs/ecg-algorithm-equivalence/weighted-diamond.wsg"
    if not graph_path.is_file():
        pytest.skip("prepared current algorithm corpus is unavailable")
    output = tmp_path / "work.json"
    ran = subprocess.run([
        str(binary), "--algorithm", "sssp", "--graph", str(graph_path),
        "--mode", "replacement-prefetch", "--record-bytes", "4", "--delta", "8",
        "--values", "--evidence", "--llc-bytes", "512", "--llc-ways", "2", "--output", str(output),
    ], env={**os.environ, "OMP_NUM_THREADS": "1"}, capture_output=True, text=True, timeout=20, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    payload = json.loads(output.read_text())
    options = algorithm_matrix.parse_options(f"--graph {graph_path} --delta 8")
    graph = algorithm_matrix.graph_info(graph_path, allow_weighted=True, traversal="out")
    settings = dict(algorithm="sssp", mode="replacement-prefetch", policy="LRU",
                    graph=graph, graph_path=graph_path, options=options, requested_bytes=4,
                    minimum_mantissa_bits=0, evidence=True, llc_sets=4)
    algorithm_matrix.validate_payload(payload, ran.stdout + ran.stderr, **settings)
    for key, value in (("skipped_positions", 0), ("weighted", 0), ("record_bytes", 8),
                       ("variant", "legacy-sssp"), ("prediction_semantics", "dense-actual-designated-read")):
        forged = copy.deepcopy(payload)
        forged["workload"][key] = value
        with pytest.raises(RecordReceiptError):
            algorithm_matrix.validate_payload(forged, ran.stdout + ran.stderr, **settings)


def test_current_algorithm_rows_close_existing_runner_policy_roster(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    binary = ROOT / "bench/bin_sim/algorithms"
    graph = ROOT / "results/graphs/ecg-algorithm-equivalence/weighted-diamond.wsg"
    if not binary.is_file() or not graph.is_file():
        pytest.skip("built current algorithm and prepared graph are required")
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "spmv",
        "--options", f"--graph {graph} --repeat 2",
        "--policies", "LRU", "ECG:transport", "ECG", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "512B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    status, detail = experiment_run.csv_status(
        output / "roi_matrix.csv", ["LRU", "ECG:transport", "ECG"])
    assert status == "ok", detail
    for row in json.loads((output / "roi_matrix.json").read_text()):
        assert row["l3_accesses"] == row["l3_hits"] + row["l3_misses"] > 0
