#include "mem/cache/prefetch/ecg_pfx.hh"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace gem5 {
namespace prefetch {

GraphEcgPfxPrefetcher::GraphEcgPfxPrefetcher(const Params &p)
    : Queued(p),
      recentFilterSize([&p]() -> int {
          const char* v = std::getenv("GEM5_ECG_PFX_RECENT_FILTER_SIZE");
          if (!v || !v[0]) return static_cast<int>(p.recent_filter_size);
          long parsed = std::atol(v);
          if (parsed <= 0) return static_cast<int>(p.recent_filter_size);
          if (parsed > (1L << 20)) parsed = (1L << 20);
          return static_cast<int>(parsed);
      }())
{
}

void
GraphEcgPfxPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    if (!sidebandLoaded) {
        sidebandProbeCount++;
        if (sidebandProbeCount == 1 || (sidebandProbeCount & 0x3ff) == 0) {
            tryLoadSideband();
        }
    }

    if (!sidebandLoaded || !propertyConfigured) {
        return;
    }

    // Batch-drain: Path A (epoch-filtered lookahead) pushes up to K hints per
    // demand edge, so drain several per call (DROPLET-style multi-address-per-
    // call) instead of one — otherwise the 256-entry ring overflows and drops
    // most Path A targets. Bounded by GEM5_ECG_PFX_DRAIN_BATCH. Path B (single
    // target) is unaffected: it only ever has one hint pending.
    static const int drainBatch = []() -> int {
        const char* v = std::getenv("GEM5_ECG_PFX_DRAIN_BATCH");
        int b = (v && v[0]) ? std::atoi(v) : 8;
        if (b < 1) b = 1;
        if (b > 64) b = 64;
        return b;
    }();

    for (int drained = 0; drained < drainBatch; ++drained) {
        uint32_t target = 0;
        if (!replacement_policy::graph::consumePrefetchTargetHint(target)) {
            break;  // ring empty
        }

        uint64_t address = propertyAddrForVertex(target);
        if (!isPropertyAddress(address) || !shouldPrefetch(address)) {
            continue;  // keep draining the rest of the batch
        }

        if (!reportedFirstHint) {
            std::cerr << "ECG_PFX: first target vertex=" << target
                      << " addr=0x" << std::hex << address
                      << " property=[0x" << propertyBase << ",0x" << propertyEnd
                      << ")" << std::dec << std::endl;
            reportedFirstHint = true;
        }
        addresses.push_back(AddrPriority(address, 0));
    }
}

void
GraphEcgPfxPrefetcher::tryLoadSideband()
{
    const char* env_path = std::getenv("GEM5_GRAPHBREW_CTX");
    const std::string path = env_path ? env_path : "/tmp/gem5_graphbrew_ctx.json";
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    std::string prop = findPreferredObject(content, "\"property_regions\"", "contrib");
    if (prop.empty()) {
        prop = findPreferredObject(content, "\"property_regions\"", "scores");
    }
    if (prop.empty()) {
        prop = findPreferredObject(content, "\"property_regions\"", "dist");
    }
    if (prop.empty()) {
        prop = findPreferredObject(content, "\"property_regions\"", "parent");
    }
    if (prop.empty()) return;

    uint64_t base = parseJsonUint(prop, "\"base\"");
    uint64_t size = parseJsonUint(prop, "\"size\"");
    uint32_t elem = static_cast<uint32_t>(parseJsonUint(prop, "\"elem_size\""));
    if (base == 0 || size == 0 || elem == 0) return;

    propertyBase = base;
    propertyEnd = base + size;
    propertyElemSize = elem;
    propertyConfigured = true;
    sidebandLoaded = true;

    if (!reportedSideband) {
        std::cerr << "ECG_PFX: loaded sideband property=[0x" << std::hex
                  << propertyBase << ",0x" << propertyEnd << ") elem="
                  << std::dec << propertyElemSize << std::endl;
        reportedSideband = true;
    }
}

uint64_t
GraphEcgPfxPrefetcher::parseJsonUint(const std::string& json,
                                     const std::string& key)
{
    size_t pos = json.find(key);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return std::strtoull(json.c_str() + pos, nullptr, 10);
}

std::string
GraphEcgPfxPrefetcher::findPreferredObject(const std::string& json,
                                           const std::string& section,
                                           const std::string& preferred)
{
    size_t pos = json.find(section);
    if (pos == std::string::npos) return "";
    size_t arr_start = json.find('[', pos);
    size_t arr_end = json.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) return "";

    std::string fallback;
    size_t obj_pos = arr_start;
    while ((obj_pos = json.find('{', obj_pos)) != std::string::npos && obj_pos < arr_end) {
        size_t obj_end = json.find('}', obj_pos);
        if (obj_end == std::string::npos || obj_end > arr_end) break;
        std::string obj = json.substr(obj_pos, obj_end - obj_pos + 1);
        if (fallback.empty()) fallback = obj;
        if (obj.find(preferred) != std::string::npos) return obj;
        obj_pos = obj_end + 1;
    }
    return fallback;
}

uint64_t
GraphEcgPfxPrefetcher::propertyAddrForVertex(uint32_t vertex) const
{
    return propertyBase + static_cast<uint64_t>(vertex) * propertyElemSize;
}

bool
GraphEcgPfxPrefetcher::isPropertyAddress(uint64_t address) const
{
    return propertyConfigured && address >= propertyBase && address < propertyEnd;
}

bool
GraphEcgPfxPrefetcher::shouldPrefetch(uint64_t address)
{
    uint64_t line = address & ~uint64_t(63);
    if (recentPrefetches.count(line)) return false;
    recentPrefetches.insert(line);
    recentOrder.push_back(line);
    while (recentOrder.size() > static_cast<size_t>(recentFilterSize)) {
        recentPrefetches.erase(recentOrder.front());
        recentOrder.pop_front();
    }
    return true;
}

} // namespace prefetch
} // namespace gem5