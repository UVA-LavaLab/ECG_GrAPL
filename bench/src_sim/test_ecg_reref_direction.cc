// ECG reref-matrix DIRECTION oracle (Phase A / A3).
//
// The P-OPT/ECG next-ref reref matrix must be the graph TRANSPOSE of a kernel's property-
// access edge list (makeOffsetMatrix traverseCSR: true=out_neigh, false=in_neigh). PR's
// in-pull wants out_neigh (true); BFS-top-down / SSSP / BC traverse OUT-edges and want the
// transpose (false). This pins that makeOffsetMatrix actually RESPECTS the direction flag,
// so a kernel that passes the wrong direction (e.g. the former gem5/Sniper BFS/SSSP default
// true) produces a provably different matrix.
//
// Mutation-proof: if makeOffsetMatrix ignored traverseCSR (always out_neigh), then
// false==true (test 1 fails) AND false != transpose-out (test 2 fails).
#include "external/gapbs/graph.h"
#include "external/gapbs/builder.h"
#include "external/gapbs/command_line.h"
#include "graphbrew/partition/cagra/popt.h"

#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(const char* name, bool ok) {
    printf("    %-62s [%s]\n", name, ok ? "OK" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}
static bool same(const pvector<uint8_t>& a, const pvector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) return false;
    return true;
}

int main() {
    printf("[test_ecg_reref_direction] makeOffsetMatrix transpose-direction oracle\n");
    using NodeID = int32_t;
    using Edge = EdgePair<NodeID, NodeID>;

    // Directed graph with in_neigh != out_neigh (e.g. vertex 0: out={5,6}, in={7}).
    pvector<Edge> el, el_rev, el_sym;
    auto add = [&](NodeID u, NodeID v) {
        el.push_back(Edge(u, v));
        el_rev.push_back(Edge(v, u));               // transpose
        el_sym.push_back(Edge(u, v)); el_sym.push_back(Edge(v, u));  // symmetric
    };
    add(0, 5); add(0, 6); add(1, 5); add(7, 0); add(2, 6); add(6, 3);

    CLBase cl(0, nullptr);
    auto build = [&](pvector<Edge>& e) {
        BuilderBase<NodeID, NodeID, NodeID, /*invert=*/true> b(cl);
        return b.MakeGraphFromEL(e);
    };
    CSRGraph<NodeID> g  = build(el);
    CSRGraph<NodeID> gT = build(el_rev);   // transpose graph: gT.out_neigh == g.in_neigh
    CSRGraph<NodeID> gS = build(el_sym);   // symmetric (undirected)

    const int nvpl = 1;
    const int ne = 256;   // makeOffsetMatrix asserts numEpochs==256; epochSz=ceil(n/256)=1
                          // for this tiny graph, so epoch(v)=v and the direction logic is
                          // unchanged — but this stays valid under assert-enabled builds too.
    printf("    g: n=%d directed=%d  (out=true / in=false)\n", ne, (int)g.directed());

    pvector<uint8_t> m_out, m_in, mT_out, mS_out, mS_in;
    makeOffsetMatrix(g,  m_out,  nvpl, ne, /*traverseCSR=*/true);
    makeOffsetMatrix(g,  m_in,   nvpl, ne, /*traverseCSR=*/false);
    makeOffsetMatrix(gT, mT_out, nvpl, ne, /*traverseCSR=*/true);
    makeOffsetMatrix(gS, mS_out, nvpl, ne, /*traverseCSR=*/true);
    makeOffsetMatrix(gS, mS_in,  nvpl, ne, /*traverseCSR=*/false);

    // (1) On a DIRECTED graph the flag is load-bearing: out_neigh != in_neigh.
    check("directed: makeOffsetMatrix(true) != makeOffsetMatrix(false)", !same(m_out, m_in));
    // (2) TRANSPOSE certification: false (in_neigh) == transpose's out_neigh. This is the
    //     exact relation BFS/SSSP/BC rely on (their reref must be the transpose).
    check("makeOffsetMatrix(g,false) == makeOffsetMatrix(transpose(g),true)", same(m_in, mT_out));
    // (3) On an UNDIRECTED graph direction is irrelevant (out==in; makeOffsetMatrix forces true).
    check("undirected: makeOffsetMatrix(true) == makeOffsetMatrix(false)", same(mS_out, mS_in));

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
