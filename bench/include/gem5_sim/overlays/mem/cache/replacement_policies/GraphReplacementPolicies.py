# ============================================================================
# gem5 SimObject definitions for GraphBrew graph-aware replacement policies
# ============================================================================
#
# Defines the Python-side SimObject classes that expose Hawkeye, GRASP, P-OPT,
# and ECG replacement policies to gem5's configuration system.
#
# Usage in gem5 Python config:
#   from m5.objects import GraphGraspRP, GraphPoptRP, GraphEcgRP
#
#   l3_repl = GraphGraspRP(max_rrpv=7, num_buckets=11, hot_fraction=0.1)
#   l3_repl = GraphPoptRP(max_rrpv=7)
#   l3_repl = GraphEcgRP(rrpv_max=7, num_buckets=11, ecg_mode="DBG_PRIMARY")
# ============================================================================

from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.ReplacementPolicies import BaseReplacementPolicy


class GraphHawkeyeRP(BaseReplacementPolicy):
    """Hawkeye: OPTgen-trained instruction-PC replacement (ISCA 2016).

    Artifact scope is a conventional uncompressed set-associative LLC.
    """
    type = 'GraphHawkeyeRP'
    cxx_header = "mem/cache/replacement_policies/hawkeye_rp.hh"
    cxx_class = 'gem5::replacement_policy::GraphHawkeyeRP'

    num_sets = Param.Unsigned(8192, "Number of LLC sets.")
    num_ways = Param.Unsigned(16, "LLC associativity.")
    line_size = Param.Unsigned(64, "Cache line size in bytes.")


class GraphGraspRP(BaseReplacementPolicy):
    """GRASP: Graph-aware cache Replacement with Software Prefetching
    (Faldu et al., 2020)

    Extends SRRIP with degree-based 3-tier insertion and hit promotion.
    Property regions loaded from sideband JSON written by benchmark at runtime.
    """
    type = 'GraphGraspRP'
    cxx_header = "mem/cache/replacement_policies/grasp_rp.hh"
    cxx_class = 'gem5::replacement_policy::GraphGraspRP'

    max_rrpv = Param.Unsigned(7,
        "Maximum RRPV value (2^rrpv_bits - 1). Default 7 for 3-bit RRPV.")
    num_buckets = Param.Unsigned(11,
        "Number of degree buckets for vertex classification (matching DBG).")
    hot_fraction = Param.Float(0.15,
        "GRASP frontier_frac as fraction of the VERTEX SPACE (array-relative, "
        "GRASP-faithful + auto-scaling). ~0.15 ~ Faldu's vertex-relative '10%'.")
    llc_size_bytes = Param.Unsigned(8388608,
        "LLC size in bytes (default 8MB). Used for GRASP hot-region boundary.")
    sideband_path = Param.String("/tmp/gem5_graphbrew_ctx.json",
        "Path to sideband JSON written by benchmark with property regions.")


class GraphPoptRP(BaseReplacementPolicy):
    """P-OPT: Practical Optimal cache replacement for Graph Analytics
    (Balaji et al., 2021)

    Oracle baseline using pre-computed rereference distances from the graph
    transpose. 3-phase eviction: non-graph first, then max rereference
    distance, then RRIP tiebreaker.
    Property regions and P-OPT matrix loaded from sideband files at runtime.
    """
    type = 'GraphPoptRP'
    cxx_header = "mem/cache/replacement_policies/popt_rp.hh"
    cxx_class = 'gem5::replacement_policy::GraphPoptRP'

    max_rrpv = Param.Unsigned(7,
        "Maximum RRPV value for tiebreaking. Default 7 (3-bit).")
    sideband_path = Param.String("/tmp/gem5_graphbrew_ctx.json",
        "Path to sideband JSON with property regions.")
    popt_matrix_path = Param.String("/tmp/gem5_popt_matrix.bin",
        "Path to P-OPT rereference matrix binary file.")


class GraphEcgRP(BaseReplacementPolicy):
    """ECG: Expressing Locality and Prefetching for Optimal Caching in Graphs
    (Mughrabi et al., GrAPL @ IPDPS 2026)

    3-level layered eviction with mode-dependent tiebreaker:
      DBG_PRIMARY:  SRRIP -> DBG tier -> dynamic P-OPT (default)
            POPT_PRIMARY: pure P-OPT 3-phase -> DBG tier
    DBG_ONLY:     GRASP-faithful DBG insertion/hit, plain SRRIP victim
            ECG_EMBEDDED: SRRIP -> stored P-OPT hint -> DBG tier (zero LLC overhead)
            ECG_COMBINED: combined DBG+P-OPT insertion RRPV, then pure SRRIP

    Property regions loaded from sideband JSON written by benchmark at runtime.
    """
    type = 'GraphEcgRP'
    cxx_header = "mem/cache/replacement_policies/ecg_rp.hh"
    cxx_class = 'gem5::replacement_policy::GraphEcgRP'

    rrpv_max = Param.Unsigned(7,
        "Maximum RRPV value. 7 for 3-bit, 255 for 8-bit.")
    num_buckets = Param.Unsigned(11,
        "Number of degree buckets for DBG classification.")
    ecg_mode = Param.String("DBG_PRIMARY",
        "Eviction mode: DBG_PRIMARY, POPT_PRIMARY, DBG_ONLY, ECG_EMBEDDED, or ECG_COMBINED.")
    llc_size_bytes = Param.Unsigned(8388608,
        "LLC size in bytes (default 8MB).")
    sideband_path = Param.String("/tmp/gem5_graphbrew_ctx.json",
        "Path to sideband JSON written by benchmark with property regions.")
    popt_matrix_path = Param.String("/tmp/gem5_popt_matrix.bin",
        "Path to P-OPT rereference matrix binary file.")
