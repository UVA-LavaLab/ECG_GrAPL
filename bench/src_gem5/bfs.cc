// ============================================================================
// BFS (Top-Down only) for gem5 SE-mode simulation
// ============================================================================
// Simple queue-based BFS for single-threaded gem5 SE mode.
// Bottom-up phase requires parallel/atomics which gem5 SE doesn't support well.
// ============================================================================

#include <cstring>
#include <iostream>
#include <queue>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"

#include "graphbrew/partition/cagra/popt.h"
#include "ecg_reuse_plan_builder.h"
#include "ecg_mode6_builder.h"

#include "gem5_sim/gem5_harness.h"
#include "ecg_metadata.h"

using namespace std;

pvector<NodeID> BFS_Gem5(const Graph &g, NodeID source) {
    constexpr size_t kPropAlign = 4096;  // page-align hot property array (see pr.cc)
    pvector<NodeID> parent(g.num_nodes(), -1, kPropAlign);
    parent[source] = source;

    gem5_report_region("parent", parent.data(), g.num_nodes(), sizeof(NodeID));

    Gem5PropertyRegion regions[1] = {
        {"parent", reinterpret_cast<uint64_t>(parent.data()),
         static_cast<uint64_t>(g.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(g.num_nodes()), sizeof(NodeID)},
    };
    Gem5EdgeRegion edge_regions[2];
    int num_edge_regions = gem5_make_edge_regions(g, edge_regions, 2);

    // Per-edge next-ref EPOCH budget (mirror gem5 PR pr.cc:46-53): epoch packs into the
    // spare high bits above the dest id.
    constexpr int kNumVtxPerLine = 64 / sizeof(NodeID);
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
    // A5: deliver the per-edge next-ref epoch for ECG_GRASP_POPT. BFS-TD pushes along
    // OUT-edges writing parent[dest]; dest's property is next-referenced by dest's
    // IN-neighbours, so build epochs with push_out_edges=true (the transpose — same
    // direction as cache_sim's buildOutEdgeMasks). Without this, gem5 BFS delivered no
    // epoch and ECG_GRASP_POPT degenerated to recency.
    bool ecg_extract_on = gem5_ecg_extract_enabled();
    // two-epoch ReusePlan loads the packed record, then carries its ReusePlan mask on the exact
    // parent[dest] request. FlowThrough remains on the record request.
    const bool ecg_plan_load_on = gem5_ecg_plan_load_enabled();
    const bool ecg_flow_load_on = gem5_ecg_flow_load_enabled();
    const bool ecg_bind_iload_on =
        gem5_ecg_pload_enabled() && ecg_reuse_plan_depth == 2;
    const bool ecg_bind_computed_address_on =
        ecg_bind_iload_on && gem5_ecg_bind_computed_address_enabled();
    if (ecg_plan_load_on || ecg_flow_load_on || ecg_bind_iload_on)
        ecg_extract_on = true;
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on) {
        if (ecg_reuse_plan_depth != 2) {
            ecg_reuse_plan::buildInEdgeEpochs(
                g, static_cast<uint32_t>(kNumVtxPerLine),
                edge_epoch_count, /*linemin=*/true,
                out_edge_epochs, /*push_out_edges=*/true);
        }
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
        // No compact path here yet: this kernel builds the 64-bit two-epoch ReusePlan
        // record, so it streams 8 bytes per edge whatever the budget computes.
        // Declaring it keeps the receipt honest; only gem5 PR has the compact
        // 32-bit record so far.
        if (ecg_reuse_plan_depth == 2)
            ::ecg_metadata::declareContainerBytes(ecg_meta, 8);
        ::ecg_metadata::announce(ecg_meta, "gem5-bfs");
        ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "gem5-bfs");
    }
    if (ecg_extract_on && ecg_reuse_plan_depth == 2) {
        std::vector<uint64_t> pair_records;
        ecg_reuse_plan::buildInEdgeReusePlanRecords(
            g, static_cast<uint32_t>(kNumVtxPerLine),
            edge_epoch_count, /*linemin=*/true,
            pair_off, pair_records, /*push_out_edges=*/true);
        gem5_require_canonical_reuse_plan_offsets(
            g, pair_off, pair_records.size(),
            /*push_out_edges=*/true, "bfs");
        pair_flat = pvector<uint64_t>(
            pair_records.size(), uint64_t(0), 4096);
        std::copy(
            pair_records.begin(), pair_records.end(),
            pair_flat.begin());
        pair_ok = true;
    }
    std::vector<uint64_t> epoch_packed_off;
    pvector<uint32_t> epoch_packed_flat;
    uint32_t epoch_pack_id_bits = 1;
    uint32_t epoch_pack_id_mask = 1;
    bool epoch_packed_ok = false;
    if (ecg_extract_on && ecg_reuse_plan_depth != 2) {
        const uint32_t nn = static_cast<uint32_t>(g.num_nodes());
        while (epoch_pack_id_bits < 31 &&
               (uint64_t{1} << epoch_pack_id_bits) < nn)
            ++epoch_pack_id_bits;
        uint32_t epoch_bits = 1;
        while (epoch_bits < 16 &&
               (uint32_t{1} << epoch_bits) < edge_epoch_count)
            ++epoch_bits;
        if (epoch_pack_id_bits + epoch_bits <= 32) {
            epoch_pack_id_mask = (uint32_t{1} << epoch_pack_id_bits) - 1;
            epoch_packed_off.assign(static_cast<size_t>(nn) + 1, 0);
            for (uint32_t u = 0; u < nn; ++u)
                epoch_packed_off[u + 1] =
                    epoch_packed_off[u] + g.out_degree(u);
            epoch_packed_flat = pvector<uint32_t>(
                epoch_packed_off[nn], uint32_t(0), 4096);
            for (uint32_t u = 0; u < nn; ++u) {
                const auto& epochs = out_edge_epochs[u];
                size_t edge_pos = 0;
                for (NodeID v_raw : g.out_neigh(u)) {
                    const uint32_t v = static_cast<uint32_t>(v_raw);
                    const uint16_t epoch = edge_pos < epochs.size()
                        ? epochs[edge_pos]
                        : static_cast<uint16_t>(edge_epoch_count - 1);
                    epoch_packed_flat[epoch_packed_off[u] + edge_pos] =
                        (v & epoch_pack_id_mask) |
                        (static_cast<uint32_t>(epoch) << epoch_pack_id_bits);
                    ++edge_pos;
                }
            }
            epoch_packed_ok = true;
            gem5_require_canonical_reuse_plan_offsets(
                g, epoch_packed_off, epoch_packed_flat.size(),
                /*push_out_edges=*/true, "bfs",
                /*emit_receipt=*/false);
        }
    }
    const char* configured_prefetcher = std::getenv("GRAPHBREW_PREFETCHER");
    const bool packed_stream_compatible =
        !configured_prefetcher ||
        std::string(configured_prefetcher) == "none" ||
        std::string(configured_prefetcher) == "STRIDE";
    const bool pair_extract_only =
        pair_ok && !gem5_ecg_pfx_hints_enabled() &&
        packed_stream_compatible;
    const bool epoch_packed_extract_only =
        epoch_packed_ok && epoch_pack_id_bits <= 24 &&
        !gem5_ecg_pfx_hints_enabled() && packed_stream_compatible;
    gem5_export_context(
        regions, 1, g, GEM5_SIDEBAND_PATH,
        edge_regions, num_edge_regions, edge_epoch_count,
        pair_ok && !pair_flat.empty()
            ? reinterpret_cast<uint64_t>(pair_flat.data())
            : epoch_packed_ok && !epoch_packed_flat.empty()
                ? reinterpret_cast<uint64_t>(epoch_packed_flat.data())
                : 0,
        pair_ok ? pair_flat.size() * sizeof(uint64_t)
            : epoch_packed_ok
                ? epoch_packed_flat.size() * sizeof(uint32_t) : 0,
        nullptr, 0, nullptr, 0, nullptr, 0,
        pair_extract_only && !pair_flat.empty()
            ? reinterpret_cast<uint64_t>(pair_flat.data())
            : epoch_packed_extract_only && !epoch_packed_flat.empty()
                ? reinterpret_cast<uint64_t>(epoch_packed_flat.data())
                : 0,
        pair_extract_only ? pair_flat.size() * sizeof(uint64_t)
            : epoch_packed_extract_only
                ? epoch_packed_flat.size() * sizeof(uint32_t) : 0,
        pair_extract_only || epoch_packed_extract_only
            ? "packed-substitute" : nullptr);

    if (ecg_reuse_plan_depth != 2) {
        constexpr int numVtxPerLine = 64 / sizeof(NodeID);
        constexpr int numEpochs = 256;
        static pvector<uint8_t> popt_matrix;
        // BFS top-down pushes along OUT-edges reading parent[dest]; the next-ref of a
        // vertex's property is over its IN-neighbours, so the reref matrix is the graph
        // TRANSPOSE (CSC/in_neigh, traverseCSR=false) — matching cache_sim's natural_csr=
        // false. Default true=out_neigh is only correct for PR's in-pull. Undirected graphs
        // force true internally (out==in), so this is do-no-harm on the symmetric corpus.
        makeOffsetMatrix(g, popt_matrix, numVtxPerLine, numEpochs, /*traverseCSR=*/false);
        int numCacheLines = (g.num_nodes() + numVtxPerLine - 1) / numVtxPerLine;
        gem5_export_popt_matrix(popt_matrix.data(), numCacheLines,
                                numEpochs, g.num_nodes());
    }
    volatile NodeID* warm_parent = parent.data();
    for (NodeID n = 0; n < g.num_nodes(); ++n)
        warm_parent[n] = n == source ? source : -1;

    Gem5EcgEpochQuantizer epoch_quantizer;
    if (gem5_ecg_epoch_csr_enabled())
        epoch_quantizer.reset(g.num_nodes(), edge_epoch_count);

    GEM5_ECG_BEGIN_CONTEXT();
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    int pfx_lookahead = gem5_env_int_clamped("GEM5_ECG_PFX_LOOKAHEAD", 4, 0, 64);

    // A5: the fused ecg.load EVICT (indexed-property) op reads parent[v] AND delivers v's
    // next-ref epoch to the LLC in one custom-0 instruction (RISC-V), stamping the property
    // line for ECG_GRASP_POPT next-reference eviction — exactly as gem5 PR delivers contrib[v]
    // (pr.cc gem5_ecg_load_evict). Gated on GEM5_ENABLE_ECG_PLOAD; X86 falls back to a plain
    // indexed load (no delivery -> cache_sim is authoritative there).
    const bool ecg_load_evict_on =
        gem5_ecg_pload_enabled() && ecg_extract_on && ecg_reuse_plan_depth != 2;
    const int  ecg_evict_wc = ecg_mode6::ecgEvictWidthClass(g.num_nodes());
    if (pair_extract_only) {
        fprintf(stderr,
                ecg_flow_load_on && ecg_bind_iload_on
                    ? (ecg_bind_computed_address_on
                        ? "[ECG_REUSE_BIND_LOAD] BFS computed-address computed-address load "
                          "+ FlowThrough record load ACTIVE\n"
                        : "[ECG_REUSE_BIND_ILOAD] BFS fused indexed computed-address load "
                          "+ FlowThrough record load ACTIVE\n")
                    : ecg_bind_iload_on
                        ? (ecg_bind_computed_address_on
                            ? "[ECG_REUSE_BIND_LOAD] BFS computed-address computed-address load ACTIVE\n"
                            : "[ECG_REUSE_BIND_ILOAD] BFS fused indexed computed-address load ACTIVE\n")
                    : ecg_flow_load_on
                        ? "[ECG_FLOW_LOAD] BFS request-bound FlowThrough+ReusePlan ACTIVE\n"
                    : ecg_plan_load_on
                        ? "[ECG_PLAN_LOAD] BFS fused ReusePlan record load ACTIVE\n"
                        : "[ECG_PACKED8_REUSE_PLAN] BFS two-epoch ReusePlan packed record path ACTIVE\n");
    } else if (ecg_load_evict_on) {
        static bool _ann = false;
        if (!_ann) { _ann = true;
            fprintf(stderr, "[ECG_PLOAD] BFS fused ecg.load EVICT delivery ACTIVE\n"); }
    }

    queue<NodeID> frontier;
    frontier.push(source);
    while (!frontier.empty()) {
        NodeID u = frontier.front();
        frontier.pop();
        GEM5_SET_QUANTIZED_VERTEX_EPOCH(epoch_quantizer, u);
        auto out_neigh = g.out_neigh(u);
        const std::vector<uint16_t>* u_epochs =
            (ecg_extract_on && static_cast<size_t>(u) < out_edge_epochs.size())
                ? &out_edge_epochs[u] : nullptr;
        if (pair_extract_only) {
            const uint64_t begin =
                static_cast<uint64_t>(g.out_offset(u));
            const uint64_t end =
                static_cast<uint64_t>(g.out_offset(u + 1));
            for (uint64_t pos = begin; pos < end; ++pos) {
                const uint64_t rec = ecg_flow_load_on
                    ? gem5_ecg_flow_load_instruction(&pair_flat[pos])
                    : ecg_plan_load_on
                        ? gem5_ecg_plan_load_instruction(&pair_flat[pos])
                        : pair_flat[pos];
                const NodeID v =
                    static_cast<NodeID>(rec & 0xFFFFFFFFULL);
                NodeID pv;
                if (ecg_bind_iload_on) {
                    if (ecg_bind_computed_address_on) {
                        pv = static_cast<NodeID>(
                            gem5_ecg_bind_load_s32(&parent[v], rec));
                    } else {
                        const uint32_t bits =
                            gem5_ecg_bind_iload_u32(parent.data(), rec);
                        std::memcpy(&pv, &bits, sizeof(NodeID));
                    }
                } else {
                    if (!ecg_plan_load_on)
                        GEM5_ECG_EXTRACT2(rec);
                    pv = parent[v];
                    GEM5_ECG_CLEAR_EXTRACT2_HINT();
                }
                if (pv == -1) {
                    parent[v] = u;
                    frontier.push(v);
                }
            }
            continue;
        }
        if (epoch_packed_extract_only) {
            const uint64_t begin =
                static_cast<uint64_t>(g.out_offset(u));
            const uint64_t end =
                static_cast<uint64_t>(g.out_offset(u + 1));
            for (uint64_t pos = begin; pos < end; ++pos) {
                const uint32_t rec = epoch_packed_flat[pos];
                const NodeID v =
                    static_cast<NodeID>(rec & epoch_pack_id_mask);
                const uint16_t epoch =
                    static_cast<uint16_t>(rec >> epoch_pack_id_bits);
                NodeID pv;
                if (ecg_load_evict_on) {
                    const uint64_t fat = ecg_mode6::packEvict(
                        static_cast<uint32_t>(v), epoch, ecg_evict_wc);
                    const uint32_t bits =
                        gem5_ecg_load_evict(parent.data(), fat, ecg_evict_wc);
                    std::memcpy(&pv, &bits, sizeof(NodeID));
                } else {
                    const uint64_t mask =
                        (static_cast<uint64_t>(v) & 0xFFFFFFULL) |
                        (static_cast<uint64_t>(epoch) << 24);
                    GEM5_ECG_EXTRACT_MASK(mask);
                    pv = parent[v];
                }
                if (pv == -1) {
                    parent[v] = u;
                    frontier.push(v);
                }
            }
            continue;
        }
        size_t edge_pos = 0;
        for (auto it = out_neigh.begin(); it != out_neigh.end(); ++it, ++edge_pos) {
            NodeID v = *it;
            if (pfx_lookahead > 0) {
                auto jt = it;
                for (int step = 0; step < pfx_lookahead; step++) {
                    ++jt;
                    if (jt == out_neigh.end()) break;
                    GEM5_ECG_PFX_TARGET(*jt);
                    break;
                }
            } else {
                GEM5_ECG_PFX_TARGET(v);
            }
            // Read parent[v]. On the ecg.load EVICT path the load also stamps the property
            // line with v's epoch (push_out_edges=true transpose matches BFS-TD's out-edge
            // traversal); otherwise it is a plain read.
            NodeID pv;
            if (ecg_load_evict_on && u_epochs) {
                uint16_t epoch = (edge_pos < u_epochs->size())
                    ? (*u_epochs)[edge_pos]
                    : static_cast<uint16_t>(edge_epoch_count - 1);
                uint64_t fat = ecg_mode6::packEvict(static_cast<uint32_t>(v),
                                                    epoch, ecg_evict_wc);
                uint32_t bits = gem5_ecg_load_evict(parent.data(), fat, ecg_evict_wc);
                std::memcpy(&pv, &bits, sizeof(NodeID));
            } else {
                if (ecg_extract_on && u_epochs &&
                    static_cast<uint32_t>(v) < (1u << 24)) {
                    const uint16_t epoch = edge_pos < u_epochs->size()
                        ? (*u_epochs)[edge_pos]
                        : static_cast<uint16_t>(edge_epoch_count - 1);
                    const uint64_t mask =
                        (static_cast<uint64_t>(v) & 0xFFFFFFULL) |
                        (static_cast<uint64_t>(epoch) << 24);
                    GEM5_ECG_EXTRACT_MASK(mask);
                }
                pv = parent[v];
            }
            if (pv == -1) {
                parent[v] = u;
                frontier.push(v);
            }
        }
    }

    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    gem5_report_semantic_result(
        "bfs", parent.data(), static_cast<size_t>(parent.size()));
    GEM5_DUMP_STATS();
    GEM5_ECG_END_CONTEXT();
    return parent;
}

void PrintBFSStats(const Graph &g, const pvector<NodeID> &parent) {
    int64_t visited = 0;
    for (NodeID n = 0; n < g.num_nodes(); n++)
        if (parent[n] >= 0) visited++;
    cout << "Visited: " << static_cast<int>(visited) << "/" << static_cast<int>(g.num_nodes()) << endl;
}

bool BFSVerifier(const Graph &g, NodeID source, const pvector<NodeID> &parent) {
    if (parent[source] != source) return false;
    pvector<int32_t> depth(g.num_nodes(), -1);
    depth[source] = 0;
    queue<NodeID> q;
    q.push(source);
    while (!q.empty()) {
        NodeID u = q.front(); q.pop();
        for (NodeID v : g.out_neigh(u)) {
            if (depth[v] == -1) {
                depth[v] = depth[u] + 1;
                q.push(v);
            }
        }
    }
    for (NodeID n = 0; n < g.num_nodes(); n++) {
        if (depth[n] >= 0 && parent[n] < 0) return false;
        if (depth[n] < 0 && parent[n] >= 0) return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    CLApp cli(argc, argv, "bfs-gem5");
    if (!cli.ParseArgs()) return -1;
    Builder b(cli);
    Graph g = b.MakeGraph();
    SourcePicker<Graph> sp(g, cli.start_vertex());
    // Separate verifier picker seeded identically to sp so BFSBound (kernel) and
    // VerifyBound (verifier) draw the SAME source sequence — matches cache_sim/canonical
    // (bench/src/bfs.cc). Without this the single picker hands the verifier a DIFFERENT
    // source than the kernel ran on, so verification FAILs unless -r fixes the source.
    SourcePicker<Graph> vsp(g, cli.start_vertex());

    auto BFSBound = [&sp](const Graph &g) {
        return BFS_Gem5(g, sp.PickNext());
    };
    auto PrintBound = [](const Graph &g, const pvector<NodeID> &p) {
        PrintBFSStats(g, p);
    };
    auto VerifyBound = [&vsp](const Graph &g, const pvector<NodeID> &p) {
        return BFSVerifier(g, vsp.PickNext(), p);
    };
    BenchmarkKernel(cli, g, BFSBound, PrintBound, VerifyBound);
    return 0;
}
