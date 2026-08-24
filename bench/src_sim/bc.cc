// Copyright (c) 2024, UVA LavaLab
// Betweenness Centrality with Cache Simulation

#include <iostream>
#include <fstream>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"
#include "sliding_queue.h"
#include "timer.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

#include "graphbrew/partition/cagra/popt.h"

using namespace std;
using namespace cache_sim;

typedef float ScoreT;

template<typename CacheType>
void BCBFS_Sim(const Graph &g, NodeID source, 
               pvector<ScoreT> &scores, CacheType &cache) {
    pvector<int32_t> depths(
        g.num_nodes(), -1, GRAPH_SIM_PROPERTY_ALIGNMENT);
    depths[source] = 0;
    
    pvector<NodeID> succ;
    succ.reserve(g.num_edges_directed());
    
    pvector<int64_t> succ_start(g.num_nodes() + 1, 0);
    pvector<int64_t> path_counts(
        g.num_nodes(), int64_t(0), GRAPH_SIM_PROPERTY_ALIGNMENT);
    path_counts[source] = 1;
    pvector<ScoreT> deltas(
        g.num_nodes(), ScoreT(0), GRAPH_SIM_PROPERTY_ALIGNMENT);

    // --- Graph-aware cache context ---
    // Upstream GRASP's default BC instrumentation protects backward
    // dependencies (propertyA). Other arrays remain property data for P-OPT/ECG.
    GraphCacheContext graph_ctx;
    pvector<uint32_t> deg_arr(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        deg_arr[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(deg_arr.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());
    size_t llc_size = 8 * 1024 * 1024;
    llc_size = GetEnvSizeBytes("CACHE_L3_SIZE", llc_size);
    // GRASP protects vertex-indexed property arrays. BC has four
    // such arrays (all indexed by vertex id), so we mark all of them as
    // grasp_region=true: classifyGRASP() applies the same hot/moderate
    // boundary inside each region.  Marking only one of four arrays as a
    // GRASP region (the original behaviour) caused the other three to
    // thrash under SRRIP while the one protected array hogged the LLC.
    graph_ctx.registerPropertyArray(depths.data(), g.num_nodes(), sizeof(int32_t), llc_size, -1.0, true);
    graph_ctx.registerPropertyArray(path_counts.data(), g.num_nodes(), sizeof(int64_t), llc_size, -1.0, true);
    graph_ctx.registerPropertyArray(scores.data(), g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    graph_ctx.registerPropertyArray(deltas.data(), g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    cache.initGraphContext(&graph_ctx);

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    cache.initGraphContext(&graph_ctx);
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());
    // ECG_EDGE_MASKS (generic, matrix knob) / ECG_BC_EDGE_MASKS (alias): build the
    // OUT-edge per-edge masks so the forward (top-down) phase carries the src-aware,
    // transpose-correct epoch (next in-neighbour of dest > u) per edge instead of the
    // per-vertex value. Mirror of BFS-TD. The backward phase keeps plain per-vertex
    // accesses (succ DAG is not a static edge list). Built per source (matches the
    // per-source makeOffsetMatrix below); inert on the symmetric corpus.
    if (std::getenv("ECG_EDGE_MASKS") || std::getenv("ECG_BC_EDGE_MASKS")) {
        graph_ctx.buildOutEdgeMasks(g);
        static bool logged_bc_em = false;  // BCBFS runs per source; log once
        if (!logged_bc_em) {
            logged_bc_em = true;
            cout << "BC: OUT-edge per-edge masks enabled (forward push/out)" << endl;
        }
    }
    const bool ecg_record = GraphSimEcgEdgeRecord();
    const bool record_charged = ecg_record &&
        GraphSimEnvIntClamped("ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    const bool flowthrough =
        GraphSimEnvIntClamped("ECG_FLOWTHROUGH", 0, 0, 1) > 0;
    int epoch_bits = 1;
    const uint32_t edge_epochs =
        graph_ctx.edge_epoch_count ? graph_ctx.edge_epoch_count : 2;
    while (epoch_bits < 16 &&
           (uint32_t(1) << epoch_bits) < edge_epochs) {
        ++epoch_bits;
    }
    const auto ecg_meta = ::ecg_metadata::configure(
        static_cast<uint64_t>(g.num_nodes()), edge_epochs);
    const int record_bytes = ecg_meta.record_bytes;
    (void)record_bytes; (void)epoch_bits;
    ::ecg_metadata::announce(ecg_meta, "bc");
    ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "bc");
    NodeID* out_edge_base = g.num_nodes() > 0
        ? g.out_neigh(0).begin() : nullptr;

    // Build P-OPT rereference matrix (for POPT and ECG policies)
    static pvector<uint8_t> popt_matrix;
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        if (policy == EvictionPolicy::POPT ||
            policy == EvictionPolicy::ECG) {
            constexpr int numVtxPerLine = 64 / sizeof(int32_t);
            constexpr int numEpochs = 256;
            // BC forward phase is top-down BFS over out_neigh(u) reading depths[v]/
            // path_counts[v]; next-ref is in_neigh(v) => transpose = CSC/in_neigh.
            // (NB the per-vertex mask above is built before this matrix, so its POPT
            // field is degree-fallback — harmless: BC eviction uses the matrix/epoch,
            // the per-vertex POPT field is vestigial.)
            buildAndRegisterReref(g, graph_ctx, /*natural_csr=*/false, "BC(push/out)",
                                  numVtxPerLine, numEpochs, popt_matrix);
            if (std::getenv("ECG_EXACT_REREF")) {
                const char* eb = std::getenv("ECG_EXACT_BITS");
                if (eb) graph_ctx.exact_bits = (uint32_t)atoi(eb);
                if (std::getenv("ECG_EXACT_BFS")) {
                    // bc forward phase is pure top-down BFS -> in-adjacency visit-order.
                    graph_ctx.buildBFSVisitOrder(g, (uint32_t)source);
                    graph_ctx.registerInAdjacencyExactBFS(g);
                } else {
                    graph_ctx.registerOutAdjacencyExact(g);  // ECG_EXACT mode (sweep flavor)
                }
            }
        }
    }

    vector<SlidingQueue<NodeID>::iterator> depth_index;
    SlidingQueue<NodeID> queue(g.num_nodes());
    queue.push_back(source);
    queue.slide_window();
    depth_index.push_back(queue.begin());
    
    int32_t depth = 0;
    
    // BFS forward phase
    while (!queue.empty()) {
        depth++;
        depth_index.push_back(queue.begin());
        
        #pragma omp parallel for schedule(dynamic, 64)
        for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
            NodeID u = *q_iter;
            // Clear the sticky per-edge epoch before the SEQUENTIAL depths[u] source
            // read so its fill isn't stamped with the previous u's stale edge epoch.
            graph_ctx.clearEdgeEpoch();
            // P-OPT: update current destination vertex
            SIM_SET_VERTEX(cache, u);
            // Track depth read
            SIM_CACHE_READ(cache, depths.data(), u);
            SIM_CACHE_READ(cache, path_counts.data(), u);
            const int32_t current_depth = depths[u];
            const int64_t source_paths = path_counts[u];

            // ECG_EDGE_MASKS: consume the OUT-edge per-edge masks (transpose-correct —
            // epoch = next in-neighbour of dest > u = next reader of depths[dest]/
            // path_counts[dest]) via the shared helper. Mirror of BFS-TD. Inert on
            // symmetric graphs (in==out); the correct dual mask for directed graphs.
            const size_t u_outdeg = (size_t)g.out_degree(u);
            size_t edge_pos = 0;
            auto out_neigh = g.out_neigh(u);
            for (auto edge_it = out_neigh.begin();
                 edge_it != out_neigh.end(); ++edge_it) {
                if (!ecg_record) {
                    SIM_CACHE_READ_EDGE(cache, edge_it);
                } else {
                    SIM_ECG_EDGE(cache, ecg_meta, edge_it, out_edge_base,
                                 ::ecg_metadata::kOutRecordBase,
                                 ::ecg_metadata::kOutSidecarBase);
                }
                NodeID v = *edge_it;
                // Resolve this edge's mask once (sets the epoch) and reuse for both the
                // depths[v] and path_counts[v] reads (same dest -> same epoch).
                const uint32_t edge_mask_val = graph_ctx.resolveEdgeMaskAndEpoch(
                    EdgeMaskDir::OUT, (uint32_t)u, u_outdeg, edge_pos, vertex_masks[v]);
                SIM_CACHE_READ_MASKED(cache, depths.data(), v, graph_ctx, edge_mask_val);

                if (depths[v] == -1 &&
                    compare_and_swap(
                        depths[v], static_cast<int32_t>(-1),
                        current_depth + 1)) {
                    SIM_CACHE_WRITE(cache, depths.data(), v);
                    queue.push_back(v);
                }

                if (depths[v] == current_depth + 1) {
                    #pragma omp critical
                    {
                        SIM_CACHE_READ_MASKED(
                            cache, path_counts.data(), v,
                            graph_ctx, edge_mask_val);
                        succ.push_back(v);
                        fetch_and_add(succ_start[u + 1], 1);
                        fetch_and_add(path_counts[v], source_paths);
                        SIM_CACHE_WRITE(cache, path_counts.data(), v);
                    }
                }
                ++edge_pos;
            }
        }
        queue.slide_window();
    }
    depth_index.push_back(queue.begin());
    
    // Prefix sum for successor starts
    for (NodeID n = 0; n < g.num_nodes(); n++)
        succ_start[n + 1] += succ_start[n];
    
    // Backward phase - accumulate dependencies
    for (int32_t d = depth - 1; d >= 0; d--) {
        #pragma omp parallel for schedule(dynamic, 64)
        for (auto it = depth_index[d]; it < depth_index[d + 1]; it++) {
            NodeID u = *it;
            // The backward phase traverses the runtime-built compacted successor DAG
            // (succ/succ_start), NOT a static graph edge list, so it keeps plain
            // per-vertex accesses (no per-edge mask). Clear the sticky per-edge epoch
            // left by the forward phase so these plain reads aren't stamped stale.
            graph_ctx.clearEdgeEpoch();
            // Track path_counts and deltas reads
            SIM_CACHE_READ(cache, path_counts.data(), u);
            SIM_CACHE_READ(cache, deltas.data(), u);
            
            ScoreT delta_u = 0;
            for (int64_t i = succ_start[u]; i < succ_start[u + 1]; i++) {
                NodeID v = succ[i];
                // Track path_counts and deltas accesses
                SIM_CACHE_READ(cache, path_counts.data(), v);
                SIM_CACHE_READ(cache, deltas.data(), v);
                delta_u += static_cast<ScoreT>(path_counts[u]) / 
                           static_cast<ScoreT>(path_counts[v]) * (1 + deltas[v]);
            }
            deltas[u] = delta_u;
            SIM_CACHE_WRITE(cache, deltas.data(), u);
            
            #pragma omp atomic
            scores[u] += delta_u;
            SIM_CACHE_WRITE(cache, scores.data(), u);
        }
    }
}

template<typename CacheType>
pvector<ScoreT> BC_Sim(const Graph &g, int num_iters, CacheType &cache) {
    pvector<ScoreT> scores(
        g.num_nodes(), ScoreT(0), GRAPH_SIM_PROPERTY_ALIGNMENT);
    
    SourcePicker<Graph> sp(g);
    for (int i = 0; i < num_iters; i++) {
        NodeID source = sp.PickNext();
        BCBFS_Sim(g, source, scores, cache);
    }
    
    // Normalize scores
    ScoreT max_score = *max_element(scores.begin(), scores.end());
    if (max_score > 0) {
        for (NodeID n : g.vertices())
            scores[n] /= max_score;
    }
    
    return scores;
}

void PrintBCStats(const Graph &g, const pvector<ScoreT> &scores) {
    auto [min_it, max_it] = minmax_element(scores.begin(), scores.end());
    cout << "BC scores range: [" << *min_it << ", " << *max_it << "]" << endl;
}

bool BCVerifier(const Graph &g, const pvector<ScoreT> &scores, int num_iters) {
    // Simple verification: check scores are non-negative and bounded
    for (NodeID n : g.vertices()) {
        if (scores[n] < 0 || scores[n] > 1) {
            cout << "BC verification failed: score out of range at node " << n << endl;
            return false;
        }
    }
    return true;
}

int main(int argc, char *argv[]) {
    CLIterApp cli(argc, argv, "bc-sim", 4);
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        auto BCBound = [&cli, &cache](const Graph &g) {
            return BC_Sim(g, cli.num_iters(), cache);
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return BCVerifier(g, scores, cli.num_iters());
        };
        
        BenchmarkKernel(cli, g, BCBound, PrintBCStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    } else if (fast) {
        // FAST single-core cache simulation (no locks, ~10x faster)
        FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
        
        auto BCBound = [&cli, &cache](const Graph &g) {
            return BC_Sim(g, cli.num_iters(), cache);
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return BCVerifier(g, scores, cli.num_iters());
        };
        
        BenchmarkKernel(cli, g, BCBound, PrintBCStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    } else {
        CacheHierarchy cache = CacheHierarchy::fromEnvironment();
        
        auto BCBound = [&cli, &cache](const Graph &g) {
            return BC_Sim(g, cli.num_iters(), cache);
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return BCVerifier(g, scores, cli.num_iters());
        };
        
        BenchmarkKernel(cli, g, BCBound, PrintBCStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    }
    
    return 0;
}
