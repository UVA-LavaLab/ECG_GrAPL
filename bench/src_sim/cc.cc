// Copyright (c) 2024, UVA LavaLab
// Connected Components — Afforest variant with Cache Simulation
//
// Two-phase adaptive algorithm: (1) sparse sampling of 1 neighbor per round,
// (2) full edge traversal skipping the largest component.
// Different cache profile from SV (cc_sv.cc): fewer total edge touches but
// scattered sampling pattern in phase 1.

#include <iostream>
#include <fstream>
#include <random>
#include <unordered_map>

#include "benchmark.h"
#include "bitmap.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

#include "graphbrew/partition/cagra/popt.h"

using namespace std;
using namespace cache_sim;

// Link two vertices in union-find with cache tracking
template<typename CacheType>
void Link_Sim(NodeID u, NodeID v, pvector<NodeID>& comp, NodeID* comp_ptr,
             CacheType& cache, GraphCacheContext& graph_ctx,
             const std::vector<uint32_t>& vertex_masks,
             size_t out_degree, size_t edge_pos) {
    graph_ctx.clearEdgeEpoch();
    SIM_CACHE_READ(cache, comp_ptr, u);
    const uint32_t edge_mask = graph_ctx.resolveEdgeMaskAndEpoch(
        EdgeMaskDir::OUT, static_cast<uint32_t>(u), out_degree,
        edge_pos, vertex_masks[v]);
    SIM_CACHE_READ_MASKED(cache, comp_ptr, v, graph_ctx, edge_mask);
    // The remaining union-find pointer chasing is not tied to this edge record.
    graph_ctx.clearEdgeEpoch();
    NodeID p1 = comp[u];
    NodeID p2 = comp[v];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        SIM_CACHE_READ(cache, comp_ptr, high);
        NodeID p_high = comp[high];
        if ((p_high == low) ||
            (p_high == high && __sync_bool_compare_and_swap(&comp[high], high, low))) {
            SIM_CACHE_WRITE(cache, comp_ptr, high);
            break;
        }
        SIM_CACHE_READ(cache, comp_ptr, high);
        p1 = comp[comp[high]];
        SIM_CACHE_READ(cache, comp_ptr, p1);
        p2 = comp[low];
        SIM_CACHE_READ(cache, comp_ptr, p2);
    }
}

// Compress (path shortening) with cache tracking
template<typename CacheType>
void Compress_Sim(const Graph &g, pvector<NodeID>& comp, NodeID* comp_ptr,
                 CacheType& cache, GraphCacheContext& graph_ctx,
                 const std::vector<uint32_t>& vertex_masks) {
    #pragma omp parallel for schedule(dynamic, 16384)
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        graph_ctx.clearEdgeEpoch();
        SIM_CACHE_READ(cache, comp_ptr, n);
        SIM_CACHE_READ(cache, comp_ptr, comp[n]);
        while (comp[n] != comp[comp[n]]) {
            SIM_CACHE_READ(cache, comp_ptr, comp[comp[n]]);
            SIM_CACHE_WRITE(cache, comp_ptr, n);
            comp[n] = comp[comp[n]];
            SIM_CACHE_READ(cache, comp_ptr, n);
            SIM_CACHE_READ(cache, comp_ptr, comp[n]);
        }
    }
}

template<typename CacheType>
pvector<NodeID> Afforest_Sim(const Graph &g, CacheType &cache,
                              int32_t neighbor_rounds = 2) {
    pvector<NodeID> comp(
        g.num_nodes(), NodeID(0), GRAPH_SIM_PROPERTY_ALIGNMENT);
    NodeID* comp_ptr = comp.data();

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
    graph_ctx.registerPropertyArray(comp_ptr, g.num_nodes(), sizeof(NodeID), llc_size);
    cache.initGraphContext(&graph_ctx);

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());

    // Build P-OPT rereference matrix (for POPT and ECG policies)
    static pvector<uint8_t> popt_matrix;
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        if (policy == EvictionPolicy::POPT ||
            policy == EvictionPolicy::ECG) {
            constexpr int numVtxPerLine = 64 / sizeof(NodeID);
            constexpr int numEpochs = 256;
            // CC sweeps out_neigh(u); it assumes an UNDIRECTED graph (out==in), so
            // the helper forces CSR there. natural=false records the transpose
            // intent for the (out-of-scope) directed case.
            buildAndRegisterReref(g, graph_ctx, /*natural_csr=*/false, "CC(undirected)",
                                  numVtxPerLine, numEpochs, popt_matrix);
            if (std::getenv("ECG_EXACT_REREF")) {
                const char* eb = std::getenv("ECG_EXACT_BITS");
                if (eb) graph_ctx.exact_bits = (uint32_t)atoi(eb);
                graph_ctx.registerOutAdjacencyExact(g);  // ECG_EXACT mode (sweep flavor)
            }
        }
    }

    // CC traverses OUT-edges and reads comp[dest], so use the shared OUT-edge
    // builder. CC remains undirected-only, but this keeps record position,
    // tier, and K2 delivery exact instead of relying on IN/OUT order matching.
    if (std::getenv("ECG_EDGE_MASKS") ||
        std::getenv("ECG_CC_EDGE_MASKS") ||
        (graph_ctx.mask_config.prefetch_mode == 6 &&
         std::getenv("ECG_EDGE_MASK_EPOCH"))) {
        graph_ctx.buildOutEdgeMasks(g);
    }
    const bool ecg_record = GraphSimEcgEdgeRecord();
    const bool record_charged = ecg_record &&
        GraphSimEnvIntClamped("ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    const bool stream_bypass =
        GraphSimEnvIntClamped("ECG_STREAM_BYPASS", 0, 0, 1) > 0;
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
    ::ecg_metadata::announce(ecg_meta, "cc");
    ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "cc");
    NodeID* out_edge_base = g.num_nodes() > 0
        ? g.out_neigh(0).begin() : nullptr;

    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        SIM_CACHE_WRITE(cache, comp_ptr, n);
        comp[n] = n;
    }

    // Phase 1: Sparse neighbor sampling (1 edge per vertex per round)
    for (int r = 0; r < neighbor_rounds; ++r) {
        #pragma omp parallel for schedule(dynamic, 16384)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            SIM_SET_VERTEX(cache, u);
            auto out_neigh = g.out_neigh(u, r);
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
                Link_Sim(u, v, comp, comp_ptr, cache, graph_ctx,
                         vertex_masks, static_cast<size_t>(g.out_degree(u)),
                         static_cast<size_t>(r));
                break;  // exactly ONE neighbor per round
            }
        }
        Compress_Sim(g, comp, comp_ptr, cache, graph_ctx, vertex_masks);
    }

    // Sample to find the largest intermediate component
    std::unordered_map<NodeID, int> sample_counts(32);
    std::mt19937 gen;
    std::uniform_int_distribution<NodeID> distribution(0, comp.size() - 1);
    for (int i = 0; i < 1024; i++) {
        NodeID n = distribution(gen);
        sample_counts[comp[n]]++;
    }
    NodeID c = std::max_element(sample_counts.begin(), sample_counts.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;

    // Phase 2: Full traversal, skipping the largest component
    #pragma omp parallel for schedule(dynamic, 16384)
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        SIM_SET_VERTEX(cache, u);
        graph_ctx.clearEdgeEpoch();
        SIM_CACHE_READ(cache, comp_ptr, u);
        if (comp[u] == c) continue;  // skip largest component
        size_t epos = 0;
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
            Link_Sim(u, v, comp, comp_ptr, cache, graph_ctx,
                     vertex_masks, static_cast<size_t>(g.out_degree(u)), epos);
            ++epos;
        }
    }
    Compress_Sim(g, comp, comp_ptr, cache, graph_ctx, vertex_masks);

    return comp;
}

void PrintCompStats(const Graph &g, const pvector<NodeID> &comp) {
    cout << endl;
    unordered_map<NodeID, NodeID> count;
    for (NodeID n : g.vertices())
        count[comp[n]] += 1;
    int k = 5;
    vector<pair<NodeID, NodeID>> count_vector;
    count_vector.reserve(count.size());
    for (auto kvp : count)
        count_vector.push_back(kvp);
    vector<pair<NodeID, NodeID>> top_k = TopK(count_vector, k);
    k = min(k, static_cast<int>(top_k.size()));
    cout << k << " biggest clusters:" << endl;
    for (auto kvp : top_k)
        cout << kvp.second << " " << kvp.first << endl;
}

bool CCVerifier(const Graph &g, const pvector<NodeID> &comp) {
    unordered_map<NodeID, NodeID> label_to_source;
    for (NodeID n : g.vertices())
        label_to_source[comp[n]] = n;
    
    Bitmap visited(g.num_nodes());
    visited.reset();
    vector<NodeID> frontier;
    frontier.reserve(g.num_nodes());
    
    for (auto& kvp : label_to_source) {
        NodeID curr_label = kvp.first;
        NodeID source = kvp.second;
        frontier.clear();
        frontier.push_back(source);
        visited.set_bit(source);
        
        for (auto it = frontier.begin(); it != frontier.end(); it++) {
            NodeID u = *it;
            for (NodeID v : g.out_neigh(u)) {
                if (comp[v] != curr_label) return false;
                if (!visited.get_bit(v)) {
                    visited.set_bit(v);
                    frontier.push_back(v);
                }
            }
        }
    }
    
    for (NodeID n : g.vertices())
        if (!visited.get_bit(n) && g.out_degree(n) != 0)
            return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "cc-afforest-sim");
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        auto CCBound = [&cache](const Graph &g) {
            return Afforest_Sim(g, cache);
        };
        
        BenchmarkKernel(cli, g, CCBound, PrintCompStats, CCVerifier);
        
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
        
        auto CCBound = [&cache](const Graph &g) {
            return Afforest_Sim(g, cache);
        };
        
        BenchmarkKernel(cli, g, CCBound, PrintCompStats, CCVerifier);
        
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
        
        auto CCBound = [&cache](const Graph &g) {
            return Afforest_Sim(g, cache);
        };
        
        BenchmarkKernel(cli, g, CCBound, PrintCompStats, CCVerifier);
        
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
