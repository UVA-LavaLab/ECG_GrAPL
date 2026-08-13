#ifndef GRAPHBREW_SNIPER_POPT_FAST_SELECT_H
#define GRAPHBREW_SNIPER_POPT_FAST_SELECT_H

#include <cstdint>

namespace graphbrew {
namespace sniper {

inline uint32_t selectAndAgePoptVictim(
      uint8_t* rrpv, const uint8_t* distances,
      uint32_t associativity, uint8_t max_rrpv)
{
   uint8_t max_distance = 0;
   for (uint32_t way = 0; way < associativity; ++way) {
      if (distances[way] > max_distance)
         max_distance = distances[way];
   }

   uint8_t candidate_max_rrpv = 0;
   for (uint32_t way = 0; way < associativity; ++way) {
      if (distances[way] == max_distance &&
          rrpv[way] > candidate_max_rrpv) {
         candidate_max_rrpv = rrpv[way];
      }
   }

   const uint8_t age_delta = max_rrpv - candidate_max_rrpv;
   for (uint32_t way = 0; way < associativity; ++way) {
      if (distances[way] == max_distance)
         rrpv[way] = static_cast<uint8_t>(rrpv[way] + age_delta);
   }

   for (uint32_t way = 0; way < associativity; ++way) {
      if (distances[way] == max_distance && rrpv[way] >= max_rrpv)
         return way;
   }
   return associativity;
}

}  // namespace sniper
}  // namespace graphbrew

#endif  // GRAPHBREW_SNIPER_POPT_FAST_SELECT_H
