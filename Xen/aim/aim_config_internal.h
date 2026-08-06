#ifndef AIM_CONFIG_INTERNAL_H
#define AIM_CONFIG_INTERNAL_H

#include "aim/aim.h"

#include <cmath>

namespace aim::detail {

// 配置文件保存、Runtime 启动和 Aim 热路径必须共享同一套数值契约，避免配置先被
// 接受、随后又在逐帧处理时失败。这里仅校验算法实际消费的范围，不引入设备策略。
inline bool valid_aim_config(const AimConfig& config) noexcept {
    return std::isfinite(config.high_confidence) &&
           std::isfinite(config.low_confidence) &&
           config.high_confidence >= config.low_confidence &&
           config.low_confidence >= 0.0f &&
           config.high_confidence <= 1.0f &&
           config.min_confirmed_hits > 0 && config.max_lost_frames >= 0 &&
           std::isfinite(config.min_iou) &&
           config.min_iou >= 0.0f && config.min_iou <= 1.0f &&
           std::isfinite(config.max_center_distance) &&
           config.max_center_distance > 0.0f &&
           std::isfinite(config.switch_margin) &&
           config.switch_margin >= 0.0f && config.switch_margin < 1.0f &&
           config.switch_confirm_frames > 0 &&
           config.switch_cooldown_frames >= 0 &&
           std::isfinite(config.acquisition_range_percent) &&
           config.acquisition_range_percent >= 5.0f &&
           config.acquisition_range_percent <= 150.0f &&
           std::isfinite(config.body_aim_height_ratio) &&
           config.body_aim_height_ratio >= 0.0f &&
           config.body_aim_height_ratio <= 1.0f &&
           std::isfinite(config.body_aim_range_percent) &&
           config.body_aim_range_percent >= 1.0f &&
           config.body_aim_range_percent <= 100.0f &&
           std::isfinite(config.deadzone_pixels) &&
           config.deadzone_pixels >= 0.0f &&
           std::isfinite(config.smoothing) &&
           config.smoothing >= 0.0f && config.smoothing <= 1.0f &&
           std::isfinite(config.counts_per_pixel_x) &&
           config.counts_per_pixel_x > 0.0f &&
           std::isfinite(config.counts_per_pixel_y) &&
           config.counts_per_pixel_y > 0.0f &&
           std::isfinite(config.max_counts_per_frame) &&
           config.max_counts_per_frame > 0.0f &&
           std::isfinite(config.max_prediction_lead_percent) &&
           config.max_prediction_lead_percent >= 1.0f &&
           config.max_prediction_lead_percent <= 50.0f &&
           std::isfinite(config.predicted_gain) &&
           config.predicted_gain >= 0.0f && config.predicted_gain <= 1.0f;
}

} // namespace aim::detail

#endif // AIM_CONFIG_INTERNAL_H
