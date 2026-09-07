"""Focused tests for the current ECG corpus and authorization workflow."""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
ECG_DIR = ROOT / "scripts/experiments/ecg"
FLOWS = ECG_DIR / "flows"
sys.path.insert(0, str(ECG_DIR))
sys.path.insert(0, str(FLOWS))

import record_equivalence_gate as gate  # noqa: E402
from record_receipts import resolve_layout  # noqa: E402
from record_resources import graph_info  # noqa: E402


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def test_bounded_sg_serializer_preserves_fixture_and_spread():
    prep = load_module(
        "record_equivalence_prep",
        FLOWS / "prepare_record_equivalence_graphs.py")
    outputs = prep.expected_outputs()
    fixture, fixture_facts = outputs["fixture32.sg"]
    directed, records, vertices = struct.unpack_from("<?qq", fixture, 0)
    assert not directed and vertices == 32 and records == 34
    assert len(fixture) == 17 + 8 * 33 + 4 * 34 + 4 * 32
    offsets = struct.unpack_from("<33q", fixture, 17)
    neighbors = struct.unpack_from("<34i", fixture, 17 + 33 * 8)
    assert list(neighbors[offsets[8]:offsets[9]]) == [3, 6, 7, 11, 18]
    assert max(neighbors) == fixture_facts["max_vertex_id"] == 20
    original_ids = struct.unpack_from("<32i", fixture, len(fixture) - 32 * 4)
    assert original_ids == tuple(range(32))

    spread, spread_facts = outputs["spread512-d4.sg"]
    directed, records, vertices = struct.unpack_from("<?qq", spread, 0)
    assert directed and vertices == 512 and records == 2048
    assert len(spread) == 17 + 2 * (8 * 513 + 4 * 2048) + 4 * 512
    assert spread_facts["max_vertex_id"] == 511
    assert outputs == prep.expected_outputs()
    _, sink_facts = prep.serialized_graph(
        8, [(0, 7), (1, 2)], directed=True)
    assert sink_facts["max_vertex_id"] == 1


def test_prepared_corpus_matches_strict_resource_inspector(tmp_path):
    prep = load_module(
        "record_equivalence_prep_inspection",
        FLOWS / "prepare_record_equivalence_graphs.py")
    config = json.loads(
        (ECG_DIR / "configs/record_equivalence.json").read_text())
    corpus = tmp_path / "results/graphs/ecg-current-equivalence"
    prep.prepare(corpus, force=False, check=False)
    for tier in config["tiers"].values():
        info = graph_info(corpus / Path(tier["path"]).name)
        assert info.sha256 == tier["sha256"]
        assert info.vertices == tier["vertices"]
        assert info.records == tier["records"]
        assert info.maximum_id == tier["max_vertex_id"]
    prep.prepare(corpus, force=False, check=True)


def test_corpus_refuses_silent_overwrite(tmp_path):
    prep = load_module(
        "record_equivalence_prep_overwrite",
        FLOWS / "prepare_record_equivalence_graphs.py")
    prep.prepare(tmp_path, force=False, check=False)
    fixture = tmp_path / "fixture32.sg"
    fixture.write_bytes(b"changed")
    with pytest.raises(RuntimeError, match="refusing to overwrite"):
        prep.prepare(tmp_path, force=False, check=False)
    prep.prepare(tmp_path, force=True, check=False)
    prep.prepare(tmp_path, force=False, check=True)


def test_graph_preflight_rejects_pinned_hash_mismatch(tmp_path):
    runner = load_module(
        "experiment_run_graph_pin",
        FLOWS / "experiment_run.py")
    graph = tmp_path / "graph.sg"
    graph.write_bytes(b"graph")
    job = runner.Job(
        job_id="graph", stage="stage", kind="roi_matrix", command=[],
        out_dir=tmp_path / "out", log_path=tmp_path / "log",
        metadata={
            "graph_path": str(graph),
            "declared_graph_sha256": "0" * 64,
        })
    assert not runner.validate_job_graphs(
        tmp_path / "run", [job], strict=True)


def test_default_profile_has_exact_canonical_roster(monkeypatch, tmp_path):
    runner = load_module(
        "experiment_run_current_roster",
        FLOWS / "experiment_run.py")
    manifest = json.loads(
        (ECG_DIR / "experiment_manifest.json").read_text())
    args = runner.parse_args([
        "--list", "--allow-missing-runtime-inputs",
        "--run-dir", str(tmp_path / "run")])
    graph_hashes = {
        "fixture32.sg":
            "d3367321de8541ffbe7390e02d132308c407dd06e48e765e069d82b5f136aa9a",
        "spread512-d4.sg":
            "717b795ae24513d0710a1a1b93da2f4d8f28954f9b0c4a0e4c77365bacf51306",
    }
    monkeypatch.setattr(
        runner, "path_fingerprint",
        lambda path: graph_hashes.get(Path(path).name, "input-hash"))
    jobs = runner.expand_jobs(args, manifest, tmp_path / "run")
    assert args.profile == ["ecg_current_equivalence"]
    assert len(jobs) == 12
    assert sum(len(job.metadata["policies"]) for job in jobs) == 36
    assert {job.metadata["suite"] for job in jobs} == {
        "cache-sim", "gem5", "sniper"}
    assert {int(job.command[job.command.index("--ecg-record-bytes") + 1])
            for job in jobs} == {4, 8}
    for job in jobs:
        command = job.command
        assert "--ecg-equivalence" in command
        assert "--ecg-record-max-carrier-bytes" in command
        assert "--ecg-record-max-auxiliary-bytes" in command
        assert job.metadata["declared_graph_sha256"]
        assert "record_process_watchdog" in job.metadata["input_fingerprints"]
        assert job.metadata["process_tree_rss_mib"] == 2048
        assert job.metadata["process_tree_timeout_s"] > 0
        assert "record_resources" in job.metadata["input_fingerprints"]
        assert "ecg_record_evidence" in job.metadata["input_fingerprints"]
        options = command[command.index("--options") + 1]
        assert options.startswith("-f ") and "-g " not in options
        assert "-o 0 -n 1 -i 2 -t 0" in options
        if job.metadata["suite"] == "gem5":
            assert "--gem5-mem-size" in command
            assert "--gem5-record-rss-mib" in command
        if job.metadata["suite"] == "cache-sim":
            index = command.index("--cache-record-rss-mib")
            assert command[index + 1] == "2048"
        if job.metadata["suite"] == "sniper":
            assert "--sniper-record-rss-mib" in command
            assert command[command.index("--sniper-workload") + 1] == "sg_kernel"
            assert command[command.index("--sniper-frontend") + 1] == "sift"
            assert command[command.index("--sniper-address-domain") + 1] == "translated"
            assert "--allow-sniper-sg-kernel-workload" not in command
        assert not any(part.startswith("--allow-") for part in command)
    wrapped, wrapper_log = runner.bounded_job_command(jobs[0], args)
    assert wrapped[1] == str(runner.PROCESS_TREE_WATCHDOG)
    assert wrapped[wrapped.index("--rss-mib") + 1] == "2048"
    assert wrapped[wrapped.index("--seconds") + 1] == str(
        jobs[0].metadata["process_tree_timeout_s"])
    assert wrapped[wrapped.index("--") + 1:] == jobs[0].command
    assert wrapper_log.name.endswith(".runner")
    scratch = (jobs[0].out_dir / "scratch").resolve()
    environment = runner.clean_job_environment({
        runner.JOB_SCRATCH_ENV: str(scratch),
        "TMPDIR": str(scratch),
    })
    assert environment["TMPDIR"] == str(scratch)
    assert environment[runner.JOB_SCRATCH_ENV] == str(scratch)


def test_large_and_final_profiles_have_distinct_authority():
    manifest = json.loads(
        (ECG_DIR / "experiment_manifest.json").read_text())
    contract = json.loads(
        (ECG_DIR / "configs/record_equivalence.json").read_text())
    stages = manifest["stages"]
    large = [
        stage for stage in stages
        if "ecg_large_cache" in stage.get("profiles", [])]
    final = [
        stage for stage in stages
        if "ecg_detailed_final" in stage.get("profiles", [])]
    assert len(large) == 1 and large[0]["suite"] == "cache-sim"
    assert not large[0].get("requires_current_equivalence", False)
    assert large[0]["cache_record_rss_mib"] == 8192
    assert large[0]["ecg_record_max_carrier_bytes"] == 2147483648
    assert large[0]["ecg_record_max_auxiliary_bytes"] == 1073741824
    assert {stage["suite"] for stage in final} == {"gem5", "sniper"}
    assert all(stage["requires_current_equivalence"] is True and
               stage["requires_explicit_final"] is True
               for stage in final)
    assert manifest["profile_controls"]["ecg_smoke"]["status"] == "historical"
    assert "cannot authorize a current ECG final stage" in (
        manifest["historical_profile_note"])
    assert "bench/include/ecg_record_evidence.h" in contract["source_paths"]
    assert "scripts/experiments/ecg/record_resources.py" in (
        contract["source_paths"])


def test_local_release_matrix_keeps_all_reference_and_mechanism_roles():
    manifest = json.loads((ECG_DIR / "experiment_manifest.json").read_text())
    stages = [stage for stage in manifest["stages"]
              if "ecg_local_release_cache" in stage.get("profiles", [])]
    assert len(stages) == 1
    stage = stages[0]
    assert stage["suite"] == "cache-sim" and stage["current_pr_baselines"] is True
    assert stage["policy_sharding_allowed"] is False
    assert stage["policies"] == [
        "LRU", "SRRIP", "GRASP_PAPER", "POPT:CHARGED", "POPT_SE", "POPT_SE_DISTANT",
        "ECG:transport", "ECG:replacement", "ECG:prefetch", "ECG"]
    assert stage["l3_sizes"] == ["8MB"] and stage["l3_ways"] == "16"
    assert stage["popt_matrix_stream"] == "simulated"
    assert stage["cache_record_rss_mib"] == 8192 and stage["timeout_cache"] == 3600
    graphs = manifest["graph_sets"][stage["graph_set"]]
    assert [graph["name"] for graph in graphs] == [
        "web-Google", "roadNet-CA", "cit-Patents", "soc-pokec", "soc-LiveJournal1", "com-Orkut"]
    assert all(graph["options_key"] == "file_pr_current_i2" and
               len(graph["expected_sha256"]) == 64 for graph in graphs)
    assert len(stage["policies"]) * len(graphs) == 60
    assert not stage.get("requires_current_equivalence", False)


def test_twitter_reproduction_preserves_archive_controls_and_scale_budget():
    manifest = json.loads((ECG_DIR / "experiment_manifest.json").read_text())
    stages = [stage for stage in manifest["stages"]
              if "ecg_twitter_reproduction" in stage.get("profiles", [])]
    assert len(stages) == 1
    stage = stages[0]
    assert stage["suite"] == "cache-sim" and stage["current_pr_baselines"] is True
    assert stage["policies"] == [
        "ECG:transport", "LRU", "SRRIP", "GRASP_PAPER", "POPT:UNCHARGED", "POPT:CHARGED",
        "POPT_SE", "POPT_SE_DISTANT", "ECG:replacement", "ECG"]
    assert (stage["l1d_size"], stage["l2_size"], stage["l3_sizes"]) == (
        "32kB", "128kB", ["8MB", "24MB"])
    assert stage["l1d_ways"] == stage["l2_ways"] == "8" and stage["l3_ways"] == "16"
    assert stage["popt_matrix_stream"] == "analytic" and stage["popt_reserve_model"] == "size_correct"
    assert stage["ecg_record_bytes"] == 0 and stage["ecg_record_minimum_mantissa_bits"] == 0
    assert stage["cache_record_rss_mib"] == 32768 and stage["timeout_cache"] == 7200
    assert stage["process_tree_timeout_seconds"] == 43200 and stage["policy_sharding_allowed"] is False
    assert stage["reference_archive_sha256"] == "6ee0e0c21bf582f55b0ef6a4c1d8c7544348558eaccc7b4cb9454c6352b2e124"
    graphs = manifest["graph_sets"][stage["graph_set"]]
    assert len(graphs) == 1 and graphs[0]["expected_records"] == 1468364884
    assert graphs[0]["expected_vertices"] == 41652230
    assert len(graphs[0]["expected_sha256"]) == 64
    assert stage["ecg_record_max_carrier_bytes"] >= 4 * graphs[0]["expected_records"]
    assert manifest["benchmark_options"][graphs[0]["options_key"]]["pr"] == (
        "-f {graph_path} -o 0 -n 1 -i 1 -t 0")
    assert not stage.get("requires_current_equivalence", False)


def make_rows() -> tuple[
        dict[tuple[str, str, str, int, str, str], dict], list[dict[str, str]], dict]:
    manifest = json.loads((ECG_DIR / "experiment_manifest.json").read_text())
    contract = json.loads(
        (ECG_DIR / "configs/record_equivalence.json").read_text())
    expected = gate.expected_roster(manifest, contract)
    rows = []
    graph_digests = {
        "ecg-fixture32": ("1111111111111111", "2222222222222222", "aaaabbbbccccdddd"),
        "ecg-spread512-d4": ("3333333333333333", "4444444444444444", "eeeeffff00001111"),
    }
    for key, specification in expected.items():
        stage, backend, graph, width, label, l3 = key
        tier = contract["tiers"][specification["tier"]]
        layout = resolve_layout(
            records=tier["records"], vertices=tier["vertices"],
            maximum_id=tier["max_vertex_id"], traversals=2,
            requested_bytes=width)
        source_digest, destination_digest, checksum = graph_digests[graph]
        width_digit = f"{width:016x}"
        row = {
            "_equivalence_stage": stage,
            "_equivalence_graph": graph,
            "simulator": backend,
            "benchmark": "pr",
            "policy_label": label,
            "l3_size": l3,
            "status": "ok",
            "timing_valid_for_speedup": "0",
            "ecg_equivalence_only": "1",
            "ecg_equivalence_schema": "ecg.record-stream",
            "ecg_record_contract_valid": "1",
            "ecg_record_requested_bytes": str(width),
            "ecg_records": str(tier["records"]),
            "ecg_vertex_count": str(tier["vertices"]),
            "ecg_max_vertex_id": str(tier["max_vertex_id"]),
            "ecg_record_mechanism": specification["mechanism"],
            "ecg_graph_sha256": tier["sha256"],
            "ecg_source_order_digest": source_digest,
            "ecg_carrier_digest": width_digit,
            "ecg_consumed_semantic_digest": width_digit,
            "ecg_destination_stream_digest": destination_digest,
            "ecg_window_reference_digest": width_digit,
            "ecg_property_read_count": str(tier["records"] * 2),
            "pr_iterations": "2",
            "pr_semantic_edges": str(tier["records"] * 2),
            "pr_score_checksum": checksum,
        }
        row.update({"ecg_" + field: str(value)
                    for field, value in layout.items()})
        prefix = {
            "cache_sim": "ecg_functional_",
            "gem5": "ecg_prefetch_",
            "sniper": "ecg_sniper_",
        }[backend]
        if backend == "gem5":
            row[prefix + "candidates"] = "1"
            row[prefix + "property_reads"] = "1"
            row[prefix + "property_responses"] = "1"
        else:
            row[prefix + "prefetch_candidates"] = "1"
            row[prefix + "prefetch_issued"] = "1"
            row[prefix + "prefetch_fills"] = "1"
        if specification["tier"] == "micro":
            if backend == "gem5":
                row[prefix + "candidates"] = "0"
                row[prefix + "property_reads"] = "0"
                row[prefix + "property_responses"] = "0"
            else:
                row[prefix + "prefetch_candidates"] = "0"
                row[prefix + "prefetch_issued"] = "0"
                row[prefix + "prefetch_fills"] = "0"
        rows.append(row)
    return expected, rows, contract


def test_equivalence_rows_require_full_roster_and_correct_grouping():
    expected, rows, contract = make_rows()
    summary = gate.validate_rows(rows, expected, contract)
    assert summary["row_count"] == 36
    assert len(summary["roster_sha256"]) == 64
    assert len(summary["rows_sha256"]) == 64

    partial = rows[:-1]
    with pytest.raises(gate.EquivalenceGateError, match="roster"):
        gate.validate_rows(partial, expected, contract)

    changed = [dict(row) for row in rows]
    changed[1]["ecg_consumed_semantic_digest"] = "9999999999999999"
    with pytest.raises(gate.EquivalenceGateError, match="within-width"):
        gate.validate_rows(changed, expected, contract)

    disabled = [dict(row) for row in rows]
    disabled[0]["ecg_equivalence_only"] = "0"
    with pytest.raises(gate.EquivalenceGateError, match="instrumentation"):
        gate.validate_rows(disabled, expected, contract)


def test_final_stage_cannot_bypass_authorization(monkeypatch, tmp_path):
    runner = load_module(
        "experiment_run_current_final",
        FLOWS / "experiment_run.py")
    job = runner.Job(
        job_id="final", stage="140_current_detailed_final_gem5",
        kind="roi_matrix", command=[], out_dir=tmp_path / "out",
        log_path=tmp_path / "log",
        metadata={"requires_current_equivalence": True})
    base = dict(
        profile=["ecg_detailed_final"], list=False, dry_run=False,
        check_graphs=False, graph=[], benchmark=[], policy=[], job=[],
        only=[], skip=[], from_job="", limit=0, allow_blocked=True,
        final_stage=False, equivalence_receipt="")
    with pytest.raises(SystemExit, match="explicit --final-stage"):
        runner.validate_current_final_authorization(
            argparse.Namespace(**base), [job], ECG_DIR / "experiment_manifest.json")

    receipt = tmp_path / "receipt.json"
    receipt.write_text("{}")
    base.update(final_stage=True, equivalence_receipt=str(receipt))
    monkeypatch.setattr(
        runner, "validate_equivalence_receipt",
        lambda *_args: {"sha256": "ok", "input_fingerprints": {}})
    assert runner.validate_current_final_authorization(
        argparse.Namespace(**base), [job],
        ECG_DIR / "experiment_manifest.json")["sha256"] == "ok"
    base["only"] = ["140"]
    with pytest.raises(SystemExit, match="partial selection"):
        runner.validate_current_final_authorization(
            argparse.Namespace(**base), [job],
            ECG_DIR / "experiment_manifest.json")


def test_partial_equivalence_run_cannot_emit_authorization():
    runner = load_module(
        "experiment_run_partial_equivalence",
        FLOWS / "experiment_run.py")
    base = dict(
        profile=["ecg_current_equivalence"], graph=[], benchmark=[],
        policy=[], job=[], only=[], skip=[], from_job="", limit=0)
    assert runner.can_write_current_equivalence_receipt(
        argparse.Namespace(**base), True)
    for key, value in (
            ("graph", ["ecg-fixture32"]), ("benchmark", ["pr"]),
            ("policy", ["ECG"]), ("job", ["fixture"]), ("only", ["110"]),
            ("skip", ["121"]), ("from_job", "112"), ("limit", 1)):
        filtered = dict(base)
        filtered[key] = value
        assert not runner.can_write_current_equivalence_receipt(
            argparse.Namespace(**filtered), True)
    assert not runner.can_write_current_equivalence_receipt(
        argparse.Namespace(**base), False)


def test_missing_graph_override_is_planning_only():
    runner = load_module(
        "experiment_run_missing_graph_scope",
        FLOWS / "experiment_run.py")
    with pytest.raises(SystemExit, match="valid only"):
        runner.main([
            "--profile", "ecg_smoke",
            "--allow-missing-graphs"])
    assert runner.parse_args(
        ["--allow-missing-graphs", "--list"]).list


@pytest.mark.parametrize("flags", [[], ["--list"], ["--dry-run"], ["--check-graphs"]])
def test_locked_run_cannot_rewrite_existing_evidence(monkeypatch, tmp_path, flags):
    from flows import experiment_run as runner

    run = tmp_path / "run"
    run.mkdir()
    lock = tmp_path / "run.lock"
    job = runner.Job(
        job_id="held", stage="held", kind="roi_matrix", command=[],
        out_dir=run / "matrix", log_path=run / "job.log")
    manifest = run / "resolved_manifest.json"
    manifest.write_text(json.dumps({"jobs": [runner.job_snapshot(job)], "sentinel": "keep"}))
    completion = run / "run.complete.json"
    completion.write_text('{"sentinel":"completed evidence"}\n')
    saved = {path: path.read_bytes() for path in (manifest, completion)}
    monkeypatch.setattr(runner, "expand_jobs", lambda *_args: [job])
    monkeypatch.setattr(runner, "run_job", lambda *_args: pytest.fail("locked job executed"))
    with runner.run_lock(lock):
        owner = lock.read_bytes()
        code = runner.main([
            "--profile", "ecg_smoke", "--run-dir", str(run),
            "--lock-path", str(lock), "--no-build", *flags])
        assert code == 2
        assert {path: path.read_bytes() for path in saved} == saved
        assert lock.read_bytes() == owner


def test_run_lock_covers_completion_publication(monkeypatch, tmp_path):
    import fcntl
    from flows import experiment_run as runner

    lock = tmp_path / "publish.lock"
    job = runner.Job(
        job_id="publish", stage="publish", kind="roi_matrix", command=[],
        out_dir=tmp_path / "matrix", log_path=tmp_path / "job.log")
    monkeypatch.setattr(runner, "expand_jobs", lambda *_args: [job])
    monkeypatch.setattr(runner, "write_run_manifest", lambda *_args: None)
    monkeypatch.setattr(runner, "write_preflight", lambda *_args: None)
    monkeypatch.setattr(runner, "validate_job_graphs", lambda *_args, **_kwargs: True)
    monkeypatch.setattr(runner, "run_job", lambda *_args: 0)
    monkeypatch.setattr(runner, "write_combined_outputs", lambda *_args: None)

    def publish(*_args, **_kwargs):
        with lock.open("a+") as handle:
            with pytest.raises(BlockingIOError):
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True

    monkeypatch.setattr(runner, "write_run_completion", publish)
    assert runner.main([
        "--profile", "ecg_smoke", "--run-dir", str(tmp_path / "run"),
        "--lock-path", str(lock), "--no-build"]) == 0


def test_job_output_tampering_is_rejected(monkeypatch, tmp_path):
    monkeypatch.setattr(gate, "validate_raw_row", lambda *_args: {})
    out = tmp_path / "matrix"
    out.mkdir()
    csv_path = out / "roi_matrix.csv"
    json_path = out / "roi_matrix.json"
    marker_path = out / "roi_matrix.complete.json"
    log_path = tmp_path / "job.log"
    csv_path.write_text("status,policy_label\nok,ECG\n")
    json_path.write_text('[{"status":"ok","policy_label":"ECG"}]\n')
    log_path.write_text("validated\n[watchdog_reason] exit\n[watchdog_returncode] 0\n")
    marker_path.write_text(json.dumps({
        "complete": True,
        "all_rows_ok": True,
        "config_hash": "config",
        "outputs": {
            "roi_matrix.csv": gate.output_descriptor(csv_path),
            "roi_matrix.json": gate.output_descriptor(json_path),
        },
    }))
    resolved = {"jobs": [{
        "stage": "stage",
        "out_dir": str(out),
        "log_path": str(log_path),
        "metadata": {"config_hash": "config", "graph": "graph"},
    }]}
    rows, _ = gate.verify_job_outputs(resolved)
    assert len(rows) == 1
    csv_path.write_text("status,policy_label\nok,ECG_TRANSPORT\n")
    with pytest.raises(gate.EquivalenceGateError, match="changed"):
        gate.verify_job_outputs(resolved)


def test_authorization_recomputes_instead_of_trusting_valid(monkeypatch, tmp_path):
    run_dir = tmp_path / "results/run"
    run_dir.mkdir(parents=True)
    receipt = run_dir / gate.RECEIPT_NAME
    recorded = {"valid": True, "run_dir": str(run_dir), "rows_sha256": "old"}
    receipt.write_text(json.dumps(recorded))
    monkeypatch.setattr(
        gate, "build_receipt",
        lambda *_args: {"valid": True, "run_dir": str(run_dir),
                        "rows_sha256": "new"})
    with pytest.raises(gate.EquivalenceGateError, match="stale"):
        gate.validate_authorization(
            receipt, tmp_path, ECG_DIR / "experiment_manifest.json",
            ECG_DIR / "configs/record_equivalence.json")


def test_completion_descriptor_matches_production_json_row_count(tmp_path):
    path = tmp_path / "roi_matrix.json"
    path.write_text('[{"status":"ok"},{"status":"ok"}]\n')
    assert gate.output_descriptor(path) == {
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "size": path.stat().st_size,
        "rows": 2,
    }


def test_policy_specific_json_fields_match_union_column_csv(monkeypatch, tmp_path):
    monkeypatch.setattr(gate, "validate_raw_row", lambda *_args: {}, raising=False)
    csv_path = tmp_path / "roi_matrix.csv"
    json_path = tmp_path / "roi_matrix.json"
    csv_path.write_text("status,policy_label,prefetch_fills\nok,ECG_TRANSPORT,\nok,ECG,1\n")
    json_path.write_text(json.dumps([
        {"status": "ok", "policy_label": "ECG_TRANSPORT"},
        {"status": "ok", "policy_label": "ECG", "prefetch_fills": 1},
    ]))
    (tmp_path / "roi_matrix.complete.json").write_text(json.dumps({
        "complete": True, "all_rows_ok": True, "config_hash": "configuration",
        "outputs": {path.name: gate.output_descriptor(path) for path in (csv_path, json_path)},
    }))
    log = tmp_path / "job.log"
    log.write_text("[watchdog_reason] exit\n[watchdog_returncode] 0\n")
    resolved = {"jobs": [{
        "stage": "current", "out_dir": str(tmp_path), "log_path": str(log),
        "metadata": {"config_hash": "configuration", "graph": "fixture"},
    }]}
    rows, _outputs = gate.verify_job_outputs(resolved, tmp_path)
    assert len(rows) == 2 and rows[0]["prefetch_fills"] == ""


def test_gate_rejects_omitted_runtime_inputs():
    with pytest.raises(gate.EquivalenceGateError, match="input"):
        gate.verify_job_inputs(ROOT, {"jobs": [{
            "job_id": "native", "metadata": {"input_paths": {}, "input_fingerprints": {}},
        }]})


def test_equal_but_invalid_layouts_do_not_establish_equivalence():
    expected, rows, contract = make_rows()
    for row in rows:
        row["ecg_id_bits"] = "1"
    with pytest.raises(gate.EquivalenceGateError, match="layout"):
        gate.validate_rows(rows, expected, contract)


def test_final_rejects_unqualified_runtime_path(monkeypatch, tmp_path):
    runner = load_module("experiment_run_unqualified_runtime", FLOWS / "experiment_run.py")
    binary = tmp_path / "different/gem5.opt"
    job = runner.Job(
        job_id="final", stage="140_current_detailed_final_gem5", kind="roi_matrix",
        command=[], out_dir=tmp_path / "out", log_path=tmp_path / "log",
        metadata={
            "requires_current_equivalence": True,
            "input_paths": {"gem5_binary": str(binary)},
            "input_fingerprints": {"gem5_binary": "new-binary"},
        })
    args = runner.parse_args([
        "--profile", "ecg_detailed_final", "--final-stage",
        "--equivalence-receipt", str(tmp_path / gate.RECEIPT_NAME)])
    monkeypatch.setattr(runner, "validate_equivalence_receipt", lambda *_args: {
        "input_fingerprints": {str(tmp_path / "qualified/gem5.opt"): "old-binary"},
    })
    with pytest.raises(SystemExit, match="not qualified"):
        runner.validate_current_final_authorization(args, [job], ECG_DIR / "experiment_manifest.json")


def test_final_profile_cannot_drop_its_authorization_flags(tmp_path):
    runner = load_module("experiment_run_final_profile_guard", FLOWS / "experiment_run.py")
    args = runner.parse_args(["--profile", "ecg_detailed_final", "--allow-blocked"])
    job = runner.Job(
        job_id="final", stage="140_current_detailed_final_gem5", kind="roi_matrix",
        command=[], out_dir=tmp_path / "out", log_path=tmp_path / "log", metadata={})
    with pytest.raises(SystemExit, match="explicit --final-stage"):
        runner.validate_current_final_authorization(args, [job], ECG_DIR / "experiment_manifest.json")


@pytest.mark.parametrize("changed", ["command", "environment", "input_paths"])
def test_resolved_jobs_must_match_canonical_expansion(monkeypatch, tmp_path, changed):
    from flows import experiment_run as runner

    manifest_path = ECG_DIR / "experiment_manifest.json"
    manifest = json.loads(manifest_path.read_text())
    contract = json.loads((ECG_DIR / "configs/record_equivalence.json").read_text())
    graph_hashes = {
        Path(tier["path"]).name: tier["sha256"] for tier in contract["tiers"].values()
    }
    monkeypatch.setattr(
        runner, "compute_path_fingerprint",
        lambda path: graph_hashes.get(path.name, "f" * 64))
    runner.path_fingerprint.cache_clear()
    args = runner.parse_args(["--no-build", "--run-dir", str(tmp_path)])
    jobs = runner.expand_jobs(args, manifest, tmp_path)
    runner.write_run_manifest(tmp_path, args, manifest, jobs)
    resolved = json.loads((tmp_path / "resolved_manifest.json").read_text())
    gate.verify_resolved_jobs(ROOT, tmp_path, manifest_path, manifest, contract, resolved)
    if changed == "command":
        resolved["jobs"][0]["command"].append("--dry-run")
    elif changed == "environment":
        resolved["jobs"][0]["metadata"]["env"]["ECG_RECORD_EQUIVALENCE"] = "0"
    else:
        resolved["jobs"][0]["metadata"]["input_paths"].pop("cache_sim_benchmark_binary")
    with pytest.raises(gate.EquivalenceGateError, match="canonical"):
        gate.verify_resolved_jobs(ROOT, tmp_path, manifest_path, manifest, contract, resolved)
    runner.path_fingerprint.cache_clear()


def test_raw_record_receipts_are_replayed_and_bound(tmp_path):
    from scripts.test.test_ecg_record_receipts import native_fixture, text_for
    from record_receipts import validate_gem5_record, validate_equivalence

    guest, runtime = native_fixture()
    evidence = (
        "[ECG-RECORD-EQUIVALENCE schema=ecg.record-stream observer=actual-record-load "
        "property_read_count=34 " + " ".join(
            f"{name.removeprefix('ecg_')}=0123456789abcdef" for name in gate.DIGEST_FIELDS) + "]"
    )
    native_output = tmp_path / "gem5"
    native_output.mkdir()
    stderr = native_output / "benchmark_stderr.txt"
    text = text_for(guest, runtime) + "\n" + evidence
    stderr.write_text(text)
    log = tmp_path / "native.log"
    log.write_text("[watchdog_reason] exit\n[watchdog_returncode] 0\n")
    fields = validate_gem5_record(text, mechanism="replacement", requested_bytes=4)
    fields.update(validate_equivalence(text, 34))
    row = {
        "simulator": "gem5", "log_path": str(log), "gem5_out": str(native_output),
        "ecg_record_mechanism": "replacement", "ecg_record_requested_bytes": "4",
        "ecg_record_minimum_mantissa_bits": "0",
        "ecg_record_max_carrier_bytes": str(256 << 20),
        "ecg_record_max_auxiliary_bytes": str(256 << 20),
        **{key if key.startswith("pr_") or key == "method" else "ecg_" + key: str(value)
           for key, value in fields.items()},
    }
    outputs = gate.validate_raw_row(row, tmp_path)
    assert str(stderr) in outputs and str(log) in outputs
    with pytest.raises(gate.EquivalenceGateError, match="archived row"):
        gate.validate_raw_row({**row, "ecg_mantissa_bits": "0"}, tmp_path)
    stderr.write_text(text.replace("pending=0", "pending=1"))
    with pytest.raises(gate.EquivalenceGateError, match="raw record evidence"):
        gate.validate_raw_row(row, tmp_path)
    stderr.unlink()
    with pytest.raises(gate.EquivalenceGateError, match="raw record evidence"):
        gate.validate_raw_row(row, tmp_path)


def test_complete_equivalence_receipt_revalidates_archived_rows(
        monkeypatch, tmp_path):
    expected, rows, contract = make_rows()
    contract = dict(contract)
    contract["source_paths"] = []
    manifest = json.loads(
        (ECG_DIR / "experiment_manifest.json").read_text())
    for tier in contract["tiers"].values():
        tier["sha256"] = "f" * 64
    for graph_set in (
            "current_equivalence_fixture", "current_equivalence_spread"):
        manifest["graph_sets"][graph_set][0]["expected_sha256"] = "f" * 64

    manifest_path = tmp_path / "manifest.json"
    contract_path = tmp_path / "contract.json"
    manifest_path.write_text(json.dumps(manifest))
    contract_path.write_text(json.dumps(contract))
    jobs = []
    manifest_stages = {
        stage["name"]: {**manifest["defaults"], **stage}
        for stage in manifest["stages"]}
    by_stage: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        row = dict(row)
        row["ecg_graph_sha256"] = "f" * 64
        by_stage.setdefault(row["_equivalence_stage"], []).append(row)
    for stage, stage_rows in by_stage.items():
        stage_spec = manifest_stages[stage]
        timeout_key = {
            "cache-sim": "timeout_cache",
            "gem5": "timeout_gem5",
            "sniper": "timeout_sniper",
        }[stage_spec["suite"]]
        rss_key = {
            "cache-sim": "cache_record_rss_mib",
            "gem5": "gem5_record_rss_mib",
            "sniper": "sniper_record_rss_mib",
        }[stage_spec["suite"]]
        cells = len(stage_spec["policies"]) * len(stage_spec["l3_sizes"])
        out = tmp_path / "run" / "matrices" / stage
        out.mkdir(parents=True)
        clean_rows = [
            {key: value for key, value in row.items()
             if not key.startswith("_equivalence_")}
            for row in stage_rows
        ]
        fields = sorted({key for row in clean_rows for key in row})
        csv_path = out / "roi_matrix.csv"
        with csv_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(clean_rows)
        json_path = out / "roi_matrix.json"
        json_path.write_text(json.dumps(clean_rows))
        marker_path = out / "roi_matrix.complete.json"
        log_path = tmp_path / "run" / "logs" / f"{stage}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("validated\n[watchdog_reason] exit\n[watchdog_returncode] 0\n")
        marker_path.write_text(json.dumps({
            "complete": True,
            "all_rows_ok": True,
            "config_hash": stage,
            "outputs": {
                "roi_matrix.csv": gate.output_descriptor(csv_path),
                "roi_matrix.json": gate.output_descriptor(json_path),
            },
        }))
        jobs.append({
            "job_id": stage,
            "stage": stage,
            "out_dir": str(out),
            "log_path": str(log_path),
            "metadata": {
                "config_hash": stage,
                "graph": stage_rows[0]["_equivalence_graph"],
                "input_paths": {"manifest": str(manifest_path)},
                "input_fingerprints": {"manifest": gate.file_sha256(manifest_path)},
                "process_tree_rss_mib": stage_spec[rss_key],
                "process_tree_timeout_s":
                    stage_spec[timeout_key] * cells + 30,
            },
        })
    run_dir = tmp_path / "run"
    resolved = {
        "profiles": ["ecg_current_equivalence"],
        "filters": {
            "graph": [], "benchmark": [], "policy": [], "job": [],
            "only": [], "skip": [], "from_job": "", "limit": 0,
        },
        "manifest": manifest,
        "git_head": "head",
        "run_config_hash": "run-hash",
        "jobs": jobs,
    }
    (run_dir / "resolved_manifest.json").write_text(json.dumps(resolved))
    (run_dir / "run.complete.json").write_text(json.dumps({
        "complete": True, "run_config_hash": "run-hash"}))
    monkeypatch.setattr(gate, "contained", lambda _path, _parent: True)
    monkeypatch.setattr(gate, "verify_resolved_jobs", lambda *_args: None, raising=False)
    monkeypatch.setattr(gate, "validate_raw_row", lambda *_args: {})
    monkeypatch.setattr(
        gate, "validate_corpus",
        lambda *_args: {"path": "corpus", "sha256": "c" * 64})
    receipt_path = gate.write_receipt(
        ROOT, run_dir, manifest_path, contract_path)
    validated = gate.validate_authorization(
        receipt_path, ROOT, manifest_path, contract_path)
    assert validated["rows_sha256"]

    first_csv = Path(jobs[0]["out_dir"]) / "roi_matrix.csv"
    first_csv.write_text(first_csv.read_text() + "\n")
    with pytest.raises(gate.EquivalenceGateError, match="changed"):
        gate.validate_authorization(
            receipt_path, ROOT, manifest_path, contract_path)
