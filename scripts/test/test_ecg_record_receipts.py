from copy import deepcopy

import pytest

from scripts.experiments.ecg.record_receipts import (
    RecordReceiptError, receipt, unsigned, validate_gem5_record, validate_layout,
)


def layout_fields(width, maximum_id, records):
    ids = max(1, maximum_id.bit_length())
    horizon = records.bit_length()
    metadata = width * 8 - ids
    mantissa = (((1 << metadata) - 2) // (2 * horizon)).bit_length() - 1
    return {
        "record_bytes": str(width), "id_bits": str(ids), "metadata_bits": str(metadata),
        "horizon_bits": str(horizon), "exponent_bits": str((horizon - 1).bit_length()),
        "mantissa_bits": str(mantissa), "sequence_bits": "64", "deadline_bits": "64",
        "state_encoding": "joint-distance", "prefetch_selection": "record-window",
    }


def test_numeric_widths_preserve_all_available_bits():
    for ids in (18, 19, 20, 26, 32):
        maximum = (1 << ids) - 1
        width = 4 if ids <= 26 else 8
        fields = layout_fields(width, maximum, 1 << 30)
        resolved = validate_layout(
            fields, records=1 << 30, vertices=maximum + 1, maximum_id=maximum, traversals=3)
        assert resolved["metadata_bits"] == width * 8 - ids
    fields = layout_fields(4, (1 << 19) - 1, 1 << 30)
    assert fields["metadata_bits"] == "13" and fields["mantissa_bits"] == "7"
    fields["metadata_bits"] = "6"
    with pytest.raises(RecordReceiptError):
        validate_layout(
            fields, records=1 << 30, vertices=1 << 19,
            maximum_id=(1 << 19) - 1, traversals=3)


def test_layout_receipts_reject_unrequested_widening_and_bad_precision():
    fields = layout_fields(8, 31, 34)
    with pytest.raises(RecordReceiptError):
        validate_layout(fields, records=34, vertices=32, maximum_id=31, traversals=1)
    assert validate_layout(
        fields, records=34, vertices=32, maximum_id=31, traversals=1,
        requested_bytes=8)["record_bytes"] == 8
    fields["mantissa_bits"] = "0"
    with pytest.raises(RecordReceiptError):
        validate_layout(
            fields, records=34, vertices=32, maximum_id=31, traversals=1,
            requested_bytes=8)


def test_receipts_are_unique_unsigned_and_complete():
    with pytest.raises(RecordReceiptError):
        receipt("[REC a=1]\n[REC a=1]", "REC")
    with pytest.raises(RecordReceiptError):
        receipt("[REC a=1 a=2]", "REC")
    for value in ("-1", "+1", "1.0", str(1 << 64), ""):
        with pytest.raises(RecordReceiptError):
            unsigned({"key": value}, "key")


def native_fixture():
    fields = layout_fields(4, 31, 34)
    guest = {
        **fields, "native": "1", "records": "34", "vertex_count": "32", "max_vertex_id": "31",
        "source_stream_bytes": "136", "retained_source_bytes": "136",
        "carrier_payload_bytes": "136", "carrier_allocation_bytes": "136",
        "construction_auxiliary_peak_bytes": "256", "source_immutable": "1",
        "storage": "separate", "matrix_bytes": "0", "edge_sideband_bytes": "0",
    }
    runtime = {
        **fields, "replacement": "1", "prefetch": "0", "record_loads": "34",
        "record_read_bytes": "136", "governed_loads": "34", "last_sequence": "34",
        "generated": "34", "accepted": "34", "enqueued": "30", "coalesced": "4",
        "delivered": "30", "applied": "23", "stale": "2", "expired": "0", "absent": "5",
        "pending": "0", "errors": "0", "required_update_drops": "0", "capacity": "16",
        "capture_width": "8", "output_width": "1", "latency_cycles": "8", "min_latency": "8",
        "max_occupancy": "8", "accounting": "1",
    }
    return guest, runtime


def text_for(guest, runtime):
    def render(name, fields):
        return f"[{name} " + " ".join(f"{key}={value}" for key, value in fields.items()) + "]"
    return "\n".join((
        render("ECG-RECORD-GUEST", guest), render("ECG-RECORD-NATIVE", runtime),
        "[ECG-PR-RESULT iterations=1 semantic_edges=34 score_checksum=1234567890abcdef]",
    ))


def test_native_receipt_requires_closed_real_work_and_storage():
    guest, runtime = native_fixture()
    result = validate_gem5_record(text_for(guest, runtime), mechanism="replacement")
    assert result["pr_semantic_edges"] == 34 and result["record_contract_valid"] == 1
    for key, value in (
        ("pending", "1"), ("generated", "33"), ("record_read_bytes", "68"),
        ("last_sequence", "33"), ("min_latency", "7"), ("prefetch", "1"),
    ):
        altered = deepcopy(runtime)
        altered[key] = value
        with pytest.raises(RecordReceiptError):
            validate_gem5_record(text_for(guest, altered), mechanism="replacement")
    for key, value in (("native", "0"), ("carrier_allocation_bytes", "68"), ("matrix_bytes", "8")):
        altered = deepcopy(guest)
        altered[key] = value
        with pytest.raises(RecordReceiptError):
            validate_gem5_record(text_for(altered, runtime), mechanism="replacement")


def test_prefetch_receipt_requires_real_bytes_latency_and_finite_queues():
    guest, runtime = native_fixture()
    runtime["prefetch"] = "1"
    prefetch = {
        "triggers": "34", "tail_skipped": "8", "window_queue_dropped": "0",
        "selected_windows": "26", "empty_windows": "6", "candidates": "20",
        "pending_duplicates": "2", "property_queue_dropped": "1",
        "issue_resident": "5", "issue_admission": "2",
        "record_reads": "4", "record_responses": "4", "record_acquisition_bytes": "256",
        "property_reads": "10", "property_responses": "10", "property_prefetch_bytes": "640",
        "translation_failures": "0", "presence_lookups": "81", "admission_lookups": "27",
        "record_banks": "2", "record_buffer_bytes": "128", "capture_latency_cycles": "1",
        "l1_fill_latency_charged": "1", "lookup_latency_cycles": "12",
        "prefetch_latency_cycles": "8", "allocation": "llc-only",
        "request_command": "acknowledged-read", "pending": "0", "accounting": "1",
        "window_queue_size": "16", "property_queue_size": "16", "max_pending": "5",
    }

    def combined(fields):
        return text_for(guest, runtime) + "\n[ECG-RECORD-PREFETCH " + " ".join(
            f"{key}={value}" for key, value in fields.items()) + "]"

    assert validate_gem5_record(
        combined(prefetch), mechanism="replacement-prefetch")["prefetch_property_reads"] == "10"
    for key, value in (
        ("property_responses", "9"), ("record_buffer_bytes", "64"),
        ("l1_fill_latency_charged", "0"), ("presence_lookups", "27"),
        ("pending", "1"), ("max_pending", "99"), ("candidates", "21"),
    ):
        altered = deepcopy(prefetch)
        altered[key] = value
        with pytest.raises(RecordReceiptError):
            validate_gem5_record(combined(altered), mechanism="replacement-prefetch")
