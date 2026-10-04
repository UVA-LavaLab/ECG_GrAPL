#ifndef GRAPHBREW_ECG_RECORD_NATIVE_H
#define GRAPHBREW_ECG_RECORD_NATIVE_H

#include "ecg_record.h"

namespace ecg_record {

static constexpr uint64_t kNativeEnable = 1;
static constexpr uint64_t kNativeHasNext = 2;
static constexpr uint64_t kNativeManagedPasses = 4;
// The kernel also writes this property region in place, a reference no record
// describes. A line whose last record falls in the final pass still has that
// write to come, so its WRAP bound decodes as before another pass, never DEAD.
static constexpr uint64_t kNativeWrittenInPlace = 8;
static constexpr uint64_t kNativeControlMask =
    kNativeEnable | kNativeHasNext | kNativeManagedPasses | kNativeWrittenInPlace;

struct NativeConfiguration {
    uint64_t layout_descriptor = 0;
    uint64_t record_base = 0;
    uint64_t property_base = 0;
    uint64_t record_count = 0;
    uint64_t vertex_count = 0;
    uint64_t iteration_base = 0;
    uint64_t generation = 0;
    uint64_t context = 0;
    uint64_t control = 0;
    uint64_t property_descriptor = 0;
};

struct NativeRecordAccess {
    uint64_t address = 0;
    uint64_t index = 0;
    uint64_t sequence = 0;
    uint8_t bytes = 0;
};

struct NativeLoadResult {
    uint64_t raw_record = 0;
    uint64_t record_address = 0;
    uint64_t property_address = 0;
    uint64_t destination = 0;
    uint64_t sequence = 0;
    uint64_t deadline = 0;
    uint64_t generation = 0;
    uint64_t layout_descriptor = 0;
    uint16_t context = 0;
    uint8_t sequence_bits = 0;
    bool has_next_iteration = false;
    State state = State::UNKNOWN;
    uint64_t property_descriptor = 0;
    PropertyKind property_kind = PropertyKind::F32;
    uint8_t property_bytes = 4;
};

enum class InstructionKind : uint8_t {
    NONE,
    CONFIGURATION,
    RECORD,
    PROPERTY,
    PASS_CLOSE,
    INVALIDATE,
};

struct InstructionHint {
    InstructionKind kind = InstructionKind::NONE;
    NativeLoadResult load;
};

inline Status validateNativeConfiguration(
        const NativeConfiguration& configuration, Layout& layout) {
    layout = Layout{};
    if ((configuration.control & kNativeEnable) == 0 ||
        (configuration.control & ~kNativeControlMask) != 0 ||
        configuration.context == 0 || configuration.context > UINT16_MAX) {
        return Status::INVALID_LAYOUT;
    }
    Layout decoded;
    Status status = unpackLayout(configuration.layout_descriptor, decoded);
    if (status != Status::OK)
        return status;
    if (configuration.vertex_count == 0 || configuration.record_count == 0)
        return Status::INVALID_COUNTS;
    Requirements requirements;
    requirements.requested_record_bytes = decoded.record_bytes;
    requirements.vertex_count = configuration.vertex_count;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = std::min(
        configuration.vertex_count - 1, lowMask(decoded.id_bits));
    requirements.record_count = configuration.record_count;
    status = validateConfiguration(requirements, decoded);
    if (status != Status::OK)
        return status;
    uint64_t first_record = 0;
    status = recordAddress(decoded, configuration.record_base, 0,
                           configuration.record_count, first_record);
    if (status != Status::OK)
        return status;
    PropertyDescriptor property;
    status = unpackProperty(configuration.property_descriptor, property);
    if (status != Status::OK)
        return status;
    uint64_t last_property = 0, iteration_end = 0;
    status = propertyAddress(property, configuration.property_base,
                             configuration.vertex_count - 1, last_property);
    if (status != Status::OK)
        return status;
    if (!checkedAdd(configuration.iteration_base, configuration.record_count, iteration_end)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    uint64_t last_deadline = 0;
    if (!checkedAdd(iteration_end, decoded.max_finite_distance, last_deadline))
        return Status::ARITHMETIC_OVERFLOW;
    layout = decoded;
    return Status::OK;
}

inline bool nativePropertyContains(
        const NativeConfiguration& configuration, uint64_t address, uint64_t bytes = 1) {
    PropertyDescriptor property;
    uint64_t extent = 0;
    return bytes != 0 && address >= configuration.property_base &&
        unpackProperty(configuration.property_descriptor, property) == Status::OK &&
        propertyExtent(property, configuration.vertex_count, extent) == Status::OK &&
        address - configuration.property_base < extent &&
        bytes <= extent - (address - configuration.property_base);
}

inline bool nativePropertyLine(
        const NativeConfiguration& configuration, uint64_t address) {
    PropertyDescriptor property;
    uint64_t extent = 0, last = 0;
    if (unpackProperty(configuration.property_descriptor, property) != Status::OK ||
        propertyExtent(property, configuration.vertex_count, extent) != Status::OK ||
        !checkedAdd(configuration.property_base, extent - 1, last))
        return false;
    return address / 64 >= configuration.property_base / 64 && address / 64 <= last / 64;
}

// The lines of the active record stream: from the first record's line to the
// last record's, partial first and last lines included, from the validated
// configuration alone. The codec places the last record; validation already
// ran it over the whole stream. A record is aligned to its width, which divides
// the line, so the last record lies within one line. False, with an empty span,
// when the configuration is invalid.
inline bool nativeCarrierSpan(
        const NativeConfiguration& configuration, uint64_t& first_line, uint64_t& last_line) {
    first_line = last_line = 0;
    Layout layout;
    uint64_t last_record = 0;
    if (validateNativeConfiguration(configuration, layout) != Status::OK ||
        recordAddress(layout, configuration.record_base, configuration.record_count - 1,
                      configuration.record_count, last_record) != Status::OK)
        return false;
    first_line = configuration.record_base / 64;
    last_line = last_record / 64;
    return true;
}

inline bool nativeCarrierLine(const NativeConfiguration& configuration, uint64_t address) {
    uint64_t first_line = 0, last_line = 0;
    return nativeCarrierSpan(configuration, first_line, last_line) &&
           address / 64 >= first_line && address / 64 <= last_line;
}

inline Status nativeRecordAccess(
        const NativeConfiguration& configuration, uint64_t address,
        uint8_t instruction_bytes, NativeRecordAccess& output) {
    output = NativeRecordAccess{};
    Layout layout;
    Status status = validateNativeConfiguration(configuration, layout);
    if (status != Status::OK)
        return status;
    if (instruction_bytes != layout.record_bytes)
        return Status::INVALID_WIDTH;
    NativeRecordAccess access;
    status = recordPosition(layout, address, configuration.record_base,
                            configuration.record_count, configuration.iteration_base,
                            access.index, access.sequence);
    if (status != Status::OK)
        return status;
    access.address = address;
    access.bytes = instruction_bytes;
    output = access;
    return Status::OK;
}

inline Status nativeRecordResult(
        const NativeConfiguration& configuration, uint64_t record_address,
        uint64_t raw_record, NativeLoadResult& output) {
    output = NativeLoadResult{};
    Layout layout;
    Status status = validateNativeConfiguration(configuration, layout);
    if (status != Status::OK)
        return status;
    NativeRecordAccess access;
    status = nativeRecordAccess(
        configuration, record_address, layout.record_bytes, access);
    if (status != Status::OK)
        return status;
    DecodedRecord decoded;
    status = decodeRecord(layout, raw_record, decoded);
    if (status != Status::OK)
        return status;
    if (decoded.destination >= configuration.vertex_count)
        return Status::INVALID_ID;
    NativeLoadResult result;
    result.raw_record = raw_record;
    result.record_address = record_address;
    result.destination = decoded.destination;
    result.sequence = access.sequence;
    result.generation = configuration.generation;
    result.layout_descriptor = configuration.layout_descriptor;
    result.context = static_cast<uint16_t>(configuration.context);
    result.sequence_bits = layout.sequence_bits;
    result.has_next_iteration =
        (configuration.control & kNativeHasNext) != 0;
    result.state = decoded.state;
    PropertyDescriptor property;
    unpackProperty(configuration.property_descriptor, property);
    result.property_descriptor = configuration.property_descriptor;
    result.property_kind = property.kind;
    result.property_bytes = propertyBytes(property.kind);
    output = result;
    return Status::OK;
}

inline Status nativePropertyAccess(
        const NativeConfiguration& configuration, uint64_t property_base,
        uint64_t raw_record, uint64_t record_address, NativeLoadResult& output) {
    output = NativeLoadResult{};
    Layout layout;
    Status status = validateNativeConfiguration(configuration, layout);
    if (status != Status::OK)
        return status;
    if (property_base != configuration.property_base)
        return Status::INVALID_ADDRESS;
    NativeLoadResult result;
    status = nativeRecordResult(configuration, record_address, raw_record, result);
    if (status != Status::OK)
        return status;
    Prediction prediction;
    PropertyDescriptor property;
    unpackProperty(configuration.property_descriptor, property);
    status = makePrediction(
        layout, raw_record, result.sequence,
        (configuration.control & (kNativeHasNext | kNativeWrittenInPlace)) != 0,
        prediction, property.traversal);
    if (status != Status::OK)
        return status;
    status = propertyAddress(property, property_base, result.destination, result.property_address);
    if (status != Status::OK)
        return status;
    result.deadline = prediction.deadline;
    result.state = prediction.state;
    if (property.traversal == TraversalMode::ORDERED_FILTERED && result.state == State::FINITE) {
        uint64_t end = 0;
        if (!checkedAdd(configuration.iteration_base, configuration.record_count, end))
            return Status::ARITHMETIC_OVERFLOW;
        if (result.sequence == end)
            return Status::INVALID_RECORD;
        result.deadline = std::min(result.deadline, end);
    }
    output = result;
    return Status::OK;
}

}  // namespace ecg_record

#endif
