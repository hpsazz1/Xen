#include "aim/aim.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
// 74bd8cc软件基线；不是物理真值。输入与包络在跑负例前固定。
// 包络为双精度测得值向上取到1e-6，额外1e-9仅处理求和运算误差。
struct FeedbackCase {
    double plant;
    int cadence;
    bool rebound;
    double area_limit;
    double opposed_area_limit;
    double peak_limit;
};
constexpr std::array<FeedbackCase, 8> kCases{{
    {.4325, 68, false, 1.562734, 1.283095, 1.960922},
    {.4325, 68, true, 2.252477, 1.904974, 4.246136},
    {.4325, 119, false, .440894, .079907, 2.273190},
    {.4325, 119, true, 2.567245, 2.294354, 5.434301},
    {.5215, 68, false, .334935, .109593, 1.574028},
    {.5215, 68, true, 1.969400, 1.663518, 3.486636},
    {.5215, 119, false, 1.305280, 1.043588, 3.300707},
    {.5215, 119, true, 1.770845, 1.624721, 5.225801},
}};
constexpr std::array<int, 8> kIntervalsUs{7200,10400,7600,8400,7900,9000,7500,9200};

// 独立预定world；不读取Aim输出、历史counts预算或内部预测。
double world_path(double time, bool rebound) {
    if (time <= 2.0) return time;
    const double z = time - 2.0;
    if (!rebound) return z < .4 ? 2.0 + z - z*z/.8 : 2.2;
    if (z < .4) return 2.0 + z - z*z/.4;
    if (time < 3.0) return 2.0 - (time - 2.4);
    if (time < 3.4) {
        const double w = time - 3.0;
        return 1.4 - w + w*w/.8;
    }
    return 1.2;
}

bool run_case(const FeedbackCase& test, int direction) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.acquisition_range_percent = 100;
    config.smoothing = .475f;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .4f;
    config.max_counts_per_frame = 12;
    config.deadzone_pixels = 1.5f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.body_aim_height_ratio = .5f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    const double speed = test.cadence == 68 ? .97/.014620 : 96.43;
    struct Pending { std::int64_t applied_us; int counts; };
    std::vector<Pending> pending;
    std::size_t applied = 0;
    std::int64_t elapsed = 0;
    double camera = 0, area = 0, opposed_area = 0, peak = 0, tail_peak = 0;
    int tail_counts = 0, samples = 0, transition_samples = 0, tail_samples = 0;
    bool valid = true;
    // 与冻结基线相同：包含首次越过6秒的完整真实采样区间。
    for (int index = 0; elapsed <= 6000000; ++index) {
        const int interval = test.cadence == 68 ? 14620 : kIntervalsUs[(index ? index-1 : 0)%8];
        if (index) elapsed += interval;
        const double time = elapsed * 1e-6;
        while (applied < pending.size() && pending[applied].applied_us <= elapsed)
            camera += pending[applied++].counts * test.plant;
        const double world = direction * (12 + speed * world_path(time, test.rebound));
        const double error = world - camera;
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index+1);
        frame.captured_at = base + std::chrono::microseconds(elapsed);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = true;
        const float center = static_cast<float>(160 + error);
        frame.detections = {{center-20,120,center+20,200,.9f,0}};
        const auto result = aim.process(frame);
        valid = valid && result.status == AimStatus::SUCCESS && result.has_target;
        const int q = result.has_command ? result.command.dx_counts : 0;
        if (result.has_command) {
            valid = aim.record_backend_completed_command(frame.sequence, frame.control_at,
                q, result.command.dy_counts) && valid;
            // 自己的已完成q驱动下一观测，作用延迟40ms，不能喂旧camera。
            pending.push_back({elapsed+41000,q});
        }
        valid = valid && q*(result.target.base_aim_x-160) >= 0 &&
            result.command.dy_counts == 0 && std::abs(q) <= 12;
        if (time >= 2 && time <= 4) {
            area += std::abs(error) * interval * 1e-6;
            opposed_area += std::max(0.0,-direction*error) * interval * 1e-6;
            peak = std::max(peak,std::abs(error));
            ++transition_samples;
        }
        if (time >= 5) {
            tail_peak = std::max(tail_peak,std::abs(error));
            tail_counts += std::abs(q);
            ++tail_samples;
        }
        ++samples;
    }
    const bool passed = valid && samples == (test.cadence == 68 ? 412 : 716) &&
        transition_samples > 0 && tail_samples > 0 &&
        std::isfinite(area) && std::isfinite(opposed_area) && std::isfinite(peak) &&
        std::isfinite(tail_peak) && area <= test.area_limit+1e-9 &&
        opposed_area <= test.opposed_area_limit+1e-9 && peak <= test.peak_limit+1e-9 &&
        tail_peak <= config.deadzone_pixels && tail_counts == 0;
    if (!passed) std::cerr << "[失败] 独立减速/反弹反馈质量：plant/cadence/rebound/direction="
        << test.plant << '/' << test.cadence << '/' << test.rebound << '/' << direction
        << "，绝对面积/反侧面积/峰值/静止残留/尾请求=" << area << '/' << opposed_area
        << '/' << peak << '/' << tail_peak << '/' << tail_counts << '\n';
    return passed;
}
} // namespace

int run_aim_feedback_budget_tests() {
    int failures = 0;
    std::array<bool,16> covered{};
    for (const auto& test : kCases) {
        // 不支持的参数、重复case和遗漏覆盖均硬失败，不能查不到包络就跳过。
        if ((test.plant != .4325 && test.plant != .5215) ||
            (test.cadence != 68 && test.cadence != 119)) { ++failures; continue; }
        for (int direction : {-1,1}) {
            const int key = (test.plant == .5215 ? 8 : 0) +
                (test.cadence == 119 ? 4 : 0) + (test.rebound ? 2 : 0) + (direction > 0 ? 1 : 0);
            if (covered[key]) { ++failures; continue; }
            covered[key] = true;
            if (!run_case(test,direction)) ++failures;
        }
    }
    for (bool present : covered) if (!present) ++failures;
    return failures;
}
