#ifndef POPT_REREFERENCE_H
#define POPT_REREFERENCE_H

#include <cstdint>

namespace popt_reref {

enum class Encoding : uint8_t { Full, SingleEpoch };
enum class PostFinal : uint8_t { Later, Distant };

constexpr uint8_t kAbsent = 0x80;
constexpr uint8_t kNextPresent = 0x40;
constexpr uint8_t kSingleEpochMask = 0x3f;

constexpr uint32_t subEpochBins(Encoding encoding) {
    return encoding == Encoding::SingleEpoch ? 64 : 128;
}

constexpr uint32_t maxRank(Encoding encoding) {
    return subEpochBins(encoding) - 1;
}

// Section VII-B does not specify the post-final rank when next-present is
// clear. Keep both interpretations explicit; neither reads a second column.
constexpr uint32_t singleEpochNextRef(
        uint8_t entry, uint32_t current_sub_epoch, bool has_next_epoch,
        PostFinal postfinal) {
    const uint32_t value = entry & kSingleEpochMask;
    if (entry & kAbsent) return value;
    if (current_sub_epoch <= value) return 0;
    if (!has_next_epoch) return kSingleEpochMask;
    if (entry & kNextPresent) return 1;
    return postfinal == PostFinal::Later ? 2 : kSingleEpochMask;
}

}  // namespace popt_reref

#endif
