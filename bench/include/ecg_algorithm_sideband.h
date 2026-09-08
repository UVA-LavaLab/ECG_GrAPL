#ifndef GRAPHBREW_ECG_ALGORITHM_SIDEBAND_H
#define GRAPHBREW_ECG_ALGORITHM_SIDEBAND_H

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#include "ecg_algorithm_main.h"

namespace ecg_algorithm {

class AlgorithmSideband {
  public:
    explicit AlgorithmSideband(const char* environment) {
        const char* path = std::getenv(environment);
        if (!path || path[0] != '/')
            throw std::invalid_argument(std::string(environment) + " requires an isolated absolute path");
        path_ = path;
    }

    void graph(const GraphView& value) { graph_ = input_graph_ = value; }
    void activeGraph(const GraphView& value) {
        if (value.vertices != input_graph_.vertices)
            throw std::invalid_argument("active-graph-property-domain-mismatch");
        graph_ = value;
    }

    void region(const char* name, const void* base, uint64_t count, uint8_t bytes, bool property) {
        if (!property)
            return;
        if (region_count_ == regions_.size() || count != graph_.vertices ||
            (bytes != 4 && bytes != 8))
            throw std::invalid_argument("invalid-detailed-property-region");
        regions_[region_count_++] = {name, reinterpret_cast<uint64_t>(base), count, bytes};
    }

    void publish(const ecg_record::RecordStream* carrier = nullptr) const {
        if (region_count_ == 0)
            throw std::logic_error("cannot-publish-empty-property-context");
        const uint64_t base = carrier ? reinterpret_cast<uint64_t>(carrier->data()) :
            reinterpret_cast<uint64_t>(graph_.columns);
        const uint64_t count = carrier ? carrier->size() : graph_.records;
        const uint64_t bytes = carrier ? carrier->layout.record_bytes : graph_.edge_stride;
        const std::string temporary = path_ + ".new";
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(temporary);
        output << "{\"schema\":\"ecg.algorithm-sideband.v1\",\"num_vertices\":" << graph_.vertices
               << ",\"num_edges\":" << graph_.records
               << ",\"directed\":" << (graph_.directed ? "true" : "false")
               << ",\"edge_preferred_base\":" << base << ",\"edge_preferred_size\":" << count * bytes
               << ",\"edge_other_base\":" << reinterpret_cast<uint64_t>(graph_.columns)
               << ",\"edge_other_size\":" << graph_.records * graph_.edge_stride
               << ",\"csr_offsets_base\":" << reinterpret_cast<uint64_t>(graph_.offsets)
               << ",\"csr_offsets_size\":" << (graph_.vertices + 1) * sizeof(uint64_t)
               << ",\"input_num_edges\":" << input_graph_.records
               << ",\"input_edges_base\":" << reinterpret_cast<uint64_t>(input_graph_.columns)
               << ",\"input_edges_size\":" << input_graph_.records * input_graph_.edge_stride
               << ",\"input_csr_offsets_base\":" << reinterpret_cast<uint64_t>(input_graph_.offsets)
               << ",\"property_regions\":[";
        for (std::size_t index = 0; index < region_count_; ++index) {
            const auto& region = regions_[index];
            if (index) output << ',';
            output << "{\"name\":\"" << region.name << "\",\"base\":" << region.base
                   << ",\"size\":" << region.count * region.bytes << ",\"count\":" << region.count
                   << ",\"elem_size\":" << unsigned(region.bytes)
                   << ",\"stride\":" << unsigned(region.bytes) << ",\"grasp\":true}";
        }
        output << "],\"edge_regions\":[{\"name\":\""
               << (carrier ? "ecg_records" : "out_edges")
               << "\",\"base\":" << base << ",\"size\":" << count * bytes
               << ",\"elem_size\":" << bytes << ",\"preferred\":true}]}";
        output.close();
        int status;
#if defined(__riscv) && __riscv_xlen == 64 && !defined(NO_M5OPS)
        // The pinned SE model implements renameat (38), not glibc's renameat2.
        status = static_cast<int>(::syscall(
            38, AT_FDCWD, temporary.c_str(), AT_FDCWD, path_.c_str()));
#else
        status = std::rename(temporary.c_str(), path_.c_str());
#endif
        if (status != 0)
            throw std::system_error(errno, std::generic_category(), "publish algorithm sideband");
    }

  private:
    struct Region {
        const char* name = nullptr;
        uint64_t base = 0, count = 0;
        uint8_t bytes = 0;
    };
    std::string path_;
    GraphView graph_, input_graph_;
    std::array<Region, 8> regions_{};
    std::size_t region_count_ = 0;
};

inline void writeDetailedResult(
        const char* backend, const Result& result, const CommandLine& command) {
    const auto report = [&](std::ostream& output) {
        output << "{\"schema\":\"ecg.algorithm-result.v1\",\"backend\":\"" << backend
               << "\",\"timing_valid_for_speedup\":false,"
               << "\"measurement_scope\":\"algorithm-setup-kernel-drain\","
               << "\"mode\":\"" << (command.options.records ?
                    ecg_record::mechanismName(command.options.mechanism) : "csr")
               << "\",\"policy\":\"" << command.policy << "\",\"workload\":";
        writeResult(output, result, command.options);
        output << "}\n";
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
}

} // namespace ecg_algorithm

#endif
