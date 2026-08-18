// Native ReusePlan sidecar generator and verifier.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "ecg_reuse_plan_builder.h"
#include "ecg_reuse_plan_sidecar.h"

namespace {

uint32_t envUint(
        const char* name, uint32_t fallback,
        uint32_t minimum, uint32_t maximum) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || *end)
        return fallback;
    return std::max(
        minimum, std::min(maximum, static_cast<uint32_t>(parsed)));
}

bool envFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    return std::string(value) != "0";
}

template <typename RecordT>
int generateOrVerify(
        const Graph& graph, const std::string& output_path,
        bool verify_only, bool push_out_edges,
        uint32_t num_vtx_per_line, uint32_t epochs,
        bool linemin, double hot_fraction) {
    std::vector<uint64_t> offsets;
    std::vector<RecordT> records;
    ecg_reuse_plan::ReusePlanSidecarHeader header;
    std::string error;
    if (verify_only) {
        if (!ecg_reuse_plan::loadReusePlanSidecar(
                output_path, graph, push_out_edges,
                num_vtx_per_line, epochs, linemin, hot_fraction,
                offsets, records, header, error)) {
            std::cerr << "[ReusePlan-SIDECAR-FAIL] " << error << "\n";
            return 2;
        }
    } else {
        bool built = true;
        if constexpr (sizeof(RecordT) == 4) {
            built = ecg_reuse_plan::buildInEdgeReusePlanRecords32(
                graph, num_vtx_per_line, epochs, linemin,
                offsets, records, push_out_edges);
        } else {
            ecg_reuse_plan::buildInEdgeReusePlanRecords(
                graph, num_vtx_per_line, epochs, linemin,
                offsets, records, push_out_edges);
        }
        if (!built) {
            std::cerr
                << "[ReusePlan-SIDECAR-FAIL] compact record is infeasible\n";
            return 2;
        }
        header.push_out_edges = push_out_edges ? 1U : 0U;
        header.num_vtx_per_line = num_vtx_per_line;
        header.epochs =
            ecg_reuse_plan::normalizeReusePlanEpochCount(epochs);
        header.linemin = linemin ? 1U : 0U;
        header.hot_fraction_ppm =
            ecg_reuse_plan::sidecarHotFractionPpm(hot_fraction);
        header.vertices = static_cast<uint32_t>(graph.num_nodes());
        header.directed_edges =
            static_cast<uint64_t>(graph.num_edges_directed());
        header.graph_hash =
            ecg_reuse_plan::orderedGraphHash(graph, push_out_edges);
        if (!ecg_reuse_plan::writeReusePlanSidecar(
                output_path, header, offsets, records, error)) {
            std::cerr << "[ReusePlan-SIDECAR-FAIL] " << error << "\n";
            return 2;
        }
        offsets.clear();
        records.clear();
        if (!ecg_reuse_plan::loadReusePlanSidecar(
                output_path, graph, push_out_edges,
                num_vtx_per_line, epochs, linemin, hot_fraction,
                offsets, records, header, error)) {
            std::cerr
                << "[ReusePlan-SIDECAR-FAIL] post-write validation: "
                << error << "\n";
            return 2;
        }
    }
    std::cout
        << "[ReusePlan-SIDECAR-OK path=" << output_path
        << " record_bytes=" << sizeof(RecordT)
        << " vertices=" << header.vertices
        << " records=" << header.record_count
        << " graph_hash=" << header.graph_hash
        << " payload_hash=" << header.payload_hash
        << "]\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    CLPageRank cli(argc, argv, "reuse-plan-sidecar", 1e-4, 1);
    if (!cli.ParseArgs()) return 1;
    Builder builder(cli);
    Graph graph = builder.MakeGraph();

    const char* output = std::getenv("ECG_REUSE_PLAN_SIDECAR");
    if (!output || !output[0]) {
        std::cerr
            << "ECG_REUSE_PLAN_SIDECAR must name the output sidecar\n";
        return 2;
    }
    const uint32_t record_bytes = envUint(
        "ECG_REUSE_PLAN_SIDECAR_RECORD_BYTES", 8, 4, 8);
    if (record_bytes != 4 && record_bytes != 8) {
        std::cerr << "record width must be 4 or 8 bytes\n";
        return 2;
    }
    const uint32_t epochs = envUint(
        "ECG_REUSE_PLAN_SIDECAR_EPOCHS", 16, 2, 32768);
    const uint32_t num_vtx_per_line = envUint(
        "ECG_REUSE_PLAN_SIDECAR_VPL", 16, 1, 1024);
    const bool push_out_edges = envFlag(
        "ECG_REUSE_PLAN_SIDECAR_PUSH", false);
    const bool linemin = envFlag(
        "ECG_REUSE_PLAN_SIDECAR_LINEMIN", true);
    const bool verify_only = envFlag(
        "ECG_REUSE_PLAN_SIDECAR_VERIFY_ONLY", false);
    const double hot_fraction =
        ecg_reuse_plan::configuredReuseHotFraction();
    if (record_bytes == 4) {
        return generateOrVerify<uint32_t>(
            graph, output, verify_only, push_out_edges,
            num_vtx_per_line, epochs, linemin, hot_fraction);
    }
    return generateOrVerify<uint64_t>(
        graph, output, verify_only, push_out_edges,
        num_vtx_per_line, epochs, linemin, hot_fraction);
}
