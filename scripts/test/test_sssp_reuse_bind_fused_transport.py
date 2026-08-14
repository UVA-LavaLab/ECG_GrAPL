from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_weighted_sssp_fused_path_moves_validation_before_roi():
    builder = (ROOT / "bench/include/ecg_reuse_plan_builder.h").read_text()
    sniper = (ROOT / "bench/src_sniper/sg_kernel.cc").read_text()
    gem5 = (ROOT / "bench/src_gem5/sssp.cc").read_text()

    assert "validateWeightedReusePlanRecords" in builder
    assert "consume_fused_reuse_plan_sidecar" in sniper
    assert "packWeightedReusePlanSidecar" in builder
    assert "pair_sidecars.size() * sizeof(uint32_t)" in sniper
    assert "pair_flat.size() * sizeof(uint64_t)" in sniper
    assert "static_cast<uint32_t>(edge.v)" in sniper
    assert "deliver_reuse_plan_record(record, fused_reuse_plan_model);" in sniper
    assert "} else if (\n                pair_ok ||" in sniper
    assert "auto relax_edges = [&](" in sniper
    # The compact weighted path must relax straight out of the packed 8B
    # pair_compact records (the transport being charged), not re-walk the
    # ordinary CSR via relax_edges. Only the general/sidecar and no-delivery
    # fallbacks still use relax_edges.
    assert "auto relax_compact_edges = [&](" in sniper
    assert "relax_compact_edges(node, source_dist);" in sniper
    assert "const uint64_t record = pair_compact[pos];" in sniper
    assert "ecg_reuse_plan::extractCompactWeightedDest(record)" in sniper
    assert "ecg_reuse_plan::extractCompactWeightedWeight(record)" in sniper
    assert sniper.count("relax_edges(") == 4
    assert 'std::getenv("ECG_REUSE_PLAN_VALIDATE")' in sniper
    assert 'std::getenv("ECG_REUSE_PLAN_VALIDATE")' in gem5
    assert "gem5_ecg_flow_weighted_load_instruction" in gem5
    assert "gem5_ecg_plan_weighted_load_instruction" in gem5
    assert "combineWeightedReusePlanRecord" in builder
    assert "gem5_ecg_bind_iload_u32(dist.data(), record)" in gem5
    assert sniper.count("if (no_delivery_pair_loop)") == 5
    assert "[REUSE_PLAN_TRANSPORT_MATCHED] SSSP compact 8B" in sniper
    assert "[REUSE_PLAN_TRANSPORT_MATCHED] SSSP general 12B" in sniper
    sssp_source = sniper.split("int run_sssp(", 1)[1]
    declare = sssp_source.index(
        "::ecg_metadata::declareContainerBytes(metadata, transport_bytes)")
    enforce = sssp_source.index(
        "::ecg_metadata::enforceExpectedBytesPerEdge(")
    validate = sssp_source.index("validateWeightedReusePlanRecords")
    assert declare < enforce < validate

    decoder = (
        ROOT
        / "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa"
    ).read_text()
    assert decoder.count("uint64_t dest_dependency = Rs2;") == 1
    stream_block = decoder.split("0x3: ecg_flow_load", 1)[1].split(
        "0x4: ecg_plan_load", 1)[0]
    weighted_stream_block = decoder.split(
        "0x5: ecg_flow_weighted_load", 1)[1].split(
            "0x6: ecg_plan_weighted_load", 1)[0]
    assert "setDecodedEcgExtractHint2" not in stream_block
    assert "setDecodedEcgExtractHint2" not in weighted_stream_block

    sniper_roi = sniper.split("int run_sssp(", 1)[1].split(
        "SNIPER_ROI_BEGIN();", 1)[1].split("SNIPER_ROI_END();", 1)[0]
    assert "SSSP ReusePlan pair index out of range" not in sniper_roi
    assert "SSSP ReusePlan destination mismatch" not in sniper_roi

    gem5_relax = gem5.split("inline void RelaxEdges_Gem5", 1)[1].split(
        "int main(", 1)[0]
    assert "SSSP ReusePlan pair index out of range" not in gem5_relax
    assert "SSSP ReusePlan destination mismatch" not in gem5_relax

    runner = (ROOT / "scripts/experiments/ecg/roi_matrix.py").read_text()
    assert '"[ECG_FUSED_REUSE_PLAN_WEIGHTED32]"' in runner
    assert '"ecg_record_bytes": 12' in runner
    assert '"edge_stream_bytes_per_edge"' in runner
    assert '"ecg_record_replaces_edge"' in runner
