#ifndef GRAPHBREW_ECG_REF32_H
#define GRAPHBREW_ECG_REF32_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "ecg_victim_policy.h"

namespace ecg_ref32 {

static constexpr uint32_t kDefaultReferenceBits = 8;
static constexpr uint32_t kStateBits = 2;
static constexpr uint32_t kDefaultActionBits = 4;
static constexpr uint32_t kMetadataBits = 14;
static constexpr uint32_t kMaxIdBits = 32 - kMetadataBits;
static constexpr uint32_t kExponentBits = 5;
static constexpr uint32_t kDefaultDeadlineBits = 21;
static constexpr uint32_t kMaxFiniteDistance = 0x7FFFFFFFu;
static constexpr uint32_t kScaleTokenBits = 6;
static constexpr uint32_t kScaleMaxIdBits = 32 - kScaleTokenBits;

enum class State : uint8_t {
    UNKNOWN = 0,
    FINITE = 1,
    DEAD = 2,
    WRAP = 3,
};

struct DecodedRecord {
    uint32_t destination = 0;
    uint32_t distance = 0;
    State state = State::UNKNOWN;
    uint8_t action = 0;
    bool distance_valid = false;
};

enum class Format : uint8_t {
    FULL14 = 0,
    SCALE6 = 1,
};

inline uint32_t bitsForVertices(uint64_t num_vertices) {
    uint32_t bits = 1;
    while (bits < 32 && (uint64_t{1} << bits) < num_vertices) ++bits;
    return bits;
}

inline bool validFieldWidths(
        uint32_t reference_bits, uint32_t action_bits) {
    return reference_bits >= 6 && reference_bits <= 12 &&
        action_bits <= 12 &&
        reference_bits + kStateBits + action_bits == kMetadataBits;
}

inline bool canPackRecord32(
        uint64_t num_vertices,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    return num_vertices > 0 &&
        validFieldWidths(reference_bits, action_bits) &&
        bitsForVertices(num_vertices) + reference_bits +
            kStateBits + action_bits <= 32;
}

inline uint16_t saturatedDistanceCode(uint32_t reference_bits) {
    return static_cast<uint16_t>((uint32_t{1} << reference_bits) - 1u);
}

inline bool validDistanceCode(
        uint16_t code,
        uint32_t reference_bits = kDefaultReferenceBits) {
    if (reference_bits < 6 || reference_bits > 12)
        return false;
    const uint32_t mantissa_bits = reference_bits - kExponentBits;
    const uint16_t invalid_begin =
        static_cast<uint16_t>(
            ((uint32_t{1} << kExponentBits) - 1u) << mantissa_bits);
    return code < invalid_begin ||
        code == saturatedDistanceCode(reference_bits);
}

// Five exponent bits cover the full uint31 forward-distance horizon. The
// remaining bits are mantissa precision; decode returns the bucket upper bound.
inline uint16_t encodeDistance(
        uint64_t distance,
        uint32_t reference_bits = kDefaultReferenceBits) {
    if (reference_bits < 6 || reference_bits > 12)
        return 0;
    if (distance == 0) distance = 1;
    if (distance > kMaxFiniteDistance)
        return saturatedDistanceCode(reference_bits);

    uint32_t exponent = 0;
    uint32_t value = static_cast<uint32_t>(distance);
    while ((uint32_t{1} << (exponent + 1)) <= value)
        ++exponent;
    const uint32_t base = uint32_t{1} << exponent;
    const uint32_t mantissa_bits = reference_bits - kExponentBits;
    const uint32_t mantissa_levels = uint32_t{1} << mantissa_bits;
    uint32_t mantissa = static_cast<uint32_t>(
        ((static_cast<uint64_t>(value - base) << mantissa_bits) / base));
    if (mantissa >= mantissa_levels)
        mantissa = mantissa_levels - 1;
    return static_cast<uint16_t>(
        (exponent << mantissa_bits) | mantissa);
}

inline uint32_t decodeDistanceUpper(
        uint16_t code,
        uint32_t reference_bits = kDefaultReferenceBits) {
    if (code == saturatedDistanceCode(reference_bits))
        return kMaxFiniteDistance;
    if (!validDistanceCode(code, reference_bits))
        return 0;
    const uint32_t mantissa_bits = reference_bits - kExponentBits;
    const uint32_t mantissa_mask =
        (uint32_t{1} << mantissa_bits) - 1u;
    const uint32_t exponent = code >> mantissa_bits;
    const uint32_t mantissa = code & mantissa_mask;
    const uint32_t base = uint32_t{1} << exponent;
    const uint32_t bucket_upper = static_cast<uint32_t>(
        (static_cast<uint64_t>(mantissa + 1) * base +
         ((uint32_t{1} << mantissa_bits) - 1)) >> mantissa_bits);
    return std::min<uint32_t>(
        kMaxFiniteDistance, base + std::max<uint32_t>(1, bucket_upper) - 1);
}

inline uint32_t packRecord32(
        uint32_t destination, uint16_t distance_code, State state,
        uint8_t action, uint32_t id_bits,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    const uint32_t id_mask =
        id_bits >= 32 ? 0xFFFFFFFFu : ((uint32_t{1} << id_bits) - 1u);
    const uint32_t reference_shift = id_bits;
    const uint32_t reference_mask =
        (uint32_t{1} << reference_bits) - 1u;
    const uint32_t action_mask = action_bits
        ? (uint32_t{1} << action_bits) - 1u : 0u;
    const uint32_t state_shift = reference_shift + reference_bits;
    const uint32_t action_shift = state_shift + kStateBits;
    uint32_t record = (destination & id_mask) |
        ((static_cast<uint32_t>(distance_code) & reference_mask)
            << reference_shift) |
        ((static_cast<uint32_t>(state) & 0x3u) << state_shift) |
        0u;
    if (action_bits > 0)
        record |= (static_cast<uint32_t>(action) & action_mask)
            << action_shift;
    return record;
}

inline uint32_t extractDestination(uint32_t record, uint32_t id_bits) {
    const uint32_t id_mask =
        id_bits >= 32 ? 0xFFFFFFFFu : ((uint32_t{1} << id_bits) - 1u);
    return record & id_mask;
}

inline uint16_t extractDistanceCode(
        uint32_t record, uint32_t id_bits,
        uint32_t reference_bits = kDefaultReferenceBits) {
    return static_cast<uint16_t>(
        (record >> id_bits) &
        ((uint32_t{1} << reference_bits) - 1u));
}

inline State extractState(
        uint32_t record, uint32_t id_bits,
        uint32_t reference_bits = kDefaultReferenceBits) {
    return static_cast<State>(
        (record >> (id_bits + reference_bits)) & 0x3u);
}

inline uint8_t extractAction(
        uint32_t record, uint32_t id_bits,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    if (action_bits == 0)
        return 0;
    return static_cast<uint8_t>(
        (record >> (id_bits + reference_bits + kStateBits)) &
        ((uint32_t{1} << action_bits) - 1u));
}

inline uint32_t actionDelta(uint8_t action, uint32_t action_bits) {
    if (action == 0 || action_bits == 0)
        return 0;
    if (action_bits == 2) {
        static constexpr uint8_t kDeltas[4] = {0, 8, 12, 15};
        return kDeltas[action & 0x3u];
    }
    return action;
}

inline uint8_t actionForDelta(uint32_t delta, uint32_t action_bits) {
    if (action_bits == 0)
        return 0;
    if (action_bits == 2) {
        if (delta == 8) return 1;
        if (delta == 12) return 2;
        if (delta == 15) return 3;
        return 0;
    }
    const uint32_t max_action = (uint32_t{1} << action_bits) - 1u;
    return delta <= max_action ? static_cast<uint8_t>(delta) : 0;
}

inline uint32_t setAction(
        uint32_t record, uint8_t action, uint32_t id_bits,
        uint32_t reference_bits, uint32_t action_bits) {
    if (action_bits == 0)
        return record;
    const uint32_t shift = id_bits + reference_bits + kStateBits;
    const uint32_t mask =
        ((uint32_t{1} << action_bits) - 1u) << shift;
    return (record & ~mask) |
        ((static_cast<uint32_t>(action) << shift) & mask);
}

inline DecodedRecord decodeRecord32(
        uint32_t record, uint32_t id_bits,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    DecodedRecord decoded;
    decoded.destination = extractDestination(record, id_bits);
    const uint16_t code =
        extractDistanceCode(record, id_bits, reference_bits);
    decoded.distance_valid =
        validDistanceCode(code, reference_bits);
    decoded.distance =
        decodeDistanceUpper(code, reference_bits);
    decoded.state =
        extractState(record, id_bits, reference_bits);
    decoded.action =
        extractAction(record, id_bits, reference_bits, action_bits);
    if (!decoded.distance_valid)
        decoded.state = State::UNKNOWN;
    return decoded;
}

inline bool canPackScaleRecord32(
        uint64_t num_vertices, uint32_t id_bits = 0) {
    const uint32_t required = bitsForVertices(num_vertices);
    const uint32_t width = id_bits ? id_bits : required;
    return num_vertices > 0 && required <= width && width <= kScaleMaxIdBits;
}

inline uint8_t scaleDistanceBucket(uint64_t distance) {
    if (distance == 0) distance = 1;
    uint32_t bucket = 0;
    while (bucket < 30 &&
           (uint64_t{1} << (bucket + 1)) <= distance) {
        ++bucket;
    }
    return static_cast<uint8_t>(bucket);
}

inline uint32_t decodeScaleDistanceUpper(uint8_t bucket) {
    const uint32_t clamped = std::min<uint32_t>(bucket, 30);
    return clamped == 30
        ? kMaxFiniteDistance
        : (uint32_t{1} << (clamped + 1)) - 1u;
}

// Six-bit Twitter token:
//   0 unknown, 1 dead, 2..32 finite-current, 33..63 wrap.
inline uint8_t encodeScaleToken(uint64_t distance, State state) {
    if (state == State::UNKNOWN) return 0;
    if (state == State::DEAD) return 1;
    const uint8_t bucket = scaleDistanceBucket(distance);
    return static_cast<uint8_t>(
        (state == State::WRAP ? 33u : 2u) + bucket);
}

inline State decodeScaleState(uint8_t token) {
    if (token == 0) return State::UNKNOWN;
    if (token == 1) return State::DEAD;
    return token >= 33 ? State::WRAP : State::FINITE;
}

inline uint32_t decodeScaleDistance(uint8_t token) {
    if (token < 2) return 0;
    const uint8_t bucket = token >= 33
        ? static_cast<uint8_t>(token - 33)
        : static_cast<uint8_t>(token - 2);
    return decodeScaleDistanceUpper(bucket);
}

inline uint32_t packScaleRecord32(
        uint32_t destination, uint8_t token, uint32_t id_bits) {
    const uint32_t id_mask =
        id_bits >= 32 ? 0xFFFFFFFFu : ((uint32_t{1} << id_bits) - 1u);
    return (destination & id_mask) |
        ((static_cast<uint32_t>(token) & 0x3Fu) << id_bits);
}

inline DecodedRecord decodeScaleRecord32(
        uint32_t record, uint32_t id_bits) {
    DecodedRecord decoded;
    decoded.destination = extractDestination(record, id_bits);
    const uint8_t token =
        static_cast<uint8_t>((record >> id_bits) & 0x3Fu);
    decoded.state = decodeScaleState(token);
    decoded.distance = decodeScaleDistance(token);
    decoded.distance_valid =
        decoded.state == State::FINITE || decoded.state == State::WRAP;
    return decoded;
}

// RV64 configuration: records[30:0], (vertices-1)[56:31], enabled[57],
// version[59:58]. Keeping N-1 makes the exact 2^26-vertex boundary representable.
static constexpr uint64_t kNativeRecordMask = (uint64_t{1} << 31) - 1;
static constexpr uint64_t kNativeVertexMask = (uint64_t{1} << 26) - 1;
static constexpr uint64_t kNativeEnabled = uint64_t{1} << 57;
static constexpr uint64_t kNativeVersion = uint64_t{1} << 58;
static constexpr uint64_t kNativeHasNextIteration = uint64_t{1} << 32;
static constexpr uint64_t kNativeIterationMask = (uint64_t{1} << 33) - 1;

struct NativeConfig {
    uint32_t vertices = 0;
    uint32_t records = 0;
};

struct NativeAccess {
    uint64_t address = 0;
    uint32_t destination = 0;
    uint32_t sequence = 0;
    uint32_t deadline = 0;
    State state = State::UNKNOWN;
};

inline bool packNativeConfig(
        uint64_t vertices, uint64_t records, uint64_t& config) {
    config = 0;
    if (!canPackScaleRecord32(vertices, kScaleMaxIdBits) ||
        records == 0 || records > kNativeRecordMask)
        return false;
    config = records | ((vertices - 1) << 31) | kNativeEnabled | kNativeVersion;
    return true;
}

inline bool decodeNativeConfig(uint64_t config, NativeConfig& decoded) {
    decoded = NativeConfig{};
    if ((config >> 60) != 0 || (config & kNativeEnabled) == 0 ||
        (config & (uint64_t{3} << 58)) != kNativeVersion ||
        (config & kNativeRecordMask) == 0)
        return false;
    decoded.records = static_cast<uint32_t>(config & kNativeRecordMask);
    decoded.vertices =
        static_cast<uint32_t>((config >> 31) & kNativeVertexMask) + 1u;
    return true;
}

inline bool packNativeIteration(
        uint32_t iteration, uint32_t records, uint32_t iteration_count,
        uint64_t& descriptor) {
    descriptor = 0;
    if (records == 0 || records > kNativeRecordMask ||
        iteration_count == 0 || iteration >= iteration_count)
        return false;
    descriptor = static_cast<uint32_t>(static_cast<uint64_t>(iteration) * records);
    if (iteration < iteration_count - 1)
        descriptor |= kNativeHasNextIteration;
    return true;
}

// Canonical operand: semantic sequence[63:32], runtime-normalized record[31:0].
// The bool return, not a zero-word sentinel, distinguishes invalid input.
inline bool canonicalScaleRecord(
        uint32_t record, uint64_t record_address, uint64_t record_base,
        uint64_t config, uint64_t iteration, uint64_t& canonical) {
    canonical = 0;
    NativeConfig geometry;
    if (!decodeNativeConfig(config, geometry) ||
        (iteration & ~kNativeIterationMask) != 0 ||
        (record_base & 3u) != 0 || (record_address & 3u) != 0 ||
        record_address < record_base)
        return false;
    const uint64_t bytes = static_cast<uint64_t>(geometry.records) * 4;
    if (record_base > std::numeric_limits<uint64_t>::max() - (bytes - 1) ||
        record_address - record_base >= bytes)
        return false;
    const uint32_t destination = extractDestination(record, kScaleMaxIdBits);
    if (destination >= geometry.vertices)
        return false;
    uint8_t token = static_cast<uint8_t>(record >> kScaleMaxIdBits);
    if (token >= 33) {
        token = iteration & kNativeHasNextIteration
            ? static_cast<uint8_t>(token - 31) : uint8_t{1};
    }
    const uint32_t sequence = static_cast<uint32_t>(
        static_cast<uint32_t>(iteration) + ((record_address - record_base) >> 2) + 1);
    canonical = (static_cast<uint64_t>(sequence) << 32) |
        packScaleRecord32(destination, token, kScaleMaxIdBits);
    return true;
}

inline bool nativePropertyAccess(
        uint64_t canonical, uint64_t property_base,
        uint64_t config, NativeAccess& access) {
    access = NativeAccess{};
    NativeConfig geometry;
    if (!decodeNativeConfig(config, geometry) || (property_base & 3u) != 0)
        return false;
    const uint64_t bytes = static_cast<uint64_t>(geometry.vertices) * 4;
    if (property_base > std::numeric_limits<uint64_t>::max() - (bytes - 1))
        return false;
    const auto decoded = decodeScaleRecord32(
        static_cast<uint32_t>(canonical), kScaleMaxIdBits);
    if (decoded.destination >= geometry.vertices || decoded.state == State::WRAP)
        return false;
    access.address = property_base + static_cast<uint64_t>(decoded.destination) * 4;
    access.destination = decoded.destination;
    access.sequence = static_cast<uint32_t>(canonical >> 32);
    access.state = decoded.state;
    if (decoded.state == State::FINITE)
        access.deadline = access.sequence + decoded.distance;
    return true;
}

enum class SequenceOrder : uint8_t {
    OLDER,
    EQUAL,
    NEWER,
    AMBIGUOUS,
};

inline SequenceOrder compareSequence32(uint32_t candidate, uint32_t current) {
    const uint32_t delta = candidate - current;
    if (delta == 0) return SequenceOrder::EQUAL;
    if (delta == 0x80000000u) return SequenceOrder::AMBIGUOUS;
    return delta < 0x80000000u ? SequenceOrder::NEWER : SequenceOrder::OLDER;
}

struct FlatRecords {
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> records;
    // Diagnostic-only exact distances. The deployable path consumes records.
    std::vector<uint32_t> exact_distances;
};

inline uint32_t selectScalePrefetchDelta(
        const uint32_t* records, uint64_t record_count, uint64_t position,
        uint32_t id_bits, uint32_t vertices_per_line = 16) {
    if (!records || position >= record_count || vertices_per_line == 0)
        return 0;
    const uint32_t current_line =
        decodeScaleRecord32(records[position], id_bits).destination /
        vertices_per_line;
    uint64_t best = std::numeric_limits<uint64_t>::max();
    uint32_t best_distance = std::numeric_limits<uint32_t>::max();
    uint32_t best_lead_error = std::numeric_limits<uint32_t>::max();
    for (uint32_t lead = 8; lead <= 15; ++lead) {
        const uint64_t candidate = position + lead;
        if (candidate >= record_count)
            break;
        const DecodedRecord decoded =
            decodeScaleRecord32(records[candidate], id_bits);
        const uint32_t candidate_line =
            decoded.destination / vertices_per_line;
        if (candidate_line == current_line)
            continue;
        bool first_in_window = true;
        for (uint64_t prior = position + 1;
             prior < candidate; ++prior) {
            if (decodeScaleRecord32(
                    records[prior], id_bits).destination /
                    vertices_per_line == candidate_line) {
                first_in_window = false;
                break;
            }

        }
        if (!first_in_window)
            continue;
        const uint32_t distance =
            decoded.distance_valid ? decoded.distance : kMaxFiniteDistance;
        const uint32_t lead_error =
            lead > 10 ? lead - 10 : 10 - lead;
        if (best == std::numeric_limits<uint64_t>::max() ||
            distance < best_distance ||
            (distance == best_distance &&
             lead_error < best_lead_error)) {
            best = candidate;
            best_distance = distance;
            best_lead_error = lead_error;
        }
    }
    return best == std::numeric_limits<uint64_t>::max()
        ? 0 : static_cast<uint32_t>(best - position);
}

inline uint32_t selectScalePrefetchDelta(
        const std::vector<uint32_t>& records, uint64_t position,
        uint32_t id_bits, uint32_t vertices_per_line = 16) {
    return selectScalePrefetchDelta(
        records.data(), records.size(), position,
        id_bits, vertices_per_line);
}

inline bool buildFlatRecordsFromDestinations(
        uint32_t num_vertices, uint32_t vertices_per_line,
        const std::vector<uint64_t>& offsets,
        const std::vector<uint32_t>& destinations,
        FlatRecords& output,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    output = FlatRecords{};
    if (!canPackRecord32(
            num_vertices, reference_bits, action_bits) ||
        vertices_per_line == 0 ||
        offsets.empty() || offsets.front() != 0 ||
        offsets.back() != destinations.size()) {
        return false;
    }

    for (size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] < offsets[i - 1])
            return false;
    }

    const uint32_t id_bits = bitsForVertices(num_vertices);
    const uint32_t line_count =
        (num_vertices + vertices_per_line - 1) / vertices_per_line;
    const uint64_t no_position = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> first(line_count, no_position);
    std::vector<uint64_t> last_position(line_count, no_position);
    std::vector<uint64_t> next(line_count, no_position);
    std::vector<uint32_t> lines(destinations.size(), 0);

    for (uint64_t position = 0; position < destinations.size(); ++position) {
        const uint32_t destination = destinations[position];
        if (destination >= num_vertices)
            return false;
        const uint32_t line = destination / vertices_per_line;
        lines[position] = line;
        if (first[line] == no_position)
            first[line] = position;
        last_position[line] = position;
    }

    output.offsets = offsets;
    output.records.resize(destinations.size());
    output.exact_distances.resize(destinations.size());
    const uint64_t span = destinations.size();
    for (uint64_t position = span; position-- > 0;) {
        const uint32_t line = lines[position];
        const bool current_iteration = next[line] != no_position;
        const uint64_t distance = current_iteration
            ? next[line] - position
            : span - position + first[line];
        output.exact_distances[position] =
            distance > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(distance);
        output.records[position] = packRecord32(
            destinations[position],
            encodeDistance(distance, reference_bits),
            current_iteration ? State::FINITE : State::WRAP,
            /*action=*/0, id_bits, reference_bits, action_bits);
        next[line] = position;
    }

    if (action_bits > 0 && span > 0) {
        std::vector<uint64_t> previous(span, no_position);
        std::vector<uint64_t> backward_gap(span, span);
        std::fill(next.begin(), next.end(), no_position);
        for (uint64_t position = 0; position < span; ++position) {
            const uint32_t line = lines[position];
            previous[position] = next[line];
            backward_gap[position] = next[line] == no_position
                ? position + span - last_position[line]
                : position - next[line];
            next[line] = position;
        }

        for (uint64_t position = 0; position < span; ++position) {
            uint64_t best = no_position;
            uint64_t best_gap = 0;
            uint32_t best_future = std::numeric_limits<uint32_t>::max();
            uint32_t best_lead_error = std::numeric_limits<uint32_t>::max();
            auto consider = [&](uint32_t lead) {
                const uint64_t candidate = position + lead;
                if (candidate >= span ||
                    lines[candidate] == lines[position] ||
                    (previous[candidate] != no_position &&
                     previous[candidate] > position)) {
                    return;
                }
                const uint64_t gap = backward_gap[candidate];
                const uint32_t future =
                    output.exact_distances[candidate];
                const uint32_t lead_error =
                    lead > 10 ? lead - 10 : 10 - lead;
                if (best == no_position || gap > best_gap ||
                    (gap == best_gap && future < best_future) ||
                    (gap == best_gap && future == best_future &&
                     lead_error < best_lead_error)) {
                    best = candidate;
                    best_gap = gap;
                    best_future = future;
                    best_lead_error = lead_error;
                }
            };
            if (action_bits == 2) {
                consider(8);
                consider(12);
                consider(15);
            } else {
                for (uint32_t lead = 8; lead <= 15; ++lead)
                    consider(lead);
            }
            if (best == no_position)
                continue;
            const uint32_t delta =
                static_cast<uint32_t>(best - position);
            output.records[position] = setAction(
                output.records[position],
                actionForDelta(delta, action_bits),
                id_bits, reference_bits, action_bits);
        }
    }
    return true;
}

inline bool buildFlatScaleRecordsFromDestinations(
        uint32_t num_vertices, uint32_t vertices_per_line,
        const std::vector<uint64_t>& offsets,
        const std::vector<uint32_t>& destinations,
        FlatRecords& output, uint32_t id_bits = 26) {
    output = FlatRecords{};
    if (!canPackScaleRecord32(num_vertices, id_bits) ||
        vertices_per_line == 0 || offsets.empty() ||
        offsets.front() != 0 || offsets.back() != destinations.size()) {
        return false;
    }

    for (size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] < offsets[i - 1])
            return false;
    }
    const uint32_t line_count =
        (num_vertices + vertices_per_line - 1) / vertices_per_line;
    const uint64_t no_position = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> first(line_count, no_position);
    std::vector<uint64_t> next(line_count, no_position);
    std::vector<uint32_t> lines(destinations.size(), 0);
    for (uint64_t position = 0; position < destinations.size(); ++position) {
        const uint32_t destination = destinations[position];
        if (destination >= num_vertices)
            return false;
        const uint32_t line = destination / vertices_per_line;
        lines[position] = line;
        if (first[line] == no_position)
            first[line] = position;
    }
    output.offsets = offsets;
    output.records.resize(destinations.size());
    const uint64_t span = destinations.size();
    for (uint64_t position = span; position-- > 0;) {
        const uint32_t line = lines[position];
        const bool current_iteration = next[line] != no_position;
        const uint64_t distance = current_iteration
            ? next[line] - position
            : span - position + first[line];
        output.records[position] = packScaleRecord32(
            destinations[position],
            encodeScaleToken(
                distance,
                current_iteration ? State::FINITE : State::WRAP),
            id_bits);
        next[line] = position;
    }
    return true;
}

template <typename EdgeT>
bool buildScaleRecordsInPlace(
        EdgeT* edges, uint64_t edge_count, uint32_t num_vertices,
        uint32_t vertices_per_line, uint32_t id_bits = 26) {
    static_assert(sizeof(EdgeT) == sizeof(uint32_t),
                  "REF32 in-place records require 32-bit edges");
    if (!edges || edge_count == 0 ||
        !canPackScaleRecord32(num_vertices, id_bits) ||
        vertices_per_line == 0) {
        return false;
    }
    const uint32_t id_mask =
        id_bits == 32 ? UINT32_MAX : (uint32_t{1} << id_bits) - 1u;
    const uint32_t line_count =
        (num_vertices + vertices_per_line - 1) / vertices_per_line;
    const uint64_t no_position = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> first(line_count, no_position);
    std::vector<uint64_t> next(line_count, no_position);
    const uint64_t progress_interval = []() {
        const char* value = std::getenv("ECG_REF32_PROGRESS_EDGES");
        const uint64_t parsed = value
            ? std::strtoull(value, nullptr, 10) : (uint64_t{1} << 26);
        return std::max<uint64_t>(1, parsed);
    }();
    const auto build_start = std::chrono::steady_clock::now();
    auto report = [&](const char* pass, uint64_t completed) {
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - build_start).count();
        std::fprintf(
            stderr,
            "[ECG-REF32-BUILD storage=inplace pass=%s completed=%llu "
            "edges=%llu percent=%.3f elapsed_s=%.3f aux_bytes=%llu]\n",
            pass,
            static_cast<unsigned long long>(completed),
            static_cast<unsigned long long>(edge_count),
            edge_count > 0
                ? 100.0 * static_cast<double>(completed) /
                    static_cast<double>(edge_count)
                : 100.0,
            seconds,
            static_cast<unsigned long long>(
                2ULL * line_count * sizeof(uint64_t)));
    };

    for (uint64_t position = 0; position < edge_count; ++position) {
        const uint32_t destination =
            static_cast<uint32_t>(edges[position]) & id_mask;
        if (destination >= num_vertices)
            return false;
        const uint32_t line = destination / vertices_per_line;
        if (first[line] == no_position)
            first[line] = position;
        if ((position + 1) % progress_interval == 0)
            report("first", position + 1);
    }
    report("first", edge_count);
    for (uint64_t position = edge_count; position-- > 0;) {
        const uint32_t destination =
            static_cast<uint32_t>(edges[position]) & id_mask;
        const uint32_t line = destination / vertices_per_line;
        const bool current_iteration = next[line] != no_position;
        const uint64_t distance = current_iteration
            ? next[line] - position
            : edge_count - position + first[line];
        const uint32_t record = packScaleRecord32(
            destination,
            encodeScaleToken(
                distance,
                current_iteration ? State::FINITE : State::WRAP),
            id_bits);
        edges[position] = static_cast<EdgeT>(record);
        next[line] = position;
        const uint64_t completed = edge_count - position;
        if (completed % progress_interval == 0)
            report("reverse", completed);
    }
    report("reverse", edge_count);
    return true;
}

template <typename GraphT>
bool buildInEdgeRecords32(
        const GraphT& graph, uint32_t vertices_per_line,
        FlatRecords& output,
        uint32_t reference_bits = kDefaultReferenceBits,
        uint32_t action_bits = kDefaultActionBits) {
    const uint32_t num_vertices =
        static_cast<uint32_t>(graph.num_nodes());
    std::vector<uint64_t> offsets(
        static_cast<size_t>(num_vertices) + 1, 0);
    for (uint32_t source = 0; source < num_vertices; ++source) {
        offsets[source + 1] =
            offsets[source] + static_cast<uint64_t>(graph.in_degree(source));
    }

    std::vector<uint32_t> destinations;
    destinations.reserve(static_cast<size_t>(offsets.back()));
    for (uint32_t source = 0; source < num_vertices; ++source) {
        for (auto destination : graph.in_neigh(source))
            destinations.push_back(static_cast<uint32_t>(destination));
    }
    return buildFlatRecordsFromDestinations(
        num_vertices, vertices_per_line, offsets, destinations, output,
        reference_bits, action_bits);
}

template <typename GraphT>
bool buildInEdgeScaleRecords32(
        const GraphT& graph, uint32_t vertices_per_line,
        FlatRecords& output, uint32_t id_bits = 26) {
    const uint32_t num_vertices =
        static_cast<uint32_t>(graph.num_nodes());
    std::vector<uint64_t> offsets(
        static_cast<size_t>(num_vertices) + 1, 0);
    for (uint32_t source = 0; source < num_vertices; ++source) {
        offsets[source + 1] =
            offsets[source] + static_cast<uint64_t>(graph.in_degree(source));
    }
    std::vector<uint32_t> destinations;
    destinations.reserve(static_cast<size_t>(offsets.back()));
    for (uint32_t source = 0; source < num_vertices; ++source) {
        for (auto destination : graph.in_neigh(source))
            destinations.push_back(static_cast<uint32_t>(destination));
    }
    return buildFlatScaleRecordsFromDestinations(
        num_vertices, vertices_per_line, offsets, destinations,
        output, id_bits);
}

struct EffectiveFuture {
    State state = State::UNKNOWN;
    uint64_t remaining = 0;
};

inline EffectiveFuture resolveQuantizedFuture(
        State state, uint32_t deadline, uint64_t current_sequence,
        uint32_t deadline_bits = kDefaultDeadlineBits) {
    EffectiveFuture effective;
    effective.state = state;
    if (state != State::FINITE)
        return effective;
    if (deadline_bits < 2 || deadline_bits > 32) {
        effective.state = State::UNKNOWN;
        return effective;
    }
    const uint32_t mask = deadline_bits == 32
        ? UINT32_MAX : (uint32_t{1} << deadline_bits) - 1u;
    const uint32_t current =
        static_cast<uint32_t>(current_sequence) & mask;
    const uint32_t remaining = (deadline - current) & mask;
    const uint32_t max_forward = deadline_bits == 32
        ? kMaxFiniteDistance
        : (uint32_t{1} << (deadline_bits - 1)) - 1u;
    if (remaining > max_forward) {
        effective.state = State::UNKNOWN;
        return effective;
    }
    effective.remaining = remaining;
    return effective;
}

inline EffectiveFuture resolveExactFuture(
        State state, uint64_t deadline, uint64_t current_sequence) {
    EffectiveFuture effective;
    effective.state = state;
    if (state != State::FINITE)
        return effective;
    if (deadline < current_sequence) {
        effective.state = State::UNKNOWN;
        return effective;
    }
    effective.remaining = deadline - current_sequence;
    return effective;
}

inline uint8_t distanceRRPV(uint64_t remaining, uint8_t rrpv_max = 7) {
    if (remaining == 0)
        return 0;
    uint32_t log2 = 0;
    while (remaining >>= 1)
        ++log2;
    return static_cast<uint8_t>(
        std::min<uint32_t>(rrpv_max, log2 / 2));
}

struct WayState {
    bool property = false;
    uint8_t rrpv = 0;
    uint64_t recency = 0;
    uint8_t grasp_tier = 0;
    State state = State::UNKNOWN;
    uint32_t quantized_deadline = 0;
    uint64_t exact_deadline = 0;
};

enum class VictimReason : uint8_t {
    DEAD_PROPERTY = 0,
    NON_PROPERTY = 1,
    UNKNOWN_PROPERTY = 2,
    FINITE_PROPERTY = 3,
};

struct VictimRank {
    uint8_t category = 0;
    uint8_t score = 0;
    bool unknown = false;
    uint64_t remaining = 0;
    uint8_t grasp_tier = 0;
    uint64_t recency = 0;
};

inline bool betterVictimRank(
        const VictimRank& candidate, const VictimRank& incumbent) {
    if (candidate.category != incumbent.category)
        return candidate.category > incumbent.category;
    if (candidate.score != incumbent.score)
        return candidate.score > incumbent.score;
    if (candidate.unknown != incumbent.unknown)
        return candidate.unknown;
    if (candidate.remaining != incumbent.remaining)
        return candidate.remaining > incumbent.remaining;
    if (candidate.grasp_tier != incumbent.grasp_tier)
        return candidate.grasp_tier > incumbent.grasp_tier;
    return candidate.recency < incumbent.recency;
}

inline size_t selectVictim(
        const WayState* ways, size_t count, uint64_t current_sequence,
        bool exact, VictimReason* reason = nullptr,
        uint32_t deadline_bits = kDefaultDeadlineBits) {
    size_t victim = 0;
    VictimRank best;
    VictimReason best_reason = VictimReason::FINITE_PROPERTY;
    bool have_best = false;
    for (size_t index = 0; index < count; ++index) {
        const WayState& way = ways[index];
        VictimRank rank;
        VictimReason current_reason;
        if (way.property && way.state == State::DEAD) {
            rank.category = 3;
            current_reason = VictimReason::DEAD_PROPERTY;
        } else if (!way.property) {
            rank.category = 2;
            rank.recency = way.recency;
            current_reason = VictimReason::NON_PROPERTY;
        } else {
            rank.category = 1;
            const EffectiveFuture future = exact
                ? resolveExactFuture(
                    way.state, way.exact_deadline, current_sequence)
                : resolveQuantizedFuture(
                    way.state, way.quantized_deadline, current_sequence,
                    deadline_bits);
            rank.unknown = future.state != State::FINITE;
            rank.remaining = future.remaining;
            rank.grasp_tier = way.grasp_tier;
            rank.recency = way.recency;
            rank.score = rank.unknown
                ? std::max<uint8_t>(
                    way.rrpv,
                    ecg_policy::graspTierRRPV(way.grasp_tier, 7))
                : distanceRRPV(future.remaining);
            current_reason = rank.unknown
                ? VictimReason::UNKNOWN_PROPERTY
                : VictimReason::FINITE_PROPERTY;
        }
        if (!have_best || betterVictimRank(rank, best)) {
            victim = index;
            best = rank;
            best_reason = current_reason;
            have_best = true;
        }
    }
    if (reason) *reason = best_reason;
    return victim;
}

}  // namespace ecg_ref32

#endif
