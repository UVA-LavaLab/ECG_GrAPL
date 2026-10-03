#ifndef GRAPHBREW_ECG_ALGORITHM_MAIN_H
#define GRAPHBREW_ECG_ALGORITHM_MAIN_H

#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>

#include "ecg_algorithms.h"
#include "graph.h"
#include "reader.h"

namespace ecg_algorithm {

struct CommandLine {
    Options options;
    std::string graph_path;
    std::string output_path;
    std::string policy = "LRU";
    uint64_t queries = 1;
    uint64_t maximum_graph_bytes = uint64_t{512} << 20;
    uint64_t l1_bytes = 32 * 1024, l2_bytes = 256 * 1024, llc_bytes = 8 * 1024 * 1024;
    uint64_t l1_ways = 8, l2_ways = 4, llc_ways = 16;
};

inline uint64_t unsignedOption(const std::string& text) {
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("expected-unsigned-option: " + text);
    return value;
}

inline CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine command;
    bool algorithm_seen = false;
    bool window_floor_seen = false;
    bool frontier_gating_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--evidence") {
            command.options.evidence = true;
            continue;
        }
        if (argument == "--values") {
            command.options.capture_values = true;
            continue;
        }
        if (index + 1 == argc)
            throw std::invalid_argument("missing-option-value: " + argument);
        const std::string value(argv[++index]);
        if (argument == "--graph") command.graph_path = value;
        else if (argument == "--output") command.output_path = value;
        else if (argument == "--policy") {
            if (value != "LRU" && value != "SRRIP" && value != "GRASP_PAPER" && value != "POPT_UNCHARGED")
                throw std::invalid_argument("unsupported-current-algorithm-baseline");
            command.policy = value;
        } else if (argument == "--algorithm") {
            algorithm_seen = false;
            for (Algorithm candidate : {Algorithm::SPMV, Algorithm::BFS, Algorithm::SSSP,
                                        Algorithm::CC, Algorithm::BC, Algorithm::TC}) {
                if (value == name(candidate)) {
                    command.options.algorithm = candidate;
                    algorithm_seen = true;
                    break;
                }
            }
            if (!algorithm_seen)
                throw std::invalid_argument("unknown-algorithm: " + value);
        } else if (argument == "--mode") {
            command.options.records = value != "csr";
            if (command.options.records && ecg_record::parseMechanismName(
                    value.c_str(), command.options.mechanism) != ecg_record::Status::OK)
                throw std::invalid_argument("unknown-current-mechanism");
        } else if (argument == "--record-base-policy") {
            if (value == "LRU")
                command.options.record_base_policy = RecordBasePolicy::LRU;
            else if (value == "GRASP_PAPER")
                command.options.record_base_policy = RecordBasePolicy::GRASP_PAPER;
            else
                throw std::invalid_argument("record-base-policy-must-be-LRU-or-GRASP_PAPER");
        } else if (argument == "--record-bytes") {
            const uint64_t bytes = unsignedOption(value);
            if (bytes != 0 && bytes != 4 && bytes != 8)
                throw std::invalid_argument("record-bytes-must-be-0-4-or-8");
            command.options.record_bytes = static_cast<uint8_t>(bytes);
        } else if (argument == "--record-preprocess") {
            if (value != "csr" && value != "traversal")
                throw std::invalid_argument("record-preprocess-must-be-csr-or-traversal");
            command.options.traversal_preprocessing = value == "traversal";
        } else if (argument == "--record-model") {
            if (value == "next") command.options.record_model = RecordModel::NEXT;
            else if (value == "window") command.options.record_model = RecordModel::WINDOW;
            else if (value == "frontier") command.options.record_model = RecordModel::FRONTIER;
            else throw std::invalid_argument("record-model-must-be-next-window-or-frontier");
        } else if (argument == "--frontier-gating") {
            if (value != "enabled" && value != "ignored")
                throw std::invalid_argument("frontier-gating-must-be-enabled-or-ignored");
            command.options.frontier_gating = value == "enabled";
            frontier_gating_seen = true;
        } else if (argument == "--popt-rank-mode") {
            if (value != "future" && value != "constant")
                throw std::invalid_argument("popt-rank-mode-must-be-future-or-constant");
            command.options.popt_constant_rank = value == "constant";
        } else if (argument == "--record-expiry-clock") {
            if (value != "progress" && value != "delivery")
                throw std::invalid_argument("record-expiry-clock-must-be-progress-or-delivery");
            command.options.record_delivered_expiry_clock = value == "delivery";
        } else if (argument == "--record-store-bound") {
            if (value != "keep" && value != "drop")
                throw std::invalid_argument("record-store-bound-must-be-keep-or-drop");
            command.options.record_store_keeps_bound = value == "keep";
        } else if (argument == "--record-pressure-gate") {
            if (value == "no") command.options.record_pressure_gate = RecordPressureGate::NO;
            else if (value == "on") command.options.record_pressure_gate = RecordPressureGate::COUNTER;
            else if (value == "duel") command.options.record_pressure_gate = RecordPressureGate::DUEL;
            else throw std::invalid_argument("record-pressure-gate-must-be-no-on-or-duel");
        } else if (argument == "--record-governed-first") {
            if (value != "on" && value != "no")
                throw std::invalid_argument("record-governed-first-must-be-on-or-no");
            command.options.record_governed_first = value == "on";
        } else if (argument == "--record-rrpv-order") {
            if (value != "on" && value != "no")
                throw std::invalid_argument("record-rrpv-order-must-be-on-or-no");
            command.options.record_rrpv_order = value == "on";
        } else if (argument == "--record-uninformed-base") {
            if (value != "on" && value != "no")
                throw std::invalid_argument("record-uninformed-base-must-be-on-or-no");
            command.options.record_uninformed_base = value == "on";
        } else if (argument == "--record-bound-compare") {
            if (value != "on" && value != "no")
                throw std::invalid_argument("record-bound-compare-must-be-on-or-no");
            command.options.record_bound_compare = value == "on";
        } else if (argument == "--grasp-reference") {
            if (value == "off") command.options.grasp_reference = GraspReferenceMode::OFF;
            else if (value == "full") command.options.grasp_reference = GraspReferenceMode::FULL;
            else if (value == "flat") command.options.grasp_reference = GraspReferenceMode::FLAT;
            else if (value == "rank") command.options.grasp_reference = GraspReferenceMode::RANK;
            else throw std::invalid_argument("grasp-reference-must-be-off-full-flat-or-rank");
        } else if (argument == "--grasp-scope") {
            if (value != "all" && value != "graph-passes")
                throw std::invalid_argument("grasp-scope-must-be-all-or-graph-passes");
            command.options.grasp_graph_passes = value == "graph-passes";
        } else if (argument == "--bfs-traffic-phases") {
            if (value != "on" && value != "off")
                throw std::invalid_argument("bfs-traffic-phases-must-be-on-or-off");
            command.options.bfs_traffic_phases = value == "on";
        } else if (argument == "--window-candidate-rrpv") {
            const uint64_t rank = unsignedOption(value);
            if (rank != 6 && rank != 7)
                throw std::invalid_argument("window-candidate-rrpv-must-be-6-or-7");
            command.options.window_candidate_rrpv = static_cast<uint8_t>(rank);
            window_floor_seen = true;
        } else if (argument == "--window-observer") {
            if (value == "off") command.options.window_observer = WindowObserverMode::OFF;
            else if (value == "control") command.options.window_observer = WindowObserverMode::CONTROL;
            else if (value == "window") command.options.window_observer = WindowObserverMode::WINDOW;
            else throw std::invalid_argument("window-observer-must-be-off-control-or-window");
        } else if (argument == "--window-observer-bytes") {
            command.options.maximum_window_observer_bytes = unsignedOption(value);
        } else if (argument == "--source") {
            const uint64_t source = unsignedOption(value);
            if (source > INT32_MAX)
                throw std::invalid_argument("source-exceeds-node-id");
            command.options.source = static_cast<uint32_t>(source);
        } else if (argument == "--minimum-mantissa-bits") {
            const uint64_t bits = unsignedOption(value);
            if (bits > 61)
                throw std::invalid_argument("invalid-mantissa-floor");
            command.options.minimum_mantissa_bits = static_cast<uint8_t>(bits);
        } else if (argument == "--sources") {
            if (!command.options.sources.empty())
                throw std::invalid_argument("duplicate-source-list");
            std::size_t first = 0;
            for (;;) {
                const std::size_t comma = value.find(',', first);
                const uint64_t source = unsignedOption(value.substr(first,
                    comma == std::string::npos ? comma : comma - first));
                if (source > INT32_MAX)
                    throw std::invalid_argument("source-exceeds-node-id");
                command.options.sources.push_back(static_cast<uint32_t>(source));
                if (comma == std::string::npos)
                    break;
                first = comma + 1;
            }
        } else if (argument == "--queries") command.queries = unsignedOption(value);
        else if (argument == "--repeat") command.options.repetitions = unsignedOption(value);
        else if (argument == "--delta") command.options.delta = unsignedOption(value);
        else if (argument == "--bfs-direction") {
            if (value != "td" && value != "do")
                throw std::invalid_argument("bfs-direction-must-be-td-or-do");
            command.options.bfs_direction_optimizing = value == "do";
        } else if (argument == "--bfs-alpha") command.options.bfs_alpha = unsignedOption(value);
        else if (argument == "--bfs-beta") command.options.bfs_beta = unsignedOption(value);
        else if (argument == "--max-passes") command.options.maximum_passes = unsignedOption(value);
        else if (argument == "--workspace-bytes") command.options.maximum_workspace_bytes = unsignedOption(value);
        else if (argument == "--graph-bytes") command.maximum_graph_bytes = unsignedOption(value);
        else if (argument == "--carrier-bytes") command.options.build_limits.maximum_carrier_bytes = unsignedOption(value);
        else if (argument == "--auxiliary-bytes") command.options.build_limits.maximum_auxiliary_bytes = unsignedOption(value);
        else if (argument == "--l1-bytes") command.l1_bytes = unsignedOption(value);
        else if (argument == "--l2-bytes") command.l2_bytes = unsignedOption(value);
        else if (argument == "--llc-bytes") command.llc_bytes = unsignedOption(value);
        else if (argument == "--l1-ways") command.l1_ways = unsignedOption(value);
        else if (argument == "--l2-ways") command.l2_ways = unsignedOption(value);
        else if (argument == "--llc-ways") command.llc_ways = unsignedOption(value);
        else throw std::invalid_argument("unknown-option: " + argument);
    }
    if (!algorithm_seen || command.graph_path.empty())
        throw std::invalid_argument("required: --algorithm spmv|bfs|sssp|cc|bc|tc --graph path.sg|path.wsg");
    if (!command.queries || command.queries > 64)
        throw std::invalid_argument("queries-must-be-1-through-64");
    if (command.queries > 1 && (command.options.algorithm != Algorithm::SPMV || command.options.records ||
        command.options.record_model != RecordModel::NEXT || command.options.traversal_preprocessing ||
        command.options.grasp_reference != GraspReferenceMode::OFF || command.options.popt_constant_rank ||
        command.options.grasp_graph_passes || command.options.bfs_traffic_phases ||
        command.options.window_observer != WindowObserverMode::OFF ||
        command.options.bfs_direction_optimizing || !command.options.sources.empty()))
        throw std::invalid_argument("independent queries require unmodified CSR SpMV baselines");
    if (command.options.grasp_reference != GraspReferenceMode::OFF &&
        (command.policy != "GRASP_PAPER" || command.options.algorithm != Algorithm::SPMV ||
         command.options.records || command.options.popt_constant_rank ||
         command.options.record_model != RecordModel::NEXT || command.options.traversal_preprocessing ||
         command.options.bfs_direction_optimizing || command.options.grasp_graph_passes ||
         command.options.bfs_traffic_phases || command.options.window_observer != WindowObserverMode::OFF ||
         !command.options.sources.empty()))
        throw std::invalid_argument("GRASP reference diagnostic requires CSR SpMV with ordinary GRASP_PAPER");
    if (command.options.record_delivered_expiry_clock &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT))
        throw std::invalid_argument("the delivered expiry clock requires the current NEXT record model");
    if (command.options.record_store_keeps_bound &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT))
        throw std::invalid_argument("store-bound retention requires the current NEXT record model");
    if (command.options.record_pressure_gate != RecordPressureGate::NO &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT ||
         (command.options.mechanism != ecg_record::Mechanism::REPLACEMENT &&
          command.options.mechanism != ecg_record::Mechanism::REPLACEMENT_PREFETCH)))
        throw std::invalid_argument(
            "pressure-gate requires the current NEXT record replacement rule");
    // The duel counts the transfers the LLC itself sees: demand misses and
    // dirty victims. Prefetch fills are counted by the hierarchy at issue, so
    // with a prefetcher the selector would train on a different objective.
    if (command.options.record_pressure_gate == RecordPressureGate::DUEL &&
        command.options.mechanism != ecg_record::Mechanism::REPLACEMENT)
        throw std::invalid_argument(
            "pressure-gate duel requires the replacement mechanism without prefetch");
    if (command.options.record_governed_first &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT ||
         (command.options.mechanism != ecg_record::Mechanism::REPLACEMENT &&
          command.options.mechanism != ecg_record::Mechanism::REPLACEMENT_PREFETCH)))
        throw std::invalid_argument("governed-first requires the current NEXT record replacement rule");
    // Prefetch admission still asks the base victim, and only the GRASP_PAPER
    // base keeps the RRPV state the order ranks by.
    if (command.options.record_rrpv_order &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT ||
         command.options.mechanism != ecg_record::Mechanism::REPLACEMENT ||
         command.options.record_base_policy != RecordBasePolicy::GRASP_PAPER))
        throw std::invalid_argument(
            "rrpv-order requires the current NEXT record replacement rule over GRASP_PAPER");
    // Prefetch admission runs the default rule, so neither control can act there.
    if ((command.options.record_uninformed_base || !command.options.record_bound_compare) &&
        (!command.options.records || command.options.record_model != RecordModel::NEXT ||
         command.options.mechanism != ecg_record::Mechanism::REPLACEMENT))
        throw std::invalid_argument(
            "record victim controls require the current NEXT record replacement rule without prefetch");
    if (command.options.records && command.policy != "LRU")
        throw std::invalid_argument("current-record-modes-own-their-replacement-policy");
    if (!command.options.records &&
        command.options.record_base_policy != RecordBasePolicy::LRU)
        throw std::invalid_argument("record-base-policy-requires-record-mode");
    if (command.options.popt_constant_rank &&
        (command.policy != "POPT_UNCHARGED" || command.options.records ||
         (command.options.algorithm != Algorithm::SPMV && command.options.algorithm != Algorithm::BFS) ||
         command.options.bfs_direction_optimizing))
        throw std::invalid_argument("constant P-OPT ranks require CSR SpMV or TD BFS with POPT_UNCHARGED");
    if ((command.options.bfs_traffic_phases || command.options.grasp_graph_passes) &&
        (command.options.algorithm != Algorithm::BFS ||
         (command.options.records && (command.options.bfs_traffic_phases || command.options.record_model == RecordModel::NEXT)) ||
         command.options.bfs_direction_optimizing || command.options.window_observer != WindowObserverMode::OFF ||
         (command.options.grasp_graph_passes && !command.options.records && command.policy != "GRASP_PAPER")))
        throw std::invalid_argument("BFS phase controls require TD BFS and a compatible baseline/model");
    if (window_floor_seen && command.options.record_model != RecordModel::WINDOW)
        throw std::invalid_argument("window-candidate-rrpv-requires-window-model");
    if (frontier_gating_seen && command.options.record_model != RecordModel::FRONTIER)
        throw std::invalid_argument("frontier-gating-requires-frontier-model");
    if (command.options.record_model == RecordModel::FRONTIER && !command.options.grasp_graph_passes)
        throw std::invalid_argument("frontier model requires graph-pass GRASP scope");
    if (command.options.record_model == RecordModel::FRONTIER &&
        (!command.options.sources.empty() || command.options.repetitions != 1))
        throw std::invalid_argument("frontier model requires a single BFS source and query");
    if (command.options.record_model != RecordModel::NEXT &&
        (command.options.algorithm != Algorithm::BFS || !command.options.records ||
         command.options.record_base_policy != RecordBasePolicy::GRASP_PAPER ||
         command.options.bfs_direction_optimizing || command.options.traversal_preprocessing ||
         command.options.minimum_mantissa_bits || command.options.window_observer != WindowObserverMode::OFF ||
         (command.options.mechanism != ecg_record::Mechanism::TRANSPORT &&
          command.options.mechanism != ecg_record::Mechanism::REPLACEMENT)))
        throw std::invalid_argument("window model requires TD BFS, GRASP base and T/R only");
    if (command.options.window_observer != WindowObserverMode::OFF &&
        (command.options.algorithm != Algorithm::BFS || command.options.records ||
         command.policy != "GRASP_PAPER" || command.options.bfs_direction_optimizing ||
         command.options.traversal_preprocessing))
        throw std::invalid_argument("window observer requires CSR TD BFS and unchanged GRASP_PAPER");
    for (const auto& geometry : {
            std::pair<uint64_t, uint64_t>{command.l1_bytes, command.l1_ways},
            {command.l2_bytes, command.l2_ways}, {command.llc_bytes, command.llc_ways}}) {
        if (geometry.second == 0 || geometry.second > 64 || geometry.first == 0 ||
            geometry.first % (64 * geometry.second) != 0)
            throw std::invalid_argument("invalid-cache-geometry");
    }
    return command;
}

inline void writeHash(std::ostream& output, uint64_t value) {
    const auto flags = output.flags();
    const auto fill = output.fill();
    output << '"' << std::hex << std::setfill('0') << std::setw(16) << value << '"';
    output.flags(flags);
    output.fill(fill);
}

inline void writeResult(std::ostream& output, const Result& result, const Options& options) {
    const bool exact = result.algorithm == Algorithm::SPMV || result.algorithm == Algorithm::TC;
    const bool window = options.record_model == RecordModel::WINDOW;
    const bool frontier = options.record_model == RecordModel::FRONTIER;
    const bool potential = window || frontier;
    output << "{\"schema\":\"ecg.algorithm-workload.v1\",\"algorithm\":\"" << name(result.algorithm)
           << "\",\"variant\":\"" << (options.bfs_direction_optimizing
                ? "sorted-direction-optimizing-td-records-bu-bitmap" : variant(result.algorithm))
           << "\",\"prediction_semantics\":\""
           << (frontier ? "consumer-cohort-potential-read" : window ? "source-cohort-potential-read" :
               exact ? "dense-actual-designated-read" : "next-potential-designated-read")
           << "\",\"carrier\":\"" << (result.records ? "record" : "csr")
           << "\",\"record_base_policy\":\""
           << recordBasePolicyName(options.record_base_policy)
           << "\",\"record_model\":\"" << (frontier ? "frontier" : window ? "window" : "next")
           << "\",\"record_preprocess\":\"" << (options.traversal_preprocessing ? "traversal" : "csr")
           << "\",\"record_reuse_scope\":\"" << recordReuseScope(options)
           << "\",\"record_victim_order\":\""
           << (options.record_governed_first ? "governed-first" : "base-first")
           << "\",\"record_rrpv_order\":\"" << (options.record_rrpv_order ? "on" : "no")
           << "\",\"record_uninformed_base\":\"" << (options.record_uninformed_base ? "on" : "no")
           << "\",\"record_bound_compare\":\"" << (options.record_bound_compare ? "on" : "no")
           << "\",\"record_pressure_gate\":\""
           << recordPressureGateName(options.record_pressure_gate)
           << "\",\"record_store_bound\":\""
           << (options.record_store_keeps_bound ? "keep" : "drop")
           << "\",\"record_expiry_clock\":\""
           << (options.record_delivered_expiry_clock ? "delivery" : "progress") << '"';
    const auto field = [&](const char* key, uint64_t value) { output << ",\"" << key << "\":" << value; };
    field("weighted", result.weighted);
    field("evidence", result.evidence);
    field("memory_counts_measured", result.memory_counts_measured);
    field("vertices", result.vertices);
    field("source_edges", result.source_edges);
    field("carrier_records", result.carrier_records);
    field("passes", result.passes);
    field("structural_positions", result.structural_positions);
    field("actual_records", result.actual_records);
    field("skipped_positions", result.skipped_positions);
    field("record_inspections", result.record_inspections);
    field("record_inspection_bytes", result.record_inspection_bytes);
    field("csr_index_reads", result.csr_index_reads);
    field("edge_reads", result.edge_reads);
    field("weight_reads", result.weight_reads);
    field("ordinary_property_reads", result.ordinary_property_reads);
    field("property_writes", result.property_writes);
    field("auxiliary_accesses", result.auxiliary_accesses);
    field("construction_read_bytes", result.construction_reads);
    field("construction_write_bytes", result.construction_writes);
    field("carrier_allocation_bytes", result.carrier_bytes);
    field("record_weight_bytes", result.record_weight_bytes);
    field("construction_auxiliary_peak_bytes", result.construction_auxiliary_peak_bytes);
    field("constructed_finite_records", result.constructed_finite_records);
    field("constructed_wrap_records", result.constructed_wrap_records);
    field("constructed_unknown_records", result.constructed_unknown_records);
    field("workspace_peak_bytes", result.workspace_peak_bytes);
    field("source", options.source);
    field("source_count", options.sources.empty() ? 1 : options.sources.size());
    field("repetitions", options.repetitions);
    field("delta", options.delta);
    if (result.algorithm == Algorithm::BFS) {
        output << ",\"bfs_direction\":\"" << (options.bfs_direction_optimizing ? "do" : "td")
               << "\",\"bfs_bu_transport\":\"ordinary-bitmap\"";
        field("bfs_alpha", options.bfs_alpha);
        field("bfs_beta", options.bfs_beta);
        field("bfs_td_levels", result.bfs_td_levels);
        field("bfs_bu_levels", result.bfs_bu_levels);
        field("bfs_td_edges", result.bfs_td_edges);
        field("bfs_bu_edges", result.bfs_bu_edges);
        field("bfs_bu_vertices", result.bfs_bu_vertices);
        field("bfs_frontier_peak", result.bfs_frontier_peak);
        field("bfs_direction_switches", result.bfs_direction_switches);
    }
    field("reached", result.reached);
    field("levels", result.levels);
    field("components", result.components);
    field("relax_attempts", result.relax_attempts);
    field("relax_successes", result.relax_successes);
    field("light_passes", result.light_passes);
    field("heavy_passes", result.heavy_passes);
    field("sigma_max", result.sigma_max);
    field("triangles", result.triangles);
    field("oriented_edges", result.oriented_edges);
    field("intersection_comparisons", result.intersection_comparisons);
    field("bindings", result.bindings);
    field("maximum_encoded_id", result.maximum_encoded_id);
    field("record_bytes", potential ? result.window_layout.record_bytes : result.layout.record_bytes);
    field("id_bits", potential ? result.window_layout.id_bits : result.layout.id_bits);
    field("metadata_bits", potential ? result.window_layout.metadata_bits : result.layout.metadata_bits);
    field("mantissa_bits", potential ? 0 : result.layout.mantissa_bits);
    if (window) {
        field("window_token_bits", ecg_window::Layout::token_bits);
        field("window_known_records", result.window_known_records);
        field("window_candidate_rrpv", options.window_candidate_rrpv);
    }
    if (frontier) {
        field("frontier_token_bits", ecg_frontier::Layout::token_bits);
        field("frontier_known_records", result.window_known_records);
        output << ",\"frontier_carrier_digest\":";
        writeHash(output, result.frontier_carrier_digest);
    }
    for (const auto& entry : {
            std::pair<const char*, uint64_t>{"result_digest", result.result_digest},
            {"work_trace_digest", result.work_digest}, {"position_trace_digest", result.position_digest},
            {"record_trace_digest", result.record_digest},
            {"source_list_digest", result.source_list_digest}}) {
        output << ",\"" << entry.first << "\":";
        writeHash(output, entry.second);
    }
    const auto values = [&](const char* key, const auto& elements) {
        output << ",\"" << key << "\":[";
        bool first = true;
        for (const auto value : elements) {
            if (!first) output << ',';
            output << value;
            first = false;
        }
        output << ']';
    };
    if (options.capture_values) {
        values("values_u32", result.values_u32);
        values("values_u64", result.values_u64);
        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        values("values_f32", result.values_f32);
    }
    output << '}';
}

template<class Graph, class Invoke>
int invokeGraph(const Graph& graph, const CommandLine& command, Invoke invoke) {
    using Edge = std::remove_cv_t<std::remove_reference_t<decltype(*graph.out_neigh(0).begin())>>;
    const Edge* columns = graph.out_neigh(0).begin();
    const void* weights = nullptr;
    if constexpr (!std::is_integral<Edge>::value)
        weights = graph.num_edges_directed() ? static_cast<const void*>(&columns[0].w) : columns;
    const GraphView view{static_cast<uint64_t>(graph.num_nodes()),
        static_cast<uint64_t>(graph.num_edges_directed()), graph.directed(), graph.out_index_storage(),
        columns, weights, sizeof(Edge), true,
        graph.directed() ? graph.in_index_storage() : nullptr,
        graph.directed() ? graph.in_neigh(0).begin() : nullptr};
    return invoke(view, command);
}

template<class Invoke>
int applicationMain(
        int argc, char** argv, Invoke invoke, bool allow_popt = false,
        bool allow_grasp_record_base = false, bool allow_window_observer = false,
        bool allow_window_model = false, bool allow_bfs_phases = false, bool allow_frontier_model = false,
        bool allow_grasp_reference = false, bool allow_spmv_queries = false,
        bool allow_victim_controls = false) {
    try {
        const CommandLine command = parseCommandLine(argc, argv);
        if ((command.options.record_uninformed_base || !command.options.record_bound_compare) &&
            !allow_victim_controls)
            throw std::invalid_argument("record victim controls are cache_sim-only");
        if (command.queries > 1 && !allow_spmv_queries)
            throw std::invalid_argument("independent SpMV queries are cache_sim-only");
        if (command.options.grasp_reference != GraspReferenceMode::OFF && !allow_grasp_reference)
            throw std::invalid_argument("GRASP reference diagnostic is cache_sim-only");
        if (command.options.record_model == RecordModel::FRONTIER && !allow_frontier_model)
            throw std::invalid_argument("frontier model is cache_sim-only");
        if ((command.options.bfs_traffic_phases || command.options.grasp_graph_passes) && !allow_bfs_phases)
            throw std::invalid_argument("BFS phase controls are cache_sim-only");
        if (command.options.record_model == RecordModel::WINDOW && !allow_window_model)
            throw std::invalid_argument("window model is cache_sim-only");
        if (command.options.window_observer != WindowObserverMode::OFF && !allow_window_observer)
            throw std::invalid_argument("window observer is cache_sim-only");
        if (command.policy == "POPT_UNCHARGED" && !allow_popt)
            throw std::invalid_argument("current P-OPT is cache_sim-only");
        if (command.options.record_base_policy == RecordBasePolicy::GRASP_PAPER &&
            !allow_grasp_record_base)
            throw std::invalid_argument("GRASP_PAPER record base is cache_sim-only");
        const auto dot = command.graph_path.rfind('.');
        const std::string suffix = dot == std::string::npos ? "" : command.graph_path.substr(dot);
        if (command.options.record_model == RecordModel::WINDOW && suffix != ".sg")
            throw std::invalid_argument("window model requires an unweighted .sg input");
        if (suffix == ".wsg") {
            Reader<int32_t, NodeWeight<int32_t, int32_t>> reader(command.graph_path);
            auto graph = reader.ReadSerializedGraph(true, command.maximum_graph_bytes);
            return invokeGraph(graph, command, invoke);
        }
        if (suffix != ".sg")
            throw std::invalid_argument("current algorithms require explicit .sg or .wsg inputs");
        Reader<int32_t> reader(command.graph_path);
        auto graph = reader.ReadSerializedGraph(true, command.maximum_graph_bytes);
        return invokeGraph(graph, command, invoke);
    } catch (const std::invalid_argument& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR input=" << error.what() << "]\n";
        return 2;
    } catch (const std::length_error& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR resource=" << error.what() << "]\n";
        return 3;
    } catch (const std::overflow_error& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR arithmetic=" << error.what() << "]\n";
        return 4;
    } catch (const std::logic_error& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR invariant=" << error.what() << "]\n";
        return 5;
    } catch (const std::ios_base::failure& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR io=" << error.what() << "]\n";
        return 6;
    } catch (const std::system_error& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR system=" << error.what() << "]\n";
        return 7;
    } catch (const std::bad_alloc& error) {
        std::cerr << "[ECG-ALGORITHM-ERROR allocation=" << error.what() << "]\n";
        return 8;
    }
}

} // namespace ecg_algorithm

#endif
