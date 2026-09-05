#ifndef GRAPHBREW_ECG_REF32_COMMIT_H
#define GRAPHBREW_ECG_REF32_COMMIT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "ecg_ref32.h"

namespace ecg_ref32 {

struct CommitUpdate {
    uint64_t physical_line = 0;
    uint64_t property_vaddr = 0;
    uint32_t sequence = 0;
    uint32_t deadline = 0;
    uint16_t context = 0;
    State state = State::UNKNOWN;
    bool secure = false;
};

enum class CommitApplyResult : uint8_t {
    APPLIED,
    STALE,
    EXPIRED,
    NOT_RESIDENT,
    INVALID_CONTEXT,
    INVALID_ADDRESS,
    INVALID_ORDER,
    UNSUPPORTED,
};

struct ReadyCommitUpdate {
    CommitUpdate update;
    uint64_t generation_cycle = 0;
    uint64_t ready_cycle = 0;
    uint8_t capture_lane = 0;
};

enum class EnqueueStatus : uint8_t {
    ENQUEUED,
    COALESCED,
    FULL,
    BUSY,
    INVALID_INPUT,
    INVALID_ORDER,
    INVALID_TIME,
    INVALID_CONFIGURATION,
};

enum class PopStatus : uint8_t {
    POPPED,
    EMPTY,
    NOT_READY,
    BUSY,
    INVALID_TIME,
};

struct PopResult {
    PopStatus status = PopStatus::EMPTY;
    ReadyCommitUpdate ready;
};

class CommitUpdateQueue {
  public:
    static constexpr std::size_t kCapacity = 16;
    static constexpr uint64_t kDefaultLatency = 8;
    static constexpr uint64_t kMinimumLatency = 8;
    static constexpr std::size_t kDefaultCaptureWidth = 1;
    static constexpr std::size_t kMaximumCaptureWidth = kCapacity;

    explicit CommitUpdateQueue(
            uint64_t latency_cycles = kDefaultLatency,
            std::size_t capture_width = kDefaultCaptureWidth)
        : latency_cycles_(latency_cycles),
          capture_width_(capture_width) {}

    bool validConfiguration() const {
        return latency_cycles_ >= kMinimumLatency &&
            capture_width_ >= 1 && capture_width_ <= kMaximumCaptureWidth;
    }

    uint64_t latencyCycles() const {
        return latency_cycles_;
    }

    std::size_t captureWidth() const {
        return capture_width_;
    }

    unsigned captureLaneBits() const {
        unsigned bits = 0;
        for (std::size_t lanes = capture_width_ ? capture_width_ - 1 : 0;
             lanes != 0; lanes >>= 1) {
            ++bits;
        }
        return bits;
    }

    std::size_t pendingSize() const {
        return size_;
    }

    bool empty() const {
        return size_ == 0;
    }

    std::optional<uint64_t> nextReadyCycle() const {
        std::optional<uint64_t> next;
        for (const Slot& slot : slots_) {
            if (!slot.valid)
                continue;
            if (!next || slot.ready.ready_cycle < *next)
                next = slot.ready.ready_cycle;
        }
        return next;
    }

    uint64_t cancelledCount() const {
        return cancelled_count_;
    }

    std::size_t clear() {
        const std::size_t cancelled = size_;
        for (Slot& slot : slots_)
            slot.valid = false;
        size_ = 0;
        cancelled_count_ += cancelled;
        return cancelled;
    }

    EnqueueStatus enqueue(
            const CommitUpdate& update, uint64_t generation_cycle) {
        if (!validConfiguration())
            return EnqueueStatus::INVALID_CONFIGURATION;
        if (!validUpdate(update))
            return EnqueueStatus::INVALID_INPUT;
        if (generation_cycle >
            std::numeric_limits<uint64_t>::max() - latency_cycles_) {
            return EnqueueStatus::INVALID_TIME;
        }
        if (time_seen_ && generation_cycle < last_cycle_)
            return EnqueueStatus::INVALID_TIME;

        std::array<std::size_t, 2> matching{};
        std::size_t matching_count = 0;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].valid ||
                !sameKey(slots_[index].ready.update, update)) {
                continue;
            }
            if (matching_count == matching.size())
                return EnqueueStatus::INVALID_ORDER;
            matching[matching_count++] = index;
        }

        std::optional<std::size_t> newest;
        if (matching_count == 1) {
            newest = matching[0];
        } else if (matching_count == 2) {
            const SequenceOrder order = compareSequence32(
                slots_[matching[0]].ready.update.sequence,
                slots_[matching[1]].ready.update.sequence);
            if (order == SequenceOrder::NEWER) {
                newest = matching[0];
            } else if (order == SequenceOrder::OLDER) {
                newest = matching[1];
            } else {
                return EnqueueStatus::INVALID_ORDER;
            }
        }

        if (newest) {
            const SequenceOrder order = compareSequence32(
                update.sequence, slots_[*newest].ready.update.sequence);
            if (order != SequenceOrder::NEWER)
                return EnqueueStatus::INVALID_ORDER;
        }

        if (ingress_seen_ && generation_cycle == last_ingress_cycle_ &&
            ingress_count_ == capture_width_) {
            return EnqueueStatus::BUSY;
        }

        noteCycle(generation_cycle);
        if (!ingress_seen_ || generation_cycle != last_ingress_cycle_)
            ingress_count_ = 0;
        ingress_seen_ = true;
        last_ingress_cycle_ = generation_cycle;
        const uint8_t capture_lane =
            static_cast<uint8_t>(ingress_count_++);
        const uint64_t ready_cycle =
            generation_cycle + latency_cycles_;

        if (matching_count == 2) {
            Slot& secondary = slots_[*newest];
            secondary.ready.update = update;
            secondary.ready.generation_cycle = generation_cycle;
            secondary.ready.ready_cycle = ready_cycle;
            secondary.ready.capture_lane = capture_lane;
            return EnqueueStatus::COALESCED;
        }

        const std::optional<std::size_t> free = firstFreeSlot();
        if (!free)
            return EnqueueStatus::FULL;

        Slot& slot = slots_[*free];
        slot.valid = true;
        slot.ready.update = update;
        slot.ready.generation_cycle = generation_cycle;
        slot.ready.ready_cycle = ready_cycle;
        slot.ready.capture_lane = capture_lane;
        ++size_;
        return EnqueueStatus::ENQUEUED;
    }

    PopResult popReady(uint64_t current_cycle) {
        PopResult result;
        if (time_seen_ && current_cycle < last_cycle_) {
            result.status = PopStatus::INVALID_TIME;
            return result;
        }
        if (output_seen_ && current_cycle == last_output_cycle_) {
            result.status = PopStatus::BUSY;
            return result;
        }

        noteCycle(current_cycle);
        const std::optional<std::size_t> earliest = earliestSlot();
        if (!earliest) {
            result.status = PopStatus::EMPTY;
            return result;
        }

        Slot& slot = slots_[*earliest];
        if (slot.ready.ready_cycle > current_cycle) {
            result.status = PopStatus::NOT_READY;
            return result;
        }

        result.status = PopStatus::POPPED;
        result.ready = slot.ready;
        slot.valid = false;
        --size_;
        output_seen_ = true;
        last_output_cycle_ = current_cycle;
        return result;
    }

  private:
    struct Slot {
        bool valid = false;
        ReadyCommitUpdate ready;
    };

    static bool validUpdate(const CommitUpdate& update) {
        return update.context != 0 &&
            (update.state == State::UNKNOWN ||
             update.state == State::FINITE ||
             update.state == State::DEAD);
    }

    static bool sameKey(
            const CommitUpdate& left, const CommitUpdate& right) {
        return left.physical_line == right.physical_line &&
            left.secure == right.secure &&
            left.context == right.context;
    }

    void noteCycle(uint64_t cycle) {
        time_seen_ = true;
        last_cycle_ = cycle;
    }

    std::optional<std::size_t> firstFreeSlot() const {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].valid)
                return index;
        }
        return std::nullopt;
    }

    std::optional<std::size_t> earliestSlot() const {
        std::optional<std::size_t> earliest;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].valid)
                continue;
            if (!earliest ||
                slots_[index].ready.ready_cycle <
                    slots_[*earliest].ready.ready_cycle ||
                (slots_[index].ready.ready_cycle ==
                     slots_[*earliest].ready.ready_cycle &&
                 slots_[index].ready.capture_lane <
                     slots_[*earliest].ready.capture_lane)) {
                earliest = index;
            }
        }
        return earliest;
    }

    std::array<Slot, kCapacity> slots_{};
    const uint64_t latency_cycles_;
    const std::size_t capture_width_;
    std::size_t size_ = 0;
    std::size_t ingress_count_ = 0;
    uint64_t cancelled_count_ = 0;
    uint64_t last_cycle_ = 0;
    uint64_t last_ingress_cycle_ = 0;
    uint64_t last_output_cycle_ = 0;
    bool time_seen_ = false;
    bool ingress_seen_ = false;
    bool output_seen_ = false;
};

}  // namespace ecg_ref32

#endif
