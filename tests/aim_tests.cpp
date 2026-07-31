#include "aim/aim.h"
#include "log/log.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

Detection body(float center_x, float center_y,
               float confidence = 0.9f) {
    return {center_x - 20.0f, center_y - 40.0f,
            center_x + 20.0f, center_y + 40.0f,
            confidence, 0};
}

Detection head(float center_x, float center_y,
               float confidence = 0.95f) {
    return {center_x - 7.0f, center_y - 7.0f,
            center_x + 7.0f, center_y + 7.0f,
            confidence, 1};
}

AimFrame make_frame(std::uint64_t sequence,
                    std::chrono::steady_clock::time_point time) {
    AimFrame frame;
    frame.sequence = sequence;
    frame.captured_at = time;
    frame.roi_width = 320;
    frame.roi_height = 320;
    return frame;
}

void test_invalid_input() {
    Aim aim(AimConfig{});
    const AimResult result = aim.process(AimFrame{});
    expect(result.status == AimStatus::INVALID_INPUT,
           "空 AimFrame 必须返回 INVALID_INPUT");
}

void test_head_body_merge_and_confirmation() {
    AimConfig config;
    config.min_confirmed_hits = 2;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now();

    AimFrame first = make_frame(1, base);
    first.detections = {body(220.0f, 170.0f), head(220.0f, 140.0f)};
    const AimResult first_result = aim.process(first);
    expect(first_result.status == AimStatus::SUCCESS,
           "首帧 Aim 处理应成功");
    expect(!first_result.has_target,
           "未达到确认命中数的轨迹不能成为目标");

    AimFrame second = make_frame(2, base + std::chrono::milliseconds(4));
    second.detections = {body(222.0f, 170.0f), head(222.0f, 140.0f)};
    const AimResult second_result = aim.process(second);
    expect(second_result.has_target,
           "连续两帧命中后应产生确认目标");
    expect(std::fabs(second_result.target.aim_x - 222.0f) < 0.1f &&
           std::fabs(second_result.target.aim_y - 140.0f) < 0.1f,
           "身体和头部框应归并，并使用头部中心作为瞄准点");
    expect(second_result.has_command,
           "目标超出死区时应产生移动命令");
}

void test_short_loss_keeps_track_id() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.max_lost_frames = 3;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now();

    AimFrame first = make_frame(1, base);
    first.detections = {body(210.0f, 160.0f)};
    const AimResult detected = aim.process(first);
    expect(detected.has_target, "单次命中配置下应立即确认轨迹");

    AimFrame lost = make_frame(2, base + std::chrono::milliseconds(4));
    const AimResult predicted = aim.process(lost);
    expect(predicted.has_target && predicted.target.predicted,
           "短时丢框应输出降权预测目标");
    expect(predicted.target.track_id == detected.target.track_id,
           "短时丢框不得更换 track_id");
}

void test_command_limit_and_reset() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.counts_per_pixel_x = 10.0f;
    config.counts_per_pixel_y = 10.0f;
    config.max_counts_per_frame = 12.0f;
    Aim aim(config);
    const auto now = std::chrono::steady_clock::now();
    AimFrame frame = make_frame(1, now);
    frame.detections = {body(300.0f, 300.0f)};
    const AimResult result = aim.process(frame);
    expect(result.has_command &&
           std::abs(result.command.dx_counts) <= 12 &&
           std::abs(result.command.dy_counts) <= 12,
           "AimCommand 必须执行单帧 counts 限幅");

    aim.reset();
    AimFrame empty = make_frame(2, now + std::chrono::milliseconds(4));
    const AimResult after_reset = aim.process(empty);
    expect(!after_reset.has_target && !after_reset.has_command,
           "reset 后不得复用旧目标或旧命令状态");
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);

    test_invalid_input();
    test_head_body_merge_and_confirmation();
    test_short_loss_keeps_track_id();
    test_command_limit_and_reset();

    Log::shutdown();
    if (failures != 0) {
        std::cerr << "Aim 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Aim 测试全部通过。\n";
    return 0;
}
