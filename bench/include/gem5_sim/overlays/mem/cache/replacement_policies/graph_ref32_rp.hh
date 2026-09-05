#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_REF32_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_REF32_RP_HH__

#include <cstdint>
#include <memory>
#include <string>

#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/ecg_ref32_native_state.hh"
#include "mem/cache/replacement_policies/ecg_ref32_observation.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphRef32RP.hh"

namespace gem5
{
namespace replacement_policy
{

class GraphRef32RP : public Base
{
  public:
    PARAMS(GraphRef32RP);

    struct Ref32ReplData : public ReplacementData
    {
        uint8_t rrpv;
        uint8_t graspTier = 3;
        bool valid = false;
        uint64_t lastTouchTick = 0;
        ecg_ref32::NativeLineBinding binding;
        ecg_ref32::NativeLineMetadata metadata;

        explicit Ref32ReplData(uint8_t max_rrpv)
            : rrpv(max_rrpv)
        {}
    };

    explicit GraphRef32RP(const Params &params);

    void invalidate(
        const std::shared_ptr<ReplacementData>& replacement_data) override;
    void touch(
        const std::shared_ptr<ReplacementData>& replacement_data,
        const PacketPtr pkt) override;
    void touch(
        const std::shared_ptr<ReplacementData>& replacement_data)
        const override;
    void reset(
        const std::shared_ptr<ReplacementData>& replacement_data,
        const PacketPtr pkt) override;
    void reset(
        const std::shared_ptr<ReplacementData>& replacement_data)
        const override;
    ReplaceableEntry* getVictim(
        const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

    bool supportsEcgRef32() const override { return true; }
    ecg_ref32::CommitApplyResult applyEcgRef32Update(
        const std::shared_ptr<ReplacementData>& replacement_data,
        const ecg_ref32::CommitUpdate& update) override;
    void disableEcgRef32() override;

  private:
    void tryLoadContext() const;
    uint64_t lineAddress(uint64_t address) const;
    uint8_t classifyGrasp(uint64_t address) const;
    uint8_t insertionRrpv(uint8_t grasp_tier) const;
    void observe(
        Ref32ReplData& data, const PacketPtr pkt);
    ReplaceableEntry* graspFallback(
        const ReplacementCandidates& candidates) const;

    const uint8_t maxRrpv;
    const double hotFraction;
    const uint64_t llcSize;
    const uint32_t lineSize;
    const uint16_t requiredContext;
    const std::string sidebandPath;

    mutable graph::GraphCacheContext ctx;
    mutable ecg_ref32::NativeReceiverState receiver;
};

} // namespace replacement_policy
} // namespace gem5

#endif
