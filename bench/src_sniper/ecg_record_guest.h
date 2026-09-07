#ifndef GRAPHBREW_SNIPER_ECG_RECORD_GUEST_H
#define GRAPHBREW_SNIPER_ECG_RECORD_GUEST_H

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "ecg_record_stream.h"
#include "ecg_record_window.h"
#include "ecg_record_evidence.h"
#include "sniper_sim/sniper_harness.h"

namespace graphbrew_sniper {

inline bool ecg_record_requested()
{
    const char* value = std::getenv("SNIPER_ECG_RECORD_MECHANISM");
    return value && value[0];
}

inline uint64_t ecg_record_env_u64(
    const char* name, uint64_t fallback,
    uint64_t minimum, uint64_t maximum)
{
    const char* value = std::getenv(name);
    if (!value || !value[0])
        return fallback;
    for (const char* digit = value; *digit; ++digit) {
        if (*digit < '0' || *digit > '9')
            throw std::invalid_argument(std::string(name) + " is invalid");
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(std::string(name) + " is invalid");
    }
    return parsed;
}

template<class SourceId>
inline uint64_t ecg_record_max_source_id(
        const SourceId* source, uint64_t count, uint64_t vertex_count)
{
    if (!source || count == 0 || vertex_count == 0)
        throw std::invalid_argument("Sniper ECG record source is empty");
    uint64_t maximum = 0;
    for (uint64_t index = 0; index < count; ++index) {
        const SourceId destination = source[index];
        if (destination < 0 ||
            static_cast<uint64_t>(destination) >= vertex_count) {
            throw std::invalid_argument(
                "Sniper ECG record source ID is outside the graph domain");
        }
        maximum = std::max<uint64_t>(
            maximum, static_cast<uint64_t>(destination));
    }
    return maximum;
}

class EcgRecordContext
{
  public:
    static constexpr bool models_memory = false;

    void bind(
        ecg_record::NativeConfiguration configuration,
        const ecg_record::RecordStream& stream)
    {
        configuration.control = ecg_record::kNativeEnable |
            ecg_record::kNativeManagedPasses;
        configuration.iteration_base = 0;
        ecg_record::Layout layout;
        ecg_record::PropertyDescriptor property;
        if (ecg_record::validateNativeConfiguration(
                configuration, layout) != ecg_record::Status::OK ||
            ecg_record::unpackProperty(
                configuration.property_descriptor, property) !=
                ecg_record::Status::OK ||
            configuration.record_base !=
                reinterpret_cast<uint64_t>(stream.data()) ||
            configuration.record_count != stream.size() ||
            !(layout == stream.layout)) {
            throw std::invalid_argument(
                "Invalid Sniper managed ECG binding");
        }
        if (bound_) {
            uint64_t next_generation = 0;
            if (cursor_.open())
                throw std::logic_error(
                    "Cannot rebind an open Sniper ECG pass");
            if (!ecg_record::checkedAdd(
                    configuration_.generation, 1, next_generation) ||
                configuration.generation != next_generation)
                throw std::invalid_argument(
                    "Sniper ECG rebind generation is not consecutive");
            ecg_record_drain();
            ecg_record_invalidate();
            accumulated_passes_ += cursor_.passes();
            accumulated_consumed_ += cursor_.consumed();
            accumulated_skipped_ += cursor_.skipped();
        }
        configuration_ = configuration;
        layout_ = layout;
        property_ = property;
        stream_ = &stream;
        cursor_ = ecg_record::PassCursor{};
        if (cursor_.configure(
                configuration.record_count, property.traversal) !=
                ecg_record::Status::OK)
            throw std::invalid_argument(
                "Cannot configure Sniper managed ECG cursor");
        ecg_record_configure(configuration_);
        loaded_mask_ = 0;
        next_load_ = 0;
        bound_ = true;
    }

    void beginPass(bool has_next = false)
    {
        if (!bound_ || cursor_.begin() != ecg_record::Status::OK)
            throw std::logic_error("Cannot begin Sniper managed ECG pass");
        configuration_.iteration_base = cursor_.base();
        configuration_.control = ecg_record::kNativeEnable |
            ecg_record::kNativeManagedPasses |
            (has_next ? ecg_record::kNativeHasNext : 0);
        ecg_record_iteration(
            configuration_.iteration_base, has_next,
            /*managed=*/true);
        loaded_mask_ = 0;
        next_load_ = 0;
    }

    void closePass()
    {
        if (!bound_ || !cursor_.open())
            throw std::logic_error("No Sniper managed ECG pass is open");
        ecg_record_pass_close();
        if (cursor_.close() != ecg_record::Status::OK)
            throw std::logic_error("Sniper managed ECG pass close failed");
    }

    void drain() const
    {
        ecg_record_drain();
    }

    uint64_t loadRecord(uint64_t index)
    {
        if (!bound_ || !cursor_.open() ||
            index >= configuration_.record_count)
            throw std::out_of_range("Sniper managed ECG record index");
        ensureWindow(index);
        const std::size_t slot = index % words_.size();
        if ((loaded_mask_ & (uint16_t{1} << slot)) == 0 ||
            indices_[slot] != index)
            throw std::logic_error(
                "Sniper managed ECG window missed current word");
        return words_[slot];
    }

    template<typename T>
    T loadProperty(
        uint64_t index, uint64_t raw_word, const T* base)
    {
        constexpr bool f32 = std::is_same<T, float>::value;
        constexpr bool u32 = std::is_same<T, uint32_t>::value;
        constexpr bool u64 = std::is_same<T, uint64_t>::value;
        static_assert(f32 || u32 || u64,
            "Sniper managed ECG properties are F32, U32, or U64");
        const ecg_record::PropertyKind expected =
            f32 ? ecg_record::PropertyKind::F32 :
            u32 ? ecg_record::PropertyKind::U32 :
                  ecg_record::PropertyKind::U64;
        if (!bound_ || !cursor_.open() ||
            property_.kind != expected ||
            reinterpret_cast<uint64_t>(base) != configuration_.property_base ||
            cursor_.consume(index) != ecg_record::Status::OK)
            throw std::invalid_argument(
                "Invalid Sniper managed ECG property load");
        const volatile void* record_address = addressAt(index);
        ecg_record_consume(record_address);
        ecg_record::NativeLoadResult result;
        if (ecg_record::nativePropertyAccess(
                configuration_, reinterpret_cast<uint64_t>(base),
                raw_word,
                reinterpret_cast<uint64_t>(
                    const_cast<const void*>(
                        reinterpret_cast<const volatile void*>(
                            record_address))),
                result) != ecg_record::Status::OK)
            throw std::invalid_argument(
                "Invalid Sniper managed ECG property operands");
        return *reinterpret_cast<const volatile T*>(
            result.property_address);
    }

    void finish()
    {
        if (!bound_)
            return;
        if (cursor_.open())
            throw std::logic_error(
                "Cannot finish an open Sniper ECG pass");
        drain();
        ecg_record_report();
        ecg_record_deactivate();
        bound_ = false;
    }

    void finish(uint64_t actual_records)
    {
        if (consumed() != actual_records)
            throw std::logic_error(
                "Sniper ECG actual-record count disagrees with transport");
        finish();
    }

    uint64_t passes() const
    {
        return accumulated_passes_ + cursor_.passes();
    }
    uint64_t consumed() const
    {
        return accumulated_consumed_ + cursor_.consumed();
    }
    uint64_t skipped() const
    {
        return accumulated_skipped_ + cursor_.skipped();
    }

  private:
    const volatile void* addressAt(uint64_t index) const
    {
        return stream_->data() + index * layout_.record_bytes;
    }

    void load(uint64_t index)
    {
        const volatile void* address = addressAt(index);
        ecg_record_arm_read(address, layout_.record_bytes);
        uint64_t word = layout_.record_bytes == 4
            ? *reinterpret_cast<const volatile uint32_t*>(address)
            : *reinterpret_cast<const volatile uint64_t*>(address);
        ecg_record_report_loaded(address, word, layout_.record_bytes);
        const std::size_t slot = index % words_.size();
        words_[slot] = word;
        indices_[slot] = index;
        loaded_mask_ |= uint16_t{1} << slot;
    }

    void ensureWindow(uint64_t position)
    {
        const uint64_t end = std::min<uint64_t>(
            configuration_.record_count,
            position + ecg_record::RecordWindow::kRecords);
        if (next_load_ < position)
            next_load_ = position;
        while (next_load_ < end)
            load(next_load_++);
    }

    ecg_record::NativeConfiguration configuration_;
    ecg_record::Layout layout_;
    ecg_record::PropertyDescriptor property_;
    const ecg_record::RecordStream* stream_ = nullptr;
    ecg_record::PassCursor cursor_;
    std::array<uint64_t, ecg_record::RecordWindow::kRecords> words_{};
    std::array<uint64_t, ecg_record::RecordWindow::kRecords> indices_{};
    uint64_t next_load_ = 0;
    uint64_t accumulated_passes_ = 0;
    uint64_t accumulated_consumed_ = 0;
    uint64_t accumulated_skipped_ = 0;
    uint16_t loaded_mask_ = 0;
    bool bound_ = false;
};

template<class GraphT>
class EcgRecordPrStream
{
  public:
    EcgRecordPrStream(
        const GraphT& graph, const float* property_base,
        uint64_t traversal_count)
      : graph_(graph)
    {
        using SourceId = std::remove_cv_t<std::remove_reference_t<
            decltype(*graph.in_neigh(0).begin())>>;
        static_assert(sizeof(SourceId) == sizeof(uint32_t),
            "Sniper ECG record currently requires 32-bit source IDs");
        if (!ecg_record_requested())
            return;
        ecg_record::Mechanism mechanism;
        if (ecg_record::parseMechanismName(
                std::getenv("SNIPER_ECG_RECORD_MECHANISM"),
                mechanism) != ecg_record::Status::OK) {
            throw std::invalid_argument(
                "SNIPER_ECG_RECORD_MECHANISM is invalid");
        }
        if (graph.num_nodes() <= 0 || graph.num_edges_directed() <= 0 ||
            static_cast<uint64_t>(graph.num_nodes()) > uint64_t{INT32_MAX}) {
            throw std::invalid_argument(
                "Sniper ECG record graph exceeds the signed NodeID loader");
        }
        for (const char* legacy : {
                "SNIPER_ENABLE_ECG_EXTRACT",
                "SNIPER_ENABLE_ECG_PFX_HINTS",
                "SNIPER_REUSE_PLAN_TRANSPORT_MATCHED",
                "SNIPER_REUSE_PLAN_EXACT_BIND",
                "ECG_FLOWTHROUGH"}) {
            const char* value = std::getenv(legacy);
            if (value && value[0] && std::string(value) != "0") {
                throw std::invalid_argument(
                    std::string("Sniper ECG record cannot mix with ") +
                    legacy);
            }
        }

        requirements_.vertex_count =
            static_cast<uint64_t>(graph.num_nodes());
        const SourceId* source = graph.in_neigh(0).begin();
        requirements_.max_vertex_id_known = true;
        requirements_.max_vertex_id = ecg_record_max_source_id(
            source, static_cast<uint64_t>(graph.num_edges_directed()),
            requirements_.vertex_count);
        requirements_.record_count = graph.num_edges_directed();
        requirements_.traversal_count = traversal_count;
        requirements_.requested_record_bytes = static_cast<uint8_t>(
            ecg_record_env_u64(
                "SNIPER_ECG_RECORD_BYTES", 0, 0, 8));
        if (requirements_.requested_record_bytes != 0 &&
            requirements_.requested_record_bytes != 4 &&
            requirements_.requested_record_bytes != 8) {
            throw std::invalid_argument(
                "SNIPER_ECG_RECORD_BYTES must be 0, 4, or 8");
        }
        requirements_.minimum_mantissa_bits = static_cast<uint8_t>(
            ecg_record_env_u64(
                "SNIPER_ECG_RECORD_MINIMUM_MANTISSA_BITS",
                0, 0, 61));
        ecg_record::Status status =
            ecg_record::selectLayout(requirements_, layout_);
        if (status != ecg_record::Status::OK)
            throw std::invalid_argument(ecg_record::statusName(status));

        ecg_record::BuildLimits limits;
        limits.maximum_carrier_bytes =
            ecg_record_env_u64(
                "SNIPER_ECG_RECORD_MAX_CARRIER_BYTES",
                uint64_t{256} << 20, 1, UINT64_MAX);
        limits.maximum_auxiliary_bytes =
            ecg_record_env_u64(
                "SNIPER_ECG_RECORD_MAX_AUX_BYTES",
                uint64_t{256} << 20, 1, UINT64_MAX);
        limits.source_id_bytes = sizeof(SourceId);
        if (cache_line_size() != ecg_record::WindowBuffer::kLineBytes)
            throw std::invalid_argument(
                "Sniper ECG record currently requires 64-byte cache lines");
        status = ecg_record::buildRecords(
            requirements_, layout_,
            ecg_record::WindowBuffer::kLineBytes / sizeof(float),
            [&](std::size_t index) -> uint64_t {
                const SourceId destination = source[index];
                if (destination < 0)
                    return UINT64_MAX;
                return static_cast<uint64_t>(destination);
            },
            stream_, limits);
        if (status != ecg_record::Status::OK)
            throw std::invalid_argument(ecg_record::statusName(status));
        capture_evidence_ = ecg_record_env_u64(
            "SNIPER_ECG_RECORD_EQUIVALENCE", 0, 0, 1) != 0;
        if (capture_evidence_)
            evidence_.prepare(requirements_, stream_,
                [source](uint64_t index) { return static_cast<uint64_t>(source[index]); },
                [&graph](uint64_t row) { return static_cast<uint64_t>(graph.in_offset(row)); });
        status = ecg_record::packLayout(layout_, descriptor_);
        if (status != ecg_record::Status::OK)
            throw std::invalid_argument(ecg_record::statusName(status));

        configuration_.layout_descriptor = descriptor_;
        configuration_.record_base =
            reinterpret_cast<uint64_t>(stream_.data());
        configuration_.property_base =
            reinterpret_cast<uint64_t>(property_base);
        configuration_.record_count = requirements_.record_count;
        configuration_.vertex_count = requirements_.vertex_count;
        configuration_.generation = ecg_record_env_u64(
            "SNIPER_ECG_RECORD_GENERATION", 1, 1, UINT64_MAX);
        configuration_.context = ecg_record_env_u64(
            "SNIPER_ECG_RECORD_CONTEXT", 1, 1, UINT16_MAX);
        configuration_.control = ecg_record::kNativeEnable |
            (traversal_count > 1 ? ecg_record::kNativeHasNext : 0);
        active_ = true;

        std::fprintf(stderr,
            "[SNIPER-ECG-RECORD-STREAM mechanism=%s record_bytes=%u "
            "id_bits=%u metadata_bits=%u horizon_bits=%u "
            "exponent_bits=%u mantissa_bits=%u sequence_bits=%u "
            "deadline_bits=%u state_encoding=joint-distance "
            "prefetch_selection=record-window vertex_count=%llu "
            "max_vertex_id=%llu records=%llu source_stream_bytes=%llu "
            "retained_source_bytes=%llu "
            "carrier_payload_bytes=%llu carrier_allocation_bytes=%llu "
            "auxiliary_peak_bytes=%llu property_lines=%llu "
            "storage=separate construction=outside-roi "
            "guest_window_records=16 guest_window_data_bits=%llu "
            "guest_window_index_bits=1024 guest_window_valid_bits=16]\n",
            ecg_record::mechanismName(mechanism),
            layout_.record_bytes, layout_.id_bits, layout_.metadata_bits,
            layout_.horizon_bits, layout_.exponent_bits,
            layout_.mantissa_bits, layout_.sequence_bits,
            layout_.deadline_bits,
            static_cast<unsigned long long>(requirements_.vertex_count),
            static_cast<unsigned long long>(requirements_.max_vertex_id),
            static_cast<unsigned long long>(requirements_.record_count),
            static_cast<unsigned long long>(
                stream_.stats.source_stream_bytes),
            static_cast<unsigned long long>(
                stream_.stats.source_stream_bytes),
            static_cast<unsigned long long>(
                stream_.stats.carrier_payload_bytes),
            static_cast<unsigned long long>(
                stream_.stats.carrier_allocation_bytes),
            static_cast<unsigned long long>(
                stream_.stats.auxiliary_peak_bytes),
            static_cast<unsigned long long>(
                stream_.stats.property_lines),
            static_cast<unsigned long long>(
                sizeof(words_) * 8));
    }

    bool active() const { return active_; }
    const ecg_record::Layout& layout() const { return layout_; }
    uint64_t count() const { return requirements_.record_count; }

    void activate()
    {
        if (!active_)
            return;
        ecg_record_configure(configuration_);
    }

    void beginIteration(uint64_t iteration, uint64_t traversal_count)
    {
        if (!active_)
            return;
        uint64_t iteration_base = 0;
        if (!ecg_record::checkedMultiply(
                iteration, requirements_.record_count, iteration_base)) {
            throw std::overflow_error("Sniper ECG record iteration overflow");
        }
        ecg_record_iteration(
            iteration_base, iteration + 1 < traversal_count);
        iteration_ = iteration;
        loaded_mask_ = 0;
        next_load_ = 0;
    }

    uint64_t consume(uint64_t position)
    {
        if (!active_ || position >= requirements_.record_count)
            throw std::out_of_range("Sniper ECG record position");
        ensureWindow(position);
        const std::size_t slot = position % words_.size();
        if ((loaded_mask_ & (uint16_t{1} << slot)) == 0 ||
            indices_[slot] != position) {
            throw std::logic_error("Sniper ECG record window missed current word");
        }
        const volatile void* address = addressAt(position);
        ecg_record_consume(address);
        return words_[slot];
    }

    void finish()
    {
        if (active_)
            ecg_record_drain_report();
    }

    void observeEvidence(uint64_t position, uint64_t raw_word) {
        if (capture_evidence_)
            evidence_.observe(raw_word, position, iteration_);
    }

    void reportEvidence(std::ostream& output) const {
        if (capture_evidence_)
            evidence_.report(output);
    }

  private:
    const volatile void* addressAt(uint64_t index) const
    {
        return stream_.data() + index * layout_.record_bytes;
    }

    void load(uint64_t index)
    {
        const volatile void* address = addressAt(index);
        ecg_record_arm_read(address, layout_.record_bytes);
        uint64_t word = 0;
        if (layout_.record_bytes == 4) {
            word = *reinterpret_cast<const volatile uint32_t*>(address);
        } else {
            word = *reinterpret_cast<const volatile uint64_t*>(address);
        }
        ecg_record_report_loaded(address, word, layout_.record_bytes);
        const std::size_t slot = index % words_.size();
        words_[slot] = word;
        indices_[slot] = index;
        loaded_mask_ |= uint16_t{1} << slot;
    }

    void ensureWindow(uint64_t position)
    {
        const uint64_t end = std::min<uint64_t>(
            requirements_.record_count,
            position + ecg_record::RecordWindow::kRecords);
        if (next_load_ < position)
            next_load_ = position;
        while (next_load_ < end)
            load(next_load_++);
    }

    const GraphT& graph_;
    ecg_record::Requirements requirements_;
    ecg_record::Layout layout_;
    ecg_record::RecordStream stream_;
    ecg_record::NativeConfiguration configuration_;
    std::array<uint64_t, ecg_record::RecordWindow::kRecords> words_{};
    std::array<uint64_t, ecg_record::RecordWindow::kRecords> indices_{};
    uint64_t descriptor_ = 0;
    uint64_t next_load_ = 0;
    uint16_t loaded_mask_ = 0;
    uint64_t iteration_ = 0;
    ecg_record::EquivalenceEvidence evidence_;
    bool capture_evidence_ = false;
    bool active_ = false;
};

}  // namespace graphbrew_sniper

#endif
