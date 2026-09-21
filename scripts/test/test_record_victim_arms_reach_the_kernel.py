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
])
def test_requested_victim_arm_reaches_the_kernel_argv(tmp_path, option, value, flag):
    command = _captured_command(tmp_path, f"{option} {value}")
    assert command, "the cell never reached kernel command construction"
    assert flag in command, f"{flag} was parsed by the runner but dropped before the kernel"
    assert command[command.index(flag) + 1] == value


@pytest.mark.parametrize("option,default", [
    ("--record-governed-first", "no"),
    ("--record-store-bound", "drop"),
])
def test_default_victim_arm_is_not_emitted(tmp_path, option, default):
    command = _captured_command(tmp_path, f"{option} {default}")
    assert command, "the cell never reached kernel command construction"
    assert option not in command, (
        f"{option} must stay off the kernel argv at its default so existing "
        "commands and their configuration hashes are unchanged")


def test_victim_arm_labels_are_distinct_and_suffixed():
    from scripts.experiments.ecg import algorithm_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    spec = [parse_policy_spec("ECG:replacement")]
    base = algorithm_matrix.policy_labels(spec, base_policy="GRASP_PAPER")[0]
    governed = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", governed_first="on")[0]
    stored = algorithm_matrix.policy_labels(
        spec, base_policy="GRASP_PAPER", store_bound="keep")[0]
    assert base == "ECG_REPLACEMENT_BASE_GRASP_PAPER"
    assert governed == base + "_GOVERNED_FIRST"
    assert stored == base + "_STORE_BOUND"
    assert len({base, governed, stored}) == 3, (
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


@pytest.mark.parametrize("arm", ["no", "on"])
def test_completion_check_expects_the_label_the_runner_produces(tmp_path, arm):
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
        "all", "future", "enabled", "off", 1, arm, "drop")[0]

    csv_path = tmp_path / "roi_matrix.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": produced})

    status, detail = experiment_run.csv_status(
        csv_path, ["ECG:replacement"], "GRASP_PAPER", "off", "next", 6,
        "all", "future", "enabled", "off", 1, arm, "drop")
    assert status == "ok", (
        f"the completion check rejected the label the runner produces: {detail}")


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
