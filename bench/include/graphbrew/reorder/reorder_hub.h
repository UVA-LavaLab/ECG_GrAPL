// ============================================================================
// GraphBrew - Hub-Based Reordering Algorithms
// ============================================================================
// This header implements hub-based reordering algorithms:
//   - HUBSORT (3):      Sort vertices by degree, placing hubs first
//   - HUBCLUSTER (4):   Cluster hubs with their neighbors
//   - DBG (5):          Degree-Based Grouping into logarithmic buckets
//   - HUBSORTDBG (6):   HubSort within DBG buckets
//   - HUBCLUSTERDBG (7): HubCluster within DBG buckets
//
// Key Concepts:
//   - Hub: A vertex with degree > average degree
//   - DBG: Groups vertices into buckets based on degree ranges
//   - These algorithms optimize cache locality for power-law graphs
//
// Author: GraphBrew Team
// License: See LICENSE.txt
// ============================================================================

#ifndef REORDER_HUB_H_
#define REORDER_HUB_H_

#include "reorder_types.h"
#include <sliding_queue.h>

// Forward declaration for UINT_E_MAX used as sentinel value
#ifndef UINT_E_MAX
#define UINT_E_MAX std::numeric_limits<uint32_t>::max()
#endif

// Type alias for edge counts
using uintE = uint32_t;

// ============================================================================
// HUBSORT (Algorithm 3)
// ============================================================================

/**
 * @brief Sort vertices by degree, placing hubs (high-degree) first
 * 
 * Algorithm:
 *   1. Identify hubs (vertices with degree > average)
 *   2. Sort hubs by degree (descending)
 *   3. Place hubs at the beginning of the ordering
 *   4. Non-hubs fill remaining positions
 * 
 * This improves cache locality because:
 *   - Hubs are accessed frequently (many neighbors point to them)
 *   - Placing hubs together maximizes cache line reuse
 * 
 * Complexity: O(n log n) for sorting
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param useOutdeg If true, use out-degree; else use in-degree
 * 
 * @example
 *   // Original: vertices 0,1,2,3 with degrees 2,100,5,50
 *   // After:    vertex 1 (deg=100) gets ID 0
 *   //           vertex 3 (deg=50) gets ID 1  
 *   //           non-hubs fill remaining positions
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateHubSortMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                            pvector<NodeID_>& new_ids, 
                            bool useOutdeg) {
    
    using DegreeNodePair = std::pair<int64_t, NodeID_>;
    
    Timer t;
    t.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("HubSort Map Time", t.Seconds());
        return;
    }
    
    const int64_t avgDegree = num_edges / num_nodes;
    
    pvector<DegreeNodePair> degree_id_pairs(num_nodes);
    size_t hubCount = 0;
    
    // Step 1: Collect degrees and count hubs
    #pragma omp parallel for reduction(+:hubCount)
    for (int64_t v = 0; v < num_nodes; ++v) {
        int64_t degree = useOutdeg ? g.out_degree(v) : g.in_degree(v);
        degree_id_pairs[v] = std::make_pair(degree, v);
        if (degree > avgDegree) {
            ++hubCount;
        }
    }
    
    // Step 2: Sort all vertices by degree (descending)
    __gnu_parallel::stable_sort(degree_id_pairs.begin(), 
                                 degree_id_pairs.end(),
                                 std::greater<DegreeNodePair>());
    
    // Step 3: Assign sequential IDs to hubs (positions 0 to hubCount-1)
    #pragma omp parallel for
    for (size_t n = 0; n < hubCount; ++n) {
        new_ids[degree_id_pairs[n].second] = n;
    }
    
    // Free memory from degree pairs
    pvector<DegreeNodePair>().swap(degree_id_pairs);
    
    // Step 4: Assign non-hub vertices to remaining positions (hubCount..num_nodes-1)
    // Uses per-thread partitions with pre-computed offsets (race-free, parallel).
    {
        const int num_threads = omp_get_max_threads();
        const int64_t slice = num_nodes / num_threads;
        int64_t start[num_threads], end_pos[num_threads];
        int64_t non_hub_count[num_threads];
        int64_t new_index[num_threads];

        for (int th = 0; th < num_threads; ++th) {
            start[th] = th * slice;
            end_pos[th] = (th == num_threads - 1) ? num_nodes : (th + 1) * slice;
        }

        // Count non-hubs per thread partition
        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int th = 0; th < num_threads; ++th) {
            int64_t cnt = 0;
            for (int64_t v = start[th]; v < end_pos[th]; ++v) {
                if (new_ids[v] == static_cast<NodeID_>(UINT_E_MAX)) ++cnt;
            }
            non_hub_count[th] = cnt;
        }

        // Prefix-sum to get each thread's starting position
        new_index[0] = static_cast<int64_t>(hubCount);
        for (int th = 1; th < num_threads; ++th) {
            new_index[th] = new_index[th - 1] + non_hub_count[th - 1];
        }

        // Each thread assigns its non-hubs independently (no cross-thread deps)
        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int th = 0; th < num_threads; ++th) {
            for (int64_t v = start[th]; v < end_pos[th]; ++v) {
                if (new_ids[v] == static_cast<NodeID_>(UINT_E_MAX)) {
                    new_ids[v] = static_cast<NodeID_>(new_index[th]++);
                }
            }
        }
    }
    
    t.Stop();
    PrintTime("HubSort Map Time", t.Seconds());
}

// ============================================================================
// HUBCLUSTER (Algorithm 4)
// ============================================================================

/**
 * @brief Cluster hub vertices by preserving partition locality
 * 
 * Similar to HubSort but assigns IDs based on partitioned processing,
 * maintaining better locality for parallel graph algorithms.
 * 
 * Algorithm:
 *   1. Partition vertices across threads
 *   2. Each thread identifies hubs in its partition
 *   3. Hubs get sequential IDs preserving partition order
 *   4. Non-hubs are assigned to remaining positions
 * 
 * Complexity: O(n) - linear scan with parallel partitioning
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param useOutdeg If true, use out-degree; else use in-degree
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateHubClusterMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                               pvector<NodeID_>& new_ids, 
                               bool useOutdeg) {
    
    Timer t;
    t.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("HubCluster Map Time", t.Seconds());
        return;
    }
    
    const int64_t avgDegree = num_edges / num_nodes;
    
    // Padding to avoid false sharing
    const int PADDING = 64 / sizeof(uintE);
    const int num_threads = omp_get_max_threads();
    const int64_t partitionSz = num_nodes / num_threads;
    
    // Per-thread hub counts with padding
    int64_t* localOffsets = new int64_t[num_threads * PADDING]();
    
    // Step 1: Count hubs per partition and mark them
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int64_t startID = partitionSz * tid;
        int64_t stopID = (tid == num_threads - 1) ? num_nodes : partitionSz * (tid + 1);
        
        for (int64_t n = startID; n < stopID; ++n) {
            int64_t degree = useOutdeg ? g.out_degree(n) : g.in_degree(n);
            if (degree > avgDegree) {
                ++localOffsets[tid * PADDING];
                new_ids[n] = 1;  // Mark as hub
            }
        }
    }
    
    // Step 2: Compute prefix sums for hub offsets
    int64_t sum = 0;
    for (int tid = 0; tid < num_threads; ++tid) {
        auto origCount = localOffsets[tid * PADDING];
        localOffsets[tid * PADDING] = sum;
        sum += origCount;
    }
    
    // Step 3: Assign sequential IDs to hubs
    #pragma omp parallel
    {
        int64_t localCtr = 0;
        int tid = omp_get_thread_num();
        int64_t startID = partitionSz * tid;
        int64_t stopID = (tid == num_threads - 1) ? num_nodes : partitionSz * (tid + 1);
        
        for (int64_t n = startID; n < stopID; ++n) {
            if (new_ids[n] != static_cast<NodeID_>(UINT_E_MAX)) {
                new_ids[n] = static_cast<NodeID_>(localOffsets[tid * PADDING] + localCtr);
                ++localCtr;
            }
        }
    }
    delete[] localOffsets;
    
    // Step 4: Assign non-hub vertices to remaining positions (numHubs..num_nodes-1)
    // Uses per-thread partitions with pre-computed offsets (race-free, parallel).
    auto numHubs = sum;
    {
        int64_t non_hub_count[num_threads];
        int64_t new_index[num_threads];

        int64_t start_pos[num_threads], end_pos[num_threads];
        for (int th = 0; th < num_threads; ++th) {
            start_pos[th] = partitionSz * th;
            end_pos[th] = (th == num_threads - 1) ? num_nodes : partitionSz * (th + 1);
        }

        // Count non-hubs per thread partition
        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int th = 0; th < num_threads; ++th) {
            int64_t cnt = 0;
            for (int64_t v = start_pos[th]; v < end_pos[th]; ++v) {
                if (new_ids[v] == static_cast<NodeID_>(UINT_E_MAX)) ++cnt;
            }
            non_hub_count[th] = cnt;
        }

        // Prefix-sum to get each thread's starting position
        new_index[0] = numHubs;
        for (int th = 1; th < num_threads; ++th) {
            new_index[th] = new_index[th - 1] + non_hub_count[th - 1];
        }

        // Each thread assigns its non-hubs independently (no cross-thread deps)
        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int th = 0; th < num_threads; ++th) {
            for (int64_t v = start_pos[th]; v < end_pos[th]; ++v) {
                if (new_ids[v] == static_cast<NodeID_>(UINT_E_MAX)) {
                    new_ids[v] = static_cast<NodeID_>(new_index[th]++);
                }
            }
        }
    }
    
    t.Stop();
    PrintTime("HubCluster Map Time", t.Seconds());
}

// ============================================================================
// DBG - Degree-Based Grouping (Algorithm 5)
// ============================================================================

/**
 * @brief Group vertices into logarithmic degree buckets
 * 
 * Creates "frequency zones" where vertices with similar degrees
 * (and thus similar access frequencies) are placed together.
 * 
 * Bucket thresholds (based on average degree `av`):
 *   Bucket 0: degree <= av/2
 *   Bucket 1: degree <= av
 *   Bucket 2: degree <= av*2
 *   ...
 *   Bucket N: degree <= infinity
 * 
 * Higher-degree buckets are placed first for better cache utilization.
 * 
 * Complexity: O(n) - single pass through vertices
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param useOutdeg If true, use out-degree; else use in-degree
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateDBGMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                        pvector<NodeID_>& new_ids, 
                        bool useOutdeg) {
    Timer t;
    t.Start();
    
    const int64_t num_nodes = g.num_nodes();
    // Official DBG uses GA.m, the number of adjacency entries. For an
    // undirected CSR that is twice g.num_edges(); using the half-edge count
    // halves avg_degree and changes every bucket threshold on KRON/URAND.
    const int64_t num_edges = g.num_edges_directed();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("DBG Map Time", t.Seconds());
        return;
    }
    
    const uint32_t avg_degree = num_edges / num_nodes;
    
    // Define bucket thresholds (logarithmic scaling)
    const int num_buckets = 8;
    uint32_t bucket_threshold[] = {
        avg_degree / 2,
        avg_degree,
        avg_degree * 2,
        avg_degree * 4,
        avg_degree * 8,
        avg_degree * 16,
        avg_degree * 32,
        static_cast<uint32_t>(-1)  // Last bucket catches all remaining
    };
    
    // Thread-local buckets to avoid synchronization
    const int num_threads = omp_get_max_threads();
    std::vector<std::vector<uint32_t>> local_buckets[num_threads];
    for (int t = 0; t < num_threads; ++t) {
        local_buckets[t].resize(num_buckets);
    }
    
    // Step 1: Distribute vertices into thread-local buckets
    #pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < num_nodes; i++) {
        int64_t degree = useOutdeg ? g.out_degree(i) : g.in_degree(i);
        int tid = omp_get_thread_num();
        
        // Find appropriate bucket
        for (int j = 0; j < num_buckets; j++) {
            if (degree <= bucket_threshold[j]) {
                local_buckets[tid][j].push_back(i);
                break;
            }
        }
    }
    
    // Step 2: Compute starting positions for each thread's bucket
    // Process buckets from highest degree (last) to lowest (first)
    int temp_k = 0;
    uint32_t start_k[num_threads][num_buckets];
    
    for (int j = num_buckets - 1; j >= 0; j--) {
        for (int t = 0; t < num_threads; t++) {
            start_k[t][j] = temp_k;
            temp_k += local_buckets[t][j].size();
        }
    }
    
    // Step 3: Assign new IDs based on bucket positions
    #pragma omp parallel for schedule(static)
    for (int t = 0; t < num_threads; t++) {
        for (int j = num_buckets - 1; j >= 0; j--) {
            const auto& current_bucket = local_buckets[t][j];
            int k = start_k[t][j];
            for (size_t i = 0; i < current_bucket.size(); i++) {
                new_ids[current_bucket[i]] = k++;
            }
        }
    }
    
    // Cleanup
    for (int i = 0; i < num_threads; i++) {
        for (int j = 0; j < num_buckets; j++) {
            local_buckets[i][j].clear();
        }
    }
    
    t.Stop();
    PrintTime("DBG Map Time", t.Seconds());
}

// ============================================================================
// HUBSORTDBG (Algorithm 6)
// ============================================================================

/**
 * @brief HubSort within DBG buckets
 * 
 * Combines degree-based grouping with hub sorting:
 *   1. Separate vertices into hubs (degree > avg) and non-hubs
 *   2. Sort hubs by degree (descending)
 *   3. Hubs get IDs 0 to hubCount-1
 *   4. Non-hubs get remaining IDs preserving thread locality
 * 
 * This is effectively a 2-bucket DBG with sorted hubs.
 * 
 * Complexity: O(hubCount * log(hubCount)) for hub sorting
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param useOutdeg If true, use out-degree; else use in-degree
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateHubSortDBGMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                               pvector<NodeID_>& new_ids, 
                               bool useOutdeg) {
    
    using DegreeNodePair = std::pair<int64_t, NodeID_>;
    
    Timer t;
    t.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("HubSortDBG Map Time", t.Seconds());
        return;
    }
    
    const int64_t avgDegree = num_edges / num_nodes;
    
    const int num_threads = omp_get_max_threads();
    const int64_t slice = num_nodes / num_threads;
    
    // Per-thread storage for hub vertices
    pvector<DegreeNodePair> local_degree_id_pairs[num_threads];
    int64_t start[num_threads], end[num_threads];
    int64_t hub_count[num_threads], non_hub_count[num_threads];
    int64_t new_index[num_threads];
    
    // Initialize ranges
    for (int i = 0; i < num_threads; i++) {
        start[i] = i * slice;
        end[i] = (i + 1) * slice;
        hub_count[i] = 0;
    }
    end[num_threads - 1] = num_nodes;
    
    // Step 1: Collect hubs per thread
    #pragma omp parallel for schedule(static) num_threads(num_threads)
    for (int64_t th = 0; th < num_threads; th++) {
        for (int64_t v = start[th]; v < end[th]; ++v) {
            int64_t degree = useOutdeg ? g.out_degree(v) : g.in_degree(v);
            if (degree > avgDegree) {
                local_degree_id_pairs[th].push_back(std::make_pair(degree, v));
            }
        }
    }
    
    // Compute hub counts and non-hub counts
    size_t hubCount = 0;
    for (int th = 0; th < num_threads; th++) {
        hub_count[th] = local_degree_id_pairs[th].size();
        hubCount += hub_count[th];
        non_hub_count[th] = end[th] - start[th] - hub_count[th];
    }
    
    // Compute starting positions for non-hubs
    new_index[0] = hubCount;
    for (int th = 1; th < num_threads; th++) {
        new_index[th] = new_index[th - 1] + non_hub_count[th - 1];
    }
    
    // Step 2: Merge and sort hubs
    pvector<DegreeNodePair> degree_id_pairs(hubCount);
    size_t k = 0;
    for (int i = 0; i < num_threads; i++) {
        for (size_t j = 0; j < local_degree_id_pairs[i].size(); j++) {
            degree_id_pairs[k++] = local_degree_id_pairs[i][j];
        }
        local_degree_id_pairs[i].clear();
    }
    
    __gnu_parallel::stable_sort(degree_id_pairs.begin(), 
                                 degree_id_pairs.end(),
                                 std::greater<DegreeNodePair>());
    
    // Step 3: Assign IDs to hubs (0 to hubCount-1)
    #pragma omp parallel for
    for (size_t n = 0; n < hubCount; ++n) {
        new_ids[degree_id_pairs[n].second] = n;
    }
    pvector<DegreeNodePair>().swap(degree_id_pairs);
    
    // Step 4: Assign IDs to non-hubs (preserving thread locality)
    #pragma omp parallel for schedule(static) num_threads(num_threads)
    for (int th = 0; th < num_threads; th++) {
        for (int64_t v = start[th]; v < end[th]; ++v) {
            if (new_ids[v] == static_cast<NodeID_>(UINT_E_MAX)) {
                new_ids[v] = new_index[th]++;
            }
        }
    }
    
    t.Stop();
    PrintTime("HubSortDBG Map Time", t.Seconds());
}

// ============================================================================
// HUBCLUSTERDBG (Algorithm 7) - Recommended for power-law graphs
// ============================================================================

/**
 * @brief HubCluster within DBG buckets
 * 
 * A simplified 2-bucket DBG that separates hubs from non-hubs:
 *   - Bucket 0 (non-hubs): degree <= average
 *   - Bucket 1 (hubs): degree > average
 * 
 * Unlike HubSortDBG, this doesn't sort hubs by degree - it preserves
 * the original ordering within each bucket for better locality.
 * 
 * Best for: Power-law graphs where you want fast reordering with
 * good cache performance.
 * 
 * Complexity: O(n) - single pass, no sorting
 * 
 * @tparam NodeID_ Node ID type
 * @tparam DestID_ Destination ID type
 * @tparam invert Whether graph has inverse edges
 * @param g Input graph (CSR format)
 * @param new_ids Output permutation: new_ids[old_id] = new_id
 * @param useOutdeg If true, use out-degree; else use in-degree
 */
template <typename NodeID_, typename DestID_, bool invert>
void GenerateHubClusterDBGMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                                  pvector<NodeID_>& new_ids, 
                                  bool useOutdeg) {
    Timer t;
    t.Start();
    
    const int64_t num_nodes = g.num_nodes();
    const int64_t num_edges = g.num_edges();
    
    // GUARD: Empty graph - nothing to do
    if (num_nodes == 0) {
        t.Stop();
        PrintTime("HubClusterDBG Map Time", t.Seconds());
        return;
    }
    
    const uint32_t avg_degree = num_edges / num_nodes;
    
    // Two buckets: non-hubs (degree <= avg) and hubs (degree > avg)
    const int num_buckets = 2;
    uint32_t bucket_threshold[] = {avg_degree, static_cast<uint32_t>(-1)};
    
    const int num_threads = omp_get_max_threads();
    std::vector<std::vector<uint32_t>> local_buckets[num_threads];
    for (int t = 0; t < num_threads; ++t) {
        local_buckets[t].resize(num_buckets);
    }
    
    // Step 1: Distribute vertices into buckets
    #pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < num_nodes; i++) {
        int64_t degree = useOutdeg ? g.out_degree(i) : g.in_degree(i);
        int tid = omp_get_thread_num();
        
        for (int j = 0; j < num_buckets; j++) {
            if (degree <= bucket_threshold[j]) {
                local_buckets[tid][j].push_back(i);
                break;
            }
        }
    }
    
    // Step 2: Compute starting positions (high-degree bucket first)
    int temp_k = 0;
    uint32_t start_k[num_threads][num_buckets];
    
    for (int j = num_buckets - 1; j >= 0; j--) {
        for (int th = 0; th < num_threads; th++) {
            start_k[th][j] = temp_k;
            temp_k += local_buckets[th][j].size();
        }
    }
    
    // Step 3: Assign new IDs
    #pragma omp parallel for schedule(static)
    for (int th = 0; th < num_threads; th++) {
        for (int j = num_buckets - 1; j >= 0; j--) {
            const auto& current_bucket = local_buckets[th][j];
            int k = start_k[th][j];
            for (size_t i = 0; i < current_bucket.size(); i++) {
                new_ids[current_bucket[i]] = k++;
            }
        }
    }
    
    // Cleanup
    for (int i = 0; i < num_threads; i++) {
        for (int j = 0; j < num_buckets; j++) {
            local_buckets[i][j].clear();
        }
    }
    
    t.Stop();
    PrintTime("HubClusterDBG Map Time", t.Seconds());
}

#endif  // REORDER_HUB_H_
