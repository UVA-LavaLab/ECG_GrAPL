#ifndef GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H
#define GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H

#include "cache_sim.h"
#include "../ecg_algorithms.h"
#include "ecg_window_observer.h"
#include "ecg_window_runtime.h"

namespace cache_sim {

struct AlgorithmTraffic {
    uint64_t total_accesses = 0;
    uint64_t memory_accesses = 0;
    uint64_t prefetch_fills = 0;
    uint64_t llc_writebacks = 0;
    uint64_t llc_hits = 0, llc_misses = 0, llc_property_hits = 0, llc_property_misses = 0;

    uint64_t offchip() const { return memory_accesses + prefetch_fills + llc_writebacks; }

    void accumulate(const AlgorithmTraffic& other) {
        for (auto field : {&AlgorithmTraffic::total_accesses, &AlgorithmTraffic::memory_accesses,
                &AlgorithmTraffic::prefetch_fills, &AlgorithmTraffic::llc_writebacks,
                &AlgorithmTraffic::llc_hits, &AlgorithmTraffic::llc_misses,
                &AlgorithmTraffic::llc_property_hits, &AlgorithmTraffic::llc_property_misses})
            if (!ecg_record::checkedAdd(this->*field, other.*field, this->*field))
                throw std::overflow_error("BFS traffic attribution overflow");
    }

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
        : cache_(cache), options_(options), llc_bytes_(llc_bytes),
          grasp_paper_(grasp_paper || (options.records &&
              options.record_base_policy == ecg_algorithm::RecordBasePolicy::GRASP_PAPER)),
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
        if ((options_.bfs_traffic_phases || options_.grasp_graph_passes) &&
            (options_.algorithm != ecg_algorithm::Algorithm::BFS ||
             (options_.records && (options_.bfs_traffic_phases || options_.record_model != ecg_algorithm::RecordModel::WINDOW)) ||
             options_.bfs_direction_optimizing || options_.window_observer != ecg_algorithm::WindowObserverMode::OFF ||
             (options_.grasp_graph_passes && (!grasp_paper_ || popt_full_capacity_))))
            throw std::invalid_argument("BFS phase controls require TD BFS and a compatible baseline/model");
        if (options_.record_model == ecg_algorithm::RecordModel::WINDOW &&
            (options_.algorithm != ecg_algorithm::Algorithm::BFS || !options_.records || !graph.records ||
             graph.weights || options_.bfs_direction_optimizing || options_.traversal_preprocessing ||
             options_.window_observer != ecg_algorithm::WindowObserverMode::OFF ||
             options_.minimum_mantissa_bits || options_.record_base_policy != ecg_algorithm::RecordBasePolicy::GRASP_PAPER ||
             (options_.mechanism != ecg_record::Mechanism::TRANSPORT &&
              options_.mechanism != ecg_record::Mechanism::REPLACEMENT)))
            throw std::invalid_argument("window model requires unweighted TD BFS, GRASP base and T/R only");
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
        memoryKind(pointer, bytes, write, ecg_algorithm::MemoryKind::CONSTRUCTION);
    }

    void memoryKind(const void* pointer, uint64_t bytes, bool write, ecg_algorithm::MemoryKind kind) {
        if (options_.bfs_traffic_phases && static_cast<std::size_t>(kind) >= bfs_traffic_[0].size())
            throw std::invalid_argument("invalid-BFS-memory-role");
        if (bytes == 0)
            return;
        uint64_t address = reinterpret_cast<uint64_t>(pointer), last = 0;
        if (!ecg_record::checkedAdd(address, bytes - 1, last))
            throw std::overflow_error("algorithm-memory-range-overflow");
        for (;;) {
            uint64_t llc_before = 0, misses_before = 0;
            const AlgorithmTraffic before = options_.bfs_traffic_phases ? traffic() : AlgorithmTraffic{};
            if (window_observer_) {
                window_observer_->beforeAccess(address, write);
                const auto& stats = cache_.getL3Stats();
                llc_before = stats.hits.load() + stats.misses.load();
                misses_before = cache_.getMemoryAccesses();
            }
            if (window_runtime_)
                window_runtime_->memory(address, write);
            else
                cache_.access(address, write);
            if (window_observer_) {
                const auto& stats = cache_.getL3Stats();
                window_observer_->afterAccess(address, write,
                    stats.hits.load() + stats.misses.load() != llc_before,
                    cache_.getMemoryAccesses() != misses_before);
            }
            if (options_.bfs_traffic_phases)
                bfs_traffic_[static_cast<std::size_t>(bfs_phase_)][static_cast<std::size_t>(kind)].accumulate(
                    traffic().since(before));
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
        if (options_.bfs_traffic_phases)
            transitionBfsPhase(BfsPhase::BETWEEN, BfsPhase::PROBE);
        if (options_.grasp_graph_passes && !window_runtime_) {
            if (!grasp_phase_configured_ || grasp_pass_open_)
                throw std::logic_error("invalid-scoped-GRASP-pass-begin");
            cache_.graspGraphPass(true);
            grasp_pass_open_ = true;
            ++grasp_transitions_;
            ++grasp_passes_;
        }
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
        if (window_runtime_)
            window_runtime_->visitVertex(vertex);
        if (window_observer_)
            window_observer_->visitVertex(vertex);
        if (!popt_full_capacity_)
            return;
        if (!popt_live_ || vertex >= popt_graph_.vertices)
            throw std::logic_error("invalid-popt-vertex-progress");
        cache_.setCurrentVertex(static_cast<uint32_t>(vertex));
        ++popt_vertices_;
    }
    void rowContext(uint64_t vertex, uint64_t first, uint64_t last) {
        if (window_runtime_)
            window_runtime_->rowContext(vertex, first, last);
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
        if (window_runtime_)
            window_runtime_->associateStore(index, destination, reinterpret_cast<uint64_t>(base), bytes);
        if (window_observer_)
            window_observer_->associateStore(index, destination, reinterpret_cast<uint64_t>(base), bytes);
    }
    void endGraphPass() {
        if (options_.bfs_traffic_phases)
            transitionBfsPhase(BfsPhase::PROBE, BfsPhase::BETWEEN);
        if (options_.grasp_graph_passes && !window_runtime_) {
            if (!grasp_pass_open_)
                throw std::logic_error("invalid-scoped-GRASP-pass-close");
            cache_.graspGraphPass(false);
            grasp_pass_open_ = false;
            ++grasp_transitions_;
        }
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

    void frontierBuild(bool entering) {
        if (options_.bfs_traffic_phases)
            transitionBfsPhase(entering ? BfsPhase::PROBE : BfsPhase::BUILD,
                               entering ? BfsPhase::BUILD : BfsPhase::PROBE);
    }
    void frontierSort(bool entering) {
        if (options_.bfs_traffic_phases)
            transitionBfsPhase(entering ? BfsPhase::BETWEEN : BfsPhase::SORT,
                               entering ? BfsPhase::SORT : BfsPhase::BETWEEN);
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
    void bindWindow(const ecg_window::RecordStream& stream, uint64_t vertices, uint64_t base) {
        if (active_ || window_runtime_ || window_observer_ || popt_full_capacity_)
            throw std::logic_error("window model cannot rebind or share an active runtime");
        window_runtime_ = std::make_unique<WindowRuntime>(cache_, stream, vertices, base,
            options_.mechanism == ecg_record::Mechanism::REPLACEMENT, options_.window_candidate_rrpv,
            options_.grasp_graph_passes);
        beginKernel();
    }
    void beginPass(bool has_next) {
        if (window_runtime_) window_runtime_->beginPass();
        else cache_.recordBeginPass(has_next);
    }
    void closePass() {
        if (window_runtime_) window_runtime_->closePass();
        else cache_.recordClosePass();
    }
    uint64_t recordLoad(uint64_t index) {
        return window_runtime_ ? window_runtime_->recordLoad(index) : cache_.recordLoad(index);
    }
    uint64_t windowTraceValue() const { return window_runtime_->traceValue(); }
    uint64_t windowTraceSource() const { return window_runtime_->traceSource(); }

    template<class T>
    T property(uint64_t index, uint64_t word, const T* base) {
        if (window_runtime_) {
            if constexpr (std::is_same<T, uint32_t>::value)
                return base[window_runtime_->property(index, word, reinterpret_cast<uint64_t>(base))];
            else
                throw std::logic_error("window property must be U32");
        }
        ecg_record::PropertyDescriptor property;
        if (reinterpret_cast<uint64_t>(base) != configuration_.property_base ||
            ecg_record::unpackProperty(configuration_.property_descriptor, property) !=
                ecg_record::Status::OK || ecg_record::propertyBytes(property.kind) != sizeof(T))
            throw std::logic_error("invalid-algorithm-property-width");
        const uint64_t destination = cache_.recordProperty(index, word);
        return base[destination];
    }

    void drain() {
        if (window_runtime_)
            window_runtime_->drain();
        if (active_)
            cache_.drainRecord();
    }
    void finish(uint64_t actual_records) {
        if (options_.bfs_traffic_phases && bfs_phase_ != BfsPhase::BETWEEN)
            throw std::logic_error("BFS traffic attribution did not close");
        if (options_.grasp_graph_passes && !window_runtime_ && (!grasp_phase_configured_ || grasp_pass_open_ ||
            grasp_transitions_ != 2 * grasp_passes_))
            throw std::logic_error("scoped GRASP did not close its control stream");
        if (window_runtime_)
            window_runtime_->finish(actual_records);
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
    void writeWindowRuntime(std::ostream& output) const {
        if (window_runtime_) window_runtime_->write(output);
        else output << "null";
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

    void writeBfsTraffic(std::ostream& output) const {
        if (!options_.bfs_traffic_phases) {
            output << "null";
            return;
        }
        static constexpr const char* phases[] = {"setup", "edge-probe", "frontier-build", "frontier-sort", "between-passes"};
        static constexpr const char* roles[] = {"csr-index", "csr-edge", "weight", "depth", "frontier-work", "construction"};
        output << "{\"schema\":\"ecg.bfs-traffic-phases.v1\","
               << "\"writeback_attribution\":\"triggering-access-not-victim-owner\","
               << "\"cache_state_preserved\":true,\"phases\":[";
        for (std::size_t phase = 0; phase < bfs_traffic_.size(); ++phase) {
            if (phase) output << ',';
            AlgorithmTraffic total;
            for (const auto& cell : bfs_traffic_[phase]) total.accumulate(cell);
            output << "{\"phase\":\"" << phases[phase] << "\",\"total\":";
            total.write(output);
            output << ",\"roles\":{";
            for (std::size_t role = 0; role < bfs_traffic_[phase].size(); ++role) {
                if (role) output << ',';
                output << '"' << roles[role] << "\":";
                bfs_traffic_[phase][role].write(output);
            }
            output << "}}";
        }
        output << "]}";
    }

    void writeGraspPhaseControl(std::ostream& output) const {
        if (!options_.grasp_graph_passes || window_runtime_) {
            output << "null";
            return;
        }
        output << "{\"schema\":\"ecg.grasp-phase-control.v1\",\"enabled\":true,"
               << "\"setup_policy\":\"GRASP_PAPER\",\"cache_reset\":false,"
               << "\"rrpv_history_maintained\":true,\"cost_unit\":\"functional-steps-not-CPU-cycles\","
               << "\"passes\":" << grasp_passes_ << ",\"transitions\":" << grasp_transitions_
               << ",\"functional_steps\":" << (grasp_transitions_ + (grasp_phase_configured_ ? 1 : 0)) * 16
               << ",\"control_bytes\":" << (grasp_transitions_ + (grasp_phase_configured_ ? 1 : 0)) * 48 << '}';
    }

  private:
    enum class BfsPhase : uint8_t { SETUP, PROBE, BUILD, SORT, BETWEEN };
    void transitionBfsPhase(BfsPhase expected, BfsPhase next) {
        if (bfs_phase_ != expected)
            throw std::logic_error("invalid-BFS-traffic-phase-order");
        bfs_phase_ = next;
    }
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
            if (options_.grasp_graph_passes && !window_runtime_) {
                cache_.configureGraspPhases();
                grasp_phase_configured_ = true;
            }
            setup_traffic_ = traffic();
            kernel_started_ = true;
            if (options_.bfs_traffic_phases)
                transitionBfsPhase(BfsPhase::SETUP, BfsPhase::BETWEEN);
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
    BfsPhase bfs_phase_ = BfsPhase::SETUP;
    std::array<std::array<AlgorithmTraffic, 6>, 5> bfs_traffic_{};
    bool grasp_phase_configured_ = false, grasp_pass_open_ = false;
    uint64_t grasp_transitions_ = 0, grasp_passes_ = 0;
    std::unique_ptr<window_observation::Observer> window_observer_;
    std::unique_ptr<WindowRuntime> window_runtime_;
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
