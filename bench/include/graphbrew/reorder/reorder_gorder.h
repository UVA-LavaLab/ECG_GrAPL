// ============================================================================
// GraphBrew — GOrder CSR Variants: Cache-Optimized Graph Ordering
// ============================================================================
//
// CSR-native variants for GOrder (Algorithm 9).  This file provides two
// variants that operate directly on CSRGraph using existing builder
// infrastructure for RCM and relabeling:
//
//   -o 9:csr   Serial CSR-native GOrder (faithful to original algorithm)
//   -o 9:fast   Parallel batch GOrder (scalable across threads)
//
// The original GOrder (Wei et al., SIGMOD 2016) maximizes a locality
// score S(v) for each candidate vertex v, measuring how many of v's
// neighbors (via 2-hop out→in paths) are already in a sliding window
// of the last W placed vertices.
//
// Shared architecture (both variants):
//   1. Lightweight RCM pre-ordering  (this file — matches GoGraph's BFS-CM)
//   2. RelabelByMappingStandalone    (reorder_types.h — parallel CSR rebuild)
//   3. Greedy GOrder on CSRGraph     (this file — direct iterator access)
//   4. Compose permutations          (parallel)
//
// Key improvements over the baseline GoGraph GOrder (-o 9):
//   1. CSR-native — zero GoGraph conversion overhead (speed ↑, memory ↓)
//   2. Direct CSRGraph iterator access — no flat-array copies
//   3. Sorted neighbor lists (from RelabelByMapping) for binary_search
//
// CSR variant (-o 9:csr):
//   Uses the original UnitHeap priority queue for exact serial greedy.
//   7-25% faster reordering than GoGraph baseline, equivalent quality.
//
// Fast variant (-o 9:fast):
//   Replaces UnitHeap with a score array + atomic deltas for thread
//   safety.  Batch extracts top-B vertices per round, parallelizes
//   push/pop score updates via omp parallel for.  Relaxations:
//     - Batch extraction: stale scores within each batch
//     - Fan-out cap (64): bounds 2-hop inner loop work
//     - Hub threshold n^(1/3) instead of n^(1/2)
//   Auto-tunes: batch=max(64, 4×threads), window=max(5, 2×batch).
//   2-3× greedy speedup on power-law graphs at 8 threads.
//
// RCM pre-ordering:
//   Uses GoGraph's lightweight BFS-CM approach: sort vertices by total
//   degree, BFS from each unvisited vertex expanding out-edges sorted
//   by degree, then reverse for RCM.  Much faster than the full RCM BNF.
//   GOrder only uses RCM as a warm start; the greedy loop does the real
//   optimization, so the simpler RCM is sufficient.
//
// References:
//   - Wei, H., Yu, J.X., Lu, C., Lin, X. (2016): "Speedup Graph
//     Processing by Graph Ordering." SIGMOD.
//
// License: MIT (original GOrder) + GraphBrew project license
// ============================================================================

#ifndef REORDER_GORDER_CSR_H_
#define REORDER_GORDER_CSR_H_

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <atomic>
#include <parallel/algorithm>
#include <omp.h>

// ============================================================================
// Internal implementation details
// ============================================================================
namespace gorder_csr_detail {

// --------------------------------------------------------------------------
// Helper — extract plain NodeID from DestID (handles weighted/unweighted)
// --------------------------------------------------------------------------
template <typename NodeID_, typename DestID_>
inline NodeID_ nbr_id(DestID_ d) {
    return static_cast<NodeID_>(d);
}

// --------------------------------------------------------------------------
// UnitHeap — O(1) increment/decrement priority queue
// --------------------------------------------------------------------------
// Faithful reimplementation of Gorder's UnitHeap using bucket-linked-lists.
// Keys are small non-negative integers. Supports:
//   IncrementKey(i): key[i] += 1 in O(1)
//   ExtractMax(): remove and return max-key element in amortized O(1)
//   DeleteElement(i): remove element i in O(1)
//   DecreaseTop(): lazy decrease of top element's key
//
// The 'update' array tracks deferred decrements: when update[i] < 0,
// the effective key is key[i] + update[i]. These are resolved lazily
// when i reaches the top of the heap via DecreaseTop().
// --------------------------------------------------------------------------

struct ListElement {
    int key;
    int prev;
    int next;
};

struct HeadEnd {
    int first  = -1;
    int second = -1;
};

class UnitHeap {
public:
    int* update;
    ListElement* list;
    std::vector<HeadEnd> header;
    int top;
    int heapsize;

    explicit UnitHeap(int size) : heapsize(size) {
        list = new ListElement[size];
        update = new int[size];
        header.resize(std::max(size >> 4, 4));

        for (int i = 0; i < size; ++i) {
            list[i].prev = i - 1;
            list[i].next = i + 1;
            list[i].key = 0;
            update[i] = 0;
        }
        list[size - 1].next = -1;
        header[0].first = 0;
        header[0].second = size - 1;
        top = 0;
    }

    ~UnitHeap() {
        delete[] list;
        delete[] update;
    }

    void DeleteElement(int index) {
        int prev = list[index].prev;
        int next = list[index].next;
        int key  = list[index].key;

        if (prev >= 0) list[prev].next = next;
        if (next >= 0) list[next].prev = prev;

        if (header[key].first == header[key].second) {
            header[key].first = header[key].second = -1;
        } else if (header[key].first == index) {
            header[key].first = next;
        } else if (header[key].second == index) {
            header[key].second = prev;
        }

        if (top == index) top = list[top].next;
        list[index].prev = list[index].next = -1;
    }

    int ExtractMax() {
        int tmptop;
        do {
            tmptop = top;
            if (update[top] < 0) DecreaseTop();
        } while (top != tmptop);
        DeleteElement(tmptop);
        return tmptop;
    }

    /**
     * P2 3.1e: DON-augmented ExtractMax — within the top bucket,
     * pick the vertex with highest DON priority (degree-based) to
     * break ties.  Scans at most the entire bucket, so overhead
     * scales with bucket size, not N.
     */
    int ExtractMaxDON(const std::vector<int>& don_priority) {
        // Resolve pending updates at top
        int tmptop;
        do {
            tmptop = top;
            if (update[top] < 0) DecreaseTop();
        } while (top != tmptop);

        // Scan the bucket for max DON priority
        int best = tmptop;
        int best_prio = don_priority[tmptop];
        int cur = list[tmptop].next;
        int bucket_key = list[tmptop].key;
        while (cur >= 0 && list[cur].key == bucket_key) {
            // Resolve deferred decrements for this element
            if (update[cur] < 0) {
                // Effective key is lower — not really in this bucket; stop scan
                break;
            }
            if (don_priority[cur] > best_prio) {
                best = cur;
                best_prio = don_priority[cur];
            }
            cur = list[cur].next;
        }
        DeleteElement(best);
        return best;
    }

    void IncrementKey(int index) {
        int key = list[index].key;
        int head = header[key].first;
        int prev = list[index].prev;
        int next = list[index].next;

        if (head != index) {
            list[prev].next = next;
            if (next >= 0) list[next].prev = prev;

            int headprev = list[head].prev;
            list[index].prev = headprev;
            list[index].next = head;
            list[head].prev = index;
            if (headprev >= 0) list[headprev].next = index;
        }

        list[index].key++;

        if (header[key].first == header[key].second)
            header[key].first = header[key].second = -1;
        else if (header[key].first == index)
            header[key].first = next;
        else if (header[key].second == index)
            header[key].second = prev;

        key++;
        if (key + 4 >= (int)header.size())
            header.resize(header.size() * 2);

        header[key].second = index;
        if (header[key].first < 0) header[key].first = index;

        if (list[top].key < key) top = index;
    }

    void DecreaseTop() {
        int tmptop = top;
        int key = list[tmptop].key;
        int next = list[tmptop].next;
        int p = key;
        // Clamp to 0 — negative keys would cause OOB in header[]
        int newkey = std::max(0, key + update[tmptop] - (update[tmptop] / 2));

        if (next >= 0 && newkey < list[next].key) {
            int tmp = (header[p].second >= 0) ? list[header[p].second].next : -1;
            while (tmp >= 0 && list[tmp].key >= newkey) {
                p = list[tmp].key;
                tmp = (header[p].second >= 0) ? list[header[p].second].next : -1;
            }
            list[next].prev = -1;
            int psecond = header[p].second;
            int tailnext = list[psecond].next;
            list[top].prev = psecond;
            list[top].next = tailnext;
            list[psecond].next = tmptop;
            if (tailnext >= 0) list[tailnext].prev = tmptop;
            top = next;

            if (header[key].first == header[key].second)
                header[key].first = header[key].second = -1;
            else
                header[key].first = next;

            list[tmptop].key = newkey;
            update[tmptop] /= 2;
            header[newkey].second = tmptop;
            if (header[newkey].first < 0) header[newkey].first = tmptop;
        }
    }

    void ReConstruct() {
        std::vector<int> tmp(heapsize);
        std::iota(tmp.begin(), tmp.end(), 0);

        std::sort(tmp.begin(), tmp.end(), [&](int a, int b) {
            return list[a].key > list[b].key;
        });

        int key = list[tmp[0]].key;
        list[tmp[0]].next = tmp[1];
        list[tmp[0]].prev = -1;
        list[tmp.back()].next = -1;
        list[tmp.back()].prev = tmp[tmp.size() - 2];
        header.assign(std::max(key + 1, (int)header.size()), HeadEnd{});
        header[key].first = tmp[0];

        for (int i = 1; i < (int)tmp.size() - 1; ++i) {
            int v = tmp[i];
            list[v].prev = tmp[i - 1];
            list[v].next = tmp[i + 1];
            int tmpkey = list[v].key;
            if (tmpkey != key) {
                header[key].second = tmp[i - 1];
                header[tmpkey].first = tmp[i];
                key = tmpkey;
            }
        }
        if (key == list[tmp.back()].key) {
            header[key].second = tmp.back();
        } else {
            header[key].second = tmp[tmp.size() - 2];
            int lastone = tmp.back();
            int lastkey = list[lastone].key;
            if (lastkey >= 0 && lastkey < (int)header.size())
                header[lastkey].first = header[lastkey].second = lastone;
        }
        top = tmp[0];
    }
};

// --------------------------------------------------------------------------
// Lightweight RCM pre-ordering (matches GoGraph's RCMOrder)
// --------------------------------------------------------------------------
// Simple BFS-CM on out-edges: sort all vertices by total degree, BFS from
// each unvisited vertex expanding only out-edges sorted by degree, reverse
// for RCM.  Matches GoGraph's behavior exactly.  Much faster than RCM BNF
// since GOrder only needs a rough locality pre-ordering.
//
// Returns perm[old] = new_id (RCM order).
// --------------------------------------------------------------------------
template <typename NodeID_, typename DestID_, bool invert>
void rcm_gorder(const CSRGraph<NodeID_, DestID_, invert>& g,
                int n,
                pvector<NodeID_>& perm) {

    // Total degree: out + in (same as GoGraph's outdegree + indegree)
    auto total_degree = [&](int v) -> int {
        int d = static_cast<int>(g.out_degree(v));
        if constexpr (invert) d += static_cast<int>(g.in_degree(v));
        return d;
    };

    // Sort all vertices by total degree ascending (deterministic seed order)
    // Uses __gnu_parallel::stable_sort matching GoGraph
    std::vector<int> deg_sorted(n);
    std::iota(deg_sorted.begin(), deg_sorted.end(), 0);
    __gnu_parallel::stable_sort(deg_sorted.begin(), deg_sorted.end(),
        [&](int a, int b) { return total_degree(a) < total_degree(b); });

    // BFS-CM from each unvisited vertex, expanding out-edges sorted by degree
    std::vector<bool> visited(n, false);
    std::vector<int> order;
    order.reserve(n);
    std::queue<int> que;
    std::vector<int> nbrs;  // vertex IDs for degree-sorted BFS

    for (int k = 0; k < n; ++k) {
        int start = deg_sorted[k];
        if (visited[start]) continue;

        que.push(start);
        visited[start] = true;
        order.push_back(start);

        while (!que.empty()) {
            int now = que.front();
            que.pop();

            // Collect out-neighbors (matching GoGraph: out-edges only)
            nbrs.clear();
            for (DestID_ dest : g.out_neigh(now))
                nbrs.push_back(static_cast<int>(nbr_id<NodeID_>(dest)));

            // Sort by total degree ascending (CM ordering)
            // stable_sort by degree only -- matches GoGraph (no vertex-ID tie-break)
            std::stable_sort(nbrs.begin(), nbrs.end(),
                [&](int a, int b) { return total_degree(a) < total_degree(b); });

            for (int v : nbrs) {
                if (!visited[v]) {
                    visited[v] = true;
                    que.push(v);
                    order.push_back(v);
                }
            }
        }
    }

    // Reverse for RCM: perm[old_id] = new_id
    perm.resize(n);
    for (int i = 0; i < n; ++i) {
        perm[order[i]] = static_cast<NodeID_>(n - 1 - i);
    }
}

// --------------------------------------------------------------------------
// GOrder Greedy -- Core algorithm (operates directly on CSRGraph)
// --------------------------------------------------------------------------
// Works on a pre-reordered CSRGraph (RCM order) using the graph's native
// out_neigh / in_neigh iterators.  No flat-array copies needed.
//
// For each placed vertex v, score updates go to:
//   Out-neighbors of v: 1-hop direct contribution
//   In-neighbors of v: each in-neighbor u gets +1, then each
//     out-neighbor w of u gets +1 (2-hop path: v <- u -> w)
//
// Hub vertices (outdeg > sqrt(n)) are skipped in 2-hop expansion.
// Neighbor lists are sorted (by RelabelByMappingStandalone), so
// binary_search works for popvexist optimization.
// --------------------------------------------------------------------------
template <typename NodeID_, typename DestID_, bool invert>
void gorder_greedy_csr(const CSRGraph<NodeID_, DestID_, invert>& g,
                       int n,
                       int window,
                       std::vector<int>& result_order) {

    const int hugevertex = static_cast<int>(std::sqrt(static_cast<double>(n)));

    auto outdeg = [&](int v) -> int { return static_cast<int>(g.out_degree(v)); };
    auto indeg  = [&](int v) -> int {
        if constexpr (invert) return static_cast<int>(g.in_degree(v));
        else return outdeg(v);  // fallback: treat as symmetric
    };

    // Initialize UnitHeap with indegree as initial key
    UnitHeap heap(n);
    for (int i = 0; i < n; ++i) {
        int id = indeg(i);
        heap.list[i].key = id;
        heap.update[i] = -id;
    }
    heap.ReConstruct();

    // Collect zero-degree vertices and find max-indegree vertex
    std::vector<int> zero;
    zero.reserve(std::min(n, 10000));

    int max_indeg_vertex = 0;
    int max_id = -1;
    for (int i = 0; i < n; ++i) {
        int id = indeg(i);
        int od = outdeg(i);
        if (id > max_id) {
            max_id = id;
            max_indeg_vertex = i;
        } else if (id + od == 0) {
            heap.update[i] = INT_MAX / 2;
            zero.push_back(i);
            heap.DeleteElement(i);
        }
    }

    auto score_inc = [&](int w) {
        if (heap.update[w] == 0) heap.IncrementKey(w);
        else                     heap.update[w]++;
    };
    auto score_dec = [&](int w) {
        heap.update[w]--;
    };

    // Helper: iterate in-neighbors (out-neighbors if no invert)
    auto for_each_in_neigh = [&](int v, auto&& fn) {
        if constexpr (invert) {
            for (DestID_ dest : g.in_neigh(v))
                fn(static_cast<int>(nbr_id<NodeID_>(dest)));
        } else {
            for (DestID_ dest : g.out_neigh(v))
                fn(static_cast<int>(nbr_id<NodeID_>(dest)));
        }
    };

    // Helper: iterate out-neighbors
    auto for_each_out_neigh = [&](int v, auto&& fn) {
        for (DestID_ dest : g.out_neigh(v))
            fn(static_cast<int>(nbr_id<NodeID_>(dest)));
    };

    // Helper: binary search in out-neighbors (sorted by RelabelByMapping)
    auto out_contains = [&](int u, int target) -> bool {
        auto neigh = g.out_neigh(u);
        return std::binary_search(neigh.begin(), neigh.end(),
                                  static_cast<DestID_>(target));
    };

    // Start with max-indegree vertex
    std::vector<int> order;
    order.reserve(n);
    order.push_back(max_indeg_vertex);
    heap.update[max_indeg_vertex] = INT_MAX / 2;
    heap.DeleteElement(max_indeg_vertex);

    // --- Initial push: score update for first placed vertex ---
    {
        int v0 = max_indeg_vertex;

        // In-neighbors of v0 (u -> v0): 2-hop via out-edges of u
        for_each_in_neigh(v0, [&](int u) {
            if (outdeg(u) <= hugevertex) {
                score_inc(u);
                if (outdeg(u) > 1) {
                    for_each_out_neigh(u, [&](int w) { score_inc(w); });
                }
            }
        });

        // Out-neighbors of v0 (v0 -> w): 1-hop direct
        if (outdeg(v0) <= hugevertex) {
            for_each_out_neigh(v0, [&](int w) { score_inc(w); });
        }
    }

    int count = 0;
    const int num_active = n - 1 - static_cast<int>(zero.size());
    std::vector<char> popvexist(n, 0);

    // P2 3.1e: DON priority for tiebreaking (total degree as proxy)
    // Only computed when ADAPTIVE_DON_TIEBREAK=1
    std::vector<int> don_priority;
    bool use_don = false;
    #ifndef GORDER_NO_ABLATION  // allow standalone builds without AblationConfig
    use_don = AblationConfig::Get().don_tiebreak;
    #endif
    if (use_don) {
        don_priority.resize(n);
        for (int i = 0; i < n; ++i)
            don_priority[i] = indeg(i) + outdeg(i);
    }

    while (count < num_active) {
        int v = use_don ? heap.ExtractMaxDON(don_priority) : heap.ExtractMax();
        count++;
        order.push_back(v);
        heap.update[v] = INT_MAX / 2;

        // --- Pop phase: remove oldest vertex from window ---
        int popv = (count - window >= 0) ? order[count - window] : -1;

        if (popv >= 0) {
            // Decrement for out-neighbors of popv (1-hop)
            if (outdeg(popv) <= hugevertex) {
                for_each_out_neigh(popv, [&](int w) { score_dec(w); });
            }

            // In-neighbors of popv: decrement + 2-hop via out-edges
            for_each_in_neigh(popv, [&](int u) {
                if (outdeg(u) <= hugevertex) {
                    score_dec(u);
                    if (outdeg(u) > 1) {
                        if (!out_contains(u, v)) {
                            for_each_out_neigh(u, [&](int w) { score_dec(w); });
                        } else {
                            popvexist[u] = true;
                        }
                    }
                }
            });
        }

        // --- Push phase: add current vertex v to window ---
        // Out-neighbors of v (1-hop)
        if (outdeg(v) <= hugevertex) {
            for_each_out_neigh(v, [&](int w) { score_inc(w); });
        }

        // In-neighbors of v: 2-hop via out-edges
        for_each_in_neigh(v, [&](int u) {
            if (outdeg(u) <= hugevertex) {
                score_inc(u);
                if (!popvexist[u]) {
                    if (outdeg(u) > 1) {
                        for_each_out_neigh(u, [&](int w) { score_inc(w); });
                    }
                } else {
                    popvexist[u] = false;
                }
            }
        });
    }

    // Insert isolated vertices before the last element
    if (!zero.empty()) {
        order.insert(order.end() - 1, zero.begin(), zero.end());
    }

    result_order.resize(n);
    for (int i = 0; i < n; ++i) {
        result_order[order[i]] = i;
    }
}

// --------------------------------------------------------------------------
// Parallel GOrder Greedy — Batch extraction with atomic score updates
// --------------------------------------------------------------------------
// Scalable parallel variant using batch vertex extraction and concurrent
// score updates via atomic fetch_add.  An active frontier (candidates with
// score > 0) provides efficient extraction without scanning all N vertices.
//
// Relaxations vs exact serial GOrder:
//   1. Batch extraction — top-B vertices placed per round (stale scores)
//   2. Fan-out cap — 2-hop expansion capped at 64 out-edges per in-neighbor
//   3. Hub threshold — uses n^(1/3) instead of n^(1/2)
//   4. No popvexist optimization (requires serial ordering within batch)
//
// Parallelism via:
//   - omp parallel for schedule(dynamic) over B push/pop updates per round
//   - Atomic fetch_add on a shared delta array for concurrent accumulation
//   - Active frontier for O(frontier_size) extraction vs O(n)
//
// Parameters:
//   window     — sliding window size (auto-scaled to max(5, 2*batch))
//   batch_size — vertices placed per round (auto-scaled to 2*threads)
// --------------------------------------------------------------------------
template <typename NodeID_, typename DestID_, bool invert>
void gorder_greedy_parallel(const CSRGraph<NodeID_, DestID_, invert>& g,
                            int n,
                            int window,
                            int batch_size,
                            std::vector<int>& result_order) {
    // --- Tuning knobs ---
    const int hugevertex = static_cast<int>(std::cbrt(static_cast<double>(n)));
    static constexpr int FANOUT_CAP = 64;

    auto outdeg = [&](int v) -> int { return static_cast<int>(g.out_degree(v)); };
    auto indeg  = [&](int v) -> int {
        if constexpr (invert) return static_cast<int>(g.in_degree(v));
        else return outdeg(v);
    };

    // --- Score tracking ---
    std::vector<int> score(n, 0);
    std::vector<char> placed(n, 0);
    std::vector<std::atomic<int>> delta(n);
    for (int i = 0; i < n; i++)
        delta[i].store(0, std::memory_order_relaxed);

    // --- Active frontier (vertices with score > 0 and not placed) ---
    std::vector<int> frontier;
    frontier.reserve(std::min(n, 100000));
    std::vector<char> in_frontier(n, 0);

    // --- Initialization ---
    int start_v = 0, max_id = -1;
    std::vector<int> zero;
    for (int i = 0; i < n; i++) {
        int id = indeg(i);
        if (id > max_id) { max_id = id; start_v = i; }
        if (id + outdeg(i) == 0) {
            zero.push_back(i);
            placed[i] = 1;
        }
    }

    // Cold-vertex fill order: indeg descending for priority when frontier empty
    std::vector<int> fill_order(n);
    std::iota(fill_order.begin(), fill_order.end(), 0);
    __gnu_parallel::stable_sort(fill_order.begin(), fill_order.end(),
        [&](int a, int b) { return indeg(a) > indeg(b); });
    int fill_cursor = 0;

    // P2 3.1e: DON priority for tiebreaking (total degree as proxy)
    std::vector<int> don_priority;
    bool use_don = false;
    #ifndef GORDER_NO_ABLATION
    use_don = AblationConfig::Get().don_tiebreak;
    #endif
    if (use_don) {
        don_priority.resize(n);
        for (int i = 0; i < n; ++i)
            don_priority[i] = indeg(i) + outdeg(i);
    }

    // --- Neighbor update helper (thread-safe via atomic delta) ---
    // sign = +1 for push (vertex enters window)
    // sign = -1 for pop  (vertex exits window)
    // 'seen' is a per-thread dedup array: touched records only unique vertices.
    auto update_neighbors = [&](int v, int sign,
                                std::vector<int>& touched,
                                std::vector<char>& seen) {
        auto touch = [&](int w) {
            if (placed[w]) return;
            delta[w].fetch_add(sign, std::memory_order_relaxed);
            if (!seen[w]) { seen[w] = 1; touched.push_back(w); }
        };
        // 1-hop: out-neighbors of v
        if (outdeg(v) <= hugevertex) {
            for (DestID_ d : g.out_neigh(v))
                touch(static_cast<int>(nbr_id<NodeID_>(d)));
        }
        // 2-hop: in-neighbors of v, then capped out-fan of each
        auto do_2hop = [&](auto in_range) {
            for (DestID_ d : in_range) {
                int u = static_cast<int>(nbr_id<NodeID_>(d));
                if (outdeg(u) > hugevertex) continue;
                touch(u);
                if (outdeg(u) > 1) {
                    int cnt = 0;
                    for (DestID_ e : g.out_neigh(u)) {
                        if (++cnt > FANOUT_CAP) break;
                        touch(static_cast<int>(nbr_id<NodeID_>(e)));
                    }
                }
            }
        };
        if constexpr (invert) do_2hop(g.in_neigh(v));
        else                  do_2hop(g.out_neigh(v));
    };

    // --- Place starting vertex ---
    std::vector<int> order;
    order.reserve(n);
    order.push_back(start_v);
    placed[start_v] = 1;
    {   // Initial push (serial for the single start vertex)
        std::vector<int> touched;
        std::vector<char> seen(n, 0);
        update_neighbors(start_v, +1, touched, seen);
        for (int w : touched) {
            seen[w] = 0;
            int d = delta[w].exchange(0, std::memory_order_relaxed);
            if (d != 0) score[w] += d;
            if (score[w] > 0 && !in_frontier[w] && !placed[w]) {
                frontier.push_back(w);
                in_frontier[w] = 1;
            }
        }
    }

    // total_to_place includes start vertex; pos=1 means 1 already placed
    const int total_to_place = n - static_cast<int>(zero.size());
    int pos = 1; // vertices placed so far (including start)

    // Thread-local touched lists and dedup arrays
    const int nthreads = omp_get_max_threads();
    std::vector<std::vector<int>> t_touched(nthreads);
    std::vector<std::vector<char>> t_seen(nthreads, std::vector<char>(n, 0));
    for (auto& tt : t_touched) tt.reserve(batch_size * FANOUT_CAP);

    while (pos < total_to_place) {
        int B = std::min(batch_size, total_to_place - pos);

        // === Phase 1: Extract top-B from frontier (serial) ===
        // Prune dead entries (placed or score <= 0)
        {
            int wp = 0;
            for (int i = 0; i < static_cast<int>(frontier.size()); i++) {
                int v = frontier[i];
                if (!placed[v] && score[v] > 0)
                    frontier[wp++] = v;
                else
                    in_frontier[v] = 0;
            }
            frontier.resize(wp);
        }

        // Select top-B by score (tie-break: DON priority or indeg)
        int B_front = std::min(B, static_cast<int>(frontier.size()));
        if (B_front > 0) {
            auto cmp = [&](int a, int b) {
                if (score[a] != score[b]) {
                    // P2 3.1e: When scores are within 5%, use DON priority
                    if (use_don) {
                        int hi = std::max(score[a], score[b]);
                        if (hi > 0 && std::abs(score[a] - score[b]) * 20 <= hi) {
                            // Within 5%: break tie by DON priority
                            if (don_priority[a] != don_priority[b])
                                return don_priority[a] > don_priority[b];
                        }
                    }
                    return score[a] > score[b];
                }
                // Exact tie: use DON priority if available, else indeg
                if (use_don && don_priority[a] != don_priority[b])
                    return don_priority[a] > don_priority[b];
                return indeg(a) > indeg(b);
            };
            if (static_cast<int>(frontier.size()) > B_front)
                std::partial_sort(frontier.begin(), frontier.begin() + B_front,
                                  frontier.end(), cmp);
            else
                std::sort(frontier.begin(), frontier.end(), cmp);
        }

        // Build batch from frontier top
        std::vector<int> batch;
        batch.reserve(B);
        for (int i = 0; i < B_front; i++) {
            int v = frontier[i];
            batch.push_back(v);
            placed[v] = 1;
            in_frontier[v] = 0;
        }
        if (B_front > 0)
            frontier.erase(frontier.begin(), frontier.begin() + B_front);

        // Fill remaining batch slots from cold vertices (highest indeg first)
        while (static_cast<int>(batch.size()) < B && fill_cursor < n) {
            int v = fill_order[fill_cursor++];
            if (!placed[v]) {
                batch.push_back(v);
                placed[v] = 1;
            }
        }
        // Safety sweep if fill_cursor exhausted (handles pruned-then-lost vertices)
        if (static_cast<int>(batch.size()) < B) {
            for (int v = 0; v < n && static_cast<int>(batch.size()) < B; v++) {
                if (!placed[v]) {
                    batch.push_back(v);
                    placed[v] = 1;
                }
            }
        }

        for (int v : batch) order.push_back(v);
        int actual_B = static_cast<int>(batch.size());
        if (actual_B == 0) break;

        // === Phase 2: Parallel push/pop score updates ===
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            t_touched[tid].clear();

            #pragma omp for schedule(dynamic)
            for (int i = 0; i < actual_B; i++) {
                // Push: batch[i] enters window
                update_neighbors(batch[i], +1, t_touched[tid], t_seen[tid]);
                // Pop: window-exit vertex
                int exit_pos = pos + i - window;
                if (exit_pos >= 0)
                    update_neighbors(order[exit_pos], -1, t_touched[tid], t_seen[tid]);
            }
        }

        // === Phase 3: Merge deltas and update frontier (serial) ===
        // Per-thread dedup ensures each vertex appears at most once per thread.
        // Cross-thread duplicates are handled by delta[w].exchange(0):
        // first exchange gets the accumulated value, subsequent return 0.
        for (int tid = 0; tid < nthreads; tid++) {
            for (int w : t_touched[tid]) {
                t_seen[tid][w] = 0;  // clear dedup flag
                if (placed[w]) continue;
                int d = delta[w].exchange(0, std::memory_order_relaxed);
                if (d != 0) score[w] += d;
                if (score[w] > 0 && !in_frontier[w]) {
                    frontier.push_back(w);
                    in_frontier[w] = 1;
                }
            }
            t_touched[tid].clear();
        }

        pos += actual_B;
    }

    // Append zero-degree vertices before last
    if (!zero.empty()) {
        if (order.size() > 1)
            order.insert(order.end() - 1, zero.begin(), zero.end());
        else
            order.insert(order.end(), zero.begin(), zero.end());
    }

    result_order.resize(n);
    for (int i = 0; i < n; i++)
        result_order[order[i]] = i;
}

} // namespace gorder_csr_detail

// ============================================================================
// Public API -- GenerateGOrderCSRMapping
// ============================================================================
// GOrder CSR variant:
//   Step 1: Lightweight RCM pre-ordering (GoGraph-style BFS-CM)
//   Step 2: RelabelByMappingStandalone to rebuild CSR in RCM order
//   Step 3: Greedy GOrder on reordered CSRGraph (direct iterator access)
//   Step 4: Compose permutations
// ============================================================================

template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateGOrderCSRMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                              pvector<NodeID_>& new_ids,
                              const std::string& /*filename*/,
                              bool /*symmetric*/ = false,
                              int window = -1) {
    // Window size: default W=5 matches SIGMOD'16 paper exactly.
    // The original GOrder paper (Wei et al., 2016) uses w=5 as its default.
    Timer tm;
    if (g.num_nodes() > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        std::cerr << "GOrder: graph has " << g.num_nodes()
                  << " nodes, exceeding int32 limit. Falling back to identity.\n";
        new_ids.resize(g.num_nodes());
        #pragma omp parallel for
        for (int64_t i = 0; i < g.num_nodes(); ++i)
            new_ids[i] = static_cast<NodeID_>(i);
        return;
    }
    const int n = static_cast<int>(g.num_nodes());
    if (n == 0) return;

    if (window <= 0) {
        window = 5;  // Match SIGMOD'16 paper default (w=5)
    }

    // Allow runtime window override via GORDER_WINDOW env var
    if (const char* env_w = std::getenv("GORDER_WINDOW")) {
        int w = std::atoi(env_w);
        if (w >= 2 && w <= 100) window = w;
    }

    std::cout << "GOrder_CSR window=" << window << std::endl;

    if (static_cast<int64_t>(new_ids.size()) < n)
        new_ids.resize(n);

    // --- Step 1: Lightweight RCM pre-ordering (matches GoGraph) ---
    tm.Start();
    pvector<NodeID_> rcm_ids(n);
    gorder_csr_detail::rcm_gorder(g, n, rcm_ids);
    tm.Stop();
    PrintTime("GOrder_CSR RCM", tm.Seconds());

    // --- Step 2: Rebuild CSR in RCM order (existing builder infrastructure) ---
    tm.Start();
    auto g2 = RelabelByMappingStandalone<NodeID_, DestID_, invert>(g, rcm_ids);
    tm.Stop();
    // (RelabelByMappingStandalone already prints "Relabel Map Time")

    // --- Step 3: Run greedy GOrder on reordered graph ---
    tm.Start();
    std::vector<int> greedy_order;
    gorder_csr_detail::gorder_greedy_csr(g2, n, window, greedy_order);
    tm.Stop();
    PrintTime("GOrder_CSR greedy", tm.Seconds());

    // --- Step 4: Compose permutations ---
    // greedy_order maps RCM IDs -> final positions
    // rcm_ids maps original IDs -> RCM IDs
    // So: new_ids[orig] = greedy_order[rcm_ids[orig]]
    tm.Start();
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        new_ids[i] = static_cast<NodeID_>(greedy_order[rcm_ids[i]]);
    }
    tm.Stop();
    PrintTime("GOrder_CSR compose", tm.Seconds());
}

// ============================================================================
// Public API -- GenerateGOrderFastMapping (parallel batch variant)
// ============================================================================
// Parallel GOrder (-o 9:fast):
//   Step 1: Lightweight RCM pre-ordering (same as csr variant)
//   Step 2: RelabelByMappingStandalone (parallel CSR rebuild)
//   Step 3: Parallel batch greedy with atomic score updates
//   Step 4: Compose permutations (parallel)
//
// Auto-tuning: batch = max(8, 2*threads), window = max(5, 2*batch)
// ============================================================================

template <typename NodeID_, typename DestID_, typename WeightT_, bool invert>
void GenerateGOrderFastMapping(const CSRGraph<NodeID_, DestID_, invert>& g,
                               pvector<NodeID_>& new_ids,
                               const std::string& /*filename*/) {
    Timer tm;
    if (g.num_nodes() > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        std::cerr << "GOrder_fast: graph has " << g.num_nodes()
                  << " nodes, exceeding int32 limit. Falling back to identity.\n";
        new_ids.resize(g.num_nodes());
        #pragma omp parallel for
        for (int64_t i = 0; i < g.num_nodes(); ++i)
            new_ids[i] = static_cast<NodeID_>(i);
        return;
    }
    const int n = static_cast<int>(g.num_nodes());

    if (n == 0) return;

    if (static_cast<int64_t>(new_ids.size()) < n)
        new_ids.resize(n);

    const int nthreads = omp_get_max_threads();
    const int batch  = std::max(64, nthreads * 4);
    const int window = std::max(5, batch * 2);

    std::cout << "GOrder_fast config: batch=" << batch
              << " window=" << window
              << " threads=" << nthreads << std::endl;

    // --- Step 1: Lightweight RCM pre-ordering ---
    tm.Start();
    pvector<NodeID_> rcm_ids(n);
    gorder_csr_detail::rcm_gorder(g, n, rcm_ids);
    tm.Stop();
    PrintTime("GOrder_fast RCM", tm.Seconds());

    // --- Step 2: Rebuild CSR in RCM order ---
    tm.Start();
    auto g2 = RelabelByMappingStandalone<NodeID_, DestID_, invert>(g, rcm_ids);
    tm.Stop();
    // (RelabelByMappingStandalone already prints "Relabel Map Time")

    // --- Step 3: Parallel batch greedy ---
    tm.Start();
    std::vector<int> greedy_order;
    gorder_csr_detail::gorder_greedy_parallel(g2, n, window, batch, greedy_order);
    tm.Stop();
    PrintTime("GOrder_fast greedy", tm.Seconds());

    // --- Step 4: Compose permutations ---
    tm.Start();
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        new_ids[i] = static_cast<NodeID_>(greedy_order[rcm_ids[i]]);
    }
    tm.Stop();
    PrintTime("GOrder_fast compose", tm.Seconds());
}

#endif  // REORDER_GORDER_CSR_H_
