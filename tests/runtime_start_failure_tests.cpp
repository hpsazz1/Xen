#include "runtime/runtime.h"
#include "runtime/startup_internal.h"
#include "log/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

struct CaptureEvidence {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned grabs = 0;
    std::atomic<unsigned> opened{0}, closed{0}, destroyed{0};
    int roi_width = 0;

    bool wait_for_grabs(unsigned count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return grabs >= count; });
    }
};

// 仅交付 NO_FRAME；真实 Runtime/Detector 会启动，但不读取桌面、网络或设备。
class NoFrameCapture final : public ICapture {
public:
    explicit NoFrameCapture(std::shared_ptr<CaptureEvidence> evidence)
        : evidence_(std::move(evidence)) {}
    ~NoFrameCapture() override { ++evidence_->destroyed; }
    bool open() noexcept override {
        ++evidence_->opened; status_ = CaptureStatus::READY; return true;
    }
    CaptureStatus grab(CapturedFrame&) noexcept override {
        { std::lock_guard lock(evidence_->mutex); ++evidence_->grabs; }
        evidence_->changed.notify_all();
        std::this_thread::sleep_for(1ms);
        return CaptureStatus::NO_FRAME;
    }
    void close() noexcept override { ++evidence_->closed; status_ = CaptureStatus::CLOSED; }
    CaptureStatus status() const noexcept override { return status_; }
    std::string last_error() const override { return {}; }
private:
    std::shared_ptr<CaptureEvidence> evidence_;
    std::atomic<CaptureStatus> status_{CaptureStatus::CLOSED};
};

// 只提供订阅与回调观察，不连接真实后端，也不声明已取得生产输出所有权。
class NoOutputMouse final : public IMouseController {
public:
    bool open() noexcept override { ++opened; return true; }
    void close() noexcept override { ++closed; }
    MouseStatus status() const noexcept override { return MouseStatus::DISABLED; }
    std::string last_error() const override { return {}; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool poll_input(InputSnapshot& value) noexcept override {
        value = {}; ++polled; return false;
    }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_mask(std::uint8_t, bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_space_key(bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_left_ctrl_key(bool) noexcept override { ++outputs; return {}; }
    ButtonReceipt set_left_button(bool) noexcept override { ++outputs; return {}; }
    ButtonReceipt set_right_button(bool) noexcept override { ++outputs; return {}; }
    bool set_movement_report_subscription(bool enabled) noexcept override {
        std::lock_guard lock(mutex);
        subscribed = enabled;
        if (enabled) ++subscriptions; else ++unsubscriptions;
        changed.notify_all();
        return true;
    }
    bool read_movement_reports(InputReportCursor&, InputReportBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        ++reads;
        batch = {};
        batch.subscribed = subscribed;
        batch.status = InputMonitorStatus::WAITING;
        changed.notify_all();
        return true;
    }
    bool wait_for_reads(unsigned count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return reads >= count; });
    }
    bool released() {
        std::lock_guard lock(mutex);
        return subscriptions == 1 && unsubscriptions == 1 && !subscribed;
    }
    unsigned read_count() { std::lock_guard lock(mutex); return reads; }

    std::atomic<unsigned> outputs{0}, opened{0}, closed{0}, polled{0};
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool subscribed = false;
    unsigned subscriptions = 0, unsubscriptions = 0, reads = 0;
};

struct ThreadEvidence {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned active = 0, completed = 0;
    unsigned attempts = 0, injected = 0;
    unsigned fail_at = 0;

    void entered() {
        std::lock_guard lock(mutex); ++active; changed.notify_all();
    }
    void exited() {
        std::lock_guard lock(mutex); --active; ++completed; changed.notify_all();
    }
    bool wait_for_active(unsigned count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return active == count; });
    }
    unsigned active_count() { std::lock_guard lock(mutex); return active; }
};

void check_failure(unsigned fail_at, const AppConfig& baseline) {
    const std::string label = "第" + std::to_string(fail_at) + "个顶层线程构造失败：";
    auto threads = std::make_shared<ThreadEvidence>();
    threads->fail_at = fail_at;
    auto old_mouse = std::make_shared<NoOutputMouse>();
    std::vector<std::shared_ptr<CaptureEvidence>> captures;
    runtime::detail::StartupAdapter adapter;
    adapter.create_capture = [&](const CaptureConfig& config) {
        auto evidence = std::make_shared<CaptureEvidence>();
        evidence->roi_width = config.roi_width;
        captures.push_back(evidence);
        return std::make_unique<NoFrameCapture>(std::move(evidence));
    };
    adapter.create_thread = [&, threads](std::function<void()> task) -> std::thread {
        if (++threads->attempts == threads->fail_at) {
            // 在故障点前确认子worker确已读取订阅；第二次还确认首个顶层线程已执行。
            expect(old_mouse->wait_for_reads(1), label + "真实身法子worker必须已经运行");
            if (fail_at == 2)
                expect(captures.back()->wait_for_grabs(1), label + "首个顶层采集线程必须已经执行");
            ++threads->injected;
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                "合成顶层线程构造失败");
        }
        return std::thread([task = std::move(task), threads] {
            threads->entered();
            task();
            threads->exited();
        });
    };
    auto runtime = runtime::detail::make_runtime_with_startup_adapter(std::move(adapter));
    const bool started = runtime->start(baseline, old_mouse);
    const auto failed = runtime->snapshot();
    if (threads->injected != 1)
        std::cerr << "[前置失败] " << label << "state=" << static_cast<int>(failed.state)
                  << "，thread_attempts=" << threads->attempts
                  << "，last_error=" << failed.last_error << '\n';
    expect(!started && threads->injected == 1 && threads->attempts == fail_at,
        label + "必须命中指定构造点，不能用模块初始化失败替代");
    expect(failed.state == RuntimeState::FAILED && !failed.last_error.empty() &&
        failed.emergency_stopped && !failed.output_armed && !failed.output_allowed_by_config,
        label + "必须保留FAILED诊断、急停和禁输出状态");
    const bool capture_released = captures.size() == 1 && captures.front()->opened == 1 &&
        captures.front()->closed == 1 && captures.front()->destroyed == 1;
    const bool worker_released = old_mouse->released() && old_mouse.use_count() == 1;
    expect(capture_released, label + "start返回前必须关闭并销毁已打开的Capture");
    expect(worker_released, label + "start返回前必须停止子worker、解除订阅并释放共享设备引用");
    expect(threads->active_count() == 0, label + "start返回前已创建的顶层线程必须完成");
    expect(old_mouse->outputs == 0 && old_mouse->opened == 0 && old_mouse->closed == 0,
        label + "不得输出或擅自开关调用者持有的共享设备");
    if (!capture_released || !worker_released) {
        // 红测先记录精确缺陷，再显式停止；不让旧worker与换配置的写操作产生数据竞争。
        runtime->stop();
        return;
    }

    const auto old_reads = old_mouse->read_count();
    const auto old_polls = old_mouse->polled.load();
    threads->fail_at = 0;
    AppConfig replacement = baseline;
    replacement.movement.enabled = false;
    replacement.capture.roi_width = baseline.capture.roi_width + 16;
    auto new_mouse = std::make_shared<NoOutputMouse>();
    expect(runtime->start(replacement, new_mouse), label + "无需显式stop即可用不同配置与设备重启");
    expect(captures.size() == 2 && captures.back()->roi_width == replacement.capture.roi_width,
        label + "重启必须创建新配置的Capture");
    expect(threads->wait_for_active(2) && captures.back()->wait_for_grabs(4),
        label + "新一轮两个顶层线程必须真实推进");
    const auto restarted = runtime->snapshot();
    expect(restarted.state == RuntimeState::RUNNING && restarted.last_error.empty() &&
        restarted.provider == "CPUExecutionProvider" && !restarted.movement_available && !restarted.output_armed,
        label + "新会话应清除旧错误且不沿用已关闭的身法能力");
    expect(old_mouse->released() && old_mouse->read_count() == old_reads && old_mouse->polled == old_polls &&
        old_mouse.use_count() == 1, label + "旧设备的订阅与回调不得跨越直接重启");
    runtime->stop();
    expect(threads->active_count() == 0 && captures.back()->closed == 1 && captures.back()->destroyed == 1 &&
        new_mouse.use_count() == 1, label + "重启后的显式停止必须完整回收新一轮资源");
    expect(old_mouse->outputs == 0 && new_mouse->outputs == 0 && new_mouse->opened == 0 && new_mouse->closed == 0,
        label + "整个回归只允许合成订阅，不允许键鼠输出或共享设备开关");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "用法：runtime_start_failure_tests <模型路径>\n";
        return 2;
    }
    LogConfig log_config;
    log_config.enable_console = log_config.enable_file = log_config.enable_debug_file = log_config.enable_ringbuf = false;
    log_config.enable_console = true;
    log_config.global_level = LogLevel::ERROR;
    Log::init(log_config);
    AppConfig config;
    config.detector.model_path = argv[1];
    config.detector.backend = BackendType::CPU;
    config.mouse.backend = MouseBackend::KMBOX_NET;
    config.mouse.allow_send_input = false;
    config.mouse.kmbox_ip = "127.0.0.1";
    config.mouse.kmbox_port = 50000;
    config.mouse.kmbox_uuid = "00000000";
    config.movement.enabled = true;
    config.movement.large_ctrl_enabled = false;
    // 未启用源桥接、GSI、扳机或自动急停；所有采集和鼠标对象均由实例夹具提供。
    for (const unsigned fail_at : {1U, 2U}) check_failure(fail_at, config);
    Log::shutdown();
    if (failures != 0) return 1;
    std::cout << "PASS 首/次顶层线程构造失败同步回收、FAILED诊断、换配置直接重启及零设备输出\n";
    return 0;
}
