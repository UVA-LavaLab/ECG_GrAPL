"""Every resolved job must expect the label its runner will produce.

Six sites computed output labels for one option this session, and four of them
were found only by spending a simulation cell: a correctly executed arm was
rejected because some expectation still named the default label. The failure is
always the same shape — the flow expects one label, the runner emits another —
and it is entirely decidable before anything simulates.

This resolves a profile with `--list`, which runs no cells, and compares each
job's recorded expectation against the label the runner would produce from that
job's own options and policies.
"""
from pathlib import Path
import json
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

def _current_profiles():
    """Every profile the manifest calls current.

    Checking only the profiles under active development would miss a label
    defect introduced into a neighbouring study, which is exactly how this
    class of bug stays invisible until a run is spent on it.
    """
    import json
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    return list(manifest["current_profiles"])


PROFILES = _current_profiles()


def _resolved_jobs(tmp_path, profile):
    run_dir = tmp_path / profile
    result = subprocess.run([
        sys.executable, "-I",
        str(ROOT / "scripts/experiments/ecg/flows/experiment_run.py"),
        "--profile", profile, "--run-dir", str(run_dir), "--no-build", "--list",
    ], cwd=ROOT, capture_output=True, text=True, timeout=300)
    manifest = run_dir / "resolved_manifest.json"
    if not manifest.exists():
        pytest.skip(
            f"{profile} does not resolve in this environment: "
            f"{(result.stdout + result.stderr)[-200:]}")
    return json.loads(manifest.read_text()).get("jobs", [])


def _runner_labels(metadata):
    """What the runner will actually put in the policy_label column."""
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    policies = [str(policy) for policy in metadata.get("policies", [])]
    options_text = str(metadata.get("options") or "")
    # PageRank's options are GAPBS style ("-f graph ..."), which the algorithms
    # parser cannot read; its labels take the other branch entirely.
    if "--graph" in options_text:
        options = algorithm_matrix.parse_options(str(options_text))
        # Every label-affecting option must be forwarded. This mirrored the
        # runner with only two of the trailing options for a while, which left
        # the expiry clock unchecked and would have let a pressure-gate
        # mismatch through. Forward all of them.
        return algorithm_matrix.policy_labels(
            [parse_policy_spec(policy) for policy in policies],
            options.record_base_policy, options.window_observer,
            options.record_model, options.window_candidate_rrpv,
            options.grasp_scope, options.popt_rank_mode,
            options.frontier_gating, options.grasp_reference, options.queries,
            options.record_governed_first, options.record_store_bound,
            options.record_expiry_clock, options.record_pressure_gate)
    # PageRank: a separate kernel configured by environment, labelled by its
    # own branch of output_policy_labels.
    from types import SimpleNamespace
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True, options="",
        record_governed_first=str(metadata.get("record_governed_first", "no")),
        record_store_bound=str(metadata.get("record_store_bound", "drop")),
        record_expiry_clock=str(metadata.get("record_expiry_clock", "progress")),
        record_pressure_gate=str(metadata.get("record_pressure_gate", "no")))
    return roi_matrix.output_policy_labels(
        args, [parse_policy_spec(policy) for policy in policies])


@pytest.mark.parametrize("profile", PROFILES)
def test_resolved_expectations_match_what_the_runner_emits(tmp_path, profile):
    jobs = _resolved_jobs(tmp_path, profile)
    if not jobs:
        pytest.skip(f"{profile} resolved no jobs")
    for job in jobs:
        metadata = job.get("metadata", {})
        expected = list(metadata.get("expected_policy_labels") or [])
        if not expected:
            continue
        produced = _runner_labels(metadata)
        assert sorted(expected) == sorted(produced), (
            f"{job.get('job_id', '?')}: the flow expects {expected} but the "
            f"runner emits {produced}; a correctly executed cell would be "
            "rejected as a missing policy")
        # The completion check and the marker check compute their own
        # expectation from job metadata. Both were wrong in separate attempts
        # while the recorded expectation above was already right, so they must
        # be exercised here too rather than assumed to agree.
        from scripts.experiments.ecg.flows import experiment_run
        gate = experiment_run.expected_labels_for(
            [str(policy) for policy in metadata.get("policies", [])],
            str(metadata.get("record_base_policy", "LRU")),
            str(metadata.get("window_observer", "off")),
            str(metadata.get("record_model", "next")),
            int(metadata.get("window_candidate_rrpv", 6)),
            str(metadata.get("grasp_scope", "all")),
            str(metadata.get("popt_rank_mode", "future")),
            str(metadata.get("frontier_gating", "enabled")),
            str(metadata.get("grasp_reference", "off")),
            int(metadata.get("query_count", 1)),
            str(metadata.get("record_governed_first", "no")),
            str(metadata.get("record_store_bound", "drop")),
            str(metadata.get("record_expiry_clock", "progress")),
            str(metadata.get("record_pressure_gate", "no")))
        assert sorted(gate) == sorted(produced), (
            f"{job.get('job_id', '?')}: the completion and marker checks expect "
            f"{gate} but the runner emits {produced}; a correctly executed cell "
            "would be rejected after it had already run")


@pytest.mark.parametrize("profile", PROFILES)
def test_paired_arms_resolve_to_distinct_labels(tmp_path, profile):
    """A control and a treatment must never share one output label."""
    jobs = _resolved_jobs(tmp_path, profile)
    by_benchmark: dict[str, list[tuple[str, list[str]]]] = {}
    for job in jobs:
        metadata = job.get("metadata", {})
        expected = list(metadata.get("expected_policy_labels") or [])
        if not expected:
            continue
        arm = str(metadata.get("record_governed_first", "no"))
        by_benchmark.setdefault(str(metadata.get("benchmark")), []).append((arm, expected))
    for benchmark, entries in by_benchmark.items():
        arms = {arm for arm, _ in entries}
        if len(arms) < 2:
            continue
        labels = [tuple(sorted(expected)) for _, expected in entries]
        assert len(set(labels)) == len(labels), (
            f"{benchmark}: paired arms share a label {labels}, so the control "
            "and the treatment would collide in one matrix")
