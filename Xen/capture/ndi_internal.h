#ifndef CAPTURE_NDI_INTERNAL_H
#define CAPTURE_NDI_INTERNAL_H

#include "capture/capture.h"
#include "capture/network_internal.h"

#include <chrono>
#include <memory>

namespace capture::detail {

// receiver 句柄创建成功只代表已发起连接，不能推进此状态机。
// 首帧前使用完整发现预算；收到有效帧后才切换到更短的断流预算。
class NdiSilenceWatchdog final {
public:
    using Clock = std::chrono::steady_clock;

    void reset(Clock::time_point started_at) noexcept {
        received_valid_frame_ = false;
        last_valid_frame_at_ = started_at;
    }

    void record_valid_frame(Clock::time_point received_at) noexcept {
        received_valid_frame_ = true;
        last_valid_frame_at_ = received_at;
    }

    bool received_valid_frame() const noexcept {
        return received_valid_frame_;
    }

    int timeout_ms(int discovery_timeout_ms,
                   int disconnect_timeout_ms) const noexcept {
        return received_valid_frame_
            ? disconnect_timeout_ms
            : discovery_timeout_ms;
    }

    bool expired(Clock::time_point now,
                 int discovery_timeout_ms,
                 int disconnect_timeout_ms) const noexcept {
        const auto silent_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_valid_frame_at_).count();
        return silent_ms >= timeout_ms(
            discovery_timeout_ms, disconnect_timeout_ms);
    }

private:
    bool received_valid_frame_ = false;
    Clock::time_point last_valid_frame_at_{};
};

std::unique_ptr<ICapture> create_ndi_capture(
    const CaptureConfig& config) noexcept;

} // namespace capture::detail

#endif // CAPTURE_NDI_INTERNAL_H
