from pathlib import Path

from scripts.experiments.ecg.roi_matrix import (
    validate_sniper_exact_bind_trace,
)
from types import SimpleNamespace
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.experiments.ecg import roi_matrix  # noqa: E402
from scripts.experiments.ecg.flows import aggregate_results  # noqa: E402


def test_sg_kernel_counts_static_edge_visits_in_all_kernels():
    source = (ROOT / "bench/src_sniper/sg_kernel.cc").read_text()
    assert "class SemanticEdgeBudget" in source
    assert 'std::getenv("SNIPER_SEMANTIC_EDGE_LIMIT")' in source
    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        assert f'semantic_edges.report("{benchmark}")' in source
    assert source.count("SemanticEdgeBudget semantic_edges;") == 5
    assert source.count("catch (const SemanticEdgeLimitReached&)") == 5
    assert source.count("consume_edge();") == 20
    assert "relax_compact_edges(node, source_dist);" in source
    assert source.count("execute_roi([] {}, [] {});") == 5
    assert source.count("semantic_edges.finish_roi();") == 5


def test_runner_exposes_semantic_edge_limit_and_marker_gate():
    runner = (ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()
    experiment_run = (
        ROOT / "scripts/experiments/ecg/flows/experiment_run.py").read_text()
    assert "--sniper-semantic-edge-limit" in runner
    assert 'env["SNIPER_SEMANTIC_EDGE_LIMIT"]' in runner
    assert "Sniper semantic edge-limit marker missing" in runner
    assert "semantic_work_matched" in runner
    assert "sniper_reuse_bind_exact_validated" in runner
    assert "sniper_transport_receipts_validated" in runner
    assert "--sniper-semantic-edge-limit" in experiment_run


def test_exact_bind_trace_matches_receipt_line(tmp_path: Path):
    log = tmp_path / "sniper.log"
    log.write_text(
        "[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=0 core=0 "
        "bound=0x1044 line=0x1040 size=64 current=17 context=3]\n"
        "[ECG-ReusePlan-FUSED-RECV sim=sniper seq=0 src=5 line=1 "
        "addr=0x1044 addr_line=0x1040 exact=1 requested=17 "
        "vpl=16 index=7 begin=2 end=9 "
        "dest=17 tier=1 epoch1=20 epoch2=21]\n")
    assert validate_sniper_exact_bind_trace(log) == (1, 0)

    log.write_text(log.read_text().replace("addr=0x1044", "addr=0x1048"))
    assert validate_sniper_exact_bind_trace(log) == (1, 1)


def test_exact_bind_trace_rejects_invalid_context_and_line_size(tmp_path: Path):
    log = tmp_path / "sniper.log"
    log.write_text(
        "[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=0 core=0 "
        "bound=0x1044 line=0x1040 size=63 current=17 context=0]\n"
        "[ECG-ReusePlan-FUSED-RECV sim=sniper seq=0 src=5 line=1 "
        "addr=0x1044 addr_line=0x1040 exact=1 requested=17 "
        "vpl=16 index=7 begin=2 end=9 "
        "dest=17 tier=1 epoch1=20 epoch2=21]\n")
    assert validate_sniper_exact_bind_trace(log) == (1, 1)


def test_exact_bind_trace_rejects_duplicate_sequence(tmp_path: Path):
    log = tmp_path / "sniper.log"
    bind = (
        "[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=0 core=0 "
        "bound=0x1044 line=0x1040 size=64 current=17 context=3]\n")
    receipt = (
        "[ECG-ReusePlan-FUSED-RECV sim=sniper seq=0 src=5 line=1 "
        "addr=0x1044 addr_line=0x1040 exact=1 requested=17 "
        "vpl=16 index=7 begin=2 end=9 "
        "dest=17 tier=1 epoch1=20 epoch2=21]\n")
    log.write_text(bind + bind + receipt)
    assert validate_sniper_exact_bind_trace(log) == (1, 1)


def test_fused_index_preserves_distinct_same_line_records(tmp_path: Path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available")
    source = tmp_path / "fused_index_test.cc"
    binary = tmp_path / "fused_index_test"
    source.write_text(
        """
#include <cstdint>
#include <vector>

#include "ecg_fused_reuse_plan_index.h"

int main() {
    const std::vector<uint32_t> destinations{16, 17};
    const std::vector<uint64_t> records{
        (uint64_t{1} << 32) | (uint64_t{3} << 34) | uint64_t{16},
        (uint64_t{2} << 32) | (uint64_t{9} << 34) | uint64_t{17},
    };
    uint64_t position = 99;
    if (!ecg_reuse_plan::findFusedDestination(
            destinations, 0, 2, 16, 17, position) ||
        position != 0 || records[position] == records[1]) {
        return 1;
    }
    if (!ecg_reuse_plan::findFusedDestination(
            destinations, 0, 2, 17, 18, position) ||
        position != 1 || records[position] == records[0]) {
        return 2;
    }
    if (!ecg_reuse_plan::findFusedDestination(
            destinations, 0, 2, 16, 32, position) ||
        position != 0) {
        return 3;
    }
    return 0;
}
""")
    subprocess.run(
        [
            compiler, "-std=c++17", "-O2",
            "-I", str(
                ROOT / "bench/include/sniper_sim/overlays/common/core/"
                "memory_subsystem/cache"),
            str(source), "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_exact_bind_certification_requires_stable_trace_budget():
    with pytest.raises(RuntimeError, match="requires.*>= 32"):
        roi_matrix.require_sniper_reuse_plan_certification_budget(
            {"ECG_REUSE_PLAN_DELIVERY_TRACE": "31"})
    assert roi_matrix.require_sniper_reuse_plan_certification_budget(
        {"ECG_REUSE_PLAN_DELIVERY_TRACE": "32"}) == 32


def test_nominal_record_width_uses_explicit_cell_receipt(monkeypatch):
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        '{"ECG_EXPECT_BYTES_PER_EDGE":"4"}')
    assert roi_matrix.explicit_ecg_record_bytes(8) == 4
    env = {}
    roi_matrix.apply_sniper_transport_cell_env(env)
    assert env == {"ECG_EXPECT_BYTES_PER_EDGE": "4"}
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        '{"ECG_EDGE_RECORD_BYTES":"8"}')
    assert roi_matrix.explicit_ecg_record_bytes(4) == 8


def test_instruction_and_semantic_caps_are_mutually_exclusive():
    with pytest.raises(SystemExit, match="mutually exclusive"):
        roi_matrix.main([
            "--suite", "sniper",
            "--dry-run",
            "--sniper-roi-icount", "100",
            "--sniper-semantic-edge-limit", "100",
        ])


def test_semantic_cap_requires_single_core_sg_kernel():
    with pytest.raises(SystemExit, match="requires --sniper-workload sg_kernel"):
        roi_matrix.main([
            "--suite", "sniper",
            "--dry-run",
            "--sniper-workload", "pr_kernel_smoke",
            "--sniper-semantic-edge-limit", "100",
        ])
    with pytest.raises(SystemExit, match="requires --ecg-isa-variant computed"):
        roi_matrix.main([
            "--suite", "sniper",
            "--dry-run",
            "--sniper-workload", "sg_kernel",
            "--ecg-isa-variant", "indexed",
            "--sniper-semantic-edge-limit", "100",
        ])
    with pytest.raises(SystemExit, match="requires --sniper-cores 1"):
        roi_matrix.main([
            "--suite", "sniper",
            "--dry-run",
            "--sniper-workload", "sg_kernel",
            "--ecg-isa-variant", "computed",
            "--sniper-cores", "2",
            "--sniper-semantic-edge-limit", "100",
        ])
    with pytest.raises(SystemExit, match="every --threads value"):
        roi_matrix.main([
            "--suite", "sniper",
            "--dry-run",
            "--sniper-workload", "sg_kernel",
            "--ecg-isa-variant", "computed",
            "--threads", "1", "2",
            "--sniper-semantic-edge-limit", "100",
        ])


def test_semantic_work_is_certified_only_after_cross_policy_match():
    args = SimpleNamespace(
        suite="sniper",
        sniper_semantic_edge_limit=100,
    )
    policies = [
        SimpleNamespace(label="LRU"),
        SimpleNamespace(label="ECG_REUSE_PLAN"),
    ]
    rows = [
        {
            "simulator": "sniper",
            "benchmark": "pr",
            "options": "-g 12",
            "l3_size": "128kB",
            "l3_ways": 16,
            "threads": 1,
            "sniper_cores": 1,
            "policy": policy,
            "policy_label": policy,
            "status": "ok",
            "sniper_semantic_edge_limit": 100,
            "sniper_semantic_edge_visits": 100,
            "sniper_semantic_truncated": 1,
            "sniper_semantic_result": "same",
            "sniper_transport_record_bytes": 4,
            "edge_stream_bytes_per_edge": 4,
        }
        for policy in ("LRU", "ECG_REUSE_PLAN")
    ]
    roi_matrix.certify_sniper_semantic_work(rows, args, policies)
    assert all(row["semantic_work_matched"] == 1 for row in rows)

    rows[1]["sniper_semantic_edge_visits"] = 99
    rows[1]["status"] = "ok"
    rows[0]["status"] = "ok"
    roi_matrix.certify_sniper_semantic_work(rows, args, policies)
    assert all(row["semantic_work_matched"] == 0 for row in rows)
    assert all(row["status"] == "error" for row in rows)

    rows[1]["sniper_semantic_edge_visits"] = 100
    rows[1]["sniper_transport_record_bytes"] = 8
    rows[1]["status"] = "ok"
    rows[0]["status"] = "ok"
    roi_matrix.certify_sniper_semantic_work(rows, args, policies)
    assert all(
        row["error"] ==
        "Sniper transport width differs across policy rows"
        for row in rows)


def test_single_policy_shard_waits_for_aggregate_certification(monkeypatch):
    args = SimpleNamespace(
        suite="sniper",
        sniper_semantic_edge_limit=100,
    )
    policies = [SimpleNamespace(label="LRU")]
    rows = [{
        "simulator": "sniper",
        "benchmark": "pr",
        "options": "-g 12",
        "l3_size": "128kB",
        "l3_ways": 16,
        "threads": 1,
        "sniper_cores": 1,
        "policy": "LRU",
        "policy_label": "LRU",
        "status": "ok",
        "sniper_semantic_edge_limit": 100,
        "sniper_semantic_edge_visits": 100,
        "sniper_semantic_truncated": 1,
        "sniper_semantic_result": "same",
        "sniper_transport_record_bytes": 4,
        "edge_stream_bytes_per_edge": 4,
        "semantic_work_matched": 0,
    }]
    monkeypatch.setenv(
        "GRAPHBREW_EXPECTED_POLICY_LABELS", '["LRU","ECG_REUSE_PLAN"]')
    roi_matrix.certify_sniper_semantic_work(rows, args, policies)
    assert rows[0]["semantic_work_matched"] == 0

    merged = [
        dict(rows[0]),
        {
            **rows[0],
            "policy": "ECG",
            "policy_label": "ECG_REUSE_PLAN",
        },
    ]
    assert aggregate_results.semantic_work_group_matches(merged)
    assert all(row["semantic_work_matched"] == "1" for row in merged)


def test_exact_bind_trace_requires_full_trace_budget(tmp_path: Path):
    log = tmp_path / "sniper.log"
    log.write_text(
        "[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=0 core=0 "
        "bound=0x1044 line=0x1040 size=64 current=17 context=3]\n"
        "[ECG-ReusePlan-FUSED-RECV sim=sniper seq=0 src=5 line=1 "
        "addr=0x1044 addr_line=0x1040 exact=1 requested=17 "
        "vpl=16 index=7 begin=2 end=9 "
        "dest=17 tier=1 epoch1=20 epoch2=21]\n")
    # Without a declared budget one paired transaction is internally consistent.
    assert validate_sniper_exact_bind_trace(log) == (1, 0)
    # A certification run asking for 32 transactions must not pass on one.
    count, bad = validate_sniper_exact_bind_trace(log, 32)
    assert count == 1 and bad > 0
