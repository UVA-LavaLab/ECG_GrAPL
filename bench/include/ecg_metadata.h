// Canonical ECG metadata transport definition for all simulators.
//
// This is the transport counterpart to `ecg_victim_policy.h`. That header owns
// the eviction DECISION and is byte-identical across cache_sim, gem5 and
// Sniper; this one owns how a per-edge K2 record reaches the policy, and is
// shared the same way. Neither depends on any simulator type.
//
// WHY THIS EXISTS. Metadata delivery had accumulated in three places at once:
// width helpers in graph_sim.h, a different if/else chain in each of the five
// cache_sim kernels, and separate array construction in each gem5 kernel. That
// produced real, shipped bugs -- a Schedule-2 shortcut that returned 8 bytes
// without consulting the bit budget, a weighted sidecar sized with destination
// id bits it never needs, and kernels that disagreed about which structure they
// were even using. One definition, used by every kernel on every simulator,
// removes that class of bug rather than patching instances of it.
//
// THE TWO STRUCTURES.
//
//   PackedRecord (S1): destination id + tier + stamps in ONE word that
//   SUBSTITUTES for the CSR edge read. Free while it fits in 4 bytes, doubles
//   the edge stream when id_bits push it to 8. Width therefore scales with the
//   graph, which caps a two-stamp 4-byte record near 67M vertices.
//
//   Sidecar (S2): the CSR edge is read unmodified and a narrow bit-packed
//   sidecar carries ONLY stamps and tier. It needs no destination id because
//   the edge still carries it, so its width is INDEPENDENT of graph size.
//   Valid for K2-M, which receives an already-computed property address; K2-I
//   fuses address generation and does need the id in the operand.
//
// RESEARCH KNOBS. Every axis is explicit, validated, and reported in one
// receipt line so no run is ambiguous about what it measured:
//
//   ECG_DELIVERY              packed | sidecar | none   (default packed)
//   ECG_EDGE_MASK_SCHED       stamps per record, 1 or 2
//   ECG_EDGE_MASK_EPOCHS      epoch count -> epoch_bits
//   ECG_RECORD_TIER_BITS      tier bits carried (default 2)
//   ECG_EDGE_RECORD_BYTES     force packed container width (4/8/16)
//   ECG_SIDECAR_PAYLOAD_BITS  force sidecar payload width
//   ECG_RECORD_VARIABLE_WIDTH compute Schedule-2 width instead of forcing 8
//   ECG_VIRTUAL_ID_BITS       pretend the graph needs N id bits, so format
//                             width can be swept WITHOUT changing topology
//   ECG_EDGE_MASK_CHARGED     0 = metadata delivered free (oracle ceiling)
//   ECG_STREAM_BYPASS         metadata stream does not allocate in LLC
#ifndef ECG_METADATA_H
#define ECG_METADATA_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ecg_metadata {

enum class Delivery { PackedRecord, Sidecar, None };

// Synthetic bases for simulators that model the streams rather than allocating
// them. Distinct per direction and per structure so a run can never alias two
// structures, and far above any real graph allocation.
static constexpr uint64_t kInRecordBase   = 0x100000000000ULL;
static constexpr uint64_t kOutRecordBase  = 0x200000000000ULL;
static constexpr uint64_t kInSidecarBase  = 0x300000000000ULL;
static constexpr uint64_t kOutSidecarBase = 0x400000000000ULL;

struct Config {
    Delivery delivery = Delivery::PackedRecord;
    int stamps = 1;          // future references carried per edge
    int epoch_bits = 1;
    int tier_bits = 2;
    int id_bits = 1;         // destination bits the graph actually needs
    int record_bytes = 4;    // PackedRecord container width
    int payload_bits = 0;    // Sidecar bits per edge
    bool charged = true;
    bool bypass = false;
    bool packed_fits = true; // did the packed record fit 4 bytes?
};

inline int envInt(const char* name, int fallback, int lo, int hi) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;
    int value = std::atoi(raw);
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

inline int bitsFor(uint64_t count) {
    int bits = 1;
    while (bits < 32 && (uint64_t(1) << bits) < count) ++bits;
    return bits;
}

// The single width rule. Every simulator and kernel derives width from here.
inline Config configure(uint64_t num_vertices, uint32_t num_epochs) {
    Config c;
    const char* mode = std::getenv("ECG_DELIVERY");
    if (mode && std::string(mode) == "sidecar") c.delivery = Delivery::Sidecar;
    else if (mode && std::string(mode) == "none") c.delivery = Delivery::None;

    c.stamps = envInt("ECG_EDGE_MASK_SCHED", 0, 0, 4);
    if (c.stamps < 1) c.stamps = 1;
    c.epoch_bits = bitsFor(num_epochs ? num_epochs : 2);
    c.tier_bits = envInt("ECG_RECORD_TIER_BITS", 2, 0, 8);
    // A virtual id width lets the format budget be swept on a FIXED graph, so a
    // width crossover cannot be confounded with a change in topology, working
    // set or epoch span.
    const int virtual_id_bits = envInt("ECG_VIRTUAL_ID_BITS", 0, 0, 31);
    c.id_bits = virtual_id_bits > 0 ? virtual_id_bits : bitsFor(num_vertices);
    c.charged = envInt("ECG_EDGE_MASK_CHARGED", 1, 0, 1) > 0;
    c.bypass = envInt("ECG_STREAM_BYPASS", 0, 0, 1) > 0;

    // Sidecar payload carries stamps and tier ONLY; no destination id, because
    // the unmodified CSR edge still delivers it. This is the whole reason the
    // structure is graph-size independent, so id_bits must never appear here.
    const int forced_payload = envInt("ECG_SIDECAR_PAYLOAD_BITS", 0, 0, 64);
    c.payload_bits = forced_payload > 0
        ? forced_payload
        : c.epoch_bits * c.stamps + c.tier_bits;

    const int needed = c.id_bits + c.epoch_bits * c.stamps + c.tier_bits +
                       envInt("ECG_RECORD_POPT_BITS", 0, 0, 8) +
                       envInt("ECG_RECORD_PREFETCH_BITS", 0, 0, 32);
    c.packed_fits = needed <= 32;
    const int forced_width = envInt("ECG_EDGE_RECORD_BYTES", 0, 0, 16);
    if (forced_width == 4 || forced_width == 8 || forced_width == 16) {
        c.record_bytes = forced_width;
    } else if (c.stamps == 2 &&
               envInt("ECG_RECORD_VARIABLE_WIDTH", 0, 0, 1) == 0) {
        // Historical behaviour: Schedule-2 returned 8 bytes without consulting
        // the budget. Kept as the default so committed results do not move, but
        // it is a shortcut, not a cost of the second stamp: on a 65,536-vertex
        // graph a two-stamp record needs 16 + 2*5 + 2 = 28 bits and fits in 4.
        c.record_bytes = 8;
    } else {
        c.record_bytes = needed <= 32 ? 4 : (needed <= 64 ? 8 : 16);
    }
    return c;
}

// Bytes of metadata streamed per edge. Sidecar is bit-packed, so many edges
// share a line and the cost is payload_bits/8, not a whole byte each.
inline double bytesPerEdge(const Config& c) {
    if (!c.charged) return 0.0;
    switch (c.delivery) {
        case Delivery::PackedRecord: return c.record_bytes;
        case Delivery::Sidecar:      return 4.0 + c.payload_bits / 8.0;
        case Delivery::None:         return 0.0;
    }
    return 0.0;
}

inline uint64_t recordAddress(
        const Config& c, uint64_t base, uint64_t edge_index) {
    return base + edge_index * static_cast<uint64_t>(c.record_bytes);
}

inline uint64_t sidecarAddress(
        const Config& c, uint64_t base, uint64_t edge_index) {
    return base + ((edge_index * static_cast<uint64_t>(c.payload_bits)) >> 3);
}

// A packed record can only SUBSTITUTE for the edge if the kernel can actually
// pack destination, and for weighted graphs the weight, alongside the stamps.
// SSSP checks that per graph. When it fails, the metadata cannot ride in place
// of the edge and must travel as a sidecar; it must never silently vanish.
inline void requirePackedFeasible(Config& c, bool feasible) {
    if (!feasible && c.delivery == Delivery::PackedRecord)
        c.delivery = Delivery::Sidecar;
}

// Declare the container a backend actually streams, when its implementation
// fixes the width independently of the bit budget.
//
// The budget says what a record COULD occupy; a backend may still materialise
// it in a wider container. gem5's Schedule-2 path builds
// `pvector<uint64_t> in_edge_pair_flat`, so it streams 8 bytes per edge no
// matter what the budget computes. Reporting the budget width there would make
// the receipt claim a 4-byte record while the guest moved 8, which is exactly
// the divergence this header exists to prevent: cache_sim measured 0.557
// against LRU on web-Google-n16 PageRank while gem5 measured 1.189 at the same
// geometry, purely because the two modelled different container widths.
inline void declareContainerBytes(Config& c, int container_bytes) {
    if (container_bytes <= 0) return;
    c.record_bytes = container_bytes;
    c.packed_fits = c.packed_fits &&
        (c.id_bits + c.epoch_bits * c.stamps + c.tier_bits) <=
            container_bytes * 8;
}

inline const char* deliveryName(const Config& c) {
    switch (c.delivery) {
        case Delivery::PackedRecord: return "packed";
        case Delivery::Sidecar:      return "sidecar";
        case Delivery::None:         return "none";
    }
    return "?";
}

// One receipt line per run. Emitted by every kernel on every simulator so a
// result can never be quoted without knowing exactly what produced it.
inline void announce(const Config& c, const char* kernel) {
    static bool announced = false;
    if (announced) return;
    announced = true;
    std::fprintf(stderr,
        "[ECG-METADATA kernel=%s delivery=%s stamps=%d epoch_bits=%d "
        "tier_bits=%d id_bits=%d record_bytes=%d payload_bits=%d "
        "bytes_per_edge=%.3f charged=%d bypass=%d packed_fits=%d]\n",
        kernel, deliveryName(c), c.stamps, c.epoch_bits, c.tier_bits,
        c.id_bits, c.record_bytes, c.payload_bits, bytesPerEdge(c),
        c.charged ? 1 : 0, c.bypass ? 1 : 0, c.packed_fits ? 1 : 0);
}

// Enforce, do not merely report.
//
// Four independent layers of environment plumbing silently defeated the same
// setting during this work -- a runner scrub, a double-encoded channel, a gem5
// SE allowlist, and a stale RISC-V binary -- and each was invisible except in
// the guest's own receipt. Printing a receipt only helps if somebody reads it.
//
// ECG_EXPECT_BYTES_PER_EDGE lets the runner state what it believes the guest
// will stream. The guest checks the value it actually derived and aborts BEFORE
// the ROI on mismatch, so a misconfigured cell fails immediately instead of
// producing a plausible-looking number hours later.
inline void enforceExpectedBytesPerEdge(const Config& c, const char* kernel) {
    const char* want = std::getenv("ECG_EXPECT_BYTES_PER_EDGE");
    if (!want || !*want) return;
    const double expected = std::atof(want);
    const double actual = bytesPerEdge(c);
    if (expected > 0.0 && (actual < expected - 1e-6 || actual > expected + 1e-6)) {
        std::fprintf(stderr,
            "[ECG-METADATA-FATAL kernel=%s expected_bytes_per_edge=%.3f "
            "actual=%.3f delivery=%s record_bytes=%d] "
            "the guest is not streaming what the runner intended; aborting "
            "before the ROI rather than producing a misconfigured result\n",
            kernel, expected, actual, deliveryName(c), c.record_bytes);
        std::abort();
    }
}

}  // namespace ecg_metadata

#endif  // ECG_METADATA_H
