// ============================================================================
// Triangle Counting for gem5 SE-mode simulation
// ============================================================================
// Ordered triangle counting: for each edge (u,v) where u < v,
// count |N(u) ∩ N(v)| via sorted neighbor merge.
// ============================================================================

#include <algorithm>
#include <iostream>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "gem5_sim/gem5_harness.h"

using namespace std;

size_t OrderedCount_Gem5(const Graph &g) {
    size_t total = 0;

    // TC accesses CSR neighbor lists (no separate property arrays),
    // but export context with empty regions for topology info
    Gem5PropertyRegion regions[1] = {
        {"csr_edges", 0, 0, 0, 0},  // Placeholder — TC has no vertex property array
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);
    gem5_export_context(regions, 0, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions);  // 0 regions, just topology

    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);

    for (NodeID u = 0; u < g.num_nodes(); u++) {
        GEM5_SET_VERTEX(u);
        for (NodeID v : g.out_neigh(u)) {
            if (v > u) break;  // Only count u < v (sorted neighbor lists)
            // Merge-join N(u) ∩ N(v)
            auto it_u = g.out_neigh(u).begin();
            auto it_v = g.out_neigh(v).begin();
            auto end_u = g.out_neigh(u).end();
            auto end_v = g.out_neigh(v).end();
            while (it_u != end_u && it_v != end_v) {
                if (*it_u == *it_v) {
                    total++;
                    ++it_u; ++it_v;
                } else if (*it_u < *it_v) {
                    ++it_u;
                } else {
                    ++it_v;
                }
            }
        }
    }

    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    return total;
}

void PrintTriangleStats(const Graph &g, size_t total) {
    cout << "Triangles: " << total << endl;
}

bool TCVerifier(const Graph &g, size_t test_total) {
    // Simple re-count for verification
    size_t check = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++)
        for (NodeID v : g.out_neigh(u))
            if (v > u) break;
            else {
                auto it_u = g.out_neigh(u).begin();
                auto it_v = g.out_neigh(v).begin();
                while (it_u != g.out_neigh(u).end() && it_v != g.out_neigh(v).end()) {
                    if (*it_u == *it_v) { check++; ++it_u; ++it_v; }
                    else if (*it_u < *it_v) ++it_u;
                    else ++it_v;
                }
            }
    return test_total == check;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "tc-gem5");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto TCBound = [](const Graph &g) { return OrderedCount_Gem5(g); };
    auto PrintBound = [](const Graph &g, size_t t) { PrintTriangleStats(g, t); };
    auto VerifyBound = [](const Graph &g, size_t t) { return TCVerifier(g, t); };
    BenchmarkKernel(cli, g, TCBound, PrintBound, VerifyBound);
    return 0;
}
