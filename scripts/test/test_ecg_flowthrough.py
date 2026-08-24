from pathlib import Path
import argparse
import importlib.util
import json
import os
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_flowthrough_is_explicit_and_default_off():
    # FlowThrough is now read once by the shared metadata helper rather than by each
    # kernel, so the invariant is checked there: explicit env, default off.
    meta = read("bench/include/ecg_metadata.h")
    assert 'envInt("ECG_FLOWTHROUGH", 0, 0, 1)' in meta
    assert "bool flowthrough = false;" in meta
    # The single cache_sim delivery site must honour it.
    graph_sim = read("bench/include/cache_sim/graph_sim.h")
    assert "(cfg).flowthrough" in graph_sim
    assert "access_stream_with_site" in graph_sim
    pr = read("bench/src_sim/pr.cc")
    assert "SIM_ECG_EDGE" in pr


def test_flowthrough_preserves_llc_hits_and_suppresses_miss_fill():
    cache = read("bench/include/cache_sim/cache_sim.h")
    block = cache.split("void accessStream(", 1)[1].split(
        "// FlowThrough prefetch", 1
    )[0]
    assert "if (l3_->access" in block
    assert "if (!flowthrough) l3_->insert" in block
    assert "ECG_FLOWTHROUGH_ADAPTIVE" in cache
    assert "l2_->insert" in block
    assert "l1_->insert" in block


def test_flowthrough_prefetch_avoids_false_memory_fills():
    cache = read("bench/include/cache_sim/cache_sim.h")
    block = cache.split("void prefetchStream(", 1)[1].split(
        "// Prefetch:", 1
    )[0]
    assert "l3_->contains" in block
    assert "prefetch_fills_++" in block


def test_gem5_flowthrough_suppresses_only_l3_allocation():
    patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "base_flowthrough.patch"
    )
    flag_patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "base_flowthrough_request_flag.patch"
    )
    attribution_patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "array_attribution.patch"
    )
    context = read(
        "bench/include/gem5_sim/overlays/mem/cache/replacement_policies/"
        "graph_cache_context_gem5.hh"
    )
    request_patch = read(
        "bench/include/gem5_sim/overlays/mem/request_flowthrough.patch"
    )
    prefetch_patch = read(
        "bench/include/gem5_sim/overlays/mem/cache/"
        "prefetch_flowthrough.patch"
    )
    decoder = read(
        "bench/include/gem5_sim/overlays/arch/riscv/isa/"
        "decoder_ecg_extract.isa"
    )
    harness = read("bench/include/gem5_sim/gem5_harness.h")
    assert 'find("l3cache")' in patch
    assert "getVaddr()" in patch
    assert "Request::ECG_FLOWTHROUGH" in flag_patch
    assert "GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND" in flag_patch
    assert "flag_flowthrough && (!request_bound_only || stream_range_match)" in flag_patch
    assert "rejecting squashed or stale" in flag_patch
    assert "size=%u source=%s allocate=0" in flag_patch
    assert "pkt->req->getSize()" in flag_patch
    assert "allocOnFill(pkt->cmd) && !flowthrough" in patch
    assert "allow_alloc_on_fill" in patch
    assert "recordGraphArrayDemandMiss(pkt);" in attribution_patch
    assert "recordGraphArrayMshrMiss(pkt);" in attribution_patch
    assert "pkt->req->hasVaddr()" in attribution_patch
    assert "classifyEcgArray(" in attribution_patch
    assert "pkt->getAddr()" not in attribution_patch
    assert "ecgArrayDemandReadBytes" in attribution_patch
    assert "graphArrayStatsActive" in attribution_patch
    assert 'p.name.find("l3cache")' in attribution_patch
    assert "isEcgFlowThroughAddress" in context
    assert "flowthrough_base" in context
    assert "arrayAttributionGraphContext" in context
    assert "static GraphCacheContext context;" in context
    assert "ECG_FLOWTHROUGH_ADAPTIVE" in flag_patch
    assert "globalOnlinePlacementSelector" in flag_patch
    assert "ECG_FLOWTHROUGH" in request_patch
    assert "isFlowThrough" in prefetch_patch
    assert "req->setFlags(Request::ECG_FLOWTHROUGH)" in prefetch_patch
    assert "ecg_flow_load" in decoder
    assert "ecg_flow_load_compact" in decoder
    assert "ecg_plan_load" in decoder
    assert "mem_flags=[ECG_FLOWTHROUGH]" in decoder
    assert ".insn i 0x0b, 0x3" in harness
    assert ".insn i 0x0b, 0x7" in harness
    assert ".insn i 0x0b, 0x4" in harness


def test_sniper_flowthrough_preserves_nuca_lookup_and_skips_miss_fill():
    setup = read("scripts/setup_sniper.py")
    context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc"
    )
    assert "m_flowthrough_reads" in setup
    assert "m_flowthrough_writes" in setup
    assert "latency, HitWhere::MISS" in setup
    assert "if (flowthrough) ++m_flowthrough_reads;" in setup
    assert "eviction = false" in setup
    assert "isEcgFlowThroughAddress" in context
    assert "recordEcgPlacementMiss" in context
    assert "recordEcgPlacementMiss" in setup
    assert "lookupFusedReusePlanPair" in context
    assert "reuse_plan_offsets_path" in context
    assert "reuse_plan_line_offsets" in context
    assert "std::lower_bound" in context
    assert "Sniper fused ReusePlan sideband is missing or incomplete" in context
    assert "Sniper fused ReusePlan line has inconsistent" in context
    assert '((record >> 32) & 0x3ULL) == 0' in context
    harness = read("bench/include/sniper_sim/sniper_harness.h")
    assert "sniper_write_binary_atomic" in harness
    assert '"SNIPER_CACHE_LINE_SIZE"' in harness
    assert '"SNIPER_PROPERTY_ALIGNMENT", 4096' in harness
    assert "property_alignment()" in harness
    assert "const uint64_t aligned_base = flowthrough_base + base_padding" in harness
    assert "const uint64_t aligned_upper = raw_upper - (raw_upper % line_size)" in harness
    assert "std::remove(reuse_plan_offsets_path.c_str())" in harness
    sniper = read("bench/src_sniper/sg_kernel.cc")
    assert "context/ReusePlan sideband export failed" in sniper
    assert "const bool flowthrough_on" in sniper
    assert "flowthrough_on" in sniper.split(
        "sniper_export_context(", 1)[1].split("))", 1)[0]


def test_sniper_governed_properties_use_page_alignment():
    expected = {
        "bench/src_sniper/sg_kernel.cc": [
            "pvector<ScoreT> scores(graph.num_nodes(), init_score, kPropAlign)",
            "pvector<ScoreT> contrib(graph.num_nodes(), 0.0f, kPropAlign)",
            "pvector<NodeID> parent(graph.num_nodes(), -1, kPropAlign)",
            "pvector<WeightT> dist(graph.num_nodes(), kDistInf, kPropAlign)",
            "pvector<ScoreT> scores(graph.num_nodes(), ScoreT(0), kPropAlign)",
            "pvector<int32_t> depth(graph.num_nodes(), int32_t(-1), kPropAlign)",
            "pvector<int64_t> path_counts(",
            "pvector<ScoreT> deltas(graph.num_nodes(), ScoreT(0), kPropAlign)",
            "pvector<NodeID> comp(graph.num_nodes(), NodeID(0), kPropAlign)",
        ],
        "bench/src_sniper/pr.cc": [
            "pvector<ScoreT> scores(g.num_nodes(), init_score, kPropAlign)",
            "pvector<ScoreT> outgoing_contrib(",
        ],
        "bench/src_sniper/bfs.cc": ["graphbrew_sniper::property_alignment()"],
        "bench/src_sniper/sssp.cc": ["graphbrew_sniper::property_alignment()"],
        "bench/src_sniper/bc.cc": ["graphbrew_sniper::property_alignment()"],
        "bench/src_sniper/cc.cc": ["graphbrew_sniper::property_alignment()"],
        "bench/src_sniper/cc_sv.cc": ["graphbrew_sniper::property_alignment()"],
        "bench/src_sniper/pr_kernel_smoke.cc": [
            "alignas(4096) ScoreT scores",
            "alignas(4096) ScoreT contrib",
        ],
        "bench/src_sniper/bfs_kernel_smoke.cc": ["alignas(4096) int parent"],
        "bench/src_sniper/sssp_kernel_smoke.cc": ["alignas(4096) int dist"],
    }
    for path, needles in expected.items():
        source = read(path)
        for needle in needles:
            assert needle in source, (
                f"{path} is missing aligned property allocation: {needle}")


def test_sniper_exported_property_bases_are_page_aligned(tmp_path):
    binary = ROOT / "bench/bin_sniper/sg_kernel"
    graph = ROOT / "results/graphs/email-Eu-core/email-Eu-core.sg"
    if not binary.exists() or not graph.exists():
        pytest.skip("built Sniper sg_kernel and email-Eu-core graph are required")

    for benchmark in ("pr", "bfs", "sssp", "bc", "cc"):
        context = tmp_path / f"{benchmark}.json"
        env = os.environ.copy()
        env.update({
            "OMP_NUM_THREADS": "1",
            "SNIPER_GRAPHBREW_CTX": str(context),
            "SNIPER_POPT_MATRIX": str(tmp_path / f"{benchmark}.matrix"),
            "SNIPER_GRAPHBREW_IN_EDGES": str(tmp_path / f"{benchmark}.in"),
            "SNIPER_GRAPHBREW_OUT_EDGES": str(tmp_path / f"{benchmark}.out"),
        })
        command = [
            str(binary), "-f", str(graph), "-o", "0", "-n", "1",
            "--benchmark", benchmark,
        ]
        if benchmark in ("pr", "bc"):
            command += ["-i", "1"]
        elif benchmark == "sssp":
            command += ["-d", "1"]
        subprocess.run(
            command, cwd=ROOT, env=env, check=True, timeout=60,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        payload = json.loads(context.read_text())
        regions = payload["property_regions"]
        assert regions
        assert all(int(region["base"]) % 4096 == 0 for region in regions)


def test_reuse_plan_result_rows_report_effective_epoch_count():
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_epochs", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    assert module.effective_ecg_epoch_count(65535, 0) == 65535
    assert module.effective_ecg_epoch_count(65535, 2) == 32768
    assert module.effective_ecg_epoch_count(1, 2) == 2
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    assert '"ecg_epochs_requested": args.ecg_epochs' in runner
    assert '"ecg_epochs_effective": effective_ecg_epochs' in runner


def test_reuse_plan_transport_supports_full_algorithm_suite():
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_all_reuse_plan", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    reuse_plan = module.parse_policy_spec("ECG:REUSE_PLAN")
    expected_variants = {
        "pr": "epoch_first",
        "bfs": "degree_first",
        "sssp": "degree_first",
        "bc": "rrip_first",
        "cc": "rrip_first",
    }
    for benchmark, variant in expected_variants.items():
        args = argparse.Namespace(benchmark=benchmark)
        transport = module.ecg_transport_for(reuse_plan, benchmark)
        assert transport.reuse_plan_depth == 2
        assert module.effective_ecg_variant(args, 2, reuse_plan) == variant

    for kernel in ("sssp", "bc", "cc"):
        gem5 = read(f"bench/src_gem5/{kernel}.cc")
        assert "buildInEdgeReusePlanRecords" in gem5
        assert "GEM5_ECG_EXTRACT2" in gem5

    sniper = read("bench/src_sniper/sg_kernel.cc")
    sssp = sniper.split("int run_sssp(", 1)[1].split("void cc_link(", 1)[0]
    assert "buildInEdgeReusePlanRecords" in sssp
    for start, end in (
        ("int run_bc(", "int run_cc("),
        ("int run_cc(", "}  // namespace"),
    ):
        block = sniper.split(start, 1)[1].split(end, 1)[0]
        assert "build_reuse_plan_pair_stream" in block
        assert "deliver_reuse_plan_record" in block
        assert "clear_reuse_plan_record" in block

    sniper_context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/"
        "graph_cache_context_sniper.cc")
    sniper_harness = read("bench/include/sniper_sim/sniper_harness.h")
    assert "clearEcgReusePlan" in sniper_context
    assert "if (tier == 0)" in sniper_context
    assert "SNIPER_ECG_CLEAR_EXTRACT2" in sniper_harness
    for kernel in ("sssp", "bc", "cc"):
        standalone = read(f"bench/src_sniper/{kernel}.cc")
        assert "buildInEdgeReusePlanRecords" in standalone
        assert "SNIPER_ECG_EXTRACT2" in standalone
        assert "SNIPER_ECG_CLEAR_EXTRACT2" in standalone

    cache_sssp = read("bench/src_sim/sssp.cc")
    cache_bc = read("bench/src_sim/bc.cc")
    cache_cc = read("bench/src_sim/cc.cc")
    assert "SIM_ECG_EDGE" in cache_sssp
    assert "SIM_ECG_EDGE" in cache_bc
    assert "buildOutEdgeMasks(g)" in cache_cc
    assert "resolveEdgeMaskAndEpoch(" in cache_cc

    verifier = read("scripts/experiments/ecg/verify/equiv_kernels.py")
    assert '"sssp": ["cache_sim", "gem5", "sniper"]' in verifier
    assert "--reuse-plan-depth 2 currently supports --kernels pr bfs" not in verifier


def test_sniper_dry_run_migration_updates_virtual_text(tmp_path):
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location("setup_sniper_dry_run", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    target = tmp_path / "legacy.cc"
    target.write_text("old anchor\n")
    module.SNIPER_DIR = tmp_path
    module._DRY_RUN_OVERLAY_TEXT.clear()
    module.migrate_if_present(target, "old anchor", "migrated anchor", True)
    module.replace_once(target, "migrated anchor", "final content", True)

    assert target.read_text() == "old anchor\n"
    assert module._overlay_text(target, True) == "final content\n"


def test_sniper_build_dry_run_does_not_require_checkout(tmp_path):
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_missing_checkout", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module.SNIPER_DIR = tmp_path / "missing"
    module.build_sniper(argparse.Namespace(
        skip_build=False,
        build_target="",
        jobs=2,
        dry_run=True,
        skip_deps_check=True,
    ))


def test_sniper_clean_removes_all_capability_markers(tmp_path):
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_clean_markers", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module.SNIPER_DIR = tmp_path / "snipersim"
    module.VERSION_FILE = tmp_path / ".sniper_version"
    module.OVERLAY_STATUS_FILE = tmp_path / ".sniper_overlays.json"
    module.SNIPER_DIR.mkdir()
    module.VERSION_FILE.write_text("{}")
    module.OVERLAY_STATUS_FILE.write_text("{}")
    module.clean(argparse.Namespace(dry_run=False))
    assert not module.SNIPER_DIR.exists()
    assert not module.VERSION_FILE.exists()
    assert not module.OVERLAY_STATUS_FILE.exists()


def test_flowthrough_is_policy_isolated_and_verified():
    runner = read("scripts/experiments/ecg/roi_matrix.py")
    policy_specs = read("scripts/experiments/ecg/policy_specs.py")
    verifier = read("scripts/experiments/ecg/verify/equiv_kernels.py")
    ecg_verifier = read("scripts/experiments/ecg/verify/ecg.py")
    assert "apply_ecg_transport_env" in runner
    assert '"CACHE_FAST": "0"' in runner
    assert '"CACHE_SAMPLED": "0"' in runner
    assert '"CACHE_MULTICORE": "0"' in runner
    assert "FlowThrough requested but cache_sim FlowThrough path was inactive" in runner
    assert '"ECG:REUSE_PLAN_FLOWTHROUGH"' in policy_specs
    assert '"ecg_flowthrough"' in runner
    assert "--flowthrough" in verifier
    assert "flowthrough-reads" in verifier
    assert "flowthrough-writes" in verifier
    assert "dest // vpl == line_id" in ecg_verifier
    assert "cache_sim_ecg_epoch_region_indices" in runner
    assert "Sniper FlowThrough requires --sniper-workload sg_kernel" in runner
    assert 'env.get("ECG_FLOWTHROUGH") == "1"' in runner
    assert "--flowthrough requires --reuse-plan-depth 2" in verifier
    assert "SNIPER_ECG_FUSED_REUSE_PLAN" in runner
    assert "FlowThrough inactive" in runner
    assert "ECG_FLOWTHROUGH_ADAPTIVE" in runner
    assert 'env.pop("SNIPER_ECG_FUSED_REUSE_PLAN", None)' in runner
    assert 'env.pop("SNIPER_ECG_FUSED_VALIDATE", None)' in runner
    assert 'env["SNIPER_CACHE_LINE_SIZE"] = str(args.line_size)' in runner
    assert "GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND" in runner
    assert "previous == record_count" in runner
    assert "mmap.mmap" in runner
    assert ".read_bytes()" not in runner.split(
        "def validate_sniper_fused_receipts", 1
    )[1].split("def ", 1)[0]
    assert "fused_reuse_plan = False" in runner


def test_flowthrough_is_generic_across_reuse_plan_kernels():
    for kernel in ("bfs", "sssp", "bc", "cc"):
        cache_sim = read(f"bench/src_sim/{kernel}.cc")
        gem5 = read(f"bench/src_gem5/{kernel}.cc")
        # FlowThrough is now applied by the shared delivery site, which
        # reads ECG_FLOWTHROUGH once, instead of each kernel spelling out a
        # FlowThrough branch of its own.
        assert "SIM_ECG_EDGE" in cache_sim, kernel
        assert "gem5_ecg_flow_load_enabled()" in gem5, kernel
        expected_load = (
            "gem5_ecg_flow_weighted_load_instruction"
            if kernel == "sssp" else "gem5_ecg_flow_load_instruction")
        assert expected_load in gem5, kernel
        assert "[ECG_REUSE_BIND_LOAD" in gem5, kernel
        assert "[ECG_REUSE_BIND_ILOAD" in gem5, kernel
        expected_stream = (
            "FlowThrough 4B sidecar"
            if kernel == "sssp" else "FlowThrough record load")
        assert expected_stream in gem5, kernel

    sniper = read("bench/src_sniper/sg_kernel.cc")
    for start, end in (
            ("int run_bfs(", "int run_sssp("),
            ("int run_sssp(", "void cc_link("),
            ("int run_bc(", "int run_cc("),
            ("int run_cc(", "}  // namespace")):
        block = sniper.split(start, 1)[1].split(end, 1)[0]
        assert "flowthrough_on" in block
        assert "reinterpret_cast<uint64_t>" in block


def test_flowthrough_setup_migrates_and_rebuilds():
    gem5_setup = read("scripts/setup_gem5.py")
    sniper_setup = read("scripts/setup_sniper.py")
    assert "Incrementally rebuilding gem5" in gem5_setup
    assert 'GEM5_DEFAULT_COMMIT = "b1a44b89c7bae73fae2dc547bc1f871452075b85"' in gem5_setup
    assert "def verify_installation_postconditions" in gem5_setup
    assert "PATCH_STATE_FILE" in gem5_setup
    assert "PATCH_STATE_FILE.unlink(missing_ok=True)" in gem5_setup
    assert "Tracked gem5 patch changed after installation" in gem5_setup
    assert "GRAPHBREW-QUEUE-SERVICING-PATCH" in gem5_setup
    assert "Required gem5 patch file missing" in gem5_setup
    assert "unsupported --tag" in gem5_setup
    assert "base_flowthrough_request_flag.patch" in gem5_setup
    assert "prefetch_flowthrough.patch" in gem5_setup
    assert "def migrate_if_present" in sniper_setup
    assert "def patch_cache_only_history_queue" in sniper_setup
    assert "def patch_cache_only_shmem_timing" in sniper_setup
    assert "CACHE_ONLY warming updates cache state" in sniper_setup
    assert "patch_cache_only_history_queue(args)" in sniper_setup
    assert "patch_cache_only_shmem_timing(args)" in sniper_setup
    assert '"common/performance_model/queue_model_history_list.cc"' in sniper_setup
    assert '"common/performance_model/shmem_perf_model.cc"' in sniper_setup
    assert '"cache_only_warmup_timing"' in sniper_setup
    assert 'SNIPER_DEFAULT_REF = "56505e42fd98bca863fac181e769bd3c98d2bb33"' in sniper_setup
    main_block = sniper_setup.split("def main(argv:", 1)[1]
    assert main_block.index("OVERLAY_STATUS_FILE.unlink(missing_ok=True)") < \
        main_block.index("install_graphbrew_configs(args)")
    assert main_block.index("graphbrew_smoke_test(args)") < \
        main_block.index("write_overlay_status(copied_files)")
    assert "migrate_if_present(\n        magic_server, old_decode, new_decode" in sniper_setup


def test_sniper_cache_only_history_queue_patch_is_idempotent(tmp_path):
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_queue_patch_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    module.SNIPER_DIR = tmp_path / "snipersim"
    queue_source = (
        module.SNIPER_DIR / "common" / "performance_model" /
        "queue_model_history_list.cc"
    )
    queue_source.parent.mkdir(parents=True)
    queue_source.write_text(
        "QueueModelHistoryList::computeQueueDelay("
        "SubsecondTime pkt_time, SubsecondTime processing_time, "
        "core_id_t requester)\n"
        "{\n"
        "   LOG_ASSERT_ERROR(m_free_interval_list.size() >= 1,\n"
        "         \"Free Interval list size < 1\");\n"
        "}\n"
    )

    args = argparse.Namespace(dry_run=False)
    module.patch_cache_only_history_queue(args)
    module.patch_cache_only_history_queue(args)
    text = queue_source.read_text()
    assert text.count("CACHE_ONLY warming updates cache state") == 1
    assert "Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY" in text
    assert "return SubsecondTime::Zero();" in text


def test_sniper_cache_only_shmem_timing_patch_is_idempotent(tmp_path):
    path = ROOT / "scripts/setup_sniper.py"
    spec = importlib.util.spec_from_file_location(
        "setup_sniper_shmem_patch_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    module.SNIPER_DIR = tmp_path / "snipersim"
    shmem_source = (
        module.SNIPER_DIR / "common" / "performance_model" /
        "shmem_perf_model.cc"
    )
    shmem_source.parent.mkdir(parents=True)
    shmem_source.write_text(
        "void\n"
        "ShmemPerfModel::updateElapsedTime("
        "SubsecondTime time, Thread_t thread_num)\n"
        "{\n"
        "   LOG_PRINT(\"updateElapsedTime: time(%s)\", "
        "itostr(time).c_str());\n"
        "}\n\n"
        "void\n"
        "ShmemPerfModel::incrElapsedTime("
        "SubsecondTime time, Thread_t thread_num)\n"
        "{\n"
        "   LOG_PRINT(\"incrElapsedTime: time(%s)\", "
        "itostr(time).c_str());\n"
        "}\n"
    )

    args = argparse.Namespace(dry_run=False)
    module.patch_cache_only_shmem_timing(args)
    module.patch_cache_only_shmem_timing(args)
    text = shmem_source.read_text()
    assert text.count(
        "Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY") == 2
    assert text.count("CACHE_ONLY warms cache contents") == 1


def test_schedule_bits_are_charged_in_record_width():
    pr = read("bench/src_sim/pr.cc")
    graph_sim = read("bench/include/cache_sim/graph_sim.h")
    # Record width now lives in the shared metadata implementation used by
    # gem5 and Sniper, rather than in a cache_sim-only helper.
    meta = read("bench/include/ecg_metadata.h")
    assert 'envInt("ECG_REUSE_PLAN_DEPTH", 0, 0, 4)' in meta
    # two-epoch ReusePlan still defaults to 8 bytes so committed results do not move,
    # but the return is conditional: ECG_RECORD_VARIABLE_WIDTH=1 computes the
    # width from the same bit budget as every other schedule. The unconditional
    # return was an implementation shortcut, not a cost of the second stamp.
    assert 'envInt("ECG_RECORD_VARIABLE_WIDTH", 0, 0, 1) == 0' in meta
    assert "c.record_bytes = 8;" in meta
    # Either way both stamps must be charged, never silently dropped.
    assert "c.epoch_bits * c.stamps" in meta
    assert 'std::getenv("ECG_BFS_EDGE_MASKS")' in graph_sim
    assert 'GetEnvPolicy("CACHE_L3_POLICY", policy)' in graph_sim
    assert "SIM_ECG_EDGE" in graph_sim
    assert "1 <= tier <= 3" in read("scripts/experiments/ecg/roi_matrix.py")
    assert "SIM_ECG_EDGE" in pr
    assert "reinterpret_cast<uint64_t>(src_masks.data())" not in pr
    assert "SIM_CACHE_READ_FLOWTHROUGH" not in pr
    cache_sim = read("bench/include/cache_sim/cache_sim.h")
    assert "current_src != UINT32_MAX" in cache_sim
    assert "GRAPH_SIM_IN_RECORD_BASE" in graph_sim
    assert "GRAPH_SIM_OUT_RECORD_BASE" in graph_sim
    assert "_edge_index * static_cast<uint64_t>(record_bytes)" in graph_sim
    assert "_record_addr + 8ULL" in graph_sim
    bfs = read("bench/src_sim/bfs.cc")
    assert "SIM_CACHE_READ(cache, front.data(), (size_t)v / 64)" in bfs


def test_adaptive_variant_selects_by_kernel(monkeypatch):
    path = ROOT / "scripts/experiments/ecg/roi_matrix.py"
    spec = importlib.util.spec_from_file_location("roi_matrix_adaptive", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    monkeypatch.setenv("ECG_VARIANT", "adaptive")
    expected = {
        "pr": "epoch_first",
        "bfs": "degree_first",
        "sssp": "degree_first",
        "bc": "rrip_first",
        "cc": "rrip_first",
    }
    for benchmark, variant in expected.items():
        args = argparse.Namespace(benchmark=benchmark)
        assert module.effective_ecg_variant(args) == variant
