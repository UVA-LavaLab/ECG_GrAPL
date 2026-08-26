// ============================================================================
// SSSP delta-stepping for gem5 SE-mode simulation
// ============================================================================

#include <cinttypes>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <queue>
#include <vector>
#include <omp.h>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"

#include "graphbrew/partition/cagra/popt.h"
#include "ecg_reuse_plan_builder.h"
#include "ecg_mode6_builder.h"

#include "gem5_sim/gem5_harness.h"
#include "ecg_metadata.h"

using namespace std;

const WeightT kDistInf = numeric_limits<WeightT>::max() / 2;
const size_t kMaxBin = numeric_limits<size_t>::max() / 2;
const size_t kBinSizeThreshold = 1000;

inline void RelaxEdges_Gem5(const WGraph &g, NodeID u, WeightT delta,
                            pvector<WeightT> &dist,
                            vector<vector<NodeID>> &local_bins,
                            const vector<vector<uint16_t>>* out_edge_epochs,
                            const pvector<uint64_t>* pair_flat,
                            const pvector<uint32_t>* pair_sidecars,
                            const pvector<uint64_t>* pair_compact,
                            bool ecg_load_evict_on, int ecg_evict_wc,
                            uint32_t edge_epoch_count, bool ecg_plan_load_on,
                            bool ecg_flow_load_on,
                            bool ecg_bind_iload_on,
                            bool compact_pair_ok,
                            const Gem5EcgEpochQuantizer& epoch_quantizer) {
    const bool ecg_bind_computed_address_on =
        ecg_bind_iload_on && gem5_ecg_bind_computed_address_enabled();
    const WeightT source_dist = dist[u];
    GEM5_SET_QUANTIZED_VERTEX_EPOCH(epoch_quantizer, u);
    int pfx_lookahead = gem5_env_int_clamped("GEM5_ECG_PFX_LOOKAHEAD", 4, 0, 64);
    const vector<uint16_t>* u_epochs =
        (out_edge_epochs && static_cast<size_t>(u) < out_edge_epochs->size())
            ? &(*out_edge_epochs)[u] : nullptr;
    const uint64_t pair_row_begin = pair_flat
        ? static_cast<uint64_t>(g.out_offset(u))
        : 0;
    if (compact_pair_ok && ecg_bind_iload_on && pair_compact &&
        pfx_lookahead == 0) {
        const uint64_t begin = pair_row_begin;
        const uint64_t end =
            static_cast<uint64_t>(g.out_offset(u + 1));
        for (uint64_t pos = begin; pos < end; ++pos) {
            const uint64_t record = ecg_flow_load_on
                ? gem5_ecg_flow_load_instruction(&(*pair_compact)[pos])
                : (*pair_compact)[pos];
            const NodeID dest = static_cast<NodeID>(
                ecg_reuse_plan::extractCompactWeightedDest(record));
            const WeightT weight = static_cast<WeightT>(
                ecg_reuse_plan::extractCompactWeightedWeight(record));
            const uint32_t bits = ecg_bind_computed_address_on
                ? gem5_ecg_bind_load_cw24(&dist[dest], record)
                : gem5_ecg_bind_iload_cw24(dist.data(), record);
            WeightT old_dist;
            std::memcpy(&old_dist, &bits, sizeof(WeightT));
            const WeightT new_dist = source_dist + weight;
            while (new_dist < old_dist) {
                if (compare_and_swap(dist[dest], old_dist, new_dist)) {
                    size_t dest_bin = new_dist / delta;
                    if (dest_bin >= local_bins.size())
                        local_bins.resize(dest_bin + 1);
                    local_bins[dest_bin].push_back(dest);
                    break;
                }
                old_dist = dist[dest];
            }
        }
        return;
    }
    auto out_neigh = g.out_neigh(u);
    size_t edge_pos = 0;
    for (auto it = out_neigh.begin(); it != out_neigh.end(); ++it, ++edge_pos) {
        WNode wn = *it;
        if (pfx_lookahead > 0) {
            auto jt = it;
            for (int step = 0; step < pfx_lookahead; step++) {
                ++jt;
                if (jt == out_neigh.end()) break;
                GEM5_ECG_PFX_TARGET((*jt).v);
                break;
            }
        } else {
            GEM5_ECG_PFX_TARGET(wn.v);
        }
        // A5: read dist[wn.v]. On the ecg.load EVICT path the load also stamps the property
        // line with wn.v's next-ref epoch (push_out_edges=true transpose matches the out-edge
        // relax) in one custom-0 op, so ECG_GRASP_POPT ranks dist[] by next-reference.
        WeightT old_dist;
        if (pair_flat && pair_sidecars) {
            const uint64_t pos = pair_row_begin + edge_pos;
            // The sidecar supplies the upper 32 mask bits. The masked property
            // load combines them with the edge destination and attaches the ReusePlan
            // pair to the exact dist[dest] request. FlowThrough remains on the
            // sidecar load, not the governed property request.
            const uint32_t sidecar = ecg_flow_load_on
                ? gem5_ecg_flow_weighted_load_instruction(
                    &(*pair_sidecars)[pos])
                : ecg_plan_load_on && !ecg_bind_iload_on
                    ? gem5_ecg_plan_weighted_load_instruction(
                        &(*pair_sidecars)[pos], static_cast<uint32_t>(wn.v))
                    : (*pair_sidecars)[pos];
            const uint64_t record =
                ecg_reuse_plan::combineWeightedReusePlanRecord(
                    static_cast<uint32_t>(wn.v), sidecar);
            if (ecg_bind_iload_on) {
                if (ecg_bind_computed_address_on) {
                    old_dist = static_cast<WeightT>(
                        gem5_ecg_bind_load_s32(&dist[wn.v], record));
                } else {
                    const uint32_t bits =
                        gem5_ecg_bind_iload_u32(dist.data(), record);
                    std::memcpy(&old_dist, &bits, sizeof(WeightT));
                }
            } else {
                if (!ecg_plan_load_on)
                    GEM5_ECG_EXTRACT2(record);
                old_dist = dist[wn.v];
                GEM5_ECG_CLEAR_EXTRACT2_HINT();
            }
        } else if (ecg_load_evict_on && u_epochs) {
            uint16_t epoch = (edge_pos < u_epochs->size())
                ? (*u_epochs)[edge_pos]
                : static_cast<uint16_t>(edge_epoch_count - 1);
            uint64_t fat = ecg_mode6::packEvict(static_cast<uint32_t>(wn.v),
                                                epoch, ecg_evict_wc);
            uint32_t bits = gem5_ecg_load_evict(dist.data(), fat, ecg_evict_wc);
            std::memcpy(&old_dist, &bits, sizeof(WeightT));
        } else {
            old_dist = dist[wn.v];
        }
        WeightT new_dist = source_dist + wn.w;
        while (new_dist < old_dist) {
            if (compare_and_swap(dist[wn.v], old_dist, new_dist)) {
                size_t dest_bin = new_dist / delta;
                if (dest_bin >= local_bins.size())
                    local_bins.resize(dest_bin + 1);
                local_bins[dest_bin].push_back(wn.v);
                break;
            }
            // The reference configuration is single-threaded, so this retry read is
            // contention-free and intentionally remains a plain reload.
            old_dist = dist[wn.v];
        }
    }
}

pvector<WeightT> DeltaStep_Gem5(const WGraph &g, NodeID source, WeightT delta) {
    constexpr size_t kPropAlign = 4096;  // page-align hot property array (see pr.cc)
    pvector<WeightT> dist(g.num_nodes(), kDistInf, kPropAlign);
    dist[source] = 0;

    gem5_report_region("dist", dist.data(), g.num_nodes(), sizeof(WeightT));

    Gem5PropertyRegion regions[1] = {
        {"dist", reinterpret_cast<uint64_t>(dist.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(WeightT),
         static_cast<uint32_t>(g.num_nodes()), sizeof(WeightT)},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);

    // Per-edge next-ref epoch budget. two-epoch ReusePlan uses two 15-bit epochs in
    // its fixed 8-byte record and therefore bypasses the legacy 32-bit cap.
    constexpr int kNumVtxPerLine = 64 / sizeof(WeightT);
    const int ecg_reuse_plan_depth =
        gem5_env_int_clamped("ECG_REUSE_PLAN_DEPTH", 0, 0, 4);
    uint32_t requested_epoch_count = static_cast<uint32_t>(
        gem5_env_int_clamped("ECG_EDGE_MASK_EPOCHS", 65535, 2, 65535));
    if (ecg_reuse_plan_depth == 2)
        requested_epoch_count =
            ecg_reuse_plan::normalizeReusePlanEpochCount(requested_epoch_count);
    uint8_t edge_id_bits = 1;
    while ((1ULL << edge_id_bits) < static_cast<uint64_t>(g.num_nodes())) edge_id_bits++;
    uint32_t edge_epoch_count = requested_epoch_count;
    if (ecg_reuse_plan_depth != 2) {
        if (edge_id_bits < 32) {
            uint32_t spare = 32u - edge_id_bits;
            uint32_t ne_cap = (spare >= 16) ? 65535u : (1u << spare);
            edge_epoch_count = std::min<uint32_t>(
                edge_epoch_count, std::max<uint32_t>(2u, ne_cap));
        } else {
            edge_epoch_count = 2;
        }
    }

    // A5: deliver the per-edge next-ref epoch for ECG_GRASP_POPT. SSSP relaxes OUT-edges
    // reading dist[dest]; dest's property is next-referenced by dest's IN-neighbours, so
    // build epochs with push_out_edges=true (the transpose). Without this gem5 SSSP delivered
    // no epoch and ECG_GRASP_POPT degenerated to recency.
    const bool ecg_extract_on_env = gem5_ecg_extract_enabled();
    // FUSED ecg.plan.load: mirrors gem5 PR pr.cc — enabling GEM5_ENABLE_ECG_PLAN_LOAD alone implies
    // the extract delivery so the ReusePlan records get built even if GEM5_ENABLE_ECG_EXTRACT was
    // left unset. ecg.flow.load is the request-bound FlowThrough variant of the same
    // fused load (static no-allocate primitive, not an adaptive policy) and implies the
    // same delivery.
    const bool ecg_plan_load_on = gem5_ecg_plan_load_enabled();
    const bool ecg_flow_load_on = gem5_ecg_flow_load_enabled();
    const bool ecg_bind_iload_on =
        gem5_ecg_pload_enabled() && ecg_reuse_plan_depth == 2;
    const bool ecg_bind_computed_address_on =
        ecg_bind_iload_on && gem5_ecg_bind_computed_address_enabled();
    const bool ecg_extract_on =
        ecg_extract_on_env || ecg_plan_load_on || ecg_flow_load_on ||
        ecg_bind_iload_on;
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_reuse_plan_depth != 2) {
        ecg_reuse_plan::buildInEdgeEpochs(g, static_cast<uint32_t>(kNumVtxPerLine),
                                     edge_epoch_count, /*linemin=*/true,
                                     out_edge_epochs, /*push_out_edges=*/true);
    }
    std::vector<uint64_t> pair_off;
    pvector<uint64_t> pair_flat;
    pvector<uint32_t> pair_sidecars;
    pvector<uint64_t> pair_compact;
    bool pair_ok = false;
    bool compact_pair_ok = false;
    // Width and structure come from the shared metadata definition, the same
    // header cache_sim and the other backends use, so no simulator can
    // compute a record width of its own.
    {
        auto ecg_meta = ::ecg_metadata::configure(
            static_cast<uint64_t>(g.num_nodes()), edge_epoch_count);
        // No compact path here yet: this kernel builds the 64-bit two-epoch ReusePlan
        // record, so it streams 8 bytes per edge whatever the budget computes.
        // Declaring it keeps the receipt honest; only gem5 PR has the compact
        // 32-bit record so far.
        if (ecg_reuse_plan_depth == 2)
            ::ecg_metadata::declareContainerBytes(ecg_meta, 8);
        ::ecg_metadata::announce(ecg_meta, "gem5-sssp");
        ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "gem5-sssp");
    }
    if (ecg_extract_on && ecg_reuse_plan_depth == 2) {
        std::vector<uint64_t> pair_records;
        ecg_reuse_plan::buildInEdgeReusePlanRecords(
            g, static_cast<uint32_t>(kNumVtxPerLine),
            edge_epoch_count, /*linemin=*/true,
            pair_off, pair_records, /*push_out_edges=*/true);
        gem5_require_canonical_reuse_plan_offsets(
            g, pair_off, pair_records.size(),
            /*push_out_edges=*/true, "sssp");
        pair_flat = pvector<uint64_t>(
            pair_records.size(), uint64_t(0), 4096);
        std::copy(pair_records.begin(), pair_records.end(), pair_flat.begin());
        pair_sidecars = pvector<uint32_t>(
            pair_records.size(), uint32_t(0), 4096);
        for (size_t i = 0; i < pair_records.size(); ++i) {
            pair_sidecars[i] = ecg_reuse_plan::packWeightedReusePlanSidecar(
                ecg_reuse_plan::extractReusePlanTier(pair_records[i]),
                ecg_reuse_plan::extractReusePlanFirst(pair_records[i]),
                ecg_reuse_plan::extractReusePlanSecond(pair_records[i]));
        }
        pair_compact = pvector<uint64_t>(
            pair_records.size(), uint64_t(0), 4096);
        compact_pair_ok =
            static_cast<uint64_t>(g.num_nodes()) <=
                ecg_reuse_plan::kCompactWeightedMaxVertices;
        for (NodeID src = 0; compact_pair_ok && src < g.num_nodes(); ++src) {
            uint64_t pos = pair_off[src];
            for (WNode edge : g.out_neigh(src)) {
                if (!ecg_reuse_plan::canPackCompactWeightedEdge(
                        g.num_nodes(), static_cast<uint32_t>(edge.v),
                        static_cast<int64_t>(edge.w))) {
                    compact_pair_ok = false;
                    break;
                }
                const uint64_t pair = pair_flat[pos];
                pair_compact[pos] =
                    ecg_reuse_plan::packCompactWeightedReusePlanRecord(
                        static_cast<uint32_t>(edge.v),
                        static_cast<uint32_t>(edge.w),
                        ecg_reuse_plan::extractReusePlanTier(pair),
                        ecg_reuse_plan::extractReusePlanFirst(pair),
                        ecg_reuse_plan::extractReusePlanSecond(pair));
                ++pos;
            }
        }
        if (!compact_pair_ok) pair_compact.clear();
        pair_ok = true;
    }
    const char* reuse_plan_validate_env = std::getenv("ECG_REUSE_PLAN_VALIDATE");
    if (pair_ok && reuse_plan_validate_env && reuse_plan_validate_env[0] &&
        std::strcmp(reuse_plan_validate_env, "0") != 0 &&
        (!ecg_reuse_plan::validateWeightedReusePlanRecords(
             g, pair_off, pair_flat) ||
         !ecg_reuse_plan::validateWeightedReusePlanSidecars(
             pair_off, pair_flat, pair_sidecars) ||
         (compact_pair_ok &&
          !ecg_reuse_plan::validateCompactWeightedReusePlanRecords(
              g, pair_off, pair_flat, pair_compact)))) {
        std::fprintf(stderr, "gem5 SSSP ReusePlan record validation failed\n");
        std::abort();
    }
    const bool compact_structural_substitute =
        compact_pair_ok && ecg_bind_iload_on &&
        gem5_env_int_clamped("GEM5_ECG_PFX_LOOKAHEAD", 4, 0, 64) == 0;
    gem5_export_context(regions, 1, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions, edge_epoch_count,
                        compact_structural_substitute
                            ? reinterpret_cast<uint64_t>(pair_compact.data())
                            : pair_ok && !pair_sidecars.empty()
                                ? reinterpret_cast<uint64_t>(
                                    pair_sidecars.data())
                                : 0,
                        compact_structural_substitute
                            ? pair_compact.size() * sizeof(uint64_t)
                            : pair_ok
                                ? pair_sidecars.size() * sizeof(uint32_t) : 0,
                        nullptr, 0, nullptr, 0, nullptr, 0,
                        compact_structural_substitute
                            ? reinterpret_cast<uint64_t>(pair_compact.data()) : 0,
                        compact_structural_substitute
                            ? pair_compact.size() * sizeof(uint64_t) : 0,
                        compact_structural_substitute
                            ? "packed-substitute" : nullptr);
    // The fused ecg.load EVICT op reads dist[dest] AND delivers dest's epoch in one custom-0
    // op (RISC-V); gated on GEM5_ENABLE_ECG_PLOAD. X86 falls back to a plain indexed load
    // (no delivery -> cache_sim is authoritative there).
    const bool ecg_load_evict_on = gem5_ecg_pload_enabled() && ecg_extract_on;
    const int  ecg_evict_wc = ecg_mode6::ecgEvictWidthClass(g.num_nodes());
    if (ecg_load_evict_on)
        fprintf(stderr, "[ECG_PLOAD] SSSP fused ecg.load EVICT delivery ACTIVE\n");
    if (pair_ok) {
        fprintf(stderr,
                compact_pair_ok && ecg_bind_iload_on
                    ? (ecg_bind_computed_address_on
                        ? "[ECG_REUSE_BIND_LOAD_CW24] SSSP compact computed-address load ACTIVE\n"
                        : "[ECG_REUSE_BIND_ILOAD_CW24] SSSP compact indexed load ACTIVE\n")
                    : ecg_flow_load_on && ecg_bind_iload_on
                    ? (ecg_bind_computed_address_on
                        ? "[ECG_REUSE_BIND_LOAD] SSSP computed-address computed-address load "
                          "+ FlowThrough 4B sidecar ACTIVE\n"
                        : "[ECG_REUSE_BIND_ILOAD] SSSP fused indexed computed-address load "
                          "+ FlowThrough 4B sidecar ACTIVE\n")
                    : ecg_bind_iload_on
                        ? (ecg_bind_computed_address_on
                            ? "[ECG_REUSE_BIND_LOAD] SSSP computed-address computed-address load ACTIVE\n"
                            : "[ECG_REUSE_BIND_ILOAD] SSSP fused indexed computed-address load ACTIVE\n")
                    : ecg_flow_load_on
                        ? "[ECG_FLOW_WLOAD] SSSP request-bound 4B sidecar ACTIVE\n"
                    : ecg_plan_load_on
                        ? "[ECG_PLAN_WLOAD] SSSP fused 4B sidecar ACTIVE\n"
                        : "[ECG_PACKED8_REUSE_PLAN] SSSP two-epoch ReusePlan packed record path ACTIVE\n");
    }

    if (ecg_reuse_plan_depth != 2) {
        constexpr int numVtxPerLine = 64 / sizeof(WeightT);
        constexpr int numEpochs = 256;
        static pvector<uint8_t> popt_matrix;
        // SSSP relaxes OUT-edges reading dist[dest]; the next-ref of dist[v] is over v's
        // IN-neighbours, so the reref matrix is the graph TRANSPOSE (CSC/in_neigh,
        // traverseCSR=false) — matching cache_sim's natural_csr=false. Default true=out_neigh
        // is only correct for PR's in-pull. Undirected forces true internally (do-no-harm).
        makeOffsetMatrix(g, popt_matrix, numVtxPerLine, numEpochs, /*traverseCSR=*/false);
        int numCacheLines = (g.num_nodes() + numVtxPerLine - 1) / numVtxPerLine;
        gem5_export_popt_matrix(popt_matrix.data(), numCacheLines,
                                numEpochs, g.num_nodes());
    }

    Gem5EcgEpochQuantizer epoch_quantizer;
    if (gem5_ecg_epoch_csr_enabled())
        epoch_quantizer.reset(g.num_nodes(), edge_epoch_count);

    GEM5_ECG_BEGIN_CONTEXT();
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);

    pvector<NodeID> frontier(g.num_edges_directed());
    size_t shared_indexes[2] = {0, kMaxBin};
    size_t frontier_tails[2] = {1, 0};
    frontier[0] = source;

    #pragma omp parallel
    {
        vector<vector<NodeID>> local_bins(0);
        size_t iter = 0;
        while (shared_indexes[iter & 1] != kMaxBin) {
            size_t &curr_bin_index = shared_indexes[iter & 1];
            size_t &next_bin_index = shared_indexes[(iter + 1) & 1];
            size_t &curr_frontier_tail = frontier_tails[iter & 1];
            size_t &next_frontier_tail = frontier_tails[(iter + 1) & 1];

            #pragma omp for nowait schedule(dynamic, 64)
            for (size_t i = 0; i < curr_frontier_tail; i++) {
                NodeID u = frontier[i];
                if (dist[u] >= delta * static_cast<WeightT>(curr_bin_index))
                    RelaxEdges_Gem5(g, u, delta, dist, local_bins, &out_edge_epochs,
                                    pair_ok ? &pair_flat : nullptr,
                                    pair_ok ? &pair_sidecars : nullptr,
                                    compact_pair_ok ? &pair_compact : nullptr,
                                    ecg_load_evict_on, ecg_evict_wc, edge_epoch_count,
                                    ecg_plan_load_on, ecg_flow_load_on,
                                    ecg_bind_iload_on, compact_pair_ok,
                                    epoch_quantizer);
            }

            while (curr_bin_index < local_bins.size() &&
                   !local_bins[curr_bin_index].empty() &&
                   local_bins[curr_bin_index].size() < kBinSizeThreshold) {
                vector<NodeID> curr_bin_copy = local_bins[curr_bin_index];
                local_bins[curr_bin_index].resize(0);
                for (NodeID u : curr_bin_copy)
                    RelaxEdges_Gem5(g, u, delta, dist, local_bins, &out_edge_epochs,
                                    pair_ok ? &pair_flat : nullptr,
                                    pair_ok ? &pair_sidecars : nullptr,
                                    compact_pair_ok ? &pair_compact : nullptr,
                                    ecg_load_evict_on, ecg_evict_wc, edge_epoch_count,
                                    ecg_plan_load_on, ecg_flow_load_on,
                                    ecg_bind_iload_on, compact_pair_ok,
                                    epoch_quantizer);
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

    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    gem5_report_semantic_result(
        "sssp", dist.data(), static_cast<size_t>(dist.size()));
    GEM5_DUMP_STATS();
    GEM5_ECG_END_CONTEXT();
    return dist;
}

void PrintSSSPStats(const WGraph &g, const pvector<WeightT> &dist) {
    int reachable = 0;
    for (NodeID n = 0; n < g.num_nodes(); n++)
        if (dist[n] < kDistInf) reachable++;
    cout << "Reachable: " << reachable << "/" << static_cast<int>(g.num_nodes()) << endl;
}

bool SSSPVerifier(const WGraph &g, NodeID source, const pvector<WeightT> &dist) {
    pvector<WeightT> oracle(g.num_nodes(), kDistInf);
    oracle[source] = 0;
    typedef pair<WeightT, NodeID> WN;
    priority_queue<WN, vector<WN>, greater<WN>> pq;
    pq.push({0, source});
    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (d > oracle[u]) continue;
        for (WNode wn : g.out_neigh(u)) {
            WeightT nd = oracle[u] + wn.w;
            if (nd < oracle[wn.v]) { oracle[wn.v] = nd; pq.push({nd, wn.v}); }
        }
    }
    for (NodeID n = 0; n < g.num_nodes(); n++)
        if (dist[n] != oracle[n]) return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLDelta<WeightT> cli(argc, argv, "sssp-gem5");
    if (!cli.ParseArgs()) return -1;
    omp_set_num_threads(1);
    {
        WeightedBuilder b(cli);
        WGraph g = b.MakeGraph();
        SourcePicker<WGraph> sp(g, cli.start_vertex());
        // Separate verifier picker seeded identically to sp (matches cache_sim/canonical)
        // so the kernel and verifier draw the SAME source — without it the single picker
        // hands the verifier a different source and verification FAILs unless -r is set.
        SourcePicker<WGraph> vsp(g, cli.start_vertex());

        auto SSSPBound = [&sp, &cli](const WGraph &g) {
            return DeltaStep_Gem5(g, sp.PickNext(), cli.delta());
        };
        auto PrintBound = [](const WGraph &g, const pvector<WeightT> &d) {
            PrintSSSPStats(g, d);
        };
        auto VerifyBound = [&vsp](const WGraph &g, const pvector<WeightT> &d) {
            return SSSPVerifier(g, vsp.PickNext(), d);
        };
        BenchmarkKernel(cli, g, SSSPBound, PrintBound, VerifyBound);
    }
    return 0;
}
