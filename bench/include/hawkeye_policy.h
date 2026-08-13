#ifndef GRAPHBREW_HAWKEYE_POLICY_H
#define GRAPHBREW_HAWKEYE_POLICY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hawkeye_policy {

inline constexpr std::size_t kOptgenQuanta = 128;
inline constexpr std::size_t kPredictorEntries = 2048;
inline constexpr std::size_t kSamplerSets = 350;
inline constexpr std::size_t kSamplerWays = 8;
inline constexpr std::size_t kSampledCacheSets = 64;
inline constexpr uint8_t kPredictorMax = 7;
inline constexpr uint8_t kPredictorThreshold = 4;
inline constexpr uint8_t kMaxRrpv = 7;
inline constexpr uint16_t kTimerSize = 1024;
// CRC2 Hawkeye intentionally stores a 10-bit sampled-set timer. Sampler
// replacement normally bounds retained intervals; intervals outside the
// 128-quantum OPTgen window fail closed as non-cacheable.

inline uint32_t crc32(uint64_t value)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        crc ^= static_cast<uint8_t>(value);
        value >>= 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

class Predictor {
  public:
    Predictor()
    {
        counters.fill(kPredictorThreshold);
    }

    bool friendly(uint64_t signature) const
    {
        return counters[index(signature)] >= kPredictorThreshold;
    }

    void increase(uint64_t signature)
    {
        auto& counter = counters[index(signature)];
        if (counter < kPredictorMax) ++counter;
    }

    void decrease(uint64_t signature)
    {
        auto& counter = counters[index(signature)];
        if (counter > 0) --counter;
    }

    uint8_t value(uint64_t signature) const
    {
        return counters[index(signature)];
    }

  private:
    static std::size_t index(uint64_t signature)
    {
        return crc32(signature) % kPredictorEntries;
    }

    std::array<uint8_t, kPredictorEntries> counters{};
};

class Optgen {
  public:
    explicit Optgen(uint16_t capacity = 1)
        : capacity(std::max<uint16_t>(capacity, 1))
    {
        occupancy.fill(0);
    }

    void advance(uint16_t quantum)
    {
        occupancy[quantum % kOptgenQuanta] = 0;
    }

    bool addInterval(
        uint16_t previous_quantum, uint16_t current_quantum,
        uint16_t distance)
    {
        if (distance == 0 || distance >= kOptgenQuanta) return false;
        uint16_t quantum = previous_quantum % kOptgenQuanta;
        const uint16_t end = current_quantum % kOptgenQuanta;
        while (quantum != end) {
            if (occupancy[quantum] >= capacity) return false;
            quantum = (quantum + 1) % kOptgenQuanta;
        }
        quantum = previous_quantum % kOptgenQuanta;
        while (quantum != end) {
            ++occupancy[quantum];
            quantum = (quantum + 1) % kOptgenQuanta;
        }
        return true;
    }

    uint16_t occupancyAt(uint16_t quantum) const
    {
        return occupancy[quantum % kOptgenQuanta];
    }

  private:
    uint16_t capacity;
    std::array<uint16_t, kOptgenQuanta> occupancy{};
};

struct SamplerEntry {
    bool valid = false;
    bool prefetch = false;
    uint8_t tag = 0;
    uint8_t lru = 0;
    uint16_t last_time = 0;
    uint64_t signature = 0;
};

class State {
  public:
    State(std::size_t num_sets, uint16_t ways)
        : numSets(std::max<std::size_t>(num_sets, 1)),
          optgenCapacity(std::max<uint16_t>(ways > 2 ? ways - 2 : ways, 1)),
          timers(numSets, 0),
          optgen(numSets, Optgen(optgenCapacity)),
          sampler(kSamplerSets)
    {}

    bool sampledSet(std::size_t set) const
    {
        if (numSets <= kSampledCacheSets) return set < numSets;
        const std::size_t stride =
            std::max<std::size_t>(numSets / kSampledCacheSets, 1);
        return set % stride == 0 &&
               set / stride < kSampledCacheSets;
    }

    bool access(
        std::size_t set, uint64_t line_address, uint64_t signature,
        bool prefetch = false)
    {
        Predictor& active = prefetch ? prefetchPredictor : demandPredictor;
        if (set >= numSets || !sampledSet(set))
            return active.friendly(signature);

        const uint16_t now = timers[set] % kTimerSize;
        const uint16_t quantum = now % kOptgenQuanta;
        auto& history = sampler[line_address % kSamplerSets];
        const uint8_t tag = static_cast<uint8_t>(
            crc32(line_address >> 6) & 0xFFu);
        SamplerEntry* entry = nullptr;
        for (auto& candidate : history) {
            if (candidate.valid && candidate.tag == tag) {
                entry = &candidate;
                break;
            }
        }

        if (entry) {
            const uint8_t previous_lru = entry->lru;
            const uint16_t distance =
                static_cast<uint16_t>((now + kTimerSize -
                    entry->last_time) % kTimerSize);
            const bool opt_hit = optgen[set].addInterval(
                entry->last_time % kOptgenQuanta, quantum, distance);
            Predictor& trained =
                entry->prefetch ? prefetchPredictor : demandPredictor;
            if (opt_hit) trained.increase(entry->signature);
            else trained.decrease(entry->signature);
            ageHistory(history, entry, previous_lru);
        } else {
            entry = selectSamplerVictim(history);
            ageHistory(history, entry, kSamplerWays);
        }

        optgen[set].advance(quantum);
        entry->valid = true;
        entry->prefetch = prefetch;
        entry->tag = tag;
        entry->lru = 0;
        entry->last_time = now;
        entry->signature = signature;
        timers[set] = (now + 1) % kTimerSize;
        return active.friendly(signature);
    }

    void eviction(
        std::size_t set, uint64_t signature, bool prefetch = false)
    {
        if (set >= numSets || !sampledSet(set)) return;
        Predictor& active = prefetch ? prefetchPredictor : demandPredictor;
        active.decrease(signature);
    }

    uint8_t predictorValue(uint64_t signature, bool prefetch = false) const
    {
        const Predictor& active =
            prefetch ? prefetchPredictor : demandPredictor;
        return active.value(signature);
    }

    bool samplerContains(uint64_t line_address) const
    {
        const auto& history = sampler[line_address % kSamplerSets];
        const uint8_t tag = static_cast<uint8_t>(
            crc32(line_address >> 6) & 0xFFu);
        for (const auto& entry : history)
            if (entry.valid && entry.tag == tag) return true;
        return false;
    }

  private:
    static SamplerEntry* selectSamplerVictim(
        std::array<SamplerEntry, kSamplerWays>& history)
    {
        for (auto& entry : history)
            if (!entry.valid) return &entry;
        return &*std::max_element(
            history.begin(), history.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.lru < rhs.lru;
            });
    }

    static void ageHistory(
        std::array<SamplerEntry, kSamplerWays>& history,
        SamplerEntry* touched, uint8_t threshold)
    {
        for (auto& entry : history) {
            if (!entry.valid || &entry == touched) continue;
            if (entry.lru < threshold && entry.lru < kSamplerWays - 1)
                ++entry.lru;
        }
    }

    std::size_t numSets;
    uint16_t optgenCapacity;
    std::vector<uint16_t> timers;
    std::vector<Optgen> optgen;
    std::vector<std::array<SamplerEntry, kSamplerWays>> sampler;
    Predictor demandPredictor;
    Predictor prefetchPredictor;
};

inline uint8_t insertionRrpv(bool friendly)
{
    return friendly ? 0 : kMaxRrpv;
}

inline void ageFriendlyFill(uint8_t* rrpv, std::size_t ways)
{
    bool has_six = false;
    for (std::size_t way = 0; way < ways; ++way)
        has_six |= rrpv[way] == kMaxRrpv - 1;
    if (has_six) return;
    for (std::size_t way = 0; way < ways; ++way)
        if (rrpv[way] < kMaxRrpv - 1) ++rrpv[way];
}

inline std::size_t selectVictim(const uint8_t* rrpv, std::size_t ways)
{
    std::size_t victim = 0;
    for (std::size_t way = 1; way < ways; ++way)
        if (rrpv[way] > rrpv[victim]) victim = way;
    return victim;
}

}  // namespace hawkeye_policy

#endif  // GRAPHBREW_HAWKEYE_POLICY_H
