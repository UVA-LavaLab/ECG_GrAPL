#!/usr/bin/env python3
"""Download, convert, and receipt the literature-scale final graph corpus."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
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
    command = [
        "curl", "--fail", "--location", "--retry", "5",
        "--retry-delay", "5", "--continue-at", "-",
        "--output", str(destination), url,
    ]
    subprocess.run(command, check=True)


def decompress_gzip(source: Path, destination: Path) -> None:
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    with gzip.open(source, "rb") as compressed, temporary.open("wb") as out:
        shutil.copyfileobj(compressed, out, 16 * 1024 * 1024)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, destination)


def convert(edge_list: Path, output: Path, symmetrize: bool) -> None:
    if not CONVERTER.is_file():
        raise SystemExit(
            f"missing converter {CONVERTER}; run make converter")
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    command = [
        str(CONVERTER), "-f", str(edge_list),
        *([] if not symmetrize else ["-s"]),
        "-b", str(temporary),
    ]
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)
    os.replace(temporary, output)


def graph_receipt(
        graph: dict[str, Any], graph_root: Path) -> dict[str, Any]:
    directory = graph_root / str(graph["name"])
    archive = directory / str(graph["archive"])
    edge_list = directory / str(graph["edge_list"])
    sg = directory / str(graph["sg"])
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
    temporary = receipt_path.with_suffix(receipt_path.suffix + ".tmp")
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

    if not args.receipt_only:
        for graph in selected:
            directory = graph_root / str(graph["name"])
            archive = directory / str(graph["archive"])
            edge_list = directory / str(graph["edge_list"])
            sg = directory / str(graph["sg"])
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
