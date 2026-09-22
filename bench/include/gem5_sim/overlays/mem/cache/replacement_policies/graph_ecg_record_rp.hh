#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_ECG_RECORD_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_ECG_RECORD_RP_HH__

#include <cstdint>
#include <memory>
#include <string>

#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/ecg_record_native.h"
#include "mem/cache/replacement_policies/ecg_record_runtime.h"
#include "mem/cache/replacement_policies/ecg_ref32_native_state.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphEcgRecordRP.hh"

namespace gem5
{
namespace replacement_policy
{

class GraphEcgRecordRP : public Base
{
  public:
    PARAMS(GraphEcgRecordRP);

    struct RecordReplData : public ReplacementData
    {
        ecg_ref32::NativeLineBinding binding;
        ecg_record::LineMetadata metadata;
        uint64_t recency = 0;
        uint8_t rrpv = 7;
        uint8_t graspTier = 3;
        bool valid = false;
    };

    explicit GraphEcgRecordRP(const Params& params);
    void invalidate(const std::shared_ptr<ReplacementData>& data) override;
    void touch(const std::shared_ptr<ReplacementData>& data, const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData>& data) const override;
    void reset(const std::shared_ptr<ReplacementData>& data, const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData>& data) const override;
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

    bool supportsEcgRecord() const override { return true; }
    bool bypassEcgRecordDead() const override { return replacementEnabled; }
    bool configureEcgRecord(const ecg_record::NativeConfiguration& value) override;
    ecg_record::ObservationResult advanceEcgRecordProgress(
        uint16_t context, uint64_t generation, uint64_t sequence) override;
    void invalidateEcgRecordMetadata(
        const std::shared_ptr<ReplacementData>& data) override;
    void finishEcgRecordInvalidation() override;
    ecg_record::ApplyResult applyEcgRecordUpdate(
        const std::shared_ptr<ReplacementData>& data,
        const ecg_record::CommitUpdate& update) override;
    bool canAdmitEcgRecordPrefetch(
        const ReplacementCandidates& candidates, uint64_t sequence) const override;

  private:
    uint64_t lineAddress(uint64_t address) const;
    bool isProperty(uint64_t address) const;
    uint8_t insertionRrpv(uint8_t tier) const;
    void bind(RecordReplData& data, const PacketPtr pkt);
    void observe(RecordReplData& data, const PacketPtr pkt);
    ecg_record::WayState way(const RecordReplData& data) const;

    const bool replacementEnabled;
    const bool governedFirst;
    const uint64_t llcSize;
    const uint32_t lineSize;
    const double hotFraction;
    const std::string sidebandPath;
    graph::GraphCacheContext context;
    ecg_record::NativeConfiguration configuration;
    ecg_record::PropertyDescriptor propertyDescriptor;
    ecg_record::Receiver receiver;
};

} // namespace replacement_policy
} // namespace gem5

#endif
