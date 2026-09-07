#include "cache_sim/ecg_algorithm.h"
#include "ecg_algorithm_main.h"

int main(int argc, char** argv) {
    return ecg_algorithm::applicationMain(argc, argv,
        [](const ecg_algorithm::GraphView& graph, const ecg_algorithm::CommandLine& command) {
            const bool replacement = command.options.records &&
                (command.options.mechanism == ecg_record::Mechanism::REPLACEMENT ||
                 command.options.mechanism == ecg_record::Mechanism::REPLACEMENT_PREFETCH);
            const auto policy = replacement ? cache_sim::EvictionPolicy::ECG :
                command.policy == "SRRIP" ? cache_sim::EvictionPolicy::SRRIP :
                command.policy == "GRASP_PAPER" ? cache_sim::EvictionPolicy::GRASP :
                cache_sim::EvictionPolicy::LRU;
            if (setenv("GRASP_BOUNDARY_MODE", command.policy == "GRASP_PAPER" ? "capacity" : "vertex", 1) != 0)
                throw std::system_error(errno, std::generic_category(), "GRASP boundary configuration");
            cache_sim::CacheHierarchy cache(command.l1_bytes, command.l1_ways,
                command.l2_bytes, command.l2_ways, command.llc_bytes, command.llc_ways, 64,
                cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, policy);
            cache_sim::AlgorithmBackend backend(cache, command.options, command.llc_bytes,
                command.policy == "GRASP_PAPER");
            const auto start = std::chrono::steady_clock::now();
            const auto result = ecg_algorithm::run(graph, command.options, backend);
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            const auto report = [&](std::ostream& output) {
                output << "{\"schema\":\"ecg.algorithm-result.v1\",\"backend\":\"cache_sim\","
                       << "\"timing_valid_for_speedup\":false,"
                       << "\"measurement_scope\":\"algorithm-data-traffic-including-construction\","
                       << "\"mode\":\"" << (command.options.records ?
                            ecg_record::mechanismName(command.options.mechanism) : "csr")
                       << "\",\"policy\":\"" << command.policy << "\","
                       << "\"host_seconds\":" << std::setprecision(12) << seconds << ",\"workload\":";
                ecg_algorithm::writeResult(output, result, command.options);
                output << ",\"metrics\":" << cache.toJSON() << "}\n";
            };
            if (command.output_path.empty()) {
                report(std::cout);
            } else {
                std::ofstream output;
                output.exceptions(std::ios::badbit | std::ios::failbit);
                output.open(command.output_path);
                report(output);
                output.close();
            }
            return 0;
        });
}
