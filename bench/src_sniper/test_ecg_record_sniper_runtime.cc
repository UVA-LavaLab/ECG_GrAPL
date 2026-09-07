#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ecg_record_stream.h"
#include "ecg_record_guest.h"
#include "sniper_sim/overlays/common/core/memory_subsystem/cache/ecg_record_sniper_runtime.h"

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    std::printf("%-58s [%s]\n", name, condition ? "OK" : "FAIL");
    if (!condition)
        ++failures;
}

void exercise(uint8_t requested_bytes)
{
    constexpr uint64_t vertices = 512;
    std::vector<uint64_t> destinations;
    for (uint64_t index = 0; index < 16; ++index)
        destinations.push_back(index * 16);

    ecg_record::Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = vertices - 1;
    requirements.record_count = destinations.size();
    requirements.traversal_count = 1;
    requirements.requested_record_bytes = requested_bytes;

    ecg_record::Layout layout;
    check(ecg_record::selectLayout(requirements, layout) ==
              ecg_record::Status::OK &&
          layout.record_bytes == requested_bytes,
          requested_bytes == 4 ? "select four-byte layout"
                               : "select eight-byte layout");

    ecg_record::RecordStream stream;
    ecg_record::BuildLimits limits;
    limits.maximum_carrier_bytes = 1 << 20;
    limits.maximum_auxiliary_bytes = 1 << 20;
    check(ecg_record::buildRecords(
              requirements, layout, 16,
              [&](std::size_t index) { return destinations[index]; },
              stream, limits) == ecg_record::Status::OK,
          requested_bytes == 4 ? "build four-byte stream"
                               : "build eight-byte stream");

    uint64_t descriptor = 0;
    check(ecg_record::packLayout(layout, descriptor) ==
              ecg_record::Status::OK,
          "pack selected layout");
    ecg_record::NativeConfiguration configuration;
    configuration.layout_descriptor = descriptor;
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = 0x40000000;
    configuration.record_count = stream.size();
    configuration.vertex_count = vertices;
    configuration.iteration_base = 0;
    configuration.generation = requested_bytes;
    configuration.context = 1;
    configuration.control =
        ecg_record::kNativeEnable | ecg_record::kNativeHasNext;

    graphbrew::sniper::record::Runtime runtime;
    check(runtime.configure(
              configuration,
              ecg_record::Mechanism::REPLACEMENT_PREFETCH,
              8, 4, 8, 8, 64) == ecg_record::Status::OK &&
          runtime.updateIteration(0, false) ==
              ecg_record::Status::OK,
          "configure runtime");

    for (uint64_t index = 0; index < 16; ++index) {
        uint64_t address = 0;
        check(ecg_record::recordAddress(
                  layout, configuration.record_base, index,
                  configuration.record_count, address) ==
                  ecg_record::Status::OK,
              "derive record address");
        const uint64_t word = stream.word(index);
        check(runtime.armRecordRead(address, requested_bytes) ==
                  ecg_record::Status::OK &&
              runtime.observeRecordRead(
                  address, requested_bytes, 10 + index) ==
                  ecg_record::Status::OK &&
              runtime.setLoadedAddress(address) ==
                  ecg_record::Status::OK &&
              runtime.setLoadedChunk(
                  static_cast<uint32_t>(word), false) ==
                  ecg_record::Status::OK &&
              (requested_bytes == 4 ||
               runtime.setLoadedChunk(
                   static_cast<uint32_t>(word >> 32), true) ==
                   ecg_record::Status::OK) &&
              runtime.commitLoadedValue(requested_bytes) ==
                  ecg_record::Status::OK,
              "bind actual record load and chunks");
    }

    std::array<ecg_record::LineMetadata, 16> metadata{};
    uint64_t prefetches = 0;
    for (uint64_t index = 0; index < stream.size(); ++index) {
        uint64_t address = 0;
        ecg_record::recordAddress(
            layout, configuration.record_base, index,
            configuration.record_count, address);
        const uint64_t consume_cycle = 100 + index * 20;
        check(runtime.consumeRecord(address, consume_cycle) ==
                  ecg_record::Status::OK,
              "consume record in sequence");
        ecg_record::DecodedRecord decoded;
        ecg_record::decodeRecord(layout, stream.word(index), decoded);
        const uint64_t property_address =
            configuration.property_base +
            decoded.destination * sizeof(float);
        const uint64_t physical_line =
            property_address & ~uint64_t{63};
        check(runtime.beginPropertyAccess(
                  property_address, physical_line,
                  sizeof(float), consume_cycle),
              "bind exact property access");
        check(runtime.observeLine(metadata[index]) ==
                  ecg_record::ObservationResult::ACCEPTED,
              "mark resident line pending");
        const uint64_t completion_cycle = consume_cycle + 10;
        check(runtime.completePropertyAccess(completion_cycle) ==
                  ecg_record::Status::OK,
              "enqueue from actual completion cycle");
        check(runtime.popReadyUpdate(completion_cycle + 7).status ==
                  ecg_record::PopStatus::NOT_READY,
              "reject delivery before completion plus latency");
        const ecg_record::PopResult update =
            runtime.popReadyUpdate(completion_cycle + 8);
        check(update.status == ecg_record::PopStatus::POPPED &&
              runtime.applyUpdate(
                  &metadata[index], update.ready.update) ==
                  ecg_record::ApplyResult::APPLIED,
              "deliver non-touching metadata update");

        graphbrew::sniper::record::PrefetchRequest prefetch;
        check(!runtime.takeReadyPrefetch(
                  completion_cycle + 7, prefetch),
              "prefetch drain cannot bypass readiness");
        if (runtime.takeReadyPrefetch(
                completion_cycle + 8, prefetch)) {
            ++prefetches;
            runtime.notePrefetchIssued();
            runtime.notePrefetchTranslation(false);
            runtime.notePrefetchFill(12);
        }
    }
    check(runtime.clean() &&
          runtime.deactivate() == ecg_record::Status::OK &&
          runtime.counters().record_reads == 16 &&
          runtime.counters().record_read_bytes ==
              uint64_t{16} * requested_bytes &&
          runtime.counters().generated_updates == 16 &&
          runtime.counters().delivered_updates == 16 &&
          runtime.counters().prefetch_issued == prefetches &&
          runtime.counters().prefetch_fills == prefetches &&
          runtime.counters().errors == 0,
          "runtime accounting closes");
}

void negativeProtocols()
{
    const std::vector<uint64_t> destinations = {0};
    ecg_record::Requirements requirements;
    requirements.vertex_count = 2;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = 0;
    requirements.record_count = 1;
    requirements.requested_record_bytes = 4;
    ecg_record::Layout layout;
    ecg_record::selectLayout(requirements, layout);
    ecg_record::RecordStream stream;
    ecg_record::BuildLimits limits;
    limits.maximum_carrier_bytes = 4096;
    limits.maximum_auxiliary_bytes = 4096;
    ecg_record::buildRecords(
        requirements, layout, 16,
        [&](std::size_t index) { return destinations[index]; },
        stream, limits);
    uint64_t descriptor = 0;
    ecg_record::packLayout(layout, descriptor);
    ecg_record::NativeConfiguration configuration;
    configuration.layout_descriptor = descriptor;
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = 0x80000000;
    configuration.record_count = 1;
    configuration.vertex_count = 2;
    configuration.generation = 7;
    configuration.context = 3;
    configuration.control = ecg_record::kNativeEnable;
    const uint64_t word = stream.word(0);

    graphbrew::sniper::record::Runtime geometry;
    check(geometry.configure(
              configuration, ecg_record::Mechanism::PREFETCH,
              8, 1, 8, 8, 32) == ecg_record::Status::INVALID_LAYOUT,
          "runtime rejects geometry inconsistent with its 64-byte protocol");

    ecg_record::NativeConfiguration managed_zero_configuration =
        configuration;
    managed_zero_configuration.control =
        ecg_record::kNativeEnable |
        ecg_record::kNativeManagedPasses;
    graphbrew::sniper::record::Runtime managed_zero;
    check(managed_zero.configure(
              managed_zero_configuration,
              ecg_record::Mechanism::TRANSPORT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          managed_zero.updateIteration(
              0, false, true) == ecg_record::Status::OK &&
          managed_zero.armRecordRead(
              managed_zero_configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          managed_zero.observeRecordRead(
              managed_zero_configuration.record_base, 4, 10) ==
              ecg_record::Status::OK &&
          managed_zero.setLoadedAddress(
              managed_zero_configuration.record_base) ==
              ecg_record::Status::OK &&
          managed_zero.setLoadedChunk(
              static_cast<uint32_t>(word), false) ==
              ecg_record::Status::OK &&
          managed_zero.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          managed_zero.consumeRecord(
              managed_zero_configuration.record_base, 10) ==
              ecg_record::Status::OK &&
          managed_zero.beginMemoryAccess(
              managed_zero_configuration.property_base,
              managed_zero_configuration.property_base & ~uint64_t{63},
              4, true, 20) ==
              graphbrew::sniper::record::MemoryAccessKind::DESIGNATED &&
          managed_zero.completeMemoryAccess(
              graphbrew::sniper::record::MemoryAccessKind::DESIGNATED,
              21) == ecg_record::Status::OK &&
          managed_zero.closePass() == ecg_record::Status::OK &&
          managed_zero.clean() &&
          managed_zero.deactivate() == ecg_record::Status::OK,
          "managed flag enables descriptor-zero F32 pass lifecycle");

    graphbrew::sniper::record::Runtime missing;
    check(missing.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          missing.updateIteration(0, false) == ecg_record::Status::OK &&
          missing.consumeRecord(
              configuration.record_base, 1) ==
              ecg_record::Status::NOT_READY,
          "missing record load fails closed");
    check(missing.propertyLine(configuration.property_base) &&
          !missing.propertyLine(configuration.property_base - 64) &&
          !missing.propertyLine(configuration.property_base + 64),
          "record governance is limited to its configured property array");

    graphbrew::sniper::record::Runtime chunks;
    check(chunks.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          chunks.updateIteration(0, false) == ecg_record::Status::OK &&
          chunks.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          chunks.commitLoadedValue(4) ==
              ecg_record::Status::INVALID_SEQUENCE,
          "missing completed read and chunks fail closed");

    graphbrew::sniper::record::Runtime delayed;
    check(delayed.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          delayed.updateIteration(0, false) == ecg_record::Status::OK &&
          delayed.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          delayed.observeRecordRead(
              configuration.record_base, 4, 100) ==
              ecg_record::Status::OK &&
          delayed.setLoadedAddress(
              configuration.record_base) ==
              ecg_record::Status::OK &&
          delayed.setLoadedChunk(
              static_cast<uint32_t>(stream.word(0)), false) ==
              ecg_record::Status::OK &&
          delayed.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          delayed.consumeRecord(
              configuration.record_base, 99) ==
              ecg_record::Status::NOT_READY,
          "premature record consume is rejected");

    graphbrew::sniper::record::Runtime intervening;
    check(intervening.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          intervening.updateIteration(0, false) ==
              ecg_record::Status::OK &&
          intervening.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          intervening.observeRecordRead(
              configuration.record_base, 4, 10) ==
              ecg_record::Status::OK &&
          intervening.setLoadedAddress(
              configuration.record_base) ==
              ecg_record::Status::OK &&
          intervening.setLoadedChunk(
              static_cast<uint32_t>(word), false) ==
              ecg_record::Status::OK &&
          intervening.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          intervening.consumeRecord(
              configuration.record_base, 10) ==
              ecg_record::Status::OK &&
          intervening.beginMemoryAccess(
              configuration.property_base + 128,
              (configuration.property_base + 128) & ~uint64_t{63},
              4, true, 11) ==
              graphbrew::sniper::record::MemoryAccessKind::NONE &&
          intervening.completeMemoryAccess(
              graphbrew::sniper::record::MemoryAccessKind::NONE,
              12) == ecg_record::Status::OK &&
          intervening.beginMemoryAccess(
              configuration.property_base,
              configuration.property_base & ~uint64_t{63},
              4, true, 13) ==
              graphbrew::sniper::record::MemoryAccessKind::DESIGNATED &&
          intervening.completeMemoryAccess(
              graphbrew::sniper::record::MemoryAccessKind::DESIGNATED,
              14) == ecg_record::Status::OK,
          "unrelated memory may intervene before the matching property");

    graphbrew::sniper::record::Runtime reconfigure;
    check(reconfigure.configure(
              configuration, ecg_record::Mechanism::TRANSPORT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          reconfigure.configure(
              configuration, ecg_record::Mechanism::TRANSPORT,
              8, 1, 8, 8, 64) ==
              ecg_record::Status::INVALID_SEQUENCE,
          "active reconfiguration is rejected");
    check(reconfigure.updateIteration(1, false) ==
              ecg_record::Status::INVALID_SEQUENCE,
          "discontinuous first iteration base is rejected");
    check(reconfigure.deactivate() ==
              ecg_record::Status::NOT_READY,
          "incomplete final drain is rejected");

    graphbrew::sniper::record::Runtime prefetch_only;
    check(prefetch_only.configure(
              configuration, ecg_record::Mechanism::PREFETCH,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          prefetch_only.updateIteration(0, false) ==
              ecg_record::Status::OK &&
          prefetch_only.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          prefetch_only.observeRecordRead(
              configuration.record_base, 4, 10) ==
              ecg_record::Status::OK &&
          prefetch_only.setLoadedAddress(
              configuration.record_base) ==
              ecg_record::Status::OK &&
          prefetch_only.setLoadedChunk(
              static_cast<uint32_t>(word), false) ==
              ecg_record::Status::OK &&
          prefetch_only.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          prefetch_only.consumeRecord(
              configuration.record_base, 10) ==
              ecg_record::Status::OK,
          "prefetch-only consumes a real completed record");
    ecg_record::DecodedRecord decoded;
    ecg_record::decodeRecord(layout, word, decoded);
    const uint64_t property_address =
        configuration.property_base + decoded.destination * sizeof(float);
    ecg_record::LineMetadata ignored;
    check(prefetch_only.beginPropertyAccess(
              property_address, property_address & ~uint64_t{63},
              sizeof(float), 20) &&
          prefetch_only.observeLine(ignored) ==
              ecg_record::ObservationResult::UNSUPPORTED &&
          prefetch_only.completePropertyAccess(30) ==
              ecg_record::Status::OK &&
          prefetch_only.counters().generated_updates == 0 &&
          prefetch_only.completedSequence() == 1 &&
          prefetch_only.updateQueueEmpty(),
          "prefetch-only does not generate replacement updates");

    graphbrew::sniper::record::Runtime generation;
    ecg_record::CommitUpdate wrong_generation;
    wrong_generation.physical_line = 0x1000;
    wrong_generation.property_vaddr = configuration.property_base;
    wrong_generation.sequence = 1;
    wrong_generation.deadline = 2;
    wrong_generation.generation = configuration.generation + 1;
    wrong_generation.context =
        static_cast<uint16_t>(configuration.context);
    wrong_generation.sequence_bits = 64;
    wrong_generation.state = ecg_record::State::FINITE;
    ecg_record::LineMetadata generation_line;
    check(generation.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          generation.applyUpdate(
              &generation_line, wrong_generation) ==
              ecg_record::ApplyResult::INVALID_CONTEXT,
          "generation mismatch is rejected");

    graphbrew::sniper::record::Runtime boundary;
    configuration.control =
        ecg_record::kNativeEnable | ecg_record::kNativeHasNext;
    check(boundary.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          boundary.updateIteration(0, true) ==
              ecg_record::Status::OK &&
          boundary.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          boundary.observeRecordRead(
              configuration.record_base, 4, 10) ==
              ecg_record::Status::OK &&
          boundary.setLoadedAddress(
              configuration.record_base) ==
              ecg_record::Status::OK &&
          boundary.setLoadedChunk(
              static_cast<uint32_t>(word), false) ==
              ecg_record::Status::OK &&
          boundary.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          boundary.consumeRecord(
              configuration.record_base, 10) ==
              ecg_record::Status::OK &&
          boundary.beginPropertyAccess(
              property_address, property_address & ~uint64_t{63},
              sizeof(float), 20) &&
          boundary.completePropertyAccess(30) ==
              ecg_record::Status::OK &&
          boundary.updateIteration(1, false) ==
              ecg_record::Status::OK &&
          !boundary.updateQueueEmpty(),
          "iteration boundary preserves legitimate pending updates");
    check(boundary.popReadyUpdate(37).status ==
              ecg_record::PopStatus::NOT_READY &&
          boundary.popReadyUpdate(38).status ==
              ecg_record::PopStatus::POPPED,
          "pending update retains completion-based readiness");

    configuration.control = ecg_record::kNativeEnable;
    graphbrew::sniper::record::Runtime dead;
    check(dead.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          dead.updateIteration(0, false) ==
              ecg_record::Status::OK &&
          dead.armRecordRead(
              configuration.record_base, 4) ==
              ecg_record::Status::OK &&
          dead.observeRecordRead(
              configuration.record_base, 4, 10) ==
              ecg_record::Status::OK &&
          dead.setLoadedAddress(configuration.record_base) ==
              ecg_record::Status::OK &&
          dead.setLoadedChunk(
              static_cast<uint32_t>(word), false) ==
              ecg_record::Status::OK &&
          dead.commitLoadedValue(4) ==
              ecg_record::Status::OK &&
          dead.consumeRecord(
              configuration.record_base, 10) ==
              ecg_record::Status::OK &&
          dead.beginPropertyAccess(
              property_address, property_address & ~uint64_t{63},
              sizeof(float), 20) &&
          dead.activeDeadDemand(property_address & ~uint64_t{63}),
          "known-dead demand is exposed for LLC allocation bypass");
}

void managedFilteredLifecycle()
{
    const std::vector<uint64_t> destinations = {0, 1, 2, 3};
    ecg_record::Requirements requirements;
    requirements.vertex_count = 4;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = 3;
    requirements.record_count = destinations.size();
    requirements.requested_record_bytes = 4;
    ecg_record::Layout layout;
    ecg_record::selectLayout(requirements, layout);
    ecg_record::PropertyDescriptor property{
        ecg_record::PropertyKind::U64, 8,
        ecg_record::TraversalMode::ORDERED_FILTERED};
    ecg_record::RecordStream stream;
    ecg_record::BuildLimits limits;
    limits.maximum_carrier_bytes = 4096;
    limits.maximum_auxiliary_bytes = 4096;
    check(ecg_record::buildRecords(
              requirements, layout, property, 0x90000008,
              [&](std::size_t index) { return destinations[index]; },
              stream, limits) == ecg_record::Status::OK,
          "build filtered U64 stream");
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    ecg_record::packProperty(property, configuration.property_descriptor);
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = 0x90000008;
    configuration.record_count = stream.size();
    configuration.vertex_count = 4;
    configuration.generation = 1;
    configuration.context = 1;
    configuration.control = ecg_record::kNativeEnable |
        ecg_record::kNativeManagedPasses;

    graphbrew::sniper::record::Runtime runtime;
    check(runtime.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 4, 8, 8, 64) == ecg_record::Status::OK &&
          runtime.updateIteration(
              0, false, true) == ecg_record::Status::OK,
          "configure filtered U64 managed pass");

    for (uint64_t index = 0; index < stream.size(); ++index) {
        uint64_t address = 0;
        ecg_record::recordAddress(
            layout, configuration.record_base, index,
            configuration.record_count, address);
        const uint64_t word = stream.word(index);
        check(runtime.armRecordRead(address, 4) == ecg_record::Status::OK &&
              runtime.observeRecordRead(address, 4, 10 + index) ==
                  ecg_record::Status::OK &&
              runtime.setLoadedAddress(address) == ecg_record::Status::OK &&
              runtime.setLoadedChunk(
                  static_cast<uint32_t>(word), false) ==
                  ecg_record::Status::OK &&
              runtime.commitLoadedValue(4) == ecg_record::Status::OK,
              "load filtered record window");
    }

    std::array<ecg_record::LineMetadata, 3> metadata{};
    auto consume = [&](uint64_t index, uint64_t cycle, std::size_t slot) {
        uint64_t address = 0;
        ecg_record::recordAddress(
            layout, configuration.record_base, index,
            configuration.record_count, address);
        ecg_record::DecodedRecord decoded;
        ecg_record::decodeRecord(layout, stream.word(index), decoded);
        uint64_t property_address = 0;
        ecg_record::propertyAddress(
            property, configuration.property_base,
            decoded.destination, property_address);
        bool prefix_ok = runtime.consumeRecord(address, cycle) ==
            ecg_record::Status::OK;
        if (index == 1) {
            const uint64_t ordinary_address =
                configuration.property_base;
            prefix_ok = prefix_ok &&
                runtime.beginMemoryAccess(
                    ordinary_address,
                    ordinary_address & ~uint64_t{63},
                    8, true, cycle) ==
                    graphbrew::sniper::record::MemoryAccessKind::ORDINARY &&
                runtime.observeOrdinaryLine(
                    ordinary_address & ~uint64_t{63},
                    metadata[slot]) ==
                    ecg_record::ObservationResult::ACCEPTED &&
                runtime.completeMemoryAccess(
                    graphbrew::sniper::record::MemoryAccessKind::ORDINARY,
                    cycle + 1) == ecg_record::Status::OK;
        }
        check(prefix_ok &&
              runtime.beginMemoryAccess(
                  property_address, property_address & ~uint64_t{63},
                  8, true, cycle + 2) ==
                  graphbrew::sniper::record::MemoryAccessKind::DESIGNATED &&
              runtime.observeLine(metadata[slot]) ==
                  ecg_record::ObservationResult::ACCEPTED &&
              runtime.completeMemoryAccess(
                  graphbrew::sniper::record::MemoryAccessKind::DESIGNATED,
                  cycle + 3) == ecg_record::Status::OK,
              "consume filtered designated U64 access");
    };
    consume(1, 100, 0);
    const uint64_t ordinary_address = configuration.property_base;
    check(runtime.beginMemoryAccess(
              ordinary_address, ordinary_address & ~uint64_t{63},
              8, true, 110) ==
              graphbrew::sniper::record::MemoryAccessKind::ORDINARY &&
          runtime.observeOrdinaryLine(
              ordinary_address & ~uint64_t{63}, metadata[1]) ==
              ecg_record::ObservationResult::ACCEPTED &&
          runtime.completeMemoryAccess(
              graphbrew::sniper::record::MemoryAccessKind::ORDINARY,
              111) == ecg_record::Status::OK,
          "ordinary governed access invalidates through paid path");
    consume(3, 120, 2);
    check(runtime.closePass() == ecg_record::Status::OK,
          "filtered pass closes with skipped positions");
    ecg_record::LineMetadata closed_bound;
    closed_bound.state = ecg_record::LineState::FINITE;
    closed_bound.value = configuration.record_count;
    check(runtime.victimState(closed_bound, true) ==
              ecg_record::State::UNKNOWN,
          "closed-pass filtered bound is not ranked finite");
    for (uint64_t cycle = 130; !runtime.updateQueueEmpty(); ++cycle) {
        const auto update = runtime.popReadyUpdate(cycle);
        if (update.status == ecg_record::PopStatus::POPPED)
            runtime.applyUpdate(nullptr, update.ready.update);
    }
    check(runtime.clean() &&
          runtime.invalidateBinding(4, 4) ==
              ecg_record::Status::OK &&
          runtime.counters().passes == 1 &&
          runtime.counters().consumed_records == 2 &&
          runtime.counters().skipped_positions == 2 &&
          runtime.counters().ordinary_invalidations == 2 &&
          runtime.counters().invalidation_sets == 4 &&
          runtime.counters().invalidation_cycles == 4,
          "filtered accounting and modeled invalidation close");

    configuration.generation = 2;
    check(runtime.configure(
              configuration, ecg_record::Mechanism::REPLACEMENT,
              8, 4, 8, 8, 64) == ecg_record::Status::OK &&
          runtime.counters().rebinds == 1,
          "consecutive generation rebind succeeds");
}

void managedGuestContext()
{
    const std::vector<uint64_t> destinations = {0, 1, 2, 3};
    uint64_t values[] = {10, 20, 30, 40};
    ecg_record::Requirements requirements;
    requirements.vertex_count = 4;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = 3;
    requirements.record_count = destinations.size();
    requirements.requested_record_bytes = 4;
    ecg_record::Layout layout;
    ecg_record::selectLayout(requirements, layout);
    const ecg_record::PropertyDescriptor property{
        ecg_record::PropertyKind::U64, 8,
        ecg_record::TraversalMode::ORDERED_FILTERED};
    ecg_record::RecordStream stream;
    ecg_record::BuildLimits limits;
    limits.maximum_carrier_bytes = 4096;
    limits.maximum_auxiliary_bytes = 4096;
    ecg_record::buildRecords(
        requirements, layout, property,
        reinterpret_cast<uint64_t>(values),
        [&](std::size_t index) { return destinations[index]; },
        stream, limits);
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    ecg_record::packProperty(property, configuration.property_descriptor);
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base =
        reinterpret_cast<uint64_t>(values);
    configuration.record_count = stream.size();
    configuration.vertex_count = 4;
    configuration.generation = 1;
    configuration.context = 1;
    configuration.control = ecg_record::kNativeEnable;

    graphbrew_sniper::EcgRecordContext context;
    context.bind(configuration, stream);
    context.beginPass();
    const uint64_t first_record = context.loadRecord(1);
    const uint64_t first =
        context.loadProperty(1, first_record, values);
    const uint64_t second_record = context.loadRecord(3);
    const uint64_t second =
        context.loadProperty(3, second_record, values);
    context.closePass();
    check(first == 20 && second == 40 &&
          context.passes() == 1 && context.consumed() == 2 &&
          context.skipped() == 2,
          "generic Sniper guest context supports filtered U64");

    float values_f32[] = {1, 2, 3, 4};
    const ecg_record::PropertyDescriptor property_f32{};
    ecg_record::RecordStream stream_f32;
    ecg_record::buildRecords(
        requirements, layout, property_f32,
        reinterpret_cast<uint64_t>(values_f32),
        [&](std::size_t index) { return destinations[index]; },
        stream_f32, limits);
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream_f32.data());
    configuration.property_base =
        reinterpret_cast<uint64_t>(values_f32);
    configuration.generation = 2;
    ecg_record::packProperty(
        property_f32, configuration.property_descriptor);
    context.bind(configuration, stream_f32);
    context.beginPass();
    bool f32_ok = configuration.property_descriptor == 0;
    for (uint64_t index = 0; index < stream_f32.size(); ++index) {
        const uint64_t record = context.loadRecord(index);
        f32_ok &= context.loadProperty(
            index, record, values_f32) == values_f32[index];
    }
    context.closePass();
    check(f32_ok,
          "generic Sniper guest context manages descriptor-zero F32");
    context.finish(6);
}

void isolatedVertexLayout()
{
    const int32_t source[] = {0, 1, 3};
    const uint64_t maximum =
        graphbrew_sniper::ecg_record_max_source_id(
            source, 3, 1024);
    ecg_record::Requirements requirements;
    requirements.vertex_count = 1024;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = maximum;
    requirements.record_count = 3;
    ecg_record::Layout layout;
    check(maximum == 3 &&
          ecg_record::selectLayout(requirements, layout) ==
              ecg_record::Status::OK &&
          layout.id_bits == 2,
          "isolated vertices do not inflate record ID width");

    setenv("SNIPER_ECG_RECORD_TEST_UINT", "-1", 1);
    bool negative_rejected = false;
    try {
        (void)graphbrew_sniper::ecg_record_env_u64(
            "SNIPER_ECG_RECORD_TEST_UINT", 0, 0, UINT64_MAX);
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }

    setenv(
        "SNIPER_ECG_RECORD_TEST_UINT",
        "18446744073709551616", 1);
    bool overflow_rejected = false;
    try {
        (void)graphbrew_sniper::ecg_record_env_u64(
            "SNIPER_ECG_RECORD_TEST_UINT", 0, 0, UINT64_MAX);
    } catch (const std::invalid_argument&) {
        overflow_rejected = true;
    }
    unsetenv("SNIPER_ECG_RECORD_TEST_UINT");
    check(negative_rejected && overflow_rejected,
          "unsigned environment parsing rejects negative and overflow");
}

void incompletePrefetchWindow()
{
    ecg_record::Requirements requirements;
    requirements.vertex_count = 512;
    requirements.record_count = 16;
    ecg_record::Layout layout;
    ecg_record::selectLayout(requirements, layout);
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    configuration.vertex_count = 512;
    configuration.record_count = 16;
    configuration.record_base = 0x1000;
    configuration.property_base = 0x8000;
    configuration.context = 1;
    configuration.generation = 1;
    configuration.control = ecg_record::kNativeEnable;
    uint64_t word = 0;
    ecg_record::encodeRecord(layout, 0, 4, ecg_record::State::FINITE, word);
    graphbrew::sniper::record::Runtime runtime;
    check(runtime.configure(configuration, ecg_record::Mechanism::PREFETCH,
              8, 1, 8, 8, 64) == ecg_record::Status::OK &&
          runtime.updateIteration(0, false) == ecg_record::Status::OK &&
          runtime.armRecordRead(0x1000, 4) == ecg_record::Status::OK &&
          runtime.observeRecordRead(0x1000, 4, 10) == ecg_record::Status::OK &&
          runtime.setLoadedAddress(0x1000) == ecg_record::Status::OK &&
          runtime.setLoadedChunk(static_cast<uint32_t>(word), false) == ecg_record::Status::OK &&
          runtime.commitLoadedValue(4) == ecg_record::Status::OK &&
          runtime.consumeRecord(0x1000, 10) == ecg_record::Status::NOT_READY,
          "missing promised window bytes cannot become an empty prefetch decision");
}

}  // namespace

int main()
{
    incompletePrefetchWindow();
    exercise(4);
    exercise(8);
    negativeProtocols();
    managedFilteredLifecycle();
    managedGuestContext();
    isolatedVertexLayout();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
