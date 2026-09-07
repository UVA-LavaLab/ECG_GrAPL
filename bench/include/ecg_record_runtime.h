#ifndef GRAPHBREW_ECG_RECORD_RUNTIME_H
#define GRAPHBREW_ECG_RECORD_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "ecg_record.h"
#include "ecg_ref32_commit.h"

namespace ecg_record {

using SequenceOrder = ecg_ref32::SequenceOrder;
using ApplyResult = ecg_ref32::CommitApplyResult;
using EnqueueStatus = ecg_ref32::EnqueueStatus;
using PopStatus = ecg_ref32::PopStatus;

inline SequenceOrder compareSequence(
        uint8_t bits, uint64_t candidate, uint64_t current) {
    if (bits != 64)
        return SequenceOrder::AMBIGUOUS;
    return candidate == current ? SequenceOrder::EQUAL
        : candidate > current ? SequenceOrder::NEWER : SequenceOrder::OLDER;
}

inline uint64_t forwardDistance(uint64_t newer, uint64_t older) {
    return newer - older;
}

struct CommitUpdate {
    uint64_t physical_line = 0;
    uint64_t property_vaddr = 0;
    uint64_t sequence = 0;
    uint64_t deadline = 0;
    uint64_t generation = 0;
    uint16_t context = 0;
    uint8_t sequence_bits = 64;
    State state = State::UNKNOWN;
    bool secure = false;
    uint64_t order = 0;
    bool invalidate = false;
};

struct CommitUpdateTraits {
    static bool valid(const CommitUpdate& update) {
        return update.context != 0 &&
            update.sequence_bits == 64 &&
            (update.sequence != 0 || (update.invalidate && update.order != 0)) &&
            (!update.invalidate ||
                (update.state == State::UNKNOWN && update.deadline == 0 && update.order != 0)) &&
            (update.state == State::UNKNOWN || update.state == State::FINITE ||
             update.state == State::DEAD);
    }

    static bool sameKey(const CommitUpdate& left, const CommitUpdate& right) {
        return left.physical_line == right.physical_line &&
            left.context == right.context && left.generation == right.generation &&
            left.secure == right.secure;
    }

    static SequenceOrder compare(const CommitUpdate& left, const CommitUpdate& right) {
        if (left.sequence_bits != right.sequence_bits || (left.order == 0) != (right.order == 0))
            return SequenceOrder::AMBIGUOUS;
        return compareSequence(left.sequence_bits,
            left.order ? left.order : left.sequence, right.order ? right.order : right.sequence);
    }
};

using CommitQueue = ecg_ref32::BasicCommitUpdateQueue<CommitUpdate, CommitUpdateTraits>;
using PopResult = ecg_ref32::PopResultFor<CommitUpdate>;

enum class LineState : uint8_t {
    UNKNOWN,
    FINITE,
    DEAD,
    PENDING,
};

struct LineMetadata {
    uint64_t value = 0;
    LineState state = LineState::UNKNOWN;
    bool prefetch_origin = false;

    void clear() {
        value = 0;
        state = LineState::UNKNOWN;
        prefetch_origin = false;
    }
};

enum class ObservationResult : uint8_t {
    ACCEPTED,
    IGNORED_OLD,
    IGNORED_HORIZON,
    INVALID_CONTEXT,
    INVALID_ORDER,
    UNSUPPORTED,
};

struct EffectiveFuture {
    State state = State::UNKNOWN;
    uint64_t remaining = 0;
};

inline EffectiveFuture resolveFuture(
        State state, uint64_t deadline, uint64_t current) {
    EffectiveFuture future;
    future.state = state;
    if (state != State::FINITE)
        return future;
    if (deadline < current) {
        future.state = State::UNKNOWN;
        return future;
    }
    future.remaining = deadline - current;
    return future;
}

inline State victimState(const LineMetadata& line, bool enabled) {
    if (!enabled || line.state == LineState::UNKNOWN || line.state == LineState::PENDING)
        return State::UNKNOWN;
    return line.state == LineState::FINITE ? State::FINITE : State::DEAD;
}

class Receiver {
  public:
    bool configure(
            const Layout& layout, uint64_t record_count, uint16_t context,
            uint64_t generation, TraversalMode mode = TraversalMode::DENSE_EXACT) {
        if (validateLayout(layout) != Status::OK || record_count == 0 || context == 0 ||
            bitWidth(record_count) != layout.horizon_bits ||
            (mode != TraversalMode::DENSE_EXACT && mode != TraversalMode::ORDERED_FILTERED)) {
            return false;
        }
        if (enabled_) {
            return layout_ == layout && record_count_ == record_count &&
                context_ == context && generation_ == generation && mode_ == mode;
        }
        layout_ = layout;
        record_count_ = record_count;
        context_ = context;
        generation_ = generation;
        watermark_ = 0;
        watermark_valid_ = false;
        mode_ = mode;
        progress_ = delivery_order_ = 0;
        enabled_ = true;
        return true;
    }

    void disable() { enabled_ = false; }
    bool enabled() const { return enabled_; }
    bool watermarkValid() const { return filtered() ? progress_ != 0 : watermark_valid_; }
    uint64_t watermark() const { return filtered() ? progress_ : watermark_; }
    uint64_t deliveredWatermark() const { return watermark_; }
    bool filtered() const { return mode_ == TraversalMode::ORDERED_FILTERED; }
    uint64_t generation() const { return generation_; }
    uint16_t context() const { return context_; }
    const Layout& layout() const { return layout_; }
    unsigned valueBits() const {
        return std::max(layout_.sequence_bits, layout_.deadline_bits);
    }

    ObservationResult advanceProgress(
            uint16_t context, uint64_t generation, uint64_t sequence) {
        if (!enabled_ || !filtered())
            return ObservationResult::UNSUPPORTED;
        if (context != context_ || generation != generation_)
            return ObservationResult::INVALID_CONTEXT;
        if (sequence <= progress_)
            return ObservationResult::IGNORED_OLD;
        if (sequence - progress_ > record_count_)
            return ObservationResult::IGNORED_HORIZON;
        progress_ = sequence;
        return ObservationResult::ACCEPTED;
    }

    ObservationResult invalidateObservation(
            LineMetadata& line, uint16_t context, uint64_t generation, uint64_t sequence) {
        const auto result = advanceProgress(context, generation, sequence);
        if (result != ObservationResult::ACCEPTED && result != ObservationResult::IGNORED_OLD)
            return result;
        // UNKNOWN's otherwise unused value prevents delayed updates from reviving the line.
        line.value = std::max(progress_,
            line.state == LineState::PENDING || line.state == LineState::UNKNOWN ? line.value : 0);
        line.state = LineState::UNKNOWN;
        return ObservationResult::ACCEPTED;
    }

    ObservationResult observe(
            LineMetadata& line, uint16_t context, uint64_t generation,
            uint64_t sequence) {
        if (!enabled_)
            return ObservationResult::UNSUPPORTED;
        if (context != context_ || generation != generation_)
            return ObservationResult::INVALID_CONTEXT;
        if (filtered()) {
            const auto result = advanceProgress(context, generation, sequence);
            if (result != ObservationResult::ACCEPTED && result != ObservationResult::IGNORED_OLD)
                return result;
            if (sequence <= watermark_ ||
                ((line.state == LineState::PENDING || line.state == LineState::UNKNOWN) &&
                    line.value >= sequence))
                return ObservationResult::IGNORED_OLD;
            line.value = sequence;
            line.state = LineState::PENDING;
            return ObservationResult::ACCEPTED;
        }
        const uint64_t base = watermark_valid_ ? watermark_ : 0;
        const SequenceOrder order =
            compareSequence(layout_.sequence_bits, sequence, base);
        if (order == SequenceOrder::AMBIGUOUS)
            return ObservationResult::INVALID_ORDER;
        if (order != SequenceOrder::NEWER)
            return ObservationResult::IGNORED_OLD;
        if (forwardDistance(sequence, base) > record_count_)
            return ObservationResult::IGNORED_HORIZON;
        if (line.state == LineState::PENDING) {
            const SequenceOrder pending =
                compareSequence(layout_.sequence_bits, line.value, base);
            if (pending == SequenceOrder::NEWER &&
                forwardDistance(line.value, base) <= record_count_) {
                const SequenceOrder newer =
                    compareSequence(layout_.sequence_bits, sequence, line.value);
                if (newer == SequenceOrder::AMBIGUOUS)
                    return ObservationResult::INVALID_ORDER;
                if (newer != SequenceOrder::NEWER)
                    return ObservationResult::IGNORED_OLD;
            }
        }
        line.value = sequence;
        line.state = LineState::PENDING;
        return ObservationResult::ACCEPTED;
    }

    ApplyResult apply(LineMetadata* line, const CommitUpdate& update) {
        if (!enabled_)
            return ApplyResult::UNSUPPORTED;
        if (update.context != context_ || update.generation != generation_)
            return ApplyResult::INVALID_CONTEXT;
        if (!CommitUpdateTraits::valid(update) ||
            update.sequence_bits != layout_.sequence_bits) {
            return ApplyResult::INVALID_ORDER;
        }
        if (filtered())
            return applyFiltered(line, update);
        if (update.order != 0 || update.invalidate)
            return ApplyResult::INVALID_ORDER;
        const uint64_t base = watermark_valid_ ? watermark_ : 0;
        if (compareSequence(layout_.sequence_bits, update.sequence, base) !=
            SequenceOrder::NEWER) {
            return ApplyResult::INVALID_ORDER;
        }
        bool stale = false;
        if (line && line->state == LineState::PENDING) {
            const SequenceOrder order =
                compareSequence(layout_.sequence_bits, update.sequence, line->value);
            if (order == SequenceOrder::AMBIGUOUS)
                return ApplyResult::INVALID_ORDER;
            stale = order == SequenceOrder::OLDER;
        }
        watermark_ = update.sequence;
        watermark_valid_ = true;
        if (!line)
            return ApplyResult::NOT_RESIDENT;
        if (stale)
            return ApplyResult::STALE;
        if (update.state == State::FINITE) {
            const EffectiveFuture future =
                resolveFuture(update.state, update.deadline, watermark_);
            if (future.state != State::FINITE || future.remaining == 0) {
                line->state = LineState::UNKNOWN;
                line->value = 0;
                return ApplyResult::EXPIRED;
            }
            line->state = LineState::FINITE;
            line->value = update.deadline;
        } else {
            line->state = update.state == State::DEAD ? LineState::DEAD : LineState::UNKNOWN;
            line->value = 0;
        }
        return ApplyResult::APPLIED;
    }

  private:
    ApplyResult applyFiltered(LineMetadata* line, const CommitUpdate& update) {
        if (update.order == 0 || update.order <= delivery_order_ || update.state == State::DEAD)
            return ApplyResult::INVALID_ORDER;
        delivery_order_ = update.order;
        watermark_ = std::max(watermark_, update.sequence);
        progress_ = std::max(progress_, update.sequence);
        watermark_valid_ = true;
        if (!line)
            return ApplyResult::NOT_RESIDENT;
        if (update.invalidate) {
            invalidateObservation(*line, context_, generation_, progress_);
            return ApplyResult::APPLIED;
        }
        if ((line->state == LineState::PENDING && update.sequence < line->value) ||
            (line->state == LineState::UNKNOWN && update.sequence <= line->value))
            return ApplyResult::STALE;
        if (update.state == State::FINITE) {
            if (update.deadline <= progress_) {
                line->state = LineState::UNKNOWN;
                line->value = progress_;
                return ApplyResult::EXPIRED;
            }
            line->state = LineState::FINITE;
            line->value = update.deadline;
        } else {
            line->state = LineState::UNKNOWN;
            line->value = update.sequence;
        }
        return ApplyResult::APPLIED;
    }

    Layout layout_;
    TraversalMode mode_ = TraversalMode::DENSE_EXACT;
    uint64_t record_count_ = 0;
    uint64_t generation_ = 0;
    uint64_t watermark_ = 0;
    uint64_t progress_ = 0;
    uint64_t delivery_order_ = 0;
    uint16_t context_ = 0;
    bool enabled_ = false;
    bool watermark_valid_ = false;
};

struct WayState {
    bool property = false;
    uint8_t rrpv = 0;
    uint64_t recency = 0;
    uint8_t grasp_tier = 0;
    State state = State::UNKNOWN;
    uint64_t deadline = 0;
};

inline Status selectVictim(
        const Layout& layout, const WayState* ways, std::size_t count,
        uint64_t sequence, std::size_t& victim) {
    victim = std::numeric_limits<std::size_t>::max();
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (!ways || count == 0 || count > 64)
        return Status::INVALID_COUNTS;
    std::array<ecg_ref32::WayState, 64> adapted{};
    for (std::size_t index = 0; index < count; ++index) {
        adapted[index].property = ways[index].property;
        adapted[index].rrpv = ways[index].rrpv;
        adapted[index].recency = ways[index].recency;
        adapted[index].grasp_tier = ways[index].grasp_tier;
        adapted[index].state = ways[index].state;
        adapted[index].exact_deadline = ways[index].deadline;
    }
    // The legacy helper's "exact" flag selects linear 64-bit storage, not an oracle.
    victim = ecg_ref32::selectVictim(
        adapted.data(), count, sequence, true);
    return Status::OK;
}

inline Status canAdmitPrefetch(
        const Layout& layout, const WayState* ways, const bool* valid,
        std::size_t count, uint64_t sequence, bool& admit) {
    admit = false;
    if (validateLayout(layout) != Status::OK)
        return Status::INVALID_LAYOUT;
    if (!ways || !valid || count == 0 || count > 64)
        return Status::INVALID_COUNTS;
    for (std::size_t index = 0; index < count; ++index) {
        if (!valid[index] || !ways[index].property || ways[index].state == State::DEAD) {
            admit = true;
            return Status::OK;
        }
        const EffectiveFuture future =
            resolveFuture(ways[index].state, ways[index].deadline, sequence);
        const uint8_t score = future.state == State::FINITE
            ? ecg_ref32::distanceRRPV(future.remaining)
            : std::max<uint8_t>(ways[index].rrpv,
                ecg_policy::graspTierRRPV(ways[index].grasp_tier, 7));
        if (score >= 7) {
            admit = true;
            return Status::OK;
        }
    }
    return Status::OK;
}

}  // namespace ecg_record

#endif
