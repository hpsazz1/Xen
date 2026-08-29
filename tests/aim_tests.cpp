#include "aim/aim.h"
#include "aim/aim_prediction_internal.h"
#include "log/log.h"

#include "aim_fixed_scene_replay_fixture.h"
#include "aim_latest_physical_replay_fixture.h"
#include "aim_latest_static_replay_fixture.h"
#include "aim_superjump_actual_game_replay_fixture.h"
#include "aim_static_closed_loop_replay_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool contains_log_text(const std::vector<std::string>& lines,
                       const std::string& text) {
    return std::any_of(
        lines.begin(), lines.end(), [&text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
}

std::size_t count_log_text(const std::vector<std::string>& lines,
                           const std::string& text) {
    return static_cast<std::size_t>(std::count_if(
        lines.begin(), lines.end(), [&text](const std::string& line) {
            return line.find(text) != std::string::npos;
        }));
}

Detection body(float center_x, float center_y,
               float confidence = 0.9f) {
    return {center_x - 20.0f, center_y - 40.0f,
            center_x + 20.0f, center_y + 40.0f,
            confidence, 0};
}

Detection body_box(float center_x, float center_y,
                   float width, float height,
                   float confidence = 0.9f) {
    return {center_x - width * 0.5f, center_y - height * 0.5f,
            center_x + width * 0.5f, center_y + height * 0.5f,
            confidence, 0};
}

Detection head(float center_x, float center_y, float confidence = 0.95f) {
    return {center_x - 7.0f,
            center_y - 7.0f,
            center_x + 7.0f,
            center_y + 7.0f,
            confidence,
            1};
}

Detection head_box(float center_x,
                   float center_y,
                   float width,
                   float height,
                   float confidence = 0.95f) {
    return {center_x - width * 0.5f,
            center_y - height * 0.5f,
            center_x + width * 0.5f,
            center_y + height * 0.5f,
            confidence,
            1};
}

AimFrame make_frame(std::uint64_t sequence,
                    std::chrono::steady_clock::time_point time) {
    AimFrame frame;
    frame.sequence = sequence;
    frame.captured_at = time;
    frame.roi_width = 320;
    frame.roi_height = 320;
    frame.control_center_x = 160.0f;
    frame.control_center_y = 160.0f;
    return frame;
}

void test_status_transition_logs_are_limited() {
    Log::shutdown();
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = true;
    log_config.ringbuf_capacity = 32;
    Log::init(log_config);

    Aim aim(AimConfig{});
    const auto base = std::chrono::steady_clock::now();
    const AimFrame zero_sequence = make_frame(0, base);
    expect(aim.process(zero_sequence).status == AimStatus::INVALID_INPUT,
           "日志回归首个非法帧必须返回 INVALID_INPUT");
    expect(aim.process(zero_sequence).status == AimStatus::INVALID_INPUT,
           "日志回归重复非法帧必须保持 INVALID_INPUT");

    AimFrame first = make_frame(1, base);
    expect(aim.process(first).status == AimStatus::SUCCESS,
           "日志回归合法帧必须从 INVALID_INPUT 恢复");
    AimFrame second = make_frame(2, base + std::chrono::milliseconds(1));
    expect(aim.process(second).status == AimStatus::SUCCESS,
           "日志回归连续成功帧必须保持 SUCCESS");
    AimFrame duplicate = make_frame(
        2, base + std::chrono::milliseconds(2));
    expect(aim.process(duplicate).status == AimStatus::INVALID_INPUT,
           "日志回归重复序号必须重新进入 INVALID_INPUT");

    const auto lines = Log::get_ring_buffer();
    expect(contains_log_text(lines, "Aim 已初始化"),
           "Aim 初始化必须留下单次 INFO 证据");
    expect(count_log_text(
               lines, "Aim 状态变化: NOT_RUN -> INVALID_INPUT") == 1,
           "重复 INVALID_INPUT 不得重复写 WARN");
    expect(count_log_text(
               lines, "Aim 状态变化: INVALID_INPUT -> SUCCESS") == 1,
           "从非法输入恢复必须只写一次 INFO");
    expect(count_log_text(
               lines, "Aim 状态变化: SUCCESS -> INVALID_INPUT") == 1,
           "再次失败必须形成新的单次 WARN 状态转换");
    expect(contains_log_text(lines, "reason=sequence 为 0"),
           "INVALID_INPUT 日志必须包含可诊断原因");
    Log::shutdown();
}

void test_invalid_input() {
    Aim aim(AimConfig{});
    const AimResult result = aim.process(AimFrame{});
    expect(result.status == AimStatus::INVALID_INPUT,
           "空 AimFrame 必须返回 INVALID_INPUT");
}

void test_frame_order_contract() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now();
    AimFrame first = make_frame(1, base);
    first.control_at = base + std::chrono::milliseconds(10);
    first.detections = {body(180.0f, 160.0f)};
    expect(aim.process(first).status == AimStatus::SUCCESS,
           "首个有序帧必须处理成功");

    AimFrame duplicate = make_frame(1, base + std::chrono::milliseconds(1));
    expect(aim.process(duplicate).status == AimStatus::INVALID_INPUT,
           "重复帧序号必须拒绝，不能重复更新状态估计");
    AimFrame older_time = make_frame(2, base - std::chrono::milliseconds(1));
    expect(aim.process(older_time).status == AimStatus::INVALID_INPUT,
           "倒退的采集时间必须拒绝，不能用最小 dt 掩盖乱序");
    AimFrame invalid_control_time = make_frame(
        2, base + std::chrono::milliseconds(2));
    invalid_control_time.control_at =
        invalid_control_time.captured_at - std::chrono::milliseconds(1);
    expect(aim.process(invalid_control_time).status == AimStatus::INVALID_INPUT,
           "显式控制时刻早于采集时刻必须拒绝，不能产生负观测年龄");

    AimFrame older_control_time = make_frame(
        2, base + std::chrono::milliseconds(2));
    older_control_time.control_at = base + std::chrono::milliseconds(8);
    expect(aim.process(older_control_time).status == AimStatus::INVALID_INPUT,
           "倒退的控制时刻必须拒绝，不能破坏命令历史单调顺序");

    AimFrame second = make_frame(2, base + std::chrono::milliseconds(4));
    second.control_at = base + std::chrono::milliseconds(12);
    second.detections = {body(182.0f, 160.0f)};
    expect(aim.process(second).status == AimStatus::SUCCESS,
           "拒绝乱序帧后，后续合法帧仍应继续处理");
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
    expect(second_result.target.aim_x >= second_result.target.x1 &&
           second_result.target.aim_x <= second_result.target.x2 &&
           std::fabs(second_result.target.aim_y - 158.0f) < 0.1f,
           "身体和头部框应归并，但基础瞄点必须服从统一高度参数");
    expect(second_result.has_command,
           "目标超出死区时应产生移动命令");
}

void test_selected_target_exposes_only_current_matched_observation() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame detected = make_frame(1, base);
    detected.detections = {
        body_box(210.0f, 170.0f, 50.0f, 90.0f),
        head_box(210.0f, 140.0f, 16.0f, 18.0f)};
    const AimResult matched = aim.process(detected);
    expect(matched.has_target &&
               matched.target.matched_observation_valid &&
               std::fabs(matched.target.matched_observation_x1 - 185.0f) <
                   0.001f &&
               std::fabs(matched.target.matched_observation_y1 - 125.0f) <
                   0.001f &&
               std::fabs(matched.target.matched_observation_x2 - 235.0f) <
                   0.001f &&
               std::fabs(matched.target.matched_observation_y2 - 215.0f) <
                   0.001f &&
               !matched.target.matched_observation_head_only &&
               matched.target.matched_observation_aim_from_head,
           "公开目标快照必须精确保留当前关联的 body 框与 head 瞄点语义");

    AimFrame lost = make_frame(2, base + std::chrono::milliseconds(4));
    const AimResult predicted = aim.process(lost);
    expect(predicted.has_target && predicted.target.predicted &&
               !predicted.target.matched_observation_valid &&
               predicted.target.matched_observation_x1 == 0.0f &&
               predicted.target.matched_observation_y1 == 0.0f &&
               predicted.target.matched_observation_x2 == 0.0f &&
               predicted.target.matched_observation_y2 == 0.0f &&
               !predicted.target.matched_observation_head_only &&
               !predicted.target.matched_observation_aim_from_head,
           "预测续帧不得把上一帧 Observation 冒充为当前检测证据");
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
    expect(!predicted.has_command,
           "预测关闭时可以保留轨迹身份，但不得输出丢失轨迹控制命令");
}

void test_head_only_uses_parameterized_aim_region() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.body_aim_height_ratio = 0.25f;
    config.body_aim_range_percent = 40.0f;
    Aim aim(config);
    AimFrame frame = make_frame(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    frame.detections = {head(200.0f, 140.0f)};
    const AimResult result = aim.process(frame);
    expect(result.has_target && result.has_command &&
               std::fabs(result.target.base_aim_x - 200.0f) < 0.1f &&
               std::fabs(result.target.base_aim_y - 136.5f) < 0.1f &&
               result.target.base_aim_x >= result.target.x1 &&
               result.target.base_aim_x <= result.target.x2,
           "单独头框必须正常确认，并服从统一高度与范围参数");
}

void test_command_limit_and_reset() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.counts_per_pixel_x = 10.0f;
    config.counts_per_pixel_y = 10.0f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto now = std::chrono::steady_clock::now();
    AimFrame frame = make_frame(1, now);
    frame.detections = {body(300.0f, 300.0f)};
    const AimResult result = aim.process(frame);
    expect(result.has_command &&
           std::hypot(static_cast<float>(result.command.dx_counts),
                      static_cast<float>(result.command.dy_counts)) <= 12.0f,
           "AimCommand 必须执行二维向量单帧 counts 限幅");

    aim.reset();
    AimFrame empty = make_frame(2, now + std::chrono::milliseconds(4));
    const AimResult after_reset = aim.process(empty);
    expect(!after_reset.has_target && !after_reset.has_command,
           "reset 后不得复用旧目标或旧命令状态");
}

void test_source_pixel_scale_controls_mouse_counts() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 1000.0f;
    Aim local_aim(config);
    Aim network_aim(config);

    const auto now = std::chrono::steady_clock::now();
    AimFrame local_frame = make_frame(1, now);
    local_frame.detections = {body(180.0f, 160.0f)};
    const AimResult local_result = local_aim.process(local_frame);

    AimFrame network_frame = make_frame(1, now);
    network_frame.source_pixels_per_roi_pixel_x = 4.0f;
    network_frame.source_pixels_per_roi_pixel_y = 4.0f;
    network_frame.detections = {body(165.0f, 160.0f)};
    const AimResult network_result = network_aim.process(network_frame);
    expect(local_result.has_command && network_result.has_command &&
               local_result.command.dx_counts == 20 &&
               network_result.command.dx_counts == 20,
           "本机 20 个主机像素与辅机 5 个四倍缩放像素必须生成相同 counts");
}

void test_global_head_body_assignment() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto now = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame frame = make_frame(1, now);
    // 168 头框可属于两个人，125 头框只属于左侧人物。局部分配会先把 168
    // 给左侧人物并让右侧身体失去头框；全局分配应选择 125->左、168->右。
    frame.detections = {
        body_box(150.0f, 160.0f, 60.0f, 80.0f),
        body_box(180.0f, 160.0f, 40.0f, 80.0f),
        head(168.0f, 136.0f),
        head(125.0f, 136.0f)};
    const AimResult result = aim.process(frame);
    expect(result.has_target,
           "头身归并必须做全局一对一分配，基础点仍由身体框参数决定");
}

void test_head_body_normalized_aim_stays_stable() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame with_head = make_frame(1, base);
    with_head.detections = {
        body_box(180.0f, 170.0f, 80.0f, 120.0f),
        head(184.0f, 128.0f)};
    const AimResult first = aim.process(with_head);

    AimFrame body_only = make_frame(
        2, base + std::chrono::milliseconds(10));
    body_only.detections = {
        body_box(182.0f, 170.0f, 80.0f, 120.0f)};
    const AimResult second = aim.process(body_only);

    AimFrame body_only_again = make_frame(
        3, base + std::chrono::milliseconds(20));
    body_only_again.detections = {
        body_box(184.0f, 170.0f, 80.0f, 120.0f)};
    const AimResult third = aim.process(body_only_again);

    expect(first.has_target && second.has_target && third.has_target &&
               std::fabs(second.target.aim_y - first.target.aim_y) < 0.5f &&
               std::fabs(third.target.aim_y - first.target.aim_y) < 0.5f &&
               second.target.aim_x >= second.target.x1 &&
               second.target.aim_x <= second.target.x2 &&
               second.target.aim_y >= second.target.y1 &&
               second.target.aim_y <= second.target.y2 &&
               third.target.aim_x >= third.target.x1 &&
               third.target.aim_x <= third.target.x2 &&
               third.target.aim_y >= third.target.y1 &&
               third.target.aim_y <= third.target.y2,
           "头框连续缺失时必须保留身体框归一化瞄点，不能上下切回身体默认点");
}

void test_body_box_shape_jitter_does_not_move_stable_aim_point() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float maximum_settled_error = 0.0f;
    float settled_error_sum = 0.0f;
    int settled_frames = 0;

    for (int index = 0; index < 120; ++index) {
        const float phase = (index % 2) == 0 ? -1.0f : 1.0f;
        const float width = phase < 0.0f ? 34.0f : 46.0f;
        const float height = phase < 0.0f ? 84.0f : 100.0f;
        // 人物真实控制锚点固定在 (160,160)，检测框因步态在宽高、中心和
        // 上下边缘间交替；原始框内比例点会随模型外形抖动 ±2 px。
        const float observed_aim_x = 160.0f + phase * 2.0f;
        const float observed_aim_y = 160.0f + phase * 2.0f;
        const float center_y = observed_aim_y +
            height * (0.5f - config.body_aim_height_ratio);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 5));
        frame.detections = {
            body_box(observed_aim_x, center_y, width, height)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "身体框形变回归必须持续保留确认目标");
        if (index >= 20 && result.has_target) {
            const float error = std::hypot(
                result.target.base_aim_x - 160.0f,
                result.target.base_aim_y - 160.0f);
            maximum_settled_error = std::max(maximum_settled_error, error);
            settled_error_sum += error;
            ++settled_frames;
        }
    }

    const float mean_settled_error = settled_error_sum /
        static_cast<float>(settled_frames);
    expect(maximum_settled_error <= 0.80f &&
               mean_settled_error <= 0.60f,
           "人物外形导致检测框交替缩放时，稳定基础点不得跟随框形变抖动，均值=" +
               std::to_string(mean_settled_error) + "，最大=" +
               std::to_string(maximum_settled_error));
}

void test_body_box_shape_jitter_preserves_real_translation() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float settled_position_error_sum = 0.0f;
    float settled_velocity_error_sum = 0.0f;
    int settled_frames = 0;

    for (int index = 0; index < 160; ++index) {
        const float phase = (index % 2) == 0 ? -1.0f : 1.0f;
        const float true_aim_x = 100.0f + index * 0.6f;
        const float observed_aim_x = true_aim_x + phase * 2.0f;
        const float width = phase < 0.0f ? 34.0f : 46.0f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 5));
        frame.detections = {
            body_box(observed_aim_x, 175.0f, width, 100.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "带框形变的匀速目标必须持续保留确认轨迹");
        if (index >= 40 && result.has_target) {
            settled_position_error_sum += std::fabs(
                result.target.base_aim_x - true_aim_x);
            settled_velocity_error_sum += std::fabs(
                result.target.velocity_x - 120.0f);
            ++settled_frames;
        }
    }

    const float mean_position_error = settled_position_error_sum /
        static_cast<float>(settled_frames);
    const float mean_velocity_error = settled_velocity_error_sum /
        static_cast<float>(settled_frames);
    expect(mean_position_error <= 0.50f &&
               mean_velocity_error <= 20.0f,
           "去除人物框形变时不得丢失真实匀速平移，位置均值误差=" +
               std::to_string(mean_position_error) + "，速度均值误差=" +
               std::to_string(mean_velocity_error));
}

void test_coherent_box_center_jitter_does_not_move_base_anchor() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::vector<float> settled_base_x;
    int base_outside_frames = 0;

    for (int index = 0; index < 600; ++index) {
        // 人物真实控制锚点固定在 160；模型姿态却让左右边缘同向摆动。
        // 这种整框中心高频漂移会绕过“对边反向即形变”的旧判断，正是实机
        // Run 中基础点与框中心逐帧位移相关约 0.99 的未覆盖反例。
        const float phase = (index % 2) == 0 ? -1.0f : 1.0f;
        const float pose_jitter = phase * 1.50f;
        const float width = 42.0f + phase * 0.50f;
        const float height = 90.0f + phase * 0.60f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(index * 4167));
        frame.detections = {
            body_box(160.0f + pose_jitter, 175.0f, width, height)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "整框中心形变回归必须持续保留确认目标");
        if (!result.has_target) continue;
        if (result.target.base_aim_x < result.target.x1 ||
            result.target.base_aim_x > result.target.x2 ||
            result.target.base_aim_y < result.target.y1 ||
            result.target.base_aim_y > result.target.y2) {
            ++base_outside_frames;
        }
        if (index >= 100) {
            settled_base_x.push_back(result.target.base_aim_x);
        }
    }

    std::vector<float> position_errors;
    std::vector<float> second_differences;
    for (std::size_t index = 0; index < settled_base_x.size(); ++index) {
        position_errors.push_back(std::fabs(settled_base_x[index] - 160.0f));
        if (index >= 2) {
            second_differences.push_back(std::fabs(
                settled_base_x[index] - 2.0f * settled_base_x[index - 1] +
                settled_base_x[index - 2]));
        }
    }
    std::sort(position_errors.begin(), position_errors.end());
    std::sort(second_differences.begin(), second_differences.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        const std::size_t index = std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction));
        return values[index];
    };
    const float error_p95 = percentile(position_errors, 0.95f);
    const float second_p95 = percentile(second_differences, 0.95f);
    expect(error_p95 <= 0.50f && second_p95 <= 0.75f,
           "姿态导致整框同向漂移时基础锚点必须保持稳定，位置/二阶 P95=" +
               std::to_string(error_p95) + "/" +
               std::to_string(second_p95));
    expect(base_outside_frames == 0,
           "抑制整框中心形变后基础点仍必须逐帧位于身体框内");
}

void test_coherent_box_center_jitter_preserves_real_translation() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::vector<float> settled_base_x;
    float position_error_sum = 0.0f;
    float velocity_error_sum = 0.0f;
    int settled_frames = 0;

    for (int index = 0; index < 240; ++index) {
        const float phase = (index % 2) == 0 ? -1.0f : 1.0f;
        const float true_aim_x = 100.0f + index * 0.40f;
        const float pose_jitter = phase * 1.50f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(index * 4167));
        frame.detections = {body_box(
            true_aim_x + pose_jitter, 175.0f,
            42.0f + phase * 0.50f,
            90.0f + phase * 0.60f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "移动整框中心形变回归必须持续保留确认目标");
        if (index < 60 || !result.has_target) continue;
        settled_base_x.push_back(result.target.base_aim_x);
        position_error_sum += std::fabs(
            result.target.base_aim_x - true_aim_x);
        velocity_error_sum += std::fabs(
            result.target.velocity_x - 96.0f);
        ++settled_frames;
    }

    std::vector<float> second_differences;
    for (std::size_t index = 2; index < settled_base_x.size(); ++index) {
        second_differences.push_back(std::fabs(
            settled_base_x[index] - 2.0f * settled_base_x[index - 1] +
            settled_base_x[index - 2]));
    }
    std::sort(second_differences.begin(), second_differences.end());
    const std::size_t p95_index = std::min(
        second_differences.size() - 1,
        static_cast<std::size_t>(second_differences.size() * 0.95f));
    const float mean_position_error = position_error_sum /
        static_cast<float>(settled_frames);
    const float mean_velocity_error = velocity_error_sum /
        static_cast<float>(settled_frames);
    const float second_p95 = second_differences[p95_index];
    expect(mean_position_error <= 0.75f &&
               mean_velocity_error <= 20.0f && second_p95 <= 0.75f,
           "抑制同向框形变时必须保留真实匀速平移，位置/速度均值误差和二阶 P95=" +
               std::to_string(mean_position_error) + "/" +
               std::to_string(mean_velocity_error) + "/" +
               std::to_string(second_p95));
}

void test_multiframe_pose_deformation_does_not_move_base_anchor() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::vector<float> settled_base_x;

    for (int index = 0; index < 720; ++index) {
        // 实机相邻残差并非每帧正负交替：人物一个步态内会连续 3～10 帧
        // 向同一方向形变，随后才回摆。使用 20 帧三角波复现这种慢整框
        // 漂移；真实控制锚点保持在 160，宽高变化提供明确的姿态证据。
        const int phase_index = index % 20;
        const float phase = phase_index <= 10
            ? -1.0f + static_cast<float>(phase_index) * 0.20f
            : 1.0f - static_cast<float>(phase_index - 10) * 0.20f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(index * 4167));
        frame.detections = {body_box(
            160.0f + phase * 1.50f, 175.0f,
            42.0f + phase * 0.50f,
            90.0f + phase * 0.60f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "多帧姿态形变回归必须持续保留确认目标");
        if (index >= 120 && result.has_target) {
            settled_base_x.push_back(result.target.base_aim_x);
        }
    }

    std::vector<float> position_errors;
    std::vector<float> second_differences;
    for (std::size_t index = 0; index < settled_base_x.size(); ++index) {
        position_errors.push_back(std::fabs(settled_base_x[index] - 160.0f));
        if (index >= 2) {
            second_differences.push_back(std::fabs(
                settled_base_x[index] - 2.0f * settled_base_x[index - 1] +
                settled_base_x[index - 2]));
        }
    }
    std::sort(position_errors.begin(), position_errors.end());
    std::sort(second_differences.begin(), second_differences.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };
    const float error_p95 = percentile(position_errors, 0.95f);
    const float second_p95 = percentile(second_differences, 0.95f);
    expect(error_p95 <= 0.75f && second_p95 <= 0.35f,
           "连续多帧同向的姿态形变不得推动基础锚点，位置/二阶 P95=" +
               std::to_string(error_p95) + "/" +
               std::to_string(second_p95));
}

void test_long_pose_deformation_with_sparse_evidence_does_not_leak_into_anchor() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::vector<float> settled_base_x;

    for (int index = 0; index < 960; ++index) {
        // 实机最长形变段约 17 帧，且个别帧的两条边残差会短暂失去
        // 同向证据。中心和宽高仍来自同一姿态变化，真实锚点固定在 160。
        const int phase_index = index % 34;
        const float phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) * (2.0f / 17.0f);
        const float evidence_gap = phase_index % 11 == 5 ? 0.35f : 0.0f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(index * 4167));
        frame.detections = {body_box(
            160.0f + phase * 3.0f + evidence_gap,
            175.0f,
            42.0f + phase * 1.6f,
            90.0f + phase * 1.8f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "长段姿态形变回归必须持续保留确认目标");
        if (index >= 180 && result.has_target) {
            settled_base_x.push_back(result.target.base_aim_x);
        }
    }

    std::vector<float> position_errors;
    std::vector<float> second_differences;
    for (std::size_t index = 0; index < settled_base_x.size(); ++index) {
        position_errors.push_back(std::fabs(settled_base_x[index] - 160.0f));
        if (index >= 2) {
            second_differences.push_back(std::fabs(
                settled_base_x[index] - 2.0f * settled_base_x[index - 1] +
                settled_base_x[index - 2]));
        }
    }
    std::sort(position_errors.begin(), position_errors.end());
    std::sort(second_differences.begin(), second_differences.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };
    const float position_p95 = percentile(position_errors, 0.95f);
    const float second_p95 = percentile(second_differences, 0.95f);
    expect(position_p95 <= 0.75f && second_p95 <= 0.75f,
           "最长姿态形变且证据短缺时基础锚点不得被比例校正泄漏，位置/二阶 P95=" +
               std::to_string(position_p95) + "/" +
               std::to_string(second_p95));
}

void test_horizontal_pose_trend_is_speed_independent() {
    // 同一个 240 Hz 几何观察器覆盖静止、双向慢移和最新实测约
    // 412 px/s 的高速档；速度只改变拟合斜率，不参与任何分支选择。
    constexpr std::array<float, 9> kMotionPerFrame{
        0.0f, 0.02f, -0.02f, 0.20f, -0.20f,
        0.80f, -0.80f, 1.70f, -1.70f};
    constexpr std::array<int, 3> kPosePeriods{20, 34, 40};
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };

    for (const int pose_period : kPosePeriods) {
        for (const float motion_per_frame : kMotionPerFrame) {
            AimConfig config;
            config.min_confirmed_hits = 1;
            config.deadzone_pixels = 0.0f;
            config.acquisition_range_percent = 150.0f;
            Aim aim(config);
            std::vector<float> position_errors;
            std::vector<float> velocity_errors;
            std::vector<float> second_differences;
            std::vector<float> base_points;
            std::vector<float> all_base_points;
            int horizontal_boundary_frames = 0;
            float maximum_warmup_position_error = 0.0f;
            std::uint64_t track_id = 0;
            const float start_x = motion_per_frame > 0.0f
                ? 30.0f : motion_per_frame < 0.0f ? 290.0f : 160.0f;

            for (int index = 0; index < 150; ++index) {
                const int half_period = pose_period / 2;
                const int phase_index = index % pose_period;
                const float pose_phase = phase_index <= half_period
                    ? -1.0f + static_cast<float>(phase_index) *
                        (2.0f / static_cast<float>(half_period))
                    : 1.0f - static_cast<float>(
                        phase_index - half_period) *
                        (2.0f / static_cast<float>(half_period));
                const float true_x = start_x +
                    static_cast<float>(index) * motion_per_frame;
                AimFrame frame = make_frame(
                    static_cast<std::uint64_t>(index + 1),
                    base + std::chrono::microseconds(
                        static_cast<long long>(index) * 4167));
                // 全程只有身体框，确保测试真正覆盖旧 8 px/s fallback，
                // 而不是由理想头框绕过 body-only 路径。
                frame.detections = {body_box(
                    true_x + pose_phase * 3.0f, 175.0f,
                    42.0f + pose_phase * 1.6f,
                    90.0f + pose_phase * 1.8f)};
                const AimResult result = aim.process(frame);
                expect(result.status == AimStatus::SUCCESS &&
                           result.has_target,
                       "X 趋势速度无关回归必须逐帧保留确认目标");
                if (!result.has_target) continue;
                if (track_id == 0) track_id = result.target.track_id;
                expect(result.target.track_id == track_id,
                       "X 趋势不得因人物移速或方向改变轨迹身份");
                all_base_points.push_back(result.target.base_aim_x);
                if (index >= 4 && index < 80) {
                    maximum_warmup_position_error = std::max(
                        maximum_warmup_position_error,
                        std::fabs(result.target.base_aim_x - true_x));
                }
                const float width = result.target.x2 - result.target.x1;
                const float ratio = width > 0.0f
                    ? (result.target.base_aim_x - result.target.x1) / width
                    : -1.0f;
                if (std::fabs(ratio - 0.25f) <= 0.0001f ||
                    std::fabs(ratio - 0.75f) <= 0.0001f) {
                    ++horizontal_boundary_frames;
                }
                if (index < 80) continue;

                position_errors.push_back(std::fabs(
                    result.target.base_aim_x - true_x));
                velocity_errors.push_back(std::fabs(
                    result.target.velocity_x - motion_per_frame * 240.0f));
                base_points.push_back(result.target.base_aim_x);
            }

            for (std::size_t index = 2; index < base_points.size(); ++index) {
                second_differences.push_back(std::fabs(
                    base_points[index] - 2.0f * base_points[index - 1] +
                    base_points[index - 2]));
            }
            float activation_second_maximum = 0.0f;
            for (std::size_t index = 4;
                 index <= 60 && index < all_base_points.size(); ++index) {
                activation_second_maximum = std::max(
                    activation_second_maximum,
                    std::fabs(all_base_points[index] -
                        2.0f * all_base_points[index - 1] +
                        all_base_points[index - 2]));
            }
            std::sort(position_errors.begin(), position_errors.end());
            std::sort(velocity_errors.begin(), velocity_errors.end());
            std::sort(second_differences.begin(), second_differences.end());
            const float error_p95 = percentile(position_errors, 0.95f);
            const float velocity_error_p95 =
                percentile(velocity_errors, 0.95f);
            const float second_p95 = percentile(second_differences, 0.95f);
            expect(error_p95 <= 2.0f && velocity_error_p95 <= 12.0f &&
                       second_p95 <= 0.25f &&
                       maximum_warmup_position_error <= 4.0f &&
                       activation_second_maximum <= 2.0f &&
                       horizontal_boundary_frames == 0,
                   "X 趋势必须跨姿态周期、双向人物速度保持准确且不贴边，"
                   "周期/每帧位移/位置P95/速度P95/二阶P95/"
                   "预热最大误差/增长窗最大二阶/全程贴边帧=" +
                       std::to_string(pose_period) + "/" +
                       std::to_string(motion_per_frame) + "/" +
                       std::to_string(error_p95) + "/" +
                       std::to_string(velocity_error_p95) + "/" +
                       std::to_string(second_p95) + "/" +
                       std::to_string(maximum_warmup_position_error) + "/" +
                       std::to_string(activation_second_maximum) + "/" +
                       std::to_string(horizontal_boundary_frames));
        }
    }
}

void test_horizontal_pose_trend_uses_capture_time_across_head_and_delivery_gaps() {
    constexpr std::array<int, 8> kIntervalsMicroseconds{
        4167, 4167, 8334, 4167, 12501, 4167, 4167, 8334};
    constexpr float kVelocityPixelsPerSecond = 120.0f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::chrono::microseconds elapsed{};
    std::uint64_t sequence = 1;
    std::uint64_t track_id = 0;
    std::vector<float> position_errors;
    std::vector<float> velocity_errors;
    std::vector<float> base_points;
    int horizontal_boundary_frames = 0;

    for (int index = 0; index < 180; ++index) {
        const int interval = kIntervalsMicroseconds[
            static_cast<std::size_t>(index) % kIntervalsMicroseconds.size()];
        if (index > 0) {
            elapsed += std::chrono::microseconds(interval);
            sequence += static_cast<std::uint64_t>(
                std::max(1, static_cast<int>(std::lround(
                    static_cast<float>(interval) / 4167.0f))));
        }
        const float elapsed_seconds =
            static_cast<float>(elapsed.count()) / 1000000.0f;
        const float true_x = 40.0f +
            kVelocityPixelsPerSecond * elapsed_seconds;
        const int source_phase = static_cast<int>(sequence % 34);
        const float pose_phase = source_phase <= 17
            ? -1.0f + static_cast<float>(source_phase) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(source_phase - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(sequence, base + elapsed);
        frame.detections = {body_box(
            true_x + pose_phase * 3.0f, 175.0f,
            25.6f + pose_phase * 0.8f,
            70.0f + pose_phase * 1.8f)};
        // 交替覆盖单帧、8 帧及 70 帧无头段。头框只是原始语义层，
        // 其出现/消失不能重置身体中心趋势或制造基础点阶跃。
        const int head_phase = index % 120;
        const bool head_missing = head_phase == 23 ||
            (head_phase >= 50 && head_phase < 58) ||
            (index >= 90 && index < 160);
        if (!head_missing) frame.detections.push_back(head(true_x, 145.0f));

        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "头框/交付缺口回归必须始终由身体实体维持确认目标");
        if (!result.has_target) continue;
        if (track_id == 0) track_id = result.target.track_id;
        expect(result.target.track_id == track_id,
               "头框与交付缺口不得生成第二轨迹或切换实体");
        if (index < 80) continue;

        position_errors.push_back(std::fabs(
            result.target.base_aim_x - true_x));
        velocity_errors.push_back(std::fabs(
            result.target.velocity_x - kVelocityPixelsPerSecond));
        base_points.push_back(result.target.base_aim_x);
        const float width = result.target.x2 - result.target.x1;
        const float ratio = width > 0.0f
            ? (result.target.base_aim_x - result.target.x1) / width : -1.0f;
        if (std::fabs(ratio - 0.25f) <= 0.0001f ||
            std::fabs(ratio - 0.75f) <= 0.0001f) {
            ++horizontal_boundary_frames;
        }
    }

    std::vector<float> second_differences;
    for (std::size_t index = 2; index < base_points.size(); ++index) {
        second_differences.push_back(std::fabs(
            base_points[index] - 2.0f * base_points[index - 1] +
            base_points[index - 2]));
    }
    std::sort(position_errors.begin(), position_errors.end());
    std::sort(velocity_errors.begin(), velocity_errors.end());
    std::sort(second_differences.begin(), second_differences.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };
    const float error_p95 = percentile(position_errors, 0.95f);
    const float velocity_error_p95 = percentile(velocity_errors, 0.95f);
    const float second_p99 = percentile(second_differences, 0.99f);
    // 非等间隔交付下逐样本二阶包含 8～12 ms 合法时间跨度变化，故只约束
    // 其有界上限；位置和速度误差才是 timestamp 拟合的主验收量。
    expect(error_p95 <= 2.0f && velocity_error_p95 <= 15.0f &&
               second_p99 <= 1.25f && horizontal_boundary_frames == 0,
           "趋势必须按真实采集时间跨过交付/头框缺口，"
           "位置P95/速度P95/二阶P99/贴边帧=" +
               std::to_string(error_p95) + "/" +
               std::to_string(velocity_error_p95) + "/" +
               std::to_string(second_p99) + "/" +
               std::to_string(horizontal_boundary_frames));
}

void test_horizontal_pose_trend_reversal_is_bounded() {
    constexpr std::array<float, 3> kMotionPerFrame{0.20f, 0.80f, 1.70f};
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    for (const float speed_per_frame : kMotionPerFrame) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.acquisition_range_percent = 150.0f;
        Aim aim(config);
        float true_x = 40.0f;
        std::vector<float> recovered_errors;
        std::vector<float> base_points;
        int direction_changes = 0;
        int previous_direction = 0;
        int reversal_boundary_frames = 0;
        int wrong_direction_frames = 0;
        float maximum_reversal_error = 0.0f;

        for (int index = 0; index < 205; ++index) {
            const float motion_per_frame = index < 100
                ? speed_per_frame : -speed_per_frame;
            if (index > 0) true_x += motion_per_frame;
            const int phase_index = index % 34;
            const float pose_phase = phase_index <= 17
                ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
                : 1.0f - static_cast<float>(phase_index - 17) *
                    (2.0f / 17.0f);
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.detections = {body_box(
                true_x + pose_phase * 3.0f, 175.0f,
                25.6f + pose_phase * 0.8f,
                70.0f + pose_phase * 1.8f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "不同运动尺度的 X 趋势换向必须逐帧保留确认目标");
            if (!result.has_target) continue;

            if (index >= 100 && index < 156) {
                maximum_reversal_error = std::max(
                    maximum_reversal_error,
                    std::fabs(result.target.base_aim_x - true_x));
                const float width = result.target.x2 - result.target.x1;
                const float ratio = width > 0.0f
                    ? (result.target.base_aim_x - result.target.x1) / width
                    : -1.0f;
                if (std::fabs(ratio - 0.25f) <= 0.0001f ||
                    std::fabs(ratio - 0.75f) <= 0.0001f) {
                    ++reversal_boundary_frames;
                }
            }
            if (index >= 160) {
                recovered_errors.push_back(std::fabs(
                    result.target.base_aim_x - true_x));
            }
            if (index >= 80) {
                base_points.push_back(result.target.base_aim_x);
                if (base_points.size() >= 2) {
                    const float delta = base_points.back() -
                        base_points[base_points.size() - 2];
                    const int direction = delta > 0.10f ? 1 :
                        delta < -0.10f ? -1 : 0;
                    if (index >= 100 && index < 156 && direction > 0) {
                        ++wrong_direction_frames;
                    }
                    if (direction != 0) {
                        if (previous_direction != 0 &&
                            direction != previous_direction) {
                            ++direction_changes;
                        }
                        previous_direction = direction;
                    }
                }
            }
        }

        std::sort(recovered_errors.begin(), recovered_errors.end());
        const float recovered_p95 = recovered_errors[
            std::min(recovered_errors.size() - 1,
                     static_cast<std::size_t>(
                         recovered_errors.size() * 0.95f))];
        const float wrong_direction_distance =
            static_cast<float>(wrong_direction_frames) * speed_per_frame;
        expect(maximum_reversal_error <= 16.0f && recovered_p95 <= 2.0f &&
                   direction_changes <= 1 &&
                   reversal_boundary_frames == 0 &&
                   wrong_direction_distance <= 12.0f,
               "固定窗跨运动尺度换向时允许有限几何滞后但基础点不得贴到"
               "水平安全内窗边界或往返震荡，"
               "每帧位移/最大换向误差/恢复P95/方向变化/贴边帧/"
               "错向距离=" +
                   std::to_string(speed_per_frame) + "/" +
                   std::to_string(maximum_reversal_error) + "/" +
                   std::to_string(recovered_p95) + "/" +
                   std::to_string(direction_changes) + "/" +
                   std::to_string(reversal_boundary_frames) + "/" +
                   std::to_string(wrong_direction_distance));
    }
}

void test_horizontal_unsupported_prediction_does_not_stick_at_range_boundary() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    config.body_aim_range_percent = 50.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float true_x = 60.0f;
    int stop_boundary_frames = 0;
    int stop_boundary_run = 0;
    int maximum_stop_boundary_run = 0;
    float stop_total_variation = 0.0f;
    float previous_base_x = 0.0f;
    std::vector<float> stop_base_points;

    for (int index = 0; index < 220; ++index) {
        if (index > 0 && index < 100) true_x += 0.80f;
        if (index >= 160) true_x -= 0.80f;
        const int pose_sample = std::min(index, 99);
        const int phase_index = pose_sample % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.detections = {body_box(
            true_x + pose_phase * 3.0f, 175.0f,
            16.0f + pose_phase * 0.5f,
            70.0f + pose_phase * 1.8f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "水平移动转停的 hidden-inventory 回归必须逐帧保留目标");
        if (!result.has_target) continue;
        if (index >= 100 && index < 160) {
            const float width = result.target.x2 - result.target.x1;
            const float ratio = width > 0.0f
                ? (result.target.base_aim_x - result.target.x1) / width
                : -1.0f;
            const bool boundary =
                std::fabs(ratio - 0.25f) <= 0.0001f ||
                std::fabs(ratio - 0.75f) <= 0.0001f;
            if (boundary) {
                ++stop_boundary_frames;
                ++stop_boundary_run;
                maximum_stop_boundary_run = std::max(
                    maximum_stop_boundary_run, stop_boundary_run);
            } else {
                stop_boundary_run = 0;
            }
            if (previous_base_x != 0.0f) {
                stop_total_variation += std::fabs(
                    result.target.base_aim_x - previous_base_x);
            }
            previous_base_x = result.target.base_aim_x;
            stop_base_points.push_back(result.target.base_aim_x);
        }
    }
    float maximum_stop_step = 0.0f;
    std::vector<float> stop_second_differences;
    for (std::size_t index = 1; index < stop_base_points.size(); ++index) {
        maximum_stop_step = std::max(
            maximum_stop_step,
            std::fabs(stop_base_points[index] - stop_base_points[index - 1]));
        if (index >= 2) {
            stop_second_differences.push_back(std::fabs(
                stop_base_points[index] - 2.0f * stop_base_points[index - 1] +
                stop_base_points[index - 2]));
        }
    }
    std::sort(stop_second_differences.begin(), stop_second_differences.end());
    const float stop_second_p95 = stop_second_differences[
        std::min(stop_second_differences.size() - 1,
                 static_cast<std::size_t>(
                     stop_second_differences.size() * 0.95f))];
    expect(stop_boundary_frames <= 5 && maximum_stop_boundary_run <= 5 &&
               stop_total_variation <= 6.5f && maximum_stop_step <= 1.10f &&
               stop_second_p95 <= 0.30f,
           "连续观测不再支持旧 X 预测时，基础点不得在水平内窗边界"
           "长期驻留或以阶跃释放，贴边帧/最长贴边/总变差/最大步长/"
           "二阶P95=" +
               std::to_string(stop_boundary_frames) + "/" +
               std::to_string(maximum_stop_boundary_run) + "/" +
               std::to_string(stop_total_variation) + "/" +
               std::to_string(maximum_stop_step) + "/" +
               std::to_string(stop_second_p95));

    // 实机 20260825-132050 的 1157～1178 帧并非完全静止框：人物主体
    // 中心只剩远小于旧趋势的姿态位移，而宽高共同形变仍让 OLS 保护窗
    // 持续有效。旧用例把姿态也冻结，只能覆盖 predict_tracks() 的库存；
    // 本段要求基础点跟随当前共同边，不能继续消费失去观测支持的 OLS 外推。
    Aim active_pose_aim(config);
    float active_pose_true_x = 60.0f;
    int active_pose_boundary_frames = 0;
    int active_pose_boundary_run = 0;
    int maximum_active_pose_boundary_run = 0;
    float active_pose_relative_variation = 0.0f;
    float active_pose_maximum_relative_step = 0.0f;
    float active_pose_maximum_relative_error = 0.0f;
    float previous_active_pose_relative_error = 0.0f;
    bool active_pose_stop_started = false;
    for (int index = 0; index < 180; ++index) {
        if (index > 0 && index < 100) active_pose_true_x += 0.80f;
        const int phase_index = index % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        const float observed_center_x =
            active_pose_true_x + pose_phase * 0.21f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.detections = {body_box(
            observed_center_x, 175.0f,
            16.0f + pose_phase * 0.4f,
            70.0f + pose_phase * 1.8f)};
        const AimResult result = active_pose_aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "姿态仍活动的水平移动转停回归必须逐帧保留目标");
        if (!result.has_target || index < 100 || index >= 160) continue;
        const float width = result.target.x2 - result.target.x1;
        const float ratio = width > 0.0f
            ? (result.target.base_aim_x - result.target.x1) / width : -1.0f;
        const bool boundary =
            std::fabs(ratio - 0.25f) <= 0.0001f ||
            std::fabs(ratio - 0.75f) <= 0.0001f;
        if (boundary) {
            ++active_pose_boundary_frames;
            ++active_pose_boundary_run;
            maximum_active_pose_boundary_run = std::max(
                maximum_active_pose_boundary_run, active_pose_boundary_run);
        } else {
            active_pose_boundary_run = 0;
        }
        const float relative_error =
            result.target.base_aim_x - observed_center_x;
        active_pose_maximum_relative_error = std::max(
            active_pose_maximum_relative_error, std::fabs(relative_error));
        // 三次原始共同边和 15 ms 渐入最多占前四帧；总变差/阶跃从证据
        // 完整生效后统计，贴边和最大偏差仍覆盖整个转停区间。
        if (index >= 104 && active_pose_stop_started) {
            const float relative_step = std::fabs(
                relative_error - previous_active_pose_relative_error);
            active_pose_relative_variation += relative_step;
            active_pose_maximum_relative_step = std::max(
                active_pose_maximum_relative_step, relative_step);
        }
        previous_active_pose_relative_error = relative_error;
        active_pose_stop_started = index >= 103;
    }
    expect(active_pose_boundary_frames == 0 &&
               maximum_active_pose_boundary_run == 0 &&
               active_pose_relative_variation <= 4.0f &&
               active_pose_maximum_relative_step <= 0.35f &&
               active_pose_maximum_relative_error <= 2.30f,
           "宽高形变继续活动但共同边已不支持旧 X 趋势时，OLS 不得继续"
           "推动基础点，贴边帧/最长贴边/相对总变差/最大相对步长/"
           "最大相对误差=" +
               std::to_string(active_pose_boundary_frames) + "/" +
               std::to_string(maximum_active_pose_boundary_run) + "/" +
               std::to_string(active_pose_relative_variation) + "/" +
               std::to_string(active_pose_maximum_relative_step) + "/" +
               std::to_string(active_pose_maximum_relative_error));

    Aim reversal_aim(config);
    float reversal_true_x = 60.0f;
    int reversal_boundary_frames = 0;
    int reversal_boundary_run = 0;
    int maximum_reversal_boundary_run = 0;
    for (int index = 0; index < 150; ++index) {
        if (index > 0 && index < 100) reversal_true_x += 0.80f;
        if (index >= 100) reversal_true_x -= 0.80f;
        const int pose_sample = std::min(index, 99);
        const int phase_index = pose_sample % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.detections = {body_box(
            reversal_true_x + pose_phase * 3.0f, 175.0f,
            16.0f + pose_phase * 0.5f,
            70.0f + pose_phase * 1.8f)};
        const AimResult result = reversal_aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "水平窄目标立即换向的 visible-inventory 回归必须逐帧保留目标");
        if (!result.has_target || index < 100 || index >= 130) continue;
        const float width = result.target.x2 - result.target.x1;
        const float ratio = width > 0.0f
            ? (result.target.base_aim_x - result.target.x1) / width : -1.0f;
        const bool boundary =
            std::fabs(ratio - 0.25f) <= 0.0001f ||
            std::fabs(ratio - 0.75f) <= 0.0001f;
        if (boundary) {
            ++reversal_boundary_frames;
            ++reversal_boundary_run;
            maximum_reversal_boundary_run = std::max(
                maximum_reversal_boundary_run, reversal_boundary_run);
        } else {
            reversal_boundary_run = 0;
        }
    }
    expect(reversal_boundary_frames <= 4 &&
               maximum_reversal_boundary_run <= 4,
           "旧方向基础点库存已清晰可见且共同边确认反向时，不得等待趋势"
           "候选累计后才释放，换向贴边帧/最长贴边=" +
               std::to_string(reversal_boundary_frames) + "/" +
               std::to_string(maximum_reversal_boundary_run));
}

void test_horizontal_confirmed_release_does_not_snap_hidden_inventory() {
    // 实机 20260825-140403 的 1458～1473、4559～4575 帧均出现公开
    // 基础点在配置内窗边界驻留后突然穿回，单帧边界退出 P95 已增至
    // 18.55% 框宽。这里保留相同的“持续趋势→反向→小幅二次反向”和
    // 宽高姿态形变，只按框宽归一化验收，不引入任何速度分档。
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    config.body_aim_range_percent = 50.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float true_x = 80.0f;
    float previous_relative_ratio = 0.0f;
    bool have_previous_ratio = false;
    float maximum_boundary_exit_step_ratio = 0.0f;
    float maximum_boundary_exit_two_frame_ratio = 0.0f;
    std::array<float, 3> recent_relative_ratios{};
    std::size_t recent_ratio_count = 0;
    int boundary_side_crossings = 0;
    float previous_boundary_side = 0.0f;
    std::string release_trace;

    for (int index = 0; index < 150; ++index) {
        if (index > 0 && index < 100) {
            true_x += 0.80f;
        } else if (index >= 100 && index < 120) {
            true_x -= 5.50f;
        } else if (index >= 120) {
            true_x += 0.55f;
        }
        const int phase_index = index % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.detections = {body_box(
            true_x + pose_phase * 0.21f, 175.0f,
            17.0f + pose_phase * 0.6f,
            70.0f + pose_phase * 1.8f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "实机同构的 X 二次换向回归必须逐帧保留目标");
        if (!result.has_target || index < 96) continue;

        const float width = result.target.x2 - result.target.x1;
        const float center_x = (result.target.x1 + result.target.x2) * 0.5f;
        const float relative_ratio = width > 0.0f
            ? (result.target.base_aim_x - center_x) / width : 0.0f;
        if (have_previous_ratio &&
            std::fabs(previous_relative_ratio) >= 0.2499f &&
            std::fabs(relative_ratio) < 0.2499f) {
            maximum_boundary_exit_step_ratio = std::max(
                maximum_boundary_exit_step_ratio,
                std::fabs(relative_ratio - previous_relative_ratio));
        }
        recent_relative_ratios[recent_ratio_count % 3] = relative_ratio;
        ++recent_ratio_count;
        if (recent_ratio_count >= 3) {
            const float two_frames_ago =
                recent_relative_ratios[(recent_ratio_count - 3) % 3];
            if (std::fabs(two_frames_ago) >= 0.2499f &&
                std::fabs(relative_ratio) < 0.2499f) {
                maximum_boundary_exit_two_frame_ratio = std::max(
                    maximum_boundary_exit_two_frame_ratio,
                    std::fabs(relative_ratio - two_frames_ago));
            }
        }
        const float boundary_side = relative_ratio >= 0.2499f
            ? 1.0f : relative_ratio <= -0.2499f ? -1.0f : 0.0f;
        if (boundary_side != 0.0f) {
            if (previous_boundary_side != 0.0f &&
                boundary_side != previous_boundary_side) {
                ++boundary_side_crossings;
            }
            previous_boundary_side = boundary_side;
        }
        if (index >= 98 && index <= 136) {
            release_trace += std::to_string(index) + ":" +
                std::to_string(relative_ratio) + ",";
        }
        previous_relative_ratio = relative_ratio;
        have_previous_ratio = true;
    }

    expect(maximum_boundary_exit_step_ratio <= 0.05f &&
               maximum_boundary_exit_two_frame_ratio <= 0.12f &&
               boundary_side_crossings == 0,
           "已有共同边证据触发 X 库存释放时，不得让隐藏状态从一个"
           "公开内窗边界突释，单帧/两帧边界退出量/跨边次数=" +
               std::to_string(maximum_boundary_exit_step_ratio) + "/" +
               std::to_string(maximum_boundary_exit_two_frame_ratio) + "/" +
               std::to_string(boundary_side_crossings) + "，轨迹=" +
               release_trace);
}

void test_horizontal_persistent_innovation_keeps_velocity_and_delay_continuous() {
    struct Trace {
        std::vector<float> normalized_velocity;
        std::vector<float> normalized_delay;
        std::vector<float> normalized_position_error;
        int velocity_collapses = 0;
        int wrong_direction_frames = 0;
        float response_ratio_after_twelve_frames = 0.0f;
        float maximum_delay_step = 0.0f;
    };
    constexpr int kMotionChangeFrame = 70;
    constexpr float kInitialMotionPerFrame = 0.20f;
    // 320 ROI 下 7 px/frame 明确高于旧 2% ROI（6.4 px/frame）断点；
    // 该用例要求同一观察器仍按几何一致性追赶，而不是换游戏后被隐式限速。
    constexpr float kFastMotionPerFrame = 7.00f;
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto run_case = [&](float roi_scale) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = 0.425f;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 14.0f;
        config.acquisition_range_percent = 150.0f;
        config.enable_prediction = false;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 15.0f;
        Aim aim(config);
        Trace trace;
        float true_x = 40.0f * roi_scale;
        float previous_velocity = 0.0f;
        float previous_delay = 0.0f;
        bool have_previous = false;

        for (int index = 0; index < 110; ++index) {
            const float motion_per_frame =
                index < kMotionChangeFrame
                ? kInitialMotionPerFrame : kFastMotionPerFrame;
            if (index > 0) {
                true_x += motion_per_frame * roi_scale;
            }
            const int phase_index = index % 34;
            const float pose_phase = phase_index <= 17
                ? -1.0f + static_cast<float>(phase_index) *
                    (2.0f / 17.0f)
                : 1.0f - static_cast<float>(phase_index - 17) *
                    (2.0f / 17.0f);
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.roi_width = static_cast<int>(320.0f * roi_scale);
            frame.roi_height = static_cast<int>(320.0f * roi_scale);
            frame.source_pixels_per_roi_pixel_x = 1.0f / roi_scale;
            frame.source_pixels_per_roi_pixel_y = 1.0f / roi_scale;
            frame.control_center_x = 160.0f * roi_scale;
            frame.control_center_y = 160.0f * roi_scale;
            frame.lock_active = true;
            frame.detections = {body_box(
                true_x + pose_phase * 3.0f * roi_scale,
                175.0f * roi_scale,
                (42.0f + pose_phase * 1.6f) * roi_scale,
                (90.0f + pose_phase * 1.8f) * roi_scale)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "持续归一化创新回归必须逐帧保留确认目标");
            if (!result.has_target) continue;

            const float normalized_velocity =
                result.target.velocity_x / frame.roi_width;
            const float normalized_delay =
                result.target.delay_compensation_x / frame.roi_width;
            trace.normalized_velocity.push_back(normalized_velocity);
            trace.normalized_delay.push_back(normalized_delay);
            if (index >= kMotionChangeFrame) {
                trace.normalized_position_error.push_back(
                    std::fabs(result.target.base_aim_x - true_x) /
                    frame.roi_width);
                if (have_previous && previous_velocity > 0.0f &&
                    result.target.velocity_x >= 0.0f &&
                    result.target.velocity_x < previous_velocity * 0.25f) {
                    ++trace.velocity_collapses;
                }
                if (result.target.velocity_x < 0.0f) {
                    ++trace.wrong_direction_frames;
                }
            }
            if (index == kMotionChangeFrame + 12) {
                const float expected_velocity =
                    kFastMotionPerFrame * roi_scale * 240.0f;
                trace.response_ratio_after_twelve_frames =
                    result.target.velocity_x / expected_velocity;
            }
            if (have_previous) {
                trace.maximum_delay_step = std::max(
                    trace.maximum_delay_step,
                    std::fabs(result.target.delay_compensation_x -
                              previous_delay) / frame.roi_width);
            }
            previous_velocity = result.target.velocity_x;
            previous_delay = result.target.delay_compensation_x;
            have_previous = true;
        }
        return trace;
    };

    const Trace normal = run_case(1.0f);
    const Trace doubled = run_case(2.0f);
    expect(normal.normalized_velocity.size() ==
               doubled.normalized_velocity.size(),
           "320/640 ROI 轨迹必须生成等长归一化速度序列");
    float maximum_scale_velocity_delta = 0.0f;
    float maximum_scale_delay_delta = 0.0f;
    const std::size_t sample_count = std::min(
        normal.normalized_velocity.size(),
        doubled.normalized_velocity.size());
    for (std::size_t index = 0; index < sample_count; ++index) {
        maximum_scale_velocity_delta = std::max(
            maximum_scale_velocity_delta,
            std::fabs(normal.normalized_velocity[index] -
                      doubled.normalized_velocity[index]));
        maximum_scale_delay_delta = std::max(
            maximum_scale_delay_delta,
            std::fabs(normal.normalized_delay[index] -
                      doubled.normalized_delay[index]));
    }
    std::vector<float> sorted_position_errors =
        normal.normalized_position_error;
    std::sort(sorted_position_errors.begin(), sorted_position_errors.end());
    const float position_error_p95 = sorted_position_errors[
        std::min(sorted_position_errors.size() - 1,
                 static_cast<std::size_t>(
                     sorted_position_errors.size() * 0.95f))];
    expect(normal.velocity_collapses == 0 &&
               normal.wrong_direction_frames == 0 &&
               normal.response_ratio_after_twelve_frames >= 0.55f &&
               normal.maximum_delay_step <= 0.021f &&
               position_error_p95 <= 0.05f &&
               maximum_scale_velocity_delta <= 0.0001f &&
               maximum_scale_delay_delta <= 0.0002f,
           "持续同向创新不得触发 vx 硬清零，且 320/640 ROI 必须比例同构，"
           "塌缩/错向/12帧响应/延迟单步/位置P95/速度缩放差/延迟缩放差=" +
               std::to_string(normal.velocity_collapses) + "/" +
               std::to_string(normal.wrong_direction_frames) + "/" +
               std::to_string(normal.response_ratio_after_twelve_frames) +
               "/" + std::to_string(normal.maximum_delay_step) + "/" +
               std::to_string(position_error_p95) + "/" +
               std::to_string(maximum_scale_velocity_delta) + "/" +
               std::to_string(maximum_scale_delay_delta));
}

void test_horizontal_maneuver_accepts_coherent_second_reversal() {
    constexpr int kFirstManeuverFrame = 70;
    constexpr int kSecondManeuverFrame = 77;
    constexpr float kFastMotionPerFrame = 7.0f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    float true_x = 200.0f;
    int first_negative_velocity_frame = -1;
    int late_wrong_direction_frames = 0;
    std::vector<float> recovered_position_errors;
    std::string reversal_trace;

    for (int index = 0; index < 100; ++index) {
        const float motion_per_frame = index < kFirstManeuverFrame
            ? 0.20f
            : index < kSecondManeuverFrame
                ? kFastMotionPerFrame : -kFastMotionPerFrame;
        if (index > 0) true_x += motion_per_frame;
        const int phase_index = index % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.detections = {body_box(
            true_x + pose_phase * 3.0f, 175.0f,
            42.0f + pose_phase * 1.6f,
            90.0f + pose_phase * 1.8f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "共同边二次换向回归必须逐帧保留确认目标");
        if (!result.has_target || index < kSecondManeuverFrame) continue;

        if (index <= kSecondManeuverFrame + 12) {
            reversal_trace += std::to_string(index) + ":" +
                std::to_string(result.target.velocity_x) + ",";
        }

        if (first_negative_velocity_frame < 0 &&
            result.target.velocity_x < 0.0f) {
            first_negative_velocity_frame = index;
        }
        if (index >= kSecondManeuverFrame + 8 &&
            result.target.velocity_x > 0.0f) {
            ++late_wrong_direction_frames;
        }
        if (index >= kSecondManeuverFrame + 12) {
            recovered_position_errors.push_back(std::fabs(
                result.target.base_aim_x - true_x));
        }
    }

    std::sort(recovered_position_errors.begin(),
              recovered_position_errors.end());
    const float recovered_error_p95 = recovered_position_errors[
        std::min(recovered_position_errors.size() - 1,
                 static_cast<std::size_t>(
                     recovered_position_errors.size() * 0.95f))];
    expect(first_negative_velocity_frame >= kSecondManeuverFrame &&
               first_negative_velocity_frame <= kSecondManeuverFrame + 4 &&
               late_wrong_direction_frames == 0 &&
               recovered_error_p95 <= 14.0f,
           "确认后的十帧快通道必须按共同边一致性接受二次真实换向，"
           "首次负速度帧/迟到错向帧/恢复位置P95=" +
               std::to_string(first_negative_velocity_frame) + "/" +
               std::to_string(late_wrong_direction_frames) + "/" +
               std::to_string(recovered_error_p95) + "，轨迹=" +
               reversal_trace);
}

void test_horizontal_maneuver_rejects_short_coherent_center_outliers() {
    struct EventCase {
        int start_frame = 0;
        int frame_count = 0;
        const char* boundary = "";
    };
    struct Metrics {
        float maximum_position_delta_ratio = 0.0f;
        float maximum_velocity_delta_ratio_per_second = 0.0f;
        float maximum_delay_delta_ratio = 0.0f;
        float settled_velocity_delta_ratio_per_second = 0.0f;
        float settled_delay_delta_ratio = 0.0f;
        int identity_changes = 0;
        std::string maximum_position_context;
        std::string maximum_velocity_context;
    };
    // 70 帧起由 0.0625% ROI/frame 切为 2.1875% ROI/frame，并叠加与生产
    // 姿态回归相同的中心/宽高三角波，确保实际进入 horizontal maneuver。
    // 异常幅度、框几何和运动量都只用 ROI 比例表达。
    constexpr int kFastMotionStartFrame = 70;
    constexpr int kManeuverWindowFirstFrame = 75;
    constexpr int kManeuverWindowLastFrame = 81;
    constexpr float kSlowMotionRatioPerFrame = 0.000625f;
    constexpr float kFastMotionRatioPerFrame = 0.021875f;
    constexpr float kOutlierOffsetRatio = 0.025f;
    constexpr std::array<EventCase, 4> kEvents{{
        {kManeuverWindowFirstFrame + 1, 1, "首端单帧"},
        {kManeuverWindowFirstFrame + 1, 2, "首端双帧"},
        {kManeuverWindowLastFrame, 1, "末端单帧"},
        {kManeuverWindowLastFrame - 1, 2, "末端双帧"}}};
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);

    const auto run_case = [&](float roi_scale,
                              const EventCase& event,
                              float outlier_side) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.acquisition_range_percent = 150.0f;
        config.enable_prediction = false;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 15.0f;
        Aim aim(config);
        Aim reference_aim(config);
        Metrics metrics;
        const float roi_width = 320.0f * roi_scale;
        float true_x = roi_width * 0.125f;
        std::uint64_t track_id = 0;

        for (int index = 0; index < 96; ++index) {
            const float motion_ratio = index < kFastMotionStartFrame
                ? kSlowMotionRatioPerFrame : kFastMotionRatioPerFrame;
            if (index > 0) true_x += motion_ratio * roi_width;
            const bool event_active =
                index >= event.start_frame &&
                index < event.start_frame + event.frame_count;
            const int pose_phase_index = index % 34;
            const float pose_phase = pose_phase_index <= 17
                ? -1.0f + static_cast<float>(pose_phase_index) *
                    (2.0f / 17.0f)
                : 1.0f - static_cast<float>(pose_phase_index - 17) *
                    (2.0f / 17.0f);
            const float pose_center_x =
                pose_phase * 0.009375f * roi_width;
            const float observed_x = true_x + pose_center_x +
                (event_active
                     ? outlier_side * kOutlierOffsetRatio * roi_width
                     : 0.0f);
            const float observed_width =
                (0.13125f + pose_phase * 0.005f) * roi_width;
            const float observed_height =
                (0.28125f + pose_phase * 0.005625f) * roi_width;
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.roi_width = static_cast<int>(roi_width);
            frame.roi_height = static_cast<int>(roi_width);
            frame.control_center_x = roi_width * 0.5f;
            frame.control_center_y = roi_width * 0.5f;
            frame.detections = {body_box(
                observed_x, roi_width * 0.55f,
                observed_width, observed_height)};
            AimFrame reference_frame = frame;
            reference_frame.detections = {body_box(
                true_x + pose_center_x, roi_width * 0.55f,
                observed_width, observed_height)};
            const AimResult result = aim.process(frame);
            const AimResult reference_result =
                reference_aim.process(reference_frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target &&
                       reference_result.status == AimStatus::SUCCESS &&
                       reference_result.has_target,
                   "maneuver 窗内共同边短异常及固定框对照必须逐帧保留目标");
            if (!result.has_target || !reference_result.has_target) continue;
            if (track_id == 0) track_id = result.target.track_id;
            if (result.target.track_id != track_id) ++metrics.identity_changes;

            if (index < event.start_frame - 1 ||
                index > event.start_frame + event.frame_count + 8) {
                continue;
            }
            const float position_delta_ratio = std::fabs(
                result.target.base_aim_x -
                reference_result.target.base_aim_x) / roi_width;
            const float velocity_delta_ratio_per_second = std::fabs(
                result.target.velocity_x -
                reference_result.target.velocity_x) / roi_width;
            const float delay_delta_ratio = std::fabs(
                result.target.delay_compensation_x -
                reference_result.target.delay_compensation_x) / roi_width;
            if (position_delta_ratio >
                metrics.maximum_position_delta_ratio) {
                metrics.maximum_position_delta_ratio =
                    position_delta_ratio;
                metrics.maximum_position_context =
                    "frame=" + std::to_string(index) +
                    ",base=" + std::to_string(result.target.base_aim_x) +
                    ",ref_base=" +
                    std::to_string(reference_result.target.base_aim_x) +
                    ",box_center=" + std::to_string(
                        (result.target.x1 + result.target.x2) * 0.5f) +
                    ",ref_box_center=" + std::to_string(
                        (reference_result.target.x1 +
                         reference_result.target.x2) * 0.5f);
            }
            if (velocity_delta_ratio_per_second >
                metrics.maximum_velocity_delta_ratio_per_second) {
                metrics.maximum_velocity_delta_ratio_per_second =
                    velocity_delta_ratio_per_second;
                metrics.maximum_velocity_context =
                    "frame=" + std::to_string(index) +
                    ",event=" + event.boundary +
                    ",count=" + std::to_string(event.frame_count) +
                    ",side=" + std::to_string(outlier_side) +
                    ",vx=" + std::to_string(result.target.velocity_x) +
                    ",ref_vx=" +
                    std::to_string(reference_result.target.velocity_x) +
                    ",delay=" +
                    std::to_string(result.target.delay_compensation_x) +
                    ",ref_delay=" + std::to_string(
                        reference_result.target.delay_compensation_x);
            }
            metrics.maximum_delay_delta_ratio = std::max(
                metrics.maximum_delay_delta_ratio, delay_delta_ratio);
            if (index == event.start_frame + event.frame_count + 8) {
                metrics.settled_velocity_delta_ratio_per_second =
                    velocity_delta_ratio_per_second;
                metrics.settled_delay_delta_ratio = delay_delta_ratio;
            }
        }
        return metrics;
    };

    for (const EventCase& event : kEvents) {
        for (const float outlier_side : {-1.0f, 1.0f}) {
            const Metrics normal = run_case(1.0f, event, outlier_side);
            const Metrics doubled = run_case(2.0f, event, outlier_side);
            const float scale_velocity_delta = std::fabs(
                normal.maximum_velocity_delta_ratio_per_second -
                doubled.maximum_velocity_delta_ratio_per_second);
            const float scale_delay_delta = std::fabs(
                normal.maximum_delay_delta_ratio -
                doubled.maximum_delay_delta_ratio);
            expect(
                normal.identity_changes == 0 &&
                    doubled.identity_changes == 0 &&
                    normal.maximum_position_delta_ratio <= 0.010f &&
                    normal.maximum_velocity_delta_ratio_per_second <= 0.25f &&
                    normal.maximum_delay_delta_ratio <= 0.006f &&
                    normal.settled_velocity_delta_ratio_per_second <= 0.10f &&
                    normal.settled_delay_delta_ratio <= 0.003f &&
                    doubled.maximum_position_delta_ratio <= 0.010f &&
                    doubled.maximum_velocity_delta_ratio_per_second <= 0.25f &&
                    doubled.maximum_delay_delta_ratio <= 0.006f &&
                    doubled.settled_velocity_delta_ratio_per_second <= 0.10f &&
                    doubled.settled_delay_delta_ratio <= 0.003f &&
                    scale_velocity_delta <= 0.002f &&
                    scale_delay_delta <= 0.0002f,
                "已确认 maneuver 的首末边界内，单/双帧两边共同中心异常"
                "不得重播种 vx 或制造延迟脉冲，且 320/640 ROI 必须比例同构；"
                "边界/帧数/side/320位置/速度/延迟/收敛速度/收敛延迟/"
                "640位置/速度/延迟/收敛速度/收敛延迟/缩放差=" +
                    std::string(event.boundary) + "/" +
                    std::to_string(event.frame_count) + "/" +
                    std::to_string(outlier_side) + "/" +
                    std::to_string(normal.maximum_position_delta_ratio) + "/" +
                    std::to_string(
                        normal.maximum_velocity_delta_ratio_per_second) + "/" +
                    std::to_string(normal.maximum_delay_delta_ratio) + "/" +
                    std::to_string(
                        normal.settled_velocity_delta_ratio_per_second) + "/" +
                    std::to_string(normal.settled_delay_delta_ratio) + "/" +
                    std::to_string(doubled.maximum_position_delta_ratio) + "/" +
                    std::to_string(
                        doubled.maximum_velocity_delta_ratio_per_second) + "/" +
                    std::to_string(doubled.maximum_delay_delta_ratio) + "/" +
                    std::to_string(
                        doubled.settled_velocity_delta_ratio_per_second) + "/" +
                    std::to_string(doubled.settled_delay_delta_ratio) + "/" +
                    std::to_string(scale_velocity_delta) + "/" +
                    std::to_string(scale_delay_delta) +
                    "，320最坏位置=" + normal.maximum_position_context +
                    "，320最坏速度=" + normal.maximum_velocity_context +
                    "，640最坏位置=" + doubled.maximum_position_context +
                    "，640最坏速度=" + doubled.maximum_velocity_context);
        }
    }
}

void test_horizontal_pose_trend_recovers_after_body_semantic_loss() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.max_lost_frames = 3;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::uint64_t track_id = 0;
    float maximum_recovery_error = 0.0f;
    int recovery_boundary_frames = 0;

    for (int index = 0; index < 180; ++index) {
        const float true_x = 40.0f + static_cast<float>(index) * 0.80f;
        const int phase_index = index % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        const bool empty_frame = index == 70;
        const bool head_only_frame = index >= 100 && index < 103;
        if (head_only_frame) {
            frame.detections = {head(true_x, 145.0f)};
        } else if (!empty_frame) {
            frame.detections = {body_box(
                true_x + pose_phase * 3.0f, 175.0f,
                42.0f + pose_phase * 1.6f,
                90.0f + pose_phase * 1.8f)};
        }

        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "身体丢失/语义切换期间必须保留原确认轨迹");
        if (!result.has_target) continue;
        if (track_id == 0) track_id = result.target.track_id;
        expect(result.target.track_id == track_id,
               "空帧、head-only 与身体恢复不得更换轨迹身份");

        const bool recovery_body_frame =
            (!empty_frame && !head_only_frame) &&
            ((index >= 71 && index < 100) || index >= 103);
        if (!recovery_body_frame) continue;
        maximum_recovery_error = std::max(
            maximum_recovery_error,
            std::fabs(result.target.base_aim_x - true_x));
        const float width = result.target.x2 - result.target.x1;
        const float ratio = width > 0.0f
            ? (result.target.base_aim_x - result.target.x1) / width : -1.0f;
        if (std::fabs(ratio - 0.25f) <= 0.0001f ||
            std::fabs(ratio - 0.75f) <= 0.0001f) {
            ++recovery_boundary_frames;
        }
    }

    expect(maximum_recovery_error <= 4.0f && recovery_boundary_frames == 0,
           "身体趋势重置后必须用增长窗及时重暖机，最大误差/贴边帧=" +
               std::to_string(maximum_recovery_error) + "/" +
               std::to_string(recovery_boundary_frames));
}

void test_horizontal_pose_trend_bounds_sparse_center_outliers() {
    struct OutlierPattern {
        std::array<float, 3> offsets;
        int half_width_frames;
        float half_height_offset;
    };
    constexpr std::array<OutlierPattern, 11> kPatterns{
        {{{{8.0f, 0.0f, 0.0f}}, 0, 0.0f},
         {{{8.0f, 8.0f, 0.0f}}, 0, 0.0f},
         {{{-8.0f, -8.0f, 0.0f}}, 0, 0.0f},
         {{{8.0f, -8.0f, 0.0f}}, 0, 0.0f},
         {{{8.0f, 0.0f, 8.0f}}, 0, 0.0f},
         // 左右单侧裁切各覆盖一帧和两帧；偏移等于丢失半宽的一半。
         {{{6.4f, 0.0f, 0.0f}}, 1, 0.0f},
         {{{6.4f, 6.4f, 0.0f}}, 2, 0.0f},
         {{{-6.4f, 0.0f, 0.0f}}, 1, 0.0f},
         {{{-6.4f, -6.4f, 0.0f}}, 2, 0.0f},
         // 上/下半身只改变 Y 几何，不得触发 X 的单侧截断保护。
         {{{0.0f, 0.0f, 0.0f}}, 0, -17.5f},
         {{{0.0f, 0.0f, 0.0f}}, 0, 17.5f}}};
    constexpr std::array<int, 2> kOutlierStartFrames{20, 90};
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::vector<float> position_errors;
    std::vector<float> velocity_errors;
    std::vector<float> second_differences;
    int delayed_boundary_frames = 0;
    int partial_box_edge_frames = 0;
    float maximum_outlier_position_error = 0.0f;
    float maximum_outlier_velocity_error = 0.0f;
    float maximum_warmup_position_delta = 0.0f;
    float maximum_warmup_velocity_delta = 0.0f;
    float maximum_event_position_delta = 0.0f;
    float maximum_event_velocity_delta = 0.0f;
    std::string maximum_event_velocity_context;
    float maximum_event_second_difference = 0.0f;
    float maximum_vertical_position_delta = 0.0f;
    float maximum_vertical_velocity_delta = 0.0f;
    float maximum_vertical_second_difference_delta = 0.0f;

    for (int phase_offset = 0; phase_offset < 34; ++phase_offset) {
        for (const OutlierPattern& pattern : kPatterns) {
            for (const int outlier_start : kOutlierStartFrames) {
                AimConfig config;
                config.min_confirmed_hits = 1;
                config.deadzone_pixels = 0.0f;
                config.acquisition_range_percent = 150.0f;
                Aim aim(config);
                Aim reference_aim(config);
                std::vector<float> scenario_base_points;
                std::vector<float> all_base_points;
                std::vector<float> all_reference_base_points;
                std::uint64_t track_id = 0;

                for (int index = 0; index < 150; ++index) {
                    const float true_x =
                        70.0f + static_cast<float>(index) * 0.20f;
                    const int phase_index = (index + phase_offset) % 34;
                    const float pose_phase =
                        phase_index <= 17
                            ? -1.0f + static_cast<float>(phase_index) *
                                          (2.0f / 17.0f)
                            : 1.0f - static_cast<float>(phase_index - 17) *
                                         (2.0f / 17.0f);
                    const float sparse_outlier =
                        index >= outlier_start && index <= outlier_start + 2
                            ? pattern.offsets[static_cast<std::size_t>(
                                  index - outlier_start)]
                            : 0.0f;
                    const bool outlier_window =
                        index >= outlier_start && index <= outlier_start + 2;
                    const bool horizontal_partial_frame =
                        pattern.half_width_frames > 0 &&
                        index >= outlier_start &&
                        index < outlier_start + pattern.half_width_frames;
                    const bool vertical_partial_frame =
                        outlier_window && pattern.half_height_offset != 0.0f;
                    const bool vertical_comparison_window =
                        pattern.half_height_offset != 0.0f &&
                        index >= outlier_start - 2 &&
                        index <= outlier_start + 12;
                    const float normal_width = 25.6f + pose_phase * 0.8f;
                    const float observed_width =
                        horizontal_partial_frame ? 12.8f : normal_width;
                    const float observed_height =
                        vertical_partial_frame ? 35.0f
                                               : 70.0f + pose_phase * 1.8f;
                    const float observed_center_y =
                        vertical_partial_frame
                            ? 175.0f + pattern.half_height_offset
                            : 175.0f;
                    AimFrame frame = make_frame(
                        static_cast<std::uint64_t>(index + 1),
                        base + std::chrono::microseconds(
                                   static_cast<long long>(index) * 4167));
                    frame.detections = {
                        body_box(true_x + pose_phase * 3.0f + sparse_outlier,
                                 observed_center_y,
                                 observed_width,
                                 observed_height)};
                    AimFrame reference_frame = frame;
                    reference_frame.detections = {
                        body_box(true_x + pose_phase * 3.0f,
                                 175.0f,
                                 normal_width,
                                 70.0f + pose_phase * 1.8f)};
                    const AimResult result = aim.process(frame);
                    const AimResult reference_result =
                        reference_aim.process(reference_frame);
                    expect(result.status == AimStatus::SUCCESS &&
                               result.has_target &&
                               reference_result.status == AimStatus::SUCCESS &&
                               reference_result.has_target,
                           "各姿态相位的稀疏框中心异常必须保留确认轨迹");
                    if (!result.has_target || !reference_result.has_target) {
                        continue;
                    }
                    if (track_id == 0) track_id = result.target.track_id;
                    expect(result.target.track_id == track_id,
                           "稀疏框中心异常不得切换轨迹身份");
                    const float position_error =
                        std::fabs(result.target.base_aim_x - true_x);
                    const float velocity_error =
                        std::fabs(result.target.velocity_x - 48.0f);
                    const float reference_position_delta =
                        std::fabs(result.target.base_aim_x -
                                  reference_result.target.base_aim_x);
                    const float reference_velocity_delta =
                        std::fabs(result.target.velocity_x -
                                  reference_result.target.velocity_x);
                    if (index >= outlier_start - 2 &&
                        index <= outlier_start + 12) {
                        maximum_event_position_delta =
                            std::max(maximum_event_position_delta,
                                     reference_position_delta);
                        if (reference_velocity_delta >
                            maximum_event_velocity_delta) {
                            maximum_event_velocity_delta =
                                reference_velocity_delta;
                            maximum_event_velocity_context =
                                "phase=" + std::to_string(phase_offset) +
                                ",start=" + std::to_string(outlier_start) +
                                ",frame=" + std::to_string(index) +
                                ",offsets=" +
                                std::to_string(pattern.offsets[0]) + "," +
                                std::to_string(pattern.offsets[1]) + "," +
                                std::to_string(pattern.offsets[2]) +
                                ",half=" +
                                std::to_string(pattern.half_width_frames) +
                                ",vx=" +
                                std::to_string(result.target.velocity_x) +
                                ",ref_vx=" + std::to_string(
                                    reference_result.target.velocity_x);
                        }
                    }
                    if (vertical_comparison_window) {
                        maximum_vertical_position_delta =
                            std::max(maximum_vertical_position_delta,
                                     reference_position_delta);
                        maximum_vertical_velocity_delta =
                            std::max(maximum_vertical_velocity_delta,
                                     reference_velocity_delta);
                    }
                    if (index >= outlier_start - 2 &&
                        index <= outlier_start + 12) {
                        if (outlier_start < 70) {
                            maximum_warmup_position_delta =
                                std::max(maximum_warmup_position_delta,
                                         reference_position_delta);
                            maximum_warmup_velocity_delta =
                                std::max(maximum_warmup_velocity_delta,
                                         reference_velocity_delta);
                        } else {
                            maximum_outlier_position_error = std::max(
                                maximum_outlier_position_error, position_error);
                            maximum_outlier_velocity_error = std::max(
                                maximum_outlier_velocity_error, velocity_error);
                        }
                    }
                    if (index >= outlier_start + 3 &&
                        index <= outlier_start + 12) {
                        const float width = result.target.x2 - result.target.x1;
                        const float ratio = width > 0.0f
                                                ? (result.target.base_aim_x -
                                                   result.target.x1) /
                                                      width
                                                : -1.0f;
                        if (ratio <= 0.30f || ratio >= 0.70f) {
                            ++delayed_boundary_frames;
                        }
                    }
                    if (index >= outlier_start && index <= outlier_start + 2) {
                        const float width = result.target.x2 - result.target.x1;
                        const float ratio = width > 0.0f
                                                ? (result.target.base_aim_x -
                                                   result.target.x1) /
                                                      width
                                                : -1.0f;
                        if (ratio <= 0.10f || ratio >= 0.90f) {
                            ++partial_box_edge_frames;
                        }
                    }
                    all_base_points.push_back(result.target.base_aim_x);
                    all_reference_base_points.push_back(
                        reference_result.target.base_aim_x);
                    if (all_base_points.size() >= 3 && index >= outlier_start &&
                        index <= outlier_start + 12) {
                        const std::size_t size = all_base_points.size();
                        const float second_difference =
                            all_base_points[size - 1] -
                            2.0f * all_base_points[size - 2] +
                            all_base_points[size - 3];
                        const float reference_second_difference =
                            all_reference_base_points[size - 1] -
                            2.0f * all_reference_base_points[size - 2] +
                            all_reference_base_points[size - 3];
                        maximum_event_second_difference =
                            std::max(maximum_event_second_difference,
                                     std::fabs(second_difference));
                        if (vertical_comparison_window) {
                            maximum_vertical_second_difference_delta = std::max(
                                maximum_vertical_second_difference_delta,
                                std::fabs(second_difference -
                                          reference_second_difference));
                        }
                    }
                    if (index < 70) continue;
                    position_errors.push_back(position_error);
                    velocity_errors.push_back(velocity_error);
                    scenario_base_points.push_back(result.target.base_aim_x);
                }
                for (std::size_t index = 2; index < scenario_base_points.size();
                     ++index) {
                    second_differences.push_back(
                        std::fabs(scenario_base_points[index] -
                                  2.0f * scenario_base_points[index - 1] +
                                  scenario_base_points[index - 2]));
                }
            }
        }
    }

    std::sort(position_errors.begin(), position_errors.end());
    std::sort(velocity_errors.begin(), velocity_errors.end());
    std::sort(second_differences.begin(), second_differences.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };
    const float error_p95 = percentile(position_errors, 0.95f);
    const float velocity_error_p95 = percentile(velocity_errors, 0.95f);
    const float second_p99 = percentile(second_differences, 0.99f);
    expect(error_p95 <= 2.0f && velocity_error_p95 <= 12.0f &&
               second_p99 <= 1.6f && maximum_outlier_position_error <= 5.0f &&
               maximum_outlier_velocity_error <= 24.0f &&
               maximum_warmup_position_delta <= 4.1f &&
               maximum_warmup_velocity_delta <= 24.0f &&
               maximum_event_position_delta <= 4.1f &&
               maximum_event_velocity_delta <= 24.0f &&
               maximum_event_second_difference <= 6.0f &&
               maximum_vertical_position_delta <= 1.0f &&
               maximum_vertical_velocity_delta <= 8.0f &&
               maximum_vertical_second_difference_delta <= 1.0f &&
               delayed_boundary_frames == 0 && partial_box_edge_frames == 0,
           "半身框各姿态相位的单/双帧中心异常不得污染中心比例趋势，"
           "位置P95/速度P95/二阶P99/异常邻域最大位置误差/"
           "最大速度误差/增长窗最大位置差/最大速度差/"
           "事件相对对照最大位置差/速度差/最大二阶/"
           "上下半身相对对照最大位置差/速度差/二阶差/"
           "恢复段近内窗/异常段近全框边缘=" +
               std::to_string(error_p95) + "/" +
               std::to_string(velocity_error_p95) + "/" +
               std::to_string(second_p99) + "/" +
               std::to_string(maximum_outlier_position_error) + "/" +
               std::to_string(maximum_outlier_velocity_error) + "/" +
               std::to_string(maximum_warmup_position_delta) + "/" +
               std::to_string(maximum_warmup_velocity_delta) + "/" +
               std::to_string(maximum_event_position_delta) + "/" +
               std::to_string(maximum_event_velocity_delta) + "/" +
               std::to_string(maximum_event_second_difference) + "/" +
               std::to_string(maximum_vertical_position_delta) + "/" +
               std::to_string(maximum_vertical_velocity_delta) + "/" +
               std::to_string(maximum_vertical_second_difference_delta) + "/" +
               std::to_string(delayed_boundary_frames) + "/" +
               std::to_string(partial_box_edge_frames) +
               "，最坏速度事件=" + maximum_event_velocity_context);
}

void test_horizontal_partial_visibility_isolates_small_transients() {
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const float normal_width : {12.0f, 16.0f, 25.6f}) {
        for (const float side : {-1.0f, 1.0f}) {
            for (const int partial_frames : {1, 2}) {
                AimConfig config;
                config.min_confirmed_hits = 1;
                config.deadzone_pixels = 0.0f;
                config.acquisition_range_percent = 150.0f;
                Aim aim(config);
                Aim reference_aim(config);
                std::uint64_t track_id = 0;
                float minimum_event_width = normal_width;
                float recovered_width = 0.0f;
                float maximum_position_delta = 0.0f;
                float maximum_velocity_delta = 0.0f;
                float maximum_second_difference_delta = 0.0f;
                std::vector<float> base_points;
                std::vector<float> reference_base_points;

                for (int index = 0; index < 100; ++index) {
                    const float true_x =
                        70.0f + static_cast<float>(index) * 0.20f;
                    const bool partial =
                        index >= 60 && index < 60 + partial_frames;
                    AimFrame frame = make_frame(
                        static_cast<std::uint64_t>(index + 1),
                        base + std::chrono::microseconds(
                                   static_cast<long long>(index) * 4167));
                    frame.detections = {body_box(
                        true_x + (partial ? side * normal_width * 0.25f : 0.0f),
                        175.0f,
                        partial ? normal_width * 0.5f : normal_width,
                        70.0f)};
                    AimFrame reference_frame = frame;
                    reference_frame.detections = {
                        body_box(true_x, 175.0f, normal_width, 70.0f)};
                    const AimResult result = aim.process(frame);
                    const AimResult reference_result =
                        reference_aim.process(reference_frame);
                    expect(result.status == AimStatus::SUCCESS &&
                               result.has_target &&
                               reference_result.status == AimStatus::SUCCESS &&
                               reference_result.has_target,
                           "小框短时单侧截断及完整框对照必须逐帧保留目标");
                    if (!result.has_target || !reference_result.has_target) {
                        continue;
                    }
                    if (track_id == 0) track_id = result.target.track_id;
                    expect(result.target.track_id == track_id,
                           "小框单/双帧截断不得切换轨迹身份");
                    if (index >= 58 && index <= 70) {
                        maximum_position_delta = std::max(
                            maximum_position_delta,
                            std::fabs(result.target.base_aim_x -
                                      reference_result.target.base_aim_x));
                        maximum_velocity_delta = std::max(
                            maximum_velocity_delta,
                            std::fabs(result.target.velocity_x -
                                      reference_result.target.velocity_x));
                    }
                    if (partial) {
                        minimum_event_width =
                            std::min(minimum_event_width,
                                     result.target.x2 - result.target.x1);
                    }
                    if (index == 70) {
                        recovered_width = result.target.x2 - result.target.x1;
                    }
                    base_points.push_back(result.target.base_aim_x);
                    reference_base_points.push_back(
                        reference_result.target.base_aim_x);
                    if (base_points.size() >= 3 && index >= 58 && index <= 70) {
                        const std::size_t size = base_points.size();
                        const float second_difference =
                            base_points[size - 1] -
                            2.0f * base_points[size - 2] +
                            base_points[size - 3];
                        const float reference_second_difference =
                            reference_base_points[size - 1] -
                            2.0f * reference_base_points[size - 2] +
                            reference_base_points[size - 3];
                        maximum_second_difference_delta =
                            std::max(maximum_second_difference_delta,
                                     std::fabs(second_difference -
                                               reference_second_difference));
                    }
                }

                expect(minimum_event_width >= normal_width * 0.90f &&
                           recovered_width >= normal_width * 0.95f &&
                           maximum_position_delta <= 0.25f &&
                           maximum_velocity_delta <= 2.0f &&
                           maximum_second_difference_delta <= 0.25f,
                       "小目标单/双帧裁切必须隔离中心样本并保留规范框，"
                       "原宽/side/帧数/最小宽/恢复宽/最大位置差/速度差/"
                       "二阶差=" +
                           std::to_string(normal_width) + "/" +
                           std::to_string(side) + "/" +
                           std::to_string(partial_frames) + "/" +
                           std::to_string(minimum_event_width) + "/" +
                           std::to_string(recovered_width) + "/" +
                           std::to_string(maximum_position_delta) + "/" +
                           std::to_string(maximum_velocity_delta) + "/" +
                           std::to_string(maximum_second_difference_delta));
            }
        }
    }
}

void test_horizontal_partial_visibility_exact_three_recovers() {
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const float normal_width : {12.0f, 16.0f, 25.6f}) {
        for (const float event_confidence : {0.20f, 0.90f}) {
            for (const float side : {-1.0f, 1.0f}) {
                AimConfig config;
                config.min_confirmed_hits = 1;
                config.deadzone_pixels = 0.0f;
                config.acquisition_range_percent = 150.0f;
                Aim aim(config);
                Aim reference_aim(config);
                std::uint64_t track_id = 0;
                float width_after_two = 0.0f;
                float width_at_confirmation = 0.0f;
                float width_after_three_full = 0.0f;
                float ratio_after_three_full = -1.0f;
                float maximum_first_two_position_delta = 0.0f;
                float position_delta_after_three_full = 0.0f;
                float final_position_delta = 0.0f;
                float maximum_transition_second_difference = 0.0f;
                std::vector<float> base_points;

                for (int index = 0; index < 100; ++index) {
                    const float true_x =
                        70.0f + static_cast<float>(index) * 0.20f;
                    const bool partial = index >= 60 && index <= 62;
                    const float confidence =
                        index >= 60 && index <= 70 ? event_confidence : 0.90f;
                    AimFrame frame = make_frame(
                        static_cast<std::uint64_t>(index + 1),
                        base + std::chrono::microseconds(
                                   static_cast<long long>(index) * 4167));
                    frame.detections = {body_box(
                        true_x + (partial ? side * normal_width * 0.25f : 0.0f),
                        175.0f,
                        partial ? normal_width * 0.5f : normal_width,
                        70.0f,
                        confidence)};
                    AimFrame reference_frame = frame;
                    reference_frame.detections = {body_box(
                        true_x, 175.0f, normal_width, 70.0f, confidence)};
                    const AimResult result = aim.process(frame);
                    const AimResult reference_result =
                        reference_aim.process(reference_frame);
                    expect(result.status == AimStatus::SUCCESS &&
                               result.has_target &&
                               reference_result.status == AimStatus::SUCCESS &&
                               reference_result.has_target,
                           "恰好三帧单侧截断及完整框对照必须逐帧保留目标");
                    if (!result.has_target || !reference_result.has_target) {
                        continue;
                    }
                    if (track_id == 0) track_id = result.target.track_id;
                    expect(result.target.track_id == track_id,
                           "恰好三帧截断及立即恢复不得切换轨迹身份");
                    const float width = result.target.x2 - result.target.x1;
                    const float ratio =
                        width > 0.0f
                            ? (result.target.base_aim_x - result.target.x1) /
                                  width
                            : -1.0f;
                    const float position_delta =
                        std::fabs(result.target.base_aim_x -
                                  reference_result.target.base_aim_x);
                    if (index >= 60 && index <= 61) {
                        maximum_first_two_position_delta = std::max(
                            maximum_first_two_position_delta, position_delta);
                    }
                    if (index == 61) width_after_two = width;
                    if (index == 62) width_at_confirmation = width;
                    if (index == 65) {
                        width_after_three_full = width;
                        ratio_after_three_full = ratio;
                        position_delta_after_three_full = position_delta;
                    }
                    if (index == 70) final_position_delta = position_delta;
                    base_points.push_back(result.target.base_aim_x);
                    if (base_points.size() >= 3 && index >= 58 && index <= 70) {
                        const std::size_t size = base_points.size();
                        maximum_transition_second_difference =
                            std::max(maximum_transition_second_difference,
                                     std::fabs(base_points[size - 1] -
                                               2.0f * base_points[size - 2] +
                                               base_points[size - 3]));
                    }
                }

                const float confirmation_maximum_width_ratio =
                    event_confidence >= config.high_confidence ? 0.70f : 0.82f;
                expect(
                    width_after_two >= normal_width * 0.90f &&
                        width_at_confirmation <=
                            normal_width * confirmation_maximum_width_ratio &&
                        width_after_three_full >= normal_width * 0.93f &&
                        ratio_after_three_full >= 0.30f &&
                        ratio_after_three_full <= 0.70f &&
                        maximum_first_two_position_delta <=
                            normal_width * 0.05f &&
                        position_delta_after_three_full <=
                            normal_width * 0.20f &&
                        final_position_delta <= normal_width * 0.05f &&
                        maximum_transition_second_difference <=
                            std::max(1.0f, normal_width * 0.15f),
                    "恰好三帧截断必须确认一次并在三帧完整框后恢复，"
                    "原宽/置信度/side/第二帧宽/确认宽/恢复宽/恢复ratio/"
                    "前两帧位置差/恢复位置差/末端位置差/最大二阶=" +
                        std::to_string(normal_width) + "/" +
                        std::to_string(event_confidence) + "/" +
                        std::to_string(side) + "/" +
                        std::to_string(width_after_two) + "/" +
                        std::to_string(width_at_confirmation) + "/" +
                        std::to_string(width_after_three_full) + "/" +
                        std::to_string(ratio_after_three_full) + "/" +
                        std::to_string(maximum_first_two_position_delta) + "/" +
                        std::to_string(position_delta_after_three_full) + "/" +
                        std::to_string(final_position_delta) + "/" +
                        std::to_string(maximum_transition_second_difference));
            }
        }
    }
}

void test_horizontal_partial_rebuild_does_not_inject_velocity() {
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const float normal_width : {12.0f, 25.6f}) {
        for (const float motion_per_frame :
                 {-1.70f, -0.20f, 0.0f, 0.20f, 1.70f}) {
            for (const float event_confidence : {0.20f, 0.90f}) {
                for (const float side : {-1.0f, 1.0f}) {
                    AimConfig config;
                    config.min_confirmed_hits = 1;
                    config.deadzone_pixels = 0.0f;
                    config.acquisition_range_percent = 150.0f;
                    Aim aim(config);
                    std::uint64_t track_id = 0;
                    float maximum_wrong_direction_velocity = 0.0f;
                    float maximum_rebuild_velocity_error = 0.0f;
                    float maximum_rebuild_absolute_velocity = 0.0f;
                    float settled_partial_velocity_error = 0.0f;
                    float settled_recovery_velocity_error = 0.0f;
                    const float expected_velocity = motion_per_frame * 240.0f;
                    const bool legacy_low_motion_case =
                        std::fabs(motion_per_frame) <= 0.20f;

                    for (int index = 0; index < 100; ++index) {
                        const float true_x =
                            120.0f +
                            static_cast<float>(index) * motion_per_frame;
                        const bool partial = index >= 60 && index < 75;
                        const float confidence = index >= 60 && index <= 85
                                                     ? event_confidence
                                                     : 0.90f;
                        AimFrame frame = make_frame(
                            static_cast<std::uint64_t>(index + 1),
                            base + std::chrono::microseconds(
                                       static_cast<long long>(index) * 4167));
                        frame.detections = {body_box(
                            true_x +
                                (partial ? side * normal_width * 0.25f : 0.0f),
                            175.0f,
                            partial ? normal_width * 0.5f : normal_width,
                            70.0f,
                            confidence)};
                        const AimResult result = aim.process(frame);
                        expect(result.status == AimStatus::SUCCESS &&
                                   result.has_target,
                               "静止及双向运动的半身重建必须逐帧保留目标");
                        if (!result.has_target) continue;
                        if (track_id == 0) track_id = result.target.track_id;
                        expect(result.target.track_id == track_id,
                               "静止及双向半身重建不得切换轨迹身份");
                        const bool rebuilding_partial =
                            index >= 62 && index <= 64;
                        const bool rebuilding_full = index >= 75 && index <= 78;
                        if (rebuilding_partial || rebuilding_full) {
                            maximum_rebuild_velocity_error = std::max(
                                maximum_rebuild_velocity_error,
                                std::fabs(result.target.velocity_x -
                                          expected_velocity));
                            maximum_rebuild_absolute_velocity = std::max(
                                maximum_rebuild_absolute_velocity,
                                std::fabs(result.target.velocity_x));
                            if (motion_per_frame > 0.0f) {
                                maximum_wrong_direction_velocity =
                                    std::max(maximum_wrong_direction_velocity,
                                             -result.target.velocity_x);
                            } else if (motion_per_frame < 0.0f) {
                                maximum_wrong_direction_velocity =
                                    std::max(maximum_wrong_direction_velocity,
                                             result.target.velocity_x);
                            } else {
                                maximum_wrong_direction_velocity = std::max(
                                    maximum_wrong_direction_velocity,
                                    std::fabs(result.target.velocity_x));
                            }
                        }
                        if (index == 70) {
                            settled_partial_velocity_error = std::fabs(
                                result.target.velocity_x - expected_velocity);
                        }
                        if (index == 85) {
                            settled_recovery_velocity_error = std::fabs(
                                result.target.velocity_x - expected_velocity);
                        }
                    }

                    expect(
                        maximum_wrong_direction_velocity <= 2.0f &&
                            maximum_rebuild_velocity_error <= 60.0f &&
                            (!legacy_low_motion_case ||
                             maximum_rebuild_absolute_velocity <= 60.0f) &&
                            settled_partial_velocity_error <= 15.0f &&
                            settled_recovery_velocity_error <= 15.0f,
                        "半身确认与完整框恢复的五点预热不得注入反向速度，"
                        "原宽/每帧运动/置信度/side/最大错向速度/重建速度误差/"
                        "重建绝对速度/半框稳定误差/完整框稳定误差=" +
                            std::to_string(normal_width) + "/" +
                            std::to_string(motion_per_frame) + "/" +
                            std::to_string(event_confidence) + "/" +
                            std::to_string(side) + "/" +
                            std::to_string(maximum_wrong_direction_velocity) +
                            "/" +
                            std::to_string(maximum_rebuild_velocity_error) +
                            "/" +
                            std::to_string(maximum_rebuild_absolute_velocity) +
                            "/" +
                            std::to_string(settled_partial_velocity_error) +
                            "/" +
                            std::to_string(settled_recovery_velocity_error));
                }
            }
        }
    }
}

void test_head_only_width_change_skips_body_partial_guard() {
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const float side : {-1.0f, 1.0f}) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.acquisition_range_percent = 150.0f;
        Aim aim(config);
        std::uint64_t track_id = 0;
        float first_partial_width = 0.0f;
        float third_partial_width = 0.0f;
        float recovered_width = 0.0f;

        for (int index = 0; index < 80; ++index) {
            const float true_x = 120.0f + static_cast<float>(index) * 0.20f;
            const bool partial = index >= 50 && index <= 52;
            AimFrame frame =
                make_frame(static_cast<std::uint64_t>(index + 1),
                           base + std::chrono::microseconds(
                                      static_cast<long long>(index) * 4167));
            frame.detections = {
                head_box(true_x + (partial ? side * 3.5f : 0.0f),
                         145.0f,
                         partial ? 7.0f : 14.0f,
                         14.0f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "head-only 宽度变化必须逐帧保留目标");
            if (!result.has_target) continue;
            if (track_id == 0) track_id = result.target.track_id;
            expect(result.target.track_id == track_id,
                   "head-only 宽度变化不得切换轨迹身份");
            const float width = result.target.x2 - result.target.x1;
            if (index == 50) first_partial_width = width;
            if (index == 52) third_partial_width = width;
            if (index == 60) recovered_width = width;
        }

        expect(first_partial_width <= 10.5f && third_partial_width <= 8.0f &&
                   recovered_width >= 13.5f,
               "head-only 必须直接采用自身几何，不能累计 body 半身确认，"
               "side/首帧半框宽/第三帧半框宽/恢复宽=" +
                   std::to_string(side) + "/" +
                   std::to_string(first_partial_width) + "/" +
                   std::to_string(third_partial_width) + "/" +
                   std::to_string(recovered_width));
    }
}

void test_horizontal_partial_visibility_accepts_persistent_geometry() {
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const float normal_width : {12.0f, 16.0f, 25.6f}) {
        for (const float side : {-1.0f, 1.0f}) {
            AimConfig config;
            config.min_confirmed_hits = 1;
            config.deadzone_pixels = 0.0f;
            config.acquisition_range_percent = 150.0f;
            Aim aim(config);
            Aim reference_aim(config);
            std::uint64_t track_id = 0;
            float width_before_confirmation = 0.0f;
            float width_at_confirmation = 0.0f;
            float persistent_width = 0.0f;
            float recovered_width = 0.0f;
            float maximum_first_two_position_delta = 0.0f;
            float maximum_first_two_velocity_delta = 0.0f;
            float maximum_recovery_position_delta = 0.0f;
            float final_recovery_position_delta = 0.0f;
            float maximum_transition_second_difference = 0.0f;
            int transition_edge_frames = 0;
            int stable_partial_boundary_frames = 0;
            std::vector<float> stable_canonical_center_errors;
            std::vector<float> stable_velocity_deltas;
            std::vector<float> base_points;

            for (int index = 0; index < 120; ++index) {
                const float true_x = 70.0f + static_cast<float>(index) * 0.20f;
                const bool persistent_partial = index >= 60 && index < 75;
                AimFrame frame = make_frame(
                    static_cast<std::uint64_t>(index + 1),
                    base + std::chrono::microseconds(
                               static_cast<long long>(index) * 4167));
                frame.detections = {body_box(
                    true_x + (persistent_partial ? side * normal_width * 0.25f
                                                 : 0.0f),
                    175.0f,
                    persistent_partial ? normal_width * 0.5f : normal_width,
                    70.0f)};
                AimFrame reference_frame = frame;
                reference_frame.detections = {
                    body_box(true_x, 175.0f, normal_width, 70.0f)};
                const AimResult result = aim.process(frame);
                const AimResult reference_result =
                    reference_aim.process(reference_frame);
                expect(result.status == AimStatus::SUCCESS &&
                           result.has_target &&
                           reference_result.status == AimStatus::SUCCESS &&
                           reference_result.has_target,
                       "持续单侧半身框及完整框对照必须逐帧保留目标");
                if (!result.has_target || !reference_result.has_target)
                    continue;
                if (track_id == 0) track_id = result.target.track_id;
                expect(result.target.track_id == track_id,
                       "持续单侧半身框和完整身体恢复不得切换轨迹身份");

                const float tracked_width = result.target.x2 - result.target.x1;
                const float tracked_ratio =
                    tracked_width > 0.0f
                        ? (result.target.base_aim_x - result.target.x1) /
                              tracked_width
                        : -1.0f;
                const float position_delta =
                    std::fabs(result.target.base_aim_x -
                              reference_result.target.base_aim_x);
                const float velocity_delta =
                    std::fabs(result.target.velocity_x -
                              reference_result.target.velocity_x);
                if (index >= 60 && index <= 61) {
                    maximum_first_two_position_delta = std::max(
                        maximum_first_two_position_delta, position_delta);
                    maximum_first_two_velocity_delta = std::max(
                        maximum_first_two_velocity_delta, velocity_delta);
                }
                if (index >= 62 && index <= 65 &&
                    (tracked_ratio <= 0.05f || tracked_ratio >= 0.95f)) {
                    ++transition_edge_frames;
                }
                if (index >= 70 && index < 75) {
                    if (tracked_ratio <= 0.25f || tracked_ratio >= 0.75f) {
                        ++stable_partial_boundary_frames;
                    }
                    // 单侧裁切后的可见框中心不是人物物理中心。基础点应
                    // 继续贴合完整框对照（必要时受公开内窗边界限制），
                    // 而不是追逐裁切中心并在恢复时再反向一次。
                    stable_canonical_center_errors.push_back(
                        std::fabs(result.target.base_aim_x -
                                  reference_result.target.base_aim_x));
                    stable_velocity_deltas.push_back(velocity_delta);
                }
                if (index >= 77 && index <= 84) {
                    maximum_recovery_position_delta = std::max(
                        maximum_recovery_position_delta, position_delta);
                }
                if (index == 84) {
                    final_recovery_position_delta = position_delta;
                }
                if (index == 61) width_before_confirmation = tracked_width;
                if (index == 62) width_at_confirmation = tracked_width;
                if (index == 70) persistent_width = tracked_width;
                if (index == 77) recovered_width = tracked_width;

                base_points.push_back(result.target.base_aim_x);
                if (base_points.size() >= 3 && index >= 58 && index <= 82) {
                    const std::size_t size = base_points.size();
                    maximum_transition_second_difference =
                        std::max(maximum_transition_second_difference,
                                 std::fabs(base_points[size - 1] -
                                           2.0f * base_points[size - 2] +
                                           base_points[size - 3]));
                }
            }

            std::sort(stable_canonical_center_errors.begin(),
                      stable_canonical_center_errors.end());
            std::sort(stable_velocity_deltas.begin(),
                      stable_velocity_deltas.end());
            const auto p95 = [](const std::vector<float>& values) {
                return values[std::min(
                    values.size() - 1,
                    static_cast<std::size_t>(values.size() * 0.95f))];
            };
            const float stable_center_error_p95 =
                p95(stable_canonical_center_errors);
            const float stable_velocity_delta_p95 = p95(stable_velocity_deltas);
            expect(
                width_before_confirmation >= normal_width * 0.90f &&
                    width_at_confirmation <= normal_width * 0.70f &&
                    persistent_width <= normal_width * 0.58f &&
                    recovered_width >= normal_width * 0.93f &&
                    maximum_first_two_position_delta <= normal_width * 0.05f &&
                    maximum_first_two_velocity_delta <= 2.0f &&
                    transition_edge_frames <= 2 &&
                    stable_partial_boundary_frames <= 5 &&
                    stable_center_error_p95 <= normal_width * 0.18f &&
                    stable_velocity_delta_p95 <= 24.0f &&
                    maximum_recovery_position_delta <= normal_width * 0.27f &&
                    final_recovery_position_delta <= normal_width * 0.05f &&
                    maximum_transition_second_difference <=
                        std::max(1.0f, normal_width * 0.19f),
                "左右持续截断应在第三帧接受，并以稳定边保持裁切前物理锚，"
                "原宽/side/确认前宽/第三帧宽/持续宽/恢复宽/"
                "前两帧位置差/速度差/"
                "过渡贴边/稳定贴边/稳定中心误差P95/速度差P95/恢复位置差/"
                "恢复末位置差/最大二阶=" +
                    std::to_string(normal_width) + "/" + std::to_string(side) +
                    "/" + std::to_string(width_before_confirmation) + "/" +
                    std::to_string(width_at_confirmation) + "/" +
                    std::to_string(persistent_width) + "/" +
                    std::to_string(recovered_width) + "/" +
                    std::to_string(maximum_first_two_position_delta) + "/" +
                    std::to_string(maximum_first_two_velocity_delta) + "/" +
                    std::to_string(transition_edge_frames) + "/" +
                    std::to_string(stable_partial_boundary_frames) + "/" +
                    std::to_string(stable_center_error_p95) + "/" +
                    std::to_string(stable_velocity_delta_p95) + "/" +
                    std::to_string(maximum_recovery_position_delta) + "/" +
                    std::to_string(final_recovery_position_delta) + "/" +
                    std::to_string(maximum_transition_second_difference));
        }
    }
}

void test_vertical_jump_pose_protection_keeps_configured_aim_height() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.acquisition_range_percent = 150.0f;
    Aim aim(config);
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    float maximum_ratio_error = 0.0f;
    int boundary_frames = 0;
    int legacy_inner_boundary_frames = 0;
    std::vector<float> tracking_errors;
    std::vector<float> tracking_error_second_differences;
    float previous_tracking_error = 0.0f;
    float previous_previous_tracking_error = 0.0f;

    for (int index = 0; index < 480; ++index) {
        // 一次升降各持续 30 帧，叠加逐帧宽高形变，复现实机超级跳中
        // “1 人框 + 1 头框”始终存在但旧绝对锚点被姿态保护冻结的反例。
        const int phase_index = index % 60;
        const float jump_phase = phase_index <= 30
                                     ? static_cast<float>(phase_index)
                                     : static_cast<float>(60 - phase_index);
        const float true_aim_y = 220.0f - jump_phase * 3.0f;
        const float pose_phase = (index % 2) == 0 ? -1.0f : 1.0f;
        const float height = 86.0f + pose_phase * 5.0f;
        const float observed_aim_y = true_aim_y + pose_phase * 1.5f;
        const float center_y =
            observed_aim_y + height * (0.5f - config.body_aim_height_ratio);
        AimFrame frame =
            make_frame(static_cast<std::uint64_t>(index + 1),
                       base + std::chrono::microseconds(index * 4167));
        frame.detections = {body_box(160.0f, center_y, 42.0f, height),
                            head(160.0f, center_y - height * 0.30f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "垂直跳跃形变回归必须持续保留确认目标");
        if (index < 60 || !result.has_target) continue;

        const float tracked_height =
            result.target.y2 - result.target.y1;
        const float ratio = tracked_height > 0.0f
            ? (result.target.base_aim_y - result.target.y1) /
                tracked_height
            : -1.0f;
        maximum_ratio_error = std::max(
            maximum_ratio_error,
            std::fabs(ratio - config.body_aim_height_ratio));
        if (ratio <= 0.001f || ratio >= 0.999f) {
            ++boundary_frames;
        }
        const float lower_legacy_boundary =
            config.body_aim_height_ratio - 0.05f;
        const float upper_legacy_boundary =
            config.body_aim_height_ratio + 0.05f;
        if (std::fabs(ratio - lower_legacy_boundary) < 0.0001f ||
            std::fabs(ratio - upper_legacy_boundary) < 0.0001f) {
            ++legacy_inner_boundary_frames;
        }
        const float tracking_error = result.target.base_aim_y - true_aim_y;
        tracking_errors.push_back(std::fabs(tracking_error));
        if (tracking_errors.size() >= 3) {
            tracking_error_second_differences.push_back(std::fabs(
                tracking_error - 2.0f * previous_tracking_error +
                previous_previous_tracking_error));
        }
        previous_previous_tracking_error = previous_tracking_error;
        previous_tracking_error = tracking_error;
    }

    std::sort(tracking_errors.begin(), tracking_errors.end());
    std::sort(tracking_error_second_differences.begin(),
              tracking_error_second_differences.end());
    const float tracking_error_p95 = tracking_errors[
        static_cast<std::size_t>(tracking_errors.size() * 0.95f)];
    const float tracking_error_second_p95 =
        tracking_error_second_differences[static_cast<std::size_t>(
            tracking_error_second_differences.size() * 0.95f)];
    expect(maximum_ratio_error <= 0.10f && boundary_frames == 0 &&
               legacy_inner_boundary_frames <= 2 &&
               tracking_error_p95 <= 6.0f &&
               tracking_error_second_p95 <= 4.5f,
           "超级跳 Y 基础特征必须连续跟随真实平移，不能驻留旧 ±5% 内窗"
           "或触碰完整框边界；最大比例偏差/完整边界帧/旧内窗边界帧/"
           "误差P95/误差二阶P95=" +
               std::to_string(maximum_ratio_error) + "/" +
               std::to_string(boundary_frames) + "/" +
               std::to_string(legacy_inner_boundary_frames) + "/" +
               std::to_string(tracking_error_p95) + "/" +
               std::to_string(tracking_error_second_p95));
}

void test_body_aim_range_is_static_safe_and_motion_bounded() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.body_aim_range_percent = 40.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame still = make_frame(1, base);
    still.detections = {body_box(150.0f, 160.0f, 100.0f, 100.0f)};
    const AimResult still_result = aim.process(still);
    AimFrame moving = make_frame(2, base + std::chrono::milliseconds(40));
    moving.detections = {body_box(190.0f, 160.0f, 100.0f, 100.0f)};
    const AimResult moving_result = aim.process(moving);
    const float min_x = moving_result.target.x1 + 30.0f;
    const float max_x = moving_result.target.x2 - 30.0f;
    expect(still_result.has_target &&
               std::fabs(still_result.target.base_aim_x - 150.0f) < 0.1f &&
               moving_result.has_target &&
               std::fabs(moving_result.target.base_aim_x -
                         (moving_result.target.x1 + moving_result.target.x2) * 0.5f) < 0.1f &&
               moving_result.target.base_aim_x >= min_x &&
               moving_result.target.base_aim_x <= max_x,
           "静止和移动目标都直接使用配置内窗中的稳定基础点");
}

void test_multi_target_crossing_keeps_selected_identity() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 3;
    config.max_center_distance = 0.35f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    const float right_positions[] = {
        100.0f, 115.0f, 130.0f, 145.0f, 160.0f, 175.0f, 190.0f};
    const float left_positions[] = {
        260.0f, 240.0f, 220.0f, 200.0f, 180.0f, 160.0f, 140.0f};

    std::uint64_t selected_id = 0;
    AimResult last;
    for (std::size_t index = 0; index < 7; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        if ((index % 2U) == 0U) {
            frame.detections = {
                body(right_positions[index], 160.0f),
                body(left_positions[index], 160.0f)};
        } else {
            frame.detections = {
                body(left_positions[index], 160.0f),
                body(right_positions[index], 160.0f)};
        }
        last = aim.process(frame);
        expect(last.has_target, "双目标交叉期间每帧都应保留已确认目标");
        if (index == 0) selected_id = last.target.track_id;
        expect(last.target.track_id == selected_id,
               "双目标交叉和检测顺序变化不得造成无确认的锁定切换");
    }
    expect(last.target.aim_x > 175.0f,
           "交叉结束后选中轨迹应继续沿原方向移动，不能交换为反向目标，实际 aim_x=" +
               std::to_string(last.target.aim_x));
}

void test_tentative_duplicate_cannot_steal_confirmed_observation() {
    AimConfig config;
    config.min_confirmed_hits = 2;
    config.deadzone_pixels = 0.0f;
    config.max_center_distance = 0.25f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.detections = {body(150.0f, 160.0f)};
    expect(!aim.process(first).has_target,
           "首帧暂定轨迹不得提前成为目标");

    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(10));
    second.detections = {body(154.0f, 160.0f)};
    const AimResult confirmed = aim.process(second);
    expect(confirmed.has_target && !confirmed.target.predicted,
           "连续人体观测必须先建立确认轨迹");
    const std::uint64_t confirmed_id = confirmed.target.track_id;

    // 真机 Run 中第二个人体框只出现一帧：既有轨迹匹配主框，额外框
    // 创建暂定候选；下一帧单框更靠近候选时，候选也不能抢走观测。
    AimFrame duplicate = make_frame(
        3, base + std::chrono::milliseconds(20));
    duplicate.detections = {
        body(158.0f, 160.0f), body(170.0f, 160.0f)};
    const AimResult duplicate_result = aim.process(duplicate);
    AimFrame single = make_frame(
        4, base + std::chrono::milliseconds(30));
    single.detections = {body(168.0f, 160.0f)};
    const AimResult single_result = aim.process(single);

    expect(duplicate_result.has_target && single_result.has_target &&
               duplicate_result.target.track_id == confirmed_id &&
               single_result.target.track_id == confirmed_id &&
               !single_result.target.predicted,
           "短暂重复人体框消失后，确认轨迹必须优先消费唯一观测，禁止候选抢占");
}

void test_confirmed_duplicate_cannot_starve_selected_observation() {
    AimConfig config;
    config.min_confirmed_hits = 2;
    config.deadzone_pixels = 0.0f;
    config.max_center_distance = 0.25f;
    config.switch_confirm_frames = 3;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.detections = {body(150.0f, 160.0f)};
    aim.process(first);
    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(10));
    second.detections = {body(154.0f, 160.0f)};
    const AimResult confirmed = aim.process(second);
    const std::uint64_t selected_id = confirmed.target.track_id;

    // 真机 Run 的重复人体框并非只出现一帧：重叠候选可在两帧后也进入
    // CONFIRMED。它消失为单框时，唯一观测若略靠近重复轨迹，原锁定会
    // 先滑入 LOST 两帧再切 ID，造成“非零追赶—硬零—新 ID 跳变”。
    AimFrame duplicate_once = make_frame(
        3, base + std::chrono::milliseconds(20));
    duplicate_once.detections = {
        body(158.0f, 160.0f), body(170.0f, 160.0f)};
    aim.process(duplicate_once);
    AimFrame duplicate_twice = make_frame(
        4, base + std::chrono::milliseconds(30));
    duplicate_twice.detections = {
        body(162.0f, 160.0f), body(172.0f, 160.0f)};
    aim.process(duplicate_twice);
    AimFrame single = make_frame(
        5, base + std::chrono::milliseconds(40));
    single.detections = {body(172.0f, 160.0f)};
    const AimResult result = aim.process(single);

    expect(confirmed.has_target && result.has_target &&
               result.target.track_id == selected_id &&
               !result.target.predicted,
           "重叠重复轨迹已确认后，唯一可行观测仍应优先维持当前锁定，"
           "不得先进入 LOST 再切换 ID");
}

void test_selected_association_bias_does_not_hijack_distinct_target() {
    AimConfig config;
    config.min_confirmed_hits = 2;
    config.deadzone_pixels = 0.0f;
    config.max_center_distance = 0.25f;
    config.switch_confirm_frames = 3;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.detections = {body(150.0f, 160.0f)};
    aim.process(first);
    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(10));
    second.detections = {body(154.0f, 160.0f)};
    const AimResult selected = aim.process(second);

    AimFrame two_targets_once = make_frame(
        3, base + std::chrono::milliseconds(20));
    two_targets_once.detections = {
        body(158.0f, 160.0f), body(190.0f, 160.0f)};
    aim.process(two_targets_once);
    AimFrame two_targets_twice = make_frame(
        4, base + std::chrono::milliseconds(30));
    two_targets_twice.detections = {
        body(162.0f, 160.0f), body(194.0f, 160.0f)};
    aim.process(two_targets_twice);

    AimResult missing_selected;
    std::string switch_trace;
    for (int index = 0; index < 3; ++index) {
        AimFrame only_distinct = make_frame(
            static_cast<std::uint64_t>(5 + index),
            base + std::chrono::milliseconds(40 + index * 10));
        only_distinct.detections = {
            body(198.0f + index * 4.0f, 160.0f)};
        missing_selected = aim.process(only_distinct);
        switch_trace += std::to_string(missing_selected.target.track_id) + ":" +
            (missing_selected.target.predicted ? "P" : "O") + ":" +
            std::to_string(missing_selected.target.aim_x) + ";";
        if (index == 0) {
            expect(missing_selected.has_target &&
                       missing_selected.target.track_id ==
                           selected.target.track_id &&
                       missing_selected.target.predicted,
                   "当前目标真实缺失时，软关联优先权不得劫持明显分离的另一目标");
        }
    }
    expect(missing_selected.has_target &&
               missing_selected.target.track_id != selected.target.track_id &&
               !missing_selected.target.predicted,
           "明显分离的已确认目标连续领先后仍必须按既有三帧门切换，trace=" +
               switch_trace);
}

void test_loss_prediction_does_not_compound_time() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.max_lost_frames = 4;
    config.deadzone_pixels = 0.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.detections = {body(100.0f, 160.0f)};
    aim.process(first);
    AimFrame second = make_frame(2, base + std::chrono::milliseconds(10));
    second.detections = {body(110.0f, 160.0f)};
    aim.process(second);

    const AimResult lost_once = aim.process(
        make_frame(3, base + std::chrono::milliseconds(20)));
    const AimResult lost_twice = aim.process(
        make_frame(4, base + std::chrono::milliseconds(30)));
    const float first_step = lost_once.target.aim_x - 107.2f;
    const float second_step = lost_twice.target.aim_x -
                              lost_once.target.aim_x;
    expect(lost_once.has_target && lost_twice.has_target &&
               lost_once.target.predicted && lost_twice.target.predicted &&
               std::fabs(first_step - second_step) < 0.25f,
           "连续丢帧只能推进新增时间区间，实际 first=" +
               std::to_string(first_step) + " second=" +
               std::to_string(second_step));
}

void test_observation_age_adds_bounded_lead() {
    AimConfig config;
    config.enable_prediction = true;
    config.max_prediction_lead_percent = 5.0f;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.max_counts_per_frame = 1000.0f;
    Aim aged(config);
    Aim fresh(config);
    const auto now = std::chrono::steady_clock::now();

    AimFrame aged_first = make_frame(1, now - std::chrono::milliseconds(60));
    aged_first.detections = {body(200.0f, 160.0f)};
    aged.process(aged_first);
    AimFrame aged_second = make_frame(2, now - std::chrono::milliseconds(50));
    aged_second.detections = {body(210.0f, 160.0f)};
    const AimResult aged_result = aged.process(aged_second);

    const auto future = now + std::chrono::seconds(1);
    AimFrame fresh_first = make_frame(1, future);
    fresh_first.detections = {body(200.0f, 160.0f)};
    fresh.process(fresh_first);
    AimFrame fresh_second = make_frame(2, future +
        std::chrono::milliseconds(10));
    fresh_second.detections = {body(210.0f, 160.0f)};
    const AimResult fresh_result = fresh.process(fresh_second);

    expect(aged_result.has_target && fresh_result.has_target &&
               aged_result.target.aim_x > fresh_result.target.aim_x + 2.0f,
           "控制点必须按真实观测年龄沿估计速度提前，不能只追逐过期位置");
    expect(aged_result.target.aim_x - fresh_result.target.aim_x < 20.0f,
           "观测年龄提前必须受目标框尺度安全上限约束");
    expect(aged_result.target.base_aim_x >= aged_result.target.x1 &&
               aged_result.target.base_aim_x <= aged_result.target.x2 &&
               aged_result.target.base_aim_y >= aged_result.target.y1 &&
               aged_result.target.base_aim_y <= aged_result.target.y2,
           "预测开启时基础追踪点仍必须始终位于选中模型框内");
    expect(aged_result.target.lead_active &&
               aged_result.target.lead_x > 0.0f &&
               std::hypot(aged_result.target.lead_x,
                          aged_result.target.lead_y) <=
                   std::hypot(40.0f, 80.0f) * 0.05f + 0.01f &&
               aged_result.target.observation_age_ms > 0.0f &&
               std::fabs(aged_result.target.aim_x -
                         aged_result.target.base_aim_x -
                         aged_result.target.lead_x) < 0.01f,
           "提前诊断必须准确记录基础点、实际提前向量和观测年龄，且不能越过用户最大提前距离");
}

void test_prediction_lead_can_leave_box_with_bounded_distance() {
    AimConfig config;
    config.enable_prediction = true;
    config.max_prediction_lead_percent = 50.0f;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.max_counts_per_frame = 1000.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    for (int index = 0; index < 3; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(100);
        frame.control_center_x = 60.0f;
        frame.detections = {body(100.0f + index * 20.0f, 160.0f)};
        const AimResult result = aim.process(frame);
        if (index != 2) continue;

        const float maximum_lead = std::hypot(40.0f, 80.0f) * 0.50f;
        expect(result.has_target && result.target.lead_active &&
                   result.target.base_aim_x >= result.target.x1 &&
                   result.target.base_aim_x <= result.target.x2 &&
                   result.target.aim_x > result.target.x2 &&
                   std::hypot(result.target.lead_x,
                              result.target.lead_y) <= maximum_lead + 0.01f,
               "预测点应允许越出目标框，同时严格受最大提前距离门禁约束");
    }
}

void test_dynamic_control_range_does_not_reduce_observation() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.max_center_distance = 0.50f;
    config.acquisition_range_percent = 60.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame acquired = make_frame(1, base);
    acquired.detections = {body(200.0f, 160.0f)};
    const AimResult first = aim.process(acquired);

    AimFrame locked = make_frame(2, base + std::chrono::milliseconds(10));
    locked.lock_active = true;
    locked.detections = {body(210.0f, 160.0f)};
    const AimResult second = aim.process(locked);

    AimFrame outside = make_frame(3, base + std::chrono::milliseconds(20));
    outside.lock_active = true;
    outside.detections = {body(310.0f, 160.0f)};
    const AimResult third = aim.process(outside);

    AimFrame returned = make_frame(4, base + std::chrono::milliseconds(30));
    returned.lock_active = true;
    returned.detections = {body(220.0f, 160.0f)};
    const AimResult fourth = aim.process(returned);

    expect(first.has_target && second.has_target &&
               second.target.track_id == first.target.track_id &&
               second.range_locked &&
               second.active_range_radius <
                   second.acquisition_range_radius,
           "按住控制后应在已预选轨迹上动态收缩范围，范围外挑战者不能立即切换");
    expect(third.has_target &&
               third.target.track_id == first.target.track_id &&
               !third.range_allows_control && !third.has_command,
           "目标越出控制范围时必须继续更新同一轨迹，只暂停鼠标命令");
    expect(fourth.has_target &&
               fourth.target.track_id == first.target.track_id &&
               fourth.range_allows_control && fourth.has_command,
           "同一目标回到动态范围后应直接恢复预计算命令，不能重新建轨迹");
}

void test_prediction_hysteresis_avoids_crosshair_oscillation() {
    AimConfig predicted_config;
    predicted_config.enable_prediction = true;
    predicted_config.min_confirmed_hits = 1;
    predicted_config.deadzone_pixels = 0.0f;
    predicted_config.max_counts_per_frame = 1000.0f;
    AimConfig basic_config = predicted_config;
    basic_config.enable_prediction = false;
    Aim predicted(predicted_config);
    Aim basic(basic_config);
    const auto base = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(100);

    const auto process_pair = [&](std::uint64_t sequence, int milliseconds,
                                  float target_x, float control_x) {
        AimFrame frame = make_frame(
            sequence, base + std::chrono::milliseconds(milliseconds));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(20);
        frame.control_center_x = control_x;
        frame.detections = {body(target_x, 160.0f)};
        return std::pair<AimResult, AimResult>{
            predicted.process(frame), basic.process(frame)};
    };

    process_pair(1, 0, 100.0f, 60.0f);
    process_pair(2, 10, 112.0f, 60.0f);
    process_pair(3, 20, 124.0f, 60.0f);
    const auto moving_away = process_pair(4, 30, 136.0f, 60.0f);
    const auto crossed = process_pair(5, 40, 144.0f, 170.0f);
    const auto settling = process_pair(6, 50, 148.0f, 148.0f);
    AimResult settled = settling.first;
    AimResult settled_basic = settling.second;
    for (int sequence = 7; sequence <= 17; ++sequence) {
        const auto pair = process_pair(
            static_cast<std::uint64_t>(sequence), sequence * 10 - 10,
            148.0f, 148.0f);
        settled = pair.first;
        settled_basic = pair.second;
    }
    std::pair<AimResult, AimResult> rearmed;
    for (int sequence = 18; sequence <= 24; ++sequence) {
        rearmed = process_pair(
            static_cast<std::uint64_t>(sequence), sequence * 10 - 10,
            148.0f + (sequence - 17) * 12.0f, 100.0f);
    }

    expect(moving_away.first.target.aim_x >
               moving_away.first.target.base_aim_x + 1.0f,
           "目标持续远离准星时，开启预测应产生有界提前量");
    expect(std::fabs(crossed.first.target.aim_x -
                     crossed.first.target.base_aim_x) < 0.5f,
           "目标越过准星后必须撤销提前量，不能继续预测到前方造成反向拉回");
    expect(std::fabs(settling.first.target.aim_x -
                     settling.first.target.base_aim_x) < 0.5f &&
               std::fabs(settled.target.aim_x -
                         settled.target.base_aim_x) < 0.5f,
           "归位收敛区内必须保持预测关闭，避免立即重新前探");
    expect(rearmed.first.target.aim_x >
               rearmed.first.target.base_aim_x + 1.0f,
           "目标重新远离并越过进入阈值后才允许再次预测，基础点=" +
               std::to_string(rearmed.first.target.base_aim_x) +
               "，最终点=" + std::to_string(rearmed.first.target.aim_x) +
               "，基础模式点=" +
               std::to_string(rearmed.second.target.aim_x) +
               "，速度=" +
               std::to_string(rearmed.first.target.velocity_x) +
               "，lead_active=" +
               std::to_string(rearmed.first.target.lead_active));
}

void test_closed_loop_view_feedback_converges_without_limit_cycle() {
    AimConfig config;
    config.enable_prediction = true;
    config.max_prediction_lead_percent = 35.0f;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.75f;
    config.smoothing = 0.45f;
    config.counts_per_pixel_x = 0.50f;
    config.counts_per_pixel_y = 0.50f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    Aim aim(config);

    constexpr int kFrameCount = 240;
    constexpr int kCommandDelayFrames = 2;
    constexpr float kFrameSeconds = 0.005f;
    constexpr float kCameraResponse = 0.85f;
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    float world_target_x = 52.0f;
    float camera_x = 0.0f;
    float delayed_commands[kCommandDelayFrames]{};
    float previous_observed_error = 0.0f;
    int previous_settle_command_sign = 0;
    int initial_lead_frames = 0;
    int settled_lead_frames = 0;
    int late_settled_lead_frames = 0;
    int rearmed_lead_frames = 0;
    int settle_command_frames = 0;
    int settle_direction_reversals = 0;
    int first_reverse_command_frame = -1;
    int base_point_outside_frames = 0;
    int lead_limit_violation_frames = 0;
    int command_direction_violation_frames = 0;
    bool crossed_after_camera_feedback = false;
    float maximum_settle_error = 0.0f;

    // 世界目标和镜头使用同一角度等效像素坐标。历史鼠标命令经过固定
    // 两帧延迟改变镜头，下一帧观测位置按 world - camera 反向变化。
    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kCommandDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * kCameraResponse;
        delayed_commands[delay_slot] = 0.0f;

        float world_velocity = 0.0f;
        if (index < 70) {
            world_velocity = 280.0f;
        } else if (index < 115) {
            world_velocity = -320.0f;
        } else if (index >= 175) {
            world_velocity = 260.0f;
        }
        world_target_x += world_velocity * kFrameSeconds;

        const float observed_x = 160.0f + world_target_x - camera_x;
        const float observed_error = observed_x - 160.0f;
        if (index > 0 && previous_observed_error > 0.0f &&
            observed_error < 0.0f) {
            crossed_after_camera_feedback = true;
        }
        previous_observed_error = observed_error;

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 5));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(18);
        frame.lock_active = true;
        frame.detections = {body(observed_x, 160.0f)};
        const AimResult result = aim.process(frame);

        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "闭环视角反馈期间每帧都必须保留合法目标");
        if (!result.has_target) continue;

        const bool base_inside =
            result.target.base_aim_x >= result.target.x1 &&
            result.target.base_aim_x <= result.target.x2 &&
            result.target.base_aim_y >= result.target.y1 &&
            result.target.base_aim_y <= result.target.y2;
        if (!base_inside) ++base_point_outside_frames;
        const float maximum_lead = std::hypot(
            result.target.x2 - result.target.x1,
            result.target.y2 - result.target.y1) *
            config.max_prediction_lead_percent / 100.0f;
        if (std::hypot(result.target.lead_x, result.target.lead_y) >
            maximum_lead + 0.01f) {
            ++lead_limit_violation_frames;
        }

        if (index >= 1 && index < 70 && result.target.lead_active) {
            ++initial_lead_frames;
        }
        if (index >= 155 && index < 175 && result.target.lead_active) {
            ++settled_lead_frames;
        }
        if (index >= 165 && index < 175 && result.target.lead_active) {
            ++late_settled_lead_frames;
        }
        if (index >= 185 && result.target.lead_active) {
            ++rearmed_lead_frames;
        }

        const float final_error_x = result.target.aim_x -
            frame.control_center_x;
        const float final_error_y = result.target.aim_y -
            frame.control_center_y;
        if (result.has_command) {
            const float command_dot_error =
                result.command.dx_counts * final_error_x +
                result.command.dy_counts * final_error_y;
            if (command_dot_error <= 0.0f) {
                ++command_direction_violation_frames;
            }
            delayed_commands[delay_slot] =
                static_cast<float>(result.command.dx_counts);
            if (index >= 70 && first_reverse_command_frame < 0 &&
                result.command.dx_counts < 0) {
                first_reverse_command_frame = index;
            }
        }

        if (index >= 155 && index < 175) {
            maximum_settle_error = std::max(
                maximum_settle_error, std::fabs(final_error_x));
            if (result.has_command) {
                ++settle_command_frames;
                const int command_sign = result.command.dx_counts < 0 ? -1 : 1;
                if (previous_settle_command_sign != 0 &&
                    command_sign != previous_settle_command_sign) {
                    ++settle_direction_reversals;
                }
                previous_settle_command_sign = command_sign;
            }
        }
    }

    expect(crossed_after_camera_feedback,
           "闭环模型必须实际覆盖鼠标命令改变视角后目标越过准星的反馈");
    expect(base_point_outside_frames == 0,
           "闭环预测期间基础追踪点必须始终位于当前目标框内");
    expect(lead_limit_violation_frames == 0,
           "预测点允许位于框外，但提前向量不得超过用户最大距离门禁");
    expect(command_direction_violation_frames == 0,
           "闭环命令必须始终朝向本帧基础点或预测最终点");
    expect(initial_lead_frames > 0,
           "闭环持续运动且形成足够轴向误差时必须进入预测，首次=" +
               std::to_string(initial_lead_frames) +
               "，恢复阶段=" + std::to_string(rearmed_lead_frames));
    expect(first_reverse_command_frame >= 70 &&
               first_reverse_command_frame <= 85,
           "世界目标突然反向后，闭环控制必须在有界帧数内反向响应");
    expect(settled_lead_frames <= 6 && late_settled_lead_frames == 0,
           "静止归位阶段只允许短暂过渡，后半段必须退出预测，实际预测帧=" +
               std::to_string(settled_lead_frames) +
               "，后半段预测帧=" +
               std::to_string(late_settled_lead_frames));
    expect(maximum_settle_error <= 4.0f &&
               settle_direction_reversals <= 2,
           "静止归位阶段不得形成持续往返的闭环极限环，最大误差=" +
               std::to_string(maximum_settle_error) +
               "，命令帧=" + std::to_string(settle_command_frames) +
               "，方向反转=" + std::to_string(settle_direction_reversals));
}

void test_control_trajectory_never_moves_away_from_target() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 0.35f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 50.0f;
    config.max_center_distance = 0.50f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame right = make_frame(1, base);
    right.detections = {body(250.0f, 160.0f)};
    const AimResult right_result = aim.process(right);
    AimFrame left = make_frame(2, base + std::chrono::milliseconds(10));
    left.detections = {body(70.0f, 160.0f)};
    const AimResult left_result = aim.process(left);
    AimFrame left_again = make_frame(
        3, base + std::chrono::milliseconds(20));
    left_again.detections = {body(70.0f, 160.0f)};
    const AimResult left_again_result = aim.process(left_again);

    expect(right_result.has_command &&
               right_result.command.dx_counts > 0 &&
               (!left_result.has_command ||
                left_result.command.dx_counts <= 0) &&
               left_again_result.has_command &&
               left_again_result.command.dx_counts < 0,
           "目标反向时不得沿历史动量继续远离当前框内瞄点");
}

void test_integral_tracks_constant_velocity_with_bounded_error() {
    struct ClosedLoopResult {
        float mean_error = 0.0f;
        int maximum_no_command = 0;
    };
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr float kCameraResponse = 0.85f;
    const auto run_case = [&](float target_velocity,
                              float initial_error = 32.0f,
                              int no_command_measure_start = 80,
                              bool align_vertical_aim = false,
                              float camera_response = 0.85f) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.75f;
        config.smoothing = 0.50f;
        config.counts_per_pixel_x = 0.40f;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 100.0f;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        float world_target_x = initial_error;
        float camera_x = 0.0f;
        float error_sum = 0.0f;
        int measured_frames = 0;
        int consecutive_no_command = 0;
        int maximum_no_command = 0;

        for (int index = 0; index < 420; ++index) {
            world_target_x += target_velocity * kFrameSeconds;
            const float observed_error = world_target_x - camera_x;
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index * 1000000.0f / 240.0f)));
            frame.lock_active = true;
            frame.detections = {body(
                160.0f + observed_error,
                align_vertical_aim ? 172.0f : 160.0f)};
            const AimResult result = aim.process(frame);
            if (result.has_command) {
                camera_x += result.command.dx_counts /
                    config.counts_per_pixel_x * camera_response;
                consecutive_no_command = 0;
            } else if (index >= no_command_measure_start) {
                ++consecutive_no_command;
                maximum_no_command = std::max(
                    maximum_no_command, consecutive_no_command);
            }

            if (index >= 240) {
                error_sum += std::fabs(world_target_x - camera_x);
                ++measured_frames;
            }
        }
        return ClosedLoopResult{
            error_sum / measured_frames, maximum_no_command};
    };

    const ClosedLoopResult normal = run_case(180.0f);
    expect(normal.mean_error <= 0.75f,
           "0.40 增益下，真实积分必须把恒速目标的动态稳态误差限制在 0.75 px 内，实际=" +
               std::to_string(normal.mean_error));
    expect(normal.maximum_no_command <= 1,
           "恒速目标进入死区后不得周期停发并等待再次落后，最长停发=" +
               std::to_string(normal.maximum_no_command));

    const ClosedLoopResult subcount = run_case(
        60.0f, 0.5f, 0, true, 0.20f);
    expect(subcount.maximum_no_command <= 4,
           "低速移动目标的亚整数命令必须跨帧分摊，最长停发=" +
               std::to_string(subcount.maximum_no_command));

    // 实机第六轮的命令中位数为 3 counts；高恒速用例要求内部保持量可覆盖
    // 这一档位，避免再次由积分硬上限制造稳定跟随误差。
    const ClosedLoopResult fast = run_case(720.0f);
    expect(fast.mean_error <= 2.0f,
           "高恒速目标不得破坏原有动态稳态误差门禁，实际=" +
               std::to_string(fast.mean_error));
    expect(fast.maximum_no_command <= 1,
           "高恒速目标不得周期停发，最长停发=" +
               std::to_string(fast.maximum_no_command));

    const ClosedLoopResult real_demand = run_case(
        480.0f, 32.0f, 80, false, 0.20f);
    expect(real_demand.mean_error <= 0.50f,
           "低响应闭环必须覆盖实机约 4-count 维持档，动态稳态误差=" +
               std::to_string(real_demand.mean_error));
}

void test_delayed_closed_loop_holds_moving_base_point() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr int kActuationDelayFrames = 4;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = 24.0f;
    float camera_x = 0.0f;
    float error_sum = 0.0f;
    float maximum_error = 0.0f;
    std::vector<float> measured_errors;
    measured_errors.reserve(480);
    int measured_frames = 0;
    int consecutive_no_command = 0;
    int maximum_no_command = 0;
    int previous_horizontal_command = 0;

    for (int index = 0; index < 960; ++index) {
        // 低响应与周期变速会先建立较大保持积分，再突然降低维持需求，覆盖实机报告中的过零停发。
        camera_x += delayed_commands[index % kActuationDelayFrames] /
            config.counts_per_pixel_x * 0.20f;
        delayed_commands[index % kActuationDelayFrames] = 0;
        const float target_velocity = (index / 120) % 2 == 0
            ? 480.0f : 180.0f;
        world_target_x += target_velocity * kFrameSeconds;
        const float observed_error = world_target_x - camera_x;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body(160.0f + observed_error, 160.0f)};
        const AimResult result = aim.process(frame);
        const int horizontal_command = result.has_command
            ? result.command.dx_counts : 0;
        expect(previous_horizontal_command * horizontal_command >= 0,
               "延迟闭环的单轴命令换向必须先经过零，前值=" +
                   std::to_string(previous_horizontal_command) + "，当前=" +
                   std::to_string(horizontal_command));
        previous_horizontal_command = horizontal_command;
        if (result.has_command) {
            delayed_commands[index % kActuationDelayFrames] =
                result.command.dx_counts;
            consecutive_no_command = 0;
        } else if (index >= 480) {
            ++consecutive_no_command;
            maximum_no_command = std::max(
                maximum_no_command, consecutive_no_command);
        }
        if (index >= 480) {
            const float error = std::fabs(world_target_x - camera_x);
            error_sum += error;
            maximum_error = std::max(maximum_error, error);
            measured_errors.push_back(error);
            ++measured_frames;
        }
    }

    std::sort(measured_errors.begin(), measured_errors.end());
    const float mean_error = error_sum / measured_frames;
    const float p95_error = measured_errors[
        static_cast<std::size_t>(measured_errors.size() * 0.95f)];
    expect(mean_error <= 10.0f && p95_error <= 16.1f,
           "15 ms/0.20 响应的周期变速闭环必须连续输出且误差有界；"
           "该用例不靠停发门压低反转次数，平均误差=" +
               std::to_string(mean_error) + "，最大误差=" +
               std::to_string(maximum_error) + ", P95=" +
               std::to_string(p95_error));
    expect(maximum_no_command <= 1,
           "15 ms 输出延迟闭环不得周期停发，最长停发=" +
               std::to_string(maximum_no_command));
}


void test_delayed_pose_closed_loop_keeps_tracking_pi_continuous() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr int kActuationDelayFrames = 4;
    struct Trace {
        float error_p95 = 0.0f;
        float input_second_p95 = 0.0f;
        float base_second_p95 = 0.0f;
        float final_second_p95 = 0.0f;
        float delay_second_p95 = 0.0f;
        float boundary_base_second_p95 = 0.0f;
        float boundary_final_second_p95 = 0.0f;
        int command_reversals = 0;
        int missed_true_reversals = 0;
        int wrong_direction_commands_after_base_crossing = 0;
        int maximum_high_speed_reverse_latency_frames = 0;
        int maximum_low_speed_reverse_latency_frames = 0;
        int maximum_reverse_zero_frames = 0;
        int legacy_reverse_state_frames = 0;
        int modelled_response_frames = 0;
        int phase_aligned_command_frames = 0;
        int weighted_observer_frames = 0;
        std::string first_command_reversal_context;
    };
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };
    const auto run_case = [&](bool pose_enabled) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 1.5f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = 0.425f;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 14.0f;
        config.acquisition_range_percent = 100.0f;
        // 该回归只测 X；Y 瞄点固定在框中心，避免未建模的 Y 植物响应
        // 污染二维 deadzone 与向量整形。
        config.body_aim_height_ratio = 0.50f;
        config.enable_prediction = false;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 15.0f;
        Aim aim(config);
        std::array<int, kActuationDelayFrames> delayed_commands{};
        float world_target_x = 24.0f;
        float camera_x = 0.0f;
        int previous_command_sign = 0;
        Trace trace;
        std::vector<float> true_errors;
        std::vector<float> observed_centers;
        std::vector<float> base_points;
        std::vector<float> final_points;
        std::vector<float> delay_offsets;
        std::vector<int> sample_frame_indices;
        constexpr std::array<float, 6> kTargetVelocities{
            480.0f, 180.0f, -480.0f, -180.0f, 180.0f, -180.0f};
        int active_reverse_direction = 0;
        float active_reverse_speed = 0.0f;
        int active_reverse_base_crossing_frame = -1;
        int active_reverse_zero_frames = 0;
        int expected_true_reversals = 0;
        int completed_true_reversals = 0;

        for (int index = 0; index < 720; ++index) {
            camera_x += delayed_commands[index % kActuationDelayFrames] /
                config.counts_per_pixel_x * 0.20f;
            delayed_commands[index % kActuationDelayFrames] = 0;
            const int motion_segment = std::min(
                static_cast<int>(kTargetVelocities.size()) - 1,
                index / 120);
            const float target_velocity =
                kTargetVelocities[static_cast<std::size_t>(motion_segment)];
            if (index > 0 && index % 120 == 0) {
                const float previous_target_velocity =
                    kTargetVelocities[static_cast<std::size_t>(
                        motion_segment - 1)];
                if (target_velocity * previous_target_velocity < 0.0f) {
                    active_reverse_direction = target_velocity < 0.0f ? -1 : 1;
                    active_reverse_speed = std::fabs(target_velocity);
                    active_reverse_base_crossing_frame = -1;
                    active_reverse_zero_frames = 0;
                    ++expected_true_reversals;
                }
            }
            world_target_x += target_velocity * kFrameSeconds;
            const int pose_phase_index = index % 34;
            const float pose_phase = pose_phase_index <= 17
                ? -1.0f + static_cast<float>(pose_phase_index) *
                    (2.0f / 17.0f)
                : 1.0f - static_cast<float>(pose_phase_index - 17) *
                    (2.0f / 17.0f);
            const float applied_pose = pose_enabled ? pose_phase : 0.0f;
            const float observed_x = 160.0f + world_target_x - camera_x;
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.control_at =
                frame.captured_at + std::chrono::milliseconds(1);
            frame.lock_active = true;
            frame.detections = {body_box(
                observed_x + applied_pose * 3.0f,
                160.0f,
                42.0f + applied_pose * 1.6f,
                90.0f + applied_pose * 1.8f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "tracking 四帧反馈对照必须逐帧保留确认目标");
            if (!result.has_target) continue;
            if (result.control.evaluated) {
                if (result.control.reverse_candidate_x ||
                    result.control.reverse_gate_blocked_x ||
                    result.control.reverse_probe_active_x ||
                    result.control.pending_inventory_hold_blocked_x) {
                    ++trace.legacy_reverse_state_frames;
                }
                if (std::fabs(
                        result.control.modelled_response_x_counts) > 0.001f) {
                    ++trace.modelled_response_frames;
                }
                if (std::fabs(
                        result.control.observer_phase_command_x_counts) >
                        0.001f) {
                    ++trace.phase_aligned_command_frames;
                }
                if (result.control.observer_consistency_weight_x > 0.01f) {
                    ++trace.weighted_observer_frames;
                }
                expect(
                    result.control.observer_consistency_weight_x >= 0.0f &&
                        result.control.observer_consistency_weight_x <= 1.0f,
                    "连续观察器一致性权重必须位于 [0,1]");
            }
            const int command_x = result.has_command
                ? result.command.dx_counts : 0;
            if (result.has_command) {
                delayed_commands[index % kActuationDelayFrames] = command_x;
            }
            if (active_reverse_direction != 0) {
                const float base_error_x =
                    result.target.base_aim_x - frame.control_center_x;
                if (active_reverse_base_crossing_frame < 0 &&
                    base_error_x * active_reverse_direction > 2.25f) {
                    active_reverse_base_crossing_frame = index;
                }
                if (active_reverse_base_crossing_frame >= 0) {
                    if (command_x * active_reverse_direction > 0) {
                        const int latency_frames =
                            index - active_reverse_base_crossing_frame;
                        if (active_reverse_speed >= 400.0f) {
                            trace.maximum_high_speed_reverse_latency_frames =
                                std::max(
                                    trace.maximum_high_speed_reverse_latency_frames,
                                    latency_frames);
                        } else {
                            trace.maximum_low_speed_reverse_latency_frames =
                                std::max(
                                    trace.maximum_low_speed_reverse_latency_frames,
                                    latency_frames);
                        }
                        trace.maximum_reverse_zero_frames = std::max(
                            trace.maximum_reverse_zero_frames,
                            active_reverse_zero_frames);
                        active_reverse_direction = 0;
                        ++completed_true_reversals;
                    } else if (command_x == 0) {
                        ++active_reverse_zero_frames;
                    } else {
                        ++trace.wrong_direction_commands_after_base_crossing;
                    }
                }
            }
            if (index < 120) continue;

            true_errors.push_back(std::fabs(world_target_x - camera_x));
            observed_centers.push_back(
                observed_x + applied_pose * 3.0f);
            base_points.push_back(result.target.base_aim_x);
            final_points.push_back(result.target.aim_x);
            delay_offsets.push_back(result.target.delay_compensation_x);
            sample_frame_indices.push_back(index);
            // 不再跳过变速/换向后的 24 帧：真实方向切换本身只应贡献一次
            // 命令换向，边界窗口内的额外往返就是本任务要拦截的泵振。
            if (command_x != 0) {
                const int command_sign = command_x < 0 ? -1 : 1;
                if (previous_command_sign != 0 &&
                    command_sign != previous_command_sign) {
                    ++trace.command_reversals;
                    if (trace.first_command_reversal_context.empty()) {
                        trace.first_command_reversal_context =
                            "frame=" + std::to_string(index) +
                            ",cmd=" + std::to_string(command_x) +
                            ",error=" + std::to_string(
                                world_target_x - camera_x) +
                            ",vx=" + std::to_string(
                                result.target.velocity_x) +
                            ",base=" + std::to_string(
                                result.target.base_aim_x) +
                            ",final=" + std::to_string(
                                result.target.aim_x) +
                            ",delay=" + std::to_string(
                                result.target.delay_compensation_x);
                    }
                }
                previous_command_sign = command_sign;
            }
        }

        std::vector<float> input_second_differences;
        std::vector<float> base_second_differences;
        std::vector<float> final_second_differences;
        std::vector<float> delay_second_differences;
        std::vector<float> boundary_base_second_differences;
        std::vector<float> boundary_final_second_differences;
        for (std::size_t index = 2; index < final_points.size(); ++index) {
            input_second_differences.push_back(std::fabs(
                observed_centers[index] -
                2.0f * observed_centers[index - 1] +
                observed_centers[index - 2]));
            const float base_second = std::fabs(
                base_points[index] - 2.0f * base_points[index - 1] +
                base_points[index - 2]);
            const float final_second = std::fabs(
                final_points[index] - 2.0f * final_points[index - 1] +
                final_points[index - 2]);
            base_second_differences.push_back(base_second);
            final_second_differences.push_back(final_second);
            delay_second_differences.push_back(std::fabs(
                delay_offsets[index] - 2.0f * delay_offsets[index - 1] +
                delay_offsets[index - 2]));
            if (sample_frame_indices[index] % 120 < 24) {
                boundary_base_second_differences.push_back(base_second);
                boundary_final_second_differences.push_back(final_second);
            }
        }
        std::sort(true_errors.begin(), true_errors.end());
        std::sort(input_second_differences.begin(),
                  input_second_differences.end());
        std::sort(base_second_differences.begin(),
                  base_second_differences.end());
        std::sort(final_second_differences.begin(),
                  final_second_differences.end());
        std::sort(delay_second_differences.begin(),
                  delay_second_differences.end());
        std::sort(boundary_base_second_differences.begin(),
                  boundary_base_second_differences.end());
        std::sort(boundary_final_second_differences.begin(),
                  boundary_final_second_differences.end());
        trace.error_p95 = percentile(true_errors, 0.95f);
        trace.input_second_p95 =
            percentile(input_second_differences, 0.95f);
        trace.base_second_p95 =
            percentile(base_second_differences, 0.95f);
        trace.final_second_p95 =
            percentile(final_second_differences, 0.95f);
        trace.delay_second_p95 =
            percentile(delay_second_differences, 0.95f);
        trace.boundary_base_second_p95 =
            percentile(boundary_base_second_differences, 0.95f);
        trace.boundary_final_second_p95 =
            percentile(boundary_final_second_differences, 0.95f);
        trace.missed_true_reversals =
            expected_true_reversals - completed_true_reversals;
        return trace;
    };

    const Trace fixed_box = run_case(false);
    const Trace pose_box = run_case(true);
    expect(fixed_box.error_p95 <= 12.0f &&
               fixed_box.base_second_p95 <= 1.5f &&
               fixed_box.base_second_p95 -
                       fixed_box.input_second_p95 <= 1.5f &&
               fixed_box.final_second_p95 <= 2.0f &&
               fixed_box.delay_second_p95 <= 0.001f &&
               fixed_box.boundary_base_second_p95 <= 3.0f &&
               fixed_box.boundary_final_second_p95 <= 4.0f &&
               fixed_box.command_reversals <= 3 &&
               fixed_box.missed_true_reversals == 0 &&
               fixed_box.wrong_direction_commands_after_base_crossing == 0 &&
               fixed_box.maximum_high_speed_reverse_latency_frames <= 2 &&
               fixed_box.maximum_low_speed_reverse_latency_frames <= 2 &&
               fixed_box.maximum_reverse_zero_frames <= 2 &&
               fixed_box.legacy_reverse_state_frames == 0 &&
               fixed_box.modelled_response_frames == 0 &&
               fixed_box.phase_aligned_command_frames == 0 &&
               fixed_box.weighted_observer_frames == 0,
           "固定框双向变速闭环必须同时证明高速追赶、基础点连续和真实"
           "反向及时，误差P95/输入二阶P95/基础点二阶P95/最终点二阶P95/延迟二阶P95/"
           "边界基础二阶P95/边界最终二阶P95/全部反转/漏反转/过线后错向/"
           "高速反向延迟/低速反向延迟/反向零命令=" +
               std::to_string(fixed_box.error_p95) + "/" +
               std::to_string(fixed_box.input_second_p95) + "/" +
               std::to_string(fixed_box.base_second_p95) + "/" +
               std::to_string(fixed_box.final_second_p95) + "/" +
               std::to_string(fixed_box.delay_second_p95) + "/" +
               std::to_string(fixed_box.boundary_base_second_p95) + "/" +
               std::to_string(fixed_box.boundary_final_second_p95) + "/" +
               std::to_string(fixed_box.command_reversals) + "/" +
               std::to_string(fixed_box.missed_true_reversals) + "/" +
               std::to_string(
                   fixed_box.wrong_direction_commands_after_base_crossing) +
               "/" + std::to_string(
                   fixed_box.maximum_high_speed_reverse_latency_frames) +
               "/" + std::to_string(
                   fixed_box.maximum_low_speed_reverse_latency_frames) +
               "/" + std::to_string(fixed_box.maximum_reverse_zero_frames) +
               "，首个反转=" +
               fixed_box.first_command_reversal_context);
    expect(pose_box.error_p95 <= 23.0f &&
               pose_box.base_second_p95 <= 4.25f &&
               pose_box.base_second_p95 -
                       pose_box.input_second_p95 <= 3.75f &&
               pose_box.final_second_p95 <= 5.0f &&
               pose_box.final_second_p95 -
                       pose_box.input_second_p95 <= 4.25f &&
               pose_box.delay_second_p95 <= 0.001f &&
               pose_box.boundary_base_second_p95 <= 6.0f &&
               pose_box.boundary_final_second_p95 <= 7.0f &&
               pose_box.command_reversals <= 30 &&
               pose_box.missed_true_reversals == 0 &&
               pose_box.wrong_direction_commands_after_base_crossing == 0 &&
               pose_box.maximum_high_speed_reverse_latency_frames <= 2 &&
               pose_box.maximum_low_speed_reverse_latency_frames <= 2 &&
               pose_box.maximum_reverse_zero_frames <= 2 &&
               pose_box.legacy_reverse_state_frames == 0 &&
               pose_box.modelled_response_frames == 0 &&
               pose_box.phase_aligned_command_frames == 0 &&
               pose_box.weighted_observer_frames == 0 &&
               pose_box.error_p95 - fixed_box.error_p95 <= 12.0f &&
               pose_box.base_second_p95 -
                       fixed_box.base_second_p95 <= 3.75f &&
               pose_box.final_second_p95 -
                       fixed_box.final_second_p95 <= 4.25f,
           "同一闭环叠加姿态形变后只能增加有界歧义，不能恢复"
           "高频泵振或阻塞真实反向，误差P95/输入二阶P95/基础点二阶P95/最终点二阶P95/"
           "延迟二阶P95/边界基础二阶P95/边界最终二阶P95/全部反转/漏反转/"
           "过线后错向/高速反向延迟/低速反向延迟/反向零命令/"
           "相对固定框误差增量/基础二阶增量/最终二阶增量=" +
               std::to_string(pose_box.error_p95) + "/" +
               std::to_string(pose_box.input_second_p95) + "/" +
               std::to_string(pose_box.base_second_p95) + "/" +
               std::to_string(pose_box.final_second_p95) + "/" +
               std::to_string(pose_box.delay_second_p95) + "/" +
               std::to_string(pose_box.boundary_base_second_p95) + "/" +
               std::to_string(pose_box.boundary_final_second_p95) + "/" +
               std::to_string(pose_box.command_reversals) + "/" +
               std::to_string(pose_box.missed_true_reversals) + "/" +
               std::to_string(
                   pose_box.wrong_direction_commands_after_base_crossing) +
               "/" + std::to_string(
                   pose_box.maximum_high_speed_reverse_latency_frames) +
               "/" + std::to_string(
                   pose_box.maximum_low_speed_reverse_latency_frames) +
               "/" + std::to_string(pose_box.maximum_reverse_zero_frames) +
               "/" +
               std::to_string(pose_box.error_p95 - fixed_box.error_p95) +
               "/" +
               std::to_string(pose_box.base_second_p95 -
                              fixed_box.base_second_p95) +
               "/" +
               std::to_string(pose_box.final_second_p95 -
                              fixed_box.final_second_p95) +
               "，首个反转=" +
               pose_box.first_command_reversal_context);
}

void test_vertical_shape_noise_does_not_stutter_horizontal_tracking() {
    constexpr int kActuationDelayFrames = 4;
    constexpr int kMotionPeriodFrames = 44;
    constexpr float kPi = 3.14159265358979323846f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.acquisition_range_percent = 100.0f;
    config.body_aim_height_ratio = 0.50f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float camera_x = 0.0f;
    int active_starvation_frames = 0;
    int maximum_starvation_frames = 0;
    int starvation_episodes = 0;
    int reverse_gate_frames = 0;
    std::vector<float> closed_loop_errors;

    for (int index = 0; index < 1320; ++index) {
        camera_x += delayed_commands[index % kActuationDelayFrames] /
            config.counts_per_pixel_x * 0.20f;
        delayed_commands[index % kActuationDelayFrames] = 0;
        // 44 帧周期对应本轮真机基础点 5.39 Hz 主峰；幅值按 ROI 比例
        // 表达。高度轮廓独立变化而水平宽度固定，专门验证 Y 形变不得
        // 冻结 X 位置后验。
        const float world_target_x = 320.0f * 0.10f * std::sin(
            2.0f * kPi * static_cast<float>(index) /
            static_cast<float>(kMotionPeriodFrames));
        const int height_phase_index = index % 26;
        const float height_phase = height_phase_index <= 13
            ? -1.0f + static_cast<float>(height_phase_index) *
                (2.0f / 13.0f)
            : 1.0f - static_cast<float>(height_phase_index - 13) *
                (2.0f / 13.0f);
        const float observed_x = 160.0f + world_target_x - camera_x;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.control_at =
            frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            observed_x,
            160.0f,
            42.0f,
            90.0f + height_phase * 1.8f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "周期往返闭环必须逐帧保留确认目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[index % kActuationDelayFrames] = command_x;
        }
        if (index < 120) continue;

        closed_loop_errors.push_back(std::fabs(world_target_x - camera_x));
        if (result.control.reverse_gate_blocked_x) {
            ++reverse_gate_frames;
        }
        const bool controller_requests_motion =
            std::fabs(result.control.desired_before_reverse_x_counts) >= 1.0f;
        const bool visible_error = std::fabs(
            result.target.base_aim_x - frame.control_center_x) >
            config.deadzone_pixels;
        if (controller_requests_motion && visible_error && command_x == 0) {
            if (active_starvation_frames == 0) {
                ++starvation_episodes;
            }
            ++active_starvation_frames;
            maximum_starvation_frames = std::max(
                maximum_starvation_frames, active_starvation_frames);
        } else {
            active_starvation_frames = 0;
        }
    }

    std::sort(closed_loop_errors.begin(), closed_loop_errors.end());
    const float error_p95 = closed_loop_errors[
        std::min(
            closed_loop_errors.size() - 1,
            static_cast<std::size_t>(closed_loop_errors.size() * 0.95f))];
    expect(maximum_starvation_frames <= 2 && reverse_gate_frames == 0 &&
               error_p95 <= 47.0f,
           "5.45 Hz 真机同构往返中，控制器已有非零需求时不得被反向"
           "判定切成连续停发片段，最长停发/停发episode/反向门帧/误差P95=" +
               std::to_string(maximum_starvation_frames) + "/" +
               std::to_string(starvation_episodes) + "/" +
               std::to_string(reverse_gate_frames) + "/" +
               std::to_string(error_p95));
}

void test_backend_completed_command_feedback_contract() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 1.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.control_at = base + std::chrono::milliseconds(1);
    first.lock_active = true;
    first.detections = {body(200.0f, 160.0f)};
    const AimResult first_result = aim.process(first);
    expect(first_result.status == AimStatus::SUCCESS &&
               first_result.has_command,
           "后端完成反馈契约必须先生成一条可发送命令");
    const auto first_backend_completed_at =
        first.control_at + std::chrono::microseconds(100);
    expect(!aim.record_backend_completed_command(
               first.sequence,
               first.control_at - std::chrono::microseconds(1),
               first_result.command.dx_counts,
               first_result.command.dy_counts),
           "实际完成时刻早于控制计算时刻必须拒绝");
    expect(!aim.record_backend_completed_command(
               first.sequence,
               first_backend_completed_at,
               first_result.command.dx_counts + 1,
               first_result.command.dy_counts),
           "非零后端完成命令与预计算命令不一致时必须拒绝");
    expect(aim.record_backend_completed_command(
               first.sequence,
               first_backend_completed_at,
               first_result.command.dx_counts,
               first_result.command.dy_counts),
           "Mouse 成功后必须按同一帧序号确认实际整数命令");
    expect(!aim.record_backend_completed_command(
               first.sequence,
               first_backend_completed_at,
               first_result.command.dx_counts,
               first_result.command.dy_counts),
           "同一帧后端完成命令不得重复确认");

    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(4));
    second.control_at = base + std::chrono::milliseconds(5);
    second.lock_active = true;
    second.detections = {body(200.0f, 160.0f)};
    const AimResult second_result = aim.process(second);
    expect(second_result.has_command &&
               aim.record_backend_completed_command(
                   second.sequence,
                   second.control_at + std::chrono::microseconds(100),
                   0,
                   0),
           "Mouse 失败或二次安全门拒绝时必须把预计算命令确认为零");
    expect(!aim.record_backend_completed_command(
               999,
               second.control_at + std::chrono::microseconds(200),
               0,
               0),
           "不存在的帧序号不得写入命令历史");
}

void test_backend_completed_delay_inventory_changes_tracking_reversal_response() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr float kObservedRoundTripHz = 8.0f;
    constexpr int kFrameCount = 360;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 2.25f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;

    Aim completed_history(config);
    Aim rejected_history(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    int opposed_inventory_frames = 0;
    int history_sensitive_frames = 0;
    int premature_reversal_events = 0;
    int previous_completed_sign = 0;

    for (int index = 0; index < kFrameCount; ++index) {
        const float elapsed_seconds = index * kFrameSeconds;
        const float observed_error = 12.0f * std::sin(
            2.0f * 3.14159265358979323846f * kObservedRoundTripHz *
            elapsed_seconds);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(elapsed_seconds * 1000000.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body(160.0f + observed_error, 160.0f)};

        const AimResult completed_result = completed_history.process(frame);
        const AimResult rejected_result = rejected_history.process(frame);
        expect(completed_result.status == AimStatus::SUCCESS &&
                   completed_result.has_target &&
                   rejected_result.status == AimStatus::SUCCESS &&
                   rejected_result.has_target,
               "delayed-command 反事实每帧都必须保留同一公开目标");

        const int completed_command = completed_result.has_command
            ? completed_result.command.dx_counts : 0;
        const int rejected_command = rejected_result.has_command
            ? rejected_result.command.dx_counts : 0;
        const float pending_x =
            completed_result.control.pending_net_x_counts;
        if (pending_x * observed_error < 0.0f) {
            ++opposed_inventory_frames;
            if (completed_command != rejected_command) {
                ++history_sensitive_frames;
            }
        }

        const int completed_sign = completed_command > 0
            ? 1 : (completed_command < 0 ? -1 : 0);
        if (completed_sign != 0) {
            if (previous_completed_sign != 0 &&
                completed_sign != previous_completed_sign &&
                pending_x * static_cast<float>(previous_completed_sign) >
                    0.0f) {
                ++premature_reversal_events;
            }
            previous_completed_sign = completed_sign;
        }

        const auto backend_completed_at =
            frame.control_at + std::chrono::microseconds(100);
        if (completed_result.has_command) {
            expect(completed_history.record_backend_completed_command(
                       frame.sequence, backend_completed_at,
                       completed_result.command.dx_counts,
                       completed_result.command.dy_counts),
                   "delayed-command 夹具必须写入真实后端完成命令");
        }
        if (rejected_result.has_command) {
            expect(rejected_history.record_backend_completed_command(
                       frame.sequence, backend_completed_at, 0, 0),
                   "反事实必须把同一预计算命令确认为未实际执行");
        }
    }

    const float measured_round_trip_hz =
        premature_reversal_events /
        (2.0f * kFrameCount * kFrameSeconds);
    expect(opposed_inventory_frames >= 12,
           "delayed-command 夹具必须稳定覆盖当前误差已换边、15 ms 窗内"
           "仍有旧向 backend-completed 库存，实际帧数=" +
               std::to_string(opposed_inventory_frames));
    expect(history_sensitive_frames > 0,
           "同一图像误差下，窗内旧向 backend-completed 历史不能只进入诊断；"
           "旧向库存帧/控制响应差异帧/旧向库存下提前反向/完整往返Hz=" +
               std::to_string(opposed_inventory_frames) + "/" +
               std::to_string(history_sensitive_frames) + "/" +
               std::to_string(premature_reversal_events) + "/" +
               std::to_string(measured_round_trip_hz));
}

void test_faster_closing_slope_continuously_reduces_tracking_request() {
    constexpr int kWarmupFrames = 40;
    constexpr int kFrameCount = kWarmupFrames + 2;
    constexpr auto kFrameStep = std::chrono::microseconds(4167);
    constexpr float kWarmupError = 6.0f;
    constexpr float kCurrentError = 1.5f;
    constexpr float kStationaryPreviousError = kCurrentError;
    constexpr float kFasterClosingPreviousError = 2.25f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 2.25f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;

    struct Sample {
        AimResult previous;
        AimResult current;
        int valid_frames = 0;
        int completed_frames = 0;
    };
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    const auto run_case = [&](float previous_error) {
        Aim aim(config);
        Sample sample;
        for (int index = 0; index < kFrameCount; ++index) {
            const float error = index < kWarmupFrames
                ? kWarmupError
                : (index == kWarmupFrames ? previous_error : kCurrentError);
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + kFrameStep * index);
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.control_center_x = 180.0f - error;
            frame.lock_active = true;
            // 身体中心 Y=172 时，默认 0.35 高度的基础点恰为 160，
            // 使成对夹具只改变 X 图像误差。
            frame.detections = {body(180.0f, 172.0f)};
            const AimResult result = aim.process(frame);
            if (result.status == AimStatus::SUCCESS && result.has_target &&
                result.control.evaluated) {
                ++sample.valid_frames;
            }
            if (index == kWarmupFrames) sample.previous = result;
            if (index == kFrameCount - 1) {
                sample.current = result;
                continue;
            }
            if (result.has_command && aim.record_backend_completed_command(
                    frame.sequence,
                    frame.control_at + std::chrono::microseconds(100),
                    result.command.dx_counts,
                    result.command.dy_counts)) {
                ++sample.completed_frames;
            }
        }
        return sample;
    };

    const Sample stationary = run_case(kStationaryPreviousError);
    const Sample faster_closing = run_case(kFasterClosingPreviousError);
    const auto base_error_x = [](const AimResult& result,
                                 float control_center_x) {
        return result.target.base_aim_x - control_center_x;
    };
    const float stationary_previous_error = base_error_x(
        stationary.previous, 180.0f - kStationaryPreviousError);
    const float faster_previous_error = base_error_x(
        faster_closing.previous, 180.0f - kFasterClosingPreviousError);
    const float stationary_current_error = base_error_x(
        stationary.current, 180.0f - kCurrentError);
    const float faster_current_error = base_error_x(
        faster_closing.current, 180.0f - kCurrentError);

    expect(stationary.valid_frames == kFrameCount &&
               faster_closing.valid_frames == kFrameCount &&
               stationary.completed_frames == kFrameCount - 1 &&
               faster_closing.completed_frames == kFrameCount - 1,
           "closing-slope 成对夹具必须逐帧保留同一公开目标、控制求值和"
           "backend-completed 命令，帧/完成=" +
               std::to_string(stationary.valid_frames) + "/" +
               std::to_string(faster_closing.valid_frames) + "/" +
               std::to_string(stationary.completed_frames) + "/" +
               std::to_string(faster_closing.completed_frames));
    expect(std::fabs(stationary_current_error - faster_current_error) <
                   0.0001f &&
               std::fabs(stationary_current_error - kCurrentError) <
                   0.0001f &&
               std::fabs(stationary_previous_error -
                         stationary_current_error) < 0.0001f &&
               faster_previous_error > faster_current_error,
           "成对序列最终基础误差必须相同，且只由第二条序列提供朝零闭合"
           "斜率，前帧/当前误差=" +
               std::to_string(stationary_previous_error) + "/" +
               std::to_string(faster_previous_error) + "/" +
               std::to_string(stationary_current_error) + "/" +
               std::to_string(faster_current_error));
    expect(stationary.current.range_locked &&
               faster_closing.current.range_locked &&
               stationary.current.range_allows_control &&
               faster_closing.current.range_allows_control &&
               stationary.current.target.track_id ==
                   faster_closing.current.target.track_id &&
               std::fabs(stationary.current.target.base_aim_x -
                         faster_closing.current.target.base_aim_x) < 0.0001f &&
               std::fabs(stationary.current.target.base_aim_y -
                         faster_closing.current.target.base_aim_y) < 0.0001f &&
               std::fabs(stationary.current.target.x1 -
                         faster_closing.current.target.x1) < 0.0001f &&
               std::fabs(stationary.current.target.x2 -
                         faster_closing.current.target.x2) < 0.0001f &&
               std::fabs(stationary.current.control.controller_dt_ms -
                         faster_closing.current.control.controller_dt_ms) <
                   0.0001f,
           "closing-slope 分叉不得改变锁定、目标/锚点身份、框几何或真实 dt，"
           "track/base/dt=" +
               std::to_string(stationary.current.target.track_id) + "/" +
               std::to_string(faster_closing.current.target.track_id) +
               "/" +
               std::to_string(stationary.current.target.base_aim_x) + "/" +
               std::to_string(faster_closing.current.target.base_aim_x) +
               "/" +
               std::to_string(
                   stationary.current.control.controller_dt_ms) + "/" +
               std::to_string(
                   faster_closing.current.control.controller_dt_ms));
    expect(std::fabs(stationary.current.control.proportional_x_counts) <
                   0.0001f &&
               std::fabs(faster_closing.current.control.
                             proportional_x_counts) < 0.0001f &&
               std::fabs(stationary.current.control.feedforward_x_counts -
                         faster_closing.current.control.
                             feedforward_x_counts) < 0.0001f &&
               std::fabs(stationary.current.control.filtered_x_counts -
                         faster_closing.current.control.filtered_x_counts) <
                   0.0001f,
           "死区内分叉不得改变现有 PI 积分或分轴 smoothing 历史，P/积分/"
           "滤波=" +
               std::to_string(
                   stationary.current.control.proportional_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.proportional_x_counts) +
               "/" +
               std::to_string(
                   stationary.current.control.feedforward_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.feedforward_x_counts) +
               "/" +
               std::to_string(
                   stationary.current.control.filtered_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.filtered_x_counts));
    expect(std::fabs(stationary.current.control.pending_net_x_counts -
                     faster_closing.current.control.pending_net_x_counts) <
                   0.0001f &&
               std::fabs(stationary.current.control.pending_absolute_x_counts -
                         faster_closing.current.control.
                             pending_absolute_x_counts) < 0.0001f &&
               stationary.current.control.pending_net_x_counts > 0.0f &&
               stationary.current.control.pending_absolute_x_counts > 0.0f,
           "成对序列必须保留相同且非零的 15 ms backend-completed 库存，"
           "net/absolute=" +
               std::to_string(
                   stationary.current.control.pending_net_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.pending_net_x_counts) +
               "/" +
               std::to_string(
                   stationary.current.control.pending_absolute_x_counts) +
               "/" +
               std::to_string(
                   faster_closing.current.control.pending_absolute_x_counts));
    expect(stationary.current.control.desired_x_counts > 0.0f &&
               faster_closing.current.control.desired_x_counts >= 0.0f,
           "closing-slope 阻尼只能收缩当前误差同向请求，不得自行反向，"
           "静止/闭合请求=" +
               std::to_string(
                   stationary.current.control.desired_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.desired_x_counts));
    expect(std::fabs(faster_closing.current.control.desired_x_counts) <
               std::fabs(stationary.current.control.desired_x_counts),
           "最终误差、PI 历史、smoothing 与 backend-completed 库存等价时，"
           "更快朝零闭合必须连续减小 X 请求幅值，静止/闭合请求=" +
               std::to_string(
                   stationary.current.control.desired_x_counts) + "/" +
               std::to_string(
                   faster_closing.current.control.desired_x_counts));
}

void test_same_direction_completed_inventory_brakes_closing_request() {
    constexpr int kWarmupFrames = 40;
    constexpr int kFrameCount = kWarmupFrames + 2;
    constexpr auto kFrameStep = std::chrono::microseconds(4167);
    constexpr float kWarmupError = 6.0f;
    constexpr float kPreviousError = 2.25f;
    constexpr float kCurrentError = 1.5f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 2.25f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;

    struct Sample {
        AimResult previous;
        AimResult current;
        int valid_frames = 0;
        int completed_frames = 0;
    };
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    const auto run_case = [&](bool complete_previous_command) {
        Aim aim(config);
        Sample sample;
        for (int index = 0; index < kFrameCount; ++index) {
            const float error = index < kWarmupFrames
                ? kWarmupError
                : (index == kWarmupFrames ? kPreviousError : kCurrentError);
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + kFrameStep * index);
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.control_center_x = 180.0f - error;
            frame.lock_active = true;
            frame.detections = {body(180.0f, 172.0f)};
            const AimResult result = aim.process(frame);
            if (result.status == AimStatus::SUCCESS && result.has_target &&
                result.control.evaluated) {
                ++sample.valid_frames;
            }
            if (index == kWarmupFrames) sample.previous = result;
            if (index == kFrameCount - 1) {
                sample.current = result;
                continue;
            }
            if (result.has_command) {
                const bool keep_previous = index != kWarmupFrames ||
                    complete_previous_command;
                expect(aim.record_backend_completed_command(
                           frame.sequence,
                           frame.control_at + std::chrono::microseconds(100),
                           keep_previous ? result.command.dx_counts : 0,
                           keep_previous ? result.command.dy_counts : 0),
                       "same-direction 反事实必须逐帧确认同一预计算命令");
                ++sample.completed_frames;
            }
        }
        return sample;
    };

    const Sample completed = run_case(true);
    const Sample rejected = run_case(false);
    expect(completed.valid_frames == kFrameCount &&
               rejected.valid_frames == kFrameCount &&
               completed.completed_frames == kFrameCount - 1 &&
               rejected.completed_frames == kFrameCount - 1,
           "same-direction 成对夹具必须逐帧保留公开目标、控制求值和"
           "backend completion，帧/完成=" +
               std::to_string(completed.valid_frames) + "/" +
               std::to_string(rejected.valid_frames) + "/" +
               std::to_string(completed.completed_frames) + "/" +
               std::to_string(rejected.completed_frames));
    expect(completed.previous.has_command &&
               rejected.previous.has_command &&
               completed.previous.command.dx_counts > 0 &&
               completed.previous.command.dx_counts ==
                   rejected.previous.command.dx_counts,
           "分叉前一帧必须产生相同且朝当前误差的非零 X 命令，completed/"
           "rejected=" +
               std::to_string(completed.previous.command.dx_counts) + "/" +
               std::to_string(rejected.previous.command.dx_counts));
    expect(completed.current.target.track_id ==
                   rejected.current.target.track_id &&
               std::fabs(completed.current.target.base_aim_x -
                         rejected.current.target.base_aim_x) < 0.0001f &&
               std::fabs(completed.current.target.base_aim_y -
                         rejected.current.target.base_aim_y) < 0.0001f &&
               std::fabs(completed.current.control.proportional_x_counts -
                         rejected.current.control.proportional_x_counts) <
                   0.0001f &&
               std::fabs(completed.current.control.feedforward_x_counts -
                         rejected.current.control.feedforward_x_counts) <
                   0.0001f &&
               std::fabs(completed.current.control.filtered_x_counts -
                         rejected.current.control.filtered_x_counts) <
                   0.0001f,
           "成对分叉只能改变当前 15 ms 窗内同向完成库存，不得改变目标、"
           "基础点、PI 或 smoothing 历史");
    expect(completed.current.control.pending_net_x_counts >
                   rejected.current.control.pending_net_x_counts &&
               completed.current.control.pending_net_x_counts > 0.0f,
           "completed 分支必须比 rejected 分支多一项当前误差同向库存，"
           "completed/rejected=" +
               std::to_string(
                   completed.current.control.pending_net_x_counts) + "/" +
               std::to_string(
                   rejected.current.control.pending_net_x_counts));
    expect(completed.current.control.desired_x_counts >= 0.0f &&
               rejected.current.control.desired_x_counts > 0.0f,
           "同向完成库存只能制动当前同向请求，不得自行生成反向，"
           "completed/rejected=" +
               std::to_string(
                   completed.current.control.desired_x_counts) + "/" +
               std::to_string(rejected.current.control.desired_x_counts));
    expect(std::fabs(completed.current.control.desired_x_counts) <
               std::fabs(rejected.current.control.desired_x_counts),
           "相同闭合轨迹下，15 ms 窗内新增同向 backend-completed 库存必须"
           "连续减小 X 请求，completed/rejected=" +
               std::to_string(
                   completed.current.control.desired_x_counts) + "/" +
               std::to_string(rejected.current.control.desired_x_counts));
}

void test_fixed_scene_replay_does_not_amplify_horizontal_observation() {
    using aim_fixed_scene_replay_fixture::kMeasurementStart;
    using aim_fixed_scene_replay_fixture::kObservations;

    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    Aim aim(config);
    const auto started_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::vector<float> observation_x_differences;
    std::vector<float> track_x_differences;
    std::vector<float> base_x_differences;
    std::vector<float> observation_y_differences;
    std::vector<float> base_y_differences;
    struct ReplayDifference {
        std::size_t index = 0;
        float observation_x = 0.0f;
        float track_x = 0.0f;
        float base_x = 0.0f;
        float velocity_x = 0.0f;
    };
    std::vector<ReplayDifference> replay_differences;
    float previous_observation_x = 0.0f;
    float previous_observation_y = 0.0f;
    float previous_track_x = 0.0f;
    float previous_base_x = 0.0f;
    float previous_base_y = 0.0f;

    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            started_at + std::chrono::microseconds(
                static_cast<long long>(index) * 4167));
        frame.control_at = frame.captured_at + std::chrono::microseconds(
            static_cast<long long>(observation.observation_age_ms * 1000.0f));
        frame.lock_active = true;
        frame.detections = {{observation.x1,
                             observation.y1,
                             observation.x2,
                             observation.y2,
                             0.9f,
                             0}};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "固定场景真实 Observation 回放必须逐帧处理成功");
        if (!result.has_target) continue;
        if (result.has_command) {
            expect(aim.record_backend_completed_command(
                       frame.sequence,
                       frame.control_at + std::chrono::microseconds(400),
                       result.command.dx_counts,
                       result.command.dy_counts),
                   "固定场景真实回放必须沿公开 seam 记录整数完成命令");
        }
        const float observation_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_y =
            (observation.y1 + observation.y2) * 0.5f;
        const float track_x =
            (result.target.x1 + result.target.x2) * 0.5f;
        if (index > kMeasurementStart) {
            observation_x_differences.push_back(
                std::fabs(observation_x - previous_observation_x));
            track_x_differences.push_back(
                std::fabs(track_x - previous_track_x));
            base_x_differences.push_back(
                std::fabs(result.target.base_aim_x - previous_base_x));
            observation_y_differences.push_back(
                std::fabs(observation_y - previous_observation_y));
            base_y_differences.push_back(
                std::fabs(result.target.base_aim_y - previous_base_y));
            replay_differences.push_back({
                index,
                observation_x - previous_observation_x,
                track_x - previous_track_x,
                result.target.base_aim_x - previous_base_x,
                result.target.velocity_x});
        }
        previous_observation_x = observation_x;
        previous_observation_y = observation_y;
        previous_track_x = track_x;
        previous_base_x = result.target.base_aim_x;
        previous_base_y = result.target.base_aim_y;
    }

    const auto percentile = [](std::vector<float> values, float quantile) {
        std::sort(values.begin(), values.end());
        const float position = quantile * (values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(
            std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(
            std::ceil(position));
        const float fraction = position - static_cast<float>(lower);
        return values[lower] * (1.0f - fraction) +
            values[upper] * fraction;
    };
    const float observation_x_p95 =
        percentile(observation_x_differences, 0.95f);
    const float track_x_p95 = percentile(track_x_differences, 0.95f);
    const float base_x_p95 = percentile(base_x_differences, 0.95f);
    const float observation_y_p95 =
        percentile(observation_y_differences, 0.95f);
    const float base_y_p95 = percentile(base_y_differences, 0.95f);
    auto largest_base_differences = replay_differences;
    std::sort(
        largest_base_differences.begin(), largest_base_differences.end(),
        [](const ReplayDifference& left, const ReplayDifference& right) {
            return std::fabs(left.base_x) > std::fabs(right.base_x);
        });
    std::ostringstream replay_spikes;
    const std::size_t reported_spikes = std::min<std::size_t>(
        5, largest_base_differences.size());
    for (std::size_t index = 0; index < reported_spikes; ++index) {
        const auto& difference = largest_base_differences[index];
        replay_spikes << " [i=" << difference.index
                      << ",obs=" << difference.observation_x
                      << ",track=" << difference.track_x
                      << ",base=" << difference.base_x
                      << ",vx=" << difference.velocity_x << ']';
    }
    expect(observation_x_differences.size() ==
               kObservations.size() - kMeasurementStart - 1 &&
               track_x_p95 <= observation_x_p95 + 0.02f,
           "真实回放 fixture 必须覆盖原始 X 往返，且 Track 本身不放大 "
           "Observation，frames/Observation/Track P95=" +
               std::to_string(observation_x_differences.size()) + "/" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(track_x_p95));
    expect(base_x_p95 <= observation_x_p95,
           "固定场景公开 Aim 回放不得让 base X 一阶变化放大 matched "
           "Observation，Observation/Track/base P95=" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(track_x_p95) + "/" +
               std::to_string(base_x_p95) +
               "，最大跳变=" + replay_spikes.str());
    expect(base_y_p95 <= observation_y_p95,
           "X-only green 必须保留 Y 起跳/腾空/落地连续性，Observation/base "
           "Y P95=" + std::to_string(observation_y_p95) + "/" +
               std::to_string(base_y_p95));
    std::cout << "固定场景公开回放 P95: Observation/Track/base X="
              << observation_x_p95 << '/' << track_x_p95 << '/'
              << base_x_p95 << "，Observation/base Y="
              << observation_y_p95 << '/' << base_y_p95 << '\n';
}

void test_static_closed_loop_replay_does_not_repeat_horizontal_commands() {
    using aim_static_closed_loop_replay_fixture::kMeasurementStart;
    using aim_static_closed_loop_replay_fixture::kObservations;

    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    Aim aim(config);

    const auto started_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto control_at = started_at;
    int previous_nonzero_direction = 0;
    int command_direction_reversals = 0;
    int absolute_x_commands = 0;
    int command_direction_violations = 0;
    std::vector<float> observation_x_differences;
    std::vector<float> base_x_differences;
    std::vector<float> observation_y_differences;
    std::vector<float> base_y_differences;
    float previous_observation_x = 0.0f;
    float previous_observation_y = 0.0f;
    float previous_base_x = 0.0f;
    float previous_base_y = 0.0f;

    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        if (index != 0) {
            control_at += std::chrono::nanoseconds(
                static_cast<long long>(observation.controller_dt_ms *
                                       1000000.0f));
        }
        const auto captured_at = control_at - std::chrono::nanoseconds(
            static_cast<long long>(observation.observation_age_ms *
                                   1000000.0f));
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), captured_at);
        frame.control_at = control_at;
        frame.lock_active = true;
        frame.detections = {{observation.x1,
                             observation.y1,
                             observation.x2,
                             observation.y2,
                             0.9f,
                             0}};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "静态闭环真实 Observation 必须逐帧经公开 Aim seam 成功处理，"
               "index/status=" + std::to_string(index) + "/" +
                   std::to_string(static_cast<int>(result.status)));
        if (!result.has_target) continue;
        if (result.has_command) {
            expect(aim.record_backend_completed_command(
                       frame.sequence,
                       frame.control_at + std::chrono::microseconds(
                           static_cast<long long>(
                               observation.backend_completion_ms *
                               1000.0f)),
                       result.command.dx_counts,
                       result.command.dy_counts),
                   "静态闭环回放必须写回同序列的公开整数完成命令");
        }

        const float observation_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_y =
            (observation.y1 + observation.y2) * 0.5f;
        if (index > kMeasurementStart) {
            observation_x_differences.push_back(
                std::fabs(observation_x - previous_observation_x));
            base_x_differences.push_back(
                std::fabs(result.target.base_aim_x - previous_base_x));
            observation_y_differences.push_back(
                std::fabs(observation_y - previous_observation_y));
            base_y_differences.push_back(
                std::fabs(result.target.base_aim_y - previous_base_y));
            absolute_x_commands += std::abs(result.command.dx_counts);
            if (result.has_command) {
                const float error_x = result.target.aim_x -
                    frame.control_center_x;
                const float error_y = result.target.aim_y -
                    frame.control_center_y;
                const float command_dot_error =
                    result.command.dx_counts * error_x +
                    result.command.dy_counts * error_y;
                const float command_magnitude = std::hypot(
                    static_cast<float>(result.command.dx_counts),
                    static_cast<float>(result.command.dy_counts));
                if (command_dot_error <= 0.0f ||
                    command_magnitude > config.max_counts_per_frame + 0.001f) {
                    ++command_direction_violations;
                }
            }
            const int direction = result.command.dx_counts > 0
                ? 1 : (result.command.dx_counts < 0 ? -1 : 0);
            if (direction != 0) {
                if (previous_nonzero_direction != 0 &&
                    direction != previous_nonzero_direction) {
                    ++command_direction_reversals;
                }
                previous_nonzero_direction = direction;
            }
        }
        previous_observation_x = observation_x;
        previous_observation_y = observation_y;
        previous_base_x = result.target.base_aim_x;
        previous_base_y = result.target.base_aim_y;
    }

    const auto percentile = [](std::vector<float> values, float quantile) {
        std::sort(values.begin(), values.end());
        const float position = quantile * (values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(
            std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(
            std::ceil(position));
        const float fraction = position - static_cast<float>(lower);
        return values[lower] * (1.0f - fraction) +
            values[upper] * fraction;
    };
    const float observation_x_p95 =
        percentile(observation_x_differences, 0.95f);
    const float base_x_p95 = percentile(base_x_differences, 0.95f);
    const float observation_y_p95 =
        percentile(observation_y_differences, 0.95f);
    const float base_y_p95 = percentile(base_y_differences, 0.95f);
    expect(base_x_p95 <= observation_x_p95 &&
               base_y_p95 <= observation_y_p95,
           "静态闭环新 red 必须保留既有 base 不放大 Observation 的 X/Y "
           "前提，Observation/base P95 X/Y=" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(base_x_p95) + "/" +
               std::to_string(observation_y_p95) + "/" +
               std::to_string(base_y_p95));
    expect(command_direction_reversals <= 5 &&
               absolute_x_commands <= 400 &&
               base_x_p95 <= observation_x_p95 * (2.0f / 3.0f) &&
               command_direction_violations == 0,
           "人工已声明世界 X 静止的稳定测量段必须同时减少交替 X 命令、"
           "绝对命令量和 base 对 Observation 的跟随变化，并保持二维"
           "命令方向合同，反向/绝对命令/方向违规/Observation-base P95=" +
               std::to_string(command_direction_reversals) + "/" +
               std::to_string(absolute_x_commands) + "/" +
               std::to_string(command_direction_violations) + "/" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(base_x_p95));
    std::cout << "静态闭环公开回放: X命令反向/绝对量="
              << command_direction_reversals << '/' << absolute_x_commands
              << "，方向违规=" << command_direction_violations
              << "，Observation/base P95 X=" << observation_x_p95 << '/'
              << base_x_p95 << "，Y=" << observation_y_p95 << '/'
              << base_y_p95 << '\n';
}

void test_latest_static_replay_does_not_amplify_horizontal_base() {
    using aim_latest_static_replay_fixture::kAimFromHead;
    using aim_latest_static_replay_fixture::kMeasurementStart;
    using aim_latest_static_replay_fixture::kObservations;

    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    Aim aim(config);

    auto control_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::vector<float> observation_x_differences;
    std::vector<float> base_x_differences;
    std::vector<float> observation_y_differences;
    std::vector<float> base_y_differences;
    float previous_observation_x = 0.0f;
    float previous_observation_y = 0.0f;
    float previous_base_x = 0.0f;
    float previous_base_y = 0.0f;

    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        if (index != 0) {
            control_at += std::chrono::nanoseconds(
                static_cast<long long>(observation.controller_dt_ms *
                                       1000000.0f));
        }
        const auto captured_at = control_at - std::chrono::nanoseconds(
            static_cast<long long>(observation.observation_age_ms *
                                   1000000.0f));
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), captured_at);
        frame.control_at = control_at;
        frame.lock_active = true;
        frame.detections = {{observation.x1,
                             observation.y1,
                             observation.x2,
                             observation.y2,
                             0.9f,
                             0}};
        if (kAimFromHead[index]) {
            const float center_x =
                (observation.x1 + observation.x2) * 0.5f;
            const float head_width =
                (observation.x2 - observation.x1) * 0.5f;
            frame.detections.push_back(head_box(
                center_x, observation.y1 + 6.0f,
                head_width, 10.0f));
        }
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "最新人工失败段必须逐帧经公开 Aim seam 成功回放");
        if (!result.has_target) continue;
        expect(result.target.matched_observation_valid &&
                   result.target.matched_observation_aim_from_head ==
                       kAimFromHead[index],
               "最新人工失败段必须逐帧复现 Runtime 的 body/head 关联语义");
        if (result.has_command) {
            expect(aim.record_backend_completed_command(
                       frame.sequence,
                       frame.control_at + std::chrono::microseconds(
                           static_cast<long long>(
                               observation.backend_completion_ms *
                               1000.0f)),
                       result.command.dx_counts,
                       result.command.dy_counts),
                   "最新人工失败段必须写回同序列的公开整数完成命令");
        }

        const float observation_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_y =
            (observation.y1 + observation.y2) * 0.5f;
        if (index > kMeasurementStart) {
            observation_x_differences.push_back(
                std::fabs(observation_x - previous_observation_x));
            base_x_differences.push_back(
                std::fabs(result.target.base_aim_x - previous_base_x));
            observation_y_differences.push_back(
                std::fabs(observation_y - previous_observation_y));
            base_y_differences.push_back(
                std::fabs(result.target.base_aim_y - previous_base_y));
        }
        previous_observation_x = observation_x;
        previous_observation_y = observation_y;
        previous_base_x = result.target.base_aim_x;
        previous_base_y = result.target.base_aim_y;
    }

    const auto percentile = [](std::vector<float> values, float quantile) {
        std::sort(values.begin(), values.end());
        const float position = quantile * (values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(
            std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(
            std::ceil(position));
        const float fraction = position - static_cast<float>(lower);
        return values[lower] * (1.0f - fraction) +
            values[upper] * fraction;
    };
    const float observation_x_p95 =
        percentile(observation_x_differences, 0.95f);
    const float base_x_p95 = percentile(base_x_differences, 0.95f);
    const float observation_y_p95 =
        percentile(observation_y_differences, 0.95f);
    const float base_y_p95 = percentile(base_y_differences, 0.95f);
    expect(base_x_p95 <= observation_x_p95 &&
               base_y_p95 <= observation_y_p95,
           "人工已声明世界 X 静止的最新失败段不得因短暂 head 关联缺失"
           "放大 base 对 matched Observation 的跟随，并须保留 Y；"
           "Observation/base P95 X/Y=" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(base_x_p95) + "/" +
               std::to_string(observation_y_p95) + "/" +
               std::to_string(base_y_p95));
    std::cout << "最新静态公开回放 P95: Observation/base X="
              << observation_x_p95 << '/' << base_x_p95
              << "，Y=" << observation_y_p95 << '/' << base_y_p95 << '\n';
}

void test_latest_physical_replay_brakes_before_horizontal_crossing() {
    using aim_latest_physical_replay_fixture::kMeasurementStart;
    using aim_latest_physical_replay_fixture::kObservations;

    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    Aim aim(config);

    auto control_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int previous_nonzero_direction = 0;
    int previous_error_direction = 0;
    int command_direction_reversals = 0;
    int error_direction_reversals = 0;
    int absolute_x_commands = 0;
    int command_direction_violations = 0;
    std::vector<float> observation_x_differences;
    std::vector<float> base_x_differences;
    std::vector<float> observation_y_differences;
    std::vector<float> base_y_differences;
    float previous_observation_x = 0.0f;
    float previous_observation_y = 0.0f;
    float previous_base_x = 0.0f;
    float previous_base_y = 0.0f;

    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        if (index != 0) {
            control_at += std::chrono::nanoseconds(
                static_cast<long long>(observation.controller_dt_ms *
                                       1000000.0f));
        }
        const auto captured_at = control_at - std::chrono::nanoseconds(
            static_cast<long long>(observation.observation_age_ms *
                                   1000000.0f));
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), captured_at);
        frame.control_at = control_at;
        frame.lock_active = true;
        frame.detections = {{observation.x1,
                             observation.y1,
                             observation.x2,
                             observation.y2,
                             0.9f,
                             0}};
        if (observation.aim_from_head) {
            const float center_x =
                (observation.x1 + observation.x2) * 0.5f;
            const float head_width =
                (observation.x2 - observation.x1) * 0.5f;
            frame.detections.push_back(head_box(
                center_x, observation.y1 + 6.0f, head_width, 10.0f));
        }
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "最新 Physical red 必须逐帧经公开 Aim seam 成功处理");
        if (!result.has_target) continue;
        expect(result.target.matched_observation_valid &&
                   result.target.matched_observation_aim_from_head ==
                       observation.aim_from_head,
               "最新 Physical red 必须保留逐帧 body/head 关联语义");
        if (result.has_command) {
            expect(aim.record_backend_completed_command(
                       frame.sequence,
                       frame.control_at + std::chrono::microseconds(
                           static_cast<long long>(
                               observation.backend_completion_ms *
                               1000.0f)),
                       result.command.dx_counts,
                       result.command.dy_counts),
                   "最新 Physical red 必须写回同序列整数完成命令");
        }

        const float observation_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_y =
            (observation.y1 + observation.y2) * 0.5f;
        if (index > kMeasurementStart) {
            observation_x_differences.push_back(
                std::fabs(observation_x - previous_observation_x));
            base_x_differences.push_back(
                std::fabs(result.target.base_aim_x - previous_base_x));
            observation_y_differences.push_back(
                std::fabs(observation_y - previous_observation_y));
            base_y_differences.push_back(
                std::fabs(result.target.base_aim_y - previous_base_y));
            absolute_x_commands += std::abs(result.command.dx_counts);
            const float error_x = result.target.aim_x -
                frame.control_center_x;
            const int error_direction = error_x > 0.0f
                ? 1 : (error_x < 0.0f ? -1 : 0);
            if (error_direction != 0) {
                if (previous_error_direction != 0 &&
                    error_direction != previous_error_direction) {
                    ++error_direction_reversals;
                }
                previous_error_direction = error_direction;
            }
            if (result.has_command) {
                const float error_y = result.target.aim_y -
                    frame.control_center_y;
                if (result.command.dx_counts * error_x +
                        result.command.dy_counts * error_y <= 0.0f) {
                    ++command_direction_violations;
                }
            }
            const int direction = result.command.dx_counts > 0
                ? 1 : (result.command.dx_counts < 0 ? -1 : 0);
            if (direction != 0) {
                if (previous_nonzero_direction != 0 &&
                    direction != previous_nonzero_direction) {
                    ++command_direction_reversals;
                }
                previous_nonzero_direction = direction;
            }
        }
        previous_observation_x = observation_x;
        previous_observation_y = observation_y;
        previous_base_x = result.target.base_aim_x;
        previous_base_y = result.target.base_aim_y;
    }

    const auto percentile = [](std::vector<float> values, float quantile) {
        std::sort(values.begin(), values.end());
        const float position = quantile * (values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(
            std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(
            std::ceil(position));
        const float fraction = position - static_cast<float>(lower);
        return values[lower] * (1.0f - fraction) +
            values[upper] * fraction;
    };
    const float observation_x_p95 =
        percentile(observation_x_differences, 0.95f);
    const float base_x_p95 = percentile(base_x_differences, 0.95f);
    const float observation_y_p95 =
        percentile(observation_y_differences, 0.95f);
    const float base_y_p95 = percentile(base_y_differences, 0.95f);
    expect(base_x_p95 <= observation_x_p95 &&
               base_y_p95 <= observation_y_p95 &&
               command_direction_violations == 0,
           "最新 Physical red 必须保留 base X/Y 不放大及二维方向合同，"
           "Observation/base P95 X/Y/方向违规=" +
               std::to_string(observation_x_p95) + "/" +
               std::to_string(base_x_p95) + "/" +
               std::to_string(observation_y_p95) + "/" +
               std::to_string(base_y_p95) + "/" +
               std::to_string(command_direction_violations));
    expect(command_direction_reversals <= error_direction_reversals &&
               absolute_x_commands <= 125,
           "世界 X 静止且 15 ms 已完成命令在第三帧显现的失败窗，控制器"
           "必须在过零前连续制动，不得新增超出可见误差换边的 X 往返，并"
           "须降低同窗整数命令量；命令/误差反向/绝对量=" +
               std::to_string(command_direction_reversals) + "/" +
               std::to_string(error_direction_reversals) + "/" +
               std::to_string(absolute_x_commands));
    std::cout << "最新 Physical 公开回放: X命令/误差反向/绝对量="
              << command_direction_reversals << '/'
              << error_direction_reversals << '/' << absolute_x_commands
              << "，Observation/base P95 X=" << observation_x_p95 << '/'
              << base_x_p95 << "，Y=" << observation_y_p95 << '/'
              << base_y_p95 << '\n';
}

void test_actual_game_superjump_current_common_translation_brakes_x() {
    using aim_superjump_actual_game_replay_fixture::kMeasurementStart;
    using aim_superjump_actual_game_replay_fixture::kObservations;

    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    Aim aim(config);

    auto control_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::vector<float> observation_x_errors;
    std::vector<float> track_x_errors;
    std::vector<float> base_x_errors;
    std::vector<float> observation_y_differences;
    std::vector<float> base_y_differences;
    int opposite_observation_x_commands = 0;
    int absolute_x_commands = 0;
    int eligible_current_closing_frames = 0;
    int damped_current_closing_frames = 0;
    std::uint64_t y_trace_signature = 1469598103934665603ULL;
    float previous_observation_y = 0.0f;
    float previous_base_y = 0.0f;

    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        if (index != 0) {
            control_at += std::chrono::nanoseconds(
                static_cast<long long>(observation.controller_dt_ms *
                                       1000000.0f));
        }
        const auto captured_at = control_at - std::chrono::nanoseconds(
            static_cast<long long>(observation.observation_age_ms *
                                   1000000.0f));
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), captured_at);
        frame.control_at = control_at;
        frame.lock_active = true;
        frame.detections = {{observation.x1,
                             observation.y1,
                             observation.x2,
                             observation.y2,
                             0.9f,
                             0}};
        if (observation.aim_from_head) {
            const float center_x =
                (observation.x1 + observation.x2) * 0.5f;
            const float head_width =
                (observation.x2 - observation.x1) * 0.5f;
            frame.detections.push_back(head_box(
                center_x, observation.y1 + 6.0f, head_width, 10.0f));
        }

        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "SuperJump 实际游戏回放必须逐帧经公开 Aim seam 成功处理");
        if (!result.has_target) continue;
        expect(result.target.matched_observation_valid &&
                   result.target.matched_observation_aim_from_head ==
                       observation.aim_from_head,
               "SuperJump 实际游戏回放必须保留逐帧 body/head 关联语义");
        if (result.has_command) {
            expect(aim.record_backend_completed_command(
                       frame.sequence,
                       frame.control_at + std::chrono::microseconds(
                           static_cast<long long>(
                               observation.backend_completion_ms *
                               1000.0f)),
                       result.command.dx_counts,
                       result.command.dy_counts),
                   "SuperJump 实际游戏回放必须写回同序列整数完成命令");
        }

        const float observation_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_y =
            (observation.y1 + observation.y2) * 0.5f;
        const float track_x =
            (result.target.x1 + result.target.x2) * 0.5f;
        if (index >= kMeasurementStart) {
            const float observation_error_x =
                observation_x - frame.control_center_x;
            const float track_error_x = track_x - frame.control_center_x;
            const float base_error_x =
                result.target.base_aim_x - frame.control_center_x;
            observation_x_errors.push_back(std::fabs(observation_error_x));
            track_x_errors.push_back(std::fabs(track_error_x));
            base_x_errors.push_back(std::fabs(base_error_x));
            if (std::fabs(observation_error_x) > config.deadzone_pixels &&
                result.command.dx_counts * observation_error_x < 0.0f) {
                ++opposite_observation_x_commands;
            }
            absolute_x_commands += std::abs(result.command.dx_counts);
            const float raw_common_x =
                result.control.reverse_translation_raw_common_x_roi_pixels;
            const float same_direction_filtered_x =
                base_error_x * result.control.filtered_x_counts;
            if (base_error_x * raw_common_x < 0.0f &&
                same_direction_filtered_x > 0.0f) {
                ++eligible_current_closing_frames;
                const float error_direction =
                    base_error_x > 0.0f ? 1.0f : -1.0f;
                if (error_direction *
                        (result.control.filtered_x_counts -
                         result.control.shaped_x_counts) > 0.0001f) {
                    ++damped_current_closing_frames;
                }
            }
            if (index > kMeasurementStart) {
                observation_y_differences.push_back(
                    std::fabs(observation_y - previous_observation_y));
                base_y_differences.push_back(
                    std::fabs(result.target.base_aim_y - previous_base_y));
            }
            const auto base_y_millipixel = static_cast<std::int64_t>(
                std::llround(result.target.base_aim_y * 1000.0f));
            y_trace_signature ^= static_cast<std::uint64_t>(
                base_y_millipixel + 1000000LL);
            y_trace_signature *= 1099511628211ULL;
            y_trace_signature ^= static_cast<std::uint64_t>(
                result.command.dy_counts + 128);
            y_trace_signature *= 1099511628211ULL;
        }
        previous_observation_y = observation_y;
        previous_base_y = result.target.base_aim_y;
    }

    const auto percentile = [](std::vector<float> values, float quantile) {
        std::sort(values.begin(), values.end());
        const float position = quantile * (values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(
            std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(
            std::ceil(position));
        const float fraction = position - static_cast<float>(lower);
        return values[lower] * (1.0f - fraction) +
            values[upper] * fraction;
    };
    const float observation_x_p95 =
        percentile(observation_x_errors, 0.95f);
    const float track_x_p95 = percentile(track_x_errors, 0.95f);
    const float base_x_p95 = percentile(base_x_errors, 0.95f);
    const float observation_y_d1_p95 =
        percentile(observation_y_differences, 0.95f);
    const float base_y_d1_p95 =
        percentile(base_y_differences, 0.95f);

    expect(base_y_d1_p95 <= observation_y_d1_p95 &&
               y_trace_signature == 980425601653164214ULL,
           "SuperJump 实际游戏回放必须保留已人工通过的 Y 跟随，"
           "Observation/base D1 P95/轨迹签名=" +
               std::to_string(observation_y_d1_p95) + "/" +
               std::to_string(base_y_d1_p95) + "/" +
               std::to_string(y_trace_signature));
    expect(eligible_current_closing_frames >= 150 &&
               damped_current_closing_frames * 3 >=
                   eligible_current_closing_frames * 2,
           "sidecar 已确认相邻背景与 raw-common 同步平移时，当前共同平移"
           "正在令 base X 朝零的帧对必须覆盖至少三分之二的同号减阻；"
           "eligible/damped=" +
               std::to_string(eligible_current_closing_frames) + "/" +
               std::to_string(damped_current_closing_frames));
    expect(absolute_x_commands < 278,
           "current-common X closing green 必须降低同一实际窗口的整数命令"
           "总量，基线/当前=" + std::to_string(278) + "/" +
               std::to_string(absolute_x_commands));
    std::cout << "SuperJump 实际游戏公开回放: Observation/Track/base X abs P95="
              << observation_x_p95 << '/' << track_x_p95 << '/'
              << base_x_p95 << "，反向X命令="
              << opposite_observation_x_commands << "，X绝对命令量="
              << absolute_x_commands << "，当前closing减阻="
              << damped_current_closing_frames << '/'
              << eligible_current_closing_frames << "，Y D1 P95="
              << observation_y_d1_p95 << '/' << base_y_d1_p95
              << "，Y签名=" << y_trace_signature << '\n';
}

void test_tracking_derivative_separates_in_box_reference_from_common_translation() {
    constexpr int kReferenceFrameCount = 104;
    constexpr int kReferenceBranchFrame = 100;
    constexpr int kReferencePreviousFrame = 102;
    constexpr int kReferenceCurrentFrame = 103;
    constexpr int kCommonFrameCount = 42;
    constexpr int kCommonPreviousFrame = 40;
    constexpr int kCommonCurrentFrame = 41;
    constexpr auto kFrameStep = std::chrono::microseconds(4167);
    constexpr float kNarrowBranchWidth = 16.0f;
    constexpr float kWideBranchWidth = 32.0f;
    constexpr float kNarrowCurrentWidth = 16.0f;
    // 两个值只用于让成对 public trace 在当前帧重新得到相同 base/Track；
    // 它们不是 production 框宽阈值，也不进入控制分支。
    constexpr float kWideCurrentWidth = 13.35f;
    constexpr float kCurrentError = 1.5f;

    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 2.25f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 14.0f;
    config.acquisition_range_percent = 150.0f;
    config.body_aim_range_percent = 50.0f;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    const auto track_center_x = [](const AimResult& result) {
        return (result.target.x1 + result.target.x2) * 0.5f;
    };
    const auto reference_x = [&](const AimResult& result) {
        return result.target.base_aim_x - track_center_x(result);
    };
    const auto make_reference_frame = [&](int index,
                                           float branch_width,
                                           float current_width) {
        const int pose_sample = std::min(index, 99);
        const int phase_index = pose_sample % 34;
        const float pose_phase = phase_index <= 17
            ? -1.0f + static_cast<float>(phase_index) * (2.0f / 17.0f)
            : 1.0f - static_cast<float>(phase_index - 17) *
                (2.0f / 17.0f);
        const float true_x = 60.0f +
            static_cast<float>(std::min(index, 99)) * 0.80f;
        float width = 16.0f + pose_phase * 0.5f;
        if (index == kReferenceBranchFrame) width = branch_width;
        if (index == kReferenceCurrentFrame) width = current_width;
        const float height = 70.0f + pose_phase * 1.8f;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + kFrameStep * index);
        frame.control_at = frame.captured_at +
            std::chrono::milliseconds(1);
        frame.control_center_y = 160.0f;
        frame.detections = {body_box(
            true_x + pose_phase * 3.0f,
            175.0f,
            width,
            height)};
        return frame;
    };

    struct GeometryTrace {
        std::array<float, kReferenceFrameCount> base_x{};
        std::array<float, kReferenceFrameCount> base_y{};
        int valid_frames = 0;
    };
    // 首次 pass 仍只调用 Aim::process()；它为第二次控制 pass 提供公开
    // base 几何，使 P/PI、smoothing 和库存能在 reference 分叉两侧等价。
    const auto trace_geometry = [&](float branch_width,
                                    float current_width) {
        Aim aim(config);
        GeometryTrace trace;
        for (int index = 0; index < kReferenceFrameCount; ++index) {
            AimFrame frame = make_reference_frame(
                index, branch_width, current_width);
            frame.lock_active = true;
            const AimResult result = aim.process(frame);
            if (result.status != AimStatus::SUCCESS || !result.has_target) {
                continue;
            }
            ++trace.valid_frames;
            trace.base_x[index] = result.target.base_aim_x;
            trace.base_y[index] = result.target.base_aim_y;
        }
        return trace;
    };

    const GeometryTrace narrow_geometry = trace_geometry(
        kNarrowBranchWidth, kNarrowCurrentWidth);
    const GeometryTrace wide_geometry = trace_geometry(
        kWideBranchWidth, kWideCurrentWidth);
    const float previous_control_center_x =
        narrow_geometry.base_x[kReferencePreviousFrame] - 1.48f;
    const float current_control_center_x =
        (narrow_geometry.base_x[kReferenceCurrentFrame] +
         wide_geometry.base_x[kReferenceCurrentFrame]) * 0.5f -
        kCurrentError;

    struct ReferenceSample {
        AimResult previous;
        AimResult current;
        float previous_control_center_x = 0.0f;
        float current_control_center_x = 0.0f;
        int valid_frames = 0;
        int completed_frames = 0;
    };
    const auto run_reference_case = [&](float branch_width,
                                        float current_width,
                                        const GeometryTrace& geometry) {
        Aim aim(config);
        ReferenceSample sample;
        for (int index = 0; index < kReferenceFrameCount; ++index) {
            AimFrame frame = make_reference_frame(
                index, branch_width, current_width);
            frame.lock_active = true;
            frame.control_center_y = geometry.base_y[index];
            if (index < kReferenceBranchFrame) {
                frame.control_center_x = geometry.base_x[index] - 6.0f;
            } else if (index < kReferencePreviousFrame) {
                frame.control_center_x = geometry.base_x[index] -
                    kCurrentError;
            } else if (index == kReferencePreviousFrame) {
                frame.control_center_x = previous_control_center_x;
                sample.previous_control_center_x = frame.control_center_x;
            } else {
                frame.control_center_x = current_control_center_x;
                sample.current_control_center_x = frame.control_center_x;
            }
            const AimResult result = aim.process(frame);
            if (result.status == AimStatus::SUCCESS && result.has_target &&
                result.control.evaluated) {
                ++sample.valid_frames;
            }
            if (index == kReferencePreviousFrame) sample.previous = result;
            if (index == kReferenceCurrentFrame) {
                sample.current = result;
                continue;
            }
            if (result.has_command && aim.record_backend_completed_command(
                    frame.sequence,
                    frame.control_at + std::chrono::microseconds(100),
                    result.command.dx_counts,
                    result.command.dy_counts)) {
                ++sample.completed_frames;
            }
        }
        return sample;
    };

    const ReferenceSample narrow_reference = run_reference_case(
        kNarrowBranchWidth, kNarrowCurrentWidth, narrow_geometry);
    const ReferenceSample faster_reference = run_reference_case(
        kWideBranchWidth, kWideCurrentWidth, wide_geometry);
    const float narrow_previous_error =
        narrow_reference.previous.target.base_aim_x -
        narrow_reference.previous_control_center_x;
    const float faster_previous_error =
        faster_reference.previous.target.base_aim_x -
        faster_reference.previous_control_center_x;
    const float narrow_current_error =
        narrow_reference.current.target.base_aim_x -
        narrow_reference.current_control_center_x;
    const float faster_current_error =
        faster_reference.current.target.base_aim_x -
        faster_reference.current_control_center_x;
    const float narrow_reference_delta =
        reference_x(narrow_reference.current) -
        reference_x(narrow_reference.previous);
    const float faster_reference_delta =
        reference_x(faster_reference.current) -
        reference_x(faster_reference.previous);
    const float narrow_track_delta =
        track_center_x(narrow_reference.current) -
        track_center_x(narrow_reference.previous);
    const float faster_track_delta =
        track_center_x(faster_reference.current) -
        track_center_x(faster_reference.previous);
    const float narrow_reference_damping =
        narrow_reference.current.control.filtered_x_counts -
        narrow_reference.current.control.desired_x_counts;
    const float faster_reference_damping =
        faster_reference.current.control.filtered_x_counts -
        faster_reference.current.control.desired_x_counts;

    expect(narrow_geometry.valid_frames == kReferenceFrameCount &&
               wide_geometry.valid_frames == kReferenceFrameCount &&
               narrow_reference.valid_frames == kReferenceFrameCount &&
               faster_reference.valid_frames == kReferenceFrameCount &&
               narrow_reference.completed_frames ==
                   faster_reference.completed_frames &&
               narrow_reference.completed_frames > 0,
           "reference-only 成对夹具必须逐帧保留公开目标、控制求值和"
           "backend-completed 历史，geometry/control/completed=" +
               std::to_string(narrow_geometry.valid_frames) + "/" +
               std::to_string(wide_geometry.valid_frames) + "/" +
               std::to_string(narrow_reference.valid_frames) + "/" +
               std::to_string(faster_reference.valid_frames) + "/" +
               std::to_string(narrow_reference.completed_frames) + "/" +
               std::to_string(faster_reference.completed_frames));
    expect(std::fabs(narrow_track_delta - faster_track_delta) < 0.0001f &&
               std::fabs(track_center_x(narrow_reference.current) -
                         track_center_x(faster_reference.current)) < 0.0001f &&
               narrow_reference.current.target.matched_observation_valid &&
               faster_reference.current.target.matched_observation_valid &&
               !narrow_reference.current.target.
                   matched_observation_head_only &&
               !faster_reference.current.target.
                   matched_observation_head_only &&
               !narrow_reference.current.target.
                   matched_observation_aim_from_head &&
               !faster_reference.current.target.
                   matched_observation_aim_from_head &&
               std::fabs(
                   (narrow_reference.current.target.
                        matched_observation_x1 +
                    narrow_reference.current.target.
                        matched_observation_x2) * 0.5f -
                   (faster_reference.current.target.
                        matched_observation_x1 +
                    faster_reference.current.target.
                        matched_observation_x2) * 0.5f) < 0.0001f &&
               std::fabs(narrow_current_error - faster_current_error) <
                   0.001f &&
               std::fabs(narrow_current_error - kCurrentError) < 0.001f &&
               faster_previous_error > narrow_previous_error + 0.15f &&
               faster_previous_error <= config.deadzone_pixels &&
               faster_reference_delta < narrow_reference_delta - 0.15f,
           "成对 reference 序列必须保持相同 Track 公共平移和当前完整误差，"
           "只让前帧框内 reference 更快闭合，trackΔ/refΔ/prev/current=" +
               std::to_string(narrow_track_delta) + "/" +
               std::to_string(faster_track_delta) + "/" +
               std::to_string(narrow_reference_delta) + "/" +
               std::to_string(faster_reference_delta) + "/" +
               std::to_string(narrow_previous_error) + "/" +
               std::to_string(faster_previous_error) + "/" +
               std::to_string(narrow_current_error) + "/" +
               std::to_string(faster_current_error));
    expect(std::fabs(narrow_reference.current.control.proportional_x_counts) <
                    0.0001f &&
               std::fabs(faster_reference.current.control.
                             proportional_x_counts) < 0.0001f &&
               std::fabs(narrow_reference.current.control.feedforward_x_counts -
                         faster_reference.current.control.
                             feedforward_x_counts) < 0.0001f &&
               std::fabs(narrow_reference.current.control.filtered_x_counts -
                         faster_reference.current.control.filtered_x_counts) <
                   0.0001f &&
               std::fabs(narrow_reference.current.control.
                             pending_net_x_counts -
                         faster_reference.current.control.
                             pending_net_x_counts) < 0.0001f &&
               std::fabs(narrow_reference.current.control.
                             pending_absolute_x_counts -
                         faster_reference.current.control.
                             pending_absolute_x_counts) < 0.0001f,
           "reference-only 分叉不得改变 PI、smoothing 或 backend-completed "
           "库存，P/积分/滤波/net/absolute=" +
               std::to_string(narrow_reference.current.control.
                                  proportional_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  proportional_x_counts) + "/" +
               std::to_string(narrow_reference.current.control.
                                  feedforward_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  feedforward_x_counts) + "/" +
               std::to_string(narrow_reference.current.control.
                                  filtered_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  filtered_x_counts) + "/" +
               std::to_string(narrow_reference.current.control.
                                  pending_net_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  pending_net_x_counts) + "/" +
               std::to_string(narrow_reference.current.control.
                                  pending_absolute_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  pending_absolute_x_counts));
    expect(std::fabs(narrow_reference_damping -
                     faster_reference_damping) < 0.0001f &&
               std::fabs(narrow_reference.current.control.desired_x_counts -
                         faster_reference.current.control.desired_x_counts) <
                   0.0001f &&
               std::abs(faster_reference.current.command.dx_counts) <=
                   std::abs(narrow_reference.current.command.dx_counts) &&
               faster_reference.current.command.dy_counts ==
                   narrow_reference.current.command.dy_counts &&
               std::hypot(
                   static_cast<float>(
                       faster_reference.current.command.dx_counts),
                   static_cast<float>(
                       faster_reference.current.command.dy_counts)) <=
                   config.max_counts_per_frame,
           "Track 公共平移、当前完整误差及其他控制状态相同时，"
           "框内 reference 斜率不得改变 X 阻尼，damping/request=" +
               std::to_string(narrow_reference_damping) + "/" +
               std::to_string(faster_reference_damping) + "/" +
               std::to_string(narrow_reference.current.control.
                                  desired_x_counts) + "/" +
               std::to_string(faster_reference.current.control.
                                  desired_x_counts));

    struct CommonSample {
        AimResult previous;
        AimResult current;
        int valid_frames = 0;
        int completed_frames = 0;
    };
    const auto run_common_case = [&](bool faster_closing) {
        Aim aim(config);
        CommonSample sample;
        for (int index = 0; index < kCommonFrameCount; ++index) {
            float center_x = 180.0f;
            if (faster_closing && index == kCommonPreviousFrame) {
                center_x = 181.041667f;
            } else if (faster_closing && index == kCommonCurrentFrame) {
                center_x = 179.6680f;
            }
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::seconds(2) + kFrameStep * index);
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.control_center_x = index < 20 ? 174.0f : 178.5f;
            frame.control_center_y = 160.0f;
            frame.lock_active = true;
            frame.detections = {body_box(center_x, 172.0f, 40.0f, 80.0f)};
            const AimResult result = aim.process(frame);
            if (result.status == AimStatus::SUCCESS && result.has_target &&
                result.control.evaluated) {
                ++sample.valid_frames;
            }
            if (index == kCommonPreviousFrame) sample.previous = result;
            if (index == kCommonCurrentFrame) {
                sample.current = result;
                continue;
            }
            if (result.has_command && aim.record_backend_completed_command(
                    frame.sequence,
                    frame.control_at + std::chrono::microseconds(100),
                    result.command.dx_counts,
                    result.command.dy_counts)) {
                ++sample.completed_frames;
            }
        }
        return sample;
    };

    const CommonSample stationary_common = run_common_case(false);
    const CommonSample faster_common = run_common_case(true);
    const float stationary_common_previous_error =
        stationary_common.previous.target.base_aim_x - 178.5f;
    const float faster_common_previous_error =
        faster_common.previous.target.base_aim_x - 178.5f;
    const float stationary_common_current_error =
        stationary_common.current.target.base_aim_x - 178.5f;
    const float faster_common_current_error =
        faster_common.current.target.base_aim_x - 178.5f;
    const float stationary_common_damping =
        stationary_common.current.control.filtered_x_counts -
        stationary_common.current.control.desired_x_counts;
    const float faster_common_damping =
        faster_common.current.control.filtered_x_counts -
        faster_common.current.control.desired_x_counts;

    expect(stationary_common.valid_frames == kCommonFrameCount &&
               faster_common.valid_frames == kCommonFrameCount &&
               stationary_common.completed_frames ==
                   faster_common.completed_frames &&
               stationary_common.completed_frames > 0 &&
               std::fabs(reference_x(stationary_common.previous)) < 0.0001f &&
               std::fabs(reference_x(faster_common.previous)) < 0.0001f &&
               std::fabs(reference_x(stationary_common.current)) < 0.0001f &&
               std::fabs(reference_x(faster_common.current)) < 0.0001f &&
               std::fabs(track_center_x(stationary_common.current) -
                         track_center_x(faster_common.current)) < 0.001f &&
               std::fabs(stationary_common_current_error -
                         faster_common_current_error) < 0.001f &&
               std::fabs(stationary_common_current_error -
                         kCurrentError) < 0.001f &&
               faster_common_previous_error >
                   stationary_common_previous_error + 0.50f &&
               faster_common_previous_error <= config.deadzone_pixels,
           "common-translation 成对夹具必须保持框内 reference 与当前完整"
           "误差相同，只让 Track 更快朝零，prev/current/ref=" +
               std::to_string(stationary_common_previous_error) + "/" +
               std::to_string(faster_common_previous_error) + "/" +
               std::to_string(stationary_common_current_error) + "/" +
               std::to_string(faster_common_current_error) + "/" +
               std::to_string(reference_x(faster_common.previous)) + "/" +
               std::to_string(reference_x(faster_common.current)));
    expect(faster_common_damping > stationary_common_damping + 0.001f &&
               faster_common.current.control.desired_x_counts >= 0.0f &&
               faster_common.current.control.desired_x_counts <
                   stationary_common.current.control.desired_x_counts &&
               faster_common.current.command.dy_counts ==
                   stationary_common.current.command.dy_counts &&
               std::hypot(
                   static_cast<float>(faster_common.current.command.dx_counts),
                   static_cast<float>(faster_common.current.command.dy_counts)) <=
                   config.max_counts_per_frame,
           "框内 reference 不变时，更快 Track closing 必须连续减小同号 X，"
           "不得反向、改变 Y 或提高 14-count 上限，damping/request/cmd=" +
               std::to_string(stationary_common_damping) + "/" +
               std::to_string(faster_common_damping) + "/" +
               std::to_string(stationary_common.current.control.
                                  desired_x_counts) + "/" +
               std::to_string(faster_common.current.control.
                                  desired_x_counts) + "/" +
               std::to_string(stationary_common.current.command.dx_counts) +
               "/" +
               std::to_string(faster_common.current.command.dx_counts) + "/" +
               std::to_string(stationary_common.current.command.dy_counts) +
               "/" +
               std::to_string(faster_common.current.command.dy_counts));
}

void test_delayed_partial_visibility_closed_loop_preserves_real_reversals() {
    constexpr int kFrameCount = 240;
    constexpr int kSegmentFrameCount = 80;
    constexpr int kActuationDelayFrames = 4;
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr float kFullWidthRatio = 0.14f;
    constexpr float kPartialWidthRatio = 0.70f;
    constexpr std::array<float, 3> kMotionRatiosPerSecond{
        0.75f, -0.75f, 0.75f};
    struct Trace {
        float error_p95_ratio = 0.0f;
        float base_second_p95_ratio = 0.0f;
        int command_reversals = 0;
        int expected_true_reversals = 0;
        int completed_true_reversals = 0;
        int wrong_direction_commands_after_base_crossing = 0;
        int maximum_reverse_latency_frames = 0;
        int identity_changes = 0;
        int left_partial_confirmed_frames = 0;
        int right_partial_confirmed_frames = 0;
        int first_full_recovery_frames = 0;
        int second_full_recovery_frames = 0;
        std::string first_reversal_context;
        std::string reversal_contexts;
    };
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction))];
    };

    const auto run_case = [&](float roi_scale, bool partial_enabled) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 1.5f * roi_scale;
        config.smoothing = 0.475f;
        // ROI 放大时每 ROI 像素对应 counts 成比例缩小，使闭环镜头响应保持
        // 归一化同构；运动本身只由 ROI/s 定义，不按 px/s 或游戏速度分档。
        config.counts_per_pixel_x = 0.425f / roi_scale;
        config.counts_per_pixel_y = 0.40f / roi_scale;
        config.max_counts_per_frame = 14.0f;
        config.acquisition_range_percent = 100.0f;
        config.body_aim_height_ratio = 0.50f;
        config.enable_prediction = false;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 15.0f;
        Aim aim(config);
        Trace trace;
        std::array<int, kActuationDelayFrames> delayed_commands{};
        const float roi_width = 320.0f * roi_scale;
        const float control_center_x = roi_width * 0.5f;
        const float full_width = roi_width * kFullWidthRatio;
        const float partial_width = full_width * kPartialWidthRatio;
        float world_target_x = roi_width * 0.075f;
        float camera_x = 0.0f;
        std::uint64_t track_id = 0;
        int previous_command_sign = 0;
        int active_reverse_direction = 0;
        int active_reverse_base_crossing_frame = -1;
        std::vector<float> normalized_errors;
        std::vector<float> normalized_base_points;

        for (int index = 0; index < kFrameCount; ++index) {
            camera_x += delayed_commands[index % kActuationDelayFrames] /
                config.counts_per_pixel_x * 0.20f;
            delayed_commands[index % kActuationDelayFrames] = 0;
            const int segment = std::min(
                static_cast<int>(kMotionRatiosPerSecond.size()) - 1,
                index / kSegmentFrameCount);
            const float motion_ratio_per_second =
                kMotionRatiosPerSecond[static_cast<std::size_t>(segment)];
            if (index > 0 && index % kSegmentFrameCount == 0) {
                active_reverse_direction =
                    motion_ratio_per_second < 0.0f ? -1 : 1;
                active_reverse_base_crossing_frame = -1;
                ++trace.expected_true_reversals;
            }
            world_target_x +=
                motion_ratio_per_second * roi_width * kFrameSeconds;
            const float observed_x =
                control_center_x + world_target_x - camera_x;
            const bool left_partial =
                partial_enabled && index >= 44 && index < 60;
            const bool right_partial =
                partial_enabled && index >= 124 && index < 140;
            float observation_center_x = observed_x;
            float observation_width = full_width;
            if (left_partial) {
                // 保持完整框左边界，仅内收右边界。
                observation_width = partial_width;
                observation_center_x = observed_x -
                    (full_width - partial_width) * 0.5f;
            } else if (right_partial) {
                // 保持完整框右边界，仅内收左边界。
                observation_width = partial_width;
                observation_center_x = observed_x +
                    (full_width - partial_width) * 0.5f;
            }

            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.roi_width = static_cast<int>(roi_width);
            frame.roi_height = static_cast<int>(roi_width);
            frame.control_center_x = control_center_x;
            frame.control_center_y = roi_width * 0.5f;
            frame.control_at =
                frame.captured_at + std::chrono::milliseconds(1);
            frame.lock_active = true;
            frame.detections = {body_box(
                observation_center_x, roi_width * 0.5f,
                observation_width, roi_width * 0.28f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "左右半身、完整框恢复及真实反转闭环必须逐帧保留目标");
            if (!result.has_target) continue;
            if (track_id == 0) track_id = result.target.track_id;
            if (result.target.track_id != track_id) ++trace.identity_changes;
            const int command_x = result.has_command
                ? result.command.dx_counts : 0;
            if (result.has_command) {
                delayed_commands[index % kActuationDelayFrames] = command_x;
            }

            const float tracked_width =
                result.target.x2 - result.target.x1;
            if (left_partial && index >= 47 &&
                tracked_width <= full_width * 0.82f) {
                ++trace.left_partial_confirmed_frames;
            }
            if (right_partial && index >= 127 &&
                tracked_width <= full_width * 0.82f) {
                ++trace.right_partial_confirmed_frames;
            }
            if (partial_enabled && index >= 64 && index < 80 &&
                tracked_width >= full_width * 0.90f) {
                ++trace.first_full_recovery_frames;
            }
            if (partial_enabled && index >= 144 && index < 160 &&
                tracked_width >= full_width * 0.90f) {
                ++trace.second_full_recovery_frames;
            }

            if (active_reverse_direction != 0) {
                const float normalized_base_error =
                    (result.target.base_aim_x - control_center_x) / roi_width;
                if (active_reverse_base_crossing_frame < 0 &&
                    normalized_base_error * active_reverse_direction >
                        0.007f) {
                    active_reverse_base_crossing_frame = index;
                }
                if (active_reverse_base_crossing_frame >= 0) {
                    if (command_x * active_reverse_direction > 0) {
                        const int latency =
                            index - active_reverse_base_crossing_frame;
                        trace.maximum_reverse_latency_frames = std::max(
                            trace.maximum_reverse_latency_frames, latency);
                        if (trace.first_reversal_context.empty()) {
                            trace.first_reversal_context =
                                "frame=" + std::to_string(index) +
                                ",latency=" + std::to_string(latency) +
                                ",cmd=" + std::to_string(command_x) +
                                ",base_ratio=" +
                                std::to_string(normalized_base_error) +
                                ",width_ratio=" +
                                std::to_string(tracked_width / full_width);
                        }
                        active_reverse_direction = 0;
                        ++trace.completed_true_reversals;
                    } else if (command_x != 0) {
                        ++trace.wrong_direction_commands_after_base_crossing;
                    }
                }
            }
            if (index >= 20 && command_x != 0) {
                const int command_sign = command_x < 0 ? -1 : 1;
                if (previous_command_sign != 0 &&
                    command_sign != previous_command_sign) {
                    ++trace.command_reversals;
                    trace.reversal_contexts +=
                        "frame=" + std::to_string(index) +
                        ",cmd=" + std::to_string(command_x) +
                        ",base=" + std::to_string(
                            result.target.base_aim_x - control_center_x) +
                        ",delay=" + std::to_string(
                            result.target.delay_compensation_x) +
                        ",ff=" + std::to_string(
                            result.control.feedforward_x_counts) + ";";
                }
                previous_command_sign = command_sign;
            }
            if (index >= 20) {
                normalized_errors.push_back(
                    std::fabs(world_target_x - camera_x) / roi_width);
                normalized_base_points.push_back(
                    result.target.base_aim_x / roi_width);
            }
        }

        std::vector<float> normalized_base_seconds;
        for (std::size_t index = 2;
             index < normalized_base_points.size(); ++index) {
            normalized_base_seconds.push_back(std::fabs(
                normalized_base_points[index] -
                2.0f * normalized_base_points[index - 1] +
                normalized_base_points[index - 2]));
        }
        std::sort(normalized_errors.begin(), normalized_errors.end());
        std::sort(normalized_base_seconds.begin(),
                  normalized_base_seconds.end());
        trace.error_p95_ratio = percentile(normalized_errors, 0.95f);
        trace.base_second_p95_ratio =
            percentile(normalized_base_seconds, 0.95f);
        return trace;
    };

    const Trace fixed = run_case(1.0f, false);
    const Trace partial = run_case(1.0f, true);
    const Trace doubled_partial = run_case(2.0f, true);
    expect(kPartialWidthRatio <= 0.75f &&
               fixed.expected_true_reversals == 2 &&
               fixed.completed_true_reversals == 2 &&
               fixed.command_reversals == 2 &&
               fixed.wrong_direction_commands_after_base_crossing == 0,
           "固定框闭环对照必须先稳定完成两次真实反转且没有额外往返，"
           "预期/完成/命令反转/过线错向=" +
               std::to_string(fixed.expected_true_reversals) + "/" +
               std::to_string(fixed.completed_true_reversals) + "/" +
               std::to_string(fixed.command_reversals) + "/" +
               std::to_string(
                   fixed.wrong_direction_commands_after_base_crossing));
    const auto partial_contract_holds = [](const Trace& trace) {
        return trace.expected_true_reversals == 2 &&
            trace.completed_true_reversals == 2 &&
            trace.command_reversals == 2 &&
            trace.wrong_direction_commands_after_base_crossing == 0 &&
            trace.maximum_reverse_latency_frames <= 10 &&
            trace.identity_changes == 0 &&
            trace.left_partial_confirmed_frames >= 8 &&
            trace.right_partial_confirmed_frames >= 8 &&
            trace.first_full_recovery_frames >= 12 &&
            trace.second_full_recovery_frames >= 12 &&
            trace.error_p95_ratio <= 0.08f &&
            trace.base_second_p95_ratio <= 0.02f;
    };
    expect(
        partial_contract_holds(partial) &&
            partial_contract_holds(doubled_partial) &&
            partial.command_reversals == fixed.command_reversals &&
            partial.error_p95_ratio - fixed.error_p95_ratio <= 0.04f &&
            std::fabs(partial.error_p95_ratio -
                      doubled_partial.error_p95_ratio) <= 0.005f &&
            std::fabs(partial.base_second_p95_ratio -
                      doubled_partial.base_second_p95_ratio) <= 0.001f &&
            std::abs(partial.maximum_reverse_latency_frames -
                     doubled_partial.maximum_reverse_latency_frames) <= 1,
        "稳定真实闭环中的左/右半身必须以不超过 75% 的宽度确认，恢复完整"
        "框后两次真实水平反转不得漏发或产生额外往返，且 320/640 ROI"
        " 必须比例同构；320预期/完成/反转/错向/延迟/身份/左半/右半/"
        "恢复1/恢复2/误差P95/基础D2P95，640对应值=" +
            std::to_string(partial.expected_true_reversals) + "/" +
            std::to_string(partial.completed_true_reversals) + "/" +
            std::to_string(partial.command_reversals) + "/" +
            std::to_string(
                partial.wrong_direction_commands_after_base_crossing) + "/" +
            std::to_string(partial.maximum_reverse_latency_frames) + "/" +
            std::to_string(partial.identity_changes) + "/" +
            std::to_string(partial.left_partial_confirmed_frames) + "/" +
            std::to_string(partial.right_partial_confirmed_frames) + "/" +
            std::to_string(partial.first_full_recovery_frames) + "/" +
            std::to_string(partial.second_full_recovery_frames) + "/" +
            std::to_string(partial.error_p95_ratio) + "/" +
            std::to_string(partial.base_second_p95_ratio) + ";" +
            std::to_string(doubled_partial.expected_true_reversals) + "/" +
            std::to_string(doubled_partial.completed_true_reversals) + "/" +
            std::to_string(doubled_partial.command_reversals) + "/" +
            std::to_string(
                doubled_partial.wrong_direction_commands_after_base_crossing) +
            "/" + std::to_string(
                doubled_partial.maximum_reverse_latency_frames) + "/" +
            std::to_string(doubled_partial.identity_changes) + "/" +
            std::to_string(
                doubled_partial.left_partial_confirmed_frames) + "/" +
            std::to_string(
                doubled_partial.right_partial_confirmed_frames) + "/" +
            std::to_string(
                doubled_partial.first_full_recovery_frames) + "/" +
            std::to_string(
                doubled_partial.second_full_recovery_frames) + "/" +
            std::to_string(doubled_partial.error_p95_ratio) + "/" +
            std::to_string(doubled_partial.base_second_p95_ratio) +
            "，320首反转=" + partial.first_reversal_context +
            "，640首反转=" + doubled_partial.first_reversal_context +
            "，320全部反转=" + partial.reversal_contexts +
            "，640全部反转=" + doubled_partial.reversal_contexts);
}

void test_delayed_left_motion_quantizes_from_world_feedforward() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr int kActuationDelayFrames = 4;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = -24.0f;
    float camera_x = 0.0f;
    int consecutive_no_command = 0;
    int maximum_no_command = 0;
    int base_only_frames = 0;

    for (int index = 0; index < 960; ++index) {
        camera_x += delayed_commands[index % kActuationDelayFrames] /
            config.counts_per_pixel_x * 0.20f;
        delayed_commands[index % kActuationDelayFrames] = 0;
        const float target_velocity = (index / 120) % 2 == 0
            ? -480.0f : -180.0f;
        world_target_x += target_velocity * kFrameSeconds;
        const float observed_error = world_target_x - camera_x;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body(160.0f + observed_error, 160.0f)};
        const AimResult result = aim.process(frame);
        if (result.has_command) {
            delayed_commands[index % kActuationDelayFrames] =
                result.command.dx_counts;
            consecutive_no_command = 0;
        } else if (index >= 480) {
            ++consecutive_no_command;
            maximum_no_command = std::max(
                maximum_no_command, consecutive_no_command);
        }
        if (index >= 480 && result.has_target &&
            !result.target.delay_compensation_active &&
            std::fabs(result.target.delay_compensation_x) < 0.001f &&
            std::fabs(result.target.delay_compensation_y) < 0.001f &&
            std::fabs(result.target.aim_x - result.target.base_aim_x) <
                0.001f &&
            std::fabs(result.target.aim_y - result.target.base_aim_y) <
                0.001f &&
            result.target.aim_x >= result.target.x1 &&
            result.target.aim_x <= result.target.x2 &&
            result.target.aim_y >= result.target.y1 &&
            result.target.aim_y <= result.target.y2 &&
            std::fabs(result.control.modelled_response_x_counts) < 0.001f) {
            ++base_only_frames;
        }
    }

    expect(base_only_frames == 480,
           "prediction 关闭的左移追踪必须始终直接控制框内基础特征，"
           "不能再由闭环命令历史制造反向延迟端点，满足帧=" +
               std::to_string(base_only_frames));
    expect(maximum_no_command <= 1,
           "左移闭环的连续 PI 不得周期停发，最长停发=" +
               std::to_string(maximum_no_command));
}

void test_tracking_pi_is_separate_from_prediction_projection() {
    const auto run_axis = [](bool prediction_enabled, bool vertical) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 1.0f;
        config.counts_per_pixel_x = 1.0f;
        config.counts_per_pixel_y = 1.0f;
        config.max_counts_per_frame = 100.0f;
        config.acquisition_range_percent = 150.0f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 50.0f;
        config.enable_prediction = prediction_enabled;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);

        AimFrame first = make_frame(1, base);
        first.lock_active = true;
        first.detections = {vertical
            ? body(160.0f, 182.0f)
            : body(180.0f, 172.0f)};
        const AimResult issued = aim.process(first);
        AimFrame second = make_frame(
            2, base + std::chrono::milliseconds(4));
        second.lock_active = true;
        second.detections = first.detections;
        const AimResult pending = aim.process(second);
        return std::pair{issued, pending};
    };

    const auto [tracking_x_issued, tracking_x] = run_axis(false, false);
    const auto [tracking_y_issued, tracking_y] = run_axis(false, true);
    const auto [prediction_x_issued, prediction_x] = run_axis(true, false);
    const auto [prediction_y_issued, prediction_y] = run_axis(true, true);
    const auto tracking_is_base_only = [](const AimResult& result) {
        return result.has_target &&
            !result.target.delay_compensation_active &&
            std::fabs(result.target.delay_compensation_x) < 0.001f &&
            std::fabs(result.target.delay_compensation_y) < 0.001f &&
            std::fabs(result.target.aim_x - result.target.base_aim_x) <
                0.001f &&
            std::fabs(result.target.aim_y - result.target.base_aim_y) <
                0.001f &&
            std::fabs(result.control.modelled_response_x_counts) < 0.001f;
    };
    expect(tracking_x_issued.has_command && tracking_y_issued.has_command &&
               tracking_is_base_only(tracking_x) &&
               tracking_is_base_only(tracking_y),
           "prediction 关闭时 X/Y 追踪都必须直接控制框内基础特征，"
           "不得消费闭环命令历史形成延迟端点");

    const float prediction_x_response =
        -prediction_x.target.delay_compensation_x /
        static_cast<float>(prediction_x_issued.command.dx_counts);
    const float prediction_y_response =
        -prediction_y.target.delay_compensation_y /
        static_cast<float>(prediction_y_issued.command.dy_counts);
    expect(prediction_x_issued.has_command && prediction_x.has_target &&
               prediction_y_issued.has_command && prediction_y.has_target &&
               std::fabs(prediction_x_response - 0.15f) < 0.001f &&
               std::fabs(prediction_y_response - 0.15f) < 0.001f,
           "prediction profile 必须独立保留原有双轴 15% 在途库存契约，"
           "X/Y=" + std::to_string(prediction_x_response) + "/" +
               std::to_string(prediction_y_response));
}

void test_base_tracking_quantization_has_no_speed_threshold() {
    struct Sample {
        float velocity_x = 0.0f;
        float desired_x = 0.0f;
        int command_x = 0;
    };
    const auto run_case = [](float motion_per_frame) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 1.0f;
        config.counts_per_pixel_x = 0.40f;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 150.0f;
        config.enable_delay_compensation = false;
        config.enable_prediction = false;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        AimResult result;
        for (int index = 0; index < 2; ++index) {
            const float target_x = 160.0f + motion_per_frame * index;
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.control_center_x = target_x - 1.0f;
            frame.control_center_y = 160.0f;
            frame.lock_active = true;
            frame.detections = {body(target_x, 172.0f)};
            result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "基础追踪速度阈值删除测试必须逐帧保留确认目标");
        }
        return Sample{
            result.target.velocity_x,
            result.control.desired_x_counts,
            result.has_command ? result.command.dx_counts : 0};
    };
    const Sample below_old_threshold = run_case(0.40f);
    const Sample above_old_threshold = run_case(0.50f);
    expect(below_old_threshold.velocity_x < 10.0f &&
               above_old_threshold.velocity_x > 10.0f &&
               below_old_threshold.desired_x > 0.0f &&
               below_old_threshold.desired_x < 1.0f &&
               above_old_threshold.desired_x > 0.0f &&
               above_old_threshold.desired_x < 1.0f,
           "删除测试必须让同一亚整数需求位于旧 10 px/s 判据两侧");
    expect(below_old_threshold.command_x == 1 &&
               above_old_threshold.command_x == 1,
           "固定 320 ROI 的同向几何运动不得因跨过旧速度阈值改变亚整数分摊");
}

void test_delay_shaping_has_no_speed_threshold_before_prediction() {
    struct Sample {
        float velocity_x = 0.0f;
        float shaped_x = 0.0f;
        bool lead_active = false;
    };
    const auto run_case = [](float motion_per_frame) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 1.0f;
        config.counts_per_pixel_x = 1.0f;
        config.counts_per_pixel_y = 1.0f;
        config.max_counts_per_frame = 20.0f;
        config.acquisition_range_percent = 150.0f;
        config.body_aim_height_ratio = 0.50f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 16.0f;
        config.max_delay_compensation_percent = 50.0f;
        config.enable_prediction = true;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        AimResult result;
        for (int index = 0; index < 3; ++index) {
            const float target_x =
                160.0f + motion_per_frame * static_cast<float>(index);
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.control_center_x = index < 2 ? target_x : target_x - 20.0f;
            frame.control_center_y = 160.0f;
            frame.lock_active = false;
            frame.detections = {body(target_x, 160.0f)};
            result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "延迟整形删除测试必须逐帧保留确认目标");
        }
        return Sample{
            result.target.velocity_x,
            result.control.shaped_x_counts,
            result.target.lead_active};
    };

    const Sample below_old_threshold = run_case(0.30f);
    const Sample above_old_threshold = run_case(1.00f);
    expect(below_old_threshold.velocity_x < 20.0f &&
               above_old_threshold.velocity_x > 20.0f &&
               !below_old_threshold.lead_active &&
               !above_old_threshold.lead_active,
           "删除测试必须只覆盖 prediction 建立前的旧 20 px/s 延迟整形两侧，"
           "速度=" + std::to_string(below_old_threshold.velocity_x) + "/" +
               std::to_string(above_old_threshold.velocity_x) +
               "，lead=" + std::to_string(below_old_threshold.lead_active) +
               "/" + std::to_string(above_old_threshold.lead_active));
    expect(std::fabs(below_old_threshold.shaped_x -
                     above_old_threshold.shaped_x) <= 1.5f,
           "相同 20-count 误差阶跃不得因目标速度跨过旧 20 px/s 判据切换延迟整形，"
           "慢/快=" + std::to_string(below_old_threshold.shaped_x) + "/" +
               std::to_string(above_old_threshold.shaped_x));
}

void test_prediction_direct_feedforward_has_no_absolute_velocity_mode() {
    constexpr float kFrameSeconds = 1.0f / 120.0f;
    constexpr float kBuildMotionPerFrame = 2.50f;
    constexpr float kMeasuredMotionPerFrame = 1.25f;
    constexpr int kBuildFrames = 360;
    constexpr int kActuationDelayFrames = 2;
    constexpr int kFrameCount = 960;
    constexpr int kMeasureFrames = 240;
    // 0.5 count 是整数执行的一半量化步，只约束同一连续轨迹的单帧输出
    // 跳变；它不是人物速度判据，也不得进入生产控制分支。
    constexpr float kMaximumFeedforwardStepCounts = 0.50f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.80f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.body_aim_height_ratio = 0.50f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 16.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = 0.0f;
    float camera_x = 0.0f;
    float previous_feedforward = 0.0f;
    float previous_lead_x = 0.0f;
    bool previous_active_sample = false;
    int valid_frames = 0;
    int lead_active_frames = 0;
    int discontinuous_feedforward_steps = 0;
    float maximum_feedforward_step = 0.0f;
    float maximum_lead_step = 0.0f;
    float maximum_base_error = 0.0f;

    for (int index = 0; index < kFrameCount; ++index) {
        const int slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[slot] /
            config.counts_per_pixel_x;
        delayed_commands[slot] = 0;
        world_target_x += index < kBuildFrames
            ? kBuildMotionPerFrame : kMeasuredMotionPerFrame;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(
                    index * kFrameSeconds * 1000000.0f)));
        frame.control_at = frame.captured_at +
            std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x,
            160.0f, 40.0f, 80.0f)};
        const AimResult result = aim.process(frame);
        if (result.status != AimStatus::SUCCESS || !result.has_target) {
            previous_active_sample = false;
            continue;
        }
        if (result.has_command) {
            delayed_commands[slot] = result.command.dx_counts;
        }
        if (index < kFrameCount - kMeasureFrames) continue;
        ++valid_frames;
        maximum_base_error = std::max(
            maximum_base_error,
            std::fabs(result.target.base_aim_x -
                      frame.control_center_x));
        if (!result.target.lead_active) {
            previous_active_sample = false;
            continue;
        }
        ++lead_active_frames;
        if (previous_active_sample) {
            const float feedforward_step = std::fabs(
                result.control.feedforward_x_counts -
                previous_feedforward);
            const float lead_step = std::fabs(
                result.target.lead_x - previous_lead_x);
            maximum_feedforward_step = std::max(
                maximum_feedforward_step, feedforward_step);
            maximum_lead_step = std::max(
                maximum_lead_step, lead_step);
            if (feedforward_step > kMaximumFeedforwardStepCounts) {
                ++discontinuous_feedforward_steps;
            }
        }
        previous_feedforward = result.control.feedforward_x_counts;
        previous_lead_x = result.target.lead_x;
        previous_active_sample = true;
    }

    expect(valid_frames == kMeasureFrames &&
               lead_active_frames == kMeasureFrames &&
               maximum_base_error < 20.0f,
           "低 cadence X deletion fixture 必须保持同一目标、连续 lead 和框内基础控制，"
           "有效/活动=" + std::to_string(valid_frames) + "/" +
               std::to_string(lead_active_frames) + "，基础误差=" +
               std::to_string(maximum_base_error));
    expect(discontinuous_feedforward_steps == 0 &&
               maximum_feedforward_step <=
                   kMaximumFeedforwardStepCounts,
           "同一连续轨迹的 X direct feedforward 不得因旧绝对 counts/s 分类"
           "反复切换公式，超半量化步帧=" +
               std::to_string(discontinuous_feedforward_steps) +
               "，最大前馈步长=" +
               std::to_string(maximum_feedforward_step) +
               "，同期最大 lead 步长=" +
               std::to_string(maximum_lead_step));
}

void test_prediction_confirmed_stop_has_no_absolute_velocity_mode() {
    constexpr float kFrameSeconds = 1.0f / 120.0f;
    constexpr int kActuationDelayFrames = 2;
    constexpr int kBuildFrames = 360;
    constexpr int kMovingFrames = 480;
    constexpr int kFrameCount = 500;
    // 0.5 count 是整数执行的一半量化步，只用于确认连续状态清理；它不是
    // 人物速度判据，也不得进入生产控制分支。
    constexpr float kMaximumResidualFeedforwardCounts = 0.50f;
    struct Sample {
        int valid_frames = 0;
        float before_feedforward = 0.0f;
        float confirmed_feedforward = 0.0f;
        float after_feedforward = 0.0f;
        float confirmed_lead = 0.0f;
        bool confirmed_active = false;
    };
    const auto run_case = [&](float build_motion_per_frame,
                              float counts_per_pixel_x) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 1.5f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = counts_per_pixel_x;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 100.0f;
        config.body_aim_height_ratio = 0.50f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 16.0f;
        config.max_delay_compensation_ms = 16.0f;
        config.max_delay_compensation_percent = 15.0f;
        config.enable_prediction = true;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        std::array<int, kActuationDelayFrames> delayed_commands{};
        float world_target_x = 0.0f;
        float camera_x = 0.0f;
        Sample sample;

        for (int index = 0; index < kFrameCount; ++index) {
            const int slot = index % kActuationDelayFrames;
            camera_x += delayed_commands[slot] /
                config.counts_per_pixel_x;
            delayed_commands[slot] = 0;
            if (index < kBuildFrames) {
                world_target_x += build_motion_per_frame;
            } else if (index < kMovingFrames) {
                world_target_x += 4.0f;
            }
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(
                        index * kFrameSeconds * 1000000.0f)));
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.lock_active = true;
            frame.detections = {body_box(
                160.0f + world_target_x - camera_x,
                160.0f, 40.0f, 80.0f)};
            const AimResult result = aim.process(frame);
            if (result.status != AimStatus::SUCCESS ||
                !result.has_target) {
                continue;
            }
            ++sample.valid_frames;
            if (result.has_command) {
                delayed_commands[slot] = result.command.dx_counts;
            }
            if (index == kMovingFrames + 12) {
                sample.before_feedforward =
                    result.control.feedforward_x_counts;
            } else if (index == kMovingFrames + 13) {
                sample.confirmed_feedforward =
                    result.control.feedforward_x_counts;
                sample.confirmed_lead = result.target.lead_x;
                sample.confirmed_active = result.target.lead_active;
            } else if (index == kMovingFrames + 14) {
                sample.after_feedforward =
                    result.control.feedforward_x_counts;
            }
        }
        return sample;
    };

    const Sample above_old_threshold = run_case(4.90f, 0.845f);
    const Sample below_old_threshold = run_case(5.10f, 0.805f);
    expect(above_old_threshold.valid_frames == kFrameCount &&
               below_old_threshold.valid_frames == kFrameCount &&
               above_old_threshold.confirmed_active &&
               below_old_threshold.confirmed_active &&
               above_old_threshold.before_feedforward >
                   kMaximumResidualFeedforwardCounts &&
               below_old_threshold.before_feedforward >
                   kMaximumResidualFeedforwardCounts,
           "confirmed-stop deletion fixture 必须保持同一目标、已建立 X lead，"
           "并在确认帧前仍有前馈；有效帧=" +
               std::to_string(above_old_threshold.valid_frames) + "/" +
               std::to_string(below_old_threshold.valid_frames) +
               "，活动=" +
               std::to_string(above_old_threshold.confirmed_active) + "/" +
               std::to_string(below_old_threshold.confirmed_active) +
               "，确认前前馈=" +
               std::to_string(above_old_threshold.before_feedforward) + "/" +
               std::to_string(below_old_threshold.before_feedforward));
    expect(std::fabs(above_old_threshold.confirmed_feedforward) <=
                   kMaximumResidualFeedforwardCounts &&
               std::fabs(below_old_threshold.confirmed_feedforward) <=
                   kMaximumResidualFeedforwardCounts &&
               std::fabs(above_old_threshold.after_feedforward) <=
                   kMaximumResidualFeedforwardCounts &&
               std::fabs(below_old_threshold.after_feedforward) <=
                   kMaximumResidualFeedforwardCounts,
           "同一 12 帧 confirmed-stop 不得因旧绝对 counts/s 分类决定是否清理 X "
           "prediction，确认帧前馈=" +
               std::to_string(above_old_threshold.confirmed_feedforward) + "/" +
               std::to_string(below_old_threshold.confirmed_feedforward) +
               "，后一帧=" +
               std::to_string(above_old_threshold.after_feedforward) + "/" +
               std::to_string(below_old_threshold.after_feedforward) +
               "，确认帧 lead=" +
               std::to_string(above_old_threshold.confirmed_lead) + "/" +
               std::to_string(below_old_threshold.confirmed_lead));
}

void test_prediction_stop_measurement_has_no_absolute_velocity_mode() {
    constexpr float kFrameSeconds = 1.0f / 120.0f;
    constexpr int kActuationDelayFrames = 2;
    constexpr int kBuildFrames = 360;
    constexpr int kMovingFrames = 480;
    constexpr int kEvidenceFrame = kMovingFrames + 19;
    constexpr int kFrameCount = 520;
    constexpr float kCountsPerPixelX = 0.81f;
    constexpr float kCameraResponse = 1.10f;
    constexpr float kRetainedFeedforwardRatio = 0.75f;
    const auto make_config = [&] {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 1.5f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = kCountsPerPixelX;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 100.0f;
        config.body_aim_height_ratio = 0.50f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 16.0f;
        config.max_delay_compensation_ms = 16.0f;
        config.max_delay_compensation_percent = 15.0f;
        config.enable_prediction = true;
        return config;
    };
    struct MotionSample {
        int valid_frames = 0;
        float before_feedforward = 0.0f;
        float evidence_feedforward = 0.0f;
        float after_feedforward = 0.0f;
        float before_lead = 0.0f;
        float evidence_lead = 0.0f;
        bool before_active = false;
        bool evidence_active = false;
        bool after_active = false;
    };
    const auto run_moving_case = [&] {
        AimConfig config = make_config();
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        std::array<int, kActuationDelayFrames> delayed_commands{};
        float world_target_x = 2.5f;
        float camera_x = 0.0f;
        MotionSample sample;
        for (int index = 0; index < kFrameCount; ++index) {
            const int slot = index % kActuationDelayFrames;
            camera_x += delayed_commands[slot] /
                config.counts_per_pixel_x * kCameraResponse;
            delayed_commands[slot] = 0;
            if (index < kBuildFrames) {
                world_target_x += 2.5f;
            } else if (index < kMovingFrames) {
                world_target_x += 1.25f;
            }
            if (index == kEvidenceFrame) {
                // 停稳后的同向 1.65 px 小位移只提供一次新的屏幕运动证据，
                // 不改变 120 Hz、两帧反馈、控制参数或物理上限。
                world_target_x += 1.65f;
            }
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(static_cast<long long>(
                    index * kFrameSeconds * 1000000.0f)));
            frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
            frame.lock_active = true;
            frame.detections = {body_box(
                160.0f + world_target_x - camera_x,
                160.0f, 40.0f, 80.0f)};
            const AimResult result = aim.process(frame);
            if (result.status != AimStatus::SUCCESS || !result.has_target) {
                continue;
            }
            ++sample.valid_frames;
            if (result.has_command) {
                delayed_commands[slot] = result.command.dx_counts;
            }
            if (index == kEvidenceFrame) {
                sample.before_feedforward =
                    result.control.feedforward_x_counts;
                sample.before_lead = result.target.lead_x;
                sample.before_active = result.target.lead_active;
            } else if (index == kEvidenceFrame + 1) {
                sample.evidence_feedforward =
                    result.control.feedforward_x_counts;
                sample.evidence_lead = result.target.lead_x;
                sample.evidence_active = result.target.lead_active;
            } else if (index == kEvidenceFrame + 2) {
                sample.after_feedforward =
                    result.control.feedforward_x_counts;
                sample.after_active = result.target.lead_active;
            }
        }
        return sample;
    };

    const MotionSample moving = run_moving_case();
    expect(moving.valid_frames == kFrameCount && moving.before_active &&
               moving.before_feedforward > 0.0f && moving.before_lead > 0.0f,
           "测量源 deletion fixture 必须保持同一目标和已建立的同向 X lead，"
           "有效帧=" + std::to_string(moving.valid_frames) +
               "，确认前活动=" + std::to_string(moving.before_active) +
               "，前馈=" + std::to_string(moving.before_feedforward) +
               "，lead=" + std::to_string(moving.before_lead));
    expect(moving.evidence_active && moving.after_active &&
               moving.evidence_feedforward >=
                   moving.before_feedforward * kRetainedFeedforwardRatio &&
               moving.after_feedforward >=
                   moving.before_feedforward * kRetainedFeedforwardRatio,
           "同一停止窗内的新同向运动证据不得仅因旧绝对 counts/s 资格被忽略，"
           "确认前/证据帧/后一帧前馈=" +
               std::to_string(moving.before_feedforward) + "/" +
               std::to_string(moving.evidence_feedforward) + "/" +
               std::to_string(moving.after_feedforward) +
               "，活动=" + std::to_string(moving.before_active) + "/" +
               std::to_string(moving.evidence_active) + "/" +
               std::to_string(moving.after_active) +
               "，证据帧 lead=" + std::to_string(moving.evidence_lead));

    AimConfig static_config = make_config();
    Aim static_aim(static_config);
    const auto static_base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> static_delayed_commands{};
    constexpr float kStaticWorldTargetX = 2.5f;
    float static_camera_x = 0.0f;
    int static_command_frames = 0;
    int static_prediction_frames = 0;
    float maximum_static_lead = 0.0f;
    for (int index = 0; index < kFrameCount; ++index) {
        const int slot = index % kActuationDelayFrames;
        static_camera_x += static_delayed_commands[slot] /
            static_config.counts_per_pixel_x * kCameraResponse;
        static_delayed_commands[slot] = 0;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            static_base + std::chrono::microseconds(static_cast<long long>(
                index * kFrameSeconds * 1000000.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + kStaticWorldTargetX - static_camera_x,
            160.0f, 40.0f, 80.0f)};
        const AimResult result = static_aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "同 cadence 静态相机反馈负例必须逐帧保留合法目标");
        if (!result.has_target) continue;
        const float lead = std::fabs(result.target.lead_x);
        maximum_static_lead = std::max(maximum_static_lead, lead);
        if (result.target.lead_active && lead > 0.25f) {
            ++static_prediction_frames;
        }
        if (result.has_command) {
            ++static_command_frames;
            static_delayed_commands[slot] = result.command.dx_counts;
        }
    }
    expect(static_command_frames > 0 && static_prediction_frames == 0 &&
               maximum_static_lead <= 0.25f,
           "删除测量源速度资格后，120 Hz 两帧静态相机反馈仍不得建立 X prediction，"
           "命令帧=" + std::to_string(static_command_frames) +
               "，prediction 帧=" + std::to_string(static_prediction_frames) +
               "，最大 lead=" + std::to_string(maximum_static_lead));
}

void test_tracking_public_point_is_independent_of_command_age() {
    const auto project_after = [](int age_microseconds) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 1.0f;
        config.counts_per_pixel_x = 1.0f;
        config.counts_per_pixel_y = 1.0f;
        config.max_counts_per_frame = 100.0f;
        config.acquisition_range_percent = 150.0f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 50.0f;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        AimFrame first = make_frame(1, base);
        first.lock_active = true;
        first.detections = {body(180.0f, 172.0f)};
        const AimResult issued = aim.process(first);
        AimFrame second = make_frame(
            2, base + std::chrono::microseconds(age_microseconds));
        second.lock_active = true;
        second.detections = {body(180.0f, 172.0f)};
        const AimResult projected = aim.process(second);
        return std::pair{issued, projected};
    };

    const auto [young_issued, young] = project_after(4000);
    const auto [before_issued, before] = project_after(14900);
    const auto [after_issued, after] = project_after(15100);
    const auto [expired_issued, expired] = project_after(30000);
    expect(young_issued.has_command &&
               young_issued.command.dx_counts != 0 && young.has_target &&
               before_issued.has_command &&
               before_issued.command.dx_counts != 0 && before.has_target &&
               after_issued.has_command &&
               after_issued.command.dx_counts != 0 && after.has_target &&
               expired_issued.has_command &&
               expired_issued.command.dx_counts != 0 && expired.has_target,
           "命令年龄回归必须生成非零首帧命令和有效次帧目标；"
           "4/14.9 ms 样本才真实含有在途库存");
    expect(young_issued.command.dx_counts ==
                   before_issued.command.dx_counts &&
               before_issued.command.dx_counts ==
                   after_issued.command.dx_counts &&
               before_issued.command.dx_counts ==
                   expired_issued.command.dx_counts,
           "命令年龄回归必须从相同首帧整数命令开始");
    const auto is_base_only = [](const AimResult& result) {
        return result.has_target &&
            !result.target.delay_compensation_active &&
            std::fabs(result.target.delay_compensation_x) < 0.001f &&
            std::fabs(result.target.delay_compensation_y) < 0.001f &&
            std::fabs(result.target.aim_x - result.target.base_aim_x) <
                0.001f &&
            std::fabs(result.target.aim_y - result.target.base_aim_y) <
                0.001f &&
            std::fabs(result.control.modelled_response_x_counts) < 0.001f &&
            std::fabs(result.control.observer_phase_command_x_counts) <
                0.001f;
    };
    expect(is_base_only(young) && is_base_only(before) &&
               is_base_only(after) && is_base_only(expired),
           "tracking 公有瞄点必须与命令年龄解耦；4/14.9/15.1/30 ms"
           " 均不得重新引入闭环命令响应投影，实际投影=" +
               std::to_string(young.target.delay_compensation_x) + "/" +
               std::to_string(before.target.delay_compensation_x) + "/" +
               std::to_string(after.target.delay_compensation_x) + "/" +
               std::to_string(expired.target.delay_compensation_x));
}

void test_pending_command_age_uses_control_execution_time() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 1.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 100.0f;
    config.acquisition_range_percent = 150.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 50.0f;
    config.enable_prediction = true;
    Aim aim(config);
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);

    AimFrame first = make_frame(1, base);
    first.control_at = base + std::chrono::milliseconds(10);
    first.lock_active = true;
    first.detections = {body(180.0f, 172.0f)};
    const AimResult issued = aim.process(first);

    // 相对截图已经过去 20 ms，但相对实际控制执行仅过去 10 ms；首条命令
    // 仍应属于 15 ms 在途窗。若错误使用 captured_at，库存会被提前清除。
    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(14));
    second.control_at = base + std::chrono::milliseconds(20);
    second.lock_active = true;
    second.detections = {body(180.0f, 172.0f)};
    const AimResult pending = aim.process(second);
    const float response = issued.command.dx_counts != 0
        ? -pending.target.delay_compensation_x /
            static_cast<float>(issued.command.dx_counts)
        : 0.0f;
    expect(issued.has_command && issued.command.dx_counts != 0 &&
               pending.has_target &&
               std::fabs(response - 0.15f) < 0.001f,
           "命令库存年龄必须从控制执行时刻计算，不能从截图时刻提前到期，"
           "响应=" + std::to_string(response));
}


void test_tracking_pi_filters_axes_independently() {
    const auto run_case = [](float roi_scale) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 1.0f;
        config.counts_per_pixel_x = 1.0f;
        config.counts_per_pixel_y = 1.0f;
        config.max_counts_per_frame = 100.0f;
        config.acquisition_range_percent = 150.0f;
        config.body_aim_height_ratio = 0.50f;
        config.enable_prediction = false;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 44.0f;
        config.max_delay_compensation_percent = 50.0f;
        Aim combined(config);
        Aim horizontal_only(config);
        const auto base =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        const float roi_width = 320.0f * roi_scale;
        const auto make_case_frame = [&](int index, float center_x,
                                         float control_center_y) {
            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(index) * 4167));
            frame.roi_width = static_cast<int>(roi_width);
            frame.roi_height = static_cast<int>(roi_width);
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(1);
            frame.control_center_x = roi_width * 0.50f;
            frame.control_center_y = control_center_y;
            frame.lock_active = true;
            frame.detections = {body_box(
                center_x,
                roi_width * 0.45f,
                roi_width * 0.125f,
                roi_width * 0.25f)};
            return frame;
        };

        const auto combined_frame = [&](int index, float center_x) {
            return make_case_frame(
                index, center_x, roi_width * 0.50f);
        };
        const auto horizontal_frame = [&](int index, float center_x) {
            return make_case_frame(
                index, center_x, roi_width * 0.45f);
        };
        const AimResult vertical = combined.process(combined_frame(
            0, roi_width * 0.50f));
        horizontal_only.process(horizontal_frame(
            0, roi_width * 0.50f));
        const AimResult rotated = combined.process(combined_frame(
            1, roi_width * 0.55f));
        const AimResult horizontal_rotated = horizontal_only.process(
            horizontal_frame(1, roi_width * 0.55f));
        const AimResult growing = combined.process(combined_frame(
            2, roi_width * 0.55f));
        const AimResult horizontal_growing = horizontal_only.process(
            horizontal_frame(2, roi_width * 0.55f));
        expect(vertical.status == AimStatus::SUCCESS &&
                   vertical.has_command &&
                   vertical.command.dx_counts == 0 &&
                   vertical.command.dy_counts < 0,
               "二维方向整形回归必须先建立纯 Y 命令");
        expect(rotated.status == AimStatus::SUCCESS &&
                   rotated.has_command &&
                   rotated.command.dx_counts > 0 &&
                   rotated.command.dy_counts < 0 &&
                   rotated.control.evaluated &&
                   !rotated.control.post_alignment_growth_limited_x &&
                   horizontal_rotated.has_command &&
                   rotated.command.dx_counts ==
                       horizontal_rotated.command.dx_counts &&
                   std::fabs(rotated.control.filtered_x_counts -
                       horizontal_rotated.control.filtered_x_counts) <
                       0.001f &&
                   growing.status == AimStatus::SUCCESS &&
                   growing.has_command &&
                   growing.command.dx_counts > 0 &&
                   growing.command.dy_counts < 0 &&
                   horizontal_growing.has_command &&
                   growing.command.dx_counts ==
                       horizontal_growing.command.dx_counts &&
                   std::fabs(growing.control.filtered_x_counts -
                       horizontal_growing.control.filtered_x_counts) <
                       0.001f,
               "追踪 PI 的 X 滤波必须与既有 Y 误差解耦；加入 Y 误差不得"
               "改变相同 X 图像特征产生的 X 命令，ROI/命令=" +
                   std::to_string(roi_width) + "/(" +
                   std::to_string(vertical.command.dx_counts) + "," +
                   std::to_string(vertical.command.dy_counts) + ")->(" +
                   std::to_string(rotated.command.dx_counts) + "," +
                   std::to_string(rotated.command.dy_counts) + ")->(" +
                   std::to_string(growing.command.dx_counts) + "," +
                   std::to_string(growing.command.dy_counts) + ")");
        return std::pair<int, int>{
            rotated.command.dx_counts, growing.command.dx_counts};
    };

    const auto normal = run_case(1.0f);
    const auto doubled = run_case(2.0f);
    expect(std::abs(doubled.first - normal.first * 2) <= 1 &&
               std::abs(doubled.second - normal.second * 2) <= 1,
           "双轴独立连续控制必须保持 320/640 ROI 几何同构，320/640=" +
               std::to_string(normal.first) + "," +
               std::to_string(normal.second) + "/" +
               std::to_string(doubled.first) + "," +
               std::to_string(doubled.second));
}


void test_prediction_uses_world_motion_when_delay_vector_points_backward() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr int kActuationDelayFrames = 4;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = -24.0f;
    float camera_x = 0.0f;
    int backward_delay_frames = 0;
    int wrong_prediction_direction_frames = 0;
    int prediction_not_ahead_of_base_frames = 0;
    int all_wrong_prediction_direction_frames = 0;
    int lead_active_frames = 0;
    int shifted_base_frames = 0;
    float maximum_lead_distance = 0.0f;
    std::string first_not_ahead_trace;

    for (int index = 0; index < 960; ++index) {
        camera_x += delayed_commands[index % kActuationDelayFrames] /
            config.counts_per_pixel_x * 0.20f;
        delayed_commands[index % kActuationDelayFrames] = 0;
        const float target_velocity = (index / 120) % 2 == 0
            ? -480.0f : -180.0f;
        world_target_x += target_velocity * kFrameSeconds;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {
            body(160.0f + world_target_x - camera_x, 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "左移世界方向预测回归必须每帧保留合法目标");
        if (!result.has_target) continue;
        const float box_width = result.target.x2 - result.target.x1;
        const float base_ratio = box_width > 0.0f
            ? (result.target.base_aim_x - result.target.x1) / box_width
            : 0.5f;
        if (std::fabs(base_ratio - 0.5f) > 0.05f) {
            ++shifted_base_frames;
        }
        if (result.target.lead_active &&
            std::fabs(result.target.lead_x) > 0.0f) {
            ++lead_active_frames;
            maximum_lead_distance = std::max(
                maximum_lead_distance,
                std::hypot(result.target.lead_x, result.target.lead_y));
            if (result.target.lead_x >= 0.0f) {
                ++all_wrong_prediction_direction_frames;
            }
        }
        if (result.target.lead_active &&
            std::fabs(result.target.lead_x) > 0.0f &&
            result.target.base_aim_x < frame.control_center_x &&
            result.target.delay_compensation_x > 0.5f) {
            ++backward_delay_frames;
            if (result.target.lead_x >= 0.0f ||
                result.target.aim_x >=
                    result.target.delay_compensated_aim_x) {
                ++wrong_prediction_direction_frames;
            }
            // MoveLeft 的延迟点被屏幕相对速度拉到基础点右侧时，prediction
            // 必须先吃掉这段反向位移，再从基础点继续沿世界运动方向前探。
            // 只验证最终投影，不允许为了通过回归改写轨迹或基础瞄点。
            if (result.target.aim_x >= result.target.base_aim_x) {
                ++prediction_not_ahead_of_base_frames;
                if (first_not_ahead_trace.empty()) {
                    first_not_ahead_trace =
                        "帧=" + std::to_string(index) +
                        "，base=" +
                        std::to_string(result.target.base_aim_x) +
                        "，delay=" +
                        std::to_string(result.target.delay_compensated_aim_x) +
                        "，final=" +
                        std::to_string(result.target.aim_x) +
                        "，lead=" +
                        std::to_string(result.target.lead_x) +
                        "，delay_dx=" +
                        std::to_string(result.target.delay_compensation_x);
                }
            }
        }
        if (result.has_command) {
            delayed_commands[index % kActuationDelayFrames] =
                result.command.dx_counts;
        }
    }

    expect(backward_delay_frames > 0,
           "左移预测必须实际覆盖延迟拉回向量与世界运动方向相反的窗口");
    expect(wrong_prediction_direction_frames == 0,
           "延迟向量向右拉回时，MoveLeft prediction 仍必须沿世界运动向左，错误帧=" +
               std::to_string(wrong_prediction_direction_frames));
    expect(prediction_not_ahead_of_base_frames == 0,
           "延迟补偿与 MoveLeft 世界运动反向时，最终预测点必须越过基础点形成可见提前量，未越过帧=" +
               std::to_string(prediction_not_ahead_of_base_frames) +
               "，首帧=" + first_not_ahead_trace);
    expect(lead_active_frames > 0 &&
               all_wrong_prediction_direction_frames == 0 &&
               maximum_lead_distance >= 0.25f &&
               maximum_lead_distance <=
                   std::hypot(40.0f, 80.0f) *
                       config.max_prediction_lead_percent / 100.0f +
                       0.01f,
           "MoveLeft 全程的 prediction 必须只向左，且反向延迟抵消量与额外前探都受几何上限约束，活动=" +
               std::to_string(lead_active_frames) + "，错向=" +
               std::to_string(all_wrong_prediction_direction_frames) +
               "，最大提前=" +
               std::to_string(maximum_lead_distance));
    expect(shifted_base_frames == 0,
           "prediction 只能移动最终点，基础瞄点必须保持模型框中央附近，偏移帧=" +
               std::to_string(shifted_base_frames));
}

void test_prediction_survives_short_world_motion_measurement_dips() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    constexpr float kFrameSeconds = 1.0f / 60.0f;
    constexpr float kWorldPixelsPerFrame = -1.5f;
    constexpr float kCameraResponse = 0.50f;
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float world_target_x = 40.0f;
    float camera_x = 0.0f;
    int delayed_command = 0;
    bool lead_armed = false;
    int armed_frames = 0;
    int lead_dropout_frames = 0;
    int lead_state_edges = 0;
    bool previous_lead_active = false;

    for (int index = 0; index < 180; ++index) {
        camera_x += delayed_command / config.counts_per_pixel_x *
            kCameraResponse;
        delayed_command = 0;
        // 人物持续 MoveLeft，但模型动画允许连续十帧的中心速度低谷。
        // 低谷小于新的生产停止确认窗口，不得把已经建立的世界运动状态释放。
        const int phase = index % 36;
        if (phase < 20 || phase > 29) {
            world_target_x += kWorldPixelsPerFrame;
        }
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * kFrameSeconds * 1000000.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        const int pose_phase = index % 40;
        const float pose_progress = pose_phase <= 20
            ? static_cast<float>(pose_phase) / 20.0f
            : static_cast<float>(40 - pose_phase) / 20.0f;
        const float pose_x = -1.5f + 3.0f * pose_progress;
        const float pose_width = 40.0f - 2.0f + 4.0f * pose_progress;
        frame.detections = {
            body_box(
                160.0f + world_target_x - camera_x + pose_x,
                160.0f, pose_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "短时世界运动低谷回归必须每帧保留合法目标");
        if (!result.has_target) continue;
        if (result.target.lead_active) {
            lead_armed = true;
        }
        if (lead_armed) {
            ++armed_frames;
            if (!result.target.lead_active) {
                ++lead_dropout_frames;
            }
        }
        if (index > 0 && result.target.lead_active != previous_lead_active) {
            ++lead_state_edges;
        }
        previous_lead_active = result.target.lead_active;
        if (result.has_command) {
            delayed_command = result.command.dx_counts;
        }
    }

    expect(lead_armed && armed_frames >= 90,
           "短时世界运动低谷回归必须先建立足够长的 prediction 观察窗口");
    expect(lead_dropout_frames == 0 && lead_state_edges <= 1,
           "少于五帧的世界运动测量低谷不得让 prediction 周期性开关，掉线帧=" +
               std::to_string(lead_dropout_frames) + "，边沿=" +
               std::to_string(lead_state_edges));
}

void test_prediction_motion_candidate_tolerates_one_low_sample() {
    constexpr float kMinimumCounts = 0.25f;
    constexpr float kDeltaSeconds = 0.05f;
    constexpr float kEstablishmentSeconds = 0.15f;
    constexpr int kGraceFrames = 1;
    float candidate_seconds = 0.0f;
    int low_frames = 0;
    const auto update = [&](float measurement) {
        return aim::detail::update_prediction_motion_candidate(
            measurement, kMinimumCounts, kDeltaSeconds,
            kEstablishmentSeconds, kGraceFrames,
            candidate_seconds, low_frames);
    };

    expect(!update(-0.50f) && candidate_seconds < -0.049f &&
               low_frames == 0,
           "首次 prediction 候选必须按有效世界测量累计同向时长");
    expect(!update(0.0f) && candidate_seconds < -0.049f &&
               low_frames == 1,
           "单个低世界测量只能暂停首次确认，不能清空已有同向证据");
    const bool resumed_without_confirmation = update(-0.50f);
    const bool confirmed_after_resume = update(-0.50f);
    expect(!resumed_without_confirmation && confirmed_after_resume &&
               candidate_seconds < -0.149f && low_frames == 0,
           "单帧低谷后恢复同向测量必须继续累计并按原窗口确认");

    candidate_seconds = -0.10f;
    low_frames = 0;
    const bool first_low_confirmed = update(0.0f);
    const bool second_low_confirmed = update(0.0f);
    expect(!first_low_confirmed && !second_low_confirmed &&
               candidate_seconds == 0.0f && low_frames == 2,
           "连续第二个低世界测量必须清空候选，禁止拼接断续相机反馈");

    candidate_seconds = -0.10f;
    low_frames = 0;
    expect(!update(0.50f) && candidate_seconds > 0.049f &&
               candidate_seconds < 0.051f && low_frames == 0,
           "世界测量反向时必须从新方向重新计时，不能继承旧方向资格");
}

void test_prediction_motion_axis_requires_confirmed_stop() {
    constexpr float kDeltaSeconds = 1.0f / 60.0f;
    constexpr float kMinimumCounts = 0.25f;
    constexpr int kConfirmFrames = 5;
    constexpr float kBuildGain = 2.0f;
    constexpr float kReleaseGain = 120.0f;
    // 独立 prediction 状态使用 counts/second；60 FPS 下 1 count/frame
    // 等价于 60 counts/second。
    float velocity = 60.0f;
    int low_frames = 0;

    // 同向前馈从 1.0 降到 0.6 是速度变化，不是停止。它既不能朝零释放，
    // 也不能用停止增益逐帧复制新值，否则模型框动画会直接变成提前点晃动。
    aim::detail::update_prediction_velocity_axis(
        0.60f, 0.60f, kDeltaSeconds, kMinimumCounts, kConfirmFrames,
        kBuildGain, kReleaseGain, velocity, low_frames);
    expect(velocity > 59.0f && velocity < 60.0f && low_frames == 0,
           "同向世界运动幅值下降必须按持续运动增益平滑，不能复制框动画，实际=" +
               std::to_string(velocity));

    velocity = 60.0f;
    low_frames = 0;
    for (int index = 0; index < kConfirmFrames - 1; ++index) {
        aim::detail::update_prediction_velocity_axis(
            0.0f, 0.0f, kDeltaSeconds, kMinimumCounts, kConfirmFrames,
            kBuildGain, kReleaseGain, velocity, low_frames);
    }
    const float minimum_velocity = kMinimumCounts / kDeltaSeconds;
    expect(velocity > minimum_velocity &&
               low_frames == kConfirmFrames - 1,
           "少于五帧的低测量只能慢速保持 prediction，实际=" +
               std::to_string(velocity) + "，计数=" +
               std::to_string(low_frames));
    aim::detail::update_prediction_velocity_axis(
        0.0f, 0.0f, kDeltaSeconds, kMinimumCounts, kConfirmFrames,
        kBuildGain, kReleaseGain, velocity, low_frames);
    expect(velocity < minimum_velocity && low_frames == kConfirmFrames,
           "连续五帧低测量后必须快速释放 prediction，实际=" +
               std::to_string(velocity) + "，计数=" +
               std::to_string(low_frames));
}

void test_prediction_release_offset_is_slew_limited() {
    constexpr float kBoxDiagonal = 110.0f;
    constexpr float kMaximumSlew = 1.5f;
    constexpr float kDeltaSeconds = 1.0f / 240.0f;
    constexpr float kMaximumLeadPercent = 35.0f;
    const float maximum_step =
        kBoxDiagonal * kMaximumSlew * kDeltaSeconds;
    float offset_x = -22.0f;
    float offset_y = 0.0f;
    const float target_offset_x = 13.0f;
    const float target_offset_y = 0.0f;
    float maximum_observed_step = 0.0f;

    for (int index = 0; index < 200; ++index) {
        const float previous_x = offset_x;
        const float previous_y = offset_y;
        aim::detail::slew_prediction_offset(
            target_offset_x, target_offset_y,
            target_offset_x, target_offset_y,
            kBoxDiagonal, kMaximumLeadPercent, kMaximumSlew,
            kDeltaSeconds, offset_x, offset_y);
        maximum_observed_step = std::max(
            maximum_observed_step,
            std::hypot(offset_x - previous_x, offset_y - previous_y));
    }

    expect(maximum_observed_step <= maximum_step + 0.001f,
           "prediction 退出回收到延迟点时不得绕过偏移限速，最大步长=" +
               std::to_string(maximum_observed_step) + "，上限=" +
               std::to_string(maximum_step));
    expect(std::hypot(offset_x - target_offset_x,
                      offset_y - target_offset_y) <= 0.001f,
           "prediction 退出限速最终必须准确回到延迟补偿点，残差=" +
               std::to_string(std::hypot(
                   offset_x - target_offset_x,
                   offset_y - target_offset_y)));
}

void test_prediction_pullback_command_requires_causal_world_motion() {
    constexpr float kMinimumWorldCounts = 0.25f;
    const auto allowed = [&](float desired_counts, float final_error_pixels,
                             float world_measurement_counts,
                             float prediction_direction) {
        return aim::detail::prediction_pullback_command_allowed(
            desired_counts, final_error_pixels, world_measurement_counts,
            prediction_direction,
            kMinimumWorldCounts);
    };

    expect(allowed(0.8f, 1.0f, -0.5f, -1.0f),
           "X 轴命令朝当前最终点且世界测量支持历史 prediction 时必须放行");
    expect(!allowed(-0.8f, 1.0f, -0.5f, -1.0f),
           "命令背离当前最终点时必须继续执行反拉停发");
    expect(allowed(0.8f, 3.0f, -0.5f, -1.0f),
           "库存补偿生效后，保持带外朝最终点的正确制动也必须放行");
    expect(!allowed(0.8f, 1.0f, -0.25f, -1.0f),
           "世界测量处于 prediction 噪声门槛时不得放行");
    expect(!allowed(0.8f, 1.0f, 0.5f, -1.0f),
           "世界测量背离历史 prediction 时不得放行");
    expect(!allowed(0.8f, 1.0f, -0.5f, 0.0f),
           "没有有效历史 prediction 方向时不得放行");
}

void test_prediction_inventory_brake_is_short_and_minimal() {
    constexpr int kMaximumFrames = 0;
    expect(!aim::detail::prediction_inventory_brake_allowed(
               1, 0, kMaximumFrames) &&
               !aim::detail::prediction_inventory_brake_allowed(
                   -1, 0, kMaximumFrames),
           "公开 prediction 点保持带外不得再发送任何反向库存制动");
    expect(!aim::detail::prediction_inventory_brake_allowed(
               2, 0, kMaximumFrames) &&
               !aim::detail::prediction_inventory_brake_allowed(
                   -2, 0, kMaximumFrames),
           "反向库存制动不得发送超过 1 count 的可见脉冲");
}

void test_prediction_closed_loop_keeps_visible_left_lead_without_pullback() {
    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr int kActuationDelayFrames = 4;
    constexpr int kMovingFrames = 1100;
    constexpr int kFrameCount = 1260;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = -12.0f;
    float camera_x = 0.0f;
    bool previous_lead_active = false;
    float previous_forecast_x = 0.0f;
    bool previous_forecast_valid = false;
    int moving_lead_frames = 0;
    int moving_lead_edges = 0;
    int pullback_command_frames = 0;
    int moving_direction_switches = 0;
    int previous_nonzero_command_sign = 0;
    int stop_tail_command_frames = 0;
    int stop_tail_lead_frames = 0;
    std::vector<float> moving_base_offsets;
    std::vector<float> moving_forecast_distances;
    std::vector<float> moving_forecast_steps;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * 0.18f;
        delayed_commands[delay_slot] = 0;
        if (index < kMovingFrames) {
            world_target_x -= 0.65f;
        }

        // 高频中心与宽度扰动近似人物步态和模型框动画；真实平移仍只由
        // world_target_x 表达。prediction 必须平滑世界运动，不能把框动画
        // 变成逐帧提前量跳动。
        const float animation_scale = index < kMovingFrames ? 1.0f : 0.0f;
        const float animation_x = animation_scale * 0.35f *
            std::sin(index * 0.73f);
        const float animation_width = 40.0f +
            animation_scale * 3.0f * std::sin(index * 0.51f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x + animation_x,
            160.0f, animation_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "prediction 闭环提前回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[delay_slot] = command_x;
        }
        if (index >= 500 && index < kMovingFrames) {
            if (result.target.lead_active != previous_lead_active) {
                ++moving_lead_edges;
            }
            if (result.target.lead_active) {
                ++moving_lead_frames;
                const float forecast_x = result.target.aim_x -
                    result.target.base_aim_x;
                moving_base_offsets.push_back(
                    result.target.base_aim_x - frame.control_center_x);
                moving_forecast_distances.push_back(-forecast_x);
                if (previous_forecast_valid) {
                    moving_forecast_steps.push_back(
                        std::fabs(forecast_x - previous_forecast_x));
                }
                previous_forecast_x = forecast_x;
                previous_forecast_valid = true;
            } else {
                previous_forecast_valid = false;
            }
            if (command_x > 0) {
                ++pullback_command_frames;
            }
            const int command_sign = command_x < 0 ? -1 :
                command_x > 0 ? 1 : 0;
            if (command_sign != 0) {
                if (previous_nonzero_command_sign != 0 &&
                    command_sign != previous_nonzero_command_sign) {
                    ++moving_direction_switches;
                }
                previous_nonzero_command_sign = command_sign;
            }
        }
        if (index >= kFrameCount - 40) {
            if (command_x != 0) ++stop_tail_command_frames;
            if (std::fabs(result.target.lead_x) > 0.25f) {
                ++stop_tail_lead_frames;
            }
        }
        previous_lead_active = result.target.lead_active;
    }

    std::sort(moving_base_offsets.begin(), moving_base_offsets.end());
    std::sort(
        moving_forecast_distances.begin(), moving_forecast_distances.end());
    std::sort(moving_forecast_steps.begin(), moving_forecast_steps.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        const std::size_t index = std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction));
        return values[index];
    };
    const float base_offset_p50 = percentile(moving_base_offsets, 0.50f);
    const float forecast_p50 =
        percentile(moving_forecast_distances, 0.50f);
    const float forecast_step_p95 =
        percentile(moving_forecast_steps, 0.95f);

    expect(moving_lead_frames >= 570 && moving_lead_edges <= 2,
           "持续 MoveLeft 中 prediction 不得在追上后释放并重新进入，活动帧=" +
               std::to_string(moving_lead_frames) + "，边沿=" +
               std::to_string(moving_lead_edges));
    expect(pullback_command_frames == 0 && moving_direction_switches == 0,
           "持续 MoveLeft 中不得退回基础点并产生向右拉回，拉回帧=" +
               std::to_string(pullback_command_frames) + "，换向=" +
               std::to_string(moving_direction_switches));
    expect(base_offset_p50 >= 0.75f && forecast_p50 >= 4.0f,
           "预测最终点必须覆盖控制稳态误差，使准星实际位于 MoveLeft 基础点前方，基础点相对准星 P50=" +
               std::to_string(base_offset_p50) + "，最终点前探 P50=" +
               std::to_string(forecast_p50));
    expect(forecast_step_p95 <= 0.75f,
           "人物框动画下 prediction 前探不得逐帧晃动，前探阶跃 P95=" +
               std::to_string(forecast_step_p95));
    expect(stop_tail_command_frames == 0 && stop_tail_lead_frames == 0,
           "人物真实停止后水平 prediction 和命令必须在尾部持续归零，命令帧=" +
               std::to_string(stop_tail_command_frames) + "，预测帧=" +
               std::to_string(stop_tail_lead_frames));
}

void test_horizontal_prediction_does_not_block_vertical_height_correction() {
    constexpr int kActuationDelayFrames = 4;
    constexpr int kFrameCount = 1200;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands_x{};
    std::array<int, kActuationDelayFrames> delayed_commands_y{};
    float world_target_x = -12.0f;
    constexpr float kWorldTargetYOffset = 25.0f;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    int far_vertical_stop_frames = 0;
    int medium_vertical_stop_frames = 0;
    int maximum_far_vertical_stop_run = 0;
    int current_far_vertical_stop_run = 0;
    int vertical_prediction_frames = 0;
    int all_vertical_prediction_frames = 0;
    float maximum_vertical_prediction_offset = 0.0f;
    float maximum_all_vertical_prediction_offset = 0.0f;
    std::string first_vertical_prediction_trace;
    std::string first_medium_stop_trace;
    std::vector<float> settled_vertical_errors;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands_x[delay_slot] /
            config.counts_per_pixel_x * 0.18f;
        camera_y += delayed_commands_y[delay_slot] /
            config.counts_per_pixel_y * 0.18f;
        delayed_commands_x[delay_slot] = 0;
        delayed_commands_y[delay_slot] = 0;
        world_target_x -= 0.65f;

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        // body_aim_height_ratio=0.35 时，80 px 身体框的瞄点比框中心高
        // 12 px。这里显式补回该偏移，使真实指定高度只由相机闭环改变。
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x,
            172.0f + kWorldTargetYOffset - camera_y,
            40.0f, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "二维 prediction 高度闭环必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        const int command_y = result.has_command
            ? result.command.dy_counts : 0;
        if (result.has_command) {
            delayed_commands_x[delay_slot] = command_x;
            delayed_commands_y[delay_slot] = command_y;
        }
        const float all_vertical_prediction_offset =
            std::fabs(result.target.lead_y);
        maximum_all_vertical_prediction_offset = std::max(
            maximum_all_vertical_prediction_offset,
            all_vertical_prediction_offset);
        if (all_vertical_prediction_offset > 0.25f) {
            ++all_vertical_prediction_frames;
            if (first_vertical_prediction_trace.empty()) {
                first_vertical_prediction_trace =
                    "帧=" + std::to_string(index) +
                    "，base_y=" + std::to_string(
                        result.target.base_aim_y) +
                    "，prediction_y=" + std::to_string(
                        result.target.prediction_aim_y) +
                    "，lead_y=" + std::to_string(result.target.lead_y);
            }
        }

        if (index >= 400) {
            const float vertical_error =
                std::fabs(result.target.base_aim_y - frame.control_center_y);
            const float vertical_prediction_offset =
                std::fabs(result.target.lead_y);
            settled_vertical_errors.push_back(vertical_error);
            maximum_vertical_prediction_offset = std::max(
                maximum_vertical_prediction_offset,
                vertical_prediction_offset);
            if (vertical_prediction_offset > 0.25f) {
                ++vertical_prediction_frames;
            }
            if (vertical_error > 3.0f && command_y == 0) {
                ++medium_vertical_stop_frames;
                if (first_medium_stop_trace.empty()) {
                    first_medium_stop_trace =
                        "帧=" + std::to_string(index) +
                        "，base_y=" +
                        std::to_string(result.target.base_aim_y) +
                        "，delay_y=" + std::to_string(
                            result.target.delay_compensated_aim_y) +
                        "，final_y=" +
                        std::to_string(result.target.aim_y) +
                        "，lead_y=" +
                        std::to_string(result.target.lead_y);
                }
            }
            if (vertical_error > 8.0f && command_y == 0) {
                ++far_vertical_stop_frames;
                ++current_far_vertical_stop_run;
                maximum_far_vertical_stop_run = std::max(
                    maximum_far_vertical_stop_run,
                    current_far_vertical_stop_run);
            } else {
                current_far_vertical_stop_run = 0;
            }
        }
    }

    std::sort(settled_vertical_errors.begin(),
              settled_vertical_errors.end());
    const std::size_t p95_index = std::min(
        settled_vertical_errors.size() - 1,
        static_cast<std::size_t>(settled_vertical_errors.size() * 0.95f));
    const float vertical_error_p95 = settled_vertical_errors[p95_index];
    expect(vertical_error_p95 <= 5.0f && far_vertical_stop_frames == 0 &&
               medium_vertical_stop_frames == 0 &&
               maximum_far_vertical_stop_run == 0 &&
               vertical_prediction_frames == 0 &&
               maximum_all_vertical_prediction_offset <= 2.0f,
           "水平 prediction 不得阻断 Y 轴回到配置高度，误差 P95=" +
               std::to_string(vertical_error_p95) + "，远距停发帧=" +
               std::to_string(far_vertical_stop_frames) + "，最长连续=" +
               std::to_string(maximum_far_vertical_stop_run) +
               "，中距停发帧=" +
               std::to_string(medium_vertical_stop_frames) +
               "，Y prediction 帧=" +
               std::to_string(vertical_prediction_frames) +
               "，最大 Y prediction 偏移=" +
               std::to_string(maximum_vertical_prediction_offset) +
               "，全程 Y prediction 帧=" +
               std::to_string(all_vertical_prediction_frames) +
               "，全程最大 Y prediction 偏移=" +
               std::to_string(maximum_all_vertical_prediction_offset) +
               "，首个 Y prediction=" + first_vertical_prediction_trace +
               "，首个中距停发=" + first_medium_stop_trace);
}

void test_vertical_pullback_hold_releases_while_horizontal_prediction_continues() {
    constexpr int kActuationDelayFrames = 4;
    constexpr int kVerticalMovingFrames = 520;
    constexpr int kFrameCount = 1400;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands_x{};
    std::array<int, kActuationDelayFrames> delayed_commands_y{};
    float world_target_x = -12.0f;
    float world_target_y = 24.0f;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    int moving_vertical_lead_frames = 0;
    int early_vertical_correction_frames = 0;
    int late_vertical_stop_frames = 0;
    int late_vertical_correction_frames = 0;
    std::vector<float> late_vertical_errors;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands_x[delay_slot] /
            config.counts_per_pixel_x * 0.18f;
        camera_y += delayed_commands_y[delay_slot] /
            config.counts_per_pixel_y * 0.18f;
        delayed_commands_x[delay_slot] = 0;
        delayed_commands_y[delay_slot] = 0;
        world_target_x -= 0.65f;
        if (index < kVerticalMovingFrames) {
            world_target_y -= 0.80f;
        } else if (index == kVerticalMovingFrames) {
            // 目标停止时叠加一次向旧 prediction 反侧的姿态/观测偏移，
            // 模拟真机归位阶段把基础 Y 点推离准星的窗口。
            world_target_y += 20.0f;
        }

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x,
            172.0f + world_target_y - camera_y,
            40.0f, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "逐轴反拉释放回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        const int command_y = result.has_command
            ? result.command.dy_counts : 0;
        if (result.has_command) {
            delayed_commands_x[delay_slot] = command_x;
            delayed_commands_y[delay_slot] = command_y;
        }
        if (index >= 300 && index < kVerticalMovingFrames &&
            result.target.lead_active && result.target.lead_y < -0.25f) {
            ++moving_vertical_lead_frames;
        }
        // 旧 Y 门禁会无条件服从历史向上 prediction，必须等满 300 ms
        // 超时后才允许向下恢复高度。当前世界测量仍支持历史方向时，向下
        // 命令实际是在追当前最终点，应与 X 轴一样立即因果放行。
        if (index > kVerticalMovingFrames &&
            index <= kVerticalMovingFrames + 60) {
            const float vertical_error =
                result.target.base_aim_y - frame.control_center_y;
            if (vertical_error > 3.0f && command_y > 0) {
                ++early_vertical_correction_frames;
            }
        }
        if (index >= kVerticalMovingFrames + 72) {
            const float vertical_error =
                std::fabs(result.target.base_aim_y - frame.control_center_y);
            if (vertical_error > 3.0f && command_y > 0) {
                ++late_vertical_correction_frames;
            }
            if (index >= kVerticalMovingFrames + 180) {
                late_vertical_errors.push_back(vertical_error);
                if (vertical_error > 3.0f && command_y == 0) {
                    ++late_vertical_stop_frames;
                }
            }
        }
    }

    std::sort(late_vertical_errors.begin(), late_vertical_errors.end());
    const std::size_t p95_index = std::min(
        late_vertical_errors.size() - 1,
        static_cast<std::size_t>(late_vertical_errors.size() * 0.95f));
    const float late_vertical_error_p95 = late_vertical_errors[p95_index];
    expect(moving_vertical_lead_frames >= 60,
           "逐轴反拉释放回归必须先建立真实 Y prediction，活动帧=" +
               std::to_string(moving_vertical_lead_frames));
    expect(early_vertical_correction_frames > 0 &&
               late_vertical_stop_frames == 0 &&
               late_vertical_correction_frames > 0 &&
               late_vertical_error_p95 <= 5.0f,
           "Y 运动停止后不得因 X prediction 继续活动而永久停发，高度误差 P95=" +
               std::to_string(late_vertical_error_p95) +
               "，超时前纠正帧=" +
               std::to_string(early_vertical_correction_frames) +
               "，停发帧=" + std::to_string(late_vertical_stop_frames) +
               "，纠正帧=" +
               std::to_string(late_vertical_correction_frames));
}

void test_prediction_pullback_timeout_has_no_absolute_velocity_mode() {
    constexpr int kActuationDelayFrames = 4;
    constexpr int kVerticalMovingFrames = 520;
    constexpr int kFrameCount = 1400;
    struct Trace {
        int moving_vertical_lead_frames = 0;
        int late_vertical_stop_frames = 0;
        int late_vertical_correction_frames = 0;
    };

    const auto run = [&]() {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.0f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = 0.40f;
        // 该标定把公开几何轨迹换算到旧 60 counts/s 资格的高侧；
        // 测试仍只观察 Aim::process() 返回的目标和命令。
        config.counts_per_pixel_y = 4.00f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 100.0f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 16.0f;
        config.max_delay_compensation_percent = 15.0f;
        config.enable_prediction = true;
        Aim aim(config);

        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        std::array<int, kActuationDelayFrames> delayed_commands_x{};
        std::array<int, kActuationDelayFrames> delayed_commands_y{};
        float world_target_x = -12.0f;
        float world_target_y = 24.0f;
        float camera_x = 0.0f;
        float camera_y = 0.0f;
        Trace trace;

        for (int index = 0; index < kFrameCount; ++index) {
            const int delay_slot = index % kActuationDelayFrames;
            camera_x += delayed_commands_x[delay_slot] /
                config.counts_per_pixel_x * 0.18f;
            camera_y += delayed_commands_y[delay_slot] /
                config.counts_per_pixel_y * 0.18f;
            delayed_commands_x[delay_slot] = 0;
            delayed_commands_y[delay_slot] = 0;
            world_target_x -= 0.65f;
            if (index < kVerticalMovingFrames) {
                world_target_y -= 1.0f / 12.0f;
            } else {
                world_target_y += 0.25f;
            }

            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(
                        index * 1000000.0f / 240.0f)));
            frame.control_at =
                frame.captured_at + std::chrono::milliseconds(1);
            frame.lock_active = true;
            frame.detections = {body_box(
                160.0f + world_target_x - camera_x,
                172.0f + world_target_y - camera_y,
                40.0f, 80.0f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "Y pullback 无速度模式回归必须逐帧保留合法目标");
            if (!result.has_target) continue;

            const int command_x = result.has_command
                ? result.command.dx_counts : 0;
            const int command_y = result.has_command
                ? result.command.dy_counts : 0;
            if (result.has_command) {
                delayed_commands_x[delay_slot] = command_x;
                delayed_commands_y[delay_slot] = command_y;
            }
            if (index >= 300 && index < kVerticalMovingFrames &&
                result.target.lead_active && result.target.lead_y < -0.25f) {
                ++trace.moving_vertical_lead_frames;
            }
            if (index >= kVerticalMovingFrames + 180) {
                const float vertical_error =
                    std::fabs(result.target.base_aim_y -
                              frame.control_center_y);
                if (vertical_error > 3.0f && command_y == 0) {
                    ++trace.late_vertical_stop_frames;
                }
                if (vertical_error > 3.0f && command_y > 0) {
                    ++trace.late_vertical_correction_frames;
                }
            }
        }
        return trace;
    };

    const Trace trace = run();
    expect(trace.moving_vertical_lead_frames >= 60,
           "固定 320 ROI/真实时序轨迹必须先建立 Y prediction，活动帧=" +
               std::to_string(trace.moving_vertical_lead_frames));
    expect(trace.late_vertical_stop_frames == 0 &&
               trace.late_vertical_correction_frames > 0,
           "同一 Y hold 超过 300 ms 后不得再用固定 counts/s 阻止释放，停发/纠正帧=" +
               std::to_string(trace.late_vertical_stop_frames) + "/" +
               std::to_string(trace.late_vertical_correction_frames));
}

void test_horizontal_prediction_rejects_delayed_vertical_camera_feedback() {
    constexpr int kActuationDelayFrames = 10;
    constexpr int kFrameCount = 1600;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands_x{};
    std::array<int, kActuationDelayFrames> delayed_commands_y{};
    float world_target_x = -12.0f;
    constexpr float kWorldTargetYOffset = 38.0f;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    int vertical_prediction_frames = 0;
    float maximum_vertical_prediction_offset = 0.0f;
    std::vector<float> settled_vertical_errors;
    std::vector<float> horizontal_prediction_offsets;
    std::vector<float> horizontal_prediction_second_differences;
    std::vector<float> legacy_control_second_differences;
    std::vector<float> coherent_control_second_differences;
    float previous_horizontal_prediction_offset = 0.0f;
    float previous_previous_horizontal_prediction_offset = 0.0f;
    float legacy_control_offset = 0.0f;
    float coherent_control_offset = 0.0f;
    float previous_legacy_control_target = 0.0f;
    float previous_previous_legacy_control_target = 0.0f;
    float previous_coherent_control_target = 0.0f;
    float previous_previous_coherent_control_target = 0.0f;
    int horizontal_prediction_samples = 0;
    int horizontal_control_samples = 0;
    int horizontal_command_state_changes = 0;
    bool previous_horizontal_command_active = false;
    int current_horizontal_stop_frames = 0;
    int longest_horizontal_stop_frames = 0;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands_x[delay_slot] /
            config.counts_per_pixel_x * 0.15f;
        camera_y += delayed_commands_y[delay_slot] /
            config.counts_per_pixel_y * 0.15f;
        delayed_commands_x[delay_slot] = 0;
        delayed_commands_y[delay_slot] = 0;
        world_target_x -= 0.65f;

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x,
            172.0f + kWorldTargetYOffset - camera_y,
            40.0f, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "长延迟二维反馈回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        const int command_y = result.has_command
            ? result.command.dy_counts : 0;
        if (result.has_command) {
            delayed_commands_x[delay_slot] = command_x;
            delayed_commands_y[delay_slot] = command_y;
        }
        const float prediction_alpha = result.target.lead_active
            ? 0.35f : 0.12f;
        const float legacy_control_target_offset =
            result.target.aim_x - result.target.delay_compensated_aim_x;
        legacy_control_offset +=
            (legacy_control_target_offset - legacy_control_offset) *
            prediction_alpha;
        const float legacy_control_target =
            result.target.delay_compensated_aim_x + legacy_control_offset;
        const float coherent_control_target_offset =
            result.target.aim_x - result.target.base_aim_x;
        coherent_control_offset +=
            (coherent_control_target_offset - coherent_control_offset) *
            prediction_alpha;
        const float coherent_control_target =
            result.target.base_aim_x + coherent_control_offset;
        if (index >= 500) {
            const float horizontal_prediction_offset =
                result.target.aim_x - result.target.base_aim_x;
            horizontal_prediction_offsets.push_back(
                std::fabs(horizontal_prediction_offset));
            if (horizontal_prediction_samples >= 2) {
                horizontal_prediction_second_differences.push_back(std::fabs(
                    horizontal_prediction_offset -
                    2.0f * previous_horizontal_prediction_offset +
                    previous_previous_horizontal_prediction_offset));
            }
            previous_previous_horizontal_prediction_offset =
                previous_horizontal_prediction_offset;
            previous_horizontal_prediction_offset =
                horizontal_prediction_offset;
            ++horizontal_prediction_samples;
            if (horizontal_control_samples >= 2) {
                legacy_control_second_differences.push_back(std::fabs(
                    legacy_control_target -
                    2.0f * previous_legacy_control_target +
                    previous_previous_legacy_control_target));
                coherent_control_second_differences.push_back(std::fabs(
                    coherent_control_target -
                    2.0f * previous_coherent_control_target +
                    previous_previous_coherent_control_target));
            }
            previous_previous_legacy_control_target =
                previous_legacy_control_target;
            previous_legacy_control_target = legacy_control_target;
            previous_previous_coherent_control_target =
                previous_coherent_control_target;
            previous_coherent_control_target = coherent_control_target;
            ++horizontal_control_samples;
            const bool horizontal_command_active = command_x != 0;
            if (horizontal_command_active !=
                previous_horizontal_command_active) {
                ++horizontal_command_state_changes;
            }
            if (horizontal_command_active) {
                current_horizontal_stop_frames = 0;
            } else {
                ++current_horizontal_stop_frames;
                longest_horizontal_stop_frames = std::max(
                    longest_horizontal_stop_frames,
                    current_horizontal_stop_frames);
            }
            previous_horizontal_command_active = horizontal_command_active;
            const float vertical_error = std::fabs(
                result.target.base_aim_y - frame.control_center_y);
            const float vertical_prediction_offset =
                std::fabs(result.target.lead_y);
            settled_vertical_errors.push_back(vertical_error);
            maximum_vertical_prediction_offset = std::max(
                maximum_vertical_prediction_offset,
                vertical_prediction_offset);
            if (vertical_prediction_offset > 0.25f) {
                ++vertical_prediction_frames;
            }
        }
    }

    std::sort(settled_vertical_errors.begin(),
              settled_vertical_errors.end());
    const std::size_t p95_index = std::min(
        settled_vertical_errors.size() - 1,
        static_cast<std::size_t>(settled_vertical_errors.size() * 0.95f));
    const float vertical_error_p95 = settled_vertical_errors[p95_index];
    std::sort(horizontal_prediction_offsets.begin(),
              horizontal_prediction_offsets.end());
    std::sort(horizontal_prediction_second_differences.begin(),
              horizontal_prediction_second_differences.end());
    std::sort(legacy_control_second_differences.begin(),
              legacy_control_second_differences.end());
    std::sort(coherent_control_second_differences.begin(),
              coherent_control_second_differences.end());
    const float horizontal_offset_p95 = horizontal_prediction_offsets[
        std::min(
            horizontal_prediction_offsets.size() - 1,
            static_cast<std::size_t>(
                horizontal_prediction_offsets.size() * 0.95f))];
    const float horizontal_second_difference_p95 =
        horizontal_prediction_second_differences[
            std::min(
                horizontal_prediction_second_differences.size() - 1,
                static_cast<std::size_t>(
                    horizontal_prediction_second_differences.size() *
                    0.95f))];
    const auto p95 = [](const std::vector<float>& values) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * 0.95f))];
    };
    const float legacy_control_second_difference_p95 =
        p95(legacy_control_second_differences);
    const float coherent_control_second_difference_p95 =
        p95(coherent_control_second_differences);
    expect(vertical_error_p95 <= 6.0f &&
               vertical_prediction_frames == 0 &&
               maximum_vertical_prediction_offset <= 0.25f,
           "10 帧相机反馈不得自激建立 Y prediction，高度误差 P95=" +
               std::to_string(vertical_error_p95) +
               "，Y prediction 帧=" +
               std::to_string(vertical_prediction_frames) +
               "，最大 Y prediction 偏移=" +
               std::to_string(maximum_vertical_prediction_offset));
    const float maximum_world_forecast = std::hypot(40.0f, 80.0f) *
        config.max_delay_compensation_percent / 100.0f * 1.5f;
    expect(horizontal_offset_p95 <= maximum_world_forecast + 0.01f &&
               horizontal_second_difference_p95 <= 0.30f,
           "40 ms 控制延迟不得同步放大额外 prediction 时域，X 独立偏移 P95=" +
               std::to_string(horizontal_offset_p95) +
               "，二阶 P95=" +
               std::to_string(horizontal_second_difference_p95));
    expect(coherent_control_second_difference_p95 <= 0.30f &&
               coherent_control_second_difference_p95 <=
                   legacy_control_second_difference_p95 * 0.70f &&
               horizontal_command_state_changes <= 65 &&
               longest_horizontal_stop_frames <= 8,
           "10 帧反馈下必须统一处理延迟与反向 lead，旧/统一控制目标二阶 P95=" +
               std::to_string(legacy_control_second_difference_p95) + "/" +
               std::to_string(coherent_control_second_difference_p95) +
               "，命令启停切换=" +
               std::to_string(horizontal_command_state_changes) +
               "，最长停发=" +
               std::to_string(longest_horizontal_stop_frames));
}

void test_long_delay_prediction_distributes_horizontal_hold_command() {
    constexpr int kActuationDelayFrames = 10;
    constexpr int kFrameCount = 1800;
    constexpr int kSettledFrame = 600;
    // Run 20260811-023032 的命令反馈回归得到约 0.433 px/count、
    // -0.97 px/frame 世界位移。闭环平均只需约 -2.24 counts/frame；
    // 若观察器跟随延迟后的 -4~-6 counts 脉冲，就会形成 18~24 帧停发。
    constexpr float kCameraResponse = 0.173f;
    constexpr float kWorldStep = -0.97f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = -12.0f;
    float camera_x = 0.0f;
    int active_frames = 0;
    int current_stop_frames = 0;
    int longest_stop_frames = 0;
    int nonzero_frames = 0;
    int public_direction_violations = 0;
    float command_sum = 0.0f;
    float nonzero_command_sum = 0.0f;
    float public_error_abs_sum = 0.0f;
    int public_error_samples = 0;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * kCameraResponse;
        delayed_commands[delay_slot] = 0;
        world_target_x += kWorldStep;

        const float animation_x = 0.35f * std::sin(index * 0.73f);
        const float animation_width =
            40.0f + 3.0f * std::sin(index * 0.51f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x + animation_x,
            160.0f, animation_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "实测长延迟 prediction 回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[delay_slot] = command_x;
        }
        if (index < kSettledFrame || !result.target.lead_active) continue;

        ++active_frames;
        command_sum += static_cast<float>(command_x);
        const float public_error_x =
            result.target.aim_x - frame.control_center_x;
        public_error_abs_sum += std::fabs(public_error_x);
        ++public_error_samples;
        const float hold_band = std::max(
            2.0f, config.deadzone_pixels * 1.5f);
        if (std::fabs(public_error_x) > hold_band &&
            command_x * public_error_x <= 0.0f) {
            ++public_direction_violations;
        }
        if (command_x == 0) {
            ++current_stop_frames;
            longest_stop_frames = std::max(
                longest_stop_frames, current_stop_frames);
        } else {
            current_stop_frames = 0;
            ++nonzero_frames;
            nonzero_command_sum += static_cast<float>(command_x);
        }
    }

    expect(active_frames >= 1000 && nonzero_frames > 0,
           "实测长延迟 prediction 回归必须覆盖稳定活动窗口");
    const float mean_command = command_sum /
        static_cast<float>(active_frames);
    const float mean_nonzero_command = nonzero_command_sum /
        static_cast<float>(nonzero_frames);
    const float mean_public_error = public_error_abs_sum /
        static_cast<float>(public_error_samples);
    expect(longest_stop_frames <= 8 &&
               mean_command < -1.75f && mean_command > -2.75f &&
               mean_nonzero_command > -3.25f &&
               mean_public_error <= 3.0f &&
               public_direction_violations == 0,
           "长延迟世界维持量不得退化为高幅脉冲和长停发，最长停发=" +
               std::to_string(longest_stop_frames) +
               "，平均命令=" + std::to_string(mean_command) +
               "，非零平均=" + std::to_string(mean_nonzero_command) +
               "，公开最终点平均误差=" +
               std::to_string(mean_public_error) +
               "，公开最终点方向违规=" +
               std::to_string(public_direction_violations));
}

void test_real_cadence_prediction_closes_public_point_error() {
    constexpr int kActuationDelayFrames = 3;
    constexpr int kFrameCount = 900;
    constexpr int kSettledFrame = 300;
    // Run 20260811-145413 的 5347 个样本覆盖 78.17 秒，真实控制节奏约
    // 68.4 Hz；40 ms 在该节奏下约为 3 帧，而不是 240 Hz 回归的 10 帧。
    constexpr int kFrameIntervalMicroseconds = 14620;
    constexpr float kCameraResponse = 0.173f;
    constexpr float kWorldStep = -0.97f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = -12.0f;
    float camera_x = 0.0f;
    int active_frames = 0;
    int current_stop_frames = 0;
    int longest_stop_frames = 0;
    int direction_violations = 0;
    float public_error_abs_sum = 0.0f;
    float command_sum = 0.0f;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * kCameraResponse;
        delayed_commands[delay_slot] = 0;
        world_target_x += kWorldStep;

        const float animation_x = 0.35f * std::sin(index * 0.73f);
        const float animation_width =
            40.0f + 3.0f * std::sin(index * 0.51f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index) *
                kFrameIntervalMicroseconds));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x + animation_x,
            160.0f, animation_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "真实采样节奏 prediction 回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[delay_slot] = command_x;
        }
        if (index < kSettledFrame || !result.target.lead_active) continue;

        ++active_frames;
        command_sum += static_cast<float>(command_x);
        const float public_error_x =
            result.target.aim_x - frame.control_center_x;
        public_error_abs_sum += std::fabs(public_error_x);
        const float hold_band = std::max(
            2.0f, config.deadzone_pixels * 1.5f);
        if (std::fabs(public_error_x) > hold_band &&
            command_x * public_error_x <= 0.0f) {
            ++direction_violations;
        }
        if (command_x == 0) {
            ++current_stop_frames;
            longest_stop_frames = std::max(
                longest_stop_frames, current_stop_frames);
        } else {
            current_stop_frames = 0;
        }
    }

    expect(active_frames >= 500,
           "真实采样节奏回归必须覆盖稳定 prediction 活动窗口");
    const float mean_public_error = public_error_abs_sum /
        static_cast<float>(active_frames);
    const float mean_command = command_sum /
        static_cast<float>(active_frames);
    expect(mean_public_error <= 1.20f &&
               mean_command < -1.75f && mean_command > -2.75f &&
               longest_stop_frames <= 8 &&
               direction_violations == 0,
           "真实 68.4 Hz/40 ms 闭环必须贴合公开 prediction 点，平均误差=" +
               std::to_string(mean_public_error) +
               "，平均命令=" + std::to_string(mean_command) +
               "，最长停发=" + std::to_string(longest_stop_frames) +
               "，方向违规=" + std::to_string(direction_violations));
}

void test_variable_real_cadence_prediction_closes_public_point_error() {
    constexpr int kActuationDelayFrames = 4;
    constexpr int kFrameCount = 900;
    constexpr int kSettledFrame = 300;
    // 最新真实 Run 约 119 Hz，单帧间隔会跨越 8 ms 门槛。固定世界速度
    // 下交替 7.2～10.4 ms，验证低频同源前馈的有限幅度校正在边界节奏下
    // 仍能收敛，且不引入停发或错向命令。均值 8.4 ms 同时覆盖上一轮
    // 107.1 Hz 边界。
    constexpr std::array<int, 8> kFrameIntervalsMicroseconds{
        7200, 10400, 7600, 8400, 7900, 9000, 7500, 9200};
    constexpr float kWorldVelocityPixelsPerSecond = -96.43f;
    constexpr float kCameraResponse = 0.173f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    std::chrono::microseconds elapsed{};
    float world_target_x = -12.0f;
    float camera_x = 0.0f;
    int active_frames = 0;
    int current_stop_frames = 0;
    int longest_stop_frames = 0;
    int direction_violations = 0;
    float public_error_abs_sum = 0.0f;
    float command_sum = 0.0f;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * kCameraResponse;
        delayed_commands[delay_slot] = 0;
        const int interval_us = kFrameIntervalsMicroseconds[
            static_cast<std::size_t>(index) %
            kFrameIntervalsMicroseconds.size()];
        if (index > 0) elapsed += std::chrono::microseconds(interval_us);
        world_target_x += kWorldVelocityPixelsPerSecond *
            static_cast<float>(interval_us) / 1000000.0f;

        const float animation_x = 0.35f * std::sin(index * 0.73f);
        const float animation_width =
            40.0f + 3.0f * std::sin(index * 0.51f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + elapsed);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x + animation_x,
            160.0f, animation_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "119 Hz 变周期 prediction 回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[delay_slot] = command_x;
        }
        if (index < kSettledFrame || !result.target.lead_active) continue;

        ++active_frames;
        command_sum += static_cast<float>(command_x);
        const float public_error_x =
            result.target.aim_x - frame.control_center_x;
        public_error_abs_sum += std::fabs(public_error_x);
        const float hold_band = std::max(
            2.0f, config.deadzone_pixels * 1.5f);
        if (std::fabs(public_error_x) > hold_band &&
            command_x * public_error_x <= 0.0f) {
            ++direction_violations;
        }
        if (command_x == 0) {
            ++current_stop_frames;
            longest_stop_frames = std::max(
                longest_stop_frames, current_stop_frames);
        } else {
            current_stop_frames = 0;
        }
    }

    expect(active_frames >= 500,
           "119 Hz 变周期回归必须覆盖稳定 prediction 活动窗口");
    const float mean_public_error = public_error_abs_sum /
        static_cast<float>(active_frames);
    const float mean_command = command_sum /
        static_cast<float>(active_frames);
    expect(mean_public_error <= 0.60f &&
               mean_command < -1.45f && mean_command > -2.35f &&
               longest_stop_frames <= 8 &&
               direction_violations == 0,
           "真实 119 Hz 变周期/40 ms 闭环必须贴合公开 prediction 点，平均误差=" +
               std::to_string(mean_public_error) +
               "，平均命令=" + std::to_string(mean_command) +
               "，最长停发=" + std::to_string(longest_stop_frames) +
               "，方向违规=" + std::to_string(direction_violations));
}

void test_prediction_lead_is_stable_across_bursty_frame_intervals() {
    constexpr std::array<int, 8> kFrameIntervalsMicroseconds{
        4167, 4167, 1000, 7334, 4167, 1500, 6834, 4167};
    constexpr int kActuationDelayFrames = 4;
    constexpr float kWorldVelocityPixelsPerSecond = -600.0f;
    constexpr float kCameraResponse = 0.18f;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    std::chrono::microseconds elapsed{};
    float world_target_x = -12.0f;
    float camera_x = 0.0f;
    float previous_lead_x = 0.0f;
    bool previous_lead_valid = false;
    int active_frames = 0;
    int base_outside_frames = 0;
    int final_outside_frames = 0;
    std::vector<float> lead_distances;
    std::vector<float> lead_steps;

    for (int index = 0; index < 1200; ++index) {
        const int interval_us = kFrameIntervalsMicroseconds[
            static_cast<std::size_t>(index) %
            kFrameIntervalsMicroseconds.size()];
        elapsed += std::chrono::microseconds(interval_us);
        const float delta_seconds =
            static_cast<float>(interval_us) / 1000000.0f;
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * kCameraResponse;
        delayed_commands[delay_slot] = 0;
        world_target_x += kWorldVelocityPixelsPerSecond * delta_seconds;

        const float animation_x = 0.35f * std::sin(index * 0.73f);
        const float animation_width =
            40.0f + 3.0f * std::sin(index * 0.51f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), base + elapsed);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + world_target_x - camera_x + animation_x,
            160.0f, animation_width, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "突发帧间隔 prediction 回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        if (result.has_command) {
            delayed_commands[delay_slot] = result.command.dx_counts;
        }
        const bool base_inside =
            result.target.base_aim_x >= result.target.x1 &&
            result.target.base_aim_x <= result.target.x2 &&
            result.target.base_aim_y >= result.target.y1 &&
            result.target.base_aim_y <= result.target.y2;
        if (!base_inside) ++base_outside_frames;
        if (result.target.aim_x < result.target.x1 ||
            result.target.aim_x > result.target.x2 ||
            result.target.aim_y < result.target.y1 ||
            result.target.aim_y > result.target.y2) {
            ++final_outside_frames;
        }
        if (index >= 500 && result.target.lead_active) {
            ++active_frames;
            const float prediction_offset_x = result.target.aim_x -
                result.target.base_aim_x;
            // 最终预测点是用户实际观察且控制器实际消费的量。延迟抵消属于
            // 合成过程，回归必须直接限制最终点相对基础点的独立偏移，而不能
            // 只验证某个中间世界前探量。
            lead_distances.push_back(-prediction_offset_x);
            if (previous_lead_valid) {
                lead_steps.push_back(std::fabs(
                    prediction_offset_x - previous_lead_x));
            }
            previous_lead_x = prediction_offset_x;
            previous_lead_valid = true;
        } else {
            previous_lead_valid = false;
        }
    }

    std::sort(lead_distances.begin(), lead_distances.end());
    std::sort(lead_steps.begin(), lead_steps.end());
    const auto percentile = [](const std::vector<float>& values,
                               float fraction) {
        if (values.empty()) return 0.0f;
        const std::size_t index = std::min(
            values.size() - 1,
            static_cast<std::size_t>(values.size() * fraction));
        return values[index];
    };
    const float lead_p50 = percentile(lead_distances, 0.50f);
    const float lead_step_p95 = percentile(lead_steps, 0.95f);
    const float lead_step_p99 = percentile(lead_steps, 0.99f);

    expect(active_frames >= 650 && lead_p50 >= 8.0f,
           "突发帧间隔下仍须保持可见 MoveLeft 提前量，活动帧=" +
               std::to_string(active_frames) + "，提前 P50=" +
               std::to_string(lead_p50));
    expect(lead_step_p95 <= 1.10f && lead_step_p99 <= 1.50f,
           "恒定世界速度不能因 NDI 突发帧间隔改变 prediction 距离，阶跃 P95/P99=" +
               std::to_string(lead_step_p95) + "/" +
               std::to_string(lead_step_p99));
    expect(base_outside_frames == 0,
           "突发帧间隔下基础点仍必须保持框内，基础出框=" +
               std::to_string(base_outside_frames) + "，预测出框=" +
               std::to_string(final_outside_frames));
}

void test_prediction_pullback_hold_releases_after_real_reversal() {
    constexpr int kActuationDelayFrames = 4;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    float world_target_x = 12.0f;
    float camera_x = 0.0f;
    int stop_pullback_commands = 0;
    int first_real_reverse_command = -1;
    int reverse_lead_frames = 0;

    for (int index = 0; index < 900; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * 0.18f;
        delayed_commands[delay_slot] = 0;
        if (index < 500) {
            world_target_x += 0.65f;
        } else if (index >= 580) {
            world_target_x -= 0.65f;
        }

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {
            body(160.0f + world_target_x - camera_x, 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "prediction 真实反向回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const int command_x = result.has_command
            ? result.command.dx_counts : 0;
        if (result.has_command) {
            delayed_commands[delay_slot] = command_x;
        }
        if (index >= 520 && index < 580 && command_x < 0) {
            ++stop_pullback_commands;
        }
        if (index >= 580 && command_x < 0 &&
            first_real_reverse_command < 0) {
            first_real_reverse_command = index;
        }
        if (index >= 650 && result.target.lead_active &&
            result.target.lead_x < -0.25f) {
            ++reverse_lead_frames;
        }
    }

    expect(stop_pullback_commands == 0,
           "目标只停止时不得把已形成的右向提前反拉，反向命令帧=" +
               std::to_string(stop_pullback_commands));
    expect(first_real_reverse_command >= 580 &&
               first_real_reverse_command <= 720,
           "目标真实反向并越过接管距离后必须解除逐轴停发保持，首次反向命令帧=" +
               std::to_string(first_real_reverse_command));
    expect(reverse_lead_frames >= 80,
           "真实反向稳定后 prediction 必须按新世界方向重新建立，反向提前帧=" +
               std::to_string(reverse_lead_frames));
}


void test_two_axis_command_reversal_passes_through_zero() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.45f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    int previous_x = 0;
    int previous_y = 0;
    int tested_frames = 0;
    for (int index = 0; index < 600; ++index) {
        const float x = 160.0f + 18.0f * std::sin(index * 0.37f) +
            6.0f * std::sin(index * 1.13f);
        const float y = 172.0f + 10.0f * std::sin(index * 0.23f);
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body(x, y)};
        const AimResult result = aim.process(frame);
        const int current_x = result.has_command
            ? result.command.dx_counts : 0;
        const int current_y = result.has_command
            ? result.command.dy_counts : 0;
        if (index > 20) {
            ++tested_frames;
            expect(previous_x * current_x >= 0,
                   "二维整形后的水平命令换向必须先经过零，前值=" +
                       std::to_string(previous_x) + "，当前=" +
                       std::to_string(current_x));
            expect(previous_y * current_y >= 0,
                   "二维整形后的垂直命令换向必须先经过零，前值=" +
                       std::to_string(previous_y) + "，当前=" +
                       std::to_string(current_y));
        }
        previous_x = current_x;
        previous_y = current_y;
    }
    expect(tested_frames > 0, "二维单轴反转回归必须实际处理稳定轨迹帧");
}

void test_integral_releases_on_reversal_and_static_settle() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.75f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.max_center_distance = 1.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float target_error = 18.0f;
    int first_reverse_command_frame = -1;
    int previous_settle_sign = 0;
    int settle_reversals = 0;

    for (int index = 0; index < 180; ++index) {
        if (index < 80) {
            target_error += 0.75f;
        } else if (index < 100) {
            target_error -= 2.0f;
        }
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.lock_active = true;
        frame.detections = {body(160.0f + target_error, 160.0f)};
        const AimResult result = aim.process(frame);
        if (!result.has_command) continue;
        target_error -= result.command.dx_counts /
            config.counts_per_pixel_x * 0.85f;
        if (index >= 80 && result.command.dx_counts < 0 &&
            first_reverse_command_frame < 0) {
            first_reverse_command_frame = index;
        }
        if (index >= 120) {
            const int sign = result.command.dx_counts < 0 ? -1 : 1;
            if (previous_settle_sign != 0 && sign != previous_settle_sign) {
                ++settle_reversals;
            }
            previous_settle_sign = sign;
        }
    }

    expect(first_reverse_command_frame >= 80 &&
               first_reverse_command_frame <= 86,
           "目标反向时积分必须立即释放，不能延迟反向命令");
    expect(std::fabs(target_error) <= 2.5f && settle_reversals <= 2,
           "静止归位后积分不得形成持续往返，误差=" +
               std::to_string(target_error) + "，反转=" +
               std::to_string(settle_reversals));
}

void test_quantization_residual_cannot_reverse_after_crossing() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 0.35f;
    config.counts_per_pixel_x = 0.50f;
    config.counts_per_pixel_y = 0.50f;
    config.max_counts_per_frame = 50.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    for (int index = 0; index < 50; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.detections = {body(120.0f + index * 2.0f, 160.0f)};
        const AimResult result = aim.process(frame);
        if (!result.has_command) continue;
        const float error_x = result.target.aim_x - frame.control_center_x;
        const float error_y = result.target.aim_y - frame.control_center_y;
        const float dot = result.command.dx_counts * error_x +
                          result.command.dy_counts * error_y;
        expect(dot > 0.0f || std::hypot(error_x, error_y) <=
                                  config.deadzone_pixels,
               "目标穿越准星后，上一方向的量化残余不得生成反向整数命令");
    }
}

void test_tracking_public_point_stays_on_base_feature() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.35f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.body_aim_height_ratio = 0.50f;
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    Aim aim(config);
    const auto is_base_only = [](const AimResult& result) {
        return result.has_target &&
            !result.target.delay_compensation_active &&
            std::fabs(result.target.delay_compensation_x) < 0.001f &&
            std::fabs(result.target.delay_compensation_y) < 0.001f &&
            std::fabs(result.target.aim_x - result.target.base_aim_x) <
                0.001f &&
            std::fabs(result.target.aim_y - result.target.base_aim_y) <
                0.001f &&
            result.target.aim_x >= result.target.x1 &&
            result.target.aim_x <= result.target.x2 &&
            result.target.aim_y >= result.target.y1 &&
            result.target.aim_y <= result.target.y2 &&
            std::fabs(result.control.modelled_response_x_counts) < 0.001f &&
            std::fabs(result.control.observer_phase_command_x_counts) <
                0.001f;
    };

    // 先建立向左的连续控制，再以约 100 px/s 向右回穿。该序列曾让
    // 闭环命令响应模型把公有瞄点推到基础点另一侧；现在逐帧验证追踪态
    // 只公开框内视觉特征，控制器内部状态不得改写目标几何。
    for (int index = 0; index < 40; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.lock_active = true;
        frame.detections = {body(140.0f - index * 0.5f, 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && is_base_only(result),
               "追踪基础特征回归的建立阶段必须逐帧保持框内基础点");
    }

    for (int index = 0; index < 19; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 41),
            base + std::chrono::milliseconds((index + 40) * 10));
        frame.lock_active = true;
        frame.detections = {body(140.0f + (index + 1), 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && is_base_only(result),
               "追踪基础特征回归的回穿阶段必须逐帧保持框内基础点");
    }

    AimFrame crossed = make_frame(60, base + std::chrono::milliseconds(590));
    crossed.lock_active = true;
    crossed.detections = {body(159.8f, 160.0f)};
    const AimResult result = aim.process(crossed);
    const float final_error = result.target.aim_x -
        crossed.control_center_x;
    const float hold_band = std::max(2.0f, config.deadzone_pixels * 1.5f);
    expect(result.status == AimStatus::SUCCESS && is_base_only(result),
           "追踪基础特征回归必须先建立有效框内目标");
    if (result.has_command && std::fabs(final_error) > hold_band) {
        expect(result.command.dx_counts * final_error >= 0.0f,
               "连续 PI 的整数命令不得背离框内基础瞄点");
    }

    // 基础视觉特征可以随观测短暂过线再返回；全过程的最终点必须严格
    // 等于基础点，不得由命令库存、响应模型或反向门另行改写。
    const std::array<float, 12> transient_positions{
        166.0f, 166.0f, 156.7f, 157.0f, 157.0f, 157.0f,
        157.0f, 157.0f, 157.0f, 157.0f, 157.0f, 157.0f};
    bool observed_base_overshoot = false;
    bool observed_returned_base = false;
    float maximum_transient_base_error = -1000.0f;
    float minimum_returned_base_error = 1000.0f;
    float returned_final_error = 0.0f;
    std::string transient_trace;
    for (std::size_t offset = 0; offset < transient_positions.size();
         ++offset) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(61 + offset),
            base + std::chrono::milliseconds(
                600 + static_cast<int>(offset) * 10));
        frame.lock_active = true;
        frame.detections = {body(transient_positions[offset], 160.0f)};
        const AimResult transient = aim.process(frame);
        expect(is_base_only(transient),
               "基础点过线瞬态仍必须保持追踪公有点与框内基础特征一致");
        const float transient_base_error = transient.target.base_aim_x -
            frame.control_center_x;
        const float transient_final_error =
            transient.target.delay_compensated_aim_x -
            frame.control_center_x;
        const float transient_final_error_y =
            transient.target.delay_compensated_aim_y -
            frame.control_center_y;
        if (transient.has_command && std::hypot(
                transient_final_error, transient_final_error_y) > hold_band) {
            expect(transient.command.dx_counts * transient_final_error +
                       transient.command.dy_counts *
                           transient_final_error_y > 0.0f,
                   "延迟基础维持的二维合成命令在保持带外必须朝向最终点");
        }
        maximum_transient_base_error = std::max(
            maximum_transient_base_error, transient_base_error);
        transient_trace += " [" + std::to_string(offset) + ":" +
            std::to_string(transient_base_error) + "," +
            std::to_string(transient_final_error) + "]";
        if (transient_base_error > hold_band) {
            observed_base_overshoot = true;
        }
        if (observed_base_overshoot && transient_base_error <
                minimum_returned_base_error) {
            minimum_returned_base_error = transient_base_error;
            returned_final_error = transient_final_error;
        }
        if (observed_base_overshoot && transient_base_error < -0.1f) {
            observed_returned_base = true;
            expect(std::fabs(
                       transient_final_error - transient_base_error) <
                       0.001f && is_base_only(transient),
                   "tracking X 基础点回到原侧时，公有瞄点必须仍等于"
                   "框内基础视觉特征");
            if (transient.has_command) {
                expect(transient.command.dx_counts * transient_final_error >= 0.0f,
                       "基础点过冲后回到原侧时，整数命令不得背离补偿最终点");
            }
            break;
        }
    }
    expect(observed_base_overshoot &&
               observed_returned_base,
           "回归必须覆盖基础点短暂越过保持带后回到原侧，基础最大=" +
               std::to_string(maximum_transient_base_error) +
               "，返回基础最小=" +
               std::to_string(minimum_returned_base_error) +
               "，对应投影=" + std::to_string(returned_final_error) +
               "，轨迹=" + transient_trace);
}

void test_control_step_cannot_cross_in_box_aim_point() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 1.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 100.0f;
    Aim aim(config);

    AimFrame frame = make_frame(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    frame.detections = {body_box(160.0f, 160.0f, 100.0f, 100.0f)};
    const AimResult result = aim.process(frame);
    const float reached_x = frame.control_center_x +
        result.command.dx_counts / config.counts_per_pixel_x;
    const float reached_y = frame.control_center_y +
        result.command.dy_counts / config.counts_per_pixel_y;
    expect(result.has_target &&
               result.target.aim_x >= result.target.x1 &&
               result.target.aim_x <= result.target.x2 &&
               result.target.aim_y >= result.target.y1 &&
               result.target.aim_y <= result.target.y2 &&
               reached_x >= result.target.x1 && reached_x <= result.target.x2 &&
               reached_y >= result.target.y1 && reached_y <= result.target.y2,
           "准星已在模型框内时，单帧控制不得把它推出选中框");
}

void test_tracking_and_prediction_projection_contracts_are_separate() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 1.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 100.0f;
    config.max_center_distance = 1.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 50.0f;
    AimConfig prediction_config = config;
    prediction_config.enable_prediction = true;
    Aim aim(config);
    Aim prediction(prediction_config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimResult result;
    AimResult prediction_result;
    for (int index = 0; index < 12; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(5);
        frame.lock_active = false;
        frame.detections = {body_box(
            140.0f + index * 2.0f, 120.0f + index,
            40.0f, 80.0f)};
        result = aim.process(frame);
        prediction_result = prediction.process(frame);
    }

    expect(result.status == AimStatus::SUCCESS && result.has_target &&
               !result.target.delay_compensation_active &&
               std::fabs(result.target.delay_compensation_ms_x) < 0.01f &&
               std::fabs(result.target.delay_compensation_ms_y) < 0.01f &&
               std::fabs(result.target.delay_compensation_ms) < 0.01f &&
               std::fabs(result.target.delay_compensation_x) < 0.001f &&
               std::fabs(result.target.delay_compensation_y) < 0.001f &&
               std::fabs(result.target.aim_x - result.target.base_aim_x) <
                   0.001f &&
               std::fabs(result.target.aim_y - result.target.base_aim_y) <
                   0.001f,
           "tracking 必须报告零投影时域并直接公开框内基础视觉特征");
    expect(prediction_result.has_target &&
               prediction_result.target.delay_compensation_active &&
               std::fabs(
                   prediction_result.target.delay_compensation_ms_x -
                   16.0f) < 0.01f &&
               std::fabs(
                   prediction_result.target.delay_compensation_ms_y -
                   16.0f) < 0.01f,
           "prediction 必须保留已验证的双轴 16 ms 几何时域");
}

void test_delay_compensation_stacks_before_prediction() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.0f;
    config.smoothing = 1.0f;
    config.counts_per_pixel_x = 1.0f;
    config.counts_per_pixel_y = 1.0f;
    config.max_counts_per_frame = 100.0f;
    config.max_center_distance = 1.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 10.0f;
    config.max_delay_compensation_ms = 20.0f;
    config.max_delay_compensation_percent = 50.0f;
    config.enable_prediction = true;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimResult result;
    int first_horizontal_prediction_frame = -1;
    // 未取得“相对运动与命令同向”的外部运动证据时，水平 prediction 从零
    // 建立仍需连续同向确认，但真实移动不应继续等待旧 250 ms 窗口。
    for (int index = 0; index < 40; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.control_at = frame.captured_at +
            std::chrono::milliseconds(5);
        frame.lock_active = true;
        frame.detections = {body(180.0f + index * 2.0f, 172.0f)};
        result = aim.process(frame);
        if (first_horizontal_prediction_frame < 0 && result.has_target &&
            result.target.lead_active && result.target.lead_x > 0.25f) {
            first_horizontal_prediction_frame = index;
        }
    }
    const float lead_distance = std::hypot(
        result.target.lead_x, result.target.lead_y);
    expect(result.has_target && result.target.delay_compensation_active &&
               result.target.lead_active && result.target.lead_x > 0.0f &&
               std::fabs(result.target.aim_x -
                          result.target.delay_compensated_aim_x -
                          result.target.lead_x) < 0.01f &&
               lead_distance >= 0.25f && lead_distance <= 5.0f,
            "prediction 必须从延迟补偿点按稳定世界运动速度生成有界前探，目标=" +
                std::to_string(result.has_target) + "，延迟=" +
                std::to_string(result.target.delay_compensation_active) +
                "，prediction=" +
                std::to_string(result.target.lead_active) + "，lead_x=" +
                std::to_string(result.target.lead_x) + "，距离=" +
                std::to_string(lead_distance) + "，基础=" +
                std::to_string(result.target.base_aim_x) + "，延迟点=" +
                std::to_string(result.target.delay_compensated_aim_x) +
                "，最终点=" + std::to_string(result.target.aim_x));
    expect(first_horizontal_prediction_frame >= 0 &&
               first_horizontal_prediction_frame <= 20,
           "持续水平移动必须在 200 ms 内建立可见 prediction，首帧=" +
               std::to_string(first_horizontal_prediction_frame));
}

void test_horizontal_prediction_startup_rejects_static_camera_feedback() {
    constexpr int kActuationDelayFrames = 10;
    constexpr int kFrameCount = 480;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 40.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = true;
    Aim aim(config);

    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::array<int, kActuationDelayFrames> delayed_commands{};
    constexpr float kStaticWorldTargetX = 36.0f;
    float camera_x = 0.0f;
    int horizontal_prediction_frames = 0;
    float maximum_horizontal_lead = 0.0f;

    for (int index = 0; index < kFrameCount; ++index) {
        const int delay_slot = index % kActuationDelayFrames;
        camera_x += delayed_commands[delay_slot] /
            config.counts_per_pixel_x * 0.15f;
        delayed_commands[delay_slot] = 0;

        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {body_box(
            160.0f + kStaticWorldTargetX - camera_x,
            172.0f, 40.0f, 80.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "静止水平相机反馈回归必须逐帧保留合法目标");
        if (!result.has_target) continue;

        const float horizontal_lead = std::fabs(result.target.lead_x);
        maximum_horizontal_lead = std::max(
            maximum_horizontal_lead, horizontal_lead);
        if (result.target.lead_active && horizontal_lead > 0.25f) {
            ++horizontal_prediction_frames;
        }
        if (result.has_command) {
            delayed_commands[delay_slot] = result.command.dx_counts;
        }
    }

    expect(horizontal_prediction_frames == 0 &&
               maximum_horizontal_lead <= 0.25f,
           "缩短首次 X 确认后，静止目标的 10 帧相机归位反馈仍不得误建 prediction，帧=" +
               std::to_string(horizontal_prediction_frames) + "，最大 lead=" +
               std::to_string(maximum_horizontal_lead));
}

void test_prediction_never_changes_base_tracking_sequence() {
    AimConfig tracking_config;
    tracking_config.min_confirmed_hits = 1;
    tracking_config.deadzone_pixels = 0.75f;
    tracking_config.smoothing = 0.475f;
    tracking_config.counts_per_pixel_x = 0.40f;
    tracking_config.counts_per_pixel_y = 0.40f;
    tracking_config.max_counts_per_frame = 12.0f;
    tracking_config.acquisition_range_percent = 100.0f;
    tracking_config.enable_delay_compensation = true;
    tracking_config.control_delay_ms = 15.0f;
    tracking_config.max_delay_compensation_ms = 16.0f;
    tracking_config.max_delay_compensation_percent = 15.0f;
    AimConfig prediction_config = tracking_config;
    prediction_config.enable_prediction = true;
    Aim tracking(tracking_config);
    Aim prediction(prediction_config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    int lead_active_frames = 0;
    int different_final_frames = 0;

    // 两个实例消费完全相同的平移+框宽高形变序列，但各自保留
    // 不同的控制器、命令历史和 prediction 状态。这是基础控制点的强不变量：
    // 即使预测已真正活动，每帧轨迹框、速度与基础点也必须与 tracking 逐位相同。
    for (int index = 0; index < 240; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 60.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(2);
        frame.lock_active = true;
        const float center_x = 220.0f - index * 2.5f;
        const float width = 40.0f + static_cast<float>((index % 6) - 3) * 1.4f;
        const float height = 80.0f + static_cast<float>((index % 5) - 2) * 1.6f;
        frame.detections = {body_box(center_x, 160.0f, width, height)};
        const AimResult tracking_result = tracking.process(frame);
        const AimResult prediction_result = prediction.process(frame);
        expect(tracking_result.status == AimStatus::SUCCESS &&
                   prediction_result.status == AimStatus::SUCCESS &&
                   tracking_result.has_target && prediction_result.has_target,
               "prediction 基础点不变量回归每帧必须保留合法目标");
        if (!tracking_result.has_target || !prediction_result.has_target) {
            continue;
        }
        expect(tracking_result.target.track_id ==
                   prediction_result.target.track_id &&
                   tracking_result.target.state ==
                   prediction_result.target.state &&
                   tracking_result.target.x1 == prediction_result.target.x1 &&
                   tracking_result.target.y1 == prediction_result.target.y1 &&
                   tracking_result.target.x2 == prediction_result.target.x2 &&
                   tracking_result.target.y2 == prediction_result.target.y2 &&
                   tracking_result.target.velocity_x ==
                   prediction_result.target.velocity_x &&
                   tracking_result.target.velocity_y ==
                   prediction_result.target.velocity_y &&
                   tracking_result.target.base_aim_x ==
                   prediction_result.target.base_aim_x &&
                   tracking_result.target.base_aim_y ==
                   prediction_result.target.base_aim_y &&
                   prediction_result.target.prediction_aim_x ==
                       prediction_result.target.aim_x &&
                   prediction_result.target.prediction_aim_y ==
                       prediction_result.target.aim_y,
               "prediction 开关不得改变任何 tracking 轨迹或基础控制点字段");
        if (prediction_result.target.lead_active) {
            ++lead_active_frames;
        }
        if (std::fabs(prediction_result.target.aim_x -
                      tracking_result.target.aim_x) > 0.25f ||
            std::fabs(prediction_result.target.aim_y -
                      tracking_result.target.aim_y) > 0.25f) {
            ++different_final_frames;
        }
    }

    expect(lead_active_frames >= 60 && different_final_frames >= 60,
           "基础点相同的同时 prediction 最终点必须真正产生独立前探，活动=" +
               std::to_string(lead_active_frames) + "，差异=" +
               std::to_string(different_final_frames));
}

void test_short_glide_preserves_base_tracking_hold() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.max_lost_frames = 3;
    config.deadzone_pixels = 0.75f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    Aim aim(config);

    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr float kTargetVelocity = 180.0f;
    constexpr float kCameraResponse = 0.85f;
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    float world_target_x = 32.0f;
    float camera_x = 0.0f;
    int first_recovery_command = -1;

    for (int index = 0; index < 220; ++index) {
        world_target_x += kTargetVelocity * kFrameSeconds;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        // 两帧滑行期间模拟用户已经释放物理安全门。恢复同一 Track 时既要
        // 接回基础前馈，也不能继承释放前的 X 输出方向/反向证据。
        frame.lock_active = index < 150 || index > 151;
        if (index < 150 || index > 151) {
            frame.detections = {
                body(160.0f + world_target_x - camera_x, 160.0f)};
        }
        const AimResult result = aim.process(frame);
        if (index == 150 || index == 151) {
            expect(result.has_target && result.target.predicted &&
                       !result.has_command,
                   "短时滑行必须保留轨迹但禁止发送物理命令");
        }
        if (result.has_command) {
            camera_x += result.command.dx_counts /
                config.counts_per_pixel_x * kCameraResponse;
            if (index >= 152 && first_recovery_command < 0) {
                first_recovery_command = index;
            }
        }
    }

    expect(first_recovery_command >= 152 && first_recovery_command <= 153,
           "同一轨迹短时丢框恢复后必须连续接回基础保持量，首次恢复命令帧=" +
               std::to_string(first_recovery_command));
    expect(std::fabs(world_target_x - camera_x) <= 3.0f,
           "短时滑行恢复后不得因重建基础保持量留下持续滞后，误差=" +
               std::to_string(std::fabs(world_target_x - camera_x)));
}

void test_prediction_layer_keeps_base_tracking_hold_continuous() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 0.75f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.enable_prediction = true;
    Aim aim(config);

    constexpr float kFrameSeconds = 1.0f / 240.0f;
    constexpr float kTargetVelocity = 180.0f;
    // 低响应闭环让目标在屏幕坐标中持续远离准星，从而稳定触发 prediction；
    // 本测试验证状态连续性，不用于模拟已标定的真实镜头响应。
    constexpr float kCameraResponse = 0.01f;
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    // 初始轴向误差显式超过按目标框对角线归一化的 prediction 进入阈值，
    // 保证本回归确实覆盖提前量状态，而不是只运行基础 tracking。
    float world_target_x = 20.0f;
    float camera_x = 0.0f;
    int lead_active_frames = 0;
    int consecutive_no_command = 0;
    int maximum_no_command = 0;
    float maximum_velocity = 0.0f;
    float maximum_error = 0.0f;

    for (int index = 0; index < 140; ++index) {
        world_target_x += kTargetVelocity * kFrameSeconds;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.lock_active = true;
        frame.detections = {
            body(160.0f + world_target_x - camera_x, 160.0f)};
        const AimResult result = aim.process(frame);
        if (result.has_target && result.target.lead_active) {
            ++lead_active_frames;
        }
        if (result.has_target) {
            maximum_velocity = std::max(
                maximum_velocity, result.target.velocity_x);
            maximum_error = std::max(
                maximum_error,
                result.target.delay_compensated_aim_x -
                    frame.control_center_x);
        }
        if (result.has_command) {
            camera_x += result.command.dx_counts /
                config.counts_per_pixel_x * kCameraResponse;
            consecutive_no_command = 0;
        } else if (index >= 40) {
            ++consecutive_no_command;
            maximum_no_command = std::max(
                maximum_no_command, consecutive_no_command);
        }
    }

    expect(lead_active_frames > 0,
           "prediction 连续性回归必须实际覆盖提前量活动帧，最大速度=" +
               std::to_string(maximum_velocity) + "，最大误差=" +
               std::to_string(maximum_error));
    expect(maximum_no_command <= 1,
           "prediction 活动不得重置基础 tracking 保持量，最长停发=" +
               std::to_string(maximum_no_command));
}

void test_prediction_adds_continuous_delay_derived_lead() {
    struct Metrics {
        float control_error_sum = 0.0f;
        int control_error_samples = 0;
        int lead_active_frames = 0;
        int late_stationary_lead_frames = 0;
        int late_stationary_command_frames = 0;
        float maximum_stationary_lead = 0.0f;
        float maximum_stationary_error = 0.0f;
        float maximum_stationary_hold_error = 0.0f;
        int stationary_direction_reversals = 0;
        int direct_command_reversals = 0;
        int zero_mediated_command_reversals = 0;
        int maximum_command_step = 0;
        int maximum_command_step_frame = -1;
        int maximum_command_step_before = 0;
        int maximum_command_step_after = 0;
        std::string command_reversal_trace;
    };
    const auto run = [](bool prediction_enabled) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.deadzone_pixels = 0.75f;
        config.smoothing = 0.475f;
        config.counts_per_pixel_x = 0.40f;
        config.counts_per_pixel_y = 0.40f;
        config.max_counts_per_frame = 12.0f;
        config.acquisition_range_percent = 100.0f;
        config.enable_delay_compensation = true;
        config.control_delay_ms = 15.0f;
        config.max_delay_compensation_ms = 16.0f;
        config.enable_prediction = prediction_enabled;
        Aim aim(config);

        constexpr int kFrameCount = 440;
        constexpr int kMovingFrameCount = 300;
        constexpr int kCommandDelayFrames = 4;
        constexpr float kFrameSeconds = 1.0f / 240.0f;
        constexpr float kTargetVelocity = 180.0f;
        // 与已验证的延迟闭环使用同一保守相机响应，避免把未标定的超高
        // 响应合成模型误当作产品稳定性要求。
        constexpr float kCameraResponse = 0.20f;
        float world_target_x = 24.0f;
        float camera_x = 0.0f;
        float delayed_commands[kCommandDelayFrames]{};
        int previous_stationary_command_sign = 0;
        int previous_nonzero_command_sign = 0;
        int previous_horizontal_command = 0;
        bool have_previous_command = false;
        Metrics metrics;
        metrics.maximum_stationary_hold_error =
            std::hypot(40.0f, 80.0f) *
            config.max_delay_compensation_percent / 100.0f;
        const auto base = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);

        for (int index = 0; index < kFrameCount; ++index) {
            const int slot = index % kCommandDelayFrames;
            camera_x += delayed_commands[slot] /
                config.counts_per_pixel_x * kCameraResponse;
            delayed_commands[slot] = 0.0f;
            if (index < kMovingFrameCount) {
                world_target_x += kTargetVelocity * kFrameSeconds;
            }

            AimFrame frame = make_frame(
                static_cast<std::uint64_t>(index + 1),
                base + std::chrono::microseconds(
                    static_cast<long long>(
                        index * 1000000.0f / 240.0f)));
            frame.control_at = frame.captured_at +
                std::chrono::milliseconds(2);
            frame.lock_active = true;
            frame.detections = {
                body(160.0f + world_target_x - camera_x, 160.0f)};
            const AimResult result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "延迟 prediction 闭环回归必须每帧保留合法目标");
            if (!result.has_target) continue;
            if (index >= 20 && index < kMovingFrameCount &&
                result.target.lead_active) {
                ++metrics.lead_active_frames;
            }
            if (index >= 100 && index < kMovingFrameCount - 20) {
                // tracking 以延迟点为控制目标；prediction 则必须允许延迟点
                // 落在准星后方，只要求真正发送控制的最终预测点保持收敛。
                const float control_point_x = prediction_enabled
                    ? result.target.aim_x
                    : result.target.delay_compensated_aim_x;
                metrics.control_error_sum += std::fabs(
                    control_point_x - frame.control_center_x);
                ++metrics.control_error_samples;
            }
            if (index >= 400) {
                if (result.target.lead_active) {
                    ++metrics.late_stationary_lead_frames;
                }
                metrics.maximum_stationary_lead = std::max(
                    metrics.maximum_stationary_lead,
                    std::fabs(result.target.lead_x));
                metrics.maximum_stationary_error = std::max(
                    metrics.maximum_stationary_error,
                    std::fabs(result.target.aim_x -
                              frame.control_center_x));
            }
            const int horizontal_command = result.has_command
                ? result.command.dx_counts : 0;
            if (result.has_command) {
                const int command_sign = horizontal_command < 0 ? -1 : 1;
                if (previous_nonzero_command_sign != 0 &&
                    command_sign != previous_nonzero_command_sign) {
                    ++metrics.zero_mediated_command_reversals;
                    if (metrics.command_reversal_trace.empty()) {
                        metrics.command_reversal_trace =
                            "帧=" + std::to_string(index) +
                            "，前向=" +
                            std::to_string(previous_nonzero_command_sign) +
                            "，当前命令=" +
                            std::to_string(horizontal_command) +
                            "，base=" +
                            std::to_string(result.target.base_aim_x) +
                            "，delay=" +
                            std::to_string(
                                result.target.delay_compensated_aim_x) +
                            "，final=" +
                            std::to_string(result.target.aim_x) +
                            "，lead=" +
                            std::to_string(result.target.lead_x);
                    }
                }
                previous_nonzero_command_sign = command_sign;
                if (have_previous_command) {
                    const int command_step = std::abs(
                        horizontal_command - previous_horizontal_command);
                    if (command_step > metrics.maximum_command_step) {
                        metrics.maximum_command_step = command_step;
                        metrics.maximum_command_step_frame = index;
                        metrics.maximum_command_step_before =
                            previous_horizontal_command;
                        metrics.maximum_command_step_after =
                            horizontal_command;
                    }
                    if (horizontal_command * previous_horizontal_command < 0) {
                        ++metrics.direct_command_reversals;
                    }
                }
                previous_horizontal_command = horizontal_command;
                have_previous_command = true;
            } else {
                previous_horizontal_command = 0;
                have_previous_command = false;
            }
            if (result.has_command) {
                delayed_commands[slot] =
                    static_cast<float>(result.command.dx_counts);
                if (index >= 400 && result.command.dx_counts != 0) {
                    ++metrics.late_stationary_command_frames;
                    const int sign = result.command.dx_counts < 0 ? -1 : 1;
                    if (previous_stationary_command_sign != 0 &&
                        sign != previous_stationary_command_sign) {
                        ++metrics.stationary_direction_reversals;
                    }
                    previous_stationary_command_sign = sign;
                }
            }
        }
        return metrics;
    };

    const Metrics tracking = run(false);
    const Metrics prediction = run(true);
    const float tracking_mean = tracking.control_error_sum /
        static_cast<float>(tracking.control_error_samples);
    const float prediction_mean = prediction.control_error_sum /
        static_cast<float>(prediction.control_error_samples);
    expect(prediction.lead_active_frames >= 220,
           "匀速闭环开启 prediction 后必须持续从延迟补偿点产生提前量，实际=" +
               std::to_string(prediction.lead_active_frames));
    expect(prediction_mean <= 3.3f,
           "prediction 最终点平均误差不得超过 3.3 px，tracking=" +
               std::to_string(tracking_mean) + "，prediction=" +
               std::to_string(prediction_mean));
    expect(prediction.direct_command_reversals == 0 &&
               prediction.zero_mediated_command_reversals == 0 &&
               prediction.maximum_command_step <= 4,
           "可见预测提前不得破坏同轴命令连续性，直接反转=" +
               std::to_string(prediction.direct_command_reversals) +
               "，经零反转=" +
               std::to_string(prediction.zero_mediated_command_reversals) +
               "，tracking 经零反转=" +
               std::to_string(tracking.zero_mediated_command_reversals) +
               "，最大阶跃=" +
               std::to_string(prediction.maximum_command_step) +
               "，帧=" +
               std::to_string(prediction.maximum_command_step_frame) +
               "，命令=" +
               std::to_string(prediction.maximum_command_step_before) +
               "→" +
               std::to_string(prediction.maximum_command_step_after) +
               "，首个经零反转=" + prediction.command_reversal_trace);
    expect(prediction.maximum_stationary_lead <= 0.50f &&
               prediction.late_stationary_command_frames == 0 &&
               prediction.maximum_stationary_error <=
                   prediction.maximum_stationary_hold_error &&
               prediction.stationary_direction_reversals <= 2,
           "延迟 prediction 在目标停止后必须撤销水平提前并停发，禁止为归位反拉；残余位置只允许保留在有界提前区，预测帧=" +
               std::to_string(prediction.late_stationary_lead_frames) +
               "，最大提前量=" +
               std::to_string(prediction.maximum_stationary_lead) +
               "，命令帧=" +
               std::to_string(prediction.late_stationary_command_frames) +
               "，最大误差=" +
               std::to_string(prediction.maximum_stationary_error) +
               "，保持上限=" +
               std::to_string(prediction.maximum_stationary_hold_error) +
               "，方向反转=" +
               std::to_string(prediction.stationary_direction_reversals) +
               "；tracking 最大误差=" +
               std::to_string(tracking.maximum_stationary_error) +
               "，方向反转=" +
               std::to_string(tracking.stationary_direction_reversals));
}

} // namespace

int main() {
    test_status_transition_logs_are_limited();

    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);

    test_invalid_input();
    test_frame_order_contract();
    test_head_body_merge_and_confirmation();
    test_selected_target_exposes_only_current_matched_observation();
    test_head_only_uses_parameterized_aim_region();
    test_short_loss_keeps_track_id();
    test_command_limit_and_reset();
    test_source_pixel_scale_controls_mouse_counts();
    test_global_head_body_assignment();
    test_head_body_normalized_aim_stays_stable();
    test_body_box_shape_jitter_does_not_move_stable_aim_point();
    test_body_box_shape_jitter_preserves_real_translation();
    test_coherent_box_center_jitter_does_not_move_base_anchor();
    test_coherent_box_center_jitter_preserves_real_translation();
    test_multiframe_pose_deformation_does_not_move_base_anchor();
    test_long_pose_deformation_with_sparse_evidence_does_not_leak_into_anchor();
    test_horizontal_pose_trend_is_speed_independent();
    test_horizontal_pose_trend_uses_capture_time_across_head_and_delivery_gaps();
    test_horizontal_pose_trend_reversal_is_bounded();
    test_horizontal_unsupported_prediction_does_not_stick_at_range_boundary();
    test_horizontal_confirmed_release_does_not_snap_hidden_inventory();
    test_horizontal_persistent_innovation_keeps_velocity_and_delay_continuous();
    test_horizontal_maneuver_accepts_coherent_second_reversal();
    test_horizontal_maneuver_rejects_short_coherent_center_outliers();
    test_horizontal_pose_trend_recovers_after_body_semantic_loss();
    test_horizontal_pose_trend_bounds_sparse_center_outliers();
    test_horizontal_partial_visibility_isolates_small_transients();
    test_horizontal_partial_visibility_exact_three_recovers();
    test_horizontal_partial_rebuild_does_not_inject_velocity();
    test_head_only_width_change_skips_body_partial_guard();
    test_horizontal_partial_visibility_accepts_persistent_geometry();
    test_vertical_jump_pose_protection_keeps_configured_aim_height();
    test_body_aim_range_is_static_safe_and_motion_bounded();
    test_multi_target_crossing_keeps_selected_identity();
    test_tentative_duplicate_cannot_steal_confirmed_observation();
    test_confirmed_duplicate_cannot_starve_selected_observation();
    test_selected_association_bias_does_not_hijack_distinct_target();
    test_loss_prediction_does_not_compound_time();
    test_observation_age_adds_bounded_lead();
    test_prediction_lead_can_leave_box_with_bounded_distance();
    test_dynamic_control_range_does_not_reduce_observation();
    test_prediction_hysteresis_avoids_crosshair_oscillation();
    test_closed_loop_view_feedback_converges_without_limit_cycle();
    test_control_trajectory_never_moves_away_from_target();
    test_integral_tracks_constant_velocity_with_bounded_error();
    test_delayed_closed_loop_holds_moving_base_point();
    test_delayed_pose_closed_loop_keeps_tracking_pi_continuous();
    test_vertical_shape_noise_does_not_stutter_horizontal_tracking();
    test_backend_completed_command_feedback_contract();
    test_backend_completed_delay_inventory_changes_tracking_reversal_response();
    test_faster_closing_slope_continuously_reduces_tracking_request();
    test_same_direction_completed_inventory_brakes_closing_request();
    test_fixed_scene_replay_does_not_amplify_horizontal_observation();
    test_static_closed_loop_replay_does_not_repeat_horizontal_commands();
    test_latest_static_replay_does_not_amplify_horizontal_base();
    test_latest_physical_replay_brakes_before_horizontal_crossing();
    test_actual_game_superjump_current_common_translation_brakes_x();
    test_tracking_derivative_separates_in_box_reference_from_common_translation();
    test_delayed_partial_visibility_closed_loop_preserves_real_reversals();
    test_delayed_left_motion_quantizes_from_world_feedforward();
    test_tracking_pi_is_separate_from_prediction_projection();
    test_base_tracking_quantization_has_no_speed_threshold();
    test_delay_shaping_has_no_speed_threshold_before_prediction();
    test_prediction_direct_feedforward_has_no_absolute_velocity_mode();
    test_prediction_confirmed_stop_has_no_absolute_velocity_mode();
    test_prediction_stop_measurement_has_no_absolute_velocity_mode();
    test_tracking_public_point_is_independent_of_command_age();
    test_pending_command_age_uses_control_execution_time();
    test_tracking_pi_filters_axes_independently();
    test_prediction_uses_world_motion_when_delay_vector_points_backward();
    test_prediction_survives_short_world_motion_measurement_dips();
    test_prediction_motion_candidate_tolerates_one_low_sample();
    test_prediction_motion_axis_requires_confirmed_stop();
    test_prediction_closed_loop_keeps_visible_left_lead_without_pullback();
    test_horizontal_prediction_does_not_block_vertical_height_correction();
    test_vertical_pullback_hold_releases_while_horizontal_prediction_continues();
    test_prediction_pullback_timeout_has_no_absolute_velocity_mode();
    test_horizontal_prediction_rejects_delayed_vertical_camera_feedback();
    test_long_delay_prediction_distributes_horizontal_hold_command();
    test_real_cadence_prediction_closes_public_point_error();
    test_variable_real_cadence_prediction_closes_public_point_error();
    test_prediction_lead_is_stable_across_bursty_frame_intervals();
    test_prediction_release_offset_is_slew_limited();
    test_prediction_pullback_hold_releases_after_real_reversal();
    test_prediction_pullback_command_requires_causal_world_motion();
    test_prediction_inventory_brake_is_short_and_minimal();
    test_two_axis_command_reversal_passes_through_zero();
    test_integral_releases_on_reversal_and_static_settle();
    test_quantization_residual_cannot_reverse_after_crossing();
    test_tracking_public_point_stays_on_base_feature();
    test_control_step_cannot_cross_in_box_aim_point();
    test_tracking_and_prediction_projection_contracts_are_separate();
    test_delay_compensation_stacks_before_prediction();
    test_horizontal_prediction_startup_rejects_static_camera_feedback();
    test_prediction_never_changes_base_tracking_sequence();
    test_short_glide_preserves_base_tracking_hold();
    test_prediction_layer_keeps_base_tracking_hold_continuous();
    test_prediction_adds_continuous_delay_derived_lead();

    Log::shutdown();
    if (failures != 0) {
        std::cerr << "Aim 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Aim 测试全部通过。\n";
    return 0;
}
