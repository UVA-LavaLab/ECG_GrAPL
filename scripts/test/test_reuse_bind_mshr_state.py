import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_reuse_plan_mshr_merge_state(tmp_path: Path):
    source = tmp_path / "reuse_plan_mshr_state.cc"
    binary = tmp_path / "reuse_plan_mshr_state"
    source.write_text(
        r'''
#include <cassert>
#include <cstdint>
#include <memory>

#include "mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh"

using gem5::Request;
using gem5::RequestPtr;
using namespace gem5::replacement_policy::graph;

RequestPtr
reuse_plan(uint16_t requestor, uint16_t context, uint32_t sequence,
   uint32_t dest = 7, uint16_t epoch1 = 10, uint16_t epoch2 = 20)
{
    auto req = std::make_shared<Request>();
    req->requestorId(requestor);
    attachEcgReusePlan(
        req, dest, 1, epoch1, epoch2, 3, context, sequence);
    return req;
}

RequestPtr
plain(uint16_t requestor)
{
    auto req = std::make_shared<Request>();
    req->requestorId(requestor);
    return req;
}

int
main()
{
    {
        EcgReuseBindMshrState state;
        auto first = reuse_plan(1, 4, 100);
        state.merge(first);
        state.merge(reuse_plan(1, 4, 250, 9, 30, 40));
        auto forwarded = first->getExtension<EcgReusePlanExtension>();
        assert(forwarded && forwarded->sequence() == 250);
        assert(forwarded->dest() == 9);
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && !ext->conflicted());
        assert(ext->sequence() == 250);
        assert(ext->dest() == 9);
    }
    {
        EcgReuseBindMshrState state;
        auto req = reuse_plan(1, 4, 100);
        state.merge(req);
        state.merge(req);
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && !ext->conflicted() && ext->sequence() == 100);
    }
    {
        EcgReuseBindMshrState state;
        auto first = reuse_plan(1, 4, 100);
        state.merge(first);
        state.merge(reuse_plan(2, 4, 200));
        assert(first->getExtension<EcgReusePlanExtension>()->conflicted());
        state.merge(reuse_plan(1, 4, 300));
        assert(first->getExtension<EcgReusePlanExtension>()->conflicted());
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    {
        EcgReuseBindMshrState state;
        state.merge(reuse_plan(1, 4, 100));
        state.merge(reuse_plan(1, 5, 200));
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    {
        EcgReuseBindMshrState state;
        state.merge(reuse_plan(1, 4, 100));
        state.merge(reuse_plan(1, 4, 100, 8));
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    {
        EcgReuseBindMshrState state;
        auto first = reuse_plan(1, 4, 100);
        state.merge(first);
        state.merge(plain(1));
        assert(first->getExtension<EcgReusePlanExtension>()->conflicted());
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    {
        EcgReuseBindMshrState state;
        state.merge(reuse_plan(1, 0, 100));
        auto out = plain(1);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    {
        EcgReuseBindMshrState state;
        state.merge(plain(1));
        state.merge(plain(1));
        auto out = plain(1);
        state.apply(out);
        assert(!out->getExtension<EcgReusePlanExtension>());
    }
    {
        EcgReuseBindMshrState state;
        state.merge(reuse_plan(1, 4, 100));
        auto out = reuse_plan(1, 4, 90);
        markEcgReuseBindConflict(out);
        state.apply(out);
        auto ext = out->getExtension<EcgReusePlanExtension>();
        assert(ext && ext->conflicted());
    }
    return 0;
}
'''
    )
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-I",
            str(ROOT / "bench/include/gem5_sim/overlays"),
            "-I",
            str(ROOT / "bench/include/gem5_sim/gem5/build/RISCV"),
            "-I",
            str(ROOT / "bench/include/gem5_sim/gem5/src"),
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True, cwd=ROOT)
