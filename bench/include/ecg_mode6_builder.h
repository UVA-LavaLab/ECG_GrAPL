// ============================================================================
// ECG Mode 6: Per-Edge Mask Builder (shared across cache_sim/gem5/Sniper)
// ============================================================================
//
// Per-edge prefetch metadata: each edge in the CSR carries
// a packed mask encoding (dest_id | DBG_tier | POPT_quant | prefetch_target).
// The prefetch_target is selected from src's iteration context as the
// "next-K POPT-best dest" in src's in_neighbors after the current one.
//
// This header is included by:
//   * bench/src_sim/{pr,bfs,sssp}.cc   (cache_sim mode 6 path)
//   * bench/src_gem5/{pr,bfs,sssp}.cc  (gem5 cycle-accurate mode 6 path)
//   * bench/src_sniper/{pr,bfs,sssp}.cc (Sniper cycle-accurate mode 6 path)
//
// Caller responsibilities:
//   1) Build POPT offset matrix via popt.h::makeOffsetMatrix and pass it in
//      as `popt_matrix`.
//   2) Compute the tier array (degree-bucketed) and pass it as `tiers`,
//      or pass empty vector to skip DBG encoding.
//   3) Provide k_lookahead (typical default: 8). 0 disables prefetch encoding.
//
// Output:
//   `out_masks[src][edge_pos]` is a 64-bit mask:
//     [0:24]  dest_id (24 bits)
//     [24:26] DBG tier (2 bits)
//     [26:33] POPT quant (7 bits, 0-127)
//     [33:64] prefetch target (31 bits, 0 = no prefetch)
//   Epoch-carrying masks use:
//     [33:49] next-ref epoch (16 bits)
//     [49:64] prefetch target (15 bits, 0 = no prefetch)
//
// LOAD-BEARING note: the
// ECG_GRASP_POPT primary eviction reads only the epoch (the 7-bit POPT-quant field
// is vestigial there; it survives for legacy cache_sim modes + gem5 decode). The
// prefetch target is read only by ECG_PFX. DIRECTION: the next-ref matrix must be the
// graph TRANSPOSE of the kernel's property-access edge list (P-OPT's thesis) — correct
// for PageRank's in-pull (out_neigh matrix); out-traversal kernels (SSSP/BC/BFS-TD)
// need traverseCSR=false on directed graphs.
//
// For gem5/Sniper kernels that only need the prefetch target, use the
// convenience helper `extractPrefetchTarget(mask)`.

#ifndef ECG_MODE6_BUILDER_H
#define ECG_MODE6_BUILDER_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ecg_mode6 {

// === Mask field accessors (constexpr inline for free encoding/decoding) ===

constexpr int kDestBits     = 24;
constexpr int kDbgBits      = 2;
constexpr int kPoptBits     = 7;
constexpr int kPrefetchBits = 31;
constexpr int kEpochBits    = 16;
constexpr int kPrefetchEpochBits = 15;
constexpr int kDestShift     = 0;
constexpr int kDbgShift      = kDestBits;
constexpr int kPoptShift     = kDestBits + kDbgBits;
constexpr int kPrefetchShift = kDestBits + kDbgBits + kPoptBits;
constexpr int kEpochShift    = kPrefetchShift;
constexpr int kPrefetchEpochShift = kEpochShift + kEpochBits;

inline uint32_t extractDest(uint64_t mask)         { return static_cast<uint32_t>((mask >> kDestShift)     & 0xFFFFFFu); }
inline uint8_t  extractDbg(uint64_t mask)          { return static_cast<uint8_t> ((mask >> kDbgShift)      & 0x3u); }
inline uint8_t  extractPopt(uint64_t mask)         { return static_cast<uint8_t> ((mask >> kPoptShift)     & 0x7Fu); }
inline uint16_t extractEpoch(uint64_t mask)        { return static_cast<uint16_t>((mask >> kEpochShift)    & 0xFFFFu); }
inline uint32_t extractPrefetchTarget(uint64_t m)  { return static_cast<uint32_t>((m    >> kPrefetchShift) & 0x7FFFFFFFu); }
inline uint32_t extractPrefetchTargetEpoch(uint64_t m) { return static_cast<uint32_t>((m >> kPrefetchEpochShift) & 0x7FFFu); }

inline uint64_t packMask(uint32_t dest, uint8_t dbg, uint8_t popt, uint32_t pfx) {
    return (static_cast<uint64_t>(dest & 0xFFFFFFu)) |
           (static_cast<uint64_t>(dbg  & 0x3u)   << kDbgShift) |
           (static_cast<uint64_t>(popt & 0x7Fu)  << kPoptShift) |
           (static_cast<uint64_t>(pfx  & 0x7FFFFFFFu) << kPrefetchShift);
}

inline uint64_t packMaskEpoch(uint32_t dest, uint8_t dbg, uint8_t popt,
                              uint16_t epoch, uint32_t pfx) {
    return (static_cast<uint64_t>(dest & 0xFFFFFFu)) |
           (static_cast<uint64_t>(dbg  & 0x3u)   << kDbgShift) |
           (static_cast<uint64_t>(popt & 0x7Fu)  << kPoptShift) |
           (static_cast<uint64_t>(epoch & 0xFFFFu) << kEpochShift) |
           (static_cast<uint64_t>(pfx  & 0x7FFFu) << kPrefetchEpochShift);
}

// === Wide epoch+prefetch layout for large-graph ECG_PFX =======================
// packMaskEpoch squeezes the prefetch target into 15 bits (<=32767) because it also
// carries the vestigial dbg(2)+popt(7) fields. For the ECG_GRASP_POPT + ECG_PFX path
// those are NOT load-bearing (eviction uses the epoch; popt/dbg are for the legacy
// ECG_EMBEDDED mode), so reclaim them to widen the prefetch target to 24 bits:
//   packMaskEpochWide:  dest[0:24] | epoch[24:40] (16) | pfx[40:64] (24)
// -> targets up to 16,777,215 ids, covering all evaluated graphs with no wider record.
// This is a SEPARATE layout: packMask (cache_sim/Sniper, 31-bit) and packMaskEpoch
// (legacy gem5 ECG_EMBEDDED) are UNCHANGED. The gem5 `ecg.extract.wide` ISA op decodes
// this layout in lockstep (decoder_ecg_extract.isa).
constexpr int kEpochWideShift     = kDestBits;                 // 24
constexpr int kPrefetchWideBits   = 24;
constexpr int kPrefetchWideShift  = kDestBits + kEpochBits;    // 24 + 16 = 40

inline uint64_t packMaskEpochWide(uint32_t dest, uint16_t epoch, uint32_t pfx) {
    return (static_cast<uint64_t>(dest  & 0xFFFFFFu)) |
           (static_cast<uint64_t>(epoch & 0xFFFFu)   << kEpochWideShift) |
           (static_cast<uint64_t>(pfx   & 0xFFFFFFu) << kPrefetchWideShift);
}
inline uint16_t extractEpochWide(uint64_t m) {
    return static_cast<uint16_t>((m >> kEpochWideShift) & 0xFFFFu);
}
inline uint32_t extractPrefetchTargetWide(uint64_t m) {
    return static_cast<uint32_t>((m >> kPrefetchWideShift) & 0xFFFFFFu);
}

// === Configurable-width EVICT layout ==========================================
// The dest field is sized to fit the graph so one instruction scales without a
// wider register: width class wc in {0,1,2,3} -> W = 8/16/24/32 dest bits, and the
// next-ref EPOCH always rides the next 16 bits [W:W+16]; EVICT+PFX puts the (Path-B)
// prefetch target above [W+16:64]. The gem5 ecg.load decoder reads wc from FUNCT7
// bits[26:25] (ECG_WIDTH) and computes W = 8*(wc+1) -> these helpers MUST stay
// bit-identical to that ea_code (the field-parity test pins it across all 3 sims).
//   wc=2 (W=24) == packMaskEpochWide above (the 24-bit primary default).
inline int      ecgEvictWidthBits(int wc)  { return 8 * ((wc & 0x3) + 1); }          // 8/16/24/32
inline int      ecgEvictWidthClass(uint64_t num_vertices) {
    // smallest W in {8,16,24,32} that holds an id in [0, num_vertices)
    if (num_vertices <= (1ULL << 8))  return 0;
    if (num_vertices <= (1ULL << 16)) return 1;
    if (num_vertices <= (1ULL << 24)) return 2;
    return 3;
}
inline uint64_t ecgEvictDestMask(int wc) {
    int W = ecgEvictWidthBits(wc);
    return (W >= 32) ? 0xFFFFFFFFULL : ((1ULL << W) - 1);
}
inline uint64_t packEvict(uint32_t dest, uint16_t epoch, int wc) {
    int W = ecgEvictWidthBits(wc);
    return (static_cast<uint64_t>(dest) & ecgEvictDestMask(wc)) |
           (static_cast<uint64_t>(epoch & 0xFFFFu) << W);
}
inline uint64_t packEvictPfx(uint32_t dest, uint16_t epoch, uint32_t pfx, int wc) {
    int W = ecgEvictWidthBits(wc);
    return packEvict(dest, epoch, wc) |
           (static_cast<uint64_t>(pfx & 0xFFFFFFu) << (W + 16));
}
inline uint32_t extractEvictDest(uint64_t m, int wc) {
    return static_cast<uint32_t>(m & ecgEvictDestMask(wc));
}
inline uint16_t extractEvictEpoch(uint64_t m, int wc) {
    return static_cast<uint16_t>((m >> ecgEvictWidthBits(wc)) & 0xFFFFu);
}
inline uint32_t extractEvictPfxTarget(uint64_t m, int wc) {
    return static_cast<uint32_t>((m >> (ecgEvictWidthBits(wc) + 16)) & 0xFFFFFFu);
}

// === Epoch-only layout (ECG_GRASP_POPT + Path A) ===============================
// The stored prefetch-target field belongs to Path B and is unused by this path:
// Path A reads the next-K targets straight from the CSR edge stream (= DROPLET's
// read-ahead), so nothing is stored for prefetch. Removing it (and the
// unused popt rank) reclaims 38 bits, all given to the next-ref EPOCH so the
// epoch never gets starved as ids grow (the at-scale bit-fit worry): a full
// 34-bit epoch coexists with a 28-bit dest (<=268M verts, covers twitter 41M /
// friendster 65M / kron-s27 134M).
//   packMaskEpochOnly:  dest[0:28] | dbg[28:30] (tier) | epoch[30:64] (34b)
// NO prefetch target. The gem5 `ecg.extract` funct7=0x1 decodes this in lockstep.
constexpr int kEoDestBits   = 28;
constexpr int kEoDbgBits    = 2;
constexpr int kEoDbgShift   = kEoDestBits;               // 28
constexpr int kEoEpochShift = kEoDestBits + kEoDbgBits;  // 30
constexpr int kEoEpochBits  = 64 - kEoEpochShift;        // 34
constexpr uint32_t kEoDestMask  = (1u << kEoDestBits) - 1;           // 0x0FFFFFFF
constexpr uint64_t kEoEpochMask = (1ULL << kEoEpochBits) - 1;        // 34-bit

inline uint64_t packMaskEpochOnly(uint32_t dest, uint8_t dbg, uint64_t epoch) {
    return (static_cast<uint64_t>(dest & kEoDestMask)) |
           (static_cast<uint64_t>(dbg & 0x3u) << kEoDbgShift) |
           ((epoch & kEoEpochMask) << kEoEpochShift);
}
inline uint32_t extractDestEpochOnly(uint64_t m)  { return static_cast<uint32_t>(m & kEoDestMask); }
inline uint8_t  extractDbgEpochOnly(uint64_t m)   { return static_cast<uint8_t>((m >> kEoDbgShift) & 0x3u); }
inline uint64_t extractEpochOnly(uint64_t m)      { return (m >> kEoEpochShift) & kEoEpochMask; }

// === Canonical ECG prefetch-target decision ===
//
// Given a vertex's in-neighbour list and the position of the current edge `i`,
// pick the prefetch target: among the next `k_lookahead` neighbours (positions
// i+1 .. i+K), the one whose property line has the SMALLEST average re-reference
// distance (POPT-best = soonest reused). Returns 0 if prefetch is disabled
// (k_lookahead<=0), there is no re-reference data, or no candidate qualifies.
//
// This is the ECG prefetch decision used by every simulator: the kernel
// (bench/src_{sim,gem5,sniper}) builds the per-edge mask offline with this
// function via buildInEdgeMasks, and the prefetcher simply consumes the encoded
// target. A single unit test of this pure function therefore verifies the ECG
// prefetch target for cache_sim, gem5 and Sniper alike. See
// bench/src_sim/test_ecg_prefetch.cc and scripts/experiments/ecg/verify/pfx.py.
inline uint32_t selectPrefetchTarget(
    const uint32_t* neighbors, size_t n_neighbors, size_t i,
    const std::vector<uint8_t>& avg_reref_by_line,
    int k_lookahead, int num_vtx_per_line)
{
    if (k_lookahead <= 0 || avg_reref_by_line.empty()) return 0;
    if (num_vtx_per_line < 1) num_vtx_per_line = 16;
    uint32_t prefetch_target = 0;
    uint8_t best = 128;
    int probe = std::min<int>(k_lookahead,
        static_cast<int>(n_neighbors) - static_cast<int>(i) - 1);
    for (int step = 1; step <= probe; step++) {
        uint32_t cand = neighbors[i + step];
        uint32_t cl = cand / static_cast<uint32_t>(num_vtx_per_line);
        if (cl < avg_reref_by_line.size()) {
            uint8_t d = avg_reref_by_line[cl];
            if (d < best) {
                best = d;
                prefetch_target = cand;
            }
        }
    }
    return prefetch_target;
}

// === Helper: derive avg_reref_by_line from POPT matrix ===
//
// POPT matrix layout (from popt.h::makeOffsetMatrix):
//   matrix[epoch * num_cache_lines + cline] = uint8_t
//     - high bit (0x80) set: invalid/no-data entry
//     - low 7 bits: quantized re-reference distance (0..127)
inline void computeAvgRerefByLine(
    const uint8_t* popt_matrix,
    uint32_t num_cache_lines,
    uint32_t num_epochs,
    std::vector<uint8_t>& out_avg_reref)
{
    out_avg_reref.assign(num_cache_lines, 0);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static)
#endif
    for (int64_t cl_i = 0; cl_i < static_cast<int64_t>(num_cache_lines); ++cl_i) {
        uint32_t cline = static_cast<uint32_t>(cl_i);
        uint32_t total_dist = 0;
        uint32_t count = 0;
        for (uint32_t e = 0; e < num_epochs; e++) {
            uint8_t entry = popt_matrix[e * num_cache_lines + cline];
            if ((entry & 0x80) == 0) {
                total_dist += (entry & 0x7F);
                count++;
            }
        }
        out_avg_reref[cline] = count > 0
            ? static_cast<uint8_t>(std::min(total_dist / count, uint32_t(127)))
            : 0;
    }
}

// === Per-vertex tier computation (degree-bucketed, mirrors cache_sim) ===
template <typename GraphT>
void computeDegreeTiers(const GraphT& g, std::vector<uint8_t>& out_tiers) {
    uint32_t n = g.num_nodes();
    out_tiers.assign(n, 0);
    if (n == 0) return;

    std::vector<int64_t> degs(n);
#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for (uint32_t v = 0; v < n; v++)
        degs[v] = g.out_degree(v);

    // Compute simple quartile cutoffs by sorting a copy.
    std::vector<int64_t> sorted_degs = degs;
    std::sort(sorted_degs.begin(), sorted_degs.end());
    int64_t q1 = sorted_degs[(n * 1) / 4];
    int64_t q2 = sorted_degs[(n * 2) / 4];
    int64_t q3 = sorted_degs[(n * 3) / 4];

#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for (uint32_t v = 0; v < n; v++) {
        int64_t d = degs[v];
        uint8_t tier = (d >= q3) ? 3 : (d >= q2) ? 2 : (d >= q1) ? 1 : 0;
        out_tiers[v] = tier;
    }
}

// === Main builder: in_edge masks for src-side iteration (PR pull pattern) ===
//
// PR pull iterates over each src's in_neighbors. For each (src, edge_pos)
// where edge_pos indexes into src.in_neigh, we encode a mask whose
// prefetch_target field names the next-K POPT-best vertex in src's
// in_neighbors at positions edge_pos+1..edge_pos+K.
//
// The builder is thread-safe across src (outer loop parallelizes).
//
// Arguments:
//   g                 — input graph
//   tiers             — per-vertex tier array (or empty to skip DBG encoding)
//   avg_reref_by_line — derived from POPT matrix via computeAvgRerefByLine
//                       (or empty to skip POPT-ranked target selection)
//   k_lookahead       — how many positions ahead to scan for POPT-best target
//                       (0 disables prefetch encoding)
//   num_vtx_per_line  — used to map vertex -> cache line for reref lookup
//                       (default: 16, matching cache_sim's numVtxPerLine)
//   out_masks         — output: out_masks[src][edge_pos] is the 64-bit mask
//   stats_log_label   — optional label printed to stdout summarizing build stats
//                       (pass empty string to suppress logging)
template <typename GraphT>
void buildInEdgeMasks(
    const GraphT& g,
    const std::vector<uint8_t>& tiers,
    const std::vector<uint8_t>& avg_reref_by_line,
    int k_lookahead,
    int num_vtx_per_line,
    std::vector<std::vector<uint64_t>>& out_masks,
    const char* stats_log_label = nullptr)
{
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    uint32_t n = g.num_nodes();
    out_masks.clear();
    out_masks.resize(n);
    if (n == 0) return;
    if (num_vtx_per_line < 1) num_vtx_per_line = 16;

    uint64_t edge_count = 0;
    uint64_t encoded_count = 0;
    const bool have_tiers = !tiers.empty();
    const bool have_reref = !avg_reref_by_line.empty();

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 128) reduction(+:edge_count, encoded_count)
#endif
    for (uint32_t src = 0; src < n; src++) {
        std::vector<uint32_t> neighbors;
        neighbors.reserve(64);
        for (auto v : g.in_neigh(src))
            neighbors.push_back(static_cast<uint32_t>(v));

        auto& masks = out_masks[src];
        masks.resize(neighbors.size(), 0);

        for (size_t i = 0; i < neighbors.size(); i++) {
            uint32_t dest = neighbors[i];
            edge_count++;

            uint8_t dbg = (have_tiers && dest < tiers.size()) ? tiers[dest] : 0;

            uint8_t popt = 0;
            if (have_reref) {
                uint32_t dcl = dest / static_cast<uint32_t>(num_vtx_per_line);
                if (dcl < avg_reref_by_line.size())
                    popt = avg_reref_by_line[dcl] & 0x7F;
            }

            uint32_t prefetch_target = selectPrefetchTarget(
                neighbors.data(), neighbors.size(), i,
                avg_reref_by_line, k_lookahead, num_vtx_per_line);
            if (prefetch_target != 0) encoded_count++;

            masks[i] = packMask(dest, dbg, popt, prefetch_target);
        }
    }

    if (stats_log_label && stats_log_label[0]) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - t0).count();
        double pct = edge_count > 0
            ? (100.0 * static_cast<double>(encoded_count) / static_cast<double>(edge_count))
            : 0.0;
        std::printf("[ecg_mode6 %s] vertices=%u edges=%llu encoded=%llu (%.1f%%) build_s=%.4f\n",
            stats_log_label, n,
            static_cast<unsigned long long>(edge_count),
            static_cast<unsigned long long>(encoded_count),
            pct, us / 1e6);
    }
}

} // namespace ecg_mode6

#endif // ECG_MODE6_BUILDER_H
