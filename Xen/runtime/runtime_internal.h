#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "capture/capture.h"

namespace runtime::detail {

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
