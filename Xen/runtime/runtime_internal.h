#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "capture/capture.h"

namespace runtime::detail {

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
