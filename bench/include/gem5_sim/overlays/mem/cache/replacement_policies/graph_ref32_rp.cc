#include "mem/cache/replacement_policies/graph_ref32_rp.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

#include "base/logging.hh"
#include "mem/packet.hh"

namespace gem5
{
namespace replacement_policy
{

GraphRef32RP::GraphRef32RP(const Params &params)
    : Base(params),
      maxRrpv(params.rrpv_max),
      hotFraction(params.hot_fraction),
      llcSize(params.llc_size_bytes),
      lineSize(params.line_size),
      requiredContext(static_cast<uint16_t>(params.required_context)),
      sidebandPath(params.sideband_path)
{
    fatal_if(lineSize != 64, "%s requires 64-byte cache lines", name());
    fatal_if(
        params.required_context == 0 ||
            params.required_context > std::numeric_limits<uint16_t>::max(),
        "%s requires a context in [1,65535]", name());
    fatal_if(
        graph::stringToECGMode("ECG_REF32", true) !=
            graph::ECGMode::ECG_REF32,
        "%s failed to enable the native REF32 parser gate", name());
}

void
GraphRef32RP::tryLoadContext() const
{
    if (ctx.loaded)
        return;
    if (!ctx.loadFromSideband(sidebandPath))
        return;

    fatal_if(
        ctx.num_regions != 2 ||
            ctx.regions[0].name != "scores" ||
            ctx.regions[1].name != "contrib",
        "%s requires sealed PR property regions scores,contrib", name());
    fatal_if(
        ctx.topology.num_edges == 0 ||
            ctx.topology.num_edges >= 0x80000000ULL,
        "%s requires record_count in [1,2^31)", name());
    fatal_if(
        !receiver.configure(
            static_cast<uint32_t>(ctx.topology.num_edges),
            requiredContext),
        "%s could not configure the native REF32 receiver", name());

    std::fprintf(
        stderr,
        "[ECG-REF32-RP-ACTIVE context=%u records=%llu "
        "line_bits=35 local_grasp=region hot_fraction=%.6f "
        "prefetch=0]\n",
        static_cast<unsigned>(requiredContext),
        static_cast<unsigned long long>(ctx.topology.num_edges),
        hotFraction);
}

uint64_t
GraphRef32RP::lineAddress(uint64_t address) const
{
    return address - (address % lineSize);
}

uint8_t
GraphRef32RP::classifyGrasp(uint64_t address) const
{
    return ctx.loaded
        ? static_cast<uint8_t>(
            ctx.classifyGRASP(address, llcSize, hotFraction))
        : 3;
}

uint8_t
GraphRef32RP::insertionRrpv(uint8_t grasp_tier) const
{
    if (grasp_tier == 1)
        return std::min<uint8_t>(1, maxRrpv);
    if (grasp_tier == 2)
        return maxRrpv > 0 ? maxRrpv - 1 : 0;
    return maxRrpv;
}

void
GraphRef32RP::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    data->rrpv = maxRrpv;
    data->graspTier = 3;
    data->valid = false;
    data->lastTouchTick = 0;
    data->binding.clear();
    data->metadata.clear();
}

void
GraphRef32RP::observe(Ref32ReplData& data, const PacketPtr pkt)
{
    if (!pkt || !pkt->req)
        return;
    graph::EcgRef32Observation observation;
    if (!graph::readEcgRef32Observation(pkt->req, observation))
        return;
    if (observation.conflict) {
        data.metadata.state =
            ecg_ref32::NativeLineState::UNKNOWN;
        data.metadata.value = 0;
        return;
    }

    fatal_if(!pkt->req->hasVaddr() || !data.binding.property,
             "%s received an REF32 observation for a non-governed line",
             name());
    fatal_if(
        observation.context != requiredContext ||
            !ctx.ecgEpochDestinationMatches(
                pkt->req->getVaddr(), observation.destination, lineSize),
        "%s received an invalid REF32 observation", name());

    const ecg_ref32::ObservationResult result = receiver.observe(
        data.metadata, observation.context, observation.sequence);
    fatal_if(
        result == ecg_ref32::ObservationResult::INVALID_CONTEXT ||
            result == ecg_ref32::ObservationResult::INVALID_ORDER,
        "%s rejected an REF32 observation protocol invariant", name());
}

void
GraphRef32RP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    tryLoadContext();
    data->lastTouchTick = curTick();

    if (pkt && pkt->req) {
        const uint64_t physical_line =
            lineAddress(pkt->getAddr());
        ecg_ref32::NativeBindingResult binding_result;
        if (pkt->req->hasVaddr()) {
            const uint64_t address = pkt->req->getVaddr();
            binding_result = data->binding.bindVirtual(
                physical_line, lineAddress(address),
                ctx.loaded && ctx.isEcgEpochData(lineAddress(address)),
                ctx.loaded);
            if (binding_result !=
                    ecg_ref32::NativeBindingResult::CONFLICT &&
                data->binding.property) {
                data->graspTier = classifyGrasp(address);
            }
        } else {
            binding_result =
                data->binding.bindPhysical(physical_line);
        }
        fatal_if(
            binding_result == ecg_ref32::NativeBindingResult::CONFLICT,
            "%s detected a conflicting valid VA/PA alias", name());
        observe(*data, pkt);
    }

    if (data->binding.property && data->graspTier == 1) {
        data->rrpv = 0;
    } else if (data->rrpv > 0) {
        --data->rrpv;
    }
}

void
GraphRef32RP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    data->lastTouchTick = curTick();
    if (data->binding.property && data->graspTier == 1) {
        data->rrpv = 0;
    } else if (data->rrpv > 0) {
        --data->rrpv;
    }
}

void
GraphRef32RP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    tryLoadContext();

    data->valid = true;
    data->lastTouchTick = curTick();
    data->graspTier = 3;
    data->binding.clear();
    data->metadata.clear();
    data->metadata.prefetchOrigin =
        pkt && pkt->cmd.isHWPrefetch();

    if (pkt && pkt->req) {
        const uint64_t physical_line =
            lineAddress(pkt->getAddr());
        if (pkt->req->hasVaddr()) {
            const uint64_t address = pkt->req->getVaddr();
            const bool property =
                ctx.loaded && ctx.isEcgEpochData(lineAddress(address));
            const auto result = data->binding.bindVirtual(
                physical_line, lineAddress(address), property, ctx.loaded);
            fatal_if(
                result == ecg_ref32::NativeBindingResult::CONFLICT,
                "%s could not bind a reset VA/PA identity", name());
            if (data->binding.property)
                data->graspTier = classifyGrasp(address);
        } else {
            const auto result =
                data->binding.bindPhysical(physical_line);
            fatal_if(
                result == ecg_ref32::NativeBindingResult::CONFLICT,
                "%s could not bind a physical-only reset", name());
        }
    }
    data->rrpv = insertionRrpv(data->graspTier);
    observe(*data, pkt);
}

void
GraphRef32RP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    data->rrpv = maxRrpv;
    data->graspTier = 3;
    data->valid = true;
    data->lastTouchTick = curTick();
    data->binding.clear();
    data->metadata.clear();
}

ReplaceableEntry*
GraphRef32RP::graspFallback(
    const ReplacementCandidates& candidates) const
{
    while (true) {
        for (const auto candidate : candidates) {
            auto data = std::static_pointer_cast<Ref32ReplData>(
                candidate->replacementData);
            if (data->rrpv >= maxRrpv)
                return candidate;
        }
        for (const auto candidate : candidates) {
            auto data = std::static_pointer_cast<Ref32ReplData>(
                candidate->replacementData);
            if (data->rrpv < maxRrpv)
                ++data->rrpv;
        }
    }
}

ReplaceableEntry*
GraphRef32RP::getVictim(
    const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());
    for (const auto candidate : candidates) {
        auto data = std::static_pointer_cast<Ref32ReplData>(
            candidate->replacementData);
        if (!data->valid)
            return candidate;
    }
    if (!receiver.enabled())
        return graspFallback(candidates);

    fatal_if(candidates.size() > 64,
             "%s supports at most 64-way native REF32 caches", name());
    std::array<ecg_ref32::WayState, 64> ways{};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto data = std::static_pointer_cast<Ref32ReplData>(
            candidates[index]->replacementData);
        ways[index].property = data->binding.property;
        ways[index].rrpv = data->rrpv;
        ways[index].recency = data->lastTouchTick;
        ways[index].grasp_tier = data->graspTier;
        ways[index].state = receiver.watermarkValid()
            ? ecg_ref32::victimState(data->metadata, true)
            : ecg_ref32::State::UNKNOWN;
        ways[index].quantized_deadline = data->metadata.value;
    }
    const std::size_t victim = ecg_ref32::selectVictim(
        ways.data(), candidates.size(),
        receiver.watermarkValid() ? receiver.watermark() : 0,
        false, nullptr, 32);
    return candidates[victim];
}

ecg_ref32::CommitApplyResult
GraphRef32RP::applyEcgRef32Update(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const ecg_ref32::CommitUpdate& update)
{
    tryLoadContext();
    if (!receiver.enabled())
        return ecg_ref32::CommitApplyResult::UNSUPPORTED;
    if (update.context != requiredContext)
        return ecg_ref32::CommitApplyResult::INVALID_CONTEXT;
    if (update.secure || update.physical_line % lineSize != 0 ||
        (update.property_vaddr & 0x3u) != 0 || !ctx.loaded ||
        !ctx.isEcgEpochData(update.property_vaddr)) {
        return ecg_ref32::CommitApplyResult::INVALID_ADDRESS;
    }

    auto data = std::static_pointer_cast<Ref32ReplData>(replacement_data);
    if (data) {
        if (!data->valid) {
            return ecg_ref32::CommitApplyResult::INVALID_ADDRESS;
        }
        const auto binding_result = data->binding.bindVirtual(
            update.physical_line,
            lineAddress(update.property_vaddr), true);
        if (binding_result ==
            ecg_ref32::NativeBindingResult::CONFLICT) {
            return ecg_ref32::CommitApplyResult::INVALID_ADDRESS;
        }
        data->graspTier = classifyGrasp(update.property_vaddr);
    }

    const ecg_ref32::CommitApplyResult result =
        receiver.apply(data ? &data->metadata : nullptr, update);
    if (data && result == ecg_ref32::CommitApplyResult::APPLIED) {
        if (data->metadata.state ==
            ecg_ref32::NativeLineState::DEAD) {
            data->rrpv = maxRrpv;
        } else if (data->metadata.state ==
                   ecg_ref32::NativeLineState::FINITE) {
            const ecg_ref32::EffectiveFuture future =
                ecg_ref32::resolveQuantizedFuture(
                    ecg_ref32::State::FINITE,
                    data->metadata.value, receiver.watermark(), 32);
            data->rrpv = ecg_ref32::distanceRRPV(
                future.remaining, maxRrpv);
        }
    }
    return result;
}

void
GraphRef32RP::disableEcgRef32()
{
    receiver.disable();
}

std::shared_ptr<ReplacementData>
GraphRef32RP::instantiateEntry()
{
    return std::make_shared<Ref32ReplData>(maxRrpv);
}

} // namespace replacement_policy
} // namespace gem5
