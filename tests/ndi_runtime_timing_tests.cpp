#include "ndi_capture_test_system.h"
#include "runtime/runtime_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void test_ndi_timing_reaches_runtime_and_aim() {
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto config = ndi_test::config();
    config.roi_width = 64;
    config.roi_height = 64;
    auto capture = capture::detail::create_ndi_capture(config, std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "NDI 系统夹具必须打开真实 Capture");
        return;
    }
    runtime::detail::LatestFrameQueue queue;
    runtime::detail::CaptureFramePublisher publisher(queue);
    runtime::detail::RuntimeObservationClock observation_clock;
    AimConfig aim_config;
    aim_config.min_confirmed_hits = 1;
    Aim aim(aim_config);
    const std::atomic<bool> stop{false};
    std::uint64_t previous_sequence = 0;

    // 先让生产池及 Runtime 队列跨槽复用，再分别送缺失、过期和未来映射。
    // 系统输入不构造 FrameTiming，所有事实都来自真实 NDI 发布路径。
    for (int index = 0; index < 10; ++index) {
        ndi_test::VideoInput video;
        video.width = 64;
        video.height = 64;
        if (index == 6) video.timestamp = NDIlib_recv_timestamp_undefined;
        if (index == 7) video.mapping = ndi_test::MappingMode::STALE;
        if (index == 8) video.mapping = ndi_test::MappingMode::FUTURE;
        const bool expected_source_valid = index < 6 || index == 9;
        auto destination = queue.acquire_write();
        if (!destination) {
            expect(false, "Runtime 必须能取得下一个可写帧槽");
            return;
        }
        input->send_video(std::move(video));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool published = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto outcome = publisher.capture_and_publish(*capture, destination, false);
            if (outcome.frame_publish_result == runtime::detail::FramePublishResult::PUBLISHED) {
                published = true;
                break;
            }
            if (outcome.capture_status != CaptureStatus::NO_FRAME &&
                outcome.capture_status != CaptureStatus::READY) break;
        }
        if (!published) {
            expect(false, "真实 ICapture::grab 的本帧必须经 Runtime publisher 发布");
            return;
        }
        const auto frame = queue.wait_latest(previous_sequence, stop);
        if (!frame) {
            expect(false, "Runtime 消费端必须取得本次发布的帧");
            return;
        }
        previous_sequence = frame->timing.sequence;
        AimFrame aim_frame;
        const bool reset = observation_clock.apply(frame->timing, aim_frame);
        expect(reset == (index == 0 || index == 6 || index == 9),
               "时间基准转换必须沿 Runtime 原有连续性合同重置 Aim");
        if (reset) aim.reset();
        const auto expected_observation = expected_source_valid
            ? frame->timing.captured_at - std::chrono::milliseconds(5)
            : frame->timing.captured_at;
        expect(frame->timing.source_time_timing_valid == expected_source_valid &&
                   aim_frame.sequence == frame->timing.sequence &&
                   aim_frame.captured_at == expected_observation,
               "Runtime 交给 Aim 的时刻必须来自本帧有效映射或本帧 capture，不能用旧槽时刻");
        aim_frame.control_at = frame->timing.captured_at + std::chrono::milliseconds(10);
        aim_frame.roi_width = frame->width;
        aim_frame.roi_height = frame->height;
        aim_frame.control_center_x = 32.0f;
        aim_frame.control_center_y = 32.0f;
        aim_frame.detections = {{16.0f, 16.0f, 48.0f, 56.0f, 0.9f, 0}};
        const auto result = aim.process(aim_frame);
        expect(result.status == AimStatus::SUCCESS && result.has_target &&
                   std::abs(result.target.observation_age_ms -
                       (expected_source_valid ? 15.0f : 10.0f)) < 0.001f,
               "公开 Aim::process 的观测年龄必须反映实际送入的本帧时刻");
    }
    expect(publisher.published_frames() == 10,
           "十个受控 SDK 视频帧必须逐一经 Runtime 队列消费");
    capture->close();
}

} // namespace

int main() {
    test_ndi_timing_reaches_runtime_and_aim();
    if (failures != 0) return 1;
    std::cout << "NDI → Runtime 队列/观测时钟 → Aim 公有输入测试通过\n";
    return 0;
}
