from m5.objects.IndexingPolicies import BaseIndexingPolicy


class GraphModuloSetAssociative(BaseIndexingPolicy):
    type = "GraphModuloSetAssociative"
    cxx_class = "gem5::GraphModuloSetAssociative"
    cxx_header = "mem/cache/tags/indexing_policies/graph_modulo.hh"
