from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_sniper_popt_host_profile_is_opt_in_and_behavior_neutral():
    source = (
        ROOT
        / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/cache_set_popt.cc"
    ).read_text()

    assert 'std::getenv("SNIPER_POPT_PROFILE")' in source
    assert 'std::getenv("SNIPER_POPT_FAST")' in source
    assert "return !value || !value[0]" in source
    assert "[POPT-HOST-PROFILE replacement_calls=" in source
    assert "PoptProfileScope profile_scope;" in source
    assert source.count("profiledFindNextRef(") == 4
    assert source.count("profiledFindNextRefAtVertex(") == 2
    assert "profilePropertyCheck();" in source
    assert "profileRripAgeRound();" in source
    assert "selectAndAgePoptVictim(" in source

    runner = (
        ROOT / "scripts/experiments/ecg/roi_matrix.py"
    ).read_text()
    assert '"0" if os.environ.get("SNIPER_POPT_FAST") == "0" else "1"' in runner
    assert 'row["sniper_popt_fast"]' in runner

    context_header = (
        ROOT
        / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.h"
    ).read_text()
    context_source = (
        ROOT
        / "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.cc"
    ).read_text()
    assert "findNextRefAtVertex(" in context_header
    assert "return rereference.findNextRef(" in context_source


def test_popt_fast_rrip_selection_matches_legacy(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available")

    source = tmp_path / "popt_fast_select_test.cc"
    binary = tmp_path / "popt_fast_select_test"
    source.write_text(
        r'''
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/popt_fast_select.h"

static uint32_t legacy(
      std::vector<uint8_t>& rrpv,
      const std::vector<uint8_t>& distances,
      uint8_t max_rrpv) {
   const uint8_t max_distance =
      *std::max_element(distances.begin(), distances.end());
   while (true) {
      for (uint32_t way = 0; way < rrpv.size(); ++way) {
         if (distances[way] == max_distance && rrpv[way] >= max_rrpv)
            return way;
      }
      for (uint32_t way = 0; way < rrpv.size(); ++way) {
         if (distances[way] == max_distance && rrpv[way] < max_rrpv)
            ++rrpv[way];
      }
   }
}

int main() {
   std::mt19937 rng(0x504f5054);
   for (uint8_t max_rrpv : {uint8_t{1}, uint8_t{3}, uint8_t{7}, uint8_t{15}}) {
      for (uint32_t associativity = 1; associativity <= 32; ++associativity) {
         for (uint32_t trial = 0; trial < 5000; ++trial) {
            std::vector<uint8_t> distances(associativity);
            std::vector<uint8_t> reference_rrpv(associativity);
            for (uint32_t way = 0; way < associativity; ++way) {
               distances[way] = static_cast<uint8_t>(rng() % 128);
               reference_rrpv[way] =
                  static_cast<uint8_t>(rng() % (max_rrpv + 1));
            }
            std::vector<uint8_t> fast_rrpv = reference_rrpv;
            const uint32_t expected =
               legacy(reference_rrpv, distances, max_rrpv);
            const uint32_t actual =
               graphbrew::sniper::selectAndAgePoptVictim(
                  fast_rrpv.data(), distances.data(),
                  associativity, max_rrpv);
            if (actual != expected || fast_rrpv != reference_rrpv)
               return 1;
         }
      }
   }
   return 0;
}
'''
    )
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-O2",
            "-I.",
            str(source),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run([str(binary)], cwd=ROOT, check=True)
