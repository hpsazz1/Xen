// dml_detector.cpp - DirectML 推理检测器实现
// 使用 ONNX Runtime 的 DirectML 执行提供程序在 GPU 上运行 AI 模型推理。
// 支持两种输出格式：
//   1. SunPoint raw（heat/box/offset 三输出）- 基于 CenterNet 的关键点检测架构
//   2. 标准 YOLO 格式输出 - 传统的锚框检测架构
// 包含完整的预处理（图像缩放、归一化、BGR 转 RGB）、推理和后处理（NMS）流程。

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <dml_provider_factory.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>
#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <utility>

#include "dml_detector.h"
#include "Xen.h"
#include "scr/data_collector.h"
#include "postProcess.h"
#include "capture.h"
#include "capture/circle_fov.h"
#include "other_tools.h"
#ifdef USE_CUDA
#include "depth/depth_mask.h"
#endif
#include "detection_filters.h"

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> detectionPaused;

namespace
{
    /**
     * utf8ToWide - 将 UTF-8 字符串正确转换为 UTF-16 宽字符串。
     * 不能使用逐字节扩展（wstring(begin, end)），那样会损坏非 ASCII 路径。
     * @param utf8 输入的 UTF-8 字符串
     * @return 转换后的 std::wstring
     */
    std::wstring utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return std::wstring();

        int required = MultiByteToWideChar(
            CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (required <= 0)
            return std::wstring();

        std::wstring wide(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], required);
        return wide;
    }

    /**
     * tryInt64ToInt - 安全地将 int64_t 转换为 int，检查是否超出 int 范围
     * @param value  输入的 int64_t 值
     * @param out    [输出] 转换后的 int 值
     * @return 转换成功返回 true，溢出返回 false
     */
    bool tryInt64ToInt(int64_t value, int* out)
    {
        if (!out)
        {
            return false;
        }

        if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
            value > static_cast<int64_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        *out = static_cast<int>(value);
        return true;
    }

    /**
     * sigmoidFloat - 计算 Sigmoid 激活函数值
     * 使用分段计算以提高数值稳定性：
     *   当 x >= 0 时: 1 / (1 + exp(-x))
     *   当 x < 0 时:  exp(x) / (1 + exp(x))
     * @param x 输入值
     * @return Sigmoid(x)，范围 (0, 1)
     */
    float sigmoidFloat(float x)
    {
        if (x >= 0.0f)
        {
            const float z = std::exp(-x);
            return 1.0f / (1.0f + z);
        }
        const float z = std::exp(x);
        return z / (1.0f + z);
    }

    /**
     * softplusFloat - 计算 Softplus 激活函数值: ln(1 + exp(x))
     * 使用分段实现以避免大数值时的指数溢出：
     *   x > 20:  直接返回 x（线性近似）
     *   x < -20: 返回 exp(x)（指数近似）
     *   其他:    返回 log1p(exp(x))（标准计算）
     * @param x 输入值
     * @return softplus(x)
     */
    float softplusFloat(float x)
    {
        if (x > 20.0f) return x;
        if (x < -20.0f) return std::exp(x);
        return std::log1p(std::exp(x));
    }

    /**
     * isShape4 - 检查张量形状是否为有效的 4D 形状 [N, C, H, W]
     * @param shape 张量形状向量
     * @return 如果是有效的 4D 形状（所有维度 > 0）返回 true
     */
    bool isShape4(const std::vector<int64_t>& shape)
    {
        return shape.size() == 4 && shape[0] > 0 && shape[1] > 0 && shape[2] > 0 && shape[3] > 0;
    }

    /**
     * nchwOffset - 计算 NCHW 格式张量的线性索引偏移
     * @param batch    批次索引
     * @param channel  通道索引
     * @param y        高度方向索引
     * @param x        宽度方向索引
     * @param channels 通道总数
     * @param height   高度
     * @param width    宽度
     * @return 线性内存中的偏移量
     */
    size_t nchwOffset(int batch, int channel, int y, int x, int channels, int height, int width)
    {
        return (((static_cast<size_t>(batch) * static_cast<size_t>(channels) + static_cast<size_t>(channel))
            * static_cast<size_t>(height) + static_cast<size_t>(y)) * static_cast<size_t>(width))
            + static_cast<size_t>(x);
    }

    /**
     * findOutputIndex - 在输出名称列表中查找指定名称的索引
     * @param names  输出名称列表
     * @param wanted 要查找的名称
     * @return 找到返回索引，未找到返回 -1
     */
    int findOutputIndex(const std::vector<std::string>& names, const char* wanted)
    {
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
        {
            if (names[i] == wanted)
                return i;
        }
        return -1;
    }

    /**
     * getDmlOrtLogLevel - 根据 verbose 配置获取 ONNX Runtime 日志级别
     * @return verbose 开启返回 WARNING 级别，否则返回 ERROR 级别
     */
    OrtLoggingLevel getDmlOrtLogLevel()
    {
        return config.verbose ? ORT_LOGGING_LEVEL_WARNING : ORT_LOGGING_LEVEL_ERROR;
    }

    /**
     * decodeSunPointRaw - 解码 SunPoint raw 输出格式的检测结果
     *
     * SunPoint 是一种基于 CenterNet 架构的关键点检测方法。模型输出三个张量：
     *   - heat: 热力图 (classes x gridH x gridW)，每个网格点对应一个类别的存在概率
     *   - box:  边框预测 (4 x gridH x gridW)，四个值分别表示左/上/右/下边界距离
     *   - offset: 中心点偏移 (2 x gridH x gridW)，x 和 y 方向的亚像素偏移
     *
     * 解码步骤:
     * 1. 在热力图上执行 3x3 局部极大值抑制（NMS），只保留峰值点
     * 2. 对每个峰值点，计算中心坐标（网格坐标 + 偏移）* 步长
     * 3. 使用 softplus 解码边界框的四个边距
     * 4. 对检测结果按置信度排序并执行全局 NMS
     *
     * @param heat          热力图数据指针
     * @param heatShape     热力图形状 [batch, classes, gridH, gridW]
     * @param box           边框预测数据指针
     * @param boxShape      边框形状 [batch, 4, gridH, gridW]
     * @param offset        偏移预测数据指针
     * @param offsetShape   偏移形状 [batch, 2, gridH, gridW]
     * @param batchIndex    当前处理的批次索引
     * @param targetW       模型输入宽度（缩放后）
     * @param targetH       模型输入高度（缩放后）
     * @param confThreshold 置信度阈值
     * @param nmsThreshold  NMS 的 IoU 阈值
     * @param maxDetections 最大检测数上限
     * @param nmsTime       [输出] NMS 耗时统计
     * @return 解码后的检测结果列表
     */
    std::vector<Detection> decodeSunPointRaw(
        const float* heat,
        const std::vector<int64_t>& heatShape,
        const float* box,
        const std::vector<int64_t>& boxShape,
        const float* offset,
        const std::vector<int64_t>& offsetShape,
        int batchIndex,
        int targetW,
        int targetH,
        float confThreshold,
        float nmsThreshold,
        int maxDetections,
        std::chrono::duration<double, std::milli>* nmsTime)
    {
        std::vector<Detection> detections;
        if (!heat || !box || !offset || !isShape4(heatShape) || !isShape4(boxShape) || !isShape4(offsetShape))
            return detections;

        const int batch = static_cast<int>(heatShape[0]);
        const int classes = static_cast<int>(heatShape[1]);
        const int gridH = static_cast<int>(heatShape[2]);
        const int gridW = static_cast<int>(heatShape[3]);
        if (batchIndex < 0 || batchIndex >= batch || classes <= 0 || gridH <= 0 || gridW <= 0)
            return detections;
        if (boxShape[0] != heatShape[0] || boxShape[1] != 4 || boxShape[2] != heatShape[2] || boxShape[3] != heatShape[3])
            return detections;
        if (offsetShape[0] != heatShape[0] || offsetShape[1] != 2 || offsetShape[2] != heatShape[2] || offsetShape[3] != heatShape[3])
            return detections;

        // 计算网格步长：每个网格点在输出图像上对应的像素跨度
        const float strideX = static_cast<float>(targetW) / static_cast<float>(gridW);
        const float strideY = static_cast<float>(targetH) / static_cast<float>(gridH);
        detections.reserve(static_cast<size_t>(std::max(maxDetections, 16)));

        // 遍历每个类别和网格点，提取热力图中的峰值
        for (int c = 0; c < classes; ++c)
        {
            for (int y = 0; y < gridH; ++y)
            {
                for (int x = 0; x < gridW; ++x)
                {
                    const size_t heatIdx = nchwOffset(batchIndex, c, y, x, classes, gridH, gridW);
                    const float heatLogit = heat[heatIdx];
                    const float score = sigmoidFloat(heatLogit);
                    if (score <= confThreshold)
                        continue;

                    // 局部峰值检测：检查 3x3 邻域内是否为最大值
                    bool isPeak = true;
                    for (int yy = std::max(0, y - 1); yy <= std::min(gridH - 1, y + 1) && isPeak; ++yy)
                    {
                        for (int xx = std::max(0, x - 1); xx <= std::min(gridW - 1, x + 1); ++xx)
                        {
                            if (yy == y && xx == x)
                                continue;
                            const size_t neighborIdx = nchwOffset(batchIndex, c, yy, xx, classes, gridH, gridW);
                            if (heat[neighborIdx] > heatLogit)
                            {
                                isPeak = false;
                                break;
                            }
                        }
                    }
                    if (!isPeak)
                        continue;

                    // 解码中心点位置：网格坐标 + sigmoid 偏移量，再乘以步长
                    const float offX = sigmoidFloat(offset[nchwOffset(batchIndex, 0, y, x, 2, gridH, gridW)]);
                    const float offY = sigmoidFloat(offset[nchwOffset(batchIndex, 1, y, x, 2, gridH, gridW)]);
                    const float centerX = (static_cast<float>(x) + offX) * strideX;
                    const float centerY = (static_cast<float>(y) + offY) * strideY;

                    // 解码边界框：使用 softplus 确保边框距离为正数
                    const float left = softplusFloat(box[nchwOffset(batchIndex, 0, y, x, 4, gridH, gridW)]) * strideX;
                    const float top = softplusFloat(box[nchwOffset(batchIndex, 1, y, x, 4, gridH, gridW)]) * strideY;
                    const float right = softplusFloat(box[nchwOffset(batchIndex, 2, y, x, 4, gridH, gridW)]) * strideX;
                    const float bottom = softplusFloat(box[nchwOffset(batchIndex, 3, y, x, 4, gridH, gridW)]) * strideY;

                    // 由中心点和四边距离计算左上角和右下角坐标，并限制在图像范围内
                    const float x1 = std::clamp(centerX - left, 0.0f, static_cast<float>(targetW));
                    const float y1 = std::clamp(centerY - top, 0.0f, static_cast<float>(targetH));
                    const float x2 = std::clamp(centerX + right, 0.0f, static_cast<float>(targetW));
                    const float y2 = std::clamp(centerY + bottom, 0.0f, static_cast<float>(targetH));
                    if (x2 <= x1 || y2 <= y1)
                        continue;

                    cv::Rect rect;
                    rect.x = static_cast<int>(x1);
                    rect.y = static_cast<int>(y1);
                    rect.width = std::max(1, static_cast<int>(x2 - x1));
                    rect.height = std::max(1, static_cast<int>(y2 - y1));
                    detections.push_back(Detection{ rect, score, c });
                }
            }
        }

        // 如果检测数量超过最大限制，保留置信度最高的 top-N 个
        if (maxDetections > 0 && detections.size() > static_cast<size_t>(maxDetections))
        {
            const auto kth = detections.begin() + maxDetections;
            std::nth_element(
                detections.begin(),
                kth,
                detections.end(),
                [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
            detections.resize(static_cast<size_t>(maxDetections));
        }

        // 按置信度降序排序后执行 NMS
        std::sort(
            detections.begin(),
            detections.end(),
            [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
        NMS(detections, nmsThreshold, nmsTime);
        return detections;
    }

// filterDetectionsByCircleFov 已移至 detection_filters.cpp（共享实现）
/**
 * GetDMLDeviceName - 通过 DXGI API 获取 DirectML 设备的名称
 * @param deviceId 设备索引
 * @return 设备名称字符串，失败返回错误描述
 */
std::string GetDMLDeviceName(int deviceId)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))
        return "Unknown";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(dxgiFactory->EnumAdapters1(deviceId, &adapter)))
        return "Invalid device ID";

    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(adapter->GetDesc1(&desc)))
        return "Failed to get description";

    std::wstring wname(desc.Description);
    return WideToUtf8(wname);
}

} // namespace

/**
 * DirectMLDetector 构造函数
 * 创建 ONNX Runtime 环境并尝试加载指定路径的模型。
 * @param model_path ONNX 模型文件路径
 */
DirectMLDetector::DirectMLDetector(const std::string& model_path)
    :
    env(getDmlOrtLogLevel(), "DML_Detector"),
    memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    if (!initializeModel(model_path))
    {
        std::cerr << "[DML] Detector started without an active model."
                  << "请选择另一个ONNX模型或修复模型/提供程序兼容性。" << std::endl;
    }
}

/**
 * DirectMLDetector 析构函数
 * 通知推理线程退出并唤醒条件变量。
 */
DirectMLDetector::~DirectMLDetector()
{
    requestStop();
}

void DirectMLDetector::requestStop() noexcept
{
    shouldExit.store(true);
    inferenceCV.notify_all();
}

/**
 * isReady - 检查模型是否已成功加载并可进行推理
 * @return 模型就绪返回 true
 */
bool DirectMLDetector::isReady() const
{
    return model_ready.load();
}

DirectMLDetector::TimingSnapshot DirectMLDetector::getTimingSnapshot() const
{
    std::lock_guard<std::mutex> lock(timingMutex);
    return timingSnapshot;
}

void DirectMLDetector::publishTimingSnapshot(
    std::chrono::duration<double, std::milli> preprocess,
    std::chrono::duration<double, std::milli> tensorSetup,
    std::chrono::duration<double, std::milli> inference,
    std::chrono::duration<double, std::milli> copy,
    std::chrono::duration<double, std::milli> postprocess,
    std::chrono::duration<double, std::milli> nms,
    std::chrono::duration<double, std::milli> total)
{
    TimingSnapshot snapshot;
    snapshot.preprocessMs = preprocess.count();
    snapshot.tensorSetupMs = tensorSetup.count();
    snapshot.inferenceMs = inference.count();
    snapshot.copyMs = copy.count();
    snapshot.postprocessMs = postprocess.count();
    snapshot.nmsMs = nms.count();
    snapshot.totalMs = total.count();

    std::lock_guard<std::mutex> lock(timingMutex);
    snapshot.modelPath = activeModelPath;
    snapshot.inputWidth = activeInputWidth;
    snapshot.inputHeight = activeInputHeight;
    timingSnapshot = snapshot;
}

/**
 * createSessionOptions - 创建 ONNX Runtime 会话配置选项
 * @param useDirectML             是否使用 DirectML 执行提供程序
 * @param graphOptimizationLevel   图优化级别
 * @return 配置好的 SessionOptions 对象
 */
Ort::SessionOptions DirectMLDetector::createSessionOptions(
    bool useDirectML,
    GraphOptimizationLevel graphOptimizationLevel)
{
    Ort::SessionOptions options;
    options.SetLogId("DML_Detector");
    options.SetLogSeverityLevel(static_cast<int>(getDmlOrtLogLevel()));
    options.SetGraphOptimizationLevel(graphOptimizationLevel);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.DisableMemPattern();
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);

    if (useDirectML)
    {
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, config.dml_device_id));
    }

    return options;
}

/**
 * tryInitializeModel - 尝试用给定的配置初始化模型会话
 *
 * 创建 ONNX Runtime 会话，读取模型的输入/输出张量信息，
 * 自动检测是否为 SunPoint raw 输出格式，设置固定/动态输入尺寸。
 *
 * @param model_path              模型文件路径
 * @param useDirectML             是否使用 DirectML
 * @param graphOptimizationLevel  图优化级别
 * @param providerLabel           提供程序标签（用于日志）
 * @param error                   [输出] 错误信息
 * @return 初始化成功返回 true
 */
bool DirectMLDetector::tryInitializeModel(
    const std::string& model_path,
    bool useDirectML,
    GraphOptimizationLevel graphOptimizationLevel,
    const char* providerLabel,
    std::string* error)
{
    try
    {
        Ort::SessionOptions options = createSessionOptions(useDirectML, graphOptimizationLevel);
        std::wstring model_path_wide = utf8ToWide(model_path);
        Ort::Session newSession(env, model_path_wide.c_str(), options);

        // 读取输入张量名称和形状
        std::string newInputName = newSession.GetInputNameAllocated(0, allocator).get();

        std::vector<std::string> newOutputNames;
        const size_t outputCount = newSession.GetOutputCount();
        newOutputNames.reserve(outputCount);
        for (size_t i = 0; i < outputCount; ++i)
        {
            newOutputNames.emplace_back(newSession.GetOutputNameAllocated(i, allocator).get());
        }

        // 检测是否为 SunPoint raw 输出（包含 heat/box/offset 三个输出）
        const int newHeatOutputIndex = findOutputIndex(newOutputNames, "heat");
        const int newBoxOutputIndex = findOutputIndex(newOutputNames, "box");
        const int newOffsetOutputIndex = findOutputIndex(newOutputNames, "offset");
        const bool newSunpointRawOutput =
            newHeatOutputIndex >= 0 &&
            newBoxOutputIndex >= 0 &&
            newOffsetOutputIndex >= 0;

        // 读取输入张量的形状信息
        Ort::TypeInfo input_type_info = newSession.GetInputTypeInfo(0);
        auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> newInputShape = input_tensor_info.GetShape();

        int newModelInputH = -1;
        int newModelInputW = -1;
        if (newInputShape.size() > 2)
        {
            int converted = 0;
            if (tryInt64ToInt(newInputShape[2], &converted))
                newModelInputH = converted;
        }
        if (newInputShape.size() > 3)
        {
            int converted = 0;
            if (tryInt64ToInt(newInputShape[3], &converted))
                newModelInputW = converted;
        }

        // 判断是否为静态形状（所有维度都是正数，无动态维度）
        bool isStatic = true;
        for (auto d : newInputShape) if (d <= 0) isStatic = false;

        // 将会话和元数据保存到成员变量
        session = std::move(newSession);
        input_name = std::move(newInputName);
        output_names = std::move(newOutputNames);
        output_name = output_names.empty() ? std::string() : output_names.front();
        output_name_ptrs.clear();
        output_name_ptrs.reserve(output_names.size());
        for (const auto& name : output_names)
            output_name_ptrs.push_back(name.c_str());

        input_shape = std::move(newInputShape);
        model_input_h = newModelInputH;
        model_input_w = newModelInputW;
        heat_output_index = newHeatOutputIndex;
        box_output_index = newBoxOutputIndex;
        offset_output_index = newOffsetOutputIndex;
        sunpoint_raw_output = newSunpointRawOutput;
        using_directml_provider = useDirectML;
        model_ready.store(true);
        {
            std::lock_guard<std::mutex> lock(timingMutex);
            activeModelPath = model_path;
            activeInputWidth = model_input_w > 0 ? model_input_w : config.detection_resolution;
            activeInputHeight = model_input_h > 0 ? model_input_h : config.detection_resolution;
            timingSnapshot = TimingSnapshot{};
            timingSnapshot.modelPath = activeModelPath;
            timingSnapshot.inputWidth = activeInputWidth;
            timingSnapshot.inputHeight = activeInputHeight;
        }

        // 自动设置 fixed_input_size，并通知其他模块重新加载
        if (isStatic != config.fixed_input_size)
        {
            config.fixed_input_size = isStatic;
            detector_model_changed.store(true);
            std::cout << "[DML] Automatically set fixed_input_size = "
                      << (isStatic ? "true" : "false") << std::endl;
        }

        std::cout << "[DML] 已使用 " << providerLabel << " 提供程序初始化模型："
                  << model_path << std::endl;

        if (useDirectML && config.verbose)
            std::cout << "[DirectML] Using adapter: " << GetDMLDeviceName(config.dml_device_id) << std::endl;

        if (config.verbose)
        {
            std::cout << "[DML] Outputs:";
            for (const auto& name : output_names)
                std::cout << " " << name;
            std::cout << (sunpoint_raw_output ? " (SunPoint raw)" : "") << std::endl;
        }

        return true;
    }
    catch (const Ort::Exception& e)
    {
        if (error) *error = e.what();
    }
    catch (const std::exception& e)
    {
        if (error) *error = e.what();
    }
    catch (...)
    {
        if (error) *error = "unknown exception";
    }

    return false;
}

/**
 * initializeModel - 初始化 AI 模型，尝试多个执行提供程序和优化级别
 *
 * 尝试顺序：
 * 1. DirectML 提供程序 + 全部图优化
 * 2. DirectML 提供程序 + 基本图优化（兼容模式）
 * 3. CPU 提供程序 + 全部图优化（回退方案）
 *
 * 如果之前已有工作会话且所有尝试都失败，保留旧会话。
 * @param model_path 模型文件路径
 * @return 初始化成功返回 true
 */
bool DirectMLDetector::initializeModel(const std::string& model_path)
{
    env.UpdateEnvWithCustomLogLevel(getDmlOrtLogLevel());
    const bool hadReadySession = model_ready.load();

    // 尝试 DirectML 提供程序 + 全部图优化
    std::string directMlError;
    if (tryInitializeModel(
        model_path,
        true,
        GraphOptimizationLevel::ORT_ENABLE_ALL,
        "DirectML",
        &directMlError))
    {
        return true;
    }

    std::cerr << "[DML] DirectML initialization failed for " << model_path
              << ": " << directMlError << std::endl;

    // 尝试 DirectML 提供程序 + 基本图优化（兼容模式）
    std::string directMlCompatError;
    std::cerr << "[DML] Retrying DirectML with compatibility graph optimizations." << std::endl;
    if (tryInitializeModel(
        model_path,
        true,
        GraphOptimizationLevel::ORT_ENABLE_BASIC,
        "DirectML compatibility",
        &directMlCompatError))
    {
        return true;
    }

    std::cerr << "[DML] DirectML compatibility initialization failed for " << model_path
              << ": " << directMlCompatError << std::endl;

    // 最后的回退方案：使用 CPU 提供程序
    std::string cpuError;
    std::cerr << "[DML] Falling back to ONNX Runtime CPU provider. Detection may be slower." << std::endl;
    if (tryInitializeModel(
        model_path,
        false,
        GraphOptimizationLevel::ORT_ENABLE_ALL,
        "CPU",
        &cpuError))
    {
        return true;
    }

    std::cerr << "[DML] CPU fallback initialization failed for " << model_path
              << ": " << cpuError << std::endl;

    // 所有尝试失败，保留旧会话（如果有）
    if (hadReadySession)
    {
        std::cerr << "[DML] Keeping the previous working detector session." << std::endl;
    }
    else
    {
        model_ready.store(false);
        using_directml_provider = false;
    }

    return false;
}

/**
 * preprocessFrameToTensor - 将输入图像帧预处理为 NCHW 张量
 *
 * 处理步骤：
 * 1. 将图像缩放到目标尺寸（若尺寸不匹配）
 * 2. 将像素值从 [0, 255] 归一化到 [0.0, 1.0]
 * 3. 将 BGR 通道顺序转换为 RGB
 * 4. 输出为 NCHW 布局的 float 数组（已分离通道）
 *
 * 支持输入: 1 通道灰度图、3 通道 BGR、4 通道 BGRA
 *
 * @param frame    输入图像帧
 * @param dst      目标浮点数组指针（NCHW 格式，需预先分配）
 * @param target_w 目标宽度
 * @param target_h 目标高度
 */
void DirectMLDetector::preprocessFrameToTensor(const cv::Mat& frame, float* dst, int target_w, int target_h)
{
    if (!dst || target_w <= 0 || target_h <= 0)
        return;

    const size_t channelSize = static_cast<size_t>(target_w) * static_cast<size_t>(target_h);
    cv::Mat rgbPlanes[3] = {
        cv::Mat(target_h, target_w, CV_32F, dst),
        cv::Mat(target_h, target_w, CV_32F, dst + channelSize),
        cv::Mat(target_h, target_w, CV_32F, dst + channelSize * 2)
    };

    auto clearTensor = [&]()
    {
        rgbPlanes[0].setTo(0.0f);
        rgbPlanes[1].setTo(0.0f);
        rgbPlanes[2].setTo(0.0f);
    };

    if (frame.empty())
    {
        clearTensor();
        return;
    }

    // 处理单通道灰度图
    if (frame.channels() == 1)
    {
        cv::Mat grayResized;
        if (frame.cols != target_w || frame.rows != target_h)
        {
            cv::resize(frame, preprocessGrayResizeBuffer, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
            grayResized = preprocessGrayResizeBuffer;
        }
        else
        {
            grayResized = frame;
        }

        grayResized.convertTo(preprocessGrayFloatBuffer, CV_32F, 1.0f / 255.0f);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[0]);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[1]);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[2]);
        return;
    }

    // 处理彩色图像：先转为 3 通道 BGR
    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, preprocessBgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = preprocessBgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        clearTensor();
        return;
    }

    // 缩放到目标尺寸
    cv::Mat resizedBgr;
    if (bgrFrame.cols != target_w || bgrFrame.rows != target_h)
    {
        cv::resize(bgrFrame, preprocessResizeBuffer, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
        resizedBgr = preprocessResizeBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }

    // 归一化到 [0, 1] 并转为 float
    resizedBgr.convertTo(preprocessFloatBuffer, CV_32FC3, 1.0f / 255.0f);

    // 拆分 BGR 通道并重新排列为 RGB (planes[2]=R, planes[1]=G, planes[0]=B)
    cv::Mat bgrToRgbPlanes[3] = {
        rgbPlanes[2],
        rgbPlanes[1],
        rgbPlanes[0]
    };
    cv::split(preprocessFloatBuffer, bgrToRgbPlanes);
}

/**
 * detect - 对单帧图像进行目标检测
 * @param input_frame 输入图像帧
 * @return 检测结果列表
 */
std::vector<Detection> DirectMLDetector::detect(const cv::Mat& input_frame)
{
    std::vector<cv::Mat> batch = { input_frame };
    auto batchResult = detectBatch(batch);
    if (!batchResult.empty())
        return batchResult[0];
    else
        return {};
}

/**
 * detectBatch - 对一批图像帧进行目标检测
 *
 * 完整的检测流程：
 * 1. 预处理：缩放、归一化、BGR 转 RGB、NCHW 布局
 * 2. 模型推理：使用 DirectML 或 CPU 执行 ONNX 模型
 * 3. 后处理：根据输出格式（SunPoint raw 或 YOLO）解码检测结果
 * 4. 如果模型使用固定输入尺寸且与配置的检测分辨率不同，缩放检测框
 * 5. 记录各阶段耗时统计
 *
 * @param frames 输入图像帧列表（批量）
 * @return 批量检测结果，每帧对应一个 Detection 列表
 */
std::vector<std::vector<Detection>> DirectMLDetector::detectBatch(const std::vector<cv::Mat>& frames)
{
    std::vector<std::vector<Detection>> empty;
    if (!isReady()) return empty;
    if (frames.empty()) return empty;
    const size_t batch_size = frames.size();

    const int model_h = model_input_h;
    const int model_w = model_input_w;
    const bool useFixed = config.fixed_input_size && model_h > 0 && model_w > 0;

    // 确定推理时的输入尺寸：固定尺寸模型使用模型原始尺寸，否则使用配置的检测分辨率
    const int target_h = useFixed ? model_h : config.detection_resolution;
    const int target_w = useFixed ? model_w : config.detection_resolution;

    auto t0 = std::chrono::steady_clock::now();
    const size_t frameTensorSize =
        static_cast<size_t>(3) * static_cast<size_t>(target_h) * static_cast<size_t>(target_w);
    input_tensor_values.resize(batch_size * frameTensorSize);

    // 预处理所有帧
    for (size_t b = 0; b < batch_size; ++b)
    {
        float* dst = input_tensor_values.data() + b * frameTensorSize;
        preprocessFrameToTensor(frames[b], dst, target_w, target_h);
    }
    auto t1 = std::chrono::steady_clock::now();

    // 创建 ONNX Runtime 输入张量
    std::vector<int64_t> ort_input_shape{
        static_cast<int64_t>(batch_size),
        3,
        static_cast<int64_t>(target_h),
        static_cast<int64_t>(target_w)
    };
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input_tensor_values.data(), input_tensor_values.size(),
        ort_input_shape.data(), ort_input_shape.size());

    const char* input_names[] = { input_name.c_str() };
    if (output_name_ptrs.empty())
    {
        std::cerr << "[DirectMLDetector] No output tensors are defined." << std::endl;
        return empty;
    }

    // 执行模型推理
    auto t2 = std::chrono::steady_clock::now();
    auto output_tensors = session.Run(Ort::RunOptions{ nullptr },
        input_names, &input_tensor, 1,
        output_name_ptrs.data(), output_name_ptrs.size());
    auto t3 = std::chrono::steady_clock::now();

    std::vector<std::vector<Detection>> batchDetections(batch_size);
    float conf_thr = config.confidence_threshold;
    float nms_thr = config.nms_threshold;
    int max_detections = std::max(1, config.max_detections);

    auto t4 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> nmsTimeTmp{ 0 };

    // 后处理：根据输出格式选择解码方式
    if (sunpoint_raw_output)
    {
        // SunPoint raw 格式（heat/box/offset 三输出）
        const int heatIdx = heat_output_index;
        const int boxIdx = box_output_index;
        const int offsetIdx = offset_output_index;
        if (heatIdx < 0 || boxIdx < 0 || offsetIdx < 0)
        {
            std::cerr << "[DirectMLDetector] SunPoint raw outputs are missing." << std::endl;
            return empty;
        }

        float* heatData = output_tensors[heatIdx].GetTensorMutableData<float>();
        float* boxData = output_tensors[boxIdx].GetTensorMutableData<float>();
        float* offsetData = output_tensors[offsetIdx].GetTensorMutableData<float>();
        std::vector<int64_t> heatShape = output_tensors[heatIdx].GetTensorTypeAndShapeInfo().GetShape();
        std::vector<int64_t> boxShape = output_tensors[boxIdx].GetTensorTypeAndShapeInfo().GetShape();
        std::vector<int64_t> offsetShape = output_tensors[offsetIdx].GetTensorTypeAndShapeInfo().GetShape();

        for (size_t b = 0; b < batch_size; ++b)
        {
            std::vector<Detection> detections = decodeSunPointRaw(
                heatData,
                heatShape,
                boxData,
                boxShape,
                offsetData,
                offsetShape,
                static_cast<int>(b),
                target_w,
                target_h,
                conf_thr,
                nms_thr,
                max_detections,
                &nmsTimeTmp);

            // 如果模型使用固定尺寸且与检测分辨率不同，缩放检测框坐标
            if (useFixed && (target_w != config.detection_resolution || target_h != config.detection_resolution))
            {
                float scaleX = static_cast<float>(config.detection_resolution) / target_w;
                float scaleY = static_cast<float>(config.detection_resolution) / target_h;
                for (auto& d : detections)
                {
                    d.box.x = static_cast<int>(d.box.x * scaleX);
                    d.box.y = static_cast<int>(d.box.y * scaleY);
                    d.box.width = static_cast<int>(d.box.width * scaleX);
                    d.box.height = static_cast<int>(d.box.height * scaleY);
                }
            }

            batchDetections[b] = std::move(detections);
        }

        auto t5 = std::chrono::steady_clock::now();
        publishTimingSnapshot(t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, nmsTimeTmp, t5 - t0);
        return batchDetections;
    }

    // 标准 YOLO 格式输出（单输出张量）
    float* outData = output_tensors.front().GetTensorMutableData<float>();
    Ort::TensorTypeAndShapeInfo outInfo = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> outShape = outInfo.GetShape(); // [B, rows, cols]
    if (outShape.size() < 3)
    {
        std::cerr << "[DirectMLDetector] Unexpected output tensor rank." << std::endl;
        return empty;
    }

    int rows = 0;
    int cols = 0;
    if (!tryInt64ToInt(outShape[1], &rows) || !tryInt64ToInt(outShape[2], &cols) || rows <= 0 || cols <= 0)
    {
        std::cerr << "[DirectMLDetector] Output tensor dimensions are invalid." << std::endl;
        return empty;
    }
    const int num_classes = rows - 4;

    for (size_t b = 0; b < batch_size; ++b)
    {
        const float* ptr = outData + b * rows * cols;
        std::vector<Detection> detections;

        std::vector<int64_t> shp = { static_cast<int64_t>(rows), static_cast<int64_t>(cols) };
        detections = postProcessYolo(ptr, shp, num_classes, conf_thr, nms_thr, max_detections, &nmsTimeTmp);

        // 缩放检测框（如果使用了固定输入尺寸）
        if (useFixed && (target_w != config.detection_resolution || target_h != config.detection_resolution))
        {
            float scaleX = static_cast<float>(config.detection_resolution) / target_w;
            float scaleY = static_cast<float>(config.detection_resolution) / target_h;
            for (auto& d : detections)
            {
                d.box.x = static_cast<int>(d.box.x * scaleX);
                d.box.y = static_cast<int>(d.box.y * scaleY);
                d.box.width = static_cast<int>(d.box.width * scaleX);
                d.box.height = static_cast<int>(d.box.height * scaleY);
            }
        }

        batchDetections[b] = std::move(detections);
    }
    auto t5 = std::chrono::steady_clock::now();

    publishTimingSnapshot(t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, nmsTimeTmp, t5 - t0);

    return batchDetections;
}

/**
 * getNumberOfClasses - 获取模型的检测类别数量
 *
 * 对于 SunPoint raw 输出固定返回 2（玩家和头部）。
 * 对于 YOLO 格式输出，从输出张量的形状计算类别数（channels - 4）。
 * @return 类别数量，未就绪返回 -1
 */
int DirectMLDetector::getNumberOfClasses()
{
    if (!isReady())
        return -1;

    if (sunpoint_raw_output)
        return 2;

    Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
    auto tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
    std::vector<int64_t> output_shape = tensor_info.GetShape();

    if (output_shape.size() == 3)
    {
        int channels = 0;
        if (!tryInt64ToInt(output_shape[1], &channels))
        {
            std::cerr << "[DirectMLDetector] Output tensor channel dimension is invalid." << std::endl;
            return -1;
        }
        int num_classes = channels - 4;
        return num_classes;
    }
    else
    {
        std::cerr << "[DirectMLDetector] Unexpected output tensor shape." << std::endl;
        return -1;
    }
}

/**
 * processFrame - 处理单帧图像（主线程调用入口）
 *
 * 将输入帧和源帧（可选，用于数据采集的原始帧）推送到推理线程。
 * 使用条件变量通知推理线程开始处理。
 *
 * @param detection_frame 用于检测的图像帧（已缩放到检测分辨率）
 * @param source_frame     原始源帧（用于数据采集和覆盖层绘制）
 * @param frameTiming      捕获与提交阶段时间契约
 */
void DirectMLDetector::processFrame(
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
    currentFrameTiming = frameTiming;
    frameReady = true;
    inferenceCV.notify_one();
}

/**
 * dmlInferenceThread - DirectML 推理线程主循环
 *
 * 线程循环逻辑：
 * 1. 检查后端配置是否为 DML，否则等待
 * 2. 检查模型是否已更改，若是则重新加载
 * 3. 等待新帧到来（通过条件变量）
 * 4. 执行批处理推理
 * 5. 应用深度掩码和圆形 FOV 过滤
 * 6. 将检测结果写入全局缓冲区
 * 7. 触发数据采集（如果启用）
 *
 * 异常保护：任何未捕获的异常都会导致线程退出并清理资源。
 */
void DirectMLDetector::dmlInferenceThread()
{
    try
    {
        while (!shouldExit)
        {
            // 检查后端配置
            if (config.backend != "DML")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // 模型热重载
            if (detector_model_changed.load())
            {
                const bool reloaded = initializeModel("models/" + config.ai_model);
                if (reloaded)
                {
                    detection_resolution_changed.store(true);
                    std::cout << "[DML] Detector reloaded: " << config.ai_model << std::endl;
                }
                detector_model_changed.store(false);
                if (!reloaded && !isReady())
                    detectionBuffer.clear();
            }

            if (!isReady())
            {
                detectionBuffer.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (detectionPaused)
            {
                detectionBuffer.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 等待并获取新帧
            cv::Mat frame;
            cv::Mat sourceFrame;
            FrameTiming frameTiming;
            bool hasNewFrame = false;
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                if (!frameReady && !shouldExit)
                    inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

                if (shouldExit) break;

                if (frameReady)
                {
                    frame = std::move(currentFrame);
                    sourceFrame = std::move(currentSourceFrame);
                    frameTiming = currentFrameTiming;
                    frameTiming.inferenceStartTime = FrameTiming::Clock::now();
                    frameReady = false;
                    hasNewFrame = true;
                }
            }

            // 执行推理和后处理
            if (hasNewFrame && !frame.empty())
            {
                std::vector<cv::Mat> batchFrames = { frame };
                auto detectionsBatch = detectBatch(batchFrames);
                if (detectionsBatch.empty())
                {
                    continue;
                }
                const std::vector<Detection>& detections = detectionsBatch.back();
                std::vector<Detection> filteredDetections = detections;
                filterDetectionsByDepthMask(filteredDetections);
                filterDetectionsByCircleFov(filteredDetections);

                // 转换为缓冲区格式
                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(filteredDetections.size());
                classes.reserve(filteredDetections.size());
                confidences.reserve(filteredDetections.size());
                for (const auto& d : filteredDetections)
                {
                    boxes.push_back(d.box);
                    classes.push_back(d.classId);
                    confidences.push_back(d.confidence);
                }

                detectionBuffer.set(boxes, classes, confidences, frameTiming);

                // 数据采集
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
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[DML] Inference thread crashed: " << e.what() << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
    catch (...)
    {
        std::cerr << "[DML] Inference thread crashed: unknown exception." << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
}
