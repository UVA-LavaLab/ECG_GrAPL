#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_HAWKEYE_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_HAWKEYE_RP_HH__

#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/hawkeye_policy.h"
#include "params/GraphHawkeyeRP.hh"

#include <cstdint>
#include <memory>
#include <vector>

namespace gem5 {
namespace replacement_policy {

class GraphHawkeyeRP : public Base
{
  public:
    PARAMS(GraphHawkeyeRP);

    struct HawkeyeReplData : public ReplacementData
    {
        uint8_t rrpv = hawkeye_policy::kMaxRrpv;
        uint64_t signature = 0;
        uint64_t line_addr = 0;
        bool is_prefetch = false;
        bool valid = false;
    };

    GraphHawkeyeRP(const Params& p);
    ~GraphHawkeyeRP() override = default;

    void invalidate(
        const std::shared_ptr<ReplacementData>& replacement_data) override;
    void touch(
        const std::shared_ptr<ReplacementData>& replacement_data,
        const PacketPtr pkt) override;
    void touch(
        const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void reset(
        const std::shared_ptr<ReplacementData>& replacement_data,
        const PacketPtr pkt) override;
    void reset(
        const std::shared_ptr<ReplacementData>& replacement_data) const override;
    void setVictimRequest(const PacketPtr pkt) override;
    ReplaceableEntry* getVictim(
        const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

  private:
    uint64_t signature(const PacketPtr pkt) const;
    bool isPrefetch(const PacketPtr pkt) const;
    uint64_t lineAddress(const PacketPtr pkt) const;
    std::size_t setIndex(uint64_t line_addr) const;
    void applyAccess(
        HawkeyeReplData* data, const PacketPtr pkt, bool is_fill) const;

    const std::size_t numSets;
    const std::size_t numWays;
    const uint64_t lineSize;
    mutable hawkeye_policy::State state;

    mutable bool pendingValid = false;
    mutable bool pendingTrainIncoming = false;
    mutable bool pendingReady = false;
    mutable bool pendingFriendly = false;
    mutable bool pendingPrefetch = false;
    mutable uint64_t pendingLineAddr = 0;
    mutable uint64_t pendingSignature = 0;
    mutable std::size_t pendingSet = 0;
    mutable std::vector<std::shared_ptr<HawkeyeReplData>> pendingCandidates;
    mutable std::size_t pendingVictim = 0;
    mutable bool pendingVictimValid = false;
    mutable uint8_t pendingVictimRrpv = hawkeye_policy::kMaxRrpv;
    mutable uint64_t pendingVictimSignature = 0;
    mutable bool pendingVictimPrefetch = false;
};

}  // namespace replacement_policy
}  // namespace gem5

#endif  // __MEM_CACHE_REPLACEMENT_POLICIES_HAWKEYE_RP_HH__
