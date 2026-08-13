import csv
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.experiments.ecg.verify.matched_k2m import validate  # noqa: E402


def write_rows(
        path: Path, ratio: float = 1.001,
        binding_validated: bool = True) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "policy_label", "status", "instructions",
        "sniper_transport_matched", "sniper_k2_exact_bind",
        "sniper_k2_epoch_context_bound",
        "ecg_isa_variant", "sniper_workload",
        "sniper_roi_icount", "timing_valid_for_speedup",
        "sniper_workload_sha256", "sniper_simulator_sha256",
        "benchmark", "options", "prefetcher",
        "l1d_size", "l2_size", "l3_size", "l3_ways", "threads",
        "sniper_cores", "sniper_cache_warming",
        "sniper_transport_record_bytes",
        "sniper_transport_bytes_per_edge",
        "ecg_record_bytes", "edge_stream_bytes_per_edge",
        "ecg_record_replaces_edge",
        "sniper_semantic_result", "log_path",
        "sniper_semantic_edge_limit", "sniper_semantic_edge_visits",
        "sniper_semantic_truncated", "semantic_work_matched",
        "sniper_transport_receipts_validated",
        "sniper_k2_exact_bind_validated",
        "sniper_k2_epoch_context_validated",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerow({
            "policy_label": "LRU",
            "status": "ok",
            "instructions": "100000",
            "sniper_transport_matched": "1",
            "sniper_k2_exact_bind": "1",
            "sniper_k2_epoch_context_bound": "1",
            "ecg_isa_variant": "baseline",
            "sniper_workload": "sg_kernel",
            "sniper_roi_icount": "0",
            "timing_valid_for_speedup": "0",
            "sniper_workload_sha256": "a" * 64,
            "sniper_simulator_sha256": "b" * 64,
            "benchmark": path.parent.name,
            "options": "-f graph.sg",
            "prefetcher": "none",
            "l1d_size": "2kB",
            "l2_size": "4kB",
            "l3_size": "16kB",
            "l3_ways": "8",
            "threads": "1",
            "sniper_cores": "1",
            "sniper_cache_warming": "1",
            "sniper_transport_record_bytes": "8",
            "sniper_transport_bytes_per_edge": "8",
            "ecg_record_bytes": "8",
            "edge_stream_bytes_per_edge": "8",
            "ecg_record_replaces_edge": "1",
            "sniper_semantic_result": "same",
            "sniper_semantic_edge_limit": "0",
            "sniper_semantic_edge_visits": "",
            "sniper_semantic_truncated": "",
            "semantic_work_matched": "",
            "sniper_transport_receipts_validated": "0",
            "sniper_k2_exact_bind_validated": "0",
            "sniper_k2_epoch_context_validated": "0",
            "log_path": str(path.parent / "lru.log"),
        })
        writer.writerow({
            "policy_label": "ECG_K2",
            "status": "ok",
            "instructions": str(round(100000 * ratio)),
            "sniper_transport_matched": "1",
            "sniper_k2_exact_bind": "1",
            "sniper_k2_epoch_context_bound": "1",
            "ecg_isa_variant": "mask",
            "sniper_workload": "sg_kernel",
            "sniper_roi_icount": "0",
            "timing_valid_for_speedup": "0",
            "sniper_workload_sha256": "a" * 64,
            "sniper_simulator_sha256": "b" * 64,
            "benchmark": path.parent.name,
            "options": "-f graph.sg",
            "prefetcher": "none",
            "l1d_size": "2kB",
            "l2_size": "4kB",
            "l3_size": "16kB",
            "l3_ways": "8",
            "threads": "1",
            "sniper_cores": "1",
            "sniper_cache_warming": "1",
            "sniper_transport_record_bytes": "8",
            "sniper_transport_bytes_per_edge": "8",
            "ecg_record_bytes": "8",
            "edge_stream_bytes_per_edge": "8",
            "ecg_record_replaces_edge": "1",
            "sniper_semantic_result": "same",
            "sniper_semantic_edge_limit": "0",
            "sniper_semantic_edge_visits": "",
            "sniper_semantic_truncated": "",
            "semantic_work_matched": "",
            "sniper_transport_receipts_validated":
                "1" if binding_validated else "0",
            "sniper_k2_exact_bind_validated":
                "1" if binding_validated else "0",
            "sniper_k2_epoch_context_validated":
                "1" if binding_validated else "0",
            "log_path": str(path.parent / "k2.log"),
        })
    (path.parent / "lru.log").write_text(
        "[K2_TRANSPORT_MATCHED]\n[K2_EXACT_BIND]\n")
    (path.parent / "k2.log").write_text(
        "[K2_TRANSPORT_MATCHED]\n[K2_EXACT_BIND]\n")
    json_path = path.parent / "roi_matrix.json"
    json_rows = list(csv.DictReader(path.open()))
    json_path.write_text(json.dumps(json_rows))
    marker = {
        "complete": True,
        "all_rows_ok": True,
        "outputs": {
            "roi_matrix.csv": {
                "rows": 2,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            },
            "roi_matrix.json": {
                "rows": 2,
                "sha256": hashlib.sha256(json_path.read_bytes()).hexdigest(),
            },
        },
    }
    (path.parent / "roi_matrix.complete.json").write_text(json.dumps(marker))


def write_binding_proof(
        root: Path, workload_hash: str = "a" * 64,
        simulator_hash: str = "b" * 64, dirty: bool = False) -> Path:
    inputs_dir = root / "inputs"
    inputs_dir.mkdir(parents=True)
    simulator_path = inputs_dir / "sniper_binary"
    workload = inputs_dir / "sniper_workload"
    simulator_path.write_text(simulator_hash)
    workload.write_text(workload_hash)
    simulator_hash = hashlib.sha256(simulator_path.read_bytes()).hexdigest()
    workload_hash = hashlib.sha256(workload.read_bytes()).hexdigest()
    cells = []
    for simulator_name in ("cache_sim", "gem5", "sniper"):
        cell_dir = root / "cells" / "pr" / simulator_name
        cell_dir.mkdir(parents=True)
        raw = cell_dir / "raw.log"
        raw.write_text(f"{simulator_name} validated trace\n")
        raw_record = {
            "path": str(raw.relative_to(root)),
            "bytes": raw.stat().st_size,
            "sha256": hashlib.sha256(raw.read_bytes()).hexdigest(),
        }
        coverage = {}
        if simulator_name == "sniper":
            coverage = {
                "k2_delivery_match": True,
                "k2_bind_consume_valid": True,
                "k2_bind_receipt_match": True,
                "k2_exact_bind_required": True,
                "k2_bind_consumes": 32,
                "k2_fused_receipts": 32,
                "k2_distance_mismatches": 0,
            }
        cell = {
            "simulator": simulator_name,
            "kernel": "pr",
            "status": "ok",
            "expected_policy": "ECG:epoch_first",
            "banner": "[ECG-CONFIG]",
            "coverage": coverage,
            "runner": {},
            "outputs": {"raw_log": raw_record},
        }
        (cell_dir / "cell.json").write_text(
            json.dumps(cell, sort_keys=True))
        cells.append(cell)
    summary = {
        "schema_version": 1,
        "status": "passed",
        "preflight": {
            "exact_victim_unit": True,
            "field_layout_parity": True,
        },
        "cells": cells,
        "cell_count": 3,
        "passed_cells": 3,
        "matrix": {
            "pr": {
                "cache_sim": "ok",
                "gem5": "ok",
                "sniper": "ok",
            },
        },
        "git_head": "deadbeef",
        "configuration": {
            "kernels": ["pr"],
            "simulators": ["cache_sim", "gem5", "sniper"],
            "schedule_k": 2,
            "isa_variant": "mask",
        },
        "proof_inputs": {
            "sniper_binary": {
                "path": "inputs/sniper_binary",
                "bytes": simulator_path.stat().st_size,
                "sha256": simulator_hash,
            },
            "sniper_workload": {
                "path": "inputs/sniper_workload",
                "bytes": workload.stat().st_size,
                "sha256": workload_hash,
            },
        },
    }
    summary_path = root / "summary.json"
    summary_path.write_text(json.dumps(summary, sort_keys=True))
    manifest = {
        "schema_version": 1,
        "status": "passed",
        "git_head": "deadbeef",
        "git_status_porcelain": " M dirty" if dirty else "",
        "summary_sha256": hashlib.sha256(
            summary_path.read_bytes()).hexdigest(),
        "configuration": summary["configuration"],
        "inputs": summary["proof_inputs"],
    }
    (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True))
    return summary_path


def test_matched_rows_pass(tmp_path: Path):
    for kernel in ("pr", "bfs", "sssp", "bc", "cc"):
        write_rows(tmp_path / kernel / "roi_matrix.csv")
    assert validate(tmp_path) == []


def test_instruction_drift_fails(tmp_path: Path):
    write_rows(tmp_path / "pr" / "roi_matrix.csv", ratio=1.01)
    errors = validate(tmp_path, ("pr",))
    assert len(errors) == 1
    assert "instruction ratio" in errors[0]


def test_semantic_edge_mismatch_fails(tmp_path: Path):
    path = tmp_path / "pr" / "roi_matrix.csv"
    write_rows(path)
    rows = list(csv.DictReader(path.open()))
    for row in rows:
        row["sniper_semantic_edge_limit"] = "1000"
        row["sniper_semantic_edge_visits"] = "1000"
        row["sniper_semantic_truncated"] = "1"
        row["semantic_work_matched"] = "1"
    rows[1]["sniper_semantic_edge_visits"] = "999"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    errors = validate(tmp_path, ("pr",))
    assert any("sniper_semantic_edge_visits" in error for error in errors)


def test_transport_edge_width_mismatch_fails(tmp_path: Path):
    path = tmp_path / "pr" / "roi_matrix.csv"
    write_rows(path)
    rows = list(csv.DictReader(path.open()))
    rows[1]["edge_stream_bytes_per_edge"] = "4"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    errors = validate(tmp_path, ("pr",))
    assert any("edge_stream_bytes_per_edge" in error for error in errors)


def test_external_binding_proof_must_match_row_binaries(tmp_path: Path):
    rows_root = tmp_path / "rows"
    write_rows(
        rows_root / "pr" / "roi_matrix.csv",
        binding_validated=False)
    proof = write_binding_proof(tmp_path / "proof")
    manifest = json.loads((proof.parent / "manifest.json").read_text())
    rows_path = rows_root / "pr" / "roi_matrix.csv"
    rows = list(csv.DictReader(rows_path.open()))
    for row in rows:
        row["sniper_workload_sha256"] = (
            manifest["inputs"]["sniper_workload"]["sha256"])
        row["sniper_simulator_sha256"] = (
            manifest["inputs"]["sniper_binary"]["sha256"])
    with rows_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    json_path = rows_path.with_name("roi_matrix.json")
    json_path.write_text(json.dumps(rows))
    marker_path = rows_path.with_name("roi_matrix.complete.json")
    marker = json.loads(marker_path.read_text())
    marker["outputs"]["roi_matrix.csv"]["sha256"] = hashlib.sha256(
        rows_path.read_bytes()).hexdigest()
    marker["outputs"]["roi_matrix.json"]["sha256"] = hashlib.sha256(
        json_path.read_bytes()).hexdigest()
    marker_path.write_text(json.dumps(marker))
    assert validate(
        rows_root, ("pr",), binding_proof=proof) == []

    mismatched = write_binding_proof(
        tmp_path / "mismatched", simulator_hash="c" * 64)
    errors = validate(rows_root, ("pr",), binding_proof=mismatched)
    assert any("binary-matched external" in error for error in errors)


def test_external_binding_proof_rejects_dirty_or_tampered_evidence(
        tmp_path: Path):
    rows_root = tmp_path / "rows"
    write_rows(
        rows_root / "pr" / "roi_matrix.csv",
        binding_validated=False)
    dirty = write_binding_proof(tmp_path / "dirty", dirty=True)
    errors = validate(rows_root, ("pr",), binding_proof=dirty)
    assert any("dirty worktree" in error for error in errors)

    proof = write_binding_proof(tmp_path / "tampered")
    raw = proof.parent / "cells" / "pr" / "sniper" / "raw.log"
    raw.write_text("tampered\n")
    errors = validate(rows_root, ("pr",), binding_proof=proof)
    assert any("archived output hash failed" in error for error in errors)


def test_binding_proof_rejects_records_outside_the_archive(tmp_path: Path):
    """A proof must stand on evidence copied into its own directory; absolute
    or escaping paths would certify mutable files outside the frozen archive."""
    from scripts.experiments.ecg.verify.matched_k2m import (  # noqa: E402
        proof_record_path,
        valid_proof_record,
    )

    root = tmp_path / "evidence"
    (root / "inputs").mkdir(parents=True)
    inside = root / "inputs" / "sniper_binary"
    inside.write_text("archived")
    outside = tmp_path / "external_binary"
    outside.write_text("mutable")

    good = {
        "path": "inputs/sniper_binary",
        "bytes": inside.stat().st_size,
        "sha256": hashlib.sha256(inside.read_bytes()).hexdigest(),
    }
    assert proof_record_path(good, root) == inside.resolve()
    assert valid_proof_record(good, root)

    absolute = {
        "path": str(outside.resolve()),
        "bytes": outside.stat().st_size,
        "sha256": hashlib.sha256(outside.read_bytes()).hexdigest(),
    }
    assert proof_record_path(absolute, root) is None
    assert not valid_proof_record(absolute, root)

    escaping = {
        "path": "../external_binary",
        "bytes": outside.stat().st_size,
        "sha256": hashlib.sha256(outside.read_bytes()).hexdigest(),
    }
    assert proof_record_path(escaping, root) is None
    assert not valid_proof_record(escaping, root)
