from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.SimObject import PyBindMethod


class EcgRef32CommitTransport(ClockedObject):
    type = "EcgRef32CommitTransport"
    cxx_class = "gem5::EcgRef32CommitTransport"
    cxx_header = (
        "mem/cache/replacement_policies/ecg_ref32_commit_transport.hh"
    )
    cxx_exports = [
        PyBindMethod("report"),
        PyBindMethod("pendingUpdates"),
        PyBindMethod("drainBudgetTicks"),
    ]

    cpu = Param.BaseCPU("O3 CPU whose Commit probe supplies updates")
    llc = Param.BaseCache("LLC receiving committed REF32 metadata")
    latency = Param.Cycles(8, "Dedicated metadata-link latency in CPU cycles")
    capture_width = Param.Unsigned(
        1, "Retirement capture lanes per CPU cycle; link output remains one-wide"
    )
    apply_updates = Param.Bool(
        True, "Apply queued updates; false validates the ISA path only"
    )
    allow_drops = Param.Bool(
        False, "Permit diagnostic degradation instead of failing on drops"
    )
    required_context = Param.Unsigned(
        1, "Only supported nonzero REF32 context"
    )
