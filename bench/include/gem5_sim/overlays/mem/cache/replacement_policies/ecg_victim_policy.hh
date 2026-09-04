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
// The eleven variants (selected by ECG_VARIANT) and the invariants are documented
// in wiki/ReusePlan-FlowThrough.md. Summary:
//   - epoch is PROPERTY-ONLY; record (non-property) lines never carry a usable
//     epoch and are ranked by recency / set order.
//   - "recency" is normalised so SMALLER == older == evict-first. cache_sim,
//     gem5, and Sniper pass last_access, lastTouchTick, and m_last_touch.
//   - rrpv is aged in place (the SRRIP state update); the caller must write the
//     possibly-incremented rrpv back to its native lines.
#ifndef ECG_VICTIM_POLICY_H
#define ECG_VICTIM_POLICY_H

#include <algorithm>
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
    RECORD_LRU   = 7,  // records first by recency, then property LRU; no epoch
    RRIP_NO_EPOCH = 8, // rrip_first eligibility/records-first with epoch disabled
    RRIP_NO_EPOCH_RECENCY = 9, // same, but property ties use recency
    FUTURE_TIER_FIRST = 10, // future distance, then cold tier, then recency
    NEXT_USE_LRU = 11, // known-dead override; otherwise refine property LRU
};

enum class VictimReason : uint8_t {
    RRIP,
    LRU,
    NON_PROPERTY,
    EPOCH_PROPERTY,
    DEGREE_PROPERTY,
    RECENCY_FALLBACK,
    PROPERTY_FALLBACK,
    PROPERTY_RECENCY,
    FUTURE_TIER_PROPERTY,
    DEAD_PROPERTY,
    NEXT_USE_PROPERTY,
};

enum class FutureState : uint8_t {
    UNKNOWN = 0,
    FINITE = 1,
    DEAD = 2,
};

inline FutureState effectiveFutureState(
        FutureState state, uint32_t next_use, uint32_t current,
        bool refresh_guaranteed = true) {
    if (state != FutureState::FINITE || next_use >= current)
        return state;
    return refresh_guaranteed ? FutureState::DEAD
                              : FutureState::UNKNOWN;
}

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
    if (variant == "record_lru") return RECORD_LRU;
    if (variant == "rrip_no_epoch") return RRIP_NO_EPOCH;
    if (variant == "rrip_no_epoch_recency")
        return RRIP_NO_EPOCH_RECENCY;
    if (variant == "future_tier_first")
        return FUTURE_TIER_FIRST;
    if (variant == "next_use_lru")
        return NEXT_USE_LRU;
    std::fprintf(stderr, "[FATAL] unknown ECG_VARIANT=%s\n", value);
    std::abort();
}

inline bool parseReuseAdmission(const char* value) {
    if (!value || !value[0] || std::string(value) == "0") return false;
    if (std::string(value) == "1") return true;
    std::fprintf(
        stderr, "[FATAL] ECG_REUSE_ADMISSION must be exactly 0 or 1, got %s\n",
        value);
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
    PLACE_FLOWTHROUGH = 1,
    PLACE_ARM_COUNT = 2,
};

// Slots 0..4 are reserved for replacement-policy leaders. Placement dueling
// uses two disjoint leaders per 64 sets and leaves the other 62 as followers.
inline int placementLeaderArm(size_t set_index) {
    const uint64_t slot = static_cast<uint64_t>(set_index) & 63u;
    if (slot == 5) return PLACE_ALLOCATE;
    if (slot == 6) return PLACE_FLOWTHROUGH;
    return -1;
}

class OnlinePlacementSelector {
  public:
    bool shouldFlowThrough(size_t set_index) const {
        const int leader = placementLeaderArm(set_index);
        const uint8_t arm = leader >= 0
            ? static_cast<uint8_t>(leader)
            : winner_.load(std::memory_order_relaxed);
        return arm == PLACE_FLOWTHROUGH;
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
        const uint64_t flowthrough_misses = misses_[PLACE_FLOWTHROUGH].exchange(
            0, std::memory_order_relaxed);
        winner_.store(
            flowthrough_misses < allocate_misses
                ? PLACE_FLOWTHROUGH : PLACE_ALLOCATE,
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

enum AdmissionArm : uint8_t {
    ADMIT_GRASP = 0,
    ADMIT_FUTURE = 1,
    ADMIT_ARM_COUNT = 2,
};

inline int admissionLeaderArm(size_t set_index, uint32_t offset = 0) {
    const uint64_t slot =
        (static_cast<uint64_t>(set_index) + (offset & 63u)) & 63u;
    if ((slot & 7u) == 3u) return ADMIT_GRASP;
    if ((slot & 7u) == 7u) return ADMIT_FUTURE;
    return -1;
}

struct AdmissionSampleEvent {
    bool leader_sample = false;
    bool completed_window = false;
    bool winner_changed = false;
    uint8_t sampled_arm = ADMIT_GRASP;
    uint8_t winner_before = ADMIT_GRASP;
    uint8_t winner_after = ADMIT_GRASP;
};

class OnlineAdmissionSelector {
  public:
    explicit OnlineAdmissionSelector(uint32_t offset = 0)
        : offset_(offset & 63u) {}

    uint8_t armForSet(size_t set_index) const {
        if (trained_.load(std::memory_order_relaxed))
            return winner_.load(std::memory_order_relaxed);
        const int leader = admissionLeaderArm(set_index, offset_);
        return leader >= 0
            ? static_cast<uint8_t>(leader)
            : winner_.load(std::memory_order_relaxed);
    }

    AdmissionSampleEvent recordAccess(size_t set_index, bool missed) {
        AdmissionSampleEvent event;
        event.winner_before = winner_.load(std::memory_order_relaxed);
        event.winner_after = event.winner_before;
        if (trained_.load(std::memory_order_relaxed)) return event;
        const int leader = admissionLeaderArm(set_index, offset_);
        if (leader < 0) return event;
        const size_t arm = static_cast<size_t>(leader);
        uint64_t reserved = accesses_[arm].load(std::memory_order_relaxed);
        while (reserved < kSamplesPerArm &&
               !accesses_[arm].compare_exchange_weak(
                   reserved, reserved + 1, std::memory_order_relaxed)) {}
        if (reserved >= kSamplesPerArm) return event;
        event.leader_sample = true;
        event.sampled_arm = static_cast<uint8_t>(leader);
        total_accesses_[arm].fetch_add(1, std::memory_order_relaxed);
        if (missed) {
            misses_[arm].fetch_add(1, std::memory_order_relaxed);
            total_misses_[arm].fetch_add(1, std::memory_order_relaxed);
        }
        sampled_accesses_.fetch_add(1, std::memory_order_relaxed);
        completed_samples_[arm].fetch_add(1, std::memory_order_release);
        if (
                completed_samples_[ADMIT_GRASP].load(
                    std::memory_order_acquire) <
                    kSamplesPerArm ||
                completed_samples_[ADMIT_FUTURE].load(
                    std::memory_order_acquire) <
                    kSamplesPerArm)
            return event;
        bool expected = false;
        if (!window_claimed_.compare_exchange_strong(
                expected, true, std::memory_order_relaxed))
            return event;
        event.completed_window = true;

        std::array<uint64_t, ADMIT_ARM_COUNT> window_accesses{};
        std::array<uint64_t, ADMIT_ARM_COUNT> window_misses{};
        for (size_t index = 0; index < ADMIT_ARM_COUNT; ++index) {
            window_accesses[index] =
                accesses_[index].load(std::memory_order_relaxed);
            window_misses[index] =
                misses_[index].load(std::memory_order_relaxed);
        }
        uint8_t best = winner_.load(std::memory_order_relaxed);
        const uint8_t other =
            best == ADMIT_GRASP ? ADMIT_FUTURE : ADMIT_GRASP;
        if (window_accesses[best] > 0 && window_accesses[other] > 0) {
            const uint64_t other_scaled =
                window_misses[other] * window_accesses[best];
            const uint64_t best_scaled =
                window_misses[best] * window_accesses[other];
            if (other_scaled < best_scaled) best = other;
        }
        const uint8_t previous =
            winner_.exchange(best, std::memory_order_relaxed);
        completed_windows_.fetch_add(1, std::memory_order_relaxed);
        trained_.store(true, std::memory_order_relaxed);
        event.winner_before = previous;
        event.winner_after = best;
        event.winner_changed = best != previous;
        return event;
    }

    uint8_t winnerArm() const {
        return winner_.load(std::memory_order_relaxed);
    }

    uint64_t totalAccesses(uint8_t arm) const {
        return arm < ADMIT_ARM_COUNT
            ? total_accesses_[arm].load(std::memory_order_relaxed) : 0;
    }

    uint64_t totalMisses(uint8_t arm) const {
        return arm < ADMIT_ARM_COUNT
            ? total_misses_[arm].load(std::memory_order_relaxed) : 0;
    }

    uint64_t completedWindows() const {
        return completed_windows_.load(std::memory_order_relaxed);
    }

    bool trained() const {
        return trained_.load(std::memory_order_relaxed);
    }

    uint32_t offset() const { return offset_; }

    void reset() {
        for (size_t arm = 0; arm < ADMIT_ARM_COUNT; ++arm) {
            accesses_[arm].store(0, std::memory_order_relaxed);
            misses_[arm].store(0, std::memory_order_relaxed);
            completed_samples_[arm].store(0, std::memory_order_relaxed);
            total_accesses_[arm].store(0, std::memory_order_relaxed);
            total_misses_[arm].store(0, std::memory_order_relaxed);
        }
        sampled_accesses_.store(0, std::memory_order_relaxed);
        completed_windows_.store(0, std::memory_order_relaxed);
        winner_.store(ADMIT_GRASP, std::memory_order_relaxed);
        trained_.store(false, std::memory_order_relaxed);
        window_claimed_.store(false, std::memory_order_relaxed);
    }

  private:
    static constexpr uint64_t kSamplesPerArm = 64;
    uint32_t offset_ = 0;
    std::array<std::atomic<uint64_t>, ADMIT_ARM_COUNT> accesses_{};
    std::array<std::atomic<uint64_t>, ADMIT_ARM_COUNT> misses_{};
    std::array<std::atomic<uint64_t>, ADMIT_ARM_COUNT> completed_samples_{};
    std::array<std::atomic<uint64_t>, ADMIT_ARM_COUNT> total_accesses_{};
    std::array<std::atomic<uint64_t>, ADMIT_ARM_COUNT> total_misses_{};
    std::atomic<uint64_t> sampled_accesses_{0};
    std::atomic<uint64_t> completed_windows_{0};
    std::atomic<uint8_t> winner_{ADMIT_GRASP};
    std::atomic<bool> trained_{false};
    std::atomic<bool> window_claimed_{false};
};

inline OnlineAdmissionSelector& globalOnlineAdmissionSelector() {
    static OnlineAdmissionSelector selector;
    return selector;
}

struct WayState {
    bool     prop;     // property (vertex) line, vs record (edge-stream) line
    uint8_t  rrpv;     // RRIP age (aged in place for variants that age)
    uint64_t recency;  // smaller == older == evict-first
    uint8_t  dbg;      // DBG degree tier (shortcircuit all-property tiebreak)
    uint32_t dist;     // raw circular next-ref distance (stored_epoch + ne - cur_epoch) % ne
    bool     stamped;  // epoch is meaningful here (property line with a live stamp)
    uint32_t next_use = 0; // quantized absolute future position
    FutureState future_state = FutureState::UNKNOWN;
};

inline bool victimUsedEpoch(
        VictimReason reason, const WayState& selectedWay) {
    return selectedWay.stamped &&
           (reason == VictimReason::EPOCH_PROPERTY ||
            reason == VictimReason::DEGREE_PROPERTY ||
            reason == VictimReason::FUTURE_TIER_PROPERTY);
}

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

// Map the nearest delivered future-use epoch monotonically into RRIP state.
// A use in the current epoch receives RRPV 0; the farthest representable use
// receives rrpvMax. Ceiling division preserves a distinct nonzero state for
// every positive distance when the epoch space is wider than the RRPV space.
// The first ReusePlan epoch is already the nearest future line reference; the
// second is retained for stale/coalesced-line eviction handling, not admission.
inline uint8_t reuseAdmissionRRPV(
        uint16_t first, uint32_t current, uint32_t ne, uint8_t rrpvMax) {
    if (ne < 2 || rrpvMax == 0) return 0;
    const uint32_t distance = epochDistance(first, current, ne);
    const uint32_t denominator = ne - 1;
    const uint32_t scaled =
        (distance * static_cast<uint32_t>(rrpvMax) + denominator - 1) /
        denominator;
    return static_cast<uint8_t>(
        std::min<uint32_t>(scaled, static_cast<uint32_t>(rrpvMax)));
}

// two-epoch ReusePlan effective distance: the line is needed at the nearer of its next
// two references. count<=1 preserves the legacy single-epoch behavior.
inline uint32_t reusePlanDistance(uint16_t first, uint16_t second,
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
inline size_t selectVictim(WayState* ways, size_t n, int variant,
                           uint8_t rrpvMax,
                           VictimReason* selectedReason = nullptr) {
    auto selected = [selectedReason](size_t index, VictimReason reason) {
        if (selectedReason) *selectedReason = reason;
        return index;
    };
    // grasp_only: pure RRIP — first line at max RRPV, aging until one reaches it.
    if (variant == GRASP_ONLY) {
        for (;;) {
            for (size_t i = 0; i < n; i++)
                if (ways[i].rrpv >= rrpvMax)
                    return selected(i, VictimReason::RRIP);
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
        return selected(victim, VictimReason::LRU);
    }

    // Preserve global LRU for ungoverned traffic. A known-dead property line is
    // always safe to discard before a line that may be reused. Otherwise, only
    // refine an LRU choice that is already a governed finite-use property line.
    if (variant == NEXT_USE_LRU) {
        size_t lru = 0;
        uint64_t oldest = ways[0].recency;
        for (size_t i = 1; i < n; ++i) {
            if (ways[i].recency < oldest) {
                oldest = ways[i].recency;
                lru = i;
            }
        }

        size_t dead = n;
        uint64_t deadOldest = 0;
        for (size_t i = 0; i < n; ++i) {
            if (ways[i].prop &&
                ways[i].future_state == FutureState::DEAD &&
                (dead == n || ways[i].recency < deadOldest)) {
                dead = i;
                deadOldest = ways[i].recency;
            }
        }
        if (dead != n)
            return selected(dead, VictimReason::DEAD_PROPERTY);

        if (!ways[lru].prop ||
            ways[lru].future_state != FutureState::FINITE) {
            return selected(lru, VictimReason::LRU);
        }

        size_t victim = lru;
        uint32_t farthest = ways[lru].next_use;
        uint64_t victimOldest = ways[lru].recency;
        for (size_t i = 0; i < n; ++i) {
            if (!ways[i].prop ||
                ways[i].future_state != FutureState::FINITE) {
                continue;
            }
            if (ways[i].next_use > farthest ||
                (ways[i].next_use == farthest &&
                 ways[i].recency < victimOldest)) {
                victim = i;
                farthest = ways[i].next_use;
                victimOldest = ways[i].recency;
            }
        }
        return selected(victim, VictimReason::NEXT_USE_PROPERTY);
    }

    if (variant == RECORD_LRU) {
        size_t record = n;
        uint64_t recordOldest = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!ways[i].prop &&
                (record == n || ways[i].recency < recordOldest)) {
                record = i;
                recordOldest = ways[i].recency;
            }
        }
        if (record != n)
            return selected(record, VictimReason::NON_PROPERTY);
        size_t victim = 0;
        uint64_t oldest = ways[0].recency;
        for (size_t i = 1; i < n; ++i) {
            if (ways[i].recency < oldest) {
                oldest = ways[i].recency;
                victim = i;
            }
        }
        return selected(victim, VictimReason::RECENCY_FALLBACK);
    }

    // shortcircuit (legacy): evict any non-property line first (set order); if the
    // set is all property, evict the farthest effective-dist line (unstamped property
    // -> dist 0 = kept, so only genuinely stamped property competes; DBG tiebreak).
    if (variant == SHORTCIRCUIT) {
        for (size_t i = 0; i < n; i++)
            if (!ways[i].prop)
                return selected(i, VictimReason::NON_PROPERTY);
        size_t best = 0; uint32_t bd = 0; uint8_t bdbg = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t d = effDist(ways[i]);
            if (d > bd || (d == bd && ways[i].dbg > bdbg)) { best = i; bd = d; bdbg = ways[i].dbg; }
        }
        return selected(best, VictimReason::EPOCH_PROPERTY);
    }

    // epoch_first / epoch_only: records first by recency (no rrpv gate); else the
    // farthest-next-ref stamped property; else recency fallback (LRU).
    if (variant == EPOCH_FIRST || variant == EPOCH_ONLY) {
        size_t rec = n; uint64_t ro = 0;
        for (size_t i = 0; i < n; i++) if (!ways[i].prop)
            if (rec == n || ways[i].recency < ro) { rec = i; ro = ways[i].recency; }
        if (rec != n)
            return selected(rec, VictimReason::NON_PROPERTY);
        size_t best = n; uint32_t bd = 0;
        for (size_t i = 0; i < n; i++) if (ways[i].stamped) {
            uint32_t d = ways[i].dist;
            if (best == n || d > bd) { best = i; bd = d; }
        }
        if (best != n)
            return selected(best, VictimReason::EPOCH_PROPERTY);
        size_t v = 0; uint64_t o = ways[0].recency;
        for (size_t i = 1; i < n; i++) if (ways[i].recency < o) { o = ways[i].recency; v = i; }
        return selected(v, VictimReason::RECENCY_FALLBACK);
    }

    // degree_first (frontier traversal): keep RRIP's eligibility gate, then
    // protect high-degree property lines independent of visit order. Within the
    // coldest degree tier, two-epoch ReusePlan/epoch distance selects the farthest next
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
            if (recIdx != n)
                return selected(recIdx, VictimReason::NON_PROPERTY);
            if (propIdx != n)
                return selected(propIdx, VictimReason::DEGREE_PROPERTY);
            for (size_t i = 0; i < n; ++i)
                if (ways[i].rrpv < rrpvMax) ways[i].rrpv++;
        }
    }

    if (variant == FUTURE_TIER_FIRST) {
        for (;;) {
            size_t recIdx = n; uint64_t recOldest = 0;
            size_t propIdx = n; uint32_t farthest = 0;
            uint8_t coldest = 0; uint64_t propOldest = 0;
            for (size_t i = 0; i < n; ++i) {
                if (ways[i].rrpv < rrpvMax) continue;
                if (!ways[i].prop) {
                    if (recIdx == n || ways[i].recency < recOldest) {
                        recIdx = i;
                        recOldest = ways[i].recency;
                    }
                    continue;
                }
                const uint32_t d = effDist(ways[i]);
                if (propIdx == n || d > farthest ||
                    (d == farthest && ways[i].dbg > coldest) ||
                    (d == farthest && ways[i].dbg == coldest &&
                     ways[i].recency < propOldest)) {
                    propIdx = i;
                    farthest = d;
                    coldest = ways[i].dbg;
                    propOldest = ways[i].recency;
                }
            }
            if (recIdx != n)
                return selected(recIdx, VictimReason::NON_PROPERTY);
            if (propIdx != n)
                return selected(
                    propIdx, VictimReason::FUTURE_TIER_PROPERTY);
            for (size_t i = 0; i < n; ++i)
                if (ways[i].rrpv < rrpvMax) ways[i].rrpv++;
        }
    }

    // rrip_first (default): among the max-RRPV set, evict the oldest record by
    // recency; else the farthest effective-epoch property. The two no-epoch
    // controls retain the same gate and records-first rule, then use either
    // fixed set order or true recency among property candidates.
    const bool rripNoEpochPosition = variant == RRIP_NO_EPOCH;
    const bool rripNoEpochRecency =
        variant == RRIP_NO_EPOCH_RECENCY;
    for (;;) {
        size_t recIdx = n; uint64_t ro = 0;
        size_t propIdx = n; uint32_t pb = 0; uint64_t propOldest = 0;
        for (size_t i = 0; i < n; i++) {
            if (ways[i].rrpv < rrpvMax) continue;
            if (!ways[i].prop) {
                if (recIdx == n || ways[i].recency < ro) { recIdx = i; ro = ways[i].recency; }
            } else {
                if (rripNoEpochRecency) {
                    if (
                            propIdx == n ||
                            ways[i].recency < propOldest) {
                        propIdx = i;
                        propOldest = ways[i].recency;
                    }
                } else {
                    uint32_t d =
                        rripNoEpochPosition ? 0 : effDist(ways[i]);
                    if (propIdx == n || d > pb) {
                        propIdx = i;
                        pb = d;
                    }
                }
            }
        }
        if (recIdx != n)
            return selected(recIdx, VictimReason::NON_PROPERTY);
        if (propIdx != n)
            return selected(
                propIdx,
                rripNoEpochPosition
                    ? VictimReason::PROPERTY_FALLBACK
                    : rripNoEpochRecency
                        ? VictimReason::PROPERTY_RECENCY
                        : VictimReason::EPOCH_PROPERTY);
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

inline uint32_t classifyGraspTierCapacity(
        uint64_t addr, uint64_t base, uint64_t upper,
        uint64_t llc_size, double capacity_fraction) {
    if (addr < base || addr >= upper + 8) return 0;
    const uint64_t hot_bytes = static_cast<uint64_t>(
        capacity_fraction * static_cast<double>(llc_size));
    uint64_t hot_bound = std::min<uint64_t>(upper, base + hot_bytes) + 8;
    uint64_t moderate_bound =
        std::min<uint64_t>(upper, base + 2 * hot_bytes) + 8;
    if (addr < hot_bound) return 1;
    if (addr < moderate_bound) return 2;
    return 3;
}

// GRASP insertion RRPV for a degree tier (1/2/3 from classifyGraspTier; 0 or
// out-of-region maps to cold). P_RRIP=1 (protected), I_RRIP=rrpvMax-1,
// M_RRIP=rrpvMax — i.e. 1 / 6 / 7 for a 3-bit RRPV.
inline uint8_t graspTierRRPV(uint32_t tier, uint8_t rrpvMax) {
    if (tier == 1) return 1;
    if (tier == 2) return (rrpvMax > 1) ? static_cast<uint8_t>(rrpvMax - 1) : rrpvMax;
    return rrpvMax;
}

inline uint8_t combinedInsertionRRPV(
        uint32_t tier, uint32_t distance_hint, uint32_t hint_max,
        uint8_t rrpvMax) {
    const uint8_t dbg = graspTierRRPV(tier, rrpvMax);
    const uint32_t denominator = std::max<uint32_t>(1, hint_max);
    const uint8_t future = static_cast<uint8_t>(std::min<uint32_t>(
        rrpvMax, (distance_hint * rrpvMax) / denominator));
    uint8_t combined = static_cast<uint8_t>(
        (static_cast<uint32_t>(dbg) + future) / 2);
    if (combined == 0 && dbg > 0) combined = 1;
    return std::min<uint8_t>(combined, rrpvMax);
}

inline uint8_t combinedReuseAdmissionRRPV(
        uint32_t tier, uint16_t first, uint32_t current,
        uint32_t ne, uint8_t rrpvMax) {
    const uint32_t distance = epochDistance(first, current, ne);
    return combinedInsertionRRPV(
        tier, distance, ne > 1 ? ne - 1 : 1, rrpvMax);
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
