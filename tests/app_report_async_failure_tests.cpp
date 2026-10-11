#include "app/report_lifecycle_internal.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

namespace fault {
thread_local bool next_allocation = false;
thread_local unsigned hits = 0;
}

// 只拦截本测试EXE当前线程的一次分配；生产std::async和future.get不替换。
void* operator new(std::size_t size) {
    if (std::exchange(fault::next_allocation, false)) {
        ++fault::hits;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace std::chrono_literals;
using app::detail::ReportJobKind;
using app::detail::ReportRestartTarget;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}

// 没有设备、socket或采集；封尾future与主线程回收使用App生产调度器。
struct RuntimeAdapter {
    std::atomic<unsigned> stops = 0, disarms = 0, archive_stops = 0;
    bool throw_snapshot = false, throw_stop = false;
    RuntimeSnapshot snapshot() {
        if (throw_snapshot) throw std::bad_alloc();
        RuntimeSnapshot value;
        value.state = stops ? RuntimeState::STOPPED : RuntimeState::RUNNING;
        return value;
    }
    void post_intent(const RuntimeIntent& intent) {
        if (intent.type == RuntimeIntentType::DISARM_OUTPUT) ++disarms;
    }
    void stop() {
        if (throw_stop) throw std::bad_alloc();
        ++stops;
    }
    void set_recoil_archive(std::optional<RecoilArchiveConfig>) { ++archive_stops; }
};

struct Fixture {
    bool report_active = true, archive_active = false;
    app::detail::ReportFinalization finalization;
    app::detail::ReportLifecycle lifecycle{report_active, archive_active, finalization};
    RuntimeAdapter runtime;
    unsigned collected = 0, failed = 0;
    ReportJobKind last_kind = ReportJobKind::FINISH;
    bool poll() {
        bool escaped = false;
        try {
            lifecycle.poll(true, [](bool) {}, [&](bool success, ReportJobKind kind) {
                ++collected; if (!success) ++failed; last_kind = kind;
            });
        } catch (...) { escaped = true; }
        return escaped;
    }
    void drain() {
        bool escaped = false;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            escaped |= poll();
            if (!lifecycle.busy()) break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        expect(!escaped, "后台异常不得从主线程poll逃逸");
        expect(!lifecycle.busy(), "异常结果必须回收并解除future屏障");
    }
};

void allocation_control() {
    bool caught = false;
    const auto before = fault::hits;
    fault::next_allocation = true;
    try { auto job = std::async(std::launch::async, [] { return true; }); job.get(); }
    catch (const std::bad_alloc&) { caught = true; }
    fault::next_allocation = false;
    expect(caught && fault::hits == before + 1, "控制组确实在原生std::async构造期间抛bad_alloc");
}

void launch_failure(bool stop) {
    Fixture fixture;
    std::atomic<unsigned> work_calls = 0;
    bool escaped = false;
    const auto before = fault::hits;
    fault::next_allocation = true;
    try {
        fixture.lifecycle.request_application_restart([&] {
            if (stop) fixture.lifecycle.stop(fixture.runtime, [&] { ++work_calls; return true; });
            else fixture.lifecycle.finish_async([&] { ++work_calls; return true; });
        });
    } catch (...) { escaped = true; }
    fault::next_allocation = false;
    expect(fault::hits == before + 1, "生产调度路径触发一次真实分配故障");
    expect(!escaped, "后台任务创建失败不得退出App动作路由");
    expect(work_calls == 0 && fixture.runtime.stops == 0, "创建失败不冒充已执行停止或封尾");
    if (stop) expect(fixture.runtime.disarms == 1, "创建停止任务前仍立即请求解除输出");
    expect(fixture.lifecycle.busy(), "创建失败仍保留待回收结果，不能提前重启或退出");
    if (stop) expect(fixture.lifecycle.stop(fixture.runtime, [] { return true; }) ==
                     app::detail::ReportStopRequest::BUSY, "失败尚未回收时重复点击停止不能另建任务");
    fixture.lifecycle.finish_async([&] { ++work_calls; return true; });
    expect(work_calls == 0, "失败尚未回收时新封尾请求不能越过屏障");
    fixture.drain();
    expect(*fixture.lifecycle.error(), "创建故障保留无需分配的可见错误");
    expect(fixture.collected == 1 && fixture.failed == 1, "创建失败向App提交一次明确失败结果");
    expect(fixture.last_kind == (stop ? ReportJobKind::STOP : ReportJobKind::FINISH), "保留失败任务类别");
    expect(fixture.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "创建失败取消待处理重启");
    fixture.poll();
    expect(fixture.collected == 1, "重复poll不重复交付失败");
    fixture.lifecycle.request_application_restart([&] {
        fixture.lifecycle.stop(fixture.runtime, [&] { ++work_calls; return true; });
    });
    fixture.drain();
    expect(fixture.runtime.stops == 1 && work_calls == 1 && fixture.collected == 2 && fixture.failed == 1,
           "同一调度器显式重试后真正停止并成功封尾");
    expect(fixture.lifecycle.take_restart(false) == ReportRestartTarget::APPLICATION &&
           fixture.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "健康重试只恢复一次明确请求的重启");
    expect(!*fixture.lifecycle.error(), "健康重试清除旧创建错误");
}

void worker_failure(bool stop) {
    Fixture fixture;
    std::atomic<unsigned> finishes = 0;
    if (stop) {
        fixture.runtime.throw_stop = true;
        fixture.lifecycle.request_application_restart([&] {
            fixture.lifecycle.stop(fixture.runtime, [&] { ++finishes; return true; });
        });
    } else {
        fixture.runtime.throw_snapshot = true;
        fixture.lifecycle.request_application_restart([&] {
            fixture.lifecycle.finish_async([&] {
                return app::detail::finish_reports_after_archive(fixture.runtime, {}, [&](auto) {
                    ++finishes; return true;
                });
            });
        });
    }
    fixture.drain();
    expect(fixture.collected == 1 && fixture.failed == 1, "任务体异常必须交付失败回调");
    expect(*fixture.lifecycle.error(), "工作异常保留可见错误");
    expect(fixture.lifecycle.take_restart(false) == ReportRestartTarget::NONE, "任务体异常阻止自动重启");
    expect(finishes == 0, "异常前未完成封尾不能冒充完成");
    if (!stop) expect(fixture.runtime.archive_stops == 1, "真实封尾辅助入口确实在归档后快照边界失败");
    fixture.runtime.throw_snapshot = fixture.runtime.throw_stop = false;
    fixture.lifecycle.stop(fixture.runtime, [&] { ++finishes; return true; });
    fixture.drain();
    expect(fixture.collected == 2 && fixture.failed == 1 && fixture.runtime.stops == 1 && finishes == 1,
           "任务体异常之后同实例停止和封尾恢复");
    expect(!*fixture.lifecycle.error(), "健康重试清除旧工作异常");
}
}

int main() {
    allocation_control();
    launch_failure(false); launch_failure(true);
    worker_failure(false); worker_failure(true);
    std::cout << "App异步故障回归失败数: " << failures << '\n';
    return failures ? 1 : 0;
}
