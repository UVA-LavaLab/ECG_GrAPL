import subprocess
from pathlib import Path

from scripts.experiments.ecg.policy_specs import parse_policy_spec


ROOT = Path(__file__).resolve().parents[2]


def test_hawkeye_proxy_policy_label_is_explicit():
    spec = parse_policy_spec("HAWKEYE:PROXY")
    assert spec.policy == "HAWKEYE"
    assert spec.label == "HAWKEYE_PROXY"
    faithful = parse_policy_spec("HAWKEYE")
    assert faithful.policy == "HAWKEYE"
    assert faithful.label == "HAWKEYE"


def test_hawkeye_policy_clean_room_core(tmp_path: Path):
    source = tmp_path / "hawkeye_policy_test.cc"
    binary = tmp_path / "hawkeye_policy_test"
    source.write_text(
        r'''
#include <cassert>
#include <cstdint>

#include "hawkeye_policy.h"

int main()
{
    using namespace hawkeye_policy;

    Predictor predictor;
    const uint64_t pc = 0x1234;
    assert(predictor.friendly(pc));
    for (int i = 0; i < 4; ++i) predictor.decrease(pc);
    assert(!predictor.friendly(pc));
    for (int i = 0; i < 20; ++i) predictor.increase(pc);
    assert(predictor.value(pc) == kPredictorMax);

    Optgen optgen(2);
    optgen.advance(0);
    assert(optgen.addInterval(0, 3, 3));
    assert(optgen.occupancyAt(0) == 1);
    assert(optgen.addInterval(0, 3, 3));
    assert(!optgen.addInterval(0, 3, 3));
    assert(!optgen.addInterval(0, 0, kOptgenQuanta));

    State state(8192, 16);
    std::size_t sampled = 0;
    for (std::size_t set = 0; set < 8192; ++set)
        sampled += state.sampledSet(set) ? 1 : 0;
    assert(sampled == 64);
    assert(state.access(0, 0x100, pc));
    state.access(0, 0x100, pc);

    State lru_state(8192, 16);
    uint64_t blocks[9] = {};
    for (uint64_t i = 0; i < 9; ++i)
        blocks[i] = i * kSamplerSets * 64;
    for (uint64_t i = 0; i < 8; ++i)
        lru_state.access(0, blocks[i], 0x2000 + i);
    lru_state.access(0, blocks[0], 0x2000);
    lru_state.access(0, blocks[8], 0x2008);
    assert(lru_state.samplerContains(blocks[0]));
    assert(!lru_state.samplerContains(blocks[1]));
    for (int i = 2; i < 9; ++i)
        assert(lru_state.samplerContains(blocks[i]));

    uint8_t rrpv[4] = {0, 4, 7, 6};
    assert(selectVictim(rrpv, 4) == 2);
    uint8_t friendly[4] = {0, 1, 2, 7};
    ageFriendlyFill(friendly, 4);
    assert(friendly[0] == 1 && friendly[1] == 2);
    assert(insertionRrpv(true) == 0);
    assert(insertionRrpv(false) == 7);
    return 0;
}
'''
    )
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "bench/include"),
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True, cwd=ROOT)


def test_cache_sim_hawkeye_adapter_and_site_proxy(tmp_path: Path):
    source = tmp_path / "cache_sim_hawkeye_test.cc"
    binary = tmp_path / "cache_sim_hawkeye_test"
    source.write_text(
        r'''
#include <cassert>
#include <cstdint>

#include "cache_sim/cache_sim.h"
#include "cache_sim/graph_sim.h"

int main()
{
    using namespace cache_sim;
    assert(StringToPolicy("HAWKEYE") == EvictionPolicy::HAWKEYE);
    assert(PolicyToString(EvictionPolicy::HAWKEYE) == "HAWKEYE");

    CacheHierarchy cache(
        256, 2, 512, 2, 1024, 4, 64,
        EvictionPolicy::LRU, EvictionPolicy::LRU,
        EvictionPolicy::HAWKEYE);
    uint64_t values[64] = {};
    SIM_CACHE_READ(cache, values, 0);
    SIM_CACHE_READ(cache, values, 0);
    SIM_CACHE_READ(cache, values, 16);
    assert(cache.getTotalAccesses() == 3);

    bool rejected_private = false;
    try {
        CacheLevel invalid(
            "L1", 256, 64, 2, EvictionPolicy::HAWKEYE);
    } catch (const std::invalid_argument&) {
        rejected_private = true;
    }
    assert(rejected_private);
    return 0;
}
'''
    )
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O0",
            "-Wall",
            "-Wextra",
            "-fopenmp",
            "-I",
            str(ROOT / "bench/include"),
            "-I",
            str(ROOT / "bench/include/external/gapbs"),
            "-I",
            str(ROOT / "bench/include/graphbrew"),
            "-I",
            str(ROOT / "bench/include/external"),
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True, cwd=ROOT)


def test_gem5_hawkeye_real_pc_surface():
    header = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/hawkeye_rp.hh").read_text()
    source = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/hawkeye_rp.cc").read_text()
    policies = (
        ROOT / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/GraphReplacementPolicies.py").read_text()
    config = (
        ROOT / "bench/include/gem5_sim/configs/graphbrew/"
        "graph_cache_config.py").read_text()
    graph_se = (
        ROOT / "bench/include/gem5_sim/configs/graphbrew/"
        "graph_se.py").read_text()
    setup = (ROOT / "scripts/setup_gem5.py").read_text()

    assert "class GraphHawkeyeRP" in header
    assert "pkt->req->hasPC()" in source
    assert "pkt->req->getPC()" in source
    assert "pkt->cmd.isPrefetch()" in source
    assert "pkt->isWriteback()" in source
    assert "class GraphHawkeyeRP" in policies
    assert 'upper == "HAWKEYE"' in config
    assert "Hawkeye is an LLC-only replacement policy" in config
    assert '"HAWKEYE"' in graph_se
    assert '"../../hawkeye_policy.h"' in setup
    assert '"hawkeye_rp.cc"' in setup

    import json
    manifest = json.loads(
        (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
    stage = next(
        stage for stage in manifest["stages"]
        if "ecg_gem5_hawkeye_gate" in stage.get("profiles", []))
    assert stage["suite"] == "gem5"
    assert "HAWKEYE" in stage["policies"]
    assert "HAWKEYE:PROXY" not in stage["policies"]
