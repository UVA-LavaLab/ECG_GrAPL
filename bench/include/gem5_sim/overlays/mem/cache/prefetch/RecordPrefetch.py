from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class EcgRecordPrefetch(ClockedObject):
    type = "EcgRecordPrefetch"
    cxx_class = "gem5::EcgRecordPrefetch"
    cxx_header = "mem/cache/prefetch/ecg_record_prefetch.hh"

    cpu = Param.BaseCPU("CPU supplying the real translation context")
    l1 = Param.BaseCache("Private L1D observed for real record fills and presence")
    l2 = Param.BaseCache("Private L2 presence lookup")
    llc = Param.BaseCache("Allocation-only-at-LLC property prefetch target")
    record_port = RequestPort("Retry-capable real record reads at the L2 input")
    property_port = RequestPort("Retry-capable property prefetch reads at the LLC input")
    window_queue_size = Param.Unsigned(16, "Optional retired record triggers")
    property_queue_size = Param.Unsigned(16, "Outstanding translated property prefetches")
    lookup_latency = Param.Cycles(12, "Dedicated three-level presence and admission pipeline")
    prefetch_latency = Param.Cycles(8, "Record decode and property request pipeline")
    progress_limit = Param.Unsigned(100000, "Maximum cycles without prefetch-engine progress")
