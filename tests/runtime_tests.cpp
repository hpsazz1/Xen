#include "runtime/runtime_internal.h"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace {

int failures = 0;

struct ReleaseProbe {
    explicit ReleaseProbe(int* counter) : released(counter) {}
    ReleaseProbe(const ReleaseProbe&) = delete;
    ReleaseProbe& operator=(const ReleaseProbe&) = delete;
    int* released = nullptr;
    ~ReleaseProbe() {
        if (released) ++*released;
    }
};

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
    slot->storage = CapturedFrameStorage::CPU_BGR;
    slot->width = 2;
    slot->height = 2;
    slot->timing.sequence = sequence;
    slot->timing.captured_at = std::chrono::steady_clock::now();
    queue.publish(slot);
}

void test_latest_frame_queue() {
    runtime::detail::LatestFrameQueue queue;
    {
        const auto initialization_slots = queue.initialization_slots();
        expect(initialization_slots[0] && initialization_slots[1] &&
                   initialization_slots[2] &&
                   initialization_slots[0] != initialization_slots[1] &&
                   initialization_slots[1] != initialization_slots[2],
               "Runtime 启动期必须取得三个互不相同的固定帧槽");
    }
    std::atomic<bool> stop{false};
    publish(queue, 1, 10);
    std::uint64_t overwritten_at_consume = 99;
    const auto first = queue.wait_latest(0, stop, &overwritten_at_consume);
    expect(first && first->timing.sequence == 1,
           "消费者应取得首个发布帧");
    expect(overwritten_at_consume == 0,
           "首个样本必须在消费锁边界固化零覆盖端点");
    expect(first && first->bgr.at<cv::Vec3b>(0, 0)[0] == 10,
           "消费者持有的帧内容应正确");

    publish(queue, 2, 20);
    publish(queue, 3, 30);
    expect(queue.overwritten_frames() == 1,
           "消费者落后时只统计被覆盖的未消费帧");
    expect(first && first->bgr.at<cv::Vec3b>(0, 0)[0] == 10,
           "生产者覆写新帧时不得破坏消费者持有的槽");
    const auto latest = queue.wait_latest(
        1, stop, &overwritten_at_consume);
    expect(latest && latest->timing.sequence == 3,
           "队列必须只返回最新帧而不是积压帧");
    expect(overwritten_at_consume == 1,
           "消费端点覆盖累计值必须与 sequence 缺口一致");

    queue.stop();
    expect(!queue.wait_latest(3, stop),
           "停止队列后等待者必须立即退出");
}

void test_detection_observability_preserves_team_classes() {
    AimConfig config;
    config.person_class_ids = {0, 2};
    config.head_class_ids = {1, 3};

    RuntimePipelineSample ct_sample;
    const std::array<Detection, 2> ct_detections{{
        {10.0f, 10.0f, 100.0f, 200.0f, 0.864f, 0},
        {30.0f, 15.0f, 70.0f, 60.0f, 0.871f, 1}}};
    runtime::detail::summarize_detections(
        ct_detections, config, ct_sample);
    expect(ct_sample.person_detection_count == 1 &&
               ct_sample.head_detection_count == 1 &&
               ct_sample.detection_count_by_class[0] == 1 &&
               ct_sample.detection_count_by_class[1] == 1 &&
               ct_sample.detection_count_by_class[2] == 0 &&
               ct_sample.detection_count_by_class[3] == 0,
           "单个 CT 目标只能形成 C0 身体与 C1 头部，不得伪造 T 类别");

    RuntimePipelineSample t_sample;
    const std::array<Detection, 2> t_detections{{
        {10.0f, 10.0f, 100.0f, 200.0f, 0.855f, 2},
        {30.0f, 15.0f, 70.0f, 60.0f, 0.862f, 3}}};
    runtime::detail::summarize_detections(t_detections, config, t_sample);
    expect(t_sample.person_detection_count == 1 &&
               t_sample.head_detection_count == 1 &&
               t_sample.detection_count_by_class[0] == 0 &&
               t_sample.detection_count_by_class[1] == 0 &&
               t_sample.detection_count_by_class[2] == 1 &&
               t_sample.detection_count_by_class[3] == 1,
           "单个 T 目标只能形成 C2 身体与 C3 头部，不得伪造 CT 类别");
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

void test_gpu_storage_released_on_reset() {
    runtime::detail::LatestFrameQueue queue;
    auto slot = queue.acquire_write();
    expect(slot != nullptr, "队列应提供 GPU 帧测试槽");
    if (!slot) return;
    int released = 0;
    slot->native_storage = std::make_shared<ReleaseProbe>(&released);
    slot->native_synchronization = std::make_shared<std::mutex>();
    slot->storage = CapturedFrameStorage::D3D11_BGRA8;
    slot->width = 320;
    slot->height = 320;
    slot->timing.sequence = 1;
    queue.publish(slot);
    queue.reset();
    expect(!slot->native_storage && !slot->native_synchronization &&
               slot->storage == CapturedFrameStorage::CPU_BGR &&
               slot->width == 0 && slot->height == 0 && released == 1,
           "Runtime 重置必须释放旧会话 GPU 资源并清空显式几何");
}

void test_directml_frame_requires_fence() {
    runtime::detail::LatestFrameQueue queue;
    auto slot = queue.acquire_write();
    expect(slot != nullptr, "队列应提供 DirectML GPU 帧测试槽");
    if (!slot) return;
    slot->native_storage = std::make_shared<int>(1);
    slot->native_synchronization = std::make_shared<std::mutex>();
    slot->storage = CapturedFrameStorage::D3D11_BGRA8_DIRECTML;
    slot->width = 320;
    slot->height = 320;
    slot->timing.sequence = 1;
    queue.publish(slot);
    std::atomic<bool> stop{false};
    expect(!queue.wait_latest(0, stop),
           "缺少 shared fence 的 DirectML GPU 帧必须拒绝发布");

    slot->native_fence = std::make_shared<int>(2);
    slot->native_fence_value = 1;
    queue.publish(slot);
    const auto published = queue.wait_latest(0, stop);
    expect(published && published->timing.sequence == 1,
           "纹理、提交锁和非零 fence 完整时才允许发布 DirectML 帧");
}

void test_safety_gate() {
    runtime::detail::SafetyGate gate;
    expect(!gate.can_dispatch(), "安全门默认必须关闭");
    expect(gate.arm(), "无急停时允许物理武装");
    gate.set_hold(true);
    expect(gate.can_dispatch(), "物理武装且按住热键时允许派发");
    gate.emergency_stop();
    expect(!gate.can_dispatch() && !gate.output_armed(),
           "急停必须原子解除武装并拒绝后续命令");
    expect(!gate.reset_emergency(),
           "热键仍按住时不得复位急停");
    gate.set_hold(false);
    expect(gate.reset_emergency(), "释放热键后允许复位急停");
    expect(!gate.can_dispatch(), "复位急停不能自动重新武装");
}

void test_bounded_sample_ring() {
    runtime::detail::BoundedSampleRing<int, 3> ring;
    expect(ring.push(1) && ring.push(2) && ring.push(3) && ring.push(4),
           "固定容量诊断环应接受样本并覆盖最旧项");
    expect(ring.dropped() == 1,
           "诊断环满载时必须累计覆盖丢弃数");
    std::vector<int> output;
    expect(ring.drain(output) && output == std::vector<int>({2, 3, 4}),
           "诊断环必须按时间顺序取出保留样本");
    expect(ring.drain(output) && output.empty(),
           "诊断环取出后必须为空而不重复交付");
    ring.reset();
    expect(ring.dropped() == 0 && ring.push(9),
           "诊断环 reset 必须清零统计并允许复用");

    runtime::detail::BoundedSampleRing<int, 3> two_phase;
    expect(two_phase.push(1), "两阶段诊断环应先接受普通样本");
    const auto pending = two_phase.push_pending(2);
    expect(static_cast<bool>(pending) && two_phase.push(3),
           "两阶段诊断环应返回可校验 token");
    expect(two_phase.drain(output) && output == std::vector<int>({1}),
           "drain 遇到 pending 样本必须停止，不能越过交付后续样本");
    expect(two_phase.finalize(pending, [](int& value) noexcept {
               value += 20;
           }),
           "合法 token 必须能原位补齐同一槽样本");
    expect(two_phase.drain(output) && output == std::vector<int>({22, 3}),
           "finalize 后必须按原发布顺序交付完整样本");
    expect(!two_phase.finalize(pending, [](int&) noexcept {}),
           "已交付 token 不得二次 finalize");

    runtime::detail::BoundedSampleRing<int, 2> overwritten_pending;
    const auto stale = overwritten_pending.push_pending(7);
    expect(overwritten_pending.push(8) && overwritten_pending.push(9) &&
               overwritten_pending.dropped() == 1 &&
               !overwritten_pending.finalize(
                   stale, [](int&) noexcept {}),
           "已被容量覆盖的 pending token 必须失效且不得污染新样本");
    overwritten_pending.reset();
    const auto fresh = overwritten_pending.push_pending(10);
    expect(!overwritten_pending.finalize(stale, [](int&) noexcept {}) &&
               overwritten_pending.finalize(
                   fresh, [](int& value) noexcept { value += 1; }) &&
               overwritten_pending.drain(output) &&
               output == std::vector<int>({11}),
           "reset 后必须保持代际单调，旧 token 不得命中新占用槽");
}

void test_runtime_preview_channel() {
    runtime::detail::RuntimePreviewChannel preview;
    cv::Mat image(320, 640, CV_8UC3, cv::Scalar(10, 20, 30));
    const std::vector<Detection> detections{
        {10.0f, 20.0f, 110.0f, 120.0f, 0.9f, 1}};
    AimResult aim_result;
    aim_result.status = AimStatus::SUCCESS;
    aim_result.has_target = true;
    aim_result.acquisition_range_radius = 144.0f;
    aim_result.active_range_radius = 72.0f;
    aim_result.range_locked = true;
    aim_result.range_allows_control = false;
    aim_result.target.track_id = 7;
    aim_result.target.base_aim_x = 55.0f;
    aim_result.target.base_aim_y = 40.0f;
    aim_result.target.aim_x = 60.0f;
    aim_result.target.aim_y = 40.0f;
    aim_result.target.lead_x = 5.0f;
    aim_result.target.lead_active = true;
    const auto started = std::chrono::steady_clock::now();

    expect(!preview.publish(
               image, 1, 320.0f, 160.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started),
           "预览默认关闭时不得复制图像");
    expect(preview.set_enabled(true), "启用预览时应成功预分配固定槽");
    expect(!preview.publish(
               image, 1, 320.0f, 160.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started),
           "Runtime 会话未启动时即使用户启用预览也不得发布图像");
    preview.set_session_active(true);
    expect(preview.publish(
               image, 1, 320.0f, 160.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started),
           "启用后首个预览样本应发布成功");
    const auto first = preview.latest();
    expect(first && first->sequence == 1 &&
               first->width == 512 && first->height == 256 &&
               first->roi_width == 640 && first->roi_height == 320,
           "预览必须按最长边 512 等比缩放并保留原 ROI 尺寸");
    expect(first && first->bgra[0] == 10 && first->bgra[1] == 20 &&
               first->bgra[2] == 30 && first->bgra[3] == 255,
           "BGR 预览必须转换为可直接上传 D3D11 的 BGRA");
    expect(first && first->detection_count == 1 &&
               first->detections[0].class_id == 1 &&
               first->has_target && first->target.track_id == 7,
           "图像、检测框和 Aim 目标必须固化为同一序号快照");
    expect(first && first->aim_acquisition_range_radius == 144.0f &&
               first->aim_active_range_radius == 72.0f &&
               first->aim_range_locked &&
               !first->aim_range_allows_control &&
               first->target.base_aim_x == 55.0f &&
               first->target.lead_x == 5.0f &&
               first->target.lead_active,
           "预览必须复制动态范围和预测标量，范围阻断不能删除同帧检测或目标观测");
    expect(!preview.publish(
               image, 2, 320.0f, 160.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(99)),
           "预览采样间隔不足 100 ms 时必须跳过而不复制");
    expect(preview.publish(
               image, 2, 320.0f, 160.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(100)),
           "预览达到 10 FPS 节流边界时应发布");
    const auto stats = preview.stats();
    expect(stats.enabled && stats.sampled_frames == 2 &&
               stats.dropped_frames == 0,
           "正常消费下预览统计应记录样本且无丢弃");
    expect(preview.set_enabled(false) && !preview.latest(),
           "关闭预览后必须立即停止交付旧帧");
}

void test_runtime_preview_overwrite_and_truncation() {
    runtime::detail::RuntimePreviewChannel preview;
    cv::Mat image(32, 32, CV_8UC3, cv::Scalar(1, 2, 3));
    std::vector<Detection> detections(kRuntimePreviewMaxDetections + 2);
    for (std::size_t index = 0; index < detections.size(); ++index) {
        detections[index].class_id = static_cast<int>(index);
    }
    AimResult aim_result;
    const auto started = std::chrono::steady_clock::now();

    expect(preview.set_enabled(true), "覆盖测试应能启用预览");
    preview.set_session_active(true);
    expect(preview.publish(
               image, 1, 16.0f, 16.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started),
           "覆盖测试首帧应发布成功");
    expect(preview.publish(
               image, 2, 16.0f, 16.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(100)),
           "消费者未读取时仍应以最新帧覆盖而不反压 Pipeline");
    const auto latest = preview.latest();
    const auto stats = preview.stats();
    expect(latest && latest->sequence == 2 &&
               latest->detection_count == kRuntimePreviewMaxDetections &&
               latest->detections[kRuntimePreviewMaxDetections - 1].class_id ==
                   static_cast<int>(kRuntimePreviewMaxDetections - 1),
           "预览必须只保留最新帧并将检测框截断到固定容量");
    expect(stats.sampled_frames == 2 && stats.dropped_frames == 1,
           "覆盖未消费预览时必须累计一次丢弃");
}

void test_runtime_preview_finish_session_reports_before_reset() {
    runtime::detail::RuntimePreviewChannel preview;
    cv::Mat image(16, 16, CV_8UC3, cv::Scalar(7, 8, 9));
    const std::vector<Detection> detections;
    AimResult aim_result;
    const auto started = std::chrono::steady_clock::now();

    expect(preview.set_enabled(true), "结束统计测试应能启用预览");
    preview.set_session_active(true);
    expect(preview.publish(
               image, 1, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started) &&
               preview.publish(
                   image, 2, 8.0f, 8.0f, DetectionStatus::SUCCESS,
                   AimStatus::SUCCESS, detections, aim_result,
                   started + std::chrono::milliseconds(100)),
           "结束统计测试必须先形成两个采样和一次未消费覆盖");
    const auto finished_stats = preview.finish_session();
    const auto reset_stats = preview.stats();
    expect(finished_stats.enabled && finished_stats.sampled_frames == 2 &&
               finished_stats.dropped_frames == 1,
           "结束会话必须在清零前返回完整预览采样与丢弃统计");
    expect(reset_stats.enabled && reset_stats.sampled_frames == 0 &&
               reset_stats.dropped_frames == 0 && !preview.latest(),
           "结束会话返回统计后必须清除旧画面和内部计数");
    expect(!preview.publish(
               image, 3, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(200)),
           "结束会话后必须拒绝在途旧帧重新发布");
}

void test_runtime_preview_held_slots_and_reset() {
    runtime::detail::RuntimePreviewChannel preview;
    cv::Mat image(16, 16, CV_8UC3, cv::Scalar(4, 5, 6));
    const std::vector<Detection> detections;
    AimResult aim_result;
    const auto started = std::chrono::steady_clock::now();

    expect(preview.set_enabled(true), "持槽测试应能启用预览");
    preview.set_session_active(true);
    expect(preview.publish(
               image, 1, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result, started),
           "持槽测试第一帧应发布成功");
    auto first = preview.latest();
    expect(preview.publish(
               image, 2, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(100)),
           "持槽测试第二帧应发布成功");
    auto second = preview.latest();
    expect(preview.publish(
               image, 3, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(200)),
           "持槽测试第三帧应发布成功");
    auto third = preview.latest();
    expect(first && second && third &&
               first->sequence == 1 && second->sequence == 2 &&
               third->sequence == 3,
           "外部持有的三帧内容不得被后续发布原地覆盖");
    expect(!preview.publish(
               image, 4, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(300)),
           "三槽均被消费者持有时必须丢弃新预览且不得阻塞");
    expect(preview.stats().dropped_frames == 1,
           "无空闲预览槽时必须累计一次丢弃");
    expect(!preview.publish(
               image, 5, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(350)) &&
               preview.stats().dropped_frames == 1,
           "无空闲槽后的重试也必须受 100 ms 采样间隔限制");

    const auto* first_storage = first->bgra.data();
    expect(preview.set_enabled(false) && preview.set_enabled(true),
           "消费者仍持槽时预览通道应能安全重启");
    expect(first->sequence == 1 && first->bgra.data() == first_storage,
           "通道重启不得重新分配或改写消费者仍持有的旧槽");
    first.reset();
    second.reset();
    third.reset();
    preview.finish_session();
    const auto reset_stats = preview.stats();
    expect(reset_stats.enabled && reset_stats.sampled_frames == 0 &&
               reset_stats.dropped_frames == 0 && !preview.latest(),
           "会话关闭必须保留开关、清除旧帧并清零预览统计");
    expect(!preview.publish(
               image, 5, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(400)),
           "会话关闭后不得让最后一帧重新进入预览通道");
    preview.set_session_active(true);
    expect(preview.publish(
               image, 5, 8.0f, 8.0f, DetectionStatus::SUCCESS,
               AimStatus::SUCCESS, detections, aim_result,
               started + std::chrono::milliseconds(400)),
           "会话重置后应立即允许发布新预览");
    const auto after_reset = preview.latest();
    expect(after_reset && after_reset->bgra.data() == first_storage &&
               after_reset->bgra.capacity() >=
                   static_cast<std::size_t>(kRuntimePreviewMaxDimension) *
                       kRuntimePreviewMaxDimension * 4,
           "会话重置必须复用已预分配的大缓冲");
}

} // namespace

int main() {
    test_latest_frame_queue();
    test_detection_observability_preserves_team_classes();
    test_network_storage_released_on_reset();
    test_gpu_storage_released_on_reset();
    test_directml_frame_requires_fence();
    test_safety_gate();
    test_bounded_sample_ring();
    test_runtime_preview_channel();
    test_runtime_preview_overwrite_and_truncation();
    test_runtime_preview_finish_session_reports_before_reset();
    test_runtime_preview_held_slots_and_reset();
    if (failures != 0) {
        std::cerr << "Runtime 核心测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Runtime 核心测试全部通过。\n";
    return 0;
}
