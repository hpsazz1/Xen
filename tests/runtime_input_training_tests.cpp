#include "runtime/runtime.h"
#include "mouse/input_report_internal.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>

namespace {
void require(bool passed, const char* message) {
    if (!passed) { std::cerr << message << '\n'; std::exit(1); }
}
class Monitor final : public IMouseController {
public:
    bool open() noexcept override { ++opened; return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++output; return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    ButtonReceipt set_left_button(bool) noexcept override { ++output; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++output; return {}; }
    KeyboardReceipt set_wasd_mask(std::uint8_t, bool) noexcept override { ++output; return {}; }
    bool set_input_report_subscription(bool value) noexcept override {
        std::lock_guard lock(mutex);
        return buffer.subscribe(value);
    }
    bool read_input_reports(InputReportCursor& cursor, InputReportBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        buffer.read(cursor, batch);
        return true;
    }
    void close() noexcept override { ++closed; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    void push(bool left, int index, std::uint8_t mask = 0) {
        std::lock_guard lock(mutex);
        InputReportEvent report;
        report.received_at_steady_ns = 1000000000ll + index * 1000000ll;
        report.state_valid = true;
        report.mouse_buttons = left ? 1 : 0;
        report.wasd_mask = mask;
        report.raw_x = 7;
        report.raw_report[1] = report.mouse_buttons;
        report.raw_report[2] = 7;
        report.datagram_size = 20;
        buffer.publish(report);
    }
    int opened = 0, closed = 0, output = 0;
    mouse::detail::InputReportBuffer buffer;
    std::mutex mutex;
};
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-runtime-input-training-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto monitor = std::make_shared<Monitor>();
    Runtime runtime;
    require(!runtime.start_input_training(root / "unsupported"), "没有设备不得偷偷创建或连接");
    require(runtime.start_input_training(root / "captured", monitor), "停止态可记录已有输入");
    require(runtime.snapshot().state == RuntimeState::STOPPED, "输入记录不能启动检测Runtime");
    require(!runtime.start_input_training(root / "duplicate", monitor), "运行中的记录必须拒绝替换");
    monitor->push(false, 1);
    monitor->push(true, 2);
    for (int index = 3; index <= 1300; ++index) monitor->push(true, index);
    monitor->push(false, 1301);
    runtime.stop_input_training();
    const auto captured = runtime.snapshot().training;
    require(captured && captured->status == input_training::Status::STOPPED,
            "停止应保留可查看的快照");
    require(captured->received_events == 1301 && captured->dropped_events == 0,
            "冻结后必须排空大于单批的所有尾报告");
    require(captured->total_holds == 1 && !captured->holds.empty(), "down到up应保留一个hold");
    const auto& hold = *captured->holds.back();
    require(hold.complete_received_stream && !hold.motion_available && !hold.physical_motion_verified,
            "接收片段完整不等于XY已验证或物理轨迹可用");
    require(hold.points.size() > 128 && hold.points.back().event.raw_report[2] == 7,
            "原始报告不得重采样覆盖");
    require(monitor->opened == 0 && monitor->output == 0 && monitor->closed == 0,
            "训练只能订阅，不得open/输出/关闭共享设备");
    require(!runtime.start_input_training(root / "captured", monitor), "既有Run不得覆盖");
    require(!monitor->buffer.subscribed(), "目录失败后必须释放输入订阅");
    require(runtime.start_input_training(root / "restart", monitor), "新Run可重新记录");
    monitor->push(false, 1);
    monitor->push(true, 2);
    runtime.stop();
    const auto stopped = runtime.snapshot().training;
    require(stopped && !stopped->holds.empty() && !stopped->holds.back()->complete_received_stream,
            "Runtime停止不能把缺up片段伪造完整");
    require(monitor->output == 0 && monitor->opened == 0, "停止记录不能发送清理输入");
    std::cout << "Runtime输入训练独立生命周期、尾水位、原始保留及零输出契约通过\n";
}
