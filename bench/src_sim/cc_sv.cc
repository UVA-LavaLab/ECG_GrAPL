// Copyright (c) 2024, UVA LavaLab
// Connected Components — Shiloach-Vishkin variant with Cache Simulation
//
// Iterative hook-and-shortcut algorithm that processes ALL edges every round.
// O(m log n) total edge traversals with tight pointer-chasing on comp[] array.
// Different cache profile from Afforest (cc.cc) which uses adaptive sampling.

#include <iostream>
#include <fstream>

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

template<typename CacheType>
pvector<NodeID> ShiloachVishkin_Sim(const Graph &g, CacheType &cache) {
    pvector<NodeID> comp(
        g.num_nodes(), NodeID(0), GRAPH_SIM_PROPERTY_ALIGNMENT);

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
    graph_ctx.registerPropertyArray(comp.data(), g.num_nodes(), sizeof(NodeID), llc_size);
    cache.initGraphContext(&graph_ctx);

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    cache.initGraphContext(&graph_ctx);
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
            makeOffsetMatrix(g, popt_matrix, numVtxPerLine, numEpochs);
            int numCacheLines = (g.num_nodes() + numVtxPerLine - 1) / numVtxPerLine;
            graph_ctx.initRereference(popt_matrix.data(), numCacheLines,
                                      numEpochs, g.num_nodes(), 64);
        }
    }
    
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        SIM_CACHE_WRITE(cache, comp.data(), n);
        comp[n] = n;
    }
    
    bool change = true;
    while (change) {
        change = false;
        #pragma omp parallel for
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            // P-OPT: update current destination vertex
            SIM_SET_VERTEX(cache, u);
            for (NodeID v : g.out_neigh(u)) {
                // Track: read comp[u] and comp[v]
                SIM_CACHE_READ(cache, comp.data(), u);
                SIM_CACHE_READ_MASKED(cache, comp.data(), v, graph_ctx, vertex_masks[v]);
                NodeID comp_u = comp[u];
                NodeID comp_v = comp[v];
                
                if (comp_u == comp_v) continue;
                NodeID high = comp_u > comp_v ? comp_u : comp_v;
                NodeID low = comp_u + (comp_v - high);
                if (high == comp[high]) {
                    change = true;
                    // Track: write comp[high]
                    SIM_CACHE_WRITE(cache, comp.data(), high);
                    comp[high] = low;
                }
            }
        }
        
        #pragma omp parallel for
        for (NodeID n = 0; n < g.num_nodes(); n++) {
            while (comp[n] != comp[comp[n]]) {
                // Track: read comp[n], comp[comp[n]]
                SIM_CACHE_READ(cache, comp.data(), n);
                SIM_CACHE_READ(cache, comp.data(), comp[n]);
                SIM_CACHE_WRITE(cache, comp.data(), n);
                comp[n] = comp[comp[n]];
            }
        }
    }
    
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
    CLApp cli(argc, argv, "cc-sv-sim");
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        auto CCBound = [&cache](const Graph &g) {
            return ShiloachVishkin_Sim(g, cache);
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
            return ShiloachVishkin_Sim(g, cache);
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
            return ShiloachVishkin_Sim(g, cache);
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
