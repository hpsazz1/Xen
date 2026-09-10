#include "aim/aim.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <string>

using Json = nlohmann::json;

std::chrono::steady_clock::time_point time_point(const Json& value) {
    const auto ns = std::stoll(value.get<std::string>());
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

int main() {
    try {
        std::ifstream input(XEN_AIM_PREDICTION_RECOVERY_FIXTURE);
        if (!input) throw std::runtime_error("无法读取真实预测恢复fixture");
        Json data;
        input >> data;
        const auto& source_config = data.at("aim_config");
        AimConfig config;
        config.person_class_ids = {0, 2};
        config.head_class_ids = {1, 3};
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
        Aim aim(config);
        Json output = {{"schema", 1}, {"input_mode", data.at("input_mode")},
                       {"backend_confirmation", "无Mouse；确认本程序请求，使用本帧原backend完成offset；与原counts逐行核对，变化后不称真实设备重放"},
                       {"rows", Json::array()}};
        for (const auto& row : data.at("rows")) {
            if (row.at("runtime_observation_clock_reset").get<bool>()) aim.reset();
            AimFrame frame;
            frame.sequence = std::stoull(row.at("sequence").get<std::string>());
            frame.captured_at = time_point(row.at("observation_ns"));
            frame.control_at = time_point(row.at("control_ns"));
            frame.roi_width = row.at("roi_width");
            frame.roi_height = row.at("roi_height");
            frame.control_center_x = row.at("center_x");
            frame.control_center_y = row.at("center_y");
            frame.source_pixels_per_roi_pixel_x = row.at("scale_x");
            frame.source_pixels_per_roi_pixel_y = row.at("scale_y");
            frame.lock_active = row.at("lock_active");
            for (const auto& d : row.at("detections")) {
                const auto& b = d.at("box");
                frame.detections.push_back(Detection{b[0], b[1], b[2], b[3], d.at("confidence"), d.at("class_id")});
            }
            if (row.contains("background")) {
                const auto& b = row.at("background");
                frame.observation_epoch = std::stoull(row.at("frame_epoch").get<std::string>());
                auto& bg = frame.background_motion_x;
                bg.status = static_cast<AimBackgroundMotionStatus>(b.at("status_code").get<int>());
                bg.previous_sequence = std::stoull(b.at("previous_sequence").get<std::string>());
                bg.sequence = std::stoull(b.at("sequence").get<std::string>());
                bg.previous_captured_at = time_point(b.at("previous_captured_ns"));
                bg.captured_at = time_point(b.at("captured_ns"));
                bg.observation_epoch = std::stoull(b.at("observation_epoch").get<std::string>());
                bg.dx_roi_pixels = b.at("dx_roi_pixels");
                bg.min_response = b.at("min_response");
                bg.disagreement_roi_pixels = b.at("disagreement_roi_pixels");
                bg.usable_patch_count = b.at("usable_patch_count");
            }
            const AimResult result = aim.process(frame);
            if (result.status != AimStatus::SUCCESS ||
                std::hypot(static_cast<double>(result.command.dx_counts),
                           static_cast<double>(result.command.dy_counts)) >
                    config.max_counts_per_frame + 1e-5) {
                throw std::runtime_error("真实回放必须保持成功状态与二维输出上限");
            }
            if (result.has_target &&
                (!std::isfinite(result.target.base_aim_x) ||
                 !std::isfinite(result.target.base_aim_y) ||
                 !std::isfinite(result.target.aim_x) ||
                 !std::isfinite(result.target.aim_y) ||
                 result.target.base_aim_x < result.target.x1 - 1e-4f ||
                 result.target.base_aim_x > result.target.x2 + 1e-4f ||
                 result.target.base_aim_y < result.target.y1 - 1e-4f ||
                 result.target.base_aim_y > result.target.y2 + 1e-4f)) {
                throw std::runtime_error("基础瞄点必须保持当前目标框内");
            }
            bool confirmed = true;
            if (result.has_command) {
                const auto completed_at = time_point(row.at("backend_ns"));
                confirmed = aim.record_backend_completed_command(frame.sequence, completed_at,
                    frame.lock_active ? result.command.dx_counts : 0,
                    frame.lock_active ? result.command.dy_counts : 0);
            }
            if (!confirmed) throw std::runtime_error("回放请求完成记录失败");
            output["rows"].push_back({
                {"base", {result.target.base_aim_x, result.target.base_aim_y}},
                {"aim", {result.target.aim_x, result.target.aim_y}},
                {"command", {result.command.dx_counts, result.command.dy_counts}}});
        }
        // 测试观察窗口只用于断言，不回灌控制输入或植入状态。
        int independent_positive_frames = 0;
        int recovered_commands = 0;
        for (std::size_t i = 1; i < data.at("rows").size(); ++i) {
            const auto& row = data.at("rows").at(i);
            const auto seq = std::stoull(row.at("sequence").get<std::string>());
            if (seq < 1410 || seq > 1448) continue;
            const auto& previous = data.at("rows").at(i - 1);
            const auto& bg = row.at("background");
            if (bg.at("status_code").get<int>() != 8 ||
                bg.at("previous_sequence") != previous.at("sequence") ||
                bg.at("sequence") != row.at("sequence") ||
                bg.at("captured_ns") != row.at("observation_ns") ||
                bg.at("previous_captured_ns") != previous.at("observation_ns") ||
                bg.at("observation_epoch") != row.at("frame_epoch") ||
                previous.at("frame_epoch") != row.at("frame_epoch")) continue;
            const auto& box = row.at("detections").at(0).at("box");
            const auto& before = previous.at("detections").at(0).at("box");
            const double dx = bg.at("dx_roi_pixels").get<double>();
            const double left = box.at(0).get<double>() - before.at(0).get<double>() - dx;
            const double right = box.at(2).get<double>() - before.at(2).get<double>() - dx;
            const auto& result = output.at("rows").at(i);
            const double base_error = result.at("base").at(0).get<double>() -
                row.at("center_x").get<double>();
            if (left > 0.0 && right > 0.0 && base_error > 0.0) {
                ++independent_positive_frames;
                if (result.at("command").at(0).get<int>() > 0) ++recovered_commands;
                if (result.at("aim").at(0).get<double>() >
                        row.at("center_x").get<double>() &&
                    result.at("command").at(0).get<int>() < 0) {
                    throw std::runtime_error("恢复追踪时不得发出同时背离基础点与公有瞄点的X命令");
                }
            }
        }
        std::uint64_t y_fingerprint = 1469598103934665603ULL;
        for (const auto& row : output.at("rows")) {
            y_fingerprint ^= static_cast<std::uint32_t>(row.at("command").at(1).get<int>());
            y_fingerprint *= 1099511628211ULL;
        }
        const auto baseline_y = std::stoull(data.at("baseline_y_fingerprint").get<std::string>());
        // X请求历史可经二维上限与延迟投影影响Y，此指纹仅观察，不能伪装成硬不变合同。
        std::cout << "真实回放帧=" << output.at("rows").size()
                  << "，独立正向观测=" << independent_positive_frames
                  << "，恢复X命令=" << recovered_commands
                  << "，Y指纹=" << y_fingerprint
                  << "，基线Y指纹=" << baseline_y
                  << "，Y指纹相同=" << (y_fingerprint == baseline_y) << '\n';
        if (independent_positive_frames < 30) {
            std::cerr << "fixture合同失效：独立正向观测不足\n";
            return 3;
        }
        // 偶发一帧追赶不足以解除持续停发；多数有效运动观测应得到纠偏。
        if (recovered_commands * 2 <= independent_positive_frames) {
            std::cerr << "真实红：基础点已偏离且独立世界观测反旧方向，多数有效帧仍无正确X命令\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "失败：" << error.what() << '\n';
        return 1;
    }
}
