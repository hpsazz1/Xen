#include "recoil_tuner/recoil_tuner.h"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace recoil_tuner {
namespace {
cv::Mat gray(const cv::Mat& image) {
    if (image.channels() == 1) return image;
    cv::Mat result;
    cv::cvtColor(image, result, image.channels() == 3 ? cv::COLOR_BGR2GRAY : cv::COLOR_BGRA2GRAY);
    return result;
}
bool inside(const cv::Rect& roi, const cv::Size& size) {
    return roi.width > 0 && roi.height > 0 && roi.x >= 0 && roi.y >= 0 &&
        static_cast<std::int64_t>(roi.x) + roi.width <= size.width && static_cast<std::int64_t>(roi.y) + roi.height <= size.height;
}
}
ImageMeasurement measure_image_pair(const cv::Mat& before, const cv::Mat& after, const ImageRequest& request) noexcept {
    ImageMeasurement result;
    result.reference = request.reference;
    try {
        if (before.empty() || after.empty() || before.size() != after.size() || before.type() != after.type() || before.depth() != CV_8U ||
            (before.channels() != 1 && before.channels() != 3 && before.channels() != 4) || before.total() > 33554432 ||
            !request.reference_confirmed || !std::isfinite(request.reference.x) || !std::isfinite(request.reference.y) ||
            request.reference.x < 0 || request.reference.y < 0 || request.reference.x >= before.cols || request.reference.y >= before.rows ||
            !inside(request.registration_roi, before.size()) || !inside(request.measurement_roi, before.size()) ||
            (request.registration_roi & request.measurement_roi).area() != 0 || request.registration_roi.width < 32 || request.registration_roi.height < 32 ||
            !std::isfinite(request.max_translation_pixels) || request.max_translation_pixels < 0 || request.max_translation_pixels > 64 ||
            !std::isfinite(request.min_registration_response) || request.min_registration_response < 0.1 || request.min_registration_response > 1 ||
            !std::isfinite(request.threshold) || request.threshold < 1 || request.threshold > 254 ||
            !std::isfinite(request.min_area) || !std::isfinite(request.max_area) || request.min_area < 1 || request.max_area < request.min_area ||
            !std::isfinite(request.max_changed_fraction) || request.max_changed_fraction <= 0 || request.max_changed_fraction > 0.2) {
            result.message = "图像、人工参考锚点或互不相交的测量区域无效。"; return result;
        }
        const auto a = gray(before), b = gray(after);
        cv::Scalar mean, deviation;
        cv::meanStdDev(a(request.registration_roi), mean, deviation);
        if (deviation[0] < 5.0) { result.message = "背景纹理不足，不能建立平移配准。"; return result; }
        cv::Mat af, bf, window;
        a(request.registration_roi).convertTo(af, CV_64F);
        b(request.registration_roi).convertTo(bf, CV_64F);
        cv::createHanningWindow(window, af.size(), CV_64F);
        result.translation = cv::phaseCorrelate(af, bf, window, &result.registration_response);
        if (!std::isfinite(result.translation.x) || !std::isfinite(result.translation.y) || !std::isfinite(result.registration_response) ||
            result.registration_response < request.min_registration_response || cv::norm(result.translation) > request.max_translation_pixels) {
            result.message = "配准置信度或平移超出已声明范围，拒绝自动测量。"; return result;
        }
        const int border = static_cast<int>(std::ceil(request.max_translation_pixels)) + 1;
        const auto& roi = request.measurement_roi;
        if (roi.x < border || roi.y < border || roi.x + roi.width > before.cols - border || roi.y + roi.height > before.rows - border) {
            result.message = "测量区域靠近配准边界，可能包含填充伪影。"; return result;
        }
        cv::Mat aligned;
        const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, -result.translation.x, 0, 1, -result.translation.y);
        cv::warpAffine(b, aligned, transform, a.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
        cv::Mat background_error;
        cv::absdiff(a(request.registration_roi), aligned(request.registration_roi), background_error);
        if (cv::mean(background_error)[0] > 8.0) { result.message = "平移后背景仍不一致，可能有透视、亮度或场景变化。"; return result; }
        cv::Mat difference, binary;
        // 只提取新增暗斑；特效/弹孔身份仍需人工确认，不自动写为真实弹点。
        cv::subtract(a(roi), aligned(roi), difference);
        cv::threshold(difference, binary, request.threshold, 255, cv::THRESH_BINARY);
        if (static_cast<double>(cv::countNonZero(binary)) / binary.total() > request.max_changed_fraction) {
            result.message = "变化区域过大，不能区分弹孔与遮挡或特效。"; return result;
        }
        cv::Mat labels, stats, centers;
        const int components = cv::connectedComponentsWithStats(binary, labels, stats, centers, 8, CV_32S);
        for (int i = 1; i < components; ++i) {
            const int area = stats.at<int>(i, cv::CC_STAT_AREA), width = stats.at<int>(i, cv::CC_STAT_WIDTH), height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            const int left = stats.at<int>(i, cv::CC_STAT_LEFT), top = stats.at<int>(i, cv::CC_STAT_TOP);
            if (area < request.min_area || area > request.max_area || width > 4 * height || height > 4 * width ||
                left == 0 || top == 0 || left + width >= roi.width || top + height >= roi.height) continue;
            result.candidate_centers.emplace_back(roi.x + centers.at<double>(i, 0), roi.y + centers.at<double>(i, 1));
        }
        if (result.candidate_centers.empty() || result.candidate_centers.size() > 32) {
            result.candidate_centers.clear(); result.message = "没有可用暗斑或候选过多；请人工核对图像。"; return result;
        }
        result.valid = true;
        result.message = "已提取配准后的新增暗斑候选；须人工确认弹孔身份，无逐发或时间归属。";
        return result;
    } catch (...) { result.message = "图像测量失败；未生成可用于拟合的观测。"; return result; }
}
} // namespace recoil_tuner
