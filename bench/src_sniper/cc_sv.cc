// ============================================================================
// Connected Components (Shiloach-Vishkin) for Sniper simulation
// ============================================================================
// Parallel label-propagation CC (one of P-OPT's five evaluated apps), mirroring
// the audited gem5 cc_sv wrapper but multi-threaded across the OpenMP threads
// (= Sniper cores). Each hooking sweep and compress pass runs in parallel with
// atomic hooks; the shared `change` flag is only ever set true (benign race, as
// in GAPBS). Sideband/ROI/ECG wiring matches cc.cc so every replacement policy
// and DROPLET see CC's comp[] property stream.
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
#include "ecg_epoch_builder.h"

using namespace std;

pvector<NodeID> ShiloachVishkin_Sniper(const Graph &g) {
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
    // P-OPT reref matrix: SV reads comp[dest] over OUT-edges -> a vertex's comp is
    // next-referenced by its IN-neighbours -> transpose reref (traverseCSR=false).
    constexpr int kNumVtxPerLine = 64 / sizeof(NodeID);
    constexpr int kNumEpochs = 256;
    static pvector<uint8_t> popt_matrix;
    int popt_num_cache_lines = (g.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    makeOffsetMatrix(g, popt_matrix, kNumVtxPerLine, kNumEpochs, /*traverseCSR=*/false);
    sniper_export_popt_matrix(popt_matrix.data(), popt_num_cache_lines,
                              kNumEpochs, g.num_nodes());
    sniper_export_context(regions, 1, g, nullptr, edge_regions, num_edge_regions);

    // Per-edge next-ref epochs for ECG_GRASP_POPT (SNIPER_ECG_EXTRACT, push_out_edges=true).
    bool ecg_extract_enabled = graphbrew_sniper::ecg_extract_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", kNumEpochs, 2, 65535));
    vector<vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_enabled) {
        ecg_epoch::buildInEdgeEpochs(g, kNumVtxPerLine, ecg_epoch_count,
                                     /*linemin=*/true, out_edge_epochs,
                                     /*push_out_edges=*/true);
    }
    auto deliver = [&](NodeID u, size_t edge_pos, NodeID v) {
        if (!ecg_extract_enabled || static_cast<size_t>(u) >= out_edge_epochs.size())
            return;
        const auto& eps = out_edge_epochs[u];
        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                      : static_cast<uint16_t>(ecg_epoch_count - 1);
        SNIPER_ECG_EXTRACT(v, ep);
    };

    SNIPER_ROI_BEGIN();

    bool change = true;
    while (change) {
        change = false;
        // Parallel hooking sweep: hook the higher label onto the lower with an
        // atomic CAS so only one thread updates a given root. `change` is only
        // set true (never false) inside the region -> benign race (GAPBS).
        #pragma omp parallel for schedule(dynamic, 1024)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            SNIPER_SET_VERTEX(u);
            size_t edge_pos = 0;
            for (NodeID v : g.out_neigh(u)) {
                deliver(u, edge_pos, v);
                ++edge_pos;
                NodeID comp_u = comp[u];
                NodeID comp_v = comp[v];
                if (comp_u == comp_v) continue;
                NodeID high = comp_u > comp_v ? comp_u : comp_v;
                NodeID low = comp_u + (comp_v - high);
                if (comp[high] == high &&
                    compare_and_swap(comp[high], high, low))
                    change = true;
            }
        }
        // Parallel multi-hop compression to component roots.
        #pragma omp parallel for schedule(dynamic, 16384)
        for (NodeID n = 0; n < g.num_nodes(); n++)
            while (comp[n] != comp[comp[n]])
                comp[n] = comp[comp[n]];
    }

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
    CLApp cli(argc, argv, "cc-sv-sniper");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto SVBound = [](const Graph &g) { return ShiloachVishkin_Sniper(g); };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &c) { PrintCompStats(g, c); };
    auto VerifyBound = [](const Graph &g, const pvector<NodeID> &c) { return CCVerifier(g, c); };
    BenchmarkKernel(cli, g, SVBound, PrintBound, VerifyBound);
    return 0;
}
