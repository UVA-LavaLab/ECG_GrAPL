// ============================================================================
// GRASP Replacement Policy for gem5
// ============================================================================
//
// Graph-aware cache Replacement with Software Prefetching (GRASP)
// Faldu et al. (2020)
//
// Extends BRRIP with degree-based insertion and promotion:
//   - High-reuse (hot hubs): insert RRPV=1, hit → RRPV=0
//   - Moderate-reuse:        insert RRPV=M-1, hit → decrement
//   - Low-reuse (cold):      insert RRPV=M, hit → decrement
//
// Eviction: find way with max RRPV, age all if none at max (same as SRRIP).
//
// Requires DBG-reordered graph so hot vertices occupy low addresses, OR
// uses GraphCacheContext for region-based bucket classification.
//
// Reference implementation: bench/include/cache_sim/cache_sim.h
//   GRASPState (lines 218-280), findVictimGRASP (line ~870)
// ============================================================================

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_GRASP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_GRASP_RP_HH__

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphGraspRP.hh"

#include <cstdint>
#include <memory>

namespace gem5 {
namespace replacement_policy {

class GraphGraspRP : public Base
{
  public:
    PARAMS(GraphGraspRP);

    // Per-cache-line replacement data
    struct GraspReplData : public ReplacementData
    {
        uint8_t rrpv;           // Re-Reference Prediction Value (0 = near, M = distant)
        uint8_t degree_bucket;  // Degree bucket index (0=hub, N-1=cold)
        bool is_property_data;  // Whether this line holds vertex property data
        uint64_t line_addr;     // Cache-line-aligned address (for hit re-classification)

        GraspReplData(uint8_t max_rrpv)
            : rrpv(max_rrpv), degree_bucket(0), is_property_data(false),
              line_addr(0) {}
    };

    GraphGraspRP(const Params &p);
    ~GraphGraspRP() override = default;

    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
        override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data)
        const override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data)
        const override;

    ReplaceableEntry* getVictim(
        const ReplacementCandidates& candidates) const override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;

  private:
    enum class ReuseTier { HIGH, MODERATE, LOW };

    // Try to load sideband context (called lazily on first access)
    void tryLoadContext() const;

    ReuseTier classifyAddress(uint64_t addr) const;
    uint8_t insertionRRPV(ReuseTier tier) const;
    void promoteOnHit(GraspReplData* data) const;

    // Configuration
    const uint8_t maxRRPV;
    const uint8_t numBuckets;
    const double hotFraction;
    const uint64_t llcSize;
    const std::string sidebandPath;

    // Graph context — loaded lazily from sideband file
    mutable graph::GraphCacheContext ctx;
    mutable bool loadAttempted = false;
    mutable uint64_t loadAttemptCount = 0;

    mutable struct GraspStats : public statistics::Group
    {
        GraspStats(statistics::Group* parent);
        statistics::Scalar hotPropertyAccesses;
    } graspStats;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_GRASP_RP_HH__
