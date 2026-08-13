from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_dbg_average_degree_uses_adjacency_entries():
    """Match faldupriyank/dbg's GA.m semantics on undirected graphs.

    The official DBG implementation computes average degree from GA.m, which is
    the number of directed adjacency entries. GraphBrew used g.num_edges(),
    which returns half that value for undirected CSR graphs and therefore
    changed every logarithmic bucket threshold on KRON and URAND.
    """
    source = (
        ROOT / "bench/include/graphbrew/reorder/reorder_hub.h"
    ).read_text()
    start = source.index("void GenerateDBGMapping")
    end = source.index("// HUBSORTDBG", start)
    body = source[start:end]
    assert "g.num_edges_directed()" in body
    assert "const int64_t num_edges = g.num_edges();" not in body
