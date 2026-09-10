#ifndef AUTO_STOP_PROBE_INTERNAL_H
#define AUTO_STOP_PROBE_INTERNAL_H

#include <algorithm>
#include <chrono>
#include <charconv>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "mouse/mouse.h"

namespace auto_stop_probe_detail {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
struct Step { std::uint8_t held_mask; int hold_ms; };

inline int parse_monitor_ready_timeout(std::string_view value) {
    int timeout = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || timeout < 0 || timeout > 30000)
        throw std::runtime_error("监听观测等待必须是0至30000毫秒的整数");
    return timeout;
}

inline std::vector<Step> parse_plan(const Json& input) {
    if (!input.is_object() || input.size() != 1 || !input.contains("steps") ||
        !input["steps"].is_array() || input["steps"].empty() || input["steps"].size() > 64)
        throw std::runtime_error("计划必须仅包含1至64个steps");
    std::vector<Step> result;
    int total = 0;
    for (const auto& item : input["steps"]) {
        if (!item.is_object() || item.size() != 2 || !item.contains("held_mask") ||
            !item.contains("hold_ms") || !item["held_mask"].is_number_integer() ||
            !item["hold_ms"].is_number_integer())
            throw std::runtime_error("步骤只接受整数held_mask和hold_ms");
        const auto mask = item["held_mask"].get<std::int64_t>();
        const auto duration = item["hold_ms"].get<std::int64_t>();
        if (mask < 0 || mask > 15 || (mask & 5) == 5 || (mask & 10) == 10 ||
            duration < 0 || duration > 2000)
            throw std::runtime_error("步骤方向冲突或保持时间超限");
        total += static_cast<int>(duration);
        if (total > 15000) throw std::runtime_error("计划保持总时长超过15秒");
        result.push_back({static_cast<std::uint8_t>(mask), static_cast<int>(duration)});
    }
    return result;
}

inline std::int64_t ns(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
inline const char* disposition_name(KeyboardDisposition value) {
    switch (value) {
    case KeyboardDisposition::ACKNOWLEDGED: return "ACKNOWLEDGED";
    case KeyboardDisposition::APPLICATION_UNKNOWN: return "APPLICATION_UNKNOWN";
    case KeyboardDisposition::REJECTED: return "REJECTED";
    default: return "UNSUPPORTED";
    }
}
inline Json receipt_json(const KeyboardReceipt& receipt) {
    return {{"disposition", disposition_name(receipt.disposition)},
        {"datagram_sent", receipt.datagram_sent},
        {"backend_completed_ns", ns(receipt.backend_completed_at)},
        {"ack_received_ns", ns(receipt.protocol_ack_received_at)}};
}

// 即使报告分配失败或上层抛异常，也先清理本工具拥有的键态再释放设备。
class CleanupGuard {
public:
    explicit CleanupGuard(IMouseController& mouse, bool close_on_exit = true) : mouse_(mouse), close_on_exit_(close_on_exit) {}
    ~CleanupGuard() { if (!finished_) mouse_.cleanup_wasd_keyboard(); if (close_on_exit_) mouse_.close(); }
    KeyboardReceipt finish() { auto result = mouse_.cleanup_wasd_keyboard(); finished_ = true; return result; }
private:
    IMouseController& mouse_;
    bool finished_ = false;
    bool close_on_exit_ = true;
};

// 仅供有界实验工具：未见首报告的WAITING不是全释放，也不阻塞已授权合成计划。
struct MonitorObservation {
    bool ever_ready = false;
    std::string poll(IMouseController& mouse, InputSnapshot& input) {
        if (!mouse.poll_input(input)) return "MONITOR_INVALID";
        if (!ever_ready && input.status == InputMonitorStatus::WAITING && !input.state_valid) return {};
        if (input.status != InputMonitorStatus::READY || !input.state_valid) return "MONITOR_INVALID";
        ever_ready = true;
        return input.virtual_keys[0x23] ? "END_CANCELED" : "";
    }
};

inline Json execute(IMouseController& mouse, const std::vector<Step>& plan, int monitor_ready_timeout_ms = 0,
                    bool close_on_exit = true, const std::function<std::string()>& external_cancel = {},
                    MonitorObservation* shared_monitor = nullptr) {
    CleanupGuard guard(mouse, close_on_exit);
    if (monitor_ready_timeout_ms < 0 || monitor_ready_timeout_ms > 30000)
        throw std::runtime_error("监听准备超时超出范围");
    Json report = {{"schema_version", 1}, {"steps", Json::array()},
        {"physical_effect_observed", false}, {"settled", nullptr}, {"success", false}};
    const auto started = Clock::now();
    report["started_steady_ns"] = ns(started);
    report["ready_started_steady_ns"] = ns(started);
    report["monitor_ready_timeout_ms"] = monitor_ready_timeout_ms;
    std::string failure;
    InputSnapshot input;
    MonitorObservation local_monitor;
    auto& monitor = shared_monitor ? *shared_monitor : local_monitor;
    bool first_poll = true;
    report["monitor_initial_state_known"] = false;
    // 显式等待只是收集首报告的机会；超时不伪造键态，也不要求人为唤醒。
    const auto ready_deadline = started + std::chrono::milliseconds(monitor_ready_timeout_ms);
    while (true) {
        if (external_cancel && !(failure = external_cancel()).empty()) break;
        failure = monitor.poll(mouse, input);
        if (first_poll) { report["monitor_initial_state_known"] = input.status == InputMonitorStatus::READY && input.state_valid; first_poll = false; }
        if (!failure.empty() || monitor.ever_ready || Clock::now() >= ready_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto action_started = Clock::now();
    report["ready_finished_steady_ns"] = ns(action_started);
    report["action_started_steady_ns"] = failure.empty() ? Json(ns(action_started)) : Json(nullptr);
    auto canceled = [&]() {
        if (external_cancel && !(failure = external_cancel()).empty()) return true;
        failure = monitor.poll(mouse, input);
        if (!failure.empty()) return true;
        if (Clock::now() - action_started > std::chrono::seconds(20)) {
            failure = "WALL_TIME_LIMIT"; return true;
        }
        return false;
    };
    auto send_step = [&](const Step& step, const char* phase) {
        if (canceled()) return false;
        const auto send_started = Clock::now();
        if (!report["steps"].empty()) {
            auto& previous = report["steps"].back();
            previous["next_command_started_ns"] = ns(send_started);
            previous["command_interval_ns"] = ns(send_started) - previous["send_started_ns"].get<std::int64_t>();
        }
        const auto receipt = mouse.set_wasd_keyboard(step.held_mask);
        const auto returned = Clock::now();
        Json entry = receipt_json(receipt);
        entry["phase"] = phase;
        entry["held_mask"] = step.held_mask;
        entry["hold_ms"] = step.hold_ms;
        entry["send_started_ns"] = ns(send_started);
        entry["returned_ns"] = ns(returned);
        if (receipt.disposition != KeyboardDisposition::ACKNOWLEDGED) failure = "COMMAND_NOT_ACKNOWLEDGED";
        const auto deadline = send_started + std::chrono::milliseconds(step.hold_ms);
        while (failure.empty() && Clock::now() < deadline) {
            if (canceled()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto ended = Clock::now();
        entry["wait_finished_ns"] = ns(ended);
        entry["actual_call_and_hold_ns"] = ns(ended) - ns(send_started);
        entry["overshoot_ns"] = std::max<std::int64_t>(0, ns(ended) - ns(deadline));
        report["steps"].push_back(std::move(entry));
        return failure.empty();
    };
    if (failure.empty() && send_step({0, 0}, "initial_release")) {
        for (const auto& step : plan) if (!send_step(step, "plan")) break;
        if (failure.empty()) send_step({0, 0}, "final_release");
    }
    const auto cleanup_started = Clock::now();
    if (!report["steps"].empty()) {
        auto& previous = report["steps"].back();
        previous["next_command_started_ns"] = ns(cleanup_started);
        previous["command_interval_ns"] = ns(cleanup_started) - previous["send_started_ns"].get<std::int64_t>();
    }
    report["cleanup"] = receipt_json(guard.finish());
    report["cleanup"]["send_started_ns"] = ns(cleanup_started);
    report["cleanup"]["returned_ns"] = ns(Clock::now());
    if (report["cleanup"]["disposition"] != "ACKNOWLEDGED" && failure.empty()) failure = "CLEANUP_NOT_ACKNOWLEDGED";
    report["failure"] = failure;
    report["monitor_ever_ready"] = monitor.ever_ready;
    report["success"] = failure.empty();
    report["finished_steady_ns"] = ns(Clock::now());
    return report;
}
}
#endif
