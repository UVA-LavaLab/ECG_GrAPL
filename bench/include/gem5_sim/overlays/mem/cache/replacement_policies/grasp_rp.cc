// ============================================================================
// GRASP Replacement Policy for gem5 - Implementation
// ============================================================================
// Faithful implementation matching bench/include/cache_sim/cache_sim.h.
// Loads property region metadata from sideband JSON written by the benchmark.
// ============================================================================

#include "mem/cache/replacement_policies/grasp_rp.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace gem5 {
namespace replacement_policy {

GraphGraspRP::GraphGraspRP(const Params &p)
    : Base(p),
      maxRRPV(p.max_rrpv),
      numBuckets(p.num_buckets),
      hotFraction(p.hot_fraction),
      llcSize(p.llc_size_bytes),
      sidebandPath(p.sideband_path),
      graspStats(this)
{
}

void
GraphGraspRP::tryLoadContext() const
{
    if (ctx.loaded) return;
    loadAttempted = true;

    constexpr uint64_t retryInterval = 512;
    if ((loadAttemptCount++ % retryInterval) != 0) return;

    if (ctx.loadFromSideband(sidebandPath)) {
        ctx.loaded = true;
        std::fprintf(
            stderr,
            "[GRASP-ACTIVE sim=gem5 context=1 regions=%u]\n",
            ctx.num_regions);
    }
}

void
GraphGraspRP::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<GraspReplData>(replacement_data);
    data->rrpv = maxRRPV;
    data->is_property_data = false;
    data->degree_bucket = numBuckets;
    data->line_addr = 0;
}

void
GraphGraspRP::touch(const std::shared_ptr<ReplacementData>& replacement_data,
                    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<GraspReplData>(replacement_data);
    promoteOnHit(data.get());
}

void
GraphGraspRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<GraspReplData>(replacement_data);
    if (data->rrpv > 0) data->rrpv--;
}

void
GraphGraspRP::reset(const std::shared_ptr<ReplacementData>& replacement_data,
                    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<GraspReplData>(replacement_data);

    tryLoadContext();

    if (pkt && pkt->req) {
        uint64_t addr = pkt->req->hasVaddr() ? pkt->req->getVaddr()
                             : pkt->req->getPaddr();
        data->line_addr = addr & ~uint64_t(63);

        if (ctx.loaded && ctx.isPropertyData(addr)) {
            ReuseTier tier = classifyAddress(addr);
            data->rrpv = insertionRRPV(tier);
            data->is_property_data = true;
            ctx.updateVertexFromAddr(addr);
        } else {
            // Official faldupriyank/grasp: everything outside the high/moderate
            // regions inserts at M_RRIP (max=7), including CSR/stack/instruction
            // traffic. This must match cache_sim and ECG's GRASP insertion arm.
            data->rrpv = maxRRPV;
            data->is_property_data = false;
        }
    } else {
        data->rrpv = maxRRPV;
        data->is_property_data = false;
    }
}

void
GraphGraspRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<GraspReplData>(replacement_data);
    data->rrpv = maxRRPV;
    data->is_property_data = false;
    data->line_addr = 0;
}

ReplaceableEntry*
GraphGraspRP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    while (true) {
        for (const auto& candidate : candidates) {
            auto data = std::static_pointer_cast<GraspReplData>(
                candidate->replacementData);
            if (data->rrpv >= maxRRPV) return candidate;
        }
        for (const auto& candidate : candidates) {
            auto data = std::static_pointer_cast<GraspReplData>(
                candidate->replacementData);
            if (data->rrpv < maxRRPV) data->rrpv++;
        }
    }
}

std::shared_ptr<ReplacementData>
GraphGraspRP::instantiateEntry()
{
    return std::make_shared<GraspReplData>(maxRRPV);
}

GraphGraspRP::ReuseTier
GraphGraspRP::classifyAddress(uint64_t addr) const
{
    if (ctx.loaded) {
        uint32_t tier = ctx.classifyGRASP(addr, llcSize, hotFraction);
        if (tier == 1) {
            ++graspStats.hotPropertyAccesses;
            return ReuseTier::HIGH;
        }
        if (tier == 2) return ReuseTier::MODERATE;
        return ReuseTier::LOW;
    }
    return ReuseTier::LOW;
}

uint8_t
GraphGraspRP::insertionRRPV(ReuseTier tier) const
{
    switch (tier) {
        case ReuseTier::HIGH:     return 1;
        case ReuseTier::MODERATE: return maxRRPV - 1;
        case ReuseTier::LOW:      return maxRRPV;
    }
    return maxRRPV;
}

void
GraphGraspRP::promoteOnHit(GraspReplData* data) const
{
    if (
            data->line_addr != 0 &&
            ctx.loaded && ctx.isPropertyData(data->line_addr)) {
        data->is_property_data = true;
        uint32_t tier = ctx.classifyGRASP(data->line_addr, llcSize, hotFraction);
        if (tier == 1) {
            ++graspStats.hotPropertyAccesses;
            data->rrpv = 0;
        } else if (data->rrpv > 0) {
            data->rrpv--;
        }
        ctx.updateVertexFromAddr(data->line_addr);
    } else if (data->rrpv > 0) {
        data->rrpv--;
    }
}

GraphGraspRP::GraspStats::GraspStats(statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(
        hotPropertyAccesses,
        "Number of GRASP hot-tier property classifications")
{
}

} // namespace replacement_policy
} // namespace gem5
