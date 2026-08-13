#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "pvector.h"
#include "reader.h"

#include "graphbrew/partition/cagra/popt.h"
#include "sniper_sim/sniper_harness.h"

// ECG mode 6 (per-edge mask) builder — shared with cache_sim and gem5.
#include "ecg_mode6_builder.h"
// Shared per-edge next-reference epoch builder used by
// cache_sim/gem5).
#include "ecg_epoch_builder.h"
#include "ecg_metadata.h"

// File-backed kernel diagnostic target. Native execution is intentionally kept
// lightweight for checking .sg parameters and sideband export. Do not use this
// as a default Sniper/SDE workload until the frontend high-memory run mode is
// fixed; roi_matrix.py keeps it guarded by default.

namespace {

struct SemanticEdgeLimitReached {};

class SemanticEdgeBudget {
  public:
    SemanticEdgeBudget()
    {
        const char* value = std::getenv("SNIPER_SEMANTIC_EDGE_LIMIT");
        if (!value) return;
        if (*value == '\0') {
            std::fprintf(stderr,
                         "[FATAL] empty SNIPER_SEMANTIC_EDGE_LIMIT\n");
            std::abort();
        }
        for (const char* digit = value; *digit != '\0'; ++digit) {
            if (*digit < '0' || *digit > '9') {
                std::fprintf(
                    stderr,
                    "[FATAL] invalid SNIPER_SEMANTIC_EDGE_LIMIT=%s\n",
                    value);
                std::abort();
            }
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0') {
            std::fprintf(stderr,
                         "[FATAL] invalid SNIPER_SEMANTIC_EDGE_LIMIT=%s\n",
                         value);
            std::abort();
        }
        limit_ = parsed;
    }

    void consume()
    {
        if (limit_ == 0) return;
        if (visits_ >= limit_) {
            truncated_ = true;
            finish_roi();
            throw SemanticEdgeLimitReached{};
        }
        ++visits_;
    }

    void finish_roi()
    {
        if (limit_ == 0 || roi_finished_) return;
        SNIPER_ROI_END();
        roi_finished_ = true;
    }

    bool enabled() const { return limit_ != 0; }

    void report(const char* benchmark) const
    {
        if (limit_ == 0) return;
        std::fprintf(
            stderr,
            "[SEMANTIC-ROI benchmark=%s edge_visits=%llu limit=%llu "
            "truncated=%d]\n",
            benchmark,
            static_cast<unsigned long long>(visits_),
            static_cast<unsigned long long>(limit_),
            truncated_ ? 1 : 0);
    }

  private:
    uint64_t limit_ = 0;
    uint64_t visits_ = 0;
    bool truncated_ = false;
    bool roi_finished_ = false;
};

using ScoreT = float;
constexpr float kDamp = 0.85f;
constexpr WeightT kDistInf = std::numeric_limits<WeightT>::max() / 2;

struct Options {
    std::string benchmark = "pr";
    std::string graph_path;
    int max_iters = 2;
    NodeID source = 0;
    WeightT delta = 1;
    int scale = -1;
    int degree = 16;
    std::string reorder_spec;   // -o value (e.g. "5" = DBG); empty = no reorder
    bool symmetrize = false;    // -s
};

bool has_value(int index, int argc) {
    return index + 1 < argc;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--benchmark" || arg == "-B") && has_value(i, argc)) {
            options.benchmark = argv[++i];
        } else if (arg == "-f" && has_value(i, argc)) {
            options.graph_path = argv[++i];
        } else if (arg == "-i" && has_value(i, argc)) {
            options.max_iters = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "-r" && has_value(i, argc)) {
            options.source = static_cast<NodeID>(std::atol(argv[++i]));
        } else if (arg == "-d" && has_value(i, argc)) {
            options.delta = static_cast<WeightT>(std::atol(argv[++i]));
            if (options.delta <= 0) options.delta = 1;
        } else if (arg == "-o" && has_value(i, argc)) {
            options.reorder_spec = argv[++i];   // forward to Builder reorder (was discarded!)
        } else if (arg == "-g" && has_value(i, argc)) {
            options.scale = std::atoi(argv[++i]);
        } else if (arg == "-k" && has_value(i, argc)) {
            options.degree = std::max(1, std::atoi(argv[++i]));
        } else if ((arg == "-n" || arg == "-t") && has_value(i, argc)) {
            ++i;
        } else if (arg == "-s") {
            options.symmetrize = true;
        } else if (arg == "-a" || arg == "-v" || arg == "--") {
            continue;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: sg_kernel --benchmark pr|bfs|sssp|bc|cc "
                         "(-f graph.sg | -g scale [-k degree]) "
                         "[-i iters] [-r source] [-d delta]\n";
            std::exit(0);
        }
    }
    return options;
}

// Build a minimal GAPBS CLI argv from the parsed options so the graph is loaded
// through Builder.MakeGraph() — IDENTICAL path to cache_sim/gem5 (bench/src_sim,
// bench/src_gem5 pr.cc), which applies the -o reorder. Reading the .sg directly
// (the old behaviour) silently skipped the reorder, making all Sniper degree-
// policy runs operate on UNREORDERED graphs.
namespace {
std::vector<std::string> build_gapbs_args(const Options& opt) {
    std::vector<std::string> args = {"sg_kernel"};
    if (!opt.graph_path.empty()) {
        args.insert(args.end(), {"-f", opt.graph_path});
    } else {
        args.insert(args.end(), {
            "-g", std::to_string(opt.scale),
            "-k", std::to_string(opt.degree),
        });
    }
    if (opt.symmetrize) args.push_back("-s");
    if (!opt.reorder_spec.empty()) {
        args.push_back("-o");
        args.push_back(opt.reorder_spec);
    }
    return args;
}
}  // namespace

Graph load_graph(const Options& opt) {
    std::vector<std::string> args = build_gapbs_args(opt);
    std::vector<char*> cargv;
    cargv.reserve(args.size());
    for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
    CLApp cli(static_cast<int>(cargv.size()), cargv.data(), "sg_kernel");
    cli.ParseArgs();
    Builder b(cli);
    return b.MakeGraph();
}

WGraph load_weighted_graph(const Options& opt) {
    std::vector<std::string> args = build_gapbs_args(opt);
    std::vector<char*> cargv;
    cargv.reserve(args.size());
    for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
    CLDelta<WeightT> cli(static_cast<int>(cargv.size()), cargv.data(), "sg_kernel");
    cli.ParseArgs();
    WeightedBuilder b(cli);
    return b.MakeGraph();
}

template <typename GraphType, typename ValueT>
void export_popt_for_graph(const GraphType& graph) {
    constexpr int kNumEpochs = 256;
    const int num_vtx_per_line = std::max<int>(1, 64 / sizeof(ValueT));
    pvector<uint8_t> popt_matrix;
    // Only BFS + SSSP call this helper; both traverse OUT-edges reading the dest
    // property, so the reref matrix is the graph TRANSPOSE (CSC/in_neigh,
    // traverseCSR=false) — matching cache_sim. PR uses its own inline call (true).
    // Undirected graphs force true internally (out==in), so this is do-no-harm there.
    makeOffsetMatrix(graph, popt_matrix, num_vtx_per_line, kNumEpochs, /*traverseCSR=*/false);
    int num_cache_lines = (graph.num_nodes() + num_vtx_per_line - 1) / num_vtx_per_line;
    sniper_export_popt_matrix(popt_matrix.data(), num_cache_lines, kNumEpochs, graph.num_nodes());
}

bool fused_k2_model_enabled() {
    const char* value = std::getenv("SNIPER_ECG_FUSED_K2");
    return value && value[0] && std::string(value) != "0";
}

bool k2_transport_matched_enabled() {
    const char* value = std::getenv("SNIPER_K2_TRANSPORT_MATCHED");
    return value && value[0] && std::string(value) != "0";
}

bool popt_matrix_required() {
    const char* value = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");
    return value && value[0] && std::string(value) != "0";
}

bool stream_bypass_enabled() {
    const char* value = std::getenv("ECG_STREAM_BYPASS");
    return value && value[0] && std::string(value) != "0";
}

bool k2_record_validation_enabled() {
    const char* value = std::getenv("ECG_K2_VALIDATE");
    return value && value[0] && std::string(value) != "0";
}

inline uint32_t consume_fused_k2_sidecar(const uint32_t* sidecar_ptr) {
    const uint32_t sidecar = *sidecar_ptr;
    // Keep the real 4-byte load, but allow it to overlap the weighted-edge
    // stream instead of imposing a software-only full memory barrier.
    asm volatile("" : : "r"(sidecar));
    return sidecar;
}

void deliver_k2_record(uint64_t record, bool fused_k2_model) {
    const uint32_t dest = ecg_epoch::extractEpochPairDest(record);
    const uint8_t tier = ecg_epoch::extractEpochPairTier(record);
    const uint16_t first = ecg_epoch::extractEpochPairFirst(record);
    const uint16_t second = ecg_epoch::extractEpochPairSecond(record);
    if (fused_k2_model) {
        SNIPER_ECG_EXPECT2(dest, tier, first, second);
    } else {
        SNIPER_ECG_EXTRACT2(dest, tier, first, second);
    }
}

void clear_k2_record(uint64_t record, bool fused_k2_model) {
    if (!fused_k2_model) {
        SNIPER_ECG_CLEAR_EXTRACT2(
            ecg_epoch::extractEpochPairDest(record));
    }
}

struct K2PairStream {
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> wide_records;
    std::vector<uint32_t> compact_records;
    uint32_t compact_id_bits = 1;
    uint32_t compact_epoch_bits = 1;
    bool compact = false;

    uint64_t record(uint64_t index) const {
        if (compact) {
            return ecg_epoch::widenEpochPair32(
                compact_records[index],
                compact_id_bits,
                compact_epoch_bits);
        }
        return wide_records[index];
    }

    uint64_t stream_base() const {
        if (compact && !compact_records.empty()) {
            return reinterpret_cast<uint64_t>(compact_records.data());
        }
        return wide_records.empty()
            ? 0
            : reinterpret_cast<uint64_t>(wide_records.data());
    }

    uint64_t stream_bytes() const {
        return compact
            ? compact_records.size() * sizeof(uint32_t)
            : wide_records.size() * sizeof(uint64_t);
    }

    uint32_t record_bytes() const {
        return compact ? sizeof(uint32_t) : sizeof(uint64_t);
    }
};

template <typename GraphT>
bool build_k2_pair_stream(
        const GraphT& graph, uint32_t vertices_per_line,
        uint32_t epoch_count, bool push_out_edges,
        const char* kernel, K2PairStream& stream) {
    const uint32_t num_nodes = static_cast<uint32_t>(graph.num_nodes());
    auto metadata = ::ecg_metadata::configure(num_nodes, epoch_count);
    const bool use_compact =
        metadata.record_bytes == sizeof(uint32_t) &&
        ecg_epoch::canPackEpochPair32(num_nodes, epoch_count);
    ::ecg_metadata::declareContainerBytes(
        metadata, use_compact ? sizeof(uint32_t) : sizeof(uint64_t));
    ::ecg_metadata::announce(metadata, "sniper-sg_kernel");
    ::ecg_metadata::enforceExpectedBytesPerEdge(
        metadata, "sniper-sg_kernel");

    if (use_compact) {
        if (!ecg_epoch::buildInEdgeEpochPairRecords32(
                graph, vertices_per_line, epoch_count,
                /*linemin=*/true, stream.offsets,
                stream.compact_records, push_out_edges)) {
            std::fprintf(
                stderr,
                "sniper-sg %s: compact K2 record construction failed\n",
                kernel);
            return false;
        }
        stream.compact = true;
        stream.compact_id_bits =
            ecg_epoch::epochPair32IdBits(num_nodes);
        stream.compact_epoch_bits =
            ecg_epoch::epochPair32EpochBits(epoch_count);
        std::fprintf(
            stderr,
            "[ECG-PAIR32 sim=sniper kernel=%s records=%llu "
            "id_bits=%u epoch_bits=%u (4-byte, substitutes for the CSR edge)]\n",
            kernel,
            static_cast<unsigned long long>(stream.compact_records.size()),
            stream.compact_id_bits,
            stream.compact_epoch_bits);
        stream.wide_records.resize(stream.compact_records.size());
        for (size_t index = 0; index < stream.compact_records.size(); ++index) {
            stream.wide_records[index] = ecg_epoch::widenEpochPair32(
                stream.compact_records[index],
                stream.compact_id_bits,
                stream.compact_epoch_bits);
        }
        return true;
    }

    ecg_epoch::buildInEdgeEpochPairRecords(
        graph, vertices_per_line, epoch_count,
        /*linemin=*/true, stream.offsets,
        stream.wide_records, push_out_edges);
    return true;
}

int run_pr(const Graph& graph, int max_iters) {
    const ScoreT init_score = graph.num_nodes() > 0 ? 1.0f / graph.num_nodes() : 0.0f;
    const ScoreT base_score = graph.num_nodes() > 0 ? (1.0f - kDamp) / graph.num_nodes() : 0.0f;
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<ScoreT> scores(graph.num_nodes(), init_score, kPropAlign);
    pvector<ScoreT> contrib(graph.num_nodes(), 0.0f, kPropAlign);

    for (NodeID node = 0; node < graph.num_nodes(); ++node) {
        int64_t degree = graph.out_degree(node);
        contrib[node] = degree > 0 ? scores[node] / degree : 0.0f;
    }

    SniperPropertyRegion regions[2] = {
        {"scores", reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(graph.num_nodes()), sizeof(ScoreT), true},
        {"contrib", reinterpret_cast<uint64_t>(contrib.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(ScoreT),
            static_cast<uint32_t>(graph.num_nodes()), sizeof(ScoreT), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(graph, edge_regions, 2, true);

    const int ecg_sched_k =
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_SCHED", 0, 0, 4);
    // Build POPT matrix inline for legacy single-epoch/POPT runs. K2 is
    // matrix-free and retains only its packed 8-byte record stream.
    constexpr int kNumEpochs = 256;
    const int num_vtx_per_line = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_VERTICES_PER_LINE",
        std::max<int>(1, 64 / static_cast<int>(sizeof(ScoreT))),
        1, 1024);
    pvector<uint8_t> popt_matrix;
    const int popt_num_cache_lines =
        (graph.num_nodes() + num_vtx_per_line - 1) / num_vtx_per_line;
    if (ecg_sched_k != 2 || popt_matrix_required()) {
        makeOffsetMatrix(graph, popt_matrix, num_vtx_per_line, kNumEpochs);
        sniper_export_popt_matrix(popt_matrix.data(), popt_num_cache_lines,
                                  kNumEpochs, graph.num_nodes());
    }

    // Per-edge next-reference epoch delivery for ECG_GRASP_POPT. PR pulls
    // IN-edges reading contrib[neighbor], so each entry in node's in-neighbour
    // list carries neighbor's next-reference epoch (the default PR direction,
    // push_out_edges=false). Build before ROI; deliver immediately before the
    // governed contrib[] demand, matching cache_sim/gem5.
    const bool ecg_extract_on = graphbrew_sniper::ecg_extract_enabled();
    const bool ecg_pfx_hints_on =
        graphbrew_sniper::ecg_pfx_hints_enabled();
    const bool fused_k2_model = fused_k2_model_enabled();
    const bool k2_transport_matched = k2_transport_matched_enabled();
    const bool k2_trace_on = graphbrew_sniper::ecg_k2_trace_enabled();
    const bool software_k2_delivery =
        !fused_k2_model || k2_trace_on;
    const bool no_delivery_pair_loop =
        !software_k2_delivery || (k2_transport_matched && !k2_trace_on);
    const bool stream_bypass_on = stream_bypass_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped(
            "ECG_EDGE_MASK_EPOCHS", kNumEpochs, 2, 65535));
    if (ecg_sched_k == 2 || k2_transport_matched)
        ecg_epoch_count =
            ecg_epoch::normalizeK2EpochCount(ecg_epoch_count);
    std::vector<std::vector<uint16_t>> in_edge_epochs_by_src;
    std::vector<uint64_t> epoch_packed_off;
    std::vector<uint32_t> epoch_packed_flat;
    std::vector<uint64_t> epoch_pair_off;
    std::vector<uint64_t> epoch_pair_flat;
    std::vector<uint32_t> epoch_pair32_flat;
    uint32_t epoch_pack_id_bits = 1;
    uint32_t epoch_pack_id_mask = 1;
    uint32_t epoch_pair32_id_bits = 1;
    uint32_t epoch_pair32_epoch_bits = 1;
    bool epoch_packed_ok = false;
    bool epoch_pair_ok = false;
    bool epoch_pair32_ok = false;
    if (ecg_extract_on || k2_transport_matched) {
        if (ecg_extract_on && ecg_sched_k != 2) {
            ecg_epoch::buildInEdgeEpochs(
                graph, num_vtx_per_line, ecg_epoch_count,
                /*linemin=*/true, in_edge_epochs_by_src);
        }

        const uint32_t nn = static_cast<uint32_t>(graph.num_nodes());
        while (epoch_pack_id_bits < 31 &&
               (uint64_t{1} << epoch_pack_id_bits) < nn)
            ++epoch_pack_id_bits;
        uint32_t epoch_bits = 1;
        while (epoch_bits < 16 &&
               (uint32_t{1} << epoch_bits) < ecg_epoch_count)
            ++epoch_bits;
        // Width and structure come from the shared metadata definition, the same
        // header cache_sim and gem5 use, so the three cannot disagree about
        // record width or whether a packed record fits.
        auto ecg_meta = ::ecg_metadata::configure(nn, ecg_epoch_count);
        // The shared rule computes the budget a record could occupy; a backend that
        // materialises it wider must say so. Sniper's Schedule-2 array used to
        // be uint64_t unconditionally while the receipt printed the 4-byte
        // budget. Decide the
        // container first, declare it, and only then announce.
        const bool sniper_pair_requested =
            (ecg_extract_on && ecg_sched_k == 2) || k2_transport_matched;
        const bool use_compact_pair =
            sniper_pair_requested && ecg_meta.record_bytes == 4 &&
            ecg_epoch::canPackEpochPair32(nn, ecg_epoch_count);
        if (sniper_pair_requested)
            ::ecg_metadata::declareContainerBytes(
                ecg_meta, use_compact_pair ? 4 : 8);
        ::ecg_metadata::announce(ecg_meta, "sniper-sg_kernel");
        ::ecg_metadata::enforceExpectedBytesPerEdge(ecg_meta, "sniper-sg_kernel");
        if (ecg_extract_on && ecg_sched_k != 2 && ecg_meta.packed_fits) {
            epoch_pack_id_mask = (uint32_t{1} << epoch_pack_id_bits) - 1;
            epoch_packed_off.assign(static_cast<size_t>(nn) + 1, 0);
            for (uint32_t u = 0; u < nn; ++u)
                epoch_packed_off[u + 1] =
                    epoch_packed_off[u] + graph.in_degree(u);
            epoch_packed_flat.assign(epoch_packed_off[nn], 0);
            for (uint32_t u = 0; u < nn; ++u) {
                const auto& epochs = in_edge_epochs_by_src[u];
                size_t edge_pos = 0;
                for (NodeID v_raw : graph.in_neigh(u)) {
                    const uint32_t v = static_cast<uint32_t>(v_raw);
                    const uint16_t epoch = edge_pos < epochs.size()
                        ? epochs[edge_pos]
                        : static_cast<uint16_t>(ecg_epoch_count - 1);
                    epoch_packed_flat[epoch_packed_off[u] + edge_pos] =
                        (v & epoch_pack_id_mask) |
                        (static_cast<uint32_t>(epoch) << epoch_pack_id_bits);
                    ++edge_pos;
                }
            }
            epoch_packed_ok = true;
        }
        if (sniper_pair_requested) {
            // Prefer the COMPACT 32-bit two-stamp record when the fields fit:
            // it SUBSTITUTES for the 4-byte CSR edge, which is the width
            // cache_sim models, instead of doubling the structural stream.
            // The canonical 64-bit array is still built because the K2 sideband
            // file is a fixed uint64 wire format read out of band by the
            // simulator -- it carries no simulated traffic, so the ROI streams
            // 4 bytes per edge either way.
            if (use_compact_pair &&
                ecg_epoch::buildInEdgeEpochPairRecords32(
                    graph, num_vtx_per_line, ecg_epoch_count,
                    /*linemin=*/true, epoch_pair_off, epoch_pair32_flat)) {
                epoch_pair32_ok = true;
                epoch_pair32_id_bits = ecg_epoch::epochPair32IdBits(nn);
                epoch_pair32_epoch_bits =
                    ecg_epoch::epochPair32EpochBits(ecg_epoch_count);
                std::fprintf(stderr,
                             "[ECG-PAIR32 sim=sniper kernel=pr records=%llu "
                             "id_bits=%u epoch_bits=%u (4-byte, substitutes "
                             "for the CSR edge)]\n",
                             (unsigned long long)epoch_pair32_flat.size(),
                             epoch_pair32_id_bits, epoch_pair32_epoch_bits);
            }
            ecg_epoch::buildInEdgeEpochPairRecords(
                graph, num_vtx_per_line, ecg_epoch_count,
                /*linemin=*/true, epoch_pair_off, epoch_pair_flat);
            epoch_pair_ok = true;
        }

        const char* debug = std::getenv("ECG_DEBUG");
        if (debug && debug[0] && std::string(debug) != "0") {
            uint64_t total = 0;
            uint64_t nonzero = 0;
            uint16_t min_epoch = std::numeric_limits<uint16_t>::max();
            uint16_t max_epoch = 0;
            if (epoch_pair_ok) {
                total = epoch_pair_flat.size();
                for (uint64_t record : epoch_pair_flat) {
                    uint16_t first = ecg_epoch::extractEpochPairFirst(record);
                    uint16_t second = ecg_epoch::extractEpochPairSecond(record);
                    if (first != 0 || second != 0) ++nonzero;
                    min_epoch = std::min(min_epoch, std::min(first, second));
                    max_epoch = std::max(max_epoch, std::max(first, second));
                }
            } else {
                for (const auto& epochs : in_edge_epochs_by_src) {
                    for (uint16_t epoch : epochs) {
                        ++total;
                        if (epoch != 0) ++nonzero;
                        min_epoch = std::min(min_epoch, epoch);
                        max_epoch = std::max(max_epoch, epoch);
                    }
                }
            }
            if (epoch_packed_ok) {
                std::fprintf(stderr,
                             "[ECG_PACKED4 sim=sniper kernel=pr records=%llu "
                             "id_bits=%u epoch_bits=%u]\n",
                             (unsigned long long)epoch_packed_flat.size(),
                             epoch_pack_id_bits, epoch_bits);
            }
            if (total == 0) min_epoch = 0;
            std::fprintf(stderr,
                         "[ECG-EPOCH-BUILD sim=sniper kernel=pr total=%llu "
                         "nonzero=%llu min=%u max=%u ne=%u]\n",
                         (unsigned long long)total,
                         (unsigned long long)nonzero,
                         static_cast<unsigned>(min_epoch),
                         static_cast<unsigned>(max_epoch),
                         ecg_epoch_count);
        }
    }
    // The bypass region must describe the array the ROI actually streams. With
    // the compact record that is the 32-bit array; the 64-bit one exists only
    // as the sideband source and is never touched inside the ROI.
    const uint64_t streamed_pair_base =
        epoch_pair32_ok && !epoch_pair32_flat.empty()
            ? reinterpret_cast<uint64_t>(epoch_pair32_flat.data())
            : (epoch_pair_ok && !epoch_pair_flat.empty()
                ? reinterpret_cast<uint64_t>(epoch_pair_flat.data()) : 0);
    const uint64_t streamed_pair_size =
        epoch_pair32_ok
            ? epoch_pair32_flat.size() * sizeof(uint32_t)
            : (epoch_pair_ok ? epoch_pair_flat.size() * sizeof(uint64_t) : 0);
    if (!sniper_export_context(
        regions, 2, graph, nullptr, edge_regions, num_edge_regions,
        stream_bypass_on
            ? (streamed_pair_base != 0
                ? streamed_pair_base
                : (epoch_packed_ok && !epoch_packed_flat.empty()
                    ? reinterpret_cast<uint64_t>(epoch_packed_flat.data()) : 0))
            : 0,
        stream_bypass_on
            ? (streamed_pair_base != 0
                ? streamed_pair_size
                : (epoch_packed_ok
                    ? epoch_packed_flat.size() * sizeof(uint32_t) : 0))
            : 0,
        epoch_pair_ok ? epoch_pair_off.data() : nullptr,
        epoch_pair_ok ? epoch_pair_off.size() : 0,
        epoch_pair_ok ? epoch_pair_flat.data() : nullptr,
        epoch_pair_ok ? epoch_pair_flat.size() : 0)) {
        std::fprintf(stderr, "sniper-sg PR: context/K2 sideband export failed\n");
        return 2;
    }
    if (epoch_pair_ok && k2_transport_matched)
        std::fprintf(
            stderr,
            "[K2_TRANSPORT_MATCHED] PR %uB record loop ACTIVE\n",
            epoch_pair32_ok
                ? static_cast<unsigned>(sizeof(uint32_t))
                : static_cast<unsigned>(sizeof(uint64_t)));
    if (epoch_pair_ok && k2_transport_matched &&
        graphbrew_sniper::k2_exact_bind_enabled())
        std::fprintf(stderr,
                     "[K2_EXACT_BIND] PR contrib load binding ACTIVE\n");
    volatile ScoreT* warm_scores = scores.data();
    volatile ScoreT* warm_contrib = contrib.data();
    for (NodeID node = 0; node < graph.num_nodes(); ++node) {
        warm_scores[node] = warm_scores[node];
        warm_contrib[node] = warm_contrib[node];
    }

    // Lookahead distance for ECG_PFX hints. node+1 is too close on
    // small graphs (Sniper's cache_cntlr.cc:1146 filters
    // already-in-cache addresses, dropping ECG_PFX hints whose target
    // line is still warm from the previous vertex). Pull from env so
    // sweeps can tune per-graph; default 8 gives PREFETCH_INTERVAL
    // ~250 cycles × 8 vertex iterations ≈ 2000 cycles of head-start.
    const char* pfx_lookahead_env = std::getenv("SNIPER_ECG_PFX_LOOKAHEAD");
    const NodeID pfx_lookahead =
        (pfx_lookahead_env && pfx_lookahead_env[0])
            ? std::max(1, std::atoi(pfx_lookahead_env))
            : 8;

    // === Mode 6: per-edge ECG mask ===
    // SNIPER_ECG_PFX_MODE (preferred) or ECG_PREFETCH_MODE selects the
    // prefetch policy. Mode 6 = per-edge mask path; anything else falls
    // back to the trivial linear-lookahead path below.
    const char* mode_env = std::getenv("SNIPER_ECG_PFX_MODE");
    if (!mode_env || !mode_env[0]) mode_env = std::getenv("ECG_PREFETCH_MODE");
    const int ecg_pfx_mode = (mode_env && mode_env[0]) ? std::atoi(mode_env) : 0;
    const char* ecg_enable_env = std::getenv("SNIPER_ENABLE_ECG_PFX_HINTS");
    const bool ecg_enabled = ecg_enable_env && std::string(ecg_enable_env) != "0";
    const char* configured_prefetcher = std::getenv("SNIPER_GRAPHBREW_PREFETCHER");
    const bool packed_stream_compatible =
        !configured_prefetcher ||
        std::string(configured_prefetcher) == "none" ||
        std::string(configured_prefetcher) == "STRIDE";

    // Build mode-6 fat-mask array BEFORE entering ROI (otherwise
    // Sniper cycle-accurately simulates the offline construction
    // pass, adding 3M-edge × allocation-heavy work to the timed
    // region.
    std::vector<std::vector<uint64_t>> in_edge_masks_by_src;
    if (ecg_enabled && ecg_pfx_mode == 6) {
        std::vector<uint8_t> avg_reref_by_line;
        ecg_mode6::computeAvgRerefByLine(popt_matrix.data(), popt_num_cache_lines,
                                         kNumEpochs, avg_reref_by_line);
        std::vector<uint8_t> tiers;
        ecg_mode6::computeDegreeTiers(graph, tiers);
        ecg_mode6::buildInEdgeMasks(graph, tiers, avg_reref_by_line,
                                    pfx_lookahead, num_vtx_per_line,
                                    in_edge_masks_by_src, "sniper-sg-PR");
        std::printf("[sniper-sg ECG mode 6] lookahead=%d (per-edge mask path active)\n",
                    pfx_lookahead);
    }

    // === Kernel-side hint dedup with an O(1) bitmap ===
    //
    // Each SNIPER_ECG_PFX_TARGET call traps to Sniper main-thread
    // (Pin context-switch, ~5-50us each). For graphs like
    // delaunay_n19 (3M edges × 2 iter = 6.3M calls), this dominates
    // wall time. Suppress calls where the target was emitted within
    // the last KERNEL_DEDUP_WINDOW emissions.
    //
    // First implementation used a linear-scan ring buffer (O(window)
    // per edge → 3M × 256 = 1.6B comparisons that Sniper
    // cycle-accurately simulated). Replaced with an O(1) bitmap
    // indexed by cache-line / hash of cache-line: each emission sets
    // a bit; check is single load. To preserve the recency-window
    // semantics we age the bitmap every WINDOW emissions by clearing
    // and replaying.
    //
    // Tunable via SNIPER_ECG_PFX_KERNEL_DEDUP env var.
    int kernel_dedup_window;
    {
        const char* v = std::getenv("SNIPER_ECG_PFX_KERNEL_DEDUP");
        kernel_dedup_window = (v && v[0]) ? std::atoi(v) : 256;
        if (kernel_dedup_window < 0) kernel_dedup_window = 0;
        if (kernel_dedup_window > (1 << 16)) kernel_dedup_window = (1 << 16);
    }

    // Per-edge AMPLIFY matches cache_sim mode 6.
    //
    // For each edge, after emitting the encoded prefetch_target from
    // the mask, also emit prefetches for the next AMPLIFY masks'
    // decoded destinations. AMPLIFY=0 (default) preserves prior
    // single-target-per-edge behavior. AMPLIFY=N adds N sequential
    // next-dest prefetches per edge (mirrors cache_sim's
    // ECG_EDGE_MASK_AMPLIFY env var).
    //
    // AMPLIFY saturates at 1 because
    // the dedup window absorbs additional candidates. AMPLIFY=1 is
    // the cache_sim-validated sweet spot.
    int amplify;
    {
        const char* v = std::getenv("SNIPER_ECG_EDGE_MASK_AMPLIFY");
        amplify = (v && v[0]) ? std::atoi(v) : 0;
        if (amplify < 0) amplify = 0;
        if (amplify > 8) amplify = 8;
    }
    // Bitmap sized to property-array cache-line count. One bit per
    // property cache line.
    const uint32_t num_property_lines =
        (graph.num_nodes() + num_vtx_per_line - 1) / num_vtx_per_line;
    std::vector<uint64_t> dedup_bitmap;
    if (kernel_dedup_window > 0) {
        dedup_bitmap.assign((num_property_lines + 63) / 64, 0);
    }
    uint64_t kernel_emit_count = 0;
    uint64_t kernel_dedup_count = 0;
    uint64_t emit_since_clear = 0;

    SemanticEdgeBudget semantic_edges;
    auto execute_roi = [&](auto&& consume_edge, auto&& finish_semantic_roi) {
    for (int iter = 0; iter < max_iters; ++iter) {
        for (NodeID node = 0; node < graph.num_nodes(); ++node) {
            SNIPER_SET_VERTEX(node);
            ScoreT incoming_total = 0.0f;

            if (epoch_pair_ok && !ecg_enabled && packed_stream_compatible &&
                static_cast<size_t>(node + 1) < epoch_pair_off.size()) {
                const uint64_t begin = epoch_pair_off[node];
                const uint64_t end = epoch_pair_off[node + 1];
                if (no_delivery_pair_loop) {
                    for (uint64_t pos = begin; pos < end; ++pos) {
                        consume_edge();
                        const uint64_t rec = epoch_pair32_ok
                            ? ecg_epoch::widenEpochPair32(
                                  epoch_pair32_flat[pos], epoch_pair32_id_bits,
                                  epoch_pair32_epoch_bits)
                            : epoch_pair_flat[pos];
                        const NodeID neighbor = static_cast<NodeID>(
                            ecg_epoch::extractEpochPairDest(rec));
                        incoming_total +=
                            graphbrew_sniper::k2_bound_load(
                                &contrib[neighbor]);
                    }
                } else {
                    for (uint64_t pos = begin; pos < end; ++pos) {
                        consume_edge();
                        const uint64_t rec = epoch_pair32_ok
                            ? ecg_epoch::widenEpochPair32(
                                  epoch_pair32_flat[pos], epoch_pair32_id_bits,
                                  epoch_pair32_epoch_bits)
                            : epoch_pair_flat[pos];
                        const NodeID neighbor = static_cast<NodeID>(
                            ecg_epoch::extractEpochPairDest(rec));
                        deliver_k2_record(rec, fused_k2_model);
                        incoming_total +=
                            graphbrew_sniper::k2_bound_load(
                                &contrib[neighbor]);
                        if (!fused_k2_model) {
                            clear_k2_record(rec, fused_k2_model);
                        }
                    }
                }
                scores[node] = base_score + kDamp * incoming_total;
                const int64_t degree = graph.out_degree(node);
                contrib[node] = degree > 0 ? scores[node] / degree : 0.0f;
                continue;
            }

            if (epoch_packed_ok && !ecg_enabled && packed_stream_compatible &&
                static_cast<size_t>(node + 1) < epoch_packed_off.size()) {
                const uint64_t begin = epoch_packed_off[node];
                const uint64_t end = epoch_packed_off[node + 1];
                for (uint64_t pos = begin; pos < end; ++pos) {
                    consume_edge();
                    const uint32_t rec = epoch_packed_flat[pos];
                    const NodeID neighbor =
                        static_cast<NodeID>(rec & epoch_pack_id_mask);
                    const uint16_t epoch =
                        static_cast<uint16_t>(rec >> epoch_pack_id_bits);
                    SNIPER_ECG_EXTRACT(neighbor, epoch);
                    incoming_total += contrib[neighbor];
                }
                scores[node] = base_score + kDamp * incoming_total;
                const int64_t degree = graph.out_degree(node);
                contrib[node] = degree > 0 ? scores[node] / degree : 0.0f;
                continue;
            }

            // Mode 6: per-edge ECG fat-mask path.
            //
            // The fat-mask REPLACES the CSR edge entry: instead of
            // loading a 4-byte vertex ID from CSR + a separate 8-byte
            // mask, we load ONE 8-byte fat-mask that contains both
            // the dest (lower 24 bits) and the prefetch info (upper
            // bits). The ecg_extract operation
            // takes a single 64-bit fat-ID register and decodes
            // vertex + DBG + POPT + prefetch atomically.
            //
            // An earlier implementation read both
            // the CSR and the mask per edge, doubling memory cost
            // and producing an 8x DRAM-traffic regression on Sniper.
            // The current implementation iterates ONLY the mask
            // array; CSR is bypassed entirely in this path.
            if (ecg_enabled && ecg_pfx_mode == 6 &&
                node < static_cast<NodeID>(in_edge_masks_by_src.size())) {
                const auto& src_masks = in_edge_masks_by_src[node];
                const size_t num_masks = src_masks.size();
                for (size_t edge_idx = 0; edge_idx < num_masks; ++edge_idx) {
                    consume_edge();
                    const uint64_t mask = src_masks[edge_idx];
                    NodeID neighbor = static_cast<NodeID>(ecg_mode6::extractDest(mask));
                    if (neighbor < 0 || neighbor >= graph.num_nodes()) continue;
                    if (ecg_extract_on &&
                        static_cast<size_t>(node) < in_edge_epochs_by_src.size()) {
                        const auto& eps = in_edge_epochs_by_src[node];
                        uint16_t ep = (edge_idx < eps.size()) ? eps[edge_idx]
                            : static_cast<uint16_t>(ecg_epoch_count - 1);
                        SNIPER_ECG_EXTRACT(neighbor, ep);
                    }
                    uint32_t prefetch_target = ecg_mode6::extractPrefetchTarget(mask);
                    if (prefetch_target != 0 &&
                        prefetch_target < static_cast<uint32_t>(graph.num_nodes())) {
                        // O(1) bitmap deduplication.
                        // Suppress emission if cache-line was already
                        // emitted within the recency window.
                        uint32_t target_line = prefetch_target /
                            static_cast<uint32_t>(num_vtx_per_line);
                        bool seen = false;
                        if (kernel_dedup_window > 0) {
                            size_t word_idx = target_line / 64;
                            uint64_t bit = uint64_t{1} << (target_line % 64);
                            if (word_idx < dedup_bitmap.size()) {
                                if (dedup_bitmap[word_idx] & bit) {
                                    seen = true;
                                } else {
                                    dedup_bitmap[word_idx] |= bit;
                                }
                            }
                        }
                        if (!seen && ecg_pfx_hints_on) {
                            SNIPER_ECG_PFX_TARGET(prefetch_target);
                            kernel_emit_count++;
                            emit_since_clear++;
                            // Age the bitmap every WINDOW emissions
                            // to keep dedup window-bounded recency
                            // semantics (without per-emit O(window)
                            // scans).
                            if (kernel_dedup_window > 0 &&
                                emit_since_clear >= static_cast<uint64_t>(kernel_dedup_window)) {
                                std::fill(dedup_bitmap.begin(), dedup_bitmap.end(), 0);
                                emit_since_clear = 0;
                            }
                        } else if (seen) {
                            kernel_dedup_count++;
                        }
                    }
                    // AMPLIFY emits the next N decoded
                    // dests as additional prefetches. Mirrors cache_sim
                    // mode 6 AMPLIFY semantics. AMPLIFY=0 (default)
                    // = no extra emissions = unchanged from before.
                    for (int step = 1; step <= amplify; ++step) {
                        const size_t fwd_idx = edge_idx + static_cast<size_t>(step);
                        if (fwd_idx >= num_masks) break;
                        const uint32_t fwd_dest = ecg_mode6::extractDest(src_masks[fwd_idx]);
                        if (fwd_dest == 0 ||
                            fwd_dest >= static_cast<uint32_t>(graph.num_nodes())) continue;
                        uint32_t fwd_line = fwd_dest /
                            static_cast<uint32_t>(num_vtx_per_line);
                        bool fwd_seen = false;
                        if (kernel_dedup_window > 0) {
                            size_t word_idx = fwd_line / 64;
                            uint64_t bit = uint64_t{1} << (fwd_line % 64);
                            if (word_idx < dedup_bitmap.size()) {
                                if (dedup_bitmap[word_idx] & bit) {
                                    fwd_seen = true;
                                } else {
                                    dedup_bitmap[word_idx] |= bit;
                                }
                            }
                        }
                        if (!fwd_seen && ecg_pfx_hints_on) {
                            SNIPER_ECG_PFX_TARGET(fwd_dest);
                            kernel_emit_count++;
                            emit_since_clear++;
                            if (kernel_dedup_window > 0 &&
                                emit_since_clear >= static_cast<uint64_t>(kernel_dedup_window)) {
                                std::fill(dedup_bitmap.begin(), dedup_bitmap.end(), 0);
                                emit_since_clear = 0;
                            }
                        } else if (fwd_seen) {
                            kernel_dedup_count++;
                        }
                    }
                    incoming_total += contrib[neighbor];
                }
            } else {
                // Default: trivial linear lookahead (preserves prior
                // behavior when mode != 6).
                NodeID pfx_target = node + pfx_lookahead;
                if (ecg_pfx_hints_on && pfx_target < graph.num_nodes()) {
                    SNIPER_ECG_PFX_TARGET(pfx_target);
                }
                size_t edge_pos = 0;
                auto neighbors = graph.in_neigh(node);
                for (auto it = neighbors.begin(); it != neighbors.end(); ++it) {
                    consume_edge();
                    const NodeID neighbor = *it;
                    if (ecg_extract_on &&
                        static_cast<size_t>(node) < in_edge_epochs_by_src.size()) {
                        const auto& eps = in_edge_epochs_by_src[node];
                        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                            : static_cast<uint16_t>(ecg_epoch_count - 1);
                        SNIPER_ECG_EXTRACT(neighbor, ep);
                    }
                    ++edge_pos;
                    incoming_total += contrib[neighbor];
                }
            }

            scores[node] = base_score + kDamp * incoming_total;
            int64_t degree = graph.out_degree(node);
            contrib[node] = degree > 0 ? scores[node] / degree : 0.0f;
        }
    }
    finish_semantic_roi();
    };
    if (semantic_edges.enabled()) {
        SNIPER_ROI_BEGIN();
        try {
            execute_roi(
                [&] { semantic_edges.consume(); },
                [&] { semantic_edges.finish_roi(); });
        } catch (const SemanticEdgeLimitReached&) {
        }
    } else {
        SNIPER_ROI_BEGIN();
        execute_roi([] {}, [] {});
        SNIPER_ROI_END();
    }
    semantic_edges.report("pr");

    if (ecg_enabled && ecg_pfx_mode == 6) {
        std::printf("[sniper-sg ECG mode 6] emit=%llu kernel-dedup-skip=%llu (window=%d)\n",
                    static_cast<unsigned long long>(kernel_emit_count),
                    static_cast<unsigned long long>(kernel_dedup_count),
                    kernel_dedup_window);
    }

    ScoreT checksum = 0.0f;
    for (ScoreT score : scores) checksum += score;
    std::cout << "GraphBrew Sniper SG PR checksum: " << checksum << std::endl;
    return std::fabs(checksum) > 0.0f ? 0 : 1;
}

int run_bfs(const Graph& graph, NodeID source) {
    if (source < 0 || source >= graph.num_nodes()) source = 0;
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<NodeID> parent(graph.num_nodes(), -1, kPropAlign);
    parent[source] = source;

    SniperPropertyRegion regions[1] = {
        {"parent", reinterpret_cast<uint64_t>(parent.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(NodeID)},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(graph, edge_regions, 2);
    const int bfs_sched_k =
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_SCHED", 0, 0, 4);
    if (bfs_sched_k != 2 || popt_matrix_required())
        export_popt_for_graph<Graph, NodeID>(graph);

    // SNIPER_ECG_EXTRACT (delivery-faithful, mirrors gem5 ecg.load EVICT): deliver each
    // demand edge's next-ref epoch so ECG_GRASP_POPT ranks parent[] by a delivered epoch
    // instead of the host-side findNextRef matrix. BFS-TD pushes OUT-edges writing
    // parent[dest]; dest is next-referenced by its IN-neighbours -> push_out_edges=true
    // (the transpose; same builder as cache_sim/gem5). Gated on SNIPER_ENABLE_ECG_EXTRACT.
    const uint32_t kNumVtxPerLine = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped(
            "SNIPER_ECG_VERTICES_PER_LINE",
            64 / sizeof(NodeID), 1, 1024));
    const bool ecg_extract_on = graphbrew_sniper::ecg_extract_enabled();
    const bool ecg_pfx_hints_on =
        graphbrew_sniper::ecg_pfx_hints_enabled();
    const bool fused_k2_model = fused_k2_model_enabled();
    const bool k2_transport_matched = k2_transport_matched_enabled();
    const bool k2_trace_on = graphbrew_sniper::ecg_k2_trace_enabled();
    const bool software_k2_delivery =
        !fused_k2_model || k2_trace_on;
    const bool no_delivery_pair_loop =
        !software_k2_delivery || (k2_transport_matched && !k2_trace_on);
    const bool stream_bypass_on = stream_bypass_enabled();
    const char* configured_prefetcher = std::getenv("SNIPER_GRAPHBREW_PREFETCHER");
    const bool packed_stream_compatible =
        !configured_prefetcher ||
        std::string(configured_prefetcher) == "none" ||
        std::string(configured_prefetcher) == "STRIDE";
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", 256, 2, 65535));
    if (bfs_sched_k == 2 || k2_transport_matched)
        ecg_epoch_count =
            ecg_epoch::normalizeK2EpochCount(ecg_epoch_count);
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on) {
        if (bfs_sched_k != 2) {
            ecg_epoch::buildInEdgeEpochs(
                graph, kNumVtxPerLine, ecg_epoch_count,
                /*linemin=*/true, out_edge_epochs,
                /*push_out_edges=*/true);
        }
    }
    K2PairStream bfs_pairs;
    bool bfs_pair_ok = false;
    if ((ecg_extract_on && bfs_sched_k == 2) ||
        k2_transport_matched) {
        bfs_pair_ok = build_k2_pair_stream(
            graph, kNumVtxPerLine, ecg_epoch_count,
            /*push_out_edges=*/true, "bfs", bfs_pairs);
        if (!bfs_pair_ok) return 2;
    }
    std::vector<uint64_t> bfs_packed_off;
    std::vector<uint32_t> bfs_packed_flat;
    uint32_t bfs_pack_id_bits = 1;
    uint32_t bfs_pack_id_mask = 1;
    bool bfs_packed_ok = false;
    if (ecg_extract_on && bfs_sched_k != 2) {
        const uint32_t nn = static_cast<uint32_t>(graph.num_nodes());
        while (bfs_pack_id_bits < 31 &&
               (uint64_t{1} << bfs_pack_id_bits) < nn)
            ++bfs_pack_id_bits;
        uint32_t bfs_epoch_bits = 1;
        while (bfs_epoch_bits < 16 &&
               (uint32_t{1} << bfs_epoch_bits) < ecg_epoch_count)
            ++bfs_epoch_bits;
        if (bfs_pack_id_bits + bfs_epoch_bits <= 32) {
            bfs_pack_id_mask = (uint32_t{1} << bfs_pack_id_bits) - 1;
            bfs_packed_off.assign(static_cast<size_t>(nn) + 1, 0);
            for (uint32_t u = 0; u < nn; ++u)
                bfs_packed_off[u + 1] =
                    bfs_packed_off[u] + graph.out_degree(u);
            bfs_packed_flat.assign(bfs_packed_off[nn], 0);
            for (uint32_t u = 0; u < nn; ++u) {
                const auto& epochs = out_edge_epochs[u];
                size_t edge_pos = 0;
                for (NodeID v_raw : graph.out_neigh(u)) {
                    const uint32_t v = static_cast<uint32_t>(v_raw);
                    const uint16_t epoch = edge_pos < epochs.size()
                        ? epochs[edge_pos]
                        : static_cast<uint16_t>(ecg_epoch_count - 1);
                    bfs_packed_flat[bfs_packed_off[u] + edge_pos] =
                        (v & bfs_pack_id_mask) |
                        (static_cast<uint32_t>(epoch) << bfs_pack_id_bits);
                    ++edge_pos;
                }
            }
            bfs_packed_ok = true;
            if (std::getenv("ECG_DEBUG")) {
                std::fprintf(stderr,
                             "[ECG_PACKED4 sim=sniper kernel=bfs records=%llu "
                             "id_bits=%u epoch_bits=%u]\n",
                             (unsigned long long)bfs_packed_flat.size(),
                             bfs_pack_id_bits, bfs_epoch_bits);
            }
        }
    }
    if (!sniper_export_context(
            regions, 1, graph, nullptr, edge_regions, num_edge_regions,
            stream_bypass_on && bfs_pair_ok
                ? bfs_pairs.stream_base() : 0,
            stream_bypass_on && bfs_pair_ok
                ? bfs_pairs.stream_bytes() : 0,
            fused_k2_model && bfs_pair_ok
                ? bfs_pairs.offsets.data() : nullptr,
            fused_k2_model && bfs_pair_ok
                ? bfs_pairs.offsets.size() : 0,
            fused_k2_model && bfs_pair_ok
                ? bfs_pairs.wide_records.data() : nullptr,
            fused_k2_model && bfs_pair_ok
                ? bfs_pairs.wide_records.size() : 0)) {
        std::fprintf(stderr, "sniper-sg BFS: context/K2 sideband export failed\n");
        return 2;
    }
    if (bfs_pair_ok) {
        std::fprintf(
            stderr,
            fused_k2_model
                ? "[ECG_FUSED_K2] BFS Schedule-2 fused sideband ACTIVE\n"
                : "[ECG_PACKED8_K2] BFS Schedule-2 packed record path ACTIVE\n");
    }
    if (bfs_pair_ok && k2_transport_matched)
        std::fprintf(
            stderr,
            "[K2_TRANSPORT_MATCHED] BFS %uB record loop ACTIVE\n",
            bfs_pairs.record_bytes());
    if (bfs_pair_ok && k2_transport_matched &&
        graphbrew_sniper::k2_exact_bind_enabled())
        std::fprintf(stderr,
                     "[K2_EXACT_BIND] BFS parent load binding ACTIVE\n");
    volatile NodeID* warm_parent = parent.data();
    for (NodeID node = 0; node < graph.num_nodes(); ++node)
        warm_parent[node] = node == source ? source : -1;

    SemanticEdgeBudget semantic_edges;
    std::queue<NodeID> frontier;
    auto execute_roi = [&](auto&& consume_edge, auto&& finish_semantic_roi) {
    frontier.push(source);
    while (!frontier.empty()) {
        NodeID node = frontier.front();
        frontier.pop();
        SNIPER_SET_VERTEX(node);
        // ECG_PFX hint: emit the head of the frontier (the node we'll expand next) so the
        // prefetcher can warm parent[next_node]. Env-gated (SNIPER_ENABLE_ECG_PFX_HINTS).
        if (ecg_pfx_hints_on && !frontier.empty()) {
            SNIPER_ECG_PFX_TARGET(frontier.front());
        }
        if (bfs_pair_ok && !ecg_pfx_hints_on &&
            packed_stream_compatible &&
            static_cast<size_t>(node + 1) < bfs_pairs.offsets.size()) {
            const uint64_t begin = bfs_pairs.offsets[node];
            const uint64_t end = bfs_pairs.offsets[node + 1];
            if (no_delivery_pair_loop) {
                for (uint64_t pos = begin; pos < end; ++pos) {
                    consume_edge();
                    const uint64_t rec = bfs_pairs.record(pos);
                    const NodeID neighbor = static_cast<NodeID>(
                        ecg_epoch::extractEpochPairDest(rec));
                    const NodeID parent_value =
                        graphbrew_sniper::k2_bound_load(
                            &parent[neighbor]);
                    if (parent_value == -1) {
                        parent[neighbor] = node;
                        frontier.push(neighbor);
                    }
                }
            } else {
                for (uint64_t pos = begin; pos < end; ++pos) {
                    consume_edge();
                    const uint64_t rec = bfs_pairs.record(pos);
                    const NodeID neighbor = static_cast<NodeID>(
                        ecg_epoch::extractEpochPairDest(rec));
                    deliver_k2_record(rec, fused_k2_model);
                    const NodeID parent_value =
                        graphbrew_sniper::k2_bound_load(
                            &parent[neighbor]);
                    if (!fused_k2_model) {
                        clear_k2_record(rec, fused_k2_model);
                    }
                    if (parent_value == -1) {
                        parent[neighbor] = node;
                        frontier.push(neighbor);
                    }
                }
            }
            continue;
        }
        if (bfs_packed_ok && !ecg_pfx_hints_on &&
            packed_stream_compatible &&
            static_cast<size_t>(node + 1) < bfs_packed_off.size()) {
            const uint64_t begin = bfs_packed_off[node];
            const uint64_t end = bfs_packed_off[node + 1];
            for (uint64_t pos = begin; pos < end; ++pos) {
                consume_edge();
                const uint32_t rec = bfs_packed_flat[pos];
                const NodeID neighbor =
                    static_cast<NodeID>(rec & bfs_pack_id_mask);
                const uint16_t epoch =
                    static_cast<uint16_t>(rec >> bfs_pack_id_bits);
                SNIPER_ECG_EXTRACT(neighbor, epoch);
                if (parent[neighbor] == -1) {
                    parent[neighbor] = node;
                    frontier.push(neighbor);
                }
            }
            continue;
        }
        const std::vector<uint16_t>* eps =
            (ecg_extract_on && static_cast<size_t>(node) < out_edge_epochs.size())
                ? &out_edge_epochs[node] : nullptr;
        size_t edge_pos = 0;
        auto neighbors = graph.out_neigh(node);
        for (auto it = neighbors.begin(); it != neighbors.end(); ++it) {
            consume_edge();
            const NodeID neighbor = *it;
            // Deliver neighbor's epoch BEFORE reading parent[neighbor] so cache_set_ecg
            // stamps the property line on fill.
            if (eps) {
                uint16_t ep = (edge_pos < eps->size()) ? (*eps)[edge_pos]
                    : static_cast<uint16_t>(ecg_epoch_count - 1);
                SNIPER_ECG_EXTRACT(neighbor, ep);
            }
            ++edge_pos;
            if (parent[neighbor] == -1) {
                parent[neighbor] = node;
                frontier.push(neighbor);
            }
        }
    }
    finish_semantic_roi();
    };
    if (semantic_edges.enabled()) {
        SNIPER_ROI_BEGIN();
        try {
            execute_roi(
                [&] { semantic_edges.consume(); },
                [&] { semantic_edges.finish_roi(); });
        } catch (const SemanticEdgeLimitReached&) {
        }
    } else {
        SNIPER_ROI_BEGIN();
        execute_roi([] {}, [] {});
        SNIPER_ROI_END();
    }
    semantic_edges.report("bfs");

    int64_t reached = 0;
    for (NodeID value : parent) reached += value >= 0 ? 1 : 0;
    std::cout << "GraphBrew Sniper SG BFS reached: " << static_cast<long long>(reached) << std::endl;
    return reached > 0 ? 0 : 1;
}

int run_sssp(const WGraph& graph, NodeID source, WeightT delta) {
    (void)delta;
    if (source < 0 || source >= graph.num_nodes()) source = 0;
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<WeightT> dist(graph.num_nodes(), kDistInf, kPropAlign);
    pvector<uint8_t> in_queue(graph.num_nodes(), 0);
    dist[source] = 0;

    SniperPropertyRegion regions[1] = {
        {"dist", reinterpret_cast<uint64_t>(dist.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(WeightT),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(WeightT)},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(graph, edge_regions, 2);
    const int ecg_sched_k =
        graphbrew_sniper::env_int_clamped(
            "ECG_EDGE_MASK_SCHED", 0, 0, 4);
    if (ecg_sched_k != 2 || popt_matrix_required())
        export_popt_for_graph<WGraph, WeightT>(graph);

    // SNIPER_ECG_EXTRACT (delivery-faithful, mirrors gem5 ecg.load EVICT): deliver each
    // relaxed edge's next-ref epoch so ECG_GRASP_POPT ranks dist[] by a delivered epoch.
    // SSSP relaxes OUT-edges reading dist[dest]; dest is next-referenced by its
    // IN-neighbours -> push_out_edges=true (transpose). Gated on SNIPER_ENABLE_ECG_EXTRACT.
    const uint32_t kNumVtxPerLine = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped(
            "SNIPER_ECG_VERTICES_PER_LINE",
            64 / sizeof(WeightT), 1, 1024));
    const bool ecg_extract_on = graphbrew_sniper::ecg_extract_enabled();
    const bool ecg_pfx_hints_on =
        graphbrew_sniper::ecg_pfx_hints_enabled();
    const bool fused_k2_model = fused_k2_model_enabled();
    const bool k2_transport_matched = k2_transport_matched_enabled();
    const bool k2_trace_on = graphbrew_sniper::ecg_k2_trace_enabled();
    const bool software_k2_delivery =
        !fused_k2_model || k2_trace_on;
    const bool stream_bypass_on = stream_bypass_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", 256, 2, 65535));
    if (ecg_sched_k == 2 || k2_transport_matched)
        ecg_epoch_count =
            ecg_epoch::normalizeK2EpochCount(ecg_epoch_count);
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_sched_k != 2) {
        ecg_epoch::buildInEdgeEpochs(graph, kNumVtxPerLine, ecg_epoch_count,
                                     /*linemin=*/true, out_edge_epochs,
                                     /*push_out_edges=*/true);
    }
    std::vector<uint64_t> pair_off;
    std::vector<uint64_t> pair_flat;
    pvector<uint32_t> pair_sidecars;
    pvector<uint64_t> pair_compact;
    bool pair_ok = false;
    bool compact_pair_ok = false;
    if ((ecg_extract_on && ecg_sched_k == 2) ||
        k2_transport_matched) {
        ecg_epoch::buildInEdgeEpochPairRecords(
            graph, kNumVtxPerLine, ecg_epoch_count,
            /*linemin=*/true, pair_off, pair_flat,
            /*push_out_edges=*/true);
        pair_sidecars = pvector<uint32_t>(
            pair_flat.size(), uint32_t(0), kPropAlign);
        for (size_t i = 0; i < pair_flat.size(); ++i) {
            pair_sidecars[i] = ecg_epoch::packWeightedEpochPairSidecar(
                ecg_epoch::extractEpochPairTier(pair_flat[i]),
                ecg_epoch::extractEpochPairFirst(pair_flat[i]),
                ecg_epoch::extractEpochPairSecond(pair_flat[i]));
        }
        pair_compact = pvector<uint64_t>(
            pair_flat.size(), uint64_t(0), kPropAlign);
        compact_pair_ok =
            static_cast<uint64_t>(graph.num_nodes()) <=
                ecg_epoch::kCompactWeightedMaxVertices;
        for (NodeID src = 0; compact_pair_ok && src < graph.num_nodes(); ++src) {
            uint64_t pos = pair_off[src];
            for (WNode edge : graph.out_neigh(src)) {
                if (!ecg_epoch::canPackCompactWeightedEdge(
                        graph.num_nodes(), static_cast<uint32_t>(edge.v),
                        static_cast<int64_t>(edge.w))) {
                    compact_pair_ok = false;
                    break;
                }
                const uint64_t pair = pair_flat[pos];
                pair_compact[pos] =
                    ecg_epoch::packCompactWeightedEpochPairRecord(
                        static_cast<uint32_t>(edge.v),
                        static_cast<uint32_t>(edge.w),
                        ecg_epoch::extractEpochPairTier(pair),
                        ecg_epoch::extractEpochPairFirst(pair),
                        ecg_epoch::extractEpochPairSecond(pair));
                ++pos;
            }
        }
        if (!compact_pair_ok) pair_compact.clear();
        pair_ok = true;
    }
    if (pair_ok) {
        auto metadata = ::ecg_metadata::configure(
            static_cast<uint32_t>(graph.num_nodes()), ecg_epoch_count);
        const int transport_bytes = (
            fused_k2_model && !compact_pair_ok) ? 12 : 8;
        ::ecg_metadata::declareContainerBytes(metadata, transport_bytes);
        metadata.packed_fits = compact_pair_ok;
        ::ecg_metadata::announce(metadata, "sniper-sg_kernel");
        ::ecg_metadata::enforceExpectedBytesPerEdge(
            metadata, "sniper-sg_kernel");
    }
    if (pair_ok && k2_record_validation_enabled() &&
        (!ecg_epoch::validateWeightedEpochPairRecords(
             graph, pair_off, pair_flat) ||
         !ecg_epoch::validateWeightedEpochPairSidecars(
             pair_off, pair_flat, pair_sidecars) ||
         (compact_pair_ok &&
          !ecg_epoch::validateCompactWeightedEpochPairRecords(
              graph, pair_off, pair_flat, pair_compact)))) {
        std::fprintf(stderr, "Sniper SSSP K2 record validation failed\n");
        std::abort();
    }
    if (!sniper_export_context(
            regions, 1, graph, nullptr, edge_regions, num_edge_regions,
            stream_bypass_on && pair_ok
                ? (fused_k2_model
                    ? reinterpret_cast<uint64_t>(
                        compact_pair_ok
                            ? static_cast<const void*>(pair_compact.data())
                            : static_cast<const void*>(pair_sidecars.data()))
                    : reinterpret_cast<uint64_t>(pair_flat.data())) : 0,
            stream_bypass_on && pair_ok
                ? (fused_k2_model
                    ? (compact_pair_ok
                        ? pair_compact.size() * sizeof(uint64_t)
                        : pair_sidecars.size() * sizeof(uint32_t))
                    : pair_flat.size() * sizeof(uint64_t)) : 0,
            fused_k2_model && pair_ok ? pair_off.data() : nullptr,
            fused_k2_model && pair_ok ? pair_off.size() : 0,
            fused_k2_model && pair_ok ? pair_flat.data() : nullptr,
            fused_k2_model && pair_ok ? pair_flat.size() : 0)) {
        std::fprintf(stderr, "sniper-sg SSSP: context/K2 sideband export failed\n");
        return 2;
    }
    if (pair_ok)
        std::fprintf(
            stderr,
            fused_k2_model && compact_pair_ok
                ? "[ECG_FUSED_K2_WEIGHTED64] SSSP compact 8B record ACTIVE\n"
                : fused_k2_model
                ? "[ECG_FUSED_K2_WEIGHTED32] SSSP 4B sidecar ACTIVE\n"
                : "[ECG_PACKED8_K2] SSSP Schedule-2 packed record path ACTIVE\n");
    if (k2_transport_matched)
        std::fprintf(
            stderr,
            compact_pair_ok
                ? "[K2_TRANSPORT_MATCHED] SSSP compact 8B record loop ACTIVE\n"
                : "[K2_TRANSPORT_MATCHED] SSSP general 12B edge+sidecar loop ACTIVE\n");
    if (pair_ok && k2_transport_matched &&
        graphbrew_sniper::k2_exact_bind_enabled())
        std::fprintf(stderr,
                     "[K2_EXACT_BIND] SSSP edge-governed dist[dest] binding ACTIVE\n");

    SemanticEdgeBudget semantic_edges;
    std::queue<NodeID> frontier;
    auto execute_roi = [&](auto&& consume_edge, auto&& finish_semantic_roi) {
    frontier.push(source);
    in_queue[source] = 1;
    auto relax_edges = [&](
            NodeID node, WeightT source_dist,
            auto&& before_property_load,
            auto&& after_property_load) {
        size_t edge_pos = 0;
        auto edges = graph.out_neigh(node);
        for (auto it = edges.begin(); it != edges.end(); ++it) {
            consume_edge();
            const WNode edge = *it;
            before_property_load(edge, edge_pos);
            const WeightT candidate = source_dist + edge.w;
            const WeightT old_dist =
                graphbrew_sniper::k2_bound_load(&dist[edge.v]);
            after_property_load(edge, edge_pos);
            if (candidate < old_dist) {
                dist[edge.v] = candidate;
                if (!in_queue[edge.v]) {
                    frontier.push(edge.v);
                    in_queue[edge.v] = 1;
                }
            }
            ++edge_pos;
        }
    };
    auto relax_compact_edges = [&](NodeID node, WeightT source_dist) {
        for (uint64_t pos = pair_off[node];
             pos < pair_off[node + 1]; ++pos) {
            consume_edge();
            const uint64_t record = pair_compact[pos];
            const NodeID dest = static_cast<NodeID>(
                ecg_epoch::extractCompactWeightedDest(record));
            const WeightT weight = static_cast<WeightT>(
                ecg_epoch::extractCompactWeightedWeight(record));
            if (software_k2_delivery) {
                deliver_k2_record(pair_flat[pos], fused_k2_model);
            }
            const WeightT candidate = source_dist + weight;
            const WeightT old_dist =
                graphbrew_sniper::k2_bound_load(&dist[dest]);
            if (candidate < old_dist) {
                dist[dest] = candidate;
                if (!in_queue[dest]) {
                    frontier.push(dest);
                    in_queue[dest] = 1;
                }
            }
        }
    };
    while (!frontier.empty()) {
        NodeID node = frontier.front();
        frontier.pop();
        in_queue[node] = 0;
        const WeightT source_dist = dist[node];
        SNIPER_SET_VERTEX(node);
        // ECG_PFX hint: emit the head of the frontier (next node to expand) so the
        // prefetcher can warm dist[next]. Env-gated.
        if (ecg_pfx_hints_on && !frontier.empty()) {
            SNIPER_ECG_PFX_TARGET(frontier.front());
        }
        if (fused_k2_model && pair_ok && compact_pair_ok) {
            relax_compact_edges(node, source_dist);
        } else if (fused_k2_model && pair_ok) {
            uint64_t pair_pos = pair_off[node];
            if (software_k2_delivery) {
                relax_edges(
                    node, source_dist,
                    [&](WNode edge, size_t) {
                    const uint32_t sidecar =
                        consume_fused_k2_sidecar(
                            &pair_sidecars[pair_pos]);
                    const uint64_t record = ecg_epoch::packEpochPairRecord(
                        static_cast<uint32_t>(edge.v),
                        ecg_epoch::extractWeightedEpochPairTier(sidecar),
                        ecg_epoch::extractWeightedEpochPairFirst(sidecar),
                        ecg_epoch::extractWeightedEpochPairSecond(sidecar));
                    ++pair_pos;
                    deliver_k2_record(record, fused_k2_model);
                    },
                    [](WNode, size_t) {});
            } else {
                relax_edges(
                    node, source_dist,
                    [&](WNode, size_t) {
                    (void)consume_fused_k2_sidecar(
                        &pair_sidecars[pair_pos++]);
                    },
                    [](WNode, size_t) {});
            }
        } else if (
                pair_ok ||
                (ecg_extract_on &&
                 static_cast<size_t>(node) < out_edge_epochs.size())) {
            const std::vector<uint16_t>* eps =
                (ecg_extract_on &&
                 static_cast<size_t>(node) < out_edge_epochs.size())
                    ? &out_edge_epochs[node] : nullptr;
            uint64_t delivered_k2_record = 0;
            bool delivered_k2 = false;
            relax_edges(
                node, source_dist,
                [&](WNode edge, size_t edge_pos) {
                if (pair_ok &&
                    static_cast<size_t>(node + 1) < pair_off.size()) {
                    delivered_k2_record =
                        pair_flat[pair_off[node] + edge_pos];
                    if (software_k2_delivery) {
                        deliver_k2_record(
                            delivered_k2_record, fused_k2_model);
                    }
                    delivered_k2 = true;
                } else if (eps) {
                    const uint16_t ep =
                        (edge_pos < eps->size()) ? (*eps)[edge_pos]
                        : static_cast<uint16_t>(ecg_epoch_count - 1);
                    SNIPER_ECG_EXTRACT(edge.v, ep);
                }
                },
                [&](WNode, size_t) {
                if (delivered_k2 && !fused_k2_model)
                    clear_k2_record(delivered_k2_record, fused_k2_model);
                delivered_k2 = false;
                });
        } else {
            relax_edges(
                node, source_dist,
                [](WNode, size_t) {}, [](WNode, size_t) {});
        }
    }
    finish_semantic_roi();
    };
    if (semantic_edges.enabled()) {
        SNIPER_ROI_BEGIN();
        try {
            execute_roi(
                [&] { semantic_edges.consume(); },
                [&] { semantic_edges.finish_roi(); });
        } catch (const SemanticEdgeLimitReached&) {
        }
    } else {
        SNIPER_ROI_BEGIN();
        execute_roi([] {}, [] {});
        SNIPER_ROI_END();
    }
    semantic_edges.report("sssp");

    int64_t reached = 0;
    uint64_t checksum = 0;
    for (WeightT value : dist) {
        if (value < kDistInf) {
            reached++;
            checksum += static_cast<uint64_t>(value);
        }
    }
    std::cout << "GraphBrew Sniper SG SSSP reached/checksum: "
              << static_cast<long long>(reached) << " / " << checksum << std::endl;
    return reached > 0 ? 0 : 1;
}

// ── CC (Afforest) union-find helpers — single-threaded (equivalence workload) ──
// Same logic as bench/src_sniper/cc.cc with the atomics removed (no CAS needed on
// one thread); the eviction DECISION is thread-count-agnostic.
void cc_link(NodeID u, NodeID v, pvector<NodeID>& comp) {
    NodeID p1 = comp[u];
    NodeID p2 = comp[v];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        NodeID p_high = comp[high];
        if (p_high == low) break;
        if (p_high == high) { comp[high] = low; break; }
        p1 = comp[comp[high]];
        p2 = comp[low];
    }
}

void cc_link_loaded(NodeID u, NodeID v, NodeID p2, pvector<NodeID>& comp) {
    NodeID p1 = comp[u];
    while (p1 != p2) {
        NodeID high = p1 > p2 ? p1 : p2;
        NodeID low = p1 + (p2 - high);
        NodeID p_high = comp[high];
        if (p_high == low) break;
        if (p_high == high) { comp[high] = low; break; }
        p1 = comp[comp[high]];
        p2 = comp[low];
    }
}

void cc_compress(const Graph& g, pvector<NodeID>& comp) {
    for (NodeID n = 0; n < g.num_nodes(); n++)
        while (comp[n] != comp[comp[n]])
            comp[n] = comp[comp[n]];
}

// Betweenness Centrality (Brandes) — single-threaded port of the audited
// Brandes_Sniper (bench/src_sniper/bc.cc): four grasp-protected vertex property
// regions (scores/depth/path_counts/deltas), transpose P-OPT reref matrix keyed on
// depth (BC pushes OUT-edges reading depth[dest] -> traverseCSR=false), and per-edge
// ECG epoch delivery. Mirrors the cache_sim/gem5 BC so the shared eviction decision
// is exercised identically across the three simulators.
int run_bc(const Graph& graph, int num_iters) {
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<ScoreT> scores(graph.num_nodes(), ScoreT(0), kPropAlign);
    pvector<int32_t> depth(graph.num_nodes(), int32_t(-1), kPropAlign);
    pvector<int64_t> path_counts(
        graph.num_nodes(), int64_t(0), kPropAlign);
    pvector<ScoreT> deltas(graph.num_nodes(), ScoreT(0), kPropAlign);

    SniperPropertyRegion regions[4] = {
        {"scores", reinterpret_cast<uint64_t>(scores.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(ScoreT),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(ScoreT), true},
        {"depth", reinterpret_cast<uint64_t>(depth.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(int32_t),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(int32_t), true},
        {"path_counts", reinterpret_cast<uint64_t>(path_counts.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(int64_t),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(int64_t), true},
        {"deltas", reinterpret_cast<uint64_t>(deltas.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(ScoreT),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(ScoreT), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(graph, edge_regions, 2, true);
    const int kNumVtxPerLine = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_VERTICES_PER_LINE",
        64 / sizeof(int32_t), 1, 1024);
    constexpr int kNumEpochs = 256;
    const int ecg_sched_k =
        graphbrew_sniper::env_int_clamped(
            "ECG_EDGE_MASK_SCHED", 0, 0, 4);
    pvector<uint8_t> popt_matrix;
    int popt_num_cache_lines = (graph.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    if (ecg_sched_k != 2 || popt_matrix_required()) {
        makeOffsetMatrix(
            graph, popt_matrix, kNumVtxPerLine, kNumEpochs,
            /*traverseCSR=*/false);
        sniper_export_popt_matrix(
            popt_matrix.data(), popt_num_cache_lines,
            kNumEpochs, graph.num_nodes());
    }

    const bool ecg_extract_on = graphbrew_sniper::ecg_extract_enabled();
    const bool fused_k2_model = fused_k2_model_enabled();
    const bool k2_transport_matched = k2_transport_matched_enabled();
    const bool k2_trace_on = graphbrew_sniper::ecg_k2_trace_enabled();
    const bool software_k2_delivery =
        !fused_k2_model || k2_trace_on;
    const bool no_delivery_pair_loop =
        !software_k2_delivery || (k2_transport_matched && !k2_trace_on);
    const bool stream_bypass_on = stream_bypass_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", kNumEpochs, 2, 65535));
    if (ecg_sched_k == 2 || k2_transport_matched)
        ecg_epoch_count =
            ecg_epoch::normalizeK2EpochCount(ecg_epoch_count);
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_sched_k != 2) {
        ecg_epoch::buildInEdgeEpochs(graph, kNumVtxPerLine, ecg_epoch_count,
                                     /*linemin=*/true, out_edge_epochs,
                                     /*push_out_edges=*/true);
    }
    K2PairStream pairs;
    bool pair_ok = false;
    if ((ecg_extract_on && ecg_sched_k == 2) ||
        k2_transport_matched) {
        pair_ok = build_k2_pair_stream(
            graph, kNumVtxPerLine, ecg_epoch_count,
            /*push_out_edges=*/true, "bc", pairs);
        if (!pair_ok) return 2;
    }
    if (!sniper_export_context(
            regions, 4, graph, nullptr, edge_regions, num_edge_regions,
            stream_bypass_on && pair_ok
                ? pairs.stream_base() : 0,
            stream_bypass_on && pair_ok
                ? pairs.stream_bytes() : 0,
            fused_k2_model && pair_ok ? pairs.offsets.data() : nullptr,
            fused_k2_model && pair_ok ? pairs.offsets.size() : 0,
            fused_k2_model && pair_ok
                ? pairs.wide_records.data() : nullptr,
            fused_k2_model && pair_ok
                ? pairs.wide_records.size() : 0)) {
        std::fprintf(stderr, "sniper-sg BC: context/K2 sideband export failed\n");
        return 2;
    }
    auto deliver = [&](NodeID u, size_t edge_pos, NodeID v) {
        if (!ecg_extract_on || static_cast<size_t>(u) >= out_edge_epochs.size()) return;
        const auto& eps = out_edge_epochs[u];
        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                      : static_cast<uint16_t>(ecg_epoch_count - 1);
        SNIPER_ECG_EXTRACT(v, ep);
    };
    if (pair_ok)
        std::fprintf(
            stderr,
            fused_k2_model
                ? "[ECG_FUSED_K2] BC Schedule-2 fused sideband ACTIVE\n"
                : "[ECG_PACKED8_K2] BC Schedule-2 packed record path ACTIVE\n");
    if (pair_ok && k2_transport_matched)
        std::fprintf(
            stderr,
            "[K2_TRANSPORT_MATCHED] BC %uB record loop ACTIVE\n",
            pairs.record_bytes());
    if (pair_ok && k2_transport_matched &&
        graphbrew_sniper::k2_exact_bind_enabled())
        std::fprintf(stderr,
                     "[K2_EXACT_BIND] BC edge-governed depth/path_counts[dest] binding ACTIVE\n");

    if (num_iters < 1) num_iters = 1;
    SemanticEdgeBudget semantic_edges;
    auto execute_roi = [&](auto&& consume_edge, auto&& finish_semantic_roi) {
    for (int iter = 0; iter < num_iters; iter++) {
        NodeID source = static_cast<NodeID>(iter % graph.num_nodes());
        for (NodeID n = 0; n < graph.num_nodes(); n++) {
            depth[n] = -1; path_counts[n] = 0; deltas[n] = 0;
        }
        depth[source] = 0;
        path_counts[source] = 1;

        // Forward BFS, single-threaded, recording per-level frontiers.
        std::vector<std::vector<NodeID>> levels;
        levels.push_back(std::vector<NodeID>{source});
        int cur_level = 0;
        while (!levels[cur_level].empty()) {
            std::vector<NodeID> next_level;
            for (NodeID u : levels[cur_level]) {
                const int64_t source_paths = path_counts[u];
                SNIPER_SET_VERTEX(u);
                auto visit = [&](NodeID v, int32_t depth_v) {
                    if (depth_v == -1) {
                        depth[v] = cur_level + 1;
                        depth_v = cur_level + 1;
                        next_level.push_back(v);
                    }
                    if (depth_v == cur_level + 1)
                        path_counts[v] += source_paths;
                };
                if (pair_ok &&
                    static_cast<size_t>(u + 1) < pairs.offsets.size()) {
                    if (no_delivery_pair_loop) {
                        for (uint64_t pos = pairs.offsets[u];
                             pos < pairs.offsets[u + 1]; ++pos) {
                            consume_edge();
                            const uint64_t record = pairs.record(pos);
                            const NodeID v = static_cast<NodeID>(
                                ecg_epoch::extractEpochPairDest(record));
                            int32_t depth_v =
                                graphbrew_sniper::k2_bound_load(&depth[v]);
                            if (depth_v == -1) {
                                depth[v] = cur_level + 1;
                                depth_v = cur_level + 1;
                                next_level.push_back(v);
                            }
                            if (depth_v == cur_level + 1) {
                                const int64_t old_paths =
                                    graphbrew_sniper::k2_bound_load(
                                        &path_counts[v]);
                                path_counts[v] = old_paths + source_paths;
                            }
                        }
                    } else {
                        for (uint64_t pos = pairs.offsets[u];
                             pos < pairs.offsets[u + 1]; ++pos) {
                            consume_edge();
                            const uint64_t record = pairs.record(pos);
                            const NodeID v = static_cast<NodeID>(
                                ecg_epoch::extractEpochPairDest(record));
                            deliver_k2_record(record, fused_k2_model);
                            int32_t depth_v =
                                graphbrew_sniper::k2_bound_load(&depth[v]);
                            if (depth_v == -1) {
                                depth[v] = cur_level + 1;
                                depth_v = cur_level + 1;
                                next_level.push_back(v);
                            }
                            if (depth_v == cur_level + 1) {
                                const int64_t old_paths =
                                    graphbrew_sniper::k2_bound_load(
                                        &path_counts[v]);
                                path_counts[v] = old_paths + source_paths;
                            }
                            if (!fused_k2_model) {
                                clear_k2_record(record, fused_k2_model);
                            }
                        }
                    }
                } else {
                    size_t edge_pos = 0;
                    auto neighbors = graph.out_neigh(u);
                    for (auto it = neighbors.begin();
                         it != neighbors.end(); ++it) {
                        consume_edge();
                        const NodeID v = *it;
                        deliver(u, edge_pos, v);
                        ++edge_pos;
                        visit(v, depth[v]);
                    }
                }
            }
            if (next_level.empty()) break;
            levels.push_back(std::move(next_level));
            cur_level++;
        }

        // Backward dependency accumulation, deepest level first.
        SNIPER_CLEAR_VERTEX();
        for (int d = static_cast<int>(levels.size()) - 1; d > 0; d--) {
            for (NodeID w : levels[d]) {
                ScoreT delta_w = 0;
                auto neighbors = graph.out_neigh(w);
                for (auto it = neighbors.begin();
                     it != neighbors.end(); ++it) {
                    consume_edge();
                    const NodeID v = *it;
                    if (depth[v] == depth[w] + 1)
                        delta_w += static_cast<ScoreT>(path_counts[w]) /
                                   path_counts[v] * (1.0f + deltas[v]);
                }
                deltas[w] = delta_w;
                if (w != source) scores[w] += delta_w;
            }
        }
    }
    finish_semantic_roi();
    };
    if (semantic_edges.enabled()) {
        SNIPER_ROI_BEGIN();
        try {
            execute_roi(
                [&] { semantic_edges.consume(); },
                [&] { semantic_edges.finish_roi(); });
        } catch (const SemanticEdgeLimitReached&) {
        }
    } else {
        SNIPER_ROI_BEGIN();
        execute_roi([] {}, [] {});
        SNIPER_ROI_END();
    }
    semantic_edges.report("bc");

    double checksum = 0;
    for (ScoreT s : scores) checksum += s;
    std::cout << "GraphBrew Sniper SG BC checksum: " << checksum << std::endl;
    return graph.num_nodes() > 0 ? 0 : 1;
}

// Connected Components (Afforest) — single-threaded port of the audited
// Afforest_Sniper (bench/src_sniper/cc.cc): one grasp-protected comp[] region,
// transpose P-OPT reref matrix (CC reads comp[dest] over OUT-edges -> traverseCSR=
// false), and per-edge ECG epoch delivery. CC is the documented DO-NO-HARM cell
// (low property reuse, ECG ~= GRASP), certified for policy-compliance not a win.
int run_cc(const Graph& graph, int neighbor_rounds) {
    if (neighbor_rounds < 1) neighbor_rounds = 2;
    const size_t kPropAlign = graphbrew_sniper::property_alignment();
    pvector<NodeID> comp(graph.num_nodes(), NodeID(0), kPropAlign);
    for (NodeID n = 0; n < graph.num_nodes(); n++) comp[n] = n;

    SniperPropertyRegion regions[1] = {
        {"comp", reinterpret_cast<uint64_t>(comp.data()),
         static_cast<uint64_t>(graph.num_nodes()) * sizeof(NodeID),
         static_cast<uint32_t>(graph.num_nodes()), sizeof(NodeID), true},
    };
    SniperEdgeRegion edge_regions[2];
    int num_edge_regions = sniper_make_edge_regions(graph, edge_regions, 2, true);
    const int kNumVtxPerLine = graphbrew_sniper::env_int_clamped(
        "SNIPER_ECG_VERTICES_PER_LINE",
        64 / sizeof(NodeID), 1, 1024);
    constexpr int kNumEpochs = 256;
    const int ecg_sched_k =
        graphbrew_sniper::env_int_clamped(
            "ECG_EDGE_MASK_SCHED", 0, 0, 4);
    pvector<uint8_t> popt_matrix;
    int popt_num_cache_lines = (graph.num_nodes() + kNumVtxPerLine - 1) / kNumVtxPerLine;
    if (ecg_sched_k != 2 || popt_matrix_required()) {
        makeOffsetMatrix(
            graph, popt_matrix, kNumVtxPerLine, kNumEpochs,
            /*traverseCSR=*/false);
        sniper_export_popt_matrix(
            popt_matrix.data(), popt_num_cache_lines,
            kNumEpochs, graph.num_nodes());
    }

    const bool ecg_extract_on = graphbrew_sniper::ecg_extract_enabled();
    const bool fused_k2_model = fused_k2_model_enabled();
    const bool k2_transport_matched = k2_transport_matched_enabled();
    const bool k2_trace_on = graphbrew_sniper::ecg_k2_trace_enabled();
    const bool software_k2_delivery =
        !fused_k2_model || k2_trace_on;
    const bool no_delivery_pair_loop =
        !software_k2_delivery || (k2_transport_matched && !k2_trace_on);
    const bool stream_bypass_on = stream_bypass_enabled();
    uint32_t ecg_epoch_count = static_cast<uint32_t>(
        graphbrew_sniper::env_int_clamped("ECG_EDGE_MASK_EPOCHS", kNumEpochs, 2, 65535));
    if (ecg_sched_k == 2 || k2_transport_matched)
        ecg_epoch_count =
            ecg_epoch::normalizeK2EpochCount(ecg_epoch_count);
    std::vector<std::vector<uint16_t>> out_edge_epochs;
    if (ecg_extract_on && ecg_sched_k != 2) {
        ecg_epoch::buildInEdgeEpochs(graph, kNumVtxPerLine, ecg_epoch_count,
                                     /*linemin=*/true, out_edge_epochs,
                                     /*push_out_edges=*/true);
    }
    K2PairStream pairs;
    bool pair_ok = false;
    if ((ecg_extract_on && ecg_sched_k == 2) ||
        k2_transport_matched) {
        pair_ok = build_k2_pair_stream(
            graph, kNumVtxPerLine, ecg_epoch_count,
            /*push_out_edges=*/true, "cc", pairs);
        if (!pair_ok) return 2;
    }
    if (!sniper_export_context(
            regions, 1, graph, nullptr, edge_regions, num_edge_regions,
            stream_bypass_on && pair_ok
                ? pairs.stream_base() : 0,
            stream_bypass_on && pair_ok
                ? pairs.stream_bytes() : 0,
            fused_k2_model && pair_ok ? pairs.offsets.data() : nullptr,
            fused_k2_model && pair_ok ? pairs.offsets.size() : 0,
            fused_k2_model && pair_ok
                ? pairs.wide_records.data() : nullptr,
            fused_k2_model && pair_ok
                ? pairs.wide_records.size() : 0)) {
        std::fprintf(stderr, "sniper-sg CC: context/K2 sideband export failed\n");
        return 2;
    }
    auto deliver = [&](NodeID u, size_t edge_pos, NodeID v) {
        if (!ecg_extract_on || static_cast<size_t>(u) >= out_edge_epochs.size()) return;
        const auto& eps = out_edge_epochs[u];
        uint16_t ep = (edge_pos < eps.size()) ? eps[edge_pos]
                      : static_cast<uint16_t>(ecg_epoch_count - 1);
        SNIPER_ECG_EXTRACT(v, ep);
    };
    if (pair_ok)
        std::fprintf(
            stderr,
            fused_k2_model
                ? "[ECG_FUSED_K2] CC Schedule-2 fused sideband ACTIVE\n"
                : "[ECG_PACKED8_K2] CC Schedule-2 packed record path ACTIVE\n");
    if (pair_ok && k2_transport_matched)
        std::fprintf(
            stderr,
            "[K2_TRANSPORT_MATCHED] CC %uB record loop ACTIVE\n",
            pairs.record_bytes());
    if (pair_ok && k2_transport_matched &&
        graphbrew_sniper::k2_exact_bind_enabled())
        std::fprintf(stderr,
                     "[K2_EXACT_BIND] CC comp[dest] binding ACTIVE\n");

    SemanticEdgeBudget semantic_edges;
    std::unordered_map<NodeID, int64_t> count;
    auto execute_roi = [&](auto&& consume_edge, auto&& finish_semantic_roi) {
    // Phase 1: sample the r-th out-neighbour of each vertex, compress.
    for (int r = 0; r < neighbor_rounds; r++) {
        for (NodeID u = 0; u < graph.num_nodes(); u++) {
            SNIPER_SET_VERTEX(u);
            if (pair_ok &&
                static_cast<size_t>(u + 1) < pairs.offsets.size() &&
                pairs.offsets[u] + static_cast<uint64_t>(r) <
                    pairs.offsets[u + 1]) {
                consume_edge();
                const uint64_t record =
                    pairs.record(
                        pairs.offsets[u] + static_cast<uint64_t>(r));
                const NodeID v = static_cast<NodeID>(
                    ecg_epoch::extractEpochPairDest(record));
                if (no_delivery_pair_loop) {
                    const NodeID delivered_comp =
                        graphbrew_sniper::k2_bound_load(&comp[v]);
                    cc_link_loaded(u, v, delivered_comp, comp);
                } else {
                    deliver_k2_record(record, fused_k2_model);
                    const NodeID delivered_comp =
                        graphbrew_sniper::k2_bound_load(&comp[v]);
                    if (!fused_k2_model) {
                        clear_k2_record(record, fused_k2_model);
                    }
                    cc_link_loaded(u, v, delivered_comp, comp);
                }
            } else if (!pair_ok) {
                auto out_neigh = graph.out_neigh(u);
                auto it = out_neigh.begin();
                for (int i = 0; i < r && it != out_neigh.end(); ++i, ++it) {}
                if (it != out_neigh.end()) {
                    consume_edge();
                    deliver(u, static_cast<size_t>(r), *it);
                    cc_link(u, *it, comp);
                }
            }
        }
        SNIPER_CLEAR_VERTEX();
        cc_compress(graph, comp);
    }

    // Most frequent component = the giant component skipped in phase 2.
    for (NodeID n = 0; n < graph.num_nodes(); n++) count[comp[n]]++;
    NodeID largest = graph.num_nodes() > 0 ? comp[0] : 0;
    int64_t largest_count = -1;
    for (const auto& kv : count) {
        if (kv.second > largest_count) { largest_count = kv.second; largest = kv.first; }
    }

    // Phase 2: full traversal for vertices outside the giant component.
    for (NodeID u = 0; u < graph.num_nodes(); u++) {
        if (comp[u] == largest) continue;
        SNIPER_SET_VERTEX(u);
        if (pair_ok && static_cast<size_t>(u + 1) < pairs.offsets.size()) {
            if (no_delivery_pair_loop) {
                for (uint64_t pos = pairs.offsets[u];
                     pos < pairs.offsets[u + 1]; ++pos) {
                    consume_edge();
                    const uint64_t record = pairs.record(pos);
                    const NodeID v = static_cast<NodeID>(
                        ecg_epoch::extractEpochPairDest(record));
                    const NodeID delivered_comp =
                        graphbrew_sniper::k2_bound_load(&comp[v]);
                    cc_link_loaded(u, v, delivered_comp, comp);
                }
            } else {
                for (uint64_t pos = pairs.offsets[u];
                     pos < pairs.offsets[u + 1]; ++pos) {
                    consume_edge();
                    const uint64_t record = pairs.record(pos);
                    const NodeID v = static_cast<NodeID>(
                        ecg_epoch::extractEpochPairDest(record));
                    deliver_k2_record(record, fused_k2_model);
                    const NodeID delivered_comp =
                        graphbrew_sniper::k2_bound_load(&comp[v]);
                    if (!fused_k2_model) {
                        clear_k2_record(record, fused_k2_model);
                    }
                    cc_link_loaded(u, v, delivered_comp, comp);
                }
            }
        } else {
            size_t edge_pos = 0;
            auto neighbors = graph.out_neigh(u);
            for (auto it = neighbors.begin(); it != neighbors.end(); ++it) {
                consume_edge();
                const NodeID v = *it;
                deliver(u, edge_pos, v);
                ++edge_pos;
                cc_link(u, v, comp);
            }
        }
    }
    SNIPER_CLEAR_VERTEX();
    cc_compress(graph, comp);
    finish_semantic_roi();
    };
    if (semantic_edges.enabled()) {
        SNIPER_ROI_BEGIN();
        try {
            execute_roi(
                [&] { semantic_edges.consume(); },
                [&] { semantic_edges.finish_roi(); });
        } catch (const SemanticEdgeLimitReached&) {
        }
    } else {
        SNIPER_ROI_BEGIN();
        execute_roi([] {}, [] {});
        SNIPER_ROI_END();
    }
    semantic_edges.report("cc");

    int64_t num_comps = 0;
    for (NodeID n = 0; n < graph.num_nodes(); n++)
        if (comp[n] == n) num_comps++;
    std::cout << "GraphBrew Sniper SG CC components: "
              << static_cast<long long>(num_comps) << std::endl;
    return graph.num_nodes() > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parse_options(argc, argv);
    if (options.graph_path.empty() && options.scale < 1) {
        std::cerr << "sg_kernel requires -f graph.sg or -g scale" << std::endl;
        return 2;
    }

    if (options.benchmark == "pr") {
        Graph graph = load_graph(options);
        return run_pr(graph, options.max_iters);
    }
    if (options.benchmark == "bfs") {
        Graph graph = load_graph(options);
        return run_bfs(graph, options.source);
    }
    if (options.benchmark == "sssp") {
        WGraph graph = load_weighted_graph(options);
        return run_sssp(graph, options.source, options.delta);
    }

    if (options.benchmark == "bc") {
        Graph graph = load_graph(options);
        return run_bc(graph, options.max_iters);
    }
    if (options.benchmark == "cc") {
        Graph graph = load_graph(options);
        return run_cc(graph, /*neighbor_rounds=*/2);
    }

    std::cerr << "unsupported sg_kernel benchmark: " << options.benchmark << std::endl;
    return 2;
}
