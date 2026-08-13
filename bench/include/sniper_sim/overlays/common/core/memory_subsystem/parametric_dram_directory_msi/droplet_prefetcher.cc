#include "droplet_prefetcher.h"

#include "config.hpp"
#include "simulator.h"
#include "stats.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {

const char* envOrDefault(const char* name, const char* fallback)
{
   const char* value = std::getenv(name);
   return value && value[0] ? value : fallback;
}

UInt32 cfgUIntOrDefault(const String& key, core_id_t core_id, UInt32 fallback)
{
   if (Sim()->getCfg()->hasKey(key, core_id)) {
      return Sim()->getCfg()->getIntArray(key, core_id);
   }
   if (Sim()->getCfg()->hasKey(key)) {
      return Sim()->getCfg()->getInt(key);
   }
   return fallback;
}

}  // namespace

DropletPrefetcher::DropletPrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_prefetch_degree(cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/prefetch_degree", core_id, 1))
   , m_indirect_degree(cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/indirect_degree", core_id, 16))
   , m_stride_table_size(cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/stride_table_size", core_id, 64))
   , m_cache_line_size(cfgUIntOrDefault("perf_model/" + configName + "/cache_block_size", core_id, 64))
   , m_stride_table(m_stride_table_size)
{
   registerStatsMetric("droplet-prefetcher", core_id, "sideband-loaded", &m_stat_sideband_loaded);
   registerStatsMetric("droplet-prefetcher", core_id, "edge-accesses", &m_stat_edge_accesses);
   registerStatsMetric("droplet-prefetcher", core_id, "stride-issued", &m_stat_stride_issued);
   registerStatsMetric("droplet-prefetcher", core_id, "indirect-issued", &m_stat_indirect_issued);
   registerStatsMetric("droplet-prefetcher", core_id, "duplicate-skips", &m_stat_duplicate_skips);
}

std::vector<IntPtr>
DropletPrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id,
      Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   (void)core_id;
   (void)mem_op_type;
   (void)cache_hit;
   (void)prefetch_hit;
   (void)eip;

   if (!m_sideband_loaded) {
      m_sideband_probe_count++;
      if (!m_sideband_tried || (m_sideband_probe_count & 0x3ff) == 0) {
         tryLoadSideband();
      }
   }

   std::vector<IntPtr> addresses;
   if (!m_sideband_loaded || !isEdgeAccess(current_address)) return addresses;
   m_stat_edge_accesses++;

   IntPtr line_addr = lineAddress(current_address);
   std::vector<IntPtr> stride_predictions;
   updateStrideDetector(line_addr, stride_predictions);

   for (IntPtr pred : stride_predictions) {
      if (isEdgeAccess(pred) && shouldPrefetch(pred)) {
         addresses.push_back(pred);
         m_stat_stride_issued++;
         issueIndirectPrefetches(pred, addresses);
      }
   }

   issueIndirectPrefetches(current_address, addresses);
   return addresses;
}

void
DropletPrefetcher::tryLoadSideband()
{
   m_sideband_tried = true;
   const std::string path = envOrDefault("SNIPER_GRAPHBREW_CTX", "/tmp/sniper_graphbrew_ctx.json");
   std::ifstream file(path);
   if (!file.is_open()) return;

   std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

   std::string prop = findPreferredObject(content, "\"property_regions\"", "contrib");
   if (prop.empty()) prop = findPreferredObject(content, "\"property_regions\"", "scores");
   if (!prop.empty()) {
      UInt64 base = parseJsonUint(prop, "\"base\"");
      UInt64 size = parseJsonUint(prop, "\"size\"");
      UInt32 elem = static_cast<UInt32>(parseJsonUint(prop, "\"elem_size\""));
      if (base && size && elem) {
         m_property_base = static_cast<IntPtr>(base);
         m_property_end = static_cast<IntPtr>(base + size);
         m_property_elem_size = elem;
         m_property_configured = true;
      }
   }

   std::string edge = findPreferredObject(content, "\"edge_regions\"", "\"preferred\": true");
   if (!edge.empty()) {
      UInt64 base = parseJsonUint(edge, "\"base\"");
      UInt64 size = parseJsonUint(edge, "\"size\"");
      UInt32 elem = static_cast<UInt32>(parseJsonUint(edge, "\"elem_size\""));
      if (base && size) {
         m_edge_base = static_cast<IntPtr>(base);
         m_edge_end = static_cast<IntPtr>(base + size);
         m_edge_elem_size = elem ? elem : sizeof(UInt32);
         m_edge_configured = true;
         std::string data_path = parseJsonString(edge, "\"data_path\"");
         if (!data_path.empty()) loadEdgeData(data_path, m_edge_elem_size);
      }
   }

   m_sideband_loaded = m_property_configured && m_edge_configured;
   m_stat_sideband_loaded = m_sideband_loaded ? 1 : 0;
   if (m_sideband_loaded && !m_reported_loaded) {
      std::cerr << "SNIPER_DROPLET: loaded sideband property=[0x" << std::hex
                << m_property_base << ",0x" << m_property_end << ") edge=[0x"
                << m_edge_base << ",0x" << m_edge_end << ") edge_data="
                << std::dec << m_edge_data.size() << std::endl;
      m_reported_loaded = true;
   }
}

UInt64
DropletPrefetcher::parseJsonUint(const std::string& json, const std::string& key)
{
   size_t pos = json.find(key);
   if (pos == std::string::npos) return 0;
   pos = json.find(':', pos);
   if (pos == std::string::npos) return 0;
   pos++;
   while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
   return std::strtoull(json.c_str() + pos, nullptr, 10);
}

std::string
DropletPrefetcher::parseJsonString(const std::string& json, const std::string& key)
{
   size_t pos = json.find(key);
   if (pos == std::string::npos) return "";
   pos = json.find(':', pos);
   if (pos == std::string::npos) return "";
   pos = json.find('"', pos + 1);
   if (pos == std::string::npos) return "";
   size_t end = json.find('"', pos + 1);
   if (end == std::string::npos) return "";
   return json.substr(pos + 1, end - pos - 1);
}

std::string
DropletPrefetcher::findPreferredObject(const std::string& json,
      const std::string& section, const std::string& preferred)
{
   size_t pos = json.find(section);
   if (pos == std::string::npos) return "";
   size_t arr_start = json.find('[', pos);
   size_t arr_end = json.find(']', arr_start);
   if (arr_start == std::string::npos || arr_end == std::string::npos) return "";

   std::string fallback;
   size_t obj_pos = arr_start;
   while ((obj_pos = json.find('{', obj_pos)) != std::string::npos && obj_pos < arr_end) {
      size_t obj_end = json.find('}', obj_pos);
      if (obj_end == std::string::npos || obj_end > arr_end) break;
      std::string obj = json.substr(obj_pos, obj_end - obj_pos + 1);
      if (fallback.empty()) fallback = obj;
      if (obj.find(preferred) != std::string::npos) return obj;
      obj_pos = obj_end + 1;
   }
   return fallback;
}

bool
DropletPrefetcher::loadEdgeData(const std::string& path, UInt32 elem_size)
{
   if (elem_size != sizeof(UInt32) && elem_size != 2 * sizeof(UInt32)) return false;
   std::ifstream file(path, std::ios::binary);
   if (!file.is_open()) return false;
   file.seekg(0, std::ios::end);
   std::streamoff bytes = file.tellg();
   file.seekg(0, std::ios::beg);
   if (bytes <= 0 || (bytes % static_cast<std::streamoff>(elem_size)) != 0) return false;

   std::vector<char> raw(static_cast<size_t>(bytes));
   file.read(raw.data(), bytes);
   if (!file.good() && !file.eof()) return false;

   size_t entries = raw.size() / elem_size;
   m_edge_data.assign(entries, 0);
   for (size_t i = 0; i < entries; ++i) {
      std::memcpy(&m_edge_data[i], raw.data() + i * elem_size, sizeof(UInt32));
   }
   m_edge_data_loaded = true;
   return true;
}

bool
DropletPrefetcher::isEdgeAccess(IntPtr address) const
{
   IntPtr line = lineAddress(address);
   return m_edge_configured && line < m_edge_end && line + IntPtr(m_cache_line_size) > m_edge_base;
}

bool
DropletPrefetcher::isPropertyAccess(IntPtr address) const
{
   IntPtr line = lineAddress(address);
   return m_property_configured && line < m_property_end && line + IntPtr(m_cache_line_size) > m_property_base;
}

IntPtr
DropletPrefetcher::lineAddress(IntPtr address) const
{
   return address & ~(IntPtr(m_cache_line_size) - 1);
}

IntPtr
DropletPrefetcher::propertyAddress(UInt32 vertex_id) const
{
   return m_property_base + IntPtr(UInt64(vertex_id) * m_property_elem_size);
}

void
DropletPrefetcher::updateStrideDetector(IntPtr line_addr, std::vector<IntPtr>& predictions)
{
   StrideEntry* best = nullptr;
   for (auto& entry : m_stride_table) {
      if (!entry.valid) {
         if (!best) best = &entry;
         continue;
      }
      IntPtr delta = line_addr - entry.last_addr;
      if (delta == entry.stride && delta != 0) {
         entry.confidence = std::min<UInt8>(3, entry.confidence + 1);
         entry.last_addr = line_addr;
         best = &entry;
         break;
      }
      if (delta != 0 && std::llabs(static_cast<long long>(delta)) <= 512) {
         entry.stride = delta;
         entry.confidence = 1;
         entry.last_addr = line_addr;
         best = &entry;
         break;
      }
   }
   if (!best && !m_stride_table.empty()) best = &m_stride_table.front();
   if (!best) return;
   if (!best->valid) {
      best->valid = true;
      best->last_addr = line_addr;
      best->stride = m_cache_line_size;
      best->confidence = 0;
      return;
   }
   if (best->confidence >= 2 && best->stride != 0) {
      for (UInt32 degree = 1; degree <= m_prefetch_degree; degree++) {
         predictions.push_back(line_addr + best->stride * IntPtr(degree));
      }
   }
}

void
DropletPrefetcher::issueIndirectPrefetches(IntPtr edge_addr, std::vector<IntPtr>& addresses)
{
   if (!m_property_configured) return;
   IntPtr base_line = edge_addr & ~(IntPtr(m_cache_line_size) - 1);
   IntPtr first_edge = std::max(base_line, m_edge_base);
   IntPtr alignment = (first_edge - m_edge_base) % IntPtr(m_edge_elem_size);
   if (alignment != 0) first_edge += IntPtr(m_edge_elem_size) - alignment;
   for (UInt32 i = 0; i < m_indirect_degree; i++) {
      IntPtr target_edge = first_edge + IntPtr(i * m_edge_elem_size);
      if (!isEdgeAccess(target_edge)) break;
      UInt64 edge_offset = UInt64(target_edge - m_edge_base) / m_edge_elem_size;
      UInt32 vertex = 0;
      if (m_edge_data_loaded) {
         if (edge_offset >= m_edge_data.size()) break;
         vertex = m_edge_data[edge_offset];
      } else {
         vertex = static_cast<UInt32>(edge_offset);
      }
      IntPtr prop = propertyAddress(vertex);
      IntPtr prop_line = lineAddress(prop);
      if (isPropertyAccess(prop) && shouldPrefetch(prop_line)) {
         addresses.push_back(prop_line);
         m_stat_indirect_issued++;
      }
   }
}

bool
DropletPrefetcher::shouldPrefetch(IntPtr address)
{
   IntPtr line = lineAddress(address);
   if (m_recent_prefetches.count(line)) {
      m_stat_duplicate_skips++;
      return false;
   }
   if (m_recent_prefetches.size() >= MAX_RECENT) m_recent_prefetches.clear();
   m_recent_prefetches.insert(line);
   return true;
}