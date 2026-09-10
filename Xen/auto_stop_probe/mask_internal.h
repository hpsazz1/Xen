#ifndef AUTO_STOP_PROBE_MASK_INTERNAL_H
#define AUTO_STOP_PROBE_MASK_INTERNAL_H
#include "auto_stop_probe/probe_internal.h"

namespace auto_stop_probe_detail {
// 实体操作留出两分钟准备；准备期不发送任何键盘命令，动作仍限五秒。
struct MaskCheckTiming { int wait_ms = 120000; int before_ms = 150; int masked_ms = 1500; int after_ms = 250; int action_limit_ms = 5000; };

inline Json execute_mask_check(IMouseController& mouse, const MaskCheckTiming& timing = {}) {
    CleanupGuard cleanup(mouse);
    Json report = {{"schema_version", 1}, {"mode", "mask_check"}, {"success", false},
        {"raw_monitor_observed", false}, {"physical_effect_observed", false}, {"monitor", Json::array()}, {"commands", Json::array()}};
    std::string failure;
    if (!mouse.set_wasd_event_subscription(true)) failure = "MONITOR_SUBSCRIPTION_FAILED";
    WasdEventCursor cursor;
    std::uint64_t epoch = 0, wasd_sequence = 0, previous_sequence = 0;
    bool baseline_seen = false, w = false, ctrl = false, last_w = false;
    report["activation_basis"] = "observed_w_held";
    report["physical_w_down_time_ns"] = nullptr;
    auto observe = [&](const char* phase) {
        InputSnapshot input;
        const bool polled = mouse.poll_input(input);
        if (polled && !baseline_seen && input.status == InputMonitorStatus::WAITING && std::string_view(phase) == "waiting") return false;
        if (!polled || !input.state_valid || input.status != InputMonitorStatus::READY) {
            failure = "MONITOR_INVALID"; return false;
        }
        WasdEventBatch batch;
        if (!mouse.read_wasd_events(cursor, batch) || batch.gap || !batch.subscribed) {
            failure = "MONITOR_EVENT_GAP"; return false;
        }
        for (std::size_t i = 0; i < batch.count; ++i) {
            if (!batch.events[i].state_valid || (epoch && epoch != batch.events[i].epoch)) {
                failure = "MONITOR_EVENT_INVALID"; return false;
            }
            epoch = batch.events[i].epoch;
            wasd_sequence = batch.events[i].sequence;
        }
        for (std::size_t key = 0; key < input.virtual_keys.size(); ++key) {
            if (input.virtual_keys[key] && key != 'W' && key != 0x11 && key != 0xA2 && key != 0xA3) {
                failure = "KEY_CANCELED"; break;
            }
        }
        if (baseline_seen && input.sequence < previous_sequence) { failure = "MONITOR_SEQUENCE_REGRESSED"; return false; }
        if (baseline_seen && input.sequence == previous_sequence) return false;
        w = input.virtual_keys['W'];
        ctrl = input.virtual_keys[0x11] || input.virtual_keys[0xA2] || input.virtual_keys[0xA3];
        report["monitor"].push_back({{"phase", phase}, {"observed_steady_ns", ns(Clock::now())},
            {"snapshot_sequence", input.sequence}, {"epoch", epoch}, {"wasd_event_sequence", wasd_sequence}, {"w", w}, {"ctrl", ctrl}});
        if (failure.empty() && std::string_view(phase) == "masked" && baseline_seen && w != last_w && epoch != 0)
            report["raw_monitor_observed"] = true;
        last_w = w;
        previous_sequence = input.sequence;
        baseline_seen = true;
        return failure.empty();
    };
    const auto wait_started = Clock::now();
    report["wait_started_steady_ns"] = ns(wait_started);
    bool activated = false;
    while (failure.empty() && Clock::now() - wait_started < std::chrono::milliseconds(timing.wait_ms)) {
        if (observe("waiting")) {
            if (w) { activated = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (failure.empty() && !activated) failure = "W_OBSERVATION_TIMEOUT";
    const auto action_started = Clock::now();
    report["action_started_steady_ns"] = activated ? Json(ns(action_started)) : Json(nullptr);
    auto hold = [&](const char* phase, int duration_ms) {
        const auto until = Clock::now() + std::chrono::milliseconds(duration_ms);
        do {
            observe(phase);
            if (Clock::now() - action_started >= std::chrono::milliseconds(timing.action_limit_ms) && failure.empty()) failure = "ACTION_TIME_LIMIT";
            if (!failure.empty()) return false;
            if (Clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (true);
        return true;
    };
    auto mask = [&](bool enabled) {
        const auto started = Clock::now();
        const auto receipt = mouse.set_wasd_mask(1, enabled);
        Json command = receipt_json(receipt);
        command["masked"] = enabled;
        command["send_started_ns"] = ns(started);
        command["returned_ns"] = ns(Clock::now());
        report["commands"].push_back(command);
        if (receipt.disposition != KeyboardDisposition::ACKNOWLEDGED) failure = "MASK_NOT_ACKNOWLEDGED";
        return failure.empty();
    };
    if (failure.empty() && hold("before_mask", timing.before_ms) && mask(true) && hold("masked", timing.masked_ms)) {
        report["w_held_before_unmask"] = w;
        report["return_observation_requested"] = w;
        if (mask(false)) hold("after_unmask", w ? timing.after_ms : 0);
    }
    report["cleanup"] = receipt_json(cleanup.finish());
    mouse.set_wasd_event_subscription(false);
    if (report["cleanup"]["disposition"] != "ACKNOWLEDGED" && failure.empty()) failure = "CLEANUP_NOT_ACKNOWLEDGED";
    report["failure"] = failure;
    report["success"] = failure.empty();
    report["finished_steady_ns"] = ns(Clock::now());
    return report;
}
}
#endif
