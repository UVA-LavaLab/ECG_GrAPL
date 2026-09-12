#ifndef GRAPHBREW_ECG_FRONTIER_H
#define GRAPHBREW_ECG_FRONTIER_H

#include <array>

#include "ecg_window.h"

namespace ecg_frontier {

using Bitmap = std::array<uint64_t, 4>;
using Layout = ecg_window::Layout;

struct Profile {
    static constexpr bool fingerprint_carrier = true;
    static constexpr uint64_t maximum_anchor = UINT64_MAX >> 10;
    uint64_t vertices, cohort_rows, bin_rows, bins;

    explicit Profile(uint64_t count, uint64_t fixture_cohort = 0)
        : vertices(count), cohort_rows(ecg_window::Profile(count, fixture_cohort).cohort_rows),
          bin_rows(cohort_rows), bins((count + cohort_rows - 1) / cohort_rows) {
        if (bins > 256)
            throw std::invalid_argument("frontier-cohort-count");
    }

    uint16_t reverse(uint64_t& entry, uint32_t row) const {
        constexpr uint64_t valid = uint64_t{1} << 19;
        if (row >= vertices || entry >> 52 || (entry && !(entry & valid)))
            throw std::invalid_argument("frontier-producer-state");
        uint16_t mask = 0;
        bool beyond = false;
        if (entry & valid) {
            const uint32_t previous = static_cast<uint32_t>(entry >> 20);
            if (row > previous)
                throw std::logic_error("frontier-reverse-order");
            if (row == previous)
                return static_cast<uint16_t>((entry >> 9) & 1023);
            const uint64_t gap = previous / cohort_rows - row / cohort_rows;
            beyond = entry & 256;
            if (gap >= 8) {
                beyond = beyond || (entry & 255);
            } else {
                const uint16_t shifted = static_cast<uint16_t>((entry & 255) << gap);
                beyond = beyond || shifted > 255;
                mask = shifted & 255;
            }
        }
        const uint16_t token = static_cast<uint16_t>(512 | (beyond ? 256 : 0) | mask);
        entry = (uint64_t(row) << 20) | valid | (uint64_t(token) << 9) |
            (beyond ? 256 : 0) | mask | 1;
        return token;
    }

    uint64_t decode(uint16_t token, uint64_t row, uint64_t pass_base) const {
        if (row >= vertices || token > 1023 || (token && !(token & 512)) ||
            pass_base > maximum_anchor - bins)
            throw std::invalid_argument("frontier-token");
        if (!token)
            return 0;
        const uint64_t cohort = row / cohort_rows;
        const uint64_t remaining = bins - cohort;
        const uint16_t valid_bits = remaining >= 8 ? 255 : (uint16_t{1} << remaining) - 1;
        if ((token & 255 & ~valid_bits) || ((token & 256) && remaining <= 8))
            throw std::invalid_argument("frontier-token-outside-graph");
        return ((pass_base + cohort) << 10) | token;
    }

    bool live(uint64_t payload, uint64_t watermark, uint64_t pass_base) const {
        const uint64_t anchor = payload >> 10;
        return payload && (payload & 512) && anchor >= pass_base && anchor - pass_base < bins &&
            watermark >= pass_base && watermark - pass_base < bins;
    }

    bool rank(uint64_t payload, uint64_t watermark, uint64_t pass_base,
              const Bitmap& current, bool gating, uint64_t& first) const {
        if (!live(payload, watermark, pass_base))
            return false;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            if (!(payload & (uint64_t{1} << bit)))
                continue;
            const uint64_t cohort = (payload >> 10) + bit;
            if (cohort < watermark || cohort - pass_base >= bins)
                continue;
            const uint64_t local = cohort - pass_base;
            if (!gating || (current[local / 64] & (uint64_t{1} << (local % 64)))) {
                first = cohort;
                return true;
            }
        }
        return false;
    }
};

using RecordStream = ecg_window::PotentialRecordStream<Profile>;

class Counts {
  public:
    static constexpr uint64_t storage_bytes = 2 * 256 * sizeof(uint32_t);
    uint64_t read_bytes = 0, write_bytes = 0, arithmetic_steps = 0, scan_steps = 0;
    uint64_t appends = 0, clears = 0, swaps = 0;

    template<class Memory, class Step>
    void initialize(const Profile& profile, uint32_t seed, Memory memory, Step step) {
        if (initialized_ || seed >= profile.vertices)
            throw std::logic_error("invalid-frontier-count-initialization");
        vertices_ = profile.vertices;
        cohort_rows_ = profile.cohort_rows;
        cohorts_ = profile.bins;
        for (auto& bank : counts_)
            for (auto& value : bank) {
                memory(&value, sizeof(value), true);
                write_bytes += sizeof(value);
                value = 0;
            }
        update(0, seed, true, memory, step);
        remaining_ = 1;
        initialized_ = true;
    }

    template<class Memory, class Step>
    Bitmap begin(Memory memory, Step step) {
        if (!initialized_ || active_ || swap_due_ || !remaining_ || next_)
            throw std::logic_error("invalid-frontier-count-begin");
        Bitmap bitmap{};
        uint64_t sum = 0;
        for (uint64_t cohort = 0; cohort < cohorts_; ++cohort) {
            const uint32_t count = read(counts_[bank_][cohort], memory);
            step();
            ++scan_steps;
            sum += count;
            if (count)
                bitmap[cohort / 64] |= uint64_t{1} << (cohort % 64);
        }
        if (sum != remaining_)
            throw std::logic_error("frontier-count-sum-mismatch");
        active_ = true;
        return bitmap;
    }

    template<class Memory, class Step>
    bool consume(uint32_t vertex, Memory memory, Step step) {
        if (!active_ || !remaining_ || vertex >= vertices_)
            throw std::logic_error("invalid-frontier-count-consume");
        const uint32_t count = update(bank_, vertex, false, memory, step);
        --remaining_;
        clears += count == 0;
        return count == 0;
    }

    template<class Memory, class Step>
    void append(uint64_t index, uint32_t vertex, Memory memory, Step step) {
        if (!active_ || index != next_ || next_ >= vertices_ || vertex >= vertices_)
            throw std::logic_error("invalid-frontier-count-append");
        update(bank_ ^ 1, vertex, true, memory, step);
        ++next_;
        ++appends;
    }

    void close() {
        if (!active_ || remaining_)
            throw std::logic_error("frontier-count-pass-not-exhausted");
        active_ = false;
        swap_due_ = true;
    }

    template<class Step>
    void swap(uint64_t count, Step step) {
        if (active_ || !swap_due_ || remaining_ || next_ != count)
            throw std::logic_error("frontier-count-swap-mismatch");
        step();
        ++swaps;
        bank_ ^= 1;
        remaining_ = next_;
        next_ = 0;
        swap_due_ = false;
    }

    void finish() const {
        if (!initialized_ || active_ || swap_due_ || remaining_ || next_)
            throw std::logic_error("frontier-count-work-not-exhausted");
    }
    uint64_t remaining() const { return remaining_; }
    uint64_t next() const { return next_; }

  private:
    template<class Memory>
    uint32_t read(const uint32_t& value, Memory memory) {
        memory(&value, sizeof(value), false);
        read_bytes += sizeof(value);
        return value;
    }
    template<class Memory, class Step>
    uint32_t update(unsigned bank, uint32_t vertex, bool increment, Memory memory, Step step) {
        auto& value = counts_[bank][vertex / cohort_rows_];
        const uint32_t old = read(value, memory);
        if ((increment && old == UINT32_MAX) || (!increment && old == 0))
            throw std::logic_error("frontier-counter-overflow-or-underflow");
        step();
        ++arithmetic_steps;
        const uint32_t next = increment ? old + 1 : old - 1;
        memory(&value, sizeof(value), true);
        write_bytes += sizeof(value);
        value = next;
        return next;
    }

    alignas(64) std::array<std::array<uint32_t, 256>, 2> counts_{};
    uint64_t vertices_ = 0, cohort_rows_ = 0, cohorts_ = 0, remaining_ = 0, next_ = 0;
    unsigned bank_ = 0;
    bool initialized_ = false, active_ = false, swap_due_ = false;
};

struct NoCounts {};

}  // namespace ecg_frontier

#endif
