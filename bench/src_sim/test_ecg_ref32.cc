#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cache_sim/cache_sim.h"
#include "ecg_ref32.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%-58s [%s]\n", message, condition ? "OK" : "FAIL");
    if (!condition) ++failures;
}

void testLayout() {
    check(ecg_ref32::kMaxIdBits == 18, "REF32 leaves exactly 18 destination bits");
    check(ecg_ref32::canPackRecord32(uint32_t{1} << 18),
          "n18 record fits in 32 bits");
    check(!ecg_ref32::canPackRecord32((uint32_t{1} << 18) + 1),
          "n19 record is rejected");

    const uint32_t record = ecg_ref32::packRecord32(
        (uint32_t{1} << 18) - 1, 0xA5, ecg_ref32::State::WRAP, 0xF, 18);
    const auto decoded = ecg_ref32::decodeRecord32(record, 18);
    check(decoded.destination == (uint32_t{1} << 18) - 1 &&
          ecg_ref32::extractDistanceCode(record, 18) == 0xA5 &&
          decoded.state == ecg_ref32::State::WRAP &&
          decoded.action == 0xF,
          "all n18 fields survive the 32-bit round trip");

    const uint32_t invalid = ecg_ref32::packRecord32(
        7, 0xF9, ecg_ref32::State::FINITE, 1, 18);
    check(ecg_ref32::decodeRecord32(invalid, 18).state ==
              ecg_ref32::State::UNKNOWN,
          "reserved reference codes fail closed to UNKNOWN");

    check(ecg_ref32::canPackScaleRecord32(1u << 26, 26) &&
          !ecg_ref32::canPackScaleRecord32(
              (uint64_t{1} << 26) + 1, 27),
          "scale6 fits Twitter-class n26 and rejects n27");
    const uint32_t scale_record = ecg_ref32::packScaleRecord32(
        (uint32_t{1} << 26) - 1,
        ecg_ref32::encodeScaleToken(
            1u << 20, ecg_ref32::State::WRAP),
        26);
    const auto scale_decoded =
        ecg_ref32::decodeScaleRecord32(scale_record, 26);
    check(scale_decoded.destination == (uint32_t{1} << 26) - 1 &&
          scale_decoded.state == ecg_ref32::State::WRAP &&
          scale_decoded.distance >= (1u << 20),
          "scale6 preserves destination, state, and safe distance");
}

void testQuantizer() {
    uint32_t previous = 0;
    bool monotonic = true;
    bool upper_bound = true;
    for (uint32_t distance = 1; distance <= (1u << 20); ++distance) {
        const uint8_t code = ecg_ref32::encodeDistance(distance);
        const uint32_t decoded = ecg_ref32::decodeDistanceUpper(code);
        monotonic = monotonic && decoded >= previous;
        upper_bound = upper_bound && decoded >= distance;
        previous = decoded;
    }
    check(monotonic, "reference-distance decode is monotonic");
    check(upper_bound, "reference-distance decode never expires early");
    check(ecg_ref32::decodeDistanceUpper(
              ecg_ref32::encodeDistance(1)) == 1,
          "distance one remains exact");
    check(ecg_ref32::encodeDistance(
              static_cast<uint64_t>(ecg_ref32::kMaxFiniteDistance) + 1) ==
              ecg_ref32::saturatedDistanceCode(8) &&
          ecg_ref32::decodeDistanceUpper(
              ecg_ref32::saturatedDistanceCode(8)) ==
              ecg_ref32::kMaxFiniteDistance,
          "far distances saturate safely");
    for (uint32_t bits : {8u, 10u, 12u}) {
        uint32_t prior = 0;
        bool precise_monotonic = true;
        bool precise_upper = true;
        for (uint32_t distance = 1; distance <= (1u << 20); ++distance) {
            const uint32_t decoded = ecg_ref32::decodeDistanceUpper(
                ecg_ref32::encodeDistance(distance, bits), bits);
            precise_monotonic =
                precise_monotonic && decoded >= prior;
            precise_upper =
                precise_upper && decoded >= distance;
            prior = decoded;
        }
        check(precise_monotonic && precise_upper,
              bits == 8 ? "8-bit reference quantizer is safe" :
              bits == 10 ? "10-bit reference quantizer is safe" :
                           "12-bit reference quantizer is safe");
    }
    for (uint32_t distance : {
            1u << 20, 1u << 24, 1u << 28,
            ecg_ref32::kMaxFiniteDistance}) {
        check(ecg_ref32::decodeDistanceUpper(
                  ecg_ref32::encodeDistance(distance, 10), 10) >= distance,
              "10-bit reference covers long graph traces");
    }
    check(ecg_ref32::canPackRecord32(1u << 18, 10, 2) &&
          ecg_ref32::canPackRecord32(1u << 18, 12, 0) &&
          !ecg_ref32::canPackRecord32(1u << 18, 10, 4),
          "only exact 14-bit metadata allocations are accepted");
}

void testBuilder() {
    const std::vector<uint64_t> offsets = {0, 2, 4};
    const std::vector<uint32_t> destinations = {0, 1, 16, 0};
    ecg_ref32::FlatRecords built;
    check(ecg_ref32::buildFlatRecordsFromDestinations(
              32, 16, offsets, destinations, built),
          "flat trace builder accepts a valid two-row trace");
    if (built.records.size() != destinations.size()) {
        check(false, "flat trace builder returns one record per edge");
        return;
    }
    check(built.offsets == offsets &&
          built.exact_distances == std::vector<uint32_t>({1, 2, 4, 1}),
          "builder preserves rows and exact line-level distances");
    check(ecg_ref32::extractState(built.records[0], 5) ==
              ecg_ref32::State::FINITE &&
          ecg_ref32::extractState(built.records[1], 5) ==
              ecg_ref32::State::FINITE &&
          ecg_ref32::extractState(built.records[2], 5) ==
              ecg_ref32::State::WRAP &&
          ecg_ref32::extractState(built.records[3], 5) ==
              ecg_ref32::State::WRAP,
          "builder distinguishes same-sweep and wrap references");
    bool destinations_match = true;
    for (size_t i = 0; i < destinations.size(); ++i) {
        destinations_match = destinations_match &&
            ecg_ref32::extractDestination(built.records[i], 5) ==
                destinations[i] &&
            ecg_ref32::extractAction(built.records[i], 5) == 0;
    }
    check(destinations_match,
          "records substitute destinations and default to no prefetch");

    std::vector<uint64_t> long_offsets = {0, 20};
    std::vector<uint32_t> long_destinations;
    for (uint32_t line = 0; line < 20; ++line)
        long_destinations.push_back(line * 16);
    ecg_ref32::FlatRecords action4;
    ecg_ref32::FlatRecords action2;
    check(ecg_ref32::buildFlatRecordsFromDestinations(
              512, 16, long_offsets, long_destinations, action4, 8, 4) &&
          ecg_ref32::actionDelta(
              ecg_ref32::extractAction(action4.records[0], 9, 8, 4),
              4) == 10,
          "4-bit action selects the best record 8-15 ahead");
    check(ecg_ref32::buildFlatRecordsFromDestinations(
              512, 16, long_offsets, long_destinations, action2, 10, 2) &&
          ecg_ref32::actionDelta(
              ecg_ref32::extractAction(action2.records[0], 9, 10, 2),
              2) == 8,
          "2-bit action maps to a fixed bounded lookahead");

    ecg_ref32::FlatRecords scale;
    check(ecg_ref32::buildFlatScaleRecordsFromDestinations(
              512, 16, long_offsets, long_destinations, scale, 26) &&
          ecg_ref32::decodeScaleRecord32(
              scale.records[0], 26).destination ==
              long_destinations[0] &&
          ecg_ref32::selectScalePrefetchDelta(
              scale.records, 0, 26) >= 8,
          "scale6 builder supports derived bounded lookahead");
    std::vector<int32_t> inplace(
        long_destinations.begin(), long_destinations.end());
    check(ecg_ref32::buildScaleRecordsInPlace(
              inplace.data(), inplace.size(), 512, 16, 26) &&
          std::equal(
              scale.records.begin(), scale.records.end(),
              inplace.begin(),
              [](uint32_t expected, int32_t actual) {
                  return expected == static_cast<uint32_t>(actual);
              }),
          "in-place scale6 builder matches the flat reference");
}

void testFreshnessAndVictims() {
    check(ecg_ref32::resolveQuantizedFuture(
              ecg_ref32::State::FINITE, 100, 100).state ==
              ecg_ref32::State::FINITE &&
          ecg_ref32::resolveQuantizedFuture(
              ecg_ref32::State::FINITE, 100, 101).state ==
              ecg_ref32::State::UNKNOWN,
          "current use stays live and a passed use becomes UNKNOWN");
    check(ecg_ref32::resolveQuantizedFuture(
              ecg_ref32::State::FINITE, 2, (1u << 21) - 1,
              21).remaining == 3,
          "21-bit deadline arithmetic survives sequence wrap");
    check(ecg_ref32::resolveQuantizedFuture(
              ecg_ref32::State::FINITE, 2, UINT32_MAX,
              32).remaining == 3,
          "32-bit Twitter deadline survives sequence wrap");

    ecg_ref32::WayState ways[4];
    ways[0] = {true, 7, 40, 3, ecg_ref32::State::FINITE, 120, 120};
    ways[1] = {false, 7, 10, 0, ecg_ref32::State::UNKNOWN, 0, 0};
    ways[2] = {true, 7, 30, 1, ecg_ref32::State::DEAD, 0, 0};
    ways[3] = {true, 7, 20, 2, ecg_ref32::State::FINITE, 180, 180};
    ecg_ref32::VictimReason reason;
    check(ecg_ref32::selectVictim(
              ways, 4, 100, false, &reason) == 2 &&
          reason == ecg_ref32::VictimReason::DEAD_PROPERTY,
          "known-dead property outranks non-property data");

    ways[2].state = ecg_ref32::State::FINITE;
    ways[2].quantized_deadline = 110;
    check(ecg_ref32::selectVictim(
              ways, 4, 100, false, &reason) == 1 &&
          reason == ecg_ref32::VictimReason::NON_PROPERTY,
          "non-property data outranks live property data");

    ways[1].property = true;
    ways[1].grasp_tier = 3;
    ways[1].state = ecg_ref32::State::UNKNOWN;
    check(ecg_ref32::selectVictim(
              ways, 4, 100, false, &reason) == 1 &&
          reason == ecg_ref32::VictimReason::UNKNOWN_PROPERTY,
          "cold UNKNOWN property is the safe fallback victim");

    ways[1].state = ecg_ref32::State::FINITE;
    ways[1].quantized_deadline = 105;
    ways[1].exact_deadline = 105;
    ways[0].rrpv = ways[2].rrpv = ways[3].rrpv = 0;
    ways[1].rrpv = 0;
    check(ecg_ref32::selectVictim(
              ways, 4, 100, true, &reason) == 3 &&
          reason == ecg_ref32::VictimReason::FINITE_PROPERTY,
          "exact diagnostic selects the farthest finite use");
}

void testCacheIntegration() {
    constexpr uint64_t property_base = 0x10000;
    cache_sim::GraphCacheContext context;
    context.num_regions = 1;
    context.regions[0].base_address = property_base;
    context.regions[0].upper_bound = property_base + 4096;
    context.regions[0].elem_size = 4;
    context.regions[0].grasp_region = true;
    context.regions[0].grasp_hot_percent = 15;
    context.mask_config.enabled = true;
    context.mask_config.ecg_mode = cache_sim::ECGMode::ECG_REF32;

    cache_sim::CacheLevel l3(
        "L3", 1024, 64, 2, cache_sim::EvictionPolicy::ECG);
    l3.initGraphContext(&context);

    auto& hints = context.hints_for_thread();
    hints.edge_ref_sequence = 10;
    hints.edge_ref_state =
        static_cast<uint8_t>(ecg_ref32::State::DEAD);
    hints.edge_ref_valid = true;
    l3.insert(property_base, false);
    check(!l3.contains(property_base) &&
          l3.getRef32DeadBypasses() == 1,
          "governed DEAD miss bypasses the LLC");

    hints.edge_ref_state =
        static_cast<uint8_t>(ecg_ref32::State::FINITE);
    hints.edge_ref_distance = 12;
    l3.insert(property_base, false);
    check(l3.contains(property_base),
          "governed finite miss allocates in the LLC");
    check(l3.access(property_base, false) &&
          l3.getRef32GovernedHits() == 1,
          "governed LLC hit is counted and refreshes metadata");
    check(!l3.access(property_base + 64, false) &&
          l3.getRef32GovernedMisses() == 1,
          "governed LLC miss is counted independently");
}

void testNonPowerOfTwoSharedCacheGeometry() {
    cache_sim::CacheLevel cache(
        "L3", 3 * 2 * 64, 64, 2, cache_sim::EvictionPolicy::LRU);
    check(cache.getNumSets() == 3 &&
          cache.setIndexForAddress(0) == 0 &&
          cache.setIndexForAddress(64) == 1 &&
          cache.setIndexForAddress(128) == 2 &&
          cache.setIndexForAddress(192) == 0,
          "non-power-of-two LLC uses modulo set indexing");

    cache.insert(0, false);
    cache.insert(192, false);
    check(cache.contains(0) && cache.contains(192),
          "non-power-of-two LLC tags distinguish lines in one set");
}

void testDelayedLlcOnlyPrefetch() {
    setenv("ECG_REF32_PREFETCH", "1", 1);
    setenv("ECG_REF32_PREFETCH_QUEUE", "8", 1);
    setenv("ECG_REF32_PREFETCH_LATENCY", "8", 1);
    setenv("ECG_REF32_PREFETCH_BANDWIDTH", "1", 1);
    setenv("ECG_REF32_PREFETCH_INTERVAL", "8", 1);
    setenv("ECG_REF32_DEADLINE_BITS", "21", 1);
    setenv("CACHE_ECG_EPOCH_REGION_INDEX", "0", 1);

    alignas(4096) static float property[64] = {};
    const uint64_t base = reinterpret_cast<uint64_t>(property);
    const uint64_t current = reinterpret_cast<uint64_t>(&property[0]);
    const uint64_t target = reinterpret_cast<uint64_t>(&property[16]);
    const uint64_t progress = reinterpret_cast<uint64_t>(&property[32]);

    cache_sim::GraphCacheContext context;
    context.num_regions = 1;
    context.regions[0].base_address = base;
    context.regions[0].upper_bound = base + sizeof(property);
    context.regions[0].elem_size = sizeof(float);
    context.regions[0].grasp_region = true;
    context.regions[0].grasp_hot_percent = 15;
    context.mask_config.enabled = true;
    context.mask_config.ecg_mode = cache_sim::ECGMode::ECG_REF32;

    cache_sim::CacheHierarchy cache(
        256, 2, 512, 2, 1024, 2, 64,
        cache_sim::EvictionPolicy::LRU,
        cache_sim::EvictionPolicy::LRU,
        cache_sim::EvictionPolicy::ECG);
    cache.initGraphContext(&context);
    cache.resetStats();

    auto& hints = context.hints_for_thread();
    hints.edge_ref_valid = true;
    hints.edge_ref_state =
        static_cast<uint8_t>(ecg_ref32::State::FINITE);
    hints.edge_ref_distance = 32;
    hints.edge_ref_sequence = 0;
    hints.edge_ref_action = 8;
    hints.edge_ref_prefetch_address = target;
    hints.edge_ref_prefetch_valid = true;
    cache.access(current, false);

    hints.edge_ref_action = 0;
    hints.edge_ref_prefetch_valid = false;
    for (uint64_t sequence = 1; sequence < 8; ++sequence) {
        hints.edge_ref_sequence = sequence;
        cache.access(current, false);
    }
    hints.edge_ref_sequence = 8;
    cache.access(progress, false);
    check(!cache.L1()->contains(target) &&
          !cache.L2()->contains(target) &&
          cache.L3()->contains(target),
          "completed REF32 prefetch fills only the LLC");

    hints.edge_ref_sequence = 9;
    cache.access(target, false);
    cache.flushRef32CommitUpdates();
    const std::string json = cache.toJSON();
    check(json.find("\"ecg_ref32_prefetch_requests_issued\": 1") !=
              std::string::npos &&
          json.find("\"prefetch_useful\": 1") != std::string::npos &&
          json.find("\"ecg_ref32_prefetch_pending\": 0") !=
              std::string::npos,
          "delayed REF32 prefetch is useful and fully drained");
}

}  // namespace

int main() {
    testLayout();
    testQuantizer();
    testBuilder();
    testFreshnessAndVictims();
    testCacheIntegration();
    testNonPowerOfTwoSharedCacheGeometry();
    testDelayedLlcOnlyPrefetch();
    std::printf("REF32 TESTS: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
