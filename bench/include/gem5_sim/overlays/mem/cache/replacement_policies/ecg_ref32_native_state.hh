#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_NATIVE_STATE_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_NATIVE_STATE_HH__

#include <cstdint>

#include "ecg_ref32.h"
#include "ecg_ref32_commit.h"

namespace ecg_ref32
{

enum class NativeBindingResult : uint8_t
{
    BOUND,
    MATCHED,
    CONFLICT,
};

struct NativeLineBinding
{
    uint64_t physicalLine = 0;
    uint64_t virtualLine = 0;
    bool physicalKnown = false;
    bool virtualKnown = false;
    // Address identity can precede registration of the property regions.
    bool classificationKnown = false;
    bool property = false;

    void clear()
    {
        physicalLine = 0;
        virtualLine = 0;
        physicalKnown = false;
        virtualKnown = false;
        classificationKnown = false;
        property = false;
    }

    NativeBindingResult bindPhysical(uint64_t physical_line)
    {
        if (physicalKnown) {
            return physicalLine == physical_line
                ? NativeBindingResult::MATCHED
                : NativeBindingResult::CONFLICT;
        }
        physicalLine = physical_line;
        physicalKnown = true;
        return NativeBindingResult::BOUND;
    }

    NativeBindingResult bindVirtual(
            uint64_t physical_line, uint64_t virtual_line,
            bool governed_property, bool classification_known = true)
    {
        if ((!classification_known && governed_property) ||
            (physicalKnown && physicalLine != physical_line) ||
            (virtualKnown && virtualLine != virtual_line) ||
            (classificationKnown && classification_known &&
             property != governed_property)) {
            return NativeBindingResult::CONFLICT;
        }
        const bool changed = !physicalKnown || !virtualKnown ||
            (!classificationKnown && classification_known);
        physicalLine = physical_line;
        virtualLine = virtual_line;
        physicalKnown = true;
        virtualKnown = true;
        if (classification_known) {
            classificationKnown = true;
            property = governed_property;
        }
        return changed ? NativeBindingResult::BOUND
                       : NativeBindingResult::MATCHED;
    }
};

enum class NativeLineState : uint8_t
{
    UNKNOWN = 0,
    FINITE = 1,
    DEAD = 2,
    PENDING_OBSERVED = 3,
};

struct NativeLineMetadata
{
    uint32_t value = 0;
    NativeLineState state = NativeLineState::UNKNOWN;
    bool prefetchOrigin = false;

    void clear()
    {
        value = 0;
        state = NativeLineState::UNKNOWN;
        prefetchOrigin = false;
    }
};

enum class ObservationResult : uint8_t
{
    ACCEPTED,
    IGNORED_OLD,
    IGNORED_HORIZON,
    INVALID_CONTEXT,
    INVALID_ORDER,
    UNSUPPORTED,
};

class NativeReceiverState
{
  public:
    NativeReceiverState() = default;

    NativeReceiverState(uint32_t record_count, uint16_t context)
    {
        configure(record_count, context);
    }

    bool configure(uint32_t record_count, uint16_t context)
    {
        if (record_count == 0 || record_count >= 0x80000000u ||
            context == 0) {
            return false;
        }
        recordCount_ = record_count;
        context_ = context;
        enabled_ = true;
        watermarkValid_ = false;
        watermark_ = 0;
        return true;
    }

    void disable()
    {
        enabled_ = false;
    }

    bool enabled() const
    {
        return enabled_;
    }

    bool watermarkValid() const
    {
        return watermarkValid_;
    }

    uint32_t watermark() const
    {
        return watermark_;
    }

    uint32_t recordCount() const
    {
        return recordCount_;
    }

    uint16_t context() const
    {
        return context_;
    }

    ObservationResult observe(
            NativeLineMetadata& line, uint16_t context,
            uint32_t sequence)
    {
        if (!enabled_)
            return ObservationResult::UNSUPPORTED;
        if (context == 0 || context != context_)
            return ObservationResult::INVALID_CONTEXT;

        const uint32_t base = watermarkValid_ ? watermark_ : 0;
        const SequenceOrder from_watermark =
            compareSequence32(sequence, base);
        if (from_watermark == SequenceOrder::AMBIGUOUS)
            return ObservationResult::INVALID_ORDER;
        if (from_watermark != SequenceOrder::NEWER)
            return ObservationResult::IGNORED_OLD;
        if (static_cast<uint32_t>(sequence - base) > recordCount_)
            return ObservationResult::IGNORED_HORIZON;

        if (line.state == NativeLineState::PENDING_OBSERVED) {
            const SequenceOrder pending_from_watermark =
                compareSequence32(line.value, base);
            const bool pending_in_horizon =
                pending_from_watermark == SequenceOrder::NEWER &&
                static_cast<uint32_t>(line.value - base) <= recordCount_;
            if (pending_in_horizon) {
                const SequenceOrder from_pending =
                    compareSequence32(sequence, line.value);
                if (from_pending == SequenceOrder::AMBIGUOUS)
                    return ObservationResult::INVALID_ORDER;
                if (from_pending != SequenceOrder::NEWER)
                    return ObservationResult::IGNORED_OLD;
            }
        }

        line.state = NativeLineState::PENDING_OBSERVED;
        line.value = sequence;
        return ObservationResult::ACCEPTED;
    }

    CommitApplyResult apply(
            NativeLineMetadata* line, const CommitUpdate& update)
    {
        if (!enabled_)
            return CommitApplyResult::UNSUPPORTED;
        if (update.context == 0 || update.context != context_)
            return CommitApplyResult::INVALID_CONTEXT;
        if (update.state != State::UNKNOWN &&
            update.state != State::FINITE &&
            update.state != State::DEAD) {
            return CommitApplyResult::INVALID_ORDER;
        }

        const uint32_t base = watermarkValid_ ? watermark_ : 0;
        const SequenceOrder delivery_order =
            compareSequence32(update.sequence, base);
        if (delivery_order != SequenceOrder::NEWER)
            return CommitApplyResult::INVALID_ORDER;

        // Coalescing can skip several traversals, unlike an issue observation.
        bool stale = false;
        if (line &&
            line->state == NativeLineState::PENDING_OBSERVED) {
            const SequenceOrder pending_order =
                compareSequence32(update.sequence, line->value);
            if (pending_order == SequenceOrder::AMBIGUOUS)
                return CommitApplyResult::INVALID_ORDER;
            stale = pending_order == SequenceOrder::OLDER;
        }

        watermark_ = update.sequence;
        watermarkValid_ = true;
        if (!line)
            return CommitApplyResult::NOT_RESIDENT;
        if (stale)
            return CommitApplyResult::STALE;

        if (update.state == State::FINITE) {
            const EffectiveFuture future = resolveQuantizedFuture(
                update.state, update.deadline, watermark_, 32);
            if (future.state != State::FINITE || future.remaining == 0) {
                line->state = NativeLineState::UNKNOWN;
                line->value = 0;
                return CommitApplyResult::EXPIRED;
            }
            line->state = NativeLineState::FINITE;
            line->value = update.deadline;
            return CommitApplyResult::APPLIED;
        }

        line->state = update.state == State::DEAD
            ? NativeLineState::DEAD : NativeLineState::UNKNOWN;
        line->value = 0;
        return CommitApplyResult::APPLIED;
    }

  private:
    uint32_t recordCount_ = 0;
    uint32_t watermark_ = 0;
    uint16_t context_ = 0;
    bool enabled_ = false;
    bool watermarkValid_ = false;
};

inline State
victimState(const NativeLineMetadata& line, bool receiver_enabled)
{
    if (!receiver_enabled ||
        line.state == NativeLineState::UNKNOWN ||
        line.state == NativeLineState::PENDING_OBSERVED) {
        return State::UNKNOWN;
    }
    return line.state == NativeLineState::FINITE
        ? State::FINITE : State::DEAD;
}

} // namespace ecg_ref32

#endif
