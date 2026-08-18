#!/usr/bin/env python3
"""Fail-closed gate for the literature-scale ReusePlan campaign."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
ECG_DIR = ROOT / "scripts" / "experiments" / "ecg"
sys.path.insert(0, str(ECG_DIR))
sys.path.insert(0, str(ECG_DIR / "flows"))

from analysis import final_campaign_gate, pagerank_gate, three_costs  # noqa: E402
from flows import aggregate_results, experiment_run  # noqa: E402
from policy_specs import policy_output_label  # noqa: E402


DEFAULT_MANIFEST = ECG_DIR / "experiment_manifest.json"
DEFAULT_SCREEN = ECG_DIR / "configs/pagerank_literature_scale.json"
DEFAULT_CORPUS_RECEIPT = (
    ROOT / "results/graphs/literature_scale_corpus.receipt.json")
PROFILE = "reuse_plan_literature_scale_campaign"
SCREEN_STAGES = {
    "60_gem5_proposal_reuse_bind_o3",
    "90_gem5_literature_scale_i1",
    "91_gem5_literature_scale_i8",
}
COMPLETE_STAGES = {
    *SCREEN_STAGES,
    "92_cache_sim_literature_scale_wide16",
    "93_cache_sim_literature_scale_popt",
    "94_cache_sim_literature_scale_compact16",
    "95_sniper_literature_scale_matched",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stages(
        manifest: dict[str, Any],
        selected: set[str]) -> dict[str, dict[str, Any]]:
    result = {
        str(stage["name"]): experiment_run.merged_defaults(manifest, stage)
        for stage in manifest.get("stages", [])
        if PROFILE in stage.get("profiles", []) and
        str(stage["name"]) in selected
    }
    if set(result) != selected:
        raise ValueError(
            f"literature-scale stage roster mismatch: "
            f"expected={sorted(selected)} actual={sorted(result)}")
    return result


def expected_cells(
        manifest: dict[str, Any],
        screen: dict[str, Any],
        selected: set[str],
) -> dict[tuple[str, str, str], tuple[str, ...]]:
    expected = {}
    for name, stage in stages(manifest, selected).items():
        if name.startswith(("90_", "91_")):
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
                expected[(
                    name, str(graph["name"]), str(benchmark))] = roster
    return expected


def grouped_rows(
        rows: list[dict[str, Any]],
        selected: set[str],
) -> dict[tuple[str, str, str], list[dict[str, Any]]]:
    groups: dict[
        tuple[str, str, str], list[dict[str, Any]]
    ] = defaultdict(list)
    for row in rows:
        stage = str(row.get("final_stage", ""))
        if stage in selected:
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
    if set(groups) != set(expected):
        missing = sorted(set(expected) - set(groups))
        extra = sorted(set(groups) - set(expected))
        if missing:
            errors.append(f"missing literature-scale cells: {missing}")
        if extra:
            errors.append(f"unexpected literature-scale cells: {extra}")
    for key in sorted(set(groups) & set(expected)):
        actual = tuple(
            str(row.get("policy_label", ""))
            for row in groups[key])
        if actual != expected[key]:
            errors.append(
                f"{key} policy roster mismatch: "
                f"expected={expected[key]} actual={actual}")
    return errors


def validate_corpus(
        manifest: dict[str, Any],
        receipt_path: Path,
) -> list[str]:
    errors = []
    try:
        receipt = json.loads(receipt_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return [f"literature-scale corpus receipt is invalid: {error}"]
    received = {
        str(graph["name"]): graph
        for graph in receipt.get("graphs", [])
    }
    expected_names = {
        str(graph["name"])
        for graph in manifest["graph_sets"]["literature_scale_core_8mb"]
    }
    if not expected_names <= set(received):
        errors.append("literature-scale corpus receipt is incomplete")
    for graph in manifest["graph_sets"]["literature_scale_core_8mb"]:
        name = str(graph["name"])
        row = received.get(name, {})
        path = ROOT / str(graph["path"])
        if str(row.get("dbg_sg", "")) != str(path.relative_to(ROOT)):
            errors.append(f"{name} manifest path differs from corpus receipt")
            continue
        if not path.is_file():
            errors.append(f"{name} preordered graph is missing")
            continue
        if row.get("dbg_sg_sha256") != sha256(path):
            errors.append(f"{name} preordered graph hash mismatch")
        info = three_costs.read_sg(path, name)
        if (
                info.directed or
                info.vertices != int(row.get("dbg_serialized_vertices", -1)) or
                info.serialized_edges != int(
                    row.get("dbg_serialized_edges", -1))):
            errors.append(f"{name} preordered graph header mismatch")
    for graph in manifest["graph_sets"]["literature_scale_compact16"]:
        name = str(graph["name"])
        path = ROOT / str(graph["path"])
        info = three_costs.read_sg(path, name)
        id_bits = max(1, (info.vertices - 1).bit_length())
        if id_bits + 2 + 2 * 4 > 32:
            errors.append(f"{name} compact16 record does not fit")
    return errors


def validate_rows(
        groups: dict[tuple[str, str, str], list[dict[str, Any]]],
        manifest: dict[str, Any],
) -> list[str]:
    errors = []
    sniper_graphs = {
        str(graph["name"]): graph
        for graph in manifest["graph_sets"]["literature_scale_sniper"]
    }
    for key, rows in sorted(groups.items()):
        stage, graph, _benchmark = key
        for row in rows:
            policy = str(row.get("policy_label", ""))
            if row.get("status") != "ok":
                errors.append(f"{key}/{policy} status={row.get('status')}")
            if not row.get("final_matrix_config_hash"):
                errors.append(f"{key}/{policy} missing matrix config hash")
            if stage.startswith(("90_", "91_")):
                if (
                        row.get("simulator") != "gem5" or
                        row.get("gem5_cpu_type") != "O3" or
                        str(row.get("timing_valid_for_speedup")) != "1"):
                    errors.append(f"{key}/{policy} is not valid gem5 O3 timing")
                if policy.startswith("ECG_REUSE_PLAN") and (
                        final_campaign_gate.integer(
                            row, "ecg_record_bytes") != 4 or
                        str(row.get(
                            "gem5_reuse_plan_sidecar_active")) != "1"):
                    errors.append(
                        f"{key}/{policy} lacks compact validated sidecar")
            elif stage.startswith("60_"):
                if (
                        row.get("simulator") != "gem5" or
                        str(row.get("timing_valid_for_speedup")) != "0"):
                    errors.append(f"{key}/{policy} mechanism row is malformed")
            elif stage.startswith(("92_", "93_", "94_")):
                if (
                        row.get("simulator") != "cache_sim" or
                        str(row.get("timing_valid_for_speedup")) != "0"):
                    errors.append(f"{key}/{policy} cache row is malformed")
                if policy.startswith("ECG_REUSE_PLAN"):
                    expected_width = 4 if stage.startswith("94_") else 8
                    if (
                            final_campaign_gate.integer(
                                row, "ecg_record_bytes") != expected_width or
                            final_campaign_gate.integer(
                                row, "ecg_epochs_effective") != 16):
                        errors.append(
                            f"{key}/{policy} width/epoch mismatch")
                if stage.startswith("93_") and policy == "POPT":
                    if (
                            row.get("popt_matrix_stream_mode") != "simulated" or
                            not final_campaign_gate.positive(
                                row, "popt_matrix_stream_lines_simulated")):
                        errors.append(f"{key}/{policy} P-OPT stream is uncharged")
            elif stage.startswith("95_"):
                graph_spec = sniper_graphs.get(graph, {})
                expected_limit = int(
                    graph_spec.get("sniper_semantic_edge_limit", 0))
                if (
                        row.get("simulator") != "sniper" or
                        str(row.get("timing_valid_for_speedup")) != "0" or
                        row.get("sniper_queue_model") != "windowed_mg1" or
                        final_campaign_gate.integer(
                            row, "sniper_semantic_edge_limit") !=
                        expected_limit or
                        str(row.get("semantic_work_matched")) != "1"):
                    errors.append(f"{key}/{policy} Sniper role is malformed")
                if policy.startswith("ECG_REUSE_PLAN") and (
                        final_campaign_gate.integer(
                            row, "sniper_transport_record_bytes") != 8 or
                        final_campaign_gate.integer(
                            row, "sniper_reuse_bind_consumes") < 32 or
                        final_campaign_gate.integer(
                            row, "sniper_reuse_bind_bad_consumes") != 0 or
                        str(row.get(
                            "sniper_reuse_bind_exact_validated")) != "1"):
                    errors.append(f"{key}/{policy} exact-bind proof failed")
    return errors


def evaluate(
        rows: list[dict[str, Any]],
        manifest: dict[str, Any],
        screen: dict[str, Any],
        run_dirs: list[Path],
        phase: str,
        corpus_receipt: Path,
) -> dict[str, Any]:
    selected = SCREEN_STAGES if phase == "screen" else COMPLETE_STAGES
    groups = grouped_rows(rows, selected)
    errors = []
    errors.extend(validate_rosters(
        groups, expected_cells(manifest, screen, selected)))
    errors.extend(validate_rows(groups, manifest))
    errors.extend(final_campaign_gate.validate_run_preflight(run_dirs))
    if phase == "complete":
        errors.extend(validate_corpus(manifest, corpus_receipt))

    timing_rows = [
        row for row in rows
        if row.get("final_stage") in {
            "90_gem5_literature_scale_i1",
            "91_gem5_literature_scale_i8",
        }
    ]
    pagerank_result = None
    if len(timing_rows) == 96:
        try:
            pagerank_result = pagerank_gate.evaluate(timing_rows, screen)
            if not pagerank_result["screen_valid"]:
                errors.append("PageRank baseline/oracle sanity failed")
            if not pagerank_result["screen_passes"]:
                errors.append("PageRank primary candidate failed")
        except ValueError as error:
            errors.append(f"PageRank timing gate failed: {error}")
    else:
        errors.append(
            f"PageRank timing rows incomplete: "
            f"expected=96 actual={len(timing_rows)}")
    return {
        "valid": not errors,
        "phase": phase,
        "errors": errors,
        "run_dirs": [str(path) for path in run_dirs],
        "cell_count": len(groups),
        "row_count": sum(len(group) for group in groups.values()),
        "stage_rows": {
            stage: sum(
                len(group) for key, group in groups.items()
                if key[0] == stage)
            for stage in sorted(selected)
        },
        "pagerank_gate": pagerank_result,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input-run-dirs", nargs="+", required=True)
    parser.add_argument(
        "--phase", choices=("screen", "complete"), default="complete")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--screen-config", default=str(DEFAULT_SCREEN))
    parser.add_argument(
        "--corpus-receipt", default=str(DEFAULT_CORPUS_RECEIPT))
    parser.add_argument("--output", default="")
    args = parser.parse_args(argv)

    run_dirs = final_campaign_gate.discover_run_dirs([
        experiment_run.resolve_path(path)
        for path in args.input_run_dirs
    ])
    with redirect_stdout(sys.stderr):
        rows, _ = aggregate_results.collect_csvs(run_dirs, [])
    result = evaluate(
        rows,
        json.loads(Path(args.manifest).read_text()),
        json.loads(Path(args.screen_config).read_text()),
        run_dirs,
        args.phase,
        Path(args.corpus_receipt),
    )
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text, end="")
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
