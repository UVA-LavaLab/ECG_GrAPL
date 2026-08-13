// GraphBrew graph-cache context for Sniper overlays.
//
// This file mirrors the active cache_sim/gem5 metadata model while remaining a
// tracked overlay. It is not copied into the ignored upstream Sniper checkout
// until the cache-set policy wiring is finalized.

#ifndef GRAPHBREW_SNIPER_GRAPH_CACHE_CONTEXT_H
#define GRAPHBREW_SNIPER_GRAPH_CACHE_CONTEXT_H

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace graphbrew {
namespace sniper {

static constexpr uint32_t MAX_REGION_BUCKETS = 16;
static constexpr uint32_t MAX_PROPERTY_REGIONS = 8;
static constexpr uint32_t MAX_TRACKED_CORES = 1024;
static constexpr uint64_t GRAPHBREW_SET_VERTEX_WORK_ID = 0x47525654ULL;
static constexpr uint64_t GRAPHBREW_CONTEXT_READY_WORK_ID = 0x47524358ULL;
static constexpr uint64_t GRAPHBREW_ECG_PFX_TARGET_WORK_ID = 0x47504658ULL;
// SNIPER_ECG_EXTRACT: delivers a per-edge next-ref epoch
// (vertex[31:0] | epoch[15:0]<<32, entirely within the magic ABI's low 48 bits) so
// ECG_GRASP_POPT eviction can use a delivered (HW-faithful) epoch instead of the
// host-side findNextRef matrix, matching gem5/cache_sim.
static constexpr uint64_t GRAPHBREW_ECG_EXTRACT_WORK_ID = 0x47464C44ULL;  // ECG epoch-extract delivery
static constexpr uint64_t GRAPHBREW_ECG_EXTRACT2_WORK_ID = 0x47464C45ULL;
static constexpr uint64_t GRAPHBREW_K2_BIND_WORK_ID = 0x4B32424EULL;
static constexpr uint64_t GRAPHBREW_K2_CLEAR_WORK_ID = 0x4B324243ULL;

void setCurrentVertexHint(uint32_t core_id, uint64_t vertex);
bool hasCurrentVertexHint(uint32_t core_id);
bool hasAnyCurrentVertexHint();
uint32_t getCurrentVertexHint(uint32_t core_id);
void clearCurrentVertexHint(uint32_t core_id);
void setCurrentNucaRequesterCore(uint32_t core_id);
uint32_t currentNucaRequesterCore();

void setPrefetchTargetHint(uint32_t core_id, uint64_t vertex);
bool hasPrefetchTargetHint(uint32_t core_id);
uint32_t getPrefetchTargetHint(uint32_t core_id);
bool consumePrefetchTargetHint(uint32_t core_id, uint32_t& vertex);
void clearPrefetchTargetHint(uint32_t core_id);

// SNIPER_ECG_EXTRACT per-edge epoch delivery: a bounded per-core map keyed by
// property cache line (the delivered epoch is line-min). A collision invalidates
// the old line tag instead of falling back to stale metadata from another vertex.
void recordEcgEpoch(uint32_t core_id, uint32_t vertex, uint16_t epoch);
void recordEcgEpochPair(uint32_t core_id, uint32_t vertex,
                        uint8_t tier, uint16_t first, uint16_t second);
void clearEcgEpochPair(uint32_t core_id, uint32_t vertex);
bool lookupEcgEpoch(uint32_t core_id, uint32_t vertex,
                    uint16_t& epoch, uint64_t& sequence);
bool lookupEcgEpochPair(uint32_t core_id, uint32_t vertex,
                        uint8_t& tier, uint16_t& first, uint16_t& second,
                        uint8_t& count, uint64_t& sequence);
void recordBoundK2Load(uint32_t core_id, uint64_t address);
void clearBoundK2Load(uint32_t core_id);
bool consumeBoundK2Load(
    uint32_t core_id, uint64_t line_addr, uint64_t line_size,
    uint16_t* current_epoch = nullptr, uint16_t* context_id = nullptr,
    uint64_t* trace_sequence = nullptr);
void beginEcgContext();
void endEcgContext();
uint16_t currentEcgContextId();

enum class ECGMode : uint8_t {
    DBG_PRIMARY,
    POPT_PRIMARY,
    DBG_ONLY,
    ECG_EMBEDDED,
    ECG_COMBINED,
    ECG_GRASP_POPT,
};

ECGMode stringToECGMode(const std::string& text);
std::string ecgModeToString(ECGMode mode);

struct PropertyRegion {
    std::string name;
    uint64_t base_address = 0;
    uint64_t upper_bound = 0;
    uint32_t num_elements = 0;
    uint32_t elem_size = 0;
    uint32_t region_id = 0;
    uint32_t num_buckets = 0;
    bool grasp_region = true;
    std::array<uint64_t, MAX_REGION_BUCKETS> bucket_bounds{};

    bool contains(uint64_t addr) const;
    uint32_t classifyBucket(uint64_t addr) const;
};

struct EdgeRegion {
    std::string name;
    uint64_t base_address = 0;
    uint64_t upper_bound = 0;
    uint32_t elem_size = 0;
    bool preferred = false;
    std::string data_path;

    bool contains(uint64_t addr) const;
};

struct RereferenceMatrix {
    std::vector<uint8_t> data;
    uint32_t num_cache_lines = 0;
    uint32_t num_epochs = 256;
    uint32_t epoch_size = 0;
    uint32_t sub_epoch_size = 0;
    uint64_t base_address = 0;
    uint64_t cache_line_size = 64;
    bool enabled = false;

    bool loadFromFile(const std::string& path);
    uint32_t findNextRef(uint32_t cline_id, uint32_t current_vertex) const;
    uint32_t findNextRefByAddr(uint64_t addr, uint32_t current_vertex) const;
};

struct MaskConfig {
    uint8_t mask_width = 8;
    uint8_t dbg_bits = 2;
    uint8_t popt_bits = 4;
    uint8_t prefetch_bits = 2;
    uint8_t num_buckets = 11;
    uint8_t rrpv_max = 7;
    ECGMode ecg_mode = ECGMode::DBG_PRIMARY;
    bool enabled = false;

    uint8_t prefetch_shift = 0;
    uint8_t popt_shift = 0;
    uint8_t dbg_shift = 0;
    uint32_t prefetch_mask_val = 0;
    uint32_t popt_mask_val = 0;
    uint32_t dbg_mask_val = 0;

    void computeShifts();
    uint8_t decodeDBG(uint32_t mask_entry) const;
    uint8_t decodePOPT(uint32_t mask_entry) const;
    uint8_t dbgTierToRRPV(uint8_t dbg_tier) const;
};

struct GraphTopology {
    uint32_t num_vertices = 0;
    uint64_t num_edges = 0;
    uint32_t num_buckets = 11;
    double avg_degree = 0.0;
    uint32_t max_degree = 0;
    std::array<uint32_t, MAX_REGION_BUCKETS> bucket_vertex_counts{};
    std::array<uint64_t, MAX_REGION_BUCKETS> bucket_degrees{};
    bool enabled = false;
};

struct GraphCacheContext {
    std::array<PropertyRegion, MAX_PROPERTY_REGIONS> regions{};
    uint32_t num_regions = 0;
    std::array<EdgeRegion, 2> edge_regions{};
    uint32_t num_edge_regions = 0;
    uint64_t stream_bypass_base = 0;
    uint64_t stream_bypass_upper = 0;
    std::vector<uint64_t> k2_offsets;
    std::vector<uint64_t> k2_line_offsets;
    std::vector<uint32_t> k2_line_ids;
    std::vector<uint64_t> k2_line_records;
    std::vector<uint64_t> k2_line_indices;
    std::vector<uint64_t> k2_line8_offsets;
    std::vector<uint32_t> k2_line8_ids;
    std::vector<uint64_t> k2_line8_records;
    std::vector<uint64_t> k2_line8_indices;
    mutable std::atomic<uint64_t> k2_profile_calls{0};
    mutable std::atomic<uint64_t> k2_profile_found{0};
    mutable std::atomic<uint64_t> k2_profile_total_ns{0};
    mutable std::atomic<uint64_t> k2_profile_classify_ns{0};
    mutable std::atomic<uint64_t> k2_profile_search_ns{0};

    GraphTopology topology;
    MaskConfig mask_config;
    RereferenceMatrix rereference;

    uint32_t current_src_vertex = 0;
    mutable uint32_t current_dst_vertex = 0;
    uint8_t current_mask = 0;
    mutable uint32_t current_outer_vertex = 0;
    bool loaded = false;

    // Epoch count (ne) for SNIPER_ECG_EXTRACT circular next-ref distance; must
    // match the kernel's ECG_EDGE_MASK_EPOCHS packing. Set at context load.
    uint32_t edge_epoch_count = 256;

    ~GraphCacheContext();
    bool loadFromSideband(const std::string& path);
    bool loadRereferenceMatrix(const std::string& path);
    void setCacheLineSize(uint64_t line_size);

    uint32_t currentVertexForPopt(uint32_t core_id) const;
    uint16_t currentEcgEpoch(uint32_t core_id) const;
    void updateVertexFromAddr(uint64_t addr, uint32_t core_id) const;
    // Vertex owning a property-region address (UINT32_MAX if not property data).
    uint32_t vertexForAddress(uint64_t addr) const;
    // elem_size of the property region owning addr (0 if none).
    uint32_t propertyElemSizeForAddress(uint64_t addr) const;
    bool isPropertyData(uint64_t addr) const;
    bool isEcgEpochData(uint64_t addr) const;
    bool isStreamBypassData(uint64_t addr) const;
    bool lookupFusedK2Pair(uint64_t line_addr, uint32_t core_id,
                           uint8_t& tier,
                           uint16_t& first, uint16_t& second,
                           uint64_t trace_sequence = ~uint64_t{0}) const;
    bool isEdgeData(uint64_t addr) const;
    uint32_t classifyBucket(uint64_t addr) const;
    uint32_t findNextRefAtVertex(
        uint64_t addr, uint32_t current_vertex) const;
    uint32_t findNextRef(uint64_t addr, uint32_t core_id) const;
    uint32_t classifyGRASP(uint64_t addr, uint64_t llc_size) const;
    uint8_t getInsertRRPV(uint64_t addr) const;
};

GraphCacheContext& globalContext();
bool isEcgStreamBypassAddress(uint64_t addr);
void recordEcgPlacementMiss(uint64_t addr);

}  // namespace sniper
}  // namespace graphbrew

#endif  // GRAPHBREW_SNIPER_GRAPH_CACHE_CONTEXT_H