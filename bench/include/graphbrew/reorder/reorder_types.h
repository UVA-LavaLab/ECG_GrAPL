// ============================================================================
// GraphBrew - Reordering Types and Common Utilities
// ============================================================================
// This header defines common types, structures, and utility functions used
// across all reordering algorithm implementations.
//
// UNIFIED CONFIGURATION (reorder::ReorderConfig):
//   All community-based reordering algorithms (GraphBrew, Leiden,
//   RabbitOrder, Adaptive) use the unified defaults defined in the
//   reorder:: namespace. This ensures:
//   - Consistent behavior across all algorithms
//   - Canonical default parameters
//   - Fair benchmarking comparisons
//
//   Key defaults:
//     reorder::DEFAULT_RESOLUTION = 1.0       (auto-computed from graph)
//     reorder::DEFAULT_MAX_ITERATIONS = 10    (per pass)
//     reorder::DEFAULT_MAX_PASSES = 10        (total)
//
// Architecture:
//   - reorder_types.h     : Types, ReorderConfig, utilities (this file)
//   - reorder_basic.h     : Basic algorithms (Original, Random, Sort)
//   - reorder_hub.h       : Hub-based algorithms (HubSort, HubCluster, DBG)
//   - reorder_rabbit.h    : RabbitOrder (Louvain-based community detection)
//   - reorder_classic.h   : Classic algorithms (GOrder, COrder, RCM)
//   - reorder_graphbrew.h      : GraphBrew unified reordering framework
//   - reorder_adaptive.h  : Adaptive algorithm selection
//   - reorder.h           : Main dispatcher that includes all headers
//
// Author: GraphBrew Team
// License: See LICENSE.txt
// ============================================================================

#ifndef REORDER_TYPES_H_
#define REORDER_TYPES_H_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <parallel/algorithm>
#include <parallel/numeric>
#include <queue>
#include <random>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>     // sysconf, _SC_LEVEL3_CACHE_SIZE
#endif

#include <omp.h>

// Include the full graph.h instead of forward declaring CSRGraph
// This ensures template parameters match
#include <graph.h>
#include <pvector.h>
#include <timer.h>
#include <util.h>

// ============================================================================
// TYPE ALIASES
// ============================================================================

// Default node weight type for weighted graphs
#ifndef TYPE
#define TYPE float
#endif

// Community ID type - uint32_t supports up to 4B communities
using CommunityID = uint32_t;

// K type used in Leiden algorithms  
using K = uint32_t;

// Edge type alias - represents an edge as (source, destination) pair
// For weighted graphs, DestID_ will be NodeWeight<NodeID_, WeightT_>
template <typename NodeID_, typename DestID_ = NodeID_>
using Edge = EdgePair<NodeID_, DestID_>;

// EdgeList type alias - a vector of edges
// Used throughout reordering algorithms for edge-list graph representation
template <typename NodeID_, typename DestID_ = NodeID_>
using EdgeList = pvector<Edge<NodeID_, DestID_>>;

// ============================================================================
// REORDERING ALGORITHM STRING CONVERSION
// ============================================================================

/**
 * @brief Convert ReorderingAlgo enum to human-readable string
 * 
 * Used for logging, debugging, and output formatting.
 * 
 * @param algo The reordering algorithm enum value
 * @return String name of the algorithm
 */
inline const std::string ReorderingAlgoStr(ReorderingAlgo algo) {
    switch (algo) {
        case ORIGINAL:        return "Original";
        case Random:          return "Random";
        case Sort:            return "Sort";
        case HubSort:         return "HubSort";
        case HubCluster:      return "HubCluster";
        case DBG:             return "DBG";
        case HubSortDBG:      return "HubSortDBG";
        case HubClusterDBG:   return "HubClusterDBG";
        case RabbitOrder:     return "RabbitOrder";
        case GOrder:          return "GOrder";
        case COrder:          return "COrder";
        case RCMOrder:        return "RCMOrder";
        case GraphBrewOrder:  return "GraphBrewOrder";
        case MAP:             return "MAP";
        case AdaptiveOrder:   return "AdaptiveOrder";
        case LeidenOrder:     return "LeidenOrder";
        case GoGraphOrder:    return "GoGraphOrder";
        default:
            std::cerr << "Unknown Reordering Algorithm: " << static_cast<int>(algo) << std::endl;
            std::abort();
    }
}

/**
 * @brief Convert integer ID to ReorderingAlgo enum
 * 
 * Used when parsing command-line arguments.
 * 
 * @param value Integer algorithm ID (0-15)
 * @return Corresponding ReorderingAlgo enum value
 */
inline ReorderingAlgo getReorderingAlgo(int value) {
    switch (value) {
        case 0:  return ORIGINAL;
        case 1:  return Random;
        case 2:  return Sort;
        case 3:  return HubSort;
        case 4:  return HubCluster;
        case 5:  return DBG;
        case 6:  return HubSortDBG;
        case 7:  return HubClusterDBG;
        case 8:  return RabbitOrder;
        case 9:  return GOrder;
        case 10: return COrder;
        case 11: return RCMOrder;
        case 12: return GraphBrewOrder;
        case 13: return MAP;
        case 14: return AdaptiveOrder;
        case 15: return LeidenOrder;
        case 16: return GoGraphOrder;
        default:
            std::cerr << "Unknown algorithm ID: " << value << std::endl;
            std::abort();
    }
}

/**
 * @brief Convert string argument to ReorderingAlgo enum
 * 
 * Parses command-line format: "algo_id[:param1[:param2...]]"
 * 
 * @param arg Command line argument string
 * @return Corresponding ReorderingAlgo enum value
 */
inline ReorderingAlgo getReorderingAlgo(const char* arg) {
    // Parse "algo_id:options" format
    std::string s(arg);
    std::string algo_str = s.substr(0, s.find(':'));
    int algo_id = std::stoi(algo_str);
    return getReorderingAlgo(algo_id);
}

// ============================================================================
// UNIFIED REORDER CONFIGURATION
// ============================================================================
// All reordering algorithms (GraphBrew, Leiden, RabbitOrder, Adaptive)
// should use these unified defaults for consistency and fair comparison.
//
// Design rationale:
//   - Canonical default parameters
//   - Consistent behavior across all community-detection-based algorithms
//   - Easy to modify defaults in one place
//   - Supports both fixed and auto-computed resolution
// ============================================================================

/**
 * @brief Unified default constants for all reordering algorithms
 * 
 * These values are used by GraphBrew, Leiden, RabbitOrder, and Adaptive.
 * They provide consistency across all community-based reordering algorithms.
 */
namespace reorder {

// Resolution parameter (modularity): Controls community granularity
// - Lower values → larger communities (coarser)
// - Higher values → smaller communities (finer)
// - 1.0 is standard modularity, <1.0 favors larger communities
constexpr double DEFAULT_RESOLUTION = 1.0;      ///< Standard modularity resolution

// Convergence thresholds
constexpr double DEFAULT_TOLERANCE = 1e-2;      ///< Node movement convergence
constexpr double DEFAULT_AGGREGATION_TOLERANCE = 0.8;  ///< When to stop aggregating
constexpr double DEFAULT_TOLERANCE_DROP = 10.0; ///< Tolerance reduction factor per pass

// Iteration limits
constexpr int DEFAULT_MAX_ITERATIONS = 10;      ///< Max iterations per pass (local moving)
constexpr int DEFAULT_MAX_PASSES = 10;          ///< Max aggregation passes

// Cache optimization
constexpr size_t DEFAULT_TILE_SIZE = 4096;      ///< Tile size for cache blocking
constexpr size_t DEFAULT_PREFETCH_DISTANCE = 8; ///< Prefetch lookahead

/**
 * @brief Resolution mode for community-based algorithms
 */
enum class ResolutionMode {
    FIXED,      ///< Use user-specified fixed value
    AUTO,       ///< Compute from graph properties (density-based)
    DYNAMIC     ///< Adjust per-pass based on runtime metrics
};

/**
 * @brief Ordering strategy for final vertex ordering
 */
enum class OrderingStrategy {
    HIERARCHICAL,   ///< Sort communities then by degree within (default)
    DFS,            ///< DFS traversal of community hierarchy
    BFS,            ///< BFS traversal of community hierarchy
    DBG,            ///< Degree-Based Grouping within communities
    CORDER,         ///< COrder-style cache-aware ordering
    HUB_CLUSTER,    ///< Hub clustering with community awareness
    COMMUNITY_SORT  ///< Simple community-then-node-id sort
};

/**
 * @brief Aggregation strategy for multi-level algorithms
 */
enum class AggregationStrategy {
    CSR_BUFFER,     ///< Standard CSR-based aggregation (Leiden-style)
    LAZY_STREAMING, ///< Lazy aggregation (RabbitOrder-style)
    HYBRID,         ///< Auto-select based on graph density
    GVE_CSR_MERGE   ///< GVE-style: explicit adjacency + super-graph local-moving merge
};

/**
 * @brief Unified configuration for all reordering algorithms
 * 
 * This structure provides a consistent interface for configuring any
 * community-detection-based reordering algorithm in GraphBrew.
 * 
 * Usage:
 *   ReorderConfig cfg;
 *   cfg.resolution = 0.75;
 *   cfg.maxIterations = 20;
 *   // Pass to any algorithm: GraphBrew, Leiden, etc.
 */
struct ReorderConfig {
    // ========================================================================
    // RESOLUTION PARAMETERS
    // ========================================================================
    ResolutionMode resolutionMode = ResolutionMode::AUTO;  ///< How resolution is determined
    double resolution = DEFAULT_RESOLUTION;     ///< Modularity resolution parameter
    double initialResolution = DEFAULT_RESOLUTION;  ///< Initial value for dynamic mode
    
    // ========================================================================
    // CONVERGENCE PARAMETERS
    // ========================================================================
    double tolerance = DEFAULT_TOLERANCE;       ///< Node movement convergence threshold
    double aggregationTolerance = DEFAULT_AGGREGATION_TOLERANCE; ///< Aggregation stop threshold
    double toleranceDrop = DEFAULT_TOLERANCE_DROP;  ///< Tolerance reduction per pass
    
    // ========================================================================
    // ITERATION LIMITS
    // ========================================================================
    int maxIterations = DEFAULT_MAX_ITERATIONS; ///< Max iterations per pass
    int maxPasses = DEFAULT_MAX_PASSES;         ///< Max aggregation passes
    
    // ========================================================================
    // ALGORITHM SELECTION
    // ========================================================================
    OrderingStrategy ordering = OrderingStrategy::HIERARCHICAL;
    AggregationStrategy aggregation = AggregationStrategy::CSR_BUFFER;
    
    // ========================================================================
    // FEATURE FLAGS
    // ========================================================================
    bool useRefinement = true;      ///< Enable Leiden refinement step
    bool usePrefetch = true;        ///< Enable cache prefetching
    bool useParallelSort = true;    ///< Use parallel sorting
    bool verifyTopology = false;    ///< Verify topology after reordering
    bool verbose = false;           ///< Print debug/progress info
    
    // ========================================================================
    // CACHE OPTIMIZATION
    // ========================================================================
    size_t tileSize = DEFAULT_TILE_SIZE;           ///< Tile size for cache blocking
    size_t prefetchDistance = DEFAULT_PREFETCH_DISTANCE;  ///< Prefetch lookahead
    
    // ========================================================================
    // HELPER METHODS
    // ========================================================================
    
    /// Check if dynamic resolution mode
    bool isDynamic() const { return resolutionMode == ResolutionMode::DYNAMIC; }
    
    /// Check if auto resolution mode
    bool isAuto() const { return resolutionMode == ResolutionMode::AUTO; }
    
    /// Get effective resolution (for non-dynamic algorithms)
    double getResolution() const {
        return isDynamic() ? initialResolution : resolution;
    }
    
    /// Apply auto-resolution from graph properties
    template <typename NodeID_T, typename DestID_T>
    void applyAutoResolution(const CSRGraph<NodeID_T, DestID_T, true>& g) {
        if (resolutionMode == ResolutionMode::AUTO) {
            resolution = computeGraphAdaptiveResolution(g);
        } else if (resolutionMode == ResolutionMode::DYNAMIC) {
            initialResolution = computeGraphAdaptiveResolution(g);
        }
    }
    
    /// Compute graph-adaptive resolution based on density and degree distribution
    template <typename NodeID_T, typename DestID_T>
    static double computeGraphAdaptiveResolution(const CSRGraph<NodeID_T, DestID_T, true>& g) {
        const int64_t n = g.num_nodes();
        const int64_t m = g.num_edges();
        
        if (n == 0) return DEFAULT_RESOLUTION;
        
        double avg_degree = static_cast<double>(m) / n;
        
        // Compute coefficient of variation for degree distribution
        double sum_sq = 0.0;
        #pragma omp parallel for reduction(+:sum_sq)
        for (int64_t v = 0; v < n; ++v) {
            double d = static_cast<double>(g.out_degree(v));
            double diff = d - avg_degree;
            sum_sq += diff * diff;
        }
        double variance = sum_sq / n;
        double cv = (avg_degree > 0) ? std::sqrt(variance) / avg_degree : 0.0;
        
        // Base resolution from average degree
        double res = 0.5 + 0.25 * std::log10(avg_degree + 1);
        res = std::max(0.5, std::min(1.2, res));
        
        // Adjust for power-law/hubby graphs (high CV)
        if (cv > 2.0) {
            double factor = 2.0 / std::max(2.0, std::sqrt(cv));
            res = std::max(0.5, res * factor);
        }
        
        return res;
    }
    
    /**
     * @brief Parse configuration from command-line options
     * 
     * Supports formats:
     *   - Numeric: resolution, iterations, passes (positional)
     *   - Keywords: auto, dynamic, dfs, bfs, dbg, corder, streaming, norefine
     * 
     * Example: {"0.75", "20", "10"} → resolution=0.75, iters=20, passes=10
     * Example: {"auto", "dfs"} → auto-resolution, DFS ordering
     */
    static ReorderConfig FromOptions(const std::vector<std::string>& options) {
        ReorderConfig cfg;
        
        for (const auto& opt : options) {
            if (opt.empty()) continue;
            
            // Resolution mode keywords
            if (opt == "auto" || opt == "0") {
                cfg.resolutionMode = ResolutionMode::AUTO;
            } else if (opt == "dynamic") {
                cfg.resolutionMode = ResolutionMode::DYNAMIC;
            }
            // Ordering keywords
            else if (opt == "dfs") {
                cfg.ordering = OrderingStrategy::DFS;
            } else if (opt == "bfs") {
                cfg.ordering = OrderingStrategy::BFS;
            } else if (opt == "dbg") {
                cfg.ordering = OrderingStrategy::DBG;
            } else if (opt == "corder") {
                cfg.ordering = OrderingStrategy::CORDER;
            } else if (opt == "hubcluster" || opt == "hub") {
                cfg.ordering = OrderingStrategy::HUB_CLUSTER;
            } else if (opt == "community" || opt == "sort") {
                cfg.ordering = OrderingStrategy::COMMUNITY_SORT;
            }
            // Aggregation keywords
            else if (opt == "streaming" || opt == "lazy") {
                cfg.aggregation = AggregationStrategy::LAZY_STREAMING;
            } else if (opt == "hybrid") {
                cfg.aggregation = AggregationStrategy::HYBRID;
            }
            // Feature flags
            else if (opt == "norefine") {
                cfg.useRefinement = false;
            } else if (opt == "verify") {
                cfg.verifyTopology = true;
            } else if (opt == "verbose") {
                cfg.verbose = true;
            }
            // Numeric values: resolution, iterations, passes
            else {
                try {
                    double val = std::stod(opt);
                    if (val > 0 && val <= 3 && (opt.find('.') != std::string::npos || val < 1)) {
                        cfg.resolution = val;
                        cfg.resolutionMode = ResolutionMode::FIXED;
                    } else if (val >= 1 && val <= 100) {
                        int intVal = static_cast<int>(val);
                        if (cfg.maxIterations == DEFAULT_MAX_ITERATIONS) {
                            cfg.maxIterations = intVal;
                        } else {
                            cfg.maxPasses = intVal;
                        }
                    } else if (val > 0 && val <= 3) {
                        cfg.resolution = val;
                        cfg.resolutionMode = ResolutionMode::FIXED;
                    }
                } catch (...) {
                    // Ignore parsing errors
                }
            }
        }
        
        return cfg;
    }
    
    /// Print configuration summary
    void print() const {
        printf("ReorderConfig: resolution=%.4f (%s), iters=%d, passes=%d\n",
               resolution,
               resolutionMode == ResolutionMode::AUTO ? "auto" :
               resolutionMode == ResolutionMode::DYNAMIC ? "dynamic" : "fixed",
               maxIterations, maxPasses);
        printf("  ordering=%s, aggregation=%s, refinement=%s\n",
               ordering == OrderingStrategy::HIERARCHICAL ? "hierarchical" :
               ordering == OrderingStrategy::DFS ? "dfs" :
               ordering == OrderingStrategy::BFS ? "bfs" :
               ordering == OrderingStrategy::DBG ? "dbg" :
               ordering == OrderingStrategy::CORDER ? "corder" : "other",
               aggregation == AggregationStrategy::CSR_BUFFER ? "csr" :
               aggregation == AggregationStrategy::LAZY_STREAMING ? "streaming" : "hybrid",
               useRefinement ? "on" : "off");
    }
};

} // namespace reorder

// ============================================================================
// STANDALONE GRAPH BUILDING UTILITIES
// ============================================================================
// These functions enable building CSR graphs from edge lists without
// requiring a BuilderBase instance. Used by extracted reordering functions.

/**
 * @brief Helper to extract NodeID from DestID_ (handles both weighted and unweighted)
 * 
 * For unweighted graphs: DestID_ == NodeID_, just return the value
 * For weighted graphs: DestID_ == NodeWeight<NodeID_, WeightT_>, extract the .v member
 */
template <typename NodeID_, typename DestID_>
inline NodeID_ GetNodeID(const DestID_& dest) {
    if constexpr (std::is_same_v<NodeID_, DestID_>) {
        // Unweighted: DestID_ == NodeID_
        return dest;
    } else {
        // Weighted: DestID_ == NodeWeight<NodeID_, WeightT_>
        return dest.v;
    }
}

// Trait to extract WeightT_ from DestID_; defaults to NodeID_ for unweighted graphs
template <typename DestID_, typename NodeID_>
struct WeightTFromDestID {
    using type = NodeID_;
};

template <typename NodeID_, typename WeightT_>
struct WeightTFromDestID<NodeWeight<NodeID_, WeightT_>, NodeID_> {
    using type = WeightT_;
};

// Helper to extract edge weight as double
template <typename DestID_>
inline double GetWeight(const DestID_&) {
    return 1.0;
}

template <typename NodeID_, typename WeightT_>
inline double GetWeight(const NodeWeight<NodeID_, WeightT_>& dest) {
    return static_cast<double>(dest.w);
}

/**
 * @brief Find maximum node ID in edge list (standalone)
 */
template <typename NodeID_, typename DestID_>
NodeID_ FindMaxNodeIDStandalone(const std::vector<std::pair<NodeID_, DestID_>>& el) {
    NodeID_ max_seen = 0;
    #pragma omp parallel for reduction(max : max_seen)
    for (size_t i = 0; i < el.size(); ++i) {
        max_seen = std::max(max_seen, el[i].first);
        max_seen = std::max(max_seen, GetNodeID<NodeID_, DestID_>(el[i].second));
    }
    return max_seen;
}

/**
 * @brief Count degrees from edge list (standalone)
 */
template <typename NodeID_, typename DestID_>
pvector<NodeID_> CountLocalDegreesStandalone(
    const std::vector<std::pair<NodeID_, DestID_>>& el, 
    bool transpose, 
    int64_t num_nodes_local) 
{
    pvector<NodeID_> degrees(num_nodes_local, 0);
    #pragma omp parallel for
    for (size_t i = 0; i < el.size(); ++i) {
        if (!transpose)
            fetch_and_add(degrees[el[i].first], 1);
        else
            fetch_and_add(degrees[GetNodeID<NodeID_, DestID_>(el[i].second)], 1);
    }
    return degrees;
}

/**
 * @brief Parallel prefix sum (standalone)
 */
template <typename NodeID_>
pvector<SGOffset> ParallelPrefixSumStandalone(const pvector<NodeID_>& degrees) {
    const size_t block_size = 1 << 20;
    const size_t num_blocks = (degrees.size() + block_size - 1) / block_size;
    pvector<SGOffset> local_sums(num_blocks);
    pvector<SGOffset> bulk_prefix(num_blocks + 1);
    pvector<SGOffset> sums(degrees.size() + 1);
    
    #pragma omp parallel for
    for (size_t block = 0; block < num_blocks; block++) {
        SGOffset lsum = 0;
        size_t block_end = std::min((block + 1) * block_size, degrees.size());
        for (size_t i = block * block_size; i < block_end; i++)
            lsum += degrees[i];
        local_sums[block] = lsum;
    }
    
    bulk_prefix[0] = 0;
    for (size_t block = 0; block < num_blocks; block++)
        bulk_prefix[block + 1] = bulk_prefix[block] + local_sums[block];
    
    #pragma omp parallel for
    for (size_t block = 0; block < num_blocks; block++) {
        SGOffset local_total = bulk_prefix[block];
        size_t block_end = std::min((block + 1) * block_size, degrees.size());
        for (size_t i = block * block_size; i < block_end; i++) {
            sums[i] = local_total;
            local_total += degrees[i];
        }
    }
    sums[degrees.size()] = bulk_prefix[num_blocks];
    return sums;
}

/**
 * @brief Squish CSR to remove gaps (standalone)
 */
template <typename NodeID_, typename DestID_, bool invert>
void SquishCSRStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g, 
    bool transpose,
    DestID_*** index, 
    DestID_** neighs) 
{
    int64_t num_nodes = g.num_nodes();
    pvector<NodeID_> degrees(num_nodes);
    
    #pragma omp parallel for
    for (NodeID_ n = 0; n < num_nodes; n++) {
        degrees[n] = transpose ? g.in_degree(n) : g.out_degree(n);
    }
    
    pvector<SGOffset> offsets = ParallelPrefixSumStandalone(degrees);
    *neighs = new DestID_[offsets[num_nodes]];
    *index = CSRGraph<NodeID_, DestID_>::GenIndex(offsets, *neighs);
    
    #pragma omp parallel for
    for (NodeID_ n = 0; n < num_nodes; n++) {
        if (transpose) {
            for (DestID_ neighbor : g.in_neigh(n))
                (*neighs)[offsets[n]++] = neighbor;
        } else {
            for (DestID_ neighbor : g.out_neigh(n))
                (*neighs)[offsets[n]++] = neighbor;
        }
    }
}

/**
 * @brief Squish graph to remove gaps (standalone)
 */
template <typename NodeID_, typename DestID_, bool invert>
CSRGraph<NodeID_, DestID_, invert> SquishGraphStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g) 
{
    DestID_ **out_index, *out_neighs;
    SquishCSRStandalone<NodeID_, DestID_, invert>(g, false, &out_index, &out_neighs);
    
    if (g.directed() && invert) {
        DestID_ **in_index, *in_neighs;
        SquishCSRStandalone<NodeID_, DestID_, invert>(g, true, &in_index, &in_neighs);
        return CSRGraph<NodeID_, DestID_, invert>(
            g.num_nodes(), out_index, out_neighs, in_index, in_neighs);
    }
    return CSRGraph<NodeID_, DestID_, invert>(g.num_nodes(), out_index, out_neighs);
}

/**
 * @brief Build CSR graph from edge list (standalone)
 * 
 * This is a standalone version of MakeLocalGraphFromEL that doesn't
 * require a BuilderBase instance.
 */
template <typename NodeID_, typename DestID_, bool invert>
CSRGraph<NodeID_, DestID_, invert> MakeLocalGraphFromELStandalone(
    std::vector<std::pair<NodeID_, DestID_>>& el, 
    bool verbose = false) 
{
    Timer t;
    t.Start();
    
    if (el.empty()) {
        return CSRGraph<NodeID_, DestID_, invert>(0, nullptr, nullptr);
    }
    
    int64_t num_nodes_local = FindMaxNodeIDStandalone<NodeID_, DestID_>(el) + 1;
    pvector<NodeID_> degrees = CountLocalDegreesStandalone<NodeID_, DestID_>(el, false, num_nodes_local);
    pvector<SGOffset> offsets = ParallelPrefixSumStandalone(degrees);
    
    DestID_* neighs = new DestID_[offsets[num_nodes_local]];
    DestID_** index = CSRGraph<NodeID_, DestID_>::GenIndex(offsets, neighs);
    
    #pragma omp parallel for
    for (size_t i = 0; i < el.size(); ++i) {
        neighs[fetch_and_add(offsets[el[i].first], 1)] = el[i].second;
    }
    
    CSRGraph<NodeID_, DestID_, invert> g(num_nodes_local, index, neighs);
    g = SquishGraphStandalone<NodeID_, DestID_, invert>(g);
    
    t.Stop();
    if (verbose) {
        PrintTime("Local Build Time", t.Seconds());
    }
    return g;
}

/**
 * @brief Convert EdgeList to simple pair vector for standalone processing
 */
template <typename NodeID_, typename DestID_>
std::vector<std::pair<NodeID_, DestID_>> EdgeListToVector(
    const std::vector<std::pair<NodeID_, DestID_>>& el) 
{
    return el;  // Already in the right format
}

// ============================================================================
// CSR GRAPH RELABELING (STANDALONE)
// ============================================================================

/**
 * @brief Relabel graph vertices according to a new ID mapping (standalone)
 * 
 * Creates a new CSR graph where vertices are renumbered according to new_ids.
 * Handles both directed and undirected graphs. For directed graphs, maintains
 * both in-neighbor and out-neighbor CSR arrays.
 * 
 * This is a standalone version that doesn't require a BuilderBase instance.
 * 
 * @tparam NodeID_ Node identifier type
 * @tparam DestID_ Destination type (NodeID_ for unweighted, NodeWeight for weighted)
 * @tparam invert Whether to invert edge direction
 * @param g Source graph to relabel
 * @param new_ids Mapping where new_ids[v] = new ID for vertex v (-1 means assign next available)
 * @return New graph with relabeled vertices
 */
template <typename NodeID_, typename DestID_, bool invert>
CSRGraph<NodeID_, DestID_, invert> RelabelByMappingStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g,
    pvector<NodeID_>& new_ids) 
{
    Timer t;
    t.Start();
    
    bool outDegree = true;
    CSRGraph<NodeID_, DestID_, invert> g_relabel;

    // Find max assigned ID, excluding unmapped (-1) entries.
    // Bug fix: the old code ran max_element over the entire array including
    // sentinel -1 values, which for unsigned NodeID_ is UINT_MAX, causing
    // subsequent IDs to overflow.
    const NodeID_ sentinel = static_cast<NodeID_>(-1);
    size_t max_id = 0;
    #pragma omp parallel for reduction(max:max_id)
    for (NodeID_ v = 0; v < g.num_nodes(); ++v) {
        if (new_ids[v] != sentinel && static_cast<size_t>(new_ids[v]) > max_id) {
            max_id = static_cast<size_t>(new_ids[v]);
        }
    }
    // Start assigning unmapped IDs after the current maximum
    max_id += 1;

    #pragma omp parallel for
    for (NodeID_ v = 0; v < g.num_nodes(); ++v) {
        if (new_ids[v] == sentinel) {
            // Assign new IDs atomically starting from max_id
            size_t local_id = __sync_fetch_and_add(&max_id, 1);
            new_ids[v] = static_cast<NodeID_>(local_id);
        }
    }

    if (g.directed()) {
        // Directed graph: build both in-neighbor and out-neighbor CSRs
        
        #pragma omp parallel for
        for (NodeID_ v = 0; v < g.num_nodes(); ++v)
            assert(new_ids[v] != static_cast<NodeID_>(-1));

        // Compute degrees in new ordering
        pvector<NodeID_> degrees(g.num_nodes());
        pvector<NodeID_> inv_degrees(g.num_nodes());
        
        if (outDegree) {
            #pragma omp parallel for
            for (NodeID_ n = 0; n < g.num_nodes(); n++) {
                degrees[new_ids[n]] = g.out_degree(n);
                inv_degrees[new_ids[n]] = g.in_degree(n);
            }
        } else {
            #pragma omp parallel for
            for (NodeID_ n = 0; n < g.num_nodes(); n++) {
                degrees[new_ids[n]] = g.in_degree(n);
                inv_degrees[new_ids[n]] = g.out_degree(n);
            }
        }

        // Build in-neighbor CSR (transpose)
        pvector<SGOffset> offsets = ParallelPrefixSumStandalone(inv_degrees);
        DestID_* neighs = new DestID_[offsets[g.num_nodes()]];
        DestID_** index = CSRGraph<NodeID_, DestID_>::GenIndex(offsets, neighs);
        
        #pragma omp parallel for schedule(dynamic, 1024)
        for (NodeID_ u = 0; u < g.num_nodes(); u++) {
            if (outDegree) {
                for (NodeID_ v : g.in_neigh(u))
                    neighs[offsets[new_ids[u]]++] = new_ids[v];
            } else {
                for (NodeID_ v : g.out_neigh(u))
                    neighs[offsets[new_ids[u]]++] = new_ids[v];
            }
            std::sort(index[new_ids[u]], index[new_ids[u] + 1]);
        }

        // Build out-neighbor CSR
        pvector<SGOffset> inv_offsets = ParallelPrefixSumStandalone(degrees);
        DestID_* inv_neighs = new DestID_[inv_offsets[g.num_nodes()]];
        DestID_** inv_index = CSRGraph<NodeID_, DestID_>::GenIndex(inv_offsets, inv_neighs);
        
        #pragma omp parallel for schedule(dynamic, 1024)
        for (NodeID_ u = 0; u < g.num_nodes(); u++) {
            if (outDegree) {
                for (NodeID_ v : g.out_neigh(u))
                    inv_neighs[inv_offsets[new_ids[u]]++] = new_ids[v];
            } else {
                for (NodeID_ v : g.in_neigh(u))
                    inv_neighs[inv_offsets[new_ids[u]]++] = new_ids[v];
            }
            std::sort(inv_index[new_ids[u]], inv_index[new_ids[u] + 1]);
        }

        t.Stop();
        PrintTime("Relabel Map Time", t.Seconds());
        
        if (outDegree) {
            g_relabel = CSRGraph<NodeID_, DestID_, invert>(
                g.num_nodes(), inv_index, inv_neighs, index, neighs);
        } else {
            g_relabel = CSRGraph<NodeID_, DestID_, invert>(
                g.num_nodes(), index, neighs, inv_index, inv_neighs);
        }
    } else {
        // Undirected graph: single CSR for both directions
        
        #pragma omp parallel for
        for (NodeID_ v = 0; v < g.num_nodes(); ++v)
            assert(new_ids[v] != static_cast<NodeID_>(-1));

        // Compute degrees in new ordering
        pvector<NodeID_> degrees(g.num_nodes());
        #pragma omp parallel for
        for (NodeID_ n = 0; n < g.num_nodes(); n++) {
            degrees[new_ids[n]] = g.out_degree(n);
        }

        // Build CSR
        pvector<SGOffset> offsets = ParallelPrefixSumStandalone(degrees);
        DestID_* neighs = new DestID_[offsets[g.num_nodes()]];
        DestID_** index = CSRGraph<NodeID_, DestID_>::GenIndex(offsets, neighs);
        
        #pragma omp parallel for schedule(dynamic, 1024)
        for (NodeID_ u = 0; u < g.num_nodes(); u++) {
            for (NodeID_ v : g.out_neigh(u))
                neighs[offsets[new_ids[u]]++] = new_ids[v];
            std::sort(index[new_ids[u]], index[new_ids[u] + 1]);
        }

        t.Stop();
        PrintTime("Relabel Map Time", t.Seconds());
        g_relabel = CSRGraph<NodeID_, DestID_, invert>(g.num_nodes(), index, neighs);
    }

    // Copy and update original ID tracking
    g_relabel.copy_org_ids(g.get_org_ids());
    g_relabel.update_org_ids(new_ids);
    return g_relabel;
}

// ============================================================================
// RESULT STRUCTURES
// ============================================================================

/**
 * @brief Result structure for GVE-Leiden community detection
 * 
 * Contains the final community assignments, quality metrics, and
 * iteration statistics from the Leiden algorithm execution.
 * 
 * @tparam K Community ID type (default: uint32_t)
 */
template <typename K = uint32_t>
struct GVELeidenResult {
    // ---------- Community Assignment ----------
    /** Final community assignment: final_community[v] = community ID of vertex v */
    std::vector<K> final_community;
    
    /** Community assignments per pass for hierarchical structure */
    std::vector<std::vector<K>> community_per_pass;
    
    // ---------- Quality Metrics ----------
    /** Final modularity score (higher = better community structure) */
    double modularity = 0.0;
    
    // ---------- Iteration Statistics ----------
    /** Total local-moving iterations across all passes */
    int total_iterations = 0;
    
    /** Number of aggregation passes performed */
    int total_passes = 0;
};

/**
 * @brief Dendrogram result with explicit tree structure
 * 
 * Uses parent/child/sibling pointers for efficient tree traversal.
 * This is more memory-efficient than storing community_per_pass.
 * 
 * @tparam K Community ID type (default: uint32_t)
 */
template <typename K = uint32_t>
struct GVEDendroResult {
    std::vector<K> final_community;        ///< Final community assignment
    std::vector<int64_t> parent;           ///< Dendrogram parent (-1 = root)
    std::vector<int64_t> first_child;      ///< First child (-1 = leaf)
    std::vector<int64_t> sibling;          ///< Next sibling at same level (-1 = last)
    std::vector<int64_t> subtree_size;     ///< Size of subtree rooted here
    std::vector<double> weight;            ///< Node weight (degree)
    std::vector<int64_t> roots;            ///< Root nodes (top-level communities)
    int total_iterations = 0;
    int total_passes = 0;
    double modularity = 0.0;
};

/**
 * @brief Atomic dendrogram result for lock-free parallel building
 * 
 * Uses atomic operations for concurrent dendrogram construction.
 * Based on RabbitOrder's lock-free merge approach.
 * 
 * @tparam K Community ID type (default: uint32_t)
 */
template <typename K = uint32_t>
struct GVEAtomicDendroResult {
    std::vector<K> final_community;
    std::unique_ptr<std::atomic<int64_t>[]> parent;
    std::unique_ptr<std::atomic<int64_t>[]> first_child;
    std::unique_ptr<std::atomic<int64_t>[]> sibling;
    std::unique_ptr<std::atomic<int64_t>[]> subtree_size;
    std::vector<double> weight;
    std::vector<int64_t> roots;
    int64_t num_nodes = 0;
    int total_iterations = 0;
    int total_passes = 0;
    double modularity = 0.0;
    
    GVEAtomicDendroResult() = default;
    
    /** Initialize atomic dendrogram for n vertices */
    void init(int64_t n, const std::vector<double>& vtot) {
        num_nodes = n;
        final_community.resize(n);
        parent = std::make_unique<std::atomic<int64_t>[]>(n);
        first_child = std::make_unique<std::atomic<int64_t>[]>(n);
        sibling = std::make_unique<std::atomic<int64_t>[]>(n);
        subtree_size = std::make_unique<std::atomic<int64_t>[]>(n);
        weight.resize(n);
        
        #pragma omp parallel for
        for (int64_t v = 0; v < n; ++v) {
            final_community[v] = static_cast<K>(v);
            parent[v].store(-1, std::memory_order_relaxed);
            first_child[v].store(-1, std::memory_order_relaxed);
            sibling[v].store(-1, std::memory_order_relaxed);
            subtree_size[v].store(1, std::memory_order_relaxed);
            weight[v] = vtot[v];
        }
        
        total_iterations = 0;
        total_passes = 0;
        modularity = 0.0;
    }
    
    /** Convert atomic result to non-atomic for traversal */
    GVEDendroResult<K> toNonAtomic() const {
        GVEDendroResult<K> result;
        
        result.final_community = final_community;
        result.parent.resize(num_nodes);
        result.first_child.resize(num_nodes);
        result.sibling.resize(num_nodes);
        result.subtree_size.resize(num_nodes);
        result.weight = weight;
        result.total_iterations = total_iterations;
        result.total_passes = total_passes;
        result.modularity = modularity;
        
        #pragma omp parallel for
        for (int64_t v = 0; v < num_nodes; ++v) {
            result.parent[v] = parent[v].load(std::memory_order_relaxed);
            result.first_child[v] = first_child[v].load(std::memory_order_relaxed);
            result.sibling[v] = sibling[v].load(std::memory_order_relaxed);
            result.subtree_size[v] = subtree_size[v].load(std::memory_order_relaxed);
        }
        
        // Collect roots
        for (int64_t v = 0; v < num_nodes; ++v) {
            if (result.parent[v] == -1) {
                result.roots.push_back(v);
            }
        }
        
        return result;
    }
};

/**
 * @brief Result structure for GVE-Rabbit hybrid ordering
 * 
 * Combines GVE-Leiden community detection with RabbitOrder-style
 * dendrogram building for efficient hierarchical ordering.
 * 
 * @tparam K Community ID type (default: uint32_t)
 */
template <typename K = uint32_t>
struct GVERabbitResult {
    std::vector<K> final_community;
    std::vector<int64_t> parent;
    std::vector<int64_t> first_child;
    std::vector<int64_t> sibling;
    std::vector<int64_t> subtree_size;
    std::vector<double> weight;
    std::vector<int64_t> roots;
    int total_iterations = 0;
    double modularity = 0.0;
};

// ============================================================================
// DENDROGRAM HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Lock-free atomic merge: Prepend vertex v to u's child list using CAS
 * 
 * This is the core RabbitOrder-style operation for building dendrograms.
 * Uses compare-and-swap for thread-safe concurrent construction.
 * 
 * @tparam K Community ID type
 * @param dendro The atomic dendrogram structure
 * @param v Vertex being merged (child)
 * @param u Community representative (parent)
 * @return true if merge was performed, false if skipped
 */
template <typename K = uint32_t>
inline bool atomicMergeToDendro(
    GVEAtomicDendroResult<K>& dendro,
    int64_t v,
    int64_t u) {
    
    if (v == u) return false;
    if (u < 0 || u >= dendro.num_nodes) return false;
    
    // Skip if v already has a parent
    if (dendro.parent[v].load(std::memory_order_acquire) != -1) {
        return false;
    }
    
    // Try to claim v by setting its parent to u
    int64_t expected_parent = -1;
    if (!dendro.parent[v].compare_exchange_strong(expected_parent, u,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    
    // Successfully claimed v - add to u's child list with CAS loop
    int64_t old_first_child = dendro.first_child[u].load(std::memory_order_acquire);
    do {
        dendro.sibling[v].store(old_first_child, std::memory_order_relaxed);
    } while (!dendro.first_child[u].compare_exchange_weak(old_first_child, v,
                std::memory_order_acq_rel, std::memory_order_acquire));
    
    // Update subtree size (atomic add)
    int64_t v_size = dendro.subtree_size[v].load(std::memory_order_relaxed);
    dendro.subtree_size[u].fetch_add(v_size, std::memory_order_relaxed);
    
    return true;
}

/**
 * @brief Non-atomic merge for sequential dendrogram building
 * 
 * Simpler version without atomic operations for single-threaded use.
 * 
 * @tparam K Community ID type
 * @param dendro The dendrogram result structure
 * @param v Vertex being merged (child)
 * @param u Community representative (parent)
 * @return true if merge was performed
 */
template <typename K = uint32_t>
inline bool mergeToDendro(
    GVEDendroResult<K>& dendro,
    int64_t v,
    int64_t u) {
    
    if (v == u) return false;
    if (dendro.parent[v] != -1) return false;
    
    dendro.parent[v] = u;
    dendro.sibling[v] = dendro.first_child[u];
    dendro.first_child[u] = v;
    dendro.subtree_size[u] += dendro.subtree_size[v];
    
    return true;
}

/**
 * @brief Initialize dendrogram with leaf nodes
 * 
 * Each vertex starts as its own community (leaf node) with
 * no parent, children, or siblings.
 * 
 * @tparam K Community ID type
 * @param dendro The dendrogram result structure to initialize
 * @param num_nodes Number of vertices
 * @param vtot Total weight (degree) of each vertex
 */
template <typename K = uint32_t>
inline void initDendrogram(
    GVEDendroResult<K>& dendro,
    const int64_t num_nodes,
    const std::vector<double>& vtot) {
    
    dendro.parent.resize(num_nodes, -1);
    dendro.first_child.resize(num_nodes, -1);
    dendro.sibling.resize(num_nodes, -1);
    dendro.subtree_size.resize(num_nodes, 1);
    dendro.weight.resize(num_nodes);
    
    #pragma omp parallel for
    for (int64_t v = 0; v < num_nodes; ++v) {
        dendro.weight[v] = vtot[v];
    }
}

/**
 * @brief Features computed for a community/subgraph
 * 
 * Used by AdaptiveOrder to select the best reordering algorithm
 * for each community based on its structural properties.
 */
struct CommunityFeatures {
    // ---------- Size Metrics ----------
    size_t num_nodes = 0;           ///< Number of vertices in community
    size_t num_edges = 0;           ///< Number of internal edges
    
    // ---------- Density Metrics ----------
    double internal_density = 0.0;  ///< edges / possible_edges
    double avg_degree = 0.0;        ///< Mean degree of vertices
    double degree_variance = 0.0;   ///< Normalized variance in degrees
    
    // ---------- Structure Metrics ----------
    double hub_concentration = 0.0;  ///< Fraction of edges from top 10% nodes
    double modularity = 0.0;         ///< Community/subgraph modularity score
    double clustering_coeff = 0.0;   ///< Local clustering coefficient (sampled)
    
    // ---------- Path Metrics ----------
    double avg_path_length = 0.0;    ///< Estimated average path length
    double diameter_estimate = 0.0;  ///< Estimated graph diameter
    
    // ---------- Hierarchy Metrics ----------
    double community_count = 0.0;    ///< Number of sub-communities detected
    
    // ---------- Performance Metrics ----------
    double reorder_time = 0.0;       ///< Estimated reordering time (if known)
    
    // ---------- Locality Metrics (IISWC'18 / GoGraph) ----------
    double packing_factor = 0.0;     ///< Fraction of hub neighbors already co-located (nearby IDs)
    double forward_edge_fraction = 0.0; ///< Fraction of edges (u,v) where ID(u) < ID(v)
    
    // ---------- System-Cache Metrics (P-OPT) ----------
    double working_set_ratio = 0.0;  ///< graph_bytes / LLC_size (>1 = exceeds cache)
    double wsr_l1 = 0.0;             ///< graph_bytes / L1_size (per-level cache geometry)
    double wsr_l2 = 0.0;             ///< graph_bytes / L2_size (per-level cache geometry)
    
    // ---------- DON-RL Features (Zhao et al.) ----------
    double vertex_significance_skewness = 0.0; ///< Skewness of per-vertex locality contributions (CV)
    double window_neighbor_overlap = 0.0;      ///< Mean fraction of neighbors within locality window
    
    // ---------- Sampled Locality Score (P1 3.1d) ----------
    double sampled_locality_score = 0.0;       ///< Sampled F(σ) approximation — baseline ordering quality
    // ---------- Transpose Reuse Distance (P3 3.2) ----------
    double avg_reuse_distance = 0.0;           ///< Mean next-reuse distance from transpose (CSC)
    // ---- Paper-aligned feature variants ----
    double packing_factor_cl = 0.0;            ///< IISWC'18 cache-line packing
    double locality_score_pairwise = 0.0;      ///< DON-RL pairwise F(σ)
    double reuse_distance_lru = 0.0;           ///< P-OPT LRU stack distance
};

/**
 * @brief Dendrogram node for hierarchical community structure
 * 
 * Used internally by GraphBrew to build and traverse the community
 * hierarchy for ordering vertices.
 */
struct LeidenDendrogramNode {
    int64_t parent = -1;       ///< Parent node in dendrogram (-1 for root)
    int64_t first_child = -1;  ///< First child node (-1 for leaf)
    int64_t sibling = -1;      ///< Next sibling node (-1 if last)
    int64_t vertex_id = -1;    ///< Original vertex ID (-1 for internal nodes)
    size_t subtree_size = 1;   ///< Number of leaves in subtree
    double weight = 0.0;       ///< Degree sum for this subtree
    int level = 0;             ///< Level in hierarchy (0 = leaf)
    int64_t dfs_start = -1;    ///< Starting DFS position for parallel ordering
    
    LeidenDendrogramNode() = default;
};

// ============================================================================
// BENCHMARK TYPE FOR ALGORITHM SELECTION
// ============================================================================

/**
 * @brief Benchmark types for workload-specific algorithm selection
 * 
 * Different benchmarks have different access patterns:
 * - PR (PageRank): Iterative, benefits from cache locality
 * - BFS: Traversal-heavy, frontier-based access
 * - CC: Union-find based, irregular access
 * - SSSP: Priority queue based
 * - BC: All-pairs traversal
 * - TC: Neighborhood intersection
 * 
 * Use BENCH_GENERIC when the benchmark is unknown or for balanced selection.
 */
enum BenchmarkType {
    BENCH_GENERIC = 0,  ///< Generic/default - no benchmark-specific adjustment
    BENCH_PR,           ///< PageRank (push-based) - iterative, random scatter
    BENCH_BFS,          ///< Breadth-First Search - traversal-heavy
    BENCH_CC,           ///< Connected Components (label propagation) - iterative
    BENCH_SSSP,         ///< Single-Source Shortest Path - priority queue based
    BENCH_BC,           ///< Betweenness Centrality - all-pairs traversal
    BENCH_TC,           ///< Triangle Counting - neighborhood intersection
    BENCH_PR_SPMV,      ///< PageRank (SpMV, pull-based) - row-sequential access
    BENCH_CC_SV         ///< Connected Components (Shiloach-Vishkin) - pointer chasing
};

/**
 * @brief Global benchmark type hint for AdaptiveOrder.
 * 
 * Each benchmark binary sets this before graph construction so that
 * AdaptiveOrder can use benchmark-specific multipliers (bench_pr, bench_bfs, etc.)
 * instead of the generic multiplier (1.0).
 * 
 * Usage:  SetBenchmarkTypeHint(BENCH_BFS);  // in main(), before Builder
 */
inline BenchmarkType& _BenchmarkTypeHint() {
    static BenchmarkType hint = BENCH_GENERIC;
    return hint;
}
inline void SetBenchmarkTypeHint(BenchmarkType t) { _BenchmarkTypeHint() = t; }
inline BenchmarkType GetBenchmarkTypeHint() { return _BenchmarkTypeHint(); }

/**
 * @brief Global graph name hint for AdaptiveOrder.
 *
 * Automatically extracted from the input filename so that
 * database/oracle modes can identify known graphs without
 * the user having to supply the name on the command line.
 *
 * Usage:  SetGraphNameHint("ca-GrQc");  // set in Builder::MakeGraph()
 */
inline std::string& _GraphNameHint() {
    static std::string hint;
    return hint;
}
inline void SetGraphNameHint(const std::string& name) { _GraphNameHint() = name; }
inline const std::string& GetGraphNameHint() { return _GraphNameHint(); }

/**
 * @brief Extract a graph name from a file path.
 *
 * Given "results/graphs/ca-GrQc/ca-GrQc.sg" returns "ca-GrQc".
 * Strips directory prefix and known extensions (.sg, .wsg, .el, .wel, .mtx, .txt, .tsv).
 */
inline std::string ExtractGraphNameFromPath(const std::string& path) {
    // Find last component
    std::string name = path;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // Strip known extensions
    const char* exts[] = {".sg", ".wsg", ".el", ".wel", ".mtx", ".txt", ".tsv"};
    for (auto ext : exts) {
        std::string e(ext);
        if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) {
            name = name.substr(0, name.size() - e.size());
            break;
        }
    }
    return name;
}

/**
 * @brief Convert benchmark name string to enum
 * @param name Benchmark name (e.g., "pr", "bfs", "generic")
 * @return Corresponding BenchmarkType enum value
 */
inline BenchmarkType GetBenchmarkType(const std::string& name) {
    if (name.empty() || name == "generic" || name == "GENERIC" || name == "all") return BENCH_GENERIC;
    if (name == "pr" || name == "PR" || name == "pagerank" || name == "PageRank") return BENCH_PR;
    if (name == "bfs" || name == "BFS") return BENCH_BFS;
    if (name == "cc" || name == "CC") return BENCH_CC;
    if (name == "sssp" || name == "SSSP") return BENCH_SSSP;
    if (name == "bc" || name == "BC") return BENCH_BC;
    if (name == "tc" || name == "TC") return BENCH_TC;
    if (name == "pr_spmv" || name == "PR_SPMV") return BENCH_PR_SPMV;
    if (name == "cc_sv" || name == "CC_SV") return BENCH_CC_SV;
    return BENCH_GENERIC;
}

// ============================================================================
// ALGORITHM NAME MAPPING
// ============================================================================
// Variant names (e.g. GraphBrewOrder_leiden, RABBITORDER_csr, RCM_bnf) are
// resolved dynamically by ResolveVariantSelection() via prefix matching.
// This retained dependency is frozen for ECG graph loading/reordering support;
// C++ recognizes any variant-prefixed name automatically.
//
// Variant prefixes used for dynamic dispatch and weight-file discovery:
constexpr const char* VARIANT_PREFIXES[] = {
    "GOGRAPHORDER_",
    "GraphBrewOrder_",
    "RABBITORDER_",
    "RCM_",
};
constexpr size_t VARIANT_PREFIX_COUNT = sizeof(VARIANT_PREFIXES) / sizeof(VARIANT_PREFIXES[0]);

/**
 * @brief Case-insensitive uppercase helper
 */
inline std::string toUpper(const std::string& s) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), ::toupper);
    return u;
}

/**
 * @brief Base algorithm name map (case-insensitive, UPPERCASE keys only)
 *
 * Maps UPPERCASE base algorithm names to enum values.  Variant-prefixed
 * names like "GraphBrewOrder_leiden" are NOT listed here — they are
 * handled by ResolveVariantSelection() via prefix matching.
 *
 * @return Const reference to static map of UPPERCASE string -> ReorderingAlgo
 */
inline const std::map<std::string, ReorderingAlgo>& getAlgorithmNameMap() {
    static const std::map<std::string, ReorderingAlgo> name_to_algo = {
        {"ORIGINAL",        ORIGINAL},
        {"RANDOM",          Random},
        {"SORT",            Sort},
        {"HUBSORT",         HubSort},
        {"HUBCLUSTER",      HubCluster},
        {"DBG",             DBG},
        {"HUBSORTDBG",      HubSortDBG},
        {"HUBCLUSTERDBG",   HubClusterDBG},
#ifdef RABBIT_ENABLE
        {"RABBITORDER",     RabbitOrder},
#endif
        {"GORDER",          GOrder},
        {"CORDER",          COrder},
        {"RCM",             RCMOrder},
        {"RCMORDER",        RCMOrder},
        {"GRAPHBREWORDER",  GraphBrewOrder},
        {"MAP",             MAP},
        {"ADAPTIVEORDER",   AdaptiveOrder},
        {"LEIDENORDER",     LeidenOrder},
        {"GOGRAPHORDER",    GoGraphOrder},
    };
    return name_to_algo;
}

/**
 * @brief Case-insensitive algorithm lookup
 *
 * Uppercases the input and looks it up in the base name map.
 * Does NOT handle variant-prefixed names — use ResolveVariantSelection()
 * for those.
 *
 * @param name Algorithm name (any case)
 * @return Pair of (found, ReorderingAlgo)
 */
inline std::pair<bool, ReorderingAlgo> lookupAlgorithm(const std::string& name) {
    const auto& map = getAlgorithmNameMap();
    auto it = map.find(toUpper(name));
    if (it != map.end()) return {true, it->second};
    return {false, ORIGINAL};
}

// ============================================================================
// PERCEPTRON SELECTION RESULT
// ============================================================================

/**
 * @brief Result of perceptron-based algorithm selection.
 *
 * Carries both the base enum (for dispatch) and the variant name
 * (so the perceptron can distinguish between e.g. GraphBrewOrder_leiden
 * vs GraphBrewOrder_rabbit at scoring time).
 */
struct PerceptronSelection {
    ReorderingAlgo algo;                  ///< Base enum for dispatch
    std::string variant_name;             ///< Canonical name (e.g. "GraphBrewOrder_leiden")
    std::vector<std::string> options;     ///< Variant options for dispatch (e.g. {"leiden"})
    double score = 0.0;                   ///< Winning score
    double margin = 0.0;                  ///< Score margin over ORIGINAL (P1 1.4)
    double confidence = 0.0;              ///< Platt-calibrated P(win) in [0,1] (P1 1.4)
    bool explored = false;                ///< True if selected via bandit exploration (P2 2.1)

    /// Chained ordering steps (e.g. SORT+RABBITORDER_csr → 2 steps).
    /// Empty for single-algorithm selections.
    std::vector<PerceptronSelection> chain;

    /// True if the selection is ORIGINAL (no reordering)
    bool isOriginal() const { return algo == ORIGINAL && chain.empty(); }

    /// True if this is a chained (multi-step) ordering
    bool isChained() const { return !chain.empty(); }
};

/**
 * @brief Split a string on a delimiter into a vector of tokens.
 */
inline std::vector<std::string> splitString(
    const std::string& s, char delim) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(delim, start);
        if (end == std::string::npos) end = s.size();
        if (end > start) tokens.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return tokens;
}

/**
 * @brief Resolve a canonical variant name to dispatch information.
 *
 * Maps perceptron string keys like "GraphBrewOrder_leiden" to:
 *   - base enum (GraphBrewOrder)
 *   - dispatch options ({"leiden"})
 *
 * Compound variants are split on '_' after the prefix:
 *   "GraphBrewOrder_leiden_dfs"  → options = {"leiden", "dfs"}
 *   "GraphBrewOrder_rabbit_dbg" → options = {"rabbit", "dbg"}
 *
 * Chained orderings (containing '+') are split into sequential steps:
 *   "SORT+RABBITORDER_csr" → chain = [ResolveVariantSelection("SORT"),
 *                                      ResolveVariantSelection("RABBITORDER_csr")]
 *
 * Non-variant algorithms use case-insensitive lookup in the base map.
 *
 * @param variant_name Canonical name from weight file
 * @param score The perceptron score
 * @return PerceptronSelection with dispatch info
 */
inline PerceptronSelection ResolveVariantSelection(
    const std::string& variant_name, double score = 0.0) {
    
    PerceptronSelection sel;
    sel.score = score;
    
    // Chained orderings: "SORT+RABBITORDER_csr" → split on '+' and resolve each step
    if (variant_name.find('+') != std::string::npos) {
        auto steps = splitString(variant_name, '+');
        if (steps.size() >= 2) {
            sel.variant_name = variant_name;
            // Use first step's algo as the base (for legacy callers)
            auto first = ResolveVariantSelection(steps[0], score);
            sel.algo = first.algo;
            sel.options = first.options;
            for (const auto& step : steps) {
                sel.chain.push_back(ResolveVariantSelection(step, score));
            }
            return sel;
        }
    }
    
    // GraphBrewOrder variants: GraphBrewOrder_{leiden[_dfs][_dbg]...}
    if (variant_name.rfind("GraphBrewOrder_", 0) == 0) {
        sel.algo = GraphBrewOrder;
        sel.variant_name = variant_name;
        std::string suffix = variant_name.substr(15);  // after "GraphBrewOrder_"
        sel.options = splitString(suffix, '_');
        return sel;
    }
    
    // RabbitOrder variants: RABBITORDER_{csr,boost}
    if (variant_name.rfind("RABBITORDER_", 0) == 0) {
        sel.algo = RabbitOrder;
        sel.variant_name = variant_name;
        std::string suffix = variant_name.substr(12);  // after "RABBITORDER_"
        sel.options = splitString(suffix, '_');
        return sel;
    }
    
    // RCM variants: RCM_{default,bnf}
    if (variant_name.rfind("RCM_", 0) == 0) {
        sel.algo = RCMOrder;
        sel.variant_name = variant_name;
        std::string suffix = variant_name.substr(4);  // after "RCM_"
        if (suffix == "default") suffix = "";  // default RCM needs no option
        if (!suffix.empty()) sel.options = splitString(suffix, '_');
        return sel;
    }
    
    // GoGraphOrder variants: GOGRAPHORDER_{default,fast,naive}
    if (variant_name.rfind("GOGRAPHORDER_", 0) == 0) {
        sel.algo = GoGraphOrder;
        sel.variant_name = variant_name;
        std::string suffix = variant_name.substr(13);  // after "GOGRAPHORDER_"
        if (suffix == "default") suffix = "";  // default needs no option
        if (!suffix.empty()) sel.options = splitString(suffix, '_');
        return sel;
    }
    
    // Non-variant algorithms: case-insensitive lookup in base map
    auto [found, algo] = lookupAlgorithm(variant_name);
    if (found) {
        sel.algo = algo;
        sel.variant_name = variant_name;
        return sel;
    }
    
    // Unknown name — fallback to ORIGINAL
    sel.algo = ORIGINAL;
    sel.variant_name = "ORIGINAL";
    return sel;
}

// Note: ParseWeightsFromJSON is defined later in this file after PerceptronWeights

// ============================================================================
// GRAPH TYPE CLASSIFICATION
// ============================================================================

/**
 * @brief Graph type for algorithm selection
 * 
 * Used to classify graphs into categories for type-specific algorithm selection.
 * The classification is based on structural features like modularity, degree
 * distribution, and hub concentration.
 */
enum GraphType {
    GRAPH_GENERIC = 0,  ///< Unknown or mixed graph type
    GRAPH_SOCIAL,       ///< Social networks (Facebook, Twitter)
    GRAPH_ROAD,         ///< Road networks (planar, mesh-like)
    GRAPH_WEB,          ///< Web graphs (bow-tie structure)
    GRAPH_POWERLAW,     ///< Power-law/RMAT graphs
    GRAPH_UNIFORM       ///< Uniform random graphs
};

/**
 * @brief Convert graph type enum to string
 */
inline std::string GraphTypeToString(GraphType type) {
    switch (type) {
        case GRAPH_SOCIAL:   return "social";
        case GRAPH_ROAD:     return "road";
        case GRAPH_WEB:      return "web";
        case GRAPH_POWERLAW: return "powerlaw";
        case GRAPH_UNIFORM:  return "uniform";
        case GRAPH_GENERIC:
        default:             return "generic";
    }
}

/**
 * @brief Convert string to graph type enum
 */
inline GraphType GetGraphType(const std::string& name) {
    if (name.empty() || name == "generic" || name == "GENERIC" || name == "default") return GRAPH_GENERIC;
    if (name == "social" || name == "SOCIAL") return GRAPH_SOCIAL;
    if (name == "road" || name == "ROAD" || name == "mesh") return GRAPH_ROAD;
    if (name == "web" || name == "WEB") return GRAPH_WEB;
    if (name == "powerlaw" || name == "POWERLAW" || name == "rmat" || name == "RMAT") return GRAPH_POWERLAW;
    if (name == "uniform" || name == "UNIFORM" || name == "random") return GRAPH_UNIFORM;
    return GRAPH_GENERIC;
}

/**
 * @brief Auto-detect graph type from graph features
 * 
 * Uses a decision tree based on empirical observations:
 * 1. High modularity (>0.3) + power-law degrees → SOCIAL
 * 2. Low modularity (<0.1) + low degree variance → ROAD
 * 3. High hub concentration (>0.5) → WEB
 * 4. High degree variance (>1.5) + low modularity → POWERLAW
 * 5. Low degree variance (<0.5) + low hub concentration → UNIFORM
 * 6. Otherwise → GENERIC
 * 
 * @param modularity Graph modularity score
 * @param degree_variance Coefficient of variation of degree distribution
 * @param hub_concentration Fraction of edges from top 10% degree nodes
 * @param avg_degree Average node degree
 * @param num_nodes Number of nodes in graph
 * @return Detected GraphType
 */
inline GraphType DetectGraphType(double modularity, double degree_variance, 
                                  double hub_concentration, double avg_degree,
                                  size_t num_nodes) {
    (void)num_nodes;  // Unused but kept for API compatibility
    
    // Decision tree for graph type classification
    
    // Road networks: low modularity, mesh-like (low variance, moderate degree)
    if (modularity < 0.1 && degree_variance < 0.5 && avg_degree < 10) {
        return GRAPH_ROAD;
    }
    
    // Social networks: high modularity with community structure
    if (modularity > 0.3 && degree_variance > 0.8) {
        return GRAPH_SOCIAL;
    }
    
    // Web graphs: extreme hub concentration (bow-tie structure)
    if (hub_concentration > 0.5 && degree_variance > 1.0) {
        return GRAPH_WEB;
    }
    
    // Power-law (RMAT): high skew but lower modularity than social
    if (degree_variance > 1.5 && modularity < 0.3) {
        return GRAPH_POWERLAW;
    }
    
    // Uniform random: low variance, low hub concentration
    if (degree_variance < 0.5 && hub_concentration < 0.3 && modularity < 0.1) {
        return GRAPH_UNIFORM;
    }
    
    // Default to generic
    return GRAPH_GENERIC;
}

// ============================================================================
// SELECTION MODE FOR ADAPTIVE ORDER
// ============================================================================

/**
 * @brief Selection mode for AdaptiveOrder algorithm selection
 * 
 * Controls how AdaptiveOrder selects the best reordering algorithm:
 * 
 * FASTEST_REORDER (0): Select algorithm with lowest reordering time
 *   - Use when reordering cost dominates (single execution)
 *   - Falls back to this mode for UNKNOWN/UNTRAINED graphs
 * 
 * FASTEST_EXECUTION (1): Select algorithm with best cache performance
 *   - Use for repeated algorithm executions
 *   - Uses perceptron weights to predict execution speedup
 *   - Ignores reordering overhead
 * 
 * BEST_ENDTOEND (2): Minimize (reorder_time + execution_time)
 *   - Balanced approach for typical workloads
 *   - Combines perceptron score with reorder time penalty
 * 
 * BEST_AMORTIZATION (3): Minimize iterations to amortize reorder cost
 *   - For scenarios where you need minimum runs to break even
 *   - Considers: reorder_time / (predicted_speedup - 1.0)
 *
 * DECISION_TREE (4): Use trained Decision Tree classifier
 *   - Per-benchmark trees select algorithm family based on graph features
 *   - Lower regret than perceptron (~5-8% vs 45-1355%)
 *   - C++ trains DT models at runtime from benchmarks.json (reorder_database.h)
 */

// ============================================================================
// NEW: Separated Model + Criterion enums (orthogonal axes)
// ============================================================================

/**
 * @brief Which prediction model to use for algorithm selection.
 *
 * Orthogonal to SelectionCriterion — any model can optimize any criterion.
 */
enum SelectionModel {
    SELECTION_MODEL_PERCEPTRON = 0,        ///< Multi-class perceptron (SSO weights)
    SELECTION_MODEL_DECISION_TREE = 1,     ///< Trained Decision Tree classifier
    SELECTION_MODEL_HYBRID = 2,            ///< Hybrid DT + per-leaf perceptron
    SELECTION_MODEL_KNN_DATABASE = 3,      ///< Distance-weighted k-NN from DB
    // Label granularity modes (orthogonal to model; controls class count)
    SELECTION_MODEL_FAMILY = 4,            ///< Predict at family level (7 classes)
    SELECTION_MODEL_TOPN = 5,              ///< Predict top-N algorithms (~8 classes)
    SELECTION_MODEL_INDIVIDUAL = 6         ///< Predict individual algorithm (~17 classes)
};

/**
 * @brief What metric defines the "best" reordering algorithm.
 *
 * Orthogonal to SelectionModel — any criterion can be evaluated by any model.
 */
enum SelectionCriterion {
    CRITERION_FASTEST_REORDER = 0,         ///< Minimize reordering time only
    CRITERION_FASTEST_EXECUTION = 1,       ///< Minimize kernel execution time
    CRITERION_BEST_ENDTOEND = 2,           ///< Minimize (reorder_time + exec_time)
    CRITERION_BEST_AMORTIZATION = 3        ///< Minimize iterations to amortize
};

/**
 * @brief Convert selection model enum to string
 */
inline std::string SelectionModelToString(SelectionModel m) {
    switch (m) {
        case SELECTION_MODEL_PERCEPTRON:     return "perceptron";
        case SELECTION_MODEL_DECISION_TREE:  return "decision-tree";
        case SELECTION_MODEL_HYBRID:         return "hybrid";
        case SELECTION_MODEL_KNN_DATABASE:   return "knn-database";
        case SELECTION_MODEL_FAMILY:         return "family";
        case SELECTION_MODEL_TOPN:           return "topn";
        case SELECTION_MODEL_INDIVIDUAL:     return "individual";
        default:                             return "unknown-model";
    }
}

/**
 * @brief Convert selection criterion enum to string
 */
inline std::string SelectionCriterionToString(SelectionCriterion c) {
    switch (c) {
        case CRITERION_FASTEST_REORDER:    return "fastest-reorder";
        case CRITERION_FASTEST_EXECUTION:  return "fastest-execution";
        case CRITERION_BEST_ENDTOEND:      return "best-endtoend";
        case CRITERION_BEST_AMORTIZATION:  return "best-amortization";
        default:                           return "unknown-criterion";
    }
}

/**
 * @brief Parse a model string (e.g. "perceptron", "dt", "hybrid", "knn")
 */
inline SelectionModel GetSelectionModel(const std::string& name) {
    if (name == "perceptron" || name == "perc" || name == "p") return SELECTION_MODEL_PERCEPTRON;
    if (name == "decision-tree" || name == "dt" || name == "tree") return SELECTION_MODEL_DECISION_TREE;
    if (name == "hybrid" || name == "model-tree" || name == "hme") return SELECTION_MODEL_HYBRID;
    if (name == "knn-database" || name == "knn" || name == "database" || name == "db") return SELECTION_MODEL_KNN_DATABASE;
    if (name == "family" || name == "fam") return SELECTION_MODEL_FAMILY;
    if (name == "topn" || name == "top-n" || name == "top") return SELECTION_MODEL_TOPN;
    if (name == "individual" || name == "ind" || name == "all") return SELECTION_MODEL_INDIVIDUAL;
    return SELECTION_MODEL_KNN_DATABASE;  // Default: use the database model
}

/**
 * @brief Parse a criterion string (e.g. "execution", "e2e", "reorder")
 */
inline SelectionCriterion GetSelectionCriterion(const std::string& name) {
    if (name == "fastest-reorder" || name == "reorder" || name == "fr") return CRITERION_FASTEST_REORDER;
    if (name == "fastest-execution" || name == "execution" || name == "fe") return CRITERION_FASTEST_EXECUTION;
    if (name == "best-endtoend" || name == "endtoend" || name == "e2e") return CRITERION_BEST_ENDTOEND;
    if (name == "best-amortization" || name == "amortization" || name == "amortize") return CRITERION_BEST_AMORTIZATION;
    return CRITERION_FASTEST_EXECUTION;  // Default
}

// Legacy SelectionMode enum — kept for backward compatibility.
// Internally, all dispatch uses (SelectionModel, SelectionCriterion).
enum SelectionMode {
    MODE_FASTEST_REORDER = 0,      ///< Minimize reordering time
    MODE_FASTEST_EXECUTION = 1,    ///< Minimize execution time (perceptron)
    MODE_BEST_ENDTOEND = 2,        ///< Minimize total time
    MODE_BEST_AMORTIZATION = 3,    ///< Minimize iterations to amortize
    MODE_DECISION_TREE = 4,        ///< Decision Tree classifier (per-benchmark)
    MODE_HYBRID = 5,               ///< Hybrid DT+Perceptron (Model Tree)
    MODE_DATABASE = 6              ///< Database-driven kNN (streaming equation)
};

/**
 * @brief Convert selection mode to string
 */
inline std::string SelectionModeToString(SelectionMode mode) {
    switch (mode) {
        case MODE_FASTEST_REORDER:    return "fastest-reorder";
        case MODE_FASTEST_EXECUTION:  return "fastest-execution";
        case MODE_BEST_ENDTOEND:      return "best-endtoend";
        case MODE_BEST_AMORTIZATION:  return "best-amortization";
        case MODE_DECISION_TREE:      return "decision-tree";
        case MODE_HYBRID:              return "hybrid";
        case MODE_DATABASE:            return "database";
        default:                      return "unknown";
    }
}

/**
 * @brief Convert string to selection mode
 */
inline SelectionMode GetSelectionMode(const std::string& name) {
    if (name == "0" || name == "fastest-reorder" || name == "reorder") return MODE_FASTEST_REORDER;
    if (name == "1" || name == "fastest-execution" || name == "execution" || name == "cache") return MODE_FASTEST_EXECUTION;
    if (name == "2" || name == "best-endtoend" || name == "endtoend" || name == "e2e") return MODE_BEST_ENDTOEND;
    if (name == "3" || name == "best-amortization" || name == "amortization" || name == "amortize") return MODE_BEST_AMORTIZATION;
    if (name == "4" || name == "decision-tree" || name == "dt" || name == "tree") return MODE_DECISION_TREE;
    if (name == "5" || name == "hybrid" || name == "model-tree" || name == "hme") return MODE_HYBRID;
    if (name == "6" || name == "database" || name == "db" || name == "knn") return MODE_DATABASE;
    return MODE_FASTEST_EXECUTION;  // Default
}

/**
 * @brief Decompose legacy SelectionMode into (model, criterion) pair.
 *
 * Backward compatibility: the old 0-6 enum maps as follows:
 *   0 → (perceptron, fastest-reorder)
 *   1 → (perceptron, fastest-execution)
 *   2 → (perceptron, best-endtoend)
 *   3 → (perceptron, best-amortization)
 *   4 → (decision-tree, fastest-execution)
 *   5 → (hybrid, fastest-execution)
 *   6 → (knn-database, fastest-execution)
 */
inline std::pair<SelectionModel, SelectionCriterion>
DecomposeSelectionMode(SelectionMode mode) {
    switch (mode) {
        case MODE_FASTEST_REORDER:    return {SELECTION_MODEL_PERCEPTRON, CRITERION_FASTEST_REORDER};
        case MODE_FASTEST_EXECUTION:  return {SELECTION_MODEL_PERCEPTRON, CRITERION_FASTEST_EXECUTION};
        case MODE_BEST_ENDTOEND:      return {SELECTION_MODEL_PERCEPTRON, CRITERION_BEST_ENDTOEND};
        case MODE_BEST_AMORTIZATION:  return {SELECTION_MODEL_PERCEPTRON, CRITERION_BEST_AMORTIZATION};
        case MODE_DECISION_TREE:      return {SELECTION_MODEL_DECISION_TREE, CRITERION_FASTEST_EXECUTION};
        case MODE_HYBRID:             return {SELECTION_MODEL_HYBRID, CRITERION_FASTEST_EXECUTION};
        case MODE_DATABASE:           return {SELECTION_MODEL_KNN_DATABASE, CRITERION_FASTEST_EXECUTION};
        default:                      return {SELECTION_MODEL_PERCEPTRON, CRITERION_FASTEST_EXECUTION};
    }
}

/**
 * @brief Parse "model:criterion" format or fall back to legacy mode parsing.
 *
 * Accepts formats:
 *   "dt:e2e"               → (decision-tree, best-endtoend)
 *   "perceptron:reorder"   → (perceptron, fastest-reorder)
 *   "knn:amortize"         → (knn-database, best-amortization)
 *   "4" or "decision-tree" → legacy DecomposeSelectionMode
 *   "1" or "execution"     → legacy DecomposeSelectionMode
 */
inline std::pair<SelectionModel, SelectionCriterion>
ParseModelCriterion(const std::string& spec) {
    // Try "model:criterion" format first
    auto colon = spec.find(':');
    if (colon != std::string::npos) {
        std::string model_str = spec.substr(0, colon);
        std::string crit_str = spec.substr(colon + 1);
        return {GetSelectionModel(model_str), GetSelectionCriterion(crit_str)};
    }
    // Fall back to legacy SelectionMode
    SelectionMode legacy = GetSelectionMode(spec);
    return DecomposeSelectionMode(legacy);
}

// ============================================================================
// ADAPTIVE MODE FOR MULTI-LEVEL ORDERING
// ============================================================================

/**
 * @brief Adaptive algorithm selection mode
 * 
 * Controls how AdaptiveOrder processes the graph:
 * - FullGraph: Analyze entire graph, select single best algorithm
 * - PerCommunity: Run community detection, select algorithm per community
 * - Recursive: Recursively partition and select for sub-communities
 */
enum class AdaptiveMode {
    FullGraph = 0,    ///< Analyze entire graph, select one algorithm
    PerCommunity = 1, ///< Select algorithm per community (default)
    Recursive = 2     ///< Recursively partition and select
};

/**
 * @brief Parse integer to AdaptiveMode enum
 */
inline AdaptiveMode ParseAdaptiveMode(int mode) {
    switch (mode) {
        case 0: return AdaptiveMode::FullGraph;
        case 1: return AdaptiveMode::PerCommunity;
        case 2: return AdaptiveMode::Recursive;
        default: return AdaptiveMode::PerCommunity;
    }
}

/**
 * @brief Convert AdaptiveMode to string
 */
inline std::string AdaptiveModeToString(AdaptiveMode m) {
    switch (m) {
        case AdaptiveMode::FullGraph: return "full_graph";
        case AdaptiveMode::PerCommunity: return "per_community";
        case AdaptiveMode::Recursive: return "recursive";
        default: return "unknown";
    }
}

/**
 * @brief Convert AdaptiveMode to integer
 */
inline int AdaptiveModeToInt(AdaptiveMode m) {
    return static_cast<int>(m);
}

// ============================================================================
// ABLATION CONFIGURATION
// ============================================================================

/**
 * Runtime ablation toggles for AdaptiveOrder experiments.
 * 
 * Controlled via environment variables. Checked once and cached.
 * 
 * Environment variables:
 *   ADAPTIVE_NO_TYPES=1     — Force type_id = "type_0" for all graphs
 *   ADAPTIVE_NO_OOD=1       — Disable OOD guardrail (always use perceptron)
 *   ADAPTIVE_NO_MARGIN=1    — Disable margin-based ORIGINAL fallback
 *   ADAPTIVE_NO_LEIDEN=1    — Skip Leiden, treat whole graph as one community
 *   ADAPTIVE_FORCE_ALGO=N   — Force algorithm N for all communities (bypass perceptron)
 *   ADAPTIVE_ZERO_FEATURES=packing,fef,wsr,quadratic  — Zero specific feature groups
 *
 * Usage: ADAPTIVE_NO_OOD=1 ADAPTIVE_NO_MARGIN=1 ./bench/bin/pr -f graph.sg -o 14 -n 3
 */
struct AblationConfig {
    bool no_types;           // Force type_id = "type_0"
    bool no_ood;             // Disable OOD guardrail
    bool no_margin;          // Disable margin-based ORIGINAL fallback
    bool no_leiden;          // Skip Leiden partitioning
    int force_algo;          // Force specific algorithm (-1 = disabled)
    bool zero_packing;       // Zero packing_factor weight
    bool zero_fef;           // Zero forward_edge_fraction + fef_convergence
    bool zero_wsr;           // Zero working_set_ratio weight
    bool zero_quadratic;     // Zero all quadratic interaction terms
    // Cost and confidence controls
    bool cost_model;         // Enable cost-aware dynamic margin threshold
    bool packing_skip;       // Enable packing-factor short-circuit
    bool overhead_filter;    // Skip heavyweight algorithms when cost exceeds budget
    bool fef_validate;       // Post-selection FEF validation for PR/SSSP
    bool platt_scaling;      // Use Platt-calibrated margin threshold
    bool bandit_explore;     // ε-greedy exploration for low-margin cases
    double bandit_epsilon;   // Initial ε (probability of exploring), default 0.1
    bool don_tiebreak;       // DON-augmented Gorder tiebreaking
    // Hierarchical model gating
    bool hierarchical_gate;   // Two-level model: graph-type gate + regime-specific expert
    // DON-Lite neural ordering
    bool don_lite;            // MLP-based vertex ordering for uncertain communities
    // Mode 3: Top-K pilot selection
    int top_k;               // Number of candidate orderings to try (default 1)
    
    static const AblationConfig& Get() {
        static AblationConfig instance = Init();
        return instance;
    }
    
    bool any_active() const {
        return no_types || no_ood || no_margin || no_leiden ||
               force_algo >= 0 || zero_packing || zero_fef ||
               zero_wsr || zero_quadratic || cost_model || packing_skip ||
               overhead_filter || fef_validate || platt_scaling ||
               bandit_explore || don_tiebreak || hierarchical_gate || don_lite || top_k > 1;
    }
    
    void print() const {
        if (!any_active()) return;
        printf("=== Ablation Toggles Active ===\n");
        if (no_types)       printf("  NO_TYPES: force type_0\n");
        if (no_ood)         printf("  NO_OOD: disable OOD guardrail\n");
        if (no_margin)      printf("  NO_MARGIN: disable margin fallback\n");
        if (no_leiden)      printf("  NO_LEIDEN: skip partitioning\n");
        if (force_algo >= 0) printf("  FORCE_ALGO: %d\n", force_algo);
        if (zero_packing)   printf("  ZERO: packing_factor\n");
        if (zero_fef)       printf("  ZERO: forward_edge_fraction + fef_convergence\n");
        if (zero_wsr)       printf("  ZERO: working_set_ratio\n");
        if (zero_quadratic) printf("  ZERO: quadratic interaction terms\n");
        if (cost_model)     printf("  COST_MODEL: cost-aware dynamic margin (P0 1.1)\n");
        if (packing_skip)   printf("  PACKING_SKIP: packing factor short-circuit (P0 1.2)\n");
        if (overhead_filter) printf("  OVERHEAD_FILTER: skip heavyweights (P1 2.3)\n");
        if (fef_validate)   printf("  FEF_VALIDATE: post-selection FEF check (P1 1.3)\n");
        if (platt_scaling)  printf("  PLATT_SCALING: calibrated margin threshold (P1 1.4)\n");
        if (bandit_explore) printf("  BANDIT_EXPLORE: ε-greedy exploration, ε₀=%.3f (P2 2.1)\n", bandit_epsilon);
        if (don_tiebreak)   printf("  DON_TIEBREAK: DON-augmented Gorder tiebreaking (P2 3.1e)\n");
        if (hierarchical_gate) printf("  HIERARCHICAL: two-level regime-specific gating (P3 3.3)\n");
        if (don_lite)       printf("  DON_LITE: MLP-based vertex ordering (P3 3.1f)\n");
        if (top_k > 1)      printf("  TOP_K: try %d candidate orderings\n", top_k);
        printf("===============================\n");
    }
    
private:
    static bool env_bool(const char* name) {
        const char* val = std::getenv(name);
        return val && (std::string(val) == "1" || std::string(val) == "true");
    }
    
    static int env_int(const char* name, int default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        try { return std::stoi(val); } catch (...) { return default_val; }
    }
    
    static double env_double(const char* name, double default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        try { return std::stod(val); } catch (...) { return default_val; }
    }
    
    static bool feature_in_list(const char* feature) {
        const char* val = std::getenv("ADAPTIVE_ZERO_FEATURES");
        if (!val) return false;
        std::string s(val);
        return s.find(feature) != std::string::npos;
    }
    
    static AblationConfig Init() {
        AblationConfig cfg;
        cfg.no_types       = env_bool("ADAPTIVE_NO_TYPES");
        cfg.no_ood         = env_bool("ADAPTIVE_NO_OOD");
        cfg.no_margin      = env_bool("ADAPTIVE_NO_MARGIN");
        cfg.no_leiden      = env_bool("ADAPTIVE_NO_LEIDEN");
        cfg.force_algo     = env_int("ADAPTIVE_FORCE_ALGO", -1);
        cfg.zero_packing   = feature_in_list("packing");
        cfg.zero_fef       = feature_in_list("fef");
        cfg.zero_wsr       = feature_in_list("wsr");
        cfg.zero_quadratic = feature_in_list("quadratic");
        cfg.cost_model     = env_bool("ADAPTIVE_COST_MODEL");
        cfg.packing_skip   = env_bool("ADAPTIVE_PACKING_SKIP");
        cfg.overhead_filter = env_bool("ADAPTIVE_OVERHEAD_FILTER");
        cfg.fef_validate   = env_bool("ADAPTIVE_FEF_VALIDATE");
        cfg.platt_scaling  = env_bool("ADAPTIVE_PLATT");
        cfg.bandit_explore = env_bool("ADAPTIVE_BANDIT");
        cfg.bandit_epsilon = env_double("ADAPTIVE_BANDIT_EPSILON", 0.1);
        cfg.don_tiebreak   = env_bool("ADAPTIVE_DON_TIEBREAK");
        cfg.hierarchical_gate = env_bool("ADAPTIVE_HIERARCHICAL");
        cfg.don_lite       = env_bool("ADAPTIVE_DON_LITE");
        cfg.top_k          = env_int("ADAPTIVE_TOP_K", 1);
        if (cfg.top_k < 1) cfg.top_k = 1;
        return cfg;
    }
};

// ============================================================================
// PERCEPTRON WEIGHTS FOR ADAPTIVE ORDER
// ============================================================================

/**
 * @brief Perceptron weights for ML-based algorithm selection
 * 
 * AdaptiveOrder uses these weights to compute a score for each
 * reordering algorithm based on graph features. The algorithm
 * with the highest score is selected.
 * 
 * Score = bias + sum(weight_i * feature_i) * benchmark_multiplier
 */
struct PerceptronWeights {
    // ---------- Base ----------
    double bias = 0.0;              ///< Base preference for this algorithm
    
    // ---------- Feature Weights (core) ----------
    double w_modularity = 0.0;       ///< Weight for modularity feature
    double w_log_nodes = 0.0;        ///< Weight for log(num_nodes)
    double w_log_edges = 0.0;        ///< Weight for log(num_edges)
    double w_density = 0.0;          ///< Weight for density feature
    double w_avg_degree = 0.0;       ///< Weight for average degree
    double w_degree_variance = 0.0;  ///< Weight for degree variance
    double w_hub_concentration = 0.0;///< Weight for hub concentration
    
    // ---------- Feature Weights (extended) ----------
    double w_clustering_coeff = 0.0; ///< Weight for clustering coefficient
    double w_avg_path_length = 0.0;  ///< Weight for average path length
    double w_diameter = 0.0;         ///< Weight for diameter estimate
    double w_community_count = 0.0;  ///< Weight for community count
    
    // ---------- Locality Feature Weights (IISWC'18 / GoGraph) ----------
    double w_packing_factor = 0.0;     ///< Weight for packing factor (hub co-location)
    double w_forward_edge_fraction = 0.0; ///< Weight for forward edge fraction (convergence)
    
    // ---------- System-Cache Feature Weights (P-OPT) ----------
    double w_working_set_ratio = 0.0;  ///< Weight for working set ratio (graph_bytes / LLC)
    double w_wsr_l1 = 0.0;             ///< Weight for L1-level WSR
    double w_wsr_l2 = 0.0;             ///< Weight for L2-level WSR
    
    // ---------- DON-RL Feature Weights (Zhao et al.) ----------
    double w_vertex_significance_skewness = 0.0;  ///< Weight for vertex significance skewness
    double w_window_neighbor_overlap = 0.0;       ///< Weight for window neighbor overlap
    
    // ---------- Quadratic Interaction Weights ----------
    double w_dv_x_hub = 0.0;            ///< degree_variance × hub_concentration (IISWC'18)
    double w_mod_x_logn = 0.0;          ///< modularity × log_nodes (community-at-scale)
    double w_pf_x_wsr = 0.0;            ///< packing_factor × working_set_ratio (cache-locality)
    double w_vss_x_hc = 0.0;            ///< vertex_significance_skewness × hub_concentration
    double w_wno_x_pf = 0.0;            ///< window_neighbor_overlap × packing_factor
    
    // ---------- Convergence Bonus Weight (GoGraph) ----------
    double w_fef_convergence = 0.0;     ///< Extra forward_edge_fraction weight for iterative algos (PR/SSSP)
    
    // ---------- Sampled Locality Score Weight (P1 3.1d) ----------
    double w_sampled_locality = 0.0;    ///< Weight for sampled F(σ) locality quality
    
    // ---------- Transpose Reuse Distance Weight (P3 3.2) ----------
    double w_avg_reuse_distance = 0.0;  ///< Weight for transpose-derived reuse distance
    
    // ---------- Paper-Aligned Feature Weights ----------
    double w_packing_factor_cl = 0.0;       ///< IISWC'18 cache-line packing factor
    double w_locality_score_pairwise = 0.0; ///< DON-RL pairwise F(σ)
    double w_reuse_distance_lru = 0.0;      ///< P-OPT LRU stack distance
    
    // ---------- Cache Impact Weights ----------
    double cache_l1_impact = 0.0;    ///< L1 cache impact weight
    double cache_l2_impact = 0.0;    ///< L2 cache impact weight
    double cache_l3_impact = 0.0;    ///< L3 cache impact weight
    double cache_dram_penalty = 0.0; ///< DRAM access penalty weight
    
    // ---------- Reorder Time Weight ----------
    double w_reorder_time = 0.0;     ///< Weight for reorder time estimate
    
    // ---------- Metadata from Training ----------
    double avg_speedup = 1.0;        ///< Average speedup observed during training
    double avg_reorder_time = 0.0;   ///< Average reorder time in seconds
    
    // ---------- Platt Scaling (P1 1.4) ----------
    // After training, logistic regression maps score margin to P(win):
    //   P(win) = 1 / (1 + exp(-(platt_A * margin + platt_B)))
    // When platt_A == 0, Platt scaling is disabled (uncalibrated).
    double platt_A = 0.0;           ///< Platt sigmoid slope parameter
    double platt_B = 0.0;           ///< Platt sigmoid intercept parameter
    
    // ---------- Z-score Normalization (Mode 5) ----------
    // When use_normalization is true, weights are in z-score space and
    // scoreBase() normalizes features before computing the dot product.
    // This eliminates de-normalization weight explosion from Python training.
    static constexpr int N_FEATURES = 24;
    bool use_normalization = false;      ///< True when _normalization block present in JSON
    double norm_mean[N_FEATURES] = {};   ///< Per-feature means for z-score normalization
    double norm_std[N_FEATURES] = {};    ///< Per-feature stds for z-score normalization
    
    // ---------- Per-Benchmark Multipliers ----------
    double bench_pr = 1.0;           ///< PageRank weight multiplier
    double bench_bfs = 1.0;          ///< BFS weight multiplier
    double bench_cc = 1.0;           ///< CC weight multiplier
    double bench_sssp = 1.0;         ///< SSSP weight multiplier
    double bench_bc = 1.0;           ///< BC weight multiplier
    double bench_tc = 1.0;           ///< TC weight multiplier
    double bench_pr_spmv = 1.0;      ///< PR_SPMV weight multiplier
    double bench_cc_sv = 1.0;        ///< CC_SV weight multiplier
    
    /**
     * @brief Calculate iterations needed to amortize reorder cost
     * 
     * Formula: iterations = reorder_time / time_saved_per_iteration
     * Where time_saved ≈ baseline_time * (1 - 1/speedup)
     * 
     * @return Number of iterations to amortize (INFINITY if never pays off)
     */
    double iterationsToAmortize() const {
        if (avg_speedup <= 1.0) {
            return std::numeric_limits<double>::infinity();
        }
        double time_saved_per_iter = (avg_speedup - 1.0) / avg_speedup;
        if (time_saved_per_iter <= 0) {
            return std::numeric_limits<double>::infinity();
        }
        return avg_reorder_time / time_saved_per_iter;
    }
    
    /**
     * @brief Get benchmark-specific multiplier
     * @param bench Benchmark type
     * @return Multiplier for that benchmark (1.0 for GENERIC)
     */
    double getBenchmarkMultiplier(BenchmarkType bench) const {
        switch (bench) {
            case BENCH_PR:   return bench_pr;
            case BENCH_BFS:  return bench_bfs;
            case BENCH_CC:   return bench_cc;
            case BENCH_SSSP: return bench_sssp;
            case BENCH_BC:   return bench_bc;
            case BENCH_TC:   return bench_tc;
            case BENCH_PR_SPMV: return bench_pr_spmv;
            case BENCH_CC_SV: return bench_cc_sv;
            case BENCH_GENERIC:
            default:         return 1.0;
        }
    }
    
    /**
     * @brief Compute base score (without benchmark adjustment)
     * @param feat Community features to evaluate
     * @return Base score for this algorithm
     */
    double scoreBase(const CommunityFeatures& feat) const {
        // Mode 5: if normalization stats are available, z-normalize features
        // before scoring — weights are in z-score space (small, ≤ W_MAX).
        if (use_normalization) {
            return scoreBaseNormalized(feat);
        }
        double log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        double log_edges = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        
        // Ablation config (cached singleton, no per-call overhead)
        const auto& abl = AblationConfig::Get();
        
        // Core score
        double s = bias 
             + w_modularity * feat.modularity
             + w_log_nodes * log_nodes
             + w_log_edges * log_edges
             + w_density * feat.internal_density
             + w_avg_degree * feat.avg_degree / 100.0
             + w_degree_variance * feat.degree_variance
             + w_hub_concentration * feat.hub_concentration;
        
        // Extended features
        s += w_clustering_coeff * feat.clustering_coeff;
        s += w_avg_path_length * feat.avg_path_length / 10.0;
        s += w_diameter * feat.diameter_estimate / 50.0;
        s += w_community_count * std::log10(feat.community_count + 1.0);
        
        // Locality features (IISWC'18 Packing Factor, GoGraph forward edge fraction)
        // Ablation: ADAPTIVE_ZERO_FEATURES=packing zeroes packing_factor
        // Ablation: ADAPTIVE_ZERO_FEATURES=fef zeroes forward_edge_fraction
        if (!abl.zero_packing)
            s += w_packing_factor * feat.packing_factor;
        if (!abl.zero_fef)
            s += w_forward_edge_fraction * feat.forward_edge_fraction;
        
        // System-cache feature (P-OPT: cache hierarchy geometry)
        // Ablation: ADAPTIVE_ZERO_FEATURES=wsr zeroes working_set_ratio
        double log_wsr = std::log2(feat.working_set_ratio + 1.0);
        if (!abl.zero_wsr) {
            s += w_working_set_ratio * log_wsr;
            s += w_wsr_l1 * std::log2(feat.wsr_l1 + 1.0);
            s += w_wsr_l2 * std::log2(feat.wsr_l2 + 1.0);
        }
        
        // DON-RL features (Zhao et al.)
        s += w_vertex_significance_skewness * feat.vertex_significance_skewness;
        s += w_window_neighbor_overlap * feat.window_neighbor_overlap;
        
        // Quadratic interaction terms (paper-motivated cross-features)
        // Ablation: ADAPTIVE_ZERO_FEATURES=quadratic zeroes all five
        if (!abl.zero_quadratic) {
            s += w_dv_x_hub * feat.degree_variance * feat.hub_concentration;
            s += w_mod_x_logn * feat.modularity * log_nodes;
            s += w_pf_x_wsr * feat.packing_factor * log_wsr;
            s += w_vss_x_hc * feat.vertex_significance_skewness * feat.hub_concentration;
            s += w_wno_x_pf * feat.window_neighbor_overlap * feat.packing_factor;
        }
        
        // Cache impact weights
        s += cache_l1_impact * 0.5;
        s += cache_l2_impact * 0.3;
        s += cache_l3_impact * 0.2;
        s += cache_dram_penalty;
        
        // Reorder time penalty
        s += w_reorder_time * feat.reorder_time;
        
        // Sampled locality score (P1 3.1d: baseline ordering quality)
        s += w_sampled_locality * feat.sampled_locality_score;
        // P3 3.2: Transpose reuse distance
        s += w_avg_reuse_distance * feat.avg_reuse_distance;
        // Paper-aligned feature variants
        s += w_packing_factor_cl * feat.packing_factor_cl;
        s += w_locality_score_pairwise * feat.locality_score_pairwise;
        s += w_reuse_distance_lru * feat.reuse_distance_lru;
        
        return s;
    }
    
    /**
     * @brief Compute base score using z-score normalized features (Mode 5)
     *
     * Weights are in z-score space (learned via perceptron on z-normalized
     * features, all within [-W_MAX, W_MAX]).  This eliminates de-normalization
     * weight explosion that occurred when dividing by feature stds (e.g.
     * density std=1e-5 → 1/std=10k → millions-scale weights).
     *
     * Feature order matches Python training (feat_to_weight):
     *   0=modularity, 1=dv, 2=hc, 3=log_n, 4=log_e, 5=density, 6=ad/100,
     *   7=cc, 8=apl/10, 9=dia/50, 10=log_comm, 11=pf, 12=fef, 13=log_wsr,
     *   14=vss, 15=wno, 16=dv×hc, 17=mod×logn, 18=pf×wsr, 19=vss×hc, 20=wno×pf
     */
    double scoreBaseNormalized(const CommunityFeatures& feat) const {
        double log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        double log_edges = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        double log_wsr = std::log2(feat.working_set_ratio + 1.0);
        
        // Compute 24 transformed features matching Python training order
        const double raw[N_FEATURES] = {
            feat.modularity,                                    // 0
            feat.degree_variance,                               // 1
            feat.hub_concentration,                             // 2
            log_nodes,                                          // 3
            log_edges,                                          // 4
            feat.internal_density,                              // 5
            feat.avg_degree / 100.0,                            // 6
            feat.clustering_coeff,                              // 7
            feat.avg_path_length / 10.0,                        // 8
            feat.diameter_estimate / 50.0,                      // 9
            std::log10(feat.community_count + 1.0),             // 10
            feat.packing_factor,                                // 11
            feat.forward_edge_fraction,                         // 12
            log_wsr,                                            // 13
            feat.vertex_significance_skewness,                  // 14 (DON-RL)
            feat.window_neighbor_overlap,                       // 15 (DON-RL)
            feat.degree_variance * feat.hub_concentration,      // 16 (quadratic)
            feat.modularity * log_nodes,                        // 17 (quadratic)
            feat.packing_factor * log_wsr,                      // 18 (quadratic)
            feat.vertex_significance_skewness * feat.hub_concentration, // 19 (quadratic)
            feat.window_neighbor_overlap * feat.packing_factor, // 20 (quadratic)
            feat.packing_factor_cl,                             // 21 (IISWC'18 CL)
            std::log2(feat.wsr_l1 + 1.0),                      // 22 (P-OPT L1)
            std::log2(feat.wsr_l2 + 1.0),                      // 23 (P-OPT L2)
        };
        
        // Corresponding z-score weights in the same order
        const double w[N_FEATURES] = {
            w_modularity, w_degree_variance, w_hub_concentration,
            w_log_nodes, w_log_edges, w_density, w_avg_degree,
            w_clustering_coeff, w_avg_path_length, w_diameter,
            w_community_count, w_packing_factor, w_forward_edge_fraction,
            w_working_set_ratio, w_vertex_significance_skewness,
            w_window_neighbor_overlap,
            w_dv_x_hub, w_mod_x_logn, w_pf_x_wsr,
            w_vss_x_hc, w_wno_x_pf,
            w_packing_factor_cl,
            w_wsr_l1, w_wsr_l2,
        };
        
        const auto& abl = AblationConfig::Get();
        
        // Z-normalize each feature and accumulate weighted sum
        double s = bias;
        for (int i = 0; i < N_FEATURES; i++) {
            if (norm_std[i] < 1e-12) continue;  // skip zero-variance features
            // Ablation zeroing: packing=11, fef=12, wsr=13, quadratic=16-20
            if (i == 11 && abl.zero_packing) continue;
            if (i == 12 && abl.zero_fef) continue;
            if (i == 13 && abl.zero_wsr) continue;
            if (i >= 16 && i <= 20 && abl.zero_quadratic) continue;
            double z = (raw[i] - norm_mean[i]) / norm_std[i];
            s += w[i] * z;
        }
        
        // Extra terms not part of the 24-feature perceptron
        s += cache_l1_impact * 0.5 + cache_l2_impact * 0.3
           + cache_l3_impact * 0.2 + cache_dram_penalty;
        s += w_reorder_time * feat.reorder_time;
        // P1 3.1d: Sampled locality score (extra term, not z-normalized)
        s += w_sampled_locality * feat.sampled_locality_score;
        // P3 3.2: Transpose reuse distance (extra term, not z-normalized)
        s += w_avg_reuse_distance * feat.avg_reuse_distance;
        // Paper-aligned feature variants (extra terms, not z-normalized)
        s += w_locality_score_pairwise * feat.locality_score_pairwise;
        s += w_reuse_distance_lru * feat.reuse_distance_lru;
        
        return s;
    }
    
    /**
     * @brief Compute score with benchmark-specific adjustment
     * 
     * For convergence-sensitive benchmarks (PR, SSSP), adds an extra
     * forward_edge_fraction bonus (GoGraph: ordering direction affects
     * Gauss-Seidel convergence speed). For traversal benchmarks (BFS,
     * CC, TC), only locality matters.
     *
     * @param feat Community features to evaluate
     * @param bench Benchmark type for adjustment
     * @return Adjusted score for this algorithm
     */
    double score(const CommunityFeatures& feat, BenchmarkType bench = BENCH_GENERIC) const {
        double s = scoreBase(feat);
        
        // Convergence bonus: iterative algorithms benefit from ordering
        // that respects data-flow direction (forward edges → faster convergence)
        // Ablation: ADAPTIVE_ZERO_FEATURES=fef zeroes this bonus too
        // NOTE: The benchmark multiplier below intentionally scales the full
        // score including this bonus (consistent with per-benchmark weighting).
        if ((bench == BENCH_PR || bench == BENCH_PR_SPMV || bench == BENCH_SSSP) && !AblationConfig::Get().zero_fef) {
            s += w_fef_convergence * feat.forward_edge_fraction;
        }
        
        return s * getBenchmarkMultiplier(bench);
    }
};

// ============================================================================
// UTILITY MACROS
// ============================================================================

/**
 * @brief Print a labeled time value
 * @param label Description of the timing
 * @param seconds Time in seconds
 */
#ifndef REORDER_PRINT_TIME
#define REORDER_PRINT_TIME(label, seconds) \
    printf("%-21s%3.5lf\n", (std::string(label) + ":").c_str(), seconds)
#endif

// ============================================================================
// COMMON UTILITY FUNCTIONS
// ============================================================================

/**
 * @brief Compute total edge weight for a vertex
 * 
 * For unweighted graphs, returns the degree. For weighted graphs,
 * returns the sum of edge weights.
 * 
 * @tparam W Weight type
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @param v Vertex ID
 * @param g CSR graph
 * @param symmetric Whether graph is symmetric (affects weight counting)
 * @return Total edge weight for vertex v
 */
template <typename W, typename NodeID_, typename DestID_>
inline W computeVertexTotalWeight(NodeID_ v, 
                                   const CSRGraph<NodeID_, DestID_, true>& g,
                                   bool symmetric) {
    W total = W(0);
    for (auto neighbor : g.out_neigh(v)) {
        if constexpr (std::is_same_v<NodeID_, DestID_>) {
            total += W(1);  // Unweighted
        } else {
            total += static_cast<W>(neighbor.w);  // Weighted
        }
    }
    // For symmetric graphs stored with both directions, don't double count
    return total;
}

/**
 * @brief Verify that a permutation is valid
 * 
 * Checks that new_ids is a valid bijection from [0, n) to [0, n).
 * 
 * @tparam NodeID_ Node ID type
 * @param new_ids The permutation to verify
 * @param n Number of vertices
 * @return true if valid permutation, false otherwise
 */
template <typename NodeID_>
bool verifyPermutation(const pvector<NodeID_>& new_ids, NodeID_ n) {
    std::vector<bool> seen(n, false);
    for (NodeID_ i = 0; i < n; ++i) {
        if (new_ids[i] < 0 || new_ids[i] >= n) {
            return false;  // Out of range
        }
        if (seen[new_ids[i]]) {
            return false;  // Duplicate
        }
        seen[new_ids[i]] = true;
    }
    return true;
}

/**
 * @brief Count unique communities in an assignment
 * 
 * @tparam K Community ID type
 * @param communities Vector of community assignments
 * @return Number of unique communities
 */
template <typename K>
size_t countUniqueCommunities(const std::vector<K>& communities) {
    std::unordered_set<K> unique(communities.begin(), communities.end());
    return unique.size();
}

/**
 * @brief Auto-compute Leiden resolution based on graph density
 * 
 * Higher resolution = more communities
 /**
 * @brief Compute auto-resolution for Leiden based on graph properties
 * 
 * Higher resolution = more, smaller communities
 * Lower resolution = fewer, larger communities
 * 
 * Formula: γ = clip(0.5 + 0.25 × log₁₀(avg_degree + 1), 0.5, 1.2)
 * 
 * CV guardrail for power-law/hubby graphs:
 * - High CV (> 2.0) indicates heavy-tailed degree distribution (social/web graphs)
 * - These graphs benefit from LOWER resolution (coarser communities)
 * - Cap resolution at 0.75 for high-CV graphs to get better cache locality
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @param g CSR graph
 * @return Recommended resolution parameter
 */
template <typename NodeID_, typename DestID_>
double computeAutoResolution(const CSRGraph<NodeID_, DestID_, true>& g) {
    const int64_t n = g.num_nodes();
    const int64_t m = g.num_edges();
    
    if (n == 0) return 1.0;
    
    double avg_degree = static_cast<double>(m) / n;
    
    // Compute degree variance for CV-based adjustment
    double sum_sq = 0.0;
    #pragma omp parallel for reduction(+:sum_sq)
    for (int64_t v = 0; v < n; ++v) {
        double d = static_cast<double>(g.out_degree(v));
        double diff = d - avg_degree;
        sum_sq += diff * diff;
    }
    double variance = sum_sq / n;
    double cv = (avg_degree > 0) ? std::sqrt(variance) / avg_degree : 0.0;
    
    // Base resolution from average degree
    double resolution = 0.5 + 0.25 * std::log10(avg_degree + 1);
    resolution = std::max(0.5, std::min(1.2, resolution));
    
    // CV-based adjustment for power-law/hubby graphs
    // High-CV graphs (social networks, web graphs) benefit from coarser communities
    // which provide better cache locality for graph traversals
    if (cv > 2.0) {
        // Scale down resolution: higher CV → lower resolution
        // CV=2 → factor=1.0, CV=50 → factor≈0.6
        double factor = 2.0 / std::max(2.0, std::sqrt(cv));
        resolution = std::max(0.5, resolution * factor);
    }
    
    return resolution;
}

// ============================================================================
// UNIFIED RESOLUTION CONFIGURATION
// ============================================================================

/**
 * @brief Resolution mode for Leiden-based algorithms
 */
enum class ResolutionMode {
    FIXED,      ///< Use user-specified fixed value
    AUTO,       ///< Compute from graph properties (computeAutoResolution)
    DYNAMIC     ///< Adjust per-pass based on runtime metrics (GVELeidenAdaptiveCSR)
};

/**
 * @brief Resolution configuration parsed from command-line options
 */
struct ResolutionConfig {
    ResolutionMode mode = ResolutionMode::AUTO;
    double value = 1.0;           ///< Used if FIXED or computed if AUTO
    double initial_value = 1.0;   ///< Starting value if DYNAMIC
    
    /// Check if dynamic mode is requested
    bool isDynamic() const { return mode == ResolutionMode::DYNAMIC; }
    
    /// Get the effective resolution value (for non-dynamic algorithms)
    double getResolution() const {
        return (mode == ResolutionMode::DYNAMIC) ? initial_value : value;
    }
};

/**
 * @brief Parse resolution from command-line option string
 * 
 * Supports multiple formats:
 *   - "auto" or "0" or empty → AUTO mode (compute from graph)
 *   - "dynamic" → DYNAMIC mode with auto-computed initial
 *   - "dynamic_2.0" → DYNAMIC mode with specified initial value (use _ not :)
 *   - "1.5" → FIXED mode with value 1.5
 *   - ">3.0" → AUTO mode (legacy: values >3 trigger auto)
 * 
 * @tparam NodeID_T Node ID type
 * @tparam DestID_T Destination ID type
 * @param option Option string from command line
 * @param g CSR graph (used for auto-resolution computation)
 * @return ResolutionConfig with parsed mode and values
 */
template <typename NodeID_T, typename DestID_T>
ResolutionConfig parseResolution(
    const std::string& option,
    const CSRGraph<NodeID_T, DestID_T, true>& g) {
    
    ResolutionConfig cfg;
    
    // Empty or "auto" or "0" → AUTO mode
    if (option.empty() || option == "auto" || option == "0") {
        cfg.mode = ResolutionMode::AUTO;
        cfg.value = computeAutoResolution<NodeID_T, DestID_T>(g);
        cfg.initial_value = cfg.value;
        return cfg;
    }
    
    // "dynamic" or "dynamic_X" → DYNAMIC mode (use underscore to avoid CLI colon issues)
    if (option.rfind("dynamic", 0) == 0) {  // starts_with
        cfg.mode = ResolutionMode::DYNAMIC;
        // Check for underscore delimiter (e.g., "dynamic_2.0")
        size_t sep_pos = option.find('_');
        if (sep_pos == std::string::npos) {
            // Also support colon for programmatic use (e.g., "dynamic:2.0")
            sep_pos = option.find(':');
        }
        if (sep_pos != std::string::npos && sep_pos + 1 < option.size()) {
            cfg.initial_value = std::stod(option.substr(sep_pos + 1));
        } else {
            cfg.initial_value = computeAutoResolution<NodeID_T, DestID_T>(g);
        }
        cfg.value = cfg.initial_value;
        return cfg;
    }
    
    // Numeric value
    try {
        double parsed = std::stod(option);
        if (parsed > 3.0 || parsed <= 0) {
            // Legacy: values >3 or <=0 trigger auto-resolution
            cfg.mode = ResolutionMode::AUTO;
            cfg.value = computeAutoResolution<NodeID_T, DestID_T>(g);
        } else {
            cfg.mode = ResolutionMode::FIXED;
            cfg.value = parsed;
        }
        cfg.initial_value = cfg.value;
    } catch (...) {
        // Parse error → fallback to AUTO
        cfg.mode = ResolutionMode::AUTO;
        cfg.value = computeAutoResolution<NodeID_T, DestID_T>(g);
        cfg.initial_value = cfg.value;
    }
    
    return cfg;
}

/**
 * @brief Get resolution string for logging
 */
inline std::string resolutionModeString(ResolutionMode mode) {
    switch (mode) {
        case ResolutionMode::FIXED:   return "fixed";
        case ResolutionMode::AUTO:    return "auto";
        case ResolutionMode::DYNAMIC: return "dynamic";
        default: return "unknown";
    }
}

// ============================================================================
// SIGNAL HANDLER FOR DEBUGGING
// ============================================================================

/**
 * @brief Install SIGSEGV handler for debugging memory issues
 * 
 * Useful during development to get stack traces on segfaults.
 */
inline void install_sigsegv_handler() {
#ifdef _DEBUG
    struct sigaction sa;
    sa.sa_handler = [](int) {
        std::cerr << "SIGSEGV received - memory access error" << std::endl;
        std::abort();
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, nullptr);
#endif
}

// ============================================================================
// FAST MODULARITY (lightweight Louvain — no runGraphBrew overhead)
// ============================================================================

/**
 * @brief Hub-capped fast modularity via degree-bounded Louvain.
 *
 * Inspired by Afforest CC (Sutton et al., IPDPS 2018): instead of scanning
 * all O(m) edges, cap the per-vertex neighbor scan at HUB_CAP.
 *
 * Key insight: in power-law graphs, a few hub vertices own most edges.
 * Capping their scan to HUB_CAP (e.g. 64) strided neighbors dramatically
 * reduces total work while losing little quality — hubs' community
 * assignments can be well-estimated from a uniform sample of their neighbors.
 *
 * Algorithm:
 *  1. vtot — O(N) via out_degree (no edge scan for unweighted graphs).
 *  2. Local-moving — ROUNDS iterations, each vertex scans min(deg, HUB_CAP)
 *     neighbors (strided for hubs, full for low-degree vertices), finds
 *     best community via ΔQ with weight correction, moves if positive.
 *  3. Q estimation — same hub-capped scan to estimate cin, exact ctot.
 *
 * Complexity: O(ROUNDS × N × HUB_CAP + N) — sublinear in m for graphs
 * where hubs dominate the edge count.
 *
 * @tparam K   Community ID type (uint32_t)
 * @tparam NodeID_T  Node ID type
 * @tparam DestID_T  Destination ID type
 * @param g          CSR graph (symmetric)
 * @param resolution Resolution parameter (default 1.0)
 * @param neighborRounds Number of local-moving rounds (default 3)
 * @return Modularity value in [-0.5, 1.0]
 */
template <typename K, typename NodeID_T, typename DestID_T>
double computeFastModularity(
    const CSRGraph<NodeID_T, DestID_T, true>& g,
    double resolution = 1.0,
    int neighborRounds = 3) {

    const int64_t N = g.num_nodes();
    const double M = static_cast<double>(g.num_edges());  // total directed edges
    if (M == 0 || N == 0) return 0.0;

    // Maximum neighbors scanned per vertex per round.
    // Low-degree vertices get full scans; hubs are strided.
    static constexpr int64_t HUB_CAP = 64;

    // ── vertex total weight: O(N) for unweighted, O(m) for weighted ──
    std::vector<double> vtot(N);
    #pragma omp parallel for schedule(static, 4096)
    for (int64_t u = 0; u < N; ++u) {
        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
            vtot[u] = static_cast<double>(g.out_degree(u));
        } else {
            double s = 0.0;
            for (auto v : g.out_neigh(u)) s += static_cast<double>(v.w);
            vtot[u] = s;
        }
    }

    // ── singleton community init ──
    std::vector<K>      vcom(N);
    std::vector<double> ctot(N);
    #pragma omp parallel for schedule(static, 4096)
    for (int64_t u = 0; u < N; ++u) {
        vcom[u] = static_cast<K>(u);
        ctot[u] = vtot[u];
    }

    // ── per-thread hash table for community aggregation ──
    struct HashScan {
        std::vector<K>      keys;
        std::vector<double> vals;
        std::vector<char>   used;
        std::vector<size_t> occ;
        size_t cap  = 0;
        size_t mask = 0;

        void resize(size_t need) {
            size_t c = 64;
            while (c < need * 2) c <<= 1;
            if (c <= cap) return;
            cap  = c;
            mask = c - 1;
            keys.resize(c);
            vals.resize(c, 0.0);
            used.assign(c, 0);
            occ.reserve(c);
        }
        void clear() {
            for (size_t s : occ) { used[s] = 0; vals[s] = 0.0; }
            occ.clear();
        }
        void add(K community, double weight) {
            size_t h = std::hash<K>{}(community) & mask;
            while (true) {
                if (!used[h]) {
                    used[h] = 1; keys[h] = community; vals[h] = weight;
                    occ.push_back(h); return;
                }
                if (keys[h] == community) { vals[h] += weight; return; }
                h = (h + 1) & mask;
            }
        }
        double get(K community) const {
            size_t h = std::hash<K>{}(community) & mask;
            while (true) {
                if (!used[h]) return 0.0;
                if (keys[h] == community) return vals[h];
                h = (h + 1) & mask;
            }
        }
    };

    const int T = omp_get_max_threads();
    std::vector<HashScan> scans(T);

    // =================================================================
    // LOCAL-MOVING: hub-capped Louvain iterations
    // =================================================================
    const int ROUNDS = neighborRounds;

    for (int round = 0; round < ROUNDS; ++round) {
        int64_t moves = 0;

        #pragma omp parallel reduction(+:moves)
        {
            int tid = omp_get_thread_num();
            auto& hs = scans[tid];

            #pragma omp for schedule(dynamic, 2048)
            for (int64_t u = 0; u < N; ++u) {
                const int64_t deg = g.out_degree(u);
                if (deg == 0) continue;

                const K    cu = vcom[u];
                const double wu = vtot[u];
                const int64_t nscan = std::min(deg, HUB_CAP);

                hs.resize(static_cast<size_t>(nscan) + 1);

                // Weight correction: for hubs, scale sampled counts
                // to estimated full-neighbor counts.
                double scaleW = 1.0;

                if (deg <= HUB_CAP) {
                    // Full neighbor scan
                    for (auto nb : g.out_neigh(u)) {
                        NodeID_T v;
                        double   w;
                        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                            v = nb; w = 1.0;
                        } else {
                            v = nb.v; w = static_cast<double>(nb.w);
                        }
                        if (v == static_cast<NodeID_T>(u)) continue;
                        hs.add(vcom[v], w);
                    }
                } else {
                    // Strided scan for hubs: uniformly sample HUB_CAP neighbors
                    const int64_t stride = std::max((int64_t)1, deg / HUB_CAP);
                    scaleW = static_cast<double>(deg) / static_cast<double>(nscan);
                    int64_t count = 0;
                    for (int64_t r = 0; r < deg && count < HUB_CAP; r += stride) {
                        NodeID_T v;
                        double   w;
                        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                            v = *(g.out_neigh(u, r).begin());
                            w = 1.0;
                        } else {
                            auto nb = *(g.out_neigh(u, r).begin());
                            v = nb.v; w = static_cast<double>(nb.w);
                        }
                        if (v != static_cast<NodeID_T>(u)) {
                            hs.add(vcom[v], w);
                        }
                        count++;
                    }
                }

                // Find best community to move to
                double kout_own = hs.get(cu) * scaleW;
                K      bestC  = cu;
                double bestDQ = 0.0;

                for (size_t idx = 0; idx < hs.occ.size(); ++idx) {
                    size_t s = hs.occ[idx];
                    K      c     = hs.keys[s];
                    double vcout = hs.vals[s] * scaleW;
                    if (c == cu) continue;
                    double dQ = (vcout - kout_own) / M
                        - resolution * wu * (ctot[c] - ctot[cu] + wu) / (2.0 * M * M);
                    if (dQ > bestDQ) {
                        bestDQ = dQ;
                        bestC  = c;
                    }
                }

                if (bestC != cu) {
                    vcom[u] = bestC;
                    moves++;
                }
                hs.clear();
            }
        }

        // Recompute ctot after each round
        #pragma omp parallel for schedule(static, 4096)
        for (int64_t c = 0; c < N; ++c) ctot[c] = 0.0;
        #pragma omp parallel for schedule(static, 4096)
        for (int64_t u = 0; u < N; ++u) {
            #pragma omp atomic
            ctot[vcom[u]] += vtot[u];
        }

        if (moves == 0) break;
    }

    // =================================================================
    // Q ESTIMATION: hub-capped cin sampling + exact ctot
    // =================================================================
    // For each vertex, scan min(deg, HUB_CAP) strided neighbors to count
    // same-community edges. Scale up for hubs. This avoids a full O(m) scan.
    double cin_total = 0.0;
    #pragma omp parallel for schedule(dynamic, 2048) reduction(+:cin_total)
    for (int64_t u = 0; u < N; ++u) {
        const int64_t deg = g.out_degree(u);
        if (deg == 0) continue;
        const K cu = vcom[u];

        if (deg <= HUB_CAP) {
            // Full scan — exact cin contribution
            for (auto nb : g.out_neigh(u)) {
                NodeID_T v;
                double   w;
                if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                    v = nb; w = 1.0;
                } else {
                    v = nb.v; w = static_cast<double>(nb.w);
                }
                if (vcom[v] == cu) cin_total += w;
            }
        } else {
            // Strided sample — estimate cin contribution
            const int64_t stride = std::max((int64_t)1, deg / HUB_CAP);
            double same_w = 0.0;
            int64_t count = 0;
            for (int64_t r = 0; r < deg && count < HUB_CAP; r += stride) {
                NodeID_T v;
                double   w;
                if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                    v = *(g.out_neigh(u, r).begin());
                    w = 1.0;
                } else {
                    auto nb = *(g.out_neigh(u, r).begin());
                    v = nb.v; w = static_cast<double>(nb.w);
                }
                if (vcom[v] == cu) same_w += w;
                count++;
            }
            // Scale sampled cin to estimated full cin
            cin_total += same_w * static_cast<double>(deg)
                / static_cast<double>(std::max((int64_t)1, count));
        }
    }

    // Sum of squared community weights — O(N)
    double ctot_sq = 0.0;
    #pragma omp parallel for schedule(static, 4096) reduction(+:ctot_sq)
    for (int64_t c = 0; c < N; ++c) {
        ctot_sq += ctot[c] * ctot[c];
    }

    double Q = cin_total / (2.0 * M) - resolution * ctot_sq / (4.0 * M * M);
    return Q;
}

// ============================================================================
// MODULARITY COMPUTATION
// ============================================================================

/**
 * @brief Compute modularity for a community assignment on CSR graph
 * 
 * Modularity Q = (1/2m) * Σ[A_ij - R * k_i * k_j / (2m)] * δ(c_i, c_j)
 * where m = total edge weight, R = resolution parameter
 * 
 * Uses parallel reduction for efficiency on large graphs.
 * 
 * @tparam K Community ID type
 * @tparam NodeID_T Node ID type
 * @tparam DestID_T Destination ID type (NodeID_T for unweighted, NodeWeight for weighted)
 * @param g CSR graph
 * @param community Community assignment vector
 * @param resolution Resolution parameter (default 1.0)
 * @return Modularity value in range [-0.5, 1.0]
 */
template <typename K, typename NodeID_T, typename DestID_T>
double computeModularityCSR(
    const CSRGraph<NodeID_T, DestID_T, true>& g,
    const std::vector<K>& community,
    double resolution = 1.0) {
    
    const int64_t num_nodes = g.num_nodes();
    const bool graph_is_symmetric = !g.directed();
    const double M = static_cast<double>(g.num_edges());
    
    if (M == 0) return 0.0;
    
    // Compute vertex degrees (weighted)
    std::vector<double> vtot(num_nodes, 0.0);
    #pragma omp parallel for
    for (int64_t u = 0; u < num_nodes; ++u) {
        double total = 0.0;
        for (auto neighbor : g.out_neigh(u)) {
            if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                total += 1.0;
            } else {
                total += static_cast<double>(neighbor.w);
            }
        }
        if (!graph_is_symmetric) {
            for (auto neighbor : g.in_neigh(u)) {
                if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                    total += 1.0;
                } else {
                    total += static_cast<double>(neighbor.w);
                }
            }
        }
        vtot[u] = total;
    }
    
    // Compute modularity using parallel reduction
    double Q = 0.0;
    #pragma omp parallel for reduction(+:Q)
    for (int64_t u = 0; u < num_nodes; ++u) {
        K cu = community[u];
        
        for (auto neighbor : g.out_neigh(u)) {
            NodeID_T v;
            double w;
            if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                v = neighbor;
                w = 1.0;
            } else {
                v = neighbor.v;
                w = static_cast<double>(neighbor.w);
            }
            
            K cv = community[v];
            if (cu == cv) {
                Q += w - resolution * vtot[u] * vtot[v] / (2.0 * M);
            }
        }
        
        if (!graph_is_symmetric) {
            for (auto neighbor : g.in_neigh(u)) {
                NodeID_T v;
                double w;
                if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                    v = neighbor;
                    w = 1.0;
                } else {
                    v = neighbor.v;
                    w = static_cast<double>(neighbor.w);
                }
                
                K cv = community[v];
                if (cu == cv) {
                    Q += w - resolution * vtot[u] * vtot[v] / (2.0 * M);
                }
            }
        }
    }
    
    return Q / (2.0 * M);
}

// ============================================================================
// VERTEX EDGE SCANNING
// ============================================================================

/**
 * @brief Scan all edges connected to vertex u and accumulate by community
 * 
 * Scans both out-edges and in-edges (for non-symmetric graphs).
 * Accumulates edge weights into the hash map by community.
 * 
 * @tparam K Community ID type
 * @tparam W Weight type
 * @tparam NodeID_T Node ID type
 * @tparam DestID_T Destination ID type
 * @param u Vertex to scan
 * @param vcom Community assignment array
 * @param hash Hash map to accumulate weights by community
 * @param d Target community to track separately
 * @param g CSR graph
 * @param graph_is_symmetric Whether graph is symmetric (avoids double-counting)
 * @return Total weight to community d
 */
template <typename K, typename W, typename NodeID_T, typename DestID_T>
inline W scanVertexEdges(
    NodeID_T u,
    const K* vcom,
    std::unordered_map<K, W>& hash,
    K d,
    const CSRGraph<NodeID_T, DestID_T, true>& g,
    bool graph_is_symmetric) {
    
    W ku_to_d = W(0);
    
    // Scan out-neighbors
    for (auto neighbor : g.out_neigh(u)) {
        NodeID_T v;
        W w;
        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
            v = neighbor;
            w = W(1);
        } else {
            v = neighbor.v;
            w = static_cast<W>(neighbor.w);
        }
        
        K c = vcom[v];
        hash[c] += w;
        if (c == d) ku_to_d += w;
    }
    
    // For non-symmetric graphs, also scan in-neighbors
    if (!graph_is_symmetric) {
        for (auto neighbor : g.in_neigh(u)) {
            NodeID_T v;
            W w;
            if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                v = neighbor;
                w = W(1);
            } else {
                v = neighbor.v;
                w = static_cast<W>(neighbor.w);
            }
            
            K c = vcom[v];
            hash[c] += w;
            if (c == d) ku_to_d += w;
        }
    }
    
    return ku_to_d;
}

/**
 * @brief Compute total edge weight for a vertex
 * 
 * @tparam W Weight type
 * @tparam NodeID_T Node ID type
 * @tparam DestID_T Destination ID type
 * @param u Vertex ID
 * @param g CSR graph
 * @param graph_is_symmetric Whether graph is symmetric
 * @return Total edge weight for vertex u
 */
template <typename W, typename NodeID_T, typename DestID_T>
inline W computeVertexTotalWeightCSR(
    NodeID_T u,
    const CSRGraph<NodeID_T, DestID_T, true>& g,
    bool graph_is_symmetric) {
    
    W total = W(0);
    
    for (auto neighbor : g.out_neigh(u)) {
        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
            total += W(1);
        } else {
            total += static_cast<W>(neighbor.w);
        }
    }
    
    if (!graph_is_symmetric) {
        for (auto neighbor : g.in_neigh(u)) {
            if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                total += W(1);
            } else {
                total += static_cast<W>(neighbor.w);
            }
        }
    }
    
    return total;
}

/**
 * @brief Mark all neighbors of a vertex as affected
 * 
 * Used in local-moving phase to mark vertices that need re-evaluation
 * after a neighbor has moved communities.
 * 
 * @tparam NodeID_T Node ID type
 * @tparam DestID_T Destination ID type
 * @param u Vertex whose neighbors should be marked
 * @param vaff Affected flag array
 * @param g CSR graph
 * @param graph_is_symmetric Whether graph is symmetric
 */
template <typename NodeID_T, typename DestID_T>
inline void markNeighborsAffected(
    NodeID_T u,
    std::vector<char>& vaff,
    const CSRGraph<NodeID_T, DestID_T, true>& g,
    bool graph_is_symmetric) {
    
    for (auto neighbor : g.out_neigh(u)) {
        NodeID_T v;
        if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
            v = neighbor;
        } else {
            v = neighbor.v;
        }
        vaff[v] = 1;
    }
    
    if (!graph_is_symmetric) {
        for (auto neighbor : g.in_neigh(u)) {
            NodeID_T v;
            if constexpr (std::is_same_v<DestID_T, NodeID_T>) {
                v = neighbor;
            } else {
                v = neighbor.v;
            }
            vaff[v] = 1;
        }
    }
}

/**
 * @brief Build dendrogram from community assignments
 * 
 * Creates a hierarchical structure from flat community assignments.
 * For each community, the vertex with highest weight becomes the representative (root),
 * and all other vertices in that community become children of the representative.
 *
 * @tparam K Community ID type
 * @tparam W Weight type
 * @param dendro Dendrogram result structure to populate
 * @param vcom Community assignment for each vertex
 * @param vtot Total weight (degree) of each vertex
 * @param num_nodes Number of nodes in the graph
 */
template <typename K, typename W>
void buildDendrogramFromCommunities(
    GVEDendroResult<K>& dendro,
    const std::vector<K>& vcom,
    const std::vector<W>& vtot,
    int64_t num_nodes) {
    
    // Use vector instead of unordered_map for efficiency (community ID = vertex ID)
    // Track representative per community: rep[c] = vertex with max weight in community c
    std::vector<int64_t> comm_rep(num_nodes, -1);
    
    // Find representative (highest weight vertex) for each community
    for (int64_t v = 0; v < num_nodes; ++v) {
        K c = vcom[v];
        if (comm_rep[c] == -1 || vtot[v] > vtot[comm_rep[c]]) {
            comm_rep[c] = v;
        }
    }
    
    // Clear existing dendrogram structure (rebuild fresh each pass)
    #pragma omp parallel for
    for (int64_t v = 0; v < num_nodes; ++v) {
        dendro.first_child[v] = -1;
        dendro.sibling[v] = -1;
        dendro.parent[v] = -1;
        dendro.subtree_size[v] = 1;
        dendro.weight[v] = vtot[v];
    }
    
    // Link non-representatives to their representative (sequential to avoid races)
    for (int64_t v = 0; v < num_nodes; ++v) {
        K c = vcom[v];
        int64_t rep = comm_rep[c];
        if (v != rep && rep >= 0) {
            // Prepend v to rep's child list
            dendro.sibling[v] = dendro.first_child[rep];
            dendro.first_child[rep] = v;
            dendro.parent[v] = rep;
            dendro.subtree_size[rep] += dendro.subtree_size[v];
            dendro.weight[rep] += dendro.weight[v];
        }
    }
}

/**
 * Traverse dendrogram in DFS order to produce vertex ordering.
 * 
 * @tparam K Community ID type
 * @tparam NodeID_T Node ID type for new_ids array
 * @param dendro The dendrogram structure to traverse
 * @param new_ids Output array mapping original vertex -> new position
 * @param hub_first If true, visit higher-weight children first
 */
template <typename K, typename NodeID_T>
void traverseDendrogramDFS(
    const GVEDendroResult<K>& dendro,
    std::vector<NodeID_T>& new_ids,
    bool hub_first = true) {
    
    const int64_t num_nodes = static_cast<int64_t>(dendro.parent.size());
    
    // Find root nodes (nodes with no parent)
    std::vector<int64_t> roots;
    for (int64_t v = 0; v < num_nodes; ++v) {
        if (dendro.parent[v] == -1) {
            roots.push_back(v);
        }
    }
    
    // Sort roots by weight (hub-first)
    if (hub_first) {
        std::sort(roots.begin(), roots.end(),
            [&dendro](int64_t a, int64_t b) {
                return dendro.weight[a] > dendro.weight[b];
            });
    }
    
    // DFS traversal with stack
    std::vector<int64_t> stack;
    int64_t current_id = 0;
    
    for (int64_t root : roots) {
        stack.push_back(root);
        
        while (!stack.empty()) {
            int64_t v = stack.back();
            stack.pop_back();
            
            // Assign new ID
            new_ids[v] = current_id++;
            
            // Collect children and sort by weight if hub-first
            std::vector<int64_t> children;
            int64_t child = dendro.first_child[v];
            while (child != -1) {
                children.push_back(child);
                child = dendro.sibling[child];
            }
            
            if (hub_first && !children.empty()) {
                std::sort(children.begin(), children.end(),
                    [&dendro](int64_t a, int64_t b) {
                        return dendro.weight[a] < dendro.weight[b]; // Reverse for stack
                    });
            }
            
            // Push children to stack (in reverse order for correct DFS)
            for (int64_t c : children) {
                stack.push_back(c);
            }
        }
    }
}

// ============================================================================
// LEIDEN DENDROGRAM HELPERS
// ============================================================================

/**
 * Build dendrogram from Leiden's per-pass community mappings (PARALLEL VERSION)
 * 
 * Uses parallel sorting instead of hash maps for O(n log n) parallel grouping.
 * Key optimizations:
 * 1. Parallel sort by community ID for grouping
 * 2. Parallel scan to find community boundaries
 * 3. Parallel creation of internal nodes
 * 
 * @param nodes Output: dendrogram nodes (resized and filled)
 * @param roots Output: indices of root nodes (top-level communities)
 * @param communityMappingPerPass Per-pass community assignments from Leiden
 * @param degrees Vertex degrees for weight computation
 * @param num_vertices Number of vertices in graph
 */
template<typename K>
void buildLeidenDendrogram(
    std::vector<LeidenDendrogramNode>& nodes,
    std::vector<int64_t>& roots,
    const std::vector<std::vector<K>>& communityMappingPerPass,
    const std::vector<K>& degrees,
    size_t num_vertices) {
    
    const size_t num_passes = communityMappingPerPass.size();
    
    // Create leaf nodes for all vertices (parallel)
    nodes.resize(num_vertices);
    #pragma omp parallel for
    for (size_t v = 0; v < num_vertices; ++v) {
        nodes[v].vertex_id = v;
        nodes[v].subtree_size = 1;
        nodes[v].weight = degrees[v];
        nodes[v].level = 0;
    }
    
    if (num_passes == 0) {
        roots.resize(num_vertices);
        #pragma omp parallel for
        for (size_t v = 0; v < num_vertices; ++v) {
            roots[v] = v;
        }
        return;
    }
    
    // Build hierarchy from finest to coarsest
    std::vector<int64_t> current_nodes(num_vertices);
    #pragma omp parallel for
    for (size_t i = 0; i < num_vertices; ++i) {
        current_nodes[i] = i;
    }
    
    for (size_t pass = 0; pass < num_passes; ++pass) {
        const auto& comm_map = communityMappingPerPass[pass];
        const size_t n_current = current_nodes.size();
        
        // Step 1: Create (community, node_id, weight) tuples (parallel)
        std::vector<std::tuple<K, int64_t, double>> node_comm(n_current);
        
        #pragma omp parallel for
        for (size_t i = 0; i < n_current; ++i) {
            int64_t node_id = current_nodes[i];
            // Find representative vertex (first leaf in subtree)
            int64_t v = node_id;
            while (v >= 0 && nodes[v].vertex_id < 0) {
                v = nodes[v].first_child;
            }
            K comm = 0;
            if (v >= 0 && nodes[v].vertex_id >= 0) {
                size_t vertex = nodes[v].vertex_id;
                if (vertex < comm_map.size()) {
                    comm = comm_map[vertex];
                }
            }
            node_comm[i] = std::make_tuple(comm, node_id, nodes[node_id].weight);
        }
        
        // Step 2: Parallel sort by community, then by weight (descending for hub-first)
        __gnu_parallel::sort(node_comm.begin(), node_comm.end(),
            [](const auto& a, const auto& b) {
                if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
                return std::get<2>(a) > std::get<2>(b);  // Higher weight first
            });
        
        // Step 3: Find community boundaries (parallel scan)
        std::vector<size_t> is_boundary(n_current + 1, 0);
        is_boundary[0] = 1;
        
        #pragma omp parallel for
        for (size_t i = 1; i < n_current; ++i) {
            if (std::get<0>(node_comm[i]) != std::get<0>(node_comm[i-1])) {
                is_boundary[i] = 1;
            }
        }
        is_boundary[n_current] = 1;
        
        // Step 4: Collect boundary indices
        std::vector<size_t> boundaries;
        boundaries.reserve(n_current / 2);
        for (size_t i = 0; i <= n_current; ++i) {
            if (is_boundary[i]) {
                boundaries.push_back(i);
            }
        }
        
        const size_t num_communities = boundaries.size() - 1;
        
        // Step 5: Count communities needing internal nodes
        std::vector<size_t> needs_internal(num_communities);
        #pragma omp parallel for
        for (size_t c = 0; c < num_communities; ++c) {
            size_t start = boundaries[c];
            size_t end = boundaries[c + 1];
            needs_internal[c] = (end - start > 1) ? 1 : 0;
        }
        
        // Prefix sum for internal node offsets
        std::vector<size_t> internal_offset(num_communities + 1);
        internal_offset[0] = 0;
        for (size_t c = 0; c < num_communities; ++c) {
            internal_offset[c + 1] = internal_offset[c] + needs_internal[c];
        }
        size_t num_new_internals = internal_offset[num_communities];
        
        // Reserve space for new internal nodes
        size_t internal_base = nodes.size();
        nodes.resize(internal_base + num_new_internals);
        
        // Step 6: Create internal nodes and link children
        std::vector<int64_t> next_nodes(num_communities);
        
        #pragma omp parallel for schedule(dynamic, 64)
        for (size_t c = 0; c < num_communities; ++c) {
            size_t start = boundaries[c];
            size_t end = boundaries[c + 1];
            size_t member_count = end - start;
            
            if (member_count == 1) {
                next_nodes[c] = std::get<1>(node_comm[start]);
            } else {
                int64_t internal_id = internal_base + internal_offset[c];
                nodes[internal_id].vertex_id = -1;
                nodes[internal_id].level = pass + 1;
                nodes[internal_id].subtree_size = 0;
                nodes[internal_id].weight = 0.0;
                nodes[internal_id].first_child = -1;
                nodes[internal_id].sibling = -1;
                nodes[internal_id].parent = -1;
                
                int64_t prev_sibling = -1;
                for (size_t i = start; i < end; ++i) {
                    int64_t child_id = std::get<1>(node_comm[i]);
                    nodes[child_id].parent = internal_id;
                    
                    if (nodes[internal_id].first_child == -1) {
                        nodes[internal_id].first_child = child_id;
                    }
                    if (prev_sibling >= 0) {
                        nodes[prev_sibling].sibling = child_id;
                    }
                    prev_sibling = child_id;
                    
                    nodes[internal_id].subtree_size += nodes[child_id].subtree_size;
                    nodes[internal_id].weight += nodes[child_id].weight;
                }
                
                next_nodes[c] = internal_id;
            }
        }
        
        current_nodes = std::move(next_nodes);
    }
    
    // Copy roots and sort by subtree size
    roots = current_nodes;
    __gnu_parallel::sort(roots.begin(), roots.end(), [&nodes](int64_t a, int64_t b) {
        return nodes[a].subtree_size > nodes[b].subtree_size;
    });
}

/**
 * Parallel DFS ordering of Leiden dendrogram
 * 
 * Uses subtree sizes to compute DFS positions in parallel:
 * 1. Compute DFS start position for each subtree using prefix sums
 * 2. Process all vertices in parallel using their computed positions
 * 
 * @param nodes Dendrogram nodes (modified: dfs_start field updated)
 * @param roots Root node indices
 * @param new_ids Output permutation array
 * @param hub_first If true, visit higher-weight children first
 * @param size_first If true, visit larger subtrees first
 */
template <typename NodeID_T>
void orderDendrogramDFSParallel(
    std::vector<LeidenDendrogramNode>& nodes,
    const std::vector<int64_t>& roots,
    pvector<NodeID_T>& new_ids,
    bool hub_first,
    bool size_first) {
    
    const size_t num_nodes = nodes.size();
    
    // Step 1: Compute DFS start positions for each root's subtree
    size_t total_vertices = 0;
    for (int64_t root : roots) {
        nodes[root].dfs_start = total_vertices;
        total_vertices += nodes[root].subtree_size;
    }
    
    // Step 2: Propagate DFS positions down the tree (BFS order)
    std::vector<int64_t> current_level;
    current_level.reserve(roots.size());
    for (int64_t root : roots) {
        current_level.push_back(root);
    }
    
    while (!current_level.empty()) {
        std::vector<int64_t> next_level;
        next_level.reserve(current_level.size() * 2);
        
        for (int64_t node_id : current_level) {
            auto& node = nodes[node_id];
            
            if (node.vertex_id >= 0) {
                // Leaf node - processed in final step
                continue;
            }
            
            // Collect and optionally sort children
            std::vector<int64_t> children;
            int64_t child = node.first_child;
            while (child >= 0) {
                children.push_back(child);
                child = nodes[child].sibling;
            }
            
            if (hub_first) {
                std::sort(children.begin(), children.end(), 
                     [&nodes](int64_t a, int64_t b) {
                         return nodes[a].weight > nodes[b].weight;
                     });
            } else if (size_first) {
                std::sort(children.begin(), children.end(),
                     [&nodes](int64_t a, int64_t b) {
                         return nodes[a].subtree_size > nodes[b].subtree_size;
                     });
            }
            
            // Assign DFS start positions to children
            int64_t pos = node.dfs_start;
            for (int64_t child_id : children) {
                nodes[child_id].dfs_start = pos;
                pos += nodes[child_id].subtree_size;
                next_level.push_back(child_id);
            }
        }
        
        current_level = std::move(next_level);
    }
    
    // Step 3: Assign new IDs to leaf nodes in parallel
    #pragma omp parallel for
    for (size_t i = 0; i < num_nodes; ++i) {
        if (nodes[i].vertex_id >= 0) {
            new_ids[nodes[i].vertex_id] = nodes[i].dfs_start;
        }
    }
}

/**
 * Sequential DFS ordering of dendrogram (original version for comparison)
 * 
 * @param nodes Dendrogram nodes
 * @param roots Root node indices
 * @param new_ids Output permutation array
 * @param hub_first Sort children by weight (hub-first) if true
 * @param size_first Sort children by subtree size if true
 */
template <typename NodeID_>
void orderDendrogramDFS(
    const std::vector<LeidenDendrogramNode>& nodes,
    const std::vector<int64_t>& roots,
    pvector<NodeID_>& new_ids,
    bool hub_first,
    bool size_first) {
    
    NodeID_ current_id = 0;
    std::deque<int64_t> stack;
    
    for (int64_t root : roots) {
        stack.push_back(root);
        
        while (!stack.empty()) {
            int64_t node_id = stack.back();
            stack.pop_back();
            
            const auto& node = nodes[node_id];
            
            if (node.vertex_id >= 0) {
                new_ids[node.vertex_id] = current_id++;
            } else {
                std::vector<int64_t> children;
                int64_t child = node.first_child;
                while (child >= 0) {
                    children.push_back(child);
                    child = nodes[child].sibling;
                }
                
                if (hub_first) {
                    std::sort(children.begin(), children.end(), 
                         [&nodes](int64_t a, int64_t b) {
                             return nodes[a].weight > nodes[b].weight;
                         });
                } else if (size_first) {
                    std::sort(children.begin(), children.end(),
                         [&nodes](int64_t a, int64_t b) {
                             return nodes[a].subtree_size > nodes[b].subtree_size;
                         });
                }
                
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    stack.push_back(*it);
                }
            }
        }
    }
}

/**
 * BFS ordering of dendrogram (by level)
 * 
 * @param nodes Dendrogram nodes
 * @param roots Root node indices
 * @param new_ids Output permutation array
 */
template <typename NodeID_>
void orderDendrogramBFS(
    const std::vector<LeidenDendrogramNode>& nodes,
    const std::vector<int64_t>& roots,
    pvector<NodeID_>& new_ids) {
    
    NodeID_ current_id = 0;
    std::deque<int64_t> queue;
    
    for (int64_t root : roots) {
        queue.push_back(root);
    }
    
    while (!queue.empty()) {
        int64_t node_id = queue.front();
        queue.pop_front();
        
        const auto& node = nodes[node_id];
        
        if (node.vertex_id >= 0) {
            new_ids[node.vertex_id] = current_id++;
        } else {
            int64_t child = node.first_child;
            while (child >= 0) {
                queue.push_back(child);
                child = nodes[child].sibling;
            }
        }
    }
}

/**
 * Hybrid ordering: sort by (community, degree descending)
 * 
 * This is a simple ordering that places vertices within the same community
 * together, with higher-degree vertices first within each community.
 * 
 * @tparam K Community ID type
 * @tparam NodeID_ Node ID type
 * @param communityMappingPerPass Community assignments per Leiden pass
 * @param degrees Degree of each vertex
 * @param new_ids Output permutation array
 */
template<typename K, typename NodeID_>
void orderLeidenHybridHubDFS(
    const std::vector<std::vector<K>>& communityMappingPerPass,
    const std::vector<K>& degrees,
    pvector<NodeID_>& new_ids) {
    
    const size_t n = degrees.size();
    
    if (communityMappingPerPass.empty()) {
        // No community - sort by degree
        std::vector<std::pair<K, size_t>> deg_vertex(n);
        #pragma omp parallel for
        for (size_t v = 0; v < n; ++v) {
            deg_vertex[v] = {degrees[v], v};
        }
        __gnu_parallel::sort(deg_vertex.begin(), deg_vertex.end(), std::greater<>());
        #pragma omp parallel for
        for (size_t i = 0; i < n; ++i) {
            new_ids[deg_vertex[i].second] = i;
        }
        return;
    }
    
    // Get last-pass communities
    const auto& last_pass = communityMappingPerPass.back();
    
    // Create (vertex, community, degree) tuples
    std::vector<std::tuple<size_t, K, K>> vertices(n);
    #pragma omp parallel for
    for (size_t v = 0; v < n; ++v) {
        vertices[v] = std::make_tuple(v, last_pass[v], degrees[v]);
    }
    
    // Sort by (community, degree descending)
    __gnu_parallel::sort(vertices.begin(), vertices.end(),
        [](const auto& a, const auto& b) {
            if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
            return std::get<2>(a) > std::get<2>(b);
        });
    
    #pragma omp parallel for
    for (size_t i = 0; i < n; ++i) {
        new_ids[std::get<0>(vertices[i])] = i;
    }
}

// ============================================================================
// JSON PARSING FOR PERCEPTRON WEIGHTS
// ============================================================================

/**
 * @brief Data-driven JSON parser for perceptron weights file
 * 
 * Parses any JSON file with format:
 * {
 *   "ORIGINAL": {"bias": 1.0, "w_modularity": 0.3, ...},
 *   "GraphBrewOrder_leiden": {"bias": 0.95, ...},
 *   ...
 * }
 * 
 * Discovery strategy (no hardcoded variant list needed):
 *   1. All base algorithm names from getAlgorithmNameMap() 
 *   2. Any variant-prefixed keys (GraphBrewOrder_*, RABBITORDER_*, RCM_*)
 *      auto-discovered directly from the JSON content
 *
 * This means adding a new variant in Python training automatically makes
 * its weights loadable in C++ — zero C++ source changes required.
 * 
 * @param json_content String containing JSON content
 * @param weights Output map to populate with parsed weights (string-keyed)
 * @return true if at least one algorithm was successfully parsed
 */
inline bool ParseWeightsFromJSON(const std::string& json_content, 
                                  std::map<std::string, PerceptronWeights>& weights) {
    // Simple JSON field parser
    auto find_double = [](const std::string& s, const std::string& key) -> double {
        size_t pos = s.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0.0;
        pos = s.find(':', pos);
        if (pos == std::string::npos) return 0.0;
        pos++;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
        size_t end = pos;
        while (end < s.size() && (isdigit(s[end]) || s[end] == '.' || s[end] == '-' || s[end] == 'e' || s[end] == 'E' || s[end] == '+')) end++;
        try {
            return std::stod(s.substr(pos, end - pos));
        } catch (...) {
            return 0.0;
        }
    };
    
    // Find matching closing brace, accounting for nested objects
    auto find_matching_brace = [](const std::string& s, size_t open_pos) -> size_t {
        int depth = 1;
        for (size_t i = open_pos + 1; i < s.size(); i++) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') {
                depth--;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    };
    
    // ---- Step 1: Collect candidate algorithm keys ----
    // (a) Base names from the algorithm map
    std::set<std::string> candidate_keys;
    for (const auto& kv : getAlgorithmNameMap()) {
        candidate_keys.insert(kv.first);
    }
    
    // (b) Auto-discover variant-prefixed keys from the JSON content
    //     This makes new Python-trained variants loadable with zero C++ changes.
    for (size_t pi = 0; pi < VARIANT_PREFIX_COUNT; ++pi) {
        std::string prefix = VARIANT_PREFIXES[pi];
        std::string needle = "\"" + prefix;
        size_t pos = 0;
        while ((pos = json_content.find(needle, pos)) != std::string::npos) {
            size_t key_start = pos + 1;  // skip opening quote
            size_t key_end = json_content.find('"', key_start);
            if (key_end == std::string::npos) break;
            candidate_keys.insert(json_content.substr(key_start, key_end - key_start));
            pos = key_end + 1;
        }
    }
    
    // ---- Step 2: Parse weights for each discovered key ----
    for (const auto& key : candidate_keys) {
        size_t pos = json_content.find("\"" + key + "\"");
        if (pos == std::string::npos) continue;
        
        // Find the block for this algorithm (with proper brace matching
        // to handle nested objects like benchmark_weights and _metadata)
        size_t start = json_content.find('{', pos);
        if (start == std::string::npos) continue;
        size_t end = find_matching_brace(json_content, start);
        if (end == std::string::npos) continue;
        
        std::string block = json_content.substr(start, end - start + 1);
        
        PerceptronWeights w;
        // Core weights
        w.bias = find_double(block, "bias");
        w.w_modularity = find_double(block, "w_modularity");
        w.w_log_nodes = find_double(block, "w_log_nodes");
        w.w_log_edges = find_double(block, "w_log_edges");
        w.w_density = find_double(block, "w_density");
        w.w_avg_degree = find_double(block, "w_avg_degree");
        w.w_degree_variance = find_double(block, "w_degree_variance");
        w.w_hub_concentration = find_double(block, "w_hub_concentration");
        
        // Extended graph structure weights
        w.w_clustering_coeff = find_double(block, "w_clustering_coeff");
        w.w_avg_path_length = find_double(block, "w_avg_path_length");
        w.w_diameter = find_double(block, "w_diameter");
        w.w_community_count = find_double(block, "w_community_count");
        
        // Cache impact weights
        w.cache_l1_impact = find_double(block, "cache_l1_impact");
        w.cache_l2_impact = find_double(block, "cache_l2_impact");
        w.cache_l3_impact = find_double(block, "cache_l3_impact");
        w.cache_dram_penalty = find_double(block, "cache_dram_penalty");
        
        // Reorder time weight
        w.w_reorder_time = find_double(block, "w_reorder_time");
        
        // Locality feature weights (IISWC'18 / GoGraph)
        w.w_packing_factor = find_double(block, "w_packing_factor");
        w.w_forward_edge_fraction = find_double(block, "w_forward_edge_fraction");
        
        // System-cache feature weight (P-OPT)
        w.w_working_set_ratio = find_double(block, "w_working_set_ratio");
        
        // Quadratic interaction weights
        w.w_dv_x_hub = find_double(block, "w_dv_x_hub");
        w.w_mod_x_logn = find_double(block, "w_mod_x_logn");
        w.w_pf_x_wsr = find_double(block, "w_pf_x_wsr");
        w.w_vss_x_hc = find_double(block, "w_vss_x_hc");
        w.w_wno_x_pf = find_double(block, "w_wno_x_pf");
        
        // DON-RL feature weights (Zhao et al.)
        w.w_vertex_significance_skewness = find_double(block, "w_vertex_significance_skewness");
        w.w_window_neighbor_overlap = find_double(block, "w_window_neighbor_overlap");
        
        // Convergence bonus weight (GoGraph)
        w.w_fef_convergence = find_double(block, "w_fef_convergence");
        
        // P1 3.1d: Sampled locality score weight
        w.w_sampled_locality = find_double(block, "w_sampled_locality");
        
        // P3 3.2: Transpose reuse distance weight
        w.w_avg_reuse_distance = find_double(block, "w_avg_reuse_distance");
        
        // Paper-aligned feature weights
        w.w_packing_factor_cl = find_double(block, "w_packing_factor_cl");
        w.w_wsr_l1 = find_double(block, "w_wsr_l1");
        w.w_wsr_l2 = find_double(block, "w_wsr_l2");
        w.w_locality_score_pairwise = find_double(block, "w_locality_score_pairwise");
        w.w_reuse_distance_lru = find_double(block, "w_reuse_distance_lru");
        
        // P1 1.4: Platt scaling parameters
        w.platt_A = find_double(block, "platt_A");
        w.platt_B = find_double(block, "platt_B");
        
        // Parse _metadata block for avg_speedup and avg_reorder_time
        size_t meta_pos = block.find("\"_metadata\"");
        if (meta_pos != std::string::npos) {
            size_t meta_start = block.find('{', meta_pos);
            size_t meta_end = find_matching_brace(block, meta_start);
            if (meta_start != std::string::npos && meta_end != std::string::npos) {
                std::string meta_block = block.substr(meta_start, meta_end - meta_start + 1);
                double speedup = find_double(meta_block, "avg_speedup");
                double reorder_time = find_double(meta_block, "avg_reorder_time");
                w.avg_speedup = (speedup > 0) ? speedup : 1.0;
                w.avg_reorder_time = (reorder_time > 0) ? reorder_time : 0.0;
            }
        }
        
        // Benchmark-specific weights (parse nested benchmark_weights object)
        size_t bw_pos = block.find("\"benchmark_weights\"");
        if (bw_pos != std::string::npos) {
            size_t bw_start = block.find('{', bw_pos);
            size_t bw_end = find_matching_brace(block, bw_start);
            if (bw_start != std::string::npos && bw_end != std::string::npos) {
                std::string bw_block = block.substr(bw_start, bw_end - bw_start + 1);
                auto find_bench_weight = [&](const std::string& blk, const std::string& bkey) -> double {
                    if (blk.find("\"" + bkey + "\"") == std::string::npos) return 1.0;
                    return find_double(blk, bkey);
                };
                w.bench_pr   = find_bench_weight(bw_block, "pr");
                w.bench_bfs  = find_bench_weight(bw_block, "bfs");
                w.bench_cc   = find_bench_weight(bw_block, "cc");
                w.bench_sssp = find_bench_weight(bw_block, "sssp");
                w.bench_bc   = find_bench_weight(bw_block, "bc");
                w.bench_tc   = find_bench_weight(bw_block, "tc");
                w.bench_pr_spmv = find_bench_weight(bw_block, "pr_spmv");
                w.bench_cc_sv   = find_bench_weight(bw_block, "cc_sv");
            }
        }
        
        // Store with the exact string key from the JSON — each variant
        // (e.g. GraphBrewOrder_leiden vs GraphBrewOrder_rabbit) gets its own
        // entry so the perceptron can select at variant granularity.
        weights[key] = w;
    }
    
    // ---- Step 3: Parse _normalization block (Mode 5) ----
    // If present, weights are in z-score space and features must be
    // z-normalized before scoring.  Parse feat_means and feat_stds arrays.
    {
        size_t norm_pos = json_content.find("\"_normalization\"");
        if (norm_pos != std::string::npos) {
            size_t norm_start = json_content.find('{', norm_pos);
            size_t norm_end = find_matching_brace(json_content, norm_start);
            if (norm_start != std::string::npos && norm_end != std::string::npos) {
                std::string norm_block = json_content.substr(norm_start, norm_end - norm_start + 1);
                
                // Parse JSON double arrays: "key": [v0, v1, ..., v16]
                auto find_double_array = [](const std::string& s, const std::string& key,
                                            double* out, int max_n) -> int {
                    size_t pos = s.find("\"" + key + "\"");
                    if (pos == std::string::npos) return 0;
                    pos = s.find('[', pos);
                    if (pos == std::string::npos) return 0;
                    pos++; // skip '['
                    int count = 0;
                    while (pos < s.size() && count < max_n) {
                        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == '\n' || s[pos] == '\r' || s[pos] == ','))
                            pos++;
                        if (pos >= s.size() || s[pos] == ']') break;
                        size_t end = pos;
                        while (end < s.size() && (isdigit(s[end]) || s[end] == '.' ||
                               s[end] == '-' || s[end] == 'e' || s[end] == 'E' || s[end] == '+'))
                            end++;
                        try {
                            out[count++] = std::stod(s.substr(pos, end - pos));
                        } catch (...) {
                            break;
                        }
                        pos = end;
                    }
                    return count;
                };
                
                double means[PerceptronWeights::N_FEATURES] = {};
                double stds[PerceptronWeights::N_FEATURES] = {};
                int n_means = find_double_array(norm_block, "feat_means", means,
                                                 PerceptronWeights::N_FEATURES);
                int n_stds = find_double_array(norm_block, "feat_stds", stds,
                                                PerceptronWeights::N_FEATURES);
                
                if (n_means == PerceptronWeights::N_FEATURES &&
                    n_stds == PerceptronWeights::N_FEATURES) {
                    // Apply normalization stats to all parsed algorithm weights
                    for (auto& kv : weights) {
                        kv.second.use_normalization = true;
                        std::copy(means, means + PerceptronWeights::N_FEATURES,
                                  kv.second.norm_mean);
                        std::copy(stds, stds + PerceptronWeights::N_FEATURES,
                                  kv.second.norm_std);
                    }
                }
            }
        }
    }
    
    return !weights.empty();
}

// ============================================================================
// DEFAULT PERCEPTRON WEIGHTS FOR ALL ALGORITHMS
// ============================================================================

/**
 * @brief Get default perceptron weights for all reordering algorithms
 * 
 * These weights are used by AdaptiveOrder when no trained weights are available.
 * They encode domain knowledge about when each algorithm performs well:
 * 
 * Weight interpretation:
 * - Positive weight = algorithm prefers higher values of that feature
 * - Negative weight = algorithm prefers lower values
 * 
 * Key observations from benchmarks:
 * - GraphBrewOrder: Best overall with community-sort/12:community (avg 2.86x speedup)
 * - RabbitOrder: Best on low-modularity synthetic graphs (4.06x)
 * - HubClusterDBG: Good general-purpose (2.38x), fast reordering
 * - RCMOrder: Best for sparse graphs (high w_density penalty on dense)
 * - GraphBrewOrder: Mixed - good baseline but overhead matters on small graphs
 * - AdaptiveOrder: Recursive application shows benefit on large communities
 * 
 * @return Map of algorithm enum to default PerceptronWeights
 */
inline const std::map<std::string, PerceptronWeights>& GetPerceptronWeights() {
    static const std::map<std::string, PerceptronWeights> weights = {
        // ORIGINAL: baseline, no reordering overhead
        {"ORIGINAL", {
            .bias = 1.0,
            .w_modularity = 0.3,      // good on high-mod (no overhead)
            .w_log_nodes = -0.05,     // worse as graph grows
            .w_log_edges = -0.02,
            .w_density = 0.0,
            .w_avg_degree = 0.0,
            .w_degree_variance = -0.1,
            .w_hub_concentration = -0.1,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = 0.0
        }},
        // HubSort: light reordering, puts hubs first
        {"HUBSORT", {
            .bias = 0.85,
            .w_modularity = 0.0,
            .w_log_nodes = 0.02,
            .w_log_edges = 0.02,
            .w_density = -0.5,        // worse on dense
            .w_avg_degree = 0.02,
            .w_degree_variance = 0.15,
            .w_hub_concentration = 0.25,  // best when hubs dominate
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.1     // fast reordering
        }},
        // HubCluster: groups hubs together
        {"HUBCLUSTER", {
            .bias = 0.82,
            .w_modularity = 0.05,
            .w_log_nodes = 0.03,
            .w_log_edges = 0.03,
            .w_density = -0.3,
            .w_avg_degree = 0.03,
            .w_degree_variance = 0.2,
            .w_hub_concentration = 0.3,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.1
        }},
        // DBG: degree-based grouping
        {"DBG", {
            .bias = 0.8,
            .w_modularity = -0.1,     // better on low-mod
            .w_log_nodes = 0.02,
            .w_log_edges = 0.02,
            .w_density = -0.4,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.25,    // best with varied degrees
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.05
        }},
        // HubClusterDBG: combination approach
        // Benchmark: avg_speedup=2.38x, good but outperformed by Leiden variants
        {"HUBCLUSTERDBG", {
            .bias = 0.62,             // lowered - Leiden variants are better
            .w_modularity = 0.10,     // better on higher mod
            .w_log_nodes = 0.04,
            .w_log_edges = 0.04,
            .w_density = -0.20,
            .w_avg_degree = 0.02,
            .w_degree_variance = 0.25,
            .w_hub_concentration = 0.20,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.1
        }},
#ifdef RABBIT_ENABLE
        // RabbitOrder: recursive bisection
        // Benchmark: avg_speedup=4.06x, good on social networks
        {"RABBITORDER_csr", {
            .bias = 0.70,
            .w_modularity = -0.30,    // better on LOW modularity (synth graphs)
            .w_log_nodes = 0.05,      // scales well
            .w_log_edges = 0.05,
            .w_density = -0.30,       // worse on dense
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.20,
            .w_hub_concentration = 0.15,
            .w_clustering_coeff = 0.05, .w_avg_path_length = -0.02, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.3     // higher reorder cost
        }},
#endif
        // RCM_default: reverse Cuthill-McKee (GoGraph baseline)
        {"RCM_default", {
            .bias = 0.7,
            .w_modularity = 0.0,
            .w_log_nodes = 0.03,
            .w_log_edges = 0.02,
            .w_density = -0.8,        // strong penalty on dense
            .w_avg_degree = -0.03,    // worse on high degree
            .w_degree_variance = 0.05,
            .w_hub_concentration = 0.0,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.1, .w_diameter = 0.05, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.2
        }},
        // LeidenOrder: community-based
        // Benchmark: avg_speedup=4.40x (highest trial speedup but high reorder cost)
        {"LeidenOrder", {
            .bias = 0.76,             // win_trial=2, high trial speedup
            .w_modularity = 0.40,     // best on high modularity
            .w_log_nodes = 0.05,
            .w_log_edges = 0.05,
            .w_density = -0.20,
            .w_avg_degree = 0.01,
            .w_degree_variance = 0.10,
            .w_hub_concentration = 0.10,
            .w_clustering_coeff = 0.1, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.05,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.4     // high reorder cost
        }},
        // GraphBrewOrder_leiden: Leiden + per-community RabbitOrder (default variant)
        {"GraphBrewOrder_leiden", {
            .bias = 0.6,
            .w_modularity = -0.25,    // better on low-mod (community reorder helps)
            .w_log_nodes = 0.1,       // scales well
            .w_log_edges = 0.08,
            .w_density = -0.3,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.2,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.05, .w_avg_path_length = -0.02, .w_diameter = 0.0, .w_community_count = 0.1,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.5     // highest reorder cost
        }},
        // GraphBrewOrder_rabbit: GraphBrew with RabbitOrder clustering
        {"GraphBrewOrder_rabbit", {
            .bias = 0.6,
            .w_modularity = -0.25,
            .w_log_nodes = 0.1,
            .w_log_edges = 0.08,
            .w_density = -0.3,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.2,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.05, .w_avg_path_length = -0.02, .w_diameter = 0.0, .w_community_count = 0.1,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.5
        }},
        // GraphBrewOrder_hubcluster: GraphBrew with HubCluster clustering
        {"GraphBrewOrder_hubcluster", {
            .bias = 0.6,
            .w_modularity = -0.25,
            .w_log_nodes = 0.1,
            .w_log_edges = 0.08,
            .w_density = -0.3,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.2,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.05, .w_avg_path_length = -0.02, .w_diameter = 0.0, .w_community_count = 0.1,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.5
        }},
        // RABBITORDER_boost: Boost-based RabbitOrder
        {"RABBITORDER_boost", {
            .bias = 0.70,
            .w_modularity = -0.30,
            .w_log_nodes = 0.05,
            .w_log_edges = 0.05,
            .w_density = -0.30,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.20,
            .w_hub_concentration = 0.15,
            .w_clustering_coeff = 0.05, .w_avg_path_length = -0.02, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.3
        }},
        // RCM_bnf: CSR-native BNF variant
        {"RCM_bnf", {
            .bias = 0.7,
            .w_modularity = 0.0,
            .w_log_nodes = 0.03,
            .w_log_edges = 0.02,
            .w_density = -0.8,
            .w_avg_degree = -0.03,
            .w_degree_variance = 0.05,
            .w_hub_concentration = 0.0,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.1, .w_diameter = 0.05, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.2
        }},
        // HUBSORTDBG
        {"HUBSORTDBG", {
            .bias = 0.8,
            .w_modularity = 0.0,
            .w_log_nodes = 0.03,
            .w_log_edges = 0.03,
            .w_density = -0.4,
            .w_avg_degree = 0.02,
            .w_degree_variance = 0.2,
            .w_hub_concentration = 0.2,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.1
        }},
        // SORT
        {"SORT", {
            .bias = 0.5,
            .w_modularity = 0.0,
            .w_log_nodes = 0.0,
            .w_log_edges = 0.0,
            .w_density = 0.0,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.05,
            .w_hub_concentration = 0.05,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.05
        }},
        // RANDOM
        {"RANDOM", {
            .bias = 0.3,
            .w_modularity = 0.0,
            .w_log_nodes = 0.0,
            .w_log_edges = 0.0,
            .w_density = 0.0,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.0,
            .w_hub_concentration = 0.0,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.02
        }},
        // GORDER
        {"GORDER", {
            .bias = 0.65,
            .w_modularity = 0.1,
            .w_log_nodes = -0.05,     // slow on large graphs
            .w_log_edges = -0.03,
            .w_density = -0.3,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.15,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.8     // very slow reordering
        }},
        // CORDER
        {"CORDER", {
            .bias = 0.6,
            .w_modularity = 0.05,
            .w_log_nodes = -0.03,
            .w_log_edges = -0.02,
            .w_density = -0.2,
            .w_avg_degree = 0.0,
            .w_degree_variance = 0.1,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.6
        }},
        // GoGraphOrder: M(σ)-maximizing GetOptVal reordering (P3 3.4)
        {"GOGRAPHORDER", {
            .bias = 0.65,
            .w_modularity = 0.1,
            .w_log_nodes = -0.02,
            .w_log_edges = -0.02,
            .w_density = -0.1,
            .w_avg_degree = 0.02,
            .w_degree_variance = 0.1,
            .w_hub_concentration = 0.1,
            .w_clustering_coeff = 0.0, .w_avg_path_length = 0.0, .w_diameter = 0.0, .w_community_count = 0.0,
            .w_forward_edge_fraction = 0.3,   // GoGraph excels at maximizing FEF
            .w_fef_convergence = 0.4,          // strongly helps iterative convergence
            .cache_l1_impact = 0.0, .cache_l2_impact = 0.0, .cache_l3_impact = 0.0, .cache_dram_penalty = 0.0,
            .w_reorder_time = -0.3
        }},
    };
    return weights;
}

/**
 * Threshold for determining "unknown" graph types
 * 
 * When the Euclidean distance to the closest type centroid exceeds this
 * threshold, the graph is considered "unknown" or "distant" from the
 * training distribution. With [0,1]-normalized features in 7D,
 * max possible distance is sqrt(7) ≈ 2.65. A threshold of 1.5 means
 * the graph is ~57% of max distance from any known centroid.
 */
inline constexpr double UNKNOWN_TYPE_DISTANCE_THRESHOLD = 1.5;

// ============================================================================
// PERCEPTRON WEIGHT SELECTION HELPERS
// ============================================================================

/**
 * Check if a graph is distant from known types (i.e., "unknown")
 * 
 * P2 2.2: When per-type radius is available, uses distance/radius > 1.5
 * instead of a global distance threshold.  Falls back to the global
 * threshold when radius is 0 (uncalibrated).
 * 
 * @param type_distance Distance to the closest type centroid
 * @param type_radius   Per-type OOD radius (p95 of training distances), 0 = unknown
 * @return true if the graph is considered "unknown"
 */
inline bool IsDistantGraphType(double type_distance, double type_radius = 0.0) {
    if (type_radius > 0.0) {
        return (type_distance / type_radius) > UNKNOWN_TYPE_DISTANCE_THRESHOLD;
    }
    return type_distance > UNKNOWN_TYPE_DISTANCE_THRESHOLD;
}

/**
 * Select algorithm with fastest reorder time based on w_reorder_time weight.
 * 
 * The w_reorder_time weight encodes how fast each algorithm is at reordering:
 * - Higher (less negative) = faster reordering
 * - Lower (more negative) = slower reordering
 * 
 * This uses the type_X.json weights directly, no .time files needed.
 * 
 * @param weights Map of algorithm -> perceptron weights
 * @param verbose Print selection details
 * @return Algorithm with highest w_reorder_time (fastest reorder)
 */
inline PerceptronSelection SelectFastestReorderFromWeights(
    const std::map<std::string, PerceptronWeights>& weights, bool verbose = false) {
    
    if (weights.empty()) {
        if (verbose) {
            std::cout << "No weights available, defaulting to Random\n";
        }
        return ResolveVariantSelection("RANDOM", 0.0);
    }
    
    // Find algorithm with highest w_reorder_time (least negative = fastest)
    std::string best_name = "RANDOM";
    double best_reorder_weight = -std::numeric_limits<double>::infinity();
    
    for (const auto& [name, w] : weights) {
        if (name == "ORIGINAL") continue;  // Skip ORIGINAL (no reordering)
        
        if (w.w_reorder_time > best_reorder_weight) {
            best_reorder_weight = w.w_reorder_time;
            best_name = name;
        }
    }
    
    if (verbose) {
        std::cout << "Selected fastest-reorder: " << best_name
                  << " (w_reorder_time: " << best_reorder_weight << ")\n";
    }
    
    return ResolveVariantSelection(best_name, best_reorder_weight);
}

// ---------------------------------------------------------------------------
// M2 database hook — LoadPerceptronWeightsFromDB lives in reorder_database.h
// which cannot be included yet (it needs ModelTree, defined later).  We use a
// late-bound function pointer that is set once reorder_database.h is included.
// ---------------------------------------------------------------------------
using LoadPerceptronDBFn = bool(*)(BenchmarkType,
                                   std::map<std::string, PerceptronWeights>&,
                                   bool);
inline LoadPerceptronDBFn& GetLoadPerceptronDBHook() {
    static LoadPerceptronDBFn fn = nullptr;
    return fn;
}

/**
 * @brief P3 3.3: Hierarchical gating — regime-specific weight loading.
 *
 * Two-level model (Rabbit Order insight, Arai et al. IPDPS'16):
 *   Level 1 (gate): GraphType classifies broad regime (social/road/web/etc.)
 *   Level 2 (expert): Regime-specific perceptron weights for finer decisions
 *
 * Tries PERCEPTRON_WEIGHTS_DIR/<regime_name>/weights.json or
 * PERCEPTRON_WEIGHTS_FILE_<REGIME> env var. Falls back to default if
 * no regime-specific data exists.
 *
 * @param graph_type Detected graph type from DetectGraphType()
 * @param bench Benchmark type
 * @param verbose Print loading details
 * @param out_weights Output map of regime-specific weights
 * @return true if regime-specific weights were loaded successfully
 */
inline bool LoadPerceptronWeightsForRegime(
    GraphType graph_type, BenchmarkType bench,
    bool verbose, std::map<std::string, PerceptronWeights>& out_weights) {
    
    if (graph_type == GRAPH_GENERIC) return false;
    
    // Map GraphType → regime name for file lookup
    const char* regime_names[] = {"generic", "social", "road", "web", "powerlaw", "uniform"};
    const char* regime = regime_names[static_cast<int>(graph_type)];
    
    // Try env var override: PERCEPTRON_WEIGHTS_FILE_SOCIAL, etc.
    std::string env_name = std::string("PERCEPTRON_WEIGHTS_FILE_") + regime;
    // Convert to uppercase
    for (auto& c : env_name) c = static_cast<char>(std::toupper(c));
    
    const char* env_path = std::getenv(env_name.c_str());
    if (env_path != nullptr) {
        std::ifstream file(env_path);
        if (file.is_open()) {
            std::string json_content((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            file.close();
            
            auto base = GetPerceptronWeights();
            if (ParseWeightsFromJSON(json_content, out_weights)) {
                // Merge: regime-specific overrides on top of defaults
                for (const auto& kv : out_weights) {
                    base[kv.first] = kv.second;
                }
                out_weights = base;
                if (verbose) {
                    printf("Hierarchical: Loaded regime '%s' weights from %s\n",
                           regime, env_path);
                }
                return true;
            }
        }
    }
    
    // Try PERCEPTRON_WEIGHTS_DIR/<regime>/weights.json
    const char* weights_dir = std::getenv("PERCEPTRON_WEIGHTS_DIR");
    if (weights_dir != nullptr) {
        std::string regime_path = std::string(weights_dir) + "/" + regime + "/weights.json";
        std::ifstream file(regime_path);
        if (file.is_open()) {
            std::string json_content((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            file.close();
            
            auto base = GetPerceptronWeights();
            if (ParseWeightsFromJSON(json_content, out_weights)) {
                for (const auto& kv : out_weights) {
                    base[kv.first] = kv.second;
                }
                out_weights = base;
                if (verbose) {
                    printf("Hierarchical: Loaded regime '%s' weights from %s\n",
                           regime, regime_path.c_str());
                }
                return true;
            }
        }
    }
    
    return false;  // No regime-specific data → caller uses default path
}

/**
 * Load perceptron weights for the given benchmark.
 *
 * M2: Delegates to BenchmarkDatabase which reads from the unified
 * adaptive_models.json.  Falls back to hardcoded defaults if the
 * database has no model data.
 *
 * The PERCEPTRON_WEIGHTS_FILE env-var override is preserved as a
 * development escape hatch.
 *
 * @param modularity Graph modularity score (unused after M2, kept for ABI compat)
 * @param degree_variance (unused)
 * @param hub_concentration (unused)
 * @param avg_degree (unused)
 * @param num_nodes (unused)
 * @param num_edges (unused)
 * @param verbose Print which source was used
 * @return Map of algorithm -> perceptron weights
 */
inline std::map<std::string, PerceptronWeights> LoadPerceptronWeightsForFeatures(
    double modularity, double degree_variance, double hub_concentration,
    double avg_degree, size_t num_nodes, size_t num_edges, bool verbose = false,
    double clustering_coeff = 0.0, BenchmarkType bench = BENCH_GENERIC) {
    
    // Start with hardcoded defaults — ensures ALL algorithms have weights
    auto weights = GetPerceptronWeights();
    
    // Per-benchmark weight resolution: if the loaded JSON contains keys like
    // "HUBSORT__pr", "HUBSORT__bfs", etc., prefer those over the global "HUBSORT"
    // for the active benchmark. This allows per-feature per-benchmark weights
    // instead of global multipliers (Gap #1 from theory audit).
    auto resolveBenchmarkWeights = [&](std::map<std::string, PerceptronWeights>& w,
                                       BenchmarkType b, bool v) {
        if (b == BENCH_GENERIC) return;
        static const char* bench_suffixes[] = {
            "", "pr", "bfs", "cc", "sssp", "bc", "tc", "pr_spmv", "cc_sv"
        };
        const char* suffix = bench_suffixes[static_cast<int>(b)];
        if (!suffix[0]) return;
        
        std::string sep("__");
        size_t resolved = 0;
        // Collect per-bench keys first to avoid iterator invalidation
        std::vector<std::pair<std::string, std::string>> overrides;
        for (const auto& kv : w) {
            if (kv.first.size() > sep.size() + std::strlen(suffix) &&
                kv.first.compare(kv.first.size() - sep.size() - std::strlen(suffix),
                                  std::string::npos, sep + suffix) == 0) {
                std::string base_algo = kv.first.substr(0, kv.first.size() - sep.size() - std::strlen(suffix));
                overrides.emplace_back(base_algo, kv.first);
            }
        }
        for (const auto& [base_algo, bench_key] : overrides) {
            w[base_algo] = w[bench_key];
            resolved++;
        }
        if (v && resolved > 0) {
            printf("Perceptron: Resolved %zu per-benchmark (%s) weight overrides\n",
                   resolved, suffix);
        }
    };
    
    // Check environment variable override first (dev escape hatch)
    const char* env_path = std::getenv("PERCEPTRON_WEIGHTS_FILE");
    if (env_path != nullptr) {
        std::ifstream file(env_path);
        if (file.is_open()) {
            std::string json_content((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            file.close();
            
            std::map<std::string, PerceptronWeights> loaded_weights;
            if (ParseWeightsFromJSON(json_content, loaded_weights)) {
                for (const auto& kv : loaded_weights) {
                    weights[kv.first] = kv.second;
                }
                resolveBenchmarkWeights(weights, bench, verbose);
                if (verbose) {
                    std::cout << "Perceptron: Loaded " << loaded_weights.size()
                              << " weights from env override: " << env_path << "\n";
                }
                return weights;
            }
        }
    }
    
    // M2: Load from BenchmarkDatabase (reads adaptive_models.json)
    auto dbFn = GetLoadPerceptronDBHook();
    if (dbFn) {
        std::map<std::string, PerceptronWeights> db_weights;
        if (dbFn(bench, db_weights, verbose)) {
            for (const auto& kv : db_weights) {
                weights[kv.first] = kv.second;
            }
            resolveBenchmarkWeights(weights, bench, verbose);
            if (verbose) {
                std::cout << "Perceptron: Loaded " << db_weights.size()
                          << " weights from adaptive_models.json\n";
            }
            return weights;
        }
    }
    
    // No model data available — use hardcoded defaults
    if (verbose) {
        std::cout << "Perceptron: Using hardcoded defaults (adaptive_models.json "
                  << "not loaded or empty)\n";
    }
    return weights;
}

// ============================================================================
// PERCEPTRON-BASED ALGORITHM SELECTION
// ============================================================================

/**
 * Minimum margin by which a reordering algorithm's score must exceed ORIGINAL's
 * score for it to be selected. This prevents reordering when the predicted
 * benefit is too small to justify the overhead. (IISWC'18: reorder overhead
 * often exceeds speedup for marginal cases.)
 * 
 * The threshold is benchmark-dependent: convergence-heavy benchmarks (PR, SSSP)
 * may benefit more from reordering than traversal-based ones (BFS).
 */
constexpr double ORIGINAL_MARGIN_THRESHOLD = 0.05;

/**
 * @brief Platt sigmoid: convert score margin to calibrated probability.
 * P(win) = 1 / (1 + exp(-(A * margin + B)))
 * When A == 0 (uncalibrated), returns 0.0 to signal "not available".
 */
inline double PlattProbability(double margin, double A, double B) {
    if (A == 0.0) return 0.0;  // uncalibrated
    double z = A * margin + B;
    // Clamp to avoid overflow
    if (z > 20.0) return 1.0;
    if (z < -20.0) return 0.0;
    return 1.0 / (1.0 + std::exp(-z));
}

/// Default calibrated confidence threshold for Platt scaling.
/// If P(win) < this, fall back to ORIGINAL.
constexpr double PLATT_CONFIDENCE_THRESHOLD = 0.55;

/**
 * Select best reordering algorithm using perceptron scores.
 * 
 * Evaluates all candidate algorithms and returns the one with
 * the highest perceptron score based on community features.
 * Applies a margin-based ORIGINAL fallback: if the best non-ORIGINAL
 * algorithm doesn't exceed ORIGINAL's score by at least
 * ORIGINAL_MARGIN_THRESHOLD, ORIGINAL is returned instead.
 * 
 * @param feat Community features for scoring
 * @param weights Pre-loaded perceptron weights
 * @param bench Benchmark type (default: BENCH_GENERIC for balanced performance)
 * @return Best algorithm based on perceptron scoring
 */
inline PerceptronSelection SelectReorderingFromWeights(
    const CommunityFeatures& feat,
    const std::map<std::string, PerceptronWeights>& weights,
    BenchmarkType bench = BENCH_GENERIC) {
    
    std::string best_name = "ORIGINAL";
    double best_score = -std::numeric_limits<double>::infinity();
    double original_score = -std::numeric_limits<double>::infinity();
    
    // P1 2.3: Overhead-class filtering — skip heavyweight algorithms whose
    // expected reorder time exceeds a fraction of expected kernel time.
    // ADAPTIVE_OVERHEAD_FILTER=1 enables this enhancement.
    // Estimate kernel time from graph size: ~1ms per 100K edges (rough median).
    constexpr double OVERHEAD_RATIO_THRESHOLD = 0.5;  // skip if reorder > 50% of kernel
    double estimated_kernel_time = static_cast<double>(feat.num_edges) * 1e-8;  // rough estimate
    
    for (const auto& kv : weights) {
        // P1 2.3: Skip heavyweight algorithms when cost exceeds budget
        if (AblationConfig::Get().overhead_filter && kv.first != "ORIGINAL" && kv.first != "RANDOM") {
            if (kv.second.avg_reorder_time > 0 && estimated_kernel_time > 0) {
                if (kv.second.avg_reorder_time / estimated_kernel_time > OVERHEAD_RATIO_THRESHOLD) {
                    continue;  // Too expensive for this graph size
                }
            }
        }
        
        double score = kv.second.score(feat, bench);
        if (kv.first == "ORIGINAL") {
            original_score = score;
        }
        if (score > best_score) {
            best_score = score;
            best_name = kv.first;
        }
    }
    
    // Margin-based ORIGINAL fallback (IISWC'18):
    // If the best algorithm doesn't beat ORIGINAL by a sufficient margin,
    // the reordering overhead likely exceeds the benefit.
    // Ablation: ADAPTIVE_NO_MARGIN=1 disables this fallback.
    double margin = (original_score > -1e30) ? best_score - original_score : best_score;
    double platt_conf = 0.0;
    
    if (best_name != "ORIGINAL" && original_score > -1e30 && !AblationConfig::Get().no_margin) {
        // P1 1.4: Platt-calibrated margin threshold.
        // If Platt params are available and ADAPTIVE_PLATT=1, use calibrated
        // P(win) instead of fixed margin threshold.
        bool used_platt = false;
        bool below_threshold = false;   // For P2 2.1 bandit exploration
        if (AblationConfig::Get().platt_scaling) {
            auto it = weights.find(best_name);
            if (it != weights.end() && it->second.platt_A != 0.0) {
                platt_conf = PlattProbability(margin, it->second.platt_A, it->second.platt_B);
                below_threshold = (platt_conf < PLATT_CONFIDENCE_THRESHOLD);
                used_platt = true;
            }
        }
        
        double threshold = ORIGINAL_MARGIN_THRESHOLD;
        if (!used_platt) {
            // Legacy fixed-threshold path
            
            // P0 1.1: Cost-aware dynamic threshold (IISWC'18 cost model).
            // Higher avg_reorder_time → need proportionally larger margin to justify.
            // ADAPTIVE_COST_MODEL=1 enables this enhancement.
            if (AblationConfig::Get().cost_model) {
                auto it = weights.find(best_name);
                if (it != weights.end() && it->second.avg_reorder_time > 0) {
                    constexpr double COST_MODEL_ALPHA = 0.01;  // seconds → score units
                    double cost_threshold = COST_MODEL_ALPHA * it->second.avg_reorder_time;
                    threshold = std::max(threshold, cost_threshold);
                }
            }
            
            below_threshold = (margin < threshold);
        }
        
        if (below_threshold) {
            // P2 2.1: ε-greedy bandit exploration for low-margin cases.
            // When the model is uncertain (margin below threshold), with
            // probability ε, explore by using the model's top pick anyway.
            // ε decays with log(evaluations) to converge toward exploitation.
            // ADAPTIVE_BANDIT=1 enables, ADAPTIVE_BANDIT_EPSILON sets ε₀ (default 0.1).
            if (AblationConfig::Get().bandit_explore) {
                // Thread-local evaluation counter for ε decay
                static thread_local int eval_count = 0;
                ++eval_count;
                
                // ε = ε₀ / (1 + 0.1 × ln(n_evals))
                double eps0 = AblationConfig::Get().bandit_epsilon;
                double eps = eps0 / (1.0 + 0.1 * std::log(static_cast<double>(eval_count)));
                
                // Safety rail: never explore with algos whose avg_reorder_time
                // exceeds estimated kernel time × overhead ratio
                bool safe_to_explore = true;
                {
                    auto it = weights.find(best_name);
                    if (it != weights.end() && it->second.avg_reorder_time > 0) {
                        double est_kernel = static_cast<double>(feat.num_edges) * 1e-8;
                        constexpr double EXPLORE_OVERHEAD_RATIO = 0.5;
                        if (it->second.avg_reorder_time > est_kernel * EXPLORE_OVERHEAD_RATIO) {
                            safe_to_explore = false;
                        }
                    }
                }
                
                if (safe_to_explore) {
                    // Thread-local RNG for ε-greedy coin flip
                    static thread_local std::mt19937 rng(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()));
                    std::uniform_real_distribution<double> dist(0.0, 1.0);
                    
                    if (dist(rng) < eps) {
                        // Explore: use model's pick despite low margin
                        auto result = ResolveVariantSelection(best_name, best_score);
                        result.margin = margin;
                        result.confidence = platt_conf;
                        result.explored = true;
                        return result;
                    }
                }
            }
            // Exploit: fall back to ORIGINAL
            return ResolveVariantSelection("ORIGINAL", original_score);
        }
    }
    
    // Populate margin and confidence on the result
    auto result = ResolveVariantSelection(best_name, best_score);
    result.margin = margin;
    result.confidence = platt_conf;
    return result;
}

/**
 * @brief Select top-K reordering algorithms ranked by perceptron score (Mode 3)
 *
 * Returns up to K candidate algorithms sorted by descending perceptron score.
 * Excludes ORIGINAL and RANDOM from candidates.  Used for Top-K pilot-based
 * selection: try K orderings and pick the fastest.
 *
 * @param feat Community features for scoring
 * @param weights Pre-loaded perceptron weights
 * @param bench Benchmark type
 * @param k Number of top candidates to return
 * @return Vector of PerceptronSelection sorted by descending score (up to k)
 */
inline std::vector<PerceptronSelection> SelectTopKFromWeights(
    const CommunityFeatures& feat,
    const std::map<std::string, PerceptronWeights>& weights,
    BenchmarkType bench = BENCH_GENERIC,
    int k = 3) {
    
    // Score all algorithms
    std::vector<std::pair<double, std::string>> scored;
    scored.reserve(weights.size());

    // P1 2.3: Overhead-class filtering — same logic as SelectReorderingFromWeights
    constexpr double TOPK_OVERHEAD_RATIO = 0.5;
    double est_kernel = static_cast<double>(feat.num_edges) * 1e-8;

    for (const auto& kv : weights) {
        // Skip baselines — they add no value to Top-K reordering
        if (kv.first == "ORIGINAL" || kv.first == "RANDOM") continue;
        // P1 2.3: Skip heavyweight algorithms when overhead filter is active
        if (AblationConfig::Get().overhead_filter) {
            if (kv.second.avg_reorder_time > 0 && est_kernel > 0) {
                if (kv.second.avg_reorder_time / est_kernel > TOPK_OVERHEAD_RATIO) {
                    continue;
                }
            }
        }
        double s = kv.second.score(feat, bench);
        scored.push_back({s, kv.first});
    }
    
    // Sort descending by score
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Return top-k
    std::vector<PerceptronSelection> result;
    int n = std::min(k, static_cast<int>(scored.size()));
    result.reserve(n);
    for (int i = 0; i < n; i++) {
        result.push_back(ResolveVariantSelection(scored[i].second, scored[i].first));
    }
    return result;
}

// ============================================================================
// RUNTIME MODEL TREE INTERPRETER (Decision Tree & Hybrid DT+Perceptron)
// ============================================================================
//
// Instead of embedding thousands of lines of auto-generated C++ if/else trees,
// this interpreter loads trained models from JSON files at runtime:
//
//   results/models/decision_tree/{bench}.json    — Decision Tree models
//   results/models/hybrid/{bench}.json           — Hybrid DT+Perceptron models
//
// Benefits:
//   - Retrain on new graphs by running Python → new JSON files → zero recompile
//   - Single generic interpreter handles all benchmarks and model types
//   - Models are human-readable JSON, easy to inspect and version-control
//
// JSON format (flat node array — easy to parse without recursive descent):
//   {
//     "model_type": "decision_tree" | "hybrid",
//     "benchmark": "pr",
//     "families": ["GORDER", "HUBSORT", "LEIDEN", ...],
//     "features": ["modularity", "hub_concentration", ...],
//     "n_nodes": 7,
//     "nodes": [
//       {"feature_idx": 0, "threshold": 0.617, "left": 1, "right": 4, "samples": 44},
//       {"leaf_class": "ORIGINAL", "samples": 5, "weights": {"GORDER": [...], ...}}
//     ]
//   }
//
// Training: C++ trains DT/hybrid models at runtime from benchmarks.json
//           via reorder_database.h — no Python training step needed.
// ============================================================================

/// Directory for model tree JSON files (relative to working directory)
inline constexpr const char* MODEL_TREE_DIR = "results/models/";

/// Number of features used by the model trees (matches Python DT_FEATURE_NAMES)
inline constexpr int MODEL_TREE_N_FEATURES = 24;

/**
 * @brief A single node in a serialized model tree.
 *
 * Split nodes have feature_idx >= 0 and valid left/right children.
 * Leaf nodes have feature_idx == -1 and a leaf_class prediction.
 * Hybrid leaves additionally carry per-family perceptron weight vectors.
 */
struct ModelTreeNode {
    int feature_idx = -1;       ///< Feature index for split (-1 = leaf)
    double threshold = 0.0;     ///< Split threshold (feat <= threshold → left)
    int left = -1;              ///< Left child node index
    int right = -1;             ///< Right child node index
    std::string leaf_class;     ///< Predicted class for leaf nodes
    int samples = 0;            ///< Training samples at this node

    /// Per-family perceptron weights for hybrid leaves.
    /// Key: family name, Value: weight vector [w0, w1, ..., w11, bias]
    std::map<std::string, std::vector<double>> leaf_weights;

    bool is_leaf() const { return feature_idx < 0; }
};

/**
 * @brief A complete model tree (DT or hybrid) for one benchmark.
 */
struct ModelTree {
    std::string model_type;                 ///< "decision_tree" or "hybrid"
    std::string benchmark;                  ///< e.g. "pr", "bfs"
    std::vector<std::string> families;      ///< Algorithm family names
    std::vector<ModelTreeNode> nodes;        ///< Flat node array
    bool loaded = false;                    ///< True if successfully loaded

    /// Extract feature array from CommunityFeatures (same order as Python DT_FEATURE_NAMES)
    static void extract_features(const CommunityFeatures& feat, double out[MODEL_TREE_N_FEATURES]) {
        double log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        double log_wsr   = std::log2(feat.working_set_ratio + 1.0);
        // 14 linear features
        out[0]  = feat.modularity;
        out[1]  = feat.hub_concentration;
        out[2]  = log_nodes;
        out[3]  = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        out[4]  = feat.internal_density;
        out[5]  = feat.avg_degree / 100.0;
        out[6]  = feat.clustering_coeff;
        out[7]  = feat.packing_factor;
        out[8]  = feat.forward_edge_fraction;
        out[9]  = log_wsr;
        out[10] = std::log10(feat.community_count + 1.0);
        out[11] = feat.diameter_estimate / 50.0;
        out[12] = feat.degree_variance;
        out[13] = feat.avg_path_length / 10.0;
        // DON-RL features
        out[14] = feat.vertex_significance_skewness;
        out[15] = feat.window_neighbor_overlap;
        // Quadratic cross-terms
        out[16] = feat.degree_variance * feat.hub_concentration;
        out[17] = feat.modularity * log_nodes;
        out[18] = feat.packing_factor * log_wsr;
        out[19] = feat.vertex_significance_skewness * feat.hub_concentration;
        out[20] = feat.window_neighbor_overlap * feat.packing_factor;
        // IISWC'18 cache-line packing factor
        out[21] = feat.packing_factor_cl;
        // Per-level WSR (P-OPT cache hierarchy)
        out[22] = std::log2(feat.wsr_l1 + 1.0);
        out[23] = std::log2(feat.wsr_l2 + 1.0);
    }

    /**
     * @brief Predict the best algorithm family for given features.
     *
     * Walks the tree from root (node 0) to a leaf. For pure DT, returns
     * the leaf_class. For hybrid, scores all families via dot product and
     * returns the highest-scoring family.
     */
    std::string predict(const CommunityFeatures& feat) const {
        if (nodes.empty()) return "ORIGINAL";

        double features[MODEL_TREE_N_FEATURES];
        extract_features(feat, features);

        // Walk tree from root (with iteration limit to prevent infinite loops
        // on malformed trees with cycles)
        int idx = 0;
        const int max_iters = static_cast<int>(nodes.size()) * 2;
        for (int iter = 0; iter < max_iters && idx >= 0 &&
             idx < static_cast<int>(nodes.size()); ++iter) {
            const auto& node = nodes[idx];
            if (node.is_leaf()) {
                // Hybrid: score families via perceptron dot products
                if (!node.leaf_weights.empty()) {
                    double best_score = -1e30;
                    std::string best_family = node.leaf_class;
                    for (const auto& [fam, weights] : node.leaf_weights) {
                        double score = 0.0;
                        int n_w = static_cast<int>(weights.size());
                        // weights = [w0, w1, ..., w_{n-2}, bias]
                        for (int i = 0; i < n_w - 1 && i < MODEL_TREE_N_FEATURES; ++i) {
                            score += weights[i] * features[i];
                        }
                        if (n_w > 0) score += weights[n_w - 1]; // bias
                        if (score > best_score) {
                            best_score = score;
                            best_family = fam;
                        }
                    }
                    return best_family;
                }
                // Pure DT: return class directly
                return node.leaf_class;
            }
            // Split node: go left if feature <= threshold, else right
            if (node.feature_idx >= MODEL_TREE_N_FEATURES) {
                // Invalid feature index — go right as fallback but warn
                idx = node.right;
            } else if (features[node.feature_idx] <= node.threshold) {
                idx = node.left;
            } else {
                idx = node.right;
            }
        }
        return "ORIGINAL"; // Safety fallback (exhausted iterations or OOB index)
    }
};

/**
 * @brief Parse a model tree from JSON content.
 *
 * Hand-rolled parser matching the project's existing JSON parsing style
 * (no external dependencies). Parses the flat node array format from
 * adaptive_models.json (C++ runtime training or legacy export).
 *
 * @param json_content  Raw JSON string
 * @param tree          Output ModelTree to populate
 * @return true if parsing succeeded
 */
inline bool ParseModelTreeFromJSON(const std::string& json_content, ModelTree& tree) {
    // --- Helper lambdas ---
    auto find_string_value = [](const std::string& s, const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\"";
        size_t pos = s.find(needle);
        if (pos == std::string::npos) return "";
        pos = s.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = s.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        size_t end = s.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return s.substr(pos + 1, end - pos - 1);
    };

    auto find_int_value = [](const std::string& s, const std::string& key) -> int {
        std::string needle = "\"" + key + "\"";
        size_t pos = s.find(needle);
        if (pos == std::string::npos) return -1;
        pos = s.find(':', pos);
        if (pos == std::string::npos) return -1;
        pos++;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) pos++;
        try { return std::stoi(s.substr(pos, 20)); } catch (...) { return -1; }
    };

    auto find_double_value = [](const std::string& s, const std::string& key) -> double {
        std::string needle = "\"" + key + "\"";
        size_t pos = s.find(needle);
        if (pos == std::string::npos) return 0.0;
        pos = s.find(':', pos);
        if (pos == std::string::npos) return 0.0;
        pos++;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) pos++;
        size_t end = pos;
        while (end < s.size() && (std::isdigit(s[end]) || s[end] == '.' ||
               s[end] == '-' || s[end] == 'e' || s[end] == 'E' || s[end] == '+')) end++;
        try { return std::stod(s.substr(pos, end - pos)); } catch (...) { return 0.0; }
    };

    auto find_matching_bracket = [](const std::string& s, size_t open, char open_c, char close_c) -> size_t {
        int depth = 1;
        for (size_t i = open + 1; i < s.size(); i++) {
            if (s[i] == open_c) depth++;
            else if (s[i] == close_c) { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    };

    // --- Parse top-level fields ---
    tree.model_type = find_string_value(json_content, "model_type");
    tree.benchmark  = find_string_value(json_content, "benchmark");

    // Parse families array
    {
        size_t pos = json_content.find("\"families\"");
        if (pos != std::string::npos) {
            size_t arr_start = json_content.find('[', pos);
            if (arr_start != std::string::npos) {
                size_t arr_end = find_matching_bracket(json_content, arr_start, '[', ']');
                if (arr_end != std::string::npos) {
                    std::string arr = json_content.substr(arr_start, arr_end - arr_start + 1);
                    size_t p = 0;
                    while ((p = arr.find('"', p)) != std::string::npos) {
                        size_t e = arr.find('"', p + 1);
                        if (e == std::string::npos) break;
                        tree.families.push_back(arr.substr(p + 1, e - p - 1));
                        p = e + 1;
                    }
                }
            }
        }
    }

    // --- Parse nodes array ---
    size_t nodes_pos = json_content.find("\"nodes\"");
    if (nodes_pos == std::string::npos) return false;
    size_t nodes_arr_start = json_content.find('[', nodes_pos);
    if (nodes_arr_start == std::string::npos) return false;
    size_t nodes_arr_end = find_matching_bracket(json_content, nodes_arr_start, '[', ']');
    if (nodes_arr_end == std::string::npos) return false;

    // Walk through nodes array, finding each { ... } object
    size_t pos = nodes_arr_start + 1;
    while (pos < nodes_arr_end) {
        size_t obj_start = json_content.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= nodes_arr_end) break;
        size_t obj_end = find_matching_bracket(json_content, obj_start, '{', '}');
        if (obj_end == std::string::npos) break;

        std::string obj = json_content.substr(obj_start, obj_end - obj_start + 1);
        ModelTreeNode node;

        // Check if this is a leaf (has "leaf_class") or split (has "feature_idx")
        if (obj.find("\"leaf_class\"") != std::string::npos) {
            // Leaf node
            node.feature_idx = -1;
            node.leaf_class = find_string_value(obj, "leaf_class");
            node.samples = find_int_value(obj, "samples");

            // Parse hybrid weights if present
            size_t wt_pos = obj.find("\"weights\"");
            if (wt_pos != std::string::npos) {
                size_t wt_obj_start = obj.find('{', wt_pos);
                if (wt_obj_start != std::string::npos) {
                    size_t wt_obj_end = find_matching_bracket(obj, wt_obj_start, '{', '}');
                    if (wt_obj_end != std::string::npos) {
                        std::string wt_block = obj.substr(wt_obj_start, wt_obj_end - wt_obj_start + 1);

                        // Find each family's weight array: "FAMILY": [w0, w1, ...]
                        size_t fp = 0;
                        while ((fp = wt_block.find('"', fp)) != std::string::npos) {
                            size_t fe = wt_block.find('"', fp + 1);
                            if (fe == std::string::npos) break;
                            std::string fam_name = wt_block.substr(fp + 1, fe - fp - 1);
                            fp = fe + 1;

                            // Skip non-family keys (like stray fields)
                            size_t colon = wt_block.find(':', fp);
                            if (colon == std::string::npos) break;
                            size_t arr_s = wt_block.find('[', colon);
                            if (arr_s == std::string::npos) break;
                            size_t arr_e = find_matching_bracket(wt_block, arr_s, '[', ']');
                            if (arr_e == std::string::npos) break;

                            // Parse doubles from array
                            std::string arr_str = wt_block.substr(arr_s + 1, arr_e - arr_s - 1);
                            std::vector<double> wvec;
                            size_t dp = 0;
                            while (dp < arr_str.size()) {
                                while (dp < arr_str.size() &&
                                       (arr_str[dp] == ' ' || arr_str[dp] == ',' ||
                                        arr_str[dp] == '\n' || arr_str[dp] == '\t')) dp++;
                                if (dp >= arr_str.size()) break;
                                size_t de = dp;
                                while (de < arr_str.size() &&
                                       arr_str[de] != ',' && arr_str[de] != ' ' &&
                                       arr_str[de] != ']' && arr_str[de] != '\n') de++;
                                try {
                                    wvec.push_back(std::stod(arr_str.substr(dp, de - dp)));
                                } catch (...) {}
                                dp = de;
                            }
                            if (!wvec.empty()) {
                                node.leaf_weights[fam_name] = std::move(wvec);
                            }
                            fp = arr_e + 1;
                        }
                    }
                }
            }
        } else {
            // Split node
            node.feature_idx = find_int_value(obj, "feature_idx");
            node.threshold = find_double_value(obj, "threshold");
            node.left = find_int_value(obj, "left");
            node.right = find_int_value(obj, "right");
            node.samples = find_int_value(obj, "samples");
        }

        tree.nodes.push_back(std::move(node));
        pos = obj_end + 1;
    }

    tree.loaded = !tree.nodes.empty();
    return tree.loaded;
}

/**
 * @brief Load a model tree from a JSON file, with caching.
 *
 * Looks for the model at: {MODEL_TREE_DIR}/{subdir}/{bench}.json
 * where subdir is "decision_tree" or "hybrid".
 *
 * Also checks env var MODEL_TREE_PATH for an override directory.
 *
 * @param bench    Benchmark type
 * @param subdir   "decision_tree" or "hybrid"
 * @param verbose  Print loading messages
 * @return Reference to the (possibly cached) ModelTree
 */
inline const ModelTree& LoadModelTree(BenchmarkType bench, const std::string& subdir,
                                       bool verbose = false) {
    // Cache: one ModelTree per (bench, subdir) combination (thread-safe)
    static std::mutex cache_mtx;
    static std::map<std::pair<int,std::string>, ModelTree> cache;
    auto key = std::make_pair(static_cast<int>(bench), subdir);

    {
        std::lock_guard<std::mutex> lk(cache_mtx);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    // Benchmark name mapping (pr_spmv→pr, cc_sv→cc)
    const char* bench_names[] = {"generic","pr","bfs","cc","sssp","bc","tc","pr","cc"};
    int bench_idx = static_cast<int>(bench);
    if (bench_idx < 0 || bench_idx > 8) bench_idx = 0;
    std::string bench_name = bench_names[bench_idx];

    // Determine base directory
    std::string base_dir = MODEL_TREE_DIR;
    const char* env_path = std::getenv("MODEL_TREE_PATH");
    if (env_path != nullptr) base_dir = std::string(env_path) + "/";

    std::string filepath = base_dir + subdir + "/" + bench_name + ".json";

    // Load the model (outside lock, I/O is slow)
    ModelTree loaded;
    std::ifstream file(filepath);
    if (file.is_open()) {
        std::string json_content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        file.close();

        if (ParseModelTreeFromJSON(json_content, loaded)) {
            if (verbose) {
                std::cout << "ModelTree: Loaded " << subdir << "/" << bench_name
                          << " (" << loaded.nodes.size() << " nodes)\n";
            }
        } else {
            if (verbose) {
                std::cout << "ModelTree: Failed to parse " << filepath << "\n";
            }
        }
    } else {
        if (verbose) {
            std::cout << "ModelTree: File not found: " << filepath << "\n";
        }
    }

    // Insert into cache under lock
    std::lock_guard<std::mutex> lk(cache_mtx);
    auto& cached = cache[key];
    cached = std::move(loaded);
    return cached;
}

/**
 * @brief Select algorithm using a runtime-loaded model tree.
 *
 * Tries to load the appropriate JSON model. Returns empty string if model
 * is not available (caller should fall back).
 *
 * @param feat    Graph community features
 * @param bench   Benchmark type
 * @param subdir  "decision_tree" or "hybrid"
 * @return Family name, or empty string if model unavailable
 */
inline std::string SelectAlgorithmModelTree(const CommunityFeatures& feat,
                                             BenchmarkType bench,
                                             const std::string& subdir,
                                             bool verbose = false) {
    const ModelTree& mt = LoadModelTree(bench, subdir, verbose);
    if (!mt.loaded) return "";  // Model not available
    return mt.predict(feat);
}


/**
 * Select best reordering algorithm using feature-based type matching.
 * 
 * This version first tries to find a matching auto-generated type (type_0, etc.)
 * from the type registry based on graph features, then falls back to semantic types.
 * 
 * @param feat Community features for scoring
 * @param global_modularity Global graph modularity
 * @param global_degree_variance Global degree variance
 * @param global_hub_concentration Global hub concentration
 * @param num_nodes Total number of nodes
 * @param num_edges Total number of edges
 * @param bench Benchmark type
 * @return Best algorithm based on perceptron scoring with feature-matched weights
 */
inline PerceptronSelection SelectReorderingPerceptronWithFeatures(
    const CommunityFeatures& feat,
    double global_modularity, double global_degree_variance,
    double global_hub_concentration, size_t num_nodes, size_t num_edges,
    BenchmarkType bench = BENCH_GENERIC) {
    
    // Load weights based on features (tries {matched_type}/{bench}.json first,
    // then type_0/{bench}.json, then {matched_type}/weights.json, then defaults)
    auto weights = LoadPerceptronWeightsForFeatures(
        global_modularity, global_degree_variance, global_hub_concentration,
        feat.avg_degree, num_nodes, num_edges, false, feat.clustering_coeff, bench);
    
    return SelectReorderingFromWeights(feat, weights, bench);
}

// ============================================================================
// DATABASE-DRIVEN SELECTION (MODE_DATABASE)
// Include after all types and helpers are defined.
// ============================================================================
#include "reorder_database.h"
// Register the M2 database loader now that reorder_database.h is available.
inline bool _InitLoadPerceptronDBHook() {
    GetLoadPerceptronDBHook() = &graphbrew::database::LoadPerceptronWeightsFromDB;
    return true;
}
static const bool _loadPerceptronDBHookInit = _InitLoadPerceptronDBHook();


/**
 * Select best reordering algorithm with MODEL × CRITERION selection.
 *
 * This is the main entry point for AdaptiveOrder algorithm selection.
 * Model and Criterion are orthogonal axes:
 *
 * **Models** (how to predict):
 * - PERCEPTRON: Multi-class perceptron with SSO-trained weights
 * - DECISION_TREE: Trained DT classifier (12D features, per-benchmark)
 * - HYBRID: DT structure with per-leaf perceptron weights
 * - KNN_DATABASE: Distance-weighted k-NN from benchmark database
 *
 * **Criteria** (what to optimize):
 * - FASTEST_REORDER: Minimize reordering time only
 * - FASTEST_EXECUTION: Minimize kernel execution time
 * - BEST_ENDTOEND: Minimize (reorder + execution) time
 * - BEST_AMORTIZATION: Minimize iterations needed to amortize reorder cost
 *
 * @param feat Community features for scoring
 * @param global_modularity Global graph modularity
 * @param global_degree_variance Global degree variance
 * @param global_hub_concentration Global hub concentration
 * @param num_nodes Total number of nodes
 * @param num_edges Total number of edges
 * @param model Which prediction model to use
 * @param criterion What metric to optimize
 * @param graph_name Name of the graph (for oracle lookup)
 * @param bench Benchmark type
 * @param verbose Print selection details
 * @return Best algorithm based on model and criterion
 */
inline PerceptronSelection SelectReorderingWithModelCriterion(
    const CommunityFeatures& feat,
    double global_modularity, double global_degree_variance,
    double global_hub_concentration, size_t num_nodes, size_t num_edges,
    SelectionModel model, SelectionCriterion criterion,
    const std::string& graph_name = "",
    BenchmarkType bench = BENCH_GENERIC, bool verbose = false) {

    if (verbose) {
        std::cout << "Selection: model=" << SelectionModelToString(model)
                  << " criterion=" << SelectionCriterionToString(criterion) << "\n";
    }

    // ================================================================
    // Model dispatch: KNN_DATABASE uses streaming DB directly
    // ================================================================
    if (model == SELECTION_MODEL_KNN_DATABASE) {
        // Map criterion to legacy mode for the DB's select_for_mode()
        SelectionMode legacy_mode;
        switch (criterion) {
            case CRITERION_FASTEST_REORDER:    legacy_mode = MODE_FASTEST_REORDER; break;
            case CRITERION_FASTEST_EXECUTION:  legacy_mode = MODE_DATABASE; break;
            case CRITERION_BEST_ENDTOEND:      legacy_mode = MODE_BEST_ENDTOEND; break;
            case CRITERION_BEST_AMORTIZATION:  legacy_mode = MODE_BEST_AMORTIZATION; break;
            default:                           legacy_mode = MODE_DATABASE; break;
        }

        std::string family = graphbrew::database::SelectForMode(
            feat, bench, graph_name, legacy_mode, verbose);
        if (!family.empty()) {
            if (verbose) {
                std::cout << "kNN-database → " << family << "\n";
            }
            return ResolveVariantSelection(family, 0.0);
        }
        // If DB is empty, fall through to perceptron fallback
        if (verbose) {
            std::cout << "kNN-database: no data, falling back to perceptron\n";
        }
    }

    // ================================================================
    // Model dispatch: DECISION_TREE or HYBRID
    // ================================================================
    if (model == SELECTION_MODEL_DECISION_TREE || model == SELECTION_MODEL_HYBRID) {
        std::string subdir = (model == SELECTION_MODEL_DECISION_TREE) ? "decision_tree" : "hybrid";
        std::string family = graphbrew::database::SelectAlgorithmModelTreeFromDB(
            feat, bench, subdir, verbose);
        if (!family.empty()) {
            if (verbose) {
                std::cout << SelectionModelToString(model) << " → " << family << "\n";
            }
            return ResolveVariantSelection(family, 0.0);
        }
        // No model tree available — fall through to perceptron
        if (verbose) {
            std::cout << SelectionModelToString(model) << ": no model, falling back to perceptron\n";
        }
    }

    // ================================================================
    // Model: PERCEPTRON (or fallback from other models)
    // Criterion dispatch determines how to score algorithms
    // ================================================================
    switch (criterion) {
        case CRITERION_FASTEST_REORDER: {
            auto weights = LoadPerceptronWeightsForFeatures(
                global_modularity, global_degree_variance, global_hub_concentration,
                feat.avg_degree, num_nodes, num_edges, verbose, feat.clustering_coeff, bench);

            PerceptronSelection fastest = SelectFastestReorderFromWeights(weights, verbose);
            if (verbose) {
                std::cout << "Perceptron (fastest-reorder) → " << fastest.variant_name << "\n";
            }
            return fastest;
        }

        case CRITERION_FASTEST_EXECUTION: {
            PerceptronSelection best = SelectReorderingPerceptronWithFeatures(
                feat, global_modularity, global_degree_variance,
                global_hub_concentration, num_nodes, num_edges, bench);
            if (verbose) {
                std::cout << "Perceptron (fastest-execution) → " << best.variant_name << "\n";
            }
            return best;
        }

        case CRITERION_BEST_ENDTOEND: {
            auto weights = LoadPerceptronWeightsForFeatures(
                global_modularity, global_degree_variance, global_hub_concentration,
                feat.avg_degree, num_nodes, num_edges, verbose, feat.clustering_coeff, bench);

            std::string best_name = "ORIGINAL";
            double best_score = -std::numeric_limits<double>::infinity();
            const double REORDER_WEIGHT_BOOST = 2.0;

            for (const auto& [name, w] : weights) {
                double exec_score = w.score(feat, bench);
                double reorder_bonus = w.w_reorder_time * REORDER_WEIGHT_BOOST;
                double total_score = exec_score + reorder_bonus;

                if (total_score > best_score) {
                    best_score = total_score;
                    best_name = name;
                }
            }

            if (verbose) {
                std::cout << "Perceptron (best-endtoend) → " << best_name << "\n";
            }
            return ResolveVariantSelection(best_name, best_score);
        }

        case CRITERION_BEST_AMORTIZATION: {
            auto weights = LoadPerceptronWeightsForFeatures(
                global_modularity, global_degree_variance, global_hub_concentration,
                feat.avg_degree, num_nodes, num_edges, verbose, feat.clustering_coeff, bench);

            std::string best_name = "ORIGINAL";
            double best_iters = std::numeric_limits<double>::infinity();

            for (const auto& [name, w] : weights) {
                if (name == "ORIGINAL") continue;
                double iters = w.iterationsToAmortize();
                if (verbose) {
                    std::cout << "  " << name
                              << ": speedup=" << w.avg_speedup
                              << ", reorder=" << w.avg_reorder_time << "s"
                              << ", iters_to_amortize=" << iters << "\n";
                }
                if (iters < best_iters) {
                    best_iters = iters;
                    best_name = name;
                }
            }

            if (verbose) {
                std::cout << "Perceptron (best-amortization) → " << best_name
                          << " (amortizes in " << best_iters << " iterations)\n";
            }
            return ResolveVariantSelection(best_name, 0.0);
        }

        default:
            return SelectReorderingPerceptronWithFeatures(
                feat, global_modularity, global_degree_variance,
                global_hub_concentration, num_nodes, num_edges, bench);
    }
}

/**
 * Select best reordering algorithm with MODE-AWARE selection (LEGACY).
 *
 * Backward-compatible wrapper: decomposes the single SelectionMode into
 * (SelectionModel, SelectionCriterion) and delegates to
 * SelectReorderingWithModelCriterion().
 */
inline PerceptronSelection SelectReorderingWithMode(
    const CommunityFeatures& feat,
    double global_modularity, double global_degree_variance,
    double global_hub_concentration, size_t num_nodes, size_t num_edges,
    SelectionMode mode, const std::string& graph_name = "",
    BenchmarkType bench = BENCH_GENERIC, bool verbose = false) {

    auto [model, criterion] = DecomposeSelectionMode(mode);
    return SelectReorderingWithModelCriterion(
        feat, global_modularity, global_degree_variance,
        global_hub_concentration, num_nodes, num_edges,
        model, criterion, graph_name, bench, verbose);
}

// ============================================================================
// COMMUNITY-BASED ALGORITHM SELECTION
// ============================================================================

/**
 * Compute dynamic minimum community size threshold.
 * 
 * Communities smaller than this threshold will use ORIGINAL ordering
 * (reordering overhead exceeds benefit for small subgraphs).
 * 
 * Formula: max(ABSOLUTE_MIN, min(avg_size / FACTOR, sqrt(N)))
 * - ABSOLUTE_MIN = 50 (never go below this)
 * - FACTOR = 4 (communities < 1/4 of average are "small")
 * - sqrt(N) = classic graph algorithm heuristic
 * 
 * @param num_nodes Total nodes in graph
 * @param num_communities Number of communities detected
 * @param avg_community_size Average community size (num_nodes / num_communities)
 * @return Dynamic threshold for minimum community size
 */
inline size_t ComputeDynamicMinCommunitySize(size_t num_nodes, 
                                              size_t num_communities,
                                              size_t avg_community_size = 0)
{
    const size_t ABSOLUTE_MIN = 50;      // Never go below this
    const size_t FACTOR = 4;             // Communities < avg/4 are "small"
    const size_t MAX_THRESHOLD = 2000;   // Cap for very large graphs
    
    // Compute average if not provided
    if (avg_community_size == 0 && num_communities > 0) {
        avg_community_size = num_nodes / num_communities;
    }
    
    // Threshold based on average community size
    size_t avg_based = (avg_community_size > 0) ? avg_community_size / FACTOR : ABSOLUTE_MIN;
    
    // Threshold based on sqrt(N) - classic heuristic
    size_t sqrt_based = static_cast<size_t>(std::sqrt(static_cast<double>(num_nodes)));
    
    // Take minimum of avg-based and sqrt-based (don't want threshold too high)
    // But ensure at least ABSOLUTE_MIN
    size_t threshold = std::max(ABSOLUTE_MIN, std::min(avg_based, sqrt_based));
    
    // Cap at MAX_THRESHOLD for very large graphs
    return std::min(threshold, MAX_THRESHOLD);
}

/**
 * @brief Compute dynamic threshold for local reordering
 * 
 * Local reordering (e.g., per-community) has overhead for subgraph construction. We
 * set a threshold higher than the minimum community size to ensure worthwhile work.
 * 
 * @param num_nodes Total nodes in graph
 * @param num_communities Number of communities
 * @param avg_community_size Average community size (optional)
 * @return Threshold for local reordering (higher = more batching)
 */
inline size_t ComputeDynamicLocalReorderThreshold(size_t num_nodes,
                                                   size_t num_communities,
                                                   size_t avg_community_size = 0)
{
    // Local reorder threshold is 2x the min community size due to subgraph overhead
    size_t min_size = ComputeDynamicMinCommunitySize(num_nodes, num_communities, avg_community_size);
    return std::min(min_size * 2, static_cast<size_t>(5000));  // Cap at 5000
}

/**
 * Select best reordering algorithm for a community/subgraph.
 * 
 * This is the main entry point for community-aware algorithm selection.
 * It handles small community optimization and delegates to the perceptron-based
 * selection system for larger communities.
 * 
 * @param feat Community features for scoring
 * @param global_modularity Global graph modularity
 * @param global_degree_variance Global degree variance
 * @param global_hub_concentration Global hub concentration
 * @param global_avg_degree Global average degree (currently unused)
 * @param num_nodes Total graph nodes
 * @param num_edges Total graph edges
 * @param bench Benchmark type for optimization
 * @param graph_type Detected graph type
 * @param mode Selection mode (default: MODE_FASTEST_EXECUTION)
 * @param graph_name Graph name for loading reorder times
 * @param dynamic_min_size Minimum size threshold (0 = auto-compute)
 * @return Best algorithm for this community
 */
inline PerceptronSelection SelectBestReorderingForCommunity(
    CommunityFeatures feat, 
    double global_modularity,
    double global_degree_variance,
    double global_hub_concentration,
    double global_avg_degree,
    size_t num_nodes, size_t num_edges,
    BenchmarkType bench = BENCH_GENERIC,
    GraphType graph_type = GRAPH_GENERIC,
    SelectionMode mode = MODE_FASTEST_EXECUTION,
    const std::string& graph_name = "",
    size_t dynamic_min_size = 0)
{
    (void)global_avg_degree;  // Reserved for future use
    
    // Ablation: ADAPTIVE_FORCE_ALGO=N bypasses all perceptron logic
    if (AblationConfig::Get().force_algo >= 0) {
        ReorderingAlgo forced = static_cast<ReorderingAlgo>(AblationConfig::Get().force_algo);
        return PerceptronSelection{forced, ReorderingAlgoStr(forced), {}, 0.0};
    }
    
    // P3 3.3: Hierarchical gating — use regime-specific perceptron weights
    // when graph type is non-generic and hierarchical mode is enabled.
    if (AblationConfig::Get().hierarchical_gate && graph_type != GRAPH_GENERIC) {
        std::map<std::string, PerceptronWeights> regime_weights;
        if (LoadPerceptronWeightsForRegime(graph_type, bench, false, regime_weights)) {
            feat.modularity = global_modularity;
            auto sel = SelectReorderingFromWeights(feat, regime_weights, bench);
            // Tag as hierarchical selection for diagnostics
            return sel;
        }
        // No regime-specific data → fall through to default path
    }
    
    // P0 1.2 (IISWC'18): Packing factor short-circuit.
    // High packing factor = hub neighbours already co-located in memory.
    // Low working_set_ratio = graph fits well in cache hierarchy.
    // Together: reordering has diminishing returns → skip it.
    // ADAPTIVE_PACKING_SKIP=1 enables this short-circuit.
    constexpr double PACKING_SKIP_THRESHOLD = 0.7;
    constexpr double WSR_SKIP_THRESHOLD = 2.0;
    if (AblationConfig::Get().packing_skip &&
        feat.packing_factor > PACKING_SKIP_THRESHOLD &&
        feat.working_set_ratio < WSR_SKIP_THRESHOLD) {
        return ResolveVariantSelection("ORIGINAL", 0.0);
    }
    
    // Small communities: reordering overhead exceeds benefit
    const size_t MIN_COMMUNITY_SIZE = (dynamic_min_size > 0) ? dynamic_min_size : 
        ComputeDynamicMinCommunitySize(num_nodes, 1, feat.num_nodes);
    
    if (feat.num_nodes < MIN_COMMUNITY_SIZE) {
        return ResolveVariantSelection("ORIGINAL", 0.0);
    }
    
    // Set the modularity in features for perceptron scoring
    feat.modularity = global_modularity;
    
    // Use MODE-AWARE selection (handles unknown graph fallback automatically)
    PerceptronSelection selected = SelectReorderingWithMode(
        feat, global_modularity, global_degree_variance, global_hub_concentration,
        num_nodes, num_edges, mode, graph_name, bench, false);
    
    // Safety check: if perceptron selects an unavailable algorithm, fallback
#ifndef RABBIT_ENABLE
    if (selected.algo == RabbitOrder) {
        // Recompute without RabbitOrder by using heuristic fallback
        if (feat.degree_variance > 0.8) {
            selected = ResolveVariantSelection("HUBCLUSTERDBG", selected.score);
        } else if (feat.hub_concentration > 0.3) {
            selected = ResolveVariantSelection("HUBSORT", selected.score);
        } else {
            selected = ResolveVariantSelection("DBG", selected.score);
        }
    }
#endif
    
    // P1 1.3: FEF convergence validation for iterative benchmarks.
    // For PR/PR_SPMV/SSSP: if the selected algorithm's w_fef_convergence
    // weight is negative (i.e., it's expected to hurt convergence), try
    // runner-up from top-2 selection. ADAPTIVE_FEF_VALIDATE=1 enables.
    if (AblationConfig::Get().fef_validate &&
        (bench == BENCH_PR || bench == BENCH_PR_SPMV || bench == BENCH_SSSP)) {
        // Load perceptron weights for this graph's features
        auto pw = LoadPerceptronWeightsForFeatures(
            global_modularity, global_degree_variance, global_hub_concentration,
            feat.avg_degree, num_nodes, num_edges, false, feat.clustering_coeff, bench);
        auto it = pw.find(selected.variant_name);
        if (it != pw.end() && it->second.w_fef_convergence < -0.01) {
            // Selected algo has negative FEF convergence weight → bad for iterative
            // Try top-2 and see if runner-up has a positive convergence weight
            auto topk = SelectTopKFromWeights(feat, pw, bench, 2);
            if (topk.size() >= 2) {
                auto it2 = pw.find(topk[1].variant_name);
                if (it2 != pw.end() && it2->second.w_fef_convergence > 0.0) {
                    selected = topk[1];
                }
            }
        }
    }
    
    // P3 3.1f: DON-Lite neural ordering override.
    // When the perceptron is uncertain (low margin) on a large community,
    // override with the DON-Lite MLP-based ordering which can capture
    // nonlinear feature interactions that the linear perceptron misses.
    // DON-Lite constants (mirrored from reorder_don_lite.h to avoid circular include)
    constexpr size_t kDonLiteMinCommunity = 50000;
    constexpr double kDonLiteMarginThreshold = 0.1;

    if (AblationConfig::Get().don_lite &&
        feat.num_nodes >= kDonLiteMinCommunity) {
        // Check selection margin: |best_score - runner_up_score|
        // If below threshold, perceptron is uncertain → DON-Lite may do better.
        if (selected.margin < kDonLiteMarginThreshold) {
            selected.variant_name = "DON_LITE";
            // Keep selected.algo as-is (fallback if dispatch doesn't catch name)
        }
    }
    
    return selected;
}

// ============================================================================
// SAMPLED DEGREE FEATURE COMPUTATION
// ============================================================================

/**
 * @brief Result structure for sampled degree features
 * 
 * Contains key graph features computed via fast sampling.
 */
struct SampledDegreeFeatures {
    double degree_variance = 0.0;      ///< Normalized degree variance (CV)
    double hub_concentration = 0.0;    ///< Fraction of edges from top 10% degree nodes
    double avg_degree = 0.0;           ///< Sampled average degree
    double clustering_coeff = 0.0;     ///< Estimated clustering coefficient
    double estimated_modularity = 0.0; ///< Rough modularity estimate
    double packing_factor = 0.0;       ///< Hub neighbor co-location (IISWC'18)
    double forward_edge_fraction = 0.0;///< Fraction of edges (u,v) where u < v (GoGraph)
    double working_set_ratio = 0.0;    ///< graph_bytes / LLC_size (P-OPT)
    double wsr_l1 = 0.0;               ///< graph_bytes / L1_size (per-level P-OPT)
    double wsr_l2 = 0.0;               ///< graph_bytes / L2_size (per-level P-OPT)
    // DON-RL-inspired features (Zhao et al.)
    double vertex_significance_skewness = 0.0; ///< Skewness of per-vertex locality contributions (CV)
    double window_neighbor_overlap = 0.0;      ///< Mean fraction of neighbors within locality window
    // Sampled locality score (P1 3.1d)
    double sampled_locality_score = 0.0;       ///< Sampled F(σ) approximation — baseline ordering quality
    // P3 3.2: Transpose reuse distance (P-OPT inspired)
    double avg_reuse_distance = 0.0;           ///< Mean next-reuse distance from transpose (CSC)
    // ---- Paper-aligned variants (kept alongside approximations) ----
    double packing_factor_cl = 0.0;            ///< IISWC'18 cache-line packing: fraction of neighbors on same CL
    double locality_score_pairwise = 0.0;      ///< DON-RL pairwise F(σ): sampled Σ 1/(|σ(u)-σ(v)|) over edges
    double reuse_distance_lru = 0.0;           ///< P-OPT LRU stack distance: mean unique-accesses between reuses
};

/**
 * @brief Detect last-level cache (LLC) size in bytes.
 * Uses sysconf on Linux, falls back to 30 MB (common desktop LLC).
 */
inline size_t GetLLCSizeBytes() {
#if defined(__linux__)
    long llc = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (llc > 0) return static_cast<size_t>(llc);
    llc = sysconf(_SC_LEVEL2_CACHE_SIZE);
    if (llc > 0) return static_cast<size_t>(llc);
#endif
    return 30ULL * 1024 * 1024;  // 30 MB fallback
}

/// L1 data cache size in bytes. Falls back to 32 KB.
inline size_t GetL1SizeBytes() {
#if defined(__linux__)
    long l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    if (l1 > 0) return static_cast<size_t>(l1);
#endif
    return 32ULL * 1024;  // 32 KB fallback
}

/// L2 cache size in bytes. Falls back to 256 KB.
inline size_t GetL2SizeBytes() {
#if defined(__linux__)
    long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
    if (l2 > 0) return static_cast<size_t>(l2);
#endif
    return 256ULL * 1024;  // 256 KB fallback
}

/**
 * @brief Compute degree-based features via sampling
 * 
 * This is a fast feature computation method that samples vertices
 * to estimate degree variance and hub concentration without scanning
 * the entire graph.
 * 
 * @tparam GraphT Graph type (must support out_degree, out_neigh)
 * @param g Input graph
 * @param sample_size Number of vertices to sample (0 = auto-scale with graph size)
 *                    Auto-scale formula: max(5000, min(sqrt(N), 50000))
 *                    - Small graphs (<25M nodes): 5000 samples (sufficient coverage)
 *                    - Large graphs (25M-2.5B):   sqrt(N) samples (scales with size)
 *                    - Very large graphs (>2.5B): 50000 samples (bounded overhead)
 *                    Strided sampling ensures even spatial coverage regardless of
 *                    sample size, which works well for power-law degree distributions
 *                    since hubs are spread throughout the vertex ID space.
 * @param compute_clustering Whether to compute clustering coefficient (slower)
 * @return SampledDegreeFeatures with computed features
 */
template<typename GraphT>
inline SampledDegreeFeatures ComputeSampledDegreeFeatures(
    const GraphT& g,
    size_t sample_size = 0,
    bool compute_clustering = false)
{
    SampledDegreeFeatures result;
    
    const int64_t num_nodes = g.num_nodes();
    if (num_nodes < 10) return result;
    
    // Auto-scale sample size based on graph size if sample_size == 0
    // For graphs up to ~25M nodes, 5000 strided samples provide >0.02%
    // coverage which is sufficient for degree statistics on power-law
    // graphs. For larger graphs, sqrt(N) scaling maintains statistical
    // significance while capping at 50K to bound overhead.
    if (sample_size == 0) {
        sample_size = std::max(
            size_t(5000),
            std::min(
                static_cast<size_t>(std::sqrt(static_cast<double>(num_nodes))),
                size_t(50000)
            )
        );
    }
    
    // Adjust sample size to graph size
    sample_size = std::min(sample_size, static_cast<size_t>(num_nodes));
    std::vector<int64_t> sampled_degrees(sample_size);
    
    // Hybrid sampling: 80% evenly strided (spatial coverage) +
    // 20% highest-degree vertices (hub oversampling for power-law graphs).
    // This ensures hub concentration is computed from actual hubs rather
    // than uniformly-spaced IDs that may miss spatially clustered hubs.
    const size_t uniform_count = sample_size * 4 / 5;
    const size_t hub_extra = sample_size - uniform_count;
    
    // Phase 1: Uniform stride for spatial coverage
    double sum = 0.0;
    for (size_t i = 0; i < uniform_count; ++i) {
        int64_t node = (num_nodes > static_cast<int64_t>(uniform_count)) ? 
            static_cast<int64_t>((i * num_nodes) / uniform_count) : static_cast<int64_t>(i);
        sampled_degrees[i] = g.out_degree(node);
        sum += sampled_degrees[i];
    }
    
    // Phase 2: Hub oversampling — find highest-degree vertices from a
    // scan of the uniform sample, then replace the tail of sampled_degrees
    // with actual top-degree nodes. This is O(sample_size) not O(N).
    if (hub_extra > 0 && num_nodes > static_cast<int64_t>(sample_size)) {
        // Use the uniform sample to find a degree threshold for "hubs"
        std::vector<int64_t> sorted_copy(sampled_degrees.begin(),
                                          sampled_degrees.begin() + uniform_count);
        std::sort(sorted_copy.rbegin(), sorted_copy.rend());
        int64_t hub_threshold = sorted_copy[std::min(hub_extra, uniform_count) - 1];
        
        // Scan a second strided pass with offset to find high-degree nodes
        size_t found = 0;
        for (size_t i = 0; i < static_cast<size_t>(num_nodes) && found < hub_extra; ++i) {
            int64_t node = static_cast<int64_t>((i * num_nodes) / (hub_extra * 50));
            if (node >= num_nodes) break;
            int64_t deg = g.out_degree(node);
            if (deg >= hub_threshold) {
                sampled_degrees[uniform_count + found] = deg;
                sum += deg;
                found++;
            }
        }
        // Fill any remaining slots with strided samples
        for (size_t i = found; i < hub_extra; ++i) {
            int64_t node = static_cast<int64_t>(
                ((uniform_count + i) * num_nodes) / sample_size);
            sampled_degrees[uniform_count + i] = g.out_degree(node);
            sum += sampled_degrees[uniform_count + i];
        }
    } else {
        // Small graph: just fill remaining with stride
        for (size_t i = uniform_count; i < sample_size; ++i) {
            int64_t node = static_cast<int64_t>((i * num_nodes) / sample_size);
            sampled_degrees[i] = g.out_degree(node);
            sum += sampled_degrees[i];
        }
    }
    double sample_mean = sum / sample_size;
    result.avg_degree = sample_mean;
    
    // Compute variance
    double sum_sq_diff = 0.0;
    for (size_t i = 0; i < sample_size; ++i) {
        double diff = sampled_degrees[i] - sample_mean;
        sum_sq_diff += diff * diff;
    }
    double variance = (sample_size > 1) ? sum_sq_diff / (sample_size - 1) : 0.0;
    result.degree_variance = (sample_mean > 0) ? std::sqrt(variance) / sample_mean : 0.0;
    
    // Hub concentration: fraction of edges from top 10% degree nodes
    std::sort(sampled_degrees.rbegin(), sampled_degrees.rend());
    size_t top_10 = std::max(size_t(1), sample_size / 10);
    int64_t top_edge_sum = 0;
    int64_t total_edge_sum = 0;
    for (size_t i = 0; i < sample_size; ++i) {
        if (i < top_10) top_edge_sum += sampled_degrees[i];
        total_edge_sum += sampled_degrees[i];
    }
    result.hub_concentration = (total_edge_sum > 0) ? 
        static_cast<double>(top_edge_sum) / total_edge_sum : 0.0;
    
    // Packing Factor — hub neighborhood co-location proxy.
    // APPROXIMATION of IISWC'18 Algorithm 2:  The paper defines packing as
    // the ratio of hub *vertices* fitting in a cache line (vertices per CL ÷
    // expected CL per hub access, yielding values ≥1).  Our implementation
    // instead measures the *fraction* of hub *neighbors* with nearby IDs
    // (within a locality window), producing [0,1] values.  Both metrics
    // correlate with cache-line utilization, but the scales differ.
    // High packing = less benefit from hub-based reordering.
    {
        size_t pf_samples = std::min(sample_size, static_cast<size_t>(500));
        size_t hub_count = std::max(size_t(1), pf_samples / 10);
        int64_t locality_window = std::max(int64_t(64), num_nodes / 100);
        
        // We already have sampled_degrees sorted in descending order
        // Re-sample the actual hub nodes (not the sorted copy)
        size_t total_neighbors = 0;
        size_t colocated_neighbors = 0;
        
        for (size_t i = 0; i < hub_count; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(sample_size)) ?
                static_cast<int64_t>((i * num_nodes) / sample_size) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg < 2) continue;
            
            for (auto n : g.out_neigh(node)) {
                int64_t neighbor = static_cast<int64_t>(n);
                total_neighbors++;
                if (std::abs(neighbor - node) <= locality_window) {
                    colocated_neighbors++;
                }
            }
        }
        
        result.packing_factor = (total_neighbors > 0) ?
            static_cast<double>(colocated_neighbors) / total_neighbors : 0.0;
    }
    
    // Forward Edge Fraction (GoGraph): fraction of edges (u,v) where ID(u) < ID(v).
    // High forward fraction = ordering already respects data flow direction.
    // Important for async iterative algorithms (PR, SSSP).
    {
        size_t fef_samples = std::min(sample_size, static_cast<size_t>(2000));
        size_t forward_count = 0;
        size_t total_count = 0;
        
        for (size_t i = 0; i < fef_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(fef_samples)) ?
                static_cast<int64_t>((i * num_nodes) / fef_samples) : static_cast<int64_t>(i);
            
            for (auto n : g.out_neigh(node)) {
                int64_t neighbor = static_cast<int64_t>(n);
                total_count++;
                if (node < neighbor) {
                    forward_count++;
                }
            }
        }
        
        result.forward_edge_fraction = (total_count > 0) ?
            static_cast<double>(forward_count) / total_count : 0.5;
    }
    
    // Working Set Ratio (P-OPT): graph_bytes / LLC_size
    // Estimates how much of the graph's working set overflows the LLC.
    // ratio ≈ 1 → graph fits in cache → reordering has limited benefit
    // ratio >> 1 → graph exceeds cache → reordering can significantly help
    {
        int64_t num_edges = g.num_edges_directed();  // directed edge count
        // CSR working set ≈ offsets array + edges array + vertex data
        // offsets: (num_nodes+1) * sizeof(int64_t)
        // edges:   num_edges * sizeof(int32_t)  (NodeID)
        // vertex:  num_nodes * sizeof(double)   (PR values, distances, etc.)
        size_t graph_bytes = static_cast<size_t>(num_nodes + 1) * sizeof(int64_t) +
                             static_cast<size_t>(num_edges) * sizeof(int32_t) +
                             static_cast<size_t>(num_nodes) * sizeof(double);
        size_t llc_bytes = GetLLCSizeBytes();
        result.working_set_ratio = (llc_bytes > 0) ?
            static_cast<double>(graph_bytes) / llc_bytes : 0.0;
        // Per-level WSR (P-OPT: distinguishes L1-optimal vs L3-optimal algorithms)
        size_t l1_bytes = GetL1SizeBytes();
        size_t l2_bytes = GetL2SizeBytes();
        result.wsr_l1 = (l1_bytes > 0) ?
            static_cast<double>(graph_bytes) / l1_bytes : 0.0;
        result.wsr_l2 = (l2_bytes > 0) ?
            static_cast<double>(graph_bytes) / l2_bytes : 0.0;
    }

    // DON-RL Feature 1: Vertex Significance Skewness (Zhao et al.)
    // Coefficient of variation of per-vertex "locality contributions"
    // (degree × packing). High skewness → hub-dominated → HubSort works well.
    // Low skewness → uniform → community-based algorithms better.
    {
        size_t vss_samples = std::min(sample_size, static_cast<size_t>(2000));
        int64_t locality_window = std::max(int64_t(64), num_nodes / 100);
        std::vector<double> significance(vss_samples);

        for (size_t i = 0; i < vss_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(vss_samples)) ?
                static_cast<int64_t>((i * num_nodes) / vss_samples) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg == 0) { significance[i] = 0.0; continue; }

            // Count neighbors within locality window
            int64_t local_count = 0;
            for (auto n : g.out_neigh(node)) {
                if (std::abs(static_cast<int64_t>(n) - node) <= locality_window)
                    local_count++;
            }
            // Significance = degree * local fraction (how much this vertex
            // contributes to locality if placed optimally)
            significance[i] = static_cast<double>(deg) *
                               (static_cast<double>(local_count) / deg);
        }

        // Compute CV = std / mean
        double sig_sum = 0.0;
        for (size_t i = 0; i < vss_samples; ++i) sig_sum += significance[i];
        double sig_mean = sig_sum / vss_samples;

        double sig_var = 0.0;
        for (size_t i = 0; i < vss_samples; ++i) {
            double d = significance[i] - sig_mean;
            sig_var += d * d;
        }
        sig_var /= std::max(size_t(1), vss_samples - 1);
        result.vertex_significance_skewness = (sig_mean > 1e-12) ?
            std::sqrt(sig_var) / sig_mean : 0.0;
    }

    // DON-RL Feature 2: Window Neighbor Overlap
    // Mean fraction of a vertex's neighbors within a locality window.
    // Generalizes packing_factor with broader sampling — packing_factor
    // only samples hubs, this samples uniformly across the graph.
    {
        size_t wno_samples = std::min(sample_size, static_cast<size_t>(2000));
        int64_t locality_window = std::max(int64_t(64), num_nodes / 100);
        double total_overlap = 0.0;
        size_t counted = 0;

        for (size_t i = 0; i < wno_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(wno_samples)) ?
                static_cast<int64_t>((i * num_nodes) / wno_samples) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg == 0) continue;

            int64_t in_window = 0;
            for (auto n : g.out_neigh(node)) {
                if (std::abs(static_cast<int64_t>(n) - node) <= locality_window)
                    in_window++;
            }
            total_overlap += static_cast<double>(in_window) / deg;
            counted++;
        }
        result.window_neighbor_overlap = (counted > 0) ?
            total_overlap / counted : 0.0;
    }

    // P1 3.1d: Sampled Locality Score — simplified F(σ) proxy.
    // APPROXIMATION of DON-RL's F(σ): The paper computes F(σ) by summing
    // pairwise joint similarity over a permutation window, combining S_s
    // (common sibling count) and S_n (common neighbor count) for each pair.
    // Our proxy instead measures the fraction of a vertex's neighbors within
    // a narrow cache window (|neighbor_id − node_id| ≤ cache_window),
    // omitting the sibling-similarity (S_s) component entirely.
    // Both capture "how cache-friendly is the current ordering" but at
    // different granularity.  High value → already cache-friendly.
    // Complementary to window_neighbor_overlap which uses a broader window.
    {
        size_t loc_samples = std::min(sample_size, static_cast<size_t>(1000));
        // Use a narrow window approximating L1 cache line reach (~16 vertices)
        int64_t cache_window = std::max(int64_t(16), num_nodes / 1000);
        double total_locality = 0.0;
        size_t counted_loc = 0;

        for (size_t i = 0; i < loc_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(loc_samples)) ?
                static_cast<int64_t>((i * num_nodes) / loc_samples) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg == 0) continue;

            int64_t in_cache = 0;
            for (auto n : g.out_neigh(node)) {
                if (std::abs(static_cast<int64_t>(n) - node) <= cache_window)
                    in_cache++;
            }
            total_locality += static_cast<double>(in_cache) / deg;
            counted_loc++;
        }
        result.sampled_locality_score = (counted_loc > 0) ?
            total_locality / counted_loc : 0.0;
    }
    
    // P3 3.2: Transpose reuse distance — spatial ID-distance proxy.
    // APPROXIMATION of P-OPT's temporal reuse distance: The paper measures
    // the number of DISTINCT memory accesses between consecutive references
    // to the same cache line during an execution trace (LRU stack distance).
    // Our proxy uses SPATIAL vertex-ID distance: mean |in_neighbor_id − node_id|
    // from the transpose (CSC).  This correlates with temporal reuse because
    // vertices with far-apart IDs tend to map to distant cache lines, but it
    // does not capture access-pattern-specific reuse.
    // High reuse distance → graph benefits from reordering to reduce cache misses.
    {
        const size_t reuse_samples = std::min(sample_size, static_cast<size_t>(1000));
        double total_reuse = 0.0;
        size_t counted_reuse = 0;
        for (size_t i = 0; i < reuse_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(reuse_samples)) ?
                static_cast<int64_t>((i * num_nodes) / reuse_samples) : static_cast<int64_t>(i);
            int64_t in_deg = g.in_degree(node);
            if (in_deg == 0) continue;

            double sum_dist = 0.0;
            for (auto pred : g.in_neigh(node)) {
                sum_dist += std::abs(static_cast<int64_t>(pred) - node);
            }
            total_reuse += sum_dist / in_deg;
            counted_reuse++;
        }
        // Normalize by num_nodes to get a [0,1]-ish scale
        result.avg_reuse_distance = (counted_reuse > 0 && num_nodes > 0) ?
            (total_reuse / counted_reuse) / num_nodes : 0.0;
    }
    
    // ====================================================================
    // PAPER-ALIGNED FEATURE VARIANTS
    // Kept alongside the approximations above for comparison/training.
    // ====================================================================
    
    // IISWC'18 cache-line packing factor — fraction of a hub's neighbors
    // that map to the SAME cache line as the hub (vertex ID / CL_VERTS).
    // More faithful to Su et al.'s "packing factor" which measures cache-line
    // utilization rather than ID-distance proximity.
    {
        constexpr int64_t CL_BYTES = 64;  // x86 cache line size
        constexpr int64_t VERT_BYTES = 4; // sizeof(int32_t) vertex ID
        constexpr int64_t CL_VERTS = CL_BYTES / VERT_BYTES;  // 16 verts per CL
        
        size_t pf_samples = std::min(sample_size, static_cast<size_t>(500));
        size_t hub_count = std::max(size_t(1), pf_samples / 10);
        size_t total_neighbors = 0;
        size_t same_cl_neighbors = 0;
        
        for (size_t i = 0; i < hub_count; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(sample_size)) ?
                static_cast<int64_t>((i * num_nodes) / sample_size) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg < 2) continue;
            
            int64_t node_cl = node / CL_VERTS;
            for (auto n : g.out_neigh(node)) {
                int64_t neighbor = static_cast<int64_t>(n);
                total_neighbors++;
                if (neighbor / CL_VERTS == node_cl) {
                    same_cl_neighbors++;
                }
            }
        }
        result.packing_factor_cl = (total_neighbors > 0) ?
            static_cast<double>(same_cl_neighbors) / total_neighbors : 0.0;
    }
    
    // DON-RL pairwise locality score — sampled Σ 1/(|σ(u)−σ(v)|) over edges.
    // The paper defines F(σ) = Σ_{(u,v)∈E} 1/(|σ(u)−σ(v)|) as the locality
    // objective.  We sample edges uniformly and compute the mean reciprocal
    // ID-distance, normalized to [0,1] by dividing by 1 (since max single
    // contribution is 1 for adjacent IDs).
    {
        size_t loc_p_samples = std::min(sample_size, static_cast<size_t>(2000));
        double total_recip = 0.0;
        size_t counted_edges = 0;
        
        for (size_t i = 0; i < loc_p_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(loc_p_samples)) ?
                static_cast<int64_t>((i * num_nodes) / loc_p_samples) : static_cast<int64_t>(i);
            
            for (auto n : g.out_neigh(node)) {
                int64_t diff = std::abs(static_cast<int64_t>(n) - node);
                if (diff > 0) {
                    total_recip += 1.0 / static_cast<double>(diff);
                    counted_edges++;
                }
            }
        }
        result.locality_score_pairwise = (counted_edges > 0) ?
            total_recip / counted_edges : 0.0;
    }
    
    // P-OPT LRU stack distance — approximate temporal reuse distance using
    // a small LRU simulation.  For each sampled vertex, simulate cache-line
    // accesses to its neighbors and measure the mean LRU position (number of
    // unique cache lines accessed since last reference to same line).
    // This is much more faithful to P-OPT's stack distance than the spatial
    // ID-distance proxy (avg_reuse_distance) but more expensive.
    {
        constexpr int64_t CL_BYTES = 64;
        constexpr int64_t VERT_BYTES = 4;
        constexpr int64_t CL_VERTS = CL_BYTES / VERT_BYTES;
        constexpr size_t LRU_CAP = 64;  // small LRU cache (64 cache lines)
        
        size_t lru_samples = std::min(sample_size, static_cast<size_t>(500));
        double total_stack_dist = 0.0;
        size_t counted_access = 0;
        
        for (size_t i = 0; i < lru_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(lru_samples)) ?
                static_cast<int64_t>((i * num_nodes) / lru_samples) : static_cast<int64_t>(i);
            int64_t deg = g.out_degree(node);
            if (deg < 2) continue;
            
            // Mini LRU simulation: access neighbors' cache lines in order
            std::vector<int64_t> lru_stack;  // most-recent at back
            lru_stack.reserve(std::min(static_cast<size_t>(deg), LRU_CAP));
            
            for (auto n : g.out_neigh(node)) {
                int64_t cl = static_cast<int64_t>(n) / CL_VERTS;
                
                // Search for cl in LRU stack
                auto it = std::find(lru_stack.begin(), lru_stack.end(), cl);
                if (it != lru_stack.end()) {
                    // Hit: distance = number of unique lines between this and top
                    size_t dist = static_cast<size_t>(lru_stack.end() - it - 1);
                    total_stack_dist += static_cast<double>(dist);
                    // Move to most-recent
                    lru_stack.erase(it);
                    lru_stack.push_back(cl);
                } else {
                    // Miss: distance = LRU size (cold miss)
                    total_stack_dist += static_cast<double>(lru_stack.size());
                    lru_stack.push_back(cl);
                    if (lru_stack.size() > LRU_CAP) {
                        lru_stack.erase(lru_stack.begin());  // evict oldest
                    }
                }
                counted_access++;
            }
        }
        // Normalize by LRU_CAP to get [0,1]-ish scale
        result.reuse_distance_lru = (counted_access > 0) ?
            (total_stack_dist / counted_access) / static_cast<double>(LRU_CAP) : 0.0;
    }
    
    // Optional: Compute clustering coefficient (expensive but useful)
    // NOTE: Assumes symmetric CSR (invert=true). Counts undirected triplets
    // using out_neigh() traversal. Incorrect for directed-only graph inputs.
    if (compute_clustering && sample_size >= 100) {
        size_t triangles_sampled = 0;
        size_t triplets_sampled = 0;
        
        size_t cc_samples = std::min(sample_size, static_cast<size_t>(1000));
        for (size_t i = 0; i < cc_samples; ++i) {
            int64_t node = (num_nodes > static_cast<int64_t>(sample_size)) ? 
                static_cast<int64_t>((i * num_nodes) / sample_size) : static_cast<int64_t>(i);
            
            int64_t deg = g.out_degree(node);
            if (deg < 2) continue;
            
            triplets_sampled += deg * (deg - 1) / 2;
            
            // Count triangles (only for small neighborhoods)
            if (deg <= 100) {
                std::unordered_set<int64_t> neighbors;
                for (auto n : g.out_neigh(node)) {
                    neighbors.insert(static_cast<int64_t>(n));
                }
                for (auto n1 : g.out_neigh(node)) {
                    for (auto n2 : g.out_neigh(static_cast<int64_t>(n1))) {
                        if (neighbors.count(static_cast<int64_t>(n2))) {
                            triangles_sampled++;
                        }
                    }
                }
            }
        }
        
        result.clustering_coeff = (triplets_sampled > 0) ? 
            static_cast<double>(triangles_sampled) / (2 * triplets_sampled) : 0.0;
        
        // Note: estimated_modularity is set by ComputeFastModularity() below
        // if available. Fall back to the rough CC-based heuristic here.
        result.estimated_modularity = std::min(0.9, result.clustering_coeff * 1.5);
    }
    
    return result;
}

// ============================================================================
// EXTENDED FEATURE COMPUTATION (avg_path_length, diameter, community_count)
// ============================================================================

/**
 * @brief Extended structural features computed via sampled BFS
 *
 * These features complement SampledDegreeFeatures with path-based and
 * connectivity metrics. They are more expensive than degree sampling
 * (O(k×(V+E)) for k BFS sources) but provide information about graph
 * diameter, path lengths, and connected component structure that
 * degree statistics alone cannot capture.
 *
 * **avg_path_length** — Average shortest-path distance from k random
 * BFS sources. Each BFS is bounded to visit at most max_bfs_visits
 * nodes to keep overhead manageable on large graphs. Useful for
 * distinguishing "small-world" graphs (short paths) from road networks
 * (long paths); short-path graphs benefit more from hub-based reorderings.
 *
 * **diameter_estimate** — Maximum BFS depth observed across all sources.
 * A lower bound on the true diameter. Low-diameter graphs (social networks)
 * favor community-aware reorderings; high-diameter graphs (road networks,
 * meshes) favor bandwidth-minimization (RCM) or spatial reorderings.
 *
 * **component_count** — Number of connected components found via BFS
 * over the full graph. Multi-component graphs may benefit from reorderings
 * that group components contiguously in memory.
 */
struct ExtendedFeatures {
    double avg_path_length = 0.0;    ///< Mean BFS distance across sampled pairs
    int    diameter_estimate = 0;    ///< Max BFS depth (lower bound on diameter)
    int    component_count = 1;      ///< Number of connected components
};

/**
 * @brief Compute extended structural features via sampled BFS
 *
 * Runs multi-source BFS for path length and diameter estimation, plus
 * a full connected-components sweep. The BFS sources are strided across
 * the vertex ID space for even coverage.
 *
 * Overhead relative to degree sampling:
 *   - Small graphs  (<100K nodes): ~1-5 ms   (negligible)
 *   - Medium graphs (<10M nodes):  ~50-200 ms (acceptable)
 *   - Large graphs  (>10M nodes):  ~0.5-2 s   (still <1% of reorder time)
 *
 * For very large graphs (>100M nodes), the BFS visit cap limits each
 * source to visiting ~0.1% of the graph, giving a useful approximation
 * without scanning the entire structure.
 *
 * @tparam GraphT Graph type (must support out_degree, out_neigh, num_nodes)
 * @param g Input graph
 * @param num_bfs_sources Number of BFS sources for path/diameter estimation (default: 5)
 * @param max_bfs_visits Max nodes visited per BFS source (0 = auto-scale)
 * @return ExtendedFeatures with path length, diameter, and component count
 */
template<typename GraphT>
inline ExtendedFeatures ComputeExtendedFeatures(
    const GraphT& g,
    int num_bfs_sources = 5,
    size_t max_bfs_visits = 0)
{
    ExtendedFeatures result;
    const int64_t num_nodes = g.num_nodes();
    if (num_nodes < 10) return result;

    // Auto-scale BFS visit budget: min(100K, N/10) per source
    if (max_bfs_visits == 0) {
        max_bfs_visits = std::min(
            size_t(100000),
            std::max(size_t(1000), static_cast<size_t>(num_nodes) / 10)
        );
    }

    // ---- Path length & diameter via multi-source BFS ----
    double total_path_sum = 0.0;
    size_t total_path_count = 0;
    int max_depth_seen = 0;

    for (int src_idx = 0; src_idx < num_bfs_sources; ++src_idx) {
        // Strided source selection for even coverage
        int64_t source = static_cast<int64_t>(
            (static_cast<uint64_t>(src_idx) * num_nodes) / num_bfs_sources);

        // Skip isolated nodes — find nearest non-isolated vertex
        int64_t orig = source;
        while (source < num_nodes && g.out_degree(source) == 0) ++source;
        if (source >= num_nodes) {
            source = 0;
            while (source < orig && g.out_degree(source) == 0) ++source;
            if (g.out_degree(source) == 0) continue;  // all isolated
        }

        // BFS from source with visit cap
        std::vector<int32_t> dist(num_nodes, -1);
        std::queue<int64_t> bfs_q;
        bfs_q.push(source);
        dist[source] = 0;
        size_t visits = 0;

        while (!bfs_q.empty() && visits < max_bfs_visits) {
            int64_t u = bfs_q.front();
            bfs_q.pop();
            for (auto v_raw : g.out_neigh(u)) {
                int64_t v = static_cast<int64_t>(v_raw);
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    bfs_q.push(v);
                    total_path_sum += dist[v];
                    ++total_path_count;
                    ++visits;
                    if (dist[v] > max_depth_seen) {
                        max_depth_seen = dist[v];
                    }
                }
            }
        }
    }

    result.avg_path_length = (total_path_count > 0) ?
        total_path_sum / total_path_count : 1.0;
    result.diameter_estimate = max_depth_seen;

    // ---- Connected component count via BFS ----
    // Uses a visited bitvector; each unvisited node starts a new component.
    // Full O(V+E) scan but very cache-friendly since we visit in ID order.
    {
        std::vector<bool> visited(num_nodes, false);
        int components = 0;
        for (int64_t start = 0; start < num_nodes; ++start) {
            if (visited[start]) continue;
            ++components;
            // BFS from start
            std::queue<int64_t> q;
            q.push(start);
            visited[start] = true;
            while (!q.empty()) {
                int64_t u = q.front();
                q.pop();
                for (auto v_raw : g.out_neigh(u)) {
                    int64_t v = static_cast<int64_t>(v_raw);
                    if (!visited[v]) {
                        visited[v] = true;
                        q.push(v);
                    }
                }
            }
        }
        result.component_count = components;
    }

    return result;
}

// Wrapper to compute modularity using membership result structure
template <class G, class R>
inline double getModularity(const G &x, const R &a, double M)
{
    auto fc = [&](auto u)
    {
        return a.membership[u];
    };
    return modularityByOmp(x, fc, M, 1.0);
}

// Auto-resolution helper for Leiden based on graph density and degree stats
template<typename NodeID_, typename DestID_>
double LeidenAutoResolution(const CSRGraph<NodeID_, DestID_, true>& g) {
    return computeAutoResolution<NodeID_, DestID_>(g);
}

// ============================================================================
// MERGED COMMUNITY FEATURES
// ============================================================================

/**
 * @brief Features computed for a merged group of small communities
 * 
 * When processing many small communities together as one "mega community",
 * we compute features to decide which algorithm to use.
 */
struct MergedCommunityFeatures {
    size_t num_nodes = 0;
    size_t num_edges = 0;
    double density = 0.0;
    double degree_variance = 0.0;
    double hub_concentration = 0.0;
    double avg_degree = 0.0;
};

/**
 * Compute features for a merged group of nodes (small communities processed together).
 * 
 * This is used when many small communities are grouped together for batch processing.
 * We compute features of the induced subgraph to decide which algorithm to apply.
 * 
 * @tparam GraphT CSRGraph type
 * @tparam NodeID_ Node ID type
 * @param g The graph
 * @param nodes Vector of nodes in the merged group
 * @param node_set Set of nodes for fast membership lookup
 * @return Computed features
 */
template<typename GraphT, typename NodeID_>
MergedCommunityFeatures ComputeMergedCommunityFeatures(
    const GraphT& g,
    const std::vector<NodeID_>& nodes,
    const std::unordered_set<NodeID_>& node_set)
{
    MergedCommunityFeatures feat;
    feat.num_nodes = nodes.size();
    
    if (feat.num_nodes == 0) return feat;
    
    // Count internal edges and compute degree stats
    std::vector<int64_t> degrees(feat.num_nodes);
    int64_t total_deg = 0;
    
    for (size_t idx = 0; idx < feat.num_nodes; ++idx) {
        int64_t deg = 0;
        for (auto neighbor : g.out_neigh(nodes[idx])) {
            NodeID_ dest = static_cast<NodeID_>(neighbor);
            if (node_set.count(dest)) {
                deg++;
            }
        }
        degrees[idx] = deg;
        total_deg += deg;
    }
    
    // Each edge counted twice (once from each endpoint)
    feat.num_edges = total_deg / 2;
    
    // Average degree
    feat.avg_degree = (feat.num_nodes > 0) ? 
        static_cast<double>(total_deg) / feat.num_nodes : 0.0;
    
    // Density
    feat.density = (feat.num_nodes > 1) ? 
        feat.avg_degree / (feat.num_nodes - 1) : 0.0;
    
    // Degree variance (coefficient of variation)
    double sum_sq_diff = 0;
    for (auto d : degrees) {
        double diff = d - feat.avg_degree;
        sum_sq_diff += diff * diff;
    }
    feat.degree_variance = (feat.num_nodes > 1 && feat.avg_degree > 0) ? 
        std::sqrt(sum_sq_diff / (feat.num_nodes - 1)) / feat.avg_degree : 0.0;
    
    // Hub concentration: fraction of edges from top 10% degree nodes
    std::sort(degrees.rbegin(), degrees.rend());
    size_t top_10 = std::max(size_t(1), feat.num_nodes / 10);
    int64_t top_sum = 0;
    for (size_t i = 0; i < top_10 && i < degrees.size(); ++i) {
        top_sum += degrees[i];
    }
    feat.hub_concentration = (total_deg > 0) ? 
        static_cast<double>(top_sum) / total_deg : 0.0;
    
    return feat;
}

/**
 * Select algorithm for a merged group of small communities using heuristics.
 * 
 * This is a fast heuristic-based selection that doesn't require full perceptron
 * weights. Used for GraphBrew and as fallback for AdaptiveOrder.
 * 
 * @param feat Features of the merged community group
 * @return Selected reordering algorithm
 */
inline ReorderingAlgo SelectAlgorithmForSmallGroup(const MergedCommunityFeatures& feat) {
    // Too small - just degree sort via ORIGINAL
    if (feat.num_nodes < 100) {
        return ORIGINAL;
    }
    
    // Hub-dominated with high variance → HubClusterDBG
    if (feat.hub_concentration > 0.5 && feat.degree_variance > 1.5) {
        return HubClusterDBG;
    }
    
    // Hub-dominated → HubSort
    if (feat.hub_concentration > 0.4) {
        return HubSort;
    }
    
    // Dense graph → DBG
    if (feat.density > 0.05) {
        return DBG;
    }
    
    // Default for sparse graphs
    return HubSortDBG;
}

/**
 * Convert MergedCommunityFeatures to CommunityFeatures for perceptron selection.
 * 
 * @param merged Merged community features
 * @return CommunityFeatures struct compatible with SelectBestReorderingForCommunity
 */
inline CommunityFeatures MergedToCommunityFeatures(const MergedCommunityFeatures& merged) {
    CommunityFeatures feat;
    feat.num_nodes = merged.num_nodes;
    feat.num_edges = merged.num_edges;
    feat.internal_density = merged.density;
    feat.degree_variance = merged.degree_variance;
    feat.hub_concentration = merged.hub_concentration;
    feat.avg_degree = merged.avg_degree;
    feat.clustering_coeff = 0.0;  // Not computed for merged groups
    feat.avg_path_length = 0.0;
    feat.diameter_estimate = 0.0;
    feat.community_count = 1.0;
    feat.modularity = 0.0;
    return feat;
}

// ============================================================================
// COMMUNITY FEATURE COMPUTATION (Standalone version)
// ============================================================================

/**
 * @brief Compute features for a community (standalone template function)
 * 
 * This is a standalone version that can be used outside of BuilderBase.
 * Computes structural features useful for algorithm selection.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type (may include weights)
 * @tparam invert Whether graph stores incoming edges
 * @param comm_nodes Vector of node IDs in the community
 * @param g The graph
 * @param node_set Set of nodes for O(1) membership test
 * @param compute_extended Whether to compute expensive features (clustering, diameter)
 * @return CommunityFeatures struct with computed features
 */
template <typename NodeID_, typename DestID_, bool invert>
CommunityFeatures ComputeCommunityFeaturesStandalone(
    const std::vector<NodeID_>& comm_nodes,
    const CSRGraph<NodeID_, DestID_, invert>& g,
    const std::unordered_set<NodeID_>& node_set,
    bool compute_extended = true)
{
    CommunityFeatures feat;
    feat.num_nodes = comm_nodes.size();
    feat.modularity = 0.0;  // Set externally if needed
    
    if (feat.num_nodes < 2) {
        feat.num_edges = 0;
        feat.internal_density = 0.0;
        feat.avg_degree = 0.0;
        feat.degree_variance = 0.0;
        feat.hub_concentration = 0.0;
        feat.clustering_coeff = 0.0;
        feat.avg_path_length = 0.0;
        feat.diameter_estimate = 0.0;
        feat.community_count = 1.0;
        return feat;
    }

    // Count internal edges and compute degrees
    std::vector<size_t> internal_degrees(feat.num_nodes, 0);
    size_t total_internal_edges = 0;
    
    #pragma omp parallel for reduction(+:total_internal_edges)
    for (size_t i = 0; i < feat.num_nodes; ++i) {
        NodeID_ node = comm_nodes[i];
        size_t local_internal = 0;
        for (DestID_ neighbor : g.out_neigh(node)) {
            NodeID_ dest = static_cast<NodeID_>(neighbor);
            if (node_set.count(dest)) {
                ++local_internal;
            }
        }
        internal_degrees[i] = local_internal;
        total_internal_edges += local_internal;
    }
    
    feat.num_edges = total_internal_edges / 2; // undirected
    
    // Internal density: actual edges / possible edges
    size_t possible_edges = feat.num_nodes * (feat.num_nodes - 1) / 2;
    feat.internal_density = (possible_edges > 0) ? 
        static_cast<double>(feat.num_edges) / possible_edges : 0.0;
    
    // Average degree
    feat.avg_degree = (feat.num_nodes > 0) ? 
        static_cast<double>(total_internal_edges) / feat.num_nodes : 0.0;
    
    // Degree variance (normalized)
    double sum_sq_diff = 0.0;
    #pragma omp parallel for reduction(+:sum_sq_diff)
    for (size_t i = 0; i < feat.num_nodes; ++i) {
        double diff = internal_degrees[i] - feat.avg_degree;
        sum_sq_diff += diff * diff;
    }
    double variance = (feat.num_nodes > 1) ? sum_sq_diff / (feat.num_nodes - 1) : 0.0;
    feat.degree_variance = (feat.avg_degree > 0) ? 
        std::sqrt(variance) / feat.avg_degree : 0.0; // coefficient of variation
    
    // Hub concentration: fraction of edges from top 10% degree nodes
    std::vector<size_t> sorted_degrees = internal_degrees;
    std::sort(sorted_degrees.rbegin(), sorted_degrees.rend());
    size_t top_10_percent = std::max(size_t(1), feat.num_nodes / 10);
    size_t top_edges = 0;
    for (size_t i = 0; i < top_10_percent; ++i) {
        top_edges += sorted_degrees[i];
    }
    feat.hub_concentration = (total_internal_edges > 0) ? 
        static_cast<double>(top_edges) / total_internal_edges : 0.0;
    
    // Extended features (only for large communities)
    const size_t MIN_SIZE_FOR_EXTENDED = 1000;
    
    if (compute_extended && feat.num_nodes >= MIN_SIZE_FOR_EXTENDED) {
        // Clustering coefficient (fast sampled estimate)
        const size_t MAX_SAMPLES_CC = std::min(size_t(50), feat.num_nodes / 20 + 1);
        double total_cc = 0.0;
        size_t valid_cc_samples = 0;
        
        // Find high-degree nodes for sampling
        std::vector<std::pair<size_t, size_t>> deg_idx(feat.num_nodes);
        for (size_t i = 0; i < feat.num_nodes; ++i) {
            deg_idx[i] = {internal_degrees[i], i};
        }
        std::partial_sort(deg_idx.begin(), 
                          deg_idx.begin() + std::min(MAX_SAMPLES_CC, feat.num_nodes),
                          deg_idx.end(),
                          std::greater<std::pair<size_t, size_t>>());
        
        for (size_t s = 0; s < MAX_SAMPLES_CC && s < feat.num_nodes; ++s) {
            size_t idx = deg_idx[s].second;
            NodeID_ node = comm_nodes[idx];
            size_t deg = internal_degrees[idx];
            if (deg < 2 || deg > 500) continue;
            
            std::unordered_set<NodeID_> neighbor_set;
            for (DestID_ neighbor : g.out_neigh(node)) {
                NodeID_ dest = static_cast<NodeID_>(neighbor);
                if (node_set.count(dest)) {
                    neighbor_set.insert(dest);
                }
            }
            
            size_t triangles = 0;
            for (NodeID_ n1 : neighbor_set) {
                for (DestID_ n2_edge : g.out_neigh(n1)) {
                    NodeID_ n2 = static_cast<NodeID_>(n2_edge);
                    if (n2 > n1 && neighbor_set.count(n2)) {
                        ++triangles;
                    }
                }
            }
            
            double local_cc = (2.0 * triangles) / (deg * (deg - 1));
            total_cc += local_cc;
            ++valid_cc_samples;
        }
        feat.clustering_coeff = (valid_cc_samples > 0) ? 
            total_cc / valid_cc_samples : 0.0;
        
        // Diameter & Avg Path (single BFS from highest degree node)
        if (!deg_idx.empty()) {
            size_t start_idx = deg_idx[0].second;
            std::vector<int> dist(feat.num_nodes, -1);
            std::queue<size_t> bfs_queue;
            bfs_queue.push(start_idx);
            dist[start_idx] = 0;
            
            std::unordered_map<NodeID_, size_t> node_to_idx;
            node_to_idx.reserve(feat.num_nodes);
            for (size_t i = 0; i < feat.num_nodes; ++i) {
                node_to_idx[comm_nodes[i]] = i;
            }
            
            double path_sum = 0.0;
            size_t path_count = 0;
            size_t max_dist = 0;
            
            while (!bfs_queue.empty()) {
                size_t curr_idx = bfs_queue.front();
                bfs_queue.pop();
                NodeID_ curr_node = comm_nodes[curr_idx];
                
                for (DestID_ neighbor : g.out_neigh(curr_node)) {
                    NodeID_ dest = static_cast<NodeID_>(neighbor);
                    auto it = node_to_idx.find(dest);
                    if (it != node_to_idx.end() && dist[it->second] == -1) {
                        size_t dest_idx = it->second;
                        dist[dest_idx] = dist[curr_idx] + 1;
                        bfs_queue.push(dest_idx);
                        path_sum += dist[dest_idx];
                        ++path_count;
                        if (static_cast<size_t>(dist[dest_idx]) > max_dist) {
                            max_dist = dist[dest_idx];
                        }
                    }
                }
            }
            
            feat.avg_path_length = (path_count > 0) ? path_sum / path_count : 1.0;
            feat.diameter_estimate = static_cast<double>(max_dist);
            
            size_t unreached = 0;
            for (size_t i = 0; i < feat.num_nodes; ++i) {
                if (dist[i] == -1) ++unreached;
            }
            feat.community_count = (unreached > 0) ? 2.0 : 1.0;
        } else {
            feat.avg_path_length = 1.0;
            feat.diameter_estimate = 1.0;
            feat.community_count = 1.0;
        }
    } else {
        // Small community - use fast estimates
        feat.clustering_coeff = feat.internal_density;
        feat.avg_path_length = (feat.internal_density > 0.1) ? 1.5 : 
                               std::log2(feat.num_nodes + 1);
        feat.diameter_estimate = feat.avg_path_length * 2.0;
        feat.community_count = 1.0;
    }
    
    // Packing Factor (IISWC'18): for hub nodes, measure how many neighbors
    // have nearby original IDs (already co-located in memory).
    {
        size_t hub_count = std::max(size_t(1), feat.num_nodes / 10);
        int64_t locality_window = std::max(int64_t(64), 
            static_cast<int64_t>(feat.num_nodes) / 100);
        size_t total_neighbors = 0;
        size_t colocated_neighbors = 0;
        
        size_t pf_samples = std::min(hub_count, std::min(size_t(200), feat.num_nodes));
        
        // Build sorted degree index for hub selection
        std::vector<std::pair<size_t, size_t>> pf_deg_idx(feat.num_nodes);
        for (size_t i = 0; i < feat.num_nodes; ++i) {
            pf_deg_idx[i] = {internal_degrees[i], i};
        }
        std::partial_sort(pf_deg_idx.begin(),
                          pf_deg_idx.begin() + std::min(pf_samples, feat.num_nodes),
                          pf_deg_idx.end(),
                          std::greater<std::pair<size_t, size_t>>());
        
        for (size_t i = 0; i < pf_samples; ++i) {
            size_t idx = pf_deg_idx[i].second;
            NodeID_ node = comm_nodes[idx];
            
            for (DestID_ neighbor : g.out_neigh(node)) {
                NodeID_ dest = static_cast<NodeID_>(neighbor);
                if (node_set.count(dest)) {
                    total_neighbors++;
                    if (std::abs(static_cast<int64_t>(dest) - static_cast<int64_t>(node)) 
                        <= locality_window) {
                        colocated_neighbors++;
                    }
                }
            }
        }
        feat.packing_factor = (total_neighbors > 0) ?
            static_cast<double>(colocated_neighbors) / total_neighbors : 0.0;
    }
    
    // Forward Edge Fraction (GoGraph): fraction of edges where src < dst.
    // Predicts convergence speed for async iterative algorithms.
    {
        size_t forward_count = 0;
        size_t total_count = 0;
        size_t fef_samples = std::min(feat.num_nodes, size_t(2000));
        
        for (size_t i = 0; i < fef_samples; ++i) {
            size_t idx = (i * feat.num_nodes) / fef_samples;
            NodeID_ node = comm_nodes[idx];
            
            for (DestID_ neighbor : g.out_neigh(node)) {
                NodeID_ dest = static_cast<NodeID_>(neighbor);
                if (node_set.count(dest)) {
                    total_count++;
                    if (static_cast<int64_t>(node) < static_cast<int64_t>(dest)) {
                        forward_count++;
                    }
                }
            }
        }
        feat.forward_edge_fraction = (total_count > 0) ?
            static_cast<double>(forward_count) / total_count : 0.5;
    }
    
    // Working Set Ratio (P-OPT): community_bytes / LLC_size
    {
        // Community CSR working set estimate
        size_t comm_bytes = static_cast<size_t>(feat.num_nodes + 1) * sizeof(int64_t) +
                            static_cast<size_t>(feat.num_edges * 2) * sizeof(int32_t) +
                            static_cast<size_t>(feat.num_nodes) * sizeof(double);
        size_t llc_bytes = GetLLCSizeBytes();
        feat.working_set_ratio = (llc_bytes > 0) ?
            static_cast<double>(comm_bytes) / llc_bytes : 0.0;
    }
    
    // Paper-aligned features (cache-line packing, pairwise locality, LRU distance)
    {
        constexpr int64_t CL_VERTS = 64 / 4;  // 16 verts per cache line
        size_t pf_cl_total = 0, pf_cl_same = 0;
        double loc_recip_sum = 0.0;
        size_t loc_edge_count = 0;
        
        size_t pa_samples = std::min(feat.num_nodes, size_t(1000));
        for (size_t i = 0; i < pa_samples; ++i) {
            size_t idx = (i * feat.num_nodes) / pa_samples;
            NodeID_ node = comm_nodes[idx];
            int64_t node_cl = static_cast<int64_t>(node) / CL_VERTS;
            
            for (DestID_ neighbor : g.out_neigh(node)) {
                NodeID_ dest = static_cast<NodeID_>(neighbor);
                if (!node_set.count(dest)) continue;
                
                // Cache-line packing
                pf_cl_total++;
                if (static_cast<int64_t>(dest) / CL_VERTS == node_cl) pf_cl_same++;
                
                // Pairwise reciprocal distance
                int64_t diff = std::abs(static_cast<int64_t>(dest) - static_cast<int64_t>(node));
                if (diff > 0) {
                    loc_recip_sum += 1.0 / static_cast<double>(diff);
                    loc_edge_count++;
                }
            }
        }
        feat.packing_factor_cl = (pf_cl_total > 0) ?
            static_cast<double>(pf_cl_same) / pf_cl_total : 0.0;
        feat.locality_score_pairwise = (loc_edge_count > 0) ?
            loc_recip_sum / loc_edge_count : 0.0;
    }
    // LRU stack distance for community subgraph
    {
        constexpr int64_t CL_VERTS = 64 / 4;
        constexpr size_t LRU_CAP = 64;
        size_t lru_samples = std::min(feat.num_nodes, size_t(500));
        double total_sd = 0.0;
        size_t counted = 0;
        
        for (size_t i = 0; i < lru_samples; ++i) {
            size_t idx = (i * feat.num_nodes) / lru_samples;
            NodeID_ node = comm_nodes[idx];
            std::vector<int64_t> lru;
            lru.reserve(LRU_CAP);
            
            for (DestID_ neighbor : g.out_neigh(node)) {
                NodeID_ dest = static_cast<NodeID_>(neighbor);
                if (!node_set.count(dest)) continue;
                int64_t cl = static_cast<int64_t>(dest) / CL_VERTS;
                auto it = std::find(lru.begin(), lru.end(), cl);
                if (it != lru.end()) {
                    total_sd += static_cast<double>(lru.end() - it - 1);
                    lru.erase(it);
                    lru.push_back(cl);
                } else {
                    total_sd += static_cast<double>(lru.size());
                    lru.push_back(cl);
                    if (lru.size() > LRU_CAP) lru.erase(lru.begin());
                }
                counted++;
            }
        }
        feat.reuse_distance_lru = (counted > 0) ?
            (total_sd / counted) / static_cast<double>(LRU_CAP) : 0.0;
    }
    
    return feat;
}

#endif  // REORDER_TYPES_H_
