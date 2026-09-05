#include "mem/cache/replacement_policies/ecg_ref32_commit_transport.hh"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "arch/generic/isa.hh"
#include "base/logging.hh"
#include "cpu/base.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "mem/cache/base.hh"
#include "mem/request.hh"

namespace gem5
{

EcgRef32CommitTransport::EcgRef32CommitTransport(const Params &params)
    : ClockedObject(params),
      cpu(dynamic_cast<o3::CPU *>(params.cpu)),
      llc(params.llc),
      applyUpdates(params.apply_updates),
      allowDrops(params.allow_drops),
      requiredContext(static_cast<uint16_t>(params.required_context)),
      queue(static_cast<uint64_t>(params.latency), params.capture_width),
      serviceEvent(
          [this] { service(); }, name() + ".service",
          false, Event::Default_Pri)
{
    fatal_if(!cpu, "%s requires a DerivO3CPU", name());
    fatal_if(cpu->numContexts() != 1,
             "%s supports exactly one hardware thread", name());
    fatal_if(
        cpu->getContext(0)->getIsaPtr()->getIsaName() != "riscv",
        "%s supports only RISC-V", name());
    fatal_if(!llc, "%s requires an LLC", name());
    fatal_if(
        params.required_context == 0 ||
            params.required_context > std::numeric_limits<uint16_t>::max(),
        "%s requires a context in [1,65535]", name());
    fatal_if(!queue.validConfiguration(),
             "%s requires latency >= %llu cycles and capture width in [1,%u]",
             name(),
             static_cast<unsigned long long>(
                 ecg_ref32::CommitUpdateQueue::kMinimumLatency),
             static_cast<unsigned>(
                 ecg_ref32::CommitUpdateQueue::kMaximumCaptureWidth));
    fatal_if(
        applyUpdates && !llc->supportsEcgRef32(),
        "%s apply_updates requires an REF32-capable LLC", name());
}

void
EcgRef32CommitTransport::regProbeListeners()
{
    fatal_if(commitListener, "%s Commit listener registered twice", name());
    commitListener = std::make_unique<
        ProbeListenerArgFunc<o3::DynInstPtr>>(
        cpu->getProbeManager(), "Commit",
        [this](const o3::DynInstPtr &inst) { observeCommit(inst); });
}

uint64_t
EcgRef32CommitTransport::currentCycleValue() const
{
    return static_cast<uint64_t>(curCycle());
}

bool
EcgRef32CommitTransport::validContext(uint16_t context) const
{
    return context != 0 && context == requiredContext;
}

bool
EcgRef32CommitTransport::validFourByteLoad(
        const o3::DynInstPtr &inst) const
{
    return inst && inst->isLoad() && inst->effAddrValid() &&
        inst->translationCompleted() && inst->effSize == sizeof(uint32_t) &&
        (inst->effAddr & (sizeof(uint32_t) - 1)) == 0 &&
        (inst->physEffAddr & (sizeof(uint32_t) - 1)) == 0 &&
        (inst->memReqFlags & Request::SECURE) == 0;
}

void
EcgRef32CommitTransport::observeCommit(const o3::DynInstPtr &inst)
{
    if (!inst)
        return;

    switch (inst->ecgRef32Hint().kind) {
      case ecg_ref32::InstructionKind::NONE:
        return;
      case ecg_ref32::InstructionKind::RECORD:
        observeRecord(inst);
        return;
      case ecg_ref32::InstructionKind::PROPERTY:
        observeProperty(inst);
        return;
    }
    protocolError("unknown REF32 instruction kind", false);
}

void
EcgRef32CommitTransport::observeRecord(const o3::DynInstPtr &inst)
{
    const auto &hint = inst->ecgRef32Hint();
    if (!validFourByteLoad(inst)) {
        protocolError(
            "RECORD retirement is not an aligned translated nonsecure "
            "four-byte load", false);
        return;
    }
    if (!validContext(hint.context)) {
        protocolError("RECORD retirement has an invalid context", false);
        return;
    }
    if (hint.state > 1) {
        protocolError(
            "RECORD retirement has an invalid has-next-iteration flag",
            false);
        return;
    }
    if (pendingRecord.valid) {
        protocolError(
            "RECORD retirement arrived before its predecessor PROPERTY",
            false);
        return;
    }

    ++recordLoads;
    recordBytes += sizeof(uint32_t);
    pendingRecord.destination = hint.destination;
    pendingRecord.sequence = hint.sequence;
    pendingRecord.context = hint.context;
    pendingRecord.valid = true;
}

void
EcgRef32CommitTransport::observeProperty(const o3::DynInstPtr &inst)
{
    const auto &hint = inst->ecgRef32Hint();
    if (!validFourByteLoad(inst)) {
        protocolError(
            "PROPERTY retirement is not an aligned translated nonsecure "
            "four-byte load", false);
        return;
    }
    if (!validContext(hint.context)) {
        protocolError("PROPERTY retirement has an invalid context", false);
        return;
    }
    if (hint.state > static_cast<uint8_t>(ecg_ref32::State::DEAD)) {
        protocolError("PROPERTY retirement has a non-normalized state", false);
        return;
    }
    if (!pendingRecord.valid) {
        protocolError(
            "PROPERTY retirement has no preceding RECORD retirement",
            false);
        return;
    }
    if (pendingRecord.destination != hint.destination ||
        pendingRecord.sequence != hint.sequence ||
        pendingRecord.context != hint.context) {
        protocolError(
            "PROPERTY retirement does not match its pending RECORD",
            false);
        return;
    }

    const uint32_t expected =
        haveLastSequence ? lastSequence + 1u : 1u;
    if (hint.sequence != expected) {
        protocolError(
            "PROPERTY semantic sequence is not globally contiguous",
            false);
        return;
    }

    pendingRecord.valid = false;
    haveLastSequence = true;
    lastSequence = hint.sequence;
    ++governedLoads;

    const uint64_t cycle = currentCycleValue();
    if (retirementCycle != cycle) {
        retirementCycle = cycle;
        retirementBurst = 0;
    }
    ++retirementBurst;
    maxRetirementBurst = std::max(maxRetirementBurst, retirementBurst);

    if (!applyUpdates)
        return;

    ++generated;
    if (degraded) {
        ++degradedDrops;
        return;
    }

    const uint64_t block_size = llc->getBlockSize();
    if (block_size == 0) {
        protocolError("LLC reports a zero block size", true);
        return;
    }

    ecg_ref32::CommitUpdate update;
    update.physical_line =
        inst->physEffAddr - (inst->physEffAddr % block_size);
    update.property_vaddr = inst->effAddr;
    update.sequence = hint.sequence;
    update.deadline = hint.value;
    update.context = hint.context;
    update.state = static_cast<ecg_ref32::State>(hint.state);
    update.secure = false;

    handleEnqueue(queue.enqueue(update, cycle), update);
}

void
EcgRef32CommitTransport::handleEnqueue(
        ecg_ref32::EnqueueStatus status,
        const ecg_ref32::CommitUpdate &update)
{
    switch (status) {
      case ecg_ref32::EnqueueStatus::ENQUEUED:
        ++accepted;
        ++enqueued;
        maxOccupancy = std::max<uint64_t>(
            maxOccupancy, queue.pendingSize());
        scheduleService();
        return;
      case ecg_ref32::EnqueueStatus::COALESCED:
        ++accepted;
        ++coalesced;
        scheduleService();
        return;
      case ecg_ref32::EnqueueStatus::FULL:
        ++fullDrops;
        {
            std::ostringstream message;
            message << "commit-update queue is full for seq="
                    << update.sequence << " line=0x" << std::hex
                    << update.physical_line;
            enterDegraded(message.str());
        }
        return;
      case ecg_ref32::EnqueueStatus::BUSY:
        ++ingressDrops;
        {
            std::ostringstream message;
            message << "commit-update ingress is busy for seq="
                    << update.sequence << " line=0x" << std::hex
                    << update.physical_line;
            enterDegraded(message.str());
        }
        return;
      case ecg_ref32::EnqueueStatus::INVALID_INPUT:
        ++invalidInputErrors;
        break;
      case ecg_ref32::EnqueueStatus::INVALID_ORDER:
        ++invalidOrderErrors;
        break;
      case ecg_ref32::EnqueueStatus::INVALID_TIME:
        ++invalidTimeErrors;
        break;
      case ecg_ref32::EnqueueStatus::INVALID_CONFIGURATION:
        ++invalidConfigErrors;
        break;
    }

    std::ostringstream message;
    message << "commit-update queue rejected seq=" << update.sequence
            << " line=0x" << std::hex << update.physical_line;
    protocolError(message.str(), true);
}

void
EcgRef32CommitTransport::scheduleService()
{
    const auto next_ready = queue.nextReadyCycle();
    if (!next_ready)
        return;

    const uint64_t now = currentCycleValue();
    uint64_t target = std::max(*next_ready, now);
    if (target <= now) {
        if (now == std::numeric_limits<uint64_t>::max()) {
            ++invalidTimeErrors;
            protocolError("service-cycle reschedule overflow", false);
            return;
        }
        target = now + 1;
    }
    const uint64_t delta = target - now;
    const Tick edge = clockEdge();
    if (delta >
        (std::numeric_limits<Tick>::max() - edge) / clockPeriod()) {
        ++invalidTimeErrors;
        protocolError("service tick conversion overflow", false);
        return;
    }
    const Tick when = edge + delta * clockPeriod();
    if (!serviceEvent.scheduled()) {
        schedule(serviceEvent, when);
    } else if (when < serviceEvent.when()) {
        reschedule(serviceEvent, when);
    }
}

void
EcgRef32CommitTransport::service()
{
    const uint64_t now = currentCycleValue();
    const ecg_ref32::PopResult result = queue.popReady(now);
    switch (result.status) {
      case ecg_ref32::PopStatus::POPPED: {
        ++delivered;
        const uint64_t latency =
            now - result.ready.generation_cycle;
        minimumLatency = std::min(minimumLatency, latency);
        handleDelivery(llc->applyEcgRef32Update(result.ready.update));
        scheduleService();
        break;
      }
      case ecg_ref32::PopStatus::EMPTY:
        break;
      case ecg_ref32::PopStatus::NOT_READY:
        scheduleService();
        break;
      case ecg_ref32::PopStatus::BUSY:
        scheduleService();
        break;
      case ecg_ref32::PopStatus::INVALID_TIME:
        ++invalidTimeErrors;
        protocolError("commit-update queue rejected service time", false);
        break;
    }

    if (drainState() == DrainState::Draining && queue.empty())
        signalDrainDone();
}

void
EcgRef32CommitTransport::handleDelivery(
        ecg_ref32::CommitApplyResult result)
{
    switch (result) {
      case ecg_ref32::CommitApplyResult::APPLIED:
        ++applied;
        return;
      case ecg_ref32::CommitApplyResult::STALE:
        ++stale;
        return;
      case ecg_ref32::CommitApplyResult::EXPIRED:
        ++expired;
        return;
      case ecg_ref32::CommitApplyResult::NOT_RESIDENT:
        ++notResident;
        return;
      case ecg_ref32::CommitApplyResult::INVALID_CONTEXT:
      case ecg_ref32::CommitApplyResult::INVALID_ADDRESS:
      case ecg_ref32::CommitApplyResult::INVALID_ORDER:
      case ecg_ref32::CommitApplyResult::UNSUPPORTED:
        ++invalidDelivery;
        protocolError("LLC rejected a delivered REF32 update", false);
        return;
    }
    ++invalidDelivery;
    protocolError("LLC returned an unknown REF32 apply result", false);
}

void
EcgRef32CommitTransport::protocolError(
        const std::string &reason, bool generated_update)
{
    ++protocolErrors;
    if (!allowDrops)
        fatal("%s protocol error: %s", name(), reason);
    if (generated_update)
        ++degradedDrops;
    enterDegraded(reason);
}

void
EcgRef32CommitTransport::enterDegraded(const std::string &reason)
{
    if (!allowDrops)
        fatal("%s dropped REF32 metadata: %s", name(), reason);
    if (degraded)
        return;

    degraded = true;
    pendingRecord.valid = false;
    cancelled += queue.clear();
    if (serviceEvent.scheduled())
        deschedule(serviceEvent);
    if (applyUpdates)
        llc->disableEcgRef32();
    warn("%s entered diagnostic degraded mode: %s", name(), reason);
    if (drainState() == DrainState::Draining)
        signalDrainDone();
}

DrainState
EcgRef32CommitTransport::drain()
{
    if (queue.empty())
        return DrainState::Drained;
    scheduleService();
    return DrainState::Draining;
}

Tick
EcgRef32CommitTransport::drainBudgetTicks() const
{
    constexpr uint64_t service_cycles =
        ecg_ref32::CommitUpdateQueue::kCapacity + 1;
    fatal_if(queue.latencyCycles() >
                 std::numeric_limits<uint64_t>::max() - service_cycles,
             "%s drain cycle budget overflows", name());
    const uint64_t cycles = queue.latencyCycles() + service_cycles;
    fatal_if(cycles > std::numeric_limits<Tick>::max() / clockPeriod(),
             "%s drain tick budget overflows", name());
    return cycles * clockPeriod();
}

void
EcgRef32CommitTransport::report() const
{
    const uint64_t pending = queue.pendingSize();
    const uint64_t min_latency =
        minimumLatency == std::numeric_limits<uint64_t>::max()
        ? 0 : minimumLatency;
    const bool generated_ok =
        generated ==
            accepted + fullDrops + ingressDrops + degradedDrops;
    const bool accepted_ok = accepted == enqueued + coalesced;
    const bool delivered_ok =
        delivered ==
            applied + stale + expired + notResident + invalidDelivery;
    const bool enqueued_ok =
        enqueued == delivered + cancelled + pending;

    std::cout
        << "[ECG-REF32-NATIVE"
        << " mode=" << (applyUpdates ? "apply" : "validate")
        << " context=" << requiredContext
        << " recordLoads=" << recordLoads
        << " recordBytes=" << recordBytes
        << " governedLoads=" << governedLoads
        << " generated=" << generated
        << " accepted=" << accepted
        << " enqueued=" << enqueued
        << " coalesced=" << coalesced
        << " delivered=" << delivered
        << " applied=" << applied
        << " stale=" << stale
        << " expired=" << expired
        << " notResident=" << notResident
        << " invalidDelivery=" << invalidDelivery
        << " cancelled=" << cancelled
        << " pending=" << pending
        << " fullDrops=" << fullDrops
        << " ingressDrops=" << ingressDrops
        << " degradedDrops=" << degradedDrops
        << " invalidInputErrors=" << invalidInputErrors
        << " invalidOrderErrors=" << invalidOrderErrors
        << " invalidTimeErrors=" << invalidTimeErrors
        << " invalidConfigErrors=" << invalidConfigErrors
        << " errors=" << protocolErrors
        << " maxRetirementBurst=" << maxRetirementBurst
        << " maxOccupancy=" << maxOccupancy
        << " minLatency=" << min_latency
        << " latencyCycles=" << queue.latencyCycles()
        << " capacity=" << ecg_ref32::CommitUpdateQueue::kCapacity
        << " captureWidth=" << queue.captureWidth()
        << " captureOrderBits="
        << ecg_ref32::CommitUpdateQueue::kCapacity * queue.captureLaneBits()
        << " outputWidth=1"
        << " dedicated_link=1"
        << " dedicated_tag_port=1"
        << " normal_tag_contention=0"
        << " data_network_contention=0"
        << " degraded=" << (degraded ? 1 : 0)
        << " accounting="
        << (generated_ok && accepted_ok && delivered_ok && enqueued_ok
            ? 1 : 0)
        << "]\n";
}

} // namespace gem5
