#include "debug/auxiliary_report.h"
#include <nlohmann/json.hpp>

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
    for (const auto& event : log.events) {
        const auto& state = event.snapshot;
        events.push_back({{"sequence",identity(event.sequence)}, {"command_id",identity(state.command_id)},
            {"candidate_id",identity(state.candidate_id)}, {"observation_epoch",identity(state.observation_epoch)},
            {"observation_sequence",identity(state.observation_sequence)}, {"stop_request_id",identity(event.stop_request_id)},
            {"active_stop_request_id",identity(state.stop_request_id)},
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
    return Json{{"schema",1}, {"clock_domain","local_steady"}, {"first_sequence",identity(log.first_sequence)},
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
