#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "runtime/runtime.h"

namespace runtime::detail {

inline bool class_id_configured(
        std::span<const int> class_ids, int class_id) noexcept {
    return std::find(class_ids.begin(), class_ids.end(), class_id) !=
           class_ids.end();
}

inline void summarize_detections(
        std::span<const Detection> detections,
        const AimConfig& aim_config,
        RuntimePipelineSample& sample) noexcept {
    for (const Detection& detection : detections) {
        const float confidence = std::clamp(detection.confidence, 0.0f, 1.0f);
        if (detection.class_id >= 0 &&
            static_cast<std::size_t>(detection.class_id) <
                kRuntimeReportedClassCount) {
            const std::size_t class_index =
                static_cast<std::size_t>(detection.class_id);
            ++sample.detection_count_by_class[class_index];
            sample.max_confidence_by_class[class_index] = std::max(
                sample.max_confidence_by_class[class_index], confidence);
        }
        // 与 Aim 保持相同的头部优先语义；身体列表为空时，非头部类别
        // 均可作为身体候选。原始 class_id 同时保留在上方固定类别槽中。
        if (class_id_configured(
                aim_config.head_class_ids, detection.class_id)) {
            ++sample.head_detection_count;
            sample.max_head_confidence =
                std::max(sample.max_head_confidence, confidence);
        } else if (aim_config.person_class_ids.empty() ||
                   class_id_configured(
                       aim_config.person_class_ids, detection.class_id)) {
            ++sample.person_detection_count;
            sample.max_person_confidence =
                std::max(sample.max_person_confidence, confidence);
        }
    }
}

template <typename T, std::size_t Capacity>
class BoundedSampleRing final {
    static_assert(Capacity > 0, "诊断环容量必须大于零");

public:
    bool push(const T& sample) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (size_ == Capacity) {
                read_index_ = (read_index_ + 1) % Capacity;
                --size_;
                ++dropped_;
            }
            storage_[write_index_] = sample;
            write_index_ = (write_index_ + 1) % Capacity;
            ++size_;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool drain(std::vector<T>& output) noexcept {
        try {
            std::vector<T> drained;
            std::lock_guard<std::mutex> lock(mutex_);
            drained.reserve(size_);
            for (std::size_t index = 0; index < size_; ++index) {
                drained.push_back(storage_[(read_index_ + index) % Capacity]);
            }
            read_index_ = write_index_;
            size_ = 0;
            output = std::move(drained);
            return true;
        } catch (...) {
            return false;
        }
    }

    void reset() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            read_index_ = 0;
            write_index_ = 0;
            size_ = 0;
            dropped_ = 0;
        } catch (...) {
        }
    }

    std::uint64_t dropped() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return dropped_;
        } catch (...) {
            return 0;
        }
    }

private:
    mutable std::mutex mutex_;
    std::array<T, Capacity> storage_{};
    std::size_t read_index_ = 0;
    std::size_t write_index_ = 0;
    std::size_t size_ = 0;
    std::uint64_t dropped_ = 0;
};

class LatestFrameQueue {
public:
    LatestFrameQueue();

    std::array<std::shared_ptr<CapturedFrame>, 3>
        initialization_slots() noexcept;
    std::shared_ptr<CapturedFrame> acquire_write() noexcept;
    void publish(const std::shared_ptr<CapturedFrame>& frame) noexcept;
    std::shared_ptr<const CapturedFrame> wait_latest(
        std::uint64_t last_sequence,
        const std::atomic<bool>& stop_requested) noexcept;
    void stop() noexcept;
    void reset() noexcept;
    std::uint64_t overwritten_frames() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::array<std::shared_ptr<CapturedFrame>, 3> pool_;
    std::shared_ptr<CapturedFrame> latest_;
    std::uint64_t consumed_sequence_ = 0;
    std::uint64_t overwritten_frames_ = 0;
    bool stopped_ = false;
};

struct PreviewStats {
    bool enabled = false;
    std::uint64_t sampled_frames = 0;
    std::uint64_t dropped_frames = 0;
};

class RuntimePreviewChannel {
public:
    RuntimePreviewChannel();

    bool set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept;
    // 用户启用选择与 Runtime 会话活动状态分离。停止或故障时关闭会话，
    // 可以保留 UI 开关和预分配缓冲，但任何在途旧帧都不得再次发布。
    void set_session_active(bool active) noexcept;
    bool publish(
        const cv::Mat& bgr,
        std::uint64_t sequence,
        float control_center_x,
        float control_center_y,
        DetectionStatus detection_status,
        AimStatus aim_status,
        std::span<const Detection> detections,
        const AimResult& aim_result,
        std::chrono::steady_clock::time_point now) noexcept;
    std::shared_ptr<const RuntimePreviewFrame> latest() noexcept;
    PreviewStats stats() const noexcept;

private:
    struct Slot;

    bool prepare_slots() noexcept;

    mutable std::mutex mutex_;
    std::array<std::shared_ptr<Slot>, 3> pool_;
    std::shared_ptr<Slot> latest_;
    // 成功发布和无空闲槽的失败尝试都推进采样时钟，确保消费者阻塞时
    // Pipeline 也只会每 100 ms 做一次槽检查，不退化为逐帧争锁。
    std::chrono::steady_clock::time_point last_sampled_at_{};
    // 启停或 Runtime 会话重置时递增。颜色转换在锁外执行，代际号用于阻止
    // 上一代正在加工的图像在快速关闭并重新启用后混入新会话。
    std::uint64_t generation_ = 0;
    std::uint64_t consumed_sequence_ = 0;
    std::uint64_t sampled_frames_ = 0;
    std::uint64_t dropped_frames_ = 0;
    bool slots_prepared_ = false;
    bool session_active_ = false;
    bool enabled_ = false;
};

class SafetyGate {
public:
    bool arm() noexcept;
    void disarm() noexcept;
    void set_hold(bool active) noexcept;
    void emergency_stop() noexcept;
    bool reset_emergency() noexcept;
    bool can_dispatch() const noexcept;
    bool armed() const noexcept;
    bool hold_active() const noexcept;
    bool emergency_stopped() const noexcept;

private:
    std::atomic<bool> armed_{false};
    std::atomic<bool> hold_active_{false};
    std::atomic<bool> emergency_stopped_{false};
};

} // namespace runtime::detail

#endif // RUNTIME_INTERNAL_H
