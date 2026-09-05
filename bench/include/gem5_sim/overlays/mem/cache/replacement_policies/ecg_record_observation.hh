#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_OBSERVATION_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_OBSERVATION_HH__

#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "ecg_record_native.h"
#include "mem/request.hh"

namespace gem5
{
namespace replacement_policy
{
namespace graph
{

struct EcgRecordObservation
{
    uint64_t generation = 0;
    uint64_t sequence = 0;
    uint64_t destination = 0;
    uint64_t propertyVaddr = 0;
    uint16_t context = 0;
    bool dead = false;
    bool conflict = false;
};

class EcgRecordObservationExtension
    : public Extension<Request, EcgRecordObservationExtension>
{
  public:
    EcgRecordObservation observation;

    explicit EcgRecordObservationExtension(const EcgRecordObservation& value)
        : observation(value)
    {}

    std::unique_ptr<ExtensionBase> clone() const override
    {
        return std::make_unique<EcgRecordObservationExtension>(*this);
    }
};

inline void
attachEcgRecordObservation(
    const RequestPtr& request, const ecg_record::NativeLoadResult& load)
{
    EcgRecordObservation observation;
    observation.generation = load.generation;
    observation.sequence = load.sequence;
    observation.destination = load.destination;
    observation.propertyVaddr = load.property_address;
    observation.context = load.context;
    observation.dead = load.state == ecg_record::State::DEAD;
    request->setExtension(
        std::make_shared<EcgRecordObservationExtension>(observation));
}

inline bool
readEcgRecordObservation(
    const RequestPtr& request, EcgRecordObservation& observation)
{
    if (!request)
        return false;
    const auto extension = request->getExtension<EcgRecordObservationExtension>();
    if (!extension)
        return false;
    observation = extension->observation;
    return true;
}

inline bool
isEcgRecordDead(const RequestPtr& request)
{
    EcgRecordObservation observation;
    return readEcgRecordObservation(request, observation) &&
        observation.context != 0 && observation.sequence != 0 &&
        observation.dead && !observation.conflict;
}

class EcgRecordObservationMshrState
{
  public:
    void reset()
    {
        valid_ = false;
        conflict_ = false;
        requestor_ = Request::invldRequestorId;
        selected_ = {};
    }

    void merge(const RequestPtr& request)
    {
        EcgRecordObservation incoming;
        if (!readEcgRecordObservation(request, incoming))
            return;
        if (incoming.conflict || incoming.context == 0 || incoming.sequence == 0) {
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
            selected_.context != incoming.context ||
            selected_.generation != incoming.generation) {
            conflict_ = true;
            return;
        }
        if (incoming.sequence > selected_.sequence) {
            selected_ = incoming;
        } else if (incoming.sequence == selected_.sequence &&
                   (incoming.destination != selected_.destination ||
                    incoming.propertyVaddr != selected_.propertyVaddr ||
                    incoming.dead != selected_.dead)) {
            conflict_ = true;
        }
    }

    void apply(const RequestPtr& request) const
    {
        if (!valid_ && !conflict_) {
            request->removeExtension<EcgRecordObservationExtension>();
            return;
        }
        auto observation = selected_;
        observation.conflict = conflict_;
        request->setExtension(
            std::make_shared<EcgRecordObservationExtension>(observation));
    }

    bool valid() const { return valid_; }
    bool conflicted() const { return conflict_; }
    const EcgRecordObservation& selected() const { return selected_; }

  private:
    bool valid_ = false;
    bool conflict_ = false;
    RequestorID requestor_ = Request::invldRequestorId;
    EcgRecordObservation selected_;
};

} // namespace graph
} // namespace replacement_policy
} // namespace gem5

#endif
