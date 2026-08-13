// ============================================================================
// TRUST: Graph Partitioning for Triangle Counting
// ============================================================================
// This header implements the TRUST graph partitioning algorithm:
//
// TRUST partitioning is designed for triangle counting workloads, organizing
// graphs into partitions that maximize data locality for triangle enumeration.
//
// Key Functions:
//   - TrustPartitioner: Main class for TRUST partitioning
//   - MakeTrustPartitionedGraph: Create partitioned graph using TRUST
//   - MakeTrustPartitionedEL: Create partitioned edge lists
//   - trust_orientation: Orient edges for triangle counting
//   - trust_reassignID: Reassign vertex IDs based on degree
//
// These functions are used for graph partitioning experiments,
// not for the core GraphBrew reordering algorithms.
//
// Author: GraphBrew Team (extracted from builder.h)
// License: See LICENSE.txt
// ============================================================================

#ifndef TRUST_PARTITION_H_
#define TRUST_PARTITION_H_

#include <algorithm>
#include <iostream>
#include <limits>
#include <numeric>
#include <parallel/algorithm>
#include <parallel/numeric>
#include <vector>

#include <graph.h>
#include <pvector.h>
#include <timer.h>

// ============================================================================
// TRUST EDGE LIST STRUCTURE
// ============================================================================

/**
 * @brief Edge list structure for TRUST partitioning
 * 
 * Stores vertex adjacency information during the TRUST preprocessing phase.
 * Each entry contains the original vertex ID, its adjacency list, and a
 * reassigned ID after the orientation and sorting steps.
 * 
 * @tparam NodeID_ Node ID type
 */
template <typename NodeID_>
struct TrustEdgeList
{
    NodeID_ vertexID;              ///< Original vertex identifier
    std::vector<NodeID_> edge;     ///< Adjacency list (neighbors)
    NodeID_ newid;                 ///< Reassigned ID after processing
};

// ============================================================================
// TRUST PARTITIONER CLASS
// ============================================================================

/**
 * @brief TRUST graph partitioning implementation
 * 
 * Implements the TRUST partitioning algorithm for efficient triangle counting.
 * The algorithm:
 * 1. Sorts vertices by degree
 * 2. Orients edges from lower to higher degree
 * 3. Reassigns IDs to improve locality
 * 4. Partitions the graph into p_n x p_m partitions
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam WeightT_ Edge weight type
 * @tparam invert Whether to store inverse edges
 */
template <typename NodeID_, typename DestID_, typename WeightT_, bool invert = true>
class TrustPartitioner
{
public:
    using EdgeList = pvector<EdgePair<NodeID_, DestID_>>;
    using Edge = EdgePair<NodeID_, DestID_>;
    using Graph = CSRGraph<NodeID_, DestID_, invert>;
    using TrustVertex = TrustEdgeList<NodeID_>;

private:
    // TRUST state variables
    int64_t trust_vertex_count_;
    int64_t trust_edge_count_;
    NodeID_ trust_endofprocess_;
    std::vector<TrustVertex> trust_vertex_;
    std::vector<TrustVertex> trust_vertexb_;
    bool logging_enabled_;

public:
    /**
     * @brief Construct a TRUST partitioner
     * @param logging Enable verbose logging
     */
    explicit TrustPartitioner(bool logging = false)
        : trust_vertex_count_(0)
        , trust_edge_count_(0)
        , trust_endofprocess_(-1)
        , logging_enabled_(logging)
    {}

    // ========================================================================
    // MAIN PUBLIC INTERFACE
    // ========================================================================

    /**
     * @brief Partition a graph using TRUST algorithm
     * 
     * @param g Input graph
     * @param p_n Number of row partitions
     * @param p_m Number of column partitions
     * @return Vector of partitioned graphs (p_n x p_m partitions)
     */
    std::vector<Graph> MakeTrustPartitionedGraph(const Graph &g, 
                                                   int p_n = 1, int p_m = 1)
    {
        if ((p_n * p_m) <= 0)
        {
            throw std::invalid_argument(
                "Number of partitions must be greater than 0");
        }

        std::vector<Graph> partitions_g(p_n * p_m);
        std::vector<EdgeList> partitions_el;

        Graph uni_g;
        Graph dir_g;
        Graph prep_g;
        EdgeList dir_el;
        EdgeList uni_el;

        // Step 1: Convert directed to undirected
        dir_el = MakeTrustDirectELFromGraph(g);
        dir_g = MakeLocalGraphFromEL(dir_el, g.num_nodes());

        if (logging_enabled_)
        {
            dir_g.PrintTopology();
            std::cout << std::endl;
        }

        NodeID_ min_seen = FindMinNodeID(dir_el);

        uni_el = MakeUniDirectELFromGraph(dir_g, min_seen);
        uni_g = MakeLocalGraphFromEL(uni_el, g.num_nodes());

        if (logging_enabled_)
        {
            uni_g.PrintTopology();
            std::cout << std::endl;
        }

        // Step 2: TRUST preprocessing
        prep_g = MakeTrustOriginalPreprocessStep(uni_g);

        if (logging_enabled_)
        {
            prep_g.PrintTopology();
            std::cout << std::endl;
        }

        // Step 3: Create partitioned edge lists
        partitions_el = MakeTrustPartitionedEL(prep_g, p_n, p_m);

        // Step 4: Create graphs from partitions
        for (int row = 0; row < p_n; ++row)
        {
            for (int col = 0; col < p_m; ++col)
            {
                int idx = row * p_m + col;
                Graph partition_g = MakeLocalGraphFromEL(partitions_el[idx], 0);
                partitions_g[idx] = std::move(partition_g);
            }
        }

        return partitions_g;
    }

    /**
     * @brief Create edge lists for each partition
     * 
     * @param g Preprocessed graph
     * @param p_n Number of row partitions
     * @param p_m Number of column partitions
     * @return Vector of edge lists, one per partition
     */
    std::vector<EdgeList> MakeTrustPartitionedEL(const Graph &g,
                                                   int p_n = 1, int p_m = 1)
    {
        int num_partitions = p_n * p_m;
        int num_threads = omp_get_max_threads();
        std::vector<EdgeList> partitions_el(num_partitions);

        // Local edge lists for each thread
        std::vector<std::vector<EdgeList>> local_partitions_el(num_threads);

        for (int thread_id = 0; thread_id < num_threads; ++thread_id)
        {
            local_partitions_el[thread_id].resize(num_partitions);
        }

        trust_endofprocess_ = std::numeric_limits<NodeID_>::max();

        // Find first vertex with degree < 2
        #pragma omp parallel
        {
            #pragma omp for schedule(static)
            for (NodeID_ i = 0; i < g.num_nodes(); ++i)
            {
                if (g.out_degree(i) < 2)
                {
                    int current_value = trust_endofprocess_;
                    while (i < current_value && 
                           !__sync_bool_compare_and_swap(&trust_endofprocess_, 
                                                          current_value, i))
                    {
                        current_value = trust_endofprocess_;
                    }
                }
            }
        }

        // Build local edge lists per thread
        #pragma omp parallel
        {
            int thread_id = omp_get_thread_num();

            #pragma omp for schedule(static)
            for (NodeID_ i = 0; i < g.num_nodes(); ++i)
            {
                NodeID_ src = i;
                for (DestID_ j : g.out_neigh(i))
                {
                    if (g.is_weighted())
                    {
                        NodeID_ dest = static_cast<NodeWeight<NodeID_, WeightT_>>(j).v;
                        WeightT_ weight = static_cast<NodeWeight<NodeID_, WeightT_>>(j).w;
                        int partition_idx = (src % p_n) * p_m + (dest % p_m);
                        Edge e = Edge(src / p_n, 
                                      NodeWeight<NodeID_, WeightT_>(dest / p_m, weight));
                        local_partitions_el[thread_id][partition_idx].push_back(e);
                    }
                    else
                    {
                        NodeID_ dest = j;
                        int partition_idx = (src % p_n) * p_m + (dest % p_m);
                        Edge e = Edge(src / p_n, dest / p_m);
                        local_partitions_el[thread_id][partition_idx].push_back(e);
                    }
                }
            }
        }

        // Merge local partitions into global
        #pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < num_partitions; ++i)
        {
            for (int t = 0; t < num_threads; ++t)
            {
                partitions_el[i].reserve(partitions_el[i].size() +
                                         local_partitions_el[t][i].size());
            }
            for (int t = 0; t < num_threads; ++t)
            {
                partitions_el[i].insert(partitions_el[i].end(),
                                        local_partitions_el[t][i].begin(),
                                        local_partitions_el[t][i].end());
            }
        }

        return partitions_el;
    }

    // ========================================================================
    // INTERNAL HELPER FUNCTIONS
    // ========================================================================

private:
    /**
     * @brief Convert graph to directed edge list (lower ID -> higher ID)
     */
    EdgeList MakeTrustDirectELFromGraph(const Graph &g)
    {
        int64_t num_edges = g.num_edges_directed();
        int64_t num_nodes = g.num_nodes();
        EdgeList el(num_edges);

        std::vector<int> a(num_nodes, 0);

        // Mark vertices that appear in edges
        #pragma omp parallel for
        for (NodeID_ i = 0; i < num_nodes; ++i)
        {
            for (DestID_ neighbor : g.out_neigh(i))
            {
                #pragma omp atomic write
                a[i] = 1;

                if (g.is_weighted())
                {
                    NodeID_ dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                    #pragma omp atomic write
                    a[dest] = 1;
                }
                else
                {
                    #pragma omp atomic write
                    a[neighbor] = 1;
                }
            }
        }

        __gnu_parallel::partial_sum(a.begin(), a.end(), a.begin());

        // Build oriented edge list
        #pragma omp parallel for
        for (NodeID_ i = 0; i < num_nodes; ++i)
        {
            NodeID_ out_start = g.out_offset(i);
            NodeID_ j = 0;
            for (DestID_ neighbor : g.out_neigh(i))
            {
                NodeID_ new_u = a[i];

                if (g.is_weighted())
                {
                    NodeID_ dest = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).v;
                    WeightT_ weight = static_cast<NodeWeight<NodeID_, WeightT_>>(neighbor).w;
                    NodeID_ new_v = a[dest];

                    if (new_u > new_v)
                        el[out_start + j] = Edge(new_v, 
                                                  NodeWeight<NodeID_, WeightT_>(new_u, weight));
                    else
                        el[out_start + j] = Edge(new_u, 
                                                  NodeWeight<NodeID_, WeightT_>(new_v, weight));
                }
                else
                {
                    NodeID_ new_v = a[neighbor];
                    if (new_u > new_v)
                        el[out_start + j] = Edge(new_v, new_u);
                    else
                        el[out_start + j] = Edge(new_u, new_v);
                }

                ++j;
            }
        }

        return el;
    }

    /**
     * @brief Find minimum node ID in edge list
     */
    NodeID_ FindMinNodeID(const EdgeList &el)
    {
        NodeID_ min_seen = std::numeric_limits<NodeID_>::max();
        
        #pragma omp parallel for reduction(min:min_seen)
        for (size_t i = 0; i < el.size(); ++i)
        {
            min_seen = std::min(min_seen, el[i].u);
        }
        
        return min_seen;
    }

    /**
     * @brief Convert directed graph to undirected edge list
     */
    EdgeList MakeUniDirectELFromGraph(const Graph &g, NodeID_ min_seen)
    {
        EdgeList el;
        
        for (NodeID_ u = 0; u < g.num_nodes(); ++u)
        {
            for (DestID_ v : g.out_neigh(u))
            {
                if (g.is_weighted())
                {
                    NodeID_ dest = static_cast<NodeWeight<NodeID_, WeightT_>>(v).v;
                    WeightT_ weight = static_cast<NodeWeight<NodeID_, WeightT_>>(v).w;
                    el.push_back(Edge(u - min_seen, 
                                      NodeWeight<NodeID_, WeightT_>(dest - min_seen, weight)));
                    el.push_back(Edge(dest - min_seen, 
                                      NodeWeight<NodeID_, WeightT_>(u - min_seen, weight)));
                }
                else
                {
                    el.push_back(Edge(u - min_seen, v - min_seen));
                    el.push_back(Edge(v - min_seen, u - min_seen));
                }
            }
        }
        
        return el;
    }

    /**
     * @brief Build local graph from edge list
     */
    Graph MakeLocalGraphFromEL(EdgeList &el, NodeID_ num_nodes_hint = 0)
    {
        // Determine number of nodes
        NodeID_ max_seen = 0;
        for (const auto &e : el)
        {
            max_seen = std::max(max_seen, e.u);
            if (std::is_same<DestID_, NodeWeight<NodeID_, WeightT_>>::value)
            {
                max_seen = std::max(max_seen, 
                    static_cast<NodeWeight<NodeID_, WeightT_>>(e.v).v);
            }
            else
            {
                max_seen = std::max(max_seen, static_cast<NodeID_>(e.v));
            }
        }
        
        NodeID_ num_nodes = max_seen + 1;
        if (num_nodes_hint > 0) num_nodes = std::max(num_nodes, num_nodes_hint);

        // Count degrees
        pvector<NodeID_> degrees(num_nodes, 0);
        for (const auto &e : el)
        {
            degrees[e.u]++;
        }

        // Build CSR
        pvector<SGOffset> offsets(num_nodes + 1);
        offsets[0] = 0;
        for (NodeID_ n = 0; n < num_nodes; ++n)
        {
            offsets[n + 1] = offsets[n] + degrees[n];
        }

        DestID_ *neighs = new DestID_[el.size()];
        DestID_ **index = CSRGraph<NodeID_, DestID_>::GenIndex(offsets, neighs);

        pvector<NodeID_> pos(num_nodes, 0);
        for (const auto &e : el)
        {
            NodeID_ offset = offsets[e.u] + pos[e.u]++;
            neighs[offset] = e.v;
        }

        return Graph(num_nodes, index, neighs);
    }

    /**
     * @brief TRUST preprocessing step
     * 
     * Sorts vertices by degree, orients edges, and reassigns IDs.
     */
    Graph MakeTrustOriginalPreprocessStep(const Graph &g)
    {
        Graph sort_g;
        trust_vertex_count_ = g.num_nodes();
        trust_vertex_.resize(trust_vertex_count_);
        trust_edge_count_ = g.num_edges_directed();

        // Initialize trust vertices
        #pragma omp parallel for
        for (NodeID_ i = 0; i < trust_vertex_count_; ++i)
        {
            trust_vertex_[i].vertexID = i;
            trust_vertex_[i].edge.insert(trust_vertex_[i].edge.end(), 
                                          g.out_neigh(i).begin(), 
                                          g.out_neigh(i).end());
            trust_vertex_[i].newid = -1;
        }

        // Sort by degree (ascending)
        __gnu_parallel::stable_sort(trust_vertex_.begin(), trust_vertex_.end(), 
                                    trust_cmp_asc);

        if (logging_enabled_)
        {
            std::cout << "Before orientation:" << std::endl;
            printTrustVertexStructure();
        }

        trust_orientation();

        if (logging_enabled_)
        {
            std::cout << "After orientation:" << std::endl;
            printTrustVertexStructure();
        }

        // Sort by degree (descending)
        __gnu_parallel::stable_sort(trust_vertex_.begin(), trust_vertex_.end(), 
                                    trust_cmp_desc);

        if (logging_enabled_)
        {
            std::cout << "After sort:" << std::endl;
            printTrustVertexStructure();
        }

        trust_computeCSR();

        if (logging_enabled_)
        {
            std::cout << "After computeCSR:" << std::endl;
            printTrustVertexStructure();
        }

        // Count edges
        trust_edge_count_ = 0;
        #pragma omp parallel for reduction(+: trust_edge_count_)
        for (NodeID_ i = 0; i < trust_vertex_count_; ++i)
        {
            trust_edge_count_ += trust_vertex_[i].edge.size();
        }

        if (logging_enabled_)
        {
            std::cout << "trust_edge_count: " << trust_edge_count_ << std::endl;
        }

        // Build edge list from trust structure
        std::vector<NodeID_> trust_vertex_edge_sizes(trust_vertex_count_);
        #pragma omp parallel for
        for (NodeID_ i = 0; i < trust_vertex_count_; ++i)
        {
            trust_vertex_edge_sizes[i] = trust_vertex_[i].edge.size();
        }

        std::vector<size_t> trust_vertex_offset(trust_vertex_count_ + 1, 0);
        __gnu_parallel::partial_sum(trust_vertex_edge_sizes.begin(), 
                                    trust_vertex_edge_sizes.end(), 
                                    trust_vertex_offset.begin() + 1);

        EdgeList trust_el(trust_edge_count_);

        #pragma omp parallel for
        for (NodeID_ i = 0; i < trust_vertex_count_; ++i)
        {
            NodeID_ out_start = trust_vertex_offset[i];
            NodeID_ j = 0;
            for (NodeID_ neighbor : trust_vertex_[i].edge)
            {
                if (g.is_weighted())
                {
                    WeightT_ weight = 1;
                    trust_el[out_start + j] = Edge(i, 
                        NodeWeight<NodeID_, WeightT_>(neighbor, weight));
                }
                else
                {
                    trust_el[out_start + j] = Edge(i, neighbor);
                }
                ++j;
            }
        }

        sort_g = MakeLocalGraphFromEL(trust_el, 0);

        return sort_g;
    }

    // ========================================================================
    // TRUST CORE ALGORITHMS
    // ========================================================================

    /**
     * @brief Orient edges from lower to higher index
     * 
     * For each vertex, keeps only edges pointing to vertices with higher
     * sorted index. This creates a DAG structure useful for triangle counting.
     */
    void trust_orientation()
    {
        NodeID_ *a = new NodeID_[trust_vertex_count_];
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            a[trust_vertex_[i].vertexID] = i;
        }

        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            std::vector<NodeID_> x(trust_vertex_[i].edge);
            trust_vertex_[i].edge.clear();
            while (!x.empty())
            {
                NodeID_ v = x.back();
                x.pop_back();
                if (a[v] > i) trust_vertex_[i].edge.push_back(v);
            }
        }

        delete[] a;
    }

    /**
     * @brief Reassign vertex IDs based on degree classes
     * 
     * Vertices are grouped into three classes:
     * - High degree (> 100)
     * - Medium degree (2-100)
     * - Low degree (< 2)
     * 
     * IDs are reassigned to group similar vertices together.
     */
    void trust_reassignID()
    {
        NodeID_ k1 = 0, k2 = -1, k3 = -1;
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            trust_vertex_[i].newid = -1;
            if (k2 == -1 && trust_vertex_[i].edge.size() <= 100)
                k2 = i;
            if (k3 == -1 && trust_vertex_[i].edge.size() < 2)
                k3 = i;
        }
        
        std::cout << k2 << ' ' << k3 << std::endl;
        NodeID_ s2 = k2, s3 = k3;
        
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            if (trust_vertex_[i].edge.size() <= 2) break;
            for (size_t j = 0; j < trust_vertex_[i].edge.size(); j++)
            {
                NodeID_ v = trust_vertex_[i].edge[j];
                if (trust_vertex_[v].newid == -1)
                {
                    if (v >= s3)
                    {
                        trust_vertex_[v].newid = k3;
                        k3++;
                    }
                    else if (v >= s2)
                    {
                        trust_vertex_[v].newid = k2;
                        k2++;
                    }
                    else
                    {
                        trust_vertex_[v].newid = k1;
                        k1++;
                    }
                }
            }
        }
        
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            int u = trust_vertex_[i].newid;
            if (u == -1)
            {
                if (i >= s3)
                {
                    trust_vertex_[i].newid = k3;
                    k3++;
                }
                else if (i >= s2)
                {
                    trust_vertex_[i].newid = k2;
                    k2++;
                }
                else
                {
                    trust_vertex_[i].newid = k1;
                    k1++;
                }
            }
        }
        
        trust_vertexb_.swap(trust_vertex_);
        trust_vertex_.resize(trust_vertex_count_);

        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            NodeID_ u = trust_vertexb_[i].newid;
            for (size_t j = 0; j < trust_vertexb_[i].edge.size(); j++)
            {
                NodeID_ v = trust_vertexb_[i].edge[j];
                v = trust_vertexb_[v].newid;
                trust_vertex_[u].edge.push_back(v);
            }
        }
    }

    /**
     * @brief Compute CSR representation from trust vertex structure
     */
    void trust_computeCSR()
    {
        NodeID_ *a = new NodeID_[trust_vertex_count_];
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            a[trust_vertex_[i].vertexID] = i;
        }
        for (NodeID_ i = 0; i < trust_vertex_count_; i++)
        {
            for (size_t j = 0; j < trust_vertex_[i].edge.size(); j++)
            {
                trust_vertex_[i].edge[j] = a[trust_vertex_[i].edge[j]];
            }
            trust_vertex_[i].vertexID = i;
        }

        trust_reassignID();

        delete[] a;
    }

    // ========================================================================
    // COMPARISON FUNCTIONS
    // ========================================================================

    static bool trust_cmp_asc(const TrustVertex &a, const TrustVertex &b)
    {
        return a.edge.size() < b.edge.size();
    }

    static bool trust_cmp_desc(const TrustVertex &a, const TrustVertex &b)
    {
        return a.edge.size() > b.edge.size();
    }

    // ========================================================================
    // DEBUG UTILITIES
    // ========================================================================

    void printTrustVertexStructure()
    {
        for (const auto &v : trust_vertex_)
        {
            std::cout << "Trust Vertex " << v.vertexID << ": ";
            for (const auto &e : v.edge)
            {
                std::cout << e << " ";
            }
            std::cout << std::endl;
        }
    }
};

#endif  // TRUST_PARTITION_H_
