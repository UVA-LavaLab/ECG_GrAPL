#ifndef GRAPHBREW_ECG_REF32_H
#define GRAPHBREW_ECG_REF32_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

struct FlatRecords {
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> records;
    // Diagnostic-only exact distances. The deployable path consumes records.
    std::vector<uint32_t> exact_distances;
};

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
    if (deadline_bits < 2 || deadline_bits > 31) {
        effective.state = State::UNKNOWN;
        return effective;
    }
    const uint32_t mask = (uint32_t{1} << deadline_bits) - 1u;
    const uint32_t current =
        static_cast<uint32_t>(current_sequence) & mask;
    const uint32_t remaining = (deadline - current) & mask;
    const uint32_t max_forward =
        (uint32_t{1} << (deadline_bits - 1)) - 1u;
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
