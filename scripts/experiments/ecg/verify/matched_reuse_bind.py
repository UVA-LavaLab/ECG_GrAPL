#!/usr/bin/env python3
"""Validate transport-matched Sniper ReuseBind instruction parity."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path


DEFAULT_KERNELS = ("pr", "bfs", "sssp", "bc", "cc")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def proof_record_path(
        record: dict[str, object], evidence_root: Path) -> Path | None:
    """Resolve an archived record path, refusing anything outside the archive.

    A proof must stand on evidence copied into its own directory: absolute
    paths (or relative paths that escape via ../symlinks) would let a proof
    certify mutable files that live outside the frozen archive.
    """
    raw = str(record.get("path", ""))
    if not raw:
        return None
    path = Path(raw)
    if path.is_absolute():
        return None
    root = evidence_root.resolve()
    resolved = (root / path).resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        return None
    return resolved


def valid_proof_record(
        record: dict[str, object], evidence_root: Path) -> bool:
    path = proof_record_path(record, evidence_root)
    if path is None or not path.exists() or not path.is_file():
        return False
    return (
        record.get("bytes") == path.stat().st_size and
        record.get("sha256") == sha256_file(path)
    )


def validated_binding_proof(
        path: Path | None,
        kernels: tuple[str, ...]) -> tuple[dict[str, str] | None, list[str]]:
    if path is None:
        return None, []
    summary_path = path / "summary.json" if path.is_dir() else path
    evidence_root = summary_path.parent
    manifest_path = evidence_root / "manifest.json"
    errors: list[str] = []
    if not summary_path.exists():
        return None, [f"binding proof summary missing: {summary_path}"]
    if not manifest_path.exists():
        return None, [f"binding proof manifest missing: {manifest_path}"]
    try:
        payload = json.loads(summary_path.read_text())
        manifest = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as exc:
        return None, [f"binding proof JSON is invalid: {exc}"]
    if payload.get("schema_version") != 1:
        errors.append("binding proof summary schema is not 1")
    if manifest.get("schema_version") != 1:
        errors.append("binding proof manifest schema is not 1")
    if payload.get("status") != "passed":
        errors.append("binding proof summary is not passed")
    if manifest.get("status") != "passed":
        errors.append("binding proof manifest is not passed")
    if manifest.get("git_status_porcelain") not in ("", None):
        errors.append("binding proof was captured from a dirty worktree")
    if manifest.get("summary_sha256") != sha256_file(summary_path):
        errors.append("binding proof summary hash does not match manifest")
    if payload.get("git_head") != manifest.get("git_head"):
        errors.append("binding proof git head differs between summary and manifest")
    configuration = manifest.get("configuration", {})
    if payload.get("configuration") != configuration:
        errors.append(
            "binding proof configuration differs between summary and manifest")
    if configuration.get("reuse_plan_depth") != 2:
        errors.append("binding proof is not two-epoch ReusePlan")
    if configuration.get("isa_variant") != "computed":
        errors.append("binding proof is not computed-address ReuseBind")
    required_simulators = {"cache_sim", "gem5", "sniper"}
    if not required_simulators.issubset(
            set(configuration.get("simulators", []))):
        errors.append("binding proof does not include all three simulators")
    if not set(kernels).issubset(set(configuration.get("kernels", []))):
        errors.append("binding proof does not cover all requested kernels")
    preflight = payload.get("preflight", {})
    if not preflight or not all(value is True for value in preflight.values()):
        errors.append("binding proof independent preflight is incomplete")
    cells = {
        (cell.get("kernel"), cell.get("simulator")): cell
        for cell in payload.get("cells", [])
    }
    if payload.get("cell_count") != len(payload.get("cells", [])):
        errors.append("binding proof cell count is inconsistent")
    if payload.get("passed_cells") != sum(
            cell.get("status") == "ok" for cell in payload.get("cells", [])):
        errors.append("binding proof passed-cell count is inconsistent")
    matrix = payload.get("matrix", {})
    for kernel in kernels:
        for simulator in sorted(required_simulators):
            cell = cells.get((kernel, simulator), {})
            if (cell.get("status") != "ok" or
                    matrix.get(kernel, {}).get(simulator) != "ok"):
                errors.append(
                    f"binding proof {simulator}/{kernel} is not passed")
                continue
            cell_path = (
                evidence_root / "cells" / kernel / simulator / "cell.json")
            if not cell_path.exists():
                errors.append(
                    f"binding proof cell archive missing: {cell_path}")
            else:
                try:
                    archived = json.loads(cell_path.read_text())
                except json.JSONDecodeError:
                    errors.append(
                        f"binding proof cell archive invalid: {cell_path}")
                else:
                    if archived != cell:
                        errors.append(
                            "binding proof cell archive disagrees for "
                            f"{simulator}/{kernel}")
            outputs = cell.get("outputs", {})
            if not outputs or not all(
                    valid_proof_record(record, evidence_root)
                    for record in outputs.values()):
                errors.append(
                    "binding proof archived output hash failed for "
                    f"{simulator}/{kernel}")
        sniper_cell = cells.get((kernel, "sniper"), {})
        coverage = sniper_cell.get("coverage", {})
        if not all((
                coverage.get("reuse_plan_delivery_match") is True,
                coverage.get("reuse_plan_bind_consume_valid") is True,
                coverage.get("reuse_plan_bind_receipt_match") is True,
                coverage.get("reuse_plan_exact_bind_required") is True,
                int(coverage.get("reuse_plan_bind_consumes") or 0) > 0,
                int(coverage.get("reuse_plan_fused_receipts") or 0) > 0,
                int(coverage.get("reuse_plan_distance_mismatches") or 0) == 0,
        )):
            errors.append(
                f"binding proof Sniper/{kernel} lacks exact delivery coverage")
    inputs = manifest.get("inputs", {})
    proof_inputs = payload.get("proof_inputs", {})
    required_inputs = ("sniper_binary", "sniper_workload")
    for name in required_inputs:
        record = inputs.get(name)
        if proof_inputs.get(name) != record:
            errors.append(
                f"binding proof {name} differs between summary and manifest")
            continue
        if not isinstance(record, dict) or not valid_proof_record(
                record, evidence_root):
            errors.append(
                f"binding proof archived {name} hash failed")
    if errors:
        return None, errors
    return {
        "sniper_simulator_sha256": str(
            inputs["sniper_binary"]["sha256"]),
        "sniper_workload_sha256": str(
            inputs["sniper_workload"]["sha256"]),
        "git_head": str(manifest.get("git_head", "")),
    }, []


def validate(
        root: Path, kernels: tuple[str, ...] = DEFAULT_KERNELS,
        tolerance: float = 0.0025,
        binding_proof: Path | None = None) -> list[str]:
    errors: list[str] = []
    external_binding_proof, proof_errors = validated_binding_proof(
        binding_proof, kernels)
    errors.extend(proof_errors)
    for kernel in kernels:
        path = root / kernel / "roi_matrix.csv"
        if not path.exists():
            errors.append(f"{kernel}: missing {path}")
            continue
        rows = list(csv.DictReader(path.open()))
        if len(rows) != 2:
            errors.append(f"{kernel}: expected exactly two rows, got {len(rows)}")
            continue
        by_policy = {row.get("policy_label"): row for row in rows}
        if set(by_policy) != {"LRU", "ECG_REUSE_PLAN"}:
            errors.append(
                f"{kernel}: expected LRU/ECG_REUSE_PLAN, got {sorted(by_policy)}")
            continue
        lru = by_policy["LRU"]
        reuse_plan = by_policy["ECG_REUSE_PLAN"]
        if lru.get("status") != "ok" or reuse_plan.get("status") != "ok":
            errors.append(f"{kernel}: non-ok row")
            continue
        if lru.get("sniper_transport_matched") != "1":
            errors.append(f"{kernel}: LRU transport not matched")
        if reuse_plan.get("sniper_transport_matched") != "1":
            errors.append(f"{kernel}: ReusePlan transport not matched")
        if lru.get("sniper_reuse_bind_exact") != "1":
            errors.append(f"{kernel}: LRU exact binding is not enabled")
        if reuse_plan.get("sniper_reuse_bind_exact") != "1":
            errors.append(f"{kernel}: ReusePlan exact binding is not enabled")
        if lru.get("sniper_reuse_plan_epoch_context_bound") != "1":
            errors.append(f"{kernel}: LRU epoch/context binding is not enabled")
        if reuse_plan.get("sniper_reuse_plan_epoch_context_bound") != "1":
            errors.append(f"{kernel}: ReusePlan epoch/context binding is not enabled")
        row_binding_validated = all((
            reuse_plan.get("sniper_transport_receipts_validated") == "1",
            reuse_plan.get("sniper_reuse_bind_exact_validated") == "1",
            reuse_plan.get("sniper_reuse_plan_epoch_context_validated") == "1",
        ))
        row_workload_hash = reuse_plan.get("sniper_workload_sha256", "")
        row_simulator_hash = reuse_plan.get("sniper_simulator_sha256", "")
        proof_matches_row = bool(external_binding_proof) and all((
            len(row_workload_hash) == 64,
            len(row_simulator_hash) == 64,
            row_workload_hash ==
            external_binding_proof["sniper_workload_sha256"],
            row_simulator_hash ==
            external_binding_proof["sniper_simulator_sha256"],
        ))
        if not row_binding_validated and not proof_matches_row:
            errors.append(
                f"{kernel}: ReusePlan runtime binding is not validated and no "
                "passing binary-matched external conformance proof was supplied")
        if reuse_plan.get("ecg_isa_variant") != "computed":
            errors.append(f"{kernel}: ReusePlan ISA variant is not computed")
        if lru.get("sniper_workload") != "sg_kernel":
            errors.append(f"{kernel}: LRU workload is not sg_kernel")
        if reuse_plan.get("sniper_workload") != "sg_kernel":
            errors.append(f"{kernel}: ReusePlan workload is not sg_kernel")
        if lru.get("sniper_roi_icount") not in ("", "0", None):
            errors.append(f"{kernel}: LRU row is instruction-capped")
        if reuse_plan.get("sniper_roi_icount") not in ("", "0", None):
            errors.append(f"{kernel}: ReusePlan row is instruction-capped")
        if lru.get("timing_valid_for_speedup") != "0":
            errors.append(f"{kernel}: LRU matched row is not diagnostic-only")
        if reuse_plan.get("timing_valid_for_speedup") != "0":
            errors.append(f"{kernel}: ReusePlan matched row is not diagnostic-only")
        if not lru.get("sniper_workload_sha256"):
            errors.append(f"{kernel}: missing workload hash")
        elif (lru.get("sniper_workload_sha256") !=
              reuse_plan.get("sniper_workload_sha256")):
            errors.append(f"{kernel}: workload hashes differ")
        matched_fields = (
            "benchmark", "options", "prefetcher", "l1d_size", "l2_size",
            "l3_size", "l3_ways", "threads", "sniper_cores",
            "sniper_cache_warming",
            "sniper_transport_record_bytes",
            "sniper_transport_bytes_per_edge",
            "ecg_record_bytes",
            "edge_stream_bytes_per_edge",
            "ecg_record_replaces_edge",
            "sniper_simulator_sha256",
            "sniper_semantic_edge_limit", "sniper_semantic_edge_visits",
            "sniper_semantic_truncated",
        )
        for field in matched_fields:
            if lru.get(field) != reuse_plan.get(field):
                errors.append(f"{kernel}: configuration mismatch in {field}")
        semantic_limit = int(lru.get("sniper_semantic_edge_limit") or 0)
        if semantic_limit > 0:
            if lru.get("semantic_work_matched") != "1":
                errors.append(f"{kernel}: LRU semantic work is not certified")
            if reuse_plan.get("semantic_work_matched") != "1":
                errors.append(f"{kernel}: ReusePlan semantic work is not certified")
        if (not lru.get("sniper_semantic_result") or
                lru.get("sniper_semantic_result") !=
                reuse_plan.get("sniper_semantic_result")):
            errors.append(f"{kernel}: semantic results differ or are missing")
        for row in (lru, reuse_plan):
            log_path = Path(row.get("log_path", ""))
            text = log_path.read_text(errors="ignore") if log_path.exists() else ""
            if "[REUSE_PLAN_TRANSPORT_MATCHED]" not in text:
                errors.append(
                    f"{kernel}/{row.get('policy_label')}: transport marker missing")
            if "[REUSE_PLAN_EXACT_BIND]" not in text:
                errors.append(
                    f"{kernel}/{row.get('policy_label')}: exact-bind marker missing")

        marker_path = root / kernel / "roi_matrix.complete.json"
        json_path = root / kernel / "roi_matrix.json"
        if not marker_path.exists():
            errors.append(f"{kernel}: completion marker missing")
        elif not json_path.exists():
            errors.append(f"{kernel}: roi_matrix.json missing")
        else:
            marker = json.loads(marker_path.read_text())
            if not marker.get("complete") or not marker.get("all_rows_ok"):
                errors.append(f"{kernel}: completion marker is not all-ok")
            json_rows = json.loads(json_path.read_text())
            outputs = marker.get("outputs", {})
            descriptors = (
                ("roi_matrix.csv", path, len(rows)),
                ("roi_matrix.json", json_path, len(json_rows)),
            )
            for name, output_path, row_count in descriptors:
                descriptor = outputs.get(name, {})
                digest = hashlib.sha256(output_path.read_bytes()).hexdigest()
                if (descriptor.get("rows") != row_count or
                        descriptor.get("sha256") != digest):
                    errors.append(
                        f"{kernel}: {name} marker hash/rows mismatch")
        try:
            lru_instructions = int(lru["instructions"])
            reuse_plan_instructions = int(reuse_plan["instructions"])
        except (KeyError, TypeError, ValueError):
            errors.append(f"{kernel}: missing instruction count")
            continue
        ratio = reuse_plan_instructions / lru_instructions
        if abs(ratio - 1.0) > tolerance:
            errors.append(
                f"{kernel}: instruction ratio {ratio:.6f} exceeds "
                f"{tolerance:.4%} tolerance")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--kernels", nargs="+", default=list(DEFAULT_KERNELS))
    parser.add_argument("--tolerance", type=float, default=0.0025)
    parser.add_argument(
        "--binding-proof", type=Path,
        help="Structured three-simulator conformance summary that validates "
             "Sniper runtime bind consumption for every requested kernel.")
    args = parser.parse_args()
    errors = validate(
        args.root, tuple(args.kernels), float(args.tolerance),
        args.binding_proof)
    if errors:
        for error in errors:
            print(f"[FAIL] {error}")
        return 1
    print(
        f"[PASS] matched ReuseBind instruction parity: "
        f"{len(args.kernels)} kernels within {args.tolerance:.4%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
