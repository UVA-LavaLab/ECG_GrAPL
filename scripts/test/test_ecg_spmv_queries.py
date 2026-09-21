"""Real cache-backend reuse, on tiny independent SpMV queries only."""

import json
import copy
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIELDS = ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks",
          "llc_hits", "llc_misses", "llc_property_hits", "llc_property_misses", "total_offchip_traffic")


@pytest.mark.parametrize("policy", ["LRU", "SRRIP", "GRASP_PAPER", "POPT_UNCHARGED"])
def test_spmv_queries_reuse_real_matrix_and_close_all_costs(tmp_path, policy):
    from scripts.experiments.ecg import algorithm_matrix, spmv_queries
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import serialized_graph
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "queries64.sg"
    graph.write_bytes(serialized_graph(
        64, [(0, 16), (16, 0), (0, 32), (32, 0), (16, 32), (32, 16)], False)[0])
    output = tmp_path / "queries.json"
    ran = subprocess.run([
        str(binary), "--algorithm", "spmv", "--graph", str(graph), "--policy", policy,
        "--repeat", "2", "--queries", "3", "--values", "--evidence", "--delta", "8",
        "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
        "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output),
    ], env={**os.environ, "OMP_NUM_THREADS": "1", "POPT_MATRIX_STREAM_SIM": "1"},
        capture_output=True, text=True, timeout=30, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    batch = json.loads(output.read_text())
    assert batch["schema"] == "ecg.spmv-queries.v1" and batch["query_count"] == 3
    assert batch["graph_storage_reused"] is True and batch["cache_state_preserved"] is True
    assert batch["properties"] == "fresh-query-local-arrays" and batch["timing_valid_for_speedup"] is False
    queries = batch["queries"]
    assert [q["query_index"] for q in queries] == [1, 2, 3]
    expected = [0] * 64
    expected[0], expected[16], expected[32] = 50, 34, 18
    previous = {field: 0 for field in FIELDS}
    for index, query in enumerate(queries):
        assert query["workload"] == queries[0]["workload"]
        assert query["workload"]["values_f32"] == expected
        assert query["workload"]["passes"] == 2 and query["workload"]["actual_records"] == 12
        assert query["workload"]["property_writes"] == 192
        for field in FIELDS:
            prep = query["graph_preparation"][field]
            setup = query["query_setup"][field]
            kernel = query["traffic_phases"]["kernel"][field]
            assert prep + setup == query["traffic_phases"]["setup"][field]
            assert previous[field] + prep + setup + kernel == query["cumulative_traffic"][field]
        previous = query["cumulative_traffic"]
        if policy == "POPT_UNCHARGED":
            matrix = query["popt"]
            assert matrix["reused"] is (index != 0) and matrix["construction_count"] == 1
            assert matrix["matrix_bytes"] == 1024
            assert matrix["matrix_digest"] == queries[0]["popt"]["matrix_digest"]
            assert matrix["passes"] == 2 and matrix["governed_reads"] == 12
            for field in ("construction_read_bytes", "construction_write_bytes"):
                assert (matrix[field] > 0) if index == 0 else (matrix[field] == 0)
        else:
            assert query["popt"] is None and all(query["graph_preparation"][f] == 0 for f in FIELDS)
    for field in FIELDS:
        assert batch["graph_preparation"][field] == sum(q["graph_preparation"][field] for q in queries)
        assert batch["query_setup"][field] == sum(q["query_setup"][field] for q in queries)
        assert batch["query_kernel"][field] == sum(q["traffic_phases"]["kernel"][field] for q in queries)
        assert batch["total_traffic"][field] == previous[field]
    assert batch["metrics"]["total_offchip_traffic"] == previous["total_offchip_traffic"]
    assert batch["matrix_constructions"] == int(policy == "POPT_UNCHARGED")
    options = algorithm_matrix.parse_options(f"--graph {graph} --repeat 2 --queries 3")
    validation = dict(graph=algorithm_matrix.graph_info(graph, allow_weighted=True, traversal="out"),
        options=options, policy=policy, evidence=True, llc_bytes=1024, llc_ways=2)
    row = spmv_queries.validate_batch(batch, ran.stdout, **validation)
    assert row["algorithm_actual_records"] == 36 and row["algorithm_passes"] == 6
    assert row["algorithm_property_writes"] == 576 and row["query_count"] == 3
    for key in ("query_count", "shared_matrix_bytes", "matrix_constructions"):
        forged = copy.deepcopy(batch)
        forged[key] += 1
        with pytest.raises(RecordReceiptError):
            spmv_queries.validate_batch(forged, ran.stdout, **validation)
    for key in ("query_index", "graph_preparation", "query_setup", "cumulative_traffic"):
        forged = copy.deepcopy(batch)
        if key == "query_index":
            forged["queries"][1][key] = 1
        else:
            forged["queries"][1][key]["total_accesses"] += 1
        with pytest.raises(RecordReceiptError):
            spmv_queries.validate_batch(forged, ran.stdout, **validation)
    if policy == "POPT_UNCHARGED":
        forged = copy.deepcopy(batch)
        forged["queries"][1]["popt"]["matrix_digest"] += 1
        with pytest.raises(RecordReceiptError):
            spmv_queries.validate_batch(forged, ran.stdout, **validation)
        forged = copy.deepcopy(batch)
        for field in FIELDS:
            moved = forged["queries"][0]["graph_preparation"][field]
            forged["queries"][0]["graph_preparation"][field] = 0
            forged["queries"][0]["query_setup"][field] += moved
            forged["graph_preparation"][field] = 0
            forged["query_setup"][field] += moved
        with pytest.raises(RecordReceiptError, match="one-time matrix work"):
            spmv_queries.validate_batch(forged, ran.stdout, **validation)
    (tmp_path / "validated-row.json").write_text(json.dumps(row, indent=2) + "\n")


@pytest.mark.parametrize("extra", [
    ["--queries", "0"], ["--queries", "65"], ["--algorithm", "bfs"],
    ["--mode", "transport"], ["--record-model", "frontier"], ["--record-model", "window"],
    ["--record-preprocess", "traversal"], ["--grasp-reference", "full"],
    ["--grasp-reference", "rank"],
    ["--policy", "POPT_UNCHARGED", "--popt-rank-mode", "constant"],
    ["--grasp-scope", "graph-passes"], ["--sources", "0,1"],
])
def test_spmv_queries_reject_unsupported_cli_before_loading(tmp_path, extra):
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([str(binary), "--algorithm", "spmv", "--policy", "GRASP_PAPER",
        "--graph", str(tmp_path / "not-read.sg"), "--queries", "2", *extra],
        capture_output=True, text=True, timeout=10, check=False)
    assert ran.returncode == 2 and "ECG-ALGORITHM-ERROR" in ran.stderr
    assert "not-read.sg" not in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_spmv_queries_reject_native_runner(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "spmv",
        "--options", f"--graph {tmp_path / 'not-read.sg'} --queries 2",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    row = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("LRU"), "8MB", backend, services)[0]
    assert row["status"] == "error" and row["error"] == "independent SpMV queries are cache_sim-only"
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg import algorithm_matrix
    options = algorithm_matrix.parse_options(args.options)
    with pytest.raises(RecordReceiptError, match="cache_sim-only"):
        algorithm_detailed.guest_options(args, options, {}, roi_matrix.parse_policy_spec("LRU"),
            roi_matrix.parse_size_bytes, "8MB", tmp_path / "not-written.json")


def test_spmv_queries_close_existing_runner_labels_and_provenance(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    from scripts.experiments.ecg.flows.experiment_run import csv_status
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "matrix"
    policies = ["LRU", "GRASP_PAPER", "POPT:UNCHARGED"]
    ran = subprocess.run([
        "python3", str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim", "--current-algorithms", "--benchmark", "spmv",
        "--options", f"--graph {graph} --repeat 2 --queries 2",
        "--policies", *policies, "--ecg-equivalence",
        "--algorithm-workspace-bytes", str(32 << 20), "--cache-record-rss-mib", "512",
        "--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
        "--l3-sizes", "1024B", "--l3-ways", "2", "--no-build", "--out-dir", str(output),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    rows = json.loads((output / "roi_matrix.json").read_text())
    marker = json.loads((output / "roi_matrix.complete.json").read_text())
    labels = sorted(row["policy_label"] for row in rows)
    assert labels == ["GRASP_PAPER_QUERIES2", "LRU_QUERIES2", "POPT_UNCHARGED_QUERIES2"]
    assert sorted(marker["policy_labels"]) == sorted(marker["expected_policy_labels"]) == labels
    assert all(row["status"] == "ok" and row["query_count"] == 2 and row["algorithm_passes"] == 4 for row in rows)
    assert all(row["traffic_ratio_vs_csr_lru"] > 0 for row in rows)
    assert csv_status(output / "roi_matrix.csv", policies, query_count=2)[0] == "ok"
    assert csv_status(output / "roi_matrix.csv", policies)[0] == "partial"
    assert csv_status(output / "roi_matrix.csv", policies, query_count=3)[0] == "partial"
    popt = next(row for row in rows if row["policy_label"].startswith("POPT_"))
    assert popt["matrix_constructions"] == 1 and popt["shared_matrix_bytes"] == 8192


def test_spmv_queries_reject_weighted_input(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "weighted-diamond.wsg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    run = subprocess.run([str(binary), "--algorithm", "spmv", "--graph", str(graph), "--queries", "2"],
                         capture_output=True, text=True, timeout=10, check=False)
    assert run.returncode == 2 and "unweighted CSR SpMV" in run.stderr


@pytest.mark.parametrize("policy", ["GRASP_PAPER", "POPT_UNCHARGED"])
def test_explicit_single_query_preserves_existing_output(tmp_path, policy):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    output = tmp_path / "same-output.json"
    controls = (["--repeat", "2"], ["--queries", "1"])
    padding = max(sum(len(value) for value in extra) for extra in controls)
    results = []
    for extra in controls:
        run = subprocess.run([
            "setarch", os.uname().machine, "-R", str(binary), "--algorithm", "spmv",
            "--graph", str(graph), "--policy", policy, "--repeat", "2", "--values", "--evidence",
            "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "1024", "--llc-ways", "2", "--output", str(output), *extra,
        ], capture_output=True, text=True, timeout=30, check=False,
            env={**os.environ, "OMP_NUM_THREADS": "1",
                 "GRAPHBREW_TEST_ARGV_PADDING": "x" * (padding - sum(len(v) for v in extra))})
        assert run.returncode == 0, run.stdout + run.stderr
        payload = json.loads(output.read_text())
        assert payload["schema"] == "ecg.algorithm-result.v1"
        results.append(payload)
    for key in ("workload", "metrics", "traffic_phases", "popt"):
        assert results[0][key] == results[1][key]


def test_spmv_query_orchestrator_keeps_count_in_expected_labels(tmp_path):
    from scripts.experiments.ecg.flows import experiment_run
    manifest = experiment_run.load_manifest(experiment_run.DEFAULT_MANIFEST)
    for stage in manifest["stages"]:
        if "ecg_grasp_reference_cache" in stage.get("profiles", []):
            if stage.get("algorithm_grasp_reference"):
                stage["profiles"] = []
            else:
                stage["algorithm_queries"] = 3
    jobs = experiment_run.expand_jobs(experiment_run.parse_args(
        ["--profile", "ecg_grasp_reference_cache", "--list"]), manifest, tmp_path)
    assert len(jobs) == 1 and jobs[0].metadata["query_count"] == 3
    assert jobs[0].metadata["expected_policy_labels"] == ["GRASP_PAPER_QUERIES3", "POPT_UNCHARGED_QUERIES3"]
    assert "--queries 3" in jobs[0].metadata["options"]
