// Copyright (c) 2024, UVA LavaLab
// SSSP with Cache Simulation

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"
#include "timer.h"

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

#include "graphbrew/partition/cagra/popt.h"

using namespace std;
using namespace cache_sim;

const WeightT kDistInf = numeric_limits<WeightT>::max() / 2;
const size_t kMaxBin = numeric_limits<size_t>::max() / 2;
const size_t kBinSizeThreshold = 1000;

template<typename CacheType>
inline void RelaxEdges_Sim(const WGraph &g, NodeID u, WeightT delta,
                           WeightT source_dist,
                           pvector<WeightT> &dist,
                           vector<vector<NodeID>> &local_bins,
                           CacheType &cache, GraphCacheContext &graph_ctx,
                           const vector<uint32_t> &vertex_masks,
                           int pfx_lookahead, int pfx_top_k,
                           bool record_charged, bool flowthrough,
                           bool compact_weighted,
                           const ::ecg_metadata::Config &ecg_meta,
                           WNode* out_edge_base) {
    (void)compact_weighted; (void)flowthrough;
    auto out_neigh = g.out_neigh(u);
    // ECG_EDGE_MASKS: consume the OUT-edge per-edge masks (transpose-correct — the
    // epoch is the next in-neighbour of dest > u, i.e. the next reader of dist[dest])
    // instead of the single per-vertex mask via the shared helper. Mirror of BFS
    // top-down. Inert on symmetric graphs; the correct dual mask for directed graphs.
    const size_t u_outdeg = (size_t)g.out_degree(u);
    size_t edge_pos = 0;
    for (auto it = out_neigh.begin(); it != out_neigh.end(); ++it, ++edge_pos) {
        // The weighted CSR record remains an 8-byte baseline demand. Charged
        // ECG additionally reads its packed metadata record; uncharged ISA
        // delivery leaves the baseline stream unchanged.
        // One delivery site. When the compact weighted record is feasible it
        // substitutes for the 8-byte weighted edge; otherwise the shared rule
        // has already downgraded delivery to a sidecar, which reads the edge
        // normally and adds a narrow entry. Previously these were two separate
        // if/else chains that had to agree.
        if (!record_charged) {
            SIM_CACHE_READ_EDGE(cache, it);
        } else {
            SIM_ECG_EDGE(cache, ecg_meta, it, out_edge_base,
                         ::ecg_metadata::kOutRecordBase,
                         ::ecg_metadata::kOutSidecarBase);
        }
        WNode wn = *it;
        if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
            if (graph_ctx.mask_config.prefetch_mode == 3) {
                // DROPLET-style: prefetch every next-K out-neighbor.
                auto jt = it;
                for (int step = 0; step < pfx_lookahead; step++) {
                    ++jt;
                    if (jt == out_neigh.end()) break;
                    WNode candidate_node = *jt;
                    NodeID candidate = candidate_node.v;
                    if (candidate < 0) continue;
                    SIM_CACHE_PREFETCH_VERTEX(cache, dist.data(),
                        static_cast<uint32_t>(candidate), graph_ctx);
                }
            } else {
                // Top-K POPT/degree-ranked selection.
                struct Cand { uint32_t v; uint16_t key; };
                Cand cands[64];
                int n_cand = 0;
                auto jt = it;
                for (int step = 0; step < pfx_lookahead; step++) {
                    ++jt;
                    if (jt == out_neigh.end()) break;
                    WNode candidate_node = *jt;
                    NodeID candidate = candidate_node.v;
                    if (candidate < 0) continue;
                    uint16_t key;
                    if (graph_ctx.mask_config.prefetch_mode == 1) {
                        uint64_t od = g.out_degree(candidate);
                        key = od > 65535 ? 0 : static_cast<uint16_t>(65535 - od);
                    } else {
                        key = graph_ctx.mask_config.decodePOPT(vertex_masks[candidate]);
                    }
                    cands[n_cand++] = {static_cast<uint32_t>(candidate), key};
                }
                if (n_cand == 0) {
                    graph_ctx.recordPrefetchNoTarget();
                } else if (pfx_top_k <= 1) {
                    int best = 0;
                    for (int i = 1; i < n_cand; i++)
                        if (cands[i].key < cands[best].key) best = i;
                    SIM_CACHE_PREFETCH_VERTEX(cache, dist.data(), cands[best].v, graph_ctx);
                } else {
                    int k_eff = pfx_top_k < n_cand ? pfx_top_k : n_cand;
                    for (int i = 0; i < k_eff; i++) {
                        int best = i;
                        for (int j = i + 1; j < n_cand; j++)
                            if (cands[j].key < cands[best].key) best = j;
                        if (best != i) std::swap(cands[i], cands[best]);
                        SIM_CACHE_PREFETCH_VERTEX(cache, dist.data(), cands[i].v, graph_ctx);
                    }
                }
            }
        }
        WeightT old_dist = dist[wn.v];
        WeightT new_dist = source_dist + wn.w;
        // Per-edge OUT mask (+ epoch hint) or per-vertex fallback. Re-resolved at the
        // retry read below (same dest -> same mask + epoch).
        uint32_t edge_mask_val = graph_ctx.resolveEdgeMaskAndEpoch(
            EdgeMaskDir::OUT, (uint32_t)u, u_outdeg, edge_pos, vertex_masks[wn.v]);
        SIM_CACHE_READ_MASKED(cache, dist.data(), wn.v, graph_ctx, edge_mask_val);
        while (new_dist < old_dist) {
            SIM_CACHE_WRITE(cache, dist.data(), wn.v);
            if (compare_and_swap(dist[wn.v], old_dist, new_dist)) {
                size_t dest_bin = new_dist / delta;
                if (dest_bin >= local_bins.size())
                    local_bins.resize(dest_bin + 1);
                local_bins[dest_bin].push_back(wn.v);
                break;
            }
            old_dist = dist[wn.v];
            edge_mask_val = graph_ctx.resolveEdgeMaskAndEpoch(
                EdgeMaskDir::OUT, (uint32_t)u, u_outdeg, edge_pos, vertex_masks[wn.v]);
            SIM_CACHE_READ_MASKED(cache, dist.data(), wn.v, graph_ctx, edge_mask_val);
        }
    }
}

template<typename CacheType>
pvector<WeightT> DeltaStep_Sim(const WGraph &g, NodeID source, 
                                WeightT delta, CacheType &cache) {
    pvector<WeightT> dist(
        g.num_nodes(), kDistInf, GRAPH_SIM_PROPERTY_ALIGNMENT);
    dist[source] = 0;

    // --- Graph-aware cache context ---
    GraphCacheContext graph_ctx;
    pvector<uint32_t> deg_arr(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        deg_arr[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(deg_arr.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());
    size_t llc_size = 8 * 1024 * 1024;
    llc_size = GetEnvSizeBytes("CACHE_L3_SIZE", llc_size);
    graph_ctx.registerPropertyArray(dist.data(), g.num_nodes(), sizeof(WeightT), llc_size);
    cache.initGraphContext(&graph_ctx);

    // Build P-OPT rereference matrix before masks so POPT-ranked PFX can use it.
    static pvector<uint8_t> popt_matrix;
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        const char* pfx_env = getenv("ECG_PREFETCH_MODE");
        bool popt_prefetch = pfx_env && atoi(pfx_env) == 2;
        if (policy == EvictionPolicy::POPT ||
            policy == EvictionPolicy::ECG || popt_prefetch) {
            constexpr int numVtxPerLine = 64 / sizeof(WeightT);
            constexpr int numEpochs = 256;
            // SSSP traverses out_neigh(u) reading dist[v]; next-ref of dist[v] is
            // in_neigh(v) => transpose = CSC/in_neigh (traverseCSR=false).
            buildAndRegisterReref(g, graph_ctx, /*natural_csr=*/false, "SSSP(push/out)",
                                  numVtxPerLine, numEpochs, popt_matrix);
            if (std::getenv("ECG_EXACT_REREF")) {
                const char* eb = std::getenv("ECG_EXACT_BITS");
                if (eb) graph_ctx.exact_bits = (uint32_t)atoi(eb);
                graph_ctx.registerOutAdjacencyExact(g);  // ECG_EXACT mode (sweep flavor)
            }
        }
    }

    // Compute per-vertex ECG mask array
    graph_ctx.initMaskConfig();
    cache.initGraphContext(&graph_ctx);
    auto vertex_masks = graph_ctx.computeVertexMasks(g);
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());
    // ECG_EDGE_MASKS (generic, matrix knob) / ECG_SSSP_EDGE_MASKS (alias): build the
    // OUT-edge per-edge masks so RelaxEdges carries the src-aware, transpose-correct
    // epoch (next in-neighbour of dest > u) per relaxed edge instead of the per-vertex
    // value. Mirror of BFS-TD. Inert on the symmetric corpus; correct for directed.
    if (std::getenv("ECG_EDGE_MASKS") || std::getenv("ECG_SSSP_EDGE_MASKS")) {
        graph_ctx.buildOutEdgeMasks(g);
        cout << "SSSP: OUT-edge per-edge masks enabled (push/out)" << endl;
    }
    const bool record_charged = GraphSimEcgEdgeRecord() &&
        GraphSimEnvIntClamped("ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    const bool flowthrough =
        GraphSimEnvIntClamped("ECG_FLOWTHROUGH", 0, 0, 1) > 0;
    int epoch_bits = 1;
    const uint32_t edge_epochs =
        graph_ctx.edge_epoch_count ? graph_ctx.edge_epoch_count : 2;
    while (epoch_bits < 16 &&
           (uint32_t(1) << epoch_bits) < edge_epochs) {
        ++epoch_bits;
    }
    bool compact_weighted =
        GraphSimEnvIntClamped("ECG_REUSE_PLAN_DEPTH", 0, 0, 4) == 2 &&
        static_cast<uint64_t>(g.num_nodes()) <=
            ecg_reuse_plan::kCompactWeightedMaxVertices;
    for (NodeID u = 0; compact_weighted && u < g.num_nodes(); ++u) {
        for (WNode edge : g.out_neigh(u)) {
            if (!ecg_reuse_plan::canPackCompactWeightedEdge(
                    g.num_nodes(), static_cast<uint32_t>(edge.v),
                    static_cast<int64_t>(edge.w))) {
                compact_weighted = false;
                break;
            }
        }
    }
    auto ecg_meta = ::ecg_metadata::configure(
        static_cast<uint64_t>(g.num_nodes()), edge_epochs);
    // The compact weighted record must actually pack dest+weight+stamps; when
    // it cannot, the shared rule downgrades delivery to a sidecar rather than dropping
    // the metadata.
    ::ecg_metadata::requirePackedFeasible(ecg_meta, compact_weighted);
    if (compact_weighted)
        ::ecg_metadata::declareContainerBytes(ecg_meta, 8);
    ::ecg_metadata::announce(ecg_meta, "sssp");
    ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "sssp");
    const int record_bytes = ecg_meta.record_bytes;
    (void)record_bytes; (void)epoch_bits;
    if (compact_weighted) {
        std::fprintf(
            stderr,
            "[ECG_COMPACT_REUSE_PLAN_WEIGHTED64] SSSP 8B replacement record ACTIVE\n");
    }
    WNode* out_edge_base = g.num_nodes() > 0
        ? g.out_neigh(0).begin() : nullptr;
    int pfx_lookahead = GraphSimEnvIntClamped("ECG_PREFETCH_LOOKAHEAD", 0, 0, 64);
    int pfx_top_k = GraphSimEnvIntClamped("ECG_PREFETCH_TOP_K", 1, 1, 64);
    if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
        cout << "SSSP relax PFX lookahead: window=" << pfx_lookahead
             << " mode=" << int(graph_ctx.mask_config.prefetch_mode)
             << " top_k=" << pfx_top_k << endl;
    }

    pvector<NodeID> frontier(g.num_edges_directed());
    size_t shared_indexes[2] = {0, kMaxBin};
    size_t frontier_tails[2] = {1, 0};
    frontier[0] = source;
    
    #pragma omp parallel
    {
        vector<vector<NodeID>> local_bins(0);
        size_t iter = 0;
        while (shared_indexes[iter&1] != kMaxBin) {
            size_t &curr_bin_index = shared_indexes[iter&1];
            size_t &next_bin_index = shared_indexes[(iter+1)&1];
            size_t &curr_frontier_tail = frontier_tails[iter&1];
            size_t &next_frontier_tail = frontier_tails[(iter+1)&1];
            
            #pragma omp for nowait schedule(dynamic, 64)
            for (size_t i = 0; i < curr_frontier_tail; i++) {
                NodeID u = frontier[i];
                SIM_SET_VERTEX(cache, u);
                graph_ctx.clearEdgeEpoch();  // dist[u] is a SEQUENTIAL source read, not a per-edge delivery
                SIM_CACHE_READ(cache, dist.data(), u);
                const WeightT source_dist = dist[u];
                if (source_dist >= delta * static_cast<WeightT>(curr_bin_index))
                    RelaxEdges_Sim(g, u, delta, source_dist, dist, local_bins, cache,
                                   graph_ctx, vertex_masks, pfx_lookahead,
                                   pfx_top_k, record_charged, flowthrough,
                                   compact_weighted,
                                   ecg_meta,
                                   out_edge_base);
            }

            while (curr_bin_index < local_bins.size() &&
                   !local_bins[curr_bin_index].empty() &&
                   local_bins[curr_bin_index].size() < kBinSizeThreshold) {
                vector<NodeID> curr_bin_copy = local_bins[curr_bin_index];
                local_bins[curr_bin_index].resize(0);
                for (NodeID u : curr_bin_copy) {
                    SIM_SET_VERTEX(cache, u);
                    graph_ctx.clearEdgeEpoch();  // dist[u] is a SEQUENTIAL source read, not a per-edge delivery
                    SIM_CACHE_READ(cache, dist.data(), u);
                    const WeightT source_dist = dist[u];
                    RelaxEdges_Sim(g, u, delta, source_dist, dist, local_bins, cache,
                                   graph_ctx, vertex_masks, pfx_lookahead,
                                   pfx_top_k, record_charged, flowthrough,
                                   compact_weighted,
                                   ecg_meta,
                                   out_edge_base);
                }
            }
            
            for (size_t i = curr_bin_index; i < local_bins.size(); i++) {
                if (!local_bins[i].empty()) {
                    #pragma omp critical
                    next_bin_index = min(next_bin_index, i);
                    break;
                }
            }
            
            #pragma omp barrier
            #pragma omp single nowait
            {
                curr_bin_index = kMaxBin;
                curr_frontier_tail = 0;
            }
            
            if (next_bin_index < local_bins.size()) {
                size_t copy_start = fetch_and_add(next_frontier_tail, 
                                                  local_bins[next_bin_index].size());
                copy(local_bins[next_bin_index].begin(),
                     local_bins[next_bin_index].end(), 
                     frontier.data() + copy_start);
                local_bins[next_bin_index].resize(0);
            }
            
            iter++;
            #pragma omp barrier
        }
    }
    
    return dist;
}

void PrintSSSPStats(const WGraph &g, const pvector<WeightT> &dist) {
    auto NotInf = [](WeightT d) { return d != kDistInf; };
    int64_t num_reached = count_if(dist.begin(), dist.end(), NotInf);
    cout << "SSSP Tree reaches " << static_cast<long long>(num_reached) << " nodes" << endl;
}

bool SSSPVerifier(const WGraph &g, NodeID source,
                  const pvector<WeightT> &dist_to_test) {
    pvector<WeightT> dist(g.num_nodes(), kDistInf);
    dist[source] = 0;
    
    typedef pair<WeightT, NodeID> WN;
    priority_queue<WN, vector<WN>, greater<WN>> mq;
    mq.push(make_pair(static_cast<WeightT>(0), source));
    
    while (!mq.empty()) {
        WeightT td = mq.top().first;
        NodeID u = mq.top().second;
        mq.pop();
        if (td == dist[u]) {
            for (WNode wn : g.out_neigh(u)) {
                if (td + wn.w < dist[wn.v]) {
                    dist[wn.v] = td + wn.w;
                    mq.push(make_pair(dist[wn.v], wn.v));
                }
            }
        }
    }
    
    for (NodeID n : g.vertices())
        if (dist[n] != dist_to_test[n])
            return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLDelta<WeightT> cli(argc, argv, "sssp-sim");
    if (!cli.ParseArgs())
        return -1;
    
    WeightedBuilder b(cli);
    WGraph g = b.MakeGraph();
    
    bool multicore = IsMultiCoreMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        SourcePicker<WGraph> sp(g, cli.start_vertex(), cli.num_trials());
        auto SSSPBound = [&sp, &cli, &cache](const WGraph &g) {
            return DeltaStep_Sim(g, sp.PickNext(), cli.delta(), cache);
        };
        SourcePicker<WGraph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const WGraph &g, const pvector<WeightT> &dist) {
            return SSSPVerifier(g, vsp.PickNext(), dist);
        };
        
        BenchmarkKernel(cli, g, SSSPBound, PrintSSSPStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    } else if (fast) {
        // FAST single-core cache simulation (no locks, ~10x faster)
        FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
        
        SourcePicker<WGraph> sp(g, cli.start_vertex(), cli.num_trials());
        auto SSSPBound = [&sp, &cli, &cache](const WGraph &g) {
            return DeltaStep_Sim(g, sp.PickNext(), cli.delta(), cache);
        };
        SourcePicker<WGraph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const WGraph &g, const pvector<WeightT> &dist) {
            return SSSPVerifier(g, vsp.PickNext(), dist);
        };
        
        BenchmarkKernel(cli, g, SSSPBound, PrintSSSPStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    } else {
        CacheHierarchy cache = CacheHierarchy::fromEnvironment();
        
        SourcePicker<WGraph> sp(g, cli.start_vertex(), cli.num_trials());
        auto SSSPBound = [&sp, &cli, &cache](const WGraph &g) {
            return DeltaStep_Sim(g, sp.PickNext(), cli.delta(), cache);
        };
        SourcePicker<WGraph> vsp(g, cli.start_vertex(), cli.num_trials());
        auto VerifierBound = [&vsp](const WGraph &g, const pvector<WeightT> &dist) {
            return SSSPVerifier(g, vsp.PickNext(), dist);
        };
        
        BenchmarkKernel(cli, g, SSSPBound, PrintSSSPStats, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
            }
        }
    }
    
    return 0;
}
