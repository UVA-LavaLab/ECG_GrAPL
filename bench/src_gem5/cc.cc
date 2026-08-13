// ============================================================================
// Connected Components (Afforest) for gem5 SE-mode simulation
// ============================================================================
// Single-threaded Afforest (subgraph sampling) for gem5 SE mode.
// ============================================================================

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "ecg_epoch_builder.h"
#include "ecg_mode6_builder.h"

#include "gem5_sim/gem5_harness.h"
#include "ecg_metadata.h"

using namespace std;

void Link(NodeID u, NodeID v, pvector<NodeID>& comp) {
    NodeID p1 = comp[u], p2 = comp[v];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        if (comp[high] == high) { comp[high] = low; break; }
        NodeID p_high = comp[high];
        p1 = comp[p_high];
        p2 = comp[low];
    }
}

void LinkLoaded(NodeID u, NodeID v, NodeID p2, pvector<NodeID>& comp) {
    NodeID p1 = comp[u];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        if (comp[high] == high) { comp[high] = low; break; }
        NodeID p_high = comp[high];
        p1 = comp[p_high];
        p2 = comp[low];
    }
}

void Compress(const Graph &g, pvector<NodeID>& comp) {
    for (NodeID n = 0; n < g.num_nodes(); n++)
        while (comp[n] != comp[comp[n]])
            comp[n] = comp[comp[n]];
}

pvector<NodeID> Afforest_Gem5(const Graph &g, int32_t neighbor_rounds = 2) {
    constexpr size_t kPropAlign = 4096;  // page-align hot property array (see pr.cc)
    pvector<NodeID> comp(g.num_nodes(), NodeID(0), kPropAlign);
    for (NodeID n = 0; n < g.num_nodes(); n++) comp[n] = n;

    gem5_report_region("comp", comp.data(), g.num_nodes(), sizeof(NodeID));

    Gem5PropertyRegion regions[1] = {
        {"comp", reinterpret_cast<uint64_t>(comp.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(g.num_nodes()), sizeof(NodeID)},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);

    // Per-edge next-ref epoch budget keyed on comp (int32). CC traverses
    // OUT-edges reading comp[dest]; Schedule-2 uses its fixed 8-byte pair
    // record and bypasses the legacy 32-bit single-epoch cap.
    constexpr int kNumVtxPerLine = 64 / sizeof(NodeID);
    const int ecg_sched_k =
        gem5_env_int_clamped("ECG_EDGE_MASK_SCHED", 0, 0, 4);
    uint32_t requested_epoch_count = static_cast<uint32_t>(
        gem5_env_int_clamped("ECG_EDGE_MASK_EPOCHS", 65535, 2, 65535));
    if (ecg_sched_k == 2)
        requested_epoch_count =
            ecg_epoch::normalizeK2EpochCount(requested_epoch_count);
    uint8_t edge_id_bits = 1;
    while ((1ULL << edge_id_bits) < static_cast<uint64_t>(g.num_nodes())) edge_id_bits++;
    uint32_t edge_epoch_count = requested_epoch_count;
    if (ecg_sched_k != 2) {
        if (edge_id_bits < 32) {
            uint32_t spare = 32u - edge_id_bits;
            uint32_t ne_cap = (spare >= 16) ? 65535u : (1u << spare);
            edge_epoch_count = std::min<uint32_t>(
                edge_epoch_count, std::max<uint32_t>(2u, ne_cap));
        } else {
            edge_epoch_count = 2;
        }
    }

    // A5: deliver comp[dest]'s next-ref epoch for ECG_GRASP_POPT via the fused ecg.load EVICT
    // (RISC-V); gated on GEM5_ENABLE_ECG_PLOAD. X86 falls back to a plain indexed load. The
    // ecg.load warms+stamps comp[v] before Link re-reads it (comp[] is the irregular per-neighbour
    // property; the union-find pointer-chasing reads stay plain).
    bool ecg_extract_on = gem5_ecg_extract_enabled();
    // Schedule-2 loads the packed record, then carries its K2 mask on the exact
    // comp[dest] request. StreamShield remains on the record request.
    const bool ecg_load2_on = gem5_ecg_load2_enabled();
    const bool ecg_stream_load2_on = gem5_ecg_stream_load2_enabled();
    const bool ecg_k2_pload_on =
        gem5_ecg_pload_enabled() && ecg_sched_k == 2;
    const bool ecg_k2_mask_only_on =
        ecg_k2_pload_on && gem5_ecg_k2_mask_only_enabled();
    if (ecg_load2_on || ecg_stream_load2_on || ecg_k2_pload_on)
        ecg_extract_on = true;
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_sched_k != 2) {
        ecg_epoch::buildInEdgeEpochs(g, static_cast<uint32_t>(kNumVtxPerLine),
                                     edge_epoch_count, /*linemin=*/true,
                                     out_edge_epochs, /*push_out_edges=*/true);
    }
    std::vector<uint64_t> pair_off;
    pvector<uint64_t> pair_flat;
    bool pair_ok = false;
    // Width and structure come from the shared metadata definition, the same
    // header cache_sim and the other backends use, so no simulator can
    // compute a record width of its own.
    {
        auto ecg_meta = ::ecg_metadata::configure(
            static_cast<uint64_t>(g.num_nodes()), edge_epoch_count);
        // No compact path here yet: this kernel builds the 64-bit Schedule-2
        // record, so it streams 8 bytes per edge whatever the budget computes.
        // Declaring it keeps the receipt honest; only gem5 PR has the compact
        // 32-bit record so far.
        if (ecg_sched_k == 2)
            ::ecg_metadata::declareContainerBytes(ecg_meta, 8);
        ::ecg_metadata::announce(ecg_meta, "gem5-cc");
        ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "gem5-cc");
    }
    if (ecg_extract_on && ecg_sched_k == 2) {
        std::vector<uint64_t> pair_records;
        ecg_epoch::buildInEdgeEpochPairRecords(
            g, static_cast<uint32_t>(kNumVtxPerLine),
            edge_epoch_count, /*linemin=*/true,
            pair_off, pair_records, /*push_out_edges=*/true);
        pair_flat = pvector<uint64_t>(
            pair_records.size(), uint64_t(0), 4096);
        std::copy(pair_records.begin(), pair_records.end(), pair_flat.begin());
        pair_ok = true;
    }
    gem5_export_context(regions, 1, g, GEM5_SIDEBAND_PATH,
                        edge_regions, num_edge_regions, edge_epoch_count);
    const bool ecg_load_evict_on =
        gem5_ecg_pload_enabled() && ecg_extract_on && ecg_sched_k != 2;
    const int  ecg_evict_wc = ecg_mode6::ecgEvictWidthClass(g.num_nodes());
    auto load_delivered_comp = [&](NodeID u, size_t edge_pos, NodeID v,
                                   NodeID& delivered) {
        if (!ecg_load_evict_on ||
            static_cast<size_t>(u) >= out_edge_epochs.size()) {
            return false;
        }
        const auto& eps = out_edge_epochs[u];
        uint16_t epoch = (edge_pos < eps.size()) ? eps[edge_pos]
            : static_cast<uint16_t>(edge_epoch_count - 1);
        uint32_t bits = gem5_ecg_load_evict(
            comp.data(),
            ecg_mode6::packEvict(
                static_cast<uint32_t>(v), epoch, ecg_evict_wc),
            ecg_evict_wc);
        std::memcpy(&delivered, &bits, sizeof(NodeID));
        return true;
    };
    if (ecg_load_evict_on)
        fprintf(stderr, "[ECG_PLOAD] CC fused ecg.load EVICT delivery (comp) ACTIVE\n");
    if (pair_ok) {
        fprintf(stderr,
                ecg_stream_load2_on && ecg_k2_pload_on
                    ? (ecg_k2_mask_only_on
                        ? "[ECG_K2_MLOAD] CC computed-address masked load "
                          "+ StreamShield record load ACTIVE\n"
                        : "[ECG_K2_ILOAD] CC fused indexed masked load "
                          "+ StreamShield record load ACTIVE\n")
                    : ecg_k2_pload_on
                        ? (ecg_k2_mask_only_on
                            ? "[ECG_K2_MLOAD] CC computed-address masked load ACTIVE\n"
                            : "[ECG_K2_ILOAD] CC fused indexed masked load ACTIVE\n")
                    : ecg_stream_load2_on
                        ? "[ECG_STREAM_LOAD2] CC request-bound StreamShield+K2 ACTIVE\n"
                    : ecg_load2_on
                        ? "[ECG_LOAD2] CC fused K2 record load ACTIVE\n"
                        : "[ECG_PACKED8_K2] CC Schedule-2 packed record path ACTIVE\n");
    }

    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    GEM5_ECG_BEGIN_CONTEXT();

    // Phase 1: sparse sampling
    for (int32_t r = 0; r < neighbor_rounds; r++) {
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            GEM5_SET_VERTEX_EPOCH(
                u, g.num_nodes(), edge_epoch_count);
            if (pair_ok &&
                static_cast<size_t>(u + 1) < pair_off.size() &&
                pair_off[u] + static_cast<uint64_t>(r) < pair_off[u + 1]) {
                const uint64_t pos = pair_off[u] + static_cast<uint64_t>(r);
                const uint64_t record = ecg_stream_load2_on
                    ? gem5_ecg_stream_load2_instruction(&pair_flat[pos])
                    : ecg_load2_on
                        ? gem5_ecg_load2_instruction(&pair_flat[pos])
                        : pair_flat[pos];
                const NodeID v = static_cast<NodeID>(
                    ecg_epoch::extractEpochPairDest(record));
                NodeID delivered_comp;
                if (ecg_k2_pload_on) {
                    if (ecg_k2_mask_only_on) {
                        delivered_comp = static_cast<NodeID>(
                            gem5_ecg_mload_k2_s32(&comp[v], record));
                    } else {
                        const uint32_t bits =
                            gem5_ecg_load_k2(comp.data(), record);
                        std::memcpy(
                            &delivered_comp, &bits, sizeof(NodeID));
                    }
                } else {
                    if (!ecg_load2_on)
                        GEM5_ECG_EXTRACT2(record);
                    delivered_comp = comp[v];
                    GEM5_ECG_CLEAR_EXTRACT2_HINT();
                }
                LinkLoaded(u, v, delivered_comp, comp);
            } else if (!pair_ok) {
                auto it = g.out_neigh(u).begin();
                for (int32_t i = 0;
                     i < r && it != g.out_neigh(u).end(); ++i, ++it) {}
                if (it != g.out_neigh(u).end()) {
                    NodeID delivered_comp = 0;
                    if (load_delivered_comp(
                            u, static_cast<size_t>(r), *it,
                            delivered_comp)) {
                        LinkLoaded(u, *it, delivered_comp, comp);
                    } else {
                        Link(u, *it, comp);
                    }
                }
            }
        }
        Compress(g, comp);
    }

    // Find largest component
    unordered_map<NodeID, int64_t> count;
    for (NodeID n = 0; n < g.num_nodes(); n++) count[comp[n]]++;
    NodeID largest = max_element(count.begin(), count.end(),
        [](auto &a, auto &b){ return a.second < b.second; })->first;

    // Phase 2: full edge traversal skipping largest
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        GEM5_SET_VERTEX_EPOCH(
            u, g.num_nodes(), edge_epoch_count);
        if (comp[u] == largest) continue;
        if (pair_ok && static_cast<size_t>(u + 1) < pair_off.size()) {
            for (uint64_t pos = pair_off[u]; pos < pair_off[u + 1]; ++pos) {
                const uint64_t record = ecg_stream_load2_on
                    ? gem5_ecg_stream_load2_instruction(&pair_flat[pos])
                    : ecg_load2_on
                        ? gem5_ecg_load2_instruction(&pair_flat[pos])
                        : pair_flat[pos];
                const NodeID v = static_cast<NodeID>(
                    ecg_epoch::extractEpochPairDest(record));
                NodeID delivered_comp;
                if (ecg_k2_pload_on) {
                    if (ecg_k2_mask_only_on) {
                        delivered_comp = static_cast<NodeID>(
                            gem5_ecg_mload_k2_s32(&comp[v], record));
                    } else {
                        const uint32_t bits =
                            gem5_ecg_load_k2(comp.data(), record);
                        std::memcpy(
                            &delivered_comp, &bits, sizeof(NodeID));
                    }
                } else {
                    if (!ecg_load2_on)
                        GEM5_ECG_EXTRACT2(record);
                    delivered_comp = comp[v];
                    GEM5_ECG_CLEAR_EXTRACT2_HINT();
                }
                LinkLoaded(u, v, delivered_comp, comp);
            }
        } else {
            size_t edge_pos = 0;
            for (NodeID v : g.out_neigh(u)) {
                NodeID delivered_comp = 0;
                const bool delivered = load_delivered_comp(
                    u, edge_pos, v, delivered_comp);
                ++edge_pos;
                if (delivered) {
                    LinkLoaded(u, v, delivered_comp, comp);
                } else {
                    Link(u, v, comp);
                }
            }
        }
    }
    Compress(g, comp);

    GEM5_ECG_END_CONTEXT();
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    return comp;
}

void PrintCompStats(const Graph &g, const pvector<NodeID> &comp) {
    unordered_map<NodeID, int64_t> count;
    for (NodeID n = 0; n < g.num_nodes(); n++) count[comp[n]]++;
    cout << "Components: " << count.size() << endl;
}

bool CCVerifier(const Graph &g, const pvector<NodeID> &comp) {
    // Check: all connected vertices have same component
    for (NodeID u = 0; u < g.num_nodes(); u++)
        for (NodeID v : g.out_neigh(u))
            if (comp[u] != comp[v]) return false;
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "cc-gem5");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto CCBound = [](const Graph &g) { return Afforest_Gem5(g); };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &c) { PrintCompStats(g, c); };
    auto VerifyBound = [](const Graph &g, const pvector<NodeID> &c) { return CCVerifier(g, c); };
    BenchmarkKernel(cli, g, CCBound, PrintBound, VerifyBound);
    return 0;
}
