#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

#include "gem5_sim/gem5_harness.h"

int main(int argc, char** argv) {
    constexpr uint32_t vertices = uint32_t{1} << 26;
    constexpr uint64_t bytes = static_cast<uint64_t>(vertices) * sizeof(float);
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        std::perror("mmap");
        return 2;
    }
    auto* properties = static_cast<float*>(memory);
    alignas(64) uint32_t records[] = {
        0x10000012, 0x8c000011, 0,
        (2u << 26) | (vertices - 1),
    };
    const uint32_t destinations[] = {18, 17, 0, vertices - 1};
    const uint32_t expected_bits[] = {
        0x3fa00000, 0x80000000, 0x7fc12345, 0xc0600000,
    };
    for (unsigned i = 0; i < 4; ++i)
        std::memcpy(&properties[destinations[i]], &expected_bits[i], sizeof(float));

    uint64_t config;
    if (!ecg_ref32::packNativeConfig(vertices, 4, config))
        return 3;
    Gem5Ref32Context context(records, config, 1);
    unsigned completed = 0;
    bool ok = true;
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    context.activate();
    if (argc > 1 && std::strcmp(argv[1], "bad-address") == 0) {
        try {
            context.record(records + 4, 0);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 4;
        }
        return 5;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const uint64_t canonical = context.record(
            records + i, ecg_ref32::kNativeHasNextIteration);
        const uint32_t normalized = i == 1 ? 0x10000011 : records[i];
        ok = ok && canonical == ((static_cast<uint64_t>(i + 1) << 32) | normalized);
        const float value = context.property(properties, canonical);
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        ok = ok && bits == expected_bits[i];
        ++completed;
    }
    const uint64_t final_record = context.record(records + 1, 4);
    ok = ok && final_record == ((uint64_t{6} << 32) | 0x04000011);
    const float final_value = context.property(properties, final_record);
    uint32_t bits;
    std::memcpy(&bits, &final_value, sizeof(bits));
    ok = ok && bits == 0x80000000;
    ++completed;

    const uint64_t zero_record = context.record(records + 2, UINT32_MAX - 2u);
    ok = ok && zero_record == 0;
    const float zero_value = context.property(properties, zero_record);
    std::memcpy(&bits, &zero_value, sizeof(bits));
    ok = ok && bits == 0x7fc12345;
    ++completed;
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    context.deactivate();
    std::printf("[ECG-REF32-ISA-SMOKE native=%u cases=%u result=%s]\n",
                Gem5Ref32Context::nativeAvailable() ? 1u : 0u,
                completed, ok ? "PASS" : "FAIL");
    if (munmap(memory, bytes) != 0) {
        std::perror("munmap");
        return 6;
    }
    return ok ? 0 : 1;
}
