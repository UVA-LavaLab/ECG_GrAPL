#include "mem/cache/tags/indexing_policies/graph_modulo.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/cache_indexing.h"

namespace gem5
{

const GraphModuloSetAssociative::Params&
GraphModuloSetAssociative::validatedParams(const Params& params)
{
    fatal_if(params.assoc < 1 || params.entry_size < 4 || !isPowerOf2(params.entry_size),
             "Modulo cache indexing requires positive associativity and power-of-two lines");
    const uint64_t set_bytes = uint64_t(params.entry_size) * params.assoc;
    fatal_if(params.size % set_bytes || params.size / set_bytes == 0 ||
        params.size / set_bytes > UINT32_MAX,
        "Modulo cache indexing requires an exact positive number of representable sets");
    return params;
}

GraphModuloSetAssociative::GraphModuloSetAssociative(const Params& params)
    : BaseIndexingPolicy(validatedParams(params), true)
{}

Addr
GraphModuloSetAssociative::extractTag(Addr address) const
{
    uint64_t tag = 0, set = 0;
    fatal_if(!cache_indexing::split(address, setShift, numSets, tag, set),
             "Invalid modulo cache geometry");
    return tag;
}

std::vector<ReplaceableEntry*>
GraphModuloSetAssociative::getPossibleEntries(Addr address) const
{
    uint64_t tag = 0, set = 0;
    fatal_if(!cache_indexing::split(address, setShift, numSets, tag, set),
             "Invalid modulo cache geometry");
    return sets[set];
}

Addr
GraphModuloSetAssociative::regenerateAddr(Addr tag, const ReplaceableEntry* entry) const
{
    uint64_t address = 0;
    fatal_if(!cache_indexing::restore(tag, entry->getSet(), setShift, numSets, address),
             "Invalid modulo cache tag/set address");
    return address;
}

} // namespace gem5
