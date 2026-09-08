#include "ecg_algorithm_sideband.h"
#include "gem5_sim/gem5_harness.h"

namespace {

class AlgorithmBackend {
  public:
    static constexpr bool models_memory = false;
    explicit AlgorithmBackend(const ecg_algorithm::Options& options)
        : options_(options), sideband_("GEM5_GRAPHBREW_CTX") {}
    void start(const ecg_algorithm::GraphView& graph) {
        sideband_.graph(graph);
        GEM5_RESET_STATS();
        GEM5_WORK_BEGIN(GEM5_WORK_COMPUTE);
    }
    void memory(const void*, uint64_t, bool) {}
    void region(const char* name, const void* base, uint64_t count, uint8_t bytes, bool property) {
        sideband_.region(name, base, count, bytes, property);
    }
    void selectProperty(const ecg_algorithm::GraphView& graph, const void*,
                        const ecg_record::PropertyDescriptor&) {
        sideband_.activeGraph(graph);
        if (!options_.records)
            sideband_.publish();
    }
    void bind(const ecg_record::NativeConfiguration& configuration, const ecg_record::RecordStream& stream) {
        sideband_.publish(&stream);
        context_.bind(configuration, stream);
        active_ = true;
    }
    void beginPass(bool has_next) { context_.beginPass(has_next); }
    void closePass() { context_.closePass(); }
    uint64_t recordLoad(uint64_t index) { return context_.loadRecord(index); }
    template<class T>
    T property(uint64_t index, uint64_t word, const T* base) {
        return context_.loadProperty(index, word, base);
    }
    void drain() {
        if (active_)
            context_.drain();
    }
    void finish(uint64_t actual_records) {
        if (active_)
            context_.finish(actual_records);
        GEM5_WORK_END(GEM5_WORK_COMPUTE);
        GEM5_DUMP_STATS();
    }

  private:
    const ecg_algorithm::Options& options_;
    ecg_algorithm::AlgorithmSideband sideband_;
    Gem5ManagedRecordContext context_;
    bool active_ = false;
};

} // namespace

int main(int argc, char** argv) {
    return ecg_algorithm::applicationMain(argc, argv,
        [](const ecg_algorithm::GraphView& graph, const ecg_algorithm::CommandLine& command) {
            AlgorithmBackend backend(command.options);
            const auto result = ecg_algorithm::run(graph, command.options, backend);
#if defined(__riscv) && __riscv_xlen == 64 && !defined(NO_M5OPS)
            const char* name = "gem5";
#else
            const char* name = "gem5-host-preflight";
#endif
            ecg_algorithm::writeDetailedResult(name, result, command);
            return 0;
        });
}
