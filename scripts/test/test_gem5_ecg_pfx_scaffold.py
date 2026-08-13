#!/usr/bin/env python3
"""Regression tests for the gem5 ECG_PFX scaffold wiring."""

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SETUP_GEM5_PATH = PROJECT_ROOT / "scripts" / "setup_gem5.py"
spec = importlib.util.spec_from_file_location("setup_gem5", SETUP_GEM5_PATH)
setup_gem5 = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["setup_gem5"] = setup_gem5
spec.loader.exec_module(setup_gem5)

ROI_MATRIX_PATH = PROJECT_ROOT / "scripts/experiments/ecg/roi_matrix.py"
roi_spec = importlib.util.spec_from_file_location("ecg_roi_matrix", ROI_MATRIX_PATH)
roi_matrix = importlib.util.module_from_spec(roi_spec)
assert roi_spec.loader is not None
sys.modules["ecg_roi_matrix"] = roi_matrix
roi_spec.loader.exec_module(roi_matrix)


def read(path: str) -> str:
    return (PROJECT_ROOT / path).read_text()


def test_setup_gem5_installs_ecg_pfx_overlays():
    overlay_values = set(setup_gem5.OVERLAY_FILE_MAP.values())

    assert "mem/cache/prefetch/ecg_pfx.hh" in overlay_values
    assert "mem/cache/prefetch/ecg_pfx.cc" in overlay_values
    assert "arch/riscv/isa/formats/ecg.isa" in overlay_values


def test_prefetch_sconscript_registers_ecg_pfx():
    text = read("bench/include/gem5_sim/overlays/mem/cache/prefetch/SConscript.patch")

    assert "GraphEcgPfxPrefetcher" in text
    assert "Source('ecg_pfx.cc')" in text


def test_graph_se_accepts_ecg_pfx_prefetcher():
    text = read("bench/include/gem5_sim/configs/graphbrew/graph_se.py")

    assert 'choices=["none", "DROPLET", "ECG_PFX", "STRIDE"]' in text
    assert "GEM5_ENABLE_ECG_PFX_HINTS" in text
    assert "GEM5_ECG_PFX_LOOKAHEAD" in text
    assert "GEM5_ECG_PFX_HINT_FILTER" in text
    assert "GEM5_ECG_PFX_FILTER_ELEM_SIZE" in text
    assert "GEM5_ECG_PFX_FILTER_LINE_SIZE" in text
    assert "GEM5_ENABLE_ECG_EXTRACT" in text
    assert "make_ecg_pfx_prefetcher" in text


def test_graph_se_caps_instructions_relative_to_roi():
    text = read("bench/include/gem5_sim/configs/graphbrew/graph_se.py")
    assert "system.exit_on_work_items = True" in text
    assert "system.cpu.scheduleInstStop(" in text
    assert '"ROI instruction cap reached"' in text
    assert "simulation exited before ROI work-begin" in text


def test_gem5_harness_defines_ecg_pfx_m5ops_macro():
    text = read("bench/include/gem5_sim/gem5_harness.h")

    assert "GEM5_WORK_ECG_PFX_TARGET" in text
    assert "GEM5_ECG_PFX_TARGET" in text
    assert "gem5_should_emit_ecg_pfx_hint" in text
    assert "gem5_ecg_extract_target_instruction" in text
    assert "gem5_ecg_pfx_target_instruction" in text
    assert ".insn r 0x0b" in text


def test_x86_instruction_path_emits_gem5_pseudo_op_bytes():
    harness = read("bench/include/gem5_sim/gem5_harness.h")

    assert 'asm volatile (".byte 0x0F, 0x04' in harness
    assert '"D"(work_id)' in harness
    assert '"S"(argument)' in harness
    assert "M5OP_WORK_BEGIN" in harness


def test_riscv_ecg_extract_overlay_uses_custom0_opcode():
    text = read("bench/include/gem5_sim/overlays/arch/riscv/isa/decoder_ecg_extract.isa")

    # custom-0 opcode space (full opcode 0x0b -> OPCODE5 0x02), FUNCT3 decode.
    assert "0x02: decode FUNCT3" in text
    assert "ecg_extract" in text
    # Wide mode-6 delivery: next-reference epoch plus a 24-bit prefetch
    # target (dbg/popt reclaimed; see packMaskEpochWide). Hints are delivered via
    # the per-vertex metadata table and the legacy single-slot mailbox.
    assert "epoch" in text
    assert "pfx_target" in text
    assert "storeEcgMetadataByVertex" in text
    assert "setDecodedEcgExtractHint" in text
    assert "setPrefetchTargetHint" in text


def test_gem5_graph_context_stores_decoded_ecg_extract_hint():
    text = read("bench/include/gem5_sim/overlays/mem/cache/replacement_policies/graph_cache_context_gem5.hh")

    assert "decodedEcgRealVertexStorage" in text
    assert "decodedEcgMetadataStorage" in text
    assert "setDecodedEcgExtractHint" in text
    assert "GRAPHBREW_ECG_EXTRACT_MASK_WORK_ID" in text


def test_gem5_schedule2_delivery_is_pair_aware():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa"
    )
    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh"
    )
    policy = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_rp.cc"
    )
    setup = read("scripts/setup_gem5.py")
    graph_se = read(
        "bench/include/gem5_sim/configs/graphbrew/graph_se.py")

    assert "GEM5_ECG_EXTRACT2" in harness
    assert "0x01: ecg_extract2" in decoder
    assert "(packed >> 32) & 0x3" in decoder
    assert "(packed >> 34) & 0x7FFF" in decoder
    assert "(packed >> 49) & 0x7FFF" in decoder
    assert "setDecodedEcgExtractHint2" in decoder
    assert "0x03: ecg_load_k2" in decoder
    assert "0x06: ecg_mload_k2_u32" in decoder
    assert "0x07: ecg_mload_k2_s32" in decoder
    assert "0x08: ecg_mload_k2_u64" in decoder
    assert "0x09: ecg_mload_k2_compact_u32" in decoder
    assert "0x0A: ecg_mload_k2_f32" in decoder
    assert "xc->setEcgLoadHint2(" in decoder
    assert "lookupDecodedEcgHint2" in context
    assert "isEcgEpochData" in context
    assert "ecg_epoch2" in policy
    assert "ecg_epoch_count" in policy
    assert "bool valid;" in read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_rp.hh"
    )
    assert "if (!getData(candidate)->valid) return candidate;" in policy
    assert "ctx.isEcgEpochData(getData(c)->line_addr)" in policy
    assert "setDueling && victimRequestValid" in policy
    assert "dd->ecg_dbg_tier < 1 || dd->ecg_dbg_tier > 3" in policy
    assert "ctx.classifyGRASP(addr, llcSize, ghf)" in policy
    assert "isa_dbg >= 1 && isa_dbg <= 3" in policy
    assert "epochPairDistance(" in policy
    assert policy.count("readEcgEpochPair(") >= 2
    assert policy.count(
        "!got && !requestBoundEcgProducerEnabled()") >= 2
    assert "GRAPHBREW_ECG_EXTRACT2_WORK_ID" in setup

    request_ext = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_epoch_request_ext.hh")
    assert "attachEcgEpochPair" in request_ext
    assert "readEcgEpochPair" in request_ext
    assert "epoch2_" in request_ext
    assert "epoch_count_" in request_ext
    assert "current_epoch_" in request_ext
    assert "context_id_" in request_ext
    assert "sequence_" in request_ext
    assert "class EcgMshrState" in request_ext

    exec_patch = read(
        "bench/include/gem5_sim/overlays/cpu/exec_context_ecg_producer.patch")
    dyn_patch = read(
        "bench/include/gem5_sim/overlays/cpu/o3/dyn_inst_ecg_producer.patch")
    lsq_patch = read(
        "bench/include/gem5_sim/overlays/cpu/o3/lsq_ecg_producer.patch")
    assert "setEcgLoadHint2" in exec_patch
    assert "setEcgLoadHint2" in dyn_patch
    assert "attachEcgEpochPair" in lsq_patch
    assert "ecg_current_epoch" in lsq_patch
    assert "ecg_context_id" in lsq_patch
    assert "ecg_sequence" in lsq_patch
    assert 'schedule_k == "2"' in graph_se
    assert '"GRASP_HOT_FRACTION"' in graph_se


def test_gem5_k2_uses_architectural_epoch_context_csrs():
    csr_patch = read(
        "bench/include/gem5_sim/overlays/arch/riscv/ecg_csr.patch")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    graph_se = read(
        "bench/include/gem5_sim/configs/graphbrew/graph_se.py")
    setup = read("scripts/setup_gem5.py")
    policy = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_rp.cc")
    victim_patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "ecg_victim_request.patch")
    mshr_patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/mshr_ecg_merge.patch")

    assert "CSR_ECG_CUR_EPOCH = 0x800" in csr_patch
    assert "CSR_ECG_CONTEXT = 0x801" in csr_patch
    assert "CSR_ECG_RECORD_FORMAT = 0x802" in csr_patch
    assert "MISCREG_ECG_CUR_EPOCH" in decoder
    assert "MISCREG_ECG_CONTEXT" in decoder
    assert "MISCREG_ECG_RECORD_FORMAT" in decoder
    # Deliberate acknowledgement tripwire, not a completeness check. Property
    # metadata-delivery instructions read epoch/context. Placement-only record
    # loads such as ecg_stream_load2_compact deliberately do not; K2-M owns
    # delivery on the subsequent property Request. The format CSR count changes
    # for compact decode instructions.
    assert decoder.count("MISCREG_ECG_CUR_EPOCH") == 18
    assert decoder.count("MISCREG_ECG_CONTEXT") == 18
    assert decoder.count("MISCREG_ECG_RECORD_FORMAT") == 2
    assert 'asm volatile ("csrw 0x800, %0"' in harness
    assert 'asm volatile ("csrw 0x801, %0"' in harness
    assert 'asm volatile ("csrw 0x802, %0"' in harness
    assert "GEM5_SET_VERTEX_EPOCH" in harness
    assert "gem5_ecg_allocate_context_id" in harness
    assert "GEM5_ECG_END_CONTEXT" in harness
    assert "ID reuse requires" in harness
    assert "GEM5_WORK_SET_CONTEXT" in harness
    assert 'env["GEM5_ECG_EPOCH_CSR"] = "1"' in runner
    assert '"runtime-monotonic"' in runner
    assert "GEM5_ECG_EPOCH_CSR=" in graph_se
    assert "GEM5_ECG_CONTEXT_ID=" not in graph_se
    assert "setVictimRequest" in victim_patch
    assert "applyEcgMetadata" in mshr_patch
    assert "data->ecg_context_id == victimContextId" in policy
    assert "data->ecg_context_id = pf_context" in policy
    assert "recordPendingPrefetchEpoch(" in setup
    assert "pfxa_context" in setup
    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh")
    assert "previous_context != context_id" in context
    assert "s.drops.fetch_add" in context
    assert "context_id != expected_context" in context
    assert "getCurrentContextHint" in policy
    assert "legacyRequestState" in policy
    assert "getCurrentContextHint" in policy
    assert "GRAPHBREW_SET_CONTEXT_WORK_ID" in setup
    assert "ctx.currentVertexForPopt()" not in policy.split(
        "GraphEcgRP::getVictim", 1)[1]

    for kernel in ("pr", "bfs", "sssp", "bc", "cc"):
        source = read(f"bench/src_gem5/{kernel}.cc")
        assert "GEM5_ECG_BEGIN_CONTEXT();" in source
        assert "GEM5_ECG_END_CONTEXT();" in source
        assert "GEM5_SET_VERTEX_EPOCH(" in source


def test_schedule2_runner_selects_adaptive_variants_and_rejects_o3(monkeypatch):
    monkeypatch.delenv("ECG_VARIANT", raising=False)
    monkeypatch.setenv("ECG_EDGE_MASK_SCHED", "2")
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="pr")) == "epoch_first"
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="bfs")) == "degree_first"
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="sssp")) == "degree_first"
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="bc")) == "rrip_first"
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="cc")) == "rrip_first"

    monkeypatch.setenv("ECG_VARIANT", "rrip_first")
    assert roi_matrix.effective_ecg_variant(
        SimpleNamespace(benchmark="pr")) == "rrip_first"

    runner = read("scripts/experiments/ecg/roi_matrix.py")
    graph_se = read("bench/include/gem5_sim/configs/graphbrew/graph_se.py")
    assert "Schedule-2 O3 requires the RISC-V masked property-load" in runner
    assert 'args.gem5_cpu_type == "O3"' in runner
    assert "request_bound_k2" in graph_se
    assert "Schedule-2 O3 requires the masked property-load path" in graph_se
    assert "prefetcher none or STRIDE" in runner
    assert "GEM5_ECG_EPOCH_REGION_INDICES" in graph_se
    assert "GEM5_ECG_EPOCH_REGION_INDEX" in graph_se
    assert "GEM5_ECG_ISA_VARIANT" in graph_se
    verifier = read("scripts/experiments/ecg/verify/ecg.py")
    assert "required = set(range(32))" in verifier


def test_gem5_k2_uses_configured_epoch_count_not_packed4_cap():
    for path in (
        "bench/src_gem5/pr.cc",
        "bench/src_gem5/bfs.cc",
        "bench/src_gem5/sssp.cc",
        "bench/src_gem5/bc.cc",
        "bench/src_gem5/cc.cc",
    ):
        text = read(path)
        assert 'gem5_env_int_clamped("ECG_EDGE_MASK_EPOCHS"' in text
        assert "ecg_sched_k != 2" in text
        assert "requested_epoch_count" in text
    pr = read("bench/src_gem5/pr.cc")
    assert "Schedule-2 record ON" in pr
    assert "buildInEdgeEpochPairRecords" in pr
    cache_context = read("bench/include/cache_sim/graph_cache_context.h")
    assert 'std::getenv("ECG_EDGE_MASK_PACK") && sched_k != 2' in cache_context


def test_gem5_k2_mailbox_is_cleared_after_governed_load():
    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh")
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    assert "clearDecodedEcgExtractHint()" in context
    assert "if (tier == 0)" in context
    assert "GEM5_ECG_CLEAR_EXTRACT2_HINT" in harness
    for path in (
        "bench/src_gem5/pr.cc",
        "bench/src_gem5/bfs.cc",
        "bench/src_gem5/sssp.cc",
        "bench/src_gem5/bc.cc",
        "bench/src_gem5/cc.cc",
    ):
        text = read(path)
        # Either spelling clears the mailbox. PR calls the function directly
        # because the macro re-tests gem5_ecg_extract_enabled() on every edge,
        # which is measurable overhead in an arm priced in instructions per
        # edge; what matters is that the clear happens, not how it is spelled.
        assert ("GEM5_ECG_CLEAR_EXTRACT2_HINT()" in text
                or "gem5_ecg_clear_extract2_hint()" in text), path
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    assert '"prototype_instruction_delivery"' in runner
    assert 'packed8+k2+ecg.extract2' in runner


def test_gem5_exports_prefetch_and_dram_traffic_metrics():
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    for metric in (
        "l3_prefetch_misses",
        "l3_prefetch_accesses",
        "dram_read_bytes",
        "dram_write_bytes",
        "dram_prefetch_read_bytes",
    ):
        assert f'"{metric}"' in runner
    assert "system.mem_ctrl.dram.bytesRead::total" in runner
    assert "system.l3cache.overallMisses::l2cache.prefetcher" in runner
    assert '"gem5_stats_sections_seen": len(sections)' in runner
    assert "row.update(sections[0])" in runner


def test_gem5_srrip_is_true_three_bit_srrip():
    text = read("bench/include/gem5_sim/configs/graphbrew/graph_cache_config.py")
    assert '"SRRIP": lambda: RRIPRP(num_bits=3)' in text
    assert '"SRRIP": lambda: BRRIPRP(btp=0)' not in text


def test_roi_matrix_auto_selects_riscv_ecg_delivery():
    text = read("scripts/experiments/ecg/roi_matrix.py")
    graph_se = read("bench/include/gem5_sim/configs/graphbrew/graph_se.py")
    assert 'env["GEM5_FORCE_ECG_PLOAD"] = "1"' in text
    assert '"packed4+ecg.extract"' in text
    assert 'os.environ.get("GEM5_FORCE_ECG_LOAD") == "1"' in text
    assert '"ecg.pload-request-bound"' in text
    assert 'env["GEM5_ECG_PRODUCER"] = "1"' in text
    assert '"ECG_EDGE_MASK_PREFETCH"' in text
    assert 'row["gem5_ecg_delivery"] = "ecg.load"' not in text
    assert 'base["gem5_ecg_delivery"] = gem5_ecg_delivery' in text
    assert 'os.environ.get("ECG_FORCE_DELIVERY") == "1"' in graph_se
    assert 'ecg_pfx_enabled = args.prefetcher == "ECG_PFX"' in graph_se
    assert "or ecg_epoch_delivery" not in graph_se


def test_epoch_extract_is_not_gated_by_prefetch_enable():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    good = (
        "#define GEM5_ECG_EXTRACT_MASK(mask_u64) \\\n"
        "    do { \\\n"
        "        if (gem5_ecg_extract_enabled()) {"
    )
    bad = (
        "#define GEM5_ECG_EXTRACT_MASK(mask_u64) \\\n"
        "    do { \\\n"
        "        if (gem5_ecg_pfx_hints_enabled() && "
        "gem5_ecg_extract_enabled()) {"
    )
    assert harness.count(good) == 2
    assert bad not in harness
    assert "GEM5_WORK_ECG_EXTRACT_MASK" in harness


def test_k2_property_load_clears_mailbox_without_extra_instruction():
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    k2_load = decoder.split("0x03: ecg_load_k2", 1)[1].split(
        "}}, ea_code={{", 1)[0]
    assert "Rd = Mem_uw;" in k2_load
    assert "clearDecodedEcgExtractHint();" in k2_load
    assert "traceExpectedEcgExtractHint2(packed);" in decoder

    harness = read("bench/include/gem5_sim/gem5_harness.h")
    helper = harness.split(
        "inline uint32_t gem5_ecg_load_k2(", 1)[1].split(
            "inline uint32_t gem5_ecg_load_k2_compact(", 1)[0]
    assert "gem5_trace_ecg_k2_expect" not in helper
    compact_helper = harness.split(
        "inline uint32_t gem5_ecg_load_k2_compact(", 1)[1].split(
            "inline uint32_t gem5_ecg_load_k2_compact_traced(", 1)[0]
    assert "gem5_trace_ecg_k2_expect" not in compact_helper

    for kernel in ("bfs", "sssp", "bc", "cc"):
        source = read(f"bench/src_gem5/{kernel}.cc")
        for block in source.split("if (ecg_k2_pload_on) {")[1:]:
            canonical = block.split("} else {", 1)[0]
            assert "GEM5_ECG_CLEAR_EXTRACT2_HINT" not in canonical, kernel

    pr = read("bench/src_gem5/pr.cc")
    canonical_pr = pr.split("if (ecg_k2_pload_on) {", 1)[1].split(
        "continue;", 1)[0]
    assert "GEM5_ECG_CLEAR_EXTRACT2_HINT" not in canonical_pr
    assert "gem5_ecg_clear_extract2_hint" not in canonical_pr
    assert ("GEM5_ECG_CLEAR_EXTRACT2_HINT" in pr
            or "gem5_ecg_clear_extract2_hint()" in pr)


def test_k2_mask_only_variant_is_distinct_from_indexed_load():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    assert "GEM5_ECG_ISA_VARIANT" in harness
    assert '"ecg_isa_variant"' in runner
    assert 'env["GEM5_ECG_ISA_VARIANT"] = args.ecg_isa_variant' in runner
    assert "SNIPER_K2_TRANSPORT_MATCHED" in runner
    assert "matched-k2m-sideband-model" in runner
    assert '"prototype_mask_only_load"' in runner
    assert "architectural compact StreamShield record load" in runner
    assert "request-bound property load with per-event tracing disabled" in (
        runner)
    assert '"architectural_compact_k2m_streamshield"' in runner
    assert 'row["gem5_ecg_epoch_channel"]' not in runner
    assert 'base["gem5_ecg_epoch_channel"]' in runner
    assert "transport.schedule_k == 2" in runner
    assert 'std::strcmp(value, "mask") == 0' in harness
    assert '".insn r 0x0b, 0x2, 0x18' in harness
    assert '".insn r 0x0b, 0x2, 0x1c' in harness
    assert '".insn r 0x0b, 0x2, 0x20' in harness
    assert '".insn r 0x0b, 0x2, 0x24' in harness
    assert '".insn r 0x0b, 0x2, 0x28' in harness

    u32 = decoder.split("0x06: ecg_mload_k2_u32", 1)[1].split(
        "// 0x07 K2-M S32.D32", 1)[0]
    s32 = decoder.split("0x07: ecg_mload_k2_s32", 1)[1].split(
        "// 0x08 K2-M U64.D32", 1)[0]
    u64 = decoder.split("0x08: ecg_mload_k2_u64", 1)[1].split(
        "// 0x09 K2-M U32.CW24", 1)[0]
    compact = decoder.split(
        "0x09: ecg_mload_k2_compact_u32", 1)[1].split(
            "// 0x0A K2-M F32.D32", 1)[0]
    f32 = decoder.split("0x0A: ecg_mload_k2_f32", 1)[1].split(
        "// 0x0B K2-C", 1)[0]
    for block in (u32, s32, u64, compact, f32):
        assert "EA = rvZext(Rs1);" in block
        assert "Rs1 +" not in block
        assert "xc->setEcgLoadHint2(" in block
    assert "Rd = Mem_uw;" in u32
    assert "Rd_sd = Mem_sw;" in s32
    assert "Rd = Mem_ud;" in u64
    assert "packed & 0x00FFFFFFULL" in compact
    assert "Fd_bits = fd.v;" in f32
    assert "FloatMemReadOp" in f32
    assert "Rd =" not in f32

    fused_compact = decoder.split(
        "0x0: ecg_load_k2_compact", 1)[1].split(
            "\n                }", 1)[0]
    assert "MISCREG_ECG_RECORD_FORMAT" in fused_compact
    assert "id_bits + 2 + 2 * epoch_bits > 32" in fused_compact
    assert "Rs1 + static_cast<uint64_t>(dest_id) * 4" in fused_compact
    assert "xc->setEcgLoadHint2(" in fused_compact
    assert '".insn r 0x0b, 0x2, 0x2c' in harness

    expected_helpers = {
        "pr": ("gem5_ecg_mload_k2_f32",),
        "bfs": ("gem5_ecg_mload_k2_s32",),
        "sssp": (
            "gem5_ecg_mload_k2_s32",
            "gem5_ecg_mload_k2_compact_u32",
        ),
        "bc": (
            "gem5_ecg_mload_k2_s32",
            "gem5_ecg_mload_k2_u64",
        ),
        "cc": ("gem5_ecg_mload_k2_s32",),
    }
    for kernel, helpers in expected_helpers.items():
        source = read(f"bench/src_gem5/{kernel}.cc")
        assert "gem5_ecg_k2_mask_only_enabled()" in source
        for helper in helpers:
            assert helper in source
        assert "ECG_K2_MLOAD" in source
        assert "ECG_K2_ILOAD" in source


def test_riscv_gem5_build_unswitches_runtime_policy_loops():
    makefile = read("Makefile")
    flags = makefile.split("CXXFLAGS_GEM5_RISCV :=", 1)[1].splitlines()[0]
    assert "-funswitch-loops" in flags


def test_fused_compact_load_is_architectural_and_fail_closed():
    """The fused compact arm must not infer format or silently widen.

    rs1 and rs2 are already occupied by the property base and compact record, so
    the loop-invariant field widths belong in architectural state. A sideband
    lookup would make the ISA depend on simulator-only metadata, while silently
    falling back to the 64-bit fused load would recreate the width+decode
    confound this instruction exists to remove.
    """
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    guest = read("bench/src_gem5/pr.cc")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    graph_se = read(
        "bench/include/gem5_sim/configs/graphbrew/graph_se.py")

    assert "CSR_ECG_RECORD_FORMAT = 0x802" in read(
        "bench/include/gem5_sim/overlays/arch/riscv/ecg_csr.patch")
    assert 'asm volatile ("csrw 0x802, %0"' in harness
    assert '".insn r 0x0b, 0x2, 0x2c' in harness
    assert "MISCREG_ECG_RECORD_FORMAT" in decoder
    assert "ecg_load_k2_compact" in decoder
    compact_decode = decoder.split(
        "0x0B: decode ECG_WIDTH", 1)[1].split(
            "\n                }", 1)[0]
    assert "0x0: ecg_load_k2_compact" in compact_decode
    assert "0x1:" not in compact_decode

    assert "GEM5_ECG_COMPACT_FUSED" in graph_se
    assert "GEM5_ECG_COMPACT_FUSED=1 but" in guest
    assert "std::abort()" in guest
    assert "[ECG_K2_ILOAD_C]" in guest

    # Tracing is a separate helper selected outside the loop; the untraced hot
    # path must not pay a disabled-trace guard on every edge.
    untraced = harness.split(
        "inline uint32_t gem5_ecg_load_k2_compact(", 1)[1].split(
            "inline uint32_t gem5_ecg_load_k2_compact_traced(", 1)[0]
    assert "gem5_trace_ecg_k2_expect" not in untraced


def test_fused_compact_cli_rejects_unsupported_kernels():
    proc = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "bfs",
            "--gem5-compact-fused", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert proc.returncode != 0
    assert "implemented only for --benchmark pr" in (
        proc.stdout + proc.stderr)


def test_fused_compact_row_is_attested_from_runtime_not_requested_env():
    active = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_fused_receipt(
        active, "[ECG_K2_ILOAD_C] PR ACTIVE", requested=True)
    assert active["gem5_compact_fused_active"] == 1
    assert active["gem5_ecg_delivery"] == "ecg.k2.iload.compact"
    assert "error" not in active

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_fused_receipt(
        missing, "[ECG_K2_ILOAD] PR ACTIVE", requested=True)
    assert missing["gem5_compact_fused_active"] == 0
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"

    baseline = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_fused_receipt(
        baseline, "", requested=False)
    assert baseline["gem5_compact_fused_active"] == 0
    assert "error" not in baseline


def test_proposal_compact_k2m_streamshield_is_fail_closed():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    guest = read("bench/src_gem5/pr.cc")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    graph_se = read(
        "bench/include/gem5_sim/configs/graphbrew/graph_se.py")

    stream_block = decoder.split(
        "0x7: ecg_stream_load2_compact", 1)[1].split(
            "\n            }", 1)[0]
    assert "Mem_uw" in stream_block
    assert "MISCREG_ECG_RECORD_FORMAT" in stream_block
    assert "mem_flags=[ECG_STREAM_BYPASS]" in stream_block
    assert "setDecodedEcgExtractHint" not in stream_block
    assert '".insn i 0x0b, 0x7' in harness
    assert "gem5_ecg_stream_load2_compact_instruction" in guest
    assert "gem5_ecg_mload_k2_f32" in guest
    assert "wide_k2m_streamshield_on" in guest
    assert "in_edge_pair32_flat.data()" in guest
    assert "in_edge_pair32_flat.size() * sizeof(uint32_t)" in guest
    assert "GEM5_ECG_COMPACT_K2M_SS=1 but" in guest
    assert "[ECG_K2_MLOAD_C_SS]" in guest
    assert "GEM5_ECG_COMPACT_K2M_SS" in graph_se

    active = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_k2m_streamshield_receipt(
        active,
        "[ECG_K2_MLOAD_C_SS] PR ACTIVE\n"
        "[ECG-STREAM-BYPASS sim=gem5 cache=l3cache addr=0x40 "
        "vaddr=0x40 size=4 source=request-flag allocate=0]",
        requested=True)
    assert active["proposal_path_active"] == 1
    assert active["gem5_stream_bypass_request_flag_events"] == 1
    assert active["gem5_stream_bypass_request_flag_size4_events"] == 1
    assert active["gem5_stream_bypass_request_flag_bad_size_events"] == 0
    assert active["gem5_stream_bypass_all_events"] == 1
    assert active["gem5_stream_bypass_range_events"] == 0
    assert active["gem5_ecg_delivery"] == (
        "ecg.stream.load2.compact+ecg.k2.mload.f32")

    performance = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_k2m_streamshield_receipt(
        performance,
        "[ECG_K2_MLOAD_C_SS] PR compact StreamShield record load "
        "+ computed-address masked property load ACTIVE "
        "(id_bits=16 epoch_bits=5)\n",
        requested=True,
        require_trace_receipts=False,
        performance_requested=True)
    assert performance["proposal_performance_mode_active"] == 1
    assert performance["proposal_compact_id_bits"] == 16
    assert performance["proposal_compact_epoch_bits"] == 5
    assert performance["proposal_compact_tier_bits"] == 2
    assert "error" not in performance

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_k2m_streamshield_receipt(
        missing, "[ECG_K2_MLOAD] PR ACTIVE", requested=True)
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"

    no_bypass = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_k2m_streamshield_receipt(
        no_bypass, "[ECG_K2_MLOAD_C_SS] PR ACTIVE", requested=True)
    assert no_bypass["status"] == "error"
    assert "request-flag StreamShield" in no_bypass["error"]

    wrong_width = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_k2m_streamshield_receipt(
        wrong_width,
        "[ECG_K2_MLOAD_C_SS] PR ACTIVE\n"
        "[ECG-STREAM-BYPASS sim=gem5 cache=l3cache addr=0x40 "
        "vaddr=0x40 size=8 source=request-flag allocate=0]",
        requested=True)
    assert wrong_width["status"] == "error"
    assert "4-byte request-flag record requests" in wrong_width["error"]


def test_proposal_compact_k2m_streamshield_cli_guards():
    wrong_kernel = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "bfs",
            "--ecg-isa-variant", "mask",
            "--gem5-compact-k2m-streamshield", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert wrong_kernel.returncode != 0
    assert "implemented only for --benchmark pr" in (
        wrong_kernel.stdout + wrong_kernel.stderr)

    wrong_isa = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "indexed",
            "--gem5-compact-k2m-streamshield", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert wrong_isa.returncode != 0
    assert "requires --ecg-isa-variant mask" in (
        wrong_isa.stdout + wrong_isa.stderr)

    wrong_policy = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "mask",
            "--gem5-cpu-type", "O3",
            "--policies", "ECG:K1_STREAMSHIELD",
            "--gem5-compact-k2m-streamshield", "--dry-run",
        ],
        cwd=PROJECT_ROOT,
        env={
            **os.environ,
            "GEM5_OPT": str(
                PROJECT_ROOT /
                "bench/include/gem5_sim/gem5/build/RISCV/gem5.opt"),
            "GEM5_KERNEL_SUFFIX": "_riscv_m5ops",
        },
        capture_output=True, text=True, timeout=60)
    assert wrong_policy.returncode != 0
    assert "requires at least one Schedule-2 ECG StreamShield policy" in (
        wrong_policy.stdout + wrong_policy.stderr)

    timing_cpu = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "mask",
            "--gem5-cpu-type", "timing",
            "--gem5-compact-k2m-streamshield", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert timing_cpu.returncode != 0
    assert "requires --gem5-cpu-type O3" in (
        timing_cpu.stdout + timing_cpu.stderr)

    wrong_line = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "mask",
            "--gem5-cpu-type", "O3",
            "--line-size", "128",
            "--gem5-compact-k2m-streamshield", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert wrong_line.returncode != 0
    assert "requires --line-size 64" in (
        wrong_line.stdout + wrong_line.stderr)

    riscv_env = {
        **os.environ,
        "GEM5_OPT": str(
            PROJECT_ROOT /
            "bench/include/gem5_sim/gem5/build/RISCV/gem5.opt"),
        "GEM5_KERNEL_SUFFIX": "_riscv_m5ops",
    }
    performance_args = roi_matrix.parse_args([
        "--suite", "gem5", "--benchmark", "pr",
        "--ecg-isa-variant", "mask",
        "--gem5-cpu-type", "O3",
        "--policies", "LRU", "ECG:K2_STREAMSHIELD",
        "--gem5-compact-k2m-performance", "--dry-run",
    ])
    assert performance_args.gem5_compact_k2m_performance is True

    mixed_modes = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "mask",
            "--gem5-cpu-type", "O3",
            "--policies", "LRU", "ECG:K2_STREAMSHIELD",
            "--gem5-compact-k2m-streamshield",
            "--gem5-compact-k2m-performance", "--dry-run",
        ],
        cwd=PROJECT_ROOT, env=riscv_env,
        capture_output=True, text=True, timeout=60)
    assert mixed_modes.returncode != 0
    assert "choose either" in (
        mixed_modes.stdout + mixed_modes.stderr)


def test_trace_free_k2m_does_not_override_other_timing_caveats():
    args = roi_matrix.parse_args([])
    args.benchmark = "pr"
    args.prefetcher = "ECG_PFX"
    args.ecg_pfx_delivery = "instruction"
    args.ecg_isa_variant = "mask"
    args.gem5_compact_k2m_performance = True
    args.has_lru_baseline = True
    row = roi_matrix.base_row(
        "gem5", args,
        roi_matrix.parse_policy_spec("ECG:K2_STREAMSHIELD"),
        "32kB")
    assert row["timing_valid_for_speedup"] == "0"
    assert row["timing_model"] == "prototype_instruction_delivery"


def test_trace_free_k2m_scrubs_all_gem5_event_traces():
    env = {
        "ECG_K2_DELIVERY_TRACE": "2048",
        "ECG_STREAM_BYPASS_TRACE": "2048",
        "GEM5_ECG_EXT_TRACE": "2048",
        "ECG_EVICT_TRACE": "2048",
        "ECG_EVICT_TRACE_ROI": "1",
        "UNRELATED": "keep",
    }
    roi_matrix.disable_gem5_event_traces(env)
    assert env == {"UNRELATED": "keep"}


def test_proposal_request_bound_receipt_matches_request_extension():
    log = "\n".join([
        "[ECG-K2-REQUEST sim=gem5 seq=0 request_seq=17 "
        "dest=9 tier=2 epoch1=3 epoch2=7 current=1 context=4]",
        "[ECG-K2-ACCEPT sim=gem5 seq=0 request_seq=17 "
        "request_dest=9 fill_dest=9 source=request tier=2 "
        "epoch1=3 epoch2=7 current=1 context=4 "
        "property_elem_bytes=4]",
    ])
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_request_bound_k2_receipt(
        row, log, requested=True)
    assert row["gem5_k2_exact_request_bound"] == 1
    assert row["gem5_k2_request_bad_receipts"] == 0

    same_line = {"timing_valid_for_speedup": "1"}
    same_line_log = log.replace(
        "dest=9 tier=2", "dest=11 tier=2").replace(
        "request_dest=9 fill_dest=9",
        "request_dest=11 fill_dest=1")
    assert roi_matrix.apply_gem5_request_bound_k2_receipt(
        same_line, same_line_log, requested=True, line_bytes=64)
    assert same_line["gem5_k2_coalesced_line_accepts"] == 1
    assert same_line["gem5_k2_exact_vertex_accepts"] == 0

    bad = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_request_bound_k2_receipt(
        bad, same_line_log.replace("fill_dest=1", "fill_dest=32"),
        requested=True)
    assert bad["status"] == "error"
    assert bad["timing_valid_for_speedup"] == "0"

    discriminating_log = "\n".join([
        log,
        "[ECG-K2-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=3 epoch1=5 epoch2=9 current=2 context=4]",
        "[ECG-K2-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=3 "
        "epoch1=5 epoch2=9 current=2 context=4 "
        "property_elem_bytes=4]",
    ])
    discriminating = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_request_bound_k2_receipt(
        discriminating, discriminating_log, requested=True,
        require_discriminating=True)
    assert discriminating["gem5_k2_payload_discriminating"] == 1
    assert discriminating["gem5_k2_request_metadata_values"] == 2
    assert discriminating["gem5_k2_accept_metadata_values"] == 2
    assert discriminating["gem5_k2_request_epoch_states"] == 2
    assert discriminating["gem5_k2_accept_epoch_states"] == 2
    assert discriminating["gem5_k2_request_record_epoch_pairs"] == 2
    assert discriminating["gem5_k2_accept_record_epoch_pairs"] == 2

    tier_only_log = "\n".join([
        log,
        "[ECG-K2-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=3 epoch1=3 epoch2=7 current=1 context=4]",
        "[ECG-K2-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=3 "
        "epoch1=3 epoch2=7 current=1 context=4 "
        "property_elem_bytes=4]",
    ])
    tier_only = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_request_bound_k2_receipt(
        tier_only, tier_only_log, requested=True,
        require_discriminating=True)
    assert tier_only["gem5_k2_payload_discriminating"] == 0
    assert tier_only["gem5_k2_accept_epoch_states"] == 1

    current_only_log = "\n".join([
        log,
        "[ECG-K2-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=2 epoch1=3 epoch2=7 current=2 context=4]",
        "[ECG-K2-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=2 "
        "epoch1=3 epoch2=7 current=2 context=4 "
        "property_elem_bytes=4]",
    ])
    current_only = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_request_bound_k2_receipt(
        current_only, current_only_log, requested=True,
        require_discriminating=True)
    assert current_only["gem5_k2_payload_discriminating"] == 0
    assert current_only["gem5_k2_accept_epoch_states"] == 2
    assert current_only["gem5_k2_accept_record_epoch_pairs"] == 1

    duplicate_accept = {"timing_valid_for_speedup": "1"}
    duplicate_log = discriminating_log + "\n" + discriminating_log.splitlines()[-1]
    assert not roi_matrix.apply_gem5_request_bound_k2_receipt(
        duplicate_accept, duplicate_log, requested=True,
        require_discriminating=True)
    assert duplicate_accept["gem5_k2_duplicate_accepts"] == 1

    replay_log = "\n".join([
        log,
        "[ECG-K2-REQUEST sim=gem5 seq=1 request_seq=17 "
        "dest=9 tier=2 epoch1=3 epoch2=7 current=1 context=4]",
    ])
    replay = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_request_bound_k2_receipt(
        replay, replay_log, requested=False, trace_limit=2)
    assert replay["gem5_k2_request_trace_events"] == 2
    assert replay["gem5_k2_request_receipts"] == 1
    assert replay["gem5_k2_duplicate_request_receipts"] == 1
    assert replay["gem5_k2_request_trace_max_seq"] == 1
    assert replay["gem5_k2_delivery_trace_saturated"] == 1


def test_proposal_run_gate_requires_every_requested_row():
    args = SimpleNamespace(
        gem5_compact_k2m_streamshield=True,
        dry_run=False,
        l3_sizes=["32kB"],
        ecg_isa_variant="mask",
        benchmark="pr",
    )
    policies = [
        roi_matrix.parse_policy_spec("ECG:K2"),
        roi_matrix.parse_policy_spec("ECG:K2_LRU_STREAMSHIELD"),
        roi_matrix.parse_policy_spec("ECG:K2_STREAMSHIELD"),
    ]
    good = {
        "gem5_compact_k2m_streamshield_requested": 1,
        "policy_label": "ECG_K2_STREAMSHIELD",
        "status": "ok",
        "proposal_path_active": 1,
        "gem5_k2_exact_request_bound": 1,
        "gem5_k2_payload_discriminating": 1,
        "gem5_k2_coalesced_line_accepts": 1,
        "gem5_k2_nonzero_epoch_accepts": 8,
        "gem5_stream_bypass_request_flag_size4_events": 2,
        "gem5_stream_bypass_request_flag_bad_size_events": 0,
        "gem5_stream_bypass_request_flag_events": 2,
        "gem5_stream_bypass_all_events": 2,
        "gem5_stream_bypass_range_events": 0,
        "gem5_stream_bypass_trace_saturated": 0,
    }
    roi_matrix.validate_gem5_compact_k2m_streamshield_rows(
        [
            {
                **good, "policy_label": "ECG_K2_LRU_STREAMSHIELD",
                "l3_size": "32kB",
            },
            {**good, "l3_size": "32kB"},
            {"gem5_compact_k2m_streamshield_requested": 0, "status": "ok"},
        ],
        args, policies)

    with pytest.raises(SystemExit, match="proposal compact K2-M"):
        roi_matrix.validate_gem5_compact_k2m_streamshield_rows(
            [
                {
                    **good, "policy_label": "ECG_K2_LRU_STREAMSHIELD",
                    "l3_size": "32kB",
                },
                {
                    **good, "l3_size": "32kB",
                    "gem5_k2_exact_request_bound": 0,
                },
            ],
            args, policies)
    with pytest.raises(SystemExit, match="proposal compact K2-M"):
        roi_matrix.validate_gem5_compact_k2m_streamshield_rows(
            [
                {
                    **good, "policy_label": "ECG_K2_LRU_STREAMSHIELD",
                    "l3_size": "32kB",
                    "gem5_k2_coalesced_line_accepts": 0,
                },
                {**good, "l3_size": "32kB"},
            ],
            args, policies)
    with pytest.raises(SystemExit, match="observed=.*32kB"):
        roi_matrix.validate_gem5_compact_k2m_streamshield_rows(
            [{**good, "l3_size": "32kB"}], args, policies)
    with pytest.raises(SystemExit, match="proposal compact K2-M"):
        roi_matrix.validate_gem5_compact_k2m_streamshield_rows(
            [
                {
                    **good, "policy_label": "ECG_K2_LRU_STREAMSHIELD",
                    "l3_size": "32kB",
                    "gem5_stream_bypass_trace_saturated": 1,
                },
                {**good, "l3_size": "32kB"},
            ],
            args, policies)


def test_real_decoder_probe_covers_compact_streamshield_k2m_request():
    probe = read("bench/src_gem5/test_ecg_load_modes.cc")
    verifier = read("scripts/experiments/ecg/verify/ecg.py")

    assert "gem5_ecg_stream_load2_compact_instruction" in probe
    assert "gem5_ecg_mload_k2_f32" in probe
    assert "K2-C-SS-MLOAD" in probe
    assert "gem5_ecg_write_record_format_csr" in probe
    assert "g_context_retry_lines[2048 * 64]" in probe
    assert "kProposalValueBits = 0x41234567u" in probe
    assert "if (proposal_only || proposal_wrong_format)" in probe
    assert "proposal-wrong-format" in probe
    assert "wrong_record_format ? kProposalIdBits - 2" in probe
    assert '"current=%u context=%u value_bits=%#x [%s]\\n"' in probe

    assert '"--cpu-type", "O3"' in verifier
    assert '"GEM5_ECG_PRODUCER": "1"' in verifier
    assert '"GEM5_ECG_STREAM_REQUEST_BOUND": "1"' in verifier
    assert 'ROOT / "bench" / "include" / "gem5_sim" / "configs"' in verifier
    assert '"PATH": "/usr/bin:/bin"' in verifier
    assert "expected_payload = (37, 3, 17, 29, 11, 7)" in verifier
    assert "compact_request_bound_pass" in verifier
    assert "compact_request_bypass_pass" in verifier
    assert "size=4" in verifier
    assert 'r"K2-C-SS-MLOAD[^\\n]*\\[OK\\]"' in verifier
    assert '"[test_ecg_load_modes] RESULT: PASS" in o3_text' in verifier
    assert "--gem5-isa-only" in verifier
    assert "--isa-receipt-dir" in verifier
    assert "decoder_probe_receipt.json" in verifier
    assert '"overall_pass": overall_pass' in verifier
    assert '"o3_proposal.log"' in verifier
    assert "normal_process_pass" in verifier
    assert 'atomic_runs[-1]["exit_code"] == 0' in verifier
    assert "atomic_proposal_wrong_format_teeth" in verifier
    assert "atomic_proposal_format_teeth.log" in verifier


def test_proposal_o3_manifest_profile_is_exact_and_mechanism_only():
    manifest = json.loads(read(
        "scripts/experiments/ecg/experiment_manifest.json"))
    assert "ecg_proposal_k2m_o3_gate" in manifest["profiles"]
    stage = next(
        item for item in manifest["stages"]
        if item["name"] == "60_gem5_proposal_k2m_o3")
    assert stage["suite"] == "gem5"
    assert stage["graph_set"] == "synthetic_kron12_all"
    assert stage["benchmarks"] == ["pr"]
    assert stage["policies"] == [
        "ECG:K2", "ECG:K2_LRU_STREAMSHIELD",
        "ECG:K2_STREAMSHIELD"]
    assert stage["ecg_isa_variant"] == "mask"
    assert stage["gem5_cpu_type"] == "O3"
    assert stage["gem5_compact_k2m_streamshield"] is True
    assert stage["ecg_epochs"] == 32
    assert "ECG_K2_DELIVERY_TRACE" not in stage["env"]
    assert "ECG_STREAM_BYPASS_TRACE" not in stage["env"]
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    experiment_run = read("scripts/experiments/ecg/flows/experiment_run.py")
    assert 'env["ECG_K2_DELIVERY_TRACE"] = "2048"' in runner
    assert 'env["ECG_STREAM_BYPASS_TRACE"] = "2048"' in runner
    assert 'env["ECG_K2_DELIVERY_TRACE"] = "131072"' in runner
    assert 'env["ECG_STREAM_BYPASS_TRACE"] = "131072"' in runner
    assert "max(k2_trace, 2048)" not in runner
    assert "max(bypass_trace, 2048)" not in runner
    assert '"mechanism_probe_exact_request"' in runner
    assert 'row.setdefault("status", "ok")' in runner
    assert "planning-missing-gem5-guest-sha256" in runner
    assert "planning-missing-gem5-guest-sha256" in experiment_run
    assert "rather than request-count or performance coverage" in stage["notes"]
    assert 'stdout_path.suffix + ".env.json"' in runner


def test_proposal_certification_preserves_layered_errors_and_persists_first():
    rows = [
        {
            "simulator": "gem5", "status": "error",
            "error": "proposal K2-M exact Request binding was not attested",
            "options": "-i 1", "l3_size": "32kB", "l3_ways": 8,
            "prefetcher": "none",
        },
        {
            "simulator": "gem5", "status": "ok",
            "options": "-i 1", "l3_size": "32kB", "l3_ways": 8,
            "prefetcher": "none",
            "pr_iterations": 1, "pr_semantic_edges": 10,
            "pr_score_checksum": "abc",
        },
    ]
    roi_matrix.certify_gem5_pr_results(
        rows, SimpleNamespace(benchmark="pr", suite="gem5"))
    assert "exact Request binding" in rows[0]["error"]
    assert "PageRank semantic receipt mismatch" in rows[0]["error"]

    runner = read("scripts/experiments/ecg/roi_matrix.py")
    main_tail = runner.split(
        "certify_sniper_semantic_work(rows, args, policies)", 1)[1]
    assert main_tail.index("write_outputs(out_dir, rows)") < (
        main_tail.index(
            "validate_gem5_compact_k2m_streamshield_rows"))


def test_proposal_compact_k2m_streamshield_native_path_is_reachable(
        tmp_path):
    binary = tmp_path / "pr"
    compile_result = subprocess.run(
        [
            "g++", "-std=c++17", "-O0", "-g", "-DNDEBUG",
            "-DNO_M5OPS", "-fopenmp",
            "-Ibench/include/external/gapbs",
            "-Ibench/include/graphbrew",
            "-Ibench/include/external",
            "-Ibench/include",
            "bench/src_gem5/pr.cc", "-o", str(binary),
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=300)
    assert compile_result.returncode == 0, (
        compile_result.stdout + compile_result.stderr)
    compact_env = {
        "PATH": "/usr/bin:/bin",
        "HOME": "/tmp",
        "TMPDIR": str(tmp_path),
        "LC_ALL": "C",
        "LANG": "C",
        "OMP_NUM_THREADS": "1",
        "GRAPHBREW_PREFETCHER": "none",
        "GEM5_ENABLE_ECG_EXTRACT": "1",
        "GEM5_ECG_PFX_MODE": "6",
        "ECG_PREFETCH_MODE": "6",
        "ECG_EDGE_MASK_SCHED": "2",
        "ECG_EDGE_MASK_EPOCH": "1",
        "ECG_EDGE_MASK_LINEMIN": "1",
        "ECG_EDGE_MASK_EPOCHS": "32",
        "ECG_EDGE_MASK_PACK_BITS": "64",
        "GEM5_ENABLE_ECG_STREAM_LOAD2": "1",
        "GEM5_ENABLE_ECG_PLOAD": "1",
        "GEM5_ECG_ISA_VARIANT": "mask",
        "GEM5_ECG_COMPACT_K2M_SS": "1",
        "ECG_STREAM_BYPASS": "1",
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
        "GEM5_GRAPHBREW_CTX": str(tmp_path / "compact-context.json"),
    }

    compact = subprocess.run(
        [
            str(binary), "-g", "8", "-k", "2", "-o", "0",
            "-n", "1", "-i", "1",
        ],
        cwd=PROJECT_ROOT, env=compact_env,
        capture_output=True, text=True,
        timeout=60)
    compact_text = compact.stdout + compact.stderr
    assert compact.returncode == 0, compact_text
    assert "[ECG_K2_MLOAD_C_SS]" in compact_text
    assert "[ECG-METADATA-FATAL]" not in compact_text

    wide_env = dict(compact_env)
    wide_env.pop("GEM5_ECG_COMPACT_K2M_SS")
    wide_env.update({
        "ECG_RECORD_VARIABLE_WIDTH": "0",
        "ECG_EDGE_RECORD_BYTES": "8",
        "ECG_EXPECT_BYTES_PER_EDGE": "8",
        "GEM5_GRAPHBREW_CTX": str(tmp_path / "wide-context.json"),
    })
    wide = subprocess.run(
        [
            str(binary), "-g", "8", "-k", "2", "-o", "0",
            "-n", "1", "-i", "1",
        ],
        cwd=PROJECT_ROOT, env=wide_env,
        capture_output=True, text=True,
        timeout=60)
    wide_text = wide.stdout + wide.stderr
    assert wide.returncode == 0, wide_text
    assert (
        "[ECG_K2_MLOAD] PR computed-address masked load "
        "+ StreamShield record load ACTIVE"
    ) in wide_text
    assert "[ECG_K2_MLOAD_C_SS]" not in wide_text
    assert "[ECG-METADATA-FATAL]" not in wide_text

    receipt = re.compile(
        r"\[ECG-PR-RESULT iterations=(\d+) semantic_edges=(\d+) "
        r"score_checksum=([0-9a-fA-F]+)\]")
    compact_result = receipt.search(compact_text)
    wide_result = receipt.search(wide_text)
    assert compact_result is not None
    assert wide_result is not None
    assert compact_result.groups() == wide_result.groups()


def test_gem5_pr_semantic_receipts_fail_closed():
    args = SimpleNamespace(benchmark="pr", suite="gem5")
    rows = [
        {
            "simulator": "gem5", "status": "ok", "options": "-i 1",
            "l3_size": "128kB", "l3_ways": 16, "prefetcher": "none",
            "pr_iterations": 1, "pr_semantic_edges": 100,
            "pr_score_checksum": "abc",
        },
        {
            "simulator": "gem5", "status": "ok", "options": "-i 1",
            "l3_size": "128kB", "l3_ways": 16, "prefetcher": "none",
            "pr_iterations": 1, "pr_semantic_edges": 100,
            "pr_score_checksum": "abc",
        },
    ]
    roi_matrix.certify_gem5_pr_results(rows, args)
    assert all(row["pr_result_matched"] == 1 for row in rows)

    rows[1]["pr_score_checksum"] = "def"
    roi_matrix.certify_gem5_pr_results(rows, args)
    assert all(row["status"] == "error" for row in rows)
    assert all(row["timing_valid_for_speedup"] == "0" for row in rows)


def test_gem5_variant_receipt_is_machine_validated():
    good = {"timing_valid_for_speedup": "1"}
    text = (
        "[ECG-VARIANT-RECEIPT sim=gem5 requested=lru_only "
        "effective=6 dueling=0]")
    assert roi_matrix.apply_gem5_variant_receipt(
        good, text, "lru_only", required=True)
    assert good["gem5_variant_effective_receipt"] == 6

    bad = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_variant_receipt(
        bad, text, "epoch_first", required=True)
    assert bad["status"] == "error"
    assert bad["timing_valid_for_speedup"] == "0"


def test_setup_gem5_uses_dedicated_x86_extract_work_id():
    text = read("scripts/setup_gem5.py")
    assert "legacy content-based PFX/mask multiplexing" in text
    assert "GRAPHBREW_ECG_EXTRACT_MASK_WORK_ID" in text