#ifndef GRAPHBREW_ECG_WINDOW_H
#define GRAPHBREW_ECG_WINDOW_H

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ecg_record_runtime.h"
#include "ecg_record_stream.h"

namespace ecg_window {

struct Profile {
    uint64_t vertices, cohort_rows = 8, bin_rows = 1, bins = 0;

    explicit Profile(uint64_t count, uint64_t fixture_cohort = 0) : vertices(count) {
        if (!count || count > UINT32_MAX)
            throw std::invalid_argument("window-source-domain");
        while (cohort_rows < (count + 255) / 256)
            cohort_rows *= 2;
        if (fixture_cohort)
            cohort_rows = fixture_cohort;
        if (cohort_rows < 8 || (cohort_rows & (cohort_rows - 1)))
            throw std::invalid_argument("window-cohort");
        bin_rows = cohort_rows / 8;
        bins = (count + bin_rows - 1) / bin_rows;
    }

    uint16_t reverse(uint64_t& entry, uint32_t row) const {
        constexpr uint64_t valid = uint64_t{1} << 48;
        if (row >= vertices)
            throw std::invalid_argument("window-row");
        uint16_t token = 0;
        uint64_t endpoint = (row % cohort_rows) / bin_rows, strength = 0;
        if (entry & valid) {
            const uint32_t previous = static_cast<uint32_t>(entry);
            if (row > previous)
                throw std::logic_error("window-reverse-order");
            if (row == previous)
                return static_cast<uint16_t>((entry >> 38) & 1023);
            const uint64_t gap = previous / cohort_rows - row / cohort_rows;
            if (gap <= 7)
                token = static_cast<uint16_t>(512 | (gap << 6) |
                    (((entry >> 35) & 7) << 3) | ((entry >> 32) & 7));
            if (gap == 0) {
                endpoint = (entry >> 32) & 7;
                strength = std::min<uint64_t>(7, ((entry >> 35) & 7) + 1);
            }
        }
        entry = valid | row | (endpoint << 32) | (strength << 35) | (uint64_t(token) << 38);
        return token;
    }

    uint64_t decode(uint16_t token, uint64_t row, uint64_t pass_base) const {
        if (!token)
            return 0;
        if (token > 1023 || !(token & 512) || row >= vertices)
            throw std::invalid_argument("window-token");
        const uint64_t selected = row / cohort_rows + ((token >> 6) & 7);
        const uint64_t local_end = selected * 8 + (token & 7) + 1;
        uint64_t end = 0;
        if (selected * 8 >= bins || local_end > bins || local_end <= row / bin_rows ||
            !ecg_record::checkedAdd(pass_base, local_end, end) || end > (UINT64_MAX >> 3))
            throw std::invalid_argument("window-endpoint");
        return (end << 3) | ((token >> 3) & 7);
    }

    bool live(uint64_t payload, uint64_t watermark, uint64_t pass_base) const {
        const uint64_t end = payload >> 3;
        return end > watermark && end > pass_base && end - pass_base <= bins;
    }

    uint64_t distance(uint64_t payload, uint64_t watermark, uint64_t pass_base) const {
        if (!live(payload, watermark, pass_base))
            throw std::logic_error("window-invalid-live-window");
        const uint64_t start = pass_base + (((payload >> 3) - 1 - pass_base) / 8) * 8;
        return start > watermark ? start - watermark : 0;
    }

    bool poorerRetention(uint64_t candidate, uint64_t reference, uint64_t watermark, uint64_t pass_base) const {
        const uint64_t a = distance(candidate, watermark, pass_base);
        const uint64_t b = distance(reference, watermark, pass_base);
        return a > b || (a == b && (candidate & 7) < (reference & 7));
    }
};

struct Layout {
    uint8_t record_bytes = 0, id_bits = 0, metadata_bits = 0;
    static constexpr uint8_t token_bits = 10;

    void validate() const {
        if (id_bits == 0 || id_bits > 32 || (record_bytes != 4 && record_bytes != 8) ||
            id_bits + token_bits > record_bytes * 8 || metadata_bits != record_bytes * 8 - id_bits)
            throw std::invalid_argument("invalid-window-layout");
    }
    static Layout select(uint64_t maximum_id, uint8_t requested_bytes) {
        if (maximum_id > UINT32_MAX || (requested_bytes && requested_bytes != 4 && requested_bytes != 8))
            throw std::invalid_argument("window-record-layout");
        const uint8_t ids = ecg_record::bitWidth(maximum_id);
        const uint8_t bytes = requested_bytes ? requested_bytes : ids + token_bits <= 32 ? 4 : 8;
        if (ids + token_bits > bytes * 8)
            throw std::invalid_argument("window-token-does-not-fit-record");
        return {bytes, ids, static_cast<uint8_t>(bytes * 8 - ids)};
    }
    uint64_t idMask() const { return ecg_record::lowMask(id_bits); }
    uint64_t pack(uint32_t id, uint16_t token) const {
        validate();
        if (id > idMask() || token > 1023 || (token && !(token & 512)))
            throw std::invalid_argument("invalid-window-record");
        return id | (uint64_t(token) << id_bits);
    }
    uint32_t id(uint64_t word, uint64_t vertices) const {
        validate();
        if ((word >> (id_bits + token_bits)) || (word & idMask()) >= vertices)
            throw std::invalid_argument("invalid-actual-window-record");
        const uint16_t token = static_cast<uint16_t>(word >> id_bits);
        if (token && !(token & 512))
            throw std::invalid_argument("noncanonical-window-token");
        return static_cast<uint32_t>(word & idMask());
    }
};

struct RecordStream {
    Layout layout;
    ecg_record::BuildStats stats;
    uint64_t vertices = 0, cohort_rows = 0;
    std::vector<uint32_t> words32;
    std::vector<uint64_t> words64;
    uint64_t known_records = 0;
    uint64_t size() const { return layout.record_bytes == 4 ? words32.size() : words64.size(); }
    const uint8_t* data() const {
        return layout.record_bytes == 4 ? reinterpret_cast<const uint8_t*>(words32.data()) :
            reinterpret_cast<const uint8_t*>(words64.data());
    }
    uint64_t word(uint64_t index) const {
        return layout.record_bytes == 4 ? words32.at(index) : words64.at(index);
    }
};

template<class RowAt, class IdAt, class Observe>
RecordStream build(const Profile& profile, uint64_t records, const Layout& layout,
                   const ecg_record::BuildLimits& limits, RowAt row_at, IdAt id_at, Observe observe) {
    layout.validate();
    if (limits.source_id_bytes != 4)
        throw std::invalid_argument("window-construction-source-width");
    uint64_t payload = 0, source_bytes = 0;
    const uint64_t lines = (profile.vertices + 15) / 16;
    if (!records || !ecg_record::checkedMultiply(records, layout.record_bytes, payload) ||
        !ecg_record::checkedMultiply(records, 4, source_bytes) ||
        payload > limits.maximum_carrier_bytes || lines * 8 > limits.maximum_auxiliary_bytes)
        throw std::length_error("window-construction-budget");
    RecordStream stream;
    stream.layout = layout;
    stream.vertices = profile.vertices;
    stream.cohort_rows = profile.cohort_rows;
    if (layout.record_bytes == 4) {
        if (records > stream.words32.max_size())
            throw std::length_error("window-carrier-domain");
        stream.words32.resize(static_cast<std::size_t>(records));
    } else if (layout.record_bytes == 8) {
        if (records > stream.words64.max_size())
            throw std::length_error("window-carrier-domain");
        stream.words64.resize(static_cast<std::size_t>(records));
    } else {
        throw std::invalid_argument("window-record-width");
    }
    std::vector<uint64_t> positions(static_cast<std::size_t>(lines));
    stream.stats.carrier_allocation_bytes = layout.record_bytes == 4 ?
        stream.words32.capacity() * uint64_t{4} : stream.words64.capacity() * uint64_t{8};
    stream.stats.auxiliary_peak_bytes = positions.capacity() * uint64_t{8};
    if (stream.stats.carrier_allocation_bytes > limits.maximum_carrier_bytes ||
        stream.stats.auxiliary_peak_bytes > limits.maximum_auxiliary_bytes)
        throw std::length_error("window-construction-allocation");
    stream.stats.carrier_payload_bytes = payload;
    stream.stats.source_stream_bytes = source_bytes;
    observe(stream.data(), payload, true);
    observe(positions.data(), lines * 8, true);
    uint64_t next = records;
    for (uint64_t row = profile.vertices; row-- > 0;) {
        const auto bounds = row_at(row);
        if (bounds.second != next || bounds.first > bounds.second)
            throw std::invalid_argument("window-construction-row-domain");
        for (uint64_t index = bounds.second; index-- > bounds.first;) {
            const uint32_t id = id_at(index);
            if (id >= profile.vertices)
                throw std::invalid_argument("window-construction-id");
            auto& entry = positions[id / 16];
            observe(&entry, sizeof(entry), false);
            const uint64_t before = entry;
            const uint16_t token = profile.reverse(entry, static_cast<uint32_t>(row));
            if (!(before & (uint64_t{1} << 48)))
                ++stream.stats.property_lines;
            if (entry != before)
                observe(&entry, sizeof(entry), true);
            const uint64_t word = layout.pack(id, token);
            observe(stream.data() + index * layout.record_bytes, layout.record_bytes, true);
            if (layout.record_bytes == 4)
                stream.words32[index] = static_cast<uint32_t>(word);
            else
                stream.words64[index] = word;
            stream.known_records += token != 0;
        }
        next = bounds.first;
    }
    if (next)
        throw std::invalid_argument("window-construction-incomplete");
    return stream;
}

inline void observe(ecg_record::LineMetadata& metadata, uint64_t order) {
    if (!order)
        throw std::invalid_argument("window-observation-order");
    metadata.state = ecg_record::LineState::PENDING;
    metadata.value = order;
}

inline bool apply(ecg_record::LineMetadata& metadata, uint64_t order, uint64_t payload) {
    if (!order || metadata.state != ecg_record::LineState::PENDING || metadata.value != order)
        return false;
    metadata.state = payload ? ecg_record::LineState::FINITE : ecg_record::LineState::UNKNOWN;
    metadata.value = payload ? payload : order;
    return true;
}

struct PolicyStats {
    uint64_t decisions = 0, live_base = 0, overrides = 0, protected_overrides = 0;
};

}  // namespace ecg_window

#endif
