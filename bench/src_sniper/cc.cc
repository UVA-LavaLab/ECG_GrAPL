// ============================================================================
// Connected Components (Afforest) for Sniper simulation
// ============================================================================
// Parallel Afforest (subgraph sampling + atomic union-find) mirroring the
// audited gem5 CC wrapper, but multi-threaded so the whole current frontier of
// work is expanded across the OpenMP threads (= Sniper cores). Uses Sniper ROI
// markers, sideband export, the P-OPT reref matrix, and the per-edge ECG epoch
// delivery channel (SNIPER_ECG_EXTRACT) so every replacement policy (LRU/SRRIP/
// GRASP/POPT/ECG) and DROPLET can act on CC's comp[] property stream.
// ============================================================================

#include <algorithm>
#include <iostream>
#include <unordered_map>
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

// Atomic union-find link (GAPBS Afforest) — safe to call from multiple threads.
static void Link(NodeID u, NodeID v, pvector<NodeID>& comp) {
    NodeID p1 = comp[u];
    NodeID p2 = comp[v];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        NodeID p_high = comp[high];
        if ((p_high == low) ||
            (p_high == high && compare_and_swap(comp[high], high, low)))
            break;
        p1 = comp[comp[high]];
        p2 = comp[low];
    }
}

static void LinkLoaded(
        NodeID u, NodeID v, NodeID p2, pvector<NodeID>& comp) {
    NodeID p1 = comp[u];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        NodeID p_high = comp[high];
        if ((p_high == low) ||
            (p_high == high && compare_and_swap(comp[high], high, low)))
            break;
        p1 = comp[comp[high]];
        p2 = comp[low];
    }
}

static void Compress(const Graph &g, pvector<NodeID>& comp) {
    #pragma omp parallel for schedule(dynamic, 16384)
    for (NodeID n = 0; n < g.num_nodes(); n++)
        while (comp[n] != comp[comp[n]])
            comp[n] = comp[comp[n]];
}

pvector<NodeID> Afforest_Sniper(const Graph &g, int32_t neighbor_rounds = 2) {
    pvector<NodeID> comp(
        g.num_nodes(), NodeID(0), graphbrew_sniper::property_alignment());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++) comp[n] = n;

    sniper_report_region("comp", comp.data(), g.num_nodes(), sizeof(NodeID));
    SniperPropertyRegion regions[1] = {
        {"comp", reinterpret_cast<uint64_t>(comp.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(g.num_nodes()), sizeof(NodeID), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(g, edge_regions, 2, true);
    // P-OPT reref matrix: CC reads comp[dest] over OUT-edges, so a vertex's comp is
    // next-referenced by its IN-neighbours -> transpose reref (traverseCSR=false),
    // matching bfs.cc and the push_out_edges=true epoch direction below.
    constexpr int kNumVtxPerLine = 64 / sizeof(NodeID);
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

    // Per-edge next-ref epochs for the ECG_GRASP_POPT eviction hint, delivered per
    // demand edge via SNIPER_ECG_EXTRACT (mirror gem5 cc.cc: push_out_edges=true).
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
        pair_ok = true;
    }
    sniper_export_context(
        regions, 1, g, nullptr, edge_regions, num_edge_regions);
    auto deliver = [&](NodeID u, size_t edge_pos, NodeID v) {
        if (!ecg_extract_enabled || static_cast<size_t>(u) >= out_edge_epochs.size())
            return;
        const auto& eps = out_edge_epochs[u];
        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                      : static_cast<uint16_t>(ecg_epoch_count - 1);
        SNIPER_ECG_EXTRACT(v, ep);
    };

    SNIPER_ROI_BEGIN();

    // Phase 1: sample the r-th out-neighbour of each vertex (parallel), compress.
    for (int32_t r = 0; r < neighbor_rounds; r++) {
        #pragma omp parallel for schedule(dynamic, 1024)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            SNIPER_SET_VERTEX(u);
            if (pair_ok &&
                static_cast<size_t>(u + 1) < pair_off.size() &&
                pair_off[u] + static_cast<uint64_t>(r) < pair_off[u + 1]) {
                const uint64_t record =
                    pair_flat[pair_off[u] + static_cast<uint64_t>(r)];
                const NodeID v = static_cast<NodeID>(
                    ecg_reuse_plan::extractReusePlanDest(record));
                SNIPER_ECG_EXTRACT2(
                    static_cast<uint32_t>(v),
                    ecg_reuse_plan::extractReusePlanTier(record),
                    ecg_reuse_plan::extractReusePlanFirst(record),
                    ecg_reuse_plan::extractReusePlanSecond(record));
                const NodeID delivered_comp = comp[v];
                SNIPER_ECG_CLEAR_EXTRACT2(v);
                LinkLoaded(u, v, delivered_comp, comp);
            } else if (!pair_ok) {
                auto out_neigh = g.out_neigh(u);
                auto it = out_neigh.begin();
                for (int32_t i = 0;
                     i < r && it != out_neigh.end(); ++i, ++it) {}
                if (it != out_neigh.end()) {
                    deliver(u, static_cast<size_t>(r), *it);
                    Link(u, *it, comp);
                }
            }
        }
        Compress(g, comp);
    }

    // Most frequent component = the giant component skipped in phase 2.
    unordered_map<NodeID, int64_t> count;
    for (NodeID n = 0; n < g.num_nodes(); n++) count[comp[n]]++;
    NodeID largest = max_element(count.begin(), count.end(),
        [](const auto &a, const auto &b){ return a.second < b.second; })->first;

    // Phase 2: full traversal for vertices outside the giant component (parallel).
    #pragma omp parallel for schedule(dynamic, 1024)
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        if (comp[u] == largest) continue;
        SNIPER_SET_VERTEX(u);
        if (pair_ok && static_cast<size_t>(u + 1) < pair_off.size()) {
            for (uint64_t pos = pair_off[u]; pos < pair_off[u + 1]; ++pos) {
                const uint64_t record = pair_flat[pos];
                const NodeID v = static_cast<NodeID>(
                    ecg_reuse_plan::extractReusePlanDest(record));
                SNIPER_ECG_EXTRACT2(
                    static_cast<uint32_t>(v),
                    ecg_reuse_plan::extractReusePlanTier(record),
                    ecg_reuse_plan::extractReusePlanFirst(record),
                    ecg_reuse_plan::extractReusePlanSecond(record));
                const NodeID delivered_comp = comp[v];
                SNIPER_ECG_CLEAR_EXTRACT2(v);
                LinkLoaded(u, v, delivered_comp, comp);
            }
        } else {
            size_t edge_pos = 0;
            for (NodeID v : g.out_neigh(u)) {
                deliver(u, edge_pos, v);
                ++edge_pos;
                Link(u, v, comp);
            }
        }
    }
    Compress(g, comp);

    SNIPER_ROI_END();
    return comp;
}

void PrintCompStats(const Graph &g, const pvector<NodeID> &comp) {
    unordered_map<NodeID, int64_t> count;
    for (NodeID n = 0; n < g.num_nodes(); n++) count[comp[n]]++;
    cout << "Components: " << count.size() << endl;
}

bool CCVerifier(const Graph &g, const pvector<NodeID> &comp) {
    for (NodeID u = 0; u < g.num_nodes(); u++)
        for (NodeID v : g.out_neigh(u))
            if (comp[u] != comp[v]) return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "cc-sniper");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto CCBound = [](const Graph &g) { return Afforest_Sniper(g); };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &c) { PrintCompStats(g, c); };
    auto VerifyBound = [](const Graph &g, const pvector<NodeID> &c) { return CCVerifier(g, c); };
    BenchmarkKernel(cli, g, CCBound, PrintBound, VerifyBound);
    return 0;
}
