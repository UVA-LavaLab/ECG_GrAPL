#include "mem/cache/replacement_policies/hawkeye_rp.hh"

#include <algorithm>
#include <cassert>
#include <vector>

namespace gem5 {
namespace replacement_policy {

GraphHawkeyeRP::GraphHawkeyeRP(const Params& p)
    : Base(p),
      numSets(std::max<std::size_t>(p.num_sets, 1)),
      numWays(std::max<std::size_t>(p.num_ways, 1)),
      lineSize(std::max<uint64_t>(p.line_size, 1)),
      state(numSets, static_cast<uint16_t>(numWays))
{}

void
GraphHawkeyeRP::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<HawkeyeReplData>(replacement_data);
    data->rrpv = hawkeye_policy::kMaxRrpv;
    data->signature = 0;
    data->line_addr = 0;
    data->is_prefetch = false;
    data->valid = false;
}

void
GraphHawkeyeRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<HawkeyeReplData>(replacement_data);
    applyAccess(data.get(), pkt, false);
}

void
GraphHawkeyeRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<HawkeyeReplData>(replacement_data);
    data->rrpv = 0;
}

void
GraphHawkeyeRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<HawkeyeReplData>(replacement_data);
    const uint64_t line_addr = lineAddress(pkt);
    if (pendingReady && line_addr == pendingLineAddr) {
        if (pendingVictimValid &&
            pendingVictimRrpv != hawkeye_policy::kMaxRrpv) {
            state.eviction(
                pendingSet, pendingVictimSignature,
                pendingVictimPrefetch);
        }
        pendingFriendly = false;
        if (pendingTrainIncoming) {
            pendingFriendly = state.access(
                pendingSet, pendingLineAddr / lineSize,
                pendingSignature, pendingPrefetch);
        }
        if (pendingTrainIncoming && pendingFriendly) {
            bool has_six = false;
            for (std::size_t index = 0;
                 index < pendingCandidates.size(); ++index) {
                if (index == pendingVictim) continue;
                const auto& candidate = pendingCandidates[index];
                has_six |= candidate->valid &&
                    candidate->rrpv == hawkeye_policy::kMaxRrpv - 1;
            }
            if (!has_six) {
                for (std::size_t index = 0;
                     index < pendingCandidates.size(); ++index) {
                    if (index == pendingVictim) continue;
                    auto& candidate = pendingCandidates[index];
                    if (candidate->valid &&
                        candidate->rrpv < hawkeye_policy::kMaxRrpv - 1) {
                        ++candidate->rrpv;
                    }
                }
            }
        }
        data->rrpv = hawkeye_policy::insertionRrpv(pendingFriendly);
        data->signature = pendingSignature;
        data->line_addr = pendingLineAddr;
        data->is_prefetch = pendingPrefetch;
        data->valid = true;
        pendingReady = false;
        pendingValid = false;
        pendingTrainIncoming = false;
        pendingCandidates.clear();
        return;
    }
    if (pendingReady) {
        pendingReady = false;
        pendingValid = false;
        pendingTrainIncoming = false;
        pendingCandidates.clear();
    }
    applyAccess(data.get(), pkt, true);
}

void
GraphHawkeyeRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<HawkeyeReplData>(replacement_data);
    data->rrpv = hawkeye_policy::kMaxRrpv;
    data->signature = 0;
    data->line_addr = 0;
    data->is_prefetch = false;
    data->valid = true;
}

void
GraphHawkeyeRP::setVictimRequest(const PacketPtr pkt)
{
    if (!pkt) return;
    pendingCandidates.clear();
    pendingValid = true;
    pendingTrainIncoming = !pkt->isWriteback();
    pendingReady = false;
    pendingLineAddr = lineAddress(pkt);
    pendingSignature = pendingTrainIncoming ? signature(pkt) : 0;
    pendingPrefetch = pendingTrainIncoming && isPrefetch(pkt);
    pendingSet = setIndex(pendingLineAddr);
}

ReplaceableEntry*
GraphHawkeyeRP::getVictim(
    const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());
    std::size_t victim = 0;
    bool found_invalid = false;
    std::vector<uint8_t> rrpv(candidates.size(), 0);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto data = std::static_pointer_cast<HawkeyeReplData>(
            candidates[index]->replacementData);
        rrpv[index] = data->rrpv;
        if (!found_invalid && !data->valid) {
            victim = index;
            found_invalid = true;
        }
    }
    if (!found_invalid) {
        victim = hawkeye_policy::selectVictim(
            rrpv.data(), rrpv.size());
    }

    if (pendingValid) {
        pendingSet = candidates.front()->getSet();
        pendingCandidates.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            pendingCandidates.push_back(
                std::static_pointer_cast<HawkeyeReplData>(
                    candidate->replacementData));
        }
        pendingVictim = victim;
        pendingVictimValid = pendingCandidates[victim]->valid;
        pendingVictimRrpv = pendingCandidates[victim]->rrpv;
        pendingVictimSignature = pendingCandidates[victim]->signature;
        pendingVictimPrefetch = pendingCandidates[victim]->is_prefetch;
        pendingReady = true;
    }
    return candidates[victim];
}

std::shared_ptr<ReplacementData>
GraphHawkeyeRP::instantiateEntry()
{
    return std::make_shared<HawkeyeReplData>();
}

uint64_t
GraphHawkeyeRP::signature(const PacketPtr pkt) const
{
    return pkt && pkt->req && pkt->req->hasPC()
        ? pkt->req->getPC() : 0;
}

bool
GraphHawkeyeRP::isPrefetch(const PacketPtr pkt) const
{
    return pkt && (pkt->cmd.isPrefetch() ||
        (pkt->req && pkt->req->isPrefetch()));
}

uint64_t
GraphHawkeyeRP::lineAddress(const PacketPtr pkt) const
{
    return pkt ? (pkt->getAddr() & ~(lineSize - 1)) : 0;
}

std::size_t
GraphHawkeyeRP::setIndex(uint64_t line_addr) const
{
    return (line_addr / lineSize) % numSets;
}

void
GraphHawkeyeRP::applyAccess(
    HawkeyeReplData* data, const PacketPtr pkt, bool is_fill) const
{
    if (!pkt || pkt->isWriteback()) {
        data->rrpv = hawkeye_policy::kMaxRrpv;
        data->signature = 0;
        data->line_addr = lineAddress(pkt);
        data->is_prefetch = false;
        data->valid = true;
        return;
    }
    const uint64_t line_addr = lineAddress(pkt);
    const uint64_t pc = signature(pkt);
    const bool prefetch = isPrefetch(pkt);
    const bool friendly = state.access(
        setIndex(line_addr), line_addr / lineSize, pc, prefetch);
    data->rrpv = hawkeye_policy::insertionRrpv(friendly);
    data->signature = pc;
    data->line_addr = line_addr;
    data->is_prefetch = prefetch;
    data->valid = true;
    (void)is_fill;
}

}  // namespace replacement_policy
}  // namespace gem5
