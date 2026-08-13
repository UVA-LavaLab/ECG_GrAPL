// ECG preprocessing benchmark without cache simulation.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "cache_sim/graph_cache_context.h"
#include "cache_sim/graph_sim.h"
#include "graphbrew/partition/cagra/popt.h"

using cache_sim::GraphCacheContext;

namespace {

using Clock = std::chrono::steady_clock;

struct WallTimer {
    Clock::time_point start = Clock::now();

    double seconds() const {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
};

struct Sample {
    double degree_scan_s = 0.0;
    double popt_matrix_s = 0.0;
    double mask_build_s = 0.0;
    double total_s = 0.0;
    uint64_t mask_build_us = 0;
    uint64_t mask_vertices = 0;
    uint64_t pfx_candidates = 0;
    uint64_t pfx_encoded = 0;
    uint64_t pfx_no_candidate = 0;
    uint64_t pfx_table_miss = 0;
    uint64_t pfx_dedup_skips = 0;
};

int envInt(const char* name, int fallback, int min_value, int max_value) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    int parsed = std::atoi(value);
    return std::max(min_value, std::min(max_value, parsed));
}

size_t envSize(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end && *end) {
        if (*end == 'K' || *end == 'k') parsed *= 1024ULL;
        else if (*end == 'M' || *end == 'm') parsed *= 1024ULL * 1024ULL;
        else if (*end == 'G' || *end == 'g') parsed *= 1024ULL * 1024ULL * 1024ULL;
    }
    return parsed > 0 ? static_cast<size_t>(parsed) : fallback;
}

bool envFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(text == "0" || text == "false" || text == "no" || text == "off");
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double minValue(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}

double maxValue(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

void writeMetric(std::ostream& os, const char* name, double value, bool& first) {
    if (!first) os << ",\n";
    first = false;
    os << "  \"" << name << "\": " << std::fixed << std::setprecision(9) << value;
}

void writeMetric(std::ostream& os, const char* name, uint64_t value, bool& first) {
    if (!first) os << ",\n";
    first = false;
    os << "  \"" << name << "\": " << value;
}

void writeMetric(std::ostream& os, const char* name, int value, bool& first) {
    writeMetric(os, name, static_cast<uint64_t>(value), first);
}

}  // namespace

int main(int argc, char* argv[]) {
    CLApp cli(argc, argv, "ecg-preprocess-benchmark");
    if (!cli.ParseArgs()) return -1;

    WallTimer graph_load_timer;
    Builder builder(cli);
    Graph graph = builder.MakeGraph();
    double graph_load_s = graph_load_timer.seconds();

    int repeats = envInt("ECG_PREPROCESS_REPEATS", 3, 1, 1000);
    int num_epochs = envInt("ECG_PREPROCESS_EPOCHS", 256, 1, 4096);
    int property_bytes = envInt("ECG_PREPROCESS_PROPERTY_BYTES", 4, 1, 64);
    size_t llc_size = envSize("CACHE_L3_SIZE", 8ULL * 1024ULL * 1024ULL);
    int prefetch_mode = envInt("ECG_PREFETCH_MODE", 0, 0, 2);
    bool build_popt = envFlag("ECG_PREPROCESS_BUILD_POPT", prefetch_mode == 2);
    int num_vtx_per_line = std::max(1, 64 / property_bytes);

    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(repeats));

    for (int rep = 0; rep < repeats; ++rep) {
        Sample sample;
        WallTimer total_timer;

        WallTimer degree_timer;
        pvector<uint32_t> degrees(graph.num_nodes());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (NodeID n = 0; n < graph.num_nodes(); ++n) {
            degrees[n] = static_cast<uint32_t>(graph.out_degree(n));
        }
        sample.degree_scan_s = degree_timer.seconds();

        GraphCacheContext graph_ctx;
        graph_ctx.initTopology(degrees.data(), graph.num_nodes(),
                               graph.num_edges_directed(), graph.directed());
        pvector<uint8_t> property_storage(
            static_cast<size_t>(graph.num_nodes()) *
                static_cast<size_t>(property_bytes),
            uint8_t(0), cache_sim::GRAPH_SIM_PROPERTY_ALIGNMENT);
        graph_ctx.registerPropertyArray(property_storage.data(), graph.num_nodes(),
                                        static_cast<uint32_t>(property_bytes), llc_size);

        pvector<uint8_t> popt_matrix;
        if (build_popt) {
            WallTimer popt_timer;
            makeOffsetMatrix(graph, popt_matrix, num_vtx_per_line, num_epochs);
            int num_cache_lines = (graph.num_nodes() + num_vtx_per_line - 1) / num_vtx_per_line;
            graph_ctx.initRereference(popt_matrix.data(), num_cache_lines,
                                      static_cast<uint32_t>(num_epochs), graph.num_nodes(), 64);
            sample.popt_matrix_s = popt_timer.seconds();
        }

        graph_ctx.initMaskConfig();
        WallTimer mask_timer;
        auto masks = graph_ctx.computeVertexMasks(graph);
        sample.mask_build_s = mask_timer.seconds();
        sample.total_s = total_timer.seconds();
        sample.mask_build_us = graph_ctx.ecg_stats.mask_build_us;
        sample.mask_vertices = graph_ctx.ecg_stats.mask_vertices;
        sample.pfx_candidates = graph_ctx.ecg_stats.pfx_candidates;
        sample.pfx_encoded = graph_ctx.ecg_stats.pfx_encoded;
        sample.pfx_no_candidate = graph_ctx.ecg_stats.pfx_no_candidate;
        sample.pfx_table_miss = graph_ctx.ecg_stats.pfx_table_miss;
        sample.pfx_dedup_skips = graph_ctx.ecg_stats.pfx_dedup_skips;

        volatile uint32_t keep = masks.empty() ? 0 : masks[0];
        (void)keep;
        samples.push_back(sample);
    }

    std::vector<double> degree_s, popt_s, mask_s, total_s;
    degree_s.reserve(samples.size());
    popt_s.reserve(samples.size());
    mask_s.reserve(samples.size());
    total_s.reserve(samples.size());
    for (const Sample& sample : samples) {
        degree_s.push_back(sample.degree_scan_s);
        popt_s.push_back(sample.popt_matrix_s);
        mask_s.push_back(sample.mask_build_s);
        total_s.push_back(sample.total_s);
    }
    const Sample& last = samples.back();

    std::ostream* out = &std::cout;
    std::ofstream file;
    const char* output_path = std::getenv("ECG_PREPROCESS_OUTPUT_JSON");
    if (output_path && output_path[0]) {
        file.open(output_path);
        if (!file.is_open()) {
            std::cerr << "Could not open ECG_PREPROCESS_OUTPUT_JSON=" << output_path << std::endl;
            return 2;
        }
        out = &file;
    }

    bool first = true;
    *out << "{\n";
    writeMetric(*out, "graph_load_s", graph_load_s, first);
    writeMetric(*out, "nodes", static_cast<uint64_t>(graph.num_nodes()), first);
    writeMetric(*out, "edges_directed", static_cast<uint64_t>(graph.num_edges_directed()), first);
    writeMetric(*out, "directed", graph.directed() ? 1 : 0, first);
    writeMetric(*out, "omp_threads", envInt("OMP_NUM_THREADS",
#ifdef _OPENMP
        omp_get_max_threads(),
#else
        1,
#endif
        1, 100000), first);
    writeMetric(*out, "repeats", repeats, first);
    writeMetric(*out, "property_bytes", property_bytes, first);
    writeMetric(*out, "num_vtx_per_line", num_vtx_per_line, first);
    writeMetric(*out, "popt_epochs", num_epochs, first);
    writeMetric(*out, "popt_matrix_enabled", build_popt ? 1 : 0, first);
    writeMetric(*out, "ecg_prefetch_mode", prefetch_mode, first);
    writeMetric(*out, "degree_scan_s_mean", mean(degree_s), first);
    writeMetric(*out, "degree_scan_s_min", minValue(degree_s), first);
    writeMetric(*out, "degree_scan_s_max", maxValue(degree_s), first);
    writeMetric(*out, "popt_matrix_s_mean", mean(popt_s), first);
    writeMetric(*out, "popt_matrix_s_min", minValue(popt_s), first);
    writeMetric(*out, "popt_matrix_s_max", maxValue(popt_s), first);
    writeMetric(*out, "mask_build_s_mean", mean(mask_s), first);
    writeMetric(*out, "mask_build_s_min", minValue(mask_s), first);
    writeMetric(*out, "mask_build_s_max", maxValue(mask_s), first);
    writeMetric(*out, "total_preprocess_s_mean", mean(total_s), first);
    writeMetric(*out, "total_preprocess_s_min", minValue(total_s), first);
    writeMetric(*out, "total_preprocess_s_max", maxValue(total_s), first);
    writeMetric(*out, "last_mask_build_us", last.mask_build_us, first);
    writeMetric(*out, "mask_vertices", last.mask_vertices, first);
    writeMetric(*out, "pfx_candidates", last.pfx_candidates, first);
    writeMetric(*out, "pfx_encoded", last.pfx_encoded, first);
    writeMetric(*out, "pfx_no_candidate", last.pfx_no_candidate, first);
    writeMetric(*out, "pfx_table_miss", last.pfx_table_miss, first);
    writeMetric(*out, "pfx_dedup_skips", last.pfx_dedup_skips, first);
    *out << "\n}\n";

    return 0;
}