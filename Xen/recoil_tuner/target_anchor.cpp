#include "recoil_tuner/target_anchor.h"
#include "recoil_tuner/wall_registration_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace recoil_tuner {
namespace {
bool supported_image(const cv::Mat& image) {
    return !image.empty() && image.dims == 2 && image.depth() == CV_8U &&
        (image.channels() == 1 || image.channels() == 3 || image.channels() == 4) &&
        image.total() <= 33554432;
}

bool supported_roi(const cv::Rect& roi, cv::Size size) {
    // 包含既有中点残差配准所需的1像素真实支撑，不允许越界或整数溢出。
    return roi.width >= 32 && roi.height >= 32 && roi.x >= 1 && roi.y >= 1 &&
        static_cast<std::int64_t>(roi.x) + roi.width < size.width &&
        static_cast<std::int64_t>(roi.y) + roi.height < size.height;
}

TargetAnchorQuality quality(const detail::WallRegistration& value) {
    TargetAnchorQuality result;
    result.response = value.response;
    result.texture_stddev = value.texture_stddev;
    result.residual = value.residual;
    result.raw_residual = value.raw_residual;
    result.template_score = value.template_score;
    result.peak_separation = value.peak_separation;
    result.search_region = value.search_region;
    result.failure = value.failure;
    return result;
}
}

bool TargetAnchor::initialize(const cv::Mat& reference, const TargetAnchorConfig& config,
    std::string& error) noexcept {
    reference_.release();
    config_ = {};
    try {
        const auto reject = [&](const char* reason) { error = reason; return false; };
        if (!supported_image(reference) || !supported_roi(config.target_roi, reference.size()) ||
            !supported_roi(config.background_roi, reference.size()))
            return reject("invalid_anchor_geometry");
        if (!std::isfinite(config.max_relative_shift_normalized) ||
            config.max_relative_shift_normalized <= 0 || config.max_relative_shift_normalized > 1 ||
            config.search_limit.width < 0 || config.search_limit.height < 0)
            return reject("invalid_anchor_policy");
        // 目标模板与背景模板连同滤波halo均不相交，避免重复使用同一块纹理充当独立核对。
        const cv::Rect target_support(config.target_roi.x - 1, config.target_roi.y - 1,
            config.target_roi.width + 2, config.target_roi.height + 2);
        const cv::Rect background_support(config.background_roi.x - 1, config.background_roi.y - 1,
            config.background_roi.width + 2, config.background_roi.height + 2);
        if ((target_support & background_support).area() != 0)
            return reject("anchor_regions_not_independent");

        // 初始化同时检查两块模板的纹理、歧义和支撑，不能在开火后才发现不可用。
        const auto target = detail::register_wall(reference, reference, config.target_roi, config.search_limit);
        if (!target.failure.empty()) { error = "target_" + target.failure; return false; }
        const auto background = detail::register_wall(reference, reference, config.background_roi, config.search_limit);
        if (!background.failure.empty()) { error = "background_" + background.failure; return false; }
        reference_ = reference.clone();
        config_ = config;
        error.clear();
        return true;
    } catch (...) {
        reference_.release();
        error = "anchor_initialization_failed";
        return false;
    }
}

TargetAnchorObservation TargetAnchor::observe(const cv::Mat& frame) const noexcept {
    TargetAnchorObservation result;
    try {
        if (reference_.empty()) { result.failure = "anchor_not_initialized"; return result; }
        const auto& roi = config_.target_roi;
        result.reference_position = {roi.x + (roi.width - 1) * 0.5, roi.y + (roi.height - 1) * 0.5};
        if (!supported_image(frame) || frame.size() != reference_.size() || frame.type() != reference_.type()) {
            result.failure = "anchor_frame_geometry_changed";
            return result;
        }
        const auto target = detail::register_wall(reference_, frame, roi, config_.search_limit);
        const auto background = detail::register_wall(reference_, frame, config_.background_roi, config_.search_limit);
        result.target_quality = quality(target);
        result.background_quality = quality(background);
        result.shift = target.shift;
        result.background_shift = background.shift;
        result.position = result.reference_position + result.shift;
        if (!target.failure.empty()) { result.failure = "target_" + target.failure; return result; }
        if (!background.failure.empty()) { result.failure = "background_" + background.failure; return result; }
        result.relative_shift_normalized = std::hypot(target.shift.x - background.shift.x,
            target.shift.y - background.shift.y) / std::min(roi.width, roi.height);
        if (!std::isfinite(result.relative_shift_normalized) ||
            result.relative_shift_normalized > config_.max_relative_shift_normalized) {
            result.failure = "target_background_motion_inconsistent";
            return result;
        }
        // 背景仅核对共同运动，绝不从目标位移中扣除相机运动；后者正是观测信号。
        // 此处只有平移拟合。尺度/姿态变化由匹配残差拒绝，不宣称测得了尺度或真实射向。
        result.valid = true;
    } catch (...) {
        result.valid = false;
        result.failure = "anchor_observation_failed";
    }
    return result;
}
}
