import importlib.util
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "scripts/experiments/ecg/flows/setup_popt_pin4_port.py"
SPEC = importlib.util.spec_from_file_location("setup_popt_pin4_port", PATH)
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules["setup_popt_pin4_port"] = MOD
SPEC.loader.exec_module(MOD)


def test_graph_ownership_patch_preserves_normal_application_return(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    (source / "cache_pinsim.cpp").write_text(
        """void printStats() { cache.reportTotalStats(); }
VOID Fini(INT32 code, VOID *v)
{
    std::cout << "[PINTOOL] No. of Instructions = " << numInsns << std::endl;
    cache.reportTotalStats();
}
""")
    (source / "llc.h").write_text(
        "class LLC {\n        Graph m_graph;\n};\n")
    (source / "llc.cpp").write_text(
        """void LLC::registerGraph(Graph &g, bool isPull)
{
    m_graph.setGraphProperties(g.num_nodes(), g.num_edges(), g.directed());
    m_graph.setGraphDatastructures(g.out_index(), g.out_neighbors(),
                                   g.in_index(), g.in_neighbors());
    m_isPull = isPull;
}
int f() { return m_graph.num_nodes(); }
""")
    target = tmp_path / "target"
    MOD.copy_simulator_sources(source, target)
    MOD.patch_nonowning_graph(target)
    MOD.patch_fini_receipt(target)

    assert "PIN_ExitProcess" not in (target / "cache_pinsim.cpp").read_text()
    assert "[PIN-FINI] App Exit Code" in (
        target / "cache_pinsim.cpp").read_text()
    assert "Graph* m_graph {nullptr};" in (target / "llc.h").read_text()
    cpp = (target / "llc.cpp").read_text()
    assert "m_graph = &g;" in cpp
    assert "m_graph->num_nodes()" in cpp


def test_grasp_rules_are_parsed_from_preserved_official_source(tmp_path):
    source = tmp_path / "grasp/trace-based-simulators"
    source.mkdir(parents=True)
    (source / "grasp.cpp").write_text(
        "const int num_bits_rrip = 3;\n"
        "const int P_RRIP = 1;\n"
        "const int H_RRIP = 0;\n")
    (source / "common.h").write_text(
        "is_in_high_reuse_region\n"
        "is_in_moderate_reuse_region\n"
        "border_high_reuse = regions[i].min + (f)\n"
        "border_moderate_reuse = regions[i].min + (2*f)\n")

    rules = MOD.parse_grasp_rules(tmp_path / "grasp")

    assert rules["maximum_rrpv"] == 7
    assert rules["intermediate_insert_rrpv"] == 6
    assert rules["priority_insert_rrpv"] == 1
    assert rules["priority_hit_rrpv"] == 0


def test_completed_output_requires_post_stats_application_receipt():
    text = """
~~~ PINTOOL STATS BEGIN ~~~
[LLC-STAT] Total Misses = 531
~~~ PINTOOL STATS END ~~~
[APP] Error = 0.125
[PIN-FINI] App Exit Code = 0
"""
    assert MOD.validate_completed_output(text) == (531, 0.125)
    with pytest.raises(ValueError):
        MOD.validate_completed_output(text.replace("[APP] Error", "[APP] X"))


def test_hash_tree_rejects_missing_provenance(tmp_path):
    with pytest.raises(FileNotFoundError):
        MOD.hash_tree(tmp_path / "missing")
