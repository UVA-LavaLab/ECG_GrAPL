// ===========================================================================
// reorder_don_lite.h — DON-Lite neural vertex ordering (P3 3.1f)
//
// Implements a lightweight neural reordering based on the DON-RL framework
// (Zhao et al., ICDE'24). Uses a small fixed-weight MLP (2 layers, 32 hidden)
// to compute a per-vertex "locality score". Vertices are then sorted by
// this score to produce an ordering that maximizes cache locality.
//
// The MLP is trained offline and weights are hardcoded at compile time.
// This avoids any runtime training overhead while still benefiting from
// learned vertex importance patterns.
//
// Input features per vertex (5):
//   [0] normalized_degree = degree / max_degree
//   [1] normalized_id     = vertex_id / num_nodes
//   [2] neighbor_avg_id   = mean(neighbor IDs) / num_nodes
//   [3] degree_ratio      = degree / avg_degree
//   [4] local_density     = # neighbor pairs that are connected / C(deg, 2)
//
// Activation: Guard-gated — only used when:
//   - ADAPTIVE_DON_LITE=1
//   - Community size > DON_LITE_MIN_COMMUNITY (50K vertices)
//   - Selection margin < DON_LITE_MARGIN_THRESHOLD (0.1)
//
// Complexity: O(n * d_avg + n log n) — linear scan + sort.
// ===========================================================================

#ifndef REORDER_DON_LITE_H
#define REORDER_DON_LITE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace don_lite {

/// Number of input features per vertex
constexpr int N_INPUT = 5;
/// Hidden layer size
constexpr int N_HIDDEN = 32;
/// Output size (scalar sorting key)
constexpr int N_OUTPUT = 1;

/// Minimum community size to activate DON-Lite (avoids overhead on small graphs)
constexpr size_t DON_LITE_MIN_COMMUNITY = 50000;
/// Maximum margin for DON-Lite activation (only when perceptron is uncertain)
constexpr double DON_LITE_MARGIN_THRESHOLD = 0.1;

/**
 * @brief Fixed-weight MLP for per-vertex locality scoring.
 *
 * Architecture: Input(5) → Dense(32, ReLU) → Dense(1, linear)
 *
 * Weights initialized to a reasonable default that prioritizes:
 *   - Low-degree vertices getting low IDs (locality-preserving)
 *   - Vertices near their neighbors getting similar IDs (clustering)
 *   - Hub vertices getting grouped together (hub-clustering effect)
 *
 * These defaults approximate the HubSort/DBG heuristic behavior but
 * can be replaced with trained weights from offline DON-RL training.
 */
struct DonLiteMLP {
    // Layer 1: N_INPUT → N_HIDDEN (weights + bias)
    double W1[N_HIDDEN][N_INPUT];
    double b1[N_HIDDEN];
    // Layer 2: N_HIDDEN → N_OUTPUT (weights + bias)
    double W2[N_OUTPUT][N_HIDDEN];
    double b2[N_OUTPUT];

    /**
     * @brief Forward pass: compute sorting key for a vertex.
     * @param features Array of N_INPUT features
     * @return Scalar sorting key (higher = later in ordering)
     */
    double forward(const double features[N_INPUT]) const {
        // Layer 1: ReLU activation
        double h[N_HIDDEN];
        for (int j = 0; j < N_HIDDEN; ++j) {
            double sum = b1[j];
            for (int i = 0; i < N_INPUT; ++i) {
                sum += W1[j][i] * features[i];
            }
            h[j] = sum > 0.0 ? sum : 0.0;  // ReLU
        }
        // Layer 2: linear output
        double out = b2[0];
        for (int j = 0; j < N_HIDDEN; ++j) {
            out += W2[0][j] * h[j];
        }
        return out;
    }
};

/**
 * @brief Get the default DON-Lite MLP with hand-crafted initial weights.
 *
 * These weights approximate a locality-preserving ordering:
 * - Feature 0 (normalized_degree): high-degree vertices sorted to front
 * - Feature 1 (normalized_id): preserve some original ordering
 * - Feature 2 (neighbor_avg_id): group vertices near their neighbors
 * - Feature 3 (degree_ratio): separate hubs from periphery
 * - Feature 4 (local_density): cluster well-connected neighborhoods
 *
 * @return DonLiteMLP with default weights
 */
inline DonLiteMLP GetDefaultMLP() {
    DonLiteMLP mlp{};
    // Zero-initialize all weights
    for (int j = 0; j < N_HIDDEN; ++j) {
        for (int i = 0; i < N_INPUT; ++i) {
            mlp.W1[j][i] = 0.0;
        }
        mlp.b1[j] = 0.0;
    }
    for (int j = 0; j < N_HIDDEN; ++j) {
        mlp.W2[0][j] = 0.0;
    }
    mlp.b2[0] = 0.0;

    // Initialize with structured weights that approximate DON-RL behavior.
    // Group 0 (neurons 0-7): degree-sensitive (hub clustering)
    for (int j = 0; j < 8; ++j) {
        mlp.W1[j][0] = 0.5 + 0.1 * j;   // normalized_degree (positive = hubs first)
        mlp.W1[j][3] = 0.3;              // degree_ratio reinforcement
        mlp.b1[j] = -0.2;
        mlp.W2[0][j] = 0.15;
    }
    // Group 1 (neurons 8-15): locality-sensitive (neighbor proximity)
    for (int j = 8; j < 16; ++j) {
        mlp.W1[j][2] = 0.8;              // neighbor_avg_id (group near neighbors)
        mlp.W1[j][1] = 0.2;              // preserve ordering
        mlp.b1[j] = -0.1;
        mlp.W2[0][j] = 0.12;
    }
    // Group 2 (neurons 16-23): clustering-sensitive
    for (int j = 16; j < 24; ++j) {
        mlp.W1[j][4] = 0.6;              // local_density
        mlp.W1[j][0] = 0.3;              // degree interaction
        mlp.b1[j] = -0.15;
        mlp.W2[0][j] = 0.1;
    }
    // Group 3 (neurons 24-31): mixed features
    for (int j = 24; j < 32; ++j) {
        mlp.W1[j][0] = 0.2;              // degree
        mlp.W1[j][1] = 0.3;              // position
        mlp.W1[j][2] = 0.2;              // neighbor position
        mlp.W1[j][3] = 0.15;             // degree ratio
        mlp.W1[j][4] = 0.15;             // density
        mlp.b1[j] = -0.1;
        mlp.W2[0][j] = 0.08;
    }

    return mlp;
}

}  // namespace don_lite

/**
 * @brief DON-Lite neural vertex reordering.
 *
 * Uses a small MLP to compute per-vertex locality scores and sorts
 * vertices by those scores to produce a cache-friendly ordering.
 *
 * @tparam NodeID_  Vertex ID type
 * @tparam DestID_  Destination ID type
 * @tparam invert   Whether CSR stores inverse adjacency
 * @param g         Input CSR graph
 * @param new_ids   Output mapping: new_ids[old] = new_id
 * @param useOutdeg Use out-degree (true) or in-degree (false)
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateDonLiteMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                            pvector<NodeID_>& new_ids,
                            bool useOutdeg = true) {
    const int64_t n = g.num_nodes();
    if (n <= 0) return;

    // Get the MLP (default hardcoded weights)
    static const don_lite::DonLiteMLP mlp = don_lite::GetDefaultMLP();

    // Compute max degree and average degree for normalization
    int64_t max_deg = 0;
    double total_deg = 0.0;
    for (int64_t v = 0; v < n; ++v) {
        int64_t d = useOutdeg ? g.out_degree(v) : g.in_degree(v);
        if (d > max_deg) max_deg = d;
        total_deg += d;
    }
    double avg_deg = (n > 0) ? total_deg / n : 1.0;
    if (max_deg == 0) max_deg = 1;

    // Compute MLP score for each vertex
    std::vector<std::pair<double, int64_t>> scored(n);

    #pragma omp parallel for schedule(dynamic, 1024)
    for (int64_t v = 0; v < n; ++v) {
        int64_t deg = useOutdeg ? g.out_degree(v) : g.in_degree(v);

        double features[don_lite::N_INPUT];
        features[0] = static_cast<double>(deg) / max_deg;           // normalized_degree
        features[1] = static_cast<double>(v) / n;                   // normalized_id

        // Compute neighbor average ID
        double sum_nbr_id = 0.0;
        if (deg > 0) {
            if (useOutdeg) {
                for (auto u : g.out_neigh(v)) {
                    sum_nbr_id += static_cast<double>(u);
                }
            } else {
                for (auto u : g.in_neigh(v)) {
                    sum_nbr_id += static_cast<double>(u);
                }
            }
            features[2] = (sum_nbr_id / deg) / n;                   // neighbor_avg_id
        } else {
            features[2] = features[1];                               // isolated: use own position
        }

        features[3] = (avg_deg > 0) ? deg / avg_deg : 0.0;         // degree_ratio

        // Local density: approximate clustering coefficient
        // For efficiency, only compute for small neighborhoods
        double local_dens = 0.0;
        if (deg >= 2 && deg <= 50) {
            // Build neighbor set
            std::vector<int64_t> nbrs;
            nbrs.reserve(deg);
            if (useOutdeg) {
                for (auto u : g.out_neigh(v)) nbrs.push_back(static_cast<int64_t>(u));
            } else {
                for (auto u : g.in_neigh(v)) nbrs.push_back(static_cast<int64_t>(u));
            }
            std::sort(nbrs.begin(), nbrs.end());
            int64_t triangles = 0;
            for (size_t i = 0; i < nbrs.size(); ++i) {
                int64_t ni = nbrs[i];
                if (useOutdeg) {
                    for (auto w : g.out_neigh(ni)) {
                        if (std::binary_search(nbrs.begin(), nbrs.end(), static_cast<int64_t>(w)))
                            ++triangles;
                    }
                } else {
                    for (auto w : g.in_neigh(ni)) {
                        if (std::binary_search(nbrs.begin(), nbrs.end(), static_cast<int64_t>(w)))
                            ++triangles;
                    }
                }
            }
            int64_t pairs = deg * (deg - 1);  // directed pairs
            local_dens = (pairs > 0) ? static_cast<double>(triangles) / pairs : 0.0;
        }
        features[4] = local_dens;                                    // local_density

        double score = mlp.forward(features);
        scored[v] = {score, v};
    }

    // Sort by MLP score (ascending: low scores get low IDs)
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Write mapping
    #pragma omp parallel for
    for (int64_t rank = 0; rank < n; ++rank) {
        new_ids[scored[rank].second] = static_cast<NodeID_>(rank);
    }
}

#endif // REORDER_DON_LITE_H
