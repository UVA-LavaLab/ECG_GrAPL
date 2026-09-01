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

namespace {

// The record layout is arithmetic, so prove it arithmetically before touching
// a graph: the exact n18/128-epoch boundary is 18 + 0 + 7 + 7 = 32 bits, and
// the same geometry with the canonical 2 tier bits needs 34 and must be
// refused rather than truncated.
bool checkTierWidthBoundary() {
    const uint32_t n18 = 1u << 18;
    if (ecg_reuse_plan::reusePlan32IdBits(n18) != 18 ||
        ecg_reuse_plan::reusePlan32EpochBits(128) != 7) {
        printf("N18 FIELD WIDTH MISMATCH\n");
        return false;
    }
    if (!ecg_reuse_plan::canPackReusePlan32(n18, 128, 0)) {
        printf("N18 TIERLESS RECORD MUST FIT IN 32 BITS\n");
        return false;
    }
    if (ecg_reuse_plan::canPackReusePlan32(n18, 128, 2)) {
        printf("N18 TIERED RECORD MUST NOT FIT IN 32 BITS\n");
        return false;
    }
    // Undefined tier widths fail closed instead of packing a field no
    // decoder implements.
    for (uint32_t tier_bits : {1u, 3u, 4u, 8u}) {
        if (ecg_reuse_plan::reusePlan32TierBitsSupported(tier_bits) ||
            ecg_reuse_plan::canPackReusePlan32(1u << 10, 8, tier_bits)) {
            printf("UNSUPPORTED TIER WIDTH %u WAS ACCEPTED\n", tier_bits);
            return false;
        }
    }
    // Every field must survive a round trip at the exact 32-bit boundary,
    // including the widest destination and epoch the geometry allows.
    for (uint32_t dest : {0u, 1u, (1u << 18) - 1u}) {
        for (uint16_t first : {uint16_t(0), uint16_t(63), uint16_t(127)}) {
            for (uint16_t second : {uint16_t(0), uint16_t(1), uint16_t(127)}) {
                const uint32_t record = ecg_reuse_plan::packReusePlanRecord32(
                    dest, 3, first, second, 18, 7, 0);
                if (ecg_reuse_plan::extractReusePlan32Dest(record, 18) != dest ||
                    ecg_reuse_plan::extractReusePlan32Tier(record, 18, 0) != 0 ||
                    ecg_reuse_plan::extractReusePlan32First(
                        record, 18, 7, 0) != first ||
                    ecg_reuse_plan::extractReusePlan32Second(
                        record, 18, 7, 0) != second) {
                    printf("N18 TIERLESS ROUND TRIP MISMATCH "
                           "dest=%u first=%u second=%u record=%08x\n",
                           dest, first, second, record);
                    return false;
                }
                const uint64_t wide = ecg_reuse_plan::widenReusePlan32(
                    record, 18, 7, 0);
                // A tierless record widens with tier=0; the canonical 64-bit
                // layout is unchanged.
                if (wide != ecg_reuse_plan::packReusePlanRecord(
                        dest, 0, first, second)) {
                    printf("N18 TIERLESS WIDEN MISMATCH dest=%u\n", dest);
                    return false;
                }
            }
        }
    }
    // The legacy 2-bit layout must remain bit-for-bit what it was.
    const uint32_t legacy = ecg_reuse_plan::packReusePlanRecord32(
        12345, 2, 9, 17, 16, 5);
    if (legacy != ((12345u) | (2u << 16) | (9u << 18) | (17u << 23))) {
        printf("LEGACY TIERED LAYOUT DRIFTED record=%08x\n", legacy);
        return false;
    }
    if (ecg_reuse_plan::extractReusePlan32Tier(legacy, 16) != 2 ||
        ecg_reuse_plan::extractReusePlan32First(legacy, 16, 5) != 9 ||
        ecg_reuse_plan::extractReusePlan32Second(legacy, 16, 5) != 17) {
        printf("LEGACY TIERED DECODE DRIFTED\n");
        return false;
    }
    printf("N18 128-EPOCH TIERLESS BOUNDARY OK\n");
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!checkTierWidthBoundary()) return 1;
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
    uint64_t total_csr_checked = 0;

    for (uint32_t ne : {8u, 16u, 32u, 64u}) {
        std::vector<uint64_t> off64, rec64;
        std::vector<uint64_t> off32;
        std::vector<uint32_t> rec32;
        std::vector<std::vector<uint16_t>> first_epochs;
        ecg_reuse_plan::buildInEdgeEpochs(
            g, 16, ne, true, first_epochs);
        ecg_reuse_plan::buildInEdgeReusePlanRecords(g, 16, ne, true, off64, rec64);
        if (!ecg_reuse_plan::reusePlanOffsetsMatchInCsr(
                g, off64, static_cast<uint64_t>(rec64.size()))) {
            printf("ne=%u: WIDE CSR OFFSET MISMATCH\n", ne);
            return 1;
        }
        bool ok32 = ecg_reuse_plan::buildInEdgeReusePlanRecords32(
            g, 16, ne, true, off32, rec32);
        if (!ok32) { printf("ne=%u: compact refused (expected when fields do not fit)\n", ne); continue; }
        if (!ecg_reuse_plan::reusePlanOffsetsMatchInCsr(
                g, off32, static_cast<uint64_t>(rec32.size()))) {
            printf("ne=%u: COMPACT CSR OFFSET MISMATCH\n", ne);
            return 1;
        }
        if (off64 != off32) { printf("ne=%u: OFFSET MISMATCH\n", ne); return 1; }
        if (rec64.size() != rec32.size()) { printf("ne=%u: SIZE MISMATCH\n", ne); return 1; }
        const uint32_t idb = ecg_reuse_plan::reusePlan32IdBits(n);
        const uint32_t epb = ecg_reuse_plan::reusePlan32EpochBits(ne);
        uint64_t bad = 0, checked = 0, csr_checked = 0;
        for (uint32_t u = 0; u < n; ++u) {
            uint64_t pos = off64[u];
            const uint64_t end = off64[u + 1];
            for (auto v_raw : g.in_neigh(u)) {
                const uint32_t v = static_cast<uint32_t>(v_raw);
                if (pos >= end ||
                    ecg_reuse_plan::extractReusePlanDest(rec64[pos]) != v ||
                    ecg_reuse_plan::extractReusePlan32Dest(
                        rec32[pos], idb) != v) {
                    printf("ne=%u: CSR DESTINATION MISMATCH row=%u pos=%llu\n",
                           ne, u, (unsigned long long)pos);
                    return 1;
                }
                ++pos;
                ++csr_checked;
            }
            if (pos != end) {
                printf("ne=%u: CSR ROW LENGTH MISMATCH row=%u\n", ne, u);
                return 1;
            }
        }
        if (csr_checked != rec64.size()) {
            printf("ne=%u: CSR RECORD COUNT MISMATCH\n", ne);
            return 1;
        }
        total_csr_checked += csr_checked;
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
    // Tierless records must carry exactly the same destinations and stamps as
    // the tiered ones on the SAME graph -- only the tier is dropped -- so the
    // 0-bit width is a transport change and not a different policy.
    {
        uint64_t tierless_checked = 0;
        for (uint32_t ne : {8u, 32u}) {
            std::vector<uint64_t> off64, rec64;
            std::vector<uint64_t> off0;
            std::vector<uint32_t> rec0;
            ecg_reuse_plan::buildInEdgeReusePlanRecords(
                g, 16, ne, true, off64, rec64);
            if (!ecg_reuse_plan::buildInEdgeReusePlanRecords32(
                    g, 16, ne, true, off0, rec0,
                    /*push_out_edges=*/false, /*tier_bits=*/0)) {
                printf("ne=%u: TIERLESS COMPACT RECORD REFUSED\n", ne);
                return 1;
            }
            if (off64 != off0 || rec64.size() != rec0.size()) {
                printf("ne=%u: TIERLESS OFFSET/SIZE MISMATCH\n", ne);
                return 1;
            }
            const uint32_t idb = ecg_reuse_plan::reusePlan32IdBits(n);
            const uint32_t epb = ecg_reuse_plan::reusePlan32EpochBits(ne);
            for (size_t i = 0; i < rec64.size(); ++i) {
                const uint64_t widened =
                    ecg_reuse_plan::widenReusePlan32(rec0[i], idb, epb, 0);
                if (ecg_reuse_plan::extractReusePlanDest(widened) !=
                        ecg_reuse_plan::extractReusePlanDest(rec64[i]) ||
                    ecg_reuse_plan::extractReusePlanFirst(widened) !=
                        ecg_reuse_plan::extractReusePlanFirst(rec64[i]) ||
                    ecg_reuse_plan::extractReusePlanSecond(widened) !=
                        ecg_reuse_plan::extractReusePlanSecond(rec64[i]) ||
                    ecg_reuse_plan::extractReusePlanTier(widened) != 0) {
                    printf("ne=%u: TIERLESS RECORD MISMATCH edge %zu "
                           "64b=%016llx widened=%016llx\n",
                           ne, i, (unsigned long long)rec64[i],
                           (unsigned long long)widened);
                    return 1;
                }
                ++tierless_checked;
            }
            printf("ne=%2u: %llu tierless records checked  "
                   "id_bits=%u epoch_bits=%u tier_bits=0\n",
                   ne, (unsigned long long)rec0.size(), idb, epb);
        }
        if (tierless_checked == 0) {
            printf("NO TIERLESS RECORDS CHECKED\n");
            return 1;
        }
    }
    {
        std::vector<uint64_t> out_off;
        std::vector<uint64_t> out_records;
        std::vector<uint64_t> out32_off;
        std::vector<uint32_t> out32_records;
        ecg_reuse_plan::buildInEdgeReusePlanRecords(
            g, 16, 32, true, out_off, out_records,
            /*push_out_edges=*/true);
        if (!ecg_reuse_plan::buildInEdgeReusePlanRecords32(
                g, 16, 32, true, out32_off, out32_records,
                /*push_out_edges=*/true)) {
            printf("OUT COMPACT RECORD CONSTRUCTION FAILED\n");
            return 1;
        }
        if (!ecg_reuse_plan::reusePlanOffsetsMatchCsr(
                g, out_off, static_cast<uint64_t>(out_records.size()),
                /*push_out_edges=*/true) ||
            !ecg_reuse_plan::reusePlanOffsetsMatchCsr(
                g, out32_off, static_cast<uint64_t>(out32_records.size()),
                /*push_out_edges=*/true)) {
            printf("OUT CSR OFFSET MISMATCH\n");
            return 1;
        }
        const uint32_t out_id_bits =
            ecg_reuse_plan::reusePlan32IdBits(n);
        uint64_t checked = 0;
        for (uint32_t u = 0; u < n; ++u) {
            uint64_t pos = out_off[u];
            for (auto v_raw : g.out_neigh(u)) {
                if (ecg_reuse_plan::extractReusePlanDest(
                        out_records[pos++]) !=
                        static_cast<uint32_t>(v_raw) ||
                    ecg_reuse_plan::extractReusePlan32Dest(
                        out32_records[pos - 1], out_id_bits) !=
                        static_cast<uint32_t>(v_raw)) {
                    printf("OUT CSR DESTINATION MISMATCH row=%u\n", u);
                    return 1;
                }
                ++checked;
            }
            if (pos != out_off[u + 1]) {
                printf("OUT CSR ROW LENGTH MISMATCH row=%u\n", u);
                return 1;
            }
        }
        if (checked == 0 || checked != out_records.size()) {
            printf("OUT CSR RECORD COUNT MISMATCH\n");
            return 1;
        }
    }
    if (total_csr_checked == 0) {
        printf("NO CSR DESTINATIONS CHECKED\n");
        return 1;
    }
    printf("OUT CSR OFFSETS AND DESTINATIONS MATCH\n");
    printf("IN CSR OFFSETS AND DESTINATIONS MATCH\n");
    printf("TIERLESS RECORDS EQUIVALENT\n");
    printf("ALL EQUIVALENT\n");
    return 0;
}
