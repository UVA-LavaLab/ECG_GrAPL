from copy import deepcopy
from types import SimpleNamespace

import pytest

from scripts.experiments.ecg.record_receipts import (
    RecordReceiptError, receipt, unsigned, validate_gem5_record, validate_layout,
    validate_sniper_record,
    validate_equivalence,
    validate_pr_workload,
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


# The same cases as the C++ codec's testWeightedLayoutSelection.
WEIGHTED_LAYOUTS = (
    # records, vertices, weight bits, requested, (width, id, metadata, mantissa)
    (680108, 1 << 18, 6, 0, (4, 18, 8, 2)),
    (680108, 1 << 18, 6, 8, (8, 18, 40, 34)),
    (33037894, 3774768, 6, 0, (8, 22, 36, 30)),
    (33037894, 3774768, 32, 0, (8, 22, 10, 4)),
    (3, 1024, 6, 0, (4, 10, 16, 13)),
)


def test_weighted_layouts_mirror_the_codec():
    from scripts.experiments.ecg.record_receipts import resolve_layout
    for records, vertices, weight_bits, requested, expected in WEIGHTED_LAYOUTS:
        layout = resolve_layout(records=records, vertices=vertices, maximum_id=vertices - 1,
                                traversals=1, requested_bytes=requested, weight_bits=weight_bits)
        assert (layout["record_bytes"], layout["id_bits"], layout["metadata_bits"],
                layout["mantissa_bits"]) == expected
        assert layout["weight_bits"] == weight_bits
    with pytest.raises(RecordReceiptError, match="record width"):
        resolve_layout(records=33037894, vertices=3774768, maximum_id=3774767, traversals=1,
                       requested_bytes=4, weight_bits=6)
    for invalid in (-1, 33):
        with pytest.raises(RecordReceiptError, match="weight"):
            resolve_layout(records=34, vertices=32, maximum_id=31, traversals=1, weight_bits=invalid)
    assert "weight_bits" not in resolve_layout(records=34, vertices=32, maximum_id=31, traversals=1)


def test_weighted_layout_fields_are_present_exactly_when_expected():
    from scripts.experiments.ecg.record_receipts import resolve_layout
    expected = resolve_layout(records=680108, vertices=1 << 18, maximum_id=(1 << 18) - 1,
                              traversals=1, weight_bits=6)
    fields = {key: str(value) for key, value in expected.items()}
    common = dict(records=680108, vertices=1 << 18, maximum_id=(1 << 18) - 1, traversals=1)
    assert validate_layout(fields, weight_bits=6, **common)["weight_bits"] == 6
    for forged in ({**fields, "weight_bits": "5"}, {k: v for k, v in fields.items() if k != "weight_bits"}):
        with pytest.raises(RecordReceiptError):
            validate_layout(forged, weight_bits=6, **common)
    plain = layout_fields(4, (1 << 18) - 1, 680108)
    assert "weight_bits" not in validate_layout(plain, **common)
    with pytest.raises(RecordReceiptError, match="weight"):
        validate_layout({**plain, "weight_bits": "0"}, **common)


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


def test_current_policy_names_select_mechanisms_not_bit_presets():
    from scripts.experiments.ecg.policy_specs import parse_policy_spec
    for text, mechanism, policy in (
        ("ECG", "replacement-prefetch", "ECG"),
        ("ECG:replacement-prefetch", "replacement-prefetch", "ECG"),
        ("ECG:replacement", "replacement", "ECG"),
        ("ECG:prefetch", "prefetch", "LRU"),
        ("ECG:transport", "transport", "LRU"),
    ):
        spec = parse_policy_spec(text)
        assert spec.record_mechanism == mechanism and spec.policy == policy
        assert spec.ecg_mode is None
    with pytest.raises(ValueError):
        parse_policy_spec("ECG:UNCHARGED")
    assert parse_policy_spec("ECG:REF32_SCALE_R_COMMIT").record_mechanism is None


def test_current_admission_rejects_unimplemented_shapes(monkeypatch):
    from scripts.experiments.ecg import roi_matrix
    spec = roi_matrix.parse_policy_spec("ECG")
    args = SimpleNamespace(
        benchmark="pr", prefetcher="none", flowthrough="off", ecg_charged=1,
        cache_stream_prefetch_degree=0, line_size="64",
        options="-g 8 -k 4 -o 5 -n 1 -i 2 -t 0",
        cache_sim_omp_threads=1, gem5_cpu_type="O3", gem5_max_insts="0")
    monkeypatch.setattr(roi_matrix, "selected_gem5_isa", lambda: "riscv")
    assert not roi_matrix.current_record_error(args, spec, "gem5")
    assert not roi_matrix.current_record_error(args, spec, "cache_sim")
    args.gem5_max_insts = 100
    assert "uncapped" in roi_matrix.current_record_error(args, spec, "gem5")
    args.options = "-g 8"
    assert "explicit fixed" in roi_matrix.current_record_error(args, spec, "cache_sim")


def test_valid_record_receipt_never_reopens_an_earlier_error():
    from scripts.experiments.ecg import roi_matrix
    guest, runtime = native_fixture()
    args = SimpleNamespace(
        ecg_record_bytes=0, ecg_record_minimum_mantissa_bits=0, has_record_baseline=True)
    row = {"status": "error", "error": "geometry mismatch", "timing_valid_for_speedup": "0"}
    roi_matrix.apply_current_record_receipt(
        row, text_for(guest, runtime), args, roi_matrix.parse_policy_spec("ECG:replacement"), "gem5")
    assert row["status"] == "error" and row["timing_valid_for_speedup"] == "0"


def test_native_timing_requires_a_successful_matching_record_control():
    from scripts.experiments.ecg import roi_matrix
    guest, runtime = native_fixture()
    args = SimpleNamespace(
        ecg_record_bytes=0, ecg_record_minimum_mantissa_bits=0, has_record_baseline=True)
    row = {
        "status": "ok", "simulator": "gem5", "benchmark": "pr",
        "ecg_record_mechanism": "replacement",
    }
    roi_matrix.apply_current_record_receipt(
        row, text_for(guest, runtime), args, roi_matrix.parse_policy_spec("ECG:replacement"), "gem5")
    assert row["timing_valid_for_speedup"] == "0"
    roi_matrix.certify_current_record_results([row])
    assert row["timing_valid_for_speedup"] == "0"
    control = deepcopy(row)
    control["ecg_record_mechanism"] = "transport"
    control["policy_label"] = "ECG_TRANSPORT"
    roi_matrix.certify_current_record_results([control, row])
    assert row["timing_valid_for_speedup"] == "1"
    control["status"] = "error"
    roi_matrix.certify_current_record_results([control, row])
    assert row["status"] == "error" and row["timing_valid_for_speedup"] == "0"


def test_current_runner_accepts_exact_24_mib_modulo_geometry(tmp_path):
    import subprocess
    import sys
    from pathlib import Path
    root = Path(__file__).resolve().parents[2]
    result = subprocess.run([
        sys.executable, str(root / "scripts/experiments/ecg/roi_matrix.py"),
        "--suite", "sniper", "--benchmark", "pr", "--sniper-workload", "sg_kernel",
        "--policies", "ECG:transport", "ECG",
        "--options", "-g 8 -k 2 -o 0 -n 1 -i 2 -t 0",
        "--l3-sizes", "24MB", "--l3-ways", "16", "--out-dir", str(tmp_path),
        "--no-build", "--dry-run",
    ], cwd=root, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ECG_TRANSPORT" in result.stdout


def test_sniper_receipt_rejects_free_timing_and_missing_work():
    fields = layout_fields(4, 31, 34)
    stream = {
        **fields, "mechanism": "replacement-prefetch", "records": "34",
        "vertex_count": "32", "max_vertex_id": "31", "source_stream_bytes": "136",
        "retained_source_bytes": "136", "carrier_payload_bytes": "136",
        "carrier_allocation_bytes": "136", "auxiliary_peak_bytes": "256",
        "storage": "separate", "construction": "outside-roi",
        "guest_window_records": "16", "guest_window_data_bits": "1024",
        "guest_window_index_bits": "1024", "guest_window_valid_bits": "16",
    }
    configuration = {
        **fields, "mechanism": "replacement-prefetch", "record_count": "34",
        "vertex_count": "32", "context": "1", "line_bytes": "64",
        "core_scope": "single-core", "prefetch_request_model": "llc-only-read",
        "update_link_model": "bounded-completion-corroboration",
        "dead_miss_bypass": "llc-request-scoped", "window_entries": "16",
        "window_entry_bits": "257", "update_latency": "8",
        "update_output_width": "1", "capture_width": "1", "prefetch_queue": "8",
        "lookup_latency": "1", "drain_max_cycles": "1024", "nuca_sets": "32",
        "nuca_indexing": "mod",
    }
    runtime = {
        "mechanism": "replacement-prefetch", "record_reads": "34",
        "loaded_values": "34", "consumed_records": "34", "property_accesses": "34",
        "record_read_bytes": "136", "timing_scope": "modeled-corroboration",
        "clean": "1", "accounting": "1", "errors": "0", "pending_updates": "0",
        "pending_prefetches": "0", "prefetch_translation_faults": "0",
        "prefetch_queue_full": "0", "generated_updates": "34", "enqueued_updates": "34",
        "coalesced_updates": "0", "delivered_updates": "34", "applied_updates": "30",
        "stale_updates": "0", "expired_updates": "0", "not_resident_updates": "4",
        "max_update_occupancy": "2", "minimum_update_latency": "8",
        "prefetch_candidates": "20", "prefetch_enqueued": "18",
        "prefetch_pending_duplicates": "2", "prefetch_issued": "10",
        "prefetch_private_duplicates": "5", "prefetch_llc_duplicates": "2",
        "prefetch_issue_admission_drops": "1", "prefetch_fills": "7",
        "prefetch_completion_private_duplicates": "1", "prefetch_completion_resident": "1",
        "prefetch_completion_admission_drops": "1", "prefetch_demand_merges": "0",
        "prefetch_translation_requests": "18", "prefetch_translation_bypasses": "0",
        "prefetch_request_bytes": "640", "prefetch_fill_bytes": "448",
        "prefetch_private_lookups": "28", "prefetch_llc_lookups": "22",
        "prefetch_issue_admission_checks": "11", "prefetch_completion_admission_checks": "8",
        "lookup_cycles_charged": "100", "drain_cycles_charged": "8",
        "max_prefetch_occupancy": "1", "prefetch_latency_cycles": "700",
    }

    def text(values):
        sections = [
            ("SNIPER-ECG-RECORD-STREAM", stream),
            ("SNIPER-ECG-RECORD-CONFIG", configuration),
            ("SNIPER-ECG-RECORD", values),
            ("ECG-CONTEXT-READY", {"sim": "sniper", "loaded": "1", "reref": "0"}),
            ("ECG-PR-RESULT", {"iterations": "1", "semantic_edges": "34",
                              "score_checksum": "1234567890abcdef"}),
        ]
        return "\n".join("[" + name + " " + " ".join(
            f"{key}={value}" for key, value in data.items()) + "]" for name, data in sections)

    assert validate_sniper_record(text(runtime), mechanism="replacement-prefetch")["record_contract_valid"] == 1
    for key, value in (
        ("minimum_update_latency", "7"), ("lookup_cycles_charged", "0"),
        ("prefetch_fills", "10"), ("record_reads", "33"), ("pending_updates", "1"),
    ):
        altered = {**runtime, key: value}
        with pytest.raises(RecordReceiptError):
            validate_sniper_record(text(altered), mechanism="replacement-prefetch")


def test_equivalence_requires_complete_actual_load_fingerprints():
    fields = (
        "source_order_digest", "carrier_digest", "consumed_semantic_digest",
        "destination_stream_digest", "window_reference_digest",
    )
    text = "[ECG-RECORD-EQUIVALENCE schema=ecg.record-stream observer=actual-record-load " \
        "property_read_count=34 " + " ".join(f"{name}=0123456789abcdef" for name in fields) + "]"
    assert validate_equivalence(text, 34)["property_read_count"] == 34
    with pytest.raises(RecordReceiptError):
        validate_equivalence(text, 35)
    with pytest.raises(RecordReceiptError):
        validate_equivalence(text.replace("actual-record-load", "host-future-table"), 34)
    with pytest.raises(RecordReceiptError):
        validate_equivalence(text.replace("carrier_digest=0123456789abcdef", ""), 34)


def test_equivalence_observation_never_authorizes_native_timing():
    from scripts.experiments.ecg import roi_matrix
    guest, runtime = native_fixture()
    args = SimpleNamespace(
        ecg_record_bytes=0, ecg_record_minimum_mantissa_bits=0, has_record_baseline=True)
    row = {
        "status": "ok", "simulator": "gem5", "benchmark": "pr",
        "ecg_record_mechanism": "replacement", "ecg_equivalence_only": 1,
    }
    roi_matrix.apply_current_record_receipt(
        row, text_for(guest, runtime), args, roi_matrix.parse_policy_spec("ECG:replacement"), "gem5")
    control = deepcopy(row)
    control["ecg_record_mechanism"] = "transport"
    roi_matrix.certify_current_record_results([control, row])
    assert row["timing_valid_for_speedup"] == "0"
    assert row["timing_model"] == "record_equivalence_diagnostic"


def test_current_comparison_rejects_a_legacy_csr_workload():
    from scripts.experiments.ecg import roi_matrix
    rows = [
        {"simulator": "cache_sim", "policy_label": "POPT_SE", "popt_se_reconstruction": 1,
         "status": "ok", "pr_iterations": 2, "pr_semantic_edges": 6898,
         "pr_score_checksum": "259bae42decebd88", "pr_workload_contract": "legacy"},
        {"simulator": "cache_sim", "policy_label": "ECG_TRANSPORT", "status": "ok",
         "ecg_record_mechanism": "transport", "pr_iterations": 2,
         "pr_semantic_edges": 6898, "pr_score_checksum": "f157f41979260953",
         "pr_workload_contract": "fixed-pull-gs"},
    ]
    args = SimpleNamespace(benchmark="pr", suite="cache-sim", current_pr_baselines=False)
    roi_matrix.certify_cache_sim_pr_results(rows, args)
    assert all(row["status"] == "error" for row in rows)


def test_fixed_workload_receipt_requires_all_csr_indices_and_work():
    text = "[ECG-PR-WORKLOAD traversal=pull-gs arithmetic=separate-f32 carrier=csr vertices=32 records=34 csr_index_reads=256]"
    result = validate_pr_workload(text, carrier="csr", iterations=2, semantic_edges=68)
    assert result["pr_workload_contract"] == "fixed-pull-gs"
    for bad in (
        text.replace("csr_index_reads=256", "csr_index_reads=0"),
        text.replace("records=34", "records=33"),
        text.replace("carrier=csr", "carrier=record"),
        text.replace("separate-f32", "unspecified"),
    ):
        with pytest.raises(RecordReceiptError):
            validate_pr_workload(bad, carrier="csr", iterations=2, semantic_edges=68)
