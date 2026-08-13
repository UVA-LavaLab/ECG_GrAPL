// ============================================================================
// DROPLET Indirect Graph Prefetcher for gem5
// ============================================================================
//
// Data-awaRe decOuPLed prEfeTcher for Graphs
// Basak et al. (2019)
//
// Key insight: Graph workloads have two distinct data types:
//   1. Edge lists (CSR adjacency): Short reuse, streaming, fits in L1/L2
//   2. Property data (vertex values): Long reuse, irregular, thrashes LLC
//
// DROPLET uses separated prefetch engines:
//   Edge-list engine:   Stride-based prefetcher for sequential CSR traversal
//   Property engine:    Indirect prefetcher triggered by neighbor IDs from
//                       prefetched edge data → issues property address prefetch
//
// Indirect prefetching chain:
//   1. Prefetch edge list entries ahead → gets neighbor vertex IDs
//   2. Immediately issue property data prefetch for those neighbors
//   3. Decouples from core's dependency chain (avoids pointer-chasing stall)
//
// Reported performance: 1.37× average speedup, up to 1.76× on BFS,
//                      15-45% LLC miss reduction
//
// Complementarity with ECG:
//   DROPLET brings data in faster (prefetch), ECG decides what to keep
//   (replacement). Combined system benefits from both. ECG's prefetch hints
//   in fat-ID approximate DROPLET's hardware benefit at zero metadata cost.
//
// Detailed derivation remains in local research notes.
// ============================================================================

#ifndef __MEM_CACHE_PREFETCH_DROPLET_HH__
#define __MEM_CACHE_PREFETCH_DROPLET_HH__

#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphDropletPrefetcher.hh"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace gem5 {
namespace prefetch {

class GraphDropletPrefetcher : public Queued
{
  public:
    PARAMS(GraphDropletPrefetcher);

    GraphDropletPrefetcher(const Params &p);
    ~GraphDropletPrefetcher() override = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    // Configure graph memory layout (called from Python config)
    void setPropertyRegion(uint64_t base, uint64_t size, uint32_t elemSize) {
        propertyBase = base;
        propertyEnd = base + size;
        propertyElemSize = elemSize;
        propertyConfigured = true;
    }

    void setEdgeArrayRegion(uint64_t base, uint64_t size) {
        edgeArrayBase = base;
        edgeArrayEnd = base + size;
        edgeConfigured = true;
    }

  private:
    // ── Edge-list stride detector ──
    struct StrideEntry {
        uint64_t lastAddr = 0;
        int64_t stride = 0;
        uint8_t confidence = 0;
        bool valid = false;
    };

    // ── Configuration ──
    const int prefetchDegree;      // How many edges ahead to prefetch
    const int indirectDegree;      // How many indirect property prefetches per edge
    const int strideTableSize;     // Number of stride detector entries

    // ── Property region ──
    uint64_t propertyBase = 0;
    uint64_t propertyEnd = 0;
    uint32_t propertyElemSize = sizeof(float);  // Default: vertex scores
    bool propertyConfigured = false;

    // ── Edge array region ──
    uint64_t edgeArrayBase = 0;
    uint64_t edgeArrayEnd = 0;
    uint32_t edgeElemSize = sizeof(uint32_t);
    bool edgeConfigured = false;

    // Shadow copy of CSR edge IDs, used to model the reference property address
    // generator reading prefetched structure cache-line contents.
    std::vector<uint32_t> edgeData;
    bool edgeDataLoaded = false;

    // ── Runtime sideband configuration ──
    bool sidebandTried = false;
    bool sidebandLoaded = false;
    uint64_t sidebandProbeCount = 0;
    bool reportedSideband = false;
    bool reportedFirstAccess = false;
    bool reportedFirstEdge = false;

    // ── Stride detector state ──
    std::deque<StrideEntry> strideTable;

    // ── Recently prefetched filter (avoid redundant prefetches) ──
    std::unordered_set<uint64_t> recentPrefetches;
    static constexpr size_t MAX_RECENT = 256;

    // ── Helper functions ──
    void tryLoadSideband();
    static uint64_t parseJsonUint(const std::string& json,
                                  const std::string& key);
    static std::string parseJsonString(const std::string& json,
                                       const std::string& key);
    static std::string findPreferredObject(const std::string& json,
                                           const std::string& section,
                                           const std::string& preferred);
    bool loadEdgeData(const std::string& path, uint32_t elemSize);

    bool isEdgeArrayAccess(uint64_t addr) const {
        return edgeConfigured && addr >= edgeArrayBase && addr < edgeArrayEnd;
    }

    bool isPropertyAccess(uint64_t addr) const {
        return propertyConfigured && addr >= propertyBase && addr < propertyEnd;
    }

    // Compute property address from vertex ID
    uint64_t propertyAddrForVertex(uint32_t vertex_id) const {
        return propertyBase +
            static_cast<uint64_t>(vertex_id) * propertyElemSize;
    }

    // Update stride detector and return predicted next addresses
    void updateStrideDetector(uint64_t addr,
                              std::vector<uint64_t>& predictions);

    // Issue indirect property prefetches from edge data
    void issueIndirectPrefetches(uint64_t edgeAddr,
                                 std::vector<AddrPriority>& addresses);

    // Track prefetch to avoid duplicates
    bool shouldPrefetch(uint64_t addr);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_DROPLET_HH__
