// ============================================================================
// DROPLET Indirect Graph Prefetcher for gem5 — Implementation
// ============================================================================
// Reference: Basak et al. (2019).
// ============================================================================

#include "mem/cache/prefetch/droplet.hh"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace gem5 {
namespace prefetch {

GraphDropletPrefetcher::GraphDropletPrefetcher(const Params &p)
    : Queued(p),
      prefetchDegree(p.prefetch_degree),
      indirectDegree(p.indirect_degree),
      strideTableSize(p.stride_table_size)
{
    strideTable.resize(strideTableSize);
}

void
GraphDropletPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    Addr addr = pfi.getAddr();
    if ((!edgeConfigured || !propertyConfigured) && !sidebandLoaded) {
        sidebandProbeCount++;
        if (sidebandProbeCount == 1 || (sidebandProbeCount & 0x3ff) == 0) {
            tryLoadSideband();
        }
    }

    if (sidebandLoaded && !reportedFirstAccess) {
        std::cerr << "DROPLET: first access after sideband addr=0x" << std::hex << addr
                  << " edge=[0x" << edgeArrayBase << ",0x" << edgeArrayEnd
                  << ") property=[0x" << propertyBase << ",0x" << propertyEnd
                  << ") isEdge=" << isEdgeArrayAccess(addr)
                  << " isProperty=" << isPropertyAccess(addr)
                  << std::dec << std::endl;
        reportedFirstAccess = true;
    }

    // ── Engine 1: Edge-list stride prefetcher ──
    // Detect sequential CSR edge array traversal and prefetch ahead
    if (isEdgeArrayAccess(addr)) {
        if (!reportedFirstEdge) {
            std::cerr << "DROPLET: first edge access addr=0x" << std::hex << addr
                      << " edge=[0x" << edgeArrayBase << ",0x" << edgeArrayEnd
                      << ") property=[0x" << propertyBase << ",0x" << propertyEnd
                      << ")" << std::dec << std::endl;
            reportedFirstEdge = true;
        }
        uint64_t lineAddr = addr & ~uint64_t(63);
        std::vector<uint64_t> stridePredictions;
        updateStrideDetector(lineAddr, stridePredictions);

        for (const auto& predAddr : stridePredictions) {
            if (isEdgeArrayAccess(predAddr) && shouldPrefetch(predAddr)) {
                addresses.push_back(AddrPriority(predAddr, 0));
                if (edgeDataLoaded) {
                    issueIndirectPrefetches(predAddr, addresses);
                }
            }
        }

        // ── Engine 2: Indirect property prefetcher ──
        // From the current edge data, extract neighbor IDs and prefetch
        // their property data. This is the key DROPLET innovation:
        // the indirect chain edge_list[i] → neighbor_id → property[neighbor_id]
        issueIndirectPrefetches(addr, addresses);
    }

    // Property data misses: no action needed (covered by indirect prefetch)
    // Index array accesses: very short reuse, fits in L1 (no prefetch needed)
}

void
GraphDropletPrefetcher::tryLoadSideband()
{
    sidebandTried = true;

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
    if (!prop.empty()) {
        uint64_t base = parseJsonUint(prop, "\"base\"");
        uint64_t size = parseJsonUint(prop, "\"size\"");
        uint32_t elem = static_cast<uint32_t>(parseJsonUint(prop, "\"elem_size\""));
        if (base != 0 && size != 0 && elem != 0) {
            setPropertyRegion(base, size, elem);
        }
    }

    std::string edge = findPreferredObject(content, "\"edge_regions\"", "\"preferred\": true");
    if (!edge.empty()) {
        uint64_t base = parseJsonUint(edge, "\"base\"");
        uint64_t size = parseJsonUint(edge, "\"size\"");
        uint32_t elem = static_cast<uint32_t>(parseJsonUint(edge, "\"elem_size\""));
        if (base != 0 && size != 0) {
            setEdgeArrayRegion(base, size);
            if (elem != 0) edgeElemSize = elem;
            std::string dataPath = parseJsonString(edge, "\"data_path\"");
            if (!dataPath.empty()) {
                loadEdgeData(dataPath, edgeElemSize);
            }
        }
    }

    sidebandLoaded = propertyConfigured && edgeConfigured;
    if (sidebandLoaded && !reportedSideband) {
        std::cerr << "DROPLET: loaded sideband property=[0x" << std::hex
                  << propertyBase << ",0x" << propertyEnd << ") edge=[0x"
                  << edgeArrayBase << ",0x" << edgeArrayEnd << ") elem="
                  << std::dec << propertyElemSize
                  << " edge_data=" << (edgeDataLoaded ? edgeData.size() : 0)
                  << std::endl;
        reportedSideband = true;
    }
}

uint64_t
GraphDropletPrefetcher::parseJsonUint(const std::string& json,
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
GraphDropletPrefetcher::parseJsonString(const std::string& json,
                                        const std::string& key)
{
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

bool
GraphDropletPrefetcher::loadEdgeData(const std::string& path, uint32_t elemSize)
{
    if (elemSize != sizeof(uint32_t) && elemSize != 2 * sizeof(uint32_t)) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    std::streamoff bytes = file.tellg();
    file.seekg(0, std::ios::beg);
    if (bytes <= 0 || (bytes % static_cast<std::streamoff>(elemSize)) != 0) {
        return false;
    }

    std::vector<char> raw(static_cast<size_t>(bytes));
    file.read(raw.data(), bytes);
    bool readOk = file.good() || file.eof();
    if (!readOk) return false;

    size_t entries = raw.size() / elemSize;
    edgeData.resize(entries);
    for (size_t i = 0; i < entries; ++i) {
        std::memcpy(&edgeData[i], raw.data() + i * elemSize, sizeof(uint32_t));
    }
    edgeDataLoaded = true;
    return edgeDataLoaded;
}

std::string
GraphDropletPrefetcher::findPreferredObject(const std::string& json,
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

void
GraphDropletPrefetcher::updateStrideDetector(
    uint64_t addr, std::vector<uint64_t>& predictions)
{
    // Simple stride detector: track last N accesses to edge array,
    // detect stride pattern, predict next addresses

    // Find matching entry or create new one
    StrideEntry* bestEntry = nullptr;
    for (auto& entry : strideTable) {
        if (entry.valid && addr == entry.lastAddr) {
            return;
        }
        if (!entry.valid) {
            if (!bestEntry) bestEntry = &entry;
            continue;
        }
        int64_t delta = static_cast<int64_t>(addr) -
                        static_cast<int64_t>(entry.lastAddr);
        if (delta == entry.stride && delta != 0) {
            entry.confidence = std::min(uint8_t(3), uint8_t(entry.confidence + 1));
            entry.lastAddr = addr;
            bestEntry = &entry;
            break;
        } else if (std::abs(delta) <= 512 && delta != 0) {
            // New stride detected
            entry.stride = delta;
            entry.confidence = 1;
            entry.lastAddr = addr;
            bestEntry = &entry;
            break;
        }
    }

    if (!bestEntry) {
        // Evict oldest entry (front of deque)
        bestEntry = &strideTable.front();
    }

    if (!bestEntry->valid) {
        bestEntry->lastAddr = addr;
        bestEntry->stride = 64;
        bestEntry->confidence = 0;
        bestEntry->valid = true;
        return;
    }

    // Generate predictions if confidence is high enough
    if (bestEntry->confidence >= 2 && bestEntry->stride != 0) {
        for (int d = 1; d <= prefetchDegree; d++) {
            uint64_t predAddr = addr +
                static_cast<uint64_t>(bestEntry->stride * d);
            predictions.push_back(predAddr);
        }
    }
}

void
GraphDropletPrefetcher::issueIndirectPrefetches(
    uint64_t edgeAddr, std::vector<AddrPriority>& addresses)
{
    if (!propertyConfigured) return;

    uint64_t baseLineAddr = edgeAddr & ~uint64_t(63);

    for (int i = 0; i < indirectDegree; i++) {
        uint64_t targetEdgeAddr = baseLineAddr +
            static_cast<uint64_t>(i * edgeElemSize);

        if (!isEdgeArrayAccess(targetEdgeAddr)) break;

        uint32_t vertexId = 0;
        uint64_t edgeOffset = (targetEdgeAddr - edgeArrayBase) / edgeElemSize;
        if (edgeDataLoaded) {
            if (edgeOffset >= edgeData.size()) break;
            vertexId = edgeData[edgeOffset];
        } else {
            vertexId = static_cast<uint32_t>(edgeOffset);
        }

        uint64_t propAddr = propertyAddrForVertex(vertexId);
        if (isPropertyAccess(propAddr) && shouldPrefetch(propAddr)) {
            addresses.push_back(AddrPriority(propAddr, -1));
        }
    }
}

bool
GraphDropletPrefetcher::shouldPrefetch(uint64_t addr)
{
    uint64_t lineAddr = addr & ~uint64_t(63);

    if (recentPrefetches.count(lineAddr)) return false;

    recentPrefetches.insert(lineAddr);
    if (recentPrefetches.size() > MAX_RECENT) {
        // Simple eviction: clear half the set
        auto it = recentPrefetches.begin();
        size_t toRemove = recentPrefetches.size() / 2;
        while (toRemove > 0 && it != recentPrefetches.end()) {
            it = recentPrefetches.erase(it);
            toRemove--;
        }
    }
    return true;
}

} // namespace prefetch
} // namespace gem5
