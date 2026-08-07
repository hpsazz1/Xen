#include "aim/aim.h"
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
        // 直接归零，但绝不能跨零跳到反方向的多 count 命令。
        if (have_previous_command && previous_command * command > 0) {
            expect(std::abs(command - previous_command) <= 1,
                   "基础点回穿的同向物理命令必须逐 count 变化，前值=" +
                       std::to_string(previous_command) + "，当前=" +
                       std::to_string(command));
        }
        if (have_previous_command && previous_command * command < 0) {
            expect(std::abs(command) <= 1,
                   "基础点真实换向不得跨零跳到多 count 反向命令");
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
    bool observed_returned_projection_crossing = false;
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
        if (observed_base_overshoot && transient_base_error < -0.1f &&
            transient_final_error > 0.0f &&
            transient_final_error <= hold_band) {
            observed_returned_projection_crossing = true;
            if (transient.has_command) {
                expect(transient.command.dx_counts * transient_final_error >= 0.0f,
                       "基础点过冲后回到原侧时，整数命令不得背离延迟最终点");
            }
            break;
        }
    }
    expect(observed_base_overshoot &&
               observed_returned_projection_crossing,
           "回归必须覆盖基础点短暂越过保持带后回到原侧、投影点仍过零，基础最大=" +
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

    AimFrame first = make_frame(1, base);
    first.control_at = base + std::chrono::milliseconds(5);
    first.detections = {body(180.0f, 160.0f)};
    aim.process(first);

    AimFrame second = make_frame(
        2, base + std::chrono::milliseconds(10));
    second.control_at = second.captured_at +
        std::chrono::milliseconds(5);
    second.detections = {body(190.0f, 160.0f)};
    const AimResult result = aim.process(second);
    expect(result.has_target && result.target.delay_compensation_active &&
               result.target.delay_compensation_x > 0.0f &&
               result.target.delay_compensated_aim_x >
                   result.target.base_aim_x &&
               result.target.aim_x >= result.target.delay_compensated_aim_x,
           "延迟补偿必须先于 prediction 叠加，且沿确认速度方向生效");
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
    test_base_crossing_releases_integral_smoothly();
    test_integral_releases_on_reversal_and_static_settle();
    test_quantization_residual_cannot_reverse_after_crossing();
    test_delay_projection_crossing_keeps_base_tracking_hold();
    test_control_step_cannot_cross_in_box_aim_point();
    test_delay_compensation_stacks_before_prediction();
    test_short_glide_preserves_base_tracking_hold();
    test_prediction_layer_keeps_base_tracking_hold_continuous();

    Log::shutdown();
    if (failures != 0) {
        std::cerr << "Aim 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Aim 测试全部通过。\n";
    return 0;
}
