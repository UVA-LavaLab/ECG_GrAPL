#ifndef __MEM_CACHE_PREFETCH_ECG_RECORD_PREFETCH_HH__
#define __MEM_CACHE_PREFETCH_ECG_RECORD_PREFETCH_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <memory>

#include "arch/generic/mmu.hh"
#include "mem/cache/cache_probe_arg.hh"
#include "mem/cache/prefetch/ecg_record_prefetch_request.hh"
#include "mem/cache/replacement_policies/ecg_record_native.h"
#include "mem/cache/replacement_policies/ecg_record_window.h"
#include "mem/port.hh"
#include "params/EcgRecordPrefetch.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

class BaseCPU;
class BaseCache;

class EcgRecordPrefetch : public ClockedObject, public EcgRecordPrefetchFilter
{
  public:
    PARAMS(EcgRecordPrefetch);
    explicit EcgRecordPrefetch(const Params& params);
    Port& getPort(const std::string& name, PortID id=InvalidPortID) override;
    void regProbeListeners() override;
    DrainState drain() override;
    void configure(const ecg_record::NativeConfiguration& configuration);
    void invalidateBinding();
    void observeProperty(const ecg_record::NativeLoadResult& load);
    uint64_t pendingWork() const;
    void report() const;
    void deferFill(
        BaseCache* cache, PacketPtr packet,
        const std::shared_ptr<EcgRecordFillDecision>& decision) override;

  private:
    static constexpr unsigned kLineBytes = 64;
    static constexpr unsigned kBanks = 3;
    static constexpr unsigned kPropertySlots = 16;
    static constexpr unsigned kAccesses = kBanks + kPropertySlots;
    enum class Phase { FREE, TRANSLATING, LOOKUP, READY, INFLIGHT, FILL_LOOKUP };

    struct Access : public BaseMMU::Translation
    {
        EcgRecordPrefetch& owner;
        const unsigned slot;
        Phase phase = Phase::FREE;
        uint64_t ticket = 0;
        uint64_t readyCycle = 0;
        uint64_t virtualAddress = 0;
        uint64_t physicalAddress = 0;
        RequestPtr request;
        PacketPtr unsent = nullptr;
        PacketPtr heldResponse = nullptr;
        std::shared_ptr<EcgRecordFillDecision> decision;

        Access(EcgRecordPrefetch& owner, unsigned slot) : owner(owner), slot(slot) {}
        void markDelayed() override {}
        void finish(const Fault& fault, const RequestPtr& request,
                    ThreadContext* context, BaseMMU::Mode mode) override;
    };

    struct AccessState : public Packet::SenderState
    {
        unsigned slot;
        uint64_t ticket;
        AccessState(unsigned slot, uint64_t ticket) : slot(slot), ticket(ticket) {}
    };

    class MemoryPort : public RequestPort
    {
      public:
        EcgRecordPrefetch& owner;
        const bool records;
        bool blocked = false;
        MemoryPort(const std::string& name, EcgRecordPrefetch& owner, bool records)
            : RequestPort(name), owner(owner), records(records) {}
        bool recvTimingResp(PacketPtr packet) override;
        void recvReqRetry() override;
    };

    void service();
    void kick();
    void progress();
    void observeFill(const CacheAccessProbeArg& access);
    void observeDataUpdate(const CacheDataUpdateProbeArg& update);
    bool storeBank(uint64_t virtual_line, uint64_t physical_line,
                   const uint8_t* data, Tick ready);
    void processWindow();
    void beginAccess(unsigned slot, uint64_t virtual_address);
    void translated(unsigned slot, const Fault& fault, const RequestPtr& request);
    void received(PacketPtr packet, bool records);
    void release(unsigned slot);
    void issueLookup(Access& access);
    void fillLookup(Access& access);
    void send(MemoryPort& port);
    uint64_t reserveLookup();

    BaseCPU* const cpu;
    BaseCache* const l1;
    BaseCache* const l2;
    BaseCache* const llc;
    const unsigned windowQueueSize;
    const unsigned propertyQueueSize;
    const uint64_t lookupLatency;
    const uint64_t prefetchLatency;
    const uint64_t progressLimit;
    const RequestorID recordRequestor;
    const RequestorID propertyRequestor;
    MemoryPort recordPort;
    MemoryPort propertyPort;
    EventFunctionWrapper serviceEvent;
    std::unique_ptr<ProbeListenerArgFunc<CacheAccessProbeArg>> fillListener;
    std::unique_ptr<ProbeListenerArgFunc<CacheDataUpdateProbeArg>> dataListener;
    std::array<std::unique_ptr<Access>, kAccesses> accesses;
    ecg_record::WindowBuffer buffer;
    std::deque<ecg_record::NativeLoadResult> windows;
    ecg_record::NativeConfiguration configuration;
    ecg_record::Layout layout;
    ecg_record::PropertyDescriptor propertyDescriptor;
    unsigned bankCount = 0;
    uint64_t ticket = 0;
    uint64_t lastSequence = 0;
    uint64_t nextLookupCycle = 0;
    uint64_t lastProgressCycle = 0;
    bool configured = false;

    uint64_t triggers = 0;
    uint64_t tailSkipped = 0;
    uint64_t windowDropped = 0;
    uint64_t selectedWindows = 0;
    uint64_t emptyWindows = 0;
    uint64_t candidates = 0;
    uint64_t pendingDuplicates = 0;
    uint64_t propertyQueueDropped = 0;
    uint64_t issueResident = 0;
    uint64_t issueAdmission = 0;
    uint64_t fillResident = 0;
    uint64_t fillAdmission = 0;
    uint64_t recordReads = 0;
    uint64_t recordResponses = 0;
    uint64_t propertyReads = 0;
    uint64_t propertyResponses = 0;
    uint64_t fills = 0;
    uint64_t mergedFills = 0;
    uint64_t hitResponses = 0;
    uint64_t fillObservations = 0;
    uint64_t dataUpdateObservations = 0;
    uint64_t translationRequests = 0;
    uint64_t translationFailures = 0;
    uint64_t presenceLookups = 0;
    uint64_t admissionLookups = 0;
    uint64_t requestRetries = 0;
    uint64_t maxPending = 0;
};

} // namespace gem5

#endif
