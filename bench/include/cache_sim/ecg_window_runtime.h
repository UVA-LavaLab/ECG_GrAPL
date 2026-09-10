#ifndef GRAPHBREW_CACHE_SIM_ECG_WINDOW_RUNTIME_H
#define GRAPHBREW_CACHE_SIM_ECG_WINDOW_RUNTIME_H

#include <array>
#include <cstring>

#include "cache_sim.h"
#include "../ecg_window.h"

namespace cache_sim {

class WindowRuntime {
  public:
    static constexpr std::size_t kQueue = 16;
    static constexpr uint64_t kLatency = 8, kControlSteps = 16;

    WindowRuntime(CacheHierarchy& cache, const ecg_window::RecordStream& stream,
                  uint64_t vertices, uint64_t property_base, bool replacement, uint8_t floor)
        : cache_(cache), profile_(vertices), layout_(stream.layout), records_(stream.size()),
          record_base_(stream.data()), property_base_(property_base), replacement_(replacement), floor_(floor) {
        layout_.validate();
        if (!records_ || stream.vertices != vertices || stream.cohort_rows != profile_.cohort_rows ||
            !record_base_ || reinterpret_cast<uint64_t>(record_base_) % layout_.record_bytes ||
            property_base % 64 || !ecg_record::checkedAdd(property_base, vertices * 4, property_end_) ||
            cursor_.configure(records_, ecg_record::TraversalMode::ORDERED_FILTERED) != ecg_record::Status::OK)
            throw std::invalid_argument("invalid-window-runtime-binding");
        cache_.configureWindow(profile_, property_base, replacement, floor);
        control(0, 0, false);
    }
    ~WindowRuntime() { cache_.disableWindow(); }
    WindowRuntime(const WindowRuntime&) = delete;
    WindowRuntime& operator=(const WindowRuntime&) = delete;

    void beginPass() {
        if (finished_ || open_ || pending_record_ || store_authorized_ ||
            cursor_.begin() != ecg_record::Status::OK ||
            !ecg_record::checkedMultiply(passes_, profile_.bins, pass_base_) ||
            pass_base_ > (UINT64_MAX >> 3) - profile_.bins)
            throw std::logic_error("invalid-window-pass-begin");
        open_ = true;
        source_ = UINT64_MAX;
        previous_row_end_ = 0;
        row_ready_ = last_read_.valid = false;
        control(pass_base_, pass_base_, true);
        ++markers_;
    }

    void visitVertex(uint64_t source) {
        if (!open_ || pending_record_ || store_authorized_ || source >= profile_.vertices ||
            (source_ != UINT64_MAX && source <= source_))
            throw std::logic_error("invalid-window-source-progress");
        last_read_.valid = row_ready_ = false;
        const uint64_t next = pass_base_ + source / profile_.bin_rows;
        if (source_ == UINT64_MAX || next != watermark_) {
            control(pass_base_, next, true);
            ++markers_;
        }
        source_ = source;
        ++source_rows_;
    }

    void rowContext(uint64_t source, uint64_t first, uint64_t last) {
        if (!open_ || source != source_ || row_ready_ || pending_record_ || store_authorized_ ||
            first < previous_row_end_ || first > last || last > records_)
            throw std::logic_error("invalid-window-row-context");
        tick();
        ++row_context_steps_;
        row_first_ = first;
        row_last_ = previous_row_end_ = last;
        row_ready_ = true;
    }

    uint64_t recordLoad(uint64_t index) {
        if (!open_ || !row_ready_ || pending_record_ || store_authorized_ ||
            index < row_first_ || index >= row_last_ || index >= records_)
            throw std::logic_error("invalid-window-record-load");
        const auto* address = record_base_ + index * layout_.record_bytes;
        memory(reinterpret_cast<uint64_t>(address), false);
        uint64_t word = 0;
        std::memcpy(&word, address, layout_.record_bytes);
        layout_.id(word, profile_.vertices);
        pending_index_ = index;
        pending_word_ = word;
        pending_record_ = true;
        ++record_loads_;
        return word;
    }

    uint32_t property(uint64_t index, uint64_t word, uint64_t base) {
        if (!open_ || !row_ready_ || !pending_record_ || pending_index_ != index ||
            pending_word_ != word || base != property_base_ ||
            cursor_.consume(index) != ecg_record::Status::OK)
            throw std::logic_error("window-record-property-association");
        const uint32_t destination = layout_.id(word, profile_.vertices);
        expected_address_ = base + uint64_t(destination) * 4;
        expected_value_ = profile_.decode(static_cast<uint16_t>(word >> layout_.id_bits), source_, pass_base_);
        expected_index_ = index;
        designated_ = true;
        memory(expected_address_, false);
        pending_record_ = false;
        ++property_reads_;
        trace_value_ = expected_value_;
        trace_source_ = source_;
        return destination;
    }

    void associateStore(uint64_t index, uint32_t destination, uint64_t base, uint64_t bytes) {
        if (!open_ || pending_record_ || store_authorized_ || !last_read_.valid ||
            base != property_base_ || bytes != 4 || destination >= profile_.vertices ||
            index != last_read_.index || base + uint64_t(destination) * 4 != last_read_.address ||
            last_read_.source != source_ || last_read_.pass != passes_ ||
            last_read_.request != memory_steps_ || last_read_.order != order_)
            throw std::logic_error("invalid-window-checked-store");
        tick();
        ++association_steps_;
        store_authorized_ = true;
    }

    void memory(uint64_t address, bool write) {
        if (finished_ || in_memory_ || (designated_ && (write || address != expected_address_)) ||
            (store_authorized_ && (!write || !last_read_.valid || address != last_read_.address)))
            throw std::logic_error("invalid-window-memory-order");
        in_memory_ = true;
        const bool forward = store_authorized_;
        const uint64_t payload = designated_ ? expected_value_ : forward ? last_read_.value : 0;
        store_authorized_ = last_read_.valid = false;
        tick();
        ++memory_steps_;
        const bool property_access = address >= property_base_ && address < property_end_;
        bool resident = false;
        if (property_access) {
            if (!ecg_record::checkedAdd(order_, 1, order_))
                throw std::overflow_error("window-event-order-overflow");
            observationSlot();
            resident = cache_.observeWindow(address, order_);
        }
        cache_.access(address, write);
        if (property_access) {
            if (!resident) {
                observationSlot();
                cache_.observeWindow(address, order_);
            }
            if (queued_ == kQueue || step_ > UINT64_MAX - kLatency)
                throw std::length_error("window-required-update-overflow");
            queue_[(head_ + queued_) % kQueue] = {address & ~uint64_t{63}, order_, payload, step_ + kLatency};
            ++queued_;
            ++enqueued_;
            peak_ = std::max(peak_, queued_);
            if (designated_)
                last_read_ = {expected_index_, expected_address_, expected_value_, source_, passes_, memory_steps_, order_, true};
            else if (forward)
                ++forwarded_;
            else
                ++ordinary_invalidations_;
        }
        designated_ = in_memory_ = false;
    }

    void closePass() {
        if (!open_ || pending_record_ || store_authorized_ || in_memory_ ||
            cursor_.close() != ecg_record::Status::OK)
            throw std::logic_error("incomplete-window-pass");
        last_read_.valid = false;
        control(pass_base_, pass_base_ + profile_.bins, false);
        ++markers_;
        ++passes_;
        open_ = row_ready_ = false;
    }

    void drain() {
        while (queued_) {
            tick();
            ++drain_steps_;
        }
    }

    void finish(uint64_t actual_records) {
        if (open_ || pending_record_ || store_authorized_ || in_memory_ || finished_ ||
            actual_records != property_reads_ || record_loads_ != property_reads_)
            throw std::logic_error("incomplete-window-work");
        drain();
        if (enqueued_ != delivered_ || delivered_ != applied_ + stale_ + absent_ + expired_ ||
            enqueued_ != property_reads_ + forwarded_ + ordinary_invalidations_ ||
            association_steps_ != forwarded_)
            throw std::logic_error("incomplete-window-accounting");
        finished_ = true;
        record_base_ = nullptr;
        cache_.disableWindow();
    }

    uint64_t traceValue() const { return trace_value_; }
    uint64_t traceSource() const { return trace_source_; }

    void write(std::ostream& output) const {
        const auto& policy = cache_.windowStats();
        output << "{\"schema\":\"ecg.window-runtime.v1\",\"record_model\":\"potential-window-u32\","
               << "\"cost_unit\":\"functional-steps-not-CPU-cycles\","
               << "\"replacement\":" << (replacement_ ? "true" : "false")
               << ",\"candidate_floor\":" << unsigned(floor_)
               << ",\"record_bytes\":" << unsigned(layout_.record_bytes)
               << ",\"token_bits\":10,\"cohort_rows\":" << profile_.cohort_rows
               << ",\"bin_rows\":" << profile_.bin_rows << ",\"bins_per_pass\":" << profile_.bins
               << ",\"record_loads\":" << record_loads_ << ",\"record_read_bytes\":" << record_loads_ * layout_.record_bytes
               << ",\"property_reads\":" << property_reads_ << ",\"forwarded_stores\":" << forwarded_
               << ",\"ordinary_invalidations\":" << ordinary_invalidations_
               << ",\"passes\":" << passes_ << ",\"source_rows\":" << source_rows_
               << ",\"structural_positions\":" << cursor_.sequence() << ",\"skipped_positions\":" << cursor_.skipped()
               << ",\"steps\":" << step_ << ",\"memory_steps\":" << memory_steps_
               << ",\"row_context_steps\":" << row_context_steps_ << ",\"association_steps\":" << association_steps_
               << ",\"observation_steps\":" << observation_steps_ << ",\"observation_wait_steps\":" << observation_wait_steps_
               << ",\"configuration_steps\":" << kControlSteps
               << ",\"markers\":" << markers_ << ",\"marker_steps\":" << markers_ * kControlSteps
               << ",\"control_bytes\":" << (markers_ + 1) * 48 << ",\"drain_steps\":" << drain_steps_
               << ",\"queue_capacity\":" << kQueue << ",\"latency_steps\":" << kLatency
               << ",\"enqueued\":" << enqueued_ << ",\"delivered\":" << delivered_
               << ",\"applied\":" << applied_ << ",\"stale\":" << stale_
               << ",\"absent\":" << absent_ << ",\"expired\":" << expired_
               << ",\"pending\":" << queued_ << ",\"queue_peak\":" << peak_
               << ",\"victim_decisions\":" << policy.decisions << ",\"live_base_victims\":" << policy.live_base
               << ",\"victim_overrides\":" << policy.overrides << ",\"protected_overrides\":" << policy.protected_overrides
               << ",\"metadata_payload_bits_per_line\":67,\"controller_object_bytes\":" << sizeof(*this)
               << ",\"unmodeled_runtime_table_bytes\":0}";
    }

  private:
    struct Update { uint64_t line, order, value, ready; };
    struct Read {
        uint64_t index = 0, address = 0, value = 0, source = 0, pass = 0, request = 0, order = 0;
        bool valid = false;
    };
    bool tick() {
        if (!ecg_record::checkedAdd(step_, 1, step_))
            throw std::overflow_error("window-step-overflow");
        if (!queued_ || queue_[head_].ready > step_)
            return false;
        const auto update = queue_[head_];
        head_ = (head_ + 1) % kQueue;
        --queued_;
        ++delivered_;
        switch (cache_.applyWindow(update.line, update.order, update.value)) {
          case ecg_record::ApplyResult::APPLIED: ++applied_; break;
          case ecg_record::ApplyResult::STALE: ++stale_; break;
          case ecg_record::ApplyResult::NOT_RESIDENT: ++absent_; break;
          case ecg_record::ApplyResult::EXPIRED: ++expired_; break;
          default: throw std::logic_error("invalid-window-delivery-result");
        }
        return true;
    }
    void observationSlot() {
        while (tick())
            ++observation_wait_steps_;
        ++observation_steps_;
    }
    void control(uint64_t base, uint64_t watermark, bool open) {
        drain();
        for (uint64_t step = 0; step < kLatency; ++step)
            tick();
        cache_.windowProgress(base, watermark, open);
        for (uint64_t step = 0; step < kLatency; ++step)
            tick();
        watermark_ = watermark;
    }

    CacheHierarchy& cache_;
    ecg_window::Profile profile_;
    ecg_window::Layout layout_;
    uint64_t records_;
    const uint8_t* record_base_;
    uint64_t property_base_, property_end_ = 0;
    bool replacement_;
    uint8_t floor_;
    ecg_record::PassCursor cursor_;
    std::array<Update, kQueue> queue_{};
    Read last_read_;
    uint64_t step_ = 0, order_ = 0, passes_ = 0, pass_base_ = 0, watermark_ = 0, source_ = UINT64_MAX;
    uint64_t row_first_ = 0, row_last_ = 0, previous_row_end_ = 0;
    uint64_t pending_index_ = 0, pending_word_ = 0;
    uint64_t expected_index_ = 0, expected_address_ = 0, expected_value_ = 0;
    uint64_t trace_value_ = 0, trace_source_ = 0;
    uint64_t record_loads_ = 0, property_reads_ = 0, forwarded_ = 0, ordinary_invalidations_ = 0;
    uint64_t source_rows_ = 0, memory_steps_ = 0, row_context_steps_ = 0, association_steps_ = 0;
    uint64_t observation_steps_ = 0, observation_wait_steps_ = 0, markers_ = 0, drain_steps_ = 0;
    uint64_t enqueued_ = 0, delivered_ = 0, applied_ = 0, stale_ = 0, absent_ = 0, expired_ = 0;
    std::size_t head_ = 0, queued_ = 0, peak_ = 0;
    bool open_ = false, row_ready_ = false, pending_record_ = false, designated_ = false;
    bool store_authorized_ = false, in_memory_ = false, finished_ = false;
};

}  // namespace cache_sim

#endif
