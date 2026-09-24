// Reading a kernel census out of a cache receipt, for the tests that check it.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// The one-line kernel census of a receipt, or an empty string without one.
inline std::string kernelCensusLine(const std::string& json) {
    const std::size_t at = json.find("\"kernel_census\": ");
    return at == std::string::npos ? std::string() : json.substr(at, json.find('\n', at) - at);
}

// The receipt without its census line, or an empty string unless it carries
// exactly one, so only an armed receipt can equal an unarmed one this way.
inline std::string withoutKernelCensus(const std::string& json) {
    const std::size_t at = json.find("  \"kernel_census\": ");
    if (at == std::string::npos || json.find("kernel_census", at + 4) != std::string::npos)
        return {};
    return json.substr(0, at) + json.substr(json.find('\n', at) + 1);
}

// The unsigned value after the first "key": in a receipt, or UINT64_MAX.
inline uint64_t receiptValue(const std::string& json, const std::string& key) {
    const std::size_t at = json.find('"' + key + "\":");
    if (at == std::string::npos)
        return UINT64_MAX;
    return std::strtoull(json.c_str() + at + key.size() + 3, nullptr, 10);
}

// The unsigned value of key inside one segment of a census, or UINT64_MAX.
inline uint64_t segmentValue(const std::string& census, const std::string& segment,
                             const std::string& key) {
    const std::size_t at = census.find('"' + segment + "\":{");
    if (at == std::string::npos)
        return UINT64_MAX;
    return receiptValue(census.substr(at, census.find('}', at) - at), key);
}

// The objects of a census's pass detail in pass order, or none without one.
inline std::vector<std::string> censusPassDetail(const std::string& census) {
    std::vector<std::string> passes;
    const std::string key = "\"pass_detail\":[";
    std::size_t at = census.find(key);
    if (at == std::string::npos)
        return passes;
    for (at += key.size(); at < census.size() && census[at] == '{';) {
        const std::size_t end = census.find('}', at);
        if (end == std::string::npos)
            break;
        passes.push_back(census.substr(at, end + 1 - at));
        at = end + 1 + (end + 1 < census.size() && census[end + 1] == ',');
    }
    return passes;
}
