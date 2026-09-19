#ifndef RECOIL_WALL_REGISTRATION_INTERNAL_H
#define RECOIL_WALL_REGISTRATION_INTERNAL_H
#include <opencv2/core.hpp>
#include <limits>
#include <string>

namespace recoil_tuner::detail {
inline constexpr const char* kWallRegistration = "bounded_patch_midpoint_filtered_v3";
inline constexpr const char* kWallSearchPolicy = "real_template_midpoint_support_v1";
struct WallRegistration {
    cv::Point2d shift;
    cv::Rect search_region;
    cv::Size explicit_search_limit;
    double response = 0, texture_stddev = 0, residual = 0;
    double raw_residual = std::numeric_limits<double>::quiet_NaN();
    double template_score = 0, peak_separation = 0;
    std::string failure;
};
// 三个调用层共用真实有界区域对应；标定另传既有64像素搜索上限。
WallRegistration register_wall(const cv::Mat& before, const cv::Mat& after,
    const cv::Rect& roi, cv::Size search_limit = {});
}
#endif
