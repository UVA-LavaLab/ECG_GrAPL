#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_COMMIT_TRANSPORT_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_COMMIT_TRANSPORT_HH__

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "cpu/o3/dyn_inst_ptr.hh"
#include "mem/cache/replacement_policies/ecg_ref32_commit.h"
#include "params/EcgRef32CommitTransport.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

class BaseCache;
class BaseCPU;

namespace o3
{
class CPU;
}

class EcgRef32CommitTransport : public ClockedObject
{
  public:
    PARAMS(EcgRef32CommitTransport);

    explicit EcgRef32CommitTransport(const Params &params);

    void regProbeListeners() override;
    DrainState drain() override;
    void report() const;
    uint64_t pendingUpdates() const { return queue.pendingSize(); }
    Tick drainBudgetTicks() const;

  private:
    struct PendingRecord
    {
        uint32_t destination = 0;
        uint32_t sequence = 0;
        uint16_t context = 0;
        bool valid = false;
    };

    void observeCommit(const o3::DynInstPtr &inst);
    void observeRecord(const o3::DynInstPtr &inst);
    void observeProperty(const o3::DynInstPtr &inst);
    void service();
    void scheduleService();
    void enterDegraded(const std::string &reason);
    void protocolError(const std::string &reason, bool generated_update);
    void handleEnqueue(
        ecg_ref32::EnqueueStatus status, const ecg_ref32::CommitUpdate &update);
    void handleDelivery(ecg_ref32::CommitApplyResult result);
    bool validFourByteLoad(const o3::DynInstPtr &inst) const;
    bool validContext(uint16_t context) const;
    uint64_t currentCycleValue() const;

    o3::CPU *const cpu;
    BaseCache *const llc;
    const bool applyUpdates;
    const bool allowDrops;
    const uint16_t requiredContext;

    ecg_ref32::CommitUpdateQueue queue;
    PendingRecord pendingRecord;
    std::unique_ptr<ProbeListenerArgFunc<o3::DynInstPtr>> commitListener;
    EventFunctionWrapper serviceEvent;

    bool degraded = false;
    bool haveLastSequence = false;
    uint32_t lastSequence = 0;

    uint64_t recordLoads = 0;
    uint64_t recordBytes = 0;
    uint64_t governedLoads = 0;
    uint64_t generated = 0;
    uint64_t accepted = 0;
    uint64_t enqueued = 0;
    uint64_t coalesced = 0;
    uint64_t delivered = 0;
    uint64_t applied = 0;
    uint64_t stale = 0;
    uint64_t expired = 0;
    uint64_t notResident = 0;
    uint64_t invalidDelivery = 0;
    uint64_t cancelled = 0;
    uint64_t fullDrops = 0;
    uint64_t ingressDrops = 0;
    uint64_t degradedDrops = 0;
    uint64_t invalidInputErrors = 0;
    uint64_t invalidOrderErrors = 0;
    uint64_t invalidTimeErrors = 0;
    uint64_t invalidConfigErrors = 0;
    uint64_t protocolErrors = 0;
    uint64_t retirementCycle = 0;
    uint64_t retirementBurst = 0;
    uint64_t maxRetirementBurst = 0;
    uint64_t maxOccupancy = 0;
    uint64_t minimumLatency = std::numeric_limits<uint64_t>::max();
};

} // namespace gem5

#endif
