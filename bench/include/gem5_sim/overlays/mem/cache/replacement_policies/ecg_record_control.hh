#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_CONTROL_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ECG_RECORD_CONTROL_HH__

#include <cstdint>

namespace gem5
{

class EcgRecordControl
{
  public:
    virtual ~EcgRecordControl() = default;
    virtual uint64_t pendingWork() const = 0;
};

} // namespace gem5

#endif
