#ifndef POPT_REREFERENCE_H
#define POPT_REREFERENCE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace popt_reref {

enum class Encoding : uint8_t { Full, SingleEpoch };
enum class PostFinal : uint8_t { Later, Distant };

constexpr uint8_t kAbsent = 0x80;
constexpr uint8_t kNextPresent = 0x40;
constexpr uint8_t kSingleEpochMask = 0x3f;

constexpr uint32_t subEpochBins(Encoding encoding) {
    return encoding == Encoding::SingleEpoch ? 64 : 128;
}

constexpr uint32_t maxRank(Encoding encoding) {
    return subEpochBins(encoding) - 1;
}

// Section VII-B does not specify the post-final rank when next-present is
// clear. Keep both interpretations explicit; neither reads a second column.
constexpr uint32_t singleEpochNextRef(
        uint8_t entry, uint32_t current_sub_epoch, bool has_next_epoch,
        PostFinal postfinal) {
    const uint32_t value = entry & kSingleEpochMask;
    if (entry & kAbsent) return value;
    if (current_sub_epoch <= value) return 0;
    if (!has_next_epoch) return kSingleEpochMask;
    if (entry & kNextPresent) return 1;
    return postfinal == PostFinal::Later ? 2 : kSingleEpochMask;
}

struct IgnoreMatrixAccess {
    void operator()(const void*, uint64_t, bool) const {}
};

class FullMatrix {
  public:
    static constexpr uint32_t kEpochs = 256;

    template<class Observe = IgnoreMatrixAccess>
    void configure(uint32_t vertices, uint32_t lines, uint64_t maximum_bytes,
                   Observe observe = Observe{}) {
        const uint64_t bytes = uint64_t(lines) * kEpochs;
        if (vertices == 0 || lines == 0 || !entries_.empty())
            throw std::invalid_argument("invalid-popt-matrix-domain");
        if (bytes > maximum_bytes || bytes > entries_.max_size())
            throw std::length_error("popt-matrix-workspace-limit");
        vertices_ = vertices;
        lines_ = lines;
        epoch_size_ = static_cast<uint32_t>((uint64_t(vertices) + kEpochs - 1) / kEpochs);
        sub_epoch_size_ = (epoch_size_ + 127) / 128;
        entries_.assign(static_cast<std::size_t>(bytes), 0xff);
        if (entries_.capacity() > maximum_bytes)
            throw std::length_error("popt-matrix-workspace-limit");
        observe(entries_.data(), bytes, true);
    }

    template<class Observe = IgnoreMatrixAccess>
    void reference(uint32_t line, uint32_t source, Observe observe = Observe{}) {
        if (entries_.empty() || ready_ || line >= lines_ || source >= vertices_)
            throw std::invalid_argument("invalid-popt-structural-reference");
        const uint32_t epoch = source / epoch_size_;
        const uint8_t sub = static_cast<uint8_t>((source % epoch_size_) / sub_epoch_size_);
        uint8_t& entry = entries_[uint64_t(epoch) * lines_ + line];
        observe(&entry, 1, false);
        const uint8_t next = entry & kAbsent ? sub : std::max(entry, sub);
        if (entry != next) {
            observe(&entry, 1, true);
            entry = next;
        }
    }

    template<class Observe = IgnoreMatrixAccess>
    void finish(Observe observe = Observe{}) {
        if (entries_.empty() || ready_)
            throw std::logic_error("invalid-popt-matrix-finalization");
        // Quantizing the per-epoch maximum commutes with taking its maximum.
        // A small line block keeps the distance scan bounded and contiguous.
        for (uint64_t first = 0; first < lines_; first += 64) {
            const uint64_t count = std::min<uint64_t>(64, lines_ - first);
            std::array<uint8_t, 64> distance;
            distance.fill(127);
            observe(distance.data(), count, true);
            for (uint32_t epoch = kEpochs; epoch-- > 0;) {
                for (uint64_t lane = 0; lane < count; ++lane) {
                    uint8_t& entry = entries_[uint64_t(epoch) * lines_ + first + lane];
                    observe(&entry, 1, false);
                    if (!(entry & kAbsent)) {
                        observe(&distance[lane], 1, true);
                        distance[lane] = 1;
                    } else {
                        observe(&distance[lane], 1, false);
                        observe(&entry, 1, true);
                        entry = kAbsent | distance[lane];
                        if (distance[lane] < 127) {
                            observe(&distance[lane], 1, true);
                            ++distance[lane];
                        }
                    }
                }
            }
        }
        ready_ = true;
    }

    const uint8_t* data() const {
        if (!ready_)
            throw std::logic_error("popt-matrix-is-not-ready");
        return entries_.data();
    }
    uint64_t bytes() const { return entries_.capacity(); }
    uint32_t lines() const { return lines_; }
    uint32_t epochSize() const { return epoch_size_; }
    uint32_t subEpochSize() const { return sub_epoch_size_; }

  private:
    std::vector<uint8_t> entries_;
    uint32_t vertices_ = 0, lines_ = 0, epoch_size_ = 0, sub_epoch_size_ = 0;
    bool ready_ = false;
};

}  // namespace popt_reref

#endif
