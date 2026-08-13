// Copyright (c) 2024, UVA LavaLab
// BFS with Cache Simulation

#include <iostream>
#include <vector>
#include <fstream>

#include "benchmark.h"
#include "bitmap.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"
#include "sliding_queue.h"
#include "timer.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

// P-OPT rereference matrix builder
#include "graphbrew/partition/cagra/popt.h"

using namespace std;
using namespace cache_sim;

template<typename CacheType>
int64_t BUStep_Sim(const Graph &g, pvector<NodeID> &parent, Bitmap &front,
                   Bitmap &next, CacheType &cache,
                   GraphCacheContext &graph_ctx, const std::vector<uint32_t> &vertex_masks) {
    const bool ecg_record = GraphSimEcgEdgeRecord();
    const uint32_t edge_epochs =
        graph_ctx.edge_epoch_count ? graph_ctx.edge_epoch_count : 2u;
    int epoch_bits = 1;
    while (epoch_bits < 16 &&
           (uint32_t(1) << epoch_bits) < edge_epochs) {
        ++epoch_bits;
    }
    const auto ecg_meta = ::ecg_metadata::configure(
        static_cast<uint64_t>(g.num_nodes()), edge_epochs);
    const int record_bytes = ecg_meta.record_bytes;
    (void)record_bytes; (void)epoch_bits;
    ::ecg_metadata::announce(ecg_meta, "bfs");
    ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "bfs");
    const bool record_charged = ecg_record &&
        GraphSimEnvIntClamped(
            "ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    const bool stream_bypass =
        GraphSimEnvIntClamped("ECG_STREAM_BYPASS", 0, 0, 1) > 0;
    const auto in_edge_base = g.num_nodes() > 0
        ? g.in_neigh(0).begin() : nullptr;
    int64_t awake_count = 0;
    next.reset();
    #pragma omp parallel for reduction(+ : awake_count) schedule(dynamic, 1024)
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        // Clear any sticky per-edge epoch before the SEQUENTIAL parent[u] source read
        // so its fill isn't stamped with the previous u's stale neighbour epoch.
        graph_ctx.clearEdgeEpoch();
        // P-OPT: update current destination vertex
        SIM_SET_VERTEX(cache, u);
        // Track: read parent[u]
        SIM_CACHE_READ(cache, parent.data(), u);
        if (parent[u] < 0) {
            auto in_neigh = g.in_neigh(u);
            // Every policy tracks the real frontier-bitmap membership load. ECG
            // additionally carries the IN-edge metadata on that same demand so the
            // policy never changes the functional access stream.
            const bool use_in_edge_masks =
                graph_ctx.edgeMaskReady(EdgeMaskDir::IN, (uint32_t)u, (size_t)g.in_degree(u));
            size_t edge_pos = 0;
            for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it, ++edge_pos) {
                if (!ecg_record) SIM_CACHE_READ_EDGE(cache, it);
                else SIM_ECG_EDGE(cache, ecg_meta, it, in_edge_base,
                                  ::ecg_metadata::kInRecordBase,
                                  ::ecg_metadata::kInSidecarBase);
                NodeID v = *it;
                if (use_in_edge_masks) {
                    uint32_t m = graph_ctx.resolveEdgeMaskAndEpoch(
                        EdgeMaskDir::IN, (uint32_t)u, (size_t)g.in_degree(u), edge_pos, 0);
                    SIM_CACHE_READ_MASKED(cache, front.data(), (size_t)v / 64, graph_ctx, m);
                } else {
                    SIM_CACHE_READ(cache, front.data(), (size_t)v / 64);
                }
                // Track: check if v is in frontier
                if (front.get_bit(v)) {
                    // The parent[u] write targets the OUTER vertex u (not the masked
                    // dest v), so clear the frontier epoch first (don't stamp parent[u]
                    // with v's epoch).
                    graph_ctx.clearEdgeEpoch();
                    // Track: write parent[u]
                    SIM_CACHE_WRITE(cache, parent.data(), u);
                    parent[u] = v;
                    awake_count++;
                    next.set_bit(u);
                    break;
                }
            }
        }
    }
    return awake_count;
}

template<typename CacheType>
int64_t TDStep_Sim(const Graph &g, pvector<NodeID> &parent,
                   SlidingQueue<NodeID> &queue, CacheType &cache,
                   GraphCacheContext &graph_ctx, const std::vector<uint32_t> &vertex_masks,
                   int pfx_lookahead, int pfx_top_k = 1) {
    const bool ecg_record = GraphSimEcgEdgeRecord();
    const uint32_t edge_epochs =
        graph_ctx.edge_epoch_count ? graph_ctx.edge_epoch_count : 2u;
    int epoch_bits = 1;
    while (epoch_bits < 16 &&
           (uint32_t(1) << epoch_bits) < edge_epochs) {
        ++epoch_bits;
    }
    const auto ecg_meta = ::ecg_metadata::configure(
        static_cast<uint64_t>(g.num_nodes()), edge_epochs);
    const int record_bytes = ecg_meta.record_bytes;
    (void)record_bytes; (void)epoch_bits;
    ::ecg_metadata::announce(ecg_meta, "bfs");
    ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "bfs");
    const bool record_charged = ecg_record &&
        GraphSimEnvIntClamped(
            "ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    const bool stream_bypass =
        GraphSimEnvIntClamped("ECG_STREAM_BYPASS", 0, 0, 1) > 0;
    const auto out_edge_base = g.num_nodes() > 0
        ? g.out_neigh(0).begin() : nullptr;
    int64_t scout_count = 0;
    #pragma omp parallel
    {
        QueueBuffer<NodeID> lqueue(queue);
        #pragma omp for reduction(+ : scout_count)
        for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
            NodeID u = *q_iter;
            SIM_SET_VERTEX(cache, u);  // current frontier vertex = the traversal clock
            auto out_neigh = g.out_neigh(u);
            // ECG_BFS_EDGE_MASKS: consume the OUT-edge per-edge masks (the transpose-
            // correct dual-direction masks built for the out edge list) instead of the
            // per-vertex masks. The per-edge epoch is src-iteration-aware (next
            // in-neighbour of dest > u), which the single per-vertex mask cannot encode.
            const size_t u_outdeg = (size_t)g.out_degree(u);
            size_t edge_pos = 0;
            for (auto it = out_neigh.begin(); it != out_neigh.end(); ++it, ++edge_pos) {
                if (!ecg_record) SIM_CACHE_READ_EDGE(cache, it);
                else SIM_ECG_EDGE(cache, ecg_meta, it, out_edge_base,
                                  ::ecg_metadata::kOutRecordBase,
                                  ::ecg_metadata::kOutSidecarBase);
                NodeID v = *it;
                if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
                    if (graph_ctx.mask_config.prefetch_mode == 3) {
                        // DROPLET-style: prefetch every next-K out-neighbor
                        // sequentially (no target selection).
                        auto jt = it;
                        for (int step = 0; step < pfx_lookahead; step++) {
                            ++jt;
                            if (jt == out_neigh.end()) break;
                            NodeID candidate = *jt;
                            if (candidate < 0) continue;
                            SIM_CACHE_PREFETCH_VERTEX(cache, parent.data(),
                                static_cast<uint32_t>(candidate), graph_ctx);
                        }
                    } else {
                        // Top-K POPT/degree-ranked selection.
                        struct Cand { uint32_t v; uint16_t key; };
                        Cand cands[64];
                        int n_cand = 0;
                        auto jt = it;
                        for (int step = 0; step < pfx_lookahead; step++) {
                            ++jt;
                            if (jt == out_neigh.end()) break;
                            NodeID candidate = *jt;
                            if (candidate < 0) continue;
                            uint16_t key;
                            if (graph_ctx.mask_config.prefetch_mode == 1) {
                                uint64_t od = g.out_degree(candidate);
                                key = od > 65535 ? 0 : static_cast<uint16_t>(65535 - od);
                            } else {
                                key = graph_ctx.mask_config.decodePOPT(vertex_masks[candidate]);
                            }
                            cands[n_cand++] = {static_cast<uint32_t>(candidate), key};
                        }
                        if (n_cand == 0) {
                            graph_ctx.recordPrefetchNoTarget();
                        } else if (pfx_top_k <= 1) {
                            int best = 0;
                            for (int i = 1; i < n_cand; i++)
                                if (cands[i].key < cands[best].key) best = i;
                            SIM_CACHE_PREFETCH_VERTEX(cache, parent.data(), cands[best].v, graph_ctx);
                        } else {
                            int k_eff = pfx_top_k < n_cand ? pfx_top_k : n_cand;
                            for (int i = 0; i < k_eff; i++) {
                                int best = i;
                                for (int j = i + 1; j < n_cand; j++)
                                    if (cands[j].key < cands[best].key) best = j;
                                if (best != i) std::swap(cands[i], cands[best]);
                                SIM_CACHE_PREFETCH_VERTEX(cache, parent.data(), cands[i].v, graph_ctx);
                            }
                        }
                    }
                }
                // Track: read parent[v] with this edge's OUT mask (transpose-correct,
                // src-aware epoch + POPT) or the per-vertex fallback.
                uint32_t pmask = graph_ctx.resolveEdgeMaskAndEpoch(
                    EdgeMaskDir::OUT, (uint32_t)u, u_outdeg, edge_pos, vertex_masks[v]);
                SIM_CACHE_READ_MASKED(cache, parent.data(), v, graph_ctx, pmask);
                NodeID curr_val = parent[v];
                if (curr_val < 0) {
                    // Track: write parent[v]
                    SIM_CACHE_WRITE(cache, parent.data(), v);
                    if (compare_and_swap(parent[v], curr_val, u)) {
                        lqueue.push_back(v);
                        scout_count += -curr_val;
                    }
                }
            }
        }
        lqueue.flush();
    }
    return scout_count;
}

void QueueToBitmap(const SlidingQueue<NodeID> &queue, Bitmap &bm) {
    #pragma omp parallel for
    for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
        NodeID u = *q_iter;
        bm.set_bit_atomic(u);
    }
}

void BitmapToQueue(const Graph &g, const Bitmap &bm,
                   SlidingQueue<NodeID> &queue) {
    #pragma omp parallel
    {
        QueueBuffer<NodeID> lqueue(queue);
        #pragma omp for
        for (NodeID n = 0; n < g.num_nodes(); n++)
            if (bm.get_bit(n))
                lqueue.push_back(n);
        lqueue.flush();
    }
    queue.slide_window();
}

pvector<NodeID> InitParent(const Graph &g) {
    pvector<NodeID> parent(
        g.num_nodes(), NodeID(0), GRAPH_SIM_PROPERTY_ALIGNMENT);
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        parent[n] = g.out_degree(n) != 0 ? -g.out_degree(n) : -1;
    return parent;
}

template<typename CacheType>
pvector<NodeID> DOBFS_Sim(const Graph &g, NodeID source, CacheType &cache,
                          int alpha = 15, int beta = 18) {
    pvector<NodeID> parent = InitParent(g);
    parent[source] = source;

    // --- Graph-aware cache context ---
    GraphCacheContext graph_ctx;
    pvector<uint32_t> deg_arr(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        deg_arr[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(deg_arr.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());
    size_t llc_size = 8 * 1024 * 1024;
    llc_size = GetEnvSizeBytes("CACHE_L3_SIZE", llc_size);
    graph_ctx.registerPropertyArray(parent.data(), g.num_nodes(), sizeof(NodeID), llc_size);
    cache.initGraphContext(&graph_ctx);

    // Build P-OPT rereference matrix before masks so POPT-ranked PFX can use it.
    static pvector<uint8_t> popt_matrix;       // TD-phase matrix (registered)
    static pvector<uint8_t> popt_matrix_bu;    // BU-phase matrix (POPT_DUAL_REREF only)
    // POPT_DUAL_REREF: real-time per-phase matrix load. reref_td/reref_bu point at the
    // two pre-built matrices; the phase loop swaps the active one into the single
    // reserved reref way (no 2nd way). Default off (reref_bu stays null -> no swap).
    const bool popt_dual_reref = std::getenv("POPT_DUAL_REREF") != nullptr;
    const uint8_t* reref_td = nullptr;
    const uint8_t* reref_bu = nullptr;
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        const char* pfx_env = getenv("ECG_PREFETCH_MODE");
        bool popt_prefetch = pfx_env && atoi(pfx_env) == 2;
        const bool matrix_free_k2 = GraphSimMatrixFreeK2();
        if (policy == EvictionPolicy::POPT ||
            (policy == EvictionPolicy::ECG && !matrix_free_k2) ||
            (popt_prefetch && !matrix_free_k2)) {
            constexpr int numVtxPerLine = 64 / sizeof(NodeID);
            constexpr int numEpochs = 256;
            // BFS is direction-optimizing, but its ONLY masked property read is the
            // TD (push) parent[v] over out_neigh(u); BU (pull) uses a frontier bitmap
            // (no masked read). The next reader of parent[v] is in_neigh(v), so the
            // transpose-correct rereference direction is IN/CSC (traverseCSR=false).
            // ECG_BFS_FORCE_OUT reverts to CSR for direction-transfer experiments;
            // ECG_EXACT_BFS instead uses its own visit-order skeleton clock (below).
            // On the symmetric evaluation corpus in==out, so this is inert; it
            // is the correct default for directed graphs.
            bool bfs_natural_csr = std::getenv("ECG_BFS_FORCE_OUT") != nullptr;
            reref_td = buildAndRegisterReref(g, graph_ctx, bfs_natural_csr,
                          "BFS(TD/out->in-transpose)", numVtxPerLine, numEpochs, popt_matrix);
            // POPT_DUAL_REREF: pre-build the BU-phase matrix (the transpose of BU's
            // in_neigh traversal = the OPPOSITE direction of TD) so the phase loop can
            // real-time-load the transpose-correct matrix into the single reserved way
            // per phase. On the symmetric corpus
            // CSR==CSC so the swap is inert (byte-identical), and for BFS specifically
            // BU-parent is SEQUENTIAL (parent[u], u in ID order), so this does NOT make
            // parent's P-OPT management more correct (the dual matters for the ECG edge
            // masks, which target BU's IRREGULAR frontier probe, not for the P-OPT reref
            // which manages the sequential parent). The mechanism is here for a future
            // direction-optimizing kernel with irregular property access in
            // both directions.
            if (popt_dual_reref) {
                reref_bu = buildRerefMatrix(g, /*natural_csr=*/!bfs_natural_csr,
                              "BFS(BU/in->out-transpose)", numVtxPerLine, numEpochs, popt_matrix_bu);
                std::cout << "BFS: POPT_DUAL_REREF enabled (TD + BU matrices pre-built; "
                             "real-time per-phase load into the single reserved way)" << std::endl;
            }
            if (std::getenv("ECG_EXACT_REREF")) {
                const char* eb = std::getenv("ECG_EXACT_BITS");
                if (eb) graph_ctx.exact_bits = (uint32_t)atoi(eb);
                if (std::getenv("ECG_EXACT_BFS")) {
                    // source-specific: BFS skeleton from the kernel's own source —
                    // UNLESS ECG_BFS_MASK_SRC overrides it (source-TRANSFER test: build
                    // the mask from a DIFFERENT source than the kernel runs).
                    uint32_t mask_src = (uint32_t)source;
                    if (const char* ms = std::getenv("ECG_BFS_MASK_SRC"))
                        mask_src = (uint32_t)atoi(ms);
                    if (std::getenv("ECG_BFS_HUBSRC")) {
                        // canonical deterministic source-independent choice: the highest
                        // out-degree hub (most central -> most representative BFS layering).
                        uint32_t best = 0; uint64_t bd = 0;
                        for (uint32_t v = 0; v < (uint32_t)g.num_nodes(); ++v)
                            if (g.out_degree(v) > bd) { bd = g.out_degree(v); best = v; }
                        mask_src = best;
                    }
                    if (std::getenv("ECG_BFS_KSOURCE")) {
                        // K-source EXPECTED-REUSE consensus clock (source-independent).
                        uint32_t K = 8, seed = 12345;
                        if (const char* s = std::getenv("ECG_BFS_K")) K = (uint32_t)atoi(s);
                        if (const char* s = std::getenv("ECG_BFS_KSEED")) seed = (uint32_t)atoi(s);
                        graph_ctx.buildBFSVisitOrderKSource(g, K, seed);
                    } else if (std::getenv("ECG_BFS_BOUNDED")) {
                        // SOURCE-INDEPENDENT depth-bounded degree-seeded clustering clock.
                        uint32_t d = 8;
                        if (const char* s = std::getenv("ECG_BFS_BOUND_DEPTH")) d = (uint32_t)atoi(s);
                        if (std::getenv("ECG_BFS_COMMUNITY"))
                            graph_ctx.buildBoundedBFSOrderCommunity(g, d);
                        else
                            graph_ctx.buildBoundedBFSOrder(g, d);
                    } else if (std::getenv("ECG_BFS_DEPTHORDER")) {
                        graph_ctx.buildBFSVisitOrderByDepth(g, mask_src);
                    } else {
                        graph_ctx.buildBFSVisitOrder(g, mask_src);
                    }
                    graph_ctx.registerInAdjacencyExactBFS(g);
                } else if (std::getenv("ECG_EXACT_IN")) {
                    // SOURCE-INDEPENDENT (the RCM variation): in-adjacency mask with
                    // ID-order clock. Built ONCE, no per-source BFS. On an RCM-reordered
                    // graph ID-order ~ BFS-frontier-order for any source, so this
                    // approximates the per-source BFS mask without knowing the source.
                    graph_ctx.visit_pos.resize(g.num_nodes());
                    for (uint32_t v = 0; v < (uint32_t)g.num_nodes(); ++v)
                        graph_ctx.visit_pos[v] = v;
                    graph_ctx.registerInAdjacencyExactBFS(g);
                } else {
                    graph_ctx.registerOutAdjacencyExact(g);  // ECG_EXACT mode (sweep flavor)
                }
            }
        }
    }

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());
    // ECG_EDGE_MASKS (generic, single matrix knob) or ECG_BFS_EDGE_MASKS (alias):
    // build the dual-direction per-edge masks so BOTH BFS phases carry a src-aware,
    // transpose-correct epoch per edge instead of the single per-vertex value. TD
    // (push) traverses out_neigh(u) reading parent[v] -> OUT-edge masks (epoch from
    // in_neigh(v)); BU (pull) traverses in_neigh(u) probing the frontier bit of v ->
    // IN-edge masks (epoch from out_neigh(v)). Single OR-gate (no double build).
    // Inert on symmetric graphs (in==out); the correct dual mask for directed graphs.
    if (std::getenv("ECG_EDGE_MASKS") || std::getenv("ECG_BFS_EDGE_MASKS")) {
        graph_ctx.buildOutEdgeMasks(g);     // TD push: parent[v] over out_neigh(u)
        graph_ctx.buildInEdgeMasks(g);      // BU pull: frontier bit of v over in_neigh(u)
        cout << "BFS: dual-direction per-edge masks enabled (TD=OUT-edge, BU=IN-edge)" << endl;
    }
    int pfx_lookahead = GraphSimEnvIntClamped("ECG_PREFETCH_LOOKAHEAD", 0, 0, 64);
    int pfx_top_k = GraphSimEnvIntClamped("ECG_PREFETCH_TOP_K", 1, 1, 64);
    if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
        cout << "BFS TD PFX lookahead: window=" << pfx_lookahead
             << " mode=" << int(graph_ctx.mask_config.prefetch_mode)
             << " top_k=" << pfx_top_k << endl;
    }

    // Match gem5/Sniper ROI state: model the initialized parent[] stores before
    // the ROI, retain the warmed cache contents, and reset only statistics.
    graph_ctx.clearEdgeEpoch();
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); ++n)
        SIM_CACHE_WRITE(cache, parent.data(), n);
    cache.resetStats();

    SlidingQueue<NodeID> queue(g.num_nodes());
    queue.push_back(source);
    queue.slide_window();
    Bitmap curr(g.num_nodes());
    curr.reset();
    Bitmap front(g.num_nodes());
    front.reset();
    int64_t edges_to_check = g.num_edges_directed();
    int64_t scout_count = g.out_degree(source);
    // ECG_BFS_FORCE_TD: stay top-down (skip the bottom-up phase) so the BFS-order
    // EXACT generator (which models the top-down access pattern) can be validated
    // against the actual access order. Direction-optimizing BU needs its own model.
    static const bool force_td = std::getenv("ECG_BFS_FORCE_TD") != nullptr;

    while (!queue.empty()) {
        if (!force_td && scout_count > edges_to_check / alpha) {
            int64_t awake_count, old_awake_count;
            QueueToBitmap(queue, front);
            awake_count = queue.size();
            queue.slide_window();
            // POPT_DUAL_REREF: load the BU-phase (out-transpose) matrix into the single
            // reserved reref way for the bottom-up phase (no-op when the flag is off).
            if (reref_bu) graph_ctx.setActiveRerefMatrix(reref_bu);
            do {
                old_awake_count = awake_count;
                awake_count = BUStep_Sim(g, parent, front, curr, cache, graph_ctx, vertex_masks);
                front.swap(curr);
            } while ((awake_count >= old_awake_count) ||
                     (awake_count > g.num_nodes() / beta));
            BitmapToQueue(g, front, queue);
            scout_count = 1;
        } else {
            edges_to_check -= scout_count;
            // POPT_DUAL_REREF: restore the TD-phase (in-transpose) matrix for the
            // top-down phase (no-op when the flag is off).
            if (reref_bu) graph_ctx.setActiveRerefMatrix(reref_td);
            scout_count = TDStep_Sim(g, parent, queue, cache, graph_ctx, vertex_masks,
                                     pfx_lookahead, pfx_top_k);
            queue.slide_window();
        }
    }
    if (popt_dual_reref)
        std::cout << "BFS: POPT_DUAL_REREF real-time per-direction loads this run = "
                  << graph_ctx.reref_swap_count << std::endl;
    // Finalize the parent array to the GAPBS BFS contract: unreached vertices still
    // carry InitParent's -out_degree(n) encoding (< -1); canonically they must be -1
    // (parent[x] < 0 => unvisited). Without this the BFSVerifier's depth[u]==parent[u]
    // reachability check FAILs on any graph with unreached vertices. This is plain
    // post-processing (no cache accesses), so it leaves the cache stats byte-identical
    // and only corrects the returned tree. Matches canonical DOBFS (bench/src/bfs.cc).
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        if (parent[n] < -1)
            parent[n] = -1;
    return parent;
}

void PrintBFSStats(const Graph &g, const pvector<NodeID> &bfs_tree) {
    int64_t tree_size = 0;
    int64_t n_edges = 0;
    for (NodeID n : g.vertices()) {
        if (bfs_tree[n] >= 0) {
            n_edges += g.out_degree(n);
            tree_size++;
        }
    }
    cout << "BFS Tree has " << static_cast<long long>(tree_size) << " nodes and ";
    cout << static_cast<long long>(n_edges) << " edges" << endl;
}

bool BFSVerifier(const Graph &g, NodeID source,
                 const pvector<NodeID> &parent) {
    pvector<int> depth(g.num_nodes(), -1);
    depth[source] = 0;
    vector<NodeID> to_visit;
    to_visit.reserve(g.num_nodes());
    to_visit.push_back(source);
    for (auto it = to_visit.begin(); it != to_visit.end(); it++) {
        NodeID u = *it;
        for (NodeID v : g.out_neigh(u)) {
            if (depth[v] == -1) {
                depth[v] = depth[u] + 1;
                to_visit.push_back(v);
            }
        }
    }
    for (NodeID u : g.vertices()) {
        if ((depth[u] != -1) && (parent[u] != -1)) {
            if (u == source) {
                if (parent[u] != u) return false;
            } else {
                bool found = false;
                for (NodeID v : g.in_neigh(u)) {
                    if (parent[u] == v) {
                        if (depth[v] != depth[u] - 1) return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        } else if (depth[u] != parent[u]) {
            return false;
        }
    }
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "bfs-sim");
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        SourcePicker<Graph> sp(g, cli.start_vertex(), cli.num_trials());
        auto BFSBound = [&sp, &cache](const Graph &g) {
            return DOBFS_Sim(g, sp.PickNext(), cache);
        };
        SourcePicker<Graph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const Graph &g, const pvector<NodeID> &parent) {
            return BFSVerifier(g, vsp.PickNext(), parent);
        };
        
        BenchmarkKernel(cli, g, BFSBound, PrintBFSStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else if (fast) {
        // FAST single-core cache simulation (no locks, ~10x faster)
        FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
        
        SourcePicker<Graph> sp(g, cli.start_vertex(), cli.num_trials());
        auto BFSBound = [&sp, &cache](const Graph &g) {
            return DOBFS_Sim(g, sp.PickNext(), cache);
        };
        SourcePicker<Graph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const Graph &g, const pvector<NodeID> &parent) {
            return BFSVerifier(g, vsp.PickNext(), parent);
        };
        
        BenchmarkKernel(cli, g, BFSBound, PrintBFSStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else {
        CacheHierarchy cache = CacheHierarchy::fromEnvironment();
        
        SourcePicker<Graph> sp(g, cli.start_vertex(), cli.num_trials());
        auto BFSBound = [&sp, &cache](const Graph &g) {
            return DOBFS_Sim(g, sp.PickNext(), cache);
        };
        SourcePicker<Graph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const Graph &g, const pvector<NodeID> &parent) {
            return BFSVerifier(g, vsp.PickNext(), parent);
        };
        
        BenchmarkKernel(cli, g, BFSBound, PrintBFSStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    }
    
    return 0;
}
