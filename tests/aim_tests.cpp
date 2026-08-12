#include "aim/aim.h"
#include "aim/aim_prediction_internal.h"
#include "log/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

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

Detection body_box(float center_x, float center_y,
                   float width, float height,
                   float confidence = 0.9f) {
    return {center_x - width * 0.5f, center_y - height * 0.5f,
            center_x + width * 0.5f, center_y + height * 0.5f,
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
    frame.control_center_x = 160.0f;
    frame.control_center_y = 160.0f;
    return frame;
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

    AimFrame second = make_frame(2, base + std::chrono::milliseconds(4));
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
    expect(mean_error <= 6.0f && p95_error <= 12.0f,
           "15 ms 输出延迟闭环必须持续贴合移动基础点，平均误差=" +
               std::to_string(mean_error) + "，最大误差=" +
               std::to_string(maximum_error) + ", P95=" +
               std::to_string(p95_error));
    expect(maximum_no_command <= 1,
           "15 ms 输出延迟闭环不得周期停发，最长停发=" +
               std::to_string(maximum_no_command));
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
    int self_motion_crossing_frames = 0;

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
            result.target.base_aim_x < frame.control_center_x &&
            result.target.delay_compensated_aim_x > frame.control_center_x &&
            result.target.velocity_x > 20.0f) {
            ++self_motion_crossing_frames;
        }
    }

    expect(self_motion_crossing_frames > 0,
           "左移延迟闭环必须覆盖相机反馈使相对速度和延迟点反向的窗口");
    expect(maximum_no_command <= 1,
           "左移延迟闭环必须按世界运动前馈量化，最长停发=" +
               std::to_string(maximum_no_command));
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

void test_tracking_projection_velocity_uses_matching_frame_interval() {
    constexpr float kMinimumCounts = 0.25f;
    constexpr int kConfirmFrames = 4;
    constexpr float kBuildGain = 8.0f;
    constexpr float kReleaseGain = 40.0f;
    constexpr float kReversalGain = 40.0f;
    constexpr float kWorldVelocity = 120.0f;
    float velocity = 0.0f;
    int low_frames = 0;

    // 同一物理速度在 4/12 ms 帧中对应不同 counts/frame；用各自 dt 归一化后，
    // 观察器目标必须保持 120 counts/s，不能随交付节奏在三倍幅值间摆动。
    for (int index = 0; index < 80; ++index) {
        const float dt = index % 2 == 0 ? 0.004f : 0.012f;
        aim::detail::update_tracking_projection_velocity_axis(
            kWorldVelocity * dt, dt, kMinimumCounts, kConfirmFrames,
            kBuildGain, kReleaseGain, kReversalGain, velocity, low_frames);
    }
    expect(std::fabs(velocity - kWorldVelocity) < 1.0f && low_frames == 0,
           "变化 dt 下 tracking 世界速度必须收敛到物理速度，实际=" +
               std::to_string(velocity));

    // 真实反向不能再沿 8/s 的建立时间常数缓慢穿零；40/s 在三个 12 ms
    // 采样内应完成换向，同时仍经过连续状态而不是单帧硬清零。
    for (int index = 0; index < 3; ++index) {
        constexpr float kDeltaSeconds = 0.012f;
        aim::detail::update_tracking_projection_velocity_axis(
            -kWorldVelocity * kDeltaSeconds, kDeltaSeconds,
            kMinimumCounts, kConfirmFrames, kBuildGain, kReleaseGain,
            kReversalGain, velocity, low_frames);
    }
    expect(velocity < 0.0f,
           "tracking 世界速度确认反向后三帧内必须穿零，实际=" +
               std::to_string(velocity));
}

void test_tracking_delay_output_back_calculates_saturation() {
    constexpr float kDesiredOutput = 22.0f;
    constexpr float kMaximumStep = 1.0f;
    bool initialized = false;
    float output_state = 0.0f;

    // 首次从基础 2 px 延迟开始，再按原渐变速度建立额外提前量。
    float output = aim::detail::update_tracking_delay_output(
        kDesiredOutput, 2.0f, 20.0f, kMaximumStep,
        initialized, output_state);
    expect(initialized && std::fabs(output_state - 3.0f) < 0.001f &&
               std::fabs(output - 3.0f) < 0.001f,
           "tracking 总 X 延迟首次必须从基础值按单帧上限建立");

    for (int index = 0; index < 10; ++index) {
        output = aim::detail::update_tracking_delay_output(
            kDesiredOutput, 2.0f, 20.0f, kMaximumStep,
            initialized, output_state);
    }
    expect(output_state > 10.0f,
           "反饱和回归必须先形成可观察的总 X 延迟状态");

    // Y 跳跃把水平剩余空间压到 4 px 时，总状态和公开输出必须同步回写。
    output = aim::detail::update_tracking_delay_output(
        kDesiredOutput, 2.0f, 4.0f, kMaximumStep,
        initialized, output_state);
    expect(std::fabs(output_state - 4.0f) < 0.001f &&
               std::fabs(output - 4.0f) < 0.001f,
           "水平预算收缩必须把限幅结果回写到总 X 延迟状态");

    // Y 回落后，基础分量即使同时跳到 8 px，总输出也只能从 4 增到 5 px；
    // 这覆盖上一版未限制基础 16 ms 分量的缺口。
    output = aim::detail::update_tracking_delay_output(
        kDesiredOutput, 8.0f, 10.0f, kMaximumStep,
        initialized, output_state);
    expect(std::fabs(output_state - 5.0f) < 0.001f &&
               std::fabs(output - 5.0f) < 0.001f,
           "预算扩张和基础延迟变化必须统一服从总 X 输出渐变上限");
}

void test_tracking_jump_base_range_expands_and_releases_smoothly() {
    bool initialized = false;
    float half_range = 0.0f;

    float published = aim::detail::update_tracking_horizontal_half_range(
        0.25f, 40.0f, 20.0f, 60.0f, 0.40f, 0.010f, 4.0f, 1.0f,
        initialized, half_range);
    expect(initialized && std::fabs(published - 0.29f) < 0.001f,
           "垂直速度进入过渡区后必须提前渐扩水平范围");

    published = aim::detail::update_tracking_horizontal_half_range(
        0.25f, 80.0f, 20.0f, 60.0f, 0.40f, 0.010f, 4.0f, 1.0f,
        initialized, half_range);
    expect(std::fabs(published - 0.33f) < 0.001f,
           "高速首帧仍必须渐扩，不能直接切到完整框");
    for (int index = 0; index < 5; ++index) {
        published = aim::detail::update_tracking_horizontal_half_range(
            0.25f, 80.0f, 20.0f, 60.0f, 0.40f, 0.010f, 4.0f, 1.0f,
            initialized, half_range);
    }
    expect(std::fabs(published - 0.40f) < 0.001f,
           "持续高速垂直运动必须快速释放到受控水平上界");

    published = aim::detail::update_tracking_horizontal_half_range(
        0.25f, 0.0f, 20.0f, 60.0f, 0.40f, 0.010f, 4.0f, 1.0f,
        initialized, half_range);
    expect(std::fabs(published - 0.39f) < 0.001f,
           "落地后水平内窗必须慢速回收，禁止快速夹回配置范围");
}

void test_tracking_jump_horizontal_projection_rejects_camera_feedback() {
    constexpr float kRelativeCameraFeedback = -420.0f;
    constexpr float kConfirmedWorldMotion = 135.0f;
    constexpr float kDeltaSeconds = 0.010f;
    constexpr float kBlendSlewPerSecond = 4.0f;
    float blend = 0.0f;

    aim::detail::update_tracking_jump_world_velocity_blend(
        true, kDeltaSeconds, kBlendSlewPerSecond, blend);
    const float first_jump_velocity =
        aim::detail::select_tracking_horizontal_projection_velocity(
            kRelativeCameraFeedback, kConfirmedWorldMotion, blend);
    expect(std::fabs(blend - 0.04f) < 0.001f &&
               first_jump_velocity > kRelativeCameraFeedback &&
               first_jump_velocity < kConfirmedWorldMotion,
           "跳跃 X 首帧只能渐入世界速度，禁止在垂直阈值处硬切相位");

    for (int index = 0; index < 24; ++index) {
        aim::detail::update_tracking_jump_world_velocity_blend(
            true, kDeltaSeconds, kBlendSlewPerSecond, blend);
    }
    const float sustained_jump_velocity =
        aim::detail::select_tracking_horizontal_projection_velocity(
            kRelativeCameraFeedback, kConfirmedWorldMotion, blend);
    expect(std::fabs(blend - 1.0f) < 0.001f &&
               std::fabs(sustained_jump_velocity -
                         kConfirmedWorldMotion) < 0.001f,
           "持续跳跃 250 ms 后必须完整隔离水平镜头回流");

    aim::detail::update_tracking_jump_world_velocity_blend(
        false, kDeltaSeconds, kBlendSlewPerSecond, blend);
    const float first_release_velocity =
        aim::detail::select_tracking_horizontal_projection_velocity(
            kRelativeCameraFeedback, kConfirmedWorldMotion, blend);
    expect(std::fabs(blend - 0.96f) < 0.001f &&
               first_release_velocity > kRelativeCameraFeedback &&
               first_release_velocity < kConfirmedWorldMotion,
           "落地首帧必须渐退世界速度，禁止立即切回屏幕相对速度");
}

void test_tracking_jump_reversal_waits_for_pending_inventory() {
    const auto allowed = [](int command_counts, float pending_counts,
                            float control_error_counts, bool jump_active) {
        return aim::detail::tracking_jump_reversal_command_allowed(
            command_counts, pending_counts, control_error_counts,
            0.10f, jump_active);
    };

    expect(!allowed(-2, 33.0f, -2.0f, true) &&
               !allowed(1, -27.0f, 1.0f, true),
           "跳跃 X 的弱反向纠偏必须等待旧方向库存影响下降");
    expect(allowed(-2, 33.0f, -4.0f, true) &&
               allowed(1, -27.0f, 3.0f, true),
           "真实反向需求超过预计库存影响后必须恢复 X，禁止等待全清空");
    expect(allowed(2, 33.0f, 0.5f, true) &&
               allowed(-1, -27.0f, -0.5f, true),
           "跳跃 X 同向续发不得被库存门禁误停");
    expect(allowed(-2, 0.0f, -0.5f, true) &&
               allowed(-2, 33.0f, -0.5f, false),
           "库存清空后的真实反向和非跳跃场景必须保持原控制路径");
    expect(allowed(0, 33.0f, 0.0f, true),
           "零命令必须始终允许，门禁不得自行产生物理输出");
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

void test_base_crossing_releases_integral_smoothly() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.50f;
    config.counts_per_pixel_x = 0.40f;
    config.counts_per_pixel_y = 0.40f;
    config.max_counts_per_frame = 12.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 16.0f;
    config.max_delay_compensation_percent = 15.0f;
    Aim aim(config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    for (int index = 0; index < 120; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.lock_active = true;
        frame.detections = {body(150.0f, 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "延迟积分过零回归的保持量建立阶段必须成功");
        if (index >= 20 && !result.target.delay_compensation_active) {
            expect(result.target.delay_compensation_ms == 0.0f,
                   "延迟向量未激活时必须报告 0 ms，禁止产生报告契约矛盾");
        }
    }

    bool observed_base_crossing = false;
    bool observed_reverse_command = false;
    int base_crossing_offset = -1;
    int reverse_command_offset = -1;
    int previous_command = 0;
    bool have_previous_command = false;
    const float hold_band = std::max(2.0f, config.deadzone_pixels * 1.5f);
    for (int offset = 0; offset < 20; ++offset) {
        const int index = 120 + offset;
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::microseconds(
                static_cast<long long>(index * 1000000.0f / 240.0f)));
        frame.lock_active = true;
        frame.detections = {body(150.0f + (offset + 1) * 0.75f, 160.0f)};
        const AimResult result = aim.process(frame);
        const float base_error = result.target.base_aim_x -
            frame.control_center_x;
        const int command = result.has_command
            ? result.command.dx_counts : 0;
        // 同方向增减速受逐 count 斜率约束；真实换向时方向安全门允许一次
        // 直接归零，但任一轴都必须经过零，不能从旧方向直接跳到反方向。
        if (have_previous_command && previous_command * command > 0) {
            expect(std::abs(command - previous_command) <= 1,
                   "基础点回穿的同向物理命令必须逐 count 变化，前值=" +
                       std::to_string(previous_command) + "，当前=" +
                       std::to_string(command));
        }
        if (have_previous_command && previous_command * command < 0) {
            expect(false,
                   "基础点真实换向的单轴命令必须先经过零，前值=" +
                       std::to_string(previous_command) + "，当前=" +
                       std::to_string(command));
        }
        previous_command = command;
        have_previous_command = true;
        if (base_error > hold_band) {
            if (!observed_base_crossing) {
                observed_base_crossing = true;
                base_crossing_offset = offset;
            }
            expect(command >= 0,
                   "基础点越过保持带后不得继续发送旧方向维持命令");
            if (command > 0) {
                observed_reverse_command = true;
                reverse_command_offset = offset;
                break;
            }
        }
    }
    expect(observed_base_crossing,
           "回归必须实际覆盖基础点越过死区，而不只是延迟投影点过零");
    expect(observed_reverse_command &&
               reverse_command_offset - base_crossing_offset <= 6,
           "基础点真实越过后必须平滑并及时反向，越过帧=" +
               std::to_string(base_crossing_offset) + "，反向帧=" +
               std::to_string(reverse_command_offset));
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

void test_delay_projection_crossing_keeps_base_tracking_hold() {
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
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    Aim aim(config);

    // 先建立向左的基础保持量，再以约 100 px/s 向右回穿。实机第十一轮
    // 的长停发都发生在基础点仍位于原侧、但相机反馈使延迟投影点短暂过零
    // 的阶段；这里固定复现该职责冲突。
    for (int index = 0; index < 40; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.lock_active = true;
        frame.detections = {body(140.0f - index * 0.5f, 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "延迟补偿方向回归的建立阶段必须成功");
    }

    for (int index = 0; index < 19; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 41),
            base + std::chrono::milliseconds((index + 40) * 10));
        frame.lock_active = true;
        frame.detections = {body(140.0f + (index + 1), 160.0f)};
        const AimResult result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS,
               "延迟补偿方向回归的回穿阶段必须成功");
    }

    AimFrame crossed = make_frame(60, base + std::chrono::milliseconds(590));
    crossed.lock_active = true;
    crossed.detections = {body(159.8f, 160.0f)};
    const AimResult result = aim.process(crossed);
    const float base_error = result.target.base_aim_x -
        crossed.control_center_x;
    const float final_error = result.target.delay_compensated_aim_x -
        crossed.control_center_x;
    const float hold_band = std::max(2.0f, config.deadzone_pixels * 1.5f);
    expect(result.status == AimStatus::SUCCESS && result.has_target &&
               base_error < 0.0f && final_error > 0.0f &&
               std::fabs(final_error) <= hold_band,
           "回归必须覆盖基础点未过零、延迟投影点在保持带内过零");
    if (result.has_command) {
        expect(result.command.dx_counts * final_error >= 0.0f,
               "延迟投影点瞬时过零后的整数命令不得背离最终瞄点");
    }

    // 实机第十二轮证明：基础点在一个延迟闭环周期中可能先短暂越过保持带，
    // 随后回到原侧且投影点仍在另一侧。单次过冲不得清空恒速前馈，否则
    // 回到保持带后只能等待比例误差再次扩大，形成 5~6 帧停发窗口。
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
            expect(transient_final_error <= 0.0f,
                   "在途命令补偿后，返回原侧的基础点不得保留虚假跨侧投影");
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

void test_tracking_delay_projection_uses_longer_horizontal_horizon_only() {
    AimConfig tracking_config;
    tracking_config.min_confirmed_hits = 1;
    tracking_config.deadzone_pixels = 0.0f;
    tracking_config.smoothing = 1.0f;
    tracking_config.counts_per_pixel_x = 1.0f;
    tracking_config.counts_per_pixel_y = 1.0f;
    tracking_config.max_counts_per_frame = 100.0f;
    tracking_config.max_center_distance = 1.0f;
    tracking_config.enable_delay_compensation = true;
    tracking_config.control_delay_ms = 40.0f;
    tracking_config.max_delay_compensation_ms = 44.0f;
    tracking_config.max_delay_compensation_percent = 50.0f;
    AimConfig prediction_config = tracking_config;
    prediction_config.enable_prediction = true;
    Aim tracking(tracking_config);
    Aim prediction(prediction_config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    AimResult tracking_result;
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
        tracking_result = tracking.process(frame);
        prediction_result = prediction.process(frame);
    }

    expect(tracking_result.status == AimStatus::SUCCESS &&
               tracking_result.has_target &&
               tracking_result.target.delay_compensation_active,
           "分轴延迟投影回归必须形成有效 tracking 目标和补偿向量");
    expect(std::fabs(
               tracking_result.target.delay_compensation_ms_x - 40.0f) <
               0.01f &&
               std::fabs(
                   tracking_result.target.delay_compensation_ms_y - 16.0f) <
                   0.01f &&
               std::fabs(
                   tracking_result.target.delay_compensation_ms - 40.0f) <
                   0.01f,
           "prediction 关闭时必须报告 X=40 ms、Y=16 ms，兼容字段取两轴最大值");
    expect(std::fabs(
               prediction_result.target.delay_compensation_ms_x - 16.0f) <
               0.01f &&
               std::fabs(
                   prediction_result.target.delay_compensation_ms_y - 16.0f) <
                   0.01f,
           "prediction 开启时必须保留已经验证的双轴 16 ms 几何基准");
    expect(std::fabs(
               tracking_result.target.delay_compensation_y -
               prediction_result.target.delay_compensation_y) < 0.01f,
           "X 轴扩展不得改变同一输入下的 Y 轴延迟补偿量");
    expect(std::fabs(tracking_result.target.delay_compensation_x) >
               std::fabs(prediction_result.target.delay_compensation_x) +
                   0.10f,
           "prediction 关闭时 X 轴 40 ms 投影必须产生可观测的额外水平提前量");
}

void test_tracking_horizontal_extension_rejects_camera_feedback_direction() {
    AimConfig short_config;
    short_config.min_confirmed_hits = 1;
    short_config.deadzone_pixels = 0.0f;
    short_config.smoothing = 1.0f;
    short_config.counts_per_pixel_x = 0.40f;
    short_config.counts_per_pixel_y = 0.40f;
    short_config.max_counts_per_frame = 14.0f;
    short_config.max_center_distance = 1.0f;
    short_config.enable_delay_compensation = true;
    short_config.control_delay_ms = 15.0f;
    short_config.max_delay_compensation_ms = 16.0f;
    short_config.max_delay_compensation_percent = 50.0f;
    AimConfig extended_config = short_config;
    extended_config.control_delay_ms = 40.0f;
    extended_config.max_delay_compensation_ms = 44.0f;
    Aim short_projection(short_config);
    Aim extended_projection(extended_config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);

    for (int index = 0; index < 20; ++index) {
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1),
            base + std::chrono::milliseconds(index * 10));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(5);
        frame.lock_active = false;
        frame.detections = {body(180.0f + index * 1.5f, 160.0f)};
        short_projection.process(frame);
        extended_projection.process(frame);
    }

    // 人物世界运动已经稳定向右后，模拟一帧由相机追踪造成的屏幕左移。
    // 额外 24 ms 必须继续使用上一帧的世界维持量，而不是放大负 track.vx。
    AimFrame feedback_frame = make_frame(
        21, base + std::chrono::milliseconds(200));
    feedback_frame.control_at = feedback_frame.captured_at +
        std::chrono::milliseconds(5);
    feedback_frame.lock_active = false;
    feedback_frame.detections = {body(190.0f, 160.0f)};
    const AimResult short_result = short_projection.process(feedback_frame);
    const AimResult extended_result =
        extended_projection.process(feedback_frame);

    expect(short_result.has_target && extended_result.has_target &&
               extended_result.target.velocity_x < 0.0f,
           "相机反馈方向回归必须形成与既有世界运动相反的屏幕 X 速度");
    expect(extended_result.target.delay_compensation_x >
               short_result.target.delay_compensation_x + 0.10f,
           "额外 X 时域必须沿自运动扣除后的世界方向，禁止继续积分反向屏幕回流");
}

void test_tracking_horizontal_extension_is_slew_limited_under_variable_dt() {
    constexpr float kExpectedMaximumSlewDiagonalsPerSecond = 0.75f;
    AimConfig short_config;
    short_config.min_confirmed_hits = 1;
    short_config.deadzone_pixels = 0.0f;
    short_config.smoothing = 1.0f;
    short_config.counts_per_pixel_x = 0.40f;
    short_config.counts_per_pixel_y = 0.40f;
    short_config.max_counts_per_frame = 14.0f;
    short_config.max_center_distance = 1.0f;
    short_config.enable_delay_compensation = true;
    short_config.control_delay_ms = 15.0f;
    short_config.max_delay_compensation_ms = 16.0f;
    short_config.max_delay_compensation_percent = 50.0f;
    AimConfig extended_config = short_config;
    extended_config.control_delay_ms = 40.0f;
    extended_config.max_delay_compensation_ms = 44.0f;
    Aim short_projection(short_config);
    Aim extended_projection(extended_config);
    const auto base = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    std::chrono::milliseconds elapsed{};
    float previous_extension = 0.0f;
    float maximum_extension = 0.0f;
    bool have_previous_extension = false;

    for (int index = 0; index < 100; ++index) {
        const auto frame_dt = std::chrono::milliseconds(
            index % 2 == 0 ? 4 : 12);
        if (index != 0) elapsed += frame_dt;
        const float elapsed_seconds =
            std::chrono::duration<float>(elapsed).count();
        AimFrame frame = make_frame(
            static_cast<std::uint64_t>(index + 1), base + elapsed);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(2);
        frame.lock_active = false;
        frame.detections = {body(
            180.0f + elapsed_seconds * 120.0f, 160.0f)};
        const AimResult short_result = short_projection.process(frame);
        const AimResult extended_result = extended_projection.process(frame);
        expect(short_result.has_target && extended_result.has_target,
               "变化 dt 的水平扩展回归必须持续保留目标");
        if (!short_result.has_target || !extended_result.has_target) continue;
        const float extension =
            extended_result.target.delay_compensation_x -
            short_result.target.delay_compensation_x;
        maximum_extension = std::max(maximum_extension, std::fabs(extension));
        if (have_previous_extension) {
            const float target_diagonal = std::hypot(
                extended_result.target.x2 - extended_result.target.x1,
                extended_result.target.y2 - extended_result.target.y1);
            const float maximum_step = target_diagonal *
                kExpectedMaximumSlewDiagonalsPerSecond *
                std::chrono::duration<float>(frame_dt).count();
            expect(std::fabs(extension - previous_extension) <=
                       maximum_step + 0.02f,
                   "变化 dt 下额外 X 位移不得超过按目标尺度归一化的单帧渐变上限");
        }
        previous_extension = extension;
        have_previous_extension = true;
    }

    expect(maximum_extension >= 1.0f,
           "渐变限制不得把持续水平世界运动的额外提前量永久压为零");
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
        frame.lock_active = true;
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
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);

    test_invalid_input();
    test_frame_order_contract();
    test_head_body_merge_and_confirmation();
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
    test_body_aim_range_is_static_safe_and_motion_bounded();
    test_multi_target_crossing_keeps_selected_identity();
    test_loss_prediction_does_not_compound_time();
    test_observation_age_adds_bounded_lead();
    test_prediction_lead_can_leave_box_with_bounded_distance();
    test_dynamic_control_range_does_not_reduce_observation();
    test_prediction_hysteresis_avoids_crosshair_oscillation();
    test_closed_loop_view_feedback_converges_without_limit_cycle();
    test_control_trajectory_never_moves_away_from_target();
    test_integral_tracks_constant_velocity_with_bounded_error();
    test_delayed_closed_loop_holds_moving_base_point();
    test_delayed_left_motion_quantizes_from_world_feedforward();
    test_prediction_uses_world_motion_when_delay_vector_points_backward();
    test_prediction_survives_short_world_motion_measurement_dips();
    test_prediction_motion_candidate_tolerates_one_low_sample();
    test_prediction_motion_axis_requires_confirmed_stop();
    test_tracking_projection_velocity_uses_matching_frame_interval();
    test_tracking_delay_output_back_calculates_saturation();
    test_tracking_jump_base_range_expands_and_releases_smoothly();
    test_tracking_jump_horizontal_projection_rejects_camera_feedback();
    test_tracking_jump_reversal_waits_for_pending_inventory();
    test_prediction_closed_loop_keeps_visible_left_lead_without_pullback();
    test_horizontal_prediction_does_not_block_vertical_height_correction();
    test_vertical_pullback_hold_releases_while_horizontal_prediction_continues();
    test_horizontal_prediction_rejects_delayed_vertical_camera_feedback();
    test_long_delay_prediction_distributes_horizontal_hold_command();
    test_real_cadence_prediction_closes_public_point_error();
    test_variable_real_cadence_prediction_closes_public_point_error();
    test_prediction_lead_is_stable_across_bursty_frame_intervals();
    test_prediction_release_offset_is_slew_limited();
    test_prediction_pullback_hold_releases_after_real_reversal();
    test_prediction_pullback_command_requires_causal_world_motion();
    test_prediction_inventory_brake_is_short_and_minimal();
    test_base_crossing_releases_integral_smoothly();
    test_two_axis_command_reversal_passes_through_zero();
    test_integral_releases_on_reversal_and_static_settle();
    test_quantization_residual_cannot_reverse_after_crossing();
    test_delay_projection_crossing_keeps_base_tracking_hold();
    test_control_step_cannot_cross_in_box_aim_point();
    test_tracking_delay_projection_uses_longer_horizontal_horizon_only();
    test_tracking_horizontal_extension_rejects_camera_feedback_direction();
    test_tracking_horizontal_extension_is_slew_limited_under_variable_dt();
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
