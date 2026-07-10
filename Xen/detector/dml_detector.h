#ifndef DIRECTML_DETECTOR_H
#define DIRECTML_DETECTOR_H

// DirectML 推理检测器
// 使用 ONNX Runtime + DirectML 执行提供程序在 GPU 上运行目标检测模型

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>

#include "postProcess.h"

class DirectMLDetector
{
public:
    DirectMLDetector(const std::string& model_path);
    ~DirectMLDetector();

    // 同步检测单帧
    std::vector<Detection> detect(const cv::Mat& input_frame);
    // 批量检测多帧
    std::vector<std::vector<Detection>> detectBatch(const std::vector<cv::Mat>& frames);

    // DML 推理线程入口（异步模式）
    void dmlInferenceThread();
    // 向推理线程提交一帧进行处理
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        std::chrono::steady_clock::time_point frameTimestamp = {});

    int getNumberOfClasses();
    bool isReady() const;

    // 各阶段耗时统计（毫秒）
    std::chrono::duration<double, std::milli> lastInferenceTimeDML;
    std::chrono::duration<double, std::milli> lastPreprocessTimeDML;
    std::chrono::duration<double, std::milli> lastCopyTimeDML;
    std::chrono::duration<double, std::milli> lastPostprocessTimeDML;
    std::chrono::duration<double, std::milli> lastNmsTimeDML;

    // 推理线程同步条件变量
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit = false;

private:
    Ort::Env env;                              // ONNX Runtime 环境
    Ort::Session session{ nullptr };           // ONNX Runtime 会话
    Ort::AllocatorWithDefaultOptions allocator;
    std::atomic<bool> model_ready = false;
    bool using_directml_provider = false;

    std::string input_name;
    std::string output_name;
    std::vector<std::string> output_names;
    std::vector<const char*> output_name_ptrs;
    std::vector<int64_t> input_shape;
    // Sunpoint 模型原始输出模式标志
    bool sunpoint_raw_output = false;
    int heat_output_index = -1;   // Sunpoint 热力图输出索引
    int box_output_index = -1;    // Sunpoint 边界框输出索引
    int offset_output_index = -1; // Sunpoint 偏移量输出索引
    int model_input_h = -1;       // 模型输入高度
    int model_input_w = -1;       // 模型输入宽度

    // 预处理缓冲区
    std::vector<float> input_tensor_values;
    cv::Mat preprocessBgrBuffer;
    cv::Mat preprocessResizeBuffer;
    cv::Mat preprocessFloatBuffer;
    cv::Mat preprocessGrayResizeBuffer;
    cv::Mat preprocessGrayFloatBuffer;

    std::mutex inferenceMutex;
    cv::Mat currentFrame;
    cv::Mat currentSourceFrame;
    std::chrono::steady_clock::time_point currentFrameTimestamp{};
    bool frameReady = false;

    bool initializeModel(const std::string& model_path);
    bool tryInitializeModel(
        const std::string& model_path,
        bool useDirectML,
        GraphOptimizationLevel graphOptimizationLevel,
        const char* providerLabel,
        std::string* error);
    Ort::SessionOptions createSessionOptions(
        bool useDirectML,
        GraphOptimizationLevel graphOptimizationLevel);
    // 将输入帧预处理为模型张量
    void preprocessFrameToTensor(const cv::Mat& frame, float* dst, int target_w, int target_h);
    Ort::MemoryInfo memory_info;
};

#endif // DIRECTML_DETECTOR_H
