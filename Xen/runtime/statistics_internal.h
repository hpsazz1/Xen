#ifndef RUNTIME_STATISTICS_INTERNAL_H
#define RUNTIME_STATISTICS_INTERNAL_H

#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <span>
#include <vector>

namespace runtime::detail {

inline bool pipeline_sample_succeeded(DetectionStatus detection, AimStatus aim,
        MouseStatus mouse, bool mouse_sent) noexcept {
    return detection == DetectionStatus::SUCCESS && aim == AimStatus::SUCCESS &&
        (!mouse_sent || mouse == MouseStatus::READY);
}

// 输入已按升序排列；保留既有 (N-1)*q 线性插值定义。
inline double sorted_percentile(std::span<const double> values, double quantile) noexcept {
    if (values.empty()) return 0.0;
    const double position = quantile * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

struct LatencyPercentiles {
    std::size_t count = 0;
    double last = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
};

class PipelineLatencyWindow {
public:
    void reset(std::size_t capacity) {
        capacity_ = std::max<std::size_t>(1, capacity);
        pipeline_.clear();
        control_.clear();
        pipeline_sorted_.clear();
        control_sorted_.clear();
        pipeline_sorted_.reserve(capacity_);
        control_sorted_.reserve(capacity_);
        pipeline_summary_ = {};
        control_summary_ = {};
    }

    void observe(const PipelineProfile& profile, AimStatus aim_status,
            MouseStatus mouse_status, bool mouse_sent) {
        if (!pipeline_sample_succeeded(profile.detector.status, aim_status, mouse_status, mouse_sent)) return;
        append(pipeline_, pipeline_sorted_, profile.total_ms, pipeline_summary_);
        if (profile.mouse_backend_completion_timing_valid) {
            append(control_, control_sorted_, profile.capture_to_mouse_backend_completion_ms, control_summary_);
        }
    }

    const LatencyPercentiles& pipeline() const noexcept { return pipeline_summary_; }
    const LatencyPercentiles& control() const noexcept { return control_summary_; }

private:
    void append(std::deque<double>& values, std::vector<double>& sorted,
                double value, LatencyPercentiles& summary) {
        values.push_back(value);
        if (values.size() > capacity_) {
            // 每路独立保留有序副本，只移除最旧样本的一次出现，重复值不多删。
            sorted.erase(std::lower_bound(sorted.begin(), sorted.end(), values.front()));
            values.pop_front();
        }
        sorted.insert(std::lower_bound(sorted.begin(), sorted.end(), value), value);
        summary = {values.size(), value,
                   sorted_percentile(sorted, 0.50), sorted_percentile(sorted, 0.95)};
    }

    std::size_t capacity_ = 1;
    std::deque<double> pipeline_, control_;
    std::vector<double> pipeline_sorted_, control_sorted_;
    LatencyPercentiles pipeline_summary_, control_summary_;
};

} // namespace runtime::detail

#endif
