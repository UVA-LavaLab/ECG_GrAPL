#ifndef __MEM_CACHE_PREFETCH_ECG_RECORD_PREFETCH_REQUEST_HH__
#define __MEM_CACHE_PREFETCH_ECG_RECORD_PREFETCH_REQUEST_HH__

#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5
{

class BaseCache;
class Packet;
struct EcgRecordFillDecision;

class EcgRecordPrefetchFilter
{
  public:
    virtual ~EcgRecordPrefetchFilter() = default;
    virtual void deferFill(
        BaseCache* cache, Packet* packet,
        const std::shared_ptr<EcgRecordFillDecision>& decision) = 0;
};

struct EcgRecordFillDecision
{
    EcgRecordPrefetchFilter* owner = nullptr;
    uint64_t ticket = 0;
    unsigned slot = 0;
    bool ready = false;
    bool admitted = false;
    bool cacheFill = false;
    bool allocated = false;
    bool mergedDemand = false;
};

class EcgRecordPrefetchExtension : public Extension<Request, EcgRecordPrefetchExtension>
{
  public:
    std::shared_ptr<EcgRecordFillDecision> decision;

    explicit EcgRecordPrefetchExtension(std::shared_ptr<EcgRecordFillDecision> value)
        : decision(std::move(value))
    {}

    std::unique_ptr<ExtensionBase> clone() const override
    {
        return std::make_unique<EcgRecordPrefetchExtension>(*this);
    }
};

inline std::shared_ptr<EcgRecordFillDecision>
ecgRecordFillDecision(const RequestPtr& request)
{
    if (!request)
        return {};
    const auto extension = request->getExtension<EcgRecordPrefetchExtension>();
    return extension ? extension->decision : nullptr;
}

} // namespace gem5

#endif
