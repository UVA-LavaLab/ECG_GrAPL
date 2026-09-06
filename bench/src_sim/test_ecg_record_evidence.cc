#include <cstdio>
#include <sstream>
#include <vector>

#include "ecg_record_evidence.h"

int main() {
    ecg_record::StreamDigest bytes;
    bytes.add(0x0706050403020100ULL);
    if (bytes.value() != 0xc934670e7229dc03ULL)
        return 1;
    std::vector<uint64_t> source{0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};
    ecg_record::EquivalenceEvidence narrow, wide;
    for (const uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        ecg_record::Requirements requirements;
        requirements.vertex_count = 32;
        requirements.record_count = source.size();
        requirements.traversal_count = 2;
        requirements.requested_record_bytes = width;
        ecg_record::Layout layout;
        ecg_record::RecordStream stream;
        if (ecg_record::selectLayout(requirements, layout) != ecg_record::Status::OK ||
            ecg_record::buildRecords(requirements, layout, 16,
                [&](std::size_t index) { return source[index]; }, stream) != ecg_record::Status::OK)
            return 2;
        auto& evidence = width == 4 ? narrow : wide;
        evidence.prepare(requirements, stream,
            [&](uint64_t index) { return source[index]; },
            [&](uint64_t row) { return std::min<uint64_t>(row, source.size()); });
        bool incomplete = false, unordered = false;
        try { std::ostringstream output; evidence.report(output); }
        catch (const std::logic_error&) { incomplete = true; }
        try { evidence.observe(stream.word(1), 1, 0); }
        catch (const std::invalid_argument&) { unordered = true; }
        if (!incomplete || !unordered)
            return 3;
        for (uint64_t iteration = 0; iteration < 2; ++iteration)
            for (uint64_t index = 0; index < source.size(); ++index)
                evidence.observe(stream.word(index), index, iteration);
        std::ostringstream output;
        evidence.report(output);
        if (evidence.count() != 32 || output.str().find("property_read_count=32") == std::string::npos)
            return 4;
    }
    if (narrow.sourceOrder() != wide.sourceOrder() ||
        narrow.destinationStream() != wide.destinationStream() ||
        narrow.carrier() == wide.carrier() ||
        narrow.consumedSemantics() == wide.consumedSemantics())
        return 5;
    std::puts("[SUMMARY] failures=0");
    return 0;
}
