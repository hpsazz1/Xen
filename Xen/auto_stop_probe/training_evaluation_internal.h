#ifndef AUTO_STOP_TRAINING_EVALUATION_INTERNAL_H
#define AUTO_STOP_TRAINING_EVALUATION_INTERNAL_H

#include "probe_internal.h"
#include "input_training/input_training.h"

namespace auto_stop_probe_detail {
inline Json training_snapshot_json(const input_training::Snapshot& snapshot, const char* source) {
    Json result{{"source", source}, {"time_domain", "LOCAL_STEADY_RECEIPT"},
        {"success", snapshot.status == input_training::Status::STOPPED},
        {"success_meaning", "ARCHIVE_COMPLETED_ONLY"}, {"samples_present", snapshot.received_events != 0},
        {"physical_validation_passed", false}, {"game_shot_stability", nullptr}, {"settled", nullptr},
        {"status", input_training::status_name(snapshot.status)}, {"error", snapshot.error},
        {"archive", snapshot.directory}, {"received_events", snapshot.received_events},
        {"dropped_events", snapshot.dropped_events}, {"invalid_events", snapshot.invalid_events},
        {"unpaired_edges", snapshot.unpaired_edges}, {"total_timings", snapshot.total_timings},
        {"total_holds", snapshot.total_holds}, {"timings", Json::array()}, {"holds", Json::array()}};
    for (const auto& timing : snapshot.timings) {
        result["timings"].push_back({{"from", std::string(1, timing.from)}, {"to", std::string(1, timing.to)},
            {"delta_ns", timing.delta_ns}, {"grade", input_training::grade_name(timing.grade)},
            {"completed_at_ns", timing.completed_at_ns}, {"epoch", timing.epoch},
            {"release_sequence", timing.release_sequence}, {"press_sequence", timing.press_sequence},
            {"atomic_ambiguous", timing.atomic_ambiguous},
            {"timing_uncertainty_known", timing.timing_uncertainty_known},
            {"uncertainty_crosses_boundary", timing.uncertainty_crosses_boundary}});
    }
    const auto add_hold = [&](const input_training::Hold& hold) {
        // 仅该来源接收事件的观察跨度，不表示真实开枪持续时间或延迟。
        const Json observed_duration = hold.points.empty() ? Json(nullptr) :
            Json(hold.points.back().event.received_at_ns - hold.points.front().event.received_at_ns);
        result["holds"].push_back({{"id", hold.id}, {"end", input_training::hold_end_name(hold.end)},
            {"observed_duration_ns", observed_duration}, {"duration_meaning", "SOURCE_EVENT_OBSERVATION_SPAN"},
            {"event_count", hold.points.size()}, {"complete_received_stream", hold.complete_received_stream},
            {"source_loss_verifiable", hold.source_loss_verifiable},
            {"physical_motion_verified", hold.physical_motion_verified}, {"motion_available", hold.motion_available},
            {"boundary_ambiguous", hold.boundary_ambiguous}});
    };
    for (const auto& hold : snapshot.holds) if (hold) add_hold(*hold);
    if (snapshot.active_hold) add_hold(*snapshot.active_hold);
    result["timings_retained"] = snapshot.timings.size();
    result["holds_retained"] = result["holds"].size();
    result["timings_summary_complete"] = snapshot.timings.size() == snapshot.total_timings;
    result["holds_summary_complete"] = result["holds"].size() == snapshot.total_holds;
    return result;
}

inline Json evaluate_counterpulse_training(const Json& report, const std::filesystem::path& archive) {
    if (!report.contains("commands") || !report["commands"].is_array() || report["commands"].size() > 512)
        throw std::runtime_error("命令训练报告缺少有界 commands 数组");
    auto events = std::make_shared<std::vector<input_training::Event>>();
    bool keyboard_known = false, button_known = false, button_down = false;
    std::uint8_t mask = 0;
    std::int64_t previous_time = 0;
    std::uint64_t invalid_receipts = 0;
    for (const auto& command : report["commands"]) {
        input_training::Event event;
        event.epoch = 1; event.sequence = events->size() + 1;
        // 无效回执只以已有接收水位归档，绝不把该占位时间用于有效边沿。
        event.received_at_ns = previous_time;
        bool valid = false;
        try {
            const auto kind = command.at("kind").get<std::string>();
            const auto integer = [&](const char* key) -> std::int64_t {
                const auto& value = command.at(key);
                if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX))
                    throw std::runtime_error("命令字段必须是有符号范围整数");
                return value.get<std::int64_t>();
            };
            const auto value = integer("value"), disposition = integer("disposition");
            const auto submit = integer("submit_ns"), ack = integer("ack_received_ns");
            const auto completed = integer("backend_completed_ns"), returned = integer("returned_ns");
            valid = submit >= 0 && ack > 0 && submit <= ack && ack <= completed && completed <= returned &&
                ack >= previous_time && ((kind == "wasd" && value >= 0 && value <= 15 &&
                disposition == static_cast<int>(KeyboardDisposition::ACKNOWLEDGED)) ||
                (kind == "left_button" && value >= 0 && value <= 1 &&
                disposition == static_cast<int>(ButtonDisposition::ACKNOWLEDGED)));
            if (valid) {
                previous_time = event.received_at_ns = ack;
                if (kind == "wasd") { mask = static_cast<std::uint8_t>(value); keyboard_known = true; }
                else { button_down = value != 0; button_known = true; }
            }
        } catch (const Json::exception&) { valid = false; }
          catch (const std::runtime_error&) { valid = false; }
        if (!valid) {
            ++invalid_receipts; keyboard_known = button_known = false;
            event.gap = true;
        }
        event.held_mask = mask; event.left_down = button_down;
        event.state_valid = valid && keyboard_known && button_known;
        events->push_back(event);
    }
    input_training::Limits limits;
    // 使用生产合法保留预算；完整原始事件留在档案，摘要显式标明是否截断。
    input_training::Session session;
    if (!session.start(archive, limits, [events] {
        input_training::ReadBatch batch; batch.events.swap(*events); return batch;
    })) throw std::runtime_error("命令训练档案启动失败");
    session.stop();
    const auto snapshot = session.snapshot();
    if (!snapshot) throw std::runtime_error("命令训练档案缺少结果");
    auto result = training_snapshot_json(*snapshot, "COMMAND_ACK");
    result["invalid_receipts"] = invalid_receipts;
    result["time_domain"] = "LOCAL_STEADY_COMMAND_ACK";
    result["timing_uncertainty_ns"] = -1;
    return result;
}
}
#endif
