#!/usr/bin/env python3
"""Generate the ReusePlan/P-OPT three-cost accounting table."""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_GRAPHS = {
    name: PROJECT_ROOT / "results/graphs" / name / f"{name}.sg"
    for name in ("web-Google", "soc-pokec", "cit-Patents")
}


@dataclass(frozen=True)
class GraphInfo:
    name: str
    path: str
    directed: bool
    vertices: int
    serialized_edges: int


TRANSPORTS = {
    "unweighted_reuse_plan": {
        "baseline_edge_bytes": 4,
        "reuse_plan_edge_bytes": 8,
        "description": "PR/BFS/BC/CC 8B ReusePlan record replaces 4B destination",
    },
    "weighted_compact_reuse_plan": {
        "baseline_edge_bytes": 8,
        "reuse_plan_edge_bytes": 8,
        "description": "compact SSSP record replaces 8B weighted edge",
    },
    "weighted_fallback_reuse_plan": {
        "baseline_edge_bytes": 8,
        "reuse_plan_edge_bytes": 12,
        "description": "general SSSP keeps 8B edge plus 4B ReusePlan sidecar",
    },
}


def parse_size(text: str) -> int:
    value = text.strip().lower()
    units = (
        ("gib", 1024**3), ("gb", 1024**3), ("g", 1024**3),
        ("mib", 1024**2), ("mb", 1024**2), ("m", 1024**2),
        ("kib", 1024), ("kb", 1024), ("k", 1024),
        ("b", 1),
    )
    for suffix, multiplier in units:
        if value.endswith(suffix):
            number = value[:-len(suffix)]
            return int(number) * multiplier
    return int(value)


def read_sg(path: Path, name: str | None = None) -> GraphInfo:
    with path.open("rb") as handle:
        data = handle.read(17)
    if len(data) != 17:
        raise ValueError(f"invalid GAPBS serialized header: {path}")
    directed = bool(data[0])
    serialized_edges = struct.unpack_from("<q", data, 1)[0]
    vertices = struct.unpack_from("<q", data, 9)[0]
    if vertices <= 0 or serialized_edges < 0:
        raise ValueError(f"invalid graph counts in {path}")
    return GraphInfo(
        name=name or path.stem,
        path=str(path),
        directed=directed,
        vertices=vertices,
        serialized_edges=serialized_edges,
    )


def cost_rows(
        graph: GraphInfo, cache_bytes: int, *,
        ways: int = 16, line_bytes: int = 64,
        minimum_reuse_plan_line_bits: int = 33,
        contextual_reuse_plan_line_bits: int = 49,
        popt_property_bytes: int = 4,
        popt_active_columns: int = 2,
        popt_min_data_ways: int = 1) -> list[dict[str, Any]]:
    if cache_bytes <= 0 or ways <= 0 or line_bytes <= 0:
        raise ValueError("cache geometry must be positive")
    if cache_bytes % (ways * line_bytes):
        raise ValueError(
            "cache size must be divisible by ways * line bytes")
    if popt_property_bytes <= 0 or popt_active_columns <= 0:
        raise ValueError("P-OPT geometry must be positive")

    lines = cache_bytes // line_bytes
    sets = cache_bytes // (ways * line_bytes)
    bytes_per_way = sets * line_bytes
    popt_property_lines = math.ceil(
        graph.vertices * popt_property_bytes / line_bytes)
    popt_matrix_bytes = popt_active_columns * popt_property_lines
    popt_needed_ways = math.ceil(popt_matrix_bytes / bytes_per_way)
    popt_max_reservable = max(ways - popt_min_data_ways, 0)
    popt_fits = popt_needed_ways <= popt_max_reservable
    popt_reserved_ways = min(popt_needed_ways, popt_max_reservable)

    minimum_metadata_bytes = math.ceil(
        lines * minimum_reuse_plan_line_bits / 8)
    contextual_metadata_bytes = math.ceil(
        lines * contextual_reuse_plan_line_bits / 8)

    common = {
        **asdict(graph),
        "cache_bytes": cache_bytes,
        "cache_mib": cache_bytes / 1024**2,
        "baseline_ways": ways,
        "line_bytes": line_bytes,
        "cache_lines": lines,
        "bytes_per_way": bytes_per_way,
        "reuse_plan_minimum_metadata_bits_per_line": minimum_reuse_plan_line_bits,
        "reuse_plan_contextual_metadata_bits_per_line":
            contextual_reuse_plan_line_bits,
        "reuse_plan_minimum_metadata_bytes": minimum_metadata_bytes,
        "reuse_plan_contextual_metadata_bytes": contextual_metadata_bytes,
        "reuse_plan_contextual_way_equivalent":
            contextual_metadata_bytes / bytes_per_way,
        "reuse_plan_cost_unit":
            "added metadata SRAM area expressed as baseline-way equivalent",
        "popt_property_bytes": popt_property_bytes,
        "popt_active_columns": popt_active_columns,
        "popt_property_lines": popt_property_lines,
        "popt_matrix_bytes": popt_matrix_bytes,
        "popt_needed_ways": popt_needed_ways,
        "popt_reserved_ways": popt_reserved_ways,
        "popt_cost_unit": "reserved LLC data ways (capacity loss)",
        "popt_effective_data_ways": ways - popt_reserved_ways,
        "popt_matrix_fits": int(popt_fits),
        "popt_capacity_note": (
            "exact runner size-correct charge for one 4B property array"),
    }
    rows: list[dict[str, Any]] = []
    for transport, values in TRANSPORTS.items():
        extra = values["reuse_plan_edge_bytes"] - values["baseline_edge_bytes"]
        rows.append({
            **common,
            "transport": transport,
            **values,
            "reuse_plan_extra_bytes_per_edge": extra,
            "reuse_plan_extra_active_stream_bytes":
                graph.serialized_edges * extra,
            "transport_scope":
                "one active traversal-direction edge stream",
            "weighted_transport_note": (
                "weighted rows apply if this topology is represented with "
                "the stated weighted-edge format"),
        })
    return rows


def markdown(rows: list[dict[str, Any]]) -> str:
    lines = [
        "| Graph | LLC | Transport | Extra B/edge | Extra active-stream MiB | "
        "ReusePlan bits/line | Added SRAM way-eq | P-OPT matrix MiB | "
        "Reserved data ways | Fits |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['cache_mib']:.0f} MiB | "
            f"{row['transport']} | {row['reuse_plan_extra_bytes_per_edge']} | "
            f"{row['reuse_plan_extra_active_stream_bytes'] / 1024**2:.2f} | "
            f"{row['reuse_plan_contextual_metadata_bits_per_line']} | "
            f"{row['reuse_plan_contextual_way_equivalent']:.3f} | "
            f"{row['popt_matrix_bytes'] / 1024**2:.3f} | "
            f"{row['popt_reserved_ways']} | "
            f"{row['popt_matrix_fits']} |")
    return "\n".join(lines)


def parse_graph(value: str) -> tuple[str, Path]:
    if "=" not in value:
        path = Path(value)
        return path.stem, path
    name, raw_path = value.split("=", 1)
    return name, Path(raw_path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate ReusePlan transport/metadata/P-OPT capacity table.")
    parser.add_argument(
        "--graph", action="append", default=[],
        help="Graph as NAME=PATH.sg; repeat for multiple graphs.")
    parser.add_argument(
        "--cache-sizes", nargs="+", default=["2MB", "8MB"])
    parser.add_argument("--ways", type=int, default=16)
    parser.add_argument("--line-bytes", type=int, default=64)
    parser.add_argument("--popt-property-bytes", type=int, default=4)
    parser.add_argument("--popt-active-columns", type=int, default=2)
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-csv", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    graph_args = (
        [f"{name}={path}" for name, path in DEFAULT_GRAPHS.items()]
        if not args.graph else args.graph)
    graphs = [
        read_sg(path if path.is_absolute() else PROJECT_ROOT / path, name)
        for name, path in map(parse_graph, graph_args)
    ]
    rows = [
        row
        for graph in graphs
        for cache_size in args.cache_sizes
        for row in cost_rows(
            graph, parse_size(cache_size),
            ways=args.ways,
            line_bytes=args.line_bytes,
            popt_property_bytes=args.popt_property_bytes,
            popt_active_columns=args.popt_active_columns)
    ]
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(
            json.dumps(rows, indent=2, sort_keys=True) + "\n")
    if args.out_csv:
        args.out_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.out_csv.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=sorted(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(markdown(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
