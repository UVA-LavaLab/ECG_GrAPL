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
    assert (
        "mem/cache/array_attribution.patch", "."
    ) in setup_gem5.UNIFIED_DIFF_PATCHES
    assert (
        "arch/riscv/vector_vtype_guard.patch", "."
    ) in setup_gem5.UNIFIED_DIFF_PATCHES


def test_riscv_vector_config_rejects_reserved_vsew_without_asserting():
    patch = read(
        "bench/include/gem5_sim/overlays/arch/riscv/"
        "vector_vtype_guard.patch")
    assert "GRAPHBREW-RISCV-VTYPE-GUARD" in patch
    assert "const bool invalidVsew = newVtype.vsew > 3;" in patch
    assert "invalidVsew ? 0 : getSew(newVtype.vsew)" in patch
    assert "invalidVsew ||" in patch


def test_every_required_gem5_patch_is_tracked():
    tracked = set(subprocess.run(
        ["git", "ls-files"],
        cwd=PROJECT_ROOT, capture_output=True, text=True, check=True,
    ).stdout.splitlines())
    for relative, _target in setup_gem5.UNIFIED_DIFF_PATCHES:
        path = (
            PROJECT_ROOT / "bench/include/gem5_sim/overlays" / relative)
        repo_relative = str(path.relative_to(PROJECT_ROOT))
        assert path.is_file(), f"required gem5 patch is missing: {relative}"
        assert repo_relative in tracked, (
            f"required gem5 patch is untracked: {repo_relative}")
        if relative == "mem/cache/array_attribution.patch":
            assert not re.search(
                r"(?m)^@@ -\d+(?:,0)? \+\d+", path.read_text()), (
                "array attribution patch needs real context; insertion-only "
                "line-number hunks can silently land in the wrong function")


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
    assert "0x03: ecg_bind_iload_u32" in decoder
    assert "0x06: ecg_bind_load_u32" in decoder
    assert "0x07: ecg_bind_load_s32" in decoder
    assert "0x08: ecg_bind_load_u64" in decoder
    assert "0x09: ecg_bind_load_cw24" in decoder
    assert "0x0A: ecg_bind_load_f32" in decoder
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
    assert "reusePlanDistance(" in policy
    assert policy.count("readEcgReusePlan(") >= 2
    assert policy.count(
        "!got && !requestBoundEcgProducerEnabled()") >= 2
    assert "GRAPHBREW_ECG_EXTRACT2_WORK_ID" in setup

    request_ext = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "ecg_reuse_bind_request_ext.hh")
    assert "attachEcgReusePlan" in request_ext
    assert "readEcgReusePlan" in request_ext
    assert "epoch2_" in request_ext
    assert "epoch_count_" in request_ext
    assert "current_epoch_" in request_ext
    assert "context_id_" in request_ext
    assert "sequence_" in request_ext
    assert "class EcgReuseBindMshrState" in request_ext

    exec_patch = read(
        "bench/include/gem5_sim/overlays/cpu/exec_context_ecg_producer.patch")
    dyn_patch = read(
        "bench/include/gem5_sim/overlays/cpu/o3/dyn_inst_ecg_producer.patch")
    lsq_patch = read(
        "bench/include/gem5_sim/overlays/cpu/o3/lsq_ecg_producer.patch")
    assert "setEcgLoadHint2" in exec_patch
    assert "setEcgLoadHint2" in dyn_patch
    assert "attachEcgReusePlan" in lsq_patch
    assert "ecg_current_epoch" in lsq_patch
    assert "ecg_context_id" in lsq_patch
    assert "ecg_sequence" in lsq_patch
    assert 'reuse_plan_depth == "2"' in graph_se
    assert '"GRASP_HOT_FRACTION"' in graph_se


def test_gem5_reuse_plan_uses_architectural_epoch_context_csrs():
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
    # loads such as ecg_flow_load_compact deliberately do not; ReuseBind owns
    # delivery on the subsequent property Request. The format CSR count changes
    # for compact decode instructions.
    assert decoder.count("MISCREG_ECG_CUR_EPOCH") == 18
    assert decoder.count("MISCREG_ECG_CONTEXT") == 18
    assert decoder.count("MISCREG_ECG_RECORD_FORMAT") == 2
    assert 'asm volatile ("csrw 0x800, %0"' in harness
    assert 'asm volatile ("csrw 0x801, %0"' in harness
    assert 'asm volatile ("csrw 0x802, %0"' in harness
    assert "GEM5_SET_VERTEX_EPOCH" in harness
    assert "GEM5_SET_MONOTONIC_VERTEX_EPOCH" in harness
    assert "GEM5_SET_QUANTIZED_VERTEX_EPOCH" in harness
    assert "gem5_ecg_update_current_epoch_csr" in harness
    assert "gem5_ecg_current_epoch_csr_changed" in harness
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

    for kernel in ("bfs", "sssp", "bc", "cc"):
        source = read(f"bench/src_gem5/{kernel}.cc")
        assert "GEM5_ECG_BEGIN_CONTEXT();" in source
        assert "GEM5_ECG_END_CONTEXT();" in source
        assert source.index("GEM5_ECG_BEGIN_CONTEXT();") < source.index(
            "GEM5_RESET_STATS();")
        assert source.index("GEM5_DUMP_STATS();") < source.index(
            "GEM5_ECG_END_CONTEXT();")
        assert "Gem5EcgEpochQuantizer epoch_quantizer;" in source
        assert "GEM5_SET_QUANTIZED_VERTEX_EPOCH(" in source
        assert "GEM5_SET_VERTEX_EPOCH(" not in source
    pr = read("bench/src_gem5/pr.cc")
    assert "GEM5_ECG_BEGIN_CONTEXT();" in pr
    assert "GEM5_ECG_END_CONTEXT();" in pr
    assert pr.index("GEM5_ECG_BEGIN_CONTEXT();") < pr.index(
        "GEM5_RESET_STATS();")
    assert pr.index("GEM5_DUMP_STATS();") < pr.index(
        "GEM5_ECG_END_CONTEXT();")
    assert "Gem5EcgMonotonicEpochCursor epoch_cursor;" in pr
    assert "GEM5_SET_MONOTONIC_VERTEX_EPOCH(epoch_cursor, u);" in pr


def test_gem5_monotonic_epoch_cursor_is_exact(tmp_path):
    source = tmp_path / "epoch_cursor.cc"
    binary = tmp_path / "epoch_cursor"
    source.write_text(
        r'''
#include <cstdint>
#include "bench/include/gem5_sim/gem5_harness.h"
#include "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.h"

int main() {
    Gem5EcgMonotonicEpochCursor cursor;
    for (uint64_t vertices = 1; vertices <= 257; ++vertices) {
        for (uint32_t epochs = 2; epochs <= 64; ++epochs) {
            cursor.reset(vertices, epochs);
            for (uint64_t vertex = 0; vertex < vertices; ++vertex) {
                if (cursor.epoch(vertex) !=
                        gem5_ecg_quantize_current_epoch(
                            vertex, vertices, epochs)) return 1;
                if (graphbrew::sniper::quantizeEcgEpoch(
                        vertex, vertices, epochs) !=
                        gem5_ecg_quantize_current_epoch(
                            vertex, vertices, epochs)) return 8;
                if (ecg_reuse_plan::currentEpoch(
                        static_cast<uint32_t>(vertex),
                        static_cast<uint32_t>(vertices), epochs) !=
                        gem5_ecg_quantize_current_epoch(
                            vertex, vertices, epochs)) return 9;
            }
        }
    }
    cursor.reset(262144, 16);
    for (uint64_t vertex = 0; vertex < 262144; ++vertex)
        if (cursor.epoch(vertex) != (vertex >> 14)) return 2;
    gem5_ecg_reset_current_epoch_csr_shadow(0);
    if (gem5_ecg_current_epoch_csr_changed(0)) return 3;
    if (!gem5_ecg_current_epoch_csr_changed(1)) return 4;
    if (gem5_ecg_current_epoch_csr_changed(1)) return 5;
    if (!gem5_ecg_current_epoch_csr_changed(0)) return 6;
    Gem5EcgEpochQuantizer quantizer;
    for (uint64_t vertices = 1; vertices <= 257; ++vertices) {
        for (uint32_t epochs = 2; epochs <= 64; ++epochs) {
            quantizer.reset(vertices, epochs);
            for (uint64_t vertex = 0; vertex < vertices; ++vertex) {
                if (quantizer.epoch(vertex) !=
                        gem5_ecg_quantize_current_epoch(
                            vertex, vertices, epochs)) return 7;
            }
        }
    }
    return 0;
}
''')
    subprocess.run([
        "g++", "-std=c++17", "-O2", "-DNO_M5OPS",
        f"-I{PROJECT_ROOT}",
        f"-I{PROJECT_ROOT / 'bench/include/external/gapbs'}",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        str(source), "-o", str(binary),
    ], check=True, cwd=PROJECT_ROOT)
    subprocess.run([str(binary)], check=True)


def test_schedule2_runner_selects_adaptive_variants_and_rejects_o3(monkeypatch):
    monkeypatch.delenv("ECG_VARIANT", raising=False)
    monkeypatch.setenv("ECG_REUSE_PLAN_DEPTH", "2")
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
    assert "two-epoch ReusePlan O3 requires the RISC-V masked property-load" in runner
    assert 'args.gem5_cpu_type == "O3"' in runner
    assert "reuse_bind_active" in graph_se
    assert "two-epoch ReusePlan O3 requires the masked property-load path" in graph_se
    assert "prefetcher none or STRIDE" in runner
    assert "GEM5_ECG_EPOCH_REGION_INDICES" in graph_se
    assert "GEM5_ECG_EPOCH_REGION_INDEX" in graph_se
    assert "GEM5_ECG_ISA_VARIANT" in graph_se
    verifier = read("scripts/experiments/ecg/verify/ecg.py")
    assert "required = set(range(32))" in verifier


def test_gem5_reuse_plan_uses_configured_epoch_count_not_packed4_cap():
    for path in (
        "bench/src_gem5/pr.cc",
        "bench/src_gem5/bfs.cc",
        "bench/src_gem5/sssp.cc",
        "bench/src_gem5/bc.cc",
        "bench/src_gem5/cc.cc",
    ):
        text = read(path)
        assert 'gem5_env_int_clamped("ECG_EDGE_MASK_EPOCHS"' in text
        assert "ecg_reuse_plan_depth != 2" in text
        assert "requested_epoch_count" in text
    pr = read("bench/src_gem5/pr.cc")
    assert "two-epoch ReusePlan record ON" in pr
    assert "buildInEdgeReusePlanRecords" in pr
    cache_context = read("bench/include/cache_sim/graph_cache_context.h")
    assert 'std::getenv("ECG_EDGE_MASK_PACK") && reuse_plan_depth != 2' in cache_context


def test_gem5_reuse_plan_mailbox_is_cleared_after_governed_load():
    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh")
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    assert "clearDecodedEcgExtractHint()" in context
    assert "if (tier == 0)" not in context
    assert "decodedEcgEpochCountStorage().store(2" in context
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
    assert 'packed8+reuse_plan+ecg.extract2' in runner


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


def test_reuse_plan_property_load_clears_mailbox_without_extra_instruction():
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    reuse_plan_load = decoder.split("0x03: ecg_bind_iload_u32", 1)[1].split(
        "}}, ea_code={{", 1)[0]
    assert "Rd = Mem_uw;" in reuse_plan_load
    assert "clearDecodedEcgExtractHint();" in reuse_plan_load
    assert "traceExpectedEcgExtractHint2(packed);" in decoder

    harness = read("bench/include/gem5_sim/gem5_harness.h")
    helper = harness.split(
        "inline uint32_t gem5_ecg_bind_iload_u32(", 1)[1].split(
            "inline uint32_t gem5_ecg_bind_iload_compact(", 1)[0]
    assert "gem5_trace_ecg_reuse_plan_expect" not in helper
    compact_helper = harness.split(
        "inline uint32_t gem5_ecg_bind_iload_compact(", 1)[1].split(
            "inline uint32_t gem5_ecg_bind_iload_compact_traced(", 1)[0]
    assert "gem5_trace_ecg_reuse_plan_expect" not in compact_helper

    for kernel in ("bfs", "sssp", "bc", "cc"):
        source = read(f"bench/src_gem5/{kernel}.cc")
        for block in source.split("if (ecg_bind_iload_on) {")[1:]:
            canonical = block.split("} else {", 1)[0]
            assert "GEM5_ECG_CLEAR_EXTRACT2_HINT" not in canonical, kernel

    pr = read("bench/src_gem5/pr.cc")
    canonical_pr = pr.split("if (ecg_bind_iload_on) {", 1)[1].split(
        "continue;", 1)[0]
    assert "GEM5_ECG_CLEAR_EXTRACT2_HINT" not in canonical_pr
    assert "gem5_ecg_clear_extract2_hint" not in canonical_pr
    assert ("GEM5_ECG_CLEAR_EXTRACT2_HINT" in pr
            or "gem5_ecg_clear_extract2_hint()" in pr)


def test_reuse_plan_computed_address_variant_is_distinct_from_indexed_load():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    assert "GEM5_ECG_ISA_VARIANT" in harness
    assert '"ecg_isa_variant"' in runner
    assert 'env["GEM5_ECG_ISA_VARIANT"] = args.ecg_isa_variant' in runner
    assert "SNIPER_REUSE_PLAN_TRANSPORT_MATCHED" in runner
    assert "matched-reuse_bind-sideband-model" in runner
    assert '"prototype_computed_address_load"' in runner
    assert "architectural compact FlowThrough record load" in runner
    assert "request-bound property load with per-event tracing disabled" in (
        runner)
    assert '"architectural_compact_reuse_bind_flowthrough"' in runner
    assert 'row["gem5_ecg_epoch_channel"]' not in runner
    assert 'base["gem5_ecg_epoch_channel"]' in runner
    assert "transport.reuse_plan_depth == 2" in runner
    assert 'std::strcmp(value, "computed") == 0' in harness
    assert '".insn r 0x0b, 0x2, 0x18' in harness
    assert '".insn r 0x0b, 0x2, 0x1c' in harness
    assert '".insn r 0x0b, 0x2, 0x20' in harness
    assert '".insn r 0x0b, 0x2, 0x24' in harness
    assert '".insn r 0x0b, 0x2, 0x28' in harness

    u32 = decoder.split("0x06: ecg_bind_load_u32", 1)[1].split(
        "// 0x07 ReuseBind S32.D32", 1)[0]
    s32 = decoder.split("0x07: ecg_bind_load_s32", 1)[1].split(
        "// 0x08 ReuseBind U64.D32", 1)[0]
    u64 = decoder.split("0x08: ecg_bind_load_u64", 1)[1].split(
        "// 0x09 ReuseBind U32.CW24", 1)[0]
    compact = decoder.split(
        "0x09: ecg_bind_load_cw24", 1)[1].split(
            "// 0x0A ReuseBind F32.D32", 1)[0]
    f32 = decoder.split("0x0A: ecg_bind_load_f32", 1)[1].split(
        "// 0x0B ReusePlan-Compact", 1)[0]
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
        "0x0: ecg_bind_iload_compact", 1)[1].split(
            "\n                }", 1)[0]
    assert "MISCREG_ECG_RECORD_FORMAT" in fused_compact
    assert "id_bits + tier_bits + 2 * epoch_bits > 32" in fused_compact
    assert "(fmt & 0x80000000U) == 0" in fused_compact, (
        "the fused compact load must reject a format word that declares no "
        "tier width, because tier_bits=0 is itself a legal width")
    assert "Rs1 + static_cast<uint64_t>(dest_id) * 4" in fused_compact
    assert "xc->setEcgLoadHint2(" in fused_compact
    assert '".insn r 0x0b, 0x2, 0x2c' in harness

    expected_helpers = {
        "pr": ("gem5_ecg_bind_load_f32",),
        "bfs": ("gem5_ecg_bind_load_s32",),
        "sssp": (
            "gem5_ecg_bind_load_s32",
            "gem5_ecg_bind_load_cw24",
        ),
        "bc": (
            "gem5_ecg_bind_load_s32",
            "gem5_ecg_bind_load_u64",
        ),
        "cc": ("gem5_ecg_bind_load_s32",),
    }
    for kernel, helpers in expected_helpers.items():
        source = read(f"bench/src_gem5/{kernel}.cc")
        assert "gem5_ecg_bind_computed_address_enabled()" in source
        for helper in helpers:
            assert helper in source
        assert "ECG_REUSE_BIND_LOAD" in source
        assert "ECG_REUSE_BIND_ILOAD" in source


def test_riscv_gem5_build_unswitches_runtime_policy_loops():
    makefile = read("Makefile")
    common_flags = makefile.split(
        "CXXFLAGS_GEM5 :=", 1)[1].splitlines()[0]
    flags = makefile.split("CXXFLAGS_GEM5_RISCV :=", 1)[1].splitlines()[0]
    assert "-O3" in common_flags
    assert "-O1" not in common_flags
    assert "-funswitch-loops" in flags


def test_pagerank_updates_do_not_reload_just_stored_scores():
    for source in ("bench/src_gem5/pr.cc", "bench/src_sniper/pr.cc"):
        text = read(source)
        assert "fabs(scores[u] - old_score)" not in text
        assert "outgoing_contrib[u] = scores[u]" not in text
        assert "const ScoreT new_score" in text


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
    assert "ecg_bind_iload_compact" in decoder
    compact_decode = decoder.split(
        "0x0B: decode ECG_WIDTH", 1)[1].split(
            "\n                }", 1)[0]
    assert "0x0: ecg_bind_iload_compact" in compact_decode
    assert "0x1:" not in compact_decode

    assert "GEM5_ECG_COMPACT_FUSED" in graph_se
    assert "GEM5_ECG_COMPACT_FUSED=1 but" in guest
    assert "std::abort()" in guest
    assert "[ECG_REUSE_BIND_ILOAD_C]" in guest

    # Tracing is a separate helper selected outside the loop; the untraced hot
    # path must not pay a disabled-trace guard on every edge.
    untraced = harness.split(
        "inline uint32_t gem5_ecg_bind_iload_compact(", 1)[1].split(
            "inline uint32_t gem5_ecg_bind_iload_compact_traced(", 1)[0]
    assert "gem5_trace_ecg_reuse_plan_expect" not in untraced


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
        active, "[ECG_REUSE_BIND_ILOAD_C] PR ACTIVE", requested=True)
    assert active["gem5_compact_fused_active"] == 1
    assert active["gem5_ecg_delivery"] == "ecg.bind.iload.compact"
    assert "error" not in active

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_fused_receipt(
        missing, "[ECG_REUSE_BIND_ILOAD] PR ACTIVE", requested=True)
    assert missing["gem5_compact_fused_active"] == 0
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"

    baseline = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_fused_receipt(
        baseline, "", requested=False)
    assert baseline["gem5_compact_fused_active"] == 0
    assert "error" not in baseline


def test_proposal_compact_reuse_bind_flowthrough_is_fail_closed():
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    guest = read("bench/src_gem5/pr.cc")
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa")
    graph_se = read(
        "bench/include/gem5_sim/configs/graphbrew/graph_se.py")

    stream_block = decoder.split(
        "0x7: ecg_flow_load_compact", 1)[1].split(
            "\n            }", 1)[0]
    assert "Mem_uw" in stream_block
    assert "MISCREG_ECG_RECORD_FORMAT" in stream_block
    assert "mem_flags=[ECG_FLOWTHROUGH]" in stream_block
    assert "setDecodedEcgExtractHint" not in stream_block
    assert '".insn i 0x0b, 0x7' in harness
    assert "gem5_ecg_flow_load_compact_instruction" in guest
    assert "gem5_ecg_bind_load_f32" in guest
    assert "wide_reuse_bind_flowthrough_on" in guest
    assert "in_edge_pair32_flat.data()" in guest
    assert "in_edge_pair32_flat.size() * sizeof(uint32_t)" in guest
    assert "GEM5_ECG_COMPACT_REUSE_BIND_FLOW=1 but" in guest
    assert "[ECG_REUSE_BIND_LOAD_C_FLOW]" in guest
    assert "reusePlanOffsetsMatchInCsr" in guest
    assert "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=1" in guest
    measured_roi = guest.split(
        "GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE)", 1)[1].split(
            "GEM5_WORK_END(GEM5_WORK_COMPUTE)", 1)[0]
    proposal_call = (
        "PageRankPullGSCompactReuseBindFlowthroughIteration(")
    assert measured_roi.count(proposal_call) == 1
    assert measured_roi.index(proposal_call) < measured_roi.index(
        "for (NodeID u = 0; u < g.num_nodes(); u++)")
    assert not re.search(r"\b\w*_off\s*\[", measured_roi)
    assert "g.in_offset(0)" in measured_roi
    assert "g.in_offset(u + 1)" in measured_roi
    assert "csr_pair_begin = end" in measured_roi
    assert "GEM5_ECG_COMPACT_REUSE_BIND_FLOW" in graph_se

    active = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        active,
        "[ECG_REUSE_BIND_LOAD_C_FLOW] PR ACTIVE\n"
        "[ECG-FLOWTHROUGH sim=gem5 cache=l3cache addr=0x40 "
        "vaddr=0x40 size=4 source=request-flag allocate=0]",
        requested=True)
    assert active["proposal_path_active"] == 1
    assert active["gem5_flowthrough_request_flag_events"] == 1
    assert active["gem5_flowthrough_request_flag_size4_events"] == 1
    assert active["gem5_flowthrough_request_flag_bad_size_events"] == 0
    assert active["gem5_flowthrough_all_events"] == 1
    assert active["gem5_flowthrough_range_events"] == 0
    assert active["gem5_ecg_delivery"] == (
        "ecg.flow.load.compact+ecg.bind.load.f32")

    performance = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        performance,
        "[ECG_REUSE_BIND_LOAD_C_FLOW] PR compact FlowThrough record load "
        "+ computed-address computed-address property load ACTIVE "
        "(id_bits=16 epoch_bits=5 tier_bits=2)\n",
        requested=True,
        require_trace_receipts=False,
        performance_requested=True)
    assert performance["proposal_performance_mode_active"] == 1
    assert performance["proposal_compact_id_bits"] == 16
    assert performance["proposal_compact_epoch_bits"] == 5
    assert performance["proposal_compact_tier_bits"] == 2
    assert "error" not in performance

    # The tier width is read from the guest, not assumed: the n18/128-epoch
    # cell only fits because the two tier bits are dropped.
    tierless = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        tierless,
        "[ECG_REUSE_BIND_LOAD_C_FLOW] PR compact FlowThrough record load "
        "+ computed-address computed-address property load ACTIVE "
        "(id_bits=18 epoch_bits=7 tier_bits=0)\n",
        requested=True,
        require_trace_receipts=False,
        performance_requested=True)
    assert tierless["proposal_compact_id_bits"] == 18
    assert tierless["proposal_compact_epoch_bits"] == 7
    assert tierless["proposal_compact_tier_bits"] == 0
    assert (
        tierless["proposal_compact_id_bits"] +
        tierless["proposal_compact_tier_bits"] +
        2 * tierless["proposal_compact_epoch_bits"]) == 32
    assert "error" not in tierless

    # A guest that reports no tier width cannot be timed as a compact cell.
    untyped = {"timing_valid_for_speedup": "1"}
    roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        untyped,
        "[ECG_REUSE_BIND_LOAD_C_FLOW] PR compact FlowThrough record load "
        "ACTIVE (id_bits=16 epoch_bits=5)\n",
        requested=True,
        require_trace_receipts=False,
        performance_requested=True)
    assert untyped["status"] == "error"
    assert "proposal_compact_tier_bits" not in untyped

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        missing, "[ECG_REUSE_BIND_LOAD] PR ACTIVE", requested=True)
    assert missing["status"] == "error"
    assert missing["timing_valid_for_speedup"] == "0"

    missing_flowthrough = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        missing_flowthrough, "[ECG_REUSE_BIND_LOAD_C_FLOW] PR ACTIVE", requested=True)
    assert missing_flowthrough["status"] == "error"
    assert "request-flag FlowThrough" in missing_flowthrough["error"]

    wrong_width = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_compact_reuse_bind_flowthrough_receipt(
        wrong_width,
        "[ECG_REUSE_BIND_LOAD_C_FLOW] PR ACTIVE\n"
        "[ECG-FLOWTHROUGH sim=gem5 cache=l3cache addr=0x40 "
        "vaddr=0x40 size=8 source=request-flag allocate=0]",
        requested=True)
    assert wrong_width["status"] == "error"
    assert "4-byte request-flag record requests" in wrong_width["error"]


def test_gem5_csr_substitution_receipt_is_fail_closed():
    receipt = (
        "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=1 valid=1 "
        "offset_source=csr direction=in rows=256 records=4096]")
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_csr_substitution_receipt(
        row, receipt, "pr", required=True)
    assert row["ecg_csr_substitution_receipt_count"] == 1
    assert row["ecg_csr_substitution_active"] == 1
    assert row["ecg_csr_substitution_valid"] == 1
    assert row["ecg_offset_source"] == "csr"
    assert row["ecg_csr_substitution_rows"] == 256
    assert row["ecg_csr_substitution_records"] == 4096
    assert "error" not in row

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        missing, "", "pr", required=True)
    assert missing["status"] == "error"

    duplicate = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        duplicate, f"{receipt}\n{receipt}", "pr", required=True)
    assert duplicate["status"] == "error"

    invalid = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        invalid,
        "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=1 valid=0 "
        "offset_source=csr direction=in rows=256 records=4096]",
        "pr", required=True)
    assert invalid["status"] == "error"
    assert invalid["timing_valid_for_speedup"] == "0"

    empty = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        empty, receipt.replace("records=4096", "records=0"),
        "pr", required=True)
    assert empty["status"] == "error"

    wrong_record_count = {
        "timing_valid_for_speedup": "1",
        "gem5_reuse_plan_sidecar_records": 4095,
    }
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        wrong_record_count, receipt, "pr", required=True)
    assert wrong_record_count["status"] == "error"
    assert wrong_record_count["timing_valid_for_speedup"] == "0"

    optional_invalid = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        optional_invalid,
        "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=0 valid=0 "
        "offset_source=pair direction=in rows=256 records=4096]",
        "pr", required=False)
    assert "error" not in optional_invalid
    assert optional_invalid["timing_valid_for_speedup"] == "1"

    optional_duplicate = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_csr_substitution_receipt(
        optional_duplicate, f"{receipt}\n{receipt}", "pr", required=False)
    assert optional_duplicate["ecg_csr_substitution_receipt_count"] == 2
    assert "error" not in optional_duplicate
    assert optional_duplicate["timing_valid_for_speedup"] == "1"


def test_gem5_non_pr_reuse_plan_uses_canonical_out_csr_offsets():
    for kernel in ("bfs", "bc", "cc", "sssp"):
        guest = read(f"bench/src_gem5/{kernel}.cc")
        assert "gem5_require_canonical_reuse_plan_offsets" in guest
        measured_roi = guest.split(
            "GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE)", 1)[1].split(
                "GEM5_WORK_END(GEM5_WORK_COMPUTE)", 1)[0]
        assert not re.search(r"\b\w*_off\s*\[", measured_roi), kernel
        if kernel == "sssp":
            assert "RelaxEdges_Gem5(" in measured_roi
            helper = guest.split(
                "inline void RelaxEdges_Gem5", 1)[1].split(
                    "pvector<WeightT> DeltaStep", 1)[0]
            assert "pair_off" not in helper
            assert not re.search(r"\b\w*_off\s*\[", helper)
            assert "g.out_offset(" in helper
        else:
            assert "g.out_offset(" in measured_roi, kernel


def test_gem5_array_attribution_is_per_requestor_and_fail_closed():
    values = {
        ("property0", "demand_misses", "cpu.data"): 3,
        ("record", "demand_misses", "cpu.data"): 2,
        ("csr_offsets", "demand_misses", "cpu.data"): 1,
        ("property0", "demand_read_mshr_misses", "cpu.data"): 3,
        ("record", "demand_read_mshr_misses", "cpu.data"): 1,
        ("csr_offsets", "demand_read_mshr_misses", "cpu.data"): 1,
        ("record", "demand_nonread_mshr_misses", "cpu.data"): 1,
        ("property0", "demand_read_bytes", "cpu.data"): 192,
        ("record", "demand_read_bytes", "cpu.data"): 64,
        ("csr_offsets", "demand_read_bytes", "cpu.data"): 64,
        ("other", "demand_misses", "cpu.inst"): 2,
        ("other", "demand_read_mshr_misses", "cpu.inst"): 2,
        ("other", "demand_read_bytes", "cpu.inst"): 128,
    }
    lines = []
    for metric, stat_name in roi_matrix.GEM5_ARRAY_STAT_METRICS.items():
        for category in roi_matrix.GEM5_ARRAY_CATEGORIES:
            for requestor in ("cpu.data", "cpu.inst"):
                value = values.get((category, metric, requestor), 0)
                lines.append(
                    f"system.l3cache.{stat_name}_{category}::"
                    f"{requestor} {value} # test")
    parsed = roi_matrix.parse_gem5_array_stats("\n".join(lines))
    row = {
        **parsed,
        "timing_valid_for_speedup": "1",
        "line_size": "64",
        "benchmark": "pr",
        "policy": "ECG",
        "ecg_reuse_plan_depth": 2,
        "ecg_flowthrough": 1,
        "pr_iterations": 1,
        "gem5_l3_mshrs_actual": 32,
        "l3_demand_data_misses": 6,
        "l3_demand_inst_misses": 2,
        "l3_demand_mshr_data_misses": 6,
        "l3_demand_mshr_inst_misses": 2,
        "dram_read_bytes_cpu_data": 320,
        "dram_read_bytes_cpu_inst": 128,
        "ecg_csr_substitution_active": 1,
    }
    receipt = (
        "[ECG-ARRAY-ATTRIBUTION active=1 schema=2 "
        "categories=10 edge_regions_aliased=1 "
        "p0=scores:0x1000+100 p1=contrib:0x2000+100 "
        "record=0x3000+100 edge=0x4000+100 "
        "edge_other=0x4000+100 csr=0x5000+100 "
        "csr_other=0x5500+100 "
        "plan=0x6000+100]")
    assert roi_matrix.apply_gem5_array_attribution(
        row, receipt, required=True)
    assert row["gem5_array_attribution_active"] == 1
    assert row["gem5_array_edge_regions_aliased"] == 1
    assert row["gem5_array_demand_misses_cpu_data"] == 6
    assert row["gem5_array_demand_read_bytes_cpu_data"] == 320
    assert row["gem5_plan_offset_roi_activity"] == 0
    assert row["gem5_array_expected_record_lines"] == 2
    assert row["gem5_array_attribution_validated"] == 1
    assert "error" not in row

    replayed = dict(row)
    replayed["gem5_cpu_type"] = "O3"
    replayed["gem5_array_record_demand_misses_cpu_data"] = 3
    replayed["l3_demand_data_misses"] = 7
    replayed.pop("error", None)
    replayed["status"] = "ok"
    assert roi_matrix.apply_gem5_array_attribution(
        replayed, receipt, required=True)
    assert replayed["gem5_array_record_replay_excess_lines"] == 1
    assert replayed["gem5_array_record_replay_limit_lines"] == 1

    zero_inst = dict(row)
    for metric in roi_matrix.GEM5_ARRAY_STAT_METRICS:
        for category in roi_matrix.GEM5_ARRAY_CATEGORIES:
            zero_inst[
                f"gem5_array_{category}_{metric}_cpu_inst"] = 0
    for field in (
            "l3_demand_inst_misses",
            "l3_demand_mshr_inst_misses",
            "dram_read_bytes_cpu_inst"):
        zero_inst.pop(field, None)
    zero_inst.pop("error", None)
    zero_inst["timing_valid_for_speedup"] = "1"
    assert roi_matrix.apply_gem5_array_attribution(
        zero_inst, receipt, required=True)
    assert zero_inst["gem5_array_demand_misses_cpu_inst"] == 0

    missing_csr_range = dict(row)
    assert not roi_matrix.apply_gem5_array_attribution(
        missing_csr_range, receipt.replace(
            "csr=0x5000+100", "csr=0+0"), required=True)
    assert missing_csr_range["gem5_array_attribution_validated"] == 0
    assert "omitted a required PR range" in missing_csr_range["error"]

    missing = dict(row)
    missing.pop("gem5_array_property0_demand_misses_cpu_data")
    assert not roi_matrix.apply_gem5_array_attribution(
        missing, receipt, required=True)
    assert missing["status"] == "error"

    unattributed = dict(row)
    unattributed[
        "gem5_array_unattributed_demand_misses_cpu_data"] = 1
    unattributed["l3_demand_data_misses"] = 7
    assert not roi_matrix.apply_gem5_array_attribution(
        unattributed, receipt, required=True)
    assert "unclassified demand traffic" in unattributed["error"]

    plan_offsets = dict(row)
    plan_offsets[
        "gem5_array_plan_offsets_demand_read_bytes_cpu_data"] = 64
    plan_offsets["dram_read_bytes_cpu_data"] = 384
    assert not roi_matrix.apply_gem5_array_attribution(
        plan_offsets, receipt, required=True)
    assert "still accessed plan offsets" in plan_offsets["error"]

    excessive_other = dict(row)
    excessive_other["gem5_array_other_demand_misses_cpu_data"] = 1
    excessive_other[
        "gem5_array_other_demand_read_mshr_misses_cpu_data"] = 1
    excessive_other["gem5_array_other_demand_read_bytes_cpu_data"] = 64
    excessive_other["l3_demand_data_misses"] = 7
    excessive_other["l3_demand_mshr_data_misses"] = 7
    excessive_other["dram_read_bytes_cpu_data"] = 384
    excessive_other.pop("error", None)
    excessive_other["timing_valid_for_speedup"] = "1"
    assert not roi_matrix.apply_gem5_array_attribution(
        excessive_other, receipt, required=True)
    assert "declared 2% attribution bound" in excessive_other["error"]

    optional = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_array_attribution(
        optional, "", required=False)
    assert optional["gem5_array_attribution_validated"] == 0
    assert "error" not in optional
    assert optional["timing_valid_for_speedup"] == "1"


def test_gem5_reuse_plan_stamp_coverage_stats_are_resettable_and_parsed(
        tmp_path):
    rp_header = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.hh")
    rp_source = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/ecg_rp.cc")
    for name in (
            "victimSelections",
            "victimRequestInvalid",
            "victimZeroStampedSelections",
            "victimStampedWays",
            "victimPropertyWays",
            "victimPropertyEpochInvalidWays",
            "victimContextMismatchWays",
            "victimAllPropertySelections",
            "victimAllPropertyStampedSelections",
            "victimEpochEligibleSelections",
            "victimEpochDecisiveSelections",
            "victimEpochVsRecencyDecisiveSelections",
            "victimWaySelections"):
        stat_type = "Vector" if name == "victimWaySelections" else "Scalar"
        assert f"statistics::{stat_type} {name}" in rp_header
        assert name in rp_source
    assert "++onlineDuelingStats.victimSelections;" in rp_source
    assert "++onlineDuelingStats.victimRequestInvalid;" in rp_source
    assert "++onlineDuelingStats.victimEpochEligibleSelections;" in rp_source
    assert "++onlineDuelingStats.victimEpochDecisiveSelections;" in rp_source
    assert (
        "++onlineDuelingStats.victimEpochVsRecencyDecisiveSelections;"
        in rp_source)
    assert "victimUsedEpoch(selectedReason, ws[vidx])" in rp_source
    assert (
        rp_source.index("if (!getData(candidate)->valid) return candidate;")
        < rp_source.index("++onlineDuelingStats.victimSelections;")
        < rp_source.index("ecg_policy::selectVictim(\n"
                          "            ws, nc, variant, rrpvMax"))

    stats_path = tmp_path / "stats.txt"
    stats_path.write_text(
        "---------- Begin Simulation Statistics ----------\n"
        "system.l3cache.replacements 20 #\n"
        "system.l3cache.replacement_policy.victimSelections 20 #\n"
        "system.l3cache.replacement_policy.victimRequestInvalid 2 #\n"
        "system.l3cache.replacement_policy.victimZeroStampedSelections 3 #\n"
        "system.l3cache.replacement_policy.victimStampedWays 40 #\n"
        "system.l3cache.replacement_policy.victimPropertyWays 45 #\n"
        "system.l3cache.replacement_policy.victimPropertyEpochInvalidWays 2 #\n"
        "system.l3cache.replacement_policy.victimContextMismatchWays 3 #\n"
        "system.l3cache.replacement_policy.victimAllPropertySelections 1 #\n"
        "system.l3cache.replacement_policy.victimAllPropertyStampedSelections 1 #\n"
        "system.l3cache.replacement_policy.victimEpochEligibleSelections 10 #\n"
        "system.l3cache.replacement_policy.victimEpochDecisiveSelections 4 #\n"
        "system.l3cache.replacement_policy.victimEpochVsRecencyDecisiveSelections 3 #\n"
        "system.l3cache.replacement_policy.victimWaySelections::way0 12 #\n"
        "system.l3cache.replacement_policy.victimWaySelections::way1 8 #\n"
        "---------- End Simulation Statistics   ----------\n")
    parsed = roi_matrix.parse_gem5_sections(stats_path)[0]
    assert parsed["l3_replacements"] == 20
    assert parsed["gem5_reuse_plan_victim_selections"] == 20
    assert parsed["gem5_reuse_plan_victim_request_invalid"] == 2
    assert (
        parsed["gem5_reuse_plan_victim_zero_stamped_selections"] == 3)
    assert parsed["gem5_reuse_plan_victim_stamped_ways"] == 40
    assert parsed["gem5_reuse_plan_victim_property_ways"] == 45
    assert (
        parsed["gem5_reuse_plan_victim_property_epoch_invalid_ways"] == 2)
    assert parsed["gem5_reuse_plan_victim_context_mismatch_ways"] == 3
    assert parsed["gem5_reuse_plan_victim_all_property_selections"] == 1
    assert (
        parsed[
            "gem5_reuse_plan_victim_all_property_stamped_selections"] == 1)
    assert (
        parsed["gem5_reuse_plan_victim_epoch_eligible_selections"] == 10)
    assert (
        parsed["gem5_reuse_plan_victim_epoch_decisive_selections"] == 4)
    assert (
        parsed[
            "gem5_reuse_plan_victim_epoch_vs_recency_decisive_selections"]
        == 3)
    assert parsed["gem5_reuse_plan_victim_way_counts"] == [12, 8]
    assert parsed["gem5_reuse_plan_victim_way_max_index"] == 0
    assert roi_matrix.apply_gem5_reuse_plan_coverage(parsed, required=True)
    assert parsed["gem5_reuse_plan_victim_selection_retry_excess"] == 0
    assert parsed["gem5_reuse_plan_victim_request_valid_share"] == 0.9
    assert (
        parsed["gem5_reuse_plan_victim_valid_zero_stamped_share"]
        == pytest.approx(1 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_mean_stamped_ways"]
        == pytest.approx(40 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_property_stamp_coverage"]
        == pytest.approx(40 / 45))
    assert (
        parsed["gem5_reuse_plan_victim_context_mismatch_share"]
        == pytest.approx(3 / 45))
    assert (
        parsed["gem5_reuse_plan_victim_all_property_share"]
        == pytest.approx(1 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_all_property_stamped_share"]
        == pytest.approx(1 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_epoch_eligible_share"]
        == pytest.approx(10 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_epoch_decisive_share"]
        == pytest.approx(4 / 18))
    assert (
        parsed["gem5_reuse_plan_victim_epoch_decisive_given_eligible"]
        == pytest.approx(0.4))
    assert (
        parsed[
            "gem5_reuse_plan_victim_epoch_vs_recency_decisive_share"]
        == pytest.approx(3 / 18))
    assert parsed["gem5_reuse_plan_victim_way_max_share"] == 0.6
    assert parsed["gem5_reuse_plan_coverage_validated"] == 1

    missing = dict(parsed)
    missing.pop("gem5_reuse_plan_victim_stamped_ways")
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        missing, required=True)
    assert missing["status"] == "error"

    skipped = {}
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        skipped, required=False)
    assert skipped["gem5_reuse_plan_coverage_validated"] == 0

    bad_histogram = dict(parsed)
    bad_histogram["gem5_reuse_plan_victim_way_counts"] = [11, 8]
    bad_histogram.pop("error", None)
    bad_histogram["status"] = "ok"
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        bad_histogram, required=True)
    assert "victim-way histogram" in bad_histogram["error"]

    zero_stamps = dict(parsed)
    zero_stamps.update({
        "ecg_variant_effective": "rrip_first",
        "gem5_reuse_plan_victim_zero_stamped_selections": 20,
        "gem5_reuse_plan_victim_stamped_ways": 0,
        "gem5_reuse_plan_victim_property_ways": 45,
        "gem5_reuse_plan_victim_property_epoch_invalid_ways": 45,
        "gem5_reuse_plan_victim_context_mismatch_ways": 0,
        "gem5_reuse_plan_victim_epoch_eligible_selections": 0,
        "gem5_reuse_plan_victim_epoch_decisive_selections": 0,
        "gem5_reuse_plan_victim_epoch_vs_recency_decisive_selections": 0,
        "status": "ok",
    })
    zero_stamps.pop("error", None)
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        zero_stamps, required=True)
    assert "no live stamped property ways" in zero_stamps["error"]
    assert "never selected a live stamped property" in zero_stamps["error"]

    epoch_inert = dict(parsed)
    epoch_inert.update({
        "ecg_variant_effective": "epoch_first",
        "gem5_reuse_plan_victim_all_property_selections": 0,
        "gem5_reuse_plan_victim_all_property_stamped_selections": 0,
        "gem5_reuse_plan_victim_epoch_eligible_selections": 0,
        "gem5_reuse_plan_victim_epoch_decisive_selections": 0,
        "status": "ok",
    })
    epoch_inert.pop("error", None)
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        epoch_inert, required=True)
    assert "had no all-property victim set" in epoch_inert["error"]

    invalid_no_epoch = dict(parsed)
    invalid_no_epoch.update({
        "ecg_variant_effective": "rrip_no_epoch",
        "gem5_reuse_plan_victim_epoch_eligible_selections": 1,
        "gem5_reuse_plan_victim_epoch_decisive_selections": 1,
        "status": "ok",
    })
    invalid_no_epoch.pop("error", None)
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        invalid_no_epoch, required=True)
    assert "must be epoch-inert" in invalid_no_epoch["error"]

    invalid_recency = dict(parsed)
    invalid_recency.update({
        "ecg_variant_effective": "rrip_no_epoch_recency",
        "gem5_reuse_plan_victim_epoch_eligible_selections": 0,
        "gem5_reuse_plan_victim_epoch_decisive_selections": 0,
        "gem5_reuse_plan_victim_epoch_vs_recency_decisive_selections": 1,
        "status": "ok",
    })
    invalid_recency.pop("error", None)
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        invalid_recency, required=True)
    assert "must match its recency shadow" in invalid_recency["error"]

    vacuous_rrip = dict(parsed)
    vacuous_rrip.update({
        "ecg_variant_effective": "rrip_first",
        "gem5_reuse_plan_victim_epoch_vs_recency_decisive_selections": 0,
        "status": "ok",
    })
    vacuous_rrip.pop("error", None)
    assert not roi_matrix.apply_gem5_reuse_plan_coverage(
        vacuous_rrip, required=True)
    assert "property-recency shadow" in vacuous_rrip["error"]

    rrip = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH")
    assert roi_matrix.requires_gem5_reuse_plan_coverage(
        rrip, roi_matrix.ecg_transport_for(rrip, "pr"), requested=True)
    assert not roi_matrix.requires_gem5_reuse_plan_coverage(
        rrip, roi_matrix.ecg_transport_for(rrip, "pr"), requested=False)
    lru = roi_matrix.parse_policy_spec("LRU")
    assert not roi_matrix.requires_gem5_reuse_plan_coverage(
        lru, roi_matrix.ecg_transport_for(lru, "pr"), requested=True)
    online = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH")
    assert not roi_matrix.requires_gem5_reuse_plan_coverage(
        online, roi_matrix.ecg_transport_for(online, "pr"), requested=True)
    record_lru = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_RECORD_LRU_FLOWTHROUGH")
    assert record_lru.ecg_variant == "record_lru"
    receipt_row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_variant_receipt(
        receipt_row,
        "[ECG-VARIANT-RECEIPT sim=gem5 requested=record_lru "
        "effective=7 dueling=0]",
        "record_lru",
        required=True)
    rrip_no_epoch = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH")
    assert rrip_no_epoch.ecg_variant == "rrip_no_epoch"
    assert roi_matrix.apply_gem5_variant_receipt(
        {"timing_valid_for_speedup": "1"},
        "[ECG-VARIANT-RECEIPT sim=gem5 requested=rrip_no_epoch "
        "effective=8 dueling=0]",
        "rrip_no_epoch",
        required=True)
    rrip_recency = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH")
    assert rrip_recency.ecg_variant == "rrip_no_epoch_recency"
    assert roi_matrix.apply_gem5_variant_receipt(
        {"timing_valid_for_speedup": "1"},
        "[ECG-VARIANT-RECEIPT sim=gem5 "
        "requested=rrip_no_epoch_recency effective=9 dueling=0]",
        "rrip_no_epoch_recency",
        required=True)
    future_tier = roi_matrix.parse_policy_spec(
        "ECG:REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH")
    assert future_tier.ecg_variant == "future_tier_first"
    assert roi_matrix.apply_gem5_variant_receipt(
        {"timing_valid_for_speedup": "1"},
        "[ECG-VARIANT-RECEIPT sim=gem5 "
        "requested=future_tier_first effective=10 dueling=0]",
        "future_tier_first",
        required=True)


def test_gem5_array_attribution_is_explicit_and_pr_scoped(monkeypatch):
    env = {
        "GEM5_GRAPH_ARRAY_STATS": "1",
        "GEM5_REUSE_PLAN_COVERAGE_REQUIRED": "1",
    }
    roi_matrix.scrub_cell_mechanism_env(env)
    assert "GEM5_GRAPH_ARRAY_STATS" not in env
    assert "GEM5_REUSE_PLAN_COVERAGE_REQUIRED" not in env

    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        '{"GEM5_GRAPH_ARRAY_STATS":"1",'
        '"GEM5_REUSE_PLAN_COVERAGE_REQUIRED":"1"}')
    roi_matrix.apply_explicit_cell_mechanism_env(
        env, roi_matrix.parse_policy_spec("LRU"))
    assert env["GEM5_GRAPH_ARRAY_STATS"] == "1"
    assert env["GEM5_REUSE_PLAN_COVERAGE_REQUIRED"] == "1"

    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/graph_cache_context_gem5.hh")
    assert 'std::strcmp(value, "1") == 0' in context


def test_gem5_array_context_classifies_virtual_regions(tmp_path):
    context_path = tmp_path / "context.json"
    context_path.write_text(json.dumps({
        "num_vertices": 256,
        "num_edges": 4096,
        "edge_epoch_count": 32,
        "flowthrough_base": 3000,
        "flowthrough_size": 100,
        "array_attribution_schema": 2,
        "edge_preferred_base": 4000,
        "edge_preferred_size": 100,
        "edge_other_base": 4000,
        "edge_other_size": 100,
        "edge_regions_aliased": True,
        "csr_offsets_base": 5000,
        "csr_offsets_size": 100,
        "csr_offsets_other_base": 5500,
        "csr_offsets_other_size": 100,
        "plan_offsets_base": 6000,
        "plan_offsets_size": 100,
        "property_regions": [
            {
                "name": "scores", "base": 1000, "size": 100,
                "count": 25, "elem_size": 4, "grasp": True,
            },
            {
                "name": "contrib", "base": 2000, "size": 100,
                "count": 25, "elem_size": 4, "grasp": True,
            },
        ],
    }))
    source = tmp_path / "array_context.cc"
    binary = tmp_path / "array_context"
    source.write_text(r'''
#include <cstdlib>
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"

int main(int argc, char** argv) {
    using namespace gem5::replacement_policy::graph;
    if (argc != 2) return 1;
    setenv("GEM5_GRAPH_ARRAY_STATS", "1", 1);
    setenv("GEM5_GRAPHBREW_CTX", argv[1], 1);
    if (classifyEcgArray(true, 1001) != GraphArrayCategory::Property0)
        return 2;
    if (classifyEcgArray(true, 2001) != GraphArrayCategory::Property1)
        return 3;
    if (classifyEcgArray(true, 3001) != GraphArrayCategory::Record)
        return 4;
    if (classifyEcgArray(true, 4001) != GraphArrayCategory::EdgePreferred)
        return 5;
    if (classifyEcgArray(true, 5001) != GraphArrayCategory::CsrOffsets)
        return 6;
    if (classifyEcgArray(true, 5501) != GraphArrayCategory::CsrOffsetsOther)
        return 7;
    if (classifyEcgArray(true, 6001) != GraphArrayCategory::PlanOffsets)
        return 8;
    if (classifyEcgArray(true, 7001) != GraphArrayCategory::Other)
        return 9;
    if (classifyEcgArray(false, 1001) != GraphArrayCategory::Unattributed)
        return 10;
    if (numGraphArrayCategories() != 10) return 11;
    return 0;
}
''')
    compile_result = subprocess.run(
        [
            "g++", "-std=c++17", "-O2",
            "-Ibench/include/gem5_sim/overlays",
            "-Ibench/include",
            str(source), "-o", str(binary),
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=120)
    assert compile_result.returncode == 0, (
        compile_result.stdout + compile_result.stderr)
    result = subprocess.run(
        [str(binary), str(context_path)],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert output.count("[ECG-ARRAY-ATTRIBUTION active=1") == 1
    assert "schema=2 categories=10 edge_regions_aliased=1" in output
    assert "p0=scores:0x3e8+100 p1=contrib:0x7d0+100" in output
    assert "csr=0x1388+100 csr_other=0x157c+100 plan=0x1770+100" in output


def test_proposal_compact_reuse_bind_flowthrough_cli_guards():
    wrong_kernel = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "bfs",
            "--ecg-isa-variant", "computed",
            "--gem5-compact-reuse-bind-flowthrough", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert wrong_kernel.returncode != 0
    assert "implemented only for --benchmark pr" in (
        wrong_kernel.stdout + wrong_kernel.stderr)

    symmetric = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--policies", "LRU", "--flowthrough", "all", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert symmetric.returncode == 0, symmetric.stdout + symmetric.stderr

    wrong_isa = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "indexed",
            "--gem5-compact-reuse-bind-flowthrough", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert wrong_isa.returncode != 0
    assert "requires --ecg-isa-variant computed" in (
        wrong_isa.stdout + wrong_isa.stderr)

    wrong_policy = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "computed",
            "--gem5-cpu-type", "O3",
            "--policies", "ECG:REUSE_PLAN_1_FLOWTHROUGH",
            "--gem5-compact-reuse-bind-flowthrough", "--dry-run",
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
    assert "requires at least one two-epoch ReusePlan ECG FlowThrough policy" in (
        wrong_policy.stdout + wrong_policy.stderr)

    timing_cpu = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "computed",
            "--gem5-cpu-type", "timing",
            "--gem5-compact-reuse-bind-flowthrough", "--dry-run",
        ],
        cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=60)
    assert timing_cpu.returncode != 0
    assert "requires --gem5-cpu-type O3" in (
        timing_cpu.stdout + timing_cpu.stderr)

    wrong_line = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "computed",
            "--gem5-cpu-type", "O3",
            "--line-size", "128",
            "--gem5-compact-reuse-bind-flowthrough", "--dry-run",
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
        "--ecg-isa-variant", "computed",
        "--gem5-cpu-type", "O3",
        "--policies", "LRU", "ECG:REUSE_PLAN_FLOWTHROUGH",
        "--gem5-compact-reuse-bind-performance", "--dry-run",
    ])
    assert performance_args.gem5_compact_reuse_bind_performance is True

    mixed_modes = subprocess.run(
        [
            sys.executable, str(ROI_MATRIX_PATH),
            "--suite", "gem5", "--benchmark", "pr",
            "--ecg-isa-variant", "computed",
            "--gem5-cpu-type", "O3",
            "--policies", "LRU", "ECG:REUSE_PLAN_FLOWTHROUGH",
            "--gem5-compact-reuse-bind-flowthrough",
            "--gem5-compact-reuse-bind-performance", "--dry-run",
        ],
        cwd=PROJECT_ROOT, env=riscv_env,
        capture_output=True, text=True, timeout=60)
    assert mixed_modes.returncode != 0
    assert "choose either" in (
        mixed_modes.stdout + mixed_modes.stderr)


def test_trace_free_reuse_bind_does_not_override_other_timing_caveats():
    args = roi_matrix.parse_args([])
    args.benchmark = "pr"
    args.prefetcher = "ECG_PFX"
    args.ecg_pfx_delivery = "instruction"
    args.ecg_isa_variant = "computed"
    args.gem5_compact_reuse_bind_performance = True
    args.has_lru_baseline = True
    row = roi_matrix.base_row(
        "gem5", args,
        roi_matrix.parse_policy_spec("ECG:REUSE_PLAN_FLOWTHROUGH"),
        "32kB")
    assert row["timing_valid_for_speedup"] == "0"
    assert row["timing_model"] == "prototype_instruction_delivery"


def test_trace_free_reuse_bind_scrubs_all_gem5_event_traces():
    env = {
        "ECG_REUSE_PLAN_DELIVERY_TRACE": "2048",
        "ECG_FLOWTHROUGH_TRACE": "2048",
        "GEM5_ECG_EXT_TRACE": "2048",
        "ECG_EVICT_TRACE": "2048",
        "ECG_EVICT_TRACE_ROI": "1",
        "UNRELATED": "keep",
    }
    roi_matrix.disable_gem5_event_traces(env)
    assert env == {"UNRELATED": "keep"}


def test_proposal_request_bound_receipt_matches_request_extension():
    log = "\n".join([
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=0 request_seq=17 "
        "dest=9 tier=2 epoch1=3 epoch2=7 current=1 context=4]",
        "[ECG-ReuseBind-ACCEPT sim=gem5 seq=0 request_seq=17 "
        "request_dest=9 fill_dest=9 source=request tier=2 "
        "epoch1=3 epoch2=7 current=1 context=4 "
        "property_elem_bytes=4]",
    ])
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_reuse_bind_receipt(
        row, log, requested=True)
    assert row["gem5_reuse_plan_exact_bind"] == 1
    assert row["gem5_reuse_bind_request_bad_receipts"] == 0

    same_line = {"timing_valid_for_speedup": "1"}
    same_line_log = log.replace(
        "dest=9 tier=2", "dest=11 tier=2").replace(
        "request_dest=9 fill_dest=9",
        "request_dest=11 fill_dest=1")
    assert roi_matrix.apply_gem5_reuse_bind_receipt(
        same_line, same_line_log, requested=True, line_bytes=64)
    assert same_line["gem5_reuse_bind_coalesced_line_accepts"] == 1
    assert same_line["gem5_reuse_bind_exact_vertex_accepts"] == 0

    bad = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_reuse_bind_receipt(
        bad, same_line_log.replace("fill_dest=1", "fill_dest=32"),
        requested=True)
    assert bad["status"] == "error"
    assert bad["timing_valid_for_speedup"] == "0"

    discriminating_log = "\n".join([
        log,
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=3 epoch1=5 epoch2=9 current=2 context=4]",
        "[ECG-ReuseBind-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=3 "
        "epoch1=5 epoch2=9 current=2 context=4 "
        "property_elem_bytes=4]",
    ])
    discriminating = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_reuse_bind_receipt(
        discriminating, discriminating_log, requested=True,
        require_discriminating=True)
    assert discriminating["gem5_reuse_bind_payload_discriminating"] == 1
    assert discriminating["gem5_reuse_bind_request_metadata_values"] == 2
    assert discriminating["gem5_reuse_bind_accept_metadata_values"] == 2
    assert discriminating["gem5_reuse_bind_request_epoch_states"] == 2
    assert discriminating["gem5_reuse_bind_accept_epoch_states"] == 2
    assert discriminating["gem5_reuse_bind_request_plan_epochs"] == 2
    assert discriminating["gem5_reuse_bind_accept_plan_epochs"] == 2

    tier_only_log = "\n".join([
        log,
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=3 epoch1=3 epoch2=7 current=1 context=4]",
        "[ECG-ReuseBind-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=3 "
        "epoch1=3 epoch2=7 current=1 context=4 "
        "property_elem_bytes=4]",
    ])
    tier_only = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_reuse_bind_receipt(
        tier_only, tier_only_log, requested=True,
        require_discriminating=True)
    assert tier_only["gem5_reuse_bind_payload_discriminating"] == 0
    assert tier_only["gem5_reuse_bind_accept_epoch_states"] == 1

    current_only_log = "\n".join([
        log,
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=1 request_seq=18 "
        "dest=25 tier=2 epoch1=3 epoch2=7 current=2 context=4]",
        "[ECG-ReuseBind-ACCEPT sim=gem5 seq=1 request_seq=18 "
        "request_dest=25 fill_dest=16 source=request tier=2 "
        "epoch1=3 epoch2=7 current=2 context=4 "
        "property_elem_bytes=4]",
    ])
    current_only = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_gem5_reuse_bind_receipt(
        current_only, current_only_log, requested=True,
        require_discriminating=True)
    assert current_only["gem5_reuse_bind_payload_discriminating"] == 0
    assert current_only["gem5_reuse_bind_accept_epoch_states"] == 2
    assert current_only["gem5_reuse_bind_accept_plan_epochs"] == 1

    duplicate_accept = {"timing_valid_for_speedup": "1"}
    duplicate_log = discriminating_log + "\n" + discriminating_log.splitlines()[-1]
    assert not roi_matrix.apply_gem5_reuse_bind_receipt(
        duplicate_accept, duplicate_log, requested=True,
        require_discriminating=True)
    assert duplicate_accept["gem5_reuse_bind_duplicate_accepts"] == 1

    replay_log = "\n".join([
        log,
        "[ECG-ReuseBind-REQUEST sim=gem5 seq=1 request_seq=17 "
        "dest=9 tier=2 epoch1=3 epoch2=7 current=1 context=4]",
    ])
    replay = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_reuse_bind_receipt(
        replay, replay_log, requested=False, trace_limit=2)
    assert replay["gem5_reuse_bind_request_trace_events"] == 2
    assert replay["gem5_reuse_bind_request_receipts"] == 1
    assert replay["gem5_reuse_bind_duplicate_request_receipts"] == 1
    assert replay["gem5_reuse_bind_request_trace_max_seq"] == 1
    assert replay["gem5_reuse_bind_trace_saturated"] == 1


def test_proposal_run_gate_requires_every_requested_row():
    args = SimpleNamespace(
        gem5_compact_reuse_bind_flowthrough=True,
        dry_run=False,
        l3_sizes=["32kB"],
        ecg_isa_variant="computed",
        benchmark="pr",
    )
    policies = [
        roi_matrix.parse_policy_spec("ECG:REUSE_PLAN"),
        roi_matrix.parse_policy_spec("ECG:REUSE_PLAN_LRU_FLOWTHROUGH"),
        roi_matrix.parse_policy_spec("ECG:REUSE_PLAN_FLOWTHROUGH"),
    ]
    good = {
        "gem5_compact_reuse_bind_flowthrough_requested": 1,
        "policy_label": "ECG_REUSE_PLAN_FLOWTHROUGH",
        "status": "ok",
        "proposal_path_active": 1,
        "gem5_reuse_plan_exact_bind": 1,
        "gem5_reuse_bind_payload_discriminating": 1,
        "gem5_reuse_bind_coalesced_line_accepts": 1,
        "gem5_reuse_bind_nonzero_epoch_accepts": 8,
        "gem5_flowthrough_request_flag_size4_events": 2,
        "gem5_flowthrough_request_flag_bad_size_events": 0,
        "gem5_flowthrough_request_flag_events": 2,
        "gem5_flowthrough_all_events": 2,
        "gem5_flowthrough_range_events": 0,
        "gem5_flowthrough_trace_saturated": 0,
    }
    roi_matrix.validate_gem5_compact_reuse_bind_flowthrough_rows(
        [
            {
                **good, "policy_label": "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
                "l3_size": "32kB",
            },
            {**good, "l3_size": "32kB"},
            {"gem5_compact_reuse_bind_flowthrough_requested": 0, "status": "ok"},
        ],
        args, policies)

    with pytest.raises(SystemExit, match="proposal compact ReuseBind"):
        roi_matrix.validate_gem5_compact_reuse_bind_flowthrough_rows(
            [
                {
                    **good, "policy_label": "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
                    "l3_size": "32kB",
                },
                {
                    **good, "l3_size": "32kB",
                    "gem5_reuse_plan_exact_bind": 0,
                },
            ],
            args, policies)
    with pytest.raises(SystemExit, match="proposal compact ReuseBind"):
        roi_matrix.validate_gem5_compact_reuse_bind_flowthrough_rows(
            [
                {
                    **good, "policy_label": "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
                    "l3_size": "32kB",
                    "gem5_reuse_bind_coalesced_line_accepts": 0,
                },
                {**good, "l3_size": "32kB"},
            ],
            args, policies)
    with pytest.raises(SystemExit, match="observed=.*32kB"):
        roi_matrix.validate_gem5_compact_reuse_bind_flowthrough_rows(
            [{**good, "l3_size": "32kB"}], args, policies)
    with pytest.raises(SystemExit, match="proposal compact ReuseBind"):
        roi_matrix.validate_gem5_compact_reuse_bind_flowthrough_rows(
            [
                {
                    **good, "policy_label": "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
                    "l3_size": "32kB",
                    "gem5_flowthrough_trace_saturated": 1,
                },
                {**good, "l3_size": "32kB"},
            ],
            args, policies)


def test_real_decoder_probe_covers_compact_flowthrough_reuse_bind_request():
    probe = read("bench/src_gem5/test_ecg_load_modes.cc")
    verifier = read("scripts/experiments/ecg/verify/ecg.py")

    assert "gem5_ecg_flow_load_compact_instruction" in probe
    assert "gem5_ecg_bind_load_f32" in probe
    assert "ReuseBind-Compact-Flow" in probe
    assert "gem5_ecg_write_record_format_csr" in probe
    assert "g_context_retry_lines[2048 * 64]" in probe
    assert "kProposalValueBits = 0x41234567u" in probe
    assert "if (proposal_only || proposal_wrong_format)" in probe
    assert "proposal-wrong-format" in probe
    assert "wrong_record_format ? kProposalIdBits - 2" in probe
    assert '"current=%u context=%u value_bits=%#x [%s]\\n"' in probe

    assert '"--cpu-type", "O3"' in verifier
    assert '"GEM5_ECG_PRODUCER": "1"' in verifier
    assert '"GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND": "1"' in verifier
    assert 'ROOT / "bench" / "include" / "gem5_sim" / "configs"' in verifier
    assert '"PATH": "/usr/bin:/bin"' in verifier
    assert "expected_payload = (37, 3, 17, 29, 11, 7)" in verifier
    assert "compact_request_bound_pass" in verifier
    assert "compact_request_flowthrough_pass" in verifier
    assert "size=4" in verifier
    assert 'r"ReuseBind-Compact-Flow[^\\n]*\\[OK\\]"' in verifier
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
    assert "ecg_proposal_reuse_bind_o3_gate" in manifest["profiles"]
    stage = next(
        item for item in manifest["stages"]
        if item["name"] == "60_gem5_proposal_reuse_bind_o3")
    assert stage["suite"] == "gem5"
    assert stage["graph_set"] == "synthetic_kron12_all"
    assert stage["benchmarks"] == ["pr"]
    assert stage["policies"] == [
        "ECG:REUSE_PLAN", "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG:REUSE_PLAN_FLOWTHROUGH"]
    assert stage["ecg_isa_variant"] == "computed"
    assert stage["gem5_cpu_type"] == "O3"
    assert stage["gem5_compact_reuse_bind_flowthrough"] is True
    assert stage["ecg_epochs"] == 32
    assert "ECG_REUSE_PLAN_DELIVERY_TRACE" not in stage["env"]
    assert "ECG_FLOWTHROUGH_TRACE" not in stage["env"]
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    experiment_run = read("scripts/experiments/ecg/flows/experiment_run.py")
    assert 'env["ECG_REUSE_PLAN_DELIVERY_TRACE"] = "2048"' in runner
    assert 'env["ECG_FLOWTHROUGH_TRACE"] = "2048"' in runner
    assert 'env["ECG_REUSE_PLAN_DELIVERY_TRACE"] = "131072"' in runner
    assert 'env["ECG_FLOWTHROUGH_TRACE"] = "131072"' in runner
    assert "max(reuse_plan_trace, 2048)" not in runner
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
            "error": "proposal ReuseBind exact Request binding was not attested",
            "options": "-i 1", "l3_size": "32kB", "l3_ways": 8,
            "prefetcher": "none",
            "pr_iterations": 1, "pr_semantic_edges": 10,
            "pr_score_checksum": "abc",
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
    assert "PageRank semantic receipt mismatch" not in rows[0]["error"]
    assert all(row["pr_result_matched"] == 1 for row in rows)
    assert all(row["pr_result_group_rows_ok"] == 0 for row in rows)
    assert all(row["timing_valid_for_speedup"] == "0" for row in rows)
    assert all(
        "another policy row" in row["timing_caveat"] for row in rows)

    runner = read("scripts/experiments/ecg/roi_matrix.py")
    main_tail = runner.split(
        "certify_sniper_semantic_work(rows, args, policies)", 1)[1]
    assert main_tail.index("write_outputs(out_dir, rows)") < (
        main_tail.index(
            "validate_gem5_compact_reuse_bind_flowthrough_rows"))


def test_proposal_compact_reuse_bind_flowthrough_native_path_is_reachable(
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
        "ECG_REUSE_PLAN_DEPTH": "2",
        "ECG_EDGE_MASK_EPOCH": "1",
        "ECG_EDGE_MASK_LINEMIN": "1",
        "ECG_EDGE_MASK_EPOCHS": "32",
        "ECG_EDGE_MASK_PACK_BITS": "64",
        "GEM5_ENABLE_ECG_FLOW_LOAD": "1",
        "GEM5_ENABLE_ECG_PLOAD": "1",
        "GEM5_ECG_ISA_VARIANT": "computed",
        "GEM5_ECG_COMPACT_REUSE_BIND_FLOW": "1",
        "ECG_FLOWTHROUGH": "1",
        "ECG_RECORD_VARIABLE_WIDTH": "1",
        "ECG_EXPECT_BYTES_PER_EDGE": "4",
        "GEM5_GRAPHBREW_CTX": str(tmp_path / "compact-context.json"),
    }

    compact = subprocess.run(
        [
            str(binary), "-g", "8", "-k", "2", "-o", "0",
            "-n", "2", "-i", "1",
        ],
        cwd=PROJECT_ROOT, env=compact_env,
        capture_output=True, text=True,
        timeout=60)
    compact_text = compact.stdout + compact.stderr
    assert compact.returncode == 0, compact_text
    assert "[ECG_REUSE_BIND_LOAD_C_FLOW]" in compact_text
    assert (
        "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=1 valid=1 "
        "offset_source=csr direction=in"
    ) in compact_text
    compact_csr = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_csr_substitution_receipt(
        compact_csr, compact_text, "pr", required=True)
    assert compact_csr["ecg_csr_substitution_receipt_count"] == 1
    assert compact_csr["ecg_csr_substitution_rows"] == 256
    assert compact_csr["ecg_csr_substitution_records"] > 0
    compact_context = json.loads(
        (tmp_path / "compact-context.json").read_text())
    assert compact_context["array_attribution_schema"] == 2
    assert compact_context["edge_regions_aliased"] is True
    assert compact_context["edge_preferred_base"] == (
        compact_context["edge_other_base"])
    assert compact_context["csr_offsets_size"] == 257 * 8
    assert compact_context["csr_offsets_other_size"] == 257 * 8
    assert compact_context["plan_offsets_size"] == 257 * 8
    assert compact_context["csr_offsets_base"] != (
        compact_context["plan_offsets_base"])
    assert "[ECG-METADATA-FATAL]" not in compact_text

    wide_env = dict(compact_env)
    wide_env.pop("GEM5_ECG_COMPACT_REUSE_BIND_FLOW")
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
        "[ECG_REUSE_BIND_LOAD] PR computed-address computed-address load "
        "+ FlowThrough record load ACTIVE"
    ) in wide_text
    assert "[ECG_REUSE_BIND_LOAD_C_FLOW]" not in wide_text
    assert (
        "[ECG-CSR-SUBSTITUTION sim=gem5 kernel=pr active=1 valid=1 "
        "offset_source=csr direction=in"
    ) in wide_text
    wide_csr = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_gem5_csr_substitution_receipt(
        wide_csr, wide_text, "pr", required=True)
    assert wide_csr["ecg_csr_substitution_receipt_count"] == 1
    assert wide_csr["ecg_csr_substitution_rows"] == 256
    assert wide_csr["ecg_csr_substitution_records"] > 0
    wide_context = json.loads((tmp_path / "wide-context.json").read_text())
    assert wide_context["csr_offsets_size"] == 257 * 8
    assert wide_context["csr_offsets_other_size"] == 257 * 8
    assert wide_context["plan_offsets_size"] == 257 * 8
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

    missing = [
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
        },
    ]
    roi_matrix.certify_gem5_pr_results(missing, args)
    assert all(row["status"] == "error" for row in missing)
    assert all(row["pr_result_matched"] == 0 for row in missing)


def test_detailed_kernel_semantic_receipts_fail_closed():
    gem5_args = SimpleNamespace(benchmark="bfs", suite="gem5")
    gem5_rows = [
        {
            "simulator": "gem5", "benchmark": "bfs", "status": "ok",
            "options": "-i 1", "l3_size": "128kB", "l3_ways": 16,
            "prefetcher": "none", "kernel_semantic_name": "bfs",
            "kernel_semantic_items": 100, "kernel_semantic_checksum": "abc",
        },
        {
            "simulator": "gem5", "benchmark": "bfs", "status": "ok",
            "options": "-i 1", "l3_size": "128kB", "l3_ways": 16,
            "prefetcher": "none", "kernel_semantic_name": "bfs",
            "kernel_semantic_items": 100, "kernel_semantic_checksum": "abc",
        },
    ]
    roi_matrix.certify_detailed_kernel_results(gem5_rows, gem5_args)
    assert all(row["kernel_result_matched"] == 1 for row in gem5_rows)

    gem5_rows[1]["kernel_semantic_checksum"] = "def"
    roi_matrix.certify_detailed_kernel_results(gem5_rows, gem5_args)
    assert all(row["status"] == "error" for row in gem5_rows)

    sniper_args = SimpleNamespace(benchmark="cc", suite="sniper")
    sniper_rows = [
        {
            "simulator": "sniper", "benchmark": "cc", "status": "ok",
            "options": "", "l3_size": "128kB", "l3_ways": 16,
            "prefetcher": "none", "sniper_workload": "sg_kernel",
            "sniper_semantic_result": "7",
        },
        {
            "simulator": "sniper", "benchmark": "cc", "status": "ok",
            "options": "", "l3_size": "128kB", "l3_ways": 16,
            "prefetcher": "none", "sniper_workload": "sg_kernel",
            "sniper_semantic_result": "",
        },
    ]
    roi_matrix.certify_detailed_kernel_results(sniper_rows, sniper_args)
    assert all(row["status"] == "error" for row in sniper_rows)
    assert all(row["kernel_result_matched"] == 0 for row in sniper_rows)


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