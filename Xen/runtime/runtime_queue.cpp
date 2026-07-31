#include "runtime/runtime_internal.h"

#include <chrono>
#include <memory>

namespace runtime::detail {

LatestFrameQueue::LatestFrameQueue() {
    for (auto& slot : pool_) {
        slot = std::make_shared<CapturedFrame>();
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
    if (!frame || frame->timing.sequence == 0) return;
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
