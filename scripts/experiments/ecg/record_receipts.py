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


def validate_pr_workload(
        text: str, *, carrier: str, iterations: int, semantic_edges: int) -> dict[str, int | str]:
    fields = receipt(text, "ECG-PR-WORKLOAD")
    require(fields.get("traversal") == "pull-gs" and
            fields.get("arithmetic") == "separate-f32" and fields.get("carrier") == carrier,
            "unexpected fixed PageRank workload contract")
    vertices, records = unsigned(fields, "vertices"), unsigned(fields, "records")
    require(vertices > 0 and records > 0 and iterations > 0 and
            records * iterations == semantic_edges,
            "fixed PageRank did not cover the requested traversal")
    reads = unsigned(fields, "csr_index_reads")
    require(reads == 4 * vertices * iterations, "fixed PageRank omitted CSR index accesses")
    return {
        "pr_workload_contract": "fixed-pull-gs", "pr_arithmetic": "separate-f32",
        "pr_input_carrier": carrier, "pr_vertex_count": vertices,
        "pr_source_records": records, "pr_csr_index_reads": reads,
    }


CENSUS_SEGMENTS = ("before_first_pass", "first_pass", "between_passes", "later_passes", "after_last_pass")
CENSUS_COUNTERS = (
    "total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks",
    "llc_hits", "llc_misses", "llc_property_hits", "llc_property_misses",
)
CENSUS_LINES = (
    "entry_valid_lines", "entry_dirty_lines", "entry_property_lines", "entry_property_dirty_lines",
    "entry_dirty_writebacks", "entry_dirty_rewritten",
    "exit_valid_lines", "exit_dirty_lines", "exit_property_lines", "exit_entry_dirty_lines",
)


def _census_unsigned(fields: object, key: str) -> int:
    value = fields.get(key) if isinstance(fields, Mapping) else None
    require(type(value) is int and 0 <= value <= UINT64_MAX, f"invalid kernel census field {key}")
    return value


def validate_kernel_census(census: object, kernel: Mapping[str, object]) -> dict[str, int]:
    """Check one kernel's passive census against the kernel phase it divides.

    The segments must sum to the kernel counters exactly, each segment's setup
    writebacks must lie within its own writebacks and sum to the census's,
    every last-level line dirty at entry must be written back, rewritten or
    still resident, and the lines resident at exit must bound their dirty and
    property lines.
    """
    require(isinstance(census, Mapping), "missing kernel census")
    require(_census_unsigned(census, "entries") == 1, "kernel census does not cover exactly one kernel")
    passes = _census_unsigned(census, "passes")
    lines = {key: _census_unsigned(census, key) for key in CENSUS_LINES}
    require(lines["entry_dirty_lines"] == lines["entry_dirty_writebacks"] + lines["entry_dirty_rewritten"] +
            lines["exit_entry_dirty_lines"], "kernel census leaves an entry-dirty line unaccounted")
    require(lines["entry_property_dirty_lines"] <= min(lines["entry_property_lines"], lines["entry_dirty_lines"]) and
            max(lines["entry_property_lines"], lines["entry_dirty_lines"]) <= lines["entry_valid_lines"] and
            lines["exit_entry_dirty_lines"] <= lines["exit_dirty_lines"] and
            max(lines["exit_dirty_lines"], lines["exit_property_lines"]) <= lines["exit_valid_lines"],
            "kernel census line counts are inconsistent")
    segments = census.get("segments")
    require(isinstance(segments, Mapping) and set(segments) == set(CENSUS_SEGMENTS),
            "kernel census segments are incomplete")
    result = {"kernel_census_passes": passes, **{"kernel_census_" + key: value for key, value in lines.items()}}
    for counter in CENSUS_COUNTERS:
        values = [_census_unsigned(segments[segment], counter) for segment in CENSUS_SEGMENTS]
        require(sum(values) == _census_unsigned(kernel, counter),
                f"kernel census segments do not sum to the kernel: {counter}")
        result.update({f"kernel_census_{segment}_{counter}": value
                       for segment, value in zip(CENSUS_SEGMENTS, values)})
    for segment in CENSUS_SEGMENTS:
        offchip = _census_unsigned(segments[segment], "total_offchip_traffic")
        require(offchip == sum(result[f"kernel_census_{segment}_{counter}"] for counter in
                               ("memory_accesses", "prefetch_fills", "llc_writebacks")),
                f"invalid kernel census traffic sum: {segment}")
        result[f"kernel_census_{segment}_total_offchip_traffic"] = offchip
        setup = _census_unsigned(segments[segment], "entry_dirty_writebacks")
        require(setup <= result[f"kernel_census_{segment}_llc_writebacks"],
                f"kernel census charges a segment more setup writebacks than it wrote back: {segment}")
        result[f"kernel_census_{segment}_entry_dirty_writebacks"] = setup
    # Pass segments exist only for the passes the kernel ran.
    empty = ("first_pass", "between_passes", "later_passes", "after_last_pass") if passes == 0 else (
        ("between_passes", "later_passes") if passes == 1 else ())
    require(all(result[f"kernel_census_{segment}_{counter}"] == 0
                for segment in empty for counter in CENSUS_COUNTERS),
            "kernel census charges a pass segment the kernel did not run")
    require(lines["entry_dirty_writebacks"] <= _census_unsigned(kernel, "llc_writebacks"),
            "kernel census charges more setup writebacks than the kernel wrote back")
    require(sum(result[f"kernel_census_{segment}_entry_dirty_writebacks"] for segment in CENSUS_SEGMENTS) ==
            lines["entry_dirty_writebacks"], "kernel census setup writebacks by segment do not sum to the census")
    return result


CENSUS_PASS_DETAIL_LIMIT = 64
CENSUS_PASS_TRAFFIC = (*CENSUS_COUNTERS, "total_offchip_traffic", "entry_dirty_writebacks")
CENSUS_PASS_ENDS = ("end_valid_lines", "end_dirty_lines", "end_property_lines", "end_entry_dirty_lines")


def validate_kernel_census_passes(census: object) -> list[dict[str, int]]:
    """Check a census's pass detail against its passes, segments and exit.

    The census details its first CENSUS_PASS_DETAIL_LIMIT passes. The first
    detailed pass must be the first-pass segment and the rest must sum to the
    later-pass segment, or lie within it when the kernel ran more passes than
    it details. Each pass's end must bound its dirty and property lines, the
    lines dirty at entry may only retire, and when nothing runs after the last
    pass its end must be the census's exit.
    """
    require(isinstance(census, Mapping), "missing kernel census")
    passes = _census_unsigned(census, "passes")
    require(_census_unsigned(census, "pass_detail_limit") == CENSUS_PASS_DETAIL_LIMIT,
            f"kernel census pass detail limit is not {CENSUS_PASS_DETAIL_LIMIT}")
    detail = census.get("pass_detail")
    require(isinstance(detail, list), "invalid kernel census pass detail")
    require(len(detail) == min(passes, CENSUS_PASS_DETAIL_LIMIT),
            "kernel census pass detail does not cover its passes")
    keys = (*CENSUS_PASS_TRAFFIC, *CENSUS_PASS_ENDS)
    result = []
    for number, fields in enumerate(detail, 1):
        require(isinstance(fields, Mapping) and set(fields) == set(keys),
                f"kernel census pass detail fields differ: pass {number}")
        values = {key: _census_unsigned(fields, key) for key in keys}
        require(values["total_offchip_traffic"] ==
                values["memory_accesses"] + values["prefetch_fills"] + values["llc_writebacks"],
                f"invalid kernel census pass traffic sum: pass {number}")
        require(values["entry_dirty_writebacks"] <= values["llc_writebacks"],
                f"kernel census charges a pass more setup writebacks than it wrote back: pass {number}")
        require(values["end_entry_dirty_lines"] <= values["end_dirty_lines"] and
                max(values["end_dirty_lines"], values["end_property_lines"]) <= values["end_valid_lines"],
                f"kernel census pass end line counts are inconsistent: pass {number}")
        result.append(values)
    segments = census.get("segments")
    require(isinstance(segments, Mapping) and set(segments) == set(CENSUS_SEGMENTS),
            "kernel census segments are incomplete")
    if result:
        for key in CENSUS_PASS_TRAFFIC:
            require(result[0][key] == _census_unsigned(segments["first_pass"], key),
                    f"kernel census first pass detail differs from its segment: {key}")
            later = sum(values[key] for values in result[1:])
            segment = _census_unsigned(segments["later_passes"], key)
            if passes <= CENSUS_PASS_DETAIL_LIMIT:
                require(later == segment, f"kernel census later pass detail does not sum to its segment: {key}")
            else:
                require(later <= segment, f"kernel census later pass detail exceeds its segment: {key}")
    # A line dirty at entry keeps its mark until it is written back or
    # rewritten, so the marks never grow or outnumber what entry left.
    entry = _census_unsigned(census, "entry_dirty_lines")
    retired = _census_unsigned(segments["before_first_pass"], "entry_dirty_writebacks")
    marks = entry
    for number, values in enumerate(result, 1):
        retired += values["entry_dirty_writebacks"]
        require(values["end_entry_dirty_lines"] + retired <= entry,
                f"kernel census pass detail keeps more setup-dirty lines than entry left: pass {number}")
        require(values["end_entry_dirty_lines"] <= marks,
                f"kernel census pass detail marks a setup-dirty line after it retired: pass {number}")
        marks = values["end_entry_dirty_lines"]
    if result:
        require(marks >= _census_unsigned(census, "exit_entry_dirty_lines"),
                "kernel census pass detail and exit disagree on setup-dirty lines")
    # With nothing run after the last pass, the exit reads the cache it left.
    quiet = all(_census_unsigned(segments["after_last_pass"], counter) == 0 for counter in CENSUS_COUNTERS)
    if result and passes <= CENSUS_PASS_DETAIL_LIMIT and quiet:
        require(all(result[-1][end] == _census_unsigned(census, end.replace("end_", "exit_", 1))
                    for end in CENSUS_PASS_ENDS), "kernel census exit differs from its last pass end")
    return result


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
