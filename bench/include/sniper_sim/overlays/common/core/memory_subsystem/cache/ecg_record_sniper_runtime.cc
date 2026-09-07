#include "ecg_record_sniper_runtime.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace graphbrew {
namespace sniper {
namespace record {

ecg_record::Status
Runtime::fail(ecg_record::Status status)
{
    ++counters_.errors;
    return status;
}

ecg_record::Status
Runtime::configure(
    const ecg_record::NativeConfiguration& configuration,
    ecg_record::Mechanism mechanism,
    uint64_t update_latency, std::size_t capture_width,
    std::size_t prefetch_capacity, uint64_t prefetch_latency,
    uint64_t line_bytes)
{
    if (active_ || (!invalidated_ && update_queue_) ||
        pending_property_.valid || pending_ordinary_.valid ||
        armed_read_bytes_ != 0 || !prefetch_queue_.empty()) {
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    ecg_record::Layout layout;
    if (invalidated_) {
        uint64_t next_generation = 0;
        if (!ecg_record::checkedAdd(
                configuration_.generation, 1, next_generation) ||
            configuration.generation != next_generation)
            return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    const ecg_record::Status status =
        ecg_record::validateNativeConfiguration(configuration, layout);
    if (status != ecg_record::Status::OK ||
        mechanism == ecg_record::Mechanism::INVALID ||
        update_latency < ecg_record::CommitQueue::kMinimumLatency ||
        capture_width == 0 ||
        capture_width > ecg_record::CommitQueue::kMaximumCaptureWidth ||
        prefetch_capacity == 0 || prefetch_capacity > 256 ||
        prefetch_latency == 0 || line_bytes != 64 ||
        ecg_record::unpackProperty(
            configuration.property_descriptor,
            property_descriptor_) != ecg_record::Status::OK) {
        return fail(status == ecg_record::Status::OK
            ? ecg_record::Status::INVALID_LAYOUT : status);
    }
    configuration_ = configuration;
    layout_ = layout;
    mechanism_ = mechanism;
    update_queue_ = std::make_unique<ecg_record::CommitQueue>(
        update_latency, capture_width);
    if (!receiver_.configure(
            layout_, configuration.record_count,
            static_cast<uint16_t>(configuration.context),
            configuration.generation,
            property_descriptor_.traversal)) {
        return fail(ecg_record::Status::INVALID_LAYOUT);
    }
    prefetch_capacity_ = prefetch_capacity;
    prefetch_latency_ = prefetch_latency;
    line_bytes_ = line_bytes;
    managed_ = (configuration.control &
        ecg_record::kNativeManagedPasses) != 0;
    if (managed_) {
        pass_cursor_ = ecg_record::PassCursor{};
        if (pass_cursor_.configure(
                configuration.record_count,
                property_descriptor_.traversal) !=
                ecg_record::Status::OK)
            return fail(ecg_record::Status::INVALID_LAYOUT);
    }
    if (invalidated_)
        ++counters_.rebinds;
    invalidated_ = false;
    iteration_configured_ = false;
    iteration_consumed_count_ = 0;
    have_last_consumed_ = false;
    last_consumed_sequence_ = 0;
    completed_sequence_ = 0;
    event_order_ = 0;
    bank_ = {};
    next_bank_slot_ = 0;
    active_ = true;
    return ecg_record::Status::OK;
}

bool
Runtime::replacementActive() const
{
    return mechanism_ == ecg_record::Mechanism::REPLACEMENT ||
        mechanism_ == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
}

bool
Runtime::prefetchActive() const
{
    return mechanism_ == ecg_record::Mechanism::PREFETCH ||
        mechanism_ == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
}

ecg_record::Status
Runtime::updateIteration(
    uint64_t iteration_base, bool has_next, bool managed)
{
    if (!active_ || pending_property_.valid || armed_read_bytes_ != 0 ||
        pending_ordinary_.valid || read_observed_ || loaded_chunks_ != 0)
        return fail(ecg_record::Status::INVALID_LAYOUT);
    if (managed) {
        if (!managed_ || pass_cursor_.open() ||
            iteration_base != pass_cursor_.base() ||
            pass_cursor_.begin() != ecg_record::Status::OK)
            return fail(ecg_record::Status::INVALID_SEQUENCE);
        iteration_consumed_count_ = 0;
        iteration_configured_ = true;
        configuration_.iteration_base = iteration_base;
        configuration_.control = ecg_record::kNativeEnable |
            ecg_record::kNativeManagedPasses |
            (has_next ? ecg_record::kNativeHasNext : 0);
        have_last_consumed_ = iteration_base != 0;
        last_consumed_sequence_ = iteration_base;
        return ecg_record::Status::OK;
    }
    if (managed_)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    uint64_t end = 0;
    if (!ecg_record::checkedAdd(
            iteration_base, configuration_.record_count, end)) {
        return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
    }
    if (iteration_configured_) {
        if (iteration_consumed_count_ != configuration_.record_count ||
            iteration_base != expected_iteration_base_) {
            return fail(ecg_record::Status::INVALID_SEQUENCE);
        }
    } else if (iteration_base != configuration_.iteration_base) {
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    expected_iteration_base_ = end;
    iteration_consumed_count_ = 0;
    iteration_configured_ = true;
    configuration_.iteration_base = iteration_base;
    configuration_.control = ecg_record::kNativeEnable |
        (has_next ? ecg_record::kNativeHasNext : 0);
    have_last_consumed_ = iteration_base != 0;
    last_consumed_sequence_ = iteration_base;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::closePass()
{
    if (!active_ || !managed_ || pending_property_.valid ||
        pending_ordinary_.valid || armed_read_bytes_ != 0 ||
        !iteration_configured_ ||
        pass_cursor_.close() != ecg_record::Status::OK)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    configuration_.iteration_base = pass_cursor_.base();
    iteration_configured_ = false;
    if (!ecg_record::checkedAdd(
            counters_.passes, 1, counters_.passes) ||
        !ecg_record::checkedAdd(
            counters_.structural_positions,
            configuration_.record_count,
            counters_.structural_positions))
        return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
    counters_.skipped_positions =
        counters_.structural_positions - counters_.consumed_records;
    if (property_descriptor_.traversal ==
            ecg_record::TraversalMode::ORDERED_FILTERED) {
        const auto result = receiver_.advanceProgress(
            static_cast<uint16_t>(configuration_.context),
            configuration_.generation, pass_cursor_.base());
        if (result != ecg_record::ObservationResult::ACCEPTED &&
            result != ecg_record::ObservationResult::IGNORED_OLD)
            return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::invalidateBinding(uint64_t sets, uint64_t cycles)
{
    if (!active_ || !managed_ || pass_cursor_.open() ||
        !clean() || sets == 0 || cycles != sets)
        return fail(ecg_record::Status::NOT_READY);
    if (!ecg_record::checkedAdd(
            counters_.invalidation_sets, sets,
            counters_.invalidation_sets) ||
        !ecg_record::checkedAdd(
            counters_.invalidation_cycles, cycles,
            counters_.invalidation_cycles))
        return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
    receiver_.disable();
    active_ = false;
    invalidated_ = true;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::deactivate()
{
    if (!clean() || (managed_
            ? pass_cursor_.open() || iteration_configured_
            : !iteration_configured_ ||
                iteration_consumed_count_ != configuration_.record_count)) {
        return fail(ecg_record::Status::NOT_READY);
    }
    active_ = false;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::armRecordRead(uint64_t address, uint32_t bytes)
{
    if (!active_ || (managed_ && !pass_cursor_.open()) ||
        bytes != layout_.record_bytes ||
        armed_read_bytes_ != 0) {
        return fail(ecg_record::Status::INVALID_WIDTH);
    }
    ecg_record::NativeRecordAccess access;
    const ecg_record::Status status = ecg_record::nativeRecordAccess(
        configuration_, address, static_cast<uint8_t>(bytes), access);
    if (status != ecg_record::Status::OK)
        return fail(status);
    armed_read_address_ = address;
    armed_read_bytes_ = bytes;
    read_observed_ = false;
    observed_read_cycle_ = 0;
    loaded_address_ = 0;
    loaded_word_ = 0;
    loaded_chunks_ = 0;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::observeRecordRead(
    uint64_t address, uint32_t bytes, uint64_t cycle)
{
    if (!active_ || armed_read_bytes_ == 0)
        return ecg_record::Status::NOT_READY;
    if (address != armed_read_address_ || bytes != armed_read_bytes_)
        return ecg_record::Status::NOT_READY;
    if (read_observed_)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    read_observed_ = true;
    observed_read_cycle_ = cycle;
    ++counters_.record_reads;
    counters_.record_read_bytes += bytes;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::setLoadedAddress(uint64_t address)
{
    if (!active_ || !read_observed_ || address != armed_read_address_)
        return fail(ecg_record::Status::INVALID_ADDRESS);
    loaded_address_ = address;
    loaded_chunks_ = 0;
    loaded_word_ = 0;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::setLoadedChunk(uint32_t value, bool high)
{
    if (!active_ || loaded_address_ != armed_read_address_ ||
        !read_observed_) {
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    if (high) {
        if (layout_.record_bytes != 8 || (loaded_chunks_ & 2) != 0)
            return fail(ecg_record::Status::INVALID_WIDTH);
        loaded_word_ |= uint64_t{value} << 32;
        loaded_chunks_ |= 2;
    } else {
        if ((loaded_chunks_ & 1) != 0)
            return fail(ecg_record::Status::INVALID_SEQUENCE);
        loaded_word_ |= value;
        loaded_chunks_ |= 1;
    }
    return ecg_record::Status::OK;
}

Runtime::BankEntry*
Runtime::findBank(uint64_t address)
{
    for (BankEntry& entry : bank_) {
        if (entry.valid && entry.address == address)
            return &entry;
    }
    return nullptr;
}

const Runtime::BankEntry*
Runtime::findBank(uint64_t address) const
{
    for (const BankEntry& entry : bank_) {
        if (entry.valid && entry.address == address)
            return &entry;
    }
    return nullptr;
}

Runtime::BankEntry&
Runtime::allocateBank(uint64_t index)
{
    BankEntry& entry = bank_[next_bank_slot_++ % bank_.size()];
    entry = BankEntry{};
    entry.index = index;
    entry.valid = true;
    return entry;
}

ecg_record::Status
Runtime::commitLoadedValue(uint32_t bytes)
{
    const uint8_t required = bytes == 4 ? uint8_t{1} : uint8_t{3};
    if (!active_ || bytes != layout_.record_bytes ||
        armed_read_bytes_ != bytes || loaded_address_ != armed_read_address_ ||
        !read_observed_ || (loaded_chunks_ & required) != required) {
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    if (bytes == 4 && (loaded_word_ >> 32) != 0)
        return fail(ecg_record::Status::INVALID_RECORD);
    ecg_record::NativeRecordAccess access;
    ecg_record::Status status = ecg_record::nativeRecordAccess(
        configuration_, loaded_address_, static_cast<uint8_t>(bytes), access);
    if (status != ecg_record::Status::OK)
        return fail(status);
    ecg_record::NativeLoadResult result;
    status = ecg_record::nativeRecordResult(
        configuration_, loaded_address_, loaded_word_, result);
    if (status != ecg_record::Status::OK)
        return fail(status);
    BankEntry* entry = findBank(loaded_address_);
    if (!entry)
        entry = &allocateBank(access.index);
    entry->address = loaded_address_;
    entry->index = access.index;
    entry->word = loaded_word_;
    entry->completion_cycle = observed_read_cycle_;
    ++counters_.loaded_values;
    armed_read_bytes_ = 0;
    read_observed_ = false;
    loaded_chunks_ = 0;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::buildWindow(
    uint64_t current_index, uint64_t cycle,
    ecg_record::RecordWindow& window) const
{
    window = ecg_record::RecordWindow{};
    window.remaining_records = configuration_.record_count - current_index;
    window.vertex_count = configuration_.vertex_count;
    const uint64_t count = std::min<uint64_t>(
        ecg_record::RecordWindow::kRecords, window.remaining_records);
    for (uint64_t lead = 0; lead < count; ++lead) {
        uint64_t address = 0;
        const ecg_record::Status status = ecg_record::recordAddress(
            layout_, configuration_.record_base, current_index + lead,
            configuration_.record_count, address);
        if (status != ecg_record::Status::OK)
            return status;
        const BankEntry* entry = findBank(address);
        if (entry && entry->completion_cycle <= cycle) {
            window.words[lead] = entry->word;
            window.valid_mask |= uint16_t{1} << lead;
        }
    }
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::selectPrefetch(
    PendingProperty& property, uint64_t cycle)
{
    if (!prefetchActive())
        return ecg_record::Status::OK;
    const BankEntry* current_entry = findBank(property.load.record_address);
    if (!current_entry)
        return ecg_record::Status::NOT_READY;
    ecg_record::RecordWindow window;
    const ecg_record::Status window_status =
        buildWindow(current_entry->index, cycle, window);
    if (window_status != ecg_record::Status::OK)
        return window_status;
    ecg_record::PrefetchTarget target;
    const ecg_record::Status selected = ecg_record::selectWindowTarget(
        layout_, window, property_descriptor_,
        configuration_.property_base, target);
    if (selected != ecg_record::Status::OK)
        return selected;
    if (!target.valid)
        return ecg_record::Status::OK;
    ++counters_.prefetch_candidates;
    uint64_t address = 0;
    const ecg_record::Status addressed = ecg_record::propertyAddress(
        property_descriptor_, configuration_.property_base,
        target.destination, address);
    if (addressed != ecg_record::Status::OK)
        return addressed;
    property.prefetch_vaddr = address;
    property.prefetch_valid = true;
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::enqueuePrefetch(
    const PendingProperty& property, uint64_t cycle)
{
    if (!property.prefetch_valid)
        return ecg_record::Status::OK;
    const uint64_t line =
        property.prefetch_vaddr -
        property.prefetch_vaddr % line_bytes_;
    for (const PendingPrefetch& pending : prefetch_queue_) {
        if ((pending.property_vaddr & ~uint64_t{63}) == line) {
            ++counters_.prefetch_pending_duplicates;
            return ecg_record::Status::OK;
        }
    }
    if (prefetch_queue_.size() >= prefetch_capacity_) {
        ++counters_.prefetch_queue_full;
        return fail(ecg_record::Status::RESOURCE_LIMIT);
    }
    uint64_t ready = 0;
    if (!ecg_record::checkedAdd(
            cycle, prefetch_latency_, ready)) {
        return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
    }
    prefetch_queue_.push_back({
        property.prefetch_vaddr, property.load.sequence, ready});
    ++counters_.prefetch_enqueued;
    counters_.max_prefetch_occupancy = std::max<uint64_t>(
        counters_.max_prefetch_occupancy, prefetch_queue_.size());
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::consumeRecord(uint64_t address, uint64_t cycle)
{
    if (!active_ || !iteration_configured_ || pending_property_.valid ||
        pending_ordinary_.valid)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    const BankEntry* entry = findBank(address);
    if (!entry || entry->completion_cycle > cycle)
        return fail(ecg_record::Status::NOT_READY);
    ecg_record::NativeLoadResult load;
    ecg_record::Status status = ecg_record::nativePropertyAccess(
        configuration_, configuration_.property_base,
        entry->word, address, load);
    if (status != ecg_record::Status::OK)
        return fail(status);
    if (managed_) {
        ecg_record::NativeRecordAccess access;
        const uint64_t prior_sequence = pass_cursor_.sequence();
        status = ecg_record::nativeRecordAccess(
            configuration_, address, layout_.record_bytes, access);
        if (status != ecg_record::Status::OK ||
            pass_cursor_.consume(access.index) != ecg_record::Status::OK ||
            load.sequence != pass_cursor_.sequence())
            return fail(ecg_record::Status::INVALID_SEQUENCE);
        pending_property_.prior_sequence = prior_sequence;
    } else {
        uint64_t expected = 0;
        if (!ecg_record::checkedAdd(
                have_last_consumed_ ? last_consumed_sequence_
                                    : configuration_.iteration_base,
                1, expected))
            return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
        if (load.sequence != expected)
            return fail(ecg_record::Status::INVALID_SEQUENCE);
    }
    last_consumed_sequence_ = load.sequence;
    have_last_consumed_ = true;
    pending_property_.load = load;
    pending_property_.valid = true;
    ++iteration_consumed_count_;
    if (iteration_consumed_count_ > configuration_.record_count)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    ++counters_.consumed_records;
    const ecg_record::Status selected =
        selectPrefetch(pending_property_, cycle);
    if (selected != ecg_record::Status::OK)
        return fail(selected);
    return ecg_record::Status::OK;
}

bool
Runtime::beginPropertyAccess(
    uint64_t virtual_address, uint64_t physical_line,
    uint32_t bytes, uint64_t cycle)
{
    if (!active_ || !pending_property_.valid ||
        pending_property_.memory_active ||
        bytes != pending_property_.load.property_bytes ||
        virtual_address != pending_property_.load.property_address) {
        return false;
    }
    pending_property_.physical_line = physical_line;
    pending_property_.begin_cycle = cycle;
    pending_property_.memory_active = true;
    ++counters_.property_accesses;
    return true;
}

MemoryAccessKind
Runtime::beginMemoryAccess(
    uint64_t virtual_address, uint64_t physical_line,
    uint32_t bytes, bool read, uint64_t cycle)
{
    if (read && beginPropertyAccess(
            virtual_address, physical_line, bytes, cycle))
        return MemoryAccessKind::DESIGNATED;
    if (!active_ || !managed_ ||
        property_descriptor_.traversal !=
            ecg_record::TraversalMode::ORDERED_FILTERED ||
        !pass_cursor_.open() || pending_ordinary_.valid ||
        !ecg_record::nativePropertyContains(
            configuration_, virtual_address, bytes))
        return MemoryAccessKind::NONE;
    pending_ordinary_.physical_line = physical_line;
    pending_ordinary_.property_vaddr = virtual_address;
    pending_ordinary_.sequence = pending_property_.valid
        ? pending_property_.prior_sequence
        : pass_cursor_.sequence();
    pending_ordinary_.begin_cycle = cycle;
    pending_ordinary_.valid = true;
    ++counters_.ordinary_invalidations;
    return MemoryAccessKind::ORDINARY;
}

ecg_record::Status
Runtime::completePropertyAccess(uint64_t cycle)
{
    if (!active_ || !pending_property_.valid ||
        !pending_property_.memory_active) {
        return ecg_record::Status::NOT_READY;
    }
    if (cycle < pending_property_.begin_cycle)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    completed_sequence_ = pending_property_.load.sequence;
    if (!replacementActive()) {
        const ecg_record::Status prefetch =
            enqueuePrefetch(pending_property_, cycle);
        pending_property_ = PendingProperty{};
        return prefetch;
    }
    ecg_record::CommitUpdate update;
    update.physical_line = pending_property_.physical_line;
    update.property_vaddr = pending_property_.load.property_address;
    update.sequence = pending_property_.load.sequence;
    update.deadline = pending_property_.load.deadline;
    update.generation = pending_property_.load.generation;
    update.context = pending_property_.load.context;
    update.sequence_bits = pending_property_.load.sequence_bits;
    update.state = pending_property_.load.state;
    if (managed_ && property_descriptor_.traversal ==
            ecg_record::TraversalMode::ORDERED_FILTERED) {
        if (!ecg_record::checkedAdd(
                event_order_, 1, event_order_)) {
            pending_property_ = PendingProperty{};
            return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
        }
        update.order = event_order_;
    }
    const ecg_record::Status queued = enqueueUpdate(update, cycle);
    if (queued != ecg_record::Status::OK) {
        pending_property_ = PendingProperty{};
        return queued;
    }
    const ecg_record::Status prefetch =
        enqueuePrefetch(pending_property_, cycle);
    pending_property_ = PendingProperty{};
    return prefetch;
}

ecg_record::Status
Runtime::enqueueUpdate(
    const ecg_record::CommitUpdate& update, uint64_t cycle)
{
    ++counters_.generated_updates;
    const ecg_record::EnqueueStatus result =
        update_queue_->enqueue(update, cycle);
    if (result == ecg_record::EnqueueStatus::ENQUEUED) {
        ++counters_.enqueued_updates;
    } else if (result == ecg_record::EnqueueStatus::COALESCED) {
        ++counters_.coalesced_updates;
    } else {
        std::fprintf(
            stderr,
            "[SNIPER-ECG-RECORD-QUEUE-ERROR status=%u cycle=%llu "
            "sequence=%llu pending=%llu]\n",
            static_cast<unsigned>(result),
            static_cast<unsigned long long>(cycle),
            static_cast<unsigned long long>(update.sequence),
            static_cast<unsigned long long>(
                update_queue_->pendingSize()));
        return fail(ecg_record::Status::RESOURCE_LIMIT);
    }
    counters_.max_update_occupancy = std::max<uint64_t>(
        counters_.max_update_occupancy, update_queue_->pendingSize());
    return ecg_record::Status::OK;
}

ecg_record::Status
Runtime::completeMemoryAccess(
    MemoryAccessKind kind, uint64_t cycle)
{
    if (kind == MemoryAccessKind::NONE)
        return ecg_record::Status::OK;
    if (kind == MemoryAccessKind::DESIGNATED)
        return completePropertyAccess(cycle);
    if (!active_ || !pending_ordinary_.valid ||
        cycle < pending_ordinary_.begin_cycle)
        return fail(ecg_record::Status::INVALID_SEQUENCE);
    if (replacementActive()) {
        ecg_record::CommitUpdate update;
        update.physical_line = pending_ordinary_.physical_line;
        update.property_vaddr = pending_ordinary_.property_vaddr;
        update.sequence = pending_ordinary_.sequence;
        update.generation = configuration_.generation;
        update.context = static_cast<uint16_t>(configuration_.context);
        update.sequence_bits = layout_.sequence_bits;
        update.state = ecg_record::State::UNKNOWN;
        if (!ecg_record::checkedAdd(
                event_order_, 1, event_order_)) {
            pending_ordinary_ = PendingOrdinary{};
            return fail(ecg_record::Status::ARITHMETIC_OVERFLOW);
        }
        update.order = event_order_;
        update.invalidate = true;
        const auto status = enqueueUpdate(update, cycle);
        pending_ordinary_ = PendingOrdinary{};
        return status;
    }
    pending_ordinary_ = PendingOrdinary{};
    return ecg_record::Status::OK;
}

bool
Runtime::activeObservation(
    uint64_t physical_line, Observation& observation) const
{
    observation = Observation{};
    if (!replacementActive() ||
        !pending_property_.valid || !pending_property_.memory_active ||
        pending_property_.physical_line != physical_line) {
        return false;
    }
    const auto& load = pending_property_.load;
    observation.physical_line = physical_line;
    observation.property_vaddr = load.property_address;
    observation.destination = load.destination;
    observation.sequence = load.sequence;
    observation.deadline = load.deadline;
    observation.generation = load.generation;
    observation.context = load.context;
    observation.state = load.state;
    observation.valid = true;
    return true;
}

bool
Runtime::activeDeadDemand(uint64_t physical_line) const
{
    return replacementActive() &&
        pending_property_.valid &&
        pending_property_.memory_active &&
        pending_property_.physical_line == physical_line &&
        pending_property_.load.state == ecg_record::State::DEAD;
}

void
Runtime::noteDeadDemandBypass()
{
    ++counters_.dead_demand_bypasses;
}

ecg_record::ObservationResult
Runtime::observeLine(ecg_record::LineMetadata& metadata)
{
    if (!replacementActive() ||
        !pending_property_.valid || !pending_property_.memory_active)
        return ecg_record::ObservationResult::UNSUPPORTED;
    const ecg_record::ObservationResult result = receiver_.observe(
        metadata, pending_property_.load.context,
        pending_property_.load.generation,
        pending_property_.load.sequence);
    if (result == ecg_record::ObservationResult::ACCEPTED)
        ++counters_.observations;
    return result;
}

ecg_record::ObservationResult
Runtime::observeOrdinaryLine(
    uint64_t physical_line, ecg_record::LineMetadata& metadata)
{
    if (!active_ || !pending_ordinary_.valid ||
        pending_ordinary_.physical_line != physical_line)
        return ecg_record::ObservationResult::UNSUPPORTED;
    return receiver_.invalidateObservation(
        metadata, static_cast<uint16_t>(configuration_.context),
        configuration_.generation, pending_ordinary_.sequence);
}

ecg_record::PopResult
Runtime::popReadyUpdate(uint64_t cycle)
{
    if (!update_queue_)
        return {};
    const auto result = update_queue_->popReady(cycle);
    if (result.status == ecg_record::PopStatus::POPPED)
        counters_.minimum_update_latency = std::min(
            counters_.minimum_update_latency, cycle - result.ready.generation_cycle);
    return result;
}

ecg_record::ApplyResult
Runtime::applyUpdate(
    ecg_record::LineMetadata* metadata,
    const ecg_record::CommitUpdate& update)
{
    if (!replacementActive())
        return ecg_record::ApplyResult::UNSUPPORTED;
    ++counters_.delivered_updates;
    const ecg_record::ApplyResult result =
        receiver_.apply(metadata, update);
    if (result == ecg_record::ApplyResult::APPLIED)
        ++counters_.applied_updates;
    else if (result == ecg_record::ApplyResult::STALE)
        ++counters_.stale_updates;
    else if (result == ecg_record::ApplyResult::EXPIRED)
        ++counters_.expired_updates;
    else if (result == ecg_record::ApplyResult::NOT_RESIDENT)
        ++counters_.not_resident_updates;
    else
        fail(ecg_record::Status::INVALID_SEQUENCE);
    return result;
}

bool
Runtime::takeReadyPrefetch(
    uint64_t cycle, PrefetchRequest& request)
{
    request = PrefetchRequest{};
    if (prefetch_queue_.empty())
        return false;
    if (prefetch_output_seen_ && cycle == last_prefetch_output_cycle_)
        return false;
    PendingPrefetch& front = prefetch_queue_.front();
    if (front.ready_cycle > cycle)
        return false;
    request.property_vaddr = front.property_vaddr;
    request.sequence = front.sequence;
    request.ready_cycle = front.ready_cycle;
    request.valid = true;
    prefetch_queue_.pop_front();
    prefetch_output_seen_ = true;
    last_prefetch_output_cycle_ = cycle;
    return true;
}

void Runtime::notePrefetchPrivateDuplicate()
{ ++counters_.prefetch_private_duplicates; }
void Runtime::notePrefetchLlcDuplicate()
{ ++counters_.prefetch_llc_duplicates; }
void Runtime::notePrefetchCompletionPrivateDuplicate()
{ ++counters_.prefetch_completion_private_duplicates; }
void Runtime::notePrefetchCompletionResident()
{ ++counters_.prefetch_completion_resident; }
void Runtime::notePrefetchIssueAdmissionDrop()
{ ++counters_.prefetch_issue_admission_drops; }
void Runtime::notePrefetchCompletionAdmissionDrop()
{ ++counters_.prefetch_completion_admission_drops; }
void Runtime::notePrefetchIssued()
{
    ++counters_.prefetch_issued;
    counters_.prefetch_request_bytes += line_bytes_;
}
void Runtime::notePrefetchRetry()
{ ++counters_.prefetch_retry_waits; }
void Runtime::notePrefetchPrivateLookup()
{ ++counters_.prefetch_private_lookups; }
void Runtime::notePrefetchLlcLookup()
{ ++counters_.prefetch_llc_lookups; }
void Runtime::notePrefetchIssueAdmissionCheck()
{ ++counters_.prefetch_issue_admission_checks; }
void Runtime::notePrefetchCompletionAdmissionCheck()
{ ++counters_.prefetch_completion_admission_checks; }
void Runtime::notePrefetchDemandMerge()
{ ++counters_.prefetch_demand_merges; }
void Runtime::noteLookupCycles(uint64_t cycles)
{ counters_.lookup_cycles_charged += cycles; }
void Runtime::notePrefetchTranslation(bool fault)
{
    ++counters_.prefetch_translation_requests;
    if (fault)
        ++counters_.prefetch_translation_faults;
}
void Runtime::notePrefetchTranslationBypass()
{ ++counters_.prefetch_translation_bypasses; }
void Runtime::notePrefetchFill(uint64_t latency_cycles)
{
    ++counters_.prefetch_fills;
    counters_.prefetch_fill_bytes += line_bytes_;
    counters_.prefetch_latency_cycles += latency_cycles;
}

std::optional<uint64_t>
Runtime::nextReadyCycle() const
{
    std::optional<uint64_t> next;
    if (update_queue_) {
        const auto ready = update_queue_->nextReadyCycle();
        if (ready)
            next = ready;
    }
    if (!prefetch_queue_.empty() &&
        (!next || prefetch_queue_.front().ready_cycle < *next))
        next = prefetch_queue_.front().ready_cycle;
    return next;
}

bool
Runtime::updateQueueEmpty() const
{
    return !update_queue_ || update_queue_->empty();
}

bool
Runtime::clean() const
{
    return !pending_property_.valid && !pending_ordinary_.valid &&
        armed_read_bytes_ == 0 &&
        !read_observed_ && loaded_chunks_ == 0 &&
        updateQueueEmpty() && prefetch_queue_.empty() &&
        (managed_
            ? !pass_cursor_.open() && !iteration_configured_
            : iteration_configured_ &&
                iteration_consumed_count_ == configuration_.record_count) &&
        counters_.errors == 0;
}

}  // namespace record
}  // namespace sniper
}  // namespace graphbrew
