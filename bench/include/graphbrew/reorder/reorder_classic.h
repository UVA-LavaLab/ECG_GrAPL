// ============================================================================
// GraphBrew - Classic Reordering Algorithms
// ============================================================================
// This header implements classic graph reordering algorithms:
//   - GORDER (9):   Graph Ordering using dynamic programming + BFS
//   - CORDER (10):  Cache-aware workload balancing
//   - RCMORDER (11): Reverse Cuthill-McKee (bandwidth reduction)
//
// These algorithms focus on bandwidth/cache optimization rather than
// community detection.
//
// External Dependencies:
//   - GOrder requires GoGraph.h and GoUtil.h (included)
//   - COrder uses vec2d.h for 2D vectors
//
// Author: GraphBrew Team
// License: See LICENSE.txt
// ============================================================================

#ifndef REORDER_CLASSIC_H_
#define REORDER_CLASSIC_H_

#include "reorder_types.h"

// External headers required for these algorithms
// These are part of the GraphBrew include tree
#include <gorder/GoGraph.h>
#include <gorder/GoUtil.h>
#include <corder/vec2d.h>

// ============================================================================
// CORDER PARAMETERS
// ============================================================================

/**
 * @brief Global parameters for COrder algorithm
 * 
 * These control the partitioning behavior of the cache-aware ordering.
 */
namespace corder_params {
    inline unsigned partition_size = 1024;    ///< Size of each partition
    inline unsigned num_partitions = 0;       ///< Number of partitions (computed)
    inline unsigned overflow_ceil = 0;        ///< Overflow ceiling for large vertices
}

// ============================================================================
// GORDER (Algorithm 9)
// ============================================================================

/**
 * @brief GOrder - Graph Ordering using dynamic programming and windowing
 * 
 * GOrder uses a greedy algorithm with a sliding window to optimize
 * cache performance. It processes vertices in BFS order and places
 * them to maximize locality within a window.
 * 
 * Algorithm:
 *   1. Build internal graph representation
 *   2. Transform graph for processing
 *   3. Apply greedy ordering with window size w
 *   4. Map back to original vertex IDs
 * 
 * Complexity: O(n × w) where w is window size (default: 5)
 * 
 * Reference:
 *   Wei, H., Yu, J.X., Lu, C., Lin, X. (2016). Speedup Graph Processing
 *   by Graph Ordering. SIGMOD.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param filename Graph filename (used for caching)
 * 
 * @note Requires GoGraph.h and GoUtil.h from gorder include
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateGOrderMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                           pvector<NodeID_>& new_ids,
                           const std::string& filename) {
    
    const int window = 5;  // Default window size (w=5 per SIGMOD'16 paper)
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges_directed();
    
    // Build edge list for GOrder
    std::vector<std::pair<int, int>> edges(num_edges);
    
    #pragma omp parallel for
    for (NodeID_ i = 0; i < num_nodes; ++i) {
        NodeID_ out_start = g.out_offset(i);
        NodeID_ j = 0;
        
        for (DestID_ neighbor : g.out_neigh(i)) {
            NodeID_ dest;
            if (g.is_weighted()) {
                dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
            } else {
                dest = static_cast<NodeID_>(neighbor);
            }
            edges[out_start + j] = std::make_pair(static_cast<int>(i), 
                                                   static_cast<int>(dest));
            ++j;
        }
    }
    
    // Initialize GOrder graph structure
    Gorder::GoGraph go;
    std::vector<int> order;
    Timer tm;
    
    std::string name = GorderUtil::extractFilename(filename.c_str());
    go.setFilename(name);
    
    // Read and transform graph
    tm.Start();
    go.readGraphEdgelist(edges, g.num_nodes());
    edges.clear();
    go.Transform();
    tm.Stop();
    PrintTime("GOrder graph", tm.Seconds());
    
    // Compute ordering
    tm.Start();
    go.GorderGreedy(order, window);
    tm.Stop();
    PrintTime("GOrder Map Time", tm.Seconds());
    
    // Ensure output is large enough
    if (new_ids.size() < static_cast<size_t>(go.vsize)) {
        new_ids.resize(go.vsize);
    }
    
    // Map back to original IDs
    #pragma omp parallel for
    for (int i = 0; i < go.vsize; i++) {
        int u = order[go.order_l1[i]];
        new_ids[i] = static_cast<NodeID_>(u);
    }
}

// ============================================================================
// RCMORDER (Algorithm 11) — Original GoGraph-based MIND-start RCM
// ============================================================================

/**
 * @brief Reverse Cuthill-McKee ordering for bandwidth reduction (baseline)
 * 
 * Original RCM implementation using GoGraph. Uses MIND starting-node
 * strategy (global min-degree). Default variant for -o 11.
 * BNF variant (-o 11:bnf) is in reorder_rcm.h.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param filename Graph filename (used for caching)
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateRCMOrderMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                             pvector<NodeID_>& new_ids,
                             const std::string& filename) {
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges_directed();
    
    // Build edge list
    std::vector<std::pair<int, int>> edges(num_edges);
    
    #pragma omp parallel for
    for (NodeID_ i = 0; i < num_nodes; ++i) {
        NodeID_ out_start = g.out_offset(i);
        NodeID_ j = 0;
        
        for (DestID_ neighbor : g.out_neigh(i)) {
            NodeID_ dest;
            if (g.is_weighted()) {
                dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
            } else {
                dest = static_cast<NodeID_>(neighbor);
            }
            edges[out_start + j] = std::make_pair(static_cast<int>(i),
                                                   static_cast<int>(dest));
            ++j;
        }
    }
    
    // Initialize GoGraph (RCM is implemented in GoGraph)
    Gorder::GoGraph go;
    std::vector<int> order;
    Timer tm;
    
    std::string name = GorderUtil::extractFilename(filename.c_str());
    go.setFilename(name);
    
    // Read and transform graph
    tm.Start();
    go.readGraphEdgelist(edges, g.num_nodes());
    edges.clear();
    go.Transform();
    tm.Stop();
    PrintTime("RCMOrder graph", tm.Seconds());
    
    // Compute RCM ordering
    tm.Start();
    go.RCMOrder(order);
    tm.Stop();
    PrintTime("RCMOrder Map Time", tm.Seconds());
    
    // Ensure output is large enough
    if (new_ids.size() < static_cast<size_t>(go.vsize)) {
        new_ids.resize(go.vsize);
    }
    
    // Map back to original IDs
    #pragma omp parallel for
    for (int i = 0; i < go.vsize; i++) {
        int u = order[go.order_l1[i]];
        new_ids[i] = static_cast<NodeID_>(u);
    }
}

// ============================================================================
// RCM BNF variant (-o 11:bnf) — Modern CSR-native BNF-start RCM
// ============================================================================
// Improved RCM variant using George-Liu + BNF starting node selection and
// level-parallel BFS, operating directly on the CSR graph.
// Accessed via: -o 11:bnf  (default -o 11 uses GoGraph above)
// See agent/optimizing-rcm/ for design rationale and literature.
#include "reorder_rcm.h"

// ============================================================================
// Improved GOrder variant operating directly on the CSR graph with parallel
// score updates.  Accessed via: -o 9:csr  (default -o 9 uses GoGraph above)
#include "reorder_gorder.h"

// ============================================================================
// CORDER (Algorithm 10)
// ============================================================================

/**
 * @brief Cache-aware workload balancing ordering
 * 
 * COrder partitions vertices into "hot" (high-degree) and "cold" (low-degree)
 * segments, then interleaves them within fixed-size partitions to balance
 * workload across cache lines.
 * 
 * Algorithm:
 *   1. Classify vertices as hot (degree > avg) or cold (degree <= avg)
 *   2. Partition the graph into fixed-size segments
 *   3. Distribute hot/cold vertices evenly within each segment
 * 
 * Best for: Cache-sensitive applications with mixed degree distribution
 * 
 * Complexity: O(n) - linear pass through vertices
 * 
 * Reference:
 *   Zhang, Y., et al. (2017). Making caches work for graph analytics.
 *   IEEE Big Data.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateCOrderMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                           pvector<NodeID_>& new_ids) {
    Timer t;
    t.Start();
    
    const auto num_nodes = g.num_nodes();
    const auto num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("COrder Map Time", t.Seconds());
        return;
    }
    
    const unsigned average_degree = num_edges / num_nodes;
    
    // Configure partitioning
    corder_params::partition_size = 1024;
    corder_params::num_partitions = (num_nodes - 1) / corder_params::partition_size + 1;
    const unsigned num_partitions = corder_params::num_partitions;
    
    // Classify vertices into hot (large degree) and cold (small degree)
    std::vector<unsigned> segment_large;
    segment_large.reserve(num_nodes);
    std::vector<unsigned> segment_small;
    segment_small.reserve(num_nodes / 2);
    
    for (unsigned i = 0; i < num_nodes; i++) {
        if (g.out_degree(i) > average_degree) {
            segment_large.push_back(i);
        } else {
            segment_small.push_back(i);
        }
    }
    
    // Calculate distribution per segment
    const unsigned num_large_per_seg = 
        std::ceil(static_cast<float>(segment_large.size()) / num_partitions);
    corder_params::overflow_ceil = num_large_per_seg;
    const unsigned num_small_per_seg = corder_params::partition_size - num_large_per_seg;
    
    // Find last complete segment
    // Fix: Guard against unsigned underflow when last_cls reaches 0
    unsigned last_cls = num_partitions - 1;
    while (last_cls > 0 &&
           ((num_large_per_seg * last_cls > segment_large.size()) ||
            (num_small_per_seg * last_cls > segment_small.size()))) {
        last_cls -= 1;
    }
    
    // Assign IDs for complete segments
    #pragma omp parallel for schedule(static)
    for (unsigned i = 0; i < last_cls; i++) {
        unsigned index = i * corder_params::partition_size;
        
        // Place hot vertices first
        for (unsigned j = 0; j < num_large_per_seg; j++) {
            new_ids[segment_large[i * num_large_per_seg + j]] = index++;
        }
        
        // Then cold vertices
        for (unsigned j = 0; j < num_small_per_seg; j++) {
            new_ids[segment_small[i * num_small_per_seg + j]] = index++;
        }
    }
    
    // Handle remaining vertices in last segment
    auto last_large = num_large_per_seg * last_cls;
    auto last_small = num_small_per_seg * last_cls;
    unsigned index = last_cls * corder_params::partition_size;
    
    #pragma omp parallel for
    for (unsigned i = last_large; i < segment_large.size(); i++) {
        unsigned local_index = __sync_fetch_and_add(&index, 1);
        new_ids[segment_large[i]] = local_index;
    }
    
    #pragma omp parallel for
    for (unsigned i = last_small; i < segment_small.size(); i++) {
        unsigned local_index = __sync_fetch_and_add(&index, 1);
        new_ids[segment_small[i]] = local_index;
    }
    
    t.Stop();
    PrintTime("COrder Map Time", t.Seconds());
}

/**
 * @brief COrder v2 - Optimized parallel version using Vector2d
 * 
 * This version uses thread-local storage for better parallel performance
 * on systems with many cores.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateCOrderMapping_v2(const CSRGraph<NodeID_, DestID_, invert>& g,
                              pvector<NodeID_>& new_ids) {
    Timer t;
    t.Start();
    
    const auto num_nodes = g.num_nodes();
    const auto num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("COrder_v2 Map Time", t.Seconds());
        return;
    }
    
    corder_params::partition_size = 1024;
    corder_params::num_partitions = (num_nodes - 1) / corder_params::partition_size + 1;
    const unsigned num_partitions = corder_params::num_partitions;
    const uint32_t average_degree = num_edges / num_nodes;
    
    const int max_threads = omp_get_max_threads();
    
    // Thread-local storage for hot/cold vertices
    Vector2d<unsigned> large_segment(max_threads);
    Vector2d<unsigned> small_segment(max_threads);
    
    // Parallel classification
    #pragma omp parallel for schedule(static, 1024) num_threads(max_threads)
    for (unsigned i = 0; i < num_nodes; i++) {
        if (g.out_degree(i) > average_degree) {
            large_segment[omp_get_thread_num()].push_back(i);
        } else {
            small_segment[omp_get_thread_num()].push_back(i);
        }
    }
    
    // Compute offsets for each thread's contribution
    std::vector<unsigned> large_offset(max_threads + 1, 0);
    std::vector<unsigned> small_offset(max_threads + 1, 0);
    
    for (int i = 0; i < max_threads; i++) {
        large_offset[i + 1] = large_offset[i] + large_segment[i].size();
        small_offset[i + 1] = small_offset[i] + small_segment[i].size();
    }
    
    const unsigned total_large = large_offset[max_threads];
    const unsigned total_small = small_offset[max_threads];
    
    const unsigned cluster_size = corder_params::partition_size;
    const unsigned num_clusters = num_partitions;
    const unsigned num_large_per_seg = std::ceil(static_cast<float>(total_large) / num_clusters);
    const unsigned num_small_per_seg = cluster_size - num_large_per_seg;
    
    // Parallel construction of partitions
    #pragma omp parallel for schedule(static) num_threads(max_threads)
    for (unsigned i = 0; i < num_clusters; i++) {
        unsigned index = i * cluster_size;
        
        // Calculate range for hot vertices
        unsigned num_large = (i != num_clusters - 1) ? 
            (i + 1) * num_large_per_seg : total_large;
        unsigned large_per_seg = (i != num_clusters - 1) ?
            num_large_per_seg : total_large - i * num_large_per_seg;
        
        // Calculate range for cold vertices  
        unsigned num_small = (i != num_clusters - 1) ?
            (i + 1) * num_small_per_seg : total_small;
        unsigned small_per_seg = (i != num_clusters - 1) ?
            num_small_per_seg : total_small - i * num_small_per_seg;
        
        // Find starting thread and vertex for hot vertices
        unsigned large_start_t = 0, large_start_v = 0;
        unsigned large_end_t = 0, large_end_v = 0;
        
        for (int th = 0; th < max_threads; th++) {
            if (large_offset[th + 1] > num_large - large_per_seg) {
                large_start_t = th;
                large_start_v = num_large - large_per_seg - large_offset[th];
                break;
            }
        }
        
        for (int th = large_start_t; th < max_threads; th++) {
            if (large_offset[th + 1] >= num_large) {
                large_end_t = th;
                large_end_v = num_large - large_offset[th] - 1;
                break;
            }
        }
        
        // Find starting thread and vertex for cold vertices
        unsigned small_start_t = 0, small_start_v = 0;
        unsigned small_end_t = 0, small_end_v = 0;
        
        for (int th = 0; th < max_threads; th++) {
            if (small_offset[th + 1] > num_small - small_per_seg) {
                small_start_t = th;
                small_start_v = num_small - small_per_seg - small_offset[th];
                break;
            }
        }
        
        for (int th = small_start_t; th < max_threads; th++) {
            if (small_offset[th + 1] >= num_small) {
                small_end_t = th;
                small_end_v = num_small - small_offset[th] - 1;
                break;
            }
        }
        
        // Place hot vertices
        if (large_start_t == large_end_t) {
            if (!large_segment[large_start_t].empty()) {
                for (unsigned j = large_start_v; j <= large_end_v; j++) {
                    new_ids[large_segment[large_start_t][j]] = index++;
                }
            }
        } else {
            for (unsigned th = large_start_t; th <= large_end_t; th++) {
                unsigned start = (th == large_start_t) ? large_start_v : 0;
                unsigned end = (th == large_end_t) ? large_end_v + 1 : large_segment[th].size();
                for (unsigned j = start; j < end; j++) {
                    new_ids[large_segment[th][j]] = index++;
                }
            }
        }
        
        // Place cold vertices
        if (small_start_t == small_end_t) {
            if (!small_segment[small_start_t].empty()) {
                for (unsigned j = small_start_v; j <= small_end_v; j++) {
                    new_ids[small_segment[small_start_t][j]] = index++;
                }
            }
        } else {
            for (unsigned th = small_start_t; th <= small_end_t; th++) {
                unsigned start = (th == small_start_t) ? small_start_v : 0;
                unsigned end = (th == small_end_t) ? small_end_v + 1 : small_segment[th].size();
                for (unsigned j = start; j < end; j++) {
                    new_ids[small_segment[th][j]] = index++;
                }
            }
        }
    }
    
    t.Stop();
    PrintTime("COrder Map Time", t.Seconds());
}

#endif  // REORDER_CLASSIC_H_
