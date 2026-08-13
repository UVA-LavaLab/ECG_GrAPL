#include "cache_set_grasp.h"

#include "config.hpp"
#include "log.h"
#include "simulator.h"

#include <cstdlib>

namespace {

const char* envOrDefault(const char* name, const char* fallback)
{
   const char* value = std::getenv(name);
   return value && value[0] ? value : fallback;
}

}  // namespace

CacheSetGRASP::CacheSetGRASP(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize,
      CacheSetInfoLRU* set_info, UInt8 num_attempts, bool is_tlb_set)
   : CacheSet(cache_type, associativity, blocksize, is_tlb_set)
   , m_cfgname(cfgname)
   , m_core_id(core_id)
   , m_rrip_numbits(Sim()->getCfg()->getIntArray(cfgname + "/srrip/bits", core_id))
   , m_rrip_max((1 << m_rrip_numbits) - 1)
   , m_rrip_insert(m_rrip_max - 1)
   , m_num_attempts(num_attempts)
   , m_replacement_pointer(0)
   , m_set_info(set_info)
   , m_srrip_tlb_enabled(Sim()->getCfg()->getBoolArray(cfgname + "/srrip/tlb_enabled", core_id))
   , m_context_load_attempted(false)
   , m_has_pending_insert(false)
   , m_pending_insert_addr(0)
   , m_pending_insert_rrpv(m_rrip_max)
   , m_llc_size_bytes(0)
   , m_sideband_path(envOrDefault("SNIPER_GRAPHBREW_CTX", "/tmp/sniper_graphbrew_ctx.json"))
{
   m_rrip_bits = new UInt8[m_associativity];
   m_line_addrs = new IntPtr[m_associativity];
   m_property_lines = new bool[m_associativity];
   for (UInt32 way = 0; way < m_associativity; way++) {
      m_rrip_bits[way] = m_rrip_insert;
      m_line_addrs[way] = 0;
      m_property_lines[way] = false;
   }
   if (Sim()->getCfg()->hasKey(cfgname + "/cache_size", core_id)) {
      m_llc_size_bytes = UInt64(Sim()->getCfg()->getIntArray(cfgname + "/cache_size", core_id)) * k_KILO;
   }
}

CacheSetGRASP::~CacheSetGRASP()
{
   delete [] m_rrip_bits;
   delete [] m_line_addrs;
   delete [] m_property_lines;
}

void
CacheSetGRASP::tryLoadContext()
{
   auto& context = graphbrew::sniper::globalContext();
   if (context.loaded || m_context_load_attempted) return;
   m_context_load_attempted = true;
   context.setCacheLineSize(m_blocksize);
   context.loadFromSideband(m_sideband_path);
}

void
CacheSetGRASP::prepareInsertion(IntPtr addr)
{
   tryLoadContext();
   m_pending_insert_addr = addr & ~(IntPtr(m_blocksize) - 1);
   m_pending_insert_rrpv = insertionRRPV(m_pending_insert_addr);
   m_has_pending_insert = true;
   graphbrew::sniper::globalContext().updateVertexFromAddr(m_pending_insert_addr, m_core_id);
}

UInt8
CacheSetGRASP::insertionRRPV(IntPtr addr) const
{
   const auto& context = graphbrew::sniper::globalContext();
   if (context.loaded && context.isPropertyData(static_cast<uint64_t>(addr))) {
      uint64_t llc_size = m_llc_size_bytes ? m_llc_size_bytes : UInt64(m_associativity) * m_blocksize;
      uint32_t tier = context.classifyGRASP(static_cast<uint64_t>(addr), llc_size);
      if (tier == 1) return 1;
      if (tier == 2) return m_rrip_max > 0 ? m_rrip_max - 1 : 0;
      return m_rrip_max;
   }
   // Official GRASP inserts every address outside the high/moderate regions
   // at M_RRIP (max=7), not SRRIP's max-1 insertion.
   return m_rrip_max;
}

void
CacheSetGRASP::applyPendingInsertion(UInt32 way)
{
   if (m_has_pending_insert) {
      m_rrip_bits[way] = m_pending_insert_rrpv;
      m_line_addrs[way] = m_pending_insert_addr;
      m_property_lines[way] = graphbrew::sniper::globalContext().isPropertyData(
            static_cast<uint64_t>(m_pending_insert_addr));
      m_has_pending_insert = false;
      return;
   }
   m_rrip_bits[way] = m_rrip_max;
   m_line_addrs[way] = 0;
   m_property_lines[way] = false;
}

UInt32
CacheSetGRASP::getReplacementIndex(CacheCntlr *cntlr)
{
   for (UInt32 way = 0; way < m_associativity; way++) {
      if (!m_cache_block_info_array[way]->isValid()) {
         applyPendingInsertion(way);
         if (m_cache_block_info_array[way]->isPageTableBlock() && m_srrip_tlb_enabled) {
            m_rrip_bits[way] = 0;
         }
         return way;
      }
   }

   UInt8 attempt = 0;
   for (UInt32 age_round = 0; age_round <= m_rrip_max; ++age_round) {
      for (UInt32 probe = 0; probe < m_associativity; probe++) {
         if (m_rrip_bits[m_replacement_pointer] >= m_rrip_max) {
            UInt8 index = m_replacement_pointer;
            bool qbs_reject = false;
            bool attempt_goforit = false;
            if (attempt < m_num_attempts - 1) {
               LOG_ASSERT_ERROR(cntlr != NULL, "CacheCntlr == NULL, QBS can only be used when cntlr is passed in");
               qbs_reject = cntlr->isInLowerLevelCache(m_cache_block_info_array[index]);
               attempt_goforit = true;
            }

            if (qbs_reject) {
               m_rrip_bits[index] = 0;
               cntlr->incrementQBSLookupCost();
               ++attempt;
               continue;
            }

            if (m_cache_block_info_array[index]->isPageTableBlock() &&
                m_srrip_tlb_enabled && attempt_goforit) {
               m_rrip_bits[index] = 0;
               cntlr->incrementQBSLookupCost();
               ++attempt;
               continue;
            }

            m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
            applyPendingInsertion(index);
            m_set_info->incrementAttempt(attempt);
            LOG_ASSERT_ERROR(isValidReplacement(index), "GRASP selected an invalid replacement candidate");
            return index;
         }
         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
      }

      for (UInt32 way = 0; way < m_associativity; way++) {
         if (m_rrip_bits[way] < m_rrip_max) m_rrip_bits[way]++;
      }
   }

   LOG_PRINT_ERROR("Error finding GRASP replacement index");
}

void
CacheSetGRASP::updateReplacementIndex(UInt32 accessed_index)
{
   m_set_info->increment(accessed_index);

   if (m_cache_block_info_array[accessed_index]->isPageTableBlock() && m_srrip_tlb_enabled) {
      m_rrip_bits[accessed_index] = 0;
      return;
   }

   tryLoadContext();
   if (m_property_lines[accessed_index] && graphbrew::sniper::globalContext().loaded) {
      uint64_t llc_size = m_llc_size_bytes ? m_llc_size_bytes : UInt64(m_associativity) * m_blocksize;
      uint32_t tier = graphbrew::sniper::globalContext().classifyGRASP(
            static_cast<uint64_t>(m_line_addrs[accessed_index]), llc_size);
      if (tier == 1) {
         m_rrip_bits[accessed_index] = 0;
         graphbrew::sniper::globalContext().updateVertexFromAddr(
               static_cast<uint64_t>(m_line_addrs[accessed_index]), m_core_id);
         return;
      }
   }

   if (m_rrip_bits[accessed_index] > 0) m_rrip_bits[accessed_index]--;
}