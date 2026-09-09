#ifndef GRAPHBREW_CACHE_SIM_ECG_WINDOW_OBSERVER_H
#define GRAPHBREW_CACHE_SIM_ECG_WINDOW_OBSERVER_H

#include <array>
#include <iomanip>
#include <limits>
#include <vector>

#include "cache_sim.h"
#include "../ecg_algorithms.h"

namespace cache_sim {
namespace window_observation {

struct Profile {
    uint64_t vertices, cohort_rows = 8, bin_rows = 1, bins = 0;

    explicit Profile(uint64_t count, uint64_t fixture_cohort = 0) : vertices(count) {
        if (!count || count > UINT32_MAX)
            throw std::invalid_argument("window-observer-source-domain");
        while (cohort_rows < (count + 255) / 256)
            cohort_rows *= 2;
        if (fixture_cohort)
            cohort_rows = fixture_cohort;
        if (cohort_rows < 8 || (cohort_rows & (cohort_rows - 1)))
            throw std::invalid_argument("window-observer-cohort");
        bin_rows = cohort_rows / 8;
        bins = (count + bin_rows - 1) / bin_rows;
    }

    uint16_t reverse(uint64_t& entry, uint32_t row) const {
        constexpr uint64_t valid = uint64_t{1} << 48;
        if (row >= vertices)
            throw std::invalid_argument("window-observer-row");
        uint16_t token = 0;
        uint64_t endpoint = (row % cohort_rows) / bin_rows, strength = 0;
        if (entry & valid) {
            const uint32_t previous = static_cast<uint32_t>(entry);
            if (row > previous)
                throw std::logic_error("window-observer-reverse-order");
            if (row == previous)
                return static_cast<uint16_t>((entry >> 38) & 1023);
            const uint64_t gap = previous / cohort_rows - row / cohort_rows;
            if (gap <= 7)
                token = static_cast<uint16_t>(512 | (gap << 6) |
                    (((entry >> 35) & 7) << 3) | ((entry >> 32) & 7));
            if (gap == 0) {
                endpoint = (entry >> 32) & 7;
                strength = std::min<uint64_t>(7, ((entry >> 35) & 7) + 1);
            }
        }
        entry = valid | row | (endpoint << 32) | (strength << 35) | (uint64_t(token) << 38);
        return token;
    }

    uint64_t decode(uint16_t token, uint64_t row, uint64_t pass_base) const {
        if (!token)
            return 0;
        if (token > 1023 || !(token & 512) || row >= vertices)
            throw std::invalid_argument("window-observer-token");
        const uint64_t selected = row / cohort_rows + ((token >> 6) & 7);
        const uint64_t local_end = selected * 8 + (token & 7) + 1;
        uint64_t end = 0;
        if (selected * 8 >= bins || local_end > bins || local_end <= row / bin_rows ||
            !ecg_record::checkedAdd(pass_base, local_end, end) || end > (UINT64_MAX >> 3))
            throw std::invalid_argument("window-observer-endpoint");
        return (end << 3) | ((token >> 3) & 7);
    }

    uint64_t distance(uint64_t payload, uint64_t watermark, uint64_t pass_base) const {
        const uint64_t end = payload >> 3;
        if (!end || end <= watermark || end <= pass_base || end - pass_base > bins)
            throw std::logic_error("window-observer-invalid-live-window");
        const uint64_t start = pass_base + ((end - 1 - pass_base) / 8) * 8;
        return start > watermark ? start - watermark : 0;
    }
};

class Observer final : public CacheObservationSink {
  public:
    static constexpr uint64_t kSamplePeriod = 256;
    static constexpr uint64_t kHorizon = 131072;
    static constexpr std::size_t kTrials = 256, kPasses = 128, kQueue = 16;
    static constexpr uint64_t kLatency = 8;

    Observer(const ecg_algorithm::GraphView& graph, uint64_t property_base,
             std::size_t sets, std::size_t ways, bool windows, uint64_t maximum_bytes)
        : profile_(graph.vertices), graph_(graph), property_base_(property_base),
          sets_(sets), ways_(ways), windows_(windows) {
        if (!sets || !ways || ways > 64 || property_base % 64 ||
            !ecg_record::checkedAdd(property_base, graph.vertices * 4, property_end_))
            throw std::invalid_argument("window-observer-property-or-cache-domain");
        const uint64_t lines = (graph.vertices + 15) / 16;
        uint64_t tokens = 0, shadows = 0, heads = 0, total = 0;
        if (!ecg_record::checkedMultiply(graph.records, sizeof(uint16_t), tokens) ||
            !ecg_record::checkedMultiply(sets, ways, shadows) ||
            !ecg_record::checkedMultiply(shadows, sizeof(Shadow), shadows) ||
            !ecg_record::checkedMultiply(sets, sizeof(int32_t), heads) ||
            !ecg_record::checkedAdd(tokens, shadows, total) ||
            !ecg_record::checkedAdd(total, heads, total) ||
            !ecg_record::checkedAdd(total, lines * 8 + sizeof(*this), total))
            throw std::length_error("window-observer-storage-overflow");
        reserved_bytes_ = windows ? total : sizeof(*this);
        if (reserved_bytes_ > maximum_bytes)
            throw std::length_error("window-observer-storage-limit");
        if (windows_) {
            buildTokens(lines);
            shadow_.resize(sets * ways);
            trial_heads_.assign(sets, -1);
            if (tokens_.capacity() * sizeof(uint16_t) + shadow_.capacity() * sizeof(Shadow) +
                trial_heads_.capacity() * sizeof(int32_t) + sizeof(*this) > reserved_bytes_)
                throw std::length_error("window-observer-allocation-exceeds-reservation");
        }
    }

    void initialSet(std::size_t set, const CacheLine* lines, std::size_t count) override {
        if (set >= sets_ || count != ways_)
            throw std::logic_error("window-observer-cache-geometry");
        if (windows_)
            for (std::size_t way = 0; way < count; ++way)
                shadow_[set * ways_ + way].line = lines[way].valid ? lines[way].line_addr : UINT64_MAX;
    }

    void insertion(std::size_t set, const CacheLine* lines, std::size_t count,
                   std::size_t victim, uint64_t incoming) override {
        if (!in_access_ || set >= sets_ || count != ways_ || victim >= count)
            throw std::logic_error("window-observer-unowned-insertion");
        ++fills_;
        victim_digest_.add(set);
        victim_digest_.add(victim);
        victim_digest_.add(incoming);
        victim_digest_.add(lines[victim].valid ? lines[victim].line_addr : UINT64_MAX);
        victim_digest_.add(lines[victim].rrpv);
        if (lines[victim].valid) {
            ++evictions_;
            if (open_) {
                ++pass_evictions_;
                ++passes_[pass_count_ - 1].evictions;
                if (windows_ && pass_evictions_ % kSamplePeriod == 0) {
                    sample(set, lines, victim, false, immediate_, passes_[pass_count_ - 1].immediate);
                    sample(set, lines, victim, true, delivered_, passes_[pass_count_ - 1].delivered);
                }
            }
        }
        if (windows_)
            shadow_[set * ways_ + victim] = Shadow{incoming};
    }

    void beginPass() {
        if (open_ || expected_ || pass_count_ == kPasses)
            throw std::logic_error("window-observer-pass-limit-or-order");
        pass_base_ = pass_count_ * profile_.bins;
        watermark_ = pass_base_;
        source_ = UINT64_MAX;
        ++pass_count_;
        open_ = true;
        marker();
    }

    void visitVertex(uint64_t source) {
        if (!open_ || expected_ || source >= profile_.vertices ||
            (source_ != UINT64_MAX && source <= source_))
            throw std::logic_error("window-observer-nonmonotone-source");
        const uint64_t next = pass_base_ + source / profile_.bin_rows;
        if (source_ == UINT64_MAX || next != watermark_) {
            marker();
            watermark_ = next;
        }
        source_ = source;
        ++passes_[pass_count_ - 1].vertices;
        if (windows_) {
            // Diagnostic association checks are excluded from the active memory trace.
            BuildAccess access{*this};
            const auto row = graph_.row(access, source);
            row_first_ = row.first;
            row_last_ = row.second;
        }
    }

    void designated(uint64_t index, uint32_t destination) {
        if (!open_ || source_ == UINT64_MAX || expected_ || destination >= profile_.vertices ||
            (windows_ && (index < row_first_ || index >= row_last_)))
            throw std::logic_error("window-observer-invalid-record-association");
        expected_ = true;
        expected_address_ = property_base_ + uint64_t(destination) * 4;
        expected_value_ = windows_ ? profile_.decode(tokens_.at(index), source_, pass_base_) : 0;
        ++designated_;
        ++passes_[pass_count_ - 1].designated;
        if (expected_value_)
            ++executed_known_;
    }

    void beforeAccess(uint64_t address, bool write) {
        if (in_access_ || (expected_ && (write || address != expected_address_)))
            throw std::logic_error("window-observer-memory-association");
        if (!ecg_record::checkedAdd(requests_, 1, requests_))
            throw std::overflow_error("window-observer-request-order-overflow");
        in_access_ = true;
        tick();
        if (windows_ && requests_ % 1024 == 0)
            expireTrials(false);
        current_property_ = isProperty(address);
        if (windows_ && current_property_) {
            if (!ecg_record::checkedAdd(order_, 1, order_))
                throw std::overflow_error("window-observer-event-order-overflow");
            if (!expected_) {
                if (write) ++ordinary_writes_;
                else ++ordinary_reads_;
            }
            if (auto* line = resident(address)) {
                line->order = order_;
                line->delayed_state = expected_ ? State::PENDING : State::INVALIDATED;
                if (!expected_) {
                    line->immediate_state = State::INVALIDATED;
                    line->immediate = 0;
                }
            }
        }
    }

    void afterAccess(uint64_t address, bool write, bool at_llc, bool memory_miss) {
        if (!in_access_)
            throw std::logic_error("window-observer-missing-access");
        demand_digest_.add(address);
        demand_digest_.add(uint64_t(write) | (uint64_t(at_llc) << 1) | (uint64_t(memory_miss) << 2));
        if (windows_ && current_property_) {
            if (expected_) {
                if (auto* line = resident(address)) {
                    line->order = order_;
                    line->immediate = expected_value_;
                    line->immediate_state = expected_value_ ? State::LIVE : State::UNKNOWN;
                    line->delayed_state = State::PENDING;
                } else {
                    ++nonresident_observations_;
                }
            } else if (auto* line = resident(address)) {
                line->order = order_;
                line->immediate_state = line->delayed_state = State::INVALIDATED;
            }
            enqueue(address & ~uint64_t{63}, expected_ ? expected_value_ : 0, !expected_);
            if (!write)
                observeReuse(address & ~uint64_t{63}, at_llc, memory_miss);
        }
        expected_ = false;
        in_access_ = false;
    }

    void endPass() {
        if (!open_ || expected_ || in_access_)
            throw std::logic_error("window-observer-invalid-pass-close");
        marker();
        watermark_ = pass_base_ + profile_.bins;
        if (windows_)
            expireTrials(true);
        open_ = false;
    }

    void finish(uint64_t actual_records) {
        if (open_ || expected_ || in_access_ || actual_records != designated_)
            throw std::logic_error("window-observer-incomplete-work");
        drain();
        if (active_trials_)
            throw std::logic_error("window-observer-uncensored-trials");
        if (enqueued_ != dispatched_ || dispatched_ != applied_ + stale_ + absent_ ||
            trials_started_ != base_first_ + alternative_first_ + censored_horizon_ + censored_pass_ ||
            trials_started_ + trials_dropped_ != delivered_.overrides)
            throw std::logic_error("window-observer-incomplete-accounting");
    }

    void write(std::ostream& output) const {
        output << "{\"schema\":\"ecg.window-eviction-observer.v1\",\"mode\":\""
               << (windows_ ? "window" : "control")
               << "\",\"active_policy_changed\":false,\"diagnostic_costs_in_cache_counters\":false,"
               << "\"delivery_model\":\"uncoalesced-eight-access-steps-serialized-markers\","
               << "\"sample_period\":" << kSamplePeriod << ",\"trial_capacity\":" << kTrials
               << ",\"trial_horizon_requests\":" << kHorizon << ",\"latency_steps\":" << kLatency
               << ",\"cohort_rows\":" << profile_.cohort_rows << ",\"bin_rows\":" << profile_.bin_rows
               << ",\"bins_per_pass\":" << profile_.bins << ",\"reserved_bytes\":" << reserved_bytes_
               << ",\"annotation_bytes\":" << tokens_.capacity() * sizeof(uint16_t)
               << ",\"shadow_bytes\":" << shadow_.capacity() * sizeof(Shadow)
               << ",\"constructed_records\":" << tokens_.size()
               << ",\"constructed_known\":" << constructed_known_
               << ",\"executed_known\":" << executed_known_ << ",\"passes\":" << pass_count_
               << ",\"designated_reads\":" << designated_ << ",\"memory_requests\":" << requests_
               << ",\"llc_fills\":" << fills_ << ",\"llc_evictions\":" << evictions_
               << ",\"pass_evictions\":" << pass_evictions_ << ",\"markers\":" << markers_
               << ",\"marker_control_bytes\":" << markers_ * 48
               << ",\"shadow_steps\":" << step_ << ",\"shadow_drain_steps\":" << drain_steps_
               << ",\"queue_enqueued\":" << enqueued_ << ",\"queue_delivered\":" << dispatched_
               << ",\"queue_applied\":" << applied_ << ",\"queue_stale\":" << stale_
               << ",\"queue_absent\":" << absent_ << ",\"queue_pending\":" << queued_
               << ",\"queue_peak\":" << queue_peak_ << ",\"ordinary_reads\":" << ordinary_reads_
               << ",\"ordinary_writes\":" << ordinary_writes_
               << ",\"nonresident_observations\":" << nonresident_observations_
               << ",\"diagnostic_graph_read_bytes\":" << graph_read_bytes_
               << ",\"sample_state_order\":[\"unseen\",\"unknown\",\"pending\",\"invalidated\","
               << "\"expired\",\"old-pass\",\"live\",\"non-property\"],\"demand_digest\":";
        const auto hash = [&](uint64_t value) {
            const auto flags = output.flags();
            const auto fill = output.fill();
            output << '"' << std::hex << std::setfill('0') << std::setw(16) << value << '"';
            output.flags(flags);
            output.fill(fill);
        };
        hash(demand_digest_.value());
        output << ",\"victim_digest\":";
        hash(victim_digest_.value());
        output << ",\"immediate\":";
        immediate_.write(output);
        output << ",\"delivered\":";
        delivered_.write(output);
        output << ",\"trials\":{\"started\":" << trials_started_ << ",\"capacity_dropped\":" << trials_dropped_
               << ",\"base_first\":" << base_first_ << ",\"alternative_first\":" << alternative_first_
               << ",\"base_first_llc\":" << base_first_llc_ << ",\"alternative_first_llc\":" << alternative_first_llc_
               << ",\"base_first_memory\":" << base_first_memory_
               << ",\"alternative_first_memory\":" << alternative_first_memory_
               << ",\"censored_horizon\":" << censored_horizon_ << ",\"censored_pass\":" << censored_pass_
               << ",\"peak_pending\":" << trials_peak_ << "},\"per_pass\":[";
        for (std::size_t pass = 0; pass < pass_count_; ++pass) {
            if (pass) output << ',';
            const auto& stats = passes_[pass];
            output << "{\"pass\":" << pass << ",\"vertices\":" << stats.vertices
                   << ",\"designated_reads\":" << stats.designated << ",\"evictions\":" << stats.evictions
                   << ",\"immediate\":";
            stats.immediate.write(output);
            output << ",\"delivered\":";
            stats.delivered.write(output);
            output << '}';
        }
        output << "]}";
    }

  private:
    enum class State : uint8_t { UNSEEN, UNKNOWN, PENDING, INVALIDATED, EXPIRED, OLD_PASS, LIVE, NON_PROPERTY };
    struct Shadow {
        uint64_t line = UINT64_MAX, immediate = 0, delayed = 0, order = 0;
        State immediate_state = State::UNSEEN, delayed_state = State::UNSEEN;
    };
    struct Samples {
        uint64_t count = 0, eligible = 0, live_eligible = 0, overrides = 0;
        std::array<uint64_t, 8> base_states{};
        void write(std::ostream& output) const {
            output << "{\"samples\":" << count << ",\"eligible_candidates\":" << eligible
                   << ",\"live_eligible_candidates\":" << live_eligible
                   << ",\"hypothetical_overrides\":" << overrides << ",\"base_states\":[";
            for (std::size_t index = 0; index < base_states.size(); ++index) {
                if (index) output << ',';
                output << base_states[index];
            }
            output << "]}";
        }
    };
    struct PassStats {
        uint64_t vertices = 0, designated = 0, evictions = 0;
        Samples immediate, delivered;
    };
    struct Update {
        uint64_t line = 0, value = 0, order = 0, ready = 0;
        bool invalidate = false;
    };
    struct Trial {
        uint64_t base = 0, alternative = 0, start = 0, expires = 0;
        int32_t next = -1;
        bool active = false;
    };
    struct BuildAccess {
        Observer& observer;
        template<class T>
        T read(const void* address, ecg_algorithm::MemoryKind, uint64_t, uint64_t, bool = true) {
            observer.graph_read_bytes_ += sizeof(T);
            T value;
            std::memcpy(&value, address, sizeof(T));
            return value;
        }
    };

    void buildTokens(uint64_t lines) {
        if (graph_.records > tokens_.max_size() || lines > std::vector<uint64_t>{}.max_size())
            throw std::length_error("window-observer-allocation-domain");
        tokens_.resize(static_cast<std::size_t>(graph_.records));
        std::vector<uint64_t> state(static_cast<std::size_t>(lines));
        BuildAccess access{*this};
        for (uint64_t source = graph_.vertices; source-- > 0;) {
            const auto row = graph_.row(access, source);
            for (uint64_t index = row.second; index-- > row.first;) {
                const uint32_t id = graph_.id(access, index);
                const uint16_t token = profile_.reverse(state[id / 16], static_cast<uint32_t>(source));
                tokens_[index] = token;
                if (token) ++constructed_known_;
            }
        }
    }

    bool isProperty(uint64_t address) const {
        return address >= property_base_ && address < property_end_;
    }
    Shadow* resident(uint64_t address) {
        const uint64_t line = address & ~uint64_t{63};
        const std::size_t start = ((line / 64) % sets_) * ways_;
        for (std::size_t way = 0; way < ways_; ++way)
            if (shadow_[start + way].line == line)
                return &shadow_[start + way];
        return nullptr;
    }
    State state(const Shadow& line, bool delayed) const {
        if (!isProperty(line.line))
            return State::NON_PROPERTY;
        const State raw = delayed ? line.delayed_state : line.immediate_state;
        if (raw != State::LIVE)
            return raw;
        const uint64_t end = (delayed ? line.delayed : line.immediate) >> 3;
        if (end <= pass_base_ || end - pass_base_ > profile_.bins)
            return State::OLD_PASS;
        return end <= watermark_ ? State::EXPIRED : State::LIVE;
    }
    void tick() {
        if (!ecg_record::checkedAdd(step_, 1, step_))
            throw std::overflow_error("window-observer-step-overflow");
        if (!queued_ || queue_[queue_head_].ready > step_)
            return;
        const auto update = queue_[queue_head_];
        queue_head_ = (queue_head_ + 1) % kQueue;
        --queued_;
        ++dispatched_;
        auto* line = resident(update.line);
        if (!line) {
            ++absent_;
        } else if (line->order != update.order) {
            ++stale_;
        } else {
            line->delayed = update.value;
            line->delayed_state = update.invalidate ? State::INVALIDATED :
                update.value ? State::LIVE : State::UNKNOWN;
            ++applied_;
        }
    }
    void enqueue(uint64_t line, uint64_t value, bool invalidate) {
        if (queued_ == kQueue || step_ > UINT64_MAX - kLatency)
            throw std::length_error("window-observer-required-update-overflow");
        queue_[(queue_head_ + queued_) % kQueue] = {line, value, order_, step_ + kLatency, invalidate};
        ++queued_;
        ++enqueued_;
        queue_peak_ = std::max(queue_peak_, queued_);
    }
    void drain() {
        while (queued_) {
            tick();
            ++drain_steps_;
        }
    }
    void marker() {
        drain();
        for (uint64_t step = 0; step < kLatency; ++step)
            tick();
        ++markers_;
    }

    void sample(std::size_t set, const CacheLine* lines, std::size_t base, bool delayed,
                Samples& total, Samples& pass) {
        if (lines[base].rrpv < 7)
            throw std::logic_error("window-observer-non-GRASP-eviction-candidate");
        const auto add = [&](uint64_t Samples::* field, uint64_t amount) {
            total.*field += amount;
            pass.*field += amount;
        };
        add(&Samples::count, 1);
        const Shadow* entries = shadow_.data() + set * ways_;
        const State base_state = state(entries[base], delayed);
        ++total.base_states[static_cast<std::size_t>(base_state)];
        ++pass.base_states[static_cast<std::size_t>(base_state)];
        std::size_t proposed = base;
        for (std::size_t way = 0; way < ways_; ++way) {
            if (!lines[way].valid || entries[way].line != lines[way].line_addr)
                throw std::logic_error("window-observer-residency-mismatch");
            if (lines[way].rrpv < 7 || !isProperty(lines[way].line_addr))
                continue;
            add(&Samples::eligible, 1);
            if (state(entries[way], delayed) != State::LIVE)
                continue;
            add(&Samples::live_eligible, 1);
            if (base_state != State::LIVE)
                continue;
            const uint64_t a = delayed ? entries[proposed].delayed : entries[proposed].immediate;
            const uint64_t b = delayed ? entries[way].delayed : entries[way].immediate;
            const uint64_t da = profile_.distance(a, watermark_, pass_base_);
            const uint64_t db = profile_.distance(b, watermark_, pass_base_);
            if (db > da || (db == da && (b & 7) < (a & 7)))
                proposed = way;
        }
        if (proposed != base) {
            add(&Samples::overrides, 1);
            if (delayed)
                startTrial(set, entries[base].line, entries[proposed].line);
        }
    }

    void startTrial(std::size_t set, uint64_t base, uint64_t alternative) {
        if (requests_ > UINT64_MAX - kHorizon)
            throw std::overflow_error("window-observer-trial-horizon-overflow");
        if (active_trials_ == kTrials)
            expireTrials(false);
        if (active_trials_ == kTrials) {
            ++trials_dropped_;
            return;
        }
        for (std::size_t index = 0; index < trials_.size(); ++index) {
            auto& trial = trials_[index];
            if (trial.active)
                continue;
            trial = {base, alternative, requests_, requests_ + kHorizon, trial_heads_[set], true};
            trial_heads_[set] = static_cast<int32_t>(index);
            ++trials_started_;
            ++active_trials_;
            trials_peak_ = std::max(trials_peak_, active_trials_);
            return;
        }
        throw std::logic_error("window-observer-trial-accounting");
    }
    void removeTrial(std::size_t index) {
        auto& trial = trials_[index];
        int32_t* link = &trial_heads_[(trial.base / 64) % sets_];
        while (*link >= 0 && static_cast<std::size_t>(*link) != index)
            link = &trials_[*link].next;
        if (*link < 0 || !trial.active)
            throw std::logic_error("window-observer-trial-link");
        *link = trial.next;
        trial.active = false;
        --active_trials_;
    }
    void expireTrials(bool close) {
        for (std::size_t index = 0; index < trials_.size(); ++index) {
            const auto& trial = trials_[index];
            if (trial.active && (close || requests_ >= trial.expires)) {
                if (requests_ >= trial.expires) ++censored_horizon_;
                else ++censored_pass_;
                removeTrial(index);
            }
        }
    }
    void observeReuse(uint64_t line, bool at_llc, bool memory_miss) {
        int32_t index = trial_heads_[(line / 64) % sets_];
        while (index >= 0) {
            const auto trial = trials_[index];
            const int32_t next = trial.next;
            if (requests_ >= trial.expires) {
                ++censored_horizon_;
                removeTrial(static_cast<std::size_t>(index));
            } else if (requests_ > trial.start && (line == trial.base || line == trial.alternative)) {
                if (line == trial.base) {
                    ++base_first_;
                    base_first_llc_ += at_llc;
                    base_first_memory_ += memory_miss;
                } else {
                    ++alternative_first_;
                    alternative_first_llc_ += at_llc;
                    alternative_first_memory_ += memory_miss;
                }
                removeTrial(static_cast<std::size_t>(index));
            }
            index = next;
        }
    }

    Profile profile_;
    ecg_algorithm::GraphView graph_;
    uint64_t property_base_, property_end_ = 0;
    std::size_t sets_, ways_;
    bool windows_;
    std::vector<uint16_t> tokens_;
    std::vector<Shadow> shadow_;
    std::vector<int32_t> trial_heads_;
    std::array<PassStats, kPasses> passes_{};
    std::array<Update, kQueue> queue_{};
    std::array<Trial, kTrials> trials_{};
    Samples immediate_, delivered_;
    ecg_record::StreamDigest demand_digest_, victim_digest_;
    uint64_t reserved_bytes_ = 0, constructed_known_ = 0, executed_known_ = 0;
    uint64_t graph_read_bytes_ = 0, requests_ = 0, fills_ = 0, evictions_ = 0, pass_evictions_ = 0;
    uint64_t pass_count_ = 0, pass_base_ = 0, watermark_ = 0, source_ = UINT64_MAX;
    uint64_t row_first_ = 0, row_last_ = 0, expected_address_ = 0, expected_value_ = 0;
    uint64_t designated_ = 0, ordinary_reads_ = 0, ordinary_writes_ = 0, nonresident_observations_ = 0;
    uint64_t step_ = 0, order_ = 0, markers_ = 0, drain_steps_ = 0;
    uint64_t enqueued_ = 0, dispatched_ = 0, applied_ = 0, stale_ = 0, absent_ = 0;
    std::size_t queued_ = 0, queue_head_ = 0, queue_peak_ = 0;
    uint64_t trials_started_ = 0, trials_dropped_ = 0, base_first_ = 0, alternative_first_ = 0;
    uint64_t base_first_llc_ = 0, alternative_first_llc_ = 0;
    uint64_t base_first_memory_ = 0, alternative_first_memory_ = 0;
    uint64_t censored_horizon_ = 0, censored_pass_ = 0;
    std::size_t active_trials_ = 0, trials_peak_ = 0;
    bool open_ = false, expected_ = false, in_access_ = false, current_property_ = false;
};

}  // namespace window_observation
}  // namespace cache_sim

#endif
