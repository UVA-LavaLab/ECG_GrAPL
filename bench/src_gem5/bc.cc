// ============================================================================
// Betweenness Centrality (Brandes) for gem5 SE-mode simulation
// ============================================================================
// Single-threaded Brandes BC for gem5. BFS forward + backward accumulation.
// ============================================================================

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <queue>
#include <stack>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "ecg_epoch_builder.h"
#include "ecg_mode6_builder.h"

#include "gem5_sim/gem5_harness.h"
#include "ecg_metadata.h"

using namespace std;

typedef float ScoreT;

pvector<ScoreT> Brandes_Gem5(const Graph &g, int num_iters) {
    constexpr size_t kPropAlign = 4096;  // page-align hot property arrays (see pr.cc)
    pvector<ScoreT> scores(g.num_nodes(), ScoreT(0), kPropAlign);
    pvector<int32_t> depth(g.num_nodes(), int32_t(0), kPropAlign);
    pvector<int64_t> path_counts(g.num_nodes(), int64_t(0), kPropAlign);
    pvector<ScoreT> deltas(g.num_nodes(), ScoreT(0), kPropAlign);

    gem5_report_region("scores", scores.data(), g.num_nodes(), sizeof(ScoreT));
    gem5_report_region("depth", depth.data(), g.num_nodes(), sizeof(int32_t));
    gem5_report_region("path_counts", path_counts.data(), g.num_nodes(), sizeof(int64_t));
        gem5_report_region("deltas", deltas.data(), g.num_nodes(), sizeof(ScoreT));

        // GRASP protects vertex-indexed property arrays. BC has four
        // such arrays (all indexed by vertex id), so we mark all of them as
        // grasp_region=true. classifyGRASP() applies the same hot/moderate
        // boundary per region; marking only one of four arrays (the original
        // behaviour) caused the other three to thrash under SRRIP. Keep this
        // registration identical to bench/src_sim/bc.cc.
        Gem5PropertyRegion regions[4] = {
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
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);

    // Per-edge next-ref epoch budget keyed on depth (int32). BC pushes along
    // OUT-edges reading depth[dest]; Schedule-2 uses its fixed 8-byte pair
    // record and bypasses the legacy 32-bit single-epoch cap.
    constexpr int kNumVtxPerLine = 64 / sizeof(int32_t);
    const int ecg_sched_k =
        gem5_env_int_clamped("ECG_EDGE_MASK_SCHED", 0, 0, 4);
    uint32_t requested_epoch_count = static_cast<uint32_t>(
        gem5_env_int_clamped("ECG_EDGE_MASK_EPOCHS", 65535, 2, 65535));
    if (ecg_sched_k == 2)
        requested_epoch_count =
            ecg_epoch::normalizeK2EpochCount(requested_epoch_count);
    uint8_t edge_id_bits = 1;
    while ((1ULL << edge_id_bits) < static_cast<uint64_t>(g.num_nodes())) edge_id_bits++;
    uint32_t edge_epoch_count = requested_epoch_count;
    if (ecg_sched_k != 2) {
        if (edge_id_bits < 32) {
            uint32_t spare = 32u - edge_id_bits;
            uint32_t ne_cap = (spare >= 16) ? 65535u : (1u << spare);
            edge_epoch_count = std::min<uint32_t>(
                edge_epoch_count, std::max<uint32_t>(2u, ne_cap));
        } else {
            edge_epoch_count = 2;
        }
    }

    // Deliver one edge mask on both irregular forward-BFS loads: depth[dest]
    // and path_counts[dest]. The backward successor-DAG phase remains plain.
    bool ecg_extract_on = gem5_ecg_extract_enabled();
    // Schedule-2 loads the packed record, then carries its K2 mask on the exact
    // depth[dest] request. StreamShield remains on the record request.
    const bool ecg_load2_on = gem5_ecg_load2_enabled();
    const bool ecg_stream_load2_on = gem5_ecg_stream_load2_enabled();
    const bool ecg_k2_pload_on =
        gem5_ecg_pload_enabled() && ecg_sched_k == 2;
    const bool ecg_k2_mask_only_on =
        ecg_k2_pload_on && gem5_ecg_k2_mask_only_enabled();
    if (ecg_load2_on || ecg_stream_load2_on || ecg_k2_pload_on)
        ecg_extract_on = true;
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_sched_k != 2) {
        ecg_epoch::buildInEdgeEpochs(g, static_cast<uint32_t>(kNumVtxPerLine),
                                     edge_epoch_count, /*linemin=*/true,
                                     out_edge_epochs, /*push_out_edges=*/true);
    }
    std::vector<uint64_t> pair_off;
    pvector<uint64_t> pair_flat;
    bool pair_ok = false;
    // Width and structure come from the shared metadata definition, the same
    // header cache_sim and the other backends use, so no simulator can
    // compute a record width of its own.
    {
        auto ecg_meta = ::ecg_metadata::configure(
            static_cast<uint64_t>(g.num_nodes()), edge_epoch_count);
        // No compact path here yet: this kernel builds the 64-bit Schedule-2
        // record, so it streams 8 bytes per edge whatever the budget computes.
        // Declaring it keeps the receipt honest; only gem5 PR has the compact
        // 32-bit record so far.
        if (ecg_sched_k == 2)
            ::ecg_metadata::declareContainerBytes(ecg_meta, 8);
        ::ecg_metadata::announce(ecg_meta, "gem5-bc");
        ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "gem5-bc");
    }
    if (ecg_extract_on && ecg_sched_k == 2) {
        std::vector<uint64_t> pair_records;
        ecg_epoch::buildInEdgeEpochPairRecords(
            g, static_cast<uint32_t>(kNumVtxPerLine),
            edge_epoch_count, /*linemin=*/true,
            pair_off, pair_records, /*push_out_edges=*/true);
        pair_flat = pvector<uint64_t>(
            pair_records.size(), uint64_t(0), 4096);
        std::copy(pair_records.begin(), pair_records.end(), pair_flat.begin());
        pair_ok = true;
    }
    gem5_export_context(regions, 4, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions, edge_epoch_count);
    const bool ecg_load_evict_on =
        gem5_ecg_pload_enabled() && ecg_extract_on && ecg_sched_k != 2;
    const int  ecg_evict_wc = ecg_mode6::ecgEvictWidthClass(g.num_nodes());
    if (ecg_load_evict_on)
        fprintf(stderr, "[ECG_PLOAD] BC fused ecg.load EVICT delivery (depth) ACTIVE\n");
    if (pair_ok) {
        fprintf(stderr,
                ecg_stream_load2_on && ecg_k2_pload_on
                    ? (ecg_k2_mask_only_on
                        ? "[ECG_K2_MLOAD] BC computed-address masked loads "
                          "+ StreamShield record load ACTIVE\n"
                        : "[ECG_K2_ILOAD] BC fused indexed masked loads "
                          "+ StreamShield record load ACTIVE\n")
                    : ecg_k2_pload_on
                        ? (ecg_k2_mask_only_on
                            ? "[ECG_K2_MLOAD] BC computed-address masked loads ACTIVE\n"
                            : "[ECG_K2_ILOAD] BC fused indexed masked loads ACTIVE\n")
                    : ecg_stream_load2_on
                        ? "[ECG_STREAM_LOAD2] BC request-bound StreamShield+K2 ACTIVE\n"
                    : ecg_load2_on
                        ? "[ECG_LOAD2] BC fused K2 record load ACTIVE\n"
                        : "[ECG_PACKED8_K2] BC Schedule-2 packed record path ACTIVE\n");
    }

    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    GEM5_ECG_BEGIN_CONTEXT();

    // Pick sources round-robin
    for (int iter = 0; iter < num_iters; iter++) {
        NodeID source = iter % g.num_nodes();

        // Reset
        for (NodeID n = 0; n < g.num_nodes(); n++) {
            depth[n] = -1;
            path_counts[n] = 0;
            deltas[n] = 0;
        }
        depth[source] = 0;
        path_counts[source] = 1;

        // Forward BFS
        stack<NodeID> order;
        queue<NodeID> q;
        q.push(source);
        while (!q.empty()) {
            NodeID u = q.front(); q.pop();
            const int32_t current_depth = depth[u];
            const int64_t source_paths = path_counts[u];
            GEM5_SET_VERTEX_EPOCH(
                u, g.num_nodes(), edge_epoch_count);
            order.push(u);
            const std::vector<uint16_t>* u_epochs =
                (ecg_load_evict_on && static_cast<size_t>(u) < out_edge_epochs.size())
                    ? &out_edge_epochs[u] : nullptr;
            auto process_neighbor = [&](
                    NodeID v, int32_t dv,
                    uint64_t record, bool masked_path_count) {
                if (dv == -1) {
                    depth[v] = current_depth + 1;
                    q.push(v);
                    dv = current_depth + 1;
                }
                if (dv == current_depth + 1) {
                    const int64_t old_paths = masked_path_count
                        ? static_cast<int64_t>(ecg_k2_mask_only_on
                            ? gem5_ecg_mload_k2_u64(
                                &path_counts[v], record)
                            : gem5_ecg_load_k2_u64(
                                path_counts.data(), record))
                        : path_counts[v];
                    path_counts[v] = old_paths + source_paths;
                }
            };
            if (pair_ok &&
                static_cast<size_t>(u + 1) < pair_off.size()) {
                // The packed K2 record replaces the unweighted CSR edge word.
                for (uint64_t pos = pair_off[u];
                     pos < pair_off[u + 1]; ++pos) {
                    const uint64_t record = ecg_stream_load2_on
                        ? gem5_ecg_stream_load2_instruction(&pair_flat[pos])
                        : ecg_load2_on
                            ? gem5_ecg_load2_instruction(&pair_flat[pos])
                            : pair_flat[pos];
                    const NodeID v = static_cast<NodeID>(
                        ecg_epoch::extractEpochPairDest(record));
                    int32_t dv;
                    if (ecg_k2_pload_on) {
                        if (ecg_k2_mask_only_on) {
                            dv = gem5_ecg_mload_k2_s32(
                                &depth[v], record);
                        } else {
                            const uint32_t bits =
                                gem5_ecg_load_k2(depth.data(), record);
                            std::memcpy(&dv, &bits, sizeof(int32_t));
                        }
                    } else {
                        if (!ecg_load2_on)
                            GEM5_ECG_EXTRACT2(record);
                        dv = depth[v];
                        GEM5_ECG_CLEAR_EXTRACT2_HINT();
                    }
                    process_neighbor(
                        v, dv, record, ecg_k2_pload_on);
                }
            } else {
                size_t edge_pos = 0;
                for (NodeID v : g.out_neigh(u)) {
                    int32_t dv;
                    if (u_epochs) {
                    uint16_t epoch = (edge_pos < u_epochs->size())
                        ? (*u_epochs)[edge_pos]
                        : static_cast<uint16_t>(edge_epoch_count - 1);
                    uint64_t fat = ecg_mode6::packEvict(static_cast<uint32_t>(v),
                                                        epoch, ecg_evict_wc);
                    uint32_t bits = gem5_ecg_load_evict(depth.data(), fat, ecg_evict_wc);
                    std::memcpy(&dv, &bits, sizeof(int32_t));
                    } else {
                        dv = depth[v];
                    }
                    ++edge_pos;
                    process_neighbor(v, dv, 0, false);
                }
            }
        }

        // Backward accumulation
        while (!order.empty()) {
            NodeID w = order.top(); order.pop();
            for (NodeID v : g.out_neigh(w)) {
                if (depth[v] == depth[w] + 1) {
                    deltas[w] += (ScoreT)path_counts[w] / path_counts[v]
                                 * (1.0f + deltas[v]);
                }
            }
            if (w != source)
                scores[w] += deltas[w];
        }
    }

    GEM5_ECG_END_CONTEXT();
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> sp(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) sp[n] = {n, scores[n]};
    int k = min(5, (int)g.num_nodes());
    partial_sort(sp.begin(), sp.begin() + k, sp.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
    for (int i = 0; i < k; i++)
        cout << sp[i].first << ": " << sp[i].second << endl;
}

bool BCVerifier(const Graph &g, const pvector<ScoreT> &scores, int num_iters) {
    // Accept if scores are non-negative
    for (NodeID n = 0; n < g.num_nodes(); n++)
        if (scores[n] < 0) return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLIterApp cli(argc, argv, "bc-gem5", 1);
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto BCBound = [&cli](const Graph &g) {
        return Brandes_Gem5(g, cli.num_iters());
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
