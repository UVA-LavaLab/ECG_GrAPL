#ifndef __MEM_CACHE_INDEXING_POLICIES_GRAPH_MODULO_HH__
#define __MEM_CACHE_INDEXING_POLICIES_GRAPH_MODULO_HH__

#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/GraphModuloSetAssociative.hh"

namespace gem5
{

class GraphModuloSetAssociative : public BaseIndexingPolicy
{
  public:
    PARAMS(GraphModuloSetAssociative);
    explicit GraphModuloSetAssociative(const Params& params);
    Addr extractTag(Addr address) const override;
    std::vector<ReplaceableEntry*> getPossibleEntries(Addr address) const override;
    Addr regenerateAddr(Addr tag, const ReplaceableEntry* entry) const override;

  private:
    static const Params& validatedParams(const Params& params);
};

} // namespace gem5

#endif
