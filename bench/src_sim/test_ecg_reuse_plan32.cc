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
#include "ecg_victim_policy.h"

int main(int argc, char* argv[]) {
    if (ecg_reuse_plan::quantizedFutureEpoch(2, 1, 16, 4, false) != 0 ||
        ecg_reuse_plan::quantizedFutureEpoch(1, 1, 16, 4, true) != 3 ||
        ecg_reuse_plan::quantizedFutureEpoch(6, 5, 16, 4, false) != 1 ||
        ecg_reuse_plan::quantizedFutureEpoch(5, 5, 16, 4, true) != 0) {
        printf("WRAP ENCODING MISMATCH\n");
        return 1;
    }
    if (ecg_policy::reuseAdmissionRRPV(0, 0, 16, 7) != 0 ||
        ecg_policy::reuseAdmissionRRPV(1, 0, 16, 7) != 1 ||
        ecg_policy::reuseAdmissionRRPV(8, 0, 16, 7) != 4 ||
        ecg_policy::reuseAdmissionRRPV(15, 0, 16, 7) != 7) {
        printf("ADMISSION MAPPING MISMATCH\n");
        return 1;
    }

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
        std::vector<std::vector<uint16_t>> first_epochs;
        ecg_reuse_plan::buildInEdgeEpochs(
            g, 16, ne, true, first_epochs);
        ecg_reuse_plan::buildInEdgeReusePlanRecords(g, 16, ne, true, off64, rec64);
        bool ok32 = ecg_reuse_plan::buildInEdgeReusePlanRecords32(
            g, 16, ne, true, off32, rec32);
        if (!ok32) { printf("ne=%u: compact refused (expected when fields do not fit)\n", ne); continue; }
        if (off64 != off32) { printf("ne=%u: OFFSET MISMATCH\n", ne); return 1; }
        if (rec64.size() != rec32.size()) { printf("ne=%u: SIZE MISMATCH\n", ne); return 1; }
        const uint32_t idb = ecg_reuse_plan::reusePlan32IdBits(n);
        const uint32_t epb = ecg_reuse_plan::reusePlan32EpochBits(ne);
        uint64_t bad = 0, checked = 0;
        size_t flat = 0;
        for (size_t src = 0; src < first_epochs.size(); ++src) {
          for (size_t edge = 0; edge < first_epochs[src].size(); ++edge, ++flat) {
            if (flat >= rec64.size() ||
                first_epochs[src][edge] !=
                    ecg_reuse_plan::extractReusePlanFirst(rec64[flat])) {
                printf("ne=%u: FIRST-EPOCH MISMATCH src=%zu edge=%zu\n",
                       ne, src, edge);
                return 1;
            }
          }
        }
        if (flat != rec64.size()) {
            printf("ne=%u: FIRST-EPOCH SIZE MISMATCH\n", ne);
            return 1;
        }
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
