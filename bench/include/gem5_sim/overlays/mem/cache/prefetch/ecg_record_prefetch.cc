#include "mem/cache/prefetch/ecg_record_prefetch.hh"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

#include "base/logging.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "mem/cache/base.hh"
#include "sim/system.hh"

namespace gem5
{

EcgRecordPrefetch::EcgRecordPrefetch(const Params& params)
    : ClockedObject(params), cpu(params.cpu), l1(params.l1), l2(params.l2), llc(params.llc),
      windowQueueSize(params.window_queue_size), propertyQueueSize(params.property_queue_size),
      lookupLatency(static_cast<uint64_t>(params.lookup_latency)),
      prefetchLatency(static_cast<uint64_t>(params.prefetch_latency)),
      progressLimit(params.progress_limit),
      recordRequestor(cpu->system->getRequestorId(this, "records")),
      propertyRequestor(cpu->system->getRequestorId(this, "properties")),
      recordPort(name() + ".record_port", *this, true),
      propertyPort(name() + ".property_port", *this, false),
      serviceEvent([this] { service(); }, name() + ".service")
{
    fatal_if(!l1 || !l2 || !llc || cpu->numContexts() != 1 ||
        l1->getBlockSize() != kLineBytes || l2->getBlockSize() != kLineBytes ||
        llc->getBlockSize() != kLineBytes || !llc->supportsEcgRecord() ||
        windowQueueSize == 0 || windowQueueSize > 16 ||
        propertyQueueSize == 0 || propertyQueueSize > kPropertySlots ||
        lookupLatency == 0 || prefetchLatency == 0 ||
        progressLimit <= lookupLatency + prefetchLatency,
        "%s has invalid ECG prefetch geometry or latency", name());
    for (unsigned slot = 0; slot < kAccesses; ++slot)
        accesses[slot] = std::make_unique<Access>(*this, slot);
}

Port&
EcgRecordPrefetch::getPort(const std::string& port_name, PortID id)
{
    if (port_name == "record_port")
        return recordPort;
    if (port_name == "property_port")
        return propertyPort;
    return ClockedObject::getPort(port_name, id);
}

void
EcgRecordPrefetch::regProbeListeners()
{
    fatal_if(fillListener, "%s fill observer registered twice", name());
    fillListener = std::make_unique<ProbeListenerArgFunc<CacheAccessProbeArg>>(
        l1->getProbeManager(), "Fill",
        [this](const CacheAccessProbeArg& access) { observeFill(access); });
    dataListener = std::make_unique<ProbeListenerArgFunc<CacheDataUpdateProbeArg>>(
        l1->getProbeManager(), "Data Update",
        [this](const CacheDataUpdateProbeArg& update) { observeDataUpdate(update); });
}

void
EcgRecordPrefetch::configure(const ecg_record::NativeConfiguration& value)
{
    ecg_record::Layout resolved;
    fatal_if(ecg_record::validateNativeConfiguration(value, resolved) != ecg_record::Status::OK,
             "%s received an invalid ECG prefetch configuration", name());
    fatal_if(configured &&
        (value.record_base != configuration.record_base ||
         value.property_base != configuration.property_base ||
         value.generation != configuration.generation ||
         value.context != configuration.context ||
         value.layout_descriptor != configuration.layout_descriptor ||
         value.record_count != configuration.record_count ||
         value.vertex_count != configuration.vertex_count),
        "%s graph identity changed without draining its prefetch engine", name());
    layout = resolved;
    configuration = value;
    fatal_if(!configured && buffer.configure(layout, value.record_base, value.record_count) !=
                 ecg_record::Status::OK,
             "%s cannot store its complete unaligned record window", name());
    bankCount = buffer.bankCount();
    configured = true;
    progress();
}

void
EcgRecordPrefetch::observeProperty(const ecg_record::NativeLoadResult& load)
{
    fatal_if(!configured || load.generation != configuration.generation ||
        load.context != configuration.context ||
        load.layout_descriptor != configuration.layout_descriptor || load.sequence <= lastSequence,
        "%s received an invalid retired ECG prefetch trigger", name());
    lastSequence = load.sequence;
    ++triggers;
    const uint64_t index = (load.record_address - configuration.record_base) / layout.record_bytes;
    fatal_if(load.record_address < configuration.record_base ||
        load.record_address % layout.record_bytes || index >= configuration.record_count,
        "%s prefetch trigger is outside its real record stream", name());
    if (configuration.record_count - index <= 8)
        ++tailSkipped;
    else if (windows.size() == windowQueueSize)
        ++windowDropped;
    else
        windows.push_back(load);
    progress();
    kick();
}

void
EcgRecordPrefetch::progress()
{
    lastProgressCycle = static_cast<uint64_t>(curCycle());
}

void
EcgRecordPrefetch::kick()
{
    maxPending = std::max(maxPending, pendingWork());
    if (pendingWork() && !serviceEvent.scheduled())
        schedule(serviceEvent, clockEdge(Cycles(1)));
}

bool
EcgRecordPrefetch::storeBank(
    uint64_t virtual_line, uint64_t physical_line, const uint8_t* data, Tick ready)
{
    const auto status = buffer.storeLine(
        virtual_line, physical_line, data, std::max(ready, clockEdge(Cycles(1))));
    fatal_if(status != ecg_record::Status::OK && status != ecg_record::Status::NOT_READY,
             "%s received an invalid record-line capture", name());
    return status == ecg_record::Status::OK;
}

void
EcgRecordPrefetch::observeFill(const CacheAccessProbeArg& access)
{
    const PacketPtr packet = access.pkt;
    if (!configured || !packet || !packet->req || !packet->req->hasVaddr() ||
        packet->isSecure() || !packet->hasData() || packet->getSize() != kLineBytes)
        return;
    const uint64_t address = packet->req->getVaddr();
    const uint64_t bytes = configuration.record_count * layout.record_bytes;
    if (address < configuration.record_base || address - configuration.record_base >= bytes)
        return;
    if (storeBank(address - address % kLineBytes, packet->getBlockAddr(kLineBytes),
                  packet->getConstPtr<uint8_t>(), l1->ecgRecordFillReady(packet))) {
        ++fillObservations;
        progress();
        kick();
    }
}

void
EcgRecordPrefetch::observeDataUpdate(const CacheDataUpdateProbeArg& update)
{
    if (!configured || update.isSecure || update.oldData.empty() ||
        update.newData.size() * sizeof(uint64_t) != kLineBytes)
        return;
    const auto status = buffer.updatePhysical(update.addr,
        reinterpret_cast<const uint8_t*>(update.newData.data()), clockEdge(Cycles(1)));
    fatal_if(status != ecg_record::Status::OK && status != ecg_record::Status::NOT_READY,
             "%s received a malformed record data update", name());
    if (status == ecg_record::Status::OK) {
        ++dataUpdateObservations;
        progress();
        kick();
    }
}

void
EcgRecordPrefetch::beginAccess(unsigned slot, uint64_t virtual_address)
{
    auto& access = *accesses[slot];
    fatal_if(access.phase != Phase::FREE || ticket == UINT64_MAX,
             "%s has no free ECG memory transaction identity", name());
    access.phase = Phase::TRANSLATING;
    access.ticket = ++ticket;
    access.virtualAddress = virtual_address;
    const bool records = slot < kBanks;
    auto* context = cpu->getContext(0);
    access.request = std::make_shared<Request>(
        virtual_address, kLineBytes, records ? 0 : Request::PREFETCH,
        records ? recordRequestor : propertyRequestor, 0, context->contextId());
    if (!records) {
        access.decision = std::make_shared<EcgRecordFillDecision>();
        access.decision->owner = this;
        access.decision->slot = slot;
        access.decision->ticket = access.ticket;
        access.request->setExtension(
            std::make_shared<EcgRecordPrefetchExtension>(access.decision));
    }
    ++translationRequests;
    context->getMMUPtr()->translateTiming(access.request, context, &access, BaseMMU::Read);
    progress();
}

void
EcgRecordPrefetch::Access::finish(
    const Fault& fault, const RequestPtr& request, ThreadContext*, BaseMMU::Mode)
{
    owner.translated(slot, fault, request);
}

uint64_t
EcgRecordPrefetch::reserveLookup()
{
    const uint64_t now = static_cast<uint64_t>(curCycle());
    const uint64_t start = std::max(now, nextLookupCycle);
    uint64_t finish = 0;
    fatal_if(!ecg_record::checkedAdd(start, lookupLatency, finish) ||
        !ecg_record::checkedAdd(start, 1, nextLookupCycle),
        "%s dedicated lookup pipeline time overflows", name());
    return finish;
}

void
EcgRecordPrefetch::translated(
    unsigned slot, const Fault& fault, const RequestPtr& request)
{
    auto& access = *accesses[slot];
    fatal_if(access.phase != Phase::TRANSLATING || access.request != request,
             "%s translation completed for the wrong ECG transaction", name());
    if (fault != NoFault) {
        ++translationFailures;
        fatal_if(slot < kBanks, "%s could not translate an in-range real record line", name());
        release(slot);
        return;
    }
    access.physicalAddress = request->getPaddr();
    fatal_if(access.physicalAddress % kLineBytes,
             "%s record translation returned a misaligned line", name());
    if (slot < kBanks) {
        access.phase = Phase::READY;
    } else {
        access.phase = Phase::LOOKUP;
        fatal_if(!ecg_record::checkedAdd(reserveLookup(), prefetchLatency, access.readyCycle),
                 "%s prefetch pipeline time overflows", name());
    }
    progress();
    kick();
}

void
EcgRecordPrefetch::processWindow()
{
    if (windows.empty())
        return;
    const auto& trigger = windows.front();
    fatal_if(buffer.pin(trigger.record_address) != ecg_record::Status::OK,
             "%s could not pin its real record window", name());
    for (uint64_t line = buffer.firstLine(); ; line += kLineBytes) {
        if (!buffer.hasLine(line)) {
            bool pending = false;
            unsigned available = kBanks;
            for (unsigned slot = 0; slot < bankCount; ++slot) {
                const auto& access = *accesses[slot];
                pending |= access.phase != Phase::FREE && access.virtualAddress == line;
                if (access.phase == Phase::FREE)
                    available = slot;
            }
            if (!pending && available < kBanks)
                beginAccess(available, line);
        }
        if (line == buffer.lastLine())
            break;
    }
    ecg_record::RecordWindow window;
    const auto status = buffer.readWindow(
        trigger.record_address, configuration.vertex_count, curTick(), window);
    if (status == ecg_record::Status::NOT_READY)
        return;
    fatal_if(status != ecg_record::Status::OK, "%s has an invalid record window", name());
    fatal_if(window.words[0] != trigger.raw_record,
             "%s acquired record bytes disagree with the actual retired load", name());
    ecg_record::PrefetchTarget target;
    fatal_if(ecg_record::selectWindowTarget(layout, window, 16, target) != ecg_record::Status::OK,
             "%s could not decode its complete real record window", name());
    windows.pop_front();
    buffer.unpin();
    ++selectedWindows;
    progress();
    if (!target.valid) {
        ++emptyWindows;
        return;
    }
    ++candidates;
    const uint64_t address = configuration.property_base + target.destination * 4;
    const uint64_t line = address - address % kLineBytes;
    unsigned available = kAccesses;
    for (unsigned slot = kBanks; slot < kBanks + propertyQueueSize; ++slot) {
        const auto& access = *accesses[slot];
        if (access.phase != Phase::FREE && access.virtualAddress == line) {
            ++pendingDuplicates;
            return;
        }
        if (access.phase == Phase::FREE)
            available = slot;
    }
    if (available == kAccesses) {
        ++propertyQueueDropped;
        return;
    }
    beginAccess(available, line);
}

void
EcgRecordPrefetch::issueLookup(Access& access)
{
    const Addr address = access.physicalAddress;
    const bool in_l1 = l1->inCache(address, false) || l1->inMissQueue(address, false);
    const bool in_l2 = l2->inCache(address, false) || l2->inMissQueue(address, false);
    const bool in_llc = llc->inCache(address, false) || llc->inMissQueue(address, false);
    presenceLookups += 3;
    ++admissionLookups;
    const bool admit = llc->canAdmitEcgRecordPrefetch(address, lastSequence);
    if (in_l1 || in_l2 || in_llc) {
        ++issueResident;
        release(access.slot);
    } else if (!admit) {
        ++issueAdmission;
        release(access.slot);
    } else {
        access.phase = Phase::READY;
    }
    progress();
}

void
EcgRecordPrefetch::deferFill(
    BaseCache* cache, PacketPtr packet,
    const std::shared_ptr<EcgRecordFillDecision>& decision)
{
    fatal_if(cache != llc || !decision || decision->slot >= kAccesses ||
        decision->slot < kBanks, "%s received an unrelated prefetch fill", name());
    auto& access = *accesses[decision->slot];
    fatal_if(access.phase != Phase::INFLIGHT || access.ticket != decision->ticket ||
        access.decision != decision || access.heldResponse,
        "%s received a duplicate or mismatched prefetch response", name());
    access.heldResponse = packet;
    access.phase = Phase::FILL_LOOKUP;
    access.readyCycle = reserveLookup();
    progress();
    kick();
}

void
EcgRecordPrefetch::fillLookup(Access& access)
{
    const Addr address = access.physicalAddress;
    const bool in_l1 = l1->inCache(address, false);
    const bool in_l2 = l2->inCache(address, false);
    const bool in_llc = llc->inCache(address, false);
    presenceLookups += 3;
    ++admissionLookups;
    const bool admit = llc->canAdmitEcgRecordPrefetch(address, lastSequence);
    access.decision->admitted = !in_l1 && !in_l2 && !in_llc && admit;
    access.decision->ready = true;
    if (in_l1 || in_l2 || in_llc)
        ++fillResident;
    else if (!admit)
        ++fillAdmission;
    auto* packet = access.heldResponse;
    access.heldResponse = nullptr;
    access.phase = Phase::INFLIGHT;
    llc->resumeEcgRecordResponse(packet);
    progress();
}

void
EcgRecordPrefetch::send(MemoryPort& port)
{
    if (port.blocked)
        return;
    const unsigned first = port.records ? 0 : kBanks;
    const unsigned end = port.records ? bankCount : kBanks + propertyQueueSize;
    for (unsigned slot = first; slot < end; ++slot) {
        auto& access = *accesses[slot];
        if (access.phase != Phase::READY)
            continue;
        if (!access.unsent) {
            // An auxiliary master uses acknowledged reads, not cache-owned HardPFReqs.
            access.unsent = new Packet(access.request, MemCmd::ReadReq);
            access.unsent->allocate();
            access.unsent->pushSenderState(new AccessState(slot, access.ticket));
        }
        if (!port.sendTimingReq(access.unsent)) {
            port.blocked = true;
            ++requestRetries;
            return;
        }
        access.unsent = nullptr;
        access.phase = Phase::INFLIGHT;
        if (port.records)
            ++recordReads;
        else
            ++propertyReads;
        progress();
        return;
    }
}

bool
EcgRecordPrefetch::MemoryPort::recvTimingResp(PacketPtr packet)
{
    owner.received(packet, records);
    return true;
}

void
EcgRecordPrefetch::MemoryPort::recvReqRetry()
{
    fatal_if(!blocked, "%s received an unsolicited request retry", name());
    blocked = false;
    owner.kick();
}

void
EcgRecordPrefetch::received(PacketPtr packet, bool records)
{
    auto* state = dynamic_cast<AccessState*>(packet->popSenderState());
    fatal_if(!state || state->slot >= kAccesses, "%s received an unbound response", name());
    const unsigned slot = state->slot;
    auto& access = *accesses[slot];
    fatal_if(access.phase != Phase::INFLIGHT || access.ticket != state->ticket ||
        records != (slot < kBanks) || packet->isError() ||
        !packet->hasData() || packet->getSize() != kLineBytes,
        "%s received an invalid real-byte ECG memory response", name());
    if (records) {
        ++recordResponses;
        storeBank(access.virtualAddress, access.physicalAddress, packet->getConstPtr<uint8_t>(),
                  curTick() + packet->headerDelay + packet->payloadDelay);
    } else {
        ++propertyResponses;
        if (access.decision->cacheFill) {
            if (access.decision->allocated) {
                if (access.decision->mergedDemand)
                    ++mergedFills;
                else
                    ++fills;
            }
        } else {
            ++hitResponses;
        }
    }
    delete state;
    delete packet;
    release(slot);
}

void
EcgRecordPrefetch::release(unsigned slot)
{
    auto& access = *accesses[slot];
    fatal_if(access.unsent || access.heldResponse, "%s released owned memory packets", name());
    access.request.reset();
    access.decision.reset();
    access.phase = Phase::FREE;
    progress();
    kick();
}

void
EcgRecordPrefetch::service()
{
    const uint64_t now = static_cast<uint64_t>(curCycle());
    fatal_if(pendingWork() && now - lastProgressCycle > progressLimit,
             "%s ECG prefetch engine made no progress for %llu cycles (pending=%llu)",
             name(), progressLimit, pendingWork());
    for (auto& value : accesses) {
        auto& access = *value;
        if (access.readyCycle > now)
            continue;
        if (access.phase == Phase::LOOKUP)
            issueLookup(access);
        else if (access.phase == Phase::FILL_LOOKUP)
            fillLookup(access);
    }
    processWindow();
    send(recordPort);
    send(propertyPort);
    kick();
    if (!pendingWork() && drainState() == DrainState::Draining)
        signalDrainDone();
}

uint64_t
EcgRecordPrefetch::pendingWork() const
{
    uint64_t pending = windows.size();
    for (const auto& access : accesses)
        pending += access && access->phase != Phase::FREE;
    return pending;
}

DrainState
EcgRecordPrefetch::drain()
{
    if (!pendingWork())
        return DrainState::Drained;
    kick();
    return DrainState::Draining;
}

void
EcgRecordPrefetch::report() const
{
    fatal_if(pendingWork() || recordReads != recordResponses || propertyReads != propertyResponses ||
        triggers != tailSkipped + windowDropped + selectedWindows ||
        selectedWindows != candidates + emptyWindows,
        "%s ECG prefetch accounting or bounded completion failed", name());
    std::cout << "[ECG-RECORD-PREFETCH"
              << " triggers=" << triggers << " tail_skipped=" << tailSkipped
              << " window_queue_dropped=" << windowDropped
              << " selected_windows=" << selectedWindows << " empty_windows=" << emptyWindows
              << " candidates=" << candidates << " pending_duplicates=" << pendingDuplicates
              << " property_queue_dropped=" << propertyQueueDropped
              << " issue_resident=" << issueResident << " issue_admission=" << issueAdmission
              << " fill_resident=" << fillResident << " fill_admission=" << fillAdmission
              << " record_reads=" << recordReads << " record_responses=" << recordResponses
              << " record_acquisition_bytes=" << recordReads * kLineBytes
              << " property_reads=" << propertyReads << " property_responses=" << propertyResponses
              << " property_prefetch_bytes=" << propertyReads * kLineBytes
              << " fills=" << fills << " merged_demand_fills=" << mergedFills
              << " hit_or_merged_responses=" << hitResponses
              << " l1_fill_observations=" << fillObservations
              << " data_update_observations=" << dataUpdateObservations
              << " translation_requests=" << translationRequests
              << " translation_failures=" << translationFailures
              << " presence_lookups=" << presenceLookups
              << " admission_lookups=" << admissionLookups
              << " request_retries=" << requestRetries << " max_pending=" << maxPending
              << " record_banks=" << bankCount << " record_buffer_bytes=" << bankCount * kLineBytes
              << " window_queue_size=" << windowQueueSize
              << " property_queue_size=" << propertyQueueSize
              << " lookup_latency_cycles=" << lookupLatency
              << " prefetch_latency_cycles=" << prefetchLatency
              << " lookup_issue_width=1 dedicated_presence_ports=3"
              << " capture_latency_cycles=1 l1_fill_latency_charged=1"
              << " request_command=acknowledged-read"
              << " record_requestor=" << recordRequestor
              << " property_requestor=" << propertyRequestor
              << " allocation=llc-only pending=0 accounting=1]\n";
}

} // namespace gem5
