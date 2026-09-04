#!/usr/bin/env python3
"""Regression tests for P-OPT charged-overhead policy expansion."""

from argparse import Namespace
import importlib.util
import json
from pathlib import Path
import re
import sys


PROJECT_ROOT = Path(__file__).resolve().parents[2]
ROI_MATRIX_PATH = PROJECT_ROOT / "scripts" / "experiments" / "ecg" / "roi_matrix.py"
spec = importlib.util.spec_from_file_location("roi_matrix", ROI_MATRIX_PATH)
roi_matrix = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["roi_matrix"] = roi_matrix
spec.loader.exec_module(roi_matrix)


def _charge_args(reserve_model: str = "fixed_one") -> Namespace:
    return Namespace(
        options="-g 12 -k 16 -o 5 -n 1 -i 2",
        line_size="64",
        l3_ways="16",
        popt_property_bytes="4",
        popt_active_columns="2",
        popt_min_data_ways="1",
        popt_num_epochs="256",
        popt_reserve_model=reserve_model,
    )


def test_popt_charged_label_and_default_fixed_one_reserves_single_way():
    spec = roi_matrix.parse_policy_spec("POPT_CHARGED")
    # Plain/charged POPT keeps the label "POPT"; the
    # charge is carried by charge_popt_overhead, not a label suffix.
    assert spec.policy == "POPT"
    assert spec.label == "POPT"
    assert spec.charge_popt_overhead

    charge = roi_matrix.popt_charge_metadata(_charge_args("fixed_one"), spec, "4kB")
    assert charge["popt_reserve_model"] == "fixed_one"
    assert charge["popt_estimated_vertices"] == 4096
    assert charge["popt_matrix_column_bytes"] == 256
    assert charge["popt_matrix_bytes"] == 512
    # fixed_one (legacy / P-OPT-favorable): one reserved streaming-buffer way.
    assert charge["popt_reserved_ways"] == 1
    assert charge["popt_reserved_bytes"] == 256
    assert charge["popt_effective_l3_ways"] == "15"
    assert charge["popt_effective_l3_size"] == "3840B"
    assert charge["popt_matrix_fits"] == 1


def test_popt_charged_size_correct_reserves_resident_matrix_columns():
    spec = roi_matrix.parse_policy_spec("POPT_CHARGED")
    charge = roi_matrix.popt_charge_metadata(_charge_args("size_correct"), spec, "4kB")
    # size_correct (reference-compatible): reserve
    # ceil(matrix_bytes / bytes_per_way) ways for the resident rereference-matrix
    # columns. matrix=512B at 256B/way -> 2 reserved ways, 14 data ways = 3584B.
    assert charge["popt_reserve_model"] == "size_correct"
    assert charge["popt_reserved_ways"] == 2
    assert charge["popt_reserved_bytes"] == 512
    assert charge["popt_effective_l3_ways"] == "14"
    assert charge["popt_effective_l3_size"] == "3584B"
    assert charge["popt_matrix_fits"] == 1
    assert charge["popt_matrix_stream_cache_lines"] == 1024


def test_analytic_popt_stream_charge_scales_with_iterations():
    row = {
        "options": "-g 12 -i 2",
        "line_size": "64",
        "popt_overhead_charged": 1,
        "popt_matrix_stream_requested": "analytic",
        "popt_matrix_stream_bytes": 65536,
        "popt_matrix_stream_cache_lines": 1024,
        "dram_read_bytes": 1000,
        "dram_write_bytes": 200,
        "l3_misses": 10,
        "total_memory_traffic": 20,
    }

    roi_matrix.apply_overhead_metrics(row)

    assert row["popt_matrix_stream_mode"] == "analytic_cumulative"
    assert row["popt_matrix_stream_iterations"] == 2
    assert row["popt_cumulative_stream_bytes"] == 131072
    assert row["popt_matrix_stream_requests"] == 2048
    assert row["popt_dram_offchip_bytes_without_matrix_stream"] == 1200
    assert row["dram_offchip_bytes"] == 132272
    assert row["l3_misses_with_overhead"] == 2058
    assert row["popt_timing_optimistic"] == 1


def test_gem5_popt_activation_receipt_is_required():
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_popt_receipt(
        row,
        "[POPT-ACTIVE sim=gem5 context=1 reref=1 phase2=1 "
        "epochs=256 cache_lines=64]",
        required=True)
    assert row["popt_policy_active"] == 1
    assert row["popt_context_loaded"] == 1
    assert row["popt_rereference_loaded"] == 1
    assert row["popt_runtime_epochs"] == 256

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_popt_receipt(
        missing, "", required=True)
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"


def test_gem5_geometry_receipt_checks_realized_cache(tmp_path):
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "system": {"l3cache": {"size": 3584, "assoc": 14}},
    }))
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_geometry_receipt(
        row, config, "3584B", "14")
    assert row["gem5_l3_size_actual"] == 3584
    assert row["gem5_l3_ways_actual"] == 14

    wrong = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_geometry_receipt(
        wrong, config, "4kB", "16")
    assert wrong["status"] == "error"


def test_gem5_analytic_popt_timing_is_labeled_optimistic():
    args = roi_matrix.parse_args([
        "--suite", "gem5",
        "--policies", "LRU", "POPT",
        "--gem5-cpu-type", "O3",
        "--popt-matrix-stream", "analytic",
    ])
    args.has_lru_baseline = True
    spec = roi_matrix.parse_policy_spec("POPT")
    charge = roi_matrix.popt_charge_metadata(
        _charge_args("size_correct"), spec, "4kB")
    row = roi_matrix.base_row("gem5", args, spec, "4kB", charge)

    assert row["timing_valid_for_speedup"] == "1"
    assert row["timing_model"] == "optimistic_popt_analytic_stream"
    assert "timing therefore favors P-OPT" in row["timing_caveat"]


def test_popt_charged_size_correct_marks_infeasible_when_matrix_exceeds_llc():
    # A graph whose two resident matrix columns exceed the whole LLC: the
    # design point cannot fit while leaving data ways -> mark the cell infeasible
    # (still emit a clamped min-data-way number as a labeled sensitivity).
    args = _charge_args("size_correct")
    args.options = "-g 18 -k 16 -o 5 -n 1 -i 2"  # 2^18 = 262144 vertices
    spec = roi_matrix.parse_policy_spec("POPT_CHARGED")
    charge = roi_matrix.popt_charge_metadata(args, spec, "4kB")
    assert charge["popt_matrix_fits"] == 0
    assert charge["popt_infeasible"] == 1
    assert charge["popt_effective_l3_ways"] == "1"
    assert "matrix_exceeds_llc" in charge["popt_charge_warning"]


def test_popt_uncharged_default_is_charged_but_explicit_uncharged_is_not():
    # Plain "POPT" is charged by default; the
    # ":UNCHARGED" diagnostic disables the capacity tax (full-cache oracle).
    assert roi_matrix.parse_policy_spec("POPT").charge_popt_overhead
    assert not roi_matrix.parse_policy_spec("POPT:UNCHARGED").charge_popt_overhead


def test_charged_ecg_policy_maps_to_underlying_ecg_mode():
    spec = roi_matrix.parse_policy_spec("ECG:DBG_PRIMARY_CHARGED")

    assert spec.label == "ECG_DBG_PRIMARY_CHARGED"
    assert spec.policy == "ECG"
    assert spec.ecg_mode == "DBG_PRIMARY"
    assert spec.charge_popt_overhead


def test_sniper_defaults_to_virtual_address_domain():
    args = roi_matrix.parse_args(["--suite", "sniper"])

    assert args.sniper_address_domain == "virtual"
    assert args.sniper_root == "bench/include/sniper_sim/snipersim"
    assert roi_matrix.sniper_root_path(args) == PROJECT_ROOT / "bench" / "include" / "sniper_sim" / "snipersim"
    assert args.sniper_frontend == "live"
    assert args.sniper_omp_wait_policy == "passive"
    assert args.sniper_base_config == "graphbrew/graph_sniper"
    assert args.sniper_memory_limit_gb == 16.0
    assert args.sniper_mimicos_memory_mb == "4096"
    assert args.sniper_mimicos_kernel_mb == "128"
    assert not args.allow_sniper_sg_kernel_workload


def test_sniper_root_accepts_absolute_path():
    args = roi_matrix.parse_args(["--suite", "sniper", "--sniper-root", "/tmp/snipersim-test"])

    assert roi_matrix.sniper_root_path(args) == Path("/tmp/snipersim-test")
    assert roi_matrix.sniper_runner_path(args) == Path("/tmp/snipersim-test/run-sniper")


def test_sniper_frontend_accepts_sift():
    args = roi_matrix.parse_args(["--suite", "sniper", "--sniper-frontend", "sift"])

    assert args.sniper_frontend == "sift"


def test_ecg_pfx_prefetcher_sets_cache_sim_env(tmp_path):
    args = roi_matrix.parse_args([
        "--suite", "cache-sim",
        "--prefetcher", "ECG_PFX",
        "--ecg-pfx-mode", "popt",
        "--ecg-pfx-window", "12",
        "--ecg-pfx-lookahead", "6",
    ])
    spec = roi_matrix.parse_policy_spec("ECG:POPT_PRIMARY")

    env = roi_matrix.cache_sim_env(args, spec, "4kB", "16", tmp_path / "out.json")
    row = roi_matrix.base_row("cache_sim", args, spec, "4kB")

    assert env["ECG_PREFETCH_MODE"] == "2"
    assert env["ECG_PREFETCH_WINDOW"] == "12"
    assert env["ECG_PREFETCH_LOOKAHEAD"] == "6"
    assert row["prefetcher"] == "ECG_PFX"
    assert row["ecg_prefetch_mode"] == "2"
    assert row["ecg_prefetch_window"] == "12"
    assert row["ecg_prefetch_lookahead"] == "6"


def test_next_use_lru_sets_packed_hardware_free_env(tmp_path):
    args = roi_matrix.parse_args(["--suite", "cache-sim"])
    spec = roi_matrix.parse_policy_spec("ECG:NEXT_USE_LRU")

    env = roi_matrix.cache_sim_env(
        args, spec, "512kB", "16", tmp_path / "out.json")

    assert env["ECG_MODE"] == "ECG_EXACT_STORED"
    assert env["ECG_VARIANT"] == "next_use_lru"
    assert env["ECG_NEXT_USE_RECORD"] == "1"
    assert env["ECG_NEXT_USE_LRU"] == "1"
    assert env["ECG_NEXT_USE_BITS"] == "8"
    assert env["ECG_RECORD_TIER_BITS"] == "0"
    assert env["ECG_EDGE_MASK_CHARGED"] == "1"
    assert env["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
    assert "ECG_STORED_REFRESH" not in env


def test_next_use_record_receipt_is_fail_closed():
    row = {"status": "ok"}
    assert roi_matrix.apply_next_use_record_receipt(
        row,
        "[ECG-NEXT-USE-RECORD bits=32 id_bits=18 tier_bits=0 "
        "next_bits=8 state_bits=2 records=680108]",
        required=True,
    )
    assert row["ecg_next_use_record_validated"] == 1
    assert row["ecg_record_replaces_edge"] == 1

    missing = {"status": "ok"}
    assert not roi_matrix.apply_next_use_record_receipt(
        missing, "", required=True)
    assert missing["status"] == "error"

    overflow = {"status": "ok"}
    assert not roi_matrix.apply_next_use_record_receipt(
        overflow,
        "[ECG-NEXT-USE-RECORD bits=32 id_bits=23 tier_bits=0 "
        "next_bits=8 state_bits=2 records=1]",
        required=True,
    )
    assert overflow["status"] == "error"


def test_ref32_sets_isolated_matrix_free_env(tmp_path):
    args = roi_matrix.parse_args(["--suite", "cache-sim"])
    exact = roi_matrix.parse_policy_spec("ECG:REF32_EXACT_COMMIT")
    quantized = roi_matrix.parse_policy_spec("ECG:REF32_R_COMMIT")

    exact_env = roi_matrix.cache_sim_env(
        args, exact, "512kB", "16", tmp_path / "exact.json")
    quantized_env = roi_matrix.cache_sim_env(
        args, quantized, "512kB", "16", tmp_path / "quantized.json")

    for env in (exact_env, quantized_env):
        assert env["ECG_MODE"] == "ECG_REF32"
        assert env["ECG_REF32_RECORD"] == "1"
        assert env["ECG_RECORD_TIER_BITS"] == "0"
        assert env["ECG_RECORD_POPT_BITS"] == "0"
        assert env["ECG_RECORD_PREFETCH_BITS"] == "0"
        assert env["ECG_EDGE_RECORD_BYTES"] == "4"
        assert env["ECG_EXPECT_BYTES_PER_EDGE"] == "4"
        assert env["ECG_PREFETCH_MODE"] == "0"
        assert env["ECG_REF32_DEADLINE_BITS"] == "21"
        assert "ECG_NEXT_USE_RECORD" not in env
    assert exact_env["ECG_REF32_EXACT"] == "1"
    assert "ECG_REF32_EXACT" not in quantized_env

    assert quantized_env["ECG_REF32_COMMIT_CHANNEL"] == "1"
    assert quantized_env["ECG_REF32_UPDATE_QUEUE"] == "16"
    assert quantized_env["ECG_REF32_UPDATE_LATENCY"] == "8"
    assert quantized_env["ECG_REF32_UPDATE_BANDWIDTH"] == "1"
    assert "ECG_STORED_REFRESH" not in quantized_env

    combined = roi_matrix.cache_sim_env(
        args, roi_matrix.parse_policy_spec("ECG:REF32_RP_COMMIT"),
        "512kB", "16", tmp_path / "combined.json")
    assert combined["ECG_REF32_COMMIT_CHANNEL"] == "1"
    assert combined["ECG_REF32_PREFETCH"] == "1"
    assert combined["ECG_REF32_PREFETCH_QUEUE"] == "8"
    assert combined["ECG_REF32_PREFETCH_INTERVAL"] == "8"

    scale = roi_matrix.cache_sim_env(
        args, roi_matrix.parse_policy_spec(
            "ECG:REF32_SCALE_RP_COMMIT"),
        "8MB", "16", tmp_path / "scale.json")
    assert scale["ECG_REF32_FORMAT"] == "scale6"
    assert scale["ECG_VIRTUAL_ID_BITS"] == "26"
    assert scale["ECG_REF32_DEADLINE_BITS"] == "32"
    assert scale["ECG_REF32_PREFETCH"] == "1"


def test_ref32_record_receipt_is_fail_closed():
    text = (
        "[ECG-REF32-RECORD format=full14 bits=32 id_bits=18 "
        "token_bits=0 reference_bits=8 "
        "state_bits=2 action_bits=4 exact_sidecar=0 deadline_bits=21 "
        "records=680108 actions=500000 storage=separate "
        "matrix_free=1 local_grasp=1]")
    row = {"status": "ok"}
    assert roi_matrix.apply_ref32_record_receipt(
        row, text, required=True, exact_expected=False)
    assert row["ecg_ref32_record_validated"] == 1
    assert row["ecg_record_replaces_edge"] == 1

    wrong_exact = {"status": "ok"}
    assert not roi_matrix.apply_ref32_record_receipt(
        wrong_exact, text, required=True, exact_expected=True)
    assert wrong_exact["status"] == "error"

    overflow = {"status": "ok"}
    assert not roi_matrix.apply_ref32_record_receipt(
        overflow,
        text.replace("id_bits=18", "id_bits=19"),
        required=True,
        exact_expected=False,
    )
    assert overflow["status"] == "error"

    scale = {"status": "ok"}
    assert roi_matrix.apply_ref32_record_receipt(
        scale,
        "[ECG-REF32-RECORD format=scale6 bits=32 id_bits=26 "
        "token_bits=6 reference_bits=0 state_bits=0 action_bits=0 "
        "exact_sidecar=0 deadline_bits=32 records=1468364884 "
        "actions=0 storage=inplace matrix_free=1 local_grasp=1]",
        required=True,
        expected_format="scale6",
    )

def test_ref32_commit_receipt_is_fail_closed():
    text = (
        "[ECG-REF32-COMMIT queue=16 latency=8 bandwidth=1 tag_bits=48 "
        "deadline_bits=21 state_bits=2 generated=100 coalesced=20 "
        "queue_dropped=3 applied=60 not_resident=10 expired=7 "
        "bandwidth_deferred=4 max_occupancy=16 pending=0]")
    row = {"status": "ok"}
    assert roi_matrix.apply_ref32_commit_receipt(
        row, text, required=True)
    assert row["ecg_ref32_commit_validated"] == 1

    pending = {"status": "ok"}
    assert not roi_matrix.apply_ref32_commit_receipt(
        pending, text.replace("pending=0", "pending=1"), required=True)
    assert pending["status"] == "error"

    scale = {"status": "ok"}
    assert roi_matrix.apply_ref32_commit_receipt(
        scale, text.replace("deadline_bits=21", "deadline_bits=32"),
        required=True, expected_deadline_bits=32)


def test_ref32_prefetch_receipt_is_fail_closed():
    text = (
        "[ECG-REF32-PREFETCH queue=8 latency=8 bandwidth=1 "
        "issue_interval=8 lookahead_records=16 placement=llc "
        "actions_seen=100 rate_limited=50 resident_duplicates=20 "
        "pending_duplicates=5 admission_dropped=5 queue_dropped=0 "
        "requests_issued=20 fills_completed=15 late_merged=4 "
        "completion_resident=3 completion_admission_dropped=2 "
        "bandwidth_deferred=0 max_occupancy=4 demand_displacements=1 "
        "evicted_before_use=2 pending=0]")
    row = {"status": "ok"}
    assert roi_matrix.apply_ref32_prefetch_receipt(
        row, text, required=True)
    assert row["ecg_ref32_prefetch_validated"] == 1


def test_ref32_requires_certified_dbg_order():
    assert roi_matrix.ref32_dbg_order_certified(
        "-f results/graphs/g/g-final-n18-dbg.sg -o 0 -i 1 -t 0")
    assert not roi_matrix.ref32_dbg_order_certified(
        "-f results/graphs/g/g-final-n18.sg -o 0 -i 1 -t 0")
    assert not roi_matrix.ref32_dbg_order_certified("-g 10 -i 1 -t 0")


def test_ref32_resource_receipt_is_fail_closed():
    text = (
        "[ECG-REF32-RESOURCES line_bits=24 lines=8192 "
        "line_state_bits=196608 commit_entries=16 "
        "commit_entry_bits=93 prefetch_entries=8 "
        "prefetch_entry_bits=70 lookahead_records=16 "
        "lookahead_bits=512 control_bits=64 record_extra_bits=0 "
        "total_bits=199232 popt_matrix_bits=33554432 "
        "reduction_x=168.419]")
    row = {"status": "ok"}
    assert roi_matrix.apply_ref32_resource_receipt(
        row, text, required=True)
    assert row["ecg_ref32_resource_validated"] == 1

    hidden_sideband = {"status": "ok"}
    assert not roi_matrix.apply_ref32_resource_receipt(
        hidden_sideband,
        text.replace("record_extra_bits=0", "record_extra_bits=8")
            .replace("total_bits=199232", "total_bits=199240"),
        required=True,
    )
    assert hidden_sideband["status"] == "error"


def test_ref32_detailed_backends_are_explicitly_unsupported(tmp_path):
    spec = roi_matrix.parse_policy_spec("ECG:REF32_RP_COMMIT")
    gem5_args = roi_matrix.parse_args(["--suite", "gem5"])
    gem5_row = roi_matrix.run_gem5(
        gem5_args, tmp_path, spec, "512kB")[0]
    assert gem5_row["status"] == "unsupported"
    assert "native commit-update" in gem5_row["error"]

    sniper_args = roi_matrix.parse_args(["--suite", "sniper"])
    sniper_row = roi_matrix.run_sniper(
        sniper_args, tmp_path, spec, "512kB")[0]
    assert sniper_row["status"] == "unsupported"
    assert "native commit-update" in sniper_row["error"]


def test_next_use_gem5_mechanism_is_request_bound():
    guest = (PROJECT_ROOT / "bench/src_gem5/pr.cc").read_text()
    policy = (
        PROJECT_ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.cc").read_text()
    runner = (
        PROJECT_ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()

    assert "PageRankPullGSNextUseIteration" in guest
    assert "gem5_ecg_bind_load_f32" in guest
    assert "[ECG_NEXT_USE_BIND_LOAD]" in guest
    assert "buildInEdgeNextUseRecords32Flat" in guest
    assert policy.count(
        "ecgMode == graph::ECGMode::ECG_EXACT_STORED") >= 4
    assert "graph::readEcgReusePlan" in policy
    assert "ecg_policy::effectiveFutureState" in policy
    assert "nextUseMetadataAccepts" in policy
    assert '"timing_model"] = "next_use_mechanism_probe"' in runner
    assert (
        '"timing_valid_for_speedup"] = "0"' in runner)
    assert "ECG_NEXT_USE_RECORD requires -t 0" in guest
    cache_guest = (
        PROJECT_ROOT / "bench/src_sim/pr.cc").read_text()
    assert "ECG_NEXT_USE_RECORD requires -t 0" in cache_guest


def test_ecg_pfx_gem5_returns_unsupported_without_launch(tmp_path):
    args = roi_matrix.parse_args(["--suite", "gem5", "--prefetcher", "ECG_PFX"])
    spec = roi_matrix.parse_policy_spec("LRU")

    rows = roi_matrix.run_gem5(args, tmp_path, spec, "4kB")

    assert rows[0]["status"] == "unsupported"
    assert "experimental" in rows[0]["error"]


def test_ecg_pfx_gem5_can_be_explicitly_enabled(monkeypatch, tmp_path):
    args = roi_matrix.parse_args([
        "--suite", "gem5",
        "--prefetcher", "ECG_PFX",
        "--allow-gem5-ecg-pfx",
        "--dry-run",
    ])
    spec = roi_matrix.parse_policy_spec("LRU")

    monkeypatch.setattr(roi_matrix, "run_command", lambda *call_args, **kwargs: None)

    assert roi_matrix.run_gem5(args, tmp_path, spec, "4kB") == []


def test_ecg_pfx_sniper_requires_overlays_without_launch(tmp_path, monkeypatch):
    monkeypatch.setattr(roi_matrix, "SNIPER_OVERLAY_STATUS", tmp_path / "missing_overlays.json")
    args = roi_matrix.parse_args(["--suite", "sniper", "--prefetcher", "ECG_PFX"])
    spec = roi_matrix.parse_policy_spec("LRU")

    rows = roi_matrix.run_sniper(args, tmp_path, spec, "4kB")

    assert rows[0]["status"] == "unsupported"
    assert "requires overlays" in rows[0]["error"]


def test_unsafe_sniper_memory_limit_wraps_command(monkeypatch):
    monkeypatch.setattr(roi_matrix.shutil, "which", lambda name: "/usr/bin/prlimit" if name == "prlimit" else None)

    limited = roi_matrix.memory_limited_command(["run-sniper", "--", "bench"], 1.5)

    assert limited == ["/usr/bin/prlimit", "--as=1610612736", "--", "run-sniper", "--", "bench"]


def test_sniper_memory_limit_can_be_disabled():
    cmd = ["run-sniper", "--", "bench"]

    assert roi_matrix.memory_limited_command(cmd, 0.0) == cmd


def test_run_command_timeout_returns_error_code(tmp_path):
    log_path = tmp_path / "timeout.log"

    result = roi_matrix.run_command(
        [sys.executable, "-c", "import time; time.sleep(2)"],
        PROJECT_ROOT,
        None,
        1,
        log_path,
        False,
    )

    assert result is not None
    assert result.returncode == 124
    text = log_path.read_text()
    assert "[timeout_s] 1" in text
    assert "[timeout_action] SIGTERM process group" in text
    assert "[exit_code] 124" in text


def test_gem5_sideband_paths_are_per_output_directory(tmp_path):
    row_a = tmp_path / "gem5" / "row_a"
    row_b = tmp_path / "gem5" / "row_b_with_a_much_longer_policy_named_directory"
    a = roi_matrix.gem5_sideband_paths(row_a)
    b = roi_matrix.gem5_sideband_paths(row_b)

    # Fixed sideband filenames, all four under one per-cell directory.
    assert a["context"].name == "gem5_graphbrew_ctx.json"
    assert a["out_edges"].name == "gem5_graphbrew_out_edges.bin"
    assert a["in_edges"].name == "gem5_graphbrew_in_edges.bin"
    parent = a["context"].parent
    assert a["popt_matrix"].parent == parent
    assert a["out_edges"].parent == parent
    assert a["in_edges"].parent == parent

    # Deterministic: the same out-dir yields the same paths.
    assert roi_matrix.gem5_sideband_paths(row_a) == a
    # Isolated: different out-dirs yield different sideband directories.
    assert b["context"].parent != a["context"].parent
    # Constant-length: the per-cell directory name length is independent of the
    # (policy-named) out-dir length, so the benchmark heap layout stays
    # policy-independent (only the hash hex differs, never the length).
    assert len(a["context"].parent.name) == len(b["context"].parent.name)

# ---------------------------------------------------------------------------
# Rereference-matrix stream accounting
# ---------------------------------------------------------------------------
# The 2026-07-25 review found the charged P-OPT column implausible (2.684 on
# web-Google PageRank, worse than LRU for a near-oracle policy). The cause was
# accounting, not P-OPT: its matrix stream was a flat analytic penalty that no
# prefetcher can cover, while ReusePlan's per-edge records were simulated accesses the
# prefetcher does cover. Both are sequential streams.


def test_simulated_stream_is_not_double_charged():
    """When cache_sim streams the columns, the flat charge must not be added."""
    row = {
        "options": "-i 1",
        "popt_overhead_charged": 1,
        "popt_matrix_stream_cache_lines": 229108,
        "popt_matrix_stream_lines_simulated": 229120,
        "l3_misses": 1000000,
        "total_memory_traffic": 1000000,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row["popt_matrix_stream_mode"] == "simulated"
    # Already inside the simulated totals.
    assert row["l3_misses_with_overhead"] == 1000000
    assert row["total_memory_traffic_with_overhead"] == 1000000


def test_simulated_stream_supports_single_pass_kernels_without_i_option():
    row = {
        "options": "-f graph.sg -o 5 -n 1",
        "popt_overhead_charged": 1,
        "popt_matrix_stream_cache_lines": 10,
        "popt_matrix_stream_lines_simulated": 10,
        "l3_misses": 100,
        "total_memory_traffic": 100,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row.get("status", "ok") == "ok"
    assert row["popt_matrix_stream_mode"] == "simulated"
    assert row["popt_matrix_stream_iterations"] == 1
    assert row["popt_target_time_charged"] == 0


def test_cache_sim_runtime_receipt_overrides_nominal_reuse_plan_width(tmp_path):
    args = roi_matrix.parse_args([
        "--suite", "cache-sim",
        "--benchmark", "pr",
        "--policies", "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "--options", "-g 10 -k 4 -o 5 -n 1 -i 1",
        "--l3-sizes", "32kB",
        "--ecg-epochs", "16",
        "--dry-run",
    ])
    args.dry_run = False
    args.has_lru_baseline = True
    spec = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH")
    out = tmp_path / "matrix"
    out.mkdir()
    row = roi_matrix.base_row("cache_sim", args, spec, "32kB")
    assert row["ecg_record_bytes"] == 8

    log = (
        "[ECG-METADATA kernel=pr delivery=packed stamps=2 "
        "epoch_bits=4 tier_bits=2 id_bits=10 record_bytes=4 "
        "payload_bits=10 bytes_per_edge=4.000 charged=1 flowthrough=1 "
        "packed_fits=1]\n")
    receipt = re.search(
        r"\[ECG-METADATA [^\]]*record_bytes=(\d+)"
        r"[^\]]*bytes_per_edge=([0-9.]+)[^\]]*\]", log)
    assert receipt
    row["ecg_record_bytes"] = int(receipt.group(1))
    row["edge_stream_bytes_per_edge"] = int(
        float(receipt.group(2)))
    assert row["ecg_record_bytes"] == 4
    assert row["edge_stream_bytes_per_edge"] == 4


def test_analytic_stream_is_still_charged():
    row = {
        "options": "-i 1",
        "popt_overhead_charged": 1,
        "popt_matrix_stream_cache_lines": 229108,
        "popt_matrix_stream_lines_simulated": 0,
        "l3_misses": 1000000,
        "total_memory_traffic": 1000000,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row["popt_matrix_stream_mode"] == "analytic_cumulative"
    assert row["l3_misses_with_overhead"] == 1229108
    assert row["total_memory_traffic_with_overhead"] == 1229108


def test_charged_stream_requires_an_iteration_count():
    row = {
        "popt_overhead_charged": 1,
        "popt_matrix_stream_cache_lines": 10,
        "popt_matrix_stream_lines_simulated": 0,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row["status"] == "error"
    assert "iteration count" in row["error"]


def test_uncharged_policy_pays_nothing():
    row = {
        "popt_overhead_charged": 0,
        "popt_matrix_stream_cache_lines": 229108,
        "l3_misses": 1000000,
        "total_memory_traffic": 1000000,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row["popt_matrix_stream_mode"] == "none"
    assert row["l3_misses_with_overhead"] == 1000000


def _run_cli(extra: list, suite: str = "cache-sim") -> tuple:
    import subprocess
    cmd = [
        sys.executable, str(ROI_MATRIX_PATH), "--suite", suite,
        "--benchmark", "pr", "--policies", "POPT", "--dry-run",
    ] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def test_analytic_charge_with_prefetcher_is_rejected():
    """The exact asymmetric comparison the review invalidated must fail closed."""
    code, out = _run_cli(["--prefetcher", "STRIDE"])
    assert code != 0
    assert "--popt-matrix-stream simulated" in out


def test_simulated_stream_with_prefetcher_is_allowed():
    code, out = _run_cli(
        ["--prefetcher", "STRIDE", "--popt-matrix-stream", "simulated"])
    assert code == 0, out


def test_explicit_analytic_prefetch_upper_bound_is_allowed():
    code, out = _run_cli([
        "--prefetcher", "STRIDE",
        "--popt-matrix-stream", "analytic_prefetch_upper_bound",
    ], suite="gem5")
    assert code == 0, out


def test_analytic_prefetch_upper_bound_is_rejected_for_cache_sim():
    code, out = _run_cli([
        "--prefetcher", "STRIDE",
        "--popt-matrix-stream", "analytic_prefetch_upper_bound",
    ])
    assert code != 0
    assert "gem5-only sensitivity" in out


def test_analytic_prefetch_upper_bound_is_disclosed():
    row = {
        "options": "-i 2",
        "line_size": "64",
        "popt_overhead_charged": 1,
        "popt_matrix_stream_requested":
            "analytic_prefetch_upper_bound",
        "popt_matrix_stream_cache_lines": 10,
        "dram_read_bytes": 1000,
        "dram_write_bytes": 200,
    }
    roi_matrix.apply_overhead_metrics(row)
    assert row["popt_matrix_stream_mode"] == (
        "analytic_cumulative_prefetch_upper_bound")
    assert row["popt_prefetch_upper_bound"] == 1
    assert row["popt_cumulative_stream_bytes"] == 1280

    args = roi_matrix.parse_args([
        "--suite", "gem5",
        "--policies", "LRU", "POPT",
        "--gem5-cpu-type", "O3",
        "--prefetcher", "STRIDE",
        "--popt-matrix-stream", "analytic_prefetch_upper_bound",
    ])
    args.has_lru_baseline = True
    spec = roi_matrix.parse_policy_spec("POPT")
    charge = roi_matrix.popt_charge_metadata(
        _charge_args("size_correct"), spec, "4kB")
    result = roi_matrix.base_row("gem5", args, spec, "4kB", charge)
    assert result["timing_valid_for_speedup"] == "1"
    assert result["timing_model"] == (
        "optimistic_popt_prefetch_upper_bound")
    assert "perfect matrix latency hiding" in result["timing_caveat"]
    assert result["timing_comparison_bound"] == (
        "popt_favorable_lower_bound")
    assert result["offchip_comparison_bound"] == (
        "popt_favorable_lower_bound")
    assert result["l3_miss_comparison_valid"] == 0


def test_analytic_charge_without_prefetcher_is_allowed():
    """Without a prefetcher neither stream is covered, so accounting is symmetric."""
    code, out = _run_cli([])
    assert code == 0, out
