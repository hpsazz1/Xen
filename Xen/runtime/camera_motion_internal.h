#ifndef RUNTIME_CAMERA_MOTION_INTERNAL_H
#define RUNTIME_CAMERA_MOTION_INTERNAL_H

#include <array>
#include <cstdint>

#include "aim/aim.h"
#include "capture/capture.h"

namespace runtime::detail {

struct CameraMotionMeasurement {
    AimBackgroundMotionX motion;
    std::uint64_t observation_epoch = 0;
    double elapsed_ms = 0.0;
};

// 仅持有自有的小块灰度/频域工作区，不延长 Capture 槽位生命周期。
class CameraMotionEstimator final {
public:
    CameraMotionEstimator() = default;
    CameraMotionEstimator(const CameraMotionEstimator&) = delete;
    CameraMotionEstimator& operator=(const CameraMotionEstimator&) = delete;
    void reset() noexcept;
    CameraMotionMeasurement observe(
        const CapturedFrame& captured, const AimFrame& frame,
        bool clock_reset = false) noexcept;

private:
    std::array<cv::Mat, 2> previous_gray_;
    std::array<cv::Mat, 2> current_gray_;
    std::array<cv::Mat, 2> previous_float_;
    std::array<cv::Mat, 2> current_float_;
    cv::Mat window_;
    std::array<bool, 2> previous_foreground_{};
    std::array<double, 8> geometry_{};
    std::uint64_t epoch_ = 1;
    std::uint64_t previous_sequence_ = 0;
    std::chrono::steady_clock::time_point previous_at_{};
    bool previous_source_time_valid_ = false;
    std::uint64_t previous_source_session_ = 0;
    SourceTimeBasis previous_source_basis_ = SourceTimeBasis::UNAVAILABLE;
    bool initialized_ = false;
};

} // namespace runtime::detail

#endif
