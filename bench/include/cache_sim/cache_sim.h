// Copyright (c) 2024, UVA LavaLab
// Cache Simulator for Graph Algorithm Analysis
// Tracks L1/L2/L3 cache hits and misses with configurable parameters
// Supports multi-core architecture: private L1/L2 per core, shared L3

#ifndef CACHE_SIM_H_
#define CACHE_SIM_H_

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <deque>
#include <random>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <limits>
#include <atomic>
#include <omp.h>
#include <chrono>
#include <parallel/algorithm>

#include "graph_cache_context.h"
#include "../ecg_victim_policy.h"
#include "../hawkeye_policy.h"

namespace cache_sim {

// Whether the per-edge ECG record carries GRASP tier bits at all. A record
// configured with zero tier bits cannot deliver one, so this makes
// ECG_RECORD_TIER_BITS=0 a genuine mechanism ablation rather than a width-only
// change. The insertion paths fall back to address-based GRASP classification,
// so this ablates the CARRIED tier, not GRASP tiering as a whole.
inline bool ecgTierCarried() {
    static const bool carried = [](){
        const char* v = std::getenv("ECG_RECORD_TIER_BITS");
        return !v || std::atoi(v) > 0;
    }();
    return carried;
}


inline thread_local uint64_t current_hawkeye_site_id = 0;

class HawkeyeSiteScope {
  public:
    explicit HawkeyeSiteScope(uint64_t site)
        : previous(current_hawkeye_site_id)
    {
        current_hawkeye_site_id = site;
    }

    ~HawkeyeSiteScope()
    {
        current_hawkeye_site_id = previous;
    }

  private:
    uint64_t previous;
};

inline uint64_t currentHawkeyeSite()
{
    return current_hawkeye_site_id;
}

// ============================================================================
// T-OPT: trace-based TRUE Belady oracle (T_OPT=1). Records the actual L3 input
// stream (post-L1/L2 filtering, identical regardless of L3 policy since L1/L2
// are LRU) and computes the offline MIN miss rate over the ENTIRE stream — the
// absolute optimal floor, algorithm-agnostic. Validates ECG:EXACT flavors and
// proves "no bugs". Single-thread only (OMP_NUM_THREADS=1). Reports to stderr.
//
// Also reports T_OPT_PROP: Belady over the property-data substream only — the
// achievable ceiling for any property-data exact-reuse policy (P-OPT, ECG:EXACT,
// the deployable trace-mask). This is the trace-EXACT full-precision ceiling:
// the best attainable by exactly knowing each property line's next reference in
// the TRUE access order (any algorithm, incl. frontier). EXACT-sweep (adjacency)
// approaches it where sweep-order==truth (pr); the gap on frontier kernels = the
// value of trace-derived ordering over ID-order adjacency.
// ============================================================================
namespace topt {
    inline std::vector<uint64_t> trace;        // ALL L3 input addresses, in order
    inline std::vector<uint64_t> trace_prop;   // property-data L3 addresses, in order
    inline std::vector<uint8_t>  trace_is_prop; // per-entry property flag (aligned with trace)
    inline std::vector<uint8_t>  trace_bypass;  // miss bypasses LLC allocation
    inline std::vector<uint8_t>  trace_prop_bypass;
    inline thread_local bool current_request_bypass = false;
    inline uint32_t offset_bits = 6;
    inline uint64_t num_sets = 1;
    inline uint32_t ways = 16;
    inline bool geom_captured = false;
    inline size_t roi_start_index = 0;
    inline size_t roi_property_start_index = 0;
    inline bool roi_started = false;
    inline uint64_t trace_hash = 1469598103934665603ULL;
    inline uint64_t trace_line_hash = 1469598103934665603ULL;

    inline uint64_t set_index(uint64_t line) {
        return (num_sets & (num_sets - 1)) == 0
            ? line & (num_sets - 1)
            : line % num_sets;
    }

    class RequestClassScope {
      public:
        explicit RequestClassScope(bool bypass)
            : previous(current_request_bypass) {
            current_request_bypass = bypass;
        }
        ~RequestClassScope() { current_request_bypass = previous; }
      private:
        bool previous;
    };

    inline void hash_byte(uint8_t value) {
        trace_hash ^= value;
        trace_hash *= 1099511628211ULL;
    }

    inline void hash_line_byte(uint8_t value) {
        trace_line_hash ^= value;
        trace_line_hash *= 1099511628211ULL;
    }

    inline void mark_roi_start() {
        roi_start_index = trace.size();
        roi_property_start_index = trace_prop.size();
        roi_started = true;
        trace_hash = 1469598103934665603ULL;
        trace_line_hash = 1469598103934665603ULL;
    }

    // Parallel next-occurrence construction — the deployable traversal-mask bottleneck.
    // next_use[i] = next index j>i with the same cache line, else INF. A line maps to
    // exactly ONE set, so per-set next-occurrence == global next-occurrence: we bucket
    // indices by the DENSE set index (parallel counting-sort, O(T), no log factor) then
    // compute next-occurrence within each set in PARALLEL (each set's small line-set fits
    // in cache). Same embarrassingly-parallel structure P-OPT/sweep construction use, so
    // the traversal mask builds in parallel too. TOPT_SEQ=1 forces the sequential
    // reference path (A/B timing + correctness check). The forward Belady sim that
    // CONSUMES next_use stays sequential (inherent) — that's the oracle MEASUREMENT, not
    // part of the deployable mask.
    inline void compute_next_use(const std::vector<uint64_t>& t, std::vector<uint32_t>& next_use) {
        const size_t T = t.size();
        next_use.assign(T, UINT32_MAX);
        if (T == 0) return;
        const uint32_t ob = offset_bits;
        const uint64_t set_count = num_sets;
        static const bool seq = std::getenv("TOPT_SEQ") != nullptr;
        int nthreads = 1;
        if (!seq) {
            // Recording is DONE (atexit) — raise threads for pure post-processing.
            // OMP_NUM_THREADS=1 is a recording-determinism constraint, not a
            // construction one; the next_use result is thread-count-independent.
            const char* tt = std::getenv("TOPT_THREADS");
            nthreads = tt ? std::atoi(tt) : omp_get_num_procs();
            if (nthreads < 1) nthreads = 1;
            omp_set_num_threads(nthreads);
        }
        auto t0 = std::chrono::steady_clock::now();
        if (seq || set_count <= 1) {
            std::unordered_map<uint64_t, uint32_t> np;
            np.reserve(T / 4 + 16);
            for (size_t i = T; i-- > 0; ) {
                uint64_t line = t[i] >> ob;
                auto it = np.find(line);
                next_use[i] = (it == np.end()) ? UINT32_MAX : it->second;
                np[line] = static_cast<uint32_t>(i);
            }
        } else {
            const uint64_t* tp = t.data();
            const int P = nthreads;
            std::vector<size_t> cstart(P + 1);
            for (int p = 0; p <= P; ++p) cstart[p] = (T * (size_t)p) / P;
            // Step 1: per-thread per-set histogram (fully parallel, no contention).
            std::vector<uint64_t> cnt((size_t)P * set_count, 0);
            #pragma omp parallel num_threads(P)
            {
                int p = omp_get_thread_num();
                uint64_t* c = &cnt[(size_t)p * set_count];
                for (size_t i = cstart[p]; i < cstart[p + 1]; ++i)
                    ++c[set_index(tp[i] >> ob)];
            }
            // Step 2: exclusive prefix over (set, thread) -> global start per (thread,set).
            std::vector<uint64_t> off(set_count + 1, 0);
            std::vector<uint64_t> tstart((size_t)P * set_count);
            {
                uint64_t running = 0;
                for (uint64_t s = 0; s < set_count; ++s) {
                    off[s] = running;
                    for (int p = 0; p < P; ++p) {
                        tstart[(size_t)p * set_count + s] = running;
                        running += cnt[(size_t)p * set_count + s];
                    }
                }
                off[set_count] = running;
            }
            // Step 3: scatter (fully parallel; each thread owns disjoint slots, order preserved).
            std::vector<uint32_t> by_set(T);
            #pragma omp parallel num_threads(P)
            {
                int p = omp_get_thread_num();
                std::vector<uint64_t> cur(set_count);
                for (uint64_t s = 0; s < set_count; ++s)
                    cur[s] = tstart[(size_t)p * set_count + s];
                for (size_t i = cstart[p]; i < cstart[p + 1]; ++i) {
                    uint64_t s = set_index(tp[i] >> ob);
                    by_set[cur[s]++] = (uint32_t)i;
                }
            }
            // Step 4: per-set next-occurrence (parallel over sets; small per-set line map).
            #pragma omp parallel for schedule(dynamic, 8)
            for (uint64_t s = 0; s < set_count; ++s) {
                std::unordered_map<uint64_t, uint32_t> last;
                for (uint64_t k = off[s + 1]; k-- > off[s]; ) {
                    uint32_t i = by_set[k];
                    uint64_t line = tp[i] >> ob;
                    auto it = last.find(line);
                    next_use[i] = (it == last.end()) ? UINT32_MAX : it->second;
                    last[line] = i;
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "[T_OPT] next_use build: " << ms << " ms for " << T << " accesses ("
                  << (seq ? "sequential" : "parallel")
                  << ", threads=" << nthreads << ")\n";
    }

    // Offline MIN (Belady) over an address stream with the L3 set geometry.
    inline void min_miss(
            const std::vector<uint64_t>& t, uint64_t& hits,
            uint64_t& misses, size_t count_from = 0,
            const std::vector<uint8_t>* bypasses = nullptr) {
        hits = 0; misses = 0;
        const size_t T = t.size();
        if (T == 0) return;
        const uint64_t set_count = num_sets;
        std::vector<uint32_t> next_use;
        compute_next_use(t, next_use);
        std::vector<std::unordered_map<uint64_t, uint32_t>> resident(set_count);
        for (size_t i = 0; i < T; ++i) {
            uint64_t line = t[i] >> offset_bits;
            uint64_t s = set_index(line);
            auto& R = resident[s];
            auto it = R.find(line);
            if (it != R.end()) {
                if (i >= count_from) ++hits;
                it->second = next_use[i];
            } else {
                if (i >= count_from) ++misses;
                if (bypasses && i < bypasses->size() && (*bypasses)[i])
                    continue;
                if (R.size() >= ways) {
                    auto victim = R.begin();
                    for (auto jt = R.begin(); jt != R.end(); ++jt)
                        if (jt->second > victim->second) victim = jt;
                    R.erase(victim);
                }
                R.emplace(line, next_use[i]);
            }
        }
    }

    // EXACT-trace POLICY simulated in the SHARED cache (the deployable trace-mask's
    // ceiling): evict non-property lines first (like the real ECG:EXACT policy),
    // then among property lines evict the one whose NEXT reference in TRUE traversal
    // order (recorded next-occurrence) is farthest. Same policy structure as the real
    // EXACT-sweep, differing ONLY in how property next-reference is derived (recorded
    // traversal order vs ID-order adjacency). The EXACT-sweep-vs-EXACT-trace gap thus
    // isolates the value of trace-derived ordering — large on frontier kernels.
    inline void exact_trace_policy_miss(
            uint64_t& hits, uint64_t& misses, size_t count_from = 0) {
        hits = 0; misses = 0;
        const size_t T = trace.size();
        if (T == 0 || trace_is_prop.size() != T) return;
        const uint64_t set_count = num_sets;
        std::vector<uint32_t> next_use;
        compute_next_use(trace, next_use);
        struct Rl { uint32_t nu; uint8_t prop; };
        std::vector<std::unordered_map<uint64_t, Rl>> res(set_count);
        for (size_t i = 0; i < T; ++i) {
            uint64_t line = trace[i] >> offset_bits;
            uint64_t s = set_index(line);
            uint8_t prop = trace_is_prop[i];
            auto& M = res[s];
            auto it = M.find(line);
            if (it != M.end()) {
                if (i >= count_from) ++hits;
                it->second.nu = next_use[i]; it->second.prop = prop;
            } else {
                if (i >= count_from) ++misses;
                if (i < trace_bypass.size() && trace_bypass[i])
                    continue;
                if (M.size() >= ways) {
                    auto victim = M.end(); bool nonprop = false; uint32_t best = 0;
                    for (auto jt = M.begin(); jt != M.end(); ++jt)
                        if (!jt->second.prop && (!nonprop || jt->second.nu > best)) {
                            nonprop = true; best = jt->second.nu; victim = jt;
                        }
                    if (!nonprop) {
                        best = 0;
                        for (auto jt = M.begin(); jt != M.end(); ++jt)
                            if (victim == M.end() || jt->second.nu > best) {
                                best = jt->second.nu; victim = jt;
                            }
                    }
                    M.erase(victim);
                }
                M.emplace(line, Rl{next_use[i], prop});
            }
        }
    }

    inline void compute_and_report() {
        if (!roi_started || roi_start_index >= trace.size()) {
            std::cerr << "[T_OPT] no ROI L3 accesses recorded\n";
            return;
        }
        const size_t roi_accesses = trace.size() - roi_start_index;
        const size_t roi_property_accesses =
            trace_prop.size() - roi_property_start_index;
        std::cerr << "[T_OPT-TRACE accesses=" << roi_accesses
                  << " property_accesses=" << roi_property_accesses
                  << " hash=" << std::hex << trace_hash
                  << " line_hash=" << trace_line_hash << std::dec
                  << " sets=" << num_sets << " ways=" << ways << "]\n";
        uint64_t h = 0, m = 0;
        min_miss(trace, h, m, roi_start_index, &trace_bypass);
        std::cerr << "[T_OPT] L3 FlowThrough-aware Belady "
                  << "(warm-start ROI window): accesses="
                  << roi_accesses
                  << " hits=" << h << " misses=" << m
                  << " sets=" << num_sets << " ways=" << ways
                  << " miss_rate=" << (static_cast<double>(m) / static_cast<double>(h + m)) << "\n";
        if (!trace_prop.empty()) {
            uint64_t hp = 0, mp = 0;
            min_miss(
                trace_prop, hp, mp, roi_property_start_index,
                &trace_prop_bypass);
            std::cerr << "[T_OPT_PROP] L3 property-only Belady (isolated, optimistic diag): accesses="
                      << roi_property_accesses << " hits=" << hp << " misses=" << mp
                      << " miss_rate=" << (static_cast<double>(mp) / static_cast<double>(hp + mp)) << "\n";
        }
        {
            uint64_t he = 0, me = 0;
            exact_trace_policy_miss(he, me, roi_start_index);
            if (he + me > 0)
                std::cerr << "[EXACT_TRACE] L3 trace-order EXACT policy (shared cache, deployable ceiling): "
                          << "hits=" << he << " misses=" << me
                          << " miss_rate=" << (static_cast<double>(me) / static_cast<double>(he + me)) << "\n";
        }
    }

    inline bool enabled = []{
        bool e = std::getenv("T_OPT") != nullptr;
        if (e) std::atexit([]{ compute_and_report(); });
        return e;
    }();

    inline void capture_geom(uint32_t ob, uint64_t sets, uint32_t w) {
        if (!geom_captured) {
            offset_bits = ob;
            num_sets = sets;
            ways = w;
            geom_captured = true;
        }
    }
    inline void record(
            uint64_t address, bool is_property, bool is_write) {
        trace.push_back(address);
        trace_is_prop.push_back(is_property ? 1 : 0);
        trace_bypass.push_back(current_request_bypass ? 1 : 0);
        if (is_property) {
            trace_prop.push_back(address);
            trace_prop_bypass.push_back(
                current_request_bypass ? 1 : 0);
        }
        if (roi_started) {
            const uint64_t line = address >> offset_bits;
            for (unsigned shift = 0; shift < 64; shift += 8) {
                hash_byte(static_cast<uint8_t>(line >> shift));
                hash_line_byte(static_cast<uint8_t>(line >> shift));
            }
            hash_byte(is_property ? 1 : 0);
            hash_byte(is_write ? 1 : 0);
            hash_byte(current_request_bypass ? 1 : 0);
        }
    }
}

// ============================================================================
// Eviction Policy Enumeration
// ============================================================================
enum class EvictionPolicy {
    LRU,      // Least Recently Used
    FIFO,     // First In First Out
    RANDOM,   // Random eviction
    LFU,      // Least Frequently Used
    PLRU,     // Pseudo-LRU (tree-based)
    SRRIP,    // Static Re-Reference Interval Prediction
    HAWKEYE,  // Hawkeye-style OPTgen + static-access-site predictor
    PIN,      // LRU with pinning of high-reuse graph regions (Faldu et al., 2020)
    GRASP,    // Graph-aware cache replacement (Faldu et al., 2020)
    POPT,     // Practical optimal graph-cache replacement (Balaji et al., 2021)
    ECG       // Expressing Locality for Caching in Graphs — fat-ID encoding (Mughrabi et al., GrAPL)
};

inline std::string PolicyToString(EvictionPolicy policy) {
    switch (policy) {
        case EvictionPolicy::LRU:    return "LRU";
        case EvictionPolicy::FIFO:   return "FIFO";
        case EvictionPolicy::RANDOM: return "RANDOM";
        case EvictionPolicy::LFU:    return "LFU";
        case EvictionPolicy::PLRU:   return "PLRU";
        case EvictionPolicy::SRRIP:  return "SRRIP";
        case EvictionPolicy::HAWKEYE:return "HAWKEYE";
        case EvictionPolicy::PIN:    return "PIN";
        case EvictionPolicy::GRASP:  return "GRASP";
        case EvictionPolicy::POPT:   return "POPT";
        case EvictionPolicy::ECG:    return "ECG";
        default: return "UNKNOWN";
    }
}

inline EvictionPolicy StringToPolicy(const std::string& s) {
    if (s == "LRU" || s == "lru") return EvictionPolicy::LRU;
    if (s == "FIFO" || s == "fifo") return EvictionPolicy::FIFO;
    if (s == "RANDOM" || s == "random") return EvictionPolicy::RANDOM;
    if (s == "LFU" || s == "lfu") return EvictionPolicy::LFU;
    if (s == "PLRU" || s == "plru") return EvictionPolicy::PLRU;
    if (s == "SRRIP" || s == "srrip") return EvictionPolicy::SRRIP;
    if (s == "HAWKEYE" || s == "hawkeye") return EvictionPolicy::HAWKEYE;
    if (s == "PIN" || s == "pin") return EvictionPolicy::PIN;
    if (s == "GRASP" || s == "grasp") return EvictionPolicy::GRASP;
    if (s == "POPT" || s == "popt" || s == "P-OPT" || s == "p-opt") return EvictionPolicy::POPT;
    if (s == "ECG" || s == "ecg") return EvictionPolicy::ECG;
    std::fprintf(stderr, "[FATAL] unknown cache policy '%s'\n", s.c_str());
    std::abort();
}

inline EvictionPolicy GetEnvPolicy(const char* name, EvictionPolicy default_policy) {
    const char* val = std::getenv(name);
    return val ? StringToPolicy(val) : default_policy;
}

inline size_t ParseSizeBytes(const char* value, size_t default_val) {
    if (!value) return default_val;

    char* end;
    size_t result = std::strtoull(value, &end, 10);
    if (result == 0) return default_val;

    if (*end == 'K' || *end == 'k') result *= 1024;
    else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
    else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;

    return result;
}

inline size_t GetEnvSizeBytes(const char* name, size_t default_val) {
    return ParseSizeBytes(std::getenv(name), default_val);
}

// ============================================================================
// Cache Statistics
// ============================================================================
struct CacheStats {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> writebacks{0};
    std::atomic<uint64_t> prop_hits{0};    // hits on PROPERTY data (cached, irregular)
    std::atomic<uint64_t> prop_misses{0};  // misses on PROPERTY data (the metric that matters)

    CacheStats() = default;
    
    // Copy constructor (needed for aggregation)
    CacheStats(const CacheStats& other) 
        : hits(other.hits.load())
        , misses(other.misses.load())
        , reads(other.reads.load())
        , writes(other.writes.load())
        , evictions(other.evictions.load())
        , writebacks(other.writebacks.load())
        , prop_hits(other.prop_hits.load())
        , prop_misses(other.prop_misses.load()) {}
    
    // Copy assignment
    CacheStats& operator=(const CacheStats& other) {
        hits = other.hits.load();
        misses = other.misses.load();
        reads = other.reads.load();
        writes = other.writes.load();
        evictions = other.evictions.load();
        writebacks = other.writebacks.load();
        prop_hits = other.prop_hits.load();
        prop_misses = other.prop_misses.load();
        return *this;
    }

    void reset() {
        hits = 0;
        misses = 0;
        reads = 0;
        writes = 0;
        evictions = 0;
        writebacks = 0;
        prop_hits = 0;
        prop_misses = 0;
    }

    double hitRate() const {
        uint64_t total = hits + misses;
        return total > 0 ? (double)hits / total : 0.0;
    }

    double missRate() const {
        return 1.0 - hitRate();
    }

    uint64_t totalAccesses() const {
        return hits + misses;
    }
};

// ============================================================================
// Cache Line
// ============================================================================
struct CacheLine {
    uint64_t tag = 0;
    bool valid = false;
    bool dirty = false;
    uint64_t last_access = 0;    // For LRU
    uint64_t insert_time = 0;    // For FIFO
    uint64_t access_count = 0;   // For LFU
    uint8_t rrpv = 3;            // For SRRIP, GRASP, P-OPT, ECG
    uint64_t hawkeye_signature = 0;
    bool hawkeye_prefetch = false;
    uint64_t line_addr = 0;      // Cache-line-aligned address
    uint8_t ecg_dbg_tier = 0;    // ECG: stored DBG degree tier (structural, for eviction tiebreak)
    uint8_t ecg_popt_hint = 0;   // ECG_EMBEDDED: stored P-OPT quantized rereference hint
    uint16_t ecg_epoch = 0;      // ECG_GRASP_POPT: stored ABSOLUTE next-ref epoch (full resolution)
    bool ecg_epoch_valid = false; // ECG_GRASP_POPT: a per-edge epoch was DELIVERED to this line.
                                  // Distinguishes a real epoch-0 (low-ID next-referencer) from an
                                  // undelivered line — epoch==0 alone is ambiguous. Mirrors Sniper's
                                  // m_ecg_epoch_valid so all 3 sims represent "stamped" identically.
    // ECG_REUSE_PLAN_DEPTH=K: a short per-line forward SCHEDULE of the next-K ABSOLUTE
    // next-ref epochs (sorted ascending). Recovers the P-OPT matrix's per-epoch
    // self-advance: at eviction the SOONEST schedule entry still ahead of cur_epoch is
    // used, so a resident line is no longer BLIND to references after the first stamped
    // one (the root cause of the 1-D mask's staleness vs the matrix's 2-D row). Inert
    // (n=0) unless ECG_REUSE_PLAN_DEPTH delivers a schedule; ecg_epoch stays primary.
    static constexpr int ECG_REUSE_PLAN_DEPTHMAX = 4;
    uint16_t ecg_epoch_sched[ECG_REUSE_PLAN_DEPTHMAX] = {0, 0, 0, 0};
    uint8_t  ecg_epoch_sched_n = 0;
    uint32_t ecg_exact_pred = UINT32_MAX; // ECG_EXACT_STORED: exact next-ref STAMPED at access (precomputed-mask model)
    uint32_t ecg_next_use = 0;  // quantized absolute next-use position
    ecg_policy::FutureState ecg_future_state =
        ecg_policy::FutureState::UNKNOWN;
    uint32_t ecg_ref32_deadline = 0;
    uint64_t ecg_ref32_exact_deadline = 0;
    ecg_ref32::State ecg_ref32_state = ecg_ref32::State::UNKNOWN;
    bool ecg_ref32_prefetch = false;
    bool pin = false;            // PIN policy: line is pinned in cache (high-reuse region)
};

enum class Ref32UpdateResult : uint8_t {
    APPLIED = 0,
    NOT_RESIDENT = 1,
    EXPIRED = 2,
};

// ============================================================================
// P-OPT State: Rereference Matrix context for graph-aware replacement
// ============================================================================
struct POPTState {
    const uint8_t* reref_matrix = nullptr;  // Compressed rereference matrix [epochs × cache_lines]
    uint64_t irreg_base = 0;       // Base address of irregular (vertex) data region
    uint64_t irreg_bound = 0;      // Upper bound of irregular data region
    uint32_t num_cache_lines = 0;  // Number of cache lines covering vertex data
    uint32_t num_epochs = 256;     // Number of epochs (default 256)
    uint32_t epoch_size = 0;       // Vertices per epoch
    uint32_t sub_epoch_size = 0;   // Vertices per sub-epoch (epoch_size / 128)
    uint32_t current_vertex = 0;   // Current destination vertex being processed
    bool enabled = false;          // Whether P-OPT state has been initialized

    // P-OPT Algorithm 2 (Balaji et al., 2021, Section 4.1):
    // Compute next-reference distance for a cache line.
    //
    // Encoding (from makeOffsetMatrix in popt.h, matching reference llc.cpp):
    //   MSB=0 (bit 7 clear): cache line IS referenced in this epoch
    //     → bits [6:0] = sub-epoch of LAST access within the epoch
    //   MSB=1 (bit 7 set): cache line is NOT referenced in this epoch
    //     → bits [6:0] = distance (in epochs) to next epoch with a reference
    //
    // Returns: rereference distance (0 = accessed soon, higher = farther away)
    uint32_t findNextRef(uint32_t cline_id) const {
        if (!enabled || cline_id >= num_cache_lines) return 127;
        if (epoch_size == 0 || sub_epoch_size == 0) return 127;
        struct PositionCache {
            const POPTState* owner = nullptr;
            uint32_t vertex = UINT32_MAX;
            uint32_t epoch_size = 0;
            uint32_t sub_epoch_size = 0;
            uint32_t num_epochs = 0;
            uint32_t epoch_id = 0;
            uint32_t current_sub_epoch = 0;
        };
        static thread_local PositionCache cache;
        if (cache.owner != this || cache.vertex != current_vertex ||
            cache.epoch_size != epoch_size ||
            cache.sub_epoch_size != sub_epoch_size ||
            cache.num_epochs != num_epochs) {
            cache.owner = this;
            cache.vertex = current_vertex;
            cache.epoch_size = epoch_size;
            cache.sub_epoch_size = sub_epoch_size;
            cache.num_epochs = num_epochs;
            cache.epoch_id = current_vertex / epoch_size;
            cache.current_sub_epoch =
                (current_vertex % epoch_size) / sub_epoch_size;
        }
        const uint32_t epoch_id = cache.epoch_id;
        if (epoch_id >= num_epochs) return 127;

        // Look up rereference matrix entry: matrix is transposed as [epoch][cline]
        uint8_t entry = reref_matrix[epoch_id * num_cache_lines + cline_id];
        constexpr uint8_t OR_MASK = 0x80;   // MSB
        constexpr uint8_t AND_MASK = 0x7F;  // lower 7 bits

        if ((entry & OR_MASK) != 0) {
            // MSB=1: NOT referenced in this epoch — data = distance to next epoch
            uint8_t reref = entry & AND_MASK;
            return reref;
        } else {
            // MSB=0: Referenced in this epoch — data = sub-epoch of last access
            uint8_t lastRefSubEpoch = entry & AND_MASK;
            const uint32_t currSubEpoch = cache.current_sub_epoch;
            if (currSubEpoch <= lastRefSubEpoch) {
                return 0;  // Still will be accessed within this epoch
            } else {
                // Past final access in this epoch — check next epoch
                if (epoch_id + 1 < num_epochs) {
                    uint8_t next_entry = reref_matrix[(epoch_id + 1) * num_cache_lines + cline_id];
                    if ((next_entry & OR_MASK) == 0) {
                        return 1;  // Referenced next epoch
                    } else {
                        uint8_t reref = next_entry & AND_MASK;
                        return (reref < 127) ? reref + 1 : 127;
                    }
                }
                return 127;  // No future reference found (max distance)
            }
        }
    }
};

// ============================================================================
// GRASP State: Degree-aware retention with 3-tier RRIP insertion
// (Faldu et al., 2020; reference implementation: grasp.cpp + common.h)
//
// GRASP uses DBG-reordered vertex data where high-degree vertices are
// placed at the front of the array.  Three reuse tiers:
//   High-reuse:      addr ∈ [data_base, data_base + high_reuse_bound)
//   Moderate-reuse:  addr ∈ [high_reuse_bound, moderate_reuse_bound)
//   Low-reuse:       addr ∉ above ranges
//
// The border between high/moderate is determined by what fraction of
// the LLC capacity should be reserved for high-degree vertices.  The original
// GRASP app instrumentation defaults frontier_frac=50, so PR/BC/Radii traces
// carry propertyA/B-f=50; BellmanFord-style traces may override this to 100.
// ============================================================================
struct GRASPState {
    uint64_t data_base = 0;            // Base address of vertex data array (DBG-ordered)
    uint64_t data_end = 0;             // End address of vertex data array
    uint64_t high_reuse_bound = 0;     // Addresses < this are high-reuse (hot hubs)
    uint64_t moderate_reuse_bound = 0; // Addresses < this (and >= high_reuse_bound) are moderate
    bool enabled = false;

    enum class ReuseTier { HIGH, MODERATE, LOW };

    // Classify an address into one of three reuse tiers
    ReuseTier classify(uint64_t address) const {
        if (!enabled) return ReuseTier::LOW;
        if (address >= data_base && address < high_reuse_bound)
            return ReuseTier::HIGH;
        if (address >= high_reuse_bound && address < moderate_reuse_bound)
            return ReuseTier::MODERATE;
        return ReuseTier::LOW;
    }

    // Initialize from graph properties:
    //   data_ptr: base address of the vertex property array (must be DBG-ordered)
    //   num_vertices: total vertices
    //   elem_size: sizeof each element (e.g., sizeof(float) for PageRank scores)
    //   llc_size: LLC size in bytes
    //   hot_fraction: fraction of LLC to reserve for hot vertices (0.0-1.0, default 0.5)
    void init(uint64_t data_ptr, uint32_t num_vertices, size_t elem_size,
              size_t llc_size, double hot_fraction = 0.5) {
        data_base = data_ptr;
        data_end = data_ptr + num_vertices * elem_size;
        // High-reuse border: hot_fraction of LLC capacity worth of vertex data
        // Align to 64-byte cache line boundary for correct line_addr classification
        uint64_t high_bytes = static_cast<uint64_t>(hot_fraction * llc_size);
        constexpr uint64_t LINE_MASK = ~uint64_t(63);
        high_reuse_bound = (data_base + high_bytes + 63) & LINE_MASK;
        if (high_reuse_bound > data_end) high_reuse_bound = data_end;
        // Moderate-reuse border: 2× the high-reuse region (per reference common.h)
        moderate_reuse_bound = (data_base + 2 * high_bytes + 63) & LINE_MASK;
        if (moderate_reuse_bound > data_end) moderate_reuse_bound = data_end;
        enabled = true;
    }
};

// ============================================================================
// Fast Cache Level - NO LOCKS, uses clock algorithm (faster than LRU)
// Use this for private per-thread caches where no locking is needed
// ============================================================================
class FastCacheLevel {
public:
    FastCacheLevel(const std::string& name, size_t size_bytes, size_t line_size,
                   size_t associativity)
        : name_(name), size_bytes_(size_bytes), line_size_(line_size),
          associativity_(associativity) {
        if (line_size_ == 0 || associativity_ == 0)
            throw std::invalid_argument(
                "cache line size and associativity must be positive");
        if ((line_size_ & (line_size_ - 1)) != 0)
            throw std::invalid_argument(
                "cache line size must be a power of two");
        if (size_bytes_ < line_size_ * associativity_ ||
            size_bytes_ % (line_size_ * associativity_) != 0)
            throw std::invalid_argument(
                "cache size must contain an integral number of sets");
        num_sets_ = size_bytes / (line_size * associativity);
        if ((num_sets_ & (num_sets_ - 1)) != 0)
            throw std::invalid_argument(
                "cache set count must be a power of two");
        set_mask_ = num_sets_ - 1;
        
        offset_bits_ = log2i(line_size);
        
        // Flat arrays for better cache locality
        tags_.resize(num_sets_ * associativity, 0);
        valid_.resize(num_sets_ * associativity, false);
        clock_.resize(num_sets_ * associativity, false);
    }

    // Fast access - no locks, inline everything
    __attribute__((always_inline))
    inline bool access(uint64_t address) {
        stats_.reads++;
        
        uint64_t tag = address >> offset_bits_;
        size_t set_base = (tag & set_mask_) * associativity_;
        
        // Check all ways
        for (size_t i = 0; i < associativity_; i++) {
            size_t idx = set_base + i;
            if (valid_[idx] && tags_[idx] == tag) {
                stats_.hits++;
                clock_[idx] = true;  // Mark recently used
                return true;
            }
        }
        
        stats_.misses++;
        return false;
    }

    // Fast insert using clock algorithm
    __attribute__((always_inline))
    inline void insert(uint64_t address) {
        uint64_t tag = address >> offset_bits_;
        size_t set_base = (tag & set_mask_) * associativity_;
        
        // Find invalid slot first
        for (size_t i = 0; i < associativity_; i++) {
            size_t idx = set_base + i;
            if (!valid_[idx]) {
                tags_[idx] = tag;
                valid_[idx] = true;
                clock_[idx] = true;
                return;
            }
        }
        
        // Clock algorithm: find slot with clock=false
        for (size_t i = 0; i < associativity_; i++) {
            size_t idx = set_base + i;
            if (!clock_[idx]) {
                stats_.evictions++;
                tags_[idx] = tag;
                clock_[idx] = true;
                return;
            }
            clock_[idx] = false;  // Second chance
        }
        
        // All had clock=true, use first
        stats_.evictions++;
        tags_[set_base] = tag;
        clock_[set_base] = true;
    }

    const CacheStats& getStats() const { return stats_; }
    void resetStats() { stats_.reset(); }
    const std::string& getName() const { return name_; }
    size_t getSizeBytes() const { return size_bytes_; }
    size_t getAssociativity() const { return associativity_; }
    size_t getNumSets() const { return num_sets_; }

private:
    static size_t log2i(size_t n) {
        size_t r = 0;
        while (n > 1) { n >>= 1; r++; }
        return r;
    }

    std::string name_;
    size_t size_bytes_;
    size_t line_size_;
    size_t associativity_;
    size_t num_sets_;
    size_t set_mask_;
    size_t offset_bits_;
    
    std::vector<uint64_t> tags_;
    std::vector<bool> valid_;
    std::vector<bool> clock_;
    CacheStats stats_;
};

// ============================================================================
// ULTRA-FAST Cache Level - Packed cache line structure for better locality
// ~2-3x faster than FastCacheLevel through better memory layout
// ============================================================================
class UltraFastCacheLevel {
public:
    // Pack tag + valid + clock into single struct for cache-friendly access
    struct alignas(16) CacheEntry {
        uint64_t tag;
        uint8_t valid;
        uint8_t clock;
        uint8_t pad[6];  // Pad to 16 bytes for alignment
    };

    UltraFastCacheLevel(const std::string& name, size_t size_bytes, size_t line_size,
                        size_t associativity)
        : name_(name), size_bytes_(size_bytes), line_size_(line_size),
          associativity_(associativity), hits_(0), misses_(0), evictions_(0) {
        if (line_size_ == 0 || associativity_ == 0)
            throw std::invalid_argument(
                "cache line size and associativity must be positive");
        if ((line_size_ & (line_size_ - 1)) != 0)
            throw std::invalid_argument(
                "cache line size must be a power of two");
        if (size_bytes_ < line_size_ * associativity_ ||
            size_bytes_ % (line_size_ * associativity_) != 0)
            throw std::invalid_argument(
                "cache size must contain an integral number of sets");
        num_sets_ = size_bytes / (line_size * associativity);
        if ((num_sets_ & (num_sets_ - 1)) != 0)
            throw std::invalid_argument(
                "cache set count must be a power of two");
        set_mask_ = num_sets_ - 1;
        
        offset_bits_ = __builtin_ctzll(line_size);  // Fast log2 for power of 2
        
        // Single contiguous array - all data for a set is together
        entries_.resize(num_sets_ * associativity);
        memset(entries_.data(), 0, entries_.size() * sizeof(CacheEntry));
    }

    // Ultra-fast access with packed structure
    __attribute__((always_inline, hot))
    inline bool access(uint64_t address) {
        const uint64_t tag = address >> offset_bits_;
        CacheEntry* __restrict__ set = &entries_[(tag & set_mask_) * associativity_];
        
        // Unrolled check for 8-way (most common)
        if (__builtin_expect(associativity_ == 8, 1)) {
            #pragma GCC unroll 8
            for (size_t i = 0; i < 8; i++) {
                if (set[i].valid && set[i].tag == tag) {
                    hits_++;
                    set[i].clock = 1;
                    return true;
                }
            }
        } else {
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].valid && set[i].tag == tag) {
                    hits_++;
                    set[i].clock = 1;
                    return true;
                }
            }
        }
        
        misses_++;
        return false;
    }

    // Ultra-fast insert
    __attribute__((always_inline, hot))
    inline void insert(uint64_t address) {
        const uint64_t tag = address >> offset_bits_;
        CacheEntry* __restrict__ set = &entries_[(tag & set_mask_) * associativity_];
        
        // Find invalid slot first
        for (size_t i = 0; i < associativity_; i++) {
            if (!set[i].valid) {
                set[i].tag = tag;
                set[i].valid = 1;
                set[i].clock = 1;
                return;
            }
        }
        
        // Clock algorithm: find slot with clock=0
        for (size_t i = 0; i < associativity_; i++) {
            if (!set[i].clock) {
                evictions_++;
                set[i].tag = tag;
                set[i].clock = 1;
                return;
            }
            set[i].clock = 0;
        }
        
        // All had clock=1, use first
        evictions_++;
        set[0].tag = tag;
        set[0].clock = 1;
    }

    CacheStats getStats() const {
        CacheStats s;
        s.hits = hits_;
        s.misses = misses_;
        s.evictions = evictions_;
        return s;
    }
    
    void resetStats() { hits_ = misses_ = evictions_ = 0; }
    const std::string& getName() const { return name_; }
    size_t getSizeBytes() const { return size_bytes_; }
    size_t getAssociativity() const { return associativity_; }
    size_t getNumSets() const { return num_sets_; }
    uint64_t getHits() const { return hits_; }
    uint64_t getMisses() const { return misses_; }
    double hitRate() const { 
        uint64_t total = hits_ + misses_;
        return total > 0 ? (double)hits_ / total : 0.0;
    }

private:
    std::string name_;
    size_t size_bytes_;
    size_t line_size_;
    size_t associativity_;
    size_t num_sets_;
    size_t set_mask_;
    size_t offset_bits_;
    
    std::vector<CacheEntry> entries_;
    uint64_t hits_;
    uint64_t misses_;
    uint64_t evictions_;
};

// ============================================================================
// Single Cache Level (with locks - for shared caches)
// ============================================================================
class CacheLevel {
public:
    CacheLevel(const std::string& name, size_t size_bytes, size_t line_size,
               size_t associativity, EvictionPolicy policy)
        : name_(name), size_bytes_(size_bytes), line_size_(line_size),
          associativity_(associativity), policy_(policy) {
        if (line_size_ == 0 || associativity_ == 0)
            throw std::invalid_argument(
                "cache line size and associativity must be positive");
        if ((line_size_ & (line_size_ - 1)) != 0)
            throw std::invalid_argument(
                "cache line size must be a power of two");
        if (size_bytes_ < line_size_ * associativity_ ||
            size_bytes_ % (line_size_ * associativity_) != 0)
            throw std::invalid_argument(
                "cache size must contain an integral number of sets");
        if ((policy_ == EvictionPolicy::POPT ||
             policy_ == EvictionPolicy::ECG) &&
            associativity_ > 64) {
            throw std::invalid_argument(
                "P-OPT and ECG support at most 64 cache ways");
        }
        num_sets_ = size_bytes / (line_size * associativity);
        power_of_two_sets_ =
            (num_sets_ & (num_sets_ - 1)) == 0;
        
        // Calculate bit widths
        offset_bits_ = log2i(line_size);
        index_bits_ = log2i(num_sets_);
        
        // Initialize cache structure
        cache_.resize(num_sets_);
        for (auto& set : cache_) {
            set.resize(associativity);
        }
        
        // Initialize random generator for RANDOM policy
        rng_.seed(42);
        if (policy_ == EvictionPolicy::HAWKEYE) {
            if (name_ != "L3" && name_ != "L3-Shared") {
                throw std::invalid_argument(
                    "Hawkeye proxy is an LLC-only replacement policy");
            }
            hawkeye_state_ = std::make_unique<hawkeye_policy::State>(
                num_sets_, static_cast<uint16_t>(associativity_));
        }
    }

    // Access cache (returns true on hit, false on miss)
    bool access(uint64_t address, bool is_write) {
        std::lock_guard<std::mutex> lock(mutex_);

        // T-OPT: record the L3 input stream (post-L1/L2). Only the LLC level.
        if (topt::enabled && (name_ == "L3" || name_ == "L3-Shared")) {
            topt::capture_geom(
                offset_bits_, num_sets_, (uint32_t)associativity_);
            bool is_prop = graph_ctx_ && graph_ctx_->isPropertyData(address);
            topt::record(address, is_prop, is_write);
        }

        if (is_write) {
            stats_.writes++;
        } else {
            stats_.reads++;
        }
        
        uint64_t tag = getTag(address);
        size_t set_idx = getSetIndex(address);
        auto& set = cache_[set_idx];
        
        // Check for hit
        for (size_t i = 0; i < associativity_; i++) {
            if (set[i].valid && set[i].tag == tag) {
                // Hit!
                stats_.hits++;
                if (graph_ctx_ && graph_ctx_->findRegion(address)) stats_.prop_hits++;
                if (isGovernedProperty(address))
                    ++governed_property_hits_;
                if (isRef32Governed(address))
                    ++ref32_governed_hits_;
                updateOnHit(set, i, set_idx);
                if (is_write) {
                    set[i].dirty = true;
                }
                recordAdmissionAccess(set_idx, false);
                return true;
            }
        }
        
        // Miss
        stats_.misses++;
        if (graph_ctx_ && graph_ctx_->findRegion(address)) stats_.prop_misses++;
        if (isGovernedProperty(address))
            ++governed_property_misses_;
        if (isRef32Governed(address))
            ++ref32_governed_misses_;
        recordAdmissionAccess(set_idx, true);
        return false;
    }

    // ECG_REUSE_PLAN_DEPTH: copy the per-thread schedule hint onto a line at fill/refresh.
    // No-op (clears to n=0) when no schedule is delivered, so the single-epoch path is
    // byte-identical to before. Mirrors the ecg_epoch stamp, kept next to it at every site.
    inline void stampEpochSchedule(CacheLine& L) {
        if (!graph_ctx_) { L.ecg_epoch_sched_n = 0; return; }
        const auto& H = graph_ctx_->hints_for_thread();
        uint8_t kn = H.edge_epoch_sched_n;
        if (kn > CacheLine::ECG_REUSE_PLAN_DEPTHMAX) kn = CacheLine::ECG_REUSE_PLAN_DEPTHMAX;
        L.ecg_epoch_sched_n = kn;
        for (uint8_t k = 0; k < CacheLine::ECG_REUSE_PLAN_DEPTHMAX; ++k)
            L.ecg_epoch_sched[k] = (k < kn) ? H.edge_epoch_sched[k] : 0;
        if (ecgTierCarried() && H.edge_grasp_tier_valid)
            L.ecg_dbg_tier = H.edge_grasp_tier;
    }

    // ECG_EXACT_STORED: refresh a resident line's stamped prediction on a demand
    // access EVEN IF this level didn't serve it. Models the per-edge mask hint
    // being broadcast to the LLC on every edge load (the ecg.extract instruction
    // emits a hint per edge), keeping the stamp fresh despite L1/L2 filtering.
    // No-op unless ECG_EXACT_STORED and the line is resident here.
    void refreshExactStamp(uint64_t address) {
        if (policy_ != EvictionPolicy::ECG || !graph_ctx_) return;
        ECGMode mode = graph_ctx_->mask_config.enabled
            ? graph_ctx_->mask_config.ecg_mode : ECGMode::DBG_PRIMARY;
        if (mode != ECGMode::ECG_EXACT_STORED &&
            mode != ECGMode::ECG_GRASP_POPT) {
            return;
        }
        if (mode == ECGMode::ECG_GRASP_POPT &&
            !graph_ctx_->isEcgEpochData(address)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t tag = getTag(address);
        size_t set_idx = getSetIndex(address);
        auto& set = cache_[set_idx];
        for (size_t i = 0; i < associativity_; i++) {
            if (set[i].valid && set[i].tag == tag) {
                if (mode == ECGMode::ECG_EXACT_STORED) {
                    if (nextUseLruEnabled())
                        stampQuantizedNextUse(set[i]);
                    else
                        set[i].ecg_exact_pred =
                            computeExactPredForStamp(set[i].line_addr);
                }
                else if (graph_ctx_->hints_for_thread().edge_epoch_valid) {
                    // ECG_GRASP_POPT: refresh the stored epoch only on a real delivery
                    set[i].ecg_epoch = graph_ctx_->hints_for_thread().edge_epoch;
                    set[i].ecg_epoch_valid = true;
                    stampEpochSchedule(set[i]);
                }
                return;
            }
        }
    }

    // Non-counting presence check: returns true if the line is resident WITHOUT
    // touching demand hit/miss stats or replacement state. Used by prefetch() to
    // probe each level — a prefetch probe must NOT register as a demand access
    // (otherwise an avoided demand miss is cancelled by the probe miss, making
    // prefetch a no-op for the miss rate).
    bool contains(uint64_t address) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t tag = getTag(address);
        size_t set_idx = getSetIndex(address);
        auto& set = cache_[set_idx];
        for (size_t i = 0; i < associativity_; i++) {
            if (set[i].valid && set[i].tag == tag) return true;
        }
        return false;
    }

    bool canAdmitRef32Prefetch(
            uint64_t address, uint64_t current_sequence) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& set = cache_[getSetIndex(address)];
        for (const auto& line : set) {
            if (!line.valid)
                return true;
        }
        if (policy_ == EvictionPolicy::LRU)
            return true;
        if (!ref32Enabled())
            return false;
        for (const auto& line : set) {
            if (line.ecg_ref32_state == ecg_ref32::State::DEAD ||
                !graph_ctx_->isEcgEpochData(line.line_addr)) {
                return true;
            }
            const ecg_ref32::EffectiveFuture future =
                graph_ctx_->ref32_exact_diagnostic
                ? ecg_ref32::resolveExactFuture(
                    line.ecg_ref32_state,
                    line.ecg_ref32_exact_deadline,
                    current_sequence)
                : ecg_ref32::resolveQuantizedFuture(
                    line.ecg_ref32_state,
                    line.ecg_ref32_deadline,
                    current_sequence,
                    configuredRef32DeadlineBits());
            const uint8_t score =
                future.state == ecg_ref32::State::FINITE
                ? ecg_ref32::distanceRRPV(future.remaining)
                : std::max<uint8_t>(
                    line.rrpv,
                    ecg_policy::graspTierRRPV(
                        graph_ctx_->classifyGRASP(
                            line.line_addr, size_bytes_), 7));
            if (score >= 7)
                return true;
        }
        return false;
    }

    Ref32UpdateResult applyRef32CommitUpdate(
            uint64_t address, ecg_ref32::State state,
            uint32_t quantized_deadline, uint64_t exact_deadline,
            uint64_t current_sequence) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isRef32Governed(address))
            return Ref32UpdateResult::NOT_RESIDENT;
        const uint64_t tag = getTag(address);
        auto& set = cache_[getSetIndex(address)];
        for (size_t way = 0; way < associativity_; ++way) {
            if (!set[way].valid || set[way].tag != tag)
                continue;
            ecg_ref32::EffectiveFuture future;
            if (state == ecg_ref32::State::FINITE) {
                future = graph_ctx_->ref32_exact_diagnostic
                    ? ecg_ref32::resolveExactFuture(
                        state, exact_deadline, current_sequence)
                    : ecg_ref32::resolveQuantizedFuture(
                        state, quantized_deadline, current_sequence,
                        configuredRef32DeadlineBits());
                if (future.state != ecg_ref32::State::FINITE)
                    return Ref32UpdateResult::EXPIRED;
            }
            set[way].ecg_ref32_state = state;
            set[way].ecg_ref32_deadline = quantized_deadline;
            set[way].ecg_ref32_exact_deadline = exact_deadline;
            set[way].rrpv = state == ecg_ref32::State::DEAD
                ? 7 : ecg_ref32::distanceRRPV(future.remaining);
            return Ref32UpdateResult::APPLIED;
        }
        return Ref32UpdateResult::NOT_RESIDENT;
    }

    void updatePrefetchHit(uint64_t address) {
        if (policy_ != EvictionPolicy::HAWKEYE || !hawkeye_state_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t tag = getTag(address);
        const size_t set_idx = getSetIndex(address);
        auto& set = cache_[set_idx];
        for (size_t way = 0; way < associativity_; ++way) {
            if (!set[way].valid || set[way].tag != tag) continue;
            const uint64_t signature = currentHawkeyeSite();
            const bool friendly = hawkeye_state_->access(
                set_idx, set[way].line_addr >> offset_bits_,
                signature, true);
            set[way].hawkeye_signature = signature;
            set[way].hawkeye_prefetch = true;
            set[way].rrpv = hawkeye_policy::insertionRrpv(friendly);
            return;
        }
    }

    // Insert a line after a miss (called when lower level provides data)
    void insert(
        uint64_t address, bool is_write, bool is_prefetch = false) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!is_prefetch && shouldBypassRef32(address)) {
            ++ref32_dead_bypasses_;
            return;
        }
        
        uint64_t tag = getTag(address);
        size_t set_idx = getSetIndex(address);
        auto& set = cache_[set_idx];
        if (policy_ == EvictionPolicy::ECG && set_dueling_ && graph_ctx_ &&
            graph_ctx_->hints_for_thread().current_src != UINT32_MAX) {
            const size_t sample_set = duelingSampleSetIndex(set_idx);
            const ecg_policy::MissRecordEvent event =
                dueling_selector_.recordMiss(sample_set);
            const int leader = ecg_policy::duelingLeaderArm(sample_set);
            if (event.leader_sample && leader >= 0)
                ++dueling_leader_samples_by_arm_[
                    static_cast<size_t>(leader)];
            if (event.completed_window) {
                ++dueling_winner_windows_by_arm_[event.winner_after];
                ++dueling_completed_windows_;
            }
            if (event.winner_changed) ++dueling_winner_changes_;
        }
        
        // Find victim
        evicting_set_idx_ = set_idx;   // for set-dueling arm selection
        size_t victim_idx = findVictim(set);

        // PIN bypass: all ways pinned, do not insert (miss already counted).
        if (victim_idx == SIZE_MAX) return;

        // Evict if necessary
        if (set[victim_idx].valid) {
            stats_.evictions++;
            if (is_prefetch) {
                if (set[victim_idx].ecg_ref32_prefetch)
                    ++ref32_prefetch_evictions_;
                else
                    ++ref32_prefetch_demand_displacements_;
            }
            if (set[victim_idx].dirty) {
                stats_.writebacks++;
            }
        }
        
        // Insert new line
        set[victim_idx].tag = tag;
        set[victim_idx].valid = true;
        set[victim_idx].dirty = is_write;
        set[victim_idx].last_access = global_time_++;
        set[victim_idx].insert_time = global_time_;
        set[victim_idx].access_count = 1;
        set[victim_idx].rrpv = 2;  // For SRRIP: long re-reference (M-1 = 2, per Jaleel ISCA'10)
        set[victim_idx].line_addr = address & ~(uint64_t(line_size_ - 1));  // Store line-aligned address
        set[victim_idx].ecg_next_use = 0;
        set[victim_idx].ecg_future_state =
            ecg_policy::FutureState::UNKNOWN;
        set[victim_idx].ecg_ref32_deadline = 0;
        set[victim_idx].ecg_ref32_exact_deadline = 0;
        set[victim_idx].ecg_ref32_state = ecg_ref32::State::UNKNOWN;
        set[victim_idx].ecg_ref32_prefetch = is_prefetch;

        if (policy_ == EvictionPolicy::HAWKEYE && hawkeye_state_) {
            const uint64_t signature = currentHawkeyeSite();
            const bool friendly = hawkeye_state_->access(
                set_idx, address >> offset_bits_, signature, is_prefetch);
            set[victim_idx].hawkeye_signature = signature;
            set[victim_idx].hawkeye_prefetch = is_prefetch;
            set[victim_idx].rrpv = hawkeye_policy::insertionRrpv(friendly);
            if (friendly) {
                bool has_six = false;
                for (size_t way = 0; way < associativity_; ++way) {
                    if (set[way].valid &&
                        set[way].rrpv == hawkeye_policy::kMaxRrpv - 1) {
                        has_six = true;
                        break;
                    }
                }
                if (!has_six) {
                    for (size_t way = 0; way < associativity_; ++way) {
                        if (way != victim_idx && set[way].valid &&
                            set[way].rrpv < hawkeye_policy::kMaxRrpv - 1) {
                            ++set[way].rrpv;
                        }
                    }
                }
                set[victim_idx].rrpv = 0;
            }
        }

        // GRASP: 3-tier RRIP insertion matching Faldu et al. (2020).
        // HIGH reuse (hubs):    RRPV = 1 (P_RRIP — protect in cache)
        // MODERATE reuse:       RRPV = 6 (I_RRIP — intermediate)
        // LOW reuse (cold/OOB): RRPV = 7 (M_RRIP — evict sooner)
        //
        // Hot boundary: first hot_fraction (10%) of LLC capacity within
        // each property region. Moderate = next 10%. Cold = rest.
        // After DBG reorder, highest-degree vertices are at front (low addr).
        if (policy_ == EvictionPolicy::GRASP) {
            constexpr uint8_t P_RRIP = 1;   // Priority insertion (hot), matching upstream GRASP
            constexpr uint8_t I_RRIP = 6;   // Intermediate (moderate)
            constexpr uint8_t M_RRIP = 7;   // Max (cold)
            if (graph_ctx_) {
                uint32_t tier = graph_ctx_->classifyGRASP(address, size_bytes_);
                if (tier == 1)       set[victim_idx].rrpv = P_RRIP;
                else if (tier == 2)  set[victim_idx].rrpv = I_RRIP;
                else                 set[victim_idx].rrpv = M_RRIP;
            } else if (grasp_state_.enabled) {
                auto t = grasp_state_.classify(address);
                if (t == GRASPState::ReuseTier::HIGH)           set[victim_idx].rrpv = P_RRIP;
                else if (t == GRASPState::ReuseTier::MODERATE)  set[victim_idx].rrpv = I_RRIP;
                else                                            set[victim_idx].rrpv = M_RRIP;
            }
        }

        // P-OPT: insert with SRRIP-style RRPV (long re-reference = M-1)
        if (policy_ == EvictionPolicy::POPT) {
            set[victim_idx].rrpv = 6;  // M_RRPV - 1 = long re-reference (SRRIP default)
        }

        // PIN: set pin bit when newly inserted line falls in the high-reuse
        // region (Faldu et al., 2020 PIN baseline; mirrors upstream pin.cpp).
        if (policy_ == EvictionPolicy::PIN) {
            set[victim_idx].pin = false;
            if (graph_ctx_ && graph_ctx_->num_regions > 0) {
                if (graph_ctx_->classifyGRASP(address, size_bytes_) == 1) {
                    set[victim_idx].pin = true;
                }
            }
        }

        // ECG: Mode-dependent insertion RRPV.
        // DBG_ONLY / DBG_PRIMARY / ECG_EMBEDDED variants: use GRASP 3-tier (1/6/7)
        // POPT_PRIMARY: use P-OPT-style RRPV=6 (matches pure P-OPT aging)
        if (policy_ == EvictionPolicy::ECG) {
            ECGMode mode = (graph_ctx_ && graph_ctx_->mask_config.enabled)
                ? graph_ctx_->mask_config.ecg_mode : ECGMode::DBG_PRIMARY;
            const AccessHints* access_hints =
                graph_ctx_ ? &graph_ctx_->hints_for_thread() : nullptr;
            // A record configured with zero tier bits cannot carry a tier.
            // Gating here, at the single point every insertion path consults,
            // makes ECG_RECORD_TIER_BITS=0 a genuine mechanism ablation instead
            // of a width-only change. Without it the record narrowed while the
            // tier kept arriving and kept breaking eviction ties, so "the tier
            // bits are free to drop" was never actually tested. Note the else
            // branches fall back to address-based GRASP classification, so this
            // ablates the CARRIED tier, not GRASP tiering as a whole.
            const bool has_carried_tier =
                ecgTierCarried() &&
                mode == ECGMode::ECG_GRASP_POPT &&
                graph_ctx_ && access_hints &&
                graph_ctx_->isEcgEpochData(address) &&
                access_hints->edge_grasp_tier_valid;

            if (mode == ECGMode::ECG_REF32) {
                if (graph_ctx_) {
                    set[victim_idx].ecg_dbg_tier =
                        static_cast<uint8_t>(graph_ctx_->classifyGRASP(
                            address, size_bytes_));
                    if (is_prefetch) {
                        set[victim_idx].rrpv = 7;
                    } else {
                        set[victim_idx].rrpv =
                            ecg_policy::graspTierRRPV(
                                set[victim_idx].ecg_dbg_tier, 7);
                        stampRef32(set[victim_idx]);
                    }
                }
            } else if (mode == ECGMode::ECG_EXACT_MASK) {
                // Precomputed exact 5-bit next-ref carried on the demand (per-edge
                // mask POPT field, bits [26:33]). Map near->keep (low RRPV),
                // far->evict (high RRPV). Set fresh at every access; eviction is
                // plain RRIP — no eviction-time recompute, no graph query.
                uint8_t popt5 = static_cast<uint8_t>(
                    (graph_ctx_->hints_for_thread().mask >> 26) & 0x1F);
                uint8_t rmax = graph_ctx_->mask_config.rrpv_max;
                set[victim_idx].rrpv = static_cast<uint8_t>((uint32_t(popt5) * rmax) / 31u);
                set[victim_idx].ecg_popt_hint = popt5;
            } else if (mode == ECGMode::POPT_PRIMARY || mode == ECGMode::ECG_EXACT
                || mode == ECGMode::ECG_EXACT_STORED) {
                // Match P-OPT insertion: uniform RRPV=6 for all lines
                set[victim_idx].rrpv = 6;
            } else if (mode == ECGMode::ECG_COMBINED) {
                uint32_t tier = 3;
                uint32_t hint15 = 0;
                if (graph_ctx_ && graph_ctx_->mask_array.enabled) {
                    tier = graph_ctx_->classifyGRASP(address, size_bytes_);
                    uint32_t mask_entry = graph_ctx_->hints_for_thread().mask;
                    const uint32_t hint =
                        graph_ctx_->mask_config.decodePOPT(mask_entry);
                    const uint32_t popt_max =
                        graph_ctx_->mask_config.popt_bits > 0
                        ? ((1 << graph_ctx_->mask_config.popt_bits) - 1) : 1;
                    hint15 = (hint * 15u) / std::max<uint32_t>(1, popt_max);
                } else if (graph_ctx_) {
                    tier = graph_ctx_->classifyGRASP(address, size_bytes_);
                    hint15 = std::min<uint32_t>(
                        graph_ctx_->findNextRef(address), 127u) >> 3;
                }
                set[victim_idx].rrpv =
                    ecg_policy::combinedInsertionRRPV(
                        tier, hint15, 15, 7);
            } else if (
                    futureAdmissionForSet(evicting_set_idx_) &&
                    mode == ECGMode::ECG_GRASP_POPT && graph_ctx_ &&
                    access_hints &&
                    graph_ctx_->isEcgEpochData(address) &&
                    access_hints->edge_epoch_valid &&
                    access_hints->current_src != UINT32_MAX) {
                const uint32_t tier = has_carried_tier
                    ? access_hints->edge_grasp_tier
                    : graph_ctx_->classifyGRASP(address, size_bytes_);
                set[victim_idx].rrpv = combinedReuseAdmissionEnabled()
                    ? ecg_policy::combinedReuseAdmissionRRPV(
                        tier, deliveredFirstReuseEpoch(*access_hints),
                        currentReuseEpoch(),
                        graph_ctx_->edge_epoch_count, 7)
                    : ecg_policy::reuseAdmissionRRPV(
                        deliveredFirstReuseEpoch(*access_hints),
                        currentReuseEpoch(),
                        graph_ctx_->edge_epoch_count, 7);
                ++reuse_admission_updates_;
            } else {
                // GRASP 3-tier insertion for DBG_PRIMARY/DBG_ONLY/ECG_EMBEDDED/
                // ECG_GRASP_POPT. TWO VARIANTS (mask_config.grasp_tier_source):
                //   MASK (0, our ECG): tier DELIVERED in the per-edge mask
                //     (decodeDBG of the current hint) — cross-sim identical.
                //   REGION (1, original GRASP): classifyGRASP(address) recomputed
                //     from the property region (Faldu spatial top-fraction).
                // Both map through the shared ecg_policy::graspTierRRPV (1/6/7).
                if (graph_ctx_) {
                    uint32_t tier;
                    if (has_carried_tier) {
                        tier = access_hints->edge_grasp_tier;
                    } else if (graph_ctx_->mask_config.grasp_tier_source == 0) {  // MASK (ECG)
                        // The DELIVERED per-vertex GRASP tier, keyed by the INSERTED
                        // LINE's own vertex, BYTE-EXACT to the region variant.
                        tier = graph_ctx_->maskGraspTier(address);
                    } else {                                               // REGION (GRASP)
                        tier = graph_ctx_->classifyGRASP(address, size_bytes_);
                    }
                    set[victim_idx].rrpv = ecg_policy::graspTierRRPV(
                        static_cast<uint32_t>(tier), 7);
                }
            }

            // Store ECG mask fields for eviction tiebreaking
            if (mode == ECGMode::ECG_REF32 && graph_ctx_) {
                set[victim_idx].ecg_dbg_tier =
                    static_cast<uint8_t>(graph_ctx_->classifyGRASP(
                        address, size_bytes_));
                set[victim_idx].ecg_epoch_valid = false;
                set[victim_idx].ecg_epoch_sched_n = 0;
            } else if (graph_ctx_ && graph_ctx_->mask_array.enabled) {
                uint32_t mask_entry = graph_ctx_->hints_for_thread().mask;
                set[victim_idx].ecg_dbg_tier =
                    (mode == ECGMode::ECG_GRASP_POPT)
                        ? static_cast<uint8_t>(has_carried_tier
                              ? access_hints->edge_grasp_tier
                              : graph_ctx_->classifyGRASP(address, size_bytes_))
                        : graph_ctx_->mask_config.decodeDBG(mask_entry);
                set[victim_idx].ecg_epoch_valid = false;  // reset; set true only on a real delivery
                set[victim_idx].ecg_epoch_sched_n = 0;
                if (mode == ECGMode::ECG_EXACT_MASK) {
                    // already set ecg_popt_hint from the fixed per-edge layout above
                } else if (mode == ECGMode::ECG_GRASP_POPT &&
                           graph_ctx_->isEcgEpochData(address)) {
                    // per-edge mask carries the ABSOLUTE next-ref epoch (untruncated).
                    // Stamp validity from the delivery flag: a cleared/sequential read
                    // (clearEdgeEpoch -> valid=false) fills an UNSTAMPED line. This brings
                    // the CLEARED-read case into line with gem5/Sniper (which stamp only on
                    // real per-edge delivery). NOTE: never-delivered fills (PR init, before
                    // any vertex hint) keep the legacy stamped default (edge_epoch_valid=true)
                    // — a cache_sim functional-authority convention, NOT bit-identical to
                    // gem5/Sniper which reset such fills to unstamped.
                    set[victim_idx].ecg_epoch = graph_ctx_->hints_for_thread().edge_epoch;
                    set[victim_idx].ecg_epoch_valid =
                        graph_ctx_->hints_for_thread().edge_epoch_valid;
                    stampEpochSchedule(set[victim_idx]);
                } else {
                    set[victim_idx].ecg_popt_hint = graph_ctx_->mask_config.decodePOPT(mask_entry);
                }
            } else if (graph_ctx_) {
                if (has_carried_tier) {
                    set[victim_idx].ecg_dbg_tier =
                        access_hints->edge_grasp_tier;
                } else {
                    uint32_t bucket = graph_ctx_->classifyBucket(address);
                    set[victim_idx].ecg_dbg_tier =
                        (bucket < 11) ? static_cast<uint8_t>(bucket) : 0;
                }
                // Compute live P-OPT hint if matrix available
                if (graph_ctx_->rereference.matrix) {
                    uint32_t dist = graph_ctx_->findNextRef(address);
                    // Quantize 0-127 distance to 4-bit (0-15)
                    set[victim_idx].ecg_popt_hint = static_cast<uint8_t>(
                        std::min(dist, uint32_t(127)) >> 3);
                } else {
                    set[victim_idx].ecg_popt_hint = 0;
                }
            }

            // ECG_EXACT_STORED: stamp the exact next-reference NOW (at access /
            // fill), modeling a precomputed per-edge mask. The value is computed
            // at the CONSUMING position (current_src), exactly what an offline
            // per-edge table would hold for this edge — so eviction reads a
            // STORED hint, never recomputing. This isolates the only difference
            // from live ECG_EXACT: staleness (stamp at last access vs at evict).
            if (mode == ECGMode::ECG_EXACT_STORED) {
                if (nextUseLruEnabled())
                    stampQuantizedNextUse(set[victim_idx]);
                else
                    set[victim_idx].ecg_exact_pred =
                        computeExactPredForStamp(set[victim_idx].line_addr);
            }
        }
    }

    const CacheStats& getStats() const { return stats_; }
    void resetStats() {
        stats_.reset();
        reuse_admission_updates_ = 0;
        ref32_governed_hits_ = 0;
        ref32_governed_misses_ = 0;
        governed_property_hits_ = 0;
        governed_property_misses_ = 0;
        ref32_dead_bypasses_ = 0;
        ref32_dead_victims_ = 0;
        ref32_non_property_victims_ = 0;
        ref32_unknown_victims_ = 0;
        ref32_finite_victims_ = 0;
        ref32_prefetch_demand_displacements_ = 0;
        ref32_prefetch_evictions_ = 0;
        admission_selector_.reset();
        admission_follower_selections_.fill(0);
        admission_completed_windows_ = 0;
        admission_winner_changes_ = 0;
    }
    
    const std::string& getName() const { return name_; }
    size_t getSizeBytes() const { return size_bytes_; }
    size_t getLineSize() const { return line_size_; }
    size_t getAssociativity() const { return associativity_; }
    size_t getNumSets() const { return num_sets_; }
    EvictionPolicy getPolicy() const { return policy_; }
    std::string getEcgMode() const {
        if (policy_ != EvictionPolicy::ECG) return "";
        return ECGModeToString(ecg_mode_snapshot_);
    }
    uint32_t getDuelingSetOffset() const { return dueling_set_offset_; }
    uint8_t getDuelingWinnerArm() const {
        return dueling_selector_.winnerArm();
    }
    uint64_t getDuelingCompletedWindows() const {
        return dueling_completed_windows_;
    }
    uint64_t getDuelingWinnerChanges() const {
        return dueling_winner_changes_;
    }
    uint64_t getReuseAdmissionUpdates() const {
        return reuse_admission_updates_;
    }
    uint64_t getRef32GovernedHits() const {
        return ref32_governed_hits_;
    }
    uint64_t getRef32GovernedMisses() const {
        return ref32_governed_misses_;
    }
    uint64_t getGovernedPropertyHits() const {
        return governed_property_hits_;
    }
    uint64_t getGovernedPropertyMisses() const {
        return governed_property_misses_;
    }
    uint64_t getRef32DeadBypasses() const {
        return ref32_dead_bypasses_;
    }
    uint64_t getRef32DeadVictims() const {
        return ref32_dead_victims_;
    }
    uint64_t getRef32NonPropertyVictims() const {
        return ref32_non_property_victims_;
    }
    uint64_t getRef32UnknownVictims() const {
        return ref32_unknown_victims_;
    }
    uint64_t getRef32FiniteVictims() const {
        return ref32_finite_victims_;
    }
    uint64_t getRef32PrefetchDemandDisplacements() const {
        return ref32_prefetch_demand_displacements_;
    }
    uint64_t getRef32PrefetchEvictions() const {
        return ref32_prefetch_evictions_;
    }
    uint64_t getAdmissionLeaderAccesses(uint8_t arm) const {
        return admission_selector_.totalAccesses(arm);
    }
    uint64_t getAdmissionLeaderMisses(uint8_t arm) const {
        return admission_selector_.totalMisses(arm);
    }
    uint64_t getAdmissionFollowerSelections(uint8_t arm) const {
        return arm < ecg_policy::ADMIT_ARM_COUNT
            ? admission_follower_selections_[arm] : 0;
    }
    uint64_t getAdmissionCompletedWindows() const {
        return admission_completed_windows_;
    }
    uint64_t getAdmissionWinnerChanges() const {
        return admission_winner_changes_;
    }
    uint8_t getAdmissionWinnerArm() const {
        return admission_selector_.winnerArm();
    }
    uint32_t getAdmissionSetOffset() const {
        return admission_selector_.offset();
    }
    const std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>&
    getDuelingLeaderSamplesByArm() const {
        return dueling_leader_samples_by_arm_;
    }
    const std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>&
    getDuelingWinnerWindowsByArm() const {
        return dueling_winner_windows_by_arm_;
    }
    const std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>&
    getDuelingFollowerSelectionsByArm() const {
        return dueling_follower_selections_by_arm_;
    }

    // ================================================================
    // P-OPT initialization: call once after building the rereference
    // matrix with makeOffsetMatrix() from popt.h
    // ================================================================
    void initPOPT(const uint8_t* reref_matrix, uint64_t irreg_base,
                  uint64_t irreg_bound, uint32_t num_vertices,
                  uint32_t num_epochs = 256) {
        uint32_t vtx_per_line = static_cast<uint32_t>(line_size_ / sizeof(float));
        if (vtx_per_line == 0) vtx_per_line = 1;
        popt_state_.reref_matrix = reref_matrix;
        popt_state_.irreg_base = irreg_base;
        popt_state_.irreg_bound = irreg_bound;
        popt_state_.num_cache_lines = (num_vertices + vtx_per_line - 1) / vtx_per_line;
        popt_state_.num_epochs = num_epochs;
        popt_state_.epoch_size = (num_vertices + num_epochs - 1) / num_epochs;
        popt_state_.sub_epoch_size = (popt_state_.epoch_size + 127) / 128;
        popt_state_.current_vertex = 0;
        popt_state_.enabled = true;
    }

    // ================================================================
    // GRASP initialization: call once with vertex data address range.
    // Requires DBG-reordered graph (hot vertices at low addresses).
    //   data_ptr: base address of vertex property array
    //   num_vertices: total vertices
    //   elem_size: sizeof each element (e.g. sizeof(float))
    //   llc_size: LLC size in bytes (for computing border regions)
    //   hot_fraction: fraction of LLC reserved for hot vertices (0.0-1.0)
    // ================================================================
    void initGRASP(uint64_t data_ptr, uint32_t num_vertices,
                   size_t elem_size, size_t llc_size,
                   double hot_fraction = 0.5) {
        grasp_state_.init(data_ptr, num_vertices, elem_size, llc_size, hot_fraction);
    }

    // Update current vertex for P-OPT (call at each outer-loop iteration)
    void setCurrentVertex(uint32_t vertex_id) {
        popt_state_.current_vertex = vertex_id;
        // Update unified context (thread-safe via per-thread hints)
        if (graph_ctx_) {
            const_cast<GraphCacheContext*>(graph_ctx_)->setCurrentVertices(vertex_id, vertex_id);
        }
    }

    // ================================================================
    // Unified GraphCacheContext: preferred over legacy init methods.
    // Replaces initPOPT() and initGRASP() with a single context.
    // ================================================================
    void initGraphContext(const GraphCacheContext* ctx) {
        graph_ctx_ = ctx;
        if (ctx) ecg_mode_snapshot_ = ctx->mask_config.ecg_mode;
    }

    // Test hook (NOT used in the simulation path): run the policy's victim
    // selection on a caller-supplied set with controlled CacheLine state, so a
    // unit test can assert the EXACT victim per policy / ECG_VARIANT against an
    // independently hand-computed answer. See bench/src_sim/test_ecg_victim.cc.
    size_t selectVictimForTest(std::vector<CacheLine>& set) { return findVictim(set); }
    size_t setIndexForAddress(uint64_t address) const {
        return getSetIndex(address);
    }

private:
    static size_t log2i(size_t n) {
        size_t r = 0;
        while (n > 1) { n >>= 1; r++; }
        return r;
    }

    uint64_t getTag(uint64_t address) const {
        const uint64_t line = address >> offset_bits_;
        return power_of_two_sets_
            ? address >> (offset_bits_ + index_bits_)
            : line / num_sets_;
    }

    size_t getSetIndex(uint64_t address) const {
        const uint64_t line = address >> offset_bits_;
        return power_of_two_sets_
            ? line & (num_sets_ - 1)
            : static_cast<size_t>(line % num_sets_);
    }

    void updateOnHit(
        std::vector<CacheLine>& set, size_t idx, size_t set_idx) {
        set[idx].last_access = global_time_++;
        set[idx].access_count++;
        set[idx].ecg_ref32_prefetch = false;
        
        // SRRIP: reset RRPV to 0 on hit
        if (policy_ == EvictionPolicy::SRRIP) {
            set[idx].rrpv = 0;
        }

        if (policy_ == EvictionPolicy::HAWKEYE && hawkeye_state_) {
            const uint64_t signature = currentHawkeyeSite();
            const bool friendly = hawkeye_state_->access(
                set_idx, set[idx].line_addr >> offset_bits_,
                signature, false);
            set[idx].hawkeye_signature = signature;
            set[idx].hawkeye_prefetch = false;
            set[idx].rrpv = hawkeye_policy::insertionRrpv(friendly);
        }

        // GRASP: 3-tier hit promotion (Faldu et al., 2020)
        // Hot region → RRPV=0 (aggressive reset), others → decrement by 1
        if (policy_ == EvictionPolicy::GRASP) {
            uint64_t addr = set[idx].line_addr;
            if (graph_ctx_) {
                uint32_t tier = graph_ctx_->classifyGRASP(addr, size_bytes_);
                if (tier == 1) {
                    set[idx].rrpv = 0;  // Hot (hub): aggressive reset
                } else if (set[idx].rrpv > 0) {
                    set[idx].rrpv--;    // Others: gradual decrement
                }
            } else if (grasp_state_.enabled) {
                auto t = grasp_state_.classify(addr);
                if (t == GRASPState::ReuseTier::HIGH) set[idx].rrpv = 0;
                else if (set[idx].rrpv > 0) set[idx].rrpv--;
            }
        }

        // P-OPT: reset RRPV to 0 on hit (same as SRRIP hit promotion)
        if (policy_ == EvictionPolicy::POPT) {
            set[idx].rrpv = 0;
        }

        // ECG: Mode-dependent hit promotion
        if (policy_ == EvictionPolicy::ECG) {
            ECGMode mode = (graph_ctx_ && graph_ctx_->mask_config.enabled)
                ? graph_ctx_->mask_config.ecg_mode : ECGMode::DBG_PRIMARY;

            if (mode == ECGMode::ECG_REF32) {
                if (graph_ctx_ &&
                    graph_ctx_->isEcgEpochData(set[idx].line_addr) &&
                    graph_ctx_->hints_for_thread().edge_ref_valid) {
                    stampRef32(set[idx]);
                }
            } else if (mode == ECGMode::ECG_EXACT_MASK) {
                // Re-apply the FRESH per-edge 5-bit at this re-reference (each edge
                // carries its own hint), so RRPV tracks the current next-ref —
                // this freshness is what the eviction-time recompute gave for free.
                uint8_t popt5 = static_cast<uint8_t>(
                    (graph_ctx_->hints_for_thread().mask >> 26) & 0x1F);
                uint8_t rmax = graph_ctx_->mask_config.rrpv_max;
                set[idx].rrpv = static_cast<uint8_t>((uint32_t(popt5) * rmax) / 31u);
                set[idx].ecg_popt_hint = popt5;
            } else if (mode == ECGMode::POPT_PRIMARY || mode == ECGMode::ECG_COMBINED
                || mode == ECGMode::ECG_EXACT || mode == ECGMode::ECG_EXACT_STORED) {
                // Hawkeye/P-OPT-style: reset to 0 on hit
                // Every hit is evidence of cache-friendliness
                set[idx].rrpv = 0;
                // ECG_EXACT_STORED: re-stamp the exact next-ref at THIS access.
                // The line is being re-referenced now (current_src advanced), so
                // its stored prediction must refresh — exactly what a per-edge
                // mask does (each edge consumed carries its own hint). Without
                // this, the prediction would be frozen at first-fill position.
                if (mode == ECGMode::ECG_EXACT_STORED) {
                    if (nextUseLruEnabled())
                        stampQuantizedNextUse(set[idx]);
                    else
                        set[idx].ecg_exact_pred =
                            computeExactPredForStamp(set[idx].line_addr);
                }
            } else {
                bool admitted = false;
                // Refresh the prediction at the same property access that
                // delivers the new first future epoch. Unlike epoch-first
                // eviction, this preserves RRIP as the victim mechanism.
                if (futureAdmissionForSet(set_idx) &&
                    mode == ECGMode::ECG_GRASP_POPT && graph_ctx_ &&
                    graph_ctx_->isEcgEpochData(set[idx].line_addr) &&
                    graph_ctx_->hints_for_thread().edge_epoch_valid &&
                    graph_ctx_->hints_for_thread().current_src != UINT32_MAX) {
                    const auto& hints = graph_ctx_->hints_for_thread();
                    if (ecgTierCarried() &&
                        hints.edge_grasp_tier_valid) {
                        set[idx].ecg_dbg_tier = hints.edge_grasp_tier;
                    }
                    const uint32_t tier =
                        set[idx].ecg_dbg_tier >= 1 &&
                            set[idx].ecg_dbg_tier <= 3
                        ? set[idx].ecg_dbg_tier
                        : graph_ctx_->classifyGRASP(
                            set[idx].line_addr, size_bytes_);
                    set[idx].rrpv = combinedReuseAdmissionEnabled()
                        ? ecg_policy::combinedReuseAdmissionRRPV(
                            tier, deliveredFirstReuseEpoch(hints),
                            currentReuseEpoch(),
                            graph_ctx_->edge_epoch_count, 7)
                        : ecg_policy::reuseAdmissionRRPV(
                            deliveredFirstReuseEpoch(hints),
                            currentReuseEpoch(),
                            graph_ctx_->edge_epoch_count, 7);
                    ++reuse_admission_updates_;
                    admitted = true;
                }
                // GRASP-faithful fallback for accesses without a delivered
                // ReusePlan epoch.
                if (!admitted && graph_ctx_) {
                    uint64_t addr = set[idx].line_addr;
                    if (ecgTierCarried() &&
                        mode == ECGMode::ECG_GRASP_POPT &&
                        graph_ctx_->isEcgEpochData(addr) &&
                        graph_ctx_->hints_for_thread().edge_grasp_tier_valid) {
                        set[idx].ecg_dbg_tier =
                            graph_ctx_->hints_for_thread().edge_grasp_tier;
                    }
                    uint32_t tier = mode == ECGMode::ECG_GRASP_POPT &&
                            set[idx].ecg_dbg_tier != 0
                        ? set[idx].ecg_dbg_tier
                        : graph_ctx_->classifyGRASP(addr, size_bytes_);
                    if (tier == 1) set[idx].rrpv = 0;           // Hot: aggressive reset
                    else if (set[idx].rrpv > 0) set[idx].rrpv--; // Others: gradual
                }
                // ECG_GRASP_POPT: refresh the stored ABSOLUTE next-ref epoch at this
                // re-reference, but ONLY when this access actually DELIVERED a per-edge
                // epoch. A sequential/cleared read (clearEdgeEpoch -> valid=false) is NOT
                // a delivery and must leave the line's existing stamp untouched (matching
                // gem5/Sniper, which only stamp on real delivery).
                if (mode == ECGMode::ECG_GRASP_POPT && graph_ctx_ &&
                    graph_ctx_->isEcgEpochData(set[idx].line_addr) &&
                    graph_ctx_->hints_for_thread().edge_epoch_valid) {
                    set[idx].ecg_epoch = graph_ctx_->hints_for_thread().edge_epoch;
                    set[idx].ecg_epoch_valid = true;
                    stampEpochSchedule(set[idx]);
                }
            }
        }
    }

    // ── Eviction verification trace (ECG_EVICT_TRACE=N prints the first N
    // eviction decisions with every candidate's fields + the chosen victim and
    // reason, so each policy's behavior can be hand-verified). ──
    void traceEvict(const char* pol, std::vector<CacheLine>& set,
                    size_t victim, const char* reason, uint32_t curEpoch) {
        static long budget = -2;
        if (budget == -2) {
            const char* e = std::getenv("ECG_EVICT_TRACE");
            budget = e ? std::atol(e) : 0;
        }
        if (budget <= 0) return;
        // Only trace the LLC (L3) — L1/L2 are LRU and would consume the budget.
        if (name_ != "L3" && name_ != "L3-Shared") return;
        // ECG-CONFIG one-shot debug banner (ECG_DEBUG=1): fires once on the first L3
        // eviction for ANY kernel (PR/BFS/BC), proving the resolved policy/mode/variant.
        // Universal — unlike the per-edge-mask init path, every evicting kernel hits this.
        static bool ecg_cfg_announced = false;
        if (!ecg_cfg_announced) {
            ecg_cfg_announced = true;
            const char* dbg = std::getenv("ECG_DEBUG");
            if (dbg && *dbg && std::string(dbg) != "0") {
                const char* m = std::getenv("ECG_MODE");
                const char* var = std::getenv("ECG_VARIANT");
                const char* ch = std::getenv("ECG_EDGE_MASK_CHARGED");
                std::cerr << "[ECG-CONFIG sim=cache_sim policy=" << pol
                          << " mode=" << (m ? m : "-")
                          << " variant=" << (var ? var : "rrip_first")
                          << " charged=" << (ch ? ch : "?") << "]\n";
            }
        }
        --budget;
        std::cerr << "[EVICT L3 pol=" << pol << " curEpoch=" << curEpoch
                  << " set_ways=" << associativity_ << "]\n";
        for (size_t i = 0; i < associativity_; i++) {
            bool prop = false;
            if (graph_ctx_) {
                const char* mode = std::getenv("ECG_MODE");
                prop = (policy_ == EvictionPolicy::ECG && mode &&
                        std::string(mode) == "ECG_GRASP_POPT")
                    ? graph_ctx_->isEcgEpochData(set[i].line_addr)
                    : graph_ctx_->isPropertyData(set[i].line_addr);
            }
            uint32_t ne = (graph_ctx_ && graph_ctx_->edge_epoch_count)
                ? graph_ctx_->edge_epoch_count : 32u;
            uint32_t dist = ecg_policy::epochDistance(
                set[i].ecg_epoch, curEpoch, ne);
            for (uint8_t k = 0; k < set[i].ecg_epoch_sched_n; ++k) {
                uint32_t scheduled = ecg_policy::epochDistance(
                    set[i].ecg_epoch_sched[k], curEpoch, ne);
                if (scheduled < dist) dist = scheduled;
            }
            uint16_t epoch2 = set[i].ecg_epoch_sched_n > 1
                ? set[i].ecg_epoch_sched[1] : set[i].ecg_epoch;
            std::cerr << "   way" << i
                      << " valid=" << set[i].valid
                      << " rrpv=" << (int)set[i].rrpv
                      << " epoch=" << set[i].ecg_epoch
                      << " dist=" << dist
                      << " prop=" << (int)prop
                      << " stamped=" << (int)(prop && set[i].ecg_epoch_valid)
                      << " dbg=" << (int)set[i].ecg_dbg_tier
                      << " last=" << set[i].last_access
                      << " epoch2=" << epoch2
                      << " sched_n=" << (int)set[i].ecg_epoch_sched_n
                      << (i == victim ? "   <== VICTIM" : "") << "\n";
        }
        std::cerr << "   -> victim=way" << victim << " reason=" << reason << "\n";
    }

    size_t findVictim(std::vector<CacheLine>& set) {
        // Real-cache invariant: every policy fills an invalid (empty) way
        // before evicting a valid line. RRIP/SRRIP insert into an invalid way
        // first (Jaleel ISCA'10), and gem5/Sniper fill invalid ways natively, so
        // cache_sim must too — for cross-policy AND cross-sim fairness. (Earlier,
        // GRASP and ECG:DBG_ONLY skipped this to mirror Faldu's trace simulator,
        // which models no fills; that made them pathological at low pressure —
        // evicting valid lines while empty ways sat idle — and unfairly weakened
        // the GRASP baseline relative to SRRIP/ECG. Fixed: always invalid-first.)
        const bool replayOfficialGraspEmptyWayBehavior =
            policy_ == EvictionPolicy::GRASP &&
            std::getenv("GRASP_OFFICIAL_TRACE_EMPTY_WAYS") != nullptr;
        if (!replayOfficialGraspEmptyWayBehavior) {
            for (size_t i = 0; i < associativity_; i++) {
                if (!set[i].valid) return i;
            }
        }
        
        // All lines valid, use eviction policy
        switch (policy_) {
            case EvictionPolicy::LRU:
                return findVictimLRU(set);
            case EvictionPolicy::FIFO:
                return findVictimFIFO(set);
            case EvictionPolicy::RANDOM:
                return findVictimRandom(set);
            case EvictionPolicy::LFU:
                return findVictimLFU(set);
            case EvictionPolicy::PLRU:
                return findVictimPLRU(set);
            case EvictionPolicy::SRRIP:
                return findVictimSRRIP(set);
            case EvictionPolicy::HAWKEYE:
                return findVictimHAWKEYE(set);
            case EvictionPolicy::PIN:
                return findVictimPIN(set);
            case EvictionPolicy::GRASP:
                return findVictimGRASP(set);
            case EvictionPolicy::POPT:
                return findVictimPOPT(set);
            case EvictionPolicy::ECG:
                return findVictimECG(set);
            default:
                return findVictimLRU(set);
        }
    }

    size_t findVictimLRU(std::vector<CacheLine>& set) {
        size_t victim = 0;
        uint64_t oldest = set[0].last_access;
        for (size_t i = 1; i < associativity_; i++) {
            if (set[i].last_access < oldest) {
                oldest = set[i].last_access;
                victim = i;
            }
        }
        traceEvict("LRU", set, victim, "min last_access", 0);
        return victim;
    }

    // PIN (Faldu et al., 2020 baseline; mirrors upstream pin.cpp):
    // LRU among unpinned ways. Returns SIZE_MAX to signal bypass when every
    // way in the set is pinned, matching upstream's all-pinned bypass.
    size_t findVictimPIN(std::vector<CacheLine>& set) {
        size_t victim = SIZE_MAX;
        uint64_t oldest = 0;
        for (size_t i = 0; i < associativity_; i++) {
            if (set[i].pin) continue;
            if (victim == SIZE_MAX || set[i].last_access < oldest) {
                oldest = set[i].last_access;
                victim = i;
            }
        }
        return victim;
    }

    size_t findVictimFIFO(std::vector<CacheLine>& set) {
        size_t victim = 0;
        uint64_t oldest = set[0].insert_time;
        for (size_t i = 1; i < associativity_; i++) {
            if (set[i].insert_time < oldest) {
                oldest = set[i].insert_time;
                victim = i;
            }
        }
        return victim;
    }

    size_t findVictimRandom(std::vector<CacheLine>& set) {
        std::uniform_int_distribution<size_t> dist(0, associativity_ - 1);
        return dist(rng_);
    }

    size_t findVictimLFU(std::vector<CacheLine>& set) {
        // LFU aging: periodically halve access counts to prevent stale data
        // from staying cached forever. Triggered every 1024 evictions.
        // (ECG reference ages by decrementing; halving is equivalent and faster.)
        if ((stats_.evictions.load() & 1023) == 0) {
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].valid) set[i].access_count >>= 1;
            }
        }
        size_t victim = 0;
        uint64_t min_count = set[0].access_count;
        for (size_t i = 1; i < associativity_; i++) {
            if (set[i].access_count < min_count) {
                min_count = set[i].access_count;
                victim = i;
            }
        }
        return victim;
    }

    size_t findVictimPLRU(std::vector<CacheLine>& set) {
        // Simplified PLRU: use LRU for now
        // Full tree-PLRU would need additional state
        return findVictimLRU(set);
    }

    size_t findVictimSRRIP(std::vector<CacheLine>& set) {
        // Find line with RRPV = 3 (distant re-reference)
        while (true) {
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv == 3) return i;
            }

            // Increment all RRPVs
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv < 3) set[i].rrpv++;
            }
        }
    }

    size_t findVictimHAWKEYE(std::vector<CacheLine>& set) {
        // Faithful Hawkeye/CRC2 rule: evict the first RRPV=7 line when
        // present; otherwise evict a current maximum-RRPV line. Unlike SRRIP,
        // Hawkeye does not age the set until a maximum appears.
        std::vector<uint8_t> rrpv(associativity_, 0);
        for (size_t way = 0; way < associativity_; ++way)
            rrpv[way] = set[way].rrpv;
        const size_t victim =
            hawkeye_policy::selectVictim(rrpv.data(), rrpv.size());
        if (hawkeye_state_ && set[victim].valid &&
            set[victim].rrpv != hawkeye_policy::kMaxRrpv) {
            hawkeye_state_->eviction(
                evicting_set_idx_,
                set[victim].hawkeye_signature,
                set[victim].hawkeye_prefetch);
        }
        return victim;
    }

    // ================================================================
    // GRASP: Graph-aware cache Replacement with Software Prefetching
    // (Faldu et al., 2020; reference: grasp.cpp)
    //
    // RRIP-based policy with 3-tier insertion depending on address region:
    //   High-reuse (hot hubs):    insert RRPV = 1 (P_RRIP), hit → 0
    //   Moderate-reuse:           insert RRPV = M-1 (I_RRIP), hit → decrement
    //   Low-reuse (cold/other):   insert RRPV = M (M_RRIP), hit → decrement
    // Eviction: find way with max RRPV, age all if none at max.
    // Requires DBG-reordered graph so hot vertices occupy low addresses.
    // ================================================================
    size_t findVictimGRASP(std::vector<CacheLine>& set) {
        // Same eviction as SRRIP: find way with rrpv == max, age until found
        constexpr uint8_t M_RRIP = 7;  // 3-bit RRPV, max = 2^3-1
        while (true) {
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv >= M_RRIP) {
                    traceEvict("GRASP", set, i, "first rrpv==max (RRIP, no epoch)", 0);
                    return i;
                }
            }
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv < M_RRIP) set[i].rrpv++;
            }
        }
    }

    // ================================================================
    // P-OPT: Practical Optimal cache replacement for Graph Analytics
    // (Balaji et al., 2021; reference: llc.cpp)
    //
    // Uses the graph's transpose (encoded in a compressed rereference
    // matrix) to predict exactly when each cache line will be accessed
    // again. Evicts the line with the furthest next-reference distance.
    // When multiple lines tie on rereference distance, uses RRIP aging
    // to break ties (matching the reference implementation).
    //
    // Requires: initPOPT() called before simulation with a precomputed
    // rereference matrix from makeOffsetMatrix() in popt.h.
    // ================================================================
    size_t findVictimPOPT(std::vector<CacheLine>& set) {
        // Check if either unified context or legacy state is available
        bool has_popt = (graph_ctx_ && graph_ctx_->rereference.matrix) || popt_state_.enabled;
        if (!has_popt) {
            return findVictimLRU(set);  // Fallback if not initialized
        }

        // Phase 1: Evict non-graph data first (streaming/CSR metadata)
        for (size_t i = 0; i < associativity_; i++) {
            uint64_t la = set[i].line_addr;
            bool is_graph_data = false;
            if (graph_ctx_) {
                is_graph_data = graph_ctx_->isPropertyData(la);
            } else {
                is_graph_data = (la >= popt_state_.irreg_base && la < popt_state_.irreg_bound);
            }
            if (!is_graph_data) return i;  // Not graph vertex data — evict immediately
        }

        // Phase 2: All ways contain graph vertex data — find max rereference distance
        uint8_t maxRerefDist = 0;
        uint8_t wayRerefDists[64] = {};
        for (size_t i = 0; i < associativity_; i++) {
            uint64_t la = set[i].line_addr;
            uint32_t dist;
            if (graph_ctx_) {
                dist = graph_ctx_->findNextRef(la);
            } else {
                uint32_t cline_id = static_cast<uint32_t>(
                    (la - popt_state_.irreg_base) / line_size_);
                dist = popt_state_.findNextRef(cline_id);
            }
            uint8_t d = static_cast<uint8_t>(std::min(dist, uint32_t(127)));
            wayRerefDists[i] = d;
            if (d > maxRerefDist) maxRerefDist = d;
        }

        // Phase 3: RRIP tiebreaker among lines with max rereference distance
        // (matching reference llc.cpp: age RRPV only for tied lines)
        constexpr uint8_t M_RRPV = 7;
        while (true) {
            for (size_t i = 0; i < associativity_; i++) {
                if (wayRerefDists[i] == maxRerefDist && set[i].rrpv >= M_RRPV) {
                    return i;
                }
            }
            // Age only the tied lines
            for (size_t i = 0; i < associativity_; i++) {
                if (wayRerefDists[i] == maxRerefDist && set[i].rrpv < M_RRPV) {
                    set[i].rrpv++;
                }
            }
        }
    }

    // ================================================================
    // ECG: Graph-aware cache replacement (Mughrabi et al., GrAPL)
    //
    // Layered eviction with mode-dependent tiebreaker priority:
    //   Level 1 (all modes): SRRIP aging — find max RRPV, age until found
    //   Level 2/3 depend on ECGMode:
    //     DBG_PRIMARY:  L2=DBG tier (coldest vertex), L3=dynamic P-OPT
    //     POPT_PRIMARY: L2=dynamic P-OPT (furthest future), L3=DBG tier
    //     DBG_ONLY:     GRASP-faithful SRRIP victim selection, no L2/L3 tiebreak
    //
    // Key design points:
    //  - RRPV set at insert from DBG tier (bucketToRRPV), ages via SRRIP
    //  - ecg_dbg_tier stored per-line (structural, constant)
    //  - P-OPT consulted dynamically via findNextRef() at eviction time
    //    (not cached — avoids stale snapshot problem)
    // ================================================================
    // ECG_EXACT_STORED helper: stamp the ABSOLUTE next-reference POSITION for a
    // property line at the CURRENT traversal position (current_src). pull-PR sets
    // current_src=u before reading each in-neighbor's property, so this is the
    // value an offline per-edge mask would carry for the edge consumed now.
    //
    // We store the ABSOLUTE position (cur + distance), not the relative distance:
    // a cached line receives no reference between its last access and eviction
    // (any such reference is a cache hit that re-stamps), so the next-ref position
    // measured at last-access equals the one at eviction. Absolute positions are
    // comparable across ways (same 0..N timeline); relative distances stamped at
    // different last-access positions are NOT. Eviction reads the stored value —
    // no live recompute. (exact_bits log-quantization is incompatible with
    // absolute stamping and must stay 0 for this mode.)
    uint32_t computeExactPredForStamp(uint64_t line_addr) {
        if (!graph_ctx_) return UINT32_MAX;
        if (graph_ctx_->exact_off.empty() && graph_ctx_->bfs_in_off.empty())
            return UINT32_MAX;
        if (!graph_ctx_->isPropertyData(line_addr)) return UINT32_MAX;
        uint32_t cur = graph_ctx_->hints_for_thread().current_src;
        if (graph_ctx_->exact_bfs) {
            // BFS clock: exactNextRefBFS returns the distance in VISIT-ORDER units
            // measured from visit_pos[cur], so the absolute position is
            // visit_pos[cur] + d (NOT cur + d, which mixes vertex-id and visit-order).
            if (cur >= graph_ctx_->visit_pos.size()) return UINT32_MAX;
            uint32_t base = graph_ctx_->visit_pos[cur];
            if (base == UINT32_MAX) return UINT32_MAX;
            uint32_t db = graph_ctx_->exactNextRefBFS(line_addr, cur);
            if (db == UINT32_MAX) return UINT32_MAX;
            return base + db;
        }
        uint32_t d = graph_ctx_->exactNextRef(line_addr, cur);
        if (d == UINT32_MAX) return UINT32_MAX;   // no future ref -> evict first
        return cur + d;                            // absolute next-reference position
    }

    static bool nextUseLruEnabled() {
        static const bool enabled = []() {
            const char* value = std::getenv("ECG_NEXT_USE_LRU");
            return value && value[0] && std::string(value) != "0";
        }();
        return enabled;
    }

    static bool nextUseLiveEnabled() {
        static const bool enabled = []() {
            const char* value = std::getenv("ECG_NEXT_USE_LIVE");
            return value && value[0] && std::string(value) != "0";
        }();
        return enabled;
    }

    static bool nextUseRefreshGuaranteed() {
        static const bool guaranteed = []() {
            const bool refresh =
                std::getenv("ECG_STORED_REFRESH") != nullptr;
            const bool llc_only =
                std::getenv("ECG_REFRESH_LLC_ONLY") != nullptr;
            return refresh && !llc_only;
        }();
        return guaranteed;
    }

    static uint32_t configuredNextUseBits() {
        static const uint32_t bits = []() {
            const char* value = std::getenv("ECG_NEXT_USE_BITS");
            if (!value || !value[0]) {
                std::fprintf(
                    stderr,
                    "[FATAL] ECG_NEXT_USE_LRU requires "
                    "ECG_NEXT_USE_BITS\n");
                std::abort();
            }
            int parsed = std::atoi(value);
            if (parsed < 1) parsed = 1;
            if (parsed > 15) parsed = 15;
            return static_cast<uint32_t>(parsed);
        }();
        return bits;
    }

    void stampQuantizedNextUse(CacheLine& line) {
        line.ecg_next_use = 0;
        line.ecg_future_state = ecg_policy::FutureState::UNKNOWN;
        if (!graph_ctx_ || !graph_ctx_->isPropertyData(line.line_addr))
            return;
        const auto& hints = graph_ctx_->hints_for_thread();
        if (hints.edge_next_use_valid) {
            line.ecg_next_use = hints.edge_next_use;
            line.ecg_future_state =
                static_cast<ecg_policy::FutureState>(
                    hints.edge_future_state);
            return;
        }

        if (!nextUseLiveEnabled())
            return;
        const uint32_t current =
            hints.current_src;
        const uint32_t vertices = graph_ctx_->exact_nv;
        if (current == UINT32_MAX || vertices == 0)
            return;
        const uint32_t distance =
            graph_ctx_->exactNextRef(line.line_addr, current);
        if (distance == UINT32_MAX) {
            line.ecg_future_state = ecg_policy::FutureState::DEAD;
            return;
        }
        const uint32_t bits = configuredNextUseBits();
        const uint32_t levels = (uint32_t{1} << bits) - 1;
        const uint64_t absolute =
            std::min<uint64_t>(
                static_cast<uint64_t>(current) + distance,
                vertices - 1);
        line.ecg_next_use = vertices > 1
            ? static_cast<uint32_t>(
                  (absolute * levels) / (vertices - 1))
            : 0;
        line.ecg_future_state = ecg_policy::FutureState::FINITE;
    }

    bool ref32Enabled() const {
        return policy_ == EvictionPolicy::ECG && graph_ctx_ &&
            graph_ctx_->mask_config.ecg_mode == ECGMode::ECG_REF32;
    }

    static uint32_t configuredRef32DeadlineBits() {
        static const uint32_t bits = []() {
            const char* value = std::getenv("ECG_REF32_DEADLINE_BITS");
            int parsed = value
                ? std::atoi(value)
                : static_cast<int>(ecg_ref32::kDefaultDeadlineBits);
            if (parsed < 2) parsed = 2;
            if (parsed > 32) parsed = 32;
            return static_cast<uint32_t>(parsed);
        }();
        return bits;
    }

    bool isGovernedProperty(uint64_t address) const {
        return graph_ctx_ && graph_ctx_->isRef32Data(address);
    }

    bool isRef32Governed(uint64_t address) const {
        return ref32Enabled() && graph_ctx_->isRef32Data(address);
    }

    bool shouldBypassRef32(uint64_t address) const {
        if (!isRef32Governed(address))
            return false;
        const auto& hints = graph_ctx_->hints_for_thread();
        return hints.edge_ref_valid &&
            static_cast<ecg_ref32::State>(hints.edge_ref_state) ==
                ecg_ref32::State::DEAD;
    }

    void stampRef32(CacheLine& line) {
        line.ecg_ref32_deadline = 0;
        line.ecg_ref32_exact_deadline = 0;
        line.ecg_ref32_state = ecg_ref32::State::UNKNOWN;
        if (!isRef32Governed(line.line_addr))
            return;
        const auto& hints = graph_ctx_->hints_for_thread();
        if (!hints.edge_ref_valid)
            return;
        const auto state =
            static_cast<ecg_ref32::State>(hints.edge_ref_state);
        line.ecg_ref32_state = state;
        if (state == ecg_ref32::State::DEAD) {
            line.rrpv = 7;
            return;
        }
        if (state != ecg_ref32::State::FINITE)
            return;
        const uint32_t distance = std::max<uint32_t>(
            1, hints.edge_ref_distance);
        const uint32_t deadline_bits = configuredRef32DeadlineBits();
        const uint32_t mask = deadline_bits == 32
            ? UINT32_MAX : (uint32_t{1} << deadline_bits) - 1u;
        const uint32_t max_forward = deadline_bits == 32
            ? ecg_ref32::kMaxFiniteDistance
            : (uint32_t{1} << (deadline_bits - 1)) - 1u;
        line.ecg_ref32_deadline = static_cast<uint32_t>(
            hints.edge_ref_sequence +
            std::min<uint32_t>(distance, max_forward)) & mask;
        line.ecg_ref32_exact_deadline =
            hints.edge_ref_sequence + distance;
        line.rrpv = ecg_ref32::distanceRRPV(distance);
    }

    uint32_t currentNextUseBucket() const {
        if (!graph_ctx_ || graph_ctx_->exact_nv == 0)
            return 0;
        const uint32_t current =
            graph_ctx_->hints_for_thread().current_src;
        if (current == UINT32_MAX)
            return 0;
        const uint32_t levels =
            (uint32_t{1} << configuredNextUseBits()) - 1;
        const uint32_t within_iteration = graph_ctx_->exact_nv > 1
            ? static_cast<uint32_t>(
                  (static_cast<uint64_t>(current) * levels) /
                  (graph_ctx_->exact_nv - 1))
            : 0;
        return graph_ctx_->hints_for_thread().current_iteration *
            (levels + 1) + within_iteration;
    }

    size_t findVictimECG(std::vector<CacheLine>& set) {
        uint8_t rrpv_max = (graph_ctx_ && graph_ctx_->mask_config.enabled)
            ? graph_ctx_->mask_config.rrpv_max : 7;

        // Determine ECG mode
        ECGMode mode = (graph_ctx_ && graph_ctx_->mask_config.enabled)
            ? graph_ctx_->mask_config.ecg_mode : ECGMode::DBG_PRIMARY;

        if (mode == ECGMode::ECG_REF32 && graph_ctx_) {
            ecg_ref32::WayState ways[64];
            for (size_t i = 0; i < associativity_; ++i) {
                ways[i].property =
                    graph_ctx_->isEcgEpochData(set[i].line_addr);
                ways[i].rrpv = set[i].rrpv;
                ways[i].recency = set[i].last_access;
                ways[i].grasp_tier =
                    static_cast<uint8_t>(graph_ctx_->classifyGRASP(
                        set[i].line_addr, size_bytes_));
                ways[i].state = set[i].ecg_ref32_state;
                ways[i].quantized_deadline =
                    set[i].ecg_ref32_deadline;
                ways[i].exact_deadline =
                    set[i].ecg_ref32_exact_deadline;
            }
            ecg_ref32::VictimReason reason;
            const size_t victim = ecg_ref32::selectVictim(
                ways, associativity_,
                graph_ctx_->hints_for_thread().edge_ref_sequence,
                graph_ctx_->ref32_exact_diagnostic, &reason,
                configuredRef32DeadlineBits());
            switch (reason) {
                case ecg_ref32::VictimReason::DEAD_PROPERTY:
                    ++ref32_dead_victims_;
                    break;
                case ecg_ref32::VictimReason::NON_PROPERTY:
                    ++ref32_non_property_victims_;
                    break;
                case ecg_ref32::VictimReason::UNKNOWN_PROPERTY:
                    ++ref32_unknown_victims_;
                    break;
                case ecg_ref32::VictimReason::FINITE_PROPERTY:
                    ++ref32_finite_victims_;
                    break;
            }
            return victim;
        }

        // ── Phase 0: Evict non-property data first (matching P-OPT Phase 1) ──
        // Non-property data (CSR edges, offsets, streaming) has no oracle
        // prediction and should be evicted before property data.
        // For DBG modes, this is already handled by RRPV=7 at insert (cold).
        // For POPT_PRIMARY, all lines get RRPV=6 at insert, so non-property
        // lines compete equally — we must explicitly prefer evicting them.
        if (graph_ctx_ && mode == ECGMode::POPT_PRIMARY) {
            for (size_t i = 0; i < associativity_; i++) {
                if (!graph_ctx_->isPropertyData(set[i].line_addr)) {
                    return i;  // Evict non-property data immediately
                }
            }
        }

        // ── ECG_EXACT: exact position-indexed next-reference (per-edge idea) ──
        // Mirrors POPT_PRIMARY (non-property first, max-distance over ALL ways,
        // DBG tiebreak) but the distance is the EXACT next-reference computed
        // from the graph's out-adjacency at the CURRENT traversal vertex
        // (graph_ctx_->exactNextRef) — no [epoch×line] matrix, no quantization,
        // no averaging. Tests whether exact position-indexed reuse (the limit of
        // the per-edge mask) beats P-OPT's coarse 256-epoch matrix.
        if (mode == ECGMode::ECG_EXACT && graph_ctx_ &&
            (!graph_ctx_->exact_off.empty() || !graph_ctx_->bfs_in_off.empty())) {
            const bool use_bfs = graph_ctx_->exact_bfs;
            for (size_t i = 0; i < associativity_; i++) {
                if (!graph_ctx_->isPropertyData(set[i].line_addr)) return i;
            }
            uint32_t cur = graph_ctx_->hints_for_thread().current_src;
            uint64_t maxDist = 0;
            uint64_t wayDist[64] = {};
            for (size_t i = 0; i < associativity_; i++) {
                uint64_t d = use_bfs
                    ? graph_ctx_->exactNextRefBFS(set[i].line_addr, cur)
                    : graph_ctx_->exactNextRef(set[i].line_addr, cur);
                wayDist[i] = d;
                if (d > maxDist) maxDist = d;
            }
            constexpr uint8_t M_RRPV = 7;
            while (true) {
                size_t best = SIZE_MAX;
                uint8_t best_dbg = 0;
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayDist[i] == maxDist && set[i].rrpv >= M_RRPV) {
                        if (best == SIZE_MAX || set[i].ecg_dbg_tier > best_dbg) {
                            best = i;
                            best_dbg = set[i].ecg_dbg_tier;
                        }
                    }
                }
                if (best != SIZE_MAX) return best;
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayDist[i] == maxDist && set[i].rrpv < M_RRPV) set[i].rrpv++;
                }
            }
        }

        // ── ECG_EXACT_STORED: realizable per-edge-mask version of ECG_EXACT ──
        // Identical eviction structure (non-property first, max-distance over
        // ALL ways, DBG tiebreak, SRRIP aging) but the distance is the value
        // STAMPED at the line's last access (set[i].ecg_exact_pred) — what a
        // precomputed per-edge mask carries — instead of being recomputed live
        // at eviction. The only semantic difference from ECG_EXACT is staleness
        // (stamp taken at last access position, not the eviction position).
        if (mode == ECGMode::ECG_EXACT_STORED && graph_ctx_) {
            if (nextUseLruEnabled()) {
                constexpr uint8_t RRPV_MAX = 7;
                const uint32_t current_bucket = currentNextUseBucket();
                ecg_policy::WayState ways[64];
                for (size_t i = 0; i < associativity_; ++i) {
                    ways[i].prop =
                        graph_ctx_->isPropertyData(set[i].line_addr);
                    ways[i].rrpv = set[i].rrpv;
                    ways[i].recency = set[i].last_access;
                    ways[i].dbg = set[i].ecg_dbg_tier;
                    ways[i].dist = 0;
                    ways[i].stamped =
                        set[i].ecg_future_state !=
                        ecg_policy::FutureState::UNKNOWN;
                    ways[i].next_use = set[i].ecg_next_use;
                    ways[i].future_state =
                        ecg_policy::effectiveFutureState(
                            set[i].ecg_future_state,
                            ways[i].next_use, current_bucket,
                            nextUseRefreshGuaranteed());
                }
                return ecg_policy::selectVictim(
                    ways, associativity_,
                    ecg_policy::NEXT_USE_LRU, RRPV_MAX);
            }
            for (size_t i = 0; i < associativity_; i++) {
                if (!graph_ctx_->isPropertyData(set[i].line_addr)) return i;
            }
            uint64_t maxDist = 0;
            uint64_t wayDist[64] = {};
            for (size_t i = 0; i < associativity_; i++) {
                wayDist[i] = set[i].ecg_exact_pred;
                if (wayDist[i] > maxDist) maxDist = wayDist[i];
            }
            constexpr uint8_t M_RRPV = 7;
            while (true) {
                size_t best = SIZE_MAX;
                uint8_t best_dbg = 0;
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayDist[i] == maxDist && set[i].rrpv >= M_RRPV) {
                        if (best == SIZE_MAX || set[i].ecg_dbg_tier > best_dbg) {
                            best = i;
                            best_dbg = set[i].ecg_dbg_tier;
                        }
                    }
                }
                if (best != SIZE_MAX) return best;
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayDist[i] == maxDist && set[i].rrpv < M_RRPV) set[i].rrpv++;
                }
            }
        }

        // ── ECG_COMBINED: Pure SRRIP aging (both signals already at insertion) ──
        // Hawkeye-inspired: the combined insertion RRPV already encodes
        // both degree and rereference signals. Standard SRRIP aging is
        // sufficient — no tiebreakers needed. First line at max RRPV wins.
        if (mode == ECGMode::ECG_EXACT_MASK && graph_ctx_) {
            // Precomputed exact 5-bit drove insertion/hit RRPV (near=keep,
            // far=evict). Eviction = non-property first, then RRIP aging, with the
            // stored 5-bit as tiebreak among max-RRPV ways (higher = farther = evict).
            for (size_t i = 0; i < associativity_; i++) {
                if (!graph_ctx_->isPropertyData(set[i].line_addr)) return i;
            }
            while (true) {
                size_t best = SIZE_MAX;
                uint8_t best_hint = 0;
                for (size_t i = 0; i < associativity_; i++) {
                    if (set[i].rrpv >= rrpv_max &&
                        (best == SIZE_MAX || set[i].ecg_popt_hint > best_hint)) {
                        best = i;
                        best_hint = set[i].ecg_popt_hint;
                    }
                }
                if (best != SIZE_MAX) return best;
                for (size_t i = 0; i < associativity_; i++) {
                    if (set[i].rrpv < rrpv_max) set[i].rrpv++;
                }
            }
        }

        // ── ECG_GRASP_POPT: GRASP insertion + P-OPT-style eviction over the stored
        // ABSOLUTE next-ref epoch. Evict the line with the MAX circular distance
        // (stored_epoch - current_epoch mod 32): near-future (small) -> keep,
        // far-future AND stale/passed (large) -> evict. Non-property first; the
        // stored epoch is the sole key (no matrix, no query). ──
        if (mode == ECGMode::ECG_GRASP_POPT && graph_ctx_) {
            // ── ECG_VARIANT factorial ablation. Shared invariants in ALL variants:
            //   epoch is PROPERTY-ONLY; records evicted by recency; unstamped
            //   property (epoch==0) falls back to recency (never treated as farthest).
            //     grasp_only(0): pure RRIP, no epoch         (== GRASP sanity)
            //     epoch_first(1): farthest-epoch property (epoch VETOES recency);
            //                     no stamped property -> recency
            //     rrip_first(2,default): max-rrpv set (recency VETOES); records-first
            //                     by recency, then farthest-epoch property
            //     epoch_only(3): records-first by recency, then farthest-epoch property
            //                     (insertion uniform -> isolates the epoch vs P-OPT)
            //     shortcircuit(4,legacy): non-property first, then epoch among property
            static const int configured_variant =
                ecg_policy::parseVariant(std::getenv("ECG_VARIANT"));
            int variant = configured_variant;
            if (set_dueling_) {
                const size_t sample_set =
                    duelingSampleSetIndex(evicting_set_idx_);
                const int leader = ecg_policy::duelingLeaderArm(sample_set);
                const uint8_t arm = leader >= 0
                    ? static_cast<uint8_t>(leader)
                    : dueling_selector_.winnerArm();
                if (leader < 0 && graph_ctx_->hints_for_thread().current_src !=
                        UINT32_MAX)
                    ++dueling_follower_selections_by_arm_[arm];
                variant = ecg_policy::duelingArmVariant(arm);
            }
            const uint32_t n = graph_ctx_->exact_nv
                ? graph_ctx_->exact_nv : graph_ctx_->topology.num_vertices;
            const uint32_t ne = graph_ctx_->edge_epoch_count ? graph_ctx_->edge_epoch_count : 32u;
            uint32_t cur = graph_ctx_->hints_for_thread().current_src;
            uint32_t cur_epoch = (n > 0 && cur != UINT32_MAX)
                ? static_cast<uint32_t>(((uint64_t)cur * ne) / n) : 0;
            if (cur_epoch >= ne) cur_epoch = ne - 1;
            constexpr uint8_t RRPV_MAX = 7;
            auto isProp  = [&](size_t i){
                return graph_ctx_->isEcgEpochData(set[i].line_addr);
            };
            auto dist    = [&](size_t i){
                // 1-D base: circular distance to the single stamped next-ref epoch.
                uint32_t d = ecg_policy::epochDistance(
                    set[i].ecg_epoch, cur_epoch, ne);
                // 2-D recovery (ECG_REUSE_PLAN_DEPTH): take the SOONEST upcoming entry in
                // the per-line schedule. A passed entry wraps to a large circular
                // distance, so min() naturally skips it and the line self-advances to
                // its next true reference — emulating the matrix's per-epoch recompute.
                for (uint8_t k = 0; k < set[i].ecg_epoch_sched_n; ++k) {
                    uint32_t dk = ecg_policy::epochDistance(
                        set[i].ecg_epoch_sched[k], cur_epoch, ne);
                    if (dk < d) d = dk;
                }
                return d;
            };
            auto stamped = [&](size_t i){ return isProp(i) && set[i].ecg_epoch_valid; };

            // Build the per-way state and delegate the DECISION to the shared
            // ecg_policy::selectVictim (identical across cache_sim / gem5 / Sniper).
            ecg_policy::WayState ws[64];
            for (size_t i = 0; i < associativity_; i++) {
                ws[i].prop    = isProp(i);
                ws[i].rrpv    = set[i].rrpv;
                ws[i].recency = set[i].last_access;
                ws[i].dbg     = set[i].ecg_dbg_tier;
                ws[i].dist    = dist(i);
                ws[i].stamped = stamped(i);
            }
            size_t victim = ecg_policy::selectVictim(ws, associativity_, variant, RRPV_MAX);
            for (size_t i = 0; i < associativity_; i++) set[i].rrpv = ws[i].rrpv;  // persist SRRIP aging

            // Reconstruct the trace pol/reason (verify_ecg.py keys on the pol name).
            const char* pol; const char* reason;
            if (variant == 0)      { pol = "ECG:grasp_only";  reason = "RRIP max-rrpv"; }
            else if (variant == 4) {
                if (!isProp(victim)) { pol = "ECG:shortcircuit";       reason = "first non-property"; }
                else                 { pol = "ECG:shortcircuit+epoch"; reason = "all-prop farthest epoch"; }
            } else if (variant == 2) {
                pol = "ECG:rrip_first";
                reason = !isProp(victim) ? "max-rrpv record by recency" : "max-rrpv farthest-epoch property";
            } else if (variant == 5) {
                pol = "ECG:degree_first";
                reason = !isProp(victim) ? "max-rrpv record by recency"
                                         : "max-rrpv coldest-degree then epoch";
            } else if (variant == 6) {
                pol = "ECG:lru_only";
                reason = "oldest recency";
            } else if (variant == 7) {
                pol = "ECG:record_lru";
                reason = !isProp(victim) ? "record by recency"
                                         : "property recency fallback";
            } else if (variant == 8) {
                pol = "ECG:rrip_no_epoch";
                reason = !isProp(victim) ? "max-rrpv record by recency"
                                         : "max-rrpv property fallback";
            } else if (variant == 9) {
                pol = "ECG:rrip_no_epoch_recency";
                reason = !isProp(victim) ? "max-rrpv record by recency"
                                         : "max-rrpv property by recency";
            } else if (variant == 10) {
                pol = "ECG:future_tier_first";
                reason = !isProp(victim)
                    ? "max-rrpv record by recency"
                    : "max-rrpv future then tier then recency";
            } else {
                pol = (variant == 1) ? "ECG:epoch_first" : "ECG:epoch_only";
                reason = !isProp(victim) ? "record by recency"
                       : stamped(victim) ? "farthest-epoch property" : "recency fallback";
            }
            traceEvict(pol, set, victim, reason, cur_epoch);
            return victim;
        }

        if (mode == ECGMode::ECG_COMBINED) {
            while (true) {
                for (size_t i = 0; i < associativity_; i++) {
                    if (set[i].rrpv >= rrpv_max) return i;
                }
                for (size_t i = 0; i < associativity_; i++) {
                    if (set[i].rrpv < rrpv_max) set[i].rrpv++;
                }
            }
        }

        // ── POPT_PRIMARY: P-OPT's 3-phase algorithm + ECG degree (DBG) tiebreak ──
        // Bypass SRRIP aging — P-OPT operates on ALL ways, not just RRPV-aged
        // candidates. NOTE: this is NOT identical to pure POPT. Among lines tied at
        // the max rereference distance, ECG additionally prefers the highest DBG
        // (degree) tier (the Level-3 enhancement in the loop below), whereas pure
        // findVictimPOPT returns the FIRST tied way. Because reref distance is
        // capped at 127, ties at the max are common, so this degree tiebreak is a
        // genuine ECG contribution (P-OPT + degree), applied identically in gem5
        // (ecg_rp.cc) for cross-sim consistency — it is the ECG:POPT_PRIMARY arm,
        // NOT a pure-P-OPT-parity arm (the plain POPT policy is that parity arm).
        if (mode == ECGMode::POPT_PRIMARY && graph_ctx_ && graph_ctx_->rereference.matrix) {
            // Phase 2: Find max rereference distance across ALL ways
            uint8_t maxRerefDist = 0;
            uint8_t wayRerefDists[64] = {};
            for (size_t i = 0; i < associativity_; i++) {
                uint32_t dist = graph_ctx_->findNextRef(set[i].line_addr);
                uint8_t d = static_cast<uint8_t>(std::min(dist, uint32_t(127)));
                wayRerefDists[i] = d;
                if (d > maxRerefDist) maxRerefDist = d;
            }

            // Phase 3: RRIP tiebreak among max-distance lines
            // (age only tied lines, matching P-OPT's Algorithm 2)
            // Level 3 enhancement: among RRIP ties, prefer highest DBG tier
            constexpr uint8_t M_RRPV = 7;
            while (true) {
                size_t best = SIZE_MAX;
                uint8_t best_dbg = 0;
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayRerefDists[i] == maxRerefDist && set[i].rrpv >= M_RRPV) {
                        if (best == SIZE_MAX || set[i].ecg_dbg_tier > best_dbg) {
                            best = i;
                            best_dbg = set[i].ecg_dbg_tier;
                        }
                    }
                }
                if (best != SIZE_MAX) return best;

                // Age only the tied lines
                for (size_t i = 0; i < associativity_; i++) {
                    if (wayRerefDists[i] == maxRerefDist && set[i].rrpv < M_RRPV) {
                        set[i].rrpv++;
                    }
                }
            }
        }

        // ── DBG_ONLY: GRASP-faithful mode ──
        // DBG_ONLY should isolate the degree/DBG insertion effect and match
        // GRASP's RRIP victim selection. Extra DBG tiebreaking belongs to
        // DBG_PRIMARY, not the GRASP-equivalence mode.
        if (mode == ECGMode::DBG_ONLY) {
            return findVictimGRASP(set);
        }

        // ── Level 1: SRRIP aging — find lines at max RRPV ──
        // Age all lines until at least one reaches rrpv_max.
        while (true) {
            bool found = false;
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv >= rrpv_max) { found = true; break; }
            }
            if (found) break;
            for (size_t i = 0; i < associativity_; i++) {
                if (set[i].rrpv < rrpv_max) set[i].rrpv++;
            }
        }

        // Collect candidates at max RRPV
        size_t candidates[64];
        size_t num_candidates = 0;
        for (size_t i = 0; i < associativity_; i++) {
            if (set[i].rrpv >= rrpv_max && num_candidates < 64)
                candidates[num_candidates++] = i;
        }
        if (num_candidates == 1) return candidates[0];

        // ── Level 2 tiebreak (mode-dependent) ──
        if (mode == ECGMode::DBG_PRIMARY || mode == ECGMode::DBG_ONLY) {
            // DBG tiebreak: evict highest ecg_dbg_tier (coldest/lowest-degree)
            uint8_t max_dbg = 0;
            for (size_t c = 0; c < num_candidates; c++)
                if (set[candidates[c]].ecg_dbg_tier > max_dbg)
                    max_dbg = set[candidates[c]].ecg_dbg_tier;

            // Narrow candidates to max-DBG lines
            size_t narrowed[64];
            size_t num_narrowed = 0;
            for (size_t c = 0; c < num_candidates; c++)
                if (set[candidates[c]].ecg_dbg_tier == max_dbg)
                    narrowed[num_narrowed++] = candidates[c];

            if (num_narrowed == 0)
                throw std::logic_error("DBG victim narrowing produced no candidate");
            if (num_narrowed == 1 || mode == ECGMode::DBG_ONLY)
                return narrowed[0];

            // ── Level 3: Dynamic P-OPT tiebreak via rereference matrix ──
            if (graph_ctx_ && graph_ctx_->rereference.matrix) {
                uint32_t max_dist = 0;
                size_t victim = narrowed[0];
                for (size_t c = 0; c < num_narrowed; c++) {
                    uint32_t dist = graph_ctx_->findNextRef(set[narrowed[c]].line_addr);
                    if (dist > max_dist) {
                        max_dist = dist;
                        victim = narrowed[c];
                    }
                }
                return victim;
            }
            return narrowed[0];

        } else if (mode == ECGMode::POPT_TIE && graph_ctx_ && graph_ctx_->rereference.matrix) {
            // POPT_TIE: keep GRASP insertion/hit behavior, but use dynamic
            // P-OPT as the first tiebreak after SRRIP has selected max-RRPV
            // candidates. This is cheaper than full POPT_PRIMARY because it
            // only queries candidates that are already eligible for eviction.
            uint32_t max_dist = 0;
            uint32_t candidate_distances[64] = {};
            for (size_t c = 0; c < num_candidates; c++) {
                uint32_t dist = graph_ctx_->findNextRef(set[candidates[c]].line_addr);
                candidate_distances[c] = dist;
                if (dist > max_dist) max_dist = dist;
            }

            size_t narrowed[64];
            size_t num_narrowed = 0;
            for (size_t c = 0; c < num_candidates; c++) {
                if (candidate_distances[c] == max_dist)
                    narrowed[num_narrowed++] = candidates[c];
            }
            if (num_narrowed == 0)
                throw std::logic_error("P-OPT victim narrowing produced no candidate");
            if (num_narrowed == 1) return narrowed[0];

            uint8_t max_dbg = 0;
            size_t victim = narrowed[0];
            for (size_t c = 0; c < num_narrowed; c++) {
                if (set[narrowed[c]].ecg_dbg_tier > max_dbg) {
                    max_dbg = set[narrowed[c]].ecg_dbg_tier;
                    victim = narrowed[c];
                }
            }
            return victim;

        } else if (mode == ECGMode::ECG_EMBEDDED || mode == ECGMode::ECG_EPOCH_EMBEDDED) {
            // ECG_EMBEDDED: stored P-OPT hint as Level 2 (zero LLC overhead).
            // ECG_EPOCH_EMBEDDED: compact current-epoch P-OPT hint as Level 2.
            // Both fall back to DBG tier as Level 3 tiebreaker.
            uint8_t max_hint = 0;
            uint8_t hints[64] = {};
            uint32_t popt_max = 127;
            if (graph_ctx_ && graph_ctx_->mask_config.popt_bits > 0)
                popt_max = (1U << graph_ctx_->mask_config.popt_bits) - 1;
            for (size_t c = 0; c < num_candidates; c++) {
                size_t idx = candidates[c];
                if (mode == ECGMode::ECG_EPOCH_EMBEDDED && graph_ctx_ && graph_ctx_->rereference.matrix) {
                    uint32_t dist = std::min(graph_ctx_->findNextRef(set[idx].line_addr), uint32_t(127));
                    hints[c] = static_cast<uint8_t>((dist * popt_max) / 127);
                } else {
                    hints[c] = set[idx].ecg_popt_hint;
                }
                if (hints[c] > max_hint) max_hint = hints[c];
            }

            size_t narrowed[64];
            size_t num_narrowed = 0;
            for (size_t c = 0; c < num_candidates; c++)
                if (hints[c] == max_hint)
                    narrowed[num_narrowed++] = candidates[c];

            if (num_narrowed == 0)
                throw std::logic_error("ECG hint narrowing produced no candidate");
            if (num_narrowed == 1) return narrowed[0];

            // Level 3: DBG tier tiebreak among same-hint lines
            uint8_t max_dbg = 0;
            size_t victim = narrowed[0];
            for (size_t c = 0; c < num_narrowed; c++) {
                if (set[narrowed[c]].ecg_dbg_tier > max_dbg) {
                    max_dbg = set[narrowed[c]].ecg_dbg_tier;
                    victim = narrowed[c];
                }
            }
            return victim;

        } else {  // POPT_PRIMARY fallback (no matrix available)
            // Fall back to DBG tiebreak if no P-OPT matrix

            // No P-OPT matrix: fall back to DBG tiebreak
            uint8_t max_dbg = 0;
            size_t victim = candidates[0];
            for (size_t c = 0; c < num_candidates; c++) {
                if (set[candidates[c]].ecg_dbg_tier > max_dbg) {
                    max_dbg = set[candidates[c]].ecg_dbg_tier;
                    victim = candidates[c];
                }
            }
            return victim;
        }
    }

    std::string name_;
    size_t size_bytes_;
    size_t line_size_;
    size_t associativity_;
    size_t num_sets_;
    size_t offset_bits_;
    size_t index_bits_;
    bool power_of_two_sets_ = true;
    EvictionPolicy policy_;
    
    std::vector<std::vector<CacheLine>> cache_;
    CacheStats stats_;
    uint64_t global_time_ = 0;
    std::mt19937 rng_;
    std::mutex mutex_;
    std::unique_ptr<hawkeye_policy::State> hawkeye_state_;

    POPTState popt_state_;    // P-OPT rereference matrix state (legacy, used if no GraphCacheContext)
    GRASPState grasp_state_;  // GRASP degree-aware state (legacy, used if no GraphCacheContext)
    const GraphCacheContext* graph_ctx_ = nullptr;  // Unified graph-aware context (preferred)
    ECGMode ecg_mode_snapshot_ = ECGMode::DBG_PRIMARY;

    // ECG_GRASP_POPT online set dueling: five sampled leader arms
    // (RRIP-first, GRASP-only, epoch-first, degree-first, LRU) train a
    // phase-resetting winner used by follower sets.
    bool set_dueling_ = []() {
        const char* value = std::getenv("ECG_SET_DUELING");
        return value && value[0] && std::string(value) != "0";
    }();
    uint32_t dueling_set_offset_ = []() {
        const char* value = std::getenv("CACHE_ECG_DUELING_SET_OFFSET");
        return value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10) & 63u)
            : 0u;
    }();
    ecg_policy::OnlineDuelingSelector dueling_selector_;
    std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>
        dueling_leader_samples_by_arm_{};
    std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>
        dueling_winner_windows_by_arm_{};
    std::array<uint64_t, ecg_policy::DUEL_ARM_COUNT>
        dueling_follower_selections_by_arm_{};
    uint64_t dueling_completed_windows_ = 0;
    uint64_t dueling_winner_changes_ = 0;
    uint64_t reuse_admission_updates_ = 0;
    uint64_t ref32_governed_hits_ = 0;
    uint64_t ref32_governed_misses_ = 0;
    uint64_t governed_property_hits_ = 0;
    uint64_t governed_property_misses_ = 0;
    uint64_t ref32_dead_bypasses_ = 0;
    uint64_t ref32_dead_victims_ = 0;
    uint64_t ref32_non_property_victims_ = 0;
    uint64_t ref32_unknown_victims_ = 0;
    uint64_t ref32_finite_victims_ = 0;
    uint64_t ref32_prefetch_demand_displacements_ = 0;
    uint64_t ref32_prefetch_evictions_ = 0;
    ecg_policy::OnlineAdmissionSelector admission_selector_{[]() {
        const char* value =
            std::getenv("CACHE_ECG_ADMISSION_SET_OFFSET");
        return value
            ? static_cast<uint32_t>(
                std::strtoul(value, nullptr, 10) & 63u)
            : 0u;
    }()};
    std::array<uint64_t, ecg_policy::ADMIT_ARM_COUNT>
        admission_follower_selections_{};
    uint64_t admission_completed_windows_ = 0;
    uint64_t admission_winner_changes_ = 0;
    size_t evicting_set_idx_ = 0;    // set index of the in-progress eviction

    static bool reuseAdmissionEnabled() {
        static const bool enabled = ecg_policy::parseReuseAdmission(
            std::getenv("ECG_REUSE_ADMISSION"));
        return enabled;
    }

    static bool combinedReuseAdmissionEnabled() {
        static const bool enabled = ecg_policy::parseReuseAdmission(
            std::getenv("ECG_REUSE_ADMISSION_COMBINED"));
        return enabled;
    }

    static bool onlineAdmissionEnabled() {
        static const bool enabled = ecg_policy::parseReuseAdmission(
            std::getenv("ECG_REUSE_ADMISSION_ONLINE"));
        return enabled;
    }

    bool futureAdmissionForSet(size_t set_idx) const {
        return reuseAdmissionEnabled() ||
            (onlineAdmissionEnabled() &&
             admission_selector_.armForSet(set_idx) ==
                ecg_policy::ADMIT_FUTURE);
    }

    void recordAdmissionAccess(size_t set_idx, bool missed) {
        if (!onlineAdmissionEnabled() ||
            (name_ != "L3" && name_ != "L3-Shared"))
            return;
        const int leader = ecg_policy::admissionLeaderArm(
            set_idx, admission_selector_.offset());
        const uint8_t selected = admission_selector_.armForSet(set_idx);
        if (leader < 0 && selected < ecg_policy::ADMIT_ARM_COUNT)
            ++admission_follower_selections_[selected];
        const auto event = admission_selector_.recordAccess(set_idx, missed);
        if (event.completed_window) ++admission_completed_windows_;
        if (event.winner_changed) ++admission_winner_changes_;
    }

    uint32_t currentReuseEpoch() const {
        if (!graph_ctx_ || graph_ctx_->edge_epoch_count < 2)
            return 0;
        const uint32_t current =
            graph_ctx_->hints_for_thread().current_src;
        const uint32_t vertices = graph_ctx_->exact_nv
            ? graph_ctx_->exact_nv : graph_ctx_->topology.num_vertices;
        if (current == UINT32_MAX || vertices == 0) return 0;
        return static_cast<uint32_t>(
            (static_cast<uint64_t>(current) *
             graph_ctx_->edge_epoch_count) / vertices);
    }

    static uint16_t deliveredFirstReuseEpoch(const AccessHints& hints) {
        return hints.edge_epoch_sched_n > 0
            ? hints.edge_epoch_sched[0] : hints.edge_epoch;
    }

    size_t duelingSampleSetIndex(size_t set_idx) const {
        return set_idx + 64u - dueling_set_offset_;
    }
};

// ============================================================================
// Cache Hierarchy (L1 -> L2 -> L3)
// ============================================================================
class CacheHierarchy {
public:
    // Default: Intel-like hierarchy
    // L1: 32KB, 8-way, 64B lines
    // L2: 256KB, 4-way, 64B lines
    // L3: 8MB, 16-way, 64B lines
    CacheHierarchy(
        size_t l1_size = 32 * 1024,
        size_t l1_ways = 8,
        size_t l2_size = 256 * 1024,
        size_t l2_ways = 4,
        size_t l3_size = 8 * 1024 * 1024,
        size_t l3_ways = 16,
        size_t line_size = 64,
        EvictionPolicy policy = EvictionPolicy::LRU
    ) : CacheHierarchy(l1_size, l1_ways, l2_size, l2_ways,
                       l3_size, l3_ways, line_size,
                       policy, policy, policy) {
    }

    CacheHierarchy(
        size_t l1_size,
        size_t l1_ways,
        size_t l2_size,
        size_t l2_ways,
        size_t l3_size,
        size_t l3_ways,
        size_t line_size,
        EvictionPolicy l1_policy,
        EvictionPolicy l2_policy,
        EvictionPolicy l3_policy
    ) : line_size_(line_size), enabled_(true) {
        if (ref32_commit_channel_ && refresh_exact_stamp_) {
            throw std::invalid_argument(
                "ECG_REF32_COMMIT_CHANNEL and ECG_STORED_REFRESH are "
                "mutually exclusive");
        }
        l1_ = std::make_unique<CacheLevel>("L1", l1_size, line_size, l1_ways, l1_policy);
        l2_ = std::make_unique<CacheLevel>("L2", l2_size, line_size, l2_ways, l2_policy);
        l3_ = std::make_unique<CacheLevel>("L3", l3_size, line_size, l3_ways, l3_policy);
        if (ref32_commit_channel_) {
            std::cerr
                << "[ECG-REF32-COMMIT-CONFIG queue="
                << ref32_commit_queue_limit_
                << " latency=" << ref32_commit_latency_
                << " bandwidth=" << ref32_commit_bandwidth_
                << " tag_bits=48 deadline_bits="
                << ref32_commit_deadline_bits_ << " state_bits=2]\n";
        }
        if (ref32_prefetch_enabled_) {
            std::cerr
                << "[ECG-REF32-PREFETCH-CONFIG queue="
                << ref32_prefetch_queue_limit_
                << " latency=" << ref32_prefetch_latency_
                << " bandwidth=" << ref32_prefetch_bandwidth_
                << " issue_interval=" << ref32_prefetch_issue_interval_
                << " lookahead_records=16 placement=llc]\n";
        }
    }

    // Configure from environment variables
    static CacheHierarchy fromEnvironment() {
        size_t l1_size = getEnvSize("CACHE_L1_SIZE", 32 * 1024);
        size_t l1_ways = getEnvSize("CACHE_L1_WAYS", 8);
        size_t l2_size = getEnvSize("CACHE_L2_SIZE", 256 * 1024);
        size_t l2_ways = getEnvSize("CACHE_L2_WAYS", 4);
        size_t l3_size = getEnvSize("CACHE_L3_SIZE", 8 * 1024 * 1024);
        size_t l3_ways = getEnvSize("CACHE_L3_WAYS", 16);
        size_t line_size = getEnvSize("CACHE_LINE_SIZE", 64);
        
        EvictionPolicy policy = GetEnvPolicy("CACHE_POLICY", EvictionPolicy::LRU);
        EvictionPolicy l1_policy = GetEnvPolicy("CACHE_L1_POLICY", policy);
        EvictionPolicy l2_policy = GetEnvPolicy("CACHE_L2_POLICY", policy);
        EvictionPolicy l3_policy = GetEnvPolicy("CACHE_L3_POLICY", policy);
        
        return CacheHierarchy(l1_size, l1_ways, l2_size, l2_ways,
                     l3_size, l3_ways, line_size,
                     l1_policy, l2_policy, l3_policy);
    }

    // Main access function - simulates hierarchical access
    void access(uint64_t address, bool is_write = false) {
        if (!enabled_) return;

        const uint64_t line_addr = lineAddress(address);
        const bool ref32_record_request =
            isCurrentRef32RecordRequest(address);
        const bool ref32_commit_request =
            ref32_commit_channel_ && ref32_record_request;
        if (ref32_commit_request) {
            processRef32CommitUpdates(
                graph_ctx_->hints_for_thread().edge_ref_sequence);
        }
        if (ref32_prefetch_enabled_ && ref32_record_request) {
            const uint64_t sequence =
                graph_ctx_->hints_for_thread().edge_ref_sequence;
            processRef32Prefetches(sequence);
            completeLateRef32Prefetch(line_addr, sequence);
        }
        const bool was_prefetched = hasPrefetchedLine(line_addr);
        
        total_accesses_++;
        
        // ECG_EXACT_STORED: broadcast the per-edge hint to the LLC every demand
        // access so its stamp stays fresh even when L1/L2 serve the reference
        // (gated env, no-op for other policies). Under ECG_REFRESH_LLC_ONLY the
        // write is deferred to the L3-reaching path below (piggybacks a real L3
        // access = HW-free), instead of firing here on L1/L2 hits too.
        if (refresh_exact_stamp_ && !refresh_llc_only_) l3_->refreshExactStamp(address);

        // Structure-stream prefetcher.
        //
        // Two models, selected by CACHE_STREAM_PREFETCH_MODEL:
        //
        //   "stride" (default): an address-based next-line stream detector. It
        //   sees only addresses, exactly like hardware. A stream must be
        //   CONFIRMED by consecutive ascending line accesses within a 4 KiB
        //   region before it issues, it can and does mispredict, and it is
        //   bounded by a finite in-flight budget. It has no idea which
        //   addresses are structural, so it will happily train on a regular
        //   property access and waste fills on an irregular one.
        //
        //   "oracle": the legacy model, kept only as a labelled upper bound.
        //   It asks graph_ctx_->findRegion() whether an address is property
        //   data and refuses to prefetch it, so it never mispredicts on the
        //   distinction that matters, and it issues unconditionally with no
        //   MSHR, queue, lateness or bandwidth backpressure. A mechanism built
        //   so that it cannot be wrong cannot confirm a hypothesis; the frozen
        // reporting rules in wiki/Evaluation-Methodology.md make results that
        //   depend on it ineligible for performance claims.
        //
        // Both are applied identically to every policy.
        static const int stream_pf_degree = [](){
            const char* v = std::getenv("CACHE_STREAM_PREFETCH_DEGREE");
            int d = v ? std::atoi(v) : 0;
            return d < 0 ? 0 : (d > 32 ? 32 : d);
        }();
        if (stream_pf_degree > 0) {
            if (streamPrefetchOracle()) {
                if (graph_ctx_ && !graph_ctx_->findRegion(address)) {
                    for (int k = 1; k <= stream_pf_degree; k++) {
                        stride_pf_issued_++;
                        prefetch(address + (uint64_t)k * line_size_);
                    }
                }
            } else {
                issueStridePrefetch(address, stream_pf_degree);
            }
        }

        // Try L1
        if (l1_->access(address, is_write)) {
            // Hawkeye is LLC-only: a private-cache hit does not reach or retrain
            // LLC replacement state, matching the hardware visibility boundary.
            if (was_prefetched) markPrefetchUseful(line_addr);
            if (ref32_commit_request)
                enqueueRef32CommitUpdate(line_addr);
            if (ref32_prefetch_enabled_ && ref32_record_request)
                issueCurrentRef32Prefetch();
            return;  // L1 hit
        }
        
        // L1 miss, try L2
        if (l2_->access(address, is_write)) {
            if (was_prefetched) markPrefetchUseful(line_addr);
            l1_->insert(address, is_write);  // Bring to L1
            if (ref32_commit_request)
                enqueueRef32CommitUpdate(line_addr);
            if (ref32_prefetch_enabled_ && ref32_record_request)
                issueCurrentRef32Prefetch();
            return;  // L2 hit
        }
        
        // L2 miss, try L3
        if (l3_->access(address, is_write)) {
            // ECG_REFRESH_LLC_ONLY: the access reached L3, so stamping the epoch here
            // piggybacks an L3 access already in flight (HW-free). (L3-miss fills stamp
            // via insert() already, so the hit path is the only extra site needed.)
            if (refresh_exact_stamp_ && refresh_llc_only_) l3_->refreshExactStamp(address);
            if (was_prefetched) markPrefetchUseful(line_addr);
            l2_->insert(address, is_write);  // Bring to L2
            l1_->insert(address, is_write);  // Bring to L1
            if (ref32_prefetch_enabled_ && ref32_record_request)
                issueCurrentRef32Prefetch();
            return;  // L3 hit
        }
        
        // L3 miss - fetch from memory
        if (adaptive_flowthrough_) {
            placement_selector_.recordMiss(l3_->setIndexForAddress(address));
        }
        memory_accesses_++;
        if (was_prefetched) markPrefetchEvictedBeforeUse(line_addr);
        l3_->insert(address, is_write);
        l2_->insert(address, is_write);
        l1_->insert(address, is_write);
        if (ref32_prefetch_enabled_ && ref32_record_request)
            issueCurrentRef32Prefetch();
    }

    // ECG FlowThrough: an explicit non-temporal packed-edge request preserves
    // LLC hits but suppresses allocation after an LLC miss. L1/L2 fill normally.
    void accessStream(uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::cerr << "[ECG-FLOWTHROUGH sim=cache_sim active=1 adaptive="
                      << (adaptive_flowthrough_ ? 1 : 0) << "]\n";
        }
        accessNonTemporal(address, is_write, /*adaptive_placement=*/true);
    }

    void accessStructuralStream(uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        structural_flowthrough_accesses_++;
        accessNonTemporal(address, is_write, /*adaptive_placement=*/false);
    }

    // Shared non-temporal core. Kept separate from accessStream so P-OPT's
    // rereference-matrix column stream can reuse the identical accounting
    // instead of duplicating it; only the announcement differs.
    void accessNonTemporal(
            uint64_t address, bool is_write = false,
            bool adaptive_placement = true) {
        if (!enabled_) return;
        const uint64_t line_addr = lineAddress(address);
        const bool was_prefetched = hasPrefetchedLine(line_addr);
        static const int stream_pf_degree = [](){
            const char* value = std::getenv("CACHE_STREAM_PREFETCH_DEGREE");
            int degree = value ? std::atoi(value) : 0;
            return degree < 0 ? 0 : (degree > 32 ? 32 : degree);
        }();
        // Route through the SAME detector as ordinary accesses. This path
        // carries ReusePlan's per-edge records and P-OPT's simulated matrix columns,
        // i.e. exactly the metadata streams the ReusePlan-versus-P-OPT comparison
        // turns on. Leaving it on an unconditional issue loop meant the
        // address-only prefetcher did not reach the streams it was written
        // for, and both policies kept oracle-quality coverage of their
        // metadata whatever CACHE_STREAM_PREFETCH_MODEL said.
        if (stream_pf_degree > 0) {
            if (streamPrefetchOracle()) {
                for (int k = 1; k <= stream_pf_degree; ++k) {
                    stride_pf_issued_++;
                    prefetchStream(
                        address + static_cast<uint64_t>(k) * line_size_,
                        adaptive_placement);
                }
            } else {
                issueStridePrefetch(
                    address, stream_pf_degree, /*non_temporal=*/true,
                    adaptive_placement);
            }
        }

        total_accesses_++;
        if (l1_->access(address, is_write)) {
            if (was_prefetched) markPrefetchUseful(line_addr);
            return;
        }
        if (l2_->access(address, is_write)) {
            if (was_prefetched) markPrefetchUseful(line_addr);
            l1_->insert(address, is_write);
            return;
        }
        const bool adaptive =
            adaptive_placement && adaptive_flowthrough_;
        topt::RequestClassScope request_class(/*bypass=*/!adaptive);
        if (l3_->access(address, is_write)) {
            if (was_prefetched) markPrefetchUseful(line_addr);
            l2_->insert(address, is_write);
            l1_->insert(address, is_write);
            return;
        }
        const size_t set_index = l3_->setIndexForAddress(address);
        if (adaptive) {
            placement_selector_.recordMiss(set_index);
        }
        const bool flowthrough = !adaptive ||
            placement_selector_.shouldFlowThrough(set_index);
        // LLC miss: the static arm applies FlowThrough; the adaptive allocate arm retains
        // the record so reused streams can opt out of FlowThrough.
        memory_accesses_++;
        if (was_prefetched) markPrefetchEvictedBeforeUse(line_addr);
        if (!flowthrough) l3_->insert(address, is_write);
        l2_->insert(address, is_write);
        l1_->insert(address, is_write);
    }

    // FlowThrough prefetch: warm private caches without allocating the
    // one-touch record in LLC. Existing L3 data may still be promoted downward.
    void prefetchStream(
            uint64_t address, bool adaptive_placement = true) {
        if (!enabled_) return;
        const uint64_t line_addr = lineAddress(address);
        prefetch_requests_++;
        recordPrefetchTranslation(address);
        if (l1_->contains(address)) {
            prefetch_cache_hits_++;
            return;
        }
        if (l2_->contains(address)) {
            prefetch_cache_hits_++;
            l1_->insert(address, false, true);
            return;
        }
        if (l3_->contains(address)) {
            prefetch_cache_hits_++;
            l3_->updatePrefetchHit(address);
            l2_->insert(address, false, true);
            l1_->insert(address, false, true);
            return;
        }
        const size_t set_index = l3_->setIndexForAddress(address);
        const bool adaptive =
            adaptive_placement && adaptive_flowthrough_;
        if (adaptive) {
            placement_selector_.recordMiss(set_index);
        }
        const bool flowthrough = !adaptive ||
            placement_selector_.shouldFlowThrough(set_index);
        prefetch_fills_++;
        markPrefetchFill(line_addr);
        if (!flowthrough) l3_->insert(address, false, true);
        l2_->insert(address, false, true);
        l1_->insert(address, false, true);
    }

    // Prefetch: bring data into cache without counting as a demand access.
    // In real hardware, prefetches are non-blocking fills that don't
    // appear in demand miss statistics. Only demand accesses count.
    //
    // Prefetch hits: data already in cache — no action needed.
    // Prefetch misses: fill cache from memory but do NOT increment
    //   total_accesses_ or memory_accesses_.
    void prefetch(uint64_t address) {
        if (!enabled_) return;

        const uint64_t line_addr = lineAddress(address);
        prefetch_requests_++;
        recordPrefetchTranslation(address);
        
        // Check if already in cache (any level) using a NON-counting probe.
        // Using access() here would register the probe as a demand hit/miss and
        // mutate LRU state — an avoided demand miss would be cancelled by the
        // probe miss, making prefetch a no-op for the miss rate (and inflating
        // hit counts for already-cached probes). contains() avoids both.
        if (l1_->contains(address)) {
            prefetch_cache_hits_++;
            return;
        }
        if (l2_->contains(address)) {
            prefetch_cache_hits_++;
            l1_->insert(address, false, true);
            return;
        }
        if (l3_->contains(address)) {
            prefetch_cache_hits_++;
            l3_->updatePrefetchHit(address);
            l2_->insert(address, false, true);
            l1_->insert(address, false, true);
            return;
        }
        
        // Not in cache — fetch from memory into hierarchy
        // Does NOT increment demand counters
        prefetch_fills_++;
        markPrefetchFill(line_addr);
        l3_->insert(address, false, true);
        l2_->insert(address, false, true);
        l1_->insert(address, false, true);
    }

    // Convenience methods for common access patterns
    template<typename T>
    void read(const T* ptr) {
        access(reinterpret_cast<uint64_t>(ptr), false);
    }

    template<typename T>
    void write(T* ptr) {
        access(reinterpret_cast<uint64_t>(ptr), true);
    }

    // Read an array element
    template<typename T>
    void readArray(const T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), false);
    }

    // Write an array element
    template<typename T>
    void writeArray(T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), true);
    }

    // Read a range (e.g., CSR row)
    template<typename T>
    void readRange(const T* arr, size_t start, size_t end) {
        for (size_t i = start; i < end; i++) {
            // Only access each cache line once
            uint64_t addr = reinterpret_cast<uint64_t>(&arr[i]);
            access(addr, false);
        }
    }

    // Reset all statistics
    void resetStats() {
        if (topt::enabled) topt::mark_roi_start();
        l1_->resetStats();
        l2_->resetStats();
        l3_->resetStats();
        total_accesses_ = 0;
        memory_accesses_ = 0;
        structural_flowthrough_accesses_ = 0;
        prefetch_requests_ = 0;
        prefetch_cache_hits_ = 0;
        prefetch_fills_ = 0;
        prefetch_useful_ = 0;
        prefetch_evicted_before_use_ = 0;
        pfx_pages_4k_.clear();
        pfx_pages_2m_.clear();
        pfx_mtlb_lru_.clear();
        pfx_mtlb_pos_.clear();
        pfx_mtlb_misses_ = 0;
        ref32_commit_updates_.clear();
        ref32_commit_generated_ = 0;
        ref32_commit_coalesced_ = 0;
        ref32_commit_queue_dropped_ = 0;
        ref32_commit_applied_ = 0;
        ref32_commit_not_resident_ = 0;
        ref32_commit_expired_ = 0;
        ref32_commit_bandwidth_deferred_ = 0;
        ref32_commit_max_occupancy_ = 0;
        ref32_commit_last_sequence_ =
            std::numeric_limits<uint64_t>::max();
        ref32_prefetch_updates_.clear();
        ref32_prefetch_actions_seen_ = 0;
        ref32_prefetch_rate_limited_ = 0;
        ref32_prefetch_resident_duplicates_ = 0;
        ref32_prefetch_pending_duplicates_ = 0;
        ref32_prefetch_admission_dropped_ = 0;
        ref32_prefetch_queue_dropped_ = 0;
        ref32_prefetch_requests_issued_ = 0;
        ref32_prefetch_fills_completed_ = 0;
        ref32_prefetch_late_merged_ = 0;
        ref32_prefetch_completion_resident_ = 0;
        ref32_prefetch_completion_admission_dropped_ = 0;
        ref32_prefetch_bandwidth_deferred_ = 0;
        ref32_prefetch_max_occupancy_ = 0;
        ref32_prefetch_last_issue_sequence_ =
            std::numeric_limits<uint64_t>::max();
        ref32_prefetch_last_process_sequence_ =
            std::numeric_limits<uint64_t>::max();
        ref32_resource_snapshot_valid_ = false;
        ref32_resource_deployable_snapshot_ = false;
        ref32_resource_line_count_snapshot_ = 0;
        ref32_resource_popt_bits_snapshot_ = 0;
        ref32_resource_total_bits_snapshot_ = 0;
        // Reset alongside the demand/fill counters, or the prefetcher counts
        // would span the pre-ROI warm replay while everything else is ROI-only.
        stride_pf_issued_ = 0;
        stride_pf_throttled_ = 0;
        stride_pf_untrained_ = 0;
        popt_stream_lines_ = 0;
        popt_stream_columns_ = 0;
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        prefetched_lines_.clear();
    }

    void flushRef32CommitUpdates() {
        if (!ref32ResourcesActive())
            return;
        captureRef32ResourceSnapshot();
        uint64_t sequence = graph_ctx_
            ? graph_ctx_->hints_for_thread().edge_ref_sequence : 0;
        if (ref32_commit_channel_) {
            while (!ref32_commit_updates_.empty()) {
                sequence = std::max<uint64_t>(
                    sequence + 1,
                    ref32_commit_updates_.front().ready_sequence);
                processRef32CommitUpdates(sequence, true);
            }
            std::cerr
                << "[ECG-REF32-COMMIT queue=" << ref32_commit_queue_limit_
                << " latency=" << ref32_commit_latency_
                << " bandwidth=" << ref32_commit_bandwidth_
                << " tag_bits=48 deadline_bits="
                << ref32_commit_deadline_bits_ << " state_bits=2"
                << " generated=" << ref32_commit_generated_
                << " coalesced=" << ref32_commit_coalesced_
                << " queue_dropped=" << ref32_commit_queue_dropped_
                << " applied=" << ref32_commit_applied_
                << " not_resident=" << ref32_commit_not_resident_
                << " expired=" << ref32_commit_expired_
                << " bandwidth_deferred=" << ref32_commit_bandwidth_deferred_
                << " max_occupancy=" << ref32_commit_max_occupancy_
                << " pending=" << ref32_commit_updates_.size()
                << "]\n";
        }
        if (ref32DeployableResourcesActive()) {
            std::cerr
                << "[ECG-REF32-RESOURCES line_bits="
                << ref32ResourceLineBits() << " lines="
                << ref32ResourceLineCount()
                << " line_state_bits=" << ref32LineStateBits()
                << " commit_entries="
                << (ref32_commit_channel_ ? ref32_commit_queue_limit_ : 0)
                << " commit_entry_bits=" << ref32CommitEntryBits()
                << " prefetch_entries="
                << (ref32_prefetch_enabled_
                    ? ref32_prefetch_queue_limit_ : 0)
                << " prefetch_entry_bits=" << ref32PrefetchEntryBits()
                << " lookahead_records="
                << (ref32_prefetch_enabled_ ? 16 : 0)
                << " lookahead_bits="
                << (ref32_prefetch_enabled_ ? 512 : 0)
                << " control_bits=64 record_extra_bits=0 total_bits="
                << ref32TotalResourceBits()
                << " popt_matrix_bits=" << ref32PoptMatrixBits()
                << " reduction_x=" << std::fixed << std::setprecision(3)
                << ref32ResourceReduction()
                << std::defaultfloat << "]\n";
        }
        if (ref32_prefetch_enabled_) {
            while (!ref32_prefetch_updates_.empty()) {
                sequence = std::max<uint64_t>(
                    sequence + 1,
                    ref32_prefetch_updates_.front().ready_sequence);
                processRef32Prefetches(sequence, true);
            }
            std::cerr
                << "[ECG-REF32-PREFETCH queue="
                << ref32_prefetch_queue_limit_
                << " latency=" << ref32_prefetch_latency_
                << " bandwidth=" << ref32_prefetch_bandwidth_
                << " issue_interval=" << ref32_prefetch_issue_interval_
                << " lookahead_records=16 placement=llc"
                << " actions_seen=" << ref32_prefetch_actions_seen_
                << " rate_limited=" << ref32_prefetch_rate_limited_
                << " resident_duplicates="
                << ref32_prefetch_resident_duplicates_
                << " pending_duplicates="
                << ref32_prefetch_pending_duplicates_
                << " admission_dropped="
                << ref32_prefetch_admission_dropped_
                << " queue_dropped=" << ref32_prefetch_queue_dropped_
                << " requests_issued=" << ref32_prefetch_requests_issued_
                << " fills_completed=" << ref32_prefetch_fills_completed_
                << " late_merged=" << ref32_prefetch_late_merged_
                << " completion_resident="
                << ref32_prefetch_completion_resident_
                << " completion_admission_dropped="
                << ref32_prefetch_completion_admission_dropped_
                << " bandwidth_deferred="
                << ref32_prefetch_bandwidth_deferred_
                << " max_occupancy=" << ref32_prefetch_max_occupancy_
                << " demand_displacements="
                << l3_->getRef32PrefetchDemandDisplacements()
                << " evicted_before_use="
                << prefetch_evicted_before_use_.load()
                << " pending=" << ref32_prefetch_updates_.size()
                << "]\n";
        }
    }

    // Enable/disable simulation
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Get cache levels
    CacheLevel* L1() { return l1_.get(); }
    CacheLevel* L2() { return l2_.get(); }
    CacheLevel* L3() { return l3_.get(); }

    // ================================================================
    // Unified GraphCacheContext (preferred over legacy init methods)
    // ================================================================
    void initGraphContext(const GraphCacheContext* ctx) {
        graph_ctx_ = ctx;
        l1_->initGraphContext(ctx);
        l2_->initGraphContext(ctx);
        l3_->initGraphContext(ctx);
    }

    // P-OPT: Initialize rereference matrix on LLC (L3) — legacy API
    void initPOPT(const uint8_t* reref_matrix, uint64_t irreg_base,
                  uint64_t irreg_bound, uint32_t num_vertices,
                  uint32_t num_epochs = 256) {
        l3_->initPOPT(reref_matrix, irreg_base, irreg_bound, num_vertices, num_epochs);
    }

    // GRASP: Initialize degree-aware RRIP retention — legacy API
    void initGRASP(uint64_t data_ptr, uint32_t num_vertices,
                   size_t elem_size, double hot_fraction = 0.5) {
        size_t llc_size = l3_->getSizeBytes();
        l1_->initGRASP(data_ptr, num_vertices, elem_size, l1_->getSizeBytes(), hot_fraction);
        l2_->initGRASP(data_ptr, num_vertices, elem_size, l2_->getSizeBytes(), hot_fraction);
        l3_->initGRASP(data_ptr, num_vertices, elem_size, llc_size, hot_fraction);
    }

    // P-OPT: Update current vertex (call at each outer-loop iteration)
    void setCurrentVertex(uint32_t vertex_id) {
        streamPoptMatrixIfEpochAdvanced(vertex_id);
        l3_->setCurrentVertex(vertex_id);
    }

    // ------------------------------------------------------------------
    // P-OPT rereference-matrix column stream
    // ------------------------------------------------------------------
    // Balaji and Lucia keep the current and next rereference-matrix
    // columns resident in reserved LLC ways and stream in a fresh column at
    // every epoch boundary. cache_sim consults the matrix host-side, so that
    // stream previously existed only as a flat analytic charge added to the
    // miss count after the run. ReusePlan's per-edge records, by contrast, are real
    // simulated accesses, so a structure prefetcher covered ReusePlan's sequential
    // stream while no prefetcher could ever cover P-OPT's. Both are sequential
    // streams; pricing only one of them through the hierarchy systematically
    // favours ReusePlan. This issues the column stream as real accesses so the two are
    // accounted symmetrically.
    //
    // The stream is non-temporal because the resident columns live in the
    // reserved ways, and those ways are already deducted from the simulated
    // cache geometry. Allocating them again in the modelled cache would charge
    // P-OPT for the same capacity twice.
    //
    // The synthetic column buffer sits outside every registered property
    // region. Note the mechanism: accessNonTemporal() issues stream prefetches
    // unconditionally rather than consulting findRegion(), because both callers
    // are structural streams by construction. The effect is the intended one --
    // the column stream is prefetch-covered exactly as ReusePlan's records are -- but
    // it is not the region classifier that makes it so.
    //
    // Fidelity boundary: published P-OPT streams columns with a dedicated
    // engine that writes into the reserved ways, and is evaluated with
    // conventional prefetching disabled. Modelling the stream on the ordinary
    // demand path is what lets a CPU prefetcher cover it here, so results about
    // that coverage describe our accounting, not P-OPT hardware.
    void initPoptMatrixStream(uint32_t column_bytes, uint32_t epoch_size,
                              uint32_t num_epochs) {
        if (column_bytes == 0 || epoch_size == 0 || num_epochs == 0) return;
        popt_stream_column_bytes_ = column_bytes;
        // Round the per-epoch backing stride up to a line so distinct epochs
        // never share a cache line.
        popt_stream_column_stride_ =
            ((column_bytes + line_size_ - 1) / line_size_) * line_size_;
        popt_stream_epoch_size_ = epoch_size;
        popt_stream_num_epochs_ = num_epochs;
        popt_stream_resident_[0] = UINT32_MAX;
        popt_stream_resident_[1] = UINT32_MAX;
        popt_stream_next_slot_ = 0;
        popt_stream_enabled_ = true;
        std::cerr << "[POPT-MATRIX-STREAM sim=cache_sim active=1 column_bytes="
                  << column_bytes << " epoch_size=" << epoch_size
                  << " epochs=" << num_epochs << "]\n";
    }

    uint64_t getPoptMatrixStreamLines() const { return popt_stream_lines_; }

    // ------------------------------------------------------------------
    // Address-only structure-stream prefetcher
    // ------------------------------------------------------------------
    // Address-only stream detection, so it can mispredict, plus a finite
    // in-flight budget, so it cannot issue without limit. This replaces an
    // oracle model that asked the graph context whether an address was
    // property data and therefore never mispredicted the one distinction the
    // experiment turned on.
    static bool streamPrefetchOracle() {
        static const bool oracle = [](){
            const char* v = std::getenv("CACHE_STREAM_PREFETCH_MODEL");
            return v && std::string(v) == "oracle";
        }();
        return oracle;
    }

    uint64_t getStreamPrefetchIssued() const { return stride_pf_issued_; }
    uint64_t getStreamPrefetchThrottled() const { return stride_pf_throttled_; }
    uint64_t getStreamPrefetchUntrained() const { return stride_pf_untrained_; }

private:
    // Confirmations required before a detected stream is trusted. One ascending
    // neighbour could be coincidence; hardware stream prefetchers likewise wait.
    static constexpr int kStreamConfirmations = 2;
    // 4 KiB detection regions, as a hardware stream detector would use, so a
    // stream cannot be trained across an unrelated allocation.
    static constexpr uint64_t kStreamRegionShift = 12;
    static constexpr size_t kStreamTableEntries = 32;

    struct StreamEntry {
        uint64_t region = UINT64_MAX;
        uint64_t last_line = 0;
        int confirmations = 0;
    };

    static int streamPrefetchMaxInFlight() {
        static const int limit = [](){
            const char* v = std::getenv("CACHE_STREAM_PREFETCH_MAX_INFLIGHT");
            int n = v ? std::atoi(v) : 32;   // MSHR-scale default
            return n < 1 ? 1 : n;
        }();
        return limit;
    }

    void issueStridePrefetch(
            uint64_t address, int degree, bool non_temporal = false,
            bool adaptive_placement = true) {
        const uint64_t line = address / line_size_;
        const uint64_t region = address >> kStreamRegionShift;
        StreamEntry& e = stride_table_[region % kStreamTableEntries];
        if (e.region != region) {
            // New or evicted stream: start training, issue nothing.
            e.region = region;
            e.last_line = line;
            e.confirmations = 0;
            stride_pf_untrained_++;
            return;
        }
        if (line == e.last_line) return;            // same line, no new information
        if (line == e.last_line + 1) {
            if (e.confirmations < kStreamConfirmations) e.confirmations++;
        } else {
            // Any non-sequential step breaks the stream. An irregular property
            // access lands here, which is exactly the mispredict the oracle
            // model could not make.
            e.confirmations = 0;
        }
        e.last_line = line;
        if (e.confirmations < kStreamConfirmations) {
            stride_pf_untrained_++;
            return;
        }
        for (int k = 1; k <= degree; k++) {
            if (getPrefetchPending() >= (uint64_t)streamPrefetchMaxInFlight()) {
                stride_pf_throttled_++;
                break;
            }
            const uint64_t target = address + (uint64_t)k * line_size_;
            // Stop at the detection-region boundary. The detector cannot have
            // trained across it, and crossing it in hardware would need a
            // translation the model does not perform.
            if ((target >> kStreamRegionShift) != region) break;
            stride_pf_issued_++;
            if (non_temporal)
                prefetchStream(target, adaptive_placement);
            else prefetch(target);
        }
    }

    StreamEntry stride_table_[kStreamTableEntries];
    uint64_t stride_pf_issued_ = 0;
    uint64_t stride_pf_throttled_ = 0;
    uint64_t stride_pf_untrained_ = 0;

public:

private:
    // Enabled by POPT_MATRIX_STREAM_SIM=1 and configured from the registered
    // rereference matrix, so no kernel needs to know about it and all five
    // kernels behave identically.
    void initPoptMatrixStreamFromContext() {
        popt_stream_checked_ = true;
        static const bool requested = [](){
            const char* v = std::getenv("POPT_MATRIX_STREAM_SIM");
            return v && std::atoi(v) != 0;
        }();
        if (!requested || !graph_ctx_) return;
        const auto& r = graph_ctx_->rereference;
        if (r.matrix == nullptr || r.num_cache_lines == 0 || r.epoch_size == 0)
            return;
        // One matrix entry per cache line using the reference 1-byte encoding.
        initPoptMatrixStream(r.num_cache_lines, r.epoch_size, r.num_epochs);
    }

    void streamPoptMatrixIfEpochAdvanced(uint32_t vertex_id) {
        if (!popt_stream_checked_) initPoptMatrixStreamFromContext();
        if (!popt_stream_enabled_ || popt_stream_epoch_size_ == 0) return;
        const uint32_t epoch = vertex_id / popt_stream_epoch_size_;
        if (epoch >= popt_stream_num_epochs_) return;
        // Two-column residency for the current and next columns.
        // Charging on "the epoch advanced" is wrong: a multi-iteration kernel
        // sweeps epochs 0..N-1 once per iteration and must pay for every sweep,
        // while a frontier kernel that oscillates across one boundary must not
        // pay twice for a column the hardware still holds. Residency answers
        // both. An earlier forward-progress-only rule silently charged
        // PageRank for a single sweep no matter how many iterations it ran.
        if (popt_stream_resident_[0] == epoch || popt_stream_resident_[1] == epoch)
            return;
        popt_stream_resident_[popt_stream_next_slot_] = epoch;
        popt_stream_next_slot_ ^= 1;
        popt_stream_columns_++;
        // Distinct backing address per epoch, so a column can never hit on a
        // stale line left by a different column that happened to share a slot.
        const uint64_t base = kPoptStreamBase +
            static_cast<uint64_t>(epoch) * popt_stream_column_stride_;
        for (uint64_t off = 0; off < popt_stream_column_bytes_;
             off += line_size_) {
            popt_stream_lines_++;
            accessNonTemporal(base + off, false);
        }
    }

    // Well outside any graph allocation, so it can never alias a registered
    // property region.
    static constexpr uint64_t kPoptStreamBase = 0x7000000000000ULL;
    bool popt_stream_enabled_ = false;
    bool popt_stream_checked_ = false;
    uint32_t popt_stream_column_bytes_ = 0;
    uint64_t popt_stream_column_stride_ = 0;
    uint32_t popt_stream_epoch_size_ = 0;
    uint32_t popt_stream_num_epochs_ = 0;
    uint32_t popt_stream_resident_[2] = {UINT32_MAX, UINT32_MAX};
    unsigned popt_stream_next_slot_ = 0;
    uint64_t popt_stream_lines_ = 0;
    uint64_t popt_stream_columns_ = 0;

public:

    uint64_t getTotalAccesses() const { return total_accesses_; }
    uint64_t getMemoryAccesses() const { return memory_accesses_; }
    uint64_t getStructuralFlowThroughAccesses() const {
        return structural_flowthrough_accesses_;
    }
    uint64_t getPrefetchRequests() const { return prefetch_requests_; }
    uint64_t getPrefetchCacheHits() const { return prefetch_cache_hits_; }
    uint64_t getPrefetchFills() const { return prefetch_fills_; }
    uint64_t getPrefetchUseful() const { return prefetch_useful_; }
    uint64_t getPrefetchEvictedBeforeUse() const { return prefetch_evicted_before_use_; }
    uint64_t getPrefetchPending() const {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        return prefetched_lines_.size();
    }
    // Off-chip READ traffic only: demand fills plus prefetch fills. This
    // deliberately excludes dirty writebacks, which are a separate direction of
    // memory-controller traffic; see getTotalOffChipTraffic().
    uint64_t getTotalMemoryTraffic() const { return memory_accesses_ + prefetch_fills_; }

    // Dirty LLC evictions written back to memory. A policy that retains dirty
    // lines longer, or that changes which lines are resident at all, changes
    // this independently of the read stream, so a read-only total can rank
    // write-heavy kernels such as PageRank and CC incorrectly.
    uint64_t getWritebackTraffic() const { return l3_->getStats().writebacks.load(); }

    // The frozen primary metric: memory-controller lines in BOTH directions.
    uint64_t getTotalOffChipTraffic() const {
        return getTotalMemoryTraffic() + getWritebackTraffic();
    }

    // Print statistics
    void printStats(std::ostream& os = std::cout) const {
        os << "\n";
        os << "╔══════════════════════════════════════════════════════════════════╗\n";
        os << "║                    CACHE SIMULATION RESULTS                      ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        
        printLevelStats(os, *l1_);
        printLevelStats(os, *l2_);
        printLevelStats(os, *l3_);
        
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ SUMMARY                                                          ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ Total Accesses:      " << std::setw(15) << total_accesses_ 
           << "                          ║\n";
        os << "║ Memory Accesses:     " << std::setw(15) << memory_accesses_
           << "                          ║\n";
          os << "║ Prefetch Requests:   " << std::setw(15) << prefetch_requests_
              << "                          ║\n";
          os << "║ Prefetch Fills:      " << std::setw(15) << prefetch_fills_
              << "                          ║\n";
          os << "║ Useful Prefetches:   " << std::setw(15) << prefetch_useful_
              << "                          ║\n";
          os << "║ Total Memory Traffic:" << std::setw(15) << getTotalMemoryTraffic()
              << "                          ║\n";
        os << "║ Overall Hit Rate:    " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (1.0 - (double)memory_accesses_ / total_accesses_)
           << "%                          ║\n";
        os << "╚══════════════════════════════════════════════════════════════════╝\n";
    }

    // Export statistics as JSON
    std::string toJSON() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"total_accesses\": " << total_accesses_ << ",\n";
        ss << "  \"memory_accesses\": " << memory_accesses_ << ",\n";
        ss << "  \"prefetch_requests\": " << prefetch_requests_ << ",\n";
        ss << "  \"prefetch_cache_hits\": " << prefetch_cache_hits_ << ",\n";
        ss << "  \"prefetch_fills\": " << prefetch_fills_ << ",\n";
        ss << "  \"prefetch_useful\": " << prefetch_useful_ << ",\n";
        ss << "  \"prefetch_evicted_before_use\": " << prefetch_evicted_before_use_ << ",\n";
        ss << "  \"prefetch_distinct_pages_4k\": " << pfx_pages_4k_.size() << ",\n";
        ss << "  \"prefetch_distinct_pages_2m\": " << pfx_pages_2m_.size() << ",\n";
        ss << "  \"prefetch_mtlb_entries\": " << pfx_mtlb_size_ << ",\n";
        ss << "  \"prefetch_mtlb_misses\": " << pfx_mtlb_misses_ << ",\n";
        ss << "  \"prefetch_pending\": " << getPrefetchPending() << ",\n";
        ss << "  \"total_memory_traffic\": " << getTotalMemoryTraffic() << ",\n";
        ss << "  \"structural_flowthrough_accesses\": "
           << getStructuralFlowThroughAccesses() << ",\n";
        ss << "  \"llc_writebacks\": " << getWritebackTraffic() << ",\n";
        ss << "  \"total_offchip_traffic\": " << getTotalOffChipTraffic() << ",\n";
        ss << "  \"ecg_mode_effective\": \"" << l3_->getEcgMode()
           << "\",\n";
        ss << "  \"ecg_dueling_set_offset\": "
           << l3_->getDuelingSetOffset() << ",\n";
        ss << "  \"ecg_dueling_final_winner_arm\": "
           << static_cast<unsigned>(l3_->getDuelingWinnerArm()) << ",\n";
        ss << "  \"ecg_dueling_completed_windows\": "
           << l3_->getDuelingCompletedWindows() << ",\n";
        ss << "  \"ecg_dueling_winner_changes\": "
           << l3_->getDuelingWinnerChanges() << ",\n";
        ss << "  \"ecg_reuse_admission_updates\": "
           << l3_->getReuseAdmissionUpdates() << ",\n";
        ss << "  \"ecg_ref32_governed_hits\": "
           << l3_->getRef32GovernedHits() << ",\n";
        ss << "  \"ecg_ref32_governed_misses\": "
           << l3_->getRef32GovernedMisses() << ",\n";
        ss << "  \"governed_property_hits\": "
           << l3_->getGovernedPropertyHits() << ",\n";
        ss << "  \"governed_property_misses\": "
           << l3_->getGovernedPropertyMisses() << ",\n";
        ss << "  \"ecg_ref32_dead_bypasses\": "
           << l3_->getRef32DeadBypasses() << ",\n";
        ss << "  \"ecg_ref32_dead_victims\": "
           << l3_->getRef32DeadVictims() << ",\n";
        ss << "  \"ecg_ref32_non_property_victims\": "
           << l3_->getRef32NonPropertyVictims() << ",\n";
        ss << "  \"ecg_ref32_unknown_victims\": "
           << l3_->getRef32UnknownVictims() << ",\n";
        ss << "  \"ecg_ref32_finite_victims\": "
           << l3_->getRef32FiniteVictims() << ",\n";
        ss << "  \"ecg_ref32_commit_generated\": "
           << ref32_commit_generated_ << ",\n";
        ss << "  \"ecg_ref32_commit_coalesced\": "
           << ref32_commit_coalesced_ << ",\n";
        ss << "  \"ecg_ref32_commit_queue_dropped\": "
           << ref32_commit_queue_dropped_ << ",\n";
        ss << "  \"ecg_ref32_commit_applied\": "
           << ref32_commit_applied_ << ",\n";
        ss << "  \"ecg_ref32_commit_not_resident\": "
           << ref32_commit_not_resident_ << ",\n";
        ss << "  \"ecg_ref32_commit_expired\": "
           << ref32_commit_expired_ << ",\n";
        ss << "  \"ecg_ref32_commit_bandwidth_deferred\": "
           << ref32_commit_bandwidth_deferred_ << ",\n";
        ss << "  \"ecg_ref32_commit_max_occupancy\": "
           << ref32_commit_max_occupancy_ << ",\n";
        ss << "  \"ecg_ref32_commit_pending\": "
           << ref32_commit_updates_.size() << ",\n";
        ss << "  \"ecg_ref32_prefetch_actions_seen\": "
           << ref32_prefetch_actions_seen_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_rate_limited\": "
           << ref32_prefetch_rate_limited_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_resident_duplicates\": "
           << ref32_prefetch_resident_duplicates_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_pending_duplicates\": "
           << ref32_prefetch_pending_duplicates_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_admission_dropped\": "
           << ref32_prefetch_admission_dropped_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_queue_dropped\": "
           << ref32_prefetch_queue_dropped_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_requests_issued\": "
           << ref32_prefetch_requests_issued_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_fills_completed\": "
           << ref32_prefetch_fills_completed_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_late_merged\": "
           << ref32_prefetch_late_merged_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_completion_resident\": "
           << ref32_prefetch_completion_resident_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_completion_admission_dropped\": "
           << ref32_prefetch_completion_admission_dropped_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_bandwidth_deferred\": "
           << ref32_prefetch_bandwidth_deferred_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_max_occupancy\": "
           << ref32_prefetch_max_occupancy_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_demand_displacements\": "
           << l3_->getRef32PrefetchDemandDisplacements() << ",\n";
        ss << "  \"ecg_ref32_prefetch_evicted_before_use\": "
           << prefetch_evicted_before_use_ << ",\n";
        ss << "  \"ecg_ref32_prefetch_pending\": "
           << ref32_prefetch_updates_.size() << ",\n";
        ss << "  \"ecg_ref32_resource_line_bits\": "
           << (ref32DeployableResourcesActive()
               ? ref32ResourceLineBits() : 0) << ",\n";
        ss << "  \"ecg_ref32_resource_lines\": "
           << ref32ResourceLineCount() << ",\n";
        ss << "  \"ecg_ref32_resource_line_state_bits\": "
           << ref32LineStateBits() << ",\n";
        ss << "  \"ecg_ref32_resource_commit_queue_bits\": "
           << (ref32DeployableResourcesActive() &&
               ref32_commit_channel_
               ? ref32_commit_queue_limit_ *
                    ref32CommitEntryBits() : 0u) << ",\n";
        ss << "  \"ecg_ref32_resource_prefetch_queue_bits\": "
           << (ref32DeployableResourcesActive() &&
               ref32_prefetch_enabled_
               ? ref32_prefetch_queue_limit_ *
                    ref32PrefetchEntryBits() : 0u) << ",\n";
        ss << "  \"ecg_ref32_resource_lookahead_bits\": "
           << (ref32DeployableResourcesActive() &&
               ref32_prefetch_enabled_ ? 512 : 0) << ",\n";
        ss << "  \"ecg_ref32_resource_control_bits\": "
           << (ref32DeployableResourcesActive() ? 64 : 0) << ",\n";
        ss << "  \"ecg_ref32_resource_record_extra_bits\": 0,\n";
        ss << "  \"ecg_ref32_resource_total_bits\": "
           << ref32TotalResourceBits() << ",\n";
        ss << "  \"ecg_ref32_popt_matrix_bits\": "
           << ref32PoptMatrixBits() << ",\n";
        ss << "  \"ecg_ref32_resource_reduction_x\": "
           << std::fixed << std::setprecision(6)
           << ref32ResourceReduction() << ",\n";
        static constexpr const char* kAdmissionArmNames[] = {
            "grasp", "future"
        };
        for (uint8_t arm = 0; arm < ecg_policy::ADMIT_ARM_COUNT; ++arm) {
            ss << "  \"ecg_admission_leader_accesses_"
               << kAdmissionArmNames[arm] << "\": "
               << l3_->getAdmissionLeaderAccesses(arm) << ",\n";
            ss << "  \"ecg_admission_leader_misses_"
               << kAdmissionArmNames[arm] << "\": "
               << l3_->getAdmissionLeaderMisses(arm) << ",\n";
            ss << "  \"ecg_admission_follower_selections_"
               << kAdmissionArmNames[arm] << "\": "
               << l3_->getAdmissionFollowerSelections(arm) << ",\n";
        }
        ss << "  \"ecg_admission_completed_windows\": "
           << l3_->getAdmissionCompletedWindows() << ",\n";
        ss << "  \"ecg_admission_winner_changes\": "
           << l3_->getAdmissionWinnerChanges() << ",\n";
        ss << "  \"ecg_admission_final_winner_arm\": "
           << static_cast<unsigned>(l3_->getAdmissionWinnerArm()) << ",\n";
        ss << "  \"ecg_admission_set_offset\": "
           << l3_->getAdmissionSetOffset() << ",\n";
        static constexpr const char* kDuelingArmNames[] = {
            "rrip", "grasp", "epoch", "degree", "lru"
        };
        const auto& leader_samples =
            l3_->getDuelingLeaderSamplesByArm();
        const auto& winner_windows =
            l3_->getDuelingWinnerWindowsByArm();
        const auto& follower_selections =
            l3_->getDuelingFollowerSelectionsByArm();
        for (size_t arm = 0; arm < ecg_policy::DUEL_ARM_COUNT; ++arm) {
            ss << "  \"ecg_dueling_leader_samples_"
               << kDuelingArmNames[arm] << "\": "
               << leader_samples[arm] << ",\n";
            ss << "  \"ecg_dueling_winner_windows_"
               << kDuelingArmNames[arm] << "\": "
               << winner_windows[arm] << ",\n";
            ss << "  \"ecg_dueling_follower_selections_"
               << kDuelingArmNames[arm] << "\": "
               << follower_selections[arm] << ",\n";
        }
        ss << "  \"popt_matrix_stream_lines_simulated\": "
           << popt_stream_lines_ << ",\n";
        ss << "  \"popt_matrix_stream_columns_simulated\": "
           << popt_stream_columns_ << ",\n";
        ss << "  \"stream_prefetch_model\": \""
           << (streamPrefetchOracle() ? "oracle" : "stride") << "\",\n";
        ss << "  \"stream_prefetch_issued\": " << stride_pf_issued_ << ",\n";
        ss << "  \"stream_prefetch_throttled\": " << stride_pf_throttled_ << ",\n";
        ss << "  \"stream_prefetch_untrained\": " << stride_pf_untrained_ << ",\n";
        ss << "  \"L1\": " << levelToJSON(*l1_) << ",\n";
        ss << "  \"L2\": " << levelToJSON(*l2_) << ",\n";
        ss << "  \"L3\": " << levelToJSON(*l3_) << "\n";
        ss << "}";
        return ss.str();
    }

    // Get stats as feature vector for perceptron
    std::vector<double> getFeatures() const {
        const auto& l1s = l1_->getStats();
        const auto& l2s = l2_->getStats();
        const auto& l3s = l3_->getStats();
        
        return {
            l1s.hitRate(),
            l2s.hitRate(),
            l3s.hitRate(),
            (double)memory_accesses_ / total_accesses_,  // DRAM access rate
            (double)l1s.evictions / l1s.totalAccesses(), // L1 eviction rate
            (double)l2s.evictions / l2s.totalAccesses(), // L2 eviction rate
            (double)l3s.evictions / l3s.totalAccesses(), // L3 eviction rate
        };
    }

private:
    struct Ref32CommitUpdate {
        uint64_t line_addr = 0;
        uint64_t ready_sequence = 0;
        uint32_t quantized_deadline = 0;
        uint64_t exact_deadline = 0;
        ecg_ref32::State state = ecg_ref32::State::UNKNOWN;
    };

    bool ref32ResourcesActive() const {
        return graph_ctx_ &&
            graph_ctx_->mask_config.ecg_mode == ECGMode::ECG_REF32;
    }

    bool ref32DeployableResourcesActive() const {
        if (ref32_resource_snapshot_valid_)
            return ref32_resource_deployable_snapshot_;
        return ref32ResourcesActive() &&
            !graph_ctx_->ref32_exact_diagnostic;
    }

    uint64_t ref32ResourceLineCount() const {
        if (ref32_resource_snapshot_valid_)
            return ref32_resource_line_count_snapshot_;
        return ref32DeployableResourcesActive()
            ? l3_->getSizeBytes() / l3_->getLineSize() : 0;
    }

    uint32_t ref32ResourceLineBits() const {
        return ref32_commit_deadline_bits_ + 3u;
    }

    uint32_t ref32CommitEntryBits() const {
        return 51u + 2u * ref32_commit_deadline_bits_;
    }

    uint32_t ref32PrefetchEntryBits() const {
        return 49u + ref32_commit_deadline_bits_;
    }

    uint64_t ref32LineStateBits() const {
        return ref32ResourceLineCount() * ref32ResourceLineBits();
    }

    uint64_t ref32PoptMatrixBits() const {
        if (ref32_resource_snapshot_valid_)
            return ref32_resource_popt_bits_snapshot_;
        if (!ref32DeployableResourcesActive())
            return 0;
        const uint64_t vertices =
            graph_ctx_->topology.num_vertices;
        const uint64_t property_lines = (vertices + 15u) / 16u;
        return property_lines * 256u * 8u;
    }

    uint64_t ref32TotalResourceBits() const {
        if (ref32_resource_snapshot_valid_)
            return ref32_resource_total_bits_snapshot_;
        if (!ref32DeployableResourcesActive())
            return 0;
        return ref32LineStateBits() +
            (ref32_commit_channel_
                ? ref32_commit_queue_limit_ *
                    ref32CommitEntryBits() : 0u) +
            (ref32_prefetch_enabled_
                ? ref32_prefetch_queue_limit_ *
                    ref32PrefetchEntryBits() + 512u : 0u) +
            64u;
    }

    void captureRef32ResourceSnapshot() {
        ref32_resource_snapshot_valid_ = true;
        ref32_resource_deployable_snapshot_ =
            ref32ResourcesActive() &&
            !graph_ctx_->ref32_exact_diagnostic;
        if (!ref32_resource_deployable_snapshot_)
            return;
        ref32_resource_line_count_snapshot_ =
            l3_->getSizeBytes() / l3_->getLineSize();
        const uint64_t vertices =
            graph_ctx_->topology.num_vertices;
        ref32_resource_popt_bits_snapshot_ =
            ((vertices + 15u) / 16u) * 256u * 8u;
        ref32_resource_total_bits_snapshot_ =
            ref32_resource_line_count_snapshot_ *
                ref32ResourceLineBits() +
            (ref32_commit_channel_
                ? ref32_commit_queue_limit_ *
                    ref32CommitEntryBits() : 0u) +
            (ref32_prefetch_enabled_
                ? ref32_prefetch_queue_limit_ *
                    ref32PrefetchEntryBits() + 512u : 0u) +
            64u;
    }

    double ref32ResourceReduction() const {
        const uint64_t total = ref32TotalResourceBits();
        return total > 0
            ? static_cast<double>(ref32PoptMatrixBits()) /
                static_cast<double>(total)
            : 0.0;
    }

    struct Ref32PrefetchUpdate {
        uint64_t line_addr = 0;
        uint64_t ready_sequence = 0;
    };

    static uint32_t ref32CommitEnv(
            const char* name, uint32_t fallback, uint32_t maximum) {
        const char* value = std::getenv(name);
        long parsed = value ? std::strtol(value, nullptr, 10)
                            : static_cast<long>(fallback);
        if (parsed < 1) parsed = 1;
        if (parsed > static_cast<long>(maximum))
            parsed = static_cast<long>(maximum);
        return static_cast<uint32_t>(parsed);
    }

    bool isCurrentRef32RecordRequest(uint64_t address) const {
        if (!graph_ctx_ ||
            graph_ctx_->mask_config.ecg_mode != ECGMode::ECG_REF32 ||
            !graph_ctx_->isEcgEpochData(address)) {
            return false;
        }
        const auto& hints = graph_ctx_->hints_for_thread();
        return hints.edge_ref_valid &&
            static_cast<ecg_ref32::State>(hints.edge_ref_state) !=
                ecg_ref32::State::UNKNOWN;
    }

    bool ref32PrefetchPending(uint64_t line_addr) const {
        return std::any_of(
            ref32_prefetch_updates_.begin(),
            ref32_prefetch_updates_.end(),
            [&](const Ref32PrefetchUpdate& update) {
                return update.line_addr == line_addr;
            });
    }

    void completeRef32Prefetch(
            const Ref32PrefetchUpdate& update,
            uint64_t current_sequence) {
        if (l1_->contains(update.line_addr) ||
            l2_->contains(update.line_addr) ||
            l3_->contains(update.line_addr)) {
            ++ref32_prefetch_completion_resident_;
            return;
        }
        if (!l3_->canAdmitRef32Prefetch(
                update.line_addr, current_sequence)) {
            ++ref32_prefetch_completion_admission_dropped_;
            return;
        }
        l3_->insert(update.line_addr, false, true);
        markPrefetchFill(update.line_addr);
        ++ref32_prefetch_fills_completed_;
    }

    void processRef32Prefetches(
            uint64_t current_sequence, bool force = false) {
        if (!ref32_prefetch_enabled_)
            return;
        if (!force &&
            ref32_prefetch_last_process_sequence_ == current_sequence) {
            return;
        }
        ref32_prefetch_last_process_sequence_ = current_sequence;
        uint32_t budget = ref32_prefetch_bandwidth_;
        while (budget > 0 && !ref32_prefetch_updates_.empty()) {
            const Ref32PrefetchUpdate& front =
                ref32_prefetch_updates_.front();
            if (front.ready_sequence > current_sequence)
                break;
            const Ref32PrefetchUpdate update = front;
            ref32_prefetch_updates_.pop_front();
            completeRef32Prefetch(update, current_sequence);
            --budget;
        }
        if (!ref32_prefetch_updates_.empty() &&
            ref32_prefetch_updates_.front().ready_sequence <=
                current_sequence) {
            ++ref32_prefetch_bandwidth_deferred_;
        }
    }

    void completeLateRef32Prefetch(
            uint64_t line_addr, uint64_t current_sequence) {
        auto found = std::find_if(
            ref32_prefetch_updates_.begin(),
            ref32_prefetch_updates_.end(),
            [&](const Ref32PrefetchUpdate& update) {
                return update.line_addr == line_addr;
            });
        if (found == ref32_prefetch_updates_.end())
            return;
        const Ref32PrefetchUpdate update = *found;
        ref32_prefetch_updates_.erase(found);
        ++ref32_prefetch_late_merged_;
        completeRef32Prefetch(update, current_sequence);
    }

    void issueCurrentRef32Prefetch() {
        const auto& hints = graph_ctx_->hints_for_thread();
        if (!hints.edge_ref_prefetch_valid ||
            hints.edge_ref_action == 0) {
            return;
        }
        ++ref32_prefetch_actions_seen_;
        const uint64_t sequence = hints.edge_ref_sequence;
        if (ref32_prefetch_last_issue_sequence_ !=
                std::numeric_limits<uint64_t>::max() &&
            sequence - ref32_prefetch_last_issue_sequence_ <
                ref32_prefetch_issue_interval_) {
            ++ref32_prefetch_rate_limited_;
            return;
        }
        const uint64_t target =
            lineAddress(hints.edge_ref_prefetch_address);
        if (l1_->contains(target) || l2_->contains(target) ||
            l3_->contains(target)) {
            ++ref32_prefetch_resident_duplicates_;
            return;
        }
        if (ref32PrefetchPending(target)) {
            ++ref32_prefetch_pending_duplicates_;
            return;
        }
        if (ref32_prefetch_updates_.size() >=
            ref32_prefetch_queue_limit_) {
            ++ref32_prefetch_queue_dropped_;
            return;
        }
        if (!l3_->canAdmitRef32Prefetch(target, sequence)) {
            ++ref32_prefetch_admission_dropped_;
            return;
        }

        ++prefetch_requests_;
        ++prefetch_fills_;
        ++ref32_prefetch_requests_issued_;
        recordPrefetchTranslation(target);
        Ref32PrefetchUpdate update;
        update.line_addr = target;
        update.ready_sequence =
            sequence + ref32_prefetch_latency_;
        ref32_prefetch_updates_.push_back(update);
        ref32_prefetch_last_issue_sequence_ = sequence;
        ref32_prefetch_max_occupancy_ = std::max<uint64_t>(
            ref32_prefetch_max_occupancy_,
            ref32_prefetch_updates_.size());
    }

    void enqueueRef32CommitUpdate(uint64_t line_addr) {
        const auto& hints = graph_ctx_->hints_for_thread();
        const auto state =
            static_cast<ecg_ref32::State>(hints.edge_ref_state);
        ++ref32_commit_generated_;
        const uint32_t distance = std::max<uint32_t>(
            1, hints.edge_ref_distance);
        const uint32_t deadline_mask =
            ref32_commit_deadline_bits_ == 32
            ? UINT32_MAX
            : (uint32_t{1} << ref32_commit_deadline_bits_) - 1u;
        const uint32_t max_forward =
            ref32_commit_deadline_bits_ == 32
            ? ecg_ref32::kMaxFiniteDistance
            : (uint32_t{1} << (ref32_commit_deadline_bits_ - 1)) - 1u;
        const uint32_t quantized_deadline = static_cast<uint32_t>(
            hints.edge_ref_sequence +
            std::min<uint32_t>(distance, max_forward)) & deadline_mask;
        const uint64_t exact_deadline =
            hints.edge_ref_sequence + distance;

        for (auto& update : ref32_commit_updates_) {
            if (update.line_addr != line_addr)
                continue;
            update.quantized_deadline = quantized_deadline;
            update.exact_deadline = exact_deadline;
            update.state = state;
            ++ref32_commit_coalesced_;
            return;
        }
        if (ref32_commit_updates_.size() >=
            ref32_commit_queue_limit_) {
            ++ref32_commit_queue_dropped_;
            return;
        }
        Ref32CommitUpdate update;
        update.line_addr = line_addr;
        update.ready_sequence =
            hints.edge_ref_sequence + ref32_commit_latency_;
        update.quantized_deadline = quantized_deadline;
        update.exact_deadline = exact_deadline;
        update.state = state;
        ref32_commit_updates_.push_back(update);
        ref32_commit_max_occupancy_ = std::max<uint64_t>(
            ref32_commit_max_occupancy_,
            ref32_commit_updates_.size());
    }

    void processRef32CommitUpdates(
            uint64_t current_sequence, bool force = false) {
        if (!ref32_commit_channel_)
            return;
        if (!force && ref32_commit_last_sequence_ == current_sequence)
            return;
        ref32_commit_last_sequence_ = current_sequence;
        uint32_t budget = ref32_commit_bandwidth_;
        while (budget > 0 && !ref32_commit_updates_.empty()) {
            const Ref32CommitUpdate& front =
                ref32_commit_updates_.front();
            if (front.ready_sequence > current_sequence)
                break;
            const Ref32CommitUpdate update = front;
            ref32_commit_updates_.pop_front();
            const Ref32UpdateResult result =
                l3_->applyRef32CommitUpdate(
                    update.line_addr, update.state,
                    update.quantized_deadline, update.exact_deadline,
                    current_sequence);
            if (result == Ref32UpdateResult::APPLIED)
                ++ref32_commit_applied_;
            else if (result == Ref32UpdateResult::NOT_RESIDENT)
                ++ref32_commit_not_resident_;
            else
                ++ref32_commit_expired_;
            --budget;
        }
        if (!ref32_commit_updates_.empty() &&
            ref32_commit_updates_.front().ready_sequence <=
                current_sequence) {
            ++ref32_commit_bandwidth_deferred_;
        }
    }

    static size_t getEnvSize(const char* name, size_t default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        
        char* end;
        size_t result = std::strtoul(val, &end, 10);
        
        // Handle K, M, G suffixes
        if (*end == 'K' || *end == 'k') result *= 1024;
        else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;
        
        return result > 0 ? result : default_val;
    }

    void printLevelStats(std::ostream& os, const CacheLevel& level) const {
        const auto& stats = level.getStats();
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ " << level.getName() << " Cache (" 
           << formatSize(level.getSizeBytes()) << ", "
           << level.getAssociativity() << "-way, "
           << PolicyToString(level.getPolicy()) << ")"
           << std::string(40 - level.getName().length() - 
                         formatSize(level.getSizeBytes()).length() -
                         std::to_string(level.getAssociativity()).length() -
                         PolicyToString(level.getPolicy()).length(), ' ')
           << "║\n";
        os << "║   Hits:              " << std::setw(15) << stats.hits.load()
           << "                          ║\n";
        os << "║   Misses:            " << std::setw(15) << stats.misses.load()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (stats.hitRate() * 100) << "%"
           << "                          ║\n";
        os << "║   Evictions:         " << std::setw(15) << stats.evictions.load()
           << "                          ║\n";
    }

    std::string levelToJSON(const CacheLevel& level) const {
        const auto& stats = level.getStats();
        std::ostringstream ss;
        ss << "{\n";
        ss << "    \"size_bytes\": " << level.getSizeBytes() << ",\n";
        ss << "    \"ways\": " << level.getAssociativity() << ",\n";
        ss << "    \"sets\": " << level.getNumSets() << ",\n";
        ss << "    \"line_size\": " << level.getLineSize() << ",\n";
        ss << "    \"policy\": \"" << PolicyToString(level.getPolicy()) << "\",\n";
        ss << "    \"hits\": " << stats.hits.load() << ",\n";
        ss << "    \"misses\": " << stats.misses.load() << ",\n";
        ss << "    \"prop_hits\": " << stats.prop_hits.load() << ",\n";
        ss << "    \"prop_misses\": " << stats.prop_misses.load() << ",\n";
        ss << "    \"hit_rate\": " << std::fixed << std::setprecision(6) << stats.hitRate() << ",\n";
        ss << "    \"evictions\": " << stats.evictions.load() << ",\n";
        ss << "    \"writebacks\": " << stats.writebacks.load() << "\n";
        ss << "  }";
        return ss.str();
    }

    uint64_t lineAddress(uint64_t address) const {
        return address & ~(uint64_t(line_size_ - 1));
    }

    bool hasPrefetchedLine(uint64_t line_addr) const {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        return prefetched_lines_.find(line_addr) != prefetched_lines_.end();
    }

    void markPrefetchFill(uint64_t line_addr) {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        prefetched_lines_.insert(line_addr);
    }

    void markPrefetchUseful(uint64_t line_addr) {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        if (prefetched_lines_.erase(line_addr) > 0) {
            prefetch_useful_++;
        }
    }

    void markPrefetchEvictedBeforeUse(uint64_t line_addr) {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        if (prefetched_lines_.erase(line_addr) > 0) {
            prefetch_evicted_before_use_++;
        }
    }

    static std::string formatSize(size_t bytes) {
        if (bytes >= 1024 * 1024 * 1024) {
            return std::to_string(bytes / (1024 * 1024 * 1024)) + "GB";
        } else if (bytes >= 1024 * 1024) {
            return std::to_string(bytes / (1024 * 1024)) + "MB";
        } else if (bytes >= 1024) {
            return std::to_string(bytes / 1024) + "KB";
        }
        return std::to_string(bytes) + "B";
    }

    std::unique_ptr<CacheLevel> l1_;
    std::unique_ptr<CacheLevel> l2_;
    std::unique_ptr<CacheLevel> l3_;
    size_t line_size_;
    bool enabled_;
    // ECG_EXACT_STORED: when set (env ECG_STORED_REFRESH=1), broadcast the
    // per-edge hint to the LLC on every demand access to keep its stamp fresh.
    bool refresh_exact_stamp_ = std::getenv("ECG_STORED_REFRESH") != nullptr;
    // ECG_REFRESH_LLC_ONLY: HW-feasibility gate. When set, the epoch hint is written
    // to the L3 line ONLY on accesses that actually REACH L3 (miss L1+L2) — i.e. the
    // metadata write piggybacks an L3 access that is already happening (free), instead
    // of the default aggressive broadcast that also writes L3 on L1/L2 hits (an extra
    // L3 metadata transaction per inner-cache hit). Isolates how much of the refresh
    // win needs the aggressive broadcast vs is recoverable by the free piggybacked form.
    bool refresh_llc_only_ = std::getenv("ECG_REFRESH_LLC_ONLY") != nullptr;
    bool ref32_commit_channel_ = []() {
        const char* value = std::getenv("ECG_REF32_COMMIT_CHANNEL");
        return value && value[0] && std::string(value) != "0";
    }();
    uint32_t ref32_commit_queue_limit_ =
        ref32CommitEnv("ECG_REF32_UPDATE_QUEUE", 16, 256);
    uint32_t ref32_commit_latency_ =
        ref32CommitEnv("ECG_REF32_UPDATE_LATENCY", 8, 4096);
    uint32_t ref32_commit_bandwidth_ =
        ref32CommitEnv("ECG_REF32_UPDATE_BANDWIDTH", 1, 16);
    uint32_t ref32_commit_deadline_bits_ =
        ref32CommitEnv(
            "ECG_REF32_DEADLINE_BITS",
            ecg_ref32::kDefaultDeadlineBits, 32);
    std::deque<Ref32CommitUpdate> ref32_commit_updates_;
    uint64_t ref32_commit_generated_ = 0;
    uint64_t ref32_commit_coalesced_ = 0;
    uint64_t ref32_commit_queue_dropped_ = 0;
    uint64_t ref32_commit_applied_ = 0;
    uint64_t ref32_commit_not_resident_ = 0;
    uint64_t ref32_commit_expired_ = 0;
    uint64_t ref32_commit_bandwidth_deferred_ = 0;
    uint64_t ref32_commit_max_occupancy_ = 0;
    uint64_t ref32_commit_last_sequence_ =
        std::numeric_limits<uint64_t>::max();
    bool ref32_prefetch_enabled_ = []() {
        const char* value = std::getenv("ECG_REF32_PREFETCH");
        return value && value[0] && std::string(value) != "0";
    }();
    uint32_t ref32_prefetch_queue_limit_ =
        ref32CommitEnv("ECG_REF32_PREFETCH_QUEUE", 8, 256);
    uint32_t ref32_prefetch_latency_ =
        ref32CommitEnv("ECG_REF32_PREFETCH_LATENCY", 8, 4096);
    uint32_t ref32_prefetch_bandwidth_ =
        ref32CommitEnv("ECG_REF32_PREFETCH_BANDWIDTH", 1, 16);
    uint32_t ref32_prefetch_issue_interval_ =
        ref32CommitEnv("ECG_REF32_PREFETCH_INTERVAL", 8, 4096);
    std::deque<Ref32PrefetchUpdate> ref32_prefetch_updates_;
    uint64_t ref32_prefetch_actions_seen_ = 0;
    uint64_t ref32_prefetch_rate_limited_ = 0;
    uint64_t ref32_prefetch_resident_duplicates_ = 0;
    uint64_t ref32_prefetch_pending_duplicates_ = 0;
    uint64_t ref32_prefetch_admission_dropped_ = 0;
    uint64_t ref32_prefetch_queue_dropped_ = 0;
    uint64_t ref32_prefetch_requests_issued_ = 0;
    uint64_t ref32_prefetch_fills_completed_ = 0;
    uint64_t ref32_prefetch_late_merged_ = 0;
    uint64_t ref32_prefetch_completion_resident_ = 0;
    uint64_t ref32_prefetch_completion_admission_dropped_ = 0;
    uint64_t ref32_prefetch_bandwidth_deferred_ = 0;
    uint64_t ref32_prefetch_max_occupancy_ = 0;
    uint64_t ref32_prefetch_last_issue_sequence_ =
        std::numeric_limits<uint64_t>::max();
    uint64_t ref32_prefetch_last_process_sequence_ =
        std::numeric_limits<uint64_t>::max();
    bool ref32_resource_snapshot_valid_ = false;
    bool ref32_resource_deployable_snapshot_ = false;
    uint64_t ref32_resource_line_count_snapshot_ = 0;
    uint64_t ref32_resource_popt_bits_snapshot_ = 0;
    uint64_t ref32_resource_total_bits_snapshot_ = 0;
    bool adaptive_flowthrough_ = []() {
        const char* value = std::getenv("ECG_FLOWTHROUGH_ADAPTIVE");
        return value && value[0] && std::string(value) != "0";
    }();
    ecg_policy::OnlinePlacementSelector placement_selector_;
    const GraphCacheContext* graph_ctx_ = nullptr;  // for the structure-stream prefetcher
    std::atomic<uint64_t> total_accesses_{0};
    std::atomic<uint64_t> memory_accesses_{0};
    std::atomic<uint64_t> structural_flowthrough_accesses_{0};
    std::atomic<uint64_t> prefetch_requests_{0};
    std::atomic<uint64_t> prefetch_cache_hits_{0};
    std::atomic<uint64_t> prefetch_fills_{0};
    std::atomic<uint64_t> prefetch_useful_{0};
    std::atomic<uint64_t> prefetch_evicted_before_use_{0};
    mutable std::mutex prefetch_mutex_;
    std::unordered_set<uint64_t> prefetched_lines_;
    // Property-prefetch translation-pressure proxy
    // Raw prefetch count is not a useful TLB-pressure metric (a 4KB page holds
    // ~1024 4B properties), so track the DISTINCT pages the prefetch targets touch
    // and the misses of a finite LRU MTLB. This tests the DROPLET(all-K) vs ECG_PFX
    // (best-1) comparison at PAGE granularity. Single-thread (cache_sim pins
    // OMP_NUM_THREADS=1), so no atomics/locks are needed for these.
    std::unordered_set<uint64_t> pfx_pages_4k_;
    std::unordered_set<uint64_t> pfx_pages_2m_;
    std::list<uint64_t> pfx_mtlb_lru_;                                      // front = MRU
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> pfx_mtlb_pos_;
    uint64_t pfx_mtlb_misses_{0};
    static size_t pfxMtlbEntriesFromEnv() {
        if (const char* e = std::getenv("CACHE_PFX_MTLB_ENTRIES")) {
            long v = std::atol(e); if (v > 0) return static_cast<size_t>(v);
        }
        return 128;
    }
    size_t pfx_mtlb_size_{pfxMtlbEntriesFromEnv()};                        // entries (env)

    // One translation per generated (deduped) prefetch target: record the 4KB/2MB
    // page touched and model a finite LRU MTLB (DROPLET-style MC-side TLB).
    void recordPrefetchTranslation(uint64_t address) {
        const uint64_t pg4k = address >> 12, pg2m = address >> 21;
        pfx_pages_4k_.insert(pg4k);
        pfx_pages_2m_.insert(pg2m);
        auto it = pfx_mtlb_pos_.find(pg4k);
        if (it != pfx_mtlb_pos_.end()) {                                   // hit -> MRU
            pfx_mtlb_lru_.erase(it->second);
            pfx_mtlb_lru_.push_front(pg4k);
            it->second = pfx_mtlb_lru_.begin();
            return;
        }
        pfx_mtlb_misses_++;                                                // miss -> insert
        pfx_mtlb_lru_.push_front(pg4k);
        pfx_mtlb_pos_[pg4k] = pfx_mtlb_lru_.begin();
        if (pfx_mtlb_lru_.size() > pfx_mtlb_size_) {
            pfx_mtlb_pos_.erase(pfx_mtlb_lru_.back());
            pfx_mtlb_lru_.pop_back();
        }
    }
};

// ============================================================================
// FAST Cache Hierarchy - NO LOCKS, optimized for single-threaded simulation
// ~10-20x faster than CacheHierarchy, exact results
// ============================================================================
class FastCacheHierarchy {
public:
    FastCacheHierarchy(
        size_t l1_size = 32 * 1024,
        size_t l1_ways = 8,
        size_t l2_size = 256 * 1024,
        size_t l2_ways = 8,
        size_t l3_size = 8 * 1024 * 1024,
        size_t l3_ways = 16,
        size_t line_size = 64
    ) : line_size_(line_size), enabled_(true) {
        l1_ = std::make_unique<FastCacheLevel>("L1", l1_size, line_size, l1_ways);
        l2_ = std::make_unique<FastCacheLevel>("L2", l2_size, line_size, l2_ways);
        l3_ = std::make_unique<FastCacheLevel>("L3", l3_size, line_size, l3_ways);
    }

    static FastCacheHierarchy fromEnvironment() {
        size_t l1_size = getEnvSize("CACHE_L1_SIZE", 32 * 1024);
        size_t l1_ways = getEnvSize("CACHE_L1_WAYS", 8);
        size_t l2_size = getEnvSize("CACHE_L2_SIZE", 256 * 1024);
        size_t l2_ways = getEnvSize("CACHE_L2_WAYS", 8);
        size_t l3_size = getEnvSize("CACHE_L3_SIZE", 8 * 1024 * 1024);
        size_t l3_ways = getEnvSize("CACHE_L3_WAYS", 16);
        size_t line_size = getEnvSize("CACHE_LINE_SIZE", 64);
        
        return FastCacheHierarchy(l1_size, l1_ways, l2_size, l2_ways,
                                  l3_size, l3_ways, line_size);
    }

    // FAST access - no locks, inline
    __attribute__((always_inline))
    inline void access(uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        
        total_accesses_++;
        address = address & ~(line_size_ - 1);  // Align to cache line
        
        if (l1_->access(address)) return;
        if (l2_->access(address)) { l1_->insert(address); return; }
        if (l3_->access(address)) { l2_->insert(address); l1_->insert(address); return; }
        
        memory_accesses_++;
        l3_->insert(address);
        l2_->insert(address);
        l1_->insert(address);
    }

    inline void accessStream(uint64_t address, bool is_write = false) {
        access(address, is_write);
    }
    inline void accessStructuralStream(
            uint64_t address, bool is_write = false) {
        access(address, is_write);
    }

    template<typename T>
    inline void read(const T* ptr) { access(reinterpret_cast<uint64_t>(ptr), false); }

    template<typename T>
    inline void write(T* ptr) { access(reinterpret_cast<uint64_t>(ptr), true); }

    template<typename T>
    inline void readArray(const T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), false);
    }

    template<typename T>
    inline void writeArray(T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), true);
    }

    void resetStats() {
        l1_->resetStats();
        l2_->resetStats();
        l3_->resetStats();
        total_accesses_ = 0;
        memory_accesses_ = 0;
    }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // No-op: FastCacheHierarchy uses clock algorithm, not policy-based eviction
    void setCurrentVertex(uint32_t) {}
    void prefetch(uint64_t address) { access(address, false); }
    void initGraphContext(const GraphCacheContext*) {}
    
    uint64_t getTotalAccesses() const { return total_accesses_; }
    uint64_t getMemoryAccesses() const { return memory_accesses_; }

    void printStats(std::ostream& os = std::cout) const {
        os << "\n";
        os << "╔══════════════════════════════════════════════════════════════════╗\n";
        os << "║              FAST CACHE SIMULATION RESULTS                       ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        
        printFastLevelStats(os, *l1_);
        printFastLevelStats(os, *l2_);
        printFastLevelStats(os, *l3_);
        
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ SUMMARY                                                          ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ Total Accesses:      " << std::setw(15) << total_accesses_ 
           << "                          ║\n";
        os << "║ Memory Accesses:     " << std::setw(15) << memory_accesses_
           << "                          ║\n";
        double overall = total_accesses_ > 0 ? 
            (1.0 - (double)memory_accesses_ / total_accesses_) : 0.0;
        os << "║ Overall Hit Rate:    " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (overall * 100) << "%"
           << "                          ║\n";
        os << "╚══════════════════════════════════════════════════════════════════╝\n";
    }

    std::string toJSON() const {
        auto& l1 = l1_->getStats();
        auto& l2 = l2_->getStats();
        auto& l3 = l3_->getStats();
        
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"mode\": \"fast\",\n";
        ss << "  \"total_accesses\": " << total_accesses_ << ",\n";
        ss << "  \"memory_accesses\": " << memory_accesses_ << ",\n";
        ss << "  \"L1\": { \"hits\": " << l1.hits.load() << ", \"misses\": " << l1.misses.load() 
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l1.hitRate() << " },\n";
        ss << "  \"L2\": { \"hits\": " << l2.hits.load() << ", \"misses\": " << l2.misses.load() 
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l2.hitRate() << " },\n";
        ss << "  \"L3\": { \"hits\": " << l3.hits.load() << ", \"misses\": " << l3.misses.load() 
           << ", \"prop_hits\": " << l3.prop_hits.load() << ", \"prop_misses\": " << l3.prop_misses.load()
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l3.hitRate() << " }\n";
        ss << "}";
        return ss.str();
    }

    std::vector<double> getFeatures() const {
        const auto& l1s = l1_->getStats();
        const auto& l2s = l2_->getStats();
        const auto& l3s = l3_->getStats();
        
        return {
            l1s.hitRate(),
            l2s.hitRate(),
            l3s.hitRate(),
            total_accesses_ > 0 ? (double)memory_accesses_ / total_accesses_ : 0.0,
            l1s.totalAccesses() > 0 ? (double)l1s.evictions / l1s.totalAccesses() : 0.0,
            l2s.totalAccesses() > 0 ? (double)l2s.evictions / l2s.totalAccesses() : 0.0,
            l3s.totalAccesses() > 0 ? (double)l3s.evictions / l3s.totalAccesses() : 0.0,
        };
    }

private:
    static size_t getEnvSize(const char* name, size_t default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        char* end;
        size_t result = std::strtoul(val, &end, 10);
        if (*end == 'K' || *end == 'k') result *= 1024;
        else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;
        return result > 0 ? result : default_val;
    }

    void printFastLevelStats(std::ostream& os, const FastCacheLevel& level) const {
        const auto& stats = level.getStats();
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ " << level.getName() << " Cache (" 
           << formatSize(level.getSizeBytes()) << ", "
           << level.getAssociativity() << "-way, Clock)"
           << std::string(42 - level.getName().length() - 
                         formatSize(level.getSizeBytes()).length() -
                         std::to_string(level.getAssociativity()).length(), ' ')
           << "║\n";
        os << "║   Hits:              " << std::setw(15) << stats.hits.load()
           << "                          ║\n";
        os << "║   Misses:            " << std::setw(15) << stats.misses.load()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (stats.hitRate() * 100) << "%"
           << "                          ║\n";
    }

    static std::string formatSize(size_t bytes) {
        if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + "MB";
        if (bytes >= 1024) return std::to_string(bytes / 1024) + "KB";
        return std::to_string(bytes) + "B";
    }

    std::unique_ptr<FastCacheLevel> l1_;
    std::unique_ptr<FastCacheLevel> l2_;
    std::unique_ptr<FastCacheLevel> l3_;
    size_t line_size_;
    bool enabled_;
    uint64_t total_accesses_ = 0;
    uint64_t memory_accesses_ = 0;
};

// ============================================================================
// ULTRA-FAST Cache Hierarchy - Maximum performance with packed structures
// ~2-3x faster than FastCacheHierarchy through better memory layout
// ============================================================================
class UltraFastCacheHierarchy {
public:
    UltraFastCacheHierarchy(
        size_t l1_size = 32 * 1024,
        size_t l1_ways = 8,
        size_t l2_size = 256 * 1024,
        size_t l2_ways = 8,
        size_t l3_size = 8 * 1024 * 1024,
        size_t l3_ways = 16,
        size_t line_size = 64
    ) : line_size_(line_size), line_mask_(~(line_size - 1)), enabled_(true),
        total_accesses_(0), memory_accesses_(0) {
        l1_ = std::make_unique<UltraFastCacheLevel>("L1", l1_size, line_size, l1_ways);
        l2_ = std::make_unique<UltraFastCacheLevel>("L2", l2_size, line_size, l2_ways);
        l3_ = std::make_unique<UltraFastCacheLevel>("L3", l3_size, line_size, l3_ways);
    }

    static UltraFastCacheHierarchy fromEnvironment() {
        size_t l1_size = getEnvSize("CACHE_L1_SIZE", 32 * 1024);
        size_t l1_ways = getEnvSize("CACHE_L1_WAYS", 8);
        size_t l2_size = getEnvSize("CACHE_L2_SIZE", 256 * 1024);
        size_t l2_ways = getEnvSize("CACHE_L2_WAYS", 8);
        size_t l3_size = getEnvSize("CACHE_L3_SIZE", 8 * 1024 * 1024);
        size_t l3_ways = getEnvSize("CACHE_L3_WAYS", 16);
        size_t line_size = getEnvSize("CACHE_LINE_SIZE", 64);
        
        return UltraFastCacheHierarchy(l1_size, l1_ways, l2_size, l2_ways,
                                       l3_size, l3_ways, line_size);
    }

    // ULTRA-FAST access - maximum inlining, minimal branching
    __attribute__((always_inline, hot))
    inline void access(uint64_t address, bool is_write = false) {
        total_accesses_++;
        address &= line_mask_;
        
        if (__builtin_expect(l1_->access(address), 1)) return;
        if (l2_->access(address)) { l1_->insert(address); return; }
        if (l3_->access(address)) { l2_->insert(address); l1_->insert(address); return; }
        
        memory_accesses_++;
        l3_->insert(address);
        l2_->insert(address);
        l1_->insert(address);
    }

    inline void accessStream(uint64_t address, bool is_write = false) {
        access(address, is_write);
    }
    inline void accessStructuralStream(
            uint64_t address, bool is_write = false) {
        access(address, is_write);
    }

    template<typename T>
    __attribute__((always_inline))
    inline void read(const T* ptr) { access(reinterpret_cast<uint64_t>(ptr)); }

    template<typename T>
    __attribute__((always_inline))
    inline void write(T* ptr) { access(reinterpret_cast<uint64_t>(ptr)); }

    template<typename T>
    __attribute__((always_inline))
    inline void readArray(const T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]));
    }

    template<typename T>
    __attribute__((always_inline))
    inline void writeArray(T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]));
    }

    void resetStats() {
        l1_->resetStats();
        l2_->resetStats();
        l3_->resetStats();
        total_accesses_ = 0;
        memory_accesses_ = 0;
    }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // No-op: UltraFastCacheHierarchy uses packed clock algorithm
    void setCurrentVertex(uint32_t) {}
    void prefetch(uint64_t address) { access(address, false); }
    void initGraphContext(const GraphCacheContext*) {}
    
    uint64_t getTotalAccesses() const { return total_accesses_; }
    uint64_t getMemoryAccesses() const { return memory_accesses_; }

    void printStats(std::ostream& os = std::cout) const {
        os << "\n";
        os << "╔══════════════════════════════════════════════════════════════════╗\n";
        os << "║            ULTRA-FAST CACHE SIMULATION RESULTS                   ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        
        printUltraFastLevelStats(os, *l1_);
        printUltraFastLevelStats(os, *l2_);
        printUltraFastLevelStats(os, *l3_);
        
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ SUMMARY                                                          ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ Total Accesses:      " << std::setw(15) << total_accesses_ 
           << "                          ║\n";
        os << "║ Memory Accesses:     " << std::setw(15) << memory_accesses_
           << "                          ║\n";
        double overall = total_accesses_ > 0 ? 
            (1.0 - (double)memory_accesses_ / total_accesses_) : 0.0;
        os << "║ Overall Hit Rate:    " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (overall * 100) << "%"
           << "                          ║\n";
        os << "╚══════════════════════════════════════════════════════════════════╝\n";
    }

    std::string toJSON() const {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"mode\": \"ultrafast\",\n";
        ss << "  \"total_accesses\": " << total_accesses_ << ",\n";
        ss << "  \"memory_accesses\": " << memory_accesses_ << ",\n";
        ss << "  \"L1\": { \"hits\": " << l1_->getHits() << ", \"misses\": " << l1_->getMisses() 
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l1_->hitRate() << " },\n";
        ss << "  \"L2\": { \"hits\": " << l2_->getHits() << ", \"misses\": " << l2_->getMisses() 
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l2_->hitRate() << " },\n";
        ss << "  \"L3\": { \"hits\": " << l3_->getHits() << ", \"misses\": " << l3_->getMisses() 
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l3_->hitRate() << " }\n";
        ss << "}";
        return ss.str();
    }

private:
    static size_t getEnvSize(const char* name, size_t default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        char* end;
        size_t result = std::strtoul(val, &end, 10);
        if (*end == 'K' || *end == 'k') result *= 1024;
        else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;
        return result > 0 ? result : default_val;
    }

    void printUltraFastLevelStats(std::ostream& os, const UltraFastCacheLevel& level) const {
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ " << level.getName() << " Cache (" 
           << formatSize(level.getSizeBytes()) << ", "
           << level.getAssociativity() << "-way, Clock)"
           << std::string(40 - level.getName().length() - 
                         formatSize(level.getSizeBytes()).length() -
                         std::to_string(level.getAssociativity()).length(), ' ')
           << "║\n";
        os << "║   Hits:              " << std::setw(15) << level.getHits()
           << "                          ║\n";
        os << "║   Misses:            " << std::setw(15) << level.getMisses()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (level.hitRate() * 100) << "%"
           << "                          ║\n";
    }

    static std::string formatSize(size_t bytes) {
        if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + "MB";
        if (bytes >= 1024) return std::to_string(bytes / 1024) + "KB";
        return std::to_string(bytes) + "B";
    }

    std::unique_ptr<UltraFastCacheLevel> l1_;
    std::unique_ptr<UltraFastCacheLevel> l2_;
    std::unique_ptr<UltraFastCacheLevel> l3_;
    size_t line_size_;
    uint64_t line_mask_;
    bool enabled_;
    uint64_t total_accesses_;
    uint64_t memory_accesses_;
};

// ============================================================================
// Multi-Core Cache Hierarchy (Private L1/L2 per core, Shared L3)
// Simulates realistic multi-core architecture like Intel/AMD processors
// ============================================================================
class MultiCoreCacheHierarchy {
public:
    static constexpr int MAX_CORES = 64;
    
    // Default: 8-core Intel-like hierarchy
    // Per-core: L1 32KB 8-way, L2 256KB 8-way (private)
    // Shared:   L3 8MB 16-way
    MultiCoreCacheHierarchy(
        int num_cores = 8,
        size_t l1_size = 32 * 1024,
        size_t l1_ways = 8,
        size_t l2_size = 256 * 1024,
        size_t l2_ways = 8,
        size_t l3_size = 8 * 1024 * 1024,
        size_t l3_ways = 16,
        size_t line_size = 64,
        EvictionPolicy policy = EvictionPolicy::LRU
    ) : MultiCoreCacheHierarchy(num_cores, l1_size, l1_ways, l2_size, l2_ways,
                                l3_size, l3_ways, line_size,
                                policy, policy, policy) {
    }

    MultiCoreCacheHierarchy(
        int num_cores,
        size_t l1_size,
        size_t l1_ways,
        size_t l2_size,
        size_t l2_ways,
        size_t l3_size,
        size_t l3_ways,
        size_t line_size,
        EvictionPolicy l1_policy,
        EvictionPolicy l2_policy,
        EvictionPolicy l3_policy
    ) : num_cores_(num_cores), line_size_(line_size), enabled_(true) {
        
        if (num_cores_ > MAX_CORES) num_cores_ = MAX_CORES;
        if (num_cores_ < 1) num_cores_ = 1;
        
        // Create private L1 and L2 for each core
        for (int i = 0; i < num_cores_; i++) {
            l1_caches_.push_back(std::make_unique<CacheLevel>(
                "L1-Core" + std::to_string(i), l1_size, line_size, l1_ways, l1_policy));
            l2_caches_.push_back(std::make_unique<CacheLevel>(
                "L2-Core" + std::to_string(i), l2_size, line_size, l2_ways, l2_policy));
        }
        
        // Create shared L3 (total size, not per-core)
        l3_shared_ = std::make_unique<CacheLevel>("L3-Shared", l3_size, line_size, l3_ways, l3_policy);
        
        // Initialize per-core statistics
        core_accesses_.resize(num_cores_, 0);
        core_memory_accesses_.resize(num_cores_, 0);
    }

    // Configure from environment variables
    static MultiCoreCacheHierarchy fromEnvironment() {
        int num_cores = static_cast<int>(getEnvSize("CACHE_NUM_CORES", 8));
        size_t l1_size = getEnvSize("CACHE_L1_SIZE", 32 * 1024);
        size_t l1_ways = getEnvSize("CACHE_L1_WAYS", 8);
        size_t l2_size = getEnvSize("CACHE_L2_SIZE", 256 * 1024);
        size_t l2_ways = getEnvSize("CACHE_L2_WAYS", 8);
        size_t l3_size = getEnvSize("CACHE_L3_SIZE", 8 * 1024 * 1024);
        size_t l3_ways = getEnvSize("CACHE_L3_WAYS", 16);
        size_t line_size = getEnvSize("CACHE_LINE_SIZE", 64);
        
        EvictionPolicy policy = GetEnvPolicy("CACHE_POLICY", EvictionPolicy::LRU);
        EvictionPolicy l1_policy = GetEnvPolicy("CACHE_L1_POLICY", policy);
        EvictionPolicy l2_policy = GetEnvPolicy("CACHE_L2_POLICY", policy);
        EvictionPolicy l3_policy = GetEnvPolicy("CACHE_L3_POLICY", policy);
        
        return MultiCoreCacheHierarchy(num_cores, l1_size, l1_ways, l2_size, l2_ways,
                           l3_size, l3_ways, line_size,
                           l1_policy, l2_policy, l3_policy);
    }

    // Main access function - uses OMP thread ID to select core
    void access(uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        
        int core_id = omp_get_thread_num() % num_cores_;
        accessCore(core_id, address, is_write);
    }

    void accessStream(uint64_t address, bool is_write = false) {
        // Prototype falls back to the normal multicore path. The reference path is
        // validated first in the deterministic single-core cache simulator.
        access(address, is_write);
    }
    void accessStructuralStream(
            uint64_t address, bool is_write = false) {
        access(address, is_write);
    }

    // Access from specific core
    void accessCore(int core_id, uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        if (core_id >= num_cores_) core_id = core_id % num_cores_;
        
        total_accesses_++;
        core_accesses_[core_id]++;
        
        CacheLevel* l1 = l1_caches_[core_id].get();
        CacheLevel* l2 = l2_caches_[core_id].get();
        
        // Try L1 (private, no contention)
        if (l1->access(address, is_write)) {
            return;  // L1 hit
        }
        
        // L1 miss, try L2 (private, no contention)
        if (l2->access(address, is_write)) {
            l1->insert(address, is_write);
            return;  // L2 hit
        }
        
        // L2 miss, try shared L3 (may have contention)
        if (l3_shared_->access(address, is_write)) {
            l2->insert(address, is_write);
            l1->insert(address, is_write);
            return;  // L3 hit
        }
        
        // L3 miss - fetch from memory
        memory_accesses_++;
        core_memory_accesses_[core_id]++;
        l3_shared_->insert(address, is_write);
        l2->insert(address, is_write);
        l1->insert(address, is_write);
    }

    // Convenience methods
    template<typename T>
    void read(const T* ptr) {
        access(reinterpret_cast<uint64_t>(ptr), false);
    }

    template<typename T>
    void write(T* ptr) {
        access(reinterpret_cast<uint64_t>(ptr), true);
    }

    template<typename T>
    void readArray(const T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), false);
    }

    template<typename T>
    void writeArray(T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), true);
    }

    // Reset all statistics
    void resetStats() {
        for (int i = 0; i < num_cores_; i++) {
            l1_caches_[i]->resetStats();
            l2_caches_[i]->resetStats();
            core_accesses_[i] = 0;
            core_memory_accesses_[i] = 0;
        }
        l3_shared_->resetStats();
        total_accesses_ = 0;
        memory_accesses_ = 0;
    }

    // Enable/disable simulation
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Get cache levels
    CacheLevel* L1(int core) { return l1_caches_[core % num_cores_].get(); }
    CacheLevel* L2(int core) { return l2_caches_[core % num_cores_].get(); }
    CacheLevel* L3() { return l3_shared_.get(); }
    int getNumCores() const { return num_cores_; }

    // Unified GraphCacheContext (preferred over legacy init methods)
    void initGraphContext(const GraphCacheContext* ctx) {
        for (int i = 0; i < num_cores_; i++) {
            l1_caches_[i]->initGraphContext(ctx);
            l2_caches_[i]->initGraphContext(ctx);
        }
        l3_shared_->initGraphContext(ctx);
    }

    // P-OPT: Initialize rereference matrix on shared LLC (L3) — legacy API
    void initPOPT(const uint8_t* reref_matrix, uint64_t irreg_base,
                  uint64_t irreg_bound, uint32_t num_vertices,
                  uint32_t num_epochs = 256) {
        l3_shared_->initPOPT(reref_matrix, irreg_base, irreg_bound, num_vertices, num_epochs);
    }

    // GRASP: Initialize degree-aware RRIP retention — legacy API
    void initGRASP(uint64_t data_ptr, uint32_t num_vertices,
                   size_t elem_size, double hot_fraction = 0.5) {
        size_t llc_size = l3_shared_->getSizeBytes();
        for (int i = 0; i < num_cores_; i++) {
            l1_caches_[i]->initGRASP(data_ptr, num_vertices, elem_size, l1_caches_[i]->getSizeBytes(), hot_fraction);
            l2_caches_[i]->initGRASP(data_ptr, num_vertices, elem_size, l2_caches_[i]->getSizeBytes(), hot_fraction);
        }
        l3_shared_->initGRASP(data_ptr, num_vertices, elem_size, llc_size, hot_fraction);
    }

    // P-OPT: Update current vertex (call at each outer-loop iteration)
    void setCurrentVertex(uint32_t vertex_id) {
        l3_shared_->setCurrentVertex(vertex_id);
    }

    void prefetch(uint64_t address) { access(address, false); }

    uint64_t getTotalAccesses() const { return total_accesses_; }
    uint64_t getMemoryAccesses() const { return memory_accesses_; }
    
    // Get aggregated L1/L2 stats across all cores
    CacheStats getAggregatedL1Stats() const {
        CacheStats agg;
        for (int i = 0; i < num_cores_; i++) {
            const auto& s = l1_caches_[i]->getStats();
            agg.hits += s.hits.load();
            agg.misses += s.misses.load();
            agg.reads += s.reads.load();
            agg.writes += s.writes.load();
            agg.evictions += s.evictions.load();
            agg.writebacks += s.writebacks.load();
        }
        return agg;
    }
    
    CacheStats getAggregatedL2Stats() const {
        CacheStats agg;
        for (int i = 0; i < num_cores_; i++) {
            const auto& s = l2_caches_[i]->getStats();
            agg.hits += s.hits.load();
            agg.misses += s.misses.load();
            agg.reads += s.reads.load();
            agg.writes += s.writes.load();
            agg.evictions += s.evictions.load();
            agg.writebacks += s.writebacks.load();
        }
        return agg;
    }

    // Print statistics
    void printStats(std::ostream& os = std::cout) const {
        os << "\n";
        os << "╔══════════════════════════════════════════════════════════════════╗\n";
        os << "║          MULTI-CORE CACHE SIMULATION RESULTS (" << num_cores_ << " cores)          ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        
        // Aggregate L1 stats
        CacheStats l1_agg = getAggregatedL1Stats();
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ L1 Cache (Private per core, " << l1_caches_[0]->getSizeBytes()/1024 << "KB each)                       ║\n";
        os << "║   Total Hits:        " << std::setw(15) << l1_agg.hits.load()
           << "                          ║\n";
        os << "║   Total Misses:      " << std::setw(15) << l1_agg.misses.load()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (l1_agg.hitRate() * 100) << "%"
           << "                          ║\n";
        
        // Aggregate L2 stats
        CacheStats l2_agg = getAggregatedL2Stats();
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ L2 Cache (Private per core, " << l2_caches_[0]->getSizeBytes()/1024 << "KB each)                      ║\n";
        os << "║   Total Hits:        " << std::setw(15) << l2_agg.hits.load()
           << "                          ║\n";
        os << "║   Total Misses:      " << std::setw(15) << l2_agg.misses.load()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (l2_agg.hitRate() * 100) << "%"
           << "                          ║\n";
        
        // L3 shared stats
        const auto& l3_stats = l3_shared_->getStats();
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ L3 Cache (Shared, " << l3_shared_->getSizeBytes()/(1024*1024) << "MB)                                      ║\n";
        os << "║   Hits:              " << std::setw(15) << l3_stats.hits.load()
           << "                          ║\n";
        os << "║   Misses:            " << std::setw(15) << l3_stats.misses.load()
           << "                          ║\n";
        os << "║   Hit Rate:          " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (l3_stats.hitRate() * 100) << "%"
           << "                          ║\n";
        
        // Summary
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ SUMMARY                                                          ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ Total Accesses:      " << std::setw(15) << total_accesses_.load() 
           << "                          ║\n";
        os << "║ Memory Accesses:     " << std::setw(15) << memory_accesses_.load()
           << "                          ║\n";
        double overall = total_accesses_ > 0 ? 
            (1.0 - (double)memory_accesses_.load() / total_accesses_.load()) : 0.0;
        os << "║ Overall Hit Rate:    " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (overall * 100) << "%"
           << "                          ║\n";
        os << "╚══════════════════════════════════════════════════════════════════╝\n";
        
        // Per-core breakdown (optional, detailed)
        os << "\nPer-Core Statistics:\n";
        os << "  Core │     Accesses │ Memory Acc │ Mem Rate\n";
        os << "───────┼──────────────┼────────────┼─────────\n";
        for (int i = 0; i < num_cores_; i++) {
            double rate = core_accesses_[i] > 0 ? 
                (double)core_memory_accesses_[i] / core_accesses_[i] * 100 : 0;
            os << std::setw(6) << i << " │ " 
               << std::setw(12) << core_accesses_[i] << " │ "
               << std::setw(10) << core_memory_accesses_[i] << " │ "
               << std::setw(6) << std::fixed << std::setprecision(2) << rate << "%\n";
        }
    }

    // Export statistics as JSON
    std::string toJSON() const {
        std::ostringstream ss;
        CacheStats l1_agg = getAggregatedL1Stats();
        CacheStats l2_agg = getAggregatedL2Stats();
        const auto& l3_stats = l3_shared_->getStats();
        
        ss << "{\n";
        ss << "  \"architecture\": \"multi-core\",\n";
        ss << "  \"num_cores\": " << num_cores_ << ",\n";
        ss << "  \"total_accesses\": " << total_accesses_.load() << ",\n";
        ss << "  \"memory_accesses\": " << memory_accesses_.load() << ",\n";
        ss << "  \"L1\": {\n";
        ss << "    \"type\": \"private\",\n";
        ss << "    \"size_per_core\": " << l1_caches_[0]->getSizeBytes() << ",\n";
        ss << "    \"total_hits\": " << l1_agg.hits.load() << ",\n";
        ss << "    \"total_misses\": " << l1_agg.misses.load() << ",\n";
        ss << "    \"hit_rate\": " << std::fixed << std::setprecision(6) << l1_agg.hitRate() << "\n";
        ss << "  },\n";
        ss << "  \"L2\": {\n";
        ss << "    \"type\": \"private\",\n";
        ss << "    \"size_per_core\": " << l2_caches_[0]->getSizeBytes() << ",\n";
        ss << "    \"total_hits\": " << l2_agg.hits.load() << ",\n";
        ss << "    \"total_misses\": " << l2_agg.misses.load() << ",\n";
        ss << "    \"hit_rate\": " << std::fixed << std::setprecision(6) << l2_agg.hitRate() << "\n";
        ss << "  },\n";
        ss << "  \"L3\": {\n";
        ss << "    \"type\": \"shared\",\n";
        ss << "    \"size_total\": " << l3_shared_->getSizeBytes() << ",\n";
        ss << "    \"hits\": " << l3_stats.hits.load() << ",\n";
        ss << "    \"misses\": " << l3_stats.misses.load() << ",\n";
        ss << "    \"hit_rate\": " << std::fixed << std::setprecision(6) << l3_stats.hitRate() << "\n";
        ss << "  },\n";
        ss << "  \"per_core\": [\n";
        for (int i = 0; i < num_cores_; i++) {
            ss << "    {\"core\": " << i 
               << ", \"accesses\": " << core_accesses_[i]
               << ", \"memory_accesses\": " << core_memory_accesses_[i] << "}";
            if (i < num_cores_ - 1) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }

    // Get stats as feature vector for perceptron
    std::vector<double> getFeatures() const {
        CacheStats l1_agg = getAggregatedL1Stats();
        CacheStats l2_agg = getAggregatedL2Stats();
        const auto& l3s = l3_shared_->getStats();
        
        double total = total_accesses_.load();
        double mem = memory_accesses_.load();
        
        return {
            l1_agg.hitRate(),
            l2_agg.hitRate(),
            l3s.hitRate(),
            total > 0 ? mem / total : 0.0,  // DRAM access rate
            l1_agg.totalAccesses() > 0 ? (double)l1_agg.evictions / l1_agg.totalAccesses() : 0.0,
            l2_agg.totalAccesses() > 0 ? (double)l2_agg.evictions / l2_agg.totalAccesses() : 0.0,
            l3s.totalAccesses() > 0 ? (double)l3s.evictions / l3s.totalAccesses() : 0.0,
        };
    }

private:
    static size_t getEnvSize(const char* name, size_t default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        
        char* end;
        size_t result = std::strtoul(val, &end, 10);
        
        // Handle K, M, G suffixes
        if (*end == 'K' || *end == 'k') result *= 1024;
        else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;
        
        return result > 0 ? result : default_val;
    }

    int num_cores_;
    size_t line_size_;
    bool enabled_;
    
    std::vector<std::unique_ptr<CacheLevel>> l1_caches_;  // Private L1 per core
    std::vector<std::unique_ptr<CacheLevel>> l2_caches_;  // Private L2 per core
    std::unique_ptr<CacheLevel> l3_shared_;               // Shared L3
    
    std::atomic<uint64_t> total_accesses_{0};
    std::atomic<uint64_t> memory_accesses_{0};
    
    // Per-core statistics
    std::vector<uint64_t> core_accesses_;
    std::vector<uint64_t> core_memory_accesses_;
};

// ============================================================================
// SAMPLED Cache Hierarchy - Uses statistical sampling for ~5-20x speedup
// Samples every Nth access and extrapolates results
// ============================================================================
class SampledCacheHierarchy {
public:
    SampledCacheHierarchy(
        size_t sample_rate = 8,  // Sample 1 in N accesses
        size_t l1_size = 32 * 1024,
        size_t l1_ways = 8,
        size_t l2_size = 256 * 1024,
        size_t l2_ways = 8,
        size_t l3_size = 8 * 1024 * 1024,
        size_t l3_ways = 16,
        size_t line_size = 64
    ) : sample_rate_(sample_rate), line_size_(line_size), enabled_(true), counter_(0) {
        l1_ = std::make_unique<FastCacheLevel>("L1", l1_size, line_size, l1_ways);
        l2_ = std::make_unique<FastCacheLevel>("L2", l2_size, line_size, l2_ways);
        l3_ = std::make_unique<FastCacheLevel>("L3", l3_size, line_size, l3_ways);
        
        // Use prime-based sampling to avoid patterns
        sample_mask_ = sample_rate_ - 1;  // Works best when sample_rate is power of 2
    }

    static SampledCacheHierarchy fromEnvironment() {
        size_t sample_rate = getEnvSize("CACHE_SAMPLE_RATE", 8);
        // Round to power of 2
        size_t sr = 1;
        while (sr < sample_rate) sr <<= 1;
        sample_rate = sr;
        
        size_t l1_size = getEnvSize("CACHE_L1_SIZE", 32 * 1024);
        size_t l1_ways = getEnvSize("CACHE_L1_WAYS", 8);
        size_t l2_size = getEnvSize("CACHE_L2_SIZE", 256 * 1024);
        size_t l2_ways = getEnvSize("CACHE_L2_WAYS", 8);
        size_t l3_size = getEnvSize("CACHE_L3_SIZE", 8 * 1024 * 1024);
        size_t l3_ways = getEnvSize("CACHE_L3_WAYS", 16);
        size_t line_size = getEnvSize("CACHE_LINE_SIZE", 64);
        
        return SampledCacheHierarchy(sample_rate, l1_size, l1_ways, l2_size, l2_ways,
                                     l3_size, l3_ways, line_size);
    }

    // Sampled access - only simulates every Nth access
    __attribute__((always_inline))
    inline void access(uint64_t address, bool is_write = false) {
        if (!enabled_) return;
        
        total_accesses_++;
        
        // Fast modulo for power-of-2 sample rate
        if ((counter_++ & sample_mask_) != 0) return;
        
        // This access is sampled - simulate it
        address = address & ~(line_size_ - 1);
        
        if (l1_->access(address)) return;
        if (l2_->access(address)) { l1_->insert(address); return; }
        if (l3_->access(address)) { l2_->insert(address); l1_->insert(address); return; }
        
        sampled_memory_accesses_++;
        l3_->insert(address);
        l2_->insert(address);
        l1_->insert(address);
    }

    inline void accessStream(uint64_t address, bool is_write = false) {
        access(address, is_write);
    }
    inline void accessStructuralStream(
            uint64_t address, bool is_write = false) {
        access(address, is_write);
    }

    template<typename T>
    inline void read(const T* ptr) { access(reinterpret_cast<uint64_t>(ptr), false); }

    template<typename T>
    inline void write(T* ptr) { access(reinterpret_cast<uint64_t>(ptr), true); }

    template<typename T>
    inline void readArray(const T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), false);
    }

    template<typename T>
    inline void writeArray(T* arr, size_t index) {
        access(reinterpret_cast<uint64_t>(&arr[index]), true);
    }

    void resetStats() {
        l1_->resetStats();
        l2_->resetStats();
        l3_->resetStats();
        total_accesses_ = 0;
        sampled_memory_accesses_ = 0;
        counter_ = 0;
    }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // No-op: P-OPT/GRASP require CacheLevel-based hierarchy
    void setCurrentVertex(uint32_t) {}
    void prefetch(uint64_t address) { access(address, false); }
    void initGraphContext(const GraphCacheContext*) {}
    
    uint64_t getTotalAccesses() const { return total_accesses_; }
    uint64_t getSampleRate() const { return sample_rate_; }
    
    // Extrapolated memory accesses
    uint64_t getMemoryAccesses() const { 
        return sampled_memory_accesses_ * sample_rate_; 
    }

    void printStats(std::ostream& os = std::cout) const {
        auto& l1 = l1_->getStats();
        auto& l2 = l2_->getStats();
        auto& l3 = l3_->getStats();
        
        os << "\n";
        os << "╔══════════════════════════════════════════════════════════════════╗\n";
        os << "║          SAMPLED CACHE SIMULATION RESULTS (1:" << sample_rate_ << " sampling)        ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        
        printSampledLevelStats(os, "L1", l1);
        printSampledLevelStats(os, "L2", l2);
        printSampledLevelStats(os, "L3", l3);
        
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ SUMMARY (Extrapolated from " << std::setw(3) << (100.0/sample_rate_) << "% sample)                       ║\n";
        os << "╠══════════════════════════════════════════════════════════════════╣\n";
        os << "║ Total Accesses:      " << std::setw(15) << total_accesses_ 
           << "                          ║\n";
        os << "║ Sampled Accesses:    " << std::setw(15) << (total_accesses_ / sample_rate_)
           << "                          ║\n";
        uint64_t mem = getMemoryAccesses();
        os << "║ Memory Accesses:     " << std::setw(15) << mem
           << " (extrapolated)           ║\n";
        double overall = total_accesses_ > 0 ? 
            (1.0 - (double)mem / total_accesses_) : 0.0;
        os << "║ Overall Hit Rate:    " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (overall * 100) << "%"
           << "                          ║\n";
        os << "╚══════════════════════════════════════════════════════════════════╝\n";
    }

    std::string toJSON() const {
        auto& l1 = l1_->getStats();
        auto& l2 = l2_->getStats();
        auto& l3 = l3_->getStats();
        
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"mode\": \"sampled\",\n";
        ss << "  \"sample_rate\": " << sample_rate_ << ",\n";
        ss << "  \"total_accesses\": " << total_accesses_ << ",\n";
        ss << "  \"sampled_accesses\": " << (total_accesses_ / sample_rate_) << ",\n";
        ss << "  \"memory_accesses\": " << getMemoryAccesses() << ",\n";
        ss << "  \"L1\": { \"hits\": " << (l1.hits.load() * sample_rate_) 
           << ", \"misses\": " << (l1.misses.load() * sample_rate_)
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l1.hitRate() << " },\n";
        ss << "  \"L2\": { \"hits\": " << (l2.hits.load() * sample_rate_)
           << ", \"misses\": " << (l2.misses.load() * sample_rate_)
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l2.hitRate() << " },\n";
        ss << "  \"L3\": { \"hits\": " << (l3.hits.load() * sample_rate_)
           << ", \"misses\": " << (l3.misses.load() * sample_rate_)
           << ", \"hit_rate\": " << std::fixed << std::setprecision(6) << l3.hitRate() << " }\n";
        ss << "}";
        return ss.str();
    }

private:
    static size_t getEnvSize(const char* name, size_t default_val) {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        char* end;
        size_t result = std::strtoul(val, &end, 10);
        if (*end == 'K' || *end == 'k') result *= 1024;
        else if (*end == 'M' || *end == 'm') result *= 1024 * 1024;
        else if (*end == 'G' || *end == 'g') result *= 1024 * 1024 * 1024;
        return result > 0 ? result : default_val;
    }

    void printSampledLevelStats(std::ostream& os, const std::string& name, 
                                 const CacheStats& stats) const {
        os << "╠──────────────────────────────────────────────────────────────────╣\n";
        os << "║ " << name << " Cache (sampled)                                           ║\n";
        os << "║   Hits:                " << std::setw(15) << (stats.hits.load() * sample_rate_)
           << " (extrapolated)           ║\n";
        os << "║   Misses:              " << std::setw(15) << (stats.misses.load() * sample_rate_)
           << " (extrapolated)           ║\n";
        os << "║   Hit Rate:            " << std::setw(14) << std::fixed 
           << std::setprecision(4) << (stats.hitRate() * 100) << "%"
           << "                          ║\n";
    }

    std::unique_ptr<FastCacheLevel> l1_;
    std::unique_ptr<FastCacheLevel> l2_;
    std::unique_ptr<FastCacheLevel> l3_;
    size_t sample_rate_;
    size_t sample_mask_;
    size_t line_size_;
    bool enabled_;
    uint64_t counter_ = 0;
    uint64_t total_accesses_ = 0;
    uint64_t sampled_memory_accesses_ = 0;
};

// ============================================================================
// Global Cache Simulator Instances
// ============================================================================

// Single-core cache (original behavior - with locks, slower)
inline CacheHierarchy& GlobalCache() {
    static CacheHierarchy cache = CacheHierarchy::fromEnvironment();
    return cache;
}

// FAST single-core cache (no locks, ~10-20x faster)
inline FastCacheHierarchy& GlobalFastCache() {
    static FastCacheHierarchy cache = FastCacheHierarchy::fromEnvironment();
    return cache;
}

// ULTRA-FAST single-core cache (packed structures, ~2-3x faster than Fast)
inline UltraFastCacheHierarchy& GlobalUltraFastCache() {
    static UltraFastCacheHierarchy cache = UltraFastCacheHierarchy::fromEnvironment();
    return cache;
}

// Multi-core cache (for realistic architecture simulation)
inline MultiCoreCacheHierarchy& GlobalMultiCoreCache() {
    static MultiCoreCacheHierarchy cache = MultiCoreCacheHierarchy::fromEnvironment();
    return cache;
}

// SAMPLED cache (statistical sampling for ~5-20x speedup)
inline SampledCacheHierarchy& GlobalSampledCache() {
    static SampledCacheHierarchy cache = SampledCacheHierarchy::fromEnvironment();
    return cache;
}

// Check if multi-core mode is enabled via environment
inline bool TOptForcesAccurateMode() {
    static const bool enabled = std::getenv("T_OPT") != nullptr;
    static bool announced = false;
    if (enabled && !announced) {
        announced = true;
        std::cerr << "[T_OPT] forcing accurate single-core cache hierarchy\n";
    }
    return enabled;
}

inline bool IsMultiCoreMode() {
    static int mode = -1;
    if (mode < 0) {
        const char* val = std::getenv("CACHE_MULTICORE");
        mode = TOptForcesAccurateMode() ? 0 :
            (val && (std::string(val) == "1" ||
                     std::string(val) == "true")) ? 1 : 0;
    }
    return mode == 1;
}

// Check if SAMPLED mode is enabled via environment
inline bool IsSampledMode() {
    static int mode = -1;
    if (mode < 0) {
        const char* val = std::getenv("CACHE_SAMPLED");
        mode = TOptForcesAccurateMode() ? 0 :
            (val && (std::string(val) == "1" ||
                     std::string(val) == "true")) ? 1 : 0;
    }
    return mode == 1;
}

// Check if ULTRA-FAST mode is enabled via environment (default: true for best performance)
inline bool IsUltraFastMode() {
    static int mode = -1;
    if (mode < 0) {
        const char* val = std::getenv("CACHE_ULTRAFAST");
        // T-OPT requires the accurate hierarchy's complete L3 input stream.
        mode = TOptForcesAccurateMode() ? 0 :
            (val && (std::string(val) == "0" ||
                     std::string(val) == "false")) ? 0 : 1;
    }
    return mode == 1;
}

// Check if FAST mode is enabled via environment
inline bool IsFastMode() {
    static int mode = -1;
    if (mode < 0) {
        const char* val = std::getenv("CACHE_FAST");
        mode = TOptForcesAccurateMode() ? 0 :
            (val && (std::string(val) == "1" ||
                     std::string(val) == "true")) ? 1 : 0;
    }
    return mode == 1;
}

// ============================================================================
// Convenience Macros for Instrumentation
// ============================================================================
#ifdef CACHE_SIM_ENABLED

// Auto-select single-core or multi-core based on CACHE_MULTICORE env var
#define CACHE_READ(ptr) do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().read(ptr); \
    else cache_sim::GlobalCache().read(ptr); \
} while(0)

#define CACHE_WRITE(ptr) do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().write(ptr); \
    else cache_sim::GlobalCache().write(ptr); \
} while(0)

#define CACHE_READ_ARRAY(arr, idx) do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().readArray(arr, idx); \
    else cache_sim::GlobalCache().readArray(arr, idx); \
} while(0)

#define CACHE_WRITE_ARRAY(arr, idx) do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().writeArray(arr, idx); \
    else cache_sim::GlobalCache().writeArray(arr, idx); \
} while(0)

#define CACHE_READ_RANGE(arr, start, end) cache_sim::GlobalCache().readRange(arr, start, end)

#define CACHE_ACCESS(addr, is_write) do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().access(addr, is_write); \
    else cache_sim::GlobalCache().access(addr, is_write); \
} while(0)

#define CACHE_RESET() do { \
    cache_sim::GlobalCache().resetStats(); \
    cache_sim::GlobalMultiCoreCache().resetStats(); \
} while(0)

#define CACHE_PRINT() do { \
    if (cache_sim::IsMultiCoreMode()) cache_sim::GlobalMultiCoreCache().printStats(); \
    else cache_sim::GlobalCache().printStats(); \
} while(0)

#define CACHE_JSON() (cache_sim::IsMultiCoreMode() ? \
    cache_sim::GlobalMultiCoreCache().toJSON() : cache_sim::GlobalCache().toJSON())

#define CACHE_FEATURES() (cache_sim::IsMultiCoreMode() ? \
    cache_sim::GlobalMultiCoreCache().getFeatures() : cache_sim::GlobalCache().getFeatures())

#else

#define CACHE_READ(ptr) ((void)0)
#define CACHE_WRITE(ptr) ((void)0)
#define CACHE_READ_ARRAY(arr, idx) ((void)0)
#define CACHE_WRITE_ARRAY(arr, idx) ((void)0)
#define CACHE_READ_RANGE(arr, start, end) ((void)0)
#define CACHE_ACCESS(addr, is_write) ((void)0)
#define CACHE_RESET() ((void)0)
#define CACHE_PRINT() ((void)0)
#define CACHE_JSON() std::string("{}")
#define CACHE_FEATURES() std::vector<double>()

#endif

}  // namespace cache_sim

#endif  // CACHE_SIM_H_
