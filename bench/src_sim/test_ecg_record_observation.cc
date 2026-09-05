#include <cstdint>
#include <cstdio>
#include <memory>

#include "mem/cache/prefetch/ecg_record_prefetch_request.hh"
#include "mem/cache/replacement_policies/ecg_record_observation.hh"

namespace {
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::printf("%s [FAIL]\n", message);
        ++failures;
    }
}

gem5::RequestPtr observed(uint64_t sequence, uint64_t generation=1, bool dead=false) {
    auto request = std::make_shared<gem5::Request>();
    request->requestorId(1);
    ecg_record::NativeLoadResult load;
    load.sequence = sequence;
    load.generation = generation;
    load.context = 1;
    load.destination = (uint64_t{1} << 32) + 3;
    load.property_address = 0x1000 + load.destination * 4;
    load.state = dead ? ecg_record::State::DEAD : ecg_record::State::FINITE;
    gem5::replacement_policy::graph::attachEcgRecordObservation(request, load);
    return request;
}
}

int main() {
    using namespace gem5::replacement_policy::graph;
    const uint64_t high = (uint64_t{1} << 40);
    EcgRecordObservationMshrState state;
    state.merge(observed(high + 1));
    state.merge(observed(high + 3, 1, true));
    state.merge(observed(high + 2));
    check(state.valid() && !state.conflicted() && state.selected().sequence == high + 3 &&
          state.selected().destination == (uint64_t{1} << 32) + 3,
          "MSHR ordering preserves high sequence and destination bits");
    auto response = std::make_shared<gem5::Request>();
    state.apply(response);
    check(isEcgRecordDead(response), "latest known-DEAD request is eligible for bypass");
    state.merge(observed(high + 4));
    state.apply(response);
    check(!isEcgRecordDead(response), "a later live request is not marked as DEAD");
    state.merge(observed(high + 5, 2));
    state.apply(response);
    check(state.conflicted() && !isEcgRecordDead(response),
          "cross-generation coalescing fails closed instead of discarding high bits");
    state.reset();
    state.apply(response);
    EcgRecordObservation observation;
    check(!readEcgRecordObservation(response, observation),
          "a rebuilt empty target list removes stale response observations");
    state.merge(observed(0));
    check(state.conflicted(), "zero is not a valid linear semantic position");

    auto decision = std::make_shared<gem5::EcgRecordFillDecision>();
    decision->ticket = high;
    response->setExtension(std::make_shared<gem5::EcgRecordPrefetchExtension>(decision));
    auto clone = std::make_shared<gem5::Request>(*response);
    decision->ready = true;
    decision->admitted = false;
    check(gem5::ecgRecordFillDecision(clone) == decision &&
          gem5::ecgRecordFillDecision(clone)->ready &&
          !gem5::ecgRecordFillDecision(clone)->admitted,
          "cloned requests share their exact timed fill decision, not a last-request mailbox");
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures != 0;
}
