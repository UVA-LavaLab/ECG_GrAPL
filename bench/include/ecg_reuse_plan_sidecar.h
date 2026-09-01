#ifndef GRAPHBREW_ECG_REUSE_PLAN_SIDECAR_H
#define GRAPHBREW_ECG_REUSE_PLAN_SIDECAR_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>
#include <unistd.h>

#include "ecg_reuse_plan_builder.h"

namespace ecg_reuse_plan {

constexpr uint64_t REUSE_PLAN_SIDECAR_MAGIC = 0x3150435350524745ULL;
// v2 binds the compact record's tier width into the header. A v1 sidecar
// carried no tier width at all, so it can only be regenerated, never
// reinterpreted: the same bytes decode to different epochs at tier_bits=0.
constexpr uint32_t REUSE_PLAN_SIDECAR_VERSION = 2;
constexpr uint32_t REUSE_PLAN_BUILDER_VERSION = 2;

struct ReusePlanSidecarHeader {
    uint64_t magic = REUSE_PLAN_SIDECAR_MAGIC;
    uint32_t version = REUSE_PLAN_SIDECAR_VERSION;
    uint32_t builder_version = REUSE_PLAN_BUILDER_VERSION;
    uint32_t record_bytes = 0;
    uint32_t push_out_edges = 0;
    uint32_t num_vtx_per_line = 0;
    uint32_t epochs = 0;
    uint32_t linemin = 0;
    uint32_t hot_fraction_ppm = 0;
    uint32_t vertices = 0;
    uint32_t tier_bits = kReusePlanDefaultTierBits;
    uint64_t directed_edges = 0;
    uint64_t offset_count = 0;
    uint64_t record_count = 0;
    uint64_t graph_hash = 0;
    uint64_t payload_hash = 0;
};

static_assert(std::is_trivially_copyable<ReusePlanSidecarHeader>::value,
              "ReusePlan sidecar header must be binary-copyable");
static_assert(sizeof(ReusePlanSidecarHeader) == 88,
              "ReusePlan sidecar header layout drifted");

inline uint64_t sidecarHashByte(uint64_t hash, uint8_t value) {
    return (hash ^ value) * 1099511628211ULL;
}

inline uint64_t sidecarHashU64(uint64_t hash, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash = sidecarHashByte(
            hash, static_cast<uint8_t>((value >> shift) & 0xFFU));
    return hash;
}

template <typename GraphT>
uint64_t orderedGraphHash(const GraphT& graph, bool push_out_edges) {
    uint64_t hash = 1469598103934665603ULL;
    const uint32_t vertices = static_cast<uint32_t>(graph.num_nodes());
    hash = sidecarHashU64(hash, vertices);
    hash = sidecarHashU64(
        hash, static_cast<uint64_t>(graph.num_edges_directed()));
    hash = sidecarHashU64(hash, push_out_edges ? 1 : 0);
    for (uint32_t src = 0; src < vertices; ++src) {
        uint64_t count = 0;
        if (push_out_edges) {
            for (auto dest_raw : graph.out_neigh(src)) {
                hash = sidecarHashU64(
                    hash, static_cast<uint32_t>(dest_raw));
                ++count;
            }
        } else {
            for (auto dest_raw : graph.in_neigh(src)) {
                hash = sidecarHashU64(
                    hash, static_cast<uint32_t>(dest_raw));
                ++count;
            }
        }
        hash = sidecarHashU64(hash, count);
    }
    return hash;
}

template <typename RecordT>
uint64_t sidecarPayloadHash(
        const std::vector<uint64_t>& offsets,
        const std::vector<RecordT>& records) {
    uint64_t hash = 1469598103934665603ULL;
    hash = sidecarHashU64(hash, offsets.size());
    for (uint64_t value : offsets)
        hash = sidecarHashU64(hash, value);
    hash = sidecarHashU64(hash, records.size());
    for (RecordT value : records)
        hash = sidecarHashU64(hash, static_cast<uint64_t>(value));
    return hash;
}

inline uint32_t sidecarHotFractionPpm(double fraction) {
    fraction = std::max(0.0, std::min(0.5, fraction));
    return static_cast<uint32_t>(fraction * 1000000.0 + 0.5);
}

template <typename RecordT>
uint32_t sidecarRecordDest(RecordT record, uint32_t vertices) {
    if constexpr (sizeof(RecordT) == 4) {
        const uint32_t id_bits = reusePlan32IdBits(vertices);
        const uint32_t mask = id_bits >= 32
            ? std::numeric_limits<uint32_t>::max()
            : ((1U << id_bits) - 1U);
        return static_cast<uint32_t>(record) & mask;
    }
    return extractReusePlanDest(static_cast<uint64_t>(record));
}

template <typename RecordT>
bool writeReusePlanSidecar(
        const std::string& path, ReusePlanSidecarHeader header,
        const std::vector<uint64_t>& offsets,
        const std::vector<RecordT>& records,
        std::string& error) {
    if (!reusePlan32TierBitsSupported(header.tier_bits)) {
        error = "unsupported sidecar tier width";
        return false;
    }
    if constexpr (sizeof(RecordT) != 4) {
        if (header.tier_bits != kReusePlanDefaultTierBits) {
            error = "wide sidecar requires tier_bits=2";
            return false;
        }
    }
    header.magic = REUSE_PLAN_SIDECAR_MAGIC;
    header.version = REUSE_PLAN_SIDECAR_VERSION;
    header.builder_version = REUSE_PLAN_BUILDER_VERSION;
    header.record_bytes = sizeof(RecordT);
    header.offset_count = offsets.size();
    header.record_count = records.size();
    header.payload_hash = sidecarPayloadHash(offsets, records);
    const std::string temporary =
        path + ".tmp." +
        std::to_string(static_cast<unsigned long long>(::getpid()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot create temporary sidecar";
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(
            reinterpret_cast<const char*>(offsets.data()),
            static_cast<std::streamsize>(
                offsets.size() * sizeof(uint64_t)));
        output.write(
            reinterpret_cast<const char*>(records.data()),
            static_cast<std::streamsize>(
                records.size() * sizeof(RecordT)));
        output.flush();
        if (!output) {
            error = "cannot write complete sidecar";
            output.close();
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = "cannot publish sidecar";
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

template <typename GraphT, typename RecordT>
bool loadReusePlanSidecar(
        const std::string& path, const GraphT& graph,
        bool push_out_edges, uint32_t num_vtx_per_line,
        uint32_t epochs, bool linemin, double hot_fraction,
        uint32_t tier_bits,
        std::vector<uint64_t>& offsets,
        std::vector<RecordT>& records,
        ReusePlanSidecarHeader& header,
        std::string& error) {
    if (!reusePlan32TierBitsSupported(tier_bits)) {
        error = "unsupported sidecar tier width";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open sidecar";
        return false;
    }
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    const uint32_t vertices = static_cast<uint32_t>(graph.num_nodes());
    const uint64_t edges =
        static_cast<uint64_t>(graph.num_edges_directed());
    const uint64_t expected_records = edges;
    if (!input || header.magic != REUSE_PLAN_SIDECAR_MAGIC) {
        error = "sidecar magic mismatch";
        return false;
    }
    // Version and tier width are reported separately from the rest of the
    // configuration: a sidecar written before the tier width was recorded, or
    // one written for the other tier width, decodes to different epochs from
    // the same bytes and must be regenerated rather than reinterpreted.
    if (header.version != REUSE_PLAN_SIDECAR_VERSION ||
        header.builder_version != REUSE_PLAN_BUILDER_VERSION) {
        error = "sidecar version mismatch; regenerate the sidecar";
        return false;
    }
    if (header.tier_bits != tier_bits) {
        error = "sidecar tier_bits mismatch; regenerate the sidecar";
        return false;
    }
    if constexpr (sizeof(RecordT) != 4) {
        if (header.tier_bits != kReusePlanDefaultTierBits) {
            error = "wide sidecar requires tier_bits=2";
            return false;
        }
    }
    if (header.record_bytes != sizeof(RecordT) ||
        header.push_out_edges != (push_out_edges ? 1U : 0U) ||
        header.num_vtx_per_line != num_vtx_per_line ||
        header.epochs != normalizeReusePlanEpochCount(epochs) ||
        header.linemin != (linemin ? 1U : 0U) ||
        header.hot_fraction_ppm !=
            sidecarHotFractionPpm(hot_fraction) ||
        header.vertices != vertices ||
        header.directed_edges != edges ||
        header.offset_count != static_cast<uint64_t>(vertices) + 1 ||
        header.record_count != expected_records) {
        error = "sidecar header/configuration mismatch";
        return false;
    }
    const uint64_t expected_bytes =
        sizeof(header) +
        header.offset_count * sizeof(uint64_t) +
        header.record_count * sizeof(RecordT);
    input.seekg(0, std::ios::end);
    const std::streamoff file_bytes = input.tellg();
    if (file_bytes < 0 ||
        static_cast<uint64_t>(file_bytes) != expected_bytes) {
        error = "sidecar file length mismatch";
        return false;
    }
    input.seekg(sizeof(header), std::ios::beg);
    offsets.resize(static_cast<size_t>(header.offset_count));
    records.resize(static_cast<size_t>(header.record_count));
    input.read(
        reinterpret_cast<char*>(offsets.data()),
        static_cast<std::streamsize>(
            offsets.size() * sizeof(uint64_t)));
    input.read(
        reinterpret_cast<char*>(records.data()),
        static_cast<std::streamsize>(
            records.size() * sizeof(RecordT)));
    if (!input) {
        error = "sidecar payload is truncated";
        return false;
    }
    if (offsets.empty() || offsets.front() != 0 ||
        offsets.back() != records.size()) {
        error = "sidecar offsets are malformed";
        return false;
    }
    for (uint32_t src = 0; src < vertices; ++src) {
        if (offsets[src] > offsets[src + 1] ||
            offsets[src + 1] > records.size()) {
            error = "sidecar offsets are not monotonic";
            return false;
        }
        size_t index = static_cast<size_t>(offsets[src]);
        const size_t end = static_cast<size_t>(offsets[src + 1]);
        if (push_out_edges) {
            for (auto dest_raw : graph.out_neigh(src)) {
                if (index >= end ||
                    sidecarRecordDest(records[index], vertices) !=
                    static_cast<uint32_t>(dest_raw)) {
                    error = "sidecar out-edge ordering mismatch";
                    return false;
                }
                ++index;
            }
        } else {
            for (auto dest_raw : graph.in_neigh(src)) {
                if (index >= end ||
                    sidecarRecordDest(records[index], vertices) !=
                    static_cast<uint32_t>(dest_raw)) {
                    error = "sidecar in-edge ordering mismatch";
                    return false;
                }
                ++index;
            }
        }
        if (index != end) {
            error = "sidecar degree/count mismatch";
            return false;
        }
    }
    if (header.graph_hash != orderedGraphHash(graph, push_out_edges)) {
        error = "sidecar reordered-graph hash mismatch";
        return false;
    }
    if (header.payload_hash != sidecarPayloadHash(offsets, records)) {
        error = "sidecar payload hash mismatch";
        return false;
    }
    return true;
}

}  // namespace ecg_reuse_plan

#endif  // GRAPHBREW_ECG_REUSE_PLAN_SIDECAR_H
