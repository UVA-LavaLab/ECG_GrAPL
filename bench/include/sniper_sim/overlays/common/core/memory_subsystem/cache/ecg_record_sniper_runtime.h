#ifndef GRAPHBREW_SNIPER_ECG_RECORD_RUNTIME_H
#define GRAPHBREW_SNIPER_ECG_RECORD_RUNTIME_H

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "ecg_record_sniper_compat.h"

namespace graphbrew {
namespace sniper {
namespace record {

static constexpr std::size_t kBankEntries =
    ecg_record::RecordWindow::kRecords;

struct Counters {
    uint64_t record_reads = 0;
    uint64_t record_read_bytes = 0;
    uint64_t loaded_values = 0;
    uint64_t consumed_records = 0;
    uint64_t property_accesses = 0;
    uint64_t observations = 0;
    uint64_t generated_updates = 0;
    uint64_t enqueued_updates = 0;
    uint64_t coalesced_updates = 0;
    uint64_t delivered_updates = 0;
    uint64_t minimum_update_latency = UINT64_MAX;
    uint64_t applied_updates = 0;
    uint64_t stale_updates = 0;
    uint64_t expired_updates = 0;
    uint64_t not_resident_updates = 0;
    uint64_t dead_demand_bypasses = 0;
    uint64_t prefetch_candidates = 0;
    uint64_t prefetch_enqueued = 0;
    uint64_t prefetch_pending_duplicates = 0;
    uint64_t prefetch_queue_full = 0;
    uint64_t prefetch_issued = 0;
    uint64_t prefetch_request_bytes = 0;
    uint64_t prefetch_retry_waits = 0;
    uint64_t prefetch_private_lookups = 0;
    uint64_t prefetch_llc_lookups = 0;
    uint64_t prefetch_issue_admission_checks = 0;
    uint64_t prefetch_completion_admission_checks = 0;
    uint64_t prefetch_demand_merges = 0;
    uint64_t lookup_cycles_charged = 0;
    uint64_t prefetch_private_duplicates = 0;
    uint64_t prefetch_llc_duplicates = 0;
    uint64_t prefetch_completion_private_duplicates = 0;
    uint64_t prefetch_completion_resident = 0;
    uint64_t prefetch_issue_admission_drops = 0;
    uint64_t prefetch_completion_admission_drops = 0;
    uint64_t prefetch_fills = 0;
    uint64_t prefetch_fill_bytes = 0;
    uint64_t prefetch_translation_requests = 0;
    uint64_t prefetch_translation_bypasses = 0;
    uint64_t prefetch_translation_faults = 0;
    uint64_t prefetch_latency_cycles = 0;
    uint64_t errors = 0;
    uint64_t max_update_occupancy = 0;
    uint64_t max_prefetch_occupancy = 0;
};

struct Observation {
    uint64_t physical_line = 0;
    uint64_t property_vaddr = 0;
    uint64_t destination = 0;
    uint64_t sequence = 0;
    uint64_t deadline = 0;
    uint64_t generation = 0;
    uint16_t context = 0;
    ecg_record::State state = ecg_record::State::UNKNOWN;
    bool valid = false;
};

struct PrefetchRequest {
    uint64_t property_vaddr = 0;
    uint64_t sequence = 0;
    uint64_t ready_cycle = 0;
    bool valid = false;
};

class Runtime {
  public:
    Runtime() = default;

    ecg_record::Status configure(
        const ecg_record::NativeConfiguration& configuration,
        ecg_record::Mechanism mechanism,
        uint64_t update_latency, std::size_t capture_width,
        std::size_t prefetch_capacity, uint64_t prefetch_latency,
        uint64_t vertices_per_line);

    ecg_record::Status updateIteration(
        uint64_t iteration_base, bool has_next);
    ecg_record::Status deactivate();
    bool active() const { return active_; }
    ecg_record::Mechanism mechanism() const { return mechanism_; }
    const ecg_record::Layout& layout() const { return layout_; }
    const ecg_record::NativeConfiguration& configuration() const {
        return configuration_;
    }
    bool propertyLine(uint64_t virtual_address) const {
        return active_ &&
            virtual_address / 64 >= configuration_.property_base / 64 &&
            virtual_address / 64 <=
                (configuration_.property_base + configuration_.vertex_count * 4 - 1) / 64;
    }
    uint64_t completedSequence() const { return completed_sequence_; }
    const Counters& counters() const { return counters_; }

    ecg_record::Status armRecordRead(
        uint64_t address, uint32_t bytes);
    ecg_record::Status observeRecordRead(
        uint64_t address, uint32_t bytes, uint64_t cycle);
    ecg_record::Status setLoadedAddress(uint64_t address);
    ecg_record::Status setLoadedChunk(uint32_t value, bool high);
    ecg_record::Status commitLoadedValue(uint32_t bytes);
    ecg_record::Status consumeRecord(
        uint64_t address, uint64_t cycle);

    bool beginPropertyAccess(
        uint64_t virtual_address, uint64_t physical_line,
        uint32_t bytes, uint64_t cycle);
    ecg_record::Status completePropertyAccess(uint64_t cycle);

    bool activeObservation(
        uint64_t physical_line, Observation& observation) const;
    bool activeDeadDemand(uint64_t physical_line) const;
    void noteDeadDemandBypass();
    ecg_record::ObservationResult observeLine(
        ecg_record::LineMetadata& metadata);

    ecg_record::PopResult popReadyUpdate(uint64_t cycle);
    ecg_record::ApplyResult applyUpdate(
        ecg_record::LineMetadata* metadata,
        const ecg_record::CommitUpdate& update);

    bool takeReadyPrefetch(
        uint64_t cycle, PrefetchRequest& request);
    void notePrefetchPrivateDuplicate();
    void notePrefetchLlcDuplicate();
    void notePrefetchCompletionPrivateDuplicate();
    void notePrefetchCompletionResident();
    void notePrefetchIssueAdmissionDrop();
    void notePrefetchCompletionAdmissionDrop();
    void notePrefetchIssued();
    void notePrefetchRetry();
    void notePrefetchPrivateLookup();
    void notePrefetchLlcLookup();
    void notePrefetchIssueAdmissionCheck();
    void notePrefetchCompletionAdmissionCheck();
    void notePrefetchDemandMerge();
    void noteLookupCycles(uint64_t cycles);
    void notePrefetchTranslation(bool fault);
    void notePrefetchTranslationBypass();
    void notePrefetchFill(uint64_t latency_cycles);

    std::optional<uint64_t> nextReadyCycle() const;
    bool updateQueueEmpty() const;
    std::size_t updatePending() const {
        return update_queue_ ? update_queue_->pendingSize() : 0;
    }
    bool prefetchQueueEmpty() const { return prefetch_queue_.empty(); }
    std::size_t prefetchPending() const { return prefetch_queue_.size(); }
    bool watermarkValid() const { return receiver_.watermarkValid(); }
    uint64_t watermark() const { return receiver_.watermark(); }
    bool clean() const;

  private:
    struct BankEntry {
        uint64_t address = 0;
        uint64_t index = 0;
        uint64_t word = 0;
        uint64_t completion_cycle = 0;
        bool valid = false;
    };

    struct PendingProperty {
        ecg_record::NativeLoadResult load;
        uint64_t physical_line = 0;
        uint64_t begin_cycle = 0;
        uint64_t prefetch_vaddr = 0;
        bool prefetch_valid = false;
        bool memory_active = false;
        bool valid = false;
    };

    struct PendingPrefetch {
        uint64_t property_vaddr = 0;
        uint64_t sequence = 0;
        uint64_t ready_cycle = 0;
    };

    ecg_record::Status fail(ecg_record::Status status);
    BankEntry* findBank(uint64_t address);
    const BankEntry* findBank(uint64_t address) const;
    BankEntry& allocateBank(uint64_t index);
    ecg_record::Status buildWindow(
        uint64_t current_index, uint64_t cycle,
        ecg_record::RecordWindow& window) const;
    ecg_record::Status selectPrefetch(
        PendingProperty& property, uint64_t cycle);
    ecg_record::Status enqueuePrefetch(
        const PendingProperty& property, uint64_t cycle);
    bool replacementActive() const;
    bool prefetchActive() const;

    ecg_record::NativeConfiguration configuration_;
    ecg_record::Layout layout_;
    ecg_record::Mechanism mechanism_ = ecg_record::Mechanism::INVALID;
    std::unique_ptr<ecg_record::CommitQueue> update_queue_;
    ecg_record::Receiver receiver_;
    std::array<BankEntry, kBankEntries> bank_{};
    PendingProperty pending_property_;
    std::deque<PendingPrefetch> prefetch_queue_;
    Counters counters_;

    uint64_t armed_read_address_ = 0;
    uint64_t observed_read_cycle_ = 0;
    uint64_t loaded_address_ = 0;
    uint64_t loaded_word_ = 0;
    uint64_t last_consumed_sequence_ = 0;
    uint64_t completed_sequence_ = 0;
    uint64_t expected_iteration_base_ = 0;
    uint64_t iteration_consumed_count_ = 0;
    uint64_t last_prefetch_output_cycle_ = 0;
    uint32_t armed_read_bytes_ = 0;
    uint8_t loaded_chunks_ = 0;
    std::size_t next_bank_slot_ = 0;
    std::size_t prefetch_capacity_ = 0;
    uint64_t prefetch_latency_ = 0;
    uint64_t vertices_per_line_ = 0;
    uint64_t line_bytes_ = 0;
    bool read_observed_ = false;
    bool have_last_consumed_ = false;
    bool prefetch_output_seen_ = false;
    bool iteration_configured_ = false;
    bool active_ = false;
};

}  // namespace record
}  // namespace sniper
}  // namespace graphbrew

#endif
