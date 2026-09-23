#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "cache_sim/cache_sim.h"

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
    std::puts("[SUMMARY] failures=0");
    return 0;
}
