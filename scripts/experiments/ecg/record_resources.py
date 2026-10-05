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
        extrema: list[tuple[int, int]] = []
        encoded_direction = 1 if directed and traversal == "in" else 0
        for direction in range(2 if directed else 1):
            direction_minimum: int | None = None
            direction_maximum: int | None = None
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
                    direction_minimum = low_weight if direction_minimum is None else min(direction_minimum, low_weight)
                    direction_maximum = high_weight if direction_maximum is None else max(direction_maximum, high_weight)
            if weighted:
                extrema.append((direction_minimum, direction_maximum))
        # A record's weight lane is sized from the stream it encodes; both
        # directions of one graph carry the same weights.
        if len(set(extrema)) > 1:
            raise RecordResourceError("serialized directions disagree on their weights")
        minimum_weight, maximum_weight = extrema[0] if extrema else (None, None)
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


def popt_matrix_lines(algorithm: str, vertices: int) -> int:
    widths = {"spmv": (4,), "bfs": (4,), "sssp": (8,), "cc": (4,),
              "bc": (4, 8, 4), "tc": (8,)}
    if algorithm not in widths:
        raise RecordResourceError("unsupported P-OPT algorithm")
    return sum((vertices * width + 63) // 64 for width in widths[algorithm])


def popt_reservation(column_bytes: int, *, l3_bytes: int, l3_ways: int, line_size: int = 64,
                     active_columns: int = 2, min_data_ways: int = 1) -> dict[str, int | bool]:
    """P-OPT's size-correct reservation, for every runner (NEXT.md §bw).

    Its resident rereference-matrix columns, one byte per property line each, occupy whole last-level ways, and at
    least ``min_data_ways`` stay for data. Columns that need more make the cell infeasible: the clamped geometry is
    then only a P-OPT-favourable sensitivity.
    """
    if min(column_bytes, l3_bytes, l3_ways, line_size, active_columns, min_data_ways) <= 0:
        raise RecordResourceError("invalid P-OPT reservation geometry")
    min_data_ways = min(min_data_ways, l3_ways)
    sets = max(l3_bytes // (l3_ways * line_size), 1)
    bytes_per_way = sets * line_size
    matrix_bytes = active_columns * column_bytes
    needed_ways = (matrix_bytes + bytes_per_way - 1) // bytes_per_way
    reservable = l3_ways - min_data_ways
    reserved_ways = min(needed_ways, reservable)
    return {"matrix_bytes": matrix_bytes, "bytes_per_way": bytes_per_way, "needed_ways": needed_ways,
            "reserved_ways": reserved_ways, "effective_ways": l3_ways - reserved_ways,
            "effective_bytes": sets * (l3_ways - reserved_ways) * line_size, "fits": needed_ways <= reservable}


def record_weight_bits(graph: GraphInfo, algorithm: str) -> int:
    """The weight lane of a record carrier (ecg_record.h): the kernels that read
    edge weights carry them, max(1, bit width of the largest int32 weight's uint32
    pattern); a negative weight sets bit 31. Every other carrier has none."""
    if not graph.weighted or algorithm not in ("sssp", "spmv"):
        return 0
    if graph.minimum_weight is None or graph.maximum_weight is None:
        raise RecordResourceError("a weighted carrier needs its weight extrema")
    if graph.minimum_weight < 0:
        return 32
    return max(1, graph.maximum_weight.bit_length())


def window_layout(maximum_id: int, requested_bytes: int) -> dict[str, int]:
    if not 0 <= maximum_id <= (1 << 32) - 1 or requested_bytes not in (0, 4, 8):
        raise RecordResourceError("invalid window record layout")
    ids = max(1, maximum_id.bit_length())
    width = requested_bytes or (4 if ids + 10 <= 32 else 8)
    if ids + 10 > width * 8:
        raise RecordResourceError("window token does not fit requested record")
    return {"record_bytes": width, "id_bits": ids, "metadata_bits": width * 8 - ids, "mantissa_bits": 0}


def plan_algorithm_resources(
    graph: GraphInfo, *, algorithm: str, records: bool,
    requested_bytes: int, minimum_mantissa_bits: int,
    traversals: int, sources: int, workspace_limit: int,
    carrier_limit: int, auxiliary_limit: int, rss_mib: int,
    backend: str = "cache_sim", target_memory_bytes: int = 0,
    bfs_direction_optimizing: bool = False,
    preprocessing: str = "csr",
    popt_full_capacity: bool = False,
    record_model: str = "next",
    queries: int = 1,
) -> dict[str, int | str | bool | None]:
    coefficients = {"spmv": 8, "bfs": 12, "sssp": 25, "cc": 12, "bc": 40, "tc": 24}
    if algorithm not in coefficients or min(
            traversals, sources, workspace_limit, carrier_limit, auxiliary_limit, rss_mib) <= 0:
        raise RecordResourceError("invalid algorithm or resource limits")
    if not 1 <= queries <= 64 or (queries > 1 and
            (algorithm != "spmv" or records or graph.weighted or backend != "cache_sim")):
        raise RecordResourceError("independent queries require unweighted cache-only CSR SpMV")
    if requested_bytes not in (0, 4, 8) or not 0 <= minimum_mantissa_bits <= 61:
        raise RecordResourceError("invalid algorithm record layout request")
    if preprocessing not in ("csr", "traversal"):
        raise RecordResourceError("invalid algorithm preprocessing")
    if record_model not in ("next", "window", "frontier"):
        raise RecordResourceError("invalid record model")
    window = record_model in ("window", "frontier")
    context_bytes = 4096 if record_model == "frontier" else 0
    if window and (not records or algorithm != "bfs" or graph.weighted or not graph.records or
                   bfs_direction_optimizing or preprocessing != "csr" or minimum_mantissa_bits or backend != "cache_sim"):
        raise RecordResourceError("window model requires unweighted cache-only TD BFS records")
    if popt_full_capacity and (records or bfs_direction_optimizing or backend != "cache_sim"):
        raise RecordResourceError("current P-OPT requires cache-only CSR scalar graph passes")
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
    if window:
        layout = window_layout(graph.maximum_id, requested_bytes)
    elif records and algorithm != "tc":
        try:
            layout = resolve_layout(
                records=carrier_records, vertices=graph.vertices, maximum_id=graph.maximum_id,
                traversals=traversals, requested_bytes=requested_bytes,
                minimum_mantissa_bits=minimum_mantissa_bits,
                weight_bits=record_weight_bits(graph, algorithm))
        except RecordReceiptError as error:
            raise RecordResourceError(str(error)) from error
    # TC's oriented target IDs are known only after its charged orientation.
    width_upper = int(layout.get("record_bytes", requested_bytes or 8))
    carrier = carrier_records * width_upper if records else 0
    lines = (graph.vertices * property_bytes + 63) // 64
    slots = 1 << max(0, (2 * min(carrier_records, lines) - 1).bit_length())
    partitions = {"sssp": 2, "cc": 3}.get(algorithm, 1) if preprocessing == "traversal" else 1
    filtered = algorithm not in ("spmv", "tc")
    scratch = (min(lines * 8 * partitions, slots * (8 + 8 * partitions)) if filtered
               else slots * (8 + 16 * partitions)) if records else 0
    if window:
        scratch = lines * 8
    popt_lines = popt_matrix_lines(algorithm, graph.vertices) if popt_full_capacity else 0
    popt_bytes = popt_lines * 256
    if popt_full_capacity:
        scratch += 192
    shared_owner = 512 if popt_full_capacity and queries > 1 else 0
    scratch += shared_owner
    # The weight readers' records carry the weights, so no second array is planned.
    if carrier > carrier_limit or scratch + popt_bytes > auxiliary_limit or (
            arrays + carrier + scratch + popt_bytes + context_bytes > workspace_limit):
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
        "record_weight_bytes_upper": 0,
        "construction_auxiliary_bytes_upper": scratch, "graph_loader_bytes_upper": graph_peak,
        "record_preprocess": preprocessing, "construction_partitions": partitions,
        "record_model": record_model,
        "frontier_runtime_budget_bytes": context_bytes,
        "queries": queries, "shared_preparation_bytes_upper": shared_owner,
        "popt_matrix_lines": popt_lines, "popt_matrix_bytes_upper": popt_bytes,
        "workspace_limit_bytes": workspace_limit, "planned_host_bytes": host,
        "planned_target_bytes": planned, "rss_limit_mib": rss_mib,
        "memory_plan": "algorithm-conservative-reservation",
    }
