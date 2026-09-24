"""The kernel census divides the ROI without changing it.

The frozen metric charges the kernel for writing back whatever setup left
dirty, and a policy whose setup leaves the property array warm enters the
kernel with a different cache from one whose setup evicts it. The census makes
both visible: it marks the last-level lines that are dirty at the kernel
boundary, follows each mark to a writeback, a rewrite or the end of the run,
and splits the kernel's traffic, setup's writebacks included, at its graph
passes. It also details the kernel's first passes one by one, each with the
cache state it ended in. The C++ fixtures prove it passive, receipt for
receipt: a cache replay under LRU, FIFO, RANDOM, SRRIP and GRASP, and the
study's four roster policies through the algorithm backend, each run in a
fresh process as the matrix runs a cell. These tests hold the validators to
the census contract and require both executables to emit a detailed census
through the real runner for each row the study compares.
"""
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

# Segment counters in the receipt's order: total, memory, prefetch, writebacks,
# LLC hits and misses, property hits and misses.
_SEGMENTS = {
    "before_first_pass": (10, 2, 0, 3, 3, 2, 1, 1),
    "first_pass": (100, 20, 2, 5, 30, 20, 10, 12),
    "between_passes": (4, 1, 0, 0, 1, 1, 0, 0),
    "later_passes": (100, 15, 0, 4, 35, 15, 14, 8),
    "after_last_pass": (6, 1, 0, 2, 2, 1, 0, 0),
}
# The segments a kernel with this many passes can charge.
_CHARGED = {
    0: ("before_first_pass",),
    1: ("before_first_pass", "first_pass", "after_last_pass"),
    2: tuple(_SEGMENTS),
}
# Where the census's three setup writebacks fall, within each segment's writebacks.
_SETUP_WRITEBACKS = {
    0: {"before_first_pass": 3},
    1: {"before_first_pass": 1, "first_pass": 2},
    2: {"before_first_pass": 1, "first_pass": 2},
}


def _census(passes=2):
    from scripts.experiments.ecg.record_receipts import CENSUS_COUNTERS
    segments = {}
    for name, values in _SEGMENTS.items():
        fields = dict(zip(CENSUS_COUNTERS, values if name in _CHARGED[passes] else (0,) * len(values)))
        fields["total_offchip_traffic"] = (
            fields["memory_accesses"] + fields["prefetch_fills"] + fields["llc_writebacks"])
        fields["entry_dirty_writebacks"] = _SETUP_WRITEBACKS[passes].get(name, 0)
        segments[name] = fields
    kernel = {counter: sum(fields[counter] for fields in segments.values()) for counter in CENSUS_COUNTERS}
    census = {
        "entries": 1, "passes": passes,
        "entry_valid_lines": 64, "entry_dirty_lines": 10,
        "entry_property_lines": 40, "entry_property_dirty_lines": 6,
        "entry_dirty_writebacks": 3, "entry_dirty_rewritten": 4,
        "exit_valid_lines": 60, "exit_dirty_lines": 9,
        "exit_property_lines": 30, "exit_entry_dirty_lines": 3,
        "segments": segments,
    }
    return census, kernel


@pytest.mark.parametrize("passes", sorted(_CHARGED))
def test_a_census_that_divides_its_kernel_is_flattened_into_the_row(passes):
    from scripts.experiments.ecg.record_receipts import (
        CENSUS_COUNTERS, CENSUS_SEGMENTS, validate_kernel_census)
    census, kernel = _census(passes)
    row = validate_kernel_census(census, kernel)
    assert len(row) == 1 + 10 + len(CENSUS_SEGMENTS) * (len(CENSUS_COUNTERS) + 2)
    assert row["kernel_census_passes"] == passes
    assert row["kernel_census_entry_dirty_writebacks"] == 3
    assert row["kernel_census_exit_entry_dirty_lines"] == 3
    assert row["kernel_census_exit_valid_lines"] == 60
    assert row["kernel_census_exit_property_lines"] == 30
    for segment in CENSUS_SEGMENTS:
        charged = segment in _CHARGED[passes]
        assert row[f"kernel_census_{segment}_total_accesses"] == (_SEGMENTS[segment][0] if charged else 0)
        assert (row[f"kernel_census_{segment}_entry_dirty_writebacks"] ==
                _SETUP_WRITEBACKS[passes].get(segment, 0))
    if passes == 2:
        assert row["kernel_census_first_pass_total_offchip_traffic"] == 27
        assert row["kernel_census_later_passes_total_offchip_traffic"] == 19


def _corrupt(name):
    census, kernel = _census()
    segments = census["segments"]
    if name == "absent":
        census = None
    elif name == "two-kernels":
        census["entries"] = 2
    elif name == "unarmed":
        census["entries"] = 0
    elif name == "boolean-field":
        census["entry_valid_lines"] = True
    elif name == "negative-field":
        census["exit_dirty_lines"] = -1
    elif name == "overflowing-field":
        census["passes"] = 1 << 64
    elif name == "string-field":
        census["entry_dirty_rewritten"] = "4"
    elif name == "unaccounted-dirty-line":
        census["entry_dirty_lines"] += 1
    elif name == "property-dirty-exceeds-dirty":
        census["entry_property_dirty_lines"] = census["entry_dirty_lines"] + 1
    elif name == "dirty-exceeds-valid":
        census["entry_valid_lines"] = census["entry_dirty_lines"] - 1
    elif name == "exit-mark-exceeds-exit-dirty":
        census["exit_dirty_lines"] = census["exit_entry_dirty_lines"] - 1
    elif name == "exit-dirty-exceeds-exit-valid":
        census["exit_valid_lines"] = census["exit_dirty_lines"] - 1
    elif name == "exit-property-exceeds-exit-valid":
        census["exit_property_lines"] = census["exit_valid_lines"] + 1
    elif name == "missing-exit-residency":
        del census["exit_property_lines"]
    elif name == "missing-segment":
        del segments["between_passes"]
    elif name == "extra-segment":
        segments["setup"] = dict(segments["first_pass"])
    elif name == "missing-counter":
        del segments["first_pass"]["prefetch_fills"]
    elif name == "segment-overcounts":
        segments["later_passes"]["llc_misses"] += 1
    elif name == "kernel-undercounts":
        kernel["total_accesses"] -= 1
    elif name == "offchip-disagrees":
        segments["after_last_pass"]["total_offchip_traffic"] += 1
    elif name == "one-pass-charges-later":
        census["passes"] = 1
    elif name == "no-pass-charges-first":
        census["passes"] = 0
    elif name == "more-setup-writebacks-than-kernel":
        census["entry_dirty_writebacks"] = kernel["llc_writebacks"] + 1
        census["entry_dirty_lines"] = (census["entry_dirty_writebacks"] + census["entry_dirty_rewritten"] +
                                       census["exit_entry_dirty_lines"])
    elif name == "missing-segment-setup-writebacks":
        del segments["first_pass"]["entry_dirty_writebacks"]
    elif name == "segment-setup-writebacks-disagree":
        segments["first_pass"]["entry_dirty_writebacks"] += 1
    elif name == "setup-writeback-outside-its-segment":
        segments["before_first_pass"]["entry_dirty_writebacks"] -= 1
        segments["between_passes"]["entry_dirty_writebacks"] += 1
    else:
        raise AssertionError(name)
    return census, kernel


@pytest.mark.parametrize("name,message", [
    ("absent", "missing kernel census"),
    ("two-kernels", "exactly one kernel"),
    ("unarmed", "exactly one kernel"),
    ("boolean-field", "invalid kernel census field entry_valid_lines"),
    ("negative-field", "invalid kernel census field exit_dirty_lines"),
    ("overflowing-field", "invalid kernel census field passes"),
    ("string-field", "invalid kernel census field entry_dirty_rewritten"),
    ("unaccounted-dirty-line", "entry-dirty line unaccounted"),
    ("property-dirty-exceeds-dirty", "line counts are inconsistent"),
    ("dirty-exceeds-valid", "line counts are inconsistent"),
    ("exit-mark-exceeds-exit-dirty", "line counts are inconsistent"),
    ("exit-dirty-exceeds-exit-valid", "line counts are inconsistent"),
    ("exit-property-exceeds-exit-valid", "line counts are inconsistent"),
    ("missing-exit-residency", "invalid kernel census field exit_property_lines"),
    ("missing-segment", "segments are incomplete"),
    ("extra-segment", "segments are incomplete"),
    ("missing-counter", "invalid kernel census field prefetch_fills"),
    ("segment-overcounts", "do not sum to the kernel: llc_misses"),
    ("kernel-undercounts", "do not sum to the kernel: total_accesses"),
    ("offchip-disagrees", "traffic sum: after_last_pass"),
    ("one-pass-charges-later", "pass segment the kernel did not run"),
    ("no-pass-charges-first", "pass segment the kernel did not run"),
    ("more-setup-writebacks-than-kernel", "more setup writebacks than the kernel wrote back"),
    ("missing-segment-setup-writebacks", "invalid kernel census field entry_dirty_writebacks"),
    ("segment-setup-writebacks-disagree", "setup writebacks by segment do not sum to the census"),
    ("setup-writeback-outside-its-segment", "more setup writebacks than it wrote back: between_passes"),
])
def test_a_census_that_does_not_divide_its_kernel_is_refused(name, message):
    from scripts.experiments.ecg.record_receipts import RecordReceiptError, validate_kernel_census
    census, kernel = _corrupt(name)
    with pytest.raises(RecordReceiptError, match=message):
        validate_kernel_census(census, kernel)


# The resident, dirty, property and setup-dirty lines at the end of each
# detailed pass. The last is the census's exit, as it must be when nothing
# runs after the kernel's last pass; what runs after it brings one more
# property line in.
_ENDS = ((62, 11, 35, 5), (60, 9, 30, 3))


def _kernel_of(census):
    from scripts.experiments.ecg.record_receipts import CENSUS_COUNTERS
    return {counter: sum(fields[counter] for fields in census["segments"].values())
            for counter in CENSUS_COUNTERS}


def _detailed(passes=2, *, quiet_exit=False):
    """A census whose pass detail agrees with its segments.

    With quiet_exit nothing runs after the last pass, so the census's exit is
    the last detailed pass's end.
    """
    from scripts.experiments.ecg.record_receipts import CENSUS_PASS_ENDS
    census, _ = _census(passes)
    segments = census["segments"]
    if quiet_exit:
        segments["after_last_pass"] = dict.fromkeys(segments["after_last_pass"], 0)
    census["pass_detail_limit"] = 64
    census["pass_detail"] = [
        {**segments[segment], **dict(zip(CENSUS_PASS_ENDS, ends))}
        for segment, ends in zip(("first_pass", "later_passes")[:passes], _ENDS[-passes:])]
    if passes and not quiet_exit:
        census["pass_detail"][-1]["end_property_lines"] -= 1
    return census, _kernel_of(census)


def _many(passes, *, quiet_exit=False):
    """A census of 64 passes or more, each later pass repeating the second.

    It details its first 64. Beyond them the 63 detailed later passes fall
    short of the later-pass segment, and the passes no detail shows bring one
    more property line in before the exit.
    """
    from scripts.experiments.ecg.record_receipts import CENSUS_PASS_TRAFFIC
    census, _ = _detailed(2, quiet_exit=quiet_exit)
    first, later = census["pass_detail"]
    census["passes"] = passes
    census["segments"]["later_passes"] = {key: later[key] * (passes - 1) for key in CENSUS_PASS_TRAFFIC}
    census["pass_detail"] = [first] + [dict(later) for _ in range(63)]
    if passes > 64:
        census["exit_property_lines"] += 1
    return census, _kernel_of(census)


@pytest.mark.parametrize("passes,quiet_exit", [
    (0, False), (1, False), (1, True), (2, False), (2, True),
    (64, False), (64, True), (70, False), (70, True)])
def test_a_census_details_each_pass_it_ran_up_to_its_limit(passes, quiet_exit):
    from scripts.experiments.ecg.record_receipts import (
        CENSUS_PASS_ENDS, CENSUS_PASS_TRAFFIC, validate_kernel_census, validate_kernel_census_passes)
    census, kernel = (_many if passes >= 64 else _detailed)(passes, quiet_exit=quiet_exit)
    detail = validate_kernel_census_passes(census)
    assert len(detail) == min(passes, 64)
    assert all(set(fields) == {*CENSUS_PASS_TRAFFIC, *CENSUS_PASS_ENDS} for fields in detail)
    if passes:
        assert detail[0]["total_offchip_traffic"] == 27
        assert detail[-1]["end_entry_dirty_lines"] == 3
    # The pass detail is additive: the segment validator still divides the kernel.
    assert validate_kernel_census(census, kernel)["kernel_census_passes"] == passes


def _corrupt_detail(name):
    if name == "detail-exceeds-later-passes":
        census, _ = _many(70)
        census["segments"]["later_passes"]["llc_hits"] = 63 * census["pass_detail"][1]["llc_hits"] - 1
        return census
    if name == "at-the-limit-undercounts":
        census, _ = _many(64)
        census["segments"]["later_passes"]["llc_hits"] += 1
        return census
    if name == "at-the-limit-exit-differs":
        census, _ = _many(64, quiet_exit=True)
        census["pass_detail"][-1]["end_property_lines"] -= 1
        return census
    census, _ = _detailed(2, quiet_exit=name == "quiet-exit-differs")
    detail = census["pass_detail"]
    if name == "absent":
        census = None
    elif name == "absent-detail":
        del census["pass_detail"]
    elif name == "absent-limit":
        del census["pass_detail_limit"]
    elif name == "other-limit":
        census["pass_detail_limit"] = 32
    elif name == "detail-not-a-list":
        census["pass_detail"] = {"1": detail[0], "2": detail[1]}
    elif name == "short-detail":
        detail.pop()
    elif name == "long-detail":
        detail.append(dict(detail[-1]))
    elif name == "pass-not-an-object":
        detail[0] = 5
    elif name == "missing-end":
        del detail[1]["end_dirty_lines"]
    elif name == "extra-field":
        detail[0]["begin_dirty_lines"] = 1
    elif name == "boolean-end":
        detail[0]["end_valid_lines"] = True
    elif name == "negative-counter":
        detail[1]["llc_hits"] = -1
    elif name == "offchip-disagrees":
        detail[1]["total_offchip_traffic"] += 1
    elif name == "more-setup-writebacks-than-pass":
        detail[1]["entry_dirty_writebacks"] = detail[1]["llc_writebacks"] + 1
    elif name == "end-dirty-exceeds-valid":
        detail[0]["end_dirty_lines"] = detail[0]["end_valid_lines"] + 1
    elif name == "end-property-exceeds-valid":
        detail[0]["end_property_lines"] = detail[0]["end_valid_lines"] + 1
    elif name == "end-mark-exceeds-dirty":
        detail[0]["end_entry_dirty_lines"] = detail[0]["end_dirty_lines"] + 1
    elif name == "first-pass-overcounts":
        detail[0]["llc_misses"] += 1
    elif name == "first-pass-undercounts":
        detail[0]["llc_misses"] -= 1
    elif name == "later-passes-overcount":
        detail[1]["llc_property_hits"] += 1
    elif name == "later-passes-undercount":
        detail[1]["llc_property_hits"] -= 1
    elif name == "more-marks-than-entry-left":
        detail[0]["end_entry_dirty_lines"] = 8
    elif name == "mark-reappears":
        detail[1]["end_entry_dirty_lines"] = detail[0]["end_entry_dirty_lines"] + 1
    elif name == "fewer-marks-than-exit":
        detail[1]["end_entry_dirty_lines"] = census["exit_entry_dirty_lines"] - 1
    elif name == "quiet-exit-differs":
        detail[1]["end_property_lines"] -= 1
    else:
        raise AssertionError(name)
    return census


@pytest.mark.parametrize("name,message", [
    ("absent", "missing kernel census"),
    ("absent-detail", "invalid kernel census pass detail"),
    ("absent-limit", "invalid kernel census field pass_detail_limit"),
    ("other-limit", "pass detail limit is not 64"),
    ("detail-not-a-list", "invalid kernel census pass detail"),
    ("short-detail", "pass detail does not cover its passes"),
    ("long-detail", "pass detail does not cover its passes"),
    ("pass-not-an-object", "pass detail fields differ: pass 1"),
    ("missing-end", "pass detail fields differ: pass 2"),
    ("extra-field", "pass detail fields differ: pass 1"),
    ("boolean-end", "invalid kernel census field end_valid_lines"),
    ("negative-counter", "invalid kernel census field llc_hits"),
    ("offchip-disagrees", "invalid kernel census pass traffic sum: pass 2"),
    ("more-setup-writebacks-than-pass", "more setup writebacks than it wrote back: pass 2"),
    ("end-dirty-exceeds-valid", "pass end line counts are inconsistent: pass 1"),
    ("end-property-exceeds-valid", "pass end line counts are inconsistent: pass 1"),
    ("end-mark-exceeds-dirty", "pass end line counts are inconsistent: pass 1"),
    ("first-pass-overcounts", "first pass detail differs from its segment: llc_misses"),
    ("first-pass-undercounts", "first pass detail differs from its segment: llc_misses"),
    ("later-passes-overcount", "later pass detail does not sum to its segment: llc_property_hits"),
    ("later-passes-undercount", "later pass detail does not sum to its segment: llc_property_hits"),
    ("at-the-limit-undercounts", "later pass detail does not sum to its segment: llc_hits"),
    ("detail-exceeds-later-passes", "later pass detail exceeds its segment: llc_hits"),
    ("more-marks-than-entry-left", "keeps more setup-dirty lines than entry left: pass 1"),
    ("mark-reappears", "marks a setup-dirty line after it retired: pass 2"),
    ("fewer-marks-than-exit", "pass detail and exit disagree on setup-dirty lines"),
    ("quiet-exit-differs", "exit differs from its last pass end"),
    ("at-the-limit-exit-differs", "exit differs from its last pass end"),
])
def test_a_pass_detail_that_does_not_divide_its_passes_is_refused(name, message):
    from scripts.experiments.ecg.record_receipts import RecordReceiptError, validate_kernel_census_passes
    with pytest.raises(RecordReceiptError, match=message):
        validate_kernel_census_passes(_corrupt_detail(name))


# The four rows the study compares at every capacity, with the options its
# manifest stages give them. The fixture caches are small enough to evict.
_GEOMETRY = ("--l1d-size", "128B", "--l1d-ways", "2", "--l2-size", "256B", "--l2-ways", "2",
             "--l3-sizes", "2048B", "--l3-ways", "4")
_RULE = "--record-base-policy GRASP_PAPER --record-preprocess csr --record-governed-first on"
_SPMV_ROSTER = {
    "grasp": ("GRASP_PAPER", ""),
    "popt": ("POPT:UNCHARGED", ""),
    "rule": ("ECG:replacement", _RULE),
    "arm": ("ECG:replacement", _RULE + " --record-rrpv-order on"),
}
_PAGERANK_ROSTER = {
    "grasp": ("GRASP_PAPER", "no", "no"),
    "popt": ("POPT:UNCHARGED", "no", "no"),
    "rule": ("ECG:replacement", "on", "no"),
    "arm": ("ECG:replacement", "on", "on"),
}


def _fixture_graph(tmp_path):
    from scripts.experiments.ecg.flows.prepare_record_equivalence_graphs import algorithm_outputs
    graph = tmp_path / "pressure512.sg"
    graph.write_bytes(algorithm_outputs()[graph.name][0])
    return graph


def _check_row(name, row):
    """What every row must show, whatever the policy.

    Both kernels run two graph passes over identical work, so a census whose
    boundaries sit at the passes charges them identical accesses.
    """
    assert row.get("status") == "ok", (name, row.get("error"))
    assert row["kernel_census_passes"] == 2, name
    assert row["kernel_census_detailed_passes"] == 2, name
    assert row["kernel_census_first_pass_total_accesses"] > 0, name
    assert (row["kernel_census_first_pass_total_accesses"] ==
            row["kernel_census_later_passes_total_accesses"]), f"{name}: a pass boundary is misplaced"


def test_the_spmv_roster_carries_a_census_through_the_runner(tmp_path):
    """The runner runs each cell, validates its census and flattens it into the row.

    Across the roster the fixture must carry dirty lines into the kernel, write
    some of them back and leave dirty lines at exit, or the entry and exit
    counters were never exercised through the runner. It must also leave
    property lines resident, which this executable counts after the kernel has
    released its graph context.
    """
    from scripts.experiments.ecg import algorithm_matrix, roi_matrix
    if not (ROOT / "bench/bin_sim/algorithms").is_file():
        pytest.skip("current algorithm executable is not built")
    graph = _fixture_graph(tmp_path)
    rows = {}
    for name, (policy, extra) in _SPMV_ROSTER.items():
        out = tmp_path / name
        out.mkdir()
        args = roi_matrix.parse_args([
            "--suite", "cache-sim", "--benchmark", "spmv", "--current-algorithms",
            "--options", f"--graph {graph} --repeat 2 {extra}".strip(), "--policies", policy,
            *_GEOMETRY, "--out-dir", str(out), "--no-build",
        ])
        produced = algorithm_matrix.run_cache_cell(
            args, out, roi_matrix.parse_policy_spec(policy), "2048B",
            roi_matrix.run_command, roi_matrix.parse_size_bytes)
        assert len(produced) == 1, name
        rows[name] = produced[0]
        _check_row(name, rows[name])
    assert any(row["kernel_census_entry_dirty_writebacks"] > 0 for row in rows.values())
    assert any(row["kernel_census_exit_dirty_lines"] > 0 for row in rows.values())
    assert any(row["kernel_census_exit_property_lines"] > 0 for row in rows.values())


def test_the_pagerank_roster_carries_a_census_through_the_runner(tmp_path):
    """PageRank is a separate executable with its own row path, so it is held separately.

    Its warm replay reads each score and contribution element immediately
    before writing it, so the write hits the first level and this non-inclusive
    model never dirties the last level: every policy enters the PageRank kernel
    clean. The entry counters are exercised through the runner on SpMV only;
    here the roster must leave dirty lines and property lines at exit.
    """
    from scripts.experiments.ecg import roi_matrix
    if not (ROOT / "bench/bin_sim/pr").is_file():
        pytest.skip("current PageRank executable is not built")
    graph = _fixture_graph(tmp_path)
    rows = {}
    for name, (policy, governed, rrpv) in _PAGERANK_ROSTER.items():
        out = tmp_path / name
        out.mkdir()
        args = roi_matrix.parse_args([
            "--suite", "cache-sim", "--benchmark", "pr", "--current-pr-baselines",
            "--record-governed-first", governed,
            *(("--record-rrpv-order", rrpv) if rrpv != "no" else ()),
            "--options", f"-f {graph} -o 0 -n 1 -i 2 -t 0", "--policies", policy,
            *_GEOMETRY, "--out-dir", str(out), "--no-build",
        ])
        produced = roi_matrix.run_cache_sim(args, out, roi_matrix.parse_policy_spec(policy), "2048B")
        assert len(produced) == 1, name
        rows[name] = produced[0]
        _check_row(name, rows[name])
    assert any(row["kernel_census_exit_dirty_lines"] > 0 for row in rows.values())
    assert any(row["kernel_census_exit_property_lines"] > 0 for row in rows.values())
