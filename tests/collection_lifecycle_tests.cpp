#include "app/input_router.h"
#include "app/report_lifecycle_internal.h"
#include "app/workspace_actions_internal.h"
#include "data_collection/lifecycle_internal.h"
#include "runtime/startup_internal.h"
#include "log/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << std::endl; }
}
template<class Predicate> bool await(Predicate&& predicate) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return predicate();
}
class Gate {
public:
    void enter() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(lock, 30s, [&] { return released_; })) {
            std::cerr << "屏障超时，不能作为通过证据" << std::endl;
            std::_Exit(2);
        }
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] { return entered_; });
    }
    void release() { std::lock_guard lock(mutex_); released_ = true; changed_.notify_all(); }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false, released_ = false;
};
class NoOutputMouse final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool poll_input(InputSnapshot& input) noexcept override { input = {}; return false; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::DISABLED; }
    std::string last_error() const override { return {}; }
    std::atomic<unsigned> moves{0};
};
class HeldCapture final : public ICapture {
public:
    explicit HeldCapture(Gate& gate) : gate_(gate) {}
    bool open() noexcept override { status_ = CaptureStatus::READY; return true; }
    CaptureStatus grab(CapturedFrame&) noexcept override {
        gate_.enter();
        std::this_thread::sleep_for(1ms);
        return CaptureStatus::NO_FRAME;
    }
    void close() noexcept override { status_ = CaptureStatus::CLOSED; }
    CaptureStatus status() const noexcept override { return status_; }
    std::string last_error() const override { return {}; }
private:
    Gate& gate_;
    std::atomic<CaptureStatus> status_{CaptureStatus::CLOSED};
};
struct Fixture {
    Gate capture, writer;
    std::atomic<unsigned> stop_requests{0}, joins{0}, writes{0};
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xen-collection-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    AppConfig config;
    std::shared_ptr<NoOutputMouse> mouse = std::make_shared<NoOutputMouse>();
    std::shared_ptr<data_collection::Collector> collector;
    std::unique_ptr<Runtime> runtime;
    std::unique_ptr<model_workspace::Workspace> workspace;
    model_workspace::Settings settings;
    std::string message;

    explicit Fixture(const char* model) {
        std::filesystem::create_directory(root);
        config.detector.model_path = model;
        config.detector.backend = BackendType::CPU;
        config.capture.backend = CaptureBackend::UDP_MJPEG;
        config.capture.udp_url = "udp://127.0.0.1:19876";
        config.mouse.allow_send_input = false;
        data_collection::detail::LifecycleAdapter adapter;
        adapter.before_write = [&] { ++writes; writer.enter(); };
        adapter.stop_requested = [&] { ++stop_requests; };
        adapter.before_join = [&] {
            if (++joins > 1) {
                // 旧实现在真实交错中到达第二个 join；不要实际执行未定义行为。
                std::cerr << "失败：同一写入线程出现第二个回收者" << std::endl;
                std::_Exit(1);
            }
        };
        collector = data_collection::detail::make_collector_with_lifecycle_adapter(std::move(adapter));
        runtime::detail::StartupAdapter startup;
        startup.create_capture = [&](const CaptureConfig&) { return std::make_unique<HeldCapture>(capture); };
        runtime = runtime::detail::make_runtime_with_startup_adapter(std::move(startup));
        if (!runtime->set_data_collector(collector) || !runtime->start(config, mouse))
            throw std::runtime_error("禁输出真实Runtime启动失败：" + runtime->snapshot().last_error);
        if (!capture.wait_entered()) throw std::runtime_error("真实capture线程未进入屏障");
        workspace = std::make_unique<model_workspace::Workspace>(collector);
        std::string error;
        if (!workspace->initialize(root, settings, error)) throw std::runtime_error(error);
        data_collection::Config collection;
        collection.root_directory = root / "raw";
        collection.model_path = model;
        collection.class_names = {"person"};
        if (!collector->start(collection, error)) throw std::runtime_error(error);
        stop_requests = 0;
    }
    ~Fixture() {
        writer.release(); capture.release();
        if (runtime) runtime->stop();
    }
    void queue(bool fail_write) {
        if (fail_write)
            std::filesystem::create_directory(collector->snapshot().session_directory / "samples/1.pending");
        CapturedFrame frame;
        frame.width = frame.height = 32;
        frame.bgr = cv::Mat(32, 32, CV_8UC3, cv::Scalar(10, 20, 30));
        collector->request_sample();
        if (!await([&] {
                collector->offer(frame, {}, DetectionStatus::SUCCESS, 1);
                return !collector->snapshot().manual_pending;
            }) || !writer.wait_entered()) throw std::runtime_error("真实写入线程未进入屏障");
    }
    bool action(model_workspace::Action action) {
        return app::detail::route_workspace_action(*workspace, action, settings,
            runtime->snapshot(), config.detector, [](DetectorConfig&) { return true; }, message);
    }
    void input_frame() {
        KeyboardPollResult release;
        release.input_healthy = true;
        release.events = {{KeyboardEventType::AIM_HOLD_CHANGED, false}, {KeyboardEventType::EMERGENCY_STOP, true}};
        expect(app::detail::route_input_health(*runtime, release), "下一帧继续路由输入健康");
        const auto result = app::detail::route_keyboard_events(*runtime, release, false);
        const auto snapshot = runtime->snapshot();
        expect(result.emergency_pressed && !snapshot.aim_hold_active && snapshot.emergency_stopped,
               "写盘屏障未释放时，真实输入路由已消费保持键释放与End");
    }
};
void ui_response(Fixture& f, bool fail_write) {
    f.queue(fail_write);
    f.runtime->post_intent({RuntimeIntentType::AIM_HOLD_CHANGED, true});
    auto ui = std::async(std::launch::async, [&] {
        expect(f.action(model_workspace::Action::STOP_COLLECTION), "App停止采集动作被接受");
        f.input_frame();
    });
    const bool responsive = ui.wait_for(1s) == std::future_status::ready;
    expect(responsive, "写盘屏障未释放时App动作必须返回并处理下一帧输入");
    if (responsive) {
        expect(!f.collector->snapshot().active && f.collector->snapshot().draining && f.collector->snapshot().queued == 1,
               "停止即拒绝新样本，排队计数保留在途写入");
        expect(f.action(model_workspace::Action::STOP_COLLECTION), "排空期重复停止幂等");
        expect(!f.action(model_workspace::Action::START_COLLECTION), "排空期禁止重开采集");
        expect(f.workspace->poll().message.find("请先结束采集") != std::string::npos,
               "重开被素材封尾门禁拒绝，不能借别的配置错误冒充通过");
        expect(!f.workspace->execute(model_workspace::Action::TRAIN, f.settings, false, false, ""),
               "排空期禁止离线作业读取未封尾数据");
        expect(f.workspace->poll().message.find("请先结束采集") != std::string::npos,
               "离线作业被素材封尾门禁拒绝，不能借环境未就绪冒充通过");
        expect(!f.runtime->reload_detector(f.config.detector) &&
               f.runtime->snapshot().detector_reload_error.find("请先结束图片采集") != std::string::npos,
               "排空期仍拒绝切换模型");
        f.collector->request_sample();
        expect(!f.collector->snapshot().manual_pending, "封口后拒绝新的人工采样请求");
    }
    f.writer.release(); ui.get();
    expect(await([&] { return !f.workspace->poll().collection.draining; }),
           "正常UI轮询在后台写入退出后完成唯一回收");
    const auto end = f.workspace->poll();
    expect(end.collection.queued == 0 && !end.collection.active && f.writes == 1,
           "排空完成且样本仅处理一次");
    expect(fail_write ? end.collection.saved == 0 && end.collection.dropped == 1 && !end.collection.error.empty()
                      : end.collection.saved == 1 && end.collection.error.empty(),
           "真实写盘成功/失败有正确终态，不虚报保存");
    expect(f.action(model_workspace::Action::STOP_COLLECTION), "完成后重复停止幂等");
}
void concurrent_stop(Fixture& f) {
    f.queue(false);
    auto first = std::async(std::launch::async, [&] { f.collector->stop(); });
    expect(await([&] { return f.joins == 1; }), "首个同步停止拥有写入线程回收权");
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto second = std::async(std::launch::async, [&] { entered.set_value(); f.collector->stop(); });
    entered_future.wait();
    expect(second.wait_for(100ms) == std::future_status::timeout,
           "第二个同步停止等待同一次最终回收，不能提前返回");
    auto poll = std::async(std::launch::async, [&] { return f.workspace->poll(); });
    expect(poll.wait_for(1s) == std::future_status::ready,
           "同步停止等待磁盘时，UI轮询不等待生命周期锁");
    expect(poll.get().collection.draining, "同步回收期间仍显示排空状态");
    f.writer.release(); first.get(); second.get();
    expect(f.joins == 1 && !f.collector->snapshot().draining && f.collector->snapshot().saved == 1,
           "并发同步停止共享一次回收，均在实际完成后返回");
}
void stop_interleave(Fixture& f) {
    f.queue(false);
    bool report_active = false, archive_active = false;
    app::detail::ReportFinalization finalization;
    app::detail::ReportLifecycle lifecycle(report_active, archive_active, finalization);
    std::atomic<unsigned> finishes{0};
    expect(lifecycle.stop(*f.runtime, [&] { ++finishes; return true; }) == app::detail::ReportStopRequest::STARTED,
           "真实ReportLifecycle异步停止Runtime");
    expect(await([&] { return f.runtime->snapshot().state == RuntimeState::STOPPING; }), "Runtime停止等待capture屏障");
    expect(f.collector->snapshot().active, "capture退出前Collector仍然活动，重现实际可达窗口");
    auto ui = std::async(std::launch::async, [&] { return f.action(model_workspace::Action::STOP_COLLECTION); });
    expect(await([&] { return f.stop_requests >= 1; }), "Workspace先请求停止写入");
    f.capture.release();
    expect(await([&] { return f.stop_requests >= 2; }), "Runtime随后抵达同一Collector停止路径");
    expect(ui.wait_for(1s) == std::future_status::ready, "Runtime回收期间UI不等待写盘");
    f.writer.release();
    expect(ui.get(), "UI停止被接受");
    expect(await([&] {
        lifecycle.poll(false, [](bool) {}, [](bool, app::detail::ReportJobKind) {});
        return !lifecycle.busy();
    }), "后台停止和报告结果被主循环回收");
    expect(f.joins == 1 && finishes == 1 && f.runtime->snapshot().state == RuntimeState::STOPPED,
           "真实交错中唯一join、唯一报告收尾、最终STOPPED");
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    LogConfig logging;
    logging.enable_console = logging.enable_file = logging.enable_debug_file = false;
    Log::init(logging);
    try {
        Fixture fixture(argv[1]);
        if (std::string(argv[2]) == "--stop-interleave") stop_interleave(fixture);
        else if (std::string(argv[2]) == "--concurrent-stop") concurrent_stop(fixture);
        else ui_response(fixture, std::string(argv[2]) == "--writer-failure");
        expect(fixture.mouse->moves == 0, "全过程零设备输出");
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << std::endl; }
    Log::shutdown();
    std::cout << "生命周期回归失败数：" << failures << std::endl;
    return failures ? 1 : 0;
}
