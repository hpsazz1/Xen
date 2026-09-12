#include "debug/recoil_report.h"
#include "debug/auxiliary_report.h"
#include "log/log.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        RecoilConfig config; config.enabled = true; config.conditions = "synthetic";
        RuntimeSnapshot snapshot;
        check(trigger_execution_json(snapshot) == "null" && output_arbitration_json(snapshot) == "null", "未取得独立辅助遥测保持null");
        auto json = nlohmann::json::parse(recoil_metadata_json(config, snapshot));
        check(json["schema"] == 1 && json["final"].is_null() && json["execution"].is_null(), "未取得遥测不能输出零成绩");
        snapshot.recoil_telemetry_available = true;
        snapshot.recoil.phase = RecoilPhase::FAULT; snapshot.recoil.faulted = true;
        snapshot.recoil.reason = RecoilReason::UNKNOWN_RECEIPT;
        RecoilExecutionRecord record;
        record.intent.command_id = 1; record.intent.session_id = 2; record.intent.dy_counts = 3;
        record.backend_called = true; record.receipt.command_id = 1; record.receipt.status = RecoilReceiptStatus::UNKNOWN;
        snapshot.recoil_execution_log.records.push_back(record);
        snapshot.recoil_execution_log.dropped_count = 7;
        json = nlohmann::json::parse(recoil_metadata_json(config, snapshot));
        check(json["execution"]["dropped_count"] == 7 && json["execution"]["records"][0]["receipt"] == "UNKNOWN", "未知与覆盖缺口保留");
        check(json["execution"]["records"][0]["completed_at_steady_ns"].is_null() &&
            json["physical_acceptance"].is_null(), "缺时间与未实测不能伪造");
        check(json["execution"]["records"][0]["source_firing_id"].is_null() &&
              json["execution"]["records"][0]["firing_command_interval_ns"].is_null(), "未知开火来源和命令区间不得伪造零值");
        check(json["config"]["conditions"] == "synthetic" && json.dump().find("token") == std::string::npos, "配置绑定且不带认证");
        LogConfig logging; logging.global_level = LogLevel::OFF;
        logging.enable_console = false; logging.enable_file = false; logging.enable_debug_file = false;
        Log::init(logging);
        snapshot.trigger_telemetry_available = true;
        TriggerExecutionEvent event;
        event.sequence = 9007199254740993ULL; event.snapshot.command_id = 7;
        event.backend_called = true; event.receipt_status = TriggerReceiptStatus::UNKNOWN;
        event.rejection_reason = "invalid_receipt_time";
        event.observed_at = TriggerTime(std::chrono::nanoseconds(9007199254740993LL));
        snapshot.trigger_execution_log.events.push_back(event);
        snapshot.trigger_execution_log.first_sequence = event.sequence;
        snapshot.trigger_execution_log.last_sequence = event.sequence;
        snapshot.trigger_execution_log.dropped_count = 3;
        auto trigger = nlohmann::json::parse(trigger_execution_json(snapshot));
        check(trigger["events"][0]["sequence"] == "9007199254740993" &&
              trigger["events"][0]["observed_at_steady_ns"] == "9007199254740993", "独立记录保留64位身份精度");
        check(trigger["dropped_count"] == 3 && trigger["events"][0]["receipt"] == "UNKNOWN" &&
              trigger["events"][0]["backend_completed_at_steady_ns"].is_null(), "Log关闭仍保留未知回执与缺测");
        snapshot.output_arbitration_available = true;
        snapshot.output_arbitration.sources[1].lock_busy = 4;
        const auto arbitration = nlohmann::json::parse(output_arbitration_json(snapshot));
        check(arbitration["sources"]["trigger"]["lock_busy"] == 4 && arbitration["sources"]["aim"]["lock_busy"] == 0,
              "仲裁按模块归因且不伪造Aim拒绝");
        Log::shutdown();
        std::cout << "recoil_report_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
