// ============================================================================
// P-OPT Replacement Policy for gem5
// ============================================================================
//
// Practical Optimal cache replacement for Graph Analytics (P-OPT)
// Balaji et al. (2021)
//
// Uses pre-computed rereference matrix (from graph transpose) to predict
// when each cache line will next be accessed. 3-phase eviction:
//   Phase 1: Evict non-graph data first (streaming/CSR metadata)
//   Phase 2: Among graph data → max rereference distance
//   Phase 3: RRIP tiebreaker for lines with equal distance
//
// The rereference matrix is oracle data stored host-side (not in simulated
// memory). It is loaded from a binary file produced by the GraphBrew
// pipeline's makeOffsetMatrix() function.
//
// Reference implementation: bench/include/cache_sim/cache_sim.h
//   POPTState (lines 146-212), findVictimPOPT (lines ~890-950)
// ============================================================================

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_POPT_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_POPT_RP_HH__

#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphPoptRP.hh"
#include "base/statistics.hh"

#include <cstdint>
#include <memory>

namespace gem5 {
namespace replacement_policy {

class GraphPoptRP : public Base
{
  public:
    PARAMS(GraphPoptRP);

    // Per-cache-line replacement data
    struct PoptReplData : public ReplacementData
    {
        uint8_t rrpv;
        bool is_property_data;
        uint64_t line_addr;     // Cache-line-aligned address (for matrix lookup)

        PoptReplData(uint8_t max_rrpv)
            : rrpv(max_rrpv), is_property_data(false), line_addr(0) {}
    };

    GraphPoptRP(const Params &p);
    ~GraphPoptRP() override = default;

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
    void tryLoadContext() const;

    const uint8_t maxRRPV;
    const std::string sidebandPath;
    const std::string poptMatrixPath;

    mutable graph::GraphCacheContext ctx;
    mutable bool loadAttempted = false;
    mutable bool activeAnnounced = false;
    mutable uint64_t loadAttemptCount = 0;

    mutable struct PoptStats : public statistics::Group
    {
        PoptStats(statistics::Group* parent);
        statistics::Scalar rereferenceQueries;
    } poptStats;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_POPT_RP_HH__
