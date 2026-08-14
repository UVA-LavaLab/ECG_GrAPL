#!/usr/bin/env python3
"""Validate a complete role-separated ReusePlan final campaign."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
ECG_DIR = ROOT / "scripts" / "experiments" / "ecg"
sys.path.insert(0, str(ECG_DIR))
sys.path.insert(0, str(ECG_DIR / "flows"))

from flows import aggregate_results, experiment_run  # noqa: E402
from policy_specs import policy_output_label  # noqa: E402
from analysis import pagerank_gate, three_costs  # noqa: E402


DEFAULT_MANIFEST = ECG_DIR / "experiment_manifest.json"
DEFAULT_SCREEN = ECG_DIR / "configs" / "pagerank_study.json"
FINAL_PROFILE = "reuse_plan_final_campaign"
TIMING_STAGES = {
    "70_gem5_pagerank_i1",
    "71_gem5_pagerank_i2",
    "72_gem5_pagerank_i4",
    "73_gem5_pagerank_i8",
}
CACHE_CONTROL_STAGES = {
    "80_cache_sim_final_fullgraph",
    "82_cache_sim_final_wide16",
    "83_cache_sim_final_wide256",
}
FINAL_STAGES = {
    "60_gem5_proposal_reuse_bind_o3",
    *TIMING_STAGES,
    *CACHE_CONTROL_STAGES,
    "81_sniper_final_semantic",
    "84_cache_sim_final_popt",
    "85_sniper_final_sssp_wide",
}
BASELINE_DRIFT_FIELDS = (
    "total_accesses",
    "l1_hits",
    "l1_misses",
    "l2_hits",
    "l2_misses",
    "l3_hits",
    "l3_misses",
    "llc_writebacks",
    "memory_accesses",
    "total_memory_traffic",
    "total_offchip_traffic",
)
ALLOWED_UNTRACKED_PREFLIGHT = (
    "bench/include/gem5_sim/gem5",
    "bench/include/sniper_sim/snipersim",
)


def integer(row: dict[str, Any], field: str) -> int:
    try:
        value = float(row.get(field, ""))
    except (TypeError, ValueError) as error:
        raise ValueError(
            f"{field} is not numeric: {row.get(field)!r}") from error
    if value != int(value):
        raise ValueError(f"{field} is not integral: {value}")
    return int(value)


def positive(row: dict[str, Any], field: str) -> bool:
    try:
        return float(row.get(field, 0) or 0) > 0
    except (TypeError, ValueError):
        return False


def discover_run_dirs(paths: list[Path]) -> list[Path]:
    discovered = set()
    for path in paths:
        path = path.resolve()
        if (path / "resolved_manifest.json").is_file():
            discovered.add(path)
            continue
        for manifest in path.glob("**/resolved_manifest.json"):
            discovered.add(manifest.parent.resolve())
    return sorted(discovered)


def final_stages(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    stages = {
        str(stage["name"]): experiment_run.merged_defaults(manifest, stage)
        for stage in manifest.get("stages", [])
        if FINAL_PROFILE in stage.get("profiles", [])
    }
    if set(stages) != FINAL_STAGES:
        raise ValueError(
            "final stage roster differs from the gate: "
            f"expected={sorted(FINAL_STAGES)} actual={sorted(stages)}")
    return stages


def expected_cells(
        manifest: dict[str, Any],
        screen: dict[str, Any],
) -> dict[tuple[str, str, str], tuple[str, ...]]:
    expected = {}
    for name, stage in final_stages(manifest).items():
        if name in TIMING_STAGES:
            graphs = screen["graphs"]
            benchmarks = [screen["benchmark"]]
            policies = screen["policies"]["all"]
        else:
            graphs = manifest["graph_sets"][stage["graph_set"]]
            benchmarks = stage["benchmarks"]
            policies = stage["policies"]
        roster = tuple(policy_output_label(policy) for policy in policies)
        for graph in graphs:
            for benchmark in benchmarks:
                expected[(name, str(graph["name"]), str(benchmark))] = roster
    return expected


def grouped_rows(
        rows: list[dict[str, Any]],
) -> dict[tuple[str, str, str], list[dict[str, Any]]]:
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        stage = str(row.get("final_stage", ""))
        if stage not in FINAL_STAGES:
            continue
        groups[(
            stage,
            str(row.get("final_graph", "")),
            str(row.get("benchmark", "")),
        )].append(row)
    return groups


def validate_rosters(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        expected: dict[tuple[str, str, str], tuple[str, ...]],
) -> list[str]:
    errors = []
    missing = sorted(set(expected) - set(groups))
    extra = sorted(set(groups) - set(expected))
    if missing:
        errors.append(f"missing final cells: {missing}")
    if extra:
        errors.append(f"unexpected final cells: {extra}")
    for key in sorted(set(expected) & set(groups)):
        actual = tuple(row.get("policy_label", "") for row in groups[key])
        if actual != expected[key]:
            errors.append(
                f"{key} policy roster mismatch: "
                f"expected={expected[key]} actual={actual}")
    return errors


def validate_role_rows(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        manifest: dict[str, Any],
) -> list[str]:
    errors = []
    final_graphs = {
        str(graph["name"]): graph
        for graph in manifest["graph_sets"]["factorial_graphs_uniform_8mb"]
    }
    for key, rows in sorted(groups.items()):
        stage, graph, benchmark = key
        for row in rows:
            policy = str(row.get("policy_label", ""))
            if row.get("status") != "ok":
                errors.append(f"{key}/{policy} status={row.get('status')}")
            if not row.get("final_matrix_config_hash"):
                errors.append(f"{key}/{policy} missing matrix config hash")
            if stage in TIMING_STAGES:
                if (
                        row.get("simulator") != "gem5" or
                        row.get("gem5_cpu_type") != "O3" or
                        str(row.get("timing_valid_for_speedup")) != "1"):
                    errors.append(f"{key}/{policy} is not valid gem5 O3 timing")
                if policy.startswith("ECG_REUSE_PLAN") and integer(
                        row, "ecg_record_bytes") != 4:
                    errors.append(f"{key}/{policy} is not compact 4-byte ReusePlan")
            elif stage == "60_gem5_proposal_reuse_bind_o3":
                if row.get("simulator") != "gem5":
                    errors.append(f"{key}/{policy} mechanism row is not gem5")
                if str(row.get("timing_valid_for_speedup")) != "0":
                    errors.append(f"{key}/{policy} mechanism timing is claimable")
            elif stage.startswith(("80_", "82_", "83_", "84_")):
                if (
                        row.get("simulator") != "cache_sim" or
                        str(row.get("timing_valid_for_speedup")) != "0"):
                    errors.append(f"{key}/{policy} cache role is malformed")
                expected_ways = (
                    15 if stage == "84_cache_sim_final_popt" and
                    policy == "POPT" else 16)
                if integer(row, "l3_effective_ways") != expected_ways:
                    errors.append(
                        f"{key}/{policy} effective LLC ways != {expected_ways}")
                if policy.startswith("ECG_REUSE_PLAN"):
                    expected_width = 4 if stage in {
                        "80_cache_sim_final_fullgraph",
                        "84_cache_sim_final_popt",
                    } else 8
                    expected_epochs = (
                        256 if stage == "83_cache_sim_final_wide256" else 16)
                    if integer(row, "ecg_record_bytes") != expected_width:
                        errors.append(
                            f"{key}/{policy} record width mismatch")
                    if integer(row, "ecg_epochs_effective") != expected_epochs:
                        errors.append(f"{key}/{policy} epoch count mismatch")
                if stage == "84_cache_sim_final_popt" and policy == "POPT":
                    if (
                            integer(row, "popt_effective_l3_ways") != 15 or
                            not positive(
                                row, "popt_matrix_stream_lines_simulated") or
                            row.get("popt_matrix_stream_mode") != "simulated"):
                        errors.append(f"{key}/{policy} P-OPT stream is not charged")
            elif stage.startswith(("81_", "85_")):
                expected_width = 4 if stage.startswith("81_") else 8
                if graph not in final_graphs:
                    errors.append(f"{key}/{policy} uses an unknown final graph")
                    continue
                expected_limit = integer(
                    final_graphs[graph], "sniper_semantic_edge_limit")
                if (
                        row.get("simulator") != "sniper" or
                        str(row.get("timing_valid_for_speedup")) != "0" or
                        row.get("sniper_queue_model") != "windowed_mg1"):
                    errors.append(f"{key}/{policy} Sniper role is malformed")
                if (
                        integer(row, "sniper_transport_record_bytes") !=
                        expected_width or
                        integer(row, "edge_stream_bytes_per_edge") !=
                        expected_width):
                    errors.append(f"{key}/{policy} Sniper width mismatch")
                if (
                        integer(row, "sniper_semantic_edge_visits") !=
                        expected_limit or
                        str(row.get("semantic_work_matched")) != "1"):
                    errors.append(f"{key}/{policy} semantic work mismatch")
                if not positive(row, "l3_accesses") or not positive(
                        row, "l3_misses"):
                    errors.append(f"{key}/{policy} lacks LLC turnover")
                if policy.startswith("ECG_REUSE_PLAN") and (
                        integer(row, "sniper_reuse_bind_consumes") < 32 or
                        integer(row, "sniper_reuse_bind_bad_consumes") != 0 or
                        str(row.get("sniper_reuse_bind_exact_validated")) != "1"):
                    errors.append(f"{key}/{policy} exact-bind proof failed")
    return errors


def validate_cache_baselines(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
) -> list[str]:
    errors = []
    by_cell: dict[tuple[str, str], dict[str, tuple[str, ...]]] = defaultdict(dict)
    for (stage, graph, benchmark), rows in groups.items():
        if stage not in CACHE_CONTROL_STAGES or benchmark == "sssp":
            continue
        lru = next((row for row in rows if row["policy_label"] == "LRU"), None)
        if lru is not None:
            by_cell[(graph, benchmark)][stage] = tuple(
                str(lru.get(field, "")) for field in BASELINE_DRIFT_FIELDS)
    for key, stages in sorted(by_cell.items()):
        if set(stages) != CACHE_CONTROL_STAGES:
            errors.append(f"{key} baseline controls are incomplete")
        elif len(set(stages.values())) != 1:
            errors.append(f"{key} baseline drift across width/epoch controls")
    return errors


def validate_final_graph_receipts(
        manifest: dict[str, Any], compact_tier_bits: int,
        root: Path = ROOT) -> list[str]:
    errors = []
    graphs = manifest["graph_sets"]["factorial_graphs_uniform_8mb"]
    for graph in graphs:
        name = str(graph["name"])
        source = str(graph.get("sniper_semantic_edge_source", ""))
        if source != "symmetrized .sg serialized edge count":
            errors.append(f"{name} semantic edge source is not declared")
        path = Path(str(graph["path"]))
        if not path.is_absolute():
            path = root / path
        if not path.is_file():
            errors.append(f"{name} final graph is missing: {path}")
            continue
        info = three_costs.read_sg(path, name)
        if info.directed:
            errors.append(f"{name} final graph is not symmetrized")
        expected_edges = integer(graph, "sniper_semantic_edge_limit")
        if info.serialized_edges != expected_edges:
            errors.append(
                f"{name} serialized edges mismatch: "
                f"manifest={expected_edges} graph={info.serialized_edges}")
        expected_id_bits = max(1, (info.vertices - 1).bit_length())
        if integer(graph, "compact_id_bits") != expected_id_bits:
            errors.append(
                f"{name} compact id bits mismatch: "
                f"manifest={graph['compact_id_bits']} graph={expected_id_bits}")
        total_bits = (
            expected_id_bits + compact_tier_bits +
            2 * integer(graph, "compact_epoch_bits"))
        if integer(graph, "compact_total_bits") != total_bits:
            errors.append(f"{name} compact total bits mismatch")
    return errors


def validate_run_preflight(run_dirs: list[Path]) -> list[str]:
    errors = []
    for run_dir in run_dirs:
        diff = run_dir / "preflight" / "git_diff_stat.txt"
        if diff.exists() and diff.read_text().strip():
            errors.append(f"{run_dir} executed with tracked changes")
        status = run_dir / "preflight" / "git_status.txt"
        if not status.exists():
            continue
        unexpected = []
        for line in status.read_text().splitlines():
            if not line:
                continue
            if line.startswith("?? ") and any(
                    line[3:] == allowed or
                    line[3:].startswith(allowed + "/")
                    for allowed in ALLOWED_UNTRACKED_PREFLIGHT):
                continue
            unexpected.append(line)
        if unexpected:
            errors.append(
                f"{run_dir} has unexpected worktree state: {unexpected}")
    return errors


def evaluate(
        rows: list[dict[str, Any]],
        manifest: dict[str, Any],
        screen: dict[str, Any],
        run_dirs: list[Path],
) -> dict[str, Any]:
    groups = grouped_rows(rows)
    errors = []
    errors.extend(validate_rosters(groups, expected_cells(manifest, screen)))
    errors.extend(validate_role_rows(groups, manifest))
    errors.extend(validate_cache_baselines(groups))
    errors.extend(validate_final_graph_receipts(
        manifest, integer(screen, "compact_tier_bits")))
    errors.extend(validate_run_preflight(run_dirs))
    timing_rows = [
        row for row in rows if row.get("final_stage") in TIMING_STAGES]
    pagerank_result = None
    if len(timing_rows) == 84:
        try:
            pagerank_result = pagerank_gate.evaluate(timing_rows, screen)
            if not pagerank_result["screen_valid"]:
                errors.append("PageRank timing baseline/oracle sanity failed")
        except ValueError as error:
            errors.append(f"PageRank timing gate failed: {error}")
    else:
        errors.append(
            f"PageRank timing rows incomplete: expected=84 actual={len(timing_rows)}")
    return {
        "valid": not errors,
        "errors": errors,
        "run_dirs": [str(path) for path in run_dirs],
        "cell_count": len(groups),
        "row_count": sum(len(group) for group in groups.values()),
        "stage_rows": {
            stage: sum(
                len(group) for key, group in groups.items()
                if key[0] == stage)
            for stage in sorted(FINAL_STAGES)
        },
        "pagerank_gate": pagerank_result,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input-run-dirs", nargs="+", required=True,
        help="Run directories or roots containing per-cell run directories.")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--screen-config", default=str(DEFAULT_SCREEN))
    parser.add_argument("--output", default="")
    args = parser.parse_args(argv)

    run_dirs = discover_run_dirs(
        [experiment_run.resolve_path(path) for path in args.input_run_dirs])
    with redirect_stdout(sys.stderr):
        rows, _ = aggregate_results.collect_csvs(run_dirs, [])
    result = evaluate(
        rows,
        json.loads(Path(args.manifest).read_text()),
        json.loads(Path(args.screen_config).read_text()),
        run_dirs,
    )
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text, end="")
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
