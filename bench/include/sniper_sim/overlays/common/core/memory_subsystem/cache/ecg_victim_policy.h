// ECG policy (GRASP insertion tier + ECG_GRASP_POPT eviction) — single source of
// truth for all simulators.
//
// cache_sim, gem5 and Sniper all call ecg_policy::selectVictim with a per-way
// WayState built from their native cache-line structures. The DECISION logic is
// therefore identical across the three (nothing is "ported" or "mirrored"), so a
// single unit test of this function verifies the eviction choice for every
// backend; each simulator's thin adapter (native state -> WayState) is covered by
// its live eviction trace. See scripts/experiments/ecg/verify_ecg.py and
// bench/src_sim/test_ecg_victim.cc.
//
// The seven variants (selected by ECG_VARIANT) and the invariants are documented
// in wiki/K2-StreamShield.md. Summary:
//   - epoch is PROPERTY-ONLY; record (non-property) lines never carry a usable
//     epoch and are ranked by recency / set order.
//   - "recency" is normalised so SMALLER == older == evict-first. cache_sim and
//     gem5 pass last_access / lastTouchTick directly; Sniper, which has no
//     per-line timestamp, passes a monotone-decreasing function of its RRIP age
//     so the oldest-by-RRIP line is evicted first (consistent across variants).
//   - rrpv is aged in place (the SRRIP state update); the caller must write the
//     possibly-incremented rrpv back to its native lines.
#ifndef ECG_VICTIM_POLICY_H
#define ECG_VICTIM_POLICY_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace ecg_policy {

enum Variant {
    GRASP_ONLY   = 0,  // pure RRIP, no epoch (== GRASP)
    EPOCH_FIRST  = 1,  // records by recency, then farthest-epoch property (epoch vetoes rrpv)
    RRIP_FIRST   = 2,  // max-rrpv set (recency vetoes); records-first, then farthest-epoch property
    EPOCH_ONLY   = 3,  // same eviction as EPOCH_FIRST (differs only at insertion)
    SHORTCIRCUIT = 4,  // non-property first (set order), then farthest RAW-dist property
    DEGREE_FIRST = 5,  // max-rrpv set; records, then coldest degree tier, then farthest epoch
    LRU_ONLY     = 6,  // oldest line regardless of metadata
};

inline int parseVariant(const char* value) {
    if (!value || !value[0]) return RRIP_FIRST;
    const std::string variant(value);
    if (variant == "grasp_only") return GRASP_ONLY;
    if (variant == "epoch_first") return EPOCH_FIRST;
    if (variant == "rrip_first") return RRIP_FIRST;
    if (variant == "epoch_only") return EPOCH_ONLY;
    if (variant == "shortcircuit" || variant == "legacy")
        return SHORTCIRCUIT;
    if (variant == "degree_first" || variant == "traversal")
        return DEGREE_FIRST;
    if (variant == "lru_only") return LRU_ONLY;
    std::fprintf(stderr, "[FATAL] unknown ECG_VARIANT=%s\n", value);
    std::abort();
}

enum DuelingArm : uint8_t {
    DUEL_RRIP = 0,
    DUEL_GRASP = 1,
    DUEL_EPOCH = 2,
    DUEL_DEGREE = 3,
    DUEL_LRU = 4,
    DUEL_ARM_COUNT = 5,
};

inline int duelingArmVariant(uint8_t arm) {
    switch (arm) {
        case DUEL_GRASP: return GRASP_ONLY;
        case DUEL_EPOCH: return EPOCH_FIRST;
        case DUEL_DEGREE: return DEGREE_FIRST;
        case DUEL_LRU: return LRU_ONLY;
        default: return RRIP_FIRST;
    }
}

// Five of every 64 sets are leaders, one per arm. All other sets follow
// the current online winner.
inline int duelingLeaderArm(size_t set_index) {
    const uint64_t slot = static_cast<uint64_t>(set_index) & 63u;
    return slot < DUEL_ARM_COUNT ? static_cast<int>(slot) : -1;
}

// Result of a single recordMiss() call. Every field describes ONLY what
// THAT call itself did/observed -- never a diff against a separately-read
// "before" snapshot. That distinction matters under concurrent callers
// (Sniper simulates cores as real OS threads sharing one LLC/selector):
// separate before/after reads of sampledMisses()/completedWindows()/
// winnerArm() around a recordMiss() call race against every OTHER
// thread's concurrent recordMiss() call on the same selector, so a
// caller-side "after > before" diff can attribute another thread's window
// completion or winner change to this call (or miss/double-count one
// entirely). Every counter recordMiss() touches here (misses_,
// sampled_misses_, winner_) is already an std::atomic with a
// fetch_add/exchange that returns a value unique to the calling thread, so
// deriving leader_sample/completed_window/winner_changed from THOSE return
// values (as done below) is race-free: at most one call can ever observe
// "this fetch_add hit the window boundary" for a given window, and only
// that same call performs (and reports) the resulting winner transition.
struct MissRecordEvent {
    bool leader_sample = false;     // this call sampled a leader-set miss
    bool completed_window = false;  // this call's sample completed a window
    bool winner_changed = false;    // this call's window changed the winner
    uint8_t winner_before = 0;
    uint8_t winner_after = 0;
};

class OnlineDuelingSelector {
  public:
    int variantForSet(size_t set_index) const {
        const int leader = duelingLeaderArm(set_index);
        const uint8_t arm = leader >= 0
            ? static_cast<uint8_t>(leader)
            : winner_.load(std::memory_order_relaxed);
        return duelingArmVariant(arm);
    }

    // Returns a MissRecordEvent describing what THIS call did; see the
    // struct comment above for why that must never be reconstructed from a
    // caller-side before/after diff under concurrent callers. The dueling
    // decision logic itself (leader gating, per-window arm comparison,
    // ties keeping the incumbent winner) is UNCHANGED from the original
    // void-returning implementation.
    MissRecordEvent recordMiss(size_t set_index) {
        MissRecordEvent event;
        event.winner_before = winner_.load(std::memory_order_relaxed);
        event.winner_after = event.winner_before;
        const int leader = duelingLeaderArm(set_index);
        if (leader < 0) return event;
        event.leader_sample = true;
        misses_[static_cast<size_t>(leader)].fetch_add(
            1, std::memory_order_relaxed);
        const uint64_t total =
            sampled_misses_.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((total % kWindowMisses) != 0) return event;
        event.completed_window = true;

        std::array<uint64_t, DUEL_ARM_COUNT> window{};
        for (size_t arm = 0; arm < DUEL_ARM_COUNT; ++arm) {
            window[arm] = misses_[arm].exchange(
                0, std::memory_order_relaxed);
        }
        uint8_t best = winner_.load(std::memory_order_relaxed);
        uint64_t best_misses = window[best];
        for (uint8_t arm = 0; arm < DUEL_ARM_COUNT; ++arm) {
            if (window[arm] < best_misses) {
                best = arm;
                best_misses = window[arm];
            }
        }
        // winner_before/winner_changed for a completed window MUST be
        // derived from the return value of the atomic op that actually
        // installs `best`, not from the separate winner_.load() taken at
        // the top of this call (or the initial-guess load just above).
        // Overlapping recordMiss() calls from other threads can complete
        // their OWN windows and mutate winner_ at any point between those
        // earlier reads and this store; a caller-visible "before" that
        // isn't the exact predecessor this call's store overwrote can
        // duplicate or misattribute a winner_changed transition that
        // really belongs to a different call. exchange() is a single
        // atomic RMW: its return value is, by definition, the value this
        // specific call's store replaced in winner_'s real total
        // modification order, so at most one call can ever report a given
        // transition and the transitions chain together exactly.
        const uint8_t previous_winner =
            winner_.exchange(best, std::memory_order_relaxed);
        event.winner_before = previous_winner;
        event.winner_after = best;
        event.winner_changed = (best != previous_winner);
        return event;
    }

    uint8_t winnerArm() const {
        return winner_.load(std::memory_order_relaxed);
    }

    uint64_t sampledMisses() const {
        return sampled_misses_.load(std::memory_order_relaxed);
    }

    uint64_t completedWindows() const {
        return sampledMisses() / kWindowMisses;
    }

  private:
    static constexpr uint64_t kWindowMisses = 1024;
    std::array<std::atomic<uint64_t>, DUEL_ARM_COUNT> misses_{};
    std::atomic<uint64_t> sampled_misses_{0};
    std::atomic<uint8_t> winner_{DUEL_RRIP};
};

inline OnlineDuelingSelector& globalOnlineDuelingSelector() {
    static OnlineDuelingSelector selector;
    return selector;
}

enum PlacementArm : uint8_t {
    PLACE_ALLOCATE = 0,
    PLACE_SHIELD = 1,
    PLACE_ARM_COUNT = 2,
};

// Slots 0..4 are reserved for replacement-policy leaders. Placement dueling
// uses two disjoint leaders per 64 sets and leaves the other 62 as followers.
inline int placementLeaderArm(size_t set_index) {
    const uint64_t slot = static_cast<uint64_t>(set_index) & 63u;
    if (slot == 5) return PLACE_ALLOCATE;
    if (slot == 6) return PLACE_SHIELD;
    return -1;
}

class OnlinePlacementSelector {
  public:
    bool shouldBypass(size_t set_index) const {
        const int leader = placementLeaderArm(set_index);
        const uint8_t arm = leader >= 0
            ? static_cast<uint8_t>(leader)
            : winner_.load(std::memory_order_relaxed);
        return arm == PLACE_SHIELD;
    }

    void recordMiss(size_t set_index) {
        const int leader = placementLeaderArm(set_index);
        if (leader < 0) return;
        misses_[static_cast<size_t>(leader)].fetch_add(
            1, std::memory_order_relaxed);
        const uint64_t total =
            sampled_misses_.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((total % kWindowMisses) != 0) return;
        const uint64_t allocate_misses = misses_[PLACE_ALLOCATE].exchange(
            0, std::memory_order_relaxed);
        const uint64_t shield_misses = misses_[PLACE_SHIELD].exchange(
            0, std::memory_order_relaxed);
        winner_.store(
            shield_misses < allocate_misses
                ? PLACE_SHIELD : PLACE_ALLOCATE,
            std::memory_order_relaxed);
    }

    uint8_t winnerArm() const {
        return winner_.load(std::memory_order_relaxed);
    }

  private:
    static constexpr uint64_t kWindowMisses = 1024;
    std::array<std::atomic<uint64_t>, PLACE_ARM_COUNT> misses_{};
    std::atomic<uint64_t> sampled_misses_{0};
    std::atomic<uint8_t> winner_{PLACE_ALLOCATE};
};

inline OnlinePlacementSelector& globalOnlinePlacementSelector() {
    static OnlinePlacementSelector selector;
    return selector;
}

struct WayState {
    bool     prop;     // property (vertex) line, vs record (edge-stream) line
    uint8_t  rrpv;     // RRIP age (aged in place for variants that age)
    uint64_t recency;  // smaller == older == evict-first
    uint8_t  dbg;      // DBG degree tier (shortcircuit all-property tiebreak)
    uint32_t dist;     // raw circular next-ref distance (stored_epoch + ne - cur_epoch) % ne
    bool     stamped;  // epoch is meaningful here (property line with a live stamp)
};

// Effective epoch distance: rrip_first/epoch_* treat an unstamped line as
// distance 0 (kept), so only genuinely stamped property competes on epoch.
inline uint32_t effDist(const WayState& w) { return w.stamped ? w.dist : 0; }

// Circular distance from the current traversal epoch to one delivered next-ref
// epoch. Clamp malformed/out-of-range payloads to the last valid epoch.
inline uint32_t epochDistance(uint16_t epoch, uint32_t current, uint32_t ne) {
    if (ne < 2) return 0;
    uint32_t e = epoch;
    if (e >= ne) e = ne - 1;
    return (e + ne - (current % ne)) % ne;
}

// Schedule-2 effective distance: the line is needed at the nearer of its next
// two references. count<=1 preserves the legacy single-epoch behavior.
inline uint32_t epochPairDistance(uint16_t first, uint16_t second,
                                  uint8_t count, uint32_t current,
                                  uint32_t ne) {
    uint32_t distance = epochDistance(first, current, ne);
    if (count > 1) {
        uint32_t second_distance = epochDistance(second, current, ne);
        if (second_distance < distance) distance = second_distance;
    }
    return distance;
}

// Select the victim index among ways[0..n). Ages rrpv in place where the variant
// ages. n must be >= 1.
inline size_t selectVictim(WayState* ways, size_t n, int variant, uint8_t rrpvMax) {
    // grasp_only: pure RRIP — first line at max RRPV, aging until one reaches it.
    if (variant == GRASP_ONLY) {
        for (;;) {
            for (size_t i = 0; i < n; i++) if (ways[i].rrpv >= rrpvMax) return i;
            for (size_t i = 0; i < n; i++) if (ways[i].rrpv < rrpvMax) ways[i].rrpv++;
        }

    }

    if (variant == LRU_ONLY) {
        size_t victim = 0;
        uint64_t oldest = ways[0].recency;
        for (size_t i = 1; i < n; ++i) {
            if (ways[i].recency < oldest) {
                oldest = ways[i].recency;
                victim = i;
            }
        }
        return victim;
    }

    // shortcircuit (legacy): evict any non-property line first (set order); if the
    // set is all property, evict the farthest effective-dist line (unstamped property
    // -> dist 0 = kept, so only genuinely stamped property competes; DBG tiebreak).
    if (variant == SHORTCIRCUIT) {
        for (size_t i = 0; i < n; i++) if (!ways[i].prop) return i;
        size_t best = 0; uint32_t bd = 0; uint8_t bdbg = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t d = effDist(ways[i]);
            if (d > bd || (d == bd && ways[i].dbg > bdbg)) { best = i; bd = d; bdbg = ways[i].dbg; }
        }
        return best;
    }

    // epoch_first / epoch_only: records first by recency (no rrpv gate); else the
    // farthest-next-ref stamped property; else recency fallback (LRU).
    if (variant == EPOCH_FIRST || variant == EPOCH_ONLY) {
        size_t rec = n; uint64_t ro = 0;
        for (size_t i = 0; i < n; i++) if (!ways[i].prop)
            if (rec == n || ways[i].recency < ro) { rec = i; ro = ways[i].recency; }
        if (rec != n) return rec;
        size_t best = n; uint32_t bd = 0;
        for (size_t i = 0; i < n; i++) if (ways[i].stamped) {
            uint32_t d = ways[i].dist;
            if (best == n || d > bd) { best = i; bd = d; }
        }
        if (best != n) return best;
        size_t v = 0; uint64_t o = ways[0].recency;
        for (size_t i = 1; i < n; i++) if (ways[i].recency < o) { o = ways[i].recency; v = i; }
        return v;
    }

    // degree_first (frontier traversal): keep RRIP's eligibility gate, then
    // protect high-degree property lines independent of visit order. Within the
    // coldest degree tier, Schedule-2/epoch distance selects the farthest next
    // use; true recency resolves any remaining tie.
    if (variant == DEGREE_FIRST) {
        for (;;) {
            size_t recIdx = n; uint64_t recOldest = 0;
            size_t propIdx = n; uint8_t coldest = 0;
            uint32_t farthest = 0; uint64_t propOldest = 0;
            for (size_t i = 0; i < n; ++i) {
                if (ways[i].rrpv < rrpvMax) continue;
                if (!ways[i].prop) {
                    if (recIdx == n || ways[i].recency < recOldest) {
                        recIdx = i; recOldest = ways[i].recency;
                    }
                    continue;
                }
                const uint32_t d = effDist(ways[i]);
                if (propIdx == n || ways[i].dbg > coldest ||
                    (ways[i].dbg == coldest && d > farthest) ||
                    (ways[i].dbg == coldest && d == farthest &&
                     ways[i].recency < propOldest)) {
                    propIdx = i;
                    coldest = ways[i].dbg;
                    farthest = d;
                    propOldest = ways[i].recency;
                }
            }
            if (recIdx != n) return recIdx;
            if (propIdx != n) return propIdx;
            for (size_t i = 0; i < n; ++i)
                if (ways[i].rrpv < rrpvMax) ways[i].rrpv++;
        }
    }

    // rrip_first (default): among the max-RRPV set, evict the oldest record by
    // recency; else the farthest effective-epoch property. Age and retry if the
    // max-RRPV set yields no candidate.
    for (;;) {
        size_t recIdx = n; uint64_t ro = 0;
        size_t propIdx = n; uint32_t pb = 0;
        for (size_t i = 0; i < n; i++) {
            if (ways[i].rrpv < rrpvMax) continue;
            if (!ways[i].prop) {
                if (recIdx == n || ways[i].recency < ro) { recIdx = i; ro = ways[i].recency; }
            } else {
                uint32_t d = effDist(ways[i]);
                if (propIdx == n || d > pb) { propIdx = i; pb = d; }
            }
        }
        if (recIdx != n) return recIdx;
        if (propIdx != n) return propIdx;
        for (size_t i = 0; i < n; i++) if (ways[i].rrpv < rrpvMax) ways[i].rrpv++;
    }
}

// ---------------------------------------------------------------------------
// GRASP insertion classification shared by all backends.
// Insertion RRPV follows GRASP (Faldu et al., 2020): high-degree
// property lines are protected, low-degree lines are evicted first. A single
// implementation here guarantees cache_sim / gem5 / Sniper classify and insert
// identically (the eviction DECISION above and the INSERTION tier below are now
// both single-source). cache_sim/gem5/Sniper each iterate their own property
// regions and call classifyGraspTier per region — no logic is mirrored.
// ---------------------------------------------------------------------------

// GRASP degree tier of `addr` within ONE property region [base, upper):
// the top `hot_fraction` of the (DBG-reordered) array is HOT(1), the next
// `hot_fraction` is MODERATE(2), the rest is COLD(3). Returns 0 when `addr`
// is outside [base, upper) so the caller can try the next region. The +8
// boundary nudge matches the upstream GRASP (ligra common.h add_region) rule.
inline uint32_t classifyGraspTier(uint64_t addr, uint64_t base, uint64_t upper,
                                  double hot_fraction) {
    if (addr < base || addr >= upper) return 0;
    const uint64_t array_bytes = upper - base;
    const uint64_t hot_bytes = static_cast<uint64_t>(hot_fraction * array_bytes);
    uint64_t hot_bound = base + hot_bytes;
    uint64_t moderate_bound = base + 2 * hot_bytes;
    if (hot_bound > upper) hot_bound = upper;
    if (moderate_bound > upper) moderate_bound = upper;
    hot_bound += 8;
    moderate_bound += 8;
    if (addr < hot_bound) return 1;       // HOT (hubs)  -> protected insertion
    if (addr < moderate_bound) return 2;  // MODERATE
    return 3;                             // COLD        -> evict-first insertion
}

// GRASP insertion RRPV for a degree tier (1/2/3 from classifyGraspTier; 0 or
// out-of-region maps to cold). P_RRIP=1 (protected), I_RRIP=rrpvMax-1,
// M_RRIP=rrpvMax — i.e. 1 / 6 / 7 for a 3-bit RRPV.
inline uint8_t graspTierRRPV(uint32_t tier, uint8_t rrpvMax) {
    if (tier == 1) return 1;
    if (tier == 2) return (rrpvMax > 1) ? static_cast<uint8_t>(rrpvMax - 1) : rrpvMax;
    return rrpvMax;
}

// GRASP degree tier of vertex v by its POSITION in the (DBG-reordered) property
// array: top hot_fraction = HOT(1), next hot_fraction = MODERATE(2), rest COLD(3).
// With elem_size>0 this is BYTE-EXACT to classifyGraspTier (same floor + the +8
// boundary nudge), so the DELIVERED "ECG mask" variant is byte-identical to the
// region-based "original GRASP". elem_size==0 uses the plain index split (for the
// mask dbg tiebreak, where exactness is not required). The per-vertex form is
// computed offline + delivered, so it is identical across simulators.
inline uint32_t graspTierByIndex(uint64_t v, uint64_t num_vertices,
                                 double hot_fraction, uint32_t elem_size = 0) {
    if (num_vertices == 0) return 3;
    if (elem_size == 0) {  // index split (no +8) — mask dbg tiebreak only
        double pos = static_cast<double>(v) / static_cast<double>(num_vertices);
        if (pos < hot_fraction) return 1;
        if (pos < 2.0 * hot_fraction) return 2;
        return 3;
    }
    // BYTE-EXACT mirror of classifyGraspTier (base=0, addr=v*elem_size).
    const uint64_t array_bytes = num_vertices * elem_size;
    const uint64_t addr_off = v * elem_size;
    const uint64_t hot_bytes = static_cast<uint64_t>(hot_fraction * array_bytes);
    uint64_t hot_bound = hot_bytes;            if (hot_bound > array_bytes) hot_bound = array_bytes;
    uint64_t mod_bound = 2 * hot_bytes;        if (mod_bound > array_bytes) mod_bound = array_bytes;
    hot_bound += 8;
    mod_bound += 8;
    if (addr_off < hot_bound) return 1;        // HOT (hubs at the array front)
    if (addr_off < mod_bound) return 2;        // MODERATE
    return 3;                                  // COLD
}

}  // namespace ecg_policy

#endif  // ECG_VICTIM_POLICY_H
