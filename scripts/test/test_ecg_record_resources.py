import hashlib
import io
import struct

import pytest

from scripts.experiments.ecg.record_resources import (
    GraphInfo, RecordResourceError, graph_info, plan_resources,
)


def test_vertex_estimation_reads_only_the_serialized_header():
    from scripts.experiments.ecg.roi_matrix import graph_vertices_from_sg

    class HeaderOnly:
        def read_bytes(self):
            raise AssertionError("vertex estimation must not allocate the entire graph")

        def open(self, mode):
            assert mode == "rb"
            return io.BytesIO(struct.pack("<?qq", False, 2, 32))

    assert graph_vertices_from_sg(HeaderOnly()) == 32


def test_preflight_preserves_isolated_vertices_and_actual_pull_ids(tmp_path):
    path = tmp_path / "sink.sg"
    # Only edge 0 -> 31: pull records encode source zero, not sink vertex 31.
    data = struct.pack("<?qq", True, 1, 32)
    data += struct.pack("<33q", 0, *([1] * 32)) + struct.pack("<i", 31)
    data += struct.pack("<33q", *([0] * 32), 1) + struct.pack("<i", 0)
    data += struct.pack("<32i", *range(32))
    path.write_bytes(data)
    info = graph_info(path)
    assert info.vertices == 32 and info.records == 1 and info.maximum_id == 0
    assert info.sha256 == hashlib.sha256(data).hexdigest()
    resources = plan_resources(
        info, traversals=2, requested_bytes=0, minimum_mantissa_bits=0,
        carrier_limit=256 << 20, auxiliary_limit=256 << 20,
        rss_mib=2048, backend="gem5", target_memory_bytes=4 << 30, equivalence=True)
    assert resources["record_bytes"] == 4 and resources["id_bits"] == 1
    path.write_bytes(data[:-1])
    with pytest.raises(RecordResourceError, match="size"):
        graph_info(path)


def test_resource_limits_fail_before_launch():
    graph = GraphInfo(False, 512, 2048, 511, 16409, "0" * 64)
    kwargs = dict(
        traversals=2, requested_bytes=8, minimum_mantissa_bits=0,
        carrier_limit=256 << 20, auxiliary_limit=256 << 20,
        rss_mib=2048, backend="gem5", target_memory_bytes=4 << 30)
    assert plan_resources(graph, **kwargs)["carrier_payload_bytes"] == 16384
    for change, reason in (
        ({"carrier_limit": 8192}, "carrier needs"),
        ({"auxiliary_limit": 0}, "positive"),
        ({"target_memory_bytes": 1 << 20}, "target memory"),
        ({"rss_mib": 64}, "host memory"),
        ({"carrier_limit": 1 << 64}, "64-bit"),
    ):
        with pytest.raises(RecordResourceError, match=reason):
            plan_resources(graph, **{**kwargs, **change})
    large = GraphInfo(False, 1 << 20, 1 << 23, (1 << 20) - 1, 40 << 20, "0" * 64)
    with pytest.raises(RecordResourceError, match="bounded small"):
        plan_resources(large, **kwargs, equivalence=True)


@pytest.mark.parametrize("ordering", ["", "-o 5", "-o 0 -o 5", "-o 0 -o5"])
def test_preflight_rejects_postinspection_reordering(monkeypatch, ordering):
    from scripts.experiments.ecg import roi_matrix

    graph = GraphInfo(True, 32, 1, 0, 681, "0" * 64)
    monkeypatch.setattr(roi_matrix, "graph_info", lambda _path: graph)
    args = roi_matrix.parse_args([
        "--suite", "cache-sim", "--policies", "ECG",
        "--options", f"-f prepared.sg {ordering} -n 1 -i 2 -t 0",
    ])
    error = roi_matrix.current_record_error(
        args, roi_matrix.parse_policy_spec("ECG"), "cache_sim")
    assert "prepared" in error and "-o 0" in error


def test_rss_guard_preserves_only_explicit_sealed_descriptors(tmp_path):
    import os
    from pathlib import Path
    import subprocess
    import sys
    descriptor = os.memfd_create("record-test")
    try:
        os.write(descriptor, b"sealed-input")
        os.lseek(descriptor, 0, os.SEEK_SET)
        root = Path(__file__).resolve().parents[2]
        log = tmp_path / "guard.log"
        result = subprocess.run([
            sys.executable, str(root / "scripts/test/sniper_rss_watch.py"),
            "--rss-mib", "256", "--seconds", "5", "--log", str(log),
            "--pass-fd", str(descriptor), "--", sys.executable, "-c",
            f"import os; assert os.read({descriptor}, 12) == b'sealed-input'",
        ], pass_fds=(descriptor,), capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr + log.read_text()
    finally:
        os.close(descriptor)


def test_resource_cache_does_not_change_matrix_configuration_hash(monkeypatch):
    from scripts.experiments.ecg import roi_matrix
    args = roi_matrix.parse_args(["--suite", "cache-sim", "--policies", "ECG"])
    policies = [roi_matrix.parse_policy_spec("ECG")]
    monkeypatch.setattr(roi_matrix, "hash_input_path", lambda _path: "fixed")
    before = roi_matrix.standalone_matrix_config_hash(args, policies)
    args._record_resource_plans = {("cache_sim", args.options): {"planned_target_bytes": 123}}
    assert roi_matrix.standalone_matrix_config_hash(args, policies) == before


@pytest.mark.parametrize("helper", [
    "record_receipts.py", "record_resources.py", "sniper_rss_watch.py",
    "ecg_record_evidence.h",
])
def test_current_record_helpers_are_material_configuration_inputs(monkeypatch, helper):
    from scripts.experiments.ecg import roi_matrix

    args = roi_matrix.parse_args(["--suite", "cache-sim", "--policies", "ECG"])
    policies = [roi_matrix.parse_policy_spec("ECG")]
    changed = False
    monkeypatch.setattr(
        roi_matrix, "hash_input_path",
        lambda path: "changed" if changed and path.name == helper else "original")
    before = roi_matrix.standalone_matrix_config_hash(args, policies)
    changed = True
    assert roi_matrix.standalone_matrix_config_hash(args, policies) != before


def test_runner_preserves_declared_repository_scratch(monkeypatch, tmp_path):
    from scripts.experiments.ecg import roi_matrix

    monkeypatch.setattr(roi_matrix, "PROJECT_ROOT", tmp_path)
    scratch = tmp_path / "results/run/matrix/scratch"
    scratch.mkdir(parents=True)
    environment = roi_matrix.sanitize_subprocess_environment({
        "GRAPHBREW_JOB_SCRATCH": str(scratch),
        "TMPDIR": str(scratch),
        "LD_PRELOAD": "unexpected.so",
    })
    assert environment["TMPDIR"] == str(scratch)
    assert environment["GRAPHBREW_JOB_SCRATCH"] == str(scratch)
    assert "LD_PRELOAD" not in environment
    assert roi_matrix.sanitize_subprocess_environment(environment) == environment


@pytest.mark.parametrize("invalid", ["external", "relative", "missing", "conflicting"])
def test_runner_rejects_invalid_declared_scratch(monkeypatch, tmp_path, invalid):
    from scripts.experiments.ecg import roi_matrix

    monkeypatch.setattr(roi_matrix, "PROJECT_ROOT", tmp_path)
    scratch = tmp_path / "results/scratch"
    scratch.mkdir(parents=True)
    environment = {"GRAPHBREW_JOB_SCRATCH": str(scratch), "TMPDIR": str(scratch)}
    if invalid == "external":
        environment["GRAPHBREW_JOB_SCRATCH"] = str(tmp_path)
    elif invalid == "relative":
        environment["GRAPHBREW_JOB_SCRATCH"] = "results/scratch"
    elif invalid == "missing":
        environment["GRAPHBREW_JOB_SCRATCH"] = str(tmp_path / "results/missing")
    else:
        environment["TMPDIR"] = str(tmp_path)
    with pytest.raises(ValueError, match="scratch"):
        roi_matrix.sanitize_subprocess_environment(environment)
