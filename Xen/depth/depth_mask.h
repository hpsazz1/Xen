#pragma once
#ifdef USE_CUDA

// 深度遮罩生成模块
// 利用 Depth Anything 深度估计结果生成二值遮罩，用于区分前景和背景

#include <opencv2/opencv.hpp>
#include <chrono>
#include <mutex>
#include <string>

namespace nvinfer1
{
    class ILogger;
}

namespace depth_anything
{
    class DepthAnythingTrt;

    // 深度遮罩配置选项
    struct DepthMaskOptions
    {
        bool enabled = false;   // 是否启用深度遮罩
        int fps = 5;            // 遮罩更新帧率
        int near_percent = 20;  // 近处百分比阈值（用于区分近/远）
        int expand = 0;         // 遮罩膨胀像素数
        bool invert = false;    // 是否反转遮罩
    };

    // 深度遮罩调试状态
    struct DepthMaskDebugState
    {
        bool initialized = false;
        bool has_model = false;
        bool model_ready = false;
        std::string last_model_path;
    };

    // 深度遮罩生成器
    class DepthMaskGenerator
    {
    public:
        // 更新遮罩：输入帧和配置，生成新遮罩
        void update(const cv::Mat& frame, const DepthMaskOptions& options,
            const std::string& modelPath, nvinfer1::ILogger& logger);
        // 获取当前二值遮罩
        cv::Mat getMask() const;
        bool ready() const;
        std::string lastError() const;
        std::chrono::steady_clock::time_point lastAttemptTime() const;
        std::pair<int, int> lastFrameSize() const;
        DepthMaskDebugState debugState() const;
        void reset();

    private:
        mutable std::mutex state_mutex;
        cv::Mat mask_binary;                                                        // 二值遮罩结果
        std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_attempt = std::chrono::steady_clock::time_point::min();
        int last_frame_w = 0;
        int last_frame_h = 0;
        std::string last_model_path;
        std::string last_error;
        bool initialized = false;

        class DepthAnythingTrt* model = nullptr;  // 深度估计模型实例
    };

    // 获取全局深度遮罩生成器实例
    DepthMaskGenerator& GetDepthMaskGenerator();
}

#endif
