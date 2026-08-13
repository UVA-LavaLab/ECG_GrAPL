/**
 * @file reorder_rabbit.h
 * @brief RabbitOrder Algorithm Implementation (Algorithm ID: 8)
 * 
 * RabbitOrder is a community-aware reordering algorithm that:
 *   1. Detects communities using hierarchical agglomeration
 *   2. Orders vertices within communities to maximize locality
 *   3. Places related communities adjacent in memory
 * 
 * Paper: "Rabbit Order: Just-in-time Parallel Reordering for Fast Graph Analysis"
 *        by J. Arai et al., IPDPS 2016
 * 
 * Best for: Social networks, web graphs, and graphs with community structure
 * 
 * Complexity: O(n log n + m) approximately
 * 
 * @note Requires RABBIT_ENABLE macro and Boost library for full functionality.
 *       Falls back to original ordering if RABBIT_ENABLE is not defined.
 */

#ifndef REORDER_RABBIT_H_
#define REORDER_RABBIT_H_

#include <vector>
#include <memory>
#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <atomic>
#include <cstring>
#include <deque>
#include <omp.h>
#include <parallel/algorithm>

#include <pvector.h>
#include <timer.h>
#include <graph.h>

#ifdef RABBIT_ENABLE
#include <rabbit/edge_list.hpp>
#include <rabbit/rabbit_order.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm.hpp>
#endif

// Include the full graph header instead of forward declaration
// CSRGraph has 6 template parameters which makes forward declaration complex
#include <graph.h>

// ============================================================================
// TYPE DEFINITIONS
// ============================================================================

#ifdef RABBIT_ENABLE
/**
 * @brief Adjacency list representation for RabbitOrder
 * 
 * Vector of vectors where each inner vector contains pairs of
 * (neighbor_id, edge_weight) for efficient community detection.
 */
using adjacency_list = std::vector<std::vector<std::pair<rabbit_order::vint, float>>>;
#endif

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

#ifdef RABBIT_ENABLE

/**
 * @brief Count unique elements in a range
 * 
 * @tparam InputIt Input iterator type
 * @param f First iterator
 * @param l Last iterator
 * @return Number of unique elements
 */
template <typename InputIt>
typename std::iterator_traits<InputIt>::difference_type
count_uniq_rabbit(const InputIt f, const InputIt l) {
    std::vector<typename std::iterator_traits<InputIt>::value_type> ys(f, l);
    return boost::size(boost::unique(boost::sort(ys)));
}

/**
 * @brief Compute modularity of a community assignment
 * 
 * Modularity measures the quality of a community partition.
 * Higher values indicate better community structure.
 * 
 * Q = Σ[e_ii - a_i²] where:
 *   - e_ii = fraction of edges within community i
 *   - a_i = fraction of edges incident to community i
 * 
 * @param adj Adjacency list representation
 * @param coms Community assignment for each vertex
 * @return Modularity value in range [-0.5, 1.0]
 */
inline double compute_modularity_rabbit(const adjacency_list& adj,
                                        const rabbit_order::vint* const coms) {
    const rabbit_order::vint n = static_cast<rabbit_order::vint>(adj.size());
    double m2 = 0.0;  // Total weight of bidirectional edges
    
    // Find max community ID for array sizing
    rabbit_order::vint max_com = 0;
    #pragma omp parallel for reduction(max : max_com)
    for (rabbit_order::vint v = 0; v < n; ++v) {
        if (coms[v] > max_com) max_com = coms[v];
    }
    const size_t com_array_size = static_cast<size_t>(max_com) + 1;
    
    // Use flat arrays for better cache performance
    std::vector<double> degs_all(com_array_size, 0.0);
    std::vector<double> degs_loop(com_array_size, 0.0);
    
    // Thread-local arrays to avoid critical sections
    const int num_threads = omp_get_max_threads();
    std::vector<std::vector<double>> thread_degs_all(num_threads,
        std::vector<double>(com_array_size, 0.0));
    std::vector<std::vector<double>> thread_degs_loop(num_threads,
        std::vector<double>(com_array_size, 0.0));
    
    #pragma omp parallel reduction(+ : m2)
    {
        int tid = omp_get_thread_num();
        auto& local_all = thread_degs_all[tid];
        auto& local_loop = thread_degs_loop[tid];
        
        #pragma omp for schedule(dynamic, 1024)
        for (rabbit_order::vint v = 0; v < n; ++v) {
            const rabbit_order::vint c = coms[v];
            for (const auto& e : adj[v]) {
                m2 += e.second;
                local_all[c] += e.second;
                if (coms[e.first] == c)
                    local_loop[c] += e.second;
            }
        }
    }
    
    // Parallel reduction of thread-local arrays
    #pragma omp parallel for schedule(static)
    for (size_t c = 0; c < com_array_size; ++c) {
        for (int t = 0; t < num_threads; ++t) {
            degs_all[c] += thread_degs_all[t][c];
            degs_loop[c] += thread_degs_loop[t][c];
        }
    }
    
    // Compute modularity
    double q = 0.0;
    #pragma omp parallel for reduction(+ : q) schedule(static, 1024)
    for (size_t c = 0; c < com_array_size; ++c) {
        const double all = degs_all[c];
        const double loop = degs_loop[c];
        if (all > 0.0) {
            q += loop / m2 - (all / m2) * (all / m2);
        }
    }
    
    return q;
}

/**
 * @brief Detect communities and print results
 * 
 * Used for analysis and debugging purposes.
 * 
 * @param adj Adjacency list to process
 */
inline void detect_community_rabbit(adjacency_list adj) {
    auto _adj = adj;  // Copy for modularity computation
    
    const double tstart = rabbit_order::now_sec();
    auto g = rabbit_order::aggregate(std::move(_adj));
    const auto c = std::make_unique<rabbit_order::vint[]>(g.n());
    
    #pragma omp parallel for
    for (rabbit_order::vint v = 0; v < g.n(); ++v)
        c[v] = rabbit_order::trace_com(v, &g);
    
    PrintTime("Community Time", rabbit_order::now_sec() - tstart);
    
    const double q = compute_modularity_rabbit(adj, c.get());
    std::cerr << "Modularity: " << q << std::endl;
}

/**
 * @brief Generate reordering and print permutation to stdout
 * 
 * @param adj Adjacency list to process
 */
inline void reorder_rabbit(adjacency_list adj) {
    const double tstart = rabbit_order::now_sec();
    const auto g = rabbit_order::aggregate(std::move(adj));
    const auto p = rabbit_order::compute_perm(g);
    PrintTime("Permutation generation Time", rabbit_order::now_sec() - tstart);
    
    std::copy(&p[0], &p[g.n()],
              std::ostream_iterator<rabbit_order::vint>(std::cout, "\n"));
}

/**
 * @brief Internal reordering with full statistics
 * 
 * Generates permutation and computes modularity/community stats.
 * 
 * @tparam NodeID_ Node identifier type
 * @param adj Adjacency list to process
 * @param new_ids Output permutation array
 */
template <typename NodeID_>
void reorder_internal_rabbit(adjacency_list adj, pvector<NodeID_>& new_ids) {
    auto _adj = adj;  // Copy for modularity computation
    
    const double tstart = rabbit_order::now_sec();
    auto g = rabbit_order::aggregate(std::move(_adj));
    const auto p = rabbit_order::compute_perm(g);
    const double tend = rabbit_order::now_sec();
    
    // Compute community assignments for statistics
    const auto c = std::make_unique<rabbit_order::vint[]>(g.n());
    #pragma omp parallel for
    for (rabbit_order::vint v = 0; v < g.n(); ++v)
        c[v] = rabbit_order::trace_com(v, &g);
    
    const double q = compute_modularity_rabbit(adj, c.get());
    
    // Count unique communities
    std::unordered_set<rabbit_order::vint> unique_comms;
    for (rabbit_order::vint v = 0; v < g.n(); ++v)
        unique_comms.insert(c[v]);
    const size_t num_communities = unique_comms.size();
    
    PrintTime("RabbitOrder Communities", static_cast<double>(num_communities));
    PrintTime("RabbitOrder Modularity", q);
    PrintTime("RabbitOrder Map Time", tend - tstart);
    
    // Ensure output is large enough
    if (new_ids.size() < g.n())
        new_ids.resize(g.n());
    
    #pragma omp parallel for
    for (size_t i = 0; i < g.n(); ++i) {
        new_ids[i] = static_cast<NodeID_>(p[i]);
    }
}

/**
 * @brief Internal reordering for sub-graphs (minimal output)
 * 
 * Optimized version without modularity computation for use in
 * hierarchical/sub-graph reordering.
 * 
 * @tparam NodeID_ Node identifier type
 * @param adj Adjacency list to process
 * @param new_ids Output permutation array
 */
template <typename NodeID_>
void reorder_internal_single_rabbit(adjacency_list adj, pvector<NodeID_>& new_ids) {
    auto _adj = adj;
    
    const double tstart = rabbit_order::now_sec();
    auto g = rabbit_order::aggregate(std::move(_adj));
    const auto p = rabbit_order::compute_perm(g);
    const double tend = rabbit_order::now_sec();
    
    // Compute community assignments (used internally)
    const auto c = std::make_unique<rabbit_order::vint[]>(g.n());
    #pragma omp parallel for
    for (rabbit_order::vint v = 0; v < g.n(); ++v)
        c[v] = rabbit_order::trace_com(v, &g);
    
    PrintTime("Sub-RabbitOrder Map Time", tend - tstart);
    
    if (new_ids.size() < g.n())
        new_ids.resize(g.n());
    
    #pragma omp parallel for
    for (size_t i = 0; i < g.n(); ++i) {
        new_ids[i] = static_cast<NodeID_>(p[i]);
    }
}

#endif  // RABBIT_ENABLE

// ============================================================================
// GRAPH CONVERSION FUNCTIONS
// ============================================================================

#ifdef RABBIT_ENABLE

/**
 * @brief Convert CSR graph to RabbitOrder adjacency list format
 * 
 * @tparam NodeID_ Node identifier type
 * @tparam DestID_ Destination identifier type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether graph stores incoming edges
 * @param g Input CSR graph
 * @return Adjacency list for RabbitOrder processing
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
adjacency_list readRabbitOrderGraphCSR(const CSRGraph<NodeID_, DestID_, invert>& g) {
    using boost::adaptors::transformed;
    
    const int64_t num_nodes = g.num_nodes();
    adjacency_list adj(num_nodes);
    
    #pragma omp parallel for schedule(dynamic, 1024)
    for (NodeID_ i = 0; i < num_nodes; ++i) {
        adj[i].reserve(g.out_degree(i));
        for (DestID_ neighbor : g.out_neigh(i)) {
            if (g.is_weighted()) {
                NodeID_ dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                float weight = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).w;
                adj[i].emplace_back(dest, weight);
            } else {
                adj[i].emplace_back(static_cast<rabbit_order::vint>(neighbor), 1.0f);
            }
        }
    }
    
    return adj;
}

/**
 * @brief Convert edge list to RabbitOrder adjacency list format
 * 
 * @param edges Input edge list (src, dest, weight tuples)
 * @return Adjacency list for RabbitOrder processing
 */
inline adjacency_list readRabbitOrderAdjacencylist(
    const std::vector<edge_list::edge>& edges) {
    
    // Find number of vertices
    rabbit_order::vint max_v = 0;
    #pragma omp parallel for reduction(max : max_v)
    for (size_t i = 0; i < edges.size(); ++i) {
        rabbit_order::vint src = std::get<0>(edges[i]);
        rabbit_order::vint dst = std::get<1>(edges[i]);
        if (src > max_v) max_v = src;
        if (dst > max_v) max_v = dst;
    }
    
    const size_t n = static_cast<size_t>(max_v) + 1;
    adjacency_list adj(n);
    
    // Count degrees first for efficient allocation
    std::vector<size_t> degrees(n, 0);
    for (const auto& e : edges) {
        ++degrees[std::get<0>(e)];
    }
    
    // Reserve space
    for (size_t i = 0; i < n; ++i) {
        adj[i].reserve(degrees[i]);
    }
    
    // Fill adjacency list
    for (const auto& e : edges) {
        adj[std::get<0>(e)].emplace_back(std::get<1>(e), std::get<2>(e));
    }
    
    return adj;
}

#endif  // RABBIT_ENABLE

// ============================================================================
// MAIN API FUNCTIONS
// ============================================================================

/**
 * @brief Generate RabbitOrder vertex permutation
 * 
 * RabbitOrder combines community detection with cache-aware ordering.
 * It detects communities using hierarchical agglomeration, then orders
 * vertices to maximize spatial locality within and across communities.
 * 
 * Algorithm steps:
 *   1. Build hierarchical community structure via graph aggregation
 *   2. Compute vertex permutation respecting community boundaries
 *   3. Order vertices within communities for cache efficiency
 * 
 * @tparam NodeID_ Node identifier type
 * @tparam DestID_ Destination identifier type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether graph stores incoming edges
 * @param g Input CSR graph
 * @param new_ids Output: new_ids[old_id] = new_id mapping
 * 
 * @note Falls back to original ordering if RABBIT_ENABLE is not defined
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateRabbitOrderMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                                pvector<NodeID_>& new_ids) {
#ifdef RABBIT_ENABLE
    auto adj = readRabbitOrderGraphCSR<NodeID_, DestID_, WeightT_, invert>(g);
    reorder_internal_rabbit<NodeID_>(std::move(adj), new_ids);
#else
    // Fallback to original ordering
    Timer t;
    t.Start();
    const int64_t num_nodes = g.num_nodes();
    #pragma omp parallel for
    for (int64_t i = 0; i < num_nodes; i++) {
        new_ids[i] = static_cast<NodeID_>(i);
    }
    t.Stop();
    PrintTime("RabbitOrder (disabled) Map Time", t.Seconds());
#endif
}

#ifdef RABBIT_ENABLE
/**
 * @brief Generate RabbitOrder mapping from edge list
 * 
 * Alternative entry point when graph is in edge list format.
 * 
 * @tparam NodeID_ Node identifier type
 * @param edges Input edge list
 * @param new_ids Output permutation
 */
template <typename NodeID_>
void GenerateRabbitOrderMappingEdgelist(const std::vector<edge_list::edge>& edges,
                                        pvector<NodeID_>& new_ids) {
    auto adj = readRabbitOrderAdjacencylist(edges);
    reorder_internal_single_rabbit<NodeID_>(std::move(adj), new_ids);
}

/**
 * @brief Compute modularity of an edge list
 * 
 * @tparam NodeID_ Node identifier type
 * @tparam WeightT_ Edge weight type
 * @param edgesList Input edge list
 * @param is_weighted Whether edges have weights
 * @return Modularity value
 */
template <typename NodeID_, typename WeightT_>
double GenerateRabbitModularityEdgelist(
    const std::vector<std::tuple<NodeID_, NodeID_, WeightT_>>& edgesList,
    bool is_weighted) {
    
    std::vector<edge_list::edge> edges(edgesList.size());
    
    #pragma omp parallel for
    for (size_t i = 0; i < edges.size(); ++i) {
        rabbit_order::vint src = std::get<0>(edgesList[i]);
        rabbit_order::vint dest = std::get<1>(edgesList[i]);
        float weight = is_weighted ? static_cast<float>(std::get<2>(edgesList[i])) : 1.0f;
        edges[i] = std::make_tuple(src, dest, weight);
    }
    
    auto adj = readRabbitOrderAdjacencylist(edges);
    auto _adj = adj;
    
    auto g = rabbit_order::aggregate(std::move(_adj));
    const auto c = std::make_unique<rabbit_order::vint[]>(g.n());
    
    #pragma omp parallel for
    for (rabbit_order::vint v = 0; v < g.n(); ++v)
        c[v] = rabbit_order::trace_com(v, &g);
    
    return compute_modularity_rabbit(adj, c.get());
}
#endif  // RABBIT_ENABLE

// ============================================================================
// RABBITORDERCSR - Native CSR Implementation (No Boost dependency)
// 
// Faithful implementation following the IPDPS 2016 paper:
// "Rabbit Order: Just-in-time Parallel Reordering for Fast Graph Analysis"
// by Arai et al.
//
// Algorithm overview:
// 1. Community Detection via Parallel Incremental Aggregation
//    - Process vertices in increasing order of degree
//    - For each vertex u, find best neighbor v that maximizes ΔQ(u,v)
//    - If ΔQ > 0, merge u into v (lazy aggregation with CAS)
//    - Build dendrogram during merging
//
// 2. Ordering Generation via DFS on Dendrogram
//    - Perform DFS from each top-level vertex
//    - Assign new IDs in DFS visit order
//    - Concatenate orderings from all communities
//
// Modularity gain formula (Equation 1 in paper):
//   ΔQ(u,v) = 2 * (w_uv / (2m) - d(u)*d(v) / (2m)^2)
// ============================================================================

/**
 * Packed atom structure for atomic CAS (must be 8 bytes for lock-free CAS)
 */
struct RabbitCSRAtomPacked {
    float str;       // Total weighted degree of community (negative = locked)
    uint32_t child;  // Last vertex merged into this vertex (UINT32_MAX = none)
    
    RabbitCSRAtomPacked() = default;
    RabbitCSRAtomPacked(float s, uint32_t c) : str(s), child(c) {}
    RabbitCSRAtomPacked(const RabbitCSRAtomPacked&) = default;
    RabbitCSRAtomPacked& operator=(const RabbitCSRAtomPacked&) = default;
};
static_assert(sizeof(RabbitCSRAtomPacked) == 8, "RabbitCSRAtomPacked must be 8 bytes");
static_assert(std::is_trivially_copyable<RabbitCSRAtomPacked>::value, "RabbitCSRAtomPacked must be trivially copyable");

/**
 * Vertex structure for RabbitOrderCSR with atomic CAS support
 */
struct RabbitCSRVertex {
    std::atomic<uint64_t> atom_raw;  // Packed {str, child} for atomic CAS
    std::atomic<uint32_t> sibling;   // Previous vertex merged to same destination
    uint32_t united_child;           // Last child that has been edge-aggregated
    
    RabbitCSRVertex() : atom_raw(0), sibling(UINT32_MAX), united_child(UINT32_MAX) {
        RabbitCSRAtomPacked init(0.0f, UINT32_MAX);
        atom_raw.store(pack_atom(init), std::memory_order_relaxed);
    }
    
    static uint64_t pack_atom(const RabbitCSRAtomPacked& a) {
        uint64_t result;
        static_assert(sizeof(RabbitCSRAtomPacked) == sizeof(uint64_t), "Size mismatch");
        memcpy(&result, &a, sizeof(uint64_t));
        return result;
    }
    
    static RabbitCSRAtomPacked unpack_atom(uint64_t raw) {
        RabbitCSRAtomPacked result;
        memcpy(&result, &raw, sizeof(uint64_t));
        return result;
    }
    
    RabbitCSRAtomPacked load_atom(std::memory_order order = std::memory_order_acquire) const {
        return unpack_atom(atom_raw.load(order));
    }
    
    void store_atom(const RabbitCSRAtomPacked& a, std::memory_order order = std::memory_order_release) {
        atom_raw.store(pack_atom(a), order);
    }
    
    bool cas_atom(RabbitCSRAtomPacked& expected, const RabbitCSRAtomPacked& desired) {
        uint64_t exp_raw = pack_atom(expected);
        uint64_t des_raw = pack_atom(desired);
        bool success = atom_raw.compare_exchange_weak(exp_raw, des_raw,
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (!success) {
            expected = unpack_atom(exp_raw);
        }
        return success;
    }
    
    float exchange_str(float new_str) {
        RabbitCSRAtomPacked old_atom, new_atom;
        do {
            old_atom = load_atom();
            new_atom = RabbitCSRAtomPacked(new_str, old_atom.child);
        } while (!cas_atom(old_atom, new_atom));
        return old_atom.str;
    }
    
    void init(float str) {
        store_atom(RabbitCSRAtomPacked(str, UINT32_MAX), std::memory_order_relaxed);
        sibling.store(UINT32_MAX, std::memory_order_relaxed);
        united_child = UINT32_MAX;
    }
};

/**
 * RabbitOrderCSR graph representation
 */
struct RabbitCSRGraph {
    uint32_t* dest;                   // Vertex -> community ID (plain, not atomic — matching GBrew_rabbit/Boost)
    RabbitCSRVertex* vs;              // Vertex attributes
    std::vector<std::vector<std::pair<uint32_t, float>>> es;  // Adjacency list (neighbor, weight)
    double tot_wgt;                   // Total edge weight
    double resolution;                 // Resolution parameter for modularity (default 1.0)
    std::vector<uint32_t> tops;       // Top-level vertices (roots)
    uint32_t num_vertices;
    
    // Performance counters
    std::atomic<size_t> n_reunite{0};
    std::atomic<size_t> n_fail_lock{0};
    std::atomic<size_t> n_fail_cas{0};
    std::atomic<size_t> tot_nbrs{0};
    
    RabbitCSRGraph() : dest(nullptr), vs(nullptr), tot_wgt(0.0), resolution(1.0), num_vertices(0) {}
    
    ~RabbitCSRGraph() {
        if (dest) delete[] dest;
        if (vs) delete[] vs;
    }
    
    void allocate(uint32_t n) {
        num_vertices = n;
        dest = new uint32_t[n];
        vs = new RabbitCSRVertex[n];
        es.resize(n);
        #pragma omp parallel for schedule(static)
        for (uint32_t i = 0; i < n; ++i) {
            dest[i] = i;
        }
    }
    
    uint32_t n() const { return num_vertices; }
};

/**
 * Trace community: Find the root community that vertex v belongs to
 * Uses path compression for efficiency
 */
inline uint32_t rabbitCSRTraceCom(uint32_t v, RabbitCSRGraph& g) {
    uint32_t d = g.dest[v];
    if (d == v) return v;  // Fast path: already at root
    
    // Path compression matching GBrew_rabbit's findDest()
    while (g.dest[d] != d) {
        uint32_t dd = g.dest[d];
        g.dest[v] = dd;  // Path compression
        d = dd;
    }
    return d;
}

/**
 * Simple compact for final aggregation
 */
inline void rabbitCSRCompactEdges(std::vector<std::pair<uint32_t, float>>& edges) {
    if (edges.empty()) return;
    
    std::sort(edges.begin(), edges.end(), 
        [](const auto& a, const auto& b) { return a.first < b.first; });
    
    size_t write_pos = 0;
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i].first == edges[write_pos].first) {
            edges[write_pos].second += edges[i].second;
        } else {
            ++write_pos;
            if (write_pos != i) {
                edges[write_pos] = edges[i];
            }
        }
    }
    edges.resize(write_pos + 1);
}

/**
 * Unite: Aggregate edges of vertex v and all vertices merged into v
 * Optimized with prefetching like the original boost implementation
 */
inline void rabbitCSRUnite(uint32_t v, std::vector<std::pair<uint32_t, float>>& nbrs, 
                    RabbitCSRGraph& g) {
    size_t icmb = 0;
    nbrs.clear();
    
    auto push_edges = [&](uint32_t u) {
        const auto& es = g.es[u];
        const size_t es_size = es.size();
        constexpr size_t npre = 8;
        
        for (size_t i = 0; i < es_size && i < npre; ++i) {
            __builtin_prefetch(&g.dest[es[i].first], 0, 3);
        }
        
        for (size_t i = 0; i < es_size; ++i) {
            if (i + npre < es_size) {
                __builtin_prefetch(&g.dest[es[i + npre].first], 0, 3);
            }
            
            uint32_t c = rabbitCSRTraceCom(es[i].first, g);
            if (c != v) {
                nbrs.push_back({c, es[i].second});
            }
        }
        
        if (nbrs.size() - icmb >= 2048) {
            std::sort(nbrs.begin() + icmb, nbrs.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            
            size_t write_pos = icmb;
            for (size_t i = icmb + 1; i < nbrs.size(); ++i) {
                if (nbrs[i].first == nbrs[write_pos].first) {
                    nbrs[write_pos].second += nbrs[i].second;
                } else {
                    ++write_pos;
                    if (write_pos != i) {
                        nbrs[write_pos] = nbrs[i];
                    }
                }
            }
            nbrs.resize(write_pos + 1);
            icmb = nbrs.size();
        }
    };
    
    push_edges(v);
    
    RabbitCSRAtomPacked v_atom = g.vs[v].load_atom();
    while (g.vs[v].united_child != v_atom.child) {
        uint32_t c = v_atom.child;
        for (uint32_t w = c; w != UINT32_MAX && w != g.vs[v].united_child; 
             w = g.vs[w].sibling.load(std::memory_order_relaxed)) {
            push_edges(w);
        }
        g.vs[v].united_child = c;
        v_atom = g.vs[v].load_atom();
    }
    
    g.tot_nbrs.fetch_add(nbrs.size(), std::memory_order_relaxed);
    
    g.es[v].clear();
    if (!nbrs.empty()) {
        rabbitCSRCompactEdges(nbrs);
        g.es[v] = std::move(nbrs);
    }
}

/**
 * Find best destination: Find neighbor v that maximizes ΔQ(u,v)
 */
inline uint32_t rabbitCSRFindBest(const RabbitCSRGraph& g, uint32_t u, float u_strength) {
    double dmax = 0.0;
    uint32_t best = u;
    
    for (const auto& e : g.es[u]) {
        RabbitCSRAtomPacked v_atom = g.vs[e.first].load_atom();
        double delta_q = static_cast<double>(e.second) - 
                         g.resolution * static_cast<double>(u_strength) * static_cast<double>(v_atom.str) / g.tot_wgt;
        
        if (delta_q > dmax) {
            dmax = delta_q;
            best = e.first;
        }
    }
    return best;
}

/**
 * Merge vertex v into its best neighbor
 * Returns: v if v becomes top-level, destination if merged, UINT32_MAX if failed
 */
inline uint32_t rabbitCSRMerge(uint32_t v, std::vector<std::pair<uint32_t, float>>& nbrs,
                        RabbitCSRGraph& g) {
    rabbitCSRUnite(v, nbrs, g);
    
    float vstr = g.vs[v].exchange_str(-1.0f);
    
    RabbitCSRAtomPacked v_atom = g.vs[v].load_atom();
    if (v_atom.child != g.vs[v].united_child) {
        rabbitCSRUnite(v, nbrs, g);
        g.n_reunite.fetch_add(1, std::memory_order_relaxed);
    }
    
    uint32_t u = rabbitCSRFindBest(g, v, vstr);
    
    if (u == v) {
        RabbitCSRAtomPacked new_v_atom = g.vs[v].load_atom();
        new_v_atom.str = vstr;
        g.vs[v].store_atom(new_v_atom);
        return v;
    }
    
    RabbitCSRAtomPacked u_atom = g.vs[u].load_atom();
    
    if (u_atom.str < 0.0f) {
        RabbitCSRAtomPacked new_v_atom = g.vs[v].load_atom();
        new_v_atom.str = vstr;
        g.vs[v].store_atom(new_v_atom);
        g.n_fail_lock.fetch_add(1, std::memory_order_relaxed);
        return UINT32_MAX;
    }
    
    g.vs[v].sibling.store(u_atom.child, std::memory_order_release);
    
    RabbitCSRAtomPacked new_u_atom(u_atom.str + vstr, v);
    if (!g.vs[u].cas_atom(u_atom, new_u_atom)) {
        g.vs[v].sibling.store(UINT32_MAX, std::memory_order_release);
        RabbitCSRAtomPacked new_v_atom = g.vs[v].load_atom();
        new_v_atom.str = vstr;
        g.vs[v].store_atom(new_v_atom);
        g.n_fail_cas.fetch_add(1, std::memory_order_relaxed);
        return UINT32_MAX;
    }
    
    g.dest[v] = u;
    return u;
}

/**
 * Parallel incremental aggregation (Algorithm 3 from paper)
 */
inline void rabbitCSRAggregate(RabbitCSRGraph& g) {
    const uint32_t n = g.n();
    const int np = omp_get_max_threads();
    
    // Sort all vertices by degree ascending (matching Boost/GBrew_rabbit)
    // No separate isolated vertex scan — they naturally become top-level during merge
    std::vector<std::pair<uint32_t, uint32_t>> ord(n);
    #pragma omp parallel for schedule(static)
    for (uint32_t v = 0; v < n; ++v) {
        ord[v] = {v, static_cast<uint32_t>(g.es[v].size())};
    }
    __gnu_parallel::sort(ord.begin(), ord.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    
    std::vector<std::deque<uint32_t>> topss(np);
    
    #pragma omp parallel
    {
        const int tid = omp_get_thread_num();
        std::deque<uint32_t> tops;
        std::deque<uint32_t> pends;
        std::vector<std::pair<uint32_t, float>> nbrs;
        // Boost's heuristic: N*2 total, but per-thread so divide by np.
        // This avoids reallocations during unite() while staying cache-friendly.
        nbrs.reserve(std::max<size_t>(n / np * 2, 4096));
        
        #pragma omp for schedule(static, 1)
        for (uint32_t i = 0; i < n; ++i) {
            // Retry pending vertices (Boost's inline retry pattern)
            pends.erase(
                std::remove_if(pends.begin(), pends.end(), [&](uint32_t w) {
                    uint32_t u = rabbitCSRMerge(w, nbrs, g);
                    if (u == w) tops.push_back(w);
                    return u != UINT32_MAX;  // remove if handled
                }),
                pends.end());
            
            uint32_t v = ord[i].first;
            uint32_t u = rabbitCSRMerge(v, nbrs, g);
            if (u == v) {
                tops.push_back(v);
            } else if (u == UINT32_MAX) {
                pends.push_back(v);
            }
        }
        
        #pragma omp barrier
        #pragma omp critical
        {
            for (uint32_t v : pends) {
                uint32_t u = rabbitCSRMerge(v, nbrs, g);
                if (u == v) {
                    tops.push_back(v);
                }
            }
            topss[tid] = std::move(tops);
        }
    }
    
    for (int t = 0; t < np; ++t) {
        for (uint32_t v : topss[t]) {
            g.tops.push_back(v);
        }
    }
}

/**
 * DFS traversal to collect descendants in dendrogram
 */
inline void rabbitCSRDescendants(const RabbitCSRGraph& g, uint32_t v,
                          std::vector<uint32_t>& result) {
    result.push_back(v);
    RabbitCSRAtomPacked atom = g.vs[v].load_atom();
    uint32_t child = atom.child;
    while (child != UINT32_MAX) {
        result.push_back(child);
        atom = g.vs[child].load_atom();
        child = atom.child;
    }
}

/**
 * Compute permutation from dendrogram via DFS
 */
template <typename NodeID_>
void rabbitCSRComputePerm(const RabbitCSRGraph& g, pvector<NodeID_>& perm) {
    const uint32_t n = g.n();
    const uint32_t ncom = static_cast<uint32_t>(g.tops.size());
    
    // Match Boost: treat all tops uniformly (no isolated vertex segregation)
    auto coms = std::make_unique<uint32_t[]>(n);
    std::vector<uint32_t> offsets(ncom + 1, 0);
    
    const int np = omp_get_max_threads();
    const uint32_t ntask = std::min<uint32_t>(ncom, 128 * np);
    
    #pragma omp parallel
    {
        std::vector<uint32_t> stack;
        
        #pragma omp for schedule(dynamic, 1)
        for (uint32_t i = 0; i < ntask; ++i) {
            for (uint32_t comid = i; comid < ncom; comid += ntask) {
                uint32_t newid = 0;
                stack.clear();
                
                rabbitCSRDescendants(g, g.tops[comid], stack);
                
                while (!stack.empty()) {
                    uint32_t v = stack.back();
                    stack.pop_back();
                    
                    coms[v] = comid;
                    perm[v] = newid++;
                    
                    uint32_t sib = g.vs[v].sibling.load(std::memory_order_acquire);
                    if (sib != UINT32_MAX) {
                        rabbitCSRDescendants(g, sib, stack);
                    }
                }
                
                offsets[comid + 1] = newid;
            }
        }
    }
    
    // Prefix sum to compute global offsets
    for (uint32_t i = 1; i <= ncom; ++i) {
        offsets[i] += offsets[i - 1];
    }
    
    // Add community offset to get final permutation
    #pragma omp parallel for schedule(static)
    for (uint32_t v = 0; v < n; ++v) {
        perm[v] += offsets[coms[v]];
    }
}

/**
 * Compute modularity of the community structure using original CSR graph
 */
template<typename NodeID_, typename DestID_, typename WeightT_, bool invert>
double rabbitCSRComputeModularityCSR(const CSRGraph<NodeID_, DestID_, invert>& csr_g, 
                                      RabbitCSRGraph& rg) {
    const uint32_t n = rg.n();
    double m2 = 0.0;
    
    std::vector<uint32_t> community(n);
    #pragma omp parallel for
    for (uint32_t v = 0; v < n; ++v) {
        community[v] = rabbitCSRTraceCom(v, rg);
    }
    
    uint32_t max_com = 0;
    for (uint32_t v = 0; v < n; ++v) {
        if (community[v] > max_com) max_com = community[v];
    }
    const size_t com_array_size = static_cast<size_t>(max_com) + 1;
    
    std::vector<double> degs_all(com_array_size, 0.0);
    std::vector<double> degs_loop(com_array_size, 0.0);
    
    for (int64_t v = 0; v < static_cast<int64_t>(n); ++v) {
        uint32_t c = community[v];
        for (auto neighbor : csr_g.out_neigh(v)) {
            NodeID_ dest;
            float weight = 1.0f;
            if (csr_g.is_weighted()) {
                dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                weight = static_cast<float>(
                    static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).w);
            } else {
                dest = static_cast<NodeID_>(neighbor);
            }
            
            m2 += weight;
            degs_all[c] += weight;
            if (community[static_cast<uint32_t>(dest)] == c) {
                degs_loop[c] += weight;
            }
        }
    }
    
    double q = 0.0;
    for (size_t c = 0; c < com_array_size; ++c) {
        double all = degs_all[c];
        double loop = degs_loop[c];
        if (all > 0.0) {
            q += loop / m2 - (all / m2) * (all / m2);
        }
    }
    
    return q;
}

// ============================================================================
// RABBIT ORDER CSR MAPPING (Native CSR Implementation)
// ============================================================================

/**
 * @brief Generate RabbitOrder mapping using native CSR (no Boost)
 * 
 * This is a native CSR implementation of the RabbitOrder algorithm that:
 * 1. Builds a RabbitCSRGraph from the input CSR graph
 * 2. Runs parallel incremental aggregation (community detection)
 * 3. Generates ordering via DFS on the dendrogram
 * 
 * Unlike the Boost-based implementation, this version works directly
 * with CSR graphs and doesn't require converting to Boost graph types.
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph in CSR format
 * @param new_ids Output permutation vector
 */
template<typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateRabbitOrderCSRMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                                   pvector<NodeID_>& new_ids) {
    Timer tm;
    tm.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges_directed();
    
    std::cout << "=== RabbitOrderCSR (Native CSR Implementation) ===\n";
    std::cout << "Nodes: " << static_cast<long long>(num_nodes) 
              << ", Edges: " << static_cast<long long>(num_edges) << "\n";
    
    // Build RabbitCSRGraph from CSRGraph
    RabbitCSRGraph rg;
    rg.allocate(static_cast<uint32_t>(num_nodes));
    rg.tot_wgt = 0.0;
    
    // Resolution parameter: γ in ΔQ = w_uv - γ * str(u)*str(v)/tot_wgt
    // γ = 1.0 matches the Boost RabbitOrder implementation exactly.
    // Prior auto-adaptive γ < 1 caused over-merging (fewer, larger communities)
    // which degraded traversal locality for BFS/CC/SSSP by ~80% on medium graphs.
    {
        rg.resolution = 1.0;  // Match Boost default
        const char* res_env = std::getenv("RABBIT_RESOLUTION");
        if (res_env) {
            rg.resolution = std::atof(res_env);
        }
        double avg_deg = (num_nodes > 0) ? static_cast<double>(num_edges) / num_nodes : 1.0;
        std::cout << "Resolution: " << rg.resolution 
                  << " (avg_deg=" << std::fixed << std::setprecision(1) << avg_deg << ")\n";
    }
    
    // Initialize vertices and edges — direct copy matching Boost/GBrew_rabbit.
    // Sort+aggregate is deferred to unite() where it happens incrementally on demand.
    // This eliminates the O(E·log(deg)) upfront sort that was 38-71% of core time.
    #pragma omp parallel
    {
        double local_wgt = 0.0;
        
        #pragma omp for schedule(static)
        for (int64_t v = 0; v < num_nodes; ++v) {
            float vertex_strength = 0.0f;
            rg.es[v].reserve(g.out_degree(v));
            
            for (DestID_ neighbor : g.out_neigh(v)) {
                NodeID_ d;
                float weight = 1.0f;
                
                if (g.is_weighted()) {
                    d = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                    weight = static_cast<float>(
                        static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).w);
                } else {
                    d = static_cast<NodeID_>(neighbor);
                }
                
                rg.es[v].emplace_back(static_cast<uint32_t>(d), weight);
                vertex_strength += weight;
            }
            
            rg.vs[v].init(vertex_strength);
            local_wgt += static_cast<double>(vertex_strength);
        }
        
        #pragma omp atomic
        rg.tot_wgt += local_wgt;
    }
    
    tm.Stop();
    double build_time = tm.Seconds();
    tm.Start();
    
    // Run parallel incremental aggregation (community detection)
    rabbitCSRAggregate(rg);
    
    tm.Stop();
    double agg_time = tm.Seconds();
    tm.Start();
    
    // Generate ordering via DFS on dendrogram
    rabbitCSRComputePerm(rg, new_ids);
    
    tm.Stop();
    double perm_time = tm.Seconds();
    
    // Compute modularity using original CSR graph
    double modularity = rabbitCSRComputeModularityCSR<NodeID_, DestID_, WeightT_, invert>(g, rg);
    
    // Report statistics — emit "RabbitOrder Map Time:" for consistent parsing
    // with Boost variant (which emits the same label for aggregate+compute_perm).
    // Exclude Build time: it's graph format conversion (CSR → RabbitCSRGraph),
    // analogous to Boost's readRabbitOrderGraphCSR() which is also excluded from
    // its Map Time. This gives an apples-to-apples core algorithm comparison.
    double total_map_time = agg_time + perm_time;
    PrintTime("RabbitOrder Communities", static_cast<double>(rg.tops.size()));
    PrintTime("RabbitOrder Modularity", modularity);
    PrintTime("RabbitOrder Map Time", total_map_time);
    std::cout << "RabbitOrderCSR Statistics:\n";
    PrintTime("Build Time", build_time);
    PrintTime("Aggregation Time", agg_time);
    PrintTime("Permutation Time", perm_time);
    PrintTime("Reunite calls", static_cast<double>(rg.n_reunite.load()));
    PrintTime("Lock failures", static_cast<double>(rg.n_fail_lock.load()));
    PrintTime("CAS failures", static_cast<double>(rg.n_fail_cas.load()));
}

#endif  // REORDER_RABBIT_H_
