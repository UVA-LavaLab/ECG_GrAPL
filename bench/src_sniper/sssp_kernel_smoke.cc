#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>

#include "sniper_sim/sniper_harness.h"

namespace {

constexpr int kNodes = 4;
constexpr int kEdges = 8;
constexpr int kOffsets[kNodes + 1] = {0, 2, 4, 6, 8};
constexpr int kOutEdges[kEdges] = {1, 2, 0, 2, 0, 3, 1, 2};
constexpr int kWeights[kEdges] = {1, 4, 1, 2, 4, 1, 2, 1};
constexpr int kInf = std::numeric_limits<int>::max() / 4;
constexpr int kPropertyStride = 16;

int& dist_at(int* dist, int vertex) {
    return dist[vertex * kPropertyStride];
}

void export_context(int* dist) {
    SniperPropertyRegion regions[1] = {
        {"dist", reinterpret_cast<uint64_t>(dist), sizeof(int) * kNodes * kPropertyStride,
         kNodes, sizeof(int) * kPropertyStride},
    };

    const std::string edge_path = graphbrew_sniper::out_edges_path();
    {
        std::ofstream edge_out(edge_path, std::ios::binary);
        edge_out.write(reinterpret_cast<const char*>(kOutEdges), sizeof(kOutEdges));
    }

    std::ofstream out(graphbrew_sniper::context_path());
    out << "{\n";
    out << "  \"num_vertices\": " << kNodes << ",\n";
    out << "  \"num_edges\": " << kEdges << ",\n";
    out << "  \"directed\": false,\n";
    out << "  \"property_regions\": [\n";
    out << "    {\"name\": \"" << regions[0].name << "\", \"base\": "
        << regions[0].base_address << ", \"size\": " << regions[0].size_bytes
        << ", \"count\": " << regions[0].num_elements << ", \"elem_size\": "
        << regions[0].elem_size << "}\n";
    out << "  ],\n";
    out << "  \"edge_regions\": [\n";
    out << "    {\"name\": \"out_edges\", \"base\": "
        << reinterpret_cast<uint64_t>(kOutEdges) << ", \"size\": " << sizeof(kOutEdges)
        << ", \"elem_size\": " << sizeof(kOutEdges[0])
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
    for (uint8_t& value : matrix) value = 127;
    sniper_export_popt_matrix(matrix, 1, 16, kNodes);
}

}  // namespace

int main() {
    alignas(4096) int dist[kNodes * kPropertyStride];
    for (int& value : dist) value = kInf;
    dist_at(dist, 0) = 0;

    export_popt_matrix();
    export_context(dist);

    SNIPER_ROI_BEGIN();
    const volatile int* out_edges = kOutEdges;
    const volatile int* weights = kWeights;
    for (int iter = 0; iter < kNodes - 1; ++iter) {
        for (int u = 0; u < kNodes; ++u) {
            SNIPER_SET_VERTEX(u);
            int source_dist = dist_at(dist, u);
            if (source_dist == kInf) continue;
            for (int edge = kOffsets[u]; edge < kOffsets[u + 1]; ++edge) {
                int v = out_edges[edge];
                int candidate = source_dist + weights[edge];
                int& target_dist = dist_at(dist, v);
                target_dist = std::min(target_dist, candidate);
            }
        }
    }
    SNIPER_ROI_END();

    int checksum = 0;
    for (int vertex = 0; vertex < kNodes; ++vertex) {
        checksum += dist_at(dist, vertex);
    }
    std::cout << "GraphBrew Sniper SSSP kernel smoke checksum: " << checksum << std::endl;
    return checksum > 0 && checksum < kInf ? 0 : 1;
}