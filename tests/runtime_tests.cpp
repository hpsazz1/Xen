#include "runtime/runtime_internal.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void publish(runtime::detail::LatestFrameQueue& queue,
             std::uint64_t sequence,
             unsigned char marker) {
    auto slot = queue.acquire_write();
    expect(slot != nullptr, "三槽队列应提供可写槽");
    if (!slot) return;
    slot->bgr.create(2, 2, CV_8UC3);
    slot->bgr.setTo(cv::Scalar(marker, marker, marker));
    slot->timing.sequence = sequence;
    slot->timing.captured_at = std::chrono::steady_clock::now();
    queue.publish(slot);
}

void test_latest_frame_queue() {
    runtime::detail::LatestFrameQueue queue;
    std::atomic<bool> stop{false};
    publish(queue, 1, 10);
    const auto first = queue.wait_latest(0, stop);
    expect(first && first->timing.sequence == 1,
           "消费者应取得首个发布帧");
    expect(first && first->bgr.at<cv::Vec3b>(0, 0)[0] == 10,
           "消费者持有的帧内容应正确");

    publish(queue, 2, 20);
    publish(queue, 3, 30);
    expect(queue.overwritten_frames() == 1,
           "消费者落后时只统计被覆盖的未消费帧");
    expect(first && first->bgr.at<cv::Vec3b>(0, 0)[0] == 10,
           "生产者覆写新帧时不得破坏消费者持有的槽");
    const auto latest = queue.wait_latest(1, stop);
    expect(latest && latest->timing.sequence == 3,
           "队列必须只返回最新帧而不是积压帧");

    queue.stop();
    expect(!queue.wait_latest(3, stop),
           "停止队列后等待者必须立即退出");
}

void test_network_storage_released_on_reset() {
    runtime::detail::LatestFrameQueue queue;
    auto slot = queue.acquire_write();
    expect(slot != nullptr, "队列应提供网络帧测试槽");
    if (!slot) return;
    auto storage = std::make_shared<cv::Mat>(2, 2, CV_8UC3);
    slot->bgr_storage = storage;
    slot->bgr = *storage;
    slot->timing.sequence = 1;
    queue.publish(slot);
    queue.reset();
    expect(!slot->bgr_storage && slot->bgr.empty(),
           "Runtime 重置时必须归还异步 Capture 缓冲槽");
}

void test_safety_gate() {
    runtime::detail::SafetyGate gate;
    expect(!gate.can_dispatch(), "安全门默认必须关闭");
    expect(gate.arm(), "无急停时允许武装");
    gate.set_hold(true);
    expect(gate.can_dispatch(), "武装且按住热键时允许派发");
    gate.emergency_stop();
    expect(!gate.can_dispatch() && !gate.armed(),
           "急停必须原子解除武装并拒绝后续命令");
    expect(!gate.reset_emergency(),
           "热键仍按住时不得复位急停");
    gate.set_hold(false);
    expect(gate.reset_emergency(), "释放热键后允许复位急停");
    expect(!gate.can_dispatch(), "复位急停不能自动重新武装");
}

} // namespace

int main() {
    test_latest_frame_queue();
    test_network_storage_released_on_reset();
    test_safety_gate();
    if (failures != 0) {
        std::cerr << "Runtime 核心测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Runtime 核心测试全部通过。\n";
    return 0;
}
