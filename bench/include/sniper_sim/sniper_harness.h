// GraphBrew Sniper harness scaffolding.
//
// This header mirrors the gem5 harness shape but remains safe to include in
// native builds before Sniper is present. When Sniper's sim_api.h is available,
// ROI macros call SimRoiStart/SimRoiEnd. Sideband export helpers intentionally
// use plain files so early Sniper work can share cache_sim/gem5 metadata formats.

#pragma once

#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>

#include "ecg_epoch_builder.h"
#include <graph.h>
#include <pvector.h>

#if defined(__has_include)
#  if __has_include("sim_api.h")
#    include "sim_api.h"
#    define GRAPHBREW_SNIPER_HAS_SIM_API 1
#  endif
#endif

#ifndef GRAPHBREW_SNIPER_HAS_SIM_API
#  define GRAPHBREW_SNIPER_HAS_SIM_API 0
#endif

namespace graphbrew_sniper {

constexpr uint64_t GRAPHBREW_SNIPER_USER_SET_VERTEX = 0x47525654ULL;  // "GRVT"
constexpr uint64_t GRAPHBREW_SNIPER_USER_CONTEXT_READY = 0x47524358ULL;  // "GRCX"
constexpr uint64_t GRAPHBREW_SNIPER_USER_POPT_READY = 0x47504f50ULL;  // "GPOP"
constexpr uint64_t GRAPHBREW_SNIPER_USER_ECG_PFX_TARGET = 0x47504658ULL;  // "GPFX"
constexpr uint64_t GRAPHBREW_SNIPER_USER_ECG_EXTRACT = 0x47464C44ULL;  // ECG epoch-extract delivery
constexpr uint64_t GRAPHBREW_SNIPER_USER_ECG_EXTRACT2 = 0x47464C45ULL; // dest + two epochs
constexpr uint64_t GRAPHBREW_SNIPER_USER_K2_BIND = 0x4B32424EULL;  // "K2BN"
constexpr uint64_t GRAPHBREW_SNIPER_USER_K2_CLEAR = 0x4B324243ULL; // "K2BC"

inline const char* env_or_default(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : fallback;
}

inline int env_int_clamped(const char* name, int fallback, int min_value, int max_value) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return static_cast<int>(parsed);
}

inline size_t cache_line_size() {
    return static_cast<size_t>(
        env_int_clamped("SNIPER_CACHE_LINE_SIZE", 64, 1, 4096));
}

inline size_t property_alignment() {
    const size_t alignment = static_cast<size_t>(
        env_int_clamped("SNIPER_PROPERTY_ALIGNMENT", 4096, 64, 65536));
    if (alignment < cache_line_size() ||
        alignment < sizeof(void*) ||
        (alignment & (alignment - 1)) != 0) {
        std::fprintf(stderr,
            "sniper_harness: property alignment %zu is invalid\n",
            alignment);
        std::abort();
    }
    return alignment;
}

inline std::string context_path() {
    return env_or_default("SNIPER_GRAPHBREW_CTX", "/tmp/sniper_graphbrew_ctx.json");
}

inline std::string popt_matrix_path() {
    return env_or_default("SNIPER_POPT_MATRIX", "/tmp/sniper_popt_matrix.bin");
}

inline std::string out_edges_path() {
    return env_or_default("SNIPER_GRAPHBREW_OUT_EDGES", "/tmp/sniper_graphbrew_out_edges.bin");
}

inline std::string in_edges_path() {
    return env_or_default("SNIPER_GRAPHBREW_IN_EDGES", "/tmp/sniper_graphbrew_in_edges.bin");
}

inline void notify_user(uint64_t command, uint64_t argument) {
#if GRAPHBREW_SNIPER_HAS_SIM_API
    SimUser(command, argument);
#else
    (void)command;
    (void)argument;
#endif
}

inline bool k2_exact_bind_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SNIPER_K2_EXACT_BIND");
        return value && value[0] && std::string(value) != "0";
    }();
    return enabled;
}

template <typename T>
inline T k2_bound_load(const T* address) {
    if (!k2_exact_bind_enabled()) return *address;
    asm volatile("" ::: "memory");
    notify_user(
        GRAPHBREW_SNIPER_USER_K2_BIND,
        reinterpret_cast<uint64_t>(address));
    asm volatile("" ::: "memory");
    const T value = *address;
    asm volatile("" ::: "memory");
    notify_user(GRAPHBREW_SNIPER_USER_K2_CLEAR, 0);
    asm volatile("" ::: "memory");
    return value;
}

inline void notify_context_ready() {
    const std::string path = context_path();
    notify_user(
        GRAPHBREW_SNIPER_USER_CONTEXT_READY,
        reinterpret_cast<uint64_t>(path.c_str()));
}

inline bool hints_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SNIPER_ENABLE_VERTEX_HINTS");
        return value && value[0] && std::string(value) != "0";
    }();
    return enabled;
}

inline bool ecg_pfx_hints_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SNIPER_ENABLE_ECG_PFX_HINTS");
        if (!value) {
            value = std::getenv("ECG_PREFETCH");
        }
        return value && value[0] && std::string(value) != "0";
    }();
    return enabled;
}

inline bool ecg_extract_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("SNIPER_ENABLE_ECG_EXTRACT");
        return value && value[0] && std::string(value) != "0";
    }();
    return enabled;
}

inline bool should_emit_ecg_pfx_hint(uint64_t vertex_id) {
    static const int capacity =
        env_int_clamped("SNIPER_ECG_PFX_HINT_FILTER", 16, 0, 64);
    if (capacity == 0) {
        return true;
    }
    static const uint64_t vertices_per_line = []() {
        const int elem_size =
            env_int_clamped("SNIPER_ECG_PFX_FILTER_ELEM_SIZE", 4, 1, 64);
        const int line_size =
            env_int_clamped("SNIPER_ECG_PFX_FILTER_LINE_SIZE", 64, 1, 4096);
        const uint64_t value = static_cast<uint64_t>(line_size / elem_size);
        return value == 0 ? 1 : value;
    }();
    uint64_t filter_key = vertex_id / vertices_per_line;
    thread_local uint64_t recent[64] = {};
    thread_local int count = 0;
    thread_local int next = 0;
    for (int i = 0; i < count; ++i) {
        if (recent[i] == filter_key) {
            return false;
        }
    }
    recent[next] = filter_key;
    next = (next + 1) % capacity;
    if (count < capacity) {
        ++count;
    }
    return true;
}

inline void roi_begin() {
#if GRAPHBREW_SNIPER_HAS_SIM_API
    SimRoiStart();
#endif
}

inline void roi_end() {
#if GRAPHBREW_SNIPER_HAS_SIM_API
    SimRoiEnd();
#endif
}

inline void set_vertex(uint64_t vertex_id) {
    if (!hints_enabled()) {
        return;
    }
    notify_user(GRAPHBREW_SNIPER_USER_SET_VERTEX, vertex_id);
}

inline void clear_vertex() {
    if (!hints_enabled()) {
        return;
    }
    notify_user(GRAPHBREW_SNIPER_USER_SET_VERTEX, ~uint64_t(0));
}

inline void set_prefetch_target(uint64_t vertex_id) {
    if (!ecg_pfx_hints_enabled()) {
        return;
    }
    if (!should_emit_ecg_pfx_hint(vertex_id)) {
        return;
    }
    notify_user(GRAPHBREW_SNIPER_USER_ECG_PFX_TARGET, vertex_id);
}

inline void ecg_extract(uint64_t vertex, uint16_t epoch) {
    if (!ecg_extract_enabled()) {
        return;
    }
    // Sniper's magic user payload reliably carries 48 bits. Keep the complete
    // 32-bit NodeID plus 16-bit epoch inside bits [0:47]; the previous
    // vertex[47:0]|epoch[63:48] layout silently dropped every nonzero epoch.
    uint64_t packed = (vertex & 0xFFFFFFFFULL) |
                      (static_cast<uint64_t>(epoch) << 32);
    notify_user(GRAPHBREW_SNIPER_USER_ECG_EXTRACT, packed);
}

inline uint64_t ecg_k2_trace_limit() {
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_K2_DELIVERY_TRACE");
        return value ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) : 0;
    }();
    return trace_limit;
}

inline bool ecg_k2_trace_enabled() {
    return ecg_k2_trace_limit() > 0;
}

inline void trace_ecg_extract2(
        uint32_t vertex, uint8_t tier,
        uint16_t first, uint16_t second) {
    const uint64_t trace_limit = ecg_k2_trace_limit();
    if (trace_limit == 0) return;
    static std::atomic<uint64_t> trace_sequence{0};
    const uint64_t sequence =
        trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence < trace_limit) {
        std::fprintf(stderr,
            "[ECG-K2-EXPECT sim=sniper seq=%llu dest=%u tier=%u "
            "epoch1=%u epoch2=%u]\n",
            (unsigned long long)sequence, vertex,
            static_cast<unsigned>(tier),
            static_cast<unsigned>(first), static_cast<unsigned>(second));
    }
}

inline void ecg_extract2(
        uint32_t vertex, uint8_t tier,
        uint16_t first, uint16_t second) {
    if (!ecg_extract_enabled()) return;
    trace_ecg_extract2(vertex, tier, first, second);
    // SimMagic2 is 64-bit safe after the early-clobber fix.
    const uint64_t packed =
        ecg_epoch::packEpochPairRecord(vertex, tier, first, second);
    notify_user(GRAPHBREW_SNIPER_USER_ECG_EXTRACT2, packed);
}

inline void ecg_clear_extract2(uint32_t vertex) {
    if (!ecg_extract_enabled()) return;
    const uint64_t clear_record =
        ecg_epoch::packEpochPairRecord(vertex, 0, 0, 0);
    notify_user(GRAPHBREW_SNIPER_USER_ECG_EXTRACT2, clear_record);
}

inline void write_minimal_context(uint64_t vertices, uint64_t edges) {
    const std::string path = context_path();
    std::ofstream out(path);
    if (!out) {
        return;
    }
    out << "{\n";
    out << "  \"format\": \"graphbrew-sniper-context-v0\",\n";
    out << "  \"vertices\": " << vertices << ",\n";
    out << "  \"edges\": " << edges << "\n";
    out << "}\n";
    notify_context_ready();
}

}  // namespace graphbrew_sniper

#define SNIPER_MAX_REGIONS 8

struct SniperPropertyRegion {
    const char* name;
    uint64_t base_address;
    uint64_t size_bytes;
    uint32_t num_elements;
    uint32_t elem_size;
    bool grasp_region = true;
};

struct SniperEdgeRegion {
    const char* name;
    uint64_t base_address;
    uint64_t size_bytes;
    uint32_t elem_size;
    const void* data = nullptr;
    const char* data_path = nullptr;
};

template<typename GraphType>
inline int sniper_make_edge_regions(const GraphType& g,
                                    SniperEdgeRegion* edge_regions,
                                    int max_edge_regions,
                                    bool prefer_in_edges = false) {
    if (max_edge_regions < 2 || g.num_nodes() == 0 || g.num_edges_directed() == 0) {
        return 0;
    }

    auto out0 = g.out_neigh(0);
    auto in0 = g.in_neigh(0);
    const void* out_base = static_cast<const void*>(out0.begin());
    const void* in_base = static_cast<const void*>(in0.begin());
    const uint64_t edge_count = static_cast<uint64_t>(g.num_edges_directed());
    const uint32_t out_elem_size = static_cast<uint32_t>(sizeof(*out0.begin()));
    const uint32_t in_elem_size = static_cast<uint32_t>(sizeof(*in0.begin()));

    SniperEdgeRegion out_region = {
        "out_edges", reinterpret_cast<uint64_t>(out_base),
        edge_count * out_elem_size, out_elem_size, out_base
    };
    SniperEdgeRegion in_region = {
        "in_edges", reinterpret_cast<uint64_t>(in_base),
        edge_count * in_elem_size, in_elem_size, in_base
    };

    edge_regions[0] = prefer_in_edges ? in_region : out_region;
    edge_regions[1] = prefer_in_edges ? out_region : in_region;
    return 2;
}

inline bool sniper_write_binary_atomic(
    const std::string& path, const void* data, size_t elem_size, size_t count) {
    const std::string temp_path = path + ".tmp";
    FILE* output = fopen(temp_path.c_str(), "wb");
    if (!output) return false;

    const bool wrote_all =
        fwrite(data, elem_size, count, output) == count;
    const bool flushed = fflush(output) == 0 && ferror(output) == 0;
    const bool closed = fclose(output) == 0;
    if (wrote_all && flushed && closed &&
        std::rename(temp_path.c_str(), path.c_str()) == 0) {
        return true;
    }
    std::remove(temp_path.c_str());
    return false;
}

template<typename GraphType>
inline bool sniper_export_context(
    const SniperPropertyRegion* regions, int num_regions,
    const GraphType& g,
    const char* path = nullptr,
    const SniperEdgeRegion* edge_regions = nullptr,
    int num_edge_regions = 0,
    uint64_t stream_bypass_base = 0,
    uint64_t stream_bypass_size = 0,
    const uint64_t* k2_offsets = nullptr,
    size_t k2_offset_count = 0,
    const uint64_t* k2_records = nullptr,
    size_t k2_record_count = 0) {
    const std::string resolved_path = path ? std::string(path) : graphbrew_sniper::context_path();
    if (stream_bypass_size > 0) {
        const uint64_t line_size =
            static_cast<uint64_t>(graphbrew_sniper::cache_line_size());
        if (stream_bypass_base > UINT64_MAX - stream_bypass_size) {
            fprintf(stderr, "sniper_harness: stream bypass range overflow\n");
            return false;
        }
        const uint64_t raw_upper = stream_bypass_base + stream_bypass_size;
        const uint64_t base_remainder = stream_bypass_base % line_size;
        const uint64_t base_padding =
            base_remainder == 0 ? 0 : line_size - base_remainder;
        if (stream_bypass_base > UINT64_MAX - base_padding) {
            fprintf(stderr, "sniper_harness: aligned stream bypass base overflow\n");
            return false;
        }
        const uint64_t aligned_base = stream_bypass_base + base_padding;
        const uint64_t aligned_upper = raw_upper - (raw_upper % line_size);
        stream_bypass_base = aligned_base;
        stream_bypass_size = aligned_upper > aligned_base
            ? aligned_upper - aligned_base : 0;
    }

    std::string k2_offsets_path;
    std::string k2_records_path;
    const bool k2_requested =
        k2_offsets || k2_offset_count || k2_records || k2_record_count;
    if (k2_requested) {
        if (!k2_offsets || k2_offset_count == 0 ||
            !k2_records || k2_record_count == 0) {
            fprintf(stderr, "sniper_harness: incomplete K2 sideband inputs\n");
            return false;
        }
        k2_offsets_path = resolved_path + ".k2_offsets.bin";
        k2_records_path = resolved_path + ".k2_records.bin";
        const bool offsets_ok = sniper_write_binary_atomic(
            k2_offsets_path, k2_offsets, sizeof(uint64_t), k2_offset_count);
        const bool records_ok = sniper_write_binary_atomic(
            k2_records_path, k2_records, sizeof(uint64_t), k2_record_count);
        if (!offsets_ok || !records_ok) {
            std::remove(k2_offsets_path.c_str());
            std::remove(k2_records_path.c_str());
            fprintf(stderr, "sniper_harness: cannot publish complete K2 sidebands\n");
            return false;
        }
    }

    const std::string temp_context_path = resolved_path + ".tmp";
    FILE* f = fopen(temp_context_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "sniper_harness: cannot write sideband to %s\n", resolved_path.c_str());
        std::remove(k2_offsets_path.c_str());
        std::remove(k2_records_path.c_str());
        return false;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"num_vertices\": %ld,\n", (long)g.num_nodes());
    fprintf(f, "  \"num_edges\": %ld,\n", (long)g.num_edges_directed());
    fprintf(f, "  \"stream_bypass_base\": %lu,\n",
            (unsigned long)stream_bypass_base);
    fprintf(f, "  \"stream_bypass_size\": %lu,\n",
            (unsigned long)stream_bypass_size);
    if (stream_bypass_size > 0) {
        fprintf(stderr,
            "[ECG-STREAM-REGION sim=sniper base=%#lx size=%lu]\n",
            (unsigned long)stream_bypass_base,
            (unsigned long)stream_bypass_size);
    }
    fprintf(f, "  \"k2_offsets_path\": \"%s\",\n",
            k2_offsets_path.c_str());
    fprintf(f, "  \"k2_records_path\": \"%s\",\n",
            k2_records_path.c_str());
    fprintf(f, "  \"directed\": %s,\n", g.directed() ? "true" : "false");

    fprintf(f, "  \"property_regions\": [\n");
    for (int i = 0; i < num_regions; i++) {
        fprintf(f, "    {\"name\": \"%s\", \"base\": %lu, \"size\": %lu, "
            "\"count\": %u, \"elem_size\": %u, \"grasp\": %s}%s\n",
                regions[i].name,
                (unsigned long)regions[i].base_address,
                (unsigned long)regions[i].size_bytes,
                regions[i].num_elements,
                regions[i].elem_size,
            regions[i].grasp_region ? "true" : "false",
                (i < num_regions - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"edge_regions\": [\n");
    for (int i = 0; i < num_edge_regions; i++) {
        char default_data_path[256];
        const char* data_path = edge_regions[i].data_path;
        if (!data_path && edge_regions[i].data && edge_regions[i].size_bytes > 0) {
            snprintf(default_data_path, sizeof(default_data_path),
                     "/tmp/sniper_graphbrew_%s.bin", edge_regions[i].name);
            data_path = default_data_path;
        }
        if (data_path && edge_regions[i].data && edge_regions[i].size_bytes > 0) {
            if (!sniper_write_binary_atomic(
                    data_path, edge_regions[i].data, 1,
                    static_cast<size_t>(edge_regions[i].size_bytes))) {
                fprintf(stderr, "sniper_harness: cannot write edge data to %s\n", data_path);
                data_path = nullptr;
            }
        }
        fprintf(f, "    {\"name\": \"%s\", \"base\": %lu, \"size\": %lu, "
                "\"elem_size\": %u, \"preferred\": %s",
                edge_regions[i].name,
                (unsigned long)edge_regions[i].base_address,
                (unsigned long)edge_regions[i].size_bytes,
                edge_regions[i].elem_size,
                (i == 0) ? "true" : "false");
        if (data_path) {
            fprintf(f, ", \"data_path\": \"%s\"", data_path);
        }
        fprintf(f, "}%s\n", (i < num_edge_regions - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    uint64_t total_edges = 0;
    uint32_t max_degree = 0;
    uint32_t bucket_counts[11] = {};
    uint64_t bucket_degrees[11] = {};
    for (int64_t n = 0; n < g.num_nodes(); n++) {
        uint32_t d = static_cast<uint32_t>(g.out_degree(n));
        total_edges += d;
        if (d > max_degree) {
            max_degree = d;
        }
        int b = 0;
        if (d >= 512) b = 10;
        else if (d >= 256) b = 9;
        else if (d >= 128) b = 8;
        else if (d >= 64) b = 7;
        else if (d >= 32) b = 6;
        else if (d >= 16) b = 5;
        else if (d >= 8) b = 4;
        else if (d >= 4) b = 3;
        else if (d >= 2) b = 2;
        else if (d >= 1) b = 1;
        bucket_counts[b]++;
        bucket_degrees[b] += d;
    }
    double avg_degree = (g.num_nodes() > 0) ? (double)total_edges / g.num_nodes() : 0.0;
    fprintf(f, "  \"avg_degree\": %.4f,\n", avg_degree);
    fprintf(f, "  \"max_degree\": %u,\n", max_degree);
    fprintf(f, "  \"degree_buckets\": {\n");
    fprintf(f, "    \"counts\": [");
    for (int i = 0; i < 11; i++) {
        fprintf(f, "%u%s", bucket_counts[i], i < 10 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "    \"total_degrees\": [");
    for (int i = 0; i < 11; i++) {
        fprintf(f, "%lu%s", (unsigned long)bucket_degrees[i], i < 10 ? ", " : "");
    }
    fprintf(f, "]\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    const bool context_complete = fflush(f) == 0 && ferror(f) == 0;
    const bool context_closed = fclose(f) == 0;
    if (!context_complete || !context_closed ||
        std::rename(temp_context_path.c_str(), resolved_path.c_str()) != 0) {
        std::remove(temp_context_path.c_str());
        std::remove(k2_offsets_path.c_str());
        std::remove(k2_records_path.c_str());
        fprintf(stderr, "sniper_harness: cannot publish complete context %s\n",
                resolved_path.c_str());
        return false;
    }

    graphbrew_sniper::notify_context_ready();
    printf("sniper_harness: exported context to %s (%ld vertices, %ld edges, %d regions)\n",
           resolved_path.c_str(), (long)g.num_nodes(), (long)g.num_edges_directed(), num_regions);
    return true;
}

inline void sniper_report_region(const char* name, const void* base, size_t count, size_t elem_size) {
    printf("GRAPHBREW_SNIPER_REGION:%s:0x%lx:%lu:%lu\n",
           name, reinterpret_cast<uint64_t>(base), count, elem_size);
}

inline bool sniper_export_popt_matrix(
    const uint8_t* matrix_data,
    uint32_t num_cache_lines,
    uint32_t num_epochs,
    uint32_t num_vertices,
    uint32_t cache_line_size = 64,
    const char* path = nullptr) {
    (void)cache_line_size;
    const std::string resolved_path = path ? std::string(path) : graphbrew_sniper::popt_matrix_path();
    FILE* f = fopen(resolved_path.c_str(), "wb");
    if (!f) {
        return false;
    }

    uint32_t epoch_size = (num_vertices + num_epochs - 1) / num_epochs;
    uint32_t sub_epoch_size = (epoch_size + 127) / 128;
    if (sub_epoch_size == 0) {
        sub_epoch_size = 1;
    }

    fwrite(&num_epochs, 4, 1, f);
    fwrite(&num_cache_lines, 4, 1, f);
    fwrite(&epoch_size, 4, 1, f);
    fwrite(&sub_epoch_size, 4, 1, f);
    fwrite(matrix_data, 1, static_cast<size_t>(num_epochs) * num_cache_lines, f);
    fclose(f);

    graphbrew_sniper::notify_user(
        graphbrew_sniper::GRAPHBREW_SNIPER_USER_POPT_READY,
        reinterpret_cast<uint64_t>(resolved_path.c_str()));
    printf("sniper_harness: exported P-OPT matrix to %s (%u epochs x %u lines = %lu bytes)\n",
           resolved_path.c_str(), num_epochs, num_cache_lines,
           (unsigned long)num_epochs * num_cache_lines);
    return true;
}

#define SNIPER_ROI_BEGIN() ::graphbrew_sniper::roi_begin()
#define SNIPER_ROI_END() ::graphbrew_sniper::roi_end()
#define SNIPER_SET_VERTEX(vertex_id) ::graphbrew_sniper::set_vertex(static_cast<uint64_t>(vertex_id))
#define SNIPER_CLEAR_VERTEX() ::graphbrew_sniper::clear_vertex()
#define SNIPER_ECG_PFX_TARGET(vertex_id) ::graphbrew_sniper::set_prefetch_target(static_cast<uint64_t>(vertex_id))
#define SNIPER_ECG_EXTRACT(vertex_id, epoch) ::graphbrew_sniper::ecg_extract(static_cast<uint64_t>(vertex_id), static_cast<uint16_t>(epoch))
#define SNIPER_ECG_EXTRACT2(vertex_id, tier, epoch1, epoch2) ::graphbrew_sniper::ecg_extract2(static_cast<uint32_t>(vertex_id), static_cast<uint8_t>(tier), static_cast<uint16_t>(epoch1), static_cast<uint16_t>(epoch2))
#define SNIPER_ECG_CLEAR_EXTRACT2(vertex_id) ::graphbrew_sniper::ecg_clear_extract2(static_cast<uint32_t>(vertex_id))
#define SNIPER_ECG_EXPECT2(vertex_id, tier, epoch1, epoch2) ::graphbrew_sniper::trace_ecg_extract2(static_cast<uint32_t>(vertex_id), static_cast<uint8_t>(tier), static_cast<uint16_t>(epoch1), static_cast<uint16_t>(epoch2))
