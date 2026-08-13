// Cross-backend P-OPT findNextRef parity adapter: gem5 side.
//
// Thin, extern "C" wrapper around gem5's RereferenceMatrix::findNextRef
// (bench/include/gem5_sim/overlays/mem/cache/replacement_policies/
// graph_cache_context_gem5.hh -- gem5::replacement_policy::graph namespace).
// See test_popt_findnextref_cross_backend_parity.cc's header comment and
// popt_findnextref_adapter_cache_sim.cc for why each backend gets its own
// translation unit.
#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"

extern "C" uint32_t gem5_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex) {
    gem5::replacement_policy::graph::RereferenceMatrix cfg;
    cfg.data.assign(matrix_bytes,
                     matrix_bytes + static_cast<size_t>(num_cache_lines) *
                     static_cast<size_t>(num_epochs));
    cfg.num_cache_lines = num_cache_lines;
    cfg.num_epochs = num_epochs;
    cfg.epoch_size = epoch_size;
    cfg.sub_epoch_size = sub_epoch_size;
    cfg.enabled = true;
    return cfg.findNextRef(cline_id, current_vertex);
}
