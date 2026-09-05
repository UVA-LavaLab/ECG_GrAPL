#include "cache_sim/cache_sim.h"
#include "popt_rereference.h"

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* label) {
    std::printf("%-68s [%s]\n", label, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

void testSingleByteLookup() {
    using popt_reref::PostFinal;
    using popt_reref::singleEpochNextRef;
    check(singleEpochNextRef(0x45, 5, true, PostFinal::Later) == 0,
          "last subepoch equality retains current-epoch reuse");
    check(singleEpochNextRef(0x45, 6, true, PostFinal::Later) == 1,
          "expired current use with next-epoch flag returns one");
    check(singleEpochNextRef(0x05, 6, true, PostFinal::Later) == 2,
          "lower-bound reconstruction groups later uses at two");
    check(singleEpochNextRef(0x05, 6, true, PostFinal::Distant) == 63,
          "distant reconstruction groups later uses at maximum rank");
    check(singleEpochNextRef(0x05, 6, false, PostFinal::Later) == 63,
          "expired last epoch has no remaining use");
    check(singleEpochNextRef(0xc1, 0, true, PostFinal::Later) == 1 &&
          singleEpochNextRef(0x82, 0, true, PostFinal::Later) == 2 &&
          singleEpochNextRef(0xbf, 0, true, PostFinal::Later) == 63,
          "inter-epoch distance uses six bits, not next-epoch flag");

    for (uint32_t value = 0; value < 256; ++value) {
        for (uint32_t sub = 0; sub < 65; ++sub) {
            const auto entry = static_cast<uint8_t>(value);
            for (const auto postfinal : {PostFinal::Later, PostFinal::Distant}) {
                for (const bool next : {false, true}) {
                    uint32_t expected;
                    if (value >= 128) expected = value % 64;
                    else if (sub <= value % 64) expected = 0;
                    else if (!next) expected = 63;
                    else if (value >= 64) expected = 1;
                    else expected = postfinal == PostFinal::Later ? 2 : 63;
                    if (singleEpochNextRef(entry, sub, next, postfinal) != expected) {
                        check(false, "exhaustive single-byte lookup");
                        return;
                    }
                }
            }
        }
    }
    check(true, "exhaustive single-byte lookup");
}

void testNoNextColumnRead() {
    const long page_size = sysconf(_SC_PAGESIZE);
    check(page_size > 0, "host page size is available");
    if (page_size <= 0) return;
    void* pages = mmap(nullptr, 2 * page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(pages != MAP_FAILED, "allocate protected-column fixture");
    if (pages == MAP_FAILED) return;
    auto* bytes = static_cast<uint8_t*>(pages);
    const int protected_next = mprotect(bytes + page_size, page_size, PROT_NONE);
    check(protected_next == 0, "protect the nonexistent next column");
    if (protected_next == 0) {
        // A full-P-OPT next-column read crosses into the inaccessible page.
        uint8_t* current = bytes + page_size - 1;
        cache_sim::GraphCacheContext context;
        context.initRereference(current, 1, 256, 25600, 64,
                                popt_reref::Encoding::SingleEpoch);
        check(context.rereference.sub_epoch_size == 2,
              "SE registration uses 64 subepoch bins");
        *current = 0x41;
        check(context.rereference.findNextRef(0, 10) == 1,
              "next-present lookup reads only the current column");
        *current = 0x01;
        check(context.rereference.findNextRef(0, 10) == 2,
              "later lookup reads only the current column");
        context.rereference.postfinal = popt_reref::PostFinal::Distant;
        check(context.rereference.findNextRef(0, 10) == 63,
              "distant sensitivity reads only the current column");
        check(context.rereference.findNextRef(1, 10) == 63 &&
              context.rereference.findNextRef(0, 25600) == 63,
              "SE bounds use the six-bit maximum rank");
    }
    check(munmap(pages, 2 * page_size) == 0, "release protected-column fixture");
}

void testOneColumnStream() {
    cache_sim::CacheHierarchy cache(
        512, 2, 1024, 2, 2048, 2, 64, cache_sim::EvictionPolicy::LRU);
    cache.initPoptMatrixStream(64, 16, 4, 1);
    for (const uint32_t vertex : {0, 16, 0})
        cache.setCurrentVertex(vertex);
    check(cache.getPoptMatrixStreamLines() == 3,
          "single-column stream cannot retain a second epoch");
    cache.resetStats();
    cache.initPoptMatrixStream(64, 16, 4, 1);
    for (int iteration = 0; iteration < 2; ++iteration)
        for (const uint32_t vertex : {0, 16, 32, 48})
            cache.setCurrentVertex(vertex);
    check(cache.getPoptMatrixStreamLines() == 8,
          "SE streams every column on every traversal");
    cache.resetStats();
    cache.initPoptMatrixStream(64, 16, 4);
    for (const uint32_t vertex : {0, 16, 0})
        cache.setCurrentVertex(vertex);
    check(cache.getPoptMatrixStreamLines() == 2,
          "ordinary P-OPT retains its two-column residency");
}

}  // namespace

int main() {
    testSingleByteLookup();
    testNoNextColumnRead();
    testOneColumnStream();
    std::printf("POPT-SE TESTS: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
