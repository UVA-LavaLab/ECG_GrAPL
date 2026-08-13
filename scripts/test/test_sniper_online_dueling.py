"""Sniper analog of test_gem5_online_dueling.py.

Covers roi_matrix.py's Sniper-side online-dueling evidence parsing/validation
(ECG:K2_ONLINE_STREAMSHIELD), the Sniper [ECG-VARIANT-RECEIPT] attestation,
and the realized-LLC-geometry receipt -- all added to reach Sniper/gem5
parity without renaming or repurposing the frozen gem5_* fields.
"""
import argparse
import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
ROI_MATRIX = ROOT / "scripts/experiments/ecg/roi_matrix.py"
spec = importlib.util.spec_from_file_location(
    "sniper_online_dueling_roi_matrix", ROI_MATRIX)
roi_matrix = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["sniper_online_dueling_roi_matrix"] = roi_matrix
spec.loader.exec_module(roi_matrix)


def valid_sniper_row():
    return {
        "timing_valid_for_speedup": "1",
        "sniper_k2_dueling_governed_victims": 20000,
        "sniper_k2_dueling_leader_samples": 2048,
        "sniper_k2_dueling_follower_selections": 18000,
        "sniper_k2_dueling_completed_windows": 2,
        "sniper_k2_dueling_winner_changes": 0,
        "sniper_k2_dueling_follower_variant_overrides": 0,
    }


def _validate_sniper(row, required):
    return roi_matrix.validate_online_dueling_activity(
        row, required,
        positive_fields=roi_matrix.SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
        leader_samples_field="sniper_k2_dueling_leader_samples")


def test_sniper_online_dueling_activity_accepts_full_roi_window():
    row = valid_sniper_row()
    assert _validate_sniper(row, required=True)
    assert "error" not in row


def test_sniper_online_dueling_activity_rejects_partial_window():
    row = valid_sniper_row()
    row["sniper_k2_dueling_leader_samples"] = 1023
    assert not _validate_sniper(row, required=True)
    assert row["timing_valid_for_speedup"] == "0"
    assert "sniper_k2_dueling_leader_samples<1024" in row["error"]


def test_sniper_online_dueling_activity_rejects_zero_governed_victims():
    row = valid_sniper_row()
    row["sniper_k2_dueling_governed_victims"] = 0
    assert not _validate_sniper(row, required=True)
    assert "sniper_k2_dueling_governed_victims" in row["error"]


def test_sniper_online_dueling_activity_is_optional_for_static_k2():
    row = {}
    assert _validate_sniper(row, required=False)
    assert row == {}


def test_gem5_and_sniper_dueling_validation_never_cross_populate_fields():
    """The gem5 call sites must keep using gem5_* fields/defaults; a Sniper
    row missing gem5_* fields must not spuriously pass gem5's validator, and
    vice versa -- the two populations are never interchangeable."""
    sniper_row = valid_sniper_row()
    assert not roi_matrix.validate_online_dueling_activity(
        sniper_row, required=True)
    assert sniper_row["timing_valid_for_speedup"] == "0"

    gem5_row = {
        "timing_valid_for_speedup": "1",
        "gem5_k2_dueling_request_bound_victims": 20000,
        "gem5_k2_dueling_leader_samples": 2048,
        "gem5_k2_dueling_follower_selections": 18000,
        "gem5_k2_dueling_completed_windows": 2,
    }
    assert not _validate_sniper(gem5_row, required=True)


def test_sniper_variant_receipt_is_machine_validated():
    good = {"timing_valid_for_speedup": "1"}
    text = (
        "[ECG-VARIANT-RECEIPT sim=sniper requested=lru_only "
        "effective=6 dueling=0]")
    assert roi_matrix.apply_sniper_variant_receipt(
        good, text, "lru_only", required=True)
    assert good["sniper_variant_effective_receipt"] == 6

    bad = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_variant_receipt(
        bad, text, "epoch_first", required=True)
    assert bad["status"] == "error"
    assert bad["timing_valid_for_speedup"] == "0"


def test_sniper_variant_receipt_does_not_match_gem5_marker():
    """A gem5-only receipt must never satisfy the Sniper parser (and vice
    versa) -- the two must stay distinguishable by sim= tag."""
    row = {"timing_valid_for_speedup": "1"}
    gem5_text = (
        "[ECG-VARIANT-RECEIPT sim=gem5 requested=lru_only "
        "effective=6 dueling=0]")
    assert not roi_matrix.apply_sniper_variant_receipt(
        row, gem5_text, "lru_only", required=True)
    assert row["status"] == "error"

def test_sniper_geometry_receipt_matches_realized_nuca_config(tmp_path):
    sim_dir = tmp_path / "simulation"
    sim_dir.mkdir()
    (sim_dir / "sim.cfg").write_text(
        "[general]\n"
        "total_cores = 1\n"
        "\n"
        "[perf_model/nuca]\n"
        "address_hash = \"xor_mod\"\n"
        "associativity = 4\n"
        "bandwidth = 128\n"
        "cache_size = 1024\n"
        "data_access_time = 20\n"
        "enabled = \"true\"\n"
        "\n"
        "[perf_model/nuca/cache]\n")
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 1024, "4")
    assert row["sniper_l3_size_actual_kb"] == 1024
    assert row["sniper_l3_ways_actual"] == 4

    wrong = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_geometry_receipt(
        wrong, tmp_path, 2048, "16")
    assert wrong["status"] == "error"


def test_sniper_geometry_receipt_fails_closed_when_sim_cfg_missing(tmp_path):
    row = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 1024, "4")
    assert row["status"] == "error"


def test_sniper_geometry_receipt_requires_nuca_enabled_true(tmp_path):
    """[perf_model/nuca] enabled = false must fail closed even when the
    cache_size/associativity keys happen to read back as requested --
    Sniper's base.cfg ships NUCA disabled by default, so those two keys
    would otherwise be inert and could not attest the realized LLC
    geometry."""
    (tmp_path / "sim.cfg").write_text(
        "[perf_model/nuca]\n"
        "associativity = 4\n"
        "cache_size = 1024\n"
        "enabled = \"false\"\n")
    row = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 1024, "4")
    assert row["status"] == "error"
    assert row["sniper_l3_nuca_enabled"] == 0
    assert "enabled = false" in row["error"]


def test_sniper_geometry_receipt_fails_closed_when_enabled_key_missing(tmp_path):
    """No 'enabled' key at all must fail closed rather than assuming NUCA is
    on -- an ambiguous config must not be silently treated as attested."""
    (tmp_path / "sim.cfg").write_text(
        "[perf_model/nuca]\n"
        "associativity = 8\n"
        "cache_size = 32\n")
    row = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 32, "8")
    assert row["status"] == "error"
    assert "sniper_l3_nuca_enabled" not in row


def test_sniper_geometry_receipt_accepts_nuca_enabled_true(tmp_path):
    (tmp_path / "sim.cfg").write_text(
        "[perf_model/nuca]\n"
        "associativity = 8\n"
        "cache_size = 32\n"
        "enabled = \"true\"\n")
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 32, "8")
    assert row["sniper_l3_nuca_enabled"] == 1
    assert "error" not in row


def test_sniper_geometry_receipt_supports_root_level_sim_cfg(tmp_path):
    (tmp_path / "sim.cfg").write_text(
        "[perf_model/nuca]\n"
        "associativity = 8\n"
        "cache_size = 32\n"
        "enabled = \"true\"\n")
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_sniper_geometry_receipt(row, tmp_path, 32, "8")


def test_sniper_mask_mode_preserves_pinned_variant_lru_streamshield():
    """BLOCKING regression: run_sniper's mask branch must export
    ECG:K2_LRU_STREAMSHIELD's pinned "lru_only" variant unchanged, not the
    generic ECG:K2 adaptive-benchmark mapping. Before the fix,
    sniper_mask_mode_ecg_variant's mask branch unconditionally recomputed
    ECG_VARIANT via effective_ecg_variant(..., spec=parse_policy_spec(
    "ECG:K2")), silently discarding any spec-pinned variant."""
    spec = roi_matrix.parse_policy_spec("ECG:K2_LRU_STREAMSHIELD")
    assert spec.ecg_variant == "lru_only"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        args = argparse.Namespace(benchmark=benchmark)
        assert roi_matrix.sniper_mask_mode_ecg_variant(
            args, spec.ecg_schedule_k, spec) == "lru_only"


def test_sniper_mask_mode_preserves_pinned_variant_rrip_streamshield():
    spec = roi_matrix.parse_policy_spec("ECG:K2_RRIP_STREAMSHIELD")
    assert spec.ecg_variant == "rrip_first"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        args = argparse.Namespace(benchmark=benchmark)
        assert roi_matrix.sniper_mask_mode_ecg_variant(
            args, spec.ecg_schedule_k, spec) == "rrip_first"


def test_sniper_mask_mode_preserves_pinned_variant_online_streamshield():
    spec = roi_matrix.parse_policy_spec("ECG:K2_ONLINE_STREAMSHIELD")
    assert spec.ecg_variant == "rrip_first"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        args = argparse.Namespace(benchmark=benchmark)
        assert roi_matrix.sniper_mask_mode_ecg_variant(
            args, spec.ecg_schedule_k, spec) == "rrip_first"


def test_sniper_mask_mode_preserves_pinned_degree_variant():
    """ECG:K2_DEGREE pins "degree_first"; this must export/expect its own
    variant, distinct from the generic ECG:K2 adaptive mapping (which would
    also resolve to "degree_first" for bfs/sssp, but to "epoch_first" for pr
    and "rrip_first" for bc/cc -- the pin must win regardless of benchmark,
    not because the two mappings happen to coincide on some benchmarks)."""
    spec = roi_matrix.parse_policy_spec("ECG:K2_DEGREE")
    assert spec.ecg_variant == "degree_first"
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        args = argparse.Namespace(benchmark=benchmark)
        assert roi_matrix.sniper_mask_mode_ecg_variant(
            args, spec.ecg_schedule_k, spec) == "degree_first"
    # And prove the pin actually diverges from the generic adaptive mapping
    # on at least one benchmark, so this test cannot pass by coincidence.
    pr_args = argparse.Namespace(benchmark="pr")
    generic_pr = roi_matrix.effective_ecg_variant(
        pr_args, schedule_k=2,
        spec=roi_matrix.parse_policy_spec("ECG:K2"))
    assert generic_pr == "epoch_first"
    assert roi_matrix.sniper_mask_mode_ecg_variant(
        pr_args, spec.ecg_schedule_k, spec) == "degree_first"
    assert generic_pr != "degree_first"


def test_sniper_mask_mode_falls_back_to_generic_k2_when_variant_unpinned():
    """A spec with NO pinned ecg_variant (spec.ecg_variant is None) is the
    ONLY case that should fall back to the generic ECG:K2 adaptive-benchmark
    mapping in mask mode."""
    spec = roi_matrix.parse_policy_spec("ECG:ECG_GRASP_POPT")
    assert spec.ecg_variant is None
    pr_args = argparse.Namespace(benchmark="pr")
    assert roi_matrix.sniper_mask_mode_ecg_variant(
        pr_args, None, spec) == "epoch_first"
    bfs_args = argparse.Namespace(benchmark="bfs")
    assert roi_matrix.sniper_mask_mode_ecg_variant(
        bfs_args, None, spec) == "degree_first"


def _fake_sniper_metrics(stats_path: str) -> dict:
    return {
        "stats_path": stats_path,
        "l1d_loads": 1000, "l1d_load_misses": 100,
        "l2_loads": 900, "l2_load_misses": 90,
        "llc_loads": 800, "llc_load_misses": 80,
        "cycles_or_time": 12345, "instructions": 6789, "ipc_raw": 0.55,
    }


def _run_sniper_end_to_end(monkeypatch, tmp_path, log_text, write_matching_geometry=True):
    """Drive the real roi_matrix.run_sniper() integration path, not just the
    apply_sniper_variant_receipt() helper in
    isolation. Every subprocess/filesystem dependency is faked (no Sniper
    binary is invoked, no simulation launches) but the metrics-population
    row.update(), apply_sniper_geometry_receipt() and the remaining Sniper
    output-shaping code in run_sniper all execute for real, so this proves
    a bad/missing variant receipt's status="error" survives all the way to
    the row run_sniper ultimately returns -- not merely what
    apply_sniper_variant_receipt() itself returns.
    """
    args = roi_matrix.parse_args([
        "--suite", "sniper",
        "--benchmark", "pr",
        "--sniper-workload", "sg_kernel",
        "--allow-sniper-sg-kernel-workload",
    ])
    policy_spec = roi_matrix.parse_policy_spec("ECG:K2_LRU")
    assert policy_spec.ecg_variant == "lru_only"

    label = (
        f"sniper_{args.benchmark}_{policy_spec.safe_label}_"
        f"L3{roi_matrix.sanitize('32kB')}")
    sniper_out = tmp_path / "sniper" / label
    if write_matching_geometry:
        sim_dir = sniper_out / "simulation"
        sim_dir.mkdir(parents=True)
        (sim_dir / "sim.cfg").write_text(
            "[perf_model/nuca]\n"
            "associativity = 16\n"
            "cache_size = 32\n"
            "enabled = \"true\"\n")

    fake_binary = tmp_path / "fake_sg_kernel"
    fake_binary.touch()
    fake_runner = tmp_path / "fake_run-sniper"
    fake_runner.touch()
    monkeypatch.setattr(
        roi_matrix, "sniper_binary_and_options",
        lambda a: (fake_binary, ["--benchmark", a.benchmark, "-g", "10"]))
    monkeypatch.setattr(roi_matrix, "sniper_runner_path", lambda a: fake_runner)
    monkeypatch.setattr(roi_matrix, "sniper_graph_policies_enabled", lambda a: True)

    def fake_run_command(cmd, cwd, env, timeout, stdout_path, dry_run, pass_fds=()):
        import subprocess
        stdout_path.parent.mkdir(parents=True, exist_ok=True)
        stdout_path.write_text(log_text)
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(roi_matrix, "run_command", fake_run_command)

    fake_stats_path = tmp_path / "fake.stats"
    fake_stats_path.write_text("")
    monkeypatch.setattr(
        roi_matrix, "read_sniper_stats",
        lambda out: {"success": True, "stats_path": str(fake_stats_path)})
    monkeypatch.setattr(
        roi_matrix, "extract_graphbrew_metrics",
        lambda raw: _fake_sniper_metrics(str(fake_stats_path)))
    monkeypatch.delenv("ECG_K2_DELIVERY_TRACE", raising=False)

    rows = roi_matrix.run_sniper(args, tmp_path, policy_spec, "32kB")
    assert len(rows) == 1
    return rows[0]


def test_sniper_mismatched_variant_receipt_stays_error_after_metrics_population(
        monkeypatch, tmp_path):
    """BLOCKING regression: run_sniper's metrics-population row.update()
    previously wrote {"status": "ok", ...} unconditionally AFTER
    apply_sniper_variant_receipt() had already called mark_row_error() (and
    set status="error") for a mismatched [ECG-VARIANT-RECEIPT], silently
    resurrecting a failed row to "ok". This exercises the REAL run_sniper()
    end to end, through metrics population and apply_sniper_geometry_receipt,
    to prove the final returned row still reports status="error"."""
    log_text = (
        "[ECG-CONTEXT-READY sim=sniper loaded=1 regions=3 reref=0]\n"
        "[ECG-VARIANT-RECEIPT sim=sniper requested=lru_only "
        "effective=2 dueling=0]\n"
    )
    row = _run_sniper_end_to_end(monkeypatch, tmp_path, log_text)

    assert row["status"] == "error", (
        "a mismatched Sniper variant receipt must not be resurrected to "
        f"status=ok by later metrics population/output shaping; row={row}")
    assert "ECG victim variant receipt mismatch" in row["error"]
    assert row["timing_valid_for_speedup"] == "0"
    # Metrics population DID run (this is the integration point that used to
    # clobber status) -- prove it actually executed rather than short-
    # circuiting before reaching the bug.
    assert row["l1_accesses"] == 1000
    assert row["sniper_l3_size_actual_kb"] == 32


def test_sniper_missing_variant_receipt_stays_error_after_metrics_population(
        monkeypatch, tmp_path):
    """Same BLOCKING regression as above, for a completely MISSING
    [ECG-VARIANT-RECEIPT] marker (e.g. an overlay that silently stopped
    emitting the receipt) rather than a mismatched one."""
    log_text = "[ECG-CONTEXT-READY sim=sniper loaded=1 regions=3 reref=0]\n"
    row = _run_sniper_end_to_end(monkeypatch, tmp_path, log_text)

    assert row["status"] == "error", (
        "a missing Sniper variant receipt must not be resurrected to "
        f"status=ok by later metrics population/output shaping; row={row}")
    assert "ECG victim variant receipt missing" in row["error"]
    assert row["timing_valid_for_speedup"] == "0"
    assert row["l1_accesses"] == 1000


def test_sniper_valid_variant_receipt_still_reaches_status_ok(monkeypatch, tmp_path):
    """Control case: a MATCHING receipt must still reach status="ok" through
    the same real run_sniper() code path, proving the fix does not turn every
    Sniper row into a false error."""
    log_text = (
        "[ECG-CONTEXT-READY sim=sniper loaded=1 regions=3 reref=0]\n"
        "[ECG-VARIANT-RECEIPT sim=sniper requested=lru_only "
        "effective=6 dueling=0]\n"
    )
    row = _run_sniper_end_to_end(monkeypatch, tmp_path, log_text)

    assert row["status"] == "ok", row
    assert "error" not in row
    assert row["l1_accesses"] == 1000
    assert row["sniper_l3_size_actual_kb"] == 32


def test_sniper_mask_mode_variant_matches_receipt_validator_expectation():
    """End-to-end regression tying the fix to the receipt validator: the
    variant sniper_mask_mode_ecg_variant exports for a pinned K2 spec must
    be exactly what apply_sniper_variant_receipt is told to expect, so a
    run cannot silently certify a variant the child process never ran."""
    for label, expected in (
            ("ECG:K2_LRU_STREAMSHIELD", "lru_only"),
            ("ECG:K2_RRIP_STREAMSHIELD", "rrip_first"),
            ("ECG:K2_ONLINE_STREAMSHIELD", "rrip_first"),
            ("ECG:K2_DEGREE", "degree_first")):
        spec = roi_matrix.parse_policy_spec(label)
        args = argparse.Namespace(benchmark="bfs")
        exported = roi_matrix.sniper_mask_mode_ecg_variant(
            args, spec.ecg_schedule_k, spec)
        assert exported == expected, (
            f"{label} exported {exported!r}, expected the pinned "
            f"{expected!r}")
        expected_effective = {
            "grasp_only": 0, "epoch_first": 1, "rrip_first": 2,
            "epoch_only": 3, "shortcircuit": 4, "legacy": 4,
            "degree_first": 5, "traversal": 5, "lru_only": 6,
        }[expected]
        receipt_text = (
            f"[ECG-VARIANT-RECEIPT sim=sniper requested={exported} "
            f"effective={expected_effective} dueling=0]")
        row = {"timing_valid_for_speedup": "1"}
        assert roi_matrix.apply_sniper_variant_receipt(
            row, receipt_text, exported, required=True)
        assert "error" not in row
