#include <cstdint>
#include <cstdio>
#include <limits>

#include "gem5_sim/overlays/mem/cache/replacement_policies/ecg_ref32_native_state.hh"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%-68s [%s]\n", message, condition ? "OK" : "FAIL");
    if (!condition)
        ++failures;
}

ecg_ref32::CommitUpdate update(
        uint32_t sequence, uint32_t deadline,
        ecg_ref32::State state = ecg_ref32::State::FINITE) {
    ecg_ref32::CommitUpdate value;
    value.physical_line = 0x1000;
    value.property_vaddr = 0x2000;
    value.sequence = sequence;
    value.deadline = deadline;
    value.context = 1;
    value.state = state;
    return value;
}

void testObservationGuards() {
    ecg_ref32::NativeReceiverState receiver(16, 1);
    ecg_ref32::NativeLineMetadata line;
    check(receiver.observe(line, 1, 1) ==
              ecg_ref32::ObservationResult::ACCEPTED &&
          !receiver.watermarkValid() &&
          line.state ==
              ecg_ref32::NativeLineState::PENDING_OBSERVED &&
          line.value == 1,
          "issue observation does not advance the receiver watermark");
    check(receiver.observe(line, 1, 1) ==
              ecg_ref32::ObservationResult::IGNORED_OLD &&
          line.value == 1,
          "equal observation is ignored without changing pending state");
    check(receiver.observe(line, 1, 17) ==
              ecg_ref32::ObservationResult::IGNORED_HORIZON &&
          line.value == 1,
          "observation beyond sealed record horizon is ignored");
    check(receiver.observe(line, 1, 0x80000000u) ==
              ecg_ref32::ObservationResult::INVALID_ORDER &&
          line.value == 1,
          "half-range observation is rejected without mutation");
    check(receiver.observe(line, 2, 2) ==
              ecg_ref32::ObservationResult::INVALID_CONTEXT &&
          line.value == 1,
          "foreign context observation is rejected");
}

void testPhysicalOnlyLineBinding() {
    ecg_ref32::NativeLineBinding binding;
    ecg_ref32::NativeLineMetadata metadata;
    metadata.value = 77;
    metadata.state = ecg_ref32::NativeLineState::PENDING_OBSERVED;
    metadata.prefetchOrigin = true;
    check(binding.bindPhysical(0) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          binding.physicalKnown && binding.physicalLine == 0 &&
          !binding.virtualKnown && !binding.property,
          "physical line zero binds without using zero as a sentinel");
    check(binding.bindVirtual(0, 0, true) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          binding.virtualKnown && binding.virtualLine == 0 &&
          binding.property && metadata.value == 77 &&
          metadata.state ==
              ecg_ref32::NativeLineState::PENDING_OBSERVED &&
          metadata.prefetchOrigin,
          "governed binding does not alter the 35-bit metadata state");
    check(binding.bindVirtual(0, 0, true) ==
              ecg_ref32::NativeBindingResult::MATCHED,
          "repeated governed binding is stable");
    check(binding.bindVirtual(0, 64, true) ==
              ecg_ref32::NativeBindingResult::CONFLICT &&
          binding.virtualLine == 0 && binding.property,
          "conflicting established VA alias is rejected without mutation");

    ecg_ref32::NativeLineBinding nongoverned;
    check(nongoverned.bindVirtual(0x1000, 0x2000, false) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          nongoverned.virtualKnown && !nongoverned.property,
          "known non-governed VA classification is retained");
    check(nongoverned.bindVirtual(0x1000, 0x3000, true) ==
              ecg_ref32::NativeBindingResult::CONFLICT &&
          nongoverned.virtualLine == 0x2000 &&
          !nongoverned.property,
          "non-governed alias cannot be rebound as governed");
    check(nongoverned.bindVirtual(0x1000, 0x2000, true) ==
              ecg_ref32::NativeBindingResult::CONFLICT &&
          !nongoverned.property,
          "immutable VA classification mismatch is rejected");

    ecg_ref32::NativeLineBinding refill;
    check(refill.bindPhysical(0x4000) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          refill.bindPhysical(0x4000) ==
              ecg_ref32::NativeBindingResult::MATCHED &&
          refill.bindPhysical(0x5000) ==
              ecg_ref32::NativeBindingResult::CONFLICT &&
          refill.physicalLine == 0x4000,
          "physical refill identity rejects conflicting PA without mutation");
}

void testDelayedContextBinding() {
    ecg_ref32::NativeLineBinding binding;
    check(binding.bindVirtual(0x6000, 0x8000, false, false) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          binding.virtualKnown && !binding.classificationKnown,
          "a pre-context demand retains its physical and virtual identity");
    check(binding.bindVirtual(0x6000, 0x8000, true) ==
              ecg_ref32::NativeBindingResult::BOUND &&
          binding.property && binding.classificationKnown,
          "late context registration can classify an unchanged resident line");
    check(binding.bindVirtual(0x6000, 0x8000, false, false) ==
              ecg_ref32::NativeBindingResult::MATCHED &&
          binding.property && binding.classificationKnown,
          "an unclassified request cannot erase an established classification");
    binding.clear();
    check(!binding.physicalKnown && !binding.virtualKnown &&
          !binding.classificationKnown && !binding.property,
          "invalidation clears address and classification validity together");
}

void testPendingCommitOrdering() {
    ecg_ref32::NativeReceiverState receiver(32, 1);
    ecg_ref32::NativeLineMetadata line;
    check(receiver.observe(line, 1, 3) ==
              ecg_ref32::ObservationResult::ACCEPTED,
          "pending sequence three is installed");
    check(receiver.apply(&line, update(1, 20)) ==
              ecg_ref32::CommitApplyResult::STALE &&
          receiver.watermarkValid() && receiver.watermark() == 1 &&
          line.state ==
              ecg_ref32::NativeLineState::PENDING_OBSERVED &&
          line.value == 3,
          "older commit advances receiver watermark but not pending line");
    check(receiver.apply(&line, update(2, 21)) ==
              ecg_ref32::CommitApplyResult::STALE &&
          receiver.watermark() == 2 && line.value == 3,
          "second older commit remains stale");
    check(receiver.apply(&line, update(3, 22)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.watermark() == 3 &&
          line.state == ecg_ref32::NativeLineState::FINITE &&
          line.value == 22,
          "equal pending commit installs finite metadata");

    ecg_ref32::NativeReceiverState newer(32, 1);
    ecg_ref32::NativeLineMetadata newer_line;
    check(newer.observe(newer_line, 1, 2) ==
              ecg_ref32::ObservationResult::ACCEPTED &&
          newer.apply(&newer_line, update(3, 30)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          newer_line.state == ecg_ref32::NativeLineState::FINITE &&
          newer_line.value == 30,
          "commit newer than pending observation installs");
}

void testWatermarkExpiryAndAbsence() {
    ecg_ref32::NativeReceiverState receiver(64, 1);
    check(receiver.apply(nullptr, update(
              1, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::NOT_RESIDENT &&
          receiver.watermark() == 1,
          "not-resident delivery still advances receiver watermark");

    ecg_ref32::NativeLineMetadata line;
    line.state = ecg_ref32::NativeLineState::FINITE;
    line.value = 99;
    check(receiver.apply(&line, update(2, 2)) ==
              ecg_ref32::CommitApplyResult::EXPIRED &&
          receiver.watermark() == 2 &&
          line.state == ecg_ref32::NativeLineState::UNKNOWN &&
          line.value == 0,
          "finite deadline at receiver coordinate expires to unknown");

    line.state = ecg_ref32::NativeLineState::FINITE;
    line.value = 123;
    line.clear();
    check(receiver.apply(&line, update(3, 9)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          line.state == ecg_ref32::NativeLineState::FINITE,
          "ordinary refill reset does not block a later live commit");
}

void testCoalescingAcrossTraversals() {
    ecg_ref32::CommitUpdateQueue queue;
    ecg_ref32::NativeReceiverState receiver(1, 1);
    ecg_ref32::NativeLineMetadata line;
    for (uint32_t sequence = 1; sequence <= 4; ++sequence) {
        const auto status = queue.enqueue(
            update(sequence, 0, ecg_ref32::State::UNKNOWN), sequence - 1);
        check(status == (sequence <= 2
                  ? ecg_ref32::EnqueueStatus::ENQUEUED
                  : ecg_ref32::EnqueueStatus::COALESCED),
              "a small traversal can coalesce while the timed link is pending");
    }
    const auto oldest = queue.popReady(8);
    check(oldest.status == ecg_ref32::PopStatus::POPPED &&
          receiver.apply(&line, oldest.ready.update) ==
              ecg_ref32::CommitApplyResult::APPLIED,
          "the protected oldest traversal update arrives first");
    const auto newest = queue.popReady(11);
    check(newest.status == ecg_ref32::PopStatus::POPPED &&
          newest.ready.update.sequence == 4 &&
          receiver.apply(&line, newest.ready.update) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.watermark() == 4,
          "ordered coalesced delivery can skip more than one traversal");
}

void testWrapZeroAndHalfRange() {
    ecg_ref32::NativeReceiverState receiver(0x7fffffffu, 1);
    ecg_ref32::NativeLineMetadata line;
    check(receiver.apply(&line, update(
              0x7fffffffu, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.apply(&line, update(
              0x80000000u, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.apply(&line, update(
              std::numeric_limits<uint32_t>::max(), 0,
              ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.apply(&line, update(
              0, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::APPLIED &&
          receiver.watermark() == 0,
          "receiver ordering wraps through semantic sequence zero");
    check(receiver.apply(&line, update(
              0x80000000u, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::INVALID_ORDER &&
          receiver.watermark() == 0,
          "half-range delivery is rejected without watermark movement");
}

void testPendingRefreshAndDisable() {
    ecg_ref32::NativeReceiverState receiver(8, 1);
    ecg_ref32::NativeLineMetadata line;
    check(receiver.observe(line, 1, 5) ==
              ecg_ref32::ObservationResult::ACCEPTED,
          "pending observation fixture is installed");
    check(receiver.apply(nullptr, update(
              6, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::NOT_RESIDENT,
          "receiver watermark can pass an old pending observation");
    check(receiver.observe(line, 1, 7) ==
              ecg_ref32::ObservationResult::ACCEPTED &&
          line.value == 7,
          "pending state behind watermark is replaceable");

    receiver.disable();
    const uint32_t before = line.value;
    check(receiver.observe(line, 1, 8) ==
              ecg_ref32::ObservationResult::UNSUPPORTED &&
          receiver.apply(&line, update(
              8, 0, ecg_ref32::State::UNKNOWN)) ==
              ecg_ref32::CommitApplyResult::UNSUPPORTED &&
          line.value == before,
          "disabled receiver leaves line metadata untouched");
}

void testSharedVictimSelection() {
    ecg_ref32::WayState ways[3];
    ways[0].property = true;
    ways[0].state = ecg_ref32::victimState(
        ecg_ref32::NativeLineMetadata{
            20, ecg_ref32::NativeLineState::FINITE, false}, true);
    ways[0].quantized_deadline = 20;
    ways[0].rrpv = 1;
    ways[0].grasp_tier = 1;
    ways[0].recency = 30;

    ways[1].property = true;
    ways[1].state = ecg_ref32::victimState(
        ecg_ref32::NativeLineMetadata{
            99, ecg_ref32::NativeLineState::PENDING_OBSERVED, false}, true);
    ways[1].rrpv = 7;
    ways[1].grasp_tier = 3;
    ways[1].recency = 20;

    ways[2].property = true;
    ways[2].state = ecg_ref32::State::DEAD;
    ways[2].rrpv = 0;
    ways[2].recency = 40;

    check(ways[1].state == ecg_ref32::State::UNKNOWN,
          "pending observations map to UNKNOWN for victim selection");
    check(ecg_ref32::selectVictim(
              ways, 3, 10, false, nullptr, 32) == 2,
          "shared REF32 selection prefers committed DEAD property");
}

} // namespace

int main() {
    testObservationGuards();
    testPhysicalOnlyLineBinding();
    testDelayedContextBinding();
    testPendingCommitOrdering();
    testWatermarkExpiryAndAbsence();
    testCoalescingAcrossTraversals();
    testWrapZeroAndHalfRange();
    testPendingRefreshAndDisable();
    testSharedVictimSelection();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
