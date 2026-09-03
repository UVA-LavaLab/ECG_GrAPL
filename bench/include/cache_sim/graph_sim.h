// Copyright (c) 2024, UVA LavaLab
// Graph Simulation Helper for Cache Tracking
// Provides helper functions and macros for cache simulation

#ifndef GRAPH_SIM_H_
#define GRAPH_SIM_H_

#include "cache_sim.h"
#include "../ecg_metadata.h"
#include "graph_cache_context.h"
#include <graph.h>
#include <pvector.h>
#include <cstdlib>
#include <string>

namespace cache_sim {

static constexpr size_t GRAPH_SIM_PROPERTY_ALIGNMENT = 4096;
static constexpr uint64_t GRAPH_SIM_IN_RECORD_BASE = 0x100000000000ULL;
static constexpr uint64_t GRAPH_SIM_OUT_RECORD_BASE = 0x200000000000ULL;
// S2 sidecar arrays. Distinct from the S1 record bases so a run can never
// alias the two structures, and far from any property region.
static constexpr uint64_t GRAPH_SIM_IN_SIDECAR_BASE  = 0x300000000000ULL;
static constexpr uint64_t GRAPH_SIM_OUT_SIDECAR_BASE = 0x400000000000ULL;

inline int GraphSimEnvIntClamped(const char* name, int default_value,
                                 int min_value, int max_value) {
    const char* value = std::getenv(name);
    if (!value) return default_value;
    int parsed = std::atoi(value);
    return std::max(min_value, std::min(max_value, parsed));
}

inline EvictionPolicy GraphSimEffectiveL3Policy() {
    EvictionPolicy policy =
        GetEnvPolicy("CACHE_POLICY", EvictionPolicy::LRU);
    return GetEnvPolicy("CACHE_L3_POLICY", policy);
}

inline bool GraphSimEcgGraspPoptPolicy() {
    const EvictionPolicy policy = GraphSimEffectiveL3Policy();
    const char* mode = std::getenv("ECG_MODE");
    return policy == EvictionPolicy::ECG && mode &&
           StringToECGMode(mode) == ECGMode::ECG_GRASP_POPT;
}

inline bool GraphSimMatrixFreeReusePlan() {
    return GraphSimEcgGraspPoptPolicy() &&
           GraphSimEnvIntClamped("ECG_REUSE_PLAN_DEPTH", 0, 0, 4) == 2;
}

inline bool GraphSimEcgEdgeRecord() {
    const bool masks_enabled =
        std::getenv("ECG_EDGE_MASKS") != nullptr ||
        std::getenv("ECG_BFS_EDGE_MASKS") != nullptr ||
        std::getenv("ECG_SSSP_EDGE_MASKS") != nullptr ||
        std::getenv("ECG_BC_EDGE_MASKS") != nullptr ||
        std::getenv("ECG_CC_EDGE_MASKS") != nullptr;
    return GraphSimEcgGraspPoptPolicy() && masks_enabled;
}

inline int GraphSimEcgRecordBytes(uint64_t num_vertices, int epoch_bits) {
    int forced = GraphSimEnvIntClamped(
        "ECG_EDGE_RECORD_BYTES", 0, 0, 16);
    if (forced == 4 || forced == 8 || forced == 16) return forced;
    int id_bits = 1;
    while (id_bits < 32 &&
           (uint64_t(1) << id_bits) < num_vertices) {
        ++id_bits;
    }

    int tier_bits = GraphSimEnvIntClamped(
        "ECG_RECORD_TIER_BITS", 2, 0, 8);
    int popt_bits = GraphSimEnvIntClamped(
        "ECG_RECORD_POPT_BITS", 0, 0, 8);
    int prefetch_bits = GraphSimEnvIntClamped(
        "ECG_RECORD_PREFETCH_BITS", 0, 0, 32);
    const bool ref32_record =
        std::getenv("ECG_REF32_RECORD") != nullptr;
    int reuse_plan_depth = GraphSimEnvIntClamped(
        "ECG_REUSE_PLAN_DEPTH", 0, 0, 4);
    // two-epoch ReusePlan historically returned 8 bytes unconditionally, skipping the
    // bit budget below. That is an implementation shortcut, not a cost of the
    // second future epoch: on a 65,536-vertex graph with 5-bit epochs and 2
    // tier bits the two-epoch record needs 16 + 2*5 + 2 = 28 bits and fits in
    // 4 bytes. Charging it 8 doubled ReusePlan's modelled transport and made every
    // ReusePlan-versus-single-epoch comparison a comparison of record widths.
    //
    // ECG_RECORD_VARIABLE_WIDTH=1 computes the width from the same budget as
    // every other schedule. The default preserves the historical 8 bytes so
    // committed results do not silently move.
    static const bool variable_width = [](){
        const char* v = std::getenv("ECG_RECORD_VARIABLE_WIDTH");
        return v && std::atoi(v) != 0;
    }();
    if (reuse_plan_depth == 2 && !variable_width) return 8;
    int epoch_payload_bits = epoch_bits * std::max(1, reuse_plan_depth);
    int needed = id_bits + epoch_payload_bits +
                 tier_bits + popt_bits + prefetch_bits;
    if (ref32_record)
        needed = id_bits +
            GraphSimEnvIntClamped(
                "ECG_REF32_REFERENCE_BITS", 8, 5, 12) +
            2 +
            GraphSimEnvIntClamped(
                "ECG_REF32_ACTION_BITS", 4, 0, 12);
    if (needed <= 32) return 4;
    if (needed <= 64) return 8;
    return 16;
}

inline int GraphSimEcgWeightedSidecarBytes(
        uint64_t num_vertices, int epoch_bits) {
    if (GraphSimEnvIntClamped("ECG_REUSE_PLAN_DEPTH", 0, 0, 4) == 2)
        return 4;
    return GraphSimEcgRecordBytes(num_vertices, epoch_bits);
}


// ============================================================================
// SimArray: Wrapper for property arrays with cache tracking
// Works with both single-core CacheHierarchy and MultiCoreCacheHierarchy
// ============================================================================
template<typename T, typename CacheType = CacheHierarchy>
class SimArray {
public:
    SimArray(pvector<T>& arr, CacheType& cache)
        : data_(arr.data()), size_(arr.size()), cache_(cache) {}
    
    SimArray(T* data, size_t size, CacheType& cache)
        : data_(data), size_(size), cache_(cache) {}

    // Read with tracking
    T read(size_t index) const {
        cache_.readArray(data_, index);
        return data_[index];
    }

    // Write with tracking
    void write(size_t index, const T& value) {
        cache_.writeArray(data_, index);
        data_[index] = value;
    }

    // Atomic add with tracking
    void atomicAdd(size_t index, const T& value) {
        cache_.readArray(data_, index);
        cache_.writeArray(data_, index);
        #pragma omp atomic
        data_[index] += value;
    }

    // Get raw pointer
    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }

private:
    T* data_;
    size_t size_;
    CacheType& cache_;
};

// ============================================================================
// Convenience macros for cache tracking with explicit cache instance
// (Use these in simulation code that passes a specific cache object)
// ============================================================================

template <typename Cache>
inline decltype(auto) access_with_site(
        Cache& cache, uint64_t address, bool is_write, uint64_t site_id) {
    HawkeyeSiteScope scope(site_id);
    return cache.access(address, is_write);
}

template <typename Cache>
inline decltype(auto) access_stream_with_site(
        Cache& cache, uint64_t address, bool is_write, uint64_t site_id) {
    HawkeyeSiteScope scope(site_id);
    return cache.accessStream(address, is_write);
}

template <typename Cache>
inline decltype(auto) access_structural_stream_with_site(
        Cache& cache, uint64_t address, bool is_write, uint64_t site_id) {
    HawkeyeSiteScope scope(site_id);
    return cache.accessStructuralStream(address, is_write);
}

template <typename Cache>
inline void prefetch_with_site(
        Cache& cache, uint64_t address, uint64_t site_id) {
    HawkeyeSiteScope scope(site_id);
    cache.prefetch(address);
}

// Structural-FlowThrough, offered to EVERY policy rather than to ReusePlan alone.
//
// FlowThrough lets ReusePlan read its one-touch per-edge records without allocating
// them in the LLC. The same argument applies to any policy's structural CSR
// edge stream: it is sequential and read-once, so allocating it evicts reusable
// property lines. Leaving the option available only to ReusePlan confounds "ReusePlan's
// replacement is better" with "ReusePlan is the only policy allowed to use
// FlowThrough".
// FLOWTHROUGH=1 applies it uniformly to the CSR edge stream of every
// kernel and every policy, so the two effects can be separated.
inline bool flowthrough_enabled() {
    static const bool enabled = [](){
        const char* v = std::getenv("FLOWTHROUGH");
        const bool active = v && std::atoi(v) != 0;
        if (active)
            std::fprintf(
                stderr,
                "[STRUCTURAL-FLOWTHROUGH sim=cache_sim active=1]\n");
        return active;
    }();
    return enabled;
}

template <typename Cache>
inline decltype(auto) access_edge_with_site(
        Cache& cache, uint64_t address, uint64_t site_id) {
    HawkeyeSiteScope scope(site_id);
    if (flowthrough_enabled())
        return cache.accessStructuralStream(address, false);
    return cache.access(address, false);
}

#define CACHE_SIM_HAWKEYE_SITE_ID \
    (static_cast<uint64_t>(__COUNTER__) + 1ULL)
// The proxy ID is binary-local, like a machine PC. Rebuilds may renumber sites;
// completion hashes bind every result to the exact workload binary.

// Track reading from array element (with explicit cache instance)
#define SIM_CACHE_READ(cache, arr, idx) \
    ::cache_sim::access_with_site( \
        (cache), reinterpret_cast<uint64_t>(&(arr)[idx]), false, \
        CACHE_SIM_HAWKEYE_SITE_ID)

// Track writing to array element (with explicit cache instance)
#define SIM_CACHE_WRITE(cache, arr, idx) \
    ::cache_sim::access_with_site( \
        (cache), reinterpret_cast<uint64_t>(&(arr)[idx]), true, \
        CACHE_SIM_HAWKEYE_SITE_ID)

// Track reading neighbor iteration (one cache access per neighbor)
#define SIM_CACHE_TRACK_NEIGHBOR(cache, neighbor_ptr) \
    ::cache_sim::access_with_site( \
        (cache), reinterpret_cast<uint64_t>(neighbor_ptr), false, \
        CACHE_SIM_HAWKEYE_SITE_ID)

// P-OPT / GRASP: Update current destination vertex being processed.
// Call this at the top of the outer loop (for each destination vertex)
// so P-OPT can compute next-reference distances from the rereference matrix.
#define SIM_SET_VERTEX(cache, vertex_id) \
    (cache).setCurrentVertex(static_cast<uint32_t>(vertex_id))

// ECG: Read with per-edge mask hint.
// Sets the mask in GraphCacheContext before the access so the ECG policy
// can read DBG tier + P-OPT quant from the mask instead of address-range.
// mask_val = pre-encoded mask entry from the parallel mask array.
#define SIM_CACHE_READ_MASKED(cache, arr, idx, graph_ctx, mask_val) \
    do { \
        (graph_ctx).hints_for_thread().mask = static_cast<uint32_t>(mask_val); \
        ::cache_sim::access_with_site( \
            (cache), reinterpret_cast<uint64_t>(&(arr)[idx]), false, \
            CACHE_SIM_HAWKEYE_SITE_ID); \
    } while(0)

// ECG: Read with mask + prefetch hint.
// After the primary access, resolves the prefetch target from the mask
// and issues a prefetch if the target is not in the runtime dedup window.
// Prefetch uses cache.prefetch() which fills the cache WITHOUT counting
// as a demand access — prefetch misses don't inflate the miss rate.
#define SIM_CACHE_READ_MASKED_PREFETCH(cache, arr, idx, graph_ctx, mask_val) \
    do { \
        const uint64_t _hawkeye_site = CACHE_SIM_HAWKEYE_SITE_ID; \
        (graph_ctx).hints_for_thread().mask = static_cast<uint32_t>(mask_val); \
        ::cache_sim::access_with_site( \
            (cache), reinterpret_cast<uint64_t>(&(arr)[idx]), false, \
            _hawkeye_site); \
        uint32_t _pfx_target = (graph_ctx).resolvePrefetchTarget(mask_val); \
        if (_pfx_target != UINT32_MAX) { \
            auto& _dw = (graph_ctx).dedup_for_thread(); \
            if (!_dw.contains(_pfx_target)) { \
                _dw.push(_pfx_target); \
                ::cache_sim::prefetch_with_site( \
                    (cache), reinterpret_cast<uint64_t>(&(arr)[_pfx_target]), \
                    _hawkeye_site); \
                (graph_ctx).recordPrefetchIssued(); \
            } else { \
                (graph_ctx).recordPrefetchDuplicate(); \
            } \
        } else { \
            (graph_ctx).recordPrefetchNoTarget(); \
        } \
    } while(0)

// ECG: Prefetch a known future vertex property element.
// This is useful for runtime lookahead paths where the access stream already
// exposes a future vertex ID and the current mask target would be too late.
#define SIM_CACHE_PREFETCH_VERTEX(cache, arr, idx, graph_ctx) \
    do { \
        const uint64_t _hawkeye_site = CACHE_SIM_HAWKEYE_SITE_ID; \
        uint32_t _pfx_target = static_cast<uint32_t>(idx); \
        auto& _dw = (graph_ctx).dedup_for_thread(); \
        if (!_dw.contains(_pfx_target)) { \
            _dw.push(_pfx_target); \
            ::cache_sim::prefetch_with_site( \
                (cache), reinterpret_cast<uint64_t>(&(arr)[_pfx_target]), \
                _hawkeye_site); \
            (graph_ctx).recordPrefetchIssued(); \
        } else { \
            (graph_ctx).recordPrefetchDuplicate(); \
        } \
    } while(0)

// Track CSR edge list traversal (reading neighbor IDs from edge array).
// Call once per edge during neighbor iteration. Honours FLOWTHROUGH so
// every policy, not only ReusePlan, can decline to allocate this one-touch stream.
#define SIM_CACHE_READ_EDGE(cache, neighbor_ptr) \
    ::cache_sim::access_edge_with_site( \
        (cache), reinterpret_cast<uint64_t>(neighbor_ptr), \
        CACHE_SIM_HAWKEYE_SITE_ID)

#define SIM_CACHE_READ_EDGE_RECORD(cache, neighbor_ptr, edge_base, synthetic_base, record_bytes) \
    do { \
        const uint64_t _edge_index = static_cast<uint64_t>( \
            (neighbor_ptr) - (edge_base)); \
        const uint64_t _record_addr = (synthetic_base) + \
            _edge_index * static_cast<uint64_t>(record_bytes); \
        const uint64_t _hawkeye_site = CACHE_SIM_HAWKEYE_SITE_ID; \
        if ((record_bytes) >= 16) { \
            ::cache_sim::access_with_site( \
                (cache), _record_addr, false, _hawkeye_site); \
            ::cache_sim::access_with_site( \
                (cache), _record_addr + 8ULL, false, _hawkeye_site); \
        } else { \
            ::cache_sim::access_with_site( \
                (cache), _record_addr, false, _hawkeye_site); \
        } \
    } while (0)

#define SIM_CACHE_READ_EDGE_RECORD_FLOWTHROUGH(cache, neighbor_ptr, edge_base, synthetic_base, record_bytes) \
    do { \
        const uint64_t _edge_index = static_cast<uint64_t>( \
            (neighbor_ptr) - (edge_base)); \
        const uint64_t _record_addr = (synthetic_base) + \
            _edge_index * static_cast<uint64_t>(record_bytes); \
        const uint64_t _hawkeye_site = CACHE_SIM_HAWKEYE_SITE_ID; \
        if ((record_bytes) >= 16) { \
            ::cache_sim::access_stream_with_site( \
                (cache), _record_addr, false, _hawkeye_site); \
            ::cache_sim::access_stream_with_site( \
                (cache), _record_addr + 8ULL, false, _hawkeye_site); \
        } else { \
            ::cache_sim::access_stream_with_site( \
                (cache), _record_addr, false, _hawkeye_site); \
        } \
    } while (0)

// Single metadata-delivery site for every cache_sim kernel.
//
// The structure, width and placement all come from ecg_metadata::Config, which
// is byte-identical to the copy gem5 and Sniper compile. Kernels no longer each
// carry an if/else chain over delivery modes, which is what previously let them
// disagree about which structure they were using.
//
// PackedRecord SUBSTITUTES for the CSR edge. Sidecar reads the CSR edge through
// the ordinary edge path and adds a narrow bit-packed entry, so the FlowThrough flag
// applies only to the metadata and never grants ReusePlan an edge-placement privilege
// the baselines lack.
#define SIM_ECG_EDGE(cache, cfg, neighbor_ptr, edge_base, record_base, sidecar_base) \
    do { \
        const uint64_t _site = CACHE_SIM_HAWKEYE_SITE_ID; \
        const uint64_t _idx = static_cast<uint64_t>( \
            (neighbor_ptr) - (edge_base)); \
        if (!(cfg).charged || (cfg).delivery == ::ecg_metadata::Delivery::None) { \
            ::cache_sim::access_edge_with_site( \
                (cache), reinterpret_cast<uint64_t>(neighbor_ptr), _site); \
        } else if ((cfg).delivery == ::ecg_metadata::Delivery::Sidecar) { \
            ::cache_sim::access_edge_with_site( \
                (cache), reinterpret_cast<uint64_t>(neighbor_ptr), _site); \
            const uint64_t _a = ::ecg_metadata::sidecarAddress( \
                (cfg), (sidecar_base), _idx); \
            if ((cfg).flowthrough) \
                ::cache_sim::access_stream_with_site((cache), _a, false, _site); \
            else \
                ::cache_sim::access_with_site((cache), _a, false, _site); \
        } else { \
            const uint64_t _a = ::ecg_metadata::recordAddress( \
                (cfg), (record_base), _idx); \
            for (int _h = 0; _h < ((cfg).record_bytes >= 16 ? 2 : 1); ++_h) { \
                const uint64_t _ha = _a + static_cast<uint64_t>(_h) * 8ULL; \
                if (::cache_sim::flowthrough_enabled()) \
                    ::cache_sim::access_structural_stream_with_site( \
                        (cache), _ha, false, _site); \
                else if ((cfg).flowthrough) \
                    ::cache_sim::access_stream_with_site( \
                        (cache), _ha, false, _site); \
                else \
                    ::cache_sim::access_with_site((cache), _ha, false, _site); \
            } \
        } \
    } while (0)

// ECG FlowThrough: one-touch packed edge records can skip shared-LLC insertion
// while still filling the private caches. Only ECG's explicit stream path uses
// this; baseline CSR accesses remain unchanged.
#define SIM_CACHE_READ_EDGE_FLOWTHROUGH(cache, neighbor_ptr) \
    ::cache_sim::access_stream_with_site( \
        (cache), reinterpret_cast<uint64_t>(neighbor_ptr), false, \
        CACHE_SIM_HAWKEYE_SITE_ID)
#define SIM_CACHE_READ_FLOWTHROUGH(cache, ptr, idx) \
    ::cache_sim::access_stream_with_site( \
        (cache), reinterpret_cast<uint64_t>(&(ptr)[idx]), false, \
        CACHE_SIM_HAWKEYE_SITE_ID)

// Track CSR offset array access (reading row pointer for vertex u).
// Call once per vertex to track the offset[u] and offset[u+1] lookups.
#define SIM_CACHE_READ_OFFSET(cache, offset_arr, u) \
    do { \
        const uint64_t _hawkeye_site = CACHE_SIM_HAWKEYE_SITE_ID; \
        ::cache_sim::access_with_site( \
            (cache), reinterpret_cast<uint64_t>(&(offset_arr)[u]), false, \
            _hawkeye_site); \
        ::cache_sim::access_with_site( \
            (cache), reinterpret_cast<uint64_t>(&(offset_arr)[(u)+1]), false, \
            _hawkeye_site ^ 0x9E3779B97F4A7C15ULL); \
    } while(0)

} // namespace cache_sim

#endif // GRAPH_SIM_H_
