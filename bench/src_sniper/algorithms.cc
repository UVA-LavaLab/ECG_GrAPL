#include "ecg_algorithm_sideband.h"
#include "ecg_record_guest.h"

namespace {

class AlgorithmBackend {
  public:
    static constexpr bool models_memory = false;
    explicit AlgorithmBackend(const ecg_algorithm::Options& options)
        : options_(options), sideband_("SNIPER_GRAPHBREW_CTX") {}
    void start(const ecg_algorithm::GraphView& graph) {
        sideband_.graph(graph);
        SNIPER_ROI_BEGIN();
    }
    void memory(const void*, uint64_t, bool) {}
    void region(const char* name, const void* base, uint64_t count, uint8_t bytes, bool property) {
        sideband_.region(name, base, count, bytes, property);
    }
    void selectProperty(const ecg_algorithm::GraphView& graph, const void*,
                        const ecg_record::PropertyDescriptor&) {
        sideband_.activeGraph(graph);
        if (!options_.records) {
            sideband_.publish();
            graphbrew_sniper::notify_context_ready();
        }
    }
    void bind(const ecg_record::NativeConfiguration& configuration, const ecg_record::RecordStream& stream) {
        sideband_.publish(&stream);
        graphbrew_sniper::notify_context_ready();
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
        SNIPER_ROI_END();
    }

  private:
    const ecg_algorithm::Options& options_;
    ecg_algorithm::AlgorithmSideband sideband_;
    graphbrew_sniper::EcgRecordContext context_;
    bool active_ = false;
};

} // namespace

int main(int argc, char** argv) {
    return ecg_algorithm::applicationMain(argc, argv,
        [](const ecg_algorithm::GraphView& graph, const ecg_algorithm::CommandLine& command) {
            AlgorithmBackend backend(command.options);
            const auto result = ecg_algorithm::run(graph, command.options, backend);
            ecg_algorithm::writeDetailedResult("sniper", result, command);
            return 0;
        });
}
