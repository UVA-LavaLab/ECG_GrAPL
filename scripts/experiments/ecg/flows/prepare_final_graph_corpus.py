#!/usr/bin/env python3
"""Download, convert, and receipt the literature-scale final graph corpus."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_CONFIG = (
    PROJECT_ROOT / "scripts/experiments/ecg/configs/"
    "final_graph_corpus.json")
DEFAULT_GRAPH_ROOT = PROJECT_ROOT / "results/graphs"
DEFAULT_RECEIPT = (
    DEFAULT_GRAPH_ROOT / "literature_scale_corpus.receipt.json")
CONVERTER = PROJECT_ROOT / "bench/bin/converter"
NATIVE_PR = PROJECT_ROOT / "bench/bin_gem5/pr"
SAMPLE_SCRIPT = (
    PROJECT_ROOT / "scripts/experiments/ecg/flows/"
    "sample_realgraph.py")

sys.path.insert(
    0, str(PROJECT_ROOT / "scripts/experiments/ecg"))
from analysis import three_costs  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_config(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text())
    if data.get("version") != 1:
        raise SystemExit("unsupported final graph corpus version")
    graphs = data.get("graphs")
    if not isinstance(graphs, list) or not graphs:
        raise SystemExit("final graph corpus has no graphs")
    names = [str(graph.get("name", "")) for graph in graphs]
    if any(not name for name in names) or len(names) != len(set(names)):
        raise SystemExit("final graph corpus names must be unique")
    return data


def selected_graphs(
        config: dict[str, Any], names: list[str],
        include_scale_stress: bool) -> list[dict[str, Any]]:
    requested = set(names)
    known = {str(graph["name"]) for graph in config["graphs"]}
    unknown = requested - known
    if unknown:
        raise SystemExit(
            "unknown graph(s): " + ", ".join(sorted(unknown)))
    selected = []
    for graph in config["graphs"]:
        if requested and graph["name"] not in requested:
            continue
        if not requested and graph.get("role") == "scale_stress" and \
                not include_scale_stress:
            continue
        selected.append(graph)
    if not selected:
        raise SystemExit("no graphs selected")
    return selected


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    command = [
        "curl", "--fail", "--location", "--retry", "5",
        "--retry-delay", "5", "--continue-at", "-",
        "--output", str(partial), url,
    ]
    subprocess.run(command, check=True)
    os.replace(partial, destination)


def decompress_gzip(source: Path, destination: Path) -> None:
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    with gzip.open(source, "rb") as compressed, temporary.open("wb") as out:
        shutil.copyfileobj(compressed, out, 16 * 1024 * 1024)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, destination)


def convert(
        edge_list: Path, output: Path, symmetrize: bool,
        reorder: int = 0) -> None:
    if not CONVERTER.is_file():
        raise SystemExit(
            f"missing converter {CONVERTER}; run make converter")
    temporary_base = output.with_name(output.stem + "_tmp")
    generated = temporary_base.with_suffix(".sg")
    generated.unlink(missing_ok=True)
    command = [
        str(CONVERTER), "-f", str(edge_list),
        *([] if not symmetrize else ["-s"]),
        "-o", str(reorder),
        "-b", str(temporary_base),
    ]
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)
    os.replace(generated, output)


def prepare_timing_sample(
        graph: dict[str, Any], graph_root: Path,
        force: bool) -> None:
    sample_name = str(graph.get("timing_sample_name", ""))
    target_vertices = int(graph.get("timing_sample_vertices", 0) or 0)
    target_edges = int(graph.get("timing_sample_edges", 0) or 0)
    if not sample_name or target_vertices <= 0:
        return
    if target_edges <= 0:
        raise SystemExit(
            f"{graph['name']} timing sample lacks an edge budget")
    source = (
        graph_root / str(graph["name"]) /
        str(graph["edge_list"]))
    if not source.is_file():
        raise SystemExit(
            f"missing edge list for timing sample {graph['name']}: "
            f"{source}")
    directory = graph_root / sample_name
    edge_list = directory / f"{sample_name}.el"
    vertices = directory / f"{sample_name}.vertices.tsv"
    metadata = directory / f"{sample_name}.sample.json"
    sg = directory / f"{sample_name}.sg"
    dbg_sg = directory / f"{sample_name}-dbg.sg"
    semantic = directory / f"{sample_name}.semantic.json"
    directory.mkdir(parents=True, exist_ok=True)
    metadata_current = False
    if metadata.is_file():
        try:
            existing = json.loads(metadata.read_text())
            metadata_current = (
                int(existing.get("vertices", 0)) == target_vertices and
                int(existing.get("target_edges", 0)) == target_edges)
        except (OSError, ValueError, json.JSONDecodeError):
            metadata_current = False
    sample_changed = force or not metadata_current or not all(
        path.is_file() for path in (edge_list, vertices))
    if sample_changed:
        subprocess.run([
            sys.executable, str(SAMPLE_SCRIPT),
            "--input", str(source),
            "--output", str(edge_list),
            "--vertices", str(vertices),
            "--metadata", str(metadata),
            "--target-vertices", str(target_vertices),
            "--target-edges", str(target_edges),
        ], cwd=PROJECT_ROOT, check=True)
        semantic.unlink(missing_ok=True)
    if force or sample_changed or not sg.is_file():
        convert(edge_list, sg, True)
    if force or sample_changed or not dbg_sg.is_file():
        convert(
            sg, dbg_sg, False,
            int(graph.get("reorder", 5)))


def parse_pr_receipt(text: str) -> dict[str, Any]:
    match = re.search(
        r"\[ECG-PR-RESULT iterations=(\d+) "
        r"semantic_edges=(\d+) score_checksum=([0-9a-f]+)\]",
        text)
    if not match:
        raise ValueError("PageRank semantic receipt is missing")
    return {
        "iterations": int(match.group(1)),
        "edges": int(match.group(2)),
        "checksum": match.group(3),
    }


def prepare_semantic_receipts(
        graph: dict[str, Any], graph_root: Path,
        force: bool) -> None:
    sample_name = str(graph.get("timing_sample_name", ""))
    if not sample_name:
        return
    sample_dir = graph_root / sample_name
    sample = sample_dir / f"{sample_name}-dbg.sg"
    output = sample_dir / f"{sample_name}.semantic.json"
    if output.is_file() and not force:
        return
    if not NATIVE_PR.is_file():
        raise SystemExit(
            f"missing native PageRank binary {NATIVE_PR}; "
            "run make gem5-pr")
    if not sample.is_file():
        raise SystemExit(
            f"missing preordered timing sample: {sample}")
    receipts = {}
    for iterations in (1, 8):
        result = subprocess.run([
            str(NATIVE_PR), "-f", str(sample),
            "-o", "0", "-n", "1", "-i", str(iterations),
            "-t", "0",
        ], cwd=PROJECT_ROOT, capture_output=True, text=True, check=True)
        receipt = parse_pr_receipt(
            result.stdout + "\n" + result.stderr)
        if receipt["iterations"] != iterations:
            raise SystemExit(
                f"{sample_name} iteration receipt mismatch")
        receipts[str(iterations)] = {
            "edges": receipt["edges"],
            "checksum": receipt["checksum"],
        }
    payload = {
        "version": 1,
        "sample": sample_name,
        "sample_sha256": sha256(sample),
        "options": "-o 0 -n 1 -t 0",
        "receipts": receipts,
    }
    temporary = output.with_suffix(
        output.suffix + f".tmp.{os.getpid()}")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, output)


def graph_receipt(
        graph: dict[str, Any], graph_root: Path) -> dict[str, Any]:
    directory = graph_root / str(graph["name"])
    archive = directory / str(graph["archive"])
    edge_list = directory / str(graph["edge_list"])
    sg = directory / str(graph["sg"])
    dbg_sg = directory / str(graph["dbg_sg"])
    def display(path: Path) -> str:
        try:
            return str(path.relative_to(PROJECT_ROOT))
        except ValueError:
            return str(path)

    row: dict[str, Any] = {
        "name": graph["name"],
        "role": graph["role"],
        "source": graph["source"],
        "source_page": graph["source_page"],
        "url": graph["url"],
        "source_vertices": graph["source_vertices"],
        "source_edges": graph["source_edges"],
        "source_directed": graph["source_directed"],
        "symmetrize": graph["symmetrize"],
        "archive": display(archive),
        "edge_list": display(edge_list),
        "sg": display(sg),
        "dbg_sg": display(dbg_sg),
        "reorder": int(graph["reorder"]),
    }
    for label, path in (
            ("archive", archive),
            ("edge_list", edge_list),
            ("sg", sg)):
        row[f"{label}_present"] = path.is_file()
        if path.is_file():
            row[f"{label}_bytes"] = path.stat().st_size
            row[f"{label}_sha256"] = sha256(path)
    if sg.is_file():
        info = three_costs.read_sg(sg, str(graph["name"]))
        row.update({
            "serialized_vertices": info.vertices,
            "serialized_edges": info.serialized_edges,
            "serialized_directed": info.directed,
        })
        if graph["symmetrize"] and info.directed:
            raise SystemExit(
                f"{graph['name']} conversion is not symmetrized")
        if not graph["symmetrize"] and not info.directed:
            raise SystemExit(
                f"{graph['name']} conversion lost directedness")
    row["dbg_sg_present"] = dbg_sg.is_file()
    if dbg_sg.is_file():
        dbg_info = three_costs.read_sg(
            dbg_sg, f"{graph['name']}-dbg")
        row.update({
            "dbg_sg_bytes": dbg_sg.stat().st_size,
            "dbg_sg_sha256": sha256(dbg_sg),
            "dbg_serialized_vertices": dbg_info.vertices,
            "dbg_serialized_edges": dbg_info.serialized_edges,
            "dbg_serialized_directed": dbg_info.directed,
        })
        if sg.is_file() and (
                dbg_info.vertices != row["serialized_vertices"] or
                dbg_info.serialized_edges != row["serialized_edges"] or
                dbg_info.directed != row["serialized_directed"]):
            raise SystemExit(
                f"{graph['name']} DBG reorder changed graph geometry")
    sample_name = str(graph.get("timing_sample_name", ""))
    if sample_name:
        sample_dir = graph_root / sample_name
        sample_sg = sample_dir / f"{sample_name}.sg"
        sample_dbg_sg = sample_dir / f"{sample_name}-dbg.sg"
        semantic_path = sample_dir / f"{sample_name}.semantic.json"
        row["timing_sample_name"] = sample_name
        row["timing_sample_vertices_requested"] = int(
            graph["timing_sample_vertices"])
        row["timing_sample_edges_requested"] = int(
            graph["timing_sample_edges"])
        row["timing_sample_sg"] = display(sample_sg)
        row["timing_sample_dbg_sg"] = display(sample_dbg_sg)
        row["timing_sample_present"] = sample_sg.is_file()
        if sample_sg.is_file():
            sample_info = three_costs.read_sg(
                sample_sg, sample_name)
            row.update({
                "timing_sample_bytes": sample_sg.stat().st_size,
                "timing_sample_sha256": sha256(sample_sg),
                "timing_sample_vertices": sample_info.vertices,
                "timing_sample_edges": sample_info.serialized_edges,
                "timing_sample_directed": sample_info.directed,
            })
            if sample_info.directed:
                raise SystemExit(
                    f"{sample_name} timing sample is not symmetrized")
        row["timing_sample_dbg_present"] = sample_dbg_sg.is_file()
        if sample_dbg_sg.is_file():
            sample_dbg = three_costs.read_sg(
                sample_dbg_sg, f"{sample_name}-dbg")
            row.update({
                "timing_sample_dbg_bytes":
                    sample_dbg_sg.stat().st_size,
                "timing_sample_dbg_sha256":
                    sha256(sample_dbg_sg),
                "timing_sample_dbg_vertices":
                    sample_dbg.vertices,
                "timing_sample_dbg_edges":
                    sample_dbg.serialized_edges,
                "timing_sample_dbg_directed":
                    sample_dbg.directed,
            })
            if sample_sg.is_file() and (
                    sample_dbg.vertices !=
                    row["timing_sample_vertices"] or
                    sample_dbg.serialized_edges !=
                    row["timing_sample_edges"] or
                    sample_dbg.directed !=
                    row["timing_sample_directed"]):
                raise SystemExit(
                    f"{sample_name} DBG reorder changed sample geometry")
        row["timing_semantic_receipt"] = display(semantic_path)
        row["timing_semantic_present"] = semantic_path.is_file()
        if semantic_path.is_file():
            semantic = json.loads(semantic_path.read_text())
            if semantic.get("sample_sha256") != \
                    row.get("timing_sample_dbg_sha256"):
                raise SystemExit(
                    f"{sample_name} semantic receipt is stale")
            row["timing_semantic_receipts"] = semantic["receipts"]
    return row


def write_receipt(
        config: dict[str, Any], config_path: Path, graph_root: Path,
        receipt_path: Path) -> None:
    rows = [
        graph_receipt(graph, graph_root)
        for graph in config["graphs"]
    ]
    payload = {
        "version": 1,
        "corpus": config["id"],
        "config_sha256": sha256(config_path),
        "graphs": rows,
    }
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = receipt_path.with_suffix(
        receipt_path.suffix + f".tmp.{os.getpid()}")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, receipt_path)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--graph-root", type=Path, default=DEFAULT_GRAPH_ROOT)
    parser.add_argument("--receipt", type=Path, default=DEFAULT_RECEIPT)
    parser.add_argument("--graphs", nargs="*", default=[])
    parser.add_argument("--include-scale-stress", action="store_true")
    parser.add_argument("--download-only", action="store_true")
    parser.add_argument("--convert-only", action="store_true")
    parser.add_argument("--force-download", action="store_true")
    parser.add_argument("--force-decompress", action="store_true")
    parser.add_argument("--force-convert", action="store_true")
    parser.add_argument("--receipt-only", action="store_true")
    parser.add_argument("--prepare-samples", action="store_true")
    parser.add_argument("--samples-only", action="store_true")
    parser.add_argument("--force-samples", action="store_true")
    parser.add_argument("--prepare-semantics", action="store_true")
    parser.add_argument("--semantics-only", action="store_true")
    parser.add_argument("--force-semantics", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    config_path = args.config.resolve()
    graph_root = args.graph_root.resolve()
    receipt_path = args.receipt.resolve()
    config = load_config(config_path)
    selected = selected_graphs(
        config, args.graphs, args.include_scale_stress)
    if args.download_only and args.convert_only:
        raise SystemExit(
            "--download-only and --convert-only are mutually exclusive")
    if args.samples_only and (
            args.download_only or args.convert_only or args.receipt_only):
        raise SystemExit(
            "--samples-only cannot be combined with download/convert/receipt-only")
    if args.semantics_only and (
            args.download_only or args.convert_only or args.receipt_only or
            args.samples_only):
        raise SystemExit(
            "--semantics-only cannot be combined with other exclusive modes")

    if not args.receipt_only and not args.samples_only and \
            not args.semantics_only:
        for graph in selected:
            directory = graph_root / str(graph["name"])
            archive = directory / str(graph["archive"])
            edge_list = directory / str(graph["edge_list"])
            sg = directory / str(graph["sg"])
            dbg_sg = directory / str(graph["dbg_sg"])
            directory.mkdir(parents=True, exist_ok=True)
            if not args.convert_only and (
                    args.force_download or (
                        not archive.is_file() and
                        not edge_list.is_file() and
                        not sg.is_file())):
                download(str(graph["url"]), archive)
            if not args.download_only and (
                    args.force_decompress or not edge_list.is_file()):
                if not archive.is_file():
                    raise SystemExit(
                        f"missing archive for {graph['name']}: {archive}")
                decompress_gzip(archive, edge_list)
            if not args.download_only and (
                    args.force_convert or not sg.is_file()):
                if not edge_list.is_file():
                    raise SystemExit(
                        f"missing edge list for {graph['name']}: "
                        f"{edge_list}")
                convert(edge_list, sg, bool(graph["symmetrize"]))
            if not args.download_only and (
                    args.force_convert or not dbg_sg.is_file()):
                if not sg.is_file():
                    raise SystemExit(
                        f"missing serialized graph for {graph['name']}: "
                        f"{sg}")
                convert(
                    sg, dbg_sg, False,
                    int(graph.get("reorder", 5)))
    if args.prepare_samples or args.samples_only:
        for graph in selected:
            prepare_timing_sample(
                graph, graph_root, args.force_samples)
    if args.prepare_semantics or args.semantics_only:
        for graph in selected:
            prepare_semantic_receipts(
                graph, graph_root, args.force_semantics)

    write_receipt(config, config_path, graph_root, receipt_path)
    print(f"[graph-corpus] wrote {receipt_path}")
    for graph in selected:
        row = graph_receipt(graph, graph_root)
        print(
            f"[graph-corpus] {graph['name']} "
            f"archive={int(row['archive_present'])} "
            f"edge_list={int(row['edge_list_present'])} "
            f"sg={int(row['sg_present'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
