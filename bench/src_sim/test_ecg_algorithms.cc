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
    for (uint8_t width : {uint8_t{4}, uint8_t{8}}) {
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
        cache_sim::WindowRuntime runtime(actual, stream, 64, base, false, 6);
        runtime.beginPass();
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
        runtime.finish(graph.edges.size());
        check(actual.getMemoryAccesses() == reference.getMemoryAccesses() &&
              actual.getWritebackTraffic() == reference.getWritebackTraffic() &&
              actual.getL3Stats().hits.load() == reference.getL3Stats().hits.load() &&
              actual.windowStats().overrides == 0,
              "window transport has exact GRASP behavior on the same real encoded data stream");
    }
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

} // namespace

int main() {
    using namespace ecg_algorithm;
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
