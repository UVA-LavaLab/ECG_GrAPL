#include "mem/cache/replacement_policies/ecg_record_transport.hh"

#include <algorithm>
#include <iostream>

#include "arch/generic/isa.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/ecg_record_prefetch.hh"
#include "mem/request.hh"

namespace gem5
{
namespace
{

bool
sameRecord(const ecg_record::NativeLoadResult& left,
           const ecg_record::NativeLoadResult& right)
{
    return left.raw_record == right.raw_record &&
        left.record_address == right.record_address &&
        left.destination == right.destination && left.sequence == right.sequence &&
        left.context == right.context && left.generation == right.generation &&
        left.layout_descriptor == right.layout_descriptor &&
        left.sequence_bits == right.sequence_bits &&
        left.has_next_iteration == right.has_next_iteration &&
        left.property_descriptor == right.property_descriptor &&
        left.property_kind == right.property_kind &&
        left.property_bytes == right.property_bytes;
}

} // anonymous namespace

EcgRecordTransport::EcgRecordTransport(const Params& params)
    : ClockedObject(params), cpu(dynamic_cast<o3::CPU*>(params.cpu)), llc(params.llc),
      prefetcher(params.prefetcher), applyUpdates(params.apply_updates),
      requiredContext(params.required_context),
      queue(static_cast<uint64_t>(params.latency), params.capture_width),
      serviceEvent([this] { service(); }, name() + ".service"),
      invalidationEvent([this] { invalidateSet(); }, name() + ".invalidate")
{
    fatal_if(!cpu || cpu->numContexts() != 1 ||
        cpu->getContext(0)->getIsaPtr()->getIsaName() != "riscv",
        "%s requires a single-threaded RISC-V O3 CPU", name());
    fatal_if(!llc || llc->getBlockSize() != 64 || !queue.validConfiguration() ||
        params.required_context == 0 || params.required_context > UINT16_MAX,
        "%s has invalid cache, context or bounded queue parameters", name());
    fatal_if((applyUpdates || prefetcher) && !llc->supportsEcgRecord(),
        "%s mechanisms require an adaptive ECG LLC", name());
    fatal_if(cpu->ecgRecordControl, "%s CPU already has an ECG controller", name());
    cpu->ecgRecordControl = this;
}

void
EcgRecordTransport::regProbeListeners()
{
    fatal_if(commitListener, "%s Commit observer was registered twice", name());
    commitListener = std::make_unique<ProbeListenerArgFunc<o3::DynInstPtr>>(
        cpu->getProbeManager(), "Commit",
        [this](const o3::DynInstPtr& inst) { observeCommit(inst); });
}

bool
EcgRecordTransport::validLoad(const o3::DynInstPtr& inst, uint8_t bytes) const
{
    return inst && inst->isLoad() && inst->effAddrValid() &&
        inst->translationCompleted() && inst->effSize == bytes &&
        inst->effAddr % bytes == 0 && inst->physEffAddr % bytes == 0 &&
        (inst->memReqFlags & Request::SECURE) == 0;
}

void
EcgRecordTransport::observeCommit(const o3::DynInstPtr& inst)
{
    if (!inst)
        return;
    switch (inst->ecgRecordHint().kind) {
      case ecg_record::InstructionKind::NONE: ordinaryAccess(inst); return;
      case ecg_record::InstructionKind::CONFIGURATION: configure(inst); return;
      case ecg_record::InstructionKind::RECORD: record(inst); return;
      case ecg_record::InstructionKind::PROPERTY: property(inst); return;
      case ecg_record::InstructionKind::PASS_CLOSE: passClose(inst); return;
      case ecg_record::InstructionKind::INVALIDATE: invalidateBinding(inst); return;
    }
    fatal("%s retired an invalid ECG instruction kind", name());
}

bool
EcgRecordTransport::managed() const
{
    return (configuration.control &
        ecg_record::kNativeManagedPasses) != 0;
}

void
EcgRecordTransport::configure(const o3::DynInstPtr& inst)
{
    const auto* value = inst->ecgRecordConfiguration();
    ecg_record::Layout resolved;
    fatal_if(!value || pendingRecord ||
        ecg_record::validateNativeConfiguration(*value, resolved) != ecg_record::Status::OK ||
        ecg_record::unpackProperty(
            value->property_descriptor, propertyDescriptor) != ecg_record::Status::OK ||
        value->context != requiredContext,
        "%s rejected the retired ECG configuration", name());
    const bool value_managed =
        (value->control & ecg_record::kNativeManagedPasses) != 0;
    if (configured && !bindingInvalidated) {
        const bool same_identity =
            value->generation == configuration.generation &&
            value->layout_descriptor == configuration.layout_descriptor &&
            value->record_base == configuration.record_base &&
            value->record_count == configuration.record_count &&
            value->vertex_count == configuration.vertex_count &&
            value->property_base == configuration.property_base &&
            value->property_descriptor == configuration.property_descriptor;
        fatal_if(!same_identity,
            "%s cannot change graph identity before a paid invalidation", name());
        fatal_if(value_managed != managed(),
            "%s cannot change its managed-pass contract while active", name());
        if (!value_managed) {
            fatal_if(value->iteration_base != lastSequence,
                "%s rejected a discontinuous legacy ECG traversal", name());
        } else {
            fatal_if(passCursor.open() ||
                value->iteration_base != passCursor.base() ||
                passCursor.begin() != ecg_record::Status::OK,
                "%s rejected a managed ECG pass begin", name());
            passConfigured = true;
        }
    } else {
        if (bindingInvalidated) {
            uint64_t next_generation = 0;
            fatal_if(!ecg_record::checkedAdd(
                    configuration.generation, 1, next_generation) ||
                value->generation != next_generation,
                "%s rejected a non-consecutive ECG generation", name());
            ++rebinds;
        }
        if (!value_managed) {
            fatal_if(value->iteration_base != lastSequence,
                "%s rejected the legacy ECG sequence base", name());
        } else {
            fatal_if(value->iteration_base != 0,
                "%s managed ECG binding must precede its first pass", name());
            passCursor = ecg_record::PassCursor{};
            fatal_if(passCursor.configure(
                value->record_count, propertyDescriptor.traversal) !=
                    ecg_record::Status::OK,
                "%s could not configure its managed pass cursor", name());
            passConfigured = false;
        }
        bindingInvalidated = false;
    }
    configuration = *value;
    layout = resolved;
    configured = true;
    ++configurations;
    fatal_if(llc->supportsEcgRecord() && !llc->configureEcgRecord(configuration),
             "%s LLC rejected the real guest configuration", name());
    if (prefetcher)
        prefetcher->configure(configuration);
}

void
EcgRecordTransport::record(const o3::DynInstPtr& inst)
{
    const auto& load = inst->ecgRecordHint().load;
    ecg_record::NativeLoadResult expected;
    fatal_if(!configured || bindingInvalidated || pendingRecord ||
        (managed() && (!passConfigured || !passCursor.open())) ||
        !validLoad(inst, layout.record_bytes) ||
        inst->effAddr != load.record_address ||
        ecg_record::nativeRecordResult(
            configuration, load.record_address, load.raw_record, expected) !=
                ecg_record::Status::OK || !sameRecord(expected, load),
        "%s retired an unbound or invalid ECG record read", name());
    pendingRecord = load;
    ++recordLoads;
    recordReadBytes += layout.record_bytes;
}

void
EcgRecordTransport::property(const o3::DynInstPtr& inst)
{
    const auto& load = inst->ecgRecordHint().load;
    ecg_record::NativeLoadResult expected;
    uint64_t next = 0;
    fatal_if(!configured || bindingInvalidated || !pendingRecord ||
        !validLoad(inst, load.property_bytes) ||
        !sameRecord(*pendingRecord, load) || inst->effAddr != load.property_address ||
        ecg_record::nativePropertyAccess(
            configuration, configuration.property_base, load.raw_record,
            load.record_address, expected) != ecg_record::Status::OK ||
        !sameRecord(expected, load) || expected.deadline != load.deadline ||
        expected.state != load.state || expected.property_address != load.property_address,
        "%s retired an ECG property read without its exact record/address association", name());
    if (managed()) {
        ecg_record::NativeRecordAccess access;
        fatal_if(!passConfigured || !passCursor.open() ||
            ecg_record::nativeRecordAccess(
                configuration, load.record_address, layout.record_bytes,
                access) != ecg_record::Status::OK ||
            passCursor.consume(access.index) != ecg_record::Status::OK ||
            load.sequence != passCursor.sequence(),
            "%s retired an out-of-order managed ECG property read", name());
    } else {
        fatal_if(!ecg_record::checkedAdd(lastSequence, 1, next) ||
            load.sequence != next,
            "%s retired a discontinuous legacy ECG property read", name());
    }
    pendingRecord.reset();
    lastSequence = load.sequence;
    ++propertyLoads;
    if (prefetcher)
        prefetcher->observeProperty(load);
    if (!applyUpdates)
        return;

    ecg_record::CommitUpdate update;
    update.physical_line = inst->physEffAddr - inst->physEffAddr % llc->getBlockSize();
    update.property_vaddr = inst->effAddr;
    update.sequence = load.sequence;
    update.deadline = load.deadline;
    update.generation = load.generation;
    update.context = load.context;
    update.state = load.state;
    if (managed() && propertyDescriptor.traversal ==
            ecg_record::TraversalMode::ORDERED_FILTERED) {
        fatal_if(!ecg_record::checkedAdd(eventOrder, 1, eventOrder),
            "%s ECG event order overflowed", name());
        update.order = eventOrder;
    }
    enqueueUpdate(update);
}

void
EcgRecordTransport::enqueueUpdate(const ecg_record::CommitUpdate& update)
{
    ++generated;
    const auto status = queue.enqueue(update, static_cast<uint64_t>(curCycle()));
    if (status == ecg_record::EnqueueStatus::ENQUEUED)
        ++enqueued;
    else if (status == ecg_record::EnqueueStatus::COALESCED)
        ++coalesced;
    else
        fatal("%s required ECG update rejected (status=%u, sequence=%llu, occupancy=%u)",
              name(), static_cast<unsigned>(status), update.sequence, queue.pendingSize());
    maxOccupancy = std::max<uint64_t>(maxOccupancy, queue.pendingSize());
    scheduleService();
}

void
EcgRecordTransport::passClose(const o3::DynInstPtr&)
{
    fatal_if(!configured || bindingInvalidated || !managed() ||
        pendingRecord || !passConfigured ||
        passCursor.close() != ecg_record::Status::OK,
        "%s rejected a managed ECG pass close", name());
    passConfigured = false;
    configuration.iteration_base = passCursor.base();
    const auto progress = llc->supportsEcgRecord()
        ? llc->advanceEcgRecordProgress(
            static_cast<uint16_t>(configuration.context),
            configuration.generation, passCursor.base())
        : ecg_record::ObservationResult::UNSUPPORTED;
    fatal_if(llc->supportsEcgRecord() &&
        propertyDescriptor.traversal ==
            ecg_record::TraversalMode::ORDERED_FILTERED &&
        progress != ecg_record::ObservationResult::ACCEPTED &&
        progress != ecg_record::ObservationResult::IGNORED_OLD,
        "%s LLC rejected managed ECG pass progress", name());
    ++passes;
    fatal_if(!ecg_record::checkedAdd(
        structuralPositions, configuration.record_count,
        structuralPositions),
        "%s managed ECG structural accounting overflowed", name());
    consumed = propertyLoads;
    skipped = structuralPositions - consumed;
}

void
EcgRecordTransport::ordinaryAccess(const o3::DynInstPtr& inst)
{
    if (!configured || bindingInvalidated || !managed() ||
        propertyDescriptor.traversal !=
            ecg_record::TraversalMode::ORDERED_FILTERED ||
        !passConfigured || !passCursor.open() || !inst || !inst->isMemRef() ||
        !inst->effAddrValid() || !inst->translationCompleted() ||
        (inst->memReqFlags & Request::SECURE) != 0 ||
        !ecg_record::nativePropertyContains(
            configuration, inst->effAddr, inst->effSize))
        return;
    ++ordinaryInvalidations;
    if (!applyUpdates)
        return;
    ecg_record::CommitUpdate update;
    update.physical_line =
        inst->physEffAddr - inst->physEffAddr % llc->getBlockSize();
    update.property_vaddr = inst->effAddr;
    update.sequence = passCursor.sequence();
    update.generation = configuration.generation;
    update.context = static_cast<uint16_t>(configuration.context);
    update.state = ecg_record::State::UNKNOWN;
    fatal_if(!ecg_record::checkedAdd(eventOrder, 1, eventOrder),
        "%s ECG invalidation order overflowed", name());
    update.order = eventOrder;
    update.invalidate = true;
    enqueueUpdate(update);
}

void
EcgRecordTransport::invalidateBinding(const o3::DynInstPtr&)
{
    fatal_if(!configured || bindingInvalidated || !managed() ||
        passCursor.open() || pendingRecord || queue.pendingSize() ||
        (prefetcher && prefetcher->pendingWork()) ||
        invalidationEvent.scheduled(),
        "%s rejected an undrained ECG binding invalidation", name());
    invalidationSetCount = llc->supportsEcgRecord()
        ? llc->ecgRecordSetCount() : 0;
    invalidationSet = 0;
    if (prefetcher)
        prefetcher->invalidateBinding();
    if (invalidationSetCount == 0) {
        bindingInvalidated = true;
        passConfigured = false;
    } else {
        schedule(invalidationEvent, clockEdge(Cycles(1)));
    }
}

void
EcgRecordTransport::invalidateSet()
{
    fatal_if(invalidationSet >= invalidationSetCount,
        "%s ECG metadata set walk overflowed", name());
    const bool final = invalidationSet + 1 == invalidationSetCount;
    llc->invalidateEcgRecordMetadataSet(invalidationSet, final);
    ++invalidationSet;
    ++invalidationSets;
    ++invalidationCycles;
    if (final) {
        bindingInvalidated = true;
        passConfigured = false;
        if (drainState() == DrainState::Draining && queue.empty() &&
            !invalidationEvent.scheduled())
            signalDrainDone();
    } else {
        schedule(invalidationEvent, clockEdge(Cycles(1)));
    }
}

void
EcgRecordTransport::scheduleService()
{
    const auto ready = queue.nextReadyCycle();
    if (!ready)
        return;
    const uint64_t now = static_cast<uint64_t>(curCycle());
    const uint64_t delta = *ready > now ? *ready - now : 1;
    fatal_if(delta > (std::numeric_limits<Tick>::max() - clockEdge()) / clockPeriod(),
             "%s ECG service time overflows", name());
    const Tick when = clockEdge() + delta * clockPeriod();
    if (!serviceEvent.scheduled())
        schedule(serviceEvent, when);
    else if (when < serviceEvent.when())
        reschedule(serviceEvent, when);
}

void
EcgRecordTransport::service()
{
    const uint64_t now = static_cast<uint64_t>(curCycle());
    const auto result = queue.popReady(now);
    if (result.status == ecg_record::PopStatus::POPPED) {
        ++delivered;
        minLatency = std::min(minLatency, now - result.ready.generation_cycle);
        switch (llc->applyEcgRecordUpdate(result.ready.update)) {
          case ecg_record::ApplyResult::APPLIED: ++applied; break;
          case ecg_record::ApplyResult::STALE: ++stale; break;
          case ecg_record::ApplyResult::EXPIRED: ++expired; break;
          case ecg_record::ApplyResult::NOT_RESIDENT: ++absent; break;
          default: fatal("%s LLC rejected a required ECG update", name());
        }
    } else {
        fatal_if(result.status == ecg_record::PopStatus::INVALID_TIME,
                 "%s ECG link service violated cycle ordering", name());
    }
    scheduleService();
    if (drainState() == DrainState::Draining && queue.empty())
        signalDrainDone();
}

DrainState
EcgRecordTransport::drain()
{
    if (queue.empty() && !invalidationEvent.scheduled())
        return DrainState::Drained;
    scheduleService();
    return DrainState::Draining;
}

uint64_t
EcgRecordTransport::pendingWork() const
{
    return queue.pendingSize() + (pendingRecord ? 1 : 0) +
        (prefetcher ? prefetcher->pendingWork() : 0) +
        (invalidationEvent.scheduled()
            ? invalidationSetCount - invalidationSet : 0);
}

void
EcgRecordTransport::report() const
{
    const bool accounting = generated == enqueued + coalesced &&
        enqueued == delivered + queue.pendingSize() &&
        delivered == applied + stale + expired + absent &&
        recordLoads == propertyLoads && recordReadBytes == recordLoads * layout.record_bytes &&
        (!managed() || (consumed == propertyLoads &&
            consumed + skipped == structuralPositions));
    fatal_if(!configured || bindingInvalidated || !accounting || pendingWork() ||
        (managed() && passCursor.open()),
             "%s ECG work is incomplete or accounting failed", name());
    std::cout << "[ECG-RECORD-NATIVE ";
    ecg_record::writeLayoutFields(std::cout, layout);
    std::cout << " replacement=" << applyUpdates << " prefetch=" << bool(prefetcher)
              << " configurations=" << configurations
              << " record_loads=" << recordLoads << " record_read_bytes=" << recordReadBytes
              << " governed_loads=" << propertyLoads << " last_sequence=" << lastSequence
              << " traversal_mode=" << ecg_record::traversalName(propertyDescriptor.traversal)
              << " property_kind=" << ecg_record::propertyKindName(propertyDescriptor.kind)
              << " property_bytes=" << unsigned(ecg_record::propertyBytes(propertyDescriptor.kind))
              << " property_stride=" << propertyDescriptor.stride_bytes
              << " passes=" << passes
              << " structural_positions=" << structuralPositions
              << " consumed=" << consumed
              << " skipped=" << skipped
              << " ordinary_invalidations=" << ordinaryInvalidations
              << " rebinds=" << rebinds
              << " invalidation_sets=" << invalidationSets
              << " invalidation_cycles=" << invalidationCycles
              << " generated=" << generated << " accepted=" << enqueued + coalesced
              << " enqueued=" << enqueued << " coalesced=" << coalesced
              << " delivered=" << delivered << " applied=" << applied
              << " stale=" << stale << " expired=" << expired << " absent=" << absent
              << " pending=0 errors=0 required_update_drops=0"
              << " capacity=" << ecg_record::CommitQueue::kCapacity
              << " capture_width=" << queue.captureWidth() << " output_width=1"
              << " latency_cycles=" << queue.latencyCycles()
              << " min_latency=" << (delivered ? minLatency : 0)
              << " max_occupancy=" << maxOccupancy
              << " dead_miss_targets=" << llc->ecgRecordDeadMisses
              << " dedicated_metadata_link=1 dedicated_update_tag_port=1"
              << " normal_tag_contention=0 accounting=1]\n";
    if (prefetcher)
        prefetcher->report();
}

} // namespace gem5
