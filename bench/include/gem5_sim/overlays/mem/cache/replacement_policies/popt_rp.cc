// ============================================================================
// P-OPT Replacement Policy for gem5 — Implementation
// ============================================================================
// Faithful P-OPT adapted for gem5's full-system address space. The victim
// algorithm mirrors the upstream P-OPT cache simulator and GraphBrew cache_sim:
// evict non-property data first, then use rereference distance, then RRIP
// tiebreaks among equal-distance lines.
//
// Reference: Balaji et al. (2021)
// ============================================================================

#include "mem/cache/replacement_policies/popt_rp.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace gem5 {
namespace replacement_policy {

GraphPoptRP::GraphPoptRP(const Params &p)
    : Base(p),
      maxRRPV(p.max_rrpv),
      sidebandPath(p.sideband_path),
      poptMatrixPath(p.popt_matrix_path),
      poptStats(this)
{
}

void
GraphPoptRP::tryLoadContext() const
{
    if (ctx.loaded && ctx.rereference.enabled) return;
    loadAttempted = true;

    constexpr uint64_t retryInterval = 512;
    if ((loadAttemptCount++ % retryInterval) != 0) return;

    if (!ctx.loaded) {
        ctx.loadFromSideband(sidebandPath);
        ctx.loaded = (ctx.num_regions > 0);
    }

    if (!ctx.rereference.enabled &&
        ctx.rereference.loadFromFile(poptMatrixPath)) {
        // base_address is no longer hardcoded to region[0].
        // findNextRef() in GraphCacheContext now searches all regions
        // to find which one the address belongs to.
        // Set base_address to region[0] as a fallback for RereferenceMatrix's
        // direct findNextRefByAddr() (used when findNextRef bypasses context).
        if (ctx.num_regions > 0) {
            ctx.rereference.base_address = ctx.regions[0].base_address;
        }
    }
}

void
GraphPoptRP::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<PoptReplData>(replacement_data);
    data->rrpv = maxRRPV;
    data->is_property_data = false;
    data->line_addr = 0;
}

void
GraphPoptRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<PoptReplData>(replacement_data);
    data->rrpv = 0;
}

void
GraphPoptRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<PoptReplData>(replacement_data);
    data->rrpv = 0;
}

void
GraphPoptRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<PoptReplData>(replacement_data);

    tryLoadContext();

    // P-OPT insertion: RRPV = M-1
    data->rrpv = (maxRRPV > 0) ? maxRRPV - 1 : 0;

    if (pkt && pkt->req) {
        uint64_t addr = pkt->req->hasVaddr() ? pkt->req->getVaddr()
                             : pkt->req->getPaddr();
        data->line_addr = addr & ~uint64_t(63);
        data->is_property_data = ctx.loaded && ctx.isPropertyData(data->line_addr);
        // Update P-OPT vertex tracking from property accesses
        if (data->is_property_data) {
            ctx.updateVertexFromAddr(addr);
        }
    } else {
        data->line_addr = 0;
        data->is_property_data = false;
    }
}

void
GraphPoptRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<PoptReplData>(replacement_data);
    data->rrpv = (maxRRPV > 0) ? maxRRPV - 1 : 0;
}

ReplaceableEntry*
GraphPoptRP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    bool hasPopt = ctx.loaded && ctx.rereference.enabled;

    // If no P-OPT context, fall back to SRRIP
    if (!hasPopt) {
        while (true) {
            for (const auto& c : candidates) {
                auto d = std::static_pointer_cast<PoptReplData>(c->replacementData);
                if (d->rrpv >= maxRRPV) return c;
            }
            for (const auto& c : candidates) {
                auto d = std::static_pointer_cast<PoptReplData>(c->replacementData);
                if (d->rrpv < maxRRPV) d->rrpv++;
            }
        }
    }

    // Count property vs non-property lines in the candidate set
    int propCount = 0;
    for (const auto& c : candidates) {
        auto d = std::static_pointer_cast<PoptReplData>(c->replacementData);
        d->is_property_data = ctx.isPropertyData(d->line_addr);
        if (d->is_property_data) propCount++;
    }

    // Phase 1: evict non-property data before applying oracle rereference.
    if (propCount != static_cast<int>(candidates.size())) {
        for (const auto& c : candidates) {
            auto d = std::static_pointer_cast<PoptReplData>(c->replacementData);
            if (!d->is_property_data) return c;
        }
    }

    // Phase 2: find max rereference distance among property lines.
    if (!activeAnnounced) {
        activeAnnounced = true;
        std::fprintf(
            stderr,
            "[POPT-ACTIVE sim=gem5 context=1 reref=1 phase2=1 epochs=%u "
            "cache_lines=%u]\n",
            ctx.rereference.num_epochs,
            ctx.rereference.num_cache_lines);
    }

    uint8_t maxDist = 0;
    std::array<uint8_t, 64> fixedWayDists{};
    std::vector<uint8_t> dynamicWayDists;
    uint8_t* wayDists = fixedWayDists.data();
    if (candidates.size() > fixedWayDists.size()) {
        dynamicWayDists.resize(candidates.size());
        wayDists = dynamicWayDists.data();
    }

    for (size_t way = 0; way < candidates.size(); ++way) {
        const auto& c = candidates[way];
        auto d = std::static_pointer_cast<PoptReplData>(c->replacementData);
        ++poptStats.rereferenceQueries;
        uint32_t dist = ctx.findNextRef(d->line_addr);
        uint8_t d8 = static_cast<uint8_t>(std::min(dist, uint32_t(127)));
        wayDists[way] = d8;
        if (d8 > maxDist) maxDist = d8;
    }

    // Phase 3: RRIP tiebreaker among max-distance lines.
    while (true) {
        for (size_t way = 0; way < candidates.size(); ++way) {
            if (wayDists[way] == maxDist) {
                auto d = std::static_pointer_cast<PoptReplData>(
                    candidates[way]->replacementData);
                if (d->rrpv >= maxRRPV) return candidates[way];
            }
        }
        for (size_t way = 0; way < candidates.size(); ++way) {
            if (wayDists[way] == maxDist) {
                auto d = std::static_pointer_cast<PoptReplData>(
                    candidates[way]->replacementData);
                if (d->rrpv < maxRRPV) d->rrpv++;
            }
        }
    }
}

std::shared_ptr<ReplacementData>
GraphPoptRP::instantiateEntry()
{
    return std::make_shared<PoptReplData>(maxRRPV);
}

GraphPoptRP::PoptStats::PoptStats(statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(
        rereferenceQueries,
        "Number of P-OPT phase-two rereference-matrix queries")
{
}

} // namespace replacement_policy
} // namespace gem5
