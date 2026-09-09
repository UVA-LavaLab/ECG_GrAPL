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
    static constexpr std::size_t kImmediate = 0, kDelivered = 1;
    static constexpr std::size_t kPreservedImmediate = 2, kPreservedDelivered = 3;
    static constexpr std::size_t kForwardedDelivered = 4;
    static constexpr std::size_t kViews = 5, kTrialViews = 3;
    inline static constexpr std::array<const char*, kViews> kViewNames{
        "immediate", "delivered", "preserved_immediate", "preserved_delivered", "forwarded_delivered"};
    inline static constexpr std::array<std::size_t, kTrialViews> kTrialViewIds{
        kDelivered, kPreservedDelivered, kForwardedDelivered};
    inline static constexpr std::array<const char*, kTrialViews> kTrialNames{
        "trials", "preserved_trials", "forwarded_trials"};

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
            !ecg_record::checkedMultiply(sets, sizeof(int32_t) * kTrialViews, heads) ||
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
            trial_heads_.assign(sets * kTrialViews, -1);
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
                    for (std::size_t view = 0; view < kViews; ++view)
                        sample(set, lines, victim, view, samples_[view], passes_[pass_count_ - 1].samples[view]);
                }
            }
        }
        if (windows_)
            shadow_[set * ways_ + victim] = Shadow{incoming};
    }

    void beginPass() {
        if (open_ || expected_ || store_authorized_ || pass_count_ == kPasses)
            throw std::logic_error("window-observer-pass-limit-or-order");
        last_read_.valid = false;
        pass_base_ = pass_count_ * profile_.bins;
        watermark_ = pass_base_;
        source_ = UINT64_MAX;
        ++pass_count_;
        open_ = true;
        marker();
    }

    void visitVertex(uint64_t source) {
        if (!open_ || expected_ || store_authorized_ || source >= profile_.vertices ||
            (source_ != UINT64_MAX && source <= source_))
            throw std::logic_error("window-observer-nonmonotone-source");
        last_read_.valid = false;
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
        if (!open_ || source_ == UINT64_MAX || expected_ || store_authorized_ || destination >= profile_.vertices ||
            (windows_ && (index < row_first_ || index >= row_last_)))
            throw std::logic_error("window-observer-invalid-record-association");
        expected_ = true;
        expected_index_ = index;
        expected_address_ = property_base_ + uint64_t(destination) * 4;
        expected_value_ = windows_ ? profile_.decode(tokens_.at(index), source_, pass_base_) : 0;
        ++designated_;
        ++passes_[pass_count_ - 1].designated;
        if (expected_value_)
            ++executed_known_;
    }

    void associateStore(uint64_t index, uint32_t destination, uint64_t base, uint64_t bytes) {
        if (!windows_)
            return;
        if (expected_ || in_access_ || store_authorized_)
            throw std::logic_error("window-observer-store-association-order");
        ++association_requests_;
        if (!open_ || !last_read_.valid || base != property_base_ || bytes != 4 ||
            destination >= profile_.vertices || index != last_read_.index ||
            property_base_ + uint64_t(destination) * 4 != last_read_.address ||
            last_read_.pass != pass_count_ || last_read_.source != source_ ||
            last_read_.request != requests_ || last_read_.order != order_) {
            ++association_rejected_;
            last_read_.valid = false;
            return;
        }
        ++association_accepted_;
        store_authorized_ = true;
    }

    void beforeAccess(uint64_t address, bool write) {
        if (in_access_ || (expected_ && (write || address != expected_address_)) ||
            (store_authorized_ && (!write || !last_read_.valid || address != last_read_.address)))
            throw std::logic_error("window-observer-memory-association");
        forwarding_store_ = store_authorized_;
        forwarded_value_ = forwarding_store_ ? last_read_.value : 0;
        store_authorized_ = false;
        last_read_.valid = false;
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
            auto* line = resident(address);
            if (!expected_ && write) {
                if (line && state(*line, kPreservedDelivered) == State::LIVE)
                    ++writes_kept_live_;
                else if (line && line->states[kPreservedDelivered] == State::PENDING)
                    ++writes_cancelling_pending_;
                else
                    ++writes_without_live_;
            }
            if (line) {
                line->order = order_;
                if (expected_) {
                    for (std::size_t view : kTrialViewIds) {
                        line->states[view] = State::PENDING;
                        line->values[view] = 0;
                    }
                } else {
                    invalidateOrdinary(*line, write);
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
                    for (std::size_t view : {kImmediate, kPreservedImmediate}) {
                        line->values[view] = expected_value_;
                        line->states[view] = expected_value_ ? State::LIVE : State::UNKNOWN;
                    }
                    for (std::size_t view : kTrialViewIds) {
                        line->states[view] = State::PENDING;
                        line->values[view] = 0;
                    }
                } else {
                    ++nonresident_observations_;
                }
            } else if (auto* line = resident(address)) {
                line->order = order_;
                invalidateOrdinary(*line, write);
            }
            enqueue(address & ~uint64_t{63}, expected_ ? expected_value_ : forwarded_value_,
                expected_ ? UpdateKind::HINT : forwarding_store_ ? UpdateKind::FORWARDED_WRITE :
                write ? UpdateKind::WRITE_BARRIER : UpdateKind::INVALIDATE_READ);
            if (forwarding_store_) {
                ++forwarded_updates_;
                forwarded_known_ += forwarded_value_ != 0;
            }
            if (expected_)
                last_read_ = {expected_index_, expected_address_, expected_value_,
                    order_, requests_, source_, pass_count_, true};
            if (!write)
                observeReuse(address & ~uint64_t{63}, at_llc, memory_miss);
        }
        expected_ = false;
        in_access_ = false;
        forwarding_store_ = false;
    }

    void endPass() {
        if (!open_ || expected_ || in_access_ || store_authorized_)
            throw std::logic_error("window-observer-invalid-pass-close");
        last_read_.valid = false;
        marker();
        watermark_ = pass_base_ + profile_.bins;
        if (windows_)
            expireTrials(true);
        open_ = false;
    }

    void finish(uint64_t actual_records) {
        if (open_ || expected_ || in_access_ || store_authorized_ || actual_records != designated_)
            throw std::logic_error("window-observer-incomplete-work");
        drain();
        if (enqueued_ != dispatched_ || dispatched_ != applied_ + stale_ + absent_ ||
            ordinary_writes_ != writes_kept_live_ + writes_cancelling_pending_ + writes_without_live_ ||
            association_requests_ != association_accepted_ + association_rejected_ ||
            association_accepted_ != forwarded_updates_ ||
            forwarded_updates_ != forwarded_applied_ + forwarded_stale_ + forwarded_absent_)
            throw std::logic_error("window-observer-incomplete-accounting");
        for (std::size_t group = 0; group < kTrialViews; ++group) {
            const auto& trials = trial_stats_[group];
            const auto& samples = samples_[kTrialViewIds[group]];
            if (trials.active || trials.started != trials.base_first + trials.alternative_first +
                trials.censored_horizon + trials.censored_pass ||
                trials.started + trials.dropped != samples.overrides)
                throw std::logic_error("window-observer-incomplete-trial-accounting");
        }
    }

    void write(std::ostream& output) const {
        output << "{\"schema\":\"ecg.window-eviction-observer.v3\",\"mode\":\""
               << (windows_ ? "window" : "control")
               << "\",\"active_policy_changed\":false,\"diagnostic_costs_in_cache_counters\":false,"
               << "\"delivery_model\":\"uncoalesced-eight-access-steps-serialized-markers\","
               << "\"write_survival_rule\":\"published-only-pending-cancelled\","
               << "\"publication_rule\":\"checked-read-store-new-event\","
               << "\"association_rule\":\"exact-index-element-binding-source-pass-adjacent-access\","
               << "\"read_pair_window_scope\":\"sampled-endpoints-not-live-state\","
               << "\"sample_period\":" << kSamplePeriod << ",\"trial_capacity\":" << kTrials
               << ",\"trial_views\":" << kTrialViews
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
               << ",\"writes_kept_live\":" << writes_kept_live_
               << ",\"writes_cancelling_pending\":" << writes_cancelling_pending_
               << ",\"writes_without_live\":" << writes_without_live_
               << ",\"association_requests\":" << association_requests_
               << ",\"association_accepted\":" << association_accepted_
               << ",\"association_rejected\":" << association_rejected_
               << ",\"association_slot_bytes\":" << sizeof(ReadAssociation)
               << ",\"forwarded_store_updates\":" << forwarded_updates_
               << ",\"forwarded_known_updates\":" << forwarded_known_
               << ",\"forwarded_applied\":" << forwarded_applied_
               << ",\"forwarded_known_applied\":" << forwarded_known_applied_
               << ",\"forwarded_stale\":" << forwarded_stale_
               << ",\"forwarded_absent\":" << forwarded_absent_
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
        for (std::size_t view = 0; view < kViews; ++view) {
            output << ",\"" << kViewNames[view] << "\":";
            samples_[view].write(output);
        }
        for (std::size_t group = 0; group < kTrialViews; ++group) {
            output << ",\"" << kTrialNames[group] << "\":";
            trial_stats_[group].write(output);
        }
        output << ",\"per_pass\":[";
        for (std::size_t pass = 0; pass < pass_count_; ++pass) {
            if (pass) output << ',';
            const auto& stats = passes_[pass];
            output << "{\"pass\":" << pass << ",\"vertices\":" << stats.vertices
                   << ",\"designated_reads\":" << stats.designated << ",\"evictions\":" << stats.evictions;
            for (std::size_t view = 0; view < kViews; ++view) {
                output << ",\"" << kViewNames[view] << "\":";
                stats.samples[view].write(output);
            }
            output << '}';
        }
        output << "]}";
    }

  private:
    enum class State : uint8_t { UNSEEN, UNKNOWN, PENDING, INVALIDATED, EXPIRED, OLD_PASS, LIVE, NON_PROPERTY };
    struct Shadow {
        uint64_t line = UINT64_MAX, order = 0;
        std::array<uint64_t, kViews> values{};
        std::array<State, kViews> states{};
    };
    struct Samples {
        uint64_t count = 0, eligible = 0, live_eligible = 0, overrides = 0;
        uint64_t live_base_without_alternative = 0, live_base_equal_ranks = 0;
        uint64_t live_base_already_worst = 0, only_ineligible_worse = 0;
        uint64_t live_current_cohort = 0, live_saturated_strength = 0;
        uint64_t live_base_no_eligible_depth = 0, live_base_eligible_without_live_hint = 0;
        std::array<uint64_t, 8> base_states{};
        void write(std::ostream& output) const {
            output << "{\"samples\":" << count << ",\"eligible_candidates\":" << eligible
                   << ",\"live_eligible_candidates\":" << live_eligible
                   << ",\"hypothetical_overrides\":" << overrides
                   << ",\"live_base_without_alternative\":" << live_base_without_alternative
                   << ",\"live_base_equal_ranks\":" << live_base_equal_ranks
                   << ",\"live_base_already_worst\":" << live_base_already_worst
                   << ",\"only_ineligible_worse\":" << only_ineligible_worse
                   << ",\"live_current_cohort\":" << live_current_cohort
                   << ",\"live_saturated_strength\":" << live_saturated_strength
                   << ",\"live_base_no_eligible_depth\":" << live_base_no_eligible_depth
                   << ",\"live_base_eligible_without_live_hint\":" << live_base_eligible_without_live_hint
                   << ",\"base_states\":[";
            for (std::size_t index = 0; index < base_states.size(); ++index) {
                if (index) output << ',';
                output << base_states[index];
            }
            output << "]}";
        }
    };
    struct PassStats {
        uint64_t vertices = 0, designated = 0, evictions = 0;
        std::array<Samples, kViews> samples;
    };
    enum class UpdateKind : uint8_t { HINT, INVALIDATE_READ, WRITE_BARRIER, FORWARDED_WRITE };
    struct Update {
        uint64_t line = 0, value = 0, order = 0, ready = 0;
        UpdateKind kind = UpdateKind::HINT;
    };
    struct ReadAssociation {
        uint64_t index = 0, address = 0, value = 0, order = 0, request = 0, source = 0, pass = 0;
        bool valid = false;
    };
    struct Trial {
        uint64_t base = 0, alternative = 0, start = 0, expires = 0;
        uint64_t base_end = 0, alternative_end = 0;
        int32_t next = -1;
        bool active = false;
    };
    struct TrialStats {
        uint64_t started = 0, dropped = 0, base_first = 0, alternative_first = 0;
        uint64_t base_first_llc = 0, alternative_first_llc = 0;
        uint64_t base_first_memory = 0, alternative_first_memory = 0;
        uint64_t base_first_before_endpoint = 0, alternative_first_before_endpoint = 0;
        uint64_t base_first_before_both_endpoints = 0, alternative_first_before_both_endpoints = 0;
        uint64_t censored_horizon = 0, censored_pass = 0;
        std::size_t active = 0, peak = 0;
        void write(std::ostream& output) const {
            output << "{\"started\":" << started << ",\"capacity_dropped\":" << dropped
                   << ",\"base_first\":" << base_first << ",\"alternative_first\":" << alternative_first
                   << ",\"base_first_llc\":" << base_first_llc
                   << ",\"alternative_first_llc\":" << alternative_first_llc
                   << ",\"base_first_memory\":" << base_first_memory
                   << ",\"alternative_first_memory\":" << alternative_first_memory
                   << ",\"base_first_before_endpoint\":" << base_first_before_endpoint
                   << ",\"alternative_first_before_endpoint\":" << alternative_first_before_endpoint
                   << ",\"base_first_before_both_endpoints\":" << base_first_before_both_endpoints
                   << ",\"alternative_first_before_both_endpoints\":" << alternative_first_before_both_endpoints
                   << ",\"censored_horizon\":" << censored_horizon
                   << ",\"censored_pass\":" << censored_pass << ",\"peak_pending\":" << peak << '}';
        }
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
    void invalidateOrdinary(Shadow& line, bool write) {
        for (std::size_t view : {kImmediate, kDelivered}) {
            line.states[view] = State::INVALIDATED;
            line.values[view] = 0;
        }
        // A write advances every view's order cutoff, but cannot resurrect pending evidence.
        if (!write) {
            for (std::size_t view : {kPreservedImmediate, kPreservedDelivered}) {
                line.states[view] = State::INVALIDATED;
                line.values[view] = 0;
            }
        } else if (line.states[kPreservedDelivered] == State::PENDING) {
            line.states[kPreservedDelivered] = State::INVALIDATED;
            line.values[kPreservedDelivered] = 0;
        }
        line.states[kForwardedDelivered] = forwarding_store_ ? State::PENDING : State::INVALIDATED;
        line.values[kForwardedDelivered] = 0;
    }
    State state(const Shadow& line, std::size_t view) const {
        if (!isProperty(line.line))
            return State::NON_PROPERTY;
        const State raw = line.states[view];
        if (raw != State::LIVE)
            return raw;
        const uint64_t end = line.values[view] >> 3;
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
        const bool forwarded = update.kind == UpdateKind::FORWARDED_WRITE;
        if (!line) {
            ++absent_;
            forwarded_absent_ += forwarded;
        } else if (line->order != update.order) {
            ++stale_;
            forwarded_stale_ += forwarded;
        } else {
            line->values[kDelivered] = update.kind == UpdateKind::HINT ? update.value : 0;
            line->states[kDelivered] = update.kind != UpdateKind::HINT ? State::INVALIDATED :
                update.value ? State::LIVE : State::UNKNOWN;
            if (update.kind == UpdateKind::HINT || update.kind == UpdateKind::INVALIDATE_READ) {
                line->values[kPreservedDelivered] = update.value;
                line->states[kPreservedDelivered] = update.kind == UpdateKind::INVALIDATE_READ ?
                    State::INVALIDATED : update.value ? State::LIVE : State::UNKNOWN;
            }
            line->values[kForwardedDelivered] = update.kind == UpdateKind::HINT || forwarded ? update.value : 0;
            line->states[kForwardedDelivered] = update.kind == UpdateKind::HINT || forwarded ?
                (update.value ? State::LIVE : State::UNKNOWN) : State::INVALIDATED;
            ++applied_;
            forwarded_applied_ += forwarded;
            forwarded_known_applied_ += forwarded && update.value != 0;
        }
    }
    void enqueue(uint64_t line, uint64_t value, UpdateKind kind) {
        if (queued_ == kQueue || step_ > UINT64_MAX - kLatency)
            throw std::length_error("window-observer-required-update-overflow");
        queue_[(queue_head_ + queued_) % kQueue] = {line, value, order_, step_ + kLatency, kind};
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

    void sample(std::size_t set, const CacheLine* lines, std::size_t base, std::size_t view,
                Samples& total, Samples& pass) {
        if (lines[base].rrpv < 7)
            throw std::logic_error("window-observer-non-GRASP-eviction-candidate");
        const auto add = [&](uint64_t Samples::* field, uint64_t amount) {
            total.*field += amount;
            pass.*field += amount;
        };
        add(&Samples::count, 1);
        const Shadow* entries = shadow_.data() + set * ways_;
        const State base_state = state(entries[base], view);
        ++total.base_states[static_cast<std::size_t>(base_state)];
        ++pass.base_states[static_cast<std::size_t>(base_state)];
        std::size_t proposed = base;
        uint64_t alternatives = 0, equal_ranks = 0, eligible_alternatives = 0;
        bool worse_ineligible = false;
        for (std::size_t way = 0; way < ways_; ++way) {
            if (!lines[way].valid || entries[way].line != lines[way].line_addr)
                throw std::logic_error("window-observer-residency-mismatch");
            if (!isProperty(lines[way].line_addr))
                continue;
            const bool eligible = lines[way].rrpv >= 7;
            if (eligible) {
                add(&Samples::eligible, 1);
                if (way != base)
                    ++eligible_alternatives;
            }
            if (state(entries[way], view) != State::LIVE)
                continue;
            const uint64_t b = entries[way].values[view];
            const uint64_t db = profile_.distance(b, watermark_, pass_base_);
            if (eligible) {
                add(&Samples::live_eligible, 1);
                if (db == 0) add(&Samples::live_current_cohort, 1);
                if ((b & 7) == 7) add(&Samples::live_saturated_strength, 1);
            }
            if (base_state != State::LIVE || way == base)
                continue;
            const uint64_t original = entries[base].values[view];
            const uint64_t original_distance = profile_.distance(original, watermark_, pass_base_);
            if (!eligible) {
                worse_ineligible = worse_ineligible || db > original_distance ||
                    (db == original_distance && (b & 7) < (original & 7));
                continue;
            }
            ++alternatives;
            if (db == original_distance && (b & 7) == (original & 7))
                ++equal_ranks;
            const uint64_t a = entries[proposed].values[view];
            const uint64_t da = profile_.distance(a, watermark_, pass_base_);
            if (db > da || (db == da && (b & 7) < (a & 7)))
                proposed = way;
        }
        if (proposed != base) {
            add(&Samples::overrides, 1);
            for (std::size_t group = 0; group < kTrialViews; ++group)
                if (view == kTrialViewIds[group])
                    startTrial(group, set, entries[base].line, entries[proposed].line,
                        entries[base].values[view] >> 3, entries[proposed].values[view] >> 3);
        } else if (base_state == State::LIVE) {
            if (!alternatives) {
                add(&Samples::live_base_without_alternative, 1);
                add(eligible_alternatives ? &Samples::live_base_eligible_without_live_hint :
                    &Samples::live_base_no_eligible_depth, 1);
            } else if (equal_ranks == alternatives)
                add(&Samples::live_base_equal_ranks, 1);
            else
                add(&Samples::live_base_already_worst, 1);
            if (worse_ineligible)
                add(&Samples::only_ineligible_worse, 1);
        }
    }

    void startTrial(std::size_t group, std::size_t set, uint64_t base, uint64_t alternative,
                    uint64_t base_end, uint64_t alternative_end) {
        if (requests_ > UINT64_MAX - kHorizon)
            throw std::overflow_error("window-observer-trial-horizon-overflow");
        auto& stats = trial_stats_[group];
        if (stats.active == kTrials)
            expireTrials(false);
        if (stats.active == kTrials) {
            ++stats.dropped;
            return;
        }
        for (std::size_t index = group * kTrials; index < (group + 1) * kTrials; ++index) {
            auto& trial = trials_[index];
            if (trial.active)
                continue;
            auto& head = trial_heads_[group * sets_ + set];
            trial = {base, alternative, requests_, requests_ + kHorizon, base_end, alternative_end, head, true};
            head = static_cast<int32_t>(index);
            ++stats.started;
            ++stats.active;
            stats.peak = std::max(stats.peak, stats.active);
            return;
        }
        throw std::logic_error("window-observer-trial-accounting");
    }
    void removeTrial(std::size_t index) {
        auto& trial = trials_[index];
        const std::size_t group = index / kTrials;
        int32_t* link = &trial_heads_[group * sets_ + (trial.base / 64) % sets_];
        while (*link >= 0 && static_cast<std::size_t>(*link) != index)
            link = &trials_[*link].next;
        if (*link < 0 || !trial.active)
            throw std::logic_error("window-observer-trial-link");
        *link = trial.next;
        trial.active = false;
        --trial_stats_[group].active;
    }
    void expireTrials(bool close) {
        for (std::size_t index = 0; index < trials_.size(); ++index) {
            const auto& trial = trials_[index];
            if (trial.active && (close || requests_ >= trial.expires)) {
                auto& stats = trial_stats_[index / kTrials];
                if (requests_ >= trial.expires) ++stats.censored_horizon;
                else ++stats.censored_pass;
                removeTrial(index);
            }
        }
    }
    void observeReuse(uint64_t line, bool at_llc, bool memory_miss) {
        for (std::size_t group = 0; group < kTrialViews; ++group) {
            auto& stats = trial_stats_[group];
            int32_t index = trial_heads_[group * sets_ + (line / 64) % sets_];
            while (index >= 0) {
                const auto trial = trials_[index];
                const int32_t next = trial.next;
                if (requests_ >= trial.expires) {
                    ++stats.censored_horizon;
                    removeTrial(static_cast<std::size_t>(index));
                } else if (requests_ > trial.start && (line == trial.base || line == trial.alternative)) {
                    const bool before_both = watermark_ < trial.base_end && watermark_ < trial.alternative_end;
                    if (line == trial.base) {
                        ++stats.base_first;
                        stats.base_first_llc += at_llc;
                        stats.base_first_memory += memory_miss;
                        stats.base_first_before_endpoint += watermark_ < trial.base_end;
                        stats.base_first_before_both_endpoints += before_both;
                    } else {
                        ++stats.alternative_first;
                        stats.alternative_first_llc += at_llc;
                        stats.alternative_first_memory += memory_miss;
                        stats.alternative_first_before_endpoint += watermark_ < trial.alternative_end;
                        stats.alternative_first_before_both_endpoints += before_both;
                    }
                    removeTrial(static_cast<std::size_t>(index));
                }
                index = next;
            }
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
    std::array<Trial, kTrials * kTrialViews> trials_{};
    std::array<TrialStats, kTrialViews> trial_stats_{};
    std::array<Samples, kViews> samples_{};
    ReadAssociation last_read_;
    ecg_record::StreamDigest demand_digest_, victim_digest_;
    uint64_t reserved_bytes_ = 0, constructed_known_ = 0, executed_known_ = 0;
    uint64_t graph_read_bytes_ = 0, requests_ = 0, fills_ = 0, evictions_ = 0, pass_evictions_ = 0;
    uint64_t pass_count_ = 0, pass_base_ = 0, watermark_ = 0, source_ = UINT64_MAX;
    uint64_t row_first_ = 0, row_last_ = 0, expected_address_ = 0, expected_value_ = 0;
    uint64_t expected_index_ = 0, forwarded_value_ = 0;
    uint64_t designated_ = 0, ordinary_reads_ = 0, ordinary_writes_ = 0, nonresident_observations_ = 0;
    uint64_t writes_kept_live_ = 0, writes_cancelling_pending_ = 0, writes_without_live_ = 0;
    uint64_t association_requests_ = 0, association_accepted_ = 0, association_rejected_ = 0;
    uint64_t forwarded_updates_ = 0, forwarded_known_ = 0, forwarded_applied_ = 0;
    uint64_t forwarded_known_applied_ = 0, forwarded_stale_ = 0, forwarded_absent_ = 0;
    uint64_t step_ = 0, order_ = 0, markers_ = 0, drain_steps_ = 0;
    uint64_t enqueued_ = 0, dispatched_ = 0, applied_ = 0, stale_ = 0, absent_ = 0;
    std::size_t queued_ = 0, queue_head_ = 0, queue_peak_ = 0;
    bool open_ = false, expected_ = false, in_access_ = false, current_property_ = false;
    bool store_authorized_ = false, forwarding_store_ = false;
};

}  // namespace window_observation
}  // namespace cache_sim

#endif
