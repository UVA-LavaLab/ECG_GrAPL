#!/usr/bin/env python3
"""Prepare the bounded, deterministic graph corpus for current ECG equivalence."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[4]
CONFIG_PATH = (
    PROJECT_ROOT / "scripts/experiments/ecg/configs/record_equivalence.json")
FIXTURE_PATH = PROJECT_ROOT / "fig/ecg-figure-fixture.json"
TEMPORAL_GENERATOR = Path(__file__).with_name("generate_temporal_reuse_graph.py")
OUTPUT_ROOT = PROJECT_ROOT / "results/graphs/ecg-current-equivalence"
ALGORITHM_OUTPUT_ROOT = PROJECT_ROOT / "results/graphs/ecg-algorithm-equivalence"
ALGORITHM_CONFIG = PROJECT_ROOT / "scripts/experiments/ecg/configs/algorithm_equivalence.json"
MAX_VERTICES = 4096
MAX_EDGES = 1_000_000


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def display_path(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(PROJECT_ROOT))
    except ValueError:
        return str(path.resolve())


def load_temporal_edges():
    spec = importlib.util.spec_from_file_location(
        "ecg_temporal_generator", TEMPORAL_GENERATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load temporal graph generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.edges


def normalized_edges(
        vertices: int, edges: Iterable[tuple[int, int]],
        directed: bool) -> tuple[list[list[int]], list[list[int]] | None, int]:
    if vertices <= 0 or vertices > MAX_VERTICES:
        raise ValueError(f"vertices must be in [1,{MAX_VERTICES}]")
    out_rows = [[] for _ in range(vertices)]
    in_rows = [[] for _ in range(vertices)] if directed else None
    edge_count = 0
    for source, destination in edges:
        if not (0 <= source < vertices and 0 <= destination < vertices):
            raise ValueError("edge endpoint is outside the declared graph")
        out_rows[source].append(destination)
        if directed:
            assert in_rows is not None
            in_rows[destination].append(source)
        edge_count += 1
        if edge_count > MAX_EDGES:
            raise ValueError(f"edge count exceeds bounded limit {MAX_EDGES}")
    for row in out_rows:
        row.sort()
    if in_rows is not None:
        for row in in_rows:
            row.sort()
    return out_rows, in_rows, edge_count


def encode_csr(rows: list[list[int]]) -> tuple[bytes, bytes]:
    offsets = [0]
    neighbors: list[int] = []
    for row in rows:
        neighbors.extend(row)
        offsets.append(len(neighbors))
    return (
        b"".join(struct.pack("<q", value) for value in offsets),
        b"".join(struct.pack("<i", value) for value in neighbors),
    )


def serialized_graph(
        vertices: int, edges: Iterable[tuple[int, int]],
        directed: bool, *, traversal: str = "in") -> tuple[bytes, dict[str, int | bool | str]]:
    if traversal not in ("in", "out"):
        raise ValueError("serialized traversal must be in or out")
    out_rows, in_rows, edge_count = normalized_edges(
        vertices, edges, directed)
    out_offsets, out_neighbors = encode_csr(out_rows)
    payload = bytearray(struct.pack("<?qq", directed, edge_count, vertices))
    payload.extend(out_offsets)
    payload.extend(out_neighbors)
    if directed:
        assert in_rows is not None
        in_offsets, in_neighbors = encode_csr(in_rows)
        payload.extend(in_offsets)
        payload.extend(in_neighbors)
    payload.extend(
        b"".join(struct.pack("<i", vertex) for vertex in range(vertices)))
    encoded_rows = in_rows if directed and traversal == "in" else out_rows
    assert encoded_rows is not None
    maximum_id = max(
        (source for row in encoded_rows for source in row), default=0)
    semantic = hashlib.sha256()
    semantic.update(struct.pack("<?qq", directed, edge_count, vertices))
    for source, row in enumerate(out_rows):
        for destination in row:
            semantic.update(struct.pack("<ii", source, destination))
    return bytes(payload), {
        "directed": directed,
        "vertices": vertices,
        "records": edge_count,
        "max_vertex_id": maximum_id,
        "semantic_sha256": semantic.hexdigest(),
    }

def serialized_weighted_graph(
        vertices: int, edges: Iterable[tuple[int, int, int]],
        directed: bool) -> tuple[bytes, dict[str, object]]:
    if not 1 <= vertices <= MAX_VERTICES:
        raise ValueError(f"vertices must be in [1,{MAX_VERTICES}]")
    out_rows: list[list[tuple[int, int]]] = [[] for _ in range(vertices)]
    in_rows: list[list[tuple[int, int]]] = [[] for _ in range(vertices)]
    count = 0
    for source, destination, weight in edges:
        if not (0 <= source < vertices and 0 <= destination < vertices):
            raise ValueError("weighted endpoint is outside the graph")
        if not -(1 << 31) <= weight < (1 << 31):
            raise ValueError("weight is outside signed-32")
        out_rows[source].append((destination, weight))
        in_rows[destination].append((source, weight))
        count += 1
        if count > MAX_EDGES:
            raise ValueError("weighted graph exceeds the bounded edge limit")
    payload = bytearray(struct.pack("<?qq", directed, count, vertices))
    for rows in (out_rows, in_rows) if directed else (out_rows,):
        offsets = [0]
        records = bytearray()
        for row in rows:
            row.sort()
            records.extend(b"".join(struct.pack("<ii", *edge) for edge in row))
            offsets.append(offsets[-1] + len(row))
        payload.extend(b"".join(struct.pack("<q", offset) for offset in offsets))
        payload.extend(records)
    payload.extend(b"".join(struct.pack("<i", vertex) for vertex in range(vertices)))
    return bytes(payload), {
        "vertices": vertices, "records": count, "directed": directed, "weighted": True,
        "maximum_out_id": max((v for row in out_rows for v, _ in row), default=0),
    }


def algorithm_outputs() -> dict[str, tuple[bytes, dict[str, object]]]:
    diamond = [
        (0, 1, 2), (0, 2, 5), (0, 5, 20), (1, 2, 1), (1, 3, 2),
        (2, 3, 1), (2, 4, 4), (3, 4, 1), (4, 5, 3), (6, 7, 1),
    ]
    cliques = [
        (0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3),
        (4, 5), (4, 6), (5, 6), (7, 8),
    ]
    symmetric = [(u, v) for left, right in cliques for u, v in ((left, right), (right, left))]
    pressure: set[tuple[int, int]] = set()
    for first, size in ((0, 384), (384, 112)):
        for vertex in range(first, first + size):
            for step in (1, 2, 17, 65):
                target = first + (vertex - first + step) % size
                pressure.add((min(vertex, target), max(vertex, target)))
    pressure_edges = [
        (u, v, (left * 13 + right * 7) % 32)
        for left, right in sorted(pressure)
        for u, v in ((left, right), (right, left))
    ]
    outputs = {
        "weighted-diamond.wsg": serialized_weighted_graph(8, diamond, True),
        "diamond.sg": serialized_graph(8, ((u, v) for u, v, _ in diamond), True, traversal="out"),
        "cliques-and-isolate.sg": serialized_graph(10, symmetric, False, traversal="out"),
        "pressure512.wsg": serialized_weighted_graph(512, pressure_edges, False),
        "pressure512.sg": serialized_graph(
            512, ((u, v) for u, v, _ in pressure_edges), False, traversal="out"),
    }
    for _data, facts in outputs.values():
        if "max_vertex_id" in facts:
            facts["maximum_out_id"] = facts.pop("max_vertex_id")
        facts["traversal"] = "out"
    return outputs


def prepare_algorithms(output_root: Path, force: bool, check: bool) -> dict[str, object]:
    config = json.loads(ALGORITHM_CONFIG.read_text())
    outputs = algorithm_outputs()
    graphs = {}
    for name, (data, facts) in outputs.items():
        digest = sha256_bytes(data)
        if digest != config["graphs"][name]["sha256"]:
            raise RuntimeError(f"algorithm recipe differs from pinned graph {name}")
        graphs[name] = {
            **facts, "sha256": digest, "path": display_path(output_root / name),
            "size_bytes": len(data),
        }
    receipt = {
        "schema": "ecg-algorithm-corpus.v1", "graphs": graphs,
        "config_sha256": sha256_path(ALGORITHM_CONFIG),
        "preparer_sha256": sha256_path(Path(__file__)),
    }
    encoded = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode()
    artifacts = {name: data for name, (data, _) in outputs.items()}
    artifacts["corpus.receipt.json"] = encoded
    for name, data in artifacts.items():
        path = output_root / name
        if check:
            if not path.is_file() or path.read_bytes() != data:
                raise RuntimeError(f"algorithm corpus artifact is missing or changed: {path}")
        elif path.exists() and path.read_bytes() != data and not force:
            raise RuntimeError(f"refusing to overwrite changed algorithm artifact {path}")
    if not check:
        for name, data in artifacts.items():
            write_checked(output_root / name, data, force)
    return receipt


def fixture_graph() -> tuple[bytes, dict[str, object]]:
    raw = json.loads(FIXTURE_PATH.read_text())
    mapping = [int(value) for value in raw["source_to_internal"]]
    edges: list[tuple[int, int]] = []
    for left, right, _weight in raw["weighted_undirected_edges"]:
        source, destination = mapping[left], mapping[right]
        edges.extend(((source, destination), (destination, source)))
    data, facts = serialized_graph(int(raw["num_vertices"]), edges, False)
    if (facts["directed"] is not False or facts["vertices"] != 32 or
            facts["records"] != 34 or facts["max_vertex_id"] != 20):
        raise RuntimeError("fixture graph invariants changed")
    return data, {
        **facts,
        "recipe": "ecg-figure-fixture mapped undirected CSR",
        "recipe_inputs": {
            str(FIXTURE_PATH.relative_to(PROJECT_ROOT)):
                sha256_path(FIXTURE_PATH),
        },
    }


def spread_graph() -> tuple[bytes, dict[str, object]]:
    vertices, degree = 512, 4
    edges = list(load_temporal_edges()(vertices, degree, "spread"))
    data, facts = serialized_graph(vertices, edges, True)
    if facts["records"] != vertices * degree or facts["max_vertex_id"] != 511:
        raise RuntimeError("temporal spread graph invariants changed")
    return data, {
        **facts,
        "recipe": "generate_temporal_reuse_graph.edges",
        "parameters": {
            "vertices": vertices,
            "degree": degree,
            "mode": "spread",
        },
        "recipe_inputs": {
            str(TEMPORAL_GENERATOR.relative_to(PROJECT_ROOT)):
                sha256_path(TEMPORAL_GENERATOR),
        },
    }


def expected_outputs() -> dict[str, tuple[bytes, dict[str, object]]]:
    return {
        "fixture32.sg": fixture_graph(),
        "spread512-d4.sg": spread_graph(),
    }


def write_checked(path: Path, data: bytes, force: bool) -> None:
    if path.exists() and path.read_bytes() != data and not force:
        raise RuntimeError(
            f"refusing to overwrite changed corpus artifact {path}; use --force")
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_bytes(data)
    temporary.replace(path)


def prepare(output_root: Path, force: bool, check: bool) -> dict[str, object]:
    config = json.loads(CONFIG_PATH.read_text())
    outputs = expected_outputs()
    graphs: dict[str, object] = {}
    for name, (data, facts) in outputs.items():
        path = output_root / name
        digest = sha256_bytes(data)
        configured = next(
            tier for tier in config["tiers"].values()
            if Path(tier["path"]).name == name)
        expected = str(configured.get("sha256", ""))
        if not expected:
            raise RuntimeError(f"missing pinned sha256 for {name}")
        if digest != expected:
            raise RuntimeError(
                f"recipe hash for {name} is {digest}, expected {expected}")
        graphs[name] = {
            **facts,
            "path": display_path(path),
            "sha256": digest,
            "size_bytes": len(data),
        }
    receipt = {
        "schema": "ecg-current-equivalence-corpus",
        "schema_version": 1,
        "config_sha256": sha256_path(CONFIG_PATH),
        "preparer_sha256": sha256_path(Path(__file__)),
        "graphs": graphs,
    }
    receipt_path = output_root / "corpus.receipt.json"
    encoded = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode()
    if check:
        for name, (data, _facts) in outputs.items():
            path = output_root / name
            if not path.is_file() or path.read_bytes() != data:
                raise RuntimeError(
                    f"prepared graph is missing or changed: {path}")
        if not receipt_path.is_file() or receipt_path.read_bytes() != encoded:
            raise RuntimeError(f"corpus receipt is missing or changed: {receipt_path}")
        return receipt
    if not force:
        for name, (data, _facts) in outputs.items():
            path = output_root / name
            if path.exists() and path.read_bytes() != data:
                raise RuntimeError(
                    f"refusing to overwrite changed corpus artifact {path}; "
                    "use --force")
        if receipt_path.exists() and receipt_path.read_bytes() != encoded:
            raise RuntimeError(
                f"refusing to overwrite changed corpus artifact {receipt_path}; "
                "use --force")
    for name, (data, _facts) in outputs.items():
        write_checked(output_root / name, data, force)
    write_checked(receipt_path, encoded, force)
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--algorithms", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if args.check and args.force:
        parser.error("--check and --force are mutually exclusive")
    requested_root = args.output_root or (ALGORITHM_OUTPUT_ROOT if args.algorithms else OUTPUT_ROOT)
    output_root = requested_root if requested_root.is_absolute() else PROJECT_ROOT / requested_root
    try:
        receipt = (prepare_algorithms if args.algorithms else prepare)(
            output_root, args.force, args.check)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
