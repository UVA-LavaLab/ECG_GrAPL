/**
 * @file reorder_adaptive.h
 * @brief AdaptiveOrder - ML-based Algorithm Selection
 *
 * This header provides the AdaptiveOrder algorithm configuration and utilities.
 * AdaptiveOrder automatically selects the best reordering algorithm based on
 * graph features using a trained perceptron model.
 *
 * ============================================================================
 * ALGORITHM OVERVIEW
 * ============================================================================
 *
 * AdaptiveOrder (ID 14) analyzes graph structure and selects the best algorithm:
 * 
 * 1. FEATURE EXTRACTION
 *    Samples ~5000 vertices to compute:
 *    - degree_variance: Normalized variance in degree distribution (CV)
 *    - hub_concentration: Fraction of edges from top 10% degree nodes
 *    - avg_degree: Average vertex degree
 *    - clustering_coeff: Local clustering coefficient
 *    - estimated_modularity: Rough modularity from degree structure
 *    - packing_factor: Hub neighbor co-location (IISWC'18)
 *    - forward_edge_fraction: Fraction of edges (u,v) where u < v (GoGraph)
 *    - working_set_ratio: graph_bytes / LLC_size (P-OPT)
 *
 * 2. TYPE MATCHING
 *    - Compare features against trained type centroids (type_0, type_1, etc.)
 *    - Find closest matching graph type using Euclidean distance
 *    - Load corresponding perceptron weights
 *
 * 3. ALGORITHM SELECTION
 *    - Compute perceptron score for each candidate algorithm
 *    - Select algorithm with highest score
 *    - Safety fallback for unavailable algorithms
 *
 * ============================================================================
 * COMMAND LINE FORMAT
 * ============================================================================
 *
 * -o 14[:_[:_[:_[:selection_mode[:graph_name]]]]]
 *
 * Parameters (positions relative to algorithm ID):
 *   0-2: Reserved (currently unused by standalone entry point)
 *   3: selection_mode: 0=fastest-reorder, 1=fastest-execution (default),
 *                      2=best-endtoend, 3=best-amortization
 *   4: graph_name: Graph name for loading reorder times
 *
 * Examples:
 *   -o 14                           # Default: full-graph, fastest-execution
 *   -o 14::::0                      # Full-graph, fastest-reorder mode
 *   -o 14::::1:web-Google           # fastest-execution with graph name hint
 *
 * ============================================================================
 * SELECTION MODES
 * ============================================================================
 *
 * MODE_FASTEST_REORDER (0):
 *   Select algorithm with lowest reordering time.
 *   Best for: Unknown graphs, one-shot reordering.
 *
 * MODE_FASTEST_EXECUTION (1) [default]:
 *   Use perceptron to predict best cache performance.
 *   Best for: Repeated traversals on known graph types.
 *
 * MODE_BEST_ENDTOEND (2):
 *   Balance perceptron score with reorder time penalty.
 *   Best for: Single execution where total time matters.
 *
 * MODE_BEST_AMORTIZATION (3):
 *   Minimize iterations to amortize reorder cost.
 *   Best for: When you know the iteration count.
 *
 * ============================================================================
 * RUNTIME BEHAVIOR
 * ============================================================================
 *
 * The standalone entry point (GenerateAdaptiveMappingStandalone) always
 * delegates to GenerateAdaptiveMappingFullGraphStandalone. Full-graph
 * mode was found to outperform per-community mode because:
 *   1. Training data is whole-graph, so features match better
 *   2. No Leiden partitioning overhead
 *   3. Cross-community edge patterns are preserved
 *
 * GenerateAdaptiveMappingRecursiveStandalone exists but is not called
 * from the CLI entry point.
 *
 * Author: GraphBrew Team
 * License: See LICENSE.txt
 */

#ifndef REORDER_ADAPTIVE_H_
#define REORDER_ADAPTIVE_H_

#include <cstddef>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>
#include "reorder_types.h"

namespace adaptive {

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

// Default parameters for AdaptiveOrder - use unified defaults
constexpr int DEFAULT_MODE = 1;           // Per-community
constexpr int DEFAULT_RECURSION_DEPTH = 0;
constexpr double DEFAULT_RESOLUTION = reorder::DEFAULT_RESOLUTION;
constexpr size_t DEFAULT_MIN_RECURSE_SIZE = 50000;

// Thresholds for recursion decisions
constexpr double RECURSION_MODULARITY_THRESHOLD = 0.3;
constexpr size_t MIN_SIZE_FOR_RECURSION = 10000;

// Feature thresholds for heuristic fallback
namespace thresholds {
    constexpr double HIGH_DEGREE_VARIANCE = 0.8;
    constexpr double HIGH_HUB_CONCENTRATION = 0.3;
    constexpr double LOW_MODULARITY = 0.2;
    constexpr double HIGH_DENSITY = 0.1;
}

// ============================================================================
// ADAPTIVE MODE - imported from reorder_types.h
// ============================================================================
// AdaptiveMode, ParseAdaptiveMode, AdaptiveModeToString, AdaptiveModeToInt
// are defined in reorder_types.h at global scope
using ::AdaptiveMode;
using ::ParseAdaptiveMode;
using ::AdaptiveModeToString;
using ::AdaptiveModeToInt;

// ============================================================================
// CONFIGURATION STRUCTURE
// ============================================================================

/**
 * @brief Configuration for AdaptiveOrder algorithm
 */
struct AdaptiveConfig {
    AdaptiveMode mode = AdaptiveMode::PerCommunity;
    int max_depth = DEFAULT_RECURSION_DEPTH;
    double resolution = DEFAULT_RESOLUTION;
    size_t min_recurse_size = DEFAULT_MIN_RECURSE_SIZE;
    SelectionMode selection_mode = MODE_FASTEST_EXECUTION;
    std::string graph_name = "";
    BenchmarkType benchmark = BENCH_GENERIC;
    bool verbose = false;
    
    /**
     * Parse configuration from reordering options
     * Format: mode:depth:resolution:min_size:selection_mode:graph_name
     */
    static AdaptiveConfig FromOptions(const std::vector<std::string>& options) {
        AdaptiveConfig cfg;
        
        // Parse mode (param 0)
        if (options.size() > 0 && !options[0].empty()) {
            try {
                int mode_int = std::stoi(options[0]);
                cfg.mode = ParseAdaptiveMode(mode_int);
            } catch (...) {}
        }
        
        // Parse max_depth (param 1)
        if (options.size() > 1 && !options[1].empty()) {
            try {
                cfg.max_depth = std::stoi(options[1]);
            } catch (...) {}
        }
        
        // Parse resolution (param 2)
        if (options.size() > 2 && !options[2].empty()) {
            try {
                cfg.resolution = std::stod(options[2]);
            } catch (...) {}
        }
        
        // Parse min_recurse_size (param 3)
        if (options.size() > 3 && !options[3].empty()) {
            try {
                cfg.min_recurse_size = std::stoull(options[3]);
            } catch (...) {}
        }
        
        // Parse selection_mode (param 4)
        if (options.size() > 4 && !options[4].empty()) {
            try {
                int sm = std::stoi(options[4]);
                switch (sm) {
                    case 0: cfg.selection_mode = MODE_FASTEST_REORDER; break;
                    case 1: cfg.selection_mode = MODE_FASTEST_EXECUTION; break;
                    case 2: cfg.selection_mode = MODE_BEST_ENDTOEND; break;
                    case 3: cfg.selection_mode = MODE_BEST_AMORTIZATION; break;
                    default: cfg.selection_mode = MODE_FASTEST_EXECUTION; break;
                }
            } catch (...) {}
        }
        
        // Parse graph_name (param 5)
        if (options.size() > 5 && !options[5].empty()) {
            cfg.graph_name = options[5];
        }
        
        return cfg;
    }
    
    /**
     * Print configuration for verbose output
     */
    void print() const {
        std::cout << "AdaptiveOrder Configuration:\n";
        std::cout << "  Mode: " << AdaptiveModeToString(mode) << "\n";
        std::cout << "  Max Depth: " << max_depth << "\n";
        std::cout << "  Resolution: " << resolution << "\n";
        std::cout << "  Min Recurse Size: " << min_recurse_size << "\n";
        std::cout << "  Selection Mode: " << SelectionModeToString(selection_mode) << "\n";
        if (!graph_name.empty()) {
            std::cout << "  Graph Name: " << graph_name << "\n";
        }
    }
};

// ============================================================================
// HEURISTIC FALLBACK
// ============================================================================

/**
 * Heuristic algorithm selection for edge cases.
 * 
 * Used when:
 * - Perceptron selects unavailable algorithm (e.g., RabbitOrder when disabled)
 * - Very small communities where perceptron overhead isn't worth it
 * 
 * @param feat Community features
 * @return Selected algorithm based on simple heuristics
 */
inline ReorderingAlgo SelectHeuristicFallback(const CommunityFeatures& feat) {
    // High degree variance → hub-based approaches work well
    if (feat.degree_variance > thresholds::HIGH_DEGREE_VARIANCE) {
        return HubClusterDBG;
    }
    
    // High hub concentration → hub sorting helps
    if (feat.hub_concentration > thresholds::HIGH_HUB_CONCENTRATION) {
        return HubSort;
    }
    
    // Low modularity, high density → simple grouping is sufficient
    if (feat.modularity < thresholds::LOW_MODULARITY && 
        feat.internal_density > thresholds::HIGH_DENSITY) {
        return Sort;
    }
    
    // Default: DBG is a safe middle ground
    return DBG;
}

/**
 * Check if a community is suitable for recursive processing.
 * 
 * Criteria:
 * 1. Large enough to benefit from recursion
 * 2. Has sufficient community structure (modularity)
 * 3. Not too dense (dense graphs don't need complex reordering)
 * 
 * @param feat Community features
 * @param min_size Minimum size for recursion
 * @return true if community should be recursively processed
 */
inline bool ShouldRecurse(const CommunityFeatures& feat, size_t min_size) {
    // Too small
    if (feat.num_nodes < min_size) return false;
    
    // Has community structure
    if (feat.modularity < RECURSION_MODULARITY_THRESHOLD) return false;
    
    // Not too dense
    if (feat.internal_density > 0.1) return false;
    
    return true;
}

} // namespace adaptive

// Import key types and functions into global scope for convenience
using adaptive::AdaptiveMode;
using adaptive::AdaptiveConfig;
using adaptive::SelectHeuristicFallback;
using adaptive::ShouldRecurse;

// ============================================================================
// STANDALONE ADAPTIVE IMPLEMENTATIONS
// ============================================================================

/**
 * @brief Full-Graph Adaptive Mode (Standalone)
 * 
 * Analyzes entire graph features and selects a single best algorithm.
 * Uses ApplyBasicReorderingStandalone for algorithm dispatch.
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateAdaptiveMappingFullGraphStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g,
    pvector<NodeID_>& new_ids,
    bool useOutdeg,
    const std::vector<std::string>& reordering_options = {}) {
    
    Timer tm;
    tm.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges_directed();
    
    std::cout << "=== Full-Graph Adaptive Mode (Standalone) ===\n";
    std::cout << "Nodes: " << num_nodes << ", Edges: " << num_edges << "\n";
    
    // GUARD: Empty graph - create identity mapping
    if (num_nodes == 0) {
        tm.Stop();
        PrintTime("AdaptiveOrder Total Time", tm.Seconds());
        return;
    }
    
    // Compute global features (auto-scaled sample size)
    auto features = ::ComputeSampledDegreeFeatures(g, 0, true);
    
    // Compute extended features (avg_path_length, diameter, component_count)
    auto ext_features = ::ComputeExtendedFeatures(g);
    
    double global_modularity = features.estimated_modularity;
    double global_degree_variance = features.degree_variance;
    double global_hub_concentration = features.hub_concentration;
    double global_avg_degree = (num_nodes > 0) ? static_cast<double>(num_edges) / num_nodes : 0.0;
    double clustering_coeff = features.clustering_coeff;
    
    // Detect graph type
    GraphType detected_graph_type = DetectGraphType(
        global_modularity, global_degree_variance, global_hub_concentration,
        global_avg_degree, static_cast<size_t>(num_nodes));
    
    std::cout << "Graph Type: " << GraphTypeToString(detected_graph_type) << "\n";
    PrintTime("Degree Variance", global_degree_variance);
    PrintTime("Hub Concentration", global_hub_concentration);
    PrintTime("Modularity", global_modularity);
    
    // Create community features for the whole graph
    CommunityFeatures global_feat;
    global_feat.num_nodes = num_nodes;
    global_feat.num_edges = num_edges;
    global_feat.internal_density = (num_nodes > 1) ? global_avg_degree / (num_nodes - 1) : 0.0;
    global_feat.avg_degree = global_avg_degree;
    global_feat.degree_variance = global_degree_variance;
    global_feat.hub_concentration = global_hub_concentration;
    global_feat.clustering_coeff = clustering_coeff;
    global_feat.packing_factor = features.packing_factor;
    global_feat.forward_edge_fraction = features.forward_edge_fraction;
    global_feat.working_set_ratio = features.working_set_ratio;
    global_feat.vertex_significance_skewness = features.vertex_significance_skewness;
    global_feat.window_neighbor_overlap = features.window_neighbor_overlap;
    global_feat.sampled_locality_score = features.sampled_locality_score;
    global_feat.avg_reuse_distance = features.avg_reuse_distance;
    global_feat.packing_factor_cl = features.packing_factor_cl;
    global_feat.locality_score_pairwise = features.locality_score_pairwise;
    global_feat.reuse_distance_lru = features.reuse_distance_lru;
    
    // Populate extended features (previously always 0 at runtime)
    global_feat.avg_path_length = ext_features.avg_path_length;
    global_feat.diameter_estimate = static_cast<double>(ext_features.diameter_estimate);
    global_feat.community_count = static_cast<double>(ext_features.component_count);
    
    std::cout << "Avg Path Length: " << ext_features.avg_path_length << "\n";
    std::cout << "Diameter Estimate: " << ext_features.diameter_estimate << "\n";
    std::cout << "Component Count: " << ext_features.component_count << "\n";
    
    // Parse selection model:criterion and graph name from options (positions 3 & 4)
    // Supports both new "model:criterion" format (e.g. "dt:e2e") and legacy
    // integer modes (0-6) for backward compatibility.
    SelectionModel selection_model = SELECTION_MODEL_PERCEPTRON;
    SelectionCriterion selection_criterion = CRITERION_FASTEST_EXECUTION;
    SelectionMode selection_mode = MODE_FASTEST_EXECUTION;  // kept for legacy callers
    std::string graph_name;
    
    if (reordering_options.size() > 3 && !reordering_options[3].empty()
        && reordering_options[3] != "_") {
        auto mc = ParseModelCriterion(reordering_options[3]);
        selection_model = mc.first;
        selection_criterion = mc.second;
        // Also set legacy mode for any remaining callers
        try {
            int mode_val = std::stoi(reordering_options[3]);
            if (mode_val >= 0 && mode_val <= 6)
                selection_mode = static_cast<SelectionMode>(mode_val);
        } catch (...) {
            selection_mode = GetSelectionMode(reordering_options[3]);
        }
    }
    if (reordering_options.size() > 4 && !reordering_options[4].empty()
        && reordering_options[4] != "_") {
        graph_name = reordering_options[4];
    }
    // Fallback: use the global graph-name hint (auto-extracted from filename)
    if (graph_name.empty()) {
        graph_name = GetGraphNameHint();
    }
    
    std::cout << "Selection: model=" << SelectionModelToString(selection_model)
              << " criterion=" << SelectionCriterionToString(selection_criterion);
    if (!graph_name.empty()) std::cout << " (graph: " << graph_name << ")";
    std::cout << "\n";
    
    // Select best algorithm
    PerceptronSelection best = SelectBestReorderingForCommunity(
        global_feat, global_modularity, global_degree_variance, global_hub_concentration,
        global_avg_degree, static_cast<size_t>(num_nodes), num_edges,
        GetBenchmarkTypeHint(), detected_graph_type, selection_mode, graph_name);
    
    // Complexity guard constants (shared by Top-K and single-selection paths)
    constexpr int64_t GORDER_MAX_NODES = 500000;
    constexpr int64_t CORDER_MAX_NODES = 2000000;
    
    // Helper lambda: apply complexity guard to a candidate, returning a safe
    // fallback selection when the algorithm is too expensive for this graph.
    auto apply_complexity_guard = [&](PerceptronSelection sel) -> PerceptronSelection {
        if ((sel.algo == GOrder && num_nodes > GORDER_MAX_NODES) ||
            (sel.algo == COrder && num_nodes > CORDER_MAX_NODES)) {
            PerceptronSelection fallback;
            if (global_hub_concentration > 0.5 && global_degree_variance > 1.5)
                fallback = ResolveVariantSelection("HUBCLUSTERDBG", sel.score);
            else if (global_hub_concentration > 0.3)
                fallback = ResolveVariantSelection("HUBSORT", sel.score);
            else
                fallback = ResolveVariantSelection("DBG", sel.score);
            std::cout << "Complexity guard: " << num_nodes << " nodes, "
                      << sel.variant_name << " -> " << fallback.variant_name << "\n";
            return fallback;
        }
        return sel;
    };
    
    // Mode 3: True Top-K execution
    // When ADAPTIVE_TOP_K > 1, try each of the top-K candidates: apply the
    // reordering, evaluate with a fast locality metric (average edge gap =
    // mean |new_id[u] - new_id[v]| over all edges), and keep the reordering
    // with the best cache locality.
    const int top_k = AblationConfig::Get().top_k;
    bool top_k_handled = false;
    if (top_k > 1) {
        auto weights = LoadPerceptronWeightsForFeatures(
            global_modularity, global_degree_variance, global_hub_concentration,
            global_avg_degree, static_cast<size_t>(num_nodes), num_edges, false,
            clustering_coeff, GetBenchmarkTypeHint());
        auto candidates = SelectTopKFromWeights(
            global_feat, weights, GetBenchmarkTypeHint(), top_k);
        
        std::cout << "=== Top-" << top_k << " Execution ===\n";
        
        double best_avg_gap = std::numeric_limits<double>::max();
        pvector<NodeID_> best_ids(num_nodes, -1);
        
        for (size_t ci = 0; ci < candidates.size(); ci++) {
            PerceptronSelection cand = apply_complexity_guard(candidates[ci]);
            
            Timer trial_tm;
            trial_tm.Start();
            
            pvector<NodeID_> trial_ids(num_nodes, -1);
            ApplyBasicReorderingStandalone<NodeID_, DestID_, WeightT_, invert>(
                g, trial_ids, cand, useOutdeg, "");
            
            // Locality metric: average |new_id[u] - new_id[v]| over all edges.
            // Lower is better — nodes connected by edges are closer in memory.
            double gap_sum = 0.0;
            int64_t edge_count = 0;
            #pragma omp parallel for reduction(+:gap_sum,edge_count)
            for (NodeID_ u = 0; u < num_nodes; u++) {
                for (DestID_ neighbor : g.out_neigh(u)) {
                    NodeID_ v;
                    if (g.is_weighted())
                        v = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                    else
                        v = static_cast<NodeID_>(neighbor);
                    if (trial_ids[u] >= 0 && trial_ids[v] >= 0) {
                        gap_sum += std::abs(
                            static_cast<double>(trial_ids[u]) - trial_ids[v]);
                        edge_count++;
                    }
                }
            }
            double avg_gap = (edge_count > 0) ? gap_sum / edge_count : 0.0;
            
            trial_tm.Stop();
            
            std::cout << "  " << (ci + 1) << ". " << cand.variant_name
                      << " (score=" << cand.score
                      << ", avg_gap=" << std::fixed << std::setprecision(1)
                      << avg_gap << ", time=" << trial_tm.Seconds() << "s)\n";
            
            if (avg_gap < best_avg_gap) {
                best_avg_gap = avg_gap;
                std::swap(best_ids, trial_ids);
                best = cand;
            }
        }
        
        if (best_avg_gap < std::numeric_limits<double>::max()) {
            // Copy winning mapping to output
            #pragma omp parallel for
            for (NodeID_ n = 0; n < num_nodes; n++) {
                new_ids[n] = best_ids[n];
            }
            top_k_handled = true;
            std::cout << "=== Top-K Winner: " << best.variant_name
                      << " (avg_gap=" << std::fixed << std::setprecision(1)
                      << best_avg_gap << ") ===\n";
        }
    }
    
    if (!top_k_handled) {
        // Single-selection path (top_k <= 1 or Top-K produced no valid result)
        best = apply_complexity_guard(best);
        
        std::cout << "\n=== Selected Algorithm: " << best.variant_name << " ===\n";
        
        // Use standalone dispatcher
        ApplyBasicReorderingStandalone<NodeID_, DestID_, WeightT_, invert>(
            g, new_ids, best, useOutdeg, "");
    }
    
    tm.Stop();
    PrintTime("Full-Graph Adaptive Time", tm.Seconds());
}

/**
 * @brief Recursive Adaptive Mapping (Standalone)
 * 
 * Uses GVE-Leiden for community detection (native, no external library).
 * For each community, selects best algorithm based on features.
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateAdaptiveMappingRecursiveStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g,
    pvector<NodeID_>& new_ids,
    bool useOutdeg,
    const std::vector<std::string>& reordering_options,
    int depth = 0,
    bool verbose = true,
    SelectionMode selection_mode = MODE_FASTEST_EXECUTION,
    const std::string& graph_name = "") {
    
    Timer tm;
    tm.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges_directed();
    
    // GUARD: Empty graph - nothing to partition
    if (num_nodes == 0) {
        tm.Stop();
        PrintTime("Adaptive Map Time", tm.Seconds());
        return;
    }
    
    // Parse options
    int MAX_DEPTH = 0;
    size_t MIN_COMMUNITY_FOR_RECURSION = 50000;
    double resolution = LeidenAutoResolution<NodeID_, DestID_>(g);
    int max_iterations = 30;
    int max_passes = 30;
    
    if (reordering_options.size() > 0) {
        double first_val = std::stod(reordering_options[0]);
        if (first_val >= 0 && first_val <= 10 && std::floor(first_val) == first_val) {
            MAX_DEPTH = static_cast<int>(first_val);
        } else {
            resolution = first_val;
        }
    }
    if (reordering_options.size() > 1) {
        resolution = std::stod(reordering_options[1]);
    }
    if (reordering_options.size() > 2) {
        MIN_COMMUNITY_FOR_RECURSION = std::stoul(reordering_options[2]);
    }
    
    if (depth == 0 && verbose) {
        PrintTime("Max Depth", static_cast<double>(MAX_DEPTH));
        PrintTime("Resolution", resolution);
        PrintTime("Min Recurse Size", static_cast<double>(MIN_COMMUNITY_FOR_RECURSION));
        // Print active ablation toggles
        AblationConfig::Get().print();
    }
    
    // Ablation: ADAPTIVE_NO_LEIDEN=1 — skip Leiden, treat whole graph as one community.
    // All nodes go to community 0, bypassing partitioning entirely.
    
    // Use GraphBrew's Leiden engine for community detection (native CSR)
    // NOTE: Parallel Leiden (OMP_NUM_THREADS > 1) is non-deterministic due to
    // concurrent community updates in localMovingPhase. For reproducible results,
    // set OMP_NUM_THREADS=1 or use precomputed label maps (--precompute).
    Timer t_leiden;
    t_leiden.Start();
    
    std::vector<K> comm_ids_k;
    double global_modularity = 0.0;
    
    if (AblationConfig::Get().no_leiden) {
        // Ablation: skip Leiden, treat entire graph as one community
        comm_ids_k.assign(num_nodes, K(0));
        global_modularity = 0.0;
        if (verbose) printf("ABLATION: Leiden skipped, single community\n");
    } else {
        graphbrew::GraphBrewConfig gb_config;
        gb_config.resolution = resolution;
        gb_config.maxIterations = max_iterations;
        gb_config.maxPasses = max_passes;
        gb_config.ordering = graphbrew::OrderingStrategy::COMMUNITY_SORT;
        auto gb_result = graphbrew::runGraphBrew<K>(g, gb_config);
        comm_ids_k = gb_result.membership;
        global_modularity = gb_result.modularity;
    }
    
    t_leiden.Stop();
    
    // Convert to size_t
    std::vector<size_t> comm_ids(num_nodes);
    K max_comm = 0;
    for (int64_t v = 0; v < num_nodes; ++v) {
        comm_ids[v] = static_cast<size_t>(comm_ids_k[v]);
        max_comm = std::max(max_comm, comm_ids_k[v]);
    }
    size_t num_communities = static_cast<size_t>(max_comm + 1);
    
    if (depth == 0 && verbose) {
        PrintTime("Modularity", global_modularity);
        PrintTime("Num Communities", static_cast<double>(num_communities));
    }
    
    // Compute global features
    Timer t_features;
    t_features.Start();
    auto deg_features = ::ComputeSampledDegreeFeatures(g, 0, true);
    double global_degree_variance = deg_features.degree_variance;
    double global_hub_concentration = deg_features.hub_concentration;
    double global_avg_degree = (num_nodes > 0) ? static_cast<double>(num_edges) / num_nodes : 0.0;
    t_features.Stop();
    
    // Detect graph type
    GraphType detected_graph_type = DetectGraphType(
        global_modularity, global_degree_variance, global_hub_concentration,
        global_avg_degree, static_cast<size_t>(num_nodes));
    
    if (depth == 0 && verbose) {
        std::cout << "Graph Type: " << GraphTypeToString(detected_graph_type) << "\n";
        PrintTime("Degree Variance", global_degree_variance);
        PrintTime("Hub Concentration", global_hub_concentration);
    }
    
    // Count community sizes
    std::vector<size_t> comm_freq(num_communities, 0);
    for (int64_t v = 0; v < num_nodes; ++v) {
        comm_freq[comm_ids[v]]++;
    }
    
    // Compute dynamic thresholds
    size_t non_empty_communities = 0;
    for (size_t c = 0; c < num_communities; ++c) {
        if (comm_freq[c] > 0) non_empty_communities++;
    }
    size_t avg_community_size = (non_empty_communities > 0) ?
        static_cast<size_t>(num_nodes) / non_empty_communities : static_cast<size_t>(num_nodes);
    
    const size_t MIN_FEATURES_SAMPLE = ComputeDynamicMinCommunitySize(
        static_cast<size_t>(num_nodes), non_empty_communities, avg_community_size);
    const size_t MIN_LOCAL_REORDER = ComputeDynamicLocalReorderThreshold(
        static_cast<size_t>(num_nodes), non_empty_communities, avg_community_size);
    
    if (depth == 0 && verbose) {
        printf("\n=== Adaptive Reordering Selection (Depth %d, Modularity: %.4f) ===\n",
               depth, global_modularity);
        printf("Dynamic thresholds: MIN_FEATURES=%zu, MIN_LOCAL_REORDER=%zu (avg_comm=%zu, num_comm=%zu)\n",
               MIN_FEATURES_SAMPLE, MIN_LOCAL_REORDER, avg_community_size, non_empty_communities);
    }
    
    // Collect small community nodes
    std::vector<NodeID_> small_community_nodes;
    std::vector<bool> is_small_community(num_communities, false);
    
    for (size_t c = 0; c < num_communities; ++c) {
        if (comm_freq[c] > 0 && comm_freq[c] < MIN_LOCAL_REORDER) {
            is_small_community[c] = true;
        }
    }
    
    for (int64_t v = 0; v < num_nodes; ++v) {
        if (is_small_community[comm_ids[v]]) {
            small_community_nodes.push_back(static_cast<NodeID_>(v));
        }
    }
    
    // Get large communities
    std::vector<std::pair<size_t, size_t>> freq_comm_pairs;
    for (size_t c = 0; c < num_communities; ++c) {
        if (comm_freq[c] >= MIN_LOCAL_REORDER) {
            freq_comm_pairs.emplace_back(comm_freq[c], c);
        }
    }
    std::sort(freq_comm_pairs.begin(), freq_comm_pairs.end(), std::greater<>());
    
    std::vector<size_t> top_communities;
    std::vector<bool> is_top_community(num_communities, false);
    for (auto& [freq, comm] : freq_comm_pairs) {
        top_communities.push_back(comm);
        is_top_community[comm] = true;
    }
    
    // Process communities and assign new IDs
    NodeID_ current_id = 0;
    
    // Stage timers for per-community work
    double t_comm_features_total = 0.0;
    double t_comm_scoring_total = 0.0;
    double t_comm_reorder_total = 0.0;
    
    // First: handle small communities
    double t_small_total = 0.0;
    if (!small_community_nodes.empty()) {
        Timer t_small;
        t_small.Start();
        std::unordered_set<NodeID_> small_node_set(
            small_community_nodes.begin(), small_community_nodes.end());
        
        auto merged_feat = ::ComputeMergedCommunityFeatures(g, small_community_nodes, small_node_set);
        
        // Use whole-graph features for the merged small-community group.
        // The perceptron was trained on whole-graph features, and the merged
        // group is 96-98% of nodes, so global features are a much better match
        // than recomputed subgraph features. This avoids the distribution
        // mismatch between training data and runtime features.
        CommunityFeatures comm_feat;
        comm_feat.num_nodes = num_nodes;
        comm_feat.num_edges = num_edges;
        comm_feat.internal_density = global_avg_degree / std::max(1.0, static_cast<double>(num_nodes - 1));
        comm_feat.degree_variance = deg_features.degree_variance;
        comm_feat.hub_concentration = deg_features.hub_concentration;
        comm_feat.clustering_coeff = deg_features.clustering_coeff;
        comm_feat.packing_factor = deg_features.packing_factor;
        comm_feat.forward_edge_fraction = deg_features.forward_edge_fraction;
        comm_feat.working_set_ratio = deg_features.working_set_ratio;
        comm_feat.vertex_significance_skewness = deg_features.vertex_significance_skewness;
        comm_feat.window_neighbor_overlap = deg_features.window_neighbor_overlap;
        comm_feat.sampled_locality_score = deg_features.sampled_locality_score;
        comm_feat.avg_reuse_distance = deg_features.avg_reuse_distance;
        comm_feat.packing_factor_cl = deg_features.packing_factor_cl;
        comm_feat.locality_score_pairwise = deg_features.locality_score_pairwise;
        comm_feat.reuse_distance_lru = deg_features.reuse_distance_lru;
        const BenchmarkType bench_hint = GetBenchmarkTypeHint();
        if (verbose) {
            const char* bnames[] = {"GENERIC","PR","BFS","CC","SSSP","BC","TC","PR_SPMV","CC_SV"};
            printf("AdaptiveOrder: Benchmark hint = %s (%d)\n", 
                   bench_hint < 9 ? bnames[bench_hint] : "?", bench_hint);
        }
        PerceptronSelection small_sel = SelectBestReorderingForCommunity(
            comm_feat, global_modularity, global_degree_variance, global_hub_concentration,
            global_avg_degree, static_cast<size_t>(num_nodes), num_edges,
            bench_hint, detected_graph_type, selection_mode, graph_name);
        
        // Complexity guard: GOrder is O(n*m*w) and CORDER is O(n*m) — prohibitively
        // slow for large merged groups. Fall back to fast O(n+m) alternatives when
        // the merged group exceeds a node threshold.
        constexpr size_t EXPENSIVE_ALGO_MAX_NODES = 20000;
        if (small_community_nodes.size() > EXPENSIVE_ALGO_MAX_NODES) {
            if (small_sel.algo == GOrder || small_sel.algo == COrder) {
                // Re-select excluding expensive algorithms: use HubSort family or DBG
                // which are O(n log n) and produce reasonable locality
                if (deg_features.hub_concentration > 0.5 && deg_features.degree_variance > 1.5) {
                    small_sel = ResolveVariantSelection("HUBCLUSTERDBG", small_sel.score);
                } else if (deg_features.hub_concentration > 0.3) {
                    small_sel = ResolveVariantSelection("HUBSORT", small_sel.score);
                } else {
                    small_sel = ResolveVariantSelection("DBG", small_sel.score);
                }
                if (verbose) {
                    printf("  -> Complexity guard: large group (%zu > %zu nodes), using %s instead\n",
                           small_community_nodes.size(), EXPENSIVE_ALGO_MAX_NODES,
                           small_sel.variant_name.c_str());
                }
            }
        }
        
        if (verbose) {
            printf("AdaptiveOrder: Grouped %zu small communities (%zu nodes, %zu edges) -> %s\n",
                   non_empty_communities - top_communities.size(),
                   small_community_nodes.size(), merged_feat.num_edges,
                   small_sel.variant_name.c_str());
        }
        
        if (small_sel.algo == ORIGINAL || small_community_nodes.size() < 100) {
            // Simple degree sort
            std::vector<std::pair<int64_t, NodeID_>> degree_node_pairs;
            degree_node_pairs.reserve(small_community_nodes.size());
            for (NodeID_ node : small_community_nodes) {
                int64_t deg = useOutdeg ? g.out_degree(node) : g.in_degree(node);
                degree_node_pairs.emplace_back(-deg, node);
            }
            std::sort(degree_node_pairs.begin(), degree_node_pairs.end());
            for (auto& [neg_deg, node] : degree_node_pairs) {
                new_ids[node] = current_id++;
            }
        } else if (small_sel.variant_name == "DON_LITE") {
            // P3 3.1f: DON-Lite neural ordering override.
            // Build local subgraph and apply MLP-based reordering.
            std::unordered_map<NodeID_, NodeID_> g2l;
            std::vector<NodeID_> l2g(small_community_nodes.size());
            for (size_t i = 0; i < small_community_nodes.size(); ++i) {
                g2l[small_community_nodes[i]] = static_cast<NodeID_>(i);
                l2g[i] = small_community_nodes[i];
            }
            std::vector<std::pair<NodeID_, DestID_>> sub_edges;
            for (NodeID_ node : small_community_nodes) {
                NodeID_ ls = g2l[node];
                for (DestID_ nb : g.out_neigh(node)) {
                    if (small_node_set.count(static_cast<NodeID_>(nb)))
                        sub_edges.push_back({ls, static_cast<DestID_>(g2l[static_cast<NodeID_>(nb)])});
                }
            }
            if (!sub_edges.empty()) {
                auto sub_g = MakeLocalGraphFromELStandalone<NodeID_, DestID_, invert>(sub_edges, false);
                pvector<NodeID_> sub_ids(small_community_nodes.size(), -1);
                GenerateDonLiteMapping<NodeID_, DestID_, invert>(sub_g, sub_ids, useOutdeg);
                // Fix: Validate sub_ids before using as index — unmapped entries
                // are still -1 (UINT32_MAX for unsigned), which would OOB.
                std::vector<NodeID_> reordered(small_community_nodes.size());
                std::vector<bool> placed(small_community_nodes.size(), false);
                for (size_t i = 0; i < small_community_nodes.size(); ++i) {
                    NodeID_ sid = sub_ids[i];
                    if (sid < small_community_nodes.size()) {
                        reordered[sid] = l2g[i];
                        placed[sid] = true;
                    }
                }
                // Append any unplaced nodes (unmapped by DON-Lite)
                size_t fill = 0;
                for (size_t i = 0; i < small_community_nodes.size(); ++i) {
                    if (!placed[i]) {
                        while (fill < small_community_nodes.size() && placed[fill]) ++fill;
                        if (fill < small_community_nodes.size()) {
                            reordered[fill] = l2g[i];
                            placed[fill] = true;
                        }
                    }
                }
                for (NodeID_ node : reordered)
                    new_ids[node] = current_id++;
            } else {
                for (NodeID_ node : small_community_nodes)
                    new_ids[node] = current_id++;
            }
        } else {
            ReorderCommunitySubgraphStandalone<NodeID_, DestID_, WeightT_, invert>(
                g, small_community_nodes, small_node_set, small_sel, useOutdeg, new_ids, current_id);
        }
        t_small.Stop();
        t_small_total = t_small.Seconds();
    }
    
    // Then: process large communities
    for (size_t comm_id : top_communities) {
        // Collect nodes in this community
        std::vector<NodeID_> comm_nodes;
        comm_nodes.reserve(comm_freq[comm_id]);
        for (int64_t v = 0; v < num_nodes; ++v) {
            if (comm_ids[v] == comm_id) {
                comm_nodes.push_back(static_cast<NodeID_>(v));
            }
        }
        
        std::unordered_set<NodeID_> comm_node_set(comm_nodes.begin(), comm_nodes.end());
        
        // Compute features for this community
        Timer t_cf;
        t_cf.Start();
        auto feat = ComputeCommunityFeaturesStandalone<NodeID_, DestID_, invert>(
            comm_nodes, g, comm_node_set);
        t_cf.Stop();
        t_comm_features_total += t_cf.Seconds();
        
        // Select algorithm for this community
        Timer t_cs;
        t_cs.Start();
        PerceptronSelection selected = SelectBestReorderingForCommunity(
            feat, global_modularity, global_degree_variance, global_hub_concentration,
            global_avg_degree, static_cast<size_t>(num_nodes), num_edges,
            GetBenchmarkTypeHint(), detected_graph_type, selection_mode, graph_name);
        t_cs.Stop();
        t_comm_scoring_total += t_cs.Seconds();
        
        // Per-community complexity guard: GOrder O(n*m*w) is expensive even for
        // mid-size communities when there are hundreds of them. Also, GOrder can
        // produce invalid permutations on some subgraph topologies.
        // Only block for communities above EXPENSIVE_ALGO_MAX_NODES — small
        // communities where GOrder is genuinely optimal should not be overridden.
        constexpr size_t EXPENSIVE_ALGO_MAX_NODES = 20000;
        if ((selected.algo == GOrder || selected.algo == COrder) &&
            comm_nodes.size() > EXPENSIVE_ALGO_MAX_NODES) {
            if (feat.hub_concentration > 0.5 && feat.degree_variance > 1.5) {
                selected = ResolveVariantSelection("HUBCLUSTERDBG", selected.score);
            } else if (feat.hub_concentration > 0.3) {
                selected = ResolveVariantSelection("HUBSORT", selected.score);
            } else {
                selected = ResolveVariantSelection("DBG", selected.score);
            }
        }
        
        if (verbose && comm_nodes.size() >= MIN_FEATURES_SAMPLE) {
            printf("  Community %zu: %zu nodes, %zu edges -> %s\n",
                   comm_id, comm_nodes.size(), feat.num_edges, 
                   selected.variant_name.c_str());
        }
        
        // Apply algorithm
        Timer t_cr;
        t_cr.Start();
        if (selected.variant_name == "DON_LITE") {
            // P3 3.1f: DON-Lite neural ordering override for large community.
            std::unordered_map<NodeID_, NodeID_> g2l;
            std::vector<NodeID_> l2g(comm_nodes.size());
            for (size_t i = 0; i < comm_nodes.size(); ++i) {
                g2l[comm_nodes[i]] = static_cast<NodeID_>(i);
                l2g[i] = comm_nodes[i];
            }
            std::vector<std::pair<NodeID_, DestID_>> sub_edges;
            for (NodeID_ node : comm_nodes) {
                NodeID_ ls = g2l[node];
                for (DestID_ nb : g.out_neigh(node)) {
                    if (comm_node_set.count(static_cast<NodeID_>(nb)))
                        sub_edges.push_back({ls, static_cast<DestID_>(g2l[static_cast<NodeID_>(nb)])});
                }
            }
            if (!sub_edges.empty()) {
                auto sub_g = MakeLocalGraphFromELStandalone<NodeID_, DestID_, invert>(sub_edges, false);
                pvector<NodeID_> sub_ids(comm_nodes.size(), -1);
                GenerateDonLiteMapping<NodeID_, DestID_, invert>(sub_g, sub_ids, useOutdeg);
                // Fix: Validate sub_ids before using as index — unmapped entries
                // are still -1 (UINT32_MAX for unsigned), which would OOB.
                std::vector<NodeID_> reordered(comm_nodes.size());
                std::vector<bool> placed(comm_nodes.size(), false);
                for (size_t i = 0; i < comm_nodes.size(); ++i) {
                    NodeID_ sid = sub_ids[i];
                    if (sid < comm_nodes.size()) {
                        reordered[sid] = l2g[i];
                        placed[sid] = true;
                    }
                }
                // Append any unplaced nodes (unmapped by DON-Lite)
                size_t fill = 0;
                for (size_t i = 0; i < comm_nodes.size(); ++i) {
                    if (!placed[i]) {
                        while (fill < comm_nodes.size() && placed[fill]) ++fill;
                        if (fill < comm_nodes.size()) {
                            reordered[fill] = l2g[i];
                            placed[fill] = true;
                        }
                    }
                }
                for (NodeID_ node : reordered)
                    new_ids[node] = current_id++;
            } else {
                for (NodeID_ node : comm_nodes)
                    new_ids[node] = current_id++;
            }
        } else {
            ReorderCommunitySubgraphStandalone<NodeID_, DestID_, WeightT_, invert>(
                g, comm_nodes, comm_node_set, selected, useOutdeg, new_ids, current_id);
        }
        t_cr.Stop();
        t_comm_reorder_total += t_cr.Seconds();
    }
    
    tm.Stop();
    if (!verbose || depth == 0) {
        PrintTime("Adaptive Map Time", tm.Seconds());
    }
    
    // Stage breakdown (always print at depth 0 when verbose)
    if (depth == 0 && verbose) {
        printf("\n=== AdaptiveOrder Stage Breakdown ===\n");
        PrintTime("  Leiden Partitioning", t_leiden.Seconds());
        PrintTime("  Global Features", t_features.Seconds());
        PrintTime("  Small Communities", t_small_total);
        PrintTime("  Comm Features (sum)", t_comm_features_total);
        PrintTime("  Comm Scoring (sum)", t_comm_scoring_total);
        PrintTime("  Comm Reorder (sum)", t_comm_reorder_total);
        double accounted = t_leiden.Seconds() + t_features.Seconds() + t_small_total
                         + t_comm_features_total + t_comm_scoring_total + t_comm_reorder_total;
        PrintTime("  Overhead (unaccounted)", tm.Seconds() - accounted);
        PrintTime("  Total", tm.Seconds());
        printf("Large communities: %zu, Small-group nodes: %zu\n",
               top_communities.size(), small_community_nodes.size());
    }
}

/**
 * @brief Main Adaptive entry point (Standalone)
 * 
 * Parses options and dispatches to appropriate mode.
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateAdaptiveMappingStandalone(
    const CSRGraph<NodeID_, DestID_, invert>& g,
    pvector<NodeID_>& new_ids,
    bool useOutdeg,
    const std::vector<std::string>& reordering_options) {
    
    SelectionModel selection_model = SELECTION_MODEL_PERCEPTRON;
    SelectionCriterion selection_criterion = CRITERION_FASTEST_EXECUTION;
    SelectionMode selection_mode = MODE_FASTEST_EXECUTION;  // kept for legacy callers
    std::string graph_name = "";
    
    if (reordering_options.size() > 3) {
        // Check for legacy mode=100 first
        try {
            int mode_val = std::stoi(reordering_options[3]);
            if (mode_val == 100) {
                GenerateAdaptiveMappingFullGraphStandalone<NodeID_, DestID_, WeightT_, invert>(
                    g, new_ids, useOutdeg, reordering_options);
                return;
            }
        } catch (...) {}
        
        auto mc = ParseModelCriterion(reordering_options[3]);
        selection_model = mc.first;
        selection_criterion = mc.second;
        // Also set legacy mode for any remaining callers
        try {
            int mode_val = std::stoi(reordering_options[3]);
            if (mode_val >= 0 && mode_val <= 6)
                selection_mode = static_cast<SelectionMode>(mode_val);
        } catch (...) {
            selection_mode = GetSelectionMode(reordering_options[3]);
        }
    }
    
    if (reordering_options.size() > 4) {
        graph_name = reordering_options[4];
    }
    
    printf("AdaptiveOrder: model=%s criterion=%s",
           SelectionModelToString(selection_model).c_str(),
           SelectionCriterionToString(selection_criterion).c_str());
    if (!graph_name.empty()) {
        printf(" (graph: %s)", graph_name.c_str());
    }
    printf("\n");
    fflush(stdout);
    
    // Default: full-graph mode. The perceptron selects the best algorithm for
    // the whole graph based on graph features and benchmark type. Per-community
    // reordering (recursive mode) was found to degrade performance because:
    //   1. Leiden decomposition disrupts original memory layout
    //   2. Community-level features differ from training data (whole-graph)
    //   3. Cross-community edge patterns are not captured
    // Full-graph mode achieves 96.3% accuracy on training data.
    GenerateAdaptiveMappingFullGraphStandalone<NodeID_, DestID_, WeightT_, invert>(
        g, new_ids, useOutdeg, reordering_options);
}

#endif // REORDER_ADAPTIVE_H_
