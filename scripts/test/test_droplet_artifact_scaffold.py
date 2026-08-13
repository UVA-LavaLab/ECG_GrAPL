#!/usr/bin/env python3
"""Static checks for the DROPLET artifact-informed simulator port."""

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (PROJECT_ROOT / path).read_text()


def test_gem5_droplet_issues_current_edge_indirect_prefetches():
    text = read("bench/include/gem5_sim/overlays/mem/cache/prefetch/droplet.cc")

    assert "issueIndirectPrefetches(addr, addresses);" in text
    assert "if (isMiss && !edgeDataLoaded)" not in text


def test_droplet_artifact_defaults_match_runner_and_overlays():
    graph_cache_config = read("bench/include/gem5_sim/configs/graphbrew/graph_cache_config.py")
    graph_se = read("bench/include/gem5_sim/configs/graphbrew/graph_se.py")
    sniper_prefetcher = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/parametric_dram_directory_msi/droplet_prefetcher.cc"
    )
    roi_matrix = read("scripts/experiments/ecg/roi_matrix.py")

    assert 'prefetch_degree=kwargs.get("prefetch_degree", 1)' in graph_cache_config
    assert 'indirect_degree=kwargs.get("indirect_degree", 16)' in graph_cache_config
    assert 'stride_table_size=kwargs.get("stride_table_size", 64)' in graph_cache_config

    assert 'cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/prefetch_degree", core_id, 1)' in sniper_prefetcher
    assert 'cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/indirect_degree", core_id, 16)' in sniper_prefetcher
    assert 'cfgUIntOrDefault("perf_model/" + configName + "/prefetcher/droplet/stride_table_size", core_id, 64)' in sniper_prefetcher

    for text in (graph_se, roi_matrix):
        assert "--droplet-prefetch-degree" in text
        assert "--droplet-indirect-degree" in text
        assert "--droplet-stride-table-size" in text


def test_roi_rows_record_droplet_knobs():
    text = read("scripts/experiments/ecg/roi_matrix.py")

    assert '"droplet_prefetch_degree": args.droplet_prefetch_degree' in text
    assert '"droplet_indirect_degree": args.droplet_indirect_degree' in text
    assert '"droplet_stride_table_size": args.droplet_stride_table_size' in text
    assert 'row["droplet_useful_activity"] = "useful" if prefetch_useful > 0 else "issued_no_useful"' in text
    assert '"droplet_useful_activity": "no_fill"' in text


def test_prefetcher_readme_records_old_artifact_caveats():
    text = read("bench/include/sniper_sim/overlays/common/core/memory_subsystem/prefetcher/README.md")

    assert "old public DROPLET Sniper-6.1 repo" in text
    assert "trainPrefetcherForProperty()" in text
    assert "DROPLET-style port" in text