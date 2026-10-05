#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ecg_algorithms.h"
#include "cache_sim/ecg_algorithm.h"
#include "kernel_census_receipt.h"

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

void testPoptDirectMatrix() {
    for (uint32_t vertices : {1u, 129u, 513u, 33025u}) {
        std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges;
        for (uint32_t source = 0; source < vertices; source += 17) {
            edges.emplace_back(source, (source * 13 + 1) % vertices, 1);
            edges.emplace_back(source, (source * 29 + 3) % vertices, 1);
        }
        const Fixture graph(vertices, true, edges);
        for (uint32_t per_line : {8u, 16u}) {
            const uint32_t lines = (vertices + per_line - 1) / per_line;
            const uint32_t epoch_size = (vertices + 255) / 256;
            const uint32_t sub_epoch = (epoch_size + 127) / 128;
            popt_reref::FullMatrix direct;
            direct.configure(vertices, lines, uint64_t(lines) * 256);
            for (uint32_t source = 0; source < vertices; ++source)
                for (uint64_t index = graph.offsets[source]; index < graph.offsets[source + 1]; ++index)
                    direct.reference(graph.edges[index].id / per_line, source);
            direct.finish();
            std::vector<int32_t> last(uint64_t(lines) * 256, -1);
            for (uint32_t vertex = 0; vertex < vertices; ++vertex)
                for (uint64_t index = graph.in_offsets[vertex]; index < graph.in_offsets[vertex + 1]; ++index) {
                    const uint32_t source = graph.in_edges[index].id;
                    int32_t& value = last[uint64_t(vertex / per_line) * 256 + source / epoch_size];
                    value = std::max(value, static_cast<int32_t>(source));
                }
            bool equal = true;
            for (uint32_t line = 0; line < lines; ++line) {
                uint8_t distance = 127;
                for (uint32_t epoch = 256; epoch-- > 0;) {
                    const int32_t source = last[uint64_t(line) * 256 + epoch];
                    uint8_t expected;
                    if (source >= 0) {
                        expected = static_cast<uint8_t>((source % epoch_size) / sub_epoch) & 0x7f;
                        distance = 1;
                    } else {
                        expected = 0x80 | distance;
                        if (distance < 127)
                            ++distance;
                    }
                    equal = equal && direct.data()[uint64_t(epoch) * lines + line] == expected;
                }
            }
            check(equal && direct.bytes() == uint64_t(lines) * 256,
                  "direct compressed P-OPT construction equals the canonical transpose formulation");
        }
    }
    popt_reref::FullMatrix matrix;
    matrix.configure(32, 6, 6 * 256);
    matrix.reference(0, 17);
    matrix.reference(2, 5);
    matrix.finish();
    cache_sim::GraphCacheContext context;
    context.topology.num_vertices = 32;
    context.registerPropertyArray(reinterpret_cast<const void*>(0x1000), 32, 4, 512);
    context.registerPropertyArray(reinterpret_cast<const void*>(0x2000), 32, 8, 512);
    context.registerPropertyArray(reinterpret_cast<const void*>(0x3000), 32, 4, 512);
    context.regions[0].popt_line_offset = 0;
    context.regions[1].popt_line_offset = 2;
    context.compound_popt = true;
    context.initRereference(matrix.data(), 6, 256, 32, 64);
    context.setCurrentVertices(0, 0);
    check(context.findNextRef(0x1000) == 17 && context.findNextRef(0x2000) == 5 &&
          context.isPoptData(0x1000) && !context.isPoptData(0x3000) &&
          context.findRegion(0x3000) != nullptr,
          "P-OPT uses the correct typed bank and excludes source-only streamed output");
}

void testPoptConstantRanksPreserveOtherMechanics() {
    popt_reref::FullMatrix matrix;
    matrix.configure(64, 4, 4 * 256);
    matrix.reference(0, 1);
    matrix.reference(1, 10);
    matrix.reference(2, 5);
    matrix.finish();
    for (bool constant : {false, true}) {
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 64;
        context.registerPropertyArray(reinterpret_cast<const void*>(0x1000), 64, 4, 192, 0.5);
        context.regions[0].popt_line_offset = 0;
        context.compound_popt = true;
        context.popt_constant_rank = constant;
        context.initRereference(matrix.data(), 4, 256, 64, 64);
        context.setCurrentVertices(0, 0);
        cache_sim::CacheLevel cache("L3", 192, 64, 3, cache_sim::EvictionPolicy::POPT);
        cache.initGraphContext(&context);
        std::vector<cache_sim::CacheLine> ways(3);
        for (std::size_t way = 0; way < ways.size(); ++way) {
            ways[way].valid = true;
            ways[way].line_addr = 0x1000 + way * 64;
            ways[way].last_access = 10 + way;
            ways[way].rrpv = 0;
        }
        ways[0].rrpv = 7;
        check(cache.selectVictimForTest(ways) == (constant ? 0 : 1) &&
              ways[1].rrpv == (constant ? 0 : 7),
              "constant ranks change only future ordering and the resulting RRIP tie set");
        check(context.popt_lookup_count == 3 && context.popt_original_rank_sum == 16 &&
              context.popt_constant_rank_lookups == (constant ? 3 : 0),
              "the ablation still computes every original matrix rank before withholding it");
        const auto lookups = context.popt_lookup_count;
        ways[2].valid = false;
        check(cache.selectVictimForTest(ways) == 2 && context.popt_lookup_count == lookups,
              "both rank modes fill invalid ways without unnecessary future lookups");
        ways[2].valid = true;
        ways[2].line_addr = 0x3000;
        check(cache.selectVictimForTest(ways) == 2 && context.popt_lookup_count == lookups,
              "constant ranks preserve P-OPT non-property victim precedence");
        ways[2].line_addr = 0x1080;
        context.setCurrentVertices(UINT32_MAX, UINT32_MAX);
        check(cache.selectVictimForTest(ways) == 0 && context.popt_lookup_count == lookups,
              "both modes retain the current P-OPT outside-pass LRU fallback");
        context.setCurrentVertices(0, 0);
        cache.insert(0x1000, true);
        cache_sim::CacheLine snapshot;
        check(cache.lineSnapshotForTest(0x1000, snapshot) && snapshot.rrpv == 6 && snapshot.dirty,
              "the ablation preserves ordinary P-OPT insertion state");
        check(cache.access(0x1000, false) && cache.lineSnapshotForTest(0x1000, snapshot) &&
              snapshot.rrpv == 0 && snapshot.dirty,
              "the ablation preserves ordinary P-OPT hit promotion and dirty state");
        context.compound_popt = false;
        check(context.poptVictimRank(0x1000) == 1,
              "the diagnostic flag cannot change archived non-compound P-OPT ranks");
    }
}

void testGraspReferenceConsumer() {
    popt_reref::FullMatrix matrix;
    matrix.configure(64, 4, 4 * 256);
    matrix.reference(0, 1);
    matrix.reference(1, 10);
    matrix.reference(2, 1);
    matrix.finish();
    for (bool flat : {false, true}) {
        cache_sim::GraphCacheContext context, ordinary_context;
        for (auto* c : {&context, &ordinary_context}) {
            c->topology.num_vertices = 64;
            c->registerPropertyArray(reinterpret_cast<const void*>(0x1000), 64, 4, 192, 0.5);
            c->registerPropertyArray(reinterpret_cast<const void*>(0x2000), 64, 4, 192, 0.5);
        }
        context.regions[0].popt_line_offset = 0;
        context.compound_popt = true;
        context.popt_constant_rank = flat;
        context.grasp_reference_consumer = true;
        context.initRereference(matrix.data(), 4, 256, 64, 64);
        context.setCurrentVertices(0, 0);
        cache_sim::CacheLevel cache("L3", 192, 64, 3, cache_sim::EvictionPolicy::GRASP);
        cache_sim::CacheLevel ordinary("L3", 192, 64, 3, cache_sim::EvictionPolicy::GRASP);
        cache.initGraphContext(&context);
        ordinary.initGraphContext(&ordinary_context);
        std::vector<cache_sim::CacheLine> ways(3);
        for (std::size_t way = 0; way < ways.size(); ++way) {
            ways[way].valid = true;
            ways[way].line_addr = way == 2 ? 0x2000 : 0x1000 + way * 64;
            ways[way].last_access = way == 2 ? 0 : 10 + way;
            ways[way].rrpv = way == 1 ? 0 : 7;
        }
        check(cache.selectVictimForTest(ways) == (flat ? 0 : 1) && ways[1].rrpv == 0 &&
              ways[0].rrpv == 7 && ways[2].rrpv == 7,
              "reference ranks refine GRASP across RRPVs without non-property priority or extra aging");
        check(context.popt_lookup_count == 2 && context.popt_original_rank_sum == 11 &&
              context.popt_constant_rank_lookups == (flat ? 2 : 0) &&
              context.grasp_reference_lower_rrpv_overrides == (flat ? 0 : 1),
              "both diagnostic modes compute covered matrix ranks, excluding streamed output");
        const uint64_t lookups = context.popt_lookup_count;
        ways[2].valid = false;
        check(cache.selectVictimForTest(ways) == 2 && context.popt_lookup_count == lookups,
              "reference diagnostics preserve invalid-way precedence without speculative lookups");
        ways[2].valid = true;
        ways[0].line_addr = 0x2000;
        check(cache.selectVictimForTest(ways) == 0 && context.popt_lookup_count == lookups,
              "uncovered GRASP base victims cannot be replaced by property priority");
        ways[0].line_addr = 0x1000;
        context.setCurrentVertices(UINT32_MAX, UINT32_MAX);
        check(cache.selectVictimForTest(ways) == 0 && context.popt_lookup_count == lookups,
              "missing source progress retains GRASP rather than P-OPT's LRU fallback");
        context.setCurrentVertices(0, 0);
        context.rereference.matrix = nullptr;
        check(cache.selectVictimForTest(ways) == 0 && context.popt_lookup_count == lookups,
              "unbound and outside-pass reference behavior is ordinary GRASP");
        context.rereference.matrix = matrix.data();
        ways[1].line_addr = 0x1080;
        ways[0].rrpv = 6;
        ways[1].rrpv = 0;
        ways[2].rrpv = 5;
        auto expected = ways;
        const auto base = ordinary.selectVictimForTest(expected);
        check(cache.selectVictimForTest(ways) == base &&
              ways[0].rrpv == expected[0].rrpv && ways[1].rrpv == expected[1].rrpv &&
              ways[2].rrpv == expected[2].rrpv,
              "equal reference ranks retain the first GRASP victim and exactly its original aging");
        cache.insert(0x1000, true);
        ordinary.insert(0x1000, true);
        cache_sim::CacheLine a, b;
        check(cache.lineSnapshotForTest(0x1000, a) && ordinary.lineSnapshotForTest(0x1000, b) &&
              a.rrpv == b.rrpv && a.dirty == b.dirty,
              "reference diagnostics preserve ordinary GRASP insertion and dirty state");
        check(cache.access(0x1000, false) && ordinary.access(0x1000, false) &&
              cache.lineSnapshotForTest(0x1000, a) && ordinary.lineSnapshotForTest(0x1000, b) &&
              a.rrpv == b.rrpv && a.dirty == b.dirty,
              "reference diagnostics preserve ordinary GRASP hit promotion");
    }
}

void testPreparedPoptQueryReuse() {
    using namespace ecg_algorithm;
    const Fixture fixture(64, false, {{0, 16, 1}, {0, 32, 1}, {16, 32, 1}});
    const auto graph = fixture.view();
    Options options;
    options.algorithm = Algorithm::SPMV;
    options.repetitions = 2;
    options.evidence = options.capture_values = true;
    setenv("POPT_MATRIX_STREAM_SIM", "0", 1);
    cache_sim::CacheHierarchy cache(128, 2, 256, 2, 1024, 2, 64,
        cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::POPT);
    auto prepared = std::make_shared<cache_sim::PreparedSpmvMatrix>();
    Result first;
    uint64_t previous_accesses = 0;
    for (unsigned query = 0; query < 2; ++query) {
        cache_sim::AlgorithmBackend backend(cache, options, 1024, false, true, prepared);
        const auto result = ecg_algorithm::run(graph, options, backend);
        if (!query) first = result;
        check(result.values_f32 == first.values_f32 && result.work_digest == first.work_digest &&
              result.property_writes == 192 && result.actual_records == 12,
              "real P-OPT queries independently initialize and execute the same shared SpMV work");
        check(prepared->constructions() == 1 && prepared->bytes() == 1024 &&
              (query ? backend.preparationTraffic().total_accesses == 0 :
                       backend.preparationTraffic().total_accesses > 0),
              "the actual cache backend constructs the FULL matrix once and then borrows it");
        auto total = backend.preparationTraffic();
        total.accumulate(backend.querySetupTraffic());
        total.accumulate(backend.kernelTraffic());
        check(total.total_accesses == cache.getTotalAccesses() - previous_accesses &&
              total.offchip() == backend.queryTraffic().offchip(),
              "query-local snapshots do not include previous queries or duplicate preparation");
        previous_accesses = cache.getTotalAccesses();
    }
    const Fixture other(64, false, {{0, 16, 1}, {0, 32, 1}, {16, 32, 1}});
    bool rejected = false;
    try {
        cache_sim::AlgorithmBackend backend(cache, options, 1024, false, true, prepared);
        backend.start(other.view());
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "shared matrix reuse rejects a different graph owner");
    auto interrupted = std::make_shared<cache_sim::PreparedSpmvMatrix>();
    {
        cache_sim::AlgorithmBackend first_borrow(cache, options, 1024, false, true, interrupted);
        first_borrow.start(graph);
        rejected = false;
        try {
            cache_sim::AlgorithmBackend overlap(cache, options, 1024, false, true, interrupted);
            overlap.start(graph);
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "shared preparation cannot have overlapping active queries");
    }
    rejected = false;
    try {
        cache_sim::AlgorithmBackend poisoned(cache, options, 1024, false, true, interrupted);
        poisoned.start(graph);
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "an interrupted prepared query detaches and cannot be silently resumed");
    Options too_small = options;
    too_small.build_limits.maximum_auxiliary_bytes = 16;
    rejected = false;
    try {
        cache_sim::AlgorithmBackend limited(cache, too_small, 1024, false, true, prepared);
        ecg_algorithm::run(graph, too_small, limited);
    } catch (const std::length_error&) { rejected = true; }
    check(rejected, "a reused matrix remains subject to the query's explicit resource limits");
}

void testFrontierProducerAndCounts() {
    static_assert(!std::is_same<ecg_frontier::RecordStream, ecg_window::RecordStream>::value,
                  "frontier and window streams must not alias");
    static_assert(!std::is_constructible<cache_sim::WindowRuntime, cache_sim::CacheHierarchy&,
                  const ecg_frontier::RecordStream&, uint64_t, uint64_t, bool, uint8_t>::value,
                  "frontier records cannot enter the window decoder");
    const ecg_frontier::Profile profile(128);
    const std::array<uint32_t, 14> rows{{0, 1, 7, 8, 15, 16, 31, 40, 63, 64, 65, 80, 120, 127}};
    bool equal = true;
    for (uint32_t selected = 1; selected < (1u << rows.size()); ++selected) {
        uint64_t state = 0;
        for (std::size_t index = rows.size(); index-- > 0;) {
            if (!(selected & (1u << index)))
                continue;
            const uint32_t row = rows[index];
            uint16_t expected = 512;
            for (std::size_t later = index + 1; later < rows.size(); ++later) {
                if (!(selected & (1u << later)))
                    continue;
                const uint32_t gap = rows[later] / 8 - row / 8;
                expected |= gap < 8 ? uint16_t{1} << gap : 256;
            }
            const uint16_t actual = profile.reverse(state, row);
            const uint64_t frozen = state;
            equal = equal && actual == expected && state < (uint64_t{1} << 52) &&
                profile.reverse(state, row) == actual && state == frozen &&
                profile.decode(actual, row, 16) == ((16 + row / 8) << 10 | actual);
        }
    }
    check(equal, "all consumer subsets preserve exact cohort presence, overflow and frozen row duplicates");
    ecg_frontier::Bitmap active{{uint64_t{1} << 5, 0, 0, 0}};
    uint64_t first = 0;
    const auto hint = profile.decode(0x224, 0, 0);
    check(profile.rank(hint, 0, 0, active, true, first) && first == 5 &&
          profile.rank(hint, 0, 0, active, false, first) && first == 2 &&
          !profile.rank(profile.decode(0x200, 0, 0), 0, 0, active, true, first) &&
          !profile.rank(profile.decode(0x300, 0, 0), 0, 0, active, true, first) &&
          !profile.rank(hint, 16, 16, active, false, first),
          "frontier ranking distinguishes usable intersections from empty, beyond-only and old-pass data");
    bool rejected = false;
    try { profile.decode(0x280, 127, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "frontier tokens cannot invent consumers outside the graph");

    const Fixture graph(128, true, {{0, 0, 1}, {0, 1, 1}, {8, 1, 1}, {40, 2, 1}, {80, 3, 1}, {127, 0, 1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        const auto layout = ecg_frontier::Layout::select(3, width);
        const auto stream = ecg_window::build(profile, graph.edges.size(), layout, ecg_record::BuildLimits{},
            [&](uint64_t row) { return std::pair<uint64_t, uint64_t>{graph.offsets[row], graph.offsets[row + 1]}; },
            [&](uint64_t index) { return static_cast<uint32_t>(graph.edges[index].id); },
            [](const void*, uint64_t, bool) {});
        check(stream.known_records == 6 && stream.stats.property_lines == 1 &&
              stream.stats.auxiliary_peak_bytes == 64 &&
              stream.stats.carrier_payload_bytes == 6 * width &&
              stream.word(0) >> layout.id_bits == 0x322 &&
              stream.word(1) >> layout.id_bits == 0x322 && layout.id(stream.word(1), 128) == 1,
              "typed frontier carriers preserve IDs, known-empty words and graph-only construction");
    }

    ecg_frontier::Counts counts;
    uint64_t reads = 0, writes = 0, steps = 0;
    const auto memory = [&](const void*, uint64_t bytes, bool write) { (write ? writes : reads) += bytes; };
    const auto step = [&] { ++steps; };
    counts.initialize(profile, 0, memory, step);
    check(counts.begin(memory, step)[0] == 1 && counts.consume(0, memory, step),
          "consuming the seed clears its current cohort before its property accesses");
    counts.append(0, 16, memory, step);
    counts.append(1, 17, memory, step);
    counts.append(2, 40, memory, step);
    check(counts.remaining() == 0 && counts.next() == 3,
          "discoveries belong to the next frontier, not current eligibility");
    counts.close();
    rejected = false;
    try { counts.swap(2, step); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "frontier context rejects an incorrect algorithm frontier length");
    counts.swap(3, step);
    check(counts.begin(memory, step)[0] == ((uint64_t{1} << 2) | (uint64_t{1} << 5)) &&
          !counts.consume(16, memory, step) && counts.consume(17, memory, step) &&
          counts.consume(40, memory, step),
          "a cohort clears only after its last remaining source, not its first source");
    rejected = false;
    try { counts.consume(40, memory, step); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "frontier context cannot underflow");
    counts.close();
    counts.swap(0, step);
    counts.finish();
    check(reads == counts.read_bytes && writes == counts.write_bytes &&
          steps == counts.arithmetic_steps + counts.scan_steps + counts.swaps &&
          reads == 4 * (8 + 2 * profile.bins) && writes == 2048 + 4 * 8,
          "all source counter reads, writes, arithmetic, scans and swaps are charged");
}

void testFrontierVictimAndContext() {
    const ecg_frontier::Profile profile(128);
    const ecg_frontier::Bitmap current{{(uint64_t{1} << 4) | (uint64_t{1} << 5), 0, 0, 0}};
    for (bool gating : {false, true}) {
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 128;
        context.registerPropertyArray(reinterpret_cast<const void*>(0x1000), 128, 4, 192, 0.5);
        cache_sim::CacheLevel cache("L3", 192, 64, 3, cache_sim::EvictionPolicy::GRASP);
        cache.initGraphContext(&context);
        cache.prepareRecord(cache_sim::EvictionPolicy::GRASP);
        cache.configureFrontier(profile, 0x1000, true, gating);
        cache.windowProgress(0, 0, true);
        cache.frontierContext(0, current);
        std::vector<cache_sim::CacheLine> ways(3);
        for (std::size_t way = 0; way < ways.size(); ++way) {
            ways[way].valid = true;
            ways[way].line_addr = 0x1000 + way * 64;
            ways[way].rrpv = 7;
            ways[way].last_access = way == 2 ? 0 : 10 + way;
        }
        ways[0].record_metadata.state = ways[1].record_metadata.state = ecg_record::LineState::FINITE;
        ways[0].record_metadata.value = profile.decode(0x224, 0, 0);
        ways[1].record_metadata.value = profile.decode(0x210, 0, 0);
        check(cache.selectVictimForTest(ways) == (gating ? 0 : 1),
              "frontier gating alone changes the first-cohort ranking of eligible victims");
        ways[1].rrpv = 6;
        check(cache.selectVictimForTest(ways) == 0, "frontier ranking cannot steal RRPV6 capacity");
        ways[1].rrpv = 7;
        ways[0].record_metadata.value = profile.decode(0x200, 0, 0);
        check(cache.selectVictimForTest(ways) == 0, "known-empty frontier data remains neutral, not DEAD");
        ways[0].record_metadata.value = profile.decode(0x224, 0, 0);
        bool rejected = false;
        try { cache.windowProgress(0, 5, true); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "cache source progress cannot skip a remaining frontier cohort");
        cache.windowProgress(0, 4, true);
        cache.clearFrontierCohort(0, 4);
        cache.windowProgress(0, 5, true);
        cache.clearFrontierCohort(0, 5);
        cache.windowProgress(0, 16, false);
        check(cache.selectVictimForTest(ways) == 2, "frontier pass closure uses LRU with intact recency");
        cache.windowProgress(16, 16, true);
        cache.frontierContext(16, current);
        check(cache.selectVictimForTest(ways) == 0, "old-pass frontier words cannot revive after a new bitmap");
        cache.insert(0x1000, false);
        check(cache.observeWindow(0x1000, 10) &&
              cache.applyWindow(0x1000, 10, profile.decode(0x224, 0, 0)) == ecg_record::ApplyResult::EXPIRED &&
              cache.observeWindow(0x1000, 11) &&
              cache.applyWindow(0x1000, 10, profile.decode(0x224, 0, 16)) == ecg_record::ApplyResult::STALE &&
              cache.applyWindow(0x1000, 11, 0) == ecg_record::ApplyResult::APPLIED,
              "frontier delivery preserves old-pass expiry, event cutoffs and newer UNKNOWN publication");
    }
}

void testWindowObservationProfile() {
    using cache_sim::window_observation::Profile;
    const Profile profile(64, 32);
    uint64_t entry = 0;
    check(profile.reverse(entry, 26) == 0 && profile.reverse(entry, 18) == 0x206 &&
          profile.reverse(entry, 12) == 0x20e && profile.reverse(entry, 12) == 0x20e &&
          (entry >> 49) == 0,
          "window observer freezes same-row line hints and counts only future distinct rows");
    check(profile.decode(0x20e, 12, 0) == 0x39 &&
          profile.decode(0x20e, 12, 32) == ((uint64_t{39} << 3) | 1),
          "window observer captures an absolute endpoint in the original pass");
    entry = 0;
    check(profile.reverse(entry, 40) == 0 && profile.reverse(entry, 12) == 0x242 &&
          profile.reverse(entry, 12) == 0x242 && profile.decode(0x242, 12, 0) == 0x58,
          "same-row duplicates preserve the frozen next-cohort token");
    entry = 0;
    check(profile.reverse(entry, 32) == 0 && profile.reverse(entry, 31) == 0x240 &&
          profile.reverse(entry, 30) == 0x207,
          "the selected cohort does not gain an invented successor at its boundary");
    const Profile partial(35, 32);
    check(partial.decode(0x240, 12, 0) == (uint64_t{9} << 3),
          "a partial final cohort ends at the final logical bin");
    for (uint16_t token : {uint16_t{1}, uint16_t{0x241}, uint16_t{0x400}}) {
        bool rejected = false;
        try { partial.decode(token, 12, 0); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "window observer rejects noncanonical or outside-domain tokens");
    }
    const Profile distant(320, 32);
    entry = 0;
    check(distant.reverse(entry, 256) == 0 && distant.reverse(entry, 12) == 0,
          "cohort gap overflow becomes UNKNOWN, never a nearer false window");
    entry = 0;
    for (uint32_t row = 12; row > 0; --row)
        profile.reverse(entry, row);
    check(profile.reverse(entry, 0) == 0x23b,
          "window structural strength saturates without becoming a probability");
    const Profile patents(3774768);
    check(patents.cohort_rows == 16384 && patents.bin_rows == 2048 && patents.bins == 1844,
          "the fixed structural sizing rule agrees with the Patents contract");
}

void testWindowObservationCallbacks() {
    using cache_sim::window_observation::Observer;
    const Fixture graph(128, true, {{0, 0, 1}, {0, 1, 1}, {0, 16, 1}, {1, 0, 1}, {16, 16, 1}});
    Observer observer(graph.view(), 0x1000, 1, 3, true, 1 << 20);
    std::vector<cache_sim::CacheLine> lines(3);
    for (std::size_t way = 0; way < lines.size(); ++way) {
        lines[way].line_addr = way < 2 ? 0x1000 + way * 64 : 0x2000;
        lines[way].valid = true;
        lines[way].rrpv = 7;
    }
    observer.initialSet(0, lines.data(), lines.size());
    observer.beginPass();
    observer.visitVertex(0);
    for (uint32_t index = 0; index < 3; ++index) {
        const uint32_t target = index == 2 ? 16 : index;
        observer.designated(index, target);
        observer.beforeAccess(0x1000 + target * 4, false);
        observer.afterAccess(0x1000 + target * 4, false, false, false);
    }
    for (uint64_t event = 0; event < Observer::kSamplePeriod; ++event) {
        const std::size_t victim = event + 1 == Observer::kSamplePeriod ? 0 : 2;
        const uint64_t incoming = 0x4000 + event * 64;
        observer.beforeAccess(incoming, false);
        const auto before = lines;
        observer.insertion(0, lines.data(), lines.size(), victim, incoming);
        for (std::size_t way = 0; way < lines.size(); ++way)
            check(lines[way].line_addr == before[way].line_addr && lines[way].rrpv == before[way].rrpv &&
                  lines[way].valid == before[way].valid && lines[way].dirty == before[way].dirty,
                  "observer receives read-only candidates and never changes the active victim state");
        lines[victim].line_addr = incoming;
        observer.afterAccess(incoming, false, true, true);
    }
    observer.beforeAccess(0x1000, false);
    observer.insertion(0, lines.data(), lines.size(), 0, 0x1000);
    lines[0].line_addr = 0x1000;
    observer.afterAccess(0x1000, false, true, true);
    observer.endPass();
    observer.finish(3);
    std::ostringstream report;
    observer.write(report);
    const auto text = report.str();
    check(text.find("\"hypothetical_overrides\":1") != std::string::npos &&
          text.find("\"base_first\":1") != std::string::npos &&
          text.find("\"base_first_llc\":1") != std::string::npos &&
          text.find("\"base_first_memory\":1") != std::string::npos,
          "a sampled disagreement observes later real demand without enacting its hypothetical victim");
    check(text.find("\"queue_stale\":1") != std::string::npos &&
          text.find("\"queue_pending\":0") != std::string::npos,
          "older delivered hints cannot overwrite the newer same-line observation");
    bool rejected = false;
    try { Observer too_large(graph.view(), 0x1000, 1, 3, true, 1); }
    catch (const std::length_error&) { rejected = true; }
    check(rejected, "diagnostic storage is rejected before exceeding its explicit reservation");
}

void testWindowPublishedWriteSurvival() {
    using cache_sim::window_observation::Observer;
    const Fixture graph(128, true, {{0, 0, 1}, {0, 16, 1}, {1, 0, 1}, {16, 16, 1}});
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        Observer observer(graph.view(), 0x1000, 1, 3, true, 1 << 20);
        std::vector<cache_sim::CacheLine> lines(3);
        for (std::size_t way = 0; way < lines.size(); ++way) {
            lines[way].line_addr = way < 2 ? 0x1000 + way * 64 : 0x2000;
            lines[way].valid = true;
            lines[way].rrpv = 7;
        }
        observer.initialSet(0, lines.data(), lines.size());
        observer.beginPass();
        observer.visitVertex(0);
        for (uint32_t index = 0; index < 2; ++index) {
            observer.designated(index, index * 16);
            observer.beforeAccess(0x1000 + index * 64, false);
            observer.afterAccess(0x1000 + index * 64, false, false, false);
        }
        if (scenario != 1)
            for (unsigned step = 0; step < Observer::kLatency; ++step) {
                observer.beforeAccess(0x2000, false);
                observer.afterAccess(0x2000, false, false, false);
            }
        if (scenario == 2)
            observer.visitVertex(8);
        if (scenario == 5) {
            observer.endPass();
            observer.beginPass();
            observer.visitVertex(0);
        }
        const bool write = scenario != 3;
        observer.beforeAccess(0x1000, write);
        observer.afterAccess(0x1000, write, false, false);
        if (scenario == 4) {
            observer.visitVertex(1);
            observer.designated(2, 0);
            observer.beforeAccess(0x1000, false);
            observer.afterAccess(0x1000, false, false, false);
        }
        for (uint64_t event = 0; event < Observer::kSamplePeriod; ++event) {
            const std::size_t victim = event + 1 == Observer::kSamplePeriod ? 0 : 2;
            const uint64_t incoming = 0x4000 + event * 64;
            observer.beforeAccess(incoming, false);
            observer.insertion(0, lines.data(), lines.size(), victim, incoming);
            lines[victim].line_addr = incoming;
            observer.afterAccess(incoming, false, true, true);
        }
        observer.endPass();
        observer.finish(scenario == 4 ? 3 : 2);
        std::ostringstream report;
        observer.write(report);
        const auto text = report.str();
        const auto start = text.find("\"preserved_delivered\":");
        const auto section = start == std::string::npos ? "" : text.substr(start, text.find('}', start) - start);
        check(section.find(std::string("\"hypothetical_overrides\":") + (scenario == 0 ? "1" : "0")) !=
                  std::string::npos,
              "write survival cannot revive pending, expired, read-invalidated, superseded or prior-pass evidence");
        if (scenario == 1)
            check(text.find("\"writes_cancelling_pending\":1") != std::string::npos,
                  "a discovery write keeps the newer-event cutoff and cancels an undelivered hint");
    }
}

void testWindowCandidateAttribution() {
    using cache_sim::window_observation::Observer;
    const char* expected[] = {"\"only_ineligible_worse\":1", "\"live_base_equal_ranks\":1",
                              "\"live_base_already_worst\":1", "\"hypothetical_overrides\":1",
                              "\"live_base_eligible_without_live_hint\":1"};
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        const uint32_t future_a = scenario == 1 || scenario == 2 ? 16 : 1;
        const uint32_t future_b = scenario == 1 ? 17 : scenario == 2 ? 1 : 16;
        std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges{
            {0, 0, 1}, {0, 16, 1}, {future_a, 0, 1}};
        if (scenario != 4)
            edges.emplace_back(future_b, 16, 1);
        const Fixture graph(128, true, edges);
        Observer observer(graph.view(), 0x1000, 1, 3, true, 1 << 20);
        std::vector<cache_sim::CacheLine> lines(3);
        for (std::size_t way = 0; way < lines.size(); ++way) {
            lines[way].line_addr = way < 2 ? 0x1000 + way * 64 : 0x2000;
            lines[way].valid = true;
            lines[way].rrpv = scenario == 0 && way == 1 ? 6 : 7;
        }
        observer.initialSet(0, lines.data(), lines.size());
        observer.beginPass();
        observer.visitVertex(0);
        for (uint32_t index = 0; index < 2; ++index) {
            observer.designated(index, index * 16);
            observer.beforeAccess(0x1000 + index * 64, false);
            observer.afterAccess(0x1000 + index * 64, false, false, false);
        }
        for (uint64_t event = 0; event < Observer::kSamplePeriod; ++event) {
            const std::size_t victim = event + 1 == Observer::kSamplePeriod ? 0 : 2;
            const uint64_t incoming = 0x4000 + event * 64;
            observer.beforeAccess(incoming, false);
            observer.insertion(0, lines.data(), lines.size(), victim, incoming);
            lines[victim].line_addr = incoming;
            observer.afterAccess(incoming, false, true, true);
        }
        observer.endPass();
        observer.finish(2);
        std::ostringstream report;
        observer.write(report);
        const auto text = report.str();
        const auto start = text.find("\"delivered\":");
        const auto section = text.substr(start, text.find('}', start) - start);
        check(section.find(expected[scenario]) != std::string::npos,
              "candidate attribution distinguishes protected, equal-rank, already-worst and actionable cases");
        if (scenario == 0)
            check(section.find("\"hypothetical_overrides\":0") != std::string::npos,
                  "an ineligible worse-ranked line is diagnostic evidence, not a legal replacement candidate");
    }
}

void testWindowCheckedStorePublication() {
    using cache_sim::window_observation::Observer;
    const Fixture graph(4096, true, {{0, 0, 1}, {0, 16, 1}, {1, 16, 1}, {32, 0, 1}});
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        Observer observer(graph.view(), 0x1000, 1, 3, true, 1 << 20);
        std::vector<cache_sim::CacheLine> lines(3);
        for (std::size_t way = 0; way < lines.size(); ++way) {
            lines[way].line_addr = way == 0 ? 0x1040 : way == 1 ? 0x1000 : 0x100000;
            lines[way].valid = true;
            lines[way].rrpv = 7;
        }
        observer.initialSet(0, lines.data(), lines.size());
        observer.beginPass();
        observer.visitVertex(0);
        for (uint32_t index = 0; index < 2; ++index) {
            observer.designated(index, index * 16);
            observer.beforeAccess(0x1000 + index * 64, false);
            observer.afterAccess(0x1000 + index * 64, false, false, false);
        }
        if (scenario == 4) {
            observer.beforeAccess(0x100000, false);
            observer.afterAccess(0x100000, false, false, false);
        }
        if (scenario == 5) {
            observer.endPass();
            observer.beginPass();
            observer.visitVertex(0);
        }
        if (scenario == 9)
            observer.visitVertex(1);
        if (scenario != 1)
            observer.associateStore(scenario == 2 ? 0 : 1,
                scenario == 3 ? 17 : scenario == 7 ? 1040 : 16,
                scenario == 7 ? 0 : 0x1000, scenario == 8 ? 8 : 4);
        const uint64_t write_address = scenario == 3 ? 0x1044 : 0x1040;
        observer.beforeAccess(write_address, true);
        observer.afterAccess(write_address, true, false, false);
        if (scenario == 6) {
            observer.visitVertex(1);
            observer.designated(2, 16);
            observer.beforeAccess(0x1040, false);
            observer.afterAccess(0x1040, false, false, false);
        }
        for (uint64_t event = 0; event < Observer::kSamplePeriod; ++event) {
            const std::size_t victim = event + 1 == Observer::kSamplePeriod ? 0 : 2;
            const uint64_t incoming = 0x200000 + event * 64;
            observer.beforeAccess(incoming, false);
            observer.insertion(0, lines.data(), lines.size(), victim, incoming);
            lines[victim].line_addr = incoming;
            observer.afterAccess(incoming, false, true, true);
        }
        observer.endPass();
        observer.finish(scenario == 6 ? 3 : 2);
        std::ostringstream report;
        observer.write(report);
        const auto text = report.str();
        const auto start = text.find("\"forwarded_delivered\":");
        const auto section = start == std::string::npos ? "" : text.substr(start, text.find('}', start) - start);
        check(section.find(std::string("\"hypothetical_overrides\":") + (scenario == 0 ? "1" : "0")) !=
                  std::string::npos,
              "only an exact paired store may publish a current hint under its newer event identity");
        const bool paired = scenario == 0 || scenario == 6;
        check(text.find(std::string("\"forwarded_store_updates\":") + (paired ? "1" : "0")) !=
                  std::string::npos,
              "unpaired, wrong-index/element/binding/width, intervening and cross-pass stores cannot borrow hints");
        if (scenario == 6)
            check(text.find("\"forwarded_stale\":1") != std::string::npos &&
                  text.find("\"forwarded_applied\":0") != std::string::npos,
                  "a newer UNKNOWN observation supersedes an undelivered paired-store publication");
    }
}

void testProtectedWindowProbe() {
    using cache_sim::window_observation::Observer;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        const uint32_t future_a = scenario == 2 || scenario == 3 ? 16 : 1;
        const uint32_t future_b = scenario == 2 ? 17 : scenario == 3 ? 1 : scenario == 5 ? 8 : 16;
        const bool tie_alternative = scenario >= 5;
        std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges{
            {0, 0, 1}, {0, 16, 1}, {future_b, 16, 1}};
        if (scenario != 4)
            edges.emplace_back(future_a, 0, 1);
        if (tie_alternative) {
            edges.emplace_back(0, 32, 1);
            edges.emplace_back(scenario == 6 ? 8 : scenario == 7 ? 17 : 16, 32, 1);
        }
        const Fixture graph(128, true, edges);
        Observer observer(graph.view(), 0x1000, 1, 4, true, 1 << 20);
        std::vector<cache_sim::CacheLine> lines(4);
        for (std::size_t way = 0; way < lines.size(); ++way) {
            lines[way].line_addr = way < 2 || (way == 2 && tie_alternative) ?
                0x1000 + way * 64 : 0x2000 + way * 64;
            lines[way].valid = true;
            lines[way].rrpv = way == 1 ? (scenario == 1 ? 5 : 6) : 7;
        }
        lines[1].dirty = scenario == 0;
        lines[0].dirty = scenario == 6;
        observer.initialSet(0, lines.data(), lines.size());
        observer.beginPass();
        observer.visitVertex(0);
        const uint32_t reads = tie_alternative ? 3 : 2;
        for (uint32_t index = 0; index < reads; ++index) {
            observer.designated(index, index * 16);
            observer.beforeAccess(0x1000 + index * 64, false);
            observer.afterAccess(0x1000 + index * 64, false, false, false);
        }
        for (uint64_t event = 0; event < Observer::kSamplePeriod; ++event) {
            const std::size_t victim = event + 1 == Observer::kSamplePeriod ? 0 : 3;
            const uint64_t incoming = 0x4000 + event * 64;
            observer.beforeAccess(incoming, false);
            const auto before = lines;
            observer.insertion(0, lines.data(), lines.size(), victim, incoming);
            for (std::size_t way = 0; way < lines.size(); ++way)
                check(lines[way].line_addr == before[way].line_addr &&
                      lines[way].dirty == before[way].dirty && lines[way].rrpv == before[way].rrpv,
                      "protected candidate analysis never changes cache contents, dirtiness or RRIP");
            lines[victim].line_addr = incoming;
            observer.afterAccess(incoming, false, true, true);
        }
        observer.endPass();
        observer.finish(reads);
        std::ostringstream report;
        observer.write(report);
        const auto text = report.str();
        const auto start = text.find("\"protected_probe\":");
        const auto section = start == std::string::npos ? "" : text.substr(start, text.find('}', start) - start);
        const bool selected = scenario == 0 || scenario == 6;
        check(section.find(std::string("\"protected_selected\":") + (selected ? "1" : "0")) !=
                  std::string::npos,
              "only live RRPV6 strictly worse than the current hypothetical choice enters the protected probe");
        if (scenario == 0)
            check(section.find("\"extra_immediate_writeback\":1") != std::string::npos &&
                  section.find("\"additional_choices\":1") != std::string::npos,
                  "protected probe counts dirty-line risk and new interventions separately");
        if (scenario == 6)
            check(section.find("\"avoided_immediate_writeback\":1") != std::string::npos &&
                  section.find("\"retargeted_choices\":1") != std::string::npos &&
                  section.find("\"additional_choices\":0") != std::string::npos,
                  "retargeting a tie-only choice is not counted as an additional decision");
    }
}

void testActiveWindowCodecAndCache() {
    using ecg_window::Layout;
    using ecg_window::Profile;
    check(Layout::select((uint64_t{1} << 22) - 1, 0).record_bytes == 4 &&
          Layout::select(uint64_t{1} << 22, 0).record_bytes == 8 &&
          Layout::select(UINT32_MAX, 0).id_bits == 32,
          "window records select width from actual VID headroom and ten token bits");
    bool rejected = false;
    try { Layout::select(uint64_t{1} << 22, 4); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "window records cannot silently discard metadata to force four bytes");
    const Profile profile(64, 32);
    const Fixture graph(64, true, {{12, 0, 1}, {12, 1, 1}, {18, 2, 1}, {26, 3, 1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        const auto layout = Layout::select(3, width);
        uint64_t read_bytes = 0, write_bytes = 0;
        const auto stream = ecg_window::build(profile, graph.edges.size(), layout, ecg_record::BuildLimits{},
            [&](uint64_t row) { return std::pair<uint64_t,uint64_t>{graph.offsets[row], graph.offsets[row + 1]}; },
            [&](uint64_t index) { read_bytes += 4; return static_cast<uint32_t>(graph.edges[index].id); },
            [&](const void*, uint64_t bytes, bool write) { (write ? write_bytes : read_bytes) += bytes; });
        check(stream.known_records == 3 && (stream.word(0) >> layout.id_bits) == 0x20e &&
              (stream.word(1) >> layout.id_bits) == 0x20e && layout.id(stream.word(1), 64) == 1 &&
              stream.stats.carrier_payload_bytes == 4 * width && stream.stats.auxiliary_peak_bytes == 32 &&
              read_bytes > 0 && write_bytes >= 4 * width + 32,
              "actual window records preserve IDs, frozen duplicate hints and observed construction");
        rejected = false;
        try { layout.id(stream.word(0) | (uint64_t{1} << 63), 64); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "actual window records reject noncanonical unused upper bits");
    }
    ecg_record::LineMetadata metadata;
    ecg_window::observe(metadata, 10);
    check(!ecg_window::apply(metadata, 9, 16) && ecg_window::apply(metadata, 10, 16) &&
          !ecg_window::apply(metadata, 10, 24),
          "window delivery requires its matching pending observation, not any live line");
    ecg_window::observe(metadata, 11);
    check(!ecg_window::apply(metadata, 10, 16) && ecg_window::apply(metadata, 11, 0) &&
          metadata.state == ecg_record::LineState::UNKNOWN && metadata.value == 11,
          "newer unknown publication retains its ordering cutoff");

    for (uint8_t floor : {uint8_t{6}, uint8_t{7}}) {
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 128;
        context.registerPropertyArray(reinterpret_cast<const void*>(0x1000), 128, 4, 192, 0.5);
        cache_sim::CacheLevel cache("L3", 192, 64, 3, cache_sim::EvictionPolicy::GRASP);
        cache.initGraphContext(&context);
        cache.prepareRecord(cache_sim::EvictionPolicy::GRASP);
        const Profile active_profile(128);
        cache.configureWindow(active_profile, 0x1000, true, floor);
        cache.windowProgress(0, 0, true);
        std::vector<cache_sim::CacheLine> ways(3);
        for (std::size_t way = 0; way < ways.size(); ++way) {
            ways[way].valid = true;
            ways[way].line_addr = 0x1000 + way * 64;
            ways[way].rrpv = way == 1 ? 6 : 7;
        }
        ways[0].record_metadata.state = ways[1].record_metadata.state = ecg_record::LineState::FINITE;
        ways[0].record_metadata.value = uint64_t{2} << 3;
        ways[1].record_metadata.value = uint64_t{17} << 3;
        check(cache.selectVictimForTest(ways) == (floor == 6 ? 1 : 0),
              "active window policy changes only its declared RRPV6/7 candidate set");
        ways[0].record_metadata.state = ecg_record::LineState::UNKNOWN;
        check(cache.selectVictimForTest(ways) == 0,
              "an unrankable window base victim preserves actual GRASP");
        ways[0].record_metadata.state = ecg_record::LineState::FINITE;
        cache.insert(0x1000, false);
        check(cache.observeWindow(0x1000, 10) &&
              cache.applyWindow(0x1000, 10, uint64_t{2} << 3) == ecg_record::ApplyResult::APPLIED &&
              cache.observeWindow(0x1000, 11) &&
              cache.applyWindow(0x1000, 10, uint64_t{2} << 3) == ecg_record::ApplyResult::STALE &&
              cache.applyWindow(0x1000, 11, 0) == ecg_record::ApplyResult::APPLIED,
              "resident window delivery respects matching observation order and newer UNKNOWN");
        cache.windowProgress(128, 128, true);
        check(cache.selectVictimForTest(ways) == 0, "previous-pass windows cannot revive after progress resets");
        check(cache.observeWindow(0x1000, 12) &&
              cache.applyWindow(0x1000, 12, uint64_t{2} << 3) == ecg_record::ApplyResult::EXPIRED &&
              cache.applyWindow(0x1040, 12, uint64_t{129} << 3) == ecg_record::ApplyResult::NOT_RESIDENT,
              "old-pass and nonresident window updates cannot allocate or revive data");
        rejected = false;
        try { cache.windowProgress(0, 0, true); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "window receiver rejects backward absolute progress");
    }
}

void testActiveWindowAssociation() {
    const Fixture graph(32, true, {{0, 16, 1}, {0, 17, 1}, {1, 16, 1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        const ecg_window::Profile profile(32);
        const auto layout = ecg_window::Layout::select(17, width);
        auto stream = ecg_window::build(profile, graph.edges.size(), layout, ecg_record::BuildLimits{},
            [&](uint64_t row) { return std::pair<uint64_t,uint64_t>{graph.offsets[row], graph.offsets[row + 1]}; },
            [&](uint64_t index) { return static_cast<uint32_t>(graph.edges[index].id); },
            [](const void*, uint64_t, bool) {});
        alignas(64) uint32_t depth[32]{};
        const uint64_t base = reinterpret_cast<uint64_t>(depth);
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 32;
        context.registerPropertyArray(depth, 32, 4, 128, 0.5);
        cache_sim::CacheHierarchy cache(64, 1, 64, 1, 128, 2, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        cache.initGraphContext(&context);
        cache.prepareRecord(cache_sim::EvictionPolicy::GRASP);
        cache_sim::WindowRuntime runtime(cache, stream, 32, base, true, 6);
        runtime.beginPass();
        runtime.visitVertex(0);
        runtime.rowContext(0, 0, 2);
        const uint64_t word = runtime.recordLoad(0);
        check(word == stream.word(0), "window transport returns the actual loaded record word");
        bool rejected = false;
        try { runtime.property(0, word ^ 1, base); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "property operands cannot substitute a synthesized record word");
        check(runtime.property(0, word, base) == 16,
              "window property access uses the real record's unmodified destination");
        rejected = false;
        try { runtime.associateStore(0, 17, base, 4); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "window publication requires the exact element, not another VID on its line");
        runtime.associateStore(0, 16, base, 4);
        runtime.memory(base + 16 * 4, true);
        runtime.closePass();
        runtime.finish(1);
        std::ostringstream report;
        runtime.write(report);
        check(report.str().find("\"forwarded_stores\":1") != std::string::npos &&
              report.str().find("\"pending\":0") != std::string::npos &&
              report.str().find("\"skipped_positions\":2") != std::string::npos,
              "window runtime closes real filtered work and drains its paired store publication");
        rejected = false;
        try { runtime.recordLoad(1); }
        catch (const std::logic_error&) { rejected = true; }
        check(rejected, "finished window bindings cannot access released carriers");
    }
}

void testWindowTransportMatchesGrasp() {
    std::vector<std::tuple<uint32_t,uint32_t,int32_t>> edges;
    for (uint32_t source = 0; source < 64; ++source)
        for (uint32_t offset : {1u, 17u, 33u})
            edges.emplace_back(source, (source + offset) % 64, 1);
    const Fixture graph(64, true, edges);
    for (const auto& settings : {
            std::pair<uint8_t, bool>{4, false}, {8, false}, {4, true}, {8, true}}) {
        const auto width = settings.first;
        const bool phased = settings.second;
        const auto layout = ecg_window::Layout::select(63, width);
        auto stream = ecg_window::build(ecg_window::Profile(64), graph.edges.size(), layout,
            ecg_record::BuildLimits{},
            [&](uint64_t row) { return std::pair<uint64_t,uint64_t>{graph.offsets[row], graph.offsets[row + 1]}; },
            [&](uint64_t index) { return static_cast<uint32_t>(graph.edges[index].id); },
            [](const void*, uint64_t, bool) {});
        alignas(64) uint32_t depth[64]{};
        const uint64_t base = reinterpret_cast<uint64_t>(depth);
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 64;
        context.registerPropertyArray(depth, 64, 4, 128, 0.5);
        cache_sim::CacheHierarchy actual(64,1,64,1,128,2,64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        cache_sim::CacheHierarchy reference(64,1,64,1,128,2,64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        actual.initGraphContext(&context);
        reference.initGraphContext(&context);
        actual.prepareRecord(cache_sim::EvictionPolicy::GRASP);
        if (phased)
            reference.configureGraspPhases();
        cache_sim::WindowRuntime runtime(actual, stream, 64, base, false, 6, phased);
        runtime.beginPass();
        if (phased)
            reference.graspGraphPass(true);
        for (uint64_t source = 0; source < 64; ++source) {
            runtime.visitVertex(source);
            for (uint64_t offset : {source, source + 1}) {
                const uint64_t address = reinterpret_cast<uint64_t>(&graph.offsets[offset]);
                runtime.memory(address, false);
                reference.access(address, false);
            }
            runtime.rowContext(source, graph.offsets[source], graph.offsets[source + 1]);
            for (uint64_t index = graph.offsets[source]; index < graph.offsets[source + 1]; ++index) {
                const auto word = runtime.recordLoad(index);
                reference.access(reinterpret_cast<uint64_t>(stream.data()) + index * width, false);
                const uint32_t id = runtime.property(index, word, base);
                reference.access(base + uint64_t(id) * 4, false);
                runtime.associateStore(index, id, base, 4);
                runtime.memory(base + uint64_t(id) * 4, true);
                reference.access(base + uint64_t(id) * 4, true);
            }
        }
        runtime.closePass();
        if (phased)
            reference.graspGraphPass(false);
        for (uint64_t index = 0; index < 128; ++index) {
            const uint64_t address = index % 3 ? 0x1000000 + (index % 16) * 64 : base + (index % 64) * 4;
            runtime.memory(address, false);
            reference.access(address, false);
        }
        runtime.finish(graph.edges.size());
        check(actual.getMemoryAccesses() == reference.getMemoryAccesses() &&
              actual.getWritebackTraffic() == reference.getWritebackTraffic() &&
              actual.getL3Stats().hits.load() == reference.getL3Stats().hits.load() &&
              actual.windowStats().overrides == 0,
              "window transport matches its declared all-phase or phase-scoped GRASP data behavior");
    }
}

void testScopedGraspKeepsCacheState() {
    cache_sim::GraphCacheContext context;
    context.topology.num_vertices = 64;
    context.registerPropertyArray(reinterpret_cast<const void*>(0x1000), 64, 4, 192, 0.5);
    cache_sim::CacheLevel cache("L3", 192, 64, 3, cache_sim::EvictionPolicy::GRASP);
    cache.initGraphContext(&context);
    cache.insert(0x1000, true);
    cache_sim::CacheLine before, after;
    check(cache.lineSnapshotForTest(0x1000, before), "GRASP setup inserts actual data");
    cache.configureGraspPhases();
    check(cache.lineSnapshotForTest(0x1000, after) && before.tag == after.tag &&
          before.dirty == after.dirty && before.rrpv == after.rrpv && before.last_access == after.last_access,
          "phase-scoped GRASP configuration does not reset cached data or replacement history");
    std::vector<cache_sim::CacheLine> ways(3);
    for (auto& way : ways) way.valid = true;
    ways[0].last_access = 10; ways[0].rrpv = 0;
    ways[1].last_access = 20; ways[1].rrpv = 7;
    ways[2].last_access = 5; ways[2].rrpv = 1;
    check(cache.selectVictimForTest(ways) == 2 && ways[0].rrpv == 0 && ways[1].rrpv == 7 && ways[2].rrpv == 1,
          "outside graph passes selects true LRU without ageing GRASP metadata");
    cache.graspGraphPass(true);
    check(cache.selectVictimForTest(ways) == 1, "graph passes select the ordinary GRASP victim");
    cache.graspGraphPass(false);
    check(cache.selectVictimForTest(ways) == 2, "leaving the pass restores LRU without a cache walk");
    bool rejected = false;
    try { cache.graspGraphPass(false); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "phase transitions cannot silently duplicate or skip state");
}

void testUnboundRecordPreparation() {
    using namespace ecg_algorithm;
    testWindowObservationProfile();
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

// The kernel census under the study's roster, each policy configured as the
// algorithms driver configures it. Arming the census changes no decision, and
// clearing or setting every last-level dirty bit at kernel entry changes only
// writebacks, by exactly the setup writebacks and exit residue the census
// reports, so the clean kernel total needs no second run. The comparison masks
// writebacks, so a policy that read the dirty bit fails it only where the
// victims it changes move another counter. On this graph a clean-first GRASP
// changes victims without doing so; GRASP's victim loop is held instead by the
// cache replay's clean-entry counterfactual in test_ecg_record_cache.cc.
//
// The cache model sees real addresses, so a run's heap history moves its
// traffic, and so can OpenMP's thread interleaving: two identical runs in one
// process need not agree. The matrix runs every cell as a fresh process on one
// thread with address randomisation off, and every run compared here is run
// the same way, as this binary started again under setarch -R; a repeated run
// must reproduce the first, which checks that premise.
struct CensusRosterRow {
    const char* name;
    cache_sim::EvictionPolicy policy;
    bool grasp_paper, popt, records, rrpv;
};
constexpr CensusRosterRow kCensusRoster[] = {
    {"GRASP_PAPER", cache_sim::EvictionPolicy::GRASP, true, false, false, false},
    {"POPT:UNCHARGED", cache_sim::EvictionPolicy::POPT, false, true, false, false},
    {"ECG governed-first", cache_sim::EvictionPolicy::GRASP, true, false, true, false},
    {"ECG governed-first RRPV order", cache_sim::EvictionPolicy::GRASP, true, false, true, true}};
using CensusEntry = cache_sim::CacheHierarchy::KernelEntryForTest;
constexpr CensusEntry kCensusModes[] = {CensusEntry::AS_BUILT, CensusEntry::UNARMED,
    CensusEntry::CLEAN, CensusEntry::DIRTY, CensusEntry::UNARMED};
constexpr const char* kCensusRunFlag = "--census-roster-run";

// One roster run, in the child: the kernel's transfers on the first line,
// then the receipt.
int censusRosterRun(const char* row_argument, const char* mode_argument) {
    using namespace ecg_algorithm;
    const unsigned long row_index = std::strtoul(row_argument, nullptr, 10);
    const unsigned long mode_index = std::strtoul(mode_argument, nullptr, 10);
    if (row_index >= std::size(kCensusRoster) || mode_index >= std::size(kCensusModes))
        return 2;
    const CensusRosterRow& row = kCensusRoster[row_index];
    std::mt19937 random(20260923);
    std::uniform_int_distribution<uint32_t> vertex(0, 255);
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges;
    while (edges.size() < 2048) {
        const uint32_t from = vertex(random), to = vertex(random);
        if (from != to)
            edges.emplace_back(from, to, 1);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    const Fixture graph(256, true, edges);
    Options options;
    options.algorithm = Algorithm::SPMV;
    options.repetitions = 2;
    options.records = row.records;
    if (row.records) {
        options.mechanism = ecg_record::Mechanism::REPLACEMENT;
        options.record_base_policy = RecordBasePolicy::GRASP_PAPER;
        options.record_governed_first = true;
        options.record_rrpv_order = row.rrpv;
    }
    if (setenv("GRASP_BOUNDARY_MODE", row.grasp_paper ? "capacity" : "vertex", 1) != 0 ||
            (row.popt && setenv("POPT_MATRIX_STREAM_SIM", "0", 1) != 0))
        return 3;
    cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
        cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, row.policy);
    cache.setKernelEntryForTest(kCensusModes[mode_index]);
    cache_sim::AlgorithmBackend backend(cache, options, 2048, row.grasp_paper, row.popt);
    ecg_algorithm::run(graph.view(), options, backend);
    std::cout << backend.kernelTraffic().offchip() << '\n' << cache.toJSON() << std::flush;
    return std::cout ? 0 : 4;
}

// Starts this binary again for one roster run, as the matrix starts a cell:
// under setarch -R, on one thread, with the environment's cache, ECG, P-OPT,
// GRASP and OpenMP settings removed. Returns the run's output, or nothing if
// it did not exit cleanly.
std::optional<std::string> censusRosterSpawn(size_t row, size_t mode) {
    std::string self(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", self.data(), self.size());
    utsname machine{};
    if (length <= 0 || static_cast<size_t>(length) >= self.size() || uname(&machine) != 0)
        return std::nullopt;
    self.resize(static_cast<size_t>(length));
    std::vector<std::string> arguments = {"setarch", machine.machine, "-R", self, kCensusRunFlag,
        std::to_string(row), std::to_string(mode)};
    std::vector<std::string> environment = {
        "OMP_NUM_THREADS=1", "OMP_WAIT_POLICY=PASSIVE", "GRAPHBREW_SIDEBAND_LOG=0"};
    for (char** variable = environ; *variable; ++variable) {
        const std::string entry(*variable);
        bool kept = entry.rfind("GRAPHBREW_SIDEBAND_LOG=", 0) != 0;
        for (const char* prefix : {"CACHE_", "ECG_", "GEM5_", "SNIPER_", "POPT_", "GRASP_",
                "STRUCTURAL_", "TOPT_", "OMP_"})
            kept = kept && entry.rfind(prefix, 0) != 0;
        if (kept)
            environment.push_back(entry);
    }
    std::vector<char*> argv, envp;
    for (std::string& argument : arguments)
        argv.push_back(argument.data());
    for (std::string& entry : environment)
        envp.push_back(entry.data());
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    int ends[2];
    if (pipe(ends) != 0)
        return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    pid_t child = 0;
    const int spawned = posix_spawnp(&child, "setarch", &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(ends[1]);
    std::string output;
    char buffer[1 << 14];
    for (ssize_t count; spawned == 0 && (count = read(ends[0], buffer, sizeof buffer)) != 0;) {
        if (count > 0)
            output.append(buffer, static_cast<size_t>(count));
        else if (errno != EINTR)
            break;
    }
    close(ends[0]);
    int status = 0;
    if (spawned != 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
        return std::nullopt;
    return output;
}

void testKernelCensusUnderTheRoster() {
    constexpr size_t kBuilt = 0, kUnarmed = 1, kClean = 2, kDirty = 3, kRepeat = 4;
    const char* const segments[] = {
        "before_first_pass", "first_pass", "between_passes", "later_passes", "after_last_pass"};
    const std::regex writebacks("\"(writebacks|llc_writebacks|total_offchip_traffic)\": [0-9]+");
    const auto masked = [&](const std::string& json) {
        return std::regex_replace(json, writebacks, "\"$1\": #");
    };
    const auto same = [](const std::string& expected, const std::string& actual, const std::string& what) {
        if (expected == actual)
            return !expected.empty();
        std::istringstream left(expected), right(actual);
        std::string want, got;
        while (std::getline(left, want) && std::getline(right, got) && want == got) {}
        std::cerr << what << ": " << want << " | " << got << '\n';
        return false;
    };

    for (size_t index = 0; index < std::size(kCensusRoster); ++index) {
        const CensusRosterRow& row = kCensusRoster[index];
        const std::string at = std::string(" under ") + row.name;
        std::string receipt[std::size(kCensusModes)], census[std::size(kCensusModes)];
        uint64_t kernel[std::size(kCensusModes)] = {};
        bool ran = true;
        for (size_t mode = 0; ran && mode < std::size(kCensusModes); ++mode) {
            const auto output = censusRosterSpawn(index, mode);
            const size_t line = output ? output->find('\n') : std::string::npos;
            ran = line != std::string::npos;
            if (!ran)
                break;
            kernel[mode] = std::strtoull(output->c_str(), nullptr, 10);
            receipt[mode] = output->substr(line + 1);
            census[mode] = kernelCensusLine(receipt[mode]);
        }
        check(ran, ("every roster run exits cleanly as a fresh process" + at).c_str());
        if (!ran)
            continue;
        check(same(receipt[kUnarmed], receipt[kRepeat], "repeat" + at) && kernel[kRepeat] == kernel[kUnarmed],
              ("a roster run repeats exactly in a fresh process" + at).c_str());
        const auto field = [&](size_t mode, const std::string& key) {
            const uint64_t value = receiptValue(census[mode], key);
            check(value != UINT64_MAX, ("the kernel census carries " + key + at).c_str());
            return value;
        };
        const auto segment = [&](size_t mode, const char* name, const std::string& key) {
            const uint64_t value = segmentValue(census[mode], name, key);
            check(value != UINT64_MAX, ("every census segment carries " + key + at).c_str());
            return value;
        };

        check(census[kUnarmed].empty() && !census[kBuilt].empty() &&
              same(receipt[kUnarmed], withoutKernelCensus(receipt[kBuilt]), "armed" + at),
              ("arming the kernel census changes no decision" + at).c_str());
        const auto counter = [&](const char* key) {
            const uint64_t value = receiptValue(receipt[kBuilt], key);
            return value == UINT64_MAX ? 0 : value;
        };
        if (row.records)
            check(counter("ecg_record_victim_decisions") > 0 &&
                  (!row.rrpv || counter("ecg_record_victim_rrpv_ordered") > 0),
                  ("the record victim rule decides evictions in the census graph" + at).c_str());
        for (const size_t mode : {kClean, kDirty})
            check(same(masked(receipt[kUnarmed]), masked(withoutKernelCensus(receipt[mode])),
                       (mode == kClean ? "clean entry" : "dirty entry") + at),
                  ("the entry dirty state moves no counter but the writebacks" + at).c_str());

        check(field(kClean, "entry_dirty_lines") == 0 && field(kClean, "entry_property_dirty_lines") == 0 &&
              field(kClean, "entry_dirty_writebacks") == 0 && field(kClean, "entry_dirty_rewritten") == 0 &&
              field(kClean, "exit_entry_dirty_lines") == 0,
              ("a clean kernel entry leaves the kernel no setup dirt" + at).c_str());
        check(field(kDirty, "entry_valid_lines") > 0 &&
              field(kDirty, "entry_dirty_lines") == field(kDirty, "entry_valid_lines") &&
              field(kDirty, "entry_property_dirty_lines") == field(kDirty, "entry_property_lines") &&
              field(kDirty, "entry_dirty_writebacks") > 0,
              ("a dirty kernel entry charges the kernel setup writebacks" + at).c_str());
        check(field(kBuilt, "entries") == 1 && field(kBuilt, "passes") == 2 &&
              field(kBuilt, "exit_property_lines") > 0,
              ("the census sees one kernel, both passes and property lines at exit" + at).c_str());
        for (const size_t mode : {kBuilt, kDirty}) {
            uint64_t transfers = 0;
            for (const char* name : segments) {
                for (const char* key : {"total_accesses", "memory_accesses", "prefetch_fills", "llc_hits",
                        "llc_misses", "llc_property_hits", "llc_property_misses"})
                    check(segment(mode, name, key) == segment(kClean, name, key),
                          ("the entry state moves no access, miss or fill between segments" + at).c_str());
                check(segment(kClean, name, "entry_dirty_writebacks") == 0 &&
                      segment(kClean, name, "llc_writebacks") ==
                          segment(mode, name, "llc_writebacks") - segment(mode, name, "entry_dirty_writebacks"),
                      ("each segment's clean writebacks are its own less its setup writebacks" + at).c_str());
                transfers += segment(mode, name, "total_offchip_traffic");
            }
            check(transfers == kernel[mode],
                  ("the census segments sum to the kernel's transfers" + at).c_str());
            for (const char* key : {"entries", "passes", "entry_valid_lines", "entry_property_lines",
                    "exit_valid_lines", "exit_property_lines"})
                check(field(mode, key) == field(kClean, key),
                      ("the entry state moves no line into or out of the cache" + at).c_str());
            check(field(kClean, "exit_dirty_lines") ==
                      field(mode, "exit_dirty_lines") - field(mode, "exit_entry_dirty_lines"),
                  ("the clean exit residue is the residue less the setup dirt still resident" + at).c_str());
            check(kernel[mode] - field(mode, "entry_dirty_writebacks") +
                      (field(mode, "exit_dirty_lines") - field(mode, "exit_entry_dirty_lines")) ==
                      kernel[kClean] + field(kClean, "exit_dirty_lines"),
                  ("the clean kernel total needs no clean run" + at).c_str());
        }
    }
}

} // namespace

// Record mode reads one edge stream per edge, as CSR does. The
// spy counts the ordinary reads each record pass makes of the fixture's
// interleaved edges, split into ID and weight slots, and of the bound carrier;
// a paired record load reaches no hook, so carrier reads are inspections only.
struct EdgeStreamSpy : ecg_algorithm::PlainBackend {
    uint64_t edges = 0, edges_end = 0, carrier = 0, carrier_end = 0;
    uint64_t id_reads = 0, weight_reads = 0, carrier_reads = 0;
    bool open = false;
    explicit EdgeStreamSpy(const Fixture& fixture)
        : edges(reinterpret_cast<uint64_t>(fixture.edges.data())),
          edges_end(edges + fixture.edges.size() * sizeof(Fixture::Edge)) {}
    void bind(const ecg_record::NativeConfiguration& configuration,
              const ecg_record::RecordStream& stream) {
        ecg_algorithm::PlainBackend::bind(configuration, stream);
        carrier = configuration.record_base;
        carrier_end = carrier + stream.size() * stream.layout.record_bytes;
    }
    void beginPass(bool has_next) {
        ecg_algorithm::PlainBackend::beginPass(has_next);
        open = true;
    }
    void closePass() {
        ecg_algorithm::PlainBackend::closePass();
        open = false;
    }
    void memory(const void* pointer, uint64_t, bool write) {
        const uint64_t address = reinterpret_cast<uint64_t>(pointer);
        if (!open || write)
            return;
        if (address >= edges && address < edges_end) {
            if ((address - edges) % sizeof(Fixture::Edge) == 0)
                ++id_reads;
            else
                ++weight_reads;
        } else if (address >= carrier && address < carrier_end) {
            ++carrier_reads;
        }
    }
};

template<class Backend>
ecg_algorithm::Result runOn(Backend& backend, const ecg_algorithm::GraphView& graph,
                            ecg_algorithm::Algorithm algorithm, bool records, uint8_t bytes,
                            const std::vector<uint32_t>& sources = {}) {
    ecg_algorithm::Options options;
    options.algorithm = algorithm;
    options.records = records;
    options.record_bytes = bytes;
    options.source = sources.empty() ? 0 : sources.front();
    options.sources = sources;
    options.delta = 2;
    options.capture_values = options.evidence = true;
    return ecg_algorithm::run(graph, options, backend);
}

void testOneEdgeStreamPerEdge() {
    using ecg_algorithm::Algorithm;
    // From source 0 the reverse pass finds rows 1, 2 and 3 mixed, row 4 all
    // rejected and row 8 empty; 5 reaches 0 only as a source; 6 and 7 are isolated.
    const Fixture dag(9, true, {{0,1,1}, {0,2,1}, {1,2,1}, {1,3,1}, {2,1,1}, {2,3,1},
                                {3,0,1}, {3,4,1}, {3,8,1}, {4,1,1}, {4,3,1}, {5,0,1}});
    // Brandes examines every edge of every vertex a source reaches once on the way back.
    const auto examined = [&dag](const std::vector<uint32_t>& sources) {
        uint64_t total = 0;
        for (uint32_t source : sources) {
            std::vector<bool> seen(dag.offsets.size() - 1);
            std::vector<uint32_t> queue{source};
            seen[source] = true;
            for (std::size_t head = 0; head < queue.size(); ++head) {
                const uint32_t vertex = queue[head];
                for (uint64_t index = dag.offsets[vertex]; index < dag.offsets[vertex + 1]; ++index) {
                    ++total;
                    const auto next = static_cast<uint32_t>(dag.edges[index].id);
                    if (!seen[next]) {
                        seen[next] = true;
                        queue.push_back(next);
                    }
                }
            }
        }
        return total;
    };
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        for (const auto& sources : {std::vector<uint32_t>{0}, std::vector<uint32_t>{0, 3, 5}}) {
            ecg_algorithm::PlainBackend plain;
            EdgeStreamSpy spy(dag);
            const auto csr = runOn(plain, dag.view(), Algorithm::BC, false, width, sources);
            const auto ecg = runOn(spy, dag.view(), Algorithm::BC, true, width, sources);
            check(spy.id_reads == 0 && spy.weight_reads == 0,
                  "BC's record passes read no CSR edge");
            check(spy.carrier_reads == examined(sources),
                  "BC inspects one record per reverse-pass edge examination");
            check(csr.result_digest == ecg.result_digest && csr.work_digest == ecg.work_digest &&
                  csr.position_digest == ecg.position_digest && csr.values_f32 == ecg.values_f32,
                  "BC's record inspection keeps CSR's result and work");
        }
    }
    // Weights 1 and 2 are light at delta 2, so every SSSP pass filters its rows.
    const Fixture weighted(6, true, {{0,1,1}, {0,2,3}, {0,5,9}, {1,2,1}, {1,3,4},
                                     {2,3,1}, {2,4,5}, {3,5,2}, {4,5,1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        for (Algorithm algorithm : {Algorithm::SSSP, Algorithm::SPMV}) {
            ecg_algorithm::PlainBackend plain;
            EdgeStreamSpy spy(weighted);
            const auto csr = runOn(plain, weighted.view(true), algorithm, false, width);
            const auto ecg = runOn(spy, weighted.view(true), algorithm, true, width);
            check(spy.id_reads == 0 && spy.weight_reads == 0 && spy.carrier_reads == 0,
                  "weighted record passes read no interleaved edge");
            check(csr.result_digest == ecg.result_digest && csr.work_digest == ecg.work_digest &&
                  csr.position_digest == ecg.position_digest && csr.weight_reads == ecg.weight_reads,
                  "weighted record passes keep CSR's result, work and weight reads");
        }
    }
}

void testEdgeStreamAccounting() {
    using ecg_algorithm::Algorithm;
    using Engine = ecg_algorithm::Engine<ecg_algorithm::PlainBackend>;
    const Fixture dag(9, true, {{0,1,1}, {0,2,1}, {1,2,1}, {1,3,1}, {2,1,1}, {2,3,1},
                                {3,0,1}, {3,4,1}, {3,8,1}, {4,1,1}, {4,3,1}, {5,0,1}});
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        ecg_algorithm::PlainBackend plain, other;
        const auto csr = runOn(plain, dag.view(), Algorithm::BC, false, width);
        const auto ecg = runOn(other, dag.view(), Algorithm::BC, true, width);
        // From source 0 the reverse pass examines rows 0, 1, 2, 3, 4 and 8: 11 edges.
        check(ecg.record_inspections == 11 && ecg.record_inspection_bytes == 11u * width &&
              csr.record_inspections == 0 && csr.record_inspection_bytes == 0 &&
              ecg.record_weight_bytes == 0,
              "BC counts its record inspections apart from the paired loads");
    }
    const Fixture weighted(6, false, {{0,1,1}, {0,2,3}, {1,2,1}, {1,3,4}, {2,3,1},
                                      {2,4,5}, {3,5,2}, {4,5,1}});
    std::vector<int32_t> ids, costs;
    for (const auto& edge : weighted.edges) {
        ids.push_back(edge.id);
        costs.push_back(edge.weight);
    }
    const ecg_algorithm::GraphView split{weighted.offsets.size() - 1, weighted.edges.size(), false,
        weighted.offsets.data(), ids.data(), costs.data(), 4, false, nullptr, nullptr};
    const uint64_t copy = 4 * weighted.edges.size();
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
        for (Algorithm algorithm : {Algorithm::SSSP, Algorithm::SPMV}) {
            ecg_algorithm::PlainBackend a, b, c, d;
            const auto csr = runOn(a, weighted.view(true), algorithm, false, width);
            const auto ecg = runOn(b, weighted.view(true), algorithm, true, width);
            const auto compact = runOn(c, split, algorithm, true, width);
            const auto unweighted = runOn(d, weighted.view(), algorithm, true, width);
            check(ecg.record_weight_bytes == copy && csr.record_weight_bytes == 0 &&
                  compact.record_weight_bytes == 0 && unweighted.record_weight_bytes == 0 &&
                  ecg.record_inspections == 0,
                  "only an interleaved weighted view gets a compact weight copy");
            check(ecg.result_digest == csr.result_digest && compact.result_digest == csr.result_digest &&
                  compact.work_digest == ecg.work_digest && compact.weight_reads == ecg.weight_reads,
                  "the compact copy carries every weight of its edge");
            check(ecg.workspace_peak_bytes >= csr.workspace_peak_bytes + ecg.carrier_bytes + copy,
                  "the workspace holds the weights beside the carrier");
            // The construction scratch is smaller than the copy, so a limit one byte
            // short of arrays, carrier and copy can only fail at the copy.
            check(ecg.construction_auxiliary_peak_bytes < copy, "weight-limit fixture precondition");
            ecg_algorithm::Options tight;
            tight.algorithm = algorithm;
            tight.records = true;
            tight.record_bytes = width;
            tight.delta = 2;
            tight.maximum_workspace_bytes = csr.workspace_peak_bytes + ecg.carrier_bytes + copy - 1;
            std::string refusal;
            try {
                ecg_algorithm::PlainBackend backend;
                ecg_algorithm::run(weighted.view(true), tight, backend);
            } catch (const std::length_error& error) {
                refusal = error.what();
            }
            check(refusal == "algorithm-workspace-limit", "a workspace too small for the weights fails");
        }
        for (Algorithm algorithm : {Algorithm::BFS, Algorithm::BC}) {
            ecg_algorithm::PlainBackend backend;
            check(runOn(backend, weighted.view(true), algorithm, true, width).record_weight_bytes == 0,
                  "kernels that read no weight get no weight copy");
        }
    }
    // An inspection needs the NEXT model, a binding, an open pass and an edge of it.
    const auto refuses = [](auto&& inspect) {
        try {
            inspect();
        } catch (const std::logic_error& error) {
            return std::string(error.what()) == "edge-target-outside-a-record-pass";
        }
        return false;
    };
    ecg_algorithm::Options options;
    options.algorithm = Algorithm::BC;
    options.records = true;
    options.record_bytes = 4;
    ecg_algorithm::PlainBackend backend;
    ecg_algorithm::Result result;
    Engine engine(dag.view(), options, backend, result);
    check(refuses([&] { engine.edgeTarget(0); }), "an inspection without a binding fails closed");
    Engine::Buffer<uint32_t> depth(engine, dag.offsets.size() - 1, "depth", true);
    engine.bind(dag.view(), depth, ecg_record::TraversalMode::ORDERED_FILTERED);
    check(refuses([&] { engine.edgeTarget(0); }), "an inspection outside a pass fails closed");
    engine.beginPass();
    check(engine.edgeTarget(0) == 1 && result.record_inspections == 1,
          "an inspection in a pass returns its edge's target");
    check(refuses([&] { engine.edgeTarget(dag.edges.size()); }),
          "an inspection past the carrier fails closed");
    ecg_algorithm::Options window = options;
    window.record_model = ecg_algorithm::RecordModel::WINDOW;
    ecg_algorithm::PlainBackend window_backend;
    ecg_algorithm::Result window_result;
    Engine window_engine(dag.view(), window, window_backend, window_result);
    check(refuses([&] { window_engine.edgeTarget(0); }), "an inspection needs the NEXT model");
}

// True when `action` throws a standard exception whose message is `expected`.
template<class Action>
bool refusesWith(Action&& action, const std::string& expected) {
    try {
        action();
    } catch (const std::exception& error) {
        if (error.what() != expected)
            std::cerr << "refused with \"" << error.what() << "\", expected \"" << expected << "\"\n";
        return error.what() == expected;
    }
    return false;
}

Fixture declaredGraspGraph() {
    std::mt19937 random(20261004);
    std::uniform_int_distribution<uint32_t> vertex(0, 255);
    std::uniform_int_distribution<int32_t> weight(1, 4);
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> edges = {{0, 1, 1}};
    while (edges.size() < 1024) {
        const uint32_t from = vertex(random), to = vertex(random);
        if (from < to)
            edges.emplace_back(from, to, weight(random));
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        return std::get<0>(left) == std::get<0>(right) && std::get<1>(left) == std::get<1>(right);
    }), edges.end());
    return Fixture(256, false, edges);
}

// §bv C1. Under ecg.grasp-declaration.v1 each kernel phase registers the
// array upstream GRASP's add_region("propertyA", ...) protects, at its
// declared fraction of the last level: SpMV's x, BFS's depth and SSSP's
// distances at the whole capacity (BellmanFordOpt's 100), BC's path counts
// forward and dependencies backward at half (BC.C's 100 - frontier_frac).
// BC adds depth, its per-edge visited check, as the phase's propertyB at the
// other half, the role upstream gives the dense frontier bitmap. Every other
// property array stays a property region but not a GRASP region. The
// historical registration, every property array at half, stays the default.
void testDeclaredGraspRegistration() {
    using namespace ecg_algorithm;
    using Role = cache_sim::GraphCacheContext::GraspRole;
    struct Case {
        Algorithm algorithm;
        bool weighted;
        std::vector<std::tuple<std::string, uint32_t, uint64_t, Role>> declarations;
    };
    const Case cases[] = {
        {Algorithm::SPMV, true, {{"x", 100, 0, Role::PROPERTY_A}}},
        {Algorithm::BFS, false, {{"depth", 100, 0, Role::PROPERTY_A}}},
        {Algorithm::SSSP, true, {{"distances", 100, 0, Role::PROPERTY_A}}},
        {Algorithm::BC, false, {{"path_counts", 50, 0, Role::PROPERTY_A}, {"depth", 50, 0, Role::PROPERTY_B},
                                {"dependency", 50, 1, Role::PROPERTY_A}, {"depth", 50, 1, Role::PROPERTY_B}}}};
    if (setenv("GRASP_BOUNDARY_MODE", "capacity", 1) != 0) {
        check(false, "capacity tiering is configured");
        return;
    }
    const Fixture graph = declaredGraspGraph();
    for (const Case& item : cases) {
        for (bool records : {false, true}) {
            for (bool declared : {false, true}) {
                Options options;
                options.algorithm = item.algorithm;
                options.delta = 2;
                options.records = records;
                options.grasp_declared = declared;
                if (records) {
                    options.mechanism = ecg_record::Mechanism::REPLACEMENT;
                    options.record_base_policy = RecordBasePolicy::GRASP_PAPER;
                }
                cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
                    cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU,
                    cache_sim::EvictionPolicy::GRASP);
                cache_sim::AlgorithmBackend backend(cache, options, 2048, true, false);
                ecg_algorithm::run(graph.view(item.weighted), options, backend);
                const auto& context = backend.graphContext();
                const auto& log = context.graspDeclarations();
                if (!declared) {
                    bool every = context.num_regions > 0 && log.empty() && !context.grasp_declared;
                    for (uint32_t index = 0; index < context.num_regions; ++index)
                        every = every && context.regions[index].grasp_region &&
                            context.regions[index].grasp_hot_percent == 50;
                    check(every, "the historical registration keeps every property array a GRASP region at half");
                    continue;
                }
                bool logged = context.grasp_declared && log.size() == item.declarations.size();
                for (size_t index = 0; logged && index < log.size(); ++index)
                    logged = log[index].region < context.num_regions &&
                        std::string(context.regions[log[index].region].name) ==
                            std::get<0>(item.declarations[index]) &&
                        log[index].percent == std::get<1>(item.declarations[index]) &&
                        log[index].selections == std::get<2>(item.declarations[index]) &&
                        log[index].role == std::get<3>(item.declarations[index]);
                check(logged, "each kernel phase declares upstream GRASP's arrays, fractions and moment");
                std::vector<std::string> last_phase;
                std::vector<uint32_t> last_percents;
                for (const auto& declaration : item.declarations) {
                    if (std::get<3>(declaration) == Role::PROPERTY_A) {
                        last_phase.clear();
                        last_percents.clear();
                    }
                    last_phase.push_back(std::get<0>(declaration));
                    last_percents.push_back(std::get<1>(declaration));
                }
                uint32_t designated_regions = 0;
                bool cold = context.num_regions > 0;
                for (uint32_t index = 0; index < context.num_regions; ++index) {
                    const auto& region = context.regions[index];
                    const auto found = std::find(last_phase.begin(), last_phase.end(), std::string(region.name));
                    const bool designated = found != last_phase.end();
                    designated_regions += region.grasp_region;
                    cold = cold && region.grasp_region == designated &&
                        context.classifyGRASP(region.base_address, 2048) == (designated ? 1u : 3u) &&
                        (!designated || region.grasp_hot_percent ==
                            last_percents[static_cast<std::size_t>(found - last_phase.begin())]);
                }
                check(cold && designated_regions == last_phase.size(),
                      "only the phase's declared arrays are GRASP regions; every other property line is cold");
                const std::string receipt = cache.toJSON();
                const std::string first = "{\"region\":\"" + std::get<0>(item.declarations.front()) +
                    "\",\"role\":\"A\",\"percent\":" + std::to_string(std::get<1>(item.declarations.front())) + ",";
                check(receipt.find("\"grasp_registration\":\"ecg.grasp-declaration.v1\"") != std::string::npos &&
                      receipt.find(first) != std::string::npos,
                      "the receipt attests the declared contract and its declarations");
            }
        }
    }

    // A phase switch changes only later classification: lines keep the RRPV
    // they were inserted with.
    {
        cache_sim::GraphCacheContext context;
        context.topology.num_vertices = 64;
        context.enableGraspDeclarations();
        const bool grasp_declared = context.grasp_declared;
        check(refusesWith([&] { context.registerPropertyArray(reinterpret_cast<const void*>(0x60000), 64, 4, 2048, 0.5); },
                          "declared-registration-designates-by-declaration"),
              "under the declared contract registration designates nothing");
        context.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 2048, 0.5, !grasp_declared, "path_counts");
        context.registerPropertyArray(reinterpret_cast<const void*>(0x20000), 64, 4, 2048, 0.5, !grasp_declared, "dependency");
        context.declareGraspRegion(0x10000, 0.5, 0);
        cache_sim::CacheLevel level("L3", 2048, 64, 4, cache_sim::EvictionPolicy::GRASP);
        level.initGraphContext(&context);
        level.insert(0x10000, false);
        cache_sim::CacheLine before, after;
        const bool inserted = level.lineSnapshotForTest(0x10000, before) && before.rrpv == 1;
        context.declareGraspRegion(0x20000, 0.5, 1);
        check(inserted && level.lineSnapshotForTest(0x10000, after) && after.rrpv == before.rrpv &&
              context.classifyGRASP(0x10000, 2048) == 3 && context.classifyGRASP(0x20000, 2048) == 1,
              "BC's backward declaration keeps forward lines' RRPV and reclassifies only later accesses");
        context.registerPropertyArray(reinterpret_cast<const void*>(0x30000), 64, 4, 2048, 0.5, !grasp_declared, "depth");
        context.registerPropertyArray(reinterpret_cast<const void*>(0x40000), 64, 4, 2048, 0.5, !grasp_declared, "scores");
        context.declareGraspRegion(0x20000, 0.5, 1);
        check(refusesWith([&] { context.declareGraspRegion(0x20000, 0.5, 1, Role::PROPERTY_B); },
                          "grasp-property-b-repeats-property-a") &&
              refusesWith([&] { context.declareGraspRegion(0x30000, 0.6, 1, Role::PROPERTY_B); },
                          "grasp-phase-exceeds-the-last-level"),
              "a phase's propertyB is another array and the phase stays within the last level");
        context.declareGraspRegion(0x30000, 0.5, 1, Role::PROPERTY_B);
        check(context.classifyGRASP(0x20000, 2048) == 1 && context.classifyGRASP(0x30000, 2048) == 1 &&
              context.classifyGRASP(0x10000, 2048) == 3 && context.classifyGRASP(0x40000, 2048) == 3,
              "a phase protects its propertyA and propertyB and nothing else");
        check(refusesWith([&] { context.declareGraspRegion(0x40000, 0.0001, 1, Role::PROPERTY_B); },
                          "grasp-phase-has-two-regions"),
              "a phase has at most two GRASP regions, as upstream's propertyA and propertyB");
        cache_sim::GraphCacheContext unphased;
        unphased.topology.num_vertices = 64;
        unphased.enableGraspDeclarations();
        unphased.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 2048, 0.5, !grasp_declared, "depth");
        check(refusesWith([&] { unphased.declareGraspRegion(0x10000, 0.5, 0, Role::PROPERTY_B); },
                          "grasp-property-b-without-property-a"),
              "a propertyB follows its phase's propertyA");
        check(refusesWith([&] { context.declareGraspRegion(0x50000, 0.5, 2); }, "undeclared-grasp-region") &&
              refusesWith([&] { context.declareGraspRegion(0x20000, 0.0, 2); }, "invalid-grasp-fraction") &&
              refusesWith([&] { context.declareGraspRegion(0x20000, 1.5, 2); }, "invalid-grasp-fraction"),
              "a declaration names a registered array and a fraction in (0, 1]");
        cache_sim::GraphCacheContext historical;
        historical.topology.num_vertices = 64;
        historical.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 2048, 0.5);
        check(refusesWith([&] { historical.declareGraspRegion(0x10000, 0.5, 0); }, "grasp-declarations-not-enabled"),
              "the historical registration takes no declaration");
    }

    // The declared contract tiers by capacity whether or not GRASP_BOUNDARY_MODE
    // is set; the historical registration keeps reading it.
    {
        const char* saved = std::getenv("GRASP_BOUNDARY_MODE");
        const std::string kept = saved ? saved : "";
        unsetenv("GRASP_BOUNDARY_MODE");
        cache_sim::GraphCacheContext declared;
        declared.topology.num_vertices = 64;
        declared.enableGraspDeclarations();
        const bool grasp_declared = declared.grasp_declared;
        declared.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 2048, 0.5, !grasp_declared, "x");
        declared.declareGraspRegion(0x10000, 0.5, 0);
        cache_sim::GraphCacheContext historical;
        historical.topology.num_vertices = 64;
        historical.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 2048, 0.5);
        check(declared.classifyGRASP(0x10000 + 200, 2048) == 1 &&
              std::string(declared.graspBoundaryMode()) == "capacity" &&
              historical.classifyGRASP(0x10000 + 200, 2048) == 2 &&
              std::string(historical.graspBoundaryMode()) == "vertex",
              "the declared contract tiers by capacity even when no boundary mode is set");
        if (setenv("GRASP_BOUNDARY_MODE", kept.empty() ? "capacity" : kept.c_str(), 1) != 0)
            check(false, "capacity tiering is restored");
    }

    // A kernel with no declaration, or BFS's direction-optimizing path with its
    // dense bitmaps, cannot run GRASP tiers under the declared contract.
    const Fixture cliques(10, false, {
        {0,1,1}, {0,2,1}, {0,3,1}, {1,2,1}, {1,3,1}, {2,3,1},
        {4,5,1}, {4,6,1}, {5,6,1}, {7,8,1}});
    for (Algorithm algorithm : {Algorithm::CC, Algorithm::TC}) {
        Options options;
        options.algorithm = algorithm;
        options.grasp_declared = true;
        cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        cache_sim::AlgorithmBackend backend(cache, options, 2048, true, false);
        check(refusesWith([&] { ecg_algorithm::run(cliques.view(), options, backend); }, "undeclared-grasp-region"),
              "a kernel without a GRASP declaration refuses GRASP tiers under the declared contract");
        Options plain = options;
        cache_sim::CacheHierarchy lru(128, 2, 256, 2, 2048, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU);
        cache_sim::AlgorithmBackend untiered(lru, plain, 2048, false, false);
        bool ran = true;
        try { ecg_algorithm::run(cliques.view(), plain, untiered); } catch (const std::exception&) { ran = false; }
        check(ran, "a row without GRASP tiers runs every kernel under the declared contract");
    }
    {
        const Fixture switching = directionSwitchGraph();
        Options options;
        options.algorithm = Algorithm::BFS;
        options.bfs_direction_optimizing = true;
        options.grasp_declared = true;
        cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        cache_sim::AlgorithmBackend backend(cache, options, 2048, true, false);
        check(refusesWith([&] { ecg_algorithm::run(switching.view(), options, backend); },
                          "declared-grasp-has-no-direction-optimizing-bfs"),
              "direction-optimizing BFS has no GRASP declaration");
    }
}

// §bv C2. The cold kernel boundary: setup's dirty data is written back once
// per line, whichever levels hold it dirty, every level is invalidated, and
// the write-backs are setup's, so every row starts its kernel empty.
void testColdKernelEntry() {
    using namespace ecg_algorithm;
    {
        cache_sim::CacheHierarchy cache(128, 2, 256, 2, 1024, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU);
        // L1 is one set of two ways, L2 two sets and L3 four, indexed by line number.
        cache.access(0x10000, true);   // a, line 0x400: a write miss, dirty in every level
        cache.access(0x20040, false);  // b, line 0x801: read in every level ...
        cache.access(0x20040, true);   // ... then dirty in L1 only
        cache.access(0x30040, false);  // c, line 0xC01: evicts a from L1, which stays dirty in L2 and L3
        const uint64_t memory = cache.getMemoryAccesses(), writebacks = cache.getWritebackTraffic();
        check(cache.L1()->validLines() == 2 && cache.L3()->validLines() == 3,
              "the cold-entry fixture holds lines in every level");
        const uint64_t charged = cache.coldKernelEntry();
        check(charged == 2 && cache.getWritebackTraffic() == writebacks + 2 &&
              cache.getMemoryAccesses() == memory,
              "each line dirty at any level is written back once, as last-level write-backs");
        check(cache.L1()->validLines() == 0 && cache.L2()->validLines() == 0 &&
              cache.L3()->validLines() == 0, "no level holds a line after the cold boundary");
        check(cache.coldKernelEntries() == 1 && cache.kernelEntryMaintenanceWritebacks() == 2,
              "the hierarchy records its cold boundary and its write-backs");
        cache.access(0x30040, false);
        check(cache.getMemoryAccesses() == memory + 1, "the kernel's first access to setup data misses");
        const std::string json = cache.toJSON();
        check(json.find("\"kernel_entry\": \"cold\"") != std::string::npos &&
              json.find("\"kernel_entry_maintenance_writebacks\": 2") != std::string::npos &&
              json.find("\"kernel_entry_cold_boundaries\": 1") != std::string::npos &&
              json.find("\"kernel_entry_residual_lines\": [0, 0, 0]") != std::string::npos,
              "the receipt attests one cold boundary, its write-backs and no line left at any level");
        cache_sim::CacheHierarchy built(128, 2, 256, 2, 1024, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU);
        const std::string historical = built.toJSON();
        check(historical.find("\"kernel_entry\": \"as-built\"") != std::string::npos &&
              historical.find("\"kernel_entry_cold_boundaries\": 0") != std::string::npos,
              "the historical boundary stays the default and says so");
    }
    if (setenv("GRASP_BOUNDARY_MODE", "capacity", 1) != 0) {
        check(false, "capacity tiering is configured");
        return;
    }
    const Fixture graph = declaredGraspGraph();
    for (Algorithm algorithm : {Algorithm::SPMV, Algorithm::BC}) {
        for (bool records : {false, true}) {
            for (bool cold : {false, true}) {
                Options options;
                options.algorithm = algorithm;
                options.repetitions = algorithm == Algorithm::SPMV ? 2 : 1;
                options.records = records;
                options.cold_kernel_entry = cold;
                if (records) {
                    options.mechanism = ecg_record::Mechanism::REPLACEMENT;
                    options.record_base_policy = RecordBasePolicy::GRASP_PAPER;
                }
                cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
                    cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU,
                    cache_sim::EvictionPolicy::GRASP);
                cache_sim::AlgorithmBackend backend(cache, options, 2048, true, false);
                ecg_algorithm::run(graph.view(algorithm == Algorithm::SPMV), options, backend);
                const auto setup = backend.setupTraffic();
                const auto kernel = backend.kernelTraffic();
                const std::string json = cache.toJSON();
                check(setup.offchip() + kernel.offchip() == cache.getTotalOffChipTraffic(),
                      "setup and kernel traffic close across the boundary");
                if (!cold) {
                    check(cache.coldKernelEntries() == 0, "the as-built boundary runs no maintenance");
                    continue;
                }
                check(cache.coldKernelEntries() == 1, "one cold boundary per kernel, none at BC's rebind");
                check(setup.llc_writebacks >= cache.kernelEntryMaintenanceWritebacks() &&
                      cache.kernelEntryMaintenanceWritebacks() > 0,
                      "the boundary's write-backs are setup's");
                check(json.find("\"entry_valid_lines\":0,") != std::string::npos &&
                      json.find("\"entry_dirty_lines\":0,") != std::string::npos,
                      "the census finds the kernel's cache empty");
            }
        }
    }
    {
        Options options;
        options.algorithm = Algorithm::SPMV;
        options.records = true;
        options.cold_kernel_entry = true;
        options.mechanism = ecg_record::Mechanism::PREFETCH;
        cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU);
        cache_sim::AlgorithmBackend backend(cache, options, 2048, false, false);
        check(refusesWith([&] { ecg_algorithm::run(graph.view(true), options, backend); },
                          "cold-kernel-entry-requires-no-pending-prefetch"),
              "a mechanism with queued prefetches cannot cross the cold boundary");
    }
}

// §bv C3, at one level: each access is charged to the region that holds it.
void testPropertyRegionAttribution() {
    cache_sim::GraphCacheContext context;
    context.topology.num_vertices = 64;
    context.registerPropertyArray(reinterpret_cast<const void*>(0x10000), 64, 4, 1024, 0.5, true, "x");
    context.registerPropertyArray(reinterpret_cast<const void*>(0x20000), 64, 4, 1024, 0.5, true, "y");
    cache_sim::CacheLevel level("L3", 1024, 64, 4, cache_sim::EvictionPolicy::LRU);
    level.initGraphContext(&context);
    level.access(0x10000, false);            // x: miss
    level.insert(0x10000, false);
    level.access(0x10000, false);            // x: hit
    level.access(0x20000, false);            // y: miss
    level.access(0x20040, false);            // y: miss
    level.insert(0x20040, false);
    level.access(0x20040, false);            // y: hit
    level.access(0x30000, false);            // no region
    check(level.propertyRegionHits(0) == 1 && level.propertyRegionMisses(0) == 1 &&
          level.propertyRegionHits(1) == 1 && level.propertyRegionMisses(1) == 2,
          "each property access is charged to its own region");
    level.markPropertyRegionKernel();
    level.access(0x20000, false);            // y: miss after the boundary
    check(level.propertyRegionKernelMisses(1) == 1 && level.propertyRegionKernelMisses(0) == 0 &&
          level.propertyRegionMisses(1) == 3, "kernel counts start at the boundary");
    level.resetStats();
    check(level.propertyRegionMisses(1) == 0 && level.propertyRegionKernelMisses(1) == 0,
          "a statistics reset clears the region counts with the rest");
}

// §bv C3. The last level counts hits and misses per named property region,
// over the run and from the kernel boundary, so the gathered array's misses
// are comparable across every row; the region-index counters stay as they were.
void testPropertyRegionCounters() {
    using namespace ecg_algorithm;
    if (setenv("GRASP_BOUNDARY_MODE", "capacity", 1) != 0) {
        check(false, "capacity tiering is configured");
        return;
    }
    const Fixture graph = declaredGraspGraph();
    for (bool records : {false, true}) {
        Options options;
        options.algorithm = Algorithm::SPMV;
        options.repetitions = 2;
        options.records = records;
        if (records) {
            options.mechanism = ecg_record::Mechanism::REPLACEMENT;
            options.record_base_policy = RecordBasePolicy::GRASP_PAPER;
        }
        cache_sim::CacheHierarchy cache(128, 2, 256, 2, 2048, 4, 64,
            cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::LRU, cache_sim::EvictionPolicy::GRASP);
        cache_sim::AlgorithmBackend backend(cache, options, 2048, true, false);
        ecg_algorithm::run(graph.view(true), options, backend);
        const auto& stats = cache.getL3Stats();
        const auto* level = cache.L3();
        uint64_t hits = 0, misses = 0, kernel_hits = 0, kernel_misses = 0;
        for (uint32_t region = 0; region < backend.graphContext().num_regions; ++region) {
            hits += level->propertyRegionHits(region);
            misses += level->propertyRegionMisses(region);
            kernel_hits += level->propertyRegionKernelHits(region);
            kernel_misses += level->propertyRegionKernelMisses(region);
        }
        const auto kernel = backend.kernelTraffic();
        check(hits == stats.prop_hits.load() && misses == stats.prop_misses.load() &&
              kernel_hits == kernel.llc_property_hits && kernel_misses == kernel.llc_property_misses,
              "per-region counts partition the property hits and misses, over the run and the kernel");
        const std::string json = cache.toJSON();
        check(json.find("\"property_registration\": {\"grasp_registration\":\"all\",") != std::string::npos &&
              json.find("\"property_regions\":[{\"name\":\"x\",\"grasp\":true,") != std::string::npos &&
              json.find("{\"name\":\"y\",\"grasp\":true,") != std::string::npos,
              "the receipt names each property region's counts and the registration it ran under");
    }
}

int main(int argc, char** argv) {
    using namespace ecg_algorithm;
    if (argc == 4 && std::strcmp(argv[1], kCensusRunFlag) == 0)
        return censusRosterRun(argv[2], argv[3]);
    testOneEdgeStreamPerEdge();
    testEdgeStreamAccounting();
    testPreparedPoptQueryReuse();
    testGraspReferenceConsumer();
    testFrontierProducerAndCounts();
    testFrontierVictimAndContext();
    testPoptConstantRanksPreserveOtherMechanics();
    testScopedGraspKeepsCacheState();
    testWindowTransportMatchesGrasp();
    testActiveWindowAssociation();
    testActiveWindowCodecAndCache();
    testProtectedWindowProbe();
    testWindowCheckedStorePublication();
    testWindowCandidateAttribution();
    testWindowPublishedWriteSurvival();
    testWindowObservationCallbacks();
    testPoptDirectMatrix();
    testUnboundRecordPreparation();
    testTraversalPreprocessing();
    testKernelCensusUnderTheRoster();
    testDeclaredGraspRegistration();
    testColdKernelEntry();
    testPropertyRegionAttribution();
    testPropertyRegionCounters();
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
