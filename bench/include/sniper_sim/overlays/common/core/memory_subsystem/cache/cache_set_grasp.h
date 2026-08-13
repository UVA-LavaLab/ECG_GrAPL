#ifndef CACHE_SET_GRASP_H
#define CACHE_SET_GRASP_H

#include "cache_set.h"
#include "cache_set_lru.h"
#include "graph_cache_context_sniper.h"

#include <string>

class CacheSetGRASP : public CacheSet
{
   public:
      CacheSetGRASP(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize,
            CacheSetInfoLRU* set_info, UInt8 num_attempts, bool is_tlb_set);
      ~CacheSetGRASP();

      void prepareInsertion(IntPtr addr);
      UInt32 getReplacementIndex(CacheCntlr *cntlr) override;
      void updateReplacementIndex(UInt32 accessed_index) override;
      UInt8 getRecencyBits(UInt32 way) const override { return m_rrip_bits[way]; }

   private:
      void tryLoadContext();
      UInt8 insertionRRPV(IntPtr addr) const;
      void applyPendingInsertion(UInt32 way);

      const String m_cfgname;
      const core_id_t m_core_id;
      const UInt8 m_rrip_numbits;
      const UInt8 m_rrip_max;
      const UInt8 m_rrip_insert;
      const UInt8 m_num_attempts;
      UInt8* m_rrip_bits;
      IntPtr* m_line_addrs;
      bool* m_property_lines;
      UInt8 m_replacement_pointer;
      CacheSetInfoLRU* m_set_info;
      bool m_srrip_tlb_enabled;
      bool m_context_load_attempted;
      bool m_has_pending_insert;
      IntPtr m_pending_insert_addr;
      UInt8 m_pending_insert_rrpv;
      UInt64 m_llc_size_bytes;
      std::string m_sideband_path;
};

#endif  // CACHE_SET_GRASP_H