// ============================================================================
// PageRank (SpMV / Jacobi) for gem5 SE-mode simulation
// ============================================================================

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "gem5_sim/gem5_harness.h"

using namespace std;

typedef float ScoreT;
const float kDamp = 0.85;

pvector<ScoreT> PageRankPull_Gem5(const Graph &g, int max_iters,
                                  double epsilon = 0) {
    const ScoreT init_score = 1.0f / g.num_nodes();
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> scores(g.num_nodes(), init_score);
    pvector<ScoreT> outgoing_contrib(g.num_nodes());

    gem5_report_region("scores", scores.data(), g.num_nodes(), sizeof(ScoreT));
    gem5_report_region("contrib", outgoing_contrib.data(), g.num_nodes(), sizeof(ScoreT));

    Gem5PropertyRegion regions[2] = {
        {"scores", reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
        {"contrib", reinterpret_cast<uint64_t>(outgoing_contrib.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);
    gem5_export_context(regions, 2, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions);

    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);

    for (int iter = 0; iter < max_iters; iter++) {
        double error = 0;
        // Phase 1: compute contributions from previous scores
        for (NodeID n = 0; n < g.num_nodes(); n++)
            outgoing_contrib[n] = scores[n] / g.out_degree(n);
        // Phase 2: SpMV accumulation
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            GEM5_SET_VERTEX(u);
            ScoreT incoming_total = 0;
            for (NodeID v : g.in_neigh(u))
                incoming_total += outgoing_contrib[v];
            ScoreT old_score = scores[u];
            scores[u] = base_score + kDamp * incoming_total;
            error += fabs(scores[u] - old_score);
        }
        if (error < epsilon) break;
    }

    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++)
        score_pairs[n] = make_pair(n, scores[n]);
    int k = min(5, (int)g.num_nodes());
    partial_sort(score_pairs.begin(), score_pairs.begin() + k, score_pairs.end(),
                [](auto a, auto b) { return a.second > b.second; });
    for (int i = 0; i < k; i++)
        cout << score_pairs[i].first << ": " << score_pairs[i].second << endl;
}

bool PRVerifier(const Graph &g, const pvector<ScoreT> &scores, double target_error) {
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> incoming_sums(g.num_nodes(), 0);
    double error = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        ScoreT c = scores[u] / g.out_degree(u);
        for (NodeID v : g.out_neigh(u))
            incoming_sums[v] += c;
    }
    for (NodeID n = 0; n < g.num_nodes(); n++)
        error += fabs(base_score + kDamp * incoming_sums[n] - scores[n]);
    cout << "Total Error: " << error << endl;
    return error < target_error;
}

int main(int argc, char *argv[]) {
    CLPageRank cli(argc, argv, "pagerank-spmv-gem5", 1e-4, 20);
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto PRBound = [&cli](const Graph &g) {
        return PageRankPull_Gem5(g, cli.max_iters(), cli.tolerance());
    };
    auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
        return PRVerifier(g, scores, cli.tolerance());
    };
    BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
    return 0;
}
