#include <atomic>
// Copyright (c) 2024, UVA LavaLab
// PageRank with Cache Simulation
// Tracks all memory accesses to graph data structures
// Supports both single-core and multi-core cache simulation

#include <algorithm>
#include <iostream>
#include <vector>
#include <fstream>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

// Cache simulation headers
#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"
// P-OPT rereference matrix builder
#include "graphbrew/partition/cagra/popt.h"
// Shared ECG epoch helpers for cache_sim, gem5, and Sniper
#include "ecg_reuse_plan_builder.h"

using namespace std;
using namespace cache_sim;

typedef float ScoreT;
const float kDamp = 0.85;

// PageRank with cache simulation - template version works with both cache types
template<typename CacheType>
pvector<ScoreT> PageRankPullGS_Sim(const Graph &g, CacheType &cache,
                                    int max_iters, double epsilon = 0,
                                    bool logging_enabled = false) {
    const ScoreT init_score = 1.0f / g.num_nodes();
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> scores(
        g.num_nodes(), init_score, GRAPH_SIM_PROPERTY_ALIGNMENT);
    pvector<ScoreT> outgoing_contrib(
        g.num_nodes(), ScoreT(0), GRAPH_SIM_PROPERTY_ALIGNMENT);
    
    // Get raw pointers for cache tracking
    ScoreT* scores_ptr = scores.data();
    ScoreT* contrib_ptr = outgoing_contrib.data();

    // --- Graph-aware cache context (for GRASP/P-OPT/ECG policies) ---
    // Registers property arrays so cache policies know hot/warm/cold regions.
    // Auto-computes hot fraction from degree distribution (self-tuning).
    GraphCacheContext graph_ctx;

    // Build degree array for topology init
    pvector<uint32_t> degrees(g.num_nodes());
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++)
        degrees[n] = static_cast<uint32_t>(g.out_degree(n));
    graph_ctx.initTopology(degrees.data(), g.num_nodes(),
                           g.num_edges_directed(), g.directed());

    // Upstream GRASP protects the source contribution region (propertyA), not
    // the next-score destination array. Both remain property data for P-OPT/ECG.
    size_t llc_size = 8 * 1024 * 1024;  // Default 8MB, overridden by env
    llc_size = GetEnvSizeBytes("CACHE_L3_SIZE", llc_size);
    graph_ctx.registerPropertyArray(scores_ptr, g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    graph_ctx.registerPropertyArray(contrib_ptr, g.num_nodes(), sizeof(ScoreT), llc_size, -1.0, true);
    cache.initGraphContext(&graph_ctx);

    // Build P-OPT rereference matrix (for POPT and ECG policies)
    // Uses graph structure to predict future cache line accesses.
    // numVtxPerLine = cache_line_size / sizeof(ScoreT) = 64/4 = 16
    static pvector<uint8_t> popt_matrix;  // Must outlive graph_ctx
    {
        const EvictionPolicy policy = GraphSimEffectiveL3Policy();
        const char* pfx_env = getenv("ECG_PREFETCH_MODE");
        bool popt_prefetch = pfx_env && (atoi(pfx_env) == 2 || atoi(pfx_env) == 4 || atoi(pfx_env) == 6 || atoi(pfx_env) == 7);
        const bool matrix_free_reuse_plan = GraphSimMatrixFreeReusePlan();
        if (policy == EvictionPolicy::POPT ||
            (policy == EvictionPolicy::ECG && !matrix_free_reuse_plan) ||
            (popt_prefetch && !matrix_free_reuse_plan)) {
            constexpr int numVtxPerLine = 64 / sizeof(ScoreT);
            constexpr int numEpochs = 256;
            buildAndRegisterReref(g, graph_ctx, /*natural_csr=*/true, "PR(pull/in)",
                                  numVtxPerLine, numEpochs, popt_matrix);
            if (std::getenv("ECG_EXACT_REREF")) {
                const char* eb = std::getenv("ECG_EXACT_BITS");
                if (eb) graph_ctx.exact_bits = (uint32_t)atoi(eb);
                graph_ctx.registerOutAdjacencyExact(g);  // ECG_EXACT mode
            }
        }
    }

    // Compute per-vertex ECG mask array (supports 8/16/32-bit widths)
    graph_ctx.initMaskConfig();
    auto vertex_masks = graph_ctx.computeVertexMasks(g);  // uint32_t per vertex
    graph_ctx.initMaskArray32(vertex_masks.data(), vertex_masks.size());
    graph_ctx.printSummary();
    int pfx_lookahead = GraphSimEnvIntClamped("ECG_PREFETCH_LOOKAHEAD", 0, 0, 64);
    int pfx_top_k = GraphSimEnvIntClamped("ECG_PREFETCH_TOP_K", 1, 1, 64);
    bool edge_mask_charged = GraphSimEnvIntClamped("ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    if (graph_ctx.mask_config.prefetch_mode == 6 || graph_ctx.mask_config.prefetch_mode == 7) {
        int edge_mask_lookahead = GraphSimEnvIntClamped("ECG_EDGE_MASK_LOOKAHEAD", 8, 1, 64);
        int edge_mask_k_jump = GraphSimEnvIntClamped("ECG_EDGE_MASK_K_JUMP", 4, 1, 1024);
        cout << "PR per-edge ECG mask: mode=" << int(graph_ctx.mask_config.prefetch_mode)
             << " charged=" << (edge_mask_charged ? "yes" : "no") << endl;
        if (graph_ctx.mask_config.prefetch_mode == 6) {
            cout << "  lookahead=" << edge_mask_lookahead << " (mode 6 = next-K in src's own edges)" << endl;
            graph_ctx.buildInEdgeMasks_PR(g, edge_mask_lookahead);
        } else {
            cout << "  k_jump=" << edge_mask_k_jump << " (mode 7 = cross-iteration prefetch)" << endl;
            graph_ctx.buildInEdgeMasks_PR_CrossIter(g, edge_mask_k_jump);
        }
    }
    if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
        cout << "PR PFX lookahead: window=" << pfx_lookahead
             << " mode=" << int(graph_ctx.mask_config.prefetch_mode)
             << " top_k=" << pfx_top_k << endl;
    }
    
    // Initialize outgoing contributions
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        outgoing_contrib[n] = init_score / g.out_degree(n);
    }
    // Identical post-metadata warm replay across cache_sim/gem5/Sniper.
    graph_ctx.clearEdgeEpoch();
    #pragma omp parallel for
    for (NodeID n = 0; n < g.num_nodes(); ++n) {
        SIM_CACHE_READ(cache, scores_ptr, n);
        SIM_CACHE_WRITE(cache, scores_ptr, n);
        SIM_CACHE_READ(cache, contrib_ptr, n);
        SIM_CACHE_WRITE(cache, contrib_ptr, n);
    }
    cache.resetStats();
    const auto in_edge_base = g.num_nodes() > 0
        ? g.in_neigh(0).begin() : nullptr;
    
    for (int iter = 0; iter < max_iters; iter++) {
        double error = 0;
        
        #pragma omp parallel for reduction(+ : error) schedule(dynamic, 64)
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            ScoreT incoming_total = 0;

            // P-OPT: update current destination vertex for rereference lookup
            SIM_SET_VERTEX(cache, u);

            // Iterate over incoming neighbors with CSR edge tracking
            auto in_neigh = g.in_neigh(u);

            // === Mode 6: per-edge ECG mask path ===
            // Each src has a precomputed mask per edge in its in_neigh list.
            // Mask is 64-bit packed: dest_id|DBG|POPT|prefetch_target. dest_id
            // is decoded from the mask (so the mask effectively replaces the
            // direct CSR load); prefetch_target is src-iteration-aware.
            //
            // ECG_EDGE_MASK_CHARGED=1 (default): explicitly model the cache
            // traffic for reading the mask array (fair comparison)
            // ECG_EDGE_MASK_CHARGED=0: idealized — mask is "free" register hint
            // (to isolate whether the mechanism CAN help, separate from traffic cost)
            if (graph_ctx.mask_config.prefetch_mode == 6 || graph_ctx.mask_config.prefetch_mode == 7) {
                const auto& src_masks = graph_ctx.in_edge_masks_by_src[u];
                const auto& src_eps = graph_ctx.in_edge_epoch_by_src[u];
                const auto& src_tiers = graph_ctx.in_edge_grasp_tier_by_src[u];
                const bool edge_mask_lean = GraphSimEnvIntClamped("ECG_EDGE_MASK_LEAN", 0, 0, 1) > 0;
                const bool edge_mask_pack = GraphSimEnvIntClamped("ECG_EDGE_MASK_PACK", 0, 0, 1) > 0;
                // Combined stack: DROPLET-style lookahead prefetch layered ON TOP of
                // the ECG_GRASP_POPT epoch eviction. The epoch stamp reduces TOTAL
                // memory traffic (fewer unique fetches — something DROPLET cannot do,
                // it only relocates traffic); the lookahead prefetch then hides the
                // latency of the remaining demand misses. ECG_EDGE_MASK_PREFETCH=K
                // prefetches the next-K in-neighbors' contrib[] (like DROPLET mode 3).
                const int lean_pfx_k = GraphSimEnvIntClamped("ECG_EDGE_MASK_PREFETCH", 0, 0, 64);
                // 100M-scale option B: when the epoch CANNOT ride the edge word's spare
                // bits (id_bits too large), it must be an explicit per-edge field read
                // from a side array (2-byte uint16 = up to 65535 epochs). This charges
                // that extra streamed traffic so the bandwidth comparison is honest at
                // scale. (At N<=~2M the epoch packs for free; leave this off there.)
                const bool epoch_charged = GraphSimEnvIntClamped("ECG_EDGE_MASK_EPOCH_CHARGED", 0, 0, 1) > 0;
                // 8-byte-record auto-switch: pick the per-edge record width from N so
                // the full mask suite fits, and charge the wider record stream. record
                // <=4 keeps the 4-byte CSR edge read (epoch in spare bits); >=8 reads
                // the 8-byte packed record (src_masks, naturally 8B/edge) which delivers
                // dest+DBG+POPT+epoch+prefetch in ONE stream (no separate side array).
                const uint32_t rec_ne = graph_ctx.edge_epoch_count
                    ? graph_ctx.edge_epoch_count : 2u;
                // Structure, width, and placement use the shared definition.
                const auto ecg_meta = ::ecg_metadata::configure(
                    static_cast<uint64_t>(g.num_nodes()), rec_ne);
                const int record_bytes = ecg_meta.record_bytes;
                ::ecg_metadata::announce(ecg_meta, "pr");
                ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "pr");
                uint32_t id_bits = 1; while (id_bits < 31 && (1u << id_bits) < (uint32_t)g.num_nodes()) id_bits++;
                const uint32_t id_mask = (id_bits >= 32) ? 0xFFFFFFFFu : ((1u << id_bits) - 1);
                size_t edge_pos = 0;
                for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it, ++edge_pos) {
                    uint64_t mask = (edge_pos < src_masks.size()) ? src_masks[edge_pos] : 0;
                    NodeID v;
                    if (edge_mask_lean) {
                        // LEAN/PACKED delivery (ECG_GRASP_POPT realizability): the
                        // epoch packs into the spare high bits of the existing 4-byte
                        // edge word (web-Google IDs are ~20-bit -> 12 spare bits =
                        // 4096 epochs), so reading the edge (exactly like POPT) ALSO
                        // delivers the epoch — ZERO extra traffic. ecg.extract pulls
                        // the epoch from the loaded edge word. No prefetch.
                        // Auto-switch record read: width = record_bytes. <=4 reads the
                        // 4-byte CSR edge (epoch in spare bits, 16 edges/line). >=8 reads
                        // a globally contiguous 8-byte packed-record stream in the same
                        // CSR edge order (8 records/line = 2x edge traffic). 16B charges
                        // a second 8-byte half per record (4 records/line).
                        if (src_masks.empty()) {
                            SIM_CACHE_READ_EDGE(cache, it);
                        } else {
                            SIM_ECG_EDGE(cache, ecg_meta, it, in_edge_base,
                                         ::ecg_metadata::kInRecordBase,
                                         ::ecg_metadata::kInSidecarBase);
                        }
                        v = *it;
                        // Back-compat: legacy explicit 2-byte epoch charge (superseded by
                        // the record auto-switch; only fires for the 4-byte path on request).
                        if (epoch_charged && record_bytes <= 4 && edge_pos < src_eps.size())
                            SIM_CACHE_READ(cache, src_eps.data(), edge_pos);
                        // Combined stack: prefetch the next-K in-neighbors' contrib[]
                        // (DROPLET-style) on top of the ECG_GRASP_POPT epoch eviction.
                        // Stamp each prefetched line with ITS OWN next-ref epoch (from
                        // src_eps) so it participates correctly in the circular-distance
                        // eviction instead of inheriting the current demand's epoch —
                        // otherwise the prefetch displaces eviction-protected lines and
                        // reverts the bandwidth gain.
                        if (lean_pfx_k > 0) {
                            uint16_t saved_ep = graph_ctx.hints_for_thread().edge_epoch;
                            bool saved_epoch_valid =
                                graph_ctx.hints_for_thread().edge_epoch_valid;
                            uint8_t saved_tier =
                                graph_ctx.hints_for_thread().edge_grasp_tier;
                            bool saved_tier_valid =
                                graph_ctx.hints_for_thread().edge_grasp_tier_valid;
                            uint8_t saved_sched_n =
                                graph_ctx.hints_for_thread().edge_epoch_sched_n;
                            uint16_t saved_sched[4] = {};
                            for (uint8_t k = 0; k < 4; ++k)
                                saved_sched[k] =
                                    graph_ctx.hints_for_thread().edge_epoch_sched[k];
                            // Epoch filter (ecg_reuse_plan::prefetchKeep): skip low-value prefetches.
                            const int pfx_filter = GraphSimEnvIntClamped("ECG_PREFETCH_EPOCH_FILTER", 0, 0, 2);
                            const int pfx_thresh_pct = GraphSimEnvIntClamped("ECG_PREFETCH_EPOCH_THRESH_PCT", 50, 0, 100);
                            const uint32_t cur_ep_k = ecg_reuse_plan::currentEpoch(u, g.num_nodes(), (uint32_t)rec_ne);
                            const uint32_t thresh = (uint32_t)(((uint64_t)pfx_thresh_pct * (uint64_t)rec_ne) / 100);
                            auto jt = it;
                            size_t cpos = edge_pos;
                            for (int step = 0; step < lean_pfx_k; step++) {
                                ++jt; ++cpos;
                                if (jt == in_neigh.end()) break;
                                NodeID cand = *jt;
                                if (cand < 0) continue;
                                uint16_t cand_ep = (cpos < src_eps.size()) ? src_eps[cpos] : saved_ep;
                                if (edge_mask_pack) {
                                    // Faithful delivery: a lookahead prefetcher reads the
                                    // SAME packed record it walks ahead in the stream and
                                    // extracts the epoch IDENTICALLY to the demand path —
                                    // no higher-resolution side channel. Use the SAME
                                    // container width as the demand path (record_bytes>=8 ->
                                    // 64-bit, so the full epoch is preserved at scale; <=4 ->
                                    // 32-bit). Previously this packed into 32 bits always,
                                    // folding the epoch for 8B records (id_bits+epoch>32).
                                    if (record_bytes >= 8) {
                                        uint64_t id_mask64 = (id_bits >= 64) ? ~0ULL : ((1ULL << id_bits) - 1ULL);
                                        uint64_t packed = ((uint64_t)cand & id_mask64) | ((uint64_t)cand_ep << id_bits);
                                        cand = static_cast<NodeID>(packed & id_mask64);
                                        cand_ep = static_cast<uint16_t>(packed >> id_bits);
                                    } else {
                                        uint32_t packed = ((uint32_t)cand & id_mask) | ((uint32_t)cand_ep << id_bits);
                                        cand = static_cast<NodeID>(packed & id_mask);
                                        cand_ep = static_cast<uint16_t>(packed >> id_bits);
                                    }
                                }
                                if (!ecg_reuse_plan::prefetchKeep(cand_ep, cur_ep_k,
                                        (uint32_t)rec_ne, pfx_filter, thresh))
                                    continue;
                                graph_ctx.hints_for_thread().edge_epoch = cand_ep;
                                graph_ctx.hints_for_thread().edge_epoch_valid = true;
                                graph_ctx.hints_for_thread().edge_grasp_tier =
                                    cpos < src_tiers.size() ? src_tiers[cpos] : 0;
                                graph_ctx.hints_for_thread().edge_grasp_tier_valid =
                                    graph_ctx.hints_for_thread().edge_grasp_tier != 0;
                                if (graph_ctx.edge_epoch_reuse_plan_depth) {
                                    const auto& sc =
                                        graph_ctx.in_edge_epoch_sched_by_src[u];
                                    auto& H = graph_ctx.hints_for_thread();
                                    const uint32_t K =
                                        graph_ctx.edge_epoch_reuse_plan_depth;
                                    H.edge_epoch_sched_n =
                                        static_cast<uint8_t>(
                                            std::min<uint32_t>(K, 4));
                                    for (uint8_t k = 0;
                                         k < H.edge_epoch_sched_n; ++k) {
                                        H.edge_epoch_sched[k] =
                                            cpos * K + k < sc.size()
                                                ? sc[cpos * K + k]
                                                : cand_ep;
                                    }
                                } else {
                                    graph_ctx.hints_for_thread().
                                        edge_epoch_sched_n = 0;
                                }
                                SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr,
                                    static_cast<uint32_t>(cand), graph_ctx);
                            }
                            graph_ctx.hints_for_thread().edge_epoch = saved_ep;
                            graph_ctx.hints_for_thread().edge_epoch_valid =
                                saved_epoch_valid;
                            graph_ctx.hints_for_thread().edge_grasp_tier = saved_tier;
                            graph_ctx.hints_for_thread().edge_grasp_tier_valid =
                                saved_tier_valid;
                            graph_ctx.hints_for_thread().edge_epoch_sched_n =
                                saved_sched_n;
                            for (uint8_t k = 0; k < 4; ++k)
                                graph_ctx.hints_for_thread().edge_epoch_sched[k] =
                                    saved_sched[k];
                        }
                    } else {
                        // Fat-mask path: decode dest from the mask (REPLACES the CSR
                        // edge read), optional prefetch.
                        if (edge_mask_charged && !src_masks.empty())
                            SIM_CACHE_READ(cache, src_masks.data(), edge_pos);
                        v = static_cast<NodeID>(GraphCacheContext::edgeMaskDest(mask));
                        uint32_t prefetch_target = GraphCacheContext::edgeMaskPrefetch(mask);
                        if (prefetch_target != 0)
                            SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr, prefetch_target, graph_ctx);
                        int amplify = GraphSimEnvIntClamped("ECG_EDGE_MASK_AMPLIFY", 0, 0, 8);
                        for (int step = 1; step <= amplify && edge_pos + step < src_masks.size(); step++) {
                            uint32_t fwd_dest = GraphCacheContext::edgeMaskDest(src_masks[edge_pos + step]);
                            SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr, fwd_dest, graph_ctx);
                        }
                    }
                    // Carry the full-resolution absolute epoch from the dedicated
                    // per-edge array (32-bit demand_hint truncates bit 32).
                    uint32_t demand_hint = static_cast<uint32_t>(mask & 0xFFFFFFFFu);
                    uint16_t carried_epoch = (edge_pos < src_eps.size()) ? src_eps[edge_pos]
                        : static_cast<uint16_t>(GraphCacheContext::edgeMaskPOPT(mask));
                    if (edge_mask_pack && edge_mask_lean) {
                        // REAL PACKING PROOF: pack the epoch into the spare high bits
                        // of the (already-loaded) per-edge record, then unpack — the
                        // epoch rides the SAME read (zero extra traffic). The pack
                        // CONTAINER must match the record width: a 4-byte fat-CSR edge
                        // word (record_bytes<=4) packs into 32 bits (ne_cap guarantees
                        // id_bits+epoch<=32, lossless); an 8-byte ISA record
                        // (record_bytes>=8) packs into 64 bits so the FULL epoch is
                        // preserved at scale (id_bits+<=16-bit epoch <= 64) instead of
                        // overflowing uint32 and folding the high epoch bits back into
                        // the eviction. Round-trip must recover neighbor and epoch.
                        static std::atomic<uint64_t> pk_total{0}, pk_bad{0};
                        NodeID v_un; uint16_t ep_un;
                        if (record_bytes >= 8) {
                            uint64_t id_mask64 = (id_bits >= 64) ? ~0ULL : ((1ULL << id_bits) - 1ULL);
                            uint64_t packed = ((uint64_t)v & id_mask64) | ((uint64_t)carried_epoch << id_bits);
                            v_un = static_cast<NodeID>(packed & id_mask64);
                            ep_un = static_cast<uint16_t>(packed >> id_bits);
                        } else {
                            uint32_t packed = ((uint32_t)v & id_mask) | ((uint32_t)carried_epoch << id_bits);
                            v_un = static_cast<NodeID>(packed & id_mask);
                            ep_un = static_cast<uint16_t>(packed >> id_bits);
                        }
                        ++pk_total;
                        if (v_un != v || ep_un != carried_epoch) ++pk_bad;
                        if ((pk_total.load() % 5000000ULL) == 0)
                            std::cerr << "[PACK] checked=" << pk_total.load()
                                      << " roundtrip_mismatch=" << pk_bad.load() << std::endl;
                        v = v_un; carried_epoch = ep_un;
                    }
                    graph_ctx.hints_for_thread().edge_epoch = carried_epoch;
                    graph_ctx.hints_for_thread().edge_epoch_valid = true;
                    graph_ctx.hints_for_thread().edge_grasp_tier =
                        edge_pos < src_tiers.size() ? src_tiers[edge_pos] : 0;
                    graph_ctx.hints_for_thread().edge_grasp_tier_valid =
                        graph_ctx.hints_for_thread().edge_grasp_tier != 0;
                    // ECG_REUSE_PLAN_DEPTH: deliver the per-edge forward schedule so the
                    // resident line can self-advance across epochs (matrix-like). Inert
                    // (n=0) when ECG_REUSE_PLAN_DEPTH is unset.
                    if (graph_ctx.edge_epoch_reuse_plan_depth) {
                        const auto& sc = graph_ctx.in_edge_epoch_sched_by_src[u];
                        uint32_t K = graph_ctx.edge_epoch_reuse_plan_depth;
                        auto& H = graph_ctx.hints_for_thread();
                        uint8_t kn = static_cast<uint8_t>(std::min<uint32_t>(K, 4));
                        H.edge_epoch_sched_n = kn;
                        for (uint8_t k = 0; k < kn; ++k)
                            H.edge_epoch_sched[k] = ((size_t)edge_pos * K + k < sc.size())
                                ? sc[(size_t)edge_pos * K + k] : carried_epoch;
                        static uint64_t reuse_plan_trace_sequence = 0;
                        static const uint64_t reuse_plan_trace_limit = []() {
                            const char* value =
                                std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
                            return value
                                ? static_cast<uint64_t>(
                                      std::strtoull(value, nullptr, 10))
                                : 0;
                        }();
                        const uint64_t sequence = reuse_plan_trace_sequence++;
                        if (kn >= 2 && sequence < reuse_plan_trace_limit) {
                            const uint16_t expected_first =
                                ((size_t)edge_pos * K < sc.size())
                                    ? sc[(size_t)edge_pos * K]
                                    : carried_epoch;
                            const uint16_t expected_second =
                                ((size_t)edge_pos * K + 1 < sc.size())
                                    ? sc[(size_t)edge_pos * K + 1]
                                    : carried_epoch;
                            std::fprintf(stderr,
                                "[ECG-ReusePlan-EXPECT sim=cache_sim seq=%llu "
                                "dest=%u tier=%u epoch1=%u epoch2=%u]\n",
                                (unsigned long long)sequence,
                                static_cast<unsigned>(v),
                                static_cast<unsigned>(
                                    graph_ctx.hints_for_thread().edge_grasp_tier),
                                static_cast<unsigned>(expected_first),
                                static_cast<unsigned>(expected_second));
                            std::fprintf(stderr,
                                "[ECG-ReusePlan-RECV sim=cache_sim seq=%llu "
                                "dest=%u tier=%u epoch1=%u epoch2=%u]\n",
                                (unsigned long long)sequence,
                                static_cast<unsigned>(v),
                                static_cast<unsigned>(
                                    graph_ctx.hints_for_thread().edge_grasp_tier),
                                static_cast<unsigned>(H.edge_epoch_sched[0]),
                                static_cast<unsigned>(H.edge_epoch_sched[1]));
                        }
                    } else {
                        graph_ctx.hints_for_thread().edge_epoch_sched_n = 0;
                    }
                    SIM_CACHE_READ_MASKED(cache, contrib_ptr, v, graph_ctx, demand_hint);
                    incoming_total += outgoing_contrib[v];
                }
                // Score update (replicated here so mode 6 matches the canonical path)
                graph_ctx.clearEdgeEpoch();
                SIM_CACHE_READ(cache, scores_ptr, u);
                ScoreT old_score = scores[u];
                ScoreT new_score = base_score + kDamp * incoming_total;
                SIM_CACHE_WRITE(cache, scores_ptr, u);
                scores[u] = new_score;
                error += fabs(new_score - old_score);
                SIM_CACHE_WRITE(cache, contrib_ptr, u);
                outgoing_contrib[u] = new_score / g.out_degree(u);
                continue;  // skip the rest of this u's body (we handled it above)
            }

            for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it) {
                // Track CSR edge list read (reading neighbor ID from edge array)
                SIM_CACHE_READ_EDGE(cache, it);
                NodeID v = *it;
                // ECG: read contrib[v] with mask. With lookahead enabled, issue
                // the prefetch from upcoming incoming-neighbor IDs before their
                // demand read; otherwise use the per-vertex PFX target directly.
                //
                // Prefetch modes:
                //   1 = degree-ranked (ECG_PFX): pick most-popular among next K
                //   2 = POPT-ranked   (ECG_PFX): pick lowest-POPT-rank among next K
                //   3 = sequential    (DROPLET): prefetch ALL next K (no selection)
                //
                // Mode 3 = DROPLET-in-cache_sim. DROPLET's Sniper impl monitors
                // edge stream and stride-prefetches destination properties.
                // Cache_sim has explicit edge access markers so we can deliver
                // the same semantic (prefetch next-K in-neighbors' contrib[])
                // without the runtime stride detection. Faithful comparator
                // for the ECG_PFX claim.
                if (pfx_lookahead > 0 && graph_ctx.mask_config.prefetch_mode > 0) {
                    if (graph_ctx.mask_config.prefetch_mode == 3) {
                        // DROPLET-style: prefetch every next-K in-neighbor
                        // sequentially. No target selection — just sweep the
                        // upcoming edge stream's destinations.
                        auto jt = it;
                        for (int step = 0; step < pfx_lookahead; step++) {
                            ++jt;
                            if (jt == in_neigh.end()) break;
                            NodeID candidate = *jt;
                            if (candidate < 0) continue;
                            SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr,
                                static_cast<uint32_t>(candidate), graph_ctx);
                        }
                        SIM_CACHE_READ_MASKED(cache, contrib_ptr, v, graph_ctx, vertex_masks[v]);
                    } else {
                        // Mode 1 = degree-ranked, Mode 2 = POPT-ranked.
                        // Top-K extension: instead of issuing
                        // just the single best target, collect all candidates
                        // from the lookahead window and issue prefetches for
                        // the top-K ranked. K=1 reproduces the original
                        // single-best behavior; K>1 trades selection quality
                        // for higher prefetch volume (closer to DROPLET in
                        // bandwidth, but still POPT-quality filtered).
                        struct Cand { uint32_t v; uint16_t key; };
                        Cand cands[64];  // max lookahead is 64
                        int n_cand = 0;
                        auto jt = it;
                        for (int step = 0; step < pfx_lookahead; step++) {
                            ++jt;
                            if (jt == in_neigh.end()) break;
                            NodeID candidate = *jt;
                            if (candidate < 0) continue;
                            // EXACT-ranked prefetch (ECG_PFX_EXACT): rank candidates
                            // by the EXACT next-reference distance of their property
                            // line at the current traversal vertex u — finer than the
                            // coarse 7-bit POPT bucket (decodePOPT), the prefetch analog
                            // of the ECG:EXACT eviction win. Smaller distance = sooner
                            // reused = higher prefetch priority.
                            static const bool pfx_exact = std::getenv("ECG_PFX_EXACT") != nullptr;
                            uint16_t key;
                            if (pfx_exact && !graph_ctx.exact_off.empty()) {
                                uint32_t d = graph_ctx.exactNextRef(
                                    reinterpret_cast<uint64_t>(contrib_ptr + candidate),
                                    static_cast<uint32_t>(u));
                                key = d > 65535 ? 65535 : static_cast<uint16_t>(d);
                            } else if (graph_ctx.mask_config.prefetch_mode == 1) {
                                // Larger out_degree = "more popular" — invert
                                // for sorting (smaller key = higher priority).
                                uint64_t od = g.out_degree(candidate);
                                key = od > 65535 ? 0 : static_cast<uint16_t>(65535 - od);
                            } else {
                                // Lower POPT rank = sooner-rereferenced = higher priority.
                                key = graph_ctx.mask_config.decodePOPT(vertex_masks[candidate]);
                            }
                            cands[n_cand++] = {static_cast<uint32_t>(candidate), key};
                        }
                        if (n_cand == 0) {
                            graph_ctx.recordPrefetchNoTarget();
                        } else if (pfx_top_k <= 1) {
                            // Fast path: single best target — match historical mode-2 behavior bit-for-bit.
                            int best = 0;
                            for (int i = 1; i < n_cand; i++)
                                if (cands[i].key < cands[best].key) best = i;
                            SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr, cands[best].v, graph_ctx);
                        } else {
                            // Top-K path: partial sort by key (ascending), issue first K.
                            int k_eff = pfx_top_k < n_cand ? pfx_top_k : n_cand;
                            for (int i = 0; i < k_eff; i++) {
                                int best = i;
                                for (int j = i + 1; j < n_cand; j++)
                                    if (cands[j].key < cands[best].key) best = j;
                                if (best != i) std::swap(cands[i], cands[best]);
                                SIM_CACHE_PREFETCH_VERTEX(cache, contrib_ptr, cands[i].v, graph_ctx);
                            }
                        }
                        SIM_CACHE_READ_MASKED(cache, contrib_ptr, v, graph_ctx, vertex_masks[v]);
                    }
                } else {
                    SIM_CACHE_READ_MASKED_PREFETCH(cache, contrib_ptr, v, graph_ctx, vertex_masks[v]);
                }
                incoming_total += outgoing_contrib[v];
            }
            
            // Track: read old score, write new score
            SIM_CACHE_READ(cache, scores_ptr, u);
            ScoreT old_score = scores[u];
            ScoreT new_score = base_score + kDamp * incoming_total;
            SIM_CACHE_WRITE(cache, scores_ptr, u);
            scores[u] = new_score;
            error += fabs(new_score - old_score);
            
            // Update contribution for next iteration
            SIM_CACHE_WRITE(cache, contrib_ptr, u);
            outgoing_contrib[u] = new_score / g.out_degree(u);
        }
        
        if (logging_enabled)
            cout << "Iteration " << iter << ": error = " << error << endl;
        
        if (error < epsilon)
            break;
    }
    
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        score_pairs[n] = make_pair(n, scores[n]);
    }
    int k = 5;
    partial_sort(score_pairs.begin(), score_pairs.begin() + k, score_pairs.end(),
                [](auto a, auto b) { return a.second > b.second; });
    for (int i = 0; i < k; i++) {
        cout << score_pairs[i].first << ": " << score_pairs[i].second << endl;
    }
}

bool PRVerifier(const Graph &g, const pvector<ScoreT> &scores, double target_error) {
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> incomming_sums(g.num_nodes(), 0);
    double error = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        ScoreT outgoing_contrib = scores[u] / g.out_degree(u);
        for (NodeID v : g.out_neigh(u))
            incomming_sums[v] += outgoing_contrib;
    }
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        error += fabs(base_score + kDamp * incomming_sums[n] - scores[n]);
        incomming_sums[n] = 0;
    }
    cout << "Total Error: " << error << endl;
    return error < target_error;
}

int main(int argc, char *argv[]) {
    CLPageRank cli(argc, argv, "pagerank-sim", 1e-4, 20);
    if (!cli.ParseArgs())
        return -1;
    
    Builder b(cli);
    Graph g = b.MakeGraph();
    
    // Check modes: multi-core vs single-core, ultrafast vs fast vs accurate
    bool multicore = IsMultiCoreMode();
    bool sampled = IsSampledMode();
    bool ultrafast = IsUltraFastMode();
    bool fast = IsFastMode();
    
    if (multicore) {
        // Multi-core cache simulation (private L1/L2, shared L3)
        MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
        
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankPullGS_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };
        
        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else if (sampled) {
        // SAMPLED cache simulation (~5-20x faster with statistical sampling)
        SampledCacheHierarchy cache = SampledCacheHierarchy::fromEnvironment();
        
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankPullGS_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };
        
        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else if (ultrafast) {
        // ULTRA-FAST cache simulation (packed structures, best performance)
        UltraFastCacheHierarchy cache = UltraFastCacheHierarchy::fromEnvironment();
        
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankPullGS_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };
        
        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else if (fast) {
        // FAST single-core cache simulation (no locks)
        FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
        
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankPullGS_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };
        
        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        
        cout << endl;
        cache.printStats();
        
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    } else {
        // Original single-core cache simulation (with locks, slower but full LRU)
        CacheHierarchy cache = CacheHierarchy::fromEnvironment();
        
        auto PRBound = [&cli, &cache](const Graph &g) {
            return PageRankPullGS_Sim(g, cache, cli.max_iters(), cli.tolerance());
        };
        auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
            return PRVerifier(g, scores, cli.tolerance());
        };
        
        BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
        
        // Print cache statistics
        cout << endl;
        cache.printStats();
        
        // Export to JSON if requested
        const char* json_file = getenv("CACHE_OUTPUT_JSON");
        if (json_file) {
            ofstream ofs(json_file);
            if (ofs.is_open()) {
                ofs << cache.toJSON() << endl;
                ofs.close();
                cout << "Cache stats exported to: " << json_file << endl;
            }
        }
    }
    
    return 0;
}
