// ============================================================================
// GraphCacheContext for gem5: Adapted metadata structures
// ============================================================================
//
// Provides the graph-aware metadata needed by GRASP, P-OPT, and ECG
// replacement policies running inside gem5. Mirrors the structures in
// bench/include/cache_sim/graph_cache_context.h but adapted for gem5's
// SimObject lifecycle and memory model.
//
// Key differences from standalone cache_sim version:
//   - Loaded from JSON sideband file (not inline C++ initialization)
//   - Uses gem5 physical addresses observed by the cache
//   - P-OPT rereference matrix stored host-side (oracle, not simulated memory)
//   - Per-access hints can be delivered by custom ECG instruction / CSR
//
// References:
//   - GRASP: Faldu et al. (2020)
//   - P-OPT: Balaji et al. (2021)
//   - ECG:   Mughrabi et al., GrAPL @ IPDPS 2026
// ============================================================================

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_CACHE_CONTEXT_GEM5_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_CACHE_CONTEXT_GEM5_HH__

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "mem/cache/replacement_policies/ecg_mode.hh"
// Shared GRASP insertion-tier classifier.
#include "mem/cache/replacement_policies/ecg_victim_policy.hh"

namespace gem5 {
namespace replacement_policy {
namespace graph {

// Tier A sideband-registration sanity log.  One line per region parsed from
// the sideband JSON, mirroring the cache_sim variant. Suppress with
// GRAPHBREW_SIDEBAND_LOG=0.
inline bool graphCtxRegistrationLogEnabled() {
    static int enabled = []() {
        const char* value = std::getenv("GRAPHBREW_SIDEBAND_LOG");
        if (!value || !value[0]) return 1;
        return (std::strcmp(value, "0") == 0) ? 0 : 1;
    }();
    return enabled != 0;
}

inline void logGraphCtxRegistration(const char* source,
                                    const char* name,
                                    uint64_t base,
                                    uint64_t upper,
                                    uint32_t hot_pct,
                                    bool grasp_region) {
    if (!graphCtxRegistrationLogEnabled()) return;
    std::fprintf(stderr,
                 "[graphctx] register region source=%s name=%s base=0x%lx "
                 "upper=0x%lx hot_pct=%u grasp_region=%d\n",
                 source ? source : "?",
                 (name && name[0]) ? name : "(unnamed)",
                 static_cast<unsigned long>(base),
                 static_cast<unsigned long>(upper),
                 hot_pct,
                 grasp_region ? 1 : 0);
}

static constexpr uint32_t MAX_REGION_BUCKETS = 16;
static constexpr uint32_t MAX_PROPERTY_REGIONS = 8;
static constexpr uint64_t GRAPHBREW_SET_VERTEX_WORK_ID = 0x47525654ULL;
static constexpr uint64_t GRAPHBREW_SET_CONTEXT_WORK_ID = 0x47435458ULL;
static constexpr uint64_t GRAPHBREW_ECG_PFX_TARGET_WORK_ID = 0x47504658ULL;
static constexpr uint64_t GRAPHBREW_ECG_EXTRACT_MASK_WORK_ID = 0x4745584DULL;
static constexpr uint64_t GRAPHBREW_ECG_EXTRACT2_WORK_ID = 0x47455832ULL;
// Path A (epoch-filtered DROPLET lookahead): a dedicated hint work-id that
// carries (target | epoch<<32) so the prefetched line can recover its
// candidate epoch at fill — distinct from the fat-mask work-id above (no
// >>24 ambiguity, no single-slot corruption, no 24-bit target truncation).
static constexpr uint64_t GRAPHBREW_ECG_PFX_TARGET_EPOCH_WORK_ID = 0x47504659ULL;

inline std::atomic<uint32_t>& currentVertexHintStorage() {
    static std::atomic<uint32_t> vertex{0};
    return vertex;
}

inline std::atomic<bool>& currentVertexHintValidStorage() {
    static std::atomic<bool> valid{false};
    return valid;
}

inline void setCurrentVertexHint(uint64_t vertex) {
    uint32_t clamped = vertex > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(vertex);
    currentVertexHintStorage().store(clamped, std::memory_order_release);
    currentVertexHintValidStorage().store(true, std::memory_order_release);
}

inline bool hasCurrentVertexHint() {
    return currentVertexHintValidStorage().load(std::memory_order_acquire);
}

inline uint32_t getCurrentVertexHint() {
    return currentVertexHintStorage().load(std::memory_order_acquire);
}

inline std::atomic<uint16_t>& currentContextHintStorage() {
    static std::atomic<uint16_t> context{0};
    return context;
}

inline void setCurrentContextHint(uint64_t context) {
    currentContextHintStorage().store(
        static_cast<uint16_t>(context), std::memory_order_release);
}

inline uint16_t getCurrentContextHint() {
    return currentContextHintStorage().load(std::memory_order_acquire);
}

// === Prefetch-target hint queue ===
//
// Earlier revision used a single atomic<uint32_t> mailbox. The kernel
// emits thousands of hints per PR iteration; the L2 prefetcher only
// runs calculatePrefetch on cache notification events. With a single
// slot, each new kernel hint OVERWRITES the prior unconsumed hint —
// ~99% of hints were lost on email-Eu-core (38 issued of ~2360 emitted).
//
// Ring buffer of N entries (default 256) lets the kernel queue up to
// N hints between prefetcher invocations. Reads (consume) are
// single-consumer (gem5 main-thread prefetcher); writes (set) are
// single-producer (kernel m5op handler in the same thread for SE-mode).
// Multi-producer multi-consumer is not required for SE-mode 1-core
// runs; the atomics are kept for the (rare) case where the prefetcher
// runs concurrently with hint emission.
inline constexpr std::size_t kHintQueueSize = 256;

struct HintQueueState {
    std::atomic<uint32_t> entries[kHintQueueSize];
    std::atomic<std::size_t> head{0};  // next consume index
    std::atomic<std::size_t> tail{0};  // next produce index
};

inline HintQueueState& prefetchTargetHintQueue() {
    static HintQueueState q;
    return q;
}

inline void setPrefetchTargetHint(uint64_t vertex) {
    uint32_t clamped = vertex > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(vertex);
    auto& q = prefetchTargetHintQueue();
    std::size_t t = q.tail.load(std::memory_order_relaxed);
    std::size_t next = (t + 1) % kHintQueueSize;
    if (next == q.head.load(std::memory_order_acquire)) {
        // Queue full — drop oldest entry by advancing head one slot,
        // then write the new entry. This preserves FIFO order for
        // the most-recent N entries (the kernel's recency window).
        q.head.store((q.head.load(std::memory_order_relaxed) + 1) % kHintQueueSize,
                     std::memory_order_release);
    }
    q.entries[t].store(clamped, std::memory_order_relaxed);
    q.tail.store(next, std::memory_order_release);
}

inline bool consumePrefetchTargetHint(uint32_t& vertex) {
    auto& q = prefetchTargetHintQueue();
    std::size_t h = q.head.load(std::memory_order_relaxed);
    if (h == q.tail.load(std::memory_order_acquire)) {
        return false;  // empty
    }
    vertex = q.entries[h].load(std::memory_order_relaxed);
    q.head.store((h + 1) % kHintQueueSize, std::memory_order_release);
    return true;
}

// === Path A in-flight prefetch-epoch buffer (HW-faithful, bounded) ===
//
// cache_sim Path A stamps each prefetched property line with its candidate's
// next-ref epoch so the ECG_GRASP_POPT eviction keeps it correctly. In gem5 the
// demand epoch rides the in-order ecg.extract single-slot mailbox, but a prefetch
// FILL is asynchronous, so the single-slot is stale by then. This is the HW
// reality of "the prefetch engine read the epoch from the edge word and carries
// it with the request": a small bounded buffer (direct-mapped by vertex, sized
// like an MSHR / prefetch-metadata array — NOT an O(V) table) holds (vertex ->
// {epoch, context}) from hint delivery until the fill consumes it. Collisions
// drop new pending metadata on collisions or context transitions (the line
// stays unstamped, re-stamped on demand). A repeated hint for the same vertex
// in the same context refreshes the epoch, matching latest-sequence merge
// semantics without allowing cross-context aliasing.
// a drop counter makes that observable.
inline constexpr std::size_t kPendingPfxEpochSize = 256;

struct PendingPfxEpochState {
    std::atomic<uint32_t> vertex[kPendingPfxEpochSize];  // UINT32_MAX = empty
    std::atomic<uint16_t> epoch[kPendingPfxEpochSize];
    std::atomic<uint16_t> context[kPendingPfxEpochSize];
    std::atomic<uint64_t> drops{0};
    PendingPfxEpochState() {
        for (std::size_t i = 0; i < kPendingPfxEpochSize; ++i)
            vertex[i].store(UINT32_MAX, std::memory_order_relaxed);
    }
};

inline PendingPfxEpochState& pendingPfxEpoch() {
    static PendingPfxEpochState s;
    return s;
}

inline void recordPendingPrefetchEpoch(
        uint32_t vtx, uint16_t ep, uint16_t context_id) {
    auto& s = pendingPfxEpoch();
    std::size_t i = vtx % kPendingPfxEpochSize;
    uint32_t prev = s.vertex[i].load(std::memory_order_relaxed);
    if (prev != UINT32_MAX) {
        const uint16_t previous_context =
            s.context[i].load(std::memory_order_relaxed);
        if (prev != vtx || previous_context != context_id) {
            s.drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s.epoch[i].store(ep, std::memory_order_relaxed);
    s.context[i].store(context_id, std::memory_order_relaxed);
    s.vertex[i].store(vtx, std::memory_order_release);
}

// One-shot lookup: returns pending metadata for vtx and clears the slot.
inline bool consumePendingPrefetchEpoch(
        uint32_t vtx, uint16_t expected_context,
        uint16_t& ep, uint16_t& context_id) {
    auto& s = pendingPfxEpoch();
    std::size_t i = vtx % kPendingPfxEpochSize;
    if (s.vertex[i].load(std::memory_order_acquire) != vtx) return false;
    context_id = s.context[i].load(std::memory_order_relaxed);
    if (context_id == 0 || context_id != expected_context) {
        s.vertex[i].store(UINT32_MAX, std::memory_order_release);
        return false;
    }
    ep = s.epoch[i].load(std::memory_order_relaxed);
    s.vertex[i].store(UINT32_MAX, std::memory_order_release);
    return true;
}

inline std::atomic<uint32_t>& decodedEcgRealVertexStorage() {
    static std::atomic<uint32_t> vertex{0};
    return vertex;
}

inline std::atomic<uint32_t>& decodedEcgMetadataStorage() {
    static std::atomic<uint32_t> metadata{0};
    return metadata;
}

inline std::atomic<bool>& decodedEcgHintValidStorage() {
    static std::atomic<bool> valid{false};
    return valid;
}

inline std::atomic<uint16_t>& decodedEcgEpochStorage() {
    static std::atomic<uint16_t> epoch{0};
    return epoch;
}

inline std::atomic<uint16_t>& decodedEcgEpoch2Storage() {
    static std::atomic<uint16_t> epoch{0};
    return epoch;
}

inline std::atomic<uint8_t>& decodedEcgEpochCountStorage() {
    static std::atomic<uint8_t> count{0};
    return count;
}

inline std::atomic<uint16_t>& decodedEcgCurrentEpochStorage() {
    static std::atomic<uint16_t> epoch{0};
    return epoch;
}

inline std::atomic<uint16_t>& decodedEcgContextStorage() {
    static std::atomic<uint16_t> context{0};
    return context;
}

inline std::atomic<uint32_t>& decodedEcgSequenceStorage() {
    static std::atomic<uint32_t> sequence{0};
    return sequence;
}

inline std::atomic<uint32_t>& decodedEcgSequenceCounter() {
    static std::atomic<uint32_t> sequence{0};
    return sequence;
}

inline void clearDecodedEcgExtractHint() {
    decodedEcgEpochCountStorage().store(0, std::memory_order_release);
    decodedEcgHintValidStorage().store(false, std::memory_order_release);
}

inline void setDecodedEcgExtractHint(uint32_t real_vertex,
                                     uint8_t dbg_hint,
                                     uint8_t popt_hint,
                                     uint16_t pfx_hint,
                                     uint16_t epoch_hint = 0,
                                     uint16_t current_epoch = 0,
                                     uint16_t context_id = 0) {
    uint32_t metadata = static_cast<uint32_t>(dbg_hint)
        | (static_cast<uint32_t>(popt_hint) << 8)
        | (static_cast<uint32_t>(pfx_hint) << 16);
    decodedEcgEpochStorage().store(epoch_hint, std::memory_order_release);
    decodedEcgEpoch2Storage().store(epoch_hint, std::memory_order_release);
    decodedEcgEpochCountStorage().store(1, std::memory_order_release);
    decodedEcgCurrentEpochStorage().store(
        current_epoch, std::memory_order_release);
    decodedEcgContextStorage().store(context_id, std::memory_order_release);
    decodedEcgSequenceStorage().store(
        decodedEcgSequenceCounter().fetch_add(
            1, std::memory_order_relaxed),
        std::memory_order_release);
    decodedEcgRealVertexStorage().store(real_vertex, std::memory_order_release);
    decodedEcgMetadataStorage().store(metadata, std::memory_order_release);
    decodedEcgHintValidStorage().store(true, std::memory_order_release);
}

inline void setDecodedEcgExtractHint2(
        uint32_t real_vertex, uint8_t tier,
        uint16_t first, uint16_t second, uint8_t width_bytes = 4,
        uint16_t current_epoch = 0, uint16_t context_id = 0) {
    if (tier == 0) {
        clearDecodedEcgExtractHint();
        return;
    }
    static std::atomic<uint64_t> trace_sequence{0};
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) : 0;
    }();
    const uint64_t sequence =
        trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence < trace_limit) {
        std::fprintf(stderr,
            "[ECG-ReusePlan-RECV sim=gem5 seq=%llu dest=%u tier=%u "
            "epoch1=%u epoch2=%u width=%u]\n",
            (unsigned long long)sequence, real_vertex,
            static_cast<unsigned>(tier),
            static_cast<unsigned>(first), static_cast<unsigned>(second),
            static_cast<unsigned>(width_bytes));
    }
    decodedEcgEpochStorage().store(first, std::memory_order_release);
    decodedEcgEpoch2Storage().store(second, std::memory_order_release);
    decodedEcgEpochCountStorage().store(2, std::memory_order_release);
    decodedEcgCurrentEpochStorage().store(
        current_epoch, std::memory_order_release);
    decodedEcgContextStorage().store(context_id, std::memory_order_release);
    decodedEcgSequenceStorage().store(
        decodedEcgSequenceCounter().fetch_add(
            1, std::memory_order_relaxed),
        std::memory_order_release);
    decodedEcgRealVertexStorage().store(real_vertex, std::memory_order_release);
    decodedEcgMetadataStorage().store(tier, std::memory_order_release);
    decodedEcgHintValidStorage().store(true, std::memory_order_release);
}

inline void setDecodedEcgExtractHint2Silent(
        uint32_t real_vertex, uint8_t tier,
        uint16_t first, uint16_t second,
        uint16_t current_epoch = 0, uint16_t context_id = 0) {
    if (tier == 0) {
        clearDecodedEcgExtractHint();
        return;
    }
    decodedEcgEpochStorage().store(first, std::memory_order_release);
    decodedEcgEpoch2Storage().store(second, std::memory_order_release);
    decodedEcgEpochCountStorage().store(2, std::memory_order_release);
    decodedEcgCurrentEpochStorage().store(
        current_epoch, std::memory_order_release);
    decodedEcgContextStorage().store(context_id, std::memory_order_release);
    decodedEcgSequenceStorage().store(
        decodedEcgSequenceCounter().fetch_add(
            1, std::memory_order_relaxed),
        std::memory_order_release);
    decodedEcgRealVertexStorage().store(real_vertex, std::memory_order_release);
    decodedEcgMetadataStorage().store(tier, std::memory_order_release);
    decodedEcgHintValidStorage().store(true, std::memory_order_release);
}

inline void traceExpectedEcgExtractHint2(
        uint64_t packed, uint8_t width_bytes = 4) {
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value
            ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10))
            : 0;
    }();
    if (trace_limit == 0) return;

    static std::atomic<uint64_t> trace_sequence{0};
    const uint64_t sequence =
        trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= trace_limit) return;

    std::fprintf(stderr,
        "[ECG-ReusePlan-EXPECT sim=gem5 seq=%llu dest=%u tier=%u "
        "epoch1=%u epoch2=%u width=%u]\n",
        (unsigned long long)sequence,
        static_cast<uint32_t>(packed),
        static_cast<unsigned>((packed >> 32) & 0x3ULL),
        static_cast<unsigned>((packed >> 34) & 0x7FFFULL),
        static_cast<unsigned>((packed >> 49) & 0x7FFFULL),
        static_cast<unsigned>(width_bytes));
}

inline void traceExpectedCompactWeightedEcgHint2(uint64_t packed) {
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value
            ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10))
            : 0;
    }();
    if (trace_limit == 0) return;

    static std::atomic<uint64_t> trace_sequence{0};
    const uint64_t sequence =
        trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= trace_limit) return;

    std::fprintf(stderr,
        "[ECG-ReusePlan-EXPECT sim=gem5 seq=%llu dest=%u tier=%u "
        "epoch1=%u epoch2=%u]\n",
        (unsigned long long)sequence,
        static_cast<uint32_t>(packed) & 0x00FFFFFFu,
        static_cast<unsigned>((packed >> 32) & 0x3ULL),
        static_cast<unsigned>((packed >> 34) & 0x7FFFULL),
        static_cast<unsigned>((packed >> 49) & 0x7FFFULL));
}

// Single-slot mailbox lookup for ECG_GRASP_POPT. Unlike the per-vertex table
// (which is fixed-size and collides badly past its capacity, corrupting the
// EXACT epoch ECG_GRASP_POPT depends on), this reads the LAST ecg.extract's
// epoch. On the in-order TimingSimpleCPU the extract for vertex v immediately
// precedes the demand load of property[v], so the mailbox holds exactly v's
// epoch when the demand reaches the LLC — collision-free and scales to any N
// (no per-vertex storage). The vertex check ensures we only stamp the line
// whose vertex was just extracted (non-extracted property writes are skipped).
inline bool lookupDecodedEcgHint(uint32_t vertex,
                                 uint8_t& dbg_out,
                                 uint8_t& popt_out,
                                 uint16_t& epoch_out) {
    if (!decodedEcgHintValidStorage().load(std::memory_order_acquire))
        return false;
    if (decodedEcgRealVertexStorage().load(std::memory_order_acquire) != vertex)
        return false;
    uint32_t md = decodedEcgMetadataStorage().load(std::memory_order_acquire);
    dbg_out = static_cast<uint8_t>(md & 0xFF);
    popt_out = static_cast<uint8_t>((md >> 8) & 0xFF);
    epoch_out = decodedEcgEpochStorage().load(std::memory_order_acquire);
    return true;
}

inline bool lookupDecodedEcgHint2(
        uint32_t vertex, uint8_t& tier,
        uint16_t& first, uint16_t& second, uint8_t& count) {
    if (!decodedEcgHintValidStorage().load(std::memory_order_acquire))
        return false;
    if (decodedEcgRealVertexStorage().load(std::memory_order_acquire) != vertex)
        return false;
    first = decodedEcgEpochStorage().load(std::memory_order_acquire);
    second = decodedEcgEpoch2Storage().load(std::memory_order_acquire);
    count = decodedEcgEpochCountStorage().load(std::memory_order_acquire);
    tier = static_cast<uint8_t>(
        decodedEcgMetadataStorage().load(std::memory_order_acquire) & 0x3u);
    const bool valid = count > 0;
    if (valid)
        clearDecodedEcgExtractHint();
    return valid;
}

inline bool lookupDecodedEcgRequestState(
        uint16_t& current_epoch, uint16_t& context_id, uint32_t& sequence) {
    if (!decodedEcgHintValidStorage().load(std::memory_order_acquire))
        return false;
    current_epoch = decodedEcgCurrentEpochStorage().load(
        std::memory_order_acquire);
    context_id = decodedEcgContextStorage().load(std::memory_order_acquire);
    sequence = decodedEcgSequenceStorage().load(std::memory_order_acquire);
    return context_id != 0;
}

// Per-vertex ECG metadata table.
//
// The legacy setDecodedEcgExtractHint above is a single-slot mailbox.
// For CHARGED=0 the replacement policy needs to look
// up DBG/POPT metadata BY VERTEX when a cache miss for property[v]
// is being resolved. A direct-mapped 4K-entry table provides
// constant-time lookup without dynamic allocation. The kernel emits
// hints in spatial order (PR pull: for u, for v in in_neigh(u))
// matching the cache miss pattern, so direct-mapped collisions are
// rare in practice.

inline constexpr std::size_t kEcgMetadataTableSize = 4096;

struct EcgMetadataEntry {
    std::atomic<uint32_t> vertex{UINT32_MAX};  // sentinel = invalid
    std::atomic<uint8_t>  dbg_tier{0};
    std::atomic<uint8_t>  popt_quant{0};
    std::atomic<uint16_t> epoch{0};
};

inline std::array<EcgMetadataEntry, kEcgMetadataTableSize>& ecgMetadataTable() {
    static std::array<EcgMetadataEntry, kEcgMetadataTableSize> table;
    return table;
}

inline void storeEcgMetadataByVertex(uint32_t vertex,
                                     uint8_t dbg_tier,
                                     uint8_t popt_quant,
                                     uint16_t epoch) {
    auto& entry = ecgMetadataTable()[vertex % kEcgMetadataTableSize];
    entry.dbg_tier.store(dbg_tier, std::memory_order_relaxed);
    entry.popt_quant.store(popt_quant, std::memory_order_relaxed);
    entry.epoch.store(epoch, std::memory_order_relaxed);
    // Store vertex LAST so a concurrent reader sees a coherent
    // (vertex, dbg, popt) triple — happens-before via the release on
    // vertex.
    entry.vertex.store(vertex, std::memory_order_release);
}

inline bool lookupEcgMetadataByVertex(uint32_t vertex,
                                      uint8_t& dbg_tier_out,
                                      uint8_t& popt_quant_out,
                                      uint16_t& epoch_out) {
    auto& entry = ecgMetadataTable()[vertex % kEcgMetadataTableSize];
    if (entry.vertex.load(std::memory_order_acquire) != vertex) {
        return false;  // miss (sentinel, evicted, or different vertex hashed to same slot)
    }
    dbg_tier_out  = entry.dbg_tier.load(std::memory_order_relaxed);
    popt_quant_out = entry.popt_quant.load(std::memory_order_relaxed);
    epoch_out = entry.epoch.load(std::memory_order_relaxed);
    return true;
}

// Address-to-vertex helper for ECG_RP. Property region base + elem_size
// come from the sideband JSON. Returns UINT32_MAX if addr is not in any
// known property region.
inline uint32_t addressToVertex(uint64_t addr,
                                uint64_t property_base,
                                uint64_t property_end,
                                uint32_t elem_size) {
    if (addr < property_base || addr >= property_end || elem_size == 0) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>((addr - property_base) / elem_size);
}


// ============================================================================
// ECGMode: Controls eviction tiebreaker priority
// ============================================================================
using ECGMode = ecg_mode::Mode;

inline ECGMode stringToECGMode(const std::string& s) {
    const ECGMode mode = ecg_mode::parse(s);
    if (!ecg_mode::supportedByAllBackends(mode)) {
        std::fprintf(
            stderr, "[graphctx] FATAL: ECG mode '%s' is cache_sim-only\n",
            ecg_mode::name(mode));
        std::abort();
    }
    return mode;
}

inline std::string ecgModeToString(ECGMode mode) {
    return ecg_mode::name(mode);
}

// ============================================================================
// PropertyRegion: One tracked vertex data array
// ============================================================================
struct PropertyRegion {
    std::string name;
    uint64_t base_address = 0;
    uint64_t upper_bound = 0;
    uint32_t num_elements = 0;
    uint32_t elem_size = 0;
    uint32_t region_id = 0;
    uint32_t num_buckets = 0;
    bool grasp_region = true;
    uint64_t bucket_bounds[MAX_REGION_BUCKETS] = {};

    uint32_t classifyBucket(uint64_t addr) const {
        if (addr < base_address || addr >= upper_bound || num_buckets == 0) {
            return num_buckets;
        }
        for (uint32_t bucket = 0; bucket < num_buckets; ++bucket) {
            if (addr < bucket_bounds[bucket]) return bucket;
        }
        return num_buckets - 1;
    }

    bool contains(uint64_t addr) const {
        return addr >= base_address && addr < upper_bound;
    }
};

struct GraphArrayRegion {
    uint64_t base_address = 0;
    uint64_t upper_bound = 0;

    bool contains(uint64_t addr) const {
        return base_address < upper_bound &&
               addr >= base_address && addr < upper_bound;
    }
};

enum class GraphArrayCategory : uint32_t {
    Property0 = 0,
    Property1,
    Record,
    EdgePreferred,
    EdgeOther,
    CsrOffsets,
    CsrOffsetsOther,
    PlanOffsets,
    Other,
    Unattributed,
    Count,
};

inline const char* graphArrayCategoryName(uint32_t category) {
    switch (static_cast<GraphArrayCategory>(category)) {
      case GraphArrayCategory::Property0: return "property0";
      case GraphArrayCategory::Property1: return "property1";
      case GraphArrayCategory::Record: return "record";
      case GraphArrayCategory::EdgePreferred: return "edge_preferred";
      case GraphArrayCategory::EdgeOther: return "edge_other";
      case GraphArrayCategory::CsrOffsets: return "csr_offsets";
      case GraphArrayCategory::CsrOffsetsOther:
        return "csr_offsets_other";
      case GraphArrayCategory::PlanOffsets: return "plan_offsets";
      case GraphArrayCategory::Other: return "other";
      case GraphArrayCategory::Unattributed: return "unattributed";
      case GraphArrayCategory::Count: break;
    }
    return "invalid";
}

inline constexpr uint32_t numGraphArrayCategories() {
    return static_cast<uint32_t>(GraphArrayCategory::Count);
}

// ============================================================================
// RereferenceMatrix: P-OPT oracle data (host-side, not in simulated memory)
// ============================================================================
struct RereferenceMatrix {
    std::vector<uint8_t> data;
    uint32_t num_cache_lines = 0;
    uint32_t num_epochs = 256;
    uint32_t epoch_size = 0;
    uint32_t sub_epoch_size = 0;
    uint64_t base_address = 0;
    uint64_t cache_line_size = 64;
    bool enabled = false;

    struct Position {
        uint32_t epoch_id = 0;
        uint32_t current_sub_epoch = 0;
    };

    Position position(uint32_t current_vertex) const {
        struct PositionCache {
            const RereferenceMatrix* owner = nullptr;
            uint32_t vertex = UINT32_MAX;
            uint32_t epoch_size = 0;
            uint32_t sub_epoch_size = 0;
            uint32_t num_epochs = 0;
            Position position;
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
            cache.position.epoch_id =
                epoch_size > 0 ? current_vertex / epoch_size : 0;
            cache.position.current_sub_epoch =
                epoch_size > 0 && sub_epoch_size > 0
                    ? ((current_vertex % epoch_size) / sub_epoch_size)
                    : 0;
        }
        return cache.position;
    }

    // P-OPT Algorithm 2 semantics using the official artifact convention:
    // MSB=0 means referenced in this epoch (final sub-epoch in low bits),
    // MSB=1 means not referenced (distance-to-next in low bits).
    uint32_t findNextRef(uint32_t cline_id, uint32_t current_vertex) const {
        if (!enabled || cline_id >= num_cache_lines) return 127;
        const Position current = position(current_vertex);
        const uint32_t epoch_id = current.epoch_id;
        if (epoch_id >= num_epochs) return 127;

        uint8_t entry = data[epoch_id * num_cache_lines + cline_id];
        constexpr uint8_t OR_MASK = 0x80;
        constexpr uint8_t AND_MASK = 0x7F;

        if ((entry & OR_MASK) != 0) {
            return entry & AND_MASK;
        } else {
            uint8_t last_ref_sub_epoch = entry & AND_MASK;
            const uint32_t current_sub_epoch =
                current.current_sub_epoch;
            if (current_sub_epoch <= last_ref_sub_epoch) return 0;
            if (epoch_id + 1 < num_epochs) {
                uint8_t next_entry = data[(epoch_id + 1) * num_cache_lines + cline_id];
                if ((next_entry & OR_MASK) == 0) return 1;
                uint8_t reref = next_entry & AND_MASK;
                return (reref < 127) ? reref + 1 : 127;
            }
            return 127;
        }
    }

    uint32_t findNextRefEpoch(uint32_t cline_id, uint32_t current_vertex) const {
        if (!enabled || cline_id >= num_cache_lines || num_epochs == 0) return 0;
        const Position current = position(current_vertex);
        uint32_t epoch_id = current.epoch_id;
        if (epoch_id >= num_epochs) epoch_id = num_epochs - 1;

        const uint32_t current_sub_epoch =
            current.current_sub_epoch;
        constexpr uint8_t OR_MASK = 0x80;
        constexpr uint8_t AND_MASK = 0x7F;

        for (uint32_t step = 0; step < num_epochs; ++step) {
            uint32_t epoch = (epoch_id + step) % num_epochs;
            uint8_t entry = data[epoch * num_cache_lines + cline_id];
            if ((entry & OR_MASK) != 0) continue;
            if (step == 0 && current_sub_epoch > (entry & AND_MASK)) continue;
            return epoch;
        }
        return (epoch_id + num_epochs - 1) % num_epochs;
    }

    uint32_t findNextRefByAddr(uint64_t addr, uint32_t current_vertex) const {
        if (!enabled || addr < base_address) return 127;
        uint32_t cline_id = static_cast<uint32_t>(
            (addr - base_address) / cache_line_size);
        return findNextRef(cline_id, current_vertex);
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        file.read(reinterpret_cast<char*>(&num_epochs), 4);
        file.read(reinterpret_cast<char*>(&num_cache_lines), 4);
        file.read(reinterpret_cast<char*>(&epoch_size), 4);
        file.read(reinterpret_cast<char*>(&sub_epoch_size), 4);

        size_t matrix_size = static_cast<size_t>(num_epochs) * num_cache_lines;
        data.resize(matrix_size);
        file.read(reinterpret_cast<char*>(data.data()), matrix_size);

        enabled = file.good();
        return enabled;
    }
};

// ============================================================================
// MaskConfig: ECG per-edge mask hint configuration
// ============================================================================
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

    void computeShifts() {
        prefetch_shift = 0;
        popt_shift = prefetch_bits;
        dbg_shift = prefetch_bits + popt_bits;
        prefetch_mask_val = prefetch_bits ? ((1U << prefetch_bits) - 1) : 0;
        popt_mask_val = popt_bits ? (((1U << popt_bits) - 1) << popt_shift) : 0;
        dbg_mask_val = dbg_bits ? (((1U << dbg_bits) - 1) << dbg_shift) : 0;
    }

    uint8_t decodeDBG(uint32_t mask_entry) const {
        return dbg_bits ? static_cast<uint8_t>((mask_entry & dbg_mask_val) >> dbg_shift) : 0;
    }

    uint8_t decodePOPT(uint32_t mask_entry) const {
        return popt_bits ? static_cast<uint8_t>((mask_entry & popt_mask_val) >> popt_shift) : 0;
    }

    uint8_t dbgTierToRRPV(uint8_t dbg_tier) const {
        float fraction = static_cast<float>(dbg_tier) / std::max(uint8_t(1), num_buckets);
        uint8_t result = static_cast<uint8_t>(rrpv_max * fraction);
        if (result > rrpv_max) result = rrpv_max;
        if (result == 0 && fraction > 0.0f) result = 1;
        return result;
    }
};

// ============================================================================
// GraphTopology: Degree distribution for bucket classification
// ============================================================================
struct GraphTopology {
    uint32_t num_vertices = 0;
    uint64_t num_edges = 0;
    uint32_t edge_epoch_count = 2;
    uint32_t num_buckets = 11;
    double avg_degree = 0.0;
    uint32_t bucket_vertex_counts[MAX_REGION_BUCKETS] = {};
    bool enabled = false;
};

// ============================================================================
// GraphCacheContext: Unified metadata for all graph-aware policies
// ============================================================================
struct GraphCacheContext {
    PropertyRegion regions[MAX_PROPERTY_REGIONS];
    uint32_t num_regions = 0;
    uint64_t flowthrough_base = 0;
    uint64_t flowthrough_upper = 0;
    uint32_t array_attribution_schema = 0;
    GraphArrayRegion edge_preferred;
    GraphArrayRegion edge_other;
    GraphArrayRegion csr_offsets;
    GraphArrayRegion csr_offsets_other;
    GraphArrayRegion plan_offsets;
    bool edge_regions_aliased = false;

    GraphTopology topology;
    MaskConfig mask_config;
    RereferenceMatrix rereference;

    uint32_t current_src_vertex = 0;
    mutable uint32_t current_dst_vertex = 0;
    uint8_t current_mask = 0;
    mutable uint32_t current_outer_vertex = 0;
    bool loaded = false;

    uint32_t currentVertexForPopt() const {
        if (hasCurrentVertexHint()) {
            uint32_t vertex = getCurrentVertexHint();
            current_dst_vertex = vertex;
            current_outer_vertex = vertex;
            return vertex;
        }
        return current_dst_vertex;
    }

    void updateVertexFromAddr(uint64_t addr) const {
        if (num_regions > 0 && regions[0].contains(addr) &&
            regions[0].elem_size > 0) {
            uint32_t vertex = static_cast<uint32_t>(
                (addr - regions[0].base_address) / regions[0].elem_size);
            current_dst_vertex = vertex;
            if (!hasCurrentVertexHint()) current_outer_vertex = vertex;
        }
    }

    bool isPropertyData(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].contains(addr)) return true;
        }
        return false;
    }

    bool isFlowThroughData(uint64_t addr) const {
        return flowthrough_base < flowthrough_upper &&
               addr >= flowthrough_base && addr < flowthrough_upper;
    }

    bool arrayAttributionReady() const {
        return loaded && array_attribution_schema == 2 &&
               num_regions == 2 &&
               !regions[0].name.empty() && !regions[1].name.empty();
    }

    GraphArrayCategory classifyArray(uint64_t addr) const {
        if (num_regions > 0 && regions[0].contains(addr))
            return GraphArrayCategory::Property0;
        if (num_regions > 1 && regions[1].contains(addr))
            return GraphArrayCategory::Property1;
        if (isFlowThroughData(addr))
            return GraphArrayCategory::Record;
        if (edge_preferred.contains(addr))
            return GraphArrayCategory::EdgePreferred;
        if (!edge_regions_aliased && edge_other.contains(addr))
            return GraphArrayCategory::EdgeOther;
        if (csr_offsets.contains(addr))
            return GraphArrayCategory::CsrOffsets;
        if (csr_offsets_other.contains(addr))
            return GraphArrayCategory::CsrOffsetsOther;
        if (plan_offsets.contains(addr))
            return GraphArrayCategory::PlanOffsets;
        return GraphArrayCategory::Other;
    }

    bool isEcgEpochData(uint64_t addr) const {
        const char* values =
            std::getenv("GEM5_ECG_EPOCH_REGION_INDICES");
        if (values && values[0]) {
            const char* cursor = values;
            while (*cursor) {
                char* end = nullptr;
                long index = std::strtol(cursor, &end, 10);
                if (end == cursor) break;
                if (index >= 0 &&
                    static_cast<unsigned long>(index) < num_regions &&
                    regions[index].contains(addr)) {
                    return true;
                }
                cursor = (*end == ',') ? end + 1 : end;
            }
            return false;
        }
        const char* requested = std::getenv("GEM5_ECG_EPOCH_REGION_INDEX");
        if (requested && requested[0]) {
            int index = std::atoi(requested);
            return index >= 0 && static_cast<uint32_t>(index) < num_regions &&
                   regions[index].contains(addr);
        }
        if (num_regions == 1) return regions[0].contains(addr);
        // PR registers scores first and governed contrib second.
        return num_regions > 1 && regions[1].contains(addr);
    }

    uint32_t classifyBucket(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].contains(addr)) return regions[i].classifyBucket(addr);
        }
        return mask_config.num_buckets;
    }

    uint32_t findNextRef(uint64_t addr) const {
        if (!rereference.enabled) return 127;
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].contains(addr)) {
                uint32_t cline_id = static_cast<uint32_t>(
                    (addr - regions[i].base_address) / rereference.cache_line_size);
                return rereference.findNextRef(cline_id, currentVertexForPopt());
            }
        }
        return 127;
    }

    uint32_t classifyGRASP(uint64_t addr, size_t llc_size,
                           double hot_fraction = 0.15) const {
        // GRASP-faithful: hot region = a fraction of the property ARRAY (vertex
        // space), auto-scaling with graph size. The per-region tier math is
        // ecg_policy::classifyGraspTier (shared with cache_sim + Sniper). ECG
        // DBG-tier callers use the default ~0.15 (~Faldu's vertex-relative 10%).
        (void)llc_size;
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (!regions[i].grasp_region) continue;
            uint32_t tier = ecg_policy::classifyGraspTier(
                addr, regions[i].base_address, regions[i].upper_bound, hot_fraction);
            if (tier != 0) return tier;
        }
        return 3;
    }

    uint8_t getInsertRRPV(uint64_t addr) const {
        uint32_t bucket = classifyBucket(addr);
        if (bucket >= mask_config.num_buckets) return mask_config.rrpv_max;
        return mask_config.dbgTierToRRPV(static_cast<uint8_t>(bucket));
    }

    // ECG "mask" variant: map a property cache-line address to its vertex id, so
    // the INSERTED LINE gets ITS OWN delivered tier (ecg_policy::graspTierByIndex)
    // — identical across simulators. UINT32_MAX if not in any property region.
    uint32_t addressToVertex(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].contains(addr) && regions[i].elem_size > 0)
                return graph::addressToVertex(addr, regions[i].base_address,
                                              regions[i].upper_bound, regions[i].elem_size);
        }
        return UINT32_MAX;
    }

    // ECG "mask" variant insertion tier: delivered per-vertex GRASP tier for the
    // line owning `addr`, BYTE-EXACT to classifyGRASP (passes elem_size so the +8
    // boundary matches). Cross-sim identical with cache_sim/Sniper.
    uint32_t maskGraspTier(uint64_t addr, double hot_fraction) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].contains(addr) && regions[i].elem_size > 0) {
                uint32_t vtx = graph::addressToVertex(addr, regions[i].base_address,
                                   regions[i].upper_bound, regions[i].elem_size);
                return ecg_policy::graspTierByIndex(vtx, topology.num_vertices,
                                                    hot_fraction, regions[i].elem_size);
            }
        }
        return 3;
    }

    bool loadFromSideband(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();

        topology.num_vertices = parseJsonUint(content, "\"num_vertices\"");
        topology.num_edges = parseJsonUint(content, "\"num_edges\"");
        topology.edge_epoch_count = parseJsonUint(content, "\"edge_epoch_count\"");
        flowthrough_base =
            parseJsonUint(content, "\"flowthrough_base\"");
        uint64_t flowthrough_size =
            parseJsonUint(content, "\"flowthrough_size\"");
        flowthrough_upper = flowthrough_base + flowthrough_size;
        array_attribution_schema = static_cast<uint32_t>(
            parseJsonUint(content, "\"array_attribution_schema\""));
        edge_preferred.base_address =
            parseJsonUint(content, "\"edge_preferred_base\"");
        edge_preferred.upper_bound = edge_preferred.base_address +
            parseJsonUint(content, "\"edge_preferred_size\"");
        edge_other.base_address =
            parseJsonUint(content, "\"edge_other_base\"");
        edge_other.upper_bound = edge_other.base_address +
            parseJsonUint(content, "\"edge_other_size\"");
        edge_regions_aliased =
            parseJsonBool(content, "\"edge_regions_aliased\"");
        csr_offsets.base_address =
            parseJsonUint(content, "\"csr_offsets_base\"");
        csr_offsets.upper_bound = csr_offsets.base_address +
            parseJsonUint(content, "\"csr_offsets_size\"");
        csr_offsets_other.base_address =
            parseJsonUint(content, "\"csr_offsets_other_base\"");
        csr_offsets_other.upper_bound = csr_offsets_other.base_address +
            parseJsonUint(content, "\"csr_offsets_other_size\"");
        plan_offsets.base_address =
            parseJsonUint(content, "\"plan_offsets_base\"");
        plan_offsets.upper_bound = plan_offsets.base_address +
            parseJsonUint(content, "\"plan_offsets_size\"");
        if (topology.edge_epoch_count < 2) topology.edge_epoch_count = 2;
        topology.avg_degree = (topology.num_vertices > 0)
            ? static_cast<double>(topology.num_edges) / topology.num_vertices : 0.0;
        topology.enabled = true;

        num_regions = 0;
        size_t pos = content.find("\"property_regions\"");
        if (pos != std::string::npos) {
            size_t arr_start = content.find('[', pos);
            size_t arr_end = content.find(']', arr_start);
            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string arr = content.substr(arr_start, arr_end - arr_start + 1);
                size_t obj_pos = 0;
                while ((obj_pos = arr.find('{', obj_pos)) != std::string::npos &&
                       num_regions < MAX_PROPERTY_REGIONS) {
                    size_t obj_end = arr.find('}', obj_pos);
                    if (obj_end == std::string::npos) break;
                    std::string obj = arr.substr(obj_pos, obj_end - obj_pos + 1);

                    regions[num_regions].name =
                        parseJsonString(obj, "\"name\"");
                    regions[num_regions].base_address = parseJsonUint(obj, "\"base\"");
                    uint64_t size = parseJsonUint(obj, "\"size\"");
                    regions[num_regions].upper_bound = regions[num_regions].base_address + size;
                    regions[num_regions].num_elements = static_cast<uint32_t>(
                        parseJsonUint(obj, "\"count\""));
                    regions[num_regions].elem_size = static_cast<uint32_t>(
                        parseJsonUint(obj, "\"elem_size\""));
                    regions[num_regions].region_id = num_regions;
                    regions[num_regions].grasp_region =
                        obj.find("\"grasp\"") == std::string::npos || parseJsonBool(obj, "\"grasp\"");

                    uint64_t region_bytes = size;
                    uint64_t third = (region_bytes / 3 + 63) & ~uint64_t(63);
                    regions[num_regions].num_buckets = 3;
                    regions[num_regions].bucket_bounds[0] = regions[num_regions].base_address + third;
                    regions[num_regions].bucket_bounds[1] = regions[num_regions].base_address + 2 * third;
                    regions[num_regions].bucket_bounds[2] = regions[num_regions].upper_bound;

                    // GRASP hot region = frontier_frac as % of VERTEX SPACE.
                    // LOG-ONLY value: the actual classification uses the
                    // GraphGraspRP hot_fraction Param / classifyGRASP default
                    // (0.15), matching cache_sim + Sniper. Keep 15 for an
                    // honest, equivalent log (was a misleading 50).
                    constexpr uint32_t kSidebandHotPct = 15;
                    logGraphCtxRegistration("gem5", nullptr,
                                            regions[num_regions].base_address,
                                            regions[num_regions].upper_bound,
                                            kSidebandHotPct,
                                            regions[num_regions].grasp_region);

                    num_regions++;
                    obj_pos = obj_end + 1;
                }
            }
        }

        loaded = (num_regions > 0);
        return loaded;
    }

private:
    static uint64_t parseJsonUint(const std::string& json, const std::string& key) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return 0;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return 0;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return std::strtoull(json.c_str() + pos, nullptr, 10);
    }

    static bool parseJsonBool(const std::string& json, const std::string& key) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return false;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return json.compare(pos, 4, "true") == 0;
    }

    static std::string parseJsonString(
            const std::string& json, const std::string& key) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos);
        if (pos == std::string::npos) return {};
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return {};
        const size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return json.substr(pos + 1, end - pos - 1);
    }
};

inline bool graphArrayAttributionEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("GEM5_GRAPH_ARRAY_STATS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline GraphCacheContext& arrayAttributionGraphContext() {
    static GraphCacheContext context;
    return context;
}

inline bool ensureArrayAttributionGraphContext() {
    auto& context = arrayAttributionGraphContext();
    if (context.loaded) return true;

    // Every guest exports this sideband once, after all tracked arrays exist.
    // Retry until that single export appears so even short ROIs cannot begin
    // with an unclassified prefix. This observer has no simulated latency.

    const char* path = std::getenv("GEM5_GRAPHBREW_CTX");
    if (!path || !path[0]) path = "/tmp/gem5_graphbrew_ctx.json";
    context.loaded = context.loadFromSideband(path);
    if (context.loaded && graphArrayAttributionEnabled()) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(
                stderr,
                "[ECG-ARRAY-ATTRIBUTION active=%d schema=%u "
                "categories=%u edge_regions_aliased=%d "
                "p0=%s:%#llx+%llu p1=%s:%#llx+%llu "
                "record=%#llx+%llu edge=%#llx+%llu "
                "edge_other=%#llx+%llu csr=%#llx+%llu "
                "csr_other=%#llx+%llu "
                "plan=%#llx+%llu]\n",
                context.arrayAttributionReady() ? 1 : 0,
                context.array_attribution_schema,
                numGraphArrayCategories(),
                context.edge_regions_aliased ? 1 : 0,
                context.num_regions > 0
                    ? context.regions[0].name.c_str() : "missing",
                context.num_regions > 0
                    ? static_cast<unsigned long long>(
                        context.regions[0].base_address) : 0ULL,
                context.num_regions > 0
                    ? static_cast<unsigned long long>(
                        context.regions[0].upper_bound -
                        context.regions[0].base_address) : 0ULL,
                context.num_regions > 1
                    ? context.regions[1].name.c_str() : "missing",
                context.num_regions > 1
                    ? static_cast<unsigned long long>(
                        context.regions[1].base_address) : 0ULL,
                context.num_regions > 1
                    ? static_cast<unsigned long long>(
                        context.regions[1].upper_bound -
                        context.regions[1].base_address) : 0ULL,
                static_cast<unsigned long long>(context.flowthrough_base),
                static_cast<unsigned long long>(
                    context.flowthrough_upper - context.flowthrough_base),
                static_cast<unsigned long long>(
                    context.edge_preferred.base_address),
                static_cast<unsigned long long>(
                    context.edge_preferred.upper_bound -
                    context.edge_preferred.base_address),
                static_cast<unsigned long long>(
                    context.edge_other.base_address),
                static_cast<unsigned long long>(
                    context.edge_other.upper_bound -
                    context.edge_other.base_address),
                static_cast<unsigned long long>(
                    context.csr_offsets.base_address),
                static_cast<unsigned long long>(
                    context.csr_offsets.upper_bound -
                    context.csr_offsets.base_address),
                static_cast<unsigned long long>(
                    context.csr_offsets_other.base_address),
                static_cast<unsigned long long>(
                    context.csr_offsets_other.upper_bound -
                    context.csr_offsets_other.base_address),
                static_cast<unsigned long long>(
                    context.plan_offsets.base_address),
                static_cast<unsigned long long>(
                    context.plan_offsets.upper_bound -
                    context.plan_offsets.base_address));
        }
    }
    return context.loaded;
}

inline GraphArrayCategory classifyEcgArray(
        bool has_vaddr, uint64_t vaddr) {
    if (!ensureArrayAttributionGraphContext() || !has_vaddr)
        return GraphArrayCategory::Unattributed;
    const auto& context = arrayAttributionGraphContext();
    if (!context.arrayAttributionReady())
        return GraphArrayCategory::Unattributed;
    return context.classifyArray(vaddr);
}

inline bool isEcgFlowThroughAddress(uint64_t addr) {
    const char* enabled = std::getenv("ECG_FLOWTHROUGH");
    if (!enabled || std::strcmp(enabled, "0") == 0) return false;
    static GraphCacheContext context;
    if (!context.loaded ||
        context.flowthrough_base >= context.flowthrough_upper) {
        const char* path = std::getenv("GEM5_GRAPHBREW_CTX");
        if (!path || !path[0]) path = "/tmp/gem5_graphbrew_ctx.json";
        context.loaded = context.loadFromSideband(path);
    }
    const bool match = context.loaded && context.isFlowThroughData(addr);
    static uint64_t probes = 0;
    static const uint64_t limit = []() {
        const char* value = std::getenv("ECG_FLOWTHROUGH_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    if (probes++ < limit) {
        std::fprintf(stderr,
            "[ECG-STREAM-PROBE sim=gem5 addr=%#llx base=%#llx "
            "upper=%#llx loaded=%d match=%d]\n",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(context.flowthrough_base),
            static_cast<unsigned long long>(context.flowthrough_upper),
            context.loaded ? 1 : 0, match ? 1 : 0);
    }
    static uint64_t ranged_probes = 0;
    if (context.flowthrough_base < context.flowthrough_upper &&
        ranged_probes++ < limit) {
        std::fprintf(stderr,
            "[ECG-STREAM-RANGED sim=gem5 addr=%#llx base=%#llx "
            "upper=%#llx match=%d]\n",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(context.flowthrough_base),
            static_cast<unsigned long long>(context.flowthrough_upper),
            match ? 1 : 0);
    }
    return match;
}

} // namespace graph
} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_GRAPH_CACHE_CONTEXT_GEM5_HH__
