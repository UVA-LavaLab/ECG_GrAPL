#ifndef GRAPHBREW_ECG_RECORD_H
#define GRAPHBREW_ECG_RECORD_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>

#include "ecg_ref32.h"

namespace ecg_record {

using State = ecg_ref32::State;

enum class Mechanism : uint8_t {
    TRANSPORT,
    REPLACEMENT,
    PREFETCH,
    REPLACEMENT_PREFETCH,
    INVALID = 0xFF,
};

enum class Status : uint8_t {
    OK,
    INVALID_COUNTS,
    INVALID_WIDTH,
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

inline const char* mechanismName(Mechanism mechanism) {
    switch (mechanism) {
      case Mechanism::TRANSPORT: return "transport";
      case Mechanism::REPLACEMENT: return "replacement";
      case Mechanism::PREFETCH: return "prefetch";
      case Mechanism::REPLACEMENT_PREFETCH: return "replacement-prefetch";
      case Mechanism::INVALID: break;
    }
    return "invalid";
}

inline Status parseMechanismName(const char* name, Mechanism& mechanism) {
    mechanism = Mechanism::INVALID;
    if (!name)
        return Status::INVALID_LAYOUT;
    for (const Mechanism candidate : {Mechanism::TRANSPORT, Mechanism::REPLACEMENT,
             Mechanism::PREFETCH, Mechanism::REPLACEMENT_PREFETCH}) {
        if (std::strcmp(name, mechanismName(candidate)) == 0) {
            mechanism = candidate;
            return Status::OK;
        }
    }
    return Status::INVALID_LAYOUT;
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

enum class PropertyKind : uint8_t { F32, U32, U64 };
enum class TraversalMode : uint8_t { DENSE_EXACT, ORDERED_FILTERED };

struct PropertyDescriptor {
    PropertyKind kind = PropertyKind::F32;
    uint32_t stride_bytes = 4;
    TraversalMode traversal = TraversalMode::DENSE_EXACT;

    bool operator==(const PropertyDescriptor& other) const {
        return kind == other.kind && stride_bytes == other.stride_bytes &&
            traversal == other.traversal;
    }
};

inline const char* propertyKindName(PropertyKind kind) {
    switch (kind) {
      case PropertyKind::F32: return "f32";
      case PropertyKind::U32: return "u32";
      case PropertyKind::U64: return "u64";
    }
    return "invalid";
}

inline const char* traversalName(TraversalMode mode) {
    switch (mode) {
      case TraversalMode::DENSE_EXACT: return "dense-exact";
      case TraversalMode::ORDERED_FILTERED: return "ordered-filtered";
    }
    return "invalid";
}

inline uint8_t propertyBytes(PropertyKind kind) {
    return kind == PropertyKind::U64 ? 8
        : kind == PropertyKind::F32 || kind == PropertyKind::U32 ? 4 : 0;
}

inline Status validateProperty(const PropertyDescriptor& property) {
    const uint8_t bytes = propertyBytes(property.kind);
    if (bytes == 0 || property.stride_bytes < bytes ||
        property.stride_bytes > UINT16_MAX || property.stride_bytes % bytes != 0)
        return Status::INVALID_WIDTH;
    if (property.traversal != TraversalMode::DENSE_EXACT &&
        property.traversal != TraversalMode::ORDERED_FILTERED)
        return Status::INVALID_DESCRIPTOR;
    return Status::OK;
}

inline Status packProperty(const PropertyDescriptor& property, uint64_t& descriptor) {
    descriptor = 0;
    const Status status = validateProperty(property);
    if (status != Status::OK)
        return status;
    // Zero retains the original PR configuration and instruction ABI.
    if (!(property == PropertyDescriptor{}))
        descriptor = 0xED | (uint64_t(property.kind) << 8) |
            (uint64_t(property.traversal) << 10) | (uint64_t(property.stride_bytes) << 16);
    return Status::OK;
}

inline Status unpackProperty(uint64_t descriptor, PropertyDescriptor& property) {
    property = PropertyDescriptor{};
    if (descriptor == 0)
        return Status::OK;
    if ((descriptor & 0xFF) != 0xED || (descriptor & ~uint64_t{0xFFFF07FF}) != 0)
        return Status::INVALID_DESCRIPTOR;
    PropertyDescriptor decoded{
        static_cast<PropertyKind>((descriptor >> 8) & 3),
        static_cast<uint32_t>(descriptor >> 16),
        static_cast<TraversalMode>((descriptor >> 10) & 1)};
    const Status status = validateProperty(decoded);
    if (status == Status::OK)
        property = decoded;
    return status;
}

inline Status propertyExtent(
        const PropertyDescriptor& property, uint64_t vertices, uint64_t& extent) {
    extent = 0;
    const Status status = validateProperty(property);
    if (status != Status::OK)
        return status;
    if (vertices == 0)
        return Status::INVALID_COUNTS;
    uint64_t offset = 0;
    if (!checkedMultiply(vertices - 1, property.stride_bytes, offset) ||
        !checkedAdd(offset, propertyBytes(property.kind), extent))
        return Status::ARITHMETIC_OVERFLOW;
    return Status::OK;
}

inline Status propertyAddress(
        const PropertyDescriptor& property, uint64_t base, uint64_t vertex,
        uint64_t& address) {
    address = 0;
    const Status status = validateProperty(property);
    if (status != Status::OK)
        return status;
    if (base % propertyBytes(property.kind) != 0)
        return Status::INVALID_ADDRESS;
    uint64_t offset = 0, last = 0, start = 0;
    if (!checkedMultiply(vertex, property.stride_bytes, offset) ||
        !checkedAdd(base, offset, start) ||
        !checkedAdd(start, propertyBytes(property.kind) - 1, last))
        return Status::ARITHMETIC_OVERFLOW;
    address = start;
    return Status::OK;
}

class PassCursor {
  public:
    Status configure(uint64_t records, TraversalMode mode) {
        if (records == 0 || records_ != 0)
            return Status::INVALID_COUNTS;
        if (mode != TraversalMode::DENSE_EXACT && mode != TraversalMode::ORDERED_FILTERED)
            return Status::INVALID_DESCRIPTOR;
        records_ = records;
        mode_ = mode;
        return Status::OK;
    }

    Status begin() {
        if (open_ || records_ == 0)
            return Status::INVALID_SEQUENCE;
        uint64_t end = 0;
        if (!checkedAdd(base_, records_, end))
            return Status::ARITHMETIC_OVERFLOW;
        last_ = 0;
        open_ = true;
        return Status::OK;
    }

    Status consume(uint64_t index) {
        if (!open_ || index >= records_ || index + 1 <= last_ ||
            (mode_ == TraversalMode::DENSE_EXACT && index != last_))
            return Status::INVALID_SEQUENCE;
        const uint64_t gap = index - last_;
        uint64_t consumed = 0, skipped = 0;
        if (!checkedAdd(consumed_, 1, consumed) || !checkedAdd(skipped_, gap, skipped))
            return Status::ARITHMETIC_OVERFLOW;
        consumed_ = consumed;
        skipped_ = skipped;
        last_ = index + 1;
        return Status::OK;
    }

    Status close() {
        if (!open_ || (mode_ == TraversalMode::DENSE_EXACT && last_ != records_))
            return Status::INVALID_SEQUENCE;
        uint64_t end = 0, skipped = 0, passes = 0;
        if (!checkedAdd(base_, records_, end) ||
            !checkedAdd(skipped_, records_ - last_, skipped) ||
            !checkedAdd(passes_, 1, passes))
            return Status::ARITHMETIC_OVERFLOW;
        base_ = end;
        skipped_ = skipped;
        passes_ = passes;
        last_ = 0;
        open_ = false;
        return Status::OK;
    }

    bool open() const { return open_; }
    uint64_t base() const { return base_; }
    uint64_t sequence() const { return base_ + last_; }
    uint64_t passes() const { return passes_; }
    uint64_t consumed() const { return consumed_; }
    uint64_t skipped() const { return skipped_; }

  private:
    TraversalMode mode_ = TraversalMode::DENSE_EXACT;
    uint64_t records_ = 0, base_ = 0, last_ = 0;
    uint64_t passes_ = 0, consumed_ = 0, skipped_ = 0;
    bool open_ = false;
};

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
    uint64_t vertex_count = 0;
    uint64_t max_vertex_id = 0;
    uint64_t record_count = 0;
    uint64_t traversal_count = 1;
    uint8_t requested_record_bytes = 0;
    uint8_t minimum_mantissa_bits = 0;
    bool max_vertex_id_known = false;
};

// One joint state/reference grammar; only resolved bit widths vary by graph.
struct Layout {
    uint8_t record_bytes = 0;
    uint8_t id_bits = 0;
    uint8_t metadata_bits = 0;
    uint8_t horizon_bits = 0;
    uint8_t exponent_bits = 0;
    uint8_t mantissa_bits = 0;
    uint8_t sequence_bits = 64;
    uint8_t deadline_bits = 64;
    uint64_t codes_per_state = 0;
    uint64_t max_finite_distance = 0;

    bool operator==(const Layout& other) const {
        return record_bytes == other.record_bytes && id_bits == other.id_bits &&
            metadata_bits == other.metadata_bits && horizon_bits == other.horizon_bits &&
            exponent_bits == other.exponent_bits && mantissa_bits == other.mantissa_bits &&
            sequence_bits == other.sequence_bits && deadline_bits == other.deadline_bits &&
            codes_per_state == other.codes_per_state &&
            max_finite_distance == other.max_finite_distance;
    }
};

namespace detail {

inline bool precisionFor(
        unsigned metadata_bits, unsigned horizon_bits,
        uint8_t& mantissa_bits, uint64_t& codes_per_state) {
    mantissa_bits = 0;
    codes_per_state = 0;
    if (metadata_bits == 0 || metadata_bits > 63 ||
        horizon_bits == 0 || horizon_bits > 63) {
        return false;
    }
    using Wide = unsigned __int128;
    const Wide code_capacity = Wide{1} << metadata_bits;
    const Wide levels = (code_capacity - 2) / (2 * Wide{horizon_bits});
    if (levels == 0)
        return false;
    const uint8_t precision = bitWidth(static_cast<uint64_t>(levels)) - 1;
    const Wide codes = Wide{horizon_bits} << precision;
    if (2 + 2 * codes > code_capacity || codes > UINT64_MAX)
        return false;
    mantissa_bits = precision;
    codes_per_state = static_cast<uint64_t>(codes);
    return true;
}

inline uint64_t encodeDistance(const Layout& layout, uint64_t distance) {
    const unsigned exponent = bitWidth(distance) - 1;
    const uint64_t fraction = distance - (uint64_t{1} << exponent);
    const uint64_t mantissa = exponent >= layout.mantissa_bits
        ? fraction >> (exponent - layout.mantissa_bits)
        : fraction << (layout.mantissa_bits - exponent);
    return (uint64_t{exponent} << layout.mantissa_bits) | mantissa;
}

inline uint64_t decodeDistance(const Layout& layout, uint64_t code) {
    const unsigned exponent = code >> layout.mantissa_bits;
    const uint64_t mantissa = code & lowMask(layout.mantissa_bits);
    const uint64_t base = uint64_t{1} << exponent;
    uint64_t upper;
    if (exponent >= layout.mantissa_bits) {
        upper = (mantissa + 1) << (exponent - layout.mantissa_bits);
    } else {
        const unsigned shift = layout.mantissa_bits - exponent;
        upper = (mantissa + 1 + lowMask(shift)) >> shift;
    }
    return base + std::max<uint64_t>(1, upper) - 1;
}

}  // namespace detail

inline Status validateLayout(const Layout& layout) {
    if ((layout.record_bytes != 4 && layout.record_bytes != 8) ||
        layout.id_bits == 0 || layout.id_bits >= unsigned(layout.record_bytes) * 8 ||
        layout.metadata_bits != unsigned(layout.record_bytes) * 8 - layout.id_bits ||
        layout.horizon_bits == 0 || layout.horizon_bits > 63 ||
        layout.sequence_bits != 64 || layout.deadline_bits != 64) {
        return Status::INVALID_LAYOUT;
    }
    uint8_t mantissa = 0;
    uint64_t codes = 0;
    if (!detail::precisionFor(layout.metadata_bits, layout.horizon_bits, mantissa, codes) ||
        layout.mantissa_bits != mantissa || layout.codes_per_state != codes ||
        layout.exponent_bits != (layout.horizon_bits > 1
            ? bitWidth(layout.horizon_bits - 1) : 0) ||
        layout.max_finite_distance != lowMask(layout.horizon_bits)) {
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
    if ((requirements.requested_record_bytes != 0 &&
         requirements.requested_record_bytes != 4 &&
         requirements.requested_record_bytes != 8) ||
        requirements.minimum_mantissa_bits > 61) {
        return Status::INVALID_WIDTH;
    }
    Layout layout;
    layout.id_bits = bitWidth(maximum_id);
    layout.horizon_bits = bitWidth(requirements.record_count);
    if (layout.horizon_bits > 63)
        return Status::ARITHMETIC_OVERFLOW;
    layout.exponent_bits = layout.horizon_bits > 1
        ? bitWidth(layout.horizon_bits - 1) : 0;
    layout.max_finite_distance = lowMask(layout.horizon_bits);
    bool found = false;
    for (const uint8_t bytes : {uint8_t{4}, uint8_t{8}}) {
        if (requirements.requested_record_bytes != 0 &&
            requirements.requested_record_bytes != bytes) {
            continue;
        }
        if (layout.id_bits >= unsigned(bytes) * 8)
            continue;
        const unsigned metadata_bits = unsigned(bytes) * 8 - layout.id_bits;
        uint8_t mantissa = 0;
        uint64_t codes = 0;
        if (!detail::precisionFor(metadata_bits, layout.horizon_bits, mantissa, codes) ||
            mantissa < requirements.minimum_mantissa_bits) {
            continue;
        }
        layout.record_bytes = bytes;
        layout.metadata_bits = metadata_bits;
        layout.mantissa_bits = mantissa;
        layout.codes_per_state = codes;
        found = true;
        break;
    }
    if (!found)
        return Status::FORMAT_OVERFLOW;
    uint64_t total_records = 0, bytes = 0, maximum_deadline = 0;
    if (!checkedMultiply(requirements.record_count, requirements.traversal_count, total_records) ||
        !checkedMultiply(requirements.record_count, layout.record_bytes, bytes) ||
        !checkedAdd(total_records, layout.max_finite_distance, maximum_deadline)) {
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
    descriptor = uint64_t{0xEC} |
        (uint64_t{layout.record_bytes} << 8) |
        (uint64_t{layout.id_bits} << 16) |
        (uint64_t{layout.metadata_bits} << 24) |
        (uint64_t{layout.horizon_bits} << 32) |
        (uint64_t{layout.mantissa_bits} << 40) |
        (uint64_t{layout.exponent_bits} << 48);
    return Status::OK;
}

inline Status unpackLayout(uint64_t descriptor, Layout& output) {
    output = Layout{};
    if ((descriptor & 0xFFu) != 0xECu || (descriptor >> 56) != 0)
        return Status::INVALID_DESCRIPTOR;
    Layout layout;
    layout.record_bytes = (descriptor >> 8) & 0xFFu;
    layout.id_bits = (descriptor >> 16) & 0xFFu;
    layout.metadata_bits = (descriptor >> 24) & 0xFFu;
    layout.horizon_bits = (descriptor >> 32) & 0xFFu;
    layout.mantissa_bits = (descriptor >> 40) & 0xFFu;
    layout.exponent_bits = (descriptor >> 48) & 0xFFu;
    uint8_t expected_mantissa = 0;
    if (!detail::precisionFor(layout.metadata_bits, layout.horizon_bits,
                              expected_mantissa, layout.codes_per_state)) {
        return Status::INVALID_LAYOUT;
    }
    layout.max_finite_distance = lowMask(layout.horizon_bits);
    const Status status = validateLayout(layout);
    if (status == Status::OK)
        output = layout;
    return status;
}

inline Status writeLayoutFields(std::ostream& output, const Layout& layout) {
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    output << "record_bytes=" << unsigned(layout.record_bytes)
        << " id_bits=" << unsigned(layout.id_bits)
        << " metadata_bits=" << unsigned(layout.metadata_bits)
        << " horizon_bits=" << unsigned(layout.horizon_bits)
        << " exponent_bits=" << unsigned(layout.exponent_bits)
        << " mantissa_bits=" << unsigned(layout.mantissa_bits)
        << " sequence_bits=64 deadline_bits=64"
        << " state_encoding=joint-distance"
        << " prefetch_selection=record-window";
    return Status::OK;
}

struct DecodedRecord {
    uint64_t destination = 0;
    uint64_t distance = 0;
    State state = State::UNKNOWN;
    bool distance_valid = false;
};

inline Status encodeRecord(
        const Layout& layout, uint64_t destination, uint64_t distance,
        State state, uint64_t& word) {
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
    uint64_t token = state == State::UNKNOWN ? 0 : 1;
    if (state == State::FINITE || state == State::WRAP) {
        token = 2 + detail::encodeDistance(layout, distance);
        if (state == State::WRAP)
            token += layout.codes_per_state;
    }
    word = destination | (token << layout.id_bits);
    return Status::OK;
}

inline Status decodeRecord(
        const Layout& layout, uint64_t word, DecodedRecord& output) {
    output = DecodedRecord{};
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if ((word & ~lowMask(unsigned(layout.record_bytes) * 8)) != 0)
        return Status::INVALID_RECORD;
    const uint64_t token = word >> layout.id_bits;
    if (token > 2 * layout.codes_per_state + 1)
        return Status::INVALID_RECORD;
    DecodedRecord decoded;
    decoded.destination = word & lowMask(layout.id_bits);
    decoded.state = token == 0 ? State::UNKNOWN : token == 1 ? State::DEAD
        : token >= 2 + layout.codes_per_state ? State::WRAP : State::FINITE;
    if (decoded.state == State::FINITE || decoded.state == State::WRAP) {
        const uint64_t code = token - 2 -
            (decoded.state == State::WRAP ? layout.codes_per_state : 0);
        decoded.distance = detail::decodeDistance(layout, code);
        decoded.distance_valid = true;
    }
    output = decoded;
    return Status::OK;
}

struct Prediction {
    uint64_t destination = 0;
    uint64_t sequence = 0;
    uint64_t deadline = 0;
    uint64_t normalized_record = 0;
    State state = State::UNKNOWN;
};

inline Status makePrediction(
        const Layout& layout, uint64_t word, uint64_t sequence,
        bool has_next_iteration, Prediction& output,
        TraversalMode mode = TraversalMode::DENSE_EXACT) {
    output = Prediction{};
    if (mode != TraversalMode::DENSE_EXACT && mode != TraversalMode::ORDERED_FILTERED)
        return Status::INVALID_DESCRIPTOR;
    DecodedRecord decoded;
    const Status status = decodeRecord(layout, word, decoded);
    if (status != Status::OK)
        return status;
    if (sequence == 0)
        return Status::INVALID_SEQUENCE;
    Prediction prediction;
    prediction.destination = decoded.destination;
    prediction.sequence = sequence;
    prediction.state = decoded.state == State::WRAP
        ? (has_next_iteration ? State::FINITE : State::DEAD) : decoded.state;
    prediction.normalized_record = word;
    if (decoded.state == State::WRAP) {
        const uint64_t token = has_next_iteration
            ? (word >> layout.id_bits) - layout.codes_per_state : 1;
        prediction.normalized_record = decoded.destination | (token << layout.id_bits);
    }
    if (mode == TraversalMode::ORDERED_FILTERED &&
        (decoded.state == State::WRAP || decoded.state == State::DEAD)) {
        prediction.state = State::UNKNOWN;
        prediction.normalized_record = decoded.destination;
    }
    if (prediction.state == State::FINITE &&
        !checkedAdd(sequence, decoded.distance, prediction.deadline)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    output = prediction;
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

template<typename LineAt>
inline Status selectWindowTargetMapped(
        const Layout& layout, const RecordWindow& window,
        LineAt line_at, PrefetchTarget& output, TraversalMode mode) {
    output = PrefetchTarget{};
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (window.remaining_records == 0 || window.vertex_count == 0) {
        return Status::INVALID_COUNTS;
    }
    const uint32_t count = static_cast<uint32_t>(
        std::min<uint64_t>(RecordWindow::kRecords, window.remaining_records));
    const uint16_t required = static_cast<uint16_t>(lowMask(count));
    if ((window.valid_mask & required) != required)
        return Status::NOT_READY;
    DecodedRecord current;
    Status status = decodeRecord(layout, window.words[0], current);
    if (status != Status::OK)
        return status;
    if (current.destination >= window.vertex_count)
        return Status::INVALID_ID;
    const uint64_t current_line = line_at(current.destination);
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
        const uint64_t line = line_at(target.destination);
        const bool seen = std::find(
            previous_lines.begin(), previous_lines.begin() + previous_count,
            line) != previous_lines.begin() + previous_count;
        previous_lines[previous_count++] = line;
        if (lead < 8 || line == current_line || seen)
            continue;
        const uint64_t distance = target.distance_valid &&
            (mode == TraversalMode::DENSE_EXACT || target.state == State::FINITE)
            ? target.distance : std::numeric_limits<uint64_t>::max();
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

inline Status selectWindowTarget(
        const Layout& layout, const RecordWindow& window,
        uint64_t vertices_per_line, PrefetchTarget& output) {
    output = PrefetchTarget{};
    if (vertices_per_line == 0)
        return Status::INVALID_COUNTS;
    return selectWindowTargetMapped(layout, window,
        [vertices_per_line](uint64_t id) { return id / vertices_per_line; },
        output, TraversalMode::DENSE_EXACT);
}

inline Status selectWindowTarget(
        const Layout& layout, const RecordWindow& window,
        const PropertyDescriptor& property, uint64_t base, PrefetchTarget& output) {
    output = PrefetchTarget{};
    if (window.vertex_count == 0)
        return Status::INVALID_COUNTS;
    uint64_t last = 0;
    const Status status = propertyAddress(property, base, window.vertex_count - 1, last);
    if (status != Status::OK)
        return status;
    return selectWindowTargetMapped(layout, window,
        [base, &property](uint64_t id) { return (base + id * property.stride_bytes) / 64; },
        output, property.traversal);
}

inline Status recordAddress(
        const Layout& layout, uint64_t base, uint64_t index,
        uint64_t record_count, uint64_t& address) {
    address = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (record_count == 0 || index >= record_count)
        return Status::INVALID_COUNTS;
    if (base % layout.record_bytes != 0)
        return Status::INVALID_ADDRESS;
    uint64_t bytes = 0, last = 0, offset = 0;
    if (!checkedMultiply(record_count, layout.record_bytes, bytes) ||
        !checkedAdd(base, bytes - 1, last) ||
        !checkedMultiply(index, layout.record_bytes, offset) ||
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
    if (address < base || address % layout.record_bytes != 0)
        return Status::INVALID_ADDRESS;
    const uint64_t position = (address - base) / layout.record_bytes;
    if (position >= record_count)
        return Status::INVALID_ADDRESS;
    uint64_t value = 0;
    if (!checkedAdd(iteration_base, position + 1, value))
        return Status::ARITHMETIC_OVERFLOW;
    index = position;
    sequence = value;
    return Status::OK;
}

inline Status windowStorage(
        const Layout& layout, uint64_t records, uint64_t line_bytes,
        uint64_t& lines, uint64_t& bytes) {
    lines = bytes = 0;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (records == 0 || line_bytes < layout.record_bytes ||
        (line_bytes & (line_bytes - 1)) != 0 ||
        line_bytes % layout.record_bytes != 0) {
        return Status::INVALID_WIDTH;
    }
    uint64_t payload = 0, span = 0;
    if (!checkedMultiply(records, layout.record_bytes, payload) ||
        !checkedAdd(payload, line_bytes - layout.record_bytes, span)) {
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
