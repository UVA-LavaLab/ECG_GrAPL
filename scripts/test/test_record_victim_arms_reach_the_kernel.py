"""The opt-in record victim arms must survive the runner, not just the kernel.

Both `--record-governed-first` and `--record-store-bound` were once parsed by
the runner and then dropped before the kernel argv was built, because the
option was taught to the parser and to `policy_labels` but not to the command
construction or to the positional `policy_labels` call sites. The kernel then
ran its default arm, the receipt honestly said so, and only the receipt
validator caught the mismatch.

An earlier qualification missed this because it invoked the kernel binary
directly and then called `validate_payload`, exercising the kernel and the
validator while skipping the layer in between. These tests capture the argv the
runner would actually execute, so the skipped layer is the one under test.
"""
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


def _captured_command(tmp_path, extra_options):
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
        "--options", f"--graph {graph} --repeat 2 --record-base-policy GRASP_PAPER "
                     f"--record-preprocess csr {extra_options}",
        "--policies", "ECG:replacement", "--l3-sizes", "8MB", "--l3-ways", "16",
        "--out-dir", str(tmp_path), "--no-build",
    ])
    captured: list[list[str]] = []

    def run_command(command, *rest, **kw):
        captured.append([str(part) for part in command])
        raise RuntimeError("stop after the argv is built")

    try:
        algorithm_matrix.run_cache_cell(
            args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB",
            run_command, roi_matrix.parse_size_bytes)
    except Exception:
        pass
    return captured[0] if captured else []


@pytest.mark.parametrize("option,value,flag", [
    ("--record-governed-first", "on", "--record-governed-first"),
    ("--record-store-bound", "keep", "--record-store-bound"),
    ("--record-pressure-gate", "on", "--record-pressure-gate"),
    ("--record-pressure-gate", "duel", "--record-pressure-gate"),
    ("--record-rrpv-order", "on", "--record-rrpv-order"),
])
def test_requested_victim_arm_reaches_the_kernel_argv(tmp_path, option, value, flag):
    command = _captured_command(tmp_path, f"{option} {value}")
    assert command, "the cell never reached kernel command construction"
    assert flag in command, f"{flag} was parsed by the runner but dropped before the kernel"
    assert command[command.index(flag) + 1] == value


@pytest.mark.parametrize("option,default", [
    ("--record-governed-first", "no"),
    ("--record-store-bound", "drop"),
    ("--record-pressure-gate", "no"),
    ("--record-rrpv-order", "no"),
])
def test_default_victim_arm_is_not_emitted(tmp_path, option, default):
    command = _captured_command(tmp_path, f"{option} {default}")
    assert command, "the cell never reached kernel command construction"
    assert option not in command, (
        f"{option} must stay off the kernel argv at its default so existing "
        "commands and their configuration hashes are unchanged")


def _row_label_from_the_real_cell(tmp_path, extra_options):
    """The label `run_cache_cell` actually puts in the row.

    Reconstructing the call is not enough and was the mistake that let this
    through twice. `policy_labels` has several call sites — the row writer, the
    outer `output_policy_labels`, and the flow's expectation helper — and each
    was taught the options separately. A test that calls `policy_labels` itself
    passes no matter what the row writer forwards.

    So this invokes `run_cache_cell` and intercepts `policy_labels` to capture
    what that site produced, which is the value that lands in the CSV.
    """
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
        "--options", f"--graph {graph} --repeat 2 --record-base-policy GRASP_PAPER "
                     f"--record-preprocess csr {extra_options}",
        "--policies", "ECG:replacement", "--l3-sizes", "8MB", "--l3-ways", "16",
        "--out-dir", str(tmp_path), "--no-build",
    ])
    produced: list[str] = []
    real = algorithm_matrix.policy_labels

    def recording(*a, **kw):
        labels = real(*a, **kw)
        produced.extend(labels)
        return labels

    algorithm_matrix.policy_labels = recording
    try:
        algorithm_matrix.run_cache_cell(
            args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "8MB",
            lambda *a, **k: (_ for _ in ()).throw(RuntimeError("stop before simulating")),
            roi_matrix.parse_size_bytes)
    except Exception:
        pass
    finally:
        algorithm_matrix.policy_labels = real
    assert produced, "run_cache_cell never reached the row label site"
    return produced[0]


@pytest.mark.parametrize("extra,suffix", [
    ("--record-governed-first on", "_GOVERNED_FIRST"),
    ("--record-pressure-gate on", "_PRESSURE_GATE"),
    ("--record-store-bound keep", "_STORE_BOUND"),
    ("--record-governed-first on --record-pressure-gate on",
     "_GOVERNED_FIRST_PRESSURE_GATE"),
    ("--record-pressure-gate duel", "_PRESSURE_DUEL"),
    ("--record-governed-first on --record-pressure-gate duel",
     "_GOVERNED_FIRST_PRESSURE_DUEL"),
    ("--record-rrpv-order on", "_RRPV_ORDER"),
    ("--record-governed-first on --record-rrpv-order on", "_GOVERNED_FIRST_RRPV_ORDER"),
    ("--record-governed-first on --record-rrpv-order on --record-pressure-gate duel",
     "_GOVERNED_FIRST_RRPV_ORDER_PRESSURE_DUEL"),
])
def test_the_row_label_site_carries_every_arm(tmp_path, extra, suffix):
    base = _row_label_from_the_real_cell(tmp_path, "")
    assert base == "ECG_REPLACEMENT_BASE_GRASP_PAPER"
    assert _row_label_from_the_real_cell(tmp_path, extra) == base + suffix, (
        f"run_cache_cell's row label drops {extra!r}, so a correctly executed "
        "cell is rejected as a missing policy")


def test_victim_arm_labels_are_distinct_and_suffixed():
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    spec = [parse_policy_spec("ECG:replacement")]
    base = algorithm_matrix.policy_labels(spec, base_policy="GRASP_PAPER")[0]
    governed = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", governed_first="on")[0]
    stored = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", store_bound="keep")[0]
    gated = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", pressure_gate="on")[0]
    assert base == "ECG_REPLACEMENT_BASE_GRASP_PAPER"
    assert governed == base + "_GOVERNED_FIRST"
    assert stored == base + "_STORE_BOUND"
    assert gated == base + "_PRESSURE_GATE"
    # The gate composes with governed-first, since the study that motivates it
    # measures the two together and they would otherwise share one label.
    both = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", governed_first="on", pressure_gate="on")[0]
    assert both == base + "_GOVERNED_FIRST_PRESSURE_GATE"
    dueled = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", pressure_gate="duel")[0]
    both_dueled = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", governed_first="on", pressure_gate="duel")[0]
    assert dueled == base + "_PRESSURE_DUEL"
    assert both_dueled == base + "_GOVERNED_FIRST_PRESSURE_DUEL"
    assert len({base, governed, stored, gated, both, dueled, both_dueled}) == 7, (
        "each arm needs a distinct output label or paired cells collide in one matrix")


@pytest.mark.parametrize("arm,suffix", [("no", ""), ("on", "_GOVERNED_FIRST")])
def test_pagerank_branch_labels_carry_the_arm(arm, suffix):
    """PageRank never passes through algorithm_matrix.policy_labels.

    It runs as a separate kernel under current_pr_baselines, so its labels take
    a different branch of output_policy_labels. That branch once returned the
    bare spec label for both arms, which would have put a control and a
    treatment in one matrix under the same name.
    """
    from types import SimpleNamespace
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True,
        record_governed_first=arm, record_store_bound="drop", options="")
    labels = roi_matrix.output_policy_labels(
        args, [parse_policy_spec("ECG:replacement")])
    assert labels == ["ECG_REPLACEMENT" + suffix]


def test_pagerank_arms_do_not_collide():
    from types import SimpleNamespace
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    spec = [parse_policy_spec("ECG:replacement")]

    def labels(arm):
        return roi_matrix.output_policy_labels(SimpleNamespace(
            current_algorithms=False, current_pr_baselines=True,
            record_governed_first=arm, record_store_bound="drop", options=""), spec)[0]

    assert labels("no") != labels("on"), (
        "the PageRank control and treatment would share one row label")


@pytest.mark.parametrize("rrpv", ["no", "on"])
@pytest.mark.parametrize("gate", ["no", "on", "duel"])
@pytest.mark.parametrize("arm", ["no", "on"])
def test_completion_check_expects_the_label_the_runner_produces(tmp_path, arm, gate, rrpv):
    """The invariant that five diverging label sites all violated.

    Expected labels are computed in the flow's completion check, and actual
    labels by the runner. Each was taught about the opt-in arms separately, and
    a divergence rejects a correctly executed cell as a missing policy. This
    pins the two against each other directly.
    """
    import csv
    from types import SimpleNamespace
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec

    produced = algorithm_matrix.policy_labels(
        [parse_policy_spec("ECG:replacement")], "GRASP_PAPER", "off", "next", 6,
        "all", "future", "enabled", "off", 1, arm, "drop", "progress", gate, rrpv)[0]

    csv_path = tmp_path / "roi_matrix.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": produced})

    status, detail = experiment_run.csv_status(
        csv_path, ["ECG:replacement"], "GRASP_PAPER", "off", "next", 6,
        "all", "future", "enabled", "off", 1, arm, "drop", "progress", gate, rrpv)
    assert status == "ok", (
        f"the completion check rejected the label the runner produces: {detail}")


@pytest.mark.parametrize("gate", ["no", "on", "duel"])
@pytest.mark.parametrize("governed", ["no", "on"])
def test_pressure_gate_label_agrees_across_every_decider(gate, governed):
    """The gate is the fourth option to change a label; pin it like the others.

    The `governed="no"` case is the one that matters and the one an earlier
    version of this test missed. `expected_labels_for` has a fast path that
    returns the bare label for configurations that look default, and a new
    option must be added to that condition or the fast path silently ignores
    it. Setting any *other* option to non-default leaves the fast path
    unexercised, so the gate has to be tested as the only thing that differs.
    """
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    produced = algorithm_matrix.policy_labels(
        [parse_policy_spec("ECG:replacement")], "LRU", "off", "next", 6,
        "all", "future", "enabled", "off", 1, governed, "drop", "progress", gate)
    expected = experiment_run.expected_labels_for(
        ["ECG:replacement"], "LRU", "off", "next", 6, "all", "future",
        "enabled", "off", 1, governed, "drop", "progress", gate)
    assert expected == produced, "the flow expects labels the runner does not produce"
    suffix = {"no": "", "on": "_PRESSURE_GATE", "duel": "_PRESSURE_DUEL"}[gate]
    assert produced[0].endswith(suffix) and produced[0].count("_PRESSURE_") == (gate != "no"), (
        "the gate must reach the label even when every other option is default")


@pytest.mark.parametrize("arm", ["no", "on"])
def test_every_expected_label_source_agrees(arm):
    """All label deciders must agree, including the completion marker.

    The expected-label expression existed in three copies inside the flow, each
    with its own fast path for default-looking configurations. Six sites had to
    be taught one option, and four of them were found only by spending a run.
    They are now one helper; this pins that helper against what the runner
    actually emits so a future copy cannot drift silently.
    """
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows import experiment_run

    produced = algorithm_matrix.policy_labels(
        [__import__("scripts.experiments.ecg.policy_specs", fromlist=["x"])
         .parse_policy_spec("ECG:replacement")],
        "GRASP_PAPER", "off", "next", 6, "all", "future", "enabled", "off", 1,
        arm, "drop")
    expected = experiment_run.expected_labels_for(
        ["ECG:replacement"], "GRASP_PAPER", "off", "next", 6, "all", "future",
        "enabled", "off", 1, arm, "drop")
    assert expected == produced, (
        "the flow expects labels the runner does not produce")


def _runner_cell(tmp_path, extra_options, l3_size="2048B", l3_ways="4"):
    """The argv and environment the runner would execute, at fixture geometry.

    The caches are small enough that the fixture evicts, since a victim rule
    that never runs cannot be observed. The default LLC has eight sets.
    """
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
        "--options", f"--graph {graph} --repeat 2 --record-base-policy GRASP_PAPER "
                     f"--record-preprocess csr {extra_options}",
        "--policies", "ECG:replacement", "--l1d-size", "128B", "--l1d-ways", "2",
        "--l2-size", "256B", "--l2-ways", "2", "--l3-sizes", l3_size, "--l3-ways", l3_ways,
        "--out-dir", str(tmp_path), "--no-build",
    ])
    captured: list[tuple[list[str], dict[str, str]]] = []

    def run_command(command, cwd, env, *rest, **kw):
        captured.append(([str(part) for part in command], dict(env)))
        raise RuntimeError("stop after the command is built")

    try:
        algorithm_matrix.run_cache_cell(
            args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), l3_size,
            run_command, roi_matrix.parse_size_bytes)
    except Exception:
        pass
    assert captured, "run_cache_cell never reached the kernel command"
    return captured[0]


@pytest.mark.parametrize("gate", ["no", "on"])
def test_pressure_gate_counts_the_lines_the_record_rule_governs(tmp_path, gate):
    """The gate's signal must not come from the legacy epoch region selector.

    The per-set pressure counter once keyed on `isGovernedProperty`, which is
    the region `CACHE_ECG_EPOCH_REGION_INDICES` names, not the lines the
    record rule governs. The runner strips every `CACHE_` variable, so the
    kernel fell back to region 1: for SpMV the streamed output `y`, not the
    gathered `x` the records describe. On cit-Patents every `y` line missed,
    the counter stayed saturated, and the gate never relaxed once.

    Both runs set the selector to a value of the same length, because the
    environment's size moves the heap and with it a small graph's set mapping.
    The gate-off arm is the control that makes the comparison meaningful.
    """
    import json
    import subprocess
    binary = ROOT / "bench/bin_sim/algorithms"
    if not binary.is_file():
        pytest.skip("current algorithm executable is not built")
    command, env = _runner_cell(
        tmp_path, f"--record-governed-first on --record-pressure-gate {gate}")
    assert not any(key.startswith("CACHE_") for key in env)
    output = Path(command[command.index("--output") + 1])
    results = {}
    for region in ("0", "1"):
        ran = subprocess.run(
            command, cwd=ROOT, env={**env, "CACHE_ECG_EPOCH_REGION_INDICES": region},
            capture_output=True, text=True, timeout=60, check=False)
        assert ran.returncode == 0, ran.stdout + ran.stderr
        payload = json.loads(output.read_text())
        assert payload["workload"]["record_pressure_gate"] == gate
        results[region] = (
            {key: value for key, value in payload["metrics"].items()
             if key.startswith("ecg_record_victim_")},
            payload["traffic_phases"]["kernel"])
    decisions = results["1"][0]
    if gate == "on":
        assert 0 < decisions["ecg_record_victim_unpressured"] < decisions["ecg_record_victim_decisions"], (
            "the gate must both relax and hold on this fixture, or the comparison is vacuous")
    assert results["0"] == results["1"], (
        "the victim decisions follow a region selector the record rule never reads")


def _pagerank_cell(tmp_path, monkeypatch, gate, governed="no", l3_size="8192B", l3_ways="2",
                   rrpv="no", policy="ECG:replacement", written="no", graph_bytes=None):
    """The argv and environment the runner would execute for PageRank.

    PageRank is a separate executable that reads its record settings from the
    environment, so the gate reaches it through a different site than the
    algorithms CLI. The default LLC has 64 sets, the smallest the duel allows.
    The graph is pressure512 unless other serialized graph bytes are given.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    if graph_bytes is None:
        graph = tmp_path / "pressure512.sg"
        graph.write_bytes(algorithm_outputs()[graph.name][0])
    else:
        graph = tmp_path / "fixture.sg"
        graph.write_bytes(graph_bytes)
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
        "--record-governed-first", governed, "--record-pressure-gate", gate,
        *(("--record-rrpv-order", rrpv) if rrpv != "no" else ()),
        *(("--record-written-in-place", written) if written != "no" else ()),
        "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0",
        "--policies", policy, "--l1d-size", "128B", "--l1d-ways", "2",
        "--l2-size", "256B", "--l2-ways", "2", "--l3-sizes", l3_size, "--l3-ways", l3_ways,
        "--out-dir", str(tmp_path), "--no-build",
    ])
    captured: list[tuple[list[str], dict[str, str]]] = []

    def run_command(command, cwd, env, *rest, **kw):
        captured.append(([str(part) for part in command], dict(env)))
        raise RuntimeError("stop after the command is built")

    monkeypatch.setattr(roi_matrix, "run_command", run_command)
    try:
        roi_matrix.run_cache_sim(
            args, tmp_path, roi_matrix.parse_policy_spec(policy), l3_size)
    except Exception:
        pass
    assert captured, "run_cache_sim never reached the PageRank command"
    return captured[0]


@pytest.mark.parametrize("gate,value", [("no", "0"), ("on", "1"), ("duel", "2")])
def test_pagerank_environment_carries_the_pressure_gate(tmp_path, monkeypatch, gate, value):
    _, env = _pagerank_cell(tmp_path, monkeypatch, gate)
    assert env["ECG_RECORD_PRESSURE_GATE"] == value


@pytest.mark.parametrize("gate,suffix", [
    ("no", ""), ("on", "_PRESSURE_GATE"), ("duel", "_PRESSURE_DUEL")])
@pytest.mark.parametrize("governed", ["no", "on"])
def test_pagerank_branch_labels_carry_the_pressure_gate(governed, gate, suffix):
    """The PageRank branch labels the gate, and the flow expects that label.

    PageRank rows are labelled by output_policy_labels, while the completion
    check computes what it expects through expected_labels_for. The two are
    separate code, so each gate value is pinned across both.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True,
        record_governed_first=governed, record_store_bound="drop",
        record_expiry_clock="progress", record_pressure_gate=gate, options="")
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec("ECG:replacement")])
    governed_suffix = "_GOVERNED_FIRST" if governed == "on" else ""
    assert produced == ["ECG_REPLACEMENT" + governed_suffix + suffix]
    expected = experiment_run.expected_labels_for(
        ["ECG:replacement"], "LRU", "off", "next", 6, "all", "future",
        "enabled", "off", 1, governed, "drop", "progress", gate)
    assert expected == produced, "the flow expects a PageRank label the runner does not produce"


@pytest.mark.parametrize("suite,refused", [
    ("cache-sim", False), ("gem5", True), ("sniper", True), ("both", True)])
@pytest.mark.parametrize("gate", ["on", "duel"])
def test_pressure_gate_is_refused_where_no_backend_implements_it(tmp_path, suite, refused, gate):
    """Only cache_sim implements the gate, so only cache_sim may carry its label.

    gem5's L3 is configured from its own arguments and never receives the gate,
    and Sniper's record path reads no pressure variable. The runner once
    accepted the gate on both and labelled the native PageRank row
    ECG_REPLACEMENT_PRESSURE_*, while the cache ran the ungated rule.
    """
    import subprocess
    ran = subprocess.run([
        sys.executable, str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", suite, "--dry-run", "--benchmark", "pr",
        "--policies", "ECG:replacement", "--record-pressure-gate", gate,
        "--out-dir", str(tmp_path)], cwd=ROOT, capture_output=True, text=True,
        timeout=300, check=False)
    text = ran.stdout + ran.stderr
    if refused:
        assert ran.returncode != 0 and "record pressure gate is cache_sim-only" in text, text[-600:]
    else:
        assert ran.returncode == 0, text[-600:]


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_native_algorithm_cells_refuse_the_pressure_gate(tmp_path, backend):
    """The native algorithms binary parses the gate, but the native cache has none.

    The RV64 guest shares ecg_algorithm_main.h, so it accepts the option and
    would report the gate in its workload while the simulated LLC ignores it.
    """
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", backend, "--benchmark", "spmv", "--current-algorithms", "--ecg-equivalence",
        "--options", f"--graph {graph} --record-pressure-gate duel",
        "--policies", "ECG:replacement", "--dry-run", "--out-dir", str(tmp_path)])
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "32kB", backend, roi_matrix)
    assert rows and rows[0]["status"] == "error", rows
    assert "record pressure gate is cache_sim-only" in rows[0].get("error", ""), rows[0].get("error")


def _run_kernel(command, env, timeout=120):
    import subprocess
    return subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                          timeout=timeout, check=False)


def test_pressure_duel_refuses_a_set_count_that_is_not_a_multiple_of_64(tmp_path, monkeypatch):
    """Leaders are one set in every 64, so fewer or uneven sets fail closed.

    Both kernels are checked through the runner, at the runner's eight-set
    fixture LLC, so the refusal is the real binaries' and not a unit model's.
    """
    for binary in ("algorithms", "pr"):
        if not (ROOT / "bench/bin_sim" / binary).is_file():
            pytest.skip(f"current {binary} executable is not built")
    (tmp_path / "spmv").mkdir()
    command, env = _runner_cell(tmp_path / "spmv", "--record-pressure-gate duel")
    ran = _run_kernel(command, env)
    assert ran.returncode != 0 and "multiple of 64 LLC sets" in ran.stdout + ran.stderr
    (tmp_path / "pr").mkdir()
    command, env = _pagerank_cell(tmp_path / "pr", monkeypatch, "duel", l3_size="2048B", l3_ways="4")
    ran = _run_kernel(command, env)
    assert ran.returncode != 0 and "multiple of 64 LLC sets" in ran.stdout + ran.stderr


@pytest.mark.parametrize("governed", ["no", "on"])
def test_pressure_duel_trains_on_exactly_the_transfers_the_spmv_kernel_makes(tmp_path, governed):
    """The selector counts LLC demand misses and dirty victims, nothing else.

    Training starts when the record is configured, which is the kernel phase
    boundary, so the duel's transfer count must equal the kernel's memory
    accesses plus LLC writebacks with no prefetch fills. Both leader groups
    must see traffic and the followers must take both sides, or the selector
    never did anything and the comparison would be vacuous.
    """
    import json
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    extra = "--record-governed-first on " if governed == "on" else ""
    results = {}
    for gate in ("no", "duel"):
        work = tmp_path / gate
        work.mkdir()
        command, env = _runner_cell(work, extra + f"--record-pressure-gate {gate}",
                                    l3_size="8192B", l3_ways="2")
        ran = _run_kernel(command, env)
        assert ran.returncode == 0, (ran.stdout + ran.stderr)[-600:]
        results[gate] = json.loads(Path(command[command.index("--output") + 1]).read_text())
    payload = results["duel"]
    assert payload["workload"]["record_pressure_gate"] == "duel"
    metrics, kernel = payload["metrics"], payload["traffic_phases"]["kernel"]
    assert kernel["prefetch_fills"] == 0
    assert metrics["ecg_record_duel_transfers"] == kernel["memory_accesses"] + kernel["llc_writebacks"]
    assert metrics["ecg_record_duel_leader_transfers_rule"] > 0
    assert metrics["ecg_record_duel_leader_transfers_base"] > 0
    assert metrics["ecg_record_duel_follower_rule"] > 0
    assert metrics["ecg_record_duel_follower_base"] > 0
    assert metrics["ecg_record_duel_winner_changes"] > 0
    control = results["no"]["metrics"]
    assert control["ecg_record_duel_transfers"] == 0 and control["ecg_record_duel_selector"] == 511, (
        "the duel must not train when it is off")


@pytest.mark.parametrize("governed", ["no", "on"])
def test_pressure_duel_trains_on_exactly_the_transfers_pagerank_makes(tmp_path, monkeypatch, governed):
    """PageRank resets its statistics after a warm replay; the duel counts from there.

    The selector keeps what the warm replay taught it, as the per-set counter
    does, but its transfer count resets with the statistics, so it must equal
    the reported memory accesses plus LLC writebacks. The ranking must not
    change, because the gate only chooses which line leaves.
    """
    import json
    import re
    if not (ROOT / "bench/bin_sim/pr").is_file():
        pytest.skip("current PageRank executable is not built")
    results = {}
    for gate in ("no", "duel"):
        work = tmp_path / gate
        work.mkdir()
        command, env = _pagerank_cell(work, monkeypatch, gate, governed=governed)
        ran = _run_kernel(command, env)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-600:]
        checksum = re.search(r"\[ECG-PR-RESULT [^\]]*score_checksum=([0-9a-f]+)", text)
        assert checksum, text[-600:]
        results[gate] = (json.loads(Path(env["CACHE_OUTPUT_JSON"]).read_text()), checksum.group(1))
    metrics, checksum = results["duel"]
    assert checksum == results["no"][1], "the gate changed the PageRank result"
    assert metrics["prefetch_fills"] == 0
    assert metrics["ecg_record_duel_transfers"] == metrics["memory_accesses"] + metrics["llc_writebacks"]
    assert metrics["ecg_record_duel_leader_transfers_rule"] > 0
    assert metrics["ecg_record_duel_leader_transfers_base"] > 0
    assert metrics["ecg_record_duel_follower_rule"] > 0
    assert metrics["ecg_record_duel_follower_base"] > 0
    assert metrics["ecg_record_duel_winner_changes"] > 0
    control = results["no"][0]
    assert control["ecg_record_duel_transfers"] == 0 and control["ecg_record_duel_selector"] == 511, (
        "the duel must not train when it is off")


# The RRPV order is the arm under which no ECG victim decision reads recency:
# the choice among non-governed ways, the choice among DEAD ways, the tie
# between equally distant bounds, and the base victim itself all follow the
# line's GRASP re-reference value. PageRank's record rule otherwise falls back
# to an LRU base, since pr.cc configures no other; these pin the arm through
# every layer that PageRank and the algorithms kernels each take.

_GRASP_PAPER_TIERS = {"GRASP_HOT_FRACTION": "0.50", "GRASP_BOUNDARY_MODE": "capacity"}


def test_rrpv_order_labels_are_distinct_and_refused_where_the_order_cannot_hold():
    """Only the replacement mechanism under the NEXT model carries the order.

    Prefetch admission still consults the base policy and the window and
    frontier models choose victims by their own rules, so the label must be
    refused there rather than name an order the cache does not run.
    """
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    from scripts.experiments.ecg.record_receipts import RecordReceiptError

    def labels(policy, **kw):
        return algorithm_matrix.policy_labels([parse_policy_spec(policy)], base_policy="GRASP_PAPER", **kw)[0]

    base = labels("ECG:replacement")
    ordered = labels("ECG:replacement", rrpv_order="on")
    governed = labels("ECG:replacement", governed_first="on", rrpv_order="on")
    dueled = labels("ECG:replacement", governed_first="on", rrpv_order="on", pressure_gate="duel")
    assert base == "ECG_REPLACEMENT_BASE_GRASP_PAPER"
    assert ordered == base + "_RRPV_ORDER"
    assert governed == base + "_GOVERNED_FIRST_RRPV_ORDER"
    assert dueled == base + "_GOVERNED_FIRST_RRPV_ORDER_PRESSURE_DUEL"
    assert labels("ECG:replacement", rrpv_order="no") == base
    for policy in ("ECG:replacement-prefetch", "ECG:transport", "ECG:prefetch", "GRASP_PAPER", "LRU"):
        with pytest.raises(RecordReceiptError, match="RRPV order requires"):
            labels(policy, rrpv_order="on")
    for model in ("window", "frontier"):
        with pytest.raises(RecordReceiptError, match="RRPV order requires"):
            labels("ECG:replacement", record_model=model, rrpv_order="on")


@pytest.mark.parametrize("extra,refused", [
    ("--record-base-policy GRASP_PAPER", False),
    ("--record-base-policy GRASP_PAPER --record-governed-first on", False),
    ("--record-base-policy GRASP_PAPER --record-pressure-gate duel", False),
    ("--record-base-policy LRU", True),
    ("", True),
    ("--record-base-policy GRASP_PAPER --record-model window", True),
    ("--record-base-policy GRASP_PAPER --record-model frontier", True),
])
def test_rrpv_order_options_require_the_grasp_base_under_the_next_model(extra, refused):
    """The order ranks by the GRASP tiers, so the kernel must register them.

    The algorithms kernel registers GRASP_PAPER's tiers only for that base, so
    an LRU base would leave every line in one tier and the order would reduce
    to a scan by way index. The label cannot check this, because PageRank's
    labels always name the LRU base, so the options parser does.
    """
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.record_resources import RecordResourceError
    assert algorithm_matrix.parse_options("--graph g.sg").record_rrpv_order == "no"
    text = f"--graph g.sg {extra} --record-rrpv-order on"
    if refused:
        with pytest.raises(RecordResourceError, match="RRPV order requires the GRASP_PAPER record base"):
            algorithm_matrix.parse_options(text)
    else:
        assert algorithm_matrix.parse_options(text).record_rrpv_order == "on"


@pytest.mark.parametrize("gate,gate_suffix", [("no", ""), ("duel", "_PRESSURE_DUEL")])
@pytest.mark.parametrize("rrpv,suffix", [("no", ""), ("on", "_RRPV_ORDER")])
@pytest.mark.parametrize("governed", ["no", "on"])
def test_pagerank_branch_labels_carry_the_rrpv_order(governed, rrpv, suffix, gate, gate_suffix):
    """PageRank labels the order in its own branch, and the flow must agree.

    The `governed="no", gate="no"` case is the one that exercises the fast
    path in expected_labels_for, which returns the bare label for any
    configuration that looks default unless the new option is in its condition.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True,
        record_governed_first=governed, record_store_bound="drop",
        record_expiry_clock="progress", record_pressure_gate=gate,
        record_rrpv_order=rrpv, options="")
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec("ECG:replacement")])
    governed_suffix = "_GOVERNED_FIRST" if governed == "on" else ""
    assert produced == ["ECG_REPLACEMENT" + governed_suffix + suffix + gate_suffix]
    expected = experiment_run.expected_labels_for(
        ["ECG:replacement"], "LRU", "off", "next", 6, "all", "future",
        "enabled", "off", 1, governed, "drop", "progress", gate, rrpv)
    assert expected == produced, "the flow expects a PageRank label the runner does not produce"


@pytest.mark.parametrize("rrpv,value", [("no", "0"), ("on", "1")])
def test_pagerank_environment_carries_the_rrpv_order_with_grasp_paper_tiers(
        tmp_path, monkeypatch, rrpv, value):
    """Under the order PageRank tiers its lines exactly as the GRASP_PAPER cell does.

    pr.cc registers an explicit 0.15 hot fraction for its record arm, which
    the LRU base never reads. Once the order ranks by those tiers, the arm and
    the GRASP_PAPER baseline it is compared with must tier identically, or the
    comparison mixes the order with a different hot set. An ambient value is
    planted to show the runner, not the caller's shell, decides the tiers.
    """
    monkeypatch.setenv("GRASP_HOT_FRACTION", "0.15")
    monkeypatch.setenv("GRASP_BOUNDARY_MODE", "vertex")
    (tmp_path / "record").mkdir()
    _, env = _pagerank_cell(tmp_path / "record", monkeypatch, "no", rrpv=rrpv)
    assert env["ECG_RECORD_RRPV_ORDER"] == value
    tiers = {key: env.get(key) for key in _GRASP_PAPER_TIERS}
    if rrpv == "on":
        assert tiers == _GRASP_PAPER_TIERS
    else:
        assert tiers == {key: None for key in _GRASP_PAPER_TIERS}, (
            "the rule without the order must not observe GRASP tiers")
    (tmp_path / "grasp").mkdir()
    _, grasp = _pagerank_cell(tmp_path / "grasp", monkeypatch, "no", policy="GRASP_PAPER")
    assert {key: grasp.get(key) for key in _GRASP_PAPER_TIERS} == _GRASP_PAPER_TIERS
    assert "ECG_RECORD_RRPV_ORDER" not in grasp, "a baseline must not carry the record arm"


def test_rrpv_order_reaches_the_spmv_kernel_and_orders_every_decision(tmp_path):
    """Through the runner, every SpMV decision is RRPV-ordered and the result holds.

    SpMV's base is GRASP_PAPER, so even the rule without the order never
    reaches the LRU scan; what the order changes there is the choice among
    non-governed ways, which governed-first otherwise takes by recency.
    """
    import json
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    results = {}
    for rrpv in ("no", "on"):
        work = tmp_path / rrpv
        work.mkdir()
        command, env = _runner_cell(work, f"--record-governed-first on --record-rrpv-order {rrpv}")
        assert ("--record-rrpv-order" in command) == (rrpv == "on")
        ran = _run_kernel(command, env)
        assert ran.returncode == 0, (ran.stdout + ran.stderr)[-600:]
        results[rrpv] = json.loads(Path(command[command.index("--output") + 1]).read_text())
    for rrpv, payload in results.items():
        assert payload["workload"]["record_rrpv_order"] == rrpv
    control, ordered = results["no"]["metrics"], results["on"]["metrics"]
    assert control["ecg_record_victim_rrpv_ordered"] == 0
    assert control["ecg_record_victim_base_lru"] == 0
    assert ordered["ecg_record_victim_decisions"] > 0
    assert ordered["ecg_record_victim_rrpv_ordered"] == ordered["ecg_record_victim_decisions"]
    assert ordered["ecg_record_victim_base_lru"] == 0
    assert ordered["ecg_record_victim_ungoverned_first"] > 0
    assert ordered["ecg_record_victim_rrpv_changed"] > 0, (
        "the order never chose differently from recency, so this fixture cannot show it acts")
    assert results["on"]["workload"]["result_digest"] == results["no"]["workload"]["result_digest"], (
        "the victim order changed the SpMV result")


def test_rrpv_order_reaches_the_pagerank_kernel_and_removes_the_lru_base(tmp_path, monkeypatch):
    """Through the runner, PageRank's record rule stops consulting LRU.

    The control is the rule as it runs today, whose base victim is the LRU
    scan; it must show that scan in use, or the arm removes nothing.
    """
    import json
    import re
    if not (ROOT / "bench/bin_sim/pr").is_file():
        pytest.skip("current PageRank executable is not built")
    results = {}
    for rrpv in ("no", "on"):
        work = tmp_path / rrpv
        work.mkdir()
        command, env = _pagerank_cell(work, monkeypatch, "no", rrpv=rrpv)
        ran = _run_kernel(command, env)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-600:]
        checksum = re.search(r"\[ECG-PR-RESULT [^\]]*score_checksum=([0-9a-f]+)", text)
        assert checksum, text[-600:]
        results[rrpv] = (json.loads(Path(env["CACHE_OUTPUT_JSON"]).read_text()), checksum.group(1), text)
    control, ordered = results["no"][0], results["on"][0]
    assert control["ecg_record_victim_base_lru"] > 0
    assert control["ecg_record_victim_rrpv_ordered"] == 0
    assert "[ECG-PR-RRPV-ORDER" not in results["no"][2]
    assert ordered["ecg_record_victim_decisions"] > 0
    assert ordered["ecg_record_victim_rrpv_ordered"] == ordered["ecg_record_victim_decisions"]
    assert ordered["ecg_record_victim_base_lru"] == 0
    assert "[ECG-PR-RRPV-ORDER hot_percent=50 boundary=capacity]" in results["on"][2]
    assert results["on"][1] == results["no"][1], "the victim order changed the PageRank result"


@pytest.mark.parametrize("suite,refused", [
    ("cache-sim", False), ("gem5", True), ("sniper", True), ("both", True)])
def test_rrpv_order_is_refused_where_no_backend_implements_it(tmp_path, suite, refused):
    """gem5's L3 and Sniper's record path keep their own victim order."""
    import subprocess
    ran = subprocess.run([
        sys.executable, str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", suite, "--dry-run", "--benchmark", "pr",
        "--policies", "ECG:replacement", "--record-rrpv-order", "on",
        "--out-dir", str(tmp_path)], cwd=ROOT, capture_output=True, text=True,
        timeout=300, check=False)
    text = ran.stdout + ran.stderr
    if refused:
        assert ran.returncode != 0 and "record RRPV order is cache_sim-only" in text, text[-600:]
    else:
        assert ran.returncode == 0, text[-600:]


@pytest.mark.parametrize("backend", ["gem5", "sniper"])
def test_native_algorithm_cells_refuse_the_rrpv_order(tmp_path, backend):
    from scripts.experiments.ecg import algorithm_detailed, roi_matrix
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    args = roi_matrix.parse_args([
        "--suite", backend, "--benchmark", "spmv", "--current-algorithms", "--ecg-equivalence",
        "--options", f"--graph {graph} --record-base-policy GRASP_PAPER --record-rrpv-order on",
        "--policies", "ECG:replacement", "--dry-run", "--out-dir", str(tmp_path)])
    rows = algorithm_detailed.run_cell(
        args, tmp_path, roi_matrix.parse_policy_spec("ECG:replacement"), "32kB", backend, roi_matrix)
    assert rows and rows[0]["status"] == "error", rows
    assert "record RRPV order is cache_sim-only" in rows[0].get("error", ""), rows[0].get("error")


# The written-in-place bit. PageRank's kernel stores contribution[u] in place
# once u's gathers finish, so on its last pass a contribution line can be
# retired as DEAD after its last gather and then fetched back by that store. The
# bit declares the in-place store for the region, and the last pass's wrapped
# bounds then decode as FINITE, as they do before another pass, instead of DEAD.
# It is one configuration bit per region, set only by cache_sim's PageRank from
# its environment; these pin it through every layer PageRank takes and refuse
# it everywhere the bit is not set.


def test_written_in_place_labels_are_distinct_and_refused_where_the_bit_cannot_hold():
    """Only the replacement mechanism without prefetch, under the NEXT model, carries it.

    Prefetch also acts on the decoded bound and has not been checked with the
    bit, transport makes no victim decision from it, and the window and
    frontier models choose victims by their own rules. The label is refused
    there rather than name a bit the cache does not act on.
    """
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    from scripts.experiments.ecg.record_receipts import RecordReceiptError

    def labels(policy, **kw):
        return algorithm_matrix.policy_labels([parse_policy_spec(policy)], **kw)[0]

    assert labels("ECG:replacement", written_in_place="no") == "ECG_REPLACEMENT"
    assert labels("ECG:replacement", written_in_place="on") == "ECG_REPLACEMENT_WRITTEN_IN_PLACE"
    assert labels("ECG:replacement", governed_first="on", written_in_place="on") == (
        "ECG_REPLACEMENT_GOVERNED_FIRST_WRITTEN_IN_PLACE")
    assert labels("ECG:replacement", base_policy="GRASP_PAPER", governed_first="on",
                  rrpv_order="on", written_in_place="on", pressure_gate="duel") == (
        "ECG_REPLACEMENT_BASE_GRASP_PAPER_GOVERNED_FIRST_RRPV_ORDER_WRITTEN_IN_PLACE_PRESSURE_DUEL")
    for policy in ("ECG:replacement-prefetch", "ECG:transport", "ECG:prefetch",
                   "GRASP_PAPER", "LRU", "POPT:uncharged"):
        with pytest.raises(RecordReceiptError, match="written-in-place bit requires"):
            labels(policy, written_in_place="on")
    for model in ("window", "frontier"):
        with pytest.raises(RecordReceiptError, match="written-in-place bit requires"):
            labels("ECG:replacement", record_model=model, written_in_place="on")
    with pytest.raises(RecordReceiptError, match="written-in-place bit requires"):
        labels("ECG:replacement", written_in_place="yes")


def test_the_algorithms_options_have_no_written_in_place_bit():
    """The bit is plumbed for PageRank only, so the shared kernels cannot name it.

    The algorithms CLI is shared with the native guests, and neither the shared
    kernels nor the native record paths set the bit. Leaving the option out of
    that parser keeps an algorithms row from carrying a bit its kernel never set.
    """
    from scripts.experiments.ecg import algorithm_matrix
    with pytest.raises(SystemExit):
        algorithm_matrix.parse_options("--graph g.sg --record-written-in-place on")


@pytest.mark.parametrize("gate,gate_suffix", [("no", ""), ("duel", "_PRESSURE_DUEL")])
@pytest.mark.parametrize("rrpv,rrpv_suffix", [("no", ""), ("on", "_RRPV_ORDER")])
@pytest.mark.parametrize("written,suffix", [("no", ""), ("on", "_WRITTEN_IN_PLACE")])
@pytest.mark.parametrize("governed", ["no", "on"])
def test_pagerank_branch_labels_carry_the_written_in_place_bit(
        governed, written, suffix, rrpv, rrpv_suffix, gate, gate_suffix):
    """PageRank labels the bit in its own branch, and the flow must agree.

    The case with every other option at its default is the one that exercises
    the fast path in expected_labels_for, which returns the bare label for any
    configuration that looks default unless the new option is in its condition.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True,
        record_governed_first=governed, record_store_bound="drop",
        record_expiry_clock="progress", record_pressure_gate=gate,
        record_rrpv_order=rrpv, record_written_in_place=written, options="")
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec("ECG:replacement")])
    governed_suffix = "_GOVERNED_FIRST" if governed == "on" else ""
    assert produced == ["ECG_REPLACEMENT" + governed_suffix + rrpv_suffix + suffix + gate_suffix]
    expected = experiment_run.expected_labels_for(
        ["ECG:replacement"], "LRU", "off", "next", 6, "all", "future",
        "enabled", "off", 1, governed, "drop", "progress", gate, rrpv, written)
    assert expected == produced, "the flow expects a PageRank label the runner does not produce"


@pytest.mark.parametrize("governed", ["no", "on"])
def test_completion_check_tells_the_written_in_place_arm_from_its_control(tmp_path, governed):
    """The completion check accepts each arm's own label and rejects the other's.

    Accepting the right label is half of the check. One that also accepted the
    control's label for the treatment could not tell a cell that dropped the
    bit from a cell that carried it.
    """
    import csv
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    for produced_arm in ("no", "on"):
        produced = algorithm_matrix.policy_labels(
            [parse_policy_spec("ECG:replacement")], governed_first=governed,
            written_in_place=produced_arm)[0]
        csv_path = tmp_path / f"{produced_arm}.csv"
        with csv_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=["status", "policy_label"])
            writer.writeheader()
            writer.writerow({"status": "ok", "policy_label": produced})
        for expected_arm in ("no", "on"):
            status, detail = experiment_run.csv_status(
                csv_path, ["ECG:replacement"], governed_first=governed,
                written_in_place=expected_arm)
            assert (status == "ok") == (produced_arm == expected_arm), (
                f"{produced} checked as arm {expected_arm}: {status} {detail}")


@pytest.mark.parametrize("written", ["no", "on"])
def test_job_status_threads_the_written_in_place_bit_into_both_checks(tmp_path, written):
    """The CSV check and the completion-marker check each rebuild the expectation.

    Both read the job's metadata separately, and each was once left on the
    default label while the other was right. Both must see the bit, or a
    correctly executed cell is reported partial after it has already run.
    """
    import csv
    import json
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.flows import experiment_run
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    label = algorithm_matrix.policy_labels(
        [parse_policy_spec("ECG:replacement")], governed_first="on",
        written_in_place=written)[0]
    csv_path = tmp_path / "roi_matrix.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": label})
    cell = {"l3_sizes": ["8MB"], "threads": [1], "structure_prefetch_degree": 0,
            "config_hash": "fixture"}
    (tmp_path / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True, "all_rows_ok": True, "policy_labels": [label], **cell,
        "outputs": {"roi_matrix.csv": experiment_run.output_descriptor(csv_path)}}))
    job = experiment_run.Job(
        job_id="fixture", stage="fixture", kind="roi_matrix", command=[],
        out_dir=tmp_path, log_path=tmp_path / "fixture.log", metadata={
            "policies": ["ECG:replacement"], "record_governed_first": "on",
            "record_written_in_place": written, **cell})
    assert experiment_run.job_csv_status(job) == ("ok", "1 ok rows")


@pytest.mark.parametrize("written,value", [("no", "0"), ("on", "1")])
def test_pagerank_environment_carries_the_written_in_place_bit(tmp_path, monkeypatch, written, value):
    """A record cell's bit is the runner's, and a baseline never sees it.

    The opposite value is planted in the caller's environment first, so a cell
    that inherited the shell's setting fails here. Record cells always carry
    the variable, so the two arms' environments have the same size.
    """
    monkeypatch.setenv("ECG_RECORD_WRITTEN_IN_PLACE", "0" if written == "on" else "1")
    (tmp_path / "record").mkdir()
    _, env = _pagerank_cell(tmp_path / "record", monkeypatch, "no", governed="on", written=written)
    assert env["ECG_RECORD_WRITTEN_IN_PLACE"] == value
    (tmp_path / "grasp").mkdir()
    _, grasp = _pagerank_cell(tmp_path / "grasp", monkeypatch, "no", policy="GRASP_PAPER")
    assert "ECG_RECORD_WRITTEN_IN_PLACE" not in grasp, "a baseline must not carry the record bit"


@pytest.mark.parametrize("suite,benchmark,extra,refused", [
    ("cache-sim", "pr", (), False),
    ("gem5", "pr", (), True),
    ("sniper", "pr", (), True),
    ("both", "pr", (), True),
    ("cache-sim", "spmv", ("--current-algorithms",), True),
    # PageRank itself runs under the shared kernels too, which never set the bit.
    ("cache-sim", "pr", ("--current-algorithms",), True),
    ("cache-sim", "bfs", (), True),
])
def test_written_in_place_bit_is_refused_outside_cache_sim_pagerank(
        tmp_path, suite, benchmark, extra, refused):
    """Only cache_sim's PageRank sets the bit, so only its rows may carry the label.

    gem5's guest and Sniper's record path build their control words without
    it, the shared algorithms kernels never set it, and the runner's other
    kernels run no current record mode.
    """
    import subprocess
    ran = subprocess.run([
        sys.executable, str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", suite, "--dry-run", "--benchmark", benchmark, *extra,
        "--policies", "ECG:replacement", "--record-written-in-place", "on",
        "--out-dir", str(tmp_path)], cwd=ROOT, capture_output=True, text=True,
        timeout=300, check=False)
    text = ran.stdout + ran.stderr
    if refused:
        assert ran.returncode != 0 and (
            "record written-in-place bit is cache_sim PageRank-only" in text), text[-600:]
    else:
        assert ran.returncode == 0, text[-600:]


def _flow_jobs(tmp_path, **settings):
    """Resolve one PageRank stage through the real flow, running nothing.

    No tracked profile carries the bit, so the stage lives in a derived
    manifest under tmp_path and the tracked manifest is only read. `--list`
    builds each job's command, expected labels and metadata as a run would. A
    setting passed as None is left out of the stage.
    """
    import json
    import subprocess
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    manifest = json.loads((ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stage = {
        "name": "written_in_place_probe", "kind": "roi_matrix",
        "profiles": ["written_in_place_probe"], "suite": "cache-sim",
        "graph_set": "written_in_place_probe", "benchmarks": ["pr"],
        "policies": ["ECG:replacement"], "current_pr_baselines": True,
        "algorithm_record_governed_first": "on", "ecg_record_bytes": 0,
        "cache_record_rss_mib": 4096, "cache_sim_omp_threads": 1,
        "l3_sizes": ["8MB"], "l3_ways": "16", "prefetcher": "none", "flowthrough": "off",
        "structure_prefetch_degree": 0, "policy_sharding_allowed": False,
        "out_subdir": "written_in_place_probe"}
    stage.update(settings)
    manifest["stages"] = [{key: value for key, value in stage.items() if value is not None}]
    manifest["graph_sets"]["written_in_place_probe"] = [
        {"name": "pressure512", "path": str(graph), "options_key": "written_in_place_probe"}]
    manifest["benchmark_options"]["written_in_place_probe"] = {
        "pr": "-f {graph_path} -o 0 -n 1 -i 2 -t 0"}
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest))
    run_dir = tmp_path / "run"
    ran = subprocess.run([
        sys.executable, "-I", str(ROOT / "scripts/experiments/ecg/flows/experiment_run.py"),
        "--manifest", str(path), "--profile", "written_in_place_probe",
        "--run-dir", str(run_dir), "--lock-path", str(tmp_path / "lock"),
        "--no-build", "--list"], cwd=ROOT, capture_output=True, text=True,
        timeout=300, check=False)
    resolved = run_dir / "resolved_manifest.json"
    jobs = json.loads(resolved.read_text()).get("jobs", []) if ran.returncode == 0 else []
    return ran, jobs


@pytest.mark.parametrize("written", [None, "no", "on"])
def test_the_flow_carries_the_written_in_place_bit_to_the_pagerank_runner(tmp_path, written):
    """The stage setting reaches the runner's argv, the job's labels and its metadata.

    It must never reach PageRank's own options, which a GAPBS getopt parses.
    Without the setting the command is unchanged, so existing jobs keep their
    configuration hashes. The recorded expectation is checked against the label
    the runner computes from the exact argv the flow built.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    ran, jobs = _flow_jobs(tmp_path, algorithm_record_written_in_place=written)
    assert ran.returncode == 0 and len(jobs) == 1, (ran.stdout + ran.stderr)[-600:]
    command, metadata = jobs[0]["command"], jobs[0]["metadata"]
    if written is None:
        assert "--record-written-in-place" not in command
    else:
        assert command[command.index("--record-written-in-place") + 1] == written
    assert metadata["record_written_in_place"] == (written or "no")
    assert "written" not in str(metadata["options"])
    args = roi_matrix.parse_args(command[3:])
    produced = roi_matrix.output_policy_labels(
        args, [parse_policy_spec(policy) for policy in args.policies])
    assert metadata["expected_policy_labels"] == produced == [
        "ECG_REPLACEMENT_GOVERNED_FIRST" + ("_WRITTEN_IN_PLACE" if written == "on" else "")]


@pytest.mark.parametrize("settings", [
    {"algorithm_record_written_in_place": "yes"},
    {"algorithm_record_written_in_place": "on", "current_pr_baselines": None},
    {"algorithm_record_written_in_place": "on", "current_algorithms": True},
])
def test_the_flow_refuses_a_written_in_place_bit_it_cannot_deliver(tmp_path, settings):
    """A setting the flow would silently drop is refused when the job resolves.

    Without current_pr_baselines the PageRank command never receives the flag,
    and the algorithms kernels have no such option, so either would run the
    control while the stage asked for the treatment.
    """
    ran, _ = _flow_jobs(tmp_path, **settings)
    text = ran.stdout + ran.stderr
    assert ran.returncode != 0 and "invalid current written-in-place record selection" in text, (
        text[-600:])


def _backward_citation_graph():
    """A directed graph in which every vertex cites only older, distant vertices.

    Pull PageRank reads a vertex's contribution while visiting the vertices it
    cites, so each contribution line is last gathered in a pass well before the
    kernel's own in-place store to it, as most lines of cit-Patents are. The
    symmetric pressure512 fixture cannot show a store fetching back a line
    retired as DEAD, whatever the cache: each of its lines is last gathered at
    or after its own store.
    """
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import serialized_graph
    edges = [(vertex, vertex - step) for vertex in range(512)
             for step in (65, 97, 129, 193) if vertex >= step]
    return serialized_graph(512, edges, True, traversal="in")[0]


@pytest.mark.parametrize("rrpv", ["no", "on"])
def test_written_in_place_bit_reaches_the_pagerank_kernel_and_retires_nothing_as_dead(
        tmp_path, monkeypatch, rrpv):
    """Through the runner, the bit removes every DEAD-first victim and changes nothing else.

    Both the governed-first rule and its RRPV-ordered arm are run. Each control
    must retire lines DEAD first and fetch some of them back with the kernel's
    own store, or this fixture could not show the bit acting. Before the last
    pass the bit decodes nothing differently, so the first pass must match
    exactly, and the scores are unchanged.
    """
    import json
    import re
    if not (ROOT / "bench/bin_sim/pr").is_file():
        pytest.skip("current PageRank executable is not built")
    graph = _backward_citation_graph()
    results = {}
    for written in ("no", "on"):
        work = tmp_path / written
        work.mkdir()
        command, env = _pagerank_cell(work, monkeypatch, "no", governed="on", rrpv=rrpv,
                                      written=written, graph_bytes=graph)
        ran = _run_kernel(command, env)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-600:]
        checksum = re.search(r"\[ECG-PR-RESULT [^\]]*score_checksum=([0-9a-f]+)", text)
        assert checksum, text[-600:]
        results[written] = (json.loads(Path(env["CACHE_OUTPUT_JSON"]).read_text()),
                            checksum.group(1), text)
    control, treated = results["no"][0], results["on"][0]
    assert control["ecg_record_victim_dead_first"] > 0
    assert control["ecg_record_dead_first_refetch_write"] > 0, (
        "no DEAD-first victim was fetched back by the store, so the fixture cannot show the bit")
    assert treated["ecg_record_victim_decisions"] > 0
    assert treated["ecg_record_victim_dead_first"] == 0
    for access in ("write", "gather", "read"):
        assert treated[f"ecg_record_dead_first_refetch_{access}"] == 0
    assert "[ECG-PR-WRITTEN-IN-PLACE" not in results["no"][2]
    assert "[ECG-PR-WRITTEN-IN-PLACE property=contribution]" in results["on"][2]
    passes = [payload["kernel_census"]["pass_detail"] for payload in (control, treated)]
    assert len(passes[0]) == len(passes[1]) == 2
    assert passes[0][0] == passes[1][0], "the bit changed a pass it must not decode differently"
    assert results["on"][1] == results["no"][1], "the bit changed the PageRank result"


@pytest.mark.parametrize("policy,drop,message", [
    ("GRASP_PAPER", (), "requires a current ECG record mode"),
    ("GRASP_PAPER", ("ECG_CURRENT_PR_BASELINE",), "requires a current ECG record mode"),
    ("ECG:transport", (), "requires the replacement mechanism without prefetch"),
    ("ECG:prefetch", (), "requires the replacement mechanism without prefetch"),
    ("ECG:replacement-prefetch", (), "requires the replacement mechanism without prefetch"),
])
def test_the_pagerank_kernel_refuses_the_bit_where_it_cannot_hold(
        tmp_path, monkeypatch, policy, drop, message):
    """The kernel fails closed by itself, whatever reaches its environment.

    The runner never sets the bit for these cells, so it is planted directly:
    on the fixed baseline path, on the legacy path, and under the mechanisms
    whose use of the decoded bound has not been checked with it.
    """
    if not (ROOT / "bench/bin_sim/pr").is_file():
        pytest.skip("current PageRank executable is not built")
    command, env = _pagerank_cell(tmp_path, monkeypatch, "no", policy=policy)
    for name in drop:
        env.pop(name)
    env["ECG_RECORD_WRITTEN_IN_PLACE"] = "1"
    ran = _run_kernel(command, env)
    text = ran.stdout + ran.stderr
    assert ran.returncode != 0 and "ECG_RECORD_WRITTEN_IN_PLACE " + message in text, text[-600:]
