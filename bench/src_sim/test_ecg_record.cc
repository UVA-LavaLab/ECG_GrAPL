#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

#include "ecg_record.h"
#include "ecg_record_native.h"
#include "ecg_record_runtime.h"
#include "ecg_record_stream.h"
#include "ecg_record_window.h"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("%-74s [FAIL]\n", description);
        ++failures;
    }
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
    for (uint8_t id_bits = 18; id_bits <= 20; ++id_bits) {
        auto req = requirements(uint64_t{1} << id_bits);
        check(selectLayout(req, layout) == Status::OK &&
              layout.record_bytes == 4 && layout.id_bits == id_bits &&
              layout.metadata_bits == 32 - id_bits &&
              layout.horizon_bits == 11 && layout.mantissa_bits == 27 - id_bits,
              "each VID bit leaves the complete residual metadata budget");
    }
    auto req = requirements(uint64_t{1} << 19);
    req.record_count = uint64_t{1} << 30;
    check(selectLayout(req, layout) == Status::OK &&
          layout.metadata_bits == 13 && layout.horizon_bits == 31 &&
          layout.mantissa_bits == 7,
          "nineteen plus thirteen has seven mantissa bits at horizon thirty-one");
    req.record_count = 34;
    check(selectLayout(req, layout) == Status::OK &&
          layout.horizon_bits == 6 && layout.mantissa_bits == 9,
          "a shorter graph horizon increases precision without changing the method");
    req = requirements(uint64_t{1} << 27);
    check(selectLayout(req, layout) == Status::OK &&
          layout.record_bytes == 4 && layout.metadata_bits == 5 &&
          layout.mantissa_bits == 0,
          "four-byte storage remains possible when the actual horizon fits");
    req.vertex_count = uint64_t{1} << 28;
    check(selectLayout(req, layout) == Status::OK && layout.record_bytes == 8,
          "only insufficient joint code space forces a wider record");
    req.requested_record_bytes = 4;
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "forcing an insufficient record width fails without truncation");
    req = requirements(uint64_t{1} << 19);
    req.minimum_mantissa_bits = 10;
    check(selectLayout(req, layout) == Status::OK &&
          layout.record_bytes == 8 && layout.mantissa_bits >= 10,
          "explicit precision requirements are charged through record width");
    req.requested_record_bytes = 4;
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "an explicit precision floor is never silently reduced");
    req = requirements(uint64_t{1} << 60);
    check(selectLayout(req, layout) == Status::FORMAT_OVERFLOW,
          "eight-byte records do not promise unlimited VID headroom");
}

void testSixBitConfiguration() {
    using namespace ecg_record;
    auto req = requirements(uint64_t{1} << 26);
    req.record_count = uint64_t{1} << 30;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK &&
          layout.record_bytes == 4 && layout.metadata_bits == 6 &&
          layout.horizon_bits == 31 && layout.mantissa_bits == 0 &&
          layout.sequence_bits == 64 && layout.deadline_bits == 64,
          "six metadata bits are a numeric graph configuration, not another method");
    for (const State state : {State::FINITE, State::WRAP}) {
        for (unsigned exponent = 0; exponent < 31; ++exponent) {
            for (const uint64_t distance :
                 {uint64_t{1} << exponent, lowMask(exponent + 1)}) {
                uint64_t word = 0;
                DecodedRecord decoded;
                check(encodeRecord(layout, 18, distance, state, word) == Status::OK &&
                      word == ecg_ref32::packScaleRecord32(
                          18, ecg_ref32::encodeScaleToken(distance, state), 26) &&
                      decodeRecord(layout, word, decoded) == Status::OK &&
                      decoded.distance == lowMask(exponent + 1),
                      "the numeric six-bit configuration reproduces archived token bytes");
            }
        }
    }
    req.record_count = (uint64_t{1} << 31) + 123;
    check(selectLayout(req, layout) == Status::OK &&
          layout.record_bytes == 8 && layout.horizon_bits == 32,
          "a larger record horizon can require width even when IDs are unchanged");
}

void testWideArithmeticAndPrediction() {
    using namespace ecg_record;
    auto req = requirements(uint64_t{1} << 32);
    req.record_count = uint64_t{1} << 30;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK &&
          layout.record_bytes == 8 && layout.id_bits == 32 &&
          layout.metadata_bits == 32,
          "full unsigned thirty-two-bit IDs use explicitly charged eight-byte records");
    uint64_t word = 0, address = 0, index = 0, sequence = 0;
    DecodedRecord decoded;
    check(encodeRecord(layout, UINT32_MAX, uint64_t{1} << 30,
                       State::WRAP, word) == Status::OK &&
          (word & (uint64_t{1} << 63)) != 0 &&
          decodeRecord(layout, word, decoded) == Status::OK &&
          decoded.destination == UINT32_MAX && decoded.state == State::WRAP &&
          decoded.distance >= (uint64_t{1} << 30),
          "the highest raw record bit and the full destination survive round-trip");
    check(encodeRecord(layout, uint64_t{1} << 32, 4, State::FINITE, word) ==
              Status::INVALID_ID && word == 0,
          "out-of-field destination bits are rejected rather than masked");
    check(recordAddress(layout, 0x1000, 2, 4, address) == Status::OK &&
          address == 0x1010 &&
          recordPosition(layout, address, 0x1000, 4,
                         (uint64_t{1} << 32) + 5, index, sequence) == Status::OK &&
          index == 2 && sequence == (uint64_t{1} << 32) + 8,
          "record addressing and semantic positions retain their full width");
    check(recordAddress(layout, 0x1004, 0, 4, address) == Status::INVALID_ADDRESS,
          "record alignment is checked independently of cache-line alignment");
    check(recordAddress(layout, UINT64_MAX - 7, 1, 2, address) ==
              Status::ARITHMETIC_OVERFLOW && address == 0,
          "record range overflow fails before memory access");
    check(recordPosition(layout, 0x1000, 0x1000, 4, UINT64_MAX,
                         index, sequence) == Status::ARITHMETIC_OVERFLOW &&
          index == 0 && sequence == 0,
          "semantic position overflow cannot become a wrapped sequence");
    Prediction prediction;
    check(encodeRecord(layout, 18, 4, State::WRAP, word) == Status::OK &&
          makePrediction(layout, word, 19, true, prediction) == Status::OK &&
          prediction.state == State::FINITE && prediction.deadline == 23 &&
          decodeRecord(layout, prediction.normalized_record, decoded) == Status::OK &&
          decoded.state == State::FINITE &&
          makePrediction(layout, word, 19, false, prediction) == Status::OK &&
          prediction.state == State::DEAD && prediction.deadline == 0 &&
          decodeRecord(layout, prediction.normalized_record, decoded) == Status::OK &&
          decoded.state == State::DEAD,
          "a wrap becomes a finite bound or final-iteration DEAD in the same grammar");
    check(makePrediction(layout, word, UINT64_MAX - 2, true, prediction) ==
              Status::ARITHMETIC_OVERFLOW,
          "deadline overflow never creates an early prediction");
    check(makePrediction(layout, word, 0, true, prediction) == Status::INVALID_SEQUENCE,
          "linear request positions begin at one");
    req.record_count = UINT64_MAX / 2;
    check(selectLayout(req, layout) == Status::ARITHMETIC_OVERFLOW,
          "whole-work and carrier-byte arithmetic are checked");
}

void testDescriptorAndNaming() {
    using namespace ecg_record;
    auto req = requirements(uint64_t{1} << 19);
    Layout original, restored;
    uint64_t descriptor = 0, lines = 0, bytes = 0;
    check(selectLayout(req, original) == Status::OK &&
          packLayout(original, descriptor) == Status::OK &&
          (descriptor & 0xFF) == 0xEC &&
          unpackLayout(descriptor, restored) == Status::OK && original == restored &&
          validateConfiguration(req, restored) == Status::OK,
          "resolved numeric fields have an exact checked descriptor");
    auto incompatible = req;
    incompatible.record_count = uint64_t{1} << 32;
    check(validateConfiguration(incompatible, restored) != Status::OK,
          "a descriptor is revalidated against actual graph counts");
    check(unpackLayout(descriptor ^ 1, restored) == Status::INVALID_DESCRIPTOR &&
          unpackLayout(descriptor | (uint64_t{1} << 63), restored) ==
              Status::INVALID_DESCRIPTOR,
          "signature corruption and reserved descriptor bits fail closed");
    check(unpackLayout(descriptor ^ (uint64_t{1} << 40), restored) ==
              Status::INVALID_LAYOUT,
          "a claimed precision cannot disagree with the joint code capacity");
    std::ostringstream fields;
    check(writeLayoutFields(fields, original) == Status::OK &&
          fields.str().find("record_bytes=4 id_bits=19 metadata_bits=13") == 0 &&
          fields.str().find("state_encoding=joint-distance") != std::string::npos &&
          fields.str().find("prefetch_selection=record-window") != std::string::npos,
          "experiment fields describe numeric layout and the single mechanism");
    Mechanism mechanism;
    for (const auto value : {Mechanism::TRANSPORT, Mechanism::REPLACEMENT,
                            Mechanism::PREFETCH, Mechanism::REPLACEMENT_PREFETCH}) {
        check(parseMechanismName(mechanismName(value), mechanism) == Status::OK &&
              mechanism == value, "mechanism names round-trip without format families");
    }
    check(parseMechanismName("unknown", mechanism) != Status::OK &&
          mechanism == Mechanism::INVALID,
          "unrecognized experiment names do not silently select a mechanism");
    check(windowStorage(original, 16, 64, lines, bytes) == Status::OK &&
          lines == 2 && bytes == 128,
          "an unaligned sixteen-record narrow window needs two real lines");
    req.vertex_count = uint64_t{1} << 32;
    check(selectLayout(req, original) == Status::OK &&
          windowStorage(original, 16, 64, lines, bytes) == Status::OK &&
          lines == 3 && bytes == 192,
          "an unaligned sixteen-record wide window needs three real lines");
    check(windowStorage(original, 16, 0, lines, bytes) == Status::INVALID_WIDTH,
          "invalid cache geometry has no success-shaped storage result");
    req.max_vertex_id_known = true;
    req.max_vertex_id = (uint64_t{1} << 19) - 1;
    check(selectLayout(req, original) == Status::OK &&
          original.id_bits == 19 && original.record_bytes == 4,
          "actual maximum encoded VID is independent of isolated vertex count");
    req.max_vertex_id = req.vertex_count;
    check(selectLayout(req, original) == Status::INVALID_ID,
          "maximum encoded VID must belong to the governed property domain");
}

void testDistanceBoundsAndMalformedRecords() {
    using namespace ecg_record;
    std::mt19937_64 random(0x5245464552454E43ULL);
    for (const unsigned id_bits : {1u, 5u, 18u, 19u, 20u, 26u, 32u}) {
        for (const unsigned horizon : {1u, 6u, 11u, 31u, 40u, 61u}) {
            auto req = requirements(uint64_t{1} << id_bits);
            req.record_count = uint64_t{1} << (horizon - 1);
            req.traversal_count = 1;
            Layout layout;
            check(selectLayout(req, layout) == Status::OK,
                  "numeric precision sweep selects a supported layout");
            for (unsigned sample = 0; sample < 256; ++sample) {
                const uint64_t distance = sample == 0 ? layout.max_finite_distance
                    : 1 + random() % layout.max_finite_distance;
                uint64_t word = 0;
                DecodedRecord decoded;
                check(encodeRecord(layout, 1, distance, State::FINITE, word) == Status::OK &&
                      decodeRecord(layout, word, decoded) == Status::OK &&
                      decoded.distance_valid && decoded.distance >= distance &&
                      decoded.distance <= layout.max_finite_distance,
                      "every decoded reference is a horizon-bounded upper bound");
            }
        }
    }
    auto req = requirements(uint64_t{1} << 19);
    Layout layout;
    uint64_t word = 0;
    DecodedRecord decoded;
    check(selectLayout(req, layout) == Status::OK,
          "malformed-record fixture configures");
    check(encodeRecord(layout, 18, uint64_t{1} << 63, State::UNKNOWN, word) ==
              Status::HORIZON_OVERFLOW && word == 0,
          "an ignored state cannot spill unchecked distance bits");
    check(encodeRecord(layout, 18, 0, State::FINITE, word) == Status::HORIZON_OVERFLOW &&
          encodeRecord(layout, 18, 0, static_cast<State>(0xFF), word) ==
              Status::INVALID_STATE,
          "zero finite distance and invalid semantic state are rejected");
    word = 18 | ((2 * layout.codes_per_state + 2) << layout.id_bits);
    check(decodeRecord(layout, word, decoded) == Status::INVALID_RECORD,
          "unused joint-reference tokens fail closed");
    check(decodeRecord(layout, uint64_t{1} << 32, decoded) == Status::INVALID_RECORD,
          "narrow records reject rather than discard high data bits");
}

void testAvailableRecordWindow() {
    using namespace ecg_record;
    for (const uint8_t bytes : {uint8_t{4}, uint8_t{8}}) {
        auto req = requirements(128);
        req.requested_record_bytes = bytes;
        Layout layout;
        check(selectLayout(req, layout) == Status::OK,
              "both record widths use the same window selection");
        RecordWindow window;
        window.remaining_records = 16;
        window.vertex_count = 128;
        const uint64_t lines[16] = {0,0,1,1,2,2,0,2,3,3,4,3,4,5,5,5};
        for (std::size_t index = 0; index < window.words.size(); ++index) {
            check(encodeRecord(layout, lines[index] * 16, 0, State::UNKNOWN,
                               window.words[index]) == Status::OK,
                  "window entries are actual valid record words");
        }
        encodeRecord(layout, 48, 31, State::FINITE, window.words[8]);
        encodeRecord(layout, 64, 7, State::FINITE, window.words[10]);
        encodeRecord(layout, 80, 15, State::FINITE, window.words[13]);
        PrefetchTarget target;
        window.valid_mask = 1;
        check(selectWindowTarget(layout, window, 16, target) == Status::NOT_READY &&
              !target.valid,
              "missing bytes are distinct from a completed empty prediction");
        window.valid_mask = UINT16_MAX;
        check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
              target.valid && target.lead == 10 && target.destination == 64,
              "candidate ranking uses decoded future bound and lead proximity");
        encodeRecord(layout, 48, 1, State::FINITE, window.words[8]);
        check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
              target.lead == 8,
              "reference priority takes precedence over the nominal lead");
        encodeRecord(layout, 48, 1, State::FINITE, window.words[3]);
        check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
              target.lead == 10,
              "a candidate already encountered earlier in the window is suppressed");
        window.remaining_records = 8;
        window.valid_mask = 0xFF;
        check(selectWindowTarget(layout, window, 16, target) == Status::OK &&
              !target.valid,
              "a complete traversal tail has no out-of-range prefetch");
    }
}

void testRecordConstructionAndAllocationLimits() {
    using namespace ecg_record;
    std::mt19937 random(0x52454332u);
    for (unsigned sample = 0; sample < 80; ++sample) {
        const uint32_t vertices = 16 * (2 + sample % 17);
        std::vector<uint32_t> ids(32 + random() % 160);
        for (auto& id : ids)
            id = random() % vertices;
        auto req = requirements(vertices);
        req.requested_record_bytes = sample % 2 ? 4 : 8;
        req.record_count = ids.size();
        Layout layout;
        RecordStream stream;
        const Status selected = selectLayout(req, layout);
        const Status built = selected == Status::OK
            ? buildRecords(req, layout, 16,
                [&ids](std::size_t index) { return uint64_t(ids[index]); }, stream)
            : selected;
        check(built == Status::OK && stream.size() == ids.size() &&
              stream.stats.carrier_payload_bytes == ids.size() * layout.record_bytes,
              "random graph construction publishes a correctly sized real carrier");
        if (built != Status::OK)
            continue;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            std::size_t gap = 1;
            while (ids[(index + gap) % ids.size()] / 16 != ids[index] / 16)
                ++gap;
            DecodedRecord decoded;
            check(decodeRecord(layout, stream.word(index), decoded) == Status::OK &&
                  decoded.destination == ids[index] && decoded.distance >= gap &&
                  decoded.state == (index + gap < ids.size() ? State::FINITE : State::WRAP),
                  "constructed next-line references agree with an independent cyclic scan");
        }
    }
    auto req = requirements(uint64_t{1} << 32);
    req.record_count = 3;
    const std::vector<uint64_t> ids = {UINT32_MAX, 0, UINT32_MAX};
    const auto getter = [&ids](std::size_t index) { return ids[index]; };
    Layout layout;
    RecordStream stream;
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords(req, layout, 16, getter, stream) == Status::OK &&
          stream.records32.empty() && stream.records64.size() == 3 &&
          stream.stats.carrier_payload_bytes == 24 &&
          stream.stats.source_stream_bytes == 12 && stream.stats.property_lines == 2 &&
          stream.stats.auxiliary_peak_bytes < 4096,
          "sparse high IDs do not allocate a dense vertex-sized future table");
    BuildLimits limits;
    limits.maximum_carrier_bytes = 16;
    check(buildRecords(req, layout, 16, getter, stream, limits) ==
              Status::RESOURCE_LIMIT && stream.size() == 0,
          "carrier allocation is bounded before construction");
    limits.maximum_carrier_bytes = 1024;
    limits.maximum_auxiliary_bytes = 1;
    check(buildRecords(req, layout, 16, getter, stream, limits) ==
              Status::RESOURCE_LIMIT && stream.size() == 0,
          "temporary allocation failure publishes no partial carrier");
    check(buildRecords(req, layout, 16,
              [&req](std::size_t) { return req.vertex_count; }, stream) ==
              Status::INVALID_ID && stream.size() == 0,
          "out-of-domain source IDs fail without partial output");
    req = requirements(uint64_t{1} << 33);
    req.record_count = 1;
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords(req, layout, 16,
              [](std::size_t) { return uint64_t{1} << 32; }, stream) ==
              Status::INVALID_ID,
          "a claimed four-byte source cannot hide a wider source ID");
    req = requirements(32);
    req.record_count = 100000;
    limits.maximum_carrier_bytes = 800000;
    limits.maximum_auxiliary_bytes = 4096;
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords(req, layout, 16,
              [](std::size_t index) { return uint64_t(index % 2) * 16; },
              stream, limits) == Status::OK &&
          stream.stats.auxiliary_peak_bytes < 4096,
          "temporary storage scales with referenced lines, not edge count");
}

void testWindowCaptureReadinessAndPinning() {
    using namespace ecg_record;
    for (const uint8_t bytes : {uint8_t{4}, uint8_t{8}}) {
        auto req = requirements(32);
        req.record_count = 16;
        req.requested_record_bytes = bytes;
        Layout layout;
        WindowBuffer buffer;
        const uint64_t base = 0x1040 - bytes;
        check(selectLayout(req, layout) == Status::OK &&
              buffer.configure(layout, base, 16) == Status::OK &&
              buffer.bankCount() == (bytes == 4 ? 2u : 3u) &&
              buffer.pin(base) == Status::OK,
              "the buffer pins every line of an unaligned 4/8-byte window");
        std::array<std::array<uint8_t, 64>, 3> lines{};
        for (unsigned index = 0; index < 16; ++index) {
            uint64_t word = 0;
            encodeRecord(layout, index, 4, State::FINITE, word);
            const uint64_t offset = base + index * bytes - 0x1000;
            std::memcpy(lines[offset / 64].data() + offset % 64, &word, bytes);
        }
        for (unsigned index = 0; index < buffer.bankCount(); ++index)
            check(buffer.storeLine(0x1000 + index * 64, 0x8000 + index * 64,
                                   lines[index].data(), 100) == Status::OK,
                  "real line capture retains an explicit availability time");
        RecordWindow window;
        check(buffer.readWindow(base, 32, 99, window) == Status::NOT_READY,
              "host-visible fill bytes cannot be consumed before modeled data arrival");
        check(buffer.readWindow(base, 32, 100, window) == Status::OK &&
              window.valid_mask == UINT16_MAX,
              "all actual words become available at the paid capture boundary");
        check(buffer.updatePhysical(0x8000, lines[0].data(), 110) == Status::OK &&
              buffer.readWindow(base, 32, 109, window) == Status::NOT_READY &&
              buffer.readWindow(base, 32, 110, window) == Status::OK,
              "real cache data updates preserve their own capture latency");
        check(buffer.storeLine(0x2000, 0x9000, lines[0].data(), 101) == Status::NOT_READY,
              "unrelated fills cannot evict pinned live window lines");
        check(buffer.storeLine(0x1000, 0xA000, lines[0].data(), 101) == Status::INVALID_ADDRESS,
              "a buffered record line cannot silently change physical identity");
        buffer.unpin();
        check(buffer.storeLine(0x2000, 0x9000, lines[0].data(), 101) == Status::OK,
              "completed windows release their finite bank capacity");
        const uint64_t high_base = UINT64_MAX - 16 * bytes + 1;
        check(buffer.configure(layout, high_base, 16) == Status::OK &&
              buffer.pin(high_base) == Status::OK &&
              buffer.lastLine() == UINT64_MAX - 63,
              "last-line calculation never wraps a valid top-of-address-space window");
    }
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
    configuration.control = kNativeEnable | kNativeHasNext;
    check(selectLayout(req, layout) == Status::OK &&
          packLayout(layout, configuration.layout_descriptor) == Status::OK,
          "native configuration keeps layout separate from graph counts");
    NativeRecordAccess access;
    check(nativeRecordAccess(configuration, 0x1008, 8, access) == Status::OK &&
          access.index == 1 && access.sequence == (uint64_t{1} << 32) + 2,
          "native sequence derives from the real record address without truncation");
    check(nativeRecordAccess(configuration, 0x1008, 4, access) == Status::INVALID_WIDTH,
          "a narrow opcode cannot silently load half a wide record");
    uint64_t word = 0;
    NativeLoadResult record, property;
    check(encodeRecord(layout, UINT32_MAX, 2, State::WRAP, word) == Status::OK &&
          nativeRecordResult(configuration, 0x1008, word, record) == Status::OK &&
          nativePropertyAccess(configuration, configuration.property_base,
                               word, 0x1008, property) == Status::OK &&
          record.raw_record == property.raw_record &&
          record.record_address == property.record_address &&
          record.destination == property.destination && record.sequence == property.sequence &&
          record.context == property.context && record.generation == property.generation &&
          record.layout_descriptor == property.layout_descriptor &&
          property.state == State::FINITE &&
          property.property_address == 0x80000000ULL + UINT32_MAX * uint64_t{4} &&
          property.deadline == property.sequence + 2,
          "raw word, address, destination, sequence and context bind native operations");
    check(nativePropertyAccess(configuration, configuration.property_base + 4,
                               word, 0x1008, property) == Status::INVALID_ADDRESS,
          "an ungoverned property base fails closed");
    NativeLoadResult final_pass, written;
    configuration.control = kNativeEnable;
    check(nativePropertyAccess(configuration, configuration.property_base,
                               word, 0x1008, final_pass) == Status::OK &&
          final_pass.state == State::DEAD && final_pass.deadline == 0,
          "a WRAP bound with no later pass retires its line");
    configuration.control = kNativeEnable | kNativeWrittenInPlace;
    check(nativePropertyAccess(configuration, configuration.property_base,
                               word, 0x1008, written) == Status::OK &&
          written.state == State::FINITE && written.deadline == written.sequence + 2 &&
          !written.has_next_iteration,
          "an in-place written region keeps a final-pass WRAP bound as before another pass");
    configuration.control = 0;
    check(validateNativeConfiguration(configuration, layout) == Status::INVALID_LAYOUT,
          "an inactive descriptor is not a native context");
    configuration.control = kNativeEnable;
    configuration.iteration_base = UINT64_MAX - 3;
    check(validateNativeConfiguration(configuration, layout) == Status::ARITHMETIC_OVERFLOW,
          "native configuration reserves deadline headroom beyond iteration end");
    configuration.iteration_base = 0;
    configuration.record_count = uint64_t{1} << 40;
    check(validateNativeConfiguration(configuration, layout) != Status::OK,
          "native counts cannot outgrow their resolved horizon");
}

void testSharedRuntimeStateAndQueue() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 8;
    Layout layout;
    Receiver receiver;
    check(selectLayout(req, layout) == Status::OK &&
          receiver.configure(layout, 8, 1, 7) && receiver.valueBits() == 64,
          "runtime configuration accounts for full-width per-line state");
    Receiver inconsistent;
    check(!inconsistent.configure(layout, 32, 1, 7),
          "receiver counts must match the resolved layout horizon");
    LineMetadata line;
    line.prefetch_origin = true;
    check(receiver.observe(line, 1, 7, 3) == ObservationResult::ACCEPTED &&
          !receiver.watermarkValid() && line.state == LineState::PENDING,
          "an observation never freely advances the replacement watermark");
    CommitUpdate update;
    update.context = 1;
    update.generation = 7;
    update.state = State::FINITE;
    update.sequence = 1;
    update.deadline = 5;
    check(receiver.apply(&line, update) == ApplyResult::STALE &&
          receiver.watermark() == 1 && line.value == 3 && line.prefetch_origin,
          "older delivery cannot overwrite newer pending state or change prefetch origin");
    update.sequence = 3;
    update.deadline = 10;
    check(receiver.apply(&line, update) == ApplyResult::APPLIED &&
          line.state == LineState::FINITE && line.value == 10 && line.prefetch_origin,
          "committed metadata updates prediction state without touching ordinary recency");
    update.generation = 8;
    update.sequence = 4;
    check(receiver.apply(&line, update) == ApplyResult::INVALID_CONTEXT &&
          receiver.watermark() == 3,
          "another generation cannot modify the active receiver");
    update.generation = 7;
    update.sequence = (uint64_t{1} << 40) + 4;
    update.deadline = update.sequence + 4;
    check(receiver.apply(&line, update) == ApplyResult::APPLIED &&
          receiver.watermark() == update.sequence && line.value == update.deadline,
          "coalesced deliveries retain wide positions even across multiple traversals");
    check(receiver.observe(line, 1, 7, update.sequence + 9) ==
              ObservationResult::IGNORED_HORIZON,
          "speculative observation remains bounded to one traversal");
    ++update.sequence;
    check(receiver.apply(nullptr, update) == ApplyResult::NOT_RESIDENT &&
          receiver.watermark() == update.sequence,
          "paid nonresident delivery still advances semantic progress");

    CommitQueue queue(8, 2);
    update.sequence = (uint64_t{1} << 32) + 1;
    update.deadline = update.sequence + 10;
    check(queue.enqueue(update, 0) == EnqueueStatus::ENQUEUED,
          "wide updates occupy a real capture lane");
    ++update.sequence;
    check(queue.enqueue(update, 0) == EnqueueStatus::ENQUEUED,
          "a second version occupies its own physical slot");
    ++update.sequence;
    check(queue.enqueue(update, 1) == EnqueueStatus::COALESCED &&
          queue.pendingSize() == 2, "coalescing protects the oldest version");
    const auto oldest = queue.popReady(8);
    const auto newest = queue.popReady(9);
    check(oldest.status == PopStatus::POPPED &&
          oldest.ready.update.sequence == (uint64_t{1} << 32) + 1 &&
          newest.status == PopStatus::POPPED &&
          newest.ready.update.sequence == update.sequence && queue.empty(),
          "queue transport preserves wide data, latency and output ordering");
    update.sequence_bits = 32;
    check(!CommitUpdateTraits::valid(update),
          "current transport does not accept a silently narrowed sequence");

    const uint64_t current = (uint64_t{1} << 40) + 10;
    WayState ways[2];
    ways[0].property = ways[1].property = true;
    ways[0].state = State::UNKNOWN;
    ways[0].deadline = current + 2;
    ways[0].rrpv = 7;
    ways[0].grasp_tier = ways[1].grasp_tier = 1;
    ways[1].state = State::FINITE;
    ways[1].deadline = current + 100;
    std::size_t victim = 99;
    check(selectVictim(layout, ways, 2, current, victim) == Status::OK && victim == 0,
          "a pending semantic sequence is not interpreted as a finite deadline");
    const bool valid[2] = {true, true};
    bool admit = false;
    check(canAdmitPrefetch(layout, ways, valid, 2, current, admit) == Status::OK && admit,
          "prefetch admission retains score-seven fallback");
    ways[0].state = State::FINITE;
    ways[0].deadline = current + 1;
    check(canAdmitPrefetch(layout, ways, valid, 2, current, admit) == Status::OK && !admit,
          "prefetch admission does not invent a near-future threshold");
}

void testFilteredConstructionBudgetAndSemantics() {
    using namespace ecg_record;
    auto req = requirements(64);
    req.record_count = 16;
    const uint32_t ids[] = {0, 1, 7, 8, 0, 9, 7, 8, 31, 32, 63, 31, 32, 63, 31, 32};
    const PropertyDescriptor property{PropertyKind::U64, 8, TraversalMode::ORDERED_FILTERED};
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        req.requested_record_bytes = width;
        Layout layout;
        check(selectLayout(req, layout) == Status::OK, "filtered compact fixture layout");
        BuildLimits limits;
        limits.maximum_auxiliary_bytes = 9 * 2 * sizeof(uint64_t);
        uint64_t source_reads = 0;
        RecordStream stream;
        const auto scope = [](uint64_t index) {
            return BuildScope{static_cast<uint8_t>(index % 2), index < 8 ? 8u : 16u};
        };
        const Status status = buildRecords<2>(req, layout, property, 0x1008,
            [&](uint64_t index) { ++source_reads; return ids[index]; },
            stream, limits, UnobservedConstruction{}, scope);
        check(status == Status::OK && source_reads == req.record_count &&
              stream.stats.auxiliary_peak_bytes == limits.maximum_auxiliary_bytes,
              "filtered records need one reverse source scan and only dense next positions");
        if (status != Status::OK)
            continue;
        for (uint64_t index = 0; index < req.record_count; ++index) {
            uint64_t next = index + 1;
            while (next < scope(index).end &&
                   (scope(next).partition != scope(index).partition ||
                    (0x1008 + ids[next] * 8) / 64 != (0x1008 + ids[index] * 8) / 64))
                ++next;
            uint64_t expected = 0;
            check(encodeRecord(layout, ids[index], next < scope(index).end ? next - index : 0,
                      next < scope(index).end ? State::FINITE : State::UNKNOWN, expected) == Status::OK &&
                  stream.word(index) == expected,
                  "compact construction preserves scoped finite references and neutral terminal state");
        }
        RecordWindow old_window, new_window;
        old_window.remaining_records = new_window.remaining_records = 16;
        old_window.vertex_count = new_window.vertex_count = req.vertex_count;
        old_window.valid_mask = new_window.valid_mask = UINT16_MAX;
        check(buildRecords<2>(req, layout, property, 0x1008,
                  [&](uint64_t index) { return ids[index]; }, stream, limits,
                  UnobservedConstruction{},
                  [](uint64_t index) { return BuildScope{static_cast<uint8_t>(index % 2), UINT64_MAX}; }) ==
                  Status::OK, "unbounded filtered next-only construction");
        for (uint64_t index = 0; index < req.record_count; ++index) {
            uint64_t gap = 1;
            while ((index + gap) % 2 != index % 2 ||
                   (0x1008 + ids[(index + gap) % 16] * 8) / 64 != (0x1008 + ids[index] * 8) / 64)
                ++gap;
            const State old_state = index + gap < 16 ? State::FINITE : State::WRAP;
            check(encodeRecord(layout, ids[index], gap, old_state, old_window.words[index]) ==
                  Status::OK, "reference cyclic token");
            new_window.words[index] = stream.word(index);
            for (bool has_next : {false, true}) {
                Prediction old_prediction, new_prediction;
                check(makePrediction(layout, old_window.words[index], index + 1, has_next,
                          old_prediction, TraversalMode::ORDERED_FILTERED) == Status::OK &&
                      makePrediction(layout, new_window.words[index], index + 1, has_next,
                          new_prediction, TraversalMode::ORDERED_FILTERED) == Status::OK &&
                      old_prediction.state == new_prediction.state &&
                      old_prediction.deadline == new_prediction.deadline &&
                      old_prediction.normalized_record == new_prediction.normalized_record,
                      "discarding filtered wrap construction preserves delivered predictions");
            }
        }
        PrefetchTarget old_target, new_target;
        check(selectWindowTarget(layout, old_window, property, 0x1008, old_target) == Status::OK &&
              selectWindowTarget(layout, new_window, property, 0x1008, new_target) == Status::OK &&
              old_target.valid == new_target.valid && old_target.lead == new_target.lead &&
              old_target.destination == new_target.destination,
              "next-only construction preserves filtered record-window target selection");
    }
    req = requirements(uint64_t{UINT32_MAX} + 1);
    req.record_count = 3;
    Layout layout;
    RecordStream stream;
    BuildLimits limits;
    limits.maximum_auxiliary_bytes = 4096;
    uint64_t reads = 0;
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords<2>(req, layout, property, 0,
              [&](uint64_t index) { ++reads; return index == 1 ? uint64_t{UINT32_MAX} : 0; },
              stream, limits, UnobservedConstruction{},
              [](uint64_t index) { return BuildScope{static_cast<uint8_t>(index % 2), UINT64_MAX}; }) ==
              Status::OK && reads == 3 && stream.stats.property_lines == 2 &&
          stream.stats.auxiliary_peak_bytes <= limits.maximum_auxiliary_bytes,
          "sparse high-VID domains retain a bounded next-only hash representation");
}

void testTypedPropertiesAndFilteredPasses() {
    using namespace ecg_record;
    PropertyDescriptor property;
    uint64_t descriptor = 0, address = 0, extent = 0;
    check(unpackProperty(0, property) == Status::OK &&
          property.kind == PropertyKind::F32 && property.stride_bytes == 4 &&
          property.traversal == TraversalMode::DENSE_EXACT,
          "the absent property descriptor preserves the original F32 PR contract");
    property = {PropertyKind::U64, 16, TraversalMode::ORDERED_FILTERED};
    PropertyDescriptor restored;
    check(packProperty(property, descriptor) == Status::OK &&
          unpackProperty(descriptor, restored) == Status::OK && restored == property &&
          propertyAddress(property, 0x1008, 3, address) == Status::OK && address == 0x1038 &&
          propertyExtent(property, 4, extent) == Status::OK && extent == 56,
          "typed properties retain their real width, stride and unaligned line base");
    check(unpackProperty(descriptor | (uint64_t{1} << 63), restored) != Status::OK,
          "reserved property descriptor bits fail closed");
    property.stride_bytes = 4;
    check(packProperty(property, descriptor) == Status::INVALID_WIDTH,
          "an eight-byte property cannot overlap its next element");
    property.stride_bytes = 16;
    check(propertyAddress(property, UINT64_MAX - 7, 1, address) ==
              Status::ARITHMETIC_OVERFLOW,
          "typed property address overflow fails before a memory request");

    auto req = requirements(17);
    req.record_count = 4;
    Layout layout;
    RecordStream stream;
    const uint32_t ids[] = {0, 7, 8, 0};
    property = {PropertyKind::U64, 8, TraversalMode::ORDERED_FILTERED};
    check(selectLayout(req, layout) == Status::OK &&
          buildRecords(req, layout, property, 0x1008,
              [&](std::size_t i) { return ids[i]; }, stream) == Status::OK,
          "record construction groups the actual strided property cache lines");
    DecodedRecord decoded;
    check(decodeRecord(layout, stream.word(0), decoded) == Status::OK &&
          decoded.state == State::FINITE && decoded.distance == 3,
          "a non-line-aligned base does not falsely join VID zero with VID seven");
    check(decodeRecord(layout, stream.word(1), decoded) == Status::OK &&
          decoded.state == State::FINITE && decoded.distance == 1,
          "VID seven and eight share the next actual eight-byte property line");
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    packProperty(property, configuration.property_descriptor);
    configuration.record_base = 0x2000;
    configuration.property_base = 0x1008;
    configuration.record_count = 4;
    configuration.vertex_count = 17;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable | kNativeHasNext;
    NativeLoadResult load;
    check(nativePropertyAccess(configuration, 0x1008, stream.word(1),
              0x2000 + layout.record_bytes, load) == Status::OK &&
          load.property_address == 0x1040 && load.property_bytes == 8 &&
          load.property_kind == PropertyKind::U64 && load.state == State::FINITE,
          "the native record association generates an integer doubleword address");
    check(nativePropertyAccess(configuration, 0x1008, stream.word(3),
              0x2000 + 3 * layout.record_bytes, load) == Status::OK &&
          load.state == State::UNKNOWN && load.deadline == 0,
          "filtered WRAP never claims another traversal or DEAD liveness");

    PassCursor cursor;
    check(cursor.configure(4, TraversalMode::ORDERED_FILTERED) == Status::OK &&
          cursor.begin() == Status::OK && cursor.consume(1) == Status::OK &&
          cursor.consume(3) == Status::OK && cursor.consume(2) == Status::INVALID_SEQUENCE &&
          cursor.close() == Status::OK && cursor.base() == 4 &&
          cursor.consumed() == 2 && cursor.skipped() == 2,
          "filtered positions increase and a close accounts for first/middle/tail skips");
    check(cursor.begin() == Status::OK && cursor.close() == Status::OK &&
          cursor.base() == 8 && cursor.passes() == 2 && cursor.skipped() == 6 &&
          cursor.consumed() + cursor.skipped() == cursor.base(),
          "an empty filtered pass advances only the internally computed structural span");
    PassCursor exact;
    check(exact.configure(4, TraversalMode::DENSE_EXACT) == Status::OK &&
          exact.begin() == Status::OK && exact.consume(1) == Status::INVALID_SEQUENCE &&
          exact.consume(0) == Status::OK && exact.close() == Status::INVALID_SEQUENCE,
          "dense exact traversal still rejects omitted records and premature close");
}

void testFilteredReceiverInvalidations() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 8;
    Layout layout;
    Receiver receiver;
    check(selectLayout(req, layout) == Status::OK &&
          receiver.configure(layout, 8, 1, 1, TraversalMode::ORDERED_FILTERED),
          "filtered receivers explicitly declare potential-reference semantics");
    LineMetadata line;
    CommitUpdate update;
    update.context = update.generation = 1;
    update.sequence = 2;
    update.order = 1;
    update.deadline = 5;
    update.state = State::FINITE;
    check(receiver.observe(line, 1, 1, 2) == ObservationResult::ACCEPTED &&
          receiver.watermark() == 2 &&
          receiver.apply(&line, update) == ApplyResult::APPLIED,
          "filtered structural progress is independent of delivered event order");
    check(receiver.advanceProgress(1, 1, 8) == ObservationResult::ACCEPTED &&
          resolveFuture(State::FINITE, line.value, receiver.watermark()).state == State::UNKNOWN,
          "skipping a potential occurrence expires its prediction without inventing DEAD");
    update.sequence = 4;
    update.order = 2;
    update.deadline = 6;
    check(receiver.apply(&line, update) == ApplyResult::EXPIRED,
          "delayed metadata cannot revive a deadline behind structural progress");
    check(receiver.observe(line, 1, 1, 10) == ObservationResult::ACCEPTED &&
          receiver.invalidateObservation(line, 1, 1, 10) == ObservationResult::ACCEPTED,
          "ordinary governed-region traffic destroys pending potential liveness");
    update.sequence = 10;
    update.order = 3;
    update.deadline = 15;
    check(receiver.apply(&line, update) == ApplyResult::STALE &&
          line.state == LineState::UNKNOWN,
          "a queued designated update cannot resurrect an ordinary-invalidated line");
    update.order = 4;
    update.invalidate = true;
    update.state = State::UNKNOWN;
    update.deadline = 0;
    CommitQueue queue;
    check(queue.enqueue(update, 0) == EnqueueStatus::ENQUEUED &&
          receiver.apply(&line, queue.popReady(8).ready.update) == ApplyResult::APPLIED,
          "ordinary invalidation uses the same paid bounded queue");
    update.order = 5;
    update.invalidate = false;
    update.sequence = 11;
    update.state = State::FINITE;
    update.deadline = 16;
    check(receiver.apply(&line, update) == ApplyResult::APPLIED &&
          line.state == LineState::FINITE && line.value == 16,
          "a later designated use can install a new potential-reference prediction");
    update.order = 6;
    update.state = State::DEAD;
    check(receiver.apply(&line, update) == ApplyResult::INVALID_ORDER,
          "filtered receivers reject a sender that claims unproven DEAD liveness");
}

void testOlderInvalidationPreservesNewerUse() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 8;
    Layout layout;
    selectLayout(req, layout);
    for (const bool reaches_llc : {false, true}) {
        Receiver receiver;
        receiver.configure(layout, 8, 1, 1, TraversalMode::ORDERED_FILTERED);
        LineMetadata line;
        receiver.observe(line, 1, 1, 1);
        receiver.invalidateObservation(line, 1, 1, 1);
        if (reaches_llc)
            receiver.observe(line, 1, 1, 2);
        else
            receiver.advanceProgress(1, 1, 2);
        CommitUpdate invalidation;
        invalidation.context = invalidation.generation = invalidation.order = 1;
        invalidation.sequence = 1;
        invalidation.invalidate = true;
        const auto applied = receiver.apply(&line, invalidation);
        check(applied == ApplyResult::APPLIED || applied == ApplyResult::STALE,
              "an older ordinary invalidation remains a valid delivered event");
        CommitUpdate later;
        later.context = later.generation = 1;
        later.order = later.sequence = 2;
        later.state = State::FINITE;
        later.deadline = 7;
        check(receiver.apply(&line, later) == ApplyResult::APPLIED &&
              line.state == LineState::FINITE && line.value == 7,
              "older invalidation must not poison a newer private-hit or LLC-observed use");
    }
}

void testUnknownPredictionIsNeutralToLru() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 8;
    Layout layout;
    selectLayout(req, layout);
    WayState ways[3];
    ways[0].property = true;
    ways[0].state = State::UNKNOWN;
    ways[0].recency = 1;
    ways[0].grasp_tier = 1;
    ways[1].recency = 100;
    ways[2].recency = 101;
    std::size_t selected = 99;
    check(selectVictim(layout, ways, 3, 20, selected) == Status::OK && selected == 0,
          "UNKNOWN property metadata does not pin data ahead of hot non-property lines");
    ways[0].state = State::FINITE;
    ways[0].deadline = 19;
    check(selectVictim(layout, ways, 3, 20, selected) == Status::OK && selected == 0,
          "expired metadata returns to the same LRU victim");
    ways[0].deadline = 21;
    const bool valid[] = {true, true, true};
    bool admit = true;
    check(canAdmitPrefetch(layout, ways, valid, 3, 20, admit) == Status::OK && !admit,
          "prefetch admission considers its real victim, not an unrelated non-property way");
    ways[1].property = true;
    ways[1].state = State::FINITE;
    ways[1].deadline = 27;
    check(selectVictim(layout, ways, 3, 20, selected) == Status::OK && selected == 1,
          "a live finite LRU candidate may yield to a farther known finite property");
    ways[0].property = false;
    check(selectVictim(layout, ways, 3, 20, selected) == Status::OK && selected == 0,
          "known property futures do not displace an unknown LRU baseline victim");
}

void testSelectedBaseVictimRefinement() {
    using namespace ecg_record;
    for (const uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        auto req = requirements(32);
        req.record_count = 8;
        req.requested_record_bytes = width;
        Layout layout;
        check(selectLayout(req, layout) == Status::OK &&
              layout.record_bytes == width,
              "base-victim refinement covers both record widths");
        WayState ways[3];
        for (auto& way : ways)
            way.property = true;
        ways[0].state = State::UNKNOWN;
        ways[0].recency = 1;
        ways[1].state = State::FINITE;
        ways[1].deadline = 40;
        ways[1].recency = 2;
        ways[2].state = State::FINITE;
        ways[2].deadline = 30;
        ways[2].recency = 3;
        std::size_t selected = 99;
        unsigned calls = 0;
        auto select_base = [&]() {
            ++calls;
            return std::size_t{0};
        };
        check(selectVictim(layout, ways, 3, 20, select_base, selected) ==
                  Status::OK && selected == 0 && calls == 1,
              "UNKNOWN metadata preserves the selected base victim");
        ways[0].state = State::FINITE;
        ways[0].deadline = 19;
        calls = 0;
        check(selectVictim(layout, ways, 3, 20, select_base, selected) ==
                  Status::OK && selected == 0 && calls == 1,
              "expired metadata preserves the selected base victim");
        ways[0].deadline = 25;
        calls = 0;
        check(selectVictim(layout, ways, 3, 20, select_base, selected) ==
                  Status::OK && selected == 1 && calls == 1,
              "a farther live finite property overrides an eligible base victim");
        ways[2].state = State::DEAD;
        ways[2].recency = 0;
        calls = 0;
        check(selectVictim(layout, ways, 3, 20, select_base, selected) ==
                  Status::OK && selected == 2 && calls == 0,
              "known-DEAD selection does not invoke or age the base policy");
        bool valid[] = {true, false, true};
        bool admit = false;
        calls = 0;
        check(canAdmitPrefetch(
                  layout, ways, valid, 3, 20, select_base, admit) ==
                  Status::OK && admit && calls == 0,
              "an invalid way admits without invoking the base policy");
        Layout invalid = layout;
        invalid.record_bytes = 3;
        calls = 0;
        check(selectVictim(invalid, ways, 3, 20, select_base, selected) ==
                  Status::INVALID_LAYOUT && calls == 0,
              "invalid configuration fails before invoking the base policy");
    }
}

void testFilteredQuantizedPassBoundary() {
    using namespace ecg_record;
    auto req = requirements(uint64_t{1} << 26);
    req.record_count = uint64_t{1} << 30;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK && layout.mantissa_bits == 0,
          "filtered boundary fixture exercises the coarse large-graph grammar");
    NativeConfiguration configuration;
    packLayout(layout, configuration.layout_descriptor);
    packProperty({PropertyKind::U32, 4, TraversalMode::ORDERED_FILTERED},
                 configuration.property_descriptor);
    configuration.record_base = 0x1000;
    configuration.property_base = 0x80000000;
    configuration.record_count = req.record_count;
    configuration.vertex_count = req.vertex_count;
    configuration.context = configuration.generation = 1;
    configuration.control = kNativeEnable | kNativeManagedPasses;
    const uint64_t distance = (uint64_t{1} << 28) + 1;
    const uint64_t sequence = req.record_count - distance - 1;
    uint64_t word = 0;
    encodeRecord(layout, 18, distance, State::FINITE, word);
    NativeLoadResult load;
    check(nativePropertyAccess(configuration, configuration.property_base, word,
              configuration.record_base + (sequence - 1) * layout.record_bytes, load) == Status::OK &&
          load.state == State::FINITE && load.deadline == req.record_count,
          "a coarse potential bound cannot extend beyond its closed structural pass");
    Receiver receiver;
    receiver.configure(layout, req.record_count, 1, 1, TraversalMode::ORDERED_FILTERED);
    CommitUpdate update;
    update.context = update.generation = update.order = 1;
    update.sequence = load.sequence;
    update.deadline = load.deadline;
    update.state = load.state;
    LineMetadata line;
    check(receiver.apply(&line, update) == ApplyResult::APPLIED &&
          receiver.advanceProgress(1, 1, req.record_count) == ObservationResult::ACCEPTED &&
          victimState(line, receiver, true) == State::UNKNOWN,
          "pass closure expires a last-position potential bound without free line scanning");
    check(nativePropertyAccess(configuration, configuration.property_base, word,
              configuration.record_base + (req.record_count - 1) * layout.record_bytes, load) ==
              Status::INVALID_RECORD,
          "the last filtered structural position cannot have a finite in-pass successor");
}

}  // namespace


// The attribution trace must be write-only: collecting it may never change a
// victim. Proven directly here, independent of any cache/memory layout, by
// running identical way states through selectVictim with and without a trace.
void testVictimTraceIsPassive() {
    using namespace ecg_record;
    auto req = requirements(1024);
    req.record_count = 64;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "trace passivity fixture layout");
    std::mt19937_64 rng(0xA77121B0);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int mismatches = 0, traced_paths[ecg_record::kVictimPathCount] = {};
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + (rng() % 16);
        const uint64_t sequence = rng() % 64;
        WayState ways[16];
        for (std::size_t i = 0; i < count; ++i) {
            ways[i].property = (rng() % 4) != 0;
            ways[i].rrpv = static_cast<uint8_t>(rng() % 8);
            ways[i].recency = rng() % 4096;
            ways[i].grasp_tier = static_cast<uint8_t>(rng() % 3);
            ways[i].state = states[rng() % 4];
            ways[i].deadline = rng() % 128;
        }
        WayState copy_a[16], copy_b[16];
        for (std::size_t i = 0; i < count; ++i) copy_a[i] = copy_b[i] = ways[i];
        const std::size_t base_way = rng() % count;
        const auto pick = [base_way]() { return base_way; };
        std::size_t victim_untraced = 0, victim_traced = 0;
        VictimTrace trace;
        const Status s1 = selectVictim(
            layout, copy_a, count, sequence, pick, victim_untraced, nullptr);
        const Status s2 = selectVictim(
            layout, copy_b, count, sequence, pick, victim_traced, &trace);
        if (s1 != s2 || victim_untraced != victim_traced) ++mismatches;
        // The rule must not mutate the ways it inspects either.
        for (std::size_t i = 0; i < count; ++i)
            if (copy_a[i].rrpv != ways[i].rrpv || copy_a[i].state != ways[i].state ||
                copy_a[i].deadline != ways[i].deadline || copy_b[i].rrpv != ways[i].rrpv ||
                copy_b[i].state != ways[i].state || copy_b[i].deadline != ways[i].deadline)
                ++mismatches;
        if (s2 == Status::OK) {
            ++traced_paths[static_cast<int>(trace.path)];
            const unsigned governed = trace.finite + trace.dead + trace.unknown;
            // `expired` is a subset of `unknown`, never an extra bucket.
            if (governed != trace.governed || trace.governed > count || trace.ways != count ||
                trace.expired > trace.unknown)
                ++mismatches;
        }
    }
    check(mismatches == 0,
          "collecting a victim trace never changes the victim, status or way state");
    // Two paths are opt-in and unreachable here, because this fixture never
    // enables governed-first and never reports a set as unpressured. Every
    // other path must be exercised. Stated as a set difference rather than a
    // hardcoded count, so adding a path later fails loudly instead of silently
    // shrinking what this check covers.
    const int opt_in[] = {static_cast<int>(VictimPath::UNGOVERNED_FIRST),
                          static_cast<int>(VictimPath::UNPRESSURED),
                          static_cast<int>(VictimPath::UNINFORMED_BASE),
                          static_cast<int>(VictimPath::NO_BOUND_COMPARE)};
    constexpr int kOptIn = static_cast<int>(sizeof(opt_in) / sizeof(opt_in[0]));
    int covered = 0, opt_in_seen = 0;
    for (int i = 0; i < kVictimPathCount; ++i) {
        const bool is_opt_in = std::find(opt_in, opt_in + kOptIn, i) != opt_in + kOptIn;
        if (is_opt_in) opt_in_seen += traced_paths[i];
        else covered += traced_paths[i] > 0;
    }
    check(covered == kVictimPathCount - kOptIn && opt_in_seen == 0,
          "the passivity fixture exercises every default victim path and no other");
}


// Opt-in governed-first eviction. Pins the order the repository's teaching
// fixture specifies, and re-proves passivity of the trace with the option on.
// The pressure gate must be a strict relaxation: when a set is not under
// pressure the rule hands back exactly what the base policy chose, and when it
// is, the rule is bit-for-bit what it was before the gate existed. Anything
// else would make every recorded result unreproducible.
void testPressureGateRelaxesToTheBaseVictim() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 34;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "pressure gate fixture layout");

    VictimOptions gov;       gov.governed_first = true;
    VictimOptions relaxed;   relaxed.governed_first = true;  relaxed.pressured = false;
    VictimOptions plain_relaxed;                             plain_relaxed.pressured = false;

    WayState ways[3];
    ways[0].property = true;  ways[0].recency = 1; ways[0].state = State::FINITE; ways[0].deadline = 19 + 64;
    ways[1].property = false; ways[1].recency = 9; ways[1].state = State::UNKNOWN;
    ways[2].property = true;  ways[2].recency = 5; ways[2].state = State::FINITE; ways[2].deadline = 19 + 8;

    std::size_t victim = 0;
    check(selectVictim(layout, ways, 3, 19, victim, nullptr, gov) == Status::OK && victim == 1,
          "pressured governed-first still evicts the non-governed way");
    // Relaxed: the base victim here is plain LRU, which is way 0 at recency 1.
    check(selectVictim(layout, ways, 3, 19, victim, nullptr, relaxed) == Status::OK && victim == 0,
          "unpressured relaxes to the base victim even with governed-first requested");
    check(selectVictim(layout, ways, 3, 19, victim, nullptr, plain_relaxed) == Status::OK && victim == 0,
          "unpressured also suppresses the strictly-farther override");

    // DEAD still precedes everything: a line known dead has no future to trade.
    ways[2].state = State::DEAD;
    check(selectVictim(layout, ways, 3, 19, victim, nullptr, relaxed) == Status::OK && victim == 2,
          "explicit DEAD is honoured even when the set is not under pressure");
    ways[2].state = State::FINITE;

    // The reported path must name the arm, so a receipt records which ran.
    VictimTrace trace;
    check(selectVictim(layout, ways, 3, 19, victim, &trace, relaxed) == Status::OK &&
          trace.path == VictimPath::UNPRESSURED,
          "the unpressured path is reported rather than inferred");

    // Default-constructed options must be pressured, or every existing caller
    // silently changes behaviour the moment this field appears.
    check(VictimOptions().pressured, "VictimOptions defaults to pressured");

    // Randomised equivalence: pressured must equal the pre-gate rule exactly,
    // and unpressured must equal the base victim exactly, on every shape.
    std::mt19937_64 rng(0x9E3779B97F4A7C15ull);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int relaxed_differed = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + (rng() % 16);
        WayState w[16];
        for (std::size_t i = 0; i < count; ++i) {
            w[i].property = (rng() & 1) != 0;
            w[i].recency = rng() % 64;
            w[i].state = states[rng() % 4];
            w[i].deadline = rng() % 256;
        }
        const uint64_t sequence = rng() % 128;
        std::size_t lru = 0;
        for (std::size_t i = 1; i < count; ++i)
            if (w[i].recency < w[lru].recency) lru = i;
        std::size_t dead = count;
        for (std::size_t i = 0; i < count; ++i)
            if (w[i].property && w[i].state == State::DEAD &&
                (dead == count || w[i].recency < w[dead].recency)) dead = i;

        VictimOptions pressured_opts; pressured_opts.governed_first = (rng() & 1) != 0;
        VictimOptions relaxed_opts = pressured_opts; relaxed_opts.pressured = false;

        std::size_t got_relaxed = 0;
        check(selectVictim(layout, w, count, sequence, got_relaxed, nullptr, relaxed_opts) ==
              Status::OK, "relaxed selection succeeds");
        const std::size_t expected = dead != count ? dead : lru;
        if (got_relaxed != expected) ++relaxed_differed;
    }
    check(relaxed_differed == 0,
          "unpressured selection is exactly DEAD-first then the base victim");
}

void testGovernedFirstEviction() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 34;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "governed-first fixture layout");
    VictimOptions on; on.governed_first = true;
    WayState ways[2];
    ways[0].property = true;  ways[0].recency = 1; ways[0].state = State::FINITE; ways[0].deadline = 19 + 64;
    ways[1].property = true;  ways[1].recency = 9; ways[1].state = State::FINITE; ways[1].deadline = 19 + 8;
    std::size_t victim = 0;
    check(selectVictim(layout, ways, 2, 19, victim) == Status::OK && victim == 0,
          "default evicts the farther live governed future");
    check(selectVictim(layout, ways, 2, 19, victim, nullptr, on) == Status::OK && victim == 0,
          "governed-first leaves an all-governed set to the ordinary rule");
    ways[1].property = false;
    check(selectVictim(layout, ways, 2, 19, victim) == Status::OK && victim == 0,
          "the default rule does not prefer non-governed data");
    check(selectVictim(layout, ways, 2, 19, victim, nullptr, on) == Status::OK && victim == 1,
          "governed-first evicts non-governed data before governed property");
    ways[0].state = State::DEAD;
    check(selectVictim(layout, ways, 2, 19, victim, nullptr, on) == Status::OK && victim == 0,
          "explicit DEAD still precedes non-governed data");
    // Two non-governed ways: the least recently used is taken, so the rule
    // carries no way-index bias.
    ways[0].property = false; ways[0].state = State::UNKNOWN; ways[0].recency = 7;
    ways[1].recency = 3;
    check(selectVictim(layout, ways, 2, 19, victim, nullptr, on) == Status::OK && victim == 1,
          "governed-first takes the least recently used non-governed way");
    // Passivity of the trace must still hold with the option enabled.
    std::mt19937_64 rng(0x604E12D1);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int mismatches = 0, saw_ungoverned = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + (rng() % 16);
        const uint64_t sequence = rng() % 64;
        WayState sample[16];
        for (std::size_t i = 0; i < count; ++i) {
            sample[i].property = (rng() % 4) != 0;
            sample[i].rrpv = static_cast<uint8_t>(rng() % 8);
            sample[i].recency = rng() % 4096;
            sample[i].state = states[rng() % 4];
            sample[i].deadline = rng() % 128;
        }
        const std::size_t base_way = rng() % count;
        const auto pick = [base_way]() { return base_way; };
        std::size_t a = 0, b = 0;
        VictimTrace trace;
        const Status s1 = selectVictim(layout, sample, count, sequence, pick, a, nullptr, on);
        const Status s2 = selectVictim(layout, sample, count, sequence, pick, b, &trace, on);
        if (s1 != s2 || a != b) ++mismatches;
        if (s2 == Status::OK && trace.path == VictimPath::UNGOVERNED_FIRST) {
            ++saw_ungoverned;
            if (sample[b].property) ++mismatches;
        }
    }
    check(mismatches == 0, "governed-first selection is unchanged by trace collection");
    check(saw_ungoverned > 0, "the governed-first fixture exercises the non-governed path");
}

// A literal copy of cache_sim's findVictimGRASP on bare RRPVs: take the first
// way at the maximum, otherwise age every way below it and look again.
std::size_t graspScanReference(uint8_t* rrpv, std::size_t count) {
    while (true) {
        for (std::size_t index = 0; index < count; ++index)
            if (rrpv[index] >= ecg_record::kVictimRrpvMax)
                return index;
        for (std::size_t index = 0; index < count; ++index)
            if (rrpv[index] < ecg_record::kVictimRrpvMax)
                ++rrpv[index];
    }
}

void applyAgeing(uint8_t* rrpv, std::size_t count, const ecg_record::VictimAgeing& ageing) {
    for (std::size_t index = 0; index < count; ++index)
        if ((ageing.ways >> index) & 1)
            rrpv[index] = static_cast<uint8_t>(rrpv[index] + ageing.amount);
}

// Under the RRPV order every choice the rule makes comes from the line's RRPV,
// the GRASP half of ECG's state, and none from recency. Each case is built so
// the recency order and the RRPV order disagree.
void testRrpvOrderNeverReadsRecency() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 34;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "RRPV order fixture layout");
    VictimOptions governed; governed.governed_first = true;
    VictimOptions governed_rrpv = governed; governed_rrpv.rrpv_order = true;
    VictimOptions rrpv; rrpv.rrpv_order = true;
    VictimAgeing ageing;
    VictimTrace trace;
    std::size_t victim = 0;
    unsigned base_calls = 0;
    const auto first_way = [&base_calls]() { ++base_calls; return std::size_t{0}; };

    // The ageing is reported, never applied by the rule, so a caller that
    // gives it nowhere to go has asked for a decision it cannot complete.
    WayState single[1];
    check(selectVictim(layout, single, 1, 19, victim, nullptr, rrpv) == Status::INVALID_COUNTS,
          "the RRPV order refuses a call with no ageing output");

    // Non-governed choice: way 0 is least recent, way 1 holds the higher RRPV.
    WayState ways[3];
    ways[0].rrpv = 3; ways[0].recency = 1;
    ways[1].rrpv = 6; ways[1].recency = 9;
    ways[2].property = true; ways[2].rrpv = 2; ways[2].recency = 5;
    ways[2].state = State::FINITE; ways[2].deadline = 19 + 8;
    check(selectVictim(layout, ways, 3, 19, victim, nullptr, governed) == Status::OK && victim == 0,
          "the recency order takes the least recent non-governed way");
    check(selectVictim(layout, ways, 3, 19, first_way, victim, &trace, governed_rrpv, &ageing) ==
              Status::OK && victim == 1 && trace.path == VictimPath::UNGOVERNED_FIRST &&
              trace.rrpv_changed,
          "the RRPV order takes the non-governed way GRASP would evict");
    check(ageing.amount == 1 && ageing.ways == 0b011,
          "only the non-governed candidates age, by what the GRASP scan would add");

    // DEAD ties: the higher RRPV first, then the lower way; nothing ages.
    WayState dead[3];
    dead[0].property = true; dead[0].state = State::DEAD; dead[0].rrpv = 2; dead[0].recency = 1;
    dead[1].property = true; dead[1].state = State::DEAD; dead[1].rrpv = 5; dead[1].recency = 9;
    dead[2].rrpv = 7; dead[2].recency = 4;
    check(selectVictim(layout, dead, 3, 19, victim) == Status::OK && victim == 0,
          "the recency order takes the least recent DEAD way");
    check(selectVictim(layout, dead, 3, 19, first_way, victim, &trace, rrpv, &ageing) ==
              Status::OK && victim == 1 && trace.path == VictimPath::DEAD_FIRST &&
              trace.rrpv_changed && ageing.amount == 0 && ageing.ways == 0,
          "the RRPV order takes the DEAD way with the higher RRPV and ages nothing");
    dead[0].rrpv = 5; dead[0].recency = 9; dead[1].recency = 1;
    check(selectVictim(layout, dead, 3, 19, first_way, victim, nullptr, rrpv, &ageing) ==
              Status::OK && victim == 0,
          "equal RRPV among DEAD ways resolves to the lower way");

    // Override ties: way 0 is the GRASP base victim with the nearest bound;
    // ways 1 and 2 carry the same farther bound.
    WayState tie[3];
    for (auto& way : tie) { way.property = true; way.state = State::FINITE; }
    tie[0].rrpv = 7; tie[0].recency = 5; tie[0].deadline = 19 + 8;
    tie[1].rrpv = 3; tie[1].recency = 1; tie[1].deadline = 19 + 64;
    tie[2].rrpv = 5; tie[2].recency = 9; tie[2].deadline = 19 + 64;
    check(selectVictim(layout, tie, 3, 19, first_way, victim) == Status::OK && victim == 1,
          "the recency order breaks an equal-bound override by recency");
    check(selectVictim(layout, tie, 3, 19, first_way, victim, &trace, rrpv, &ageing) ==
              Status::OK && victim == 2 && trace.path == VictimPath::OVERRIDDEN &&
              trace.rrpv_changed,
          "the RRPV order breaks an equal-bound override by RRPV");
    tie[1].rrpv = 5; tie[1].recency = 9; tie[2].recency = 1;
    check(selectVictim(layout, tie, 3, 19, first_way, victim, nullptr, rrpv, &ageing) ==
              Status::OK && victim == 1,
          "equal RRPV and equal bound resolve to the lower way");

    // The base victim is the GRASP scan's, whatever the base callback says:
    // way 1 holds the maximum, the callback names way 0, and the set ages by
    // the two steps the scan would take.
    WayState plain[3];
    plain[0].rrpv = 1; plain[0].recency = 1;
    plain[1].rrpv = 5; plain[1].recency = 7;
    plain[2].rrpv = 5; plain[2].recency = 3;
    base_calls = 0;
    check(selectVictim(layout, plain, 3, 19, first_way, victim, &trace, rrpv, &ageing) ==
              Status::OK && victim == 1 && trace.path == VictimPath::BASE_NOT_GOVERNED &&
              ageing.amount == 2 && ageing.ways == 0b111 && base_calls == 0,
          "the RRPV order takes and ages as the GRASP scan, never the base callback");
    VictimOptions relaxed = rrpv; relaxed.pressured = false;
    check(selectVictim(layout, plain, 3, 19, first_way, victim, &trace, relaxed, &ageing) ==
              Status::OK && victim == 1 && trace.path == VictimPath::UNPRESSURED &&
              ageing.amount == 2 && ageing.ways == 0b111 && base_calls == 0,
          "an unpressured set under the RRPV order is the GRASP scan too");

    // The literal property: rewriting every recency changes nothing, on any
    // shape and in any arm, and a trace never changes the result.
    std::mt19937_64 rng(0x52525056);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int differed = 0, changed = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + (rng() % 16);
        const uint64_t sequence = rng() % 64;
        WayState sample[16], rewritten[16];
        for (std::size_t i = 0; i < count; ++i) {
            sample[i].property = (rng() % 4) != 0;
            sample[i].rrpv = static_cast<uint8_t>(rng() % 8);
            sample[i].recency = rng() % 4096;
            sample[i].state = states[rng() % 4];
            sample[i].deadline = rng() % 128;
            rewritten[i] = sample[i];
            rewritten[i].recency = rng() % 4096;
        }
        VictimOptions options; options.rrpv_order = true;
        options.governed_first = (rng() & 1) != 0;
        options.pressured = (rng() & 3) != 0;
        std::size_t a = 0, b = 0, c = 0;
        VictimAgeing age_a, age_b, age_c;
        VictimTrace traced;
        const Status s1 = selectVictim(layout, sample, count, sequence, first_way, a,
                                       nullptr, options, &age_a);
        const Status s2 = selectVictim(layout, rewritten, count, sequence, first_way, b,
                                       nullptr, options, &age_b);
        const Status s3 = selectVictim(layout, sample, count, sequence, first_way, c,
                                       &traced, options, &age_c);
        if (s1 != Status::OK || s2 != s1 || s3 != s1 || a != b || a != c ||
            age_a.ways != age_b.ways || age_a.amount != age_b.amount ||
            age_a.ways != age_c.ways || age_a.amount != age_c.amount)
            ++differed;
        changed += traced.rrpv_changed;
    }
    check(differed == 0, "under the RRPV order no victim or ageing depends on recency");
    check(changed > 0, "the fixture reaches choices the recency order would make differently");
    check(base_calls == 0, "the RRPV order never consults the base callback");
}

// With no governed way in the set ECG has no bound to offer, and the RRPV
// order must then be exactly GRASP: the same victim and the same ageing, in
// every arm. This is what makes ECG at worst GRASP rather than at worst LRU.
void testRrpvOrderIsGraspWithoutGovernedWays() {
    using namespace ecg_record;
    auto req = requirements(1024);
    req.record_count = 64;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "GRASP parity fixture layout");
    std::mt19937_64 rng(0x6A5F0A11);
    int differed = 0, aged = 0;
    unsigned base_calls = 0;
    const auto first_way = [&base_calls]() { ++base_calls; return std::size_t{0}; };
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + (rng() % 16);
        WayState ways[16];
        uint8_t reference[16], ordered[16];
        for (std::size_t i = 0; i < count; ++i) {
            ways[i].rrpv = static_cast<uint8_t>(rng() % 8);
            ways[i].recency = rng() % 4096;
            reference[i] = ordered[i] = ways[i].rrpv;
        }
        VictimOptions options; options.rrpv_order = true;
        options.governed_first = (rng() & 1) != 0;
        options.pressured = (rng() & 3) != 0;
        std::size_t victim = 0;
        VictimAgeing ageing;
        const std::size_t expected = graspScanReference(reference, count);
        if (selectVictim(layout, ways, count, rng() % 64, first_way, victim, nullptr,
                         options, &ageing) != Status::OK || victim != expected) {
            ++differed;
            continue;
        }
        applyAgeing(ordered, count, ageing);
        for (std::size_t i = 0; i < count; ++i)
            if (ordered[i] != reference[i]) ++differed;
        aged += ageing.amount != 0;
    }
    check(differed == 0, "with no governed way the RRPV order is the GRASP scan exactly");
    check(aged > 0, "the parity fixture exercises ageing");
    check(base_calls == 0, "the parity fixture never consults the base callback");
}

// The base decision as the rule takes it: under the RRPV order the GRASP scan
// over every way and its ageing, otherwise the base callback's way.
std::size_t baseDecision(const ecg_record::WayState* ways, std::size_t count, bool rrpv_order,
                         std::size_t base_way, ecg_record::VictimAgeing& ageing) {
    ageing = ecg_record::VictimAgeing();
    if (!rrpv_order)
        return base_way;
    uint8_t rrpv[64];
    uint8_t highest = 0;
    for (std::size_t i = 0; i < count; ++i) {
        rrpv[i] = std::min<uint8_t>(ways[i].rrpv, ecg_record::kVictimRrpvMax);
        highest = std::max(highest, rrpv[i]);
    }
    if (highest < ecg_record::kVictimRrpvMax) {
        ageing.amount = static_cast<uint8_t>(ecg_record::kVictimRrpvMax - highest);
        ageing.ways = count == 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
    }
    return graspScanReference(rrpv, count);
}

bool sameWays(const ecg_record::WayState* a, const ecg_record::WayState* b, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (a[i].property != b[i].property || a[i].rrpv != b[i].rrpv || a[i].recency != b[i].recency ||
            a[i].state != b[i].state || a[i].deadline != b[i].deadline)
            return false;
    return true;
}

// Opt-in uninformed fallback (step 6b). A set whose governed ways hold no live
// bound takes the configured base decision whole, its victim and its ageing,
// where governed-first would scan and age only the ungoverned ways.
void testUninformedSetTakesTheBaseDecision() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 34;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "uninformed fixture layout");
    constexpr uint64_t sequence = 19;
    VictimOptions arm; arm.governed_first = true; arm.rrpv_order = true;
    VictimOptions fallback = arm; fallback.uninformed_base = true;
    unsigned calls = 0;
    const auto base = [&calls]() { ++calls; return std::size_t{0}; };
    std::size_t victim = 0;
    VictimAgeing ageing;
    VictimTrace trace;
    const auto decide = [&](WayState* ways, std::size_t count, const VictimOptions& options) {
        return selectVictim(layout, ways, count, sequence, base, victim, &trace, options,
                            &ageing) == Status::OK;
    };
    WayState ways[3];
    ways[0].property = true; ways[0].state = State::UNKNOWN; ways[0].rrpv = 7; ways[0].recency = 1;
    ways[1].property = false; ways[1].rrpv = 0; ways[1].recency = 9;
    check(decide(ways, 2, arm) && victim == 1 && ageing.amount == 7 && ageing.ways == 0b10 &&
          trace.path == VictimPath::UNGOVERNED_FIRST,
          "governed-first evicts the ungoverned way and ages it by 7 without information");
    check(decide(ways, 2, fallback) && victim == 0 && ageing.amount == 0 && ageing.ways == 0 &&
          trace.path == VictimPath::UNINFORMED_BASE && calls == 0,
          "uninformed, the fallback takes the GRASP scan's RRPV-7 way and ages nothing");
    ways[0].rrpv = 5; ways[1].rrpv = 2;
    check(decide(ways, 2, fallback) && victim == 0 && ageing.amount == 2 && ageing.ways == 0b11,
          "the fallback ages every way by the GRASP scan's steps");
    // One live governed bound keeps governed-first; a bound at or past its
    // deadline, raw WRAP, or a bound on an ungoverned way is no information.
    ways[2].property = true; ways[2].rrpv = 1; ways[2].state = State::FINITE;
    ways[2].deadline = sequence + 8;
    check(decide(ways, 3, fallback) && victim == 1 && trace.path == VictimPath::UNGOVERNED_FIRST,
          "one live governed bound keeps governed-first");
    for (const auto& [state, deadline, governed] : {
            std::tuple<State, uint64_t, bool>{State::FINITE, sequence, true},
            std::tuple<State, uint64_t, bool>{State::FINITE, sequence - 1, true},
            std::tuple<State, uint64_t, bool>{State::WRAP, sequence + 8, true},
            std::tuple<State, uint64_t, bool>{State::FINITE, sequence + 8, false}}) {
        ways[2].state = state; ways[2].deadline = deadline; ways[2].property = governed;
        check(decide(ways, 3, fallback) && victim == 0 && trace.path == VictimPath::UNINFORMED_BASE,
              "a bound at or past its deadline, raw WRAP or an ungoverned bound is no information");
    }
    // DEAD and the pressure gate keep their places ahead of the fallback.
    ways[2].property = true; ways[2].state = State::DEAD;
    check(decide(ways, 3, fallback) && victim == 2 && trace.path == VictimPath::DEAD_FIRST,
          "explicit DEAD precedes the fallback");
    ways[2].state = State::UNKNOWN;
    VictimOptions relaxed = fallback; relaxed.pressured = false;
    check(decide(ways, 3, relaxed) && trace.path == VictimPath::UNPRESSURED,
          "an unpressured set keeps its own path");
    // Under the recency order the fallback is one call of the base policy.
    VictimOptions recency; recency.governed_first = true;
    calls = 0;
    check(selectVictim(layout, ways, 3, sequence, base, victim, &trace, recency) == Status::OK &&
          victim == 1 && calls == 0 && trace.path == VictimPath::UNGOVERNED_FIRST,
          "recency-ordered governed-first takes the ungoverned way");
    recency.uninformed_base = true;
    check(selectVictim(layout, ways, 3, sequence, base, victim, &trace, recency) == Status::OK &&
          victim == 0 && calls == 1 && trace.path == VictimPath::UNINFORMED_BASE,
          "under the recency order the fallback is one call of the base policy");
    // Random sets: an uninformed set takes the base decision, any other set the
    // rule's decision without the fallback; tracing changes nothing; no way is
    // written.
    std::mt19937_64 rng(0x0B5E7A11);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int mismatches = 0, uninformed = 0, informed = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = trial % 997 == 0 ? 64 : 1 + rng() % 16;
        const uint64_t now = 32 + rng() % 32;
        WayState sample[64], copy[64];
        bool live = false, dead = false;
        for (std::size_t i = 0; i < count; ++i) {
            sample[i].property = (rng() % 4) != 0;
            sample[i].rrpv = static_cast<uint8_t>(rng() % 8);
            sample[i].recency = rng() % 4096;
            sample[i].state = states[rng() % 4];
            sample[i].deadline = rng() % 128;
            copy[i] = sample[i];
            dead = dead || (sample[i].property && sample[i].state == State::DEAD);
            live = live || (sample[i].property && sample[i].state == State::FINITE &&
                            sample[i].deadline > now);
        }
        VictimOptions without;
        without.governed_first = (rng() & 1) != 0;
        without.rrpv_order = (rng() & 1) != 0;
        without.pressured = (rng() & 3) != 0;
        VictimOptions with = without; with.uninformed_base = true;
        const std::size_t base_way = rng() % count;
        const auto pick = [base_way]() { return base_way; };
        std::size_t a = 0, b = 0, c = 0;
        VictimAgeing aa, ab, ac;
        VictimTrace traced;
        const Status sa = selectVictim(layout, sample, count, now, pick, a, nullptr, without, &aa);
        const Status sb = selectVictim(layout, sample, count, now, pick, b, nullptr, with, &ab);
        const Status sc = selectVictim(layout, sample, count, now, pick, c, &traced, with, &ac);
        if (sb != sc || b != c || ab.ways != ac.ways || ab.amount != ac.amount ||
            !sameWays(sample, copy, count))
            ++mismatches;
        if (!dead && with.pressured && !live) {
            ++uninformed;
            VictimAgeing expected_ageing;
            const std::size_t expected = baseDecision(sample, count, with.rrpv_order, base_way,
                                                      expected_ageing);
            if (sc != Status::OK || traced.path != VictimPath::UNINFORMED_BASE || c != expected ||
                ac.ways != expected_ageing.ways || ac.amount != expected_ageing.amount ||
                traced.rrpv_changed)
                ++mismatches;
        } else {
            ++informed;
            if (sa != sb || a != b || aa.ways != ab.ways || aa.amount != ab.amount)
                ++mismatches;
        }
    }
    check(mismatches == 0, "the fallback takes the base decision exactly when no governed bound is live");
    check(uninformed > 0 && informed > 0, "the random fixture reaches both kinds of set");
}

// Opt-in B2 ablation (step 6c). With the bound comparison off, a governed live
// base victim stands with the base's own ageing; every earlier branch runs.
void testBoundCompareOffKeepsTheBaseVictim() {
    using namespace ecg_record;
    auto req = requirements(32);
    req.record_count = 34;
    Layout layout;
    check(selectLayout(req, layout) == Status::OK, "bound-comparison fixture layout");
    constexpr uint64_t sequence = 19;
    WayState ways[2];
    ways[0].property = true; ways[0].state = State::FINITE; ways[0].deadline = sequence + 8;
    ways[0].rrpv = 7; ways[0].recency = 1;
    ways[1].property = true; ways[1].state = State::FINITE; ways[1].deadline = sequence + 64;
    ways[1].rrpv = 3; ways[1].recency = 9;
    const auto first = []() { return std::size_t{0}; };
    VictimOptions on, off;
    off.bound_compare = false;
    std::size_t victim = 0;
    VictimTrace trace;
    check(selectVictim(layout, ways, 2, sequence, first, victim, &trace, on) == Status::OK &&
          victim == 1 && trace.path == VictimPath::OVERRIDDEN,
          "the comparison replaces a live base with a farther live bound");
    check(selectVictim(layout, ways, 2, sequence, first, victim, &trace, off) == Status::OK &&
          victim == 0 && trace.path == VictimPath::NO_BOUND_COMPARE,
          "without the comparison the live base stands");
    VictimOptions arm_off = off; arm_off.rrpv_order = true;
    VictimAgeing ageing;
    check(selectVictim(layout, ways, 2, sequence, first, victim, &trace, arm_off, &ageing) ==
              Status::OK && victim == 0 && ageing.amount == 0 &&
              trace.path == VictimPath::NO_BOUND_COMPARE && !trace.rrpv_changed,
          "under the RRPV order the GRASP scan's victim stands");
    ways[0].rrpv = 5;
    check(selectVictim(layout, ways, 2, sequence, first, victim, &trace, arm_off, &ageing) ==
              Status::OK && victim == 0 && ageing.amount == 2 && ageing.ways == 0b11,
          "and keeps the GRASP scan's ageing");
    // Random sets, each control alone and both together: where the comparison
    // would have run, the base decision stands; elsewhere nothing changes.
    std::mt19937_64 rng(0xB2AB1A7E);
    const State states[4] = {State::UNKNOWN, State::FINITE, State::DEAD, State::WRAP};
    int mismatches = 0, bypassed = 0, both = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t count = 1 + rng() % 16;
        const uint64_t now = 32 + rng() % 32;
        WayState sample[16], copy[16];
        for (std::size_t i = 0; i < count; ++i) {
            sample[i].property = (rng() % 4) != 0;
            sample[i].rrpv = static_cast<uint8_t>(rng() % 8);
            sample[i].recency = rng() % 4096;
            sample[i].state = states[rng() % 4];
            sample[i].deadline = rng() % 128;
            copy[i] = sample[i];
        }
        VictimOptions compare;
        compare.governed_first = (rng() & 1) != 0;
        compare.rrpv_order = (rng() & 1) != 0;
        compare.pressured = (rng() & 3) != 0;
        compare.uninformed_base = (rng() & 1) != 0;
        VictimOptions ablated = compare; ablated.bound_compare = false;
        both += ablated.uninformed_base;
        const std::size_t base_way = rng() % count;
        const auto pick = [base_way]() { return base_way; };
        std::size_t a = 0, b = 0, c = 0;
        VictimAgeing aa, ab, ac;
        VictimTrace ta, tb;
        const Status sa = selectVictim(layout, sample, count, now, pick, a, &ta, compare, &aa);
        const Status sb = selectVictim(layout, sample, count, now, pick, b, &tb, ablated, &ab);
        const Status sc = selectVictim(layout, sample, count, now, pick, c, nullptr, ablated, &ac);
        if (sb != sc || b != c || ab.ways != ac.ways || ab.amount != ac.amount ||
            !sameWays(sample, copy, count))
            ++mismatches;
        if (sa != Status::OK || sb != Status::OK) {
            ++mismatches;
            continue;
        }
        if (ta.path == VictimPath::BASE_KEPT || ta.path == VictimPath::OVERRIDDEN) {
            ++bypassed;
            VictimAgeing expected_ageing;
            const std::size_t expected = baseDecision(sample, count, compare.rrpv_order, base_way,
                                                      expected_ageing);
            if (tb.path != VictimPath::NO_BOUND_COMPARE || b != expected || tb.rrpv_changed ||
                ab.ways != expected_ageing.ways || ab.amount != expected_ageing.amount)
                ++mismatches;
        } else if (tb.path != ta.path || a != b || aa.ways != ab.ways || aa.amount != ab.amount) {
            ++mismatches;
        }
    }
    check(mismatches == 0, "without the comparison only the refinement changes");
    check(bypassed > 0 && both > 0, "the random fixture reaches the refinement and both controls");
}

int main() {
    testAdaptiveBudgets();
    testSixBitConfiguration();
    testWideArithmeticAndPrediction();
    testDescriptorAndNaming();
    testDistanceBoundsAndMalformedRecords();
    testAvailableRecordWindow();
    testRecordConstructionAndAllocationLimits();
    testWindowCaptureReadinessAndPinning();
    testNativeConfigurationAndBinding();
    testSharedRuntimeStateAndQueue();
    testFilteredConstructionBudgetAndSemantics();
    testTypedPropertiesAndFilteredPasses();
    testFilteredReceiverInvalidations();
    testOlderInvalidationPreservesNewerUse();
    testUnknownPredictionIsNeutralToLru();
    testSelectedBaseVictimRefinement();
    testFilteredQuantizedPassBoundary();
    testVictimTraceIsPassive();
    testGovernedFirstEviction();
    testPressureGateRelaxesToTheBaseVictim();
    testRrpvOrderNeverReadsRecency();
    testRrpvOrderIsGraspWithoutGovernedWays();
    testUninformedSetTakesTheBaseDecision();
    testBoundCompareOffKeepsTheBaseVictim();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures != 0;
}
