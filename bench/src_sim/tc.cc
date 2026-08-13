// Copyright (c) 2024, UVA LavaLab
// Triangle Counting with Cache Simulation

#include <iostream>
#include <fstream>
#include <algorithm>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"
#include "timer.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

using namespace std;
using namespace cache_sim;

// Count triangles by iterating over edges and finding common neighbors
// Uses the edge-iterator algorithm with cache tracking
template<typename CacheType>
size_t CountTriangles_Sim(const Graph &g, CacheType &cache) {
    size_t total = 0;

    // --- Graph-aware cache context ---
    // TC primarily accesses CSR neighbor lists, not vertex property arrays.
    // Register topology for degree distribution awareness.
    GraphCacheContext graph_ctx;
    pvector<uint32_t> deg_arr(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        deg_arr[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(deg_arr.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());
    cache.initGraphContext(&graph_ctx);

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());
    
    #pragma omp parallel reduction(+ : total)
    {
        #pragma omp for schedule(dynamic, 64)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            // P-OPT: update current destination vertex
            SIM_SET_VERTEX(cache, u);
            for (NodeID v : g.out_neigh(u)) {
                if (v > u) {
                    // Count common neighbors between u and v
                    auto u_begin = g.out_neigh(u).begin();
                    auto u_end = g.out_neigh(u).end();
                    auto v_begin = g.out_neigh(v).begin();
                    auto v_end = g.out_neigh(v).end();
                    
                    // Track neighbor list accesses
                    SIM_CACHE_TRACK_NEIGHBOR(cache, u_begin);
                    SIM_CACHE_TRACK_NEIGHBOR(cache, v_begin);
                    
                    // Set intersection to count common neighbors > v
                    while (u_begin < u_end && v_begin < v_end) {
                        NodeID u_neigh = *u_begin;
                        NodeID v_neigh = *v_begin;
                        
                        if (u_neigh == v_neigh && u_neigh > v) {
                            total++;
                            u_begin++;
                            v_begin++;
                        } else if (u_neigh < v_neigh) {
                            u_begin++;
                        } else {
                            v_begin++;
                        }
                    }
                }
            }
        }
    }
    
    return total;
}

// Ordered Triangle Counting (low degree to high degree)
template<typename CacheType>
size_t OrderedCount_Sim(const Graph &g, CacheType &cache) {
    size_t total = 0;
    
    // Create degree ordering
    pvector<NodeID> degrees(g.num_nodes());
    for (NodeID n : g.vertices()) {
        degrees[n] = g.out_degree(n);
        SIM_CACHE_WRITE(cache, degrees.data(), n);
    }
    
    #pragma omp parallel reduction(+ : total)
    {
        #pragma omp for schedule(dynamic, 64)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            SIM_CACHE_READ(cache, degrees.data(), u);
            
            for (NodeID v : g.out_neigh(u)) {
                SIM_CACHE_READ(cache, degrees.data(), v);
                
                // Only process edges where degree[u] < degree[v] 
                // or (degree[u] == degree[v] && u < v)
                if (degrees[u] < degrees[v] || 
                    (degrees[u] == degrees[v] && u < v)) {
                    
                    // Find common neighbors with higher degree than v
                    auto u_it = g.out_neigh(u).begin();
                    auto u_end = g.out_neigh(u).end();
                    auto v_it = g.out_neigh(v).begin();
                    auto v_end = g.out_neigh(v).end();
                    
                    SIM_CACHE_TRACK_NEIGHBOR(cache, u_it);
                    SIM_CACHE_TRACK_NEIGHBOR(cache, v_it);
                    
                    while (u_it < u_end && v_it < v_end) {
                        NodeID w1 = *u_it;
                        NodeID w2 = *v_it;
                        
                        if (w1 == w2) {
                            SIM_CACHE_READ(cache, degrees.data(), w1);
                            // Check ordering
                            if (degrees[v] < degrees[w1] || 
                                (degrees[v] == degrees[w1] && v < w1)) {
                                total++;
                            }
                            u_it++;
                            v_it++;
                        } else if (w1 < w2) {
                            u_it++;
                        } else {
                            v_it++;
                        }
                    }
                }
            }
        }
    }
    
    return total;
}

void PrintTriangleStats(const Graph &g, size_t total_triangles) {
    cout << "Total Triangles: " << total_triangles << endl;
}

bool TCVerifier(const Graph &g, size_t test_total) {
    // Simple sequential verification
    size_t total = 0;
    for (NodeID u : g.vertices()) {
        for (NodeID v : g.out_neigh(u)) {
            if (v > u) {
                auto u_begin = g.out_neigh(u).begin();
                auto u_end = g.out_neigh(u).end();
                auto v_begin = g.out_neigh(v).begin();
                auto v_end = g.out_neigh(v).end();
                
                while (u_begin < u_end && v_begin < v_end) {
                    if (*u_begin == *v_begin && *u_begin > v) {
                        total++;
                        u_begin++;
                        v_begin++;
                    } else if (*u_begin < *v_begin) {
                        u_begin++;
                    } else {
                        v_begin++;
                    }
                }
            }
        }
    }
    
    if (total != test_total) {
        cout << "Verification FAILED: expected " << total 
             << ", got " << test_total << endl;
        return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "tc-sim");
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        auto TCBound = [&cache](const Graph &g) {
            return OrderedCount_Sim(g, cache);
        };
        auto VerifierBound = [](const Graph &g, size_t triangles) {
            return TCVerifier(g, triangles);
        };
        
        BenchmarkKernel(cli, g, TCBound, PrintTriangleStats, VerifierBound);
        
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
        
        auto TCBound = [&cache](const Graph &g) {
            return OrderedCount_Sim(g, cache);
        };
        auto VerifierBound = [](const Graph &g, size_t triangles) {
            return TCVerifier(g, triangles);
        };
        
        BenchmarkKernel(cli, g, TCBound, PrintTriangleStats, VerifierBound);
        
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
        
        auto TCBound = [&cache](const Graph &g) {
            return OrderedCount_Sim(g, cache);
        };
        auto VerifierBound = [](const Graph &g, size_t triangles) {
            return TCVerifier(g, triangles);
        };
        
        BenchmarkKernel(cli, g, TCBound, PrintTriangleStats, VerifierBound);
        
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
