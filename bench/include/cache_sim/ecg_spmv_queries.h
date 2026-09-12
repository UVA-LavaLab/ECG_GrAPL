#ifndef GRAPHBREW_CACHE_SIM_ECG_SPMV_QUERIES_H
#define GRAPHBREW_CACHE_SIM_ECG_SPMV_QUERIES_H

#include "ecg_algorithm.h"
#include "../ecg_algorithm_main.h"

namespace cache_sim {

inline void writeQueryMetrics(std::ostream& output, const AlgorithmTraffic& traffic,
                              uint64_t llc_bytes, uint64_t llc_ways, uint64_t matrix_stream_lines) {
    output << "{\"total_accesses\":" << traffic.total_accesses
           << ",\"memory_accesses\":" << traffic.memory_accesses
           << ",\"prefetch_fills\":" << traffic.prefetch_fills
           << ",\"llc_writebacks\":" << traffic.llc_writebacks
           << ",\"total_offchip_traffic\":" << traffic.offchip()
           << ",\"popt_matrix_stream_lines_simulated\":" << matrix_stream_lines
           << ",\"L3\":{\"size_bytes\":" << llc_bytes
           << ",\"ways\":" << llc_ways << ",\"line_size\":64,\"hits\":" << traffic.llc_hits
           << ",\"misses\":" << traffic.llc_misses << ",\"prop_hits\":" << traffic.llc_property_hits
           << ",\"prop_misses\":" << traffic.llc_property_misses << "}}";
}

// Synchronous borrow: the caller retains the actual CSR owner until this returns.
inline void runSpmvQueries(const ecg_algorithm::GraphView& graph,
                          const ecg_algorithm::CommandLine& command, CacheHierarchy& cache,
                          std::ostream& output) {
    if (command.queries < 2 || command.queries > 64 || graph.weights || graph.encoded_id_bits ||
        command.options.algorithm != ecg_algorithm::Algorithm::SPMV || command.options.records ||
        command.options.record_model != ecg_algorithm::RecordModel::NEXT ||
        command.options.traversal_preprocessing ||
        command.options.grasp_reference != ecg_algorithm::GraspReferenceMode::OFF ||
        command.options.popt_constant_rank || command.options.grasp_graph_passes ||
        command.options.bfs_traffic_phases || command.options.bfs_direction_optimizing ||
        command.options.window_observer != ecg_algorithm::WindowObserverMode::OFF ||
        !command.options.sources.empty() ||
        (command.policy != "LRU" && command.policy != "SRRIP" &&
         command.policy != "GRASP_PAPER" && command.policy != "POPT_UNCHARGED"))
        throw std::invalid_argument("independent queries require unweighted CSR SpMV baselines");
    if (cache.getTotalAccesses() != 0)
        throw std::invalid_argument("independent SpMV queries require a fresh cache hierarchy");
    const bool popt = command.policy == "POPT_UNCHARGED";
    auto prepared = popt ? std::make_shared<PreparedSpmvMatrix>() : nullptr;
    const AlgorithmTraffic start = AlgorithmTraffic::snapshot(cache);
    AlgorithmTraffic graph_preparation, query_setup, query_kernel;
    const auto beginning = std::chrono::steady_clock::now();
    output << "{\"schema\":\"ecg.spmv-queries.v1\",\"backend\":\"cache_sim\","
           << "\"measurement_scope\":\"shared-graph-batch-data-traffic\","
           << "\"timing_valid_for_speedup\":false,\"graph_storage_reused\":true,"
           << "\"cache_state_preserved\":true,\"properties\":\"fresh-query-local-arrays\","
           << "\"serialized_loading\":\"excluded-as-in-single-query-controls\","
           << "\"query_count\":" << command.queries << ",\"policy\":\"" << command.policy << "\",\"queries\":[";
    for (uint64_t index = 0; index < command.queries; ++index) {
        AlgorithmBackend backend(cache, command.options, command.llc_bytes,
            command.policy == "GRASP_PAPER", popt, prepared);
        const auto query_start = std::chrono::steady_clock::now();
        const uint64_t stream_before = cache.getPoptMatrixStreamLines();
        const auto result = ecg_algorithm::run(graph, command.options, backend);
        const uint64_t stream_after = cache.getPoptMatrixStreamLines();
        if (stream_after < stream_before)
            throw std::logic_error("query matrix-stream counters were reset");
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - query_start).count();
        const auto preparation = backend.preparationTraffic();
        const auto setup = backend.querySetupTraffic();
        const auto kernel = backend.kernelTraffic();
        graph_preparation.accumulate(preparation);
        query_setup.accumulate(setup);
        query_kernel.accumulate(kernel);
        if (index) output << ',';
        output << "{\"schema\":\"ecg.algorithm-result.v1\",\"backend\":\"cache_sim\","
               << "\"mode\":\"csr\",\"policy\":\"" << command.policy
               << "\",\"record_base_policy\":\"LRU\",\"setup_cache_policy\":\"" << (popt ? "LRU" : command.policy)
               << "\",\"grasp_scope\":\"all\",\"diagnostic_only\":false,\"policy_ablation\":false,"
               << "\"timing_valid_for_speedup\":false,"
               << "\"measurement_scope\":\"algorithm-data-traffic-including-construction\","
               << "\"metrics_scope\":\"query-delta-LLC-and-offchip\",\"query_index\":" << index + 1
               << ",\"host_seconds\":" << std::setprecision(12) << seconds << ",\"workload\":";
        ecg_algorithm::writeResult(output, result, command.options);
        output << ",\"metrics\":";
        writeQueryMetrics(output, backend.queryTraffic(), command.llc_bytes, command.llc_ways,
                          stream_after - stream_before);
        output << ",\"traffic_phases\":{\"boundary\":\"first-binding-complete\",\"cache_state_preserved\":true,\"setup\":";
        backend.setupTraffic().write(output);
        output << ",\"kernel\":";
        kernel.write(output);
        output << "},\"graph_preparation\":";
        preparation.write(output);
        output << ",\"query_setup\":";
        setup.write(output);
        output << ",\"cumulative_traffic\":";
        AlgorithmTraffic::snapshot(cache).since(start).write(output);
        output << ",\"popt\":";
        backend.writePopt(output);
        output << '}';
    }
    output << "],\"matrix_constructions\":" << (prepared ? prepared->constructions() : 0)
           << ",\"shared_owner_reservation_bytes\":" << (prepared ? PreparedSpmvMatrix::kOwnerReservation : 0)
           << ",\"shared_matrix_bytes\":" << (prepared ? prepared->bytes() : 0) << ",\"graph_preparation\":";
    graph_preparation.write(output);
    output << ",\"query_setup\":";
    query_setup.write(output);
    output << ",\"query_kernel\":";
    query_kernel.write(output);
    output << ",\"total_traffic\":";
    AlgorithmTraffic::snapshot(cache).since(start).write(output);
    output << ",\"metrics\":" << cache.toJSON() << ",\"host_seconds\":"
           << std::chrono::duration<double>(std::chrono::steady_clock::now() - beginning).count() << "}\n";
}

} // namespace cache_sim

#endif
