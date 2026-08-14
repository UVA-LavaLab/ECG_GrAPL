// ============================================================================
// PageRank (Pull, Gauss-Seidel) for Sniper simulation
// ============================================================================
// Sniper-oriented PageRank wrapper. It mirrors the audited gem5 PR wrapper but
// uses Sniper ROI markers and Sniper sideband paths. Sniper policy support is
// not wired yet; this wrapper establishes the benchmark/harness surface.
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

#include "graphbrew/partition/cagra/popt.h"
#include "sniper_sim/sniper_harness.h"

// ECG mode 6 (per-edge mask) builder — shared with cache_sim and gem5.
#include "ecg_mode6_builder.h"
// Shared per-edge next-ref epoch builder (SNIPER_ECG_EXTRACT delivery).
#include "ecg_reuse_plan_builder.h"

using namespace std;

typedef float ScoreT;
const float kDamp = 0.85;

pvector<ScoreT> PageRankPullGS_Sniper(const Graph &g, int max_iters,
                                       double epsilon = 0) {
    const ScoreT init_score = 1.0f / g.num_nodes();
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<ScoreT> scores(g.num_nodes(), init_score, kPropAlign);
    pvector<ScoreT> outgoing_contrib(
        g.num_nodes(), ScoreT(0), kPropAlign);

    sniper_report_region("scores", scores.data(), g.num_nodes(), sizeof(ScoreT));
    sniper_report_region("contrib", outgoing_contrib.data(), g.num_nodes(), sizeof(ScoreT));

    SniperPropertyRegion regions[2] = {
        {"scores", reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
        {"contrib", reinterpret_cast<uint64_t>(outgoing_contrib.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(g, edge_regions, 2, true);
    static pvector<uint8_t> popt_matrix;
    constexpr int kNumVtxPerLine = 64 / sizeof(ScoreT);
    constexpr int kNumEpochs = 256;
    int popt_num_cache_lines = (g.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    {
        makeOffsetMatrix(g, popt_matrix, kNumVtxPerLine, kNumEpochs);
        sniper_export_popt_matrix(popt_matrix.data(), popt_num_cache_lines,
                                  kNumEpochs, g.num_nodes());
    }
    sniper_export_context(regions, 2, g, nullptr, edge_regions, num_edge_regions);

    uint8_t id_bits = 1;
    while ((1ULL << id_bits) < static_cast<uint64_t>(g.num_nodes())) id_bits++;
    uint8_t container_bits = (g.num_nodes() > (1LL << 30)) ? 64 : 32;
    uint8_t spare = container_bits - id_bits;
    if (spare < 2 && container_bits == 32) {
        container_bits = 64;
        spare = container_bits - id_bits;
    }

    uint8_t pfx_bits = 0;
    if (spare >= 16) pfx_bits = min(static_cast<int>(spare - 10), 6);
    else if (spare >= 10) pfx_bits = min(static_cast<int>(spare - 6), 4);
    else if (spare >= 6) pfx_bits = spare - 4;

    int hot_table_size = pfx_bits > 0 ? (1 << pfx_bits) : 0;
    hot_table_size = min(hot_table_size, static_cast<int>(g.num_nodes()));
    constexpr int PREFETCH_WINDOW = 16;

    vector<NodeID> hot_table(hot_table_size);
    if (hot_table_size > 0) {
        vector<pair<int64_t, NodeID>> deg_vtx(g.num_nodes());
        for (NodeID n = 0; n < g.num_nodes(); n++) {
            deg_vtx[n] = {g.out_degree(n), n};
        }
        partial_sort(deg_vtx.begin(), deg_vtx.begin() + hot_table_size, deg_vtx.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int i = 0; i < hot_table_size; i++) {
            hot_table[i] = deg_vtx[i].second;
        }
        printf("Sniper ECG adaptive prefetch: %d-bit container, %d-bit ID, %d spare, "
               "%d prefetch bits -> %d-entry hot table\n",
               container_bits, id_bits, spare, pfx_bits, hot_table_size);
    }

    vector<int> hub_rank(g.num_nodes(), -1);
    for (int i = 0; i < hot_table_size; i++) {
        hub_rank[hot_table[i]] = i;
    }

    const char* ecg_prefetch_env = getenv("SNIPER_ENABLE_ECG_PFX_HINTS");
    if (!ecg_prefetch_env) ecg_prefetch_env = getenv("ECG_PREFETCH");
    bool ecg_prefetch_enabled = ecg_prefetch_env && string(ecg_prefetch_env) != "0";
    int pfx_lookahead = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_PFX_LOOKAHEAD",
        graphbrew_sniper::env_int_clamped("ECG_PREFETCH_LOOKAHEAD", 4, 0, 64),
        0, 64);

    // === Mode 6: per-edge ECG mask ===
    // Also accepts ECG_PREFETCH_MODE for compatibility with roi_matrix.py's
    // cache_sim env naming.
    int ecg_pfx_mode = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_PFX_MODE", -1, -1, 7);
    if (ecg_pfx_mode < 0) {
        ecg_pfx_mode = graphbrew_sniper::env_int_clamped("ECG_PREFETCH_MODE", 0, 0, 7);
    }
    int edge_mask_lookahead = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_EDGE_MASK_LOOKAHEAD",
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_LOOKAHEAD", 8, 1, 64),
        1, 64);
    vector<vector<uint64_t>> in_edge_masks_by_src;
    if (ecg_prefetch_enabled && ecg_pfx_mode == 6) {
        vector<uint8_t> avg_reref_by_line;
        ecg_mode6::computeAvgRerefByLine(popt_matrix.data(), popt_num_cache_lines,
                                         kNumEpochs, avg_reref_by_line);
        vector<uint8_t> tiers;
        ecg_mode6::computeDegreeTiers(g, tiers);
        ecg_mode6::buildInEdgeMasks(g, tiers, avg_reref_by_line,
                                    edge_mask_lookahead, kNumVtxPerLine,
                                    in_edge_masks_by_src, "sniper-PR");
        printf("[sniper ECG mode 6] lookahead=%d (per-edge mask path active)\n",
               edge_mask_lookahead);
    }

    // SNIPER_ECG_EXTRACT: build per-edge next-ref epochs (same as cache_sim/gem5)
    // and deliver each demand edge's epoch so ECG_GRASP_POPT eviction can rank by
    // a delivered (HW-faithful) epoch instead of the host-side findNextRef matrix.
    bool ecg_extract_enabled = graphbrew_sniper::ecg_extract_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", 256, 2, 65535));
    // Path A (epoch-filtered next-K lookahead, the combined-stack
    // prefetcher; mirrors cache_sim/gem5). K>0 prefetches the next-K in-neighbors'
    // contrib[], each filtered + stamped by its own next-ref epoch.
    int lean_pfx_k = graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_PREFETCH", 0, 0, 64);
    int pfx_epoch_filter = graphbrew_sniper::env_int_clamped("ECG_PREFETCH_EPOCH_FILTER", 0, 0, 2);
    int pfx_epoch_thresh_pct = graphbrew_sniper::env_int_clamped("ECG_PREFETCH_EPOCH_THRESH_PCT", 50, 0, 100);
    if (lean_pfx_k > 0 && graphbrew_sniper::env_int_clamped("SNIPER_ECG_PFX_HINT_FILTER", 16, 0, 64) != 0) {
        fprintf(stderr, "[sniper Path A] WARNING: ECG_EDGE_MASK_PREFETCH=%d but "
                "SNIPER_ECG_PFX_HINT_FILTER!=0 — the emit-side dedup will drop "
                "next-K survivors; set SNIPER_ECG_PFX_HINT_FILTER=0 for parity.\n", lean_pfx_k);
    }
    if (lean_pfx_k > 0 && !(ecg_prefetch_enabled && ecg_pfx_mode == 6)) {
        fprintf(stderr, "[sniper Path A] WARNING: ECG_EDGE_MASK_PREFETCH=%d but "
                "Path A runs only under the mode-6 prefetch loop (need ECG_PREFETCH=1 "
                "+ ECG_PREFETCH_MODE=6); Path A will NOT fire in this config.\n", lean_pfx_k);
    }
    vector<vector<uint16_t>> in_edge_epochs_by_src;
    if (ecg_extract_enabled || lean_pfx_k > 0) {
        ecg_reuse_plan::buildInEdgeEpochs(g, kNumVtxPerLine, ecg_epoch_count, true,
                                     in_edge_epochs_by_src);
    }

    for (NodeID n = 0; n < g.num_nodes(); n++) {
        outgoing_contrib[n] = init_score / g.out_degree(n);
    }

    SNIPER_ROI_BEGIN();

    for (int iter = 0; iter < max_iters; iter++) {
        double error = 0;
        // Parallel PR gather: each OpenMP thread runs on a distinct Sniper core
        // (real multi-core scaling). The read-of-neighbor-contrib while a peer
        // updates its own contrib[u] is the benign convergent race GAPBS tolerates.
        #pragma omp parallel reduction(+ : error)
        {
            // Per-thread prefetch-dedup window: each core has its own prefetch
            // stream, so the window is thread-private (a shared window would race
            // and conflate the cores' prefetch histories).
            vector<NodeID> pfx_window(PREFETCH_WINDOW, -1);
            int pfx_window_pos = 0;
            #pragma omp for schedule(dynamic, 1024)
            for (NodeID u = 0; u < g.num_nodes(); u++) {
            SNIPER_SET_VERTEX(u);
            ScoreT incoming_total = 0;

            auto in_neigh = g.in_neigh(u);

            // === Mode 6: per-edge ECG mask path ===
            if (ecg_prefetch_enabled && ecg_pfx_mode == 6
                && u < static_cast<NodeID>(in_edge_masks_by_src.size())) {
                const auto& src_masks = in_edge_masks_by_src[u];
                size_t edge_pos = 0;
                for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it, ++edge_pos) {
                    uint64_t mask = (edge_pos < src_masks.size()) ? src_masks[edge_pos] : 0;
                    NodeID v = static_cast<NodeID>(ecg_mode6::extractDest(mask));
                    if (ecg_extract_enabled &&
                        u < static_cast<NodeID>(in_edge_epochs_by_src.size())) {
                        const auto& eps = in_edge_epochs_by_src[u];
                        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos] : 0;
                        SNIPER_ECG_EXTRACT(v, ep);
                    }
                    uint32_t prefetch_target = ecg_mode6::extractPrefetchTarget(mask);
                    if (lean_pfx_k > 0) {
                        // Path A: prefetch next-K in-neighbors' contrib[], filtered
                        // by epoch. Each survivor's epoch is recorded (via the demand
                        // extract channel — the per-vertex map is the only path to a
                        // prefetch-filled line's epoch) so the prefetched line evicts
                        // correctly, then issued. cand is the streamed edge id (full
                        // width, no size limit).
                        const auto& eps = (u < static_cast<NodeID>(in_edge_epochs_by_src.size()))
                            ? in_edge_epochs_by_src[u] : in_edge_epochs_by_src[0];
                        uint32_t ne = ecg_epoch_count;
                        uint32_t cur_ep = ecg_reuse_plan::currentEpoch(u, g.num_nodes(), ne);
                        uint32_t thresh = static_cast<uint32_t>(
                            (static_cast<uint64_t>(pfx_epoch_thresh_pct) * ne) / 100);
                        auto jt = it;
                        size_t cpos = edge_pos;
                        for (int step = 0; step < lean_pfx_k; step++) {
                            ++jt; ++cpos;
                            if (jt == in_neigh.end()) break;
                            NodeID cand = *jt;
                            if (cand < 0) continue;
                            uint16_t cand_ep = (cpos < eps.size()) ? eps[cpos] : 0;
                            if (!ecg_reuse_plan::prefetchKeep(cand_ep, cur_ep, ne,
                                                         pfx_epoch_filter, thresh))
                                continue;
#if GRAPHBREW_SNIPER_HAS_SIM_API
                            SNIPER_ECG_EXTRACT(cand, cand_ep);
                            SNIPER_ECG_PFX_TARGET(cand);
#else
                            volatile ScoreT pf = outgoing_contrib[cand];
                            (void)pf;
#endif
                        }
                    } else if (prefetch_target != 0) {
                        bool in_window = false;
                        for (int w = 0; w < PREFETCH_WINDOW; w++) {
                            if (pfx_window[w] == static_cast<NodeID>(prefetch_target)) {
                                in_window = true;
                                break;
                            }
                        }
                        if (!in_window) {
#if GRAPHBREW_SNIPER_HAS_SIM_API
                            SNIPER_ECG_PFX_TARGET(prefetch_target);
#else
                            volatile ScoreT pf = outgoing_contrib[prefetch_target];
                            (void)pf;
#endif
                            pfx_window[pfx_window_pos % PREFETCH_WINDOW] = prefetch_target;
                            pfx_window_pos++;
                        }
                    }
                    incoming_total += outgoing_contrib[v];
                }
                ScoreT old_score = scores[u];
                scores[u] = base_score + kDamp * incoming_total;
                error += fabs(scores[u] - old_score);
                outgoing_contrib[u] = scores[u] / g.out_degree(u);
                continue;
            }

            size_t main_edge_pos = 0;
            for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it, ++main_edge_pos) {
                NodeID v = *it;
                if (ecg_extract_enabled &&
                    u < static_cast<NodeID>(in_edge_epochs_by_src.size())) {
                    const auto& eps = in_edge_epochs_by_src[u];
                    uint16_t ep = (main_edge_pos < eps.size()) ? eps[main_edge_pos] : 0;
                    SNIPER_ECG_EXTRACT(v, ep);
                }
                if (ecg_prefetch_enabled && pfx_lookahead > 0) {
                    NodeID pfx_target = -1;
                    int best_rank = hot_table_size + 1;
                    auto jt = it;
                    for (int step = 0; step < pfx_lookahead; step++) {
                        ++jt;
                        if (jt == in_neigh.end()) break;
                        NodeID candidate = *jt;
                        if (candidate >= 0 && candidate < static_cast<NodeID>(hub_rank.size()) &&
                            hub_rank[candidate] >= 0 && hub_rank[candidate] < best_rank) {
                            best_rank = hub_rank[candidate];
                            pfx_target = candidate;
                        }
                    }
                    if (pfx_target >= 0) {
                        bool in_window = false;
                        for (int w = 0; w < PREFETCH_WINDOW; w++) {
                            if (pfx_window[w] == pfx_target) {
                                in_window = true;
                                break;
                            }
                        }
                        if (!in_window) {
                            SNIPER_ECG_PFX_TARGET(pfx_target);
#if !GRAPHBREW_SNIPER_HAS_SIM_API
                            volatile ScoreT pf = outgoing_contrib[pfx_target];
                            (void)pf;
#endif
                            pfx_window[pfx_window_pos % PREFETCH_WINDOW] = pfx_target;
                            pfx_window_pos++;
                        }
                    }
                }
                incoming_total += outgoing_contrib[v];
                if (ecg_prefetch_enabled && pfx_lookahead == 0 && hub_rank[v] >= 0 && hot_table_size > 0) {
                    int next_hub = (hub_rank[v] + 1) % hot_table_size;
                    NodeID pfx_target = hot_table[next_hub];
                    bool in_window = false;
                    for (int w = 0; w < PREFETCH_WINDOW; w++) {
                        if (pfx_window[w] == pfx_target) {
                            in_window = true;
                            break;
                        }
                    }
                    if (!in_window) {
#if GRAPHBREW_SNIPER_HAS_SIM_API
                        SNIPER_ECG_PFX_TARGET(pfx_target);
#else
                        volatile ScoreT pf = outgoing_contrib[pfx_target];
                        (void)pf;
#endif
                        pfx_window[pfx_window_pos % PREFETCH_WINDOW] = pfx_target;
                        pfx_window_pos++;
                    }
                }
            }
            ScoreT old_score = scores[u];
            scores[u] = base_score + kDamp * incoming_total;
            error += fabs(scores[u] - old_score);
            outgoing_contrib[u] = scores[u] / g.out_degree(u);
            }
        }
        if (error < epsilon) break;
    }

    SNIPER_ROI_END();
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        score_pairs[n] = make_pair(n, scores[n]);
    }
    int k = min(5, static_cast<int>(g.num_nodes()));
    partial_sort(score_pairs.begin(), score_pairs.begin() + k, score_pairs.end(),
                 [](auto a, auto b) { return a.second > b.second; });
    for (int i = 0; i < k; i++) {
        cout << score_pairs[i].first << ": " << score_pairs[i].second << endl;
    }
}

bool PRVerifier(const Graph &g, const pvector<ScoreT> &scores, double target_error) {
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> incoming_sums(g.num_nodes(), 0);
    double error = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        ScoreT outgoing_contrib = scores[u] / g.out_degree(u);
        for (NodeID v : g.out_neigh(u)) {
            incoming_sums[v] += outgoing_contrib;
        }
    }
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        error += fabs(base_score + kDamp * incoming_sums[n] - scores[n]);
    }
    cout << "Total Error: " << error << endl;
    return error < target_error;
}

int main(int argc, char *argv[]) {
    CLPageRank cli(argc, argv, "pagerank-sniper", 1e-4, 20);
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto PRBound = [&cli](const Graph &g) {
        return PageRankPullGS_Sniper(g, cli.max_iters(), cli.tolerance());
    };
    auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
        return PRVerifier(g, scores, cli.tolerance());
    };
    BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
    return 0;
}
