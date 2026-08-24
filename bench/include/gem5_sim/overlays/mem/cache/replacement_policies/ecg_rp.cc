// ============================================================================
// ECG Replacement Policy for gem5 - Implementation
// ============================================================================
// Reference: Mughrabi et al., GrAPL 2026 + bench/include/cache_sim/cache_sim.h
//
// Sideband-loading gem5 port of cache_sim.h::findVictimECG. The policy keeps
// gem5-specific context loading, but mode ordering mirrors cache_sim.
// ============================================================================

#include "mem/cache/replacement_policies/ecg_rp.hh"

#include "mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh"
#include "mem/cache/replacement_policies/ecg_victim_policy.hh"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <vector>

namespace gem5 {
namespace replacement_policy {

namespace {
inline bool requestBoundEcgProducerEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("GEM5_ECG_PRODUCER");
        return value && value[0] && std::string(value) != "0";
    }();
    return enabled;
}

inline void traceAcceptedReusePlan(
        uint32_t request_sequence, uint32_t request_dest,
        uint32_t fill_dest, bool request_bound,
        uint8_t tier, uint16_t first, uint16_t second,
        uint16_t current_epoch, uint16_t context_id,
        uint32_t property_elem_bytes) {
    static const uint64_t trace_limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value
            ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10))
            : 0;
    }();
    if (trace_limit == 0) return;
    uint64_t sequence = 0;
    if (request_bound) {
        if (!graph::ecgReuseBindTraceIndex(request_sequence, sequence))
            return;
    } else {
        // The serialized mailbox sequence is the originating decoded ReusePlan load.
        // Only log accepted loads that are part of the traced EXPECT/RECV prefix;
        // inner-cache hits legitimately never reach this LLC callback.
        sequence = request_sequence;
    }
    if (sequence >= trace_limit) return;
    std::fprintf(
        stderr,
        "[ECG-ReuseBind-ACCEPT sim=gem5 seq=%llu request_seq=%u "
        "request_dest=%u fill_dest=%u "
        "source=%s tier=%u epoch1=%u epoch2=%u current=%u context=%u "
        "property_elem_bytes=%u]\n",
        (unsigned long long)sequence, request_sequence,
        request_dest, fill_dest,
        request_bound ? "request" : "mailbox", static_cast<unsigned>(tier),
        static_cast<unsigned>(first), static_cast<unsigned>(second),
        static_cast<unsigned>(current_epoch),
        static_cast<unsigned>(context_id),
        static_cast<unsigned>(property_elem_bytes));
}

// ECG GRASP-tier SOURCE (ECG_GRASP_SRC), mirrors cache_sim's two variants:
//   mask (0, our ECG): DELIVERED per-vertex graspTierByIndex keyed by the INSERTED
//     LINE's own vertex (graph::addressToVertex) — identical across simulators.
//   region (1, default, original GRASP): classifyGRASP(addr) spatial top-fraction.
// Templated on the context type to avoid spelling graph::GraphCacheContext.
template <typename Ctx>
inline uint32_t ecgGraspTier(const Ctx& ctx, uint64_t addr, uint64_t llcSize) {
    static const int gsrc = [](){
        const char* v = std::getenv("ECG_GRASP_SRC");
        return (v && std::string(v) == "mask") ? 0 : 1;  // default 1 = region
    }();
    static const double ghf = [](){
        const char* v = std::getenv("GRASP_HOT_FRACTION");
        double f = v ? std::atof(v) : 0.15;
        return (f > 0.0 && f <= 1.0) ? f : 0.15;
    }();
    if (gsrc == 0) {  // MASK (ECG): delivered per-vertex tier, byte-exact to region
        return ctx.maskGraspTier(addr, ghf);
    }
    return ctx.classifyGRASP(addr, llcSize, ghf);  // REGION (GRASP)
}

inline bool ecgReuseAdmissionEnabled() {
    static const bool enabled = ecg_policy::parseReuseAdmission(
        std::getenv("ECG_REUSE_ADMISSION"));
    return enabled;
}
}  // namespace

GraphEcgRP::GraphEcgRP(const Params &p)
    : Base(p),
      rrpvMax(p.rrpv_max),
      numBuckets(p.num_buckets),
      ecgMode(graph::stringToECGMode(p.ecg_mode)),
      llcSize(p.llc_size_bytes),
      sidebandPath(p.sideband_path),
      poptMatrixPath(p.popt_matrix_path),
      onlineDuelingStats(this)
{
    // ECG-CONFIG proof banner (ECG_DEBUG=1): same format as cache_sim/sniper, proving
    // which mode/variant this gem5 ECG replacement policy resolved. p.ecg_mode is the
    // mode string straight from the SimObject Param (no reverse lookup needed).
    const char* dbg = std::getenv("ECG_DEBUG");
    if (dbg && *dbg && std::string(dbg) != "0") {
        const char* var = std::getenv("ECG_VARIANT");
        std::cerr << "[ECG-CONFIG sim=gem5 policy=ECG mode=" << p.ecg_mode
                  << " variant=" << (var ? var : "rrip_first")
                  << " llc=" << llcSize << "B]\n";
    }
    std::cerr << "[ECG-MODE-RECEIPT sim=gem5 requested=" << p.ecg_mode
              << " effective=" << graph::ecgModeToString(ecgMode) << "]\n";
}


void
GraphEcgRP::tryLoadContext() const
{
    if (ctx.loaded &&
        (ecgMode == graph::ECGMode::ECG_GRASP_POPT || ctx.rereference.enabled)) {
        return;
    }
    loadAttempted = true;

    constexpr uint64_t retryInterval = 512;
    if ((loadAttemptCount++ % retryInterval) != 0) return;

    if (!ctx.loaded) {
        ctx.loadFromSideband(sidebandPath);
        ctx.loaded = (ctx.num_regions > 0);
    }

    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT) return;

    if (!ctx.rereference.enabled &&
        ctx.rereference.loadFromFile(poptMatrixPath)) {
        if (ctx.num_regions > 0) {
            ctx.rereference.base_address = ctx.regions[0].base_address;
        }
    }
}

void
GraphEcgRP::setVictimRequest(const PacketPtr pkt)
{
    tryLoadContext();
    victimRequestValid = false;
    victimCurrentEpoch = 0;
    victimContextId = 0;
    if (!pkt || !pkt->req) return;

    uint16_t epoch1 = 0, epoch2 = 0;
    uint8_t dbg = 0, popt = 0, count = 0;
    uint32_t dest = 0, sequence = 0;
    bool got = graph::readEcgReusePlan(
        pkt->req, epoch1, epoch2, dbg, popt, count, dest,
        victimCurrentEpoch, victimContextId, sequence);
    if (!got && !requestBoundEcgProducerEnabled()) {
        got = graph::lookupDecodedEcgRequestState(
            victimCurrentEpoch, victimContextId, sequence);
        if (!got) {
            got = legacyRequestState(
                victimCurrentEpoch, victimContextId, sequence);
        }
    }
    victimRequestValid = got && victimContextId != 0;
}

bool
GraphEcgRP::legacyRequestState(
    uint16_t& current_epoch, uint16_t& context_id,
    uint32_t& sequence) const
{
    if (!ctx.loaded || !graph::hasCurrentVertexHint()) return false;
    const uint16_t active_context = graph::getCurrentContextHint();
    if (active_context == 0) return false;
    const uint32_t n = std::max<uint32_t>(1u, ctx.topology.num_vertices);
    const uint32_t ne =
        std::max<uint32_t>(2u, ctx.topology.edge_epoch_count);
    uint32_t epoch = static_cast<uint32_t>(
        (static_cast<uint64_t>(ctx.currentVertexForPopt()) * ne) / n);
    if (epoch >= ne) epoch = ne - 1;
    current_epoch = static_cast<uint16_t>(epoch);
    context_id = active_context;
    sequence = 0;
    return true;
}

void
GraphEcgRP::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<EcgReplData>(replacement_data);
    data->rrpv = rrpvMax;
    data->ecg_dbg_tier = 0;
    data->ecg_popt_hint = 0;
    data->ecg_epoch = 0;
    data->ecg_epoch2 = 0;
    data->ecg_context_id = 0;
    data->ecg_epoch_count = 0;
    data->ecg_epoch_valid = false;
    data->valid = false;
    data->is_property_data = false;
    data->line_addr = 0;
}

void
GraphEcgRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<EcgReplData>(replacement_data);

    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT) {
        tryLoadContext();
        uint64_t addr = data->line_addr;
        if (pkt && pkt->req) {
            addr = pkt->req->hasVaddr() ? pkt->req->getVaddr()
                         : pkt->req->getPaddr();
            data->line_addr = addr & ~uint64_t(63);
        }
        data->lastTouchTick = curTick();
        if (ctx.loaded && ctx.isPropertyData(addr)) {
            data->is_property_data = true;
            bool reuse_admitted = false;
            uint32_t vertex = UINT32_MAX;
            uint64_t reg_base = 0;
            uint32_t reg_elem = 0;
            for (uint32_t ri = 0; ri < ctx.num_regions; ++ri) {
                const auto& reg = ctx.regions[ri];
                if (addr >= reg.base_address && addr < reg.upper_bound) {
                    vertex = graph::addressToVertex(addr, reg.base_address,
                                 reg.upper_bound, reg.elem_size);
                    reg_base = reg.base_address;
                    reg_elem = reg.elem_size;
                    break;
                }
            }
            if (vertex != UINT32_MAX &&
                (ecgMode != graph::ECGMode::ECG_GRASP_POPT ||
                 ctx.isEcgEpochData(addr))) {
                uint8_t isa_dbg = 0, isa_popt = 0;
                uint16_t isa_epoch = data->ecg_epoch;
                uint16_t isa_epoch2 = data->ecg_epoch2;
                uint8_t isa_count = data->ecg_epoch_count;
                uint32_t isa_dest = 0;
                uint16_t isa_current_epoch = 0;
                uint16_t isa_context_id = 0;
                uint32_t isa_sequence = 0;
                const bool got_request =
                    pkt && pkt->req && graph::readEcgReusePlan(
                    pkt->req, isa_epoch, isa_epoch2, isa_dbg, isa_popt,
                    isa_count, isa_dest, isa_current_epoch,
                    isa_context_id, isa_sequence);
                bool got = got_request;
                if (got && reg_elem > 0) {
                    const uint64_t dest_line =
                        (reg_base + static_cast<uint64_t>(isa_dest) * reg_elem) &
                        ~uint64_t(63);
                    if (dest_line != (addr & ~uint64_t(63))) got = false;
                }
                if (!got && !requestBoundEcgProducerEnabled()) {
                    const bool got_state =
                        graph::lookupDecodedEcgRequestState(
                            isa_current_epoch, isa_context_id, isa_sequence);
                    const bool got_legacy = got_state || legacyRequestState(
                        isa_current_epoch, isa_context_id, isa_sequence);
                    got = got_legacy && graph::lookupDecodedEcgHint2(
                        vertex, isa_dbg, isa_epoch, isa_epoch2, isa_count);
                    if (got) isa_dest = vertex;
                }
                if (got) {
                    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT &&
                        isa_count == 2) {
                        traceAcceptedReusePlan(
                            isa_sequence, isa_dest, vertex, got_request, isa_dbg,
                            isa_epoch, isa_epoch2, isa_current_epoch,
                            isa_context_id, reg_elem);
                    }
                    if (isa_dbg >= 1 && isa_dbg <= 3)
                        data->ecg_dbg_tier = isa_dbg;
                    data->ecg_popt_hint = isa_popt;
                    data->ecg_epoch = isa_epoch;
                    data->ecg_epoch2 = isa_epoch2;
                    data->ecg_context_id = isa_context_id;
                    data->ecg_epoch_count = isa_count;
                    data->ecg_epoch_valid = true;
                    if (ecgReuseAdmissionEnabled() &&
                        ecgMode == graph::ECGMode::ECG_GRASP_POPT &&
                        isa_count > 0) {
                        const uint32_t ne = std::max<uint32_t>(
                            2u, ctx.topology.edge_epoch_count);
                        data->rrpv = ecg_policy::reuseAdmissionRRPV(
                            isa_epoch, isa_current_epoch, ne, rrpvMax);
                        ++onlineDuelingStats.reuseAdmissionUpdates;
                        reuse_admitted = true;
                    }
                }
            }
            if (!reuse_admitted) {
                uint32_t tier =
                    data->ecg_dbg_tier >= 1 && data->ecg_dbg_tier <= 3
                    ? data->ecg_dbg_tier : ecgGraspTier(ctx, addr, llcSize);
                data->ecg_dbg_tier = static_cast<uint8_t>(tier);
                if (tier == 1) {
                    data->rrpv = 0;
                } else if (data->rrpv > 0) {
                    data->rrpv--;
                }
            }
            ctx.updateVertexFromAddr(addr);
        } else if (data->rrpv > 0) {
            data->is_property_data = false;
            data->rrpv--;
        }
        return;
    }

    if (ecgMode == graph::ECGMode::POPT_PRIMARY ||
        ecgMode == graph::ECGMode::ECG_COMBINED) {
        data->rrpv = 0;
        return;
    }

    if (ctx.loaded && ctx.isPropertyData(data->line_addr)) {
        data->is_property_data = true;
        uint32_t tier = ctx.classifyGRASP(data->line_addr, llcSize);
        if (tier == 1) {
            data->rrpv = 0;
        } else if (data->rrpv > 0) {
            data->rrpv--;
        }
        ctx.updateVertexFromAddr(data->line_addr);
    } else if (data->rrpv > 0) {
        data->rrpv--;
    }
}

void
GraphEcgRP::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<EcgReplData>(replacement_data);
    if (data->rrpv > 0) data->rrpv--;
}

void
GraphEcgRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<EcgReplData>(replacement_data);
    data->valid = true;

    tryLoadContext();

    if (pkt && pkt->req) {
        uint64_t addr = pkt->req->hasVaddr() ? pkt->req->getVaddr()
                             : pkt->req->getPaddr();
        data->line_addr = addr & ~uint64_t(63);
        data->is_property_data = ctx.loaded && ctx.isPropertyData(addr);
        data->lastTouchTick = curTick();

        constexpr uint8_t pRrip = 1;
        constexpr uint8_t iRrip = 6;
        constexpr uint8_t mRrip = 7;

        uint32_t bucket = ctx.loaded ? ctx.classifyBucket(addr)
                                     : numBuckets;
        data->ecg_dbg_tier = (bucket < numBuckets)
            ? static_cast<uint8_t>(bucket) : (numBuckets - 1);

        data->ecg_popt_hint = 0;
        data->ecg_epoch = 0;
        data->ecg_epoch2 = 0;
        data->ecg_context_id = 0;
        data->ecg_epoch_count = 0;
        data->ecg_epoch_valid = false;
            if (data->is_property_data && ctx.rereference.enabled &&
            ecgMode != graph::ECGMode::ECG_GRASP_POPT) {
            uint32_t dist = ctx.findNextRef(data->line_addr);
            data->ecg_popt_hint = static_cast<uint8_t>(
                std::min(dist, uint32_t(127)) >> 3);
        }

        // Prefer ISA-delivered metadata over the sideband.
        // When the kernel has emitted an ecg.extract opcode for the vertex
        // owning this cache line, the per-vertex metadata table holds the
        // CHARGED=0 DBG tier + POPT quantization. Prefer those over
        // the sideband-JSON-derived values. Falls back to sideband if the
        // table has no entry for this vertex.
        bool got_carried_tier = false;
        bool got_reuse_admission = false;
        uint16_t admission_current_epoch = 0;
        if (data->is_property_data && ctx.loaded && ctx.num_regions > 0) {
            uint32_t vertex = UINT32_MAX;
            uint64_t reg_base = 0; uint32_t reg_elem = 0;
            for (uint32_t ri = 0; ri < ctx.num_regions; ++ri) {
                const auto& reg = ctx.regions[ri];
                if (addr >= reg.base_address && addr < reg.upper_bound) {
                    vertex = graph::addressToVertex(addr, reg.base_address,
                                 reg.upper_bound, reg.elem_size);
                    reg_base = reg.base_address; reg_elem = reg.elem_size;
                    break;
                }
            }
            if (vertex != UINT32_MAX &&
                (ecgMode != graph::ECGMode::ECG_GRASP_POPT ||
                 ctx.isEcgEpochData(addr))) {
                uint8_t isa_dbg = 0, isa_popt = 0;
                uint16_t isa_epoch = 0;
                uint16_t isa_epoch2 = 0;
                uint8_t isa_count = 0;
                uint32_t isa_dest = 0;
                uint16_t isa_current_epoch = 0;
                uint16_t isa_context_id = 0;
                uint32_t isa_sequence = 0;
                // OoO request-sideband FIRST (race-free; an O3 ecg.load attaches the
                // graph mask to the governed property request). Falls back to the
                // in-order mailbox/table, which is equivalent for serialized loads.
                const bool got_request = graph::readEcgReusePlan(
                    pkt->req, isa_epoch, isa_epoch2, isa_dbg, isa_popt,
                    isa_count, isa_dest, isa_current_epoch,
                    isa_context_id, isa_sequence);
                bool got = got_request;
                static const uint64_t ext_trace_limit = []() {
                    const char* value = std::getenv("GEM5_ECG_EXT_TRACE");
                    return value
                        ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10))
                        : 0;
                }();
                static std::atomic<uint64_t> ext_trace_sequence{0};
                if (got && isa_count == 2 && ext_trace_limit > 0) {
                    const uint64_t sequence =
                        ext_trace_sequence.fetch_add(1, std::memory_order_relaxed);
                    if (sequence < ext_trace_limit) {
                        std::cerr
                            << "[ECG-ReusePlan-EXT-RECV sim=gem5 seq=" << sequence
                            << " dest=" << isa_dest
                            << " tier=" << static_cast<unsigned>(isa_dbg)
                            << " epoch1=" << isa_epoch
                            << " epoch2=" << isa_epoch2
                            << " fill_vertex=" << vertex << "]\n";
                    }
                }
                if (got && reg_elem > 0) {
                    // Dest-guard: accept the sideband epoch only if the ecg.load's dest
                    // maps to the SAME cache line as this fill (defends against MSHR
                    // coalescing delivering a different line's epoch). Within-line by
                    // construction in the validated A/B, so this never rejects there.
                    uint64_t dest_line =
                        (reg_base + static_cast<uint64_t>(isa_dest) * reg_elem) & ~uint64_t(63);
                    if (dest_line != (addr & ~uint64_t(63))) got = false;
                }
                if (!got && !requestBoundEcgProducerEnabled()) {
                    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT) {
                        const bool got_state =
                            graph::lookupDecodedEcgRequestState(
                                isa_current_epoch, isa_context_id,
                                isa_sequence);
                        const bool got_legacy =
                            got_state || legacyRequestState(
                                isa_current_epoch, isa_context_id,
                                isa_sequence);
                        got = got_legacy && graph::lookupDecodedEcgHint2(
                            vertex, isa_dbg, isa_epoch, isa_epoch2, isa_count);
                        if (got) isa_dest = vertex;
                    } else {
                        got = graph::lookupEcgMetadataByVertex(
                            vertex, isa_dbg, isa_popt, isa_epoch);
                        if (got) {
                            isa_epoch2 = isa_epoch;
                            isa_count = 1;
                        }
                    }
                }
                if (got) {
                    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT &&
                        isa_count == 2) {
                        traceAcceptedReusePlan(
                            isa_sequence, isa_dest, vertex, got_request, isa_dbg,
                            isa_epoch, isa_epoch2, isa_current_epoch,
                            isa_context_id, reg_elem);
                    }
                    // Use ISA-delivered metadata directly.
                    const bool valid_dbg =
                        ecgMode == graph::ECGMode::ECG_GRASP_POPT
                            ? isa_dbg >= 1 && isa_dbg <= 3
                            : true;
                    if (valid_dbg) {
                        data->ecg_dbg_tier = isa_dbg;
                        got_carried_tier = true;
                    }
                    data->ecg_popt_hint = isa_popt;  // 7-bit POPT quant
                    data->ecg_epoch = isa_epoch;
                    data->ecg_epoch2 = isa_epoch2;
                    data->ecg_context_id = isa_context_id;
                    data->ecg_epoch_count = isa_count;
                    data->ecg_epoch_valid = true;
                    got_reuse_admission = isa_count > 0;
                    admission_current_epoch = isa_current_epoch;
                } else if (ecgMode == graph::ECGMode::ECG_GRASP_POPT) {
                    // Path A: this is a prefetch FILL (the in-order demand
                    // single-slot holds a different vertex). Recover the
                    // candidate epoch the prefetch carried, from the bounded
                    // in-flight buffer; keep the degree-derived DBG tier.
                    uint16_t pf_epoch = 0;
                    uint16_t pf_context = 0;
                    const uint16_t active_context =
                        graph::getCurrentContextHint();
                    if (graph::consumePendingPrefetchEpoch(
                            vertex, active_context,
                            pf_epoch, pf_context)) {
                        data->ecg_epoch = pf_epoch;
                        data->ecg_epoch2 = pf_epoch;
                        data->ecg_context_id = pf_context;
                        data->ecg_epoch_count = 1;
                        data->ecg_epoch_valid = true;
                    }
                }
            }
        }

        if (ecgMode == graph::ECGMode::POPT_PRIMARY) {
            data->rrpv = (rrpvMax > 0) ? rrpvMax - 1 : 0;
        } else if (ecgMode == graph::ECGMode::ECG_GRASP_POPT) {
            // Non-property insertion RRPV: max (evicted before reused property, but
            // recency-aware via touch) for all variants except legacy shortcircuit,
            // whose eviction ignores rrpv. (ECG_VARIANT read in getVictim.)
            static const bool legacy_sc = [](){
                const char* v = std::getenv("ECG_VARIANT");
                return v && std::string(v) == "shortcircuit";
            }();
            if (data->is_property_data && ctx.loaded) {
                uint32_t tier = got_carried_tier
                    ? data->ecg_dbg_tier : ecgGraspTier(ctx, addr, llcSize);
                data->ecg_dbg_tier = static_cast<uint8_t>(tier);
                if (ecgReuseAdmissionEnabled() && got_reuse_admission) {
                    const uint32_t ne = std::max<uint32_t>(
                        2u, ctx.topology.edge_epoch_count);
                    data->rrpv = ecg_policy::reuseAdmissionRRPV(
                        data->ecg_epoch, admission_current_epoch, ne, rrpvMax);
                    ++onlineDuelingStats.reuseAdmissionUpdates;
                } else if (tier == 1) data->rrpv = pRrip;
                else if (tier == 2) data->rrpv = iRrip;
                else data->rrpv = mRrip;

                ctx.updateVertexFromAddr(addr);
            } else if (ctx.loaded) {
                data->rrpv = legacy_sc ? 2 : mRrip;
            } else {
                data->rrpv = 2;
            }
        } else if (ecgMode == graph::ECGMode::ECG_COMBINED) {
            uint32_t tier = 3;
            if (data->is_property_data && ctx.loaded) {
                tier = ecgGraspTier(ctx, addr, llcSize);
            }
            data->rrpv = ecg_policy::combinedInsertionRRPV(
                tier, data->ecg_popt_hint, 15, rrpvMax);
        } else if (data->is_property_data && ctx.loaded) {
            uint32_t tier = ecgGraspTier(ctx, addr, llcSize);
            if (tier == 1) data->rrpv = pRrip;
            else if (tier == 2) data->rrpv = iRrip;
            else data->rrpv = mRrip;
            ctx.updateVertexFromAddr(addr);
        } else if (ctx.loaded) {
            data->rrpv = 2;
        } else {
            data->rrpv = 2;
        }
    } else {
        data->rrpv = (rrpvMax > 0) ? rrpvMax - 1 : 0;
        data->ecg_dbg_tier = numBuckets - 1;
        data->ecg_popt_hint = 0;
        data->ecg_epoch = 0;
        data->ecg_epoch2 = 0;
        data->ecg_context_id = 0;
        data->ecg_epoch_count = 0;
        data->ecg_epoch_valid = false;
            data->is_property_data = false;
        data->line_addr = 0;
    }
}

void
GraphEcgRP::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<EcgReplData>(replacement_data);
    data->valid = true;
    data->rrpv = (rrpvMax > 0) ? rrpvMax - 1 : 0;
    data->ecg_dbg_tier = numBuckets - 1;
    data->ecg_popt_hint = 0;
    data->ecg_epoch = 0;
    data->ecg_epoch2 = 0;
    data->ecg_context_id = 0;
    data->ecg_epoch_count = 0;
    data->ecg_epoch_valid = false;
}

ReplaceableEntry*
GraphEcgRP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    auto getData = [](ReplaceableEntry* c) {
        return std::static_pointer_cast<EcgReplData>(c->replacementData);
    };
    static const bool setDueling = []() {
        const char* value = std::getenv("ECG_SET_DUELING");
        return value && value[0] && std::string(value) != "0";
    }();
    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT &&
        setDueling && victimRequestValid &&
        !candidates.empty()) {
        const size_t setIndex = candidates.front()->getSet();
        ++onlineDuelingStats.requestBoundVictims;
        // recordMiss() now returns what THIS call itself did instead of the
        // caller diffing separately-read before/after selector snapshots
        // (ecg_victim_policy.h's MissRecordEvent comment explains why that
        // diff is race-prone under concurrent callers; gem5's O3 CPU event
        // queue processes this single-threaded per instance, so the switch
        // here is for API consistency with the shared header and Sniper's
        // now-required fix, not a behavior change).
        const ecg_policy::MissRecordEvent event =
            duelingSelector.recordMiss(setIndex);
        if (event.leader_sample) ++onlineDuelingStats.leaderSamples;
        if (event.completed_window) ++onlineDuelingStats.completedWindows;
        if (event.winner_changed) ++onlineDuelingStats.winnerChanges;
    }
    for (const auto& candidate : candidates) {
        if (!getData(candidate)->valid) return candidate;
    }

    if (ecgMode == graph::ECGMode::ECG_COMBINED) {
        while (true) {
            for (const auto& c : candidates) {
                if (getData(c)->rrpv >= rrpvMax) return c;
            }
            for (const auto& c : candidates) {
                auto data = getData(c);
                if (data->rrpv < rrpvMax) data->rrpv++;
            }
        }
    }

    if (ecgMode == graph::ECGMode::ECG_GRASP_POPT && ctx.loaded) {
        // ECG_VARIANT factorial ablation (mirrors cache_sim findVictimECG).
        // Invariants in ALL variants: epoch is PROPERTY-ONLY; records evicted by
        // recency; unstamped property (epoch==0) -> recency (never "farthest").
        //   grasp_only(0): pure RRIP, no epoch
        //   epoch_first(1): records by recency, then farthest-epoch property (epoch vetoes rrpv)
        //   rrip_first(2,default): max-rrpv set (recency vetoes); records-first by recency,
        //                          then farthest-epoch property
        //   epoch_only(3): same eviction as epoch_first (insertion uniform, set in reset())
        //   shortcircuit(4,legacy): non-property first, then epoch among property
        //   record_lru(7): records by recency, then property recency; no epoch
        //   rrip_no_epoch(8): rrip_first gate/records-first, epoch distance zero
        static const int configuredVariant =
            ecg_policy::parseVariant(std::getenv("ECG_VARIANT"));
        // Ungated receipt, printed once. The runner records the variant it
        // REQUESTED; without this nothing in an archived run proves which rule
        // actually executed, so a decomposition that turns on epoch_first
        // versus lru_only rests on the request rather than on evidence.
        static const bool variantAnnounced = [&]() {
            const char* requested = std::getenv("ECG_VARIANT");
            std::fprintf(stderr,
                "[ECG-VARIANT-RECEIPT sim=gem5 requested=%s effective=%d "
                "dueling=%d]\n",
                requested ? requested : "(unset)", configuredVariant,
                setDueling ? 1 : 0);
            return true;
        }();
        (void)variantAnnounced;
        int variant = configuredVariant;
        if (setDueling && !candidates.empty()) {
            const size_t setIndex = candidates.front()->getSet();
            const bool follower =
                ecg_policy::duelingLeaderArm(setIndex) < 0;
            if (victimRequestValid && follower)
                ++onlineDuelingStats.followerSelections;
            variant = duelingSelector.variantForSet(setIndex);
            if (
                    victimRequestValid && follower &&
                    variant != configuredVariant)
                ++onlineDuelingStats.followerVariantOverrides;
        }
        const uint32_t ne = std::max<uint32_t>(2u, ctx.topology.edge_epoch_count);
        uint32_t curEpoch = victimRequestValid ? victimCurrentEpoch : 0;
        if (curEpoch >= ne) curEpoch = ne - 1;
        auto isProp = [&](ReplaceableEntry* c) {
            return ctx.isEcgEpochData(getData(c)->line_addr);
        };
        auto dist    = [&](ReplaceableEntry* c){
            auto data = getData(c);
            return ecg_policy::reusePlanDistance(
                data->ecg_epoch, data->ecg_epoch2,
                data->ecg_epoch_count, curEpoch, ne);
        };
        auto stamped = [&](ReplaceableEntry* c){
            auto data = getData(c);
            return victimRequestValid && isProp(c) &&
                   data->ecg_epoch_valid &&
                   data->ecg_context_id == victimContextId;
        };
        // ECG_EVICT_TRACE=N: emit the first N L3 evictions in cache_sim's
        // [EVICT L3 ...] format so scripts/.../verify_ecg.py asserts each victim
        // obeys the variant spec (one checker across all three simulators).
        // ECG_EVICT_TRACE_ROI=1 restricts the trace to evictions that occur AFTER the
        // kernel has begun its property traversal (hasCurrentVertexHint() — set by the
        // first GEM5_SET_VERTEX). Without it, the first N evictions are PRE-ROI graph
        // build + reorder traffic (no property stamped yet) which makes the trace
        // record/recency-only and the epoch-eviction path appear unexercised.
        static long ecgEvTrace = [](){ const char* e=std::getenv("ECG_EVICT_TRACE"); return e?std::atol(e):0L; }();
        static bool ecgEvRoi = std::getenv("ECG_EVICT_TRACE_ROI") != nullptr;
        const char* epol = (variant==1) ? "ECG:epoch_first" : "ECG:epoch_only";
        auto traced = [&](ReplaceableEntry* victimEntry, const char* pol, const char* reason)->ReplaceableEntry* {
            if (ecgEvTrace > 0 && (!ecgEvRoi || victimRequestValid)) {
                --ecgEvTrace;
                int vidx = -1;
                for (size_t i=0;i<candidates.size();++i) if (candidates[i]==victimEntry){ vidx=(int)i; break; }
                std::cerr << "[EVICT L3 pol=" << pol << " curEpoch=" << curEpoch
                          << " set_ways=" << candidates.size() << "]\n";
                for (size_t i=0;i<candidates.size();++i){
                    auto dd = getData(candidates[i]);
                    std::cerr << "   way" << i << " valid=1 rrpv=" << (int)dd->rrpv
                              << " epoch=" << dd->ecg_epoch << " dist=" << dist(candidates[i])
                              << " prop=" << (int)isProp(candidates[i])
                              << " stamped=" << (int)(stamped(candidates[i]) ? 1 : 0)
                              << " dbg=" << (int)dd->ecg_dbg_tier
                              << " last=" << dd->lastTouchTick
                              << " epoch2=" << dd->ecg_epoch2
                              << " sched_n=" << (int)dd->ecg_epoch_count
                              << ((int)i==vidx ? "   <== VICTIM" : "") << "\n";
                }
                std::cerr << "   -> victim=way" << vidx << " reason=" << reason << "\n";
            }
            return victimEntry;
        };

        // Build the per-way state and delegate the DECISION to the shared
        // ecg_policy::selectVictim (identical across cache_sim / gem5 / Sniper).
        const size_t nc = candidates.size();
        if (nc > 64) {
            std::fprintf(
                stderr,
                "[FATAL] GraphEcgRP supports at most 64 candidates, got %zu\n",
                nc);
            std::abort();
        }
        ecg_policy::WayState ws[64];
        for (size_t i = 0; i < nc; i++) {
            auto dd = getData(candidates[i]);
            ws[i].prop    = isProp(candidates[i]);
            if (ws[i].prop &&
                (dd->ecg_dbg_tier < 1 || dd->ecg_dbg_tier > 3)) {
                dd->ecg_dbg_tier = static_cast<uint8_t>(
                    ecgGraspTier(ctx, dd->line_addr, llcSize));
            }
            ws[i].rrpv    = dd->rrpv;
            ws[i].recency = dd->lastTouchTick;
            ws[i].dbg     = dd->ecg_dbg_tier;
            ws[i].dist    = dist(candidates[i]);
            ws[i].stamped = stamped(candidates[i]);
        }
        ++onlineDuelingStats.victimSelections;
        if (!victimRequestValid)
            ++onlineDuelingStats.victimRequestInvalid;
        uint64_t stampedWays = 0;
        uint64_t propertyWays = 0;
        uint64_t propertyEpochInvalidWays = 0;
        uint64_t contextMismatchWays = 0;
        for (size_t i = 0; i < nc; ++i) {
            if (ws[i].stamped) ++stampedWays;
            auto data = getData(candidates[i]);
            if (victimRequestValid && ws[i].prop) {
                ++propertyWays;
                if (!data->ecg_epoch_valid)
                    ++propertyEpochInvalidWays;
            }
            if (
                    ws[i].prop && data->ecg_epoch_valid &&
                    victimRequestValid &&
                    data->ecg_context_id != victimContextId) {
                ++contextMismatchWays;
            }
        }
        onlineDuelingStats.victimStampedWays += stampedWays;
        onlineDuelingStats.victimPropertyWays += propertyWays;
        onlineDuelingStats.victimPropertyEpochInvalidWays +=
            propertyEpochInvalidWays;
        onlineDuelingStats.victimContextMismatchWays +=
            contextMismatchWays;
        if (victimRequestValid && propertyWays == nc)
            ++onlineDuelingStats.victimAllPropertySelections;
        if (
                victimRequestValid && propertyWays == nc &&
                stampedWays > 0)
            ++onlineDuelingStats.victimAllPropertyStampedSelections;
        if (stampedWays == 0)
            ++onlineDuelingStats.victimZeroStampedSelections;

        ecg_policy::WayState noEpochWays[64];
        ecg_policy::WayState recencyNoEpochWays[64];
        for (size_t i = 0; i < nc; ++i) {
            noEpochWays[i] = ws[i];
            noEpochWays[i].stamped = false;
            noEpochWays[i].dist = 0;
            recencyNoEpochWays[i] = noEpochWays[i];
        }
        ecg_policy::VictimReason selectedReason =
            ecg_policy::VictimReason::RRIP;
        size_t vidx = ecg_policy::selectVictim(
            ws, nc, variant, rrpvMax, &selectedReason);
        const size_t noEpochVidx = ecg_policy::selectVictim(
            noEpochWays, nc, variant, rrpvMax);
        const size_t recencyNoEpochVidx = ecg_policy::selectVictim(
            recencyNoEpochWays, nc,
            ecg_policy::RRIP_NO_EPOCH_RECENCY, rrpvMax);
        if (
                victimRequestValid &&
                ecg_policy::victimUsedEpoch(selectedReason, ws[vidx]))
            ++onlineDuelingStats.victimEpochEligibleSelections;
        if (victimRequestValid && vidx != noEpochVidx)
            ++onlineDuelingStats.victimEpochDecisiveSelections;
        if (
                victimRequestValid &&
                (variant == ecg_policy::RRIP_FIRST ||
                 variant == ecg_policy::RRIP_NO_EPOCH ||
                 variant == ecg_policy::RRIP_NO_EPOCH_RECENCY) &&
                vidx != recencyNoEpochVidx)
            ++onlineDuelingStats.victimEpochVsRecencyDecisiveSelections;
        if (vidx < onlineDuelingStats.victimWaySelections.size())
            ++onlineDuelingStats.victimWaySelections[vidx];
        for (size_t i = 0; i < nc; i++) getData(candidates[i])->rrpv = ws[i].rrpv;  // persist SRRIP aging
        ReplaceableEntry* victim = candidates[vidx];

        // Reconstruct the trace pol/reason (verify_ecg.py keys on the pol name).
        const char* pol; const char* reason;
        if (variant == 0)      { pol = "ECG:grasp_only";  reason = "RRIP max-rrpv"; }
        else if (variant == 4) {
            if (!isProp(victim)) { pol = "ECG:shortcircuit";       reason = "first non-property"; }
            else                 { pol = "ECG:shortcircuit+epoch"; reason = "all-prop farthest epoch"; }
        } else if (variant == 2) {
            pol = "ECG:rrip_first";
            reason = !isProp(victim) ? "max-rrpv record by recency" : "max-rrpv farthest-epoch property";
        } else if (variant == 5) {
            pol = "ECG:degree_first";
            reason = !isProp(victim) ? "max-rrpv record by recency"
                                     : "max-rrpv coldest-degree then epoch";
        } else if (variant == 6) {
            pol = "ECG:lru_only";
            reason = "oldest recency";
        } else if (variant == 7) {
            pol = "ECG:record_lru";
            reason = !isProp(victim) ? "record by recency"
                                     : "property recency fallback";
        } else if (variant == 8) {
            pol = "ECG:rrip_no_epoch";
            reason = !isProp(victim) ? "max-rrpv record by recency"
                                     : "max-rrpv property fallback";
        } else if (variant == 9) {
            pol = "ECG:rrip_no_epoch_recency";
            reason = !isProp(victim) ? "max-rrpv record by recency"
                                     : "max-rrpv property by recency";
        } else {
            pol = epol;
            reason = !isProp(victim) ? "record by recency"
                   : stamped(victim) ? "farthest-epoch property" : "recency fallback";
        }
        return traced(victim, pol, reason);
    }

    if (ecgMode == graph::ECGMode::POPT_PRIMARY && ctx.loaded &&
        ctx.rereference.enabled) {
        int propCount = 0;
        for (const auto& c : candidates) {
            auto data = getData(c);
            data->is_property_data =
                ecgMode == graph::ECGMode::ECG_GRASP_POPT
                    ? ctx.isEcgEpochData(data->line_addr)
                    : ctx.isPropertyData(data->line_addr);
            if (data->is_property_data) propCount++;
        }

        if (propCount != static_cast<int>(candidates.size())) {
            for (const auto& c : candidates) {
                auto data = getData(c);
                if (!data->is_property_data) return c;
            }
        }

        uint8_t maxDist = 0;
        std::vector<std::pair<ReplaceableEntry*, uint8_t>> dists;
        dists.reserve(candidates.size());
        for (const auto& c : candidates) {
            uint32_t dist = ctx.findNextRef(getData(c)->line_addr);
            uint8_t d8 = static_cast<uint8_t>(std::min(dist, uint32_t(127)));
            dists.emplace_back(c, d8);
            if (d8 > maxDist) maxDist = d8;
        }

        const uint8_t maxRrpv = rrpvMax;
        while (true) {
            ReplaceableEntry* best = nullptr;
            uint8_t bestDbg = 0;
            for (auto& [c, dist] : dists) {
                auto data = getData(c);
                if (dist == maxDist && data->rrpv >= maxRrpv &&
                    (!best || data->ecg_dbg_tier > bestDbg)) {
                    best = c;
                    bestDbg = data->ecg_dbg_tier;
                }
            }
            if (best) return best;
            for (auto& [c, dist] : dists) {
                auto data = getData(c);
                if (dist == maxDist && data->rrpv < maxRrpv) data->rrpv++;
            }
        }
    }

    if (ecgMode == graph::ECGMode::DBG_ONLY) {
        while (true) {
            for (const auto& c : candidates) {
                if (getData(c)->rrpv >= rrpvMax) return c;
            }
            for (const auto& c : candidates) {
                auto data = getData(c);
                if (data->rrpv < rrpvMax) data->rrpv++;
            }
        }
    }

    while (true) {
        bool found = false;
        for (const auto& c : candidates) {
            if (getData(c)->rrpv >= rrpvMax) { found = true; break; }
        }
        if (found) break;
        for (const auto& c : candidates) {
            auto data = getData(c);
            if (data->rrpv < rrpvMax) data->rrpv++;
        }
    }

    std::vector<ReplaceableEntry*> maxCands;
    maxCands.reserve(candidates.size());
    for (const auto& c : candidates) {
        if (getData(c)->rrpv >= rrpvMax) maxCands.push_back(c);
    }
    if (maxCands.size() == 1) return maxCands[0];

    if (ecgMode == graph::ECGMode::ECG_EMBEDDED) {
        // L2: stored P-OPT hint primary (evict highest hint = furthest future).
        // L3: DBG tier tiebreak among same-hint lines.
        uint8_t maxHint = 0;
        for (const auto& c : maxCands) {
            uint8_t hint = getData(c)->ecg_popt_hint;
            if (hint > maxHint) maxHint = hint;
        }

        std::vector<ReplaceableEntry*> hintTied;
        for (const auto& c : maxCands) {
            if (getData(c)->ecg_popt_hint == maxHint) hintTied.push_back(c);
        }
        if (hintTied.size() == 1) return hintTied[0];

        uint8_t maxDbg = 0;
        ReplaceableEntry* victim = hintTied[0];
        for (const auto& c : hintTied) {
            uint8_t dbg = getData(c)->ecg_dbg_tier;
            if (dbg > maxDbg) { maxDbg = dbg; victim = c; }
        }
        return victim;
    }

    uint8_t maxDbg = 0;
    for (const auto& c : maxCands) {
        uint8_t dbg = getData(c)->ecg_dbg_tier;
        if (dbg > maxDbg) maxDbg = dbg;
    }

    std::vector<ReplaceableEntry*> dbgTied;
    for (const auto& c : maxCands) {
        if (getData(c)->ecg_dbg_tier == maxDbg) dbgTied.push_back(c);
    }
    if (dbgTied.size() == 1) {
        return dbgTied[0];
    }

    if (ctx.loaded && ctx.rereference.enabled) {
        uint32_t maxDist = 0;
        ReplaceableEntry* victim = dbgTied[0];
        for (const auto& c : dbgTied) {
            uint32_t dist = ctx.findNextRef(getData(c)->line_addr);
            if (dist > maxDist) { maxDist = dist; victim = c; }
        }
        return victim;
    }
    return dbgTied[0];
}

std::shared_ptr<ReplacementData>
GraphEcgRP::instantiateEntry()
{
    return std::make_shared<EcgReplData>(rrpvMax);
}

GraphEcgRP::OnlineDuelingStats::OnlineDuelingStats(
        statistics::Group* parent)
  : statistics::Group(parent),
    ADD_STAT(
        requestBoundVictims,
        "Number of request-bound online-dueling victim selections"),
    ADD_STAT(
        leaderSamples,
        "Number of online-dueling leader-set miss samples"),
    ADD_STAT(
        followerSelections,
        "Number of online-dueling follower-set variant selections"),
    ADD_STAT(
        completedWindows,
        "Number of online-dueling sample windows completed"),
    ADD_STAT(
        winnerChanges,
        "Number of online-dueling winner changes"),
    ADD_STAT(
        followerVariantOverrides,
        "Number of request-bound follower selections overriding static RRIP"),
    ADD_STAT(
        reuseAdmissionUpdates,
        "Number of ReusePlan future-distance RRPV admission or refresh updates"),
    ADD_STAT(
        victimSelections,
        "Number of ECG_GRASP_POPT victim selections"),
    ADD_STAT(
        victimRequestInvalid,
        "Victim selections without request-bound current epoch/context"),
    ADD_STAT(
        victimZeroStampedSelections,
        "Victim selections with zero live stamped property ways"),
    ADD_STAT(
        victimStampedWays,
        "Sum of live stamped property ways across victim selections"),
    ADD_STAT(
        victimPropertyWays,
        "Sum of resident property ways across request-valid victim selections"),
    ADD_STAT(
        victimPropertyEpochInvalidWays,
        "Sum of request-valid property ways without a delivered epoch"),
    ADD_STAT(
        victimContextMismatchWays,
        "Sum of epoch-valid property ways rejected by context binding"),
    ADD_STAT(
        victimAllPropertySelections,
        "Request-valid victim selections whose candidates are all property"),
    ADD_STAT(
        victimAllPropertyStampedSelections,
        "All-property victim selections containing at least one live stamp"),
    ADD_STAT(
        victimEpochEligibleSelections,
        "Victim selections choosing a stamped property via an epoch-capable path"),
    ADD_STAT(
        victimEpochDecisiveSelections,
        "Victim selections changed by removing all epoch metadata"),
    ADD_STAT(
        victimEpochVsRecencyDecisiveSelections,
        "RRIP-first victims changed versus a property-recency no-epoch shadow"),
    ADD_STAT(
        victimWaySelections,
        "Victim selections by candidate-way index")
{
    victimWaySelections
        .init(64)
        .flags(statistics::total | statistics::nonan);
    for (size_t way = 0; way < 64; ++way) {
        victimWaySelections.subname(
            way, "way" + std::to_string(way));
    }
}

} // namespace replacement_policy
} // namespace gem5
