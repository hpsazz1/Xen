#ifndef AUTO_STOP_PROBE_SESSION_INTERNAL_H
#define AUTO_STOP_PROBE_SESSION_INTERNAL_H

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "auto_stop_probe/probe_internal.h"

namespace auto_stop_probe_detail {
inline int parse_session_seconds(std::string_view value) {
    const auto seconds = parse_monitor_ready_timeout(value);
    if (seconds < 1 || seconds > 600) throw std::runtime_error("会话时长必须是1至600秒");
    return seconds;
}

inline void write_session_json(const std::filesystem::path& path, const Json& value, bool replace) {
    if (!replace && std::filesystem::exists(path)) throw std::runtime_error("会话证据已存在");
    auto temporary = path;
    temporary += ".writing";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!stream || !(stream << value.dump(2) << '\n').flush()) throw std::runtime_error("会话证据写入失败");
    }
    if (replace && std::filesystem::exists(path)) std::filesystem::remove(path);
    std::filesystem::rename(temporary, path);
}

// 一个设备代际内复用变化式物理快照；每个计划清理键态，整个会话负责最终关闭。
inline Json execute_session(IMouseController& mouse, const std::filesystem::path& directory,
                            int seconds, int ready_timeout_ms) {
    CleanupGuard guard(mouse);
    if (seconds < 1 || seconds > 600) throw std::runtime_error("会话时长超限");
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    const auto status_path = directory / "session.status.json";
    Json status = {{"schema_version", 1}, {"state", "waiting_monitor"}, {"success", false},
        {"started_steady_ns", ns(started)}, {"deadline_steady_ns", ns(deadline)}, {"completed_plans", 0}};
    auto cancellation = [&]() -> std::string {
        if (std::filesystem::exists(directory / "STOP")) return "SESSION_STOP";
        if (Clock::now() >= deadline) return "SESSION_EXPIRED";
        return {};
    };
    MonitorObservation monitor;
    try {
        write_session_json(status_path, status, false);
        const auto readiness = execute(mouse, {}, ready_timeout_ms, false, cancellation, &monitor);
        status["readiness"] = readiness;
        std::string failure = readiness.value("failure", std::string{});
        if (failure.empty()) {
            status["state"] = "ready";
            write_session_json(status_path, status, true);
        }
        for (int sequence = 1; failure.empty();) {
            failure = cancellation();
            if (!failure.empty()) break;
            InputSnapshot input;
            failure = monitor.poll(mouse, input);
            if (!failure.empty()) break;
            std::ostringstream prefix;
            prefix << std::setw(3) << std::setfill('0') << sequence;
            const auto request_path = directory / (prefix.str() + ".plan.json");
            if (!std::filesystem::exists(request_path)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            const auto result_path = directory / (prefix.str() + ".result.json");
            if (std::filesystem::exists(result_path)) { failure = "RESULT_ALREADY_EXISTS"; break; }
            std::ifstream input_file(request_path);
            if (!input_file) throw std::runtime_error("无法读取会话计划");
            const auto plan = parse_plan(Json::parse(input_file));
            const auto result = execute(mouse, plan, 0, false, cancellation, &monitor);
            write_session_json(result_path, result, false);
            failure = result.value("failure", std::string{});
            if (failure.empty()) {
                status["completed_plans"] = sequence++;
                write_session_json(status_path, status, true);
            }
        }
        const auto cleanup = guard.finish();
        status["cleanup"] = receipt_json(cleanup);
        const bool orderly = failure == "SESSION_STOP" || failure == "SESSION_EXPIRED" || failure == "END_CANCELED";
        status["success"] = orderly && cleanup.disposition == KeyboardDisposition::ACKNOWLEDGED;
        status["state"] = status["success"].get<bool>() ? "finished" : "error";
        status["reason"] = failure;
        status["finished_steady_ns"] = ns(Clock::now());
        write_session_json(status_path, status, true);
        return status;
    } catch (...) {
        status["state"] = "error";
        status["reason"] = "SESSION_EXCEPTION";
        status["cleanup"] = receipt_json(guard.finish());
        status["finished_steady_ns"] = ns(Clock::now());
        try { write_session_json(status_path, status, true); } catch (...) {}
        return status;
    }
}
}
#endif
