from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_gem5_popt_memoizes_distances_before_rrip_aging():
    source = (
        ROOT
        / "bench/include/gem5_sim/overlays/mem/cache/"
        "replacement_policies/popt_rp.cc"
    ).read_text()

    popt = source.split(
        "// Phase 2: find max rereference distance", 1
    )[1].split(
        "std::shared_ptr<ReplacementData>", 1
    )[0]
    rrip = popt.split(
        "// Phase 3: RRIP tiebreaker", 1
    )[1]

    assert popt.count("ctx.findNextRef(") == 1
    assert "wayDists.emplace_back" in popt
    assert "ctx.findNextRef(" not in rrip
    marker = source.index("[POPT-ACTIVE sim=gem5")
    phase2 = source.index("// Phase 2: find max rereference distance")
    assert marker > phase2
    assert "++poptStats.rereferenceQueries" in source
