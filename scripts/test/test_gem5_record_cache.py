"""Current ECG uses per-request metadata and charged, bounded native traffic."""

import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
OVERLAYS = ROOT / "bench/include/gem5_sim/overlays"
GEM5 = ROOT / "bench/include/gem5_sim/gem5"
CONFIG = ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py"


def test_layered_patch_receipt_requires_exact_material_inputs(tmp_path, monkeypatch):
    from scripts import setup_gem5
    monkeypatch.setattr(setup_gem5, "GEM5_DIR", tmp_path)
    target = tmp_path / "target.cc"
    target.write_text("installed layer followed by another installed layer\n")
    patch = tmp_path / "layer.patch"
    patch.write_text("--- a/target.cc\n+++ b/target.cc\n@@ -1 +1 @@\n-old\n+new\n")
    receipt = {
        "patches": {"layer.patch": hashlib.sha256(patch.read_bytes()).hexdigest()},
        "installed_targets": {"target.cc": hashlib.sha256(target.read_bytes()).hexdigest()},
    }
    assert setup_gem5.patch_receipt_matches(patch, "layer.patch", receipt)
    target.write_text("unverified target drift\n")
    assert not setup_gem5.patch_receipt_matches(patch, "layer.patch", receipt)
    receipt["installed_targets"]["target.cc"] = hashlib.sha256(target.read_bytes()).hexdigest()
    patch.write_text(patch.read_text() + "unverified patch drift\n")
    assert not setup_gem5.patch_receipt_matches(patch, "layer.patch", receipt)


def test_record_request_association_cpp(tmp_path):
    binary = tmp_path / "observation"
    result = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "bench/include"), "-I", str(OVERLAYS),
         "-I", str(GEM5 / "build/RISCV"), "-I", str(GEM5 / "src"),
         str(ROOT / "bench/src_sim/test_ecg_record_observation.cc"), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr


def test_record_guest_environment_keeps_fixed_layout(monkeypatch):
    import importlib.util
    helper_path = CONFIG.with_name("graph_env_layout.py")
    spec = importlib.util.spec_from_file_location("record_environment", helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    tree = ast.parse(CONFIG.read_text())
    functions = [
        node for node in tree.body if isinstance(node, ast.FunctionDef) and
        node.name in ("needs_vertex_hints", "benchmark_environment")]
    namespace = {
        "os": os, "finalize_environment": helper.finalize_environment,
        "RUNTIME_SIDEBAND_FILES": (
            ("GEM5_GRAPHBREW_CTX", "/tmp/context.json"),
            ("GEM5_POPT_MATRIX", "/tmp/popt.bin"),
            ("GEM5_GRAPHBREW_OUT_EDGES", "/tmp/out.bin"),
            ("GEM5_GRAPHBREW_IN_EDGES", "/tmp/in.bin")),
    }
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(CONFIG), "exec"), namespace)
    for name in tuple(os.environ):
        if name.startswith(("ECG_", "GEM5_")):
            monkeypatch.delenv(name)
    for width in (0, 4, 8):
        args = SimpleNamespace(
            policy="ECG", ecg_mode="DBG_PRIMARY", prefetcher="none",
            ref32_native=False, ecg_native=True, ecg_record_bytes=width,
            ecg_minimum_mantissa_bits=0)
        environment = namespace["benchmark_environment"](args)
        assert len(environment) == helper.TARGET_ENV_ENTRIES
        assert sum(len(value.encode()) + 1 for value in environment) == helper.TARGET_ENV_BYTES
        assert "ECG_RECORD_NATIVE=1" in environment
        assert f"ECG_RECORD_BYTES={width}" in environment
        assert "GEM5_ENABLE_VERTEX_HINTS=0" in environment
        assert not any(value.startswith("ECG_REF32_") for value in environment)


def test_l3_geometry_preserves_24_mib_and_sixteen_ways(monkeypatch):
    path = CONFIG.with_name("graph_cache_config.py")
    function = next(
        node for node in ast.parse(path.read_text()).body
        if isinstance(node, ast.FunctionDef) and node.name == "make_l3_cache")
    namespace = {
        "os": os, "DEFAULTS": {"l3_size": "8MB", "l3_assoc": 16},
        "size_to_bytes": lambda size: int(size[:-2]) * 1024 * 1024,
        "Cache": lambda **kwargs: SimpleNamespace(**kwargs),
        "BaseSetAssoc": lambda **kwargs: SimpleNamespace(**kwargs),
        "GraphModuloSetAssociative": lambda **kwargs: SimpleNamespace(
            kind="modulo", **kwargs),
        "make_replacement_policy": lambda *args, **kwargs: "replacement",
    }
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), "exec"), namespace)
    cache = namespace["make_l3_cache"]("LRU", "24MB", 16)
    assert cache.size == "24MB" and cache.assoc == 16
    assert hasattr(cache, "tags"), "24 MiB/16 ways cannot use gem5's power-of-two indexing"
    assert cache.tags.indexing_policy.kind == "modulo"
    assert cache.tags.indexing_policy.size == f"{24 * 1024 * 1024}B"
    assert cache.tags.indexing_policy.assoc == 16
    assert cache.tags.indexing_policy.entry_size == 64


def test_modulo_index_round_trips_every_requested_set(tmp_path):
    source = tmp_path / "modulo.cc"
    source.write_text(r'''
#include <cstdint>
#include <initializer_list>
#include "cache_indexing.h"
namespace Index = cache_indexing;
int main() {
    for (const uint64_t sets : {uint64_t{8192}, uint64_t{24576}, uint64_t{3}}) {
        for (uint64_t set = 0; set < sets; ++set) {
            for (const uint64_t generation : {uint64_t{0}, uint64_t{1}, uint64_t{999999}}) {
                const uint64_t original = ((generation * sets + set) << 6) + 17;
                uint64_t tag = 0, found = 0, restored = 0;
                if (!Index::split(original, 6, sets, tag, found) || found != set ||
                    tag != generation || !Index::restore(tag, found, 6, sets, restored) ||
                    restored != original - 17) return 1;
            }
        }
        uint64_t tag = 0, set = 0, restored = 0;
        if (!Index::split(UINT64_MAX, 6, sets, tag, set) ||
            !Index::restore(tag, set, 6, sets, restored) ||
            restored != UINT64_MAX - 63 ||
            Index::restore(UINT64_MAX, set, 6, sets, restored)) return 2;
    }
    return 0;
}
''')
    binary = tmp_path / "modulo"
    compiled = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "bench/include"),
         str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr


def test_record_update_and_prefetch_paths_do_not_touch_recency():
    policy = (OVERLAYS / "mem/cache/replacement_policies/graph_ecg_record_rp.cc").read_text()
    apply = policy.split("GraphEcgRecordRP::applyEcgRecordUpdate", 1)[1].split(
        "GraphEcgRecordRP::canAdmitEcgRecordPrefetch", 1)[0]
    assert "receiver.apply" in apply
    assert "recency" not in apply and "accessBlock" not in apply
    patch = (OVERLAYS / "mem/cache/ecg_record_prefetch_cache.patch").read_text()
    assert "blk != nullptr && !ecgRecordFillDecision(pkt->req)" in patch
    assert "record_prefetch_only" in patch and "record_prefetch->admitted" in patch
    source = (OVERLAYS / "mem/cache/prefetch/ecg_record_prefetch.cc").read_text()
    assert "translateTiming(" in source and "sendTimingReq(" in source
    assert "sendFunctional(" not in source and "sendAtomic(" not in source
    assert "getConstPtr<uint8_t>()" in source
    assert "selectWindowTarget(" in source and "req->hasVaddr()" in source
    assert "l1->inCache" in source and "l2->inCache" in source and "llc->inCache" in source
    assert "reserveLookup()" in source and "progressLimit" in source


def native_ready():
    return ((GEM5 / "build/RISCV/params/EcgRecordTransport.hh").is_file() and
            (ROOT / "bench/bin_gem5/record_isa_smoke_riscv_m5ops").is_file())


def native_run(output, binary, options, policy, mechanism="replacement", width=0,
               *, l3_size="4kB", l1_size="1kB", l2_size="2kB"):
    output.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))}
    env.update({
        "OMP_NUM_THREADS": "1",
        "GEM5_GRAPHBREW_CTX": str(output / "context.json"),
        "GEM5_POPT_MATRIX": str(output / "popt.bin"),
        "GEM5_GRAPHBREW_OUT_EDGES": str(output / "out.bin"),
        "GEM5_GRAPHBREW_IN_EDGES": str(output / "in.bin"),
    })
    command = [
        str(GEM5 / "build/RISCV/gem5.opt"), "--outdir", str(output), str(CONFIG),
        "--binary", str(binary), "--options", options, "--cpu-type", "O3",
        "--policy", policy, "--prefetcher", "none", "--ecg-native",
        "--ecg-mechanism", mechanism, "--ecg-record-bytes", str(width),
        "--l1d-size", l1_size, "--l2-size", l2_size, "--l3-size", l3_size,
        "--l3-ways", "16",
    ]
    result = subprocess.run(
        command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
    text = result.stdout + result.stderr
    (output / "simulator.log").write_text(text)
    for name in ("benchmark_stdout.txt", "benchmark_stderr.txt"):
        path = output / name
        if path.is_file():
            text += path.read_text()
    assert result.returncode == 0, text[-10000:]
    found = re.search(r"\[ECG-RECORD-NATIVE ([^\]]+)\]", text)
    assert found, text[-10000:]
    receipt = dict(re.findall(r"(\w+)=([^\s]+)", found.group(1)))
    assert receipt["pending"] == receipt["errors"] == receipt["required_update_drops"] == "0"
    assert receipt["accounting"] == "1"
    assert int(receipt["record_loads"]) == int(receipt["governed_loads"])
    assert int(receipt["record_read_bytes"]) == (
        int(receipt["record_loads"]) * int(receipt["record_bytes"]))
    return text, receipt


@pytest.mark.skipif(not native_ready(), reason="current native ECG binaries not built")
@pytest.mark.parametrize("width", [4, 8])
def test_record_native_raw_data_and_float_values(tmp_path, width):
    text, receipt = native_run(
        tmp_path / "isa", ROOT / "bench/bin_gem5/record_isa_smoke_riscv_m5ops",
        str(width), "LRU", width=width)
    assert f"record_bytes={width} cases=8 high_bit={int(width == 8)} result=PASS" in text
    assert receipt["governed_loads"] == "8"


@pytest.mark.skipif(not native_ready(), reason="current native ECG binaries not built")
@pytest.mark.parametrize("width", [4, 8])
def test_record_native_pagerank_mechanisms(tmp_path, width):
    semantic = []
    for policy, mechanism in (
            ("LRU", "transport"), ("ECG", "replacement"),
            ("ECG", "prefetch"), ("ECG", "replacement-prefetch")):
        text, receipt = native_run(
            tmp_path / f"{policy}-{mechanism}", ROOT / "bench/bin_gem5/pr_riscv_m5ops",
            "-g 10 -k 4 -o 5 -n 1 -i 2 -t 0", policy, mechanism, width)
        result = re.search(r"\[ECG-PR-RESULT ([^\]]+)\]", text)
        assert result, text[-10000:]
        semantic.append(result.group(1))
        if mechanism in ("replacement", "replacement-prefetch"):
            assert receipt["generated"] == receipt["governed_loads"]
            assert int(receipt["applied"]) > 0
            assert int(receipt["min_latency"]) >= 8
            assert int(receipt["max_occupancy"]) <= 16
        if mechanism in ("prefetch", "replacement-prefetch"):
            found = re.search(r"\[ECG-RECORD-PREFETCH ([^\]]+)\]", text)
            assert found, text[-10000:]
            fields = dict(re.findall(r"(\w+)=([^\s]+)", found.group(1)))
            assert fields["pending"] == fields["translation_failures"] == "0"
            assert fields["accounting"] == "1"
            assert int(fields["record_buffer_bytes"]) == (128 if width == 4 else 192)
            assert int(fields["presence_lookups"]) > 0
            assert int(fields["property_reads"]) > 0
            assert fields["property_reads"] == fields["property_responses"]
            assert fields["record_reads"] == fields["record_responses"]
    assert len(set(semantic)) == 1


@pytest.mark.skipif(not native_ready(), reason="current native ECG binaries not built")
@pytest.mark.parametrize("sample,width,size", [
    ("cit-Patents-native-n12", 4, "24MB"),
    ("com-Orkut-native-n12", 8, "64kB"),
])
def test_record_native_real_graphs_and_exact_geometry(tmp_path, sample, width, size):
    from scripts.experiments.ecg.record_receipts import validate_gem5_record
    graph = ROOT / "results/graphs" / sample / f"{sample}-dbg.sg"
    if not graph.is_file():
        pytest.skip("optional bounded real-graph qualification input is absent")
    semantics = []
    for policy, mechanism in (("LRU", "transport"), ("ECG", "replacement-prefetch")):
        output = tmp_path / policy
        text, _receipt = native_run(
            output, ROOT / "bench/bin_gem5/pr_riscv_m5ops",
            f"-f {graph} -o 0 -n 1 -i 2 -t 0", policy, mechanism, width,
            l3_size=size, l1_size="4kB", l2_size="16kB")
        fields = validate_gem5_record(text, mechanism=mechanism, requested_bytes=width)
        semantics.append((fields["pr_semantic_edges"], fields["pr_score_checksum"]))
        config = json.loads((output / "config.json").read_text())
        llc = config["system"]["l3cache"]
        assert llc["assoc"] == 16
        if size == "24MB":
            assert llc["size"] == 24 * 1024 * 1024
            assert llc["tags"]["indexing_policy"]["type"] == "GraphModuloSetAssociative"
    assert len(set(semantics)) == 1
