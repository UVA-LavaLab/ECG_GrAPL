#ifndef GRAPHBREW_SNIPER_ECG_RECORD_H
#define GRAPHBREW_SNIPER_ECG_RECORD_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ecg_record_sniper_runtime.h"

class Cache;

namespace graphbrew {
namespace sniper {
namespace record {

static constexpr uint32_t kMaxCores = 1024;
static constexpr uint64_t kWorkLayout = 0x45524c59ULL;
static constexpr uint64_t kWorkRecordBase = 0x45524241ULL;
static constexpr uint64_t kWorkPropertyBase = 0x45525041ULL;
static constexpr uint64_t kWorkRecordCount = 0x4552434eULL;
static constexpr uint64_t kWorkVertexCount = 0x4552564eULL;
static constexpr uint64_t kWorkGeneration = 0x4552474eULL;
static constexpr uint64_t kWorkContext = 0x45524354ULL;
static constexpr uint64_t kWorkControl = 0x4552434fULL;
static constexpr uint64_t kWorkPropertyDescriptor = 0x45525044ULL;
static constexpr uint64_t kWorkConfigCommit = 0x4552434dULL;
static constexpr uint64_t kWorkIterationBase = 0x45524942ULL;
static constexpr uint64_t kWorkIterationCommit = 0x45524943ULL;
static constexpr uint64_t kWorkReadArm = 0x45525241ULL;
static constexpr uint64_t kWorkReadWidth = 0x45525257ULL;
static constexpr uint64_t kWorkValueAddress = 0x45525641ULL;
static constexpr uint64_t kWorkValueLow = 0x45524c4fULL;
static constexpr uint64_t kWorkValueHigh = 0x45524849ULL;
static constexpr uint64_t kWorkValueCommit = 0x45525643ULL;
static constexpr uint64_t kWorkConsume = 0x45524353ULL;
static constexpr uint64_t kWorkDrain = 0x45524452ULL;
static constexpr uint64_t kWorkReport = 0x45525250ULL;
static constexpr uint64_t kWorkDeactivate = 0x45524458ULL;
static constexpr uint64_t kWorkPassClose = 0x45525043ULL;
static constexpr uint64_t kWorkInvalidate = 0x45524956ULL;

enum class ConfigurationField : uint8_t {
    LAYOUT,
    RECORD_BASE,
    PROPERTY_BASE,
    RECORD_COUNT,
    VERTEX_COUNT,
    ITERATION_BASE,
    GENERATION,
    CONTEXT,
    CONTROL,
    PROPERTY_DESCRIPTOR,
};

void setConfigurationField(
    uint32_t core_id, ConfigurationField field, uint64_t value);
void commitConfiguration(uint32_t core_id);
void updateIteration(
    uint32_t core_id, uint64_t iteration_base, uint64_t control);
void deactivate(uint32_t core_id);
void closePass(uint32_t core_id);
void invalidateBinding(
    uint32_t core_id, uint64_t sets, uint64_t cycles);

void armRecordRead(uint32_t core_id, uint64_t address, uint32_t bytes);
void observeRecordRead(
    uint32_t core_id, uint64_t address, uint32_t bytes, uint64_t cycle);
void setLoadedAddress(uint32_t core_id, uint64_t address);
void setLoadedChunk(
    uint32_t core_id, uint32_t value, bool high);
void commitLoadedValue(uint32_t core_id, uint32_t bytes);
void consumeRecord(
    uint32_t core_id, uint64_t address, uint64_t cycle);

MemoryAccessKind beginMemoryAccess(
    uint32_t core_id, uint64_t virtual_address, uint64_t physical_line,
    uint32_t bytes, bool read, uint64_t cycle);
void completeMemoryAccess(
    uint32_t core_id, MemoryAccessKind kind, uint64_t cycle);

bool activeObservation(
    uint32_t core_id, uint64_t physical_line, Observation& observation);
bool activeDeadDemand(uint32_t core_id, uint64_t physical_line);
void noteDeadDemandBypass(uint32_t core_id);
ecg_record::ObservationResult observeLine(
    uint32_t core_id, ecg_record::LineMetadata& metadata);
ecg_record::ObservationResult observeOrdinaryLine(
    uint32_t core_id, uint64_t physical_line,
    ecg_record::LineMetadata& metadata);
ecg_record::ApplyResult applyLineUpdate(
    uint32_t core_id, ecg_record::LineMetadata* metadata,
    const ecg_record::CommitUpdate& update);
bool replacementActive(uint32_t core_id);
bool prefetchActive(uint32_t core_id);
bool propertyLine(uint32_t core_id, uint64_t virtual_address);
bool lineClassification(
    uint32_t core_id, uint64_t physical_line,
    uint64_t& virtual_line, bool& property);
uint64_t completedSequence(uint32_t core_id);
bool receiverWatermark(
    uint32_t core_id, uint64_t& sequence,
    ecg_record::Layout& layout);
ecg_record::State victimState(
    uint32_t core_id, const ecg_record::LineMetadata& metadata,
    bool enabled);

void registerLlc(uint32_t core_id, Cache* cache);
uint64_t normalizeCycle(uint32_t core_id, uint64_t observed_cycle);
void serviceUpdates(uint32_t core_id, uint64_t cycle);
bool takeReadyPrefetch(
    uint32_t core_id, uint64_t cycle, PrefetchRequest& request);
std::optional<uint64_t> nextReadyCycle(uint32_t core_id);
bool queuesEmpty(uint32_t core_id);

void notePrefetchPrivateDuplicate(uint32_t core_id);
void notePrefetchLlcDuplicate(uint32_t core_id);
void notePrefetchCompletionPrivateDuplicate(uint32_t core_id);
void notePrefetchCompletionResident(uint32_t core_id);
void notePrefetchIssueAdmissionDrop(uint32_t core_id);
void notePrefetchCompletionAdmissionDrop(uint32_t core_id);
void notePrefetchIssued(uint32_t core_id);
void notePrefetchRetry(
    uint32_t core_id, uint64_t physical_line);
void notePrefetchPrivateLookup(uint32_t core_id);
void notePrefetchLlcLookup(uint32_t core_id);
void notePrefetchIssueAdmissionCheck(uint32_t core_id);
void notePrefetchCompletionAdmissionCheck(uint32_t core_id);
void notePrefetchDemandMerge(uint32_t core_id);
void noteLookupCycles(uint32_t core_id, uint64_t cycles);
void noteDrainCycles(uint32_t core_id, uint64_t cycles);
void notePrefetchTranslation(uint32_t core_id, bool fault);
void notePrefetchTranslationBypass(uint32_t core_id);
void notePrefetchFill(uint32_t core_id, uint64_t latency_cycles);
void beginPrefetchIssue(
    uint32_t core_id, uint64_t physical_line,
    uint64_t sequence, uint64_t cycle, uint64_t virtual_address);
bool prefetchInFlight(
    uint32_t core_id, uint64_t physical_line,
    uint64_t& sequence, uint64_t& issue_cycle);
void clearPrefetchInFlight(uint32_t core_id);

void report(uint32_t core_id);
bool clean(uint32_t core_id);
uint64_t lookupLatencyCycles();
uint64_t drainLimitCycles();
uint64_t handleMagic(
    uint32_t core_id, uint64_t command, uint64_t argument,
    uint64_t cycle);
bool isMagicCommand(uint64_t command);

}  // namespace record
}  // namespace sniper
}  // namespace graphbrew

#endif
