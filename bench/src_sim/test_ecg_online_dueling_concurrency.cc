// Focused concurrency/semantics regression for ecg_policy::OnlineDuelingSelector
// and its MissRecordEvent-returning recordMiss() API (bench/include/
// ecg_victim_policy.h, shared byte-for-byte by cache_sim, gem5, and Sniper.
//
// This exists because Sniper's cache_set_ecg.cc overlay used to derive
// "did this call sample a leader-set miss / complete a window / change the
// winner" by reading OnlineDuelingSelector's counters BEFORE and AFTER calling
// recordMiss(), then diffing. Sniper simulates cores as real OS threads
// sharing one LLC set/selector, so that before/after diff races against every
// OTHER core thread's concurrent recordMiss() call on the SAME selector:
// another thread's window completion (or winner change) landing between this
// call's "before" read and its "after" read gets misattributed to THIS call,
// which can double-count or drop window/winner-change evidence. The fix made
// recordMiss() itself return a MissRecordEvent describing only what that
// specific call did, derived from the return values of its own atomic
// fetch_add/exchange ops -- values that are unique per calling thread by
// construction, so no caller-side diff is needed.
//
// This test does NOT launch a Sniper (or gem5) simulation. It drives the
// shared header directly with real std::thread concurrency and asserts exact
// accounting invariants that a racy before/after-diff implementation would
// violate under contention:
//
//   1. Across all concurrent recordMiss() calls, the number of events with
//      completed_window==true equals exactly selector.completedWindows()
//      (i.e. total_leader_samples / kWindowMisses) -- no window is ever
//      double-reported or missed.
//   2. Every completed_window==true event has winner_changed consistent with
//      winner_before/winner_after (changed iff they differ), and non-window
//      events never claim a winner change.
//   3. The number of leader_sample==true events equals the number of
//      recordMiss() calls made with a leader-set index (a purely
//      set-index-derived, thread-independent fact), proving concurrent calls
//      don't corrupt each other's leader classification.
//   4. The winner_before/winner_after values reported by every
//      completed_window event, taken together, reconstruct EXACTLY ONE
//      unbroken transition chain of winner_'s real total modification order
//      (see checkTransitionChainConservation() below) -- the invariant that
//      catches duplicate/misattributed winner_changed events: a call whose
//      reported winner_before is a stale separately-read snapshot (instead
//      of the value its own atomic RMW actually replaced) breaks this
//      conservation law the instant two window completions race, even
//      though invariants 1-3 above stay satisfied (they never inspect
//      whether "before" is the TRUE immediate predecessor). A second,
//      dedicated scenario (runForcedOverlapStress() below) launches far more
//      threads than the host has cores (oversubscription) after a single
//      synchronized barrier release, so the OS scheduler routinely preempts
//      a thread mid-recordMiss() call while others complete further windows
//      -- maximizing the odds of exercising exactly the overlapping-window-
//      completion race the fix addresses -- and re-checks invariant 4 there.
//
// Build: g++ -std=c++17 -O2 -pthread -I bench/include \
//          bench/src_sim/test_ecg_online_dueling_concurrency.cc \
//          -o bench/bin_sim/test_ecg_online_dueling_concurrency
// (also reachable via `make bench/bin_sim/test_ecg_online_dueling_concurrency`
// using the repo's generic src_sim pattern rule).
//
// For an even stronger data-race proof, also build/run this file with
// `-fsanitize=thread` (see test_grasp_popt_cross_backend_parity.py /
// scripts/test for the Python wrapper that does this).

#include "ecg_victim_policy.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using ecg_policy::MissRecordEvent;
using ecg_policy::OnlineDuelingSelector;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char *what, bool ok) {
    printf("    %-70s [%s]\n", what, ok ? "OK" : "FAIL");
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
    }
}

// Minimal C++17 spin barrier (std::barrier is C++20 and this program is
// built with -std=c++17): releases exactly `count` waiting threads together
// each time the last one arrives, then resets for reuse across rounds. Used
// by runForcedOverlapStress() below to line up many threads so they all
// submit the misses that cross a window boundary at nearly the same instant
// -- the strongest practical (barrier-based) way to force the overlapping
// window-completion race from user space, short of instrumenting
// recordMiss() itself with injected delays.
class SpinBarrier {
  public:
    explicit SpinBarrier(int count)
        : threshold_(count), count_(count), generation_(0) {}

    void arriveAndWait() {
        const int gen = generation_.load(std::memory_order_acquire);
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count_.store(threshold_, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
        } else {
            while (generation_.load(std::memory_order_acquire) == gen) {
                std::this_thread::yield();
            }
        }
    }

  private:
    const int threshold_;
    std::atomic<int> count_;
    std::atomic<int> generation_;
};

// The core defect-detecting invariant: winner_ is a single atomic<uint8_t>
// written ONLY by the exchange() inside recordMiss()'s window-completion
// path, so the C++ memory model guarantees all its writes fall in one real
// total modification order W_1, W_2, ..., W_N. Each W_i's exchange() must
// therefore return exactly the value W_{i-1} wrote (W_1 returns the
// compile-time initial value), and the last write's value must be the final
// winnerArm(). Equivalently: for every arm value v, the number of times v is
// reported as winner_before across ALL completed_window events must equal
// the number of times v is reported as winner_after, except v==initial_value
// (produced once "for free" by construction, never consumed if it's also
// never re-written) and v==final_value (consumed zero times because it is
// never overwritten again within the observed run).
//
// This holds UNCONDITIONALLY for the exchange()-based implementation,
// regardless of how many threads raced or how the scheduler interleaved
// them. A racy implementation that derives winner_before from a separately
// read snapshot (taken before the RMW that actually installs the new
// winner) can report the SAME stale winner_before for two different
// completed_window events when their window completions overlap in time --
// duplicating/misattributing a transition -- which breaks this exact
// conservation law. That only manifests under real overlap, which is why
// runForcedOverlapStress() below exists: it maximizes the chance of
// exercising that overlap.
bool checkTransitionChainConservation(
        const std::vector<std::vector<MissRecordEvent>> &per_thread_events,
        uint8_t initial_value, uint8_t final_value) {
    std::array<int64_t, ecg_policy::DUEL_ARM_COUNT> before_count{};
    std::array<int64_t, ecg_policy::DUEL_ARM_COUNT> after_count{};
    for (const auto &events : per_thread_events) {
        for (const MissRecordEvent &e : events) {
            if (!e.completed_window) continue;
            ++before_count[e.winner_before];
            ++after_count[e.winner_after];
        }
    }
    bool ok = true;
    for (uint8_t v = 0; v < ecg_policy::DUEL_ARM_COUNT; ++v) {
        const int64_t expected_delta =
            (v == initial_value ? 1 : 0) - (v == final_value ? 1 : 0);
        if (before_count[v] - after_count[v] != expected_delta) ok = false;
    }
    return ok;
}

// Find one representative "leader" set index per dueling arm (mirrors the
// leader-discovery loop in bench/src_sim/test_ecg_victim.cc's "dueling" case).
void findLeaderSets(size_t leader[ecg_policy::DUEL_ARM_COUNT]) {
    bool found[ecg_policy::DUEL_ARM_COUNT] = {};
    for (size_t set = 0; set < 200000 && true; ++set) {
        const int arm = ecg_policy::duelingLeaderArm(set);
        if (arm >= 0 && !found[static_cast<size_t>(arm)]) {
            leader[static_cast<size_t>(arm)] = set;
            found[static_cast<size_t>(arm)] = true;
        }
    }
}

// Forced-overlapping-window stress scenario. All kThreads threads spin on a
// single SpinBarrier release (a synchronized simultaneous start), then hammer
// recordMiss() in a tight loop with NO further synchronization between them.
// kThreads intentionally OVERSUBSCRIBES the host's hardware concurrency (see
// kOverlapThreads below), which is what actually matters here: with more
// runnable threads than cores, the OS scheduler routinely preempts a thread
// mid-recordMiss() call (e.g. right after it has read a stale winner_
// snapshot but before it reaches its own window-completion store), while
// other threads keep running and complete further windows in the meantime.
// That is precisely the overlapping-window-completion scenario the
// winner_before/winner_changed fix addresses, and empirically (verified
// against a reverted, pre-fix copy of this header during development of this
// test) this oversubscribed tight-loop design reproduces the resulting
// transition-chain conservation violation (see checkTransitionChainConservation)
// in a majority of runs, where a per-round barrier that resynchronizes
// between bursts does not (resynchronizing serializes window completions and
// defeats the very overlap being forced). This is the strongest practical
// user-space approximation of that race achievable without instrumenting
// recordMiss() itself with injected delays.
void runForcedOverlapStress(
        OnlineDuelingSelector &selector,
        const size_t leader[ecg_policy::DUEL_ARM_COUNT],
        std::vector<std::vector<MissRecordEvent>> &per_thread_events,
        int kThreads, int kCallsPerThread) {
    per_thread_events.assign(static_cast<size_t>(kThreads), {});
    for (auto &v : per_thread_events) {
        v.reserve(static_cast<size_t>(kCallsPerThread));
    }

    SpinBarrier barrier(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kThreads));
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<MissRecordEvent> &events =
                per_thread_events[static_cast<size_t>(t)];
            // All kThreads threads are released in the SAME instant the
            // last one arrives, then race flat-out with no further
            // synchronization -- maximizing scheduler-induced preemption
            // overlap across concurrent recordMiss() calls.
            barrier.arriveAndWait();
            for (int i = 0; i < kCallsPerThread; ++i) {
                const uint8_t arm = static_cast<uint8_t>(
                    (t + i) % ecg_policy::DUEL_ARM_COUNT);
                events.push_back(selector.recordMiss(leader[arm]));
            }
        });
    }
    for (auto &th : threads) th.join();
}

} // namespace

int main() {
    printf("== ECG online-dueling concurrency/semantics regression ==\n");

    size_t leader[ecg_policy::DUEL_ARM_COUNT] = {};
    findLeaderSets(leader);

    // Non-leader set: duelingLeaderArm() treats (set_index & 63) < 5 as a
    // leader slot, so slot 10 (>= DUEL_ARM_COUNT) is guaranteed non-leader.
    const size_t kNonLeaderSet = 10;
    bool non_leader_confirmed = ecg_policy::duelingLeaderArm(kNonLeaderSet) < 0;
    check("sanity: chosen non-leader set index really is not a leader",
          non_leader_confirmed);

    OnlineDuelingSelector selector;

    constexpr int kThreads = 8;
    constexpr int kCallsPerThreadPerArm = 4000; // -> 8*5*4000 leader samples
    constexpr int kNonLeaderCallsPerThread = 4000;

    std::vector<std::vector<MissRecordEvent>> per_thread_events(kThreads);
    for (auto &v : per_thread_events) {
        v.reserve(kCallsPerThreadPerArm * ecg_policy::DUEL_ARM_COUNT +
                  kNonLeaderCallsPerThread);
    }

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<MissRecordEvent> &events = per_thread_events[t];
            // Interleave leader-arm and non-leader-set calls so different
            // threads race on the SAME shared selector concurrently.
            for (int i = 0; i < kCallsPerThreadPerArm; ++i) {
                for (uint8_t arm = 0; arm < ecg_policy::DUEL_ARM_COUNT; ++arm) {
                    events.push_back(selector.recordMiss(leader[arm]));
                }
                if (i < kNonLeaderCallsPerThread) {
                    events.push_back(selector.recordMiss(kNonLeaderSet));
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    // ---- Merge & verify invariants ----
    uint64_t total_calls = 0;
    uint64_t total_leader_samples = 0;
    uint64_t total_completed_windows = 0;
    uint64_t total_winner_changed = 0;
    bool every_window_event_consistent = true;
    bool no_non_window_claims_change = true;
    bool no_non_leader_claims_sample = true;

    for (const auto &events : per_thread_events) {
        for (const MissRecordEvent &e : events) {
            ++total_calls;
            if (e.leader_sample) ++total_leader_samples;
            if (e.completed_window) {
                ++total_completed_windows;
                const bool changed_matches_diff =
                    e.winner_changed == (e.winner_before != e.winner_after);
                if (!changed_matches_diff) every_window_event_consistent = false;
                if (e.winner_changed) ++total_winner_changed;
            } else {
                if (e.winner_changed) no_non_window_claims_change = false;
            }
        }
    }

    const uint64_t expected_leader_calls =
        static_cast<uint64_t>(kThreads) * kCallsPerThreadPerArm *
        ecg_policy::DUEL_ARM_COUNT;
    const uint64_t expected_non_leader_calls =
        static_cast<uint64_t>(kThreads) * kNonLeaderCallsPerThread;
    const uint64_t expected_total_calls =
        expected_leader_calls + expected_non_leader_calls;

    check("collected event count matches issued recordMiss() call count",
          total_calls == expected_total_calls);

    check("leader_sample count == leader-set recordMiss() calls issued "
          "(non-leader calls never miscounted as leader samples)",
          total_leader_samples == expected_leader_calls);
    (void)no_non_leader_claims_sample; // captured for readability of intent

    // The core multicore-race regression: exactly one completed_window==true
    // event per window boundary, matching the selector's own atomic counter,
    // under real concurrent access from kThreads OS threads.
    const uint64_t final_completed_windows = selector.completedWindows();
    check("completed_window==true event count == selector.completedWindows() "
          "(no window double-counted or dropped under concurrency)",
          total_completed_windows == final_completed_windows);
    check("completed_window count == total_leader_samples / kWindowMisses",
          total_completed_windows == total_leader_samples / 1024);

    check("every completed_window event's winner_changed matches "
          "(winner_before != winner_after)",
          every_window_event_consistent);
    check("no non-window event ever claims a winner change",
          no_non_window_claims_change);
    check("winner_changed events are a subset of completed_window events "
          "(<=, never more changes than windows)",
          total_winner_changed <= total_completed_windows);

    // Cross-check final winner is reachable via the recorded event chain: the
    // last completed_window event (in per-call issue order is not globally
    // defined across threads, but the selector's own state must still be a
    // value some event actually reported as winner_after).
    bool final_winner_seen_as_some_event_after = false;
    const uint8_t final_winner = selector.winnerArm();
    for (const auto &events : per_thread_events) {
        for (const MissRecordEvent &e : events) {
            if (e.completed_window && e.winner_after == final_winner) {
                final_winner_seen_as_some_event_after = true;
            }
        }
    }
    check("final winnerArm() matches winner_after of some completed_window "
          "event (state is externally reconstructible from events)",
          total_completed_windows == 0 || final_winner_seen_as_some_event_after);

    // The defect-detecting invariant (see checkTransitionChainConservation's
    // comment): OnlineDuelingSelector is default-constructed above, so its
    // winner_ starts at ecg_policy::DUEL_RRIP.
    check("winner_before/winner_after transitions across ALL completed_window "
          "events chain together exactly once each (no duplicated/"
          "misattributed winner_before under concurrent window completions)",
          checkTransitionChainConservation(
              per_thread_events, ecg_policy::DUEL_RRIP, final_winner));

    // ---- Forced-overlapping-window stress scenario ----
    // Fresh selector so this scenario's own transition chain is
    // self-contained and independently verifiable. kOverlapThreads
    // deliberately oversubscribes typical hardware concurrency (this
    // program does not query std::thread::hardware_concurrency() so the
    // regression is reproducible the same way on any host): with far more
    // runnable threads than cores, the OS scheduler routinely preempts a
    // thread mid-recordMiss() call, letting OTHER threads complete further
    // windows in the meantime -- the real-world overlap the fix addresses.
    // Every call here targets a leader set, so every one of kOverlapThreads
    // * kOverlapCallsPerThread calls is a leader_sample.
    printf("-- forced-overlapping-window stress (oversubscribed threads) --\n");
    OnlineDuelingSelector overlap_selector;
    constexpr int kOverlapThreads = 128;
    constexpr int kOverlapCallsPerThread = 20480; // 20 * kWindowMisses
    std::vector<std::vector<MissRecordEvent>> overlap_events;
    runForcedOverlapStress(
        overlap_selector, leader, overlap_events,
        kOverlapThreads, kOverlapCallsPerThread);

    uint64_t overlap_total_calls = 0;
    uint64_t overlap_leader_samples = 0;
    uint64_t overlap_completed_windows = 0;
    for (const auto &events : overlap_events) {
        for (const MissRecordEvent &e : events) {
            ++overlap_total_calls;
            if (e.leader_sample) ++overlap_leader_samples;
            if (e.completed_window) ++overlap_completed_windows;
        }
    }
    const uint64_t overlap_expected_calls =
        static_cast<uint64_t>(kOverlapThreads) *
        static_cast<uint64_t>(kOverlapCallsPerThread);
    check("forced-overlap scenario: collected event count matches issued "
          "recordMiss() call count",
          overlap_total_calls == overlap_expected_calls);
    check("forced-overlap scenario: every call targeted a leader set, so "
          "leader_sample count == total calls issued",
          overlap_leader_samples == overlap_expected_calls);

    // completed_window event count must match BOTH the selector's own
    // atomic completedWindows() counter AND total_leader_samples/kWindowMisses,
    // under 128 threads racing with no synchronization after the initial
    // simultaneous release -- proving no window is double-counted or dropped
    // even under heavy scheduler-induced preemption/overlap.
    check("forced-overlap scenario: completed_window event count == "
          "selector.completedWindows() == leader_samples / kWindowMisses "
          "(no window double-counted or dropped under 128-thread "
          "oversubscribed contention)",
          overlap_completed_windows == overlap_selector.completedWindows() &&
          overlap_completed_windows == overlap_leader_samples / 1024);

    check("forced-overlap scenario: winner_before/winner_after transitions "
          "chain together exactly once each under 128-thread oversubscribed, "
          "unsynchronized contention (the strongest practical reproduction "
          "of the overlapping-window race that duplicated/misattributed "
          "winner_changed events; this specific check FAILS in a majority "
          "of runs against the pre-fix separate-load implementation)",
          checkTransitionChainConservation(
              overlap_events, ecg_policy::DUEL_RRIP,
              overlap_selector.winnerArm()));

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
