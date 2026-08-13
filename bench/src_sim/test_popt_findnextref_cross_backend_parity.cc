// Real, executable cross-backend PageRank P-OPT findNextRef parity test.
//
// cache_sim's ecg_policy::RereferenceConfig::findNextRef
// (bench/include/cache_sim/graph_cache_context.h), gem5's
// RereferenceMatrix::findNextRef (bench/include/gem5_sim/overlays/mem/cache/
// replacement_policies/graph_cache_context_gem5.hh) and Sniper's
// RereferenceMatrix::findNextRef (bench/include/sniper_sim/overlays/common/
// core/memory_subsystem/cache/graph_cache_context_sniper.{h,cc}) are THREE
// INDEPENDENTLY MAINTAINED implementations of the P-OPT Algorithm 2
// next-reference lookup used by PageRank's rereference matrix (see
// bench/src_sim/pr.cc's buildAndRegisterReref() call and
// bench/include/graphbrew/partition/cagra/popt.h's encoder for the shared
// 8-bit entry convention: MSB=0 -> referenced this epoch, low 7 bits = last
// sub-epoch; MSB=1 -> not referenced, low 7 bits = distance in epochs to the
// next reference). Unlike ecg_victim_policy.h, this lookup is maintained in
// three backend-specific copies, so a
// silent behavioral drift between the three copies would go undetected by
// every other existing test in this repo.
//
// This program does NOT rely on source-string assertions: it links real
// object code compiled from each backend's actual header/source file
// (via three tiny extern "C" adapters -- popt_findnextref_adapter_cache_sim
// .cc / _gem5.cc / _sniper.cc -- one per backend, each built against that
// backend's own include path/namespace to avoid any symbol collision
// between the three independent copies) and calls all three with the same
// encoded rereference-matrix bytes and the SAME (cache-line-id,
// current-vertex) query for every test case below. It also carries an
// independent Python-free, hand-computed expected value per case (derived
// directly from the documented Algorithm 2 encoding, not from any of the
// three implementations) so a bug shared by all three copies (not just a
// divergence between them) is also caught. Build/run:
//
//   g++ -std=c++17 -O2 -I bench/include -I bench/include/cache_sim \
//       -I bench/include/gem5_sim/overlays \
//       -I bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache \
//       bench/src_sim/popt_findnextref_adapter_cache_sim.cc \
//       bench/src_sim/popt_findnextref_adapter_gem5.cc \
//       bench/src_sim/popt_findnextref_adapter_sniper.cc \
//       bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.cc \
//       bench/src_sim/test_popt_findnextref_cross_backend_parity.cc \
//       -o bench/bin_sim/test_popt_findnextref_cross_backend_parity
// (also reachable via the scripts/test/ pytest wrapper, which compiles this
// exact command fresh rather than relying on a pre-built binary).

#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" uint32_t cache_sim_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex);
extern "C" uint32_t gem5_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex);
extern "C" uint32_t sniper_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex);

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char *what, bool ok) {
    printf("    %-72s [%s]\n", what, ok ? "OK" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// A PageRank-representative encoded rereference matrix: 4 cache lines x
// 4 epochs (epoch_size=100 vertices, sub_epoch_size=25 -> 4 sub-epochs per
// epoch), using the SAME MSB=0/MSB=1 Algorithm-2 encoding real PR runs
// produce via buildAndRegisterReref()/the P-OPT artifact encoder. Row-major
// as matrix[epoch_id * num_cache_lines + cline_id], matching all three
// findNextRef signatures.
constexpr uint32_t kNumCacheLines = 4;
constexpr uint32_t kNumEpochs = 4;
constexpr uint32_t kEpochSize = 100;
constexpr uint32_t kSubEpochSize = 25;

// clang-format off
const uint8_t kMatrix[kNumEpochs * kNumCacheLines] = {
    // epoch0:  cline0  cline1  cline2  cline3
                0x02,   0x81,   0x03,   0x00,
    // epoch1
                0x81,   0x01,   0x85,   0x02,
    // epoch2
                0x00,   0x00,   0x00,   0x00,
    // epoch3 (last epoch)
                0x00,   0x7F,   0x00,   0x81,
};
// clang-format on

struct Case {
    const char *label;
    uint32_t cline_id;
    uint32_t current_vertex;
    uint32_t expected;  // hand-derived from the documented Algorithm 2 rules
};

const Case kCases[] = {
    {"referenced this epoch, still upcoming (curr_sub <= last_sub)",
     0, 5, 0},
    {"referenced this epoch, past last ref; next epoch not-referenced "
     "dist=1 -> +1", 0, 90, 2},
    {"not referenced this epoch, direct distance decode", 1, 10, 1},
    {"referenced this epoch, curr_sub == last_sub boundary", 2, 99, 0},
    {"not referenced this epoch, larger direct distance decode",
     2, 199, 5},
    {"referenced this epoch, curr_sub == last_sub == 0", 3, 10, 0},
    {"referenced this epoch, past last ref; next epoch IS referenced -> 1",
     3, 60, 1},
    {"not-referenced-this-epoch case reached via referenced-then-crossed "
     "epoch boundary", 0, 250, 1},
    {"last epoch, still upcoming (curr_sub <= large last_sub 127)",
     1, 350, 0},
    {"last epoch, not referenced, direct distance decode", 3, 320, 1},
    {"current_vertex maps to epoch_id >= num_epochs -> saturate 127",
     0, 405, 127},
    {"cline_id >= num_cache_lines -> saturate 127", 9, 10, 127},
};

}  // namespace

int main() {
    printf("== Cross-backend PageRank P-OPT findNextRef parity ==\n");

    for (const Case &c : kCases) {
        const uint32_t cache_sim_result = cache_sim_find_next_ref(
                kMatrix, kNumCacheLines, kNumEpochs, kEpochSize,
                kSubEpochSize, c.cline_id, c.current_vertex);
        const uint32_t gem5_result = gem5_find_next_ref(
                kMatrix, kNumCacheLines, kNumEpochs, kEpochSize,
                kSubEpochSize, c.cline_id, c.current_vertex);
        const uint32_t sniper_result = sniper_find_next_ref(
                kMatrix, kNumCacheLines, kNumEpochs, kEpochSize,
                kSubEpochSize, c.cline_id, c.current_vertex);

        char label[256];
        snprintf(label, sizeof(label),
                 "%s (cline=%u, vtx=%u)", c.label, c.cline_id,
                 c.current_vertex);

        char sub_label[300];
        snprintf(sub_label, sizeof(sub_label),
                 "%s: cache_sim==gem5==Sniper==expected(%u)", label,
                 c.expected);
        check(sub_label,
              cache_sim_result == c.expected &&
              gem5_result == c.expected &&
              sniper_result == c.expected);

        if (cache_sim_result != c.expected || gem5_result != c.expected ||
            sniper_result != c.expected) {
            printf("        cache_sim=%u gem5=%u sniper=%u expected=%u\n",
                   cache_sim_result, gem5_result, sniper_result, c.expected);
        }
    }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
