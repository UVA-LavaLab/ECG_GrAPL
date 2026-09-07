#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_TRANSPORT_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_TRANSPORT_HH__

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "cpu/o3/dyn_inst_ptr.hh"
#include "mem/cache/replacement_policies/ecg_record_control.hh"
#include "mem/cache/replacement_policies/ecg_record_native.h"
#include "mem/cache/replacement_policies/ecg_record_runtime.h"
#include "params/EcgRecordTransport.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

class BaseCache;
class EcgRecordPrefetch;
namespace o3 { class CPU; }

class EcgRecordTransport : public ClockedObject, public EcgRecordControl
{
  public:
    PARAMS(EcgRecordTransport);
    explicit EcgRecordTransport(const Params& params);
    void regProbeListeners() override;
    DrainState drain() override;
    uint64_t pendingWork() const override;
    void report() const;

  private:
    void observeCommit(const o3::DynInstPtr& inst);
    void configure(const o3::DynInstPtr& inst);
    void record(const o3::DynInstPtr& inst);
    void property(const o3::DynInstPtr& inst);
    void passClose(const o3::DynInstPtr& inst);
    void invalidateBinding(const o3::DynInstPtr& inst);
    void ordinaryAccess(const o3::DynInstPtr& inst);
    void enqueueUpdate(const ecg_record::CommitUpdate& update);
    bool validLoad(const o3::DynInstPtr& inst, uint8_t bytes) const;
    void service();
    void scheduleService();
    void invalidateSet();
    bool managed() const;

    o3::CPU* const cpu;
    BaseCache* const llc;
    EcgRecordPrefetch* const prefetcher;
    const bool applyUpdates;
    const uint16_t requiredContext;
    ecg_record::CommitQueue queue;
    ecg_record::NativeConfiguration configuration;
    ecg_record::Layout layout;
    ecg_record::PropertyDescriptor propertyDescriptor;
    ecg_record::PassCursor passCursor;
    std::optional<ecg_record::NativeLoadResult> pendingRecord;
    std::unique_ptr<ProbeListenerArgFunc<o3::DynInstPtr>> commitListener;
    EventFunctionWrapper serviceEvent;
    EventFunctionWrapper invalidationEvent;
    bool configured = false;
    bool bindingInvalidated = false;
    bool passConfigured = false;
    uint64_t invalidationSet = 0;
    uint64_t invalidationSetCount = 0;
    uint64_t eventOrder = 0;
    uint64_t lastSequence = 0;
    uint64_t configurations = 0;
    uint64_t recordLoads = 0;
    uint64_t recordReadBytes = 0;
    uint64_t propertyLoads = 0;
    uint64_t passes = 0;
    uint64_t structuralPositions = 0;
    uint64_t consumed = 0;
    uint64_t skipped = 0;
    uint64_t ordinaryInvalidations = 0;
    uint64_t rebinds = 0;
    uint64_t invalidationSets = 0;
    uint64_t invalidationCycles = 0;
    uint64_t generated = 0;
    uint64_t enqueued = 0;
    uint64_t coalesced = 0;
    uint64_t delivered = 0;
    uint64_t applied = 0;
    uint64_t stale = 0;
    uint64_t expired = 0;
    uint64_t absent = 0;
    uint64_t maxOccupancy = 0;
    uint64_t minLatency = std::numeric_limits<uint64_t>::max();
};

} // namespace gem5

#endif
