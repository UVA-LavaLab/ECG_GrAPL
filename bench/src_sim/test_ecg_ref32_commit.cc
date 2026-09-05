#include <cstdint>
#include <cstdio>
#include <limits>

#include "ecg_ref32_commit.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%-66s [%s]\n", message, condition ? "OK" : "FAIL");
    if (!condition)
        ++failures;
}

ecg_ref32::CommitUpdate update(
        uint64_t line, uint32_t sequence, uint16_t context = 1,
        bool secure = false,
        ecg_ref32::State state = ecg_ref32::State::FINITE) {
    ecg_ref32::CommitUpdate value;
    value.physical_line = line;
    value.property_vaddr = 0x10000000ULL + line;
    value.sequence = sequence;
    value.deadline = sequence + 100;
    value.context = context;
    value.state = state;
    value.secure = secure;
    return value;
}

bool poppedWith(
        const ecg_ref32::PopResult& result, uint32_t sequence,
        uint64_t generated, uint64_t ready) {
    return result.status == ecg_ref32::PopStatus::POPPED &&
        result.ready.update.sequence == sequence &&
        result.ready.generation_cycle == generated &&
        result.ready.ready_cycle == ready &&
        result.ready.ready_cycle >=
            result.ready.generation_cycle +
                ecg_ref32::CommitUpdateQueue::kMinimumLatency;
}

void testLatencyAndBandwidth() {
    ecg_ref32::CommitUpdateQueue queue;
    check(queue.enqueue(update(0x1000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "cycle zero is a valid generation cycle");
    check(queue.enqueue(update(0x2000, 2), 0) ==
              ecg_ref32::EnqueueStatus::BUSY,
          "a second valid ingress in one cycle is BUSY");
    check(queue.popReady(7).status ==
              ecg_ref32::PopStatus::NOT_READY,
          "an update cannot leave before eight cycles");
    check(poppedWith(queue.popReady(8), 1, 0, 8),
          "an update leaves at its exact minimum-ready cycle");

    check(queue.enqueue(update(0x2000, 2), 8) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "input and output bandwidth are independent");
    check(queue.enqueue(update(0x3000, 3), 9) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "the next cycle accepts another input");
    check(poppedWith(queue.popReady(17), 2, 8, 16),
          "earliest ready update wins even when observed later");
    check(queue.popReady(17).status ==
              ecg_ref32::PopStatus::BUSY,
          "at most one update is popped per cycle");
    check(poppedWith(queue.popReady(18), 3, 9, 17),
          "a deferred ready update leaves on the following cycle");
}

void testTwoVersionCoalescing() {
    ecg_ref32::CommitUpdateQueue queue;
    check(queue.enqueue(update(0x4000, 100), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x4000, 101), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.pendingSize() == 2,
          "a same-line secondary consumes the second physical slot");
    check(queue.enqueue(
              update(0x4000, 102, 1, false, ecg_ref32::State::DEAD), 2) ==
              ecg_ref32::EnqueueStatus::COALESCED &&
          queue.pendingSize() == 2,
          "a third version replaces only the newest secondary");
    check(poppedWith(queue.popReady(8), 100, 0, 8),
          "secondary replacement never postpones the protected oldest");
    check(queue.popReady(9).status ==
              ecg_ref32::PopStatus::NOT_READY,
          "a replaced secondary is not delivered at its old ready time");
    const auto latest = queue.popReady(10);
    check(poppedWith(latest, 102, 2, 10) &&
          latest.ready.update.deadline == 202 &&
          latest.ready.update.state == ecg_ref32::State::DEAD,
          "the latest secondary uses its own generation and ready cycles");
}

void testRetirementBurstCapture() {
    ecg_ref32::CommitUpdateQueue queue(8, 2);
    check(queue.validConfiguration() && queue.captureWidth() == 2 &&
          queue.captureLaneBits() == 1,
          "capture width and per-slot ordering state are explicit");
    check(queue.enqueue(update(0x1000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x2000, 2), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x3000, 3), 0) ==
              ecg_ref32::EnqueueStatus::BUSY,
          "two-wide capture absorbs a retirement pair, not unlimited input");
    check(poppedWith(queue.popReady(8), 1, 0, 8) &&
          queue.popReady(8).status == ecg_ref32::PopStatus::BUSY &&
          poppedWith(queue.popReady(9), 2, 0, 8),
          "retirement burst retains one-per-cycle link output");

    ecg_ref32::CommitUpdateQueue reused(8, 2);
    check(reused.enqueue(update(0x3000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          reused.enqueue(update(0x1000, 2), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          reused.enqueue(update(0x1000, 3), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          poppedWith(reused.popReady(8), 1, 0, 8),
          "burst-order fixture releases the lowest physical slot");
    check(reused.enqueue(update(0x1000, 4), 8) ==
              ecg_ref32::EnqueueStatus::COALESCED &&
          reused.enqueue(update(0x2000, 5), 8) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          poppedWith(reused.popReady(9), 2, 0, 8) &&
          poppedWith(reused.popReady(16), 4, 8, 16) &&
          poppedWith(reused.popReady(17), 5, 8, 16),
          "same-cycle ordering follows capture order after slot reuse");

    ecg_ref32::CommitUpdateQueue wide(8, 8);
    check(wide.captureLaneBits() == 3,
          "eight-wide capture uses three ordering bits per physical slot");
    for (uint32_t lane = 0; lane < 8; ++lane) {
        check(wide.enqueue(update(0x10000 + 64 * lane, lane + 1), 0) ==
                  ecg_ref32::EnqueueStatus::ENQUEUED,
              "each configured capture lane accepts one real-cycle input");
    }
    check(wide.enqueue(update(0x20000, 9), 0) ==
              ecg_ref32::EnqueueStatus::BUSY,
          "eight-wide capture rejects a ninth same-cycle input");
    for (uint32_t lane = 0; lane < 8; ++lane) {
        const auto result = wide.popReady(8 + lane);
        check(poppedWith(result, lane + 1, 0, 8) &&
              result.ready.capture_lane == lane,
              "a captured burst drains in lane order at one update/cycle");
    }

    ecg_ref32::CommitUpdateQueue full(8, 2);
    for (uint32_t index = 0; index < 16; ++index) {
        check(full.enqueue(update(0x30000 + 64 * index, index + 1),
                           index / 2) ==
                  ecg_ref32::EnqueueStatus::ENQUEUED,
              "capture width does not increase the sixteen physical slots");
    }
    check(full.enqueue(update(0x40000, 17), 8) ==
              ecg_ref32::EnqueueStatus::FULL &&
          full.enqueue(update(0x40040, 18), 8) ==
              ecg_ref32::EnqueueStatus::FULL &&
          full.enqueue(update(0x40080, 19), 8) ==
              ecg_ref32::EnqueueStatus::BUSY,
          "each full-queue attempt consumes one bounded capture lane");
}

void testContinuouslyHotLine() {
    ecg_ref32::CommitUpdateQueue queue;
    check(queue.enqueue(update(0x5000, 10), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "hot-line oldest snapshot is enqueued");
    check(queue.enqueue(update(0x5000, 11), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "hot-line secondary snapshot is enqueued");
    for (uint64_t cycle = 2; cycle <= 7; ++cycle) {
        check(queue.enqueue(
                  update(0x5000, static_cast<uint32_t>(10 + cycle)),
                  cycle) == ecg_ref32::EnqueueStatus::COALESCED,
              "continuous hot-line input only replaces the secondary");
    }
    check(poppedWith(queue.popReady(8), 10, 0, 8),
          "continuous updates cannot starve the first hot-line snapshot");
    check(queue.enqueue(update(0x5000, 18), 8) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "after pop, the remaining secondary becomes protected oldest");
    for (uint64_t cycle = 9; cycle <= 14; ++cycle) {
        check(queue.enqueue(
                  update(0x5000, static_cast<uint32_t>(10 + cycle)),
                  cycle) == ecg_ref32::EnqueueStatus::COALESCED,
              "new hot-line inputs preserve the promoted oldest");
    }
    check(poppedWith(queue.popReady(15), 17, 7, 15),
          "the promoted hot-line snapshot also makes forward progress");
}

void testHotColdFairness() {
    ecg_ref32::CommitUpdateQueue queue;
    check(queue.enqueue(update(0x6000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x6000, 2), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x7000, 50), 2) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          queue.enqueue(update(0x6000, 3), 3) ==
              ecg_ref32::EnqueueStatus::COALESCED,
          "hot and cold lines share the bounded queue");
    check(poppedWith(queue.popReady(8), 1, 0, 8),
          "hot oldest is first by ready cycle");
    check(queue.popReady(9).status ==
              ecg_ref32::PopStatus::NOT_READY,
          "no newer update receives free early delivery");
    check(poppedWith(queue.popReady(10), 50, 2, 10),
          "cold line is not starved by hot secondary replacement");
    check(poppedWith(queue.popReady(11), 3, 3, 11),
          "hot secondary follows the earlier-ready cold line");
}

void testCapacityAndKeys() {
    ecg_ref32::CommitUpdateQueue queue;
    for (uint64_t index = 0;
         index < ecg_ref32::CommitUpdateQueue::kCapacity; ++index) {
        check(queue.enqueue(
                  update(0x10000 + index * 0x40,
                         static_cast<uint32_t>(index + 1)),
                  index) == ecg_ref32::EnqueueStatus::ENQUEUED,
              "each physical message slot can be allocated");
    }
    const auto before_ready = queue.nextReadyCycle();
    check(queue.pendingSize() ==
              ecg_ref32::CommitUpdateQueue::kCapacity &&
          queue.enqueue(update(0x20000, 100), 16) ==
              ecg_ref32::EnqueueStatus::FULL &&
          queue.pendingSize() ==
              ecg_ref32::CommitUpdateQueue::kCapacity &&
          queue.nextReadyCycle() == before_ready,
          "the seventeenth distinct message is FULL without slot loss");
    check(queue.enqueue(update(0x20040, 101), 16) ==
              ecg_ref32::EnqueueStatus::BUSY,
          "a FULL attempt consumes that cycle's ingress opportunity");

    ecg_ref32::CommitUpdateQueue physical_slots;
    check(physical_slots.enqueue(update(0x24000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "physical-slot fixture starts with one hot-line version");
    for (uint64_t index = 1;
         index < ecg_ref32::CommitUpdateQueue::kCapacity; ++index) {
        check(physical_slots.enqueue(
                  update(0x24000 + index * 0x40,
                         static_cast<uint32_t>(index + 1)),
                  index) == ecg_ref32::EnqueueStatus::ENQUEUED,
              "remaining physical slots accept independent messages");
    }
    check(physical_slots.enqueue(update(0x24000, 100), 16) ==
              ecg_ref32::EnqueueStatus::FULL &&
          physical_slots.pendingSize() ==
              ecg_ref32::CommitUpdateQueue::kCapacity,
          "a second same-line version requires a real physical slot");

    ecg_ref32::CommitUpdateQueue full_coalesce;
    check(full_coalesce.enqueue(update(0x28000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          full_coalesce.enqueue(update(0x28000, 2), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "full-coalesce fixture allocates both hot-line versions");
    for (uint64_t index = 2;
         index < ecg_ref32::CommitUpdateQueue::kCapacity; ++index) {
        check(full_coalesce.enqueue(
                  update(0x28000 + index * 0x40,
                         static_cast<uint32_t>(index + 1)),
                  index) == ecg_ref32::EnqueueStatus::ENQUEUED,
              "full-coalesce fixture fills every physical slot");
    }
    check(full_coalesce.enqueue(update(0x28000, 100), 16) ==
              ecg_ref32::EnqueueStatus::COALESCED &&
          full_coalesce.pendingSize() ==
              ecg_ref32::CommitUpdateQueue::kCapacity,
          "a full queue can replace an existing secondary without a slot");

    ecg_ref32::CommitUpdateQueue keys;
    check(keys.enqueue(update(0x30000, 7, 1, false), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          keys.enqueue(update(0x30000, 7, 2, false), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          keys.enqueue(update(0x30000, 7, 1, true), 2) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          keys.pendingSize() == 3,
          "context and secure state are part of the coalescing key");
}

void testSequenceOrdering() {
    ecg_ref32::CommitUpdateQueue wrap;
    check(wrap.enqueue(
              update(0x40000, std::numeric_limits<uint32_t>::max()), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          wrap.enqueue(update(0x40000, 0), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          wrap.enqueue(update(0x40000, 1), 2) ==
              ecg_ref32::EnqueueStatus::COALESCED,
          "modular ordering accepts wrap through semantic sequence zero");
    check(poppedWith(
              wrap.popReady(8), std::numeric_limits<uint32_t>::max(), 0, 8) &&
          poppedWith(wrap.popReady(10), 1, 2, 10),
          "wrapped versions retain oldest-then-newest output order");

    ecg_ref32::CommitUpdateQueue invalid;
    check(invalid.enqueue(update(0x50000, 0), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "zero is a valid semantic sequence");
    const auto ready_before = invalid.nextReadyCycle();
    check(invalid.enqueue(update(0x50000, 0x80000000u), 1) ==
              ecg_ref32::EnqueueStatus::INVALID_ORDER &&
          invalid.pendingSize() == 1 &&
          invalid.nextReadyCycle() == ready_before,
          "half-range ordering is rejected without queue mutation");
    check(invalid.enqueue(update(0x50000, 1), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "an invalid-order call does not consume the ingress cycle");
}

void testInvalidInputsAndTime() {
    ecg_ref32::CommitUpdateQueue queue;
    auto bad_context = update(0x60000, 1);
    bad_context.context = 0;
    auto bad_state = update(0x60000, 1);
    bad_state.state = ecg_ref32::State::WRAP;
    check(queue.enqueue(bad_context, 0) ==
              ecg_ref32::EnqueueStatus::INVALID_INPUT &&
          queue.enqueue(bad_state, 0) ==
              ecg_ref32::EnqueueStatus::INVALID_INPUT &&
          queue.pendingSize() == 0,
          "invalid payloads have no queue or ingress side effects");
    auto zero_fields = update(0, 0, 1, false, ecg_ref32::State::UNKNOWN);
    zero_fields.property_vaddr = 0;
    zero_fields.deadline = 0;
    check(queue.enqueue(zero_fields, 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "zero address, sequence, deadline, and cycle remain valid");

    ecg_ref32::CommitUpdateQueue overflow;
    check(overflow.enqueue(
              update(0x70000, 1),
              std::numeric_limits<uint64_t>::max() - 7) ==
              ecg_ref32::EnqueueStatus::INVALID_TIME &&
          overflow.pendingSize() == 0,
          "ready-cycle overflow fails without consuming a slot");
    check(overflow.enqueue(update(0x70000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "overflow rejection does not advance queue time");
    check(overflow.popReady(7).status ==
              ecg_ref32::PopStatus::NOT_READY &&
          overflow.enqueue(update(0x70040, 2), 6) ==
              ecg_ref32::EnqueueStatus::INVALID_TIME,
          "backwards absolute cycle input is rejected");

    ecg_ref32::CommitUpdateQueue bad_config(7);
    check(!bad_config.validConfiguration() &&
          bad_config.enqueue(update(0x80000, 1), 0) ==
              ecg_ref32::EnqueueStatus::INVALID_CONFIGURATION &&
          bad_config.pendingSize() == 0,
          "latency below eight cycles is rejected explicitly");
    for (const std::size_t width : {
             std::size_t{0},
             ecg_ref32::CommitUpdateQueue::kMaximumCaptureWidth + 1}) {
        ecg_ref32::CommitUpdateQueue bad_width(8, width);
        check(!bad_width.validConfiguration() &&
              bad_width.enqueue(update(0x80000, 1), 0) ==
                  ecg_ref32::EnqueueStatus::INVALID_CONFIGURATION &&
              bad_width.empty(),
              "capture width outside one through sixteen is rejected");
    }
}

void testDrainAndCancel() {
    ecg_ref32::CommitUpdateQueue drain;
    check(drain.enqueue(update(0x90000, 1), 0) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          drain.enqueue(update(0x90040, 2), 1) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          drain.enqueue(update(0x90080, 3), 2) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "drain fixture enqueues three independent messages");
    check(poppedWith(drain.popReady(10), 1, 0, 8) &&
          poppedWith(drain.popReady(11), 2, 1, 9) &&
          poppedWith(drain.popReady(12), 3, 2, 10) &&
          drain.empty(),
          "ready messages drain at no more than one per cycle");

    check(drain.enqueue(update(0xA0000, 4), 13) ==
              ecg_ref32::EnqueueStatus::ENQUEUED &&
          drain.enqueue(update(0xA0040, 5), 14) ==
              ecg_ref32::EnqueueStatus::ENQUEUED,
          "cancel fixture enqueues two messages");
    check(drain.clear() == 2 && drain.pendingSize() == 0 &&
          drain.cancelledCount() == 2 &&
          !drain.nextReadyCycle().has_value(),
          "clear returns and accumulates the exact cancellation count");
}

}  // namespace

int main() {
    testLatencyAndBandwidth();
    testTwoVersionCoalescing();
    testRetirementBurstCapture();
    testContinuouslyHotLine();
    testHotColdFairness();
    testCapacityAndKeys();
    testSequenceOrdering();
    testInvalidInputsAndTime();
    testDrainAndCancel();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
