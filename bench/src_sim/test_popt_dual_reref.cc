// Mechanism test for POPT_DUAL_REREF (real-time per-direction reref load). The dual
// reref is inert on the symmetric eval corpus (CSR==CSC) and forward-looking for a
// future direction-optimizing kernel with irregular property access in both
// directions (BFS-parent is sequential, so BFS does not benefit). This test runs on a directed
// graph (in_neigh != out_neigh) to prove the two matrices genuinely differ and that
// setActiveRerefMatrix repoints the single reserved reref way.
#include "cache_sim/graph_cache_context.h"
#include "graphbrew/partition/cagra/popt.h"
#include "external/gapbs/graph.h"
#include "external/gapbs/builder.h"
#include "external/gapbs/command_line.h"
#include <cstdio>
#include <cstring>

using namespace cache_sim;

static int g_pass = 0, g_fail = 0;
static void check(const char* name, bool ok) {
    printf("    %-50s [%s]\n", name, ok ? "OK" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

int main() {
    printf("[test_popt_dual_reref] dual reref build + real-time swap mechanism\n");

    // Directed graph (NOT symmetrized): in_neigh != out_neigh so CSR != CSC.
    using NodeID = int32_t;
    using Edge = EdgePair<NodeID, NodeID>;
    pvector<Edge> el;
    el.push_back(Edge(0, 2)); el.push_back(Edge(1, 2)); el.push_back(Edge(3, 2));
    el.push_back(Edge(2, 4)); el.push_back(Edge(4, 1)); el.push_back(Edge(0, 1));
    el.push_back(Edge(3, 4)); el.push_back(Edge(4, 0));
    CLBase cl(0, nullptr);
    BuilderBase<NodeID, NodeID, NodeID, /*invert=*/true> builder(cl);
    CSRGraph<NodeID> g = builder.MakeGraphFromEL(el);
    printf("    graph: n=%d directed=%d\n", (int)g.num_nodes(), (int)g.directed());

    const int vpl = 1, ne = 256;  // vpl=1 -> one line per vertex, maximal direction sensitivity
    pvector<uint8_t> csc_buf, csr_buf;
    const uint8_t* csc = buildRerefMatrix(g, /*natural_csr=*/false, "TEST-CSC(in)", vpl, ne, csc_buf);
    const uint8_t* csr = buildRerefMatrix(g, /*natural_csr=*/true,  "TEST-CSR(out)", vpl, ne, csr_buf);

    // On a directed graph the IN-transpose (CSC) and OUT-transpose (CSR) matrices must
    // genuinely differ — otherwise the per-phase swap would be a no-op everywhere.
    bool differ = (csc_buf.size() == csr_buf.size()) && csc_buf.size() > 0 &&
                  std::memcmp(csc, csr, csc_buf.size()) != 0;
    check("CSC (in) and CSR (out) matrices differ on directed graph", differ);

    // setActiveRerefMatrix repoints the single reserved reref way.
    GraphCacheContext ctx;
    int ncl = (g.num_nodes() + vpl - 1) / vpl;
    ctx.initRereference(csc, ncl, ne, g.num_nodes(), 64);
    check("initRereference activates CSC", ctx.rereference.matrix == csc);
    ctx.setActiveRerefMatrix(csr);
    check("setActiveRerefMatrix swaps active to CSR", ctx.rereference.matrix == csr);
    ctx.setActiveRerefMatrix(csc);
    check("swap back to CSC", ctx.rereference.matrix == csc);

    // The swap must not disturb the (shared) dims — same graph, so dims are identical.
    check("dims unchanged across swap (num_cache_lines)",
          ctx.rereference.num_cache_lines == (uint32_t)ncl);

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
