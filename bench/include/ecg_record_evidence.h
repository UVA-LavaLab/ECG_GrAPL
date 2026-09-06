#ifndef GRAPHBREW_ECG_RECORD_EVIDENCE_H
#define GRAPHBREW_ECG_RECORD_EVIDENCE_H

#include <iomanip>
#include <ostream>
#include <stdexcept>

#include "ecg_record_stream.h"

namespace ecg_record {

class StreamDigest {
  public:
    void add(uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            value_ ^= (value >> (8 * byte)) & 0xff;
            value_ *= 1099511628211ULL;
        }
    }
    uint64_t value() const { return value_; }

  private:
    uint64_t value_ = 1469598103934665603ULL;
};

// Diagnostic observation only: none of these hashes supplies a cache hint.
class EquivalenceEvidence {
  public:
    template<typename SourceAt, typename OffsetAt>
    void prepare(
            const Requirements& requirements, const RecordStream& stream,
            SourceAt source_at, OffsetAt offset_at) {
        if (prepared_ || validateConfiguration(requirements, stream.layout) != Status::OK ||
            stream.size() != requirements.record_count)
            throw std::invalid_argument("Invalid ECG equivalence stream");
        requirements_ = requirements;
        layout_ = stream.layout;
        source_.add(requirements.vertex_count);
        source_.add(requirements.record_count);
        uint64_t previous = 0;
        for (uint64_t row = 0; row <= requirements.vertex_count; ++row) {
            const uint64_t offset = offset_at(row);
            if (offset < previous || offset > requirements.record_count ||
                (row == 0 && offset != 0) ||
                (row == requirements.vertex_count && offset != requirements.record_count))
                throw std::invalid_argument("Invalid ECG equivalence row offsets");
            source_.add(offset);
            previous = offset;
        }
        uint64_t descriptor = 0;
        if (packLayout(layout_, descriptor) != Status::OK)
            throw std::invalid_argument("Invalid ECG equivalence layout");
        carrier_.add(descriptor);
        carrier_.add(requirements.record_count);
        windows_.add(descriptor);
        for (uint64_t index = 0; index < requirements.record_count; ++index) {
            const uint64_t destination = source_at(index);
            const uint64_t word = stream.word(index);
            DecodedRecord decoded;
            if (destination >= requirements.vertex_count ||
                decodeRecord(layout_, word, decoded) != Status::OK ||
                decoded.destination != destination)
                throw std::invalid_argument("ECG carrier differs from its actual source");
            source_.add(destination);
            carrier_.add(word);
            RecordWindow window;
            window.vertex_count = requirements.vertex_count;
            window.remaining_records = requirements.record_count - index;
            const uint64_t count = std::min<uint64_t>(RecordWindow::kRecords, window.remaining_records);
            for (unsigned lead = 0; lead < count; ++lead) {
                window.words[lead] = stream.word(index + lead);
                window.valid_mask |= uint16_t{1} << lead;
            }
            PrefetchTarget target;
            if (selectWindowTarget(layout_, window, 16, target) != Status::OK)
                throw std::invalid_argument("ECG equivalence window is incomplete");
            windows_.add(index);
            windows_.add(target.valid);
            windows_.add(target.lead);
            windows_.add(target.destination);
        }
        prepared_ = true;
    }

    void observe(uint64_t raw_word, uint64_t index, uint64_t iteration) {
        uint64_t base = 0, sequence = 0, next = 0;
        if (!prepared_ || index >= requirements_.record_count ||
            iteration >= requirements_.traversal_count ||
            !checkedMultiply(iteration, requirements_.record_count, base) ||
            !checkedAdd(base, index, sequence) || !checkedAdd(sequence, 1, sequence) ||
            !checkedAdd(count_, 1, next) || next != sequence)
            throw std::invalid_argument("ECG equivalence observation is out of order");
        const bool has_next = iteration + 1 < requirements_.traversal_count;
        Prediction prediction;
        if (makePrediction(layout_, raw_word, sequence, has_next, prediction) != Status::OK ||
            prediction.destination >= requirements_.vertex_count)
            throw std::invalid_argument("Invalid actual ECG equivalence word");
        destinations_.add(index);
        destinations_.add(prediction.destination);
        destinations_.add(sequence);
        consumed_.add(raw_word);
        consumed_.add(index);
        consumed_.add(prediction.destination);
        consumed_.add(sequence);
        consumed_.add(static_cast<uint64_t>(prediction.state));
        consumed_.add(prediction.deadline);
        consumed_.add(has_next);
        count_ = next;
    }

    uint64_t sourceOrder() const { return source_.value(); }
    uint64_t carrier() const { return carrier_.value(); }
    uint64_t consumedSemantics() const { return consumed_.value(); }
    uint64_t destinationStream() const { return destinations_.value(); }
    uint64_t windowReference() const { return windows_.value(); }
    uint64_t count() const { return count_; }

    void report(std::ostream& output) const {
        uint64_t expected = 0;
        if (!prepared_ || !checkedMultiply(
                requirements_.record_count, requirements_.traversal_count, expected) ||
            count_ != expected)
            throw std::logic_error("ECG equivalence observation is incomplete");
        output << "[ECG-RECORD-EQUIVALENCE schema=ecg.record-stream"
               << " observer=actual-record-load property_read_count=" << count_;
        hashField(output, "source_order_digest", sourceOrder());
        hashField(output, "carrier_digest", carrier());
        hashField(output, "consumed_semantic_digest", consumedSemantics());
        hashField(output, "destination_stream_digest", destinationStream());
        hashField(output, "window_reference_digest", windowReference());
        output << "]\n";
    }

  private:
    static void hashField(std::ostream& output, const char* name, uint64_t value) {
        const auto flags = output.flags();
        const auto fill = output.fill();
        output << ' ' << name << '=' << std::hex << std::setfill('0') << std::setw(16) << value;
        output.flags(flags);
        output.fill(fill);
    }

    Requirements requirements_;
    Layout layout_;
    StreamDigest source_, carrier_, consumed_, destinations_, windows_;
    uint64_t count_ = 0;
    bool prepared_ = false;
};

} // namespace ecg_record

#endif
