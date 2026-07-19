#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <string>
#include <vector>

struct PhysicalResponseSample
{
    int64_t receiveNs = 0;
    double displacementX = 0.0;
    double displacementY = 0.0;
    double trackingQuality = 0.0;
    bool valid = false;
};

struct PhysicalResponseSummary
{
    bool valid = false;
    std::string reason;
    size_t baselineSamples = 0;
    size_t tailSamples = 0;
    double finalDisplacement = 0.0;
    double orthogonalFinalDisplacement = 0.0;
    double t10Ms = 0.0;
    double t50Ms = 0.0;
    double t90Ms = 0.0;
    double t99Ms = 0.0;
};

/**
 * @brief 固定高对比目标的逐帧稀疏光流跟踪器
 *
 * 模板只从用户指定 ROI 初始化，随后在上一位置附近做归一化相关匹配。搜索窗口有界，
 * 防止跳到远处相似 HUD；输出是相对首帧的像素位移。
 */
class PhysicalResponseTracker
{
public:
    bool initialize(const cv::Mat& frame, const cv::Rect& roi, std::string& reason);
    PhysicalResponseSample update(const cv::Mat& frame, int64_t receiveNs);

private:
    cv::Mat template_gray_;
    cv::Point initial_position_{};
    cv::Point last_position_{};
};

PhysicalResponseSummary AnalyzePhysicalResponse(
    const std::vector<PhysicalResponseSample>& samples,
    int64_t commandConfirmedNs,
    bool xAxis,
    int baselineMs,
    int tailMs);
