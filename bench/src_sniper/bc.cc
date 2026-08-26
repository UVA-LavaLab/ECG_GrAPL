// ============================================================================
// Betweenness Centrality (Brandes) for Sniper simulation
// ============================================================================
// Parallel Brandes BC mirroring the audited gem5 wrapper but multi-threaded:
// each source's forward BFS is expanded level-synchronously across the OpenMP
// threads (= Sniper cores) with an atomic depth-claim and atomic path-count
// accumulation, and the backward dependency accumulation is parallelised per
// level (deepest first, so each vertex's delta is finalised before its parents
// read it). ROI markers, four GRASP-protected property regions, the P-OPT reref
// matrix, and per-edge ECG epoch delivery match the other Sniper kernels.
// ============================================================================

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"

#include "graphbrew/partition/cagra/popt.h"
#include "sniper_sim/sniper_harness.h"
#include "ecg_reuse_plan_builder.h"

using namespace std;

typedef float ScoreT;

pvector<ScoreT> Brandes_Sniper(const Graph &g, int num_iters) {
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<ScoreT> scores(g.num_nodes(), ScoreT(0), kPropAlign);
    pvector<int32_t> depth(g.num_nodes(), int32_t(-1), kPropAlign);
    pvector<int64_t> path_counts(
        g.num_nodes(), int64_t(0), kPropAlign);
    pvector<ScoreT> deltas(g.num_nodes(), ScoreT(0), kPropAlign);

    sniper_report_region("scores", scores.data(), g.num_nodes(), sizeof(ScoreT));
    sniper_report_region("depth", depth.data(), g.num_nodes(), sizeof(int32_t));
    sniper_report_region("path_counts", path_counts.data(), g.num_nodes(), sizeof(int64_t));
    sniper_report_region("deltas", deltas.data(), g.num_nodes(), sizeof(ScoreT));

    // All four arrays are vertex-indexed properties; mark them all grasp_region so
    // GRASP protects the whole BC working set (mirror of the cache_sim/gem5 fix).
    SniperPropertyRegion regions[4] = {
        {"scores", reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
         static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
        {"depth", reinterpret_cast<uint64_t>(depth.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(int32_t),
         static_cast<uint32_t>(g.num_nodes()), sizeof(int32_t), true},
        {"path_counts", reinterpret_cast<uint64_t>(path_counts.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(int64_t),
         static_cast<uint32_t>(g.num_nodes()), sizeof(int64_t), true},
        {"deltas", reinterpret_cast<uint64_t>(deltas.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
         static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(g, edge_regions, 2);
    // P-OPT reref matrix keyed on depth (int32): BC pushes OUT-edges reading
    // depth[dest], so depth is next-referenced by its IN-neighbours -> transpose
    // reref (traverseCSR=false), matching the push_out_edges=true epochs below.
    constexpr int kNumVtxPerLine = 64 / sizeof(int32_t);
    constexpr int kNumEpochs = 256;
    const int ecg_reuse_plan_depth =
        graphbrew_sniper::env_int_clamped(
            "ECG_REUSE_PLAN_DEPTH", 0, 0, 4);
    static pvector<uint8_t> popt_matrix;
    int popt_num_cache_lines = (g.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    if (ecg_reuse_plan_depth != 2) {
        makeOffsetMatrix(
            g, popt_matrix, kNumVtxPerLine, kNumEpochs,
            /*traverseCSR=*/false);
        sniper_export_popt_matrix(
            popt_matrix.data(), popt_num_cache_lines,
            kNumEpochs, g.num_nodes());
    }

    bool ecg_extract_enabled = graphbrew_sniper::ecg_extract_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", kNumEpochs, 2, 65535));
    if (ecg_reuse_plan_depth == 2)
        ecg_epoch_count =
            ecg_reuse_plan::normalizeReusePlanEpochCount(ecg_epoch_count);
    vector<vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_enabled && ecg_reuse_plan_depth != 2) {
        ecg_reuse_plan::buildInEdgeEpochs(g, kNumVtxPerLine, ecg_epoch_count,
                                     /*linemin=*/true, out_edge_epochs,
                                     /*push_out_edges=*/true);
    }
    vector<uint64_t> pair_off;
    vector<uint64_t> pair_flat;
    bool pair_ok = false;
    if (ecg_extract_enabled && ecg_reuse_plan_depth == 2) {
        ecg_reuse_plan::buildInEdgeReusePlanRecords(
            g, kNumVtxPerLine, ecg_epoch_count,
            /*linemin=*/true, pair_off, pair_flat,
            /*push_out_edges=*/true);
        graphbrew_sniper::require_canonical_reuse_plan_offsets(
            g, pair_off, pair_flat.size(),
            /*push_out_edges=*/true, "bc");
        pair_ok = true;
    }
    sniper_export_context(
        regions, 4, g, nullptr, edge_regions, num_edge_regions);
    auto deliver = [&](NodeID u, size_t edge_pos, NodeID v) {
        if (!ecg_extract_enabled || static_cast<size_t>(u) >= out_edge_epochs.size())
            return;
        const auto& eps = out_edge_epochs[u];
        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                      : static_cast<uint16_t>(ecg_epoch_count - 1);
        SNIPER_ECG_EXTRACT(v, ep);
    };

    SNIPER_ROI_BEGIN();

    for (int iter = 0; iter < num_iters; iter++) {
        NodeID source = iter % g.num_nodes();

        #pragma omp parallel for
        for (NodeID n = 0; n < g.num_nodes(); n++) {
            depth[n] = -1;
            path_counts[n] = 0;
            deltas[n] = 0;
        }
        depth[source] = 0;
        path_counts[source] = 1;

        // Forward BFS: expand each level in parallel, recording the frontier of
        // every level so the backward pass can walk them in reverse.
        vector<vector<NodeID>> levels;
        levels.push_back(vector<NodeID>{source});
        int cur_level = 0;
        while (!levels[cur_level].empty()) {
            vector<NodeID> next_level;
            const vector<NodeID>& frontier = levels[cur_level];
            #pragma omp parallel
            {
                vector<NodeID> local_next;
                #pragma omp for schedule(dynamic, 64) nowait
                for (size_t i = 0; i < frontier.size(); i++) {
                    NodeID u = frontier[i];
                    SNIPER_SET_VERTEX(u);
                    auto process_neighbor = [&](NodeID v, int32_t depth_v) {
                        if (depth_v == -1 &&
                            compare_and_swap(
                                depth[v], int32_t(-1),
                                int32_t(cur_level + 1))) {
                            local_next.push_back(v);
                            depth_v = cur_level + 1;
                        } else {
                            depth_v = depth[v];
                        }
                        if (depth_v == cur_level + 1)
                            fetch_and_add(path_counts[v], path_counts[u]);
                    };
                    if (pair_ok) {
                        const uint64_t begin =
                            static_cast<uint64_t>(g.out_offset(u));
                        const uint64_t end =
                            static_cast<uint64_t>(g.out_offset(u + 1));
                        for (uint64_t pos = begin; pos < end; ++pos) {
                            const uint64_t record = pair_flat[pos];
                            const NodeID v = static_cast<NodeID>(
                                ecg_reuse_plan::extractReusePlanDest(record));
                            SNIPER_ECG_EXTRACT2(
                                static_cast<uint32_t>(v),
                                ecg_reuse_plan::extractReusePlanTier(record),
                                ecg_reuse_plan::extractReusePlanFirst(record),
                                ecg_reuse_plan::extractReusePlanSecond(record));
                            const int32_t depth_v = depth[v];
                            SNIPER_ECG_CLEAR_EXTRACT2(v);
                            process_neighbor(v, depth_v);
                        }
                    } else {
                        size_t edge_pos = 0;
                        for (NodeID v : g.out_neigh(u)) {
                            deliver(u, edge_pos, v);
                            ++edge_pos;
                            process_neighbor(v, depth[v]);
                        }
                    }
                }
                #pragma omp critical
                next_level.insert(next_level.end(),
                                  local_next.begin(), local_next.end());
            }
            if (next_level.empty()) break;
            levels.push_back(std::move(next_level));
            cur_level++;
        }

        // Backward accumulation, deepest level first. Each vertex appears in one
        // level, so its delta/score writes are race-free, and the delta[v] it
        // reads (level d+1) were finalised in the previous outer iteration.
        for (int d = static_cast<int>(levels.size()) - 1; d > 0; d--) {
            const vector<NodeID>& level = levels[d];
            #pragma omp parallel for schedule(dynamic, 64)
            for (size_t i = 0; i < level.size(); i++) {
                NodeID w = level[i];
                ScoreT delta_w = 0;
                for (NodeID v : g.out_neigh(w)) {
                    if (depth[v] == depth[w] + 1)
                        delta_w += static_cast<ScoreT>(path_counts[w]) /
                                   path_counts[v] * (1.0f + deltas[v]);
                }
                deltas[w] = delta_w;
                if (w != source)
                    scores[w] += delta_w;
            }
        }
    }

    SNIPER_ROI_END();
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> sp(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) sp[n] = {n, scores[n]};
    int k = min(5, (int)g.num_nodes());
    partial_sort(sp.begin(), sp.begin() + k, sp.end(),
                 [](const auto &a, const auto &b) { return a.second > b.second; });
    for (int i = 0; i < k; i++)
        cout << sp[i].first << ": " << sp[i].second << endl;
}

bool BCVerifier(const Graph &g, const pvector<ScoreT> &scores, int num_iters) {
    (void)g; (void)num_iters;
    for (NodeID n = 0; n < scores.size(); n++)
        if (scores[n] < 0) return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLIterApp cli(argc, argv, "bc-sniper", 1);
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto BCBound = [&cli](const Graph &g) {
        return Brandes_Sniper(g, cli.num_iters());
    };
    auto PrintBound = [](const Graph &g, const pvector<ScoreT> &s) {
        PrintTopScores(g, s);
    };
    auto VerifyBound = [&cli](const Graph &g, const pvector<ScoreT> &s) {
        return BCVerifier(g, s, cli.num_iters());
    };
    BenchmarkKernel(cli, g, BCBound, PrintBound, VerifyBound);
    return 0;
}
