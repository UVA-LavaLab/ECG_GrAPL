#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "ecg_record.h"
#include "ecg_record_native.h"
#include "ecg_record_stream.h"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    std::printf("%-74s [%s]\n", description, condition ? "OK" : "FAIL");
    if (!condition)
        ++failures;
}

ecg_record::Requirements requirements(uint64_t vertices) {
    ecg_record::Requirements req;
    req.vertex_count = vertices;
    req.record_count = 1024;
    req.traversal_count = 3;
    return req;
}

void testAdaptiveBudgets() {
    using namespace ecg_record;
    Layout layout;
    auto req = requirements(uint64_t{1} << 18);
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 4 && layout.id_bits == 18 &&
          layout.distance_bits == 8 && layout.state_bits == 2 &&
          layout.action_bits == 4,
          "18-bit IDs retain an 8/2/4 rich mask in four bytes");
    req.vertex_count = uint64_t{1} << 19;
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 4 && layout.id_bits == 19 &&
          layout.distance_bits == 7 && layout.state_bits == 2 &&
          layout.action_bits == 4,
          "19-bit IDs use all thirteen metadata bits, not a six-bit fallback");
    req.vertex_count = uint64_t{1} << 20;
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 4 && layout.id_bits == 20 &&
          layout.distance_bits == 6 && layout.action_bits == 4,
          "20-bit IDs retain the minimum rich-reference precision");
    req.vertex_count += 1;
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 8 && layout.id_bits == 21 &&
          layout.action_bits == 4 && layout.distance_bits == 37,
          "a required rich action does not silently become another codec");
    req.carrier_bytes = 4;
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "forcing an insufficient carrier fails instead of truncating fields");

    req = requirements(uint64_t{1} << 19);
    req.action_encoding = ActionEncoding::ENUMERATED2;
    check(selectLayout(req, layout) == Status::OK &&
          layout.distance_bits == 9 && layout.action_bits == 2,
          "explicit sparse action trades two bits for reference precision");
    req.action_encoding = ActionEncoding::NONE;
    check(selectLayout(req, layout) == Status::OK &&
          layout.distance_bits == 11 && layout.action_bits == 0,
          "an explicitly action-free rich format uses the residual reference bits");
    req.require_prefetch_action = true;
    check(selectLayout(req, layout) == Status::INVALID_ACTION,
          "a required action cannot disappear during layout selection");
}

void testLegacyPresets() {
    using namespace ecg_record;
    Layout layout;
    DecodedRecord decoded;
    uint64_t word = 0;
    auto req = requirements(32);
    req.record_count = 34;
    req.preset = Preset::FULL14_V1;
    check(selectLayout(req, layout) == Status::OK &&
          layout.id_bits == 5 && layout.distance_bits == 8 &&
          layout.deadline_bits == 21 &&
          encodeRecord(layout, 18, 4, State::FINITE, 0, word) == Status::OK &&
          word == 0x00002212u &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.destination == 18 && decoded.distance == 4,
          "Full14 preserves the actual-ID-width legacy word and quantization");
    req.preset = Preset::SCALE6_V1;
    check(selectLayout(req, layout) == Status::OK &&
          layout.id_bits == 26 && layout.joint_token_bits == 6 &&
          layout.sequence_bits == 32 && layout.deadline_bits == 32 &&
          encodeRecord(layout, 18, 4, State::FINITE, 0, word) == Status::OK &&
          word == 0x10000012u &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.destination == 18 && decoded.distance == 7,
          "Scale6 preserves its fixed twenty-six-plus-six legacy word");
    uint64_t index = 99, sequence = 99;
    check(recordPosition(layout, 0x1000, 0x1000, 34, UINT32_MAX,
                         index, sequence) == Status::OK &&
          index == 0 && sequence == 0,
          "legacy Scale6 retains explicit modulo-32 sequence wrap");
    req.vertex_count = (uint64_t{1} << 26) + 1;
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "legacy Scale6 never truncates a larger vertex ID");
    req = requirements(32);
    req.preset = Preset::FULL14_V1;
    req.record_count = uint64_t{1} << 20;
    check(selectLayout(req, layout) == Status::HORIZON_OVERFLOW,
          "the legacy Full14 deadline horizon is not silently widened");
}

void testWideWordsAndActions() {
    using namespace ecg_record;
    Layout layout;
    DecodedRecord decoded;
    uint64_t word = 0;
    auto req = requirements(uint64_t{1} << 32);
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 8 && layout.id_bits == 32 &&
          layout.distance_bits == 26 && layout.state_bits == 2 &&
          layout.action_bits == 4 && layout.sequence_bits == 64,
          "full thirty-two-bit IDs spill to an explicit eight-byte carrier");
    check(encodeRecord(layout, UINT32_MAX, 100, State::WRAP, 15, word) ==
              Status::OK &&
          (word >> 32) != 0 &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.destination == UINT32_MAX &&
          decoded.state == State::WRAP && decoded.distance >= 100 &&
          decoded.action_delta == 15,
          "wide record data and high action bits survive round-trip");
    check(encodeRecord(layout, uint64_t{1} << 32, 4, State::FINITE, 0, word) ==
              Status::INVALID_ID,
          "a destination outside the declared field is not masked away");
    check(encodeRecord(layout, 0, 4, State::FINITE, 16, word) ==
              Status::INVALID_ACTION,
          "a direct action outside its defined code range fails");

    req = requirements(uint64_t{1} << 19);
    req.action_encoding = ActionEncoding::ENUMERATED2;
    check(selectLayout(req, layout) == Status::OK &&
          encodeRecord(layout, 18, 100, State::FINITE, 12, word) == Status::OK &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.action_delta == 12,
          "two-bit actions use the declared sparse lead mapping");
    check(encodeRecord(layout, 18, 100, State::FINITE, 10, word) ==
              Status::INVALID_ACTION,
          "an unrepresented sparse lead is not silently rounded");
    req = requirements(uint64_t{1} << 60);
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "eight-byte escape does not promise unlimited VID headroom");
}

void testCompactSelectionAndHorizon() {
    using namespace ecg_record;
    Layout layout;
    DecodedRecord decoded;
    uint64_t word = 0;
    auto req = requirements(uint64_t{1} << 26);
    req.preset = Preset::ADAPTIVE_COMPACT_V2;
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 4 && layout.id_bits == 26 &&
          layout.joint_token_bits == 6 && layout.action_bits == 0 &&
          layout.sequence_bits == 64,
          "compact window selection is an explicit v2 policy choice");
    req.record_count = (uint64_t{1} << 31) + 123;
    check(selectLayout(req, layout) == Status::OK &&
          layout.carrier_bytes == 8 && layout.joint_token_bits == 7 &&
          layout.horizon_bits == 32,
          "edge horizon can require spill even when vertex IDs still fit");
    check(encodeRecord(layout, 18, uint64_t{1} << 31,
                       State::FINITE, 0, word) == Status::OK &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.distance == UINT32_MAX,
          "a compact v2 reference can cover distances beyond the old horizon");
    req.action_encoding = ActionEncoding::DIRECT4;
    check(selectLayout(req, layout) == Status::INVALID_ACTION,
          "compact selection does not pretend to contain an action field");
}

void testWideArithmeticAndPrediction() {
    using namespace ecg_record;
    Layout layout;
    auto req = requirements(uint64_t{1} << 32);
    check(selectLayout(req, layout) == Status::OK, "wide address fixture configures");
    uint64_t address = 0, index = 0, sequence = 0, word = 0;
    check(recordAddress(layout, 0x1000, 2, 4, address) == Status::OK &&
          address == 0x1010,
          "record addressing uses the selected eight-byte stride");
    check(recordPosition(layout, address, 0x1000, 4,
                         (uint64_t{1} << 32) + 5, index, sequence) ==
              Status::OK &&
          index == 2 && sequence == (uint64_t{1} << 32) + 8,
          "v2 semantic positions do not truncate at thirty-two bits");
    check(recordAddress(layout, 0x1004, 0, 4, address) ==
              Status::INVALID_ADDRESS,
          "carrier alignment is checked independently of cache-line alignment");
    check(recordAddress(layout, UINT64_MAX - 7, 1, 2, address) ==
              Status::ARITHMETIC_OVERFLOW,
          "record-range overflow is rejected before a memory access");
    Prediction prediction;
    check(encodeRecord(layout, 18, 4, State::WRAP, 0, word) == Status::OK &&
          makePrediction(layout, word, 19, true, prediction) == Status::OK &&
          prediction.state == State::FINITE && prediction.deadline == 23 &&
          makePrediction(layout, word, 19, false, prediction) == Status::OK &&
          prediction.state == State::DEAD,
          "iteration normalization preserves a bound or makes the final wrap dead");
    check(makePrediction(layout, word, UINT64_MAX - 2, true, prediction) ==
              Status::ARITHMETIC_OVERFLOW,
          "v2 deadline overflow never creates a wrapped early prediction");
    check(makePrediction(layout, word, 0, true, prediction) ==
              Status::INVALID_SEQUENCE,
          "linear v2 request positions start at one");
    req.record_count = UINT64_MAX / 2;
    req.traversal_count = 3;
    check(selectLayout(req, layout) == Status::ARITHMETIC_OVERFLOW,
          "whole-work and carrier-byte arithmetic are checked");
}

void testDescriptorAndWindow() {
    using namespace ecg_record;
    Layout original, restored;
    auto req = requirements(uint64_t{1} << 19);
    uint64_t descriptor = 0, lines = 0, bytes = 0;
    check(selectLayout(req, original) == Status::OK &&
          packLayout(original, descriptor) == Status::OK &&
          unpackLayout(descriptor, restored) == Status::OK &&
          original == restored &&
          validateConfiguration(req, restored) == Status::OK,
          "resolved layout has a versioned exact descriptor round-trip");
    auto incompatible = req;
    incompatible.record_count = uint64_t{1} << 32;
    check(validateConfiguration(incompatible, restored) != Status::OK,
          "a descriptor is revalidated against actual graph counts");
    check(unpackLayout(descriptor ^ 1, restored) == Status::INVALID_DESCRIPTOR,
          "descriptor version/magic corruption is not a default layout");
    check(windowStorage(original, 16, 64, lines, bytes) == Status::OK &&
          lines == 2 && bytes == 128,
          "unaligned sixteen-record narrow window needs two real lines");
    req.vertex_count = uint64_t{1} << 32;
    check(selectLayout(req, original) == Status::OK &&
          windowStorage(original, 16, 64, lines, bytes) == Status::OK &&
          lines == 3 && bytes == 192,
          "unaligned sixteen-record wide window needs three real lines");
    check(windowStorage(original, 16, 0, lines, bytes) == Status::INVALID_WIDTH,
          "invalid cache-line geometry does not receive a silent window size");
    original.distance_bits = 0;
    check(packLayout(original, descriptor) == Status::INVALID_LAYOUT,
          "inconsistent resolved fields cannot be exported as a descriptor");
    req = requirements(uint64_t{1} << 26);
    req.max_vertex_id_known = true;
    req.max_vertex_id = (uint64_t{1} << 19) - 1;
    check(selectLayout(req, original) == Status::OK &&
          original.id_bits == 19 && original.carrier_bytes == 4,
          "explicit maximum encoded VID is independent of isolated vertex count");
    req.max_vertex_id = req.vertex_count;
    check(selectLayout(req, original) == Status::INVALID_ID,
          "maximum encoded VID must still belong to the property domain");
}

void testDistanceBoundsAndMalformedRecords() {
    using namespace ecg_record;
    const uint64_t samples[] = {
        1, 2, 3, 4, 7, 8, 15, 31, 63, 64, 100, 127, 128,
        255, 1023, 1024, 4095, uint64_t{1} << 30, 0x7fffffffu,
    };
    for (const Preset preset : {
             Preset::FULL14_V1, Preset::SCALE6_V1,
             Preset::ADAPTIVE_RICH_V2, Preset::ADAPTIVE_COMPACT_V2}) {
        auto req = requirements(32);
        req.preset = preset;
        Layout layout;
        check(selectLayout(req, layout) == Status::OK,
              "distance sweep has a valid explicit preset");
        for (const uint64_t distance : samples) {
            uint64_t word = 0;
            DecodedRecord decoded;
            check(encodeRecord(layout, 18, distance, State::FINITE, 0, word) ==
                      Status::OK &&
                  decodeRecord(layout, word, decoded) == Status::OK &&
                  decoded.distance_valid && decoded.destination == 18 &&
                  decoded.distance >= distance &&
                  decoded.distance <= layout.max_finite_distance,
                  "every decoded bucket is a bounded upper reference");
            if (preset == Preset::FULL14_V1) {
                check(word == ecg_ref32::packRecord32(
                          18, ecg_ref32::encodeDistance(distance),
                          State::FINITE, 0, 5),
                      "Full14 legacy distance words remain bit-for-bit identical");
            } else if (preset == Preset::SCALE6_V1) {
                check(word == ecg_ref32::packScaleRecord32(
                          18, ecg_ref32::encodeScaleToken(distance, State::FINITE), 26),
                      "Scale6 legacy distance words remain bit-for-bit identical");
            }
        }
    }
    auto req = requirements(2);
    req.record_count = uint64_t{1} << 60;
    req.traversal_count = 1;
    req.carrier_bytes = 8;
    req.action_encoding = ActionEncoding::NONE;
    Layout wide;
    check(selectLayout(req, wide) == Status::OK &&
          wide.distance_bits == 61 && wide.exponent_bits == 6,
          "wide references can use high precision without overflowing products");
    for (const uint64_t distance : {
             uint64_t{1}, uint64_t{3}, uint64_t{1} << 40,
             (uint64_t{1} << 60) + 123, (uint64_t{1} << 61) - 1}) {
        uint64_t word = 0;
        DecodedRecord decoded;
        check(encodeRecord(wide, 1, distance, State::FINITE, 0, word) == Status::OK &&
              decodeRecord(wide, word, decoded) == Status::OK &&
              decoded.distance >= distance &&
              decoded.distance <= wide.max_finite_distance,
              "large-distance and large-mantissa arithmetic stays bounded");
    }
    req = requirements(uint64_t{1} << 19);
    Layout layout;
    uint64_t word = 0;
    DecodedRecord decoded;
    check(selectLayout(req, layout) == Status::OK,
          "malformed-record fixture has a valid layout");
    check(encodeRecord(layout, 18, uint64_t{1} << 63,
                       State::UNKNOWN, 0, word) == Status::HORIZON_OVERFLOW &&
          word == 0,
          "an ignored future state cannot spill unchecked distance bits");
    const unsigned mantissa_bits = layout.distance_bits - layout.exponent_bits;
    const uint64_t bad_reference = uint64_t(layout.horizon_bits) << mantissa_bits;
    word = 18 | (bad_reference << layout.id_bits) |
        (uint64_t(State::FINITE) << (layout.id_bits + layout.distance_bits));
    check(decodeRecord(layout, word, decoded) == Status::INVALID_RECORD,
          "v2 rejects reserved reference codes instead of reporting success");
}

void testAvailableRecordWindow() {
    using namespace ecg_record;
    auto req = requirements(128);
    req.preset = Preset::ADAPTIVE_COMPACT_V2;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK,
          "record-window fixture selects the compact policy explicitly");
    RecordWindow window;
    window.remaining_records = 16;
    window.vertex_count = 128;
    const uint64_t lines[16] = {0,0,1,1,2,2,0,2,3,3,4,3,4,5,5,5};
    for (std::size_t index = 0; index < window.words.size(); ++index) {
        check(encodeRecord(layout, lines[index] * 16, 0,
                           State::UNKNOWN, 0, window.words[index]) == Status::OK,
              "a window word is encoded as real bounded record data");
    }
    encodeRecord(layout, 48, 31, State::FINITE, 0, window.words[8]);
    encodeRecord(layout, 64, 7, State::FINITE, 0, window.words[10]);
    encodeRecord(layout, 80, 15, State::FINITE, 0, window.words[13]);
    PrefetchTarget target;
    window.valid_mask = 1;
    check(selectWindowTarget(layout, window, 16, target) == Status::NOT_READY &&
          !target.valid,
          "missing record bytes are not an empty or successful prediction");
    window.valid_mask = UINT16_MAX;
    check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
          target.valid && target.lead == 10 && target.destination == 64,
          "available compact words preserve the shared lead-ten selection");
    req.preset = Preset::ADAPTIVE_RICH_V2;
    check(selectLayout(req, layout) == Status::OK,
          "action-bearing window fixture selects its distinct layout");
    encodeRecord(layout, 0, 100, State::FINITE, 12, window.words[0]);
    encodeRecord(layout, 96, 3, State::FINITE, 0, window.words[12]);
    window.valid_mask = 1;
    check(selectWindowTarget(layout, window, 16, target) == Status::NOT_READY,
          "an encoded action must wait for its actual target word");
    window.valid_mask |= uint16_t{1} << 12;
    check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
          target.valid && target.lead == 12 && target.destination == 96,
          "an action consumes its target word without fictitious intervening data");
    window.remaining_records = 10;
    check(selectWindowTarget(layout, window, 16, target) == Status::INVALID_RECORD,
          "an encoded target beyond the traversal is rejected");
}

void testRecordConstructionAndAllocationLimits() {
    using namespace ecg_record;
    std::mt19937 random(0x52454332u);
    bool legacy_equal = true;
    uint64_t actions = 0;
    for (unsigned sample = 0; sample < 80 && legacy_equal; ++sample) {
        const uint32_t vertices = 16 * (2 + sample % 17);
        std::vector<uint32_t> ids(32 + random() % 160);
        for (auto& id : ids)
            id = random() % vertices;
        const std::vector<uint64_t> offsets = {0, ids.size()};
        for (const Preset preset : {Preset::FULL14_V1, Preset::SCALE6_V1}) {
            auto req = requirements(vertices);
            req.preset = preset;
            req.record_count = ids.size();
            Layout layout;
            RecordStream stream;
            const Status selected = selectLayout(req, layout);
            const Status built = selected == Status::OK
                ? buildRecords(req, layout, 16,
                    [&ids](std::size_t index) { return uint64_t(ids[index]); }, stream)
                : selected;
            ecg_ref32::FlatRecords reference;
            const bool reference_ok = preset == Preset::FULL14_V1
                ? ecg_ref32::buildFlatRecordsFromDestinations(
                    vertices, 16, offsets, ids, reference)
                : ecg_ref32::buildFlatScaleRecordsFromDestinations(
                    vertices, 16, offsets, ids, reference, 26);
            if (built != Status::OK || !reference_ok || stream.size() != ids.size()) {
                std::fprintf(stderr, "construction failed: sample=%u status=%s\n",
                             sample, statusName(built));
                legacy_equal = false;
                break;
            }
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (stream.word(index) != reference.records[index]) {
                    std::fprintf(stderr,
                        "legacy mismatch sample=%u position=%zu new=%llx old=%x\n",
                        sample, index,
                        static_cast<unsigned long long>(stream.word(index)),
                        reference.records[index]);
                    legacy_equal = false;
                    break;
                }
            }
            actions += stream.stats.encoded_actions;
        }
    }
    check(legacy_equal && actions > 0,
          "generic construction preserves legacy future and action words across graphs");

    auto req = requirements(uint64_t{1} << 32);
    req.record_count = 3;
    const std::vector<uint64_t> ids = {UINT32_MAX, 0, UINT32_MAX};
    Layout layout;
    RecordStream stream;
    const auto getter = [&ids](std::size_t index) { return ids[index]; };
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords(req, layout, 16, getter, stream) == Status::OK &&
          stream.records32.empty() && stream.records64.size() == 3 &&
          stream.stats.carrier_bytes == 24 &&
          stream.stats.source_stream_bytes == 12 &&
          stream.stats.property_lines == 2 &&
          stream.stats.auxiliary_peak_bytes < 4096,
          "wide sparse IDs do not require allocating a dense vertex-sized future table");
    DecodedRecord decoded;
    check(decodeRecord(layout, stream.word(0), decoded) == Status::OK &&
          decoded.destination == UINT32_MAX && decoded.distance >= 2,
          "constructed wide records retain the complete unsigned destination");
    BuildLimits limits;
    limits.maximum_carrier_bytes = 16;
    check(buildRecords(req, layout, 16, getter, stream, limits) ==
              Status::RESOURCE_LIMIT && stream.size() == 0,
          "carrier allocation is bounded before constructing a large stream");
    limits.maximum_carrier_bytes = 1024;
    limits.maximum_auxiliary_bytes = 1;
    check(buildRecords(req, layout, 16, getter, stream, limits) ==
              Status::RESOURCE_LIMIT && stream.size() == 0,
          "auxiliary allocator enforces its budget without a success-shaped carrier");
    check(buildRecords(req, layout, 16,
              [&req](std::size_t) { return req.vertex_count; }, stream) ==
              Status::INVALID_ID && stream.size() == 0,
          "invalid source IDs are rejected before any carrier is published");
}

void testNativeConfigurationAndBinding() {
    using namespace ecg_record;
    auto req = requirements(uint64_t{1} << 32);
    req.record_count = 3;
    Layout layout;
    NativeConfiguration configuration;
    configuration.record_base = 0x1000;
    configuration.property_base = 0x80000000;
    configuration.vertex_count = req.vertex_count;
    configuration.record_count = req.record_count;
    configuration.iteration_base = uint64_t{1} << 32;
    configuration.context = 1;
    configuration.generation = 0;
    configuration.control = kNativeEnable | kNativeHasNext;
    check(selectLayout(req, layout) == Status::OK &&
          packLayout(layout, configuration.layout_descriptor) == Status::OK,
          "native v2 configuration keeps layout separate from graph counts");
    NativeRecordAccess access;
    check(nativeRecordAccess(configuration, 0x1008, 8, access) == Status::OK &&
          access.index == 1 && access.sequence == (uint64_t{1} << 32) + 2,
          "native v2 derives a full sequence from the real record address");
    check(nativeRecordAccess(configuration, 0x1008, 4, access) == Status::INVALID_WIDTH,
          "a four-byte opcode cannot silently load half of a wide record");
    uint64_t word = 0;
    NativeLoadResult record, property;
    check(encodeRecord(layout, UINT32_MAX, 2, State::WRAP, 0, word) == Status::OK &&
          nativeRecordResult(configuration, 0x1008, word, record) == Status::OK &&
          nativePropertyAccess(configuration, configuration.property_base,
                               word, 0x1008, property) == Status::OK &&
          record.raw_record == property.raw_record &&
          record.record_address == property.record_address &&
          record.destination == property.destination &&
          record.sequence == property.sequence &&
          record.generation == 0 && property.generation == 0 &&
          property.state == State::FINITE &&
          property.property_address == 0x80000000ULL + UINT32_MAX * uint64_t{4} &&
          property.deadline == property.sequence + 2,
          "raw wide record, address and context bind the two native operations");
    check(nativePropertyAccess(configuration, configuration.property_base + 4,
                               word, 0x1008, property) == Status::INVALID_ADDRESS,
          "a property base outside the configured governed region is rejected");
    configuration.control = 0;
    check(validateNativeConfiguration(configuration, layout) == Status::INVALID_LAYOUT,
          "an unactivated native descriptor is not a usable context");
    configuration.control = kNativeEnable;
    configuration.record_count = uint64_t{1} << 40;
    check(validateNativeConfiguration(configuration, layout) != Status::OK,
          "native counts cannot outgrow the declared reference horizon");
}

}  // namespace

int main() {
    testAdaptiveBudgets();
    testLegacyPresets();
    testWideWordsAndActions();
    testCompactSelectionAndHorizon();
    testWideArithmeticAndPrediction();
    testDescriptorAndWindow();
    testDistanceBoundsAndMalformedRecords();
    testAvailableRecordWindow();
    testRecordConstructionAndAllocationLimits();
    testNativeConfigurationAndBinding();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures != 0;
}
