#include <cstdint>
#include <cstdio>
#include <memory>

#include "mem/cache/replacement_policies/ecg_ref32_observation.hh"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%-68s [%s]\n", message, condition ? "OK" : "FAIL");
    if (!condition)
        ++failures;
}

gem5::RequestPtr observed(
        gem5::RequestorID requestor, uint16_t context,
        uint32_t sequence, uint32_t destination) {
    auto request = std::make_shared<gem5::Request>();
    request->requestorId(requestor);
    gem5::replacement_policy::graph::attachEcgRef32Observation(
        request, context, sequence, destination);
    return request;
}

gem5::RequestPtr ordinary(gem5::RequestorID requestor) {
    auto request = std::make_shared<gem5::Request>();
    request->requestorId(requestor);
    return request;
}

void testNeutralAndNewestMerge() {
    using namespace gem5::replacement_policy::graph;
    EcgRef32ObservationMshrState state;
    state.merge(ordinary(1));
    state.merge(observed(1, 1, 10, 7));
    state.merge(ordinary(1));
    state.merge(observed(1, 1, 9, 7));
    state.merge(observed(1, 1, 11, 8));
    check(state.valid() && !state.conflicted() &&
          state.selected().sequence == 11 &&
          state.selected().destination == 8,
          "ordinary targets are neutral and modular newest observation wins");

    auto response = ordinary(1);
    state.apply(response);
    EcgRef32Observation merged;
    check(readEcgRef32Observation(response, merged) &&
          !merged.conflict && merged.context == 1 &&
          merged.sequence == 11 && merged.destination == 8,
          "merged observation copies to the downstream response");
}

void testWrapAndConflicts() {
    using namespace gem5::replacement_policy::graph;
    EcgRef32ObservationMshrState wrap;
    wrap.merge(observed(1, 1, 0xffffffffu, 3));
    wrap.merge(observed(1, 1, 0, 4));
    check(wrap.valid() && !wrap.conflicted() &&
          wrap.selected().sequence == 0 &&
          wrap.selected().destination == 4,
          "MSHR observation ordering accepts wrap through zero");

    EcgRef32ObservationMshrState equal;
    equal.merge(observed(1, 1, 20, 5));
    equal.merge(observed(1, 1, 20, 6));
    check(equal.conflicted(),
          "equal sequence with differing destination conflicts");

    EcgRef32ObservationMshrState context;
    context.merge(observed(1, 1, 20, 5));
    context.merge(observed(1, 2, 21, 5));
    check(context.conflicted(), "cross-context merge conflicts");

    EcgRef32ObservationMshrState requestor;
    requestor.merge(observed(1, 1, 20, 5));
    requestor.merge(observed(2, 1, 21, 5));
    check(requestor.conflicted(), "cross-requestor merge conflicts");

    EcgRef32ObservationMshrState half;
    half.merge(observed(1, 1, 0, 5));
    half.merge(observed(1, 1, 0x80000000u, 5));
    check(half.conflicted(), "half-range merge conflicts");
}

void testResetAndConflictPropagation() {
    using namespace gem5::replacement_policy::graph;
    EcgRef32ObservationMshrState state;
    state.merge(observed(1, 1, 4, 2));
    state.merge(observed(2, 1, 5, 2));
    auto response = ordinary(1);
    state.apply(response);
    EcgRef32Observation merged;
    check(readEcgRef32Observation(response, merged) && merged.conflict,
          "conflict state propagates to the response");

    state.reset();
    auto plain = ordinary(1);
    state.merge(plain);
    state.apply(plain);
    check(!readEcgRef32Observation(plain, merged),
          "reset plus ordinary targets produces no observation");
    state.apply(response);
    check(!readEcgRef32Observation(response, merged),
          "empty rebuilt targets clear an older response observation");
}

} // namespace

int main() {
    testNeutralAndNewestMerge();
    testWrapAndConflicts();
    testResetAndConflictPropagation();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
