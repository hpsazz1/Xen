#include "debug/auxiliary_report.h"
#include <nlohmann/json.hpp>
#include <limits>

namespace {
using Json = nlohmann::json;
Json stamp(TriggerTime time) {
    if (time == TriggerTime{}) return nullptr;
    // 64位时刻以十进制字符串保存，避免前端浮点解析损失身份。
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}
Json identity(std::uint64_t value) { return value ? Json(std::to_string(value)) : Json(nullptr); }
}

std::string trigger_execution_json(const RuntimeSnapshot& snapshot) {
    if (!snapshot.trigger_telemetry_available) return "null";
    const auto& log = snapshot.trigger_execution_log;
    Json events = Json::array();
    std::uint64_t previous_sequence = 0;
    TriggerTime previous_down_submit{}, active_down_ack{};
    for (const auto& event : log.events) {
        const auto& state = event.snapshot;
        const auto& timing = event.button_action != TriggerButtonAction::NONE && state.firing_context_available ?
            state.firing_context : state.context;
        // 有界窗口首项和序号断档不跨缺失事件配对；未知回执可能改变按钮状态。
        if (previous_sequence == 0 || previous_sequence == std::numeric_limits<std::uint64_t>::max() ||
            event.sequence != previous_sequence + 1) {
            previous_down_submit = {}; active_down_ack = {};
        }
        previous_sequence = event.sequence;
        Json interval = nullptr, hold = nullptr;
        if (event.button_action != TriggerButtonAction::NONE) {
            const bool acknowledged = event.backend_called && event.receipt_status == TriggerReceiptStatus::ACKNOWLEDGED;
            const bool complete = event.call_started_at != TriggerTime{} &&
                event.backend_completed_at >= event.call_started_at &&
                event.protocol_ack_received_at >= event.call_started_at &&
                event.protocol_ack_received_at <= event.backend_completed_at && event.observed_at >= event.backend_completed_at;
            if (acknowledged && complete) {
                if (event.button_action == TriggerButtonAction::DOWN) {
                    if (previous_down_submit != TriggerTime{} && event.call_started_at >= previous_down_submit)
                        interval = std::chrono::duration<double, std::milli>(event.call_started_at - previous_down_submit).count();
                    previous_down_submit = event.call_started_at;
                    active_down_ack = event.protocol_ack_received_at;
                } else {
                    if (active_down_ack != TriggerTime{} && event.call_started_at >= active_down_ack)
                        hold = std::chrono::duration<double, std::milli>(event.call_started_at - active_down_ack).count();
                    active_down_ack = {};
                }
            } else if (event.receipt_status != TriggerReceiptStatus::NOT_SENT) {
                previous_down_submit = {}; active_down_ack = {};
            }
        }
        events.push_back({{"sequence",identity(event.sequence)}, {"command_id",identity(state.command_id)},
            {"candidate_id",identity(state.candidate_id)}, {"observation_epoch",identity(state.observation_epoch)},
            {"observation_sequence",identity(state.observation_sequence)}, {"stop_request_id",identity(event.stop_request_id)},
            {"active_stop_request_id",identity(state.stop_request_id)},
            {"estimated_stop_request_id",identity(state.estimated_stop_request_id)},
            {"stop_not_needed",state.stop_not_needed},
            // strict请求身份不证明图像观察通过；当前快照没有独立的观察合格证据。
            {"stop_evidence_kind",state.estimated_stop_request_id ? "ESTIMATED" :
                (state.stop_request_id || event.stop_request_id) ? "STRICT_REQUESTED" : "none"},
            {"timing_catalog_revision",identity(timing.timing_catalog_revision)},
            {"timing_weapon_id",timing.timing_weapon_id.empty() ? Json(nullptr) : Json(timing.timing_weapon_id)},
            {"shot_hold_ms",timing.timing_valid ? Json(timing.shot_hold_ms) : Json(nullptr)},
            {"fire_interval_ms",timing.timing_valid ? Json(timing.fire_interval_ms) : Json(nullptr)},
            {"actual_submit_interval_ms",interval}, {"actual_hold_ms",hold},
            {"phase",static_cast<int>(state.phase)}, {"reason",TriggerReasonName(state.reason)},
            {"button_action",static_cast<int>(event.button_action)}, {"stop_action",static_cast<int>(event.stop_action)},
            {"button_may_be_down",state.button_may_be_down}, {"faulted",state.faulted},
            {"receipt",event.backend_called ? Json(event.receipt_status == TriggerReceiptStatus::ACKNOWLEDGED ? "ACKNOWLEDGED" :
                event.receipt_status == TriggerReceiptStatus::UNKNOWN ? "UNKNOWN" : "NOT_SENT") : Json(nullptr)},
            {"backend_called",event.backend_called}, {"rejection_reason",event.rejection_reason ? event.rejection_reason : "unknown"},
            {"planned_at_steady_ns",stamp(event.planned_at)}, {"call_started_at_steady_ns",stamp(event.call_started_at)},
            {"backend_completed_at_steady_ns",stamp(event.backend_completed_at)}, {"protocol_ack_received_at_steady_ns",stamp(event.protocol_ack_received_at)},
            {"observed_at_steady_ns",stamp(event.observed_at)}, {"source_observed_at_steady_ns",stamp(event.source_observed_at)},
            {"physical_effect_observed",nullptr}});
    }
    return Json{{"schema",2}, {"clock_domain","local_steady"},
        {"interval_basis","acknowledged_down_submit_to_submit"}, {"hold_basis","down_protocol_ack_to_up_submit"},
        {"physical_shot_count",nullptr}, {"first_sequence",identity(log.first_sequence)},
        {"last_sequence",identity(log.last_sequence)}, {"dropped_count",log.dropped_count}, {"events",events}}.dump();
}

std::string output_arbitration_json(const RuntimeSnapshot& snapshot) {
    if (!snapshot.output_arbitration_available) return "null";
    Json sources = Json::object();
    constexpr const char* names[] = {"aim","trigger","recoil"};
    for (std::size_t i = 0; i < snapshot.output_arbitration.sources.size(); ++i) {
        const auto& counters = snapshot.output_arbitration.sources[i];
        sources[names[i]] = {{"acquired",counters.acquired}, {"lock_busy",counters.lock_busy},
            {"auxiliary_pending",counters.auxiliary_pending}, {"output_fault",counters.output_fault}};
    }
    return Json{{"schema",1}, {"scope","runtime_cumulative"}, {"atomic_snapshot",false}, {"sources",sources}}.dump();
}
