// ============================================================================
// BFS (Top-Down only) for Sniper simulation
// ============================================================================
// Mirrors the audited gem5 BFS wrapper while using Sniper ROI markers and
// sideband export helpers.
// ============================================================================

#include <iostream>
#include <queue>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"
#include "sliding_queue.h"

#include "graphbrew/partition/cagra/popt.h"
#include "sniper_sim/sniper_harness.h"

using namespace std;

pvector<NodeID> BFS_Sniper(const Graph &g, NodeID source) {
    pvector<NodeID> parent(
        g.num_nodes(), NodeID(-1), graphbrew_sniper::property_alignment());
    parent[source] = source;

    sniper_report_region("parent", parent.data(), g.num_nodes(), sizeof(NodeID));

    SniperPropertyRegion regions[1] = {
        {"parent", reinterpret_cast<uint64_t>(parent.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(g.num_nodes()), sizeof(NodeID)},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(g, edge_regions, 2);
    {
        constexpr int numVtxPerLine = 64 / sizeof(NodeID);
        constexpr int numEpochs = 256;
        static pvector<uint8_t> popt_matrix;
        // BFS top-down pushes along OUT-edges reading parent[dest]; the next-ref of a
        // vertex's property is over its IN-neighbours, so the reref matrix is the graph
        // TRANSPOSE (CSC/in_neigh, traverseCSR=false) — matching cache_sim's natural_csr=
        // false. Default true=out_neigh is only correct for PR's in-pull. Undirected graphs
        // force true internally (out==in), so this is do-no-harm on the symmetric corpus.
        makeOffsetMatrix(g, popt_matrix, numVtxPerLine, numEpochs, /*traverseCSR=*/false);
        int numCacheLines = (g.num_nodes() + numVtxPerLine - 1) / numVtxPerLine;
        sniper_export_popt_matrix(popt_matrix.data(), numCacheLines,
                                  numEpochs, g.num_nodes());
    }
    sniper_export_context(regions, 1, g, nullptr, edge_regions, num_edge_regions);
    volatile NodeID* warm_parent = parent.data();
    for (NodeID n = 0; n < g.num_nodes(); ++n)
        warm_parent[n] = n == source ? source : -1;

    SNIPER_ROI_BEGIN();
    int pfx_lookahead = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_PFX_LOOKAHEAD",
        graphbrew_sniper::env_int_clamped("ECG_PREFETCH_LOOKAHEAD", 4, 0, 64),
        0, 64);

    // Level-synchronous parallel BFS (GAPBS top-down): the whole current frontier
    // is expanded in parallel across the OpenMP threads (= Sniper cores). parent[]
    // is claimed with an atomic compare-and-swap so exactly one thread owns each
    // newly discovered vertex; per-thread QueueBuffers build the next frontier.
    // Any valid BFS tree at the correct depths passes BFSVerifier, so the
    // nondeterministic parent choice among a level is fine.
    SlidingQueue<NodeID> frontier(g.num_nodes());
    frontier.push_back(source);
    frontier.slide_window();
    while (!frontier.empty()) {
        #pragma omp parallel
        {
            QueueBuffer<NodeID> lqueue(frontier);
            #pragma omp for schedule(dynamic, 64) nowait
            for (auto q_iter = frontier.begin(); q_iter < frontier.end(); q_iter++) {
                NodeID u = *q_iter;
                SNIPER_SET_VERTEX(u);
                auto out_neigh = g.out_neigh(u);
                for (auto it = out_neigh.begin(); it != out_neigh.end(); ++it) {
                    NodeID v = *it;
                    if (pfx_lookahead > 0) {
                        auto jt = it;
                        for (int step = 0; step < pfx_lookahead; step++) {
                            ++jt;
                            if (jt == out_neigh.end()) break;
                            SNIPER_ECG_PFX_TARGET(*jt);
                            break;
                        }
                    } else {
                        SNIPER_ECG_PFX_TARGET(v);
                    }
                    NodeID curr_val = parent[v];
                    if (curr_val == -1) {
                        if (compare_and_swap(parent[v], curr_val, u)) {
                            lqueue.push_back(v);
                        }
                    }
                }
            }
            lqueue.flush();
        }
        frontier.slide_window();
    }

    SNIPER_ROI_END();
    return parent;
}

void PrintBFSStats(const Graph &g, const pvector<NodeID> &parent) {
    int64_t visited = 0;
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        if (parent[n] >= 0) visited++;
    }
    cout << "Visited: " << static_cast<int>(visited) << "/" << static_cast<int>(g.num_nodes()) << endl;
}

bool BFSVerifier(const Graph &g, NodeID source, const pvector<NodeID> &parent) {
    if (parent[source] != source) return false;
    pvector<int32_t> depth(g.num_nodes(), -1);
    depth[source] = 0;
    queue<NodeID> q;
    q.push(source);
    while (!q.empty()) {
        NodeID u = q.front();
        q.pop();
        for (NodeID v : g.out_neigh(u)) {
            if (depth[v] == -1) {
                depth[v] = depth[u] + 1;
                q.push(v);
            }
        }
    }
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        if (depth[n] >= 0 && parent[n] < 0) return false;
        if (depth[n] < 0 && parent[n] >= 0) return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "bfs-sniper");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();
    SourcePicker<Graph> sp(g, cli.start_vertex());
    // Separate verifier picker seeded identically to sp so BFSBound (kernel) and
    // VerifyBound (verifier) draw the SAME source sequence — matches cache_sim/canonical
    // (bench/src/bfs.cc). Without this the single picker hands the verifier a DIFFERENT
    // source than the kernel ran on, so verification FAILs unless -r fixes the source.
    SourcePicker<Graph> vsp(g, cli.start_vertex());

    auto BFSBound = [&sp](const Graph &g) {
        return BFS_Sniper(g, sp.PickNext());
    };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &p) {
        PrintBFSStats(g, p);
    };
    auto VerifyBound = [&vsp](const Graph &g, const pvector<NodeID> &p) {
        return BFSVerifier(g, vsp.PickNext(), p);
    };
    BenchmarkKernel(cli, g, BFSBound, PrintBound, VerifyBound);
    return 0;
}
