"""Build and verify authorization for the current ECG equivalence profile."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
from typing import Any

from path_fingerprints import hash_path
from record_receipts import (
    EQUIVALENCE_DIGESTS, LAYOUT_FIELDS as RECORD_LAYOUT_FIELDS,
    RecordReceiptError, resolve_layout, validate_equivalence,
    validate_functional_record, validate_gem5_record, validate_sniper_record,
    validate_pr_workload,
)
from record_resources import graph_info


SCHEMA = "ecg-current-equivalence"
RECEIPT_NAME = "current_ecg_equivalence.complete.json"
DIGEST_FIELDS = tuple("ecg_" + field for field in EQUIVALENCE_DIGESTS)
LAYOUT_FIELDS = tuple("ecg_" + field for field in RECORD_LAYOUT_FIELDS)
POLICY_MECHANISMS = {
    "ECG_TRANSPORT": "transport",
    "ECG_REPLACEMENT": "replacement",
    "ECG_PREFETCH": "prefetch",
    "ECG": "replacement-prefetch",
}


class EquivalenceGateError(ValueError):
    pass


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def output_descriptor(path: Path) -> dict[str, Any]:
    rows = None
    if path.suffix == ".csv":
        with path.open(newline="") as handle:
            rows = max(sum(1 for _ in csv.reader(handle)) - 1, 0)
    elif path.suffix == ".json":
        payload = json.loads(path.read_text())
        if isinstance(payload, list):
            rows = len(payload)
    return {
        "sha256": file_sha256(path),
        "size": path.stat().st_size,
        "rows": rows,
    }


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise EquivalenceGateError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise EquivalenceGateError(f"{path} must contain a JSON object")
    return value


def canonical_digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EquivalenceGateError(message)


def contained(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def integer(row: dict[str, str], key: str) -> int:
    value = str(row.get(key, ""))
    if not re.fullmatch(r"[0-9]+", value):
        raise EquivalenceGateError(f"invalid integer {key}={value!r}")
    return int(value)


def digest_value(row: dict[str, str], key: str) -> str:
    value = str(row.get(key, "")).lower()
    if not re.fullmatch(r"[0-9a-f]{16}", value):
        raise EquivalenceGateError(f"invalid semantic digest {key}={value!r}")
    return value


def unfiltered(resolved: dict[str, Any]) -> bool:
    filters = resolved.get("filters", {})
    return all(not filters.get(name) for name in (
        "graph", "benchmark", "policy", "job", "only", "skip",
        "from_job", "limit"))


def expected_roster(
        manifest: dict[str, Any], contract: dict[str, Any]
) -> dict[tuple[str, str, str, int, str, str], dict[str, Any]]:
    stages = {
        str(stage["name"]): stage
        for stage in manifest.get("stages", [])
    }
    graph_sets = manifest.get("graph_sets", {})
    expected: dict[tuple[str, str, str, int, str, str], dict[str, Any]] = {}
    cells: set[tuple[str, str, int]] = set()
    for stage_name in contract["required_stages"]:
        require(stage_name in stages, f"required stage is missing: {stage_name}")
        stage = dict(manifest.get("defaults", {}))
        stage.update(stages[stage_name])
        require(
            contract["profile"] in stage.get("profiles", []),
            f"required stage is not in {contract['profile']}: {stage_name}")
        require(stage.get("kind") == "roi_matrix", f"wrong stage kind: {stage_name}")
        suite = str(stage.get("suite"))
        backend = "cache_sim" if suite == "cache-sim" else suite
        require(backend in contract["backends"], f"wrong backend in {stage_name}")
        require(stage.get("benchmarks") == [contract["benchmark"]],
                f"wrong benchmark in {stage_name}")
        require(stage.get("ecg_equivalence") is True and
                stage.get("prefetcher") == "none" and
                stage.get("flowthrough") == "off",
                f"wrong equivalence controls in {stage_name}")
        width = int(stage.get("ecg_record_bytes", -1))
        require(width in contract["widths"], f"wrong record width in {stage_name}")
        graph_set = graph_sets.get(str(stage.get("graph_set")), [])
        require(len(graph_set) == 1, f"{stage_name} must name exactly one graph")
        graph = graph_set[0]
        graph_name = str(graph["name"])
        tier_name = next((
            name for name, tier in contract["tiers"].items()
            if tier["graph"] == graph_name), "")
        require(bool(tier_name), f"{stage_name} graph is outside required tiers")
        tier = contract["tiers"][tier_name]
        require(graph.get("path") == tier["path"] and
                graph.get("expected_sha256") == tier["sha256"] and
                int(graph.get("expected_vertices", -1)) ==
                    int(tier["vertices"]) and
                int(graph.get("expected_records", -1)) ==
                    int(tier["records"]) and
                int(graph.get("expected_max_vertex_id", -1)) ==
                    int(tier["max_vertex_id"]),
                f"graph contract mismatch in {stage_name}")
        effective = dict(stage)
        for field in (
                "l1d_size", "l2_size", "l3_sizes", "l3_ways", "line_size"):
            if field in graph:
                effective[field] = graph[field]
        geometry = tier["geometry"]
        require(
            effective.get("l1d_size") == geometry["l1d_size"] and
            effective.get("l2_size") == geometry["l2_size"] and
            effective.get("l3_sizes") == [geometry["l3_size"]] and
            str(effective.get("l3_ways")) == geometry["l3_ways"] and
            str(effective.get("line_size")) == geometry["line_size"],
            f"wrong cache geometry in {stage_name}")
        policies = [str(value) for value in stage.get("policies", [])]
        require(stage.get("policy_sharding_allowed") is False,
                f"{stage_name} permits policy sharding")
        labels = [{
            "ECG:transport": "ECG_TRANSPORT",
            "ECG:replacement": "ECG_REPLACEMENT",
            "ECG:prefetch": "ECG_PREFETCH",
            "ECG": "ECG",
        }.get(policy, "") for policy in policies]
        require(labels == tier["policies"],
                f"wrong policy roster in {stage_name}")
        option_key = str(graph.get("options_key", ""))
        require(
            manifest.get("benchmark_options", {}).get(
                option_key, {}).get(contract["benchmark"]) ==
            f"-f {{graph_path}} {contract['options_suffix']}",
            f"wrong graph execution contract in {stage_name}")
        timeout_key = {
            "cache_sim": "timeout_cache",
            "gem5": "timeout_gem5",
            "sniper": "timeout_sniper",
        }[backend]
        limits = contract["resource_limits"]
        require(
            int(stage.get("ecg_record_max_carrier_bytes", 0)) ==
                int(limits["record_max_carrier_bytes"]) and
            int(stage.get("ecg_record_max_auxiliary_bytes", 0)) ==
                int(limits["record_max_auxiliary_bytes"]),
            f"wrong construction limits in {stage_name}")
        require(
            0 < int(stage.get(timeout_key, 0)) <=
                int(limits["maximum_cell_seconds"]),
                f"unbounded equivalence timeout in {stage_name}")
        rss_key = {
            "cache_sim": "cache_record_rss_mib",
            "gem5": "gem5_record_rss_mib",
            "sniper": "sniper_record_rss_mib",
        }[backend]
        require(int(stage.get(rss_key, 0)) == int(limits[rss_key]),
                f"wrong RSS bound in {stage_name}")
        if backend == "gem5":
            require(stage.get("gem5_cpu_type") == "O3" and
                    stage.get("gem5_mem_size") == limits["gem5_mem_size"] and
                    int(stage.get("gem5_max_insts", 0)) == 0,
                    f"wrong native execution contract in {stage_name}")
        if backend == "sniper":
            require(stage.get("sniper_workload") == "sg_kernel" and
                    int(stage.get("sniper_cores", 0)) == 1 and
                    stage.get("sniper_frontend") == "sift" and
                    stage.get("sniper_address_domain") == "translated" and
                    int(stage.get("sniper_roi_icount", 0)) == 0 and
                    int(stage.get("sniper_semantic_edge_limit", 0)) == 0 and
                    not stage.get("allow_sniper_sg_kernel_workload") and
                    not stage.get("allow_sniper_benchmark_workload"),
                    f"wrong Sniper execution contract in {stage_name}")
        if backend == "cache_sim":
            require(int(stage.get("cache_sim_omp_threads", 0)) == 1,
                    f"wrong functional execution contract in {stage_name}")
        cell = (tier_name, backend, width)
        require(cell not in cells, f"duplicate equivalence cell: {cell}")
        cells.add(cell)
        for policy in policies:
            label = {
                "ECG:transport": "ECG_TRANSPORT",
                "ECG:replacement": "ECG_REPLACEMENT",
                "ECG:prefetch": "ECG_PREFETCH",
                "ECG": "ECG",
            }.get(policy)
            require(label is not None, f"non-current policy in {stage_name}: {policy}")
            key = (
                stage_name, backend, graph_name, width, label,
                geometry["l3_size"])
            require(key not in expected, f"duplicate expected row: {key}")
            expected[key] = {
                "tier": tier_name,
                "graph": graph,
                "mechanism": POLICY_MECHANISMS[label],
            }
    required_cells = {
        (tier, backend, width)
        for tier in contract["tiers"]
        for backend in contract["backends"]
        for width in contract["widths"]
    }
    require(cells == required_cells,
            "equivalence stage matrix is incomplete")
    require(len(expected) == 36, f"expected roster has {len(expected)} rows, not 36")
    return expected


def validate_resolved_job_bounds(
        resolved: dict[str, Any], manifest: dict[str, Any],
        required_stages: list[str]) -> None:
    stages = {
        str(stage["name"]): {
            **manifest.get("defaults", {}), **stage}
        for stage in manifest.get("stages", [])
    }
    for job in resolved.get("jobs", []):
        stage_name = str(job.get("stage", ""))
        require(stage_name in required_stages,
                f"unexpected resolved stage: {stage_name}")
        stage = stages[stage_name]
        suite = str(stage["suite"])
        timeout_key = {
            "cache-sim": "timeout_cache",
            "gem5": "timeout_gem5",
            "sniper": "timeout_sniper",
        }[suite]
        rss_key = {
            "cache-sim": "cache_record_rss_mib",
            "gem5": "gem5_record_rss_mib",
            "sniper": "sniper_record_rss_mib",
        }[suite]
        cells = len(stage["policies"]) * len(
            stage.get("l3_sizes", ["4kB"]))
        expected_seconds = int(stage.get(
            "process_tree_timeout_seconds",
            int(stage[timeout_key]) * cells + 30))
        expected_rss = int(stage.get(
            "process_tree_rss_mib", stage[rss_key]))
        metadata = job.get("metadata", {})
        require(
            int(metadata.get("process_tree_timeout_s", 0)) ==
                expected_seconds and
            int(metadata.get("process_tree_rss_mib", 0)) == expected_rss,
            f"whole-job process-tree bound mismatch: {stage_name}")


def validate_corpus(
        root: Path, contract_path: Path, contract: dict[str, Any]
) -> dict[str, Any]:
    receipt_path = (
        root / "results/graphs/ecg-current-equivalence/corpus.receipt.json")
    receipt = load_json(receipt_path)
    from flows import prepare_record_equivalence_graphs as preparer_module
    try:
        preparer_module.prepare(receipt_path.parent, force=False, check=True)
    except (OSError, ValueError, RuntimeError) as error:
        raise EquivalenceGateError(f"corpus recipe verification failed: {error}") from error
    require(receipt.get("schema") == "ecg-current-equivalence-corpus",
            "wrong corpus receipt schema")
    require(receipt.get("config_sha256") == file_sha256(contract_path),
            "corpus receipt does not match the equivalence config")
    preparer = (
        root / "scripts/experiments/ecg/flows/"
        "prepare_record_equivalence_graphs.py")
    require(receipt.get("preparer_sha256") == file_sha256(preparer),
            "corpus receipt does not match the graph preparer")
    for tier in contract["tiers"].values():
        path = root / str(tier["path"])
        require(path.is_file(), f"required graph is missing: {path}")
        actual = file_sha256(path)
        require(actual == tier["sha256"], f"graph hash mismatch: {path}")
        inspected = graph_info(path)
        require(
            inspected.sha256 == actual and
            inspected.vertices == int(tier["vertices"]) and
            inspected.records == int(tier["records"]) and
            inspected.maximum_id == int(tier["max_vertex_id"]),
            f"strict graph inspection mismatch: {path}")
        recorded = receipt.get("graphs", {}).get(path.name, {})
        require(recorded.get("sha256") == actual,
                f"corpus receipt graph mismatch: {path}")
        require(recorded.get("semantic_sha256") == tier["semantic_sha256"],
                f"corpus semantic digest mismatch: {path}")
        for key in ("vertices", "records", "max_vertex_id"):
            require(int(recorded.get(key, -1)) == int(tier[key]),
                    f"corpus receipt {key} mismatch: {path}")
        for relative, expected in recorded.get("recipe_inputs", {}).items():
            source = root / relative
            require(source.is_file() and file_sha256(source) == expected,
                    f"corpus recipe input changed: {source}")
    return {
        "path": str(receipt_path.resolve()),
        "sha256": file_sha256(receipt_path),
    }


def verify_resolved_jobs(
        root: Path, run_dir: Path, manifest_path: Path,
        manifest: dict[str, Any], contract: dict[str, Any],
        resolved: dict[str, Any]) -> None:
    # Reuse job expansion rather than trusting the archived command or input list.
    from flows import experiment_run as workflow

    require(root.resolve() == workflow.PROJECT_ROOT.resolve(),
            "canonical job verification requires the current repository")
    execution = resolved.get("execution", {})
    require(set(execution) == {"python", "shard_group"} and
            isinstance(execution["python"], str) and
            Path(execution["python"]).is_absolute() and
            isinstance(execution["shard_group"], str),
            "canonical execution settings are missing or invalid")
    args = workflow.parse_args([
        "--manifest", str(manifest_path), "--profile", contract["profile"],
        "--run-dir", str(run_dir), "--no-build",
    ])
    args._execution_python = execution["python"]
    args._shard_group = execution["shard_group"]
    workflow.path_fingerprint.cache_clear()
    try:
        jobs = workflow.expand_jobs(args, manifest, run_dir)
    except (OSError, ValueError, SystemExit) as error:
        raise EquivalenceGateError(f"cannot reconstruct canonical jobs: {error}") from error
    finally:
        workflow.path_fingerprint.cache_clear()
    require(all(value not in ("", "missing") for job in jobs
                for value in job.metadata["input_fingerprints"].values()),
            "canonical runtime inputs are missing")
    require(resolved.get("jobs") == [workflow.job_snapshot(job) for job in jobs],
            "resolved jobs differ from canonical commands, environments, or inputs")
    require(resolved.get("run_config_hash") == workflow.run_config_hash(args, jobs),
            "resolved run configuration differs from canonical job expansion")


def verify_job_inputs(root: Path, resolved: dict[str, Any]) -> dict[str, str]:
    verified: dict[str, str] = {}
    for job in resolved.get("jobs", []):
        metadata = job.get("metadata", {})
        paths = metadata.get("input_paths", {})
        expected = metadata.get("input_fingerprints", {})
        require(bool(paths) and bool(expected),
                f"job omits runtime input fingerprints: {job.get('job_id')}")
        for name, text in paths.items():
            path = Path(str(text))
            if not path.is_absolute():
                path = root / path
            key = str(path.resolve())
            if key not in verified:
                verified[key] = hash_path(path)
            actual = verified[key]
            wanted = str(expected.get(name, ""))
            require(wanted not in ("", "missing"),
                    f"missing input fingerprint for {job.get('job_id')}:{name}")
            require(actual == wanted,
                    f"input changed for {job.get('job_id')}:{name}")
    return dict(sorted(verified.items()))


def validate_raw_row(row: dict[str, str], out_dir: Path) -> dict[str, Any]:
    validators = {
        "cache_sim": validate_functional_record,
        "gem5": validate_gem5_record,
        "sniper": validate_sniper_record,
    }
    backend = row.get("simulator", "")
    require(backend in validators, f"unknown raw-evidence backend: {backend}")
    paths = [Path(row.get("log_path", ""))]
    if backend == "gem5":
        native_output = Path(row.get("gem5_out", ""))
        require(native_output.is_dir() and contained(native_output, out_dir),
                "native evidence directory is missing or external")
        paths.extend(sorted(native_output.rglob("benchmark_stderr.txt")))
    texts = []
    outputs = {}
    for path in paths:
        require(path.is_file() and contained(path, out_dir),
                f"raw record evidence is missing or external: {path}")
        require(path.stat().st_size <= 64 << 20,
                f"bounded equivalence log exceeds 64 MiB: {path}")
        texts.append(path.read_text(errors="replace"))
        outputs[str(path.resolve())] = output_descriptor(path)
    text = "\n".join(texts)
    try:
        fields = validators[backend](
            text, mechanism=row.get("ecg_record_mechanism", ""),
            requested_bytes=integer(row, "ecg_record_requested_bytes"),
            minimum_mantissa_bits=integer(row, "ecg_record_minimum_mantissa_bits"))
        if backend == "cache_sim":
            fields.update(validate_pr_workload(
                text, carrier="record", iterations=int(fields["pr_iterations"]),
                semantic_edges=int(fields["pr_semantic_edges"])))
        fields.update(validate_equivalence(text, int(fields["pr_semantic_edges"])))
    except RecordReceiptError as error:
        raise EquivalenceGateError(f"raw record evidence failed: {error}") from error
    require(
        int(fields["carrier_allocation_bytes"]) <= integer(row, "ecg_record_max_carrier_bytes") and
        int(fields["construction_auxiliary_peak_bytes"]) <= integer(row, "ecg_record_max_auxiliary_bytes"),
        "raw construction evidence exceeds the requested resource budget")
    for key, value in fields.items():
        column = key if key.startswith("pr_") or key == "method" else "ecg_" + key
        require(str(row.get(column, "")) == str(value),
                f"raw evidence disagrees with archived row: {column}")
    return outputs


def verify_job_outputs(
        resolved: dict[str, Any], run_dir: Path | None = None
) -> tuple[list[dict[str, str]], dict[str, Any]]:
    rows: list[dict[str, str]] = []
    outputs: dict[str, Any] = {}
    for job in resolved.get("jobs", []):
        out_dir = Path(str(job["out_dir"]))
        if run_dir is not None:
            require(contained(out_dir, run_dir),
                    f"job output escapes equivalence run: {out_dir}")
        csv_path = out_dir / "roi_matrix.csv"
        json_path = out_dir / "roi_matrix.json"
        marker_path = out_dir / "roi_matrix.complete.json"
        for path in (csv_path, json_path, marker_path):
            require(path.is_file(), f"missing job output: {path}")
        log_path = Path(str(job.get("log_path", "")))
        require(log_path.is_file() and
                (run_dir is None or contained(log_path, run_dir)),
                f"missing or external job log: {log_path}")
        log_text = log_path.read_text(errors="replace")
        reasons = re.findall(r"^\[watchdog_reason\] (\S+)$", log_text, re.MULTILINE)
        codes = re.findall(r"^\[watchdog_returncode\] (-?\d+)$", log_text, re.MULTILINE)
        require(bool(reasons) and reasons[-1] == "exit" and bool(codes) and codes[-1] == "0",
                f"whole-job process-tree watchdog did not exit cleanly: {log_path}")
        marker = load_json(marker_path)
        require(marker.get("complete") is True and
                marker.get("all_rows_ok") is True,
                f"job marker is incomplete: {marker_path}")
        require(marker.get("config_hash") ==
                job.get("metadata", {}).get("config_hash"),
                f"job marker config mismatch: {marker_path}")
        expected_outputs = marker.get("outputs", {})
        for path in (csv_path, json_path):
            descriptor = expected_outputs.get(path.name)
            require(isinstance(descriptor, dict),
                    f"job marker omits {path.name}: {marker_path}")
            require(descriptor == output_descriptor(path),
                    f"job output changed after completion: {path}")
        with csv_path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            job_rows = list(reader)
            columns = reader.fieldnames or []
        json_rows = json.loads(json_path.read_text())
        require(isinstance(json_rows, list) and all(isinstance(row, dict) for row in json_rows),
                f"invalid row JSON: {json_path}")
        require(len(columns) == len(set(columns)) and
                set(columns) == {key for row in json_rows for key in row},
                f"CSV/JSON column mismatch: {out_dir}")
        normalized_json = [
            {key: "" if row.get(key) is None else str(row[key]) for key in columns}
            for row in json_rows
        ]
        require(job_rows == normalized_json,
                f"CSV/JSON row mismatch: {out_dir}")
        for row in job_rows:
            outputs.update(validate_raw_row(row, out_dir))
            row["_equivalence_stage"] = str(job["stage"])
            row["_equivalence_graph"] = str(
                job.get("metadata", {}).get("graph", ""))
            rows.append(row)
        outputs[str(marker_path.resolve())] = output_descriptor(marker_path)
        outputs[str(csv_path.resolve())] = output_descriptor(csv_path)
        outputs[str(json_path.resolve())] = output_descriptor(json_path)
        outputs[str(log_path.resolve())] = output_descriptor(log_path)
    return rows, dict(sorted(outputs.items()))


def row_key(row: dict[str, str]) -> tuple[str, str, str, int, str, str]:
    return (
        row["_equivalence_stage"],
        str(row.get("simulator", "")),
        row["_equivalence_graph"],
        integer(row, "ecg_record_requested_bytes"),
        str(row.get("policy_label", "")),
        str(row.get("l3_size", "")),
    )


def prefetch_count(row: dict[str, str], kind: str) -> int:
    backend = row.get("simulator")
    keys = {
        ("cache_sim", "candidate"): "ecg_functional_prefetch_candidates",
        ("cache_sim", "issue"): "ecg_functional_prefetch_issued",
        ("cache_sim", "fill"): "ecg_functional_prefetch_fills",
        ("gem5", "candidate"): "ecg_prefetch_candidates",
        ("gem5", "issue"): "ecg_prefetch_property_reads",
        ("gem5", "fill"): "ecg_prefetch_property_responses",
        ("sniper", "candidate"): "ecg_sniper_prefetch_candidates",
        ("sniper", "issue"): "ecg_sniper_prefetch_issued",
        ("sniper", "fill"): "ecg_sniper_prefetch_fills",
    }
    return integer(row, keys[(str(backend), kind)])


def validate_rows(
        rows: list[dict[str, str]],
        expected: dict[tuple[str, str, str, int, str, str], dict[str, Any]],
        contract: dict[str, Any],
) -> dict[str, Any]:
    actual: dict[tuple[str, str, str, int, str, str], dict[str, str]] = {}
    for row in rows:
        key = row_key(row)
        require(key not in actual, f"duplicate equivalence row: {key}")
        actual[key] = row
    require(set(actual) == set(expected),
            "equivalence row roster is partial or contains unexpected rows")

    canonical_rows = []
    within_width: dict[tuple[str, int], list[dict[str, str]]] = {}
    across_width: dict[str, list[dict[str, str]]] = {}
    positive_prefetch: dict[tuple[str, int], list[int]] = {}
    for key in sorted(expected):
        row = actual[key]
        specification = expected[key]
        tier = contract["tiers"][specification["tier"]]
        require(row.get("status") == "ok", f"failed equivalence row: {key}")
        require(integer(row, "ecg_equivalence_only") == 1 and
                row.get("ecg_equivalence_schema") == "ecg.record-stream",
                f"equivalence instrumentation missing: {key}")
        require(integer(row, "ecg_record_contract_valid") == 1,
                f"record contract invalid: {key}")
        require(row.get("benchmark") == contract["benchmark"],
                f"wrong benchmark: {key}")
        require(row.get("ecg_record_mechanism") == specification["mechanism"],
                f"wrong mechanism: {key}")
        require(integer(row, "ecg_record_bytes") == key[3],
                f"resolved width mismatch: {key}")
        layout = resolve_layout(
            records=int(tier["records"]), vertices=int(tier["vertices"]),
            maximum_id=int(tier["max_vertex_id"]), traversals=2, requested_bytes=key[3])
        require(all(str(row.get("ecg_" + field, "")) == str(value)
                    for field, value in layout.items()),
                f"invalid graph-derived layout: {key}")
        require(integer(row, "ecg_records") == int(tier["records"]) and
                integer(row, "ecg_vertex_count") == int(tier["vertices"]) and
                integer(row, "ecg_max_vertex_id") == int(tier["max_vertex_id"]),
                f"graph counts mismatch: {key}")
        expected_edges = int(tier["records"]) * 2
        require(integer(row, "pr_iterations") == 2 and
                integer(row, "pr_semantic_edges") == expected_edges and
                integer(row, "ecg_property_read_count") == expected_edges,
                f"semantic work mismatch: {key}")
        require(row.get("ecg_graph_sha256") == tier["sha256"],
                f"graph digest mismatch: {key}")
        require(str(row.get("timing_valid_for_speedup", "")) == "0",
                f"equivalence-only row exposed timing: {key}")
        for field in DIGEST_FIELDS:
            digest_value(row, field)
        digest_value(row, "pr_score_checksum")
        for field in LAYOUT_FIELDS:
            require(str(row.get(field, "")) != "",
                    f"missing layout field {field}: {key}")
        within_width.setdefault((key[2], key[3]), []).append(row)
        across_width.setdefault(key[2], []).append(row)
        if specification["tier"] == "micro" and specification["mechanism"] in (
                "prefetch", "replacement-prefetch"):
            require(
                prefetch_count(row, "candidate") == 0 and
                prefetch_count(row, "issue") == 0 and
                prefetch_count(row, "fill") == 0,
                f"fixture unexpectedly selected or issued a prefetch: {key}")
        if specification["tier"] == "small" and specification["mechanism"] in (
                "prefetch", "replacement-prefetch"):
            positive_prefetch.setdefault((key[1], key[3]), []).extend(
                (prefetch_count(row, "candidate"),
                 prefetch_count(row, "issue"), prefetch_count(row, "fill")))
        canonical_rows.append({
            "key": key,
            "layout": [row[field] for field in LAYOUT_FIELDS],
            "digests": [row[field] for field in DIGEST_FIELDS],
            "property_reads": row["ecg_property_read_count"],
            "iterations": row["pr_iterations"],
            "semantic_edges": row["pr_semantic_edges"],
            "score_checksum": row["pr_score_checksum"],
        })

    for group, values in within_width.items():
        for fields in (
                LAYOUT_FIELDS,
                ("ecg_source_order_digest", "ecg_carrier_digest",
                 "ecg_consumed_semantic_digest",
                 "ecg_destination_stream_digest",
                 "ecg_window_reference_digest"),
                ("ecg_property_read_count", "pr_iterations",
                 "pr_semantic_edges", "pr_score_checksum")):
            signatures = {tuple(row[field] for field in fields) for row in values}
            require(len(signatures) == 1,
                    f"within-width equivalence mismatch for {group}: {fields}")

    for graph, values in across_width.items():
        signatures = {
            (row["ecg_source_order_digest"],
             row["ecg_destination_stream_digest"],
             row["ecg_property_read_count"],
             row["pr_iterations"], row["pr_semantic_edges"],
             row["pr_score_checksum"])
            for row in values
        }
        require(len(signatures) == 1,
                f"cross-width semantic mismatch for {graph}")

    for group, counts in positive_prefetch.items():
        require(
            sum(counts[0::3]) > 0 and sum(counts[1::3]) > 0 and
            sum(counts[2::3]) > 0,
                f"stress prefetch traffic is not positive for {group}")

    return {
        "row_count": len(canonical_rows),
        "roster_sha256": canonical_digest(sorted(expected)),
        "rows_sha256": canonical_digest(canonical_rows),
    }


def build_receipt(
        root: Path, run_dir: Path, manifest_path: Path, contract_path: Path
) -> dict[str, Any]:
    contract = load_json(contract_path)
    manifest = load_json(manifest_path)
    resolved_path = run_dir / "resolved_manifest.json"
    completion_path = run_dir / "run.complete.json"
    resolved = load_json(resolved_path)
    completion = load_json(completion_path)
    require(contract.get("schema") == SCHEMA, "wrong equivalence contract schema")
    require(resolved.get("profiles") == [contract["profile"]],
            "equivalence run must select exactly the current profile")
    require(resolved.get("manifest") == manifest,
            "resolved run manifest differs from the current manifest")
    require(unfiltered(resolved), "filtered run cannot authorize equivalence")
    require(completion.get("run_config_hash") == resolved.get("run_config_hash"),
            "generic completion does not match the resolved manifest")
    require(completion.get("complete") is True,
            "generic run completion is not successful")
    expected = expected_roster(manifest, contract)
    jobs = resolved.get("jobs", [])
    require(
        sorted(str(job.get("stage")) for job in jobs) ==
        sorted(contract["required_stages"]),
        "resolved job roster does not contain every required stage")
    validate_resolved_job_bounds(
        resolved, manifest, contract["required_stages"])
    verify_resolved_jobs(root, run_dir, manifest_path, manifest, contract, resolved)
    inputs = verify_job_inputs(root, resolved)
    rows, outputs = verify_job_outputs(resolved, run_dir)
    outputs[str(resolved_path.resolve())] = output_descriptor(resolved_path)
    outputs[str(completion_path.resolve())] = output_descriptor(completion_path)
    row_summary = validate_rows(rows, expected, contract)
    corpus = validate_corpus(root, contract_path, contract)
    sources = {}
    for relative in contract.get("source_paths", []):
        path = root / str(relative)
        digest = hash_path(path)
        require(digest != "missing", f"equivalence source missing: {path}")
        sources[str(relative)] = digest
    stage_index = {
        str(stage["name"]): stage for stage in manifest.get("stages", [])}
    profile_config = {
        "contract": contract,
        "stages": [
            stage_index[name] for name in contract["required_stages"]],
        "graph_sets": {
            name: manifest["graph_sets"][name]
            for name in sorted({
                stage_index[stage]["graph_set"]
                for stage in contract["required_stages"]})
        },
        "benchmark_options": {
            "file_pr_current_i2":
                manifest["benchmark_options"]["file_pr_current_i2"],
        },
    }
    return {
        "schema": SCHEMA,
        "schema_version": int(contract.get("schema_version", 0)),
        "valid": True,
        "profile": contract["profile"],
        "git_head": resolved["git_head"],
        "manifest_sha256": file_sha256(manifest_path),
        "contract_sha256": file_sha256(contract_path),
        "profile_config_sha256": canonical_digest(profile_config),
        "run_config_hash": resolved.get("run_config_hash"),
        "run_dir": str(run_dir.resolve()),
        "expected_stage_count": len(contract["required_stages"]),
        **row_summary,
        "corpus_receipt": corpus,
        "source_fingerprints": dict(sorted(sources.items())),
        "input_fingerprints": inputs,
        "output_digests": outputs,
    }


def write_receipt(
        root: Path, run_dir: Path, manifest_path: Path, contract_path: Path
) -> Path:
    payload = build_receipt(root, run_dir, manifest_path, contract_path)
    path = run_dir / RECEIPT_NAME
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)
    return path


def validate_authorization(
        receipt_path: Path, root: Path, manifest_path: Path,
        contract_path: Path
) -> dict[str, Any]:
    recorded = load_json(receipt_path)
    require(receipt_path.name == RECEIPT_NAME,
            "unexpected equivalence receipt filename")
    require(recorded.get("valid") is True, "equivalence receipt is not valid")
    run_dir = Path(str(recorded.get("run_dir", "")))
    require(run_dir.is_dir(), "equivalence run directory is missing")
    require(receipt_path.resolve().parent == run_dir.resolve(),
            "equivalence receipt is outside its recorded run directory")
    require(contained(run_dir, root / "results"),
            "equivalence run directory is outside repository results")
    recomputed = build_receipt(root, run_dir, manifest_path, contract_path)
    require(recomputed == recorded,
            "equivalence receipt is stale or its evidence changed")
    return {
        "path": str(receipt_path.resolve()),
        "sha256": file_sha256(receipt_path),
        "git_head": recorded["git_head"],
        "manifest_sha256": recorded["manifest_sha256"],
        "contract_sha256": recorded["contract_sha256"],
        "profile_config_sha256": recorded["profile_config_sha256"],
        "roster_sha256": recorded["roster_sha256"],
        "rows_sha256": recorded["rows_sha256"],
        "source_fingerprints": recorded["source_fingerprints"],
        "input_fingerprints": recorded["input_fingerprints"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write or independently verify a current ECG equivalence receipt.")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write-run-dir", type=Path)
    action.add_argument("--validate-receipt", type=Path)
    parser.add_argument(
        "--manifest", type=Path,
        default=Path("scripts/experiments/ecg/experiment_manifest.json"))
    parser.add_argument(
        "--config", type=Path,
        default=Path("scripts/experiments/ecg/configs/record_equivalence.json"))
    parser.add_argument("--project-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    root = args.project_root.resolve()
    manifest = (
        args.manifest if args.manifest.is_absolute()
        else root / args.manifest)
    config = args.config if args.config.is_absolute() else root / args.config
    try:
        if args.write_run_dir is not None:
            run_dir = (
                args.write_run_dir if args.write_run_dir.is_absolute()
                else root / args.write_run_dir)
            path = write_receipt(root, run_dir, manifest, config)
            print(path)
        else:
            receipt = (
                args.validate_receipt
                if args.validate_receipt.is_absolute()
                else root / args.validate_receipt)
            print(json.dumps(
                validate_authorization(receipt, root, manifest, config),
                sort_keys=True))
    except EquivalenceGateError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
