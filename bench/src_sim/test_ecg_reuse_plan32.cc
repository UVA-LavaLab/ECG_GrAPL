// Direct proof: every compact record must decode to exactly the same
// (dest, tier, first, second) as the 64-bit record built from the same graph.
// This is far stronger than comparing kernel outputs, which cannot detect a
// wrong epoch at all.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "ecg_reuse_plan_builder.h"

int main(int argc, char* argv[]) {
    CLBase cli(argc, argv, "ReusePlan-equivalence");
    if (!cli.ParseArgs()) return 1;
    WeightedBuilder bw(cli);
    Builder b(cli);
    Graph g = b.MakeGraph();
    const uint32_t n = static_cast<uint32_t>(g.num_nodes());

    for (uint32_t ne : {8u, 16u, 32u, 64u}) {
        std::vector<uint64_t> off64, rec64;
        std::vector<uint64_t> off32;
        std::vector<uint32_t> rec32;
        ecg_reuse_plan::buildInEdgeReusePlanRecords(g, 16, ne, true, off64, rec64);
        bool ok32 = ecg_reuse_plan::buildInEdgeReusePlanRecords32(
            g, 16, ne, true, off32, rec32);
        if (!ok32) { printf("ne=%u: compact refused (expected when fields do not fit)\n", ne); continue; }
        if (off64 != off32) { printf("ne=%u: OFFSET MISMATCH\n", ne); return 1; }
        if (rec64.size() != rec32.size()) { printf("ne=%u: SIZE MISMATCH\n", ne); return 1; }
        const uint32_t idb = ecg_reuse_plan::reusePlan32IdBits(n);
        const uint32_t epb = ecg_reuse_plan::reusePlan32EpochBits(ne);
        uint64_t bad = 0, checked = 0;
        for (size_t i = 0; i < rec64.size(); ++i) {
            const uint64_t w = ecg_reuse_plan::widenReusePlan32(rec32[i], idb, epb);
            if (w != rec64[i]) {
                if (bad < 3)
                    printf("  edge %zu: 64b=%016llx widened=%016llx\n",
                           i, (unsigned long long)rec64[i], (unsigned long long)w);
                bad++;
            }
            checked++;
        }
        printf("ne=%2u: %llu records checked, %llu mismatches  id_bits=%u epoch_bits=%u\n",
               ne, (unsigned long long)checked, (unsigned long long)bad, idb, epb);
        if (bad) return 1;
    }
    printf("ALL EQUIVALENT\n");
    return 0;
}
