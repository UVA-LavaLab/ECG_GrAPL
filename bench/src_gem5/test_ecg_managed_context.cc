#include <cstdint>
#include <vector>

#include "ecg_record_stream.h"
#include "gem5_sim/gem5_harness.h"

int main()
{
    const std::vector<uint64_t> destinations{0, 1, 2, 3};
    ecg_record::Requirements requirements;
    requirements.vertex_count = 4;
    requirements.max_vertex_id_known = true;
    requirements.max_vertex_id = 3;
    requirements.record_count = destinations.size();
    requirements.requested_record_bytes = 4;
    ecg_record::Layout layout;
    if (ecg_record::selectLayout(requirements, layout) !=
            ecg_record::Status::OK)
        return 1;

    const ecg_record::PropertyDescriptor property{
        ecg_record::PropertyKind::U64, 8,
        ecg_record::TraversalMode::ORDERED_FILTERED};
    uint64_t values[] = {11, 22, 33, 44};
    ecg_record::RecordStream stream;
    if (ecg_record::buildRecords(
            requirements, layout, property,
            reinterpret_cast<uint64_t>(values),
            [&](std::size_t index) { return destinations[index]; },
            stream) != ecg_record::Status::OK)
        return 2;

    ecg_record::NativeConfiguration configuration;
    if (ecg_record::packLayout(
            layout, configuration.layout_descriptor) !=
            ecg_record::Status::OK ||
        ecg_record::packProperty(
            property, configuration.property_descriptor) !=
            ecg_record::Status::OK)
        return 3;
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream.data());
    configuration.property_base =
        reinterpret_cast<uint64_t>(values);
    configuration.record_count = stream.size();
    configuration.vertex_count = 4;
    configuration.generation = 1;
    configuration.context = 1;
    configuration.control = ecg_record::kNativeEnable;

    Gem5ManagedRecordContext context;
    context.bind(configuration, stream);
    context.beginPass();
    const uint64_t first_record = context.loadRecord(1);
    const uint64_t first =
        context.loadProperty(1, first_record, values);
    const uint64_t second_record = context.loadRecord(3);
    const uint64_t second =
        context.loadProperty(3, second_record, values);
    context.closePass();
    if (first != 22 || second != 44 ||
        context.passes() != 1 || context.consumed() != 2 ||
        context.skipped() != 2)
        return 4;

    uint32_t values32[] = {7, 8, 9, 10};
    const ecg_record::PropertyDescriptor property32{
        ecg_record::PropertyKind::U32, 4,
        ecg_record::TraversalMode::DENSE_EXACT};
    ecg_record::RecordStream stream32;
    if (ecg_record::buildRecords(
            requirements, layout, property32,
            reinterpret_cast<uint64_t>(values32),
            [&](std::size_t index) { return destinations[index]; },
            stream32) != ecg_record::Status::OK)
        return 5;
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream32.data());
    configuration.property_base =
        reinterpret_cast<uint64_t>(values32);
    configuration.generation = 2;
    if (ecg_record::packProperty(
            property32, configuration.property_descriptor) !=
            ecg_record::Status::OK)
        return 6;
    context.bind(configuration, stream32);
    context.beginPass();
    for (uint64_t index = 0; index < stream32.size(); ++index) {
        const uint64_t record = context.loadRecord(index);
        if (context.loadProperty(index, record, values32) !=
                values32[index])
            return 7;
    }
    context.closePass();

    float values_f32[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const ecg_record::PropertyDescriptor property_f32{};
    ecg_record::RecordStream stream_f32;
    if (ecg_record::buildRecords(
            requirements, layout, property_f32,
            reinterpret_cast<uint64_t>(values_f32),
            [&](std::size_t index) { return destinations[index]; },
            stream_f32) != ecg_record::Status::OK)
        return 8;
    configuration.record_base =
        reinterpret_cast<uint64_t>(stream_f32.data());
    configuration.property_base =
        reinterpret_cast<uint64_t>(values_f32);
    configuration.generation = 3;
    if (ecg_record::packProperty(
            property_f32, configuration.property_descriptor) !=
            ecg_record::Status::OK ||
        configuration.property_descriptor != 0)
        return 9;
    context.bind(configuration, stream_f32);
    context.beginPass();
    for (uint64_t index = 0; index < stream_f32.size(); ++index) {
        const uint64_t record = context.loadRecord(index);
        if (context.loadProperty(index, record, values_f32) !=
                values_f32[index])
            return 10;
    }
    context.closePass();
    context.finish(10);
    return 0;
}
