// Field-width verification for the gem5 ECG_PFX prefetch-target delivery.
//
// The shared ECG mask (bench/include/ecg_mode6_builder.h) has two layouts:
//   packMask      — prefetch target at bit 33 (31-bit field), used by
//                   buildInEdgeMasks (the shared builder) and read by Sniper via
//                   extractPrefetchTarget. No practical truncation.
//   packMaskEpoch — epoch at bit 33 (16-bit) + prefetch target at bit 49
//                   (15-bit field), used by the gem5 PR kernel re-pack and the
//                   RISCV ecg.extract ISA op (decoder reads bits[49:64]).
//
// Consequence (verified here): the gem5 ISA-delivered prefetch target is capped
// at 15 bits (<=32767). On a graph with a target vertex id > 32767 the target is
// silently truncated to a WRONG vertex, so the gem5 PR kernel guards against it
// (counts targets > 0x7FFF, warns, and aborts under ECG_PFX_STRICT_TARGET=1) and
// cache_sim remains the large-graph prefetch model. Sniper is unaffected
// because it uses the 31-bit packMask field.
//
// This test pins those field widths so the limit cannot regress unnoticed.
#include "ecg_mode6_builder.h"
#include <cstdint>
#include <cstdio>

int main() {
    int fails = 0;
    const uint32_t cases[] = {1000u, 32767u, 32768u, 50000u, 65535u};
    printf("[test_ecg_pfx_field_width] packMask(bit33,31b) vs packMaskEpoch(bit49,15b)\n");
    for (uint32_t tgt : cases) {
        uint64_t m  = ecg_mode6::packMask(123, 1, 5, tgt);          // builder/Sniper layout
        uint32_t r33 = ecg_mode6::extractPrefetchTarget(m);         // 31-bit, full
        bool guard = (r33 > 0x7FFFu);                                // gem5 kernel's check
        uint64_t me = ecg_mode6::packMaskEpoch(123, 1, 5, 7, r33);  // gem5 re-pack
        uint32_t r49 = ecg_mode6::extractPrefetchTargetEpoch(me);   // 15-bit, may truncate
        bool truncated = (r49 != tgt);
        // The 31-bit field must round-trip exactly; the guard must flag exactly
        // the cases the 15-bit field truncates.
        bool ok = (r33 == tgt) && (guard == truncated);
        printf("    target=%-6u packMask=%-6u guard=%d truncated=%d(->%u)  [%s]\n",
               tgt, r33, guard, truncated, r49, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    printf("  RESULT: %s\n", fails ? "FAIL" : "PASS (guard flags exactly the >32767 targets)");
    return fails ? 1 : 0;
}
