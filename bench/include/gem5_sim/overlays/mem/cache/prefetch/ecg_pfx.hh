// ============================================================================
// ECG_PFX Hint-Driven Graph Prefetcher for gem5
// ============================================================================
//
// This prefetcher consumes GraphBrew ECG PFX target hints delivered through a
// pseudo-instruction/m5ops work item. It is intentionally distinct from
// DROPLET: DROPLET derives targets from CSR edge streams, while ECG_PFX consumes
// targets carried by ECG/fat-ID metadata.
// ============================================================================

#ifndef __MEM_CACHE_PREFETCH_ECG_PFX_HH__
#define __MEM_CACHE_PREFETCH_ECG_PFX_HH__

#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"
#include "params/GraphEcgPfxPrefetcher.hh"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace gem5 {
namespace prefetch {

class GraphEcgPfxPrefetcher : public Queued
{
  public:
    PARAMS(GraphEcgPfxPrefetcher);

    GraphEcgPfxPrefetcher(const Params &p);
    ~GraphEcgPfxPrefetcher() override = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

  private:
    const int recentFilterSize;

    uint64_t propertyBase = 0;
    uint64_t propertyEnd = 0;
    uint32_t propertyElemSize = sizeof(float);
    bool propertyConfigured = false;

    bool sidebandLoaded = false;
    uint64_t sidebandProbeCount = 0;
    bool reportedSideband = false;
    bool reportedFirstHint = false;

    std::unordered_set<uint64_t> recentPrefetches;
    std::deque<uint64_t> recentOrder;

    void tryLoadSideband();
    static uint64_t parseJsonUint(const std::string& json,
                                  const std::string& key);
    static std::string findPreferredObject(const std::string& json,
                                           const std::string& section,
                                           const std::string& preferred);
    uint64_t propertyAddrForVertex(uint32_t vertex) const;
    bool isPropertyAddress(uint64_t address) const;
    bool shouldPrefetch(uint64_t address);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_ECG_PFX_HH__