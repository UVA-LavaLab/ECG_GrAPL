#include "ecg_algorithm_main.h"
#include "cache_sim/graph_cache_context.h"
#include "ecg_window.h"

#include <cassert>
#include <memory>

namespace {

using Graph = CSRGraph<int32_t>;

struct Bytes {
    uint64_t reads = 0, writes = 0;
    void touch(uint64_t bytes, bool write) { (write ? writes : reads) += bytes; }
    template<class T>
    T read(const void* address, ecg_algorithm::MemoryKind, uint64_t, uint64_t, bool = true) {
        touch(sizeof(T), false);
        T value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }
};

// Fixture-only ownership prototype. Tags are opaque, not PASS_RANK predictions.
struct Prepared {
    enum class Mode { RAW, TAGGED, MATRIX };
    Graph graph;
    ecg_algorithm::GraphView view;
    popt_reref::FullMatrix matrix;
    Bytes preparation;
    uint64_t conversions = 0, matrix_builds = 0, next_base = 0, order = 0, queries = 0;
    bool borrowed = false, poisoned = false;

    Prepared(Graph&& source, Mode mode) : graph(std::move(source)) {
        view = {uint64_t(graph.num_nodes()), uint64_t(graph.num_edges_directed()), graph.directed(),
            graph.out_index_storage(), graph.out_neigh(0).begin(), nullptr, 4, true};
        if (view.vertices > 4096 || view.records > 65536 || graph.directed())
            throw std::invalid_argument("prepared qualification requires a tiny undirected graph");
        if (mode == Mode::TAGGED) {
            uint32_t maximum = 0;
            for (uint64_t index = 0; index < view.records; ++index)
                maximum = std::max(maximum, view.id(preparation, index));
            const auto bits = ecg_record::bitWidth(maximum);
            auto* words = graph.out_neigh(0).begin();
            for (uint64_t index = view.records; index-- > 0;) {
                const uint32_t id = view.id(preparation, index);
                const uint32_t tagged = id | 0x80000000u | (uint32_t(index + 1) << bits);
                preparation.touch(4, true);
                std::memcpy(words + index, &tagged, sizeof(tagged));
            }
            view.encoded_id_bits = bits;
            ++conversions;
        } else if (mode == Mode::MATRIX) {
            const uint32_t lines = uint32_t((view.vertices + 15) / 16);
            const auto observe = [&](const void*, uint64_t bytes, bool write) {
                preparation.touch(bytes, write);
            };
            matrix.configure(uint32_t(view.vertices), lines, uint64_t(lines) * 256, observe);
            for (uint32_t row = 0; row < view.vertices; ++row) {
                const auto bounds = view.row(preparation, row);
                for (uint64_t index = bounds.first; index < bounds.second; ++index)
                    matrix.reference(view.id(preparation, index) / 16, row, observe);
            }
            matrix.finish(observe);
            ++matrix_builds;
        }
    }
};

struct Borrow {
    std::shared_ptr<Prepared> owner;
    uint64_t base, limit;
    bool closed = false;

    explicit Borrow(std::shared_ptr<Prepared> prepared, uint64_t passes = 2)
        : owner(std::move(prepared)), base(0), limit(0) {
        uint64_t span = 0;
        if (!owner)
            throw std::invalid_argument("missing prepared owner");
        base = owner->next_base;
        if (owner->borrowed || owner->poisoned || !passes ||
            !ecg_record::checkedMultiply(owner->view.records + 1, passes, span) ||
            !ecg_record::checkedAdd(base, span, limit))
            throw std::logic_error("invalid prepared-query borrow");
        owner->borrowed = true;
        owner->next_base = limit;
    }
    Borrow(const Borrow&) = delete;
    Borrow& operator=(const Borrow&) = delete;
    ~Borrow() {
        if (!closed) {
            owner->poisoned = true;
            owner->borrowed = false;
        }
    }
    const ecg_algorithm::GraphView& graph() const {
        if (closed) throw std::logic_error("closed query view");
        return owner->view;
    }
    uint64_t observe() {
        if (closed || !ecg_record::checkedAdd(owner->order, 1, owner->order))
            throw std::logic_error("invalid query observation order");
        return owner->order;
    }
    bool finite(uint64_t deadline) const { return !closed && base < deadline && deadline <= limit; }
    bool terminal(uint64_t end) const { return !closed && end == limit; }
    void close(bool drained, bool detached) {
        if (closed || !drained || !detached) throw std::logic_error("unsafe query close");
        owner->borrowed = false;
        closed = true;
    }
};

struct QueryBackend : ecg_algorithm::PlainBackend {
    Borrow& borrow;
    cache_sim::GraphCacheContext context;
    const float* x = nullptr;
    uint64_t vertices = 0, x_writes = 0, x_reads = 0, matrix_lookups = 0;
    Bytes query;
    bool attached = false;

    explicit QueryBackend(Borrow& scoped) : borrow(scoped) {}
    void region(const char* name, const void* address, uint64_t count, uint8_t bytes, bool property) {
        if (std::strcmp(name, "x") == 0) {
            assert(property && bytes == 4 && reinterpret_cast<uint64_t>(address) % 64 == 0);
            x = static_cast<const float*>(address);
            vertices = count;
            context.topology.num_vertices = uint32_t(count);
            context.registerPropertyArray(address, uint32_t(count), 4, 1024);
        }
    }
    void memory(const void* address, uint64_t bytes, bool write) {
        query.touch(bytes, write);
        const uint64_t pointer = reinterpret_cast<uint64_t>(address);
        const uint64_t first = reinterpret_cast<uint64_t>(x);
        if (x && pointer >= first && pointer < first + vertices * 4)
            ++(write ? x_writes : x_reads);
    }
    void selectProperty(const ecg_algorithm::GraphView&, const void* base,
                        const ecg_record::PropertyDescriptor&) {
        assert(base == x && x_writes == vertices);
        if (borrow.owner->matrix_builds) {
            const auto& matrix = borrow.owner->matrix;
            context.compound_popt = true;
            context.regions[0].popt_line_offset = 0;
            context.initRereference(matrix.data(), matrix.lines(), 256, uint32_t(vertices), 64);
            context.setCurrentVertices(0, 0);
            assert(context.findNextRef(reinterpret_cast<uint64_t>(x)) == 16);
            ++matrix_lookups;
        }
        attached = true;
    }
    void finish(uint64_t actual) {
        assert(attached && x_writes == vertices && x_reads == actual);
        for (uint64_t vertex = 0; vertex < vertices; ++vertex)
            assert(x[vertex] == float(vertex + 1));
        context.rereference.matrix = nullptr;
        attached = false;
        borrow.close(true, !attached);
        ++borrow.owner->queries;
    }
};

template<class Function>
void rejects(Function operation) {
    bool rejected = false;
    try { operation(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
}

std::shared_ptr<Prepared> load(const char* path, Prepared::Mode mode) {
    Reader<int32_t> reader(path);
    return std::make_shared<Prepared>(reader.ReadSerializedGraph(true, 8 << 20), mode);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        ecg_algorithm::Options options;
        options.algorithm = ecg_algorithm::Algorithm::SPMV;
        options.repetitions = 2;
        options.evidence = options.capture_values = true;
        std::vector<float> expected(64);
        expected[0] = 50;
        expected[16] = 34;
        expected[32] = 18;
        uint64_t work_digest = 0, result_digest = 0;
        std::ostringstream cells;
        bool first = true;
        for (auto mode : {Prepared::Mode::RAW, Prepared::Mode::MATRIX, Prepared::Mode::TAGGED}) {
            auto prepared = load(argv[1], mode);
            assert(prepared->view.vertices == 64 && prepared->view.records == 6);
            const void* edge_storage = prepared->graph.out_neigh(0).begin();
            assert(prepared->view.columns == edge_storage);
            const Bytes before = prepared->preparation;
            const auto* matrix_storage = prepared->matrix_builds ? prepared->matrix.data() : nullptr;
            const uint64_t matrix_digest = prepared->matrix_builds ? prepared->matrix.digest() : 0;
            uint64_t previous_end = 0, previous_order = 0;
            for (unsigned query = 0; query < 2; ++query) {
                Borrow scoped(prepared);
                assert(scoped.base == previous_end);
                rejects([&] { Borrow overlapping(prepared); });
                rejects([&] { scoped.close(false, true); });
                rejects([&] { scoped.close(true, false); });
                assert(scoped.finite(scoped.base + 1) && scoped.terminal(scoped.limit));
                if (query) assert(!scoped.finite(previous_end) && !scoped.terminal(previous_end));
                ecg_record::LineMetadata line;
                const uint64_t order = scoped.observe();
                assert(order > previous_order);
                ecg_window::observe(line, order);
                if (query) {
                    assert(!ecg_window::apply(line, previous_order, previous_end));
                    assert(line.state == ecg_record::LineState::PENDING && line.value == order);
                }
                assert(ecg_window::apply(line, order, scoped.limit));
                previous_end = scoped.limit;
                previous_order = order;
                QueryBackend backend(scoped);
                const auto result = ecg_algorithm::run(scoped.graph(), options, backend);
                assert(result.values_f32 == expected && result.actual_records == 12 && result.passes == 2);
                if (!work_digest) {
                    work_digest = result.work_digest;
                    result_digest = result.result_digest;
                }
                assert(result.work_digest == work_digest && result.result_digest == result_digest);
                assert(backend.x_writes == 64 && backend.x_reads == 12);
                assert(backend.query.reads == 3208 && backend.query.writes == 768);
                assert(backend.matrix_lookups == (mode == Prepared::Mode::MATRIX ? 1 : 0));
                assert(!backend.attached && scoped.closed);
                rejects([&] { scoped.graph(); });
                assert(prepared->view.columns == edge_storage &&
                       prepared->preparation.reads == before.reads && prepared->preparation.writes == before.writes);
                if (matrix_storage)
                    assert(prepared->matrix.data() == matrix_storage && prepared->matrix.digest() == matrix_digest);
            }
            assert(prepared->queries == 2);
            assert(prepared->conversions == (mode == Prepared::Mode::TAGGED ? 1 : 0));
            assert(prepared->matrix_builds == (mode == Prepared::Mode::MATRIX ? 1 : 0));
            if (mode == Prepared::Mode::TAGGED) {
                assert(before.reads == 48 && before.writes == 24);
                auto raw = prepared->view;
                raw.encoded_id_bits = 0;
                Bytes access;
                rejects([&] { raw.id(access, 0); });
                auto bad = prepared->view;
                bad.encoded_id_bits = 32;
                rejects([&] { ecg_algorithm::validateGraph(bad, access, false); });
                bad = prepared->view;
                bad.weights = bad.columns;
                rejects([&] { ecg_algorithm::validateGraph(bad, access, false); });
                bad = prepared->view;
                bad.directed = true;
                rejects([&] { bad.incoming(); });
                ecg_algorithm::PlainBackend plain;
                auto unsupported = options;
                unsupported.algorithm = ecg_algorithm::Algorithm::BFS;
                rejects([&] { ecg_algorithm::run(prepared->view, unsupported, plain); });
            }
            if (!first) cells << ',';
            first = false;
            cells << "{\"kind\":\"" << (mode == Prepared::Mode::RAW ? "raw-owner" :
                mode == Prepared::Mode::MATRIX ? "matrix-owner" : "opaque-tag-owner")
                  << "\",\"queries\":2,\"edge_bytes\":24,\"extra_edge_buffers\":0,"
                  << "\"query_read_bytes\":3208,\"query_write_bytes\":768,"
                  << "\"matrix_binding_probes_per_query\":" << (mode == Prepared::Mode::MATRIX ? 1 : 0) << ','
                  << "\"conversions\":" << prepared->conversions << ",\"matrix_builds\":" << prepared->matrix_builds
                  << ",\"matrix_bytes\":" << prepared->matrix.bytes()
                  << ",\"preparation_read_bytes\":" << before.reads << ",\"preparation_write_bytes\":" << before.writes << '}';
            std::weak_ptr<Prepared> lifetime = prepared;
            {
                Borrow retained(prepared);
                prepared.reset();
                assert(!lifetime.expired() && retained.graph().columns == edge_storage);
                retained.close(true, true);
            }
            assert(lifetime.expired());
        }
        auto abandoned = load(argv[1], Prepared::Mode::RAW);
        { Borrow unfinished(abandoned); }
        assert(abandoned->poisoned);
        rejects([&] { Borrow reuse_abandoned(abandoned); });
        auto overflow = load(argv[1], Prepared::Mode::RAW);
        overflow->next_base = UINT64_MAX - 6;
        rejects([&] { Borrow wrapped(overflow); });
        rejects([&] { Borrow missing(nullptr); });
        std::cout << "{\"schema\":\"ecg.prepared-spmv-qualification.v1\","
                  << "\"scope\":\"fixture-only-ownership-not-cache-performance\","
                  << "\"pass_rank_producer\":false,\"pass_rank_policy\":false,"
                  << "\"opaque_tags\":true,\"storage_reused\":true,\"stale_query_rejected\":true,"
                  << "\"fresh_x_writes_per_query\":64,\"cells\":[" << cells.str() << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "prepared qualification failed: " << error.what() << '\n';
        return 1;
    }
}
