#ifndef GRAPHBREW_ECG_RECORD_NATIVE_H
#define GRAPHBREW_ECG_RECORD_NATIVE_H

#include "ecg_record.h"

namespace ecg_record {

static constexpr uint64_t kNativeEnable = 1;
static constexpr uint64_t kNativeHasNext = 2;
static constexpr uint64_t kNativeControlMask = kNativeEnable | kNativeHasNext;

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
    uint16_t context = 0;
    State state = State::UNKNOWN;
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
    requirements.preset = decoded.preset;
    requirements.action_encoding = decoded.action_encoding;
    requirements.carrier_bytes = decoded.carrier_bytes;
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
    if (configuration.property_base % sizeof(uint32_t) != 0)
        return Status::INVALID_ADDRESS;
    uint64_t property_bytes = 0, last_property = 0, iteration_end = 0;
    if (!checkedMultiply(configuration.vertex_count, sizeof(uint32_t), property_bytes) ||
        !checkedAdd(configuration.property_base, property_bytes - 1, last_property) ||
        !checkedAdd(configuration.iteration_base, configuration.record_count,
                    iteration_end)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    if (adaptive(decoded)) {
        uint64_t last_deadline = 0;
        if (!checkedAdd(iteration_end, decoded.max_finite_distance, last_deadline))
            return Status::ARITHMETIC_OVERFLOW;
    }
    layout = decoded;
    return Status::OK;
}

inline Status nativeRecordAccess(
        const NativeConfiguration& configuration, uint64_t address,
        uint8_t instruction_bytes, NativeRecordAccess& output) {
    output = NativeRecordAccess{};
    Layout layout;
    Status status = validateNativeConfiguration(configuration, layout);
    if (status != Status::OK)
        return status;
    if (instruction_bytes != layout.carrier_bytes)
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
        configuration, record_address, layout.carrier_bytes, access);
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
    result.context = static_cast<uint16_t>(configuration.context);
    result.state = decoded.state;
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
    status = makePrediction(
        layout, raw_record, result.sequence,
        (configuration.control & kNativeHasNext) != 0, prediction);
    if (status != Status::OK)
        return status;
    uint64_t offset = 0;
    if (!checkedMultiply(result.destination, sizeof(uint32_t), offset) ||
        !checkedAdd(property_base, offset, result.property_address)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    result.deadline = prediction.deadline;
    result.state = prediction.state;
    output = result;
    return Status::OK;
}

}  // namespace ecg_record

#endif
