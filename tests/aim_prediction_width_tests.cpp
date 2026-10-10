#include "aim/aim.h"
#include "log/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
double median(std::vector<double> values) {
    if (values.empty()) throw std::runtime_error("末段统计不能为空");
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
enum class Width { FIXED, ALTERNATING, SEEDED_ALTERNATING };
struct Summary { double lead = 0, required = 0; unsigned samples = 0; };
Summary replay(Width width, std::ostream* csv, bool closed_loop = false) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_prediction = config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.max_prediction_lead_percent = 35;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .5f;
    config.deadzone_pixels = 1.5f;
    config.soft_zone_radius_percent = 0;
    config.acquisition_range_percent = 100;
    Aim aim(config);
    const auto begin = Clock::time_point{} + 10s;
    AimFrame previous;
    std::uint64_t identity = 0;
    std::vector<double> leads, required;
    // 独立 plant：执行计数经过四个源帧才产生相机位移，增益不读取 Aim 状态。
    std::array<int, 4> delayed{};
    float camera = 0, previous_camera = 0;
    const int frames = closed_loop ? 800 : 80;
    for (int n = 0; n < frames; ++n) {
        camera += delayed[n % delayed.size()] / config.counts_per_pixel_x * .18f;
        delayed[n % delayed.size()] = 0;
        const float center = 160.0f + .75f * n - camera;
        const float size = 20.0f + ((width == Width::ALTERNATING ||
            (width == Width::SEEDED_ALTERNATING && n >= 40)) ? n % 2 : 0);
        AimFrame frame;
        frame.sequence = n + 1;
        frame.captured_at = begin + std::chrono::nanoseconds(static_cast<long long>(n) * 1000000000 / 240);
        frame.control_at = frame.captured_at + 3ms;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = true;
        frame.observation_epoch = 1;
        frame.detections = {{center-size/2, 141.1f, center+size/2, 195.1f, .95f, 0}};
        if (n > 0) {
            auto& bg = frame.background_motion_x;
            bg.status = AimBackgroundMotionStatus::VALID;
            bg.previous_sequence = previous.sequence; bg.sequence = frame.sequence;
            bg.previous_captured_at = previous.captured_at; bg.captured_at = frame.captured_at;
            bg.observation_epoch = frame.observation_epoch;
            bg.dx_roi_pixels = -(camera-previous_camera); bg.usable_patch_count = 2; bg.min_response = 1;
        }
        const auto result = aim.process(frame);
        if (!identity && result.has_target) identity = result.target.track_id;
        expect(result.status == AimStatus::SUCCESS && result.has_target &&
            result.target.track_id == identity && identity != 0 && result.target.state == TrackState::CONFIRMED &&
            !result.target.predicted && result.target.matched_observation_valid,
            "每帧必须是真实匹配的同一CONFIRMED目标");
        if (n > 0) expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
            "每个后续帧必须实际消费合法背景帧对，不能空测");
        const double lead = result.target.prediction_aim_x - result.target.base_aim_x;
        const double dt = n > 0 ? std::chrono::duration<double>(frame.captured_at-previous.captured_at).count() : 0;
        const double truth_velocity = n > 0 ? .75 / dt : 0;
        const double one_horizon = truth_velocity *
            (std::chrono::duration<double>(frame.control_at-frame.captured_at).count() + config.control_delay_ms/1000.0);
        if (n >= frames-20) { leads.push_back(lead); required.push_back(one_horizon); }
        if (csv) *csv << static_cast<int>(width) + (closed_loop ? 3 : 0) << ',' << n << ',' << center << ',' << size << ','
            << truth_velocity << ',' << result.target.velocity_x << ',' << lead << ',' << one_horizon << ','
            << result.target.delay_compensation_ms_x << ',' << result.target.lead_active << ','
            << static_cast<int>(result.control.background_motion_use_x) << ',' << result.target.track_id << '\n';
        const int executed = closed_loop && result.has_command ? result.command.dx_counts : 0;
        delayed[n % delayed.size()] = executed;
        expect(aim.record_backend_completed_command(frame.sequence, frame.control_at, executed, 0),
            "每帧通过真实回执接口明确执行，不借用请求移动推断");
        previous_camera = camera;
        previous = frame;
    }
    return {median(leads), median(required), static_cast<unsigned>(leads.size())};
}

enum class Boundary { WIDTH_ONLY, ONE_EDGE, CAMERA_ONLY, STOP_REVERSE, SOURCE_BREAK };
void boundary_replay(Boundary kind) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_prediction = config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.max_prediction_lead_percent = 35;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .5f;
    config.acquisition_range_percent = 100;
    Aim aim(config);
    const auto begin = Clock::time_point{} + 20s;
    AimFrame previous;
    std::uint64_t identity = 0;
    float previous_camera = 0;
    double max_static_lead = 0, stop_lead = 0, reverse_lead = 0;
    for (int n = 0; n < 240; ++n) {
        float world = 160, size = 20.0f + n%2;
        const float camera = kind == Boundary::CAMERA_ONLY ? 30.0f*std::sin(n*.04f) : 0;
        if (kind == Boundary::STOP_REVERSE)
            world += .75f * (n < 80 ? n : n < 120 ? 79 : 79-(n-119));
        if (kind == Boundary::SOURCE_BREAK) world += .5f*n;
        float left = world-camera-size/2, right = world-camera+size/2;
        if (kind == Boundary::ONE_EDGE) { left = 150; right = 170 + .5f*(n%40 < 20 ? n%40 : 40-n%40); }
        AimFrame frame;
        frame.sequence = n+1;
        frame.captured_at = begin + std::chrono::nanoseconds(static_cast<long long>(n)*1000000000/240);
        frame.control_at = frame.captured_at+3ms;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = true;
        frame.observation_epoch = kind == Boundary::SOURCE_BREAK && n >= 100 ? 2 : 1;
        frame.detections = {{left,141.1f,right,195.1f,.95f,0}};
        if (n > 0) {
            auto& bg = frame.background_motion_x;
            bg.status = kind == Boundary::SOURCE_BREAK && n >= 80 && n < 84
                ? AimBackgroundMotionStatus::MISSING : AimBackgroundMotionStatus::VALID;
            bg.previous_sequence = previous.sequence; bg.sequence = frame.sequence;
            bg.previous_captured_at = previous.captured_at; bg.captured_at = frame.captured_at;
            bg.observation_epoch = frame.observation_epoch;
            bg.dx_roi_pixels = -(camera-previous_camera); bg.usable_patch_count = 2; bg.min_response = 1;
        }
        const auto result = aim.process(frame);
        if (!identity && result.has_target) identity = result.target.track_id;
        expect(result.status == AimStatus::SUCCESS && result.has_target && result.target.track_id == identity &&
            result.target.state == TrackState::CONFIRMED && result.target.matched_observation_valid && !result.target.predicted,
            "反例必须逐帧保留同一实测目标，不能通过丢失目标获得零前探");
        const bool rejected = kind == Boundary::SOURCE_BREAK && ((n >= 80 && n < 84) || n == 100);
        if (n > 0) expect((result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED) != rejected,
            "反例背景消费必须与真实来源资格一致");
        const double lead = result.target.prediction_aim_x-result.target.base_aim_x;
        if (kind == Boundary::WIDTH_ONLY || kind == Boundary::ONE_EDGE || kind == Boundary::CAMERA_ONLY)
            max_static_lead = std::max(max_static_lead, std::abs(lead));
        if (kind == Boundary::STOP_REVERSE && n >= 110 && n < 120) stop_lead = std::max(stop_lead,std::abs(lead));
        if (kind == Boundary::STOP_REVERSE && n == 239) reverse_lead = lead;
        if (rejected) expect(!result.target.lead_active && std::abs(lead) < .01,
            "缺背景或 epoch 断点必须撤回前探");
        expect(aim.record_backend_completed_command(frame.sequence,frame.control_at,0,0),"反例必须明确零执行回执");
        previous = frame; previous_camera = camera;
    }
    std::cout << "boundary=" << static_cast<int>(kind) << "; static_max=" << max_static_lead
              << "; stop_tail=" << stop_lead << "; reverse_last=" << reverse_lead << '\n';
    expect(max_static_lead < .01,"静止形变、单边截断恢复或纯相机运动不能建立人物前探");
    if (kind == Boundary::STOP_REVERSE) {
        expect(stop_lead < .01,"真实停止后必须撤回前探");
        expect(reverse_lead <= -3.24,"反向稳定后必须以新世界方向建立足够前探");
    }
}
}
int main(int argc, char** argv) try {
    LogConfig logging;
    logging.enable_console = logging.enable_file = logging.enable_debug_file = false;
    Log::init(logging);
    std::ofstream csv;
    if (argc == 2) {
        csv.open(argv[1]);
        if (!csv) throw std::runtime_error("无法创建回放CSV");
        csv << "width_mode,frame,center,width,truth_velocity,target_velocity,lead,one_horizon,projection_ms_x,lead_active,background_use,track_id\n";
    }
    const auto fixed = replay(Width::FIXED, csv.is_open() ? &csv : nullptr);
    const auto alternating = replay(Width::ALTERNATING, csv.is_open() ? &csv : nullptr);
    const auto seeded = replay(Width::SEEDED_ALTERNATING, csv.is_open() ? &csv : nullptr);
    std::cout << "fixed/alternating/seeded lead P50=" << fixed.lead << '/' << alternating.lead << '/' << seeded.lead
              << "; independent one-horizon=" << alternating.required << "; samples=" << alternating.samples << '\n';
    expect(fixed.lead >= fixed.required, "固定宽基准前探应覆盖一次真实帧龄加配置延迟");
    expect(alternating.lead >= alternating.required,
           "相同中心匀速只改变框宽往返，不应使前探长期低于一次独立时域位移");
    expect(std::abs(alternating.lead-seeded.lead) <= .25,
           "相同末段观测不应保留恒宽播种导致的长期预测幅度差异");
    const auto loop_fixed = replay(Width::FIXED, csv.is_open() ? &csv : nullptr, true);
    const auto loop_alternating = replay(Width::ALTERNATING, csv.is_open() ? &csv : nullptr, true);
    std::cout << "independent plant fixed/alternating P50=" << loop_fixed.lead << '/' << loop_alternating.lead << '\n';
    expect(loop_fixed.lead >= loop_fixed.required && loop_alternating.lead >= loop_alternating.required,
           "真实回执与独立延迟相机 plant 下，宽度变化不得使前探低于一次独立时域位移");
    for (auto kind : {Boundary::WIDTH_ONLY, Boundary::ONE_EDGE, Boundary::CAMERA_ONLY,
                      Boundary::STOP_REVERSE, Boundary::SOURCE_BREAK}) boundary_replay(kind);
    Log::shutdown();
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
