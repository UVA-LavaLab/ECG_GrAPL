"""Bounded detailed cells using the ROI runner's existing execution and sealing utilities."""

from __future__ import annotations

from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
from types import ModuleType
from typing import Any

if __package__:
    from . import algorithm_matrix as algorithms
    from .record_receipts import RecordReceiptError, require
    from .record_resources import RecordResourceError, graph_info, plan_algorithm_resources
else:
    import algorithm_matrix as algorithms
    from record_receipts import RecordReceiptError, require
    from record_resources import RecordResourceError, graph_info, plan_algorithm_resources


def environment() -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith(
        ("CACHE_", "ECG_", "GEM5_", "SNIPER_", "POPT_", "GRASP_", "STRUCTURAL_", "TOPT_", "OMP_"))}
    env.update(OMP_NUM_THREADS="1", OMP_WAIT_POLICY="PASSIVE")
    return env


def normalize_sniper_metrics(metrics: dict[str, Any]) -> dict[str, Any]:
    result = dict(metrics)
    for level, prefix in (("l1", "l1d"), ("l2", "l2"), ("l3", "llc")):
        accesses, misses = metrics.get(prefix + "_loads"), metrics.get(prefix + "_load_misses")
        require(type(accesses) is int and type(misses) is int and 0 <= misses <= accesses,
                f"missing or invalid Sniper {level} load counters")
        result[level + "_accesses"] = accesses
        result[level + "_misses"] = misses
        result[level + "_miss_rate"] = misses / accesses if accesses else 0
    return result


def guest_options(args, options, plan, spec, size_bytes, l3_size: str, output: Path) -> list[str]:
    require(options.queries == 1, "independent SpMV queries are cache_sim-only")
    values = [
        "--algorithm", args.benchmark, "--graph", str(options.graph),
        "--mode", spec.record_mechanism or "csr",
        "--policy", "LRU" if spec.record_mechanism else spec.label,
        "--record-base-policy", options.record_base_policy,
        "--source", str(options.source), "--repeat", str(options.repeat), "--delta", str(options.delta),
        "--max-passes", str(options.max_passes),
        "--record-bytes", str(args.ecg_record_bytes),
        "--minimum-mantissa-bits", str(args.ecg_record_minimum_mantissa_bits),
        "--graph-bytes", str(plan["graph_loader_bytes_upper"]),
        "--workspace-bytes", str(args.algorithm_workspace_bytes),
        "--carrier-bytes", str(args.ecg_record_max_carrier_bytes),
        "--auxiliary-bytes", str(args.ecg_record_max_auxiliary_bytes),
        "--l1-bytes", str(size_bytes(str(args.l1d_size))), "--l1-ways", str(args.l1d_ways),
        "--l2-bytes", str(size_bytes(str(args.l2_size))), "--l2-ways", str(args.l2_ways),
        "--llc-bytes", str(size_bytes(l3_size)), "--llc-ways", str(args.l3_ways),
        "--output", str(output),
    ]
    if options.sources:
        values.extend(("--sources", options.sources))
    if args.benchmark == "bfs":
        values.extend(("--bfs-direction", options.bfs_direction,
                       "--bfs-alpha", str(options.bfs_alpha), "--bfs-beta", str(options.bfs_beta)))
    if args.ecg_equivalence:
        values.extend(("--evidence", "--values"))
    return values


def run_gem5(args, spec, l3_size, options, plan, directory, output, log, env, roi):
    require(roi.selected_gem5_isa() == "riscv" and args.gem5_cpu_type == "O3" and
            int(args.gem5_max_insts) == 0, "algorithm qualification requires uncapped RV64 O3")
    binary = roi.VALIDATED_GEM5_GUEST
    require(binary is not None, "current algorithm native guest was not validated")
    roi.verify_staged_guest(binary, roi.VALIDATED_GEM5_GUEST_SHA256)
    sidebands = roi.gem5_sideband_paths(directory)
    sidebands["context"].parent.mkdir(parents=True, exist_ok=True)
    env.update(GEM5_GRAPHBREW_CTX=str(sidebands["context"]),
               GEM5_POPT_MATRIX=str(sidebands["popt_matrix"]),
               GEM5_GRAPHBREW_OUT_EDGES=str(sidebands["out_edges"]),
               GEM5_GRAPHBREW_IN_EDGES=str(sidebands["in_edges"]),
               GRASP_BOUNDARY_MODE="capacity"
               if spec.label == "GRASP_PAPER" else "vertex",
               GRASP_HOT_FRACTION="0.50" if spec.label == "GRASP_PAPER" else "0.15")
    values = guest_options(args, options, plan, spec, roi.parse_size_bytes, l3_size, output)
    command = [
        str(roi.GEM5_OPT), f"--outdir={directory}", str(roi.GEM5_CONFIG),
        "--binary", str(binary), "--options", shlex.join(values),
        "--policy", "ECG" if spec.record_mechanism else spec.policy,
        "--prefetcher", "none", "--cpu-type", "O3",
        "--l1d-size", f"{roi.parse_size_bytes(str(args.l1d_size))}B",
        "--l1d-ways", str(args.l1d_ways),
        "--l2-size", f"{roi.parse_size_bytes(str(args.l2_size))}B",
        "--l2-ways", str(args.l2_ways),
        "--l3-size", f"{roi.parse_size_bytes(l3_size)}B", "--l3-ways", str(args.l3_ways),
        "--mem-size", f"{roi.parse_size_bytes(str(args.gem5_mem_size))}B",
    ]
    if spec.record_mechanism:
        command.extend(("--ecg-native", "--ecg-mechanism", spec.record_mechanism,
                        "--ecg-record-bytes", str(args.ecg_record_bytes), "--ecg-equivalence"))
    if args.dry_run:
        return roi.run_command(command, algorithms.ROOT, env, args.timeout_gem5, log, True,
                               rss_mib=args.gem5_record_rss_mib)
    with ExitStack() as runtime:
        simulator_hash = roi.hash_input_path(roi.GEM5_OPT)
        descriptor, sealed_simulator = roi.open_sealed_guest(roi.GEM5_OPT, simulator_hash)
        runtime.callback(os.close, descriptor)
        command[0] = sealed_simulator
        files = {}
        for path, expected, permissions in (
                (binary, roi.VALIDATED_GEM5_GUEST_SHA256, 0o555),
                (options.graph, plan["sha256"], 0o444)):
            data = path.read_bytes()
            require(hashlib.sha256(data).hexdigest() == expected, "native input changed while sealing")
            files[path.name] = (data, permissions)
        mount = directory / "inputs"
        runtime.enter_context(roi.immutable_fuse_files(files, mount))
        command[command.index("--binary") + 1] = str(mount / binary.name)
        values[values.index("--graph") + 1] = str(mount / options.graph.name)
        command[command.index("--options") + 1] = shlex.join(values)
        config_hash = roi.hash_input_path(roi.GEM5_CONFIG.parent)
        config_files = {}
        for path in sorted(roi.GEM5_CONFIG.parent.iterdir()):
            if path.is_file() and path.suffix == ".py":
                config_files[path.name] = (path.read_bytes(), 0o444)
        require(roi.hash_input_path(roi.GEM5_CONFIG.parent) == config_hash, "native configuration changed")
        config_mount = directory / "configuration"
        runtime.enter_context(roi.immutable_fuse_files(config_files, config_mount))
        require(roi.hash_input_path(config_mount) == config_hash, "sealed native configuration differs")
        command[2] = str(config_mount / roi.GEM5_CONFIG.name)
        return roi.run_command(command, algorithms.ROOT, env, args.timeout_gem5, log, False,
                               (descriptor,), rss_mib=args.gem5_record_rss_mib)


def run_sniper(args, spec, l3_size, options, plan, directory, output, log, env, roi):
    require(str(args.sniper_cores) == "1" and args.sniper_frontend == "sift" and
            args.sniper_address_domain == "translated" and int(args.sniper_roi_icount) == 0 and
            int(args.sniper_semantic_edge_limit) == 0,
            "algorithm Sniper qualification requires one uncapped translated SIFT core")
    require(roi.sniper_graph_policies_enabled(args), "verified Sniper overlays are required")
    runner = roi.sniper_runner_path(args)
    binary = algorithms.ROOT / "bench/bin_sniper/algorithms"
    sidebands = roi.sniper_sideband_paths(directory)
    sidebands["context"].parent.mkdir(parents=True, exist_ok=True)
    values = guest_options(args, options, plan, spec, roi.parse_size_bytes, l3_size, output)
    env.update(SNIPER_GRAPHBREW_CTX=str(sidebands["context"]),
               SNIPER_GRAPHBREW_PREFETCHER="none", SNIPER_CACHE_LINE_SIZE="64",
               SNIPER_ENABLE_VERTEX_HINTS="0", GRASP_BOUNDARY_MODE="capacity"
               if spec.label == "GRASP_PAPER" else "vertex",
               GRASP_HOT_FRACTION="0.50" if spec.label == "GRASP_PAPER" else "0.15")
    if spec.record_mechanism:
        env["SNIPER_ECG_RECORD_MECHANISM"] = spec.record_mechanism
    command = [str(runner), "--roi", "--sift", "-n", "1", "-d", str(directory),
               "-c", args.sniper_base_config]
    for config in args.sniper_config:
        command.extend(("-c", config))
    settings = {
        "general/total_cores": 1, "general/translation_enabled": "true",
        "clock_skew_minimization/scheme": "none",
        "perf_model/l1_dcache/cache_size": roi.format_sniper_kb(args.l1d_size),
        "perf_model/l1_dcache/associativity": args.l1d_ways,
        "perf_model/l1_dcache/replacement_policy": "lru",
        "perf_model/l2_cache/cache_size": roi.format_sniper_kb(args.l2_size),
        "perf_model/l2_cache/associativity": args.l2_ways,
        "perf_model/l2_cache/replacement_policy": "lru",
        "perf_model/nuca/cache_size": roi.format_sniper_kb(l3_size),
        "perf_model/nuca/associativity": args.l3_ways,
        "perf_model/nuca/replacement_policy": roi.sniper_policy_name(args, spec),
        "perf_model/nuca/address_hash": "mod",
        "perf_model/reserve_thp/memory_size": args.sniper_mimicos_memory_mb,
        "perf_model/reserve_thp/kernel_size": args.sniper_mimicos_kernel_mb,
    }
    for level in ("l1_icache", "l1_dcache", "l2_cache"):
        settings[f"perf_model/{level}/cache_block_size"] = 64
    for component in ("perf_model/nuca", "perf_model/dram", "perf_model/dram/cache",
                      "network/emesh_hop_by_hop", "network/bus"):
        settings[f"{component}/queue_model/type"] = args.sniper_queue_model
    for key, value in settings.items():
        require(value is not None, "unsupported Sniper policy configuration")
        command.extend(("-g", f"{key}={value}"))
    command.extend(("--", str(binary), *values))
    setarch = shutil.which("setarch")
    require(setarch is not None, "translated SIFT qualification requires ASLR control")
    command = [setarch, platform.machine(), "-R", *command]
    return roi.run_command(command, algorithms.ROOT, env, args.timeout_sniper, log, args.dry_run,
                           rss_mib=args.sniper_record_rss_mib)


def run_cell(args, out_dir: Path, spec, l3_size: str, backend: str, roi: ModuleType) -> list[dict[str, Any]]:
    row = {"simulator": backend, "benchmark": args.benchmark, "policy": spec.policy,
           "policy_label": spec.label, "l3_size": l3_size, "l3_ways": int(args.l3_ways),
           "status": "error", "current_algorithm": "1", "timing_valid_for_speedup": "0",
           "measurement_scope": "algorithm-setup-kernel-drain",
           "algorithm_record_requested_bytes": args.ecg_record_bytes}
    try:
        require(args.ecg_equivalence and args.benchmark in algorithms.ALGORITHMS and
                args.prefetcher == "none" and args.flowthrough == "off" and
                roi.parse_size_bytes(str(args.line_size)) == 64 and int(args.ecg_charged) == 1,
                "new detailed algorithms require bounded full-work qualification")
        mode = spec.record_mechanism or "csr"
        require(mode in algorithms.MODES or spec.label in ("LRU", "SRRIP", "GRASP_PAPER"),
                "unsupported detailed algorithm policy")
        options = algorithms.parse_options(args.options)
        require(options.record_pressure_gate == "no", "record pressure gate is cache_sim-only")
        require(options.record_rrpv_order == "no", "record RRPV order is cache_sim-only")
        require(options.record_uninformed_base == "no" and options.record_bound_compare == "on" and
                options.record_carrier_first == "no",
                "record victim controls are cache_sim-only")
        require(options.queries == 1, "independent SpMV queries are cache_sim-only")
        require(options.grasp_reference == "off", "GRASP reference diagnostic is cache_sim-only")
        require(options.popt_rank_mode == "future", "P-OPT rank ablation is cache_sim-only")
        require(options.grasp_scope == "all" and options.bfs_traffic_phases == "off",
                "BFS phase controls are cache_sim-only")
        require(options.record_model == "next", f"{options.record_model} model is cache_sim-only")
        require(options.window_observer == "off", "window observer is cache_sim-only")
        require(options.record_base_policy == "LRU",
                "GRASP_PAPER record base is cache_sim-only")
        require(options.bfs_direction == "td", "direction-optimized BFS is currently cache_sim-only")
        require(options.record_preprocess == "csr", "traversal preprocessing is currently cache_sim-only")
        graph = graph_info(options.graph, allow_weighted=True, traversal="out")
        require(graph.vertices <= 4096 and graph.records <= 65536, "detailed algorithm qualification is bounded")
        plan = plan_algorithm_resources(
            graph, algorithm=args.benchmark, records=mode != "csr", requested_bytes=args.ecg_record_bytes,
            minimum_mantissa_bits=args.ecg_record_minimum_mantissa_bits,
            traversals=options.repeat, sources=len(options.source_list) or 1,
            workspace_limit=args.algorithm_workspace_bytes, carrier_limit=args.ecg_record_max_carrier_bytes,
            auxiliary_limit=args.ecg_record_max_auxiliary_bytes,
            rss_mib=args.gem5_record_rss_mib if backend == "gem5" else args.sniper_record_rss_mib,
            backend=backend, target_memory_bytes=roi.parse_size_bytes(str(args.gem5_mem_size)))
        name = f"{backend}_{args.benchmark}_{spec.safe_label}_L3{roi.parse_size_bytes(l3_size)}"
        directory = out_dir / backend / name
        directory.mkdir(parents=True, exist_ok=True)
        output, log = directory / "algorithm.json", out_dir / "logs" / f"{name}.log"
        binary = roi.VALIDATED_GEM5_GUEST if backend == "gem5" else algorithms.ROOT / "bench/bin_sniper/algorithms"
        binary_hash = roi.hash_input_path(binary) if binary is not None else ""
        runner = run_gem5 if backend == "gem5" else run_sniper
        completed = runner(args, spec, l3_size, options, plan, directory, output, log, environment(), roi)
        if args.dry_run:
            return []
        require(completed is not None and completed.returncode == 0, "detailed algorithm process failed")
        require(output.is_file() and roi.hash_input_path(binary) == binary_hash,
                "detailed algorithm output missing or guest changed")
        require(graph_info(options.graph, allow_weighted=True, traversal="out").sha256 == graph.sha256,
                "detailed algorithm graph changed")
        text = log.read_text()
        if backend == "gem5":
            for name in ("benchmark_stdout.txt", "benchmark_stderr.txt"):
                path = directory / name
                if path.is_file():
                    text += "\n" + path.read_text()
        payload = json.loads(output.read_text())
        work = algorithms.validate_payload(
            payload, text, algorithm=args.benchmark, mode=mode,
            policy="LRU" if mode != "csr" else spec.label, graph=graph, graph_path=options.graph,
            options=options, requested_bytes=args.ecg_record_bytes,
            minimum_mantissa_bits=args.ecg_record_minimum_mantissa_bits, evidence=True,
            llc_sets=roi.parse_size_bytes(l3_size) // (64 * int(args.l3_ways)), backend=backend)
        if backend == "gem5":
            sections = roi.parse_gem5_sections(directory / "stats.txt")
            require(bool(sections), "native algorithm stats are missing")
            metrics = sections[0]
        else:
            raw = roi.read_sniper_stats(directory)
            require(raw.get("success"), "Sniper algorithm stats are missing")
            metrics = normalize_sniper_metrics(roi.extract_graphbrew_metrics(raw))
        row.update(metrics)
        row.update(status="ok", timing_valid_for_speedup="0", algorithm_workload_verified="1",
                   graph_sha256=graph.sha256, benchmark_binary_sha256=binary_hash,
                   json_path=str(output), log_path=str(log), detailed_out=str(directory),
                   planned_host_bytes=plan["planned_host_bytes"],
                   **{"algorithm_" + key: value for key, value in work.items()
                      if key != "algorithm" and not key.startswith("values_")})
        if backend == "gem5":
            row.update(
                gem5_guest_staged_path=str(binary),
                gem5_guest_staged_sha256=binary_hash,
                gem5_guest_expected_sha256=str(args.expected_gem5_guest_sha256),
                gem5_opt_expected_sha256=str(args.expected_gem5_opt_sha256),
                gem5_config_expected_sha256=str(args.expected_gem5_config_sha256),
                graph_expected_sha256=str(args.expected_graph_sha256))
    except (RecordReceiptError, RecordResourceError, OSError, json.JSONDecodeError) as error:
        row["error"] = str(error)
    return [row]
