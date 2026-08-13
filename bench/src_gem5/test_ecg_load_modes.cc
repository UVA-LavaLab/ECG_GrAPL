// Runtime gem5-DECODER test for the consolidated ecg.load (custom-0, FUNCT3=0x2).
//
// The field-parity test (test_ecg_packed_field_parity.cc) pins the layout against a C++
// MIRROR of the decoder shifts; the 3-sim verify exercises ECG eviction via the X86 m5op
// path. NEITHER runs the actual gem5-decoded ecg.load. This test does: it issues EVERY
// (mode, width) variant through the REAL RISC-V decoder and checks the decoded dest via
// rd = prop[dest] (prop[i] = i). A wrong ECG_MODE dispatch or wrong ECG_WIDTH (W) extracts
// a different dest -> rd != dest -> caught. Run under gem5 RISCV (the real decoder); the
// host build is a no-op stub (the emitters dereference the record, so it still links).
#include "gem5_sim/gem5_harness.h"
#include "ecg_mode6_builder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_fail = 0;
alignas(64) static uint64_t g_k2_record =
    ecg_epoch::packEpochPairRecord(
        0x12345678u, 2, 0x2468u, 0x6CE0u);

static constexpr uint32_t kProposalIdBits = 7;
static constexpr uint32_t kProposalEpochBits = 5;
static constexpr uint32_t kProposalDest = 37;
static constexpr uint8_t kProposalTier = 3;
static constexpr uint16_t kProposalEpoch1 = 17;
static constexpr uint16_t kProposalEpoch2 = 29;
static constexpr uint16_t kProposalCurrentEpoch = 11;
static constexpr uint16_t kProposalContext = 7;
static constexpr uint32_t kProposalValueBits = 0x41234567u;
static constexpr uint32_t kProposalCompactRecord =
    kProposalDest |
    (static_cast<uint32_t>(kProposalTier) << kProposalIdBits) |
    (static_cast<uint32_t>(kProposalEpoch1)
        << (kProposalIdBits + 2)) |
    (static_cast<uint32_t>(kProposalEpoch2)
        << (kProposalIdBits + 2 + kProposalEpochBits));
static constexpr uint64_t kProposalCanonicalRecord =
    static_cast<uint64_t>(kProposalDest) |
    (static_cast<uint64_t>(kProposalTier) << 32) |
    (static_cast<uint64_t>(kProposalEpoch1) << 34) |
    (static_cast<uint64_t>(kProposalEpoch2) << 49);

alignas(64) static const uint32_t g_proposal_compact_record =
    kProposalCompactRecord;
alignas(64) static float g_proposal_property[128] = {};
alignas(64) static uint8_t g_context_retry_lines[2048 * 64] = {};
static volatile uint64_t g_context_retry_sink = 0;

// TEETH PROOF: ECG_TEST_FORCE_WC forces the EMITTED width class (FUNCT7) to a fixed value
// while the record is still packed with the CORRECT wc. If the gem5 decoder truly reads
// ECG_WIDTH (not a hardcoded W), a forced-wrong emit must decode a DIFFERENT dest -> the
// test FAILs. Unset (the normal run) => emit the correct wc => PASS. This proves the test
// is not vacuous: the decoder's ECG_WIDTH handling is load-bearing.
static int emit_wc(int correct_wc) {
    const char* f = std::getenv("ECG_TEST_FORCE_WC");
    return f ? std::atoi(f) : correct_wc;
}

static void check(const char* mode, int wc, uint32_t dest, uint32_t rd) {
    bool ok = (rd == dest);
    printf("[test_ecg_load_modes] %-10s wc=%2d dest=%-10u rd=%-10u [%s]\n",
           mode, wc, dest, rd, ok ? "OK" : "FAIL");
    if (!ok) g_fail++;
}

static bool write_proposal_context() {
    const char* path = gem5_context_path();
    FILE* f = std::fopen(path, "w");
    if (!f) {
        std::printf(
            "[test_ecg_load_modes] proposal context write failed: %s\n",
            path);
        return false;
    }
    std::fprintf(
        f,
        "{\n"
        "  \"num_vertices\": 128,\n"
        "  \"num_edges\": 1,\n"
        "  \"edge_epoch_count\": 32,\n"
        "  \"stream_bypass_base\": %llu,\n"
        "  \"stream_bypass_size\": %zu,\n"
        "  \"property_regions\": [\n"
        "    {\"name\": \"proposal_property\", \"base\": %llu, "
        "\"size\": %zu, \"count\": 128, \"elem_size\": 4, "
        "\"grasp\": true}\n"
        "  ]\n"
        "}\n",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(&g_proposal_compact_record)),
        sizeof(g_proposal_compact_record),
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(g_proposal_property)),
        sizeof(g_proposal_property));
    std::fclose(f);
    return true;
}

static void run_proposal_probe(
        bool force_context_retry, bool wrong_record_format = false) {
    float proposal_value = 0.0f;
    std::memcpy(
        &proposal_value, &kProposalValueBits, sizeof(proposal_value));
    g_proposal_property[kProposalDest] = proposal_value;

    if (force_context_retry) {
        volatile uint8_t* lines = g_context_retry_lines;
        uint64_t sink = 0;
        for (size_t offset = 0; offset < sizeof(g_context_retry_lines);
             offset += 64) {
            lines[offset] = static_cast<uint8_t>(offset / 64);
            sink += lines[offset];
        }
        g_context_retry_sink = sink;
    }

    gem5_ecg_write_record_format_csr(
        wrong_record_format ? kProposalIdBits - 2 : kProposalIdBits,
        kProposalEpochBits);
    gem5_ecg_write_current_epoch_csr(kProposalCurrentEpoch);
    gem5_ecg_write_context_csr(kProposalContext);

    const uint64_t canonical =
        gem5_ecg_stream_load2_compact_instruction(
            &g_proposal_compact_record,
            kProposalIdBits, kProposalEpochBits);
    const float value = gem5_ecg_mload_k2_f32(
        &g_proposal_property[kProposalDest], canonical);
    uint32_t value_bits = 0;
    std::memcpy(&value_bits, &value, sizeof(value_bits));
    const bool ok =
        canonical == kProposalCanonicalRecord &&
        value_bits == kProposalValueBits;
    std::printf(
        "[test_ecg_load_modes] K2-C-SS-MLOAD compact=%#x "
        "canonical=%#llx dest=%u tier=%u epoch1=%u epoch2=%u "
        "current=%u context=%u value_bits=%#x [%s]\n",
        g_proposal_compact_record,
        static_cast<unsigned long long>(canonical),
        kProposalDest, static_cast<unsigned>(kProposalTier),
        static_cast<unsigned>(kProposalEpoch1),
        static_cast<unsigned>(kProposalEpoch2),
        static_cast<unsigned>(kProposalCurrentEpoch),
        static_cast<unsigned>(kProposalContext),
        value_bits, ok ? "OK" : "FAIL");
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    const bool proposal_only =
        argc > 1 && std::strcmp(argv[1], "proposal-only") == 0;
    const bool proposal_wrong_format =
        argc > 1 &&
        std::strcmp(argv[1], "proposal-wrong-format") == 0;
    if (proposal_only || proposal_wrong_format) {
        if (!write_proposal_context()) g_fail++;
        run_proposal_probe(true, proposal_wrong_format);
        std::printf("[test_ecg_load_modes] RESULT: %s (%d fail)\n",
                    g_fail ? "FAIL" : "PASS", g_fail);
        return g_fail ? 1 : 0;
    }

    // 4M-entry property array (16 MB). prop[dest] = dest, so a correctly decoded dest
    // returns rd == dest; a wrong width/mode lands on a different (unwritten => 0) index.
    const size_t N = (size_t)4u << 20;
    uint32_t* prop = static_cast<uint32_t*>(std::calloc(N, sizeof(uint32_t)));
    if (!prop) { printf("[test_ecg_load_modes] calloc failed\n"); return 2; }

    // dest values: valid for the width class AND large enough that a too-small W would
    // mask off high bits to a DIFFERENT (in-array) index -> clean mismatch.
    struct { int wc; uint32_t dest; uint16_t epoch; } vec[] = {
        {0, 0x000000FEu, 0xBEEF},  // W8  (dest < 256;   wrong W16 -> 0xEFFE)
        {1, 0x0000BEEFu, 0x000D},  // W16 (dest < 65536; wrong W8 -> 0xEF,  wrong W24 -> 0x0DBEEF)
        {2, 0x003ABCDEu, 0x000D},  // W24 (dest < 16.7M; wrong W16 -> 0xBCDE)
        {3, 0x003ABCDEu, 0x000D},  // W32 (representable; full 32-bit range pinned by field-parity)
    };

    for (auto& c : vec) {
        prop[c.dest] = c.dest;
        uint64_t rec = ecg_mode6::packEvict(c.dest, c.epoch, c.wc);
        check("EVICT", c.wc, c.dest, gem5_ecg_load_evict(prop, rec, emit_wc(c.wc)));
    }
    for (auto& c : vec) {
        prop[c.dest] = c.dest;
        uint64_t rec = ecg_mode6::packEvictPfx(c.dest, c.epoch, 0x5A5Au, c.wc);
        check("EVICT+PFX", c.wc, c.dest, gem5_ecg_load_pfx(prop, rec, emit_wc(c.wc)));
    }
    {
        // EMBEDDED (mode 2): NARROW packMaskEpoch, fixed 24-bit dest + dbg/popt/pfx.
        uint32_t dest = 0x003ABCDEu;
        prop[dest] = dest;
        uint64_t rec = ecg_mode6::packMaskEpoch(dest, 2, 0x5A, 0x1234, 0x7F);
        check("EMBEDDED", 24, dest, gem5_ecg_load_embedded(prop, rec));
    }
    {
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            0x12345678u, 2, 0x2468u, 0x6CE0u);
        uint64_t rd_stream = gem5_ecg_stream_load2_instruction(&g_k2_record);
        uint64_t rd = gem5_ecg_load2_instruction(&g_k2_record);
        bool ok = rd == record && rd_stream == record;
        printf("[test_ecg_load_modes] LOAD2/K2 record=%#llx rd=%#llx "
               "stream=%#llx [%s]\n",
               (unsigned long long)record, (unsigned long long)rd,
               (unsigned long long)rd_stream,
               ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    run_proposal_probe(false);
    {
        const uint32_t dest = 0x003ABCDEu;
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            dest, 2, 0x2468u, 0x6CE0u);
        prop[dest] = dest;
        check("K2-PLOAD", 32, dest, gem5_ecg_load_k2(prop, record));
        check("K2-M-U32", 32, dest,
              gem5_ecg_mload_k2_u32(&prop[dest], record));
    }
    {
        uint32_t unsigned_prop[16] = {};
        const uint32_t dest = 9u;
        const uint32_t value = 0xF1234567u;
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            dest, 2, 0x0102u, 0x0304u);
        unsigned_prop[dest] = value;
        const uint32_t rd =
            gem5_ecg_mload_k2_u32(&unsigned_prop[dest], record);
        const bool ok = rd == value;
        printf("[test_ecg_load_modes] K2-M-U32-HIGH dest=%u rd=%#x [%s]\n",
               dest, rd, ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    {
        int32_t signed_prop[16] = {};
        const uint32_t dest = 7u;
        const int32_t value = -1234567;
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            dest, 3, 0x1234u, 0x2345u);
        signed_prop[dest] = value;
        const int32_t rd =
            gem5_ecg_mload_k2_s32(&signed_prop[dest], record);
        const bool ok = rd == value;
        printf("[test_ecg_load_modes] K2-M-S32 dest=%u rd=%d [%s]\n",
               dest, rd, ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    {
        float float_prop[16] = {};
        const uint32_t dest = 5u;
        const uint32_t value_bits = 0x7FC12345u;
        float value;
        std::memcpy(&value, &value_bits, sizeof(value));
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            dest, 1, 0x1111u, 0x2222u);
        float_prop[dest] = value;
        const float rd =
            gem5_ecg_mload_k2_f32(&float_prop[dest], record);
        uint32_t rd_bits = 0;
        std::memcpy(&rd_bits, &rd, sizeof(rd_bits));
        const bool ok = rd_bits == value_bits;
        printf("[test_ecg_load_modes] K2-M-F32 dest=%u bits=%#x [%s]\n",
               dest, rd_bits, ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    {
        uint64_t prop64[1024] = {};
        const uint32_t dest = 777u;
        const uint64_t value = 0x123456789ABCDEF0ULL;
        const uint64_t record = ecg_epoch::packEpochPairRecord(
            dest, 2, 0x1357u, 0x2468u);
        prop64[dest] = value;
        const uint64_t rd = gem5_ecg_load_k2_u64(prop64, record);
        const uint64_t mrd =
            gem5_ecg_mload_k2_u64(&prop64[dest], record);
        const bool ok = rd == value && mrd == value;
        printf("[test_ecg_load_modes] K2-PLOAD64 dest=%u rd=%#llx "
               "mrd=%#llx [%s]\n",
               dest, (unsigned long long)rd, (unsigned long long)mrd,
               ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    {
        uint32_t dist[1024] = {};
        const uint32_t dest = 511u;
        const uint32_t value = 0x76543210u;
        const uint64_t record =
            ecg_epoch::packCompactWeightedEpochPairRecord(
                dest, 255u, 2u, 0x1234u, 0x5678u);
        dist[dest] = value;
        const uint32_t rd =
            gem5_ecg_load_k2_weighted64(dist, record);
        const uint32_t mrd =
            gem5_ecg_mload_k2_compact_u32(&dist[dest], record);
        const bool ok =
            rd == value && mrd == value &&
            ecg_epoch::extractCompactWeightedWeight(record) == 255;
        printf("[test_ecg_load_modes] K2-WEIGHTED64 dest=%u weight=%d "
               "rd=%#x mrd=%#x [%s]\n",
               dest, ecg_epoch::extractCompactWeightedWeight(record),
               rd, mrd, ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
    }
    {
        const uint32_t dest = 0x00300001u;
        const uint32_t sidecar =
            ecg_epoch::packWeightedEpochPairSidecar(1u, 321u, 654u);
        const uint32_t rd_stream =
            gem5_ecg_stream_weighted_load2_instruction(&sidecar);
        const uint32_t rd =
            gem5_ecg_weighted_load2_instruction(&sidecar, dest);
        const uint64_t combined =
            ecg_epoch::combineWeightedEpochPairRecord(dest, sidecar);
        const uint64_t canonical = ecg_epoch::packEpochPairRecord(
            dest, 1u, 321u, 654u);
        const bool ok =
            rd == sidecar && rd_stream == sidecar && combined == canonical;
        printf("[test_ecg_load_modes] WLOAD2/K2 sidecar=%#x rd=%#x "
               "stream=%#x [%s]\n",
               sidecar, rd, rd_stream, ok ? "OK" : "FAIL");
        if (!ok) g_fail++;
        prop[dest] = dest;
        check("K2-WPLOAD", 32, dest, gem5_ecg_load_k2(
            prop, ecg_epoch::combineWeightedEpochPairRecord(dest, sidecar)));
    }

    printf("[test_ecg_load_modes] RESULT: %s (%d fail)\n",
           g_fail ? "FAIL" : "PASS", g_fail);
    std::free(prop);
    return g_fail ? 1 : 0;
}
