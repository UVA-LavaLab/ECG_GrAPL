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
    from .record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources, popt_matrix_lines
else:
    from record_receipts import RecordReceiptError, receipt, require, resolve_layout, unsigned
    from record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources, popt_matrix_lines


ROOT = Path(__file__).resolve().parents[3]
CONFIG = Path(__file__).with_name("configs") / "algorithm_equivalence.json"
ALGORITHMS = frozenset(("spmv", "bfs", "sssp", "cc", "bc", "tc"))
MODES = frozenset(("transport", "replacement", "prefetch", "replacement-prefetch"))


def contract() -> dict[str, Any]:
    return json.loads(CONFIG.read_text())


def source_paths(suite: str) -> dict[str, Path]:
    configuration = contract()
    relative = configuration["source_paths"] + configuration["backend_source_paths"].get(suite, [])
    if suite in ("gem5", "sniper"):
        shared = json.loads((CONFIG.parent / "record_equivalence.json").read_text())
        prefixes = ("bench/include/gem5_sim/", "scripts/setup_gem5.py") if suite == "gem5" else (
            "bench/include/sniper_sim/", "bench/src_sniper/ecg_record_guest.h", "scripts/setup_sniper.py")
        relative += [path for path in shared["source_paths"] if path.startswith(prefixes)]
    return {"algorithm_contract": CONFIG, **{path: ROOT / path for path in relative}}


def parse_options(text: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--source", type=int, default=0)
    parser.add_argument("--sources", default="")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--delta", type=int, default=8)
    parser.add_argument("--max-passes", type=int, default=1000000)
    parser.add_argument("--record-preprocess", choices=("csr", "traversal"), default="csr")
    parser.add_argument("--record-base-policy", choices=("LRU", "GRASP_PAPER"), default="LRU")
    parser.add_argument("--window-observer", choices=("off", "control", "window"), default="off")
    parser.add_argument("--window-observer-bytes", type=int, default=128 << 20)
    parser.add_argument("--bfs-direction", choices=("td", "do"), default="td")
    parser.add_argument("--bfs-alpha", type=int, default=15)
    parser.add_argument("--bfs-beta", type=int, default=18)
    parsed = parser.parse_args(shlex.split(text))
    if min(parsed.repeat, parsed.delta, parsed.max_passes, parsed.bfs_alpha, parsed.bfs_beta,
           parsed.window_observer_bytes) <= 0 or parsed.source < 0:
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


def record_policy_label(label: str, mode: str, base_policy: str) -> str:
    if mode == "csr" or base_policy == "LRU":
        return label
    return f"{label}_BASE_{base_policy}"


def observer_policy_label(label: str, observer: str) -> str:
    return label if observer == "off" else f"{label}_OBS_{observer.upper()}"


def policy_labels(policies, base_policy: str = "LRU", observer: str = "off") -> list[str]:
    return [observer_policy_label(record_policy_label(spec.label, spec.record_mechanism or "csr", base_policy), observer)
            for spec in policies]


def validate_payload(
    payload: dict[str, Any], log: str, *, algorithm: str, mode: str, policy: str,
    graph: GraphInfo, graph_path: Path, options: argparse.Namespace, requested_bytes: int,
    minimum_mantissa_bits: int, evidence: bool, llc_sets: int,
    backend: str = "cache_sim",
) -> dict[str, Any]:
    require(payload.get("schema") == "ecg.algorithm-result.v1" and
            payload.get("backend") == backend and payload.get("mode") == mode and
            payload.get("policy") == policy and payload.get("timing_valid_for_speedup") is False,
            "algorithm backend/mode/policy receipt mismatch")
    expected_base = options.record_base_policy if mode != "csr" else "LRU"
    require(payload.get("record_base_policy") == expected_base,
            "algorithm record base-policy receipt mismatch")
    observing = options.window_observer != "off"
    require(payload.get("diagnostic_only", False) is observing,
            "algorithm diagnostic scope mismatch")
    require(payload.get("measurement_scope") == (
                "observation-only-unchanged-grasp" if observing else
                "algorithm-data-traffic-including-construction" if backend == "cache_sim"
                else "algorithm-setup-kernel-drain"),
            "algorithm costs do not cover construction")
    work = payload.get("workload")
    require(isinstance(work, dict), "missing algorithm workload")
    specification = contract()["algorithms"][algorithm]
    direction_optimizing = options.bfs_direction == "do"
    require(not direction_optimizing or algorithm == "bfs", "direction optimization requires BFS")
    expected_variant = "sorted-direction-optimizing-td-records-bu-bitmap" if direction_optimizing else specification["variant"]
    require(work.get("schema") == "ecg.algorithm-workload.v1" and
            work.get("algorithm") == algorithm and work.get("variant") == expected_variant and
            work.get("prediction_semantics") == specification["semantics"] and
            work.get("record_base_policy") == expected_base,
            "algorithm variant or prediction semantics mismatch")
    records = mode != "csr"
    reuse_scope = ({"sssp": "light-heavy", "cc": "sample-rounds", "bfs": "row-local", "bc": "row-local"}
                   .get(algorithm, "full-csr") if options.record_preprocess == "traversal" else "full-csr")
    if not records:
        reuse_scope = "none"
    require(work.get("record_preprocess", "csr") == options.record_preprocess and
            (("record_preprocess" not in work and options.record_preprocess == "csr") or
             work.get("record_reuse_scope") == reuse_scope),
            "algorithm preprocessing or reference-scope mismatch")
    require(work.get("carrier") == ("record" if records else "csr") and
            _integer(work, "weighted") == int(graph.weighted) and
            _integer(work, "evidence") == int(evidence) and
            _integer(work, "memory_counts_measured") == int(evidence or backend == "cache_sim"),
            "algorithm carrier, weight or instrumentation mismatch")
    require(_integer(work, "vertices") == graph.vertices and
            _integer(work, "source_edges") == graph.records,
            "algorithm receipt differs from the actual graph")
    carrier_count = graph.records // 2 if algorithm == "tc" else graph.records
    require(_integer(work, "carrier_records") == carrier_count, "unexpected structural stream count")
    passes, actual, skipped = (_integer(work, key) for key in ("passes", "actual_records", "skipped_positions"))
    require((passes > 0 or direction_optimizing) and
            actual + skipped == _integer(work, "structural_positions") == passes * carrier_count,
            "algorithm pass accounting does not close")
    if algorithm == "bfs":
        td, bu = _integer(work, "bfs_td_levels"), _integer(work, "bfs_bu_levels")
        require(work.get("bfs_direction") == options.bfs_direction and
                _integer(work, "bfs_alpha") == options.bfs_alpha and
                _integer(work, "bfs_beta") == options.bfs_beta and
                td == passes and td + bu == _integer(work, "levels") and td + bu > 0 and
                _integer(work, "bfs_td_edges") == actual and
                _integer(work, "bfs_bu_vertices") == bu * graph.vertices and
                work.get("bfs_bu_transport") == "ordinary-bitmap" and
                (direction_optimizing or bu == 0),
                "BFS direction, bitmap or level-work accounting mismatch")
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
    for key in ("result_digest", "work_trace_digest", "position_trace_digest", "record_trace_digest", "source_list_digest"):
        require(bool(re.fullmatch(r"[0-9a-f]{16}", str(work.get(key, "")))), f"missing algorithm digest {key}")
    if evidence:
        require(work["work_trace_digest"] != "0000000000000000" and
                work["position_trace_digest"] != "0000000000000000", "empty algorithm evidence")
        if records:
            require(work["record_trace_digest"] != "0000000000000000", "empty actual-record semantic evidence")
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
        if "record_preprocess" in work:
            require(sum(_integer(work, "constructed_" + state + "_records") for state in
                        ("finite", "wrap", "unknown")) == carrier_count,
                    "constructed record states do not cover the carrier")
            if reuse_scope == "row-local":
                require(_integer(work, "constructed_wrap_records") == 0,
                        "row-local preprocessing incorrectly predicts another row/pass")
        runtime = receipt(log, {"cache_sim": "ECG-RECORD-FUNCTIONAL",
                               "gem5": "ECG-RECORD-NATIVE", "sniper": "SNIPER-ECG-RECORD"}[backend])
        if backend == "gem5":
            require(unsigned(runtime, "replacement") == int(mode in ("replacement", "replacement-prefetch")) and
                    unsigned(runtime, "prefetch") == int(mode in ("prefetch", "replacement-prefetch")) and
                    unsigned(runtime, "errors") == 0 and unsigned(runtime, "required_update_drops") == 0,
                    "native mechanism or required-update delivery mismatch")
            runtime = {**runtime, "mechanism": mode, "managed_passes": "1",
                       "skipped_positions": runtime["skipped"],
                       "invalidation_steps": runtime["invalidation_cycles"]}
        elif backend == "sniper":
            require(unsigned(runtime, "errors") == 0 and unsigned(runtime, "clean") == 1 and
                    unsigned(runtime, "pending_updates") == 0 and unsigned(runtime, "pending_prefetches") == 0,
                    "Sniper algorithm transport did not finish cleanly")
            require(unsigned(runtime, "record_reads") == unsigned(runtime, "loaded_values") and
                    unsigned(runtime, "record_reads") >= actual and
                    unsigned(runtime, "record_read_bytes") == unsigned(runtime, "record_reads") * layout["record_bytes"],
                    "Sniper software window did not use real loaded values")
            runtime = {**runtime, "managed_passes": "1", "pending": "0",
                       "record_loads": runtime["consumed_records"],
                       "governed_loads": runtime["property_accesses"],
                       "record_read_bytes": str(actual * layout["record_bytes"]),
                       "generated": runtime["generated_updates"], "enqueued": runtime["enqueued_updates"],
                       "coalesced": runtime["coalesced_updates"], "delivered": runtime["delivered_updates"],
                       "applied": runtime["applied_updates"], "absent": runtime["not_resident_updates"],
                       "stale": runtime["stale_updates"], "expired": runtime["expired_updates"],
                       "invalidation_steps": runtime["invalidation_cycles"]}
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
    if policy == "POPT_UNCHARGED":
        popt = payload.get("popt")
        require(backend == "cache_sim" and mode == "csr" and not direction_optimizing and
                isinstance(popt, dict) and popt.get("encoding") == "full" and
                popt.get("scope") == "graph-pass-irregular-regions" and
                popt.get("full_data_capacity") is True and
                popt.get("runtime_matrix_traffic_charged") is False,
                "P-OPT must declare its favorable full-capacity graph-pass scope")
        lines = popt_matrix_lines(algorithm, graph.vertices)
        require(_integer(popt, "matrix_lines") == lines and
                _integer(popt, "matrix_bytes") == lines * 256 and _integer(popt, "epochs") == 256 and
                _integer(popt, "banks") == (3 if algorithm == "bc" else 1) and
                _integer(popt, "covered_regions") == (3 if algorithm == "bc" else 1) and
                _integer(popt, "passes") == passes and _integer(popt, "governed_reads") == actual and
                _integer(popt, "construction_read_bytes") > 0 and
                _integer(popt, "construction_write_bytes") > 0,
                "P-OPT matrix coverage, construction or progress mismatch")
    reference = contract()["references"][algorithm]
    if evidence and graph_path.name == reference["graph"] and options.source == 0 and not options.source_list:
        require(graph.sha256 == contract()["graphs"][reference["graph"]]["sha256"],
                "reference fixture name has changed contents")
        for key, expected in reference.items():
            if key != "graph":
                require(work.get(key) == expected, f"independent {algorithm} reference failed: {key}")
    return work


def validate_window_observer(payload: dict[str, Any], options: argparse.Namespace) -> dict[str, Any]:
    observer = payload.get("window_observer")
    if options.window_observer == "off":
        require(observer is None, "unrequested window observer")
        return {}
    work = payload["workload"]
    require(work["algorithm"] == "bfs" and work["carrier"] == "csr" and work["bfs_direction"] == "td" and
            payload["policy"] == "GRASP_PAPER" and payload["backend"] == "cache_sim" and
            isinstance(observer, dict) and observer.get("schema") == "ecg.window-eviction-observer.v1" and
            observer.get("mode") == options.window_observer and observer.get("active_policy_changed") is False and
            observer.get("diagnostic_costs_in_cache_counters") is False and
            observer.get("delivery_model") == "uncoalesced-eight-access-steps-serialized-markers" and
            _integer(payload["metrics"], "prefetch_fills") == 0,
            "window observer changed policy or omitted its diagnostic limitations")
    minimum = max(8, (work["vertices"] + 255) // 256)
    cohort = 1 << (minimum - 1).bit_length()
    require(_integer(observer, "cohort_rows") == cohort and _integer(observer, "bin_rows") == cohort // 8 and
            _integer(observer, "bins_per_pass") == (work["vertices"] + cohort // 8 - 1) // (cohort // 8) and
            _integer(observer, "sample_period") == 256 and _integer(observer, "trial_capacity") == 256 and
            _integer(observer, "trial_horizon_requests") == 131072 and _integer(observer, "latency_steps") == 8,
            "window observer parameters changed")
    require(_integer(observer, "passes") == work["passes"] and
            _integer(observer, "designated_reads") == work["actual_records"] and
            _integer(observer, "memory_requests") == payload["traffic_phases"]["kernel"]["total_accesses"] and
            _integer(observer, "reserved_bytes") <= options.window_observer_bytes and
            _integer(observer, "queue_pending") == 0 and _integer(observer, "queue_peak") <= 16 and
            _integer(observer, "queue_enqueued") == _integer(observer, "queue_delivered") ==
            sum(_integer(observer, "queue_" + key) for key in ("applied", "stale", "absent")),
            "window observer work, storage or queue does not close")
    windows = options.window_observer == "window"
    require(_integer(observer, "constructed_records") == (work["source_edges"] if windows else 0) and
            _integer(observer, "annotation_bytes") == 2 * _integer(observer, "constructed_records") and
            _integer(observer, "constructed_known") <= _integer(observer, "constructed_records") and
            _integer(observer, "executed_known") <= work["actual_records"] and
            _integer(observer, "marker_control_bytes") == _integer(observer, "markers") * 48,
            "window observer annotation or marker accounting mismatch")
    require(_integer(observer, "queue_enqueued") == (work["actual_records"] +
            _integer(observer, "ordinary_reads") + _integer(observer, "ordinary_writes") if windows else 0),
            "window observer omitted required shadow updates")
    require(observer.get("sample_state_order") == [
        "unseen", "unknown", "pending", "invalidated", "expired", "old-pass", "live", "non-property"],
        "window observer status interpretation changed")
    for key in ("demand_digest", "victim_digest"):
        require(bool(re.fullmatch("[0-9a-f]{16}", str(observer.get(key, "")))), "missing observer noninterference digest")
    for key in ("immediate", "delivered"):
        values = observer.get(key)
        require(isinstance(values, dict) and isinstance(values.get("base_states"), list) and
                len(values["base_states"]) == 8 and all(type(v) is int and v >= 0 for v in values["base_states"]) and
                sum(values["base_states"]) == _integer(values, "samples") ==
                (observer["pass_evictions"] // 256 if windows else 0) and
                _integer(values, "hypothetical_overrides") <= values["base_states"][6] and
                _integer(values, "live_eligible_candidates") <= _integer(values, "eligible_candidates"),
                "window observer sample accounting mismatch")
    trials = observer.get("trials")
    require(isinstance(trials, dict) and _integer(trials, "started") + _integer(trials, "capacity_dropped") ==
            observer["delivered"]["hypothetical_overrides"] and _integer(trials, "started") ==
            sum(_integer(trials, key) for key in ("base_first", "alternative_first", "censored_horizon", "censored_pass")) and
            _integer(trials, "peak_pending") <= 256,
            "window observer outcomes or censoring do not close")
    for side in ("base", "alternative"):
        require(_integer(trials, side + "_first_memory") <= _integer(trials, side + "_first_llc") <=
                _integer(trials, side + "_first"), "window observer confuses private reuse with LLC demand")
    per_pass = observer.get("per_pass")
    require(isinstance(per_pass, list) and len(per_pass) == work["passes"] <= 128 and
            sum(_integer(row, "designated_reads") for row in per_pass) == work["actual_records"] and
            sum(_integer(row, "evictions") for row in per_pass) == observer["pass_evictions"],
            "window observer pass accounting mismatch")
    for key in ("immediate", "delivered"):
        for field in ("samples", "eligible_candidates", "live_eligible_candidates", "hypothetical_overrides"):
            require(all(isinstance(row.get(key), dict) for row in per_pass) and
                    sum(_integer(row[key], field) for row in per_pass) == observer[key][field],
                    "window observer per-pass samples do not sum to totals")
    require(observer["markers"] <= (observer["bins_per_pass"] + 2) * work["passes"],
            "window observer invented markers for skipped bins")
    return observer


def verify_window_noninterference(control: dict[str, Any], window: dict[str, Any]) -> None:
    require(control.get("diagnostic_only") is True and window.get("diagnostic_only") is True and
            control.get("policy") == window.get("policy") == "GRASP_PAPER",
            "window noninterference requires two unchanged-policy diagnostics")
    a, b = control.get("window_observer"), window.get("window_observer")
    require(isinstance(a, dict) and isinstance(b, dict) and a.get("mode") == "control" and b.get("mode") == "window",
            "window noninterference pair is incomplete")
    for key in ("workload", "metrics", "traffic_phases", "setup_cache_policy", "record_base_policy"):
        require(control.get(key) == window.get(key), f"window observer altered {key}")
    for key in ("demand_digest", "victim_digest", "memory_requests", "llc_fills", "llc_evictions"):
        require(a.get(key) is not None and a.get(key) == b.get(key),
                f"window observer noninterference mismatch: {key}")


def _file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_traffic_phases(payload: dict[str, Any]) -> dict[str, int]:
    phases = payload.get("traffic_phases")
    require(isinstance(phases, dict) and phases.get("boundary") == "first-binding-complete" and
            phases.get("cache_state_preserved") is True, "missing nonintrusive setup/kernel traffic boundary")
    metrics = payload.get("metrics")
    require(isinstance(metrics, dict), "missing total traffic counters")
    result = {}
    for phase in ("setup", "kernel"):
        require(isinstance(phases.get(phase), dict), f"missing {phase} counters")
    for key in ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks", "total_offchip_traffic"):
        setup, kernel = _integer(phases["setup"], key), _integer(phases["kernel"], key)
        require(setup + kernel == _integer(metrics, key), f"setup/kernel counters do not close: {key}")
        result["setup_" + key] = setup
        result["kernel_" + key] = kernel
    for phase in ("setup", "kernel"):
        require(result[phase + "_total_offchip_traffic"] == sum(result[phase + "_" + key] for key in
                ("memory_accesses", "prefetch_fills", "llc_writebacks")), f"invalid {phase} traffic sum")
    llc_fields = {"llc_hits": "hits", "llc_misses": "misses",
                  "llc_property_hits": "prop_hits", "llc_property_misses": "prop_misses"}
    if any(key in phases[phase] for key in llc_fields for phase in ("setup", "kernel")):
        require(isinstance(metrics.get("L3"), dict), "missing LLC phase totals")
        for key, total_key in llc_fields.items():
            setup, kernel = _integer(phases["setup"], key), _integer(phases["kernel"], key)
            require(setup + kernel == _integer(metrics["L3"], total_key),
                    f"setup/kernel LLC counters do not close: {key}")
            result["setup_" + key], result["kernel_" + key] = setup, kernel
        for phase in ("setup", "kernel"):
            for kind in ("hits", "misses"):
                require(result[phase + "_llc_property_" + kind] <= result[phase + "_llc_" + kind],
                        "property LLC counters exceed all-data counters")
    return result


def run_cache_cell(
    args: argparse.Namespace, out_dir: Path, spec: Any, l3_size: str,
    run_command: Callable[..., Any], parse_size_bytes: Callable[[str], int],
) -> list[dict[str, Any]]:
    row: dict[str, Any] = {
        "simulator": "cache_sim", "benchmark": args.benchmark,
        "policy": spec.policy, "policy_label": spec.label,
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
        require(mode in MODES or (mode == "csr" and policy in ("LRU", "SRRIP", "GRASP_PAPER", "POPT_UNCHARGED")),
                "unsupported current algorithm policy or P-OPT accounting mode")
        options = parse_options(args.options)
        observing = options.window_observer != "off"
        require(not observing or args.benchmark == "bfs" and mode == "csr" and policy == "GRASP_PAPER" and
                options.bfs_direction == "td" and options.record_preprocess == "csr",
                "window observer requires CSR TD BFS and unchanged GRASP_PAPER")
        require(mode != "csr" or options.record_base_policy == "LRU",
                "record base policy requires a current record mode")
        row["record_base_policy"] = options.record_base_policy
        row["policy_label"] = observer_policy_label(record_policy_label(
            spec.label, mode, options.record_base_policy), options.window_observer)
        if observing:
            row.update(diagnostic_only="1", measurement_scope="observation-only-unchanged-grasp")
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
            rss_mib=args.cache_record_rss_mib, bfs_direction_optimizing=options.bfs_direction == "do",
            preprocessing=options.record_preprocess, popt_full_capacity=policy == "POPT_UNCHARGED")
        if observing:
            require(not graph.weighted and plan["array_bytes"] + options.window_observer_bytes <=
                    args.algorithm_workspace_bytes, "window observer exceeds its workspace or target scope")
        binary = ROOT / "bench/bin_sim/algorithms"
        base_suffix = "" if options.record_base_policy == "LRU" else (
            "_BASE_" + options.record_base_policy)
        observer_suffix = "" if not observing else "_OBS_" + options.window_observer.upper()
        label = (f"cache_sim_{args.benchmark}_{spec.safe_label}{base_suffix}{observer_suffix}"
                 f"_L3{parse_size_bytes(l3_size)}")
        data_path = out_dir / "cache_sim" / f"{label}.json"
        log_path = out_dir / "logs" / f"{label}.log"
        data_path.parent.mkdir(parents=True, exist_ok=True)
        command = [
            str(binary), "--algorithm", args.benchmark, "--graph", str(options.graph),
            "--source", str(options.source), "--repeat", str(options.repeat), "--delta", str(options.delta),
            "--max-passes", str(options.max_passes), "--mode", mode, "--policy", policy,
            "--record-preprocess", options.record_preprocess,
            "--record-base-policy", options.record_base_policy,
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
        if observing:
            command.extend(("--window-observer", options.window_observer,
                            "--window-observer-bytes", str(options.window_observer_bytes)))
        if args.benchmark == "bfs":
            command.extend(("--bfs-direction", options.bfs_direction,
                            "--bfs-alpha", str(options.bfs_alpha), "--bfs-beta", str(options.bfs_beta)))
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
        require(payload.get("setup_cache_policy") == (
                    options.record_base_policy if mode != "csr"
                    else "LRU" if policy == "POPT_UNCHARGED" else policy),
                "current algorithm preparation did not use its declared unbound cache policy")
        work = validate_payload(payload, log_path.read_text(), algorithm=args.benchmark, mode=mode, policy=policy,
            graph=graph, graph_path=options.graph, options=options, requested_bytes=args.ecg_record_bytes,
            minimum_mantissa_bits=args.ecg_record_minimum_mantissa_bits, evidence=args.ecg_equivalence,
            llc_sets=parse_size_bytes(l3_size) // (64 * int(args.l3_ways)))
        metrics = payload.get("metrics")
        require(isinstance(metrics, dict) and isinstance(metrics.get("L3"), dict),
                "algorithm cache metrics are missing")
        if policy == "POPT_UNCHARGED":
            require(_integer(metrics["L3"], "size_bytes") == parse_size_bytes(l3_size) and
                    _integer(metrics["L3"], "ways") == int(args.l3_ways),
                    "P-OPT full-capacity control lost data capacity")
            row.update({"popt_" + key: value for key, value in payload["popt"].items()})
        traffic = _integer(metrics, "total_offchip_traffic")
        misses, hits = _integer(metrics["L3"], "misses"), _integer(metrics["L3"], "hits")
        row.update(validate_traffic_phases(payload))
        observer = validate_window_observer(payload, options)
        if observing:
            row.update(window_observer=options.window_observer,
                       window_demand_digest=observer["demand_digest"], window_victim_digest=observer["victim_digest"])
        row.update({
            "status": "ok", "json_path": str(data_path), "log_path": str(log_path),
            "setup_cache_policy": payload["setup_cache_policy"],
            "record_base_policy": payload["record_base_policy"],
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
        "variant", "prediction_semantics", "memory_counts_measured",
        "result_digest", "work_trace_digest", "position_trace_digest",
        "source", "source_count", "source_list_digest", "repetitions", "delta", "vertices", "source_edges",
        "carrier_records", "passes", "structural_positions", "actual_records", "skipped_positions",
        "csr_index_reads", "weight_reads", "ordinary_property_reads", "property_writes", "auxiliary_accesses",
        "reached", "levels", "components", "relax_attempts", "relax_successes", "light_passes", "heavy_passes",
        "sigma_max", "triangles", "oriented_edges", "intersection_comparisons", "bindings",
        "bfs_direction", "bfs_alpha", "bfs_beta", "bfs_td_levels", "bfs_bu_levels",
        "bfs_td_edges", "bfs_bu_edges", "bfs_bu_vertices", "bfs_frontier_peak", "bfs_direction_switches",
    )
    for group in groups.values():
        good = [row for row in group if row.get("status") == "ok"]
        signatures = {tuple(str(row.get("algorithm_" + field, "")) for field in comparable) for row in good}
        record_signatures = {str(row.get("algorithm_record_trace_digest", "")) for row in good
                             if row.get("algorithm_carrier") == "record"}
        if len(signatures) != 1 or len(record_signatures) > 1:
            for row in good:
                row.update(status="error", error="current algorithm result/work differs across policies")
            continue
        baselines = {str(row["policy_label"]): row for row in good}
        for row in good:
            transport_label = record_policy_label(
                "ECG_TRANSPORT", "transport",
                str(row.get("record_base_policy", "LRU")))
            for label, column in (("LRU", "traffic_ratio_vs_csr_lru"),
                                  (transport_label, "traffic_ratio_vs_transport")):
                baseline = baselines.get(label)
                if baseline is not None and baseline.get("total_offchip_traffic") is not None and int(baseline["total_offchip_traffic"]) > 0:
                    row[column] = int(row["total_offchip_traffic"]) / int(baseline["total_offchip_traffic"])
                if baseline is not None and int(baseline.get("kernel_total_offchip_traffic", 0)) > 0:
                    row["kernel_" + column] = int(row["kernel_total_offchip_traffic"]) / int(
                        baseline["kernel_total_offchip_traffic"])
