// ============================================================================
// Connected Components (Shiloach-Vishkin) for gem5 SE-mode simulation
// ============================================================================

#include <iostream>
#include <unordered_map>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "gem5_sim/gem5_harness.h"

using namespace std;

pvector<NodeID> ShiloachVishkin_Gem5(const Graph &g) {
    pvector<NodeID> comp(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) comp[n] = n;

    gem5_report_region("comp", comp.data(), g.num_nodes(), sizeof(NodeID));

    Gem5PropertyRegion regions[1] = {
        {"comp", reinterpret_cast<uint64_t>(comp.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(g.num_nodes()), sizeof(NodeID)},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);
    gem5_export_context(regions, 1, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions);

    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);

    bool change = true;
    while (change) {
        change = false;
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            GEM5_SET_VERTEX(u);
            for (NodeID v : g.out_neigh(u)) {
                NodeID comp_u = comp[u], comp_v = comp[v];
                if (comp_u != comp_v) {
                    NodeID high = max(comp_u, comp_v);
                    NodeID low = min(comp_u, comp_v);
                    if (comp[high] == high) {
                        comp[high] = low;
                        change = true;
                    }
                }
            }
        }
        // Compress
        for (NodeID n = 0; n < g.num_nodes(); n++)
            while (comp[n] != comp[comp[n]])
                comp[n] = comp[comp[n]];
    }

    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
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
    CLApp cli(argc, argv, "cc-sv-gem5");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto SVBound = [](const Graph &g) { return ShiloachVishkin_Gem5(g); };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &c) { PrintCompStats(g, c); };
    auto VerifyBound = [](const Graph &g, const pvector<NodeID> &c) { return CCVerifier(g, c); };
    BenchmarkKernel(cli, g, SVBound, PrintBound, VerifyBound);
    return 0;
}
