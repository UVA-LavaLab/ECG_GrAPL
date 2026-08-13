#ifndef GRAPHBREW_ECG_PFX_PREFETCHER_H
#define GRAPHBREW_ECG_PFX_PREFETCHER_H

#if __has_include("../../../config/config.hpp")
#include "../../../config/config.hpp"
#else
#include "config.hpp"
#endif
#include "prefetcher.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

class EcgPfxPrefetcher : public Prefetcher
{
   public:
      EcgPfxPrefetcher(String configName, core_id_t core_id);
      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id,
            Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip) override;

   private:
      void tryLoadSideband();
      bool shouldPrefetch(IntPtr address);
      bool isPropertyAddress(IntPtr address) const;
      IntPtr lineAddress(IntPtr address) const;
      IntPtr propertyAddress(UInt32 vertex_id) const;

      static UInt64 parseJsonUint(const std::string& json, const std::string& key);
      static std::string findPreferredObject(const std::string& json,
            const std::string& section, const std::string& preferred);

      const core_id_t m_core_id;
      const UInt32 m_cache_line_size;
      const UInt32 m_recent_filter_size;

      IntPtr m_property_base = 0;
      IntPtr m_property_end = 0;
      UInt32 m_property_elem_size = sizeof(float);
      bool m_property_configured = false;

      bool m_sideband_tried = false;
      bool m_sideband_loaded = false;
      UInt64 m_sideband_probe_count = 0;
      bool m_reported_loaded = false;
      bool m_reported_first_hint = false;

      std::unordered_set<IntPtr> m_recent_prefetches;
      std::deque<IntPtr> m_recent_order;

      UInt64 m_stat_sideband_loaded = 0;
      UInt64 m_stat_target_hints_seen = 0;
      UInt64 m_stat_issued = 0;
      UInt64 m_stat_duplicate_skips = 0;
      UInt64 m_stat_no_sideband = 0;
      UInt64 m_stat_invalid_target = 0;
};

#endif  // GRAPHBREW_ECG_PFX_PREFETCHER_H