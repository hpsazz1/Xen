#include "debug/recoil_report.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        RecoilConfig config; config.enabled = true; config.conditions = "synthetic";
        RuntimeSnapshot snapshot;
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
        check(json["config"]["conditions"] == "synthetic" && json.dump().find("token") == std::string::npos, "配置绑定且不带认证");
        std::cout << "recoil_report_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
