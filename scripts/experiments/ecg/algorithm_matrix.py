"""Current shared-kernel cells and receipts, called by the existing ROI runner."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
from typing import Any, Callable

if __package__:
    from .record_receipts import RecordReceiptError, receipt, require, resolve_layout, unsigned
    from .record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources
else:
    from record_receipts import RecordReceiptError, receipt, require, resolve_layout, unsigned
    from record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources


ROOT = Path(__file__).resolve().parents[3]
CONFIG = Path(__file__).with_name("configs") / "algorithm_equivalence.json"
ALGORITHMS = frozenset(("spmv", "bfs", "sssp", "cc", "bc", "tc"))
MODES = frozenset(("transport", "replacement", "prefetch", "replacement-prefetch"))


def contract() -> dict[str, Any]:
    return json.loads(CONFIG.read_text())


def source_paths(suite: str) -> dict[str, Path]:
    configuration = contract()
    relative = configuration["source_paths"] + configuration["backend_source_paths"].get(suite, [])
    return {"algorithm_contract": CONFIG, **{path: ROOT / path for path in relative}}


def parse_options(text: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--source", type=int, default=0)
    parser.add_argument("--sources", default="")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--delta", type=int, default=8)
    parser.add_argument("--max-passes", type=int, default=1000000)
    parsed = parser.parse_args(shlex.split(text))
    if min(parsed.repeat, parsed.delta, parsed.max_passes) <= 0 or parsed.source < 0:
        raise RecordResourceError("invalid current algorithm parameters")
    parsed.source_list = []
    if parsed.sources:
        if not re.fullmatch(r"[0-9]+(?:,[0-9]+)*", parsed.sources):
            raise RecordResourceError("invalid explicit BC source list")
        parsed.source_list = [int(value) for value in parsed.sources.split(",")]
    parsed.graph = parsed.graph if parsed.graph.is_absolute() else ROOT / parsed.graph
    return parsed


def _integer(fields: dict[str, Any], name: str) -> int:
    value = fields.get(name)
    require(type(value) is int and 0 <= value <= (1 << 64) - 1, f"invalid algorithm integer {name}")
    return value


def validate_payload(
    payload: dict[str, Any], log: str, *, algorithm: str, mode: str, policy: str,
    graph: GraphInfo, graph_path: Path, options: argparse.Namespace, requested_bytes: int,
    minimum_mantissa_bits: int, evidence: bool, llc_sets: int,
) -> dict[str, Any]:
    require(payload.get("schema") == "ecg.algorithm-result.v1" and
            payload.get("backend") == "cache_sim" and payload.get("mode") == mode and
            payload.get("policy") == policy and payload.get("timing_valid_for_speedup") is False,
            "algorithm backend/mode/policy receipt mismatch")
    require(payload.get("measurement_scope") == "algorithm-data-traffic-including-construction",
            "algorithm costs do not cover construction")
    work = payload.get("workload")
    require(isinstance(work, dict), "missing algorithm workload")
    specification = contract()["algorithms"][algorithm]
    require(work.get("schema") == "ecg.algorithm-workload.v1" and
            work.get("algorithm") == algorithm and work.get("variant") == specification["variant"] and
            work.get("prediction_semantics") == specification["semantics"],
            "algorithm variant or prediction semantics mismatch")
    records = mode != "csr"
    require(work.get("carrier") == ("record" if records else "csr") and
            _integer(work, "weighted") == int(graph.weighted) and
            _integer(work, "evidence") == int(evidence),
            "algorithm carrier, weight or instrumentation mismatch")
    require(_integer(work, "vertices") == graph.vertices and
            _integer(work, "source_edges") == graph.records,
            "algorithm receipt differs from the actual graph")
    carrier_count = graph.records // 2 if algorithm == "tc" else graph.records
    require(_integer(work, "carrier_records") == carrier_count, "unexpected structural stream count")
    passes, actual, skipped = (_integer(work, key) for key in ("passes", "actual_records", "skipped_positions"))
    require(passes > 0 and actual + skipped == _integer(work, "structural_positions") == passes * carrier_count,
            "algorithm pass accounting does not close")
    require(_integer(work, "source") == options.source and
            _integer(work, "source_count") == (len(options.source_list) or 1) and
            _integer(work, "repetitions") == options.repeat and _integer(work, "delta") == options.delta,
            "algorithm parameter receipt mismatch")
    if algorithm in ("spmv", "tc"):
        require(passes == options.repeat and skipped == 0, "dense algorithm omitted structural work")
    if algorithm == "bc":
        require(_integer(work, "bindings") == 2 * (len(options.source_list) or 1),
                "BC did not execute both property phases for every source")
    else:
        require(_integer(work, "bindings") == 1, "unexpected algorithm rebind")
    for key in ("result_digest", "work_trace_digest", "position_trace_digest", "source_list_digest"):
        require(bool(re.fullmatch(r"[0-9a-f]{16}", str(work.get(key, "")))), f"missing algorithm digest {key}")
    if evidence:
        require(work["work_trace_digest"] != "0000000000000000" and
                work["position_trace_digest"] != "0000000000000000", "empty algorithm evidence")
    if records:
        maximum = _integer(work, "maximum_encoded_id")
        require(maximum <= graph.maximum_id and (algorithm == "tc" or maximum == graph.maximum_id),
                "encoded VID bound disagrees with the actual OUT stream")
        layout = resolve_layout(
            records=carrier_count, vertices=graph.vertices, maximum_id=maximum,
            traversals=options.repeat if algorithm in ("spmv", "tc") else 1,
            requested_bytes=requested_bytes, minimum_mantissa_bits=minimum_mantissa_bits)
        for key in ("record_bytes", "id_bits", "metadata_bits", "mantissa_bits"):
            require(_integer(work, key) == layout[key], f"algorithm layout mismatch: {key}")
        require(_integer(work, "carrier_allocation_bytes") == carrier_count * layout["record_bytes"] and
                _integer(work, "construction_read_bytes") > 0 and _integer(work, "construction_write_bytes") > 0,
                "missing charged immutable carrier construction")
        runtime = receipt(log, "ECG-RECORD-FUNCTIONAL")
        require(runtime.get("mechanism") == mode and unsigned(runtime, "managed_passes") == 1 and
                unsigned(runtime, "record_loads") == actual and unsigned(runtime, "governed_loads") == actual and
                unsigned(runtime, "record_read_bytes") == actual * layout["record_bytes"] and
                unsigned(runtime, "passes") == passes and unsigned(runtime, "skipped_positions") == skipped and
                unsigned(runtime, "structural_positions") == actual + skipped,
                "runtime transport did not observe the declared algorithm work")
        require(unsigned(runtime, "pending") == 0 and unsigned(runtime, "accounting") == 1 and
                unsigned(runtime, "generated") == unsigned(runtime, "enqueued") + unsigned(runtime, "coalesced") and
                unsigned(runtime, "enqueued") == unsigned(runtime, "delivered") and
                unsigned(runtime, "delivered") == sum(unsigned(runtime, key) for key in (
                    "applied", "absent", "stale", "expired")),
                "algorithm metadata queue did not drain without loss")
        rebinds = _integer(work, "bindings") - 1
        require(unsigned(runtime, "rebinds") == rebinds and
                unsigned(runtime, "invalidation_steps") == rebinds * llc_sets,
                "algorithm property transitions omitted their paid invalidation walk")
        replacement = mode in ("replacement", "replacement-prefetch")
        require(unsigned(runtime, "generated") == (actual + unsigned(runtime, "ordinary_invalidations")
                if replacement else 0), "ordinary governed-region invalidations are unaccounted")
    else:
        require(_integer(work, "carrier_allocation_bytes") == 0, "CSR baseline built an ECG carrier")
    reference = contract()["references"][algorithm]
    if evidence and graph_path.name == reference["graph"] and options.source == 0 and not options.source_list:
        require(graph.sha256 == contract()["graphs"][reference["graph"]]["sha256"],
                "reference fixture name has changed contents")
        for key, expected in reference.items():
            if key != "graph":
                require(work.get(key) == expected, f"independent {algorithm} reference failed: {key}")
    return work


def _file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_cache_cell(
    args: argparse.Namespace, out_dir: Path, spec: Any, l3_size: str,
    run_command: Callable[..., Any], parse_size_bytes: Callable[[str], int],
) -> list[dict[str, Any]]:
    row: dict[str, Any] = {
        "simulator": "cache_sim", "benchmark": args.benchmark, "policy": spec.label,
        "l3_size": l3_size, "l3_ways": int(args.l3_ways), "status": "error",
        "timing_valid_for_speedup": "0", "measurement_scope": "algorithm-data-traffic-including-construction",
        "current_algorithm": "1", "algorithm_record_requested_bytes": args.ecg_record_bytes,
    }
    try:
        require(args.benchmark in ALGORITHMS, "unknown current algorithm")
        require(args.prefetcher == "none" and args.flowthrough == "off" and int(args.ecg_charged) == 1 and
                int(args.cache_stream_prefetch_degree) == 0 and parse_size_bytes(str(args.line_size)) == 64 and
                int(args.cache_sim_omp_threads) == 1 and not args.current_pr_baselines,
                "current algorithms require serial real-record execution without legacy mechanisms")
        mode = spec.record_mechanism or "csr"
        policy = "LRU" if mode != "csr" else spec.label
        require(mode in MODES or (mode == "csr" and policy in ("LRU", "SRRIP", "GRASP_PAPER")),
                "unsupported current algorithm policy; dynamic P-OPT would require an oracle")
        options = parse_options(args.options)
        graph = graph_info(options.graph, allow_weighted=True, traversal="out")
        require(options.source < graph.vertices and all(source < graph.vertices for source in options.source_list),
                "algorithm source is outside the graph")
        require(args.benchmark == "bc" or not options.source_list, "source list requires BC")
        require(not args.ecg_equivalence or graph.vertices <= 4096 and graph.records <= 65536,
                "algorithm instrumentation is limited to bounded qualification inputs")
        plan = plan_algorithm_resources(
            graph, algorithm=args.benchmark, records=mode != "csr",
            requested_bytes=args.ecg_record_bytes, minimum_mantissa_bits=args.ecg_record_minimum_mantissa_bits,
            traversals=options.repeat, sources=len(options.source_list) or 1,
            workspace_limit=args.algorithm_workspace_bytes,
            carrier_limit=args.ecg_record_max_carrier_bytes, auxiliary_limit=args.ecg_record_max_auxiliary_bytes,
            rss_mib=args.cache_record_rss_mib)
        binary = ROOT / "bench/bin_sim/algorithms"
        label = f"cache_sim_{args.benchmark}_{spec.safe_label}_L3{parse_size_bytes(l3_size)}"
        data_path = out_dir / "cache_sim" / f"{label}.json"
        log_path = out_dir / "logs" / f"{label}.log"
        data_path.parent.mkdir(parents=True, exist_ok=True)
        command = [
            str(binary), "--algorithm", args.benchmark, "--graph", str(options.graph),
            "--source", str(options.source), "--repeat", str(options.repeat), "--delta", str(options.delta),
            "--max-passes", str(options.max_passes), "--mode", mode, "--policy", policy,
            "--record-bytes", str(args.ecg_record_bytes),
            "--minimum-mantissa-bits", str(args.ecg_record_minimum_mantissa_bits),
            "--graph-bytes", str(plan["graph_loader_bytes_upper"]),
            "--workspace-bytes", str(args.algorithm_workspace_bytes),
            "--carrier-bytes", str(args.ecg_record_max_carrier_bytes),
            "--auxiliary-bytes", str(args.ecg_record_max_auxiliary_bytes),
            "--l1-bytes", str(parse_size_bytes(str(args.l1d_size))), "--l1-ways", str(args.l1d_ways),
            "--l2-bytes", str(parse_size_bytes(str(args.l2_size))), "--l2-ways", str(args.l2_ways),
            "--llc-bytes", str(parse_size_bytes(l3_size)), "--llc-ways", str(args.l3_ways),
            "--output", str(data_path),
        ]
        if options.sources:
            command.extend(("--sources", options.sources))
        if args.ecg_equivalence:
            command.extend(("--evidence", "--values"))
        setarch = shutil.which("setarch")
        if setarch:
            command = [setarch, platform.machine(), "-R", *command]
        elif args.require_cache_sim_aslr_disable:
            raise RecordReceiptError("controlled algorithm runs require setarch -R")
        env = {key: value for key, value in os.environ.items() if not key.startswith(
            ("CACHE_", "ECG_", "GEM5_", "SNIPER_", "POPT_", "GRASP_", "STRUCTURAL_", "TOPT_", "OMP_"))}
        env.update(OMP_NUM_THREADS="1", OMP_WAIT_POLICY="PASSIVE", GRAPHBREW_SIDEBAND_LOG="0")
        before = _file_hash(binary) if binary.is_file() else ""
        completed = run_command(command, ROOT, env, args.timeout_cache, log_path, args.dry_run,
                                rss_mib=args.cache_record_rss_mib)
        if args.dry_run:
            return []
        require(completed is not None and completed.returncode == 0,
                f"algorithm execution failed: {completed.returncode if completed else 'no-result'}")
        require(data_path.is_file() and before == _file_hash(binary), "algorithm output missing or binary changed")
        require(graph_info(options.graph, allow_weighted=True, traversal="out").sha256 == graph.sha256,
                "algorithm input changed while executing")
        payload = json.loads(data_path.read_text())
        work = validate_payload(payload, log_path.read_text(), algorithm=args.benchmark, mode=mode, policy=policy,
            graph=graph, graph_path=options.graph, options=options, requested_bytes=args.ecg_record_bytes,
            minimum_mantissa_bits=args.ecg_record_minimum_mantissa_bits, evidence=args.ecg_equivalence,
            llc_sets=parse_size_bytes(l3_size) // (64 * int(args.l3_ways)))
        metrics = payload.get("metrics")
        require(isinstance(metrics, dict) and isinstance(metrics.get("L3"), dict),
                "algorithm cache metrics are missing")
        traffic = _integer(metrics, "total_offchip_traffic")
        misses, hits = _integer(metrics["L3"], "misses"), _integer(metrics["L3"], "hits")
        row.update({
            "status": "ok", "json_path": str(data_path), "log_path": str(log_path),
            "graph_sha256": graph.sha256, "benchmark_binary_sha256": before,
            "algorithm_workload_verified": "1",
            **{"algorithm_" + key: value for key, value in work.items()
               if key != "algorithm" and not key.startswith("values_")},
            "total_accesses": _integer(metrics, "total_accesses"),
            "memory_accesses": _integer(metrics, "memory_accesses"),
            "total_offchip_traffic": traffic, "llc_writebacks": _integer(metrics, "llc_writebacks"),
            "prefetch_fills": _integer(metrics, "prefetch_fills"),
            "l3_misses": misses, "l3_hits": hits, "l3_accesses": hits + misses,
            "l3_miss_rate": misses / (hits + misses) if hits + misses else 0,
            "host_seconds": payload["host_seconds"], "planned_host_bytes": plan["planned_host_bytes"],
            "resource_scope": plan["memory_plan"],
        })
    except (RecordReceiptError, RecordResourceError, OSError, json.JSONDecodeError) as error:
        row["error"] = str(error)
    return [row]


def certify_rows(rows: list[dict[str, Any]]) -> None:
    groups: dict[tuple[str, str, str, str], list[dict[str, Any]]] = {}
    for row in rows:
        key = tuple(str(row.get(field, "")) for field in ("simulator", "benchmark", "l3_size", "graph_sha256"))
        groups.setdefault(key, []).append(row)
    comparable = (
        "variant", "prediction_semantics", "result_digest", "work_trace_digest", "position_trace_digest",
        "source", "source_count", "source_list_digest", "repetitions", "delta", "vertices", "source_edges",
        "carrier_records", "passes", "structural_positions", "actual_records", "skipped_positions",
        "csr_index_reads", "weight_reads", "ordinary_property_reads", "property_writes", "auxiliary_accesses",
        "reached", "levels", "components", "relax_attempts", "relax_successes", "light_passes", "heavy_passes",
        "sigma_max", "triangles", "oriented_edges", "intersection_comparisons", "bindings",
    )
    for group in groups.values():
        good = [row for row in group if row.get("status") == "ok"]
        signatures = {tuple(str(row.get("algorithm_" + field, "")) for field in comparable) for row in good}
        if len(signatures) != 1:
            for row in good:
                row.update(status="error", error="current algorithm result/work differs across policies")
            continue
        baselines = {str(row["policy"]): row for row in good}
        for row in good:
            for label, column in (("LRU", "traffic_ratio_vs_csr_lru"),
                                  ("ECG_TRANSPORT", "traffic_ratio_vs_transport")):
                baseline = baselines.get(label)
                if baseline is not None and int(baseline["total_offchip_traffic"]) > 0:
                    row[column] = int(row["total_offchip_traffic"]) / int(baseline["total_offchip_traffic"])
