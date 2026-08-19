// ============================================================================
// PageRank (Pull, Gauss-Seidel) for gem5 SE-mode simulation
// ============================================================================
// Single-threaded PageRank for gem5. No in-process cache simulation —
// gem5's memory subsystem tracks all accesses automatically.
// The GRASP/ECG replacement policies learn property regions online.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"
#include "ecg_metadata.h"

// P-OPT rereference matrix builder (same as standalone cache_sim)
#include "graphbrew/partition/cagra/popt.h"

#include "gem5_sim/gem5_harness.h"

// ECG mode 6 (per-edge mask) builder — shared with cache_sim and Sniper.
#include "ecg_mode6_builder.h"
#include "ecg_reuse_plan_builder.h"
#include "ecg_reuse_plan_sidecar.h"

using namespace std;

typedef float ScoreT;
const float kDamp = 0.85;

pvector<ScoreT> PageRankPullGS_Gem5(const Graph &g, int max_iters,
                                     double epsilon = 0) {
    const ScoreT init_score = 1.0f / g.num_nodes();
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    // Page-align the property arrays so their cache set/line mapping is pinned
    // and does not drift with unrelated heap allocations (e.g. sideband path
    // strings). Without this, a few bytes of heap shift change conflict misses
    // in the tiny ROI caches and confound the per-policy comparison.
    constexpr size_t kDataAlign = 2 * 1024 * 1024;
    pvector<ScoreT> scores(g.num_nodes(), init_score, kDataAlign);
    pvector<ScoreT> outgoing_contrib(
        g.num_nodes(), ScoreT(0), kDataAlign);

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
            if (ne_cap < 2) ne_cap = 2;
            edge_epoch_count = std::min<uint32_t>(edge_epoch_count, ne_cap);
        } else {
            edge_epoch_count = 2;
        }
    }

    gem5_report_region("scores", scores.data(), g.num_nodes(), sizeof(ScoreT));
    gem5_report_region("contrib", outgoing_contrib.data(), g.num_nodes(), sizeof(ScoreT));

    // Export graph context sideband file for gem5 replacement policies.
    // This is the gem5 equivalent of cache_sim's registerPropertyArray() +
    // initTopology(). The SimObjects lazily load this on first eviction.
    Gem5PropertyRegion regions[2] = {
        {"scores",  reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
        {"contrib", reinterpret_cast<uint64_t>(outgoing_contrib.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(g.num_nodes()), sizeof(ScoreT), true},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2, true);
    // Build P-OPT rereference matrix (matching standalone src_sim/pr.cc)
    // Predicts future cache line accesses from graph structure.
    static pvector<uint8_t> popt_matrix;
    constexpr int kNumVtxPerLine = 64 / sizeof(ScoreT);  // 16 floats per line
    constexpr int kNumEpochs = 256;
    int popt_num_cache_lines = (g.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    if (ecg_reuse_plan_depth != 2) {
        makeOffsetMatrix(g, popt_matrix, kNumVtxPerLine, kNumEpochs);
        gem5_export_popt_matrix(popt_matrix.data(), popt_num_cache_lines,
                                kNumEpochs, g.num_nodes());
    }

    // === ECG Adaptive Prefetch ===
    // Compute bit layout dynamically from graph size (FatIDConfig logic).
    // For 32-bit edge IDs with N vertices needing B bits, we have (32-B)
    // spare bits for metadata. With 16K vertices (14 bits), we get 18 spare
    // bits → DBG=2, POPT=8, PFX=6 → 64-entry hot table.
    //
    // The hot table contains the top-K highest-degree vertices, sorted by
    // degree. Each neighbor access can prefetch one of these hub vertices
    // based on which hub is most likely to be needed next (and not already
    // in cache from a recent prefetch — dedup window prevents redundancy).
    
    // Compute spare bits (matching FatIDConfig::computeFromGraph)
    uint8_t id_bits = 1;
    while ((1ULL << id_bits) < (uint64_t)g.num_nodes()) id_bits++;
    uint8_t container_bits = (g.num_nodes() > (1LL << 30)) ? 64 : 32;
    uint8_t spare = container_bits - id_bits;
    if (spare < 2 && container_bits == 32) { container_bits = 64; spare = container_bits - id_bits; }

    // Compute prefetch bits (matching FatIDConfig allocation tiers)
    uint8_t pfx_bits = 0;
    if (spare >= 16)     pfx_bits = min((int)(spare - 10), 6);
    else if (spare >= 10) pfx_bits = min((int)(spare - 6), 4);
    else if (spare >= 6)  pfx_bits = spare - 4;
    // else: pfx_bits = 0
    
    int hot_table_size =
        (ecg_reuse_plan_depth == 2) ? 0 : (pfx_bits > 0 ? (1 << pfx_bits) : 0);
    hot_table_size = min(hot_table_size, (int)g.num_nodes());
    constexpr int PREFETCH_WINDOW = 16;  // Dedup window

    vector<NodeID> hot_table(hot_table_size);
    if (hot_table_size > 0) {
        // Build hot table: top-K by degree
        vector<pair<int64_t, NodeID>> deg_vtx(g.num_nodes());
        for (NodeID n = 0; n < g.num_nodes(); n++)
            deg_vtx[n] = {g.out_degree(n), n};
        partial_sort(deg_vtx.begin(),
                     deg_vtx.begin() + hot_table_size,
                     deg_vtx.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int i = 0; i < hot_table_size; i++)
            hot_table[i] = deg_vtx[i].second;
        
        printf("ECG adaptive prefetch: %d-bit container, %d-bit ID, %d spare, "
               "%d prefetch bits -> %d-entry hot table\n",
               container_bits, id_bits, spare, pfx_bits, hot_table_size);
        printf("  Top hubs: [%d(d=%ld)", hot_table[0], (long)g.out_degree(hot_table[0]));
        for (int i = 1; i < min(hot_table_size, 5); i++)
            printf(", %d(d=%ld)", hot_table[i], (long)g.out_degree(hot_table[i]));
        if (hot_table_size > 5) printf(", ... +%d more", hot_table_size - 5);
        printf("]\n");
    }
    
    // Build per-vertex prefetch index: for each vertex, what's its rank
    // in the hot table? This lets us quickly check if a neighbor is a hub.
    vector<int> hub_rank;
    if (hot_table_size > 0) hub_rank.assign(g.num_nodes(), -1);
    for (int i = 0; i < hot_table_size; i++)
        hub_rank[hot_table[i]] = i;

    // Check environment variable to enable/disable ECG_PFX hint emission.
    const char* ecg_prefetch_env = getenv("GEM5_ENABLE_ECG_PFX_HINTS");
    if (!ecg_prefetch_env) ecg_prefetch_env = getenv("ECG_PREFETCH");
    bool ecg_prefetch_enabled = ecg_prefetch_env && string(ecg_prefetch_env) != "0";
    int pfx_lookahead = gem5_env_int_clamped("GEM5_ECG_PFX_LOOKAHEAD", 4, 0, 64);

    // Path A (epoch-filtered next-K lookahead): prefetch the next-K in-neighbors,
    // each carrying its epoch via GEM5_ECG_PFX_TARGET_EPOCH. Mirrors cache_sim.
    int lean_pfx_k = gem5_env_int_clamped("ECG_EDGE_MASK_PREFETCH", 0, 0, 64);
    int pfx_epoch_filter = gem5_env_int_clamped("ECG_PREFETCH_EPOCH_FILTER", 0, 0, 2);
    int pfx_epoch_thresh_pct =
        gem5_env_int_clamped("ECG_PREFETCH_EPOCH_THRESH_PCT", 50, 0, 100);

    // === Mode 6: per-edge ECG mask ===
    // Build the per-edge mask array once before iteration begins. When
    // GEM5_ECG_PFX_MODE=6 the inner loop reads pre-encoded prefetch targets
    // from this array instead of computing them at runtime.
    // (Also accepts ECG_PREFETCH_MODE for compatibility with roi_matrix.py's
    // cache_sim env naming.)
    int ecg_pfx_mode = gem5_env_int_clamped("GEM5_ECG_PFX_MODE", -1, -1, 7);
    if (ecg_pfx_mode < 0) {
        ecg_pfx_mode = gem5_env_int_clamped("ECG_PREFETCH_MODE", 0, 0, 7);
    }
    int edge_mask_lookahead = gem5_env_int_clamped("GEM5_ECG_EDGE_MASK_LOOKAHEAD",
        gem5_env_int_clamped("ECG_EDGE_MASK_LOOKAHEAD", 8, 1, 64), 1, 64);
    vector<vector<uint64_t>> in_edge_masks_by_src;
    vector<vector<uint16_t>> in_edge_epochs_by_src;
    // === Single-stream packed record (LEAN+PACK; matches cache_sim) ===
    // The scattered vector<vector<uint64_t>> mask above is a SEPARATE 8-byte
    // non-property stream that pollutes the LLC and displaces the property
    // (contrib) the epoch eviction is meant to protect — the root cause of gem5
    // ECG_GRASP_POPT scoring WORSE than LRU. cache_sim avoids this by packing the
    // epoch into the spare high bits of the 4-byte edge word and reading ONE
    // contiguous stream. Mirror that with a flat, CSR-ordered uint32 array
    // (dest | epoch<<id_bits): reading record r delivers BOTH the neighbor and
    // its next-ref epoch with the footprint of a single 4-byte CSR edge.
    pvector<uint32_t> in_edge_packed_flat;
    vector<uint64_t> packed_off;
    pvector<uint64_t> in_edge_pair_flat;
    pvector<uint32_t> in_edge_pair32_flat;
    bool pair32_ok = false;
    bool pair_sidecar_loaded = false;
    uint64_t pair_sidecar_graph_hash = 0;
    uint64_t pair_sidecar_payload_hash = 0;
    uint32_t pair32_id_bits = 1, pair32_epoch_bits = 1;
    bool use_compact_pair = false;
    vector<uint64_t> pair_off;
    uint32_t pack_id_bits = 1, pack_id_mask = 1;
    bool packed_ok = false;
    bool pair_ok = false;
    bool ecg_extract_enabled = gem5_ecg_extract_enabled();
    const bool ecg_flow_load_on =
        gem5_ecg_flow_load_enabled();
    const bool ecg_plan_load_on = gem5_ecg_plan_load_enabled();
    const bool ecg_bind_iload_on =
        gem5_ecg_pload_enabled() && ecg_reuse_plan_depth == 2;
    const bool ecg_bind_computed_address_on =
        ecg_bind_iload_on && gem5_ecg_bind_computed_address_enabled();
    if (ecg_flow_load_on || ecg_plan_load_on || ecg_bind_iload_on)
        ecg_extract_enabled = true;
    // The masked property-load family implies metadata construction. two-epoch ReusePlan
    // uses the request-bound ReusePlan mode; legacy record-load delivery remains available.
    bool ecg_load_enabled = gem5_ecg_load_enabled();
    if (ecg_load_enabled) ecg_extract_enabled = true;
    if ((ecg_prefetch_enabled || ecg_extract_enabled) && ecg_pfx_mode == 6) {
        // Width and structure come from the shared metadata definition for both
        // schedules, so gem5 cannot disagree with cache_sim about how wide a
        // record is or whether a packed one fits.
        {
            auto ecg_meta = ::ecg_metadata::configure(
                static_cast<uint64_t>(g.num_nodes()), edge_epoch_count);
            // The shared rule decides the width; the budget and container
            // must agree: a compact record is used only when the shared rule
            // asks for 4 bytes AND the fields actually fit. Letting gem5 decide
            // on feasibility alone made it stream 4 bytes while cache_sim
            // streamed 8 -- the same divergence as before, inverted.
            // Legacy stream/plan_load forms are 8-byte-only. The proposal's
            // explicit compact ReuseBind+FlowThrough path has its own 4-byte
            // record-load instruction and therefore remains compact.
            const bool compact_flowthrough_supported =
                gem5_ecg_compact_reuse_bind_flowthrough_enabled();
            const bool wide_only_transport =
                gem5_ecg_plan_load_enabled() ||
                (gem5_ecg_flow_load_enabled() &&
                 !compact_flowthrough_supported);
            if (compact_flowthrough_supported &&
                (ecg_meta.record_bytes != 4 ||
                 !ecg_reuse_plan::canPackReusePlan32(
                     static_cast<uint32_t>(g.num_nodes()),
                     edge_epoch_count))) {
                fprintf(stderr,
                    "[ECG-METADATA-FATAL] compact ReuseBind+FlowThrough "
                    "requested but the 32-bit record is infeasible "
                    "(vertices=%u epochs=%u record_bytes=%u)\n",
                    static_cast<unsigned>(g.num_nodes()),
                    edge_epoch_count,
                    static_cast<unsigned>(ecg_meta.record_bytes));
                std::abort();
            }
            use_compact_pair =
                ecg_reuse_plan_depth == 2 && ecg_meta.record_bytes == 4 &&
                !wide_only_transport &&
                ecg_reuse_plan::canPackReusePlan32(
                    static_cast<uint32_t>(g.num_nodes()), edge_epoch_count);
            if (ecg_reuse_plan_depth == 2 && ecg_meta.record_bytes == 4 &&
                wide_only_transport) {
                fprintf(stderr,
                    "[ECG-METADATA-NOTE] compact 4-byte record unavailable "
                    "for the selected legacy plan_load transport, so this cell "
                    "streams 8 bytes\n");
            }
            if (ecg_reuse_plan_depth == 2)
                ::ecg_metadata::declareContainerBytes(
                    ecg_meta, use_compact_pair ? 4 : 8);
            ::ecg_metadata::announce(ecg_meta, "gem5-pr");
            ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "gem5-pr");
        }
        if (ecg_reuse_plan_depth == 2) {
            const char* sidecar_path =
                std::getenv("GEM5_REUSE_PLAN_SIDECAR");
            const bool sidecar_required =
                std::getenv("GEM5_REUSE_PLAN_SIDECAR_REQUIRED") != nullptr;
            if (sidecar_required &&
                (!sidecar_path || !sidecar_path[0])) {
                fprintf(stderr,
                    "[ECG-METADATA-FATAL] ReusePlan sidecar is required "
                    "but GEM5_REUSE_PLAN_SIDECAR is empty\n");
                std::abort();
            }
            // Prefer the COMPACT 32-bit two-stamp record when the fields fit.
            // The 64-bit form always costs 8 bytes per edge and so doubles the
            // structural stream against a 4-byte CSR edge; the compact form
            // SUBSTITUTES for that edge, which is the width cache_sim models.
            std::vector<uint32_t> pair32;
            bool compact_ready = false;
            if (use_compact_pair && sidecar_path && sidecar_path[0]) {
                ecg_reuse_plan::ReusePlanSidecarHeader header;
                std::string error;
                compact_ready = ecg_reuse_plan::loadReusePlanSidecar(
                    sidecar_path, g, false, kNumVtxPerLine,
                    edge_epoch_count, true,
                    ecg_reuse_plan::configuredReuseHotFraction(),
                    pair_off, pair32, header, error);
                if (!compact_ready && sidecar_required) {
                    fprintf(stderr,
                        "[ECG-METADATA-FATAL] ReusePlan sidecar rejected: "
                        "%s\n", error.c_str());
                    std::abort();
                }
                if (compact_ready) {
                    pair_sidecar_loaded = true;
                    pair_sidecar_graph_hash = header.graph_hash;
                    pair_sidecar_payload_hash = header.payload_hash;
                }
            }
            if (use_compact_pair && !compact_ready) {
                compact_ready =
                    ecg_reuse_plan::buildInEdgeReusePlanRecords32(
                        g, kNumVtxPerLine, edge_epoch_count, true,
                        pair_off, pair32);
            }
            if (use_compact_pair && compact_ready) {
                in_edge_pair32_flat = pvector<uint32_t>(
                    pair32.size(), uint32_t(0), kDataAlign);
                std::copy(pair32.begin(), pair32.end(),
                          in_edge_pair32_flat.begin());
                pair32_ok = true;
                pair_ok = true;
                pair32_id_bits = ecg_reuse_plan::reusePlan32IdBits(
                    static_cast<uint32_t>(g.num_nodes()));
                pair32_epoch_bits =
                    ecg_reuse_plan::reusePlan32EpochBits(edge_epoch_count);
                printf("[gem5 ECG mode 6] two-epoch ReusePlan COMPACT record ON: "
                       "ne=%u records=%llu id_bits=%u epoch_bits=%u "
                       "(4-byte, substitutes for the CSR edge)\n",
                       edge_epoch_count,
                       (unsigned long long)in_edge_pair32_flat.size(),
                       ecg_reuse_plan::reusePlan32IdBits(
                           static_cast<uint32_t>(g.num_nodes())),
                       ecg_reuse_plan::reusePlan32EpochBits(edge_epoch_count));
            } else {
                std::vector<uint64_t> pair_records;
                bool wide_ready = false;
                if (sidecar_path && sidecar_path[0]) {
                    ecg_reuse_plan::ReusePlanSidecarHeader header;
                    std::string error;
                    wide_ready = ecg_reuse_plan::loadReusePlanSidecar(
                        sidecar_path, g, false, kNumVtxPerLine,
                        edge_epoch_count, true,
                        ecg_reuse_plan::configuredReuseHotFraction(),
                        pair_off, pair_records, header, error);
                    if (!wide_ready && sidecar_required) {
                        fprintf(stderr,
                            "[ECG-METADATA-FATAL] ReusePlan sidecar rejected: "
                            "%s\n", error.c_str());
                        std::abort();
                    }
                    if (wide_ready) {
                        pair_sidecar_loaded = true;
                        pair_sidecar_graph_hash = header.graph_hash;
                        pair_sidecar_payload_hash = header.payload_hash;
                    }
                }
                if (!wide_ready) {
                    ecg_reuse_plan::buildInEdgeReusePlanRecords(
                        g, kNumVtxPerLine, edge_epoch_count, true,
                        pair_off, pair_records);
                }
                in_edge_pair_flat = pvector<uint64_t>(
                    pair_records.size(), uint64_t(0), kDataAlign);
                std::copy(
                    pair_records.begin(), pair_records.end(),
                    in_edge_pair_flat.begin());
                pair_ok = true;
                printf("[gem5 ECG mode 6] two-epoch ReusePlan record ON: "
                       "ne=%u records=%llu "
                       "(8-byte dest32+tier2+epoch15+epoch15)\n",
                       edge_epoch_count,
                       (unsigned long long)in_edge_pair_flat.size());
            }
            if (pair_sidecar_loaded) {
                fprintf(stderr,
                    "[ReusePlan-SIDECAR sim=gem5 active=1 "
                    "record_bytes=%u records=%llu graph_hash=%llu "
                    "payload_hash=%llu]\n",
                    pair32_ok ? 4U : 8U,
                    (unsigned long long)(
                        pair32_ok ? in_edge_pair32_flat.size()
                                  : in_edge_pair_flat.size()),
                    (unsigned long long)pair_sidecar_graph_hash,
                    (unsigned long long)pair_sidecar_payload_hash);
            }
        } else {
        vector<uint8_t> avg_reref_by_line;
        ecg_mode6::computeAvgRerefByLine(popt_matrix.data(), popt_num_cache_lines,
                                         kNumEpochs, avg_reref_by_line);
        vector<uint8_t> tiers;
        ecg_mode6::computeDegreeTiers(g, tiers);
        ecg_mode6::buildInEdgeMasks(g, tiers, avg_reref_by_line,
                                    edge_mask_lookahead, kNumVtxPerLine,
                                    in_edge_masks_by_src, "gem5-PR");
        ecg_reuse_plan::buildInEdgeEpochs(g, kNumVtxPerLine, edge_epoch_count, true,
                                     in_edge_epochs_by_src);
        uint64_t pfx_total = 0, pfx_truncated = 0;
        for (size_t src = 0; src < in_edge_masks_by_src.size(); ++src) {
            auto& masks = in_edge_masks_by_src[src];
            const auto& epochs = in_edge_epochs_by_src[src];
            for (size_t i = 0; i < masks.size(); ++i) {
                uint64_t mask = masks[i];
                uint16_t epoch = (i < epochs.size()) ? epochs[i]
                    : static_cast<uint16_t>(edge_epoch_count - 1);
                // Transfer the POPT-best prefetch target (packed at bit 33 by
                // buildInEdgeMasks) into the packMaskEpoch target field at bit 49,
                // where the ecg.extract.wide ISA op and the prefetcher read it.
                uint32_t pfx_target = ecg_mode6::extractPrefetchTarget(mask);
                if (pfx_target != 0) {
                    pfx_total++;
                    // packMaskEpochWide carries a 24-bit prefetch target (<=16,777,215)
                    // by reclaiming the vestigial dbg(2)+popt(7) fields. Only
                    // graphs with > 2^24 vertices still truncate (then use cache_sim or
                    // the 16-byte record). The ecg.extract.wide ISA op decodes [40:64].
                    if (pfx_target > 0xFFFFFFu) pfx_truncated++;
                }
                masks[i] = ecg_mode6::packMaskEpochWide(
                    ecg_mode6::extractDest(mask),
                    epoch,
                    pfx_target);
            }
        }
        if (pfx_truncated > 0) {
            std::cerr << "[gem5 ECG_PFX WARNING] " << pfx_truncated << "/" << pfx_total
                      << " prefetch targets exceed the 24-bit ISA mask field (>16,777,215) and "
                         "are TRUNCATED to wrong vertices. The gem5 ECG_PFX ISA testbed is "
                         "valid only for graphs <=16,777,215 vertices; use cache_sim for "
                         "larger-graph prefetch evaluation (no field limit). Set "
                         "ECG_PFX_STRICT_TARGET=1 to abort instead.\n";
            if (std::getenv("ECG_PFX_STRICT_TARGET")) {
                std::cerr << "[gem5 ECG_PFX] ECG_PFX_STRICT_TARGET set -> aborting.\n";
                std::abort();
            }
        }
        printf("[gem5 ECG mode 6] lookahead=%d ne=%u (per-edge epoch mask path active)\n",
               edge_mask_lookahead, edge_epoch_count);

        // Build the flat, contiguous, CSR-ordered 4-byte packed record stream
        // when dest+epoch fit in 32 bits. This REPLACES the scattered 8-byte
        // mask reads in the demand path, eliminating the LLC-polluting second
        // stream (the gem5-vs-cache_sim divergence root cause).
        {
            uint32_t nn = static_cast<uint32_t>(g.num_nodes());
            while ((1u << pack_id_bits) < nn && pack_id_bits < 31) pack_id_bits++;
            pack_id_mask = (pack_id_bits >= 32) ? 0xFFFFFFFFu
                                                : ((1u << pack_id_bits) - 1);
            uint32_t epoch_bits = 1;
            while ((1u << epoch_bits) < edge_epoch_count && epoch_bits < 16) epoch_bits++;
            // Width and structure come from the shared metadata definition, the same
            // header cache_sim uses, so the three simulators cannot disagree
            // about how wide a record is or whether it fits.
            const auto ecg_meta = ::ecg_metadata::configure(
                static_cast<uint64_t>(nn), edge_epoch_count);
            if (ecg_meta.packed_fits &&
                ecg_meta.delivery == ::ecg_metadata::Delivery::PackedRecord) {
                packed_off.assign(static_cast<size_t>(nn) + 1, 0);
                for (uint32_t u = 0; u < nn; u++)
                    packed_off[u + 1] = packed_off[u] +
                        static_cast<uint64_t>(g.in_degree(u));
                in_edge_packed_flat = pvector<uint32_t>(
                    packed_off[nn], uint32_t(0), kDataAlign);
                for (uint32_t u = 0; u < nn; u++) {
                    const auto& eps = in_edge_epochs_by_src[u];
                    size_t i = 0;
                    for (auto v_raw : g.in_neigh(u)) {
                        uint32_t v = static_cast<uint32_t>(v_raw);
                        uint16_t ep = (i < eps.size()) ? eps[i]
                            : static_cast<uint16_t>(edge_epoch_count - 1);
                        in_edge_packed_flat[packed_off[u] + i] =
                            (v & pack_id_mask) |
                            (static_cast<uint32_t>(ep) << pack_id_bits);
                        i++;
                    }
                }
                packed_ok = true;
                printf("[gem5 ECG mode 6] single-stream packed record ON: "
                       "id_bits=%u epoch_bits=%u (4-byte contiguous, no separate "
                       "mask array)\n", pack_id_bits, epoch_bits);
            } else {
                // The shared rule says a packed record cannot substitute for the
                // edge here. A contiguous narrow sidecar is the correct
                // fallback; the historical scattered per-vertex mask array is
                // strictly worse and is what this branch used to take.
                printf("[gem5 ECG mode 6] packed record OFF: id_bits=%u + "
                       "epoch_bits=%u > 32; sidecar payload=%d bits "
                       "(was: scattered mask fallback)\n",
                       pack_id_bits, epoch_bits, ecg_meta.payload_bits);
            }
        }
        }
    }
    gem5_export_context(
        regions, 2, g, GEM5_SIDEBAND_PATH,
        edge_regions, num_edge_regions, edge_epoch_count,
        pair32_ok && !in_edge_pair32_flat.empty()
            ? reinterpret_cast<uint64_t>(in_edge_pair32_flat.data())
            : pair_ok && !in_edge_pair_flat.empty()
            ? reinterpret_cast<uint64_t>(in_edge_pair_flat.data())
            : (packed_ok && !in_edge_packed_flat.empty()
                ? reinterpret_cast<uint64_t>(in_edge_packed_flat.data()) : 0),
        pair32_ok ? in_edge_pair32_flat.size() * sizeof(uint32_t)
                : pair_ok ? in_edge_pair_flat.size() * sizeof(uint64_t)
                : (packed_ok
                    ? in_edge_packed_flat.size() * sizeof(uint32_t) : 0));

    for (NodeID n = 0; n < g.num_nodes(); n++)
        outgoing_contrib[n] = init_score / g.out_degree(n);
    volatile ScoreT* warm_scores = scores.data();
    volatile ScoreT* warm_contrib = outgoing_contrib.data();
    for (NodeID n = 0; n < g.num_nodes(); ++n) {
        warm_scores[n] = warm_scores[n];
        warm_contrib[n] = warm_contrib[n];
    }

    // Prefetch dedup window — tracks recently prefetched hub indices
    vector<NodeID> pfx_window(PREFETCH_WINDOW, -1);
    int pfx_window_pos = 0;
    const char* configured_prefetcher = std::getenv("GRAPHBREW_PREFETCHER");
    const bool packed_stream_compatible =
        !configured_prefetcher ||
        std::string(configured_prefetcher) == "none" ||
        std::string(configured_prefetcher) == "STRIDE";
    const bool packed_extract_only =
        ecg_extract_enabled && !ecg_prefetch_enabled &&
        ecg_pfx_mode == 6 && packed_ok && pack_id_bits <= 24 &&
        !ecg_load_enabled && !gem5_ecg_pload_enabled() &&
        packed_stream_compatible;
    const bool pair_extract_only =
        ecg_extract_enabled && !ecg_prefetch_enabled &&
        ecg_pfx_mode == 6 && pair_ok &&
        !ecg_load_enabled &&
        packed_stream_compatible;
    // The compact ISA path is only meaningful when a compact record was
    // actually built; it is opt-in so the software-decode arm stays measurable.
    const bool compact_isa_requested = gem5_ecg_compact_isa_enabled();
    const bool compact_isa_on =
        compact_isa_requested && pair_extract_only && pair32_ok &&
        !ecg_bind_iload_on && !ecg_flow_load_on && !ecg_plan_load_on;
    const uint32_t compact_fmt_word =
        gem5_ecg_compact_format_word(pair32_id_bits, pair32_epoch_bits);
    // Hoisted once: see gem5_ecg_extract2c_instruction_traced for why this must
    // not be tested per edge.
    const bool compact_isa_trace =
        compact_isa_on && gem5_ecg_reuse_plan_trace_enabled();
    // Same hoist for the software-widen and wide-record path, so that arm is
    // charged the same per-edge work as the compact-ISA arm.
    const bool pair_trace_on = gem5_ecg_reuse_plan_trace_enabled();
    const bool compact_fused_requested =
        gem5_ecg_compact_fused_enabled();
    const bool compact_reuse_bind_flowthrough_requested =
        gem5_ecg_compact_reuse_bind_flowthrough_enabled();
    const bool compact_reuse_bind_flowthrough_on =
        compact_reuse_bind_flowthrough_requested && pair_extract_only &&
        pair32_ok && ecg_flow_load_on && ecg_bind_iload_on &&
        ecg_bind_computed_address_on && !ecg_plan_load_on;
    const bool wide_reuse_bind_flowthrough_on =
        pair_extract_only && !pair32_ok && ecg_flow_load_on &&
        ecg_bind_iload_on && ecg_bind_computed_address_on && !ecg_plan_load_on;
    const bool compact_fused_on =
        compact_fused_requested && pair_extract_only && pair32_ok &&
        ecg_bind_iload_on && !ecg_bind_computed_address_on &&
        !ecg_flow_load_on && !ecg_plan_load_on;
    const bool compact_fused_trace =
        compact_fused_on && pair_trace_on;
    const bool compact_software_fused_on =
        pair_extract_only && pair32_ok && ecg_bind_iload_on &&
        !ecg_bind_computed_address_on && !compact_fused_on &&
        !ecg_flow_load_on && !ecg_plan_load_on;
    const bool wide_fused_on =
        pair_extract_only && !pair32_ok && ecg_bind_iload_on &&
        !ecg_bind_computed_address_on && !ecg_flow_load_on && !ecg_plan_load_on;
    if (compact_isa_requested && !compact_isa_on) {
        // Silently falling back to software decode would produce an arm that
        // reports the compact ISA while measuring the thing it replaces, which
        // is the exact failure mode that made four earlier width arms invalid.
        fprintf(stderr,
                "[ECG-METADATA-FATAL] GEM5_ECG_COMPACT_ISA=1 but the compact "
                "decode path is unavailable (pair_extract_only=%d pair32=%d "
                "reuse_bind_iload=%d flow_load=%d plan_load=%d). The fused masked-load "
                "deliveries carry the 64-bit record and have no 32-bit "
                "variant, so this cell would silently measure software "
                "decode.\n",
                (int)pair_extract_only, (int)pair32_ok, (int)ecg_bind_iload_on,
                (int)ecg_flow_load_on, (int)ecg_plan_load_on);
        std::abort();
    }
    if (compact_fused_requested && !compact_fused_on) {
        fprintf(stderr,
                "[ECG-METADATA-FATAL] GEM5_ECG_COMPACT_FUSED=1 but the "
                "compact fused load is unavailable (pair_extract_only=%d "
                "pair32=%d reuse_bind_iload=%d computed_address=%d flow_load=%d "
                "plan_load=%d). This arm requires a compact record and indexed "
                "request-bound ReusePlan delivery; aborting rather than silently "
                "widening in software.\n",
                (int)pair_extract_only, (int)pair32_ok,
                (int)ecg_bind_iload_on, (int)ecg_bind_computed_address_on,
                (int)ecg_flow_load_on, (int)ecg_plan_load_on);
        std::abort();
    }
    if (compact_reuse_bind_flowthrough_requested &&
        !compact_reuse_bind_flowthrough_on) {
        fprintf(stderr,
                "[ECG-METADATA-FATAL] GEM5_ECG_COMPACT_REUSE_BIND_FLOW=1 but the "
                "proposal path is unavailable (pair_extract_only=%d "
                "pair32=%d flow_load=%d reuse_bind_iload=%d computed_address=%d "
                "plan_load=%d). This path requires a 4-byte FlowThrough record "
                "load followed by a computed-address ReuseBind property load.\n",
                (int)pair_extract_only, (int)pair32_ok,
                (int)ecg_flow_load_on, (int)ecg_bind_iload_on,
                (int)ecg_bind_computed_address_on, (int)ecg_plan_load_on);
        std::abort();
    }
    if (pair32_ok) {
        gem5_ecg_write_record_format_csr(
            pair32_id_bits, pair32_epoch_bits);
    }
    if (compact_isa_on)
        fprintf(stderr,
                "[ECG_EXTRACT2C] PR compact record decoded in the ISA "
                "(id_bits=%u epoch_bits=%u)\n",
                pair32_id_bits, pair32_epoch_bits);
    if (compact_reuse_bind_flowthrough_on)
        fprintf(stderr,
                "[ECG_REUSE_BIND_LOAD_C_FLOW] PR compact FlowThrough record load "
                "+ computed-address computed-address property load ACTIVE "
                "(id_bits=%u epoch_bits=%u)\n",
                pair32_id_bits, pair32_epoch_bits);
    if (pair_extract_only) {
        fprintf(stderr,
            ecg_flow_load_on
            ? (ecg_bind_iload_on
                ? (ecg_bind_computed_address_on
                    ? "[ECG_REUSE_BIND_LOAD] PR computed-address computed-address load "
                      "+ FlowThrough record load ACTIVE\n"
                    : "[ECG_REUSE_BIND_ILOAD] PR fused indexed computed-address load "
                      "+ FlowThrough record load ACTIVE\n")
                : "[ECG_FLOW_LOAD] PR request-bound FlowThrough+ReusePlan ACTIVE\n")
            : ecg_bind_iload_on
                ? (compact_fused_on
                    ? "[ECG_REUSE_BIND_ILOAD_C] PR fused compact indexed property "
                      "load ACTIVE\n"
                    : ecg_bind_computed_address_on
                    ? "[ECG_REUSE_BIND_LOAD] PR computed-address computed-address load ACTIVE\n"
                    : "[ECG_REUSE_BIND_ILOAD] PR fused indexed computed-address load ACTIVE\n")
            : ecg_plan_load_on
                ? "[ECG_PLAN_LOAD] PR fused ReusePlan record load ACTIVE\n"
                : pair32_ok
                ? "[ECG_PACKED4_REUSE_PLAN] PR two-epoch ReusePlan compact packed record path "
                  "ACTIVE\n"
                : "[ECG_PACKED8_REUSE_PLAN] PR two-epoch ReusePlan packed record path ACTIVE\n");
    } else if (packed_extract_only) {
        fprintf(stderr,
                "[ECG_PACKED4] PR eviction-only packed record fast path ACTIVE\n");
    }

    // Configuration checks, context allocation and the record-format CSR are
    // setup, not graph work. Keep them outside the measured ROI; an earlier
    // version claimed this while resetting stats first.
    GEM5_ECG_BEGIN_CONTEXT();
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);

    int executed_iters = 0;
    for (int iter = 0; iter < max_iters; iter++) {
        ++executed_iters;
        double error = 0;
        Gem5EcgMonotonicEpochCursor epoch_cursor;
        if (gem5_ecg_epoch_csr_enabled()) {
            epoch_cursor.reset(g.num_nodes(), edge_epoch_count);
        }
        for (NodeID u = 0; u < g.num_nodes(); u++) {
            GEM5_SET_MONOTONIC_VERTEX_EPOCH(epoch_cursor, u);
            ScoreT incoming_total = 0;

            if (pair_extract_only &&
                static_cast<size_t>(u + 1) < pair_off.size()) {
                const uint64_t begin = pair_off[u];
                const uint64_t end = pair_off[u + 1];
                if (compact_reuse_bind_flowthrough_on) {
                    const uint32_t* record_ptr =
                        in_edge_pair32_flat.data() + begin;
                    const uint32_t* const record_end =
                        in_edge_pair32_flat.data() + end;
                    for (; record_ptr != record_end; ++record_ptr) {
                        const uint64_t rec =
                            gem5_ecg_flow_load_compact_instruction(
                                record_ptr, pair32_id_bits,
                                pair32_epoch_bits);
                        const NodeID v = static_cast<NodeID>(
                            rec & 0xFFFFFFFFULL);
                        incoming_total += gem5_ecg_bind_load_f32(
                            &outgoing_contrib[v], rec);
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] =
                        scores[u] / g.out_degree(u);
                    continue;
                }
                if (wide_reuse_bind_flowthrough_on) {
                    const uint64_t* record_ptr =
                        in_edge_pair_flat.data() + begin;
                    const uint64_t* const record_end =
                        in_edge_pair_flat.data() + end;
                    for (; record_ptr != record_end; ++record_ptr) {
                        const uint64_t rec =
                            gem5_ecg_flow_load_instruction(record_ptr);
                        const NodeID v = static_cast<NodeID>(
                            rec & 0xFFFFFFFFULL);
                        incoming_total += gem5_ecg_bind_load_f32(
                            &outgoing_contrib[v], rec);
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] =
                        scores[u] / g.out_degree(u);
                    continue;
                }
                if (compact_fused_on) {
                    const uint32_t* record_ptr =
                        in_edge_pair32_flat.data() + begin;
                    const uint32_t* const record_end =
                        in_edge_pair32_flat.data() + end;
                    if (compact_fused_trace) {
                        for (; record_ptr != record_end; ++record_ptr) {
                            const uint32_t bits =
                                gem5_ecg_bind_iload_compact_traced(
                                    outgoing_contrib.data(),
                                    *record_ptr,
                                    pair32_id_bits, pair32_epoch_bits);
                            ScoreT delivered;
                            std::memcpy(
                                &delivered, &bits, sizeof(ScoreT));
                            incoming_total += delivered;
                        }
                    } else {
                        for (; record_ptr != record_end; ++record_ptr) {
                            const uint32_t bits =
                                gem5_ecg_bind_iload_compact(
                                    outgoing_contrib.data(),
                                    *record_ptr,
                                    pair32_id_bits, pair32_epoch_bits);
                            ScoreT delivered;
                            std::memcpy(
                                &delivered, &bits, sizeof(ScoreT));
                            incoming_total += delivered;
                        }
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] =
                        scores[u] / g.out_degree(u);
                    continue;
                }
                if (compact_software_fused_on) {
                    const uint32_t* record_ptr =
                        in_edge_pair32_flat.data() + begin;
                    const uint32_t* const record_end =
                        in_edge_pair32_flat.data() + end;
                    for (; record_ptr != record_end; ++record_ptr) {
                        const uint64_t rec = ecg_reuse_plan::widenReusePlan32(
                            *record_ptr, pair32_id_bits,
                            pair32_epoch_bits);
                        const uint32_t bits = gem5_ecg_bind_iload_u32(
                            outgoing_contrib.data(), rec);
                        ScoreT delivered;
                        std::memcpy(
                            &delivered, &bits, sizeof(ScoreT));
                        incoming_total += delivered;
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] =
                        scores[u] / g.out_degree(u);
                    continue;
                }
                if (wide_fused_on) {
                    const uint64_t* record_ptr =
                        in_edge_pair_flat.data() + begin;
                    const uint64_t* const record_end =
                        in_edge_pair_flat.data() + end;
                    for (; record_ptr != record_end; ++record_ptr) {
                        const uint64_t rec = *record_ptr;
                        const uint32_t bits = gem5_ecg_bind_iload_u32(
                            outgoing_contrib.data(), rec);
                        ScoreT delivered;
                        std::memcpy(
                            &delivered, &bits, sizeof(ScoreT));
                        incoming_total += delivered;
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] =
                        scores[u] / g.out_degree(u);
                    continue;
                }
                if (compact_isa_on) {
                    // ONE 4-byte load and ONE instruction per edge: the decoder
                    // widens the compact record and returns the destination, so
                    // the arm differs from the 8-byte arm by width alone rather
                    // than width plus about 16 instructions of software decode.
                    // The traced variant is selected HERE, once, not inside the
                    // loop: a per-edge flag test costs more than the decode it
                    // is checking for.
                    if (compact_isa_trace) {
                        for (uint64_t pos = begin; pos < end; ++pos) {
                            const NodeID v = static_cast<NodeID>(
                                gem5_ecg_extract2c_instruction_traced(
                                    in_edge_pair32_flat[pos],
                                    compact_fmt_word));
                            incoming_total += outgoing_contrib[v];
                            gem5_ecg_clear_extract2_hint();
                        }
                    } else {
                        for (uint64_t pos = begin; pos < end; ++pos) {
                            const NodeID v = static_cast<NodeID>(
                                gem5_ecg_extract2c_instruction(
                                    in_edge_pair32_flat[pos],
                                    compact_fmt_word));
                            incoming_total += outgoing_contrib[v];
                            gem5_ecg_clear_extract2_hint();
                        }
                    }
                    const ScoreT old_score = scores[u];
                    scores[u] = base_score + kDamp * incoming_total;
                    error += fabs(scores[u] - old_score);
                    outgoing_contrib[u] = scores[u] / g.out_degree(u);
                    continue;
                }
                for (uint64_t pos = begin; pos < end; ++pos) {
                    // Non-proposal compact paths widen in software. The
                    // proposal's compact FlowThrough+ReuseBind path is hoisted
                    // above so it pays no per-edge configuration branches.
                    const uint64_t rec = pair32_ok
                        ? ecg_reuse_plan::widenReusePlan32(
                              in_edge_pair32_flat[pos], pair32_id_bits,
                              pair32_epoch_bits)
                        : ecg_flow_load_on
                        ? gem5_ecg_flow_load_instruction(
                              &in_edge_pair_flat[pos])
                        : ecg_plan_load_on
                            ? gem5_ecg_plan_load_instruction(
                                  &in_edge_pair_flat[pos])
                            : in_edge_pair_flat[pos];
                    const NodeID v =
                        static_cast<NodeID>(rec & 0xFFFFFFFFULL);
                    if (ecg_bind_iload_on) {
                        ScoreT delivered;
                        if (ecg_bind_computed_address_on) {
                            delivered = gem5_ecg_bind_load_f32(
                                &outgoing_contrib[v], rec);
                        } else {
                            const uint32_t bits = gem5_ecg_bind_iload_u32(
                                outgoing_contrib.data(), rec);
                            std::memcpy(&delivered, &bits, sizeof(ScoreT));
                        }
                        incoming_total += delivered;
                        continue;
                    }
                    if (!ecg_plan_load_on) {
                        // Direct call, not GEM5_ECG_EXTRACT2: pair_extract_only
                        // already proves extraction is enabled, so the macro's
                        // per-edge re-check is pure overhead, and the traced
                        // wrapper's static guard costs more still. Both were
                        // being charged to this arm and not to the compact-ISA
                        // arm it is compared against.
                        if (pair_trace_on)
                            (void)gem5_ecg_extract2_instruction(rec);
                        else
                            (void)gem5_ecg_extract2_instruction_untraced(rec);
                    }
                    incoming_total += outgoing_contrib[v];
                    gem5_ecg_clear_extract2_hint();
                }
                const ScoreT old_score = scores[u];
                scores[u] = base_score + kDamp * incoming_total;
                error += fabs(scores[u] - old_score);
                outgoing_contrib[u] = scores[u] / g.out_degree(u);
                continue;
            }

            if (packed_extract_only &&
                static_cast<size_t>(u + 1) < packed_off.size()) {
                const uint64_t begin = packed_off[u];
                const uint64_t end = packed_off[u + 1];
                for (uint64_t pos = begin; pos < end; ++pos) {
                    const uint32_t rec = in_edge_packed_flat[pos];
                    const NodeID v = static_cast<NodeID>(rec & pack_id_mask);
                    const uint16_t ep =
                        static_cast<uint16_t>(rec >> pack_id_bits);
                    const uint64_t mask =
                        (static_cast<uint64_t>(v) & 0xFFFFFFULL) |
                        (static_cast<uint64_t>(ep) << 24);
                    GEM5_ECG_EXTRACT_MASK(mask);
                    incoming_total += outgoing_contrib[v];
                }
                const ScoreT old_score = scores[u];
                scores[u] = base_score + kDamp * incoming_total;
                error += fabs(scores[u] - old_score);
                outgoing_contrib[u] = scores[u] / g.out_degree(u);
                continue;
            }

            auto in_neigh = g.in_neigh(u);

            // === Mode 6: per-edge ECG mask path ===
            // Pre-encoded mask at in_edge_masks_by_src[u][edge_pos] carries
            // the dest, DBG/POPT bits, and a POPT-ranked prefetch target.
            // We decode dest from the mask and issue the prefetch hint;
            // demand load on outgoing_contrib[v] happens as normal.
            if ((ecg_prefetch_enabled || ecg_extract_enabled) && ecg_pfx_mode == 6
                && u < static_cast<NodeID>(in_edge_masks_by_src.size())) {
                const auto& src_masks = in_edge_masks_by_src[u];
                // FUSED ecg.load handles the demand delivery (dest + epoch + Path-B
                // prefetch target, all from the decoder) for the eviction-only and
                // Path-B cases. Path A (lean_pfx_k>0) runs a separate lookahead loop
                // that must own prefetch, so it keeps the non-fused demand path.
                const bool use_fused_load = ecg_load_enabled && (lean_pfx_k == 0);
                // FUSED indexed-property load: replaces the demand `contrib[v]` load
                // AND the epoch delivery with one ecg.pload. Eviction-only / Path-B
                // only (Path A owns its own lookahead).
                const bool use_pload = gem5_ecg_pload_enabled() && (lean_pfx_k == 0);
                // dest-field width class for the ecg.load EVICT record: fit the graph
                // (W = 8/16/24/32 bits) so the same op scales to 4.29B vertices.
                const int ecg_evict_wc =
                    ecg_mode6::ecgEvictWidthClass(g.num_nodes());
                if (use_pload) {
                    static bool _ecg_pload_announced = false;
                    if (!_ecg_pload_announced) {
                        _ecg_pload_announced = true;
                        fprintf(stderr, "[ECG_PLOAD] fused indexed-property ecg.pload ACTIVE\n");
                    }
                }
                if (use_fused_load) {
                    static bool _ecg_load_announced = false;
                    if (!_ecg_load_announced) {
                        _ecg_load_announced = true;
                        fprintf(stderr, "[ECG_LOAD] fused ecg.load delivery ACTIVE\n");
                    }
                }
                size_t edge_pos = 0;
                for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it, ++edge_pos) {
                    uint64_t mask;
                    NodeID v;
                    if (use_fused_load) {
                        // FUSED PATH: a single ecg.load reads the 8-byte WIDE record
                        // AND side-delivers its epoch (+ prefetch target) to the LLC,
                        // returning the demand vertex in rd — replacing demand-load +
                        // register-repack + ecg.extract with ONE instruction. (X86
                        // fallback dereferences the record so the kernel still runs.)
                        const uint64_t* rec_ptr =
                            (edge_pos < src_masks.size()) ? &src_masks[edge_pos] : nullptr;
                        v = static_cast<NodeID>(gem5_ecg_load_instruction(rec_ptr));
                        mask = 0;  // unused after for the fused cases
                    } else if (packed_ok && (!ecg_prefetch_enabled || lean_pfx_k > 0)) {
                        // EVICTION-ONLY demand mask (dest+epoch, NO fat-mask pfx
                        // target). Path A also takes this branch: it prefetches
                        // via the epoch-filtered lookahead below, so the demand
                        // must NOT also emit a fat-mask Path-B target. One
                        // contiguous 4-byte record read — the
                        // single edge stream that also carries the epoch (no
                        // separate scattered mask array polluting the LLC). The
                        // 4-byte record holds dest+epoch only, which is all the
                        // ECG_RP eviction path needs.
                        uint32_t rec = in_edge_packed_flat[packed_off[u] + edge_pos];
                        v = static_cast<NodeID>(rec & pack_id_mask);
                        uint16_t ep = static_cast<uint16_t>(rec >> pack_id_bits);
                        // Rebuild the 64-bit WIDE layout ecg.extract decodes
                        // (dest[0:24], epoch[24:40]); ecg.extract is a register
                        // op (no memory access).
                        mask = (static_cast<uint64_t>(v) & 0xFFFFFFULL)
                             | (static_cast<uint64_t>(ep) << 24);
                    } else {
                        // PREFETCH path (or unpacked): use the FULL 64-bit WIDE mask,
                        // which carries the 24-bit prefetch target in bits [40:64]. The
                        // 4-byte packed record drops that field, so the prefetch
                        // hint would be lost (pfx_target=0 => no hint emitted); the
                        // prefetch target needs the wider record / full mask.
                        mask = (edge_pos < src_masks.size()) ? src_masks[edge_pos] : 0;
                        v = static_cast<NodeID>(ecg_mode6::extractDest(mask));
                    }
                    if (ecg_extract_enabled && !use_fused_load && !use_pload) {
                        // Separate register-only ecg.extract delivery. Skipped on the
                        // fused path, where ecg.load already delivered the metadata.
                        GEM5_ECG_EXTRACT_MASK(mask);
                    }
                    if (lean_pfx_k > 0 && ecg_prefetch_enabled) {
                        // Path A: prefetch the next-K in-neighbors' contrib[], each
                        // carrying its own epoch. cand is the streamed edge id (full
                        // width). Mirrors cache_sim bench/src_sim/pr.cc Path A.
                        uint32_t ne = edge_epoch_count;
                        uint32_t cur_ep_k = ecg_reuse_plan::currentEpoch(u, g.num_nodes(), ne);
                        uint32_t thresh = static_cast<uint32_t>(
                            (static_cast<uint64_t>(pfx_epoch_thresh_pct) * ne) / 100);
                        auto jt = it;
                        size_t cpos = edge_pos;
                        for (int step = 0; step < lean_pfx_k; step++) {
                            ++jt; ++cpos;
                            if (jt == in_neigh.end()) break;
                            NodeID cand = *jt;
                            if (cand < 0) continue;
                            uint16_t cand_ep = packed_ok
                                ? static_cast<uint16_t>(
                                      in_edge_packed_flat[packed_off[u] + cpos]
                                          >> pack_id_bits)
                                : (cpos < src_masks.size()
                                       ? static_cast<uint16_t>(
                                             ecg_mode6::extractEpochWide(src_masks[cpos]))
                                       : 0);
                            if (!ecg_reuse_plan::prefetchKeep(cand_ep, cur_ep_k, ne,
                                                         pfx_epoch_filter, thresh))
                                continue;
                            GEM5_ECG_PFX_TARGET_EPOCH(static_cast<uint32_t>(cand),
                                                      cand_ep);
                        }
                    } else if (ecg_prefetch_enabled) {
                        // Path B: single packed prefetch target.
                        uint32_t prefetch_target =
                            ecg_mode6::extractPrefetchTargetWide(mask);
                        if (prefetch_target != 0) {
                            bool in_window = false;
                            for (int w = 0; w < PREFETCH_WINDOW; w++) {
                                if (pfx_window[w] ==
                                    static_cast<NodeID>(prefetch_target)) {
                                    in_window = true;
                                    break;
                                }
                            }
                            if (!in_window) {
                                // Emit the full mode-6 mask via
                                // ecg.extract when the ISA-delivered metadata
                                // channel is enabled. Else fall back to the
                                // legacy prefetch-target-only path.
                                if (!ecg_extract_enabled) {
                                    GEM5_ECG_PFX_TARGET(prefetch_target);
                                }
                                pfx_window[pfx_window_pos % PREFETCH_WINDOW] =
                                    prefetch_target;
                                pfx_window_pos++;
                            }
                        }
                    }
                    if (use_pload) {
                        // FUSED: ecg.load (EVICT) loads contrib[v] AND delivers v's epoch
                        // in one custom-0 op. The record is the width-aware EVICT layout
                        // (dest[0:W] | epoch[W:W+16]); the emitter encodes wc in FUNCT7.
                        uint16_t ep_p = (edge_pos < src_masks.size())
                            ? static_cast<uint16_t>(ecg_mode6::extractEpochWide(src_masks[edge_pos]))
                            : 0;
                        uint64_t fat_p = ecg_mode6::packEvict(
                            static_cast<uint32_t>(v), ep_p, ecg_evict_wc);
                        uint32_t _bits = gem5_ecg_load_evict(
                            outgoing_contrib.data(), fat_p, ecg_evict_wc);
                        ScoreT _pv;
                        std::memcpy(&_pv, &_bits, sizeof(ScoreT));
                        incoming_total += _pv;
                    } else {
                        incoming_total += outgoing_contrib[v];
                    }
                }
                ScoreT old_score = scores[u];
                scores[u] = base_score + kDamp * incoming_total;
                error += fabs(scores[u] - old_score);
                outgoing_contrib[u] = scores[u] / g.out_degree(u);
                continue;
            }

            for (auto it = in_neigh.begin(); it != in_neigh.end(); ++it) {
                NodeID v = *it;
                if (ecg_prefetch_enabled && pfx_lookahead > 0) {
                    NodeID pfx_target = -1;
                    int best_rank = hot_table_size + 1;
                    auto jt = it;
                    for (int step = 0; step < pfx_lookahead; step++) {
                        ++jt;
                        if (jt == in_neigh.end()) break;
                        NodeID candidate = *jt;
                        if (candidate >= 0 && candidate < static_cast<NodeID>(hub_rank.size()) &&
                            hub_rank[candidate] >= 0 && hub_rank[candidate] < best_rank) {
                            best_rank = hub_rank[candidate];
                            pfx_target = candidate;
                        }
                    }
                    if (pfx_target >= 0) {
                        bool in_window = false;
                        for (int w = 0; w < PREFETCH_WINDOW; w++) {
                            if (pfx_window[w] == pfx_target) {
                                in_window = true;
                                break;
                            }
                        }
                        if (!in_window) {
                            GEM5_ECG_PFX_TARGET(pfx_target);
                            pfx_window[pfx_window_pos % PREFETCH_WINDOW] = pfx_target;
                            pfx_window_pos++;
                        }
                    }
                }
                incoming_total += outgoing_contrib[v];
                
                // ECG per-edge prefetch: if neighbor v is a hub vertex,
                // prefetch the NEXT hub in the hot table (the one after v's
                // rank). This brings in the most likely next high-reuse
                // vertex before it's demanded.
                if (ecg_prefetch_enabled && pfx_lookahead == 0 && hub_rank[v] >= 0) {
                    int next_hub = (hub_rank[v] + 1) % hot_table_size;
                    NodeID pfx_target = hot_table[next_hub];
                    
                    // Dedup: skip if recently prefetched
                    bool in_window = false;
                    for (int w = 0; w < PREFETCH_WINDOW; w++) {
                        if (pfx_window[w] == pfx_target) {
                            in_window = true;
                            break;
                        }
                    }
                    if (!in_window) {
                        GEM5_ECG_PFX_TARGET(pfx_target);
                        pfx_window[pfx_window_pos % PREFETCH_WINDOW] = pfx_target;
                        pfx_window_pos++;
                    }
                }
            }
            ScoreT old_score = scores[u];
            scores[u] = base_score + kDamp * incoming_total;
            error += fabs(scores[u] - old_score);
            outgoing_contrib[u] = scores[u] / g.out_degree(u);
        }
        if (error < epsilon) break;
    }

    GEM5_ECG_END_CONTEXT();
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();

    // Post-ROI semantic receipt. The timing matrix intentionally runs one PR
    // sweep, not convergence, so verify that every mechanism produced the same
    // state rather than invoking the convergence verifier. FNV over float bits
    // is deterministic and reads no graph data inside the measured region.
    uint64_t checksum = 1469598103934665603ULL;
    for (ScoreT score : scores) {
        uint32_t bits = 0;
        std::memcpy(&bits, &score, sizeof(bits));
        checksum ^= bits;
        checksum *= 1099511628211ULL;
    }
    const uint64_t semantic_edges =
        static_cast<uint64_t>(executed_iters) *
        static_cast<uint64_t>(g.num_edges_directed());
    fprintf(stderr,
            "[ECG-PR-RESULT iterations=%d semantic_edges=%llu "
            "score_checksum=%016llx]\n",
            executed_iters, (unsigned long long)semantic_edges,
            (unsigned long long)checksum);
    return scores;
}

void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
    vector<pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
    for (NodeID n = 0; n < g.num_nodes(); n++)
        score_pairs[n] = make_pair(n, scores[n]);
    int k = min(5, (int)g.num_nodes());
    partial_sort(score_pairs.begin(), score_pairs.begin() + k, score_pairs.end(),
                [](auto a, auto b) { return a.second > b.second; });
    for (int i = 0; i < k; i++)
        cout << score_pairs[i].first << ": " << score_pairs[i].second << endl;
}

bool PRVerifier(const Graph &g, const pvector<ScoreT> &scores, double target_error) {
    const ScoreT base_score = (1.0f - kDamp) / g.num_nodes();
    pvector<ScoreT> incoming_sums(g.num_nodes(), 0);
    double error = 0;
    for (NodeID u = 0; u < g.num_nodes(); u++) {
        ScoreT outgoing_contrib = scores[u] / g.out_degree(u);
        for (NodeID v : g.out_neigh(u))
            incoming_sums[v] += outgoing_contrib;
    }
    for (NodeID n = 0; n < g.num_nodes(); n++)
        error += fabs(base_score + kDamp * incoming_sums[n] - scores[n]);
    cout << "Total Error: " << error << endl;
    return error < target_error;
}

int main(int argc, char *argv[]) {
    CLPageRank cli(argc, argv, "pagerank-gem5", 1e-4, 20);
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();

    auto PRBound = [&cli](const Graph &g) {
        return PageRankPullGS_Gem5(g, cli.max_iters(), cli.tolerance());
    };
    auto VerifierBound = [&cli](const Graph &g, const pvector<ScoreT> &scores) {
        return PRVerifier(g, scores, cli.tolerance());
    };
    BenchmarkKernel(cli, g, PRBound, PrintTopScores, VerifierBound);
    return 0;
}
