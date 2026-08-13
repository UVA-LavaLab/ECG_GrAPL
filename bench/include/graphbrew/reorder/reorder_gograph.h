// ===========================================================================
// reorder_gograph.h — GoGraph M(σ)-maximizing vertex reordering (P3 3.4)
//
// Faithful implementation of the GoGraph algorithm from:
//   Zhou et al., "GoGraph: Fast Iterative Graph Computing with Updated
//   Neighbor States", IEEE TPDS 2024.
//
// Core idea: Maximize M(σ) = count of "positive edges" (u,v) where
// σ(u) < σ(v), i.e., source vertex precedes destination in the processing
// order. This improves Gauss-Seidel convergence for iterative algorithms
// (PageRank, SSSP) by ensuring more neighbors have fresh (updated) values
// when a vertex is processed.
//
// Algorithm (Algorithm 1 in paper):
//   1. DIVIDE PHASE:
//      - Extract hub vertices (top 0.2% by degree)
//      - Identify newly-isolated vertices (non-hubs with all neighbors
//        being hubs)
//      NOTE: Community partitioning (paper's Louvain/Metis) is omitted
//      because when used from AdaptiveOrder, Leiden is already applied.
//
//   2. CONQUER PHASE:
//      - BFS vertex selection order (starting from highest-degree vertex)
//      - Greedy insertion via GetOptVal(): for each vertex in BFS order,
//        find the insertion position that maximizes M(σ)
//
//   3. REINSERT:
//      - Reinsert hub vertices at M-optimal positions
//      - Reinsert isolated vertices at M-optimal positions
//
//   4. Sort by val[] to produce final permutation
//
// GetOptVal (Section IV-C in paper):
//   Given vertex v and the current ordered set O:
//   - Extract v's placed neighbors Nv, sort by val (ascending)
//   - Initialize pev = |OUT(v) ∩ O| (positive edges with v at HEAD)
//   - Scan left→right through sorted neighbors:
//     • OUT-neighbor: pev -= 1 (was positive, now negative)
//     • IN-neighbor:  pev += 1 (was negative, now positive)
//   - Track maximum pev ⇒ insertion position = midpoint of bounding vals
//
// Complexity: O(m · log d_max) for GetOptVal + O(n log n) for final sort
//
// Variants:
//   default — Optimized faithful implementation: flat delta array instead
//             of per-call unordered_map, merged pev computation (one fewer
//             neighbor scan), degree-1 short-circuit, wider renumber interval.
//   :naive  — Original faithful implementation: per-call unordered_map,
//             separate pev scan, renumber every 10K vertices. Useful for
//             validation and comparison with the optimized variant.
//   :fast   — Iterative flow-score sorting (O(n log n + m) per iter).
//             Heuristic alternative that sorts by forward-edge fraction.
//             Faster but does not implement the paper's greedy M-maximizing
//             insertion.
//
// NOTE: For symmetric/undirected graphs, M(σ) is constant for any ordering
// (each undirected edge contributes exactly 1 regardless of vertex order).
// GoGraph benefits primarily directed graphs or asymmetric CSR storage.
// ===========================================================================

#ifndef REORDER_GOGRAPH_H
#define REORDER_GOGRAPH_H

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

namespace gograph_impl {

/// Fraction of vertices considered "hubs" (top 0.2% by degree, per paper §IV-A)
constexpr double HUB_FRACTION = 0.002;

/// Minimum hubs to extract (for non-trivial graphs)
constexpr int64_t MIN_HUBS = 1;

// ---------------------------------------------------------------------------
// Pre-allocated buffers for optimized GetOptVal
// ---------------------------------------------------------------------------

/// Reusable context for optimized GetOptVal — eliminates per-call heap
/// allocations by maintaining a flat delta array and reusable entry buffer.
struct OptContext {
    std::vector<int> delta;         ///< Flat delta array (size n, zero-initialized)
    std::vector<int64_t> touched;   ///< Modified indices in delta (for cleanup)
    struct Entry { double pos; int d; };
    std::vector<Entry> entries;     ///< Sorted neighbor entries (reusable buffer)

    void init(int64_t n, int64_t reserve_deg) {
        delta.assign(static_cast<size_t>(n), 0);
        touched.reserve(static_cast<size_t>(std::min(reserve_deg, n)));
        entries.reserve(static_cast<size_t>(std::min(reserve_deg, n)));
    }
};

// ---------------------------------------------------------------------------
// GetOptVal — OPTIMIZED variant
//
// Key improvements over naive:
//   1. Pre-allocated flat delta array (vector<int>) instead of per-call
//      unordered_map — eliminates O(d) heap allocations per vertex.
//   2. Merged pev computation — initial pev (|OUT(v) ∩ O|) is computed
//      during the delta-building loop instead of a separate third scan.
//   3. Degree-1 short-circuit — single placed neighbor has trivially
//      optimal position (before for out-only, after for in-only).
//   4. Pre-allocated entries buffer — reuses memory across calls.
// ---------------------------------------------------------------------------

/**
 * @brief Optimized GetOptVal — M-optimal insertion position for vertex v.
 *
 * @param v          Vertex to insert
 * @param g          Input CSR graph
 * @param val        Current position values
 * @param placed     Placement flags
 * @param default_val Value when no placed neighbors
 * @param ctx        Pre-allocated context (delta array, touched list, entries)
 * @return Optimal val for vertex v
 */
template <typename NodeID_, typename DestID_, bool invert>
inline double GetOptVal(int64_t v,
                        const CSRGraph<NodeID_, DestID_, invert>& g,
                        const std::vector<double>& val,
                        const std::vector<bool>& placed,
                        double default_val,
                        OptContext& ctx) {
    auto& delta_buf = ctx.delta;
    auto& touched = ctx.touched;

    // ---- Collect placed neighbors, compute deltas, AND initial pev ----
    // pev = |OUT(v) ∩ placed| (positive edges when v is at HEAD)
    int64_t pev = 0;

    for (auto u : g.out_neigh(v)) {
        int64_t uid = static_cast<int64_t>(u);
        if (placed[uid]) {
            if (delta_buf[uid] == 0) touched.push_back(uid);
            delta_buf[uid] -= 1;   // OUT-neighbor: moving past loses positive edge
            ++pev;                 // Placed out-neighbor contributes to initial pev
        }
    }
    if constexpr (invert) {
        for (auto u : g.in_neigh(v)) {
            int64_t uid = static_cast<int64_t>(u);
            if (placed[uid]) {
                if (delta_buf[uid] == 0) touched.push_back(uid);
                delta_buf[uid] += 1;   // IN-neighbor: moving past gains positive edge
            }
        }
    }

    double result;

    if (touched.empty()) {
        // No placed neighbors — append at tail
        result = default_val;
    } else if (touched.size() == 1) {
        // ---- Degree-1 short-circuit ----
        // Single placed neighbor: optimal position is trivially determined.
        // d < 0: out-only  → best BEFORE neighbor (pev stays high)
        // d > 0: in-only   → best AFTER neighbor (gains positive edge)
        // d == 0: bidirectional → head (pev unchanged by passing)
        double nval = val[touched[0]];
        int d = delta_buf[touched[0]];
        result = (d > 0) ? (nval + 1.0) : (nval - 1.0);
    } else {
        // ---- General case: sort by val and scan for best position ----
        auto& entries = ctx.entries;
        entries.clear();
        for (int64_t uid : touched) {
            entries.push_back({val[uid], delta_buf[uid]});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const OptContext::Entry& a, const OptContext::Entry& b) {
                      return a.pos < b.pos;
                  });

        int64_t best_pev = pev;
        result = entries[0].pos - 1.0;  // Before first neighbor

        for (size_t i = 0; i < entries.size(); ++i) {
            pev += entries[i].d;
            if (pev > best_pev) {
                best_pev = pev;
                if (i + 1 < entries.size()) {
                    result = (entries[i].pos + entries[i + 1].pos) / 2.0;
                } else {
                    result = entries.back().pos + 1.0;
                }
            }
        }
    }

    // ---- Cleanup: zero out touched delta entries for reuse ----
    for (int64_t uid : touched) delta_buf[uid] = 0;
    touched.clear();

    return result;
}

// ---------------------------------------------------------------------------
// GetOptValNaive — ORIGINAL variant (faithful but unoptimized)
//
// Uses per-call unordered_map for delta collection and separate pev scan.
// Kept for :naive variant and comparison/validation.
// ---------------------------------------------------------------------------

template <typename NodeID_, typename DestID_, bool invert>
inline double GetOptValNaive(int64_t v,
                             const CSRGraph<NodeID_, DestID_, invert>& g,
                             const std::vector<double>& val,
                             const std::vector<bool>& placed,
                             double default_val) {
    std::unordered_map<int64_t, int> nbr_delta;

    for (auto u : g.out_neigh(v)) {
        int64_t uid = static_cast<int64_t>(u);
        if (placed[uid]) nbr_delta[uid] -= 1;
    }
    if constexpr (invert) {
        for (auto u : g.in_neigh(v)) {
            int64_t uid = static_cast<int64_t>(u);
            if (placed[uid]) nbr_delta[uid] += 1;
        }
    }

    if (nbr_delta.empty()) return default_val;

    struct Entry { double pos; int delta; };
    std::vector<Entry> entries;
    entries.reserve(nbr_delta.size());
    for (auto& [uid, delta] : nbr_delta) {
        entries.push_back({val[uid], delta});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.pos < b.pos; });

    int64_t pev = 0;
    for (auto u : g.out_neigh(v)) {
        if (placed[static_cast<int64_t>(u)]) ++pev;
    }

    int64_t best_pev = pev;
    double best_val = entries[0].pos - 1.0;

    for (size_t i = 0; i < entries.size(); ++i) {
        pev += entries[i].delta;
        if (pev > best_pev) {
            best_pev = pev;
            if (i + 1 < entries.size()) {
                best_val = (entries[i].pos + entries[i + 1].pos) / 2.0;
            } else {
                best_val = entries.back().pos + 1.0;
            }
        }
    }

    return best_val;
}

} // namespace gograph_impl

// =============================================================================
// IMPLEMENTATION: GoGraph M-maximizing reordering — parameterized by mode
//
// Template parameter 'optimized':
//   true  → uses GetOptVal with flat delta array + OptContext (default)
//   false → uses GetOptValNaive with per-call unordered_map (:naive)
// =============================================================================

template <bool optimized, typename NodeID_, typename DestID_, bool invert>
void GenerateGoGraphMappingImpl(const CSRGraph<NodeID_, DestID_, invert>& g,
                                pvector<NodeID_>& new_ids,
                                bool /*useOutdeg*/ = true) {
    const int64_t n = g.num_nodes();
    if (n <= 0) return;

    using namespace gograph_impl;

    // -------------------------------------------------------------------
    // Phase 1: Extract hub vertices and identify isolated vertices
    // Paper §III-B: "extract the top 0.2% vertices with the highest degree"
    // -------------------------------------------------------------------

    // Compute total degree for each vertex
    std::vector<std::pair<int64_t, int64_t>> degree_vertex(n);
    for (int64_t v = 0; v < n; ++v) {
        int64_t deg = g.out_degree(v);
        if constexpr (invert) deg += g.in_degree(v);
        degree_vertex[v] = {deg, v};
    }

    // Top HUB_FRACTION by degree
    int64_t hub_count = std::max(MIN_HUBS, static_cast<int64_t>(n * HUB_FRACTION));
    hub_count = std::min(hub_count, n);

    std::partial_sort(degree_vertex.begin(),
                      degree_vertex.begin() + hub_count,
                      degree_vertex.end(),
                      [](const auto& a, const auto& b) {
                          return a.first > b.first;
                      });

    std::vector<bool> is_hub(n, false);
    for (int64_t i = 0; i < hub_count; ++i) {
        is_hub[degree_vertex[i].second] = true;
    }

    // Classify non-hub vertices:
    //   - Regular: have at least one non-hub neighbor (participate in Phase 2)
    //   - Isolated: ALL neighbors are hubs (or degree 0); reinserted in Phase 4
    // Paper §III-B: "after removing high-degree vertices, there will appear
    //   some isolated vertices that have no edges with other vertices"
    std::vector<int64_t> regular_vertices;
    std::vector<int64_t> hub_vertices;
    std::vector<int64_t> isolated_vertices;

    regular_vertices.reserve(n);

    for (int64_t v = 0; v < n; ++v) {
        if (is_hub[v]) {
            hub_vertices.push_back(v);
            continue;
        }
        bool has_non_hub_neighbor = false;
        for (auto u : g.out_neigh(v)) {
            if (!is_hub[static_cast<int64_t>(u)]) {
                has_non_hub_neighbor = true;
                break;
            }
        }
        if constexpr (invert) {
            if (!has_non_hub_neighbor) {
                for (auto u : g.in_neigh(v)) {
                    if (!is_hub[static_cast<int64_t>(u)]) {
                        has_non_hub_neighbor = true;
                        break;
                    }
                }
            }
        }
        if (has_non_hub_neighbor) {
            regular_vertices.push_back(v);
        } else {
            isolated_vertices.push_back(v);
        }
    }

    // -------------------------------------------------------------------
    // Phase 2: BFS ordering + GetOptVal greedy insertion (regular vertices)
    // Paper §III-B: "select the vertex v with BFS, so that v has better
    //   locality with the vertices in O^c_Vi"
    // -------------------------------------------------------------------

    std::vector<double> val(n, 0.0);
    std::vector<bool> placed(n, false);
    double global_min_val = 0.0;
    double global_max_val = 0.0;

    // Mode-specific: allocate pre-allocated buffers for optimized path
    [[maybe_unused]] OptContext ctx;
    if constexpr (optimized) {
        int64_t max_deg = (n > 0) ? degree_vertex[0].first : 0;
        ctx.init(n, max_deg);
    }

    // Renumber interval: optimized can go wider (fewer renumbers, lower overhead)
    constexpr size_t RENUMBER_INTERVAL = optimized ? 100000 : 10000;

    // Dispatch helper: calls the appropriate GetOptVal variant
    auto get_opt_val = [&](int64_t v, double def) -> double {
        if constexpr (optimized) {
            return GetOptVal<NodeID_, DestID_, invert>(
                v, g, val, placed, def, ctx);
        } else {
            return GetOptValNaive<NodeID_, DestID_, invert>(
                v, g, val, placed, def);
        }
    };

    if (!regular_vertices.empty()) {
        // BFS from highest-degree regular vertex for good coverage
        int64_t bfs_start = regular_vertices[0];
        int64_t max_deg = -1;
        for (int64_t v : regular_vertices) {
            int64_t deg = g.out_degree(v);
            if constexpr (invert) deg += g.in_degree(v);
            if (deg > max_deg) {
                max_deg = deg;
                bfs_start = v;
            }
        }

        // BFS traversal (skip hubs and isolated vertices)
        std::vector<int64_t> bfs_order;
        bfs_order.reserve(regular_vertices.size());
        std::vector<bool> visited(n, false);

        // Mark hubs and isolated as visited so BFS skips them
        for (int64_t v : hub_vertices) visited[v] = true;
        for (int64_t v : isolated_vertices) visited[v] = true;

        std::queue<int64_t> q;
        visited[bfs_start] = true;
        q.push(bfs_start);

        while (!q.empty()) {
            int64_t u = q.front();
            q.pop();
            bfs_order.push_back(u);

            for (auto w : g.out_neigh(u)) {
                int64_t wid = static_cast<int64_t>(w);
                if (!visited[wid]) {
                    visited[wid] = true;
                    q.push(wid);
                }
            }
            if constexpr (invert) {
                for (auto w : g.in_neigh(u)) {
                    int64_t wid = static_cast<int64_t>(w);
                    if (!visited[wid]) {
                        visited[wid] = true;
                        q.push(wid);
                    }
                }
            }
        }

        // Handle disconnected components: add unvisited regular vertices
        for (int64_t v : regular_vertices) {
            if (!visited[v]) {
                bfs_order.push_back(v);
            }
        }

        // Greedy M-maximizing insertion (paper Algorithm 1, lines 4-8)
        // For each vertex in BFS order, find optimal placement via GetOptVal
        if (!bfs_order.empty()) {
            val[bfs_order[0]] = 0.0;
            placed[bfs_order[0]] = true;

            for (size_t i = 1; i < bfs_order.size(); ++i) {
                int64_t v = bfs_order[i];
                double default_val = global_max_val + 1.0;
                double opt_val = get_opt_val(v, default_val);
                val[v] = opt_val;
                placed[v] = true;
                global_min_val = std::min(global_min_val, opt_val);
                global_max_val = std::max(global_max_val, opt_val);

                // Periodic renumbering to prevent floating-point precision loss
                // (midpoint (a+b)/2 loses precision after ~52 halvings)
                if (i % RENUMBER_INTERVAL == 0) {
                    // Collect placed vertices sorted by val
                    std::vector<std::pair<double, int64_t>> sorted_placed;
                    sorted_placed.reserve(i + 1);
                    for (size_t j = 0; j <= i; ++j) {
                        sorted_placed.push_back({val[bfs_order[j]], bfs_order[j]});
                    }
                    std::sort(sorted_placed.begin(), sorted_placed.end());
                    // Reassign evenly-spaced integer vals
                    for (size_t j = 0; j < sorted_placed.size(); ++j) {
                        val[sorted_placed[j].second] = static_cast<double>(j);
                    }
                    global_min_val = 0.0;
                    global_max_val = static_cast<double>(sorted_placed.size() - 1);
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // Phase 3: Reinsert hub vertices at M-optimal positions
    // Paper Algorithm 1, lines 30-32
    // -------------------------------------------------------------------

    // Sort hubs by degree descending (highest-degree first)
    std::sort(hub_vertices.begin(), hub_vertices.end(),
              [&](int64_t a, int64_t b) {
                  int64_t da = g.out_degree(a);
                  int64_t db = g.out_degree(b);
                  if constexpr (invert) {
                      da += g.in_degree(a);
                      db += g.in_degree(b);
                  }
                  return da > db;
              });

    for (int64_t v : hub_vertices) {
        double default_val = global_max_val + 1.0;
        double opt_val = get_opt_val(v, default_val);
        val[v] = opt_val;
        placed[v] = true;
        global_min_val = std::min(global_min_val, opt_val);
        global_max_val = std::max(global_max_val, opt_val);
    }

    // -------------------------------------------------------------------
    // Phase 4: Reinsert isolated vertices at M-optimal positions
    // Paper Algorithm 1, lines 33-35
    // -------------------------------------------------------------------

    for (int64_t v : isolated_vertices) {
        double default_val = global_max_val + 1.0;
        double opt_val = get_opt_val(v, default_val);
        val[v] = opt_val;
        placed[v] = true;
        global_min_val = std::min(global_min_val, opt_val);
        global_max_val = std::max(global_max_val, opt_val);
    }

    // -------------------------------------------------------------------
    // Phase 5: Sort by val to produce final permutation
    // Paper Algorithm 1, line 36: "Sort OV in ascending order of v.val"
    // -------------------------------------------------------------------

    std::vector<std::pair<double, int64_t>> val_vertex(n);
    for (int64_t v = 0; v < n; ++v) {
        val_vertex[v] = {val[v], v};
    }
    std::sort(val_vertex.begin(), val_vertex.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Write output: new_ids[old_vertex] = new_rank
    #pragma omp parallel for
    for (int64_t rank = 0; rank < n; ++rank) {
        new_ids[val_vertex[rank].second] = static_cast<NodeID_>(rank);
    }
}

// =============================================================================
// PUBLIC API: Default (optimized) and Naive variants
// =============================================================================

/**
 * @brief GoGraph M(σ)-maximizing reordering — optimized (default variant).
 *
 * Uses flat delta array, merged pev computation, degree-1 short-circuit,
 * and wider renumber interval for better performance. Produces identical
 * orderings to :naive (same algorithm, just faster data structures).
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateGoGraphMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                            pvector<NodeID_>& new_ids,
                            bool useOutdeg = true) {
    GenerateGoGraphMappingImpl<true, NodeID_, DestID_, invert>(g, new_ids, useOutdeg);
}

/**
 * @brief GoGraph M(σ)-maximizing reordering — naive variant (:naive).
 *
 * Original faithful implementation with per-call unordered_map and
 * separate pev scan. Useful for validation and comparison against
 * the optimized default variant.
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateGoGraphNaiveMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                                 pvector<NodeID_>& new_ids,
                                 bool useOutdeg = true) {
    GenerateGoGraphMappingImpl<false, NodeID_, DestID_, invert>(g, new_ids, useOutdeg);
}

// =============================================================================
// FAST VARIANT: Iterative flow-score sorting (:fast)
// =============================================================================

/**
 * @brief GoGraph fast variant — iterative flow-score sorting.
 *
 * Heuristic that iteratively:
 *   1. Computes "flow score": fraction of neighbors with higher rank
 *   2. Sorts vertices by descending flow score
 *   3. Repeats for 3 iterations (typically converges)
 *
 * Faster than GetOptVal but less faithful to the paper's M-maximizing
 * approach. Useful as a quick approximation.
 *
 * Complexity: O(n log n + m) per iteration.
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateGoGraphFastMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                                pvector<NodeID_>& new_ids,
                                bool useOutdeg = true) {
    const int64_t n = g.num_nodes();
    if (n <= 0) return;

    constexpr int MAX_ITERS = 3;

    // perm[rank] = old_vertex_id
    std::vector<int64_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);

    // inv_perm[old_id] = current_rank
    std::vector<int64_t> inv_perm(n);
    std::iota(inv_perm.begin(), inv_perm.end(), 0);

    for (int iter = 0; iter < MAX_ITERS; ++iter) {
        // flow_score[v] = (# neighbors with higher rank) / degree(v)
        std::vector<double> flow_score(n, 0.0);

        #pragma omp parallel for schedule(dynamic, 1024)
        for (int64_t v = 0; v < n; ++v) {
            int64_t deg = useOutdeg ? g.out_degree(v) : g.in_degree(v);
            if (deg == 0) {
                flow_score[v] = 0.5;
                continue;
            }
            int64_t forward_count = 0;
            int64_t rank_v = inv_perm[v];
            if (useOutdeg) {
                for (auto u : g.out_neigh(v)) {
                    if (inv_perm[static_cast<int64_t>(u)] > rank_v)
                        ++forward_count;
                }
            } else {
                for (auto u : g.in_neigh(v)) {
                    if (inv_perm[static_cast<int64_t>(u)] > rank_v)
                        ++forward_count;
                }
            }
            flow_score[v] = static_cast<double>(forward_count) / deg;
        }

        // Sort by descending flow score, tie-break by degree
        std::sort(perm.begin(), perm.end(),
                  [&](int64_t a, int64_t b) {
                      if (flow_score[a] != flow_score[b])
                          return flow_score[a] > flow_score[b];
                      return (useOutdeg ? g.out_degree(a) : g.in_degree(a))
                           > (useOutdeg ? g.out_degree(b) : g.in_degree(b));
                  });

        for (int64_t rank = 0; rank < n; ++rank) {
            inv_perm[perm[rank]] = rank;
        }
    }

    #pragma omp parallel for
    for (int64_t v = 0; v < n; ++v) {
        new_ids[v] = static_cast<NodeID_>(inv_perm[v]);
    }
}

#endif // REORDER_GOGRAPH_H
