// Directed-graph oracle test for buildOutEdgeMasks (the OUT-edge mirror of
// buildInEdgeMasks_PR). Symmetric graphs cannot validate direction (in==out), so
// this uses a tiny DIRECTED graph with hand-computed expected epochs.
//
// buildOutEdgeMasks fills, for each out-edge src->dest, the absolute next-ref EPOCH
// of dest's property line, computed from dest's IN-neighbours (the transpose of OUT
// traversal): the soonest in-neighbour of dest strictly > src (wrapping to the next
// iteration), mapped to epoch = next_nbr * ne / n. We assert the decoded epochs
// match an independent oracle.
#include "cache_sim/graph_cache_context.h"
#include "external/gapbs/graph.h"
#include "external/gapbs/builder.h"
#include "external/gapbs/command_line.h"
#include <cstdio>
#include <vector>

using namespace cache_sim;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, uint32_t got, uint32_t expect) {
    bool ok = (got == expect);
    printf("    %-46s got=%-3u expect=%-3u [%s]\n", name, got, expect, ok ? "OK" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

int main() {
    printf("[test_ecg_out_edge_mask] buildOutEdgeMasks directed oracle\n");

    // Directed graph (NOT symmetrized). Edges chosen so in_neigh != out_neigh:
    //   0->2  1->2  3->2   (in_neigh(2) = [0,1,3])
    //   2->4  4->1          (in_neigh(4)=[2], in_neigh(1)=[4])
    // n=5. ne=5 so epoch(v) = v (next_nbr*5/5).
    using NodeID = int32_t;
    using Edge = EdgePair<NodeID, NodeID>;
    pvector<Edge> el;
    el.push_back(Edge(0, 2));
    el.push_back(Edge(1, 2));
    el.push_back(Edge(3, 2));
    el.push_back(Edge(2, 4));
    el.push_back(Edge(4, 1));
    CLBase cl(0, nullptr);
    BuilderBase<NodeID, NodeID, NodeID, /*invert=*/true> builder(cl);
    CSRGraph<NodeID> g = builder.MakeGraphFromEL(el);
    const uint32_t n = (uint32_t)g.num_nodes();
    printf("    graph: n=%u directed=%d\n", n, (int)g.directed());

    GraphCacheContext ctx;
    ctx.edge_epoch_count = n;   // ne = n -> epoch(next_nbr) == next_nbr
    ctx.buildOutEdgeMasks(g);

    // Oracle: for out-edge src->dest, soonest in-neigh(dest) > src (else wrap to the
    // smallest in-neigh of dest); epoch = that neighbour (since ne==n).
    // in_neigh(2)=[0,1,3], in_neigh(4)=[2], in_neigh(1)=[4].
    auto epoch_of = [&](uint32_t src, uint32_t dest) -> uint32_t {
        std::vector<uint32_t> innb;
        for (auto w : g.in_neigh(dest)) innb.push_back((uint32_t)w);
        std::sort(innb.begin(), innb.end());
        if (innb.empty()) return n - 1;
        for (uint32_t w : innb) if (w > src) return w;       // next this iteration
        return innb.front();                                  // wrap to next iteration
    };

    // Walk every out-edge and compare the stored epoch to the oracle.
    for (uint32_t src = 0; src < n; ++src) {
        std::vector<uint32_t> out;
        for (auto v : g.out_neigh(src)) out.push_back((uint32_t)v);
        const auto& eps = ctx.out_edge_epoch_by_src[src];
        for (size_t i = 0; i < out.size(); ++i) {
            uint32_t dest = out[i];
            char nm[64]; snprintf(nm, sizeof nm, "edge %u->%u epoch", src, dest);
            check(nm, eps[i], epoch_of(src, dest));
        }
    }

    // Spot-check representative cases by hand:
    //   0->2: in_neigh(2)=[0,1,3], next>0 = 1  -> epoch 1
    //   1->2: next>1 = 3                        -> epoch 3
    //   3->2: none>3 -> wrap to 0               -> epoch 0
    //   2->4: in_neigh(4)=[2], none>2 -> wrap 2 -> epoch 2
    //   4->1: in_neigh(1)=[4], none>4 -> wrap 4 -> epoch 4
    printf("    (hand oracle: 0->2=1, 1->2=3, 3->2=0, 2->4=2, 4->1=4)\n");

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
