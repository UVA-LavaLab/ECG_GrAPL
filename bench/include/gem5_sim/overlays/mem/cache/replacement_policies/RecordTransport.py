from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.SimObject import PyBindMethod


class EcgRecordTransport(ClockedObject):
    type = "EcgRecordTransport"
    cxx_class = "gem5::EcgRecordTransport"
    cxx_header = "mem/cache/replacement_policies/ecg_record_transport.hh"
    cxx_exports = [PyBindMethod("report"), PyBindMethod("pendingWork")]

    cpu = Param.BaseCPU("RISC-V O3 CPU with the retirement observer")
    llc = Param.BaseCache("LLC receiving committed ECG metadata")
    prefetcher = Param.EcgRecordPrefetch(NULL, "Optional paid record-window prefetch engine")
    latency = Param.Cycles(8, "Dedicated metadata-link latency")
    capture_width = Param.Unsigned(1, "Retirement capture lanes; output remains one per cycle")
    apply_updates = Param.Bool(True, "False is the ISA-matched transport control")
    required_context = Param.Unsigned(1, "Required nonzero guest context")
