// Directed-graph oracle test for buildInEdgeMasks (the IN-edge mirror of
// buildOutEdgeMasks, used by BFS bottom-up). Symmetric graphs cannot validate
// direction (in==out), so this uses a tiny DIRECTED graph with hand-computed
// expected epochs.
//
// buildInEdgeMasks fills, for each in-edge src<-dest (dest in g.in_neigh(src)),
// the absolute next-ref EPOCH of dest's frontier bit, computed from dest's
// OUT-neighbours (the transpose of IN traversal): the soonest out-neighbour of
// dest strictly > src (wrapping to the next iteration), mapped to
// epoch = next_nbr * ne / n. We assert the decoded epochs match an independent
// oracle. This is the BU pull mirror of TD push (test_ecg_out_edge_mask.cc).
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
    printf("[test_ecg_in_edge_mask] buildInEdgeMasks directed oracle\n");

    // Directed graph (NOT symmetrized). Edges chosen so out_neigh != in_neigh and
    // a single dest is reached from several srcs (exercises the src-aware epoch):
    //   2->1  2->3  2->4   (out_neigh(2) = [1,3,4]; 2 in_neigh of 1,3,4)
    //   0->3              (out_neigh(0) = [3];     0 in_neigh of 3)
    //   3->0              (out_neigh(3) = [0];     3 in_neigh of 0)
    // n=5. ne=5 so epoch(v) = v (next_nbr*5/5).
    using NodeID = int32_t;
    using Edge = EdgePair<NodeID, NodeID>;
    pvector<Edge> el;
    el.push_back(Edge(2, 1));
    el.push_back(Edge(2, 3));
    el.push_back(Edge(2, 4));
    el.push_back(Edge(0, 3));
    el.push_back(Edge(3, 0));
    CLBase cl(0, nullptr);
    BuilderBase<NodeID, NodeID, NodeID, /*invert=*/true> builder(cl);
    CSRGraph<NodeID> g = builder.MakeGraphFromEL(el);
    const uint32_t n = (uint32_t)g.num_nodes();
    printf("    graph: n=%u directed=%d\n", n, (int)g.directed());

    GraphCacheContext ctx;
    ctx.edge_epoch_count = n;   // ne = n -> epoch(next_nbr) == next_nbr
    ctx.buildInEdgeMasks(g);

    // Oracle: for in-edge src<-dest, soonest out-neigh(dest) > src (else wrap to the
    // smallest out-neigh of dest); epoch = that neighbour (since ne==n).
    auto epoch_of = [&](uint32_t src, uint32_t dest) -> uint32_t {
        std::vector<uint32_t> outnb;
        for (auto w : g.out_neigh(dest)) outnb.push_back((uint32_t)w);
        std::sort(outnb.begin(), outnb.end());
        if (outnb.empty()) return n - 1;
        for (uint32_t w : outnb) if (w > src) return w;      // next this iteration
        return outnb.front();                                 // wrap to next iteration
    };

    // Walk every in-edge and compare the stored epoch to the oracle.
    for (uint32_t src = 0; src < n; ++src) {
        std::vector<uint32_t> in;
        for (auto v : g.in_neigh(src)) in.push_back((uint32_t)v);
        const auto& eps = ctx.in_edge_epoch_by_src[src];
        for (size_t i = 0; i < in.size(); ++i) {
            uint32_t dest = in[i];
            char nm[64]; snprintf(nm, sizeof nm, "edge %u<-%u epoch", src, dest);
            check(nm, eps[i], epoch_of(src, dest));
        }
    }

    // Spot-check representative cases by hand (dest=2 gives 3 different epochs across
    // src=1,3,4 -> proves the epoch is src-aware, not a per-vertex constant):
    //   1<-2: out_neigh(2)=[1,3,4], next>1 = 3        -> epoch 3
    //   3<-2: next>3 = 4                              -> epoch 4
    //   4<-2: none>4 -> wrap to 1                     -> epoch 1
    //   0<-3: out_neigh(3)=[0], none>0 -> wrap 0      -> epoch 0
    //   3<-0: out_neigh(0)=[3], none>3 -> wrap 3      -> epoch 3
    printf("    (hand oracle: 1<-2=3, 3<-2=4, 4<-2=1, 0<-3=0, 3<-0=3)\n");

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
