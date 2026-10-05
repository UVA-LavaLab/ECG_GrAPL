"""NEXT.md §bw: charged P-OPT, the bar ECG must tie or beat, on the current algorithms.

The bar is P-OPT as published (Balaji et al., HPCA 2021): it ranks a line from the current epoch's
rereference-matrix column and the next one's, so its reserved last-level ways hold exactly that pair, sized
size-correct, and a dedicated engine streams a column from memory whenever the pair needs one it does not hold. The
engine never passes through the private caches or the data ways, so the kernel pays each column line once, as a
memory read that is no cache miss. A multi-pass kernel pays for every sweep, and a frontier that moves between
epochs pays for every pair it needs. POPT_UNCHARGED stays P-OPT's full-capacity upper bound.

The C++ fixture in bench/src_sim/test_ecg_algorithms.cc holds the stream mechanism; these hold the reservation's
one owner, what the binary writes and what the runner sends and accepts.
"""
from argparse import Namespace
from pathlib import Path
import copy
import json
import os
import platform
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
ALGORITHMS = ROOT / "bench/bin_sim/algorithms"
PAGERANK = ROOT / "bench/bin_sim/pr"
KERNELS = ("spmv", "bfs", "sssp", "bc")
CHARGE = ("--popt-reserve-model", "size_correct", "--popt-matrix-stream", "simulated")
GEOMETRY = ("--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2", "--l3-ways", "4")


def _clean_env():
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CACHE_", "ECG_", "GEM5_", "SNIPER_", "GRASP_", "POPT_"))}
    env.update(OMP_NUM_THREADS="1", GRAPHBREW_SIDEBAND_LOG="0")
    return env


def _graph(tmp_path, algorithm):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    name = "pressure512.wsg" if algorithm == "sssp" else "pressure512.sg"
    path = tmp_path / name
    if not path.exists():
        path.write_bytes(algorithm_outputs()[name][0])
    return path


def _lines(algorithm, graph):
    from scripts.experiments.ecg.record_resources import graph_info, popt_matrix_lines
    return popt_matrix_lines(algorithm, graph_info(graph, allow_weighted=True, traversal="out").vertices)


def _run(tmp_path, algorithm, policy, *extra, name):
    """One binary run, its layout held as two compared runs need (NEXT.md §bg): address randomisation off,
    equal-length output names, and the policy's length padded in the environment, since argv and the environment
    sit above the stack and setup's per-line accounting follows its alignment."""
    output = tmp_path / f"{algorithm}-{name}.json"
    env = dict(_clean_env(), LAYOUT_PAD="x" * (len("POPT_UNCHARGED") - len(policy)))
    ran = subprocess.run([
        "setarch", platform.machine(), "-R", str(ALGORITHMS), "--algorithm", algorithm,
        "--graph", str(_graph(tmp_path, algorithm)), "--delta", "2",
        "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
        "--llc-bytes", "2048", "--llc-ways", "4", "--output", str(output), "--mode", "csr", "--policy", policy, *extra,
    ], env=env, capture_output=True, text=True, timeout=120, check=False)
    assert ran.returncode == 0, ran.stderr[-2000:]
    return json.loads(output.read_text())


def _cell(tmp_path, algorithm, *extra, policy="POPT_CHARGED", l3="2048B", options=""):
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    out = tmp_path / f"{algorithm}-{policy}-{l3}"
    out.mkdir(exist_ok=True)
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", algorithm, "--current-algorithms",
        "--options", f"--graph {_graph(tmp_path, algorithm)} --delta 2 {options}".strip(),
        "--policies", policy, *GEOMETRY, "--l3-sizes", l3, *extra, "--out-dir", str(out), "--no-build"])
    return algorithm_matrix.run_cache_cell(args, out, roi_matrix.parse_policy_spec(policy), l3,
                                           roi_matrix.run_command, roi_matrix.parse_size_bytes)


@pytest.mark.parametrize("lines,llc,ways,reserved,fits", [
    (16_384, 512 << 10, 16, 1, True),      # the 2^18-vertex fixture at 512 kB
    (235_923, 8 << 20, 16, 1, True),       # cit-Patents at 8 MiB
    (3_848_651, 8 << 20, 16, 15, True),    # 61.6M vertices at 8 MiB: one data way remains
    (3_848_651, 32 << 20, 16, 4, True),    # ... and at 32 MiB
    (4_194_304, 8 << 20, 16, 15, False),   # the columns need sixteen ways: infeasible, clamped
])
def test_the_size_correct_reservation(lines, llc, ways, reserved, fits):
    from scripts.experiments.ecg.record_resources import popt_reservation
    charge = popt_reservation(lines, l3_bytes=llc, l3_ways=ways)
    assert (charge["reserved_ways"], charge["fits"]) == (reserved, fits)
    assert charge["matrix_bytes"] == 2 * lines
    assert charge["effective_ways"] == ways - reserved
    assert charge["effective_bytes"] == llc // ways * (ways - reserved)


@pytest.mark.parametrize("scale,llc", [(18, "512kB"), (26, "8MB"), (26, "32MB")])
def test_pagerank_reserves_through_the_same_owner(scale, llc):
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.record_resources import popt_reservation
    args = Namespace(options=f"-g {scale} -k 16 -o 5 -n 1 -i 2", line_size="64", l3_ways="16",
                     popt_property_bytes="4", popt_active_columns="2", popt_num_epochs="256",
                     popt_min_data_ways="1", popt_reserve_model="size_correct")
    charge = roi_matrix.popt_charge_metadata(args, roi_matrix.parse_policy_spec("POPT_CHARGED"), llc)
    shared = popt_reservation(charge["popt_matrix_column_bytes"], l3_bytes=roi_matrix.parse_size_bytes(llc),
                              l3_ways=16)
    assert charge["popt_reserved_ways"] == shared["reserved_ways"]
    assert int(charge["popt_effective_l3_ways"]) == shared["effective_ways"]
    assert charge["popt_matrix_fits"] == int(shared["fits"])
    assert charge["popt_reserved_bytes"] == shared["reserved_ways"] * shared["bytes_per_way"]


@pytest.mark.parametrize("algorithm", KERNELS)
def test_charged_popt_streams_its_columns_and_says_so(tmp_path, algorithm):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    free = _run(tmp_path, algorithm, "POPT_UNCHARGED", name="u")
    paid = _run(tmp_path, algorithm, "POPT", name="c")
    assert free["popt"]["full_data_capacity"] is True and free["popt"]["runtime_matrix_traffic_charged"] is False
    assert "stream" not in free["popt"]
    assert paid["policy"] == "POPT" and paid["setup_cache_policy"] == "LRU"
    assert paid["popt"]["full_data_capacity"] is False and paid["popt"]["runtime_matrix_traffic_charged"] is True
    column = _lines(algorithm, _graph(tmp_path, algorithm))
    stream = paid["popt"]["stream"]
    assert stream["columns"] > 0
    assert stream == {"model": "dedicated-current-next", "active_columns": 2, "column_bytes": column,
                      "columns": stream["columns"], "lines": stream["columns"] * -(-column // 64)}
    # The engine streams beside the processor: setup, every processor access, every cache decision and the answer
    # are P-OPT's as before, and the kernel pays each column line once, as a memory read that is no cache miss.
    assert paid["traffic_phases"]["setup"] == free["traffic_phases"]["setup"]
    kernel, before = paid["traffic_phases"]["kernel"], free["traffic_phases"]["kernel"]
    assert kernel["total_accesses"] == before["total_accesses"]
    assert {key: kernel[key] for key in ("llc_hits", "llc_misses", "llc_writebacks")} == \
        {key: before[key] for key in ("llc_hits", "llc_misses", "llc_writebacks")}
    assert kernel["memory_accesses"] == before["memory_accesses"] + stream["lines"]
    for receipt, lines in ((free, 0), (paid, stream["lines"])):
        assert receipt["metrics"]["memory_accesses"] == receipt["metrics"]["L3"]["misses"] + lines
    assert paid["workload"]["result_digest"] == free["workload"]["result_digest"]


def test_a_multi_pass_kernel_pays_for_every_sweep(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    one = _run(tmp_path, "spmv", "POPT", "--repeat", "1", name="1")
    two = _run(tmp_path, "spmv", "POPT", "--repeat", "2", name="2")
    assert two["popt"]["stream"]["columns"] == 2 * one["popt"]["stream"]["columns"] > 0


@pytest.mark.parametrize("extra,message", [
    (("--queries", "2"), "independent queries require unweighted CSR SpMV baselines"),
    (("--popt-rank-mode", "constant"), "constant P-OPT ranks require CSR SpMV or TD BFS with POPT_UNCHARGED"),
    (("--mode", "replacement"), "current-record-modes-own-their-replacement-policy"),
])
def test_charged_popt_is_refused_where_it_has_no_model(tmp_path, extra, message):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = subprocess.run([
        str(ALGORITHMS), "--algorithm", "spmv", "--graph", str(_graph(tmp_path, "spmv")),
        "--output", str(tmp_path / "refused.json"), "--policy", "POPT", *extra,
    ], env=_clean_env(), capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode != 0 and message in ran.stderr, ran.stderr[-1500:]


@pytest.mark.parametrize("algorithm", KERNELS)
def test_the_runner_reserves_the_columns_and_sends_the_remaining_ways(tmp_path, algorithm):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    from scripts.experiments.ecg.record_resources import popt_reservation
    rows = _cell(tmp_path, algorithm, *CHARGE)
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    row = rows[0]
    column = _lines(algorithm, _graph(tmp_path, algorithm))
    charge = popt_reservation(column, l3_bytes=2048, l3_ways=4)
    assert charge["fits"] and charge["reserved_ways"] == 1
    assert row["policy_label"] == "POPT"
    assert (row["popt_overhead_charged"], row["popt_reserve_model"], row["popt_matrix_stream_mode"]) == (
        1, "size_correct", "simulated")
    assert (row["popt_reserved_ways"], row["popt_effective_l3_ways"], row["popt_effective_l3_bytes"]) == (
        charge["reserved_ways"], charge["effective_ways"], charge["effective_bytes"])
    payload = json.loads(Path(row["json_path"]).read_text())
    assert payload["metrics"]["L3"]["ways"] == charge["effective_ways"]
    assert payload["metrics"]["L3"]["size_bytes"] == charge["effective_bytes"]
    assert row["popt_stream_lines"] == payload["popt"]["stream"]["lines"] > 0
    assert row["popt_stream_columns"] == payload["popt"]["stream"]["columns"]


def test_the_runner_runs_the_bar_under_the_fair_contracts(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    rows = _cell(tmp_path, "bc", *CHARGE, options="--grasp-registration declared --kernel-entry cold")
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    assert rows[0]["policy_label"] == "POPT_GRASP_DECLARED_COLD_ENTRY"
    payload = json.loads(Path(rows[0]["json_path"]).read_text())
    assert payload["kernel_entry"] == "cold" and payload["popt"]["stream"]["columns"] > 0


@pytest.mark.parametrize("extra", [
    (),
    ("--popt-reserve-model", "size_correct"),
    ("--popt-matrix-stream", "simulated"),
    (*CHARGE, "--popt-active-columns", "1"),
    (*CHARGE, "--popt-num-epochs", "128"),
])
def test_the_runner_admits_one_charge_model(tmp_path, extra):
    rows = _cell(tmp_path, "spmv", *extra)
    assert rows[0]["status"] == "error"
    assert "charged P-OPT on the current algorithms is size-correct" in rows[0]["error"]


def test_the_runner_refuses_columns_the_last_level_cannot_hold(tmp_path):
    from scripts.experiments.ecg.record_resources import popt_reservation
    column = _lines("bc", _graph(tmp_path, "bc"))
    assert not popt_reservation(column, l3_bytes=256, l3_ways=4)["fits"]
    rows = _cell(tmp_path, "bc", *CHARGE, l3="256B")
    assert rows[0]["status"] == "error"
    assert "cannot hold its two resident columns" in rows[0]["error"]


def test_the_validator_refuses_a_charge_that_departs(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    column = _lines("spmv", _graph(tmp_path, "spmv"))
    paid = _run(tmp_path, "spmv", "POPT", name="c")
    free = _run(tmp_path, "spmv", "POPT_UNCHARGED", name="u")
    assert algorithm_matrix.validate_popt_charge(paid, policy="POPT", matrix_lines=column) == {
        "popt_stream_columns": paid["popt"]["stream"]["columns"],
        "popt_stream_lines": paid["popt"]["stream"]["lines"]}
    assert algorithm_matrix.validate_popt_charge(free, policy="POPT_UNCHARGED", matrix_lines=column) == {}
    lines = paid["popt"]["stream"]["lines"]
    departures = [
        ("full_data_capacity", True), ("runtime_matrix_traffic_charged", False), ("stream", None),
        ("stream.model", "analytic"), ("stream.model", "simulated-residency"), ("stream.active_columns", 1),
        ("stream.column_bytes", column + 1), ("stream.columns", 0), ("stream.lines", lines + 1),
    ]
    for key, value in departures:
        changed = copy.deepcopy(paid)
        holder = changed["popt"]["stream"] if key.startswith("stream.") else changed["popt"]
        name = key.split(".")[-1]
        if value is None:
            del holder[name]
        else:
            holder[name] = value
        with pytest.raises(RecordReceiptError):
            algorithm_matrix.validate_popt_charge(changed, policy="POPT", matrix_lines=column)
            pytest.fail(f"accepted {key}={value}")
    # The stream is paid once, as memory reads beside the last level's own misses.
    for receipt, policy in ((paid, "POPT"), (free, "POPT_UNCHARGED")):
        for delta in (-1, 1):
            changed = copy.deepcopy(receipt)
            changed["metrics"]["memory_accesses"] += delta
            with pytest.raises(RecordReceiptError):
                algorithm_matrix.validate_popt_charge(changed, policy=policy, matrix_lines=column)
                pytest.fail(f"accepted {policy} memory reads off by {delta}")
    for key, value in (("full_data_capacity", False), ("runtime_matrix_traffic_charged", True),
                       ("stream", paid["popt"]["stream"])):
        changed = copy.deepcopy(free)
        changed["popt"][key] = value
        with pytest.raises(RecordReceiptError):
            algorithm_matrix.validate_popt_charge(changed, policy="POPT_UNCHARGED", matrix_lines=column)
    l3 = {"size_bytes": 1536, "ways": 3}
    algorithm_matrix.validate_popt_geometry(l3, llc_bytes=1536, llc_ways=3)
    for departed in ({"size_bytes": 2048, "ways": 3}, {"size_bytes": 1536, "ways": 4}):
        with pytest.raises(RecordReceiptError):
            algorithm_matrix.validate_popt_geometry(departed, llc_bytes=1536, llc_ways=3)


def test_pagerank_runs_the_bar_under_the_fair_contracts(tmp_path):
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.record_resources import popt_reservation
    graph = _graph(tmp_path, "spmv")
    out = tmp_path / "pr"
    out.mkdir()
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
        "--grasp-registration", "declared", "--kernel-entry", "cold",
        "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0", "--policies", "POPT_CHARGED", *CHARGE,
        *GEOMETRY, "--l3-sizes", "2048B", "--out-dir", str(out), "--no-build"])
    rows = roi_matrix.run_cache_sim(args, out, roi_matrix.parse_policy_spec("POPT_CHARGED"), "2048B")
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    row = rows[0]
    charge = popt_reservation(-(-512 * 4 // 64), l3_bytes=2048, l3_ways=4)
    assert row["popt_reserve_model"] == "size_correct" and row["popt_matrix_stream_mode"] == "simulated"
    assert row["popt_matrix_stream_model"] == "dedicated-current-next"
    assert int(row["popt_effective_l3_ways"]) == charge["effective_ways"]
    lines = int(row["popt_matrix_stream_lines_simulated"])
    assert lines > 0
    # The engine's reads are in the memory traffic once and in no last-level count.
    assert int(row["total_memory_traffic"]) == int(row["l3_misses"]) + lines
    assert int(row["l3_misses_with_overhead"]) == int(row["l3_misses"]) + lines
    assert int(row["total_memory_traffic_with_overhead"]) == int(row["total_memory_traffic"])
    assert row["policy_label"] == "POPT_GRASP_DECLARED_COLD_ENTRY"


def _pagerank_receipt(tmp_path, name, **settings):
    output = tmp_path / f"pr-{name}.json"
    env = dict(_clean_env(), CACHE_ULTRAFAST="0", CACHE_FAST="0", ECG_CURRENT_PR_BASELINE="1",
               CACHE_L1_POLICY="LRU", CACHE_L2_POLICY="LRU", CACHE_L1_SIZE="1KB", CACHE_L2_SIZE="2KB",
               CACHE_L3_SIZE="4KB", CACHE_L3_WAYS="16", CACHE_OUTPUT_JSON=str(output), **settings)
    ran = subprocess.run([str(PAGERANK), "-g", "10", "-k", "8", "-o", "5", "-n", "1", "-i", "2", "-t", "0"],
                         cwd=tmp_path, env=env, capture_output=True, text=True, timeout=120, check=False)
    assert ran.returncode == 0, ran.stderr[-2000:]
    return json.loads(output.read_text())


def _coverage(registration):
    return {region["name"]: region["popt"] for region in registration["property_regions"]}


@pytest.mark.parametrize("settings", [{}, {"POPT_SE_POSTFINAL": "later_lower_bound"}])
def test_pagerank_popt_ranks_only_the_contributions(tmp_path, settings):
    """The authors' PageRank registers outgoing_contrib as IRREGDATA and scores as REGDATA (NEXT.md §bw)."""
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    from scripts.experiments.ecg import roi_matrix
    receipt = _pagerank_receipt(tmp_path, "popt", CACHE_POLICY="POPT", CACHE_L3_POLICY="POPT", **settings)
    assert _coverage(receipt["property_registration"]) == {"scores": False, "contribution": True}
    roi_matrix.validate_pagerank_popt_coverage(receipt)
    grasp = _pagerank_receipt(tmp_path, "grasp", CACHE_POLICY="GRASP", CACHE_L3_POLICY="GRASP",
                              GRASP_BOUNDARY_MODE="capacity", GRASP_HOT_FRACTION="0.50")
    assert _coverage(grasp["property_registration"]) == {"scores": False, "contribution": False}
    for name in ("scores", "contribution"):
        changed = copy.deepcopy(receipt)
        for region in changed["property_registration"]["property_regions"]:
            if region["name"] == name:
                region["popt"] = not region["popt"]
        # roi_matrix imports record_receipts by module name: catch the class it raises.
        with pytest.raises(roi_matrix.RecordReceiptError):
            roi_matrix.validate_pagerank_popt_coverage(changed)
            pytest.fail(f"accepted P-OPT coverage with {name} flipped")


@pytest.mark.parametrize("policy,refused", [("POPT_CHARGED", True), ("GRASP_PAPER", False)])
def test_the_runner_checks_pagerank_popt_coverage(tmp_path, monkeypatch, policy, refused):
    """Every PageRank P-OPT row passes the coverage check, and no other row is held to it."""
    if not PAGERANK.is_file():
        pytest.skip("functional PageRank binary is not built")
    from scripts.experiments.ecg import roi_matrix

    def departed(data):
        raise roi_matrix.RecordReceiptError("a scores line is ranked")

    monkeypatch.setattr(roi_matrix, "validate_pagerank_popt_coverage", departed)
    out = tmp_path / "pr"
    out.mkdir()
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
        "--options", f"-f {_graph(tmp_path, 'spmv')} -o 0 -n 1 -i 2 -t 0", "--policies", policy, *CHARGE,
        *GEOMETRY, "--l3-sizes", "2048B", "--out-dir", str(out), "--no-build"])
    rows = roi_matrix.run_cache_sim(args, out, roi_matrix.parse_policy_spec(policy), "2048B")
    assert len(rows) == 1
    if refused:
        assert rows[0]["status"] == "error"
        assert "PageRank P-OPT coverage receipt failed: a scores line is ranked" in rows[0]["error"]
    else:
        assert rows[0]["status"] == "ok", rows[0].get("error")


def test_the_algorithms_receipt_names_the_popt_coverage(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    for policy, name in (("POPT", "c"), ("POPT_UNCHARGED", "u")):
        receipt = _run(tmp_path, "spmv", policy, name=name)
        assert _coverage(receipt["metrics"]["property_registration"]) == {"x": True, "y": False}


_NATIVE_MAIN = """
#include "ecg_algorithm_main.h"
int main(int argc, char** argv) {
    bool invoked = false;
    const int status = ecg_algorithm::applicationMain(argc, argv,
        [&](const auto&, const auto&) { invoked = true; return 0; });
    return invoked ? 9 : status;
}
"""


def test_a_native_main_refuses_charged_popt(tmp_path):
    source = tmp_path / "native_main.cc"
    source.write_text(_NATIVE_MAIN)
    binary = tmp_path / "native_main"
    built = subprocess.run([
        "g++", "-std=c++17", "-O1", "-fopenmp", "-I", str(ROOT / "bench/include/external/gapbs"),
        "-I", str(ROOT / "bench/include"), str(source), "-o", str(binary),
    ], capture_output=True, text=True, timeout=300, check=False)
    assert built.returncode == 0, built.stderr[-3000:]
    ran = subprocess.run([str(binary), "--algorithm", "spmv", "--graph", str(tmp_path / "absent.sg"),
                          "--policy", "POPT"], capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 2 and "current P-OPT is cache_sim-only" in ran.stderr, (ran.returncode, ran.stderr)
