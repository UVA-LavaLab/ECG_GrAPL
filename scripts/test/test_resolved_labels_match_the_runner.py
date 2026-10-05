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
import csv
import getopt
import json
import re
import shlex
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
            options.record_expiry_clock, options.record_pressure_gate,
            options.record_rrpv_order, uninformed_base=options.record_uninformed_base,
            bound_compare=options.record_bound_compare, carrier_first=options.record_carrier_first,
            grasp_registration=options.grasp_registration, kernel_entry=options.kernel_entry,
            pass_scope=options.pass_scope)
    # PageRank: a separate kernel configured by environment, labelled by its
    # own branch of output_policy_labels.
    from types import SimpleNamespace
    args = SimpleNamespace(
        current_algorithms=False, current_pr_baselines=True, options="",
        record_base_policy=str(metadata.get("record_base_policy", "LRU")),
        grasp_registration=str(metadata.get("grasp_registration", "all")),
        kernel_entry=str(metadata.get("kernel_entry", "as-built")),
        record_governed_first=str(metadata.get("record_governed_first", "no")),
        record_store_bound=str(metadata.get("record_store_bound", "drop")),
        record_expiry_clock=str(metadata.get("record_expiry_clock", "progress")),
        record_pressure_gate=str(metadata.get("record_pressure_gate", "no")),
        record_rrpv_order=str(metadata.get("record_rrpv_order", "no")),
        record_written_in_place=str(metadata.get("record_written_in_place", "no")),
        record_uninformed_base=str(metadata.get("record_uninformed_base", "no")),
        record_bound_compare=str(metadata.get("record_bound_compare", "on")),
        record_carrier_first=str(metadata.get("record_carrier_first", "no")))
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
            str(metadata.get("record_pressure_gate", "no")),
            str(metadata.get("record_rrpv_order", "no")),
            str(metadata.get("record_written_in_place", "no")),
            str(metadata.get("record_uninformed_base", "no")),
            str(metadata.get("record_bound_compare", "on")),
            str(metadata.get("record_carrier_first", "no")),
            grasp_registration=str(metadata.get("grasp_registration", "all")),
            kernel_entry=str(metadata.get("kernel_entry", "as-built")),
            pass_scope=str(metadata.get("pass_scope", "off")))
        assert sorted(gate) == sorted(produced), (
            f"{job.get('job_id', '?')}: the completion and marker checks expect "
            f"{gate} but the runner emits {produced}; a correctly executed cell "
            "would be rejected after it had already run")


@pytest.mark.parametrize("profile", PROFILES)
def test_paired_arms_resolve_to_distinct_labels(tmp_path, profile):
    """A control and a treatment must never share one output label.

    Arms are compared within one kernel at one capacity, since a study repeats
    the same arms across capacities. The arm is every record option that splits
    a control from a treatment: keyed on governed-first alone, this skipped the
    pressure-gate study outright, whose arms all run governed-first.
    """
    jobs = _resolved_jobs(tmp_path, profile)
    by_cell: dict[tuple, list[tuple[tuple[str, str], list[str]]]] = {}
    for job in jobs:
        metadata = job.get("metadata", {})
        expected = list(metadata.get("expected_policy_labels") or [])
        if not expected:
            continue
        arm = (str(metadata.get("record_governed_first", "no")),
               str(metadata.get("record_pressure_gate", "no")),
               str(metadata.get("record_rrpv_order", "no")),
               str(metadata.get("record_written_in_place", "no")),
               str(metadata.get("record_uninformed_base", "no")),
               str(metadata.get("record_bound_compare", "on")),
               str(metadata.get("record_carrier_first", "no")),
               str(metadata.get("grasp_registration", "all")),
               str(metadata.get("kernel_entry", "as-built")),
               str(metadata.get("pass_scope", "off")))
        cell = (str(metadata.get("benchmark")),
                tuple(str(size) for size in metadata.get("l3_sizes") or []))
        by_cell.setdefault(cell, []).append((arm, expected))
    for cell, entries in by_cell.items():
        arms = {arm for arm, _ in entries}
        if len(arms) < 2:
            continue
        labels = [tuple(sorted(expected)) for _, expected in entries]
        assert len(set(labels)) == len(labels), (
            f"{cell}: paired arms share a label {labels}, so the control "
            "and the treatment would collide in one matrix")


def _pagerank_getopt():
    """The option string every PageRank binary hands getopt.

    Each of them builds a CLPageRank, which appends to CLApp's options, which
    append to CLBase's. Reading it from the header keeps this in step with the
    parser the binaries are compiled against.
    """
    header = (ROOT / "bench/include/external/gapbs/command_line.h").read_text()
    base = re.search(r'std::string get_args_ = "([^"]+)";', header).group(1)

    def appended(name):
        body = header[header.index(f"class {name} "):]
        return re.search(r'get_args_ \+= "([^"]+)";', body).group(1)

    return base + appended("CLApp") + appended("CLPageRank")


@pytest.mark.parametrize("profile", PROFILES)
def test_pagerank_options_parse_under_its_own_getopt(tmp_path, profile):
    """PageRank's options reach a GAPBS getopt, never the algorithms CLI.

    The flow once appended an algorithms-CLI flag to PageRank's options. The
    pr binary printed "invalid option -- '-'", then read the rest of the flag
    as a start vertex, which PageRank ignores, so the cell still completed with
    the right checksum and only its recorded options were wrong.
    """
    for source in ("bench/src_sim/pr.cc", "bench/src_gem5/pr.cc", "bench/src_sniper/pr.cc"):
        assert "CLPageRank cli(" in (ROOT / source).read_text(), source
    shortopts = _pagerank_getopt()
    jobs = [job for job in _resolved_jobs(tmp_path, profile)
            if job.get("metadata", {}).get("benchmark") == "pr"]
    if not jobs:
        pytest.skip(f"{profile} resolves no PageRank job")
    for job in jobs:
        tokens = shlex.split(str(job["metadata"].get("options") or ""))
        try:
            _, rest = getopt.getopt(tokens, shortopts)
        except getopt.GetoptError as error:
            pytest.fail(f"{job.get('job_id', '?')}: the pr binary cannot parse "
                        f"{tokens}: {error}")
        assert not rest, (
            f"{job.get('job_id', '?')}: {rest} would reach the pr binary as "
            "arguments it never reads")


def _algorithm_flow_jobs(tmp_path, benchmark, **settings):
    """Resolve one stage of `benchmark` through the real flow, running nothing.

    The algorithms twin of `_flow_jobs` in test_record_victim_arms_reach_the_kernel.py:
    the stage lives in a derived manifest under tmp_path, and `--list` builds each
    job's command, expected labels and metadata as a run would. A setting passed as
    None is left out of the stage.
    """
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    name = "pressure512.wsg" if benchmark == "sssp" else "pressure512.sg"
    graph = tmp_path / name
    graph.write_bytes(algorithm_outputs()[name][0])
    manifest = json.loads((ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stage = {
        "name": "contract_probe", "kind": "roi_matrix", "profiles": ["contract_probe"],
        "suite": "cache-sim", "graph_set": "contract_probe", "benchmarks": [benchmark],
        "policies": ["GRASP_PAPER"], "current_algorithms": True,
        "algorithm_workspace_bytes": 536870912, "ecg_record_bytes": 0,
        "cache_record_rss_mib": 4096, "cache_sim_omp_threads": 1,
        "l3_sizes": ["8MB"], "l3_ways": "16", "prefetcher": "none", "flowthrough": "off",
        "structure_prefetch_degree": 0, "policy_sharding_allowed": False,
        "out_subdir": "contract_probe"}
    stage.update(settings)
    manifest["stages"] = [{key: value for key, value in stage.items() if value is not None}]
    manifest["graph_sets"]["contract_probe"] = [
        {"name": "pressure512", "path": str(graph), "options_key": "contract_probe"}]
    manifest["benchmark_options"]["contract_probe"] = {
        **manifest["benchmark_options"]["file_current_algorithms"],
        "pr": "-f {graph_path} -o 0 -n 1 -i 2 -t 0"}
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest))
    run_dir = tmp_path / "run"
    ran = subprocess.run([
        sys.executable, "-I", str(ROOT / "scripts/experiments/ecg/flows/experiment_run.py"),
        "--manifest", str(path), "--profile", "contract_probe",
        "--run-dir", str(run_dir), "--lock-path", str(tmp_path / "lock"),
        "--no-build", "--list"], cwd=ROOT, capture_output=True, text=True,
        timeout=300, check=False)
    resolved = run_dir / "resolved_manifest.json"
    jobs = json.loads(resolved.read_text()).get("jobs", []) if ran.returncode == 0 else []
    return ran, jobs


# The fair-comparison contracts C1 (declared registration) and C2 (cold entry).
CONTRACTS = {"algorithm_grasp_registration": "declared", "algorithm_kernel_entry": "cold"}
PREPROCESS = {"bfs": "csr", "sssp": "traversal", "bc": "csr"}
# NEXT.md §by, methodology A: the rows of one filtered cell. The scoped GRASP row
# joins the best-of bar beside GRASP and charged P-OPT; uncharged P-OPT is the
# upper bound; the transport control and the primary run on the scoped base.
A_ROSTER = {
    "G0": {"policies": ["GRASP_PAPER"]},
    "Gs": {"policies": ["GRASP_PAPER"], "algorithm_pass_scope": "srrip"},
    "Ts": {"policies": ["ECG:transport"], "algorithm_record_base_policy": "GRASP_PAPER",
           "algorithm_pass_scope": "srrip"},
    "CFs": {"policies": ["ECG:replacement"], "algorithm_record_base_policy": "GRASP_PAPER",
            "algorithm_record_carrier_first": "on", "algorithm_record_rrpv_order": "on",
            "algorithm_pass_scope": "srrip"},
    "P": {"policies": ["POPT"], "popt_reserve_model": "size_correct",
          "popt_matrix_stream": "simulated"},
    "U": {"policies": ["POPT:UNCHARGED"]},
}


def _completion_status(tmp_path, job, labels):
    """`job_csv_status` on outputs carrying `labels`: the CSV check, then the marker check."""
    from scripts.experiments.ecg.flows import experiment_run
    resolved = experiment_run.Job(
        job_id=job["job_id"], stage=str(job["metadata"]["stage"]), kind="roi_matrix",
        command=list(job["command"]), out_dir=tmp_path / "out", log_path=tmp_path / "out.log",
        metadata=dict(job["metadata"]))
    resolved.out_dir.mkdir()
    with resolved.output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerows({"status": "ok", "policy_label": label} for label in labels)
    before = experiment_run.job_csv_status(resolved)
    metadata = resolved.metadata
    (resolved.out_dir / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True, "all_rows_ok": True, "policy_labels": list(labels),
        "l3_sizes": list(metadata.get("l3_sizes", [])),
        "threads": list(metadata.get("threads", [])),
        "structure_prefetch_degree": int(metadata.get("structure_prefetch_degree", 0)),
        "config_hash": str(metadata.get("config_hash", ""))}))
    return before, experiment_run.job_csv_status(resolved)


@pytest.mark.parametrize("benchmark", ["bfs", "sssp", "bc"])
@pytest.mark.parametrize("role", sorted(A_ROSTER))
def test_the_flow_carries_methodology_a_to_the_runner(tmp_path, benchmark, role):
    """Every row of a filtered cell under methodology A resolves as the runner runs it.

    C1, C2 and the pass scope reach the algorithms options and the job's metadata,
    and every label decider agrees: the recorded expectation, the runner's label,
    the completion check and the marker check.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    settings = {**CONTRACTS, **A_ROSTER[role]}
    if "algorithm_record_base_policy" in settings:
        settings["algorithm_record_preprocess"] = {benchmark: PREPROCESS[benchmark]}
    ran, jobs = _algorithm_flow_jobs(tmp_path, benchmark, **settings)
    assert ran.returncode == 0 and len(jobs) == 1, (ran.stdout + ran.stderr)[-600:]
    command, metadata = jobs[0]["command"], jobs[0]["metadata"]
    scope = settings.get("algorithm_pass_scope", "off")
    tokens = shlex.split(str(metadata["options"]))
    assert tokens[tokens.index("--grasp-registration") + 1] == "declared"
    assert tokens[tokens.index("--kernel-entry") + 1] == "cold"
    assert ("--pass-scope" in tokens) == (scope != "off")
    if scope != "off":
        assert tokens[tokens.index("--pass-scope") + 1] == scope
    assert (metadata["grasp_registration"], metadata["kernel_entry"], metadata["pass_scope"]) == (
        "declared", "cold", scope)
    args = roi_matrix.parse_args(command[3:])
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec(p) for p in args.policies])
    assert metadata["expected_policy_labels"] == produced, (metadata["expected_policy_labels"], produced)
    assert all("_GRASP_DECLARED" in label and "_COLD_ENTRY" in label and
               label.endswith("_PASS_SCOPED") == (scope != "off") for label in produced), produced
    before, after = _completion_status(tmp_path, jobs[0], produced)
    assert before == ("partial", "completion marker missing"), before
    assert "completion marker mismatch" not in after[1], after


@pytest.mark.parametrize("contracts", [False, True])
def test_the_flow_carries_the_contracts_to_the_pagerank_runner(tmp_path, contracts):
    """C1, C2 and C3 reach PageRank's runner as its own flags, never its GAPBS options.

    Without the settings the command is the one it was before, so every existing
    job keeps its configuration hash.
    """
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    settings = {"current_algorithms": None, "current_pr_baselines": True,
                "algorithm_workspace_bytes": None, "policies": ["ECG:transport", "GRASP_PAPER"]}
    if contracts:
        settings.update(CONTRACTS, algorithm_record_base_policy="GRASP_PAPER")
    ran, jobs = _algorithm_flow_jobs(tmp_path, "pr", **settings)
    assert ran.returncode == 0 and len(jobs) == 1, (ran.stdout + ran.stderr)[-600:]
    command, metadata = jobs[0]["command"], jobs[0]["metadata"]
    for flag, value in (("--record-base-policy", "GRASP_PAPER"),
                        ("--grasp-registration", "declared"), ("--kernel-entry", "cold")):
        if contracts:
            assert command[command.index(flag) + 1] == value, command
        else:
            assert flag not in command, command
        assert flag not in str(metadata["options"])
    assert (metadata["record_base_policy"], metadata["grasp_registration"],
            metadata["kernel_entry"], metadata["pass_scope"]) == (
        ("GRASP_PAPER", "declared", "cold", "off") if contracts else ("LRU", "all", "as-built", "off"))
    args = roi_matrix.parse_args(command[3:])
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec(p) for p in args.policies])
    assert metadata["expected_policy_labels"] == produced
    assert produced == (
        ["ECG_TRANSPORT_BASE_GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY", "GRASP_PAPER_GRASP_DECLARED_COLD_ENTRY"]
        if contracts else ["ECG_TRANSPORT", "GRASP_PAPER"])
    before, after = _completion_status(tmp_path, jobs[0], produced)
    assert before == ("partial", "completion marker missing"), before
    assert "completion marker mismatch" not in after[1], after


PAGERANK_STAGE = {"current_algorithms": None, "current_pr_baselines": True,
                  "algorithm_workspace_bytes": None}
REFUSED_SCOPE = ("invalid current pass-scope selection",)
REFUSED_ADMISSION = REFUSED_SCOPE + ("declared GRASP base on BFS, SSSP or BC",)


@pytest.mark.parametrize("benchmark,settings,words", [
    ("bfs", {"algorithm_pass_scope": "lru"}, REFUSED_SCOPE),
    ("bfs", {"algorithm_pass_scope": "srrip", "algorithm_grasp_registration": None}, REFUSED_ADMISSION),
    ("spmv", {"algorithm_pass_scope": "srrip"}, REFUSED_ADMISSION),
    ("cc", {"algorithm_pass_scope": "srrip"}, REFUSED_ADMISSION),
    ("bfs", {"algorithm_pass_scope": "srrip", "policies": ["POPT:UNCHARGED"]}, REFUSED_ADMISSION),
    ("sssp", {"algorithm_pass_scope": "srrip", "policies": ["GRASP_PAPER", "LRU"]}, REFUSED_ADMISSION),
    ("bc", {"algorithm_pass_scope": "srrip", "policies": ["ECG:transport"]}, REFUSED_ADMISSION),
    ("bfs", {"algorithm_pass_scope": "srrip", "policies": ["ECG:prefetch"],
             "algorithm_record_base_policy": "GRASP_PAPER"}, REFUSED_ADMISSION),
    ("pr", {**PAGERANK_STAGE, "algorithm_pass_scope": "srrip"}, REFUSED_SCOPE),
    ("pr", {**PAGERANK_STAGE, "algorithm_pass_scope": "off"}, REFUSED_SCOPE),
    ("bfs", {"algorithm_grasp_registration": "some"}, ("invalid current grasp-registration selection",)),
    ("bfs", {"algorithm_kernel_entry": "warm"}, ("invalid current kernel-entry selection",)),
    ("bfs", {"current_algorithms": None, "algorithm_grasp_registration": None},
     ("invalid current kernel-entry selection",)),
    ("pr", {**PAGERANK_STAGE, "algorithm_record_base_policy": "SRRIP"}, ("invalid current record base policy",)),
])
def test_the_flow_refuses_a_contract_or_scope_it_cannot_deliver(tmp_path, benchmark, settings, words):
    """Refused while resolving, in the flow's own terms, before any cell is spent."""
    ran, _ = _algorithm_flow_jobs(tmp_path, benchmark, **{**CONTRACTS, **settings})
    text = ran.stdout + ran.stderr
    assert ran.returncode != 0 and all(word in text for word in words), text[-600:]


@pytest.mark.parametrize("contract", sorted(CONTRACTS))
def test_each_contract_alone_reaches_every_label_decider(tmp_path, contract):
    """One contract without the other still changes the label, so no decider may
    take the job for the default shape."""
    from scripts.experiments.ecg import roi_matrix
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    ran, jobs = _algorithm_flow_jobs(tmp_path, "bfs", **{contract: CONTRACTS[contract]})
    assert ran.returncode == 0 and len(jobs) == 1, (ran.stdout + ran.stderr)[-600:]
    command, metadata = jobs[0]["command"], jobs[0]["metadata"]
    args = roi_matrix.parse_args(command[3:])
    produced = roi_matrix.output_policy_labels(args, [parse_policy_spec(p) for p in args.policies])
    assert metadata["expected_policy_labels"] == produced == [
        "GRASP_PAPER_GRASP_DECLARED" if contract == "algorithm_grasp_registration" else
        "GRASP_PAPER_COLD_ENTRY"], produced
    before, after = _completion_status(tmp_path, jobs[0], produced)
    assert before == ("partial", "completion marker missing"), before
    assert "completion marker mismatch" not in after[1], after
