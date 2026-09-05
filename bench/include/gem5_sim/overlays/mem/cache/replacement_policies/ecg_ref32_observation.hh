#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_OBSERVATION_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_REF32_OBSERVATION_HH__

#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "ecg_ref32.h"
#include "mem/request.hh"

namespace gem5
{
namespace replacement_policy
{
namespace graph
{

class EcgRef32ObservationExtension
    : public Extension<Request, EcgRef32ObservationExtension>
{
  public:
    EcgRef32ObservationExtension(
            uint16_t context, uint32_t sequence, uint32_t destination,
            bool conflict = false)
        : context_(context), sequence_(sequence), destination_(destination),
          conflict_(conflict)
    {}

    std::unique_ptr<ExtensionBase> clone() const override
    {
        return std::make_unique<EcgRef32ObservationExtension>(*this);
    }

    uint16_t context() const { return context_; }
    uint32_t sequence() const { return sequence_; }
    uint32_t destination() const { return destination_; }
    bool conflicted() const { return conflict_; }

  private:
    uint16_t context_;
    uint32_t sequence_;
    uint32_t destination_;
    bool conflict_;
};

struct EcgRef32Observation
{
    uint16_t context = 0;
    uint32_t sequence = 0;
    uint32_t destination = 0;
    bool conflict = false;
};

inline void
attachEcgRef32Observation(
        const RequestPtr& request, uint16_t context,
        uint32_t sequence, uint32_t destination)
{
    if (!request)
        return;
    request->setExtension(
        std::make_shared<EcgRef32ObservationExtension>(
            context, sequence, destination));
}

inline bool
readEcgRef32Observation(
        const RequestPtr& request, EcgRef32Observation& observation)
{
    if (!request)
        return false;
    const auto extension =
        request->getExtension<EcgRef32ObservationExtension>();
    if (!extension)
        return false;
    observation.context = extension->context();
    observation.sequence = extension->sequence();
    observation.destination = extension->destination();
    observation.conflict = extension->conflicted();
    return true;
}

class EcgRef32ObservationMshrState
{
  public:
    void reset()
    {
        valid_ = false;
        conflict_ = false;
        requestor_ = Request::invldRequestorId;
        selected_ = EcgRef32Observation{};
    }

    void merge(const RequestPtr& request)
    {
        EcgRef32Observation incoming;
        if (!readEcgRef32Observation(request, incoming))
            return;
        if (incoming.conflict || incoming.context == 0) {
            conflict_ = true;
            return;
        }
        if (!valid_) {
            valid_ = true;
            requestor_ = request->requestorId();
            selected_ = incoming;
            return;
        }
        if (requestor_ != request->requestorId() ||
            selected_.context != incoming.context) {
            conflict_ = true;
            return;
        }

        const ecg_ref32::SequenceOrder order =
            ecg_ref32::compareSequence32(
                incoming.sequence, selected_.sequence);
        if (order == ecg_ref32::SequenceOrder::NEWER) {
            selected_ = incoming;
        } else if (order == ecg_ref32::SequenceOrder::EQUAL) {
            if (incoming.destination != selected_.destination)
                conflict_ = true;
        } else if (order == ecg_ref32::SequenceOrder::AMBIGUOUS) {
            conflict_ = true;
        }
    }

    void apply(const RequestPtr& request) const
    {
        if (!request)
            return;
        if (!valid_ && !conflict_) {
            request->removeExtension<EcgRef32ObservationExtension>();
            return;
        }
        const EcgRef32Observation observation =
            valid_ ? selected_ : EcgRef32Observation{};
        request->setExtension(
            std::make_shared<EcgRef32ObservationExtension>(
                observation.context, observation.sequence,
                observation.destination, conflict_));
    }

    bool valid() const { return valid_; }
    bool conflicted() const { return conflict_; }
    const EcgRef32Observation& selected() const { return selected_; }

  private:
    bool valid_ = false;
    bool conflict_ = false;
    RequestorID requestor_ = Request::invldRequestorId;
    EcgRef32Observation selected_;
};

} // namespace graph
} // namespace replacement_policy
} // namespace gem5

#endif
