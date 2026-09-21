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
                output = tmp_path / "same-result.json"
                ran = subprocess.run([
                    "setarch", os.uname().machine, "-R", str(binary),
                    "--algorithm", "spmv", "--graph", str(graph),
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


@pytest.mark.parametrize("algorithm", ["spmv", "bfs"])
def test_popt_rank_ablation_retains_matrix_setup_and_work(tmp_path, algorithm):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    results = []
    for rank_mode in ("future", "constant"):
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary),
            "--algorithm", algorithm, "--graph", str(graph), "--policy", "POPT_UNCHARGED",
            "--popt-rank-mode", rank_mode, "--delta", "8",
            "--repeat", "2" if algorithm == "spmv" else "1", "--values", "--evidence",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output),
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
            capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        popt = payload["popt"]
        assert popt["rank_mode"] == rank_mode
        assert payload["policy_ablation"] is (rank_mode == "constant")
        assert popt["role"] == ("policy-ablation" if rank_mode == "constant" else "favorable-quality-control")
        assert popt["lookup_calls"] > 0 and popt["original_rank_sum"] > 0
        assert popt["constant_rank_lookups"] == (popt["lookup_calls"] if rank_mode == "constant" else 0)
        assert popt["full_data_capacity"] is True and popt["runtime_matrix_traffic_charged"] is False
        assert payload["metrics"]["L3"]["size_bytes"] == 1024 and payload["metrics"]["L3"]["ways"] == 2
        options = algorithm_matrix.parse_options(
            f"--graph {graph} --repeat {'2' if algorithm == 'spmv' else '1'} --popt-rank-mode {rank_mode}")
        validation = dict(algorithm=algorithm, mode="csr", policy="POPT_UNCHARGED",
            graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"), graph_path=graph,
            options=options, requested_bytes=0, minimum_mantissa_bits=0, evidence=True, llc_sets=8)
        algorithm_matrix.validate_payload(payload, ran.stdout, **validation)
        forged = json.loads(json.dumps(payload))
        forged["popt"]["rank_mode"] = "constant" if rank_mode == "future" else "future"
        with pytest.raises(RecordReceiptError, match="future-rank"):
            algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
        forged = json.loads(json.dumps(payload))
        forged["policy_ablation"] = rank_mode != "constant"
        with pytest.raises(RecordReceiptError, match="rank-ablation"):
            algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
        results.append(payload)
    future, constant = results
    assert future["workload"] == constant["workload"]
    assert future["traffic_phases"]["setup"] == constant["traffic_phases"]["setup"]
    for key in ("matrix_digest", "matrix_bytes", "matrix_lines", "epochs", "banks", "covered_regions",
                "construction_read_bytes", "construction_write_bytes", "passes", "vertices", "governed_reads"):
        assert future["popt"][key] == constant["popt"][key], key


@pytest.mark.parametrize("extra", [
    [], ["--policy", "GRASP_PAPER"], ["--policy", "POPT_UNCHARGED", "--algorithm", "sssp"],
    ["--policy", "POPT_UNCHARGED", "--bfs-direction", "do"],
    ["--policy", "POPT_UNCHARGED", "--mode", "transport"],
    ["--policy", "POPT_UNCHARGED", "--popt-rank-mode", "invalid"],
])
def test_popt_rank_ablation_rejects_unsupported_cli_combinations(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(binary), "--algorithm", "bfs", "--graph", str(tmp_path / "not-read.sg"),
        "--popt-rank-mode", "constant", *extra,
    ], capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2 and "ECG-ALGORITHM-ERROR" in ran.stderr
    assert "not-read.sg" not in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_popt_rank_ablation_rejects_native_runners(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "bfs",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --popt-rank-mode constant",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("LRU"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "P-OPT rank ablation is cache_sim-only"


def test_popt_rank_profile_is_six_fixed_information_attribution_cells(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(experiment_run.parse_args(
        ["--profile", "ecg_popt_rank_attribution", "--list"]), manifest, tmp_path)
    assert len(jobs) == 5 and sum(len(job.metadata["policies"]) for job in jobs) == 6
    assert manifest["profile_controls"]["ecg_popt_rank_attribution"]["development_gate"]["minimum_margin"] == 0.02
    cells = set()
    for job in jobs:
        options = parse_options(job.metadata["options"])
        assert options.graph.name == "cit-Patents-dbg.sg"
        assert options.source == 0 and options.record_model == "next" and options.window_observer == "off"
        assert options.bfs_direction == "td" and options.record_preprocess == "csr"
        assert job.metadata["l3_sizes"] == ["8MB"]
        assert job.command[job.command.index("--l3-ways") + 1] == "16"
        assert job.command[job.command.index("--cache-sim-omp-threads") + 1] == "1"
        assert job.command[job.command.index("--prefetcher") + 1] == "none"
        benchmark = job.metadata["benchmark"]
        assert options.repeat == (2 if benchmark == "spmv" else 1)
        assert options.bfs_traffic_phases == ("on" if benchmark == "bfs" else "off")
        for label in job.metadata["expected_policy_labels"]:
            assert options.grasp_scope == (
                "graph-passes" if label == "GRASP_PAPER_GRAPH_PASSES" else "all")
            assert options.popt_rank_mode == ("constant" if label.endswith("_CONST_RANK") else "future")
            cells.add((benchmark, label))
    assert cells == {
        ("spmv", "GRASP_PAPER"), ("bfs", "GRASP_PAPER_GRAPH_PASSES"),
        ("spmv", "POPT_UNCHARGED"), ("bfs", "POPT_UNCHARGED"),
        ("spmv", "POPT_UNCHARGED_CONST_RANK"), ("bfs", "POPT_UNCHARGED_CONST_RANK"),
    }


@pytest.mark.parametrize("rank_mode", ["future", "constant"])
def test_popt_rank_runner_receipts_cannot_alias_the_real_baseline(tmp_path, rank_mode):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "spmv",
        "--options", f"--graph {graph} --repeat 2 --popt-rank-mode {rank_mode}",
        "--policies", "POPT:UNCHARGED", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "1024B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    rows = json.loads((output / "roi_matrix.json").read_text())
    completion = json.loads((output / "roi_matrix.complete.json").read_text())
    label = "POPT_UNCHARGED_CONST_RANK" if rank_mode == "constant" else "POPT_UNCHARGED"
    assert len(rows) == 1 and rows[0]["policy_label"] == label
    assert rows[0]["popt_rank_mode"] == rank_mode
    assert rows[0]["policy_ablation"] == ("1" if rank_mode == "constant" else "0")
    assert completion["policy_labels"] == completion["expected_policy_labels"] == [label]
    assert csv_status(output / "roi_matrix.csv", ["POPT:UNCHARGED"], popt_rank_mode=rank_mode)[0] == "ok"
    other = "constant" if rank_mode == "future" else "future"
    assert csv_status(output / "roi_matrix.csv", ["POPT:UNCHARGED"], popt_rank_mode=other)[0] == "partial"


def test_grasp_reference_retains_matrix_setup_and_program_work(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    results = []
    for mode in ("flat", "full"):
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary),
            "--algorithm", "spmv", "--graph", str(graph), "--policy", "GRASP_PAPER",
            "--grasp-reference", mode, "--repeat", "2", "--delta", "8", "--values", "--evidence",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output),
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0",
                "POPT_MATRIX_STREAM_SIM": "1"}, capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        reference, matrix = payload["grasp_reference"], payload["popt"]
        assert reference["schema"] == "ecg.grasp-reference.v1" and reference["mode"] == mode
        assert reference["availability"] == "ideal-matrix-at-victim-selection"
        assert reference["base_policy"] == reference["outside_pass_policy"] == "GRASP_PAPER"
        assert reference["candidate_rrpv_filter"] is False and reference["strictly_farther_only"] is True
        assert reference["covered_base_victims"] > 0 and matrix["lookup_calls"] > 0
        assert matrix["original_rank_sum"] > 0
        assert matrix["rank_mode"] == ("constant" if mode == "flat" else "future")
        assert matrix["constant_rank_lookups"] == (matrix["lookup_calls"] if mode == "flat" else 0)
        assert matrix["consumer"] == "GRASP-reference" and matrix["role"] == "reference-consumer-diagnostic"
        assert payload["setup_cache_policy"] == "GRASP_PAPER" and payload["diagnostic_only"] is True
        assert payload["policy_ablation"] is (mode == "flat")
        assert payload["measurement_scope"] == "ideal-availability-reference-consumer"
        assert payload["metrics"]["L3"]["size_bytes"] == 1024 and payload["metrics"]["L3"]["ways"] == 2
        assert payload["metrics"]["popt_matrix_stream_lines_simulated"] == 0
        assert payload["workload"]["carrier_allocation_bytes"] == 0
        if mode == "flat":
            assert reference["victim_overrides"] == reference["lower_rrpv_overrides"] == 0
        options = algorithm_matrix.parse_options(f"--graph {graph} --repeat 2 --grasp-reference {mode}")
        validation = dict(algorithm="spmv", mode="csr", policy="GRASP_PAPER",
            graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"), graph_path=graph,
            options=options, requested_bytes=0, minimum_mantissa_bits=0, evidence=True, llc_sets=8)
        algorithm_matrix.validate_payload(payload, ran.stdout, **validation)
        algorithm_matrix.validate_traffic_phases(payload)
        for section, field, value in (
            ("grasp_reference", "mode", "off"), ("grasp_reference", "candidate_rrpv_filter", True),
            ("grasp_reference", "strictly_farther_only", False), ("grasp_reference", "outside_pass_policy", "LRU"),
            ("grasp_reference", "covered_base_victims", 0), ("popt", "matrix_bytes", 0),
            ("popt", "rank_mode", "future" if mode == "flat" else "constant"),
            ("popt", "consumer", "POPT"), ("popt", "role", "favorable-quality-control"),
            ("popt", "runtime_matrix_traffic_charged", True),
        ):
            forged = json.loads(json.dumps(payload))
            forged[section][field] = value
            if field == "covered_base_victims":
                forged["grasp_reference"]["victim_overrides"] = 1
            with pytest.raises(RecordReceiptError):
                algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
        results.append(payload)
    flat, full = results
    assert flat["workload"] == full["workload"]
    assert flat["traffic_phases"]["setup"] == full["traffic_phases"]["setup"]
    for key in ("matrix_digest", "matrix_bytes", "matrix_lines", "epochs", "banks", "covered_regions",
                "construction_read_bytes", "construction_write_bytes", "workspace_peak_bytes",
                "passes", "vertices", "governed_reads"):
        assert flat["popt"][key] == full["popt"][key], key


def test_grasp_reference_rank_first_receipt_passes_the_runner_validator(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    results = {}
    # Four ways: with two, base-first refinement and rank-first selection coincide.
    for mode in ("full", "rank"):
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary),
            "--algorithm", "spmv", "--graph", str(graph), "--policy", "GRASP_PAPER",
            "--grasp-reference", mode, "--repeat", "2", "--delta", "8", "--values", "--evidence",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "2048", "--llc-ways", "4", "--output", str(output),
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0",
                "POPT_MATRIX_STREAM_SIM": "1"}, capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        options = algorithm_matrix.parse_options(f"--graph {graph} --repeat 2 --grasp-reference {mode}")
        validation = dict(algorithm="spmv", mode="csr", policy="GRASP_PAPER",
            graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"), graph_path=graph,
            options=options, requested_bytes=0, minimum_mantissa_bits=0, evidence=True, llc_sets=8)
        algorithm_matrix.validate_payload(payload, ran.stdout, **validation)
        reference = payload["grasp_reference"]
        rank_first = mode == "rank"
        assert reference["mode"] == mode and reference["rank_first"] is rank_first
        assert reference["strictly_farther_only"] is (not rank_first)
        assert payload["popt"]["rank_mode"] == "future" and payload["policy_ablation"] is False
        if rank_first:
            assert reference["max_rank_tie_population"] >= reference["covered_base_victims"] > 0
        else:
            assert reference["max_rank_tie_population"] == reference["basefirst_divergence"] == 0
        for field, value in (
            ("strictly_farther_only", rank_first), ("rank_first", not rank_first),
            ("max_rank_tie_population", 0 if rank_first else 1),
            ("basefirst_divergence", reference["covered_base_victims"] + 1 if rank_first else 1),
        ):
            forged = json.loads(json.dumps(payload))
            forged["grasp_reference"][field] = value
            with pytest.raises(RecordReceiptError):
                algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
        assert algorithm_matrix.grasp_reference_label(
            "GRASP_PAPER", mode) == "DIAG_GRASP_REFERENCE_" + mode.upper()
        results[mode] = payload
    full, rank = results["full"], results["rank"]
    assert full["workload"] == rank["workload"]
    assert full["traffic_phases"]["setup"] == rank["traffic_phases"]["setup"]
    for key in ("matrix_digest", "matrix_bytes", "matrix_lines", "epochs", "construction_read_bytes",
                "construction_write_bytes", "passes", "vertices", "governed_reads"):
        assert full["popt"][key] == rank["popt"][key], key


@pytest.mark.parametrize("extra", [
    ["--algorithm", "bfs"], ["--policy", "LRU"], ["--policy", "POPT_UNCHARGED"],
    ["--mode", "transport"], ["--popt-rank-mode", "constant"], ["--grasp-scope", "graph-passes"],
    ["--bfs-direction", "do"], ["--record-model", "window"], ["--record-model", "frontier"],
    ["--window-observer", "control"], ["--bfs-traffic-phases", "on"],
    ["--record-preprocess", "traversal"], ["--sources", "0,1"], ["--grasp-reference", "invalid"],
])
def test_grasp_reference_rejects_unsupported_cli_before_loading(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(binary), "--algorithm", "spmv", "--graph", str(tmp_path / "not-read.sg"),
        "--policy", "GRASP_PAPER", "--grasp-reference", "full", *extra,
    ], capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2 and "ECG-ALGORITHM-ERROR" in ran.stderr
    assert "not-read.sg" not in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_grasp_reference_rejects_native_paths(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "spmv",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --grasp-reference full",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("GRASP_PAPER"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "GRASP reference diagnostic is cache_sim-only"


def test_grasp_reference_off_preserves_ordinary_grasp(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    results = []
    controls = (["--record-preprocess", "csr"], ["--grasp-reference", "off"])
    padding = max(sum(len(value) for value in extra) for extra in controls)
    for extra in controls:
        # Hold launcher size and allocation class fixed for this real-address cache fixture.
        env = {**os.environ, "GRAPHBREW_TEST_ARGV_PADDING": "x" * (padding - sum(len(value) for value in extra))}
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary), "--algorithm", "spmv",
            "--graph", str(graph), "--policy", "GRASP_PAPER", "--repeat", "2", "--values", "--evidence",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output), *extra,
        ], env=env, capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        assert payload["popt"] is None and payload["grasp_reference"] is None
        assert payload["diagnostic_only"] is False
        results.append(payload)
    for field in ("workload", "metrics", "traffic_phases"):
        assert results[0][field] == results[1][field]


def test_grasp_reference_profile_is_four_same_work_cells(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg import algorithm_matrix
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(experiment_run.parse_args(
        ["--profile", "ecg_grasp_reference_cache", "--list"]), manifest, tmp_path)
    assert len(jobs) == 3 and sum(len(j.metadata["policies"]) for j in jobs) == 4
    gate = manifest["profile_controls"]["ecg_grasp_reference_cache"]["development_gate"]
    assert gate["minimum_margin"] == 0.02 and gate["full_graph_runs"] == 4
    assert gate["no_automatic_follow_on"] is True
    labels = set()
    diagnostics = []
    for job in jobs:
        options = algorithm_matrix.parse_options(job.metadata["options"])
        assert job.metadata["benchmark"] == "spmv" and options.graph.name == "cit-Patents-dbg.sg"
        assert options.repeat == 2 and options.record_model == "next" and options.record_preprocess == "csr"
        assert options.grasp_scope == "all" and options.window_observer == "off" and options.popt_rank_mode == "future"
        assert job.metadata["l3_sizes"] == ["8MB"]
        for flag, value in (("--l1d-size", "32kB"), ("--l1d-ways", "8"), ("--l2-size", "256kB"),
                            ("--l2-ways", "8"), ("--l3-ways", "16"), ("--cache-sim-omp-threads", "1"),
                            ("--cache-record-rss-mib", "2048"), ("--prefetcher", "none")):
            assert job.command[job.command.index(flag) + 1] == value
        labels.update(job.metadata["expected_policy_labels"])
        if options.grasp_reference != "off":
            assert job.metadata["policies"] == ["GRASP_PAPER"]
            diagnostics.append(job)
    assert labels == {"GRASP_PAPER", "POPT_UNCHARGED", "DIAG_GRASP_REFERENCE_FULL", "DIAG_GRASP_REFERENCE_FLAT"}
    assert len(diagnostics) == 2
    assert [len(a) for a in diagnostics[0].command] == [len(a) for a in diagnostics[1].command]
    assert {k: len(v) for k, v in diagnostics[0].metadata["env"].items()} == {
        k: len(v) for k, v in diagnostics[1].metadata["env"].items()}


@pytest.mark.parametrize("mode", ["flat", "full"])
def test_grasp_reference_runner_distinguishes_diagnostic_from_baselines(tmp_path, mode):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "spmv",
        "--options", f"--graph {graph} --repeat 2 --grasp-reference {mode}",
        "--policies", "GRASP_PAPER", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "1024B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    rows = json.loads((output / "roi_matrix.json").read_text())
    marker = json.loads((output / "roi_matrix.complete.json").read_text())
    assert len(rows) == 1 and rows[0]["policy_label"] == f"DIAG_GRASP_REFERENCE_{mode.upper()}"
    assert marker["policy_labels"] == marker["expected_policy_labels"] == [rows[0]["policy_label"]]
    assert rows[0]["popt_matrix_bytes"] == 8192 and rows[0]["popt_consumer"] == "GRASP-reference"
    assert rows[0]["grasp_reference_mode"] == mode and rows[0]["setup_cache_policy"] == "GRASP_PAPER"
    assert rows[0]["diagnostic_only"] == "1" and rows[0]["policy_ablation"] == ("1" if mode == "flat" else "0")
    assert csv_status(output / "roi_matrix.csv", ["GRASP_PAPER"], grasp_reference=mode)[0] == "ok"
    assert csv_status(output / "roi_matrix.csv", ["GRASP_PAPER"])[0] == "partial"
    other = "full" if mode == "flat" else "flat"
    assert csv_status(output / "roi_matrix.csv", ["GRASP_PAPER"], grasp_reference=other)[0] == "partial"


@pytest.mark.parametrize("width", [4, 8])
def test_frontier_mask_cost_matched_enabled_and_ignored(tmp_path, width):
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    for mode in ("transport", "replacement"):
        results = []
        for gating in ("enabled", "ignored"):
            ran = subprocess.run([
                "setarch", os.uname().machine, "-R", str(binary),
                "--algorithm", "bfs", "--graph", str(graph), "--mode", mode, "--delta", "8",
                "--record-model", "frontier", "--frontier-gating", gating,
                "--record-base-policy", "GRASP_PAPER", "--grasp-scope", "graph-passes",
                "--record-bytes", str(width), "--evidence", "--values",
                "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
                "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output),
            ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
                capture_output=True, text=True, timeout=30, check=False)
            assert ran.returncode == 0, ran.stdout + ran.stderr
            payload = json.loads(output.read_text())
            runtime = payload["frontier_runtime"]
            assert payload["window_runtime"] is None and payload["popt"] is None
            assert runtime["schema"] == "ecg.frontier-runtime.v1" and runtime["gating"] == gating
            assert runtime["candidate_floor"] == 7 and runtime["protected_overrides"] == 0
            assert runtime["counter_storage_bytes"] == 2048 and runtime["llc_bitmap_bytes"] == 32
            assert runtime["metadata_payload_bits_per_line"] == 67
            assert runtime["controller_object_bytes"] <= 4096
            assert runtime["pending"] == 0 and runtime["remaining_current"] == runtime["remaining_next"] == 0
            updates = 1 + runtime["frontier_appends"] + runtime["source_rows"]
            assert runtime["counter_read_bytes"] == 4 * (updates + runtime["passes"] * runtime["bins_per_pass"])
            assert runtime["counter_write_bytes"] == 2048 + 4 * updates
            assert runtime["counter_arithmetic_steps"] == updates
            assert runtime["counter_scan_steps"] == runtime["passes"] * runtime["bins_per_pass"]
            assert runtime["frontier_swaps"] == runtime["passes"]
            assert runtime["counter_initialization_memory_steps"] == 514
            assert runtime["memory_steps"] == payload["traffic_phases"]["kernel"]["total_accesses"] + 514
            assert payload["metrics"]["prefetch_fills"] == 0
            options = algorithm_matrix.parse_options(
                f"--graph {graph} --record-model frontier --frontier-gating {gating} "
                "--record-base-policy GRASP_PAPER --grasp-scope graph-passes")
            validation = dict(algorithm="bfs", mode=mode, policy="LRU",
                graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"), graph_path=graph,
                options=options, requested_bytes=width, minimum_mantissa_bits=0, evidence=True, llc_sets=8)
            algorithm_matrix.validate_payload(payload, ran.stdout, **validation)
            algorithm_matrix.validate_traffic_phases(payload)
            algorithm_matrix.validate_bfs_phases(payload, options)
            for field in ("counter_read_bytes", "counter_write_bytes", "counter_arithmetic_steps",
                          "counter_scan_steps", "frontier_clears", "frontier_swaps", "control_bytes",
                          "metadata_payload_bits_per_line"):
                forged = json.loads(json.dumps(payload))
                forged["frontier_runtime"][field] = 0
                with pytest.raises(RecordReceiptError):
                    algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
            forged = json.loads(json.dumps(payload))
            forged["frontier_runtime"]["gating"] = "ignored" if gating == "enabled" else "enabled"
            with pytest.raises(RecordReceiptError):
                algorithm_matrix.validate_payload(forged, ran.stdout, **validation)
            results.append(payload)
        enabled, ignored = results
        assert enabled["workload"] == ignored["workload"]
        assert enabled["traffic_phases"]["setup"] == ignored["traffic_phases"]["setup"]
        for key in ("record_loads", "record_read_bytes", "property_reads", "forwarded_stores",
                    "passes", "source_rows", "counter_read_bytes", "counter_write_bytes", "markers",
                    "marker_steps", "control_bytes", "counter_arithmetic_steps", "counter_scan_steps",
                    "frontier_swaps", "frontier_appends", "frontier_clears"):
            assert enabled["frontier_runtime"][key] == ignored["frontier_runtime"][key], key
        if mode == "transport":
            assert enabled["metrics"] == ignored["metrics"]
            assert enabled["frontier_runtime"]["victim_overrides"] == 0


@pytest.mark.parametrize("extra", [
    ["--algorithm", "spmv"], ["--mode", "csr"], ["--mode", "replacement-prefetch"],
    ["--record-base-policy", "LRU"], ["--grasp-scope", "all"], ["--bfs-direction", "do"],
    ["--window-observer", "window"], ["--window-candidate-rrpv", "7"],
    ["--record-preprocess", "traversal"], ["--minimum-mantissa-bits", "1"],
    ["--sources", "0,1"], ["--repeat", "2"], ["--frontier-gating", "unknown"],
    ["--record-model", "window"],
])
def test_frontier_model_rejects_unsupported_cli_before_loading(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(binary), "--algorithm", "bfs", "--graph", str(tmp_path / "not-read.sg"),
        "--mode", "replacement", "--record-model", "frontier", "--frontier-gating", "enabled",
        "--record-base-policy", "GRASP_PAPER", "--grasp-scope", "graph-passes", *extra,
    ], capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2 and "ECG-ALGORITHM-ERROR" in ran.stderr
    assert "not-read.sg" not in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_frontier_model_rejects_native_runners(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "bfs",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --record-model frontier",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "frontier model is cache_sim-only"


@pytest.mark.parametrize("source", [0, 9])
def test_frontier_model_preserves_bfs_answers_including_an_isolated_source(tmp_path, source):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "cliques-and-isolate.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    outputs = []
    for arguments in ([], ["--mode", "replacement", "--record-model", "frontier",
                          "--record-base-policy", "GRASP_PAPER", "--grasp-scope", "graph-passes"]):
        output = tmp_path / "same-result.json"
        ran = subprocess.run([
            str(binary), "--algorithm", "bfs", "--source", str(source), "--graph", str(graph),
            "--values", "--evidence", "--output", str(output), *arguments,
        ], capture_output=True, text=True, timeout=20, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        outputs.append(json.loads(output.read_text())["workload"])
    for field in ("values_u32", "result_digest", "work_trace_digest", "position_trace_digest",
                  "passes", "actual_records", "reached", "levels", "ordinary_property_reads",
                  "property_writes", "auxiliary_accesses"):
        assert outputs[0][field] == outputs[1][field], field


def test_frontier_profile_has_four_fixed_costed_cells(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg import algorithm_matrix
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(experiment_run.parse_args(
        ["--profile", "ecg_frontier_mask_cache", "--list"]), manifest, tmp_path)
    assert len(jobs) == 4 and sum(len(j.metadata["policies"]) for j in jobs) == 4
    gate = manifest["profile_controls"]["ecg_frontier_mask_cache"]["development_gate"]
    assert gate["minimum_margin"] == 0.02 and gate["remaining_full_graph_runs"] == 4
    assert gate["no_automatic_follow_on"] is True
    masks = []
    labels = set()
    for job in jobs:
        options = algorithm_matrix.parse_options(job.metadata["options"])
        assert job.metadata["benchmark"] == "bfs" and options.graph.name == "cit-Patents-dbg.sg"
        assert options.source == 0 and options.repeat == 1 and options.bfs_direction == "td"
        assert options.window_observer == "off" and options.popt_rank_mode == "future"
        assert job.metadata["l3_sizes"] == ["8MB"]
        assert job.command[job.command.index("--l3-ways") + 1] == "16"
        assert job.command[job.command.index("--cache-sim-omp-threads") + 1] == "1"
        assert job.command[job.command.index("--cache-record-rss-mib") + 1] == "2048"
        assert job.command[job.command.index("--prefetcher") + 1] == "none"
        labels.update(job.metadata["expected_policy_labels"])
        if options.record_model == "frontier":
            assert options.grasp_scope == "graph-passes" and options.record_base_policy == "GRASP_PAPER"
            assert options.bfs_traffic_phases == "off" and job.metadata["policies"] == ["ECG:replacement"]
            masks.append(job)
    assert labels == {"GRASP_PAPER_GRAPH_PASSES", "POPT_UNCHARGED",
        "ECG_REPLACEMENT_BASE_GRASP_PAPER_MODEL_FRONTIER_RRPV7_GATING_ENABLED_GRAPH_PASSES",
        "ECG_REPLACEMENT_BASE_GRASP_PAPER_MODEL_FRONTIER_RRPV7_GATING_IGNORED_GRAPH_PASSES"}
    assert len(masks) == 2
    assert [len(argument) for argument in masks[0].command] == [len(argument) for argument in masks[1].command]
    assert {k: len(v) for k, v in masks[0].metadata["env"].items()} == {
        k: len(v) for k, v in masks[1].metadata["env"].items()}


def test_frontier_resources_include_the_runtime_and_reject_weighted_graphs(tmp_path):
    from scripts.experiments.ecg.record_resources import graph_info, plan_algorithm_resources, RecordResourceError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    for name in ("pressure512.sg", "weighted-diamond.wsg"):
        (tmp_path / name).write_bytes(algorithm_outputs()[name][0])
    graph = graph_info(tmp_path / "pressure512.sg", allow_weighted=True, traversal="out")
    settings = dict(algorithm="bfs", records=True, requested_bytes=4, minimum_mantissa_bits=0,
                    traversals=1, sources=1, carrier_limit=32 << 20, auxiliary_limit=32 << 20,
                    rss_mib=512, record_model="frontier")
    budget = 12 * graph.vertices + 4 * graph.records + 8 * ((graph.vertices + 15) // 16) + 4096
    plan = plan_algorithm_resources(graph, workspace_limit=budget, **settings)
    assert plan["frontier_runtime_budget_bytes"] == 4096
    assert plan["construction_auxiliary_bytes_upper"] == 8 * ((graph.vertices + 15) // 16)
    assert plan["popt_matrix_bytes_upper"] == 0 and plan["record_bytes"] == 4
    with pytest.raises(RecordResourceError, match="explicit limits"):
        plan_algorithm_resources(graph, workspace_limit=budget - 1, **settings)
    weighted = graph_info(tmp_path / "weighted-diamond.wsg", allow_weighted=True, traversal="out")
    with pytest.raises(RecordResourceError, match="unweighted"):
        plan_algorithm_resources(weighted, workspace_limit=32 << 20, **settings)


@pytest.mark.parametrize("gating", ["enabled", "ignored"])
def test_frontier_runner_provenance_and_complete_costs(tmp_path, gating):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "bfs",
        "--options", f"--graph {graph} --record-model frontier --frontier-gating {gating} "
                     "--record-base-policy GRASP_PAPER --grasp-scope graph-passes",
        "--policies", "ECG:transport", "ECG:replacement", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "1024B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    rows = json.loads((output / "roi_matrix.json").read_text())
    completion = json.loads((output / "roi_matrix.complete.json").read_text())
    labels = sorted(row["policy_label"] for row in rows)
    assert sorted(completion["policy_labels"]) == sorted(completion["expected_policy_labels"]) == labels
    assert all(f"_GATING_{gating.upper()}_GRAPH_PASSES" in label for label in labels)
    assert all(row["frontier_gating"] == gating and row["frontier_counter_storage_bytes"] == 2048
               and row["policy_ablation"] == ("1" if gating == "ignored" else "0") for row in rows)
    arguments = dict(record_base_policy="GRASP_PAPER", record_model="frontier", grasp_scope="graph-passes")
    policies = ["ECG:transport", "ECG:replacement"]
    assert csv_status(output / "roi_matrix.csv", policies, **arguments, frontier_gating=gating)[0] == "ok"
    other = "ignored" if gating == "enabled" else "enabled"
    assert csv_status(output / "roi_matrix.csv", policies, **arguments, frontier_gating=other)[0] == "partial"


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
        assert observer["schema"] == "ecg.window-eviction-observer.v4"
        assert observer["publication_rule"] == "checked-read-store-new-event"
        assert observer["association_rejected"] == 0
        assert observer["protected_probe"]["candidate_floor"] == 6
        if mode == "window":
            assert observer["forwarded_store_updates"] == payload["workload"]["reached"] - 1
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
    forged = copy.deepcopy(b)
    forged["window_observer"]["preserved_delivered"]["live_base_without_alternative"] += 1
    with pytest.raises(RecordReceiptError, match="attribution"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["write_survival_rule"] = "allow-stale-updates"
    with pytest.raises(RecordReceiptError, match="write-survival"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["preserved_trials"]["base_first_before_both_endpoints"] += (
        forged["window_observer"]["preserved_trials"]["base_first"] + 1)
    with pytest.raises(RecordReceiptError, match="sampled endpoints"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["association_accepted"] += 1
    with pytest.raises(RecordReceiptError, match="paired-store"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["protected_probe"]["candidate_floor"] = 5
    with pytest.raises(RecordReceiptError, match="protected probe"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["protected_probe"]["retargeted_choices"] += 1
    with pytest.raises(RecordReceiptError, match="double-counts"):
        validate_window_observer(forged, options)
    forged = copy.deepcopy(b)
    forged["window_observer"]["association_rule"] = "same-cache-line-only"
    with pytest.raises(RecordReceiptError, match="paired-store"):
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


@pytest.mark.parametrize("scope", ["all", "graph-passes"])
def test_active_window_records_preserve_bfs_and_charge_controls(tmp_path, scope):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg import algorithm_matrix
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    results = []
    baseline_output = tmp_path / "csr-reference.json"
    baseline_run = subprocess.run([
        str(binary), "--algorithm", "bfs", "--graph", str(graph), "--mode", "csr",
        "--policy", "GRASP_PAPER", "--delta", "8", "--evidence", "--values",
        "--output", str(baseline_output),
    ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
        capture_output=True, text=True, timeout=30, check=False)
    assert baseline_run.returncode == 0, baseline_run.stdout + baseline_run.stderr
    baseline = json.loads(baseline_output.read_text())["workload"]
    for width in (4, 8):
        for mode, floor in (("transport", 6), ("replacement", 7), ("replacement", 6)):
            output = tmp_path / "window-result.json"
            ran = subprocess.run([
                "setarch", os.uname().machine, "-R", str(binary),
                "--algorithm", "bfs", "--graph", str(graph), "--mode", mode,
                "--delta", "8",
                "--record-model", "window", "--window-candidate-rrpv", str(floor),
                "--grasp-scope", scope,
                "--record-base-policy", "GRASP_PAPER", "--record-bytes", str(width),
                "--evidence", "--values", "--l1-bytes", "128", "--l1-ways", "2",
                "--l2-bytes", "256", "--l2-ways", "2", "--llc-bytes", "1024", "--llc-ways", "2",
                "--output", str(output),
            ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
                capture_output=True, text=True, timeout=30, check=False)
            assert ran.returncode == 0, ran.stdout + ran.stderr
            p = json.loads(output.read_text())
            options = algorithm_matrix.parse_options(
                f"--graph {graph} --record-model window --record-base-policy GRASP_PAPER --window-candidate-rrpv {floor} --grasp-scope {scope}")
            algorithm_matrix.validate_payload(
                p, ran.stdout + ran.stderr, algorithm="bfs", mode=mode, policy="LRU",
                graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"),
                graph_path=graph, options=options, requested_bytes=width,
                minimum_mantissa_bits=0, evidence=True, llc_sets=8)
            algorithm_matrix.validate_traffic_phases(p)
            algorithm_matrix.validate_bfs_phases(p, options)
            assert p["diagnostic_only"] is False and p["window_observer"] is None
            assert p["workload"]["record_model"] == "window"
            w = p["window_runtime"]
            assert w["grasp_scope"] == scope
            assert p["grasp_phase_control"] is None
            assert w["schema"] == "ecg.window-runtime.v1"
            assert w["record_loads"] == p["workload"]["actual_records"] == w["property_reads"]
            assert w["record_read_bytes"] == width * w["record_loads"]
            assert w["enqueued"] == w["delivered"] and w["pending"] == 0
            assert w["markers"] > 0 and w["marker_steps"] == 16 * w["markers"]
            assert w["observation_steps"] > 0 and w["row_context_steps"] > 0
            assert w["association_steps"] == w["forwarded_stores"] == p["workload"]["reached"] - 1
            assert w["control_bytes"] == 48 * (w["markers"] + 1)
            assert w["metadata_payload_bits_per_line"] == 67
            assert w["unmodeled_runtime_table_bytes"] == 0
            if mode == "transport":
                assert w["victim_overrides"] == 0
            if floor == 7:
                assert w["protected_overrides"] == 0
            results.append(p)
    for key in ("result_digest", "work_trace_digest", "position_trace_digest", "actual_records", "values_u32"):
        assert all(p["workload"][key] == baseline[key] for p in results)
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    broken = copy.deepcopy(results[-1])
    broken["window_runtime"]["marker_steps"] = 0
    with pytest.raises(RecordReceiptError, match="control/lookup"):
        algorithm_matrix.validate_window_runtime(broken, options)
    if scope == "graph-passes":
        broken = copy.deepcopy(results[-1])
        broken["grasp_phase_control"] = {"functional_steps": 560}
        with pytest.raises(RecordReceiptError, match="exactly once"):
            algorithm_matrix.validate_bfs_phases(broken, options)


def test_window_runtime_profile_and_resource_limits(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    from scripts.experiments.ecg.record_resources import GraphInfo, plan_algorithm_resources, RecordResourceError
    m = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(
        experiment_run.parse_args(["--profile", "ecg_window_costed_cache", "--list"]), m, tmp_path)
    assert len(jobs) == 3 and sum(len(job.metadata["policies"]) for job in jobs) == 6
    for job in jobs:
        options = parse_options(job.metadata["options"])
        assert job.metadata["benchmark"] == "bfs" and options.bfs_direction == "td"
        if options.record_model == "window":
            assert options.record_base_policy == "GRASP_PAPER"
            assert all(f"_MODEL_WINDOW_RRPV{options.window_candidate_rrpv}" in label
                       for label in job.metadata["expected_policy_labels"])
    graph = GraphInfo(False, 512, 3968, 495, 37913, "a" * 64)
    args = dict(algorithm="bfs", records=True, requested_bytes=4, minimum_mantissa_bits=0,
                traversals=1, sources=1, workspace_limit=1 << 20, carrier_limit=1 << 20,
                auxiliary_limit=1 << 20, rss_mib=1024, record_model="window")
    plan = plan_algorithm_resources(graph, **args)
    assert plan["construction_auxiliary_bytes_upper"] == 32 * 8
    assert plan["carrier_payload_bytes_upper"] == 3968 * 4
    with pytest.raises(RecordResourceError, match="window model"):
        plan_algorithm_resources(graph, **{**args, "backend": "gem5"})
    with pytest.raises(RecordResourceError, match="explicit limits"):
        plan_algorithm_resources(graph, **{**args, "auxiliary_limit": 255})


def test_phased_window_profile_preserves_stronger_controls(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    m = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(
        experiment_run.parse_args(["--profile", "ecg_window_phased_cache", "--list"]), m, tmp_path)
    assert len(jobs) == 4 and sum(len(job.metadata["policies"]) for job in jobs) == 5
    for job in jobs:
        options = parse_options(job.metadata["options"])
        assert job.metadata["benchmark"] == "bfs" and options.bfs_direction == "td"
        if job.metadata["policies"] == ["POPT:UNCHARGED"]:
            assert options.grasp_scope == "all" and options.record_model == "next"
        else:
            assert options.grasp_scope == "graph-passes"
            assert all(label.endswith("_GRAPH_PASSES") for label in job.metadata["expected_policy_labels"])
        if options.record_model == "window":
            assert options.bfs_traffic_phases == "off" and options.record_base_policy == "GRASP_PAPER"
            assert options.window_candidate_rrpv in (6, 7)


def test_active_window_runner_uses_distinct_model_labels(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "matrix"
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "bfs",
        "--options", f"--graph {graph} --record-model window --record-base-policy GRASP_PAPER --window-candidate-rrpv 6",
        "--policies", "ECG:transport", "ECG:replacement", "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "1kB", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    policies = ["ECG:transport", "ECG:replacement"]
    assert csv_status(output / "roi_matrix.csv", policies, "GRASP_PAPER", "off", "window", 6)[0] == "ok"
    assert csv_status(output / "roi_matrix.csv", policies, "GRASP_PAPER")[0] == "partial"
    marker = json.loads((output / "roi_matrix.complete.json").read_text())
    assert all("_MODEL_WINDOW_RRPV6" in label for label in marker["policy_labels"])
    rows = json.loads((output / "roi_matrix.json").read_text())
    assert all(row["window_unmodeled_runtime_table_bytes"] == 0 for row in rows)


def test_bfs_phase_attribution_preserves_work_and_cache_state(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.algorithm_matrix import parse_options, validate_bfs_phases
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-result.json"
    payloads = []
    for scope, attribution in (("all", "off"), ("all", "on"), ("graph-passes", "on")):
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary),
            "--algorithm", "bfs", "--graph", str(graph), "--policy", "GRASP_PAPER",
            "--grasp-scope", scope, "--bfs-traffic-phases", attribution,
            "--delta", "8", "--values", "--evidence", "--output", str(output),
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256",
            "--l2-ways", "2", "--llc-bytes", "1024", "--llc-ways", "2",
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
            capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        validate_bfs_phases(payload, parse_options(
            f"--graph {graph} --grasp-scope {scope} --bfs-traffic-phases {attribution}"))
        payloads.append(payload)
    plain, attributed, scoped = payloads
    assert plain["metrics"] == attributed["metrics"]
    assert plain["workload"] == attributed["workload"] == scoped["workload"]
    assert attributed["traffic_phases"]["setup"] == scoped["traffic_phases"]["setup"]
    assert scoped["grasp_phase_control"]["transitions"] == 2 * scoped["workload"]["passes"]
    assert scoped["grasp_phase_control"]["cache_reset"] is False
    assert scoped["grasp_phase_control"]["functional_steps"] == (
        16 * (scoped["grasp_phase_control"]["transitions"] + 1))
    for payload in (attributed, scoped):
        phases = payload["bfs_traffic_phases"]
        assert phases["writeback_attribution"] == "triggering-access-not-victim-owner"
        for key in ("total_accesses", "memory_accesses", "llc_writebacks", "total_offchip_traffic"):
            assert sum(phase["total"][key] for phase in phases["phases"]) == payload["metrics"][key]
        sorting = next(phase for phase in phases["phases"] if phase["phase"] == "frontier-sort")
        assert sorting["roles"]["frontier-work"]["total_accesses"] > 0
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    forged = copy.deepcopy(scoped)
    forged["grasp_phase_control"]["cache_reset"] = True
    with pytest.raises(RecordReceiptError, match="preservation"):
        validate_bfs_phases(forged, parse_options(f"--graph {graph} --grasp-scope graph-passes --bfs-traffic-phases on"))
    forged = copy.deepcopy(attributed)
    forged["bfs_traffic_phases"]["phases"][1]["roles"]["depth"]["memory_accesses"] += 1
    with pytest.raises(RecordReceiptError, match="do not close"):
        validate_bfs_phases(forged, parse_options(f"--graph {graph} --bfs-traffic-phases on"))


def test_bfs_phase_profile_keeps_popt_and_baseline_policy_intact(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    m = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    jobs = experiment_run.expand_jobs(
        experiment_run.parse_args(["--profile", "ecg_bfs_phase_cache", "--list"]), m, tmp_path)
    assert len(jobs) == 2 and sum(len(job.metadata["policies"]) for job in jobs) == 3
    for job in jobs:
        options = parse_options(job.metadata["options"])
        assert options.bfs_traffic_phases == "on" and options.bfs_direction == "td"
        if options.grasp_scope == "all":
            assert job.metadata["policies"] == ["GRASP_PAPER", "POPT:UNCHARGED"]
            assert job.metadata["expected_policy_labels"] == ["GRASP_PAPER", "POPT_UNCHARGED"]
        else:
            assert job.metadata["expected_policy_labels"] == ["GRASP_PAPER_GRAPH_PASSES"]


def test_popt_phase_attribution_is_observation_only(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.algorithm_matrix import parse_options, validate_bfs_phases
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph, output = tmp_path / "pressure512.sg", tmp_path / "same-result.json"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    results = []
    for enabled in ("off", "on"):
        ran = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary), "--algorithm", "bfs",
            "--graph", str(graph), "--policy", "POPT_UNCHARGED", "--bfs-traffic-phases", enabled,
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--values", "--evidence", "--output", str(output),
        ], env={**os.environ, "OMP_NUM_THREADS": "1", "GRAPHBREW_SIDEBAND_LOG": "0"},
            capture_output=True, text=True, timeout=30, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        validate_bfs_phases(payload, parse_options(f"--graph {graph} --bfs-traffic-phases {enabled}"))
        results.append(payload)
    for key in ("metrics", "workload", "traffic_phases", "popt"):
        assert results[0][key] == results[1][key], key


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_bfs_phase_controls_reject_native_paths(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "bfs",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --bfs-traffic-phases on",
        "--prefetcher", "none", "--flowthrough", "off",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("GRASP_PAPER"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "BFS phase controls are cache_sim-only"


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_window_model_is_rejected_before_native_graph_loading(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "bfs",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --record-model window --record-base-policy GRASP_PAPER",
        "--prefetcher", "none", "--flowthrough", "off",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB", backend, services)
    assert rows[0]["status"] == "error" and rows[0]["error"] == "window model is cache_sim-only"


@pytest.mark.parametrize("extra", [
    ["--algorithm", "sssp"], ["--record-base-policy", "LRU"], ["--mode", "replacement-prefetch"],
    ["--bfs-direction", "do"], ["--record-preprocess", "traversal"],
    ["--window-observer", "window"], ["--minimum-mantissa-bits", "1"],
    ["--window-candidate-rrpv", "5"],
])
def test_window_model_rejects_unsupported_options(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(binary), "--algorithm", "bfs", "--graph", str(tmp_path / "not-read.sg"),
        "--mode", "replacement", "--record-base-policy", "GRASP_PAPER",
        "--record-model", "window", *extra,
    ], capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2 and "window" in ran.stderr


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
