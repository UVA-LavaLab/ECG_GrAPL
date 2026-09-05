#ifndef GRAPHBREW_ECG_RECORD_STREAM_H
#define GRAPHBREW_ECG_RECORD_STREAM_H

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
    uint64_t carrier_bytes = 0;
    uint64_t carrier_allocation_bytes = 0;
    uint64_t auxiliary_peak_bytes = 0;
    uint64_t property_lines = 0;
    uint64_t encoded_actions = 0;
};

struct RecordStream {
    Layout layout;
    BuildStats stats;
    std::vector<uint32_t> records32;
    std::vector<uint64_t> records64;

    std::size_t size() const {
        return layout.carrier_bytes == 4 ? records32.size() : records64.size();
    }

    const uint8_t* data() const {
        return layout.carrier_bytes == 4
            ? reinterpret_cast<const uint8_t*>(records32.data())
            : reinterpret_cast<const uint8_t*>(records64.data());
    }

    uint64_t word(std::size_t index) const {
        return layout.carrier_bytes == 4 ? records32.at(index) : records64.at(index);
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
    uint64_t last = kAbsent;
    uint64_t next = kAbsent;
};

inline Status storeWord(RecordStream& stream, std::size_t index, uint64_t word) {
    if (stream.layout.carrier_bytes == 4) {
        if (word > UINT32_MAX)
            return Status::INVALID_RECORD;
        stream.records32.at(index) = static_cast<uint32_t>(word);
    } else {
        stream.records64.at(index) = word;
    }
    return Status::OK;
}

}  // namespace detail

// Construction is outside the ROI. No exact-future array survives in the carrier.
template<typename DestinationAt>
Status buildRecords(
        const Requirements& requirements, const Layout& layout,
        uint64_t vertices_per_line, DestinationAt destination_at,
        RecordStream& output, const BuildLimits& limits = BuildLimits{}) {
    output = RecordStream{};
    const Status configuration = validateConfiguration(requirements, layout);
    if (configuration != Status::OK)
        return configuration;
    if (vertices_per_line == 0 ||
        (limits.source_id_bytes != 4 && limits.source_id_bytes != 8)) {
        return Status::INVALID_WIDTH;
    }
    uint64_t carrier_bytes = 0, source_bytes = 0;
    if (!checkedMultiply(requirements.record_count, layout.carrier_bytes, carrier_bytes) ||
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
    stream.stats.carrier_bytes = carrier_bytes;
    if (layout.carrier_bytes == 4) {
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
    using Scratch = std::vector<uint64_t, detail::BudgetAllocator<uint64_t>>;
    try {
        LineMap lines(0, std::hash<uint64_t>{}, std::equal_to<uint64_t>{},
                      detail::BudgetAllocator<Node>{&budget});
        Scratch backward_gap{detail::BudgetAllocator<uint64_t>{&budget}};
        Scratch exact_future{detail::BudgetAllocator<uint64_t>{&budget}};
        if (layout.action_bits != 0) {
            backward_gap.resize(count);
            exact_future.resize(count);
        }
        const uint64_t maximum_id = requirements.max_vertex_id_known
            ? requirements.max_vertex_id : requirements.vertex_count - 1;
        for (std::size_t position = 0; position < count; ++position) {
            const uint64_t destination = destination_at(position);
            if (destination > maximum_id || destination >= requirements.vertex_count ||
                destination > lowMask(layout.id_bits)) {
                return Status::INVALID_ID;
            }
            Position& positions = lines[destination / vertices_per_line];
            if (positions.first == Position::kAbsent)
                positions.first = position;
            if (layout.action_bits != 0 && positions.last != Position::kAbsent)
                backward_gap[position] = position - positions.last;
            positions.last = position;
        }
        if (layout.action_bits != 0) {
            for (const auto& entry : lines) {
                const Position& positions = entry.second;
                backward_gap[positions.first] =
                    requirements.record_count - positions.last + positions.first;
            }
        }
        for (std::size_t position = count; position-- > 0;) {
            const uint64_t destination = destination_at(position);
            Position& positions = lines.at(destination / vertices_per_line);
            const bool in_iteration = positions.next != Position::kAbsent;
            const uint64_t distance = in_iteration
                ? positions.next - position
                : requirements.record_count - position + positions.first;
            uint64_t word = 0;
            const Status encoded = encodeRecord(
                layout, destination, distance,
                in_iteration ? State::FINITE : State::WRAP, 0, word);
            if (encoded != Status::OK)
                return encoded;
            const Status stored = detail::storeWord(stream, position, word);
            if (stored != Status::OK)
                return stored;
            if (layout.action_bits != 0)
                exact_future[position] = distance;
            positions.next = position;
        }
        if (layout.action_bits != 0) {
            for (std::size_t position = 0; position < count; ++position) {
                const uint64_t current_line =
                    destination_at(position) / vertices_per_line;
                uint32_t best_lead = 0;
                uint64_t best_gap = 0, best_future = 0;
                uint32_t best_error = 0;
                for (uint32_t lead = 8; lead <= 15; ++lead) {
                    if (lead >= count - position)
                        break;
                    if (layout.action_encoding == ActionEncoding::ENUMERATED2 &&
                        lead != 8 && lead != 12 && lead != 15) {
                        continue;
                    }
                    const std::size_t candidate = position + lead;
                    if (destination_at(candidate) / vertices_per_line == current_line ||
                        backward_gap[candidate] <= lead) {
                        continue;
                    }
                    const uint32_t error = lead > 10 ? lead - 10 : 10 - lead;
                    if (best_lead == 0 || backward_gap[candidate] > best_gap ||
                        (backward_gap[candidate] == best_gap &&
                         exact_future[candidate] < best_future) ||
                        (backward_gap[candidate] == best_gap &&
                         exact_future[candidate] == best_future && error < best_error)) {
                        best_lead = lead;
                        best_gap = backward_gap[candidate];
                        best_future = exact_future[candidate];
                        best_error = error;
                    }
                }
                if (best_lead != 0) {
                    uint64_t word = 0;
                    const Status changed = setAction(
                        layout, stream.word(position), best_lead, word);
                    if (changed != Status::OK)
                        return changed;
                    const Status stored = detail::storeWord(stream, position, word);
                    if (stored != Status::OK)
                        return stored;
                    ++stream.stats.encoded_actions;
                }
            }
        }
        stream.stats.property_lines = lines.size();
        stream.stats.auxiliary_peak_bytes = budget.peak;
    } catch (const detail::AuxiliaryLimitExceeded&) {
        return Status::RESOURCE_LIMIT;
    }
    output = std::move(stream);
    return Status::OK;
}

}  // namespace ecg_record

#endif
