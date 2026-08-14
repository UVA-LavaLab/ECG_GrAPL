#ifndef CACHE_SET_ECG_H
#define CACHE_SET_ECG_H

#include "cache_set.h"
#include "cache_set_lru.h"
#include "graph_cache_context_sniper.h"

#include <string>

class CacheSetECG : public CacheSet
{
   public:
      CacheSetECG(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize,
            CacheSetInfoLRU* set_info, UInt8 num_attempts, bool is_tlb_set);
      ~CacheSetECG();

      void prepareInsertion(IntPtr addr, UInt32 set_index);
      UInt32 getReplacementIndex(CacheCntlr *cntlr) override;
      void updateReplacementIndex(UInt32 accessed_index) override;
      UInt8 getRecencyBits(UInt32 way) const override { return m_rrip_bits[way]; }

   private:
      void tryLoadContext();
      void applyPendingInsertion(UInt32 way);
      UInt32 findSRRIPVictim(CacheCntlr *cntlr);
      UInt32 findPOPTVictim(CacheCntlr *cntlr);
      UInt32 findDBGPrimaryVictim(CacheCntlr *cntlr);
      UInt32 findECGEmbeddedVictim(CacheCntlr *cntlr);
      UInt32 findECGGraspPoptVictim(CacheCntlr *cntlr);
      UInt8 graspInsertionRRPV(IntPtr addr) const;
      UInt8 dbgTier(IntPtr addr) const;
      UInt8 poptHint(IntPtr addr) const;
      // SNIPER_ECG_EXTRACT: a property cache line holds blocksize/elem_size
      // vertices; the kernel records the epoch under the DEMANDED vertex, so scan
      // the line's vertices for a delivered epoch (linemin => all agree).
      bool lookupLineEcgReusePlan(IntPtr line_addr,
            UInt8& tier, UInt16& first, UInt16& second,
            UInt8& count, UInt16& current_epoch,
            UInt16& context_id) const;

      const String m_cfgname;
      const core_id_t m_core_id;
      const UInt8 m_rrip_numbits;
      const UInt8 m_rrip_max;
      const UInt8 m_rrip_insert;
      const UInt8 m_num_attempts;
      const graphbrew::sniper::ECGMode m_mode;
      UInt8* m_rrip_bits;
      UInt8* m_dbg_tiers;
      UInt8* m_popt_hints;
      IntPtr* m_line_addrs;
      bool* m_property_lines;
      UInt16* m_ecg_epoch;        // SNIPER_ECG_EXTRACT delivered next-ref epoch
      UInt16* m_ecg_epoch2;       // two-epoch ReusePlan second next-ref epoch
      UInt8* m_ecg_epoch_count;   // 0=unstamped, 1=single, 2=pair
      bool* m_ecg_epoch_valid;    // epoch delivered for this line (0 is valid)
      UInt16* m_ecg_context_id;   // graph-generation context for the stamp
      UInt64* m_last_touch;        // true recency, smaller == older (cache_sim/gem5 parity)
      UInt64 m_access_tick;
      UInt8 m_replacement_pointer;
      CacheSetInfoLRU* m_set_info;
      bool m_srrip_tlb_enabled;
      bool m_context_load_attempted;
      bool m_has_pending_insert;
      IntPtr m_pending_insert_addr;
      bool m_pending_exact_reuse_plan_valid;
      UInt8 m_pending_exact_reuse_plan_tier;
      UInt16 m_pending_exact_reuse_plan_first;
      UInt16 m_pending_exact_reuse_plan_second;
      UInt16 m_pending_request_current_epoch;
      UInt16 m_pending_request_context_id;
      UInt32 m_set_index;
      UInt64 m_llc_size_bytes;
      std::string m_sideband_path;
      std::string m_popt_matrix_path;
};

#endif  // CACHE_SET_ECG_H