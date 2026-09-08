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
    std::vector<uint64_t> in_offsets;
    std::vector<Edge> in_edges;
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
        if (directed) {
            std::vector<std::tuple<uint32_t, uint32_t, int32_t>> incoming;
            for (const auto& edge : input)
                incoming.emplace_back(std::get<1>(edge), std::get<0>(edge), std::get<2>(edge));
            std::sort(incoming.begin(), incoming.end());
            in_offsets.resize(vertices + 1);
            for (const auto& edge : incoming)
                ++in_offsets[std::get<0>(edge) + 1];
            for (uint32_t vertex = 0; vertex < vertices; ++vertex)
                in_offsets[vertex + 1] += in_offsets[vertex];
            for (const auto& edge : incoming)
                in_edges.push_back({static_cast<int32_t>(std::get<1>(edge)), std::get<2>(edge)});
        }
    }

    ecg_algorithm::GraphView view(bool weighted = false) const {
        return {offsets.size() - 1, edges.size(), directed, offsets.data(),
            edges.data(), weighted && !edges.empty() ? &edges[0].weight : nullptr,
            sizeof(Edge), false, directed ? in_offsets.data() : nullptr,
            directed ? in_edges.data() : nullptr};
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

Fixture directionSwitchGraph() {
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges{{0, 1, 1}};
    for (uint32_t vertex = 2; vertex <= 65; ++vertex) {
        edges.emplace_back(1, vertex, 1);
        edges.emplace_back(vertex, 66, 1);
    }
    for (uint32_t vertex = 66; vertex < 128; ++vertex)
        edges.emplace_back(vertex, vertex + 1, 1);
    return Fixture(130, true, edges);
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

struct InspectRecordsBackend : ecg_algorithm::PlainBackend {
    std::vector<ecg_record::DecodedRecord> records;
    ecg_record::BuildStats stats;
    void bind(const ecg_record::NativeConfiguration& configuration,
              const ecg_record::RecordStream& stream) {
        ecg_algorithm::PlainBackend::bind(configuration, stream);
        records.resize(stream.size());
        for (std::size_t index = 0; index < stream.size(); ++index)
            check(ecg_record::decodeRecord(stream.layout, stream.word(index), records[index]) ==
                  ecg_record::Status::OK, "preprocessing emits valid records");
        stats = stream.stats;
    }
};

void testUnboundRecordPreparation() {
    using namespace ecg_algorithm;
    const Fixture graph(32, true, {{0, 1, 1}});
    uint64_t expected = 0;
    for (const auto mechanism : {ecg_record::Mechanism::TRANSPORT,
            ecg_record::Mechanism::REPLACEMENT, ecg_record::Mechanism::PREFETCH,
            ecg_record::Mechanism::REPLACEMENT_PREFETCH}) {
        Options options;
        options.records = true;
        options.mechanism = mechanism;
        const bool replacement = mechanism == ecg_record::Mechanism::REPLACEMENT ||
            mechanism == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
        cache_sim::CacheHierarchy cache(64, 1, 64, 1, 128, 2, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU,
            replacement ? cache_sim::EvictionPolicy::ECG : cache_sim::EvictionPolicy::LRU);
        cache_sim::AlgorithmBackend backend(cache, options, 128);
        backend.start(graph.view());
        backend.region("property", reinterpret_cast<const void*>(0x1000), 32, 4, true);
        for (unsigned iteration = 0; iteration < 16; ++iteration)
            for (uint64_t address : {0x1000, 0x2000, 0x2040, 0x1000})
                backend.memory(reinterpret_cast<const void*>(address), 4, false);
        if (mechanism == ecg_record::Mechanism::TRANSPORT)
            expected = cache.getMemoryAccesses();
        if (cache.getMemoryAccesses() != expected)
            std::cerr << "unbound transport misses=" << expected
                      << " mechanism misses=" << cache.getMemoryAccesses() << '\n';
        check(cache.getMemoryAccesses() == expected,
              "current-record preparation is LRU-neutral before any binding exists");
        backend.finish(0);
    }
}

void testTraversalPreprocessing() {
    using namespace ecg_algorithm;
    using ecg_record::State;
    const Fixture weighted(32, true, {
        {0,8,1}, {0,9,3}, {0,10,1}, {1,8,3}, {1,9,1}, {1,10,3}});
    const Fixture sampled(32, false, {
        {0,8,1}, {0,9,1}, {0,10,1}, {1,8,1}, {1,9,1}, {1,10,1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        Options options;
        options.records = options.evidence = true;
        options.record_bytes = width;
        options.delta = 2;
        options.algorithm = Algorithm::SSSP;
        InspectRecordsBackend original;
        const auto baseline = ecg_algorithm::run(weighted.view(true), options, original);
        check(original.records[0].distance == 1 && original.records[0].state == State::FINITE,
              "full CSR predicts the intervening opposite-weight-class reference");
        options.traversal_preprocessing = true;
        InspectRecordsBackend split;
        const auto actual = ecg_algorithm::run(weighted.view(true), options, split);
        check(split.records[0].distance == 2 && split.records[1].distance == 2 &&
              split.records[2].distance == 2 && split.records[4].state == State::UNKNOWN &&
              split.records[5].state == State::UNKNOWN,
              "SSSP predicts the next same-line reference within its static weight class");
        check(actual.result_digest == baseline.result_digest &&
              actual.work_digest == baseline.work_digest &&
              actual.position_digest == baseline.position_digest &&
              actual.carrier_bytes == baseline.carrier_bytes &&
              actual.construction_auxiliary_peak_bytes > baseline.construction_auxiliary_peak_bytes,
              "phase partitioning preserves work/carrier width and charges its larger scratch");
        check(split.stats.finite_records == 4 && split.stats.wrap_records == 0 &&
              split.stats.unknown_records == 2 && split.stats.property_lines == 1,
              "phase slots do not falsely count one physical property line twice");
        options.algorithm = Algorithm::CC;
        InspectRecordsBackend rounds;
        ecg_algorithm::run(sampled.view(), options, rounds);
        check(rounds.records[0].distance == 3 && rounds.records[1].distance == 3 &&
              rounds.records[2].distance == 3,
              "CC sample round zero, round one and remaining edges have separate next uses");
        for (Algorithm algorithm : {Algorithm::BFS, Algorithm::BC}) {
            options.algorithm = algorithm;
            InspectRecordsBackend local;
            ecg_algorithm::run(weighted.view(), options, local);
            check(local.records[0].state == State::FINITE && local.records[0].distance == 1 &&
                  local.records[2].state == State::UNKNOWN &&
                  local.records[5].state == State::UNKNOWN &&
                  local.stats.finite_records == 4 && local.stats.unknown_records == 2 &&
                  local.stats.wrap_records == 0,
                  "row-local preprocessing never claims another frontier row will execute");
        }
    }
    ecg_record::Requirements requirements;
    requirements.vertex_count = 32;
    requirements.record_count = 6;
    requirements.traversal_count = 1;
    ecg_record::Layout layout;
    check(ecg_record::selectLayout(requirements, layout) == ecg_record::Status::OK,
          "partitioned fixture has a valid layout");
    const ecg_record::PropertyDescriptor property{
        ecg_record::PropertyKind::U64, 8, ecg_record::TraversalMode::ORDERED_FILTERED};
    const auto id = [](uint64_t index) { return 8 + index % 3; };
    ecg_record::RecordStream stream;
    ecg_record::BuildLimits limits;
    limits.maximum_auxiliary_bytes = 63;
    check(ecg_record::buildRecords<2>(requirements, layout, property, 0x1000, id,
              stream, limits) == ecg_record::Status::RESOURCE_LIMIT && stream.size() == 0,
          "phase-slot allocation is rejected before exceeding its construction budget");
    limits.maximum_auxiliary_bytes = 4096;
    check(ecg_record::buildRecords<2>(requirements, layout, property, 0x1000, id,
              stream, limits, ecg_record::UnobservedConstruction{},
              [](uint64_t) { return ecg_record::BuildScope{2, UINT64_MAX}; }) ==
              ecg_record::Status::INVALID_COUNTS && stream.size() == 0,
          "an invalid static partition cannot publish a partial carrier");
    check(ecg_record::buildRecords(requirements, layout, property, 0x1000, id,
              stream, limits, ecg_record::UnobservedConstruction{},
              [](uint64_t index) { return ecg_record::BuildScope{0, index}; }) ==
              ecg_record::Status::INVALID_COUNTS && stream.size() == 0,
          "an invalid row horizon cannot publish a partial carrier");
}

} // namespace

int main() {
    using namespace ecg_algorithm;
    testUnboundRecordPreparation();
    testTraversalPreprocessing();
    const Fixture diamond(8, true, {
        {0,1,2}, {0,2,5}, {0,5,20}, {1,2,1}, {1,3,2},
        {2,3,1}, {2,4,4}, {3,4,1}, {4,5,3}, {6,7,1}});
    const Fixture cliques(10, false, {
        {0,1,1}, {0,2,1}, {0,3,1}, {1,2,1}, {1,3,1}, {2,3,1},
        {4,5,1}, {4,6,1}, {5,6,1}, {7,8,1}});
    const Fixture paths = layered(34);
    const Fixture switching = directionSwitchGraph();
    for (const Fixture* fixture : {&diamond, &cliques, &switching}) {
        const auto expected = run(fixture->view(), Algorithm::BFS, false, 4);
        for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
            for (const auto mechanism : {ecg_record::Mechanism::TRANSPORT,
                    ecg_record::Mechanism::REPLACEMENT, ecg_record::Mechanism::PREFETCH,
                    ecg_record::Mechanism::REPLACEMENT_PREFETCH}) {
                Options options;
                options.algorithm = Algorithm::BFS;
                options.bfs_direction_optimizing = true;
                options.record_bytes = width;
                options.evidence = options.capture_values = true;
                PlainBackend baseline;
                const auto csr = ecg_algorithm::run(fixture->view(), options, baseline);
                options.records = true;
                options.mechanism = mechanism;
                const bool replacement = mechanism == ecg_record::Mechanism::REPLACEMENT ||
                    mechanism == ecg_record::Mechanism::REPLACEMENT_PREFETCH;
                cache_sim::CacheHierarchy cache(128, 2, 256, 2, 512, 2, 64,
                    cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU,
                    replacement ? cache_sim::EvictionPolicy::ECG : cache_sim::EvictionPolicy::LRU);
                cache_sim::AlgorithmBackend backend(cache, options);
                const auto actual = ecg_algorithm::run(fixture->view(), options, backend);
                check(actual.values_u32 == expected.values_u32 && actual.levels == expected.levels &&
                      actual.work_digest == csr.work_digest && actual.result_digest == csr.result_digest,
                      "direction-optimized BFS preserves depths and identical CSR/record work");
                check(actual.bfs_bu_levels > 0 &&
                      actual.bfs_td_levels + actual.bfs_bu_levels == actual.levels &&
                      actual.bfs_td_edges == actual.actual_records &&
                      actual.passes == actual.bfs_td_levels,
                      "BU bitmap probes are ordinary accesses, not fake governed records");
                if (fixture == &switching)
                    check(actual.bfs_td_levels > 1 && actual.bfs_direction_switches >= 2 &&
                          actual.bfs_bu_edges == 253 &&
                          actual.values_u32[128] == 65 && actual.values_u32[129] == UINT32_MAX,
                          "TD/BU/TD preserves bitmap boundaries, isolates and BU early exit");
            }
        }
    }
    {
        Options options;
        options.algorithm = Algorithm::BFS;
        options.bfs_direction_optimizing = true;
        options.bfs_alpha = 1;
        options.capture_values = true;
        PlainBackend backend;
        const auto result = ecg_algorithm::run(switching.view(), options, backend);
        check(result.bfs_bu_levels == 0 && result.bfs_td_levels == result.levels &&
              result.values_u32 == run(switching.view(), Algorithm::BFS, false, 4).values_u32,
              "direction thresholds can select pure TD without changing BFS answers");
        options.source = 7;
        options.records = true;
        PlainBackend isolated_backend;
        const auto isolated = ecg_algorithm::run(diamond.view(), options, isolated_backend);
        check(isolated.actual_records == 0 && isolated.reached == 1 && isolated.levels == 1,
              "direction-optimized isolated source does not invent governed accesses");
        auto missing_inverse = diamond.view();
        missing_inverse.in_offsets = missing_inverse.in_columns = nullptr;
        bool rejected = false;
        try {
            PlainBackend invalid_backend;
            ecg_algorithm::run(missing_inverse, options, invalid_backend);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "directed BU cannot silently substitute outgoing adjacency");
    }
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
