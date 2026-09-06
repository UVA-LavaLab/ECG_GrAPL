#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "gem5_sim/gem5_harness.h"

int main(int argc, char** argv) {
    const bool invalid = argc > 1 && std::strcmp(argv[1], "bad-address") == 0;
    unsigned requested_bytes = 4;
    if (argc > 1 && !invalid) {
        if (std::strcmp(argv[1], "0") == 0)
            requested_bytes = 0;
        else if (std::strcmp(argv[1], "8") == 0)
            requested_bytes = 8;
        else if (std::strcmp(argv[1], "4") != 0) {
            std::fprintf(stderr, "record width must be 0, 4, or 8\n");
            return 2;
        }
    }
    const char* id_argument = argc > 2 ? argv[2] : "5";
    char* end = nullptr;
    errno = 0;
    const unsigned long id_bits = std::strtoul(id_argument, &end, 10);
    if (argc > 3 || errno || end == id_argument || *end || id_bits < 1 || id_bits > 32) {
        std::fprintf(stderr, "VID width must be an integer from 1 through 32\n");
        return 2;
    }
    ecg_record::Requirements requirements;
    requirements.vertex_count = uint64_t{1} << id_bits;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = requirements.vertex_count - 1;
    requirements.record_count = 4;
    requirements.traversal_count = 2;
    requirements.requested_record_bytes = requested_bytes;
    ecg_record::Layout layout;
    const auto selected = ecg_record::selectLayout(requirements, layout);
    if (selected != ecg_record::Status::OK) {
        std::fprintf(stderr, "Cannot resolve ISA probe layout: %s\n", ecg_record::statusName(selected));
        return 2;
    }
    const bool wide = layout.record_bytes == 8;
    alignas(64) uint32_t records32[4]{};
    alignas(64) uint64_t records64[4]{};
    alignas(64) float properties[4]{};
    const uint64_t backed_vertices = std::min<uint64_t>(requirements.vertex_count, 4);
    const uint64_t first_destination = requirements.vertex_count - backed_vertices;
    const uint64_t property_address = reinterpret_cast<uint64_t>(properties);
    const uint64_t property_offset = first_destination * sizeof(float);
    if (property_address < property_offset) {
        std::fprintf(stderr, "ISA probe backing address is too low for its sparse VID domain\n");
        return 2;
    }
    // Back only the last few logical vertices, avoiding a multi-gigabyte allocation.
    const float* property_base =
        reinterpret_cast<const float*>(property_address - property_offset);
    const uint32_t bits[] = {0x3fa00000, 0x80000000, 0x7fc12345, 0xc0600000};
    const ecg_record::State states[] = {
        ecg_record::State::FINITE, ecg_record::State::DEAD,
        ecg_record::State::UNKNOWN, ecg_record::State::WRAP};
    const uint64_t distances[] = {1, 0, 0, 4};
    for (unsigned index = 0; index < 4; ++index) {
        const uint64_t backed_index = index % backed_vertices;
        if (ecg_record::encodeRecord(layout, first_destination + backed_index, distances[index],
                states[index], records64[index]) != ecg_record::Status::OK)
            return 3;
        records32[index] = static_cast<uint32_t>(records64[index]);
        std::memcpy(&properties[backed_index], &bits[backed_index], sizeof(float));
    }
    if (wide && (records64[3] & (uint64_t{1} << 63)) == 0)
        return 4;
    ecg_record::NativeConfiguration configuration;
    configuration.record_base = wide ? reinterpret_cast<uint64_t>(records64)
                                     : reinterpret_cast<uint64_t>(records32);
    configuration.property_base = reinterpret_cast<uint64_t>(property_base);
    configuration.record_count = 4;
    configuration.vertex_count = requirements.vertex_count;
    configuration.context = 1;
    configuration.generation = 1;
    configuration.control = ecg_record::kNativeEnable;
    if (ecg_record::packLayout(layout, configuration.layout_descriptor) != ecg_record::Status::OK)
        return 5;
    Gem5RecordContext context(configuration);
    context.activate(0, true);
    if (invalid) {
        try {
            context.record(records32 + 4);
        } catch (const std::invalid_argument& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 6;
        }
        return 7;
    }
    bool ok = true;
    GEM5_RESET_STATS();
    GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    for (uint64_t iteration = 0; iteration < 2; ++iteration) {
        if (iteration)
            context.activate(4, false);
        for (unsigned index = 0; index < 4; ++index) {
            const void* address = wide ? static_cast<const void*>(records64 + index)
                                       : static_cast<const void*>(records32 + index);
            const uint64_t raw = wide ? context.record(records64 + index)
                                      : context.record(records32 + index);
            ok = ok && raw == records64[index];
            const float value = context.property(property_base, raw, address);
            uint32_t loaded_bits = 0;
            std::memcpy(&loaded_bits, &value, sizeof(float));
            ok = ok && loaded_bits == bits[index % backed_vertices];
        }
    }
    context.finish();
    GEM5_WORK_END(GEM5_WORK_COMPUTE);
    GEM5_DUMP_STATS();
    context.deactivate();
    std::printf("[ECG-RECORD-ISA native=%u record_bytes=%u cases=8 high_bit=%u result=%s"
                " requested_bytes=%u id_bits=%u metadata_bits=%u horizon_bits=%u mantissa_bits=%u"
                " max_vertex_id=%llu property_backing_bytes=%zu]\n",
                Gem5RecordContext::nativeAvailable() ? 1u : 0u,
                unsigned(layout.record_bytes), wide ? 1u : 0u, ok ? "PASS" : "FAIL",
                requested_bytes, unsigned(layout.id_bits), unsigned(layout.metadata_bits),
                unsigned(layout.horizon_bits), unsigned(layout.mantissa_bits),
                static_cast<unsigned long long>(requirements.max_vertex_id), sizeof(properties));
    return ok ? 0 : 1;
}
