#ifndef GRAPHBREW_ECG_RECORD_WINDOW_H
#define GRAPHBREW_ECG_RECORD_WINDOW_H

#include <array>
#include <cstdint>
#include <cstring>

#include "ecg_record.h"

namespace ecg_record {

class WindowBuffer {
  public:
    static constexpr unsigned kLineBytes = 64;
    static constexpr unsigned kMaximumBanks = 3;

    Status configure(const Layout& layout, uint64_t base, uint64_t count) {
        uint64_t address = 0, banks = 0, bytes = 0;
        const Status status = recordAddress(layout, base, 0, count, address);
        if (status != Status::OK)
            return status;
        if (windowStorage(layout, RecordWindow::kRecords, kLineBytes, banks, bytes) != Status::OK ||
            banks > kMaximumBanks)
            return Status::INVALID_LAYOUT;
        layout_ = layout;
        base_ = base;
        count_ = count;
        bank_count_ = static_cast<unsigned>(banks);
        banks_ = {};
        pinned_ = false;
        return Status::OK;
    }

    Status pin(uint64_t record_address) {
        uint64_t index = 0, sequence = 0;
        const Status status = recordPosition(layout_, record_address, base_, count_, 0,
                                              index, sequence);
        if (status != Status::OK)
            return status;
        const uint64_t records = std::min<uint64_t>(RecordWindow::kRecords, count_ - index);
        const uint64_t last = record_address + records * layout_.record_bytes - 1;
        first_line_ = record_address - record_address % kLineBytes;
        last_line_ = last - last % kLineBytes;
        pinned_ = true;
        return Status::OK;
    }

    void unpin() { pinned_ = false; }
    unsigned bankCount() const { return bank_count_; }
    uint64_t firstLine() const { return first_line_; }
    uint64_t lastLine() const { return last_line_; }

    bool hasLine(uint64_t virtual_line) const {
        return find(virtual_line) != nullptr;
    }

    Status storeLine(
            uint64_t virtual_line, uint64_t physical_line,
            const uint8_t* bytes, uint64_t ready_tick) {
        if (!bytes || bank_count_ == 0 || virtual_line % kLineBytes ||
            physical_line % kLineBytes)
            return Status::INVALID_ADDRESS;
        Bank* selected = nullptr;
        for (unsigned index = 0; index < bank_count_; ++index) {
            auto& bank = banks_[index];
            if (bank.valid && bank.virtual_line == virtual_line) {
                if (bank.physical_line != physical_line)
                    return Status::INVALID_ADDRESS;
                selected = &bank;
                break;
            }
            if (!bank.valid || (!needed(bank.virtual_line) &&
                (!selected || (selected->valid && bank.ready_tick < selected->ready_tick))))
                selected = &bank;
        }
        if (!selected)
            return Status::NOT_READY;
        std::memcpy(selected->bytes.data(), bytes, kLineBytes);
        selected->virtual_line = virtual_line;
        selected->physical_line = physical_line;
        selected->ready_tick = ready_tick;
        selected->valid = true;
        return Status::OK;
    }

    Status readWindow(uint64_t address, uint64_t vertices, uint64_t tick, RecordWindow& output) const {
        output = RecordWindow{};
        uint64_t index = 0, sequence = 0;
        const Status status = recordPosition(layout_, address, base_, count_, 0, index, sequence);
        if (status != Status::OK)
            return status;
        output.vertex_count = vertices;
        output.remaining_records = count_ - index;
        const auto records = std::min<uint64_t>(RecordWindow::kRecords, output.remaining_records);
        for (unsigned lead = 0; lead < records; ++lead) {
            const uint64_t word_address = address + lead * layout_.record_bytes;
            const Bank* bank = find(word_address - word_address % kLineBytes);
            if (!bank || bank->ready_tick > tick)
                return Status::NOT_READY;
            std::memcpy(&output.words[lead], bank->bytes.data() + word_address % kLineBytes,
                        layout_.record_bytes);
            output.valid_mask |= uint16_t{1} << lead;
        }
        return Status::OK;
    }

    Status updatePhysical(uint64_t physical_line, const uint8_t* bytes, uint64_t ready_tick) {
        if (!bytes || bank_count_ == 0 || physical_line % kLineBytes)
            return Status::INVALID_ADDRESS;
        for (unsigned index = 0; index < bank_count_; ++index) {
            auto& bank = banks_[index];
            if (bank.valid && bank.physical_line == physical_line) {
                std::memcpy(bank.bytes.data(), bytes, kLineBytes);
                bank.ready_tick = std::max(bank.ready_tick, ready_tick);
                return Status::OK;
            }
        }
        return Status::NOT_READY;
    }

  private:
    struct Bank {
        std::array<uint8_t, kLineBytes> bytes{};
        uint64_t virtual_line = 0;
        uint64_t physical_line = 0;
        uint64_t ready_tick = 0;
        bool valid = false;
    };

    bool needed(uint64_t line) const {
        return pinned_ && line >= first_line_ && line <= last_line_;
    }

    const Bank* find(uint64_t virtual_line) const {
        for (unsigned index = 0; index < bank_count_; ++index) {
            if (banks_[index].valid && banks_[index].virtual_line == virtual_line)
                return &banks_[index];
        }
        return nullptr;
    }

    Layout layout_;
    uint64_t base_ = 0;
    uint64_t count_ = 0;
    uint64_t first_line_ = 0;
    uint64_t last_line_ = 0;
    unsigned bank_count_ = 0;
    bool pinned_ = false;
    std::array<Bank, kMaximumBanks> banks_{};
};

} // namespace ecg_record

#endif
