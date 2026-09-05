"""Native-carrier PageRank matches the existing kernel and restores its CSR."""

import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_ref32_pagerank_semantics_and_csr_restoration(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ is unavailable")
    source = tmp_path / "ref32_guest.cc"
    source.write_text(r'''
#define main original_pagerank_main
#include "bench/src_gem5/pr.cc"
#undef main

using FixtureEdge = std::pair<NodeID, NodeID>;
static std::pair<NodeID**, NodeID*> csr(std::vector<FixtureEdge> edges) {
    std::sort(edges.begin(), edges.end());
    auto* data = new NodeID[edges.size()];
    auto** index = new NodeID*[33];
    size_t position = 0;
    for (int vertex = 0; vertex < 32; ++vertex) {
        index[vertex] = data + position;
        while (position < edges.size() && edges[position].first == vertex) {
            data[position] = edges[position].second;
            ++position;
        }
    }
    index[32] = data + position;
    return {index, data};
}
static Graph graph(bool directed) {
    std::vector<FixtureEdge> edges = {{0,1},{1,18},{18,0},{18,31},
                              {31,2},{2,18},{2,31},{31,0}};
    if (!directed) {
        const auto original = edges;
        for (auto edge : original) edges.emplace_back(edge.second, edge.first);
    }
    auto out = csr(edges);
    if (!directed) return Graph(32, out.first, out.second);
    for (auto& edge : edges) std::swap(edge.first, edge.second);
    auto in = csr(edges);
    return Graph(32, out.first, out.second, in.first, in.second);
}
static std::vector<NodeID> neighbors(const Graph& g) {
    std::vector<NodeID> values;
    for (NodeID v = 0; v < g.num_nodes(); ++v) {
        for (NodeID n : g.out_neigh(v)) values.push_back(n);
        for (NodeID n : g.in_neigh(v)) values.push_back(n);
    }
    return values;
}
int main() {
    setenv("ECG_REF32_RECORD", "0", 1);
    for (const bool directed : {false, true}) {
        for (const int iterations : {1, 3}) {
            Graph g = graph(directed);
            const auto original = neighbors(g);
            auto expected = PageRankPullGS_Gem5(g, iterations, 0);
            auto actual = PageRankPullGSRef32_Gem5(g, iterations, 0);
            if (std::memcmp(expected.data(), actual.data(), 32 * sizeof(float)) ||
                neighbors(g) != original) return 1;
            auto repeated = PageRankPullGSRef32_Gem5(g, iterations, 0);
            if (std::memcmp(expected.data(), repeated.data(), 32 * sizeof(float)) ||
                neighbors(g) != original) return 2;
        }
    }
    std::puts("native-carrier PR semantics and restoration PASS");
}
''')
    binary = tmp_path / "ref32_guest"
    built = subprocess.run([
        compiler, "-std=c++17", "-O2", "-fopenmp", "-DNO_M5OPS",
        f"-I{ROOT}", f"-I{ROOT / 'bench/include'}",
        f"-I{ROOT / 'bench/include/external/gapbs'}",
        f"-I{ROOT / 'bench/include/graphbrew'}",
        f"-I{ROOT / 'bench/include/external'}",
        str(source), "-o", str(binary),
    ], cwd=ROOT, capture_output=True, text=True, timeout=180)
    assert built.returncode == 0, built.stderr[-5000:]
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("GEM5_", "ECG_", "CACHE_", "POPT_"))
    }
    env.update({
        "OMP_NUM_THREADS": "1",
        "GEM5_GRAPHBREW_CTX": str(tmp_path / "context.json"),
        "GEM5_GRAPHBREW_OUT_EDGES": str(tmp_path / "out.bin"),
        "GEM5_GRAPHBREW_IN_EDGES": str(tmp_path / "in.bin"),
        "GEM5_POPT_MATRIX": str(tmp_path / "popt.bin"),
    })
    ran = subprocess.run(
        [str(binary)], cwd=tmp_path, env=env,
        capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "native-carrier PR semantics and restoration PASS" in ran.stdout
    assert "edge_sideband_bytes=0" in ran.stderr
    context = json.loads((tmp_path / "context.json").read_text())
    assert [region["name"] for region in context["property_regions"]] == [
        "scores", "contrib"]
    assert all("data_path" not in region for region in context["edge_regions"])
    assert context["flowthrough_size"] == context["structural_flowthrough_size"] == 0
