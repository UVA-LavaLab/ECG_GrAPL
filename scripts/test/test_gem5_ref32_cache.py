import ast
import configparser
import os
import re
import runpy
import subprocess
import sys
from types import ModuleType, SimpleNamespace

import pytest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
OVERLAYS = ROOT / "bench/include/gem5_sim/overlays"
GEM5 = ROOT / "bench/include/gem5_sim/gem5"


def compile_and_run(source: Path, binary: Path, include_gem5: bool = False):
    command = [
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "bench/include"),
        "-I", str(OVERLAYS),
    ]
    if include_gem5:
        command.extend([
            "-I", str(GEM5 / "build/RISCV"),
            "-I", str(GEM5 / "src"),
        ])
    command.extend([str(source), "-o", str(binary)])
    compiled = subprocess.run(
        command, cwd=ROOT, capture_output=True, text=True,
        timeout=30, check=False)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    executed = subprocess.run(
        [str(binary)], cwd=ROOT, capture_output=True, text=True,
        timeout=10, check=False)
    assert executed.returncode == 0, executed.stdout + executed.stderr
    assert "[SUMMARY] failures=0" in executed.stdout


def test_ref32_native_state_cpp(tmp_path: Path):
    compile_and_run(
        ROOT / "bench/src_sim/test_ecg_ref32_native_state.cc",
        tmp_path / "test_ecg_ref32_native_state")


def test_ref32_observation_cpp(tmp_path: Path):
    compile_and_run(
        ROOT / "bench/src_sim/test_ecg_ref32_observation.cc",
        tmp_path / "test_ecg_ref32_observation",
        include_gem5=True)


def test_ref32_layered_patches_apply_to_current_gem5():
    patches = [
        OVERLAYS / "mem/cache/ecg_ref32_cache_api.patch",
        OVERLAYS / "cpu/o3/ecg_ref32_observation.patch",
        OVERLAYS / "mem/cache/ecg_ref32_mshr_observation.patch",
    ]
    for patch in patches:
        result = subprocess.run(
            ["git", "apply", "--check", str(patch)],
            cwd=GEM5, capture_output=True, text=True,
            timeout=10, check=False)
        if result.returncode != 0:
            result = subprocess.run(
                ["git", "apply", "--reverse", "--check", str(patch)],
                cwd=GEM5, capture_output=True, text=True,
                timeout=10, check=False)
        assert result.returncode == 0, (
            f"{patch.name}:\n{result.stdout}{result.stderr}")


def test_ref32_policy_registration_and_non_touching_update_path():
    policy_py = OVERLAYS / (
        "mem/cache/replacement_policies/GraphReplacementPolicies.py")
    ast.parse(policy_py.read_text())
    assert "class GraphRef32RP" in policy_py.read_text()

    cache_patch = (
        OVERLAYS / "mem/cache/ecg_ref32_cache_api.patch").read_text()
    assert "findBlock(" in cache_patch
    assert "accessBlock(" not in cache_patch
    assert "applyEcgRef32Update" in cache_patch
    assert "CommitApplyResult::UNSUPPORTED" in cache_patch

    policy = (
        OVERLAYS /
        "mem/cache/replacement_policies/graph_ref32_rp.cc").read_text()
    assert "ecg_ref32::selectVictim(" in policy
    assert "ctx.classifyGRASP(address, llcSize, hotFraction)" in policy
    assert "ctx.isEcgEpochData(update.property_vaddr)" in policy
    assert "allocate" not in policy
    assert "accessBlock" not in policy
    apply_body = policy.split(
        "GraphRef32RP::applyEcgRef32Update", 1)[1].split(
            "GraphRef32RP::disableEcgRef32", 1)[0]
    assert "lastTouchTick" not in apply_body
    assert "pkt" not in apply_body

    context = (
        OVERLAYS /
        "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
    ).read_text()
    assert "bool allow_native_ref32 = false" in context
    assert (
        "allow_native_ref32 && mode == ECGMode::ECG_REF32"
        in context)


def test_ref32_physical_only_line_binding_is_shared_by_touch_and_commit():
    header = (
        OVERLAYS /
        "mem/cache/replacement_policies/graph_ref32_rp.hh").read_text()
    policy = (
        OVERLAYS /
        "mem/cache/replacement_policies/graph_ref32_rp.cc").read_text()

    assert "ecg_ref32::NativeLineBinding binding;" in header
    assert "bool property = false;" not in header
    assert "uint64_t propertyVaddrLine = 0;" not in header
    assert policy.count("binding.bindVirtual(") >= 3
    assert policy.count("binding.bindPhysical(") >= 2
    assert "!data.binding.property" in policy

    apply_body = policy.split(
        "GraphRef32RP::applyEcgRef32Update", 1)[1].split(
            "GraphRef32RP::disableEcgRef32", 1)[0]
    assert apply_body.index("data->binding.bindVirtual(") < (
        apply_body.index("receiver.apply("))
    assert "lastTouchTick" not in apply_body
    assert "increaseRefCount" not in apply_body
    assert "accessBlock" not in apply_body


def test_transport_imports_pybind_from_simobject(monkeypatch):
    m5 = ModuleType("m5")
    m5.__path__ = []
    objects = ModuleType("m5.objects")
    objects.__path__ = []
    clocked = ModuleType("m5.objects.ClockedObject")
    clocked.ClockedObject = object
    params = ModuleType("m5.params")
    params.Param = SimpleNamespace(
        **{name: lambda *args: args
           for name in ("BaseCPU", "BaseCache", "Cycles", "Bool", "Unsigned")})
    params.__all__ = ["Param"]
    simobject = ModuleType("m5.SimObject")
    simobject.PyBindMethod = lambda name: name
    for module in (m5, objects, clocked, params, simobject):
        monkeypatch.setitem(sys.modules, module.__name__, module)
    result = runpy.run_path(str(
        OVERLAYS / "mem/cache/replacement_policies/Ref32CommitTransport.py"))
    assert result["EcgRef32CommitTransport"].cxx_exports == [
        "report", "pendingUpdates", "drainBudgetTicks"]


def test_transport_object_has_no_isa_specific_link_dependency(tmp_path):
    if not (GEM5 / "build/RISCV/params/EcgRef32CommitTransport.hh").is_file():
        pytest.skip("native commit transport parameters not generated")
    binary = tmp_path / "ref32_transport.o"
    compiled = subprocess.run([
        "g++", "-std=c++17", "-O0", "-DTRACING_ON=1",
        "-I", str(GEM5 / "build/RISCV"), "-I", str(GEM5 / "src"),
        "-I", str(GEM5 / "include"), "-I", str(GEM5 / "ext"),
        "-c", str(OVERLAYS / (
            "mem/cache/replacement_policies/ecg_ref32_commit_transport.cc")),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=60, check=False)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    symbols = subprocess.run(
        ["nm", "--undefined-only", "--demangle", str(binary)],
        capture_output=True, text=True, timeout=10, check=False)
    assert symbols.returncode == 0, symbols.stderr
    assert "gem5::RiscvISA::" not in symbols.stdout, (
        "The unconditionally registered transport must also link in X86 builds:\n"
        + "\n".join(line for line in symbols.stdout.splitlines()
                    if "gem5::RiscvISA::" in line))


def test_native_completion_drains_only_the_bounded_transport():
    source = (
        ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py").read_text()
    function = next(
        (node for node in ast.parse(source).body
         if isinstance(node, ast.FunctionDef) and
         node.name == "finish_ref32_transport"), None)
    assert function is not None, "native completion must not globally drain an exited SE CPU"
    calls = []
    state = {"pending": 0}
    link = SimpleNamespace(
        pendingUpdates=lambda: state["pending"],
        drainBudgetTicks=lambda: 64,
        report=lambda: calls.append("report"))
    def simulate(ticks):
        calls.append(ticks)
        state["pending"] = 0
    namespace = {"m5": SimpleNamespace(
        simulate=simulate,
        drain=lambda: pytest.fail("global SE drain can stall after process exit"))}
    exec(compile(ast.Module(body=[function], type_ignores=[]),
                 "finish_ref32_transport", "exec"), namespace)
    finish = namespace["finish_ref32_transport"]
    finish(link)
    assert calls == ["report"]
    state["pending"] = 2
    calls.clear()
    finish(link)
    assert calls == [64, "report"]
    state["pending"] = 1
    namespace["m5"].simulate = lambda ticks: None
    with pytest.raises(RuntimeError, match="drain"):
        finish(link)


def test_native_capture_width_tracks_the_configured_cpu():
    source = (
        ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py").read_text()
    function = next(
        node for node in ast.parse(source).body
        if isinstance(node, ast.FunctionDef) and
        node.name == "resolve_ref32_capture_width")
    namespace = {}
    exec(compile(ast.Module(body=[function], type_ignores=[]),
                 "resolve_ref32_capture_width", "exec"), namespace)
    resolve = namespace["resolve_ref32_capture_width"]
    for width in (1, 2, 4, 8, 16):
        assert resolve(width, 0) == width
    assert resolve(8, 2) == 2
    for commit_width, requested in ((0, 0), (17, 0), (8, -1), (8, 17)):
        with pytest.raises(RuntimeError, match="capture width"):
            resolve(commit_width, requested)


def run_native_ref32_pair(
        tmp_path, options, *, l3_size="4kB", l1d_size="1kB",
        l2_size="2kB", minimum_burst=1, latency=8):
    results = []
    receipts = []
    for policy in ("LRU", "ECG"):
        output = tmp_path / policy
        output.mkdir()
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))
        }
        env.update({
            "OMP_NUM_THREADS": "1",
            "GEM5_GRAPHBREW_CTX": str(output / "context.json"),
            "GEM5_POPT_MATRIX": str(output / "popt.bin"),
            "GEM5_GRAPHBREW_OUT_EDGES": str(output / "out.bin"),
            "GEM5_GRAPHBREW_IN_EDGES": str(output / "in.bin"),
        })
        command = [
            str(GEM5 / "build/RISCV/gem5.opt"), "--outdir", str(output),
            str(ROOT / "bench/include/gem5_sim/configs/graphbrew/graph_se.py"),
            "--binary", str(ROOT / "bench/bin_gem5/pr_riscv_m5ops"),
            "--options", options,
            "--cpu-type", "O3", "--policy", policy,
            "--prefetcher", "none", "--ref32-native",
            "--ref32-latency", str(latency),
            "--l1d-size", l1d_size, "--l2-size", l2_size,
            "--l3-size", l3_size, "--l3-ways", "16",
        ]
        if policy == "ECG":
            command += ["--ecg-mode", "ECG_REF32"]
        run = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
        text = run.stdout + run.stderr
        (output / "simulator.log").write_text(text)
        for name in ("benchmark_stdout.txt", "benchmark_stderr.txt"):
            path = output / name
            if path.exists():
                text += path.read_text()
        assert run.returncode == 0, text[-8000:]
        semantic = re.search(
            r"\[ECG-PR-RESULT iterations=(\d+) semantic_edges=(\d+) "
            r"score_checksum=([0-9a-f]+)\]", text)
        receipt = re.search(r"\[ECG-REF32-NATIVE ([^\]]+)\]", text)
        assert semantic and receipt, text[-8000:]
        fields = dict(re.findall(r"(\w+)=([^\s]+)", receipt.group(1)))
        edges = int(semantic.group(2))
        assert int(fields["recordLoads"]) == int(fields["governedLoads"]) == edges
        assert int(fields["recordBytes"]) == 4 * edges
        assert fields["accounting"] == "1"
        config = configparser.ConfigParser()
        config.read(output / "config.ini")
        capture_width = config.getint("system.cpu", "commitWidth")
        assert int(fields["captureWidth"]) == capture_width
        assert int(fields["captureOrderBits"]) == (
            16 * (capture_width - 1).bit_length())
        assert fields["outputWidth"] == "1"
        assert all(int(fields[key]) == 0 for key in (
            "pending", "errors", "fullDrops", "ingressDrops", "degradedDrops", "degraded"))
        assert "storage=inplace matrix_free=1 edge_sideband_bytes=0" in text
        assert not (output / "popt.bin").exists()
        assert not (output / "out.bin").exists()
        assert not (output / "in.bin").exists()
        if policy == "ECG":
            assert fields["mode"] == "apply"
            assert int(fields["generated"]) == edges
            assert int(fields["applied"]) > 0
            assert int(fields["minLatency"]) >= latency
            assert int(fields["maxOccupancy"]) <= 16
            assert minimum_burst <= int(fields["maxRetirementBurst"]) <= capture_width
        else:
            assert fields["mode"] == "validate"
            assert int(fields["generated"]) == 0
        roi_stats = (output / "stats.txt").read_text().split(
            "---------- End Simulation Statistics", 1)[0]
        roi_instructions = re.search(
            r"^system\.cpu\.commitStats0\.numInsts\s+(\d+)\s",
            roi_stats, re.MULTILINE)
        assert roi_instructions and int(roi_instructions.group(1)) > 0
        results.append((*semantic.groups(), roi_instructions.group(1)))
        receipts.append(fields)
    assert results[0] == results[1]
    return receipts


@pytest.mark.skipif(
    not (GEM5 / "build/RISCV/gem5.opt").exists() or
    not (ROOT / "bench/bin_gem5/pr_riscv_m5ops").exists(),
    reason="native RISCV simulator/PageRank guest not built")
def test_native_ref32_retirement_path_matches_isa_lru(tmp_path):
    run_native_ref32_pair(
        tmp_path, "-g 10 -k 4 -o 5 -n 1 -i 1 -t 0",
        minimum_burst=2)


@pytest.mark.skipif(
    not (GEM5 / "build/RISCV/gem5.opt").exists() or
    not (ROOT / "bench/bin_gem5/pr_riscv_m5ops").exists(),
    reason="native RISCV simulator/PageRank guest not built")
@pytest.mark.parametrize("sample,l3_size,iterations", [
    ("cit-Patents-native-n12", "8MB", 1),
    ("com-Orkut-native-n12", "64kB", 3),
])
def test_native_ref32_real_graph_pair(tmp_path, sample, l3_size, iterations):
    graph = ROOT / "results/graphs" / sample / f"{sample}-dbg.sg"
    if not graph.is_file():
        pytest.skip(f"optional native qualification sample missing: {graph}")
    run_native_ref32_pair(
        tmp_path, f"-f {graph} -o 0 -n 1 -i {iterations} -t 0",
        l3_size=l3_size, l1d_size="4kB", l2_size="16kB")


@pytest.mark.skipif(
    not (GEM5 / "build/RISCV/gem5.opt").exists() or
    not (ROOT / "bench/bin_gem5/pr_riscv_m5ops").exists() or
    not (ROOT / "bench/bin/converter").exists(),
    reason="native RISCV simulator/PageRank guest/converter not built")
def test_native_ref32_coalesces_across_traversals(tmp_path):
    source = tmp_path / "one-edge.el"
    source.write_text("0 1\n")
    base = tmp_path / "one-edge"
    converted = subprocess.run([
        str(ROOT / "bench/bin/converter"), "-f", str(source),
        "-m", "-o", "0", "-b", str(base),
    ], cwd=ROOT, env={**os.environ, "OMP_NUM_THREADS": "1"},
        capture_output=True, text=True, timeout=30, check=False)
    assert converted.returncode == 0, converted.stdout + converted.stderr
    receipts = run_native_ref32_pair(
        tmp_path, f"-f {base}.sg -o 0 -n 1 -i 4 -t 0",
        l3_size="8MB", l1d_size="4kB", l2_size="16kB", latency=4096)
    assert int(receipts[1]["generated"]) == 4
    assert int(receipts[1]["coalesced"]) == 2
