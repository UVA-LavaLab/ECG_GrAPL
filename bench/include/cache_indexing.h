#ifndef GRAPHBREW_CACHE_INDEXING_H
#define GRAPHBREW_CACHE_INDEXING_H

#include <cstdint>
#include <limits>

namespace cache_indexing {

inline bool split(
        uint64_t address, unsigned shift, uint64_t sets, uint64_t& tag, uint64_t& set) {
    tag = set = 0;
    if (!sets || shift >= 64)
        return false;
    const uint64_t line = address >> shift;
    tag = line / sets;
    set = line % sets;
    return true;
}

inline bool restore(
        uint64_t tag, uint64_t set, unsigned shift, uint64_t sets, uint64_t& address) {
    address = 0;
    if (!sets || shift >= 64 || set >= sets)
        return false;
    const uint64_t maximum_line = std::numeric_limits<uint64_t>::max() >> shift;
    if (tag > maximum_line / sets || set > maximum_line - tag * sets)
        return false;
    address = (tag * sets + set) << shift;
    return true;
}

} // namespace cache_indexing

#endif
