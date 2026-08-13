#include <cstdint>
#include <iostream>
#include <queue>

#include "sniper_sim/sniper_harness.h"

namespace {

constexpr int kNodes = 4;
constexpr int kEdges = 8;
constexpr int kOffsets[kNodes + 1] = {0, 2, 4, 6, 8};
constexpr int kOutEdges[kEdges] = {1, 2, 0, 2, 0, 3, 1, 2};
constexpr int kPropertyStride = 16;

int& parent_at(int* parent, int vertex) {
    return parent[vertex * kPropertyStride];
}

void export_context(int* parent) {
    SniperPropertyRegion regions[1] = {
        {"parent", reinterpret_cast<uint64_t>(parent), sizeof(int) * kNodes * kPropertyStride,
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
    alignas(4096) int parent[kNodes * kPropertyStride];
    for (int& value : parent) value = -1;
    parent_at(parent, 0) = 0;

    export_popt_matrix();
    export_context(parent);

    SNIPER_ROI_BEGIN();
    std::queue<int> frontier;
    frontier.push(0);
    const volatile int* out_edges = kOutEdges;
    while (!frontier.empty()) {
        int u = frontier.front();
        frontier.pop();
        SNIPER_SET_VERTEX(u);
        for (int edge = kOffsets[u]; edge < kOffsets[u + 1]; ++edge) {
            int v = out_edges[edge];
            if (parent_at(parent, v) == -1) {
                parent_at(parent, v) = u;
                frontier.push(v);
            }
        }
    }
    SNIPER_ROI_END();

    int reached = 0;
    for (int vertex = 0; vertex < kNodes; ++vertex) {
        reached += parent_at(parent, vertex) >= 0 ? 1 : 0;
    }
    std::cout << "GraphBrew Sniper BFS kernel smoke reached: " << reached << std::endl;
    return reached == kNodes ? 0 : 1;
}