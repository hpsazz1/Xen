#include "aim/aim.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::chrono::steady_clock::time_point time_point(const Json& value) {
    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(std::chrono::nanoseconds(
            std::stoll(value.get<std::string>()))));
}

AimConfig config_from(const Json& data) {
    const auto& source_config = data.at("aim_config");
    AimConfig config;
    config.person_class_ids = source_config.at("person_class_ids").get<std::vector<int>>();
    config.head_class_ids = source_config.at("head_class_ids").get<std::vector<int>>();
    const auto number = [&](const char* name) { return std::stof(source_config.at(name).get<std::string>()); };
    const auto integer = [&](const char* name) { return std::stoi(source_config.at(name).get<std::string>()); };
    const auto boolean = [&](const char* name) { return source_config.at(name).get<std::string>() == "true"; };
    config.high_confidence = number("high_confidence");
    config.low_confidence = number("low_confidence");
    config.min_confirmed_hits = integer("min_confirmed_hits");
    config.max_lost_frames = integer("max_lost_frames");
    config.min_iou = number("min_iou");
    config.max_center_distance = number("max_center_distance");
    config.switch_margin = number("switch_margin");
    config.switch_confirm_frames = integer("switch_confirm_frames");
    config.switch_cooldown_frames = integer("switch_cooldown_frames");
    config.acquisition_range_percent = number("acquisition_range_percent");
    config.body_aim_height_ratio = number("body_aim_height_ratio");
    config.body_aim_range_percent = number("body_aim_range_percent");
    config.deadzone_pixels = number("deadzone_pixels");
    config.smoothing = number("smoothing");
    config.counts_per_pixel_x = number("counts_per_pixel_x");
    config.counts_per_pixel_y = number("counts_per_pixel_y");
    config.max_counts_per_frame = number("max_counts_per_frame");
    config.enable_delay_compensation = boolean("enable_delay_compensation");
    config.control_delay_ms = number("control_delay_ms");
    config.max_delay_compensation_ms = number("max_delay_compensation_ms");
    config.max_delay_compensation_percent = number("max_delay_compensation_percent");
    config.enable_prediction = boolean("enable_prediction");
    config.max_prediction_lead_percent = number("max_prediction_lead_percent");
    config.predicted_gain = number("predicted_gain");

    return config;
}

AimFrame frame_from(const Json& row) {
    AimFrame frame;
    frame.sequence = std::stoull(row.at("sequence").get<std::string>());
    frame.captured_at = time_point(row.at("observation_ns"));
    frame.control_at = time_point(row.at("control_ns"));
    frame.roi_width = row.at("roi_width"); frame.roi_height = row.at("roi_height");
    frame.control_center_x = row.at("center_x"); frame.control_center_y = row.at("center_y");
    frame.source_pixels_per_roi_pixel_x = row.at("scale_x");
    frame.source_pixels_per_roi_pixel_y = row.at("scale_y");
    frame.lock_active = row.at("lock_active");
    frame.observation_epoch = std::stoull(row.at("frame_epoch").get<std::string>());
    for (const auto& detection : row.at("detections")) {
        const auto& box = detection.at("box");
        frame.detections.push_back(Detection{box[0], box[1], box[2], box[3],
            detection.at("confidence"), detection.at("class_id")});
    }
    const auto& source = row.at("background");
    auto& bg = frame.background_motion_x;
    bg.status = static_cast<AimBackgroundMotionStatus>(source.at("status_code").get<int>());
    bg.previous_sequence = std::stoull(source.at("previous_sequence").get<std::string>());
    bg.sequence = std::stoull(source.at("sequence").get<std::string>());
    bg.previous_captured_at = time_point(source.at("previous_captured_ns"));
    bg.captured_at = time_point(source.at("captured_ns"));
    bg.observation_epoch = std::stoull(source.at("observation_epoch").get<std::string>());
    bg.dx_roi_pixels = source.at("dx_roi_pixels");
    bg.min_response = source.at("min_response");
    bg.disagreement_roi_pixels = source.at("disagreement_roi_pixels");
    bg.usable_patch_count = source.at("usable_patch_count");
    return frame;
}

float offset_x(const AimResult& result) { return result.target.aim_x - result.target.base_aim_x; }
float offset_y(const AimResult& result) { return result.target.aim_y - result.target.base_aim_y; }
float offset(const AimResult& result) { return std::hypot(offset_x(result), offset_y(result)); }

AimResult step(Aim& aim, const AimConfig& config, const AimFrame& frame,
               std::chrono::steady_clock::time_point completed_at, bool sent, bool valid) {
    const auto result = aim.process(frame);
    expect(result.status == AimStatus::SUCCESS, "来源失效回放必须成功");
    expect(std::hypot(static_cast<float>(result.command.dx_counts),
                      static_cast<float>(result.command.dy_counts)) <=
               config.max_counts_per_frame + .001f, "来源失效回放不得超二维输出上限");
    if (result.has_target) {
        const auto& t = result.target;
        expect(std::isfinite(t.aim_x) && std::isfinite(t.aim_y) &&
                   std::isfinite(t.base_aim_x) && std::isfinite(t.base_aim_y),
               "来源失效公开点必须有限");
        expect(t.base_aim_x >= t.x1 - .001f && t.base_aim_x <= t.x2 + .001f &&
                   t.base_aim_y >= t.y1 - .001f && t.base_aim_y <= t.y2 + .001f,
               "基础点必须留在当前目标框内");
    }
    if (result.has_command && valid) {
        expect(aim.record_backend_completed_command(frame.sequence, completed_at,
                   sent ? result.command.dx_counts : 0, sent ? result.command.dy_counts : 0),
               "公开接口回执本次请求必须成功");
    }
    return result;
}

AimResult replay_row(Aim& aim, const AimConfig& config, const Json& row) {
    if (row.at("runtime_observation_clock_reset").get<bool>()) aim.reset();
    return step(aim, config, frame_from(row), time_point(row.at("backend_ns")),
        row.at("mouse_sent"), row.at("backend_timing_valid"));
}

void expect_slew(const AimResult& previous, const AimResult& current, float dt) {
    const float diagonal = std::hypot(current.target.x2 - current.target.x1,
                                      current.target.y2 - current.target.y1);
    expect(std::hypot(offset_x(current) - offset_x(previous),
                      offset_y(current) - offset_y(previous)) <= diagonal * 1.5f * dt + .001f,
           "来源失效/恢复不能绕过原1.5对角线每秒公开offset slew");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string selection = argc > 1 ? argv[1] : "all";
        expect(selection == "all" || selection == "--independent" || selection == "--source-loss",
               "未知回归选择参数");
        if (selection != "--independent") {
            std::ifstream input(XEN_AIM_PREDICTION_SOURCE_LOSS_FIXTURE);
            expect(static_cast<bool>(input), "无法读取来源失效fixture");
            Json data; input >> data;
            const AimConfig config = config_from(data);
            Aim aim(config);
            AimResult previous{}, current{};
            for (const auto& row : data.at("rows")) {
                previous = current;
                current = replay_row(aim, config, row);
            }
            expect(previous.has_target && current.has_target && offset(previous) > .001f,
                   "失效前必须经公开接口建立非零offset，不能关闭预测求绿");
            expect(previous.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED &&
                       current.control.background_motion_use_x == AimBackgroundMotionUse::INVALID,
                   "fixture必须覆盖可靠来源到真实无效来源");
            const auto& rows = data.at("rows");
            const auto dt = std::chrono::duration<float>(time_point(rows.back().at("observation_ns")) -
                time_point(rows.at(rows.size() - 2).at("observation_ns"))).count();
            expect_slew(previous, current, dt);

            // 原始后续观察检验来源恢复；不人为提供旧世界运动或要求无来源强预测。
            Aim recovery(config);
            for (const auto& row : rows) current = replay_row(recovery, config, row);
            auto before_at = time_point(rows.back().at("observation_ns"));
            int consumed = 0, active = 0, regenerated = 0;
            for (const auto& row : data.at("recovery_rows")) {
                previous = current;
                current = replay_row(recovery, config, row);
                const auto at = time_point(row.at("observation_ns"));
                expect_slew(previous, current, std::chrono::duration<float>(at - before_at).count());
                before_at = at;
                if (current.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED) {
                    ++consumed;
                    if (offset(current) > .001f) ++active;
                    if (offset(current) > offset(previous) + .001f) ++regenerated;
                }
            }
            expect(consumed > 0 && active > 0 && regenerated > 0,
                   "实际来源恢复后必须重新增长预测，不能仅用退回中的残余求绿");

            // 生命周期负例均从同一真实活动前缀开始，只改变明确的公开输入事件。
            for (int scenario = 0; scenario < 4; ++scenario) {
                Aim lifecycle(config);
                for (std::size_t i = 0; i + 1 < rows.size(); ++i)
                    current = replay_row(lifecycle, config, rows.at(i));
                expect(offset(current) > .001f, "生命周期负例必须先有活动offset");
                auto frame = frame_from(rows.at(rows.size() - 2));
                const auto old_track = current.target.track_id;
                bool changed_target = false;
                for (int i = 0; i < 120; ++i) {
                    previous = current;
                    ++frame.sequence;
                    frame.captured_at += std::chrono::milliseconds(5);
                    frame.control_at += std::chrono::milliseconds(5);
                    frame.background_motion_x = {};
                    if (scenario == 1) frame.lock_active = false;
                    if (scenario == 2) ++frame.observation_epoch;
                    if (scenario == 3) {
                        // 先消失超过max_lost_frames，再用原检测重新获取新身份。
                        if (i <= config.max_lost_frames + 2) frame.detections.clear();
                        else frame.detections = frame_from(rows.back()).detections;
                    }
                    current = step(lifecycle, config, frame, frame.control_at +
                        std::chrono::milliseconds(1), frame.lock_active, true);
                    if (scenario == 0 && current.has_target) expect_slew(previous, current, .005f);
                    if (scenario == 1 || scenario == 2)
                        expect(!current.has_target || offset(current) <= .001f,
                               "松锁/epoch改变不能带入旧公开offset");
                    if (scenario == 3 && current.has_target && current.target.track_id != old_track) {
                        changed_target = true;
                        expect(offset(current) <= .001f, "新目标无来源不得继承旧offset");
                    }
                }
                expect(!current.has_target || offset(current) <= .001f,
                       "持续无来源必须收回基础点，不能永久保留旧offset");
                if (scenario == 3) expect(changed_target, "换目标负例必须实际取得新身份");
            }
            std::cout << "来源失效40帧、实际恢复、持续缺源和三类生命周期负例通过\n";
        }
        if (selection == "--source-loss") return 0;
        std::ifstream independent_input(XEN_AIM_PREDICTION_INDEPENDENT_FIXTURE);
        expect(static_cast<bool>(independent_input), "无法读取独立预测fixture");
        Json independent_data; independent_input >> independent_data;
        const auto independent_config = config_from(independent_data);
        expect(independent_config.enable_prediction && !independent_config.enable_delay_compensation,
               "独立预测fixture必须仅开启预测");
        Aim independent(independent_config);
        const auto& independent_rows = independent_data.at("rows");
        const auto established_from = time_point(independent_rows.at(1).at("observation_ns"));
        int opportunities = 0, independent_active = 0;
        for (std::size_t i = 0; i < independent_rows.size(); ++i) {
            const auto& row = independent_rows.at(i);
            const auto result = replay_row(independent, independent_config, row);
            if (i == 0) continue;
            const auto frame = frame_from(row);
            const auto before = frame_from(independent_rows.at(i - 1));
            const auto& bg = frame.background_motion_x;
            expect(!frame.detections.empty() && !before.detections.empty(),
                   "独立预测窗口必须包含真实共同边");
            const float left = frame.detections[0].x1 - before.detections[0].x1 - bg.dx_roi_pixels;
            const float right = frame.detections[0].x2 - before.detections[0].x2 - bg.dx_roi_pixels;
            const int direction = left > 0 && right > 0 ? 1 : left < 0 && right < 0 ? -1 : 0;
            expect(bg.status == AimBackgroundMotionStatus::VALID && direction != 0 &&
                       bg.previous_sequence == before.sequence && bg.sequence == frame.sequence &&
                       bg.previous_captured_at == before.captured_at && bg.captured_at == frame.captured_at &&
                       bg.observation_epoch == frame.observation_epoch &&
                       before.observation_epoch == frame.observation_epoch && frame.lock_active,
                   "独立预测窗口必须维持同帧对与可靠同向运动，不能只看开关");
            if (frame.captured_at - established_from < std::chrono::milliseconds(150)) continue;
            expect(result.has_target && result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "建立时长后的实际窗口必须被生产消费");
            ++opportunities;
            if (direction * offset_x(result) > .001f) ++independent_active;
        }
        std::cout << "独立预测机会="
                  << opportunities << "，活动=" << independent_active << '\n';
        // 与现有真实恢复回归一致：偶发一帧不代表持续运动已恢复预测。
        expect(opportunities > 0 && independent_active * 2 > opportunities,
               "仅预测的持续可靠运动必须在多数活动机会产生同向lead");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "失败：" << error.what() << '\n';
        return 1;
    }
}
