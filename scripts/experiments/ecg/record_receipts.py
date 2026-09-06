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
EQUIVALENCE_DIGESTS = (
    "source_order_digest", "carrier_digest", "consumed_semantic_digest",
    "destination_stream_digest", "window_reference_digest",
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


def validate_equivalence(text: str, expected_reads: int) -> dict[str, int | str]:
    fields = receipt(text, "ECG-RECORD-EQUIVALENCE")
    require(fields.get("schema") == "ecg.record-stream" and
            fields.get("observer") == "actual-record-load", "unrecognized equivalence observation")
    require(unsigned(fields, "property_read_count") == expected_reads,
            "equivalence observation did not cover the complete property stream")
    for key in EQUIVALENCE_DIGESTS:
        require(bool(re.fullmatch(r"[0-9a-f]{16}", fields.get(key, ""))),
                f"missing equivalence fingerprint: {key}")
    return {
        "equivalence_schema": fields["schema"],
        "property_read_count": expected_reads,
        **{key: fields[key] for key in EQUIVALENCE_DIGESTS},
    }


def resolve_layout(
    *, records: int, vertices: int, maximum_id: int,
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
    return {**expected, "state_encoding": "joint-distance", "prefetch_selection": "record-window"}


def validate_layout(
    fields: Mapping[str, str], *, records: int, vertices: int, maximum_id: int,
    traversals: int, requested_bytes: int = 0, minimum_mantissa_bits: int = 0,
) -> dict[str, int | str]:
    expected = resolve_layout(
        records=records, vertices=vertices, maximum_id=maximum_id, traversals=traversals,
        requested_bytes=requested_bytes, minimum_mantissa_bits=minimum_mantissa_bits)
    for key, value in expected.items():
        actual = unsigned(fields, key) if isinstance(value, int) else fields.get(key)
        require(actual == value, f"inconsistent resolved ECG field {key}")
    return expected


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


def validate_functional_record(
    text: str, *, mechanism: str, requested_bytes: int = 0, minimum_mantissa_bits: int = 0,
) -> dict[str, int | str]:
    require(mechanism in MECHANISMS, "unrecognized current ECG mechanism")
    stream = receipt(text, "ECG-RECORD-STREAM")
    runtime = receipt(text, "ECG-RECORD-FUNCTIONAL")
    work = receipt(text, "ECG-PR-RESULT")
    records = unsigned(stream, "records")
    iterations = unsigned(work, "iterations")
    layout = validate_layout(
        stream, records=records, vertices=unsigned(stream, "vertex_count"),
        maximum_id=unsigned(stream, "max_vertex_id"), traversals=iterations,
        requested_bytes=requested_bytes, minimum_mantissa_bits=minimum_mantissa_bits)
    require(all(stream.get(key) == runtime.get(key) for key in LAYOUT_FIELDS),
            "functional stream/runtime layout disagreement")
    require(runtime.get("mechanism") == mechanism and
            runtime.get("timing_scope") == "access-step", "wrong functional mechanism or timing scope")
    require(unsigned(runtime, "pending") == 0 and unsigned(runtime, "accounting") == 1,
            "functional transport did not finish")
    require(unsigned(stream, "source_immutable") == 1 and unsigned(stream, "matrix_bytes") == 0 and
            stream.get("storage") == "separate", "unrecognized functional carrier ownership")
    require(unsigned(stream, "source_stream_bytes") == unsigned(stream, "retained_source_bytes") == records * 4,
            "functional source storage is underreported")
    require(unsigned(stream, "carrier_payload_bytes") == records * int(layout["record_bytes"]) and
            unsigned(stream, "carrier_allocation_bytes") >= unsigned(stream, "carrier_payload_bytes"),
            "functional carrier storage is underreported")
    total = records * iterations
    require(unsigned(work, "semantic_edges") == unsigned(runtime, "record_loads") ==
            unsigned(runtime, "governed_loads") == total, "incomplete functional semantic work")
    require(unsigned(runtime, "record_read_bytes") == total * int(layout["record_bytes"]),
            "functional demand record width differs from its descriptor")
    replacement = mechanism in ("replacement", "replacement-prefetch")
    generated = unsigned(runtime, "generated")
    require(generated == (total if replacement else 0) and
            generated == unsigned(runtime, "enqueued") + unsigned(runtime, "coalesced") and
            unsigned(runtime, "enqueued") == unsigned(runtime, "delivered") and
            unsigned(runtime, "delivered") == sum(unsigned(runtime, key) for key in (
                "applied", "absent", "stale", "expired")) and
            unsigned(runtime, "max_update_occupancy") <= 16 and
            unsigned(runtime, "update_latency_steps") >= 8,
            "functional required update accounting or bounds failed")
    prefetch = mechanism in ("prefetch", "replacement-prefetch")
    bank_bytes = (128 if layout["record_bytes"] == 4 else 192) if prefetch else 0
    require(unsigned(runtime, "record_buffer_bytes") == bank_bytes and
            unsigned(runtime, "record_acquisition_bytes") ==
                64 * unsigned(runtime, "record_acquisition_requests"),
            "functional record-window acquisition is not charged")
    require(unsigned(runtime, "prefetch_candidates") == sum(unsigned(runtime, key) for key in (
                "prefetch_issued", "prefetch_resident", "prefetch_pending_duplicates",
                "prefetch_admission_dropped", "prefetch_queue_dropped")) and
            unsigned(runtime, "prefetch_issued") ==
                unsigned(runtime, "prefetch_fills") + unsigned(runtime, "prefetch_completion_dropped"),
            "functional prefetch disposition does not close")
    checksum = work.get("score_checksum", "")
    require(bool(re.fullmatch(r"[0-9a-f]{16}", checksum)), "missing functional score checksum")
    return {
        **layout, "method": "ECG", "mechanism": mechanism, "record_contract_valid": 1,
        "records": records, "vertex_count": unsigned(stream, "vertex_count"),
        "max_vertex_id": unsigned(stream, "max_vertex_id"),
        "pr_iterations": iterations, "pr_semantic_edges": total, "pr_score_checksum": checksum,
        "retained_source_bytes": unsigned(stream, "retained_source_bytes"),
        "carrier_allocation_bytes": unsigned(stream, "carrier_allocation_bytes"),
        "record_payload_bytes": unsigned(stream, "carrier_payload_bytes"),
        "construction_auxiliary_peak_bytes": unsigned(stream, "construction_auxiliary_peak_bytes"),
        **{"functional_" + key: value for key, value in runtime.items() if key not in LAYOUT_FIELDS},
    }


def validate_sniper_record(
    text: str, *, mechanism: str, requested_bytes: int = 0, minimum_mantissa_bits: int = 0,
) -> dict[str, int | str]:
    require(mechanism in MECHANISMS, "unrecognized current ECG mechanism")
    stream = receipt(text, "SNIPER-ECG-RECORD-STREAM")
    configuration = receipt(text, "SNIPER-ECG-RECORD-CONFIG")
    runtime = receipt(text, "SNIPER-ECG-RECORD")
    work = receipt(text, "ECG-PR-RESULT")
    context = receipt(text, "ECG-CONTEXT-READY")
    require(context.get("sim") == "sniper" and unsigned(context, "loaded") == 1 and
            unsigned(context, "reref") == 0, "Sniper loaded a matrix or lacked its sealed graph context")
    records = unsigned(stream, "records")
    vertices = unsigned(stream, "vertex_count")
    iterations = unsigned(work, "iterations")
    layout = validate_layout(
        stream, records=records, vertices=vertices,
        maximum_id=unsigned(stream, "max_vertex_id"), traversals=iterations,
        requested_bytes=requested_bytes, minimum_mantissa_bits=minimum_mantissa_bits)
    require(all(configuration.get(key) == stream.get(key) for key in LAYOUT_FIELDS),
            "Sniper guest/configuration layout disagreement")
    require(all(fields.get("mechanism") == mechanism for fields in (stream, configuration, runtime)),
            "Sniper mechanism differs between guest and model")
    require(unsigned(configuration, "record_count") == records and
            unsigned(configuration, "vertex_count") == vertices and
            unsigned(configuration, "context") > 0 and unsigned(configuration, "line_bytes") == 64,
            "Sniper graph context or geometry disagrees")
    require(configuration.get("core_scope") == "single-core" and
            configuration.get("prefetch_request_model") == "llc-only-read" and
            configuration.get("update_link_model") == "bounded-completion-corroboration" and
            configuration.get("dead_miss_bypass") == "llc-request-scoped",
            "unrecognized Sniper mechanism implementation")
    require(stream.get("storage") == "separate" and stream.get("construction") == "outside-roi",
            "unrecognized Sniper carrier ownership")
    require(unsigned(stream, "source_stream_bytes") == unsigned(stream, "retained_source_bytes") ==
            records * 4, "Sniper retained source storage is underreported")
    require(unsigned(stream, "carrier_payload_bytes") == records * int(layout["record_bytes"]) and
            unsigned(stream, "carrier_allocation_bytes") >= unsigned(stream, "carrier_payload_bytes"),
            "Sniper carrier storage is underreported")
    require(unsigned(stream, "guest_window_records") == 16 and
            unsigned(stream, "guest_window_data_bits") == 1024 and
            unsigned(stream, "guest_window_index_bits") == 1024 and
            unsigned(stream, "guest_window_valid_bits") == 16 and
            unsigned(configuration, "window_entries") == 16 and
            unsigned(configuration, "window_entry_bits") == 257,
            "Sniper software-window storage is not fully disclosed")
    total = records * iterations
    require(unsigned(work, "semantic_edges") == total and all(
        unsigned(runtime, key) == total for key in (
            "record_reads", "loaded_values", "consumed_records", "property_accesses")),
        "Sniper did not bind every real record/property access")
    require(unsigned(runtime, "record_read_bytes") == total * int(layout["record_bytes"]),
            "Sniper demand record width differs from its descriptor")
    require(runtime.get("timing_scope") == "modeled-corroboration" and
            unsigned(runtime, "clean") == unsigned(runtime, "accounting") == 1,
            "Sniper did not complete its modeled scope")
    require(all(unsigned(runtime, key) == 0 for key in (
        "errors", "pending_updates", "pending_prefetches", "prefetch_translation_faults",
        "prefetch_queue_full")), "Sniper ended with errors, faults or unfinished work")
    replacement = mechanism in ("replacement", "replacement-prefetch")
    generated = unsigned(runtime, "generated_updates")
    enqueued = unsigned(runtime, "enqueued_updates")
    delivered = unsigned(runtime, "delivered_updates")
    require(generated == (total if replacement else 0) and
            generated == enqueued + unsigned(runtime, "coalesced_updates") and
            enqueued == delivered and delivered == sum(unsigned(runtime, key) for key in (
                "applied_updates", "stale_updates", "expired_updates", "not_resident_updates")),
            "Sniper required metadata update accounting does not close")
    require(unsigned(configuration, "update_latency") >= 8 and
            unsigned(configuration, "update_output_width") == 1 and
            1 <= unsigned(configuration, "capture_width") <= 16 and
            unsigned(runtime, "max_update_occupancy") <= 16,
            "Sniper update transport violates its bound")
    if replacement:
        require(unsigned(runtime, "minimum_update_latency") >= unsigned(configuration, "update_latency"),
                "Sniper metadata was delivered before completion plus link latency")
    require(unsigned(runtime, "prefetch_candidates") == unsigned(runtime, "prefetch_enqueued") +
            unsigned(runtime, "prefetch_pending_duplicates") and
            unsigned(runtime, "prefetch_enqueued") == sum(unsigned(runtime, key) for key in (
                "prefetch_issued", "prefetch_private_duplicates", "prefetch_llc_duplicates",
                "prefetch_issue_admission_drops")) and
            unsigned(runtime, "prefetch_issued") == sum(unsigned(runtime, key) for key in (
                "prefetch_fills", "prefetch_completion_private_duplicates",
                "prefetch_completion_resident", "prefetch_completion_admission_drops",
                "prefetch_demand_merges")),
            "Sniper prefetch request/completion accounting does not close")
    require(unsigned(runtime, "prefetch_translation_requests") +
            unsigned(runtime, "prefetch_translation_bypasses") ==
            unsigned(runtime, "prefetch_enqueued"), "Sniper translation accounting does not close")
    require(unsigned(runtime, "prefetch_request_bytes") == 64 * unsigned(runtime, "prefetch_issued") and
            unsigned(runtime, "prefetch_fill_bytes") == 64 * unsigned(runtime, "prefetch_fills"),
            "Sniper prefetch traffic is underreported")
    lookup_minimum = unsigned(configuration, "lookup_latency") * (
        2 * unsigned(runtime, "prefetch_private_lookups") +
        unsigned(runtime, "prefetch_llc_lookups") +
        unsigned(runtime, "prefetch_issue_admission_checks") +
        unsigned(runtime, "prefetch_completion_admission_checks"))
    require(unsigned(configuration, "lookup_latency") > 0 and
            unsigned(runtime, "lookup_cycles_charged") >= lookup_minimum and
            unsigned(configuration, "drain_max_cycles") >= unsigned(runtime, "drain_cycles_charged"),
            "Sniper lookup or drain cost is uncharged/unbounded")
    require(unsigned(runtime, "max_prefetch_occupancy") <= unsigned(configuration, "prefetch_queue"),
            "Sniper prefetch queue exceeded its declared storage")
    if unsigned(runtime, "prefetch_fills"):
        require(unsigned(runtime, "prefetch_latency_cycles") > 0, "Sniper completed a zero-time prefetch")
    checksum = work.get("score_checksum", "")
    require(bool(re.fullmatch(r"[0-9a-f]{16}", checksum)), "missing Sniper PageRank checksum")
    return {
        **layout, "method": "ECG", "mechanism": mechanism, "record_contract_valid": 1,
        "records": records, "vertex_count": vertices,
        "max_vertex_id": unsigned(stream, "max_vertex_id"),
        "pr_iterations": iterations, "pr_semantic_edges": total, "pr_score_checksum": checksum,
        "retained_source_bytes": unsigned(stream, "retained_source_bytes"),
        "carrier_allocation_bytes": unsigned(stream, "carrier_allocation_bytes"),
        "record_payload_bytes": unsigned(stream, "carrier_payload_bytes"),
        "construction_auxiliary_peak_bytes": unsigned(stream, "auxiliary_peak_bytes"),
        **{"sniper_" + key: value for key, value in runtime.items()},
        "sniper_nuca_sets": unsigned(configuration, "nuca_sets"),
        "sniper_nuca_indexing": configuration.get("nuca_indexing", ""),
    }
