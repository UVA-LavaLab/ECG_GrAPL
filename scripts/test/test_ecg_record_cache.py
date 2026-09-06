from pathlib import Path
import json
import os
import re
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_current_record_cache_updates_and_bypass(tmp_path):
    binary = tmp_path / "record_cache"
    compiled = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-fopenmp", "-I", str(ROOT / "bench/include"),
         str(ROOT / "bench/src_sim/test_ecg_record_cache.cc"), "-o", str(binary)],
        capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "[SUMMARY] failures=0" in ran.stdout


def test_current_pagerank_mechanisms_preserve_semantic_result(tmp_path):
    binary = ROOT / "bench/bin_sim/pr"
    if not binary.is_file():
        pytest.skip("functional PageRank binary is not built")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))}
    env.update({
        "OMP_NUM_THREADS": "1", "CACHE_POLICY": "LRU",
        "CACHE_ULTRAFAST": "0", "CACHE_FAST": "0",
        "CACHE_L1_POLICY": "LRU", "CACHE_L2_POLICY": "LRU",
        "CACHE_L3_POLICY": "LRU", "CACHE_L1_SIZE": "1KB",
        "CACHE_L2_SIZE": "2KB", "CACHE_L3_SIZE": "4KB",
        "CACHE_L3_WAYS": "16",
    })
    results = []
    cases = [(mode, width) for width in (4, 8)
             for mode in ("transport", "replacement", "prefetch", "replacement-prefetch")]
    for mechanism, width in cases:
        case_env = dict(env)
        if mechanism is not None:
            case_env.update({
                "ECG_RECORD_MECHANISM": mechanism,
                "ECG_RECORD_BYTES": str(width),
                "CACHE_L3_POLICY": "ECG" if mechanism in (
                    "replacement", "replacement-prefetch") else "LRU",
            })
        ran = subprocess.run(
            [str(binary), "-g", "8", "-k", "4", "-o", "5", "-n", "1", "-i", "3", "-t", "0"],
            cwd=tmp_path, env=case_env, capture_output=True, text=True, timeout=30)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-8000:]
        found = re.search(r"\[ECG-PR-RESULT ([^\]]+)\]", text)
        assert found, text[-8000:]
        results.append(found.group(1))
        if mechanism is not None:
            assert f"record_bytes={width}" in text
            assert f"mechanism={mechanism}" in text
            assert "pending=0 accounting=1 timing_scope=access-step" in text
            assert "storage=separate source_immutable=1 matrix_bytes=0" in text
    assert len(set(results)) == 1


def test_fixed_csr_and_record_paths_share_work_and_accounted_indices(tmp_path):
    binary = ROOT / "bench/bin_sim/pr"
    if not binary.is_file():
        pytest.skip("functional PageRank binary is not built")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))}
    env.update({
        "OMP_NUM_THREADS": "1", "ECG_CURRENT_PR_BASELINE": "1",
        "CACHE_ULTRAFAST": "0", "CACHE_FAST": "0", "CACHE_L3_SIZE": "64KB",
        "CACHE_L1_SIZE": "4KB", "CACHE_L2_SIZE": "8KB", "CACHE_L3_WAYS": "16",
        "CACHE_L1_POLICY": "LRU", "CACHE_L2_POLICY": "LRU",
    })
    results = []
    for policy, mechanism in (
            ("LRU", None), ("SRRIP", None), ("LRU", "transport"),
            ("ECG", "replacement"), ("LRU", "prefetch"), ("ECG", "replacement-prefetch")):
        case = {**env, "CACHE_POLICY": policy, "CACHE_L3_POLICY": policy}
        output = tmp_path / f"{policy}-{mechanism}.json"
        case["CACHE_OUTPUT_JSON"] = str(output)
        if mechanism:
            case["ECG_RECORD_MECHANISM"] = mechanism
        ran = subprocess.run(
            [str(binary), "-g", "8", "-k", "4", "-o", "5", "-n", "1", "-i", "3", "-t", "0"],
            env=case, cwd=tmp_path, capture_output=True, text=True, timeout=30)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-8000:]
        result = re.search(r"\[ECG-PR-RESULT ([^\]]+)\]", text)
        assert result, text[-8000:]
        results.append(result.group(1))
        contract = re.search(r"\[ECG-PR-WORKLOAD ([^\]]+)\]", text)
        assert contract, "fixed-workload receipt is missing"
        fields = dict(re.findall(r"(\w+)=([^\s]+)", contract.group(1)))
        assert fields["traversal"] == "pull-gs" and fields["arithmetic"] == "separate-f32"
        assert fields["carrier"] == ("record" if mechanism else "csr")
        assert int(fields["csr_index_reads"]) == 4 * 256 * 3
        if mechanism in (None, "transport"):
            work = dict(re.findall(r"(\w+)=([^\s]+)", result.group(1)))
            assert json.loads(output.read_text())["total_accesses"] == (
                2 * int(work["semantic_edges"]) + 6 * 256 * 3)
    assert len(set(results)) == 1


def test_current_pagerank_matches_legacy_with_identical_fp_contract(tmp_path):
    source = tmp_path / "pr_compare.cc"
    source.write_text(r'''
#define main original_pagerank_main
#include "bench/src_sim/pr.cc"
#undef main
int main() {
    std::vector<std::string> args = {
        "compare", "-g", "8", "-k", "4", "-o", "5", "-n", "1", "-i", "3", "-t", "0"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    CLPageRank cli(argv.size(), argv.data(), "compare", 0, 3);
    if (!cli.ParseArgs()) return 1;
    Builder builder(cli);
    Graph graph = builder.MakeGraph();
    unsetenv("ECG_RECORD_MECHANISM");
    cache_sim::CacheHierarchy baseline(1024, 8, 2048, 4, 4096, 16, 64,
        cache_sim::EvictionPolicy::LRU);
    auto expected = PageRankPullGS_Sim(graph, baseline, 3, 0);
    setenv("ECG_RECORD_MECHANISM", "transport", 1);
    for (const char* width : {"4", "8"}) {
        setenv("ECG_RECORD_BYTES", width, 1);
        cache_sim::CacheHierarchy current(1024, 8, 2048, 4, 4096, 16, 64,
            cache_sim::EvictionPolicy::LRU);
        auto actual = PageRankPullGSFixed_Sim(graph, current, 3, 0);
        if (std::memcmp(expected.data(), actual.data(), graph.num_nodes() * sizeof(float)))
            return 2;
    }
    return 0;
}
''')
    binary = tmp_path / "pr_compare"
    compiled = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-ffp-contract=off", "-fopenmp",
         "-I", str(ROOT), "-I", str(ROOT / "bench/include"),
         "-I", str(ROOT / "bench/include/external/gapbs"),
         "-I", str(ROOT / "bench/include/graphbrew"),
         "-I", str(ROOT / "bench/include/external"),
         str(source), "-o", str(binary)],
        capture_output=True, text=True, timeout=120)
    assert compiled.returncode == 0, compiled.stderr[-6000:]
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_"))}
    env.update({"OMP_NUM_THREADS": "1", "CACHE_ULTRAFAST": "0", "CACHE_L3_POLICY": "LRU"})
    ran = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
