#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cache_sim/cache_sim.h"
#include "kernel_census_receipt.h"

bool exerciseHierarchy(uint8_t bytes, ecg_record::Mechanism mechanism) {
    using namespace cache_sim;
    constexpr uint64_t vertices = 512;
    constexpr uint64_t records = 32;
    alignas(64) std::array<float, vertices> properties{};
    std::vector<uint32_t> degrees(vertices, 1);
    ecg_record::Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.record_count = records;
    requirements.traversal_count = 2;
    requirements.requested_record_bytes = bytes;
    ecg_record::Layout layout;
    ecg_record::RecordStream stream;
    if (ecg_record::selectLayout(requirements, layout) != ecg_record::Status::OK ||
        ecg_record::buildRecords(requirements, layout, 16,
            [](std::size_t index) { return uint64_t(index * 16); }, stream) != ecg_record::Status::OK)
        return false;
    GraphCacheContext context;
    context.initTopology(degrees.data(), vertices, records, true);
    context.registerPropertyArray(properties.data(), vertices, 4, 128, 0.15, true);
    const bool replacement = mechanism == ecg_record::Mechanism::REPLACEMENT ||
        mechanism == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
    CacheHierarchy cache(64, 1, 128, 1, 128, 2, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU,
        replacement ? EvictionPolicy::ECG : EvictionPolicy::LRU);
    cache.initGraphContext(&context);
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    configuration.record_base = reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = reinterpret_cast<uint64_t>(properties.data());
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = 1;
    configuration.generation = 1;
    configuration.control = ecg_record::kNativeEnable;
    cache.configureRecord(configuration, stream, mechanism);
    for (uint64_t iteration = 0; iteration < 2; ++iteration) {
        cache.recordIteration(iteration * records, iteration == 0);
        for (uint64_t index = 0; index < records; ++index) {
            const uint64_t word = cache.recordLoad(index);
            if (cache.recordProperty(index, word) != index * 16)
                return false;
        }
    }
    cache.finishRecord(records * 2);
    const std::string json = cache.toJSON();
    if (json.find("\"ecg_mode_effective\": \"RECORD\"") == std::string::npos)
        return false;
    for (const char* field : {"\"ecg_record_victim_rrpv_ordered\": ",
                              "\"ecg_record_victim_rrpv_changed\": ",
                              "\"ecg_record_victim_base_lru\": "})
        if (json.find(field) == std::string::npos)
            return false;
    return (mechanism != ecg_record::Mechanism::PREFETCH &&
            mechanism != ecg_record::Mechanism::REPLACEMENT_PREFETCH) ||
        cache.getPrefetchRequests() > 0;
}

bool exerciseFilteredHierarchy(uint8_t bytes, ecg_record::Mechanism mechanism) {
    using namespace cache_sim;
    using namespace ecg_record;
    constexpr uint64_t vertices = 32, records = 4;
    alignas(64) std::array<uint64_t, vertices> distances{};
    alignas(64) std::array<uint32_t, vertices> depths{};
    std::vector<uint32_t> degrees(vertices, 1);
    Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.record_count = records;
    requirements.requested_record_bytes = bytes;
    const uint32_t ids[] = {0, 8, 0, 16};
    Layout layout;
    PropertyDescriptor property{PropertyKind::U64, 8, TraversalMode::ORDERED_FILTERED};
    RecordStream stream;
    if (selectLayout(requirements, layout) != Status::OK ||
        buildRecords(requirements, layout, property, reinterpret_cast<uint64_t>(distances.data()),
            [&](std::size_t i) { return ids[i]; }, stream) != Status::OK)
        return false;
    const bool replacement = mechanism == Mechanism::REPLACEMENT ||
        mechanism == Mechanism::REPLACEMENT_PREFETCH;
    CacheHierarchy cache(128, 2, 256, 2, 256, 2, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU,
        replacement ? EvictionPolicy::ECG : EvictionPolicy::LRU);
    GraphCacheContext context;
    context.initTopology(degrees.data(), vertices, 12, true);
    context.registerPropertyArray(distances.data(), vertices, 8, 256, 0.15, true);
    context.registerPropertyArray(depths.data(), vertices, 4, 256, 0.15, true);
    cache.initGraphContext(&context);
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    packProperty(property, configuration.property_descriptor);
    configuration.record_base = reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = reinterpret_cast<uint64_t>(distances.data());
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable | kNativeManagedPasses;
    cache.configureRecord(configuration, stream, mechanism);
    cache.recordBeginPass();
    auto word = cache.recordLoad(0);
    if (cache.recordProperty(0, word) != 0)
        return false;
    cache.readArray(distances.data(), 0);
    cache.writeArray(distances.data(), 0);
    word = cache.recordLoad(2);
    if (cache.recordProperty(2, word) != 0)
        return false;
    try {
        cache.rebindRecord(configuration, stream);
        return false;
    } catch (const std::logic_error&) {}
    cache.recordClosePass();
    cache.recordBeginPass();
    cache.recordClosePass();

    property = {PropertyKind::U32, 4, TraversalMode::ORDERED_FILTERED};
    RecordStream next_stream;
    if (buildRecords(requirements, layout, property, reinterpret_cast<uint64_t>(depths.data()),
            [&](std::size_t i) { return ids[i]; }, next_stream) != Status::OK)
        return false;
    packProperty(property, configuration.property_descriptor);
    configuration.record_base = reinterpret_cast<uint64_t>(next_stream.data());
    configuration.property_base = reinterpret_cast<uint64_t>(depths.data());
    configuration.generation = 2;
    cache.rebindRecord(configuration, next_stream);
    cache.recordBeginPass();
    word = cache.recordLoad(3);
    if (cache.recordProperty(3, word) != 16)
        return false;
    cache.recordClosePass();
    cache.finishRecord(3);
    return true;
}

bool exerciseGraspRecordBase(uint8_t bytes, bool replacement) {
    using namespace cache_sim;
    using namespace ecg_record;
    constexpr uint64_t vertices = 256, records = 8;
    alignas(64) std::array<uint32_t, vertices> properties{};
    std::vector<uint32_t> degrees(vertices, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), vertices, records, true);
    context.registerPropertyArray(
        properties.data(), vertices, 4, 256, 0.50, true);
    CacheLevel grasp("L3", 256, 64, 4, EvictionPolicy::GRASP);
    CacheLevel prepared("L3", 256, 64, 4, EvictionPolicy::GRASP);
    grasp.initGraphContext(&context);
    prepared.initGraphContext(&context);
    prepared.prepareRecord(EvictionPolicy::GRASP);
    const uint64_t base = reinterpret_cast<uint64_t>(properties.data());
    for (const uint64_t address : {base, base + 128, base + 256}) {
        grasp.insert(address, false);
        prepared.insert(address, false);
        CacheLine expected, actual;
        if (!grasp.lineSnapshotForTest(address, expected) ||
            !prepared.lineSnapshotForTest(address, actual) ||
            expected.rrpv != actual.rrpv)
            return false;
        grasp.access(address, false);
        prepared.access(address, false);
        if (!grasp.lineSnapshotForTest(address, expected) ||
            !prepared.lineSnapshotForTest(address, actual) ||
            expected.rrpv != actual.rrpv)
            return false;
    }
    std::vector<CacheLine> ordinary_set(4), prepared_set(4);
    for (std::size_t index = 0; index < ordinary_set.size(); ++index) {
        ordinary_set[index].valid = prepared_set[index].valid = true;
        ordinary_set[index].line_addr = prepared_set[index].line_addr =
            base + index * 64;
        ordinary_set[index].rrpv = prepared_set[index].rrpv =
            static_cast<uint8_t>(index + 1);
    }
    if (grasp.selectVictimForTest(ordinary_set) !=
            prepared.selectVictimForTest(prepared_set))
        return false;
    for (std::size_t index = 0; index < ordinary_set.size(); ++index)
        if (ordinary_set[index].rrpv != prepared_set[index].rrpv)
            return false;

    Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.record_count = records;
    requirements.requested_record_bytes = bytes;
    Layout layout;
    if (selectLayout(requirements, layout) != Status::OK)
        return false;
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    packProperty(
        {PropertyKind::U32, 4, TraversalMode::ORDERED_FILTERED},
        configuration.property_descriptor);
    configuration.record_base = base + 4096;
    configuration.property_base = base;
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable | kNativeManagedPasses;
    CacheLevel configured("L3", 256, 64, 4, EvictionPolicy::GRASP);
    configured.initGraphContext(&context);
    configured.prepareRecord(EvictionPolicy::GRASP);
    configured.configureRecord(
        configuration, replacement, EvictionPolicy::GRASP);
    configured.advanceRecordProgress(1);
    ordinary_set.assign(4, CacheLine{});
    auto configured_set = ordinary_set;
    for (std::size_t index = 0; index < ordinary_set.size(); ++index) {
        ordinary_set[index].valid = configured_set[index].valid = true;
        ordinary_set[index].line_addr = configured_set[index].line_addr =
            base + index * 64;
        ordinary_set[index].rrpv = configured_set[index].rrpv =
            static_cast<uint8_t>(index + 1);
        ordinary_set[index].last_access = configured_set[index].last_access =
            index + 1;
    }
    auto invalid_set = configured_set;
    invalid_set[1].valid = false;
    const auto invalid_before = invalid_set;
    if (configured.selectVictimForTest(invalid_set) != 1)
        return false;
    for (std::size_t index = 0; index < invalid_set.size(); ++index)
        if (invalid_set[index].rrpv != invalid_before[index].rrpv)
            return false;
    const std::size_t expected = grasp.selectVictimForTest(ordinary_set);
    const std::size_t actual = configured.selectVictimForTest(configured_set);
    if (expected != actual)
        return false;
    for (std::size_t index = 0; index < ordinary_set.size(); ++index)
        if (ordinary_set[index].rrpv != configured_set[index].rrpv)
            return false;
    if (replacement) {
        configured_set[2].record_metadata.state = LineState::DEAD;
        const auto before = configured_set;
        if (configured.selectVictimForTest(configured_set) != 2)
            return false;
        for (std::size_t index = 0; index < configured_set.size(); ++index)
            if (configured_set[index].rrpv != before[index].rrpv)
                return false;
    }
    return true;
}

// The duel gate's selector trains on the off-chip transfers of two fixed
// leader slots, and only while the record rule is active; followers take its
// MSB. Returns zero, or the number of the first check that failed.
int exerciseRecordPressureDuel() {
    using namespace cache_sim;
    using namespace ecg_record;
    // Fewer than 64 sets, or a count that is not a multiple of 64, would give
    // one leader slot more sets than the other, so the gate refuses both.
    for (const std::size_t sets : {std::size_t{32}, std::size_t{96}}) {
        CacheLevel refused("L3", sets * 64, 64, 1, EvictionPolicy::GRASP);
        try {
            refused.setRecordPressureGate(RecordPressureGate::DUEL);
            return 1;
        } catch (const std::invalid_argument&) {}
    }
    constexpr uint64_t vertices = 256, records = 8;
    constexpr std::size_t sets = 128;
    alignas(64) static std::array<uint32_t, vertices> properties{};
    std::vector<uint32_t> degrees(vertices, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), vertices, records, true);
    context.registerPropertyArray(properties.data(), vertices, 4, 256, 0.50, true);
    Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.record_count = records;
    Layout layout;
    if (selectLayout(requirements, layout) != Status::OK)
        return 2;
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    configuration.record_base = 0x20000000;
    configuration.property_base = reinterpret_cast<uint64_t>(properties.data());
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable;
    // Lines away from the property array, placed by set and tag.
    const auto line = [](std::size_t set, uint64_t tag) {
        return uint64_t{0x10000000} + ((tag * sets + set) << 6);
    };
    const auto make = [&](RecordPressureGate gate) {
        auto cache = std::make_unique<CacheLevel>(
            "L3", sets * 2 * 64, 64, 2, EvictionPolicy::GRASP);
        cache->initGraphContext(&context);
        cache->prepareRecord(EvictionPolicy::GRASP);
        cache->setRecordPressureGate(gate);
        return cache;
    };

    auto duel = make(RecordPressureGate::DUEL);
    const auto& a = duel->getRecordVictimAttribution();
    if (duel->getRecordDuelSelector() != 511)
        return 3;
    // Slot 16 of every 64 sets always runs the rule and slot 17 always
    // relaxes; every other set follows the selector, which starts pressured.
    for (const std::size_t set : {std::size_t{16}, std::size_t{80}})
        if (!duel->recordPressuredForTest(set) || duel->recordPressuredForTest(set + 1))
            return 4;
    for (const std::size_t set : {std::size_t{0}, std::size_t{15}, std::size_t{18}, std::size_t{127}})
        if (!duel->recordPressuredForTest(set))
            return 5;
    // Before the rule is bound both leader groups run the base policy, so
    // their misses carry no information and must not train the selector.
    duel->access(line(16, 0), false);
    if (duel->getRecordDuelSelector() != 511 || a.duel_transfers != 0)
        return 6;
    duel->configureRecord(configuration, true, EvictionPolicy::GRASP);
    // A demand miss in a rule leader counts up and relaxes the followers; one
    // in a base leader counts down and presses them again. A follower miss is
    // a transfer but never trains, and the leaders ignore the selector.
    duel->access(line(16, 1), false);
    if (duel->getRecordDuelSelector() != 512 || duel->recordPressuredForTest(0) ||
        !duel->recordPressuredForTest(16) || duel->recordPressuredForTest(17))
        return 7;
    duel->access(line(17, 1), false);
    duel->access(line(3, 1), false);
    if (duel->getRecordDuelSelector() != 511 || !duel->recordPressuredForTest(0) ||
        a.duel_transfers != 3 || a.duel_leader_transfers_rule != 1 ||
        a.duel_leader_transfers_base != 1 || a.duel_winner_changes != 2)
        return 8;
    duel->insert(line(16, 2), false);
    duel->access(line(16, 2), false);
    if (duel->getRecordDuelSelector() != 511 || a.duel_transfers != 3)
        return 9;
    // A dirty victim is the other transfer. It trains exactly as a miss does,
    // and a rule leader's victim goes through the rule, not the base.
    const uint64_t unpressured = a.unpressured;
    duel->insert(line(80, 1), true);
    duel->insert(line(80, 2), true);
    duel->insert(line(80, 3), false);
    if (duel->getStats().writebacks.load() != 1 || duel->getRecordDuelSelector() != 512 ||
        a.duel_transfers != 4 || a.duel_leader_transfers_rule != 2 ||
        a.unpressured != unpressured)
        return 10;
    // Relaxed, a follower's victim is the base's, attributed as unpressured.
    duel->insert(line(3, 2), false);
    duel->insert(line(3, 3), false);
    duel->insert(line(3, 4), false);
    if (a.duel_follower_base != 1 || a.duel_follower_rule != 0 ||
        a.unpressured != unpressured + 1)
        return 11;
    // A base leader always relaxes, and its writeback counts down.
    duel->insert(line(81, 1), true);
    duel->insert(line(81, 2), true);
    duel->insert(line(81, 3), false);
    if (duel->getRecordDuelSelector() != 511 || a.duel_leader_transfers_base != 2 ||
        a.unpressured != unpressured + 2 || a.duel_winner_changes != 4)
        return 12;
    // Pressed again, a follower's victim goes through the rule.
    duel->insert(line(4, 1), false);
    duel->insert(line(4, 2), false);
    duel->insert(line(4, 3), false);
    if (a.duel_follower_rule != 1 || a.duel_follower_base != 1 ||
        a.unpressured != unpressured + 2)
        return 13;
    // Ten bits saturate at both ends, flipping the followers once each way.
    for (uint64_t miss = 0; miss < 600; ++miss)
        duel->access(line(16, 100 + miss), false);
    if (duel->getRecordDuelSelector() != 1023 || a.duel_winner_changes != 5)
        return 14;
    for (uint64_t miss = 0; miss < 1100; ++miss)
        duel->access(line(17, 100 + miss), false);
    if (duel->getRecordDuelSelector() != 0 || a.duel_winner_changes != 6 ||
        a.duel_transfers != 1705 || duel->getStats().writebacks.load() != 2)
        return 15;
    // Without the replacement rule, or under the counter gate, nothing trains.
    for (const auto& [replacement, gate] : {std::pair{false, RecordPressureGate::DUEL},
                                            std::pair{true, RecordPressureGate::COUNTER}}) {
        auto idle = make(gate);
        idle->configureRecord(configuration, replacement, EvictionPolicy::GRASP);
        idle->access(line(16, 1), false);
        idle->access(line(16, 2), false);
        idle->access(line(17, 1), false);
        if (idle->getRecordDuelSelector() != 511 ||
            idle->getRecordVictimAttribution().duel_transfers != 0)
            return 16;
    }
    return 0;
}

// PageRank configures its record arm with no base policy, which the cache
// takes as LRU, so its base victim today is the least recently used way.
// Under the RRPV order that arm must never reach the LRU scan: with no governed
// way to consult it evicts and ages exactly as GRASP does. The GRASP-based arm
// with governed-first must do the same instead of taking the least recent
// non-governed way. Returns zero, or the number of the first check that failed.
int exerciseRrpvOrderWithoutLru() {
    using namespace cache_sim;
    using namespace ecg_record;
    constexpr uint64_t vertices = 256, records = 8;
    alignas(64) std::array<uint32_t, vertices> properties{};
    std::vector<uint32_t> degrees(vertices, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), vertices, records, true);
    context.registerPropertyArray(
        properties.data(), vertices, 4, 256, 0.50, true);
    Requirements requirements;
    requirements.vertex_count = vertices;
    requirements.record_count = records;
    Layout layout;
    if (selectLayout(requirements, layout) != Status::OK)
        return 1;
    const uint64_t base = reinterpret_cast<uint64_t>(properties.data());
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    packProperty(
        {PropertyKind::U32, 4, TraversalMode::ORDERED_FILTERED},
        configuration.property_descriptor);
    configuration.record_base = base + 4096;
    configuration.property_base = base;
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable | kNativeManagedPasses;

    // Four lines outside the governed property array, on which the two scans
    // disagree: way 3 is the least recent, way 1 is GRASP's victim.
    const uint8_t rrpv[4] = {2, 5, 5, 1};
    std::vector<CacheLine> shape(4);
    for (std::size_t index = 0; index < shape.size(); ++index) {
        shape[index].valid = true;
        shape[index].line_addr = base + 2048 + index * 64;
        shape[index].rrpv = rrpv[index];
        shape[index].last_access = 4 - index;
    }
    CacheLevel grasp("L3", 256, 64, 4, EvictionPolicy::GRASP);
    grasp.initGraphContext(&context);
    auto reference = shape;
    const std::size_t grasp_victim = grasp.selectVictimForTest(reference);
    if (grasp_victim != 1)
        return 2;
    const auto same_rrpv = [&reference](const std::vector<CacheLine>& set) {
        for (std::size_t index = 0; index < set.size(); ++index)
            if (set[index].rrpv != reference[index].rrpv)
                return false;
        return true;
    };

    CacheLevel pagerank("L3", 256, 64, 4, EvictionPolicy::ECG);
    pagerank.initGraphContext(&context);
    pagerank.configureRecord(configuration, true);
    pagerank.advanceRecordProgress(1);
    auto today = shape;
    // The dependency this arm removes: the base victim is the LRU scan's.
    if (pagerank.selectVictimForTest(today) != 3 ||
        pagerank.getRecordVictimAttribution().base_lru != 1)
        return 3;
    pagerank.setRecordRrpvOrder(true);
    auto ordered = shape;
    if (pagerank.selectVictimForTest(ordered) != grasp_victim || !same_rrpv(ordered))
        return 4;
    const auto& lru_arm = pagerank.getRecordVictimAttribution();
    if (lru_arm.decisions != 2 || lru_arm.base_lru != 1 || lru_arm.rrpv_ordered != 1)
        return 5;

    CacheLevel spmv("L3", 256, 64, 4, EvictionPolicy::GRASP);
    spmv.initGraphContext(&context);
    spmv.prepareRecord(EvictionPolicy::GRASP);
    spmv.configureRecord(configuration, true, EvictionPolicy::GRASP);
    spmv.advanceRecordProgress(1);
    spmv.setRecordGovernedFirst(true);
    auto recency = shape;
    if (spmv.selectVictimForTest(recency) != 3)
        return 6;
    spmv.setRecordRrpvOrder(true);
    auto governed_first = shape;
    if (spmv.selectVictimForTest(governed_first) != grasp_victim || !same_rrpv(governed_first))
        return 7;
    const auto& grasp_arm = spmv.getRecordVictimAttribution();
    if (grasp_arm.ungoverned_first != 2 || grasp_arm.rrpv_ordered != 1 ||
        grasp_arm.rrpv_changed != 1 || grasp_arm.base_lru != 0)
        return 8;
    return 0;
}

// The kernel census on a hand-traced stream. A 1-way L1 and a 1-way L2 set
// send every access that is not an immediate repeat to the single 4-way L3
// set, so under LRU each count below follows by hand. C and D are lines of two
// property arrays. Setup writes A, B and C and reads E, so the kernel enters
// holding one property line. Pass one rewrites A and evicts B, so the first
// pass carries setup's one writeback; pass two re-reads C and reads D,
// evicting clean E, so the kernel leaves holding two property lines and C
// still dirty from setup. Each pass is also detailed as the census would
// report it had the kernel ended there: pass one ends holding A, which it
// rewrote, and C, both dirty, and pass two adds the property line D. Returns
// zero, or the number of the failed check.
int exerciseKernelCensus() {
    using namespace cache_sim;
    constexpr uint64_t A = 0x0000, B = 0x1000, C = 0x2000, D = 0x3000, E = 0x4000, F = 0x5000;
    std::vector<uint32_t> degrees(16, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), 16, 8, true);
    for (const uint64_t array : {C, D})
        context.registerPropertyArray(reinterpret_cast<float*>(array), 16, 4, 256, 0.50, true);
    CacheHierarchy cache(64, 1, 128, 1, 256, 4, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU, EvictionPolicy::LRU);
    cache.initGraphContext(&context);
    // Before the kernel boundary, pass marks are ignored and nothing is emitted.
    cache.markKernelPass(false);
    cache.markKernelPass(true);
    for (const uint64_t line : {A, B, C})
        cache.access(line, true);
    cache.access(E);
    if (cache.toJSON().find("kernel_census") != std::string::npos)
        return 1;
    cache.markKernelEntry();
    cache.access(E);
    cache.markKernelPass(true);
    cache.access(A, true);
    cache.access(F);
    cache.markKernelPass(false);
    cache.access(F);
    cache.markKernelPass(true);
    cache.access(C);
    cache.access(D);
    cache.markKernelPass(false);
    cache.access(D);
    const std::string json = cache.toJSON();
    const std::string census = kernelCensusLine(json);
    if (census.empty())
        return 2;
    const std::string idle = "{\"total_accesses\":1,\"memory_accesses\":0,\"prefetch_fills\":0,"
        "\"llc_writebacks\":0,\"llc_hits\":0,\"llc_misses\":0,\"llc_property_hits\":0,"
        "\"llc_property_misses\":0,\"total_offchip_traffic\":0,\"entry_dirty_writebacks\":0}";
    const std::string first = "\"total_accesses\":2,\"memory_accesses\":1,\"prefetch_fills\":0,"
        "\"llc_writebacks\":1,\"llc_hits\":1,\"llc_misses\":1,\"llc_property_hits\":0,"
        "\"llc_property_misses\":0,\"total_offchip_traffic\":2,\"entry_dirty_writebacks\":1";
    const std::string later = "\"total_accesses\":2,\"memory_accesses\":1,\"prefetch_fills\":0,"
        "\"llc_writebacks\":0,\"llc_hits\":1,\"llc_misses\":1,\"llc_property_hits\":1,"
        "\"llc_property_misses\":1,\"total_offchip_traffic\":1,\"entry_dirty_writebacks\":0";
    for (const std::string& field : std::vector<std::string>{
            "\"entries\":1,", "\"passes\":2,", "\"entry_valid_lines\":4,",
            "\"entry_dirty_lines\":3,", "\"entry_property_lines\":1,",
            "\"entry_property_dirty_lines\":1,", "\"entry_dirty_writebacks\":1,",
            "\"entry_dirty_rewritten\":1,", "\"exit_valid_lines\":4,",
            "\"exit_dirty_lines\":2,", "\"exit_property_lines\":2,",
            "\"exit_entry_dirty_lines\":1,",
            "\"before_first_pass\":" + idle,
            "\"first_pass\":{" + first + "}",
            "\"between_passes\":" + idle,
            "\"later_passes\":{" + later + "}",
            "\"after_last_pass\":" + idle,
            "\"pass_detail_limit\":64,\"pass_detail\":[{" + first + ",\"end_valid_lines\":4,"
                "\"end_dirty_lines\":2,\"end_property_lines\":1,\"end_entry_dirty_lines\":1},{" +
                later + ",\"end_valid_lines\":4,\"end_dirty_lines\":2,\"end_property_lines\":2,"
                "\"end_entry_dirty_lines\":1}]"})
        if (census.find(field) == std::string::npos) {
            std::printf("kernel census lacks %s in %s\n", field.c_str(), census.c_str());
            return 3;
        }
    if (cache.toJSON() != json)
        return 4;
    // A kernel may release its graph context before the receipt is written,
    // so the exit residue is classified by the regions the census armed with.
    cache.initGraphContext(nullptr);
    if (kernelCensusLine(cache.toJSON()) != census)
        return 5;
    return 0;
}

// A reproducible stream over 48 lines, the first 24 a governed property array
// and a third of the accesses writes, that enters the kernel at access 3000
// and runs three passes with gaps around them, marking the boundaries as a
// kernel does. PLAIN leaves the census unarmed, ARMED arms it and CLEANED also
// cleans every last-level line at the boundary. Every run resets its
// statistics at the boundary, so its L3 counters cover the kernel alone. A
// replay cut at censusCutAfterPass(k) ends with the mark that closes pass k.
enum class CensusRun { PLAIN, ARMED, CLEANED };

constexpr std::size_t censusCutAfterPass(std::size_t pass) { return 3000 + pass * 1000 - 50; }

void replayCensusStream(cache_sim::CacheHierarchy& cache, CensusRun run, std::size_t end = 6000) {
    using Entry = cache_sim::CacheHierarchy::KernelEntryForTest;
    constexpr uint64_t base = 0x10000;
    constexpr std::size_t entry = 3000;
    cache.setKernelEntryForTest(run == CensusRun::PLAIN ? Entry::UNARMED :
                                run == CensusRun::CLEANED ? Entry::CLEAN : Entry::AS_BUILT);
    std::mt19937_64 random(20260923);
    for (std::size_t step = 0; step < end; ++step) {
        if (step == entry) {
            cache.resetStats();
            cache.markKernelEntry();
        }
        if (step >= entry && (step - entry) % 1000 == 50)
            cache.markKernelPass(true);
        const uint64_t line = random() % 48;
        cache.access(base + line * 64, random() % 3 == 0);
        if (step >= entry && (step - entry) % 1000 == 949)
            cache.markKernelPass(false);
    }
}

// The census must observe without steering. For every policy the armed
// receipt, less its census line, is byte-identical to the unarmed one, and the
// unarmed receipt carries no census. Returns zero, or the failed check.
int exerciseKernelCensusIsPassive() {
    using namespace cache_sim;
    std::vector<uint32_t> degrees(384, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), 384, 384, true);
    context.registerPropertyArray(reinterpret_cast<float*>(0x10000), 384, 4, 1024, 0.50, true);
    for (const auto policy : {EvictionPolicy::LRU, EvictionPolicy::FIFO, EvictionPolicy::RANDOM,
             EvictionPolicy::SRRIP, EvictionPolicy::GRASP}) {
        CacheHierarchy plain(128, 2, 256, 2, 1024, 4, 64,
            EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
        CacheHierarchy armed(128, 2, 256, 2, 1024, 4, 64,
            EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
        plain.initGraphContext(&context);
        armed.initGraphContext(&context);
        replayCensusStream(plain, CensusRun::PLAIN);
        replayCensusStream(armed, CensusRun::ARMED);
        const std::string reference = plain.toJSON(), observed = armed.toJSON();
        if (reference.find("kernel_census") != std::string::npos)
            return 1;
        if (withoutKernelCensus(observed) != reference)
            return 2;
        // A stream that never evicts an entry-dirty line could not fail below.
        const std::string census = kernelCensusLine(observed);
        if (receiptValue(census, "entries") != 1 || receiptValue(census, "passes") != 3 ||
            receiptValue(census, "entry_property_lines") == 0 ||
            receiptValue(census, "entry_dirty_writebacks") == 0)
            return 3;
    }
    return 0;
}

// No replacement decision reads the dirty bit, so cleaning the last level at
// the kernel boundary must leave every hit, miss and eviction unchanged and
// move exactly what the census attributes to setup: the writebacks of lines
// dirtied before the kernel, each from the segment the census charged it to,
// and the setup-dirty lines still resident at the end. Returns zero, or the
// number of the failed check.
int exerciseCleanEntryCounterfactual() {
    using namespace cache_sim;
    std::vector<uint32_t> degrees(384, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), 384, 384, true);
    context.registerPropertyArray(reinterpret_cast<float*>(0x10000), 384, 4, 1024, 0.50, true);
    // A stream whose setup writebacks all fell before the first pass could not
    // tell a pass from the boundary; some policy must carry one into a pass.
    bool setup_writeback_in_pass = false;
    for (const auto policy : {EvictionPolicy::LRU, EvictionPolicy::FIFO, EvictionPolicy::RANDOM,
             EvictionPolicy::SRRIP, EvictionPolicy::GRASP}) {
        CacheHierarchy census(128, 2, 256, 2, 1024, 4, 64,
            EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
        CacheHierarchy cleaned(128, 2, 256, 2, 1024, 4, 64,
            EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
        census.initGraphContext(&context);
        cleaned.initGraphContext(&context);
        replayCensusStream(census, CensusRun::ARMED);
        replayCensusStream(cleaned, CensusRun::CLEANED);
        const auto& observed = census.getL3Stats();
        const auto& clean = cleaned.getL3Stats();
        if (observed.hits.load() != clean.hits.load() ||
            observed.misses.load() != clean.misses.load() ||
            observed.evictions.load() != clean.evictions.load() ||
            census.getMemoryAccesses() != cleaned.getMemoryAccesses())
            return 1;
        const std::string line = kernelCensusLine(census.toJSON());
        const std::string clean_line = kernelCensusLine(cleaned.toJSON());
        if (receiptValue(clean_line, "entry_dirty_lines") != 0 ||
            receiptValue(clean_line, "exit_entry_dirty_lines") != 0)
            return 2;
        const uint64_t setup_writebacks = receiptValue(line, "entry_dirty_writebacks");
        if (setup_writebacks == 0 || setup_writebacks == UINT64_MAX)
            return 3;
        if (clean.writebacks.load() != observed.writebacks.load() - setup_writebacks ||
            receiptValue(clean_line, "exit_dirty_lines") !=
                receiptValue(line, "exit_dirty_lines") - receiptValue(line, "exit_entry_dirty_lines"))
            return 4;
        // Cleaning moves no line, so both runs leave the same lines resident,
        // property lines among them.
        const uint64_t resident_property = receiptValue(line, "exit_property_lines");
        if (resident_property == 0 || resident_property == UINT64_MAX ||
            receiptValue(clean_line, "exit_property_lines") != resident_property ||
            receiptValue(clean_line, "exit_valid_lines") != receiptValue(line, "exit_valid_lines"))
            return 8;
        uint64_t charged = 0;
        for (const char* segment : {"before_first_pass", "first_pass", "between_passes",
                 "later_passes", "after_last_pass"}) {
            const uint64_t setup = segmentValue(line, segment, "entry_dirty_writebacks");
            if (setup == UINT64_MAX ||
                segmentValue(clean_line, segment, "llc_writebacks") !=
                    segmentValue(line, segment, "llc_writebacks") - setup)
                return 5;
            charged += setup;
            const std::string name = segment;
            setup_writeback_in_pass |= setup > 0 && (name == "first_pass" || name == "later_passes");
        }
        if (charged != setup_writebacks)
            return 6;
    }
    return setup_writeback_in_pass ? 0 : 7;
}

// Each detailed pass is what the census would have reported had the kernel
// ended with that pass. For every policy, a replay cut right after pass k
// details the full replay's first k passes, and its exit is the full replay's
// end of pass k; the first detailed pass is the first-pass segment, and the
// others sum to the later passes. Returns zero, or the failed check.
int exerciseKernelCensusPassDetail() {
    using namespace cache_sim;
    std::vector<uint32_t> degrees(384, 1);
    GraphCacheContext context;
    context.initTopology(degrees.data(), 384, 384, true);
    context.registerPropertyArray(reinterpret_cast<float*>(0x10000), 384, 4, 1024, 0.50, true);
    // Neither comparison could fail if no pass wrote setup dirt back or if
    // every pass ended in the same state; some policy must do each.
    bool setup_writeback_in_pass = false, ends_differ = false;
    for (const auto policy : {EvictionPolicy::LRU, EvictionPolicy::FIFO, EvictionPolicy::RANDOM,
             EvictionPolicy::SRRIP, EvictionPolicy::GRASP}) {
        CacheHierarchy full(128, 2, 256, 2, 1024, 4, 64,
            EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
        full.initGraphContext(&context);
        replayCensusStream(full, CensusRun::ARMED);
        const std::string census = kernelCensusLine(full.toJSON());
        const std::vector<std::string> detail = censusPassDetail(census);
        if (receiptValue(census, "pass_detail_limit") != 64 || detail.size() != 3)
            return 1;
        for (const char* key : {"total_accesses", "memory_accesses", "prefetch_fills",
                 "llc_writebacks", "llc_hits", "llc_misses", "llc_property_hits",
                 "llc_property_misses", "total_offchip_traffic", "entry_dirty_writebacks"})
            if (receiptValue(detail[0], key) != segmentValue(census, "first_pass", key) ||
                receiptValue(detail[1], key) + receiptValue(detail[2], key) !=
                    segmentValue(census, "later_passes", key))
                return 2;
        for (std::size_t passes = 1; passes <= detail.size(); ++passes) {
            CacheHierarchy cut(128, 2, 256, 2, 1024, 4, 64,
                EvictionPolicy::LRU, EvictionPolicy::LRU, policy);
            cut.initGraphContext(&context);
            replayCensusStream(cut, CensusRun::ARMED, censusCutAfterPass(passes));
            const std::string cut_census = kernelCensusLine(cut.toJSON());
            const std::vector<std::string> cut_detail = censusPassDetail(cut_census);
            if (receiptValue(cut_census, "passes") != passes || cut_detail.size() != passes)
                return 3;
            for (std::size_t pass = 0; pass < passes; ++pass)
                if (cut_detail[pass] != detail[pass])
                    return 4;
            for (const std::string line : {"valid_lines", "dirty_lines", "property_lines",
                     "entry_dirty_lines"})
                if (receiptValue(detail[passes - 1], "end_" + line) !=
                        receiptValue(cut_census, "exit_" + line))
                    return 5;
        }
        for (std::size_t pass = 0; pass < detail.size(); ++pass) {
            setup_writeback_in_pass |= receiptValue(detail[pass], "entry_dirty_writebacks") > 0;
            ends_differ |= pass > 0 &&
                detail[pass].substr(detail[pass].find("\"end_")) !=
                    detail[pass - 1].substr(detail[pass - 1].find("\"end_"));
        }
    }
    return !setup_writeback_in_pass ? 6 : !ends_differ ? 7 : 0;
}

// The detail stops at the census's limit, so a kernel that opens thousands of
// passes pays at most 64 end-of-pass scans; the passes beyond it are counted
// and charged to the later passes, but not detailed. Returns zero, or the
// failed check.
int exerciseKernelCensusPassDetailIsBounded() {
    using namespace cache_sim;
    CacheHierarchy cache(64, 1, 128, 1, 256, 4, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU, EvictionPolicy::LRU);
    cache.markKernelEntry();
    for (uint64_t pass = 0; pass < 70; ++pass) {
        cache.markKernelPass(true);
        cache.access(pass % 6 * 0x1000, pass % 2 == 0);
        cache.markKernelPass(false);
    }
    const std::string census = kernelCensusLine(cache.toJSON());
    const std::vector<std::string> detail = censusPassDetail(census);
    if (receiptValue(census, "passes") != 70 || detail.size() != 64)
        return 1;
    uint64_t detailed = 0;
    for (std::size_t pass = 1; pass < detail.size(); ++pass)
        detailed += receiptValue(detail[pass], "total_accesses");
    if (detailed != 63 || segmentValue(census, "later_passes", "total_accesses") != 69)
        return 2;
    return 0;
}

// The census fails closed on anything that would misplace a boundary: a pass
// closed before it opens or opened twice, a receipt or a kernel boundary
// inside a pass, and a statistics reset after the boundary. A new boundary
// re-arms it and counts the entry. Returns zero, or the failed check.
int exerciseKernelCensusFailsClosed() {
    using namespace cache_sim;
    const auto refuses = [](const auto& action) {
        try {
            action();
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    CacheHierarchy cache(64, 1, 128, 1, 256, 4, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU, EvictionPolicy::LRU);
    cache.access(0x0000, true);
    cache.markKernelEntry();
    if (!refuses([&] { cache.markKernelPass(false); }))
        return 1;
    cache.markKernelPass(true);
    if (!refuses([&] { cache.markKernelPass(true); }) || !refuses([&] { cache.toJSON(); }) ||
        !refuses([&] { cache.markKernelEntry(); }))
        return 2;
    cache.markKernelPass(false);
    const std::string one_pass = kernelCensusLine(cache.toJSON());
    if (receiptValue(one_pass, "passes") != 1 || censusPassDetail(one_pass).size() != 1)
        return 3;
    cache.resetStats();
    if (!refuses([&] { cache.toJSON(); }))
        return 4;
    cache.markKernelEntry();
    const std::string census = kernelCensusLine(cache.toJSON());
    if (receiptValue(census, "entries") != 2 || receiptValue(census, "passes") != 0 ||
        census.find("\"pass_detail\":[]") == std::string::npos)
        return 5;
    return 0;
}

// A pull-PageRank graph on 64 vertices, whose F32 property array fills four
// 64-byte lines. Node u gathers source[offset[u]] up to source[offset[u + 1]],
// in the kernel's order. Vertices 16..31 fill one line: nodes 0..3 gather its
// first half, nodes 12..15 its second, and nodes 16..31 then write it in place
// after its last gather. Nodes 32..63 gather two sources each, which keeps the
// L3 under pressure.
struct PullGraph {
    std::vector<uint64_t> offset, source;
    std::vector<uint32_t> out_degree;
};

PullGraph lastPassGraph() {
    constexpr uint64_t vertices = 64;
    PullGraph graph;
    graph.offset.push_back(0);
    for (uint64_t node = 0; node < vertices; ++node) {
        if (node < 4) {
            graph.source.push_back(16 + node);
        } else if (node >= 12 && node < 16) {
            graph.source.push_back(12 + node);
        } else if (node >= 32) {
            graph.source.push_back(node % 16);
            graph.source.push_back(32 + node * 7 % 32);
        }
        graph.offset.push_back(graph.source.size());
    }
    // A vertex without out-edges still divides by its out-degree; it counts one.
    graph.out_degree.assign(vertices, 0);
    for (const uint64_t source : graph.source)
        ++graph.out_degree[source];
    for (uint32_t& degree : graph.out_degree)
        degree = std::max<uint32_t>(degree, 1);
    return graph;
}

// How a replay decodes its last pass: FINAL as the kernel does, so each line's
// last gather retires it DEAD, or ONGOING as if another pass followed.
enum class LastPass { FINAL, ONGOING };

enum class PassAccess { OTHER, GATHER, WRITE };

// One access of the last pass: its kind, the L3 misses it took, and whether a
// gather had already retired its line DEAD earlier in that pass.
struct PassStep {
    PassAccess kind;
    uint64_t misses;
    bool retired;
};

struct PullPageRankRun {
    std::vector<PassStep> last_pass;
    std::string census;
    uint64_t dead_first = UINT64_MAX;
    bool decoded = true;
};

// pr.cc's kernel, access for access, under the replacement mechanism on a
// single 16-way L3 set: setup writes both property arrays, then three passes
// each gather contribution[v] through the records and write scores[u] and
// contribution[u] in place. Each record is also decoded beside the cache, to
// know when a gather retires its line.
PullPageRankRun replayPullPageRank(const PullGraph& graph, const ecg_record::RecordStream& stream,
                                   LastPass decode, bool governed_first) {
    using namespace cache_sim;
    constexpr uint64_t vertices = 64, ways = 16, passes = 3;
    alignas(64) static float scores[vertices], contribution[vertices];
    alignas(64) static uint64_t in_index[vertices + 1], out_index[vertices + 1];
    const ecg_record::Layout& layout = stream.layout;
    const uint64_t records = graph.source.size();
    GraphCacheContext context;
    context.initTopology(graph.out_degree.data(), vertices, records, true);
    context.registerPropertyArray(scores, vertices, 4, ways * 64, 0.15, true);
    context.registerPropertyArray(contribution, vertices, 4, ways * 64, 0.15, true);
    CacheHierarchy cache(64, 1, 64, 1, ways * 64, ways, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU, EvictionPolicy::ECG);
    cache.initGraphContext(&context);
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    configuration.record_base = reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base = reinterpret_cast<uint64_t>(contribution);
    configuration.record_count = records;
    configuration.vertex_count = vertices;
    configuration.context = configuration.generation = 1;
    configuration.control = ecg_record::kNativeEnable;
    cache.configureRecord(configuration, stream, ecg_record::Mechanism::REPLACEMENT);
    cache.setRecordGovernedFirst(governed_first);
    for (uint64_t node = 0; node < vertices; ++node) {
        cache.readArray(scores, node);
        cache.writeArray(scores, node);
        cache.readArray(contribution, node);
        cache.writeArray(contribution, node);
    }
    cache.resetStats();
    cache.markKernelEntry();
    PullPageRankRun run;
    const auto line = [](const void* address) { return reinterpret_cast<uint64_t>(address) / 64; };
    for (uint64_t pass = 0; pass < passes; ++pass) {
        const bool last = pass + 1 == passes;
        const bool has_next = !last || decode == LastPass::ONGOING;
        std::set<uint64_t> retired;
        const auto step = [&](PassAccess kind, const void* address, const auto& access) {
            const bool was_retired = retired.count(line(address)) != 0;
            const uint64_t misses = cache.getL3Stats().misses.load();
            const uint64_t result = access();
            if (last)
                run.last_pass.push_back(
                    {kind, cache.getL3Stats().misses.load() - misses, was_retired});
            return result;
        };
        const auto read = [&](const void* address) {
            step(PassAccess::OTHER, address, [&] {
                cache.access(reinterpret_cast<uint64_t>(address));
                return uint64_t{0};
            });
        };
        cache.markKernelPass(true);
        cache.recordIteration(pass * records, has_next);
        for (uint64_t node = 0; node < vertices; ++node) {
            read(&in_index[node]);
            read(&in_index[node + 1]);
            for (uint64_t edge = graph.offset[node]; edge < graph.offset[node + 1]; ++edge) {
                const auto* record = reinterpret_cast<const void*>(
                    configuration.record_base + edge * layout.record_bytes);
                const uint64_t word = step(PassAccess::OTHER, record,
                    [&] { return cache.recordLoad(edge); });
                ecg_record::Prediction prediction;
                if (ecg_record::makePrediction(layout, word, pass * records + edge + 1, has_next,
                        prediction) != ecg_record::Status::OK ||
                    prediction.destination != graph.source[edge]) {
                    run.decoded = false;
                    return run;
                }
                const float* property = &contribution[prediction.destination];
                if (step(PassAccess::GATHER, property,
                        [&] { return cache.recordProperty(edge, word); }) != graph.source[edge]) {
                    run.decoded = false;
                    return run;
                }
                if (prediction.state == ecg_record::State::DEAD)
                    retired.insert(line(property));
            }
            step(PassAccess::OTHER, &scores[node], [&] {
                cache.writeArray(scores, node);
                return uint64_t{0};
            });
            read(&out_index[node]);
            read(&out_index[node + 1]);
            step(PassAccess::WRITE, &contribution[node], [&] {
                cache.writeArray(contribution, node);
                return uint64_t{0};
            });
        }
        cache.markKernelPass(false);
    }
    cache.finishRecord(records * passes);
    const std::string json = cache.toJSON();
    run.census = kernelCensusLine(json);
    const std::string field = "\"ecg_record_victim_dead_first\": ";
    const std::size_t at = json.find(field);
    if (at != std::string::npos)
        run.dead_first = std::strtoull(json.c_str() + at + field.size(), nullptr, 10);
    return run;
}

// Why PageRank's last pass can miss where a pass followed by another does not.
// DEAD bounds the gathers the records carry, but the vertex loop also writes
// contribution[u] in place, and no record describes that write. So a final
// pass may retire a line DEAD, evict it first and have the write re-fetch it,
// with a bound that held for every gather. A builder whose lines split a real
// line would instead retire half of it while the other half is still to be
// gathered, so that a gather re-fetches it: a bound that did not hold.
//
// Built from 16 F32 values per line, as pr.cc builds them on its 2 MiB-aligned
// array, no gather reads a line after its DEAD decode, and every miss the
// final pass adds is an in-place write to a retired line. Built from 8 per
// line, the same kernel also adds a gather of a retired line. The census
// cannot always tell the two apart: while a re-fetched line is still to be
// written in the pass, either miss follows a dirty eviction and the write
// dirties the line again, so the last pass's misses and the lines it turns
// dirty grow together whichever the cause. Only the charge to each access
// can. Every record offset within a line is replayed, under the LRU base and
// under governed-first. Returns zero, or the number of the failed check.
int exerciseLastPassInPlaceWrite() {
    const PullGraph graph = lastPassGraph();
    ecg_record::Requirements requirements;
    requirements.vertex_count = 64;
    requirements.record_count = graph.source.size();
    requirements.traversal_count = 3;
    requirements.max_vertex_id = *std::max_element(graph.source.begin(), graph.source.end());
    requirements.max_vertex_id_known = true;
    std::list<ecg_record::RecordStream> copies;
    std::list<std::vector<char>> padding;
    // The two pr.cc builds, then the split build, which must fail the checks
    // that hold for them.
    struct Build {
        uint64_t vertices_per_line;
        bool governed_first;
    };
    bool census_alike = false;
    for (const Build build : {Build{16, false}, Build{16, true}, Build{8, true}}) {
        ecg_record::Layout layout;
        ecg_record::RecordStream stream;
        if (ecg_record::selectLayout(requirements, layout) != ecg_record::Status::OK ||
            ecg_record::buildRecords(requirements, layout, build.vertices_per_line,
                [&graph](std::size_t edge) { return graph.source[edge]; }, stream) !=
                ecg_record::Status::OK)
            return 1;
        // The records share the L3 with the property lines, so which records
        // share a line moves the evictions. Copies separated by padding that
        // stays allocated walk the stream through every 16-byte offset in a line.
        std::array<const ecg_record::RecordStream*, 4> at_offset{};
        for (int copy = 0; copy < 256 && std::count(at_offset.begin(), at_offset.end(), nullptr);
             ++copy) {
            padding.emplace_back(16 * (copy % 7) + 1);
            copies.push_back(stream);
            const uint64_t offset = reinterpret_cast<uint64_t>(copies.back().data()) % 64;
            if (offset % 16 == 0 && !at_offset[offset / 16])
                at_offset[offset / 16] = &copies.back();
        }
        if (std::count(at_offset.begin(), at_offset.end(), nullptr))
            return 2;
        for (const ecg_record::RecordStream* records : at_offset) {
            const PullPageRankRun final_pass =
                replayPullPageRank(graph, *records, LastPass::FINAL, build.governed_first);
            const PullPageRankRun ongoing =
                replayPullPageRank(graph, *records, LastPass::ONGOING, build.governed_first);
            if (!final_pass.decoded || !ongoing.decoded)
                return 3;
            const std::vector<std::string> detail = censusPassDetail(final_pass.census);
            const std::vector<std::string> reference = censusPassDetail(ongoing.census);
            if (detail.size() != 3 || reference.size() != 3 ||
                final_pass.last_pass.size() != ongoing.last_pass.size())
                return 4;
            // The replays part only where the last pass decodes.
            if (detail[0] != reference[0] || detail[1] != reference[1])
                return 5;
            uint64_t regathers = 0, write_refetches = 0, gather_refetches = 0;
            uint64_t other_added = 0, removed = 0;
            int64_t added = 0;
            for (std::size_t index = 0; index < final_pass.last_pass.size(); ++index) {
                const PassStep& step = final_pass.last_pass[index];
                const PassStep& same = ongoing.last_pass[index];
                if (step.kind != same.kind)
                    return 4;
                regathers += step.kind == PassAccess::GATHER && step.retired;
                const int64_t more = int64_t(step.misses) - int64_t(same.misses);
                added += more;
                if (more < 0)
                    removed += uint64_t(-more);
                else if (step.kind == PassAccess::WRITE && step.retired)
                    write_refetches += uint64_t(more);
                else if (step.kind == PassAccess::GATHER && step.retired)
                    gather_refetches += uint64_t(more);
                else
                    other_added += uint64_t(more);
            }
            const auto more = [&](const char* key) {
                return int64_t(receiptValue(detail[2], key)) - int64_t(receiptValue(reference[2], key));
            };
            // Both replays enter the last pass with the same dirty lines, so the
            // lines it turns dirty differ by its writebacks and its end dirt.
            const int64_t more_misses = more("llc_misses");
            const int64_t more_dirtied = more("llc_writebacks") + more("end_dirty_lines");
            if (more_misses != added)
                return 6;
            if (build.vertices_per_line == 16) {
                if (regathers != 0)
                    return 7;
                if (write_refetches == 0 || gather_refetches + other_added + removed != 0)
                    return 8;
                if (more_dirtied != more_misses)
                    return 9;
                if (final_pass.dead_first == 0 || final_pass.dead_first == UINT64_MAX ||
                    ongoing.dead_first != 0)
                    return 10;
            } else {
                if (regathers == 0)
                    return 11;
                if (gather_refetches == 0)
                    return 12;
                census_alike |= more_dirtied == more_misses;
            }
        }
    }
    // Some offset must show the split build's gather re-fetch with the census
    // signature of an in-place write, or the census would have told them apart.
    return census_alike ? 0 : 13;
}

int main() {
    using namespace cache_sim;
    ecg_record::Requirements requirements;
    requirements.vertex_count = 64;
    requirements.record_count = 8;
    ecg_record::Layout layout;
    if (ecg_record::selectLayout(requirements, layout) != ecg_record::Status::OK)
        return 1;
    ecg_record::NativeConfiguration configuration;
    ecg_record::packLayout(layout, configuration.layout_descriptor);
    configuration.record_base = 0x2000;
    configuration.property_base = 0x1000;
    configuration.record_count = 8;
    configuration.vertex_count = 64;
    configuration.context = 1;
    configuration.generation = 7;
    configuration.control = ecg_record::kNativeEnable;
    GraphCacheContext context;
    std::vector<uint32_t> degrees(64, 1);
    context.initTopology(degrees.data(), 64, 8, true);
    context.registerPropertyArray(
        reinterpret_cast<float*>(0x1000), 64, 4, 128, 0.15, true);
    CacheLevel cache("L3", 128, 64, 2, EvictionPolicy::ECG);
    cache.initGraphContext(&context);
    cache.configureRecord(configuration, true);
    cache.insert(0x1000, true);
    cache.insert(0x1040, false);
    CacheLine before, after;
    if (!cache.lineSnapshotForTest(0x1000, before))
        return 2;
    ecg_record::CommitUpdate update;
    update.physical_line = update.property_vaddr = 0x1000;
    update.context = 1;
    update.generation = 7;
    update.sequence = 1;
    update.deadline = 5;
    update.state = ecg_record::State::FINITE;
    if (cache.applyRecordUpdate(update) != ecg_record::ApplyResult::APPLIED ||
        !cache.lineSnapshotForTest(0x1000, after) ||
        after.last_access != before.last_access || after.access_count != before.access_count ||
        after.rrpv != before.rrpv || after.dirty != before.dirty ||
        after.record_metadata.state != ecg_record::LineState::FINITE ||
        after.record_metadata.value != 5)
        return 3;
    ecg_record::NativeLoadResult observation;
    observation.property_address = 0x1000;
    observation.context = 1;
    observation.generation = 7;
    observation.sequence = 3;
    observation.state = ecg_record::State::FINITE;
    cache.access(0x1000, false, &observation);
    update.sequence = 2;
    if (cache.applyRecordUpdate(update) != ecg_record::ApplyResult::STALE ||
        !cache.lineSnapshotForTest(0x1000, after) ||
        after.record_metadata.state != ecg_record::LineState::PENDING ||
        after.record_metadata.value != 3)
        return 4;
    update.sequence = 3;
    update.physical_line = 0x1040;
    if (cache.applyRecordUpdate(update) != ecg_record::ApplyResult::INVALID_ADDRESS) {
        std::puts("a functional update must retain its actual line identity [FAIL]");
        return 5;
    }
    observation.property_address = 0x1080;
    observation.sequence = 4;
    observation.state = ecg_record::State::DEAD;
    cache.insert(0x1080, false, false, &observation);
    if (cache.contains(0x1080) || cache.recordDeadBypasses() != 1)
        return 6;
    observation.context = 0;
    try {
        cache.insert(0x1080, false, false, &observation);
        std::puts("invalid DEAD observations must fail before bypass [FAIL]");
        return 7;
    } catch (const std::logic_error&) {}
    for (const uint8_t bytes : {uint8_t{4}, uint8_t{8}}) {
        if (!exerciseGraspRecordBase(bytes, false) ||
            !exerciseGraspRecordBase(bytes, true)) {
            std::puts("GRASP record base parity failed [FAIL]");
            return 8;
        }
        for (const auto mechanism : {ecg_record::Mechanism::TRANSPORT,
                ecg_record::Mechanism::REPLACEMENT, ecg_record::Mechanism::PREFETCH,
                ecg_record::Mechanism::REPLACEMENT_PREFETCH}) {
            if (!exerciseHierarchy(bytes, mechanism)) {
                std::puts("functional hierarchy mechanism failed [FAIL]");
                return 9;
            }
            if (!exerciseFilteredHierarchy(bytes, mechanism)) {
                std::puts("filtered hierarchy lifecycle failed [FAIL]");
                return 10;
            }
        }
    }
    if (const int check = exerciseRecordPressureDuel()) {
        std::printf("record pressure duel check %d failed [FAIL]\n", check);
        return 11;
    }
    if (const int check = exerciseRrpvOrderWithoutLru()) {
        std::printf("record RRPV order check %d failed [FAIL]\n", check);
        return 12;
    }
    if (const int check = exerciseKernelCensus()) {
        std::printf("kernel census check %d failed [FAIL]\n", check);
        return 13;
    }
    if (const int check = exerciseKernelCensusIsPassive()) {
        std::printf("kernel census passivity check %d failed [FAIL]\n", check);
        return 14;
    }
    if (const int check = exerciseCleanEntryCounterfactual()) {
        std::printf("clean-entry counterfactual check %d failed [FAIL]\n", check);
        return 15;
    }
    if (const int check = exerciseKernelCensusFailsClosed()) {
        std::printf("kernel census fail-closed check %d failed [FAIL]\n", check);
        return 16;
    }
    if (const int check = exerciseKernelCensusPassDetail()) {
        std::printf("kernel census pass detail check %d failed [FAIL]\n", check);
        return 17;
    }
    if (const int check = exerciseKernelCensusPassDetailIsBounded()) {
        std::printf("kernel census bounded pass detail check %d failed [FAIL]\n", check);
        return 18;
    }
    if (const int check = exerciseLastPassInPlaceWrite()) {
        std::printf("last-pass in-place write check %d failed [FAIL]\n", check);
        return 19;
    }
    std::puts("[SUMMARY] failures=0");
    return 0;
}
