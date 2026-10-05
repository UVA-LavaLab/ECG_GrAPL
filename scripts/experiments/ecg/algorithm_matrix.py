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
    from .record_receipts import RecordReceiptError, receipt, require, resolve_layout, unsigned, validate_kernel_census, validate_kernel_census_passes
    from .record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources, popt_matrix_lines, window_layout
else:
    from record_receipts import RecordReceiptError, receipt, require, resolve_layout, unsigned, validate_kernel_census, validate_kernel_census_passes
    from record_resources import GraphInfo, RecordResourceError, graph_info, plan_algorithm_resources, popt_matrix_lines, window_layout


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
    parser.add_argument("--queries", type=int, default=1)
    parser.add_argument("--delta", type=int, default=8)
    parser.add_argument("--max-passes", type=int, default=1000000)
    parser.add_argument("--record-preprocess", choices=("csr", "traversal"), default="csr")
    parser.add_argument("--record-base-policy", choices=("LRU", "GRASP_PAPER"), default="LRU")
    parser.add_argument("--record-model", choices=("next", "window", "frontier"), default="next")
    parser.add_argument("--frontier-gating", choices=("enabled", "ignored"), default="enabled")
    parser.add_argument("--popt-rank-mode", choices=("future", "constant"), default="future")
    parser.add_argument("--grasp-reference", choices=("off", "full", "flat", "rank"), default="off")
    parser.add_argument("--record-governed-first", choices=("no", "on"), default="no")
    parser.add_argument("--record-rrpv-order", choices=("no", "on"), default="no")
    parser.add_argument("--record-uninformed-base", choices=("no", "on"), default="no")
    parser.add_argument("--record-bound-compare", choices=("on", "no"), default="on")
    parser.add_argument("--record-carrier-first", choices=("no", "on"), default="no")
    parser.add_argument("--record-pressure-gate", choices=("no", "on", "duel"), default="no")
    parser.add_argument("--record-store-bound", choices=("drop", "keep"), default="drop")
    parser.add_argument("--record-expiry-clock", choices=("progress", "delivery"), default="progress")
    parser.add_argument("--grasp-scope", choices=("all", "graph-passes"), default="all")
    parser.add_argument("--grasp-registration", choices=("all", "declared"), default="all")
    parser.add_argument("--kernel-entry", choices=("as-built", "cold"), default="as-built")
    parser.add_argument("--bfs-traffic-phases", choices=("on", "off"), default="off")
    parser.add_argument("--window-candidate-rrpv", choices=(6, 7), type=int, default=6)
    parser.add_argument("--window-observer", choices=("off", "control", "window"), default="off")
    parser.add_argument("--window-observer-bytes", type=int, default=128 << 20)
    parser.add_argument("--bfs-direction", choices=("td", "do"), default="td")
    parser.add_argument("--bfs-alpha", type=int, default=15)
    parser.add_argument("--bfs-beta", type=int, default=18)
    tokens = shlex.split(text)
    parsed = parser.parse_args(tokens)
    if not 1 <= parsed.queries <= 64:
        raise RecordResourceError("queries must be 1 through 64")
    if parsed.record_model != "frontier" and any(
            token == "--frontier-gating" or token.startswith("--frontier-gating=") for token in tokens):
        raise RecordResourceError("frontier gating requires the frontier model")
    if min(parsed.repeat, parsed.delta, parsed.max_passes, parsed.bfs_alpha, parsed.bfs_beta,
           parsed.window_observer_bytes) <= 0 or parsed.source < 0:
        raise RecordResourceError("invalid current algorithm parameters")
    # The order ranks by GRASP tiers, which the kernel registers only for the
    # GRASP_PAPER base. PageRank's labels always name the LRU base, so this is
    # checked here rather than in rrpv_order_label.
    if parsed.record_rrpv_order != "no" and (
            parsed.record_base_policy != "GRASP_PAPER" or parsed.record_model != "next"):
        raise RecordResourceError("record RRPV order requires the GRASP_PAPER record base under the NEXT model")
    if (parsed.record_uninformed_base != "no" or parsed.record_bound_compare != "on" or
            parsed.record_carrier_first != "no") and parsed.record_model != "next":
        raise RecordResourceError("record victim controls require the NEXT model")
    if parsed.record_governed_first != "no" and parsed.record_carrier_first != "no":
        raise RecordResourceError("governed-first and carrier-first are two victim orders, never one")
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


def record_policy_label(label: str, mode: str, base_policy: str, record_model: str = "next",
                        candidate_rrpv: int = 6, frontier_gating: str = "enabled") -> str:
    result = label if mode == "csr" or base_policy == "LRU" else f"{label}_BASE_{base_policy}"
    if record_model == "frontier":
        return f"{result}_MODEL_FRONTIER_RRPV7_GATING_{frontier_gating.upper()}"
    return result if record_model == "next" else f"{result}_MODEL_WINDOW_RRPV{candidate_rrpv}"


def observer_policy_label(label: str, observer: str) -> str:
    return label if observer == "off" else f"{label}_OBS_{observer.upper()}"


def grasp_scope_label(label: str, scope: str) -> str:
    return label if scope == "all" else f"{label}_GRAPH_PASSES"


# NEXT.md §bv C1, C2: the fair-comparison contracts, outermost so every
# historical label is unchanged and no row aliases one run under another.
def grasp_registration_label(label: str, registration: str) -> str:
    return label if registration == "all" else f"{label}_GRASP_DECLARED"


def kernel_entry_label(label: str, entry: str) -> str:
    return label if entry == "as-built" else f"{label}_COLD_ENTRY"


def popt_rank_label(label: str, rank_mode: str) -> str:
    if rank_mode == "future":
        return label
    require(rank_mode == "constant" and label == "POPT_UNCHARGED",
            "constant ranks require the current POPT_UNCHARGED diagnostic")
    return "POPT_UNCHARGED_CONST_RANK"


def expiry_clock_label(label: str, expiry_clock: str) -> str:
    if expiry_clock == "progress":
        return label
    require(label.startswith("ECG_"), "the delivered expiry clock requires a current ECG policy")
    return label + "_DELIVERY_CLOCK"


def store_bound_label(label: str, store_bound: str) -> str:
    if store_bound == "drop":
        return label
    require(label.startswith("ECG_"), "store-bound retention requires a current ECG policy")
    return label + "_STORE_BOUND"


def pressure_gate_label(label: str, pressure_gate: str) -> str:
    if pressure_gate == "no":
        return label
    if "ECG_REPLACEMENT" not in label:
        raise SystemExit(
            "pressure-gate requires a current ECG replacement policy")
    suffixes = {"on": "_PRESSURE_GATE", "duel": "_PRESSURE_DUEL"}
    if pressure_gate not in suffixes:
        raise SystemExit(f"unknown pressure-gate selection: {pressure_gate}")
    return label + suffixes[pressure_gate]


def governed_first_label(label: str, governed_first: str) -> str:
    if governed_first == "no":
        return label
    require(label.startswith("ECG_") and "REPLACEMENT" in label,
            "governed-first requires a current ECG replacement policy")
    return label + "_GOVERNED_FIRST"


# The third victim order takes governed-first's place in the label, since the
# two are exclusive. Like the victim controls it acts only inside the
# replacement rule without prefetch, under the NEXT model.
def carrier_first_label(label: str, carrier_first: str) -> str:
    if carrier_first == "no":
        return label
    require(carrier_first == "on" and label.startswith("ECG_REPLACEMENT") and "_MODEL_" not in label,
            "record carrier-first requires the current ECG replacement policy under the NEXT model")
    return label + "_CARRIER_FIRST"


def rrpv_order_label(label: str, rrpv_order: str) -> str:
    if rrpv_order == "no":
        return label
    # Prefetch admission still consults the base victim, and the window and
    # frontier models choose victims by their own rules.
    require(rrpv_order == "on" and label.startswith("ECG_REPLACEMENT") and "_MODEL_" not in label,
            "record RRPV order requires the current ECG replacement policy under the NEXT model")
    return label + "_RRPV_ORDER"


def written_in_place_label(label: str, written_in_place: str) -> str:
    if written_in_place == "no":
        return label
    # Only PageRank sets the bit, under the replacement mechanism without
    # prefetch; the window and frontier models choose victims by their own rules.
    require(written_in_place == "on" and label.startswith("ECG_REPLACEMENT") and "_MODEL_" not in label,
            "record written-in-place bit requires the current ECG replacement policy under the NEXT model")
    return label + "_WRITTEN_IN_PLACE"


# The two record victim controls act only inside the replacement rule: prefetch
# admission runs the default rule, and the window and frontier models choose
# victims by their own rules. Their suffixes follow every other one.
def uninformed_base_label(label: str, uninformed_base: str) -> str:
    if uninformed_base == "no":
        return label
    require(uninformed_base == "on" and label.startswith("ECG_REPLACEMENT") and "_MODEL_" not in label,
            "record uninformed fallback requires the current ECG replacement policy under the NEXT model")
    return label + "_UNINFORMED_BASE"


def bound_compare_label(label: str, bound_compare: str) -> str:
    if bound_compare == "on":
        return label
    require(bound_compare == "no" and label.startswith("ECG_REPLACEMENT") and "_MODEL_" not in label,
            "record bound-comparison ablation requires the current ECG replacement policy under the NEXT model")
    return label + "_NO_BOUND_COMPARE"


def grasp_reference_label(label: str, mode: str) -> str:
    if mode == "off":
        return label
    require(mode in ("full", "flat", "rank") and label == "GRASP_PAPER",
            "reference consumer requires the ordinary GRASP_PAPER diagnostic")
    return "DIAG_GRASP_REFERENCE_" + mode.upper()


def query_policy_label(label: str, queries: int) -> str:
    if queries == 1:
        return label
    require(1 < queries <= 64 and label in ("LRU", "SRRIP", "GRASP_PAPER", "POPT_UNCHARGED"),
            "independent queries require CSR SpMV baselines")
    return f"{label}_QUERIES{queries}"


def policy_labels(policies, base_policy: str = "LRU", observer: str = "off",
                  record_model: str = "next", candidate_rrpv: int = 6, grasp_scope: str = "all",
                  popt_rank_mode: str = "future", frontier_gating: str = "enabled",
                  grasp_reference: str = "off", queries: int = 1,
                  governed_first: str = "no", store_bound: str = "drop",
                  expiry_clock: str = "progress",
                  pressure_gate: str = "no", rrpv_order: str = "no",
                  written_in_place: str = "no", uninformed_base: str = "no",
                  bound_compare: str = "on", carrier_first: str = "no",
                  grasp_registration: str = "all", kernel_entry: str = "as-built") -> list[str]:
    require(governed_first == "no" or carrier_first == "no",
            "governed-first and carrier-first are two victim orders, never one")
    return [kernel_entry_label(grasp_registration_label(bound_compare_label(uninformed_base_label(pressure_gate_label(query_policy_label(grasp_reference_label(popt_rank_label(grasp_scope_label(observer_policy_label(expiry_clock_label(store_bound_label(written_in_place_label(rrpv_order_label(carrier_first_label(governed_first_label(record_policy_label(
                spec.label, spec.record_mechanism or "csr", base_policy, record_model, candidate_rrpv, frontier_gating), governed_first), carrier_first), rrpv_order), written_in_place), store_bound), expiry_clock), observer),
                grasp_scope), popt_rank_mode), grasp_reference), queries), pressure_gate), uninformed_base), bound_compare),
                grasp_registration), kernel_entry)
            for spec in policies]


def validate_payload(
    payload: dict[str, Any], log: str, *, algorithm: str, mode: str, policy: str,
    graph: GraphInfo, graph_path: Path, options: argparse.Namespace, requested_bytes: int,
    minimum_mantissa_bits: int, evidence: bool, llc_sets: int,
    backend: str = "cache_sim",
    allow_reused_popt: bool = False,
) -> dict[str, Any]:
    require(payload.get("schema") == "ecg.algorithm-result.v1" and
            payload.get("backend") == backend and payload.get("mode") == mode and
            payload.get("policy") == policy and payload.get("timing_valid_for_speedup") is False,
            "algorithm backend/mode/policy receipt mismatch")
    require(payload.get("grasp_scope", "all") == options.grasp_scope, "GRASP phase-scope receipt mismatch")
    validate_fair_comparison(payload, algorithm, options.grasp_registration, options.kernel_entry,
                             sources=len(options.source_list) or 1)
    constant_ranks = options.popt_rank_mode == "constant"
    reference = options.grasp_reference != "off"
    rank_constant = constant_ranks or options.grasp_reference == "flat"
    frontier = options.record_model == "frontier"
    ablation = rank_constant or frontier and options.frontier_gating == "ignored"
    require(payload.get("policy_ablation", False) is ablation and
            (not constant_ranks or backend == "cache_sim" and policy == "POPT_UNCHARGED" and
             mode == "csr" and algorithm in ("spmv", "bfs") and options.bfs_direction == "td"),
            "P-OPT rank-ablation receipt mismatch")
    expected_base = options.record_base_policy if mode != "csr" else "LRU"
    require(payload.get("record_base_policy") == expected_base,
            "algorithm record base-policy receipt mismatch")
    observing = options.window_observer != "off"
    require(payload.get("diagnostic_only", False) is (observing or reference),
            "algorithm diagnostic scope mismatch")
    require(payload.get("measurement_scope") == (
                "ideal-availability-reference-consumer" if reference else
                "observation-only-unchanged-grasp" if observing else
                "algorithm-data-traffic-including-construction" if backend == "cache_sim"
                else "algorithm-setup-kernel-drain"),
            "algorithm costs do not cover construction")
    work = payload.get("workload")
    require(isinstance(work, dict), "missing algorithm workload")
    specification = contract()["algorithms"][algorithm]
    window = options.record_model == "window"
    require(work.get("record_model", "next") == options.record_model, "algorithm record model mismatch")
    direction_optimizing = options.bfs_direction == "do"
    require(not direction_optimizing or algorithm == "bfs", "direction optimization requires BFS")
    expected_variant = "sorted-direction-optimizing-td-records-bu-bitmap" if direction_optimizing else specification["variant"]
    require(work.get("schema") == "ecg.algorithm-workload.v1" and
            work.get("algorithm") == algorithm and work.get("variant") == expected_variant and
            work.get("prediction_semantics") == (
                "consumer-cohort-potential-read" if frontier else
                "source-cohort-potential-read" if window else specification["semantics"]) and
            work.get("record_base_policy") == expected_base,
            "algorithm variant or prediction semantics mismatch")
    records = mode != "csr"
    reuse_scope = ({"sssp": "light-heavy", "cc": "sample-rounds", "bfs": "row-local", "bc": "row-local"}
                   .get(algorithm, "full-csr") if options.record_preprocess == "traversal" else "full-csr")
    if not records:
        reuse_scope = "none"
    if window:
        reuse_scope = "source-cohort-window"
    if frontier:
        reuse_scope = "consumer-cohort-mask"
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
    if records and not (window or frontier):
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
        # One edge stream per edge. BC inspects every edge its
        # forward pass examined, so its paired loads number between one and two per
        # inspection; the weight readers carry a compact copy of the weights.
        inspections = _integer(work, "record_inspections")
        require(_integer(work, "record_weight_bytes") ==
                (4 * carrier_count if graph.weighted and algorithm in ("sssp", "spmv") else 0) and
                _integer(work, "record_inspection_bytes") == inspections * layout["record_bytes"] and
                (inspections <= _integer(work, "actual_records") <= 2 * inspections
                 if algorithm == "bc" else inspections == 0),
                "record inspections or compact weights disagree with the kernel and its source")
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
    elif not records:
        require(_integer(work, "carrier_allocation_bytes") == 0, "CSR baseline built an ECG carrier")
    if not records or window or frontier:
        require(all(_integer(work, key) == 0 for key in (
                    "record_inspections", "record_inspection_bytes", "record_weight_bytes")),
                "a run without a NEXT carrier reported record inspections or weights")
    if window or frontier:
        require(algorithm == "bfs" and records and not graph.weighted and backend == "cache_sim" and
                not direction_optimizing and options.record_base_policy == "GRASP_PAPER" and
                options.record_preprocess == "csr" and options.window_observer == "off" and
                mode in ("transport", "replacement") and minimum_mantissa_bits == 0,
                "window runtime scope is not supported")
        require(_integer(work, "maximum_encoded_id") == graph.maximum_id,
                "window layout did not use the actual encoded VID bound")
        layout = window_layout(graph.maximum_id, requested_bytes)
        for key, value in layout.items():
            require(_integer(work, key) == value, f"window layout mismatch: {key}")
        prefix = "frontier" if frontier else "window"
        require(_integer(work, "carrier_allocation_bytes") == graph.records * layout["record_bytes"] and
                _integer(work, "construction_read_bytes") > 0 and _integer(work, "construction_write_bytes") > 0 and
                _integer(work, prefix + "_known_records") <= graph.records and
                _integer(work, prefix + "_token_bits") == 10 and
                _integer(work, prefix + "_known_records") + _integer(work, "constructed_unknown_records") == graph.records and
                _integer(work, "constructed_finite_records") == _integer(work, "constructed_wrap_records") == 0,
                "window carrier construction is not accounted")
        if frontier:
            require(options.grasp_scope == "graph-passes" and not options.source_list and options.repeat == 1 and
                    _integer(work, "frontier_known_records") == graph.records and
                    bool(re.fullmatch(r"[0-9a-f]{16}", str(work.get("frontier_carrier_digest", "")))),
                    "frontier source scope or immutable carrier receipt mismatch")
            validate_frontier_runtime(payload, options)
        else:
            validate_window_runtime(payload, options)
    if not window:
        require(payload.get("window_runtime") is None, "unrequested window runtime")
    if not frontier:
        require(payload.get("frontier_runtime") is None, "unrequested frontier runtime")
    if policy == "POPT_UNCHARGED" or reference:
        popt = payload.get("popt")
        require(backend == "cache_sim" and mode == "csr" and not direction_optimizing and
                isinstance(popt, dict) and popt.get("encoding") == "full" and
                popt.get("scope") == "graph-pass-irregular-regions" and
                popt.get("full_data_capacity") is True and
                popt.get("runtime_matrix_traffic_charged") is False,
                "P-OPT must declare its favorable full-capacity graph-pass scope")
        reused = popt.get("reused", False)
        require(type(reused) is bool and (not reused or allow_reused_popt),
                "unrequested P-OPT matrix reuse")
        lines = popt_matrix_lines(algorithm, graph.vertices)
        require(_integer(popt, "matrix_lines") == lines and
                _integer(popt, "matrix_bytes") == lines * 256 and _integer(popt, "epochs") == 256 and
                _integer(popt, "banks") == (3 if algorithm == "bc" else 1) and
                _integer(popt, "covered_regions") == (3 if algorithm == "bc" else 1) and
                _integer(popt, "passes") == passes and _integer(popt, "governed_reads") == actual and
                (_integer(popt, "construction_read_bytes") == 0 if reused else
                 _integer(popt, "construction_read_bytes") > 0) and
                (_integer(popt, "construction_write_bytes") == 0 if reused else
                 _integer(popt, "construction_write_bytes") > 0),
                "P-OPT matrix coverage, construction or progress mismatch")
        if "rank_mode" in popt or rank_constant or reference:
            require(popt.get("rank_mode") == ("constant" if rank_constant else "future") and popt.get("role") == (
                        "reference-consumer-diagnostic" if reference else
                        "policy-ablation" if rank_constant else "favorable-quality-control") and
                    popt.get("consumer", "POPT") == ("GRASP-reference" if reference else "POPT") and
                    _integer(popt, "constant_rank") == 0 and
                    _integer(popt, "constant_rank_lookups") == (
                        _integer(popt, "lookup_calls") if rank_constant else 0),
                    "P-OPT future-rank selection was not isolated or labeled")
            _integer(popt, "original_rank_sum")
            _integer(popt, "matrix_digest")
    else:
        require(payload.get("popt") is None, "unrequested graph-reference matrix")
    if reference:
        require(backend == "cache_sim" and algorithm == "spmv" and mode == "csr" and policy == "GRASP_PAPER" and
                not constant_ranks and options.grasp_scope == "all" and options.bfs_traffic_phases == "off" and
                options.record_preprocess == "csr" and options.record_model == "next" and not options.source_list and
                not observing and payload.get("setup_cache_policy") == "GRASP_PAPER",
                "GRASP reference diagnostic scope mismatch")
        validate_grasp_reference(payload, options)
    else:
        require(payload.get("grasp_reference") is None, "unrequested GRASP reference consumer")
    validate_victim_order(payload, options)
    require(options.record_store_bound == "drop" or (
                mode != "csr" and options.record_model == "next"),
            "store-bound retention requires the current NEXT record model")
    require(options.record_governed_first == "no" or (
                mode != "csr" and options.record_model == "next"),
            "governed-first requires the current NEXT record replacement rule")
    require(options.record_rrpv_order == "no" or (
                mode != "csr" and options.record_model == "next"),
            "record RRPV order requires the current NEXT record replacement rule")
    require((options.record_uninformed_base == "no" and options.record_bound_compare == "on" and
             options.record_carrier_first == "no") or (
                mode == "replacement" and options.record_model == "next"),
            "record victim controls require the current NEXT record replacement rule without prefetch")
    expected_result = contract()["references"][algorithm]
    if evidence and graph_path.name == expected_result["graph"] and options.source == 0 and not options.source_list:
        require(graph.sha256 == contract()["graphs"][expected_result["graph"]]["sha256"],
                "reference fixture name has changed contents")
        for key, expected in expected_result.items():
            if key != "graph":
                require(work.get(key) == expected, f"independent {algorithm} reference failed: {key}")
    return work


def validate_victim_order(payload: dict[str, Any], options: argparse.Namespace) -> None:
    expected = ("carrier-first" if options.record_carrier_first == "on" else
                "governed-first" if options.record_governed_first == "on" else "base-first")
    require(payload["workload"].get("record_victim_order") == expected,
            "record victim order does not match the requested arm")
    require(payload["workload"].get("record_store_bound") == options.record_store_bound,
            "record store-bound retention does not match the requested arm")
    require(payload["workload"].get("record_expiry_clock") == options.record_expiry_clock,
            "record expiry clock does not match the requested arm")
    require(payload["workload"].get("record_pressure_gate") == options.record_pressure_gate,
            "record pressure gate does not match the requested arm")
    require(payload["workload"].get("record_rrpv_order") == options.record_rrpv_order,
            "record RRPV order does not match the requested arm")
    require(payload["workload"].get("record_uninformed_base") == options.record_uninformed_base,
            "record uninformed fallback does not match the requested arm")
    require(payload["workload"].get("record_bound_compare") == options.record_bound_compare,
            "record bound comparison does not match the requested arm")
    # Wherever the cache reports its victim paths they close to its decisions and
    # agree with the controls; a requested control needs that report.
    metrics = payload.get("metrics")
    requested = (options.record_uninformed_base != "no" or options.record_bound_compare != "on" or
                 options.record_carrier_first != "no")
    if requested or isinstance(metrics, dict) and "ecg_record_victim_decisions" in metrics:
        require(isinstance(metrics, dict), "record victim controls need the cache's victim report")
        paths = ("dead_first", "unpressured", "ungoverned_first", "base_not_governed", "base_no_future",
                 "base_kept", "overridden", "uninformed_base", "no_bound_compare", "carrier_first")
        counts = {path: _integer(metrics, "ecg_record_victim_" + path) for path in paths}
        require(sum(counts.values()) == _integer(metrics, "ecg_record_victim_decisions"),
                "record victim paths do not close to the decisions")
        require(_integer(metrics, "ecg_record_uninformed_base") == int(options.record_uninformed_base == "on") and
                _integer(metrics, "ecg_record_bound_compare") == int(options.record_bound_compare == "on") and
                _integer(metrics, "ecg_record_carrier_first") == int(options.record_carrier_first == "on"),
                "effective record victim controls do not match the requested arm")
        # Each precedence path belongs to its own victim order.
        require((options.record_uninformed_base == "on" or counts["uninformed_base"] == 0) and
                (options.record_bound_compare == "no" or counts["no_bound_compare"] == 0) and
                (options.record_bound_compare == "on" or counts["base_kept"] + counts["overridden"] == 0) and
                (options.record_carrier_first == "on" or counts["carrier_first"] == 0) and
                (options.record_governed_first == "on" or counts["ungoverned_first"] == 0),
                "record victim paths contradict the requested controls")


def validate_grasp_reference(payload: dict[str, Any], options: argparse.Namespace) -> dict[str, Any]:
    reference = payload.get("grasp_reference")
    # The rank-first arm orders covered candidates by rank and ages only the
    # maximum-rank tie set; the base-first arms refine one GRASP victim by a
    # strictly farther rank. The receipt must describe the arm that ran.
    rank_first = options.grasp_reference == "rank"
    require(isinstance(reference, dict) and reference.get("schema") == "ecg.grasp-reference.v1" and
            reference.get("mode") == options.grasp_reference and
            reference.get("availability") == "ideal-matrix-at-victim-selection" and
            reference.get("base_policy") == reference.get("outside_pass_policy") == "GRASP_PAPER" and
            reference.get("candidate_rrpv_filter") is False and
            reference.get("strictly_farther_only") is (not rank_first) and
            reference.get("rank_first") is rank_first,
            "GRASP reference consumer mechanics or label mismatch")
    require(_integer(reference, "lower_rrpv_overrides") <= _integer(reference, "victim_overrides") <=
            _integer(reference, "covered_base_victims") <= _integer(reference, "victim_decisions") and
            _integer(payload["popt"], "lookup_calls") >= reference["covered_base_victims"] and
            (options.grasp_reference != "flat" or reference["victim_overrides"] == 0),
            "GRASP reference victim or lookup accounting mismatch")
    ties, divergence = (_integer(reference, "max_rank_tie_population"),
                        _integer(reference, "basefirst_divergence"))
    require((ties >= reference["covered_base_victims"] and divergence <= reference["covered_base_victims"])
            if rank_first else ties == divergence == 0,
            "GRASP reference rank-first tie or divergence accounting mismatch")
    return reference


def validate_bfs_phases(payload: dict[str, Any], options: argparse.Namespace) -> None:
    work = payload["workload"]
    control = payload.get("grasp_phase_control")
    if options.grasp_scope == "graph-passes" and options.record_model in ("window", "frontier"):
        runtime = payload.get(options.record_model + "_runtime")
        require(control is None and isinstance(runtime, dict) and
                runtime.get("grasp_scope") == "graph-passes" and
                runtime.get("phase_control_accounting") == f"shared-{options.record_model}-markers",
                "window phase control must use its existing paid markers exactly once")
    elif options.grasp_scope == "graph-passes":
        require(payload["backend"] == "cache_sim" and payload["policy"] == "GRASP_PAPER" and
                work["algorithm"] == "bfs" and work["carrier"] == "csr" and work["bfs_direction"] == "td" and
                isinstance(control, dict) and control.get("schema") == "ecg.grasp-phase-control.v1" and
                control.get("enabled") is True and control.get("setup_policy") == "GRASP_PAPER" and
                control.get("cache_reset") is False and control.get("rrpv_history_maintained") is True and
                control.get("cost_unit") == "functional-steps-not-CPU-cycles" and
                _integer(control, "passes") == work["passes"] and
                _integer(control, "transitions") == 2 * work["passes"] and
                _integer(control, "functional_steps") == 16 * (control["transitions"] + 1) and
                _integer(control, "control_bytes") == 48 * (control["transitions"] + 1),
                "GRASP phase transitions or state preservation are unaccounted")
    else:
        require(control is None, "unrequested GRASP phase control")
    attribution = payload.get("bfs_traffic_phases")
    if options.bfs_traffic_phases == "off":
        require(attribution is None, "unrequested BFS traffic attribution")
        return
    require(payload["backend"] == "cache_sim" and work["algorithm"] == "bfs" and work["carrier"] == "csr" and
            work["bfs_direction"] == "td" and isinstance(attribution, dict) and
            attribution.get("schema") == "ecg.bfs-traffic-phases.v1" and
            attribution.get("writeback_attribution") == "triggering-access-not-victim-owner" and
            attribution.get("cache_state_preserved") is True,
            "BFS attribution scope or writeback interpretation mismatch")
    phases = attribution.get("phases")
    names = ("setup", "edge-probe", "frontier-build", "frontier-sort", "between-passes")
    roles = {"csr-index", "csr-edge", "weight", "depth", "frontier-work", "construction"}
    require(isinstance(phases, list) and tuple(p.get("phase") for p in phases) == names,
            "BFS phase roster mismatch")
    keys = ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks",
            "llc_hits", "llc_misses", "llc_property_hits", "llc_property_misses", "total_offchip_traffic")
    for phase in phases:
        require(isinstance(phase.get("roles"), dict) and set(phase["roles"]) == roles and
                isinstance(phase.get("total"), dict), "BFS role roster mismatch")
        for key in keys:
            require(sum(_integer(row, key) for row in phase["roles"].values()) == _integer(phase["total"], key),
                    f"BFS role counters do not close: {phase['phase']}/{key}")
    for key in keys:
        require(phases[0]["total"][key] == payload["traffic_phases"]["setup"][key] and
                sum(phase["total"][key] for phase in phases[1:]) == payload["traffic_phases"]["kernel"][key],
                f"BFS setup/kernel phase counters do not close: {key}")
    by_phase = {phase["phase"]: phase for phase in phases}
    require(by_phase["frontier-build"]["roles"]["frontier-work"]["total_accesses"] == work["reached"] - 1 and
            all(by_phase["frontier-sort"]["roles"][role]["total_accesses"] == 0 for role in roles - {"frontier-work"}) and
            by_phase["between-passes"]["total"]["total_accesses"] == 0,
            "BFS construction/sorting attribution is inconsistent with current work")


def validate_window_runtime(payload: dict[str, Any], options: argparse.Namespace) -> dict[str, Any]:
    return validate_potential_runtime(payload, options, False)


def validate_frontier_runtime(payload: dict[str, Any], options: argparse.Namespace) -> dict[str, Any]:
    return validate_potential_runtime(payload, options, True)


def validate_potential_runtime(payload: dict[str, Any], options: argparse.Namespace, frontier: bool) -> dict[str, Any]:
    model = "frontier" if frontier else "window"
    w, work = payload.get(model + "_runtime"), payload["workload"]
    require(isinstance(w, dict) and w.get("schema") == f"ecg.{model}-runtime.v1" and
            w.get("record_model") == ("consumer-cohort-mask-u32" if frontier else "potential-window-u32") and
            w.get("cost_unit") == "functional-steps-not-CPU-cycles" and
            w.get("replacement") is (payload["mode"] == "replacement"),
            "window runtime model or evidence boundary mismatch")
    require(w.get("grasp_scope", "all") == options.grasp_scope and
            (options.grasp_scope == "all" or w.get("phase_control_accounting") == f"shared-{model}-markers"),
            "window outside-pass policy or control accounting mismatch")
    floor = 7 if frontier else options.window_candidate_rrpv
    require(_integer(w, "candidate_floor") == floor and
            (frontier or floor == _integer(work, "window_candidate_rrpv")) and
            _integer(w, "record_bytes") == work["record_bytes"] and _integer(w, "token_bits") == 10,
            "window runtime layout or victim-rule mismatch")
    minimum = max(8, (work["vertices"] + 255) // 256)
    cohort = 1 << (minimum - 1).bit_length()
    bin_rows = cohort if frontier else cohort // 8
    require(_integer(w, "cohort_rows") == cohort and _integer(w, "bin_rows") == bin_rows and
            _integer(w, "bins_per_pass") == (work["vertices"] + bin_rows - 1) // bin_rows,
            "window runtime cohort rule changed")
    require(_integer(w, "record_loads") == _integer(w, "property_reads") == work["actual_records"] and
            _integer(w, "record_read_bytes") == work["actual_records"] * work["record_bytes"] and
            _integer(w, "passes") == work["passes"] and
            _integer(w, "structural_positions") == work["structural_positions"] and
            _integer(w, "skipped_positions") == work["skipped_positions"] and
            _integer(w, "row_context_steps") == _integer(w, "source_rows") == work["reached"] and
            _integer(w, "forwarded_stores") == _integer(w, "association_steps") == work["reached"] - 1,
            "window runtime did not observe complete real BFS work")
    require(_integer(w, "enqueued") == _integer(w, "delivered") ==
            w["property_reads"] + w["forwarded_stores"] + _integer(w, "ordinary_invalidations") and
            w["delivered"] == sum(_integer(w, key) for key in ("applied", "stale", "absent", "expired")) and
            _integer(w, "pending") == 0 and _integer(w, "queue_capacity") == 16 and
            _integer(w, "queue_peak") <= 16 and _integer(w, "latency_steps") == 8,
            "window publication queue did not close")
    local_steps = 0
    if frontier:
        updates = 2 * work["reached"]
        require(w.get("gating") == options.frontier_gating and options.grasp_scope == "graph-passes" and
                _integer(w, "counter_storage_bytes") == 2048 and _integer(w, "llc_bitmap_bytes") == 32 and
                _integer(w, "source_staging_bytes") == 32 and _integer(w, "llc_context_budget_bytes") == 64 and
                2048 <= _integer(w, "source_count_object_bytes") <= _integer(w, "controller_object_bytes") and
                _integer(w, "counter_read_bytes") == 4 * (updates + w["passes"] * w["bins_per_pass"]) and
                _integer(w, "counter_write_bytes") == 2048 + 4 * updates and
                _integer(w, "counter_arithmetic_steps") == updates and
                _integer(w, "counter_scan_steps") == w["passes"] * w["bins_per_pass"] and
                _integer(w, "counter_initialization_memory_steps") == 514 and
                _integer(w, "frontier_appends") == work["reached"] - 1 and
                _integer(w, "frontier_swaps") == w["passes"] and
                w["passes"] <= _integer(w, "frontier_clears") <= w["passes"] * w["bins_per_pass"] and
                _integer(w, "markers") >= 2 * w["passes"] + w["frontier_clears"] and
                _integer(w, "remaining_current") == _integer(w, "remaining_next") == 0,
                "frontier causal counts, complete source costs or gating scope mismatch")
        local_steps = w["counter_arithmetic_steps"] + w["counter_scan_steps"] + w["frontier_swaps"]
    require(_integer(w, "configuration_steps") == 16 and
            _integer(w, "marker_steps") == 16 * _integer(w, "markers") + (8 * w["passes"] if frontier else 0) and
            _integer(w, "control_bytes") == 48 * (w["markers"] + 1) + (32 * w["passes"] if frontier else 0) and
            2 * w["passes"] <= w["markers"] <= ((2 if frontier else 1) * w["bins_per_pass"] + 2) * w["passes"] and
            _integer(w, "steps") == sum(_integer(w, key) for key in (
                "memory_steps", "row_context_steps", "association_steps", "observation_steps",
                "observation_wait_steps", "configuration_steps", "marker_steps", "drain_steps")) + local_steps and
            w["memory_steps"] == payload["traffic_phases"]["kernel"]["total_accesses"] + (514 if frontier else 0),
            "window control/lookup work is missing or double-counted")
    require(_integer(w, "metadata_payload_bits_per_line") == 67 and
            _integer(w, "unmodeled_runtime_table_bytes") == 0 and
            0 < _integer(w, "controller_object_bytes") <= 4096 - (96 if frontier else 0) and
            _integer(w, "protected_overrides") <= _integer(w, "victim_overrides") <=
            _integer(w, "live_base_victims") <= _integer(w, "victim_decisions") and
            (floor == 6 or w["protected_overrides"] == 0) and
            (payload["mode"] == "replacement" or w["victim_overrides"] == 0),
            "window state or candidate budget is not respected")
    return w


def validate_window_observer(payload: dict[str, Any], options: argparse.Namespace) -> dict[str, Any]:
    observer = payload.get("window_observer")
    if options.window_observer == "off":
        require(observer is None, "unrequested window observer")
        return {}
    work = payload["workload"]
    require(work["algorithm"] == "bfs" and work["carrier"] == "csr" and work["bfs_direction"] == "td" and
            payload["policy"] == "GRASP_PAPER" and payload["backend"] == "cache_sim" and
            isinstance(observer, dict) and observer.get("schema") in (
                "ecg.window-eviction-observer.v1", "ecg.window-eviction-observer.v2",
                "ecg.window-eviction-observer.v3", "ecg.window-eviction-observer.v4") and
            observer.get("mode") == options.window_observer and observer.get("active_policy_changed") is False and
            observer.get("diagnostic_costs_in_cache_counters") is False and
            observer.get("delivery_model") == "uncoalesced-eight-access-steps-serialized-markers" and
            _integer(payload["metrics"], "prefetch_fills") == 0,
            "window observer changed policy or omitted its diagnostic limitations")
    protected_probe = observer["schema"] == "ecg.window-eviction-observer.v4"
    paired = protected_probe or observer["schema"] == "ecg.window-eviction-observer.v3"
    extended = paired or observer["schema"] == "ecg.window-eviction-observer.v2"
    views = ("immediate", "delivered", "preserved_immediate", "preserved_delivered") if extended else (
        "immediate", "delivered")
    if paired:
        views += ("forwarded_delivered",)
    attribution = ("live_base_without_alternative", "live_base_equal_ranks", "live_base_already_worst",
                   "only_ineligible_worse", "live_current_cohort", "live_saturated_strength",
                   "live_base_no_eligible_depth", "live_base_eligible_without_live_hint")
    if extended:
        require(observer.get("write_survival_rule") == "published-only-pending-cancelled" and
                observer.get("read_pair_window_scope") == "sampled-endpoints-not-live-state" and
                _integer(observer, "trial_views") == (4 if protected_probe else 3 if paired else 2) and
                sum(_integer(observer, key) for key in (
                    "writes_kept_live", "writes_cancelling_pending", "writes_without_live")) ==
                _integer(observer, "ordinary_writes"),
                "window observer write-survival contract or accounting mismatch")
    if paired:
        require(observer.get("publication_rule") == "checked-read-store-new-event" and
                observer.get("association_rule") == "exact-index-element-binding-source-pass-adjacent-access" and
                _integer(observer, "association_requests") == _integer(observer, "ordinary_writes") ==
                _integer(observer, "association_accepted") == _integer(observer, "forwarded_store_updates") and
                _integer(observer, "association_rejected") == 0 and
                sum(_integer(observer, "forwarded_" + key) for key in ("applied", "stale", "absent")) ==
                observer["forwarded_store_updates"] and
                _integer(observer, "forwarded_known_updates") <= observer["forwarded_store_updates"] and
                _integer(observer, "forwarded_known_applied") <= min(observer["forwarded_known_updates"],
                                                                   observer["forwarded_applied"]) and
                0 < _integer(observer, "association_slot_bytes") <= 128,
                "window observer paired-store identity or publication accounting mismatch")
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
    for key in views:
        values = observer.get(key)
        require(isinstance(values, dict) and isinstance(values.get("base_states"), list) and
                len(values["base_states"]) == 8 and all(type(v) is int and v >= 0 for v in values["base_states"]) and
                sum(values["base_states"]) == _integer(values, "samples") ==
                (observer["pass_evictions"] // 256 if windows else 0) and
                _integer(values, "hypothetical_overrides") <= values["base_states"][6] and
                _integer(values, "live_eligible_candidates") <= _integer(values, "eligible_candidates"),
                "window observer sample accounting mismatch")
        if extended:
            require(sum(_integer(values, field) for field in attribution[:3]) +
                    values["hypothetical_overrides"] == values["base_states"][6] and
                    _integer(values, "only_ineligible_worse") <= values["base_states"][6] - values["hypothetical_overrides"] and
                    _integer(values, "live_current_cohort") <= values["live_eligible_candidates"] and
                    _integer(values, "live_saturated_strength") <= values["live_eligible_candidates"] and
                    _integer(values, "live_base_no_eligible_depth") +
                    _integer(values, "live_base_eligible_without_live_hint") == values["live_base_without_alternative"],
                    "window observer candidate attribution does not partition live base victims")
    protected_fields = ("samples", "live_base", "tie_only_choices", "expanded_choices",
                        "protected_selected", "additional_choices", "retargeted_choices",
                        "extra_immediate_writeback", "avoided_immediate_writeback", "same_dirty_state",
                        "farther_window", "equal_distance_weaker")
    if protected_probe:
        probe = observer.get("protected_probe")
        forwarded = observer["forwarded_delivered"]
        require(isinstance(probe, dict) and probe.get("rule") == "one-step-RRPV6-strictly-worse" and
                probe.get("reference") == "actual-GRASP-victim" and probe.get("view") == "forwarded_delivered" and
                _integer(probe, "candidate_floor") == 6 and _integer(probe, "samples") == forwarded["samples"] and
                _integer(probe, "live_base") == forwarded["base_states"][6] and
                _integer(probe, "tie_only_choices") == forwarded["hypothetical_overrides"],
                "protected probe changed its candidate or reference contract")
        require(_integer(probe, "expanded_choices") == probe["tie_only_choices"] +
                _integer(probe, "additional_choices") <= probe["live_base"] and
                _integer(probe, "protected_selected") == probe["additional_choices"] +
                _integer(probe, "retargeted_choices") and probe["retargeted_choices"] <= probe["tie_only_choices"] and
                sum(_integer(probe, field) for field in protected_fields[7:10]) == probe["protected_selected"] and
                _integer(probe, "farther_window") + _integer(probe, "equal_distance_weaker") == probe["protected_selected"],
                "protected probe double-counts choices or dirty-line tradeoffs")
        histogram = probe.get("worse_available_by_rrpv")
        require(isinstance(histogram, list) and len(histogram) == 7 and
                all(type(value) is int and 0 <= value <= probe["live_base"] for value in histogram) and
                probe["protected_selected"] <= histogram[6],
                "protected probe selected a deeper-protected or unavailable candidate")
    trial_views = (("trials", "delivered"), ("preserved_trials", "preserved_delivered")) if extended else (
        ("trials", "delivered"),)
    if paired:
        trial_views += (("forwarded_trials", "forwarded_delivered"),)
    if protected_probe:
        trial_views += (("protected_trials", "protected_probe"),)
    for trial_key, view in trial_views:
        trials = observer.get(trial_key)
        require(isinstance(trials, dict) and _integer(trials, "started") + _integer(trials, "capacity_dropped") ==
                observer[view]["protected_selected" if view == "protected_probe" else "hypothetical_overrides"] and _integer(trials, "started") ==
                sum(_integer(trials, key) for key in ("base_first", "alternative_first", "censored_horizon", "censored_pass")) and
                _integer(trials, "peak_pending") <= 256,
                "window observer outcomes or censoring do not close")
        for side in ("base", "alternative"):
            require(_integer(trials, side + "_first_memory") <= _integer(trials, side + "_first_llc") <=
                    _integer(trials, side + "_first"), "window observer confuses private reuse with LLC demand")
            if extended:
                require(_integer(trials, side + "_first_before_both_endpoints") <=
                        _integer(trials, side + "_first_before_endpoint") <= trials[side + "_first"],
                        "window observer overcounts reads within sampled endpoints")
    per_pass = observer.get("per_pass")
    require(isinstance(per_pass, list) and len(per_pass) == work["passes"] <= 128 and
            sum(_integer(row, "designated_reads") for row in per_pass) == work["actual_records"] and
            sum(_integer(row, "evictions") for row in per_pass) == observer["pass_evictions"],
            "window observer pass accounting mismatch")
    for key in views:
        for field in ("samples", "eligible_candidates", "live_eligible_candidates", "hypothetical_overrides") + (
                attribution if extended else ()):
            require(all(isinstance(row.get(key), dict) for row in per_pass) and
                    sum(_integer(row[key], field) for row in per_pass) == observer[key][field],
                    "window observer per-pass samples do not sum to totals")
    if protected_probe:
        require(all(isinstance(row.get("protected_probe"), dict) for row in per_pass),
                "missing per-pass protected attribution")
        for field in protected_fields:
            require(sum(_integer(row["protected_probe"], field) for row in per_pass) == observer["protected_probe"][field],
                    "per-pass protected choices do not sum to totals")
        require(all(isinstance(row["protected_probe"].get("worse_available_by_rrpv"), list) and
                    len(row["protected_probe"]["worse_available_by_rrpv"]) == 7 and
                    all(type(value) is int and 0 <= value <= row["protected_probe"]["live_base"]
                        for value in row["protected_probe"]["worse_available_by_rrpv"]) for row in per_pass),
                "missing per-pass protected-rank histogram")
        for rank in range(7):
            require(sum(row["protected_probe"]["worse_available_by_rrpv"][rank] for row in per_pass) ==
                    observer["protected_probe"]["worse_available_by_rrpv"][rank],
                    "per-pass protected ranks do not sum to totals")
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


# The frozen ecg.grasp-declaration.v1 declarations (NEXT.md §bv): each kernel's phases as (array, role, percent
# of the last level). BC repeats its forward and backward phases for every source.
GRASP_DECLARATION_CONTRACT = "ecg.grasp-declaration.v1"
GRASP_DECLARATIONS = {
    "spmv": (("x", "A", 100),),
    "bfs": (("depth", "A", 100),),
    "sssp": (("distances", "A", 100),),
    "pr": (("contribution", "A", 50),),
    "bc": (("path_counts", "A", 50), ("depth", "B", 50), ("dependency", "A", 50), ("depth", "B", 50)),
}
GRASP_DECLARED_KERNELS = frozenset(GRASP_DECLARATIONS)


def expected_grasp_declarations(algorithm: str, sources: int = 1) -> list[tuple[str, str, int, int]]:
    """The declarations a receipt must carry, with the property selections made before each."""
    if algorithm != "bc":
        return [(array, role, percent, 0) for array, role, percent in GRASP_DECLARATIONS.get(algorithm, ())]
    return [(array, role, percent, 2 * source + (index >= 2))
            for source in range(sources) for index, (array, role, percent) in enumerate(GRASP_DECLARATIONS["bc"])]


def validate_fair_comparison(payload: dict[str, Any], algorithm: str, registration: str, entry: str,
                             record_base: str | None = None, sources: int = 1) -> None:
    """NEXT.md §bv C1-C3: a receipt attests the registration, the kernel entry and, for a PageRank record row,
    the base policy it ran under, and conforms to each."""
    metrics = payload.get("metrics") if "metrics" in payload else payload
    require(isinstance(metrics, dict), "missing total traffic counters")
    if "metrics" in payload:
        require(payload.get("grasp_registration", "all") == registration, "GRASP registration receipt mismatch")
        require(payload.get("kernel_entry", "as-built") == entry, "kernel-entry receipt mismatch")
    require(metrics.get("kernel_entry", "as-built") == entry, "kernel-entry receipt mismatch")
    captured = metrics.get("property_registration")
    if registration == "declared":
        require(isinstance(captured, dict) and captured.get("grasp_registration") == GRASP_DECLARATION_CONTRACT,
                "declared GRASP registration has no declaration receipt")
        require(captured.get("boundary_mode") == "capacity", "declared GRASP registration must tier by capacity")
        declarations = captured.get("grasp_declarations")
        require(isinstance(declarations, list) and all(isinstance(item, dict) for item in declarations),
                "invalid GRASP declaration receipt")
        declared = [(item.get("region"), item.get("role"), item.get("percent"), item.get("selections"))
                    for item in declarations]
        require(declared == expected_grasp_declarations(algorithm, sources),
                f"GRASP declarations differ from {GRASP_DECLARATION_CONTRACT}")
        phase: set[str] = set()
        for array, role, _, _ in declared:
            phase = {array} if role == "A" else phase | {array}
        regions = captured.get("property_regions")
        require(isinstance(regions, list) and regions and all(isinstance(region, dict) for region in regions),
                "missing property region receipt")
        require({region.get("name") for region in regions if region.get("grasp") is True} == phase,
                "GRASP designations differ from the last declared phase")
    elif captured is not None:
        require(isinstance(captured, dict) and captured.get("grasp_registration") == "all" and
                not captured.get("grasp_declarations"), "historical GRASP registration receipt mismatch")
    if isinstance(captured, dict) and captured.get("property_regions"):
        if "traffic_phases" in payload:
            kernel = payload["traffic_phases"].get("kernel", {})
            hits, misses = kernel.get("llc_property_hits"), kernel.get("llc_property_misses")
        else:
            llc = metrics.get("L3") if isinstance(metrics.get("L3"), dict) else {}
            hits, misses = llc.get("prop_hits"), llc.get("prop_misses")
        regions = captured["property_regions"]
        require(sum(region.get("kernel_hits", 0) for region in regions) == hits and
                sum(region.get("kernel_misses", 0) for region in regions) == misses,
                "per-region counts do not close on the kernel's property traffic")
    if entry == "cold":
        maintenance = metrics.get("kernel_entry_maintenance_writebacks")
        require(type(maintenance) is int and maintenance >= 0, "cold kernel entry has no maintenance receipt")
        require(metrics.get("kernel_entry_cold_boundaries") == 1, "a cold kernel entry crosses exactly one boundary")
        require(metrics.get("kernel_entry_residual_lines") == [0, 0, 0], "a cold boundary leaves no line at any level")
        census = metrics.get("kernel_census")
        require(isinstance(census, dict) and census.get("entry_valid_lines") == 0 and
                census.get("entry_dirty_lines") == 0, "the kernel census must find an empty last level")
        if "traffic_phases" in payload:
            require(payload["traffic_phases"].get("setup", {}).get("llc_writebacks", -1) >= maintenance,
                    "the boundary's write-backs must be setup's")
    else:
        require(metrics.get("kernel_entry_cold_boundaries", 0) == 0, "an as-built kernel entry crosses no boundary")
    if record_base is not None:
        require(metrics.get("record_base_policy") == record_base, "PageRank record base policy receipt mismatch")


def validate_traffic_phases(payload: dict[str, Any]) -> dict[str, int]:
    phases = payload.get("traffic_phases")
    cold = payload.get("kernel_entry", "as-built") == "cold"
    require(isinstance(phases, dict) and phases.get("boundary") == "first-binding-complete" and
            phases.get("cache_state_preserved") is (not cold), "missing nonintrusive setup/kernel traffic boundary"
            if not cold else "a cold kernel entry must report a boundary that does not preserve the cache")
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
        batch = options.queries > 1
        require(not batch or args.benchmark == "spmv" and mode == "csr" and
                options.record_model == "next" and options.record_preprocess == "csr" and
                options.grasp_reference == "off" and options.popt_rank_mode == "future" and
                options.grasp_scope == "all" and options.bfs_traffic_phases == "off" and
                options.window_observer == "off" and options.bfs_direction == "td" and not options.source_list,
                "independent queries require unmodified CSR SpMV baselines")
        reference = options.grasp_reference != "off"
        require(not reference or args.benchmark == "spmv" and mode == "csr" and policy == "GRASP_PAPER" and
                options.popt_rank_mode == "future" and options.record_preprocess == "csr" and
                options.record_model == "next" and options.grasp_scope == "all" and
                options.bfs_traffic_phases == "off" and options.window_observer == "off" and
                options.bfs_direction == "td" and not options.source_list,
                "GRASP reference diagnostic requires CSR SpMV with ordinary GRASP_PAPER")
        require(options.popt_rank_mode == "future" or policy == "POPT_UNCHARGED" and
                mode == "csr" and args.benchmark in ("spmv", "bfs") and options.bfs_direction == "td",
                "constant P-OPT ranks require CSR SpMV or TD BFS with POPT_UNCHARGED")
        phase_modes = options.bfs_traffic_phases == "on" or options.grasp_scope != "all"
        potential = options.record_model in ("window", "frontier")
        require(not phase_modes or args.benchmark == "bfs" and
                (mode == "csr" or potential and options.bfs_traffic_phases == "off") and
                options.bfs_direction == "td" and options.window_observer == "off" and
                (options.grasp_scope == "all" or policy == "GRASP_PAPER" or
                 potential and options.record_base_policy == "GRASP_PAPER"),
                "BFS phase controls require TD BFS and a compatible baseline/model")
        require(not potential or args.benchmark == "bfs" and mode in (
            "transport", "replacement") and options.record_base_policy == "GRASP_PAPER" and
            options.window_observer == "off" and options.bfs_direction == "td" and options.record_preprocess == "csr",
            "window model requires TD BFS records, GRASP base and T/R only")
        require(options.record_model != "frontier" or options.grasp_scope == "graph-passes" and
                not options.source_list and options.repeat == 1,
                "frontier model requires single-source TD BFS and graph-pass GRASP")
        observing = options.window_observer != "off"
        require(not observing or args.benchmark == "bfs" and mode == "csr" and policy == "GRASP_PAPER" and
                options.bfs_direction == "td" and options.record_preprocess == "csr",
                "window observer requires CSR TD BFS and unchanged GRASP_PAPER")
        require(mode != "csr" or options.record_base_policy == "LRU",
                "record base policy requires a current record mode")
        row["record_base_policy"] = options.record_base_policy
        row["grasp_scope"] = options.grasp_scope
        row["policy_ablation"] = "1" if options.popt_rank_mode == "constant" or options.grasp_reference == "flat" or (
            options.record_model == "frontier" and options.frontier_gating == "ignored") else "0"
        row["frontier_gating"] = options.frontier_gating
        row["query_count"] = options.queries
        row["policy_label"] = policy_labels([spec], options.record_base_policy, options.window_observer,
            options.record_model, options.window_candidate_rrpv, options.grasp_scope, options.popt_rank_mode,
            options.frontier_gating, options.grasp_reference, options.queries,
            options.record_governed_first, options.record_store_bound,
            options.record_expiry_clock, options.record_pressure_gate,
            options.record_rrpv_order, uninformed_base=options.record_uninformed_base,
            bound_compare=options.record_bound_compare, carrier_first=options.record_carrier_first,
            grasp_registration=options.grasp_registration, kernel_entry=options.kernel_entry)[0]
        if reference:
            row.update(diagnostic_only="1", measurement_scope="ideal-availability-reference-consumer")
        if observing:
            row.update(diagnostic_only="1", measurement_scope="observation-only-unchanged-grasp")
        graph = graph_info(options.graph, allow_weighted=True, traversal="out")
        require(not batch or not graph.weighted, "independent queries require unweighted SpMV")
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
            preprocessing=options.record_preprocess, popt_full_capacity=policy == "POPT_UNCHARGED" or reference,
            record_model=options.record_model, queries=options.queries)
        if observing:
            require(not graph.weighted and plan["array_bytes"] + options.window_observer_bytes <=
                    args.algorithm_workspace_bytes, "window observer exceeds its workspace or target scope")
        binary = ROOT / "bench/bin_sim/algorithms"
        base_suffix = "" if options.record_base_policy == "LRU" else (
            "_BASE_" + options.record_base_policy)
        observer_suffix = "" if not observing else "_OBS_" + options.window_observer.upper()
        model_suffix = "" if options.record_model == "next" else f"_MODEL_WINDOW_RRPV{options.window_candidate_rrpv}"
        if options.record_model == "frontier":
            model_suffix = f"_MODEL_FRONTIER_RRPV7_GATING_{options.frontier_gating.upper()}"
        phase_suffix = "" if options.grasp_scope == "all" else "_GRAPH_PASSES"
        phase_suffix += "" if options.grasp_registration == "all" else "_GRASP_DECLARED"
        phase_suffix += "" if options.kernel_entry == "as-built" else "_COLD_ENTRY"
        rank_suffix = "" if options.popt_rank_mode == "future" else "_CONST_RANK"
        label = (f"cache_sim_{args.benchmark}_{spec.safe_label}{base_suffix}{model_suffix}{observer_suffix}{phase_suffix}"
                 f"{rank_suffix}_L3{parse_size_bytes(l3_size)}")
        if reference:
            label = f"cache_sim_{args.benchmark}_{row['policy_label']}_L3{parse_size_bytes(l3_size)}"
        if batch:
            label += f"_QUERIES{options.queries}"
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
        if batch:
            command.extend(("--queries", str(options.queries)))
        if policy == "POPT_UNCHARGED":
            command.extend(("--popt-rank-mode", options.popt_rank_mode))
        if reference:
            command.extend(("--grasp-reference", options.grasp_reference))
        # Opt-in record victim arms. The kernel defaults match the runner
        # defaults, so these are emitted only when actually requested.
        if options.record_governed_first != "no":
            command.extend(("--record-governed-first", options.record_governed_first))
        if options.record_pressure_gate != "no":
            command.extend(("--record-pressure-gate", options.record_pressure_gate))
        if options.record_rrpv_order != "no":
            command.extend(("--record-rrpv-order", options.record_rrpv_order))
        if options.record_uninformed_base != "no":
            command.extend(("--record-uninformed-base", options.record_uninformed_base))
        if options.record_bound_compare != "on":
            command.extend(("--record-bound-compare", options.record_bound_compare))
        if options.record_carrier_first != "no":
            command.extend(("--record-carrier-first", options.record_carrier_first))
        if options.record_store_bound != "drop":
            command.extend(("--record-store-bound", options.record_store_bound))
        if options.record_expiry_clock != "progress":
            command.extend(("--record-expiry-clock", options.record_expiry_clock))
        if phase_modes:
            command.extend(("--grasp-scope", options.grasp_scope, "--bfs-traffic-phases", options.bfs_traffic_phases))
        if options.grasp_registration != "all":
            command.extend(("--grasp-registration", options.grasp_registration))
        if options.kernel_entry != "as-built":
            command.extend(("--kernel-entry", options.kernel_entry))
        if options.record_model == "window":
            command.extend(("--record-model", "window", "--window-candidate-rrpv", str(options.window_candidate_rrpv)))
        if options.record_model == "frontier":
            command.extend(("--record-model", "frontier", "--frontier-gating", options.frontier_gating))
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
        if batch:
            if __package__:
                from .spmv_queries import validate_batch
            else:
                from spmv_queries import validate_batch
            row.update(validate_batch(payload, log_path.read_text(), graph=graph, options=options,
                policy=policy, evidence=args.ecg_equivalence, llc_bytes=parse_size_bytes(l3_size),
                llc_ways=int(args.l3_ways)))
            row.update(status="ok", json_path=str(data_path), log_path=str(log_path),
                graph_sha256=graph.sha256, benchmark_binary_sha256=before,
                algorithm_workload_verified="1", planned_host_bytes=plan["planned_host_bytes"],
                resource_scope=plan["memory_plan"])
            return [row]
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
        if policy == "POPT_UNCHARGED" or reference:
            require(_integer(metrics["L3"], "size_bytes") == parse_size_bytes(l3_size) and
                    _integer(metrics["L3"], "ways") == int(args.l3_ways),
                    "P-OPT full-capacity control lost data capacity")
            row.update({"popt_" + key: value for key, value in payload["popt"].items()})
        if reference:
            row.update({"grasp_reference_" + key: value for key, value in payload["grasp_reference"].items()})
        traffic = _integer(metrics, "total_offchip_traffic")
        misses, hits = _integer(metrics["L3"], "misses"), _integer(metrics["L3"], "hits")
        row.update(validate_traffic_phases(payload))
        census = metrics.get("kernel_census")
        row.update(validate_kernel_census(census, payload["traffic_phases"]["kernel"]))
        row["kernel_census_detailed_passes"] = len(validate_kernel_census_passes(census))
        validate_bfs_phases(payload, options)
        if options.bfs_traffic_phases == "on":
            for phase in payload["bfs_traffic_phases"]["phases"]:
                for key, value in phase["total"].items():
                    row["bfs_phase_" + phase["phase"].replace("-", "_") + "_" + key] = value
        if options.grasp_scope != "all" and payload.get("grasp_phase_control") is not None:
            row.update({"grasp_phase_" + key: value for key, value in payload["grasp_phase_control"].items()})
        if options.record_model == "window":
            row.update({"window_" + key: value for key, value in payload["window_runtime"].items()})
        if options.record_model == "frontier":
            row.update({"frontier_" + key: value for key, value in payload["frontier_runtime"].items()})
        observer = validate_window_observer(payload, options)
        if observing:
            require(observer["schema"] == "ecg.window-eviction-observer.v4",
                    "new observer runs require bounded protected-candidate attribution")
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
        signatures = {(str(row.get("query_count", "") or 1), *(
            str(row.get("algorithm_" + field, "")) for field in comparable)) for row in good}
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
                str(row.get("record_base_policy", "LRU")), str(row.get("algorithm_record_model", "next")),
                int(row.get("algorithm_window_candidate_rrpv", 6)), str(row.get("frontier_gating", "enabled")))
            for label, column in ((query_policy_label("LRU", int(row.get("query_count", "") or 1)), "traffic_ratio_vs_csr_lru"),
                                  (transport_label, "traffic_ratio_vs_transport")):
                baseline = baselines.get(label)
                if baseline is not None and baseline.get("total_offchip_traffic") is not None and int(baseline["total_offchip_traffic"]) > 0:
                    row[column] = int(row["total_offchip_traffic"]) / int(baseline["total_offchip_traffic"])
                if baseline is not None and int(baseline.get("kernel_total_offchip_traffic", 0)) > 0:
                    row["kernel_" + column] = int(row["kernel_total_offchip_traffic"]) / int(
                        baseline["kernel_total_offchip_traffic"])
