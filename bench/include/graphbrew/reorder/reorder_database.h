// ============================================================================
// reorder_database.h — Database-Driven Algorithm Selection (MODE_DATABASE)
//
// Loads the centralized benchmark database (results/data/benchmarks.json) and
// graph properties (results/data/graph_properties.json) at runtime. Selects
// the best reordering algorithm using:
//   1. Oracle lookup: if the graph name matches a known graph, return the
//      algorithm family with the best (lowest) benchmark time.
//   2. kNN fallback: if the graph is unknown, compute its 12 features,
//      find the k nearest known graphs by Euclidean distance, and vote
//      on the best algorithm family weighted by inverse distance.
//
// This replaces pre-trained models (perceptron, decision tree) with a
// "streaming equation": the database IS the model. When new benchmark
// data is appended, the selection automatically improves.
//
// Usage:
//   ./pr -f graph.sg -a adaptive=mode:database,bench:pr
//
// Files:
//   results/data/benchmarks.json       — append-only benchmark records
//   results/data/graph_properties.json  — graph feature vectors
//
// NOTE: This header is included from within reorder_types.h AFTER
//       CommunityFeatures, BenchmarkType, and PerceptronSelection are
//       defined. It must NOT include reorder_types.h to avoid circular deps.
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <cmath>
#include <cassert>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <mutex>
#include <limits>
#include <cstdio>
#include <sys/file.h>   // flock() for concurrent-write safety
#include <sys/stat.h>   // mkdir() for directory creation
#include <cerrno>

// nlohmann/json for robust JSON parsing of the 140K-line database
#include "../../external/nlohmann_json.hpp"

// Types CommunityFeatures, BenchmarkType, BenchmarkTypeToString() are
// expected to be already defined by reorder_types.h before this point.

namespace graphbrew {
namespace database {

// ============================================================================
// Constants & Runtime-Configurable Paths
// ============================================================================

/// Default directory containing the centralized data files
inline constexpr const char* DEFAULT_DATA_DIR = "results/data/";

/// Number of nearest neighbors for kNN selection
inline constexpr int KNN_K = 5;

/// Number of features used for kNN distance computation
/// (excludes sampled_locality_score which is only available post-ordering)
inline constexpr int N_KNN_FEATURES = 14;

/// Algorithm family names (must match Python ALGO_FAMILY values)
inline const std::vector<std::string> FAMILY_NAMES = {
    "ORIGINAL", "SORT", "RCM", "HUBSORT", "GORDER", "RABBIT", "LEIDEN"
};

/// Runtime-configurable database directory.
/// Call SetDataDir() before BenchmarkDatabase::Get() to override.
inline std::string& GetDataDir() {
    static std::string dir = DEFAULT_DATA_DIR;
    return dir;
}

/// Set the database directory at runtime (from --db-dir / -D flag or env).
/// Ensures the directory exists (creates it if needed).
/// Must be called BEFORE BenchmarkDatabase::Get().
inline void SetDataDir(const std::string& dir) {
    std::string d = dir;
    if (!d.empty() && d.back() != '/') d += '/';
    GetDataDir() = d;
    // Ensure directory exists (mkdir -p equivalent, one level)
    ::mkdir(d.c_str(), 0755);
}

/// Resolve the data directory: --db-dir flag > GRAPHBREW_DB_DIR env > default.
inline void ResolveDataDir(const std::string& cli_db_dir) {
    if (!cli_db_dir.empty()) {
        SetDataDir(cli_db_dir);
        return;
    }
    const char* env = std::getenv("GRAPHBREW_DB_DIR");
    if (env && env[0] != '\0') {
        SetDataDir(env);
        return;
    }
    // Keep default
}

/// Get the benchmarks.json path (runtime-resolved)
inline std::string GetBenchmarksFile() {
    return GetDataDir() + "benchmarks.json";
}

/// Get the graph_properties.json path (runtime-resolved)
inline std::string GetGraphPropsFile() {
    return GetDataDir() + "graph_properties.json";
}

/// Get the adaptive_models.json path (runtime-resolved)
inline std::string GetAdaptiveModelsFile() {
    return GetDataDir() + "adaptive_models.json";
}

/// Check if self-recording is enabled (a data dir has been explicitly set)
inline bool& SelfRecordingEnabled() {
    static bool enabled = false;
    return enabled;
}

/// Enable self-recording (called when --db-dir is provided or env is set)
inline void EnableSelfRecording() {
    SelfRecordingEnabled() = true;
}

/// One-call init: resolve data dir (--db-dir > $GRAPHBREW_DB_DIR > default)
/// and enable self-recording if any explicit source was provided.
/// Call this once in main() after CLI parsing; replaces manual if-blocks.
inline void InitSelfRecording(const std::string& cli_db_dir) {
    ResolveDataDir(cli_db_dir);
    if (!cli_db_dir.empty()) {
        EnableSelfRecording();
        return;
    }
    const char* env = std::getenv("GRAPHBREW_DB_DIR");
    if (env && env[0] != '\0') {
        EnableSelfRecording();
    }
}

// ============================================================================
// File Locking (flock-based, for concurrent write safety)
// ============================================================================

/// RAII file lock guard using flock(). Acquires LOCK_EX on construction,
/// releases on destruction. Used by save methods for concurrent writes.
class FileLockGuard {
public:
    explicit FileLockGuard(const std::string& path)
        : fd_(-1), path_(path + ".lock") {
        fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_EX);
        }
    }
    ~FileLockGuard() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::unlink(path_.c_str());   // Clean up lock file
            ::close(fd_);
        }
    }
    bool locked() const { return fd_ >= 0; }
    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;
private:
    int fd_;
    std::string path_;
};

// ============================================================================
// Data Structures
// ============================================================================

/// Per-algorithm score computed from kNN neighbors (streaming model)
struct AlgoKNNScore {
    std::string family;          ///< Algorithm family name
    double avg_kernel_time = 0;  ///< Weighted-avg kernel time from neighbors
    double avg_reorder_time = 0; ///< Weighted-avg reorder time from neighbors
    double vote_weight = 0;      ///< Total 1/dist weight from neighbors
    int    vote_count = 0;       ///< Number of neighbors that benchmarked this algo
};

/// A single benchmark record from benchmarks.json
struct BenchRecord {
    std::string graph;
    std::string algorithm;
    int algorithm_id = 0;
    std::string benchmark;
    double time_seconds = 0.0;
    double reorder_time = 0.0;
    int trials = 1;
    int nodes = 0;
    int edges = 0;
    bool success = true;
};

// ============================================================================
// Self-recording result structures
// ============================================================================

/// Per-trial detail: timing + benchmark-specific answer data
struct TrialResult {
    int         trial_id = 0;
    double      time_seconds = 0.0;       ///< wall-clock for this trial
    bool        verified = false;          ///< verification pass/fail

    /// Benchmark-specific result data (flexible key-value bag):
    ///  BFS:  {"tree_nodes": N, "tree_edges": E, "source": S}
    ///  CC:   {"num_components": N, "largest_component": S}
    ///  PR:   {"total_error": E, "iterations": I}
    ///  SSSP: {"source": S, "reachable_nodes": R}
    ///  BC:   {"source": S, "max_centrality": C}
    ///  TC:   {"total_triangles": T}
    nlohmann::json answer;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["trial_id"] = trial_id;
        j["time_seconds"] = time_seconds;
        j["verified"] = verified;
        if (!answer.is_null() && !answer.empty()) {
            j["answer"] = answer;
        }
        return j;
    }
};

/// Reorder metadata — stored alongside graph properties for debugging
struct ReorderMeta {
    std::string algorithm;                ///< e.g. "GraphBrewOrder", "LeidenOrder"
    int         algorithm_id = 0;
    double      reorder_time = 0.0;       ///< total wall-clock

    // Leiden/GraphBrew specific
    int         num_passes = 0;           ///< Leiden coarsening passes
    int         num_communities = 0;      ///< final community count
    double      resolution = 0.0;         ///< Leiden resolution parameter
    double      modularity = 0.0;         ///< final community modularity
    std::string final_algo;               ///< per-community algo (e.g. "RabbitOrder")
    int         depth = 0;                ///< recursive depth
    std::string sub_algo;                 ///< sub-community algo

    // Generic reorder meta
    int         bandwidth_before = 0;     ///< graph bandwidth before reorder
    int         bandwidth_after = 0;      ///< graph bandwidth after reorder

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["algorithm"] = algorithm;
        j["algorithm_id"] = algorithm_id;
        j["reorder_time"] = reorder_time;
        if (num_passes > 0) j["num_passes"] = num_passes;
        if (num_communities > 0) j["num_communities"] = num_communities;
        if (resolution > 0) j["resolution"] = resolution;
        if (modularity > 0) j["modularity"] = modularity;
        if (!final_algo.empty()) j["final_algo"] = final_algo;
        if (depth > 0) j["depth"] = depth;
        if (!sub_algo.empty()) j["sub_algo"] = sub_algo;
        if (bandwidth_before > 0) j["bandwidth_before"] = bandwidth_before;
        if (bandwidth_after > 0) j["bandwidth_after"] = bandwidth_after;
        return j;
    }
};

/// Complete benchmark run report (written by BenchmarkKernel)
struct RunReport {
    // Identity
    std::string graph_name;               ///< from filename or --graph-name
    std::string algorithm;                ///< from -o flag (e.g. "GraphBrewOrder")
    int         algorithm_id = 0;         ///< numeric algo ID
    std::string benchmark;                ///< "pr", "bfs", "cc", etc.

    // Aggregate timing
    double      avg_time = 0.0;           ///< average across trials
    double      reorder_time = 0.0;       ///< from builder reorder step
    int         num_trials = 0;

    // Per-trial details
    std::vector<TrialResult> trials;      ///< per-trial timing + answer

    // Reorder metadata (accumulated from builder reorder steps)
    std::vector<ReorderMeta> reorder_metas;  ///< per-reorder-step details

    // Graph dimensions
    int64_t     nodes = 0;
    int64_t     edges = 0;

    // Success
    bool        success = true;
    std::string error;

    /// Convert to JSON matching existing benchmarks.json schema
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["graph"] = graph_name;
        j["algorithm"] = algorithm;
        j["algorithm_id"] = algorithm_id;
        j["benchmark"] = benchmark;
        j["time_seconds"] = avg_time;
        j["reorder_time"] = reorder_time;
        j["trials"] = num_trials;
        j["nodes"] = nodes;
        j["edges"] = edges;
        j["success"] = success;
        j["error"] = error;

        // Per-trial detail (new in v2.1)
        nlohmann::json trial_arr = nlohmann::json::array();
        for (const auto& t : trials) {
            trial_arr.push_back(t.to_json());
        }
        j["trial_details"] = trial_arr;

        // Reorder details (new in v2.2)
        if (!reorder_metas.empty()) {
            nlohmann::json reorder_arr = nlohmann::json::array();
            for (const auto& rm : reorder_metas) {
                reorder_arr.push_back(rm.to_json());
            }
            j["reorder_details"] = reorder_arr;
        }

        // Extra (backward compat placeholder)
        j["extra"] = nlohmann::json::object();
        return j;
    }

    /// Convert from BenchRecord (for backward compat)
    static RunReport from_bench_record(const BenchRecord& r) {
        RunReport rr;
        rr.graph_name = r.graph;
        rr.algorithm = r.algorithm;
        rr.algorithm_id = r.algorithm_id;
        rr.benchmark = r.benchmark;
        rr.avg_time = r.time_seconds;
        rr.reorder_time = r.reorder_time;
        rr.num_trials = r.trials;
        rr.nodes = r.nodes;
        rr.edges = r.edges;
        rr.success = r.success;
        return rr;
    }

    /// Convert to BenchRecord (for internal dedup/oracle)
    BenchRecord to_bench_record() const {
        BenchRecord r;
        r.graph = graph_name;
        r.algorithm = algorithm;
        r.algorithm_id = algorithm_id;
        r.benchmark = benchmark;
        r.time_seconds = avg_time;
        r.reorder_time = reorder_time;
        r.trials = num_trials;
        r.nodes = static_cast<int>(nodes);
        r.edges = static_cast<int>(edges);
        r.success = success;
        return r;
    }
};

/// Graph-level properties for graph_properties.json (raw values, not transformed)
struct GraphProperties {
    std::string graph_name;
    int64_t     nodes = 0;
    int64_t     edges = 0;
    double      modularity = 0.0;
    double      degree_variance = 0.0;
    double      hub_concentration = 0.0;
    double      clustering_coeff = 0.0;
    double      avg_degree = 0.0;
    double      avg_path_length = 0.0;
    double      diameter_estimate = 0.0;
    double      community_count = 0.0;
    double      packing_factor = 0.0;
    double      packing_factor_cl = 0.0;   ///< Cache-line packing factor (IISWC'18 faithful)
    double      forward_edge_fraction = 0.0;
    double      working_set_ratio = 0.0;
    double      wsr_l1 = 0.0;              ///< Per-level L1 WSR: graph_bytes / L1_size
    double      wsr_l2 = 0.0;              ///< Per-level L2 WSR: graph_bytes / L2_size
    double      density = 0.0;
    // DON-RL features
    double      vertex_significance_skewness = 0.0;
    double      window_neighbor_overlap = 0.0;
    // P1 3.1d: Sampled locality score
    double      sampled_locality_score = 0.0;
    std::string graph_type;               ///< "SOCIAL", "ROAD", etc.

    // Reorder history (one per algorithm applied to this graph)
    std::vector<ReorderMeta> reorder_history;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["nodes"] = nodes;
        j["edges"] = edges;
        j["modularity"] = modularity;
        j["degree_variance"] = degree_variance;
        j["hub_concentration"] = hub_concentration;
        j["clustering_coefficient"] = clustering_coeff;
        j["avg_degree"] = avg_degree;
        j["avg_path_length"] = avg_path_length;
        j["diameter"] = diameter_estimate;
        j["community_count"] = community_count;
        j["packing_factor"] = packing_factor;
        j["packing_factor_cl"] = packing_factor_cl;
        j["forward_edge_fraction"] = forward_edge_fraction;
        j["working_set_ratio"] = working_set_ratio;
        j["wsr_l1"] = wsr_l1;
        j["wsr_l2"] = wsr_l2;
        j["density"] = density;
        j["vertex_significance_skewness"] = vertex_significance_skewness;
        j["window_neighbor_overlap"] = window_neighbor_overlap;
        j["sampled_locality_score"] = sampled_locality_score;
        if (!graph_type.empty()) j["graph_type"] = graph_type;

        if (!reorder_history.empty()) {
            nlohmann::json hist = nlohmann::json::array();
            for (const auto& rm : reorder_history) {
                hist.push_back(rm.to_json());
            }
            j["reorder_history"] = hist;
        }
        return j;
    }
};

// ============================================================================
// Global Hints for Reorder State Passing
// ============================================================================

/// Global hint: reorder time (set in GenerateMapping, read in BenchmarkKernel)
inline double& GetReorderTimeHint() {
    static double t = 0.0;
    return t;
}
inline void SetReorderTimeHint(double t) { GetReorderTimeHint() = t; }

/// Global hint: algorithm name chain (set in MakeGraph, read in BenchmarkKernel)
inline std::string& GetReorderAlgoHint() {
    static std::string name;
    return name;
}
inline void SetReorderAlgoHint(const std::string& name) { GetReorderAlgoHint() = name; }

/// Global hint: algorithm ID (set in MakeGraph, read in BenchmarkKernel)
inline int& GetReorderAlgoIdHint() {
    static int id = 0;
    return id;
}
inline void SetReorderAlgoIdHint(int id) { GetReorderAlgoIdHint() = id; }

/// Global hint: accumulated ReorderMeta from builder (read in BenchmarkKernel)
inline std::vector<ReorderMeta>& GetReorderMetaHints() {
    static std::vector<ReorderMeta> metas;
    return metas;
}
inline void AppendReorderMetaHint(const ReorderMeta& meta) {
    GetReorderMetaHints().push_back(meta);
}
inline void ClearReorderMetaHints() {
    GetReorderMetaHints().clear();
}

/// Global staging area: algorithm-specific ReorderMeta details.
/// Reorder algorithms (GraphBrew, Rabbit, Leiden, Adaptive) populate this
/// with rich metadata (num_communities, modularity, resolution, etc.).
/// GenerateMapping() reads it to enrich the ReorderMeta hint.
inline ReorderMeta& GetStagedReorderMeta() {
    static ReorderMeta staged;
    return staged;
}
inline void ClearStagedReorderMeta() {
    GetStagedReorderMeta() = ReorderMeta();
}

/// Global per-iteration benchmark log.
/// Kernel functions (DOBFS, PageRankPullGS, DeltaStep, etc.) append
/// per-step entries here.  BenchmarkKernel clears before each trial
/// and reads after the kernel returns.
inline std::vector<nlohmann::json>& GetBenchmarkIterationLog() {
    static std::vector<nlohmann::json> log;
    return log;
}
inline void AppendBenchmarkIterationEntry(nlohmann::json entry) {
    GetBenchmarkIterationLog().push_back(std::move(entry));
}
inline void ClearBenchmarkIterationLog() {
    GetBenchmarkIterationLog().clear();
}

/// Feature vector for a graph (14 features, same order as Python)
struct GraphFeatureVec {
    double modularity = 0.0;
    double hub_concentration = 0.0;
    double log_nodes = 0.0;
    double log_edges = 0.0;
    double density = 0.0;
    double avg_degree_100 = 0.0;      // avg_degree / 100
    double clustering_coeff = 0.0;
    double packing_factor = 0.0;
    double forward_edge_fraction = 0.0;
    double log2_wsr = 0.0;            // log2(working_set_ratio + 1)
    double log10_cc = 0.0;            // log10(community_count + 1)
    double diameter_50 = 0.0;         // diameter / 50
    // DON-RL features (Zhao et al.)
    double vertex_significance_skewness = 0.0;  // CV of per-vertex locality
    double window_neighbor_overlap = 0.0;       // mean neighbor-in-window fraction
    // P1 3.1d: Sampled locality score (F(σ) approximation)
    double sampled_locality_score = 0.0;        // cache-line-window locality

    static constexpr int N_FEATURES = 15;
    double& operator[](int i) {
        assert(i >= 0 && i < N_FEATURES && "GraphFeatureVec index out of bounds");
        return (&modularity)[i];
    }
    double operator[](int i) const {
        assert(i >= 0 && i < N_FEATURES && "GraphFeatureVec index out of bounds");
        return (&modularity)[i];
    }
};

/// Algorithm-to-family mapping (same as Python ALGO_FAMILY)
inline std::string AlgoToFamily(const std::string& algo) {
    // Basic
    if (algo == "ORIGINAL") return "ORIGINAL";
    if (algo == "RANDOM" || algo == "SORT") return "SORT";
    // RCM
    if (algo.find("RCM") != std::string::npos) return "RCM";
    // HubSort
    if (algo.find("HUB") != std::string::npos || algo == "DBG") return "HUBSORT";
    // Gorder
    if (algo.find("GORDER") != std::string::npos || algo == "CORDER") return "GORDER";
    // Rabbit
    if (algo.find("RABBIT") != std::string::npos) return "RABBIT";
    // Leiden / GraphBrew
    if (algo.find("Leiden") != std::string::npos || algo.find("GraphBrew") != std::string::npos) return "LEIDEN";
    // GoGraph
    if (algo.find("GoGraph") != std::string::npos || algo.find("GOGRAPH") != std::string::npos) return "GOGRAPH";
    // Compound: check suffix
    if (algo.find("+") != std::string::npos) {
        // Use the second component as the family
        auto pos = algo.find("+");
        return AlgoToFamily(algo.substr(pos + 1));
    }
    return "ORIGINAL";
}

// ============================================================================
// BenchmarkDatabase — Singleton
// ============================================================================

/**
 * @brief Centralized benchmark database loaded from JSON.
 *
 * Thread-safe singleton. Loads once on first access, caches in memory.
 * Provides:
 *   - Oracle lookup: best_family(graph_name, benchmark)
 *   - kNN selection: best_family(features, benchmark, k)
 *   - Append: add new records and save back to disk
 */
class BenchmarkDatabase {
public:
    /// Get the singleton instance (loads from disk on first call)
    static BenchmarkDatabase& Get() {
        static BenchmarkDatabase instance;
        return instance;
    }

    /// Check if the database was successfully loaded
    bool loaded() const { return loaded_; }

    /// Number of benchmark records
    size_t num_records() const { return records_.size(); }

    /// Number of known graphs
    size_t num_graphs() const { return graph_features_.size(); }

    // ========================================================================
    // Oracle Lookup (known graph)
    // ========================================================================

    /**
     * @brief Get the best algorithm family for a known graph + benchmark.
     *
     * Returns the family that achieved the lowest time_seconds in the
     * database. Returns empty string if the graph is not in the database.
     */
    std::string best_family_oracle(const std::string& graph_name,
                                    const std::string& benchmark) const {
        auto key = graph_name + "|" + benchmark;
        auto it = oracle_cache_.find(key);
        if (it != oracle_cache_.end()) {
            return it->second;
        }
        return "";
    }

    /**
     * @brief Check if a graph name is known in the database.
     */
    bool has_graph(const std::string& graph_name) const {
        return graph_features_.count(graph_name) > 0;
    }

    // ========================================================================
    // kNN Selection (unknown graph)
    // ========================================================================

    /**
     * @brief Select the best algorithm family using k-nearest neighbors.
     *
     * Computes Euclidean distance between the query feature vector and all
     * known graphs. The k closest graphs vote for the best family, where
     * each vote is weighted by 1/distance. For each voter, the vote goes
     * to the family that performed best on that graph for the given benchmark.
     *
     * @param feat CommunityFeatures of the query graph
     * @param benchmark Benchmark name (e.g., "pr", "bfs")
     * @param k Number of nearest neighbors (default: KNN_K)
     * @param verbose Print kNN details
     * @return Best algorithm family name
     */
    std::string best_family_knn(const CommunityFeatures& feat,
                                 const std::string& benchmark,
                                 int k = KNN_K,
                                 bool verbose = false) const {
        if (graph_features_.empty()) return "ORIGINAL";

        // Extract query features (same transform as ModelTree::extract_features)
        GraphFeatureVec query;
        query.modularity = feat.modularity;
        query.hub_concentration = feat.hub_concentration;
        query.log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        query.log_edges = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        query.density = feat.internal_density;
        query.avg_degree_100 = feat.avg_degree / 100.0;
        query.clustering_coeff = feat.clustering_coeff;
        query.packing_factor = feat.packing_factor;
        query.forward_edge_fraction = feat.forward_edge_fraction;
        query.log2_wsr = std::log2(feat.working_set_ratio + 1.0);
        query.log10_cc = std::log10(feat.community_count + 1.0);
        query.diameter_50 = feat.diameter_estimate / 50.0;
        query.vertex_significance_skewness = feat.vertex_significance_skewness;
        query.window_neighbor_overlap = feat.window_neighbor_overlap;
        query.sampled_locality_score = feat.sampled_locality_score;

        return best_family_knn_from_features(query, benchmark, k, verbose);
    }

    /**
     * @brief kNN selection from a pre-extracted feature vector.
     */
    std::string best_family_knn_from_features(const GraphFeatureVec& query,
                                               const std::string& benchmark,
                                               int k = KNN_K,
                                               bool verbose = false) const {
        // Compute distances to all known graphs
        struct Neighbor {
            std::string name;
            double distance;
        };
        std::vector<Neighbor> neighbors;
        neighbors.reserve(graph_features_.size());

        for (const auto& [name, fv] : graph_features_) {
            double dist = euclidean_distance(query, fv);
            neighbors.push_back({name, dist});
        }

        // Sort by distance
        std::sort(neighbors.begin(), neighbors.end(),
                  [](const Neighbor& a, const Neighbor& b) {
                      return a.distance < b.distance;
                  });

        // Take top-k
        int actual_k = std::min(k, static_cast<int>(neighbors.size()));

        // Weighted voting: each neighbor votes for the family that performed
        // best on it for this benchmark, weighted by 1/(distance + eps)
        std::map<std::string, double> family_votes;
        const double eps = 1e-8;

        if (verbose) {
            std::cout << "  kNN neighbors (k=" << actual_k << ", bench=" << benchmark << "):\n";
        }

        for (int i = 0; i < actual_k; ++i) {
            const auto& nb = neighbors[i];
            std::string nb_best = best_family_oracle(nb.name, benchmark);
            if (nb_best.empty()) {
                // Try "generic" benchmark
                nb_best = best_family_oracle(nb.name, "generic");
            }
            if (nb_best.empty()) nb_best = "ORIGINAL";

            double weight = 1.0 / (nb.distance + eps);
            family_votes[nb_best] += weight;

            if (verbose) {
                std::cout << "    " << (i+1) << ". " << nb.name
                          << " (dist=" << nb.distance
                          << ") → " << nb_best
                          << " (weight=" << weight << ")\n";
            }
        }

        // Find family with highest total vote weight
        std::string best = "ORIGINAL";
        double best_weight = -1.0;
        for (const auto& [fam, w] : family_votes) {
            if (w > best_weight) {
                best_weight = w;
                best = fam;
            }
        }

        if (verbose) {
            std::cout << "  kNN result: " << best << " (total_weight=" << best_weight << ")\n";
        }

        return best;
    }

    // ========================================================================
    // Unified Selection (oracle if known, kNN if unknown)
    // ========================================================================

    /**
     * @brief Select the best algorithm family for a graph.
     *
     * If graph_name is known in the database → oracle lookup.
     * Otherwise → kNN on the 12-feature vector.
     *
     * @param feat CommunityFeatures of the graph
     * @param graph_name Name of the graph (for oracle lookup)
     * @param benchmark Benchmark name (e.g., "pr")
     * @param verbose Print selection details
     * @return Best algorithm family name
     */
    std::string select(const CommunityFeatures& feat,
                       const std::string& graph_name,
                       const std::string& benchmark,
                       bool verbose = false) const {
        // Try oracle first (direct lookup from the database)
        if (!graph_name.empty() && has_graph(graph_name)) {
            std::string family = best_family_oracle(graph_name, benchmark);
            if (!family.empty()) {
                if (verbose) {
                    std::cout << "  Database: oracle lookup → " << family
                              << " (known graph: " << graph_name << ")\n";
                }
                return family;
            }
        }

        // Fall back to kNN
        if (verbose) {
            std::cout << "  Database: kNN lookup (unknown graph: "
                      << (graph_name.empty() ? "<unnamed>" : graph_name) << ")\n";
        }
        return best_family_knn(feat, benchmark, KNN_K, verbose);
    }

    // ========================================================================
    // Streaming kNN Algorithm Scoring (all modes from raw data)
    // ========================================================================

    /**
     * @brief Compute per-algorithm-family scores from k nearest neighbors.
     *
     * For each of the k nearest graphs (by 12D feature distance), looks up
     * all benchmark records for each algorithm family and computes the
     * weighted-average kernel time and reorder time (weighted by 1/distance).
     *
     * This is the core of the streaming model: the database IS the model.
     * No pre-trained weights needed — predictions come directly from the
     * raw benchmark data.
     *
     * @param query  Transformed feature vector of the query graph
     * @param benchmark  Benchmark name (e.g., "pr", "bfs")
     * @param k  Number of nearest neighbors
     * @param verbose  Print details
     * @return  Vector of per-family scores, sorted by avg_kernel_time ascending
     */
    std::vector<AlgoKNNScore> knn_algo_scores(
        const GraphFeatureVec& query,
        const std::string& benchmark,
        int k = KNN_K,
        bool verbose = false) const {

        std::vector<AlgoKNNScore> result;
        if (graph_features_.empty() || records_.empty()) return result;

        // Step 1: Find k nearest neighbors by feature distance
        struct Neighbor { std::string name; double distance; };
        std::vector<Neighbor> neighbors;
        neighbors.reserve(graph_features_.size());

        for (const auto& [name, fv] : graph_features_) {
            double dist = euclidean_distance(query, fv);
            neighbors.push_back({name, dist});
        }
        std::sort(neighbors.begin(), neighbors.end(),
                  [](const Neighbor& a, const Neighbor& b) {
                      return a.distance < b.distance;
                  });
        int actual_k = std::min(k, static_cast<int>(neighbors.size()));

        // Step 2: For each family, accumulate weighted kernel_time and reorder_time
        // from the k nearest neighbors
        struct FamilyAccum {
            double weighted_kernel_sum = 0;
            double weighted_reorder_sum = 0;
            double weight_sum = 0;
            int count = 0;
        };
        std::map<std::string, FamilyAccum> accum;
        const double eps = 1e-8;

        for (int i = 0; i < actual_k; ++i) {
            const auto& nb = neighbors[i];
            double w = 1.0 / (nb.distance + eps);

            // Find all records for this neighbor graph + benchmark
            for (const auto& r : records_) {
                if (r.graph != nb.name) continue;
                if (r.benchmark != benchmark && benchmark != "generic") continue;

                std::string fam = AlgoToFamily(r.algorithm);
                auto& a = accum[fam];
                a.weighted_kernel_sum += w * r.time_seconds;
                a.weighted_reorder_sum += w * r.reorder_time;
                a.weight_sum += w;
                a.count++;
            }
        }

        // Step 3: Convert accumulated data to AlgoKNNScore
        for (const auto& [fam, a] : accum) {
            if (a.weight_sum <= 0) continue;
            AlgoKNNScore score;
            score.family = fam;
            score.avg_kernel_time = a.weighted_kernel_sum / a.weight_sum;
            score.avg_reorder_time = a.weighted_reorder_sum / a.weight_sum;
            score.vote_weight = a.weight_sum;
            score.vote_count = a.count;
            result.push_back(score);
        }

        // Sort by avg_kernel_time ascending (fastest first)
        std::sort(result.begin(), result.end(),
                  [](const AlgoKNNScore& a, const AlgoKNNScore& b) {
                      return a.avg_kernel_time < b.avg_kernel_time;
                  });

        if (verbose && !result.empty()) {
            std::cout << "  kNN algo scores (" << actual_k << " neighbors, bench=" << benchmark << "):\n";
            for (const auto& s : result) {
                std::cout << "    " << s.family
                          << ": kernel=" << s.avg_kernel_time
                          << "s, reorder=" << s.avg_reorder_time
                          << "s, votes=" << s.vote_count << "\n";
            }
        }

        return result;
    }

    /**
     * @brief Mode-aware streaming selection from the database.
     *
     * Uses the raw benchmark data directly (no pre-trained weights) to
     * select the best algorithm for any SelectionMode. This is the unified
     * entry point that replaces per-mode logic with database-driven scoring.
     *
     * @param feat  CommunityFeatures of the query graph
     * @param graph_name  Graph name (for oracle lookup)
     * @param benchmark  Benchmark name string
     * @param mode  SelectionMode enum value
     * @param verbose  Print selection details
     * @return Best algorithm family name, or "" if no data
     */
    std::string select_for_mode(const CommunityFeatures& feat,
                                const std::string& graph_name,
                                const std::string& benchmark,
                                SelectionMode mode,
                                bool verbose = false) const {
        if (!loaded_ || records_.empty()) return "";

        // Oracle shortcut for known graphs (all modes benefit from direct lookup)
        if (!graph_name.empty() && has_graph(graph_name)) {
            std::string oracle = best_family_oracle(graph_name, benchmark);
            if (!oracle.empty()) {
                if (verbose) {
                    std::cout << "  Database streaming: oracle → " << oracle
                              << " (known: " << graph_name << ")\n";
                }
                // For MODE_FASTEST_REORDER, oracle may not be the right answer —
                // the fastest kernel algo isn't necessarily the fastest to reorder.
                // But for most modes, oracle is optimal.
                if (mode != MODE_FASTEST_REORDER) {
                    return oracle;
                }
                // For fastest reorder, we still use kNN scoring below
            }
        }

        // Extract feature vector for kNN
        GraphFeatureVec query;
        query.modularity = feat.modularity;
        query.hub_concentration = feat.hub_concentration;
        query.log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        query.log_edges = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        query.density = feat.internal_density;
        query.avg_degree_100 = feat.avg_degree / 100.0;
        query.clustering_coeff = feat.clustering_coeff;
        query.packing_factor = feat.packing_factor;
        query.forward_edge_fraction = feat.forward_edge_fraction;
        query.log2_wsr = std::log2(feat.working_set_ratio + 1.0);
        query.log10_cc = std::log10(feat.community_count + 1.0);
        query.diameter_50 = feat.diameter_estimate / 50.0;
        query.vertex_significance_skewness = feat.vertex_significance_skewness;
        query.window_neighbor_overlap = feat.window_neighbor_overlap;
        query.sampled_locality_score = feat.sampled_locality_score;

        auto scores = knn_algo_scores(query, benchmark, KNN_K, verbose);
        if (scores.empty()) return "";

        std::string best;

        switch (mode) {
            case MODE_FASTEST_REORDER: {
                // Pick algorithm with lowest average reorder time
                double best_rt = std::numeric_limits<double>::infinity();
                for (const auto& s : scores) {
                    if (s.avg_reorder_time < best_rt) {
                        best_rt = s.avg_reorder_time;
                        best = s.family;
                    }
                }
                if (verbose) {
                    std::cout << "  Streaming fastest-reorder → " << best
                              << " (reorder=" << best_rt << "s)\n";
                }
                break;
            }

            case MODE_FASTEST_EXECUTION:
            case MODE_DATABASE: {
                // Pick family with lowest average kernel time
                // (scores are pre-sorted by kernel time ascending)
                best = scores.front().family;
                if (verbose) {
                    std::cout << "  Streaming fastest-exec → " << best
                              << " (kernel=" << scores.front().avg_kernel_time << "s)\n";
                }
                break;
            }

            case MODE_BEST_ENDTOEND: {
                // Pick family with lowest (kernel_time + reorder_time)
                double best_total = std::numeric_limits<double>::infinity();
                for (const auto& s : scores) {
                    double total = s.avg_kernel_time + s.avg_reorder_time;
                    if (total < best_total) {
                        best_total = total;
                        best = s.family;
                    }
                }
                if (verbose) {
                    std::cout << "  Streaming end-to-end → " << best
                              << " (total=" << best_total << "s)\n";
                }
                break;
            }

            case MODE_BEST_AMORTIZATION: {
                // Pick family that amortizes fastest:
                //   iterations = reorder_time / time_saved_per_iter
                //   time_saved = (original_time - algo_time)
                // ORIGINAL kernel time as baseline
                double orig_time = 0;
                for (const auto& s : scores) {
                    if (s.family == "ORIGINAL") {
                        orig_time = s.avg_kernel_time;
                        break;
                    }
                }
                if (orig_time <= 0) {
                    // No ORIGINAL baseline — use max kernel time as proxy
                    for (const auto& s : scores) {
                        orig_time = std::max(orig_time, s.avg_kernel_time);
                    }
                }

                double best_iters = std::numeric_limits<double>::infinity();
                for (const auto& s : scores) {
                    if (s.family == "ORIGINAL") continue;
                    double saved = orig_time - s.avg_kernel_time;
                    if (saved <= 0) continue;  // No speedup → skip
                    double iters = s.avg_reorder_time / saved;
                    if (iters < best_iters) {
                        best_iters = iters;
                        best = s.family;
                    }
                }
                if (best.empty()) best = "ORIGINAL";
                if (verbose) {
                    std::cout << "  Streaming amortization → " << best
                              << " (amortizes in " << best_iters << " iters)\n";
                }
                break;
            }

            case MODE_DECISION_TREE:
            case MODE_HYBRID:
            default: {
                // For DT/hybrid modes: use fastest-execution from database
                // (the DT/hybrid model tree path is handled as fallback in
                //  SelectReorderingWithMode if this returns non-empty)
                best = scores.front().family;
                if (verbose) {
                    std::cout << "  Streaming (DT/hybrid fallback) → " << best
                              << " (kernel=" << scores.front().avg_kernel_time << "s)\n";
                }
                break;
            }
        }

        return best;
    }

    // ========================================================================
    // Append New Records
    // ========================================================================

    /**
     * @brief Append a new benchmark record to the database and save to disk.
     *
     * Deduplicates by (graph, algorithm, benchmark) key, keeping the
     * record with the lowest time_seconds.
     *
     * Thread-safe.
     */
    void append(const BenchRecord& rec) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = rec.graph + "|" + rec.algorithm + "|" + rec.benchmark;

        auto it = dedup_.find(key);
        if (it != dedup_.end()) {
            // Keep the one with lower time
            auto& existing = records_[it->second];
            if (rec.time_seconds < existing.time_seconds) {
                existing = rec;
                rebuild_oracle_entry(rec);
            }
        } else {
            dedup_[key] = records_.size();
            records_.push_back(rec);
            rebuild_oracle_entry(rec);
        }
    }

    /**
     * @brief Append a new benchmark record and update graph properties.
     *
     * Also updates the graph feature vector from CommunityFeatures.
     * Use this when the C++ runtime has just benchmarked a new graph.
     */
    void append_with_features(const BenchRecord& rec,
                               const CommunityFeatures& feat) {
        // Single lock for both append and feature update (avoid TOCTOU race)
        std::lock_guard<std::mutex> lock(mutex_);
        // --- inline append logic under the same lock ---
        {
            auto key = rec.graph + "|" + rec.algorithm + "|" + rec.benchmark;
            auto it = dedup_.find(key);
            if (it != dedup_.end()) {
                auto& existing = records_[it->second];
                if (rec.time_seconds < existing.time_seconds) {
                    existing = rec;
                    rebuild_oracle_entry(rec);
                }
            } else {
                dedup_[key] = records_.size();
                records_.push_back(rec);
                rebuild_oracle_entry(rec);
            }
        }

        // Update graph properties
        auto& fv = graph_features_[rec.graph];
        fv.modularity = feat.modularity;
        fv.hub_concentration = feat.hub_concentration;
        fv.log_nodes = std::log10(static_cast<double>(feat.num_nodes) + 1.0);
        fv.log_edges = std::log10(static_cast<double>(feat.num_edges) + 1.0);
        fv.density = feat.internal_density;
        fv.avg_degree_100 = feat.avg_degree / 100.0;
        fv.clustering_coeff = feat.clustering_coeff;
        fv.packing_factor = feat.packing_factor;
        fv.forward_edge_fraction = feat.forward_edge_fraction;
        fv.log2_wsr = std::log2(feat.working_set_ratio + 1.0);
        fv.log10_cc = std::log10(feat.community_count + 1.0);
        fv.diameter_50 = feat.diameter_estimate / 50.0;
        fv.vertex_significance_skewness = feat.vertex_significance_skewness;
        fv.window_neighbor_overlap = feat.window_neighbor_overlap;
        fv.sampled_locality_score = feat.sampled_locality_score;
    }

    /**
     * @brief Save the current database to disk (atomic write).
     *
     * Writes to benchmarks.json and graph_properties.json.
     */
    bool save() {
        std::lock_guard<std::mutex> lock(mutex_);
        return save_benchmarks() && save_graph_props();
    }

    // ========================================================================
    // Self-Recording API (v2.1)
    // ========================================================================

    /**
     * @brief Append a RunReport to the database and save to disk.
     *
     * Flock-safe: acquires file lock, re-reads latest data from disk,
     * merges in-memory, writes back. This ensures multiple concurrent
     * benchmark binaries can safely append.
     */
    void append_run(const RunReport& report) {
        if (!SelfRecordingEnabled()) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // File-level lock for concurrent process safety
        FileLockGuard flock(GetBenchmarksFile());

        // Re-read from disk to get latest (another process may have written)
        reload_benchmarks_unlocked();

        // Insert into in-memory store
        BenchRecord rec = report.to_bench_record();
        auto key = rec.graph + "|" + rec.algorithm + "|" + rec.benchmark;
        auto it = dedup_.find(key);
        if (it != dedup_.end()) {
            auto& existing = records_[it->second];
            if (rec.time_seconds < existing.time_seconds) {
                existing = rec;
                rebuild_oracle_entry(rec);
            }
        } else {
            dedup_[key] = records_.size();
            records_.push_back(rec);
            rebuild_oracle_entry(rec);
        }
        loaded_ = true;

        // Save with full RunReport JSON (includes trial_details)
        save_benchmarks_with_run(report);
    }

    /**
     * @brief Update graph properties from a GraphProperties struct.
     *
     * Flock-safe: acquires file lock, re-reads latest, merges, writes.
     */
    void update_graph_props(const GraphProperties& props) {
        if (!SelfRecordingEnabled()) return;

        std::lock_guard<std::mutex> lock(mutex_);

        FileLockGuard flock(GetGraphPropsFile());

        // Update internal feature vector
        auto& fv = graph_features_[props.graph_name];
        fv.modularity = props.modularity;
        fv.hub_concentration = props.hub_concentration;
        fv.log_nodes = std::log10(static_cast<double>(props.nodes) + 1.0);
        fv.log_edges = std::log10(static_cast<double>(props.edges) + 1.0);
        fv.density = props.density;
        fv.avg_degree_100 = props.avg_degree / 100.0;
        fv.clustering_coeff = props.clustering_coeff;
        fv.packing_factor = props.packing_factor;
        fv.forward_edge_fraction = props.forward_edge_fraction;
        fv.log2_wsr = std::log2(props.working_set_ratio + 1.0);
        fv.log10_cc = std::log10(props.community_count + 1.0);
        fv.diameter_50 = props.diameter_estimate / 50.0;
        fv.vertex_significance_skewness = props.vertex_significance_skewness;
        fv.window_neighbor_overlap = props.window_neighbor_overlap;
        fv.sampled_locality_score = props.sampled_locality_score;

        // Save: re-read from disk, merge, write
        save_graph_props_with_raw(props);
    }

    /**
     * @brief Force reload from disk.
     */
    void reload() {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
        dedup_.clear();
        oracle_cache_.clear();
        graph_features_.clear();
        perceptron_weights_.clear();
        model_trees_.clear();
        loaded_ = false;
        models_loaded_ = false;
        models_from_unified_ = false;
        load();
    }

    // ========================================================================
    // Unified Model Access (perceptron, DT, hybrid)
    // ========================================================================

    /**
     * @brief Check if the unified model file was loaded.
     */
    bool models_loaded() const { return models_loaded_; }

    /**
     * @brief Get perceptron weights for a specific benchmark.
     *
     * Tries per-benchmark weights first, falls back to averaged weights.
     * Returns nullptr if no weights are available.
     *
     * @param bench Benchmark name ("pr", "bfs", etc.), or "" for averaged
     * @return Pointer to JSON object, or nullptr if not found
     */
    const nlohmann::json* get_perceptron_weights(const std::string& bench = "") const {
        if (!models_loaded_) return nullptr;

        // Try per-benchmark weights first
        if (!bench.empty() && !perceptron_per_bench_.empty()) {
            auto it = perceptron_per_bench_.find(bench);
            if (it != perceptron_per_bench_.end()) {
                return &(it->second);
            }
        }

        // Fall back to averaged weights
        if (!perceptron_weights_.is_null() && !perceptron_weights_.empty()) {
            return &perceptron_weights_;
        }

        return nullptr;
    }

    /**
     * @brief Get a model tree (DT or hybrid) for a specific benchmark.
     *
     * @param bench  Benchmark name ("pr", "bfs", etc.)
     * @param subdir "decision_tree" or "hybrid"
     * @return Parsed ModelTree, or empty (loaded=false) if not found
     */
    ModelTree get_model_tree(const std::string& bench,
                             const std::string& subdir) const {
        ModelTree mt;
        auto section_it = model_trees_.find(subdir);
        if (section_it == model_trees_.end()) return mt;

        auto bench_it = section_it->second.find(bench);
        if (bench_it == section_it->second.end()) return mt;

        return bench_it->second;
    }

    /**
     * @brief Parse perceptron weights from JSON into PerceptronWeights map.
     *
     * Delegates to the global ParseWeightsFromJSON function in reorder_types.h.
     */
    bool parse_perceptron_weights(const std::string& bench,
                                   std::map<std::string, PerceptronWeights>& out) const {
        const nlohmann::json* jptr = get_perceptron_weights(bench);
        if (!jptr) return false;

        // Convert nlohmann JSON to string for existing parser
        std::string json_str = jptr->dump();
        return ParseWeightsFromJSON(json_str, out);
    }

    /**
     * @brief Print database statistics.
     */
    void print_stats() const {
        std::cout << "BenchmarkDatabase: "
                  << records_.size() << " records, "
                  << graph_features_.size() << " graphs";

        // Count unique benchmarks
        std::set<std::string> benchmarks;
        for (const auto& r : records_) benchmarks.insert(r.benchmark);
        std::cout << ", " << benchmarks.size() << " benchmarks";

        // Count unique algorithms
        std::set<std::string> algos;
        for (const auto& r : records_) algos.insert(r.algorithm);
        std::cout << ", " << algos.size() << " algorithms\n";
    }

private:
    BenchmarkDatabase() { load(); }
    BenchmarkDatabase(const BenchmarkDatabase&) = delete;
    BenchmarkDatabase& operator=(const BenchmarkDatabase&) = delete;

    // ========================================================================
    // Loading
    // ========================================================================

    void load() {
        load_benchmarks();
        load_graph_props();
        compute_feature_stats();  // z-norm stats for kNN
        build_oracle_cache();

        // Train models from raw DB data (replaces load_adaptive_models).
        // Falls back to adaptive_models.json if training data is insufficient.
        if (raw_graph_props_.size() >= 3 && !oracle_cache_.empty()) {
            train_all_models();
        } else {
            load_adaptive_models();
        }

        loaded_ = !records_.empty();

        if (loaded_) {
            std::cout << "[DATABASE] Loaded " << records_.size()
                      << " records, " << graph_features_.size()
                      << " graph feature vectors";
            if (models_loaded_) {
                std::cout << (models_from_unified_
                    ? ", unified models (from file)"
                    : ", models (trained from DB)");
            }
            std::cout << "\n";
        }
    }

    void load_benchmarks() {
        std::string path = GetBenchmarksFile();
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            std::cerr << "[DATABASE] Warning: cannot open " << path << "\n";
            return;
        }

        try {
            nlohmann::json j;
            ifs >> j;

            if (!j.is_array()) {
                std::cerr << "[DATABASE] Error: benchmarks.json is not an array\n";
                return;
            }

            records_.reserve(j.size());
            for (const auto& entry : j) {
                BenchRecord rec;
                rec.graph = entry.value("graph", "");
                rec.algorithm = entry.value("algorithm", "");
                rec.algorithm_id = entry.value("algorithm_id", 0);
                rec.benchmark = entry.value("benchmark", "");
                rec.time_seconds = entry.value("time_seconds", 0.0);
                rec.reorder_time = entry.value("reorder_time", 0.0);
                rec.trials = entry.value("trials", 1);
                rec.nodes = entry.value("nodes", 0);
                rec.edges = entry.value("edges", 0);
                rec.success = entry.value("success", true);

                if (!rec.success || rec.graph.empty() || rec.algorithm.empty())
                    continue;

                // Dedup by (graph, algorithm, benchmark), keep lowest time
                auto key = rec.graph + "|" + rec.algorithm + "|" + rec.benchmark;
                auto it = dedup_.find(key);
                if (it != dedup_.end()) {
                    if (rec.time_seconds < records_[it->second].time_seconds) {
                        records_[it->second] = rec;
                    }
                } else {
                    dedup_[key] = records_.size();
                    records_.push_back(rec);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[DATABASE] Error parsing " << path
                      << ": " << e.what() << "\n";
        }
    }

    /// Re-read benchmarks from disk without clearing other state (for append_run)
    void reload_benchmarks_unlocked() {
        records_.clear();
        dedup_.clear();
        oracle_cache_.clear();
        load_benchmarks();
        build_oracle_cache();
    }

    void load_graph_props() {
        std::string path = GetGraphPropsFile();
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            std::cerr << "[DATABASE] Warning: cannot open " << path << "\n";
            return;
        }

        try {
            nlohmann::json j;
            ifs >> j;

            if (!j.is_object()) {
                std::cerr << "[DATABASE] Error: graph_properties.json is not an object\n";
                return;
            }

            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string& name = it.key();
                const auto& props = it.value();

                GraphFeatureVec fv;
                double raw_nodes = props.value("nodes", 0.0);
                double raw_edges = props.value("edges", 0.0);
                double raw_avg_degree = props.value("avg_degree", 0.0);
                double raw_wsr = props.value("working_set_ratio", 0.0);
                double raw_cc = props.value("community_count", 0.0);
                double raw_diameter = props.value("diameter", 0.0);

                // Apply the same transforms as ModelTree::extract_features
                fv.modularity = props.value("modularity", 0.0);
                fv.hub_concentration = props.value("hub_concentration", 0.0);
                fv.log_nodes = std::log10(raw_nodes + 1.0);
                fv.log_edges = std::log10(raw_edges + 1.0);
                fv.density = props.value("density", 0.0);
                fv.avg_degree_100 = raw_avg_degree / 100.0;
                fv.clustering_coeff = props.value("clustering_coefficient", 0.0);
                fv.packing_factor = props.value("packing_factor", 0.0);
                fv.forward_edge_fraction = props.value("forward_edge_fraction", 0.0);
                fv.log2_wsr = std::log2(raw_wsr + 1.0);
                fv.log10_cc = std::log10(raw_cc + 1.0);
                fv.diameter_50 = raw_diameter / 50.0;
                fv.vertex_significance_skewness = props.value("vertex_significance_skewness", 0.0);
                fv.window_neighbor_overlap = props.value("window_neighbor_overlap", 0.0);
                fv.sampled_locality_score = props.value("sampled_locality_score", 0.0);

                graph_features_[name] = fv;
                raw_graph_props_[name] = props;  // keep raw JSON for training
            }
        } catch (const std::exception& e) {
            std::cerr << "[DATABASE] Error parsing " << path
                      << ": " << e.what() << "\n";
        }
    }

    // ========================================================================
    // Oracle Cache
    // ========================================================================

    /**
     * Build the oracle cache: for each (graph, benchmark) → best family.
     *
     * Strategy: group records by (graph, benchmark), find the family
     * with the lowest average time_seconds across all algorithms in that family.
     */
    void build_oracle_cache() {
        // Collect: (graph, benchmark) → family → [times]
        std::map<std::string, std::map<std::string, std::vector<double>>> grouped;

        for (const auto& r : records_) {
            std::string family = AlgoToFamily(r.algorithm);
            std::string key = r.graph + "|" + r.benchmark;
            grouped[key][family].push_back(r.time_seconds);
        }

        // For each (graph, benchmark), pick the family with the lowest average time
        for (const auto& [key, families] : grouped) {
            std::string best_fam;
            double best_time = std::numeric_limits<double>::infinity();

            for (const auto& [fam, times] : families) {
                // Use the average time across all algorithms in this family
                double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
                if (avg_t < best_time) {
                    best_time = avg_t;
                    best_fam = fam;
                }
            }

            oracle_cache_[key] = best_fam;
        }
    }

    void rebuild_oracle_entry(const BenchRecord& rec) {
        // Rebuild oracle cache for this (graph, benchmark) pair using average
        std::string bench_key = rec.graph + "|" + rec.benchmark;
        std::map<std::string, std::vector<double>> family_times;

        for (const auto& r : records_) {
            if (r.graph == rec.graph && r.benchmark == rec.benchmark) {
                std::string fam = AlgoToFamily(r.algorithm);
                family_times[fam].push_back(r.time_seconds);
            }
        }

        std::string best_fam = "ORIGINAL";
        double best_time = std::numeric_limits<double>::infinity();
        for (const auto& [fam, times] : family_times) {
            double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            if (avg < best_time) {
                best_time = avg;
                best_fam = fam;
            }
        }
        oracle_cache_[bench_key] = best_fam;
    }

    // ========================================================================
    // Saving
    // ========================================================================

    bool save_benchmarks() {
        std::string filepath = GetBenchmarksFile();
        nlohmann::json j = nlohmann::json::array();

        // Sort by (graph, benchmark, algorithm) for deterministic output
        auto sorted = records_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const BenchRecord& a, const BenchRecord& b) {
                      if (a.graph != b.graph) return a.graph < b.graph;
                      if (a.benchmark != b.benchmark) return a.benchmark < b.benchmark;
                      return a.algorithm < b.algorithm;
                  });

        for (const auto& r : sorted) {
            nlohmann::json entry;
            entry["graph"] = r.graph;
            entry["algorithm"] = r.algorithm;
            entry["algorithm_id"] = r.algorithm_id;
            entry["benchmark"] = r.benchmark;
            entry["time_seconds"] = r.time_seconds;
            entry["reorder_time"] = r.reorder_time;
            entry["trials"] = r.trials;
            entry["nodes"] = r.nodes;
            entry["edges"] = r.edges;
            entry["success"] = r.success;
            entry["error"] = "";
            entry["extra"] = nlohmann::json::object();
            j.push_back(entry);
        }

        return atomic_write_json(filepath, j);
    }

    /// Save benchmarks with full RunReport JSON (includes trial_details).
    /// Re-reads existing file, merges the new run report, writes back.
    bool save_benchmarks_with_run(const RunReport& report) {
        std::string filepath = GetBenchmarksFile();

        // Read existing array from disk
        nlohmann::json j = nlohmann::json::array();
        {
            std::ifstream ifs(filepath);
            if (ifs.is_open()) {
                try {
                    nlohmann::json existing;
                    ifs >> existing;
                    if (existing.is_array()) j = existing;
                } catch (...) {}
            }
        }

        // Dedup key
        std::string key = report.graph_name + "|" + report.algorithm + "|" + report.benchmark;

        // Find and replace existing entry, or append
        bool found = false;
        nlohmann::json new_entry = report.to_json();
        for (auto& entry : j) {
            std::string ek = entry.value("graph", "") + "|" +
                             entry.value("algorithm", "") + "|" +
                             entry.value("benchmark", "");
            if (ek == key) {
                double existing_time = entry.value("time_seconds", 1e99);
                if (report.avg_time < existing_time) {
                    entry = new_entry;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            j.push_back(new_entry);
        }

        return atomic_write_json(filepath, j);
    }

    bool save_graph_props() {
        std::string filepath = GetGraphPropsFile();
        nlohmann::json j;

        for (const auto& [name, fv] : graph_features_) {
            nlohmann::json props;
            // Reverse-transform features back to raw values for storage
            props["modularity"] = fv.modularity;
            props["hub_concentration"] = fv.hub_concentration;
            props["nodes"] = static_cast<int64_t>(std::pow(10.0, fv.log_nodes) - 1.0);
            props["edges"] = static_cast<int64_t>(std::pow(10.0, fv.log_edges) - 1.0);
            props["density"] = fv.density;
            props["avg_degree"] = fv.avg_degree_100 * 100.0;
            props["clustering_coefficient"] = fv.clustering_coeff;
            props["packing_factor"] = fv.packing_factor;
            props["forward_edge_fraction"] = fv.forward_edge_fraction;
            props["working_set_ratio"] = std::pow(2.0, fv.log2_wsr) - 1.0;
            props["community_count"] = std::pow(10.0, fv.log10_cc) - 1.0;
            props["diameter"] = fv.diameter_50 * 50.0;
            props["vertex_significance_skewness"] = fv.vertex_significance_skewness;
            props["window_neighbor_overlap"] = fv.window_neighbor_overlap;
            props["sampled_locality_score"] = fv.sampled_locality_score;
            j[name] = props;
        }

        return atomic_write_json(filepath, j);
    }

    /// Save graph properties with raw GraphProperties struct (no reverse-transform).
    /// Re-reads existing file, merges the new graph entry, writes back.
    bool save_graph_props_with_raw(const GraphProperties& props) {
        std::string filepath = GetGraphPropsFile();

        // Read existing object from disk
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream ifs(filepath);
            if (ifs.is_open()) {
                try {
                    nlohmann::json existing;
                    ifs >> existing;
                    if (existing.is_object()) j = existing;
                } catch (...) {}
            }
        }

        // Merge: overwrite this graph's properties
        j[props.graph_name] = props.to_json();

        return atomic_write_json(filepath, j);
    }

    /// Atomic JSON write: write to .tmp, then rename over target.
    static bool atomic_write_json(const std::string& filepath,
                                   const nlohmann::json& j) {
        std::string tmp = filepath + ".tmp";
        {
            std::ofstream ofs(tmp);
            if (!ofs.is_open()) {
                std::cerr << "[DATABASE] Cannot write to " << tmp << "\n";
                return false;
            }
            ofs << j.dump(2) << "\n";
        }
        if (std::rename(tmp.c_str(), filepath.c_str()) != 0) {
            std::cerr << "[DATABASE] Cannot rename " << tmp << " → "
                      << filepath << "\n";
            return false;
        }
        return true;
    }

    // ========================================================================
    // Feature Statistics (z-normalization for kNN)
    // ========================================================================

    /**
     * @brief Compute mean/std of each feature across all known graphs.
     *
     * Called after load_graph_props(). Used by euclidean_distance() to
     * z-normalize features so that no single feature dominates distance
     * (e.g., log_nodes ~6 vs density ~0.001).
     */
    void compute_feature_stats() {
        int n = static_cast<int>(graph_features_.size());
        for (int i = 0; i < N_KNN_FEATURES; i++) {
            feat_means_[i] = 0.0;
            feat_stds_[i] = 1.0;  // default: no scaling
        }
        if (n < 2) return;

        // Compute means
        for (const auto& [name, fv] : graph_features_)
            for (int i = 0; i < N_KNN_FEATURES; i++)
                feat_means_[i] += fv[i];
        for (int i = 0; i < N_KNN_FEATURES; i++)
            feat_means_[i] /= n;

        // Compute stds
        for (int i = 0; i < N_KNN_FEATURES; i++) feat_stds_[i] = 0.0;
        for (const auto& [name, fv] : graph_features_)
            for (int i = 0; i < N_KNN_FEATURES; i++) {
                double d = fv[i] - feat_means_[i];
                feat_stds_[i] += d * d;
            }
        for (int i = 0; i < N_KNN_FEATURES; i++) {
            feat_stds_[i] = std::sqrt(feat_stds_[i] / n);
            if (feat_stds_[i] < 1e-12) feat_stds_[i] = 1.0;  // avoid div-by-zero
        }
    }

    // ========================================================================
    // Distance Computation
    // ========================================================================

    /**
     * @brief Z-normalized Euclidean distance between two feature vectors.
     *
     * Each feature is scaled by (x - mean) / std before computing L2 distance,
     * ensuring features with different ranges contribute equally.
     */
    double euclidean_distance(const GraphFeatureVec& a,
                              const GraphFeatureVec& b) const {
        double sum = 0.0;
        for (int i = 0; i < N_KNN_FEATURES; ++i) {
            double za = (a[i] - feat_means_[i]) / feat_stds_[i];
            double zb = (b[i] - feat_means_[i]) / feat_stds_[i];
            double d = za - zb;
            sum += d * d;
        }
        return std::sqrt(sum);
    }

    // ========================================================================
    // Unified Model Loading
    // ========================================================================

    /**
     * @brief Load adaptive_models.json (perceptron + DT + hybrid).
     *
     * Populates perceptron_weights_, perceptron_per_bench_, and model_trees_.
     */
    void load_adaptive_models() {
        std::string models_path = GetAdaptiveModelsFile();
        std::ifstream ifs(models_path);
        if (!ifs.is_open()) {
            // No individual-file fallback: adaptive_models.json is canonical.
            // Run `export_unified_models()` from
            // Python to generate it from results/models/ staging files.
            std::cerr << "[DATABASE] adaptive_models.json not found at "
                      << models_path << " — model-based modes unavailable.\n";
            models_loaded_ = false;
            models_from_unified_ = false;
            return;
        }

        try {
            nlohmann::json j;
            ifs >> j;

            // ---- Perceptron ----
            if (j.contains("perceptron")) {
                const auto& p = j["perceptron"];
                if (p.contains("weights")) {
                    perceptron_weights_ = p["weights"];
                }
                if (p.contains("per_benchmark")) {
                    for (auto it = p["per_benchmark"].begin();
                         it != p["per_benchmark"].end(); ++it) {
                        perceptron_per_bench_[it.key()] = it.value();
                    }
                }
            }

            // ---- Decision Tree ----
            if (j.contains("decision_tree")) {
                for (auto it = j["decision_tree"].begin();
                     it != j["decision_tree"].end(); ++it) {
                    ModelTree mt;
                    if (parse_model_tree_from_nlohmann(it.value(), mt)) {
                        model_trees_["decision_tree"][it.key()] = mt;
                    }
                }
            }

            // ---- Hybrid ----
            if (j.contains("hybrid")) {
                for (auto it = j["hybrid"].begin();
                     it != j["hybrid"].end(); ++it) {
                    ModelTree mt;
                    if (parse_model_tree_from_nlohmann(it.value(), mt)) {
                        model_trees_["hybrid"][it.key()] = mt;
                    }
                }
            }

            models_loaded_ = true;
            models_from_unified_ = true;
        } catch (const std::exception& e) {
            std::cerr << "[DATABASE] Error parsing " << models_path
                      << ": " << e.what() << "\n";
            models_loaded_ = false;
            models_from_unified_ = false;
        }
    }

    /**
     * @brief Parse a ModelTree from a nlohmann::json object.
     *
     * Same format as ParseModelTreeFromJSON but using structured JSON access.
     */
    static bool parse_model_tree_from_nlohmann(const nlohmann::json& j, ModelTree& mt) {
        try {
            mt.model_type = j.value("model_type", "decision_tree");
            mt.benchmark = j.value("benchmark", "");

            // Parse families
            if (j.contains("families") && j["families"].is_array()) {
                for (const auto& f : j["families"]) {
                    mt.families.push_back(f.get<std::string>());
                }
            }

            // Parse nodes
            if (!j.contains("nodes") || !j["nodes"].is_array()) return false;

            for (const auto& node_j : j["nodes"]) {
                ModelTreeNode node;

                if (node_j.contains("leaf_class")) {
                    // Leaf node
                    node.feature_idx = -1;
                    node.leaf_class = node_j.value("leaf_class", "ORIGINAL");
                    node.samples = node_j.value("samples", 0);

                    // Hybrid: parse per-family weights
                    if (node_j.contains("weights") && node_j["weights"].is_object()) {
                        for (auto wit = node_j["weights"].begin();
                             wit != node_j["weights"].end(); ++wit) {
                            std::vector<double> wvec;
                            for (const auto& v : wit.value()) {
                                wvec.push_back(v.get<double>());
                            }
                            node.leaf_weights[wit.key()] = wvec;
                        }
                    }
                } else {
                    // Split node
                    node.feature_idx = node_j.value("feature_idx", -1);
                    node.threshold = node_j.value("threshold", 0.0);
                    node.left = node_j.value("left", -1);
                    node.right = node_j.value("right", -1);
                    node.samples = node_j.value("samples", 0);
                }

                mt.nodes.push_back(node);
            }

            mt.loaded = !mt.nodes.empty();
            return mt.loaded;
        } catch (const std::exception& e) {
            std::cerr << "[DATABASE] Error parsing model tree: " << e.what() << "\n";
            return false;
        }
    }

    // ========================================================================
    // Runtime Model Training (from raw DB data)
    // ========================================================================

    /// Number of perceptron features (22: 16 base + 5 quadratic interactions + 1 CL packing factor)
    static constexpr int N_PERCEPTRON_FEATURES = 22;

    /// Weight key names matching Python training order (index → JSON field name)
    static constexpr const char* WEIGHT_KEYS[N_PERCEPTRON_FEATURES] = {
        "w_modularity", "w_degree_variance", "w_hub_concentration",
        "w_log_nodes", "w_log_edges", "w_density", "w_avg_degree",
        "w_clustering_coeff", "w_avg_path_length", "w_diameter",
        "w_community_count", "w_packing_factor", "w_forward_edge_fraction",
        "w_working_set_ratio", "w_vertex_significance_skewness",
        "w_window_neighbor_overlap",
        "w_dv_x_hub", "w_mod_x_logn", "w_pf_x_wsr",
        "w_vss_x_hc", "w_wno_x_pf",
        "w_packing_factor_cl"
    };

    /**
     * @brief Extract 22 perceptron features from raw graph properties JSON.
     *
     * Feature order matches Python training and scoreBaseNormalized():
     *   0=modularity, 1=degree_variance, 2=hub_concentration,
     *   3=log_nodes, 4=log_edges, 5=density, 6=avg_degree/100,
     *   7=clustering_coeff, 8=avg_path_length/10, 9=diameter/50,
     *   10=log10(community_count+1), 11=packing_factor,
     *   12=forward_edge_fraction, 13=log2(working_set_ratio+1),
     *   14=vertex_significance_skewness, 15=window_neighbor_overlap,
     *   16=dv×hc, 17=mod×logn, 18=pf×log_wsr,
     *   19=vss×hc, 20=wno×pf, 21=packing_factor_cl
     */
    static void extract_perceptron_features(const nlohmann::json& props,
                                             double out[N_PERCEPTRON_FEATURES]) {
        double modularity   = props.value("modularity", 0.0);
        double dv           = props.value("degree_variance", 0.0);
        double hc           = props.value("hub_concentration", 0.0);
        double log_n        = std::log10(props.value("nodes", 0.0) + 1.0);
        double log_e        = std::log10(props.value("edges", 0.0) + 1.0);
        double density      = props.value("density", 0.0);
        double ad_100       = props.value("avg_degree", 0.0) / 100.0;
        double cc           = props.value("clustering_coefficient", 0.0);
        double apl_10       = props.value("avg_path_length", 0.0) / 10.0;
        double dia_50       = props.value("diameter", 0.0) / 50.0;
        double log_comm     = std::log10(props.value("community_count", 0.0) + 1.0);
        double pf           = props.value("packing_factor", 0.0);
        double fef          = props.value("forward_edge_fraction", 0.0);
        double log_wsr      = std::log2(props.value("working_set_ratio", 0.0) + 1.0);
        double vss          = props.value("vertex_significance_skewness", 0.0);
        double wno          = props.value("window_neighbor_overlap", 0.0);

        out[0]  = modularity;
        out[1]  = dv;
        out[2]  = hc;
        out[3]  = log_n;
        out[4]  = log_e;
        out[5]  = density;
        out[6]  = ad_100;
        out[7]  = cc;
        out[8]  = apl_10;
        out[9]  = dia_50;
        out[10] = log_comm;
        out[11] = pf;
        out[12] = fef;
        out[13] = log_wsr;
        out[14] = vss;
        out[15] = wno;
        // Quadratic interactions
        out[16] = dv * hc;
        out[17] = modularity * log_n;
        out[18] = pf * log_wsr;
        out[19] = vss * hc;     // significance skewness × hub concentration
        out[20] = wno * pf;     // window overlap × packing factor
        // IISWC'18 cache-line packing factor
        out[21] = props.value("packing_factor_cl", 0.0);
    }

    /**
     * @brief Simple deterministic LCG PRNG for reproducible training.
     *
     * Avoids dependency on <random> and gives identical results across platforms.
     */
    struct SimpleRNG {
        uint64_t state;
        explicit SimpleRNG(uint64_t seed) : state(seed ^ 0x9e3779b97f4a7c15ULL) {}
        uint64_t next() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state >> 16;
        }
        double uniform() { return (next() & 0xFFFFFFFF) / 4294967296.0; }
        double gauss(double mean, double std) {
            // Box-Muller transform
            double u1 = uniform(), u2 = uniform();
            if (u1 < 1e-15) u1 = 1e-15;
            return mean + std * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
        }
        void shuffle(std::vector<int>& v) {
            for (int i = static_cast<int>(v.size()) - 1; i > 0; --i) {
                int j = static_cast<int>(next() % (i + 1));
                std::swap(v[i], v[j]);
            }
        }
    };

    /**
     * @brief Train per-benchmark perceptrons from raw DB data.
     *
     * Implements the same multi-class averaged perceptron as Python's
     * compute_weights_from_results():
     *   - 800 epochs, 5 restarts
     *   - Jimenez margin-based updates with adaptive theta
     *   - Weight clamping at [-16, +16]
     *   - LR 0.05 with 0.997 decay per epoch
     *   - Averaged perceptron (Freund & Schapire 1999)
     *   - Best of averaged vs snapshot per restart
     *
     * Populates perceptron_per_bench_ and perceptron_weights_.
     */
    void train_perceptron() {
        static constexpr int N_EPOCHS   = 800;
        static constexpr int N_RESTARTS = 5;
        static constexpr double W_MAX   = 16.0;
        static constexpr double LR_INIT = 0.05;
        static constexpr double LR_DECAY = 0.997;
        static constexpr int NF = N_PERCEPTRON_FEATURES;
        // Jimenez theta: floor((1.93*h + 14) * W_MAX / 127)
        const int THETA_INIT = std::max(1, static_cast<int>(
            (1.93 * NF + 14) * W_MAX / 127.0));

        // Collect unique benchmarks
        std::set<std::string> bench_set;
        for (const auto& r : records_) bench_set.insert(r.benchmark);

        // Collect unique graphs that have both features and benchmark data
        std::set<std::string> graph_set;
        for (const auto& r : records_) {
            if (raw_graph_props_.count(r.graph)) graph_set.insert(r.graph);
        }
        if (graph_set.empty()) return;

        // Extract features for all training graphs
        std::map<std::string, std::vector<double>> graph_feats;
        for (const auto& g : graph_set) {
            std::vector<double> fv(NF);
            extract_perceptron_features(raw_graph_props_.at(g), fv.data());
            graph_feats[g] = std::move(fv);
        }

        // Compute z-score normalization stats from ALL training graphs
        std::vector<double> feat_means(NF, 0.0);
        std::vector<double> feat_stds(NF, 0.0);
        {
            int n = static_cast<int>(graph_feats.size());
            for (const auto& [g, fv] : graph_feats)
                for (int i = 0; i < NF; i++) feat_means[i] += fv[i];
            for (int i = 0; i < NF; i++) feat_means[i] /= n;

            for (const auto& [g, fv] : graph_feats)
                for (int i = 0; i < NF; i++) {
                    double d = fv[i] - feat_means[i];
                    feat_stds[i] += d * d;
                }
            for (int i = 0; i < NF; i++)
                feat_stds[i] = std::sqrt(feat_stds[i] / std::max(1, n));
        }

        // Z-normalize all feature vectors
        std::map<std::string, std::vector<double>> graph_feats_z;
        for (const auto& [g, fv] : graph_feats) {
            std::vector<double> z(NF);
            for (int i = 0; i < NF; i++) {
                z[i] = (feat_stds[i] > 1e-12) ? (fv[i] - feat_means[i]) / feat_stds[i] : 0.0;
            }
            graph_feats_z[g] = std::move(z);
        }

        // Build per-benchmark training data: (z-features, oracle_family)
        // across per-bench averaged perceptrons for global weights
        std::map<std::string, std::vector<double>> global_accum;  // family → accumulated bias+weights
        std::map<std::string, int> global_count;  // family → count of benchmarks contributing
        int bench_idx = 0;

        for (const auto& bn : bench_set) {
            // Collect (graph, best_family) pairs for this benchmark
            struct Sample { std::vector<double> fv; std::string family; double importance; };
            std::vector<Sample> data;
            std::set<std::string> active_families;

            for (const auto& g : graph_set) {
                std::string key = g + "|" + bn;
                auto it = oracle_cache_.find(key);
                if (it == oracle_cache_.end()) continue;

                // Significance-weighted training (DON-RL): compute speedup
                // range for this graph+benchmark. High range = algorithm
                // choice matters a lot → weight sample higher.
                double best_time = 1e30, worst_time = 0.0;
                for (const auto& r : records_) {
                    if (r.graph == g && r.benchmark == bn && r.success) {
                        best_time = std::min(best_time, r.time_seconds);
                        worst_time = std::max(worst_time, r.time_seconds);
                    }
                }
                double speedup_range = (best_time > 1e-12 && worst_time > 0.0)
                    ? worst_time / best_time : 1.0;
                // Importance = log(speedup_range + 1), clamped to [1, 5]
                double importance = std::max(1.0, std::min(5.0,
                    std::log(speedup_range + 1.0)));

                data.push_back({graph_feats_z.at(g), it->second, importance});
                active_families.insert(it->second);
            }
            if (data.empty() || active_families.empty()) {
                bench_idx++;
                continue;
            }

            // Sort active families for determinism
            std::vector<std::string> families(active_families.begin(), active_families.end());

            // Multi-class averaged perceptron training
            struct FamilyWeights {
                double bias = 0.0;
                std::vector<double> w;
                FamilyWeights() : w(NF, 0.0) {}
            };

            double global_best_acc = 0.0;
            std::map<std::string, FamilyWeights> global_best_snap;

            for (int restart = 0; restart < N_RESTARTS; restart++) {
                SimpleRNG rng(42 + restart * 1000 + bench_idx * 100);

                // Initialize with small random weights
                std::map<std::string, FamilyWeights> bw;
                for (const auto& fam : families) {
                    bw[fam].bias = rng.gauss(0.0, 0.1);
                    bw[fam].w.resize(NF);
                    for (int i = 0; i < NF; i++) bw[fam].w[i] = rng.gauss(0.0, 0.1);
                }

                // Averaged weight accumulators
                std::map<std::string, FamilyWeights> avg_bw;
                for (const auto& fam : families) avg_bw[fam].w.resize(NF, 0.0);
                int avg_count = 0;

                double lr = LR_INIT;
                double best_acc = 0.0;
                std::map<std::string, FamilyWeights> best_snap;

                // Adaptive theta
                double theta = static_cast<double>(THETA_INIT);
                double theta_max = THETA_INIT * 3.0;
                double theta_step = std::max(0.1, 1.0 / std::max(static_cast<int>(data.size()), 1));

                // Shuffled index array
                std::vector<int> indices(data.size());
                std::iota(indices.begin(), indices.end(), 0);

                for (int epoch = 0; epoch < N_EPOCHS; epoch++) {
                    rng.shuffle(indices);
                    int correct = 0;

                    for (int idx : indices) {
                        const auto& fv = data[idx].fv;
                        const auto& true_fam = data[idx].family;
                        const double imp = data[idx].importance;
                        if (bw.find(true_fam) == bw.end()) continue;

                        // Score all families
                        std::string pred;
                        double pred_score = -1e30, true_score = -1e30;
                        for (const auto& [fam, fw] : bw) {
                            double s = fw.bias;
                            for (int i = 0; i < NF; i++) s += fw.w[i] * fv[i];
                            if (s > pred_score) { pred_score = s; pred = fam; }
                            if (fam == true_fam) true_score = s;
                        }

                        bool is_correct = (pred == true_fam);
                        if (is_correct) correct++;

                        // Compute margin
                        double margin;
                        if (is_correct) {
                            double runner_up = -1e30;
                            for (const auto& [fam, fw] : bw) {
                                if (fam == true_fam) continue;
                                double s = fw.bias;
                                for (int i = 0; i < NF; i++) s += fw.w[i] * fv[i];
                                if (s > runner_up) runner_up = s;
                            }
                            margin = true_score - runner_up;
                        } else {
                            margin = -1.0;
                        }

                        // Adaptive theta update (Jimenez MICRO 2016)
                        if (is_correct && margin > theta)
                            theta = std::min(theta_max, theta + theta_step);
                        else if (!is_correct)
                            theta = std::max(0.0, theta - theta_step);

                        // Jimenez update: wrong OR margin <= theta
                        // Scaled by sample importance (DON-RL significance weighting)
                        if (!is_correct || margin <= theta) {
                            auto clamp = [](double v, double lo, double hi) {
                                return std::max(lo, std::min(hi, v));
                            };
                            double scaled_lr = lr * imp;
                            // Promote true class
                            bw[true_fam].bias = clamp(bw[true_fam].bias + scaled_lr, -W_MAX, W_MAX);
                            for (int i = 0; i < NF; i++)
                                bw[true_fam].w[i] = clamp(bw[true_fam].w[i] + scaled_lr * fv[i], -W_MAX, W_MAX);
                            // Demote predicted class (only on error)
                            if (!is_correct) {
                                bw[pred].bias = clamp(bw[pred].bias - scaled_lr, -W_MAX, W_MAX);
                                for (int i = 0; i < NF; i++)
                                    bw[pred].w[i] = clamp(bw[pred].w[i] - scaled_lr * fv[i], -W_MAX, W_MAX);
                            }
                        }

                        // Accumulate for averaging
                        avg_count++;
                        for (const auto& fam : families) {
                            avg_bw[fam].bias += bw[fam].bias;
                            for (int i = 0; i < NF; i++)
                                avg_bw[fam].w[i] += bw[fam].w[i];
                        }
                    }

                    double acc = static_cast<double>(correct) / data.size();
                    if (acc > best_acc) {
                        best_acc = acc;
                        best_snap = bw;
                    }
                    lr *= LR_DECAY;
                }

                // Compute averaged weights
                std::map<std::string, FamilyWeights> avg_snap;
                if (avg_count > 0) {
                    for (const auto& fam : families) {
                        avg_snap[fam].bias = avg_bw[fam].bias / avg_count;
                        avg_snap[fam].w.resize(NF);
                        for (int i = 0; i < NF; i++)
                            avg_snap[fam].w[i] = avg_bw[fam].w[i] / avg_count;
                    }
                } else {
                    avg_snap = best_snap;
                }

                // Evaluate averaged weights
                double avg_acc = 0.0;
                if (!avg_snap.empty()) {
                    int avg_correct = 0;
                    for (const auto& d : data) {
                        if (avg_snap.find(d.family) == avg_snap.end()) continue;
                        double best_s = -1e30;
                        std::string apred;
                        for (const auto& [fam, fw] : avg_snap) {
                            double s = fw.bias;
                            for (int i = 0; i < NF; i++) s += fw.w[i] * d.fv[i];
                            if (s > best_s) { best_s = s; apred = fam; }
                        }
                        if (apred == d.family) avg_correct++;
                    }
                    avg_acc = static_cast<double>(avg_correct) / data.size();
                }

                // Pick better of averaged vs snapshot
                double use_acc = std::max(avg_acc, best_acc);
                const auto& use_snap = (avg_acc >= best_acc && !avg_snap.empty())
                                        ? avg_snap : best_snap;

                if (use_acc > global_best_acc) {
                    global_best_acc = use_acc;
                    global_best_snap = use_snap;
                }
            }

            // Convert global_best_snap to JSON for this benchmark
            if (!global_best_snap.empty()) {
                nlohmann::json bench_j;
                for (const auto& [fam, fw] : global_best_snap) {
                    nlohmann::json entry;
                    entry["bias"] = fw.bias;
                    for (int i = 0; i < NF; i++) entry[WEIGHT_KEYS[i]] = fw.w[i];
                    entry["benchmark_weights"] = nlohmann::json::object();
                    entry["_metadata"] = nlohmann::json::object();
                    bench_j[fam] = entry;

                    // Accumulate for global averaged weights
                    if (global_accum.find(fam) == global_accum.end())
                        global_accum[fam].resize(NF + 1, 0.0);  // +1 for bias
                    global_accum[fam][0] += fw.bias;
                    for (int i = 0; i < NF; i++) global_accum[fam][i + 1] += fw.w[i];
                    global_count[fam]++;
                }

                // Add normalization stats (using raw means/stds before z-score)
                nlohmann::json norm;
                nlohmann::json means_arr = nlohmann::json::array();
                nlohmann::json stds_arr = nlohmann::json::array();
                nlohmann::json keys_arr = nlohmann::json::array();
                for (int i = 0; i < NF; i++) {
                    means_arr.push_back(feat_means[i]);
                    stds_arr.push_back(feat_stds[i]);
                    keys_arr.push_back(WEIGHT_KEYS[i]);
                }
                norm["feat_means"] = means_arr;
                norm["feat_stds"] = stds_arr;
                norm["weight_keys"] = keys_arr;
                bench_j["_normalization"] = norm;

                perceptron_per_bench_[bn] = bench_j;
            }

            std::cout << "[TRAIN] Perceptron " << bn << ": acc="
                      << static_cast<int>(global_best_acc * 100) << "% ("
                      << data.size() << " examples, "
                      << global_best_snap.size() << " families)\n";
            bench_idx++;
        }

        // Build averaged global weights from per-bench perceptrons
        if (!global_accum.empty()) {
            nlohmann::json avg_j;
            for (const auto& [fam, acc] : global_accum) {
                int nc = global_count[fam];
                if (nc <= 0) continue;
                nlohmann::json entry;
                entry["bias"] = acc[0] / nc;
                for (int i = 0; i < NF; i++) entry[WEIGHT_KEYS[i]] = acc[i + 1] / nc;
                entry["benchmark_weights"] = nlohmann::json::object();
                entry["_metadata"] = nlohmann::json::object();
                avg_j[fam] = entry;
            }
            // Add normalization to global too
            nlohmann::json norm;
            nlohmann::json means_arr = nlohmann::json::array();
            nlohmann::json stds_arr = nlohmann::json::array();
            nlohmann::json keys_arr = nlohmann::json::array();
            for (int i = 0; i < NF; i++) {
                means_arr.push_back(feat_means[i]);
                stds_arr.push_back(feat_stds[i]);
                keys_arr.push_back(WEIGHT_KEYS[i]);
            }
            norm["feat_means"] = means_arr;
            norm["feat_stds"] = stds_arr;
            norm["weight_keys"] = keys_arr;
            avg_j["_normalization"] = norm;
            perceptron_weights_ = avg_j;
        }
    }

    // ========================================================================
    // Decision Tree Training (CART)
    // ========================================================================

    /**
     * @brief Compute weighted Gini impurity for a set of label indices.
     */
    static double gini_impurity(const std::vector<int>& labels,
                                 const std::vector<int>& indices,
                                 int n_classes,
                                 const std::vector<double>& sample_weights) {
        if (indices.empty()) return 0.0;
        std::vector<double> class_sum(n_classes, 0.0);
        double total = 0.0;
        for (int idx : indices) {
            class_sum[labels[idx]] += sample_weights[idx];
            total += sample_weights[idx];
        }
        if (total <= 0.0) return 0.0;
        double gini = 1.0;
        for (int c = 0; c < n_classes; c++) {
            double p = class_sum[c] / total;
            gini -= p * p;
        }
        return gini;
    }

    /**
     * @brief Find the weighted majority class in a subset.
     */
    static int majority_class(const std::vector<int>& labels,
                               const std::vector<int>& indices,
                               int n_classes,
                               const std::vector<double>& sample_weights) {
        std::vector<double> class_sum(n_classes, 0.0);
        for (int idx : indices) class_sum[labels[idx]] += sample_weights[idx];
        return static_cast<int>(std::max_element(class_sum.begin(), class_sum.end()) - class_sum.begin());
    }

    /**
     * @brief Recursively build a CART decision tree node.
     *
     * @param features   [n_samples][n_features] feature matrix
     * @param labels     [n_samples] class labels (indices into class_names)
     * @param weights    [n_samples] sample weights (for class balancing)
     * @param indices    Subset of sample indices at this node
     * @param n_classes  Number of unique classes
     * @param tree       Output ModelTree (nodes appended)
     * @param depth      Current tree depth
     * @param max_depth  Maximum allowed depth
     * @param min_leaf   Minimum samples per leaf
     * @param n_features Number of features
     * @return Index of the created node in tree.nodes
     */
    static int build_cart_node(
            const std::vector<std::vector<double>>& features,
            const std::vector<int>& labels,
            const std::vector<double>& weights,
            const std::vector<int>& indices,
            int n_classes,
            ModelTree& tree,
            int depth, int max_depth, int min_leaf, int n_features) {

        int my_idx = static_cast<int>(tree.nodes.size());
        tree.nodes.emplace_back();

        // Stopping conditions: max depth, too few samples, or pure node
        bool all_same = true;
        int first_label = labels[indices[0]];
        for (size_t i = 1; i < indices.size(); i++) {
            if (labels[indices[i]] != first_label) { all_same = false; break; }
        }

        if (depth >= max_depth || static_cast<int>(indices.size()) < min_leaf * 2 || all_same) {
            int mc = majority_class(labels, indices, n_classes, weights);
            tree.nodes[my_idx].feature_idx = -1;
            tree.nodes[my_idx].leaf_class = tree.families[mc];
            tree.nodes[my_idx].samples = static_cast<int>(indices.size());
            return my_idx;
        }

        // Find best split across all features
        int best_feat = -1;
        double best_thresh = 0.0;
        double best_gini = std::numeric_limits<double>::infinity();
        std::vector<int> best_left, best_right;

        // Re-usable sorted-index buffer
        std::vector<std::pair<double, int>> feat_vals(indices.size());

        for (int f = 0; f < n_features; f++) {
            // Sort indices by feature value
            for (size_t i = 0; i < indices.size(); i++)
                feat_vals[i] = {features[indices[i]][f], indices[i]};
            std::sort(feat_vals.begin(), feat_vals.end());

            // Sweep to find best threshold
            // Accumulate left partition
            std::vector<double> left_sum(n_classes, 0.0);
            std::vector<double> right_sum(n_classes, 0.0);
            double left_total = 0.0, right_total = 0.0;

            for (const auto& [val, idx] : feat_vals) {
                right_sum[labels[idx]] += weights[idx];
                right_total += weights[idx];
            }

            for (size_t i = 0; i < feat_vals.size() - 1; i++) {
                int idx = feat_vals[i].second;
                left_sum[labels[idx]] += weights[idx];
                left_total += weights[idx];
                right_sum[labels[idx]] -= weights[idx];
                right_total -= weights[idx];

                // Only split between different feature values
                if (feat_vals[i].first == feat_vals[i + 1].first) continue;

                // Check min_leaf constraint
                if (static_cast<int>(i + 1) < min_leaf ||
                    static_cast<int>(feat_vals.size() - i - 1) < min_leaf) continue;

                // Compute weighted Gini
                double gini_left = 1.0, gini_right = 1.0;
                for (int c = 0; c < n_classes; c++) {
                    double pl = left_sum[c] / left_total;
                    gini_left -= pl * pl;
                    double pr = right_sum[c] / right_total;
                    gini_right -= pr * pr;
                }
                double total = left_total + right_total;
                double weighted_gini = (left_total * gini_left + right_total * gini_right) / total;

                if (weighted_gini < best_gini) {
                    best_gini = weighted_gini;
                    best_feat = f;
                    best_thresh = (feat_vals[i].first + feat_vals[i + 1].first) / 2.0;
                }
            }
        }

        // If no valid split found, make leaf
        if (best_feat < 0) {
            int mc = majority_class(labels, indices, n_classes, weights);
            tree.nodes[my_idx].feature_idx = -1;
            tree.nodes[my_idx].leaf_class = tree.families[mc];
            tree.nodes[my_idx].samples = static_cast<int>(indices.size());
            return my_idx;
        }

        // Partition indices
        std::vector<int> left_idx, right_idx;
        for (int idx : indices) {
            if (features[idx][best_feat] <= best_thresh)
                left_idx.push_back(idx);
            else
                right_idx.push_back(idx);
        }

        // Set split info
        tree.nodes[my_idx].feature_idx = best_feat;
        tree.nodes[my_idx].threshold = best_thresh;
        tree.nodes[my_idx].samples = static_cast<int>(indices.size());

        // Recursively build children
        int left_child = build_cart_node(features, labels, weights, left_idx,
                                          n_classes, tree, depth + 1, max_depth, min_leaf, n_features);
        int right_child = build_cart_node(features, labels, weights, right_idx,
                                           n_classes, tree, depth + 1, max_depth, min_leaf, n_features);

        tree.nodes[my_idx].left = left_child;
        tree.nodes[my_idx].right = right_child;
        return my_idx;
    }

    /**
     * @brief Train decision trees from raw DB data (one per benchmark).
     *
     * CART with Gini impurity, max_depth=3, min_samples_leaf=3,
     * balanced class weights. Uses 12 features matching ModelTree::extract_features.
     *
     * Populates model_trees_["decision_tree"][bench].
     */
    void train_decision_tree() {
        static constexpr int MAX_DEPTH = 3;
        static constexpr int MIN_LEAF = 3;
        static constexpr int NF = MODEL_TREE_N_FEATURES;  // 12

        std::set<std::string> bench_set;
        for (const auto& r : records_) bench_set.insert(r.benchmark);

        for (const auto& bn : bench_set) {
            // Build training samples: (12 features, family_label)
            std::set<std::string> family_set;
            struct Sample { std::vector<double> fv; std::string family; };
            std::vector<Sample> samples;

            for (const auto& [g, fv] : graph_features_) {
                std::string key = g + "|" + bn;
                auto it = oracle_cache_.find(key);
                if (it == oracle_cache_.end()) continue;

                std::vector<double> feat(NF);
                for (int i = 0; i < NF; i++) feat[i] = fv[i];
                samples.push_back({std::move(feat), it->second});
                family_set.insert(it->second);
            }

            if (samples.size() < 3) continue;

            // Build class index
            std::vector<std::string> families(family_set.begin(), family_set.end());
            std::map<std::string, int> fam_to_idx;
            for (size_t i = 0; i < families.size(); i++) fam_to_idx[families[i]] = static_cast<int>(i);
            int n_classes = static_cast<int>(families.size());

            // Convert to arrays
            std::vector<std::vector<double>> feat_matrix;
            std::vector<int> label_vec;
            feat_matrix.reserve(samples.size());
            label_vec.reserve(samples.size());
            for (const auto& s : samples) {
                feat_matrix.push_back(s.fv);
                label_vec.push_back(fam_to_idx[s.family]);
            }

            // Balanced class weights: total / (n_classes * class_count)
            std::vector<int> class_counts(n_classes, 0);
            for (int l : label_vec) class_counts[l]++;
            std::vector<double> sample_weights(samples.size());
            double total = static_cast<double>(samples.size());
            for (size_t i = 0; i < samples.size(); i++) {
                int c = label_vec[i];
                sample_weights[i] = (class_counts[c] > 0)
                    ? total / (n_classes * class_counts[c]) : 1.0;
            }

            // Build tree
            ModelTree tree;
            tree.model_type = "decision_tree";
            tree.benchmark = bn;
            tree.families = families;

            std::vector<int> all_indices(samples.size());
            std::iota(all_indices.begin(), all_indices.end(), 0);

            build_cart_node(feat_matrix, label_vec, sample_weights,
                           all_indices, n_classes, tree, 0, MAX_DEPTH, MIN_LEAF, NF);

            tree.loaded = !tree.nodes.empty();
            model_trees_["decision_tree"][bn] = std::move(tree);

            std::cout << "[TRAIN] Decision tree " << bn << ": "
                      << model_trees_["decision_tree"][bn].nodes.size()
                      << " nodes (" << samples.size() << " examples, "
                      << n_classes << " families)\n";
        }
    }

    /**
     * @brief Train hybrid models (DT routing + per-leaf perceptron).
     *
     * Builds DT (depth 4) then trains a small perceptron at each leaf
     * (200 epochs, hinge loss). Leaf weights stored in ModelTreeNode::leaf_weights
     * for dot-product scoring at prediction time.
     *
     * Populates model_trees_["hybrid"][bench].
     */
    void train_hybrid() {
        static constexpr int DT_MAX_DEPTH = 3;  // Shallower tree for routing
        static constexpr int MIN_LEAF = 3;
        static constexpr int LEAF_EPOCHS = 200;
        static constexpr double LEAF_LR = 0.01;
        static constexpr int NF = MODEL_TREE_N_FEATURES;  // 12

        std::set<std::string> bench_set;
        for (const auto& r : records_) bench_set.insert(r.benchmark);

        for (const auto& bn : bench_set) {
            // Build training samples with 12 features
            std::set<std::string> family_set;
            struct Sample { std::vector<double> fv; std::string family; };
            std::vector<Sample> samples;

            for (const auto& [g, fv] : graph_features_) {
                std::string key = g + "|" + bn;
                auto it = oracle_cache_.find(key);
                if (it == oracle_cache_.end()) continue;

                std::vector<double> feat(NF);
                for (int i = 0; i < NF; i++) feat[i] = fv[i];
                samples.push_back({std::move(feat), it->second});
                family_set.insert(it->second);
            }

            if (samples.size() < 3) continue;

            std::vector<std::string> families(family_set.begin(), family_set.end());
            std::map<std::string, int> fam_to_idx;
            for (size_t i = 0; i < families.size(); i++) fam_to_idx[families[i]] = static_cast<int>(i);
            int n_classes = static_cast<int>(families.size());

            // Build arrays
            std::vector<std::vector<double>> feat_matrix;
            std::vector<int> label_vec;
            for (const auto& s : samples) {
                feat_matrix.push_back(s.fv);
                label_vec.push_back(fam_to_idx[s.family]);
            }

            // Balanced weights
            std::vector<int> class_counts(n_classes, 0);
            for (int l : label_vec) class_counts[l]++;
            std::vector<double> sample_weights(samples.size());
            double total = static_cast<double>(samples.size());
            for (size_t i = 0; i < samples.size(); i++) {
                int c = label_vec[i];
                sample_weights[i] = (class_counts[c] > 0)
                    ? total / (n_classes * class_counts[c]) : 1.0;
            }

            // Build routing tree (shallower)
            ModelTree tree;
            tree.model_type = "hybrid";
            tree.benchmark = bn;
            tree.families = families;

            std::vector<int> all_indices(samples.size());
            std::iota(all_indices.begin(), all_indices.end(), 0);

            build_cart_node(feat_matrix, label_vec, sample_weights,
                           all_indices, n_classes, tree, 0, DT_MAX_DEPTH, MIN_LEAF, NF);

            // Train per-leaf perceptrons
            // Route each sample to its leaf, then train a pairwise ranking perceptron
            // at each leaf that has multiple classes
            auto route_to_leaf = [&](const std::vector<double>& fv) -> int {
                int idx = 0;
                for (int iter = 0; iter < static_cast<int>(tree.nodes.size()) * 2; iter++) {
                    if (idx < 0 || idx >= static_cast<int>(tree.nodes.size())) return 0;
                    const auto& node = tree.nodes[idx];
                    if (node.is_leaf()) return idx;
                    if (node.feature_idx >= NF) { idx = node.right; continue; }
                    idx = (fv[node.feature_idx] <= node.threshold) ? node.left : node.right;
                }
                return 0;
            };

            // Group samples by leaf
            std::map<int, std::vector<int>> leaf_samples;
            for (size_t i = 0; i < samples.size(); i++) {
                int leaf = route_to_leaf(feat_matrix[i]);
                leaf_samples[leaf].push_back(static_cast<int>(i));
            }

            // Train perceptron at each leaf
            for (auto& [leaf_idx, sample_ids] : leaf_samples) {
                if (sample_ids.size() < 2) continue;

                // Check if leaf has multiple classes
                std::set<int> leaf_classes;
                for (int si : sample_ids) leaf_classes.insert(label_vec[si]);
                if (leaf_classes.size() < 2) continue;

                // Train multi-class perceptron: weights = [w0..w11, bias]
                std::map<std::string, std::vector<double>> leaf_w;
                for (int ci : leaf_classes) {
                    leaf_w[families[ci]].resize(NF + 1, 0.0);  // NF weights + 1 bias
                }

                SimpleRNG rng(42 + leaf_idx * 37);
                std::vector<int> perm = sample_ids;

                for (int epoch = 0; epoch < LEAF_EPOCHS; epoch++) {
                    rng.shuffle(perm);
                    for (int si : perm) {
                        const auto& fv = feat_matrix[si];
                        const auto& true_fam = families[label_vec[si]];

                        // Score all families at this leaf
                        std::string pred;
                        double pred_s = -1e30, true_s = -1e30;
                        for (const auto& [fam, wv] : leaf_w) {
                            double s = wv[NF]; // bias
                            for (int f = 0; f < NF; f++) s += wv[f] * fv[f];
                            if (s > pred_s) { pred_s = s; pred = fam; }
                            if (fam == true_fam) true_s = s;
                        }

                        // Hinge update: margin = true - pred
                        if (pred != true_fam) {
                            for (int f = 0; f < NF; f++) {
                                leaf_w[true_fam][f] += LEAF_LR * fv[f];
                                leaf_w[pred][f] -= LEAF_LR * fv[f];
                            }
                            leaf_w[true_fam][NF] += LEAF_LR;
                            leaf_w[pred][NF] -= LEAF_LR;
                        }
                    }
                }

                tree.nodes[leaf_idx].leaf_weights = std::move(leaf_w);
            }

            tree.loaded = !tree.nodes.empty();
            model_trees_["hybrid"][bn] = std::move(tree);

            std::cout << "[TRAIN] Hybrid " << bn << ": "
                      << model_trees_["hybrid"][bn].nodes.size()
                      << " nodes, " << leaf_samples.size() << " leaves ("
                      << samples.size() << " examples)\n";
        }
    }

    /**
     * @brief Train all models from raw DB data.
     *
     * Called by load() when sufficient training data exists.
     * Replaces load_adaptive_models() — no adaptive_models.json needed.
     */
    void train_all_models() {
        std::cout << "[TRAIN] Training models from DB ("
                  << raw_graph_props_.size() << " graphs, "
                  << records_.size() << " records)...\n";

        train_perceptron();
        train_decision_tree();
        train_hybrid();

        models_loaded_ = true;
        models_from_unified_ = false;
    }

    // ========================================================================
    // Data Members
    // ========================================================================

    std::vector<BenchRecord> records_;
    std::unordered_map<std::string, size_t> dedup_;           // key → index
    std::unordered_map<std::string, std::string> oracle_cache_;  // "graph|bench" → family
    std::map<std::string, GraphFeatureVec> graph_features_;   // graph_name → features
    std::map<std::string, nlohmann::json> raw_graph_props_;   // graph_name → raw JSON (for training)
    bool loaded_ = false;
    std::mutex mutex_;

    // --- kNN z-normalization stats (computed from graph_features_) ---
    double feat_means_[N_KNN_FEATURES] = {};
    double feat_stds_[N_KNN_FEATURES] = {};

    // --- Unified models (from adaptive_models.json) ---
    bool models_loaded_ = false;
    bool models_from_unified_ = false;
    nlohmann::json perceptron_weights_;                        // averaged weights
    std::map<std::string, nlohmann::json> perceptron_per_bench_; // bench → weights
    // model_trees_[subdir][bench] → ModelTree
    std::map<std::string, std::map<std::string, ModelTree>> model_trees_;
};

// ============================================================================
// Convenience Functions (called from reorder_types.h)
// ============================================================================

/**
 * @brief Select algorithm family using the database.
 *
 * Called from SelectReorderingWithMode() for MODE_DATABASE.
 */
inline std::string SelectAlgorithmDatabase(
    const CommunityFeatures& feat,
    BenchmarkType bench,
    const std::string& graph_name = "",
    bool verbose = false) {

    auto& db = BenchmarkDatabase::Get();

    if (!db.loaded()) {
        if (verbose) {
            std::cout << "  Database: not loaded, falling back to ORIGINAL\n";
        }
        return "";
    }

    // Convert BenchmarkType enum to string
    // Note: pr_spmv→pr and cc_sv→cc (no separate models/data for those variants)
    static const char* bench_names[] = {
        "generic", "pr", "bfs", "cc", "sssp", "bc", "tc", "pr", "cc"
    };
    std::string bench_str = "generic";
    if (static_cast<int>(bench) >= 0 &&
        static_cast<int>(bench) < static_cast<int>(sizeof(bench_names)/sizeof(bench_names[0]))) {
        bench_str = bench_names[static_cast<int>(bench)];
    }

    std::string family = db.select(feat, graph_name, bench_str, verbose);

    if (verbose) {
        std::cout << "  Database selection: " << family
                  << " (graph=" << (graph_name.empty() ? "<unnamed>" : graph_name)
                  << ", bench=" << bench_str << ")\n";
    }

    return family;
}

/**
 * @brief Get a model tree from the database for DT/hybrid modes.
 *
 * Called from SelectReorderingWithMode() for MODE_DECISION_TREE / MODE_HYBRID.
 * Returns the prediction (family name), or empty string if no model is available.
 *
 * @param feat    Graph community features
 * @param bench   Benchmark type enum
 * @param subdir  "decision_tree" or "hybrid"
 * @param verbose Print selection details
 * @return Family name, or empty string if model not found
 */
inline std::string SelectAlgorithmModelTreeFromDB(
    const CommunityFeatures& feat,
    BenchmarkType bench,
    const std::string& subdir,
    bool verbose = false) {

    auto& db = BenchmarkDatabase::Get();
    if (!db.models_loaded()) return "";

    // Convert BenchmarkType enum to string
    // Note: pr_spmv→pr and cc_sv→cc (no separate models for those variants)
    static const char* bench_names[] = {
        "generic", "pr", "bfs", "cc", "sssp", "bc", "tc", "pr", "cc"
    };
    std::string bench_str = "generic";
    if (static_cast<int>(bench) >= 0 &&
        static_cast<int>(bench) < static_cast<int>(sizeof(bench_names)/sizeof(bench_names[0]))) {
        bench_str = bench_names[static_cast<int>(bench)];
    }

    ModelTree mt = db.get_model_tree(bench_str, subdir);
    if (!mt.loaded) {
        if (verbose) {
            std::cout << "  Database: no " << subdir << "/" << bench_str
                      << " model in adaptive_models.json\n";
        }
        return "";
    }

    std::string family = mt.predict(feat);
    if (verbose) {
        std::cout << "  Database " << subdir << ": " << family
                  << " (bench=" << bench_str << ")\n";
    }
    return family;
}

/**
 * @brief Load perceptron weights from the database.
 *
 * Called from SelectReorderingWithMode() for perceptron modes (0-3).
 * Tries per-benchmark weights first, falls back to averaged weights.
 *
 * @param bench   Benchmark type enum
 * @param out     Output weights map
 * @param verbose Print selection details
 * @return true if weights were loaded from the database
 */
inline bool LoadPerceptronWeightsFromDB(
    BenchmarkType bench,
    std::map<std::string, PerceptronWeights>& out,
    bool verbose = false) {

    auto& db = BenchmarkDatabase::Get();
    if (!db.models_loaded()) return false;

    // Convert BenchmarkType enum to string
    // Note: pr_spmv→pr and cc_sv→cc (no separate weights for those variants)
    static const char* bench_names[] = {
        "generic", "pr", "bfs", "cc", "sssp", "bc", "tc", "pr", "cc"
    };
    std::string bench_str = "";
    if (bench != BENCH_GENERIC &&
        static_cast<int>(bench) >= 0 &&
        static_cast<int>(bench) < static_cast<int>(sizeof(bench_names)/sizeof(bench_names[0]))) {
        bench_str = bench_names[static_cast<int>(bench)];
    }

    bool ok = db.parse_perceptron_weights(bench_str, out);
    if (ok && verbose) {
        std::cout << "  Database perceptron: loaded " << out.size()
                  << " algorithm weights"
                  << (bench_str.empty() ? " (averaged)" : " (bench=" + bench_str + ")")
                  << "\n";
    }
    return ok;
}

/**
 * @brief Append a benchmark result to the centralized database.
 *
 * Called after running a benchmark to update the database in real-time.
 */
inline void AppendBenchmarkResult(
    const std::string& graph_name,
    const std::string& algorithm,
    int algorithm_id,
    const std::string& benchmark,
    double time_seconds,
    double reorder_time,
    int trials,
    int nodes,
    int edges,
    const CommunityFeatures& feat) {

    BenchRecord rec;
    rec.graph = graph_name;
    rec.algorithm = algorithm;
    rec.algorithm_id = algorithm_id;
    rec.benchmark = benchmark;
    rec.time_seconds = time_seconds;
    rec.reorder_time = reorder_time;
    rec.trials = trials;
    rec.nodes = nodes;
    rec.edges = edges;
    rec.success = true;

    auto& db = BenchmarkDatabase::Get();
    db.append_with_features(rec, feat);
    db.save();
}

// ============================================================================
// Streaming Selection — Unified entry point for all modes
// ============================================================================

/// Convert BenchmarkType enum to string (shared helper)
inline std::string BenchTypeToString(BenchmarkType bench) {
    static const char* names[] = {
        "generic", "pr", "bfs", "cc", "sssp", "bc", "tc", "pr", "cc"
    };
    if (static_cast<int>(bench) >= 0 &&
        static_cast<int>(bench) < static_cast<int>(sizeof(names)/sizeof(names[0]))) {
        return names[static_cast<int>(bench)];
    }
    return "generic";
}

/**
 * @brief Streaming mode-aware algorithm selection from the database.
 *
 * This is the main entry point for the streaming database model. All
 * selection modes (fastest reorder, fastest execution, end-to-end,
 * amortization, DT, hybrid, database) are handled by computing
 * predictions directly from the raw benchmark data — no pre-trained
 * model files needed.
 *
 * Called from SelectReorderingWithMode() as the primary selection path.
 * If this returns empty string, the caller falls back to file-based
 * weights (backward compatibility).
 *
 * @param feat       Graph community features
 * @param bench      Benchmark type enum
 * @param graph_name Graph name (for oracle lookup)
 * @param mode       Selection mode
 * @param verbose    Print selection details
 * @return Family name, or empty string if database has no data
 */
inline std::string SelectForMode(
    const CommunityFeatures& feat,
    BenchmarkType bench,
    const std::string& graph_name,
    SelectionMode mode,
    bool verbose = false) {

    auto& db = BenchmarkDatabase::Get();
    if (!db.loaded()) {
        if (verbose) {
            std::cout << "  Database streaming: not loaded\n";
        }
        return "";
    }

    std::string bench_str = BenchTypeToString(bench);
    return db.select_for_mode(feat, graph_name, bench_str, mode, verbose);
}

}  // namespace database
}  // namespace graphbrew
