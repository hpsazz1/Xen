#include "trigger/trigger_worker.h"
#include "log/log.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>

// 仅暂停生产 worker 的决策时间采样返回，不改时间值或生产控制逻辑。
struct InterceptedTriggerClock : TriggerClock {
    static inline std::atomic<bool> armed{false}, entered{false}, released{false}, timed_out{false};
    static inline std::uint_least32_t decision_line = 0;
    static inline TriggerTime sampled{};
    static time_point now(const std::source_location location = std::source_location::current()) noexcept {
        const auto value = TriggerClock::now();
        if (location.line() == decision_line && armed.exchange(false)) {
            sampled = value;
            entered.store(true);
            const auto deadline = value + std::chrono::seconds(2);
            while (!released.load() && TriggerClock::now() < deadline)
                std::this_thread::yield();
            if (!released.load()) timed_out.store(true);
        }
        return value;
    }
};
#define TriggerClock InterceptedTriggerClock
#include "../Xen/trigger/trigger_worker.cpp"
#undef TriggerClock

// 复用纯内存设备和生产接口夹具；本目标不链接另一个 trigger_worker 实现。
#define main existing_trigger_worker_tests_main
#include "trigger_worker_tests.cpp"
#undef main

namespace {
void locate_decision_sample() {
    std::ifstream source(XEN_TRIGGER_WORKER_SOURCE);
    std::string line;
    unsigned number = 0, matches = 0;
    while (std::getline(source, line)) {
        ++number;
        if (line.find("const auto now = TriggerClock::now();") != std::string::npos) {
            InterceptedTriggerClock::decision_line = number;
            ++matches;
        }
    }
    if (matches != 1) throw std::runtime_error("必须定位唯一生产决策时间采样，不允许接缝静默失效");
}
void print_events(const TriggerExecutionLog& log, const char* label) {
    std::cout << "case=" << label << " dropped=" << log.dropped_count << '\n';
    for (const auto& e : log.events)
        std::cout << "event=" << e.sequence << " action=" << static_cast<int>(e.button_action)
            << " reason=" << TriggerReasonName(e.snapshot.reason) << " backend=" << e.backend_called
            << " source_ns=" << e.source_observed_at.time_since_epoch().count()
            << " planned_ns=" << e.planned_at.time_since_epoch().count()
            << " observed_ns=" << e.observed_at.time_since_epoch().count() << '\n';
}
void publish_across_clock_sample() {
    Fixture f;
    f.stop_idle = true;
    if (!f.start(true, 500, 1000, 100, true, true))
        throw std::runtime_error("交错夹具启动并取得释放资格失败");
    f.mouse->held = true;
    f.worker.publish(observation(1));
    expect(until([&] { return f.worker.firing_signal().confirmed_down; }), "交错前内存设备DOWN确认");
    InterceptedTriggerClock::armed.store(true);
    const bool entered = until([] { return InterceptedTriggerClock::entered.load(); });
    auto fresh = observation(2);
    expect(entered && fresh->observed_at > InterceptedTriggerClock::sampled, "新观测在旧now取样后发布");
    std::cout << "sampled_ns=" << InterceptedTriggerClock::sampled.time_since_epoch().count()
        << " published_ns=" << fresh->observed_at.time_since_epoch().count() << '\n';
    f.worker.publish(fresh);
    InterceptedTriggerClock::released.store(true);
    expect(until([&] { return f.mouse->count(false) == 1; }), "当前保持有界结束");
    const auto log = f.worker.execution_log();
    print_events(log, "publish-across-now");
    bool up = false;
    for (const auto& event : log.events) {
        if (event.button_action == TriggerButtonAction::UP && event.backend_called) {
            up = true;
            expect(event.snapshot.reason == TriggerReason::RELEASED, "真实新观测不得被旧now误判未来并中断保持");
        }
    }
    expect(up && !InterceptedTriggerClock::timed_out.load() && log.dropped_count == 0,
        "接缝准时释放且实际UP证据完整");
    f.worker.stop();
    expect(!f.mouse->dirty.load(), "交错夹具无内存按钮债务");
}
void invalid_time_control(bool future) {
    Fixture f;
    expect(f.start(false, 500), "非法时间控制启动");
    f.mouse->held = true;
    auto invalid = observation();
    invalid->observed_at += future ? 1s : -1s;
    f.worker.publish(invalid);
    const auto expected = future ? TriggerReason::TIMING_UNAVAILABLE : TriggerReason::STALE;
    expect(until([&] { return f.worker.snapshot().reason == expected; }), "真正未来或陈旧时间仍须拒绝");
    print_events(f.worker.execution_log(), future ? "future-control" : "stale-control");
    expect(f.mouse->count(true) == 0, "非法时间不产生内存DOWN");
    f.worker.stop();
}
}
int main() {
    try {
        locate_decision_sample();
        publish_across_clock_sample();
        invalid_time_control(true);
        invalid_time_control(false);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        InterceptedTriggerClock::released.store(true);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
