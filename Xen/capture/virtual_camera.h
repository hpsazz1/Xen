#ifndef VIRTUAL_CAMERA_H
#define VIRTUAL_CAMERA_H

// 虚拟摄像头捕获类
// 通过 OpenCV VideoCapture 读取虚拟摄像头设备流作为输入源

#include <opencv2/opencv.hpp>

#include "capture.h"
#include "Xen.h"

class VirtualCameraCapture final : public IScreenCapture
{
public:
    VirtualCameraCapture(int width, int height, const std::string& cameraName, int captureFps, bool verbose);
    ~VirtualCameraCapture() override;

    cv::Mat GetNextFrameCpu() override;

    // 获取可用虚拟摄像头列表
    static std::vector<std::string> GetAvailableVirtualCameras(bool forceRescan = false);
    // 清除缓存的摄像头列表
    static void ClearCachedCameraList();

private:
    std::unique_ptr<cv::VideoCapture> cap_;  // OpenCV 视频捕获对象
    int captureWidth{ 0 }, captureHeight{ 0 };
    int targetWidth_{ 0 };
    int targetHeight_{ 0 };
    std::string selectedCameraName_;
    int captureFps_{ 0 };
    double declaredFps_{ 0.0 }; // 设备驱动在打开后实际报告的帧率；0 表示不可用
    bool verbose_{ false };

    int roiW_{ 0 }, roiH_{ 0 };  // ROI 裁剪尺寸

    cv::Mat frameCpu;
};

#endif // VIRTUAL_CAMERA_H
