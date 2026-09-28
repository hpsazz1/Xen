#ifndef XEN_DEBUG_SESSION_STATISTICS_INTERNAL_H
#define XEN_DEBUG_SESSION_STATISTICS_INTERNAL_H

#include "debug/debug.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ostream>

namespace xen::debug::detail {

// 固定桶只描述诊断精度，绝不参与控制。全部数组在start时分配，不在摄入时增长。
class SessionStatistics {
    static constexpr std::size_t kFiniteBuckets = 4096;
    static constexpr std::size_t kClosedMinutes = 1439;
    static constexpr double kBucketWidthMs = .25;
    static constexpr std::int64_t kMinuteNs = 60000000000LL;

    struct Bounds {
        bool available = false;
        bool overflow = false;
        double lower = 0, upper = 0;
    };
    struct MetricSummary {
        std::uint64_t count = 0, missing = 0, invalid = 0, overflow_count = 0;
        double sum = 0, maximum = 0;
        bool arithmetic_overflow = false;
        std::array<Bounds, 3> quantiles{};
    };
    struct Histogram {
        std::array<std::uint64_t, kFiniteBuckets + 1> buckets{};
        MetricSummary values;

        void observe(bool available, double value, bool& counter_overflow) noexcept {
            if (!available) { add(values.missing, 1, counter_overflow); return; }
            if (!std::isfinite(value) || value < 0) {
                add(values.invalid, 1, counter_overflow);
                return;
            }
            const std::size_t index = value > kFiniteBuckets * kBucketWidthMs
                ? kFiniteBuckets
                : (value == 0 ? 0 : static_cast<std::size_t>(std::ceil(value / kBucketWidthMs)) - 1);
            add(buckets[index], 1, counter_overflow);
            add(values.count, 1, counter_overflow);
            if (index == kFiniteBuckets) add(values.overflow_count, 1, counter_overflow);
            values.maximum = std::max(values.maximum, value);
            if (std::isfinite(values.sum + value)) values.sum += value;
            else values.arithmetic_overflow = true;
        }
        MetricSummary summarize() const noexcept {
            auto result = values;
            if (!values.count) return result;
            constexpr std::array<std::uint64_t, 3> percentages{50, 95, 99};
            for (std::size_t q = 0; q < percentages.size(); ++q) {
                // 整数最近秩ceil(p*N)，避免大计数乘法溢出。
                const auto rank = values.count / 100 * percentages[q] +
                    (values.count % 100 * percentages[q] + 99) / 100;
                std::uint64_t cumulative = 0;
                for (std::size_t i = 0; i < buckets.size(); ++i) {
                    if (buckets[i] >= rank - cumulative) {
                        result.quantiles[q] = {true, i == kFiniteBuckets,
                            i * kBucketWidthMs, (i + 1) * kBucketWidthMs};
                        break;
                    }
                    cumulative += buckets[i];
                }
            }
            return result;
        }
    };
    struct Counts {
        std::uint64_t ingested = 0, success = 0, failed = 0, mouse_bad = 0;
        void observe(const RuntimePipelineSample& s, bool succeeded, bool& overflow) noexcept {
            add(ingested, 1, overflow);
            add(succeeded ? success : failed, 1, overflow);
            if (s.mouse_sent && s.mouse_status != MouseStatus::READY) add(mouse_bad, 1, overflow);
        }
    };
    struct Metrics {
        std::array<Histogram, 3> histograms;
        void observe(const RuntimePipelineSample& s, bool& overflow) noexcept {
            histograms[0].observe(true, s.profile.total_ms, overflow);
            histograms[1].observe(s.profile.mouse_backend_completion_timing_valid,
                s.profile.capture_to_mouse_backend_completion_ms, overflow);
            histograms[2].observe(s.profile.mouse_backend_completion_timing_valid,
                s.profile.control_to_mouse_backend_completion_ms, overflow);
        }
    };
    struct Minute {
        std::uint64_t index = 0;
        std::int64_t first_ns = 0, last_ns = 0;
        Counts counts;
        std::array<MetricSummary, 3> metrics;
    };
    Counts counts_;
    Metrics total_, current_;
    Counts current_counts_;
    std::array<Minute, kClosedMinutes> minutes_{};
    std::size_t minute_count_ = 0, minute_head_ = 0;
    std::uint64_t minute_index_ = 0, minutes_omitted_ = 0, empty_minutes_ = 0;
    std::uint64_t invalid_time_ = 0, regressed_time_ = 0, valid_time_samples_ = 0;
    std::int64_t max_control_gap_ns_ = 0;
    std::uint64_t sequence_gaps_ = 0, duplicate_sequence_ = 0, regressed_sequence_ = 0;
    std::uint64_t first_sequence_ = 0, last_sequence_ = 0;
    std::uint64_t ingest_exceptions_ = 0, raw_samples_lost_ = 0;
    std::int64_t origin_ns_ = 0, last_time_ns_ = 0, minute_first_ns_ = 0;
    bool clock_started_ = false, sequence_started_ = false, counter_overflow_ = false;

    static void add(std::uint64_t& value, std::uint64_t amount, bool& overflow) noexcept {
        if (amount > (std::numeric_limits<std::uint64_t>::max)() - value) {
            value = (std::numeric_limits<std::uint64_t>::max)();
            overflow = true;
        } else value += amount;
    }
    Minute current_summary() const noexcept {
        Minute row;
        row.index = minute_index_;
        row.first_ns = minute_first_ns_;
        row.last_ns = last_time_ns_;
        row.counts = current_counts_;
        for (std::size_t i = 0; i < row.metrics.size(); ++i)
            row.metrics[i] = current_.histograms[i].summarize();
        return row;
    }
    void close_minute() noexcept {
        const auto row = current_summary();
        if (minute_count_ < kClosedMinutes) minutes_[minute_count_++] = row;
        else {
            minutes_[minute_head_] = row;
            minute_head_ = (minute_head_ + 1) % kClosedMinutes;
            add(minutes_omitted_, 1, counter_overflow_);
        }
        current_counts_ = {};
        current_ = {};
    }
    static void write_counts(std::ostream& out, const Counts& c) {
        out << "\"ingested_samples\":" << c.ingested
            << ",\"successful_samples\":" << c.success
            << ",\"failed_samples\":" << c.failed
            << ",\"mouse_sent_not_ready\":" << c.mouse_bad;
    }
    static void write_metric(std::ostream& out, const MetricSummary& v, const Histogram* histogram = nullptr) {
        out << "{\"valid_samples\":" << v.count << ",\"missing_samples\":" << v.missing
            << ",\"invalid_samples\":" << v.invalid << ",\"overflow_samples\":" << v.overflow_count
            << ",\"arithmetic_overflow\":" << (v.arithmetic_overflow ? "true" : "false")
            << ",\"mean_ms\":";
        if (v.count && !v.arithmetic_overflow) out << v.sum / v.count; else out << "null";
        out << ",\"max_ms\":";
        if (v.count) out << v.maximum; else out << "null";
        constexpr std::array<const char*, 3> names{"p50", "p95", "p99"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            out << ",\"" << names[i] << "\":";
            const auto& q = v.quantiles[i];
            if (!q.available) out << "null";
            else {
                out << "{\"lower_ms\":" << q.lower << ",\"upper_ms\":";
                if (q.overflow) out << "null,\"overflow\":true"; else out << q.upper;
                out << '}';
            }
        }
        if (histogram) {
            out << ",\"bucket_counts\":[";
            for (std::size_t i = 0; i < histogram->buckets.size(); ++i) {
                if (i) out << ',';
                out << histogram->buckets[i];
            }
            out << ']';
        }
        out << '}';
    }
    static void write_metrics(std::ostream& out, const std::array<MetricSummary, 3>& metrics,
                              const Metrics* histograms = nullptr) {
        constexpr std::array<const char*, 3> names{"total", "capture_to_mouse_backend_completion",
                                                  "control_to_mouse_backend_completion"};
        out << "\"timing\":{";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) out << ',';
            out << '\"' << names[i] << "\":";
            write_metric(out, metrics[i], histograms ? &histograms->histograms[i] : nullptr);
        }
        out << '}';
    }
    static void write_minute(std::ostream& out, const Minute& row, bool partial) {
        out << "{\"minute_index\":" << row.index << ",\"first_control_steady_ns\":\"" << row.first_ns
            << "\",\"last_control_steady_ns\":\"" << row.last_ns << "\",\"partial\":"
            << (partial ? "true" : "false") << ',';
        write_counts(out, row.counts);
        out << ',';
        write_metrics(out, row.metrics);
        out << '}';
    }
public:
    void observe(const RuntimePipelineSample& s) noexcept {
        const bool succeeded = debug_sample_succeeded(s);
        counts_.observe(s, succeeded, counter_overflow_);
        if (succeeded) total_.observe(s, counter_overflow_);
        if (!sequence_started_) { first_sequence_ = s.sequence; sequence_started_ = true; }
        else if (s.sequence == last_sequence_) add(duplicate_sequence_, 1, counter_overflow_);
        else if (s.sequence < last_sequence_) add(regressed_sequence_, 1, counter_overflow_);
        else add(sequence_gaps_, s.sequence - last_sequence_ - 1, counter_overflow_);
        // 不静默去重；最后sequence是高水位，回退不会降低下一次比较基线。
        last_sequence_ = std::max(last_sequence_, s.sequence);
        const auto time = s.frame_timing.control_steady_ns;
        if (!s.frame_timing.control_steady_valid || time < 0) {
            add(invalid_time_, 1, counter_overflow_);
            return;
        }
        if (clock_started_ && time < last_time_ns_) {
            add(regressed_time_, 1, counter_overflow_);
            return;
        }
        if (clock_started_) max_control_gap_ns_ = std::max(max_control_gap_ns_, time - last_time_ns_);
        add(valid_time_samples_, 1, counter_overflow_);
        if (!clock_started_) {
            clock_started_ = true;
            origin_ns_ = minute_first_ns_ = last_time_ns_ = time;
        }
        const auto index = static_cast<std::uint64_t>((time - origin_ns_) / kMinuteNs);
        if (index != minute_index_) {
            close_minute();
            add(empty_minutes_, index - minute_index_ - 1, counter_overflow_);
            minute_index_ = index;
            minute_first_ns_ = time;
        }
        last_time_ns_ = time;
        current_counts_.observe(s, succeeded, counter_overflow_);
        if (succeeded) current_.observe(s, counter_overflow_);
    }
    void record_raw_ingest_failure(std::size_t remaining) noexcept {
        add(ingest_exceptions_, 1, counter_overflow_);
        add(raw_samples_lost_, static_cast<std::uint64_t>(remaining), counter_overflow_);
    }
    void write_json(std::ostream& out, const RuntimeSnapshot& snapshot) const {
        out << "  \"session_aggregate\":{\"schema\":1,\"scope\":\"all_ingested_samples\","
            << "\"success_semantics\":\"debug_sample_succeeded\",\"timing_population\":\"successful_valid_samples\","
            << "\"quantile_method\":\"nearest_rank_bucket_bounds\",\"bucket_width_ms\":0.25,"
            << "\"max_finite_bucket_ms\":1024,\"bucket_upper_inclusive\":true,"
            << "\"finite_bucket_intervals\":\"(lower,upper]; first includes zero\","
            << "\"bucket_counts_layout\":\"4096 finite buckets followed by (1024,+inf)\","
            << "\"runtime_coverage_complete\":" << (snapshot.debug_samples_dropped ? "false" : "null")
            << ",\"runtime_samples_dropped_lifetime\":" << snapshot.debug_samples_dropped
            << ",\"runtime_processed_frames_lifetime\":" << snapshot.processed_frames
            << ",\"counter_overflow\":" << (counter_overflow_ ? "true" : "false") << ',';
        write_counts(out, counts_);
        out << ",\"first_control_steady_ns\":";
        if (clock_started_) out << '\"' << origin_ns_ << '\"'; else out << "null";
        out << ",\"last_control_steady_ns\":";
        if (clock_started_) out << '\"' << last_time_ns_ << '\"'; else out << "null";
        out << ",\"max_control_gap_ns\":";
        if (valid_time_samples_ >= 2) out << '\"' << max_control_gap_ns_ << '\"'; else out << "null";
        out << ",\"first_sequence\":\"" << first_sequence_ << "\",\"last_sequence\":\"" << last_sequence_
            << "\",\"sequence_gaps\":" << sequence_gaps_ << ",\"duplicate_sequences\":" << duplicate_sequence_
            << ",\"regressed_sequences\":" << regressed_sequence_
            << ",\"ingest_exceptions\":" << ingest_exceptions_ << ",\"raw_samples_lost_on_error\":" << raw_samples_lost_ << ',';
        std::array<MetricSummary, 3> metrics;
        for (std::size_t i = 0; i < metrics.size(); ++i) metrics[i] = total_.histograms[i].summarize();
        write_metrics(out, metrics, &total_);
        out << "},\n  \"minute_trend\":{\"schema\":1,\"scope\":\"all_ingested_samples_with_monotonic_control_time\","
            << "\"complete_semantics\":\"ingested_samples_binned_without_clock_gaps_or_truncation\","
            << "\"clock\":\"SESSION_LOCAL_STEADY_CONTROL\",\"interval_ns\":\"60000000000\",\"capacity\":1440,"
            << "\"origin_control_steady_ns\":";
        if (clock_started_) out << '\"' << origin_ns_ << '\"'; else out << "null";
        out << ",\"invalid_time_samples\":" << invalid_time_ << ",\"regressed_time_samples\":" << regressed_time_
            << ",\"empty_minutes\":" << empty_minutes_ << ",\"windows_omitted\":" << minutes_omitted_
            << ",\"complete\":" << (clock_started_ && !counter_overflow_ && !minutes_omitted_ && !invalid_time_ &&
                !regressed_time_ && !empty_minutes_ && !duplicate_sequence_ && !regressed_sequence_ ? "true" : "false")
            << ",\"windows\":[";
        for (std::size_t i = 0; i < minute_count_; ++i) {
            if (i) out << ',';
            write_minute(out, minutes_[(minute_head_ + i) % kClosedMinutes], false);
        }
        if (clock_started_) {
            if (minute_count_) out << ',';
            write_minute(out, current_summary(), true);
        }
        out << "]},\n";
    }
};

} // namespace xen::debug::detail

#endif // XEN_DEBUG_SESSION_STATISTICS_INTERNAL_H
