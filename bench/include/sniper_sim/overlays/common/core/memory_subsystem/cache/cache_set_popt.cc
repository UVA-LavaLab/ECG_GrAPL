#include "cache_set_popt.h"

#include "config.hpp"
#include "log.h"
#include "popt_fast_select.h"
#include "simulator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

const char* envOrDefault(const char* name, const char* fallback)
{
   const char* value = std::getenv(name);
   return value && value[0] ? value : fallback;
}

bool poptProfileEnabled()
{
   static const bool enabled = []() {
      const char* value = std::getenv("SNIPER_POPT_PROFILE");
      return value && value[0] && std::string(value) != "0";
   }();
   return enabled;
}

bool poptFastEnabled()
{
   static const bool enabled = []() {
      const char* value = std::getenv("SNIPER_POPT_FAST");
      return !value || !value[0] || std::string(value) != "0";
   }();
   return enabled;
}

struct PoptHostProfile
{
   std::atomic<uint64_t> replacement_calls{0};
   std::atomic<uint64_t> find_next_ref_calls{0};
   std::atomic<uint64_t> property_checks{0};
   std::atomic<uint64_t> rrip_age_rounds{0};
   std::atomic<uint64_t> elapsed_ns{0};
};

PoptHostProfile& poptHostProfile()
{
   static PoptHostProfile profile;
   return profile;
}

void dumpPoptHostProfile()
{
   const auto& profile = poptHostProfile();
   std::fprintf(
      stderr,
      "[POPT-HOST-PROFILE replacement_calls=%llu find_next_ref_calls=%llu "
      "property_checks=%llu rrip_age_rounds=%llu elapsed_ns=%llu]\n",
      static_cast<unsigned long long>(
         profile.replacement_calls.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
         profile.find_next_ref_calls.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
         profile.property_checks.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
         profile.rrip_age_rounds.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
         profile.elapsed_ns.load(std::memory_order_relaxed)));
}

void ensurePoptProfileRegistered()
{
   static const bool registered = []() {
      (void)poptHostProfile();
      std::atexit(dumpPoptHostProfile);
      return true;
   }();
   (void)registered;
}

class PoptProfileScope
{
   public:
      PoptProfileScope()
         : m_enabled(poptProfileEnabled())
      {
         if (m_enabled) {
            ensurePoptProfileRegistered();
            m_start = std::chrono::steady_clock::now();
         }
      }

      ~PoptProfileScope()
      {
         if (!m_enabled) return;
         const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - m_start).count();
         auto& profile = poptHostProfile();
         profile.replacement_calls.fetch_add(1, std::memory_order_relaxed);
         profile.elapsed_ns.fetch_add(
            static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
      }

   private:
      bool m_enabled;
      std::chrono::steady_clock::time_point m_start;
};

uint32_t profiledFindNextRef(
      graphbrew::sniper::GraphCacheContext& context,
      uint64_t addr, uint32_t requester_core)
{
   if (poptProfileEnabled()) {
      poptHostProfile().find_next_ref_calls.fetch_add(
         1, std::memory_order_relaxed);
   }
   return context.findNextRef(addr, requester_core);
}

uint32_t profiledFindNextRefAtVertex(
      graphbrew::sniper::GraphCacheContext& context,
      uint64_t addr, uint32_t current_vertex)
{
   if (poptProfileEnabled()) {
      poptHostProfile().find_next_ref_calls.fetch_add(
         1, std::memory_order_relaxed);
   }
   return context.findNextRefAtVertex(addr, current_vertex);
}

void profilePropertyCheck()
{
   if (poptProfileEnabled()) {
      poptHostProfile().property_checks.fetch_add(
         1, std::memory_order_relaxed);
   }
}

void profileRripAgeRound()
{
   if (poptProfileEnabled()) {
      poptHostProfile().rrip_age_rounds.fetch_add(
         1, std::memory_order_relaxed);
   }
}

}  // namespace

CacheSetPOPT::CacheSetPOPT(
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
   , m_sideband_path(envOrDefault("SNIPER_GRAPHBREW_CTX", "/tmp/sniper_graphbrew_ctx.json"))
   , m_popt_matrix_path(envOrDefault("SNIPER_POPT_MATRIX", "/tmp/sniper_popt_matrix.bin"))
{
   m_rrip_bits = new UInt8[m_associativity];
   m_way_distances = new UInt8[m_associativity];
   m_line_addrs = new IntPtr[m_associativity];
   m_property_lines = new bool[m_associativity];
   for (UInt32 way = 0; way < m_associativity; way++) {
      m_rrip_bits[way] = m_rrip_insert;
      m_way_distances[way] = 0;
      m_line_addrs[way] = 0;
      m_property_lines[way] = false;
   }
}

CacheSetPOPT::~CacheSetPOPT()
{
   delete [] m_rrip_bits;
   delete [] m_way_distances;
   delete [] m_line_addrs;
   delete [] m_property_lines;
}

void
CacheSetPOPT::tryLoadContext()
{
   auto& context = graphbrew::sniper::globalContext();
   if (context.loaded && context.rereference.enabled) return;
   if (m_context_load_attempted) return;
   m_context_load_attempted = true;
   context.setCacheLineSize(m_blocksize);
   if (!context.loaded) {
      context.loadFromSideband(m_sideband_path);
   }
   if (!context.rereference.enabled && context.loadRereferenceMatrix(m_popt_matrix_path) &&
       context.num_regions > 0) {
      context.rereference.base_address = context.regions[0].base_address;
   }
}

void
CacheSetPOPT::prepareInsertion(IntPtr addr)
{
   tryLoadContext();
   m_pending_insert_addr = addr & ~(IntPtr(m_blocksize) - 1);
   m_has_pending_insert = true;
   graphbrew::sniper::globalContext().updateVertexFromAddr(m_pending_insert_addr, m_core_id);
}

void
CacheSetPOPT::applyPendingInsertion(UInt32 way)
{
   m_rrip_bits[way] = m_rrip_insert;
   if (m_has_pending_insert) {
      m_line_addrs[way] = m_pending_insert_addr;
      m_property_lines[way] = graphbrew::sniper::globalContext().isPropertyData(
            static_cast<uint64_t>(m_pending_insert_addr));
      m_has_pending_insert = false;
      return;
   }
   m_line_addrs[way] = 0;
   m_property_lines[way] = false;
}

UInt32
CacheSetPOPT::findSRRIPVictim(CacheCntlr *cntlr)
{
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
            LOG_ASSERT_ERROR(isValidReplacement(index), "POPT selected an invalid replacement candidate");
            return index;
         }
         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
      }

      for (UInt32 way = 0; way < m_associativity; way++) {
         if (m_rrip_bits[way] < m_rrip_max) m_rrip_bits[way]++;
      }
   }

   LOG_PRINT_ERROR("Error finding POPT replacement index");
}

UInt32
CacheSetPOPT::getReplacementIndex(CacheCntlr *cntlr)
{
   PoptProfileScope profile_scope;

   for (UInt32 way = 0; way < m_associativity; way++) {
      if (!m_cache_block_info_array[way]->isValid()) {
         applyPendingInsertion(way);
         if (m_cache_block_info_array[way]->isPageTableBlock() && m_srrip_tlb_enabled) {
            m_rrip_bits[way] = 0;
         }
         return way;
      }
   }

   tryLoadContext();
   auto& context = graphbrew::sniper::globalContext();
   if (!context.loaded || !context.rereference.enabled) {
      return findSRRIPVictim(cntlr);
   }
   uint32_t requester_core = graphbrew::sniper::currentNucaRequesterCore();
   if (requester_core >= graphbrew::sniper::MAX_TRACKED_CORES)
      requester_core = static_cast<uint32_t>(m_core_id);

   UInt32 property_count = 0;
   for (UInt32 way = 0; way < m_associativity; way++) {
      profilePropertyCheck();
      m_property_lines[way] = context.isPropertyData(static_cast<uint64_t>(m_line_addrs[way]));
      if (m_property_lines[way]) property_count++;
   }

   if (property_count != m_associativity) {
      for (UInt32 way = 0; way < m_associativity; way++) {
         if (!m_property_lines[way]) {
            applyPendingInsertion(way);
            LOG_ASSERT_ERROR(isValidReplacement(way), "POPT selected an invalid replacement candidate");
            return way;
         }
      }
   }

   // P-OPT eviction: evict the property line whose next reference (from the
   // rereference matrix, indexed by the current-vertex epoch clock) is farthest
   // in the future. This is byte-faithful to cache_sim findVictimPOPT
   // (Phase 1 non-property evict -> Phase 2 max next-ref distance -> Phase 3 RRIP
   // tiebreak) and gem5's ecg_rp: same makeOffsetMatrix builder, same
   // epoch_size/sub_epoch_size, same findNextRef, same cline_id mapping.
   //
   // NOTE (validated 2026-07): standalone POPT only produces a signal when this
   // NON-INCLUSIVE L3 is actually exercised (property working set > L1+L2). On a
   // small/sparse graph the L2 absorbs the working set, so this L3 sees only a
   // tiny cold stream (e.g. kron_s16_k4 PR -> 89 accesses, 100% miss for
   // LRU=POPT=GRASP) and POPT looks "inert". That is a non-inclusive-L3 geometry
   // effect, NOT a matrix/lookup bug: cache_sim's INCLUSIVE L3 IS exercised on the
   // same cell (POPT 0.447 < LRU 0.639). roi_matrix flags the inert Sniper cell
   // (l3_exercised=False, "[warn] L3 inert"). With this L3 exercised (e.g.
   // cit-Patents PR, ~16M L3 accesses) standalone Sniper POPT beats LRU by 3-15pp,
   // matching cache_sim direction.
   if (poptFastEnabled()) {
      const uint32_t current_vertex =
         context.currentVertexForPopt(requester_core);
      for (UInt32 way = 0; way < m_associativity; way++) {
         m_way_distances[way] = static_cast<UInt8>(std::min(
            profiledFindNextRefAtVertex(
               context, static_cast<uint64_t>(m_line_addrs[way]),
               current_vertex),
            uint32_t(127)));
      }

      const UInt32 victim = graphbrew::sniper::selectAndAgePoptVictim(
         m_rrip_bits, m_way_distances, m_associativity, m_rrip_max);
      LOG_ASSERT_ERROR(
         victim < m_associativity,
         "POPT fast path failed to select a replacement candidate");
      applyPendingInsertion(victim);
      LOG_ASSERT_ERROR(
         isValidReplacement(victim),
         "POPT selected an invalid replacement candidate");
      return victim;
   }

   UInt32 max_distance = 0;
   for (UInt32 way = 0; way < m_associativity; way++) {
      UInt32 distance = profiledFindNextRef(
            context,
            static_cast<uint64_t>(m_line_addrs[way]), requester_core);
      max_distance = std::max(max_distance, std::min(distance, uint32_t(127)));
   }

   while (true) {
      for (UInt32 way = 0; way < m_associativity; way++) {
         UInt32 distance = profiledFindNextRef(
               context,
               static_cast<uint64_t>(m_line_addrs[way]), requester_core);
         if (std::min(distance, uint32_t(127)) == max_distance && m_rrip_bits[way] >= m_rrip_max) {
            applyPendingInsertion(way);
            LOG_ASSERT_ERROR(isValidReplacement(way), "POPT selected an invalid replacement candidate");
            return way;
         }
      }
      profileRripAgeRound();
      for (UInt32 way = 0; way < m_associativity; way++) {
         UInt32 distance = profiledFindNextRef(
               context,
               static_cast<uint64_t>(m_line_addrs[way]), requester_core);
         if (std::min(distance, uint32_t(127)) == max_distance && m_rrip_bits[way] < m_rrip_max) {
            m_rrip_bits[way]++;
         }
      }
   }
}

void
CacheSetPOPT::updateReplacementIndex(UInt32 accessed_index)
{
   m_set_info->increment(accessed_index);

   if (m_cache_block_info_array[accessed_index]->isPageTableBlock() && m_srrip_tlb_enabled) {
      m_rrip_bits[accessed_index] = 0;
      return;
   }

   m_rrip_bits[accessed_index] = 0;
   tryLoadContext();
   graphbrew::sniper::globalContext().updateVertexFromAddr(
         static_cast<uint64_t>(m_line_addrs[accessed_index]), m_core_id);
}