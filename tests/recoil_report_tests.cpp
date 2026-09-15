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
        snapshot.recoil_archive.acquisition_run_id = "one-run";
        snapshot.recoil_archive.directory = "cache/runtime/one-run-recoil-batches";
        snapshot.recoil_archive.available = false;
        snapshot.recoil_archive.error = "write_failed";
        snapshot.recoil_archive.incomplete_batches = 1;
        json = nlohmann::json::parse(recoil_metadata_json(config, snapshot));
        check(json["batch_archive"]["acquisition_run_id"] == "one-run" &&
            json["batch_archive"]["error"] == "write_failed" && json["batch_archive"]["incomplete_batches"] == 1,
            "报告保留独立采集身份与归档故障");
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
        check(trigger["events"][0]["actual_submit_interval_ms"].is_null() &&
              trigger["events"][0]["actual_hold_ms"].is_null() && trigger["physical_shot_count"].is_null(),
              "未知事件不推断时间间隔或实际子弹数量");
        const auto action = [](std::uint64_t sequence, TriggerButtonAction button, int started_ms) {
            TriggerExecutionEvent result;
            result.sequence = sequence; result.button_action = button;
            result.backend_called = true; result.receipt_status = TriggerReceiptStatus::ACKNOWLEDGED;
            result.call_started_at = TriggerTime(std::chrono::milliseconds(started_ms));
            result.protocol_ack_received_at = result.call_started_at + std::chrono::milliseconds(1);
            result.backend_completed_at = result.call_started_at + std::chrono::milliseconds(2);
            result.observed_at = result.backend_completed_at;
            result.snapshot.context.timing_valid = true;
            result.snapshot.context.timing_catalog_revision = 9007199254740993ULL;
            result.snapshot.context.timing_weapon_id = "deagle";
            result.snapshot.context.shot_hold_ms = 60;
            result.snapshot.context.fire_interval_ms = 600;
            return result;
        };
        auto down = action(1,TriggerButtonAction::DOWN,100);
        down.snapshot.estimated_stop_request_id = 4;
        auto up = action(2,TriggerButtonAction::UP,162);
        up.snapshot.stop_request_id = 5;
        up.snapshot.firing_context = up.snapshot.context;
        up.snapshot.firing_context_available = true;
        up.snapshot.context.timing_weapon_id = "ak47";
        up.snapshot.context.timing_catalog_revision = 2;
        up.snapshot.context.shot_hold_ms = 40;
        up.snapshot.context.fire_interval_ms = 300;
        snapshot.trigger_execution_log.events = {down,up,action(3,TriggerButtonAction::DOWN,700)};
        trigger = nlohmann::json::parse(trigger_execution_json(snapshot));
        check(trigger["events"][0]["actual_submit_interval_ms"].is_null() &&
            trigger["events"][1]["actual_hold_ms"] == 61.0 &&
            trigger["events"][2]["actual_submit_interval_ms"] == 600.0,
            "协议ACK在101ms而后端完成在102ms，UP162ms故按住61ms；DOWN提交间隔600ms");
        check(trigger["events"][0]["timing_catalog_revision"] == "9007199254740993" &&
            trigger["events"][0]["timing_weapon_id"] == "deagle" &&
            trigger["events"][0]["shot_hold_ms"] == 60 && trigger["events"][0]["fire_interval_ms"] == 600,
            "点射报告绑定完整资料身份和生效参数");
        check(trigger["events"][1]["timing_weapon_id"] == "deagle" &&
            trigger["events"][1]["timing_catalog_revision"] == "9007199254740993" &&
            trigger["events"][1]["shot_hold_ms"] == 60 && trigger["events"][1]["fire_interval_ms"] == 600,
            "换武器取消时UP沿用DOWN锁存资料，不能归到新武器");
        check(trigger["events"][0]["stop_evidence_kind"] == "ESTIMATED" &&
            trigger["events"][0]["estimated_stop_request_id"] == "4" &&
            trigger["events"][1]["stop_evidence_kind"] == "STRICT_REQUESTED",
            "估计停稳与严格请求分开，不将请求伪装为观察合格");
        auto gap_up = action(5,TriggerButtonAction::UP,762);
        snapshot.trigger_execution_log.events.push_back(gap_up);
        snapshot.trigger_execution_log.events.push_back(action(6,TriggerButtonAction::DOWN,1300));
        trigger = nlohmann::json::parse(trigger_execution_json(snapshot));
        check(trigger["events"][3]["actual_hold_ms"].is_null() &&
            trigger["events"][4]["actual_submit_interval_ms"].is_null(), "序号断档清理两种测量基准");
        auto unknown = action(7,TriggerButtonAction::UP,1362);
        unknown.receipt_status = TriggerReceiptStatus::UNKNOWN;
        snapshot.trigger_execution_log.events.push_back(unknown);
        snapshot.trigger_execution_log.events.push_back(action(8,TriggerButtonAction::DOWN,1900));
        auto incomplete = action(9,TriggerButtonAction::UP,1962);
        incomplete.protocol_ack_received_at = {};
        snapshot.trigger_execution_log.events.push_back(incomplete);
        snapshot.trigger_execution_log.events.push_back(action(10,TriggerButtonAction::DOWN,2500));
        trigger = nlohmann::json::parse(trigger_execution_json(snapshot));
        check(trigger["events"][5]["actual_hold_ms"].is_null() &&
            trigger["events"][6]["actual_submit_interval_ms"].is_null() &&
            trigger["events"][7]["actual_hold_ms"].is_null() &&
            trigger["events"][8]["actual_submit_interval_ms"].is_null(), "未知或不完整回执不跨事件补造测量");
        snapshot.output_arbitration_available = true;
        snapshot.output_arbitration.sources[1].lock_busy = 4;
        const auto arbitration = nlohmann::json::parse(output_arbitration_json(snapshot));
        check(arbitration["sources"]["trigger"]["lock_busy"] == 4 && arbitration["sources"]["aim"]["lock_busy"] == 0,
              "仲裁按模块归因且不伪造Aim拒绝");
        Log::shutdown();
        std::cout << "recoil_report_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
