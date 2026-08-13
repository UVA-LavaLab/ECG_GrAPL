#ifndef GRAPHBREW_DROPLET_PREFETCHER_H
#define GRAPHBREW_DROPLET_PREFETCHER_H

#include "prefetcher.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

class DropletPrefetcher : public Prefetcher
{
   public:
      DropletPrefetcher(String configName, core_id_t core_id);
      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id,
            Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip) override;

   private:
      struct StrideEntry {
         IntPtr last_addr = 0;
         IntPtr stride = 0;
         UInt8 confidence = 0;
         bool valid = false;
      };

      void tryLoadSideband();
      void updateStrideDetector(IntPtr line_addr, std::vector<IntPtr>& predictions);
      void issueIndirectPrefetches(IntPtr edge_addr, std::vector<IntPtr>& addresses);
      bool shouldPrefetch(IntPtr address);
      bool loadEdgeData(const std::string& path, UInt32 elem_size);
      bool isEdgeAccess(IntPtr address) const;
      bool isPropertyAccess(IntPtr address) const;
      IntPtr lineAddress(IntPtr address) const;
      IntPtr propertyAddress(UInt32 vertex_id) const;

      static UInt64 parseJsonUint(const std::string& json, const std::string& key);
      static std::string parseJsonString(const std::string& json, const std::string& key);
      static std::string findPreferredObject(const std::string& json,
            const std::string& section, const std::string& preferred);

      const core_id_t m_core_id;
      const UInt32 m_prefetch_degree;
      const UInt32 m_indirect_degree;
      const UInt32 m_stride_table_size;
      const UInt32 m_cache_line_size;

      IntPtr m_property_base = 0;
      IntPtr m_property_end = 0;
      UInt32 m_property_elem_size = sizeof(float);
      bool m_property_configured = false;

      IntPtr m_edge_base = 0;
      IntPtr m_edge_end = 0;
      UInt32 m_edge_elem_size = sizeof(UInt32);
      bool m_edge_configured = false;

      bool m_sideband_tried = false;
      bool m_sideband_loaded = false;
      UInt64 m_sideband_probe_count = 0;
      bool m_reported_loaded = false;

      std::vector<UInt32> m_edge_data;
      bool m_edge_data_loaded = false;
      std::vector<StrideEntry> m_stride_table;
      std::unordered_set<IntPtr> m_recent_prefetches;
      static constexpr size_t MAX_RECENT = 256;

      UInt64 m_stat_sideband_loaded = 0;
      UInt64 m_stat_edge_accesses = 0;
      UInt64 m_stat_stride_issued = 0;
      UInt64 m_stat_indirect_issued = 0;
      UInt64 m_stat_duplicate_skips = 0;
};

#endif  // GRAPHBREW_DROPLET_PREFETCHER_H