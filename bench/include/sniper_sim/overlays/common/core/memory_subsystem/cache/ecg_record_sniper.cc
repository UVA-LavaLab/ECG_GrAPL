#include "ecg_record_sniper.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "cache.h"
#include "config.hpp"
#include "graph_cache_context_sniper.h"
#include "simulator.h"

namespace graphbrew {
namespace sniper {
namespace record {

namespace {

struct CoreState {
    Runtime runtime;
    ecg_record::NativeConfiguration staging;
    Cache* llc = nullptr;
    uint64_t prefetch_line = 0;
    uint64_t prefetch_sequence = 0;
    uint64_t prefetch_issue_cycle = 0;
    uint64_t prefetch_virtual_line = 0;
    uint64_t memory_virtual_line = 0;
    uint64_t memory_physical_line = 0;
    bool memory_scope = false;
    uint64_t pending_read_address = 0;
    uint64_t pending_iteration_base = 0;
    uint64_t last_observed_cycle = 0;
    uint64_t drain_cycles_charged = 0;
    bool prefetch_in_flight = false;
    bool configured = false;
};

std::array<std::unique_ptr<CoreState>, kMaxCores>& states()
{
    static std::array<std::unique_ptr<CoreState>, kMaxCores> value;
    return value;
}

std::mutex& stateMutex()
{
    static std::mutex mutex;
    return mutex;
}

CoreState&
state(uint32_t core_id)
{
    if (core_id >= kMaxCores) {
        std::fprintf(stderr,
            "[FATAL] SNIPER ECG record core %u exceeds %u\n",
            core_id, kMaxCores);
        std::abort();
    }
    std::lock_guard<std::mutex> lock(stateMutex());
    if (!states()[core_id])
        states()[core_id] = std::make_unique<CoreState>();
    return *states()[core_id];
}

uint64_t envUnsigned(
    const char* name, uint64_t fallback,
    uint64_t minimum, uint64_t maximum)
{
    const char* value = std::getenv(name);
    if (!value || !value[0])
        return fallback;
    for (const char* digit = value; *digit; ++digit) {
        if (*digit < '0' || *digit > '9') {
            std::fprintf(stderr, "[FATAL] %s is not an unsigned integer\n", name);
            std::abort();
        }
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        std::fprintf(stderr,
            "[FATAL] %s must be in [%llu,%llu], got %s\n",
            name,
            static_cast<unsigned long long>(minimum),
            static_cast<unsigned long long>(maximum), value);
        std::abort();
    }
    return parsed;
}

void require(ecg_record::Status status, const char* operation)
{
    if (status == ecg_record::Status::OK)
        return;
    std::fprintf(stderr,
        "[FATAL] SNIPER ECG record %s failed: %s\n",
        operation, ecg_record::statusName(status));
    std::abort();
}

ecg_record::Mechanism configuredMechanism()
{
    ecg_record::Mechanism mechanism;
    const ecg_record::Status status = ecg_record::parseMechanismName(
        std::getenv("SNIPER_ECG_RECORD_MECHANISM"), mechanism);
    require(status, "mechanism");
    return mechanism;
}

}  // namespace

void
setConfigurationField(
    uint32_t core_id, ConfigurationField field, uint64_t value)
{
    CoreState& core = state(core_id);
    switch (field) {
      case ConfigurationField::LAYOUT:
        core.staging.layout_descriptor = value; break;
      case ConfigurationField::RECORD_BASE:
        core.staging.record_base = value; break;
      case ConfigurationField::PROPERTY_BASE:
        core.staging.property_base = value; break;
      case ConfigurationField::RECORD_COUNT:
        core.staging.record_count = value; break;
      case ConfigurationField::VERTEX_COUNT:
        core.staging.vertex_count = value; break;
      case ConfigurationField::ITERATION_BASE:
        core.staging.iteration_base = value; break;
      case ConfigurationField::GENERATION:
        core.staging.generation = value; break;
      case ConfigurationField::CONTEXT:
        core.staging.context = value; break;
      case ConfigurationField::CONTROL:
        core.staging.control = value; break;
      case ConfigurationField::PROPERTY_DESCRIPTOR:
        core.staging.property_descriptor = value; break;
    }
}

void
commitConfiguration(uint32_t core_id)
{
    CoreState& core = state(core_id);
    const uint64_t total_cores =
        Sim()->getCfg()->getInt("general/total_cores");
    if (total_cores != 1) {
        std::fprintf(stderr,
            "[FATAL] Sniper ECG record currently requires one simulated core\n");
        std::abort();
    }
    if ((core.staging.control &
            ecg_record::kNativeManagedPasses) != 0) {
        auto& context = graphbrew::sniper::globalContext();
        ecg_record::PropertyDescriptor property;
        ecg_record::Layout layout;
        uint64_t extent = 0, record_bytes = 0, property_upper = 0,
                 record_upper = 0;
        bool property_match = false, carrier_match = false;
        if (!context.loaded ||
            ecg_record::unpackProperty(
                core.staging.property_descriptor, property) !=
                ecg_record::Status::OK ||
            ecg_record::unpackLayout(
                core.staging.layout_descriptor, layout) !=
                ecg_record::Status::OK ||
            ecg_record::propertyExtent(
                property, core.staging.vertex_count, extent) !=
                ecg_record::Status::OK ||
            !ecg_record::checkedMultiply(
                core.staging.record_count, layout.record_bytes,
                record_bytes) ||
            !ecg_record::checkedAdd(
                core.staging.property_base, extent, property_upper) ||
            !ecg_record::checkedAdd(
                core.staging.record_base, record_bytes, record_upper) ||
            context.topology.num_vertices != core.staging.vertex_count) {
            require(ecg_record::Status::INVALID_LAYOUT,
                    "managed-sideband");
        }
        for (uint32_t index = 0; index < context.num_regions; ++index) {
            const auto& region = context.regions[index];
            property_match |=
                region.base_address == core.staging.property_base &&
                region.upper_bound >= property_upper &&
                region.num_elements == core.staging.vertex_count &&
                region.elem_size ==
                    ecg_record::propertyBytes(property.kind) &&
                region.stride_bytes == property.stride_bytes;
        }
        for (uint32_t index = 0;
             index < context.num_edge_regions; ++index) {
            const auto& region = context.edge_regions[index];
            carrier_match |=
                region.base_address == core.staging.record_base &&
                region.upper_bound >= record_upper;
        }
        if (!property_match || !carrier_match)
            require(ecg_record::Status::INVALID_LAYOUT,
                    "managed-sideband");
    }
    const uint64_t update_latency = envUnsigned(
        "SNIPER_ECG_RECORD_UPDATE_LATENCY", 8, 8, 1 << 20);
    const uint64_t capture_width = envUnsigned(
        "SNIPER_ECG_RECORD_CAPTURE_WIDTH", 1, 1, 16);
    const uint64_t prefetch_capacity = envUnsigned(
        "SNIPER_ECG_RECORD_PREFETCH_QUEUE", 8, 1, 256);
    const uint64_t prefetch_latency = envUnsigned(
        "SNIPER_ECG_RECORD_PREFETCH_LATENCY", 8, 1, 1 << 20);
    const uint64_t line_bytes = envUnsigned(
        "SNIPER_ECG_RECORD_LINE_BYTES", 64, 4, 4096);
    if (line_bytes != 64) {
        std::fprintf(stderr,
            "[FATAL] SNIPER_ECG_RECORD_LINE_BYTES must currently be 64\n");
        std::abort();
    }
    const uint64_t nuca_kb =
        Sim()->getCfg()->getInt("perf_model/nuca/cache_size");
    const uint64_t nuca_ways =
        Sim()->getCfg()->getInt("perf_model/nuca/associativity");
    const String nuca_hash =
        Sim()->getCfg()->getString("perf_model/nuca/address_hash");
    uint64_t nuca_bytes = 0;
    if (!ecg_record::checkedMultiply(nuca_kb, 1024, nuca_bytes) ||
        nuca_ways == 0 ||
        nuca_bytes % (nuca_ways * line_bytes) != 0) {
        std::fprintf(stderr,
            "[FATAL] Sniper ECG record NUCA geometry is not integral\n");
        std::abort();
    }
    const uint64_t nuca_sets =
        nuca_bytes / (nuca_ways * line_bytes);
    const bool power_two_sets =
        nuca_sets != 0 && (nuca_sets & (nuca_sets - 1)) == 0;
    if (!power_two_sets && nuca_hash != "mod") {
        std::fprintf(stderr,
            "[FATAL] Sniper ECG record non-power-of-two NUCA geometry "
            "requires perf_model/nuca/address_hash=mod\n");
        std::abort();
    }
    require(core.runtime.configure(
        core.staging, configuredMechanism(),
        update_latency, capture_width,
        prefetch_capacity, prefetch_latency,
        line_bytes), "configuration");
    core.configured = true;
    const ecg_record::Layout& layout = core.runtime.layout();
    ecg_record::PropertyDescriptor property;
    require(ecg_record::unpackProperty(
        core.staging.property_descriptor, property), "property-descriptor");
    std::fprintf(stderr,
        "[SNIPER-ECG-RECORD-CONFIG core=%u mechanism=%s "
        "record_bytes=%u id_bits=%u metadata_bits=%u horizon_bits=%u "
        "exponent_bits=%u mantissa_bits=%u sequence_bits=%u "
        "deadline_bits=%u state_encoding=joint-distance "
        "prefetch_selection=record-window record_count=%llu vertex_count=%llu "
        "property_descriptor=%llu property_kind=%s property_stride=%u "
        "traversal_mode=%s "
        "context=%llu generation=%llu update_latency=%llu "
        "capture_width=%llu update_output_width=1 prefetch_queue=%llu "
        "prefetch_latency=%llu line_bytes=%llu lookup_latency=%llu "
        "drain_max_cycles=%llu dead_miss_bypass=llc-request-scoped "
        "window_transport=software-word-bank window_entries=16 "
        "window_entry_bits=257 lookup_model=serialized-cycle-charge "
        "issue_lookup_levels=up-to-4 completion_lookup_levels=4 nuca_sets=%llu "
        "nuca_indexing=%s prefetch_request_model=llc-only-read "
        "update_link_model=bounded-completion-corroboration "
        "core_scope=single-core]\n",
        core_id, ecg_record::mechanismName(core.runtime.mechanism()),
        layout.record_bytes, layout.id_bits, layout.metadata_bits,
        layout.horizon_bits, layout.exponent_bits,
        layout.mantissa_bits, layout.sequence_bits,
        layout.deadline_bits,
        static_cast<unsigned long long>(core.staging.record_count),
        static_cast<unsigned long long>(core.staging.vertex_count),
        static_cast<unsigned long long>(
            core.staging.property_descriptor),
        ecg_record::propertyKindName(property.kind),
        property.stride_bytes,
        ecg_record::traversalName(property.traversal),
        static_cast<unsigned long long>(core.staging.context),
        static_cast<unsigned long long>(core.staging.generation),
        static_cast<unsigned long long>(update_latency),
        static_cast<unsigned long long>(capture_width),
        static_cast<unsigned long long>(prefetch_capacity),
        static_cast<unsigned long long>(prefetch_latency),
        static_cast<unsigned long long>(line_bytes),
        static_cast<unsigned long long>(lookupLatencyCycles()),
        static_cast<unsigned long long>(drainLimitCycles()),
        static_cast<unsigned long long>(nuca_sets),
        nuca_hash.c_str());
}

void
updateIteration(
    uint32_t core_id, uint64_t iteration_base, uint64_t control)
{
    CoreState& core = state(core_id);
    if ((control & ~ecg_record::kNativeControlMask) != 0 ||
        (control & ecg_record::kNativeEnable) == 0) {
        require(ecg_record::Status::INVALID_LAYOUT, "iteration-control");
    }
    require(core.runtime.updateIteration(
        iteration_base,
        (control & ecg_record::kNativeHasNext) != 0,
        (control & ecg_record::kNativeManagedPasses) != 0), "iteration");
}

void deactivate(uint32_t core_id)
{ require(state(core_id).runtime.deactivate(), "deactivate"); }

void closePass(uint32_t core_id)
{ require(state(core_id).runtime.closePass(), "pass-close"); }

void
invalidateBinding(
    uint32_t core_id, uint64_t sets, uint64_t cycles)
{
    require(state(core_id).runtime.invalidateBinding(
        sets, cycles), "invalidate");
}

void armRecordRead(uint32_t core_id, uint64_t address, uint32_t bytes)
{ require(state(core_id).runtime.armRecordRead(address, bytes), "record-arm"); }

void
observeRecordRead(
    uint32_t core_id, uint64_t address, uint32_t bytes, uint64_t cycle)
{
    const ecg_record::Status status =
        state(core_id).runtime.observeRecordRead(address, bytes, cycle);
    if (status != ecg_record::Status::NOT_READY)
        require(status, "record-read");
}

void setLoadedAddress(uint32_t core_id, uint64_t address)
{ require(state(core_id).runtime.setLoadedAddress(address), "loaded-address"); }

void setLoadedChunk(uint32_t core_id, uint32_t value, bool high)
{ require(state(core_id).runtime.setLoadedChunk(value, high), "loaded-chunk"); }

void commitLoadedValue(uint32_t core_id, uint32_t bytes)
{ require(state(core_id).runtime.commitLoadedValue(bytes), "loaded-commit"); }

void consumeRecord(
    uint32_t core_id, uint64_t address, uint64_t cycle)
{
    require(
        state(core_id).runtime.consumeRecord(address, cycle),
        "record-consume");
}

MemoryAccessKind
beginMemoryAccess(
    uint32_t core_id, uint64_t virtual_address, uint64_t physical_line,
    uint32_t bytes, bool read, uint64_t cycle)
{
    CoreState& core = state(core_id);
    core.memory_scope = core.runtime.active();
    core.memory_virtual_line = virtual_address - virtual_address % 64;
    core.memory_physical_line = physical_line;
    const MemoryAccessKind kind = core.runtime.beginMemoryAccess(
        virtual_address, physical_line, bytes, read, cycle);
    return kind;
}

void
completeMemoryAccess(
    uint32_t core_id, MemoryAccessKind kind, uint64_t cycle)
{
    require(state(core_id).runtime.completeMemoryAccess(kind, cycle),
            "memory-complete");
    state(core_id).memory_scope = false;
}

ecg_record::ObservationResult
observeOrdinaryLine(
    uint32_t core_id, uint64_t physical_line,
    ecg_record::LineMetadata& metadata)
{
    return state(core_id).runtime.observeOrdinaryLine(
        physical_line, metadata);
}

bool
activeObservation(
    uint32_t core_id, uint64_t physical_line, Observation& observation)
{
    return state(core_id).runtime.activeObservation(
        physical_line, observation);
}

bool
activeDeadDemand(uint32_t core_id, uint64_t physical_line)
{
    return state(core_id).runtime.activeDeadDemand(physical_line);
}

void noteDeadDemandBypass(uint32_t core_id)
{ state(core_id).runtime.noteDeadDemandBypass(); }

ecg_record::ObservationResult
observeLine(uint32_t core_id, ecg_record::LineMetadata& metadata)
{
    return state(core_id).runtime.observeLine(metadata);
}

ecg_record::ApplyResult
applyLineUpdate(
    uint32_t core_id, ecg_record::LineMetadata* metadata,
    const ecg_record::CommitUpdate& update)
{
    return state(core_id).runtime.applyUpdate(metadata, update);
}

bool
replacementActive(uint32_t core_id)
{
    const CoreState& core = state(core_id);
    return core.configured && core.runtime.active() &&
        (core.runtime.mechanism() == ecg_record::Mechanism::REPLACEMENT ||
         core.runtime.mechanism() ==
             ecg_record::Mechanism::REPLACEMENT_PREFETCH);
}

bool
prefetchActive(uint32_t core_id)
{
    const CoreState& core = state(core_id);
    return core.configured && core.runtime.active() &&
        (core.runtime.mechanism() == ecg_record::Mechanism::PREFETCH ||
         core.runtime.mechanism() ==
             ecg_record::Mechanism::REPLACEMENT_PREFETCH);
}

bool propertyLine(uint32_t core_id, uint64_t virtual_address)
{ return state(core_id).runtime.propertyLine(virtual_address); }

uint64_t completedSequence(uint32_t core_id)
{ return state(core_id).runtime.completedSequence(); }

bool
lineClassification(
    uint32_t core_id, uint64_t physical_line,
    uint64_t& virtual_line, bool& property)
{
    const CoreState& core = state(core_id);
    if (!core.runtime.active())
        return false;
    if (core.memory_scope && core.memory_physical_line == physical_line) {
        virtual_line = core.memory_virtual_line;
    } else if (core.prefetch_in_flight && core.prefetch_line == physical_line) {
        virtual_line = core.prefetch_virtual_line;
    } else if (!Sim()->getCfg()->getBool("general/translation_enabled")) {
        virtual_line = physical_line;
    } else {
        return false;
    }
    property = core.runtime.propertyLine(virtual_line);
    return true;
}

bool
receiverWatermark(
    uint32_t core_id, uint64_t& sequence,
    ecg_record::Layout& layout)
{
    CoreState& core = state(core_id);
    if (!core.runtime.active())
        return false;
    layout = core.runtime.layout();
    if (!replacementActive(core_id) ||
        !core.runtime.watermarkValid())
        return false;
    sequence = core.runtime.watermark();
    return true;
}

ecg_record::State
victimState(
    uint32_t core_id, const ecg_record::LineMetadata& metadata,
    bool enabled)
{
    return state(core_id).runtime.victimState(metadata, enabled);
}

void registerLlc(uint32_t core_id, Cache* cache)
{ state(core_id).llc = cache; }

uint64_t
normalizeCycle(uint32_t core_id, uint64_t observed_cycle)
{
    CoreState& core = state(core_id);
    core.last_observed_cycle = std::max(
        core.last_observed_cycle, observed_cycle);
    return core.last_observed_cycle;
}

void
serviceUpdates(uint32_t core_id, uint64_t cycle)
{
    CoreState& core = state(core_id);
    if (!core.configured)
        return;
    const ecg_record::PopResult result =
        core.runtime.popReadyUpdate(cycle);
    if (result.status != ecg_record::PopStatus::POPPED)
        return;
    if (replacementActive(core_id) && core.llc) {
        core.llc->applyEcgRecordUpdate(result.ready.update);
    } else {
        core.runtime.applyUpdate(nullptr, result.ready.update);
    }
}

bool
takeReadyPrefetch(
    uint32_t core_id, uint64_t cycle, PrefetchRequest& request)
{
    return state(core_id).runtime.takeReadyPrefetch(
        cycle, request);
}

std::optional<uint64_t> nextReadyCycle(uint32_t core_id)
{ return state(core_id).runtime.nextReadyCycle(); }

bool queuesEmpty(uint32_t core_id)
{
    CoreState& core = state(core_id);
    return core.runtime.updateQueueEmpty() &&
        core.runtime.prefetchQueueEmpty();
}

void notePrefetchPrivateDuplicate(uint32_t core_id)
{ state(core_id).runtime.notePrefetchPrivateDuplicate(); }
void notePrefetchLlcDuplicate(uint32_t core_id)
{ state(core_id).runtime.notePrefetchLlcDuplicate(); }
void notePrefetchCompletionPrivateDuplicate(uint32_t core_id)
{ state(core_id).runtime.notePrefetchCompletionPrivateDuplicate(); }
void notePrefetchCompletionResident(uint32_t core_id)
{ state(core_id).runtime.notePrefetchCompletionResident(); }
void notePrefetchIssueAdmissionDrop(uint32_t core_id)
{ state(core_id).runtime.notePrefetchIssueAdmissionDrop(); }
void notePrefetchCompletionAdmissionDrop(uint32_t core_id)
{ state(core_id).runtime.notePrefetchCompletionAdmissionDrop(); }
void notePrefetchIssued(uint32_t core_id)
{ state(core_id).runtime.notePrefetchIssued(); }
void
notePrefetchRetry(uint32_t core_id, uint64_t physical_line)
{
    uint64_t sequence = 0, cycle = 0;
    if (prefetchInFlight(
            core_id, physical_line, sequence, cycle))
        state(core_id).runtime.notePrefetchRetry();
}
void notePrefetchPrivateLookup(uint32_t core_id)
{ state(core_id).runtime.notePrefetchPrivateLookup(); }
void notePrefetchLlcLookup(uint32_t core_id)
{ state(core_id).runtime.notePrefetchLlcLookup(); }
void notePrefetchIssueAdmissionCheck(uint32_t core_id)
{ state(core_id).runtime.notePrefetchIssueAdmissionCheck(); }
void notePrefetchCompletionAdmissionCheck(uint32_t core_id)
{ state(core_id).runtime.notePrefetchCompletionAdmissionCheck(); }
void notePrefetchDemandMerge(uint32_t core_id)
{ state(core_id).runtime.notePrefetchDemandMerge(); }
void noteLookupCycles(uint32_t core_id, uint64_t cycles)
{ state(core_id).runtime.noteLookupCycles(cycles); }
void
noteDrainCycles(uint32_t core_id, uint64_t cycles)
{
    CoreState& core = state(core_id);
    uint64_t total = 0;
    if (!ecg_record::checkedAdd(
            core.drain_cycles_charged, cycles, total)) {
        std::fprintf(stderr,
            "[FATAL] SNIPER ECG record drain accounting overflow\n");
        std::abort();
    }
    core.drain_cycles_charged = total;
}
void notePrefetchTranslation(uint32_t core_id, bool fault)
{ state(core_id).runtime.notePrefetchTranslation(fault); }
void notePrefetchTranslationBypass(uint32_t core_id)
{ state(core_id).runtime.notePrefetchTranslationBypass(); }
void notePrefetchFill(uint32_t core_id, uint64_t latency_cycles)
{ state(core_id).runtime.notePrefetchFill(latency_cycles); }

void
beginPrefetchIssue(
    uint32_t core_id, uint64_t physical_line,
    uint64_t sequence, uint64_t cycle, uint64_t virtual_address)
{
    CoreState& core = state(core_id);
    if (core.prefetch_in_flight) {
        std::fprintf(stderr,
            "[FATAL] SNIPER ECG record overlapping prefetch issue\n");
        std::abort();
    }
    core.prefetch_line = physical_line;
    core.prefetch_sequence = sequence;
    core.prefetch_issue_cycle = cycle;
    core.prefetch_virtual_line = virtual_address - virtual_address % 64;
    core.prefetch_in_flight = true;
}

bool
prefetchInFlight(
    uint32_t core_id, uint64_t physical_line,
    uint64_t& sequence, uint64_t& issue_cycle)
{
    CoreState& core = state(core_id);
    if (!core.prefetch_in_flight ||
        core.prefetch_line != physical_line)
        return false;
    sequence = core.prefetch_sequence;
    issue_cycle = core.prefetch_issue_cycle;
    return true;
}

void
clearPrefetchInFlight(uint32_t core_id)
{
    CoreState& core = state(core_id);
    core.prefetch_in_flight = false;
    core.prefetch_line = 0;
    core.prefetch_sequence = 0;
    core.prefetch_issue_cycle = 0;
    core.prefetch_virtual_line = 0;
}

bool clean(uint32_t core_id)
{
    CoreState& core = state(core_id);
    return core.runtime.clean() && !core.prefetch_in_flight;
}

uint64_t lookupLatencyCycles()
{
    return envUnsigned(
        "SNIPER_ECG_RECORD_LOOKUP_LATENCY", 1, 1, 1 << 20);
}

uint64_t drainLimitCycles()
{
    return envUnsigned(
        "SNIPER_ECG_RECORD_DRAIN_MAX_CYCLES",
        1 << 20, 8, uint64_t{1} << 40);
}

void
report(uint32_t core_id)
{
    CoreState& core = state(core_id);
    const Counters& c = core.runtime.counters();
    ecg_record::PropertyDescriptor property;
    require(ecg_record::unpackProperty(
        core.runtime.configuration().property_descriptor, property),
        "report-property");
    const uint64_t pending_updates = core.runtime.updatePending();
    const uint64_t pending_prefetches = core.runtime.prefetchPending();
    const bool update_accounting =
        c.generated_updates ==
            c.enqueued_updates + c.coalesced_updates &&
        c.enqueued_updates ==
            c.delivered_updates + pending_updates;
    const bool pass_accounting =
        (core.runtime.configuration().control &
            ecg_record::kNativeManagedPasses) == 0 ||
        c.consumed_records + c.skipped_positions ==
            c.structural_positions;
    const bool prefetch_accounting =
        c.prefetch_enqueued ==
            c.prefetch_issued + c.prefetch_private_duplicates +
            c.prefetch_llc_duplicates +
            c.prefetch_issue_admission_drops +
            pending_prefetches &&
        c.prefetch_translation_requests +
            c.prefetch_translation_bypasses ==
            c.prefetch_enqueued - pending_prefetches &&
        c.prefetch_issued ==
            c.prefetch_fills +
            c.prefetch_completion_private_duplicates +
            c.prefetch_completion_resident +
            c.prefetch_completion_admission_drops +
            c.prefetch_demand_merges +
            (core.prefetch_in_flight ? 1u : 0u);
    std::fprintf(stderr,
        "[SNIPER-ECG-RECORD mechanism=%s property_kind=%s "
        "property_bytes=%u property_stride=%u traversal_mode=%s "
        "record_reads=%llu "
        "record_read_bytes=%llu loaded_values=%llu consumed_records=%llu "
        "property_accesses=%llu observations=%llu passes=%llu "
        "structural_positions=%llu skipped_positions=%llu "
        "ordinary_invalidations=%llu rebinds=%llu "
        "invalidation_sets=%llu invalidation_cycles=%llu "
        "dead_demand_bypasses=%llu generated_updates=%llu "
        "enqueued_updates=%llu coalesced_updates=%llu "
        "delivered_updates=%llu minimum_update_latency=%llu applied_updates=%llu stale_updates=%llu "
        "expired_updates=%llu not_resident_updates=%llu "
        "prefetch_candidates=%llu prefetch_enqueued=%llu "
        "prefetch_pending_duplicates=%llu prefetch_queue_full=%llu "
        "prefetch_issued=%llu prefetch_request_bytes=%llu "
        "prefetch_retry_waits=%llu prefetch_private_lookups=%llu "
        "prefetch_llc_lookups=%llu prefetch_issue_admission_checks=%llu "
        "prefetch_completion_admission_checks=%llu "
        "prefetch_demand_merges=%llu "
        "prefetch_private_duplicates=%llu "
        "prefetch_llc_duplicates=%llu "
        "prefetch_completion_private_duplicates=%llu "
        "prefetch_completion_resident=%llu "
        "prefetch_issue_admission_drops=%llu "
        "prefetch_completion_admission_drops=%llu prefetch_fills=%llu "
        "prefetch_fill_bytes=%llu "
        "prefetch_translation_requests=%llu "
        "prefetch_translation_bypasses=%llu prefetch_translation_faults=%llu "
        "prefetch_latency_cycles=%llu max_update_occupancy=%llu "
        "lookup_cycles_charged=%llu drain_cycles_charged=%llu "
        "max_prefetch_occupancy=%llu "
        "pending_updates=%llu "
        "pending_prefetches=%llu errors=%llu accounting=%u clean=%u "
        "timing_scope=modeled-corroboration]\n",
        ecg_record::mechanismName(core.runtime.mechanism()),
        ecg_record::propertyKindName(property.kind),
        ecg_record::propertyBytes(property.kind),
        property.stride_bytes,
        ecg_record::traversalName(property.traversal),
        static_cast<unsigned long long>(c.record_reads),
        static_cast<unsigned long long>(c.record_read_bytes),
        static_cast<unsigned long long>(c.loaded_values),
        static_cast<unsigned long long>(c.consumed_records),
        static_cast<unsigned long long>(c.property_accesses),
        static_cast<unsigned long long>(c.observations),
        static_cast<unsigned long long>(c.passes),
        static_cast<unsigned long long>(c.structural_positions),
        static_cast<unsigned long long>(c.skipped_positions),
        static_cast<unsigned long long>(c.ordinary_invalidations),
        static_cast<unsigned long long>(c.rebinds),
        static_cast<unsigned long long>(c.invalidation_sets),
        static_cast<unsigned long long>(c.invalidation_cycles),
        static_cast<unsigned long long>(c.dead_demand_bypasses),
        static_cast<unsigned long long>(c.generated_updates),
        static_cast<unsigned long long>(c.enqueued_updates),
        static_cast<unsigned long long>(c.coalesced_updates),
        static_cast<unsigned long long>(c.delivered_updates),
        static_cast<unsigned long long>(
            c.delivered_updates ? c.minimum_update_latency : 0),
        static_cast<unsigned long long>(c.applied_updates),
        static_cast<unsigned long long>(c.stale_updates),
        static_cast<unsigned long long>(c.expired_updates),
        static_cast<unsigned long long>(c.not_resident_updates),
        static_cast<unsigned long long>(c.prefetch_candidates),
        static_cast<unsigned long long>(c.prefetch_enqueued),
        static_cast<unsigned long long>(c.prefetch_pending_duplicates),
        static_cast<unsigned long long>(c.prefetch_queue_full),
        static_cast<unsigned long long>(c.prefetch_issued),
        static_cast<unsigned long long>(c.prefetch_request_bytes),
        static_cast<unsigned long long>(c.prefetch_retry_waits),
        static_cast<unsigned long long>(c.prefetch_private_lookups),
        static_cast<unsigned long long>(c.prefetch_llc_lookups),
        static_cast<unsigned long long>(
            c.prefetch_issue_admission_checks),
        static_cast<unsigned long long>(
            c.prefetch_completion_admission_checks),
        static_cast<unsigned long long>(c.prefetch_demand_merges),
        static_cast<unsigned long long>(c.prefetch_private_duplicates),
        static_cast<unsigned long long>(c.prefetch_llc_duplicates),
        static_cast<unsigned long long>(
            c.prefetch_completion_private_duplicates),
        static_cast<unsigned long long>(c.prefetch_completion_resident),
        static_cast<unsigned long long>(c.prefetch_issue_admission_drops),
        static_cast<unsigned long long>(
            c.prefetch_completion_admission_drops),
        static_cast<unsigned long long>(c.prefetch_fills),
        static_cast<unsigned long long>(c.prefetch_fill_bytes),
        static_cast<unsigned long long>(c.prefetch_translation_requests),
        static_cast<unsigned long long>(c.prefetch_translation_bypasses),
        static_cast<unsigned long long>(c.prefetch_translation_faults),
        static_cast<unsigned long long>(c.prefetch_latency_cycles),
        static_cast<unsigned long long>(c.max_update_occupancy),
        static_cast<unsigned long long>(c.lookup_cycles_charged),
        static_cast<unsigned long long>(core.drain_cycles_charged),
        static_cast<unsigned long long>(c.max_prefetch_occupancy),
        static_cast<unsigned long long>(
            pending_updates),
        static_cast<unsigned long long>(
            pending_prefetches),
        static_cast<unsigned long long>(c.errors),
        update_accounting && prefetch_accounting &&
            pass_accounting ? 1u : 0u,
        core.runtime.clean() ? 1u : 0u);
}

uint64_t
handleMagic(
    uint32_t core_id, uint64_t command, uint64_t argument,
    uint64_t cycle)
{
    CoreState& core = state(core_id);
    if (command == kWorkLayout)
        setConfigurationField(
            core_id, ConfigurationField::LAYOUT, argument);
    else if (command == kWorkRecordBase)
        setConfigurationField(
            core_id, ConfigurationField::RECORD_BASE, argument);
    else if (command == kWorkPropertyBase)
        setConfigurationField(
            core_id, ConfigurationField::PROPERTY_BASE, argument);
    else if (command == kWorkRecordCount)
        setConfigurationField(
            core_id, ConfigurationField::RECORD_COUNT, argument);
    else if (command == kWorkVertexCount)
        setConfigurationField(
            core_id, ConfigurationField::VERTEX_COUNT, argument);
    else if (command == kWorkGeneration)
        setConfigurationField(
            core_id, ConfigurationField::GENERATION, argument);
    else if (command == kWorkContext)
        setConfigurationField(
            core_id, ConfigurationField::CONTEXT, argument);
    else if (command == kWorkControl)
        setConfigurationField(
            core_id, ConfigurationField::CONTROL, argument);
    else if (command == kWorkPropertyDescriptor)
        setConfigurationField(
            core_id, ConfigurationField::PROPERTY_DESCRIPTOR, argument);
    else if (command == kWorkConfigCommit)
        commitConfiguration(core_id);
    else if (command == kWorkIterationBase)
        core.pending_iteration_base = argument;
    else if (command == kWorkIterationCommit)
        updateIteration(core_id, core.pending_iteration_base, argument);
    else if (command == kWorkReadArm)
        core.pending_read_address = argument;
    else if (command == kWorkReadWidth)
        armRecordRead(
            core_id, core.pending_read_address,
            static_cast<uint32_t>(argument));
    else if (command == kWorkValueAddress)
        setLoadedAddress(core_id, argument);
    else if (command == kWorkValueLow)
        setLoadedChunk(core_id, static_cast<uint32_t>(argument), false);
    else if (command == kWorkValueHigh)
        setLoadedChunk(core_id, static_cast<uint32_t>(argument), true);
    else if (command == kWorkValueCommit)
        commitLoadedValue(core_id, static_cast<uint32_t>(argument));
    else if (command == kWorkConsume)
        consumeRecord(core_id, argument, cycle);
    else if (command == kWorkReport)
        report(core_id);
    else if (command == kWorkDeactivate)
        deactivate(core_id);
    else if (command == kWorkPassClose)
        closePass(core_id);
    else
        return 1;
    return 0;
}

bool
isMagicCommand(uint64_t command)
{
    switch (command) {
      case kWorkLayout:
      case kWorkRecordBase:
      case kWorkPropertyBase:
      case kWorkRecordCount:
      case kWorkVertexCount:
      case kWorkGeneration:
      case kWorkContext:
      case kWorkControl:
      case kWorkPropertyDescriptor:
      case kWorkConfigCommit:
      case kWorkIterationBase:
      case kWorkIterationCommit:
      case kWorkReadArm:
      case kWorkReadWidth:
      case kWorkValueAddress:
      case kWorkValueLow:
      case kWorkValueHigh:
      case kWorkValueCommit:
      case kWorkConsume:
      case kWorkReport:
      case kWorkDeactivate:
      case kWorkPassClose:
      case kWorkInvalidate:
        return true;
      default:
        return false;
    }
}

}  // namespace record
}  // namespace sniper
}  // namespace graphbrew
