#include "physical_response_probe.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace
{
double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() % 2 != 0)
        return upper;
    std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
    return (values[middle - 1] + upper) * 0.5;
}

double crossingMs(const std::vector<std::pair<double, double>>& response, double level)
{
    for (size_t index = 1; index < response.size(); ++index)
    {
        const auto& before = response[index - 1];
        const auto& after = response[index];
        if (after.second < level)
            continue;
        const double span = after.second - before.second;
        if (span <= 1e-9)
            return after.first;
        const double ratio = std::clamp((level - before.second) / span, 0.0, 1.0);
        return before.first + (after.first - before.first) * ratio;
    }
    return -1.0;
}
}

bool PhysicalResponseTracker::initialize(const cv::Mat& frame, const cv::Rect& roi, std::string& reason)
{
    if (frame.empty() || roi.width < 8 || roi.height < 8 ||
        roi.x < 0 || roi.y < 0 || roi.x + roi.width > frame.cols || roi.y + roi.height > frame.rows)
    {
        reason = "ROI超出首帧或尺寸小于8像素";
        return false;
    }
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(gray(roi), mean, deviation);
    if (deviation[0] < 15.0)
    {
        reason = "ROI灰度标准差低于15，缺少高对比纹理";
        return false;
    }
    template_gray_ = gray(roi).clone();
    initial_position_ = roi.tl();
    last_position_ = roi.tl();
    return true;
}

PhysicalResponseSample PhysicalResponseTracker::update(const cv::Mat& frame, int64_t receiveNs)
{
    PhysicalResponseSample sample;
    sample.receiveNs = receiveNs;
    if (frame.empty() || template_gray_.empty())
        return sample;

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    constexpr int searchMargin = 48;
    const int left = std::max(0, last_position_.x - searchMargin);
    const int top = std::max(0, last_position_.y - searchMargin);
    const int right = std::min(gray.cols, last_position_.x + template_gray_.cols + searchMargin);
    const int bottom = std::min(gray.rows, last_position_.y + template_gray_.rows + searchMargin);
    const cv::Rect search(left, top, right - left, bottom - top);
    if (search.width < template_gray_.cols || search.height < template_gray_.rows)
        return sample;

    cv::Mat scores;
    cv::matchTemplate(gray(search), template_gray_, scores, cv::TM_CCOEFF_NORMED);
    double maximum = 0.0;
    cv::Point match;
    cv::minMaxLoc(scores, nullptr, &maximum, nullptr, &match);
    sample.trackingQuality = maximum;
    if (!std::isfinite(maximum) || maximum < 0.75)
        return sample;
    last_position_ = search.tl() + match;
    sample.displacementX = static_cast<double>(last_position_.x - initial_position_.x);
    sample.displacementY = static_cast<double>(last_position_.y - initial_position_.y);
    sample.valid = true;
    return sample;
}

PhysicalResponseSummary AnalyzePhysicalResponse(
    const std::vector<PhysicalResponseSample>& samples, int64_t commandConfirmedNs,
    bool xAxis, int baselineMs, int tailMs)
{
    PhysicalResponseSummary result;
    std::vector<double> baselinePrimary;
    std::vector<double> baselineOrthogonal;
    std::vector<std::pair<double, double>> postPrimary;
    std::vector<double> postOrthogonal;
    for (const auto& sample : samples)
    {
        if (!sample.valid)
            continue;
        const double relativeMs = static_cast<double>(sample.receiveNs - commandConfirmedNs) / 1e6;
        const double primary = xAxis ? sample.displacementX : sample.displacementY;
        const double orthogonal = xAxis ? sample.displacementY : sample.displacementX;
        if (relativeMs >= -baselineMs && relativeMs < 0.0)
        {
            baselinePrimary.push_back(primary);
            baselineOrthogonal.push_back(orthogonal);
        }
        if (relativeMs >= 0.0 && relativeMs <= tailMs)
        {
            postPrimary.emplace_back(relativeMs, primary);
            postOrthogonal.push_back(orthogonal);
        }
    }
    result.baselineSamples = baselinePrimary.size();
    result.tailSamples = postPrimary.size();
    if (baselinePrimary.size() < 10 || postPrimary.size() < 20)
    {
        result.reason = "有效基线或尾部帧不足";
        return result;
    }
    const double primaryOrigin = median(baselinePrimary);
    const double orthogonalOrigin = median(baselineOrthogonal);
    const size_t tailCount = std::max<size_t>(5, postPrimary.size() / 5);
    std::vector<double> finalValues;
    std::vector<double> orthogonalFinalValues;
    for (size_t index = postPrimary.size() - tailCount;
         index < postPrimary.size(); ++index)
    {
        finalValues.push_back(postPrimary[index].second - primaryOrigin);
        orthogonalFinalValues.push_back(
            postOrthogonal[index] - orthogonalOrigin);
    }
    result.finalDisplacement = median(std::move(finalValues));
    result.orthogonalFinalDisplacement =
        median(std::move(orthogonalFinalValues));
    if (std::abs(result.finalDisplacement) < 1.0)
    {
        result.reason = "最终位移小于1像素";
        return result;
    }
    std::vector<std::pair<double, double>> normalized;
    double monotonic = 0.0;
    for (const auto& point : postPrimary)
    {
        const double ratio = std::clamp(
            (point.second - primaryOrigin) / result.finalDisplacement,
            0.0, 1.5);
        monotonic = std::max(monotonic, ratio);
        normalized.emplace_back(point.first, monotonic);
    }
    result.t10Ms = crossingMs(normalized, 0.10);
    result.t50Ms = crossingMs(normalized, 0.50);
    result.t90Ms = crossingMs(normalized, 0.90);
    result.t99Ms = crossingMs(normalized, 0.99);
    if (result.t10Ms < 0.0 || result.t50Ms < 0.0 || result.t90Ms < 0.0 || result.t99Ms < 0.0)
    {
        result.reason = "响应未达到全部归一化分位";
        return result;
    }
    result.valid = true;
    result.reason = "ok";
    return result;
}
