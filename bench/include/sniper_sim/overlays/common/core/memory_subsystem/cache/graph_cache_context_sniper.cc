#include "graph_cache_context_sniper.h"
#include "ecg_victim_policy.h"  // shared GRASP insertion-tier classifier

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace graphbrew {
namespace sniper {

namespace {

// Tier A sideband-registration sanity log.  Mirrors the cache_sim / gem5
// variants. Suppress with GRAPHBREW_SIDEBAND_LOG=0.
bool graphCtxRegistrationLogEnabled()
{
    static int enabled = []() {
        const char* value = std::getenv("GRAPHBREW_SIDEBAND_LOG");
        if (!value || !value[0]) return 1;
        return (std::strcmp(value, "0") == 0) ? 0 : 1;
    }();
    return enabled != 0;
}

bool reuse_planLookupProfileEnabled()
{
    static int enabled = []() {
        const char* value = std::getenv("SNIPER_REUSE_PLAN_LOOKUP_PROFILE");
        return value && value[0] && std::strcmp(value, "0") != 0 ? 1 : 0;
    }();
    return enabled != 0;
}

void logGraphCtxRegistration(const char* source,
                             const char* name,
                             uint64_t base,
                             uint64_t upper,
                             uint32_t hot_pct,
                             bool grasp_region)
{
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

std::array<std::atomic<uint32_t>, MAX_TRACKED_CORES>& vertexStorage()
{
    static std::array<std::atomic<uint32_t>, MAX_TRACKED_CORES> storage{};
    return storage;
}

std::array<std::atomic<bool>, MAX_TRACKED_CORES>& vertexValidStorage()
{
    static std::array<std::atomic<bool>, MAX_TRACKED_CORES> storage{};
    return storage;
}

std::array<std::atomic<uint32_t>, MAX_TRACKED_CORES>& fallbackVertexStorage()
{
    static std::array<std::atomic<uint32_t>, MAX_TRACKED_CORES> storage{};
    return storage;
}

std::array<std::atomic<uint16_t>, MAX_TRACKED_CORES>& epochStorage()
{
    static std::array<std::atomic<uint16_t>, MAX_TRACKED_CORES> storage{};
    return storage;
}

std::array<std::atomic<bool>, MAX_TRACKED_CORES>& epochValidStorage()
{
    static std::array<std::atomic<bool>, MAX_TRACKED_CORES> storage{};
    return storage;
}

std::array<std::atomic<bool>, MAX_TRACKED_CORES>& prefetchTargetValidStorage()
{
    static std::array<std::atomic<bool>, MAX_TRACKED_CORES> storage{};
    return storage;
}

uint32_t clampVertex(uint64_t vertex)
{
    return vertex > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(vertex);
}

uint64_t parseJsonUint(const std::string& json, const std::string& key)
{
    size_t pos = json.find(key);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return std::strtoull(json.c_str() + pos, nullptr, 10);
}

std::string parseJsonString(const std::string& json, const std::string& key)
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

template <typename T>
bool loadBinaryVector(const std::string& path, std::vector<T>& out)
{
    out.clear();
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    const std::streamsize bytes = file.tellg();
    if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(T)) != 0)
        return false;
    out.resize(static_cast<size_t>(bytes) / sizeof(T));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), bytes);
    return file.good();
}

bool parseJsonBool(const std::string& json, const std::string& key)
{
    size_t pos = json.find(key);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return json.compare(pos, 4, "true") == 0;
}

}  // namespace

void setCurrentVertexHint(uint32_t core_id, uint64_t vertex)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    const uint32_t clamped = clampVertex(vertex);
    epochValidStorage()[core_id].store(
        false, std::memory_order_release);
    vertexStorage()[core_id].store(clamped, std::memory_order_release);
    vertexValidStorage()[core_id].store(true, std::memory_order_release);
    const auto& context = globalContext();
    if (context.loaded && context.topology.num_vertices > 0) {
        epochStorage()[core_id].store(
            quantizeEcgEpoch(
                clamped, context.topology.num_vertices,
                context.edge_epoch_count),
            std::memory_order_relaxed);
        epochValidStorage()[core_id].store(true, std::memory_order_release);
    } else {
        epochValidStorage()[core_id].store(false, std::memory_order_release);
    }
}

bool hasCurrentVertexHint(uint32_t core_id)
{
    return core_id < MAX_TRACKED_CORES &&
        vertexValidStorage()[core_id].load(std::memory_order_acquire);
}

bool hasAnyCurrentVertexHint()
{
    for (uint32_t core_id = 0; core_id < MAX_TRACKED_CORES; ++core_id) {
        if (vertexValidStorage()[core_id].load(std::memory_order_acquire))
            return true;
    }
    return false;
}

uint32_t getCurrentVertexHint(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return 0;
    return vertexStorage()[core_id].load(std::memory_order_acquire);
}

void clearCurrentVertexHint(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    vertexValidStorage()[core_id].store(false, std::memory_order_release);
    epochValidStorage()[core_id].store(false, std::memory_order_release);
}

static uint32_t& currentNucaRequesterCoreStorage()
{
    static thread_local uint32_t requester_core = UINT32_MAX;
    return requester_core;
}

void setCurrentNucaRequesterCore(uint32_t core_id)
{
    currentNucaRequesterCoreStorage() = core_id;
}

uint32_t currentNucaRequesterCore()
{
    return currentNucaRequesterCoreStorage();
}

// === Per-core prefetch-target hint ring buffer ===
//
// Previously used a single atomic<uint32_t> mailbox per core. Kernel
// emits thousands of hints per PR iteration; the L2 prefetcher only
// runs getNextAddress() on cache notification events. With a single
// slot, each new kernel hint OVERWROTE the prior unconsumed hint —
// ~99% of hints were lost on email-Eu-core (38 issued of ~2360
// emitted). Ring buffer of N entries lets the kernel queue up to N
// hints between prefetcher invocations.

static constexpr std::size_t kHintQueueSize = 256;

struct PerCoreHintQueue {
    std::array<std::atomic<uint32_t>, kHintQueueSize> entries{};
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

std::array<PerCoreHintQueue, MAX_TRACKED_CORES>& prefetchTargetHintQueues()
{
    static std::array<PerCoreHintQueue, MAX_TRACKED_CORES> queues;
    return queues;
}

void setPrefetchTargetHint(uint32_t core_id, uint64_t vertex)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& q = prefetchTargetHintQueues()[core_id];
    std::size_t t = q.tail.load(std::memory_order_relaxed);
    std::size_t next = (t + 1) % kHintQueueSize;
    if (next == q.head.load(std::memory_order_acquire)) {
        // Queue full: drop oldest by advancing head one slot.
        q.head.store((q.head.load(std::memory_order_relaxed) + 1) % kHintQueueSize,
                     std::memory_order_release);
    }
    q.entries[t].store(clampVertex(vertex), std::memory_order_relaxed);
    q.tail.store(next, std::memory_order_release);
    // Keep the legacy "valid" flag alive for has/getPrefetchTargetHint
    // callers that may exist outside the consume path. It now means
    // "queue is non-empty."
    prefetchTargetValidStorage()[core_id].store(true, std::memory_order_release);
}

bool hasPrefetchTargetHint(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return false;
    auto& q = prefetchTargetHintQueues()[core_id];
    return q.head.load(std::memory_order_acquire) !=
           q.tail.load(std::memory_order_acquire);
}

uint32_t getPrefetchTargetHint(uint32_t core_id)
{
    // Returns the oldest entry without consuming it. Used for
    // diagnostics; callers wanting to consume should call
    // consumePrefetchTargetHint instead.
    if (core_id >= MAX_TRACKED_CORES) return 0;
    auto& q = prefetchTargetHintQueues()[core_id];
    std::size_t h = q.head.load(std::memory_order_acquire);
    if (h == q.tail.load(std::memory_order_acquire)) return 0;
    return q.entries[h].load(std::memory_order_acquire);
}

bool consumePrefetchTargetHint(uint32_t core_id, uint32_t& vertex)
{
    if (core_id >= MAX_TRACKED_CORES) return false;
    auto& q = prefetchTargetHintQueues()[core_id];
    std::size_t h = q.head.load(std::memory_order_relaxed);
    if (h == q.tail.load(std::memory_order_acquire)) {
        prefetchTargetValidStorage()[core_id].store(false, std::memory_order_release);
        return false;
    }
    vertex = q.entries[h].load(std::memory_order_relaxed);
    std::size_t next = (h + 1) % kHintQueueSize;
    q.head.store(next, std::memory_order_release);
    if (next == q.tail.load(std::memory_order_acquire)) {
        prefetchTargetValidStorage()[core_id].store(false, std::memory_order_release);
    }
    return true;
}

void clearPrefetchTargetHint(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& q = prefetchTargetHintQueues()[core_id];
    q.head.store(0, std::memory_order_release);
    q.tail.store(0, std::memory_order_release);
    prefetchTargetValidStorage()[core_id].store(false, std::memory_order_release);
}

// === SNIPER_ECG_EXTRACT per-core epoch map (direct-mapped by cache line) ===
// The builder delivers line-min epochs, so keying the bounded map by line avoids
// a stale fallback to another vertex after a direct-mapped collision.
static constexpr std::size_t kEcgEpochMapSize = 8192;

struct PerCoreEpochMap {
    std::array<std::atomic<uint32_t>, kEcgEpochMapSize> line_plus1{};
    std::array<std::atomic<uint16_t>, kEcgEpochMapSize> epoch{};
    std::array<std::atomic<uint16_t>, kEcgEpochMapSize> epoch2{};
    std::array<std::atomic<uint8_t>, kEcgEpochMapSize> tier{};
    std::array<std::atomic<uint8_t>, kEcgEpochMapSize> count{};
    // Seqlock word: (global_delivery_sequence << 1) | write_in_progress.
    std::array<std::atomic<uint64_t>, kEcgEpochMapSize> version{};
};

static std::array<PerCoreEpochMap, MAX_TRACKED_CORES>& ecgEpochMaps()
{
    static std::array<PerCoreEpochMap, MAX_TRACKED_CORES> maps;
    return maps;
}

static std::atomic<uint64_t>& ecgEpochGlobalSequence()
{
    static std::atomic<uint64_t> sequence{0};
    return sequence;
}

static uint32_t ecgVerticesPerLine()
{
    static const uint32_t value = []() {
        const char* raw = std::getenv("SNIPER_ECG_VERTICES_PER_LINE");
        int parsed = raw ? std::atoi(raw) : 16;
        if (parsed < 1) parsed = 1;
        if (parsed > 1024) parsed = 1024;
        return static_cast<uint32_t>(parsed);
    }();
    return value;
}

void recordEcgEpoch(uint32_t core_id, uint32_t vertex, uint16_t epoch)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& m = ecgEpochMaps()[core_id];
    const uint32_t line = vertex / ecgVerticesPerLine();
    std::size_t i = line % kEcgEpochMapSize;
    uint64_t sequence =
        ecgEpochGlobalSequence().fetch_add(1, std::memory_order_relaxed) + 1;
    m.version[i].exchange(
        (sequence << 1) | 1u, std::memory_order_acq_rel);
    m.epoch[i].store(epoch, std::memory_order_relaxed);
    m.epoch2[i].store(epoch, std::memory_order_relaxed);
    m.tier[i].store(0, std::memory_order_relaxed);
    m.count[i].store(1, std::memory_order_relaxed);
    m.line_plus1[i].store(line + 1u, std::memory_order_relaxed);
    m.version[i].store(sequence << 1, std::memory_order_release);
}

void recordEcgReusePlan(uint32_t core_id, uint32_t vertex,
                        uint8_t tier, uint16_t first, uint16_t second)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    static std::atomic<uint64_t> trace_sequence{0};
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) : 0;
    }();
    const uint64_t sequence_index =
        trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence_index < trace_limit) {
        std::fprintf(stderr,
            "[ECG-ReusePlan-RECV sim=sniper seq=%llu dest=%u tier=%u "
            "epoch1=%u epoch2=%u]\n",
            (unsigned long long)sequence_index, vertex,
            static_cast<unsigned>(tier),
            static_cast<unsigned>(first), static_cast<unsigned>(second));
    }
    static std::atomic<uint32_t> debug_count{0};
    static std::atomic<uint32_t> debug_nonzero_count{0};
    const char* debug = std::getenv("ECG_DEBUG");
    uint32_t debug_index = debug_count.fetch_add(1, std::memory_order_relaxed);
    uint32_t debug_nonzero_index = (first != 0 || second != 0)
        ? debug_nonzero_count.fetch_add(1, std::memory_order_relaxed)
        : UINT32_MAX;
    if (debug && debug[0] && std::strcmp(debug, "0") != 0 &&
        (debug_index < 4 ||
         ((first != 0 || second != 0) && debug_nonzero_index < 4))) {
        std::fprintf(stderr,
                     "[ECG-DELIVER2 sim=sniper core=%u vertex=%u "
                     "tier=%u epoch1=%u epoch2=%u]\n",
                     core_id, vertex,
                     static_cast<unsigned>(tier),
                     static_cast<unsigned>(first),
                     static_cast<unsigned>(second));
    }
    auto& m = ecgEpochMaps()[core_id];
    const uint32_t line = vertex / ecgVerticesPerLine();
    std::size_t i = line % kEcgEpochMapSize;
    uint64_t sequence =
        ecgEpochGlobalSequence().fetch_add(1, std::memory_order_relaxed) + 1;
    m.version[i].exchange(
        (sequence << 1) | 1u, std::memory_order_acq_rel);
    m.epoch[i].store(first, std::memory_order_relaxed);
    m.epoch2[i].store(second, std::memory_order_relaxed);
    m.tier[i].store(tier, std::memory_order_relaxed);
    m.count[i].store(2, std::memory_order_relaxed);
    m.line_plus1[i].store(line + 1u, std::memory_order_relaxed);
    m.version[i].store(sequence << 1, std::memory_order_release);
}

void clearEcgReusePlan(uint32_t core_id, uint32_t vertex)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& m = ecgEpochMaps()[core_id];
    const uint32_t line = vertex / ecgVerticesPerLine();
    const std::size_t i = line % kEcgEpochMapSize;
    const uint64_t sequence =
        ecgEpochGlobalSequence().fetch_add(1, std::memory_order_relaxed) + 1;
    m.version[i].exchange(
        (sequence << 1) | 1u, std::memory_order_acq_rel);
    if (m.line_plus1[i].load(std::memory_order_relaxed) == line + 1u) {
        m.count[i].store(0, std::memory_order_relaxed);
        m.line_plus1[i].store(0, std::memory_order_relaxed);
    }
    m.version[i].store(sequence << 1, std::memory_order_release);
}

bool lookupEcgEpoch(uint32_t core_id, uint32_t vertex,
                    uint16_t& epoch, uint64_t& sequence)
{
    uint16_t second = 0;
    uint8_t tier = 0;
    uint8_t count = 0;
    return lookupEcgReusePlan(
        core_id, vertex, tier, epoch, second, count, sequence);
}

bool lookupEcgReusePlan(uint32_t core_id, uint32_t vertex,
                        uint8_t& tier, uint16_t& first, uint16_t& second,
                        uint8_t& count, uint64_t& sequence)
{
    if (core_id >= MAX_TRACKED_CORES) return false;
    auto& m = ecgEpochMaps()[core_id];
    const uint32_t line = vertex / ecgVerticesPerLine();
    std::size_t i = line % kEcgEpochMapSize;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint64_t before = m.version[i].load(std::memory_order_acquire);
        if (before == 0 || (before & 1u)) continue;
        uint32_t stored_line =
            m.line_plus1[i].load(std::memory_order_relaxed);
        uint16_t stored_first = m.epoch[i].load(std::memory_order_relaxed);
        uint16_t stored_second = m.epoch2[i].load(std::memory_order_relaxed);
        uint8_t stored_tier = m.tier[i].load(std::memory_order_relaxed);
        uint8_t stored_count = m.count[i].load(std::memory_order_relaxed);
        uint64_t after = m.version[i].load(std::memory_order_acquire);
        if (before != after || (after & 1u)) continue;
        if (stored_line != line + 1u) return false;
        first = stored_first;
        second = stored_second;
        tier = stored_tier;
        count = stored_count;
        sequence = after >> 1;
        return true;
    }
    return false;
}

ECGMode stringToECGMode(const std::string& text)
{
    const ECGMode mode = ecg_mode::parse(text);
    if (!ecg_mode::supportedByAllBackends(mode)) {
        std::fprintf(
            stderr, "[graphctx] FATAL: ECG mode '%s' is cache_sim-only\n",
            ecg_mode::name(mode));
        std::abort();
    }
    return mode;
}

std::string ecgModeToString(ECGMode mode)
{
    return ecg_mode::name(mode);
}

bool PropertyRegion::contains(uint64_t addr) const
{
    return addr >= base_address && addr < upper_bound;
}

uint32_t PropertyRegion::classifyBucket(uint64_t addr) const
{
    if (!contains(addr) || num_buckets == 0) return num_buckets;
    for (uint32_t bucket = 0; bucket < num_buckets; ++bucket) {
        if (addr < bucket_bounds[bucket]) return bucket;
    }
    return num_buckets - 1;
}

bool EdgeRegion::contains(uint64_t addr) const
{
    return addr >= base_address && addr < upper_bound;
}

bool RereferenceMatrix::loadFromFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.read(reinterpret_cast<char*>(&num_epochs), sizeof(num_epochs));
    file.read(reinterpret_cast<char*>(&num_cache_lines), sizeof(num_cache_lines));
    file.read(reinterpret_cast<char*>(&epoch_size), sizeof(epoch_size));
    file.read(reinterpret_cast<char*>(&sub_epoch_size), sizeof(sub_epoch_size));
    if (!file || num_epochs == 0 || num_cache_lines == 0) {
        enabled = false;
        return false;
    }

    size_t matrix_size = static_cast<size_t>(num_epochs) * num_cache_lines;
    if (num_cache_lines != 0 && matrix_size / num_cache_lines != num_epochs) {
        enabled = false;
        return false;
    }
    data.assign(matrix_size, 0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(matrix_size));
    enabled = static_cast<size_t>(file.gcount()) == matrix_size;
    return enabled;
}

uint32_t RereferenceMatrix::findNextRef(uint32_t cline_id, uint32_t current_vertex) const
{
    if (!enabled || cline_id >= num_cache_lines) return 127;
    struct PositionCache {
        const RereferenceMatrix* owner = nullptr;
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
        cache.epoch_id =
            epoch_size > 0 ? current_vertex / epoch_size : 0;
        cache.current_sub_epoch =
            epoch_size > 0 && sub_epoch_size > 0
                ? ((current_vertex % epoch_size) / sub_epoch_size)
                : 0;
    }
    const uint32_t epoch_id = cache.epoch_id;
    if (epoch_id >= num_epochs) return 127;

    uint8_t entry = data[static_cast<size_t>(epoch_id) * num_cache_lines + cline_id];
    constexpr uint8_t OR_MASK = 0x80;
    constexpr uint8_t AND_MASK = 0x7F;

    if ((entry & OR_MASK) != 0) {
        return entry & AND_MASK;
    } else {
        uint8_t last_ref_sub_epoch = entry & AND_MASK;
        const uint32_t current_sub_epoch = cache.current_sub_epoch;
        if (current_sub_epoch <= last_ref_sub_epoch) return 0;
        if (epoch_id + 1 < num_epochs) {
            uint8_t next_entry = data[static_cast<size_t>(epoch_id + 1) * num_cache_lines + cline_id];
            if ((next_entry & OR_MASK) == 0) return 1;
            uint8_t reref = next_entry & AND_MASK;
            return reref < 127 ? reref + 1 : 127;
        }
        return 127;
    }
}

uint32_t RereferenceMatrix::findNextRefByAddr(uint64_t addr, uint32_t current_vertex) const
{
    if (!enabled || addr < base_address) return 127;
    uint32_t cline_id = static_cast<uint32_t>((addr - base_address) / cache_line_size);
    return findNextRef(cline_id, current_vertex);
}

void MaskConfig::computeShifts()
{
    prefetch_shift = 0;
    popt_shift = prefetch_bits;
    dbg_shift = prefetch_bits + popt_bits;
    prefetch_mask_val = prefetch_bits ? ((1U << prefetch_bits) - 1) : 0;
    popt_mask_val = popt_bits ? (((1U << popt_bits) - 1) << popt_shift) : 0;
    dbg_mask_val = dbg_bits ? (((1U << dbg_bits) - 1) << dbg_shift) : 0;
}

uint8_t MaskConfig::decodeDBG(uint32_t mask_entry) const
{
    return dbg_bits ? static_cast<uint8_t>((mask_entry & dbg_mask_val) >> dbg_shift) : 0;
}

uint8_t MaskConfig::decodePOPT(uint32_t mask_entry) const
{
    return popt_bits ? static_cast<uint8_t>((mask_entry & popt_mask_val) >> popt_shift) : 0;
}

uint8_t MaskConfig::dbgTierToRRPV(uint8_t dbg_tier) const
{
    float fraction = static_cast<float>(dbg_tier) / std::max<uint8_t>(1, num_buckets);
    uint8_t result = static_cast<uint8_t>(rrpv_max * fraction);
    if (result > rrpv_max) result = rrpv_max;
    if (result == 0 && fraction > 0.0f) result = 1;
    return result;
}

GraphCacheContext::~GraphCacheContext()
{
    if (!reuse_planLookupProfileEnabled()) return;
    const uint64_t calls =
        reuse_plan_profile_calls.load(std::memory_order_relaxed);
    const uint64_t found =
        reuse_plan_profile_found.load(std::memory_order_relaxed);
    const uint64_t total =
        reuse_plan_profile_total_ns.load(std::memory_order_relaxed);
    const uint64_t classify =
        reuse_plan_profile_classify_ns.load(std::memory_order_relaxed);
    const uint64_t search =
        reuse_plan_profile_search_ns.load(std::memory_order_relaxed);
    std::fprintf(
        stderr,
        "[ReusePlan-LOOKUP-PROFILE calls=%llu found=%llu total_ns=%llu "
        "classify_ns=%llu search_ns=%llu avg_ns=%.3f]\n",
        (unsigned long long)calls,
        (unsigned long long)found,
        (unsigned long long)total,
        (unsigned long long)classify,
        (unsigned long long)search,
        calls ? static_cast<double>(total) / static_cast<double>(calls) : 0.0);
}

bool GraphCacheContext::loadFromSideband(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    topology.num_vertices = static_cast<uint32_t>(parseJsonUint(content, "\"num_vertices\""));
    topology.num_edges = parseJsonUint(content, "\"num_edges\"");
    flowthrough_base = parseJsonUint(content, "\"flowthrough_base\"");
    const uint64_t flowthrough_size =
        parseJsonUint(content, "\"flowthrough_size\"");
    flowthrough_upper = flowthrough_base + flowthrough_size;
    structural_flowthrough_base =
        parseJsonUint(content, "\"structural_flowthrough_base\"");
    const uint64_t structural_flowthrough_size =
        parseJsonUint(content, "\"structural_flowthrough_size\"");
    structural_flowthrough_upper =
        structural_flowthrough_base + structural_flowthrough_size;
    std::vector<uint64_t> raw_reuse_plan_records;
    const char* fused_reuse_plan = std::getenv("SNIPER_ECG_FUSED_REUSE_PLAN");
    const bool offsets_loaded = loadBinaryVector(
        parseJsonString(content, "\"reuse_plan_offsets_path\""), reuse_plan_offsets);
    const bool records_loaded = loadBinaryVector(
        parseJsonString(content, "\"reuse_plan_records_path\""), raw_reuse_plan_records);
    const bool fused_reuse_plan_enabled =
        fused_reuse_plan && fused_reuse_plan[0] && std::strcmp(fused_reuse_plan, "0") != 0;
    if (fused_reuse_plan_enabled &&
        (!offsets_loaded || !records_loaded ||
         reuse_plan_offsets.size() != static_cast<size_t>(topology.num_vertices) + 1 ||
        reuse_plan_offsets.empty() || reuse_plan_offsets.back() != raw_reuse_plan_records.size())) {
        std::fprintf(stderr,
            "[FATAL] Sniper fused ReusePlan sideband is missing or incomplete "
            "(offsets=%zu records=%zu vertices=%u)\n",
           reuse_plan_offsets.size(), raw_reuse_plan_records.size(),
           topology.num_vertices);
        std::abort();
    }
    const char* trace_value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
    const uint64_t trace_limit = trace_value
        ? std::strtoull(trace_value, nullptr, 10) : 0;
    if (fused_reuse_plan_enabled) {
        const uint64_t count = std::min<uint64_t>(
            trace_limit, raw_reuse_plan_records.size());
        for (uint64_t sequence = 0; sequence < count; ++sequence) {
            const uint64_t record = raw_reuse_plan_records[sequence];
            std::fprintf(stderr,
                "[ECG-ReusePlan-SIDEBAND sim=sniper seq=%llu dest=%u "
                "tier=%u epoch1=%u epoch2=%u]\n",
                (unsigned long long)sequence,
                static_cast<unsigned>(record & 0xFFFFFFFFULL),
                static_cast<unsigned>((record >> 32) & 0x3ULL),
                static_cast<unsigned>((record >> 34) & 0x7FFFULL),
                static_cast<unsigned>((record >> 49) & 0x7FFFULL));
        }
    }
    reuse_plan_line_offsets.clear();
    reuse_plan_line_ids.clear();
    reuse_plan_line_records.clear();
    reuse_plan_line_indices.clear();
    reuse_plan_line8_offsets.clear();
    reuse_plan_line8_ids.clear();
    reuse_plan_line8_records.clear();
    reuse_plan_line8_indices.clear();
    if (fused_reuse_plan_enabled) {
        struct IndexedReusePlanRecord {
            uint32_t line_id;
            uint64_t record;
            uint64_t raw_index;
        };
        auto build_line_index = [&](
                uint32_t vertices_per_line,
                std::vector<uint64_t>& offsets,
                std::vector<uint32_t>& ids,
                std::vector<uint64_t>& records,
                std::vector<uint64_t>& indices) {
            offsets.assign(
                static_cast<size_t>(topology.num_vertices) + 1, 0);
            std::vector<IndexedReusePlanRecord> source_lines;
            for (uint32_t src = 0; src < topology.num_vertices; ++src) {
                const uint64_t begin = reuse_plan_offsets[src];
                const uint64_t end = reuse_plan_offsets[src + 1];
                source_lines.clear();
                source_lines.reserve(static_cast<size_t>(end - begin));
                for (uint64_t index = begin; index < end; ++index) {
                    const uint64_t record = raw_reuse_plan_records[index];
                    if (((record >> 32) & 0x3ULL) == 0) continue;
                    source_lines.push_back({
                        static_cast<uint32_t>(record) / vertices_per_line,
                        record,
                        index,
                    });
                }
                std::stable_sort(
                    source_lines.begin(), source_lines.end(),
                    [](const IndexedReusePlanRecord& left,
                       const IndexedReusePlanRecord& right) {
                        return left.line_id < right.line_id;
                    });
                uint32_t previous_line = UINT32_MAX;
                for (const IndexedReusePlanRecord& indexed : source_lines) {
                    if (indexed.line_id == previous_line) {
                        if ((indexed.record >> 32) !=
                            (records.back() >> 32)) {
                            std::fprintf(
                                stderr,
                                "[FATAL] Sniper fused ReusePlan line has inconsistent "
                                "tier/epoch hints (src=%u line=%u vpl=%u)\n",
                                src, indexed.line_id, vertices_per_line);
                            std::abort();
                        }
                        continue;
                    }
                    ids.push_back(indexed.line_id);
                    records.push_back(indexed.record);
                    indices.push_back(indexed.raw_index);
                    previous_line = indexed.line_id;
                }
                offsets[src + 1] = records.size();
            }
        };
        const uint32_t primary_vpl = ecgVerticesPerLine();
        build_line_index(
            primary_vpl, reuse_plan_line_offsets, reuse_plan_line_ids,
            reuse_plan_line_records, reuse_plan_line_indices);
        if (primary_vpl != 8) {
            build_line_index(
                8, reuse_plan_line8_offsets, reuse_plan_line8_ids,
                reuse_plan_line8_records, reuse_plan_line8_indices);
        }
    }
    topology.max_degree = static_cast<uint32_t>(parseJsonUint(content, "\"max_degree\""));
    topology.avg_degree = topology.num_vertices > 0
        ? static_cast<double>(topology.num_edges) / topology.num_vertices
        : 0.0;
    topology.enabled = topology.num_vertices > 0;

    // ne for SNIPER_ECG_EXTRACT circular distance — same source as the kernel's
    // ECG_EDGE_MASK_EPOCHS packing.
    if (const char* ne_env = std::getenv("ECG_EDGE_MASK_EPOCHS")) {
        uint32_t ne = static_cast<uint32_t>(std::strtoul(ne_env, nullptr, 10));
        if (ne < 2) ne = 2;
        const char* schedule = std::getenv("ECG_REUSE_PLAN_DEPTH");
        const bool tiered_reuse_plan =
            schedule && std::strcmp(schedule, "2") == 0;
        const uint32_t max_epochs = tiered_reuse_plan ? 32768u : 65535u;
        if (ne > max_epochs) ne = max_epochs;
        edge_epoch_count = ne;
    }

    num_regions = 0;
    size_t pos = content.find("\"property_regions\"");
    if (pos != std::string::npos) {
        size_t arr_start = content.find('[', pos);
        size_t arr_end = content.find(']', arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string array_text = content.substr(arr_start, arr_end - arr_start + 1);
            size_t obj_pos = 0;
            while ((obj_pos = array_text.find('{', obj_pos)) != std::string::npos &&
                   num_regions < MAX_PROPERTY_REGIONS) {
                size_t obj_end = array_text.find('}', obj_pos);
                if (obj_end == std::string::npos) break;
                std::string obj = array_text.substr(obj_pos, obj_end - obj_pos + 1);

                PropertyRegion& region = regions[num_regions];
                region.name = parseJsonString(obj, "\"name\"");
                region.base_address = parseJsonUint(obj, "\"base\"");
                uint64_t size = parseJsonUint(obj, "\"size\"");
                region.upper_bound = region.base_address + size;
                region.num_elements = static_cast<uint32_t>(parseJsonUint(obj, "\"count\""));
                region.elem_size = static_cast<uint32_t>(parseJsonUint(obj, "\"elem_size\""));
                region.region_id = num_regions;
                region.grasp_region = obj.find("\"grasp\"") == std::string::npos ||
                    parseJsonBool(obj, "\"grasp\"");
                region.num_buckets = 3;

                uint64_t third = ((size / 3) + rereference.cache_line_size - 1) & ~(rereference.cache_line_size - 1);
                region.bucket_bounds[0] = region.base_address + third;
                region.bucket_bounds[1] = region.base_address + 2 * third;
                region.bucket_bounds[2] = region.upper_bound;

                // GRASP hot region = frontier_frac as % of the VERTEX SPACE
                // (array-relative, GRASP-faithful). Actual classification reads
                // GRASP_HOT_FRACTION (default 0.15) in classifyGRASP(); this is
                // just the logged registration value.
                constexpr uint32_t kSidebandHotPct = 15;
                logGraphCtxRegistration("sniper", region.name.c_str(),
                                        region.base_address,
                                        region.upper_bound,
                                        kSidebandHotPct,
                                        region.grasp_region);

                num_regions++;
                obj_pos = obj_end + 1;
            }
        }
    }

    num_edge_regions = 0;
    pos = content.find("\"edge_regions\"");
    if (pos != std::string::npos) {
        size_t arr_start = content.find('[', pos);
        size_t arr_end = content.find(']', arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string array_text = content.substr(arr_start, arr_end - arr_start + 1);
            size_t obj_pos = 0;
            while ((obj_pos = array_text.find('{', obj_pos)) != std::string::npos &&
                   num_edge_regions < edge_regions.size()) {
                size_t obj_end = array_text.find('}', obj_pos);
                if (obj_end == std::string::npos) break;
                std::string obj = array_text.substr(obj_pos, obj_end - obj_pos + 1);
                EdgeRegion& region = edge_regions[num_edge_regions];
                region.name = parseJsonString(obj, "\"name\"");
                region.base_address = parseJsonUint(obj, "\"base\"");
                uint64_t size = parseJsonUint(obj, "\"size\"");
                region.upper_bound = region.base_address + size;
                region.elem_size = static_cast<uint32_t>(parseJsonUint(obj, "\"elem_size\""));
                region.preferred = parseJsonBool(obj, "\"preferred\"");
                region.data_path = parseJsonString(obj, "\"data_path\"");
                num_edge_regions++;
                obj_pos = obj_end + 1;
            }
        }
    }

    loaded = num_regions > 0;
    mask_config.computeShifts();
    return loaded;
}

bool GraphCacheContext::loadRereferenceMatrix(const std::string& path)
{
    return rereference.loadFromFile(path);
}

void GraphCacheContext::setCacheLineSize(uint64_t line_size)
{
    if (line_size == 0) return;
    rereference.cache_line_size = line_size;
}

uint32_t GraphCacheContext::currentVertexForPopt(uint32_t core_id) const
{
    if (hasCurrentVertexHint(core_id))
        return getCurrentVertexHint(core_id);
    if (core_id < MAX_TRACKED_CORES)
        return fallbackVertexStorage()[core_id].load(
            std::memory_order_acquire);
    return 0;
}

uint16_t GraphCacheContext::currentEcgEpoch(uint32_t core_id) const
{
    if (core_id < MAX_TRACKED_CORES &&
        epochValidStorage()[core_id].load(std::memory_order_acquire)) {
        return epochStorage()[core_id].load(std::memory_order_relaxed);
    }
    const uint16_t epoch = quantizeEcgEpoch(
        currentVertexForPopt(core_id), topology.num_vertices,
        edge_epoch_count);
    if (core_id < MAX_TRACKED_CORES) {
        epochStorage()[core_id].store(epoch, std::memory_order_relaxed);
        epochValidStorage()[core_id].store(true, std::memory_order_release);
    }
    return epoch;
}

void GraphCacheContext::updateVertexFromAddr(uint64_t addr, uint32_t core_id) const
{
    if (num_regions == 0 || !regions[0].contains(addr) || regions[0].elem_size == 0) return;
    uint32_t vertex = static_cast<uint32_t>((addr - regions[0].base_address) / regions[0].elem_size);
    if (core_id < MAX_TRACKED_CORES && !hasCurrentVertexHint(core_id)) {
        epochValidStorage()[core_id].store(
            false, std::memory_order_release);
        fallbackVertexStorage()[core_id].store(
            vertex, std::memory_order_release);
        epochStorage()[core_id].store(
            quantizeEcgEpoch(
                vertex, topology.num_vertices, edge_epoch_count),
            std::memory_order_relaxed);
        epochValidStorage()[core_id].store(true, std::memory_order_release);
    }
}

uint32_t GraphCacheContext::vertexForAddress(uint64_t addr) const
{
    // Search ALL property regions (e.g. scores AND contrib) — the eviction-
    // protected array is region[1] (contrib) for PR, so a region[0]-only check
    // would silently never stamp it. Mirrors isPropertyData/findNextRef.
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].elem_size != 0 && regions[i].contains(addr))
            return static_cast<uint32_t>((addr - regions[i].base_address) / regions[i].elem_size);
    }
    return UINT32_MAX;
}

// elem_size of the property region owning addr (0 if none); used to size the
// per-line vertex scan in lookupLineEcgEpoch.
uint32_t GraphCacheContext::propertyElemSizeForAddress(uint64_t addr) const
{
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].elem_size != 0 && regions[i].contains(addr))
            return regions[i].elem_size;
    }
    return 0;
}

bool GraphCacheContext::isPropertyData(uint64_t addr) const
{
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].contains(addr)) return true;
    }
    return false;
}

bool GraphCacheContext::isEcgEpochData(uint64_t addr) const
{
    const char* requested = std::getenv("SNIPER_ECG_EPOCH_REGION");
    if (requested && requested[0]) {
        std::string names(requested);
        size_t begin = 0;
        while (begin <= names.size()) {
            const size_t end = names.find(',', begin);
            const std::string name = names.substr(
                begin, end == std::string::npos
                    ? std::string::npos : end - begin);
            for (uint32_t i = 0; i < num_regions; ++i) {
                if (regions[i].name == name && regions[i].contains(addr))
                    return true;
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return false;
    }
    if (num_regions == 1) return regions[0].contains(addr);
    static const char* defaults[] = {
        "contrib", "parent", "dist", "depth", "path_counts", "comp"
    };
    for (const char* name : defaults) {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (regions[i].name == name && regions[i].contains(addr))
                return true;
        }
    }
    return false;
}

bool GraphCacheContext::isFlowThroughData(uint64_t addr) const
{
    return flowthrough_base < flowthrough_upper &&
           addr >= flowthrough_base && addr < flowthrough_upper;
}

bool GraphCacheContext::isStructuralFlowThroughData(uint64_t addr) const
{
    return structural_flowthrough_base < structural_flowthrough_upper &&
           addr >= structural_flowthrough_base &&
           addr < structural_flowthrough_upper;
}

bool GraphCacheContext::lookupFusedReusePlanPair(
        uint64_t line_addr, uint32_t core_id,
        uint8_t& tier, uint16_t& first, uint16_t& second,
        uint64_t trace_sequence) const
{
    using Clock = std::chrono::steady_clock;
    const bool profile = reuse_planLookupProfileEnabled();
    const auto total_start = profile ? Clock::now() : Clock::time_point{};
    struct TotalTimer {
        const GraphCacheContext* context;
        bool enabled;
        Clock::time_point start;
        ~TotalTimer() {
            if (!enabled) return;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count();
            context->reuse_plan_profile_total_ns.fetch_add(
                static_cast<uint64_t>(ns), std::memory_order_relaxed);
        }
    } total_timer{this, profile, total_start};
    if (profile)
        reuse_plan_profile_calls.fetch_add(1, std::memory_order_relaxed);
    if (reuse_plan_offsets.empty())
        return false;
    const auto classify_start = profile ? Clock::now() : Clock::time_point{};
    const uint32_t src = currentVertexForPopt(core_id);
    const uint32_t line_vertex = vertexForAddress(line_addr);
    if (line_vertex == UINT32_MAX) return false;
    const uint32_t elem_size = propertyElemSizeForAddress(line_addr);
    const uint32_t vertices_per_line =
        elem_size > 0 ? std::max<uint32_t>(1, 64 / elem_size)
                      : ecgVerticesPerLine();
    const bool use_line8 =
        vertices_per_line == 8 && !reuse_plan_line8_offsets.empty();
    const auto& line_offsets =
        use_line8 ? reuse_plan_line8_offsets : reuse_plan_line_offsets;
    const auto& line_ids = use_line8 ? reuse_plan_line8_ids : reuse_plan_line_ids;
    const auto& line_records =
        use_line8 ? reuse_plan_line8_records : reuse_plan_line_records;
    const auto& line_indices =
        use_line8 ? reuse_plan_line8_indices : reuse_plan_line_indices;
    if (line_offsets.empty() || line_ids.empty() ||
        line_records.empty() || line_indices.empty() ||
        static_cast<size_t>(src + 1) >= line_offsets.size())
        return false;
    const uint32_t line_id = line_vertex / vertices_per_line;
    if (profile) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - classify_start).count();
        reuse_plan_profile_classify_ns.fetch_add(
            static_cast<uint64_t>(ns), std::memory_order_relaxed);
    }
    const uint64_t indexed_begin = line_offsets[src];
    const uint64_t indexed_end = std::min<uint64_t>(
        line_offsets[src + 1], line_records.size());
    const auto search_start = profile ? Clock::now() : Clock::time_point{};
    const auto found = std::lower_bound(
        line_ids.begin() + indexed_begin,
        line_ids.begin() + indexed_end,
        line_id);
    if (profile) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - search_start).count();
        reuse_plan_profile_search_ns.fetch_add(
            static_cast<uint64_t>(ns), std::memory_order_relaxed);
    }
    if (found == line_ids.begin() + indexed_end || *found != line_id)
        return false;
    if (profile)
        reuse_plan_profile_found.fetch_add(1, std::memory_order_relaxed);
    const uint64_t indexed_position =
        static_cast<uint64_t>(found - line_ids.begin());
    const uint64_t record = line_records[indexed_position];
    const uint64_t raw_index = line_indices[indexed_position];
    const uint64_t raw_begin = reuse_plan_offsets[src];
    const uint64_t raw_end = reuse_plan_offsets[src + 1];
    const uint32_t dest = static_cast<uint32_t>(record);
    tier = static_cast<uint8_t>((record >> 32) & 0x3ULL);
    first = static_cast<uint16_t>((record >> 34) & 0x7FFFULL);
    second = static_cast<uint16_t>((record >> 49) & 0x7FFFULL);
    static std::atomic<uint64_t> fused_receipts{0};
    static const uint64_t fused_trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    static const bool validate_once = []() {
        const char* value = std::getenv("SNIPER_ECG_FUSED_VALIDATE");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    static std::atomic<bool> validation_emitted{false};
    const bool emit_validation =
        validate_once &&
        !validation_emitted.load(std::memory_order_relaxed) &&
        !validation_emitted.exchange(true, std::memory_order_relaxed);
    const uint64_t receipt =
        trace_sequence != ~uint64_t{0}
            ? trace_sequence
            : fused_trace_limit > 0
                ? fused_receipts.fetch_add(1, std::memory_order_relaxed)
                : 0;
    if ((fused_trace_limit > 0 && receipt < fused_trace_limit) ||
        emit_validation) {
        std::fprintf(stderr,
            "[ECG-ReusePlan-FUSED-RECV sim=sniper seq=%llu src=%u "
            "line=%u addr_line=0x%llx vpl=%u index=%llu begin=%llu end=%llu "
            "dest=%u tier=%u epoch1=%u epoch2=%u]\n",
            (unsigned long long)receipt, src,
            line_id, (unsigned long long)line_addr, vertices_per_line,
            (unsigned long long)raw_index,
            (unsigned long long)raw_begin,
            (unsigned long long)raw_end,
            dest,
            static_cast<unsigned>(tier),
            static_cast<unsigned>(first),
            static_cast<unsigned>(second));
    }
    return true;
}

bool GraphCacheContext::isEdgeData(uint64_t addr) const
{
    for (uint32_t i = 0; i < num_edge_regions; ++i) {
        if (edge_regions[i].contains(addr)) return true;
    }
    return false;
}

uint32_t GraphCacheContext::classifyBucket(uint64_t addr) const
{
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].contains(addr)) return regions[i].classifyBucket(addr);
    }
    return mask_config.num_buckets;
}

uint32_t GraphCacheContext::findNextRef(uint64_t addr, uint32_t core_id) const
{
    if (!rereference.enabled) return 127;
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].contains(addr)) {
            uint32_t cline_id = static_cast<uint32_t>(
                (addr - regions[i].base_address) / rereference.cache_line_size);
            return rereference.findNextRef(
                cline_id, currentVertexForPopt(core_id));
        }
    }
    return 127;
}

uint32_t GraphCacheContext::findNextRefAtVertex(
        uint64_t addr, uint32_t current_vertex) const
{
    if (!rereference.enabled) return 127;
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (regions[i].contains(addr)) {
            uint32_t cline_id = static_cast<uint32_t>(
                (addr - regions[i].base_address) / rereference.cache_line_size);
            return rereference.findNextRef(cline_id, current_vertex);
        }
    }
    return 127;
}

uint32_t GraphCacheContext::classifyGRASP(uint64_t addr, uint64_t llc_size) const
{
    // GRASP-faithful (ligra.h add_region): the hot region is a fraction of the
    // VERTEX SPACE (frontier_frac x n) = a fraction of the property ARRAY, not of
    // the LLC. Auto-scales with graph size. Default ~0.15 (~Faldu's vertex-relative
    // "10%"). Override via GRASP_HOT_FRACTION (0<f<=1) for sensitivity sweeps.
    static const double hot_fraction = [](){
        const char* e = std::getenv("GRASP_HOT_FRACTION");
        double v = e ? std::atof(e) : 0.15;
        return (v > 0.0 && v <= 1.0) ? v : 0.15;
    }();
    (void)llc_size;
    for (uint32_t i = 0; i < num_regions; ++i) {
        if (!regions[i].grasp_region) continue;
        // Per-region tier math shared with cache_sim and gem5.
        uint32_t tier = ecg_policy::classifyGraspTier(
            addr, regions[i].base_address, regions[i].upper_bound, hot_fraction);
        if (tier != 0) return tier;
    }
    return 3;
}

uint8_t GraphCacheContext::getInsertRRPV(uint64_t addr) const
{
    uint32_t bucket = classifyBucket(addr);
    if (bucket >= mask_config.num_buckets) return mask_config.rrpv_max;
    return mask_config.dbgTierToRRPV(static_cast<uint8_t>(bucket));
}

GraphCacheContext& globalContext()
{
    static GraphCacheContext context;
    return context;
}

namespace {
struct BoundReusePlanLoadState {
    std::array<std::atomic<uint64_t>, MAX_TRACKED_CORES> address{};
    std::array<std::atomic<uint16_t>, MAX_TRACKED_CORES> current_epoch{};
    std::array<std::atomic<uint16_t>, MAX_TRACKED_CORES> context_id{};
    std::array<std::atomic<bool>, MAX_TRACKED_CORES> valid{};
    std::array<std::atomic<bool>, MAX_TRACKED_CORES>
        certification_finished{};
};

uint64_t boundReusePlanTraceLimit()
{
    static const uint64_t limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    return limit;
}

std::atomic<uint64_t>& boundReusePlanConsumeSequence()
{
    static std::atomic<uint64_t> sequence{0};
    return sequence;
}

BoundReusePlanLoadState& boundReusePlanLoadState()
{
    static BoundReusePlanLoadState state;
    return state;
}

std::atomic<uint32_t>& nextEcgContextId()
{
    static std::atomic<uint32_t> next{1};
    return next;
}

std::atomic<uint16_t>& activeEcgContextId()
{
    static std::atomic<uint16_t> active{0};
    return active;
}

std::atomic<uint64_t>& certifiedReusePlanFallbacks()
{
    static std::atomic<uint64_t> uses{0};
    return uses;
}
}

void beginEcgContext()
{
    const uint32_t context =
        nextEcgContextId().fetch_add(1, std::memory_order_relaxed);
    if (context == 0 || context > UINT16_MAX) {
        std::fprintf(
            stderr,
            "[FATAL] Sniper ECG context ID space exhausted; reuse is disabled\n");
        std::abort();
    }
    activeEcgContextId().store(
        static_cast<uint16_t>(context), std::memory_order_release);
    certifiedReusePlanFallbacks().store(0, std::memory_order_relaxed);
    auto& state = boundReusePlanLoadState();
    for (uint32_t core_id = 0; core_id < MAX_TRACKED_CORES; ++core_id) {
        vertexValidStorage()[core_id].store(
            false, std::memory_order_relaxed);
        epochValidStorage()[core_id].store(
            false, std::memory_order_relaxed);
        fallbackVertexStorage()[core_id].store(
            0, std::memory_order_relaxed);
        state.valid[core_id].store(false, std::memory_order_relaxed);
        state.certification_finished[core_id].store(
            false, std::memory_order_relaxed);
    }
}

void endEcgContext()
{
    const uint64_t fallback_uses =
        certifiedReusePlanFallbacks().exchange(
            0, std::memory_order_relaxed);
    if (fallback_uses > 0) {
        std::fprintf(
            stderr,
            "[ECG-ReusePlan-CERTIFIED-FALLBACK sim=sniper uses=%llu]\n",
            static_cast<unsigned long long>(fallback_uses));
    }
    activeEcgContextId().store(0, std::memory_order_release);
}

uint16_t currentEcgContextId()
{
    return activeEcgContextId().load(std::memory_order_acquire);
}

void recordBoundReusePlanLoad(uint32_t core_id, uint64_t address)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& state = boundReusePlanLoadState();
    state.address[core_id].store(address, std::memory_order_relaxed);
    state.current_epoch[core_id].store(
        globalContext().currentEcgEpoch(core_id),
        std::memory_order_relaxed);
    state.context_id[core_id].store(
        currentEcgContextId(), std::memory_order_relaxed);
    state.valid[core_id].store(true, std::memory_order_release);
}

void clearBoundReusePlanLoad(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    boundReusePlanLoadState().valid[core_id].store(
        false, std::memory_order_release);
}

void finishBoundReusePlanCertification(uint32_t core_id)
{
    if (core_id >= MAX_TRACKED_CORES) return;
    auto& state = boundReusePlanLoadState();
    state.valid[core_id].store(false, std::memory_order_relaxed);
    state.certification_finished[core_id].store(
        true, std::memory_order_release);
    std::fprintf(
        stderr,
        "[ECG-ReusePlan-CERTIFIED-PREFIX sim=sniper core=%u]\n",
        core_id);
}

bool boundReusePlanCertificationFinished(uint32_t core_id)
{
    return core_id < MAX_TRACKED_CORES &&
        boundReusePlanLoadState().certification_finished[core_id].load(
            std::memory_order_acquire);
}

void recordCertifiedReusePlanFallback()
{
    certifiedReusePlanFallbacks().fetch_add(
        1, std::memory_order_relaxed);
}

bool consumeBoundReusePlanLoad(
        uint32_t core_id, uint64_t line_addr, uint64_t line_size,
        uint16_t* current_epoch, uint16_t* context_id,
        uint64_t* trace_sequence)
{
    if (core_id >= MAX_TRACKED_CORES || line_size == 0) return false;
    auto& state = boundReusePlanLoadState();
    if (!state.valid[core_id].load(std::memory_order_acquire)) return false;
    const uint64_t address =
        state.address[core_id].load(std::memory_order_relaxed);
    const uint64_t bound_line = address & ~(line_size - 1);
    if (bound_line != line_addr) return false;
    if (!state.valid[core_id].exchange(
            false, std::memory_order_acq_rel)) {
        return false;
    }
    if (current_epoch) {
        *current_epoch = state.current_epoch[core_id].load(
            std::memory_order_relaxed);
    }
    if (context_id) {
        *context_id = state.context_id[core_id].load(
            std::memory_order_relaxed);
    }
    const uint64_t sequence =
        boundReusePlanConsumeSequence().fetch_add(1, std::memory_order_relaxed);
    if (trace_sequence) *trace_sequence = sequence;
    if (sequence < boundReusePlanTraceLimit()) {
        std::fprintf(
            stderr,
            "[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=%llu core=%u "
            "bound=0x%llx line=0x%llx size=%llu current=%u context=%u]\n",
            (unsigned long long)sequence, core_id,
            (unsigned long long)address,
            (unsigned long long)line_addr,
            (unsigned long long)line_size,
            current_epoch ? static_cast<unsigned>(*current_epoch) : 0u,
            context_id ? static_cast<unsigned>(*context_id) : 0u);
    }
    return context_id == nullptr || *context_id != 0;
}

bool isStructuralFlowThroughAddress(uint64_t addr)
{
    const char* enabled = std::getenv("STRUCTURAL_FLOWTHROUGH");
    if (!enabled || std::strcmp(enabled, "0") == 0) return false;
    GraphCacheContext& context = globalContext();
    if (!context.loaded) {
        const char* path = std::getenv("SNIPER_GRAPHBREW_CTX");
        if (!path || !path[0]) path = "/tmp/sniper_graphbrew_ctx.json";
        context.loaded = context.loadFromSideband(path);
    }
    return context.loaded && context.isStructuralFlowThroughData(addr);
}

bool isEcgFlowThroughAddress(uint64_t addr)
{
    const char* metadata_env = std::getenv("ECG_FLOWTHROUGH");
    const bool metadata_enabled =
        metadata_env && std::strcmp(metadata_env, "0") != 0;
    const char* structural_env = std::getenv("STRUCTURAL_FLOWTHROUGH");
    const bool structural_enabled =
        structural_env && std::strcmp(structural_env, "0") != 0;
    if (!metadata_enabled && !structural_enabled) return false;
    GraphCacheContext& context = globalContext();
    if (!context.loaded) {
        const char* path = std::getenv("SNIPER_GRAPHBREW_CTX");
        if (!path || !path[0]) path = "/tmp/sniper_graphbrew_ctx.json";
        context.loaded = context.loadFromSideband(path);
    }
    const bool metadata_match =
        metadata_enabled && context.loaded && context.isFlowThroughData(addr);
    const bool structural_match =
        structural_enabled && context.loaded &&
        context.isStructuralFlowThroughData(addr);
    static const bool adaptive = []() {
        const char* value = std::getenv("ECG_FLOWTHROUGH_ADAPTIVE");
        return value && std::strcmp(value, "0") != 0;
    }();
    if (adaptive) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(
                stderr, "[ECG-FLOWTHROUGH-ADAPTIVE sim=sniper active=1]\n");
        }
    }
    const uint64_t line_size =
        context.rereference.cache_line_size
            ? context.rereference.cache_line_size : 64;
    const size_t set_index =
        static_cast<size_t>(addr / line_size);
    const bool metadata_flowthrough = metadata_match && (
        !adaptive ||
        ecg_policy::globalOnlinePlacementSelector().shouldFlowThrough(set_index));
    const bool flowthrough = structural_match || metadata_flowthrough;
    static uint64_t probes = 0;
    static const uint64_t limit = []() {
        const char* value = std::getenv("ECG_FLOWTHROUGH_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    if (limit > 0 && probes++ < limit) {
        std::fprintf(stderr,
            "[ECG-FLOWTHROUGH-PROBE sim=sniper addr=%#llx base=%#llx "
            "upper=%#llx structural_base=%#llx structural_upper=%#llx "
            "loaded=%d metadata_match=%d structural_match=%d match=%d]\n",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(context.flowthrough_base),
            static_cast<unsigned long long>(context.flowthrough_upper),
            static_cast<unsigned long long>(
                context.structural_flowthrough_base),
            static_cast<unsigned long long>(
                context.structural_flowthrough_upper),
            context.loaded ? 1 : 0, metadata_match ? 1 : 0,
            structural_match ? 1 : 0, flowthrough ? 1 : 0);
    }
    static uint64_t ranged_probes = 0;
    if (limit > 0 &&
        context.flowthrough_base < context.flowthrough_upper &&
        ranged_probes++ < limit) {
        std::fprintf(stderr,
            "[ECG-FLOWTHROUGH-RANGED sim=sniper addr=%#llx base=%#llx "
            "upper=%#llx match=%d]\n",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(context.flowthrough_base),
            static_cast<unsigned long long>(context.flowthrough_upper),
            flowthrough ? 1 : 0);
    }
    return flowthrough;
}

void recordEcgPlacementMiss(uint64_t addr)
{
    const char* value = std::getenv("ECG_FLOWTHROUGH_ADAPTIVE");
    if (!value || std::strcmp(value, "0") == 0) return;
    GraphCacheContext& context = globalContext();
    if (!context.loaded) {
        const char* path = std::getenv("SNIPER_GRAPHBREW_CTX");
        if (!path || !path[0]) path = "/tmp/sniper_graphbrew_ctx.json";
        context.loaded = context.loadFromSideband(path);
    }
    if (!context.loaded || !context.isFlowThroughData(addr)) return;
    const uint64_t line_size =
        context.rereference.cache_line_size
            ? context.rereference.cache_line_size : 64;
    const size_t set_index =
        static_cast<size_t>(addr / line_size);
    ecg_policy::globalOnlinePlacementSelector().recordMiss(set_index);
}

}  // namespace sniper
}  // namespace graphbrew