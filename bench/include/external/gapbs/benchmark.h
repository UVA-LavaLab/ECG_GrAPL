// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef BENCHMARK_H_
#define BENCHMARK_H_

#include <algorithm>
#include <cinttypes>
#include <functional>
#include <parallel/algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "builder.h"
#include "graph.h"
#include "timer.h"
#include "util.h"
#include "writer.h"
#include "graphbrew/reorder/reorder_database.h"

/*
   GAP Benchmark Suite
   File:   Benchmark
   Author: Scott Beamer

   Various helper functions to ease writing of kernels
 */

// Default type signatures for commonly used types
typedef int32_t NodeID;
typedef int32_t WeightT;
typedef NodeWeight<NodeID, WeightT> WNode;

typedef CSRGraph<NodeID> Graph;
typedef CSRGraph<NodeID, WNode> WGraph;

typedef BuilderBase<NodeID, NodeID, WeightT> Builder;
typedef BuilderBase<NodeID, WNode, WeightT> WeightedBuilder;

typedef WriterBase<NodeID, NodeID, WeightT> Writer;
typedef WriterBase<NodeID, WNode, WeightT> WeightedWriter;

// New type definitions for PartitionedGraph
typedef std::vector<Graph> PGraph;
typedef std::vector<WGraph> PWGraph;

typedef CSRGraphFlat<NodeID> FlatGraph;
typedef std::vector<FlatGraph> PFlatGraph;

// Used to pick random non-zero degree starting points for search algorithms
template <typename GraphT_> class SourcePicker
{
public:
    explicit SourcePicker(const GraphT_ &g, NodeID given_source = -1, int num_sources = 0)
        : given_source_(given_source), rng_(kRandSeed),
          udist_(g.num_nodes() - 1, rng_), g_(g), top_nodes_index_(0),
          source_index_(0)
    {
        // g_.copy_org_ids(g.org_ids_shared_);
        // Initialize top 100 nodes
        initializeTopNodes();
        
        // Pre-generate consistent source list if requested
        if (num_sources > 0 && given_source == -1)
        {
            GenerateConsistentSources(num_sources);
        }
    }

    // Generate a list of ORIGINAL vertex IDs to use as sources
    // This ensures all orderings explore the same vertices
    // NOTE: These are stored as INTERNAL IDs for the current graph,
    // which will be the same as original IDs for Original ordering,
    // and will be translated for reorderings
    void GenerateConsistentSources(int count)
    {
        consistent_sources_.clear();
        consistent_sources_.reserve(count);
        
        // Use a fresh RNG with fixed seed for reproducibility
        // Generate internal IDs and store the ORIGINAL IDs
        std::mt19937_64 src_rng(kRandSeed);
        UniDist<NodeID, std::mt19937_64> src_dist(g_.num_nodes() - 1, src_rng);
        
        while (static_cast<int>(consistent_sources_.size()) < count)
        {
            // Generate a random number in [0, num_nodes-1]
            // This will be used as an ORIGINAL vertex ID
            NodeID candidate_original = src_dist();
            
            // Find the internal ID for this original vertex
            NodeID internal_id = g_.get_internal_id(candidate_original);
            
            // Check if this vertex has non-zero degree
            if (g_.out_degree(internal_id) > 0)
            {
                // Store the ORIGINAL ID - we'll translate when picking
                consistent_sources_.push_back(candidate_original);
            }
        }
    }

    NodeID PickNextRand()
    {
        if (given_source_ != -1)
        {
            // given_source_ is an ORIGINAL vertex ID from user (-r flag)
            // We need to find its internal ID so BFS operates on the correct vertex
            NodeID internal_id = g_.get_internal_id(given_source_);
            return internal_id;
        }
        
        // If we have pre-generated consistent sources, use them
        if (!consistent_sources_.empty() && source_index_ < consistent_sources_.size())
        {
            NodeID original_id = consistent_sources_[source_index_++];
            NodeID internal_id = g_.get_internal_id(original_id);
            return internal_id;
        }
        
        // Fallback to random (for backward compatibility)
        NodeID source;
        NodeID real_source;
        do
        {
            source = udist_();
            real_source = g_.get_org_id(source);
            // std::cout << "source : " << source << " real_source : " << real_source << "out_degree : " << (NodeID)g_.in_degree(real_source) << std::endl;
        }
        while (g_.out_degree(real_source) == 0);
        return real_source;
    }

    NodeID PickNextTop()
    {
        if (given_source_ != -1)
            return given_source_;

        while (top_nodes_index_ < top_nodes_.size())
        {
            NodeID real_source = top_nodes_[top_nodes_index_++];
            if (g_.out_degree(real_source) > 0)
            {
                // std::cout << " real_source : " << real_source
                //           << " out_degree : " << (NodeID)g_.in_degree(real_source)
                //           << std::endl;
                return real_source;
            }
        }

        // If all top nodes have been checked and no valid node is found, fallback
        // to random pick
        return PickNextRand();
    }

    NodeID PickNext()
    {
        return PickNextRand();
    }

    void initializeTopNodes()
    {
        // Logic to determine the top 100 nodes. This is just an example.
        std::vector<std::pair<NodeID, NodeID>> nodes_with_degrees(g_.num_nodes());
        top_nodes_.resize(g_.num_nodes());
        #pragma omp parallel for
        for (NodeID i = 0; i < g_.num_nodes(); ++i)
        {
            nodes_with_degrees[i] = std::make_pair(i, g_.out_degree(i));
        }
        // Sort nodes by degree in descending order
        __gnu_parallel::sort(
            nodes_with_degrees.begin(), nodes_with_degrees.end(),
            [](const std::pair<NodeID, NodeID> &a,
               const std::pair<NodeID, NodeID> &b)
        {
            return b.second < a.second;
        });
        // Select the top nodes
        #pragma omp parallel for
        for (size_t i = 0; i < top_nodes_.size(); ++i)
        {
            top_nodes_[i] = nodes_with_degrees[i].first;
        }
    }

private:
    NodeID given_source_;
    std::mt19937_64 rng_;
    UniDist<NodeID, std::mt19937_64> udist_;
    const GraphT_ &g_;
    size_t top_nodes_index_;        // To iterate through the top nodes
    std::vector<NodeID> top_nodes_; // Store the top 100 nodes
    std::vector<NodeID> consistent_sources_; // Pre-generated ORIGINAL source IDs
    size_t source_index_;           // Current index in consistent_sources_
};

// Returns k pairs with the largest values from list of key-value pairs
template <typename KeyT, typename ValT>
std::vector<std::pair<ValT, KeyT>>
                                TopK(const std::vector<std::pair<KeyT, ValT>> &to_sort, size_t k)
{
    std::vector<std::pair<ValT, KeyT>> top_k;
    ValT min_so_far = 0;
    for (auto kvp : to_sort)
    {
        if ((top_k.size() < k) || (kvp.second > min_so_far))
        {
            top_k.push_back(std::make_pair(kvp.second, kvp.first));
            __gnu_parallel::stable_sort(top_k.begin(), top_k.end(),
                                        std::greater<std::pair<ValT, KeyT>>());
            if (top_k.size() > k)
                top_k.resize(k);
            min_so_far = top_k.back().first;
        }
    }
    return top_k;
}

bool VerifyUnimplemented(...)
{
    std::cout << "** verify unimplemented **" << std::endl;
    return false;
}

// Calls (and times) kernel according to command line arguments
template <typename GraphT_, typename GraphFunc, typename AnalysisFunc,
          typename VerifierFunc>
void BenchmarkKernel(const CLApp &cli, const GraphT_ &g, GraphFunc kernel,
                     AnalysisFunc stats, VerifierFunc verify)
{
    g.PrintStats();
    double total_seconds = 0;
    Timer trial_timer;
    for (int iter = 0; iter < cli.num_trials(); iter++)
    {
        trial_timer.Start();
        auto result = kernel(g);
        trial_timer.Stop();
        PrintTime("Trial Time", trial_timer.Seconds());
        total_seconds += trial_timer.Seconds();
        if (cli.do_analysis() && (iter == (cli.num_trials() - 1)))
            stats(g, result);
        if (cli.do_verify())
        {
            trial_timer.Start();
            PrintLabel("Verification",
                       verify(std::ref(g), std::ref(result)) ? "PASS" : "FAIL");
            trial_timer.Stop();
            PrintTime("Verification Time", trial_timer.Seconds());
        }
    }
    PrintTime("Average Time", total_seconds / cli.num_trials());
}

// ============================================================================
// Self-Recording BenchmarkKernel (v2.1)
// ============================================================================

/// Extract a clean graph name from a file path.
/// "/path/to/graphs/amazon/amazon.sg" → "amazon"
/// "/data/web-Google.sg" → "web-Google"
inline std::string ExtractGraphName(const std::string& filepath) {
    // Find the last path separator
    std::string base = filepath;
    auto slash = base.rfind('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    // Strip extension (.sg, .el, .wel, .mtx, etc.)
    auto dot = base.rfind('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);
    return base;
}

/// Self-recording BenchmarkKernel overload.
///
/// In addition to timing and verifying, this overload:
///  1. Captures per-trial TrialResult (time, verification, answer data)
///  2. Constructs a RunReport after the trial loop
///  3. Auto-saves to benchmarks.json if self-recording is enabled
///
/// @param benchmark_name    Short name: "pr", "bfs", "cc", "sssp", "bc", "tc"
/// @param result_extractor  Lambda (graph, result) → json with answer fields
template <typename GraphT_, typename GraphFunc, typename AnalysisFunc,
          typename VerifierFunc, typename ResultExtractor>
void BenchmarkKernel(const CLApp &cli, const GraphT_ &g, GraphFunc kernel,
                     AnalysisFunc stats, VerifierFunc verify,
                     const std::string& benchmark_name,
                     ResultExtractor result_extractor)
{
    using namespace graphbrew::database;

    g.PrintStats();
    double total_seconds = 0;
    Timer trial_timer;

    // Collect per-trial results
    std::vector<TrialResult> trial_results;
    trial_results.reserve(cli.num_trials());

    for (int iter = 0; iter < cli.num_trials(); iter++)
    {
        ClearBenchmarkIterationLog();   // reset per-iteration log for this trial
        trial_timer.Start();
        auto result = kernel(g);
        trial_timer.Stop();
        PrintTime("Trial Time", trial_timer.Seconds());
        total_seconds += trial_timer.Seconds();

        // Build TrialResult
        TrialResult tr;
        tr.trial_id = iter;
        tr.time_seconds = trial_timer.Seconds();

        // Extract benchmark-specific answer data
        try {
            tr.answer = result_extractor(g, result);
        } catch (...) {
            tr.answer = nlohmann::json::object();
        }

        // Attach per-iteration granular data collected by the kernel
        auto& iter_log = GetBenchmarkIterationLog();
        if (!iter_log.empty()) {
            tr.answer["iterations"] = iter_log;
        }

        if (cli.do_analysis() && (iter == (cli.num_trials() - 1)))
            stats(g, result);
        if (cli.do_verify())
        {
            trial_timer.Start();
            bool passed = verify(std::ref(g), std::ref(result));
            PrintLabel("Verification", passed ? "PASS" : "FAIL");
            trial_timer.Stop();
            PrintTime("Verification Time", trial_timer.Seconds());
            tr.verified = passed;
        }

        trial_results.push_back(std::move(tr));
    }

    double avg_time = total_seconds / cli.num_trials();
    PrintTime("Average Time", avg_time);

    // ---- Self-recording: build and save RunReport ----
    if (SelfRecordingEnabled()) {
        RunReport report;
        report.graph_name   = ExtractGraphName(cli.filename());
        report.algorithm    = GetReorderAlgoHint();
        report.algorithm_id = GetReorderAlgoIdHint();
        report.benchmark    = benchmark_name;
        report.avg_time     = avg_time;
        report.reorder_time = GetReorderTimeHint();
        report.num_trials   = cli.num_trials();
        report.trials       = std::move(trial_results);
        report.reorder_metas = GetReorderMetaHints();  // accumulated reorder metadata
        report.nodes        = g.num_nodes();
        report.edges        = g.num_edges_directed();
        report.success      = true;

        // If no algorithm hint was set, default to "Original"
        if (report.algorithm.empty()) {
            report.algorithm = "Original";
        }

        BenchmarkDatabase::Get().append_run(report);
    }
}

#endif // BENCHMARK_H_
