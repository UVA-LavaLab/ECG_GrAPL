#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "ecg_algorithms.h"
#include "cache_sim/ecg_algorithm.h"

namespace {

struct Fixture {
    struct Edge { int32_t id, weight; };
    std::vector<uint64_t> offsets;
    std::vector<Edge> edges;
    bool directed;

    Fixture(uint32_t vertices, bool is_directed,
            std::vector<std::tuple<uint32_t, uint32_t, int32_t>> input)
        : offsets(vertices + 1), directed(is_directed) {
        if (!directed) {
            const auto original = input;
            for (const auto& edge : original)
                input.emplace_back(std::get<1>(edge), std::get<0>(edge), std::get<2>(edge));
        }
        std::sort(input.begin(), input.end());
        for (const auto& edge : input)
            ++offsets.at(std::get<0>(edge) + 1);
        for (uint32_t vertex = 0; vertex < vertices; ++vertex)
            offsets[vertex + 1] += offsets[vertex];
        for (const auto& edge : input)
            edges.push_back({static_cast<int32_t>(std::get<1>(edge)), std::get<2>(edge)});
    }

    ecg_algorithm::GraphView view(bool weighted = false) const {
        return {offsets.size() - 1, edges.size(), directed, offsets.data(),
            edges.data(), weighted && !edges.empty() ? &edges[0].weight : nullptr,
            sizeof(Edge), false};
    }
};

int failures = 0;
void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "[FAIL] " << name << '\n';
        ++failures;
    }
}

ecg_algorithm::Result run(
        const ecg_algorithm::GraphView& graph, ecg_algorithm::Algorithm algorithm,
        bool records, uint8_t bytes, uint32_t source = 0) {
    ecg_algorithm::Options options;
    options.algorithm = algorithm;
    options.records = records;
    options.record_bytes = bytes;
    options.source = source;
    options.delta = 2;
    options.capture_values = options.evidence = true;
    ecg_algorithm::PlainBackend backend;
    return ecg_algorithm::run(graph, options, backend);
}

void expectError(const Fixture& graph, ecg_algorithm::Algorithm algorithm,
                 const std::string& message, bool weighted = false) {
    for (bool records : {false, true}) {
        bool rejected = false;
        try {
            run(graph.view(weighted), algorithm, records, 4);
        } catch (const std::invalid_argument& error) {
            rejected = std::string(error.what()).find(message) != std::string::npos;
        } catch (const std::overflow_error& error) {
            rejected = std::string(error.what()).find(message) != std::string::npos;
        }
        check(rejected, message.c_str());
    }
}

Fixture layered(uint32_t layers) {
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges;
    edges.emplace_back(0, 1, 1);
    edges.emplace_back(0, 2, 1);
    for (uint32_t level = 1; level < layers; ++level)
        for (uint32_t previous = 2 * level - 1; previous <= 2 * level; ++previous)
            for (uint32_t next = 2 * level + 1; next <= 2 * level + 2; ++next)
                edges.emplace_back(previous, next, 1);
    edges.emplace_back(2 * layers - 1, 2 * layers + 1, 1);
    edges.emplace_back(2 * layers, 2 * layers + 1, 1);
    return Fixture(2 * layers + 2, true, edges);
}

struct RejectRegionBackend : ecg_algorithm::PlainBackend {
    void region(const char*, const void*, uint64_t, uint8_t, bool) {
        throw std::invalid_argument("rejected-region");
    }
};

struct CorruptRecordBackend : ecg_algorithm::PlainBackend {
    uint64_t recordLoad(uint64_t) { return UINT32_MAX; }
};

struct HeapOrderBackend : ecg_algorithm::PlainBackend {
    uint64_t base = 0, bytes = 0;
    std::vector<uint64_t> reads;
    void region(const char* name, const void* pointer, uint64_t count, uint8_t width, bool) {
        if (std::string(name) == "bucket_heap") {
            base = reinterpret_cast<uint64_t>(pointer);
            bytes = count * width;
        }
    }
    void memory(const void* pointer, uint64_t, bool write) {
        const uint64_t address = reinterpret_cast<uint64_t>(pointer);
        if (!write && address >= base && address - base < bytes)
            reads.push_back((address - base) / sizeof(uint32_t));
    }
};

struct BoundGraphBackend : ecg_algorithm::PlainBackend {
    using ecg_algorithm::PlainBackend::selectProperty;
    ecg_algorithm::GraphView active;
    void start(const ecg_algorithm::GraphView& graph) { active = graph; }
    void selectProperty(const ecg_algorithm::GraphView& graph, const void*,
                        const ecg_record::PropertyDescriptor&) { active = graph; }
};

} // namespace

int main() {
    using namespace ecg_algorithm;
    const Fixture diamond(8, true, {
        {0,1,2}, {0,2,5}, {0,5,20}, {1,2,1}, {1,3,2},
        {2,3,1}, {2,4,4}, {3,4,1}, {4,5,3}, {6,7,1}});
    const Fixture cliques(10, false, {
        {0,1,1}, {0,2,1}, {0,3,1}, {1,2,1}, {1,3,1}, {2,3,1},
        {4,5,1}, {4,6,1}, {5,6,1}, {7,8,1}});
    const Fixture paths = layered(34);
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        for (bool records : {false, true}) {
            const auto spmv = run(diamond.view(true), Algorithm::SPMV, records, width);
            check(spmv.values_f32 == std::vector<float>({139,11,24,5,18,0,8,0}),
                  "weighted CSR SpMV has independently computed values");
            const auto bfs = run(diamond.view(), Algorithm::BFS, records, width);
            check(bfs.values_u32 == std::vector<uint32_t>({0,1,1,2,2,1,UINT32_MAX,UINT32_MAX}),
                  "BFS levels include the high sink and disconnected vertices");
            const auto sssp = run(diamond.view(true), Algorithm::SSSP, records, width);
            check(sssp.values_u64 == std::vector<uint64_t>({0,2,3,4,5,8,UINT64_MAX,UINT64_MAX}),
                  "delta stepping re-relaxes a weighted diamond");
            const auto bc = run(diamond.view(), Algorithm::BC, records, width);
            check(bc.values_f32 == std::vector<float>({0,0.5f,1.5f,0,0,0,0,0}),
                  "single-source Brandes agrees with independent rational path enumeration");
            const auto cc = run(cliques.view(), Algorithm::CC, records, width);
            check(cc.values_u32 == std::vector<uint32_t>({0,0,0,0,4,4,4,7,7,9}) &&
                  cc.components == 4, "Afforest returns canonical labels including an isolate");
            const auto tc = run(cliques.view(), Algorithm::TC, records, width);
            check(tc.triangles == 5 && tc.oriented_edges == 10,
                  "degree-oriented intersections count K4 and K3 exactly once");
            for (Algorithm algorithm : {Algorithm::BFS, Algorithm::SSSP, Algorithm::BC}) {
                const auto isolated = run(diamond.view(algorithm == Algorithm::SSSP),
                    algorithm, records, width, 7);
                check(isolated.actual_records == 0 && isolated.reached == 1,
                      "isolated source reports real zero governed work without a fake traversal");
            }
            const auto large_sigma = run(paths.view(), Algorithm::BC, records, width);
            check(large_sigma.sigma_max == (uint64_t{1} << 34) &&
                  large_sigma.values_u64.back() == (uint64_t{1} << 34),
                  "Brandes path counts exceed both signed and unsigned 32-bit ranges");
            float total = 0;
            for (float score : large_sigma.values_f32)
                total += score;
            check(total == 1156, "large path multiplicity preserves dependency scores");
            for (Algorithm algorithm : {Algorithm::SPMV, Algorithm::BFS, Algorithm::SSSP,
                                        Algorithm::BC, Algorithm::CC, Algorithm::TC}) {
                const auto graph = algorithm == Algorithm::CC || algorithm == Algorithm::TC
                    ? cliques.view() : diamond.view(algorithm == Algorithm::SPMV || algorithm == Algorithm::SSSP);
                const auto csr = run(graph, algorithm, false, width);
                const auto ecg = run(graph, algorithm, true, width);
                check(csr.result_digest == ecg.result_digest &&
                      csr.work_digest == ecg.work_digest && csr.position_digest == ecg.position_digest &&
                      ecg.actual_records + ecg.skipped_positions == ecg.structural_positions,
                      "CSR and current records execute the same algorithm and structural work");
            }
        }
    }
    const Fixture zero_weights(4, true, {{0,1,0}, {1,2,0}, {0,2,9}, {2,3,1}});
    check(run(zero_weights.view(true), Algorithm::SSSP, true, 4).values_u64 ==
          std::vector<uint64_t>({0,0,0,1}), "zero-weight light closure converges");
    const Fixture long_distance(3, true, {{0,1,INT32_MAX}, {1,2,INT32_MAX}});
    check(run(long_distance.view(true), Algorithm::SSSP, true, 8).values_u64.back() ==
          uint64_t{2} * INT32_MAX, "SSSP does not truncate distances at an int32 sentinel");
    const Fixture negative(3, true, {{0,1,-1}, {1,2,2}});
    expectError(negative, Algorithm::SSSP, "negative-weight", true);
    expectError(layered(65), Algorithm::BC, "path-count-overflow");
    const Fixture duplicate(3, false, {{0,1,1}, {0,1,1}});
    const Fixture loop(3, false, {{0,0,1}});
    expectError(duplicate, Algorithm::CC, "simple-undirected");
    expectError(loop, Algorithm::TC, "simple-undirected");
    expectError(diamond, Algorithm::TC, "simple-undirected");
    {
        Options options;
        options.algorithm = Algorithm::TC;
        BoundGraphBackend backend;
        const auto triangles = ecg_algorithm::run(cliques.view(), options, backend);
        check(backend.active.records == triangles.oriented_edges && backend.active.directed,
              "backend attribution follows the actual oriented TC stream");
    }
    {
        Options options;
        Result result;
        HeapOrderBackend backend;
        Engine<HeapOrderBackend> access(diamond.view(), options, backend, result);
        Engine<HeapOrderBackend>::Buffer<uint64_t> distances(access, 8, "distances", true);
        distances.fill(UINT64_MAX);
        distances.set(0, 5);
        distances.set(1, 2);
        BucketHeap<Engine<HeapOrderBackend>> heap(access, 8, distances, 1);
        heap.update(0);
        backend.reads.clear();
        heap.update(1);
        check(backend.reads == std::vector<uint64_t>({1, 0, 1, 0}),
              "heap child/parent reads have an explicit cross-compiler order");
    }
    {
        Options options;
        options.maximum_workspace_bytes = 128;
        Result result;
        RejectRegionBackend backend;
        Engine<RejectRegionBackend> access(diamond.view(), options, backend, result);
        try {
            Engine<RejectRegionBackend>::Buffer<uint32_t> rejected(access, 4, "rejected");
            check(false, "a rejected region must propagate its error");
        } catch (const std::invalid_argument&) {}
        bool released = true;
        try {
            access.reserve(128);
            access.release(128);
        } catch (const std::length_error&) {
            released = false;
        }
        check(released, "a rejected region releases its allocation and workspace reservation");
    }
    {
        Options options;
        options.algorithm = Algorithm::BC;
        options.sources = {0, 0};
        options.records = options.capture_values = true;
        PlainBackend backend;
        const auto repeated = ecg_algorithm::run(diamond.view(), options, backend);
        check(repeated.values_f32 == std::vector<float>({0,1,3,0,0,0,0,0}) &&
              repeated.bindings == 4 && repeated.reached == 12 &&
              repeated.carrier_bytes == repeated.carrier_records * repeated.layout.record_bytes,
              "BC charges explicit source traversal and reuses one immutable compatible carrier");
        check(!repeated.memory_counts_measured && repeated.ordinary_property_reads == 0 &&
              repeated.auxiliary_accesses == 0,
              "nonfunctional timing adapters do not execute diagnostic counters in the hot loop");
    }
    {
        Options options;
        options.algorithm = Algorithm::BFS;
        options.records = true;
        CorruptRecordBackend backend;
        bool rejected = false;
        try {
            ecg_algorithm::run(diamond.view(), options, backend);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected, "the owning record/property adapter rejects malformed actual metadata");
    }
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        for (Algorithm algorithm : {Algorithm::SPMV, Algorithm::BFS, Algorithm::SSSP,
                                    Algorithm::BC, Algorithm::CC, Algorithm::TC}) {
            const auto graph = algorithm == Algorithm::CC || algorithm == Algorithm::TC
                ? cliques.view() : diamond.view(algorithm == Algorithm::SPMV || algorithm == Algorithm::SSSP);
            const auto expected = run(graph, algorithm, false, width);
            const auto expected_record = run(graph, algorithm, true, width);
            for (const auto mechanism : {ecg_record::Mechanism::TRANSPORT, ecg_record::Mechanism::REPLACEMENT,
                    ecg_record::Mechanism::PREFETCH, ecg_record::Mechanism::REPLACEMENT_PREFETCH}) {
                Options options;
                options.algorithm = algorithm;
                options.records = options.evidence = true;
                options.record_bytes = width;
                options.delta = 2;
                options.mechanism = mechanism;
                const bool replacement = mechanism == ecg_record::Mechanism::REPLACEMENT ||
                    mechanism == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
                cache_sim::CacheHierarchy cache(128, 2, 256, 2, 512, 2, 64,
                    cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU,
                    replacement ? cache_sim::EvictionPolicy::ECG : cache_sim::EvictionPolicy::LRU);
                cache_sim::AlgorithmBackend backend(cache, options);
                const auto actual = ecg_algorithm::run(graph, options, backend);
                const auto setup = backend.setupTraffic();
                const auto kernel = backend.kernelTraffic();
                check(setup.total_accesses > 0 && kernel.total_accesses > 0 &&
                      setup.total_accesses + kernel.total_accesses == cache.getTotalAccesses() &&
                      setup.offchip() + kernel.offchip() == cache.getTotalOffChipTraffic(),
                      "setup and kernel snapshots close without resetting cache state");
                check(actual.result_digest == expected.result_digest &&
                      actual.work_digest == expected.work_digest &&
                      actual.position_digest == expected.position_digest &&
                      actual.record_digest == expected_record.record_digest &&
                      actual.construction_reads != 0 && actual.construction_writes != 0,
                      "every algorithm uses real cache transport and charges record construction");
            }
        }
    }
    std::cout << "[SUMMARY] failures=" << failures << '\n';
    return failures != 0;
}
