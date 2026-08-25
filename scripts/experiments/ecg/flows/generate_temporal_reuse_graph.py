#!/usr/bin/env python3
"""Generate degree-identical graphs with clustered or spread PR readers."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def edges(vertices: int, degree: int, mode: str):
    if vertices <= degree or degree < 2:
        raise ValueError("vertices must exceed degree >= 2")
    if mode == "spread" and vertices % (4 * degree):
        raise ValueError(
            "spread mode requires vertices divisible by four times degree")
    spread_step = vertices // (4 * degree)
    for source in range(vertices):
        if mode == "clustered":
            destinations = (
                (source + offset) % vertices
                for offset in range(1, degree + 1)
            )
        else:
            destinations = (
                (source + (2 * offset + 1) * spread_step) % vertices
                for offset in range(degree)
            )
        for destination in destinations:
            yield source, destination


def generate(
        output: Path, metadata: Path,
        vertices: int, degree: int, mode: str) -> dict[str, object]:
    output.parent.mkdir(parents=True, exist_ok=True)
    indegree = [0] * vertices
    digest = hashlib.sha256()
    count = 0
    with output.open("w") as handle:
        for source, destination in edges(vertices, degree, mode):
            line = f"{source} {destination}\n"
            handle.write(line)
            digest.update(line.encode())
            indegree[destination] += 1
            count += 1
    if count != vertices * degree:
        raise RuntimeError("edge-count invariant failed")
    if any(value != degree for value in indegree):
        raise RuntimeError("in-degree invariant failed")
    receipt = {
        "schema": 1,
        "mode": mode,
        "vertices": vertices,
        "directed_edges": count,
        "out_degree": degree,
        "in_degree": degree,
        "sha256": digest.hexdigest(),
        "purpose": (
            "Degree-identical PageRank reader timing control; use -o 0 to "
            "preserve vertex order."
        ),
    }
    metadata.parent.mkdir(parents=True, exist_ok=True)
    metadata.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--vertices", type=int, default=65536)
    parser.add_argument("--degree", type=int, default=8)
    parser.add_argument(
        "--mode", choices=("clustered", "spread"), required=True)
    args = parser.parse_args()
    try:
        receipt = generate(
            args.output, args.metadata,
            args.vertices, args.degree, args.mode)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.error(str(exc))
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
