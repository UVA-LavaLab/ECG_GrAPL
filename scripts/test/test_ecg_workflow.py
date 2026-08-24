import argparse
import csv
import hashlib
import importlib.util
import json
import os
import pytest
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]


def load_experiment_run_module():
    path = ROOT / "scripts/experiments/ecg/flows/experiment_run.py"
    spec = importlib.util.spec_from_file_location(
        "experiment_run_semantic_test", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules["experiment_run_semantic_test"] = module
    spec.loader.exec_module(module)
    return module


def output_descriptor(path: Path) -> dict:
    with path.open("rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    rows = None
    if path.suffix == ".csv":
        rows = max(len(path.read_text().splitlines()) - 1, 0)
    return {
        "sha256": digest,
        "size": path.stat().st_size,
        "rows": rows,
    }


@pytest.mark.parametrize(
    ("arguments", "message"),
    [
        (["--l3-ways", "0"], "associativity values must be positive"),
        (
            ["--l3-ways", "65", "--policies", "ECG:REUSE_PLAN"],
            "support at most 64 LLC ways",
        ),
        (
            ["--l3-sizes", "1000B"],
            "L3 size must contain an integral number of cache sets",
        ),
        (
            ["--l3-sizes", "3MB"],
            "L3 cache set count must be a power of two",
        ),
        (
            ["--line-size", "96"],
            "cache line size must be a positive power of two",
        ),
        (
            ["--reuse-plan-l3-ways", "65"],
            "--reuse-plan-l3-ways cannot exceed 64",
        ),
    ],
)
def test_roi_matrix_rejects_unsafe_cache_geometry(
        arguments, message):
    result = subprocess.run([
        sys.executable,
        str(ROOT / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "cache-sim",
        "--dry-run",
        *arguments,
    ], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0
    assert message in result.stderr


def test_controlled_profiles_fail_before_running_dirty_worktree(
        monkeypatch):
    module = load_experiment_run_module()
    args = SimpleNamespace(
        list=False, dry_run=False, status=False, check_graphs=False,
        profile=["controlled"])
    manifest = {
        "profile_controls": {
            "controlled": {"require_clean_worktree": True},
        },
    }
    monkeypatch.setattr(
        module.subprocess, "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            args[0], 0, stdout=" M tracked.cc\n", stderr=""))
    with pytest.raises(SystemExit, match="requires a clean worktree"):
        module.validate_clean_worktree(args, manifest)

    monkeypatch.setattr(
        module.subprocess, "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            args[0], 0,
            stdout="?? bench/include/gem5_sim/gem5/\n",
            stderr=""))
    module.validate_clean_worktree(args, manifest)


def test_full_roles_require_commit_bound_screen_authorization(
        tmp_path, monkeypatch):
    module = load_experiment_run_module()
    manifest_path = (
        ROOT / "scripts/experiments/ecg/experiment_manifest.json")
    screen_path = (
        ROOT / "scripts/experiments/ecg/configs/"
        "pagerank_literature_scale.json")
    gate = tmp_path / "screen_gate.json"
    payload = {
        "valid": True,
        "phase": "screen",
        "cell_count": 13,
        "row_count": 99,
        "stage_rows": {
            "60_gem5_proposal_reuse_bind_o3": 3,
            "90_gem5_literature_scale_i1": 48,
            "91_gem5_literature_scale_i8": 48,
        },
        "pagerank_gate": {
            "screen_valid": True,
            "screen_passes": True,
        },
        "run_dirs": ["/tmp/screen"],
        "git_head": "abc",
        "manifest_sha256": module.file_sha256(manifest_path),
        "screen_config_sha256": module.file_sha256(screen_path),
    }
    gate.write_text(json.dumps(payload))
    args = SimpleNamespace(
        dry_run=False, list=False, check_graphs=False,
        screen_gate=str(gate))
    job = module.Job(
        job_id="full", stage="92_cache_sim_literature_scale_wide16",
        kind="roi_matrix", command=[], out_dir=tmp_path / "out",
        log_path=tmp_path / "log")
    monkeypatch.setattr(module, "current_git_head", lambda: "abc")
    monkeypatch.setattr(
        module, "recompute_screen_gate",
        lambda payload, manifest_path, screen_path: dict(payload))
    authorization = module.validate_screen_authorization(
        args, [job], manifest_path)
    assert authorization["sha256"] == module.file_sha256(gate)
    assert authorization["git_head"] == "abc"

    payload["pagerank_gate"]["screen_passes"] = False
    gate.write_text(json.dumps(payload))
    with pytest.raises(SystemExit, match="stale or not GO"):
        module.validate_screen_authorization(args, [job], manifest_path)


def test_cross_stage_pr_semantic_gate_fails_on_checksum_drift(tmp_path):
    experiment_run = load_experiment_run_module()
    jobs = []
    for stage, checksum in (
            ("50_fused_compact_4b", "abc"),
            ("51_fused_software_4b", "abc"),
            ("52_fused_wide_8b", "abc")):
        csv_path = tmp_path / f"{stage}.csv"
        csv_path.write_text(
            "status,pr_iterations,pr_semantic_edges,pr_score_checksum\n"
            f"ok,1,100,{checksum}\n")
        jobs.append(SimpleNamespace(
            kind="roi_matrix", stage=stage, output_csv=csv_path,
            metadata={"graph": "g"}))
    assert experiment_run.validate_cross_stage_pr_receipts(jobs) == (
        True, "matched")

    jobs[-1].output_csv.write_text(
        "status,pr_iterations,pr_semantic_edges,pr_score_checksum\n"
        "ok,1,100,def\n")
    ok, detail = experiment_run.validate_cross_stage_pr_receipts(jobs)
    assert not ok
    assert "cross-stage PR semantic mismatch" in detail


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_reuse_plan_policy_aliases_are_first_class(monkeypatch):
    module = load_module(
        "roi_matrix_policy_config",
        ROOT / "scripts/experiments/ecg/roi_matrix.py",
    )
    monkeypatch.delenv("ECG_REUSE_PLAN_DEPTH", raising=False)
    monkeypatch.delenv("ECG_FLOWTHROUGH", raising=False)

    reuse_plan = module.parse_policy_spec("ECG:REUSE_PLAN")
    assert reuse_plan.label == "ECG_REUSE_PLAN"
    assert reuse_plan.ecg_mode == "ECG_GRASP_POPT"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        assert module.ecg_transport_for(
            reuse_plan, benchmark) == module.EcgTransport(
                2, False, False, False, True)

    flowthrough = module.parse_policy_spec("ECG:REUSE_PLAN_FLOWTHROUGH")
    assert flowthrough.label == "ECG_REUSE_PLAN_FLOWTHROUGH"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        assert module.ecg_transport_for(
            flowthrough, benchmark) == module.EcgTransport(
                2, True, False, False, True)
    k1 = module.parse_policy_spec("ECG:REUSE_PLAN_1")
    k1_ss = module.parse_policy_spec("ECG:REUSE_PLAN_1_FLOWTHROUGH")
    assert module.ecg_transport_for(
        k1, "pr") == module.EcgTransport(0, False, False, False, True)
    assert module.ecg_transport_for(
        k1_ss, "pr") == module.EcgTransport(0, True, False, False, True)
    k1_bfs_env = {}
    module.apply_ecg_transport_env(
        k1_bfs_env, module.ecg_transport_for(k1, "bfs"))
    assert k1_bfs_env["ECG_EDGE_MASKS"] == "1"
    assert "ECG_REUSE_PLAN_DEPTH" not in k1_bfs_env
    online = module.parse_policy_spec("ECG:REUSE_PLAN_ONLINE")
    assert online.label == "ECG_REUSE_PLAN_ONLINE"
    online_transport = module.ecg_transport_for(online, "pr")
    assert online_transport == module.EcgTransport(
        2, False, False, True, True)
    lru_ss = module.parse_policy_spec("ECG:REUSE_PLAN_LRU_FLOWTHROUGH")
    assert lru_ss.label == "ECG_REUSE_PLAN_LRU_FLOWTHROUGH"
    assert lru_ss.ecg_variant == "lru_only"
    assert module.ecg_transport_for(
        lru_ss, "pr") == module.EcgTransport(
            2, True, False, False, True)
    for label, variant in (
            ("ECG:REUSE_PLAN_GRASP_FLOWTHROUGH", "grasp_only"),
            ("ECG:REUSE_PLAN_EPOCH_FLOWTHROUGH", "epoch_first"),
            ("ECG:REUSE_PLAN_RECORD_LRU_FLOWTHROUGH", "record_lru"),
            ("ECG:REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH", "rrip_no_epoch"),
            ("ECG:REUSE_PLAN_RRIP_FLOWTHROUGH", "rrip_first"),
            ("ECG:REUSE_PLAN_DEGREE_FLOWTHROUGH", "degree_first"),
            ("ECG:REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH", "shortcircuit"),
            ("ECG:REUSE_PLAN_LRU_FLOWTHROUGH", "lru_only")):
        spec = module.parse_policy_spec(label)
        assert spec.ecg_variant == variant
        assert module.ecg_transport_for(
            spec, "pr") == module.EcgTransport(
                2, True, False, False, True)
    admission = module.parse_policy_spec(
        "ECG:REUSE_PLAN_ADMISSION_FLOWTHROUGH")
    assert admission.ecg_variant == "rrip_first"
    assert admission.ecg_reuse_admission is True
    assert admission.ecg_flowthrough is True
    monkeypatch.setenv("ECG_VARIANT", "grasp_only")
    assert module.effective_ecg_variant(
        argparse.Namespace(benchmark="pr"), 2, reuse_plan) == "epoch_first"
    monkeypatch.setenv("ECG_REUSE_PLAN_DELIVERY_TRACE", "32")
    run_env = {}
    module.apply_ecg_transport_env(
        run_env, module.ecg_transport_for(reuse_plan, "pr"))
    assert "ECG_REUSE_PLAN_DELIVERY_TRACE" not in run_env
    assert run_env["ECG_EDGE_MASKS"] == "1"
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        json.dumps({"ECG_FLOWTHROUGH_TRACE": "8"}))
    traced_env = dict(os.environ)
    module.scrub_cell_mechanism_env(traced_env)
    module.apply_explicit_cell_mechanism_env(traced_env, flowthrough)
    module.apply_ecg_transport_env(
        traced_env, module.ecg_transport_for(flowthrough, "pr"))
    assert traced_env["ECG_FLOWTHROUGH_TRACE"] == "8"
    online_env = {}
    module.apply_ecg_transport_env(online_env, online_transport)
    assert online_env["ECG_SET_DUELING"] == "1"

    adaptive_ss = module.parse_policy_spec(
        "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH")
    adaptive_transport = module.ecg_transport_for(adaptive_ss, "sssp")
    assert adaptive_ss.label == "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH"
    assert adaptive_transport.flowthrough
    assert adaptive_transport.flowthrough_adaptive
    adaptive_env = {}
    module.apply_ecg_transport_env(adaptive_env, adaptive_transport)
    assert adaptive_env["ECG_FLOWTHROUGH"] == "1"
    assert adaptive_env["ECG_FLOWTHROUGH_ADAPTIVE"] == "1"
    static_adaptive_ss = module.parse_policy_spec(
        "ECG:REUSE_PLAN_ADAPTIVE_FLOWTHROUGH")
    static_adaptive_transport = module.ecg_transport_for(
        static_adaptive_ss, "sssp")
    assert static_adaptive_ss.label == "ECG_REUSE_PLAN_ADAPTIVE_FLOWTHROUGH"
    assert static_adaptive_transport.flowthrough_adaptive
    assert not static_adaptive_transport.set_dueling

    monkeypatch.setenv("ECG_FLOWTHROUGH", "1")
    monkeypatch.setenv("ECG_FLOWTHROUGH_ADAPTIVE", "1")
    env_driven = module.ecg_transport_for(
        module.parse_policy_spec("ECG:ECG_GRASP_POPT"), "bfs")
    assert env_driven.flowthrough
    assert env_driven.flowthrough_adaptive

    baseline_env = {
        "ECG_REUSE_PLAN_DEPTH": "2",
        "ECG_EDGE_MASKS": "1",
        "ECG_FLOWTHROUGH": "1",
    }
    module.apply_ecg_transport_env(
        baseline_env, module.EcgTransport())
    assert "ECG_REUSE_PLAN_DEPTH" not in baseline_env
    assert "ECG_EDGE_MASKS" not in baseline_env
    assert "ECG_FLOWTHROUGH" not in baseline_env
    contaminated = {
        "ECG_PREFETCH_MODE": "6",
        "ECG_MODE": "ECG_GRASP_POPT",
        "GEM5_FORCE_ECG_PLAN_LOAD": "1",
        "SNIPER_ECG_FUSED_REUSE_PLAN": "1",
        "GRASP_HOT_FRACTION": "0.40",
        "POPT_DUAL_REREF": "1",
        "ECG_DEBUG": "1",
    }
    monkeypatch.setenv("ECG_DEBUG", "1")
    module.scrub_cell_mechanism_env(contaminated)
    assert contaminated == {"ECG_DEBUG": "1"}
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        json.dumps({
            "ECG_EDGE_MASK_AMPLIFY": "3",
            "ECG_REUSE_PLAN_DEPTH": "4",
            "GRASP_HOT_FRACTION": "0.25",
        }),
    )
    explicit_env = {}
    module.apply_explicit_cell_mechanism_env(explicit_env, reuse_plan)
    assert explicit_env == {
        "ECG_EDGE_MASK_AMPLIFY": "3",
        "ECG_REUSE_PLAN_DEPTH": "4",
        "GRASP_HOT_FRACTION": "0.25",
    }
    module.apply_ecg_transport_env(
        explicit_env, module.ecg_transport_for(reuse_plan, "pr"))
    assert explicit_env["ECG_EDGE_MASK_AMPLIFY"] == "3"
    assert explicit_env["ECG_REUSE_PLAN_DEPTH"] == "2"
    assert explicit_env["GRASP_HOT_FRACTION"] == "0.25"
    baseline = module.parse_policy_spec("LRU")
    baseline_env = {}
    module.apply_explicit_cell_mechanism_env(baseline_env, baseline)
    assert baseline_env == {}
    grasp_env = {}
    module.apply_explicit_cell_mechanism_env(
        grasp_env, module.parse_policy_spec("GRASP"))
    assert grasp_env == {"GRASP_HOT_FRACTION": "0.25"}


def test_flowthrough_manifest_is_complete():
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    assert "flowthrough_sniper_realgraph" in manifest["profiles"]
    assert "ecg_cache_sim_factorial" in manifest["profiles"]
    assert "ecg_3sim_allalg_smoke" in manifest["profiles"]
    assert "ecg_3sim_realgraph_allalg" in manifest["profiles"]
    assert "ecg_3sim_realgraph_allalg_1b" in manifest["profiles"]
    assert "ecg_3sim_sampled_allalg" in manifest["profiles"]
    assert "ecg_sniper_sampled_pr_streamengine" in manifest["profiles"]
    assert "ecg_sniper_realgraph_warm_probe" in manifest["profiles"]
    assert "ecg_sniper_realgraph_semantic_probe" in manifest["profiles"]
    assert "ecg_sniper_realgraph_600m" in manifest["profiles"]
    assert "ecg_sniper_semantic_gate" in manifest["profiles"]
    semantic_stage = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "12b_sniper_semantic_gate"
    )
    assert semantic_stage["sniper_semantic_edge_limit"] > 0
    assert not semantic_stage.get("sniper_roi_icount")
    equal_capacity = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "19a_cache_sim_equal_capacity_16"
    )
    assert not equal_capacity.get("reuse_plan_l3_ways")
    semantic_realgraph = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "34b_sniper_realgraph_semantic_probe"
    )
    assert semantic_realgraph["policies"] == ["LRU", "ECG:REUSE_PLAN"]
    assert semantic_realgraph["ecg_isa_variant"] == "computed"
    assert semantic_realgraph["sniper_semantic_edge_limit"] == 100000
    assert not semantic_realgraph.get("sniper_roi_icount")
    assert "ecg_replacement_baseline" in manifest["profiles"]
    assert "ecg_equal_capacity_16" in manifest["profiles"]
    assert "ecg_equal_area_15" in manifest["profiles"]
    assert "ecg_equal_area_14" in manifest["profiles"]
    assert "ecg_preliminary_5alg_3sim" in manifest["profiles"]
    assert "ecg_preliminary_5alg_stride" in manifest["profiles"]
    assert "ecg_online_dueling" in manifest["profiles"]
    assert "ecg_flowthrough_generality" in manifest["profiles"]
    assert "gem5_flowthrough_mechanism" in manifest["profiles"]
    assert "sniper_flowthrough_mechanism" in manifest["profiles"]
    stage = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "40_sniper_flowthrough_realgraph")
    assert stage["policies"] == [
        "LRU", "SRRIP", "GRASP", "POPT",
        "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_ONLINE",
        "ECG:REUSE_PLAN_FLOWTHROUGH", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ]
    assert stage["prefetcher"] == "STRIDE"
    assert stage["popt_reserve_model"] == "size_correct"
    graph = manifest["graph_sets"]["web_google_flowthrough"][0]
    assert graph["structure_prefetch_degree"] == 8
    assert "sniper_roi_icount" not in stage
    assert stage["sniper_workload"] == "sg_kernel"
    assert stage["sniper_frontend"] == "sift"
    assert stage["require_sniper_aslr_disable"] is True
    assert "env" not in stage
    factorial = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "20_cache_sim_flowthrough_factorial")
    assert factorial["policies"] == [
        "LRU", "SRRIP", "GRASP", "POPT:UNCHARGED", "POPT",
        "ECG:REUSE_PLAN_1", "ECG:REUSE_PLAN_1_FLOWTHROUGH",
        "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_FLOWTHROUGH",
        "ECG:REUSE_PLAN_ONLINE", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
        "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
    ]
    generality = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "21_cache_sim_flowthrough_generality")
    assert generality["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
    assert generality["policies"] == [
        "LRU", "SRRIP", "GRASP", "POPT:UNCHARGED", "POPT",
        "ECG:REUSE_PLAN_ONLINE", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
        "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
    ]
    assert generality["prefetcher"] == "none"
    assert generality["ecg_charged"] == 1
    replacement = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "19_cache_sim_replacement_baseline")
    assert replacement["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
    all_kernel_options = manifest["benchmark_options"]["file_all_kernels_dbg"]
    assert set(all_kernel_options) == {"pr", "bfs", "sssp", "bc", "cc"}
    assert replacement["prefetcher"] == "none"
    assert replacement["ecg_charged"] == 0
    assert manifest["defaults"]["ecg_epoch_pack_bits"] == 64
    assert manifest["defaults"]["require_cache_sim_aslr_disable"] is True
    assert replacement["policies"][-1] == "ECG:REUSE_PLAN_ONLINE"
    smoke_stages = [
        stage for stage in manifest["stages"]
        if "ecg_3sim_allalg_smoke" in stage.get("profiles", [])]
    assert {stage["suite"] for stage in smoke_stages} == {
        "cache-sim", "gem5", "sniper"}
    assert all(
        stage["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
        for stage in smoke_stages)
    assert all(len(stage["policies"]) == 8 for stage in smoke_stages)
    smoke_options = manifest["benchmark_options"]["synthetic_kron12_all"]
    assert smoke_options["bfs"].endswith("-r 0")
    assert smoke_options["sssp"].endswith("-r 0")
    realgraph_stages = [
        stage for stage in manifest["stages"]
        if "ecg_3sim_realgraph_allalg" in stage.get("profiles", [])]
    assert {stage["suite"] for stage in realgraph_stages} == {
        "cache-sim", "gem5", "sniper"}
    assert all(stage["graph_set"] == "factorial_graphs"
               for stage in realgraph_stages)
    assert all(
        stage["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
        for stage in realgraph_stages)
    assert all(len(stage["policies"]) == 8 for stage in realgraph_stages)
    assert all(stage["prefetcher"] == "none" for stage in realgraph_stages)
    capped_stages = [
        stage for stage in manifest["stages"]
        if "ecg_3sim_realgraph_allalg_1b" in stage.get("profiles", [])]
    assert {stage["suite"] for stage in capped_stages} == {
        "cache-sim", "gem5", "sniper"}
    capped_gem5 = next(
        stage for stage in capped_stages if stage["suite"] == "gem5")
    capped_sniper = next(
        stage for stage in capped_stages if stage["suite"] == "sniper")
    assert capped_gem5["gem5_max_insts"] == 1_000_000_000
    assert capped_sniper["sniper_roi_icount"] == 1_000_000_000
    sampled_stages = [
        stage for stage in manifest["stages"]
        if "ecg_3sim_sampled_allalg" in stage.get("profiles", [])]
    assert {stage["suite"] for stage in sampled_stages} == {
        "cache-sim", "gem5", "sniper"}
    assert all(stage["graph_set"] == "realgraph_samples"
               for stage in sampled_stages)
    sampled_timing = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "33_sniper_sampled_pr_streamengine")
    assert sampled_timing["suite"] == "sniper"
    assert sampled_timing["graph_set"] == "realgraph_samples"
    assert sampled_timing["benchmarks"] == ["pr"]
    assert sampled_timing["policies"] == [
        "GRASP", "POPT", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH"]
    assert sampled_timing["prefetcher"] == "none"
    assert sampled_timing["sniper_require_fused_receipts"] is True
    assert all("gem5_max_insts" not in stage for stage in sampled_stages)
    assert all("sniper_roi_icount" not in stage for stage in sampled_stages)
    sampled_sniper = next(
        stage for stage in sampled_stages if stage["suite"] == "sniper")
    assert sampled_sniper["timeout_sniper"] == 43_200
    sampled_options = manifest["benchmark_options"]["file_all_kernels_root0"]
    assert sampled_options["bfs"].endswith("-r 0")
    assert sampled_options["sssp"].endswith("-r 0")
    sniper_600m = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "32_sniper_realgraph_600m")
    assert sniper_600m["graph_set"] == "factorial_graphs"
    assert sniper_600m["sniper_roi_icount"] == 600_000_000
    assert sniper_600m["sniper_frontend"] == "sift"
    assert "sniper_require_fused_receipts" not in sniper_600m
    assert "blocked_reason" not in sniper_600m
    assert sniper_600m["prerequisite_profile"] == \
        "ecg_sniper_realgraph_warm_probe"
    warm_probe = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "34_sniper_realgraph_warm_probe")
    assert warm_probe["graph_set"] == "web_google_flowthrough"
    assert warm_probe["benchmarks"] == ["pr"]
    assert warm_probe["policies"] == ["LRU", "ECG:REUSE_PLAN"]
    assert warm_probe["sniper_roi_icount"] == 100_000
    assert warm_probe["sniper_frontend"] == "sift"
    assert "sniper_require_fused_receipts" not in warm_probe
    preliminary = [
        stage for stage in manifest["stages"]
        if stage["name"] in {
            "10_cache_sim_preliminary_5alg",
            "11_gem5_preliminary_5alg",
            "12_sniper_preliminary_5alg",
        }
    ]
    assert len(preliminary) == 3
    for stage in preliminary:
        assert stage["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
        assert stage["policies"] == [
            "LRU", "SRRIP", "GRASP", "POPT",
            "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_ONLINE",
        ]
        assert stage["graph_set"] == "synthetic_kron15_all"
        assert stage["prefetcher"] == "none"
        assert stage["ecg_charged"] == 1
    stride_preliminary = [
        stage for stage in manifest["stages"]
        if stage["name"] in {
            "13_cache_sim_preliminary_5alg_stride",
            "14_gem5_preliminary_5alg_stride",
            "15_sniper_preliminary_5alg_stride",
        }
    ]
    assert len(stride_preliminary) == 3
    for stage in stride_preliminary:
        assert stage["benchmarks"] == ["pr", "bfs", "sssp", "bc", "cc"]
        assert stage["policies"] == [
            "LRU", "SRRIP", "GRASP", "POPT",
            "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_ONLINE",
        ]
        assert stage["graph_set"] == "synthetic_kron15_all_stride"
        assert stage["prefetcher"] == "STRIDE"
    stride_graph = manifest["graph_sets"]["synthetic_kron15_all_stride"][0]
    assert stride_graph["structure_prefetch_degree"] == 8
    gem5_mechanism = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "30_gem5_flowthrough_mechanism")
    assert gem5_mechanism["env"]["GEM5_KERNEL_SUFFIX"] == "_riscv_m5ops"
    sniper_mechanism = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "31_sniper_flowthrough_mechanism")
    assert sniper_mechanism["sniper_workload"] == "sg_kernel"
    assert sniper_mechanism["sniper_require_fused_receipts"] is True


def test_sniper_sg_kernel_supports_synthetic_profiles():
    source = (
        ROOT / "bench/src_sniper/sg_kernel.cc"
    ).read_text()
    assert "options.scale = std::atoi" in source
    assert '"-g", std::to_string(opt.scale)' in source
    assert "requires -f graph.sg or -g scale" in source

    module = load_module(
        "roi_matrix_synthetic_sg",
        ROOT / "scripts/experiments/ecg/roi_matrix.py",
    )
    binary, options = module.sniper_binary_and_options(argparse.Namespace(
        sniper_workload="sg_kernel",
        benchmark="pr",
        options="-g 16 -k 16 -o 5 -n 1 -i 1",
    ))
    assert binary.name == "sg_kernel"
    assert options[:2] == ["--benchmark", "pr"]
    assert "-g" in options


def test_public_documents_use_the_expected_reading_flow():
    """The public flow is README, design wiki, methodology, reproduction."""
    root_readme = (ROOT / "README.md").read_text()
    wiki = (ROOT / "wiki/ReusePlan-FlowThrough.md").read_text()
    methodology = (ROOT / "wiki/Evaluation-Methodology.md").read_text()
    reproduction = (ROOT / "wiki/Reproduction.md").read_text()

    tracked = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True,
        check=True).stdout.splitlines()
    assert not any(path.startswith("research/") for path in tracked)
    assert not (
        ROOT / "scripts/experiments/ecg/analysis/claim_gate.py").exists()
    assert not (ROOT / "scripts/test/test_claim_gate.py").exists()

    # Simulator roles remain scientifically explicit.
    method_flat = " ".join(methodology.split())
    assert "gem5 O3 execution time is used for architectural speedup" in (
        method_flat)
    assert "cache_sim does not model cycles or instructions" in method_flat
    assert "time is not used as a ReuseBind speedup" in method_flat
    assert "popt_target_time_charged=0" in methodology
    assert "optimistic P-OPT bound" in methodology

    # README is concise; the wiki owns the illustrated explanation.
    assert len(root_readme.splitlines()) < 90
    assert "Illustrated design guide" in root_readme
    assert "wiki/ReusePlan-FlowThrough.md" in root_readme
    assert "wiki/Evaluation-Methodology.md" in root_readme
    assert "wiki/Reproduction.md" in root_readme
    assert "--profile" not in root_readme
    for figure in (
            "assets/reuse-plan-overview.svg",
            "assets/reuse-plan-record.svg",
            "assets/reuse-plan-example.svg",
            "assets/reuse-plan-cpu-pipeline.svg",
            "assets/riscv-instruction-family.svg",
            "assets/flowthrough-path.svg"):
        assert figure in wiki
        assert (ROOT / "wiki" / figure).is_file()
    assert "values in the following example are illustrative" in wiki
    assert "```mermaid" not in wiki

    # Public documents use direct technical language rather than internal
    # review vocabulary.
    for doc in (methodology, reproduction, root_readme, wiki):
        lowered = doc.lower()
        for term in ("result: stop", "current stop"):
            assert term not in lowered
        assert "PR uses `epoch_first`" not in doc
        assert "PR uses epoch-first eviction" not in doc

    # Superseded experiment names do not appear in public documents.
    for doc in (methodology, reproduction, root_readme, wiki):
        assert "pr_screen_v1" not in doc and "pr_screen_v2" not in doc
        assert "proposal_reuse_bind_sota_pr_screen_v1" not in doc
        assert "proposal_reuse_bind_sota_pr_screen_v2" not in doc


def test_reuse_plan_cache_sim_paths_do_not_build_popt_matrix():
    helper = (
        ROOT / "bench/include/cache_sim/graph_sim.h"
    ).read_text()
    assert "GraphSimMatrixFreeReusePlan" in helper
    for relative in (
        "bench/src_sim/pr.cc",
        "bench/src_sim/bfs.cc",
    ):
        source = (ROOT / relative).read_text()
        assert "GraphSimMatrixFreeReusePlan" in source, relative
    bfs = (ROOT / "bench/src_sim/bfs.cc").read_text()
    # One delivery site per direction (top-down and bottom-up), both routed
    # through the shared metadata helper rather than duplicating the chain.
    assert bfs.count("SIM_ECG_EDGE(") == 2
    assert bfs.count("::ecg_metadata::configure(") == 2
    assert bfs.index("cache.resetStats();") < bfs.index(
        "SlidingQueue<NodeID> queue")
    pr = (ROOT / "bench/src_sim/pr.cc").read_text()
    assert pr.index("cache.resetStats();") < pr.index(
        "for (int iter = 0; iter < max_iters; iter++)")
    assert "Identical post-metadata warm replay" in pr
    assert "volatile ScoreT* warm_scores" in (
        ROOT / "bench/src_gem5/pr.cc").read_text()
    assert "volatile ScoreT* warm_scores" in (
        ROOT / "bench/src_sniper/sg_kernel.cc").read_text()
    for relative in (
        "bench/src_sim/pr.cc",
        "bench/src_sim/pr_spmv.cc",
        "bench/src_sim/bfs.cc",
        "bench/src_sim/bc.cc",
        "bench/src_sim/cc.cc",
        "bench/src_sim/cc_sv.cc",
        "bench/src_sim/sssp.cc",
    ):
        assert "GRAPH_SIM_PROPERTY_ALIGNMENT" in (
            ROOT / relative).read_text(), relative


def test_gem5_sidecar_scope_preserves_non_pr_reuse_plan_paths():
    runner = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py"
    ).read_text()
    assert (
        'if is_reuse_plan_ecg:\n'
        '        env["GEM5_ECG_ISA_VARIANT"]' in runner)
    assert (
        'if is_reuse_plan_ecg and args.benchmark == "pr":\n'
        "        if (" in runner)
    assert "ECG_RECORD_VARIABLE_WIDTH=1 requires an explicit" in runner


def test_flowthrough_profile_and_slurm_shards(tmp_path):
    run_dir = tmp_path / "dryrun"
    listed = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "flowthrough_sniper_realgraph",
            "--run-dir", str(run_dir),
            "--list", "--dry-run", "--no-build",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert listed.returncode == 0, listed.stdout + listed.stderr
    for policy in (
        "LRU", "SRRIP", "GRASP", "POPT",
        "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_ONLINE",
        "ECG:REUSE_PLAN_FLOWTHROUGH", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ):
        assert policy in listed.stdout
    assert "--sniper-roi-icount" not in listed.stdout
    assert "--structure-prefetch-degree 8" in listed.stdout
    assert "--ecg-isa-variant indexed" in listed.stdout
    assert "--popt-reserve-model size_correct" in listed.stdout
    assert "--require-sniper-aslr-disable" in listed.stdout

    blocked = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "flowthrough_sniper_realgraph",
            "--run-dir", str(tmp_path / "blocked"),
            "--allow-missing-graphs", "--no-build",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert blocked.returncode != 0
    assert "is blocked" in blocked.stderr

    shards = tmp_path / "shards.tsv"
    generated = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/slurm/make_slurm_shards.py",
            "--profile", "flowthrough_sniper_realgraph",
            "--run-tag", "ecg_successor_test",
            "--out", str(shards),
            "--allow-missing-graphs",
            "--allow-blocked",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert generated.returncode == 0, generated.stdout + generated.stderr
    rows = [line.split("\t") for line in shards.read_text().splitlines()]
    assert len(rows) == 8
    assert [row[4] for row in rows] == [
        "LRU", "SRRIP", "GRASP", "POPT",
        "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_ONLINE",
        "ECG:REUSE_PLAN_FLOWTHROUGH", "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ]
    sbatch = (
        ROOT / "scripts/experiments/ecg/slurm/slurm_experiment_shard.sbatch"
    ).read_text()
    assert "${profile}_${safe_stage}_${graph}_${benchmark}_${safe_policy}" in sbatch


def test_capped_realgraph_profile_forwards_instruction_budgets(tmp_path):
    listed = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/experiment_run.py",
            "--profile", "ecg_3sim_realgraph_allalg_1b",
            "--run-dir", str(tmp_path / "capped"),
            "--allow-missing-graphs",
            "--allow-missing-runtime-inputs",
            "--list", "--dry-run", "--no-build",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert listed.returncode == 0, listed.stdout + listed.stderr
    assert "--gem5-max-insts 1000000000" in listed.stdout
    assert "--sniper-roi-icount 1000000000" in listed.stdout


def test_aggregate_results_uses_canonical_runner():
    pipeline = (
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py"
    ).read_text()
    assert 'ECG_DIR / "flows" / "experiment_run.py"' in pipeline
    assert "(relative_summary if relative else summary)[:24]" not in pipeline
    assert "stale.unlink()" in pipeline
    assert (
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py"
    ).is_file()
    runner = (
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py"
    ).read_text()
    assert "GRAPHBREW_EXPLICIT_CELL_ENV" in runner
    roi_matrix = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py"
    ).read_text()
    assert 'cmd.insert(cmd.index("--roi") + 1, "--no-cache-warming")' in roi_matrix
    assert "args.sniper_require_fused_receipts" in roi_matrix
    for removed_wrapper in (
        "experiment_run.py",
        "aggregate_results.py",
        "make_slurm_shards.py",
    ):
        assert not (
            ROOT / "scripts/experiments/ecg" / removed_wrapper
        ).exists()


def test_experiment_tools_use_active_python_by_default():
    runner = load_module(
        "experiment_run_python_selection",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    aggregate = load_module(
        "aggregate_results_python_selection",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    default_args = SimpleNamespace(require_pinned_python=False)
    pinned_args = SimpleNamespace(require_pinned_python=True)
    assert runner.execution_python(default_args) == Path(sys.executable)
    assert aggregate.execution_python(default_args) == Path(sys.executable)
    assert runner.execution_python(pinned_args) == runner.REFERENCE_PYTHON
    assert aggregate.execution_python(pinned_args) == aggregate.REFERENCE_PYTHON
    assert "--require-reference-python" in (
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py"
    ).read_text()


def test_aggregation_removes_stale_sniper_timing_outputs(tmp_path):
    module = load_module(
        "aggregate_results_stale_sniper_figure",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    stale_paths = [
        tmp_path / "aggregate" / "sniper_relative_metrics.csv",
        tmp_path / "aggregate" / "sniper_relative_policy_summary.csv",
    ]
    for name in module.STALE_SPEEDUP_FIGURES:
        stale_paths.append(tmp_path / "figures" / name)
    for stale in stale_paths:
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_text("stale prohibited timing artifact")
    module.generate_outputs(tmp_path, [], [])
    assert all(not stale.exists() for stale in stale_paths)


def test_final_design_docs_and_run_flow_are_consistent():
    readme = (ROOT / "README.md").read_text()
    wiki = (ROOT / "wiki/ReusePlan-FlowThrough.md").read_text()
    methodology = (ROOT / "wiki/Evaluation-Methodology.md").read_text()
    reproduction = (ROOT / "wiki/Reproduction.md").read_text()
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())

    assert "wiki/ReusePlan-FlowThrough.md" in readme
    assert "wiki/Evaluation-Methodology.md" in readme
    assert "wiki/Reproduction.md" in readme
    assert "--profile" not in readme

    # The wiki is the detailed explanatory layer and contains no measured
    # performance tables.
    assert "(Evaluation-Methodology)" in wiki
    assert "(Reproduction)" in wiki
    assert "epoch_first" not in wiki
    assert "no experimental results" in wiki
    assert "## 3. Computing future distance by hand" in wiki
    assert "## 6. FlowThrough placement" in wiki

    assert "# Evaluation Methodology" in methodology
    assert "contains no" in methodology
    assert "performance results" in methodology

    assert "## 4. Inspect the PageRank study" in reproduction
    assert "reuse_plan_pagerank_study" in reproduction
    assert "cit-Patents/cit-Patents.el" in reproduction
    assert "cit-Patents/cit-Patents.mtx" not in reproduction
    assert "python3 -I" in reproduction
    assert "--require-reference-python" in reproduction
    assert "--no-build --no-resume" in reproduction
    assert "## 6. Final role-separated campaign" in reproduction
    assert "reuse_plan_final_campaign" in reproduction
    assert "## 7. Cross-simulator consistency" in reproduction
    assert "--input-run-dirs" in reproduction

    blocked_stage = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "40_sniper_flowthrough_realgraph")
    assert blocked_stage["blocked_reason"]
    assert (
        "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH"
        not in blocked_stage["policies"])
    factorial = next(
        stage for stage in manifest["stages"]
        if stage["name"] == "20_cache_sim_flowthrough_factorial")
    assert "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH" in factorial["policies"]


def test_partial_policy_matrix_is_not_resumable(tmp_path):
    module = load_module(
        "experiment_run_partial_matrix",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    out_dir = tmp_path / "matrix"
    out_dir.mkdir()
    csv_path = out_dir / "roi_matrix.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=out_dir,
        log_path=tmp_path / "matrix.log",
        metadata={"policies": ["LRU", "SRRIP"]},
    )
    status, detail = module.job_csv_status(job)
    assert status == "partial"
    assert "SRRIP" in detail


def test_unexpected_policy_matrix_is_not_resumable(tmp_path):
    module = load_module(
        "experiment_run_unexpected_matrix",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    csv_path = tmp_path / "roi_matrix.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
        writer.writerow({"status": "ok", "policy_label": "SRRIP"})
    status, detail = module.csv_status(csv_path, ["LRU"])
    assert status == "partial"
    assert "unexpected policies=['SRRIP']" in detail


def test_complete_matrix_requires_matching_marker(tmp_path):
    module = load_module(
        "experiment_run_complete_matrix",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    out_dir = tmp_path / "matrix"
    out_dir.mkdir()
    with (out_dir / "roi_matrix.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=out_dir,
        log_path=tmp_path / "matrix.log",
        metadata={
            "policies": ["LRU"],
            "l3_sizes": ["2MB"],
            "threads": ["1"],
            "structure_prefetch_degree": 8,
            "config_hash": "abc",
        },
    )
    assert module.job_csv_status(job)[0] == "partial"
    (out_dir / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "policy_labels": ["LRU"],
        "l3_sizes": ["2MB"],
        "threads": ["1"],
        "structure_prefetch_degree": 8,
        "config_hash": "stale",
        "outputs": {
            "roi_matrix.csv": output_descriptor(
                out_dir / "roi_matrix.csv"),
        },
    }))
    assert module.job_csv_status(job)[0] == "partial"
    marker = json.loads(
        (out_dir / "roi_matrix.complete.json").read_text())
    marker["config_hash"] = "abc"
    (out_dir / "roi_matrix.complete.json").write_text(json.dumps(marker))
    assert module.job_csv_status(job)[0] == "ok"
    with (out_dir / "roi_matrix.csv").open("a") as handle:
        handle.write("\n")
    assert module.job_csv_status(job)[0] == "partial"


def test_policy_labels_share_one_parser():
    module = load_module(
        "policy_specs_shared",
        ROOT / "scripts/experiments/ecg/policy_specs.py",
    )
    assert module.policy_output_label("POPT") == "POPT"
    assert module.policy_output_label("POPT_CHARGED") == "POPT"
    assert module.policy_output_label("POPT:UNCHARGED") == "POPT_UNCHARGED"
    assert module.policy_output_label("ECG:REUSE_PLAN") == "ECG_REUSE_PLAN"


def test_sharded_policies_share_comparison_scope():
    module = load_module(
        "aggregate_results_shard_scope",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    common = {
        "status": "ok",
        "final_shard_group": "run_tag",
        "final_matrix_id": "web_pr",
        "final_matrix_config_hash": "same-config",
        "simulator": "gem5",
        "gem5_cpu_type": "O3",
        "benchmark": "pr",
        "prefetcher": "STRIDE",
        "l3_size": "2MB",
        "threads": "1",
        "section": "1",
        "timing_valid_for_speedup": "1",
        "final_expected_policy_labels": json.dumps(["LRU", "ECG_REUSE_PLAN"]),
    }
    rows = [
        {**common, "pipeline_run_name": "lru", "final_job_id": "lru",
         "policy_label": "LRU", "sim_ticks": "100", "l3_misses": "50"},
        {**common, "pipeline_run_name": "reuse_plan", "final_job_id": "reuse_plan",
         "policy_label": "ECG_REUSE_PLAN", "sim_ticks": "80", "l3_misses": "40"},
        {**common, "pipeline_run_name": "partial", "final_job_id": "partial",
         "final_output_status": "partial", "policy_label": "SRRIP",
         "sim_ticks": "90", "l3_misses": "45"},
    ]
    relative = module.roi_relative_metrics(rows)
    assert len(relative) == 2
    reuse_plan = next(row for row in relative if row["policy_label"] == "ECG_REUSE_PLAN")
    assert reuse_plan["speedup_vs_lru"] == 1.25


def test_pipeline_rejects_non_o3_speedup_flags():
    module = load_module(
        "aggregate_results_timing_authority",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    for simulator, cpu_type in (
            ("cache_sim", ""),
            ("sniper", ""),
            ("gem5", "timing")):
        common = {
            "status": "ok",
            "final_output_status": "ok",
            "final_shard_group": "run_tag",
            "final_matrix_id": f"{simulator}-{cpu_type}",
            "final_matrix_config_hash": "same-config",
            "simulator": simulator,
            "gem5_cpu_type": cpu_type,
            "benchmark": "pr",
            "prefetcher": "none",
            "l3_size": "2MB",
            "threads": "1",
            "section": "1",
            "timing_valid_for_speedup": "1",
            "final_expected_policy_labels": json.dumps(
                ["LRU", "ECG_REUSE_PLAN"]),
        }
        rows = [
            {
                **common,
                "pipeline_run_name": "lru",
                "final_job_id": "same-job",
                "policy_label": "LRU",
                "sim_ticks": "100",
                "l3_misses": "50",
            },
            {
                **common,
                "pipeline_run_name": "reuse_plan",
                "final_job_id": "same-job",
                "policy_label": "ECG_REUSE_PLAN",
                "sim_ticks": "80",
                "l3_misses": "40",
            },
        ]
        relative = module.roi_relative_metrics(rows)
        assert len(relative) == 2
        assert all(
            row["timing_valid_for_speedup"] == "0"
            for row in relative)
        assert all("speedup_vs_lru" not in row for row in relative)


def test_roi_matrix_only_marks_gem5_o3_timing_speedup_valid():
    module = load_module(
        "roi_matrix_timing_authority",
        ROOT / "scripts/experiments/ecg/roi_matrix.py",
    )
    args = module.parse_args([
        "--suite", "gem5",
        "--policies", "LRU",
        "--gem5-cpu-type", "O3",
    ])
    args.has_lru_baseline = True
    spec = module.parse_policy_spec("LRU")

    o3 = module.base_row("gem5", args, spec, "2MB")
    assert o3["timing_valid_for_speedup"] == "1"

    args.gem5_cpu_type = "timing"
    timing_cpu = module.base_row("gem5", args, spec, "2MB")
    cache_sim = module.base_row("cache_sim", args, spec, "2MB")
    sniper = module.base_row("sniper", args, spec, "2MB")
    for row in (timing_cpu, cache_sim, sniper):
        assert row["timing_valid_for_speedup"] == "0"
        assert row["timing_comparison_bound"] == "not_speedup_evidence"
    assert timing_cpu["timing_model"] == "gem5_non_o3_diagnostic"
    assert cache_sim["timing_model"] == "cache_mechanism_model"
    assert sniper["timing_model"] == "sniper_scale_direction_model"


def test_mechanism_only_roi_summary_suppresses_timing():
    module = load_module(
        "aggregate_results_mechanism_summary",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    rows = [
        {
            "status": "ok",
            "simulator": "gem5",
            "benchmark": "pr",
            "prefetcher": "none",
            "l3_size": "32kB",
            "threads": "",
            "policy_label": "ECG_REUSE_PLAN_FLOWTHROUGH",
            "sim_ticks": "123",
            "ipc": "1.5",
            "l3_misses": "45",
            "ecg_record_bytes": "4",
            "edge_stream_bytes_per_edge": "4",
            "timing_valid_for_speedup": "0",
            "timing_model": "mechanism_probe_exact_request",
            "timing_caveat": "not performance evidence",
        },
    ]
    summary = module.summarize_roi(rows)
    assert len(summary) == 1
    assert summary[0]["timing_valid_for_speedup"] == "0"
    assert summary[0]["timing_model"] == "mechanism_probe_exact_request"
    assert summary[0]["timing_caveat"] == "not performance evidence"
    assert "avg_sim_ticks" not in summary[0]
    assert "avg_ipc" not in summary[0]
    assert summary[0]["mechanism_only_correctness"] == "1"
    assert "avg_l3_misses" not in summary[0]
    assert summary[0]["avg_ecg_record_bytes"] == 4
    assert summary[0]["avg_edge_stream_bytes_per_edge"] == 4

    table_rows, columns, title = module.roi_summary_table_spec(
        [], [], summary)
    assert table_rows == summary
    assert "avg_sim_ticks" not in columns
    assert "avg_l3_misses" not in columns
    assert "avg_ecg_record_bytes" in columns
    assert "timing_valid_for_speedup" in columns
    assert title == "ECG mechanism-only ROI summary"

    relative_input = [
        {
            **rows[0],
            "final_shard_group": "run",
            "final_matrix_id": "proposal",
            "final_matrix_config_hash": "same",
            "final_expected_policy_labels": json.dumps([
                "LRU", "ECG_REUSE_PLAN_FLOWTHROUGH"]),
            "policy_label": "LRU",
            "timing_model": "",
            "timing_valid_for_speedup": "1",
        },
        {
            **rows[0],
            "final_shard_group": "run",
            "final_matrix_id": "proposal",
            "final_matrix_config_hash": "same",
            "final_expected_policy_labels": json.dumps([
                "LRU", "ECG_REUSE_PLAN_FLOWTHROUGH"]),
        },
    ]
    relative_rows = module.roi_relative_metrics(relative_input)
    assert [row["policy_label"] for row in relative_rows] == ["LRU"]


def test_pipeline_manifest_binds_inputs_scripts_and_outputs(tmp_path):
    module = load_module(
        "aggregate_results_bound_manifest",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    (run_dir / "run.complete.json").write_text(
        '{"complete": true}\n')
    (run_dir / "resolved_manifest.json").write_text(
        '{"profiles": ["proposal"]}\n')
    (run_dir / "combined_roi_matrix.csv").write_text(
        "status,policy_label\nok,ECG_REUSE_PLAN_FLOWTHROUGH\n")
    out_dir = tmp_path / "pipeline"
    rows = [{
        "status": "ok",
        "simulator": "gem5",
        "benchmark": "pr",
        "prefetcher": "none",
        "l3_size": "32kB",
        "threads": "",
        "policy_label": "ECG_REUSE_PLAN_FLOWTHROUGH",
        "ecg_record_bytes": "4",
        "edge_stream_bytes_per_edge": "4",
        "timing_valid_for_speedup": "0",
        "timing_model": "mechanism_probe_exact_request",
        "timing_caveat": "not performance evidence",
    }]
    module.generate_outputs(
        out_dir, rows, [], input_run_dirs=[run_dir])
    manifest = json.loads(
        (out_dir / "aggregation_manifest.json").read_text())
    assert "run_0/run.complete.json" in manifest["inputs"]
    assert "run_0/combined_roi_matrix.csv" in manifest["inputs"]
    assert "aggregate_results.py" in manifest["scripts"]
    assert "git_state" not in manifest
    assert "aggregate/roi_policy_summary.csv" in manifest["outputs"]
    assert "tables/roi_policy_summary.tex" in manifest["outputs"]


def test_online_dueling_regret_uses_best_static_reuse_plan_arm():
    module = load_module(
        "aggregate_results_online_regret",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    policies = [
        "POPT_UNCHARGED", "POPT",
        "ECG_REUSE_PLAN_GRASP", "ECG_REUSE_PLAN_EPOCH", "ECG_REUSE_PLAN_RRIP",
        "ECG_REUSE_PLAN_DEGREE", "ECG_REUSE_PLAN_LRU", "ECG_REUSE_PLAN_ONLINE",
    ]
    common = {
        "status": "ok",
        "final_output_status": "ok",
        "final_shard_group": "run",
        "final_matrix_id": "web_bfs",
        "final_matrix_config_hash": "same",
        "simulator": "cache_sim",
        "final_graph": "web-Google",
        "benchmark": "bfs",
        "prefetcher": "none",
        "l3_size": "2MB",
        "threads": "1",
        "section": "0",
        "total_accesses": 1000,
        "l3_exercised": 1,
        "final_expected_policy_labels": json.dumps(policies),
    }
    misses = {
        "POPT_UNCHARGED": 90,
        "POPT": 70,
        "ECG_REUSE_PLAN_GRASP": 100,
        "ECG_REUSE_PLAN_EPOCH": 120,
        "ECG_REUSE_PLAN_RRIP": 95,
        "ECG_REUSE_PLAN_DEGREE": 80,
        "ECG_REUSE_PLAN_LRU": 130,
        "ECG_REUSE_PLAN_ONLINE": 84,
    }
    rows = [
        {
            **common,
            "policy_label": policy,
            "l3_misses": value,
            "l3_prop_misses": value - 10,
            **({
                "popt_charged_l3_misses_plus_matrix_stream": 140
            } if policy == "POPT" else {}),
        }
        for policy, value in misses.items()
    ]
    regret = module.online_dueling_regret(rows)
    assert len(regret) == 1
    assert regret[0]["final_graph"] == "web-Google"
    assert regret[0]["best_static_policy"] == "ECG_REUSE_PLAN_DEGREE"
    assert regret[0]["online_regret_pct"] == 5.0
    assert regret[0]["online_delta_vs_popt_uncharged_pct"] == pytest.approx(
        -6.6666666667)
    assert regret[0]["online_delta_vs_popt_pct"] == -40.0

    mismatched = [dict(row) for row in rows]
    mismatched[-1]["total_accesses"] = 1001
    with pytest.raises(RuntimeError, match="changes the demand stream"):
        module.online_dueling_regret(mismatched)


def test_preliminary_policy_ranks_use_overhead_aware_misses():
    module = load_module(
        "aggregate_results_preliminary_ranks",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    policies = ["LRU", "SRRIP", "GRASP", "POPT", "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE"]
    common = {
        "status": "ok",
        "final_output_status": "ok",
        "final_shard_group": "prelim",
        "final_matrix_id": "10_cache_sim_preliminary_5alg_kron_bfs",
        "final_matrix_config_hash": "same",
        "simulator": "cache_sim",
        "benchmark": "bfs",
        "prefetcher": "none",
        "l3_size": "128kB",
        "threads": "1",
        "section": "0",
        "l3_exercised": 1,
        "total_accesses": 1000,
        "final_expected_policy_labels": json.dumps(policies),
    }
    misses = {
        "LRU": 100,
        "SRRIP": 90,
        "GRASP": 70,
        "POPT": 60,
        "ECG_REUSE_PLAN": 65,
        "ECG_REUSE_PLAN_ONLINE": 68,
    }
    rows = [
        {
            **common,
            "policy_label": policy,
            "l3_misses": value,
            **({"l3_misses_with_overhead": 80} if policy == "POPT" else {}),
        }
        for policy, value in misses.items()
    ]
    ranked = module.preliminary_policy_ranks(rows)
    assert len(ranked) == 6
    reuse_plan = next(row for row in ranked if row["policy_label"] == "ECG_REUSE_PLAN")
    assert reuse_plan["best_policy"] == "ECG_REUSE_PLAN"
    assert reuse_plan["rank_within_simulator"] == 1
    assert reuse_plan["effective_miss_reduction_vs_popt_pct"] == pytest.approx(18.75)
    assert reuse_plan["record_transport_model"] == "fused_wide_record"
    stride_rows = [
        {**row, "prefetcher": "STRIDE",
         "final_matrix_id":
             "13_cache_sim_preliminary_5alg_stride_kron_bfs",
         "final_matrix_config_hash": "stride"}
        for row in rows
    ]
    assert len(module.preliminary_policy_ranks(rows + stride_rows)) == 6


def test_requested_fused_receipt_validation_is_fail_closed():
    runner = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py"
    ).read_text()
    assert "if fused_validation and (fused_count == 0 or fused_bad != 0)" in runner
    assert "Fused ReusePlan receipt validation failed." in runner


def test_all_five_kernels_expose_indexed_and_computed_address_delivery():
    for kernel in ("pr", "bfs", "sssp", "bc", "cc"):
        source = (
            ROOT / f"bench/src_gem5/{kernel}.cc"
        ).read_text()
        assert "gem5_ecg_bind_iload_u32" in source, kernel
        assert f"[ECG_REUSE_BIND_LOAD" in source, kernel
        assert f"[ECG_REUSE_BIND_ILOAD" in source, kernel

    sniper = (
        ROOT / "bench/src_sniper/sg_kernel.cc"
    ).read_text()
    for kernel in ("BFS", "BC", "CC"):
        assert f"[ECG_FUSED_REUSE_PLAN] {kernel}" in sniper
    assert "[ECG_FUSED_REUSE_PLAN_WEIGHTED64] SSSP" in sniper
    assert "[ECG_FUSED_REUSE_PLAN_WEIGHTED32] SSSP" in sniper
    assert sniper.count("const bool fused_reuse_plan_model =") == 5
    assert sniper.count('"SNIPER_ECG_VERTICES_PER_LINE"') >= 5

    runner = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py"
    ).read_text()
    assert (
        'reuse_plan_depth == 2 and args.sniper_workload == "sg_kernel"'
        in runner)
    # The RISC-V fused record load must still be the default delivery, but it
    # has to be ablatable: every fused variant carries the canonical 64-bit
    # record and has no 32-bit form, so a compact record cannot be studied while
    # one is active. The condition therefore gates on an explicit switch rather
    # than on riscv_delivery alone.
    assert 'elif riscv_delivery and fused_record_load_allowed:' in runner
    assert 'GRAPHBREW_REUSE_PLAN_FUSED_LOAD' in runner, (
        "the fused delivery family must be switchable off, or width and "
        "software decode cannot be separated")
    verifier = (
        ROOT / "scripts/experiments/ecg/verify/equiv_kernels.py"
    ).read_text()
    assert "computed-address ReuseBind property load" in verifier
    assert "matched Sniper ReuseBind is not implemented" not in verifier
    assert "computed-address ReuseBind load binding" in verifier
    assert '"sniper_reuse_bind_exact") == "1"' in verifier
    assert "indexed ReuseBind-Indexed property load" in verifier
    assert "fused ReusePlan sideband" in verifier


def test_preliminary_stride_sensitivity_separates_demand_and_traffic():
    module = load_module(
        "aggregate_results_stride_sensitivity",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    policies = ["LRU", "SRRIP", "GRASP", "POPT", "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE"]
    rows = []
    for prefetcher, miss_scale, traffic_scale, matrix in (
        ("none", 1.0, 1.0, "base"),
        ("STRIDE", 0.25, 1.5, "stride"),
    ):
        for index, policy in enumerate(policies):
            rows.append({
                "status": "ok",
                "final_output_status": "ok",
                "final_shard_group": matrix,
                "final_matrix_id":
                    f"{matrix}_cache_sim_preliminary_5alg_kron_pr",
                "final_matrix_config_hash": matrix,
                "final_comparison_config_hash": "same-comparison",
                "final_graph": "kron",
                "simulator": "cache_sim",
                "benchmark": "pr",
                "prefetcher": prefetcher,
                "l3_size": "128kB",
                "threads": "1",
                "section": "0",
                "policy_label": policy,
                "l3_misses_with_overhead": (100 + index) * miss_scale,
                "total_memory_traffic_with_overhead":
                    (100 + index) * traffic_scale,
                "timing_valid_for_speedup": "0",
                "final_expected_policy_labels": json.dumps(policies),
            })
    sensitivity = module.preliminary_stride_sensitivity(rows)
    reuse_plan = next(row for row in sensitivity if row["policy_label"] == "ECG_REUSE_PLAN")
    assert reuse_plan["demand_miss_reduction_pct"] == 75.0
    assert reuse_plan["traffic_change_pct"] == 50.0
    assert reuse_plan["traffic_unit"] == "memory_transactions"
    duplicate = [dict(row) for row in rows]
    duplicate.append(dict(rows[0]))
    with pytest.raises(RuntimeError, match="duplicate inputs"):
        module.preliminary_stride_sensitivity(duplicate)
    mismatched = [dict(row) for row in rows]
    for row in mismatched:
        if row["prefetcher"] == "STRIDE":
            row["final_comparison_config_hash"] = "stale-comparison"
    with pytest.raises(RuntimeError, match="comparison hash mismatch"):
        module.preliminary_stride_sensitivity(mismatched)


def test_sniper_stride_sensitivity_reports_traffic_without_demand_split():
    module = load_module(
        "aggregate_results_sniper_stride_scope",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    policies = ["LRU", "SRRIP", "GRASP", "POPT", "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_ONLINE"]
    rows = []
    for prefetcher, misses, matrix_hash in (
            ("none", 100, "none"),
            ("STRIDE", 400, "stride")):
        for policy in policies:
            rows.append({
                "status": "ok",
                "final_output_status": "ok",
                "final_shard_group": matrix_hash,
                "final_matrix_id":
                    f"15_sniper_preliminary_5alg_{matrix_hash}_pr",
                "final_matrix_config_hash": matrix_hash,
                "final_comparison_config_hash": "same-comparison",
                "final_graph": "kron",
                "simulator": "sniper",
                "benchmark": "pr",
                "prefetcher": prefetcher,
                "l3_size": "128kB",
                "l1d_size": "16kB",
                "l2_size": "64kB",
                "l3_ways": "16",
                "options": "-g 15",
                "threads": "1",
                "section": "1",
                "policy_label": policy,
                "l3_misses": misses,
                "l3_misses_with_overhead": misses,
                "l3_exercised": 1,
                "final_expected_policy_labels": json.dumps(policies),
            })
    sensitivity = module.preliminary_stride_sensitivity(rows)
    reuse_plan = next(
        row for row in sensitivity
        if row["policy_label"] == "ECG_REUSE_PLAN")
    assert reuse_plan["demand_miss_metric_available"] == 0
    assert reuse_plan["demand_miss_unavailable_reason"] == (
        "sniper_lacks_prefetch_miss_split")
    assert "demand_miss_reduction_pct" not in reuse_plan
    assert reuse_plan["traffic_unit"] == "llc_read_misses"
    assert reuse_plan["traffic_change_pct"] == pytest.approx(300.0)


def test_l3_pressure_requires_positive_activity():
    module = load_module(
        "roi_matrix_l3_pressure",
        ROOT / "scripts/experiments/ecg/roi_matrix.py",
    )
    assert module.annotate_l3_pressure(
        {"status": "ok"})["l3_exercised"] is False
    assert module.annotate_l3_pressure({
        "status": "ok", "l3_accesses": 0, "l3_misses": 0,
    })["l3_exercised"] is False
    assert module.annotate_l3_pressure({
        "status": "ok", "l3_accesses": 10, "l3_misses": 5,
    })["l3_exercised"] is True
    assert module.annotate_l3_pressure({
        "status": "ok", "l3_accesses": 10, "l3_misses": 10,
    })["l3_exercised"] is False


def test_missing_policy_shards_produce_no_relative_rows():
    module = load_module(
        "aggregate_results_missing_shards",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    expected = json.dumps([
        "LRU", "SRRIP", "GRASP", "POPT",
        "ECG_REUSE_PLAN", "ECG_REUSE_PLAN_FLOWTHROUGH",
    ])
    common = {
        "status": "ok",
        "final_output_status": "ok",
        "final_shard_group": "group",
        "final_matrix_id": "matrix",
        "final_matrix_config_hash": "same-config",
        "final_expected_policy_labels": expected,
        "simulator": "sniper",
        "benchmark": "pr",
        "prefetcher": "STRIDE",
        "l3_size": "2MB",
        "threads": "1",
        "section": "1",
    }
    rows = [
        {**common, "policy_label": "LRU", "sim_ticks": "100"},
        {**common, "policy_label": "ECG_REUSE_PLAN", "sim_ticks": "80"},
    ]
    assert module.roi_relative_metrics(rows) == []


def test_mismatched_matrix_hashes_do_not_compare():
    module = load_module(
        "aggregate_results_mismatched_hashes",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    expected = json.dumps(["LRU", "ECG_REUSE_PLAN"])
    common = {
        "status": "ok",
        "final_output_status": "ok",
        "final_shard_group": "group",
        "final_matrix_id": "matrix",
        "final_expected_policy_labels": expected,
        "simulator": "sniper",
        "benchmark": "pr",
        "prefetcher": "STRIDE",
        "l3_size": "2MB",
        "threads": "1",
        "section": "1",
    }
    rows = [
        {**common, "final_matrix_config_hash": "a",
         "policy_label": "LRU", "sim_ticks": "100"},
        {**common, "final_matrix_config_hash": "b",
         "policy_label": "ECG_REUSE_PLAN", "sim_ticks": "80"},
    ]
    assert module.roi_relative_metrics(rows) == []


def test_raw_partial_matrix_is_not_collected(tmp_path):
    module = load_module(
        "aggregate_results_partial_collection",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix = tmp_path / "roi_matrix.csv"
    with matrix.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    roi, proof = module.collect_csvs([], [matrix])
    assert roi == []
    assert proof == []


def test_failed_proof_matrix_is_not_collected(tmp_path):
    module = load_module(
        "aggregate_results_failed_proof",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix = tmp_path / "proof_matrix.csv"
    with matrix.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    (tmp_path / "proof_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": False,
    }))
    roi, proof = module.collect_csvs([], [matrix])
    assert roi == []
    assert proof == []


def test_stale_combined_csv_requires_run_marker(tmp_path):
    module = load_module(
        "aggregate_results_stale_combined",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    with (tmp_path / "combined_roi_matrix.csv").open(
            "w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    roi, proof = module.collect_csvs([tmp_path], [])
    assert roi == []
    assert proof == []


def test_complete_combined_csv_recovers_legacy_comparison_hash(tmp_path):
    runner = load_module(
        "experiment_run_combined_legacy_comparison",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    pipeline = load_module(
        "aggregate_results_combined_legacy_comparison",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix_dir = tmp_path / "matrices" / "stage" / "graph" / "pr"
    matrix_dir.mkdir(parents=True)
    output_csv = matrix_dir / "roi_matrix.csv"
    output_csv.write_text("status,policy_label\nok,LRU\n")
    command = [
        sys.executable, "roi_matrix.py",
        "--policies", "LRU",
        "--prefetcher", "none",
        "--structure-prefetch-degree", "0",
        "--out-dir", str(matrix_dir),
    ]
    material_env = {"GRAPHBREW_EXPLICIT_CELL_ENV": "{}"}
    inputs = {"benchmark_binary": "binary"}
    config_hash = hashlib.sha256(json.dumps(
        {"command": command, "env": material_env, "inputs": inputs},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    matrix_hash = "matrix-hash"
    comparison_hash = runner.roi_comparison_config_hash(
        command, material_env, inputs, ["LRU"])
    combined = tmp_path / "combined_roi_matrix.csv"
    with combined.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "status", "policy_label", "final_output_status",
            "final_job_id", "final_matrix_id",
            "final_matrix_config_hash", "final_output_csv",
            "final_expected_policy_labels",
        ])
        writer.writeheader()
        writer.writerow({
            "status": "ok",
            "policy_label": "LRU",
            "final_output_status": "ok",
            "final_job_id": "job",
            "final_matrix_id": "matrix",
            "final_matrix_config_hash": matrix_hash,
            "final_output_csv": str(output_csv),
            "final_expected_policy_labels": json.dumps(["LRU"]),
        })
    run_hash = "run-hash"
    (tmp_path / "resolved_manifest.json").write_text(json.dumps({
        "run_config_hash": run_hash,
        "jobs": [{
            "job_id": "job",
            "kind": "roi_matrix",
            "command": command,
            "out_dir": str(matrix_dir),
            "metadata": {
                "config_hash": config_hash,
                "env": {
                    **material_env,
                    "GRAPHBREW_MATRIX_CONFIG_HASH": config_hash,
                    "GRAPHBREW_MATRIX_ID": "matrix",
                },
                "expected_policy_labels": ["LRU"],
                "input_fingerprints": inputs,
                "matrix_config_hash": matrix_hash,
                "policies": ["LRU"],
            },
        }],
    }))
    (tmp_path / "run.complete.json").write_text(json.dumps({
        "complete": True,
        "run_config_hash": run_hash,
        "outputs": {
            "combined_roi_matrix.csv": output_descriptor(combined),
        },
    }))
    roi, proof = pipeline.collect_csvs([tmp_path], [])
    assert len(roi) == 1
    assert roi[0]["final_comparison_config_hash"] == comparison_hash
    assert proof == []


def test_fallback_matrix_must_match_resolved_job_hash(tmp_path):
    module = load_module(
        "aggregate_results_stale_fallback",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix_dir = tmp_path / "matrices" / "stage" / "graph" / "pr"
    matrix_dir.mkdir(parents=True)
    with (matrix_dir / "roi_matrix.csv").open(
            "w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    (matrix_dir / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "config_hash": "old",
        "matrix_config_hash": "matrix",
        "matrix_id": "matrix",
        "shard_group": "group",
        "expected_policy_labels": ["LRU"],
        "outputs": {
            "roi_matrix.csv": output_descriptor(
                matrix_dir / "roi_matrix.csv"),
        },
    }))
    (tmp_path / "resolved_manifest.json").write_text(json.dumps({
        "run_config_hash": "new-run",
        "jobs": [{
            "out_dir": str(matrix_dir),
            "metadata": {"config_hash": "new"},
        }],
    }))
    roi, proof = module.collect_csvs([tmp_path], [])
    assert roi == []
    assert proof == []


def test_fallback_matrix_recovers_legacy_comparison_hash(tmp_path):
    runner = load_module(
        "experiment_run_legacy_comparison",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    pipeline = load_module(
        "aggregate_results_legacy_comparison",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix_dir = tmp_path / "matrices" / "stage" / "graph" / "pr"
    matrix_dir.mkdir(parents=True)
    matrix = matrix_dir / "roi_matrix.csv"
    with matrix.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    command = [
        sys.executable, "roi_matrix.py",
        "--policies", "LRU",
        "--prefetcher", "none",
        "--structure-prefetch-degree", "0",
        "--out-dir", str(matrix_dir),
    ]
    material_env = {"GRAPHBREW_EXPLICIT_CELL_ENV": "{}"}
    inputs = {"benchmark_binary": "binary"}
    config_hash = hashlib.sha256(json.dumps(
        {"command": command, "env": material_env, "inputs": inputs},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    expected_comparison_hash = runner.roi_comparison_config_hash(
        command, material_env, inputs, ["LRU"])
    (matrix_dir / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "config_hash": config_hash,
        "matrix_config_hash": "matrix",
        "matrix_id": "matrix",
        "shard_group": "group",
        "expected_policy_labels": ["LRU"],
        "outputs": {
            "roi_matrix.csv": output_descriptor(matrix),
        },
    }))
    (tmp_path / "resolved_manifest.json").write_text(json.dumps({
        "run_config_hash": "run",
        "jobs": [{
            "command": command,
            "out_dir": str(matrix_dir),
            "metadata": {
                "config_hash": config_hash,
                "env": {
                    **material_env,
                    "GRAPHBREW_MATRIX_CONFIG_HASH": config_hash,
                    "GRAPHBREW_MATRIX_ID": "matrix",
                },
                "expected_policy_labels": ["LRU"],
                "input_fingerprints": inputs,
                "policies": ["LRU"],
            },
        }],
    }))
    roi, proof = pipeline.collect_csvs([tmp_path], [])
    assert len(roi) == 1
    assert roi[0]["final_comparison_config_hash"] == (
        expected_comparison_hash)
    assert proof == []


def test_comparison_hash_ignores_legacy_git_state():
    module = load_module(
        "experiment_run_comparison_git_state",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    command = [
        sys.executable, "roi_matrix.py",
        "--policies", "LRU",
        "--prefetcher", "none",
        "--structure-prefetch-degree", "0",
        "--out-dir", "/tmp/matrix",
    ]
    common = {"benchmark_binary": "same-binary"}
    first = module.roi_comparison_config_hash(
        command, {}, {**common, "git_state": "dirty-a"}, ["LRU"])
    second = module.roi_comparison_config_hash(
        command, {}, {**common, "git_state": "dirty-b"}, ["LRU"])
    assert first == second


def test_charged_metrics_use_uniform_overhead_field():
    module = load_module(
        "aggregate_results_charged_metrics",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    assert module.effective_l3_misses({
        "l3_misses": "100",
        "l3_misses_with_overhead": "150",
    }) == 150
    common = {
        "status": "ok",
        "final_shard_group": "group",
        "final_matrix_id": "matrix",
        "final_matrix_config_hash": "same-config",
        "simulator": "sniper",
        "benchmark": "pr",
        "prefetcher": "STRIDE",
        "l3_size": "2MB",
        "threads": "1",
        "section": "1",
        "final_expected_policy_labels": json.dumps(
            ["POPT", "POPT_UNCHARGED"]),
    }
    rows = [
        {**common, "policy_label": "POPT",
         "l3_misses": "100", "l3_misses_with_overhead": "150"},
        {**common, "policy_label": "POPT_UNCHARGED",
         "l3_misses": "100", "l3_misses_with_overhead": "100"},
    ]
    overhead = module.charged_overhead(rows)
    assert len(overhead) == 1
    assert overhead[0]["l3_miss_delta"] == 50


def test_thread_scaling_uses_series_and_per_core_llc():
    module = load_module(
        "aggregate_results_scaling_scope",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    common = {
        "status": "ok",
        "final_shard_group": "group",
        "final_scaling_series_id": "series",
        "simulator": "sniper",
        "benchmark": "pr",
        "prefetcher": "none",
        "per_core_l3_size": "2MB",
        "policy_label": "LRU",
        "section": "1",
        "final_expected_policy_labels": json.dumps(["LRU"]),
    }
    rows = [
        {**common, "threads": "1", "sim_ticks": "100"},
        {**common, "threads": "2", "sim_ticks": "60"},
    ]
    scaling = module.thread_scaling_metrics(rows)
    assert len(scaling) == 2
    assert scaling[1]["thread_speedup_vs_1t"] == 100 / 60


def test_policy_filter_uses_shared_labels():
    module = load_module(
        "experiment_run_policy_filter",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    assert module.filter_policy_specs(
        ["LRU", "POPT"], ["POPT_CHARGED"]) == ["POPT"]


def test_sniper_policy_marker_is_content_validated(tmp_path):
    module = load_module(
        "roi_matrix_sniper_marker",
        ROOT / "scripts/experiments/ecg/roi_matrix.py",
    )
    relative = Path("common/core/memory_subsystem/cache/cache_set_ecg.cc")
    overlay = tmp_path / "bench/include/sniper_sim/overlays" / relative
    installed_root = tmp_path / "snipersim"
    installed = installed_root / relative
    overlay.parent.mkdir(parents=True)
    installed.parent.mkdir(parents=True)
    overlay.write_text("same")
    installed.write_text("same")
    binary = installed_root / "lib/sniper"
    binary.parent.mkdir(parents=True)
    binary.write_text("binary")
    subprocess.run(["git", "init", "-q"], cwd=installed_root, check=True)
    subprocess.run(
        ["git", "config", "user.email", "test@example.com"],
        cwd=installed_root, check=True)
    subprocess.run(
        ["git", "config", "user.name", "Test"],
        cwd=installed_root, check=True)
    subprocess.run(["git", "add", "."], cwd=installed_root, check=True)
    subprocess.run(
        ["git", "commit", "-qm", "fixture"],
        cwd=installed_root, check=True)
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=installed_root, capture_output=True, text=True, check=True,
    ).stdout.strip()
    module.PINNED_SNIPER_HEAD = head
    marker = tmp_path / ".sniper_overlays.json"
    marker.write_text(json.dumps({
        "sniper_head": head,
        "policies": ["grasp", "popt", "ecg"],
        "copied_files": [str(relative)],
        "patched_files": [],
        "file_hashes": {
            str(relative): hashlib.sha256(
                installed.read_bytes()).hexdigest(),
        },
        "binary": {
            "path": "lib/sniper",
            "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "size": binary.stat().st_size,
        },
    }))
    module.PROJECT_ROOT = tmp_path
    module.SNIPER_OVERLAY_STATUS = marker
    args = argparse.Namespace(
        sniper_enable_graph_policies=False,
        sniper_root=str(installed_root),
    )
    assert module.sniper_graph_policies_enabled(args)
    extra = overlay.parent / "extra.cc"
    extra.write_text("new overlay")
    assert not module.sniper_graph_policies_enabled(args)
    extra.unlink()
    installed.write_text("stale")
    assert not module.sniper_graph_policies_enabled(args)


def test_failed_rerun_invalidates_completion_marker(tmp_path):
    module = load_module(
        "experiment_run_failed_rerun",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    out_dir = tmp_path / "matrix"
    out_dir.mkdir()
    marker = out_dir / "roi_matrix.complete.json"
    marker.write_text("{}")
    job = module.Job(
        job_id="failed",
        stage="stage",
        kind="roi_matrix",
        command=[sys.executable, "-c", "raise SystemExit(1)"],
        out_dir=out_dir,
        log_path=tmp_path / "failed.log",
        metadata={"policies": ["LRU"]},
    )
    args = argparse.Namespace(
        force=True,
        resume=True,
        skip_failed=False,
        dry_run=False,
    )
    assert module.run_job(job, tmp_path, args) == 1
    assert not marker.exists()
    module.write_run_completion(tmp_path, [job], successful=False)
    payload = json.loads((tmp_path / "run.complete.json").read_text())
    assert payload["complete"] is False


def test_subset_run_cannot_replace_broader_manifest(tmp_path):
    module = load_module(
        "experiment_run_manifest_scope",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    selected_dir = tmp_path / "selected"
    omitted_dir = tmp_path / "omitted"
    (tmp_path / "resolved_manifest.json").write_text(json.dumps({
        "jobs": [
            {"out_dir": str(selected_dir)},
            {"out_dir": str(omitted_dir)},
        ],
    }))
    job = module.Job(
        job_id="selected",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=selected_dir,
        log_path=tmp_path / "selected.log",
    )
    with pytest.raises(SystemExit, match="broader resolved manifest"):
        module.guard_run_manifest_scope(tmp_path, [job])


def test_sniper_fingerprint_covers_sift_stack():
    module = load_module(
        "experiment_run_sniper_fingerprint",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    args = argparse.Namespace(
        manifest=str(
            ROOT / "scripts/experiments/ecg/experiment_manifest.json"))
    settings = {
        "suite": "sniper",
        "sniper_root": "bench/include/sniper_sim/snipersim",
        "sniper_workload": "sg_kernel",
    }
    fingerprints = module.roi_input_fingerprints(
        args,
        settings,
        ROOT / "results/graphs/web-Google/web-Google.sg",
        "pr",
    )
    for key in (
        "sniper_runner",
        "sniper_record_trace",
        "sniper_binary",
        "sniper_config",
        "sniper_runtime_scripts",
        "sniper_tools",
        "sniper_sde",
        "sniper_sift_recorder",
        "setarch",
        "benchmark_binary",
    ):
        assert key in fingerprints
    assert "git_state" not in fingerprints


def test_job_input_validation_detects_runtime_file_drift(tmp_path):
    module = load_module(
        "experiment_run_runtime_input_drift",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    runtime_input = tmp_path / "runtime.bin"
    runtime_input.write_bytes(b"before")
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=tmp_path / "out",
        log_path=tmp_path / "matrix.log",
        metadata={
            "input_fingerprints": {
                "runtime": module.compute_path_fingerprint(runtime_input),
            },
            "input_paths": {"runtime": str(runtime_input)},
        },
    )
    assert module.validate_job_inputs(job) == (True, "")
    runtime_input.write_bytes(b"after")
    ok, detail = module.validate_job_inputs(job)
    assert not ok
    assert "runtime input changed: runtime" in detail


def test_orchestrator_and_matrix_share_input_hashing(tmp_path):
    experiment_run = load_module(
        "experiment_run_shared_hash",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    from scripts.experiments.ecg import roi_matrix

    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "input.txt").write_text("stable")
    assert experiment_run.compute_path_fingerprint(
        tree) == roi_matrix.hash_input_path(tree)


def test_job_input_validation_ignores_legacy_repository_fingerprint(tmp_path):
    module = load_module(
        "experiment_run_legacy_repository_fingerprint",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=tmp_path / "out",
        log_path=tmp_path / "matrix.log",
        metadata={
            "input_fingerprints": {"git_state": "planned"},
            "input_paths": {},
        },
    )
    assert module.validate_job_inputs(job) == (True, "")


def test_job_input_validation_checks_stable_guest_receipt(
        tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_stable_receipt_drift",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    receipt = tmp_path / "guest.build.json"
    receipt.write_text("{}")
    exact = module.compute_path_fingerprint(receipt)
    monkeypatch.setattr(
        module, "stable_receipt_fingerprint", lambda path: "actual")
    monkeypatch.setattr(
        module, "material_input_fingerprint", lambda path: "material")
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=tmp_path / "out",
        log_path=tmp_path / "matrix.log",
        metadata={
            "input_fingerprints": {
                "gem5_guest_build_receipt": exact,
                "gem5_guest_build_receipt_stable": "expected",
                "gem5_guest_material_inputs": "material",
            },
            "input_paths": {
                "gem5_guest_build_receipt": str(receipt),
            },
        },
    )
    ok, detail = module.validate_job_inputs(job)
    assert not ok
    assert "stable gem5 guest receipt changed" in detail


def test_job_input_validation_checks_guest_material_inputs(
        tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_guest_material_drift",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    receipt = tmp_path / "guest.build.json"
    receipt.write_text("{}")
    exact = module.compute_path_fingerprint(receipt)
    monkeypatch.setattr(
        module, "stable_receipt_fingerprint", lambda path: "stable")
    monkeypatch.setattr(
        module, "material_input_fingerprint", lambda path: "changed")
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=tmp_path / "out",
        log_path=tmp_path / "matrix.log",
        metadata={
            "input_fingerprints": {
                "gem5_guest_build_receipt": exact,
                "gem5_guest_build_receipt_stable": "stable",
                "gem5_guest_material_inputs": "expected",
            },
            "input_paths": {
                "gem5_guest_build_receipt": str(receipt),
            },
        },
    )
    ok, detail = module.validate_job_inputs(job)
    assert not ok
    assert "gem5 guest material inputs changed" in detail


def test_resume_revalidates_inputs_before_skipping(
        tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_resume_input_drift",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=tmp_path / "out",
        log_path=tmp_path / "matrix.log",
    )
    monkeypatch.setattr(
        module, "validate_job_inputs",
        lambda selected: (False, "changed"))
    monkeypatch.setattr(
        module, "should_run",
        lambda selected, args: (False, "already complete"))
    args = argparse.Namespace(dry_run=False)
    assert module.run_job(job, tmp_path, args) == 2


def test_run_revalidates_inputs_after_execution(
        tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_post_execution_drift",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    marker = out_dir / "roi_matrix.complete.json"
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[
            sys.executable,
            "-c",
            f"from pathlib import Path; Path({str(marker)!r}).write_text('{{}}')",
        ],
        out_dir=out_dir,
        log_path=tmp_path / "matrix.log",
    )
    checks = iter(((True, ""), (False, "graph changed")))
    monkeypatch.setattr(
        module, "validate_job_inputs", lambda selected: next(checks))
    monkeypatch.setattr(
        module, "should_run", lambda selected, args: (True, "missing"))
    args = argparse.Namespace(dry_run=False)
    assert module.run_job(job, tmp_path, args) == 2
    assert not marker.exists()


def test_combined_outputs_exclude_non_ok_jobs(tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_combined_only_ok",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    out_dir = tmp_path / "matrix"
    out_dir.mkdir()
    (out_dir / "roi_matrix.csv").write_text(
        "status,policy_label\nok,LRU\n")
    job = module.Job(
        job_id="matrix",
        stage="stage",
        kind="roi_matrix",
        command=[],
        out_dir=out_dir,
        log_path=tmp_path / "matrix.log",
    )
    monkeypatch.setattr(
        module, "job_csv_status",
        lambda selected: ("partial", "input drift"))
    module.write_combined_outputs(tmp_path, [job])
    assert not (tmp_path / "combined_roi_matrix.csv").exists()


def test_proof_hash_ignores_ambient_environment_not_passed_to_child(
        tmp_path, monkeypatch):
    module = load_module(
        "experiment_run_proof_environment",
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py",
    )
    args = argparse.Namespace(
        manifest=str(
            ROOT / "scripts/experiments/ecg/experiment_manifest.json"),
        no_build=True,
        dry_run=False,
    )
    settings = {
        "name": "proof",
        "benchmarks": ["pr"],
        "l1d_size": "1kB",
        "l2_size": "2kB",
        "l3_sizes": ["4kB"],
        "l3_ways": "16",
        "line_size": "64",
        "timeout_cache": 60,
        "no_build": True,
    }
    monkeypatch.setenv("CACHE_FAST", "0")
    first = module.make_proof_job(args, tmp_path, settings)
    monkeypatch.setenv("CACHE_FAST", "1")
    second = module.make_proof_job(args, tmp_path, settings)
    assert first.metadata["config_hash"] == second.metadata["config_hash"]


def test_pipeline_dry_run_succeeds_without_rows(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            "scripts/experiments/ecg/flows/aggregate_results.py",
            "--profiles", "ecg_smoke",
            "--dry-run",
            "--no-build",
            "--run-root", str(tmp_path / "pipeline"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_proof_matrix_uses_supported_pfx_cli():
    source = (
        ROOT / "scripts/experiments/ecg/flows/proof_matrix.py"
    ).read_text()
    assert '"--prefetcher", "ECG_PFX"' in source
    assert '"--ecg-pfx-mode", pfx_mode' in source
    assert '"ECG_PREFETCH_MODE"' not in source


def test_direct_complete_matrix_has_standalone_hash(tmp_path):
    module = load_module(
        "aggregate_results_standalone_hash",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix = tmp_path / "roi_matrix.csv"
    with matrix.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    (tmp_path / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "matrix_id": "direct",
        "shard_group": "direct",
        "matrix_config_hash": "standalone-hash",
        "expected_policy_labels": ["LRU"],
        "outputs": {
            "roi_matrix.csv": output_descriptor(matrix),
        },
    }))
    roi, proof = module.collect_csvs([], [matrix])
    assert len(roi) == 1
    assert roi[0]["final_matrix_config_hash"] == "standalone-hash"
    assert proof == []


def test_aggregate_discovers_nested_run_directories(tmp_path):
    module = load_module(
        "aggregate_results_nested_inputs",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    direct = tmp_path / "direct"
    nested = tmp_path / "root" / "cell"
    direct.mkdir()
    nested.mkdir(parents=True)
    (direct / "combined_roi_matrix.csv").write_text("")
    (nested / "resolved_manifest.json").write_text("{}")
    assert module.discover_input_run_dirs(
        [direct, tmp_path / "root"]) == [
            direct.resolve(), nested.resolve()]


def test_direct_completed_matrix_infers_final_cell_metadata(tmp_path):
    module = load_module(
        "aggregate_results_direct_final_metadata",
        ROOT / "scripts/experiments/ecg/flows/aggregate_results.py",
    )
    matrix_dir = (
        tmp_path / "run" / "matrices" / "70_gem5_pagerank_i1" /
        "web-Google-n16" / "pr")
    matrix_dir.mkdir(parents=True)
    matrix = matrix_dir / "roi_matrix.csv"
    with matrix.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["status", "policy_label"])
        writer.writeheader()
        writer.writerow({"status": "ok", "policy_label": "LRU"})
    matrix_id = "70_gem5_pagerank_i1_web-Google-n16_pr"
    (matrix_dir / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "rows": 1,
        "matrix_id": matrix_id,
        "shard_group": "repair",
        "matrix_config_hash": "standalone-hash",
        "expected_policy_labels": ["LRU"],
        "outputs": {
            "roi_matrix.csv": output_descriptor(matrix),
        },
    }))
    roi, proof = module.collect_csvs([], [matrix])
    assert len(roi) == 1
    assert roi[0]["final_stage"] == "70_gem5_pagerank_i1"
    assert roi[0]["final_graph"] == "web-Google-n16"
    assert roi[0]["benchmark"] == "pr"
    assert roi[0]["final_job_id"] == matrix_id
    assert roi[0]["final_output_csv"] == str(matrix.resolve())
    assert roi[0]["final_output_status"] == "ok"
    assert proof == []
