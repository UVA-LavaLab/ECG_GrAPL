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


_PAGERANK_FIXTURE = ["-g", "8", "-k", "4", "-o", "5", "-n", "1", "-i", "3", "-t", "0"]
# GRASP_PAPER's tiering, as the runner gives the GRASP_PAPER PageRank cell.
_GRASP_PAPER_TIERS = {"GRASP_HOT_FRACTION": "0.50", "GRASP_BOUNDARY_MODE": "capacity"}


def _pagerank_record_environment():
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_", "SNIPER_", "GRASP_"))}
    env.update({
        "OMP_NUM_THREADS": "1", "CACHE_POLICY": "LRU",
        "CACHE_ULTRAFAST": "0", "CACHE_FAST": "0",
        "CACHE_L1_POLICY": "LRU", "CACHE_L2_POLICY": "LRU",
        "CACHE_L3_POLICY": "ECG", "CACHE_L1_SIZE": "1KB",
        "CACHE_L2_SIZE": "2KB", "CACHE_L3_SIZE": "4KB", "CACHE_L3_WAYS": "16",
        "ECG_RECORD_MECHANISM": "replacement", "ECG_RECORD_BYTES": "4",
    })
    return env


def test_pagerank_rrpv_order_never_reaches_the_lru_scan(tmp_path):
    """PageRank's record rule takes its base victim from an LRU scan today.

    pr.cc configures the record without a base policy, so every decision the
    bound does not settle falls back to recency. The RRPV order must remove
    that path rather than outvote it: every decision is taken under the order,
    none reaches the LRU scan, and the PageRank result is unchanged because
    only the evicted line differs. The rule arm is measured on the same
    fixture so that a zero under the order is not a fixture that never evicts.
    """
    binary = ROOT / "bench/bin_sim/pr"
    if not binary.is_file():
        pytest.skip("functional PageRank binary is not built")
    cases = {
        "rule": {},
        "rrpv": {"ECG_RECORD_RRPV_ORDER": "1", **_GRASP_PAPER_TIERS},
        "rrpv-governed-first": {
            "ECG_RECORD_RRPV_ORDER": "1", "ECG_RECORD_GOVERNED_FIRST": "1", **_GRASP_PAPER_TIERS},
    }
    results, victims = {}, {}
    for name, extra in cases.items():
        output = tmp_path / f"{name}.json"
        ran = subprocess.run(
            [str(binary), *_PAGERANK_FIXTURE], cwd=tmp_path,
            env={**_pagerank_record_environment(), **extra, "CACHE_OUTPUT_JSON": str(output)},
            capture_output=True, text=True, timeout=30)
        text = ran.stdout + ran.stderr
        assert ran.returncode == 0, text[-8000:]
        found = re.search(r"\[ECG-PR-RESULT ([^\]]+)\]", text)
        assert found, text[-8000:]
        results[name] = found.group(1)
        payload = json.loads(output.read_text())
        victims[name] = {key: payload[f"ecg_record_victim_{key}"] for key in (
            "decisions", "rrpv_ordered", "rrpv_changed", "base_lru", "ungoverned_first")}
        receipt = re.search(r"\[ECG-PR-RRPV-ORDER ([^\]]+)\]", text)
        if name == "rule":
            assert receipt is None, "the rule arm must not report the RRPV order"
            continue
        assert receipt, text[-8000:]
        assert dict(re.findall(r"(\w+)=([^\s]+)", receipt.group(1))) == {
            "hot_percent": "50", "boundary": "capacity"}
    rule = victims["rule"]
    assert rule["decisions"] > 0 and rule["base_lru"] > 0 and rule["rrpv_ordered"] == 0, (
        f"the rule arm must reach the LRU scan here, or the arm's zero proves nothing: {rule}")
    for name in ("rrpv", "rrpv-governed-first"):
        arm = victims[name]
        assert arm["decisions"] > 0 and arm["rrpv_ordered"] == arm["decisions"], (name, arm)
        assert arm["base_lru"] == 0, (name, arm)
    ordered = victims["rrpv-governed-first"]
    assert ordered["ungoverned_first"] > 0 and ordered["rrpv_changed"] > 0, (
        f"the fixture must reach a choice the two orders make differently: {ordered}")
    assert len(set(results.values())) == 1, results


@pytest.mark.parametrize("change,message", [
    ({"ECG_RECORD_MECHANISM": None, "ECG_CURRENT_PR_BASELINE": "1", "CACHE_L3_POLICY": "LRU"},
     "ECG_RECORD_RRPV_ORDER requires a current ECG record mode"),
    ({"ECG_RECORD_MECHANISM": "replacement-prefetch"},
     "ECG_RECORD_RRPV_ORDER requires the replacement mechanism without prefetch"),
    ({"ECG_RECORD_MECHANISM": "transport", "CACHE_L3_POLICY": "LRU"},
     "ECG_RECORD_RRPV_ORDER requires the replacement mechanism without prefetch"),
    ({"GRASP_HOT_FRACTION": None},
     "ECG_RECORD_RRPV_ORDER requires GRASP_HOT_FRACTION and GRASP_BOUNDARY_MODE"),
    ({"GRASP_BOUNDARY_MODE": None},
     "ECG_RECORD_RRPV_ORDER requires GRASP_HOT_FRACTION and GRASP_BOUNDARY_MODE"),
    ({"GRASP_HOT_FRACTION": "1.5"}, "GRASP_HOT_FRACTION must lie in (0, 1]"),
    ({"GRASP_HOT_FRACTION": "0.5x"}, "GRASP_HOT_FRACTION must lie in (0, 1]"),
    ({"GRASP_BOUNDARY_MODE": "vertex"},
     "ECG_RECORD_RRPV_ORDER requires GRASP_BOUNDARY_MODE=capacity"),
    ({"ECG_RECORD_RRPV_ORDER": "2"}, "ECG_RECORD_RRPV_ORDER is outside its supported range"),
])
def test_pagerank_rrpv_order_refuses_what_it_cannot_honour(tmp_path, change, message):
    """Each refusal names its reason, so a misconfigured arm never runs as another.

    Prefetch admission still asks a recency-ordered question, so the order is
    replacement-only. Without GRASP_PAPER's tiering declared, the registration
    would fall back to a default fraction silently and the arm would no longer
    rank by the state its GRASP_PAPER baseline uses.
    """
    binary = ROOT / "bench/bin_sim/pr"
    if not binary.is_file():
        pytest.skip("functional PageRank binary is not built")
    env = {**_pagerank_record_environment(), "ECG_RECORD_RRPV_ORDER": "1", **_GRASP_PAPER_TIERS}
    for key, value in change.items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    ran = subprocess.run(
        [str(binary), *_PAGERANK_FIXTURE], cwd=tmp_path, env=env,
        capture_output=True, text=True, timeout=30)
    text = ran.stdout + ran.stderr
    assert ran.returncode != 0 and message in text, text[-4000:]
