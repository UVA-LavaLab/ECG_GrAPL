#include "mem/cache/replacement_policies/graph_ecg_record_rp.hh"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

#include "base/logging.hh"
#include "mem/cache/replacement_policies/ecg_record_observation.hh"
#include "mem/packet.hh"

namespace gem5
{
namespace replacement_policy
{

GraphEcgRecordRP::GraphEcgRecordRP(const Params& params)
    : Base(params), replacementEnabled(params.enable_replacement),
      governedFirst(params.governed_first),
      llcSize(params.llc_size_bytes), lineSize(params.line_size),
      hotFraction(params.hot_fraction), sidebandPath(params.sideband_path)
{
    fatal_if(lineSize != 64 || hotFraction <= 0 || hotFraction > 1,
             "%s requires 64-byte lines and a hot fraction in (0,1]", name());
}

bool
GraphEcgRecordRP::configureEcgRecord(const ecg_record::NativeConfiguration& value)
{
    ecg_record::Layout layout;
    if (ecg_record::validateNativeConfiguration(value, layout) != ecg_record::Status::OK)
        return false;
    ecg_record::PropertyDescriptor property;
    if (ecg_record::unpackProperty(
            value.property_descriptor, property) != ecg_record::Status::OK)
        return false;
    if (!context.loaded && !context.loadFromSideband(sidebandPath))
        return false;
    const bool managed =
        (value.control & ecg_record::kNativeManagedPasses) != 0;
    if (!managed) {
        if (context.num_regions != 2 || context.regions[0].name != "scores" ||
            context.regions[1].name != "contrib" ||
            context.regions[1].base_address != value.property_base ||
            context.regions[1].upper_bound !=
                value.property_base + value.vertex_count * 4 ||
            context.topology.num_vertices != value.vertex_count ||
            context.topology.num_edges != value.record_count) {
            return false;
        }
    } else {
        uint64_t extent = 0, record_bytes = 0;
        uint64_t property_upper = 0, record_upper = 0;
        if (ecg_record::propertyExtent(
                property, value.vertex_count, extent) !=
                ecg_record::Status::OK ||
            !ecg_record::checkedMultiply(
                value.record_count, layout.record_bytes, record_bytes) ||
            !ecg_record::checkedAdd(
                value.property_base, extent, property_upper) ||
            !ecg_record::checkedAdd(
                value.record_base, record_bytes, record_upper) ||
            context.topology.num_vertices != value.vertex_count)
            return false;
        bool property_match = false;
        for (uint32_t index = 0; index < context.num_regions; ++index) {
            const auto& region = context.regions[index];
            property_match |=
                region.base_address == value.property_base &&
                region.upper_bound >= property_upper &&
                region.num_elements == value.vertex_count &&
                region.elem_size == ecg_record::propertyBytes(property.kind) &&
                region.stride_bytes == property.stride_bytes;
        }
        const bool carrier_match =
            (context.edge_preferred.base_address == value.record_base &&
             context.edge_preferred.upper_bound >=
                record_upper) ||
            (context.edge_other.base_address == value.record_base &&
             context.edge_other.upper_bound >=
                record_upper) ||
            (context.flowthrough_base == value.record_base &&
             context.flowthrough_upper >= record_upper);
        if (!property_match || !carrier_match)
            return false;
    }
    if (receiver.enabled() &&
        (configuration.record_base != value.record_base ||
         configuration.property_base != value.property_base ||
         configuration.vertex_count != value.vertex_count ||
         configuration.property_descriptor != value.property_descriptor)) {
        return false;
    }
    const bool first = !receiver.enabled();
    if (!receiver.configure(
            layout, value.record_count, value.context, value.generation,
            property.traversal))
        return false;
    configuration = value;
    propertyDescriptor = property;
    if (first) {
        std::cout << "[ECG-RECORD-RP ";
        ecg_record::writeLayoutFields(std::cout, layout);
        std::cout << " replacement=" << replacementEnabled
                  << " governed_first=" << governedFirst
                  << " prediction_bits_per_line=67"
                  << " matrix_bytes=0]\n";
    }
    return true;
}

uint64_t
GraphEcgRecordRP::lineAddress(uint64_t address) const
{
    return address - address % lineSize;
}

bool
GraphEcgRecordRP::isProperty(uint64_t address) const
{
    return receiver.enabled() &&
        ecg_record::nativePropertyLine(configuration, address);
}

uint8_t
GraphEcgRecordRP::insertionRrpv(uint8_t tier) const
{
    return tier == 1 ? 1 : tier == 2 ? 6 : 7;
}

void
GraphEcgRecordRP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    *std::static_pointer_cast<RecordReplData>(replacement_data) = RecordReplData{};
}

void
GraphEcgRecordRP::bind(RecordReplData& data, const PacketPtr pkt)
{
    if (!pkt || !pkt->req)
        return;
    const auto physical = lineAddress(pkt->getAddr());
    ecg_ref32::NativeBindingResult result;
    if (pkt->req->hasVaddr()) {
        const auto virtual_address = pkt->req->getVaddr();
        result = data.binding.bindVirtual(
            physical, lineAddress(virtual_address), isProperty(virtual_address),
            receiver.enabled());
        if (receiver.enabled())
            data.graspTier = context.classifyGRASP(virtual_address, llcSize, hotFraction);
    } else {
        result = data.binding.bindPhysical(physical);
    }
    fatal_if(result == ecg_ref32::NativeBindingResult::CONFLICT,
             "%s received conflicting VA/PA line identities", name());
}

void
GraphEcgRecordRP::observe(RecordReplData& data, const PacketPtr pkt)
{
    if (!replacementEnabled || !receiver.enabled() || !pkt)
        return;
    graph::EcgRecordObservation observation;
    if (!graph::readEcgRecordObservation(pkt->req, observation)) {
        if (receiver.filtered() && pkt->req->hasVaddr() &&
            isProperty(pkt->req->getVaddr())) {
            const auto status = receiver.invalidateObservation(
                data.metadata, receiver.context(), receiver.generation(),
                receiver.watermark());
            fatal_if(status == ecg_record::ObservationResult::INVALID_CONTEXT ||
                status == ecg_record::ObservationResult::INVALID_ORDER,
                "%s rejected an ordinary governed-region observation", name());
        }
        return;
    }
    if (observation.conflict) {
        data.metadata.state = ecg_record::LineState::UNKNOWN;
        data.metadata.value = 0;
        return;
    }
    uint64_t address = 0;
    fatal_if(
        observation.destination >= configuration.vertex_count ||
        ecg_record::propertyAddress(
            propertyDescriptor, configuration.property_base,
            observation.destination, address) != ecg_record::Status::OK ||
        address != observation.propertyVaddr,
        "%s received an invalid ECG property observation", name());
    fatal_if(
        data.binding.bindVirtual(lineAddress(pkt->getAddr()), lineAddress(address), true) ==
            ecg_ref32::NativeBindingResult::CONFLICT,
        "%s observation disagrees with the resident property identity", name());
    const auto status = receiver.observe(
        data.metadata, observation.context, observation.generation, observation.sequence);
    fatal_if(
        status == ecg_record::ObservationResult::INVALID_CONTEXT ||
        status == ecg_record::ObservationResult::INVALID_ORDER,
        "%s rejected an ECG observation", name());
}

void
GraphEcgRecordRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data, const PacketPtr pkt)
{
    auto& data = *std::static_pointer_cast<RecordReplData>(replacement_data);
    bind(data, pkt);
    touch(replacement_data);
    observe(data, pkt);
}

void
GraphEcgRecordRP::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto& data = *std::static_pointer_cast<RecordReplData>(replacement_data);
    data.recency = curTick();
    if (data.binding.property && data.graspTier == 1)
        data.rrpv = 0;
    else if (data.rrpv)
        --data.rrpv;
}

void
GraphEcgRecordRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data, const PacketPtr pkt)
{
    reset(replacement_data);
    auto& data = *std::static_pointer_cast<RecordReplData>(replacement_data);
    bind(data, pkt);
    data.rrpv = insertionRrpv(data.graspTier);
    data.metadata.prefetch_origin = pkt && pkt->req && pkt->req->isPrefetch();
    observe(data, pkt);
}

void
GraphEcgRecordRP::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto& data = *std::static_pointer_cast<RecordReplData>(replacement_data);
    data = RecordReplData{};
    data.valid = true;
    data.recency = curTick();
}

ecg_record::WayState
GraphEcgRecordRP::way(const RecordReplData& data) const
{
    ecg_record::WayState result;
    result.property = data.binding.property;
    result.rrpv = data.rrpv;
    result.recency = data.recency;
    result.grasp_tier = data.graspTier;
    result.state = ecg_record::victimState(
        data.metadata, receiver, replacementEnabled);
    result.deadline = data.metadata.value;
    return result;
}

ReplaceableEntry*
GraphEcgRecordRP::getVictim(const ReplacementCandidates& candidates) const
{
    fatal_if(candidates.empty() || candidates.size() > 64,
             "%s requires between 1 and 64 ways", name());
    for (auto* candidate : candidates) {
        if (!std::static_pointer_cast<RecordReplData>(candidate->replacementData)->valid)
            return candidate;
    }
    if (!replacementEnabled || !receiver.enabled()) {
        return *std::min_element(candidates.begin(), candidates.end(),
            [](const ReplaceableEntry* left, const ReplaceableEntry* right) {
                return std::static_pointer_cast<RecordReplData>(left->replacementData)->recency <
                    std::static_pointer_cast<RecordReplData>(right->replacementData)->recency;
            });
    }
    std::array<ecg_record::WayState, 64> ways{};
    for (std::size_t index = 0; index < candidates.size(); ++index)
        ways[index] = way(*std::static_pointer_cast<RecordReplData>(
            candidates[index]->replacementData));
    std::size_t victim = 0;
    ecg_record::VictimOptions options;
    options.governed_first = governedFirst;
    fatal_if(ecg_record::selectVictim(
        receiver.layout(), ways.data(), candidates.size(),
        receiver.comparisonWatermark(), victim, nullptr, options) !=
            ecg_record::Status::OK, "%s could not select an ECG victim", name());
    return candidates[victim];
}

ecg_record::ApplyResult
GraphEcgRecordRP::applyEcgRecordUpdate(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const ecg_record::CommitUpdate& update)
{
    using Result = ecg_record::ApplyResult;
    if (!replacementEnabled || !receiver.enabled())
        return Result::UNSUPPORTED;
    if (update.context != receiver.context() || update.generation != receiver.generation())
        return Result::INVALID_CONTEXT;
    if (update.secure || update.physical_line % lineSize ||
        !ecg_record::nativePropertyContains(
            configuration, update.property_vaddr,
            update.invalidate ? 1 :
                ecg_record::propertyBytes(propertyDescriptor.kind))) {
        return Result::INVALID_ADDRESS;
    }

    auto data = std::static_pointer_cast<RecordReplData>(replacement_data);
    if (data) {
        if (!data->valid || data->binding.bindVirtual(
                update.physical_line, lineAddress(update.property_vaddr), true) ==
                    ecg_ref32::NativeBindingResult::CONFLICT) {
            return Result::INVALID_ADDRESS;
        }
        data->graspTier = context.classifyGRASP(update.property_vaddr, llcSize, hotFraction);
    }
    return receiver.apply(data ? &data->metadata : nullptr, update);
}

ecg_record::ObservationResult
GraphEcgRecordRP::advanceEcgRecordProgress(
    uint16_t context_id, uint64_t generation, uint64_t sequence)
{
    return receiver.advanceProgress(context_id, generation, sequence);
}

void
GraphEcgRecordRP::invalidateEcgRecordMetadata(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    if (!replacement_data)
        return;
    auto data = std::static_pointer_cast<RecordReplData>(replacement_data);
    data->metadata.clear();
    data->binding.clear();
    data->graspTier = 3;
}

void
GraphEcgRecordRP::finishEcgRecordInvalidation()
{
    receiver.disable();
    configuration = {};
    propertyDescriptor = {};
    context.loaded = false;
}

bool
GraphEcgRecordRP::canAdmitEcgRecordPrefetch(
    const ReplacementCandidates& candidates, uint64_t sequence) const
{
    if (!replacementEnabled)
        return true;
    fatal_if(!receiver.enabled() || candidates.empty() || candidates.size() > 64,
             "%s prefetch admission has an invalid context or set", name());
    std::array<ecg_record::WayState, 64> ways{};
    std::array<bool, 64> valid{};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto data = std::static_pointer_cast<RecordReplData>(
            candidates[index]->replacementData);
        ways[index] = way(*data);
        valid[index] = data->valid;
    }
    bool admit = false;
    fatal_if(ecg_record::canAdmitPrefetch(
        receiver.layout(), ways.data(), valid.data(), candidates.size(), sequence, admit) !=
            ecg_record::Status::OK, "%s prefetch admission failed", name());
    return admit;
}

std::shared_ptr<ReplacementData>
GraphEcgRecordRP::instantiateEntry()
{
    return std::make_shared<RecordReplData>();
}

} // namespace replacement_policy
} // namespace gem5
