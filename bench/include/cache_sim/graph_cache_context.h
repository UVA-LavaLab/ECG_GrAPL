// ============================================================================
// GraphCacheContext: Unified graph-aware cache metadata for all policies
// ============================================================================
//
// Single structure carrying ALL graph metadata needed by graph-aware cache
// replacement policies (GRASP, P-OPT, MASK/ECG, GRASP-OPT, GRASP-XP).
//
// Design goals:
//   1. Unified — one struct serves all current and future graph-aware policies
//   2. Simulator-ready — flat/serializable layout for Sniper/gem5 pass-through
//   3. Multi-region — tracks multiple vertex property arrays (scores, contrib)
//   4. Per-access context — carries source/dest vertex + mask hint per access
//   5. Self-tuning — hot fractions computed from degree distribution, not manual
//
// Simulator integration (Sniper/gem5):
//   - Flat fields (topology, hints, reref config) map to memory-mapped registers
//   - PropertyRegion array is fixed-size (MAX_PROPERTY_REGIONS = 8)
//   - Rereference matrix is a contiguous uint8_t buffer (DMA-able)
//   - setCurrentVertices() maps to a magic instruction writing registers
//
// References:
//   - GRASP: Faldu et al. (2020), DBG + 3-tier RRIP
//   - P-OPT: Balaji et al. (2021), transpose-based Belady approximation
//   - ECG:   Mughrabi et al., GrAPL (MASK encoding + multi-region + prefetch)
//
// Author: GraphBrew Team
// ============================================================================

#ifndef GRAPH_CACHE_CONTEXT_H_
#define GRAPH_CACHE_CONTEXT_H_

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <unordered_set>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "../ecg_mode6_builder.h"
#include "../ecg_reuse_plan_builder.h"
#include "../ecg_victim_policy.h"

namespace cache_sim {

// The two per-edge mask sets correspond to the two edge lists of the graph:
//   OUT = the MAIN graph (g.out_neigh, masks built by buildOutEdgeMasks, epoch from
//         in_neigh) — consumed by push kernels (BFS top-down, SSSP, BC forward).
//   IN  = the INVERSE graph (g.in_neigh, masks built by buildInEdgeMasks, epoch from
//         out_neigh) — consumed by pull kernels (BFS bottom-up; PR/CC via the
//         specialized buildInEdgeMasks_PR path).
// Each set is built and "ready" independently, so a kernel that needs a given edge
// list asks GraphCacheContext for that direction's canonical mask
// for the per-edge demand path.
enum class EdgeMaskDir { OUT, IN };

// Tier A sideband-registration sanity: emit a single stderr line per region
// at registration time so external tests (and humans) can verify the loader
// saw the expected (base, upper, hot_pct, grasp_region) tuple. Suppress with
// GRAPHBREW_SIDEBAND_LOG=0 to keep noisy benchmarking runs quiet.
inline bool graphCtxRegistrationLogEnabled() {
    static int enabled = []() {
        const char* value = std::getenv("GRAPHBREW_SIDEBAND_LOG");
        if (!value || !value[0]) return 1;
        return (std::strcmp(value, "0") == 0) ? 0 : 1;
    }();
    return enabled != 0;
}

inline void logGraphCtxRegistration(const char* source,
                                    const char* name,
                                    uint64_t base,
                                    uint64_t upper,
                                    uint32_t hot_pct,
                                    bool grasp_region) {
    if (!graphCtxRegistrationLogEnabled()) return;
    std::fprintf(stderr,
                 "[graphctx] register region source=%s name=%s base=0x%lx "
                 "upper=0x%lx hot_pct=%u grasp_region=%d\n",
                 source ? source : "?",
                 (name && name[0]) ? name : "(unnamed)",
                 static_cast<unsigned long>(base),
                 static_cast<unsigned long>(upper),
                 hot_pct,
                 grasp_region ? 1 : 0);
}

// ============================================================================
// PropertyRegion: One tracked vertex data array
// ============================================================================
// Each graph algorithm accesses multiple vertex property arrays (e.g., scores,
// outgoing_contrib, comp). GRASP/ECG need to know which array an address
// belongs to, and which degree bucket a vertex falls in.
//
// After DBG reordering, vertices are sorted by descending degree. The array
// is partitioned into N degree buckets (default 11, matching DBG). Each
// bucket maps to a proportional RRPV based on its share of total edges.
//
// The number of buckets is flexible:
//   - GRASP uses three regions (simplified from the RRPV space)
//   - ECG GRASP-XP uses all 11 DBG buckets with 8-bit RRPV
//   - Our implementation supports 1-16 buckets (configurable)
//
// bucket_bounds[i] = byte offset where bucket i ends in the array
// bucket_bounds[0] = end of highest-degree bucket (smallest, at front)
// bucket_bounds[N-1] = end of lowest-degree bucket (= upper_bound)
//
// Requires DBG-reordered graph for meaningful classification.
static constexpr uint32_t MAX_REGION_BUCKETS = 16;

struct PropertyRegion {
    uint64_t base_address = 0;      // Start address of the property array
    uint64_t upper_bound = 0;       // End address
    uint32_t num_elements = 0;      // Number of elements
    uint32_t elem_size = 0;         // Size of each element in bytes
    uint32_t region_id = 0;         // ID for per-region statistics
    uint32_t num_buckets = 0;       // Active bucket count (0 = uninitialized)
    uint32_t grasp_hot_percent = 15; // GRASP frontier_frac as % of VERTEX SPACE (array-relative, GRASP-faithful per ligra.h add_region). ~0.15 reproduces Faldu corpus results AND auto-scales (vs the old fixed 0.50-of-LLC which under-protected large graphs). ~ Faldu's stated 10% (which is vertex-relative).
    bool grasp_region = true;       // Whether GRASP treats this as propertyA/B

    // Bucket boundaries: bucket_bounds[i] = upper byte address of bucket i
    // Bucket 0 = highest-degree (most important to cache)
    // Bucket num_buckets-1 = lowest-degree (least important)
    uint64_t bucket_bounds[MAX_REGION_BUCKETS] = {};

    // Classify an address into a bucket index.
    // Returns: 0..num_buckets-1 for valid buckets, num_buckets for outside region
    uint32_t classifyBucket(uint64_t addr) const {
        if (addr < base_address || addr >= upper_bound || num_buckets == 0)
            return num_buckets;  // Outside region
        for (uint32_t b = 0; b < num_buckets; ++b) {
            if (addr < bucket_bounds[b]) return b;
        }
        return num_buckets - 1;  // Last bucket
    }

    // Legacy 4-tier classification (for backward compatibility)
    // Maps N buckets to 4 tiers: first quarter = HOT(1), etc.
    uint32_t classify(uint64_t addr) const {
        uint32_t b = classifyBucket(addr);
        if (b >= num_buckets) return 0;  // Outside
        // Map bucket to tier: 0..N/4=HOT(1), N/4..N/2=WARM(2), etc.
        uint32_t quarter = (num_buckets > 0) ? (b * 4 / num_buckets) : 3;
        return quarter + 1;  // 1=HOT, 2=WARM, 3=LUKEWARM, 4=COLD
    }
};

// Maximum property regions (compile-time for simulator flat layout)
// ECG uses 5 (irregData, regData, CSR-offsets, CSR-coords, frontier)
// GraphBrew needs at most 4 (scores, contrib, comp, dist)
static constexpr uint32_t MAX_PROPERTY_REGIONS = 8;

// ============================================================================
// ECGMode: Controls how the ECG policy layers its eviction signals
// ============================================================================
// ECG uses a 3-level layered eviction strategy. The mode determines the
// priority order of the tiebreakers after SRRIP aging (Level 1).
//
// All modes use SRRIP aging as Level 1 (find max RRPV, age until found).
// The mode controls Level 2 and Level 3 tiebreakers:
//
//   DBG_PRIMARY  (default): SRRIP → DBG tier → dynamic P-OPT
//   POPT_PRIMARY:           SRRIP → dynamic P-OPT → DBG tier
//   POPT_TIE:               SRRIP candidates → dynamic P-OPT → DBG tier
//   DBG_ONLY:               GRASP-faithful insertion/hit hints, plain SRRIP victim
//   ECG_EMBEDDED:           Stored P-OPT hint as primary, DBG as secondary
//   ECG_EPOCH_EMBEDDED:     Current-epoch compact P-OPT hint, DBG as secondary
//   ECG_COMBINED:           Both DBG + P-OPT hint → unified insertion RRPV (Hawkeye-inspired)
// ECG mode taxonomy. Enum values are serialized; do not reorder them.
//   [PRIMARY]      the primary ECG policy .................... ECG_GRASP_POPT
//   [BASELINE]     primary-matrix + cross-simulator baselines  DBG_PRIMARY, POPT_PRIMARY,
//                  DBG_ONLY
//   [ARCHIVE]      compatibility and diagnostic modes used by proof_matrix and
//                  the gem5/Sniper overlays.
enum class ECGMode {
    DBG_PRIMARY,   // [BASELINE] DBG tier is primary tiebreaker, P-OPT is secondary
    POPT_PRIMARY,  // [BASELINE] Dynamic P-OPT is primary tiebreaker, DBG is secondary
    POPT_TIE,      // [ARCHIVE] SRRIP narrows candidates, dynamic P-OPT picks among ties
    DBG_ONLY,      // [BASELINE] GRASP-equivalent insertion/hit hints, no eviction tiebreak
    ECG_EMBEDDED,  // [ARCHIVE] Stored P-OPT hint primary, DBG tier secondary
    ECG_EPOCH_EMBEDDED, // [ARCHIVE] Current-epoch P-OPT hint primary, DBG tier secondary
    ECG_COMBINED,  // [ARCHIVE] Combined DBG and P-OPT insertion RRPV
    ECG_EXACT,     // [ARCHIVE] Live position-indexed next-reference eviction
    ECG_EXACT_STORED, // [ARCHIVE] Access-time exact stamps with all-way selection
    ECG_EXACT_MASK,   // [ARCHIVE] Precomputed 5-bit per-edge exact mask
    ECG_GRASP_POPT    // [PRIMARY] GRASP insertion with stored next-reference epochs
};

inline std::string ECGModeToString(ECGMode mode) {
    switch (mode) {
        case ECGMode::DBG_PRIMARY:  return "DBG_PRIMARY";
        case ECGMode::POPT_PRIMARY: return "POPT_PRIMARY";
        case ECGMode::POPT_TIE:     return "POPT_TIE";
        case ECGMode::DBG_ONLY:     return "DBG_ONLY";
        case ECGMode::ECG_EMBEDDED: return "ECG_EMBEDDED";
        case ECGMode::ECG_EPOCH_EMBEDDED: return "ECG_EPOCH_EMBEDDED";
        case ECGMode::ECG_COMBINED: return "ECG_COMBINED";
        case ECGMode::ECG_EXACT: return "ECG_EXACT";
        case ECGMode::ECG_EXACT_STORED: return "ECG_EXACT_STORED";
        case ECGMode::ECG_EXACT_MASK: return "ECG_EXACT_MASK";
        case ECGMode::ECG_GRASP_POPT: return "ECG_GRASP_POPT";
        default:                    return "UNKNOWN";
    }
}

inline ECGMode StringToECGMode(const std::string& s) {
    if (s.empty()) return ECGMode::DBG_PRIMARY;  // unset/empty env -> default (parity with gem5/Sniper)
    if (s == "DBG_PRIMARY" || s == "dbg_primary") return ECGMode::DBG_PRIMARY;
    if (s == "POPT_PRIMARY" || s == "popt_primary" || s == "popt") return ECGMode::POPT_PRIMARY;
    if (s == "POPT_TIE" || s == "popt_tie" || s == "popt_tiebreak") return ECGMode::POPT_TIE;
    if (s == "DBG_ONLY" || s == "dbg_only" || s == "dbg") return ECGMode::DBG_ONLY;
    if (s == "ECG_EMBEDDED" || s == "ecg_embedded" || s == "embedded") return ECGMode::ECG_EMBEDDED;
    if (s == "ECG_EPOCH_EMBEDDED" || s == "ecg_epoch_embedded" || s == "epoch_embedded") return ECGMode::ECG_EPOCH_EMBEDDED;
    if (s == "ECG_COMBINED" || s == "ecg_combined" || s == "combined") return ECGMode::ECG_COMBINED;
    if (s == "ECG_EXACT" || s == "ecg_exact" || s == "exact") return ECGMode::ECG_EXACT;
    if (s == "ECG_EXACT_STORED" || s == "ecg_exact_stored" || s == "exact_stored") return ECGMode::ECG_EXACT_STORED;
    if (s == "ECG_EXACT_MASK" || s == "ecg_exact_mask" || s == "exact_mask") return ECGMode::ECG_EXACT_MASK;
    if (s == "ECG_GRASP_POPT" || s == "ecg_grasp_popt" || s == "grasp_popt") return ECGMode::ECG_GRASP_POPT;
    // HARD FAIL on an unrecognized mode. Silently defaulting to DBG_PRIMARY would
    // run a DIFFERENT policy than requested (a typo, or a removed mode) while the
    // run still LABELS itself as the requested mode -> fraudulent-looking results.
    std::cerr << "[FATAL] ECG_MODE='" << s << "' is not a recognized ECG mode. "
                 "Valid: DBG_PRIMARY, POPT_PRIMARY, POPT_TIE, DBG_ONLY, ECG_EMBEDDED, "
                 "ECG_EPOCH_EMBEDDED, ECG_COMBINED, ECG_EXACT, ECG_EXACT_STORED, "
                 "ECG_EXACT_MASK, ECG_GRASP_POPT." << std::endl;
    std::exit(2);
}

// ============================================================================
// MaskConfig: Configuration for per-edge mask array
// ============================================================================
// Controls how per-edge cache hints are encoded, how DBG and P-OPT signals
// are combined, and how prefetch targets are resolved.
//
// The mask array is a parallel array alongside the CSR edge list: same size,
// same indexing. Each entry packs DBG tier + P-OPT rereference + prefetch
// target into mask_width bits.
//
// Key insight: mask decouples classification (degree/rereference) from
// ordering (community). You can use Rabbit Order for locality AND get
// GRASP-quality caching via the mask, without DBG reordering.
struct MaskConfig {
    // ── Field widths ──
    uint8_t  mask_width = 8;        // Total bits per mask entry (8,16,32)
    uint8_t  dbg_bits = 2;          // Bits for degree tier (2-4)
    uint8_t  popt_bits = 4;         // Bits for rereference quantization (0-12)
    uint8_t  prefetch_bits = 2;     // Bits for prefetch target (remaining)

    // ── Prefetch ──
    bool     prefetch_direct = false; // true = raw vertex ID, false = hot table index
    uint32_t hot_table_size = 0;     // 2^prefetch_bits (only if !prefetch_direct)
    uint8_t  prefetch_window = 8;    // Dedup window size for prefetch (4,8,16)
    uint8_t  prefetch_mode = 0;      // 0=NONE, 1=DEGREE (highest-deg neighbor), 2=POPT (nearest reref)

    // ── ECG Mode ──
    // Controls eviction tiebreaker priority (see ECGMode enum above).
    // DBG_PRIMARY:  SRRIP → DBG tier → dynamic P-OPT (default)
    // POPT_PRIMARY: SRRIP → dynamic P-OPT → DBG tier
    // DBG_ONLY:     GRASP-faithful insertion/hit hints, plain SRRIP victim
    ECGMode  ecg_mode = ECGMode::DBG_PRIMARY;

    // ── Classification ──
    uint8_t  num_buckets = 11;      // Number of degree buckets (2-16)
    uint8_t  rrpv_max = 7;          // Maximum RRPV value (3-bit=7, 8-bit=255)
    uint8_t  degree_mode = 0;       // 0=OUT, 1=IN, 2=BOTH
    bool     per_vertex = false;    // false=per-edge O(m), true=per-vertex O(n)
    bool     enabled = false;
    // GRASP insertion-tier SOURCE for the ECG policy — TWO VARIANTS (ECG_GRASP_SRC):
    //   0 = MASK  : tier is DELIVERED in the per-edge bit mask (our ECG; encoded
    //               offline via ecg_policy::graspTierByIndex, read via decodeDBG —
    //               identical across cache_sim/gem5/Sniper by construction).
    //   1 = REGION: tier is RECOMPUTED at insertion from the property region
    //               (the original GRASP / Faldu spatial top-fraction, classifyGRASP).
    // Default REGION = the faithful baseline (no number change). The MASK variant
    // (our ECG, delivered + cross-sim identical) is opt-in via ECG_GRASP_SRC=mask
    // while its prefetch-fill delivery is finalized + propagated to gem5/Sniper.
    uint8_t  grasp_tier_source = 1;       // 1=region (default, GRASP), 0=mask (ECG)
    double   grasp_hot_fraction = 0.15;   // top fraction = HOT (GRASP_HOT_FRACTION)

    // ── Field positions (computed) ──
    uint8_t  prefetch_shift = 0;
    uint8_t  popt_shift = 0;
    uint8_t  dbg_shift = 0;
    uint32_t prefetch_mask_val = 0;
    uint32_t popt_mask_val = 0;
    uint32_t dbg_mask_val = 0;

    // Dynamic bit allocation based on graph size and container width.
    //
    // The mask array entry emulates a fat-ID: given a container of mask_width
    // bits and a graph with num_vertices, the vertex ID needs ceil(log2(N)) bits.
    // The remaining bits are metadata for ECG (DBG + P-OPT + prefetch).
    //
    // If the user sets ECG_DBG_BITS / ECG_POPT_BITS / ECG_PFX_BITS env vars,
    // those override the automatic allocation. Otherwise:
    //   1. Compute id_bits = ceil(log2(num_vertices))
    //   2. spare = mask_width - id_bits (for fat-ID mode) or mask_width (for standalone mask)
    //   3. DBG = min(2, spare)
    //   4. POPT gets up to 7 bits (matches matrix precision)
    //   5. PFX gets remaining (for prefetch target vertex IDs)
    //
    // The user can also set mask_width to any value (not just 8/16/32) to
    // emulate different hardware tag SRAM budgets.
    void autoAllocate(uint32_t num_vertices) {
        // Check for user-specified bit allocation (overrides auto)
        const char* v_dbg = std::getenv("ECG_DBG_BITS");
        const char* v_popt = std::getenv("ECG_POPT_BITS");
        const char* v_pfx = std::getenv("ECG_PFX_BITS");

        if (v_dbg || v_popt || v_pfx) {
            // User-controlled allocation — use exactly what they specify
            if (v_dbg)  dbg_bits = static_cast<uint8_t>(std::atoi(v_dbg));
            if (v_popt) popt_bits = static_cast<uint8_t>(std::atoi(v_popt));
            if (v_pfx)  prefetch_bits = static_cast<uint8_t>(std::atoi(v_pfx));
            // Recompute mask_width from user allocation
            mask_width = dbg_bits + popt_bits + prefetch_bits;
        } else {
            // Auto allocation based on available space
            dbg_bits = 2;
            uint8_t remaining = (mask_width > dbg_bits) ? (mask_width - dbg_bits) : 0;

            // P-OPT: cap at 7 (matches 7-bit rereference matrix precision)
            if (remaining >= 27) popt_bits = 7;
            else if (remaining >= 13) popt_bits = 7;
            else if (remaining >= 6) popt_bits = 6;
            else if (remaining >= 2) popt_bits = 2;
            else popt_bits = 0;
            remaining -= popt_bits;

            // PFX: remaining bits (if >= 4, otherwise give to P-OPT)
            if (remaining >= 4) {
                prefetch_bits = remaining;
            } else {
                popt_bits += remaining;
                prefetch_bits = 0;
            }
        }

        // Can we encode vertex IDs directly?
        uint8_t id_bits = 1;
        while ((1ULL << id_bits) < num_vertices) id_bits++;
        prefetch_direct = (prefetch_bits >= id_bits);
        hot_table_size = prefetch_direct ? 0 : (prefetch_bits > 0 ? ((1U << prefetch_bits) - 1) : 0);

        // Compute shifts and masks
        prefetch_shift = 0;
        popt_shift = prefetch_bits;
        dbg_shift = prefetch_bits + popt_bits;

        prefetch_mask_val = prefetch_bits ? ((1U << prefetch_bits) - 1) : 0;
        popt_mask_val = popt_bits ? (((1U << popt_bits) - 1) << popt_shift) : 0;
        dbg_mask_val = dbg_bits ? (((1U << dbg_bits) - 1) << dbg_shift) : 0;

        enabled = true;
    }

    // Initialize from environment variables
    void initFromEnv() {
        const char* v;
        if ((v = std::getenv("ECG_MASK_WIDTH")))    mask_width = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("ECG_MODE")))          ecg_mode = StringToECGMode(v);
        if ((v = std::getenv("ECG_NUM_BUCKETS")))   num_buckets = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("ECG_RRPV_BITS"))) {
            int bits = std::atoi(v);
            rrpv_max = (1 << bits) - 1;
        }
        if ((v = std::getenv("ECG_PREFETCH_WINDOW"))) prefetch_window = static_cast<uint8_t>(std::atoi(v));
        if (prefetch_window > 16) prefetch_window = 16;
        if ((v = std::getenv("ECG_PREFETCH_MODE"))) prefetch_mode = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("ECG_PER_VERTEX")))    per_vertex = (std::atoi(v) != 0);
        if ((v = std::getenv("ECG_DEGREE_MODE"))) {
            if (std::string(v) == "IN") degree_mode = 1;
            else if (std::string(v) == "BOTH") degree_mode = 2;
            else degree_mode = 0;
        }
        if ((v = std::getenv("ECG_GRASP_SRC")))
            grasp_tier_source = (std::string(v) == "region" || std::string(v) == "REGION") ? 1 : 0;
        if ((v = std::getenv("GRASP_HOT_FRACTION"))) {
            double f = std::atof(v);
            if (f > 0.0 && f <= 1.0) grasp_hot_fraction = f;
        }
    }

    // Encode a mask entry from fields
    uint32_t encode(uint8_t dbg_tier, uint8_t popt_quant, uint32_t prefetch_target) const {
        uint32_t entry = 0;
        entry |= (prefetch_target & prefetch_mask_val);
        entry |= ((uint32_t(popt_quant) << popt_shift) & popt_mask_val);
        entry |= ((uint32_t(dbg_tier) << dbg_shift) & dbg_mask_val);
        return entry;
    }

    // Decode fields from a mask entry
    uint8_t decodeDBG(uint32_t entry) const {
        return dbg_bits ? static_cast<uint8_t>((entry & dbg_mask_val) >> dbg_shift) : 0;
    }
    uint8_t decodePOPT(uint32_t entry) const {
        return popt_bits ? static_cast<uint8_t>((entry & popt_mask_val) >> popt_shift) : 0;
    }
    uint32_t decodePrefetch(uint32_t entry) const {
        return prefetch_bits ? (entry & prefetch_mask_val) : 0;
    }

    // Compute RRPV from DBG tier (structural priority).
    // Higher tier = lower degree = higher RRPV (evict sooner).
    // P-OPT is NOT used at insert — consulted dynamically at eviction.
    uint8_t dbgTierToRRPV(uint8_t dbg_tier) const {
        float fraction = static_cast<float>(dbg_tier) / std::max(uint8_t(1), num_buckets);
        uint8_t result = static_cast<uint8_t>(rrpv_max * fraction);
        if (result > rrpv_max) result = rrpv_max;
        return result;
    }

    void printConfig(std::ostream& os = std::cout) const {
        if (!enabled) { os << "MaskConfig: disabled\n"; return; }
        os << "MaskConfig: " << int(mask_width) << "-bit"
           << " [DBG=" << int(dbg_bits)
           << " POPT=" << int(popt_bits)
           << " PFX=" << int(prefetch_bits) << "]"
           << " mode=" << ECGModeToString(ecg_mode)
           << " buckets=" << int(num_buckets)
           << " rrpv_max=" << int(rrpv_max)
           << " prefetch=" << (prefetch_direct ? "direct" : "table")
           << (per_vertex ? " per-vertex" : " per-edge") << "\n";
    }
};

// ============================================================================
// MaskArray: Per-edge (or per-vertex) mask storage
// ============================================================================
// Parallel array alongside CSR edges. Same indexing: masks[edge_idx].
// Stores encoded ECG hints (DBG tier + P-OPT rereference + prefetch target).
//
// For per-vertex mode: masks[vertex_id] — one entry per vertex, applied to
// all its edges. Cheaper (O(n) vs O(m)) but P-OPT loses per-edge precision.
struct MaskArray {
    const uint8_t*  data8 = nullptr;   // 8-bit mask entries
    const uint16_t* data16 = nullptr;  // 16-bit mask entries
    const uint32_t* data32 = nullptr;  // 32-bit mask entries
    uint64_t        count = 0;         // Number of entries
    uint8_t         entry_width = 0;   // 8, 16, or 32
    bool            enabled = false;

    // Get mask entry by index (auto-width)
    uint32_t get(uint64_t idx) const {
        if (!enabled || idx >= count) return 0;
        switch (entry_width) {
            case 8:  return data8  ? data8[idx]  : 0;
            case 16: return data16 ? data16[idx] : 0;
            case 32: return data32 ? data32[idx] : 0;
            default: return 0;
        }
    }

    // Initialize from raw data pointer
    void init8(const uint8_t* data, uint64_t n) {
        data8 = data; count = n; entry_width = 8; enabled = true;
    }
    void init16(const uint16_t* data, uint64_t n) {
        data16 = data; count = n; entry_width = 16; enabled = true;
    }
    void init32(const uint32_t* data, uint64_t n) {
        data32 = data; count = n; entry_width = 32; enabled = true;
    }
};

// ============================================================================
// FatIDConfig: Adaptive bit layout for encoded neighbor IDs
// ============================================================================
// After DBG reordering, vertex IDs carry inherent degree information (low
// IDs = high degree). ECG encodes cache hints into the top bits of CSR
// neighbor IDs so hints travel with the data through the cache hierarchy.
//
// This struct computes the adaptive bit allocation based on graph size:
//   [container_bits-1 ... real_id_bits]  = metadata fields
//   [real_id_bits-1 ... 0]              = real vertex ID
//
// Metadata fields (allocated from MSB to LSB in priority order):
//   1. DBG tier    (2 bits min) — cache insertion priority (HOT/WARM/LUKEWARM/COLD)
//   2. P-OPT quant (2-4 bits)  — rereference distance (log-quantized from transpose)
//   3. Prefetch Δ  (1-4 bits)  — prefetch delta hint
//
// Unlike P-OPT's rereference matrix (stored in LLC, consuming cache ways),
// fat-ID encoding has ZERO runtime storage overhead — hints are precomputed
// and baked into the CSR edge array during preprocessing.
//
// Quantization: With 2-4 bits instead of P-OPT's 7 bits, we use non-linear
// (logarithmic) mapping — more resolution at short distances where eviction
// decisions matter most.
struct FatIDConfig {
    uint8_t  container_bits = 32; // 32 or 64
    uint8_t  real_id_bits = 32;   // ceil(log2(num_vertices))
    uint8_t  spare_bits = 0;      // container_bits - real_id_bits

    // Metadata field widths (computed from spare_bits)
    uint8_t  dbg_bits = 0;        // Cache tier hint (min 2 when spare >= 2)
    uint8_t  popt_bits = 0;       // Rereference quantization
    uint8_t  prefetch_bits = 0;   // Prefetch delta
    uint8_t  _pad[2] = {};

    // Field positions (bit offset from LSB)
    uint8_t  prefetch_shift = 0;
    uint8_t  popt_shift = 0;
    uint8_t  dbg_shift = 0;

    // Masks for extraction
    uint64_t real_id_mask = 0;
    uint64_t dbg_mask = 0;
    uint64_t popt_mask = 0;
    uint64_t prefetch_mask = 0;

    bool     enabled = false;     // True after computeFromGraph()

    // Compute adaptive bit allocation from graph size.
    // Call once during preprocessing, before encoding any edges.
    void computeFromGraph(uint64_t num_vertices) {
        // Determine container size
        container_bits = (num_vertices > (1ULL << 30)) ? 64 : 32;

        // Real ID bits: minimum to address all vertices
        real_id_bits = 1;
        while ((1ULL << real_id_bits) < num_vertices)
            real_id_bits++;

        spare_bits = container_bits - real_id_bits;

        // If less than 2 spare bits in 32-bit, upgrade to 64-bit
        if (spare_bits < 2 && container_bits == 32) {
            container_bits = 64;
            spare_bits = container_bits - real_id_bits;
        }

        // Adaptive allocation scales with available spare bits.
        // 64-bit containers get much richer metadata than 32-bit.
        // With 8+ P-OPT bits, fat-ID encoding EXCEEDS P-OPT's original
        // 7-bit matrix precision — with zero LLC capacity consumed.
        //
        // Allocation tiers:
        //   spare >= 16: DBG=2, POPT=8, PFX=6 (exceeds P-OPT matrix!)
        //   spare >= 10: DBG=2, POPT=4, PFX=4
        //   spare >= 6:  DBG=2, POPT=2, PFX=2
        //   spare >= 4:  DBG=2, POPT=2, PFX=0
        //   spare >= 2:  DBG=2, POPT=0, PFX=0
        //   spare < 2:   DBG=spare, POPT=0, PFX=0
        if (spare_bits >= 16) {
            dbg_bits = 2;  popt_bits = 8;
            prefetch_bits = (spare_bits - 10 > 6) ? 6 : (spare_bits - 10);
        } else if (spare_bits >= 10) {
            dbg_bits = 2;  popt_bits = 4;
            prefetch_bits = (spare_bits - 6 > 4) ? 4 : (spare_bits - 6);
        } else if (spare_bits >= 6) {
            dbg_bits = 2;  popt_bits = 2;  prefetch_bits = spare_bits - 4;
        } else if (spare_bits >= 4) {
            dbg_bits = 2;  popt_bits = 2;  prefetch_bits = 0;
        } else if (spare_bits >= 2) {
            dbg_bits = 2;  popt_bits = 0;  prefetch_bits = 0;
        } else {
            dbg_bits = spare_bits;  popt_bits = 0;  prefetch_bits = 0;
        }

        // Compute shifts (fields packed from real_id upward)
        prefetch_shift = real_id_bits;
        popt_shift = prefetch_shift + prefetch_bits;
        dbg_shift = popt_shift + popt_bits;

        // Compute masks
        real_id_mask = (1ULL << real_id_bits) - 1;
        prefetch_mask = prefetch_bits ? (((1ULL << prefetch_bits) - 1) << prefetch_shift) : 0;
        popt_mask = popt_bits ? (((1ULL << popt_bits) - 1) << popt_shift) : 0;
        dbg_mask = dbg_bits ? (((1ULL << dbg_bits) - 1) << dbg_shift) : 0;

        enabled = true;
    }

    // ================================================================
    // Encoding (preprocessing time — called per edge)
    // ================================================================

    // Encode a fat neighbor ID from components.
    // dbg_tier: 0-3 (HOT=3, WARM=2, LUKEWARM=1, COLD=0 — high value = high priority)
    // popt_q:   quantized rereference distance (0=imminent, max=distant)
    // pfx_d:    prefetch delta encoding (0=none)
    uint64_t encode(uint64_t real_id, uint8_t dbg_tier,
                    uint8_t popt_q, uint8_t pfx_d) const {
        uint64_t fat = real_id & real_id_mask;
        if (dbg_bits)      fat |= (uint64_t(dbg_tier & ((1 << dbg_bits) - 1)) << dbg_shift);
        if (popt_bits)     fat |= (uint64_t(popt_q & ((1 << popt_bits) - 1)) << popt_shift);
        if (prefetch_bits) fat |= (uint64_t(pfx_d & ((1 << prefetch_bits) - 1)) << prefetch_shift);
        return fat;
    }

    // ================================================================
    // Decoding (runtime — called per neighbor access)
    // ================================================================

    uint64_t extractRealID(uint64_t fat) const {
        return fat & real_id_mask;
    }

    uint8_t extractDBGTier(uint64_t fat) const {
        return dbg_bits ? static_cast<uint8_t>((fat & dbg_mask) >> dbg_shift) : 0;
    }

    uint8_t extractPOPT(uint64_t fat) const {
        return popt_bits ? static_cast<uint8_t>((fat & popt_mask) >> popt_shift) : 0;
    }

    uint8_t extractPrefetch(uint64_t fat) const {
        return prefetch_bits ? static_cast<uint8_t>((fat & prefetch_mask) >> prefetch_shift) : 0;
    }

    // ================================================================
    // Quantization helpers (preprocessing time)
    // ================================================================

    // Quantize a full rereference distance (0..num_epochs) to popt_bits.
    // Uses logarithmic mapping: more resolution at short distances.
    // distance=0 → 0 (imminent), distance=max → max_val (distant)
    uint8_t quantizeRereference(uint32_t full_distance) const {
        if (popt_bits == 0) return 0;
        uint8_t max_val = (1 << popt_bits) - 1;
        if (full_distance == 0) return 0;
        // Log2-based quantization
        uint32_t log_dist = 0;
        uint32_t d = full_distance;
        while (d > 0) { d >>= 1; log_dist++; }
        return (log_dist > max_val) ? max_val : static_cast<uint8_t>(log_dist);
    }

    // Compute DBG tier for a vertex given its degree and graph topology.
    // Returns: 1=HOT, 2=WARM, 3=LUKEWARM, 4=COLD
    // Matches PropertyRegion::classify() numbering so fat-ID decode values
    // can be used directly by cache insertion code without remapping.
    uint8_t computeDBGTier(uint32_t degree, uint32_t avg_degree) const {
        if (dbg_bits == 0) return 4;  // COLD if no bits
        // ECG-style: geometric thresholds from avg_degree
        uint32_t hot_thresh = avg_degree * 4;   // Top hubs
        uint32_t warm_thresh = avg_degree;       // Above average
        uint32_t lukewarm_thresh = avg_degree / 2; // Near average
        if (degree >= hot_thresh)      return 1;  // HOT
        if (degree >= warm_thresh)     return 2;  // WARM
        if (degree >= lukewarm_thresh) return 3;  // LUKEWARM
        return 4;                                 // COLD
        if (degree >= lukewarm_thresh) return 1;  // LUKEWARM
        return 0;                                 // COLD
    }

    // Compute prefetch delta: distance to the next likely access vertex.
    // After DBG ordering, nearby IDs have similar degrees → likely co-accessed.
    // Returns encoded delta (0=none, 1=+1, 2=+2, 3=+4, etc.)
    uint8_t computePrefetchDelta(uint32_t src_id, uint32_t dst_id,
                                  uint32_t num_vertices) const {
        if (prefetch_bits == 0) return 0;
        // Simple heuristic: encode the stride to the next neighbor
        // In DBG-ordered graphs, consecutively-IDed vertices are often
        // co-accessed (same degree bucket). Delta encoding captures this.
        // For now: 0=no prefetch (default)
        // Future: compute from graph transpose neighbor patterns
        (void)src_id; (void)dst_id; (void)num_vertices;
        return 0;  // Placeholder — requires graph-specific analysis
    }

    // ================================================================
    // Diagnostics
    // ================================================================

    void printConfig(std::ostream& os = std::cout) const {
        if (!enabled) { os << "FatID: disabled\n"; return; }
        os << "FatID: " << int(container_bits) << "-bit container, "
           << int(real_id_bits) << "-bit ID, "
           << int(spare_bits) << " spare bits "
           << "[DBG=" << int(dbg_bits)
           << " POPT=" << int(popt_bits)
           << " PFX=" << int(prefetch_bits) << "]\n";
        os << "  Layout: [" << int(dbg_shift) << ":" << int(dbg_shift + dbg_bits - 1) << "]=DBG"
           << " [" << int(popt_shift) << ":" << int(popt_shift + popt_bits - 1) << "]=POPT"
           << " [" << int(prefetch_shift) << ":" << int(prefetch_shift + prefetch_bits - 1) << "]=PFX"
           << " [0:" << int(real_id_bits - 1) << "]=ID\n";
    }
};

// ============================================================================
// RereferenceConfig: P-OPT compressed transpose matrix
// ============================================================================
// Stored as flat uint8_t array of size [num_epochs × num_cache_lines].
// Layout: matrix[epoch * num_cache_lines + cache_line_id]
//
// Entry encoding (8-bit, official P-OPT artifact convention):
//   MSB=0: cache line IS referenced in this epoch
//     bits [6:0] = sub-epoch of LAST access within epoch
//   MSB=1: cache line is NOT referenced in this epoch
//     bits [6:0] = distance (in epochs) to next epoch with a reference
struct RereferenceConfig {
    const uint8_t* matrix = nullptr;  // Rereference matrix data (not owned)
    uint32_t num_cache_lines = 0;     // Cache lines covering vertex data
    uint32_t num_epochs = 256;        // Number of epochs (typically 256)
    uint32_t epoch_size = 0;          // Vertices per epoch
    uint32_t sub_epoch_size = 0;      // Vertices per sub-epoch (epoch_size / 128)
    uint32_t line_size = 64;          // Cache line size in bytes
    uint32_t _pad = 0;

    // Algorithm 2: compute next-reference distance for a cache line
    uint32_t findNextRef(uint32_t cline_id, uint32_t current_vertex) const {
        if (matrix == nullptr || cline_id >= num_cache_lines) return 127;
        if (epoch_size == 0 || sub_epoch_size == 0) return 127;
        struct PositionCache {
            const RereferenceConfig* owner = nullptr;
            uint32_t vertex = UINT32_MAX;
            uint32_t epoch_size = 0;
            uint32_t sub_epoch_size = 0;
            uint32_t num_epochs = 0;
            uint32_t epoch_id = 0;
            uint32_t current_sub_epoch = 0;
        };
        static thread_local PositionCache cache;
        if (cache.owner != this || cache.vertex != current_vertex ||
            cache.epoch_size != epoch_size ||
            cache.sub_epoch_size != sub_epoch_size ||
            cache.num_epochs != num_epochs) {
            cache.owner = this;
            cache.vertex = current_vertex;
            cache.epoch_size = epoch_size;
            cache.sub_epoch_size = sub_epoch_size;
            cache.num_epochs = num_epochs;
            cache.epoch_id = current_vertex / epoch_size;
            cache.current_sub_epoch =
                (current_vertex % epoch_size) / sub_epoch_size;
        }
        const uint32_t epoch_id = cache.epoch_id;
        if (epoch_id >= num_epochs) return 127;

        uint8_t entry = matrix[epoch_id * num_cache_lines + cline_id];
        constexpr uint8_t MSB = 0x80;
        constexpr uint8_t MASK = 0x7F;

        if ((entry & MSB) != 0) {
            // Not referenced in this epoch — data encodes distance to next epoch.
            return entry & MASK;
        } else {
            // Referenced in this epoch — check sub-epoch position.
            uint8_t last_sub = entry & MASK;
            const uint32_t curr_sub = cache.current_sub_epoch;
            if (curr_sub <= last_sub) return 0;  // Still upcoming
            // Past final access — check next epoch
            if (epoch_id + 1 < num_epochs) {
                uint8_t next = matrix[(epoch_id + 1) * num_cache_lines + cline_id];
                if ((next & MSB) == 0) return 1;  // Referenced next epoch
                uint8_t dist = next & MASK;
                return (dist < 127) ? dist + 1 : 127;
            }
            return 127;
        }
    }
};

// ============================================================================
// GraphTopology: Degree distribution summary
// ============================================================================
// Used by GRASP-XP for degree-bucket-proportional RRPV,
// and by auto-computation of hot fractions.
struct GraphTopology {
    uint32_t num_vertices = 0;
    uint64_t num_edges = 0;
    uint32_t avg_degree = 0;
    uint32_t max_degree = 0;
    bool     directed = false;

    // ECG-style logarithmic degree buckets
    static constexpr uint32_t NUM_BUCKETS = 11;
    uint32_t bucket_thresholds[NUM_BUCKETS] = {};
    uint64_t bucket_counts[NUM_BUCKETS] = {};
    uint64_t bucket_total_degrees[NUM_BUCKETS] = {};

    void computeFromDegrees(const uint32_t* degrees, uint32_t n,
                            uint64_t m, bool dir) {
        num_vertices = n;
        num_edges = m;
        directed = dir;
        avg_degree = (n > 0) ? static_cast<uint32_t>(m / n) : 0;
        max_degree = 0;

        // Initialize thresholds (ECG-style logarithmic buckets)
        bucket_thresholds[0] = (avg_degree > 1) ? avg_degree / 2 : 1;
        for (uint32_t i = 1; i < NUM_BUCKETS - 1; ++i)
            bucket_thresholds[i] = bucket_thresholds[i - 1] * 2;
        bucket_thresholds[NUM_BUCKETS - 1] = UINT32_MAX;

        std::memset(bucket_counts, 0, sizeof(bucket_counts));
        std::memset(bucket_total_degrees, 0, sizeof(bucket_total_degrees));

        for (uint32_t v = 0; v < n; ++v) {
            uint32_t d = degrees[v];
            if (d > max_degree) max_degree = d;
            for (uint32_t b = 0; b < NUM_BUCKETS; ++b) {
                if (d <= bucket_thresholds[b]) {
                    bucket_counts[b]++;
                    bucket_total_degrees[b] += d;
                    break;
                }
            }
        }
    }
};

// ============================================================================
// AccessHints: Per-access context (maps to simulator registers)
// ============================================================================
// Updated by simulation code on each outer-loop iteration and inner-loop access.
// In Sniper/gem5, these would be written via magic instructions.
struct AccessHints {
    uint32_t current_src = UINT32_MAX;  // Outer-loop vertex (regInd in P-OPT)
    uint32_t current_dst = UINT32_MAX;  // Inner-loop neighbor (irregInd in P-OPT)
    uint32_t mask = 0;                  // ECG MASK hint (supports 8/16/32-bit widths)
    uint8_t  mask_bits = 2;             // Number of mask bits (2=ECG default, 4/8 for finer control)
    uint8_t  _pad1 = 0;
    uint16_t edge_epoch = 0;            // ECG_GRASP_POPT: absolute next-ref epoch, carried untruncated (mask>>26 loses bit 32)
    bool     edge_epoch_valid = true;   // is the line's stored epoch a real per-edge DELIVERY?
                                        // Defaults TRUE so kernels that always deliver (PR pull) and
                                        // all pre-delivery/init fills stay stamped (legacy behavior,
                                        // keeps the primary PR path byte-identical). clearEdgeEpoch() sets
                                        // it FALSE for a SEQUENTIAL/cleared read (BC/SSSP/BFS source
                                        // reads) — the ONLY thing that un-stamps a fill, matching
                                        // gem5/Sniper which stamp only on real per-edge delivery.
                                        // Resolves the epoch==0 ambiguity (real epoch-0 delivery is
                                        // still valid; a cleared read is not).
    // ECG_REUSE_PLAN_DEPTH: forward schedule of the next-K absolute next-ref epochs
    // (sorted ascending) delivered alongside edge_epoch. n=0 => no schedule (inert).
    uint16_t edge_epoch_sched[4] = {0, 0, 0, 0};
    uint8_t  edge_epoch_sched_n = 0;
    uint8_t  edge_grasp_tier = 0;       // ReusePlan-carried 1/2/3 hot/moderate/cold tier
    bool     edge_grasp_tier_valid = false;

    // ECG mask encoding constants (2-bit, from ECG -M flag graphConfig.h)
    static constexpr uint8_t MASK_HOT      = 0x03;  // 11
    static constexpr uint8_t MASK_WARM     = 0x02;  // 10
    static constexpr uint8_t MASK_LUKEWARM = 0x01;  // 01
    static constexpr uint8_t MASK_COLD     = 0x00;  // 00

    // Compute mask value from tier classification (0-4) using current mask_bits
    // Maps tier 1=HOT → highest mask, tier 4=COLD → 0
    uint8_t tierToMask(uint32_t tier) const {
        if (tier == 0) return MASK_COLD;  // Unknown = cold
        uint8_t max_val = (1 << mask_bits) - 1;
        // Linearly map: tier 1 → max_val, tier 4 → 0
        if (tier >= 4) return 0;
        return static_cast<uint8_t>(max_val * (4 - tier) / 3);
    }
};

// ============================================================================
// VertexStats: Optional per-vertex cache statistics
// ============================================================================
// Enabled via enableVertexStats(). Tracks per-vertex hit/miss/reuse data
// for analysis of which vertices cause the most cache pressure.
struct VertexStats {
    std::vector<uint64_t> misses;
    std::vector<uint64_t> hits;
    std::vector<uint64_t> accesses;
    std::vector<uint64_t> reuse_distance;
    std::vector<uint64_t> last_access_time;
    bool enabled = false;

    void init(uint32_t n) {
        misses.assign(n, 0);
        hits.assign(n, 0);
        accesses.assign(n, 0);
        reuse_distance.assign(n, 0);
        last_access_time.assign(n, 0);
        enabled = true;
    }

    void recordAccess(uint32_t vertex_id, bool is_hit, uint64_t global_time) {
        if (!enabled || vertex_id >= accesses.size()) return;
        accesses[vertex_id]++;
        if (is_hit) hits[vertex_id]++;
        else misses[vertex_id]++;
        if (last_access_time[vertex_id] > 0)
            reuse_distance[vertex_id] += (global_time - last_access_time[vertex_id]);
        last_access_time[vertex_id] = global_time;
    }
};

// ============================================================================
// GraphCacheContext: The unified structure
// ============================================================================
//
// Usage in sim benchmarks:
//
//   GraphCacheContext ctx;
//   ctx.initTopology(degrees, g.num_nodes(), g.num_edges(), g.directed());
//   ctx.registerPropertyArray(scores.data(), g.num_nodes(), sizeof(float),
//                             cache.L3()->getSizeBytes());
//   ctx.registerPropertyArray(contrib.data(), g.num_nodes(), sizeof(float),
//                             cache.L3()->getSizeBytes());
//   ctx.initRereference(reref_data, num_clines, 256, g.num_nodes(), 64);
//   cache.initGraphContext(&ctx);
//
//   // In kernel loop:
//   for (NodeID u = 0; u < g.num_nodes(); u++) {
//       ctx.setCurrentVertices(u, u);
//       for (NodeID v : g.in_neigh(u)) {
//           ctx.setCurrentDst(v);
//           SIM_CACHE_READ(cache, contrib_ptr, v);
//       }
//   }
//
struct GraphCacheContext {
    // --- Property Regions (multiple tracked arrays) ---
    PropertyRegion regions[MAX_PROPERTY_REGIONS];
    uint32_t num_regions = 0;

    // --- Graph Topology (degree distribution) ---
    GraphTopology topology;

    // --- Per-Access Hints (thread-safe: one per OMP thread) ---
    // Each OMP thread gets its own AccessHints to avoid data races
    // when setting current_src/mask during parallel graph traversal.
    // Mutable because hints are updated via const GraphCacheContext* in cache.
    static constexpr int ECG_MAX_THREADS = 128;
    mutable AccessHints thread_hints[ECG_MAX_THREADS];

    // Convenience accessor: returns hints for the calling thread.
    AccessHints& hints_for_thread() {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        return thread_hints[tid < ECG_MAX_THREADS ? tid : 0];
    }
    const AccessHints& hints_for_thread() const {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        return thread_hints[tid < ECG_MAX_THREADS ? tid : 0];
    }

    // Legacy single-thread alias (for backward compatibility with non-parallel code).
    // Accesses thread_hints[0] directly. In parallel regions, use hints_for_thread().
    AccessHints& hints_thread0() { return thread_hints[0]; }

    // --- Runtime prefetch dedup window (per-thread) ---
    // Tracks recently prefetched vertex IDs to suppress duplicates.
    // When a prefetch target matches any entry in the window, it's skipped.
    static constexpr int PREFETCH_DEDUP_MAX = 16;
    struct PrefetchDedupWindow {
        uint32_t entries[PREFETCH_DEDUP_MAX] = {};
        uint8_t head = 0;
        uint8_t size = 0;
        uint8_t capacity = 8;  // configurable via prefetch_window

        bool contains(uint32_t target) const {
            for (uint8_t i = 0; i < size; i++) {
                if (entries[(head + i) % PREFETCH_DEDUP_MAX] == target) return true;
            }
            return false;
        }

        void push(uint32_t target) {
            if (size < capacity) {
                entries[(head + size) % PREFETCH_DEDUP_MAX] = target;
                size++;
            } else {
                // Overwrite oldest
                entries[head] = target;
                head = (head + 1) % PREFETCH_DEDUP_MAX;
            }
        }
    };
    mutable PrefetchDedupWindow prefetch_dedup[ECG_MAX_THREADS];

    struct ECGInstrumentation {
        uint64_t mask_build_us = 0;
        uint64_t mask_vertices = 0;
        uint64_t pfx_candidates = 0;
        uint64_t pfx_encoded = 0;
        uint64_t pfx_no_candidate = 0;
        uint64_t pfx_table_miss = 0;
        uint64_t pfx_dedup_skips = 0;
        mutable std::atomic<uint64_t> runtime_pfx_no_target{0};
        mutable std::atomic<uint64_t> runtime_pfx_duplicate{0};
        mutable std::atomic<uint64_t> runtime_pfx_issued{0};

        void resetBuild() {
            mask_build_us = 0;
            mask_vertices = 0;
            pfx_candidates = 0;
            pfx_encoded = 0;
            pfx_no_candidate = 0;
            pfx_table_miss = 0;
            pfx_dedup_skips = 0;
            runtime_pfx_no_target.store(0);
            runtime_pfx_duplicate.store(0);
            runtime_pfx_issued.store(0);
        }
    };
    mutable ECGInstrumentation ecg_stats;

    PrefetchDedupWindow& dedup_for_thread() const {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        return prefetch_dedup[tid < ECG_MAX_THREADS ? tid : 0];
    }

    void recordPrefetchNoTarget() const { ecg_stats.runtime_pfx_no_target++; }
    void recordPrefetchDuplicate() const { ecg_stats.runtime_pfx_duplicate++; }
    void recordPrefetchIssued() const { ecg_stats.runtime_pfx_issued++; }

    // --- Rereference Matrix (P-OPT) ---
    RereferenceConfig rereference;

    // --- Exact position-indexed next-reference (ECG per-edge idea) ---
    // The per-edge mask is traversed in order, so the CURRENT vertex (src) is
    // the epoch — for free. Instead of P-OPT's quantized [epoch×line] matrix or
    // an epoch-AVERAGED mask scalar, store the graph's out-adjacency and compute
    // the EXACT next-reference at eviction: for a property line (16 vertices),
    // next_ref(line | src) = min over v in line of (next w in out_neigh(v) with
    // w > src) - src. Memory-resident (sorted out-CSR copy), 0 LLC ways,
    // quantization-free. Used by ECGMode::ECG_EXACT.
    std::vector<int64_t> exact_off;    // CSR offsets (num_vertices+1)
    std::vector<int32_t> exact_nbr;    // sorted out-neighbors
    uint32_t exact_nv = 0;
    uint32_t exact_vtx_per_line = 16;
    uint32_t exact_bits = 0;           // 0 = full precision; else log2-quantize distance to B bits
    uint32_t edge_epoch_count = 32;    // ECG_GRASP_POPT: # absolute epochs the per-edge mask quantizes to (<=128 for 7-bit field)

    // --- BFS-order EXACT (the traversal-order mask generator for the BFS access class) ---
    // A dedicated, no-full-kernel-run generator (analogous to makeOffsetMatrix, but in
    // BFS-visit order instead of ID order): run a plain BFS skeleton from the kernel's
    // source -> visit_pos[v]; property line(v) is referenced when v's in-neighbour is
    // processed, so next-reference uses the in-neighbours' visit positions. This is what
    // lets the structural generator be correct for frontier kernels (bfs/bc) where the
    // ID-order sweep generator fails.
    std::vector<uint32_t> visit_pos;     // BFS visit order; UINT32_MAX = unvisited
    std::vector<int64_t>  bfs_in_off;    // in-CSR offsets (num_vertices+1)
    std::vector<uint32_t> bfs_in_vpos;   // per-vertex sorted in-neighbour visit positions
    bool exact_bfs = false;              // dispatch exactNextRef -> BFS-order variant

    // Community-aware seeding variant of the bounded order: instead of pure degree,
    // seed BFS balls from CLUSTER representatives discovered by lightweight label
    // propagation (each vertex adopts its max-degree neighbour's representative — one
    // round, O(V+E), no external partitioner). Seeds = cluster reps ordered by cluster
    // size (largest community first), then depth-bounded BFS balls fill positions. Aims
    // to generalise the bounded win beyond web/social by using COMMUNITY locality rather
    // than raw degree. Source-independent, all-nodes, no replay.
    template<typename GraphT>
    void buildBoundedBFSOrderCommunity(const GraphT& g, uint32_t max_depth) {
        uint32_t n = g.num_nodes();
        visit_pos.assign(n, UINT32_MAX);
        // One round of "adopt max-degree neighbour" -> local star clusters.
        std::vector<uint32_t> rep(n);
        #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1024)
        #endif
        for (int64_t v = 0; v < (int64_t)n; ++v) {
            uint32_t best = (uint32_t)v; uint64_t bestdeg = g.out_degree(v);
            for (auto w : g.out_neigh(v)) {
                uint64_t dw = g.out_degree((uint32_t)w);
                if (dw > bestdeg || (dw == bestdeg && (uint32_t)w < best)) {
                    bestdeg = dw; best = (uint32_t)w;
                }
            }
            rep[v] = best;   // local representative (a hub or self)
        }
        // Cluster size by representative.
        std::vector<uint32_t> csize(n, 0);
        for (uint32_t v = 0; v < n; ++v) csize[rep[v]]++;
        // Seeds = representatives ordered by cluster size desc (largest community first).
        std::vector<uint32_t> seeds(n);
        for (uint32_t v = 0; v < n; ++v) seeds[v] = v;
        std::sort(seeds.begin(), seeds.end(), [&](uint32_t a, uint32_t b) {
            if (csize[a] != csize[b]) return csize[a] > csize[b];
            return g.out_degree(a) > g.out_degree(b);
        });
        std::vector<uint32_t> q, qd;
        q.reserve(1024); qd.reserve(1024);
        uint32_t order = 0;
        for (uint32_t si = 0; si < n; ++si) {
            uint32_t seed = seeds[si];
            if (visit_pos[seed] != UINT32_MAX) continue;
            q.clear(); qd.clear();
            visit_pos[seed] = order++;
            q.push_back(seed); qd.push_back(0);
            for (size_t head = 0; head < q.size(); ++head) {
                uint32_t u = q[head], du = qd[head];
                if (du >= max_depth) continue;
                for (auto w : g.out_neigh(u)) {
                    if (visit_pos[(uint32_t)w] == UINT32_MAX) {
                        visit_pos[(uint32_t)w] = order++;
                        q.push_back((uint32_t)w); qd.push_back(du + 1);
                    }
                }
            }
        }
    }

    // DEPTH-BOUNDED, degree-seeded multi-source clustering order (the user's idea for a
    // SOURCE-INDEPENDENT, all-nodes, no-replay frontier clock): seed from the highest-
    // degree unvisited node, BFS outward until depth `max_depth` (a bounded ball /
    // frontier around the hub), assign visited nodes consecutive positions, then jump to
    // the next unvisited highest-degree node. Repeat until ALL nodes placed. Seeds go
    // high-degree -> low-degree (hubs anchor their neighbourhoods first). Graph-local
    // vertices get nearby positions WITHOUT replaying any single source's full traversal.
    template<typename GraphT>
    void buildBoundedBFSOrder(const GraphT& g, uint32_t max_depth) {
        uint32_t n = g.num_nodes();
        visit_pos.assign(n, UINT32_MAX);
        std::vector<uint32_t> seeds(n);
        for (uint32_t v = 0; v < n; ++v) seeds[v] = v;
        std::sort(seeds.begin(), seeds.end(), [&g](uint32_t a, uint32_t b) {
            return g.out_degree(a) > g.out_degree(b);   // high-degree -> low-degree
        });
        std::vector<uint32_t> q;             // (vertex) queue
        std::vector<uint32_t> qd;            // parallel depth queue
        q.reserve(1024); qd.reserve(1024);
        uint32_t order = 0;
        for (uint32_t si = 0; si < n; ++si) {
            uint32_t seed = seeds[si];
            if (visit_pos[seed] != UINT32_MAX) continue;
            q.clear(); qd.clear();
            visit_pos[seed] = order++;
            q.push_back(seed); qd.push_back(0);
            for (size_t head = 0; head < q.size(); ++head) {
                uint32_t u = q[head], du = qd[head];
                if (du >= max_depth) continue;           // bounded frontier depth
                for (auto w : g.out_neigh(u)) {
                    if (visit_pos[(uint32_t)w] == UINT32_MAX) {
                        visit_pos[(uint32_t)w] = order++;
                        q.push_back((uint32_t)w); qd.push_back(du + 1);
                    }
                }
            }
        }
    }

    // K-SOURCE EXPECTED-REUSE clock (the principled source-independent frontier mask):
    // estimate each vertex's EXPECTED BFS visit position under a random source by
    // averaging its normalised visit rank over K random training-source BFS traversals
    // (Monte-Carlo expectation), then assign visit_pos = argsort(expected rank). This is
    // a consensus visit order that transfers to UNSEEN sources. Source-independent (no
    // single source), all-nodes (forest per source), deployable (precomputed once).
    // rng_seed fixes the K training sources (held-out test source excluded by the caller).
    template<typename GraphT>
    void buildBFSVisitOrderKSource(const GraphT& g, uint32_t K, uint32_t rng_seed) {
        uint32_t n = g.num_nodes();
        std::vector<double> acc(n, 0.0);          // sum of normalised ranks
        std::vector<uint32_t> vp(n), q(n);
        std::mt19937 rng(rng_seed);
        std::uniform_int_distribution<uint32_t> pick(0, n ? n - 1 : 0);
        for (uint32_t k = 0; k < K; ++k) {
            uint32_t src = pick(rng);
            std::fill(vp.begin(), vp.end(), UINT32_MAX);
            uint32_t order = 0; size_t qh = 0, qt = 0;
            auto bfs_from = [&](uint32_t s) {
                vp[s] = order++; q[qt++] = s;
                while (qh < qt) {
                    uint32_t u = q[qh++];
                    for (auto w : g.out_neigh(u))
                        if (vp[(uint32_t)w] == UINT32_MAX) { vp[(uint32_t)w] = order++; q[qt++] = (uint32_t)w; }
                }
            };
            qh = qt = 0;
            bfs_from(src);
            for (uint32_t v = 0; v < n; ++v) if (vp[v] == UINT32_MAX) bfs_from(v);  // forest
            double inv = (n > 1) ? 1.0 / (double)(n - 1) : 0.0;
            for (uint32_t v = 0; v < n; ++v) acc[v] += (double)vp[v] * inv;          // normalised rank
        }
        std::vector<uint32_t> idx(n);
        for (uint32_t v = 0; v < n; ++v) idx[v] = v;
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
            return acc[a] < acc[b] || (acc[a] == acc[b] && a < b);
        });
        visit_pos.assign(n, 0);
        for (uint32_t r = 0; r < n; ++r) visit_pos[idx[r]] = r;
    }

    // Plain BFS skeleton, extended to a FOREST so EVERY node gets a visit position
    // (not just the source's reachable set): BFS from `source` first (correct order for
    // its component), then continue from each remaining unvisited node. O(V+E).
    template<typename GraphT>
    void buildBFSVisitOrder(const GraphT& g, uint32_t source) {
        uint32_t n = g.num_nodes();
        visit_pos.assign(n, UINT32_MAX);
        std::vector<uint32_t> q;
        q.reserve(n);
        uint32_t order = 0;
        auto bfs_from = [&](uint32_t s) {
            visit_pos[s] = order++;
            size_t qstart = q.size();
            q.push_back(s);
            for (size_t head = qstart; head < q.size(); ++head) {
                uint32_t u = q[head];
                for (auto w : g.out_neigh(u)) {
                    if (visit_pos[(uint32_t)w] == UINT32_MAX) {
                        visit_pos[(uint32_t)w] = order++;
                        q.push_back((uint32_t)w);
                    }
                }
            }
        };
        if (source < n) bfs_from(source);          // kernel's component first
        for (uint32_t v = 0; v < n; ++v)            // forest: mark ALL remaining nodes
            if (visit_pos[v] == UINT32_MAX) bfs_from(v);
    }

    // Depth-order variant (parallel-friendly): rank nodes by BFS DEPTH (level), ties by
    // id. Depth is well-defined independent of thread order, so a level-synchronous
    // PARALLEL BFS computes it deterministically -> the construction parallelizes. Tests
    // whether the level (not exact FIFO within-level order) is the signal the mask needs.
    template<typename GraphT>
    void buildBFSVisitOrderByDepth(const GraphT& g, uint32_t source) {
        uint32_t n = g.num_nodes();
        visit_pos.assign(n, UINT32_MAX);
        std::vector<uint32_t> depth(n, UINT32_MAX);
        std::vector<uint32_t> q;
        q.reserve(n);
        auto bfs_from = [&](uint32_t s, uint32_t base) {
            depth[s] = base;
            size_t qs = q.size();
            q.push_back(s);
            for (size_t h = qs; h < q.size(); ++h) {
                uint32_t u = q[h];
                for (auto w : g.out_neigh(u))
                    if (depth[(uint32_t)w] == UINT32_MAX) {
                        depth[(uint32_t)w] = depth[u] + 1;
                        q.push_back((uint32_t)w);
                    }
            }
        };
        if (source < n) bfs_from(source, 0);
        uint32_t maxd = 0;
        for (uint32_t v = 0; v < n; ++v)
            if (depth[v] != UINT32_MAX && depth[v] > maxd) maxd = depth[v];
        for (uint32_t v = 0; v < n; ++v)             // forest: cover all nodes
            if (depth[v] == UINT32_MAX) bfs_from(v, maxd + 1);
        if (std::getenv("ECG_BFS_DEPTHSTATS")) {
            uint32_t md = 0;
            for (uint32_t v = 0; v < n; ++v)
                if (depth[v] != UINT32_MAX && depth[v] < (maxd + 1) && depth[v] > md) md = depth[v];
            std::vector<uint64_t> hist(md + 2, 0);
            for (uint32_t v = 0; v < n; ++v)
                if (depth[v] <= md) hist[depth[v]]++;
            uint64_t maxlvl = 0; uint32_t argmax = 0;
            for (uint32_t d = 0; d <= md; ++d) if (hist[d] > maxlvl) { maxlvl = hist[d]; argmax = d; }
            std::fprintf(stderr, "[BFS_DEPTH] n=%u source-component max_depth(levels)=%u "
                         "fattest_level=%llu (at depth %u) avg_level=%.0f\n",
                         n, md, (unsigned long long)maxlvl, argmax,
                         (double)n / (double)(md + 1));
        }
        std::vector<uint32_t> idx(n);
        for (uint32_t v = 0; v < n; ++v) idx[v] = v;
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
            return depth[a] < depth[b] || (depth[a] == depth[b] && a < b);
        });
        for (uint32_t r = 0; r < n; ++r) visit_pos[idx[r]] = r;
    }

    // Build per-vertex sorted in-neighbour visit positions (for exactNextRefBFS).
    template<typename GraphT>
    void registerInAdjacencyExactBFS(const GraphT& g) {
        uint32_t n = g.num_nodes();
        exact_nv = n;
        bfs_in_off.assign((size_t)n + 1, 0);
        for (uint32_t v = 0; v < n; ++v)
            bfs_in_off[v + 1] = bfs_in_off[v] + g.in_degree(v);
        bfs_in_vpos.assign((size_t)bfs_in_off[n], UINT32_MAX);
        #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 256)
        #endif
        for (int64_t v = 0; v < (int64_t)n; ++v) {
            int64_t p = bfs_in_off[v];
            for (auto u : g.in_neigh(v)) bfs_in_vpos[p++] = visit_pos[(uint32_t)u];
            std::sort(bfs_in_vpos.begin() + bfs_in_off[v], bfs_in_vpos.begin() + bfs_in_off[v + 1]);
        }
        exact_bfs = true;
    }

    // Exact next-reference in BFS-visit order: line(v) is next referenced when v's next
    // in-neighbour (by visit position > current) is processed. Mirrors exactNextRef but
    // the clock is the BFS visit index, not the vertex ID.
    uint32_t exactNextRefBFS(uint64_t line_addr, uint32_t current_vertex) const {
        if (bfs_in_off.empty() || current_vertex >= exact_nv) return UINT32_MAX;
        uint32_t cur = visit_pos[current_vertex];
        if (cur == UINT32_MAX) return UINT32_MAX;  // current vertex not visited -> no info
        const PropertyRegion* r = findRegion(line_addr);
        if (r == nullptr) return UINT32_MAX;
        uint32_t cline = static_cast<uint32_t>((line_addr - r->base_address) / rereference.line_size);
        uint32_t v0 = cline * exact_vtx_per_line;
        uint32_t v1 = v0 + exact_vtx_per_line;
        if (v1 > exact_nv) v1 = exact_nv;
        uint32_t best = UINT32_MAX;
        for (uint32_t v = v0; v < v1; ++v) {
            int64_t lo = bfs_in_off[v], hi = bfs_in_off[v + 1];
            while (lo < hi) {  // first in-neighbour visit position strictly > cur
                int64_t mid = (lo + hi) >> 1;
                if (bfs_in_vpos[mid] > cur) hi = mid; else lo = mid + 1;
            }
            if (lo < bfs_in_off[v + 1]) {
                uint32_t vp = bfs_in_vpos[lo];
                if (vp != UINT32_MAX) {
                    uint32_t d = vp - cur;
                    if (d < best) best = d;
                }
            }
        }
        if (best == UINT32_MAX) return UINT32_MAX;
        if (exact_bits > 0) {
            uint32_t q = 0, x = best;
            while (x > 0) { q++; x >>= 1; }
            uint32_t qmax = (1u << exact_bits) - 1;
            return (q > qmax) ? qmax : q;
        }
        return best;
    }

    template<typename GraphT>
    void registerOutAdjacencyExact(const GraphT& g) {
        uint32_t n = g.num_nodes();
        exact_nv = n;
        exact_off.assign((size_t)n + 1, 0);
        for (uint32_t v = 0; v < n; ++v)
            exact_off[v + 1] = exact_off[v] + g.out_degree(v);
        exact_nbr.assign((size_t)exact_off[n], 0);
        #ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 256)
        #endif
        for (int64_t v = 0; v < (int64_t)n; ++v) {
            int64_t p = exact_off[v];
            for (auto w : g.out_neigh(v)) exact_nbr[p++] = static_cast<int32_t>(w);
            std::sort(exact_nbr.begin() + exact_off[v], exact_nbr.begin() + exact_off[v + 1]);
        }
    }

    // Exact next-reference distance (in vertices) for a property line, given the
    // current traversal vertex (src). Returns a large sentinel if never re-read.
    uint32_t exactNextRef(uint64_t line_addr, uint32_t src) const {
        if (exact_off.empty() || rereference.line_size == 0) return UINT32_MAX;
        const PropertyRegion* r = findRegion(line_addr);
        if (r == nullptr) return UINT32_MAX;
        uint32_t cline = static_cast<uint32_t>((line_addr - r->base_address) / rereference.line_size);
        uint32_t v0 = cline * exact_vtx_per_line;
        uint32_t v1 = v0 + exact_vtx_per_line;
        if (v1 > exact_nv) v1 = exact_nv;
        uint32_t best = UINT32_MAX;
        for (uint32_t v = v0; v < v1; ++v) {
            int64_t lo = exact_off[v], hi = exact_off[v + 1];
            // first neighbor strictly greater than src (neighbors sorted asc)
            while (lo < hi) {
                int64_t mid = (lo + hi) >> 1;
                if ((uint32_t)exact_nbr[mid] > src) hi = mid; else lo = mid + 1;
            }
            if (lo < exact_off[v + 1]) {
                uint32_t d = (uint32_t)exact_nbr[lo] - src;
                if (d < best) best = d;
            }
        }
        if (best == UINT32_MAX) return UINT32_MAX;
        if (exact_bits > 0) {
            // log2-quantize the exact distance into B bits (mask-storable):
            // q = min(floor(log2(d+1)), 2^B - 1). Position stays exact; only the
            // distance MAGNITUDE is coarsened, to test how few bits the win needs.
            uint32_t q = 0, x = best;
            while (x > 0) { q++; x >>= 1; }   // q = floor(log2(best))+1 ~ bit-length
            uint32_t qmax = (1u << exact_bits) - 1;
            return (q > qmax) ? qmax : q;
        }
        return best;
    }


    // --- Prefetch Matrix (ECG-style graph-aware prefetching) ---
    // Maps each vertex to its predicted next-access vertex.
    // When a vertex v is accessed, prefetch data for prefetch_map[v].
    // Built from graph transpose (similar to P-OPT but for prefetching).
    // nullptr = prefetching disabled.
    const uint32_t* prefetch_map = nullptr;
    uint32_t prefetch_num_vertices = 0;

    // --- Fat ID Configuration (ECG adaptive encoding) ---
    FatIDConfig fat_id;

    // --- ECG Mask Configuration + Array ---
    // Parallel mask array alongside CSR edges for per-edge cache hints.
    // Decouples degree classification from vertex ordering.
    MaskConfig mask_config;
    MaskArray  mask_array;
    std::vector<uint32_t> hot_table;  // Hot vertex table for indexed prefetch

    // Per-edge ECG mask used by the request-carried design.
    // Stored as per-src vector of 64-bit packed masks parallel to that src's
    // in_neigh list. Built once per graph by buildInEdgeMasks_PR(). Mode 6
    // ("per_edge") in pr.cc reads from this.
    //
    // Per-mask 64-bit layout (for graphs with up to 2^24 vertices):
    //   [0:24]  = dest_id (decoded by mask, replaces direct CSR read)
    //   [24:26] = DBG eviction tier
    //   [26:33] = POPT reuse quantization (7 bits)
    //   [33:64] = prefetch target vertex ID (31 bits)
    std::vector<std::vector<uint64_t>> in_edge_masks_by_src;

    // ECG_GRASP_POPT: parallel per-edge ABSOLUTE next-ref epoch (full resolution,
    // not capped by the 7-bit mask POPT field). Filled by buildInEdgeMasks_PR when
    // ECG_EDGE_MASK_EPOCH is set; the kernel carries it on the demand via
    // AccessHints::edge_epoch so the cache can use >128 epochs.
    std::vector<std::vector<uint16_t>> in_edge_epoch_by_src;

    // ECG_REUSE_PLAN_DEPTH=K: parallel per-edge forward SCHEDULE — the next-K absolute
    // next-ref epochs of dest's line (sorted ascending), flattened K-per-edge so edge i
    // occupies [i*K, i*K+K). Lets a resident line self-advance across epochs (matrix-like)
    // without a reserved way. Built by buildInEdgeMasks_PR when ECG_REUSE_PLAN_DEPTH set.
    std::vector<std::vector<uint16_t>> in_edge_epoch_sched_by_src;
    std::vector<std::vector<uint8_t>> in_edge_grasp_tier_by_src;
    uint32_t edge_epoch_reuse_plan_depth = 0;  // K (0 = disabled / single-epoch legacy path)

    // === OUT-edge per-edge masks (dual-direction capability) ===
    // The mirror of in_edge_masks_by_src for kernels that traverse the OUT edge
    // list (BFS top-down push, SSSP/BC push). Each entry out_edge_masks_by_src[src]
    // is in g.out_neigh(src) order. The epoch is the transpose of OUT traversal:
    // a property[dest] read while pushing src->dest is next read at the next
    // IN-neighbour of dest > src, so the epoch is computed from g.in_neigh (held in
    // exact_in_off/exact_in_nbr below), NOT g.out_neigh. Self-contained: built by
    // buildOutEdgeMasks(), never touches the PR in-edge path or the shared
    // exact_off/exact_nbr. On symmetric graphs (in==out) these equal the in-edge
    // masks. The two builders remain independent so direction-specific state
    // cannot overwrite the PageRank in-edge path.
    std::vector<std::vector<uint64_t>> out_edge_masks_by_src;
    std::vector<std::vector<uint16_t>> out_edge_epoch_by_src;
    std::vector<std::vector<uint16_t>> out_edge_epoch_sched_by_src;
    std::vector<std::vector<uint8_t>> out_edge_grasp_tier_by_src;
    // IN-adjacency (sorted) used as the next-ref source for OUT-edge epochs —
    // separate from exact_off/exact_nbr (which hold the OUT-adjacency for in-edge
    // masks) so the two directions never clobber each other.
    std::vector<int64_t> exact_in_off;
    std::vector<int32_t> exact_in_nbr;
    uint32_t exact_in_nv = 0;

    ~GraphCacheContext() {
        if (mask_config.enabled) printECGStats();
    }

    // --- Per-Vertex Statistics (optional) ---
    VertexStats vertex_stats;

    // ================================================================
    // Registration Functions
    // ================================================================

    // Register graph topology from degree array.
    // Needed by: GRASP-XP (degree-bucket RRPV), auto hot-fraction computation.
    void initTopology(const uint32_t* degrees, uint32_t num_vertices,
                      uint64_t num_edges, bool directed) {
        topology.computeFromDegrees(degrees, num_vertices, num_edges, directed);
    }

    // Register a property array with auto-computed hot/warm boundaries.
    //
    // Hot fraction is auto-computed from degree distribution if topology is
    // initialized (preferred), otherwise falls back to LLC capacity ratio.
    //
    // Auto-computation logic:
    //   1. How many vertex elements fit in this array's share of the LLC?
    //      vtx_per_llc = (llc_size / num_regions) / elem_size
    //   2. If topology available: what fraction of vertices covers 50% of edges?
    //      (power-law: 1% of vertices may hold 50% of edges)
    //   3. hot_fraction = min(vtx_per_llc / num_vertices, degree_cutoff)
    //
    // This makes hot/warm boundaries self-tuning based on graph properties
    // instead of requiring manual parameter tuning (GRASP's key weakness).
    //
    // Region boundary computation (DBG bucket-aligned):
    //   1. Compute how many vertex elements fit in LLC share for this array
    //      (cache_capacity_vertices = LLC_share / elem_size)
    //   2. Walk DBG degree buckets from highest to lowest, accumulating
    //      vertex counts until cache_capacity_vertices is reached
    //   3. That bucket boundary becomes hot_bound
    //   4. Continue for WARM (2× capacity) and LUKEWARM (4× capacity)
    //   5. Boundaries align with DBG bucket boundaries so region cuts
    //      don't split within a degree group
    //
    // Requires: initTopology() called before registerPropertyArray()
    //           Graph must be DBG-reordered for regions to be meaningful
    void registerPropertyArray(const void* data_ptr, uint32_t num_elements,
                               uint32_t elem_size, size_t llc_size,
                               double manual_hot_fraction = -1.0,
                               bool grasp_region = true) {
        if (num_regions >= MAX_PROPERTY_REGIONS) return;

        PropertyRegion& r = regions[num_regions];
        r.base_address = reinterpret_cast<uint64_t>(data_ptr);
        r.upper_bound = r.base_address + static_cast<uint64_t>(num_elements) * elem_size;
        r.num_elements = num_elements;
        r.elem_size = elem_size;
        r.region_id = num_regions;
        r.grasp_region = grasp_region;
        if (manual_hot_fraction > 0.0 && manual_hot_fraction <= 1.0) {
            r.grasp_hot_percent = static_cast<uint32_t>(manual_hot_fraction * 100.0 + 0.5);
        } else {
            // GRASP hot region = frontier_frac as fraction of the VERTEX SPACE
            // (array-relative, GRASP-faithful + auto-scaling). ~0.15 reproduces
            // the Faldu corpus and scales. Override via GRASP_HOT_FRACTION.
            const char* e = std::getenv("GRASP_HOT_FRACTION");
            double f = e ? std::atof(e) : 0.15;
            if (f <= 0.0 || f > 1.0) f = 0.15;
            r.grasp_hot_percent = static_cast<uint32_t>(f * 100.0 + 0.5);
        }

        constexpr uint64_t LINE_MASK = ~uint64_t(63);

        if (topology.num_vertices > 0 && elem_size > 0) {
            // N-bucket boundaries aligned with DBG degree buckets.
            // Each bucket boundary marks where the next degree group starts.
            // DBG order: highest-degree at front (bucket N-1 → position 0).
            uint32_t active_buckets = 0;
            uint64_t cumulative_vtx = 0;

            // Walk from highest-degree bucket to lowest, accumulating vertex counts
            for (int b = GraphTopology::NUM_BUCKETS - 1; b >= 0; --b) {
                uint64_t count = topology.bucket_counts[b];
                if (count == 0) continue;  // Skip empty buckets
                cumulative_vtx += count;
                if (active_buckets < MAX_REGION_BUCKETS) {
                    uint64_t bound_bytes = cumulative_vtx * elem_size;
                    r.bucket_bounds[active_buckets] = (r.base_address + bound_bytes + 63) & LINE_MASK;
                    if (r.bucket_bounds[active_buckets] > r.upper_bound)
                        r.bucket_bounds[active_buckets] = r.upper_bound;
                    active_buckets++;
                }
            }
            r.num_buckets = active_buckets;

            // Ensure last bucket covers entire array
            if (active_buckets > 0)
                r.bucket_bounds[active_buckets - 1] = r.upper_bound;
        } else if (manual_hot_fraction > 0.0 && manual_hot_fraction <= 1.0) {
            // Manual: 4 buckets with geometric sizing
            uint64_t array_bytes = static_cast<uint64_t>(num_elements) * elem_size;
            uint64_t hot = static_cast<uint64_t>(manual_hot_fraction * array_bytes);
            r.bucket_bounds[0] = (r.base_address + hot + 63) & LINE_MASK;
            r.bucket_bounds[1] = (r.base_address + hot * 4 + 63) & LINE_MASK;
            r.bucket_bounds[2] = (r.base_address + hot * 16 + 63) & LINE_MASK;
            r.bucket_bounds[3] = r.upper_bound;
            for (int i = 0; i < 4; ++i)
                if (r.bucket_bounds[i] > r.upper_bound) r.bucket_bounds[i] = r.upper_bound;
            r.num_buckets = 4;
        } else {
            // Fallback: equal thirds (3 buckets)
            uint64_t array_bytes = static_cast<uint64_t>(num_elements) * elem_size;
            r.bucket_bounds[0] = (r.base_address + array_bytes / 3 + 63) & LINE_MASK;
            r.bucket_bounds[1] = (r.base_address + 2 * array_bytes / 3 + 63) & LINE_MASK;
            r.bucket_bounds[2] = r.upper_bound;
            r.num_buckets = 3;
        }

        logGraphCtxRegistration("cache_sim", nullptr,
                                r.base_address, r.upper_bound,
                                r.grasp_hot_percent, r.grasp_region);
        num_regions++;
    }

    // Register a GRASP trace header property region directly.  The official
    // GRASP trace format carries propertyA/propertyB base/end addresses plus
    // an `f` percentage of LLC capacity used for high-reuse classification.
    void registerGRASPTraceRegion(uint64_t base_address, uint64_t upper_bound,
                                  uint32_t hot_percent) {
        if (num_regions >= MAX_PROPERTY_REGIONS || base_address >= upper_bound) return;
        PropertyRegion& r = regions[num_regions];
        r.base_address = base_address;
        r.upper_bound = upper_bound;
        r.num_elements = static_cast<uint32_t>(std::min<uint64_t>(upper_bound - base_address, UINT32_MAX));
        r.elem_size = 1;
        r.region_id = num_regions;
        r.num_buckets = 0;
        r.grasp_hot_percent = hot_percent;
        r.grasp_region = true;
        logGraphCtxRegistration("cache_sim_trace", nullptr,
                                r.base_address, r.upper_bound,
                                r.grasp_hot_percent, r.grasp_region);
        num_regions++;
    }

    // Register rereference matrix for P-OPT.
    // Call after makeOffsetMatrix() from popt.h.
    void initRereference(const uint8_t* matrix, uint32_t num_cache_lines,
                         uint32_t num_epochs, uint32_t num_vertices,
                         uint32_t line_size) {
        rereference.matrix = matrix;
        rereference.num_cache_lines = num_cache_lines;
        rereference.num_epochs = num_epochs;
        rereference.epoch_size = (num_vertices + num_epochs - 1) / num_epochs;
        rereference.sub_epoch_size = (rereference.epoch_size + 127) / 128;
        rereference.line_size = line_size;
        rereference._pad = 0;
    }

    // Real-time per-direction reref load: repoint the single reserved reref way at a
    // pre-built matrix of the SAME dims (same graph -> same num_cache_lines/epochs/
    // sizes). Lets a direction-optimizing kernel swap the transpose-correct matrix per
    // phase WITHOUT reserving a second LLC way (POPT_DUAL_REREF). The matrix is
    // non-owned, so this is a pointer swap; do it between phases (no active parallel
    // region). This preserves one reserved rereference way while allowing
    // direction-specific matrices.
    uint64_t reref_swap_count = 0;  // # real-time per-direction loads (observability)
    inline void setActiveRerefMatrix(const uint8_t* matrix) {
        if (rereference.matrix != matrix) reref_swap_count++;
        rereference.matrix = matrix;
    }

    // Enable per-vertex statistics tracking.
    void enableVertexStats() {
        if (topology.num_vertices > 0) {
            vertex_stats.init(topology.num_vertices);
        }
    }

    // Register prefetch matrix for ECG-style graph-aware prefetching.
    // The map stores: prefetch_map[v] = vertex whose data to prefetch
    // when vertex v is accessed. Built from graph transpose.
    void initPrefetchMap(const uint32_t* map, uint32_t num_vertices) {
        prefetch_map = map;
        prefetch_num_vertices = num_vertices;
    }

    // Get prefetch target for a vertex (UINT32_MAX if none)
    uint32_t getPrefetchTarget(uint32_t vertex_id) const {
        if (!prefetch_map || vertex_id >= prefetch_num_vertices) return UINT32_MAX;
        return prefetch_map[vertex_id];
    }

    // Initialize fat-ID encoding for ECG-style embedded hints.
    // Call after initTopology(). Computes adaptive bit allocation.
    void initFatID() {
        if (topology.num_vertices > 0) {
            fat_id.computeFromGraph(topology.num_vertices);
        }
    }

    // Encode a neighbor ID with all metadata fields.
    // Called during preprocessing (once per edge).
    uint64_t encodeFatNeighbor(uint64_t real_id, uint32_t degree,
                                uint32_t rereference_distance) const {
        if (!fat_id.enabled) return real_id;
        uint8_t dbg_tier = fat_id.computeDBGTier(degree, topology.avg_degree);
        uint8_t popt_q = fat_id.quantizeRereference(rereference_distance);
        uint8_t pfx_d = 0;  // Prefetch delta: placeholder for future
        return fat_id.encode(real_id, dbg_tier, popt_q, pfx_d);
    }

    // Decode a fat neighbor ID — extract just the real vertex ID.
    // Called at every neighbor access in the algorithm.
    uint64_t decodeFatNeighbor(uint64_t fat) const {
        if (!fat_id.enabled) return fat;
        return fat_id.extractRealID(fat);
    }

    // Extract all fields from a fat neighbor ID at once.
    // Used by the ECG cache policy for making replacement decisions.
    void decodeFatFull(uint64_t fat, uint64_t& real_id, uint8_t& dbg_tier,
                       uint8_t& popt_q, uint8_t& pfx_delta) const {
        real_id = fat_id.extractRealID(fat);
        dbg_tier = fat_id.extractDBGTier(fat);
        popt_q = fat_id.extractPOPT(fat);
        pfx_delta = fat_id.extractPrefetch(fat);
    }

    // ================================================================
    // ECG Mask Array Initialization
    // ================================================================

    // Initialize mask configuration from environment variables and graph size.
    // If ECG_MASK_WIDTH is not set, dynamically determines available bits
    // from the edge list data type width (default 32) minus vertex ID bits.
    // Call after initTopology().
    void initMaskConfig() {
        mask_config.initFromEnv();
        if (topology.num_vertices > 0) {
            // If mask_width not explicitly set, compute from vertex count
            // Default emulates 32-bit edge list: spare = 32 - ceil(log2(N))
            const char* v_width = std::getenv("ECG_MASK_WIDTH");
            if (!v_width) {
                // Compute how many bits the vertex ID needs
                uint8_t id_bits = 1;
                uint32_t n = topology.num_vertices;
                while ((1ULL << id_bits) < n) id_bits++;
                // Default 32-bit edge list data type → spare bits for ECG
                uint8_t container = 32;
                const char* v_container = std::getenv("ECG_CONTAINER_BITS");
                if (v_container) container = static_cast<uint8_t>(std::atoi(v_container));
                mask_config.mask_width = (container > id_bits) ? (container - id_bits) : 2;
            }
            mask_config.autoAllocate(topology.num_vertices);
            for (auto& window : prefetch_dedup) {
                window.capacity = mask_config.prefetch_window;
                window.size = 0;
                window.head = 0;
            }

            // Print the allocation for debugging
            std::cout << "ECG Mask: " << int(mask_config.mask_width) << "-bit"
                      << " [DBG=" << int(mask_config.dbg_bits)
                      << " POPT=" << int(mask_config.popt_bits)
                      << " PFX=" << int(mask_config.prefetch_bits) << "]"
                      << " pfx_direct=" << mask_config.prefetch_direct
                      << " hot_table=" << mask_config.hot_table_size
                      << " mode=" << ECGModeToString(mask_config.ecg_mode)
                      << " pfx_mode=" << int(mask_config.prefetch_mode)
                      << std::endl;
        }
    }

    // Register a pre-computed mask array (per-edge or per-vertex).
    // The caller owns the data; the context stores a non-owning view.
    void initMaskArray8(const uint8_t* data, uint64_t count) {
        mask_array.init8(data, count);
    }
    void initMaskArray16(const uint16_t* data, uint64_t count) {
        mask_array.init16(data, count);
    }
    void initMaskArray32(const uint32_t* data, uint64_t count) {
        mask_array.init32(data, count);
    }

    // Build hot table for indexed prefetch targets (sorted by degree descending).
    // Only needed when prefetch_bits < id_bits (small mask width, large graph).
    void buildHotTable(const uint32_t* degrees, uint32_t num_vertices) {
        if (mask_config.hot_table_size == 0) return;
        // Collect (degree, vertex_id) pairs, sort descending
        std::vector<std::pair<uint32_t, uint32_t>> dv(num_vertices);
        for (uint32_t i = 0; i < num_vertices; ++i)
            dv[i] = {degrees[i], i};
        std::partial_sort(dv.begin(),
                          dv.begin() + std::min(uint32_t(mask_config.hot_table_size), num_vertices),
                          dv.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        hot_table.resize(std::min(uint32_t(mask_config.hot_table_size), num_vertices));
        for (size_t i = 0; i < hot_table.size(); ++i)
            hot_table[i] = dv[i].second;
    }

    // Get mask entry for an edge (or vertex in per-vertex mode).
    uint32_t getMaskEntry(uint64_t edge_or_vertex_idx) const {
        if (!mask_array.enabled) return 0;
        return mask_array.get(edge_or_vertex_idx);
    }

    // Decode a mask entry into its components.
    void decodeMask(uint32_t entry, uint8_t& dbg_tier, uint8_t& popt_quant,
                    uint32_t& prefetch_target) const {
        dbg_tier = mask_config.decodeDBG(entry);
        popt_quant = mask_config.decodePOPT(entry);
        prefetch_target = mask_config.decodePrefetch(entry);
    }

    // Compute RRPV from a mask entry (for ECG insert).
    // Uses only the DBG tier from the mask — P-OPT is consulted dynamically
    // at eviction time via findNextRef(), not at insert.
    uint8_t maskToRRPV(uint32_t mask_entry) const {
        uint8_t dbg = mask_config.decodeDBG(mask_entry);
        return mask_config.dbgTierToRRPV(dbg);
    }

    // Resolve prefetch target vertex ID from a mask entry.
    uint32_t resolvePrefetchTarget(uint32_t mask_entry) const {
        uint32_t raw = mask_config.decodePrefetch(mask_entry);
        if (raw == 0) return UINT32_MAX;  // No prefetch
        if (mask_config.prefetch_direct) return raw;  // Direct vertex ID
        if (raw - 1 < hot_table.size()) return hot_table[raw - 1];  // 1-based table lookup
        return UINT32_MAX;
    }

    // ================================================================
    // Per-Access Updates (maps to simulator registers/magic instructions)
    // ================================================================

    // Set current source and destination vertices (outer loop).
    // In Sniper: SimMagic1(CMD_SET_VERTICES, pack(src, dst))
    void setCurrentVertices(uint32_t src, uint32_t dst) {
        auto& h = hints_for_thread();
        h.current_src = src;
        h.current_dst = dst;
    }

    // Update destination vertex only (inner loop neighbor).
    void setCurrentDst(uint32_t dst) {
        hints_for_thread().current_dst = dst;
    }

    // Set ECG mask hint for current access.
    // In Sniper: the mask is encoded in the last 2 bits of vertex property data.
    void setMask(uint8_t mask) {
        hints_for_thread().mask = mask;
    }

    // ================================================================
    // Query Functions (used by cache replacement policies)
    // ================================================================

    // Classify an address across all registered property regions.
    // Returns: 0=unknown/streaming, 1=HOT, 2=WARM, 3=LUKEWARM, 4=COLD
    uint32_t classifyAddress(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            uint32_t tier = regions[i].classify(addr);
            if (tier != 0) return tier;
        }
        return 0;
    }

    // Classify an address to its degree bucket index (0 = highest-degree).
    // Returns: bucket index (0..N-1), or UINT32_MAX if not in any region.
    uint32_t classifyBucket(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            uint32_t b = regions[i].classifyBucket(addr);
            if (b < regions[i].num_buckets) return b;
        }
        return UINT32_MAX;
    }

    // Classify address into GRASP 3-tier reuse (Faldu et al., 2020).
    //
    // After DBG reorder, highest-degree vertices are at front (low addresses).
    // The original GRASP uses the trace-header `f` percentage of LLC capacity
    // to define the hot boundary within each property region:
    //   HOT:      [base, base + f% × llc_size + 8)
    //   MODERATE: [base + f% × llc_size + 8, base + 2f% × llc_size + 8)
    //   COLD:     everything else (including non-property data)
    //
    // Returns: 1=HOT, 2=MODERATE, 3=COLD (0=not in any region)
    // GRASP-faithful (ligra.h:66 add_region): the protected region is a fraction
    // of the VERTEX SPACE (frontier_frac x n), i.e. a fraction of the property
    // ARRAY — NOT of the LLC. This auto-scales: large graphs protect a
    // proportional vertex fraction instead of a fixed LLC byte range (which
    // under-protects at scale). llc_size is no longer used for the boundary.
    uint32_t classifyGRASP(uint64_t addr, size_t llc_size) const {
        (void)llc_size;
        // The per-region GRASP tier math lives in ecg_policy::classifyGraspTier
        // (shared with gem5 + Sniper). hot_fraction = grasp_hot_percent/100 (default
        // 0.15). Returns the first region's tier; cold if in none.
        for (uint32_t i = 0; i < num_regions; ++i) {
            const PropertyRegion& r = regions[i];
            if (!r.grasp_region) continue;
            uint32_t tier = ecg_policy::classifyGraspTier(
                addr, r.base_address, r.upper_bound, r.grasp_hot_percent / 100.0);
            if (tier != 0) return tier;
        }
        return 3;  // Not in any property region → cold
    }

    // ECG "mask" variant source: the GRASP 3-tier of vertex v by its array
    // position (shared ecg_policy::graspTierByIndex). Precomputed offline and
    // delivered in the per-edge mask (encoded at the build sites below, read via
    // decodeDBG at insertion) — identical across simulators, no per-sim regions.
    // On the DBG-reordered corpus this equals the region-based classifyGRASP.
    uint8_t graspTier3(uint32_t v) const {
        return static_cast<uint8_t>(ecg_policy::graspTierByIndex(
            v, topology.num_vertices, mask_config.grasp_hot_fraction));
    }

    // Map a cache-line address to its property vertex id (the region that contains
    // it). UINT32_MAX if not in any property region. Used by the ECG "mask" variant
    // so the INSERTED LINE gets ITS OWN vertex's delivered tier (mirrors gem5's
    // addressToVertex) — NOT the current access's hint, which is stale for fills
    // whose vertex differs from the demand (e.g. prefetch fills).
    uint32_t addressToVertex(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            const PropertyRegion& r = regions[i];
            if (addr >= r.base_address && addr < r.upper_bound && r.elem_size > 0)
                return static_cast<uint32_t>((addr - r.base_address) / r.elem_size);
        }
        return UINT32_MAX;
    }

    // ECG "mask" variant insertion tier: the delivered per-vertex GRASP tier for the
    // line owning `addr`, BYTE-EXACT to classifyGRASP (passes the region's elem_size
    // to graspTierByIndex so the +8 boundary matches). Cross-sim identical.
    uint32_t maskGraspTier(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            const PropertyRegion& r = regions[i];
            if (addr >= r.base_address && addr < r.upper_bound && r.elem_size > 0)
                return ecg_policy::graspTierByIndex(
                    (addr - r.base_address) / r.elem_size, topology.num_vertices,
                    mask_config.grasp_hot_fraction, r.elem_size);
        }
        return 3;
    }

    // Map a bucket index to an RRPV value (GRASP-XP style).
    // Uses degree-proportional mapping: buckets with more total edges
    // get lower RRPV (higher cache priority).
    //
    // max_rrpv: maximum RRPV value (3-bit=7, 8-bit=255)
    // bucket: bucket index from classifyBucket() or extractDBGTier()
    //
    // Mapping: RRPV = max_rrpv × (1 - edge_fraction_of_bucket)
    //   High-degree bucket (many edges) → low RRPV (keep in cache)
    //   Low-degree bucket (few edges) → high RRPV (evict sooner)
    uint8_t bucketToRRPV(uint32_t bucket, uint8_t max_rrpv = 7) const {
        if (topology.num_vertices == 0 || topology.num_edges == 0)
            return max_rrpv;  // No topology: default to cold

        if (bucket >= GraphTopology::NUM_BUCKETS)
            return max_rrpv;  // Unknown bucket: cold

        // classifyBucket() returns: 0 = highest degree (front after DBG reorder),
        //                           N-1 = lowest degree (back).
        // topology.bucket_total_degrees[]: 0 = lowest degree, N-1 = highest.
        // Reverse spatial → topology: dbg_bucket = N-1 - classifier_bucket.
        uint32_t dbg_bucket = GraphTopology::NUM_BUCKETS - 1 - bucket;

        // GRASP RRPV formula: edge_fraction = cumulative share of edges at this
        // degree level and ALL HIGHER degree levels. For the highest-degree bucket
        // (dbg_bucket = N-1), this includes everything from N-1 down... but we want
        // hubs to get LOW RRPV, meaning HIGH edge_fraction.
        //
        // Correct summation: from dbg_bucket UP to N-1 (all degrees >= this level).
        // For hubs (dbg_bucket = N-1): just the top bucket → fraction = hub edge share.
        // For cold (dbg_bucket = 0): all buckets → fraction = 1.0.
        //
        // But this gives cold vertices LOW RRPV (fraction=1.0 → rrpv=0)!
        // The fix: sum from 0 to dbg_bucket (cumulative from bottom), so:
        //   hubs (dbg_bucket = N-1) → sum all → fraction = 1.0 → rrpv = 0 (keep)
        //   cold (dbg_bucket = 0) → sum just bottom → fraction = small → rrpv = high (evict)
        uint64_t edge_sum = 0;
        for (uint32_t b = 0; b <= dbg_bucket; ++b)
            edge_sum += topology.bucket_total_degrees[b];

        double edge_fraction = static_cast<double>(edge_sum) / topology.num_edges;

        // RRPV = max × (1 - edge_fraction)
        // High edge_fraction → low RRPV (important, keep in cache)
        // Low edge_fraction → high RRPV (unimportant, evict)
        uint8_t rrpv = static_cast<uint8_t>(max_rrpv * (1.0 - edge_fraction));
        if (rrpv >= max_rrpv) rrpv = max_rrpv;
        return rrpv;
    }

    // Check if address is in any registered property region.
    bool isPropertyData(uint64_t addr) const {
        return classifyAddress(addr) != 0;
    }

    bool isEcgEpochData(uint64_t addr) const {
        const char* values =
            std::getenv("CACHE_ECG_EPOCH_REGION_INDICES");
        if (values && values[0]) {
            const char* cursor = values;
            while (*cursor) {
                char* end = nullptr;
                long index = std::strtol(cursor, &end, 10);
                if (end == cursor) break;
                if (index >= 0 &&
                    static_cast<unsigned long>(index) < num_regions &&
                    addr >= regions[index].base_address &&
                    addr < regions[index].upper_bound) {
                    return true;
                }
                cursor = (*end == ',') ? end + 1 : end;
            }
            return false;
        }
        const char* value = std::getenv("CACHE_ECG_EPOCH_REGION_INDEX");
        int index = value ? std::atoi(value) : (num_regions > 1 ? 1 : 0);
        return index >= 0 && static_cast<uint32_t>(index) < num_regions &&
               addr >= regions[index].base_address &&
               addr < regions[index].upper_bound;
    }

    // Find the property region containing an address.
    const PropertyRegion* findRegion(uint64_t addr) const {
        for (uint32_t i = 0; i < num_regions; ++i) {
            if (addr >= regions[i].base_address && addr < regions[i].upper_bound)
                return &regions[i];
        }
        return nullptr;
    }

    // Compute P-OPT rereference distance for a cache line address.
    uint32_t findNextRef(uint64_t line_addr) const {
        if (rereference.matrix == nullptr) return 127;
        if (hints_for_thread().current_src == UINT32_MAX) return 127;  // No vertex context set
        const PropertyRegion* r = findRegion(line_addr);
        if (r == nullptr) return 127;
        uint32_t cline_id = static_cast<uint32_t>(
            (line_addr - r->base_address) / rereference.line_size);
        return rereference.findNextRef(cline_id, hints_for_thread().current_src);
    }

    // ================================================================
    // Mask Computation (Phase 2 — preprocessing, called once)
    // ================================================================

    // Compute per-vertex DBG tier classification WITHOUT reordering.
    // Classifies each vertex by degree bucket relative to avg_degree.
    // Works with ANY vertex ordering (Rabbit, Leiden, GraphBrew, etc.).
    //
    // Returns: tier per vertex (0 = highest-degree, NUM_BUCKETS-1 = lowest)
    template<typename GraphT>
    std::vector<uint8_t> computeVertexTiers(const GraphT& g) const {
        uint32_t n = g.num_nodes();
        std::vector<uint8_t> tiers(n, 0);
        if (topology.num_vertices == 0) return tiers;

        #pragma omp parallel for schedule(static)
        for (uint32_t v = 0; v < n; ++v) {
            uint32_t deg = (mask_config.degree_mode == 1)
                ? static_cast<uint32_t>(g.in_degree(v))
                : static_cast<uint32_t>(g.out_degree(v));
            uint8_t tier = static_cast<uint8_t>(topology.NUM_BUCKETS - 1);
            for (uint32_t b = 0; b < topology.NUM_BUCKETS; ++b) {
                if (deg <= topology.bucket_thresholds[b]) {
                    tier = static_cast<uint8_t>(topology.NUM_BUCKETS - 1 - b);
                    break;
                }
            }
            tiers[v] = tier;
        }
        return tiers;
    }

    // Compute per-vertex mask entries with DBG tier, P-OPT hint, and prefetch target.
    //
    // P-OPT hint: quantized rereference distance from matrix (or degree proxy).
    //   Wider popt_bits → less quantization loss → better ECG_EMBEDDED oracle.
    //
    // Prefetch target: neighbor vertex ID to prefetch when this vertex is accessed.
    //   prefetch_mode 0 (NONE): no prefetch target (all zeros)
    //   prefetch_mode 1 (DEGREE): highest-degree neighbor not in dedup window
    //   prefetch_mode 2 (POPT): neighbor with shortest rereference distance
    //
    // Construction-time dedup: sliding window over vertex scan order prevents
    //   encoding the same prefetch target for consecutive vertices.
    //
    // Returns: vector of encoded mask entries (8-bit for mask_width=8)
    template<typename GraphT>
    std::vector<uint8_t> computeVertexMasks8(const GraphT& g) {
        auto masks32 = computeVertexMasks(g);
        std::vector<uint8_t> masks8(masks32.size());
        for (size_t i = 0; i < masks32.size(); i++)
            masks8[i] = static_cast<uint8_t>(masks32[i]);
        return masks8;
    }

    // General mask computation supporting any width (8/16/32 bits).
    // Returns uint32_t entries — caller truncates to mask_width.
    template<typename GraphT>
    std::vector<uint32_t> computeVertexMasks(const GraphT& g) {
        using Clock = std::chrono::steady_clock;
        auto build_start = Clock::now();
        ecg_stats.resetBuild();

        if (!mask_config.enabled) {
            initMaskConfig();
            if (topology.num_vertices > 0)
                mask_config.autoAllocate(topology.num_vertices);
        }
        auto tiers = computeVertexTiers(g);
        uint32_t n = g.num_nodes();
        std::vector<uint32_t> masks(n, 0);

        uint32_t popt_max = mask_config.popt_bits > 0 ? ((1U << mask_config.popt_bits) - 1) : 0;
        uint32_t pfx_max = mask_config.prefetch_bits > 0 ? ((1U << mask_config.prefetch_bits) - 1) : 0;

        std::vector<uint32_t> deg_arr(n);
#ifdef _OPENMP
        #pragma omp parallel for
#endif
        for (uint32_t v = 0; v < n; v++)
            deg_arr[v] = static_cast<uint32_t>(g.out_degree(v));

        std::vector<uint8_t> avg_reref_by_line;
        if (rereference.matrix && (popt_max > 0 || mask_config.prefetch_mode == 2 || mask_config.prefetch_mode == 4)) {
            avg_reref_by_line.resize(rereference.num_cache_lines, 0);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static)
#endif
            for (int64_t cline_i = 0; cline_i < static_cast<int64_t>(rereference.num_cache_lines); ++cline_i) {
                uint32_t cline = static_cast<uint32_t>(cline_i);
                uint32_t total_dist = 0;
                uint32_t count = 0;
                for (uint32_t e = 0; e < rereference.num_epochs; e++) {
                    uint8_t entry = rereference.matrix[e * rereference.num_cache_lines + cline];
                    if ((entry & 0x80) == 0) {
                        total_dist += (entry & 0x7F);
                        count++;
                    }
                }
                avg_reref_by_line[cline] = count > 0
                    ? static_cast<uint8_t>(std::min(total_dist / count, uint32_t(127)))
                    : 0;
            }
        }

        // Build hot table if using TABLE mode for prefetch
        if (pfx_max > 0 && !mask_config.prefetch_direct && hot_table.empty()) {
            buildHotTable(deg_arr.data(), n);
        }

        // Build hot table set for O(1) membership lookup
        std::unordered_set<uint32_t> hot_set;
        if (!mask_config.prefetch_direct && !hot_table.empty()) {
            for (auto id : hot_table) hot_set.insert(id);
        }

        // Construction-time dedup window (sliding across vertex scan order)
        // Not parallelized — sequential scan to maintain window state
        PrefetchDedupWindow build_dedup;
        build_dedup.capacity = mask_config.prefetch_window;

        uint64_t pfx_candidates = 0;
        uint64_t pfx_encoded = 0;
        uint64_t pfx_no_candidate = 0;
        uint64_t pfx_table_miss = 0;
        uint64_t pfx_dedup_skips = 0;

        for (uint32_t v = 0; v < n; ++v) {
            // --- P-OPT hint ---
            uint32_t popt_hint = 0;
            if (popt_max > 0 && topology.num_vertices > 0) {
                if (rereference.matrix) {
                    constexpr int numVtxPerLine = 16;
                    uint32_t cline = v / numVtxPerLine;
                    if (cline < avg_reref_by_line.size()) {
                        uint8_t avg_dist = avg_reref_by_line[cline];
                        popt_hint = (uint32_t(avg_dist) * popt_max) / 127;
                    }
                } else {
                    uint32_t deg = deg_arr[v];
                    if (deg == 0) popt_hint = popt_max;
                    else {
                        double ratio = static_cast<double>(deg) / topology.max_degree;
                        popt_hint = static_cast<uint32_t>(popt_max * (1.0 - ratio));
                    }
                }
            }

            // --- Prefetch target ---
            uint32_t pfx_value = 0;
            if (pfx_max > 0 && mask_config.prefetch_mode > 0) {
                // Find best neighbor to prefetch
                uint32_t best_target = UINT32_MAX;

                if (mask_config.prefetch_mode == 1) {
                    // DEGREE mode: highest-degree neighbor not in dedup window
                    uint32_t best_deg = 0;
                    for (auto ngh : g.out_neigh(v)) {
                        if (build_dedup.contains(ngh)) {
                            pfx_dedup_skips++;
                            continue;
                        }
                        uint32_t nd = deg_arr[static_cast<uint32_t>(ngh)];
                        if (nd > best_deg) {
                            best_deg = nd;
                            best_target = ngh;
                        }
                    }
                } else if (mask_config.prefetch_mode == 2 && rereference.matrix) {
                    // POPT mode: neighbor with shortest avg rereference distance
                    uint32_t best_dist = 128;
                    constexpr int numVtxPerLine = 16;
                    for (auto ngh : g.out_neigh(v)) {
                        if (build_dedup.contains(ngh)) {
                            pfx_dedup_skips++;
                            continue;
                        }
                        uint32_t ncline = static_cast<uint32_t>(ngh) / numVtxPerLine;
                        if (ncline < avg_reref_by_line.size()) {
                            uint32_t dist = avg_reref_by_line[ncline];
                            if (dist < best_dist) {
                                best_dist = dist;
                                best_target = ngh;
                            }
                        }
                    }
                } else if (mask_config.prefetch_mode == 4) {
                    // Far-future mode: target is selected from
                    // a global hot vertex pool (graph-wide top-K by degree),
                    // NOT from v's immediate neighbors. Uses hot_table when
                    // populated (prefetch_direct=false), else builds a
                    // GLOBAL_HOT_LIMIT-sized hot list on-the-fly from deg_arr.
                    //
                    // Why this beats DROPLET's mechanism:
                    // - DROPLET scans v's edge stream and prefetches next-K
                    //   neighbors. It physically CANNOT see a hot vertex that
                    //   isn't in v's next-K. ECG mode 4 encodes such vertices
                    //   directly.
                    // - On hub-and-spoke graphs, the hot working set is small
                    //   (top GLOBAL_HOT_LIMIT vertices) and reused by many
                    //   sources. Pulling these into L3 early benefits future
                    //   iterations DROPLET cannot reach.
                    //
                    // Encoding: rotate the global-hot index by (v + v_pos_hint)
                    // so consecutive v values emit different targets, then
                    // probe forward up to 8 entries until a non-deduped target
                    // is found.
                    constexpr uint32_t GLOBAL_HOT_LIMIT = 4096;
                    constexpr int numVtxPerLine = 16;

                    // Lazily-built sorted list of (degree, vertex) descending —
                    // capture once per build via static thread-safe init.
                    static thread_local std::vector<uint32_t> global_hot_cache;
                    static thread_local const std::vector<uint32_t>* cached_deg_ptr = nullptr;
                    static thread_local uint32_t cached_n = 0;
                    if (cached_deg_ptr != &deg_arr || cached_n != n) {
                        cached_deg_ptr = &deg_arr;
                        cached_n = n;
                        uint32_t hot_n = std::min(GLOBAL_HOT_LIMIT, n);
                        std::vector<std::pair<uint32_t, uint32_t>> by_deg;
                        by_deg.reserve(n);
                        for (uint32_t u = 0; u < n; u++) by_deg.emplace_back(deg_arr[u], u);
                        std::partial_sort(by_deg.begin(), by_deg.begin() + hot_n, by_deg.end(),
                            [](auto& a, auto& b) { return a.first > b.first; });
                        global_hot_cache.clear();
                        global_hot_cache.reserve(hot_n);
                        for (uint32_t i = 0; i < hot_n; i++)
                            global_hot_cache.push_back(by_deg[i].second);
                    }
                    const auto& hot_pool = !hot_table.empty() ? hot_table : global_hot_cache;
                    if (!hot_pool.empty()) {
                        uint8_t v_pos_hint = (rereference.matrix && (v / numVtxPerLine) < avg_reref_by_line.size())
                            ? avg_reref_by_line[v / numVtxPerLine] : 0;
                        uint32_t pool_n = static_cast<uint32_t>(hot_pool.size());
                        uint32_t base_idx = (static_cast<uint32_t>(v_pos_hint) + v) % pool_n;
                        uint32_t probe_limit = std::min<uint32_t>(8u, pool_n);
                        for (uint32_t step = 0; step < probe_limit; step++) {
                            uint32_t idx = (base_idx + step) % pool_n;
                            uint32_t hot_v = hot_pool[idx];
                            if (build_dedup.contains(hot_v)) {
                                pfx_dedup_skips++;
                                continue;
                            }
                            best_target = hot_v;
                            break;
                        }
                    }
                }

                if (best_target != UINT32_MAX) {
                    pfx_candidates++;
                    build_dedup.push(best_target);
                    if (mask_config.prefetch_direct) {
                        pfx_value = best_target & pfx_max;
                    } else if (!hot_table.empty()) {
                        // Encode as hot table index (+1, 0 = no prefetch)
                        for (uint32_t hi = 0; hi < hot_table.size(); hi++) {
                            if (hot_table[hi] == best_target) {
                                pfx_value = hi + 1;
                                break;
                            }
                        }
                        // If target not in hot table, try to find any hot neighbor
                        if (pfx_value == 0) {
                            for (auto ngh : g.out_neigh(v)) {
                                if (build_dedup.contains(ngh)) {
                                    pfx_dedup_skips++;
                                    continue;
                                }
                                if (hot_set.count(ngh)) {
                                    for (uint32_t hi = 0; hi < hot_table.size(); hi++) {
                                        if (hot_table[hi] == static_cast<uint32_t>(ngh)) {
                                            pfx_value = hi + 1;
                                            build_dedup.push(ngh);
                                            break;
                                        }
                                    }
                                    if (pfx_value != 0) break;
                                }
                            }
                        }
                        if (pfx_value == 0) pfx_table_miss++;
                    }
                } else {
                    pfx_no_candidate++;
                }
                if (pfx_value != 0) pfx_encoded++;
            }

            masks[v] = mask_config.encode(graspTier3(v),
                                          static_cast<uint8_t>(std::min(popt_hint, popt_max)),
                                          pfx_value);
        }
        ecg_stats.mask_build_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - build_start).count());
        ecg_stats.mask_vertices = n;
        ecg_stats.pfx_candidates = pfx_candidates;
        ecg_stats.pfx_encoded = pfx_encoded;
        ecg_stats.pfx_no_candidate = pfx_no_candidate;
        ecg_stats.pfx_table_miss = pfx_table_miss;
        ecg_stats.pfx_dedup_skips = pfx_dedup_skips;

        std::cout << "ECG Mask Build Time: " << std::fixed << std::setprecision(6)
                  << (double(ecg_stats.mask_build_us) / 1000000.0)
                  << " s vertices=" << ecg_stats.mask_vertices
                  << " pfx_candidates=" << ecg_stats.pfx_candidates
                  << " pfx_encoded=" << ecg_stats.pfx_encoded
                  << " pfx_no_candidate=" << ecg_stats.pfx_no_candidate
                  << " pfx_table_miss=" << ecg_stats.pfx_table_miss
                  << " pfx_dedup_skips=" << ecg_stats.pfx_dedup_skips
                  << std::defaultfloat << std::endl;
        return masks;
    }

    // ================================================================
    // Per-edge ECG mask builder
    // ================================================================
    //
    // Build the per-edge mask array for PR's in_neigh traversal pattern.
    // Each edge in the CSR carries a packed
    // mask encoding (dest_id | DBG_tier | POPT_quant | prefetch_target).
    // The prefetch_target is selected from src's iteration context — the
    // "next-K POPT-best dest" in src's in_neighbors after the current one.
    //
    // Stored in `in_edge_masks_by_src[src]` parallel to src's in_neigh list.
    // Caller: pr.cc when ECG_PREFETCH_MODE=6 (per_edge).
    //
    // Mask layout (64-bit, for graphs with up to 2^24 vertices):
    //   [0:24]  dest_id (24 bits)
    //   [24:26] DBG tier (2 bits)
    //   [26:33] POPT quant (7 bits)
    //   [33:64] prefetch target (31 bits)
    inline uint32_t configuredEpochScheduleK() const {
        const char* value = std::getenv("ECG_REUSE_PLAN_DEPTH");
        if (!value) return 0;
        int parsed = std::atoi(value);
        if (parsed <= 0) return 0;
        return parsed > 4 ? 4u : static_cast<uint32_t>(parsed);
    }

    inline uint32_t configuredEdgeEpochCount(
            uint32_t fallback = 32u) const {
        const char* value = std::getenv("ECG_EDGE_MASK_EPOCHS");
        uint32_t count = value
            ? static_cast<uint32_t>(std::atoi(value)) : fallback;
        if (count < 2) count = 2;
        if (count > 65535) count = 65535;
        if (configuredEpochScheduleK() == 2)
            count = ecg_reuse_plan::normalizeReusePlanEpochCount(count);
        return count;
    }

    template<typename GraphT>
    void buildReusePlanEpochSchedule(
            const GraphT& g, bool push_out_edges, uint32_t ne, bool linemin,
            std::vector<std::vector<uint16_t>>& target,
            std::vector<std::vector<uint8_t>>& tier_target) {
        std::vector<std::vector<ecg_reuse_plan::ReusePlan>> pairs;
        ecg_reuse_plan::buildInEdgeReusePlans(
            g, 16, ne, linemin, pairs, push_out_edges);
        target.assign(pairs.size(), {});
        tier_target.assign(pairs.size(), {});
        for (size_t src = 0; src < pairs.size(); ++src) {
            auto& row = target[src];
            auto& tiers = tier_target[src];
            row.resize(pairs[src].size() * 2);
            tiers.resize(pairs[src].size(), 0);
            for (size_t edge = 0; edge < pairs[src].size(); ++edge) {
                row[edge * 2] = pairs[src][edge].first;
                row[edge * 2 + 1] = pairs[src][edge].second;
                tiers[edge] = pairs[src][edge].tier;
            }
        }
    }

    template<typename GraphT>
    void buildInEdgeMasks_PR(const GraphT& g, int k_lookahead) {
        using Clock = std::chrono::steady_clock;
        auto build_start = Clock::now();
        uint32_t n = g.num_nodes();
        in_edge_masks_by_src.clear();
        in_edge_masks_by_src.resize(n);
        in_edge_epoch_by_src.clear();
        in_edge_epoch_by_src.resize(n);
        in_edge_epoch_sched_by_src.clear();
        in_edge_epoch_sched_by_src.resize(n);
        in_edge_grasp_tier_by_src.clear();
        in_edge_grasp_tier_by_src.resize(n);
        if (n == 0) return;
        uint32_t reuse_plan_depth = configuredEpochScheduleK();
        if (reuse_plan_depth == 2 && exact_off.empty())
            registerOutAdjacencyExact(g);

        // ECG_EDGE_MASK_EXACT: fill the per-edge POPT field [26:33] with the
        // EXACT next-reference of dest (log2-quantized to 5 bits) instead of the
        // epoch-AVERAGE rereference. This is the precomputed-mask realization of
        // the ECG:EXACT sweep — the value an offline pass embeds per edge, then
        // the cache reads at access (no live recompute, no graph query at evict).
        const bool edge_mask_exact = std::getenv("ECG_EDGE_MASK_EXACT") != nullptr
            && !exact_off.empty();

        // ECG_EDGE_MASK_EPOCH: fill the per-edge POPT field [26:33] with the
        // ABSOLUTE next-reference EPOCH of dest (5 bits = 32 epochs), wrapping to
        // the next iteration when dest has no out-neighbor > src. At eviction the
        // cache computes circular distance (stored_epoch - current_epoch) so
        // stale/passed lines rank far and evict correctly. (ECG_GRASP_POPT.)
        const bool edge_mask_epoch = std::getenv("ECG_EDGE_MASK_EPOCH") != nullptr
            && !exact_off.empty();
        // ECG_EDGE_MASK_LINEMIN: store the per-LINE-min next-ref epoch (soonest over
        // the 16 vertices sharing dest's cache line) instead of just dest's — matches
        // P-OPT's per-line granularity. 16x build cost, fully parallel.
        const bool edge_mask_linemin = std::getenv("ECG_EDGE_MASK_LINEMIN") != nullptr;
        // ECG_REUSE_PLAN_DEPTH=K: also store the next-K (not just the soonest) line
        // next-ref epochs per edge, so a resident line self-advances as cur_epoch passes
        // each — recovering the matrix's per-epoch dimension. K in [0,4] (0=disabled).
        // Most effective WITH linemin (multiple vertices/line => multiple distinct
        // future touches of the line). edge_epoch_reuse_plan_depth records the active K.
        edge_epoch_reuse_plan_depth = reuse_plan_depth;
        edge_epoch_count = configuredEdgeEpochCount(
            edge_epoch_count ? edge_epoch_count : 32u);
        // ECG_EDGE_MASK_PACK: enforce the REAL packed4 spare-bit cap — the epoch must fit
        // in the spare high bits of the per-edge record container:
        //   spare = pack_bits - ceil(log2 N), ne_cap = 2^spare.
        // Default container = 32 bits (a 4B fat-CSR edge word). ECG_EDGE_MASK_PACK_BITS=64
        // models the ISA-faithful 64-bit packed record (the per-src masks are stored
        // 64-bit and the ecg.extract ISA delivers a 64-bit record), which keeps full
        // epoch resolution at scale instead of collapsing to 2^(32-id_bits). The wider
        // (8B) record stream is then honestly charged by pr.cc's ecgRecordBytes auto-
        // switch under CHARGED=1 (and is free under CHARGED=0 / ISA delivery). For large
        // graphs (e.g. kron-s24, id_bits=24) the 8B record is already required, so the
        // 32-bit cap throws away epoch resolution for NO bandwidth saving.
        // two-epoch ReusePlan uses its own 64-bit dest32+tier2+epoch15+epoch15 record, so this
        // legacy 32/64-bit single-epoch cap does not apply to ReusePlan.
        if (std::getenv("ECG_EDGE_MASK_PACK") && reuse_plan_depth != 2) {
            uint32_t pack_bits = 32;
            if (const char* pb = std::getenv("ECG_EDGE_MASK_PACK_BITS")) {
                pack_bits = ((uint32_t)std::atoi(pb) >= 64) ? 64u : 32u;
            }
            uint32_t id_bits = 1; while (id_bits < (pack_bits - 1) && ((uint64_t)1 << id_bits) < n) id_bits++;
            uint32_t spare = (id_bits >= pack_bits) ? 0u : (pack_bits - id_bits);
            uint32_t ne_cap = (spare >= 16) ? 65535u : (spare == 0 ? 1u : (1u << spare));
            if (edge_epoch_count > ne_cap) edge_epoch_count = ne_cap;
            std::cout << "ECG_EDGE_MASK_PACK: pack_bits=" << pack_bits << " N=" << n << " id_bits=" << id_bits
                      << " spare_bits=" << spare << " ne_cap=" << ne_cap
                      << " -> ne=" << edge_epoch_count << std::endl;
        }
        const uint32_t kNumEpochs5 = edge_epoch_count;

        // Build degree array + tiers (reuse computeVertexTiers if not already)
        auto tiers = computeVertexTiers(g);
        std::vector<uint32_t> deg_arr(n);
#ifdef _OPENMP
        #pragma omp parallel for
#endif
        for (uint32_t v = 0; v < n; v++)
            deg_arr[v] = static_cast<uint32_t>(g.out_degree(v));

        // Build avg_reref_by_line if not already populated
        constexpr int numVtxPerLine = 16;
        std::vector<uint8_t> avg_reref_by_line;
        if (rereference.matrix) {
            avg_reref_by_line.resize(rereference.num_cache_lines, 0);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static)
#endif
            for (int64_t cline_i = 0; cline_i < static_cast<int64_t>(rereference.num_cache_lines); ++cline_i) {
                uint32_t cline = static_cast<uint32_t>(cline_i);
                uint32_t total_dist = 0;
                uint32_t count = 0;
                for (uint32_t e = 0; e < rereference.num_epochs; e++) {
                    uint8_t entry = rereference.matrix[e * rereference.num_cache_lines + cline];
                    if ((entry & 0x80) == 0) {
                        total_dist += (entry & 0x7F);
                        count++;
                    }
                }
                avg_reref_by_line[cline] = count > 0
                    ? static_cast<uint8_t>(std::min(total_dist / count, uint32_t(127)))
                    : 0;
            }
        }

        uint64_t edge_count = 0;
        uint64_t encoded_count = 0;

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 128) reduction(+:edge_count, encoded_count)
#endif
        for (uint32_t src = 0; src < n; src++) {
            // Materialize this src's in-neighbors into a small vector so we
            // can index by position (the iterator's underlying offset).
            std::vector<uint32_t> neighbors;
            neighbors.reserve(64);
            for (auto v : g.in_neigh(src)) neighbors.push_back(static_cast<uint32_t>(v));

            auto& masks = in_edge_masks_by_src[src];
            masks.resize(neighbors.size(), 0);
            auto& eps = in_edge_epoch_by_src[src];
            if (edge_mask_epoch) eps.resize(neighbors.size(), 0);
            auto& sch = in_edge_epoch_sched_by_src[src];
            if (edge_mask_epoch && reuse_plan_depth) sch.assign(neighbors.size() * reuse_plan_depth, 0);

            for (size_t i = 0; i < neighbors.size(); i++) {
                uint32_t dest = neighbors[i];
                edge_count++;

                // POPT/DBG fields for dest
                uint8_t dbg = graspTier3(dest);  // delivered three-tier classification
                uint8_t popt = 0;
                if (!avg_reref_by_line.empty()) {
                    uint32_t dest_cline = dest / numVtxPerLine;
                    if (dest_cline < avg_reref_by_line.size())
                        popt = avg_reref_by_line[dest_cline] & 0x7F;
                }
                // EXACT override: next-ref of dest from the consuming position src
                // = (first out-neighbor of dest strictly > src) - src, log2-quantized
                // to 5 bits. This is exactNextRef() for a single vertex, precomputed.
                if (edge_mask_exact && dest < exact_nv) {
                    int64_t lo = exact_off[dest], hi = exact_off[dest + 1];
                    while (lo < hi) {  // first out-neighbor > src (neighbors sorted asc)
                        int64_t mid = (lo + hi) >> 1;
                        if ((uint32_t)exact_nbr[mid] > src) hi = mid; else lo = mid + 1;
                    }
                    uint32_t dist = (lo < exact_off[dest + 1])
                        ? ((uint32_t)exact_nbr[lo] - src) : UINT32_MAX;
                    uint8_t q;
                    if (dist == UINT32_MAX) q = 31;   // no future ref -> farthest (evict)
                    else { uint32_t b = 0, x = dist; while (x > 0) { b++; x >>= 1; } q = (b > 31) ? 31 : (uint8_t)b; }
                    popt = q & 0x7F;  // 5-bit log-distance in the 7-bit POPT field
                }
                // EPOCH override: ABSOLUTE next-ref epoch of dest in [0,31]. Find the
                // next out-neighbor of dest strictly > src; if none, wrap to dest's
                // first out-neighbor (next iteration). epoch = neighbor * 32 / n.
                if (edge_mask_epoch && dest < exact_nv) {
                    // Scan dest only, or the whole 16-vertex line (linemin). Keep the
                    // SOONEST next reference (min circular distance from src) and store
                    // its absolute epoch.
                    uint32_t v0 = edge_mask_linemin ? (dest / numVtxPerLine) * numVtxPerLine : dest;
                    uint32_t v1 = edge_mask_linemin ? std::min<uint32_t>(v0 + numVtxPerLine, exact_nv) : (dest + 1);
                    uint32_t best_dist = UINT32_MAX, best_ep = kNumEpochs5 - 1;
                    for (uint32_t w = v0; w < v1; ++w) {
                        int64_t a = exact_off[w], b = exact_off[w + 1];
                        if (a >= b) continue;  // isolated vertex
                        int64_t lo = a, hi = b;
                        while (lo < hi) {
                            int64_t mid = (lo + hi) >> 1;
                            if ((uint32_t)exact_nbr[mid] > src) hi = mid; else lo = mid + 1;
                        }
                        uint32_t next_nbr, dist;
                        if (lo < b) { next_nbr = (uint32_t)exact_nbr[lo]; dist = next_nbr - src; }
                        else        { next_nbr = (uint32_t)exact_nbr[a]; dist = next_nbr + n - src; }  // wrap
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_ep = (uint32_t)(((uint64_t)next_nbr * kNumEpochs5) / std::max<uint32_t>(1u, n));
                        }
                    }
                    popt = (best_ep >= kNumEpochs5 ? (kNumEpochs5 - 1) : best_ep) & 0x7F;
                    if (i < eps.size()) eps[i] = static_cast<uint16_t>(
                        best_ep >= kNumEpochs5 ? (kNumEpochs5 - 1) : best_ep);  // FULL epoch (untruncated)

                    // ECG_REUSE_PLAN_DEPTH: build the next-K forward schedule (additive;
                    // leaves eps[i]/best_ep above untouched so the sched-off path stays
                    // byte-identical). Collect up to K forward refs of EACH vertex on the
                    // line (positions lo..lo+K), plus the wrap ref for vertices with no
                    // out-neighbor > src; keep the K SOONEST distances as the schedule.
                    if (reuse_plan_depth && i * reuse_plan_depth < sch.size()) {
                        constexpr int kCandCap = 4 * numVtxPerLine;  // 64
                        std::pair<uint32_t, uint32_t> cand[kCandCap];  // (dist, epoch)
                        int nc = 0;
                        for (uint32_t w = v0; w < v1 && nc < kCandCap; ++w) {
                            int64_t a = exact_off[w], b = exact_off[w + 1];
                            if (a >= b) continue;
                            int64_t lo = a, hi = b;
                            while (lo < hi) { int64_t mid = (lo + hi) >> 1;
                                if ((uint32_t)exact_nbr[mid] > src) hi = mid; else lo = mid + 1; }
                            if (lo < b) {
                                for (int64_t p = lo; p < b && p < lo + (int64_t)reuse_plan_depth && nc < kCandCap; ++p) {
                                    uint32_t nb = (uint32_t)exact_nbr[p];
                                    uint32_t ep = (uint32_t)(((uint64_t)nb * kNumEpochs5) / std::max<uint32_t>(1u, n));
                                    if (ep >= kNumEpochs5) ep = kNumEpochs5 - 1;
                                    cand[nc++] = { nb - src, ep };
                                }
                            } else {  // wrap: vertex's first out-neighbor (next iteration)
                                uint32_t nb = (uint32_t)exact_nbr[a];
                                uint32_t ep = (uint32_t)(((uint64_t)nb * kNumEpochs5) / std::max<uint32_t>(1u, n));
                                if (ep >= kNumEpochs5) ep = kNumEpochs5 - 1;
                                cand[nc++] = { nb + n - src, ep };
                            }
                        }
                        std::sort(cand, cand + nc,
                                  [](const std::pair<uint32_t, uint32_t>& x,
                                     const std::pair<uint32_t, uint32_t>& y) { return x.first < y.first; });
                        uint32_t fallback = best_ep >= kNumEpochs5 ? (kNumEpochs5 - 1) : best_ep;
                        for (uint32_t k = 0; k < reuse_plan_depth; ++k)
                            sch[i * reuse_plan_depth + k] = (k < (uint32_t)nc)
                                ? static_cast<uint16_t>(cand[k].second)
                                : static_cast<uint16_t>(fallback);
                    }
                }

                // Per-edge prefetch target (shared decision: ecg_mode6::
                // selectPrefetchTarget, identical to gem5/Sniper's mask build).
                // Only in the lookahead mode (not exact/epoch which fill POPT).
                uint32_t prefetch_target = 0;
                if (!edge_mask_exact && !edge_mask_epoch) {
                    prefetch_target = ecg_mode6::selectPrefetchTarget(
                        neighbors.data(), neighbors.size(), i,
                        avg_reref_by_line, k_lookahead, numVtxPerLine);
                }
                if (prefetch_target != 0) encoded_count++;

                // Pack: [0:24]=dest [24:26]=dbg [26:33]=popt [33:64]=prefetch
                uint64_t mask = static_cast<uint64_t>(dest & 0xFFFFFFu);
                mask |= static_cast<uint64_t>(dbg & 0x3u) << 24;
                mask |= static_cast<uint64_t>(popt & 0x7Fu) << 26;
                mask |= static_cast<uint64_t>(prefetch_target & 0x7FFFFFFFu) << 33;
                masks[i] = mask;
            }
        }
        // two-epoch ReusePlan is shared with gem5/Sniper through ecg_reuse_plan_builder.h.
        // Overwrite the legacy local ReusePlan construction so all three simulators
        // use exactly the same second-reference and wrap semantics.
        if (edge_mask_epoch && reuse_plan_depth == 2) {
            buildReusePlanEpochSchedule(
                g, false, kNumEpochs5, edge_mask_linemin,
                in_edge_epoch_sched_by_src, in_edge_grasp_tier_by_src);
        }

        auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - build_start).count();
        std::cout << "ECG per-edge mask build: vertices=" << n
                  << " edges=" << edge_count
                  << " encoded=" << encoded_count
                  << " (" << (encoded_count * 100.0 / std::max<uint64_t>(edge_count, 1)) << "%)"
                  << " time_s=" << std::fixed << std::setprecision(4)
                  << (build_us / 1e6) << std::defaultfloat << std::endl;
    }

    // Build OUT-edge per-edge masks: the direction mirror of buildInEdgeMasks_PR for
    // kernels that traverse g.out_neigh (BFS top-down push). Self-contained — uses its
    // own out_edge_masks_by_src storage and its own IN-adjacency next-ref arrays
    // (exact_in_off/exact_in_nbr), and never reads or writes the PR in-edge path's
    // members, so PR stays byte-identical. Fills the absolute next-ref EPOCH (the
    // ECG_GRASP_POPT signal): property[dest], read while pushing src->dest, is next
    // read at the next IN-neighbour of dest strictly > src (wrapping to the next
    // iteration), so the epoch is derived from g.in_neigh. On symmetric graphs this
    // equals the in-edge mask. ne = edge_epoch_count (set by buildInEdgeMasks_PR or
    // the default 32). num_vtx_per_line matches the 16-vertex line model.
    template<typename GraphT>
    void buildOutEdgeMasks(const GraphT& g) {
        using Clock = std::chrono::steady_clock;
        auto build_start = Clock::now();
        uint32_t n = g.num_nodes();
        exact_nv = n;
        out_edge_masks_by_src.assign(n, {});
        out_edge_epoch_by_src.assign(n, {});
        out_edge_epoch_sched_by_src.assign(n, {});
        out_edge_grasp_tier_by_src.assign(n, {});
        if (n == 0) return;
        const uint32_t reuse_plan_depth = configuredEpochScheduleK();
        edge_epoch_count = configuredEdgeEpochCount(
            edge_epoch_count ? edge_epoch_count : 32u);
        const uint32_t ne = edge_epoch_count ? edge_epoch_count : 32u;
        constexpr int numVtxPerLine = 16;
        const bool linemin = std::getenv("ECG_EDGE_MASK_LINEMIN") != nullptr;
        if (reuse_plan_depth == 2) edge_epoch_reuse_plan_depth = 2;

        // Build the sorted IN-adjacency (the transpose of OUT traversal) once.
        exact_in_nv = n;
        exact_in_off.assign((size_t)n + 1, 0);
        for (uint32_t v = 0; v < n; ++v)
            exact_in_off[v + 1] = exact_in_off[v] + g.in_degree(v);
        exact_in_nbr.assign((size_t)exact_in_off[n], 0);
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 256)
#endif
        for (int64_t v = 0; v < (int64_t)n; ++v) {
            int64_t p = exact_in_off[v];
            for (auto w : g.in_neigh((uint32_t)v)) exact_in_nbr[p++] = static_cast<int32_t>(w);
            std::sort(exact_in_nbr.begin() + exact_in_off[v], exact_in_nbr.begin() + exact_in_off[v + 1]);
        }

        auto tiers = computeVertexTiers(g);
        uint64_t edge_count = 0;
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 128) reduction(+:edge_count)
#endif
        for (uint32_t src = 0; src < n; src++) {
            std::vector<uint32_t> neighbors;
            neighbors.reserve(64);
            for (auto v : g.out_neigh(src)) neighbors.push_back(static_cast<uint32_t>(v));
            auto& masks = out_edge_masks_by_src[src];
            auto& eps = out_edge_epoch_by_src[src];
            masks.resize(neighbors.size(), 0);
            eps.resize(neighbors.size(), 0);
            for (size_t i = 0; i < neighbors.size(); i++) {
                uint32_t dest = neighbors[i];
                edge_count++;
                uint8_t dbg = graspTier3(dest);  // delivered three-tier classification
                // Absolute next-ref epoch of dest via the IN-adjacency: soonest next
                // in-neighbour of dest (or its line) strictly > src, wrapping to the
                // next iteration. Identical math to buildInEdgeMasks_PR's epoch path
                // but over exact_in_* instead of exact_*.
                uint32_t v0 = linemin ? (dest / numVtxPerLine) * numVtxPerLine : dest;
                uint32_t v1 = linemin ? std::min<uint32_t>(v0 + numVtxPerLine, exact_in_nv) : (dest + 1);
                uint32_t best_dist = UINT32_MAX, best_ep = ne - 1;
                for (uint32_t w = v0; w < v1 && w < exact_in_nv; ++w) {
                    int64_t a = exact_in_off[w], b = exact_in_off[w + 1];
                    if (a >= b) continue;
                    int64_t lo = a, hi = b;
                    while (lo < hi) {
                        int64_t mid = (lo + hi) >> 1;
                        if ((uint32_t)exact_in_nbr[mid] > src) hi = mid; else lo = mid + 1;
                    }
                    uint32_t next_nbr, dist;
                    if (lo < b) { next_nbr = (uint32_t)exact_in_nbr[lo]; dist = next_nbr - src; }
                    else        { next_nbr = (uint32_t)exact_in_nbr[a]; dist = next_nbr + n - src; }
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_ep = (uint32_t)(((uint64_t)next_nbr * ne) / std::max<uint32_t>(1u, n));
                    }
                }
                uint8_t popt = (best_ep >= ne ? (ne - 1) : best_ep) & 0x7F;
                eps[i] = static_cast<uint16_t>(best_ep >= ne ? (ne - 1) : best_ep);
                uint64_t mask = static_cast<uint64_t>(dest & 0xFFFFFFu);
                mask |= static_cast<uint64_t>(dbg & 0x3u) << 24;
                mask |= static_cast<uint64_t>(popt & 0x7Fu) << 26;
                masks[i] = mask;
            }
        }
        if (reuse_plan_depth == 2) {
            buildReusePlanEpochSchedule(
                g, true, ne, linemin, out_edge_epoch_sched_by_src,
                out_edge_grasp_tier_by_src);
        }
        auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - build_start).count();
        std::cout << "ECG OUT-edge mask build: vertices=" << n << " edges=" << edge_count
                  << " ne=" << ne << " time_s=" << std::fixed << std::setprecision(4)
                  << (build_us / 1e6) << std::defaultfloat << std::endl;
    }

    // Build IN-edge (inverse-graph / CSC) per-edge masks: the GENERIC direction
    // mirror of buildOutEdgeMasks (main-graph / CSR). Not kernel-specific — any
    // kernel that traverses g.in_neigh(src) and reads a per-vertex datum keyed on
    // the in-neighbour dest uses these (PR pull reads property[dest]; BFS bottom-up
    // probes the frontier bit of dest). dest's datum is next read when dest's next
    // OUT-neighbour > src is processed, so the transpose-correct epoch is derived
    // from g.out_neigh (soonest out-neighbour of dest strictly > src, wrapping to
    // the next iteration). Self-contained: builds a LOCAL sorted out-adjacency and
    // fills only the in_edge_* members (never touches the shared exact_*/out_edge_*
    // paths), so PR and the OUT-edge path stay byte-identical. Fills the epoch
    // UNCONDITIONALLY — this is the plain transpose-epoch variant; buildInEdgeMasks_PR
    // is the same direction with the extra PR options (prefetch target + EXACT/EPOCH/
    // PACK env modes, epoch only under ECG_EDGE_MASK_EPOCH). On the symmetric eval
    // corpus in==out so this equals the out-edge mask (inert); it is the correct
    // inverse-graph mask on directed graphs.
    template<typename GraphT>
    void buildInEdgeMasks(const GraphT& g) {
        using Clock = std::chrono::steady_clock;
        auto build_start = Clock::now();
        uint32_t n = g.num_nodes();
        exact_nv = n;
        in_edge_masks_by_src.assign(n, {});
        in_edge_epoch_by_src.assign(n, {});
        in_edge_epoch_sched_by_src.assign(n, {});
        in_edge_grasp_tier_by_src.assign(n, {});
        if (n == 0) return;
        const uint32_t reuse_plan_depth = configuredEpochScheduleK();
        edge_epoch_count = configuredEdgeEpochCount(
            edge_epoch_count ? edge_epoch_count : 32u);
        const uint32_t ne = edge_epoch_count ? edge_epoch_count : 32u;
        constexpr int numVtxPerLine = 16;
        const bool linemin = std::getenv("ECG_EDGE_MASK_LINEMIN") != nullptr;
        if (reuse_plan_depth == 2) edge_epoch_reuse_plan_depth = 2;

        // LOCAL sorted OUT-adjacency (the transpose of IN traversal) for next-ref.
        std::vector<int64_t> off((size_t)n + 1, 0);
        for (uint32_t v = 0; v < n; ++v)
            off[v + 1] = off[v] + g.out_degree(v);
        std::vector<int32_t> nbr((size_t)off[n], 0);
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 256)
#endif
        for (int64_t v = 0; v < (int64_t)n; ++v) {
            int64_t p = off[v];
            for (auto w : g.out_neigh((uint32_t)v)) nbr[p++] = static_cast<int32_t>(w);
            std::sort(nbr.begin() + off[v], nbr.begin() + off[v + 1]);
        }

        auto tiers = computeVertexTiers(g);
        uint64_t edge_count = 0;
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 128) reduction(+:edge_count)
#endif
        for (uint32_t src = 0; src < n; src++) {
            std::vector<uint32_t> neighbors;
            neighbors.reserve(64);
            for (auto v : g.in_neigh(src)) neighbors.push_back(static_cast<uint32_t>(v));
            auto& masks = in_edge_masks_by_src[src];
            auto& eps = in_edge_epoch_by_src[src];
            masks.resize(neighbors.size(), 0);
            eps.resize(neighbors.size(), 0);
            for (size_t i = 0; i < neighbors.size(); i++) {
                uint32_t dest = neighbors[i];
                edge_count++;
                uint8_t dbg = graspTier3(dest);  // delivered three-tier classification
                // Soonest next out-neighbour of dest (or its 16-vertex line) strictly
                // > src, wrapping to the next iteration. Mirror of buildOutEdgeMasks
                // over the (local) out-adjacency instead of exact_in_*.
                uint32_t v0 = linemin ? (dest / numVtxPerLine) * numVtxPerLine : dest;
                uint32_t v1 = linemin ? std::min<uint32_t>(v0 + numVtxPerLine, n) : (dest + 1);
                uint32_t best_dist = UINT32_MAX, best_ep = ne - 1;
                for (uint32_t w = v0; w < v1 && w < n; ++w) {
                    int64_t a = off[w], b = off[w + 1];
                    if (a >= b) continue;
                    int64_t lo = a, hi = b;
                    while (lo < hi) {
                        int64_t mid = (lo + hi) >> 1;
                        if ((uint32_t)nbr[mid] > src) hi = mid; else lo = mid + 1;
                    }
                    uint32_t next_nbr, dist;
                    if (lo < b) { next_nbr = (uint32_t)nbr[lo]; dist = next_nbr - src; }
                    else        { next_nbr = (uint32_t)nbr[a]; dist = next_nbr + n - src; }
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_ep = (uint32_t)(((uint64_t)next_nbr * ne) / std::max<uint32_t>(1u, n));
                    }
                }
                uint8_t popt = (best_ep >= ne ? (ne - 1) : best_ep) & 0x7F;
                eps[i] = static_cast<uint16_t>(best_ep >= ne ? (ne - 1) : best_ep);
                uint64_t mask = static_cast<uint64_t>(dest & 0xFFFFFFu);
                mask |= static_cast<uint64_t>(dbg & 0x3u) << 24;
                mask |= static_cast<uint64_t>(popt & 0x7Fu) << 26;
                masks[i] = mask;
            }
        }
        if (reuse_plan_depth == 2) {
            buildReusePlanEpochSchedule(
                g, false, ne, linemin, in_edge_epoch_sched_by_src,
                in_edge_grasp_tier_by_src);
        }
        auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - build_start).count();
        std::cout << "ECG IN-edge mask build (inverse graph): vertices=" << n << " edges=" << edge_count
                  << " ne=" << ne << " time_s=" << std::fixed << std::setprecision(4)
                  << (build_us / 1e6) << std::defaultfloat << std::endl;
    }

    // Build the per-edge mask array with CROSS-ITERATION prefetch targets
    // (mode 7).
    //
    // Unlike mode 6 (which picks the prefetch target from src's OWN next-K
    // edges — equivalent to precomputed mode-2 lookahead), mode 7 picks the
    // target from a FUTURE src's edge list: src + K_JUMP where K_JUMP is
    // configurable. This is DROPLET-inaccessible because DROPLET cannot
    // project across u-iteration boundaries — it only sees the immediate
    // address stream.
    //
    // For each (src, edge_i): prefetch_target = best-POPT dest of (src + K_JUMP)
    // When kernel processes src and reads dest_i, the cache pre-fetches a
    // vertex from K_JUMP iterations ahead — giving the cache time to fill
    // BEFORE the demand arrives.
    template<typename GraphT>
    void buildInEdgeMasks_PR_CrossIter(const GraphT& g, int k_jump) {
        using Clock = std::chrono::steady_clock;
        auto build_start = Clock::now();
        uint32_t n = g.num_nodes();
        in_edge_masks_by_src.clear();
        in_edge_masks_by_src.resize(n);
        if (n == 0) return;

        auto tiers = computeVertexTiers(g);
        constexpr int numVtxPerLine = 16;
        std::vector<uint8_t> avg_reref_by_line;
        if (rereference.matrix) {
            avg_reref_by_line.resize(rereference.num_cache_lines, 0);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static)
#endif
            for (int64_t cline_i = 0; cline_i < static_cast<int64_t>(rereference.num_cache_lines); ++cline_i) {
                uint32_t cline = static_cast<uint32_t>(cline_i);
                uint32_t total_dist = 0, count = 0;
                for (uint32_t e = 0; e < rereference.num_epochs; e++) {
                    uint8_t entry = rereference.matrix[e * rereference.num_cache_lines + cline];
                    if ((entry & 0x80) == 0) {
                        total_dist += (entry & 0x7F);
                        count++;
                    }
                }
                avg_reref_by_line[cline] = count > 0
                    ? static_cast<uint8_t>(std::min(total_dist / count, uint32_t(127)))
                    : 0;
            }
        }

        uint64_t edge_count = 0, encoded_count = 0;
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 128) reduction(+:edge_count, encoded_count)
#endif
        for (uint32_t src = 0; src < n; src++) {
            // Materialize src's in-neighbors
            std::vector<uint32_t> neighbors;
            neighbors.reserve(64);
            for (auto v : g.in_neigh(src)) neighbors.push_back(static_cast<uint32_t>(v));

            // CROSS-ITERATION: look at src + K_JUMP — find its best dest
            // (lowest POPT reuse distance among its in-neighbors).
            // DROPLET cannot predict this target because it requires knowing
            // the future kernel iteration's in-neighbor set.
            uint32_t future_src = (src + k_jump < n) ? (src + k_jump) : (src);  // wrap-clamp
            uint32_t cross_iter_target = 0;
            if (future_src != src && !avg_reref_by_line.empty()) {
                uint8_t best_dist = 128;
                int probe_limit = 0;
                for (auto v : g.in_neigh(future_src)) {
                    if (probe_limit++ >= 32) break;  // cap probe to bound cost
                    uint32_t cline = uint32_t(v) / numVtxPerLine;
                    if (cline < avg_reref_by_line.size()) {
                        uint8_t dist = avg_reref_by_line[cline];
                        if (dist < best_dist) {
                            best_dist = dist;
                            cross_iter_target = uint32_t(v);
                        }
                    }
                }
            }

            auto& masks = in_edge_masks_by_src[src];
            masks.resize(neighbors.size(), 0);

            for (size_t i = 0; i < neighbors.size(); i++) {
                uint32_t dest = neighbors[i];
                edge_count++;
                uint8_t dbg = graspTier3(dest);  // delivered three-tier classification
                uint8_t popt = 0;
                if (!avg_reref_by_line.empty()) {
                    uint32_t cl = dest / numVtxPerLine;
                    if (cl < avg_reref_by_line.size()) popt = avg_reref_by_line[cl] & 0x7F;
                }
                if (cross_iter_target != 0) encoded_count++;
                uint64_t mask = static_cast<uint64_t>(dest & 0xFFFFFFu);
                mask |= static_cast<uint64_t>(dbg & 0x3u) << 24;
                mask |= static_cast<uint64_t>(popt & 0x7Fu) << 26;
                mask |= static_cast<uint64_t>(cross_iter_target & 0x7FFFFFFFu) << 33;
                masks[i] = mask;
            }
        }

        auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - build_start).count();
        std::cout << "ECG per-edge CROSS-ITER mask build: vertices=" << n
                  << " edges=" << edge_count
                  << " encoded=" << encoded_count
                  << " (" << (encoded_count * 100.0 / std::max<uint64_t>(edge_count, 1)) << "%)"
                  << " K_JUMP=" << k_jump
                  << " time_s=" << std::fixed << std::setprecision(4)
                  << (build_us / 1e6) << std::defaultfloat << std::endl;
    }

    // Helper to extract fields from a per-edge mask
    static inline uint32_t edgeMaskDest(uint64_t mask) {
        return static_cast<uint32_t>(mask & 0xFFFFFFu);
    }
    static inline uint8_t edgeMaskDBG(uint64_t mask) {
        return static_cast<uint8_t>((mask >> 24) & 0x3u);
    }
    static inline uint8_t edgeMaskPOPT(uint64_t mask) {
        return static_cast<uint8_t>((mask >> 26) & 0x7Fu);
    }
    static inline uint32_t edgeMaskPrefetch(uint64_t mask) {
        return static_cast<uint32_t>((mask >> 33) & 0x7FFFFFFFu);
    }

    // === Canonical per-edge demand-mask consumption ===============================
    // These helpers define how a kernel consumes a
    // per-edge mask, so the guard + POPT extraction + next-ref-epoch stamp + sticky-
    // epoch hygiene live in ONE place instead of being copy-pasted per kernel. A kernel
    // picks the EdgeMaskDir for the edge list it traverses; the mask is "ready" if its
    // builder ran (buildOutEdgeMasks / buildInEdgeMasks).

    // True when the `dir` edge-mask row for `src` is built and sized to `degree` (so the
    // per-edge mask is usable). Used where the masked ACCESS itself is conditional on the
    // mask existing (e.g. BFS bottom-up only models the frontier probe when masked).
    inline bool edgeMaskReady(EdgeMaskDir dir, uint32_t src, size_t degree) const {
        const auto& masks = (dir == EdgeMaskDir::OUT) ? out_edge_masks_by_src
                                                      : in_edge_masks_by_src;
        return !masks.empty() && src < masks.size() && masks[src].size() == degree;
    }

    // Resolve the full per-edge mask for edge `edge_pos` of `src` in direction `dir` and
    // set the per-thread next-ref epoch hint. Returns `vertex_fallback` (and leaves the
    // epoch untouched) when the row isn't built/sized — so the mask-off path is
    // byte-identical to the per-vertex mask. Pass this result to SIM_CACHE_READ_MASKED.
    inline uint32_t resolveEdgeMaskAndEpoch(EdgeMaskDir dir, uint32_t src, size_t degree,
                                            size_t edge_pos, uint32_t vertex_fallback) {
        const auto& masks = (dir == EdgeMaskDir::OUT) ? out_edge_masks_by_src
                                                      : in_edge_masks_by_src;
        if (masks.empty() || src >= masks.size() || masks[src].size() != degree)
            return vertex_fallback;
        const auto& eps = (dir == EdgeMaskDir::OUT) ? out_edge_epoch_by_src
                                                    : in_edge_epoch_by_src;
        hints_for_thread().edge_epoch =
            (src < eps.size() && edge_pos < eps[src].size()) ? eps[src][edge_pos] : 0;
        hints_for_thread().edge_epoch_valid = true;  // a real per-edge epoch was delivered
        // ECG_REUSE_PLAN_DEPTH: deliver the direction-matched forward schedule.
        // two-epoch ReusePlan rows for both directions come from the shared epoch builder;
        // PR retains its legacy IN-direction K3/K4 ablation support.
        {
            auto& H = hints_for_thread();
            const auto& schedules = (dir == EdgeMaskDir::OUT)
                ? out_edge_epoch_sched_by_src : in_edge_epoch_sched_by_src;
            if (edge_epoch_reuse_plan_depth && src < schedules.size()) {
                const auto& sc = schedules[src];
                uint32_t K = edge_epoch_reuse_plan_depth;
                uint8_t kn = static_cast<uint8_t>(std::min<uint32_t>(K, 4));
                if (edge_pos * K + kn <= sc.size()) {
                    H.edge_epoch_sched_n = kn;
                    for (uint8_t k = 0; k < kn; ++k)
                        H.edge_epoch_sched[k] = sc[edge_pos * K + k];
                } else {
                    H.edge_epoch_sched_n = 0;
                }
            } else {
                H.edge_epoch_sched_n = 0;
            }
            const auto& tiers = (dir == EdgeMaskDir::OUT)
                ? out_edge_grasp_tier_by_src : in_edge_grasp_tier_by_src;
            if (src < tiers.size() && edge_pos < tiers[src].size()) {
                H.edge_grasp_tier = tiers[src][edge_pos];
                H.edge_grasp_tier_valid = H.edge_grasp_tier != 0;
            } else {
                H.edge_grasp_tier = 0;
                H.edge_grasp_tier_valid = false;
            }
        }
        return masks[src][edge_pos];
    }

    // Clear the sticky per-edge epoch before a non-edge (SEQUENTIAL source) read so its
    // fill isn't stamped with the previous edge's stale neighbour epoch (cache_sim.h
    // stamps ecg_epoch from edge_epoch on every ECG_GRASP_POPT fill). No-op when no
    // edge-mask set is built, so default paths stay byte-identical.
    inline void clearEdgeEpoch() {
        if (!out_edge_masks_by_src.empty() || !in_edge_masks_by_src.empty()) {
            hints_for_thread().edge_epoch = 0;
            hints_for_thread().edge_epoch_valid = false;  // sequential read: NOT a delivery
            hints_for_thread().edge_epoch_sched_n = 0;
            hints_for_thread().edge_grasp_tier = 0;
            hints_for_thread().edge_grasp_tier_valid = false;
        }
    }

    void printECGStats(std::ostream& os = std::cout) const {
        if (!mask_config.enabled) return;
        os << "ECG Mask Stats: build_s=" << std::fixed << std::setprecision(6)
           << (double(ecg_stats.mask_build_us) / 1000000.0)
           << " vertices=" << ecg_stats.mask_vertices
           << " pfx_candidates=" << ecg_stats.pfx_candidates
           << " pfx_encoded=" << ecg_stats.pfx_encoded
           << " pfx_no_candidate=" << ecg_stats.pfx_no_candidate
           << " pfx_table_miss=" << ecg_stats.pfx_table_miss
           << " pfx_dedup_skips=" << ecg_stats.pfx_dedup_skips
           << " runtime_no_target=" << ecg_stats.runtime_pfx_no_target.load()
           << " runtime_duplicate=" << ecg_stats.runtime_pfx_duplicate.load()
           << " runtime_issued=" << ecg_stats.runtime_pfx_issued.load()
           << std::defaultfloat << "\n";
    }

    std::string ecgStatsJSON() const {
        std::ostringstream ss;
        ss << "{"
           << "\"mask_build_us\":" << ecg_stats.mask_build_us << ","
           << "\"mask_vertices\":" << ecg_stats.mask_vertices << ","
           << "\"pfx_candidates\":" << ecg_stats.pfx_candidates << ","
           << "\"pfx_encoded\":" << ecg_stats.pfx_encoded << ","
           << "\"pfx_no_candidate\":" << ecg_stats.pfx_no_candidate << ","
           << "\"pfx_table_miss\":" << ecg_stats.pfx_table_miss << ","
           << "\"pfx_dedup_skips\":" << ecg_stats.pfx_dedup_skips << ","
           << "\"runtime_no_target\":" << ecg_stats.runtime_pfx_no_target.load() << ","
           << "\"runtime_duplicate\":" << ecg_stats.runtime_pfx_duplicate.load() << ","
           << "\"runtime_issued\":" << ecg_stats.runtime_pfx_issued.load()
           << "}";
        return ss.str();
    }

    // ================================================================
    // Diagnostics
    // ================================================================

    void printSummary(std::ostream& os = std::cout) const {
        os << "\n=== Graph Cache Context ===\n";
        os << "Vertices: " << topology.num_vertices
           << "  Edges: " << topology.num_edges
           << "  AvgDeg: " << topology.avg_degree
           << "  MaxDeg: " << topology.max_degree << "\n";
        os << "Property Regions: " << num_regions << "\n";
        for (uint32_t i = 0; i < num_regions; ++i) {
            const auto& r = regions[i];
            uint64_t total = r.upper_bound - r.base_address;
            os << "  [" << i << "] "
               << total << "B (elem=" << r.elem_size << "B × " << r.num_elements << ")"
               << "  " << r.num_buckets << " buckets\n";
        }
        if (rereference.matrix) {
            os << "Rereference: " << rereference.num_epochs << " epochs × "
               << rereference.num_cache_lines << " cache lines\n";
        }
        if (vertex_stats.enabled) {
            os << "Per-vertex stats: enabled (" << vertex_stats.accesses.size() << " vertices)\n";
        }
        if (fat_id.enabled) {
            fat_id.printConfig(os);
        }
        os << std::defaultfloat << "===========================\n";
    }
};

} // namespace cache_sim

#endif // GRAPH_CACHE_CONTEXT_H_
