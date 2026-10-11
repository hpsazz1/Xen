#include "recoil/recoil_editor_job_internal.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, exited = false;
};
bool wait_until(const auto& complete) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!complete() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return complete();
}
bool wait_ready(const auto& job) {
    return wait_until([&] { return job->ready(); });
}
}
int main() {
    using Job = recoil_editor_detail::Job<int>;
    int failures = 0;
    auto expect = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    auto gate = std::make_shared<Gate>();
    auto running = Job::start(7, [gate](int& value) {
        std::unique_lock lock(gate->mutex);
        gate->entered = true; gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
        value = 42;
        gate->exited = true; gate->changed.notify_all();
    });
    {
        std::unique_lock lock(gate->mutex);
        expect(gate->changed.wait_for(lock, std::chrono::seconds(3), [&] { return gate->entered; }), "后台任务未开始");
    }
    expect(!running->ready(), "任务等待期间不能发布完成");
    expect(!running->cancel(), "已开始的同步操作不能谎报已取消");
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true; gate->changed.notify_all();
    }
    expect(wait_ready(running), "后台任务没有完成");
    if (running->ready()) expect(running->value == 42 && !running->failed(), "完成发布必须包含完整结果");

    auto failed = Job::start(0, [](int&) { throw std::runtime_error("预期错误"); });
    expect(wait_ready(failed) && failed->failed(), "异常必须成为显式失败终态");

    auto orphan_gate = std::make_shared<Gate>();
    auto orphan = Job::start(0, [orphan_gate](int&) {
        std::unique_lock lock(orphan_gate->mutex);
        orphan_gate->entered = true; orphan_gate->changed.notify_all();
        orphan_gate->changed.wait(lock, [&] { return orphan_gate->release; });
        orphan_gate->exited = true; orphan_gate->changed.notify_all();
    });
    {
        std::unique_lock lock(orphan_gate->mutex);
        expect(orphan_gate->changed.wait_for(lock, std::chrono::seconds(3), [&] { return orphan_gate->entered; }), "销毁测试任务未开始");
    }
    // 后台仍在等待时释放页面句柄，必须立即返回；后台只持有自己的数据。
    orphan.reset();
    {
        std::unique_lock lock(orphan_gate->mutex);
        orphan_gate->release = true; orphan_gate->changed.notify_all();
        expect(orphan_gate->changed.wait_for(lock, std::chrono::seconds(3), [&] { return orphan_gate->exited; }), "释放页面后后台数据生命周期无效");
    }

    const auto check_release = [&](bool throw_operation, bool release_early) {
        using ResourceJob = recoil_editor_detail::Job<std::shared_ptr<int>>;
        auto release_gate = std::make_shared<Gate>();
        auto value = std::make_shared<int>(7);
        auto captured = std::make_shared<int>(35);
        const std::weak_ptr<int> weak_value = value, weak_captured = captured;
        auto tracked = ResourceJob::start(std::move(value),
            [gate = release_gate, captured = std::move(captured), throw_operation](auto& result) {
                std::unique_lock lock(gate->mutex);
                gate->entered = true; gate->changed.notify_all();
                gate->changed.wait(lock, [&] { return gate->release; });
                *result += *captured;
                if (throw_operation) throw std::runtime_error("预期的资源释放测试异常");
            });
        const std::weak_ptr<ResourceJob> weak_job = tracked;
        {
            std::unique_lock lock(release_gate->mutex);
            expect(release_gate->changed.wait_for(lock, std::chrono::seconds(3),
                [&] { return release_gate->entered; }), "资源释放测试任务未开始");
        }
        if (release_early) {
            tracked.reset();
            expect(!weak_job.expired() && !weak_value.expired() && !weak_captured.expired(),
                "调用方提前释放句柄后，运行任务必须保留自身、值和捕获状态");
        }
        {
            std::lock_guard lock(release_gate->mutex);
            release_gate->release = true; release_gate->changed.notify_all();
        }
        if (!release_early) {
            const bool completed = wait_ready(tracked);
            expect(completed, "资源释放测试任务未完成");
            if (completed) {
                expect(tracked->failed() == throw_operation, "资源释放测试的成功和异常终态不符");
                expect(*tracked->value == 42, "资源释放前必须发布已完成的值");
            }
            tracked.reset();
        }
        // FINISHED 早于线程闭包析构；以实际所有权消失判断回收，不能只看 ready 或等待固定时长。
        expect(wait_until([&] { return weak_job.expired() && weak_value.expired() && weak_captured.expired(); }),
            release_early ? "提前释放句柄后，后台退出仍遗留任务、值或捕获状态"
                          : "任务完成且句柄释放后，仍遗留任务、值或捕获状态");
    };
    check_release(false, false);
    check_release(true, false);
    check_release(false, true);
    check_release(true, true);
    return failures ? 1 : 0;
}
