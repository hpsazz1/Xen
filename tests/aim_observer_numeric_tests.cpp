#include "aim/aim.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "[失败] " << message << '\n';
    }
}

void test_finite_observer_candidate_and_recovery(int sign) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_delay_compensation = true;
    config.enable_prediction = false;
    config.control_delay_ms = 15.0f;
    config.body_aim_height_ratio = 0.5f;
    config.counts_per_pixel_x = 0.4f;
    config.acquisition_range_percent = 100.0f;
    Aim aim(config);
    const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    const std::array<float, 6> camera_motion{0.0f, -1.0f, 1.2f, 2.0f, 0.0f, 0.0f};
    for (std::size_t index = 0; index < camera_motion.size(); ++index) {
        AimFrame frame;
        frame.sequence = index + 1;
        frame.captured_at = start + std::chrono::milliseconds(index * 8);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        // 放大合法坐标变换，区分“测量合法、真实更新合法”和派生值溢出。
        frame.source_pixels_per_roi_pixel_x = 1e36f;
        frame.observation_epoch = 1;
        frame.lock_active = true;
        const float center = 160.0f + sign * 30.0f;
        frame.detections = {{center - 20.0f, 120.0f, center + 20.0f, 200.0f, 0.9f, 0}};
        frame.background_motion_x = {
            AimBackgroundMotionStatus::VALID, index, index + 1,
            frame.captured_at - std::chrono::milliseconds(8), frame.captured_at,
            1, sign * camera_motion[index], 1.0f, 0.0f, 4};
        const auto result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target,
               "合法大尺度观测必须保留目标与成功状态");
        expect(std::isfinite(result.control.observer_target_velocity_x_counts_per_second) &&
                   std::isfinite(result.control.shaped_x_counts) &&
                   std::isfinite(result.target.aim_x) && std::isfinite(result.target.aim_y),
               "派生溢出与恢复过程中不得污染后续有限状态");
        expect(std::hypot(static_cast<float>(result.command.dx_counts),
                          static_cast<float>(result.command.dy_counts)) <= config.max_counts_per_frame,
               "数值回退仍须保持二维输出上限");
        if (index == 2) {
            expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "真实离散更新仍有限时，不得被另一公式的溢出误拒绝");
        }
        if (index == 3) {
            expect(result.control.background_motion_use_x == AimBackgroundMotionUse::INVALID,
                   "真实派生测量溢出时必须拒绝该来源");
        }
        if (index >= 4) {
            expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "后续有效背景必须恢复消费，不能停留在拒绝状态");
        }
    }
}
void test_zero_observation_requires_complete_command_history() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_prediction = false;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.body_aim_height_ratio = 0.5f;
    config.acquisition_range_percent = 100.0f;
    config.counts_per_pixel_x = 0.4f;
    const auto origin = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (int scenario = 0; scenario < 2; ++scenario) {
        Aim aim(config);
        std::uint64_t sequence = 0;
        const auto process = [&](int milliseconds, float center, bool confirm_zero) {
            AimFrame frame;
            frame.sequence = ++sequence;
            frame.captured_at = origin + std::chrono::milliseconds(milliseconds);
            frame.control_at = frame.captured_at;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160.0f;
            frame.observation_epoch = 1;
            frame.lock_active = true;
            frame.detections = {{center - 20.0f, 120.0f, center + 20.0f, 200.0f, 0.9f, 0}};
            const auto result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target && result.control.evaluated,
                   "命令历史边界必须实际产生有效公有控制帧");
            expect(std::isfinite(result.control.observer_target_velocity_x_counts_per_second),
                   "命令历史边界不得污染observer状态");
            if (confirm_zero) {
                expect(aim.record_backend_completed_command(frame.sequence, frame.control_at, 0, 0),
                       "反事实未执行记录必须由公有完成接口确认");
            }
            return result;
        };
        const auto first = process(0, 180.0f, false);
        expect(first.has_command && first.command.dx_counts != 0,
               "较早发行的A必须确有非零X请求");
        expect(aim.record_backend_completed_command(
                   1, origin + std::chrono::milliseconds(scenario == 0 ? 20 : 100),
                   first.command.dx_counts, first.command.dy_counts),
               "公有接口必须接受A较晚完成的合法记录");
        if (scenario == 0) {
            // 发行顺序A/B/C与完成顺序不同，不能遇到B早于区间就跳过A。
            process(10, 180.0f, true);
            const auto before = process(30, 181.0f, true);
            expect(before.control.pending_net_x_counts == first.command.dx_counts,
                   "完成时间乱序的A仍在30ms查询的15ms库存窗口内，不能被较晚发行的零B遮蔽");
            expect(before.control.execution_unseen_command_x_counts == first.command.dx_counts,
                   "执行P未见库存必须包含完成20ms的A，不能因B已在窗口外提前停止");
            expect(before.control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "真实框位移必须先建立非零运动估计");
            const auto current = process(35, 181.0f, true);
            expect(current.control.delayed_command_x_counts == first.command.dx_counts,
                   "到期命令须按有效完成时间选最新A，不能按发行逆序先返回旧零B");
            expect(current.control.reverse_translation_raw_left_x_roi_pixels == 0.0f &&
                       current.control.reverse_translation_raw_right_x_roi_pixels == 0.0f,
                   "乱序完成回归必须实际覆盖双边零观测");
            expect(current.control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "A的模型事件位于源区间内，不得被误判为无命令而清空估计");
        } else {
            // 逐出记录A的模型事件在115ms；环内查不到A不能证明区间完整。
            for (int milliseconds = 1; milliseconds <= 65; ++milliseconds) {
                process(milliseconds, 180.0f, true);
            }
            expect(sequence > 64, "必须实际超过命令环容量并逐出A");
            const auto before = process(80, 181.0f, true);
            expect(before.control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "逐出记录后必须重新建立非零运动估计");
            expect(process(85, 181.0f, true).control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "尚未跨过逐出记录的时间上界时，不能把未查到当作零命令");
            expect(process(120, 181.0f, true).control.observer_target_velocity_x_counts_per_second > 0.0f,
                   "跨越逐出记录模型事件的区间仍不完整");
            expect(process(125, 181.0f, true).control.observer_target_velocity_x_counts_per_second == 0.0f,
                   "前源时刻已跨过时间上界，完整零区间应能释放旧估计");
        }
    }
}
void test_y_inventory_is_independent_of_completion_order() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_delay_compensation = true;
    config.enable_prediction = false;
    config.control_delay_ms = 15.0f;
    config.body_aim_height_ratio = 0.5f;
    config.acquisition_range_percent = 100.0f;
    config.counts_per_pixel_x = config.counts_per_pixel_y = 0.4f;
    config.deadzone_pixels = 0.0f;
    config.max_counts_per_frame = 1.0f;
    Aim reordered(config), ordered(config);
    const auto origin = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    const auto step = [&](Aim& aim, int sequence, int milliseconds, float center_y) {
        AimFrame frame;
        frame.sequence = sequence;
        frame.captured_at = origin + std::chrono::milliseconds(milliseconds);
        frame.control_at = frame.captured_at;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = 160.0f;
        frame.control_center_y = center_y;
        frame.observation_epoch = 1;
        frame.lock_active = true;
        frame.detections = {{140.0f, 130.0f, 180.0f, 210.0f, 0.9f, 0}};
        const auto result = aim.process(frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target && result.control.evaluated,
               "Y乱序库存必须实际产生有效公有控制帧");
        return result;
    };
    const auto first_reordered = step(reordered, 1, 0, 160.0f);
    const auto first_ordered = step(ordered, 1, 0, 160.0f);
    expect(first_reordered.command.dx_counts == 0 && first_ordered.command.dx_counts == 0 &&
               first_reordered.command.dy_counts == 1 && first_ordered.command.dy_counts == 1,
           "两分支首发必须确有非零Y且X为零");
    expect(reordered.record_backend_completed_command(1, origin + std::chrono::milliseconds(20), 0, 1) &&
               ordered.record_backend_completed_command(1, origin, 0, 0),
           "首发Y和对照零命令必须成功确认");
    const auto second_reordered = step(reordered, 2, 10, 160.0f);
    const auto second_ordered = step(ordered, 2, 10, 160.0f);
    expect(second_reordered.command.dx_counts == 0 && second_ordered.command.dx_counts == 0 &&
               second_reordered.command.dy_counts == 1 && second_ordered.command.dy_counts == 1,
           "两分支第二发行也必须有相同非零Y请求");
    expect(reordered.record_backend_completed_command(2, origin + std::chrono::milliseconds(10), 0, 0) &&
               ordered.record_backend_completed_command(2, origin + std::chrono::milliseconds(20), 0, 1),
           "完成排序不同但有效Y事件相同的记录必须成功确认");
    // 两分支都只有20ms完成的+1Y有效；发行顺序不能改变当前库存支付。
    const auto current_reordered = step(reordered, 3, 30, 169.7f);
    const auto current_ordered = step(ordered, 3, 30, 169.7f);
    expect(current_reordered.target.base_aim_y == current_ordered.target.base_aim_y &&
               current_reordered.target.aim_y == current_ordered.target.aim_y,
           "Y库存对照必须保持同一当前瞄点");
    expect(current_reordered.command.dy_counts == current_ordered.command.dy_counts,
           "相同有效Y历史不得因完成乱序产生不同整数输出");
}
void test_physical_command_model_is_independent_of_feedback_gain() {
    const auto displacement = [](float feedback_gain, float roi_scale) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.enable_delay_compensation = false;
        config.enable_prediction = false;
        config.control_delay_ms = 15.0f;
        config.body_aim_height_ratio = 0.5f;
        config.acquisition_range_percent = 100.0f;
        config.counts_per_pixel_x = feedback_gain;
        config.deadzone_pixels = 0.0f;
        config.max_counts_per_frame = 1.0f;
        Aim aim(config);
        const auto origin = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        for (int index = 0; index < 2; ++index) {
            AimFrame frame;
            frame.sequence = index + 1;
            frame.captured_at = origin + std::chrono::milliseconds(index * 4);
            frame.control_at = frame.captured_at;
            frame.roi_width = frame.roi_height = static_cast<int>(320 * roi_scale);
            frame.control_center_x = frame.control_center_y = 160.0f * roi_scale;
            frame.source_pixels_per_roi_pixel_x = frame.source_pixels_per_roi_pixel_y = 1.0f / roi_scale;
            frame.observation_epoch = 1;
            frame.lock_active = true;
            frame.detections = {{150.0f * roi_scale, 120.0f * roi_scale,
                190.0f * roi_scale, 200.0f * roi_scale, 0.9f, 0}};
            frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
                static_cast<std::uint64_t>(index), frame.sequence,
                frame.captured_at - std::chrono::milliseconds(4), frame.captured_at,
                1, 0.0f, 1.0f, 0.0f, 4};
            const auto result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "反馈比例对照必须逐帧命中合法目标");
            if (index == 0) {
                expect(result.command.dx_counts == 1 && result.command.dy_counts == 0,
                       "不同反馈比例必须先实际发出相同非零X命令");
            } else {
                expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                           result.control.residual_role_x &&
                           result.control.execution_unseen_command_x_counts == 1.0,
                       "源像素预算对照必须覆盖有效背景和同一非空在途库存");
                const float source_error =
                    (result.target.base_aim_x - frame.control_center_x) / roi_scale;
                return source_error - result.control.execution_proportional_x_counts / feedback_gain;
            }
        }
        return 0.0f;
    };
    const float original = displacement(0.425f, 1.0f);
    const float stronger_feedback = displacement(0.85f, 1.0f);
    const float resampled = displacement(0.425f, 0.5f);
    expect(original > 0.0f && stronger_feedback > 0.0f && resampled > 0.0f,
           "三组物理位移预算必须非空");
    expect(std::fabs(original - stronger_feedback) < 0.00001f,
           "调整反馈比例不得改变同一命令的源像素物理位移估算");
    expect(std::fabs(original - resampled) < 0.00001f,
           "同一源画面重采样不得改变物理位移预算");
}

void test_zero_relative_motion_preserves_own_camera_model() {
    for (int sign : {-1, 1}) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.enable_delay_compensation = false;
        config.enable_prediction = true;
        config.control_delay_ms = 0.0f;
        config.body_aim_height_ratio = 0.5f;
        config.acquisition_range_percent = 100.0f;
        config.counts_per_pixel_x = 0.425f;
        config.deadzone_pixels = 0.0f;
        config.max_counts_per_frame = 1.0f;
        Aim aim(config);
        const auto origin = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        int issued_x = 0;
        for (int index = 0; index < 2; ++index) {
            AimFrame frame;
            frame.sequence = index + 1;
            frame.captured_at = origin + std::chrono::milliseconds(index * 4);
            frame.control_at = frame.captured_at;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160.0f;
            frame.observation_epoch = 1;
            frame.lock_active = true;
            const float center_x = 160.0f + sign * 10.0f;
            frame.detections = {{center_x - 20.0f, 120.0f, center_x + 20.0f, 200.0f, 0.9f, 0}};
            const auto result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "相对静止回归必须保留有效同源观测");
            expect(result.target.lead_x == 0.0f && result.target.lead_y == 0.0f &&
                       result.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED,
                   "自身命令模型不得授予独立预测资格");
            if (index == 0) {
                issued_x = result.command.dx_counts;
                expect(issued_x == sign, "两个方向均须先实际发出非零命令");
            } else {
                expect(result.control.reverse_translation_raw_left_x_roi_pixels == 0.0f &&
                           result.control.reverse_translation_raw_right_x_roi_pixels == 0.0f,
                       "必须实际覆盖相邻源观测的双边相对零位移");
                expect(std::fabs(result.control.observer_camera_motion_x_source_pixels +
                           issued_x * 0.5215f) < 0.000001f,
                       "相对静止不得抹掉已有非零命令的相机位移模型");
            }
        }
    }
}
void test_crossing_interval_keeps_observer_update_continuous() {
    const auto observe = [](float left_change) {
        AimConfig config;
        config.min_confirmed_hits = 1;
        config.enable_prediction = false;
        config.enable_delay_compensation = false;
        config.body_aim_height_ratio = 0.5f;
        config.acquisition_range_percent = 100.0f;
        config.counts_per_pixel_x = 0.425f;
        config.deadzone_pixels = 0.0f;
        config.max_counts_per_frame = 12.0f;
        Aim aim(config);
        const auto base = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        float prior_velocity = 0.0f;
        float final_velocity = 0.0f;
        for (int index = 0; index < 3; ++index) {
            AimFrame frame;
            frame.sequence = index + 1;
            frame.captured_at = base + std::chrono::milliseconds(index * 4);
            frame.control_at = frame.captured_at;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160.0f;
            frame.observation_epoch = 1;
            frame.lock_active = true;
            float left = index == 0 ? 140.0f : 142.0f;
            float right = index == 0 ? 180.0f : 182.0f;
            if (index == 2) {
                left += left_change;
                right += 0.1f;
            }
            frame.detections = {{left, 120.0f, right, 200.0f, 0.9f, 0}};
            const auto result = aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_target,
                   "区间端点对照必须逐帧保留有效目标");
            if (result.has_command) {
                // 两支请求明确未应用，保持相同相机输入，不伪造共同非零完成量。
                expect(aim.record_backend_completed_command(frame.sequence, frame.control_at, 0, 0),
                       "区间端点对照必须接受合法零完成量");
            }
            if (index == 1) {
                prior_velocity = result.control.observer_target_velocity_x_counts_per_second;
                expect(prior_velocity > 0.0f, "共同平移必须先建立非零观察状态");
            }
            if (index == 2) {
                final_velocity = result.control.observer_target_velocity_x_counts_per_second;
                expect(result.control.reverse_translation_raw_left_x_roi_pixels * left_change > 0.0f &&
                           result.control.reverse_translation_raw_right_x_roi_pixels > 0.0f,
                       "左端须真正跨零，右端保持同一正上界");
                expect(final_velocity > 0.0f && final_velocity < prior_velocity &&
                           result.has_command && result.command.dx_counts > 0,
                       "有效区间必须约束旧状态，同时保留非空同向输出");
                expect(result.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED &&
                           result.target.lead_x == 0.0f,
                       "模型区间更新不得获得独立预测资格");
            }
        }
        return final_velocity;
    };
    expect(std::fabs(observe(0.001f) - observe(-0.001f)) < 0.01f,
           "同一活动上界不得因另一端跨零而冻结观察状态");
}
void test_source_pair_accumulates_commands_before_latest_zero() {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.body_aim_height_ratio = 0.5f;
    config.counts_per_pixel_x = 0.425f;
    config.max_counts_per_frame = 12.0f;
    config.acquisition_range_percent = 100.0f;
    config.enable_delay_compensation = false;
    config.enable_prediction = false;
    Aim aim(config);
    const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    const auto make = [&](std::uint64_t sequence, int source_ms, int control_ms, float x) {
        AimFrame frame;
        frame.sequence = sequence;
        frame.observation_epoch = 1;
        frame.captured_at = start + std::chrono::milliseconds(source_ms);
        frame.control_at = start + std::chrono::milliseconds(control_ms);
        frame.lock_active = true;
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.detections = {{x - 20.0f, 120.0f, x + 20.0f, 200.0f, 0.9f, 0}};
        return frame;
    };
    const auto warm = make(1, 0, 1, 190.0f);
    const auto warm_result = aim.process(warm);
    expect(warm_result.has_command &&
               aim.record_backend_completed_command(1, warm.control_at, 0, 0),
           "区间相机回归须先建立合法且完整的命令历史");
    const auto first = make(2, 4, 20, 190.0f);
    const auto first_result = aim.process(first);
    const int applied_x = first_result.command.dx_counts;
    expect(first_result.has_target && first_result.has_command && applied_x != 0 &&
               aim.record_backend_completed_command(2, first.control_at,
                   applied_x, first_result.command.dy_counts),
           "相机区间必须包含本实例真正请求并确认的非零命令");
    // 两张图像都早于20ms的实际命令；后续请求以合法零应用确认。
    for (const auto& frame : {make(3, 8, 21, 190.0f), make(4, 12, 22, 190.0f)}) {
        const auto result = aim.process(frame);
        expect(result.has_target && result.has_command &&
                   aim.record_backend_completed_command(frame.sequence, frame.control_at, 0, 0),
               "后续零完成事件不得覆盖区间中更早的非零相机作用");
    }
    // 12→24ms包含20ms非零作用及21/22ms零作用，静止世界只发生镜头平移。
    const auto resumed = make(5, 24, 25, 190.0f - applied_x * 0.5215f);
    const auto result = aim.process(resumed);
    expect(result.has_target && result.control.evaluated && result.has_command &&
               result.command.dx_counts != 0,
           "区间恢复回归必须实际经过非空控制路径");
    expect(std::fabs(result.control.observer_target_velocity_x_counts_per_second) < 0.01f,
           "完整相机区间不得因末条零命令把静止世界误判为运动");
    expect(result.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED &&
               result.target.lead_x == 0.0f,
           "自身命令区间核算不得获得独立预测资格");
}
} // namespace

int main() {
    test_finite_observer_candidate_and_recovery(-1);
    test_finite_observer_candidate_and_recovery(1);
    test_zero_observation_requires_complete_command_history();
    test_y_inventory_is_independent_of_completion_order();
    test_physical_command_model_is_independent_of_feedback_gain();
    test_zero_relative_motion_preserves_own_camera_model();
    test_crossing_interval_keeps_observer_update_continuous();
    test_source_pair_accumulates_commands_before_latest_zero();
    return failures == 0 ? 0 : 1;
}
