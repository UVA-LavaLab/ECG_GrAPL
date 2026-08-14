// GraphBrew ECG mask REQUEST SIDEBAND (the OoO / multicore-general delivery).
//
// The single-slot ecg.extract mailbox (setDecodedEcgExtractHint) and the per-vertex
// table (storeEcgMetadataByVertex) are both compromises: the mailbox races under an
// out-of-order CPU (a later ecg.load's mask can overwrite an earlier one before its
// fill stamps the line), and the table is O(num_vertices) (the very cost ECG avoids).
//
// The race-free AND HW-realizable delivery is a per-REQUEST sideband: the ecg.load AGU
// tags the demand Request with its destination and graph mask, and the LLC reads it on
// hits and fills. Because the mask travels WITH the specific request,
// there is no shared structure to race and no per-vertex storage. gem5's Request is
// Extensible<Request>, so this is a first-class Request::Extension.
//
// In-order TimingSimpleCPU case study: loads are serialized, so the single-slot mailbox
// holds exactly the demanded vertex's mask when its access reaches the LLC -> the mailbox
// is mathematically equivalent to this sideband (no race possible). The replacement
// policy is therefore validated in-order via the mailbox; this extension is the same
// information delivered race-free for the O3CPU / multicore form. The read hook below is
// PREFERRED when present (so an O3 ecg.load that attaches it is correct), and falls back
// to the mailbox otherwise.
#ifndef GRAPHBREW_ECG_REUSE_BIND_REQUEST_EXT_HH
#define GRAPHBREW_ECG_REUSE_BIND_REQUEST_EXT_HH

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5 {
namespace replacement_policy {
namespace graph {

struct EcgReuseBindTraceState {
    std::atomic<uint64_t> next{0};
    std::mutex mutex;
    std::unordered_map<uint32_t, uint64_t> sequence_to_trace;
};

inline uint64_t ecgReuseBindTraceLimit() {
    static const uint64_t limit = []() {
        const char* value = std::getenv("ECG_REUSE_PLAN_DELIVERY_TRACE");
        return value
            ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10))
            : 0;
    }();
    return limit;
}

inline EcgReuseBindTraceState& ecgReuseBindTraceState() {
    static EcgReuseBindTraceState state;
    return state;
}

inline void traceEcgReuseBindRequest(
        uint32_t request_sequence, uint32_t dest, uint8_t tier,
        uint16_t epoch1, uint16_t epoch2,
        uint16_t current_epoch, uint16_t context_id) {
    const uint64_t limit = ecgReuseBindTraceLimit();
    if (limit == 0) return;
    auto& state = ecgReuseBindTraceState();
    const uint64_t trace_sequence =
        state.next.fetch_add(1, std::memory_order_relaxed);
    if (trace_sequence >= limit) return;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.sequence_to_trace.emplace(request_sequence, trace_sequence);
    }
    std::fprintf(
        stderr,
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=%llu request_seq=%u "
        "dest=%u tier=%u epoch1=%u epoch2=%u current=%u context=%u]\n",
        (unsigned long long)trace_sequence, request_sequence, dest,
        static_cast<unsigned>(tier), static_cast<unsigned>(epoch1),
        static_cast<unsigned>(epoch2),
        static_cast<unsigned>(current_epoch),
        static_cast<unsigned>(context_id));
}

inline bool ecgReuseBindTraceIndex(
        uint32_t request_sequence, uint64_t& trace_sequence) {
    auto& state = ecgReuseBindTraceState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.sequence_to_trace.find(request_sequence);
    if (found == state.sequence_to_trace.end()) return false;
    trace_sequence = found->second;
    return true;
}

// Per-request ECG metadata sideband. Carries the graph mask attached to the
// governed property-load Request. single-epoch ReusePlan uses one epoch; two-epoch ReusePlan carries
// the GRASP tier and both ReusePlan epochs.
class EcgReusePlanExtension
    : public gem5::Extension<gem5::Request, EcgReusePlanExtension> {
  public:
    EcgReusePlanExtension(uint32_t dest, uint16_t epoch,
                      uint8_t dbg = 0, uint8_t popt = 0,
                      uint16_t current_epoch = 0,
                      uint16_t context_id = 0,
                      uint32_t sequence = 0)
        : dest_(dest), epoch_(epoch), epoch2_(epoch), dbg_(dbg),
          popt_(popt), epoch_count_(1), current_epoch_(current_epoch),
          context_id_(context_id), sequence_(sequence), conflicted_(false) {}

    EcgReusePlanExtension(uint32_t dest, uint8_t tier,
                      uint16_t epoch1, uint16_t epoch2,
                      uint16_t current_epoch = 0,
                      uint16_t context_id = 0,
                      uint32_t sequence = 0)
        : dest_(dest), epoch_(epoch1), epoch2_(epoch2), dbg_(tier),
          popt_(0), epoch_count_(2), current_epoch_(current_epoch),
          context_id_(context_id), sequence_(sequence), conflicted_(false) {}

    EcgReusePlanExtension()
        : dest_(0), epoch_(0), epoch2_(0), dbg_(0), popt_(0),
          epoch_count_(2), current_epoch_(0), context_id_(0), sequence_(0),
          conflicted_(true) {}

    std::unique_ptr<gem5::ExtensionBase> clone() const override {
        return std::make_unique<EcgReusePlanExtension>(*this);
    }

    uint32_t dest()  const { return dest_; }
    uint16_t epoch() const { return epoch_; }
    uint16_t epoch2() const { return epoch2_; }
    uint8_t  dbg()   const { return dbg_; }
    uint8_t  popt()  const { return popt_; }
    uint8_t  epochCount() const { return epoch_count_; }
    uint16_t currentEpoch() const { return current_epoch_; }
    uint16_t contextId() const { return context_id_; }
    uint32_t sequence() const { return sequence_; }
    bool conflicted() const { return conflicted_; }
    bool validContext() const { return context_id_ != 0 && !conflicted_; }

    void markConflicted() { conflicted_ = true; }

    bool samePayload(const EcgReusePlanExtension& other) const {
        return dest_ == other.dest_ &&
               epoch_ == other.epoch_ &&
               epoch2_ == other.epoch2_ &&
               dbg_ == other.dbg_ &&
               popt_ == other.popt_ &&
               epoch_count_ == other.epoch_count_ &&
               current_epoch_ == other.current_epoch_ &&
               context_id_ == other.context_id_ &&
               sequence_ == other.sequence_;
    }

  private:
    uint32_t dest_;
    uint16_t epoch_;
    uint16_t epoch2_;
    uint8_t  dbg_;
    uint8_t  popt_;
    uint8_t  epoch_count_;
    uint16_t current_epoch_;
    uint16_t context_id_;
    uint32_t sequence_;
    bool conflicted_;
};

// O3/OoO ATTACH (the ecg.load AGU side): tag the demand request with its epoch sideband.
// For the in-order case study this is unused (the mailbox is the equivalent model); a
// custom ecg.load format's initiateAcc calls this on the request it issues.
inline void attachEcgEpoch(const gem5::RequestPtr& req, uint32_t dest, uint16_t epoch,
                           uint8_t dbg = 0, uint8_t popt = 0,
                           uint16_t current_epoch = 0,
                           uint16_t context_id = 0,
                           uint32_t sequence = 0) {
    if (req) {
        req->setExtension(
            std::make_shared<EcgReusePlanExtension>(
                dest, epoch, dbg, popt,
                current_epoch, context_id, sequence));
    }
}

inline void attachEcgReusePlan(const gem5::RequestPtr& req, uint32_t dest,
                               uint8_t tier,
                               uint16_t epoch1, uint16_t epoch2,
                               uint16_t current_epoch = 0,
                               uint16_t context_id = 0,
                               uint32_t sequence = 0) {
    if (req) {
        traceEcgReuseBindRequest(
            sequence, dest, tier, epoch1, epoch2,
            current_epoch, context_id);
        req->setExtension(std::make_shared<EcgReusePlanExtension>(
            dest, tier, epoch1, epoch2,
            current_epoch, context_id, sequence));
    }
}

inline void copyEcgReusePlanExtension(const gem5::RequestPtr& dest,
                                  const gem5::RequestPtr& source) {
    if (!dest || !source) return;
    auto ext = source->getExtension<EcgReusePlanExtension>();
    if (ext) {
        dest->setExtension(std::make_shared<EcgReusePlanExtension>(*ext));
    } else {
        dest->removeExtension<EcgReusePlanExtension>();
    }
}

inline void markEcgReuseBindConflict(const gem5::RequestPtr& req) {
    if (!req) return;
    auto ext = req->getExtension<EcgReusePlanExtension>();
    if (ext) {
        ext->markConflicted();
    } else {
        req->setExtension(std::make_shared<EcgReusePlanExtension>());
    }
}

class EcgReuseBindMshrState {
  public:
    void reset() {
        saw_target_ = false;
        saw_reuse_plan_ = false;
        conflicted_ = false;
        requestor_id_ = gem5::Request::invldRequestorId;
        context_id_ = 0;
        sequence_ = 0;
        selected_.reset();
    }

    void merge(const gem5::RequestPtr& req) {
        auto ext = req ? req->getExtension<EcgReusePlanExtension>() : nullptr;
        const bool is_reuse_plan = ext && ext->epochCount() == 2;
        const bool had_target = saw_target_;
        saw_target_ = true;

        if (!is_reuse_plan) {
            if (saw_reuse_plan_) setConflict();
            return;
        }

        saw_reuse_plan_ = true;
        if (conflicted_) {
            if (selected_) markEcgReuseBindConflict(selected_);
            return;
        }
        if (!ext->validContext()) {
            selected_ = req;
            requestor_id_ = req->requestorId();
            context_id_ = ext->contextId();
            sequence_ = ext->sequence();
            setConflict();
            return;
        }
        if (had_target && !selected_) {
            selected_ = req;
            requestor_id_ = req->requestorId();
            context_id_ = ext->contextId();
            sequence_ = ext->sequence();
            setConflict();
            return;
        }
        if (!selected_) {
            selected_ = req;
            requestor_id_ = req->requestorId();
            context_id_ = ext->contextId();
            sequence_ = ext->sequence();
            return;
        }
        if (requestor_id_ != req->requestorId() ||
            context_id_ != ext->contextId()) {
            setConflict();
            return;
        }
        if (ext->sequence() > sequence_) {
            copyEcgReusePlanExtension(selected_, req);
            sequence_ = ext->sequence();
        } else if (ext->sequence() == sequence_) {
            auto selected_ext =
                selected_->getExtension<EcgReusePlanExtension>();
            if (!selected_ext || !selected_ext->samePayload(*ext))
                setConflict();
        }
    }

    void apply(const gem5::RequestPtr& req) const {
        if (!req || !saw_reuse_plan_) return;
        auto downstream = req->getExtension<EcgReusePlanExtension>();
        if (downstream && downstream->conflicted()) return;
        if (selected_) copyEcgReusePlanExtension(req, selected_);
        if (conflicted_) markEcgReuseBindConflict(req);
    }

    bool conflicted() const { return conflicted_; }

  private:
    void setConflict() {
        conflicted_ = true;
        if (selected_) markEcgReuseBindConflict(selected_);
    }

    bool saw_target_ = false;
    bool saw_reuse_plan_ = false;
    bool conflicted_ = false;
    gem5::RequestorID requestor_id_ = gem5::Request::invldRequestorId;
    uint16_t context_id_ = 0;
    uint32_t sequence_ = 0;
    gem5::RequestPtr selected_;
};

// LLC fill READ (the replacement-policy side): if the request carries the sideband,
// return its metadata. Race-free under OoO (no shared mailbox).
inline bool readEcgEpoch(const gem5::RequestPtr& req, uint16_t& epoch_out,
                         uint8_t& dbg_out, uint8_t& popt_out) {
    if (!req) return false;
    auto ext = req->getExtension<EcgReusePlanExtension>();
    if (!ext || ext->conflicted()) return false;
    epoch_out = ext->epoch();
    dbg_out = ext->dbg();
    popt_out = ext->popt();
    return true;
}

// Same, but also returns the ecg.load's dest vertex so the consumer can assert the
// epoch maps to the filled line (the dest-guard — cheap correctness check under OoO
// coalescing; the mailbox path has an equivalent vertex guard).
inline bool readEcgEpoch(const gem5::RequestPtr& req, uint16_t& epoch_out,
                         uint8_t& dbg_out, uint8_t& popt_out, uint32_t& dest_out) {
    if (!req) return false;
    auto ext = req->getExtension<EcgReusePlanExtension>();
    if (!ext || ext->conflicted()) return false;
    epoch_out = ext->epoch();
    dbg_out = ext->dbg();
    popt_out = ext->popt();
    dest_out = ext->dest();
    return true;
}

inline bool readEcgReusePlan(const gem5::RequestPtr& req,
                             uint16_t& epoch1_out, uint16_t& epoch2_out,
                             uint8_t& dbg_out, uint8_t& popt_out,
                             uint8_t& count_out,
                             uint32_t& dest_out) {
    if (!req) return false;
    auto ext = req->getExtension<EcgReusePlanExtension>();
    if (!ext || ext->conflicted()) return false;
    epoch1_out = ext->epoch();
    epoch2_out = ext->epoch2();
    dbg_out = ext->dbg();
    popt_out = ext->popt();
    count_out = ext->epochCount();
    dest_out = ext->dest();
    return true;
}

inline bool readEcgReusePlan(const gem5::RequestPtr& req,
                             uint16_t& epoch1_out, uint16_t& epoch2_out,
                             uint8_t& dbg_out, uint8_t& popt_out,
                             uint8_t& count_out, uint32_t& dest_out,
                             uint16_t& current_epoch_out,
                             uint16_t& context_id_out,
                             uint32_t& sequence_out) {
    if (!readEcgReusePlan(
            req, epoch1_out, epoch2_out, dbg_out, popt_out,
            count_out, dest_out))
        return false;
    auto ext = req->getExtension<EcgReusePlanExtension>();
    if (!ext || !ext->validContext()) return false;
    current_epoch_out = ext->currentEpoch();
    context_id_out = ext->contextId();
    sequence_out = ext->sequence();
    return true;
}

}  // namespace graph
}  // namespace replacement_policy
}  // namespace gem5

#endif  // GRAPHBREW_ECG_REUSE_BIND_REQUEST_EXT_HH
