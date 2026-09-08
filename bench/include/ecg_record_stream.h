#ifndef GRAPHBREW_ECG_RECORD_STREAM_H
#define GRAPHBREW_ECG_RECORD_STREAM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ecg_record.h"

namespace ecg_record {

struct BuildLimits {
    uint64_t maximum_carrier_bytes = uint64_t{256} << 20;
    uint64_t maximum_auxiliary_bytes = uint64_t{256} << 20;
    uint8_t source_id_bytes = 4;
};

struct BuildStats {
    uint64_t source_stream_bytes = 0;
    uint64_t carrier_payload_bytes = 0;
    uint64_t carrier_allocation_bytes = 0;
    uint64_t auxiliary_peak_bytes = 0;
    uint64_t property_lines = 0;
    uint64_t finite_records = 0, wrap_records = 0, unknown_records = 0;
};

struct RecordStream {
    Layout layout;
    BuildStats stats;
    std::vector<uint32_t> records32;
    std::vector<uint64_t> records64;

    std::size_t size() const {
        return layout.record_bytes == 4 ? records32.size() : records64.size();
    }

    const uint8_t* data() const {
        return layout.record_bytes == 4
            ? reinterpret_cast<const uint8_t*>(records32.data())
            : reinterpret_cast<const uint8_t*>(records64.data());
    }

    uint64_t word(std::size_t index) const {
        return layout.record_bytes == 4 ? records32.at(index) : records64.at(index);
    }
};

namespace detail {

struct AuxiliaryLimitExceeded {};

struct AllocationBudget {
    uint64_t limit;
    uint64_t used = 0;
    uint64_t peak = 0;
};

template<typename T>
class BudgetAllocator {
  public:
    using value_type = T;
    AllocationBudget* budget;

    explicit BudgetAllocator(AllocationBudget* value) : budget(value) {}
    template<typename U>
    BudgetAllocator(const BudgetAllocator<U>& other) : budget(other.budget) {}

    T* allocate(std::size_t count) {
        uint64_t bytes = 0;
        if (!checkedMultiply(count, sizeof(T), bytes) ||
            bytes > budget->limit - budget->used) {
            throw AuxiliaryLimitExceeded{};
        }
        T* result = std::allocator<T>{}.allocate(count);
        budget->used += bytes;
        budget->peak = std::max(budget->peak, budget->used);
        return result;
    }

    void deallocate(T* pointer, std::size_t count) {
        const uint64_t bytes = uint64_t(count) * sizeof(T);
        assert(bytes <= budget->used);
        budget->used -= bytes;
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template<typename U>
    bool operator==(const BudgetAllocator<U>& other) const {
        return budget == other.budget;
    }
    template<typename U>
    bool operator!=(const BudgetAllocator<U>& other) const {
        return budget != other.budget;
    }
};

struct LinePositions {
    static constexpr uint64_t kAbsent = std::numeric_limits<uint64_t>::max();
    uint64_t first = kAbsent;
    uint64_t next = kAbsent;
};

inline Status storeWord(RecordStream& stream, std::size_t index, uint64_t word) {
    if (stream.layout.record_bytes == 4) {
        if (word > UINT32_MAX)
            return Status::INVALID_RECORD;
        stream.records32.at(index) = static_cast<uint32_t>(word);
    } else {
        stream.records64.at(index) = word;
    }
    return Status::OK;
}

}  // namespace detail

// Source IDs must remain immutable. Only sparse line positions are temporary.
template<typename DestinationAt, typename LineAt>
Status buildRecordsMapped(
        const Requirements& requirements, const Layout& layout,
        DestinationAt destination_at, LineAt line_at,
        RecordStream& output, const BuildLimits& limits = BuildLimits{}) {
    output = RecordStream{};
    const Status configuration = validateConfiguration(requirements, layout);
    if (configuration != Status::OK)
        return configuration;
    if (limits.source_id_bytes != 4 && limits.source_id_bytes != 8) {
        return Status::INVALID_WIDTH;
    }
    uint64_t carrier_bytes = 0, source_bytes = 0;
    if (!checkedMultiply(requirements.record_count, layout.record_bytes, carrier_bytes) ||
        !checkedMultiply(requirements.record_count, limits.source_id_bytes, source_bytes)) {
        return Status::ARITHMETIC_OVERFLOW;
    }
    if (carrier_bytes > limits.maximum_carrier_bytes ||
        requirements.record_count > std::numeric_limits<std::size_t>::max()) {
        return Status::RESOURCE_LIMIT;
    }
    const std::size_t count = static_cast<std::size_t>(requirements.record_count);
    RecordStream stream;
    stream.layout = layout;
    stream.stats.source_stream_bytes = source_bytes;
    stream.stats.carrier_payload_bytes = carrier_bytes;
    if (layout.record_bytes == 4) {
        if (count > stream.records32.max_size())
            return Status::RESOURCE_LIMIT;
        stream.records32.resize(count);
        stream.stats.carrier_allocation_bytes =
            uint64_t(stream.records32.capacity()) * sizeof(uint32_t);
    } else {
        if (count > stream.records64.max_size())
            return Status::RESOURCE_LIMIT;
        stream.records64.resize(count);
        stream.stats.carrier_allocation_bytes =
            uint64_t(stream.records64.capacity()) * sizeof(uint64_t);
    }
    if (stream.stats.carrier_allocation_bytes > limits.maximum_carrier_bytes)
        return Status::RESOURCE_LIMIT;

    detail::AllocationBudget budget{limits.maximum_auxiliary_bytes};
    using Position = detail::LinePositions;
    using Node = std::pair<const uint64_t, Position>;
    using LineMap = std::unordered_map<uint64_t, Position,
        std::hash<uint64_t>, std::equal_to<uint64_t>, detail::BudgetAllocator<Node>>;
    try {
        LineMap lines(0, std::hash<uint64_t>{}, std::equal_to<uint64_t>{},
                      detail::BudgetAllocator<Node>{&budget});
        const uint64_t maximum_id = requirements.max_vertex_id_known
            ? requirements.max_vertex_id : requirements.vertex_count - 1;
        for (std::size_t position = 0; position < count; ++position) {
            const uint64_t destination = destination_at(position);
            if (destination > maximum_id || destination >= requirements.vertex_count ||
                destination > lowMask(layout.id_bits) ||
                destination > lowMask(unsigned(limits.source_id_bytes) * 8)) {
                return Status::INVALID_ID;
            }
            Position& positions = lines[line_at(destination)];
            if (positions.first == Position::kAbsent)
                positions.first = position;
        }
        for (std::size_t position = count; position-- > 0;) {
            const uint64_t destination = destination_at(position);
            Position& positions = lines.at(line_at(destination));
            const bool in_iteration = positions.next != Position::kAbsent;
            const uint64_t distance = in_iteration
                ? positions.next - position
                : requirements.record_count - position + positions.first;
            uint64_t word = 0;
            const Status encoded = encodeRecord(
                layout, destination, distance,
                in_iteration ? State::FINITE : State::WRAP, word);
            if (encoded != Status::OK)
                return encoded;
            const Status stored = detail::storeWord(stream, position, word);
            if (stored != Status::OK)
                return stored;
            positions.next = position;
        }
        stream.stats.property_lines = lines.size();
        stream.stats.auxiliary_peak_bytes = budget.peak;
    } catch (const detail::AuxiliaryLimitExceeded&) {
        return Status::RESOURCE_LIMIT;
    }
    output = std::move(stream);
    return Status::OK;
}

template<typename DestinationAt>
Status buildRecords(
        const Requirements& requirements, const Layout& layout,
        uint64_t vertices_per_line, DestinationAt destination_at,
        RecordStream& output, const BuildLimits& limits = BuildLimits{}) {
    if (vertices_per_line == 0) {
        output = RecordStream{};
        return Status::INVALID_WIDTH;
    }
    return buildRecordsMapped(requirements, layout, destination_at,
        [vertices_per_line](uint64_t id) { return id / vertices_per_line; }, output, limits);
}

struct UnobservedConstruction {
    void operator()(const void*, uint64_t, bool) const {}
};

struct BuildScope {
    uint8_t partition = 0;
    uint64_t end = UINT64_MAX;
};

struct FullBuildScope {
    BuildScope operator()(uint64_t) const { return {}; }
};

template<std::size_t Partitions = 1, typename DestinationAt,
         typename Observe = UnobservedConstruction, typename ScopeAt = FullBuildScope>
Status buildRecords(
        const Requirements& requirements, const Layout& layout,
        const PropertyDescriptor& property, uint64_t property_base,
        DestinationAt destination_at, RecordStream& output,
        const BuildLimits& limits = BuildLimits{}, Observe observe = Observe{},
        ScopeAt scope_at = ScopeAt{}) {
    static_assert(Partitions > 0 && Partitions <= 64, "bounded static reference partitions");
    output = RecordStream{};
    const Status configuration = validateConfiguration(requirements, layout);
    if (configuration != Status::OK)
        return configuration;
    if (limits.source_id_bytes != 4 && limits.source_id_bytes != 8)
        return Status::INVALID_WIDTH;
    uint64_t last = 0;
    const Status status = propertyAddress(
        property, property_base, requirements.vertex_count - 1, last);
    if (status != Status::OK)
        return status;
    struct Slot {
        uint64_t key = UINT64_MAX;
        std::array<uint64_t, Partitions> first, next;
        Slot() {
            first.fill(UINT64_MAX);
            next.fill(UINT64_MAX);
        }
    };
    const uint64_t maximum_lines = std::min(requirements.record_count,
        last / 64 - property_base / 64 + 1);
    uint64_t capacity = 1, slots_needed = 0, scratch_bytes = 0;
    uint64_t carrier_bytes = 0, source_bytes = 0;
    if (!checkedMultiply(maximum_lines, 2, slots_needed) ||
        !checkedMultiply(requirements.record_count, layout.record_bytes, carrier_bytes) ||
        !checkedMultiply(requirements.record_count, limits.source_id_bytes, source_bytes))
        return Status::ARITHMETIC_OVERFLOW;
    while (capacity < slots_needed) {
        if (capacity > UINT64_MAX / 2)
            return Status::ARITHMETIC_OVERFLOW;
        capacity *= 2;
    }
    if (!checkedMultiply(capacity, sizeof(Slot), scratch_bytes) ||
        scratch_bytes > limits.maximum_auxiliary_bytes ||
        carrier_bytes > limits.maximum_carrier_bytes ||
        capacity > std::vector<Slot>{}.max_size() ||
        requirements.record_count > std::numeric_limits<std::size_t>::max())
        return Status::RESOURCE_LIMIT;
    std::vector<Slot> slots(static_cast<std::size_t>(capacity));
    observe(slots.data(), scratch_bytes, true);
    RecordStream stream;
    stream.layout = layout;
    if (layout.record_bytes == 4)
        stream.records32.resize(static_cast<std::size_t>(requirements.record_count));
    else
        stream.records64.resize(static_cast<std::size_t>(requirements.record_count));
    stream.stats.source_stream_bytes = source_bytes;
    stream.stats.carrier_payload_bytes = carrier_bytes;
    stream.stats.carrier_allocation_bytes = layout.record_bytes == 4
        ? stream.records32.capacity() * uint64_t{4} : stream.records64.capacity() * uint64_t{8};
    stream.stats.auxiliary_peak_bytes = slots.capacity() * sizeof(Slot);
    if (stream.stats.carrier_allocation_bytes > limits.maximum_carrier_bytes ||
        stream.stats.auxiliary_peak_bytes > limits.maximum_auxiliary_bytes)
        return Status::RESOURCE_LIMIT;
    observe(stream.data(), carrier_bytes, true);
    const auto find = [&](uint64_t id) -> Slot& {
        const uint64_t line = (property_base + id * property.stride_bytes) / 64;
        uint64_t index = (line * 11400714819323198485ULL) & (capacity - 1);
        for (;;) {
            Slot& slot = slots[index];
            observe(&slot.key, sizeof(slot.key), false);
            if (slot.key == UINT64_MAX || slot.key == line)
                return slot;
            index = (index + 1) & (capacity - 1);
        }
    };
    const uint64_t maximum_id = requirements.max_vertex_id_known
        ? requirements.max_vertex_id : requirements.vertex_count - 1;
    const auto valid_scope = [&](const BuildScope& scope, uint64_t position) {
        return scope.partition < Partitions && (scope.end == UINT64_MAX ||
            (scope.end > position && scope.end <= requirements.record_count));
    };
    for (uint64_t position = 0; position < requirements.record_count; ++position) {
        const uint64_t id = destination_at(position);
        if (id > maximum_id || id >= requirements.vertex_count || id > lowMask(layout.id_bits) ||
            id > lowMask(unsigned(limits.source_id_bytes) * 8))
            return Status::INVALID_ID;
        const BuildScope scope = scope_at(position);
        if (!valid_scope(scope, position))
            return Status::INVALID_COUNTS;
        Slot& slot = find(id);
        const bool new_line = slot.key == UINT64_MAX;
        if (new_line) {
            observe(&slot.key, sizeof(slot.key), true);
            slot.key = (property_base + id * property.stride_bytes) / 64;
            ++stream.stats.property_lines;
        }
        uint64_t& first = slot.first[scope.partition];
        bool new_partition = new_line;
        if constexpr (Partitions != 1) {
            observe(&first, sizeof(first), false);
            new_partition = first == UINT64_MAX;
        }
        if (new_partition) {
            observe(&first, sizeof(first), true);
            first = position;
        }
    }
    for (uint64_t position = requirements.record_count; position-- > 0;) {
        const uint64_t id = destination_at(position);
        if (id > maximum_id || id >= requirements.vertex_count || id > lowMask(layout.id_bits))
            return Status::INVALID_ID;
        const BuildScope scope = scope_at(position);
        if (!valid_scope(scope, position))
            return Status::INVALID_COUNTS;
        Slot& slot = find(id);
        if (slot.key == UINT64_MAX)
            return Status::INVALID_RECORD;
        uint64_t& next = slot.next[scope.partition];
        const uint64_t& first = slot.first[scope.partition];
        if constexpr (Partitions != 1) {
            observe(&first, sizeof(first), false);
            if (first == UINT64_MAX)
                return Status::INVALID_RECORD;
        }
        observe(&next, sizeof(next), false);
        const bool in_scope = next != UINT64_MAX && next < scope.end;
        State state = in_scope ? State::FINITE : State::UNKNOWN;
        uint64_t distance = in_scope ? next - position : 0;
        if (!in_scope && scope.end == UINT64_MAX) {
            if constexpr (Partitions == 1)
                observe(&first, sizeof(first), false);
            if (first == UINT64_MAX)
                return Status::INVALID_RECORD;
            state = State::WRAP;
            distance = requirements.record_count - position + first;
        }
        uint64_t word = 0;
        const Status encoded = encodeRecord(layout, id, distance, state, word);
        if (encoded != Status::OK)
            return encoded;
        observe(stream.data() + position * layout.record_bytes, layout.record_bytes, true);
        const Status stored = detail::storeWord(stream, position, word);
        if (stored != Status::OK)
            return stored;
        observe(&next, sizeof(next), true);
        next = position;
        if (state == State::FINITE)
            ++stream.stats.finite_records;
        else if (state == State::WRAP)
            ++stream.stats.wrap_records;
        else
            ++stream.stats.unknown_records;
    }
    output = std::move(stream);
    return Status::OK;
}

}  // namespace ecg_record

#endif
