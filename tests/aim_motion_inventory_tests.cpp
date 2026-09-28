#include "aim/aim.h"
#include "aim_motion_inventory_fixture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
auto at(std::int64_t ns) {
    return std::chrono::steady_clock::time_point{
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns))};
}
void actual_motion_reversal(bool mirror) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.1f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.1f;
    config.max_center_distance = 0.25f;
    config.switch_margin = 0.2f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 5;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    Aim aim(config);
    const int direction = mirror ? -1 : 1;
    int checked = 0;
    float previous_remaining = 0.0f;
    float window_sum = 0.0f;
    int window_commands = 0;
    int window_frames = 0;
    for (const auto& s : aim_motion_inventory_fixture::kSamples) {
        if (s.observation_clock_reset) aim.reset();
        AimFrame f;
        f.sequence = s.sequence;
        f.captured_at = at(s.source_ns);
        f.control_at = at(s.control_ns);
        f.roi_width = s.roi_width;
        f.roi_height = s.roi_height;
        f.control_center_x = s.center_x;
        f.control_center_y = s.center_y;
        f.source_pixels_per_roi_pixel_x = s.scale_x;
        f.source_pixels_per_roi_pixel_y = s.scale_y;
        f.lock_active = s.lock_active;
        f.observation_epoch = s.epoch;
        for (int i = 0; i < s.count; ++i) {
            auto d = s.detections[static_cast<std::size_t>(i)];
            if (mirror) {
                const float left = d.x1;
                d.x1 = 2.0f * s.center_x - d.x2;
                d.x2 = 2.0f * s.center_x - left;
            }
            f.detections.push_back(d);
        }
        f.background_motion_x = {s.status, s.previous_sequence, s.background_sequence,
            at(s.previous_ns), at(s.background_ns), s.background_epoch,
            mirror ? -s.bg_dx : s.bg_dx, s.response, s.disagreement, s.patches};
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "实际17帧最小输入须正常处理");
        const float scalars[] = {c.proportional_x_counts, c.feedforward_x_counts,
            c.desired_before_reverse_x_counts, c.filtered_x_counts,
            c.modelled_response_x_counts, c.shaped_x_counts,
            c.pending_net_x_counts, c.pending_absolute_x_counts,
            c.residual_before_quantization_x_counts,
            c.history_adjusted_x_counts, c.pre_eligibility_filtered_x_counts,
            c.filtered_integral_x_counts, c.target_motion_maintenance_x_counts,
            c.observer_target_velocity_x_counts_per_second,
            c.error_derivative_x_source_pixels_per_second, c.opening_weight_x};
        for (const float value : scalars)
            expect(std::isfinite(value), "原字段和分阶段维护诊断必须有限");
        expect(c.opening_weight_x >= 0.0f && c.opening_weight_x <= 1.0f,
               "opening权重保持有效范围");
        expect(r.command.dy_counts == s.expected_y, "原向与镜像都必须保持原Y输出");
        expect(std::hypot(static_cast<float>(r.command.dx_counts),
                          static_cast<float>(r.command.dy_counts)) <= 14.0f,
               "维护更新不得突破二维14上限");
        if (r.command.dx_counts != 0)
            expect(r.command.dx_counts * (r.target.base_aim_x - f.control_center_x) > 0.0f,
                   "非零X请求必须朝当前固定base误差方向");
        if (r.has_command)
            expect(aim.record_backend_completed_command(f.sequence, at(s.backend_ns),
                       r.command.dx_counts, r.command.dy_counts),
                   "每个分支只能确认自身产生的命令");
        if (s.sequence >= 2508) {
            const float total = c.shaped_x_counts + c.residual_before_quantization_x_counts;
            expect(c.residual_role_x && !c.filter_reset_x && direction * c.shaped_x_counts > 0.0f,
                   "同源运动换侧后须保持连续残差角色及非空新向净请求");
            if (window_frames == 0) {
                expect(c.residual_before_quantization_x_counts == 0.0f,
                       "新向连续请求不得继承旧方向的舍入余额");
            } else {
                expect(std::fabs(c.residual_before_quantization_x_counts - previous_remaining) < 0.00003f,
                       "新向连续请求须继承前帧合法舍入余额，不得按历史帧号清理");
            }
            expect(r.command.dx_counts == std::lround(total),
                   "连续新向净请求遵循最近整数舍入");
            window_sum += c.shaped_x_counts;
            window_commands += r.command.dx_counts;
            previous_remaining = total - r.command.dx_counts;
            expect(std::fabs(window_sum - window_commands - previous_remaining) < 0.00003f,
                   "连续新向净请求累计守恒，不得丢失亚整数请求");
            ++window_frames;
        }
        if (s.sequence != 2507 && s.sequence != 2508 &&
            s.sequence != 2510 && s.sequence != 2511) continue;
        ++checked;
        const float error = r.target.base_aim_x - f.control_center_x;
        expect(c.evaluated && r.target.matched_observation_valid &&
                   c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
               "核心行为必须建立在有效目标和同源帧对背景上");
        if (s.sequence == 2507) {
            expect(direction * error < 0.0f && std::fabs(error) < config.deadzone_pixels,
                   "前缀必须经过旧误差侧死区，不强制亚像素误差产生整数命令");
        }
        if (s.sequence >= 2510) {
            expect(direction * error > config.deadzone_pixels,
                   "后继必须覆盖死区外新向纠偏，不以静止或零请求规避累计响应");
        }
        if (s.sequence == 2508) {
            expect(direction * error > 0.0f &&
                       direction * c.observer_target_velocity_x_counts_per_second > 0.0f &&
                       direction * c.target_motion_maintenance_x_counts > 0.0f,
                   "换侧后的同源观测须支持非空新向维护，不能因旧库存整项撤销");
        }
        std::cout << "mirror=" << mirror << " seq=" << s.sequence
                  << " error=" << error << " q=" << r.command.dx_counts << '\n';
    }
    expect(checked == 4 && window_frames == 5 && direction * window_commands > 0,
           "必须覆盖旧侧死区及连续五帧新向请求且累计实际响应非零");
    expect(std::fabs(previous_remaining) > 0.00003f,
           "公开reset对照必须从非空旧量化余额开始");
    aim.reset();
    Aim fresh(config);
    int reacquired_frames = 0;
    int new_balance_frames = 0;
    for (int index = 0; index < 12; ++index) {
        AimFrame frame;
        frame.sequence = 2600 + index;
        frame.captured_at = at(aim_motion_inventory_fixture::kSamples.back().source_ns) +
            std::chrono::milliseconds(8 * (index + 1));
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float x = 160.0f + direction * 8.0f;
        frame.detections = {{x - 20.0f, 139.0f, x + 20.0f, 199.0f, .95f, 0}};
        const auto warm = aim.process(frame);
        const auto cold = fresh.process(frame);
        expect(warm.status == cold.status && warm.has_target == cold.has_target &&
                   warm.has_command == cold.has_command &&
                   warm.command.dx_counts == cold.command.dx_counts &&
                   warm.command.dy_counts == cold.command.dy_counts &&
                   warm.control.proportional_x_counts == cold.control.proportional_x_counts &&
                   warm.control.feedforward_x_counts == cold.control.feedforward_x_counts &&
                   warm.control.modelled_response_x_counts == cold.control.modelled_response_x_counts &&
                   warm.control.execution_unseen_command_x_counts == cold.control.execution_unseen_command_x_counts &&
                   warm.control.residual_before_quantization_x_counts == cold.control.residual_before_quantization_x_counts,
               "公开reset必须隔离旧P/R/M、库存和余额，重新获取与全新实例一致");
        if (warm.control.evaluated) {
            if (reacquired_frames++ == 0)
                expect(warm.control.residual_before_quantization_x_counts == 0.0f,
                       "reset后首个有效控制不继承旧舍入余额");
            if (std::fabs(warm.control.residual_before_quantization_x_counts) > .00003f)
                ++new_balance_frames;
        }
        if (warm.has_command)
            expect(aim.record_backend_completed_command(frame.sequence, frame.control_at,
                       warm.command.dx_counts, warm.command.dy_counts), "reset实例只确认自身命令");
        if (cold.has_command)
            expect(fresh.record_backend_completed_command(frame.sequence, frame.control_at,
                       cold.command.dx_counts, cold.command.dy_counts), "全新实例只确认自身命令");
    }
    expect(reacquired_frames > 1 && new_balance_frames > 0,
           "reset后须实际重新获取并形成新余额，不能每帧清理或始终无目标");
}

// 软件回执时相反事实：只延迟自身已生成命令；不代表真实物理闭环。
void delayed_own_receipt_preserves_maintenance(bool mirror) {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};
    config.high_confidence = 0.25f;
    config.low_confidence = 0.1f;
    config.min_confirmed_hits = 2;
    config.max_lost_frames = 8;
    config.min_iou = 0.1f;
    config.max_center_distance = 0.25f;
    config.switch_margin = 0.2f;
    config.switch_confirm_frames = 3;
    config.switch_cooldown_frames = 5;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    config.max_prediction_lead_percent = 35.0f;
    config.predicted_gain = 0.5f;
    Aim aim(config);
    const int direction = mirror ? -1 : 1;
    int delayed_x = 0, delayed_y = 0;
    bool delayed_valid = false;
    int window_frames = 0, window_commands = 0;
    float window_float = 0, first_residual = 0, previous_remaining = 0;
    bool previous_window = false, checked_final_balance = false;
    for (const auto& s : aim_motion_inventory_fixture::kSamples) {
        if (s.observation_clock_reset) aim.reset();
        AimFrame f;
        f.sequence = s.sequence;
        f.captured_at = at(s.source_ns);
        f.control_at = at(s.control_ns);
        f.roi_width = s.roi_width;
        f.roi_height = s.roi_height;
        f.control_center_x = s.center_x;
        f.control_center_y = s.center_y;
        f.source_pixels_per_roi_pixel_x = s.scale_x;
        f.source_pixels_per_roi_pixel_y = s.scale_y;
        f.lock_active = s.lock_active;
        f.observation_epoch = s.epoch;
        for (int i = 0; i < s.count; ++i) {
            auto d = s.detections[static_cast<std::size_t>(i)];
            if (mirror) {
                const float left = d.x1;
                d.x1 = 2.0f * s.center_x - d.x2;
                d.x2 = 2.0f * s.center_x - left;
            }
            f.detections.push_back(d);
        }
        f.background_motion_x = {s.status, s.previous_sequence, s.background_sequence,
            at(s.previous_ns), at(s.background_ns), s.background_epoch,
            mirror ? -s.bg_dx : s.bg_dx, s.response, s.disagreement, s.patches};
        const auto r = aim.process(f);
        const auto& c = r.control;
        expect(r.status == AimStatus::SUCCESS, "公有接口须正常处理");
        // 只延迟本分支已实际生成的2504命令，至2507处理完才报告ACK。
        if (s.sequence == 2504 && r.has_command) {
            delayed_x = r.command.dx_counts;
            delayed_y = r.command.dy_counts;
            delayed_valid = true;
            expect(direction * delayed_x < 0, "本分支必须自己产生旧向命令");
        } else if (r.has_command) {
            expect(aim.record_backend_completed_command(f.sequence, at(s.backend_ns),
                       r.command.dx_counts, r.command.dy_counts), "当前回执匹配自身命令");
        }
        if (s.sequence == 2507) {
            expect(delayed_valid, "必须有可延迟的自身命令");
            expect(aim.record_backend_completed_command(2504, f.control_at, delayed_x, delayed_y),
                   "到期才报告旧命令ACK，不提前注入未来回执");
        }
        const float error = r.target.base_aim_x - f.control_center_x;
        const float old_inventory =
            (c.pending_absolute_x_counts - direction * c.pending_net_x_counts) * 0.5f;
        // 此受控窗口用原死区边界限定纯维护阶段；不要求某一固定位次q非零。
        const bool in_window = direction * error > 0 &&
            std::fabs(error) <= config.deadzone_pixels &&
            direction * c.observer_target_velocity_x_counts_per_second > 0 && old_inventory > 0 &&
            c.background_motion_use_x == AimBackgroundMotionUse::CONSUMED && !c.filter_reset_x;
        if (in_window) {
            if (window_frames == 0) first_residual = c.residual_before_quantization_x_counts;
            if (previous_window)
                expect(std::fabs(c.residual_before_quantization_x_counts - previous_remaining) < 0.00003f,
                       "同向连续请求必须继承上一步舍入余额");
            const float nominal =
                c.observer_target_velocity_x_counts_per_second * c.controller_dt_ms / 1000.0f;
            expect(c.controller_dt_ms > 0 && std::fabs(nominal) < config.max_counts_per_frame,
                   "真实controller dt及未饱和维护前置");
            expect(std::fabs(c.modelled_response_x_counts - nominal) < 0.00003f,
                   "旧向库存不得撤销已观测同向M份额");
            expect(direction * c.shaped_x_counts + 0.00003f >= direction * c.modelled_response_x_counts,
                   "纯维护窗口实际shaped不得取消已接受M");
            const float total = c.shaped_x_counts + c.residual_before_quantization_x_counts;
            expect(r.command.dx_counts == std::lround(total),
                   "每帧遵守最近整数，合法亚半count可为零");
            previous_remaining = total - r.command.dx_counts;
            window_float += c.shaped_x_counts;
            window_commands += r.command.dx_counts;
            ++window_frames;
            expect(std::fabs(first_residual + window_float - window_commands - previous_remaining) < 0.00003f,
                   "累计量化守恒");
        } else if (previous_window && !c.filter_reset_x && direction * c.shaped_x_counts > 0) {
            expect(std::fabs(c.residual_before_quantization_x_counts - previous_remaining) < 0.00003f,
                   "窗口结束余额仍保留到下一合法同向帧");
            checked_final_balance = true;
        }
        previous_window = in_window;
    }
    expect(window_frames >= 2 && checked_final_balance,
           "旧库存同源纯维护窗口必须非空并覆盖累计及后继");
    expect(direction * window_commands > 0,
           "旧库存仍存在时必须已产生累计有效新向响应");
}

// 独立位置职责反例：Y几何固定，首次保Y向量cap可由公开X请求精确还原。
// 此处模拟观测只用于检验软件份额，不能当作改变命令后的物理闭环。
void observed_position_share(bool background, bool mirror,
                             float amplitude, float maximum_counts) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.4f;
    config.smoothing = 0.475f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.enable_prediction = false;
    config.body_aim_height_ratio = 1.0f / 3.0f;
    config.max_counts_per_frame = maximum_counts;
    config.acquisition_range_percent = 100.0f;
    Aim aim(config);
    struct Completed {
        std::chrono::steady_clock::time_point at;
        float x;
    };
    std::vector<Completed> completed;
    int position_frames = 0;
    int capped_position_frames = 0;
    int position_violations = 0;
    int reduced_extra_frames = 0;
    int zero_position_frames = 0;
    int fallback_frames = 0;
    const auto base = at(10000000000LL);
    // 保留原周期输入，再显式建立旧向库存并切到死区外的新位置。
    // 反馈强度变化可以改变周期输入的库存交集，不能因此丢失P保护覆盖。
    for (int index = 0; index < 512; ++index) {
        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(index + 1);
        frame.observation_epoch = 1;
        frame.captured_at = base + std::chrono::microseconds(4167 * index);
        frame.control_at = frame.captured_at + std::chrono::milliseconds(1);
        frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160.0f;
        frame.lock_active = true;
        const float error = (mirror ? -1.0f : 1.0f) * amplitude *
            (index < 480
                ? std::sin(static_cast<float>(index) * 0.20943951f)
                : (index < 496 ? -1.0f : 1.0f / 3.0f));
        frame.detections.push_back({140.0f + error, 140.0f,
            180.0f + error, 200.0f, 0.95f, 0});
        if (background && index > 0) {
            frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
                frame.sequence - 1, frame.sequence,
                frame.captured_at - std::chrono::microseconds(4167),
                frame.captured_at, 1, 0.0f, 0.9f, 0.0f, 2};
        }
        const auto result = aim.process(frame);
        const auto& control = result.control;
        expect(result.status == AimStatus::SUCCESS,
               "位置职责公有输入须正常处理");
        expect(result.command.dy_counts == 0,
               "固定Y几何及零初始积分不得产生Y请求");
        const float request = std::clamp(control.desired_before_reverse_x_counts,
            -maximum_counts, maximum_counts);
        const float actual_position = control.desired_before_reverse_x_counts != 0.0f
            ? control.proportional_x_counts * request /
                control.desired_before_reverse_x_counts : 0.0f;
        const float direction = request > 0.0f ? 1.0f :
            (request < 0.0f ? -1.0f : 0.0f);
        // 只由本测试确认过的命令与公开时间计算库存，不读取Aim私有状态。
        float inventory = 0.0f;
        for (const auto& entry : completed) {
            const float age = std::chrono::duration<float>(
                frame.control_at - entry.at).count();
            if (age >= 0.0f && age < 0.015f)
                inventory += entry.x * (1.0f - age / 0.015f);
        }
        const float opposed = std::max(0.0f, -direction * inventory);
        if (opposed > 0.0001f && std::fabs(request) > 0.0001f) {
            const float original_history = request * std::fabs(request) /
                (std::fabs(request) + opposed);
            const float actual = control.history_adjusted_x_counts;
            const bool fresh = control.background_motion_use_x ==
                AimBackgroundMotionUse::CONSUMED;
            if (fresh && direction * actual_position > 0.0f &&
                direction * (request - actual_position) >= 0.0f) {
                ++position_frames;
                if (std::fabs(control.desired_before_reverse_x_counts) >
                    maximum_counts + 0.0001f)
                    ++capped_position_frames;
                if (direction * actual + 0.0003f < direction * actual_position)
                    ++position_violations;
            }
            if (fresh && direction * (request - actual_position) > 0.0001f &&
                std::fabs(actual) + 0.0001f < std::fabs(request))
                ++reduced_extra_frames;
            if (std::fabs(actual_position) < 0.000001f) {
                ++zero_position_frames;
                expect(std::fabs(actual - original_history) < 0.0003f,
                       "P为零时仍须保持原历史缩减，不能凭新职责增加请求");
            }
            if (!fresh) {
                ++fallback_frames;
                expect(std::fabs(actual - original_history) < 0.0003f,
                       "缺少同帧背景时须保持原历史公式");
            }
        }
        if (result.has_command) {
            const auto completed_at = frame.control_at + std::chrono::microseconds(100);
            expect(aim.record_backend_completed_command(frame.sequence, completed_at,
                       result.command.dx_counts, result.command.dy_counts),
                   "位置职责分支只能记录自身backend命令");
            completed.push_back({completed_at, static_cast<float>(result.command.dx_counts)});
        }
    }
    if (background) {
        expect(position_frames > 0, "必须覆盖fresh反向历史与当前位置份额");
        expect(position_violations == 0, "反向完成历史不得压低当前实际P份额");
        expect(reduced_extra_frames > 0, "非P份额仍须受到历史缩减");
        if (maximum_counts == 2.0f)
            expect(capped_position_frames > 0, "必须覆盖实际cap后的P份额");
        else
            expect(zero_position_frames > 0, "必须覆盖P为零的历史负控");
    } else {
        expect(fallback_frames > 0, "必须实际覆盖无背景回退");
    }
}

}
int main() {
    actual_motion_reversal(false);
    actual_motion_reversal(true);
    delayed_own_receipt_preserves_maintenance(false);
    delayed_own_receipt_preserves_maintenance(true);
    for (const bool mirror : {false, true}) {
        observed_position_share(true, mirror, 12.0f, 14.0f);
        observed_position_share(true, mirror, 60.0f, 2.0f);
        observed_position_share(false, mirror, 12.0f, 14.0f);
    }
    std::cout << "失败数：" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
