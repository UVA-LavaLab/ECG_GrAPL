#include <iostream>

#include "sniper_sim/sniper_harness.h"

int main() {
  std::cout << "GraphBrew Sniper hello ROI begin" << std::endl;
  SNIPER_ROI_BEGIN();
  volatile std::uint64_t checksum = 0;
  for (std::uint64_t i = 0; i < 1024; ++i) {
    checksum += i;
  }
  SNIPER_SET_VERTEX(checksum & 0xff);
  SNIPER_ROI_END();
  graphbrew_sniper::write_minimal_context(1, 0);
  std::cout << "GraphBrew Sniper hello ROI end: " << checksum << std::endl;
  return 0;
}
