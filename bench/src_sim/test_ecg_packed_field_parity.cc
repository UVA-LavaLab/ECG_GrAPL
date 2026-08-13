// Packed-ISA field parity test across all simulator backends.
//
// Two delivery layouts share ecg_mode6_builder.h:
//   - packMask          : cache_sim/Sniper "fat" mask, 31-bit prefetch target at bits [33:64]
//                         (extractPrefetchTarget, mask 0x7FFFFFFF).
//   - packMaskEpoch     : gem5 ISA mask, epoch at [33:49] + 15-bit prefetch target at [49:64]
//                         (extractPrefetchTargetEpoch, mask 0x7FFF).
//
// This pins the field layout against silent repack/shift regressions and PROVES the
// documented divergence: the 31-bit and 15-bit targets AGREE for targets <= 32767
// (in-range parity), but packMaskEpoch TRUNCATES targets > 32767 to a WRONG vertex while
// packMask preserves them. verify/pfx.py proves target selection parity; this
// test proves field delivery parity.
#include "ecg_mode6_builder.h"
#include "ecg_epoch_builder.h"
#include <cstdio>
#include <cstdint>

using namespace ecg_mode6;

static int g_pass = 0, g_fail = 0;
static void check(const char* n, uint64_t got, uint64_t exp) {
    bool ok = (got == exp);
    printf("    %-54s got=%-10llu expect=%-10llu [%s]\n", n,
           (unsigned long long)got, (unsigned long long)exp, ok ? "OK" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

int main() {
    printf("[test_ecg_packed_field_parity] packMask (31-bit) vs packMaskEpoch (15-bit) target\n");

    // (1) Non-target fields round-trip in BOTH layouts.
    {
        uint32_t dest = 0x123456; uint8_t dbg = 2; uint8_t popt = 0x5A; uint16_t epoch = 0xABCD;
        uint64_t m1 = packMask(dest, dbg, popt, 12345);
        check("packMask dest", extractDest(m1), dest);
        check("packMask dbg", extractDbg(m1), dbg);
        check("packMask popt", extractPopt(m1), popt & 0x7F);
        uint64_t m2 = packMaskEpoch(dest, dbg, popt, epoch, 12345);
        check("packMaskEpoch dest", extractDest(m2), dest);
        check("packMaskEpoch dbg", extractDbg(m2), dbg);
        check("packMaskEpoch popt", extractPopt(m2), popt & 0x7F);
        check("packMaskEpoch epoch", extractEpoch(m2), epoch);
    }

    // (2) In-range parity: for targets <= 32767, both layouts deliver the SAME target.
    for (uint32_t t : {0u, 1u, 100u, 1000u, 16384u, 32766u, 32767u}) {
        uint64_t m1 = packMask(7, 0, 0, t);
        uint64_t m2 = packMaskEpoch(7, 0, 0, 0, t);
        char nm[80];
        snprintf(nm, sizeof nm, "in-range t=%u: packMask 31-bit", t);
        check(nm, extractPrefetchTarget(m1), t);
        snprintf(nm, sizeof nm, "in-range t=%u: packMaskEpoch 15-bit", t);
        check(nm, extractPrefetchTargetEpoch(m2), t);
        snprintf(nm, sizeof nm, "in-range t=%u: 31-bit == 15-bit (PARITY)", t);
        check(nm, extractPrefetchTarget(m1), extractPrefetchTargetEpoch(m2));
    }

    // (3) Out-of-range (the gem5 limitation): targets > 32767 survive in packMask (31-bit)
    //     but TRUNCATE in packMaskEpoch (15-bit) -> the 15-bit field is a WRONG vertex.
    for (uint32_t t : {32768u, 65535u, 916428u, 0x7FFFFFFFu}) {
        uint64_t m1 = packMask(7, 0, 0, t);
        uint64_t m2 = packMaskEpoch(7, 0, 0, 0, t);
        char nm[96];
        snprintf(nm, sizeof nm, "out-of-range t=%u: packMask 31-bit preserves", t);
        check(nm, extractPrefetchTarget(m1), t & 0x7FFFFFFFu);
        snprintf(nm, sizeof nm, "out-of-range t=%u: packMaskEpoch truncates to t&0x7FFF", t);
        check(nm, extractPrefetchTargetEpoch(m2), t & 0x7FFFu);
        snprintf(nm, sizeof nm, "out-of-range t=%u: 15-bit field is WRONG (!= true target)", t);
        check(nm, (extractPrefetchTargetEpoch(m2) != t) ? 1u : 0u, 1u);
    }

    // (4) Truncation boundary: 32767 OK, 32768 -> 0 (silent wrong vertex).
    check("boundary 32767 ok in 15-bit",
          extractPrefetchTargetEpoch(packMaskEpoch(0, 0, 0, 0, 32767)), 32767);
    check("boundary 32768 -> 0 in 15-bit (truncated)",
          extractPrefetchTargetEpoch(packMaskEpoch(0, 0, 0, 0, 32768)), 0);

    // (5) Wide layout: packMaskEpochWide reclaims the
    //     vestigial dbg+popt fields to carry a 24-bit prefetch target -> covers ids
    //     <= 16,777,215 (all evaluated graphs), fixing what the 15-bit field truncated.
    {
        printf("  -- packMaskEpochWide (24-bit target) --\n");
        uint32_t dest = 0xABCDEF; uint16_t epoch = 0x1234; uint32_t pfx = 916428; // web-Google id
        uint64_t w = packMaskEpochWide(dest, epoch, pfx);
        check("wide dest round-trip", extractDest(w), dest);
        check("wide epoch round-trip", extractEpochWide(w), epoch);
        check("wide pfx web-Google 916428 SURVIVES (15-bit truncated it)",
              extractPrefetchTargetWide(w), pfx);
        check("wide pfx boundary 16777215 (2^24-1) ok",
              extractPrefetchTargetWide(packMaskEpochWide(0, 0, 16777215u)), 16777215u);
        check("wide pfx 16777216 (2^24) truncates to 0",
              extractPrefetchTargetWide(packMaskEpochWide(0, 0, 16777216u)), 0u);
        // Cross-check: the same target that the 15-bit packMaskEpoch mangled (916428 ->
        // 31692) is preserved by the wide layout.
        check("wide preserves what 15-bit mangled (916428 != 31692)",
              (extractPrefetchTargetWide(packMaskEpochWide(0, 0, 916428u)) == 916428u) ? 1u : 0u, 1u);

        // ISA drift guard: decoder_ecg_extract.isa hand-codes the
        // wide-layout shifts (dest>>0, epoch>>24, pfx>>40) rather than calling these builder
        // extractors. Pin that the hand-coded ISA shifts match the shared builder, so a change to
        // the wide layout (kEpochWideShift/kPrefetchWideShift) that forgets to update the .isa
        // is caught HERE (fast unit test) instead of silently mis-decoding inside gem5.
        {
            uint64_t isa_dest  = (w >>  0) & 0xFFFFFFULL;  // MUST mirror decoder_ecg_extract.isa
            uint64_t isa_epoch = (w >> 24) & 0xFFFFULL;    // MUST mirror decoder_ecg_extract.isa
            uint64_t isa_pfx   = (w >> 40) & 0xFFFFFFULL;  // MUST mirror decoder_ecg_extract.isa
            check("ISA drift: hand-coded dest>>0  == builder extractDest", isa_dest, extractDest(w));
            check("ISA drift: hand-coded epoch>>24 == builder extractEpochWide", isa_epoch, extractEpochWide(w));
            check("ISA drift: hand-coded pfx>>40  == builder extractPrefetchTargetWide", isa_pfx, extractPrefetchTargetWide(w));
        }
    }

    // (6) Epoch-only layout: no prefetch-target field because Path A
    //     reads ahead in the CSR (= DROPLET), so nothing is stored for prefetch.
    //     The reclaimed bits widen the epoch (34 bits) so it never starves as ids
    //     grow, and the dest covers 268M verts (twitter/friendster/kron-s27).
    {
        printf("  -- packMaskEpochOnly (no prefetch field, 34-bit epoch) --\n");
        uint32_t dest = 0x0ABCDEF;          // 28-bit dest
        uint64_t epoch = 0x3FFFFFFFFULL;    // 34-bit all-ones
        uint64_t e = packMaskEpochOnly(dest, 2, epoch);
        check("epoch-only dest round-trip", extractDestEpochOnly(e), dest);
        check("epoch-only dbg round-trip", extractDbgEpochOnly(e), 2);
        check("epoch-only epoch round-trip (34-bit)", extractEpochOnly(e), epoch);
        // Scale: dest covers the evaluated comparison graphs.
        check("epoch-only dest twitter 41.7M ok",
              extractDestEpochOnly(packMaskEpochOnly(41700000u, 0, 0)), 41700000u);
        check("epoch-only dest friendster 65.6M ok",
              extractDestEpochOnly(packMaskEpochOnly(65600000u, 0, 0)), 65600000u);
        check("epoch-only dest kron-s27 134M ok",
              extractDestEpochOnly(packMaskEpochOnly(134217727u, 0, 0)), 134217727u);
        check("epoch-only dest boundary 2^28-1 ok",
              extractDestEpochOnly(packMaskEpochOnly(268435455u, 0, 0)), 268435455u);
        // Precision: the epoch carries values a 16-bit field would truncate.
        check("epoch-only epoch 100000 (>16-bit) survives (16-bit would truncate)",
              extractEpochOnly(packMaskEpochOnly(7, 0, 100000u)), 100000u);
        check("epoch-only epoch 2^20 survives",
              extractEpochOnly(packMaskEpochOnly(7, 0, 1048576u)), 1048576u);
        // No prefetch field: a large value in the old prefetch position is now
        // part of the EPOCH, not a separate target — dest stays clean.
        check("epoch-only: high bits are epoch (dest unpolluted)",
              extractDestEpochOnly(packMaskEpochOnly(7, 0, 916428u)), 7u);
    }

    // (7) CONFIGURABLE-WIDTH EVICT layout (the consolidated ecg.load): dest width
    //     W = 8/16/24/32 per width-class wc in {0,1,2,3}, epoch at [W:W+16], pfx at
    //     [W+16:64]. Pins round-trip at every width AND the gem5 decoder DRIFT GUARD:
    //     the ecg.load ea_code hand-codes W = 8*(wc+1), dest = m & dmask,
    //     epoch = (m>>W)&0xFFFF, pfx = (m>>(W+16))&0xFFFFFF — assert that equals the
    //     builder helpers, so a layout change that forgets the .isa fails HERE.
    {
        printf("  -- configurable-width EVICT (W=8/16/24/32) + ISA drift guard --\n");
        struct { int wc; uint32_t dest; const char* lbl; } cases[] = {
            {0, 200u,        "wc0 W8  (<=255)"},
            {1, 60000u,      "wc1 W16 (<=65535)"},
            {2, 12345678u,   "wc2 W24 (<=16.7M)"},
            {3, 3000000000u, "wc3 W32 (<=4.29B)"},
        };
        for (auto& c : cases) {
            uint16_t epoch = 0xBEEF; uint32_t pfx = 0xABCD;
            uint64_t m  = packEvict(c.dest, epoch, c.wc);
            uint64_t mp = packEvictPfx(c.dest, epoch, pfx, c.wc);
            char nm[96];
            snprintf(nm, sizeof nm, "%s dest round-trip", c.lbl);
            check(nm, extractEvictDest(m, c.wc), c.dest);
            snprintf(nm, sizeof nm, "%s epoch round-trip", c.lbl);
            check(nm, extractEvictEpoch(m, c.wc), epoch);
            snprintf(nm, sizeof nm, "%s +pfx dest/epoch/pfx round-trip", c.lbl);
            check(nm, (extractEvictDest(mp, c.wc) == c.dest &&
                       extractEvictEpoch(mp, c.wc) == epoch &&
                       extractEvictPfxTarget(mp, c.wc) == pfx) ? 1u : 0u, 1u);
            // ISA DRIFT GUARD: hand-coded decoder ea_code logic == builder helpers.
            unsigned W = 8u * (c.wc + 1);
            uint64_t dmask = (W >= 32) ? 0xFFFFFFFFULL : ((1ULL << W) - 1);
            uint64_t isa_dest  = mp & dmask;
            uint64_t isa_epoch = (mp >> W) & 0xFFFFULL;
            uint64_t isa_pfx   = (mp >> (W + 16)) & 0xFFFFFFULL;
            snprintf(nm, sizeof nm, "%s ISA W=8*(wc+1)", c.lbl);
            check(nm, W, (unsigned)ecgEvictWidthBits(c.wc));
            snprintf(nm, sizeof nm, "%s ISA dest>>0  == builder", c.lbl);
            check(nm, isa_dest, extractEvictDest(mp, c.wc));
            snprintf(nm, sizeof nm, "%s ISA epoch>>W == builder", c.lbl);
            check(nm, isa_epoch, extractEvictEpoch(mp, c.wc));
            snprintf(nm, sizeof nm, "%s ISA pfx>>(W+16) == builder", c.lbl);
            check(nm, isa_pfx, extractEvictPfxTarget(mp, c.wc));
        }
        // wc=2 (W=24) EVICT matches the existing 24-bit wide path.
        {
            uint32_t dest = 0xABCDEF; uint16_t epoch = 0x1234;
            check("wc2 EVICT dest == packMaskEpochWide dest",
                  extractEvictDest(packEvict(dest, epoch, 2), 2),
                  extractDest(packMaskEpochWide(dest, epoch, 0)));
            check("wc2 EVICT epoch == packMaskEpochWide epoch",
                  extractEvictEpoch(packEvict(dest, epoch, 2), 2),
                  extractEpochWide(packMaskEpochWide(dest, epoch, 0)));
        }
        // Width-class selection by graph size.
        check("ecgEvictWidthClass(256) -> wc0", (unsigned)ecgEvictWidthClass(256), 0u);
        check("ecgEvictWidthClass(65536) -> wc1", (unsigned)ecgEvictWidthClass(65536), 1u);
        check("ecgEvictWidthClass(16.7M) -> wc2", (unsigned)ecgEvictWidthClass(16777216ULL), 2u);
        check("ecgEvictWidthClass(100M) -> wc3", (unsigned)ecgEvictWidthClass(100000000ULL), 3u);
    }

    // (8) Tiered Schedule-2 delivery layout shared by gem5 ecg.extract2 and
    // Sniper SimMagic2:
    // dest[0:32] | tier[32:34] | epoch1[34:49] | epoch2[49:64].
    {
        printf("  -- Schedule-2 dest32+tier2+epoch15+epoch15 layout --\n");
        const uint32_t dest = 0x89ABCDEFu;
        const uint8_t tier = 2;
        const uint16_t first = 0x1234u;
        const uint16_t second = 0x7EDCu;
        const uint64_t record =
            ecg_epoch::packEpochPairRecord(dest, tier, first, second);
        check("K2 dest round-trip",
              ecg_epoch::extractEpochPairDest(record), dest);
        check("K2 tier round-trip",
              ecg_epoch::extractEpochPairTier(record), tier);
        check("K2 epoch1 round-trip",
              ecg_epoch::extractEpochPairFirst(record), first);
        check("K2 epoch2 round-trip",
              ecg_epoch::extractEpochPairSecond(record), second);
        // ISA/magic-handler drift guard: these shifts are hand-coded in the
        // gem5 RISC-V decoder and both simulator magic handlers.
        check("K2 decoder drift: dest>>0",
              record & 0xFFFFFFFFULL, dest);
        check("K2 decoder drift: tier>>32",
              (record >> 32) & 0x3ULL, tier);
        check("K2 decoder drift: epoch1>>34",
              (record >> 34) & 0x7FFFULL, first);
        check("K2 decoder drift: epoch2>>49",
              (record >> 49) & 0x7FFFULL, second);
        check("K2 epoch-count clamp",
              ecg_epoch::normalizeK2EpochCount(65535),
              ecg_epoch::kK2MaxEpochCount);
    }

    printf("  RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
