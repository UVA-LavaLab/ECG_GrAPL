#ifndef GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H
#define GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H

#include "cache_sim.h"
#include "../ecg_algorithms.h"
#include "ecg_window_observer.h"

namespace cache_sim {

struct AlgorithmTraffic {
    uint64_t total_accesses = 0;
    uint64_t memory_accesses = 0;
    uint64_t prefetch_fills = 0;
    uint64_t llc_writebacks = 0;
    uint64_t llc_hits = 0, llc_misses = 0, llc_property_hits = 0, llc_property_misses = 0;

    uint64_t offchip() const { return memory_accesses + prefetch_fills + llc_writebacks; }

    AlgorithmTraffic since(const AlgorithmTraffic& before) const {
        if (total_accesses < before.total_accesses || memory_accesses < before.memory_accesses ||
            prefetch_fills < before.prefetch_fills || llc_writebacks < before.llc_writebacks ||
            llc_hits < before.llc_hits || llc_misses < before.llc_misses ||
            llc_property_hits < before.llc_property_hits || llc_property_misses < before.llc_property_misses)
            throw std::logic_error("algorithm phase counters were reset");
        return {total_accesses - before.total_accesses, memory_accesses - before.memory_accesses,
                prefetch_fills - before.prefetch_fills, llc_writebacks - before.llc_writebacks,
                llc_hits - before.llc_hits, llc_misses - before.llc_misses,
                llc_property_hits - before.llc_property_hits, llc_property_misses - before.llc_property_misses};
    }

    void write(std::ostream& output) const {
        output << "{\"total_accesses\":" << total_accesses
               << ",\"memory_accesses\":" << memory_accesses
               << ",\"prefetch_fills\":" << prefetch_fills
               << ",\"llc_writebacks\":" << llc_writebacks
               << ",\"llc_hits\":" << llc_hits << ",\"llc_misses\":" << llc_misses
               << ",\"llc_property_hits\":" << llc_property_hits
               << ",\"llc_property_misses\":" << llc_property_misses
               << ",\"total_offchip_traffic\":" << offchip() << '}';
    }
};

class AlgorithmBackend {
  public:
    static constexpr bool models_memory = true;
    AlgorithmBackend(CacheHierarchy& cache, const ecg_algorithm::Options& options,
                     uint64_t llc_bytes = 8 * 1024 * 1024, bool grasp_paper = false,
                     bool popt_full_capacity = false)
        : cache_(cache), options_(options), llc_bytes_(llc_bytes), grasp_paper_(grasp_paper),
          popt_full_capacity_(popt_full_capacity) {}

    ~AlgorithmBackend() {
        if (window_observer_)
            cache_.observeLastLevel(nullptr);
    }

    void start(const ecg_algorithm::GraphView& graph) {
        if (graph.vertices == 0 || graph.vertices > INT32_MAX)
            throw std::invalid_argument("invalid-algorithm-cache-domain");
        if (popt_full_capacity_ && (options_.records || options_.bfs_direction_optimizing))
            throw std::invalid_argument("current-popt-requires-csr-scalar-graph-passes");
        if (options_.window_observer != ecg_algorithm::WindowObserverMode::OFF &&
            (options_.algorithm != ecg_algorithm::Algorithm::BFS || options_.records ||
             options_.bfs_direction_optimizing || graph.weights || !grasp_paper_ || popt_full_capacity_))
            throw std::invalid_argument("window observer requires unweighted CSR TD BFS with GRASP_PAPER");
        if (options_.window_observer != ecg_algorithm::WindowObserverMode::OFF &&
            (graph.vertices * 12 > options_.maximum_workspace_bytes ||
             options_.maximum_window_observer_bytes > options_.maximum_workspace_bytes - graph.vertices * 12))
            throw std::length_error("window observer exceeds algorithm workspace reservation");
        // These policies use region bounds, not an uncharged degree/oracle prepass.
        context_.topology.num_vertices = static_cast<uint32_t>(graph.vertices);
        context_.topology.num_edges = graph.records;
        context_.topology.directed = graph.directed;
        cache_.initGraphContext(&context_);
        if (options_.records)
            cache_.prepareRecord(recordBasePolicy());
    }

    void memory(const void* pointer, uint64_t bytes, bool write) {
        if (bytes == 0)
            return;
        uint64_t address = reinterpret_cast<uint64_t>(pointer), last = 0;
        if (!ecg_record::checkedAdd(address, bytes - 1, last))
            throw std::overflow_error("algorithm-memory-range-overflow");
        for (;;) {
            uint64_t llc_before = 0, misses_before = 0;
            if (window_observer_) {
                window_observer_->beforeAccess(address, write);
                const auto& stats = cache_.getL3Stats();
                llc_before = stats.hits.load() + stats.misses.load();
                misses_before = cache_.getMemoryAccesses();
            }
            cache_.access(address, write);
            if (window_observer_) {
                const auto& stats = cache_.getL3Stats();
                window_observer_->afterAccess(address, write,
                    stats.hits.load() + stats.misses.load() != llc_before,
                    cache_.getMemoryAccesses() != misses_before);
            }
            const uint64_t line = address / 64;
            if (line == last / 64)
                break;
            address = (line + 1) * 64;
        }
    }

    void region(const char*, const void* base, uint64_t count, uint8_t bytes, bool property) {
        if (popt_full_capacity_) {
            uint64_t allocation = 0;
            if (!ecg_record::checkedMultiply(count, bytes, allocation) ||
                !ecg_record::checkedAdd(array_bytes_, allocation, array_bytes_))
                throw std::overflow_error("popt-array-accounting-overflow");
        }
        if (!property)
            return;
        if (count == 0 || count > UINT32_MAX || context_.num_regions == MAX_PROPERTY_REGIONS)
            throw std::invalid_argument("invalid-algorithm-property-region");
        context_.registerPropertyArray(base, static_cast<uint32_t>(count), bytes,
            llc_bytes_, grasp_paper_ ? 0.50 : 0.15, true);
    }

    void propertyReferences(const void* base, ecg_algorithm::ReferencePattern pattern) {
        if (!popt_full_capacity_)
            return;
        const auto* region = context_.findRegion(reinterpret_cast<uint64_t>(base));
        if (!region || popt_ready_)
            throw std::logic_error("invalid-popt-region-declaration");
        patterns_[region->region_id] = pattern;
    }

    void selectProperty(const ecg_algorithm::GraphView& graph, const void* base,
                        const ecg_record::PropertyDescriptor& property) {
        const auto* region = context_.findRegion(reinterpret_cast<uint64_t>(base));
        if (!region || region->elem_size != ecg_record::propertyBytes(property.kind) ||
            property.stride_bytes != region->elem_size)
            throw std::invalid_argument("unregistered-algorithm-property");
        if (popt_full_capacity_)
            preparePopt(graph);
        if (options_.window_observer != ecg_algorithm::WindowObserverMode::OFF) {
            if (window_observer_ || property.kind != ecg_record::PropertyKind::U32 ||
                property.stride_bytes != 4 || property.traversal != ecg_record::TraversalMode::ORDERED_FILTERED)
                throw std::invalid_argument("window observer requires one U32 depth binding");
            window_observer_ = std::make_unique<window_observation::Observer>(
                graph, reinterpret_cast<uint64_t>(base), cache_.getL3Sets(), cache_.getL3Ways(),
                options_.window_observer == ecg_algorithm::WindowObserverMode::WINDOW,
                options_.maximum_window_observer_bytes);
            cache_.observeLastLevel(window_observer_.get());
        }
        if (!options_.records || graph.records == 0)
            beginKernel();
    }

    void beginGraphPass() {
        if (window_observer_)
            window_observer_->beginPass();
        if (!popt_full_capacity_)
            return;
        if (!popt_ready_ || popt_live_)
            throw std::logic_error("invalid-popt-pass-begin");
        context_.rereference.matrix = popt_matrix_.data();
        context_.setCurrentVertices(UINT32_MAX, UINT32_MAX);
        popt_live_ = true;
        ++popt_passes_;
    }
    void visitVertex(uint64_t vertex) {
        if (window_observer_)
            window_observer_->visitVertex(vertex);
        if (!popt_full_capacity_)
            return;
        if (!popt_live_ || vertex >= popt_graph_.vertices)
            throw std::logic_error("invalid-popt-vertex-progress");
        cache_.setCurrentVertex(static_cast<uint32_t>(vertex));
        ++popt_vertices_;
    }
    void governedReference() {
        if (!popt_full_capacity_)
            return;
        if (!popt_live_ || context_.hints_for_thread().current_src == UINT32_MAX)
            throw std::logic_error("popt-property-read-has-no-vertex-progress");
        ++popt_governed_;
    }
    void designatedProperty(uint64_t index, uint32_t destination) {
        if (window_observer_)
            window_observer_->designated(index, destination);
    }
    void associateNeighborWrite(uint64_t index, uint32_t destination, const void* base, uint64_t bytes) {
        if (window_observer_)
            window_observer_->associateStore(index, destination, reinterpret_cast<uint64_t>(base), bytes);
    }
    void endGraphPass() {
        if (window_observer_)
            window_observer_->endPass();
        if (!popt_full_capacity_)
            return;
        if (!popt_live_)
            throw std::logic_error("invalid-popt-pass-close");
        context_.rereference.matrix = nullptr;
        context_.setCurrentVertices(UINT32_MAX, UINT32_MAX);
        popt_live_ = false;
    }

    void bind(const ecg_record::NativeConfiguration& configuration,
              const ecg_record::RecordStream& stream) {
        if (!active_) {
            cache_.configureRecord(
                configuration, stream, options_.mechanism, recordBasePolicy());
            active_ = true;
        } else {
            cache_.rebindRecord(configuration, stream);
        }
        configuration_ = configuration;
        beginKernel();
    }
    void beginPass(bool has_next) { cache_.recordBeginPass(has_next); }
    void closePass() { cache_.recordClosePass(); }
    uint64_t recordLoad(uint64_t index) { return cache_.recordLoad(index); }

    template<class T>
    T property(uint64_t index, uint64_t word, const T* base) {
        ecg_record::PropertyDescriptor property;
        if (reinterpret_cast<uint64_t>(base) != configuration_.property_base ||
            ecg_record::unpackProperty(configuration_.property_descriptor, property) !=
                ecg_record::Status::OK || ecg_record::propertyBytes(property.kind) != sizeof(T))
            throw std::logic_error("invalid-algorithm-property-width");
        const uint64_t destination = cache_.recordProperty(index, word);
        return base[destination];
    }

    void drain() {
        if (active_)
            cache_.drainRecord();
    }
    void finish(uint64_t actual_records) {
        if (window_observer_) {
            window_observer_->finish(actual_records);
            cache_.observeLastLevel(nullptr);
        }
        if (popt_full_capacity_ && (popt_live_ || !popt_ready_ || popt_governed_ != actual_records))
            throw std::logic_error("incomplete-popt-graph-work");
        if (active_)
            cache_.finishRecord(actual_records);
        else
            cache_.initGraphContext(nullptr);
    }

    AlgorithmTraffic setupTraffic() const {
        if (!kernel_started_)
            throw std::logic_error("algorithm kernel boundary was not observed");
        return setup_traffic_;
    }

    AlgorithmTraffic kernelTraffic() const { return traffic().since(setupTraffic()); }

    void writeWindowObserver(std::ostream& output) const {
        if (window_observer_)
            window_observer_->write(output);
        else
            output << "null";
    }

    void writePopt(std::ostream& output) const {
        if (!popt_full_capacity_) {
            output << "null";
            return;
        }
        output << "{\"encoding\":\"full\",\"scope\":\"graph-pass-irregular-regions\","
               << "\"full_data_capacity\":true,\"runtime_matrix_traffic_charged\":false,"
               << "\"matrix_bytes\":" << popt_matrix_.bytes()
               << ",\"matrix_lines\":" << popt_matrix_.lines()
               << ",\"epochs\":256,\"banks\":" << banks_.size()
               << ",\"covered_regions\":" << popt_regions_
               << ",\"construction_read_bytes\":" << popt_reads_
               << ",\"construction_write_bytes\":" << popt_writes_
               << ",\"workspace_peak_bytes\":" << array_bytes_ + popt_matrix_.bytes() +
                    64 + banks_.capacity() * sizeof(PoptBank)
               << ",\"passes\":" << popt_passes_ << ",\"vertices\":" << popt_vertices_
               << ",\"governed_reads\":" << popt_governed_
               << ",\"lookup_calls\":" << context_.popt_lookup_count << '}';
    }

  private:
    struct PoptBank {
        uint32_t bytes, offset, lines;
        ecg_algorithm::ReferencePattern pattern;
    };
    struct BuildAccess {
        AlgorithmBackend& backend;
        template<class T>
        T read(const void* address, ecg_algorithm::MemoryKind, uint64_t, uint64_t, bool = true) {
            backend.matrixMemory(address, sizeof(T), false);
            T value;
            std::memcpy(&value, address, sizeof(T));
            return value;
        }
    };

    void matrixMemory(const void* address, uint64_t bytes, bool write) {
        memory(address, bytes, write);
        uint64_t& total = write ? popt_writes_ : popt_reads_;
        if (!ecg_record::checkedAdd(total, bytes, total))
            throw std::overflow_error("popt-construction-counter-overflow");
    }

    void preparePopt(const ecg_algorithm::GraphView& graph) {
        if (popt_ready_) {
            if (graph.columns != popt_graph_.columns || graph.records != popt_graph_.records ||
                graph.vertices != popt_graph_.vertices)
                throw std::invalid_argument("popt-graph-changed-after-construction");
            return;
        }
        uint64_t lines = 0;
        for (uint32_t index = 0; index < context_.num_regions; ++index) {
            auto& region = context_.regions[index];
            if (patterns_[index] == ecg_algorithm::ReferencePattern::VERTEX)
                continue;
            if (region.base_address % 64 || region.num_elements != graph.vertices ||
                (region.elem_size != 4 && region.elem_size != 8))
                throw std::invalid_argument("unsupported-popt-property-mapping");
            const auto found = std::find_if(banks_.begin(), banks_.end(), [&](const PoptBank& bank) {
                return bank.bytes == region.elem_size && bank.pattern == patterns_[index];
            });
            if (found != banks_.end()) {
                region.popt_line_offset = found->offset;
            } else {
                const uint64_t count = (graph.vertices * region.elem_size + 63) / 64;
                if (lines + count > UINT32_MAX)
                    throw std::length_error("popt-line-domain-overflow");
                region.popt_line_offset = static_cast<uint32_t>(lines);
                banks_.push_back({region.elem_size, static_cast<uint32_t>(lines),
                    static_cast<uint32_t>(count), patterns_[index]});
                lines += count;
            }
            ++popt_regions_;
        }
        const uint64_t scratch = 64 + banks_.capacity() * sizeof(PoptBank);
        if (array_bytes_ > options_.maximum_workspace_bytes ||
            scratch > options_.maximum_workspace_bytes - array_bytes_ ||
            scratch > options_.build_limits.maximum_auxiliary_bytes)
            throw std::length_error("popt-matrix-workspace-limit");
        const auto observe = [&](const void* pointer, uint64_t bytes, bool write) {
            matrixMemory(pointer, bytes, write);
        };
        popt_matrix_.configure(static_cast<uint32_t>(graph.vertices), static_cast<uint32_t>(lines),
            std::min(options_.maximum_workspace_bytes - array_bytes_ - scratch,
                     options_.build_limits.maximum_auxiliary_bytes - scratch), observe);
        BuildAccess access{*this};
        for (uint64_t source = 0; source < graph.vertices; ++source) {
            const auto row = graph.row(access, source);
            for (const auto& bank : banks_)
                if (bank.pattern == ecg_algorithm::ReferencePattern::NEIGHBOR_AND_VERTEX)
                    popt_matrix_.reference(bank.offset + source / (64 / bank.bytes),
                        static_cast<uint32_t>(source), observe);
            for (uint64_t index = row.first; index < row.second; ++index) {
                const uint32_t target = graph.id(access, index);
                for (const auto& bank : banks_)
                    popt_matrix_.reference(bank.offset + target / (64 / bank.bytes),
                        static_cast<uint32_t>(source), observe);
            }
        }
        popt_matrix_.finish(observe);
        context_.compound_popt = true;
        context_.initRereference(popt_matrix_.data(), popt_matrix_.lines(), 256,
            static_cast<uint32_t>(graph.vertices), 64, popt_reref::Encoding::Full);
        context_.rereference.matrix = nullptr;
        popt_graph_ = graph;
        popt_ready_ = true;
    }

    EvictionPolicy recordBasePolicy() const {
        return options_.record_base_policy ==
                ecg_algorithm::RecordBasePolicy::GRASP_PAPER
            ? EvictionPolicy::GRASP : EvictionPolicy::LRU;
    }

    AlgorithmTraffic traffic() const {
        const auto& llc = cache_.getL3Stats();
        return {cache_.getTotalAccesses(), cache_.getMemoryAccesses(),
                cache_.getPrefetchFills(), cache_.getWritebackTraffic(),
                llc.hits.load(), llc.misses.load(), llc.prop_hits.load(), llc.prop_misses.load()};
    }
    void beginKernel() {
        if (!kernel_started_) {
            setup_traffic_ = traffic();
            kernel_started_ = true;
        }
    }

    CacheHierarchy& cache_;
    const ecg_algorithm::Options& options_;
    uint64_t llc_bytes_;
    bool grasp_paper_;
    bool popt_full_capacity_;
    GraphCacheContext context_;
    ecg_record::NativeConfiguration configuration_;
    bool active_ = false;
    AlgorithmTraffic setup_traffic_;
    bool kernel_started_ = false;
    std::unique_ptr<window_observation::Observer> window_observer_;
    popt_reref::FullMatrix popt_matrix_;
    ecg_algorithm::GraphView popt_graph_;
    std::vector<PoptBank> banks_;
    std::array<ecg_algorithm::ReferencePattern, MAX_PROPERTY_REGIONS> patterns_{};
    uint64_t array_bytes_ = 0, popt_reads_ = 0, popt_writes_ = 0;
    uint64_t popt_passes_ = 0, popt_vertices_ = 0, popt_governed_ = 0;
    uint32_t popt_regions_ = 0;
    bool popt_ready_ = false, popt_live_ = false;
};

} // namespace cache_sim

#endif
