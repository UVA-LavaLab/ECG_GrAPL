import importlib.util
from pathlib import Path
import shutil
import subprocess

from scripts.experiments.ecg import roi_matrix


ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text()


def test_context_handler_normalizes_fresh_indent(tmp_path) -> None:
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_context_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = tmp_path / "magic_server.cc"
    target.write_text(
        "        if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)\n"
        "        {\n"
        "           return 0;\n"
        "        }\n"
    )
    module.normalize_context_ready_handler(target, False)
    module.normalize_context_ready_handler(target, False)
    text = target.read_text()
    assert text.count("GRAPHBREW_CONTEXT_READY_WORK_ID") == 1
    assert "        if (arg0 == graphbrew::sniper::GRAPHBREW_CONTEXT_READY_WORK_ID)" in text


def test_reuse_plan_bind_handler_upgrades_existing_user_case(tmp_path) -> None:
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_reuse_plan_bind_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = tmp_path / "magic_server.cc"
    target.write_text(
        "      case SIM_CMD_MARKER:\n"
        "      {\n"
        "         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };\n"
        "         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, (UInt64)&args, true /* expect return value */);\n"
        "      }\n"
        "      case SIM_CMD_USER:\n"
        "      {\n"
        "         if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT2_WORK_ID) return 0;\n"
        "         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };\n"
        "         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);\n"
        "      }\n"
    )
    module.ensure_reuse_plan_bind_magic_handler(target, False)
    module.ensure_reuse_plan_bind_magic_handler(target, False)
    text = target.read_text()
    marker_case, user_case = text.split("case SIM_CMD_USER:", 1)
    assert "GRAPHBREW_REUSE_PLAN_BIND_WORK_ID" not in marker_case
    assert user_case.count("GRAPHBREW_REUSE_PLAN_BIND_WORK_ID") == 1
    assert user_case.count("GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID") == 1

    partial = tmp_path / "magic_server_partial.cc"
    partial.write_text(
        "      case SIM_CMD_USER:\n"
        "      {\n"
        "         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_BIND_WORK_ID) return 0;\n"
        "         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };\n"
        "         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);\n"
        "      }\n"
    )
    module.ensure_reuse_plan_bind_magic_handler(partial, False)
    partial_text = partial.read_text()
    assert partial_text.count("GRAPHBREW_REUSE_PLAN_BIND_WORK_ID") == 1
    assert partial_text.count("GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID") == 1


def test_sniper_context_lifecycle_hooks_are_idempotent(tmp_path) -> None:
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_context_lifecycle_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = tmp_path / "magic_server.cc"
    target.write_text(
        "      case SIM_CMD_ROI_START:\n"
        "         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);\n"
        "         return 0;\n"
        "      case SIM_CMD_ROI_END:\n"
        "         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);\n"
        "         return 0;\n"
    )
    module.ensure_ecg_context_lifecycle_hooks(target, False)
    module.ensure_ecg_context_lifecycle_hooks(target, False)
    text = target.read_text()
    assert text.count("beginEcgContext();") == 1
    assert text.count("endEcgContext();") == 1


def test_sniper_harness_defines_ecg_pfx_hint_surface() -> None:
    text = read("bench/include/sniper_sim/sniper_harness.h")
    assert "GRAPHBREW_SNIPER_USER_ECG_PFX_TARGET" in text
    assert "SNIPER_ENABLE_ECG_PFX_HINTS" in text
    assert "SNIPER_ECG_PFX_HINT_FILTER" in text
    assert "SNIPER_ECG_PFX_FILTER_ELEM_SIZE" in text
    assert "SNIPER_ECG_PFX_FILTER_LINE_SIZE" in text
    assert "should_emit_ecg_pfx_hint" in text
    assert "SNIPER_ECG_PFX_TARGET" in text


def test_sniper_harness_caches_hot_path_environment_controls() -> None:
    text = read("bench/include/sniper_sim/sniper_harness.h")
    for function_name in (
        "hints_enabled",
        "ecg_pfx_hints_enabled",
        "ecg_extract_enabled",
    ):
        body = text.split(f"inline bool {function_name}()", 1)[1].split(
            "\n}", 1)[0]
        assert "static const bool enabled" in body
    pfx_filter = text.split(
        "inline bool should_emit_ecg_pfx_hint", 1)[1].split("\n}", 1)[0]
    assert "static const int capacity" in pfx_filter
    assert "static const uint64_t vertices_per_line" in pfx_filter


def test_sniper_fused_reuse_plan_skips_software_only_delivery() -> None:
    text = read("bench/src_sniper/sg_kernel.cc")
    assert text.count("const bool software_reuse_plan_delivery =") == 5
    assert text.count("const bool ecg_pfx_hints_on =") == 3
    assert text.count("const bool no_delivery_pair_loop =") == 4
    assert text.count("if (no_delivery_pair_loop)") == 5
    assert text.count("if (software_reuse_plan_delivery) {") == 3
    assert text.count("if (!fused_reuse_plan_model) {") >= 6
    assert "if (delivered_reuse_plan && !fused_reuse_plan_model)" in text
    assert "!graphbrew_sniper::ecg_pfx_hints_enabled()" not in text


def test_sniper_ecg_pfx_prefetcher_overlay_exists() -> None:
    header = read("bench/include/sniper_sim/overlays/common/core/memory_subsystem/parametric_dram_directory_msi/ecg_pfx_prefetcher.h")
    source = read("bench/include/sniper_sim/overlays/common/core/memory_subsystem/parametric_dram_directory_msi/ecg_pfx_prefetcher.cc")
    assert "class EcgPfxPrefetcher" in header
    assert "consumePrefetchTargetHint" in source
    assert "ecg-pfx-prefetcher" in source
    assert "target-hints-seen" in source


def test_sniper_context_tracks_prefetch_target_hint() -> None:
    header = read("bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.h")
    source = read("bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.cc")
    assert "GRAPHBREW_ECG_PFX_TARGET_WORK_ID" in header
    for symbol in (
        "setPrefetchTargetHint",
        "hasPrefetchTargetHint",
        "getPrefetchTargetHint",
        "consumePrefetchTargetHint",
        "clearPrefetchTargetHint",
    ):
        assert symbol in header
        assert symbol in source


def test_sniper_benchmarks_emit_ecg_pfx_targets() -> None:
    for relative_path in ("bench/src_sniper/pr.cc", "bench/src_sniper/bfs.cc", "bench/src_sniper/sssp.cc"):
        text = read(relative_path)
        assert "SNIPER_ECG_PFX_TARGET" in text
        assert "SNIPER_ECG_PFX_LOOKAHEAD" in text


def test_sniper_runner_wires_ecg_pfx_prefetcher() -> None:
    text = read("scripts/experiments/ecg/roi_matrix.py")
    assert 'if args.prefetcher == "ECG_PFX":' in text
    assert '"Sniper ECG_PFX requires overlays' in text
    assert 'prefetcher"] = "ecg_pfx"' in text
    assert 'SNIPER_ENABLE_ECG_PFX_HINTS' in text
    assert 'SNIPER_ECG_PFX_HINT_FILTER' in text
    assert 'SNIPER_ECG_PFX_FILTER_ELEM_SIZE' in text
    assert 'SNIPER_ECG_PFX_FILTER_LINE_SIZE' in text
    assert 'ecg_pfx_target_hints_seen' in text
    assert 'ecg_pfx_activity' in text


def test_setup_sniper_patches_simuser_hint_dispatch() -> None:
    text = read("scripts/setup_sniper.py")
    assert "patch_graphbrew_simuser_overlay" in text
    assert "patch_ecg_pfx_prefetcher_overlay" in text
    assert "ecg_pfx_prefetcher.h" in text
    assert "EcgPfxPrefetcher" in text
    assert "core/memory_subsystem/cache/graph_cache_context_sniper.h" in text
    assert "GRAPHBREW_SET_VERTEX_WORK_ID" in text
    assert "GRAPHBREW_ECG_PFX_TARGET_WORK_ID" in text
    assert "setCurrentVertexHint" in text
    assert "setPrefetchTargetHint" in text


def test_sniper_ecg_extract_payload_and_runner_are_faithful() -> None:
    harness = read("bench/include/sniper_sim/sniper_harness.h")
    setup = read("scripts/setup_sniper.py")
    context_h = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.h"
    )
    context_cc = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc"
    )
    cache = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "cache_set_ecg.cc"
    )
    runner = read("scripts/experiments/ecg/roi_matrix.py")

    # Keep NodeID[31:0]+epoch[15:0] inside the magic ABI's reliable low 48 bits.
    assert "(vertex & 0xFFFFFFFFULL)" in harness
    assert "static_cast<uint64_t>(epoch) << 32" in harness
    assert "arg1 & 0xFFFFFFFFULL" in setup
    assert "(arg1 >> 32) & 0xFFFFULL" in setup
    assert "epoch) << 48" not in harness
    assert "(arg1 >> 32) & 0x3ULL" in setup
    assert "(arg1 >> 34) & 0x7FFFULL" in setup
    assert "(arg1 >> 49) & 0x7FFFULL" in setup

    # SimMagic inputs must not alias the RAX output and get overwritten by cmd=5.
    assert "early-clobber: inputs cannot alias RAX" in setup
    assert "replace(old_constraint, new_constraint, 3)" in setup
    assert "old_decode" in setup
    assert "new_decode" in setup

    # Shared LLC consumes only the requesting core's newest stable delivery.
    assert "lookupEcgEpochAnyCore" not in context_h
    assert "ecgEpochGlobalSequence" in context_cc
    assert "before != after" in context_cc
    assert "lookupLineEcgReusePlan(" in cache
    assert "lookupEcgReusePlan(" in context_cc
    assert "recordEcgReusePlan(" in context_cc
    assert "if (tier == 0)" not in context_cc
    assert "m.count[i].store(2" in context_cc
    assert "GRAPHBREW_CONTEXT_READY_WORK_ID" in context_h
    assert "GRAPHBREW_CONTEXT_READY_WORK_ID" in setup
    assert "ECG-CONTEXT-READY sim=sniper" in setup
    assert "SNIPER_REQUIRE_POPT_MATRIX" in setup
    assert "reref=%d" in setup
    assert "normalize_context_ready_handler" in setup
    assert "text.count(marker) != 1" in setup
    for source_name in (
        "pr_kernel_smoke.cc", "bfs_kernel_smoke.cc",
        "sssp_kernel_smoke.cc",
    ):
        assert "notify_context_ready()" in read(
            f"bench/src_sniper/{source_name}")
    assert "bool epoch_property[64]" in cache
    assert "hasCurrentVertexHint(" in cache
    assert "m_property_lines[accessed_index] = context.isPropertyData" in cache
    assert "m_set_info->increment(accessed_index)" in cache
    assert "hasCurrentVertexHint(requester_core)" in cache
    assert "Sniper graph policy completed without a loaded graph context" in runner
    assert "[ECG-CONTEXT-READY sim=sniper loaded=1" in runner
    assert "m_property_lines[way] =" not in cache.split(
        "CacheSetECG::findECGGraspPoptVictim", 1)[1].split(
            "CacheSetECG::getReplacementIndex", 1)[0]
    assert "line_plus1" in context_cc
    assert "vertex_plus1" not in context_cc
    assert "ecgVerticesPerLine()" in context_cc
    assert "isEcgEpochData" in context_h
    assert "SNIPER_ECG_EPOCH_REGION" in context_cc
    assert "GRAPHBREW_ECG_EXTRACT2_WORK_ID" in setup
    assert "reusePlanDistance(" in cache
    assert "currentNucaRequesterCore()" in cache
    assert "address, requester, data_buf" in setup
    assert "NucaCache::read(IntPtr address, core_id_t requester" in setup
    assert "NucaCache::write(IntPtr address, core_id_t requester" in setup

    # Controlled runs use the real outer clock and delivered epoch, not the live oracle.
    assert 'env["SNIPER_ENABLE_VERTEX_HINTS"] = "1"' in runner
    assert 'env["SNIPER_ENABLE_ECG_EXTRACT"] = "1"' in runner
    assert 'int(args.line_size) // 4' in runner
    assert "requires --sniper-workload sg_kernel" in runner
    assert 'os.environ.get("ECG_FORCE_DELIVERY") == "1"' in runner
    assert "ws[w].recency = m_last_touch[w];" in cache


def test_sniper_computed_address_uses_transport_matched_loops():
    source = read("bench/src_sniper/sg_kernel.cc")
    harness = read("bench/include/sniper_sim/sniper_harness.h")
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    setup = read("scripts/setup_sniper.py")
    context_h = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.h")
    context_cc = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc")
    cache = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "cache_set_ecg.cc")

    assert "bool reuse_plan_transport_matched_enabled()" in source
    assert source.count(
        "const bool reuse_plan_transport_matched = "
        "reuse_plan_transport_matched_enabled();") == 5
    assert source.count("[REUSE_PLAN_TRANSPORT_MATCHED]") == 6
    assert "reuse_plan_transport_matched && !reuse_plan_trace_on" in source
    assert "require_canonical_reuse_plan_offsets" in source
    assert source.count("pair_off[") == 1  # pre-ROI weighted-record packing
    assert "reuse_plan_off[" not in source
    assert "bfs_pairs.offsets[" not in source
    assert "pairs.offsets[" not in source
    assert "const uint64_t begin = epoch_packed_off" not in source
    assert "const uint64_t begin = bfs_packed_off" not in source
    pr_window = source.split("int run_pr(", 1)[1].split("int run_bfs(", 1)[0]
    bfs_window = source.split("int run_bfs(", 1)[1].split("int run_sssp(", 1)[0]
    sssp_window = source.split("int run_sssp(", 1)[1].split("int run_bc(", 1)[0]
    bc_window = source.split("int run_bc(", 1)[1].split("int run_cc(", 1)[0]
    cc_window = source.split("int run_cc(", 1)[1]
    assert "graph.in_offset(node)" in pr_window
    assert "graph.out_offset(node)" not in pr_window
    for window in (bfs_window, sssp_window, bc_window, cc_window):
        assert "graph.out_offset(" in window
        assert "graph.in_offset(" not in window
    for path in (
            "bench/src_sniper/bc.cc",
            "bench/src_sniper/cc.cc",
            "bench/src_sniper/sssp.cc"):
        standalone = read(path)
        assert "require_canonical_reuse_plan_offsets" in standalone
        assert "pair_off[" not in standalone
    assert 'env["SNIPER_REUSE_PLAN_TRANSPORT_MATCHED"] = "1"' in runner
    assert 'env["SNIPER_ENABLE_ECG_EXTRACT"] = "1"' in runner
    assert "transport_record_bytes = explicit_ecg_record_bytes(8)" in runner
    assert '"matched_computed_address_sideband_model"' in runner
    assert 'env["SNIPER_REUSE_PLAN_EXACT_BIND"] = "1"' in runner
    assert '"sniper_reuse_bind_exact"] = 1' in runner
    assert '"sniper_reuse_plan_epoch_context_bound"] = 1' in runner
    assert "GRAPHBREW_SNIPER_USER_REUSE_PLAN_BIND" in harness
    assert "GRAPHBREW_SNIPER_USER_REUSE_PLAN_CERTIFIED" in harness
    assert "SNIPER_REUSE_PLAN_BIND_PREFIX_LOADS" in harness
    assert "reuse_plan_bound_load" in harness
    assert source.count("[REUSE_PLAN_EXACT_BIND]") == 5
    assert "const WeightT source_dist = dist[node];" in source
    assert "const int64_t source_paths = path_counts[u];" in source
    assert "edge-governed dist[dest]" in source
    assert "edge-governed depth/path_counts[dest]" in source
    assert "recordBoundReusePlanLoad" in context_h
    assert "finishBoundReusePlanCertification" in context_h
    assert "recordCertifiedReusePlanFallback" in context_h
    assert "consumeBoundReusePlanLoad" in context_cc
    assert "GRAPHBREW_REUSE_PLAN_BIND_WORK_ID" in setup
    assert "GRAPHBREW_REUSE_PLAN_CERTIFIED_WORK_ID" in setup
    user_case = setup.split('"""      case SIM_CMD_USER:', 2)[2].split(
        '"""', 1)[0]
    assert "GRAPHBREW_REUSE_PLAN_BIND_WORK_ID" in user_case
    assert "recordBoundReusePlanLoad" in user_case
    assert "def ensure_reuse_plan_bind_magic_handler" in setup
    assert "HOOK_MAGIC_USER" in setup
    assert "sniperReusePlanExactBindEnabled" in cache
    assert "m_pending_exact_reuse_plan_valid" in cache
    assert "m_pending_request_current_epoch" in cache
    assert "boundReusePlanCertificationFinished" in cache
    assert "std::numeric_limits<uint64_t>::max() - 1" in cache
    assert "m_ecg_context_id" in cache
    assert "beginEcgContext" in context_cc
    assert "currentEcgEpoch" in context_cc
    assert "ensure_ecg_context_lifecycle_hooks" in setup
    assert runner.count('env["SNIPER_REQUIRE_POPT_MATRIX"] = "1"') == 1
    assert '"sniper_popt_matrix_required"] = int(requires_popt_matrix)' in runner
    assert "Matrix-free ReuseBind row unexpectedly loaded" in runner
    assert 'env["SNIPER_REUSE_PLAN_BIND_PREFIX_LOADS"]' in runner
    assert '"sniper_reuse_bind_prefix_loads"' in runner
    assert '"sniper_reuse_bind_certified_prefixes"' in runner
    assert '"sniper_reuse_bind_certified_fallbacks"' in runner
    assert "certified ReuseBind prefix did not transition" in runner


def test_sniper_csr_substitution_receipt_is_fail_closed():
    receipt = (
        "[ECG-CSR-SUBSTITUTION sim=sniper kernel=pr active=1 valid=1 "
        "offset_source=csr direction=in rows=256 records=4096]")
    row = {"timing_valid_for_speedup": "1"}
    assert roi_matrix.apply_sniper_csr_substitution_receipt(
        row, receipt, "pr", required=True)
    assert row["sniper_csr_substitution_receipt_count"] == 1
    assert row["sniper_csr_substitution_active"] == 1
    assert row["sniper_csr_substitution_direction"] == "in"
    assert row["sniper_csr_substitution_records"] == 4096
    assert "error" not in row

    missing = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        missing, "", "pr", required=True)
    assert missing["status"] == "error"

    wrong_direction = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        wrong_direction,
        receipt.replace("direction=in", "direction=out"),
        "pr", required=True)
    assert wrong_direction["status"] == "error"

    duplicate = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        duplicate, f"{receipt}\n{receipt}", "pr", required=True)
    assert duplicate["status"] == "error"

    invalid = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        invalid, receipt.replace("valid=1", "valid=0"),
        "pr", required=True)
    assert invalid["status"] == "error"

    empty = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        empty, receipt.replace("records=4096", "records=0"),
        "pr", required=True)
    assert empty["status"] == "error"

    optional = {"timing_valid_for_speedup": "1"}
    assert not roi_matrix.apply_sniper_csr_substitution_receipt(
        optional, "", "pr", required=False)
    assert "error" not in optional


def test_sniper_epoch_cache_and_bind_prefix_are_exact(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        return
    fake_include = tmp_path / "include"
    fake_include.mkdir()
    (fake_include / "sim_api.h").write_text(
        """
#pragma once
#include <cstdint>
extern uint64_t command_counts[4];
inline uint64_t SimUser(uint64_t command, uint64_t) {
    if (command == 0x4B32424EULL) ++command_counts[0];
    else if (command == 0x4B324243ULL) ++command_counts[1];
    else if (command == 0x4B324244ULL) ++command_counts[2];
    else ++command_counts[3];
    return 0;
}
inline void SimRoiStart() {}
inline void SimRoiEnd() {}
""")
    source = tmp_path / "sniper_prefix.cc"
    binary = tmp_path / "sniper_prefix"
    source.write_text(
        r'''
#include <cstdint>
#include <cstdlib>
#include "bench/include/sniper_sim/sniper_harness.h"
#include "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/graph_cache_context_sniper.h"

uint64_t command_counts[4] = {};

int main() {
    if (graphbrew::sniper::quantizeEcgEpoch(0, 10, 3) != 0) return 1;
    if (graphbrew::sniper::quantizeEcgEpoch(3, 10, 3) != 0) return 2;
    if (graphbrew::sniper::quantizeEcgEpoch(4, 10, 3) != 1) return 3;
    if (graphbrew::sniper::quantizeEcgEpoch(7, 10, 3) != 2) return 4;
    setenv("SNIPER_REUSE_PLAN_EXACT_BIND", "1", 1);
    setenv("SNIPER_REUSE_PLAN_BIND_PREFIX_LOADS", "3", 1);
    int value = 9;
    for (int i = 0; i < 5; ++i)
        if (graphbrew_sniper::reuse_plan_bound_load(&value) != 9) return 5;
    if (command_counts[0] != 3) return 6;
    if (command_counts[1] != 3) return 7;
    if (command_counts[2] != 1) return 8;
    if (command_counts[3] != 0) return 9;
    graphbrew_sniper::roi_begin();
    for (int i = 0; i < 4; ++i)
        (void)graphbrew_sniper::reuse_plan_bound_load(&value);
    if (command_counts[0] != 6) return 10;
    if (command_counts[1] != 6) return 11;
    if (command_counts[2] != 2) return 12;
    return 0;
}
''')
    subprocess.run([
        compiler, "-std=c++17", "-O2",
        f"-I{fake_include}",
        f"-I{ROOT}",
        f"-I{ROOT / 'bench/include'}",
        f"-I{ROOT / 'bench/include/external/gapbs'}",
        str(source), "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)


def test_sniper_epoch_lookup_is_cached_per_core():
    context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc")
    header = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.h")
    assert "epochStorage()[core_id].store(" in context
    assert "epochValidStorage()[core_id].store(true" in context
    assert "epochValidStorage()[core_id].load(" in context
    assert "fallbackVertexStorage()[core_id]" in context
    assert "vertexValidStorage()[core_id].store(" in context.split(
        "void beginEcgContext()", 1)[1]
    assert "epochValidStorage()[core_id].store(" in context.split(
        "void beginEcgContext()", 1)[1]
    assert "current_dst_vertex" not in header
    assert "current_outer_vertex" not in header


def test_sniper_ecg_host_profile_covers_cache_callbacks():
    cache = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "cache_set_ecg.cc")
    context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc")

    assert 'std::getenv("SNIPER_ECG_HOST_PROFILE")' in cache
    assert "[ECG-HOST-PROFILE" in cache
    assert "(void)ecgHostProfile();" in cache
    assert "Kind::Replacement" in cache
    assert "Kind::Update" in cache
    assert "Kind::Prepare" in cache
    assert 'std::getenv("SNIPER_REUSE_PLAN_LOOKUP_PROFILE")' in context
    assert "[ReusePlan-LOOKUP-PROFILE" in context
    assert "reuse_plan_profile_classify_ns" in context
    assert "reuse_plan_profile_search_ns" in context