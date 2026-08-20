// ============================================================================
// ECG Replacement Policy for gem5
// ============================================================================
// Faithful 3-level layered eviction matching cache_sim.h findVictimECG.
// Loads context from sideband JSON written by benchmark at runtime.
// ============================================================================

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RP_HH__

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/ecg_victim_policy.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphEcgRP.hh"

#include <cstdint>
#include <memory>
#include <string>

namespace gem5 {
namespace replacement_policy {

class GraphEcgRP : public Base
{
  public:
    PARAMS(GraphEcgRP);

    struct EcgReplData : public ReplacementData
    {
        uint8_t rrpv;
        uint8_t ecg_dbg_tier;
        uint8_t ecg_popt_hint;
        uint16_t ecg_epoch;
        uint16_t ecg_epoch2;
        uint16_t ecg_context_id;
        uint8_t ecg_epoch_count;
        bool ecg_epoch_valid;  // a per-edge epoch was DELIVERED (vs epoch==0 ambiguity);
                               // mirrors cache_sim/Sniper so `stamped` is identical
        bool valid;
        bool is_property_data;
        uint64_t line_addr;
        uint64_t lastTouchTick;  // recency, for ECG_PROP_EVICT_LRU ablation

        EcgReplData(uint8_t max_rrpv)
            : rrpv(max_rrpv), ecg_dbg_tier(0), ecg_popt_hint(0),
              ecg_epoch(0), ecg_epoch2(0), ecg_context_id(0),
              ecg_epoch_count(0), ecg_epoch_valid(false), valid(false),
              is_property_data(false), line_addr(0),
              lastTouchTick(0) {}
    };

    GraphEcgRP(const Params &p);
    ~GraphEcgRP() override = default;

    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data) override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void setVictimRequest(const PacketPtr pkt) override;
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

  private:
    void tryLoadContext() const;
    bool legacyRequestState(
        uint16_t& current_epoch, uint16_t& context_id,
        uint32_t& sequence) const;

    const uint8_t rrpvMax;
    const uint8_t numBuckets;
    const graph::ECGMode ecgMode;
    const uint64_t llcSize;
    const std::string sidebandPath;
    const std::string poptMatrixPath;

    mutable graph::GraphCacheContext ctx;
    mutable bool loadAttempted = false;
    mutable uint64_t loadAttemptCount = 0;
    mutable ecg_policy::OnlineDuelingSelector duelingSelector;
    mutable bool victimRequestValid = false;
    mutable uint16_t victimCurrentEpoch = 0;
    mutable uint16_t victimContextId = 0;

    mutable struct OnlineDuelingStats : public statistics::Group
    {
        OnlineDuelingStats(statistics::Group* parent);
        statistics::Scalar requestBoundVictims;
        statistics::Scalar leaderSamples;
        statistics::Scalar followerSelections;
        statistics::Scalar completedWindows;
        statistics::Scalar winnerChanges;
        statistics::Scalar followerVariantOverrides;
        statistics::Scalar reuseAdmissionUpdates;
    } onlineDuelingStats;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RP_HH__
