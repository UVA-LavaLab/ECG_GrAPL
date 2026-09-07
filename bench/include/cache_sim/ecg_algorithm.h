#ifndef GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H
#define GRAPHBREW_CACHE_SIM_ECG_ALGORITHM_H

#include "cache_sim.h"
#include "../ecg_algorithms.h"

namespace cache_sim {

class AlgorithmBackend {
  public:
    AlgorithmBackend(CacheHierarchy& cache, const ecg_algorithm::Options& options,
                     uint64_t llc_bytes = 8 * 1024 * 1024, bool grasp_paper = false)
        : cache_(cache), options_(options), llc_bytes_(llc_bytes), grasp_paper_(grasp_paper) {}

    void start(const ecg_algorithm::GraphView& graph) {
        if (graph.vertices == 0 || graph.vertices > INT32_MAX)
            throw std::invalid_argument("invalid-algorithm-cache-domain");
        // These policies use region bounds, not an uncharged degree/oracle prepass.
        context_.topology.num_vertices = static_cast<uint32_t>(graph.vertices);
        context_.topology.num_edges = graph.records;
        context_.topology.directed = graph.directed;
        cache_.initGraphContext(&context_);
    }

    void memory(const void* pointer, uint64_t bytes, bool write) {
        if (bytes == 0)
            return;
        uint64_t address = reinterpret_cast<uint64_t>(pointer), last = 0;
        if (!ecg_record::checkedAdd(address, bytes - 1, last))
            throw std::overflow_error("algorithm-memory-range-overflow");
        for (;;) {
            cache_.access(address, write);
            const uint64_t line = address / 64;
            if (line == last / 64)
                break;
            address = (line + 1) * 64;
        }
    }

    void region(const char*, const void* base, uint64_t count, uint8_t bytes, bool property) {
        if (!property)
            return;
        if (count == 0 || count > UINT32_MAX || context_.num_regions == MAX_PROPERTY_REGIONS)
            throw std::invalid_argument("invalid-algorithm-property-region");
        context_.registerPropertyArray(base, static_cast<uint32_t>(count), bytes,
            llc_bytes_, grasp_paper_ ? 0.50 : 0.15, true);
    }

    void selectProperty(const void* base, const ecg_record::PropertyDescriptor& property) {
        const auto* region = context_.findRegion(reinterpret_cast<uint64_t>(base));
        if (!region || region->elem_size != ecg_record::propertyBytes(property.kind) ||
            property.stride_bytes != region->elem_size)
            throw std::invalid_argument("unregistered-algorithm-property");
    }

    void bind(const ecg_record::NativeConfiguration& configuration,
              const ecg_record::RecordStream& stream) {
        if (!active_) {
            cache_.configureRecord(configuration, stream, options_.mechanism);
            active_ = true;
        } else {
            cache_.rebindRecord(configuration, stream);
        }
        configuration_ = configuration;
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
        if (active_)
            cache_.finishRecord(actual_records);
        else
            cache_.initGraphContext(nullptr);
    }

  private:
    CacheHierarchy& cache_;
    const ecg_algorithm::Options& options_;
    uint64_t llc_bytes_;
    bool grasp_paper_;
    GraphCacheContext context_;
    ecg_record::NativeConfiguration configuration_;
    bool active_ = false;
};

} // namespace cache_sim

#endif
