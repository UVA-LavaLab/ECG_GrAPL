from pathlib import Path

from scripts.experiments.ecg import roi_matrix


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_sssp_hoists_source_distance_before_k2_delivery():
    cache_sim = read("bench/src_sim/sssp.cc")
    gem5 = read("bench/src_gem5/sssp.cc")
    sniper = read("bench/src_sniper/sg_kernel.cc")

    assert "RelaxEdges_Sim(g, u, delta, source_dist" in cache_sim
    assert "WeightT new_dist = source_dist + wn.w;" in cache_sim
    assert (
        "const WeightT source_dist = dist[u];\n"
        "    GEM5_SET_VERTEX_EPOCH(" in gem5)
    assert "WeightT new_dist = source_dist + wn.w;" in gem5
    assert "const WeightT source_dist = dist[node];\n        SNIPER_SET_VERTEX(node);" in sniper
    assert "const WeightT candidate = source_dist + edge.w;" in sniper
    sidecar = sniper.split(
        "inline uint32_t consume_fused_k2_sidecar", 1)[1].split(
            "\n}", 1)[0]
    assert 'asm volatile("" : : "r"(sidecar));' in sidecar
    assert '"memory"' not in sidecar
    assert "packCompactWeightedEpochPairRecord" in sniper
    assert "[ECG_FUSED_K2_WEIGHTED64]" in sniper


def test_bc_masks_depth_and_path_counts():
    cache_sim = read("bench/src_sim/bc.cc")
    gem5 = read("bench/src_gem5/bc.cc")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    sniper = read("bench/src_sniper/sg_kernel.cc")
    sniper_context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.cc")
    gem5_policy = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_rp.cc")

    assert "SIM_CACHE_READ_MASKED(cache, depths.data(), v" in cache_sim
    path_count_read = cache_sim.index("cache, path_counts.data(), v,")
    successor_test = cache_sim.index(
        "if (depths[v] == current_depth + 1)")
    assert path_count_read > successor_test
    assert "gem5_ecg_load_k2_u64(" in gem5
    assert "path_counts.data(), record" in gem5
    assert "gem5_ecg_mload_k2_u64(" in gem5
    assert "&path_counts[v], record" in gem5
    assert "0x04: ecg_load_k2_u64" in decoder
    assert "0x08: ecg_mload_k2_u64" in decoder
    k2_u64 = decoder.split("0x04: ecg_load_k2_u64", 1)[1].split(
        "// 0x05 compact weighted K2", 1)[0]
    assert "traceExpectedEcgExtractHint2(packed, 8)" in k2_u64
    assert "dest_id, tier, epoch1, epoch2, 8" in k2_u64
    assert "setDecodedEcgExtractHint2Silent" not in k2_u64
    assert "[ECG-K2-ACCEPT sim=gem5" in gem5_policy
    assert "traceAcceptedK2(" in gem5_policy
    assert '"path_counts"' in sniper_context
    assert "k2_line8_offsets" in sniper_context
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    assert roi_matrix.ecg_epoch_region("bc") == "depth,path_counts"
    assert roi_matrix.gem5_ecg_epoch_region_indices("bc") == "1,2"
    assert roi_matrix.cache_sim_ecg_epoch_region_indices("bc") == "0,1"
    assert (
        roi_matrix.property_regions("bc") ==
        "scores,depth,path_counts,deltas")
    assert '"property_regions": property_regions(args.benchmark)' in runner
    assert '"ecg_epoch_regions": ecg_epoch_region(args.benchmark)' in runner
    assert '"[ECG_K2_MLOAD_CW24]"' in runner
    assert '"[ECG_K2_ILOAD_CW24]"' in runner
    assert '"ecg_record_replaces_edge": 1' in runner
    verifier = read("scripts/experiments/ecg/verify/ecg.py")
    assert "expected_widths == received_widths" in verifier
    assert "const int64_t source_paths = path_counts[u];\n                SNIPER_SET_VERTEX(u);" in sniper
    assert "SNIPER_CLEAR_VERTEX();" in sniper
