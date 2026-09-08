"""Bounded current-record graph inspection and conservative memory planning."""

from __future__ import annotations

from array import array
from dataclasses import asdict, dataclass
import hashlib
import os
from pathlib import Path
import struct
import sys
from functools import lru_cache
from typing import Literal

try:
    from .record_receipts import RecordReceiptError, UINT64_MAX, resolve_layout
except ImportError:
    from record_receipts import RecordReceiptError, UINT64_MAX, resolve_layout


class RecordResourceError(ValueError):
    pass


@dataclass(frozen=True)
class GraphInfo:
    directed: bool
    vertices: int
    records: int
    maximum_id: int
    storage_bytes: int
    sha256: str
    weighted: bool = False
    minimum_weight: int | None = None
    maximum_weight: int | None = None


def _signature(stat: os.stat_result) -> tuple[int, int, int, int]:
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def graph_info(
        path: Path, *, allow_weighted: bool = False,
        traversal: Literal["in", "out"] = "in") -> GraphInfo:
    if traversal not in ("in", "out"):
        raise RecordResourceError("graph traversal must be in or out")
    resolved = path.resolve()
    return _graph_info(str(resolved), _signature(resolved.stat()), allow_weighted, traversal)


@lru_cache(maxsize=32)
def _graph_info(
        path_text: str, signature: tuple[int, int, int, int],
        allow_weighted: bool, traversal: Literal["in", "out"]) -> GraphInfo:
    path = Path(path_text)
    if path.suffix not in ((".sg", ".wsg") if allow_weighted else (".sg",)):
        raise RecordResourceError(
            "graph preflight requires .sg or .wsg" if allow_weighted
            else "current record preflight requires an unweighted .sg input")
    weighted = path.suffix == ".wsg"
    edge_bytes = 8 if weighted else 4
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        header = handle.read(17)
        if len(header) != 17 or header[0] not in (0, 1):
            raise RecordResourceError("invalid serialized graph header")
        directed, records, vertices = struct.unpack("<?qq", header)
        if not 0 < vertices <= (1 << 31) - 1 or records <= 0:
            raise RecordResourceError("graph exceeds the nonempty signed-32 loader domain")
        expected = 17 + (2 if directed else 1) * (8 * (vertices + 1) + edge_bytes * records) + 4 * vertices
        if expected > UINT64_MAX or expected != signature[2]:
            raise RecordResourceError("serialized graph size disagrees with its header")
        digest.update(header)

        def chunks(count: int, code: str):
            width = struct.calcsize("<" + code)
            while count:
                items = min(count, (1 << 20) // width)
                data = handle.read(items * width)
                if len(data) != items * width:
                    raise RecordResourceError("truncated serialized graph payload")
                digest.update(data)
                values = array(code)
                values.frombytes(data)
                if sys.byteorder != "little":
                    values.byteswap()
                yield values
                count -= items

        maximum = 0
        minimum_weight: int | None = None
        maximum_weight: int | None = None
        encoded_direction = 1 if directed and traversal == "in" else 0
        for direction in range(2 if directed else 1):
            previous = 0
            position = 0
            for offsets in chunks(vertices + 1, "q"):
                for value in offsets:
                    if value < previous or value > records or (position == 0 and value != 0):
                        raise RecordResourceError("invalid serialized CSR offsets")
                    previous = value
                    position += 1
            if previous != records:
                raise RecordResourceError("serialized CSR does not cover every record")
            for entries in chunks(records * (2 if weighted else 1), "i"):
                ids = entries[::2] if weighted else entries
                low, high = min(ids), max(ids)
                if low < 0 or high >= vertices:
                    raise RecordResourceError("serialized destination lies outside the graph domain")
                if direction == encoded_direction:
                    maximum = max(maximum, high)
                if weighted:
                    weights = entries[1::2]
                    low_weight, high_weight = min(weights), max(weights)
                    minimum_weight = low_weight if minimum_weight is None else min(minimum_weight, low_weight)
                    maximum_weight = high_weight if maximum_weight is None else max(maximum_weight, high_weight)
        for _ids in chunks(vertices, "i"):
            pass
        if _signature(os.fstat(handle.fileno())) != signature or _signature(path.stat()) != signature:
            raise RecordResourceError("graph changed during resource preflight")
    return GraphInfo(
        directed, vertices, records, maximum, expected, digest.hexdigest(),
        weighted, minimum_weight, maximum_weight)


def plan_resources(
    graph: GraphInfo, *, traversals: int, requested_bytes: int,
    minimum_mantissa_bits: int, carrier_limit: int, auxiliary_limit: int,
    rss_mib: int, backend: str, target_memory_bytes: int = 0,
    equivalence: bool = False,
) -> dict[str, int | str | bool | None]:
    if min(carrier_limit, auxiliary_limit, rss_mib) <= 0:
        raise RecordResourceError("record allocation and RSS limits must be positive")
    if max(carrier_limit, auxiliary_limit) > UINT64_MAX:
        raise RecordResourceError("record allocation limit exceeds 64-bit accounting")
    if equivalence and (graph.vertices > 4096 or graph.records > 65536):
        raise RecordResourceError("equivalence observation is limited to bounded small graphs")
    try:
        layout = resolve_layout(
            records=graph.records, vertices=graph.vertices, maximum_id=graph.maximum_id,
            traversals=traversals, requested_bytes=requested_bytes,
            minimum_mantissa_bits=minimum_mantissa_bits)
    except RecordReceiptError as error:
        raise RecordResourceError(str(error)) from error
    carrier = graph.records * int(layout["record_bytes"])
    if carrier > carrier_limit:
        raise RecordResourceError(f"record carrier needs {carrier} bytes, exceeding {carrier_limit}")
    # The auxiliary cap is a conservative reservation, not a prediction of its exact use.
    planned = graph.storage_bytes + carrier + max(auxiliary_limit, 12 * graph.vertices) + (256 << 20)
    if planned > UINT64_MAX:
        raise RecordResourceError("planned graph/record memory exceeds 64-bit accounting")
    host_planned = planned + (graph.storage_bytes if backend == "gem5" else 0)
    if host_planned > rss_mib * 1024 * 1024:
        raise RecordResourceError(
            f"conservative host memory plan needs {host_planned} bytes; increase the explicit RSS budget")
    if backend == "gem5" and (target_memory_bytes <= 0 or planned > target_memory_bytes):
        raise RecordResourceError(
            f"conservative target memory plan needs {planned} bytes, exceeding gem5 memory {target_memory_bytes}")
    return {
        **asdict(graph),
        **layout,
        "carrier_payload_bytes": carrier,
        "maximum_carrier_bytes": carrier_limit,
        "maximum_auxiliary_bytes": auxiliary_limit,
        "planned_target_bytes": planned,
        "planned_host_bytes": host_planned,
        "rss_limit_mib": rss_mib,
        "gem5_target_memory_bytes": target_memory_bytes if backend == "gem5" else 0,
        "memory_plan": "conservative-reservation",
    }


def plan_algorithm_resources(
    graph: GraphInfo, *, algorithm: str, records: bool,
    requested_bytes: int, minimum_mantissa_bits: int,
    traversals: int, sources: int, workspace_limit: int,
    carrier_limit: int, auxiliary_limit: int, rss_mib: int,
    backend: str = "cache_sim", target_memory_bytes: int = 0,
    bfs_direction_optimizing: bool = False,
    preprocessing: str = "csr",
) -> dict[str, int | str | bool | None]:
    coefficients = {"spmv": 8, "bfs": 12, "sssp": 25, "cc": 12, "bc": 40, "tc": 24}
    if algorithm not in coefficients or min(
            traversals, sources, workspace_limit, carrier_limit, auxiliary_limit, rss_mib) <= 0:
        raise RecordResourceError("invalid algorithm or resource limits")
    if requested_bytes not in (0, 4, 8) or not 0 <= minimum_mantissa_bits <= 61:
        raise RecordResourceError("invalid algorithm record layout request")
    if preprocessing not in ("csr", "traversal"):
        raise RecordResourceError("invalid algorithm preprocessing")
    if algorithm in ("cc", "tc") and (graph.directed or graph.records % 2):
        raise RecordResourceError("CC and TC require a simple undirected input")
    if algorithm == "sssp" and graph.minimum_weight is not None and graph.minimum_weight < 0:
        raise RecordResourceError("SSSP requires nonnegative weights")
    carrier_records = graph.records // 2 if algorithm == "tc" else graph.records
    property_bytes = 8 if algorithm in ("sssp", "tc") else 4
    arrays = coefficients[algorithm] * graph.vertices
    if bfs_direction_optimizing:
        if algorithm != "bfs":
            raise RecordResourceError("direction optimization requires BFS")
        arrays += 16 * ((graph.vertices + 63) // 64)
    if algorithm == "bc":
        arrays += 16 + 4 * sources
    if algorithm == "tc":
        arrays += 8 + 4 * carrier_records
    layout = {}
    if records and algorithm != "tc":
        try:
            layout = resolve_layout(
                records=carrier_records, vertices=graph.vertices, maximum_id=graph.maximum_id,
                traversals=traversals, requested_bytes=requested_bytes,
                minimum_mantissa_bits=minimum_mantissa_bits)
        except RecordReceiptError as error:
            raise RecordResourceError(str(error)) from error
    # TC's oriented target IDs are known only after its charged orientation.
    width_upper = int(layout.get("record_bytes", requested_bytes or 8))
    carrier = carrier_records * width_upper if records else 0
    lines = min(carrier_records, (graph.vertices * property_bytes + 63) // 64)
    slots = 1 << max(0, (2 * lines - 1).bit_length())
    partitions = {"sssp": 2, "cc": 3}.get(algorithm, 1) if preprocessing == "traversal" else 1
    scratch = (8 + 16 * partitions) * slots if records else 0
    if carrier > carrier_limit or scratch > auxiliary_limit or arrays + carrier + scratch > workspace_limit:
        raise RecordResourceError("algorithm arrays/carrier/construction exceed their explicit limits")
    directions = 2 if graph.directed else 1
    graph_peak = (directions + 1) * 8 * (graph.vertices + 1) + 12 * graph.vertices + (
        directions * graph.records * (8 if graph.weighted else 4))
    planned = graph_peak + workspace_limit + (256 << 20)
    host = planned + (graph.storage_bytes if backend == "gem5" else 0)
    if max(graph_peak, planned, host, workspace_limit, carrier_limit, auxiliary_limit) > UINT64_MAX:
        raise RecordResourceError("algorithm memory accounting exceeds uint64")
    if host > rss_mib * (1 << 20):
        raise RecordResourceError("algorithm memory reservation exceeds the process-tree RSS budget")
    if backend == "gem5" and (target_memory_bytes <= 0 or planned > target_memory_bytes):
        raise RecordResourceError("algorithm memory reservation exceeds gem5 target memory")
    return {
        **asdict(graph), **layout,
        "algorithm": algorithm, "carrier_records_upper": carrier_records,
        "carrier_payload_bytes_upper": carrier, "array_bytes": arrays,
        "construction_auxiliary_bytes_upper": scratch, "graph_loader_bytes_upper": graph_peak,
        "record_preprocess": preprocessing, "construction_partitions": partitions,
        "workspace_limit_bytes": workspace_limit, "planned_host_bytes": host,
        "planned_target_bytes": planned, "rss_limit_mib": rss_mib,
        "memory_plan": "algorithm-conservative-reservation",
    }
