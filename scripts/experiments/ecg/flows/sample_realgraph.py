#!/usr/bin/env python3
"""Create a deterministic compact real-graph sample for simulator runs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterator


def iter_edges(path: Path) -> Iterator[tuple[int, int]]:
    matrix_market = path.suffix.lower() == ".mtx"
    dimensions_seen = not matrix_market
    with path.open() as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith(("#", "%")):
                continue
            fields = line.split()
            if matrix_market and not dimensions_seen:
                if len(fields) < 3:
                    raise RuntimeError(
                        f"invalid MatrixMarket dimensions in {path}: {line!r}")
                dimensions_seen = True
                continue
            if len(fields) < 2:
                continue
            yield int(fields[0]), int(fields[1])


def select_vertices(path: Path, target_vertices: int) -> set[int]:
    selected: set[int] = set()
    for src, dst in iter_edges(path):
        new_vertices = {src, dst} - selected
        if len(selected) + len(new_vertices) > target_vertices:
            continue
        selected.update(new_vertices)
        if len(selected) == target_vertices:
            return selected
    raise RuntimeError(
        f"{path} contains only {len(selected)} selectable vertices; "
        f"requested {target_vertices}")


def write_sample(
        source: Path, output: Path, vertices_path: Path,
        metadata_path: Path, target_vertices: int,
        target_edges: int | None = None) -> dict[str, object]:
    selected = select_vertices(source, target_vertices)
    out_degree = {vertex: 0 for vertex in selected}
    covered: set[int] = set()
    coverage_indices: set[int] = set()
    induced_edges = 0
    for src, dst in iter_edges(source):
        if src in selected and dst in selected:
            out_degree[src] += 1
            if src not in covered or dst not in covered:
                coverage_indices.add(induced_edges)
                covered.update((src, dst))
            induced_edges += 1
    if covered != selected:
        missing = len(selected - covered)
        raise RuntimeError(
            f"{source} induced sample leaves {missing} vertices uncovered")
    edge_budget = (
        induced_edges if target_edges is None
        else min(target_edges, induced_edges))
    if edge_budget < len(coverage_indices):
        raise RuntimeError(
            f"edge budget {edge_budget} cannot cover all "
            f"{target_vertices} sampled vertices; "
            f"requires at least {len(coverage_indices)} edges")
    root = min(
        out_degree,
        key=lambda vertex: (-out_degree[vertex], vertex))
    ordered = [root]
    ordered.extend(vertex for vertex in sorted(selected) if vertex != root)
    remap = {vertex: index for index, vertex in enumerate(ordered)}

    output.parent.mkdir(parents=True, exist_ok=True)
    edge_count = 0
    induced_index = 0
    noncoverage_budget = edge_budget - len(coverage_indices)
    noncoverage_written = 0
    with output.open("w") as handle:
        for src, dst in iter_edges(source):
            local_src = remap.get(src)
            local_dst = remap.get(dst)
            if local_src is None or local_dst is None:
                continue
            keep = induced_index in coverage_indices
            if not keep and noncoverage_written < noncoverage_budget:
                keep = True
                noncoverage_written += 1
            induced_index += 1
            if not keep:
                continue
            handle.write(f"{local_src}\t{local_dst}\n")
            edge_count += 1
    if edge_count != edge_budget:
        raise RuntimeError(
            f"sample edge count mismatch: {edge_count} != {edge_budget}")

    vertices_path.write_text(
        "".join(f"{local}\t{original}\n"
                for local, original in enumerate(ordered)))
    metadata = {
        "source": str(source.resolve()),
        "output": str(output.resolve()),
        "selection": (
            "edge-prefix vertex set, deterministic coverage edges, "
            "then edge-prefix fill"),
        "vertices": len(ordered),
        "edges": edge_count,
        "induced_edges_before_budget": induced_edges,
        "target_edges": target_edges,
        "coverage_edges": len(coverage_indices),
        "edge_count_semantics": (
            "directed arcs in the sampled .el before optional converter "
            "symmetrization"),
        "symmetrized": False,
        "root_original_vertex": root,
        "root_out_degree": out_degree[root],
        "original_vertex_min": min(ordered),
        "original_vertex_max": max(ordered),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a deterministic compact real-graph edge list.")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--vertices", required=True)
    parser.add_argument("--metadata", required=True)
    parser.add_argument("--target-vertices", type=int, default=65536)
    parser.add_argument(
        "--target-edges", type=int, default=0,
        help="Maximum directed arcs before symmetrization; 0 keeps all induced arcs.")
    args = parser.parse_args()

    if args.target_vertices < 2:
        raise SystemExit("--target-vertices must be at least 2")
    if args.target_edges < 0:
        raise SystemExit("--target-edges must be non-negative")
    metadata = write_sample(
        Path(args.input), Path(args.output), Path(args.vertices),
        Path(args.metadata), args.target_vertices,
        args.target_edges or None)
    print(
        f"[sample-realgraph] vertices={metadata['vertices']} "
        f"edges={metadata['edges']} output={metadata['output']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
