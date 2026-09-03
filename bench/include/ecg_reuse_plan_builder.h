// ============================================================================
// ECG next-reference epoch builder (shared cache_sim/gem5/Sniper helpers)
// ============================================================================

#ifndef ECG_REUSE_PLAN_BUILDER_H
#define ECG_REUSE_PLAN_BUILDER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace ecg_reuse_plan {

static constexpr uint16_t kReusePlanEpochMask = 0x7FFFu;
static constexpr uint32_t kReusePlanMaxEpochCount = 1u << 15;
// Tier width carried by the COMPACT 32-bit record. The canonical 64-bit wire
// format always reserves 2 tier bits; the compact record may drop them.
static constexpr uint32_t kReusePlanDefaultTierBits = 2;
static constexpr uint32_t kReusePlanTierlessTierBits = 0;
static constexpr uint32_t kCompactWeightedDestMask = 0x00FFFFFFu;
static constexpr uint32_t kCompactWeightedMaxVertices = 1u << 24;
static constexpr uint32_t kCompactWeightedMaxWeight = 0xFFu;

struct ReusePlan {
    uint16_t first = 0;
    uint16_t second = 0;
    uint8_t tier = 0;
    bool valid = false;
};

inline bool reusePlan32TierBitsSupported(uint32_t tier_bits);
inline uint32_t reusePlan32TierMask(uint32_t tier_bits);
inline uint32_t reusePlan32IdBits(uint32_t num_vertices);
inline uint32_t extractReusePlan32Dest(uint32_t record, uint32_t id_bits);
inline uint8_t extractReusePlan32Tier(
    uint32_t record, uint32_t id_bits, uint32_t tier_bits);

enum class NextUseState : uint8_t {
    UNKNOWN = 0,
    FINITE = 1,
    DEAD = 2,
    WRAP = 3,
};

struct NextUsePlan {
    uint16_t next_use = 0;
    uint8_t tier = 0;
    NextUseState state = NextUseState::UNKNOWN;
};

static constexpr uint32_t kNextUseStateBits = 2;

inline bool canPackNextUseRecord32(
        uint32_t num_vertices, uint32_t next_use_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    if (!reusePlan32TierBitsSupported(tier_bits) ||
        next_use_bits == 0 || next_use_bits > 15) {
        return false;
    }
    return reusePlan32IdBits(num_vertices) + tier_bits +
        next_use_bits + kNextUseStateBits <= 32;
}

inline uint32_t packNextUseRecord32(
        uint32_t dest, uint8_t tier, uint16_t next_use,
        NextUseState state, uint32_t id_bits, uint32_t next_use_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    const uint32_t id_mask = id_bits >= 32
        ? 0xFFFFFFFFu : ((uint32_t{1} << id_bits) - 1u);
    const uint32_t tier_mask = reusePlan32TierMask(tier_bits);
    const uint32_t next_mask =
        (uint32_t{1} << next_use_bits) - 1u;
    const uint32_t next_shift = id_bits + tier_bits;
    const uint32_t state_shift = next_shift + next_use_bits;
    return (dest & id_mask) |
        ((static_cast<uint32_t>(tier) & tier_mask) << id_bits) |
        ((static_cast<uint32_t>(next_use) & next_mask) << next_shift) |
        ((static_cast<uint32_t>(state) &
          ((uint32_t{1} << kNextUseStateBits) - 1u)) << state_shift);
}

inline uint32_t extractNextUseRecord32Dest(
        uint32_t record, uint32_t id_bits) {
    return extractReusePlan32Dest(record, id_bits);
}

inline uint8_t extractNextUseRecord32Tier(
        uint32_t record, uint32_t id_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return extractReusePlan32Tier(record, id_bits, tier_bits);
}

inline uint16_t extractNextUseRecord32Position(
        uint32_t record, uint32_t id_bits, uint32_t next_use_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return static_cast<uint16_t>(
        (record >> (id_bits + tier_bits)) &
        ((uint32_t{1} << next_use_bits) - 1u));
}

inline NextUseState extractNextUseRecord32State(
        uint32_t record, uint32_t id_bits, uint32_t next_use_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return static_cast<NextUseState>(
        (record >> (id_bits + tier_bits + next_use_bits)) &
        ((uint32_t{1} << kNextUseStateBits) - 1u));
}

inline uint16_t quantizeNextUsePosition(
        uint32_t position, uint32_t num_vertices,
        uint32_t next_use_bits) {
    if (num_vertices <= 1 || next_use_bits == 0)
        return 0;
    const uint32_t levels =
        (uint32_t{1} << next_use_bits) - 1u;
    return static_cast<uint16_t>(
        (static_cast<uint64_t>(position) * levels) /
        (num_vertices - 1));
}

// Tiered two-epoch ReusePlan wire format shared by cache_sim/gem5/Sniper:
// dest[0:32] | tier[32:34] | first[34:49] | second[49:64].
// Tier 1/2/3 means hot/moderate/cold. Tier 0 is reserved for invalid metadata.
inline uint64_t packReusePlanRecord(uint32_t dest, uint8_t tier,
                                    uint16_t first, uint16_t second) {
    return static_cast<uint64_t>(dest) |
           (static_cast<uint64_t>(tier & 0x3u) << 32) |
           (static_cast<uint64_t>(first & kReusePlanEpochMask) << 34) |
           (static_cast<uint64_t>(second & kReusePlanEpochMask) << 49);
}

// COMPACT two-epoch ReusePlan wire format: the same two-stamp record in 32 bits.
//
// The 64-bit form above reserves a full 32-bit destination and 15-bit epochs,
// so it always costs 8 bytes per edge and doubles the structural stream against
// a 4-byte CSR edge. When the graph and epoch count are small enough, the same
// information fits in one 32-bit word and the record SUBSTITUTES for the edge
// instead of adding to it:
//
//     dest[id_bits] | tier[tier_bits] | first[epoch_bits] | second[epoch_bits]
//
// For a 65,536-vertex graph with 32 epochs that is 16 + 2 + 5 + 5 = 28 bits.
// This is what lets gem5 and Sniper stream the width cache_sim already models;
// without it the shared rule reports a budget width the backends cannot deliver.
//
// tier_bits is CONFIGURABLE, because those two bits are exactly what decides
// whether a larger graph still fits: an n18 graph with 128 epochs needs
// 18 + 0 + 7 + 7 = 32 bits and fits only when the tier is dropped, while
// 18 + 2 + 7 + 7 = 34 bits does not fit at all. Only 0 and 2 are defined --
// 2 is the canonical width shared with the 64-bit record, and 0 is tierless.
// Any other width fails closed rather than packing a field no decoder
// implements.
inline bool reusePlan32TierBitsSupported(uint32_t tier_bits) {
    return tier_bits == kReusePlanTierlessTierBits ||
           tier_bits == kReusePlanDefaultTierBits;
}

// Branch-free and correct at tier_bits == 0, where the mask is empty, the
// carried tier is always 0, and the first epoch starts immediately above the
// destination bits.
inline uint32_t reusePlan32TierMask(uint32_t tier_bits) {
    return (1u << tier_bits) - 1u;
}

inline bool canPackReusePlan32(uint32_t num_vertices, uint32_t ne,
                               uint32_t tier_bits = kReusePlanDefaultTierBits) {
    if (!reusePlan32TierBitsSupported(tier_bits)) return false;
    uint32_t id_bits = 1;
    while (id_bits < 32 && (uint64_t(1) << id_bits) < num_vertices) ++id_bits;
    uint32_t epoch_bits = 1;
    while (epoch_bits < 16 && (uint32_t(1) << epoch_bits) < ne) ++epoch_bits;
    return id_bits + tier_bits + 2u * epoch_bits <= 32u;
}

inline uint32_t reusePlan32IdBits(uint32_t num_vertices) {
    uint32_t id_bits = 1;
    while (id_bits < 32 && (uint64_t(1) << id_bits) < num_vertices) ++id_bits;
    return id_bits;
}

inline uint32_t reusePlan32EpochBits(uint32_t ne) {
    uint32_t epoch_bits = 1;
    while (epoch_bits < 16 && (uint32_t(1) << epoch_bits) < ne) ++epoch_bits;
    return epoch_bits;
}

inline uint32_t packReusePlanRecord32(
        uint32_t dest, uint8_t tier, uint16_t first, uint16_t second,
        uint32_t id_bits, uint32_t epoch_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    const uint32_t id_mask = (id_bits >= 32) ? 0xFFFFFFFFu
                                             : ((1u << id_bits) - 1u);
    const uint32_t ep_mask = (1u << epoch_bits) - 1u;
    const uint32_t tier_mask = reusePlan32TierMask(tier_bits);
    const uint32_t first_shift = id_bits + tier_bits;
    return (dest & id_mask) |
           ((static_cast<uint32_t>(tier) & tier_mask) << id_bits) |
           ((static_cast<uint32_t>(first) & ep_mask) << first_shift) |
           ((static_cast<uint32_t>(second) & ep_mask)
                << (first_shift + epoch_bits));
}

inline uint32_t extractReusePlan32Dest(uint32_t record, uint32_t id_bits) {
    const uint32_t id_mask = (id_bits >= 32) ? 0xFFFFFFFFu
                                             : ((1u << id_bits) - 1u);
    return record & id_mask;
}

inline uint8_t extractReusePlan32Tier(
        uint32_t record, uint32_t id_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return static_cast<uint8_t>(
        (record >> id_bits) & reusePlan32TierMask(tier_bits));
}

inline uint16_t extractReusePlan32First(
        uint32_t record, uint32_t id_bits, uint32_t epoch_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return static_cast<uint16_t>(
        (record >> (id_bits + tier_bits)) & ((1u << epoch_bits) - 1u));
}

inline uint16_t extractReusePlan32Second(
        uint32_t record, uint32_t id_bits, uint32_t epoch_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return static_cast<uint16_t>(
        (record >> (id_bits + tier_bits + epoch_bits)) &
        ((1u << epoch_bits) - 1u));
}

// Widen a compact 32-bit record to the 64-bit wire format the ReusePlan ISA helpers
// consume. This is a register operation: the 4-byte load already happened, so
// the memory traffic is 4 bytes per edge while the delivered value keeps the
// canonical layout every backend already understands. A tierless record widens
// with tier=0; the canonical 64-bit layout itself never changes.
inline uint64_t widenReusePlan32(
        uint32_t record, uint32_t id_bits, uint32_t epoch_bits,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    return packReusePlanRecord(
        extractReusePlan32Dest(record, id_bits),
        extractReusePlan32Tier(record, id_bits, tier_bits),
        extractReusePlan32First(record, id_bits, epoch_bits, tier_bits),
        extractReusePlan32Second(record, id_bits, epoch_bits, tier_bits));
}

inline uint32_t extractReusePlanDest(uint64_t record) {
    return static_cast<uint32_t>(record);
}

inline uint8_t extractReusePlanTier(uint64_t record) {
    return static_cast<uint8_t>((record >> 32) & 0x3u);
}

inline uint16_t extractReusePlanFirst(uint64_t record) {
    return static_cast<uint16_t>((record >> 34) & kReusePlanEpochMask);
}

inline uint16_t extractReusePlanSecond(uint64_t record) {
    return static_cast<uint16_t>((record >> 49) & kReusePlanEpochMask);
}

inline uint32_t packWeightedReusePlanSidecar(
        uint8_t tier, uint16_t first, uint16_t second) {
    return static_cast<uint32_t>(tier & 0x3u) |
           (static_cast<uint32_t>(first & kReusePlanEpochMask) << 2) |
           (static_cast<uint32_t>(second & kReusePlanEpochMask) << 17);
}

inline uint64_t combineWeightedReusePlanRecord(
        uint32_t dest, uint32_t sidecar) {
    return static_cast<uint64_t>(dest) |
           (static_cast<uint64_t>(sidecar) << 32);
}

inline uint8_t extractWeightedReusePlanTier(uint32_t sidecar) {
    return static_cast<uint8_t>(sidecar & 0x3u);
}

inline uint16_t extractWeightedReusePlanFirst(uint32_t sidecar) {
    return static_cast<uint16_t>((sidecar >> 2) & kReusePlanEpochMask);
}

inline uint16_t extractWeightedReusePlanSecond(uint32_t sidecar) {
    return static_cast<uint16_t>((sidecar >> 17) & kReusePlanEpochMask);
}

inline bool canPackCompactWeightedEdge(
        uint64_t num_vertices, uint32_t dest, int64_t weight) {
    return num_vertices <= kCompactWeightedMaxVertices &&
           dest <= kCompactWeightedDestMask &&
           weight >= 1 &&
           weight <= static_cast<int64_t>(kCompactWeightedMaxWeight);
}

inline uint64_t packCompactWeightedReusePlanRecord(
        uint32_t dest, uint32_t weight, uint8_t tier,
        uint16_t first, uint16_t second) {
    return static_cast<uint64_t>(dest & kCompactWeightedDestMask) |
           (static_cast<uint64_t>(weight & 0xFFu) << 24) |
           (static_cast<uint64_t>(tier & 0x3u) << 32) |
           (static_cast<uint64_t>(first & kReusePlanEpochMask) << 34) |
           (static_cast<uint64_t>(second & kReusePlanEpochMask) << 49);
}

inline uint32_t extractCompactWeightedDest(uint64_t record) {
    return static_cast<uint32_t>(record) & kCompactWeightedDestMask;
}

inline int32_t extractCompactWeightedWeight(uint64_t record) {
    return static_cast<int32_t>(
        static_cast<uint8_t>((record >> 24) & 0xFFu));
}

inline uint8_t extractCompactWeightedTier(uint64_t record) {
    return static_cast<uint8_t>((record >> 32) & 0x3u);
}

inline uint16_t extractCompactWeightedFirst(uint64_t record) {
    return static_cast<uint16_t>((record >> 34) & kReusePlanEpochMask);
}

inline uint16_t extractCompactWeightedSecond(uint64_t record) {
    return static_cast<uint16_t>((record >> 49) & kReusePlanEpochMask);
}

inline uint32_t normalizeReusePlanEpochCount(uint32_t count) {
    if (count < 2) return 2;
    return std::min(count, kReusePlanMaxEpochCount);
}

// Demand epoch for vertex u under a deterministic ID-order pull sweep (PR):
// position in [0,ne) proportional to u/num_nodes. Shared by all backends.
inline uint32_t currentEpoch(int64_t u, int64_t num_nodes, uint32_t ne) {
    return num_nodes > 0
        ? static_cast<uint32_t>(
              (static_cast<uint64_t>(u) * ne) /
              static_cast<uint64_t>(num_nodes))
        : 0u;
}

// Preserve the distinction between a forward reference in the current coarse
// epoch and a wrapped next-cycle reference that quantizes to that same epoch.
inline uint16_t quantizedFutureEpoch(
        uint32_t reader, uint32_t current, uint32_t num_nodes,
        uint32_t ne, bool wrapped) {
    if (num_nodes == 0 || ne < 2) return 0;
    uint32_t epoch = static_cast<uint32_t>(
        (static_cast<uint64_t>(reader) * ne) / num_nodes);
    if (epoch >= ne) epoch = ne - 1;
    const uint32_t current_epoch = currentEpoch(current, num_nodes, ne);
    if (wrapped && epoch == current_epoch)
        epoch = current_epoch == 0 ? ne - 1 : current_epoch - 1;
    return static_cast<uint16_t>(epoch);
}

// Path A filtered epoch gate for the lookahead-prefetch decision
// across cache_sim / gem5 / Sniper. Returns true to prefetch the candidate.
inline bool prefetchKeep(uint16_t cand_ep, uint32_t cur_ep, uint32_t ne,
                         int filter, uint32_t thresh) {
    if (filter == 0 || ne <= 1) return true;
    uint32_t dist = (static_cast<uint32_t>(cand_ep) + ne - cur_ep) % ne;
    if (filter == 1 && dist < thresh) return false;
    if (filter == 2 && dist > thresh) return false;
    return true;
}

template <typename GraphT>
void buildReaderCsr(const GraphT& g, bool push_out_edges,
                    std::vector<uint64_t>& off,
                    std::vector<uint32_t>& readers) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    off.assign(static_cast<size_t>(n) + 1, 0);
    readers.clear();
    readers.reserve(static_cast<size_t>(g.num_edges_directed()));
    for (uint32_t v = 0; v < n; ++v) {
        off[v] = readers.size();
        if (push_out_edges) {
            for (auto w_raw : g.in_neigh(v)) {
                const uint32_t w = static_cast<uint32_t>(w_raw);
                if (w < n) readers.push_back(w);
            }
        } else {
            for (auto w_raw : g.out_neigh(v)) {
                const uint32_t w = static_cast<uint32_t>(w_raw);
                if (w < n) readers.push_back(w);
            }
        }
        std::sort(
            readers.begin() + static_cast<std::ptrdiff_t>(off[v]),
            readers.end());
    }
    off[n] = readers.size();
}

// Rank vertices by the number of accesses to their property value in the
// selected kernel direction. Unlike address-region GRASP, this remains valid
// when the graph is not degree reordered.
inline std::vector<uint8_t> buildReuseTiers(
        const std::vector<uint64_t>& off, uint32_t n,
        double hot_fraction = 0.15) {
    std::vector<uint8_t> tiers(n, 3);
    if (n == 0 || off.size() < static_cast<size_t>(n) + 1) return tiers;
    hot_fraction = std::max(0.0, std::min(0.5, hot_fraction));

    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
        [&](uint32_t left, uint32_t right) {
            const uint64_t left_count = off[left + 1] - off[left];
            const uint64_t right_count = off[right + 1] - off[right];
            if (left_count != right_count) return left_count > right_count;
            return left < right;
        });

    size_t hot_count = static_cast<size_t>(hot_fraction * n);
    if (hot_fraction > 0.0 && hot_count == 0) hot_count = 1;
    hot_count = std::min(hot_count, static_cast<size_t>(n));
    const size_t moderate_end =
        std::min(static_cast<size_t>(n), hot_count * 2);
    for (size_t rank = 0; rank < order.size(); ++rank) {
        tiers[order[rank]] = rank < hot_count ? 1
            : rank < moderate_end ? 2 : 3;
    }
    return tiers;
}

inline double configuredReuseHotFraction() {
    const char* value = std::getenv("GRASP_HOT_FRACTION");
    if (!value) return 0.15;
    const double parsed = std::atof(value);
    return parsed > 0.0 && parsed <= 0.5 ? parsed : 0.15;
}

template <typename GraphT>
void accessedVertices(const GraphT& g, uint32_t src, bool push_out_edges,
                      std::vector<uint32_t>& accessed) {
    accessed.clear();
    if (push_out_edges) {
        for (auto dest_raw : g.out_neigh(src))
            accessed.push_back(static_cast<uint32_t>(dest_raw));
    } else {
        for (auto dest_raw : g.in_neigh(src))
            accessed.push_back(static_cast<uint32_t>(dest_raw));
    }
}

inline ReusePlan nextReusePlanForLine(
        const std::vector<uint64_t>& off,
        const std::vector<uint32_t>& readers,
        const std::vector<uint8_t>& reuse_tiers,
        uint32_t n, uint32_t src, uint32_t dest,
        uint32_t numVtxPerLine, uint32_t ne, bool linemin) {
    ReusePlan pair;
    if (dest >= n) return pair;
    const uint32_t v0 = linemin
        ? (dest / numVtxPerLine) * numVtxPerLine : dest;
    const uint32_t v1 = linemin
        ? std::min<uint32_t>(v0 + numVtxPerLine, n)
        : std::min<uint32_t>(dest + 1, n);
    uint8_t hottest_tier = 3;
    for (uint32_t w = v0; w < v1; ++w) {
        if (w < reuse_tiers.size())
            hottest_tier = std::min(hottest_tier, reuse_tiers[w]);
    }

    uint64_t best_distance[2] = {
        std::numeric_limits<uint64_t>::max(),
        std::numeric_limits<uint64_t>::max(),
    };
    uint16_t best_epoch[2] = {0, 0};
    auto consider = [&](uint64_t distance, uint16_t epoch) {
        if (distance < best_distance[0]) {
            best_distance[1] = best_distance[0];
            best_epoch[1] = best_epoch[0];
            best_distance[0] = distance;
            best_epoch[0] = epoch;
        } else if (distance < best_distance[1]) {
            best_distance[1] = distance;
            best_epoch[1] = epoch;
        }
    };

    for (uint32_t w = v0; w < v1; ++w) {
        const uint64_t a = off[w], b = off[w + 1];
        if (a >= b) continue;
        auto begin = readers.begin() + static_cast<std::ptrdiff_t>(a);
        auto end = readers.begin() + static_cast<std::ptrdiff_t>(b);
        auto it = std::upper_bound(begin, end, src);
        uint32_t completed_cycles = 0;
        for (int k = 0; k < 2; ++k) {
            if (it == end) {
                it = begin;
                ++completed_cycles;
            }
            const uint32_t selected = *it;
            const uint32_t epoch = quantizedFutureEpoch(
                selected, src, n, ne, completed_cycles > 0);
            const uint64_t absolute =
                static_cast<uint64_t>(selected) +
                static_cast<uint64_t>(completed_cycles) * n;
            consider(
                absolute - src, static_cast<uint16_t>(epoch));
            ++it;
        }
    }

    if (best_distance[0] == std::numeric_limits<uint64_t>::max())
        return pair;
    pair.first = best_epoch[0];
    pair.second =
        best_distance[1] == std::numeric_limits<uint64_t>::max()
            ? best_epoch[0] : best_epoch[1];
    pair.tier = hottest_tier;
    pair.valid = true;
    return pair;
}

inline ReusePlan nextReusePlanForAccess(
        const std::vector<uint64_t>& off,
        const std::vector<uint32_t>& readers,
        const std::vector<uint8_t>& reuse_tiers,
        uint32_t n, uint32_t src,
        const std::vector<uint32_t>& accessed, size_t edge_pos,
        uint32_t numVtxPerLine, uint32_t ne, bool linemin) {
    const uint32_t dest = accessed[edge_pos];
    ReusePlan pair = nextReusePlanForLine(
        off, readers, reuse_tiers, n, src, dest,
        numVtxPerLine, ne, linemin);
    if (!linemin || numVtxPerLine == 0 || dest >= n)
        return pair;

    // Reader CSR is keyed by the outer vertex and cannot order two accesses
    // within one adjacency list. Preserve those same-reader line hits here;
    // otherwise upper_bound(src) skips them and falsely predicts a wrapped use.
    const uint32_t line = dest / numVtxPerLine;
    uint32_t same_reader_uses = 0;
    for (size_t next = edge_pos + 1;
         next < accessed.size() && same_reader_uses < 2; ++next) {
        const uint32_t next_dest = accessed[next];
        if (next_dest < n && next_dest / numVtxPerLine == line)
            ++same_reader_uses;
    }
    if (same_reader_uses == 0) return pair;

    const uint16_t current_epoch = static_cast<uint16_t>(
        currentEpoch(src, n, ne));
    if (same_reader_uses >= 2) {
        pair.first = current_epoch;
        pair.second = current_epoch;
    } else {
        pair.second = pair.first;
        pair.first = current_epoch;
    }
    pair.valid = true;
    return pair;
}

inline NextUsePlan nextUsePlanForAccess(
        const std::vector<uint64_t>& off,
        const std::vector<uint32_t>& readers,
        const std::vector<uint8_t>& reuse_tiers,
        uint32_t n, uint32_t src,
        const std::vector<uint32_t>& accessed, size_t edge_pos,
        uint32_t numVtxPerLine, uint32_t next_use_bits,
        bool linemin) {
    NextUsePlan plan;
    if (edge_pos >= accessed.size() || n == 0 ||
        next_use_bits == 0 || next_use_bits > 15) {
        return plan;
    }
    const uint32_t dest = accessed[edge_pos];
    if (dest >= n) return plan;
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    const uint32_t v0 = linemin
        ? (dest / numVtxPerLine) * numVtxPerLine : dest;
    const uint32_t v1 = linemin
        ? std::min<uint32_t>(v0 + numVtxPerLine, n)
        : std::min<uint32_t>(dest + 1, n);
    uint8_t hottest_tier = 3;
    for (uint32_t vertex = v0; vertex < v1; ++vertex) {
        if (vertex < reuse_tiers.size())
            hottest_tier = std::min(
                hottest_tier, reuse_tiers[vertex]);
    }
    plan.tier = hottest_tier;

    if (linemin) {
        const uint32_t line = dest / numVtxPerLine;
        for (size_t next = edge_pos + 1;
             next < accessed.size(); ++next) {
            const uint32_t next_dest = accessed[next];
            if (next_dest < n &&
                next_dest / numVtxPerLine == line) {
                plan.next_use = quantizeNextUsePosition(
                    src, n, next_use_bits);
                plan.state = NextUseState::FINITE;
                return plan;
            }
        }
    }

    uint32_t next_reader = UINT32_MAX;
    uint32_t first_reader = UINT32_MAX;
    for (uint32_t vertex = v0; vertex < v1; ++vertex) {
        const uint64_t begin_offset = off[vertex];
        const uint64_t end_offset = off[vertex + 1];
        if (begin_offset >= end_offset) continue;
        const auto begin = readers.begin() +
            static_cast<std::ptrdiff_t>(begin_offset);
        const auto end = readers.begin() +
            static_cast<std::ptrdiff_t>(end_offset);
        first_reader = std::min(first_reader, *begin);
        const auto found = std::upper_bound(begin, end, src);
        if (found != end)
            next_reader = std::min(next_reader, *found);
    }
    if (next_reader == UINT32_MAX) {
        if (first_reader == UINT32_MAX) {
            plan.state = NextUseState::DEAD;
        } else {
            plan.next_use = quantizeNextUsePosition(
                first_reader, n, next_use_bits);
            plan.state = NextUseState::WRAP;
        }
        return plan;
    }
    plan.next_use = quantizeNextUsePosition(
        next_reader, n, next_use_bits);
    plan.state = NextUseState::FINITE;
    return plan;
}

template <typename GraphT>
bool buildInEdgeNextUseRecords32(
        const GraphT& g, uint32_t numVtxPerLine,
        uint32_t next_use_bits, bool linemin,
        std::vector<std::vector<uint32_t>>& records,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    records.clear();
    records.resize(n);
    if (n == 0 || !canPackNextUseRecord32(
            n, next_use_bits, tier_bits)) {
        return false;
    }
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    const uint32_t id_bits = reusePlan32IdBits(n);

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, false, off, readers);
    std::vector<uint8_t> reuse_tiers;
    if (tier_bits > 0) {
        reuse_tiers =
            buildReuseTiers(off, n, configuredReuseHotFraction());
    } else {
        reuse_tiers.assign(n, 0);
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, false, accessed);
        auto& row = records[src];
        row.resize(accessed.size());
        for (size_t edge = 0; edge < accessed.size(); ++edge) {
            const NextUsePlan plan = nextUsePlanForAccess(
                off, readers, reuse_tiers, n, src,
                accessed, edge, numVtxPerLine,
                next_use_bits, linemin);
            row[edge] = packNextUseRecord32(
                accessed[edge], plan.tier, plan.next_use,
                plan.state, id_bits, next_use_bits, tier_bits);
        }
    }
    return true;
}

template <typename GraphT>
bool buildInEdgeNextUseRecords32Flat(
        const GraphT& g, uint32_t numVtxPerLine,
        uint32_t next_use_bits, bool linemin,
        std::vector<uint64_t>& record_off,
        std::vector<uint32_t>& records,
        uint32_t tier_bits = kReusePlanTierlessTierBits) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    record_off.assign(static_cast<size_t>(n) + 1, 0);
    records.clear();
    if (n == 0 || !canPackNextUseRecord32(
            n, next_use_bits, tier_bits)) {
        return false;
    }
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    const uint32_t id_bits = reusePlan32IdBits(n);

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, false, off, readers);
    std::vector<uint8_t> reuse_tiers;
    if (tier_bits > 0) {
        reuse_tiers =
            buildReuseTiers(off, n, configuredReuseHotFraction());
    } else {
        reuse_tiers.assign(n, 0);
    }
    for (uint32_t src = 0; src < n; ++src) {
        record_off[src + 1] =
            record_off[src] + static_cast<uint64_t>(g.in_degree(src));
    }
    records.assign(record_off[n], 0);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, false, accessed);
        for (size_t edge = 0; edge < accessed.size(); ++edge) {
            const NextUsePlan plan = nextUsePlanForAccess(
                off, readers, reuse_tiers, n, src,
                accessed, edge, numVtxPerLine,
                next_use_bits, linemin);
            records[record_off[src] + edge] = packNextUseRecord32(
                accessed[edge], plan.tier, plan.next_use,
                plan.state, id_bits, next_use_bits, tier_bits);
        }
    }
    return true;
}

// Build one per-edge next-reference epoch. PR uses pull/in edges by default;
// BFS/SSSP use push_out_edges=true.
template <typename GraphT>
void buildInEdgeEpochs(const GraphT& g,
                       uint32_t numVtxPerLine,
                       uint32_t ne,
                       bool linemin,
                       std::vector<std::vector<uint16_t>>& out,
                       bool push_out_edges = false) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    out.clear();
    out.resize(n);
    if (n == 0) return;
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    if (ne < 2) ne = 2;
    if (ne > 65535) ne = 65535;

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, push_out_edges, off, readers);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        auto& epochs = out[src];
        epochs.resize(accessed.size(), static_cast<uint16_t>(ne - 1));

        for (size_t edge_pos = 0; edge_pos < accessed.size(); ++edge_pos) {
            const uint32_t dest = accessed[edge_pos];
            if (dest >= n) continue;
            const uint32_t v0 = linemin
                ? (dest / numVtxPerLine) * numVtxPerLine : dest;
            const uint32_t v1 = linemin
                ? std::min<uint32_t>(v0 + numVtxPerLine, n)
                : std::min<uint32_t>(dest + 1, n);
            if (linemin) {
                const uint32_t line = dest / numVtxPerLine;
                bool same_reader_use = false;
                for (size_t next = edge_pos + 1;
                     next < accessed.size(); ++next) {
                    const uint32_t next_dest = accessed[next];
                    if (next_dest < n &&
                        next_dest / numVtxPerLine == line) {
                        same_reader_use = true;
                        break;
                    }
                }
                if (same_reader_use) {
                    epochs[edge_pos] = static_cast<uint16_t>(
                        currentEpoch(src, n, ne));
                    continue;
                }
            }

            uint32_t best_dist = std::numeric_limits<uint32_t>::max();
            uint32_t best_epoch = ne - 1;
            for (uint32_t w = v0; w < v1; ++w) {
                const uint64_t a = off[w], b = off[w + 1];
                if (a >= b) continue;
                auto begin = readers.begin() + static_cast<std::ptrdiff_t>(a);
                auto end = readers.begin() + static_cast<std::ptrdiff_t>(b);
                auto it = std::upper_bound(begin, end, src);
                uint32_t reader;
                uint32_t dist;
                const bool wrapped = it == end;
                if (!wrapped) {
                    reader = *it;
                    dist = reader - src;
                } else {
                    reader = *begin;
                    dist = reader + n - src;
                }
                if (dist < best_dist) {
                    best_dist = dist;
                    best_epoch = quantizedFutureEpoch(
                        reader, src, n, ne, wrapped);
                }
            }
            epochs[edge_pos] = static_cast<uint16_t>(best_epoch);
        }
    }
}

// Build the next TWO per-line reference epochs for every accessed edge.
// first is equivalent to buildInEdgeEpochs; second is the next candidate after
// it in circular traversal order. If only one candidate exists, repeat first.
template <typename GraphT>
void buildInEdgeReusePlans(const GraphT& g,
                           uint32_t numVtxPerLine,
                           uint32_t ne,
                           bool linemin,
                           std::vector<std::vector<ReusePlan>>& out,
                           bool push_out_edges = false) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    out.clear();
    out.resize(n);
    if (n == 0) return;
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    ne = normalizeReusePlanEpochCount(ne);

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, push_out_edges, off, readers);
    const std::vector<uint8_t> reuse_tiers =
        buildReuseTiers(off, n, configuredReuseHotFraction());

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        auto& pairs = out[src];
        pairs.resize(accessed.size());

        for (size_t edge_pos = 0; edge_pos < accessed.size(); ++edge_pos) {
            pairs[edge_pos] = nextReusePlanForAccess(
                off, readers, reuse_tiers, n, src, accessed, edge_pos,
                numVtxPerLine, ne, linemin);
        }
    }
}

// Build the packed two-epoch ReusePlan stream directly, avoiding a second O(E) nested
// ReusePlan representation in gem5/Sniper.
template <typename GraphT>
void buildInEdgeReusePlanRecords(
        const GraphT& g, uint32_t numVtxPerLine, uint32_t ne, bool linemin,
        std::vector<uint64_t>& record_off,
        std::vector<uint64_t>& records,
        bool push_out_edges = false) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    record_off.assign(static_cast<size_t>(n) + 1, 0);
    records.clear();
    if (n == 0) return;
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    ne = normalizeReusePlanEpochCount(ne);

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, push_out_edges, off, readers);
    const std::vector<uint8_t> reuse_tiers =
        buildReuseTiers(off, n, configuredReuseHotFraction());
    for (uint32_t src = 0; src < n; ++src) {
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        record_off[src + 1] = record_off[src] + accessed.size();
    }
    records.assign(record_off[n], 0);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        for (size_t edge = 0; edge < accessed.size(); ++edge) {
            const ReusePlan pair = nextReusePlanForAccess(
                off, readers, reuse_tiers, n, src, accessed, edge,
                numVtxPerLine, ne, linemin);
            records[record_off[src] + edge] = packReusePlanRecord(
                accessed[edge], pair.tier, pair.first, pair.second);
        }
    }
}

// Compact 32-bit twin of buildInEdgeReusePlanRecords. Identical epoch and tier
// computation -- it calls the same nextReusePlanForLine -- so the only
// difference is the container width and the configured tier width. Returns
// false when the fields do not fit or the tier width is undefined, leaving the
// caller to use the 64-bit form.
template <typename GraphT>
bool buildInEdgeReusePlanRecords32(
        const GraphT& g, uint32_t numVtxPerLine, uint32_t ne, bool linemin,
        std::vector<uint64_t>& record_off,
        std::vector<uint32_t>& records,
        bool push_out_edges = false,
        uint32_t tier_bits = kReusePlanDefaultTierBits) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    record_off.assign(static_cast<size_t>(n) + 1, 0);
    records.clear();
    if (n == 0) return false;
    if (numVtxPerLine == 0) numVtxPerLine = 16;
    ne = normalizeReusePlanEpochCount(ne);
    if (!canPackReusePlan32(n, ne, tier_bits)) return false;
    const uint32_t id_bits = reusePlan32IdBits(n);
    const uint32_t epoch_bits = reusePlan32EpochBits(ne);

    std::vector<uint64_t> off;
    std::vector<uint32_t> readers;
    buildReaderCsr(g, push_out_edges, off, readers);
    const std::vector<uint8_t> reuse_tiers =
        buildReuseTiers(off, n, configuredReuseHotFraction());
    for (uint32_t src = 0; src < n; ++src) {
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        record_off[src + 1] = record_off[src] + accessed.size();
    }
    records.assign(record_off[n], 0u);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 128)
#endif
    for (int64_t src_i = 0; src_i < static_cast<int64_t>(n); ++src_i) {
        const uint32_t src = static_cast<uint32_t>(src_i);
        std::vector<uint32_t> accessed;
        accessedVertices(g, src, push_out_edges, accessed);
        for (size_t edge = 0; edge < accessed.size(); ++edge) {
            const ReusePlan pair = nextReusePlanForAccess(
                off, readers, reuse_tiers, n, src, accessed, edge,
                numVtxPerLine, ne, linemin);
            records[record_off[src] + edge] = packReusePlanRecord32(
                accessed[edge], pair.tier, pair.first, pair.second,
                id_bits, epoch_bits, tier_bits);
        }
    }
    return true;
}

template <typename GraphT, typename OffsetContainer>
bool reusePlanOffsetsMatchCsr(
        const GraphT& g, const OffsetContainer& record_off,
        uint64_t record_count, bool push_out_edges) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    if (record_off.size() != static_cast<size_t>(n) + 1 ||
        record_off.empty() || record_off.front() != 0 ||
        record_off.back() != record_count) {
        return false;
    }
    if (n == 0) return record_count == 0;
    for (uint32_t u = 0; u <= n; ++u) {
        if (u > 0 && record_off[u] < record_off[u - 1])
            return false;
        const int64_t csr_offset = push_out_edges
            ? g.out_offset(u)
            : g.in_offset(u);
        if (csr_offset < 0 ||
            record_off[u] != static_cast<uint64_t>(csr_offset)) {
            return false;
        }
    }
    return true;
}

template <typename GraphT, typename OffsetContainer>
bool reusePlanOffsetsMatchInCsr(
        const GraphT& g, const OffsetContainer& record_off,
        uint64_t record_count) {
    return reusePlanOffsetsMatchCsr(
        g, record_off, record_count, /*push_out_edges=*/false);
}

template <typename GraphT, typename OffsetContainer, typename RecordContainer>
bool validateWeightedReusePlanRecords(
        const GraphT& g, const OffsetContainer& record_off,
        const RecordContainer& records) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    if (record_off.size() != static_cast<size_t>(n) + 1 ||
        record_off.empty() || record_off.front() != 0 ||
        record_off.back() != records.size()) {
        return false;
    }
    for (uint32_t src = 0; src < n; ++src) {
        uint64_t pos = record_off[src];
        const uint64_t end = record_off[src + 1];
        for (const auto edge : g.out_neigh(src)) {
            if (pos >= end || pos >= records.size() ||
                extractReusePlanDest(records[pos]) !=
                    static_cast<uint32_t>(edge.v)) {
                return false;
            }
            ++pos;
        }
        if (pos != end) return false;
    }
    return true;
}

template <typename OffsetContainer, typename RecordContainer,
          typename SidecarContainer>
bool validateWeightedReusePlanSidecars(
        const OffsetContainer& record_off, const RecordContainer& records,
        const SidecarContainer& sidecars) {
    if (records.size() != sidecars.size() || record_off.empty() ||
        record_off.back() != records.size()) {
        return false;
    }
    for (size_t i = 0; i < records.size(); ++i) {
        if (sidecars[i] != packWeightedReusePlanSidecar(
                extractReusePlanTier(records[i]),
                extractReusePlanFirst(records[i]),
                extractReusePlanSecond(records[i]))) {
            return false;
        }
    }
    return true;
}

template <typename GraphT, typename OffsetContainer,
          typename PairContainer, typename CompactContainer>
bool validateCompactWeightedReusePlanRecords(
        const GraphT& g, const OffsetContainer& record_off,
        const PairContainer& pairs, const CompactContainer& compact) {
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());
    if (n > kCompactWeightedMaxVertices ||
        pairs.size() != compact.size() ||
        record_off.size() != static_cast<size_t>(n) + 1 ||
        record_off.empty() || record_off.front() != 0 ||
        record_off.back() != compact.size()) {
        return false;
    }
    for (uint32_t src = 0; src < n; ++src) {
        uint64_t pos = record_off[src];
        const uint64_t end = record_off[src + 1];
        for (const auto edge : g.out_neigh(src)) {
            if (pos >= end || pos >= compact.size() ||
                !canPackCompactWeightedEdge(
                    n, static_cast<uint32_t>(edge.v),
                    static_cast<int64_t>(edge.w)) ||
                extractCompactWeightedDest(compact[pos]) !=
                    static_cast<uint32_t>(edge.v) ||
                extractCompactWeightedWeight(compact[pos]) !=
                    static_cast<int32_t>(edge.w) ||
                extractCompactWeightedTier(compact[pos]) !=
                    extractReusePlanTier(pairs[pos]) ||
                extractCompactWeightedFirst(compact[pos]) !=
                    extractReusePlanFirst(pairs[pos]) ||
                extractCompactWeightedSecond(compact[pos]) !=
                    extractReusePlanSecond(pairs[pos])) {
                return false;
            }
            ++pos;
        }
        if (pos != end) return false;
    }
    return true;
}

} // namespace ecg_reuse_plan

#endif // ECG_REUSE_PLAN_BUILDER_H
