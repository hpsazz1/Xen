#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>

#include "trt_detector.h"
#include "nvinf.h"
#include "Xen.h"
#include "other_tools.h"
#include "postProcess.h"
#include "cuda_preprocess.h"
#include "depth/depth_mask.h"
#include "capture.h"
#include "capture/circle_fov.h"
#include "scr/data_collector.h"
#include "detection_filters.h"

extern std::atomic<bool> detectionPaused;
/*
 * 全局变量：记录模型量化类型
 * 由外部使用，表示当前模型的量化精度（如 FP32、FP16、INT8 等）
 */
int model_quant;

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

/*
 * 全局变量：是否已记录过错误消息
 * 用于避免在推理线程中重复输出相同的初始化失败错误
 */
static bool error_logged = false;

namespace {
/*
 * 匿名命名空间：尝试将 int64_t 安全转换为 int
 * 在 TensorRT 维度值（int64_t）与 OpenCV / 标准库的 int 接口之间进行安全转换
 *
 * @param value 输入的 int64_t 值
 * @param out   输出参数，转换后的 int 值
 * @return 转换成功返回 true，溢出时返回 false
 */
bool tryGetDimInt(int64_t value, int* out)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    *out = static_cast<int>(value);
    return true;
}

/*
 * 尝试将 int64_t 转换为正 int
 * 在 tryGetDimInt 的基础上增加了正值检查（维度必须 > 0）
 *
 * @param value 输入的 int64_t 值
 * @param out   输出参数，转换后的正 int 值
 * @return 值为正且转换成功返回 true，否则返回 false
 */
bool tryGetPositiveDimInt(int64_t value, int* out)
{
    if (value <= 0)
        return false;
    return tryGetDimInt(value, out);
}

} // namespace

/*
 * 构造函数：初始化检测器对象
 * - 初始化所有成员变量为默认值
 * - 创建 CUDA 流，用于所有 GPU 操作
 * - 事件指针均初始化为 nullptr
 */
TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    cudaGraph(nullptr),
    cudaGraphExec(nullptr),
    inputBufferDevice(nullptr),
    img_scale(1.0f),
    numClasses(0)
{
    stream = nullptr;
    if (cudaStreamCreate(&stream) != cudaSuccess)
    {
        std::cerr << "[Detector] cudaStreamCreate failed; stream is null" << std::endl;
        stream = nullptr;
    }
}

/*
 * 析构函数：释放所有 GPU 和主机资源
 * 清理顺序：
 * 1. 请求推理线程停止
 * 2. 销毁 CUDA Graph
 * 3. 释放锁页主机内存
 * 4. 释放所有输入/输出绑定设备内存及输入缓冲区
 * 5. 销毁所有 CUDA 事件
 * 6. 销毁 CUDA 流
 */
TrtDetector::~TrtDetector()
{
    requestStop();
    destroyCudaGraph();
    freePinnedOutputs();

    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);
    if (stream) cudaStreamDestroy(stream);
}

/*
 * 请求停止推理线程
 * 设置 shouldExit 标志为 true，并唤醒所有在条件变量上等待的线程
 */
void TrtDetector::requestStop()
{
    shouldExit = true;
    inferenceCV.notify_all();
}

/*
 * 检查检测器是否已初始化
 *
 * @return 如果 TensorRT 执行上下文已创建则返回 true，否则返回 false
 */
bool TrtDetector::isInitialized() const
{
    return context != nullptr;
}

/*
 * 释放所有锁页主机输出缓冲区（pinned memory）
 * 遍历 pinnedOutputBuffers 映射表，对每个缓冲区调用 cudaFreeHost 释放
 */
void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
}

/*
 * 为所有输出张量分配锁页主机缓冲区（pinned memory）
 * 在调用前会先释放已有的缓冲区，然后为每个输出张量
 * 分配与 outputSizes 中记录大小一致的锁页内存
 * 锁页内存用于 GPU 到主机的高速异步拷贝
 */
void TrtDetector::allocatePinnedOutputs()
{
    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        const size_t bytes = outputSizes[name];
        if (bytes == 0) continue;

        void* hostPtr = nullptr;
        cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                << " (" << bytes << " bytes): " << cudaGetErrorString(err) << std::endl;
            continue;
        }

        pinnedOutputBuffers[name] = hostPtr;

        if (config.verbose)
        {
            std::cout << "[Detector] Allocated pinned host buffer for output " << name
                << ": " << bytes << " bytes" << std::endl;
        }
    }
}

/*
 * 销毁 CUDA Graph 资源
 * 先释放可执行的 Graph 实例（cudaGraphExec），再释放原始 Graph 定义
 * 重置 cudaGraphCaptured 标志
 */
void TrtDetector::destroyCudaGraph()
{
    if (cudaGraphExec)
    {
        cudaGraphExecDestroy(cudaGraphExec);
        cudaGraphExec = nullptr;
    }
    if (cudaGraph)
    {
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
    }
    cudaGraphCaptured = false;
}

/*
 * 捕获 CUDA Graph
 * 通过 cudaStreamBeginCapture / cudaStreamEndCapture 将推理和结果拷贝操作
 * 捕获为一个 CUDA Graph，实例化后可重复使用以降低 launch 开销
 *
 * 图内包含的操作：
 * 1. enqueueV3（推理）
 * 2. 记录推理完成事件
 * 3. 异步将各输出从设备拷贝到锁页主机内存
 *
 * 要求：只有 useCudaGraph 为 true 且尚未捕获时才执行
 */
void TrtDetector::captureCudaGraph()
{
    if (!useCudaGraph || cudaGraphCaptured) return;

    destroyCudaGraph();

    cudaStreamSynchronize(stream);

    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    if (!context->enqueueV3(stream))
    {
        std::cerr << "[Detector] enqueueV3 failed during graph capture" << std::endl;
        cudaStreamEndCapture(stream, &cudaGraph);
        if (cudaGraph) { cudaGraphDestroy(cudaGraph); cudaGraph = nullptr; }
        return;
    }
    cudaEventRecord(inferenceCompleteEvent, stream);

    for (const auto& name : outputNames)
        if (pinnedOutputBuffers.count(name))
            if (cudaMemcpyAsync(pinnedOutputBuffers[name],
                    outputBindings[name],
                    outputSizes[name],
                    cudaMemcpyDeviceToHost,
                    stream) != cudaSuccess)
                std::cerr << "[Detector] cudaMemcpyAsync(output) failed during capture: " << name << std::endl;

    st = cudaStreamEndCapture(stream, &cudaGraph);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    st = cudaGraphInstantiate(&cudaGraphExec, cudaGraph, 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
        return;
    }

    cudaGraphCaptured = true;
}

/*
 * 执行已捕获的 CUDA Graph
 * 调用 cudaGraphLaunch 提交预录制的 Graph 执行。
 * 内联定义以减少函数调用开销。
 */
inline void TrtDetector::launchCudaGraph()
{
    auto err = cudaGraphLaunch(cudaGraphExec, stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch failed: " << cudaGetErrorString(err) << std::endl;
    }
}

/*
 * 枚举并记录引擎的所有输入张量
 * 遍历 TensorRT 引擎的所有 I/O 张量，筛选出模式为 kINPUT 的张量，
 * 将其名称保存到 inputNames 列表中并记录对应的尺寸信息
 */
void TrtDetector::getInputNames()
{
    inputNames.clear();
    inputSizes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        {
            inputNames.emplace_back(name);
            if (config.verbose)
            {
                std::cout << "[Detector] Detected input: " << name << std::endl;
            }
        }
    }
}

/*
 * 枚举并记录引擎的所有输出张量
 * 遍历 TensorRT 引擎的所有 I/O 张量，筛选出模式为 kOUTPUT 的张量，
 * 保存名称、数据类型和尺寸信息到对应的映射表中
 */
void TrtDetector::getOutputNames()
{
    outputNames.clear();
    outputSizes.clear();
    outputTypes.clear();
    outputShapes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        {
            outputNames.emplace_back(name);
            outputTypes[name] = engine->getTensorDataType(name);

            if (config.verbose)
            {
                std::cout << "[Detector] Detected output: " << name << std::endl;
            }
        }
    }
}

/*
 * 为所有输入和输出张量分配设备内存（GPU）
 * - 先释放已存在的绑定内存
 * - 根据 inputSizes / outputSizes 中记录的字节数调用 cudaMalloc
 * - 将分配的设备指针存入 inputBindings / outputBindings 映射表
 */
void TrtDetector::getBindings()
{
    for (auto& binding : inputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    inputBindings.clear();

    for (auto& binding : outputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    outputBindings.clear();

    for (const auto& name : inputNames)
    {
        size_t size = inputSizes[name];
        if (size > 0)
        {
            void* ptr = nullptr;

            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                inputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for input " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate input memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }

    for (const auto& name : outputNames)
    {
        size_t size = outputSizes[name];
        if (size > 0) {
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                outputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for output " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate output memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }
}

/*
 * 初始化检测器：加载引擎、创建执行上下文、分配资源
 *
 * 主要步骤：
 * 1. 创建 TensorRT 推理运行时
 * 2. 加载引擎文件（支持 .engine 和 .onnx 格式）
 * 3. 创建执行上下文
 * 4. 获取输入/输出张量名称和尺寸信息
 * 5. 判断输入是否为动态尺寸，自动设置 fixed_input_size
 * 6. 设置输入张量形状（若为动态）
 * 7. 计算各张量的字节大小并保存
 * 8. 分配 GPU 设备内存和锁页主机内存
 * 9. 估算类别数 numClasses
 * 10. 创建预处理所需的 GPU/CPU 缓冲区和 CUDA 事件
 * 11. 设置 TensorRT 上下文的张量地址
 * 12. 若启用 CUDA Graph 则进行捕获
 *
 * @param modelFile 模型文件路径（.engine 或 .onnx）
 */
void TrtDetector::initialize(const std::string& modelFile)
{
    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    loadEngine(modelFile);
    if (!engine)
    {
        std::cerr << "[Detector] Engine loading failed" << std::endl;
        return;
    }

    context.reset(engine->createExecutionContext());
    if (!context)
    {
        std::cerr << "[Detector] Context creation failed" << std::endl;
        return;
    }

    getInputNames();
    getOutputNames();
    if (inputNames.empty())
    {
        std::cerr << "[Detector] No input tensors found" << std::endl;
        return;
    }
    inputName = inputNames[0];

    nvinfer1::Dims modelInputDims = engine->getTensorShape(inputName.c_str());
    bool isStatic = true;
    for (int i = 0; i < modelInputDims.nbDims; ++i)
        if (modelInputDims.d[i] <= 0) isStatic = false;

    if (isStatic != config.fixed_input_size)
    {
        config.fixed_input_size = isStatic;
        detector_model_changed.store(true);
        std::cout << "[Detector] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    const int target = config.detection_resolution;
    nvinfer1::Dims inputDims = modelInputDims;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, target, target };
        context->setInputShape(inputName.c_str(), newShape);
        if (!context->allInputDimensionsSpecified())
        {
            std::cerr << "[Detector] Failed to set input dimensions" << std::endl;
            return;
        }
        inputDims = context->getTensorShape(inputName.c_str());
    }
    else
    {
        inputDims = context->getTensorShape(inputName.c_str());
    }

    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    fp16OutputScratch.clear();

    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(inName.c_str());
        inputSizes[inName] = getSizeByDim(d) * getElementSize(dt);
    }
    for (const auto& outName : outputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(outName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(outName.c_str());
        outputSizes[outName] = getSizeByDim(d) * getElementSize(dt);
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        outputShapes[outName] = std::move(shape);
        outputTypes[outName] = dt;
    }

    getBindings();

    allocatePinnedOutputs();

    if (!outputNames.empty())
    {
        const std::string& mainOut = outputNames[0];
        nvinfer1::Dims outDims = context->getTensorShape(mainOut.c_str());
        const int64_t outChannels = (outDims.nbDims >= 2) ? outDims.d[outDims.nbDims - 2] : 0;
        const int64_t classes64 = (outChannels > 4) ? (outChannels - 4) : 1;
        int classes = 0;
        if (!tryGetDimInt(classes64, &classes) || classes <= 0)
        {
            numClasses = 0;
        }
        else
        {
            numClasses = classes;
        }
    }

    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(inputDims.d[1], &c)
        || !tryGetPositiveDimInt(inputDims.d[2], &h)
        || !tryGetPositiveDimInt(inputDims.d[3], &w))
    {
        std::cerr << "[Detector] Invalid input dimensions" << std::endl;
        return;
    }

    img_scale = static_cast<float>(config.detection_resolution) / w;

    gpuResizedBuffer.create(h, w, CV_8UC3);
    gpuFloatBuffer.create(h, w, CV_32FC3);
    cvStream = cv::cuda::StreamAccessor::wrapStream(stream);

    cpuResizedBuffer.create(h, w, CV_8UC3);
    cpuFloatBuffer.create(h, w, CV_32FC3);
    inputHostBuffer.resize(static_cast<size_t>(c) * static_cast<size_t>(h) * static_cast<size_t>(w));

    for (const auto& n : inputNames)
        context->setTensorAddress(n.c_str(), inputBindings[n]);
    for (const auto& n : outputNames)
        context->setTensorAddress(n.c_str(), outputBindings[n]);

    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);

    preprocessStartEvent = nullptr;
    inferenceStartEvent = nullptr;
    inferenceCompleteEvent = nullptr;
    copyCompleteEvent = nullptr;

    cudaEventCreate(&preprocessStartEvent);
    cudaEventCreate(&inferenceStartEvent);
    cudaEventCreate(&inferenceCompleteEvent);
    cudaEventCreate(&copyCompleteEvent);

    useCudaGraph = config.use_cuda_graph;
    if (useCudaGraph)
    {
        captureCudaGraph();
    }

    if (config.verbose)
    {
        std::cout << "[Detector] Initialized. ModelStatic=" << std::boolalpha << isStatic
            << ", NetInput=" << h << "x" << w << " (scale=" << img_scale << ")" << std::endl;
    }
}

/*
 * 计算 TensorRT 维度的总元素数
 * 将 Dims 中各个维度的值相乘，若存在负维度（动态维度）则返回 0
 *
 * @param dims TensorRT 维度描述
 * @return 总元素数（size_t）
 */
size_t TrtDetector::getSizeByDim(const nvinfer1::Dims& dims)
{
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; ++i)
    {
        if (dims.d[i] < 0) return 0;
        size *= dims.d[i];
    }
    return size;
}

/*
 * 获取 TensorRT 数据类型的字节大小
 *
 * @param dtype TensorRT 数据类型枚举
 * @return 对应类型的字节数（kFLOAT=4, kHALF=2, kINT32=4, kINT8=1），
 *         未知类型返回 0
 */
size_t TrtDetector::getElementSize(nvinfer1::DataType dtype)
{
    switch (dtype)
    {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT8: return 1;
    default: return 0;
    }
}

/*
 * 加载 TensorRT 引擎
 * 支持两种输入格式：
 * - .engine：直接反序列化加载
 * - .onnx：调用 buildEngineFromOnnx 构建引擎，序列化后保存为 .engine 文件
 *
 * @param modelFile 模型文件路径（.engine 或 .onnx）
 */
void TrtDetector::loadEngine(const std::string& modelFile)
{
    std::string engineFilePath;
    std::filesystem::path modelPath(modelFile);
    std::string extension = modelPath.extension().string();

    if (extension == ".engine")
    {
        engineFilePath = modelFile;
    }
    else if (extension == ".onnx")
    {
        engineFilePath = modelPath.replace_extension(".engine").string();

        if (!fileExists(engineFilePath))
        {
            std::cout << "[Detector] Building engine from ONNX model" << std::endl;

            nvinfer1::ICudaEngine* builtEngine = buildEngineFromOnnx(modelFile, gLogger);
            if (builtEngine)
            {
                nvinfer1::IHostMemory* serializedEngine = builtEngine->serialize();

                if (serializedEngine)
                {
                    std::ofstream engineFile(engineFilePath, std::ios::binary);
                    if (engineFile)
                    {
                        engineFile.write(reinterpret_cast<const char*>(serializedEngine->data()), serializedEngine->size());
                        engineFile.close();

                        config.ai_model = std::filesystem::path(engineFilePath).filename().string();
                        config.saveConfig("config.ini");

                        std::cout << "[Detector] Engine saved to: " << engineFilePath << std::endl;
                    }
                    delete serializedEngine;
                }
                delete builtEngine;
            }
        }
    }
    else
    {
        std::cerr << "[Detector] Unsupported model format: " << extension << std::endl;
        return;
    }

    std::cout << "[Detector] Loading engine: " << engineFilePath << std::endl;
    engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));
}

/*
 * processFrame（CPU 版本）：提交 CPU 图像帧进行推理
 * 将输入帧保存到内部缓冲区，设置帧类型为 CPU，通知推理线程开始处理
 *
 * @param detection_frame  待检测的 BGR 图像
 * @param source_frame     原始分辨率源图像（用于数据收集），
 *                         若为空则使用 detection_frame
 * @param frameTiming      捕获与提交阶段时间契约
 */
void TrtDetector::processFrame(
    const cv::Mat& detection_frame,
    const cv::Mat& source_frame,
    FrameTiming frameTiming)
{
    if (detectionPaused)
    {
        detectionBuffer.clear();
        return;
    }

    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame = detection_frame;
    currentSourceFrame = source_frame.empty() ? detection_frame : source_frame;
    currentFrameGpu.release();
    currentFrameTiming = frameTiming;
    pendingFrameType = PendingFrameType::Cpu;
    frameReady = true;
    inferenceCV.notify_one();
}

/*
 * processFrameGpu（GPU 版本）：提交 GPU 图像帧进行推理
 * 直接使用 GPU 上的 GpuMat，避免 CPU-GPU 传输，适用于已在 GPU 上的帧
 *
 * @param frame           位于 GPU 上的待检测图像
 * @param frameTiming     捕获与提交阶段时间契约
 */
void TrtDetector::processFrameGpu(
    const cv::cuda::GpuMat& frame,
    FrameTiming frameTiming)
{
    if (detectionPaused)
    {
        detectionBuffer.clear();
        return;
    }

    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame.release();
    currentSourceFrame.release();
    currentFrameGpu = frame;
    currentFrameTiming = frameTiming;
    pendingFrameType = PendingFrameType::Gpu;
    frameReady = true;
    inferenceCV.notify_one();
}

/*
 * 同步检测接口：对单帧图像执行推理并返回检测结果
 * 流程：
 * 1. 记录预处理开始事件
 * 2. CPU 预处理（缩放、归一化、HWC 转 CHW）
 * 3. 记录推理开始事件
 * 4. 执行推理（优先使用 CUDA Graph，否则使用 enqueueV3）
 * 5. 异步将结果从 GPU 拷贝到锁页主机内存
 * 6. 记录拷贝完成事件并等待完成
 * 7. 后处理：若为 FP16 格式先转换为 FP32，然后调用 postProcessYolo
 * 8. 计算各阶段耗时
 *
 * @param frame 输入的 CPU 图像（BGR 格式）
 * @return 检测结果列表
 */
std::vector<Detection> TrtDetector::detect(const cv::Mat& frame)
{
    std::vector<Detection> latestDetections;
    if (!context || frame.empty())
        return latestDetections;

    cudaEventRecord(preprocessStartEvent, stream);
    preProcess(frame);
    cudaEventRecord(inferenceStartEvent, stream);

    const bool usedGraph = useCudaGraph && cudaGraphCaptured;
    if (usedGraph)
    {
        launchCudaGraph();
        cudaEventRecord(copyCompleteEvent, stream);
        cudaEventSynchronize(copyCompleteEvent);
    }
    else
    {
        if (!context->enqueueV3(stream))
        {
            std::cerr << "[Detector] enqueueV3 failed in detect()" << std::endl;
            return latestDetections;
        }
        cudaEventRecord(inferenceCompleteEvent, stream);

        for (const auto& name : outputNames)
        {
            const size_t size = outputSizes[name];

            auto itPinned = pinnedOutputBuffers.find(name);
            if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                continue;

            cudaError_t cpyErr = cudaMemcpyAsync(
                itPinned->second,
                outputBindings[name],
                size,
                cudaMemcpyDeviceToHost,
                stream);
            if (cpyErr != cudaSuccess)
                std::cerr << "[Detector] cudaMemcpyAsync(output) failed in detect(): "
                          << cudaGetErrorString(cpyErr) << std::endl;
        }

        cudaEventRecord(copyCompleteEvent, stream);
        cudaEventSynchronize(copyCompleteEvent);
    }

    auto tPostStart = std::chrono::steady_clock::now();

    latestDetections.clear();  // 多输出模型拼接所有 head
    for (const auto& name : outputNames)
    {
        const auto itPinned = pinnedOutputBuffers.find(name);
        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
            continue;

        nvinfer1::DataType dtype = outputTypes[name];
        if (dtype == nvinfer1::DataType::kHALF)
        {
            const size_t numElements = outputSizes[name] / sizeof(__half);
            const __half* halfPtr = reinterpret_cast<const __half*>(itPinned->second);

            auto& outputDataFloat = fp16OutputScratch[name];
            if (outputDataFloat.size() != numElements)
                outputDataFloat.resize(numElements);

            // TODO: FP16→FP32 转换移到 GPU（CUDA kernel 或 cublas）。
            // 当前 CPU 串行循环 ~700K 元素耗时 2-5ms/帧。
            for (size_t i = 0; i < numElements; ++i)
                outputDataFloat[i] = __half2float(halfPtr[i]);

            auto dets = postProcess(outputDataFloat.data(), name, &lastNmsTime);
            latestDetections.insert(latestDetections.end(), dets.begin(), dets.end());
        }
        else if (dtype == nvinfer1::DataType::kFLOAT)
        {
            const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
            auto dets = postProcess(floatPtr, name, &lastNmsTime);
            latestDetections.insert(latestDetections.end(), dets.begin(), dets.end());
        }
    }

    auto tPostEnd = std::chrono::steady_clock::now();

    float preprocessMs = 0.0f;
    float inferenceMs = 0.0f;
    float copyMs = 0.0f;

    cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
    cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
    cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

    lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
    lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
    lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
    lastPostprocessTime = tPostEnd - tPostStart;

    return latestDetections;
}

/*
 * 推理线程主循环
 * 持续从帧缓冲区获取待检测帧，执行预处理、推理和后处理
 *
 * 功能：
 * 1. 等待新的帧数据（条件变量唤醒）
 * 2. 检测模型变更（detector_model_changed），若变更则重新初始化
 * 3. 动态切换 CUDA Graph 的启用/禁用
 * 4. 对每帧执行 GPU 推理管线
 * 5. 后处理得到检测结果，更新 detectionBuffer
 * 6. 可选的训练数据收集（MaybeCollectDataSample）
 * 7. 记录各阶段耗时（预处理、推理、拷贝、后处理）
 *
 * 异常安全性：捕获所有 std::exception，防止线程崩溃
 */
void TrtDetector::inferenceThread()
{
    while (!shouldExit)
    {
        if (config.backend != "TRT")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (detector_model_changed.load())
        {
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                destroyCudaGraph();
                context.reset();
                engine.reset();

                freePinnedOutputs();

                for (auto& binding : inputBindings)
                    if (binding.second) cudaFree(binding.second);
                inputBindings.clear();
                for (auto& binding : outputBindings)
                    if (binding.second) cudaFree(binding.second);
                outputBindings.clear();

                currentFrame.release();
                currentSourceFrame.release();
                currentFrameGpu.release();
                frameReady = false;
                pendingFrameType = PendingFrameType::None;
            }
            initialize("models/" + config.ai_model);
            // 上下文重建后 CUDA Graph 失效, 下次推理前自动重新捕获
            cudaGraphCaptured = false;
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
        }

        if (useCudaGraph != config.use_cuda_graph)
        {
            useCudaGraph = config.use_cuda_graph;
            if (!useCudaGraph)
            {
                destroyCudaGraph();
            }
            else if (context)
            {
                captureCudaGraph();
            }
        }

        // dynamic-shape 模型: 分辨率变更时 CUDA Graph 需重新捕获
        if (cudaGraphCaptured && detection_resolution_changed.exchange(false))
        {
            if (!config.fixed_input_size)
            {
                destroyCudaGraph();
                captureCudaGraph();
            }
        }

        cv::Mat frame;
        cv::Mat sourceFrame;
        cv::cuda::GpuMat frameGpu;
        FrameTiming frameTiming;
        PendingFrameType frameType = PendingFrameType::None;
        bool hasNewFrame = false;

        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            if (!frameReady && !shouldExit)
                inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

            if (shouldExit) break;

            if (frameReady)
            {
                frameType = pendingFrameType;
                frameTiming = currentFrameTiming;
                frameTiming.inferenceStartTime = FrameTiming::Clock::now();
                if (frameType == PendingFrameType::Gpu)
                {
                    frameGpu = currentFrameGpu;
                    currentFrameGpu.release();
                    currentFrame.release();
                    currentSourceFrame.release();
                }
                else
                {
                    frame = std::move(currentFrame);
                    sourceFrame = std::move(currentSourceFrame);
                    currentFrameGpu.release();
                }
                pendingFrameType = PendingFrameType::None;
                frameReady = false;
                hasNewFrame = true;
            }
        }

        if (!context)
        {
            if (!error_logged)
            {
                std::cerr << "[Detector] Context not initialized" << std::endl;
                error_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        else
        {
            error_logged = false;
        }

        if (hasNewFrame)
        {
            const bool hasCpuFrame = (frameType == PendingFrameType::Cpu && !frame.empty());
            const bool hasGpuFrame = (frameType == PendingFrameType::Gpu && !frameGpu.empty());
            if (!hasCpuFrame && !hasGpuFrame)
                continue;

            try
            {
                cudaEventRecord(preprocessStartEvent, stream);
                if (hasGpuFrame)
                    preProcess(frameGpu);
                else
                    preProcess(frame);
                cudaEventRecord(inferenceStartEvent, stream);
                bool usedGraph = useCudaGraph && cudaGraphCaptured;
                if (usedGraph)
                {
                    launchCudaGraph();
                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventSynchronize(copyCompleteEvent);
                }
                else
                {
                    if (!context->enqueueV3(stream))
                    {
                        std::cerr << "[Detector] enqueueV3 failed in inference thread, skip frame" << std::endl;
                        cudaEventRecord(copyCompleteEvent, stream);
                        cudaEventSynchronize(copyCompleteEvent);
                        continue;
                    }
                    cudaEventRecord(inferenceCompleteEvent, stream);

                    for (const auto& name : outputNames)
                    {
                        const size_t size = outputSizes[name];

                        auto itPinned = pinnedOutputBuffers.find(name);
                        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                            continue;

                        cudaError_t cpyErr = cudaMemcpyAsync(
                            itPinned->second,
                            outputBindings[name],
                            size,
                            cudaMemcpyDeviceToHost,
                            stream
                        );
                        if (cpyErr != cudaSuccess)
                            std::cerr << "[Detector] cudaMemcpyAsync(output) failed in inference thread: "
                                      << cudaGetErrorString(cpyErr) << std::endl;
                    }

                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventSynchronize(copyCompleteEvent);
                }

                // 后处理（CPU 端）
                auto t_post_start = std::chrono::steady_clock::now();

                std::vector<Detection> latestDetections;
                for (const auto& name : outputNames)
                {
                    const auto itPinned = pinnedOutputBuffers.find(name);
                    if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                        continue;

                    nvinfer1::DataType dtype = outputTypes[name];

                    if (dtype == nvinfer1::DataType::kHALF)
                    {
                    // 在 CPU 上将 FP16 转换为 FP32
                        const size_t numElements = outputSizes[name] / sizeof(__half);
                        const __half* halfPtr = reinterpret_cast<const __half*>(itPinned->second);

                        auto& outputDataFloat = fp16OutputScratch[name];
                        if (outputDataFloat.size() != numElements)
                            outputDataFloat.resize(numElements);

                        // TODO: FP16→FP32 转换移到 GPU（CUDA kernel 或 cublas）。
            // 当前 CPU 串行循环 ~700K 元素耗时 2-5ms/帧。
            for (size_t i = 0; i < numElements; ++i)
                            outputDataFloat[i] = __half2float(halfPtr[i]);

                        auto dets = postProcess(outputDataFloat.data(), name, &lastNmsTime);
            latestDetections.insert(latestDetections.end(), dets.begin(), dets.end());
                    }
                    else if (dtype == nvinfer1::DataType::kFLOAT)
                    {
                        const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
                        auto dets = postProcess(floatPtr, name, &lastNmsTime);
            latestDetections.insert(latestDetections.end(), dets.begin(), dets.end());
                    }
                }

                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(latestDetections.size());
                classes.reserve(latestDetections.size());
                confidences.reserve(latestDetections.size());
                for (const auto& det : latestDetections)
                {
                    boxes.push_back(det.box);
                    classes.push_back(det.classId);
                    confidences.push_back(det.confidence);
                }

                detectionBuffer.set(boxes, classes, confidences, frameTiming);

                if (hasGpuFrame)
                {
                    cvm::MaybeCollectDataSample(
                        "",
                        config.ai_model.c_str(),
                        frameGpu,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }
                else
                {
                    const cv::Mat& frameForCollection = sourceFrame.empty() ? frame : sourceFrame;
                    cvm::MaybeCollectDataSample(
                        "",
                        config.ai_model.c_str(),
                        frameForCollection,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }

                auto t_post_end = std::chrono::steady_clock::now();

                float preprocessMs = 0.0f;
                float inferenceMs = 0.0f;
                float copyMs = 0.0f;

                cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
                cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
                cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

                lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
                lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
                lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
                lastPostprocessTime = t_post_end - t_post_start;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Detector] Error during inference: " << e.what() << std::endl;
            }
        }
    }
}

/*
 * preProcess（CPU 版本）：CPU 端图像预处理
 * 将输入的 BGR 图像转换为 TensorRT 模型所需的 NCHW 格式张量
 *
 * 步骤：
 * 1. 检查输入有效性（非空）
 * 2. 转换通道数（BGRA→BGR、GRAY→BGR）
 * 3. 缩放图像至模型输入尺寸
 * 4. 归一化至 [0, 1] 范围（除以 255.0）
 * 5. 调用 copyCpuTensorToDevice 将主机张量拷贝到设备内存
 *
 * @param frame 输入的 CPU 图像
 */
void TrtDetector::preProcess(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 1:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_GRAY2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        return;
    }

    cv::Mat resizedBgr;
    if (bgrFrame.cols != w || bgrFrame.rows != h)
    {
        cv::resize(bgrFrame, cpuResizedBuffer, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
        resizedBgr = cpuResizedBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }
    resizedBgr.convertTo(cpuFloatBuffer, CV_32FC3, 1.0f / 255.0f);

    copyCpuTensorToDevice(cpuFloatBuffer, w, h, inputBuffer);
}

/*
 * preProcess（GPU 版本）：GPU 端图像预处理
 * 直接在 GPU 上完成颜色转换、缩放和归一化，避免 CPU-GPU 传输
 *
 * 步骤：
 * 1. 检查输入有效性
 * 2. GPU 上转换通道数（BGRA→BGR、GRAY→BGR）
 * 3. GPU 上缩放至模型输入尺寸
 * 4. GPU 上归一化至 [0, 1] 范围
 * 5. 使用自定义 CUDA 内核 launch_hwc_to_chw_norm
 *    将 HWC 格式转换为 CHW 格式并拷贝到输入缓冲区
 *
 * @param frame 位于 GPU 上的输入图像
 */
void TrtDetector::preProcess(const cv::cuda::GpuMat& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    cv::cuda::GpuMat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cuda::cvtColor(frame, gpuFrameBuffer, cv::COLOR_BGRA2BGR, 0, cvStream);
        bgrFrame = gpuFrameBuffer;
        break;
    case 1:
        cv::cuda::cvtColor(frame, gpuFrameBuffer, cv::COLOR_GRAY2BGR, 0, cvStream);
        bgrFrame = gpuFrameBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        return;
    }

    cv::cuda::GpuMat resizedBgr;
    if (bgrFrame.cols != w || bgrFrame.rows != h)
    {
        cv::cuda::resize(bgrFrame, gpuResizedBuffer, cv::Size(w, h), 0, 0, cv::INTER_LINEAR, cvStream);
        resizedBgr = gpuResizedBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }
    resizedBgr.convertTo(gpuFloatBuffer, CV_32FC3, 1.0 / 255.0, 0.0, cvStream);

    launch_hwc_to_chw_norm(
        gpuFloatBuffer.ptr<float>(),
        gpuFloatBuffer.step,
        reinterpret_cast<float*>(inputBuffer),
        w,
        h,
        stream);

    if (config.verbose)
    {
        const cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] preprocess kernel launch error: " << cudaGetErrorString(err) << std::endl;
        }
    }
}

/*
 * 将 CPU 端的浮点张量拷贝到 GPU 设备缓冲区
 * 将 BGR 格式的浮点图像（HWC 布局）转换为 CHW 布局并异步上传到 GPU
 *
 * 转换步骤：
 * 1. 将 BGR 图像按通道分离，同时交换 B 和 R 通道（BGR → RGB）
 * 2. 按 R/G/B 顺序写入连续内存（CHW 布局）
 * 3. 通过 cudaMemcpyAsync 异步上传到设备
 *
 * @param bgrFloatFrame CPU 端 BGR 浮点图像（CV_32FC3）
 * @param width         模型输入宽度
 * @param height        模型输入高度
 * @param inputBuffer   目标 GPU 缓冲区指针
 */
void TrtDetector::copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer)
{
    if (bgrFloatFrame.empty() || bgrFloatFrame.channels() != 3 || !inputBuffer)
        return;

    const size_t channelSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t tensorSize = channelSize * 3;
    if (inputHostBuffer.size() != tensorSize)
        inputHostBuffer.resize(tensorSize);

    float* dst = inputHostBuffer.data();
    cv::Mat bgrToRgbPlanes[3] = {
        cv::Mat(height, width, CV_32F, dst + channelSize * 2),
        cv::Mat(height, width, CV_32F, dst + channelSize),
        cv::Mat(height, width, CV_32F, dst)
    };
    cv::split(bgrFloatFrame, bgrToRgbPlanes);

    cudaError_t err = cudaMemcpyAsync(
        inputBuffer,
        inputHostBuffer.data(),
        tensorSize * sizeof(float),
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] preprocess input copy failed: " << cudaGetErrorString(err) << std::endl;
    }
}

/*
 * 后处理：对模型原始输出进行 YOLO 后处理并过滤
 * 根据输出张量的名称查找对应的形状信息，依次执行：
 * 1. YOLO 后处理（postProcessYolo）
 * 2. 深度掩码过滤（filterDetectionsByDepthMask）
 * 3. 圆形 FOV 过滤（filterDetectionsByCircleFov）
 *
 * @param output     模型输出的浮点数据指针
 * @param outputName 输出张量名称
 * @param nmsTime    输出参数，记录 NMS（非极大值抑制）的耗时
 * @return 过滤后的检测结果列表
 */
std::vector<Detection> TrtDetector::postProcess(const float* output, const std::string& outputName, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (!output)
        return {};

    const auto shapeIt = outputShapes.find(outputName);
    if (shapeIt == outputShapes.end())
        return {};

    std::vector<Detection> detections;
    
    detections = postProcessYolo(
        output,
        shapeIt->second,
        numClasses,
        config.confidence_threshold,
        config.nms_threshold,
        std::max(1, config.max_detections),
        nmsTime,
        img_scale
    );
    filterDetectionsByDepthMask(detections);
    filterDetectionsByCircleFov(detections);
    return detections;
}
#endif
