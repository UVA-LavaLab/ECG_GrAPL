// Cross-backend P-OPT findNextRef parity adapter: Sniper side.
//
// Thin, extern "C" wrapper around Sniper's RereferenceMatrix::findNextRef
// (bench/include/sniper_sim/overlays/common/core/memory_subsystem/cache/
// graph_cache_context_sniper.{h,cc} -- graphbrew::sniper namespace). See
// test_popt_findnextref_cross_backend_parity.cc's header comment and
// popt_findnextref_adapter_cache_sim.cc for why each backend gets its own
// translation unit.
//
// NOTE: graph_cache_context_sniper.cc (which DEFINES
// RereferenceMatrix::findNextRef) is a plain, Sniper-SDK-independent C++
// translation unit -- it only includes graph_cache_context_sniper.h,
// ecg_victim_policy.h and standard headers, so it is linked directly into
// this test binary (see the Makefile/pytest build command) rather than
// re-implemented or stubbed here.
#include "graph_cache_context_sniper.h"

extern "C" uint32_t sniper_find_next_ref(
        const uint8_t *matrix_bytes, uint32_t num_cache_lines,
        uint32_t num_epochs, uint32_t epoch_size, uint32_t sub_epoch_size,
        uint32_t cline_id, uint32_t current_vertex) {
    graphbrew::sniper::RereferenceMatrix cfg;
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
