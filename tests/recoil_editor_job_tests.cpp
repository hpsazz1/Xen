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
bool wait_ready(const auto& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return job->ready();
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
    return failures ? 1 : 0;
}
