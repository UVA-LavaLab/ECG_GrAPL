#include <cstdint>
#include <cstdio>
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
    if (cache.toJSON().find("\"ecg_mode_effective\": \"RECORD\"") == std::string::npos)
        return false;
    return (mechanism != ecg_record::Mechanism::PREFETCH &&
            mechanism != ecg_record::Mechanism::REPLACEMENT_PREFETCH) ||
        cache.getPrefetchRequests() > 0;
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
        for (const auto mechanism : {ecg_record::Mechanism::TRANSPORT,
                ecg_record::Mechanism::REPLACEMENT, ecg_record::Mechanism::PREFETCH,
                ecg_record::Mechanism::REPLACEMENT_PREFETCH}) {
            if (!exerciseHierarchy(bytes, mechanism)) {
                std::puts("functional hierarchy mechanism failed [FAIL]");
                return 8;
            }
        }
    }
    std::puts("[SUMMARY] failures=0");
    return 0;
}
