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

}  // namespace

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
    testTypedPropertiesAndFilteredPasses();
    testFilteredReceiverInvalidations();
    std::printf("[SUMMARY] failures=%d\n", failures);
    return failures != 0;
}
