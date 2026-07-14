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
    struct TimingSnapshot
    {
        double preprocessMs = 0.0;  // CPU缩放、归一化和NCHW重排耗时，毫秒
        double inferenceMs = 0.0;   // 同步session.Run耗时，包含DirectML执行与ORT内部同步，毫秒
        double copyMs = 0.0;        // ORT返回后到后处理计时起点的交接耗时，毫秒
        double postprocessMs = 0.0; // 输出解码、筛选、NMS和坐标缩放总耗时，毫秒
        double nmsMs = 0.0;         // 后处理内NMS子阶段耗时，已包含在postprocessMs内
        double totalMs = 0.0;       // 前四个互斥阶段之和，不重复累加nmsMs
    };

    DirectMLDetector(const std::string& model_path);
    ~DirectMLDetector();

    // 同步检测单帧
    std::vector<Detection> detect(const cv::Mat& input_frame);
    // 批量检测多帧
    std::vector<std::vector<Detection>> detectBatch(const std::vector<cv::Mat>& frames);

    // DML 推理线程入口（异步模式）
    void dmlInferenceThread();
    // 请求停止推理线程，并唤醒条件变量等待。
    void requestStop() noexcept;
    // 向推理线程提交一帧进行处理
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        std::chrono::steady_clock::time_point frameTimestamp = {});

    int getNumberOfClasses();
    bool isReady() const;
    // 跨推理线程和UI/控制线程读取同一完整发布快照，避免混合不同推理帧。
    TimingSnapshot getTimingSnapshot() const;

private:
    // 推理线程同步状态仅由检测器自身管理。
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit = false;
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
    mutable std::mutex timingMutex; // 保护一次推理对应的完整阶段快照
    TimingSnapshot timingSnapshot;  // 最近一次成功完成后处理的推理耗时
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
    // 仅在一次推理完成全部后处理后发布，失败或提前返回不覆盖上一份有效快照。
    void publishTimingSnapshot(
        std::chrono::duration<double, std::milli> preprocess,
        std::chrono::duration<double, std::milli> inference,
        std::chrono::duration<double, std::milli> copy,
        std::chrono::duration<double, std::milli> postprocess,
        std::chrono::duration<double, std::milli> nms);
    Ort::MemoryInfo memory_info;
};

#endif // DIRECTML_DETECTOR_H
