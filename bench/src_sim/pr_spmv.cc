// Copyright (c) 2024, UVA LavaLab
// PageRank-SpMV (Jacobi) with Cache Simulation
//
// Unlike the Gauss-Seidel PR (src_sim/pr.cc), this Jacobi variant computes
// outgoing_contrib from the PREVIOUS iteration's scores (double-buffered).
// Updated scores are NOT visible to other vertices within the same iteration.
//
// Cache difference vs Gauss-Seidel:
//   - GS reads contrib[v] which may contain THIS iteration's updated value
//     → benefits from forward-edge-fraction reordering (GoGraph)
//   - Jacobi reads contrib[v] computed entirely from PREVIOUS scores
//     → insensitive to forward-edge fraction; pure SpMV access pattern

#include <algorithm>
#include <iostream>
#include <vector>
#include <fstream>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

#include "graphbrew/partition/cagra/popt.h"

using namespace std;
using namespace cache_sim;

typedef float ScoreT;
const float kDamp = 0.85;

// PageRank Jacobi/SpMV with cache simulation
// Key difference from GS: outgoing_contrib is computed FIRST from old scores,
// then all vertices read from the stale contrib array (no in-place updates).
template<typename CacheType>
pvector<ScoreT> PageRankSpMV_Sim(const Graph &g, CacheType &cache,
                                 int max_iters, double epsilon = 0,
                                 bool logging_enabled = false) {
    const ScoreT init_score = 1.0f / g.num_nodes();
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> scores(
        g.num_nodes(), init_score, GRAPH_SIM_PROPERTY_ALIGNMENT);
    pvector<ScoreT> outgoing_contrib(
        g.num_nodes(), ScoreT(0), GRAPH_SIM_PROPERTY_ALIGNMENT);

    ScoreT* scores_ptr = scores.data();
    ScoreT* contrib_ptr = outgoing_contrib.data();

    // --- Graph-aware cache context (for GRASP/P-OPT/ECG policies) ---
    GraphCacheContext graph_ctx;
    pvector<uint32_t> degrees(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        degrees[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(degrees.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());
    size_t llc_size = 8 * 1024 * 1024;
    llc_size = GetEnvSizeBytes("CACHE_L3_SIZE", llc_size);
    graph_ctx.registerPropertyArray(scores_ptr, g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    graph_ctx.registerPropertyArray(contrib_ptr, g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    cache.initGraphContext(&graph_ctx);

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    cache.initGraphContext(&graph_ctx);
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());

    // Build P-OPT rereference matrix (for POPT and ECG policies)
    static pvector<uint8_t> popt_matrix;
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        if (policy == EvictionPolicy::POPT ||
            policy == EvictionPolicy::ECG) {
            constexpr int numVtxPerLine = 64 / sizeof(ScoreT);
            constexpr int numEpochs = 256;
            makeOffsetMatrix(g, popt_matrix, numVtxPerLine, numEpochs);
            int numCacheLines = (g.num_nodes() + numVtxPerLine - 1) / numVtxPerLine;
            graph_ctx.initRereference(popt_matrix.data(), numCacheLines,
                                      numEpochs, g.num_nodes(), 64);
        }
    }
    graph_ctx.printSummary();

    for (int iter = 0; iter < max_iters; iter++) {
        double error = 0;

        // Phase 1: Compute contributions from PREVIOUS iteration's scores
        // This is the Jacobi/SpMV difference — all contribs are stale
        #pragma omp parallel for
        for (NodeID n = 0; n < g.num_nodes(); n++) {
            SIM_CACHE_READ(cache, scores_ptr, n);
            SIM_CACHE_WRITE(cache, contrib_ptr, n);
            outgoing_contrib[n] = scores[n] / g.out_degree(n);
        }

        // Phase 2: SpMV — accumulate contributions (read-only from stale contrib)
        #pragma omp parallel for reduction(+ : error) schedule(dynamic, 64)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            SIM_SET_VERTEX(cache, u);

            ScoreT incoming_total = 0;
            auto in_neigh = g.in_neigh(u);
            for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it) {
                SIM_CACHE_READ_EDGE(cache, it);
                NodeID v = *it;
                SIM_CACHE_READ_MASKED(cache, contrib_ptr, v, graph_ctx, vertex_masks[v]);
                incoming_total += outgoing_contrib[v];
            }

            SIM_CACHE_READ(cache, scores_ptr, u);
            ScoreT old_score = scores[u];
            ScoreT new_score = base_score + kDamp * incoming_total;
            SIM_CACHE_WRITE(cache, scores_ptr, u);
            scores[u] = new_score;
            error += fabs(new_score - old_score);
        }

        if (logging_enabled)
            cout << "Iteration " << iter << ": error = " << error << endl;

        if (error < epsilon)
            break;
    }

    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        score_pairs[n] = make_pair(n, scores[n]);
    }
    int k = 5;
    partial_sort(score_pairs.begin(), score_pairs.begin() + k, score_pairs.end(),
                [](auto a, auto b) { return a.second > b.second; });
    for (int i = 0; i < k; i++) {
        cout << score_pairs[i].first << ": " << score_pairs[i].second << endl;
    }
}

bool PRVerifier(const Graph &g, const pvector<ScoreT> &scores, double target_error) {
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> incomming_sums(g.num_nodes(), 0);
    double error = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        ScoreT outgoing_contrib = scores[u] / g.out_degree(u);
        for (NodeID v : g.out_neigh(u))
            incomming_sums[v] += outgoing_contrib;
    }
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        error += fabs(base_score + kDamp * incomming_sums[n] - scores[n]);
        incomming_sums[n] = 0;
    }
    cout << "Total Error: " << error << endl;
    return error < target_error;
}

int main(int argc, char *argv[]) {
    CLPageRank cli(argc, argv, "pagerank-spmv-sim", 1e-4, 20);
    if (!cli.ParseArgs())
        return -1;

    Builder b(cli);
    Graph g = b.MakeGraph();

    bool multicore = IsMultiCoreMode();
    bool sampled = IsSampledMode();
    bool ultrafast = IsUltraFastMode();
    bool fast = IsFastMode();

    auto runSim = [&](auto &cache) {
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankSpMV_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };

        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        cout << endl;
        cache.printStats();

        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    };

    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        runSim(cache);
    } else if (sampled) {
        SampledCacheHierarchy cache = SampledCacheHierarchy::fromEnvironment();
        runSim(cache);
    } else if (ultrafast) {
        UltraFastCacheHierarchy cache = UltraFastCacheHierarchy::fromEnvironment();
        runSim(cache);
    } else if (fast) {
        FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
        runSim(cache);
    } else {
        CacheHierarchy cache = CacheHierarchy::fromEnvironment();
        runSim(cache);
    }

    return 0;
}
