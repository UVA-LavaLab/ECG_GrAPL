#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cache_sim/cache_sim.h"

namespace {

struct RegionValue {
    std::string name;
    uint64_t value = 0;
};

bool readExact(std::ifstream& input, void* data, std::size_t bytes) {
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
    return input.good();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(
            stderr,
            "usage: %s TRACE CACHE_MB LRU|GRASP\n",
            argv[0]);
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input.is_open()) return 2;
    uint64_t region_count = 0;
    if (!readExact(input, &region_count, sizeof(region_count)))
        return 2;
    std::vector<RegionValue> values;
    values.reserve(region_count);
    for (uint64_t index = 0; index < region_count; ++index) {
        char raw_name[25] = {};
        uint64_t value = 0;
        if (!readExact(input, raw_name, sizeof(raw_name)) ||
            !readExact(input, &value, sizeof(value))) {
            return 2;
        }
        raw_name[24] = '\0';
        values.push_back({raw_name, value});
    }
    auto lookup = [&](const std::string& name) {
        for (const auto& entry : values) {
            if (entry.name == name) return entry.value;
        }
        return uint64_t{0};
    };

    uint64_t address_count = 0;
    if (!readExact(input, &address_count, sizeof(address_count)))
        return 2;
    std::vector<uint64_t> addresses(address_count);
    if (!readExact(
            input, addresses.data(),
            addresses.size() * sizeof(uint64_t))) {
        return 2;
    }

    const uint64_t cache_bytes =
        std::strtoull(argv[2], nullptr, 10) * 1024ULL * 1024ULL;
    const std::string policy_name = argv[3];
    const cache_sim::EvictionPolicy policy =
        policy_name == "GRASP"
        ? cache_sim::EvictionPolicy::GRASP
        : cache_sim::EvictionPolicy::LRU;
    setenv("GRASP_BOUNDARY_MODE", "capacity", 1);
    if (policy == cache_sim::EvictionPolicy::GRASP)
        setenv("GRASP_OFFICIAL_TRACE_EMPTY_WAYS", "1", 1);

    cache_sim::GraphCacheContext context;
    for (const char* prefix : {"propertyA", "propertyB"}) {
        const uint64_t base = lookup(std::string(prefix) + "-0");
        const uint64_t upper = lookup(std::string(prefix) + "-n");
        const uint64_t percent = lookup(std::string(prefix) + "-f");
        if (base == 0 || upper <= base || percent == 0)
            continue;
        auto& region = context.regions[context.num_regions++];
        region.base_address = base;
        region.upper_bound = upper;
        region.grasp_hot_percent = static_cast<uint32_t>(percent);
        region.grasp_region = true;
        region.elem_size = 1;
    }

    cache_sim::CacheLevel cache(
        "L3", cache_bytes, 64, 16, policy);
    cache.initGraphContext(&context);
    for (uint64_t address : addresses) {
        if (!cache.access(address, false))
            cache.insert(address, false);
    }
    const uint64_t misses = cache.getStats().misses.load();
    std::printf(
        "[GRASP-TRACE-RESULT policy=%s accesses=%llu misses=%llu "
        "miss_rate=%.9f]\n",
        policy_name.c_str(),
        static_cast<unsigned long long>(address_count),
        static_cast<unsigned long long>(misses),
        address_count > 0
            ? static_cast<double>(misses) / address_count : 0.0);
    return 0;
}
