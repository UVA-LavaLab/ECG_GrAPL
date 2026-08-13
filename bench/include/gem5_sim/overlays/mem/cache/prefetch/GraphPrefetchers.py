# ============================================================================
# gem5 SimObject definition for DROPLET graph prefetcher
# ============================================================================

from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.Prefetcher import QueuedPrefetcher


class GraphDropletPrefetcher(QueuedPrefetcher):
    """DROPLET: Data-awaRe decOuPLed prEfeTcher for Graphs
    (Basak et al., 2019)

    Separated prefetch engines for edge-list (stride) and property data
    (indirect). The indirect chain: edge_list[i] → neighbor_id →
    property[neighbor_id] decouples from the core's dependency chain.

    Performance: 1.37x average speedup, 15-45% LLC miss reduction.
    Complementary with ECG replacement policy.
    """
    type = 'GraphDropletPrefetcher'
    cxx_header = "mem/cache/prefetch/droplet.hh"
    cxx_class = 'gem5::prefetch::GraphDropletPrefetcher'

    prefetch_degree = Param.Int(1,
        "Number of edge-list cache lines to prefetch ahead (Basak et al. default: 1).")
    indirect_degree = Param.Int(16,
        "Number of indirect property prefetches per edge access (Basak et al. default: 16 = one 64B line of 4B IDs).")
    stride_table_size = Param.Int(64,
        "Number of entries in the stride detector table (Basak et al. default: 64).")


class GraphEcgPfxPrefetcher(QueuedPrefetcher):
    """ECG_PFX: consumes GraphBrew ECG prefetch target hints.

    The target arrives through a GraphBrew m5ops work item emitted by benchmark
    code after decoding ECG/fat-ID metadata. This differs from DROPLET, which
    infers property targets from CSR edge streams.
    """
    type = 'GraphEcgPfxPrefetcher'
    cxx_header = "mem/cache/prefetch/ecg_pfx.hh"
    cxx_class = 'gem5::prefetch::GraphEcgPfxPrefetcher'

    recent_filter_size = Param.Int(256,
        "Number of recently issued property-line prefetches to suppress.")
