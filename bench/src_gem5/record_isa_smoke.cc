#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "gem5_sim/gem5_harness.h"

int main(int argc, char** argv) {
    const bool wide = argc > 1 && std::strcmp(argv[1], "8") == 0;
    const bool invalid = argc > 1 && std::strcmp(argv[1], "bad-address") == 0;
    ecg_record::Requirements requirements;
    requirements.vertex_count = 32;
    requirements.record_count = 4;
    requirements.traversal_count = 2;
    requirements.requested_record_bytes = wide ? 8 : 4;
    ecg_record::Layout layout;
    if (ecg_record::selectLayout(requirements, layout) != ecg_record::Status::OK)
        return 2;
    alignas(64) uint32_t records32[4]{};
    alignas(64) uint64_t records64[4]{};
    alignas(64) float properties[32]{};
    const uint32_t destinations[] = {18, 17, 0, 31};
    const uint32_t bits[] = {0x3fa00000, 0x80000000, 0x7fc12345, 0xc0600000};
    const ecg_record::State states[] = {
        ecg_record::State::FINITE, ecg_record::State::DEAD,
        ecg_record::State::UNKNOWN, ecg_record::State::WRAP};
    const uint64_t distances[] = {1, 0, 0, 4};
    for (unsigned index = 0; index < 4; ++index) {
        if (ecg_record::encodeRecord(layout, destinations[index], distances[index],
                states[index], records64[index]) != ecg_record::Status::OK)
            return 3;
        records32[index] = static_cast<uint32_t>(records64[index]);
        std::memcpy(&properties[destinations[index]], &bits[index], sizeof(float));
    }
    if (wide && (records64[3] & (uint64_t{1} << 63)) == 0)
        return 4;
    ecg_record::NativeConfiguration configuration;
    configuration.record_base = wide ? reinterpret_cast<uint64_t>(records64)
                                     : reinterpret_cast<uint64_t>(records32);
    configuration.property_base = reinterpret_cast<uint64_t>(properties);
    configuration.record_count = 4;
    configuration.vertex_count = 32;
    configuration.context = 1;
    configuration.generation = 1;
    configuration.control = ecg_record::kNativeEnable;
    if (ecg_record::packLayout(layout, configuration.layout_descriptor) != ecg_record::Status::OK)
        return 5;
    Gem5RecordContext context(configuration);
    context.activate(0, true);
    if (invalid) {
        try {
            context.record(records32 + 4);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 6;
        }
        return 7;
    }
    bool ok = true;
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    for (uint64_t iteration = 0; iteration < 2; ++iteration) {
        if (iteration)
            context.activate(4, false);
        for (unsigned index = 0; index < 4; ++index) {
            const void* address = wide ? static_cast<const void*>(records64 + index)
                                       : static_cast<const void*>(records32 + index);
            const uint64_t raw = wide ? context.record(records64 + index)
                                      : context.record(records32 + index);
            ok = ok && raw == records64[index];
            const float value = context.property(properties, raw, address);
            uint32_t loaded_bits = 0;
            std::memcpy(&loaded_bits, &value, sizeof(float));
            ok = ok && loaded_bits == bits[index];
        }
    }
    context.finish();
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    context.deactivate();
    std::printf("[ECG-RECORD-ISA native=%u record_bytes=%u cases=8 high_bit=%u result=%s]\n",
                Gem5RecordContext::nativeAvailable() ? 1u : 0u,
                unsigned(layout.record_bytes), wide ? 1u : 0u, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
