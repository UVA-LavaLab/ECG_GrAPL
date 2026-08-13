// Cross-backend P-OPT findNextRef parity adapter: cache_sim side.
//
// Thin, extern "C" wrapper around cache_sim's ecg_policy-adjacent
// RereferenceConfig::findNextRef (bench/include/cache_sim/graph_cache_context.h).
// Kept in its own translation unit (compiled/linked separately from the
// gem5/Sniper adapters -- see test_popt_findnextref_cross_backend_parity.cc)
// so each backend's real header/implementation is exercised under its own
// native namespace/include path with zero risk of symbol collisions between
// the three independently maintained findNextRef copies.
#include "cache_sim/graph_cache_context.h"

extern "C" uint32_t cache_sim_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex) {
    cache_sim::RereferenceConfig cfg;
    cfg.matrix = matrix_bytes;
    cfg.num_cache_lines = num_cache_lines;
    cfg.num_epochs = num_epochs;
    cfg.epoch_size = epoch_size;
    cfg.sub_epoch_size = sub_epoch_size;
    return cfg.findNextRef(cline_id, current_vertex);
}
