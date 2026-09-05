"""Strict receipts for the single graph-adaptive ECG record contract."""

from __future__ import annotations

import re
from typing import Mapping


UINT64_MAX = (1 << 64) - 1
MECHANISMS = ("transport", "replacement", "prefetch", "replacement-prefetch")
LAYOUT_FIELDS = (
    "record_bytes", "id_bits", "metadata_bits", "horizon_bits",
    "exponent_bits", "mantissa_bits", "sequence_bits", "deadline_bits",
    "state_encoding", "prefetch_selection",
)


class RecordReceiptError(ValueError):
    pass


def receipt(text: str, name: str) -> dict[str, str]:
    matches = re.findall(r"\[" + re.escape(name) + r" ([^\]\r\n]+)\]", text)
    if len(matches) != 1:
        raise RecordReceiptError(f"expected exactly one {name} receipt, got {len(matches)}")
    fields: dict[str, str] = {}
    for item in matches[0].split():
        key, separator, value = item.partition("=")
        if not separator or not key or not value or key in fields:
            raise RecordReceiptError(f"malformed or duplicate {name} field: {item}")
        fields[key] = value
    return fields


def unsigned(fields: Mapping[str, str], key: str) -> int:
    value = fields.get(key, "")
    if not re.fullmatch(r"[0-9]+", value) or int(value) > UINT64_MAX:
        raise RecordReceiptError(f"invalid unsigned field {key}={value!r}")
    return int(value)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RecordReceiptError(message)


def validate_layout(
    fields: Mapping[str, str], *, records: int, vertices: int, maximum_id: int,
    traversals: int, requested_bytes: int = 0, minimum_mantissa_bits: int = 0,
) -> dict[str, int | str]:
    require(0 < records <= UINT64_MAX and 0 < vertices <= UINT64_MAX and
            0 <= maximum_id < vertices and traversals > 0, "invalid graph/work counts")
    require(requested_bytes in (0, 4, 8) and 0 <= minimum_mantissa_bits <= 61,
            "invalid requested record width or precision")
    horizon = records.bit_length()
    require(horizon <= 63, "record horizon exceeds the current checked arithmetic")
    id_bits = max(1, maximum_id.bit_length())
    resolved: tuple[int, int, int] | None = None
    for width in (4, 8):
        if requested_bytes and width != requested_bytes:
            continue
        metadata = width * 8 - id_bits
        if metadata <= 0:
            continue
        levels = ((1 << metadata) - 2) // (2 * horizon)
        if levels <= 0:
            continue
        mantissa = levels.bit_length() - 1
        if mantissa >= minimum_mantissa_bits:
            resolved = width, metadata, mantissa
            break
    if resolved is None:
        raise RecordReceiptError("no sufficient record width")
    width, metadata, mantissa = resolved
    require(records * width <= UINT64_MAX and
            records * traversals + (1 << horizon) - 1 <= UINT64_MAX,
            "carrier bytes or semantic deadline headroom overflows")
    expected = {
        "record_bytes": width, "id_bits": id_bits, "metadata_bits": metadata,
        "horizon_bits": horizon, "exponent_bits": (horizon - 1).bit_length(),
        "mantissa_bits": mantissa, "sequence_bits": 64, "deadline_bits": 64,
    }
    for key, value in expected.items():
        require(unsigned(fields, key) == value, f"inconsistent resolved ECG field {key}")
    require(fields.get("state_encoding") == "joint-distance", "wrong state/reference grammar")
    require(fields.get("prefetch_selection") == "record-window", "wrong prefetch rule")
    return {**expected, "state_encoding": "joint-distance", "prefetch_selection": "record-window"}


def validate_gem5_record(
    text: str, *, mechanism: str, requested_bytes: int = 0, minimum_mantissa_bits: int = 0,
) -> dict[str, int | str]:
    require(mechanism in MECHANISMS, "unrecognized current ECG mechanism")
    guest = receipt(text, "ECG-RECORD-GUEST")
    runtime = receipt(text, "ECG-RECORD-NATIVE")
    work = receipt(text, "ECG-PR-RESULT")
    records = unsigned(guest, "records")
    vertices = unsigned(guest, "vertex_count")
    maximum_id = unsigned(guest, "max_vertex_id")
    iterations = unsigned(work, "iterations")
    layout = validate_layout(
        guest, records=records, vertices=vertices, maximum_id=maximum_id,
        traversals=iterations, requested_bytes=requested_bytes,
        minimum_mantissa_bits=minimum_mantissa_bits)
    for key in LAYOUT_FIELDS:
        require(runtime.get(key) == guest.get(key), f"guest/runtime layout disagreement: {key}")
    require(unsigned(guest, "native") == 1, "guest did not execute the native record ISA")
    require(guest.get("storage") == "separate" and unsigned(guest, "source_immutable") == 1,
            "unrecognized carrier ownership")
    require(unsigned(guest, "matrix_bytes") == unsigned(guest, "edge_sideband_bytes") == 0,
            "current ECG consumed an uncharged metadata sideband")
    require(unsigned(guest, "source_stream_bytes") == records * 4 and
            unsigned(guest, "retained_source_bytes") == records * 4,
            "source storage accounting does not match the current loader")
    payload = records * int(layout["record_bytes"])
    require(unsigned(guest, "carrier_payload_bytes") == payload and
            unsigned(guest, "carrier_allocation_bytes") >= payload,
            "carrier storage was narrowed or underreported")
    semantic_edges = records * iterations
    require(unsigned(work, "semantic_edges") == semantic_edges,
            "PageRank did not execute its complete fixed traversal")
    checksum = work.get("score_checksum", "")
    require(bool(re.fullmatch(r"[0-9a-f]{16}", checksum)), "missing PageRank score checksum")
    for key in ("record_loads", "governed_loads", "last_sequence"):
        require(unsigned(runtime, key) == semantic_edges, f"incomplete native work: {key}")
    require(unsigned(runtime, "record_read_bytes") == semantic_edges * int(layout["record_bytes"]),
            "native demand record width was not honored")
    for key in ("pending", "errors", "required_update_drops"):
        require(unsigned(runtime, key) == 0, f"native ECG did not finish cleanly: {key}")
    replacement = mechanism in ("replacement", "replacement-prefetch")
    prefetch = mechanism in ("prefetch", "replacement-prefetch")
    require(unsigned(runtime, "replacement") == int(replacement) and
            unsigned(runtime, "prefetch") == int(prefetch), "wrong native mechanisms")
    generated = unsigned(runtime, "generated")
    accepted = unsigned(runtime, "accepted")
    enqueued = unsigned(runtime, "enqueued")
    coalesced = unsigned(runtime, "coalesced")
    delivered = unsigned(runtime, "delivered")
    require(generated == (semantic_edges if replacement else 0) and
            generated == accepted == enqueued + coalesced and enqueued == delivered and
            delivered == sum(unsigned(runtime, key) for key in ("applied", "stale", "expired", "absent")),
            "required update accounting does not close")
    require(unsigned(runtime, "capacity") == 16 and
            1 <= unsigned(runtime, "capture_width") <= 16 and
            unsigned(runtime, "output_width") == 1 and
            unsigned(runtime, "max_occupancy") <= 16 and
            unsigned(runtime, "latency_cycles") >= 8, "invalid bounded update transport")
    if replacement:
        require(unsigned(runtime, "min_latency") >= unsigned(runtime, "latency_cycles"),
                "metadata was delivered before its required latency")
    require(unsigned(runtime, "accounting") == 1, "native accounting failed")
    result: dict[str, int | str] = {
        **layout, "method": "ECG", "mechanism": mechanism,
        "records": records, "vertex_count": vertices, "max_vertex_id": maximum_id,
        "pr_iterations": iterations, "pr_semantic_edges": semantic_edges,
        "pr_score_checksum": checksum, "record_contract_valid": 1,
        "record_payload_bytes": payload,
        "retained_source_bytes": unsigned(guest, "retained_source_bytes"),
        "carrier_allocation_bytes": unsigned(guest, "carrier_allocation_bytes"),
        "construction_auxiliary_peak_bytes": unsigned(guest, "construction_auxiliary_peak_bytes"),
        "record_read_bytes": unsigned(runtime, "record_read_bytes"),
    }
    if prefetch:
        fields = receipt(text, "ECG-RECORD-PREFETCH")
        require(unsigned(fields, "pending") == unsigned(fields, "translation_failures") == 0,
                "native prefetch did not complete with valid translations")
        require(unsigned(fields, "accounting") == 1, "native prefetch accounting failed")
        banks = 2 if layout["record_bytes"] == 4 else 3
        require(unsigned(fields, "record_banks") == banks and
                unsigned(fields, "record_buffer_bytes") == banks * 64,
                "the unaligned real record window is not fully charged")
        require(unsigned(fields, "capture_latency_cycles") >= 1 and
                unsigned(fields, "l1_fill_latency_charged") == 1 and
                unsigned(fields, "lookup_latency_cycles") >= 1 and
                unsigned(fields, "prefetch_latency_cycles") >= 1,
                "prefetch capture or lookup latency is uncharged")
        require(unsigned(fields, "presence_lookups") == 3 * unsigned(fields, "admission_lookups"),
                "three-level issue/completion lookups do not close")
        require(fields.get("allocation") == "llc-only" and
                fields.get("request_command") == "acknowledged-read",
                "unexpected native prefetch allocation or request protocol")
        for kind in ("record", "property"):
            require(unsigned(fields, kind + "_reads") == unsigned(fields, kind + "_responses"),
                    f"unacknowledged {kind} acquisition requests")
        require(unsigned(fields, "record_acquisition_bytes") == 64 * unsigned(fields, "record_reads") and
                unsigned(fields, "property_prefetch_bytes") == 64 * unsigned(fields, "property_reads"),
                "prefetch request traffic is underreported")
        require(unsigned(fields, "triggers") == semantic_edges and
                semantic_edges == sum(unsigned(fields, key) for key in (
                    "tail_skipped", "window_queue_dropped", "selected_windows")) and
                unsigned(fields, "selected_windows") ==
                    unsigned(fields, "candidates") + unsigned(fields, "empty_windows"),
                "prefetch opportunity accounting does not close")
        window_capacity = unsigned(fields, "window_queue_size")
        property_capacity = unsigned(fields, "property_queue_size")
        require(1 <= window_capacity <= 16 and 1 <= property_capacity <= 16 and
                unsigned(fields, "max_pending") <= window_capacity + property_capacity + banks,
                "prefetch storage bounds were not honored")
        require(unsigned(fields, "candidates") == sum(unsigned(fields, key) for key in (
            "pending_duplicates", "property_queue_dropped", "issue_resident",
            "issue_admission", "property_reads", "translation_failures")),
            "prefetch candidate disposition does not close")
        result.update({"prefetch_" + key: value for key, value in fields.items()})
    return result
