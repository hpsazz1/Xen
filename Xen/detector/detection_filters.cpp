// ============================================================
// detection_filters.cpp — DML / TRT 共享检测滤波器实现
// 提取自 dml_detector.cpp(320-449) 与 trt_detector.cpp(81-194)
// ============================================================
#include "detection_filters.h"
#include "../capture/capture.h"
#include "../capture/circle_fov.h"
#include "../config/config.h"
#include <opencv2/opencv.hpp>
#include <algorithm>

extern Config config;

// ----- intersectsDepthMask -----

bool intersectsDepthMask(const cv::Rect& box, const cv::Mat& mask)
{
    if (box.width <= 0 || box.height <= 0 || mask.empty() || mask.type() != CV_8UC1)
        return false;

    const cv::Rect imageBounds(0, 0, mask.cols, mask.rows);
    const cv::Rect clipped = box & imageBounds;
    if (clipped.width <= 0 || clipped.height <= 0)
        return false;

    const int cx = clipped.x + clipped.width / 2;
    const int cy = clipped.y + clipped.height / 2;
    if (mask.at<uint8_t>(cy, cx) != 0)
        return true;

    const cv::Mat roi = mask(clipped);
    return cv::countNonZero(roi) > 0;
}

// ----- filterDetectionsByDepthMask -----

#ifdef USE_CUDA
void filterDetectionsByDepthMask(std::vector<Detection>& detections)
{
    static cv::Mat holdTtl;

    if (detections.empty())
        return;

    if (!config.depth_inference_enabled || !config.depth_mask_enabled)
    {
        holdTtl.release();
        return;
    }

    const int holdFrames = std::clamp(config.depth_mask_hold_frames, 0, 120);
    cv::Mat currentMask = getCurrentDetectionSuppressionMask();
    cv::Mat suppressionMask;

    if (holdFrames <= 0)
    {
        holdTtl.release();
        suppressionMask = currentMask;
    }
    else
    {
        if (!currentMask.empty() && currentMask.type() == CV_8UC1)
        {
            if (holdTtl.empty() || holdTtl.size() != currentMask.size())
                holdTtl = cv::Mat::zeros(currentMask.size(), CV_16UC1);
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
            holdTtl.setTo(cv::Scalar(static_cast<uint16_t>(holdFrames)), currentMask);
        }
        else if (!holdTtl.empty())
        {
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
        }

        if (!holdTtl.empty() && cv::countNonZero(holdTtl) > 0)
        {
            cv::compare(holdTtl, cv::Scalar(0), suppressionMask, cv::CMP_GT);
        }
        else
        {
            suppressionMask.release();
        }
    }

    if (suppressionMask.empty() || suppressionMask.type() != CV_8UC1)
        return;

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&suppressionMask](const Detection& det) { return intersectsDepthMask(det.box, suppressionMask); }),
        detections.end());
}
#else
void filterDetectionsByDepthMask(std::vector<Detection>&)
{
    // 深度掩码功能仅在 CUDA 路径中可用，DML 编译下为空操作
}
#endif

// ----- filterDetectionsByCircleFov -----

void filterDetectionsByCircleFov(std::vector<Detection>& detections)
{
    if (detections.empty() || !config.circle_fov_enabled)
        return;

    const cv::Size detectionSize(config.detection_resolution, config.detection_resolution);
    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&detectionSize](const Detection& det)
            {
                const cv::Point2f center(
                    static_cast<float>(det.box.x) + static_cast<float>(det.box.width) * 0.5f,
                    static_cast<float>(det.box.y) + static_cast<float>(det.box.height) * 0.5f);
                return !pointInsideCircleFov(center, detectionSize, config.circle_fov_radius_percent);
            }),
        detections.end());
}
