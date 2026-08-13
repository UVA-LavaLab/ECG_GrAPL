#include <cmath>
#include <cstdint>
#include <iostream>

#include "sniper_sim/sniper_harness.h"

namespace {

using ScoreT = float;
constexpr float kDamp = 0.85f;
constexpr int kNodes = 4;
constexpr int kEdges = 8;
constexpr int kOffsets[kNodes + 1] = {0, 2, 4, 6, 8};
constexpr int kInEdges[kEdges] = {1, 2, 0, 2, 0, 3, 1, 2};
constexpr int kOutDegree[kNodes] = {2, 2, 2, 2};

void export_context(ScoreT* scores, ScoreT* contrib) {
    SniperPropertyRegion regions[2] = {
        {"scores", reinterpret_cast<uint64_t>(scores), sizeof(ScoreT) * kNodes,
            kNodes, sizeof(ScoreT), true},
        {"contrib", reinterpret_cast<uint64_t>(contrib), sizeof(ScoreT) * kNodes,
            kNodes, sizeof(ScoreT), true},
    };

    const std::string edge_path = graphbrew_sniper::in_edges_path();
    {
        std::ofstream edge_out(edge_path, std::ios::binary);
        edge_out.write(reinterpret_cast<const char*>(kInEdges), sizeof(kInEdges));
    }

    std::ofstream out(graphbrew_sniper::context_path());
    out << "{\n";
    out << "  \"num_vertices\": " << kNodes << ",\n";
    out << "  \"num_edges\": " << kEdges << ",\n";
    out << "  \"directed\": false,\n";
    out << "  \"property_regions\": [\n";
    for (int i = 0; i < 2; ++i) {
        out << "    {\"name\": \"" << regions[i].name << "\", \"base\": "
            << regions[i].base_address << ", \"size\": " << regions[i].size_bytes
            << ", \"count\": " << regions[i].num_elements << ", \"elem_size\": "
            << regions[i].elem_size << ", \"grasp\": "
            << (regions[i].grasp_region ? "true" : "false")
            << "}" << (i == 0 ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"edge_regions\": [\n";
    out << "    {\"name\": \"in_edges\", \"base\": "
        << reinterpret_cast<uint64_t>(kInEdges) << ", \"size\": " << sizeof(kInEdges)
        << ", \"elem_size\": " << sizeof(kInEdges[0])
        << ", \"preferred\": true, \"data_path\": \"" << edge_path << "\"}\n";
    out << "  ],\n";
    out << "  \"avg_degree\": 2.0,\n";
    out << "  \"max_degree\": 2,\n";
    out << "  \"degree_buckets\": {\"counts\": [0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0], "
        << "\"total_degrees\": [0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0]}\n";
    out << "}\n";
    out.close();
    graphbrew_sniper::notify_context_ready();
}

void export_popt_matrix() {
    uint8_t matrix[16] = {};
    for (uint8_t& value : matrix) {
        value = 127;
    }
    sniper_export_popt_matrix(matrix, 1, 16, kNodes);
}

}  // namespace

int main() {
    alignas(4096) ScoreT scores[kNodes];
    alignas(4096) ScoreT contrib[kNodes];
    for (int i = 0; i < kNodes; ++i) {
        scores[i] = 1.0f / kNodes;
        contrib[i] = scores[i] / kOutDegree[i];
    }

    export_popt_matrix();
    export_context(scores, contrib);

    SNIPER_ROI_BEGIN();
    for (int iter = 0; iter < 2; ++iter) {
        for (int u = 0; u < kNodes; ++u) {
            SNIPER_SET_VERTEX(u);
            ScoreT incoming_total = 0.0f;
            for (int edge = kOffsets[u]; edge < kOffsets[u + 1]; ++edge) {
                incoming_total += contrib[kInEdges[edge]];
            }
            scores[u] = ((1.0f - kDamp) / kNodes) + kDamp * incoming_total;
            contrib[u] = scores[u] / kOutDegree[u];
        }
    }
    SNIPER_ROI_END();

    ScoreT checksum = 0.0f;
    for (float score : scores) {
        checksum += score;
    }
    std::cout << "GraphBrew Sniper PR kernel smoke checksum: " << checksum << std::endl;
    return std::fabs(checksum) > 0.0f ? 0 : 1;
}
