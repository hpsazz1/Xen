#ifdef USE_CUDA
#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

// TensorRT 推理检测器
// 使用 NVIDIA TensorRT 在 CUDA GPU 上运行目标检测模型，支持 CUDA Graph 和异步推理

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <NvInfer.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cuda_fp16.h>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include <cuda_runtime_api.h>

#include "postProcess.h"
#include "runtime/aim_pipeline_types.h"

class TrtDetector
{
public:
    TrtDetector();
    ~TrtDetector();
    // 初始化 TensorRT 引擎
    void initialize(const std::string& modelFile);
    // 提交 CPU 帧进行推理（异步）
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        FrameTiming frameTiming = {});
    // 提交 GPU 帧进行推理（异步，避免 CPU-GPU 传输）
    void processFrameGpu(
        const cv::cuda::GpuMat& frame,
        FrameTiming frameTiming = {});
    // 推理线程入口
    void inferenceThread();
    // 请求停止推理线程
    void requestStop();
    bool isInitialized() const;
    // 同步检测单帧（阻塞）
    std::vector<Detection> detect(const cv::Mat& frame);

    float img_scale;  // 图像缩放比例

    // 输入/输出绑定名称
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::unordered_map<std::string, size_t> outputSizes;

    // 各阶段耗时统计（毫秒）
    std::chrono::duration<double, std::milli> lastPreprocessTime;
    std::chrono::duration<double, std::milli> lastInferenceTime;
    std::chrono::duration<double, std::milli> lastCopyTime;
    std::chrono::duration<double, std::milli> lastPostprocessTime;
    std::chrono::duration<double, std::milli> lastNmsTime;

private:
    std::unique_ptr<nvinfer1::IRuntime> runtime;          // TensorRT 运行时
    std::unique_ptr<nvinfer1::ICudaEngine> engine;        // TensorRT 引擎
    std::unique_ptr<nvinfer1::IExecutionContext> context; // 推理执行上下文

    cudaStream_t stream;  // CUDA 流

    // CUDA Graph 加速
    bool useCudaGraph;
    bool cudaGraphCaptured;
    cudaGraph_t cudaGraph;
    cudaGraphExec_t cudaGraphExec;
    void captureCudaGraph();   // 捕获 CUDA Graph
    void launchCudaGraph();    // 启动 CUDA Graph
    void destroyCudaGraph();   // 销毁 CUDA Graph

    // 固定内存输出缓冲区（加速 CPU 回读）
    std::unordered_map<std::string, void*> pinnedOutputBuffers;
    void allocatePinnedOutputs();
    void freePinnedOutputs();

    std::mutex inferenceMutex;
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit;
    cv::Mat currentFrame;
    cv::Mat currentSourceFrame;
    cv::cuda::GpuMat currentFrameGpu;
    FrameTiming currentFrameTiming{};
    bool frameReady;

    // 待处理帧类型枚举
    enum class PendingFrameType
    {
        None = 0,
        Cpu = 1,
        Gpu = 2
    };
    PendingFrameType pendingFrameType = PendingFrameType::None;

    void loadEngine(const std::string& engineFile);  // 加载序列化引擎

    void preProcess(const cv::Mat& frame);           // CPU 端预处理
    void preProcess(const cv::cuda::GpuMat& frame);  // GPU 端预处理
    void copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer);

    // GPU 预处理缓冲区
    cv::cuda::GpuMat gpuFrameBuffer;
    cv::cuda::GpuMat gpuResizedBuffer;
    cv::cuda::GpuMat gpuFloatBuffer;
    cv::cuda::Stream cvStream;

    // CPU 预处理缓冲区
    cv::Mat cpuBgrBuffer;
    cv::Mat cpuResizedBuffer;
    cv::Mat cpuFloatBuffer;
    std::vector<float> inputHostBuffer;

    // TensorRT 输出的后处理
    std::vector<Detection> postProcess(
        const float* output,
        const std::string& outputName,
        std::chrono::duration<double, std::milli>* nmsTime
    );

    void getInputNames();
    void getOutputNames();
    void getBindings();

    std::unordered_map<std::string, size_t> inputSizes;
    std::unordered_map<std::string, void*> inputBindings;
    std::unordered_map<std::string, void*> outputBindings;
    std::unordered_map<std::string, std::vector<int64_t>> outputShapes;
    int numClasses;

    size_t getSizeByDim(const nvinfer1::Dims& dims);
    size_t getElementSize(nvinfer1::DataType dtype);

    std::string inputName;
    void* inputBufferDevice;  // 设备端输入缓冲区

    std::unordered_map<std::string, nvinfer1::DataType> outputTypes;
    std::unordered_map<std::string, std::vector<float>> fp16OutputScratch; // FP16 输出中转缓冲区

    // CUDA 事件（用于异步推理计时）
    cudaEvent_t preprocessStartEvent = nullptr;
    cudaEvent_t inferenceStartEvent = nullptr;
    cudaEvent_t inferenceCompleteEvent = nullptr;
    cudaEvent_t copyCompleteEvent = nullptr;
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
#endif
