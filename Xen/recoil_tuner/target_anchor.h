#ifndef RECOIL_TARGET_ANCHOR_H
#define RECOIL_TARGET_ANCHOR_H

#include <opencv2/core.hpp>
#include <limits>
#include <string>

namespace recoil_tuner {
// 供实验协调器使用的固定模板观测入口；不持有设备，也不提供目标切换。
struct TargetAnchorConfig {
    cv::Rect target_roi, background_roi;
    // 目标与背景平移差的欧氏长度 / 目标ROI短边。由无输出观测预先冻结，0表示未提供。
    double max_relative_shift_normalized = 0;
    // 0使用现有配准的真实图像支撑范围；正值是调用方声明的像素搜索界。
    cv::Size search_limit{};
};

struct TargetAnchorQuality {
    double response = 0, texture_stddev = 0, residual = 0;
    double raw_residual = std::numeric_limits<double>::quiet_NaN();
    double template_score = 0, peak_separation = 0;
    cv::Rect search_region;
    std::string failure;
};

struct TargetAnchorObservation {
    bool valid = false;
    cv::Point2d reference_position{}, position{}, shift{}, background_shift{};
    double relative_shift_normalized = std::numeric_limits<double>::quiet_NaN();
    TargetAnchorQuality target_quality, background_quality;
    std::string failure;
};

class TargetAnchor {
public:
    // 复制并冻结完整参考图；初始化失败会撤销原参考，不能沿用上一轮身份。
    bool initialize(const cv::Mat& reference, const TargetAnchorConfig& config,
        std::string& error) noexcept;
    // 每次直接对冻结参考配准；失败不重定位、不换模板、不提供可用的旧观测。
    TargetAnchorObservation observe(const cv::Mat& frame) const noexcept;

private:
    cv::Mat reference_;
    TargetAnchorConfig config_;
};
}
#endif
