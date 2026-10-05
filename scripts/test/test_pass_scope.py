"""NEXT.md §by: pass-scoped priority on the current algorithms.

While no graph pass is open, the last level is a plain three-bit SRRIP: a fill inserts at RRPV 6, a hit sets 0 and
the victim is the GRASP scan, with no GRASP tier and no record rule. Inside a pass nothing changes. The scope serves
the declared GRASP base, as the CSR baseline (Gs) or under the record transport and replacement rows (Ts, CFs), on
BFS, SSSP and BC without prefetch, in cache_sim only; unscoped rows are unchanged.

The C++ fixtures in bench/src_sim/test_ecg_record_cache.cc and test_ecg_algorithms.cc hold the mechanism; these hold
what the binary writes and refuses and what the runner sends, labels and accepts.
"""
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
REFUSAL = "pass-scope-requires-declared-GRASP-on-BFS-SSSP-or-BC-without-prefetch"
FAIR = "--grasp-registration declared --kernel-entry cold"
GEOMETRY = ("--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2", "--l3-ways", "4")
ROWS = {  # role: (policy spec, extra options)
    "Gs": ("GRASP_PAPER", ""),
    "Ts": ("ECG:transport", "--record-base-policy GRASP_PAPER"),
    "CFs": ("ECG:replacement", "--record-base-policy GRASP_PAPER --record-rrpv-order on --record-carrier-first on"),
}


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


def _cell(tmp_path, algorithm, role, options, l3="2048B"):
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    policy, extra = ROWS[role]
    out = tmp_path / f"{algorithm}-{role}-{len(options)}"
    out.mkdir(exist_ok=True)
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", algorithm, "--current-algorithms",
        "--options", f"--graph {_graph(tmp_path, algorithm)} --delta 2 {extra} {options}".strip(),
        "--policies", policy, *GEOMETRY, "--l3-sizes", l3, "--out-dir", str(out), "--no-build"])
    return algorithm_matrix.run_cache_cell(args, out, roi_matrix.parse_policy_spec(policy), l3,
                                           roi_matrix.run_command, roi_matrix.parse_size_bytes)


def _binary(tmp_path, algorithm, *extra):
    output = tmp_path / f"{algorithm}-binary.json"
    return subprocess.run([
        "setarch", platform.machine(), "-R", str(ALGORITHMS), "--algorithm", algorithm,
        "--graph", str(_graph(tmp_path, algorithm)), "--delta", "2",
        "--l1-bytes", "128", "--l1-ways", "2", "--l2-bytes", "256", "--l2-ways", "2",
        "--llc-bytes", "2048", "--llc-ways", "4", "--output", str(output), *extra,
    ], env=_clean_env(), capture_output=True, text=True, timeout=120, check=False)


def test_the_label_names_the_scope_outside_every_other_suffix():
    from scripts.experiments.ecg.algorithm_matrix import policy_labels
    from scripts.experiments.ecg.roi_matrix import parse_policy_spec
    fair = dict(grasp_registration="declared", kernel_entry="cold")
    assert policy_labels([parse_policy_spec("GRASP_PAPER")], pass_scope="srrip", **fair) == [
        "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY_PASS_SCOPED"]
    assert policy_labels([parse_policy_spec("ECG:transport")], "GRASP_PAPER", pass_scope="srrip", **fair) == [
        "ECG_TRANSPORT_BASE_GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY_PASS_SCOPED"]
    assert policy_labels([parse_policy_spec("ECG:replacement")], "GRASP_PAPER", rrpv_order="on",
                         carrier_first="on", pass_scope="srrip", **fair) == [
        "ECG_REPLACEMENT_BASE_GRASP_PAPER_CARRIER_FIRST_RRPV_ORDER_GRASP_DECLARED_COLD_ENTRY_PASS_SCOPED"]
    # Unscoped labels are unchanged.
    assert policy_labels([parse_policy_spec("GRASP_PAPER")], **fair) == ["GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"]


@pytest.mark.parametrize("spec,base,declared", [
    ("POPT:UNCHARGED", "LRU", "declared"),
    ("POPT_CHARGED", "LRU", "declared"),
    ("LRU", "LRU", "declared"),
    ("ECG:transport", "LRU", "declared"),
    ("GRASP_PAPER", "LRU", "all"),
])
def test_the_label_refuses_a_scope_without_the_declared_grasp_base(spec, base, declared):
    from scripts.experiments.ecg.algorithm_matrix import policy_labels
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    from scripts.experiments.ecg.roi_matrix import parse_policy_spec
    with pytest.raises(RecordReceiptError, match="pass scope"):
        policy_labels([parse_policy_spec(spec)], base, pass_scope="srrip", grasp_registration=declared)


def test_the_option_is_parsed_and_refused_beside_another_scope():
    from scripts.experiments.ecg.algorithm_matrix import parse_options
    from scripts.experiments.ecg.record_resources import RecordResourceError
    assert parse_options("--graph g.sg").pass_scope == "off"
    assert parse_options(f"--graph g.sg --pass-scope srrip {FAIR}").pass_scope == "srrip"
    for extra in ("--grasp-scope graph-passes --grasp-registration declared", "--kernel-entry cold",
                  "--record-model window --grasp-registration declared"):
        with pytest.raises(RecordResourceError, match="pass scope"):
            parse_options(f"--graph g.sg --pass-scope srrip {extra}")
    with pytest.raises(SystemExit):
        parse_options("--graph g.sg --pass-scope lru")


@pytest.mark.parametrize("algorithm", ("bfs", "sssp", "bc"))
@pytest.mark.parametrize("role", tuple(ROWS))
def test_scoped_rows_pass_the_runner_and_say_so(tmp_path, algorithm, role):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    rows = _cell(tmp_path, algorithm, role, f"{FAIR} --pass-scope srrip")
    assert len(rows) == 1 and rows[0]["status"] == "ok", rows[0].get("error")
    row = rows[0]
    assert row["policy_label"].endswith("_GRASP_DECLARED_COLD_ENTRY_PASS_SCOPED") and row["pass_scope"] == "srrip"
    assert "_PASS_SCOPED" in Path(row["json_path"]).name
    payload = json.loads(Path(row["json_path"]).read_text())
    control = payload["pass_scope_control"]
    assert payload["pass_scope"] == "srrip" and control["schema"] == "ecg.pass-scope.v1"
    assert control["signal"] == "graph-pass-markers" and control["outside_policy"] == "srrip-3bit-insert6-hit0"
    assert control["passes"] == payload["workload"]["passes"] > 0
    assert control["transitions"] == 2 * control["passes"]
    assert control["per_line_bits"] == 0 and control["cache_reset"] is False
    assert control["setup"]["fills"] > 0 and control["kernel"]["fills"] > 0
    # The same cell unscoped keeps its old label and reports no scope.
    plain = _cell(tmp_path, algorithm, role, FAIR)[0]
    assert plain["status"] == "ok" and not plain["policy_label"].endswith("_PASS_SCOPED")
    assert "pass_scope" not in plain
    unscoped = json.loads(Path(plain["json_path"]).read_text())
    assert unscoped["pass_scope"] == "off" and unscoped["pass_scope_control"] is None


def test_the_validator_refuses_a_scope_that_departs(tmp_path):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_receipts import RecordReceiptError
    scoped_row = _cell(tmp_path, "bfs", "Gs", f"{FAIR} --pass-scope srrip")[0]
    plain_row = _cell(tmp_path, "bfs", "Gs", FAIR)[0]
    assert scoped_row["status"] == plain_row["status"] == "ok"
    graph = algorithm_matrix.graph_info(_graph(tmp_path, "bfs"), allow_weighted=True, traversal="out")
    common = dict(algorithm="bfs", mode="csr", policy="GRASP_PAPER", graph=graph, graph_path=_graph(tmp_path, "bfs"),
                  requested_bytes=0, minimum_mantissa_bits=0, evidence=False, llc_sets=8)
    scoped_options = algorithm_matrix.parse_options(f"--graph {_graph(tmp_path, 'bfs')} --delta 2 {FAIR} --pass-scope srrip")
    plain_options = algorithm_matrix.parse_options(f"--graph {_graph(tmp_path, 'bfs')} --delta 2 {FAIR}")
    scoped = json.loads(Path(scoped_row["json_path"]).read_text())
    plain = json.loads(Path(plain_row["json_path"]).read_text())
    scoped_log = Path(scoped_row["log_path"]).read_text()
    plain_log = Path(plain_row["log_path"]).read_text()
    algorithm_matrix.validate_payload(scoped, scoped_log, options=scoped_options, **common)
    algorithm_matrix.validate_payload(plain, plain_log, options=plain_options, **common)

    def forged(payload, edit):
        result = copy.deepcopy(payload)
        edit(result)
        return result

    departures = [
        lambda p: p.update(pass_scope="off"),
        lambda p: p.update(pass_scope_control=None),
        lambda p: p["pass_scope_control"].update(transitions=p["pass_scope_control"]["transitions"] + 1),
        lambda p: p["pass_scope_control"].update(passes=p["pass_scope_control"]["passes"] + 1),
        # Consistent with itself, but not with the work the kernel did.
        lambda p: p["pass_scope_control"].update(passes=p["pass_scope_control"]["passes"] + 1,
                                                 transitions=p["pass_scope_control"]["transitions"] + 2),
        lambda p: p["pass_scope_control"].update(signal="record-cursor"),
        lambda p: p["pass_scope_control"].update(outside_policy="lru"),
        lambda p: p["pass_scope_control"].update(per_line_bits=1),
        lambda p: p["pass_scope_control"]["kernel"].update(victims=-1),
        lambda p: p["pass_scope_control"].pop("setup"),
    ]
    for edit in departures:
        with pytest.raises(RecordReceiptError, match="pass scope"):
            algorithm_matrix.validate_payload(forged(scoped, edit), scoped_log, options=scoped_options, **common)
    for edit in (lambda p: p.update(pass_scope="srrip"),
                 lambda p: p.update(pass_scope_control=scoped["pass_scope_control"])):
        with pytest.raises(RecordReceiptError, match="pass scope"):
            algorithm_matrix.validate_payload(forged(plain, edit), plain_log, options=plain_options, **common)
    # A scoped request is never satisfied by an unscoped run, nor the reverse.
    with pytest.raises(RecordReceiptError, match="pass scope"):
        algorithm_matrix.validate_payload(plain, plain_log, options=scoped_options, **common)
    with pytest.raises(RecordReceiptError, match="pass scope"):
        algorithm_matrix.validate_payload(scoped, scoped_log, options=plain_options, **common)


@pytest.mark.parametrize("algorithm,extra", [
    ("spmv", ("--policy", "GRASP_PAPER", "--grasp-registration", "declared")),
    ("bfs", ("--policy", "POPT_UNCHARGED", "--grasp-registration", "declared")),
    ("bfs", ("--policy", "LRU", "--grasp-registration", "declared")),
    ("bfs", ("--policy", "GRASP_PAPER")),
    ("bfs", ("--policy", "GRASP_PAPER", "--grasp-registration", "declared", "--grasp-scope", "graph-passes")),
    ("bfs", ("--mode", "transport", "--grasp-registration", "declared")),
    ("bfs", ("--mode", "replacement-prefetch", "--record-base-policy", "GRASP_PAPER",
             "--grasp-registration", "declared")),
    ("bfs", ("--mode", "transport", "--record-base-policy", "GRASP_PAPER", "--record-model", "window",
             "--grasp-registration", "declared")),
])
def test_the_binary_refuses_a_scope_it_has_no_model_for(tmp_path, algorithm, extra):
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = _binary(tmp_path, algorithm, "--pass-scope", "srrip", *extra)
    assert ran.returncode == 2 and REFUSAL in ran.stderr, ran.stderr[-1500:]


def test_the_batch_path_refuses_the_scope(tmp_path):
    """Independent SpMV queries run beside the algorithms backend, so the scope is refused before them."""
    if not ALGORITHMS.is_file():
        pytest.skip("current algorithm executable is not built")
    ran = _binary(tmp_path, "spmv", "--pass-scope", "srrip", "--policy", "GRASP_PAPER", "--queries", "2")
    assert ran.returncode == 2 and "independent queries require unmodified CSR SpMV baselines" in ran.stderr, (
        ran.stderr[-1500:])


@pytest.mark.parametrize("algorithm,spec,options", [
    ("spmv", "GRASP_PAPER", ""),
    ("bfs", "POPT:UNCHARGED", ""),
    ("bfs", "LRU", ""),
    ("bfs", "ECG:prefetch", "--record-base-policy GRASP_PAPER"),
])
def test_the_runner_refuses_a_scope_it_has_no_model_for(tmp_path, algorithm, spec, options):
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    out = tmp_path / "refused"
    out.mkdir()
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", algorithm, "--current-algorithms",
        "--options", f"--graph {_graph(tmp_path, algorithm)} --delta 2 {options} {FAIR} --pass-scope srrip",
        "--policies", spec, *GEOMETRY, "--l3-sizes", "2048B", "--out-dir", str(out), "--no-build"])
    rows = algorithm_matrix.run_cache_cell(args, out, roi_matrix.parse_policy_spec(spec), "2048B",
                                           roi_matrix.run_command, roi_matrix.parse_size_bytes)
    # The runner's own admission refuses, before the label would.
    assert rows[0]["status"] == "error" and (
        "pass scope requires the declared GRASP base on BFS, SSSP or BC without prefetch" in rows[0]["error"]), (
        rows[0].get("error"))



_NATIVE_MAIN = """
#include "ecg_algorithm_main.h"
int main(int argc, char** argv) {
    bool invoked = false;
    const int status = ecg_algorithm::applicationMain(argc, argv,
        [&](const auto&, const auto&) { invoked = true; return 0; });
    return invoked ? 9 : status;
}
"""


def test_a_native_main_refuses_the_scope(tmp_path):
    source = tmp_path / "native_main.cc"
    source.write_text(_NATIVE_MAIN)
    binary = tmp_path / "native_main"
    built = subprocess.run([
        "g++", "-std=c++17", "-O1", "-fopenmp", "-I", str(ROOT / "bench/include/external/gapbs"),
        "-I", str(ROOT / "bench/include"), str(source), "-o", str(binary),
    ], capture_output=True, text=True, timeout=300, check=False)
    assert built.returncode == 0, built.stderr[-3000:]
    ran = subprocess.run([str(binary), "--algorithm", "bfs", "--graph", str(tmp_path / "absent.sg"),
                          "--policy", "GRASP_PAPER", "--grasp-registration", "declared", "--pass-scope", "srrip"],
                         capture_output=True, text=True, timeout=60, check=False)
    assert ran.returncode == 2 and "pass scope is cache_sim-only" in ran.stderr, (ran.returncode, ran.stderr)
