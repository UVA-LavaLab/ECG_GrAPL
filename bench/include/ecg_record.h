#ifndef GRAPHBREW_ECG_RECORD_H
#define GRAPHBREW_ECG_RECORD_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

#include "ecg_ref32.h"

namespace ecg_record {

// Carrier encoding is separate from graph-loader capacity and backend admission.
using State = ecg_ref32::State;

enum class Preset : uint8_t {
    FULL14_V1,
    SCALE6_V1,
    ADAPTIVE_RICH_V2,
    ADAPTIVE_COMPACT_V2,
};

enum class ActionEncoding : uint8_t {
    DEFAULT,
    NONE,
    DIRECT4,
    ENUMERATED2,
};

enum class Status : uint8_t {
    OK,
    INVALID_COUNTS,
    INVALID_WIDTH,
    INVALID_ACTION,
    INVALID_LAYOUT,
    INVALID_DESCRIPTOR,
    INVALID_RECORD,
    INVALID_ID,
    INVALID_STATE,
    INVALID_SEQUENCE,
    INVALID_ADDRESS,
    FORMAT_OVERFLOW,
    HORIZON_OVERFLOW,
    ARITHMETIC_OVERFLOW,
    RESOURCE_LIMIT,
    NOT_READY,
};

inline const char* statusName(Status status) {
    switch (status) {
      case Status::OK: return "ok";
      case Status::INVALID_COUNTS: return "invalid-counts";
      case Status::INVALID_WIDTH: return "invalid-width";
      case Status::INVALID_ACTION: return "invalid-action";
      case Status::INVALID_LAYOUT: return "invalid-layout";
      case Status::INVALID_DESCRIPTOR: return "invalid-descriptor";
      case Status::INVALID_RECORD: return "invalid-record";
      case Status::INVALID_ID: return "invalid-id";
      case Status::INVALID_STATE: return "invalid-state";
      case Status::INVALID_SEQUENCE: return "invalid-sequence";
      case Status::INVALID_ADDRESS: return "invalid-address";
      case Status::FORMAT_OVERFLOW: return "format-overflow";
      case Status::HORIZON_OVERFLOW: return "horizon-overflow";
      case Status::ARITHMETIC_OVERFLOW: return "arithmetic-overflow";
      case Status::RESOURCE_LIMIT: return "resource-limit";
      case Status::NOT_READY: return "not-ready";
    }
    return "invalid-status";
}

inline bool checkedAdd(uint64_t left, uint64_t right, uint64_t& result) {
    result = 0;
    if (right > std::numeric_limits<uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

inline bool checkedMultiply(uint64_t left, uint64_t right, uint64_t& result) {
    result = 0;
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

inline uint8_t bitWidth(uint64_t value) {
    uint8_t bits = 1;
    while (value >>= 1)
        ++bits;
    return bits;
}

inline uint64_t lowMask(unsigned bits) {
    assert(bits <= 64);
    return bits == 64 ? std::numeric_limits<uint64_t>::max()
        : bits == 0 ? 0 : (uint64_t{1} << bits) - 1;
}

struct Requirements {
    Preset preset = Preset::ADAPTIVE_RICH_V2;
    ActionEncoding action_encoding = ActionEncoding::DEFAULT;
    uint64_t vertex_count = 0;
    uint64_t max_vertex_id = 0;
    uint64_t record_count = 0;
    uint64_t traversal_count = 1;
    uint8_t carrier_bytes = 0;
    uint8_t minimum_mantissa_bits = 1;
    bool max_vertex_id_known = false;
    bool require_prefetch_action = false;
};

struct Layout {
    Preset preset = Preset::ADAPTIVE_RICH_V2;
    ActionEncoding action_encoding = ActionEncoding::NONE;
    uint8_t carrier_bytes = 0;
    uint8_t id_bits = 0;
    uint8_t state_bits = 0;
    uint8_t distance_bits = 0;
    uint8_t exponent_bits = 0;
    uint8_t action_bits = 0;
    uint8_t joint_token_bits = 0;
    uint8_t horizon_bits = 0;
    uint8_t sequence_bits = 0;
    uint8_t deadline_bits = 0;
    uint64_t max_finite_distance = 0;

    bool operator==(const Layout& other) const {
        return preset == other.preset &&
            action_encoding == other.action_encoding &&
            carrier_bytes == other.carrier_bytes &&
            id_bits == other.id_bits && state_bits == other.state_bits &&
            distance_bits == other.distance_bits &&
            exponent_bits == other.exponent_bits &&
            action_bits == other.action_bits &&
            joint_token_bits == other.joint_token_bits &&
            horizon_bits == other.horizon_bits &&
            sequence_bits == other.sequence_bits &&
            deadline_bits == other.deadline_bits &&
            max_finite_distance == other.max_finite_distance;
    }
};

inline bool compact(const Layout& layout) {
    return layout.preset == Preset::SCALE6_V1 ||
        layout.preset == Preset::ADAPTIVE_COMPACT_V2;
}

inline bool adaptive(const Layout& layout) {
    return layout.preset == Preset::ADAPTIVE_RICH_V2 ||
        layout.preset == Preset::ADAPTIVE_COMPACT_V2;
}

inline uint8_t actionBits(ActionEncoding encoding) {
    switch (encoding) {
      case ActionEncoding::NONE: return 0;
      case ActionEncoding::DIRECT4: return 4;
      case ActionEncoding::ENUMERATED2: return 2;
      case ActionEncoding::DEFAULT: break;
    }
    return UINT8_MAX;
}

inline unsigned usedBits(const Layout& layout) {
    return unsigned(layout.id_bits) + layout.state_bits +
        layout.distance_bits + layout.action_bits + layout.joint_token_bits;
}

inline Status validateLayout(const Layout& layout) {
    if ((layout.carrier_bytes != 4 && layout.carrier_bytes != 8) ||
        layout.id_bits == 0 || layout.id_bits > 64 ||
        layout.horizon_bits < 31 || layout.horizon_bits > 63 ||
        layout.max_finite_distance != lowMask(layout.horizon_bits) ||
        actionBits(layout.action_encoding) != layout.action_bits ||
        usedBits(layout) > unsigned(layout.carrier_bytes) * 8) {
        return Status::INVALID_LAYOUT;
    }
    if (compact(layout)) {
        if (layout.state_bits != 0 || layout.distance_bits != 0 ||
            layout.exponent_bits != 0 || layout.action_bits != 0 ||
            layout.joint_token_bits != bitWidth(2u * layout.horizon_bits + 1u)) {
            return Status::INVALID_LAYOUT;
        }
    } else {
        if (layout.state_bits != 2 || layout.joint_token_bits != 0 ||
            layout.exponent_bits != bitWidth(layout.horizon_bits) ||
            layout.distance_bits <= layout.exponent_bits) {
            return Status::INVALID_LAYOUT;
        }
    }
    switch (layout.preset) {
      case Preset::FULL14_V1:
        if (layout.carrier_bytes != 4 || layout.id_bits > 18 ||
            layout.distance_bits + layout.state_bits + layout.action_bits != 14 ||
            layout.horizon_bits != 31 || layout.sequence_bits != 64 ||
            layout.deadline_bits != 21) {
            return Status::INVALID_LAYOUT;
        }
        break;
      case Preset::SCALE6_V1:
        if (layout.carrier_bytes != 4 || layout.id_bits != 26 ||
            layout.horizon_bits != 31 || layout.sequence_bits != 32 ||
            layout.deadline_bits != 32) {
            return Status::INVALID_LAYOUT;
        }
        break;
      case Preset::ADAPTIVE_RICH_V2:
        if (usedBits(layout) != unsigned(layout.carrier_bytes) * 8 ||
            layout.sequence_bits != 64 || layout.deadline_bits != 64) {
            return Status::INVALID_LAYOUT;
        }
        break;
      case Preset::ADAPTIVE_COMPACT_V2:
        if (layout.sequence_bits != 64 || layout.deadline_bits != 64)
            return Status::INVALID_LAYOUT;
        break;
      default:
        return Status::INVALID_LAYOUT;
    }
    return Status::OK;
}

inline Status selectLayout(const Requirements& requirements, Layout& output) {
    output = Layout{};
    if (requirements.vertex_count == 0 || requirements.record_count == 0 ||
        requirements.traversal_count == 0) {
        return Status::INVALID_COUNTS;
    }
    const uint64_t maximum_id = requirements.max_vertex_id_known
        ? requirements.max_vertex_id : requirements.vertex_count - 1;
    if (maximum_id >= requirements.vertex_count)
        return Status::INVALID_ID;
    if ((requirements.carrier_bytes != 0 && requirements.carrier_bytes != 4 &&
         requirements.carrier_bytes != 8) ||
        requirements.minimum_mantissa_bits == 0 ||
        requirements.minimum_mantissa_bits > 63) {
        return Status::INVALID_WIDTH;
    }
    uint64_t total_records = 0;
    if (!checkedMultiply(requirements.record_count, requirements.traversal_count,
                         total_records)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    Layout layout;
    layout.preset = requirements.preset;
    layout.id_bits = bitWidth(maximum_id);
    layout.horizon_bits = 31;
    layout.sequence_bits = 64;
    layout.deadline_bits = 64;
    const bool is_compact = compact(layout);
    layout.action_encoding = requirements.action_encoding == ActionEncoding::DEFAULT
        ? (is_compact ? ActionEncoding::NONE : ActionEncoding::DIRECT4)
        : requirements.action_encoding;
    layout.action_bits = actionBits(layout.action_encoding);
    if (layout.action_bits == UINT8_MAX ||
        (is_compact && layout.action_encoding != ActionEncoding::NONE) ||
        (requirements.require_prefetch_action && layout.action_bits == 0)) {
        return Status::INVALID_ACTION;
    }

    switch (layout.preset) {
      case Preset::FULL14_V1:
        layout.id_bits = bitWidth(requirements.vertex_count - 1);
        layout.carrier_bytes = 4;
        layout.state_bits = 2;
        layout.exponent_bits = 5;
        layout.distance_bits = 12 - layout.action_bits;
        layout.deadline_bits = 21;
        if (layout.id_bits > 18 ||
            layout.distance_bits < layout.exponent_bits +
                requirements.minimum_mantissa_bits) {
            return Status::FORMAT_OVERFLOW;
        }
        if (requirements.record_count > lowMask(20))
            return Status::HORIZON_OVERFLOW;
        break;
      case Preset::SCALE6_V1:
        layout.id_bits = 26;
        layout.carrier_bytes = 4;
        layout.joint_token_bits = 6;
        layout.sequence_bits = 32;
        layout.deadline_bits = 32;
        if (bitWidth(requirements.vertex_count - 1) > 26)
            return Status::FORMAT_OVERFLOW;
        if (requirements.record_count > lowMask(31))
            return Status::HORIZON_OVERFLOW;
        break;
      case Preset::ADAPTIVE_RICH_V2:
      case Preset::ADAPTIVE_COMPACT_V2: {
        // Preserve at least the uint31 reference range before expanding it.
        layout.horizon_bits = std::max<uint8_t>(
            31, bitWidth(requirements.record_count));
        if (layout.horizon_bits > 63)
            return Status::ARITHMETIC_OVERFLOW;
        unsigned minimum = layout.id_bits;
        if (is_compact) {
            layout.joint_token_bits = bitWidth(2u * layout.horizon_bits + 1u);
            minimum += layout.joint_token_bits;
        } else {
            layout.state_bits = 2;
            layout.exponent_bits = bitWidth(layout.horizon_bits);
            minimum += layout.state_bits + layout.action_bits +
                layout.exponent_bits + requirements.minimum_mantissa_bits;
        }
        layout.carrier_bytes = requirements.carrier_bytes != 0
            ? requirements.carrier_bytes : minimum <= 32 ? 4 : 8;
        if (minimum > unsigned(layout.carrier_bytes) * 8)
            return Status::FORMAT_OVERFLOW;
        if (!is_compact) {
            layout.distance_bits = unsigned(layout.carrier_bytes) * 8 -
                layout.id_bits - layout.state_bits - layout.action_bits;
        }
        break;
      }
      default:
        return Status::INVALID_LAYOUT;
    }
    if (requirements.carrier_bytes != 0 &&
        requirements.carrier_bytes != layout.carrier_bytes) {
        return Status::INVALID_WIDTH;
    }
    layout.max_finite_distance = lowMask(layout.horizon_bits);
    uint64_t bytes = 0, maximum_deadline = 0;
    if (!checkedMultiply(requirements.record_count, layout.carrier_bytes, bytes) ||
        (adaptive(layout) &&
         !checkedAdd(total_records, layout.max_finite_distance, maximum_deadline))) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    const Status status = validateLayout(layout);
    if (status == Status::OK)
        output = layout;
    return status;
}

inline Status validateConfiguration(const Requirements& requirements, const Layout& layout) {
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    Layout selected;
    const Status status = selectLayout(requirements, selected);
    if (status != Status::OK)
        return status;
    return selected == layout ? Status::OK : Status::INVALID_LAYOUT;
}

inline Status packLayout(const Layout& layout, uint64_t& descriptor) {
    descriptor = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    descriptor = uint64_t{0xE2} |
        (uint64_t(layout.preset) << 8) |
        (uint64_t(layout.carrier_bytes) << 16) |
        (uint64_t(layout.id_bits) << 24) |
        (uint64_t(layout.distance_bits) << 32) |
        (uint64_t(layout.exponent_bits) << 40) |
        (uint64_t(layout.horizon_bits) << 48) |
        (uint64_t(layout.action_encoding) << 56);
    return Status::OK;
}

inline Status unpackLayout(uint64_t descriptor, Layout& output) {
    output = Layout{};
    if ((descriptor & 0xFFu) != 0xE2u)
        return Status::INVALID_DESCRIPTOR;
    Layout layout;
    layout.preset = static_cast<Preset>((descriptor >> 8) & 0xFFu);
    layout.carrier_bytes = (descriptor >> 16) & 0xFFu;
    layout.id_bits = (descriptor >> 24) & 0xFFu;
    layout.distance_bits = (descriptor >> 32) & 0xFFu;
    layout.exponent_bits = (descriptor >> 40) & 0xFFu;
    layout.horizon_bits = (descriptor >> 48) & 0xFFu;
    layout.action_encoding = static_cast<ActionEncoding>(descriptor >> 56);
    if (layout.horizon_bits < 31 || layout.horizon_bits > 63)
        return Status::INVALID_LAYOUT;
    layout.max_finite_distance = lowMask(layout.horizon_bits);
    layout.action_bits = actionBits(layout.action_encoding);
    layout.state_bits = compact(layout) ? 0 : 2;
    layout.joint_token_bits = compact(layout)
        ? bitWidth(2u * layout.horizon_bits + 1u) : 0;
    layout.sequence_bits = layout.preset == Preset::SCALE6_V1 ? 32 : 64;
    layout.deadline_bits = layout.preset == Preset::FULL14_V1 ? 21
        : layout.preset == Preset::SCALE6_V1 ? 32 : 64;
    const Status status = validateLayout(layout);
    if (status == Status::OK)
        output = layout;
    return status;
}

namespace detail {

inline Status encodeAction(const Layout& layout, uint32_t lead, uint64_t& code) {
    code = 0;
    switch (layout.action_encoding) {
      case ActionEncoding::NONE:
        return lead == 0 ? Status::OK : Status::INVALID_ACTION;
      case ActionEncoding::DIRECT4:
        if (lead > 15)
            return Status::INVALID_ACTION;
        code = lead;
        return Status::OK;
      case ActionEncoding::ENUMERATED2:
        if (lead == 0) return Status::OK;
        if (lead == 8) code = 1;
        else if (lead == 12) code = 2;
        else if (lead == 15) code = 3;
        else return Status::INVALID_ACTION;
        return Status::OK;
      case ActionEncoding::DEFAULT:
        return Status::INVALID_ACTION;
    }
    return Status::INVALID_ACTION;
}

inline uint32_t decodeAction(const Layout& layout, uint64_t code) {
    if (layout.action_encoding == ActionEncoding::ENUMERATED2) {
        static constexpr uint32_t leads[4] = {0, 8, 12, 15};
        return leads[code & 3u];
    }
    return static_cast<uint32_t>(code);
}

inline uint64_t encodeDistance(const Layout& layout, uint64_t distance) {
    const unsigned exponent = bitWidth(distance) - 1;
    const unsigned mantissa_bits = layout.distance_bits - layout.exponent_bits;
    const uint64_t fraction = distance - (uint64_t{1} << exponent);
    const uint64_t mantissa = exponent >= mantissa_bits
        ? fraction >> (exponent - mantissa_bits)
        : fraction << (mantissa_bits - exponent);
    return (uint64_t(exponent) << mantissa_bits) | mantissa;
}

inline bool decodeDistance(const Layout& layout, uint64_t code, uint64_t& distance) {
    distance = 0;
    if (code == lowMask(layout.distance_bits)) {
        distance = layout.max_finite_distance;
        return true;
    }
    const unsigned mantissa_bits = layout.distance_bits - layout.exponent_bits;
    const unsigned exponent = code >> mantissa_bits;
    if (exponent >= layout.horizon_bits)
        return false;
    const uint64_t base = uint64_t{1} << exponent;
    const uint64_t mantissa = code & lowMask(mantissa_bits);
    uint64_t bucket_upper;
    if (exponent >= mantissa_bits) {
        bucket_upper = (mantissa + 1) << (exponent - mantissa_bits);
    } else {
        const unsigned shift = mantissa_bits - exponent;
        bucket_upper = (mantissa + 1 + lowMask(shift)) >> shift;
    }
    distance = base + std::max<uint64_t>(1, bucket_upper) - 1;
    return true;
}

}  // namespace detail

struct DecodedRecord {
    uint64_t destination = 0;
    uint64_t distance = 0;
    uint32_t action_delta = 0;
    State state = State::UNKNOWN;
    bool distance_valid = false;
};

inline Status encodeRecord(
        const Layout& layout, uint64_t destination, uint64_t distance,
        State state, uint32_t action_delta, uint64_t& word) {
    word = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (destination > lowMask(layout.id_bits))
        return Status::INVALID_ID;
    if (state != State::UNKNOWN && state != State::FINITE &&
        state != State::DEAD && state != State::WRAP) {
        return Status::INVALID_STATE;
    }
    if (distance > layout.max_finite_distance ||
        ((state == State::FINITE || state == State::WRAP) && distance == 0)) {
        return Status::HORIZON_OVERFLOW;
    }
    uint64_t action = 0;
    const Status action_status = detail::encodeAction(layout, action_delta, action);
    if (action_status != Status::OK)
        return action_status;
    if (compact(layout)) {
        const uint64_t token = state == State::UNKNOWN ? 0
            : state == State::DEAD ? 1
            : 2u + (state == State::WRAP ? layout.horizon_bits : 0u) +
                bitWidth(distance) - 1u;
        word = destination | (token << layout.id_bits);
    } else {
        const uint64_t reference = detail::encodeDistance(
            layout, std::max<uint64_t>(1, distance));
        word = destination | (reference << layout.id_bits) |
            (uint64_t(state) << (layout.id_bits + layout.distance_bits));
        if (layout.action_bits != 0) {
            word |= action <<
                (layout.id_bits + layout.distance_bits + layout.state_bits);
        }
    }
    return Status::OK;
}

inline Status decodeRecord(
        const Layout& layout, uint64_t word, DecodedRecord& output) {
    output = DecodedRecord{};
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if ((word & ~lowMask(usedBits(layout))) != 0)
        return Status::INVALID_RECORD;
    DecodedRecord decoded;
    decoded.destination = word & lowMask(layout.id_bits);
    if (compact(layout)) {
        const uint64_t token = (word >> layout.id_bits) & lowMask(layout.joint_token_bits);
        if (token > 2u * layout.horizon_bits + 1u)
            return Status::INVALID_RECORD;
        decoded.state = token == 0 ? State::UNKNOWN : token == 1 ? State::DEAD
            : token >= 2u + layout.horizon_bits ? State::WRAP : State::FINITE;
        if (decoded.state == State::FINITE || decoded.state == State::WRAP) {
            const unsigned exponent = token - 2u -
                (decoded.state == State::WRAP ? layout.horizon_bits : 0u);
            decoded.distance = lowMask(exponent + 1);
            decoded.distance_valid = true;
        }
    } else {
        const uint64_t reference =
            (word >> layout.id_bits) & lowMask(layout.distance_bits);
        decoded.state = static_cast<State>(
            (word >> (layout.id_bits + layout.distance_bits)) & 3u);
        const bool valid = detail::decodeDistance(layout, reference, decoded.distance);
        if (!valid && adaptive(layout))
            return Status::INVALID_RECORD;
        if (!valid)
            decoded.state = State::UNKNOWN;
        decoded.distance_valid = valid &&
            (decoded.state == State::FINITE || decoded.state == State::WRAP);
        if (layout.action_bits != 0) {
            decoded.action_delta = detail::decodeAction(layout,
                (word >> (layout.id_bits + layout.distance_bits + layout.state_bits)) &
                lowMask(layout.action_bits));
        }
    }
    output = decoded;
    return Status::OK;
}

inline Status setAction(
        const Layout& layout, uint64_t word, uint32_t action_delta,
        uint64_t& output) {
    output = 0;
    DecodedRecord decoded;
    const Status status = decodeRecord(layout, word, decoded);
    if (status != Status::OK)
        return status;
    uint64_t action = 0;
    const Status action_status = detail::encodeAction(layout, action_delta, action);
    if (action_status != Status::OK)
        return action_status;
    output = word;
    if (layout.action_bits != 0) {
        const unsigned shift = layout.id_bits + layout.distance_bits + layout.state_bits;
        output = (word & ~(lowMask(layout.action_bits) << shift)) | (action << shift);
    }
    return Status::OK;
}

struct RecordWindow {
    static constexpr std::size_t kRecords = 16;
    std::array<uint64_t, kRecords> words{};
    uint64_t remaining_records = 0;
    uint64_t vertex_count = 0;
    uint16_t valid_mask = 0;
};

struct PrefetchTarget {
    uint64_t destination = 0;
    uint64_t record = 0;
    uint32_t lead = 0;
    bool valid = false;
};

inline Status selectWindowTarget(
        const Layout& layout, const RecordWindow& window,
        uint64_t vertices_per_line, PrefetchTarget& output) {
    output = PrefetchTarget{};
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (window.remaining_records == 0 || window.vertex_count == 0 ||
        vertices_per_line == 0) {
        return Status::INVALID_COUNTS;
    }
    if ((window.valid_mask & 1u) == 0)
        return Status::NOT_READY;
    DecodedRecord current;
    Status status = decodeRecord(layout, window.words[0], current);
    if (status != Status::OK)
        return status;
    if (current.destination >= window.vertex_count)
        return Status::INVALID_ID;
    if (layout.action_bits != 0) {
        const uint32_t lead = current.action_delta;
        if (lead == 0)
            return Status::OK;
        if (lead >= window.remaining_records || lead >= RecordWindow::kRecords)
            return Status::INVALID_RECORD;
        if ((window.valid_mask & (uint16_t{1} << lead)) == 0)
            return Status::NOT_READY;
        DecodedRecord target;
        status = decodeRecord(layout, window.words[lead], target);
        if (status != Status::OK)
            return status;
        if (target.destination >= window.vertex_count)
            return Status::INVALID_ID;
        output = {target.destination, window.words[lead], lead, true};
        return Status::OK;
    }
    if (!compact(layout))
        return Status::OK;
    const uint32_t count = static_cast<uint32_t>(
        std::min<uint64_t>(RecordWindow::kRecords, window.remaining_records));
    const uint16_t required = static_cast<uint16_t>(lowMask(count));
    if ((window.valid_mask & required) != required)
        return Status::NOT_READY;
    const uint64_t current_line = current.destination / vertices_per_line;
    std::array<uint64_t, RecordWindow::kRecords> previous_lines{};
    std::size_t previous_count = 0;
    uint64_t best_distance = 0;
    uint32_t best_error = 0;
    for (uint32_t lead = 1; lead < count; ++lead) {
        DecodedRecord target;
        status = decodeRecord(layout, window.words[lead], target);
        if (status != Status::OK)
            return status;
        if (target.destination >= window.vertex_count)
            return Status::INVALID_ID;
        const uint64_t line = target.destination / vertices_per_line;
        const bool seen = std::find(
            previous_lines.begin(), previous_lines.begin() + previous_count,
            line) != previous_lines.begin() + previous_count;
        previous_lines[previous_count++] = line;
        if (lead < 8 || line == current_line || seen)
            continue;
        const uint64_t distance = target.distance_valid
            ? target.distance : layout.max_finite_distance;
        const uint32_t error = lead > 10 ? lead - 10 : 10 - lead;
        if (!output.valid || distance < best_distance ||
            (distance == best_distance && error < best_error)) {
            output = {target.destination, window.words[lead], lead, true};
            best_distance = distance;
            best_error = error;
        }
    }
    return Status::OK;
}

struct Prediction {
    uint64_t destination = 0;
    uint64_t sequence = 0;
    uint64_t deadline = 0;
    uint64_t normalized_record = 0;
    uint32_t action_delta = 0;
    State state = State::UNKNOWN;
};

inline Status makePrediction(
        const Layout& layout, uint64_t word, uint64_t sequence,
        bool has_next_iteration, Prediction& output) {
    output = Prediction{};
    DecodedRecord decoded;
    const Status decoded_status = decodeRecord(layout, word, decoded);
    if (decoded_status != Status::OK)
        return decoded_status;
    if ((adaptive(layout) && sequence == 0) ||
        (layout.sequence_bits == 32 && sequence > UINT32_MAX)) {
        return Status::INVALID_SEQUENCE;
    }
    Prediction prediction;
    prediction.destination = decoded.destination;
    prediction.sequence = sequence;
    prediction.action_delta = decoded.action_delta;
    prediction.state = decoded.state == State::WRAP
        ? (has_next_iteration ? State::FINITE : State::DEAD) : decoded.state;
    prediction.normalized_record = word;
    if (decoded.state == State::WRAP) {
        if (compact(layout)) {
            const uint64_t token = has_next_iteration
                ? ((word >> layout.id_bits) & lowMask(layout.joint_token_bits)) -
                    layout.horizon_bits
                : 1;
            prediction.normalized_record =
                decoded.destination | (token << layout.id_bits);
        } else {
            const unsigned shift = layout.id_bits + layout.distance_bits;
            prediction.normalized_record =
                (word & ~(uint64_t{3} << shift)) |
                (uint64_t(prediction.state) << shift);
        }
    }
    if (prediction.state == State::FINITE) {
        if (layout.deadline_bits == 64) {
            if (!checkedAdd(sequence, decoded.distance, prediction.deadline))
                return Status::ARITHMETIC_OVERFLOW;
        } else {
            const uint64_t mask = lowMask(layout.deadline_bits);
            if (decoded.distance > lowMask(layout.deadline_bits - 1))
                return Status::HORIZON_OVERFLOW;
            prediction.deadline = ((sequence & mask) + decoded.distance) & mask;
        }
    }
    output = prediction;
    return Status::OK;
}

inline Status recordAddress(
        const Layout& layout, uint64_t base, uint64_t index,
        uint64_t record_count, uint64_t& address) {
    address = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (record_count == 0 || index >= record_count)
        return Status::INVALID_COUNTS;
    if (base % layout.carrier_bytes != 0)
        return Status::INVALID_ADDRESS;
    uint64_t bytes = 0, last = 0, offset = 0;
    if (!checkedMultiply(record_count, layout.carrier_bytes, bytes) ||
        !checkedAdd(base, bytes - 1, last) ||
        !checkedMultiply(index, layout.carrier_bytes, offset) ||
        !checkedAdd(base, offset, address)) {
        address = 0;
        return Status::ARITHMETIC_OVERFLOW;
    }
    return Status::OK;
}

inline Status recordPosition(
        const Layout& layout, uint64_t address, uint64_t base,
        uint64_t record_count, uint64_t iteration_base,
        uint64_t& index, uint64_t& sequence) {
    index = sequence = 0;
    uint64_t first = 0;
    const Status status = recordAddress(layout, base, 0, record_count, first);
    if (status != Status::OK)
        return status;
    if (address < base || address % layout.carrier_bytes != 0)
        return Status::INVALID_ADDRESS;
    const uint64_t position = (address - base) / layout.carrier_bytes;
    if (position >= record_count)
        return Status::INVALID_ADDRESS;
    uint64_t value = 0;
    if (!checkedAdd(iteration_base, position + 1, value))
        return Status::ARITHMETIC_OVERFLOW;
    index = position;
    sequence = layout.sequence_bits == 32 ? value & UINT32_MAX : value;
    return Status::OK;
}

inline Status windowStorage(
        const Layout& layout, uint64_t records, uint64_t line_bytes,
        uint64_t& lines, uint64_t& bytes) {
    lines = bytes = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (records == 0 || line_bytes < layout.carrier_bytes ||
        (line_bytes & (line_bytes - 1)) != 0 ||
        line_bytes % layout.carrier_bytes != 0) {
        return Status::INVALID_WIDTH;
    }
    uint64_t payload = 0, span = 0;
    if (!checkedMultiply(records, layout.carrier_bytes, payload) ||
        !checkedAdd(payload, line_bytes - layout.carrier_bytes, span)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    const uint64_t count = span / line_bytes + (span % line_bytes != 0);
    uint64_t storage = 0;
    if (!checkedMultiply(count, line_bytes, storage))
        return Status::ARITHMETIC_OVERFLOW;
    lines = count;
    bytes = storage;
    return Status::OK;
}

}  // namespace ecg_record

#endif
