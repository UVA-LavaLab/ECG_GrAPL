"""Current shared algorithms agree with independent, nontrivial graph answers."""

from pathlib import Path
import importlib.util
import copy
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
