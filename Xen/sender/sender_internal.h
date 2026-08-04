#ifndef SENDER_INTERNAL_H
#define SENDER_INTERNAL_H

#include <chrono>

namespace sender::detail {

// Sender 只在成功发送后推进绝对截止时间。编码和网络发送耗时不会被
// 累加进后续周期；落后多个周期时直接跳到 now 之后，禁止突发补发旧帧。
class SenderFramePacer {
public:
    explicit SenderFramePacer(
            std::chrono::steady_clock::duration frame_interval) noexcept
        : frame_interval_(frame_interval) {}

    bool due(std::chrono::steady_clock::time_point now) const noexcept {
        return next_send_at_ == std::chrono::steady_clock::time_point::min() ||
               now >= next_send_at_;
    }

    void record_sent(
            std::chrono::steady_clock::time_point now) noexcept {
        if (next_send_at_ == std::chrono::steady_clock::time_point::min()) {
            next_send_at_ = now + frame_interval_;
            return;
        }

        const auto elapsed = now - next_send_at_;
        const auto skipped_intervals = elapsed / frame_interval_;
        next_send_at_ += frame_interval_ * (skipped_intervals + 1);
    }

    std::chrono::steady_clock::time_point next_send_at() const noexcept {
        return next_send_at_;
    }

private:
    std::chrono::steady_clock::duration frame_interval_;
    std::chrono::steady_clock::time_point next_send_at_ =
        std::chrono::steady_clock::time_point::min();
};

} // namespace sender::detail

#endif // SENDER_INTERNAL_H
