#!/usr/bin/env python3
"""
gem5 SE-mode configuration for GraphBrew graph benchmarks.

Configures a TimingSimpleCPU (or DerivO3CPU) with a 3-level cache hierarchy
matching ECG defaults, running a graph benchmark binary under SE (syscall
emulation) mode.

Usage:
    gem5.opt graph_se.py --binary bench/bin_gem5/pr_m5ops \\
        --options "-f graph.sg -s -n 1 -o 5" \\
        --policy GRASP \\
        --graph-metadata results/gem5_metadata/pokec/context.json

    gem5.opt graph_se.py --binary bench/bin_gem5/bfs_m5ops \\
        --options "-f graph.sg -s -n 1 -o 0" \\
        --policy ECG --ecg-mode DBG_PRIMARY \\
        --cpu-type O3

    gem5.opt graph_se.py --binary bench/bin_gem5/pr_m5ops \\
        --options "-f graph.sg -s -n 1 -o 5" \\
        --policy GRASP --prefetcher DROPLET
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# Add configs directory to path for local imports
script_dir = os.path.dirname(os.path.abspath(__file__))
addToPath(script_dir)

from graph_cache_config import (
    make_l1d_cache, make_l1i_cache, make_l2_cache, make_l3_cache,
    make_replacement_policy, make_droplet_prefetcher, make_ecg_pfx_prefetcher,
    make_stride_prefetcher, DEFAULTS,
)
from graph_metadata_loader import load_graph_metadata, metadata_summary
from graph_env_layout import finalize_environment


RUNTIME_SIDEBAND_FILES = (
    ("GEM5_GRAPHBREW_CTX", "/tmp/gem5_graphbrew_ctx.json"),
    ("GEM5_POPT_MATRIX", "/tmp/gem5_popt_matrix.bin"),
    ("GEM5_GRAPHBREW_OUT_EDGES", "/tmp/gem5_graphbrew_out_edges.bin"),
    ("GEM5_GRAPHBREW_IN_EDGES", "/tmp/gem5_graphbrew_in_edges.bin"),
)


def clear_runtime_sideband_files():
    """Remove stale benchmark-written metadata before simulation starts."""
    for env_name, default_path in RUNTIME_SIDEBAND_FILES:
        path = os.environ.get(env_name, default_path)
        if not path:
            continue
        try:
            os.remove(path)
            print(f"  Cleared stale runtime sideband: {path}")
        except FileNotFoundError:
            pass
        except OSError as exc:
            print(f"Warning: could not clear stale runtime sideband {path}: {exc}")


def needs_vertex_hints(args):
    """Keep the outer-vertex marker stream identical for every L3 policy."""
    return True


def benchmark_environment(args):
    """Environment variables visible inside the simulated benchmark."""
    ecg_grasp_popt = args.policy == "ECG" and args.ecg_mode == "ECG_GRASP_POPT"
    ecg_variant = os.environ.get("ECG_VARIANT", "rrip_first")
    force_delivery = os.environ.get("ECG_FORCE_DELIVERY") == "1"
    reuse_plan_depth = os.environ.get("ECG_REUSE_PLAN_DEPTH", "0")
    ecg_epoch_delivery = (
        ecg_grasp_popt and (
            reuse_plan_depth == "2" or
            ecg_variant != "grasp_only" or
            force_delivery
        )
    )
    ecg_pfx_enabled = args.prefetcher == "ECG_PFX"
    env = [
        f"GEM5_ENABLE_VERTEX_HINTS={1 if needs_vertex_hints(args) else 0}",
        f"GRAPHBREW_PREFETCHER={args.prefetcher}",
        f"GEM5_ENABLE_ECG_PFX_HINTS={1 if ecg_pfx_enabled else 0}",
        f"GEM5_ECG_PFX_LOOKAHEAD={args.ecg_pfx_lookahead if ecg_pfx_enabled else 0}",
        f"GEM5_ECG_PFX_HINT_FILTER={args.ecg_pfx_hint_filter if args.prefetcher == 'ECG_PFX' else 0}",
        f"GEM5_ECG_PFX_FILTER_ELEM_SIZE=4",
        f"GEM5_ECG_PFX_FILTER_LINE_SIZE=64",
        f"ECG_REUSE_PLAN_DEPTH={reuse_plan_depth}",
        f"ECG_REUSE_PLAN_DELIVERY_TRACE={os.environ.get('ECG_REUSE_PLAN_DELIVERY_TRACE', '0')}",
        f"ECG_FLOWTHROUGH={os.environ.get('ECG_FLOWTHROUGH', '0')}",
        f"STRUCTURAL_FLOWTHROUGH={os.environ.get('STRUCTURAL_FLOWTHROUGH', '0')}",
        f"ECG_FLOWTHROUGH_TRACE={os.environ.get('ECG_FLOWTHROUGH_TRACE', '0')}",
        f"GEM5_ECG_EPOCH_REGION_INDICES={os.environ.get('GEM5_ECG_EPOCH_REGION_INDICES', '')}",
        f"GEM5_ECG_EPOCH_REGION_INDEX={os.environ.get('GEM5_ECG_EPOCH_REGION_INDEX', '')}",
        f"GEM5_ECG_ISA_VARIANT={os.environ.get('GEM5_ECG_ISA_VARIANT', 'indexed')}",
        f"GEM5_ECG_EPOCH_CSR={os.environ.get('GEM5_ECG_EPOCH_CSR', '0')}",
        f"GEM5_ENABLE_ECG_EXTRACT={1 if ecg_epoch_delivery or (ecg_pfx_enabled and args.ecg_pfx_delivery == 'instruction') or os.environ.get('GEM5_FORCE_ECG_EXTRACT') == '1' else 0}",
        # Fused ecg.load prototype: host GEM5_FORCE_ECG_LOAD=1 selects the single
        # custom-0 load-and-deliver instruction instead of demand-load + ecg.extract.
        f"GEM5_ENABLE_ECG_LOAD={1 if os.environ.get('GEM5_FORCE_ECG_LOAD') == '1' else 0}",
        # Masked indexed-property load: host GEM5_FORCE_ECG_PLOAD=1 selects the
        # custom-0 R-type family that loads the governed property and carries its
        # graph mask on the same request.
        f"GEM5_ENABLE_ECG_PLOAD={1 if os.environ.get('GEM5_FORCE_ECG_PLOAD') == '1' else 0}",
        f"GEM5_ENABLE_ECG_FLOW_LOAD={1 if os.environ.get('GEM5_FORCE_ECG_FLOW_LOAD') == '1' else 0}",
        f"GEM5_ENABLE_ECG_PLAN_LOAD={1 if os.environ.get('GEM5_FORCE_ECG_PLAN_LOAD') == '1' else 0}",
    ]
    # Propagate ECG mode + per-edge-mask lookahead from the outer harness
    # env so kernel-side `ecg_pfx_mode` reads the value roi_matrix.py set.
    for pass_index, pass_name in enumerate((
        "GEM5_ECG_PFX_MODE",
        "ECG_PREFETCH_MODE",
        "GEM5_ECG_EDGE_MASK_LOOKAHEAD",
        "ECG_EDGE_MASK_LOOKAHEAD",
        "ECG_EDGE_MASK_EPOCH",
        "ECG_EDGE_MASK_LINEMIN",
        "ECG_EDGE_MASK_EPOCHS",
        "GRASP_HOT_FRACTION",
        # Path A (epoch-filtered DROPLET lookahead): the kernel gates the
        # next-K lookahead on these; gem5 SE mode does NOT inherit the host
        # env, so they must be forwarded explicitly or the kernel falls back
        # to Path B (lean_pfx_k=0).
        "ECG_EDGE_MASK_PREFETCH",
        "ECG_PREFETCH_EPOCH_FILTER",
        "ECG_PREFETCH_EPOCH_THRESH_PCT",
        # Metadata transport knobs (bench/include/ecg_metadata.h). The guest derives
        # record width and delivery structure from these, so without forwarding
        # them a stage that asks for a 4-byte record silently gets the
        # two-epoch ReusePlan default of 8 and measures the wrong thing while looking
        # correct from the outside.
        "ECG_RECORD_VARIABLE_WIDTH",
        "ECG_EDGE_RECORD_BYTES",
        "ECG_DELIVERY",
        "ECG_SIDECAR_PAYLOAD_BITS",
        "ECG_RECORD_TIER_BITS",
        "ECG_NEXT_USE_RECORD",
        "ECG_NEXT_USE_BITS",
        "ECG_NEXT_USE_LRU",
        "ECG_REF32_RECORD",
        "ECG_REF32_REFERENCE_BITS",
        "ECG_REF32_ACTION_BITS",
        "ECG_VIRTUAL_ID_BITS",
        "ECG_EDGE_MASK_CHARGED",
        # The enforcement knob itself. Without it the guest cannot abort on a
        # width mismatch, so the "enforce, don't report" guard silently reduces
        # to "report" on the one backend where the plumbing is deepest -- which
        # is exactly the failure it exists to prevent.
        "ECG_EXPECT_BYTES_PER_EDGE",
        "GEM5_ECG_COMPACT_ISA",
        "GEM5_ECG_COMPACT_FUSED",
        "GEM5_ECG_COMPACT_REUSE_BIND_FLOW",
        # Derived from the shared metadata definition rather than maintained
        # by hand: these three also feed the width calculation and the receipt's
        # bypass field, and were silently inert in the guest.
        "ECG_RECORD_POPT_BITS",
        "ECG_RECORD_PREFETCH_BITS",
        "GEM5_REUSE_PLAN_SIDECAR",
        "GEM5_REUSE_PLAN_SIDECAR_REQUIRED",
    )):
        outer = os.environ.get(pass_name)
        if outer is not None and outer != "":
            env.append(f"{pass_name}={outer}")
        else:
            env.append(f"GRAPHBREW_ABSENT_ENV_{pass_index:02d}=0")
    for env_name, default_path in RUNTIME_SIDEBAND_FILES:
        env.append(f"{env_name}={os.environ.get(env_name, default_path)}")
    return finalize_environment(env)


def parse_args():
    """Parse command-line arguments for graph benchmark SE simulation."""
    parser = argparse.ArgumentParser(
        description="gem5 SE-mode config for GraphBrew graph benchmarks")

    parser.add_argument("--binary", required=True,
        help="Path to the ECG benchmark binary (e.g., bench/bin_gem5/pr_m5ops)")
    parser.add_argument("--options", default="",
        help="Command-line arguments for the benchmark binary")

    # Cache policy
    parser.add_argument("--policy", default="LRU",
        choices=["LRU", "FIFO", "SRRIP", "RANDOM", "HAWKEYE",
                 "GRASP", "POPT", "ECG"],
        help="Cache replacement policy for L3 (default: LRU)")
    parser.add_argument("--ecg-mode", default="DBG_PRIMARY",
        choices=["DBG_PRIMARY", "POPT_PRIMARY", "ECG_GRASP_POPT", "DBG_ONLY",
                 "ECG_EMBEDDED", "ECG_EPOCH_EMBEDDED", "ECG_COMBINED",
                 "ECG_EXACT", "ECG_EXACT_STORED", "ECG_EXACT_MASK"],
        help="ECG eviction mode (only used with --policy ECG)")
    parser.add_argument("--l1-policy", default="LRU",
        help="L1 cache replacement policy (default: LRU)")
    parser.add_argument("--l2-policy", default="LRU",
        help="L2 cache replacement policy (default: LRU)")

    # Prefetcher
    parser.add_argument("--prefetcher", default="none",
        choices=["none", "DROPLET", "ECG_PFX", "STRIDE"],
        help="Prefetcher to attach (default: none). STRIDE = uniform "
             "structure-stream prefetcher applied to ALL policies for "
             "leveling (cache_sim CACHE_STREAM_PREFETCH_DEGREE analogue).")
    parser.add_argument("--structure-prefetch-degree", type=int, default=4,
        help="Degree (prefetches per trigger) for the STRIDE structure "
             "prefetcher; mirrors cache_sim CACHE_STREAM_PREFETCH_DEGREE.")
    parser.add_argument("--prefetcher-level", default="l2",
        choices=["l1d", "l2"],
        help="Cache level for graph prefetcher attachment (default: l2)")
    parser.add_argument("--droplet-prefetch-degree", type=int, default=1,
        help="DROPLET edge-stream cache lines to prefetch per trigger (artifact default: 1)")
    parser.add_argument("--droplet-indirect-degree", type=int, default=16,
        help="DROPLET neighbor IDs to translate into property prefetches per edge line (artifact default: 16)")
    parser.add_argument("--droplet-stride-table-size", type=int, default=64,
        help="DROPLET stream table entries (artifact config streams default: 64)")
    parser.add_argument("--ecg-pfx-lookahead", default="4",
        help="Future-neighbor lookahead distance for benchmark-emitted ECG_PFX hints")
    parser.add_argument("--ecg-pfx-hint-filter", default="16",
        help="Recent-target filter capacity before emitting ECG_PFX hints; 0 disables filtering")
    parser.add_argument("--ecg-pfx-delivery", default="explicit-hint",
        choices=["explicit-hint", "instruction"],
        help="ECG_PFX delivery path: explicit m5ops hint, RISC-V ecg.extract, or x86 gem5 pseudo-op instruction")

    # CPU model
    parser.add_argument("--cpu-type", default="timing",
        choices=["timing", "O3", "minor"],
        help="CPU model (default: timing = TimingSimpleCPU)")

    # Graph metadata
    parser.add_argument("--graph-metadata", default="",
        help="Path to graph metadata JSON file for graph-aware policies")

    # Cache sizes (override defaults)
    parser.add_argument("--l1d-size", default=DEFAULTS["l1d_size"])
    parser.add_argument("--l1i-size", default=DEFAULTS["l1i_size"])
    parser.add_argument("--l2-size", default=DEFAULTS["l2_size"])
    parser.add_argument("--l3-size", default=DEFAULTS["l3_size"])
    parser.add_argument("--l3-ways", type=int, default=DEFAULTS["l3_assoc"],
        help="L3 associativity / data ways (default: GraphBrew DEFAULTS l3_assoc)")
    parser.add_argument("--max-insts", type=int, default=0,
        help="Cap committed instructions after the benchmark's ROI work-begin "
             "marker (0 = run to completion). This excludes graph loading and "
             "matches Sniper's detailed-ROI stop-by-icount behavior.")

    return parser.parse_args()


def create_system(args):
    """Create the full gem5 system for graph benchmark simulation."""

    if os.environ.get("ECG_REUSE_PLAN_DEPTH", "0") == "2":
        reuse_bind_active = (
            os.environ.get("GEM5_FORCE_ECG_PLOAD") == "1" and
            os.environ.get("GEM5_ECG_PRODUCER") == "1"
        )
        if args.cpu_type == "O3" and not reuse_bind_active:
            raise RuntimeError(
                "two-epoch ReusePlan O3 requires the masked property-load path and "
                "request-bound ReusePlan producer.")
        if args.prefetcher not in ("none", "STRIDE"):
            raise RuntimeError(
                "two-epoch ReusePlan is implemented only with prefetcher none or STRIDE.")

    system = System()
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = "2GHz"
    system.clk_domain.voltage_domain = VoltageDomain()
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("4GB")]

    # ── CPU ──
    if args.cpu_type == "O3":
        system.cpu = DerivO3CPU()
    elif args.cpu_type == "minor":
        system.cpu = MinorCPU()
    else:
        system.cpu = TimingSimpleCPU()

    # Stop first at the benchmark's compute work-begin marker. The cap itself is
    # scheduled from that point so graph loading does not consume the ROI budget.
    if args.max_insts and args.max_insts > 0:
        system.exit_on_work_items = True

    # ── L3 replacement policy (graph-aware) ──
    l3_policy_kwargs = {}
    if args.policy == "ECG":
        l3_policy_kwargs["ecg_mode"] = args.ecg_mode
    if args.policy in ("GRASP", "POPT", "ECG"):
        l3_policy_kwargs["num_buckets"] = 11

    # ── Cache hierarchy ──
    system.cpu.icache = make_l1i_cache(
        policy=args.l1_policy, size=args.l1i_size)
    system.cpu.dcache = make_l1d_cache(
        policy=args.l1_policy, size=args.l1d_size)
    system.l2cache = make_l2_cache(
        policy=args.l2_policy, size=args.l2_size)
    system.l3cache = make_l3_cache(
        policy=args.policy, size=args.l3_size, assoc=args.l3_ways,
        **l3_policy_kwargs)

    # ── Prefetcher ──
    if args.prefetcher == "DROPLET":
        droplet_kwargs = {
            "prefetch_degree": args.droplet_prefetch_degree,
            "indirect_degree": args.droplet_indirect_degree,
            "stride_table_size": args.droplet_stride_table_size,
        }
        if args.prefetcher_level == "l1d":
            system.cpu.dcache.prefetcher = make_droplet_prefetcher(**droplet_kwargs)
            # gem5 Queued::notify drops cross-page prefetches unless the
            # prefetcher has an MMU.
            _pf = system.cpu.dcache.prefetcher
            if hasattr(system.cpu, 'mmu'):
                _pf.registerMMU(system.cpu.mmu)
            elif hasattr(system.cpu, 'dtb'):
                _pf.registerMMU(system.cpu.dtb)
        else:
            system.l2cache.prefetcher = make_droplet_prefetcher(**droplet_kwargs)
            # gem5 Queued::notify drops cross-page prefetches unless the
            # prefetcher has an MMU.
            _pf = system.l2cache.prefetcher
            if hasattr(system.cpu, 'mmu'):
                _pf.registerMMU(system.cpu.mmu)
            elif hasattr(system.cpu, 'dtb'):
                _pf.registerMMU(system.cpu.dtb)
    elif args.prefetcher == "ECG_PFX":
        if args.prefetcher_level == "l1d":
            system.cpu.dcache.prefetcher = make_ecg_pfx_prefetcher()
            # gem5 Queued::notify drops cross-page prefetches unless the
            # prefetcher has an MMU.
            _pf = system.cpu.dcache.prefetcher
            if hasattr(system.cpu, 'mmu'):
                _pf.registerMMU(system.cpu.mmu)
            elif hasattr(system.cpu, 'dtb'):
                _pf.registerMMU(system.cpu.dtb)
        else:
            system.l2cache.prefetcher = make_ecg_pfx_prefetcher()
            # gem5 Queued::notify drops cross-page prefetches unless the
            # prefetcher has an MMU.
            _pf = system.l2cache.prefetcher
            if hasattr(system.cpu, 'mmu'):
                _pf.registerMMU(system.cpu.mmu)
            elif hasattr(system.cpu, 'dtb'):
                _pf.registerMMU(system.cpu.dtb)

    elif args.prefetcher == "STRIDE":
        # Uniform structure-stream prefetcher (leveling, all policies).
        stride_kwargs = {"degree": args.structure_prefetch_degree}
        if args.prefetcher_level == "l1d":
            system.cpu.dcache.prefetcher = make_stride_prefetcher(**stride_kwargs)
            _pf = system.cpu.dcache.prefetcher
        else:
            system.l2cache.prefetcher = make_stride_prefetcher(**stride_kwargs)
            _pf = system.l2cache.prefetcher
        # gem5 Queued::notify drops cross-page prefetches unless the
        # prefetcher has an MMU.
        if hasattr(system.cpu, 'mmu'):
            _pf.registerMMU(system.cpu.mmu)
        elif hasattr(system.cpu, 'dtb'):
            _pf.registerMMU(system.cpu.dtb)

    # ── Memory bus connections ──
    system.membus = SystemXBar()
    system.l2bus = L2XBar()

    # L1 → L2
    system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
    system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports

    # L2 → L3
    system.l2cache.cpu_side = system.l2bus.mem_side_ports
    system.l3bus = L2XBar()
    system.l2cache.mem_side = system.l3bus.cpu_side_ports

    # L3 → Memory
    system.l3cache.cpu_side = system.l3bus.mem_side_ports
    system.l3cache.mem_side = system.membus.cpu_side_ports

    # Memory controller
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_16x4()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

    # System port
    system.system_port = system.membus.cpu_side_ports

    # Interrupt controller
    system.cpu.createInterruptController()
    if hasattr(system.cpu, "interrupts"):
        for interrupt in system.cpu.interrupts:
            if hasattr(interrupt, "pio"):
                interrupt.pio = system.membus.mem_side_ports
            if hasattr(interrupt, "int_requestor"):
                interrupt.int_requestor = system.membus.cpu_side_ports
            if hasattr(interrupt, "int_responder"):
                interrupt.int_responder = system.membus.mem_side_ports

    # CPU ports
    system.cpu.icache_port = system.cpu.icache.cpu_side
    system.cpu.dcache_port = system.cpu.dcache.cpu_side

    # ── Workload ──
    binary = args.binary
    system.workload = SEWorkload.init_compatible(binary)

    process = Process()
    process.cmd = [binary] + args.options.split()
    process.env = benchmark_environment(args)
    # Redirect the simulated benchmark's stdout/stderr to files in the run's
    # output dir. In SE mode this output (Reorder/Trial times, and the
    # "[ECG_LOAD] fused ecg.load delivery ACTIVE" confirmation) does not reach
    # the gem5 process stdout the runner captures, so capture it here for
    # debugging and to make the RISC-V ecg.load path visible.
    try:
        process.output = os.path.join(m5.options.outdir, "benchmark_stdout.txt")
        process.errout = os.path.join(m5.options.outdir, "benchmark_stderr.txt")
    except Exception:
        pass
    system.cpu.workload = process
    system.cpu.createThreads()

    return system


def main():
    args = parse_args()

    clear_runtime_sideband_files()

    # Load graph metadata if provided
    if args.graph_metadata:
        metadata = load_graph_metadata(args.graph_metadata)
        print(metadata_summary(metadata))
        print()
    else:
        if args.policy in ("GRASP", "POPT", "ECG"):
            print(f"Info: --graph-metadata not provided for {args.policy} policy.")
            print("  C++ graph policies will use benchmark-written runtime sideband.")
            print("  Static Python metadata is only needed for pre-exported contexts.")
            print()

    print(f"Configuring gem5 SE simulation:")
    print(f"  Binary:     {args.binary}")
    print(f"  Options:    {args.options}")
    print(f"  CPU:        {args.cpu_type}")
    print(f"  L3 Policy:  {args.policy}"
          + (f" ({args.ecg_mode})" if args.policy == "ECG" else ""))
    print(f"  Prefetcher: {args.prefetcher} ({args.prefetcher_level})")
    print()

    # Create system
    system = create_system(args)

    # Create root and instantiate
    root = Root(full_system=False, system=system)
    m5.instantiate()

    print("Starting simulation...")
    exit_event = m5.simulate()
    cause = exit_event.getCause()
    if args.max_insts and args.max_insts > 0 and "workbegin" not in cause:
        raise RuntimeError(
            "simulation exited before ROI work-begin; instruction cap was "
            f"not applied (cause={cause!r})")
    if args.max_insts and args.max_insts > 0:
        print(f"[max-insts] ROI start reached; scheduling "
              f"{args.max_insts} committed instructions.")
        system.exit_on_work_items = False
        system.cpu.scheduleInstStop(
            0, args.max_insts, "ROI instruction cap reached")
        exit_event = m5.simulate()
        cause = exit_event.getCause()
    print(f"Exiting @ tick {m5.curTick()} because {cause}")
    if args.max_insts and args.max_insts > 0 and "ROI instruction cap" in cause:
        print(f"[max-insts] ROI instruction cap ({args.max_insts}) reached; "
              f"dumping ROI-window stats.")
        m5.stats.dump()


if __name__ == "__m5_main__":
    main()
