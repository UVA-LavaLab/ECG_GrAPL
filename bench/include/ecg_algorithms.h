#ifndef GRAPHBREW_ECG_ALGORITHMS_H
#define GRAPHBREW_ECG_ALGORITHMS_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ecg_record_evidence.h"
#include "ecg_record_native.h"
#include "ecg_window.h"

namespace ecg_algorithm {

enum class Algorithm : uint8_t { SPMV, BFS, SSSP, CC, BC, TC };
enum class MemoryKind : uint8_t { INDEX, EDGE, WEIGHT, PROPERTY, AUXILIARY, CONSTRUCTION };
enum class ReferencePattern : uint8_t { NEIGHBOR, VERTEX, NEIGHBOR_AND_VERTEX };
enum class RecordBasePolicy : uint8_t { LRU, GRASP_PAPER };
enum class WindowObserverMode : uint8_t { OFF, CONTROL, WINDOW };
enum class RecordModel : uint8_t { NEXT, WINDOW };

inline const char* recordBasePolicyName(RecordBasePolicy policy) {
    switch (policy) {
      case RecordBasePolicy::LRU: return "LRU";
      case RecordBasePolicy::GRASP_PAPER: return "GRASP_PAPER";
    }
    throw std::invalid_argument("invalid-record-base-policy");
}

inline const char* name(Algorithm algorithm) {
    switch (algorithm) {
      case Algorithm::SPMV: return "spmv";
      case Algorithm::BFS: return "bfs";
      case Algorithm::SSSP: return "sssp";
      case Algorithm::CC: return "cc";
      case Algorithm::BC: return "bc";
      case Algorithm::TC: return "tc";
    }
    throw std::invalid_argument("invalid-algorithm");
}

inline const char* variant(Algorithm algorithm) {
    switch (algorithm) {
      case Algorithm::SPMV: return "csr-fixed-input-f32";
      case Algorithm::BFS: return "sorted-level-top-down";
      case Algorithm::SSSP: return "sorted-serial-delta-stepping-u64";
      case Algorithm::CC: return "deterministic-afforest";
      case Algorithm::BC: return "sorted-single-source-list-brandes-u64-f32";
      case Algorithm::TC: return "degree-oriented-node-iterator";
    }
    throw std::invalid_argument("invalid-algorithm");
}

struct Options {
    Algorithm algorithm = Algorithm::SPMV;
    bool records = false;
    bool evidence = false;
    bool capture_values = false;
    bool traversal_preprocessing = false;
    RecordModel record_model = RecordModel::NEXT;
    uint8_t window_candidate_rrpv = 6;
    WindowObserverMode window_observer = WindowObserverMode::OFF;
    uint64_t maximum_window_observer_bytes = uint64_t{128} << 20;
    uint8_t record_bytes = 0;
    uint8_t minimum_mantissa_bits = 0;
    uint32_t source = 0;
    uint64_t repetitions = 1;
    uint64_t delta = 1;
    bool bfs_direction_optimizing = false;
    uint64_t bfs_alpha = 15, bfs_beta = 18;
    uint64_t maximum_passes = 1000000;
    uint64_t maximum_workspace_bytes = uint64_t{512} << 20;
    ecg_record::BuildLimits build_limits;
    ecg_record::Mechanism mechanism = ecg_record::Mechanism::TRANSPORT;
    RecordBasePolicy record_base_policy = RecordBasePolicy::LRU;
    std::vector<uint32_t> sources;
};

inline const char* recordReuseScope(const Options& options) {
    if (!options.records)
        return "none";
    if (options.record_model == RecordModel::WINDOW)
        return "source-cohort-window";
    if (options.traversal_preprocessing) {
        switch (options.algorithm) {
          case Algorithm::SSSP: return "light-heavy";
          case Algorithm::CC: return "sample-rounds";
          case Algorithm::BFS:
          case Algorithm::BC: return "row-local";
          case Algorithm::SPMV:
          case Algorithm::TC: break;
        }
    }
    return "full-csr";
}

struct Result {
    Algorithm algorithm = Algorithm::SPMV;
    uint64_t vertices = 0, source_edges = 0, carrier_records = 0;
    uint64_t passes = 0, structural_positions = 0, actual_records = 0, skipped_positions = 0;
    uint64_t csr_index_reads = 0, edge_reads = 0, weight_reads = 0;
    uint64_t ordinary_property_reads = 0, property_writes = 0, auxiliary_accesses = 0;
    uint64_t construction_reads = 0, construction_writes = 0;
    uint64_t carrier_bytes = 0, construction_auxiliary_peak_bytes = 0, workspace_peak_bytes = 0;
    uint64_t constructed_finite_records = 0, constructed_wrap_records = 0, constructed_unknown_records = 0;
    uint64_t result_digest = 0, work_digest = 0, position_digest = 0, record_digest = 0;
    uint64_t reached = 0, levels = 0, components = 0, relax_attempts = 0, relax_successes = 0;
    uint64_t light_passes = 0, heavy_passes = 0, sigma_max = 0;
    uint64_t bfs_td_levels = 0, bfs_bu_levels = 0, bfs_td_edges = 0, bfs_bu_edges = 0;
    uint64_t bfs_bu_vertices = 0, bfs_frontier_peak = 0, bfs_direction_switches = 0;
    uint64_t triangles = 0, oriented_edges = 0, intersection_comparisons = 0, bindings = 0;
    uint64_t source_list_digest = 0;
    uint64_t maximum_encoded_id = 0;
    ecg_record::Layout layout;
    ecg_window::Layout window_layout;
    uint64_t window_known_records = 0;
    bool weighted = false, records = false, evidence = false, memory_counts_measured = false;
    std::vector<uint32_t> values_u32;
    std::vector<uint64_t> values_u64;
    std::vector<float> values_f32;
};

struct GraphView {
    uint64_t vertices = 0, records = 0;
    bool directed = true;
    const void* offsets = nullptr;
    const void* columns = nullptr;
    const void* weights = nullptr;
    uint32_t edge_stride = 4;
    bool pointer_offsets = false;
    const void* in_offsets = nullptr;
    const void* in_columns = nullptr;

    GraphView incoming() const {
        if (!directed)
            return *this;
        if (!in_offsets || (records && !in_columns))
            throw std::invalid_argument("direction-optimized BFS requires incoming CSR");
        return {vertices, records, true, in_offsets, in_columns, nullptr, edge_stride,
                pointer_offsets, offsets, columns};
    }

    template<class Access>
    uint64_t offset(Access& access, uint64_t row,
                    MemoryKind kind = MemoryKind::INDEX, bool trace = true) const {
        if (row > vertices)
            throw std::out_of_range("csr-row");
        const auto* address = static_cast<const uint8_t*>(offsets) + row * sizeof(uint64_t);
        const uint64_t raw = access.template read<uint64_t>(
            address, kind, 0, row, trace);
        uint64_t result = raw;
        if (pointer_offsets) {
            const uint64_t base = reinterpret_cast<uint64_t>(columns);
            if (raw < base || (raw - base) % edge_stride != 0)
                throw std::invalid_argument("invalid-csr-pointer");
            result = (raw - base) / edge_stride;
        }
        if (result > records)
            throw std::invalid_argument("invalid-csr-offset");
        return result;
    }

    template<class Access>
    std::pair<uint64_t, uint64_t> row(Access& access, uint64_t vertex) const {
        const uint64_t first = offset(access, vertex);
        const uint64_t last = offset(access, vertex + 1);
        if (last < first)
            throw std::invalid_argument("invalid-csr-order");
        return {first, last};
    }

    template<class Access>
    uint32_t id(Access& access, uint64_t index,
                MemoryKind kind = MemoryKind::EDGE, bool trace = true) const {
        if (index >= records)
            throw std::out_of_range("csr-edge");
        const auto* address = static_cast<const uint8_t*>(columns) + index * edge_stride;
        const int32_t value = access.template read<int32_t>(address, kind, 1, index, trace);
        if (value < 0 || uint64_t(value) >= vertices)
            throw std::invalid_argument("invalid-neighbor-id");
        return static_cast<uint32_t>(value);
    }

    template<class Access>
    int32_t weight(Access& access, uint64_t index,
                   MemoryKind kind = MemoryKind::WEIGHT, bool trace = true) const {
        if (!weights)
            return 1;
        if (index >= records)
            throw std::out_of_range("csr-weight");
        const auto* address = static_cast<const uint8_t*>(weights) + index * edge_stride;
        return access.template read<int32_t>(address, kind, 2, index, trace);
    }
};

class PlainBackend {
  public:
    static constexpr bool models_memory = false;
    void start(const GraphView&) {}
    void memory(const void*, uint64_t, bool) {}
    void region(const char*, const void*, uint64_t, uint8_t, bool) {}
    void selectProperty(const GraphView&, const void*, const ecg_record::PropertyDescriptor&) {}
    void bind(const ecg_record::NativeConfiguration& configuration,
              const ecg_record::RecordStream& stream) {
        configuration_ = configuration;
        stream_ = &stream;
        base_ = 0;
    }
    void beginPass(bool has_next) {
        configuration_.iteration_base = base_;
        configuration_.control = ecg_record::kNativeEnable | ecg_record::kNativeManagedPasses |
            (has_next ? ecg_record::kNativeHasNext : 0);
    }
    void closePass() { base_ += configuration_.record_count; }
    uint64_t recordLoad(uint64_t index) { return stream_->word(index); }
    template<class T>
    T property(uint64_t index, uint64_t word, const T* base) {
        ecg_record::NativeLoadResult load;
        if (ecg_record::nativePropertyAccess(configuration_, reinterpret_cast<uint64_t>(base),
                word, configuration_.record_base + index * stream_->layout.record_bytes, load) !=
                ecg_record::Status::OK || load.property_bytes != sizeof(T))
            throw std::logic_error("invalid-typed-property");
        T result;
        std::memcpy(&result, reinterpret_cast<const void*>(load.property_address), sizeof(T));
        return result;
    }
    void drain() {}
    void finish(uint64_t) {}

  private:
    ecg_record::NativeConfiguration configuration_;
    const ecg_record::RecordStream* stream_ = nullptr;
    uint64_t base_ = 0;
};

template<class Backend> class Engine;

template<class T, class Access>
class Array {
  public:
    Array(Access& access, uint64_t count, const char* name, bool property = false,
          ReferencePattern references = ReferencePattern::NEIGHBOR)
        : access_(access), count_(count), property_(property), token_(access.nextToken()),
          storage_(nullptr, DeleteStorage{&access, 0, std::align_val_t{64}}) {
        static_assert(std::is_trivially_copyable<T>::value, "algorithm arrays hold scalar data");
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::overflow_error("array-size-overflow");
        access_.reserve(count * sizeof(T));
        storage_.get_deleter().bytes = count * sizeof(T);
        storage_.get_deleter().alignment = alignment();
        try {
            storage_.reset(static_cast<T*>(::operator new(
                static_cast<std::size_t>(count * sizeof(T)), alignment())));
        } catch (const std::bad_alloc&) {
            access_.release(count * sizeof(T));
            throw;
        }
        data_ = storage_.get();
        access_.region(name, data_, count, sizeof(T), property, references);
    }
    ~Array() = default;
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    T* data() const { return data_; }
    uint64_t size() const { return count_; }
    uint64_t token() const { return token_; }
    T get(uint64_t index) const {
        bounds(index);
        return access_.template read<T>(data_ + index, kind(), token_, index);
    }
    void set(uint64_t index, T value) {
        bounds(index);
        access_.touch(data_ + index, sizeof(T), true, kind(), token_, index);
        data_[index] = value;
    }
    void fill(T value) {
        for (uint64_t index = 0; index < count_; ++index)
            set(index, value);
    }
    void swap(Array& other) {
        if (&access_ != &other.access_ || property_ != other.property_)
            throw std::logic_error("incompatible-array-swap");
        std::swap(data_, other.data_);
        std::swap(count_, other.count_);
        std::swap(token_, other.token_);
        storage_.swap(other.storage_);
    }

  private:
    struct DeleteStorage {
        Access* access;
        uint64_t bytes;
        std::align_val_t alignment;
        void operator()(T* data) const {
            ::operator delete(data, alignment);
            access->release(bytes);
        }
    };
    std::align_val_t alignment() const {
        return std::align_val_t(property_ ? 2 * 1024 * 1024 : 64);
    }
    MemoryKind kind() const { return property_ ? MemoryKind::PROPERTY : MemoryKind::AUXILIARY; }
    void bounds(uint64_t index) const {
        if (index >= count_)
            throw std::out_of_range("algorithm-array-index");
    }
    Access& access_;
    uint64_t count_;
    bool property_;
    uint64_t token_;
    T* data_ = nullptr;
    std::unique_ptr<T, DeleteStorage> storage_;
};

template<class Backend>
class Engine {
  public:
    template<class T> using Buffer = Array<T, Engine>;
    Engine(const GraphView& graph, const Options& options, Backend& backend, Result& result)
        : options(options), result(result), backend_(backend) {
        result.algorithm = options.algorithm;
        result.vertices = graph.vertices;
        result.source_edges = graph.records;
        result.weighted = graph.weights != nullptr;
        result.records = options.records;
        result.evidence = options.evidence;
        result.memory_counts_measured = options.evidence || Backend::models_memory;
        backend_.start(graph);
    }
    const Options& options;
    Result& result;

    uint64_t nextToken() { return next_token_++; }
    void reserve(uint64_t bytes) {
        if (bytes > options.maximum_workspace_bytes - workspace_)
            throw std::length_error("algorithm-workspace-limit");
        workspace_ += bytes;
        result.workspace_peak_bytes = std::max(result.workspace_peak_bytes, workspace_);
    }
    void release(uint64_t bytes) { workspace_ -= bytes; }
    void region(const char* name, const void* base, uint64_t count, uint8_t bytes, bool property,
                ReferencePattern references = ReferencePattern::NEIGHBOR) {
        backend_.region(name, base, count, bytes, property);
        if constexpr (Backend::models_memory) {
            if (property)
                backend_.propertyReferences(base, references);
        }
    }

    void touch(const void* address, uint64_t bytes, bool write, MemoryKind kind,
               uint64_t token = 0, uint64_t index = 0, bool trace = true, bool model = true) {
        if (model)
            backend_.memory(address, bytes, write);
        if (!result.memory_counts_measured)
            return;
        switch (kind) {
          case MemoryKind::INDEX: ++result.csr_index_reads; break;
          case MemoryKind::EDGE: ++result.edge_reads; break;
          case MemoryKind::WEIGHT: ++result.weight_reads; break;
          case MemoryKind::PROPERTY:
            if (write) ++result.property_writes;
            else ++result.ordinary_property_reads;
            break;
          case MemoryKind::AUXILIARY: ++result.auxiliary_accesses; break;
          case MemoryKind::CONSTRUCTION:
            if (write) result.construction_writes += bytes;
            else result.construction_reads += bytes;
            break;
        }
        if (options.evidence && trace && kind != MemoryKind::CONSTRUCTION) {
            work_.add(static_cast<uint64_t>(kind));
            work_.add(write);
            work_.add(token);
            work_.add(index);
            work_.add(bytes);
        }
    }
    template<class T>
    T read(const void* address, MemoryKind kind, uint64_t token, uint64_t index, bool trace = true) {
        touch(address, sizeof(T), false, kind, token, index, trace);
        T value;
        std::memcpy(&value, address, sizeof(T));
        return value;
    }

    template<class T>
    void bind(const GraphView& graph, Buffer<T>& property, ecg_record::TraversalMode mode) {
        if (pass_open_)
            throw std::logic_error("binding-inside-pass");
        if (property.size() != graph.vertices)
            throw std::invalid_argument("property-domain-mismatch");
        ecg_record::PropertyKind kind;
        if constexpr (std::is_same<T, float>::value)
            kind = ecg_record::PropertyKind::F32;
        else if constexpr (std::is_same<T, uint32_t>::value)
            kind = ecg_record::PropertyKind::U32;
        else {
            static_assert(std::is_same<T, uint64_t>::value, "only real F32/U32/U64 properties");
            kind = ecg_record::PropertyKind::U64;
        }
        property_ = {kind, sizeof(T), mode};
        graph_ = graph;
        cursor_ = ecg_record::PassCursor{};
        if (graph.records && cursor_.configure(graph.records, mode) != ecg_record::Status::OK)
            throw std::logic_error("invalid-pass-domain");
        ++result.bindings;
        result.carrier_records = graph.records;
        backend_.selectProperty(graph, property.data(), property_);
        if (!options.records || graph.records == 0)
            return;
        backend_.drain();
        const uint64_t base = reinterpret_cast<uint64_t>(property.data());
        if (options.record_model == RecordModel::WINDOW) {
            if constexpr (Backend::models_memory) {
                if (window_stream_ || kind != ecg_record::PropertyKind::U32 || sizeof(T) != 4 ||
                    mode != ecg_record::TraversalMode::ORDERED_FILTERED || base % 64)
                    throw std::invalid_argument("window model requires one aligned U32 binding");
                uint64_t maximum = 0, payload = 0;
                for (uint64_t index = 0; index < graph.records; ++index)
                    maximum = std::max<uint64_t>(maximum,
                        graph.id(*this, index, MemoryKind::CONSTRUCTION, false));
                const auto layout = ecg_window::Layout::select(maximum, options.record_bytes);
                if (!ecg_record::checkedMultiply(graph.records, layout.record_bytes, payload) ||
                    payload > options.maximum_workspace_bytes - workspace_)
                    throw std::length_error("window-carrier-workspace-limit");
                auto limits = options.build_limits;
                limits.maximum_auxiliary_bytes = std::min(limits.maximum_auxiliary_bytes,
                    options.maximum_workspace_bytes - workspace_ - payload);
                window_stream_ = std::make_unique<ecg_window::RecordStream>(ecg_window::build(
                    ecg_window::Profile(graph.vertices), graph.records, layout, limits,
                    [&](uint64_t row) {
                        const uint64_t first = graph.offset(*this, row, MemoryKind::CONSTRUCTION, false);
                        const uint64_t last = graph.offset(*this, row + 1, MemoryKind::CONSTRUCTION, false);
                        return std::pair<uint64_t, uint64_t>{first, last};
                    },
                    [&](uint64_t index) { return graph.id(*this, index, MemoryKind::CONSTRUCTION, false); },
                    [&](const void* address, uint64_t bytes, bool write) {
                        touch(address, bytes, write, MemoryKind::CONSTRUCTION, 0, 0, false);
                    }));
                const auto& stats = window_stream_->stats;
                result.workspace_peak_bytes = std::max(result.workspace_peak_bytes,
                    workspace_ + stats.carrier_allocation_bytes + stats.auxiliary_peak_bytes);
                reserve(stats.carrier_allocation_bytes);
                result.carrier_bytes = stats.carrier_allocation_bytes;
                result.construction_auxiliary_peak_bytes = stats.auxiliary_peak_bytes;
                result.maximum_encoded_id = maximum;
                result.window_layout = layout;
                result.window_known_records = window_stream_->known_records;
                result.constructed_unknown_records = graph.records - window_stream_->known_records;
                id_mask_ = layout.idMask();
                if (options.evidence) {
                    records_.add(0x57494e444f573031ULL);
                    records_.add(layout.record_bytes);
                    records_.add(layout.id_bits);
                    records_.add(graph.records);
                }
                backend_.bindWindow(*window_stream_, graph.vertices, base);
                return;
            } else {
                throw std::invalid_argument("window model is cache_sim-only");
            }
        }
        Carrier* carrier = nullptr;
        for (const auto& prepared : carriers_) {
            if (prepared->columns == graph.columns && prepared->records == graph.records &&
                prepared->vertices == graph.vertices && prepared->stride == sizeof(T) &&
                prepared->line_offset == base % 64 && prepared->mode == mode) {
                carrier = prepared.get();
                break;
            }
        }
        if (!carrier) {
            auto prepared = std::make_unique<Carrier>();
            prepared->columns = graph.columns;
            prepared->records = graph.records;
            prepared->vertices = graph.vertices;
            prepared->stride = sizeof(T);
            prepared->line_offset = base % 64;
            prepared->mode = mode;
            ecg_record::Requirements requirements;
            requirements.vertex_count = graph.vertices;
            requirements.record_count = graph.records;
            requirements.traversal_count =
                mode == ecg_record::TraversalMode::DENSE_EXACT ? options.repetitions : 1;
            requirements.max_vertex_id_known = true;
            requirements.requested_record_bytes = options.record_bytes;
            requirements.minimum_mantissa_bits = options.minimum_mantissa_bits;
            for (uint64_t index = 0; index < graph.records; ++index)
                requirements.max_vertex_id = std::max<uint64_t>(requirements.max_vertex_id,
                    graph.id(*this, index, MemoryKind::CONSTRUCTION, false));
            ecg_record::Layout layout;
            auto status = ecg_record::selectLayout(requirements, layout);
            if (status != ecg_record::Status::OK)
                throw std::invalid_argument(ecg_record::statusName(status));
            result.maximum_encoded_id = requirements.max_vertex_id;
            result.layout = layout;
            uint64_t payload = 0;
            if (!ecg_record::checkedMultiply(graph.records, layout.record_bytes, payload) ||
                payload > options.maximum_workspace_bytes - workspace_)
                throw std::length_error("carrier-workspace-limit");
            auto limits = options.build_limits;
            limits.maximum_carrier_bytes = std::min(limits.maximum_carrier_bytes, payload);
            limits.maximum_auxiliary_bytes = std::min(limits.maximum_auxiliary_bytes,
                options.maximum_workspace_bytes - workspace_ - payload);
            uint64_t row = 0, first = 0, last = 0;
            bool row_ready = false;
            const auto scope_at = [&](uint64_t index) -> ecg_record::BuildScope {
                if (!options.traversal_preprocessing || options.algorithm == Algorithm::SPMV ||
                    options.algorithm == Algorithm::TC)
                    return {};
                if (options.algorithm == Algorithm::SSSP) {
                    const uint64_t weight = static_cast<uint32_t>(
                        graph.weight(*this, index, MemoryKind::CONSTRUCTION, false));
                    return {static_cast<uint8_t>(weight > options.delta), UINT64_MAX};
                }
                if (!row_ready) {
                    row = index == 0 ? 0 : graph.vertices - 1;
                    first = graph.offset(*this, row, MemoryKind::CONSTRUCTION, false);
                    last = graph.offset(*this, row + 1, MemoryKind::CONSTRUCTION, false);
                    row_ready = true;
                }
                while (index >= last) {
                    first = last;
                    last = graph.offset(*this, ++row + 1, MemoryKind::CONSTRUCTION, false);
                }
                while (index < first) {
                    last = first;
                    first = graph.offset(*this, --row, MemoryKind::CONSTRUCTION, false);
                }
                if (options.algorithm == Algorithm::CC)
                    return {static_cast<uint8_t>(std::min<uint64_t>(index - first, 2)), UINT64_MAX};
                return {0, last};
            };
            const auto build = [&](auto partitions) {
                return ecg_record::buildRecords<decltype(partitions)::value>(
                    requirements, layout, property_, base,
                    [&](uint64_t index) { return graph.id(*this, index, MemoryKind::CONSTRUCTION, false); },
                    prepared->stream, limits,
                    [&](const void* address, uint64_t bytes, bool write) {
                        touch(address, bytes, write, MemoryKind::CONSTRUCTION, 0, 0, false);
                    }, scope_at);
            };
            if (options.traversal_preprocessing && options.algorithm == Algorithm::SSSP)
                status = build(std::integral_constant<std::size_t, 2>{});
            else if (options.traversal_preprocessing && options.algorithm == Algorithm::CC)
                status = build(std::integral_constant<std::size_t, 3>{});
            else
                status = build(std::integral_constant<std::size_t, 1>{});
            if (status != ecg_record::Status::OK)
                throw std::length_error(std::string("record-construction-") + ecg_record::statusName(status));
            const auto& stats = prepared->stream.stats;
            result.workspace_peak_bytes = std::max(result.workspace_peak_bytes,
                workspace_ + stats.carrier_allocation_bytes + stats.auxiliary_peak_bytes);
            reserve(stats.carrier_allocation_bytes);
            result.carrier_bytes += stats.carrier_allocation_bytes;
            result.construction_auxiliary_peak_bytes =
                std::max(result.construction_auxiliary_peak_bytes, stats.auxiliary_peak_bytes);
            result.constructed_finite_records += stats.finite_records;
            result.constructed_wrap_records += stats.wrap_records;
            result.constructed_unknown_records += stats.unknown_records;
            carrier = prepared.get();
            carriers_.push_back(std::move(prepared));
        }
        stream_ = &carrier->stream;
        id_mask_ = ecg_record::lowMask(stream_->layout.id_bits);
        ecg_record::NativeConfiguration configuration;
        ecg_record::packLayout(stream_->layout, configuration.layout_descriptor);
        ecg_record::packProperty(property_, configuration.property_descriptor);
        configuration.record_base = reinterpret_cast<uint64_t>(stream_->data());
        configuration.property_base = base;
        configuration.record_count = graph.records;
        configuration.vertex_count = graph.vertices;
        configuration.context = 1;
        configuration.generation = result.bindings;
        configuration.control = ecg_record::kNativeEnable | ecg_record::kNativeManagedPasses;
        configuration_ = configuration;
        if (options.evidence) {
            records_.add(configuration.layout_descriptor);
            records_.add(configuration.property_descriptor);
            records_.add(configuration.record_count);
            records_.add(configuration.generation);
        }
        backend_.bind(configuration, *stream_);
    }

    void beginPass(bool has_next = false) {
        if (pass_open_ || result.passes >= options.maximum_passes ||
            (graph_.records && cursor_.begin() != ecg_record::Status::OK))
            throw std::logic_error("algorithm-pass-limit-or-order");
        pass_open_ = true;
        pass_records_ = 0;
        if constexpr (Backend::models_memory)
            backend_.beginGraphPass();
        configuration_.iteration_base = cursor_.base();
        configuration_.control = ecg_record::kNativeEnable | ecg_record::kNativeManagedPasses |
            (has_next ? ecg_record::kNativeHasNext : 0);
        if (options.records && graph_.records)
            backend_.beginPass(has_next);
        if (options.evidence) {
            positions_.add(result.bindings);
            positions_.add(result.passes);
            positions_.add(graph_.records);
        }
    }

    void visitVertex(uint64_t vertex) {
        if constexpr (Backend::models_memory)
            backend_.visitVertex(vertex);
    }
    void rowContext(uint64_t vertex, uint64_t first, uint64_t last) {
        if constexpr (Backend::models_memory)
            backend_.rowContext(vertex, first, last);
    }

    template<class T>
    void writeNeighbor(uint64_t index, Buffer<T>& property, uint32_t destination, T value) {
        if constexpr (Backend::models_memory)
            backend_.associateNeighborWrite(index, destination, property.data(), sizeof(T));
        property.set(destination, value);
    }

    template<class T>
    std::pair<uint32_t, T> neighbor(uint64_t index, Buffer<T>& property) {
        if (!pass_open_ || cursor_.consume(index) != ecg_record::Status::OK)
            throw std::logic_error("nonmonotone-governed-reference");
        if constexpr (Backend::models_memory)
            backend_.governedReference();
        uint64_t word = 0;
        uint32_t destination;
        if (options.records) {
            word = backend_.recordLoad(index);
            const uint64_t id = word & id_mask_;
            if (id >= property.size())
                throw std::logic_error("invalid-actual-record");
            destination = static_cast<uint32_t>(id);
        } else {
            destination = graph_.id(*this, index, MemoryKind::EDGE, false);
        }
        if (options.evidence) {
            work_.add(UINT64_MAX);
            work_.add(index);
            work_.add(destination);
            positions_.add(index);
            positions_.add(destination);
            positions_.add(cursor_.sequence());
        }
        if constexpr (Backend::models_memory)
            backend_.designatedProperty(index, destination);
        touch(property.data() + destination, sizeof(T), false, MemoryKind::PROPERTY,
            property.token(), destination, true, !options.records);
        if (result.memory_counts_measured)
            --result.ordinary_property_reads;
        const T value = options.records
            ? backend_.template property<T>(index, word, property.data()) : property.data()[destination];
        if (options.records && options.evidence) {
            if (options.record_model == RecordModel::WINDOW) {
                if constexpr (Backend::models_memory) {
                    records_.add(word);
                    records_.add(index);
                    records_.add(cursor_.sequence());
                    records_.add(backend_.windowTraceSource());
                    records_.add(backend_.windowTraceValue());
                }
            } else {
                ecg_record::NativeLoadResult observed;
                if (ecg_record::nativePropertyAccess(configuration_,
                        reinterpret_cast<uint64_t>(property.data()), word,
                        configuration_.record_base + index * stream_->layout.record_bytes, observed) !=
                        ecg_record::Status::OK)
                    throw std::logic_error("invalid-observed-record-semantics");
                records_.add(word);
                records_.add(index);
                records_.add(observed.sequence);
                records_.add(static_cast<uint64_t>(observed.state));
                records_.add(observed.deadline);
            }
        }
        ++result.actual_records;
        ++pass_records_;
        return {destination, value};
    }

    void closePass() {
        if (!pass_open_ || (graph_.records && cursor_.close() != ecg_record::Status::OK))
            throw std::logic_error("incomplete-algorithm-pass");
        if (!ecg_record::checkedAdd(result.structural_positions, graph_.records,
                result.structural_positions) ||
            !ecg_record::checkedAdd(result.skipped_positions, graph_.records - pass_records_,
                result.skipped_positions))
            throw std::overflow_error("structural-count-overflow");
        if (options.records && graph_.records)
            backend_.closePass();
        if constexpr (Backend::models_memory)
            backend_.endGraphPass();
        ++result.passes;
        pass_open_ = false;
    }

    void complete() {
        if (pass_open_ || result.actual_records + result.skipped_positions != result.structural_positions)
            throw std::logic_error("incomplete-algorithm-work");
        backend_.finish(result.actual_records);
        result.work_digest = options.evidence ? work_.value() : 0;
        result.position_digest = options.evidence ? positions_.value() : 0;
        result.record_digest = options.evidence && options.records ? records_.value() : 0;
    }

    void drainRecords() {
        if (options.records && graph_.records)
            backend_.drain();
    }

    template<class T>
    void capture(const Buffer<T>& values) {
        ecg_record::StreamDigest digest;
        digest.add(values.size());
        for (uint64_t index = 0; index < values.size(); ++index) {
            uint64_t bits = 0;
            std::memcpy(&bits, values.data() + index, sizeof(T));
            digest.add(bits);
        }
        result.result_digest = digest.value();
        if (options.capture_values) {
            if constexpr (std::is_same<T, uint32_t>::value)
                result.values_u32.assign(values.data(), values.data() + values.size());
            else if constexpr (std::is_same<T, uint64_t>::value)
                result.values_u64.assign(values.data(), values.data() + values.size());
            else
                result.values_f32.assign(values.data(), values.data() + values.size());
        }
    }

  private:
    struct Carrier {
        const void* columns = nullptr;
        uint64_t vertices = 0, records = 0, stride = 0, line_offset = 0;
        ecg_record::TraversalMode mode = ecg_record::TraversalMode::DENSE_EXACT;
        ecg_record::RecordStream stream;
    };
    Backend& backend_;
    GraphView graph_;
    ecg_record::PropertyDescriptor property_;
    ecg_record::NativeConfiguration configuration_;
    ecg_record::PassCursor cursor_;
    const ecg_record::RecordStream* stream_ = nullptr;
    std::unique_ptr<ecg_window::RecordStream> window_stream_;
    std::vector<std::unique_ptr<Carrier>> carriers_;
    ecg_record::StreamDigest work_, positions_, records_;
    uint64_t workspace_ = 0, next_token_ = 3, pass_records_ = 0, id_mask_ = 0;
    bool pass_open_ = false;
};

template<class Access>
void validateGraph(const GraphView& graph, Access& access, bool simple) {
    if (graph.vertices == 0 || graph.vertices > INT32_MAX || !graph.offsets ||
        (graph.records && !graph.columns) || graph.edge_stride < sizeof(int32_t) ||
        graph.edge_stride % sizeof(int32_t) != 0)
        throw std::invalid_argument("invalid-graph-domain");
    if (simple && graph.directed)
        throw std::invalid_argument("requires-simple-undirected");
    if (graph.offset(access, 0) != 0 || graph.offset(access, graph.vertices) != graph.records)
        throw std::invalid_argument("invalid-csr-endpoints");
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto row = graph.row(access, vertex);
        uint32_t previous = 0;
        bool seen = false;
        for (uint64_t index = row.first; index < row.second; ++index) {
            const uint32_t neighbor = graph.id(access, index);
            if (simple && (neighbor == vertex || (seen && neighbor <= previous)))
                throw std::invalid_argument("requires-simple-undirected");
            if (simple) {
                auto reverse = graph.row(access, neighbor);
                while (reverse.first < reverse.second) {
                    const uint64_t middle = reverse.first + (reverse.second - reverse.first) / 2;
                    if (graph.id(access, middle) < vertex)
                        reverse.first = middle + 1;
                    else
                        reverse.second = middle;
                }
                const auto neighbor_row = graph.row(access, neighbor);
                if (reverse.first == neighbor_row.second || graph.id(access, reverse.first) != vertex)
                    throw std::invalid_argument("requires-simple-undirected");
            }
            previous = neighbor;
            seen = true;
        }
    }
}

template<class Buffer>
void sortPrefix(Buffer& values, uint64_t count) {
    const auto sift = [&](uint64_t root, uint64_t end) {
        for (;;) {
            uint64_t child = root * 2 + 1;
            if (child >= end)
                return;
            uint32_t child_value = values.get(child);
            if (child + 1 < end) {
                const uint32_t next = values.get(child + 1);
                if (child_value < next) {
                    ++child;
                    child_value = next;
                }
            }
            const uint32_t root_value = values.get(root);
            if (root_value >= child_value)
                return;
            values.set(root, child_value);
            values.set(child, root_value);
            root = child;
        }
    };
    for (uint64_t root = count / 2; root-- > 0;)
        sift(root, count);
    for (uint64_t end = count; end > 1;) {
        --end;
        const uint32_t first = values.get(0), last = values.get(end);
        values.set(0, last);
        values.set(end, first);
        sift(0, end);
    }
}

template<class Access>
void spmv(const GraphView& graph, Access& access) {
    typename Access::template Buffer<float> x(access, graph.vertices, "x", true);
    typename Access::template Buffer<float> y(
        access, graph.vertices, "y", true, ReferencePattern::VERTEX);
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex)
        x.set(vertex, static_cast<float>(vertex + 1));
    access.bind(graph, x, ecg_record::TraversalMode::DENSE_EXACT);
    for (uint64_t pass = 0; pass < access.options.repetitions; ++pass) {
        access.beginPass(pass + 1 < access.options.repetitions);
        for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
            access.visitVertex(vertex);
            float sum = 0;
            const auto row = graph.row(access, vertex);
            for (uint64_t index = row.first; index < row.second; ++index) {
                const auto item = access.neighbor(index, x);
                const float product = static_cast<float>(graph.weight(access, index)) * item.second;
                sum += product;
            }
            y.set(vertex, sum);
        }
        access.closePass();
    }
    access.complete();
    access.capture(y);
}

struct BfsStep {
    uint64_t size = 0, scouts = 0;
};

template<class Access>
BfsStep bfsTopDownLevel(
        const GraphView& graph, Access& access,
        typename Access::template Buffer<uint32_t>& depth,
        const typename Access::template Buffer<uint32_t>& frontier,
        typename Access::template Buffer<uint32_t>& next,
        uint64_t size, uint32_t level, bool count_scouts) {
    BfsStep step;
    access.beginPass();
    ++access.result.bfs_td_levels;
    for (uint64_t position = 0; position < size; ++position) {
        const uint32_t vertex = frontier.get(position);
        access.visitVertex(vertex);
        const auto row = graph.row(access, vertex);
        access.rowContext(vertex, row.first, row.second);
        for (uint64_t index = row.first; index < row.second; ++index) {
            const auto item = access.neighbor(index, depth);
            ++access.result.bfs_td_edges;
            if (item.second == UINT32_MAX) {
                access.writeNeighbor(index, depth, item.first, level + 1);
                next.set(step.size++, item.first);
                ++access.result.reached;
                if (count_scouts) {
                    const auto target_row = graph.row(access, item.first);
                    step.scouts += target_row.second - target_row.first;
                }
            }
        }
    }
    access.closePass();
    return step;
}

template<class Buffer>
void setFrontierBit(Buffer& bitmap, uint32_t vertex) {
    const uint64_t word = vertex / 64;
    bitmap.set(word, bitmap.get(word) | (uint64_t{1} << (vertex % 64)));
}

template<class Access>
void bfsDirectionOptimizing(const GraphView& graph, Access& access) {
    typename Access::template Buffer<uint32_t> depth(access, graph.vertices, "depth", true);
    typename Access::template Buffer<uint32_t> frontier(access, graph.vertices, "frontier");
    typename Access::template Buffer<uint32_t> next(access, graph.vertices, "next_frontier");
    const uint64_t words = (graph.vertices + 63) / 64;
    typename Access::template Buffer<uint64_t> front_bits(access, words, "frontier_bits");
    typename Access::template Buffer<uint64_t> next_bits(access, words, "next_frontier_bits");
    const GraphView incoming = graph.incoming();
    depth.fill(UINT32_MAX);
    depth.set(access.options.source, 0);
    frontier.set(0, access.options.source);
    uint64_t size = 1;
    uint32_t level = 0;
    const auto source_row = graph.row(access, access.options.source);
    uint64_t scouts = source_row.second - source_row.first;
    uint64_t remaining_edges = graph.records;
    access.result.reached = 1;
    access.result.bfs_frontier_peak = 1;
    access.bind(graph, depth, ecg_record::TraversalMode::ORDERED_FILTERED);
    while (size != 0) {
        if (level >= access.options.maximum_passes)
            throw std::logic_error("algorithm-pass-limit-or-order");
        if (scouts > remaining_edges / access.options.bfs_alpha) {
            access.drainRecords();
            ++access.result.bfs_direction_switches;
            front_bits.fill(0);
            for (uint64_t index = 0; index < size; ++index)
                setFrontierBit(front_bits, frontier.get(index));
            uint64_t previous_size;
            do {
                if (level >= access.options.maximum_passes)
                    throw std::logic_error("algorithm-pass-limit-or-order");
                previous_size = size;
                size = 0;
                next_bits.fill(0);
                ++access.result.bfs_bu_levels;
                for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
                    ++access.result.bfs_bu_vertices;
                    if (depth.get(vertex) != UINT32_MAX)
                        continue;
                    const auto row = incoming.row(access, vertex);
                    for (uint64_t index = row.first; index < row.second; ++index) {
                        const uint32_t neighbor = incoming.id(access, index);
                        const uint64_t bits = front_bits.get(neighbor / 64);
                        ++access.result.bfs_bu_edges;
                        if ((bits >> (neighbor % 64)) & 1) {
                            depth.set(vertex, level + 1);
                            setFrontierBit(next_bits, static_cast<uint32_t>(vertex));
                            ++size;
                            ++access.result.reached;
                            break;
                        }
                    }
                }
                front_bits.swap(next_bits);
                access.result.bfs_frontier_peak = std::max(access.result.bfs_frontier_peak, size);
                ++level;
            } while (size >= previous_size || size > graph.vertices / access.options.bfs_beta);
            if (size == 0)
                break;
            uint64_t count = 0;
            for (uint64_t word = 0; word < words; ++word) {
                uint64_t bits = front_bits.get(word);
                while (bits) {
                    const uint64_t vertex = word * 64 + __builtin_ctzll(bits);
                    if (vertex >= graph.vertices)
                        throw std::logic_error("frontier-bitmap-padding-is-set");
                    frontier.set(count++, static_cast<uint32_t>(vertex));
                    bits &= bits - 1;
                }
            }
            if (count != size)
                throw std::logic_error("frontier-bitmap-count-mismatch");
            ++access.result.bfs_direction_switches;
            scouts = 1;
        } else {
            remaining_edges -= scouts;
            const auto step = bfsTopDownLevel(graph, access, depth, frontier, next, size, level, true);
            sortPrefix(next, step.size);
            frontier.swap(next);
            size = step.size;
            scouts = step.scouts;
            access.result.bfs_frontier_peak = std::max(access.result.bfs_frontier_peak, size);
            ++level;
        }
    }
    access.result.levels = level;
    access.complete();
    access.capture(depth);
}

template<class Access>
void bfs(const GraphView& graph, Access& access) {
    if (access.options.bfs_direction_optimizing) {
        bfsDirectionOptimizing(graph, access);
        return;
    }
    typename Access::template Buffer<uint32_t> depth(access, graph.vertices, "depth", true);
    typename Access::template Buffer<uint32_t> frontier(access, graph.vertices, "frontier");
    typename Access::template Buffer<uint32_t> next(access, graph.vertices, "next_frontier");
    depth.fill(UINT32_MAX);
    depth.set(access.options.source, 0);
    frontier.set(0, access.options.source);
    uint64_t size = 1;
    uint32_t level = 0;
    access.result.reached = access.result.bfs_frontier_peak = 1;
    access.bind(graph, depth, ecg_record::TraversalMode::ORDERED_FILTERED);
    while (size != 0) {
        const auto step = bfsTopDownLevel(graph, access, depth, frontier, next, size, level, false);
        sortPrefix(next, step.size);
        frontier.swap(next);
        size = step.size;
        access.result.bfs_frontier_peak = std::max(access.result.bfs_frontier_peak, size);
        ++level;
    }
    access.result.levels = level;
    access.complete();
    access.capture(depth);
}

template<class Access>
class BucketHeap {
  public:
    using Distances = typename Access::template Buffer<uint64_t>;
    BucketHeap(Access& access, uint64_t vertices, Distances& distances, uint64_t delta)
        : nodes_(access, vertices, "bucket_heap"), positions_(access, vertices, "heap_positions"),
          distances_(distances), delta_(delta) {
        positions_.fill(UINT32_MAX);
    }
    bool empty() const { return size_ == 0; }
    uint64_t bucket() const { return distances_.get(nodes_.get(0)) / delta_; }
    void update(uint32_t vertex) {
        uint64_t index = positions_.get(vertex);
        if (index == UINT32_MAX) {
            index = size_++;
            nodes_.set(index, vertex);
            positions_.set(vertex, static_cast<uint32_t>(index));
        }
        while (index != 0) {
            const uint64_t parent = (index - 1) / 2;
            if (!lessAt(index, parent))
                break;
            exchange(index, parent);
            index = parent;
        }
    }
    uint32_t pop() {
        if (size_ == 0)
            throw std::logic_error("empty-bucket-heap");
        const uint32_t result = nodes_.get(0);
        positions_.set(result, UINT32_MAX);
        --size_;
        if (size_ != 0) {
            const uint32_t tail = nodes_.get(size_);
            nodes_.set(0, tail);
            positions_.set(tail, 0);
            uint64_t root = 0;
            for (;;) {
                uint64_t child = root * 2 + 1;
                if (child >= size_)
                    break;
                if (child + 1 < size_ && lessAt(child + 1, child))
                    ++child;
                if (!lessAt(child, root))
                    break;
                exchange(root, child);
                root = child;
            }
        }
        return result;
    }

  private:
    bool lessAt(uint64_t left, uint64_t right) const {
        const uint32_t left_vertex = nodes_.get(left);
        const uint32_t right_vertex = nodes_.get(right);
        return less(left_vertex, right_vertex);
    }
    bool less(uint32_t left, uint32_t right) const {
        const uint64_t l = distances_.get(left) / delta_, r = distances_.get(right) / delta_;
        return l < r || (l == r && left < right);
    }
    void exchange(uint64_t left, uint64_t right) {
        const uint32_t l = nodes_.get(left), r = nodes_.get(right);
        nodes_.set(left, r);
        nodes_.set(right, l);
        positions_.set(l, static_cast<uint32_t>(right));
        positions_.set(r, static_cast<uint32_t>(left));
    }
    typename Access::template Buffer<uint32_t> nodes_, positions_;
    Distances& distances_;
    uint64_t delta_, size_ = 0;
};

template<class Access>
void sssp(const GraphView& graph, Access& access) {
    for (uint64_t index = 0; index < graph.records; ++index)
        if (graph.weight(access, index) < 0)
            throw std::invalid_argument("negative-weight");
    typename Access::template Buffer<uint64_t> distance(
        access, graph.vertices, "distances", true, ReferencePattern::NEIGHBOR_AND_VERTEX);
    typename Access::template Buffer<uint32_t> frontier(access, graph.vertices, "frontier");
    typename Access::template Buffer<uint32_t> removed(access, graph.vertices, "heavy_frontier");
    typename Access::template Buffer<uint8_t> marked(access, graph.vertices, "heavy_membership");
    distance.fill(UINT64_MAX);
    marked.fill(0);
    distance.set(access.options.source, 0);
    BucketHeap<Access> heap(access, graph.vertices, distance, access.options.delta);
    heap.update(access.options.source);
    access.bind(graph, distance, ecg_record::TraversalMode::ORDERED_FILTERED);
    const auto relax = [&](uint32_t vertex, bool light) {
        access.visitVertex(vertex);
        const uint64_t source_distance = distance.get(vertex);
        const auto row = graph.row(access, vertex);
        for (uint64_t index = row.first; index < row.second; ++index) {
            const uint64_t weight = static_cast<uint32_t>(graph.weight(access, index));
            if ((weight <= access.options.delta) != light)
                continue;
            const auto item = access.neighbor(index, distance);
            uint64_t candidate = 0;
            if (!ecg_record::checkedAdd(source_distance, weight, candidate) || candidate == UINT64_MAX)
                throw std::overflow_error("distance-overflow");
            ++access.result.relax_attempts;
            if (candidate < item.second) {
                distance.set(item.first, candidate);
                heap.update(item.first);
                ++access.result.relax_successes;
            }
        }
    };
    while (!heap.empty()) {
        const uint64_t bucket = heap.bucket();
        uint64_t removed_size = 0;
        while (!heap.empty() && heap.bucket() == bucket) {
            uint64_t size = 0;
            while (!heap.empty() && heap.bucket() == bucket) {
                const uint32_t vertex = heap.pop();
                frontier.set(size++, vertex);
                if (!marked.get(vertex)) {
                    marked.set(vertex, 1);
                    removed.set(removed_size++, vertex);
                }
            }
            sortPrefix(frontier, size);
            access.beginPass();
            ++access.result.light_passes;
            for (uint64_t index = 0; index < size; ++index)
                relax(frontier.get(index), true);
            access.closePass();
        }
        sortPrefix(removed, removed_size);
        access.beginPass();
        ++access.result.heavy_passes;
        for (uint64_t index = 0; index < removed_size; ++index) {
            const uint32_t vertex = removed.get(index);
            relax(vertex, false);
            marked.set(vertex, 0);
        }
        access.closePass();
    }
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex)
        if (distance.get(vertex) != UINT64_MAX)
            ++access.result.reached;
    access.complete();
    access.capture(distance);
}

template<class Buffer>
uint32_t root(Buffer& components, uint32_t vertex) {
    for (;;) {
        const uint32_t parent = components.get(vertex);
        if (parent == vertex)
            return vertex;
        if (parent > vertex)
            throw std::logic_error("noncanonical-component-parent");
        const uint32_t grandparent = components.get(parent);
        components.set(vertex, grandparent);
        vertex = parent;
    }
}

template<class Buffer>
void link(Buffer& components, uint32_t left, uint32_t right) {
    left = root(components, left);
    right = root(components, right);
    if (left != right)
        components.set(std::max(left, right), std::min(left, right));
}

template<class Access>
void cc(const GraphView& graph, Access& access) {
    typename Access::template Buffer<uint32_t> components(
        access, graph.vertices, "components", true, ReferencePattern::NEIGHBOR_AND_VERTEX);
    typename Access::template Buffer<uint64_t> counts(access, graph.vertices, "component_counts");
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex)
        components.set(vertex, static_cast<uint32_t>(vertex));
    access.bind(graph, components, ecg_record::TraversalMode::ORDERED_FILTERED);
    const auto compress = [&] {
        for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex)
            components.set(vertex, root(components, static_cast<uint32_t>(vertex)));
    };
    for (uint64_t round = 0; round < 2; ++round) {
        access.beginPass();
        for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
            access.visitVertex(vertex);
            const auto row = graph.row(access, vertex);
            if (row.second - row.first > round) {
                const auto item = access.neighbor(row.first + round, components);
                link(components, components.get(vertex), item.second);
            }
        }
        access.closePass();
        compress();
    }
    counts.fill(0);
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const uint32_t component = components.get(vertex);
        counts.set(component, counts.get(component) + 1);
    }
    uint32_t largest = 0;
    uint64_t maximum = 0;
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const uint64_t count = counts.get(vertex);
        if (count > maximum) {
            maximum = count;
            largest = static_cast<uint32_t>(vertex);
        }
    }
    access.beginPass();
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        access.visitVertex(vertex);
        if (components.get(vertex) == largest)
            continue;
        const auto row = graph.row(access, vertex);
        for (uint64_t index = row.first + std::min<uint64_t>(2, row.second - row.first);
             index < row.second; ++index) {
            const auto item = access.neighbor(index, components);
            link(components, components.get(vertex), item.second);
        }
    }
    access.closePass();
    compress();
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex)
        if (components.get(vertex) == vertex)
            ++access.result.components;
    access.complete();
    access.capture(components);
}

template<class Access>
void bc(const GraphView& graph, Access& access) {
    typename Access::template Buffer<uint32_t> depth(access, graph.vertices, "depth", true);
    typename Access::template Buffer<uint64_t> sigma(
        access, graph.vertices, "path_counts", true, ReferencePattern::NEIGHBOR_AND_VERTEX);
    typename Access::template Buffer<float> dependency(
        access, graph.vertices, "dependency", true, ReferencePattern::NEIGHBOR_AND_VERTEX);
    typename Access::template Buffer<float> scores(
        access, graph.vertices, "scores", true, ReferencePattern::VERTEX);
    typename Access::template Buffer<uint32_t> frontier(access, graph.vertices, "frontier");
    typename Access::template Buffer<uint32_t> next(access, graph.vertices, "next_frontier");
    typename Access::template Buffer<uint32_t> order(access, graph.vertices, "level_vertices");
    typename Access::template Buffer<uint64_t> levels(access, graph.vertices + 2, "level_offsets");
    scores.fill(0);
    const uint64_t source_count = access.options.sources.empty() ? 1 : access.options.sources.size();
    typename Access::template Buffer<uint32_t> sources(access, source_count, "source_list");
    for (uint64_t index = 0; index < source_count; ++index) {
        const uint32_t source = access.options.sources.empty() ? access.options.source :
            access.template read<uint32_t>(&access.options.sources[index],
                MemoryKind::AUXILIARY, sources.token(), index);
        sources.set(index, source);
    }
    ecg_record::StreamDigest source_digest;
    source_digest.add(source_count);
    for (uint64_t source_index = 0; source_index < source_count; ++source_index) {
        const uint32_t source = sources.get(source_index);
        source_digest.add(source);
        depth.fill(UINT32_MAX);
        sigma.fill(0);
        dependency.fill(0);
        depth.set(source, 0);
        sigma.set(source, 1);
        frontier.set(0, source);
        levels.set(0, 0);
        uint64_t size = 1, reached = 0, level_count = 0;
        access.bind(graph, depth, ecg_record::TraversalMode::ORDERED_FILTERED);
        while (size != 0) {
            uint64_t next_size = 0;
            access.beginPass();
            for (uint64_t position = 0; position < size; ++position) {
                const uint32_t vertex = frontier.get(position);
                access.visitVertex(vertex);
                order.set(reached++, vertex);
                const uint64_t paths = sigma.get(vertex);
                access.result.sigma_max = std::max(access.result.sigma_max, paths);
                const auto row = graph.row(access, vertex);
                for (uint64_t index = row.first; index < row.second; ++index) {
                    const auto item = access.neighbor(index, depth);
                    uint32_t target_level = item.second;
                    if (target_level == UINT32_MAX) {
                        target_level = static_cast<uint32_t>(level_count + 1);
                        depth.set(item.first, target_level);
                        next.set(next_size++, item.first);
                    }
                    if (target_level == level_count + 1) {
                        const uint64_t old = sigma.get(item.first);
                        uint64_t sum = 0;
                        if (!ecg_record::checkedAdd(old, paths, sum))
                            throw std::overflow_error("path-count-overflow");
                        sigma.set(item.first, sum);
                    }
                }
            }
            access.closePass();
            levels.set(++level_count, reached);
            sortPrefix(next, next_size);
            frontier.swap(next);
            size = next_size;
        }
        access.result.reached += reached;
        access.result.levels += level_count;
        access.bind(graph, dependency, ecg_record::TraversalMode::ORDERED_FILTERED);
        for (uint64_t level = level_count; level-- > 0;) {
            const uint64_t first = levels.get(level), last = levels.get(level + 1);
            access.beginPass();
            for (uint64_t position = first; position < last; ++position) {
                const uint32_t vertex = order.get(position);
                access.visitVertex(vertex);
                const uint64_t paths = sigma.get(vertex);
                float sum = 0;
                const auto row = graph.row(access, vertex);
                for (uint64_t index = row.first; index < row.second; ++index) {
                    // DAG membership needs the ordinary CSR ID before a paired delta load exists.
                    const uint32_t target = graph.id(access, index);
                    if (depth.get(target) != level + 1)
                        continue;
                    const auto item = access.neighbor(index, dependency);
                    const uint64_t target_paths = sigma.get(item.first);
                    if (target_paths == 0)
                        throw std::logic_error("zero-path-count-on-dag");
                    const float ratio = static_cast<float>(paths) / static_cast<float>(target_paths);
                    const float contribution = ratio * (1.0f + item.second);
                    sum += contribution;
                }
                dependency.set(vertex, sum);
                if (vertex != source)
                    scores.set(vertex, scores.get(vertex) + sum);
            }
            access.closePass();
        }
    }
    access.result.source_list_digest = source_digest.value();
    access.complete();
    if (access.options.capture_values)
        access.result.values_u64.assign(sigma.data(), sigma.data() + sigma.size());
    access.capture(scores);
}

template<class Access>
void tc(const GraphView& graph, Access& access) {
    typename Access::template Buffer<uint64_t> degrees(access, graph.vertices, "degrees");
    typename Access::template Buffer<uint64_t> offsets(access, graph.vertices + 1, "oriented_offsets");
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto row = graph.row(access, vertex);
        degrees.set(vertex, row.second - row.first);
    }
    const auto forward = [&](uint32_t u, uint32_t v, uint64_t degree_u) {
        const uint64_t degree_v = degrees.get(v);
        return degree_u < degree_v || (degree_u == degree_v && u < v);
    };
    uint64_t oriented_count = 0;
    offsets.set(0, 0);
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto row = graph.row(access, vertex);
        const uint64_t degree = degrees.get(vertex);
        for (uint64_t index = row.first; index < row.second; ++index)
            if (forward(static_cast<uint32_t>(vertex), graph.id(access, index), degree))
                ++oriented_count;
        offsets.set(vertex + 1, oriented_count);
    }
    typename Access::template Buffer<int32_t> columns(access, oriented_count, "oriented_columns");
    typename Access::template Buffer<uint64_t> target_start(access, graph.vertices, "target_row_start", true);
    for (uint64_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto row = graph.row(access, vertex);
        const uint64_t degree = degrees.get(vertex);
        uint64_t position = offsets.get(vertex);
        target_start.set(vertex, position);
        for (uint64_t index = row.first; index < row.second; ++index) {
            const uint32_t target = graph.id(access, index);
            if (forward(static_cast<uint32_t>(vertex), target, degree))
                columns.set(position++, static_cast<int32_t>(target));
        }
    }
    const GraphView oriented{graph.vertices, oriented_count, true, offsets.data(),
        columns.data(), nullptr, sizeof(int32_t), false};
    access.result.oriented_edges = oriented_count;
    access.bind(oriented, target_start, ecg_record::TraversalMode::DENSE_EXACT);
    uint64_t triangles = 0;
    for (uint64_t pass = 0; pass < access.options.repetitions; ++pass) {
        triangles = 0;
        access.beginPass(pass + 1 < access.options.repetitions);
        for (uint64_t vertex = 0; vertex < oriented.vertices; ++vertex) {
            access.visitVertex(vertex);
            const auto row = oriented.row(access, vertex);
            for (uint64_t index = row.first; index < row.second; ++index) {
                const auto item = access.neighbor(index, target_start);
                uint64_t left = row.first, right = item.second;
                const uint64_t end = offsets.get(uint64_t(item.first) + 1);
                while (left < row.second && right < end) {
                    const uint32_t a = oriented.id(access, left), b = oriented.id(access, right);
                    ++access.result.intersection_comparisons;
                    if (a == b) {
                        if (!ecg_record::checkedAdd(triangles, 1, triangles))
                            throw std::overflow_error("triangle-count-overflow");
                        ++left;
                        ++right;
                    } else if (a < b) {
                        ++left;
                    } else {
                        ++right;
                    }
                }
            }
        }
        access.closePass();
    }
    access.result.triangles = triangles;
    access.complete();
    ecg_record::StreamDigest digest;
    digest.add(triangles);
    access.result.result_digest = digest.value();
}

template<class Backend>
Result run(const GraphView& graph, const Options& options, Backend& backend) {
    name(options.algorithm);
    if (options.repetitions == 0 || options.delta == 0 || options.maximum_passes == 0 ||
        options.bfs_alpha == 0 || options.bfs_beta == 0 ||
        (options.algorithm != Algorithm::BFS && options.bfs_direction_optimizing) ||
        (options.record_bytes != 0 && options.record_bytes != 4 && options.record_bytes != 8) ||
        options.minimum_mantissa_bits > 61 ||
        (options.algorithm != Algorithm::SPMV && options.algorithm != Algorithm::TC &&
            options.repetitions != 1))
        throw std::invalid_argument("invalid-algorithm-options");
    const bool source_algorithm = options.algorithm == Algorithm::BFS ||
        options.algorithm == Algorithm::SSSP || options.algorithm == Algorithm::BC;
    if (source_algorithm && options.source >= graph.vertices)
        throw std::invalid_argument("source-outside-graph");
    if (options.algorithm != Algorithm::BC && !options.sources.empty())
        throw std::invalid_argument("source-list-requires-bc");
    for (uint32_t source : options.sources)
        if (source >= graph.vertices)
            throw std::invalid_argument("source-outside-graph");
    Result result;
    Engine<Backend> access(graph, options, backend, result);
    validateGraph(graph, access, options.algorithm == Algorithm::CC || options.algorithm == Algorithm::TC);
    if (options.bfs_direction_optimizing && graph.directed)
        validateGraph(graph.incoming(), access, false);
    switch (options.algorithm) {
      case Algorithm::SPMV: spmv(graph, access); break;
      case Algorithm::BFS: bfs(graph, access); break;
      case Algorithm::SSSP: sssp(graph, access); break;
      case Algorithm::CC: cc(graph, access); break;
      case Algorithm::BC: bc(graph, access); break;
      case Algorithm::TC: tc(graph, access); break;
    }
    return result;
}

} // namespace ecg_algorithm

#endif
