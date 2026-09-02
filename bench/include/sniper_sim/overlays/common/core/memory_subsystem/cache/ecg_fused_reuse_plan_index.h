#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ecg_reuse_plan {

inline bool findFusedDestination(
        const std::vector<uint32_t>& sorted_destinations,
        uint64_t begin, uint64_t end,
        uint32_t first_destination, uint32_t past_last_destination,
        uint64_t& position) {
    if (begin > end || end > sorted_destinations.size() ||
        first_destination >= past_last_destination) {
        return false;
    }
    const auto found = std::lower_bound(
        sorted_destinations.begin() + begin,
        sorted_destinations.begin() + end,
        first_destination);
    if (found == sorted_destinations.begin() + end ||
        *found >= past_last_destination) {
        return false;
    }
    position = static_cast<uint64_t>(
        found - sorted_destinations.begin());
    return true;
}

}  // namespace ecg_reuse_plan
