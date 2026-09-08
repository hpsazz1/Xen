#include "runtime/camera_motion_internal.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace runtime::detail {
namespace {
constexpr double kMinimumGrayDeviation = 1.0;
constexpr double kMinimumPhaseResponse = 0.5;
constexpr double kPatchAgreementRoiRatio = 0.5 / 320.0;

bool intersects(const Detection& detection, const cv::Rect& patch) noexcept {
    // 不可信框不能作为“此处没有前景”的依据；不限类别或置信度。
    if (!std::isfinite(detection.x1) || !std::isfinite(detection.y1) ||
        !std::isfinite(detection.x2) || !std::isfinite(detection.y2) ||
        detection.x1 >= detection.x2 || detection.y1 >= detection.y2) {
        return true;
    }
    return std::max<double>(detection.x1, patch.x) <
               std::min<double>(detection.x2, patch.x + patch.width) &&
           std::max<double>(detection.y1, patch.y) <
               std::min<double>(detection.y2, patch.y + patch.height);
}

bool has_texture(const cv::Mat& gray) {
    cv::Scalar mean, deviation;
    cv::meanStdDev(gray, mean, deviation);
    return deviation[0] >= kMinimumGrayDeviation;
}
} // namespace

void CameraMotionEstimator::reset() noexcept {
    initialized_ = false;
    previous_sequence_ = 0;
    previous_at_ = {};
    if (++epoch_ == 0) ++epoch_;
}

CameraMotionMeasurement CameraMotionEstimator::observe(
    const CapturedFrame& captured, const AimFrame& frame,
    bool clock_reset) noexcept {
    const auto started = std::chrono::steady_clock::now();
    CameraMotionMeasurement result;
    auto& motion = result.motion;
    motion.sequence = frame.sequence;
    motion.captured_at = frame.captured_at;
    const auto finish = [&]() {
        result.observation_epoch = epoch_;
        motion.observation_epoch = epoch_;
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    };
    try {
        if (clock_reset) reset();
        if (captured.storage != CapturedFrameStorage::CPU_BGR) {
            reset();
            motion.status = AimBackgroundMotionStatus::UNSUPPORTED;
            return finish();
        }
        const std::array<double, 8> geometry{
            static_cast<double>(captured.width),
            static_cast<double>(captured.height),
            static_cast<double>(captured.source_width),
            static_cast<double>(captured.source_height),
            captured.roi_x, captured.roi_y,
            captured.source_pixels_per_pixel_x,
            captured.source_pixels_per_pixel_y};
        if (captured.bgr.empty() || captured.bgr.type() != CV_8UC3 ||
            captured.bgr.cols != captured.width ||
            captured.bgr.rows != captured.height ||
            captured.width < 16 || captured.height < 16 ||
            captured.source_width <= 0 || captured.source_height <= 0 ||
            frame.roi_width != captured.width || frame.roi_height != captured.height ||
            !std::all_of(geometry.begin(), geometry.end(),
                         [](double value) { return std::isfinite(value); }) ||
            captured.source_pixels_per_pixel_x <= 0.0 ||
            captured.source_pixels_per_pixel_y <= 0.0 ||
            frame.source_pixels_per_roi_pixel_x !=
                static_cast<float>(captured.source_pixels_per_pixel_x) ||
            frame.source_pixels_per_roi_pixel_y !=
                static_cast<float>(captured.source_pixels_per_pixel_y)) {
            reset();
            motion.status = AimBackgroundMotionStatus::INVALID_GEOMETRY;
            return finish();
        }
        const auto expected_at = captured.timing.source_time_timing_valid
            ? captured.timing.source_time_at : captured.timing.captured_at;
        if (frame.sequence == 0 || frame.sequence != captured.timing.sequence ||
            frame.captured_at == std::chrono::steady_clock::time_point{} ||
            frame.captured_at != expected_at ||
            (initialized_ && (frame.sequence <= previous_sequence_ ||
                              frame.captured_at <= previous_at_))) {
            reset();
            motion.status = AimBackgroundMotionStatus::INVALID_PAIR;
            return finish();
        }
        motion.status = AimBackgroundMotionStatus::WARMING;
        if (initialized_ && geometry != geometry_) {
            reset();
            motion.status = AimBackgroundMotionStatus::INVALID_GEOMETRY;
        }
        if (initialized_ &&
            (captured.timing.source_time_timing_valid != previous_source_time_valid_ ||
             captured.timing.source_clock_session_id != previous_source_session_ ||
             captured.timing.source_time_basis != previous_source_basis_)) {
            reset();
        }
        const int patch_width = captured.width * 96 / 320;
        const int patch_height = captured.height * 160 / 320;
        const std::array<cv::Rect, 2> patches{
            cv::Rect(captured.width * 16 / 320, captured.height * 48 / 320,
                     patch_width, patch_height),
            cv::Rect(captured.width * 208 / 320, captured.height * 48 / 320,
                     patch_width, patch_height)};
        std::array<bool, 2> foreground{};
        for (std::size_t index = 0; index < patches.size(); ++index) {
            cv::cvtColor(captured.bgr(patches[index]), current_gray_[index],
                         cv::COLOR_BGR2GRAY);
            foreground[index] = std::any_of(
                frame.detections.begin(), frame.detections.end(),
                [&](const Detection& detection) {
                    return intersects(detection, patches[index]);
                });
        }
        if (initialized_) {
            motion.previous_sequence = previous_sequence_;
            motion.previous_captured_at = previous_at_;
            const bool foreground_present =
                foreground[0] || foreground[1] ||
                previous_foreground_[0] || previous_foreground_[1];
            const bool textured = has_texture(previous_gray_[0]) &&
                has_texture(previous_gray_[1]) && has_texture(current_gray_[0]) &&
                has_texture(current_gray_[1]);
            if (foreground_present) {
                motion.status = AimBackgroundMotionStatus::FOREGROUND;
            } else if (!textured) {
                motion.status = AimBackgroundMotionStatus::LOW_TEXTURE;
            } else {
                if (window_.size() != patches[0].size()) {
                    cv::createHanningWindow(window_, patches[0].size(), CV_32F);
                }
                std::array<cv::Point2d, 2> shifts;
                std::array<double, 2> responses{};
                for (std::size_t index = 0; index < patches.size(); ++index) {
                    // phaseCorrelate 可能原地乘窗，每对都从未加窗灰度重建。
                    previous_gray_[index].convertTo(previous_float_[index], CV_32F);
                    current_gray_[index].convertTo(current_float_[index], CV_32F);
                    shifts[index] = cv::phaseCorrelate(
                        previous_float_[index], current_float_[index],
                        window_, &responses[index]);
                    if (std::isfinite(responses[index]) &&
                        responses[index] >= kMinimumPhaseResponse &&
                        std::isfinite(shifts[index].x) &&
                        std::isfinite(shifts[index].y) &&
                        std::fabs(shifts[index].x) < patch_width * 0.5 &&
                        std::fabs(shifts[index].y) < patch_height * 0.5) {
                        // 相同灰度输入的观测位移严格为零，避免加窗相关的
                        // 浮点残差携带虚假方向；不按速度或位移容差归零。
                        // 保留原响应/有效性检查，仍按真实帧对消费有效零观测。
                        if (cv::norm(previous_gray_[index], current_gray_[index],
                                     cv::NORM_INF) == 0.0) {
                            shifts[index] = cv::Point2d{};
                        }
                        ++motion.usable_patch_count;
                    }
                }
                if (std::isfinite(responses[0]) && std::isfinite(responses[1])) {
                    motion.min_response = static_cast<float>(
                        std::min(responses[0], responses[1]));
                }
                const double disagreement_x = std::fabs(shifts[0].x - shifts[1].x);
                const double disagreement_y = std::fabs(shifts[0].y - shifts[1].y);
                if (std::isfinite(disagreement_x) && std::isfinite(disagreement_y)) {
                    motion.disagreement_roi_pixels = static_cast<float>(
                        std::max(disagreement_x, disagreement_y));
                }
                const double agreement = std::min(captured.width, captured.height) *
                    kPatchAgreementRoiRatio;
                if (motion.usable_patch_count == 2 &&
                    disagreement_x <= agreement && disagreement_y <= agreement) {
                    motion.dx_roi_pixels = static_cast<float>(
                        (shifts[0].x + shifts[1].x) * 0.5);
                    motion.status = AimBackgroundMotionStatus::VALID;
                } else {
                    motion.status = AimBackgroundMotionStatus::INCONSISTENT;
                }
            }
        }
        // 无效测量也推进图像身份；后续只比较真实相邻消费图，不复用旧位移。
        for (std::size_t index = 0; index < patches.size(); ++index) {
            std::swap(previous_gray_[index], current_gray_[index]);
        }
        previous_foreground_ = foreground;
        geometry_ = geometry;
        previous_sequence_ = frame.sequence;
        previous_at_ = frame.captured_at;
        previous_source_time_valid_ = captured.timing.source_time_timing_valid;
        previous_source_session_ = captured.timing.source_clock_session_id;
        previous_source_basis_ = captured.timing.source_time_basis;
        initialized_ = true;
        return finish();
    } catch (...) {
        reset();
        motion.status = AimBackgroundMotionStatus::ESTIMATION_FAILED;
        return finish();
    }
}

} // namespace runtime::detail
