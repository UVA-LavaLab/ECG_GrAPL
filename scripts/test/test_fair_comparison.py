"""NEXT.md §bv: the fair comparison's three contracts, at the binaries, their receipts and the runner.

C1, ecg.grasp-declaration.v1: each kernel phase designates the arrays upstream GRASP protects (propertyA and, for
BC, depth as propertyB) at its declared fraction of the last level, and every other property array stays a
property region but not a GRASP region. C2, the cold kernel entry: every row starts its kernel with empty caches
after a maintenance step charged to setup. C3, PageRank's transport control over GRASP_PAPER's victim policy, and
last-level counts per named property region. Each is opt-in, so every recorded result keeps its meaning.

The C++ fixture in bench/src_sim/test_ecg_algorithms.cc holds the mechanisms; these hold what the binaries write
and what the runner sends and accepts.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
ALGORITHMS = ROOT / "bench/bin_sim/algorithms"
PAGERANK = ROOT / "bench/bin_sim/pr"
CONTRACT = "ecg.grasp-declaration.v1"
# The frozen declarations (NEXT.md §bv): region, role, percent of the last level, per phase.
DECLARED = {
    "spmv": [("x", "A", 100)],
    "bfs": [("depth", "A", 100)],
    "sssp": [("distances", "A", 100)],
    "bc": [("path_counts", "A", 50), ("depth", "B", 50), ("dependency", "A", 50), ("depth", "B", 50)],
}


def _clean_env():
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CACHE_", "ECG_", "GEM5_", "SNIPER_", "GRASP_", "POPT_"))}
    env.update(OMP_NUM_THREADS="1", GRAPHBREW_SIDEBAND_LOG="0")
    return env


def _corpus(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    config = json.loads((ROOT / "scripts/experiments/ecg/configs/algorithm_equivalence.json").read_text())
    for filename, (data, _) in algorithm_outputs().items():
        (tmp_path / filename).write_bytes(data)
    return {algorithm: tmp_path / expected["graph"] for algorithm, expected in config["references"].items()}


def _run_algorithm(tmp_path, algorithm, graph, *extra, name="run"):
    output = tmp_path / f"{algorithm}-{name}.json"
    ran = subprocess.run([
        str(ALGORITHMS), "--algorithm", algorithm, "--graph", str(graph), "--delta", "2",
        "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
        "--llc-bytes", "512", "--llc-ways", "2", "--output", str(output), *extra,
    ], env=_clean_env(), capture_output=True, text=True, timeout=60, check=False)
    return ran, (json.loads(output.read_text()) if ran.returncode == 0 else None)


def _last_phase(declarations):
    phase = []
    for region, role, _ in declarations:
        if role == "A":
            phase = []
        phase.append(region)
    return set(phase)


@pytest.mark.parametrize("algorithm", sorted(DECLARED))
@pytest.mark.parametrize("mode", ["csr", "replacement"])
def test_algorithm_receipts_attest_the_declared_registration_and_the_cold_entry(tmp_path, algorithm, mode):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = _corpus(tmp_path)[algorithm]
    policy = ["--mode", "csr", "--policy", "GRASP_PAPER"] if mode == "csr" else \
        ["--mode", "replacement", "--record-base-policy", "GRASP_PAPER"]
    ran, historical = _run_algorithm(tmp_path, algorithm, graph, *policy, name="historical")
    assert ran.returncode == 0, ran.stderr[-2000:]
    ran, fair = _run_algorithm(tmp_path, algorithm, graph, *policy, "--grasp-registration", "declared",
                               "--kernel-entry", "cold", name="fair")
    assert ran.returncode == 0, ran.stderr[-2000:]

    # The historical receipt says so: every property array a GRASP region, the kernel entered as built.
    assert historical["grasp_registration"] == "all" and historical["kernel_entry"] == "as-built"
    assert historical["traffic_phases"]["cache_state_preserved"] is True
    captured = historical["metrics"]["property_registration"]
    assert captured["grasp_registration"] == "all" and captured["grasp_declarations"] == []
    assert captured["property_regions"] and all(region["grasp"] for region in captured["property_regions"])
    assert historical["metrics"]["kernel_entry"] == "as-built"

    # The fair receipt: the frozen declarations, only the last phase's arrays designated, a cold boundary.
    assert fair["grasp_registration"] == "declared" and fair["kernel_entry"] == "cold"
    phases = fair["traffic_phases"]
    assert phases["boundary"] == "first-binding-complete" and phases["cache_state_preserved"] is False
    for key in ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks", "total_offchip_traffic"):
        assert phases["setup"][key] + phases["kernel"][key] == fair["metrics"][key]
    metrics = fair["metrics"]
    assert metrics["kernel_entry"] == "cold"
    assert isinstance(metrics["kernel_entry_maintenance_writebacks"], int)
    assert phases["setup"]["llc_writebacks"] >= metrics["kernel_entry_maintenance_writebacks"]
    captured = metrics["property_registration"]
    assert captured["grasp_registration"] == CONTRACT
    declarations = [(item["region"], item["role"], item["percent"]) for item in captured["grasp_declarations"]]
    assert declarations == DECLARED[algorithm]
    designated = {region["name"] for region in captured["property_regions"] if region["grasp"]}
    assert designated == _last_phase(DECLARED[algorithm])
    regions = captured["property_regions"]
    assert sum(region["kernel_misses"] for region in regions) == phases["kernel"]["llc_property_misses"]
    assert sum(region["kernel_hits"] for region in regions) == phases["kernel"]["llc_property_hits"]
    # The kernels' answers do not depend on either contract.
    assert fair["workload"]["result_digest"] == historical["workload"]["result_digest"]
    # The runner's census check: the kernel is exactly what the census divides,
    # so no maintenance write-back can fall inside it.
    from scripts.experiments.ecg.record_receipts import validate_kernel_census
    for payload in (historical, fair):
        validate_kernel_census(payload["metrics"]["kernel_census"], payload["traffic_phases"]["kernel"])
    assert captured["boundary_mode"] == "capacity"
    from scripts.experiments.ecg import algorithm_matrix
    algorithm_matrix.validate_fair_comparison(historical, algorithm, "all", "as-built")
    algorithm_matrix.validate_fair_comparison(fair, algorithm, "declared", "cold")


@pytest.mark.parametrize("extra,message", [
    (("--grasp-registration", "some"), "grasp-registration-must-be-all-or-declared"),
    (("--kernel-entry", "warm"), "kernel-entry-must-be-as-built-or-cold"),
])
def test_algorithm_cli_refuses_unknown_contract_values(tmp_path, extra, message):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = _corpus(tmp_path)["spmv"]
    ran, _ = _run_algorithm(tmp_path, "spmv", graph, "--mode", "csr", "--policy", "GRASP_PAPER", *extra)
    assert ran.returncode != 0 and message in ran.stderr


@pytest.mark.parametrize("algorithm,extra,message", [
    ("cc", ("--mode", "csr", "--policy", "GRASP_PAPER", "--grasp-registration", "declared"),
     "undeclared-grasp-region"),
    ("bfs", ("--mode", "csr", "--policy", "GRASP_PAPER", "--bfs-direction", "do",
             "--grasp-registration", "declared"), "declared-grasp-has-no-direction-optimizing-bfs"),
    ("spmv", ("--mode", "prefetch", "--kernel-entry", "cold"), "cold-kernel-entry-requires-no-pending-prefetch"),
])
def test_algorithm_runs_refuse_what_the_contracts_cannot_honour(tmp_path, algorithm, extra, message):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = _corpus(tmp_path)[algorithm]
    ran, _ = _run_algorithm(tmp_path, algorithm, graph, *extra)
    assert ran.returncode != 0 and message in ran.stderr, ran.stderr[-2000:]


def _pagerank(tmp_path, name, current=True, **settings):
    output = tmp_path / f"pr-{name}.json"
    env = _clean_env()
    env.update({
        "CACHE_ULTRAFAST": "0", "CACHE_FAST": "0", **({"ECG_CURRENT_PR_BASELINE": "1"} if current else {}),
        "CACHE_L1_POLICY": "LRU", "CACHE_L2_POLICY": "LRU", "CACHE_L1_SIZE": "1KB",
        "CACHE_L2_SIZE": "2KB", "CACHE_L3_SIZE": "4KB", "CACHE_L3_WAYS": "16",
        "CACHE_OUTPUT_JSON": str(output),
    })
    env.update(settings)
    ran = subprocess.run([str(PAGERANK), "-g", "10", "-k", "8", "-o", "5", "-n", "1", "-i", "3", "-t", "0"],
                         cwd=tmp_path, env=env, capture_output=True, text=True, timeout=120, check=False)
    text = ran.stdout + ran.stderr
    checksum = re.search(r"score_checksum=([0-9a-f]+)", text)
    return ran, (json.loads(output.read_text()) if ran.returncode == 0 else None), \
        (checksum.group(1) if checksum else None)


GRASP_PAPER_PR = {"CACHE_POLICY": "GRASP", "CACHE_L3_POLICY": "GRASP",
                  "GRASP_BOUNDARY_MODE": "capacity", "GRASP_HOT_FRACTION": "0.50"}


def test_pagerank_declares_contribution_and_enters_cold(tmp_path):
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    ran, historical, expected = _pagerank(tmp_path, "historical", **GRASP_PAPER_PR)
    assert ran.returncode == 0, ran.stderr[-2000:]
    ran, fair, checksum = _pagerank(tmp_path, "fair", **GRASP_PAPER_PR,
                                    ECG_GRASP_REGISTRATION="declared", ECG_KERNEL_ENTRY="cold")
    assert ran.returncode == 0, ran.stderr[-2000:]
    assert checksum == expected, "the contracts change no score"
    old = historical["property_registration"]
    assert old["grasp_registration"] == "all" and historical["kernel_entry"] == "as-built"
    assert {region["name"]: region["grasp"] for region in old["property_regions"]} == \
        {"scores": True, "contribution": True}
    new = fair["property_registration"]
    assert new["grasp_registration"] == CONTRACT and fair["kernel_entry"] == "cold"
    assert [(item["region"], item["role"], item["percent"]) for item in new["grasp_declarations"]] == \
        [("contribution", "A", 50)]
    assert {region["name"]: region["grasp"] for region in new["property_regions"]} == \
        {"scores": False, "contribution": True}
    census = fair["kernel_census"]
    assert census["entry_valid_lines"] == 0 and census["entry_dirty_lines"] == 0
    # As the runner checks a PageRank row: the statistics reset at the kernel
    # boundary, so the receipt's totals are the kernel and the census divides them.
    from scripts.experiments.ecg.record_receipts import validate_kernel_census
    llc = fair["L3"]
    kernel = {key: fair[key] for key in ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks")}
    kernel.update(llc_hits=llc["hits"], llc_misses=llc["misses"],
                  llc_property_hits=llc["prop_hits"], llc_property_misses=llc["prop_misses"])
    validate_kernel_census(census, kernel)
    from scripts.experiments.ecg import algorithm_matrix
    algorithm_matrix.validate_fair_comparison(fair, "pr", "declared", "cold")
    assert new["boundary_mode"] == "capacity"
    untiered = {key: value for key, value in GRASP_PAPER_PR.items() if key != "GRASP_BOUNDARY_MODE"}
    ran, unset, _ = _pagerank(tmp_path, "unset", **untiered, ECG_GRASP_REGISTRATION="declared")
    assert ran.returncode == 0, ran.stderr[-2000:]
    assert unset["property_registration"]["boundary_mode"] == "capacity", \
        "the declared contract tiers by capacity whether or not the job says so"
    # PageRankOpt's fraction is the contract's, not the job's.
    ran, _, _ = _pagerank(tmp_path, "conflict", **{**GRASP_PAPER_PR, "GRASP_HOT_FRACTION": "0.15"},
                          ECG_GRASP_REGISTRATION="declared")
    assert ran.returncode != 0 and "gives contribution half the last level" in ran.stderr
    ran, _, _ = _pagerank(tmp_path, "unknown", **GRASP_PAPER_PR, ECG_KERNEL_ENTRY="warm")
    assert ran.returncode != 0 and "ECG_KERNEL_ENTRY is not a supported choice" in ran.stderr


def test_pagerank_transport_control_runs_grasp_papers_victim_policy(tmp_path):
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    record = {"ECG_RECORD_MECHANISM": "transport", "ECG_RECORD_BYTES": "4"}
    ran, lru, expected = _pagerank(tmp_path, "lru", **record, CACHE_POLICY="LRU", CACHE_L3_POLICY="LRU")
    assert ran.returncode == 0, ran.stderr[-2000:]
    ran, grasp, checksum = _pagerank(tmp_path, "grasp", **record, **GRASP_PAPER_PR,
                                     ECG_RECORD_BASE_POLICY="GRASP_PAPER")
    assert ran.returncode == 0, ran.stderr[-2000:]
    assert checksum == expected
    assert lru["record_base_policy"] == "LRU" and grasp["record_base_policy"] == "GRASP_PAPER"
    assert grasp["total_offchip_traffic"] != lru["total_offchip_traffic"], \
        "the GRASP base decides victims the LRU base does not"
    ran, _, _ = _pagerank(tmp_path, "replacement", **{**record, "ECG_RECORD_MECHANISM": "replacement"},
                          CACHE_POLICY="ECG", CACHE_L3_POLICY="ECG", ECG_RECORD_BASE_POLICY="GRASP_PAPER")
    assert ran.returncode != 0 and "is PageRank's transport control" in ran.stderr
    ran, _, _ = _pagerank(tmp_path, "untiered", **record, CACHE_POLICY="GRASP", CACHE_L3_POLICY="GRASP",
                          ECG_RECORD_BASE_POLICY="GRASP_PAPER")
    assert ran.returncode != 0 and "requires GRASP_HOT_FRACTION and GRASP_BOUNDARY_MODE" in ran.stderr


def _pagerank_args(tmp_path, *extra, policies="ECG:transport,GRASP_PAPER"):
    from scripts.experiments.ecg import roi_matrix
    return roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
        "--options", "-f graph.sg -o 0 -n 1 -i 2 -t 0", "--policies", policies,
        "--l3-sizes", "8MB", "--out-dir", str(tmp_path), "--no-build", *extra])


def test_runner_labels_and_environment_carry_the_pagerank_contracts(tmp_path, monkeypatch):
    from scripts.experiments.ecg import roi_matrix
    monkeypatch.setenv("ECG_GRASP_REGISTRATION", "declared")
    monkeypatch.setenv("ECG_KERNEL_ENTRY", "cold")
    monkeypatch.setenv("ECG_RECORD_BASE_POLICY", "GRASP_PAPER")
    specs = [roi_matrix.parse_policy_spec("ECG:transport"), roi_matrix.parse_policy_spec("GRASP_PAPER")]
    historical = _pagerank_args(tmp_path)
    assert roi_matrix.output_policy_labels(historical, specs) == ["ECG_TRANSPORT", "GRASP_PAPER"]
    for spec in specs:
        env = roi_matrix.cache_sim_env(historical, spec, "8MB", "16", tmp_path / "row.json")
        assert not {"ECG_GRASP_REGISTRATION", "ECG_KERNEL_ENTRY", "ECG_RECORD_BASE_POLICY"} & set(env), \
            "an ambient contract never reaches a row that did not ask for it"
    fair = _pagerank_args(tmp_path, "--grasp-registration", "declared", "--kernel-entry", "cold",
                          "--record-base-policy", "GRASP_PAPER")
    assert roi_matrix.output_policy_labels(fair, specs) == [
        "ECG_TRANSPORT_BASE_GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY", "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"]
    transport = roi_matrix.cache_sim_env(fair, specs[0], "8MB", "16", tmp_path / "row.json")
    assert transport["CACHE_L3_POLICY"] == "GRASP" and transport["ECG_RECORD_BASE_POLICY"] == "GRASP_PAPER"
    assert transport["GRASP_BOUNDARY_MODE"] == "capacity" and transport["GRASP_HOT_FRACTION"] == "0.50"
    baseline = roi_matrix.cache_sim_env(fair, specs[1], "8MB", "16", tmp_path / "row.json")
    assert "ECG_RECORD_BASE_POLICY" not in baseline
    for env in (transport, baseline):
        assert env["ECG_GRASP_REGISTRATION"] == "declared" and env["ECG_KERNEL_ENTRY"] == "cold"


@pytest.mark.parametrize("change", [
    ["--suite", "gem5"],
    ["--benchmark", "bfs"],
])
def test_runner_refuses_the_pagerank_contracts_where_nothing_honours_them(tmp_path, change):
    from scripts.experiments.ecg import roi_matrix
    argv = ["--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
            "--options", "-f graph.sg -o 0 -n 1 -i 2 -t 0", "--policies", "GRASP_PAPER",
            "--l3-sizes", "8MB", "--out-dir", str(tmp_path), "--no-build", "--dry-run",
            "--grasp-registration", "declared"]
    argv[argv.index(change[0]) + 1] = change[1]
    with pytest.raises(SystemExit) as refused:
        roi_matrix.main(argv)
    assert "cache_sim PageRank runner options" in str(refused.value.code)


# The guests of gem5 and Sniper call the shared main with its defaults, which
# allow neither contract; this is that call, before any graph is read.
_NATIVE_MAIN = """
#include <sstream>
#include "ecg_algorithm_main.h"
int main(int argc, char** argv) {
    bool invoked = false;
    const int status = ecg_algorithm::applicationMain(argc, argv,
        [&](const auto&, const auto&) { invoked = true; return 0; });
    return invoked ? 9 : status;
}
"""


def test_a_native_main_refuses_the_contracts_at_its_command_line(tmp_path):
    source = tmp_path / "native_main.cc"
    source.write_text(_NATIVE_MAIN)
    binary = tmp_path / "native_main"
    built = subprocess.run([
        "g++", "-std=c++17", "-O1", "-fopenmp", "-I", str(ROOT / "bench/include/external/gapbs"),
        "-I", str(ROOT / "bench/include"), str(source), "-o", str(binary),
    ], capture_output=True, text=True, timeout=300, check=False)
    assert built.returncode == 0, built.stderr[-3000:]
    for option, value in (("--grasp-registration", "declared"), ("--kernel-entry", "cold")):
        ran = subprocess.run([str(binary), "--algorithm", "spmv", "--graph", str(tmp_path / "absent.sg"),
                              option, value], capture_output=True, text=True, timeout=60, check=False)
        assert ran.returncode == 2, (ran.returncode, ran.stderr)
        assert "GRASP declarations and the cold kernel entry are cache_sim-only" in ran.stderr


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
@pytest.mark.parametrize("option", ["--grasp-registration declared", "--kernel-entry cold"])
def test_algorithm_runner_refuses_the_contracts_on_native_backends(tmp_path, backend, option):
    from types import SimpleNamespace
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    args = roi_matrix.parse_args([
        "--suite", backend, "--current-algorithms", "--ecg-equivalence", "--benchmark", "spmv",
        "--options", f"--graph {tmp_path / 'not-read.sg'} {option}",
    ])
    services = SimpleNamespace(parse_size_bytes=roi_matrix.parse_size_bytes)
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("GRASP_PAPER"), "8MB", backend, services)
    assert rows[0]["status"] == "error"
    assert rows[0]["error"] == "GRASP declarations and the cold kernel entry are cache_sim-only"


def test_algorithm_runner_sends_validates_and_labels_the_contracts(tmp_path):
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    options = algorithm_matrix.parse_options(
        f"--graph {graph} --grasp-registration declared --kernel-entry cold")
    assert options.grasp_registration == "declared" and options.kernel_entry == "cold"
    specs = [roi_matrix.parse_policy_spec("GRASP_PAPER"), roi_matrix.parse_policy_spec("ECG:replacement")]
    assert algorithm_matrix.policy_labels(specs, "GRASP_PAPER") == [
        "GRASP_PAPER", "ECG_REPLACEMENT_BASE_GRASP_PAPER"]
    assert algorithm_matrix.policy_labels(specs, "GRASP_PAPER", grasp_registration="declared",
                                          kernel_entry="cold") == [
        "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY", "ECG_REPLACEMENT_BASE_GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"]
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
        "--options", f"--graph {graph} --repeat 2 --grasp-registration declared --kernel-entry cold",
        "--policies", "GRASP_PAPER", "--l3-sizes", "8MB", "--l3-ways", "16",
        "--out-dir", str(tmp_path), "--no-build"])
    captured = []

    def run_command(command, *rest, **kw):
        captured.append([str(part) for part in command])
        raise RuntimeError("stop after the argv is built")

    try:
        algorithm_matrix.run_cache_cell(args, tmp_path, specs[0], "8MB", run_command, roi_matrix.parse_size_bytes)
    except Exception:
        pass
    assert captured, "run_cache_cell never reached the kernel command"
    command = captured[0]
    assert command[command.index("--grasp-registration") + 1] == "declared"
    assert command[command.index("--kernel-entry") + 1] == "cold"

    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    pagerank = {"kernel_entry": "as-built", "record_base_policy": "LRU",
                "property_registration": {"grasp_registration": "all", "grasp_declarations": []}}
    algorithm_matrix.validate_fair_comparison(pagerank, "pr", "all", "as-built", record_base="LRU")
    with pytest.raises(RecordReceiptError):
        algorithm_matrix.validate_fair_comparison(pagerank, "pr", "all", "as-built", record_base="GRASP_PAPER")
    counters = {"total_accesses": 10, "memory_accesses": 4, "prefetch_fills": 0, "llc_writebacks": 2,
                "total_offchip_traffic": 6}
    for entry, preserved in (("cold", False), ("as-built", True)):
        algorithm_matrix.validate_traffic_phases({
            "kernel_entry": entry, "metrics": {key: 2 * value for key, value in counters.items()},
            "traffic_phases": {"boundary": "first-binding-complete", "cache_state_preserved": preserved,
                               "setup": counters, "kernel": counters}})
        with pytest.raises(RecordReceiptError, match="cold kernel entry must report|nonintrusive"):
            algorithm_matrix.validate_traffic_phases({
                "kernel_entry": entry, "metrics": {key: 2 * value for key, value in counters.items()},
                "traffic_phases": {"boundary": "first-binding-complete", "cache_state_preserved": not preserved,
                                   "setup": counters, "kernel": counters}})


@pytest.mark.parametrize("variable,value", [
    ("ECG_GRASP_REGISTRATION", "declared"), ("ECG_KERNEL_ENTRY", "cold"), ("ECG_RECORD_BASE_POLICY", "GRASP_PAPER")])
def test_pagerank_off_the_current_path_refuses_the_contracts(tmp_path, variable, value):
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    ran, _, _ = _pagerank(tmp_path, "legacy", current=False, **GRASP_PAPER_PR, **{variable: value})
    assert ran.returncode != 0 and "require the current PageRank path" in ran.stderr, ran.stderr[-1500:]


def test_runner_refuses_pagerank_contracts_off_the_current_path(tmp_path):
    from scripts.experiments.ecg import roi_matrix
    with pytest.raises(SystemExit) as refused:
        roi_matrix.main(["--suite", "cache-sim", "--benchmark", "pr",
                         "--options", "-f graph.sg -o 0 -n 1 -i 2 -t 0", "--policies", "GRASP_PAPER",
                         "--l3-sizes", "8MB", "--out-dir", str(tmp_path), "--no-build", "--dry-run",
                         "--kernel-entry", "cold"])
    assert "require --current-pr-baselines" in str(refused.value.code)


_GEOMETRY = ("--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
             "--l3-sizes", "2048B", "--l3-ways", "4")


def test_executed_rows_carry_the_contract_labels(tmp_path):
    """The label a row is written under is the label the completion roster expects."""
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    if not ALGORITHMS.is_file() or not PAGERANK.is_file():
        pytest.skip("current executables are not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    spec = roi_matrix.parse_policy_spec("GRASP_PAPER")
    out = tmp_path / "spmv"
    out.mkdir()
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
        "--options", f"--graph {graph} --repeat 2 --grasp-registration declared --kernel-entry cold",
        "--policies", "GRASP_PAPER", *_GEOMETRY, "--out-dir", str(out), "--no-build"])
    rows = algorithm_matrix.run_cache_cell(args, out, spec, "2048B", roi_matrix.run_command,
                                           roi_matrix.parse_size_bytes)
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    assert rows[0]["policy_label"] == roi_matrix.output_policy_labels(args, [spec])[0] == \
        "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"
    out = tmp_path / "pr"
    out.mkdir()
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
        "--grasp-registration", "declared", "--kernel-entry", "cold",
        "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0", "--policies", "GRASP_PAPER",
        *_GEOMETRY, "--out-dir", str(out), "--no-build"])
    rows = roi_matrix.run_cache_sim(args, out, spec, "2048B")
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    assert rows[0]["policy_label"] == roi_matrix.output_policy_labels(args, [spec])[0] == \
        "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"


def test_pagerank_transport_control_on_the_grasp_base_passes_the_runner(tmp_path):
    """C3's control runs GRASP_PAPER's victim policy in the last level, so the runner expects what it sent.

    Found by NEXT.md §bw's fixture pass: the row was refused for realizing GRASP where LRU was expected, and the
    refusal then failed its record group.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines", "--record-base-policy", "GRASP_PAPER",
        "--grasp-registration", "declared", "--kernel-entry", "cold",
        "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0", "--policies", "ECG:transport",
        *_GEOMETRY, "--out-dir", str(tmp_path), "--no-build"])
    rows = roi_matrix.run_cache_sim(args, tmp_path, roi_matrix.parse_policy_spec("ECG:transport"), "2048B")
    roi_matrix.certify_current_record_results(rows)
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    row = rows[0]
    assert row["policy_label"] == "ECG_TRANSPORT_BASE_GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"
    assert row["cache_runtime_policy_valid"] == 1 and row["ecg_current_matched_control"] == 1
    payload = json.loads(Path(row["json_path"]).read_text())
    assert payload["L3"]["policy"].upper() == "GRASP" and payload["record_base_policy"] == "GRASP_PAPER"


def test_bc_declares_each_phase_of_every_source(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    from scripts.experiments.ecg import algorithm_matrix
    graph = _corpus(tmp_path)["bc"]
    ran, receipt = _run_algorithm(tmp_path, "bc", graph, "--mode", "csr", "--policy", "GRASP_PAPER",
                                  "--sources", "0,1", "--grasp-registration", "declared", name="sources")
    assert ran.returncode == 0, ran.stderr[-2000:]
    declarations = [(item["region"], item["role"], item["percent"], item["selections"])
                    for item in receipt["metrics"]["property_registration"]["grasp_declarations"]]
    assert declarations == [
        ("path_counts", "A", 50, 0), ("depth", "B", 50, 0), ("dependency", "A", 50, 1), ("depth", "B", 50, 1),
        ("path_counts", "A", 50, 2), ("depth", "B", 50, 2), ("dependency", "A", 50, 3), ("depth", "B", 50, 3)]
    algorithm_matrix.validate_fair_comparison(receipt, "bc", "declared", "as-built", sources=2)
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    with pytest.raises(RecordReceiptError):
        algorithm_matrix.validate_fair_comparison(receipt, "bc", "declared", "as-built", sources=1)


@pytest.mark.parametrize("algorithm", ["spmv", "bc"])
def test_the_declared_registration_changes_nothing_for_popt(tmp_path, algorithm):
    """P-OPT reads property regions, never GRASP designations, so the contract leaves its traffic as it was.

    The cache model sees real addresses, so both runs hold the layout fixed (NEXT.md §bg): address randomisation
    off, output paths of equal length, since the path is a heap string allocated before the graph is read, and the
    registration value's length padded in the environment, since argv and the environment sit above the stack.
    """
    import platform
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    graph = _corpus(tmp_path)[algorithm]
    receipts = {}
    for name, registration in (("h", "all"), ("d", "declared")):
        output = tmp_path / f"popt-{name}.json"
        ran = subprocess.run([
            "setarch", platform.machine(), "-R", str(ALGORITHMS), "--algorithm", algorithm, "--graph", str(graph),
            "--delta", "2", "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
            "--llc-bytes", "512", "--llc-ways", "2", "--output", str(output), "--mode", "csr",
            "--policy", "POPT_UNCHARGED", "--grasp-registration", registration,
        ], env=dict(_clean_env(), LAYOUT_PAD="x" * (len("declared") - len(registration))),
            capture_output=True, text=True, timeout=60, check=False)
        assert ran.returncode == 0, ran.stderr[-2000:]
        receipts[name] = json.loads(output.read_text())
    historical, declared = receipts["h"], receipts["d"]
    assert declared["metrics"]["property_registration"]["grasp_registration"] == CONTRACT
    # P-OPT rows tier nothing; the receipt still says how GRASP would have, and only the contract changes it.
    assert historical["metrics"]["property_registration"]["boundary_mode"] == "vertex"
    assert declared["metrics"]["property_registration"]["boundary_mode"] == "capacity"
    assert declared["traffic_phases"] == historical["traffic_phases"]
    assert declared["popt"] == historical["popt"]
    assert declared["workload"]["result_digest"] == historical["workload"]["result_digest"]


def test_the_validator_refuses_every_departure_from_the_frozen_contract(tmp_path):
    import copy
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    graph = _corpus(tmp_path)["bc"]
    ran, receipt = _run_algorithm(tmp_path, "bc", graph, "--mode", "csr", "--policy", "GRASP_PAPER",
                                  "--grasp-registration", "declared", "--kernel-entry", "cold", name="validator")
    assert ran.returncode == 0, ran.stderr[-2000:]
    algorithm_matrix.validate_fair_comparison(receipt, "bc", "declared", "cold")
    metrics = lambda payload: payload["metrics"]
    registration = lambda payload: payload["metrics"]["property_registration"]

    def edit(change):
        broken = copy.deepcopy(receipt)
        change(broken)
        return broken

    def designate(payload, name, value):
        for region in registration(payload)["property_regions"]:
            if region["name"] == name:
                region["grasp"] = value

    departures = {
        "another array declared": lambda r: registration(r)["grasp_declarations"][0].update(region="scores"),
        "another fraction": lambda r: registration(r)["grasp_declarations"][0].update(percent=100),
        "another role": lambda r: registration(r)["grasp_declarations"][1].update(role="A"),
        "another moment": lambda r: registration(r)["grasp_declarations"][2].update(selections=0),
        "a phase missing": lambda r: registration(r)["grasp_declarations"].pop(),
        "a stray designation": lambda r: designate(r, "scores", True),
        "a missing designation": lambda r: designate(r, "dependency", False),
        "array-relative tiers": lambda r: registration(r).update(boundary_mode="vertex"),
        "region counts that do not close": lambda r: registration(r)["property_regions"][0].update(
            kernel_misses=registration(r)["property_regions"][0]["kernel_misses"] + 1),
        "two cold boundaries": lambda r: metrics(r).update(kernel_entry_cold_boundaries=2),
        "a line left in L2": lambda r: metrics(r).update(kernel_entry_residual_lines=[0, 1, 0]),
        "a warm census": lambda r: metrics(r)["kernel_census"].update(entry_valid_lines=1),
        "negative maintenance": lambda r: metrics(r).update(kernel_entry_maintenance_writebacks=-1),
        "maintenance outside setup": lambda r: r["traffic_phases"]["setup"].update(
            llc_writebacks=metrics(r)["kernel_entry_maintenance_writebacks"] - 1),
        # Receipts that disown their contract while its mechanics stay intact.
        "a result that disowns the declaration": lambda r: r.update(grasp_registration="all"),
        "metrics that disown the cold entry": lambda r: metrics(r).update(kernel_entry="as-built"),
    }
    for name, change in departures.items():
        with pytest.raises(RecordReceiptError):
            algorithm_matrix.validate_fair_comparison(edit(change), "bc", "declared", "cold")
            pytest.fail(f"accepted: {name}")


def test_a_pagerank_row_whose_receipt_disowns_its_contract_is_an_error(tmp_path, monkeypatch):
    """A binary that ignored the kernel entry it was given writes an as-built receipt; the row must not pass."""
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    real = roi_matrix.run_command

    def ignoring(command, cwd, env, *rest, **kw):
        return real(command, cwd, {key: value for key, value in env.items() if key != "ECG_KERNEL_ENTRY"},
                    *rest, **kw)

    monkeypatch.setattr(roi_matrix, "run_command", ignoring)
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines", "--kernel-entry", "cold",
        "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0", "--policies", "GRASP_PAPER",
        *_GEOMETRY, "--out-dir", str(tmp_path), "--no-build"])
    rows = roi_matrix.run_cache_sim(args, tmp_path, roi_matrix.parse_policy_spec("GRASP_PAPER"), "2048B")
    assert len(rows) == 1 and rows[0]["status"] == "error"
    assert "PageRank contract receipt failed" in rows[0]["error"]
