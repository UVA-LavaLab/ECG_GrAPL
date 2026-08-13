// Synthetic deterministic test for the ECG prefetch-target decision.
//
// Drives ecg_mode6::selectPrefetchTarget (bench/include/ecg_mode6_builder.h) —
// the SAME pure function the kernel uses (via buildInEdgeMasks) to encode the
// prefetch target into every per-edge mask, for cache_sim, gem5 AND Sniper (the
// builder is a single shared header compiled into each kernel). So this directly
// verifies the ECG prefetch target for all three simulators.
//
// Each case builds a controlled in-neighbour list + per-line average re-reference
// table and asserts the EXACT target: among the next k_lookahead neighbours, the
// one with the smallest re-reference distance (POPT-best); 0 when disabled / no
// candidate. Mutation-proven: flipping the impl's "< best" to ">" makes the
// min-vs-max cases below FAIL.
#include "ecg_mode6_builder.h"
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_pass = 0, g_fail = 0;

// num_vtx_per_line = 1 so avg_reref_by_line is indexed directly by vertex id.
static void check(const char* name, std::vector<uint32_t> neighbors, size_t i,
                  std::vector<uint8_t> reref, int k, uint32_t expected) {
    uint32_t got = ecg_mode6::selectPrefetchTarget(
        neighbors.data(), neighbors.size(), i, reref, k, /*num_vtx_per_line=*/1);
    bool ok = (got == expected);
    printf("    %-52s expect=%u got=%u  [%s]\n", name, expected, got, ok ? "OK" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

int main() {
    printf("[test_ecg_prefetch] ecg_mode6::selectPrefetchTarget\n");
    // reref[v] = average re-reference distance of vertex v's property line.
    // neighbors = [10, 3, 7, 2, 9]; reref: 10->50, 3->20, 7->5, 2->40, 9->30.
    std::vector<uint8_t> R(11, 100);
    R[10] = 50; R[3] = 20; R[7] = 5; R[2] = 40; R[9] = 30;
    std::vector<uint32_t> N = {10, 3, 7, 2, 9};

    check("K=4 from pos0 -> min reref is vertex7 (d=5)",        N, 0, R, 4, 7);
    check("K=2 from pos0 -> min(3:20,7:5) = vertex7",           N, 0, R, 2, 7);
    check("K=1 from pos0 -> only vertex3",                      N, 0, R, 1, 3);
    check("pos3, K=4 -> probe clipped to 1 -> vertex9",         N, 3, R, 4, 9);
    check("pos4 (last) -> no candidate -> 0",                   N, 4, R, 4, 0);
    check("k_lookahead=0 -> prefetch disabled -> 0",            N, 0, R, 0, 0);

    // tie: first minimum wins (strict <).
    std::vector<uint8_t> Rt(4, 100); Rt[2] = 10; Rt[3] = 10;
    check("tie (v2=10,v3=10) -> first min vertex2",             {1, 2, 3}, 0, Rt, 2, 2);

    // invalid entry (high bit / >=128) is never selected.
    std::vector<uint8_t> Ri(4, 100); Ri[2] = 200; Ri[3] = 60;
    check("invalid reref (v2=200) skipped -> vertex3",          {1, 2, 3}, 0, Ri, 2, 3);

    // candidate vertex beyond the reref table is skipped.
    std::vector<uint8_t> Rb(5, 100);
    check("out-of-range candidate (v100) skipped -> 0",         {1, 100}, 0, Rb, 4, 0);

    // empty reref table -> disabled.
    check("empty reref table -> 0",                             N, 0, {}, 4, 0);

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
