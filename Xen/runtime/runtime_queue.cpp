#include "runtime/runtime_internal.h"

#include <chrono>
#include <cmath>
#include <memory>

#include <opencv2/imgproc.hpp>

namespace runtime::detail {

LatestFrameQueue::LatestFrameQueue() {
    for (auto& slot : pool_) {
        slot = std::make_shared<CapturedFrame>();
    }
}

std::array<std::shared_ptr<CapturedFrame>, 3>
LatestFrameQueue::initialization_slots() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ || stopped_) return {};
        return pool_;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<CapturedFrame> LatestFrameQueue::acquire_write() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return nullptr;
        for (const auto& slot : pool_) {
            // pool_ 自身持有一个引用；use_count()==1 表示该槽既未发布，
            // 也未被 Pipeline 消费者持有，可以安全原地覆写 cv::Mat。
            if (slot.use_count() == 1 && slot != latest_) return slot;
        }
    } catch (...) {
    }
    return nullptr;
}

void LatestFrameQueue::publish(
        const std::shared_ptr<CapturedFrame>& frame) noexcept {
    if (!frame || frame->timing.sequence == 0 ||
        frame->width <= 0 || frame->height <= 0) {
        return;
    }
    if (frame->storage == CapturedFrameStorage::CPU_BGR) {
        if (frame->bgr.empty() || frame->bgr.type() != CV_8UC3 ||
            frame->bgr.cols != frame->width ||
            frame->bgr.rows != frame->height) {
            return;
        }
    } else if (!frame->native_storage || !frame->native_synchronization ||
               !frame->bgr.empty() ||
               (frame->storage ==
                    CapturedFrameStorage::D3D11_BGRA8_DIRECTML &&
                (!frame->native_fence || frame->native_fence_value == 0))) {
        return;
    }
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) return;
            if (latest_ && latest_->timing.sequence != consumed_sequence_) {
                ++overwritten_frames_;
            }
            latest_ = frame;
        }
        condition_.notify_one();
    } catch (...) {
    }
}

std::shared_ptr<const CapturedFrame> LatestFrameQueue::wait_latest(
        std::uint64_t last_sequence,
        const std::atomic<bool>& stop_requested) noexcept {
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(50), [&] {
            return stopped_ || stop_requested.load(std::memory_order_acquire) ||
                   (latest_ && latest_->timing.sequence != last_sequence);
        });
        if (stopped_ || stop_requested.load(std::memory_order_acquire) ||
            !latest_ || latest_->timing.sequence == last_sequence) {
            return nullptr;
        }
        consumed_sequence_ = latest_->timing.sequence;
        return latest_;
    } catch (...) {
        return nullptr;
    }
}

void LatestFrameQueue::stop() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        condition_.notify_all();
    } catch (...) {
    }
}

void LatestFrameQueue::reset() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset();
        consumed_sequence_ = 0;
        overwritten_frames_ = 0;
        stopped_ = false;
        for (auto& slot : pool_) {
            if (slot->bgr_storage) {
                // 停止后的队列不再有消费者；释放 UDP 后端的别名 Mat 和槽所有权，
                // 避免旧采集会话的大缓冲跨越下一次 Runtime 启动。
                slot->bgr.release();
                slot->bgr_storage.reset();
            }
            // D3D11 互操作纹理由帧槽持有，跨 Runtime 会话不能复用旧设备资源。
            slot->native_storage.reset();
            slot->native_synchronization.reset();
            slot->native_fence.reset();
            slot->native_fence_value = 0;
            slot->storage = CapturedFrameStorage::CPU_BGR;
            slot->width = 0;
            slot->height = 0;
            slot->timing = {};
            slot->roi_x = 0;
            slot->roi_y = 0;
            slot->source_width = 0;
            slot->source_height = 0;
            slot->encoded_width = 0;
            slot->encoded_height = 0;
            slot->source_pixels_per_pixel_x = 1.0;
            slot->source_pixels_per_pixel_y = 1.0;
            // bgr 保留已分配内存，下一次同 shape 取帧可直接复用。
        }
    } catch (...) {
    }
}

std::uint64_t LatestFrameQueue::overwritten_frames() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return overwritten_frames_;
    } catch (...) {
        return 0;
    }
}

struct RuntimePreviewChannel::Slot {
    RuntimePreviewFrame frame;
    cv::Mat resized_bgr;
};

RuntimePreviewChannel::RuntimePreviewChannel() {
    for (auto& slot : pool_) slot = std::make_shared<Slot>();
}

bool RuntimePreviewChannel::prepare_slots() noexcept {
    try {
        if (slots_prepared_) return true;
        constexpr std::size_t kMaximumBytes =
            static_cast<std::size_t>(kRuntimePreviewMaxDimension) *
            static_cast<std::size_t>(kRuntimePreviewMaxDimension) * 4;
        for (auto& slot : pool_) {
            if (!slot) slot = std::make_shared<Slot>();
            slot->frame.bgra.resize(kMaximumBytes);
            slot->resized_bgr.create(
                kRuntimePreviewMaxDimension,
                kRuntimePreviewMaxDimension, CV_8UC3);
        }
        slots_prepared_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool RuntimePreviewChannel::set_enabled(bool enabled) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ == enabled) return true;
        if (enabled && !prepare_slots()) return false;
        ++generation_;
        enabled_ = enabled;
        latest_.reset();
        last_sampled_at_ = {};
        consumed_sequence_ = 0;
        sampled_frames_ = 0;
        dropped_frames_ = 0;
        return true;
    } catch (...) {
        return false;
    }
}

bool RuntimePreviewChannel::enabled() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    } catch (...) {
        return false;
    }
}

void RuntimePreviewChannel::set_session_active(bool active) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        session_active_ = active;
        latest_.reset();
        last_sampled_at_ = {};
        consumed_sequence_ = 0;
        sampled_frames_ = 0;
        dropped_frames_ = 0;
    } catch (...) {
    }
}

bool RuntimePreviewChannel::publish(
        const cv::Mat& bgr,
        std::uint64_t sequence,
        float control_center_x,
        float control_center_y,
        DetectionStatus detection_status,
        AimStatus aim_status,
        std::span<const Detection> detections,
        const AimResult& aim_result,
        std::chrono::steady_clock::time_point now) noexcept {
    if (bgr.empty() || bgr.type() != CV_8UC3 || sequence == 0) return false;
    std::shared_ptr<Slot> slot;
    std::uint64_t publish_generation = 0;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_ || !session_active_) return false;
            publish_generation = generation_;
            constexpr auto kMinimumInterval = std::chrono::milliseconds(100);
            if (last_sampled_at_.time_since_epoch().count() != 0 &&
                now - last_sampled_at_ < kMinimumInterval) {
                return false;
            }
            last_sampled_at_ = now;
            for (const auto& candidate : pool_) {
                if (candidate && candidate.use_count() == 1 &&
                    candidate != latest_) {
                    slot = candidate;
                    break;
                }
            }
            if (!slot) {
                ++dropped_frames_;
                return false;
            }
        }

        const float scale = std::min(
            1.0f,
            std::min(
                static_cast<float>(kRuntimePreviewMaxDimension) /
                    static_cast<float>(bgr.cols),
                static_cast<float>(kRuntimePreviewMaxDimension) /
                    static_cast<float>(bgr.rows)));
        const int width = std::max(
            1, static_cast<int>(std::floor(bgr.cols * scale)));
        const int height = std::max(
            1, static_cast<int>(std::floor(bgr.rows * scale)));
        cv::Mat source = bgr;
        if (width != bgr.cols || height != bgr.rows) {
            cv::Mat resized = slot->resized_bgr(
                cv::Rect(0, 0, width, height));
            cv::resize(bgr, resized, cv::Size(width, height),
                       0.0, 0.0, cv::INTER_LINEAR);
            source = resized;
        }
        cv::Mat bgra(
            height, width, CV_8UC4, slot->frame.bgra.data(),
            static_cast<std::size_t>(width) * 4);
        cv::cvtColor(source, bgra, cv::COLOR_BGR2BGRA);

        RuntimePreviewFrame& frame = slot->frame;
        frame.sequence = sequence;
        frame.width = width;
        frame.height = height;
        frame.roi_width = bgr.cols;
        frame.roi_height = bgr.rows;
        frame.scale_x = static_cast<float>(width) /
                        static_cast<float>(bgr.cols);
        frame.scale_y = static_cast<float>(height) /
                        static_cast<float>(bgr.rows);
        frame.control_center_x = control_center_x;
        frame.control_center_y = control_center_y;
        frame.detection_status = detection_status;
        frame.aim_status = aim_status;
        frame.detection_count = std::min(
            detections.size(), kRuntimePreviewMaxDetections);
        for (std::size_t index = 0; index < frame.detection_count; ++index) {
            frame.detections[index] = detections[index];
        }
        frame.has_target = aim_result.has_target;
        frame.target = aim_result.target;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_ || !session_active_ ||
                generation_ != publish_generation) {
                return false;
            }
            if (latest_ && latest_->frame.sequence != consumed_sequence_) {
                ++dropped_frames_;
            }
            latest_ = slot;
            ++sampled_frames_;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<const RuntimePreviewFrame>
RuntimePreviewChannel::latest() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !session_active_ || !latest_) return nullptr;
        consumed_sequence_ = latest_->frame.sequence;
        auto slot = latest_;
        const RuntimePreviewFrame* frame = &slot->frame;
        return std::shared_ptr<const RuntimePreviewFrame>(
            std::move(slot), frame);
    } catch (...) {
        return nullptr;
    }
}

PreviewStats RuntimePreviewChannel::stats() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return {enabled_, sampled_frames_, dropped_frames_};
    } catch (...) {
        return {};
    }
}

bool SafetyGate::arm() noexcept {
    if (emergency_stopped_.load(std::memory_order_acquire)) return false;
    armed_.store(true, std::memory_order_release);
    return true;
}

void SafetyGate::disarm() noexcept {
    armed_.store(false, std::memory_order_release);
}

void SafetyGate::set_hold(bool active) noexcept {
    hold_active_.store(active, std::memory_order_release);
}

void SafetyGate::emergency_stop() noexcept {
    armed_.store(false, std::memory_order_release);
    emergency_stopped_.store(true, std::memory_order_release);
}

bool SafetyGate::reset_emergency() noexcept {
    if (hold_active_.load(std::memory_order_acquire)) return false;
    emergency_stopped_.store(false, std::memory_order_release);
    return true;
}

bool SafetyGate::can_dispatch() const noexcept {
    return armed_.load(std::memory_order_acquire) &&
           hold_active_.load(std::memory_order_acquire) &&
           !emergency_stopped_.load(std::memory_order_acquire);
}

bool SafetyGate::armed() const noexcept {
    return armed_.load(std::memory_order_acquire);
}

bool SafetyGate::hold_active() const noexcept {
    return hold_active_.load(std::memory_order_acquire);
}

bool SafetyGate::emergency_stopped() const noexcept {
    return emergency_stopped_.load(std::memory_order_acquire);
}

} // namespace runtime::detail
