"""Current shared algorithms agree with independent, nontrivial graph answers."""

from pathlib import Path
import importlib.util
import copy
import csv
import json
import os
import shutil
import struct
import subprocess
from types import SimpleNamespace

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
                phases = payload["traffic_phases"]
                for key in ("total_accesses", "memory_accesses", "prefetch_fills",
                            "llc_writebacks", "total_offchip_traffic"):
                    assert phases["setup"][key] + phases["kernel"][key] == payload["metrics"][key]
                assert phases["boundary"] == "first-binding-complete"
                assert phases["cache_state_preserved"] is True
                assert payload["timing_valid_for_speedup"] is False
                assert payload["mode"] == mode
                assert payload["setup_cache_policy"] == "LRU"
                assert payload["record_base_policy"] == "LRU"
                assert workload["record_base_policy"] == "LRU"
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


def test_current_grasp_record_base_preserves_program_work_and_provenance(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    data, _ = algorithm_outputs()["weighted-diamond.wsg"]
    graph = tmp_path / "weighted-diamond.wsg"
    graph.write_bytes(data)
    baselines = {}
    setups = {}
    for width in (4, 8):
        for mode in ("transport", "replacement"):
            for base in ("LRU", "GRASP_PAPER"):
                output = tmp_path / f"{width}-{mode}-{base}.json"
                ran = subprocess.run([
                    str(binary), "--algorithm", "spmv", "--graph", str(graph),
                    "--repeat", "2", "--delta", "8", "--mode", mode,
                    "--record-bytes", str(width),
                    "--record-base-policy", base, "--values", "--evidence",
                    "--l1-bytes", "128", "--l1-ways", "2",
                    "--l2-bytes", "256", "--l2-ways", "2",
                    "--llc-bytes", "512", "--llc-ways", "2",
                    "--output", str(output),
                ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
                    capture_output=True, text=True, timeout=20, check=False)
                assert ran.returncode == 0, ran.stdout + ran.stderr
                payload = json.loads(output.read_text())
                work = payload["workload"]
                options = algorithm_matrix.parse_options(
                    f"--graph {graph} --repeat 2 --record-base-policy {base}")
                graph_info = algorithm_matrix.graph_info(
                    graph, allow_weighted=True, traversal="out")
                algorithm_matrix.validate_payload(
                    payload, ran.stdout + ran.stderr, algorithm="spmv", mode=mode,
                    policy="LRU", graph=graph_info, graph_path=graph, options=options,
                    requested_bytes=width, minimum_mantissa_bits=0,
                    evidence=True, llc_sets=4)
                assert payload["record_base_policy"] == base
                assert payload["setup_cache_policy"] == base
                assert work["record_base_policy"] == base
                setups[(width, base, mode)] = payload["traffic_phases"]["setup"]
                signature = tuple(work[key] for key in (
                    "result_digest", "work_trace_digest", "position_trace_digest",
                    "record_trace_digest", "actual_records", "passes", "values_f32"))
                baselines.setdefault((width, mode), signature)
                assert signature == baselines[(width, mode)]
    for width in (4, 8):
        for base in ("LRU", "GRASP_PAPER"):
            assert setups[(width, base, "transport")] == setups[
                (width, base, "replacement")]


def test_record_base_cli_rejects_nonrecord_and_popt_combinations(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    data, _ = algorithm_outputs()["diamond.sg"]
    graph = tmp_path / "diamond.sg"
    graph.write_bytes(data)
    for extra in (
            ["--record-base-policy", "GRASP_PAPER"],
            ["--policy", "POPT_UNCHARGED", "--record-base-policy", "GRASP_PAPER"]):
        ran = subprocess.run([
            str(binary), "--algorithm", "bfs", "--graph", str(graph), *extra,
        ], env={**os.environ, "OMP_NUM_THREADS": "1"},
            capture_output=True, text=True, timeout=10, check=False)
        assert ran.returncode == 2
        assert "record-base-policy-requires-record-mode" in ran.stderr


def test_current_popt_full_capacity_preserves_work_and_declares_coverage(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    config = algorithm_matrix.contract()
    for filename, (data, _) in algorithm_outputs().items():
        (tmp_path / filename).write_bytes(data)
    for algorithm, expected in config["references"].items():
        graph_path = tmp_path / expected["graph"]
        outputs = []
        for policy in ("LRU", "POPT_UNCHARGED"):
            output = tmp_path / f"{algorithm}-{policy}.json"
            ran = subprocess.run([
                str(binary), "--algorithm", algorithm, "--graph", str(graph_path),
                "--delta", "2", "--policy", policy, "--values", "--evidence",
                "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256",
                "--l2-ways", "2", "--llc-bytes", "512", "--llc-ways", "2",
                "--output", str(output),
            ], env={**os.environ, "OMP_NUM_THREADS": "1", "POPT_MATRIX_STREAM_SIM": "1"},
                capture_output=True, text=True, timeout=30, check=False)
            assert ran.returncode == 0, ran.stdout + ran.stderr
            payload = json.loads(output.read_text())
            options = algorithm_matrix.parse_options(f"--graph {graph_path} --delta 2")
            graph = algorithm_matrix.graph_info(graph_path, allow_weighted=True, traversal="out")
            work = algorithm_matrix.validate_payload(
                payload, ran.stdout + ran.stderr, algorithm=algorithm, mode="csr", policy=policy,
                graph=graph, graph_path=graph_path, options=options, requested_bytes=0,
                minimum_mantissa_bits=0, evidence=True, llc_sets=4)
            algorithm_matrix.validate_traffic_phases(payload)
            assert payload["metrics"]["L3"]["size_bytes"] == 512
            assert payload["metrics"]["L3"]["ways"] == 2
            if policy == "POPT_UNCHARGED":
                assert payload["metrics"]["popt_matrix_stream_lines_simulated"] == 0
                assert payload["popt"]["workspace_peak_bytes"] >= payload["popt"]["matrix_bytes"]
                assert payload["setup_cache_policy"] == "LRU"
                assert payload["record_base_policy"] == "LRU"
            outputs.append(work)
        for key in ("result_digest", "work_trace_digest", "position_trace_digest",
                    "actual_records", "passes", "bindings", "values_u32", "values_u64", "values_f32"):
            assert outputs[0][key] == outputs[1][key], (algorithm, key)


def test_direction_optimized_cli_uses_real_incoming_csr(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import serialized_graph
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    data, _ = serialized_graph(5, [(0, 1), (0, 2), (1, 3), (2, 3)], True)
    graph = tmp_path / "directed.sg"
    graph.write_bytes(data)
    for mode in ("csr", "transport", "replacement", "replacement-prefetch"):
        output = tmp_path / f"{mode}.json"
        ran = subprocess.run([
            str(binary), "--algorithm", "bfs", "--bfs-direction", "do",
            "--graph", str(graph), "--source", "0", "--mode", mode,
            "--record-bytes", "4", "--values", "--evidence", "--output", str(output),
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
            capture_output=True, text=True, timeout=20, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        work = json.loads(output.read_text())["workload"]
        assert work["values_u32"] == [0, 1, 1, 2, 4294967295]
        assert work["bfs_bu_levels"] > 0
        assert work["bfs_bu_transport"] == "ordinary-bitmap"
        assert work["passes"] == work["bfs_td_levels"]
        assert work["levels"] == work["bfs_td_levels"] + work["bfs_bu_levels"]


def test_window_observer_does_not_change_grasp_or_bfs(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    payloads = []
    for mode in ("off", "control", "window"):
        # Different output-path allocations can change small CSR cache coloring.
        output = tmp_path / "same-output.json"
        command = [
            str(binary), "--algorithm", "bfs", "--graph", str(graph),
            "--policy", "GRASP_PAPER", "--window-observer", mode, "--evidence", "--values",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output),
        ]
        ran = subprocess.run(
            ["setarch", os.uname().machine, "-R", *command],
            env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
            capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        if mode == "off":
            assert payload["diagnostic_only"] is False and payload["window_observer"] is None
            payloads.append(payload)
            continue
        assert payload["diagnostic_only"] is True
        assert payload["measurement_scope"] == "observation-only-unchanged-grasp"
        observer = payload["window_observer"]
        assert observer["mode"] == mode and observer["active_policy_changed"] is False
        assert observer["diagnostic_costs_in_cache_counters"] is False
        assert observer["passes"] == payload["workload"]["passes"]
        assert observer["designated_reads"] == payload["workload"]["actual_records"]
        from scripts.experiments.ecg.algorithm_matrix import parse_options, validate_window_observer
        options = parse_options(f"--graph {graph} --window-observer {mode}")
        validate_window_observer(payload, options)
        payloads.append(payload)
    off, a, b = payloads
    assert off["workload"] == a["workload"] == b["workload"]
    assert off["metrics"] == a["metrics"] == b["metrics"]
    assert off["traffic_phases"] == a["traffic_phases"] == b["traffic_phases"]
    for key in ("demand_digest", "victim_digest", "memory_requests", "llc_fills", "llc_evictions"):
        assert a["window_observer"][key] == b["window_observer"][key], key
    assert b["window_observer"]["constructed_records"] == b["workload"]["source_edges"]
    assert b["window_observer"]["queue_pending"] == 0
    from scripts.experiments.ecg.algorithm_matrix import verify_window_noninterference
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    verify_window_noninterference(a, b)
    forged = copy.deepcopy(b)
    forged["window_observer"]["victim_digest"] = "0000000000000000"
    with pytest.raises(RecordReceiptError, match="noninterference"):
        verify_window_noninterference(a, forged)
    forged = copy.deepcopy(b)
    forged["window_observer"]["queue_stale"] += 1
    with pytest.raises(RecordReceiptError, match="queue"):
        validate_window_observer(forged, options)
    from scripts.experiments.ecg.flows.experiment_run import validate_window_observer_pairs
    jobs = []
    for mode, payload in (("control", a), ("window", b)):
        data_path, csv_path = tmp_path / f"{mode}-saved.json", tmp_path / f"{mode}-saved.csv"
        data_path.write_text(json.dumps(payload))
        with csv_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=[
                "status", "json_path", "graph_sha256", "benchmark_binary_sha256",
                "algorithm_source", "l3_size", "l3_ways"])
            writer.writeheader()
            writer.writerow(dict(status="ok", json_path=str(data_path), graph_sha256="a" * 64,
                                 benchmark_binary_sha256="b" * 64, algorithm_source=0, l3_size="1kB", l3_ways=2))
        jobs.append(SimpleNamespace(output_csv=csv_path, metadata={"window_observer": mode}))
    assert validate_window_observer_pairs(jobs)[0] is True
    assert validate_window_observer_pairs(jobs[:1])[0] is False
    assert validate_window_observer_pairs([*jobs, jobs[0]])[0] is False


@pytest.mark.parametrize("extra", [
    ["--algorithm", "sssp"], ["--policy", "LRU"], ["--mode", "replacement"],
    ["--bfs-direction", "do"], ["--record-preprocess", "traversal"],
])
def test_window_observer_rejects_incompatible_paths_before_graph_loading(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(binary), "--algorithm", "bfs", "--graph", str(tmp_path / "not-read.sg"),
        "--policy", "GRASP_PAPER", "--window-observer", "window", *extra,
    ], capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2
    assert "window observer requires" in ran.stderr or "current-record-modes-own" in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_window_observer_rejects_native_runners(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "bfs",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --window-observer window",
        "--prefetcher", "none", "--flowthrough", "off",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("GRASP_PAPER"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "window observer is cache_sim-only"


def test_window_observer_profile_is_one_paired_condition(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_window_observer", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 2
    assert {parse_options(job.metadata["options"]).window_observer for job in jobs} == {"control", "window"}
    assert {tuple(job.metadata["expected_policy_labels"]) for job in jobs} == {
        ("GRASP_PAPER_OBS_CONTROL",), ("GRASP_PAPER_OBS_WINDOW",)}
    for job in jobs:
        assert job.metadata["benchmark"] == "bfs" and job.metadata["policies"] == ["GRASP_PAPER"]
        assert parse_options(job.metadata["options"]).bfs_direction == "td"
        assert job.command[job.command.index("--l3-sizes") + 1] == "8MB"
        assert job.command[job.command.index("--cache-sim-omp-threads") + 1] == "1"


@pytest.mark.parametrize("width", [4, 8])
@pytest.mark.parametrize("mode", ["transport", "replacement", "prefetch", "replacement-prefetch"])
def test_traversal_preprocessing_preserves_algorithm_work(tmp_path, width, mode):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    config = json.loads((ROOT / "scripts/experiments/ecg/configs/algorithm_equivalence.json").read_text())
    for filename, (data, _) in algorithm_outputs().items():
        (tmp_path / filename).write_bytes(data)
    for algorithm, scope in (("bfs", "row-local"), ("sssp", "light-heavy"),
                             ("cc", "sample-rounds"), ("bc", "row-local")):
        results = []
        for preprocessing in ("csr", "traversal"):
            output = tmp_path / f"{algorithm}-{preprocessing}.json"
            ran = subprocess.run([
                str(binary), "--algorithm", algorithm,
                "--graph", str(tmp_path / config["references"][algorithm]["graph"]),
                "--delta", "2", "--mode", mode, "--record-bytes", str(width),
                "--record-preprocess", preprocessing, "--values", "--evidence",
                "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256",
                "--l2-ways", "2", "--llc-bytes", "512", "--llc-ways", "2",
                "--output", str(output),
            ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
                capture_output=True, text=True, timeout=20, check=False)
            assert ran.returncode == 0, ran.stdout + ran.stderr
            payload = json.loads(output.read_text())
            from scripts.experiments.ecg.algorithm_matrix import validate_traffic_phases
            counters = validate_traffic_phases(payload)
            assert "kernel_llc_property_misses" in counters
            work = payload["workload"]
            assert work["record_preprocess"] == preprocessing
            assert work["record_reuse_scope"] == ("full-csr" if preprocessing == "csr" else scope)
            results.append(work)
        for field in ("result_digest", "work_trace_digest", "position_trace_digest",
                      "actual_records", "passes", "structural_positions", "skipped_positions",
                      "values_u32", "values_u64", "values_f32", "carrier_allocation_bytes"):
            assert results[0][field] == results[1][field], (algorithm, field)
        assert results[0]["record_trace_digest"] != results[1]["record_trace_digest"], algorithm


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
    for algorithm, partitions in (("sssp", 2), ("cc", 3), ("bfs", 1), ("bc", 1)):
        original = plan_algorithm_resources(graph, algorithm=algorithm, **options)
        specialized = plan_algorithm_resources(graph, algorithm=algorithm, preprocessing="traversal", **options)
        assert specialized["construction_auxiliary_bytes_upper"] == (
            original["construction_auxiliary_bytes_upper"] * partitions)
        assert specialized["carrier_payload_bytes_upper"] == original["carrier_payload_bytes_upper"]
    with pytest.raises(RecordResourceError, match="preprocessing"):
        plan_algorithm_resources(graph, algorithm="bfs", preprocessing="oracle", **options)
    popt = plan_algorithm_resources(graph, algorithm="bc", popt_full_capacity=True,
                                    **{**options, "records": False})
    assert popt["popt_matrix_bytes_upper"] == (2 * 32 + 64) * 256
    assert popt["carrier_payload_bytes_upper"] == 0
    with pytest.raises(RecordResourceError, match="P-OPT"):
        plan_algorithm_resources(graph, algorithm="bc", popt_full_capacity=True, **options)


def test_preprocessing_profile_has_matched_serial_controls(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_preprocessing_8mb_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 8
    cells = 0
    for job in jobs:
        command = job.command
        options = parse_options(command[command.index("--options") + 1])
        assert options.bfs_direction == "td"
        expected = ["ECG:transport", "ECG:replacement"]
        if options.record_preprocess == "csr":
            expected.insert(0, "LRU")
        assert job.metadata["policies"] == expected
        cells += len(expected)
        assert job.metadata["benchmark"] in ("bfs", "sssp", "cc", "bc")
        assert command[command.index("--l3-sizes") + 1] == "8MB"
        assert command[command.index("--cache-sim-omp-threads") + 1] == "1"
    assert cells == 20


def test_grasp_profile_covers_all_shared_kernels_without_rebuilding_controls(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_grasp_8mb_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 8
    grasp = [job for job in jobs if job.metadata["policies"] == ["GRASP_PAPER"]]
    assert {job.metadata["benchmark"] for job in grasp} == {"spmv", "bfs", "sssp", "cc", "bc", "tc"}
    controls = [job for job in jobs if job not in grasp]
    assert {job.metadata["benchmark"] for job in controls} == {"spmv", "tc"}
    assert all(job.metadata["policies"] == ["LRU", "ECG:transport", "ECG:replacement"] for job in controls)
    assert sum(len(job.metadata["policies"]) for job in jobs) == 12
    for job in jobs:
        command = job.command
        options = parse_options(command[command.index("--options") + 1])
        assert options.record_preprocess == "csr" and options.bfs_direction == "td"
        assert command[command.index("--l3-sizes") + 1] == "8MB"
        assert command[command.index("--cache-sim-omp-threads") + 1] == "1"


def test_competitive_profile_keeps_capacity_work_and_base_labels_explicit(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_competitive_8mb_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 18
    assert sum(len(job.metadata["policies"]) for job in jobs) == 42
    for job in jobs:
        options = parse_options(job.metadata["options"])
        assert options.bfs_direction == "td"
        assert job.command[job.command.index("--l3-sizes") + 1] == "8MB"
        assert job.command[job.command.index("--l3-ways") + 1] == "16"
        if options.record_base_policy == "GRASP_PAPER":
            assert set(job.metadata["expected_policy_labels"]) == {
                "ECG_TRANSPORT_BASE_GRASP_PAPER", "ECG_REPLACEMENT_BASE_GRASP_PAPER"}
        elif job.stage.startswith("168_"):
            assert job.metadata["policies"] == ["LRU", "GRASP_PAPER", "POPT:UNCHARGED"]
        else:
            assert job.metadata["policies"] == ["ECG:transport", "ECG:replacement"]
        if not job.stage.startswith("168_"):
            assert options.record_preprocess == (
                "traversal" if job.metadata["benchmark"] in ("sssp", "cc") else "csr")


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
                       ("variant", "legacy-sssp"), ("prediction_semantics", "dense-actual-designated-read"),
                       ("record_preprocess", "traversal"), ("record_reuse_scope", "row-local"),
                       ("constructed_unknown_records", 1 << 32)):
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


def test_current_grasp_base_runner_rows_have_distinct_provenance(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("built current algorithm is required")
    data, _ = algorithm_outputs()["weighted-diamond.wsg"]
    graph = tmp_path / "weighted-diamond.wsg"
    graph.write_bytes(data)
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "spmv",
        "--options", f"--graph {graph} --repeat 2 --record-base-policy GRASP_PAPER",
        "--policies", "ECG:transport", "ECG:replacement", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "512B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    rows = json.loads((output / "roi_matrix.json").read_text())
    completion = json.loads((output / "roi_matrix.complete.json").read_text())
    assert set(completion["policy_labels"]) == {row["policy_label"] for row in rows}
    assert set(completion["expected_policy_labels"]) == {row["policy_label"] for row in rows}
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    policies = ["ECG:transport", "ECG:replacement"]
    assert csv_status(output / "roi_matrix.csv", policies, "GRASP_PAPER")[0] == "ok"
    assert csv_status(output / "roi_matrix.csv", policies)[0] == "partial"
    assert {row["policy_label"] for row in rows} == {
        "ECG_TRANSPORT_BASE_GRASP_PAPER",
        "ECG_REPLACEMENT_BASE_GRASP_PAPER",
    }
    assert all(row["record_base_policy"] == "GRASP_PAPER" and
               row["setup_cache_policy"] == "GRASP_PAPER" for row in rows)
    replacement = next(
        row for row in rows
        if row["policy_label"] == "ECG_REPLACEMENT_BASE_GRASP_PAPER")
    assert replacement["traffic_ratio_vs_transport"] > 0


def test_detailed_algorithm_commands_bind_the_current_guest_and_geometry(tmp_path):
    from scripts.experiments.ecg import algorithm_detailed, algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    args = roi_matrix.parse_args([
        "--suite", "gem5", "--current-algorithms", "--ecg-equivalence", "--benchmark", "sssp",
        "--l3-sizes", "1kB", "2kB", "--ecg-record-bytes", "8",
    ])
    options = algorithm_matrix.parse_options("--graph /tmp/explicit.wsg --source 3 --delta 16")
    values = algorithm_detailed.guest_options(
        args, options, {"graph_loader_bytes_upper": 4096}, parse_policy_spec("ECG"),
        roi_matrix.parse_size_bytes, "2kB", tmp_path / "result.json")
    assert values[values.index("--algorithm") + 1] == "sssp"
    assert values[values.index("--llc-bytes") + 1] == "2048"
    assert values[values.index("--mode") + 1] == "replacement-prefetch"
    assert values[values.index("--record-base-policy") + 1] == "LRU"
    assert values[values.index("--record-bytes") + 1] == "8"
    assert "--evidence" in values and "--values" in values
    assert roi_matrix.graph_path_from_options("--graph /tmp/explicit.wsg") == Path("/tmp/explicit.wsg")
    assert "bench/src_gem5/algorithms.cc" in algorithm_matrix.source_paths("gem5")
    assert "bench/src_sniper/algorithms.cc" in algorithm_matrix.source_paths("sniper")
    args.dry_run = True
    args.gem5_cpu_type = "O3"
    services = SimpleNamespace(
        selected_gem5_isa=lambda: "riscv",
        VALIDATED_GEM5_GUEST=tmp_path / "guest",
        VALIDATED_GEM5_GUEST_SHA256="a" * 64,
        verify_staged_guest=lambda *_args: None,
        gem5_sideband_paths=roi_matrix.gem5_sideband_paths,
        GEM5_OPT=tmp_path / "gem5.opt", GEM5_CONFIG=tmp_path / "graph_se.py",
        parse_size_bytes=roi_matrix.parse_size_bytes, run_command=lambda *_args, **_kwargs: None)
    environment = {}
    algorithm_detailed.run_gem5(
        args, parse_policy_spec("ECG"), "2kB", options, {"graph_loader_bytes_upper": 4096},
        tmp_path, tmp_path / "result.json", tmp_path / "run.log", environment, services)
    paths = roi_matrix.gem5_sideband_paths(tmp_path)
    for name, key in (("GEM5_GRAPHBREW_CTX", "context"), ("GEM5_POPT_MATRIX", "popt_matrix"),
                      ("GEM5_GRAPHBREW_OUT_EDGES", "out_edges"), ("GEM5_GRAPHBREW_IN_EDGES", "in_edges")):
        assert environment.get(name) == str(paths[key]), "all startup-cleanup paths must be cell-local"


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_detailed_preprocessing_is_rejected_before_loading_graph(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "sssp",
        "--options", f"--graph {tmp_path / 'not-read.wsg'} --record-preprocess traversal",
        "--prefetcher", "none", "--flowthrough", "off",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB", backend, services)
    assert rows[0]["status"] == "error"
    assert rows[0]["error"] == "traversal preprocessing is currently cache_sim-only"


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_detailed_grasp_record_base_is_rejected_before_loading_graph(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "sssp",
        "--options", f"--graph {tmp_path / 'not-read.wsg'} --record-base-policy GRASP_PAPER",
        "--prefetcher", "none", "--flowthrough", "off",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB", backend, services)
    assert rows[0]["status"] == "error"
    assert rows[0]["error"] == "GRASP_PAPER record base is cache_sim-only"


def test_grasp_record_base_labels_cannot_alias_lru_base():
    from scripts.experiments.ecg.algorithm_matrix import record_policy_label
    assert record_policy_label("ECG_TRANSPORT", "transport", "LRU") == "ECG_TRANSPORT"
    assert record_policy_label(
        "ECG_TRANSPORT", "transport", "GRASP_PAPER") == (
            "ECG_TRANSPORT_BASE_GRASP_PAPER")
    assert record_policy_label(
        "ECG_REPLACEMENT", "replacement", "GRASP_PAPER") == (
            "ECG_REPLACEMENT_BASE_GRASP_PAPER")
    assert record_policy_label("GRASP_PAPER", "csr", "LRU") == "GRASP_PAPER"


def test_rv64_sideband_publication_uses_supported_atomic_syscall(tmp_path):
    source = tmp_path / "publish.cc"
    source.write_text(r'''
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include "ecg_algorithm_sideband.h"
unsigned calls = 0;
extern "C" long syscall(long number, ...) noexcept {
    if (number != 38) std::abort();
    va_list arguments;
    va_start(arguments, number);
    const int old_fd = va_arg(arguments, int);
    const char* old_path = va_arg(arguments, const char*);
    const int new_fd = va_arg(arguments, int);
    const char* new_path = va_arg(arguments, const char*);
    va_end(arguments);
    if (old_fd != -100 || new_fd != -100) std::abort();
    ++calls;
    return std::rename(old_path, new_path);
}
int main(int argc, char** argv) {
    if (argc != 2 || setenv("GEM5_GRAPHBREW_CTX", argv[1], 1)) return 2;
    uint64_t offsets[] = {0, 1, 1};
    int32_t columns[] = {1};
    uint64_t input_offsets[] = {0, 1, 2};
    int32_t input_columns[] = {1, 0};
    float values[] = {1, 2};
    ecg_algorithm::AlgorithmSideband sideband("GEM5_GRAPHBREW_CTX");
    sideband.graph({2, 2, false, input_offsets, input_columns, nullptr, 4, false});
    sideband.region("x", values, 2, 4, true);
    sideband.activeGraph({2, 1, true, offsets, columns, nullptr, 4, false});
    sideband.publish();
    return calls == 1 ? 0 : 1;
}
''')
    binary = tmp_path / "publish"
    built = subprocess.run([
        "g++", "-std=c++17", "-O2", "-fopenmp", "-D__riscv=1", "-D__riscv_xlen=64",
        "-I", str(ROOT / "bench/include"), "-I", str(ROOT / "bench/include/external/gapbs"),
        str(source), "-o", str(binary),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert built.returncode == 0, built.stderr
    output = tmp_path / "sideband.json"
    ran = subprocess.run([str(binary), str(output)], timeout=10, check=False)
    assert ran.returncode == 0, "RV64 publication must not invoke unsupported renameat2"
    published = json.loads(output.read_text())
    assert published["num_vertices"] == 2 and published["num_edges"] == 1
    assert published["input_num_edges"] == 2
    assert published["csr_offsets_base"] != published["input_csr_offsets_base"]
    assert not output.with_suffix(".json.new").exists()


def test_detailed_rows_preserve_existing_guest_provenance_gate(tmp_path, monkeypatch):
    import csv
    from scripts.experiments.ecg import algorithm_detailed as detailed, roi_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.record_resources import GraphInfo
    digest = "a" * 64
    guest = tmp_path / "algorithms_riscv_m5ops"
    guest.write_bytes(b"unit guest")
    args = roi_matrix.parse_args([
        "--suite", "gem5", "--current-algorithms", "--ecg-equivalence",
        "--benchmark", "spmv", "--options", "--graph /tmp/fixture.sg",
        "--expected-gem5-guest-sha256", digest,
    ])
    graph = GraphInfo(False, 4, 2, 3, 100, digest)
    monkeypatch.setattr(detailed, "graph_info", lambda *_args, **_kwargs: graph)
    monkeypatch.setattr(detailed.algorithms, "validate_payload",
                        lambda *_args, **_kwargs: {"algorithm": "spmv", "result_digest": "0" * 16})
    def launch(_args, _spec, _size, _options, _plan, _directory, output, log, _env, _roi):
        output.write_text("{}")
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("")
        return subprocess.CompletedProcess([], 0)
    monkeypatch.setattr(detailed, "run_gem5", launch)
    services = SimpleNamespace(
        VALIDATED_GEM5_GUEST=guest, hash_input_path=lambda _path: digest,
        parse_size_bytes=roi_matrix.parse_size_bytes, parse_gem5_sections=lambda _path: [{}])
    rows = detailed.run_cell(args, tmp_path, roi_matrix.parse_policy_spec("LRU"), "1kB", "gem5", services)
    assert rows[0]["status"] == "ok", rows
    output = tmp_path / "roi_matrix.csv"
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    job = experiment_run.Job("unit", "unit", "roi_matrix", [], tmp_path, tmp_path / "job.log",
                             {"expected_gem5_guest_sha256": digest})
    ok, error = experiment_run.validate_cross_job_guest_hashes([job])
    assert ok, error


def test_sniper_metrics_report_observed_llc_activity():
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    metrics = {"l1d_loads": 100, "l1d_load_misses": 30,
               "l2_loads": 30, "l2_load_misses": 20,
               "llc_loads": 20, "llc_load_misses": 8}
    row = {"status": "ok", **algorithm_detailed.normalize_sniper_metrics(metrics)}
    roi_matrix.annotate_l3_pressure(row)
    assert row["l3_accesses"] == 20 and row["l3_misses"] == 8
    assert row["l3_miss_rate"] == 0.4 and row["l3_exercised"] is True


def test_setup_and_kernel_traffic_must_close():
    from scripts.experiments.ecg.algorithm_matrix import validate_traffic_phases
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    setup = {"total_accesses": 100, "memory_accesses": 20, "prefetch_fills": 0,
             "llc_writebacks": 5, "total_offchip_traffic": 25}
    kernel = {"total_accesses": 200, "memory_accesses": 30, "prefetch_fills": 2,
              "llc_writebacks": 8, "total_offchip_traffic": 40}
    payload = {"metrics": {key: setup[key] + kernel[key] for key in setup},
               "traffic_phases": {"boundary": "first-binding-complete", "cache_state_preserved": True,
                                  "setup": setup, "kernel": kernel}}
    assert validate_traffic_phases(payload)["kernel_total_offchip_traffic"] == 40
    broken = copy.deepcopy(payload)
    broken["traffic_phases"]["kernel"]["total_offchip_traffic"] = 65
    with pytest.raises(RecordReceiptError, match="do not close"):
        validate_traffic_phases(broken)
    broken = copy.deepcopy(payload)
    broken["traffic_phases"]["cache_state_preserved"] = False
    with pytest.raises(RecordReceiptError, match="nonintrusive"):
        validate_traffic_phases(broken)
    for key, total, before, after in (
            ("llc_hits", "hits", 9, 100), ("llc_misses", "misses", 20, 30),
            ("llc_property_hits", "prop_hits", 5, 80), ("llc_property_misses", "prop_misses", 12, 18)):
        setup[key], kernel[key] = before, after
        payload["metrics"].setdefault("L3", {})[total] = before + after
    assert validate_traffic_phases(payload)["kernel_llc_property_misses"] == 18
    broken = copy.deepcopy(payload)
    broken["traffic_phases"]["kernel"]["llc_property_misses"] = 31
    broken["metrics"]["L3"]["prop_misses"] = 43
    with pytest.raises(RecordReceiptError, match="property LLC"):
        validate_traffic_phases(broken)


def test_matched_8mb_profile_uses_equal_geometry_and_real_pressure():
    manifest = json.loads((ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stages = [stage for stage in manifest["stages"] if "ecg_matched_8mb_cache" in stage["profiles"]]
    assert len(stages) == 2
    assert {stage["benchmarks"][0] for stage in stages} == {"pr", "spmv"}
    for key in ("graph_set", "policies", "l1d_size", "l1d_ways", "l2_size", "l2_ways",
                "l3_sizes", "l3_ways", "line_size", "prefetcher", "flowthrough"):
        assert stages[0].get(key) == stages[1].get(key)
    assert stages[0]["l3_sizes"] == ["8MB"] and stages[0]["l3_ways"] == "16"
    graph = manifest["graph_sets"][stages[0]["graph_set"]][0]
    assert graph["expected_vertices"] * 4 > 8 << 20
    options = manifest["benchmark_options"][graph["options_key"]]
    assert "-i 2" in options["pr"] and "--repeat 2" in options["spmv"]
    assert len(stages[0]["policies"]) == 5


def test_dynamic_8mb_profile_is_six_bounded_cells(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_dynamic_8mb_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 2
    assert {job.metadata["benchmark"] for job in jobs} == {"bfs", "sssp"}
    for job in jobs:
        assert len(job.metadata["policies"]) == 3
        assert job.metadata["process_tree_rss_mib"] == 4096
        assert job.command[job.command.index("--l3-sizes") + 1] == "8MB"
        assert job.command[job.command.index("--l3-ways") + 1] == "16"
    weighted = next(job for job in jobs if job.metadata["benchmark"] == "sssp")
    assert "--delta 8" in weighted.command[weighted.command.index("--options") + 1]
    assert weighted.metadata["graph_path"].endswith("-w32.wsg")


def test_dobfs_profile_is_one_bounded_three_policy_trial(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    from scripts.experiments.ecg.record_resources import GraphInfo, plan_algorithm_resources
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    args = experiment_run.parse_args(["--profile", "ecg_dobfs_8mb_cache", "--list"])
    jobs = experiment_run.expand_jobs(args, manifest, tmp_path)
    assert len(jobs) == 1 and jobs[0].metadata["policies"] == ["LRU", "ECG:transport", "ECG:replacement"]
    command = jobs[0].command
    options = parse_options(command[command.index("--options") + 1])
    assert options.bfs_direction == "do" and options.bfs_alpha == 15 and options.bfs_beta == 18
    plan = plan_algorithm_resources(
        GraphInfo(False, 130, 200, 129, 2000, "a" * 64), algorithm="bfs",
        records=False, requested_bytes=4, minimum_mantissa_bits=0, traversals=1,
        sources=1, workspace_limit=1 << 20, carrier_limit=1 << 20,
        auxiliary_limit=1 << 20, rss_mib=1024, bfs_direction_optimizing=True)
    assert plan["array_bytes"] == 12 * 130 + 2 * 3 * 8
