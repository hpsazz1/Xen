#ifdef USE_CUDA
#ifndef NVINF_H
#define NVINF_H

// TensorRT 工具封装
// 提供 Logger 类、引擎构建/加载等辅助函数

#include "NvInfer.h"
#include "Xen.h"

// TensorRT 日志记录器
class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override;
    // 获取严重等级的字符串名称
    static const char* severityLevelName(Severity severity);
};

// 全局日志记录器实例
extern Logger gLogger;

// 创建 TensorRT 构建器
inline nvinfer1::IBuilder* createInferBuilder();
// 创建网络定义
inline nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder);
// 创建构建器配置
inline nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder);

// 从序列化文件加载 TensorRT 引擎
nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime);
// 从 ONNX 文件构建 TensorRT 引擎
nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger);

// ============================================================================
// 统一引擎构建配置与函数
// ============================================================================

// 引擎构建配置：将两个构建流程的差异参数统一为一个结构体
struct BuildConfig
{
    std::string onnxPath;                                  // ONNX 模型文件路径（必需）
    std::string enginePath;                                // 输出 .engine 文件路径（空则自动从 onnxPath 推导）
    std::string inputName;                                 // 输入张量名称（空则使用网络第一个输入）

    nvinfer1::Dims4 inputDimsMin;                          // 优化配置文件 MIN 维度（d[2]==0 表示跳过优化配置文件）
    nvinfer1::Dims4 inputDimsOpt;                          // 优化配置文件 OPT 维度
    nvinfer1::Dims4 inputDimsMax;                          // 优化配置文件 MAX 维度

    bool enableFp16 = false;                               // 启用 FP16 精度
    bool enableFp8  = false;                               // 启用 FP8 精度
    size_t workspaceSize = 0;                              // 工作空间内存池大小（字节，0 表示 TensorRT 默认值）

    nvinfer1::ILogger::Severity parserSeverity =           // ONNX 解析器的日志严重级别
        nvinfer1::ILogger::Severity::kWARNING;

    nvinfer1::ILogger* logger = nullptr;                   // 日志记录器（必需）

    nvinfer1::IProgressMonitor* progressMonitor = nullptr; // 构建进度监视器（空则使用默认 ImGuiProgressMonitor）

    bool syncStream = false;                               // 构建前后创建并同步一个 CUDA 流
    bool verbose = false;                                  // 启用详细日志输出
};

// 轻量级探测：从 ONNX 文件读取第一个输入张量的名称和维度（不进行完整构建）
// 返回 true 表示成功，输出 outName 和 outDims
bool peekOnnxInputInfo(const std::string& onnxFile, nvinfer1::ILogger& logger,
                       std::string& outName, nvinfer1::Dims& outDims);

// 统一引擎构建函数：从 ONNX 构建 TensorRT 引擎并保存到 .engine 文件
// 返回 ICudaEngine* 指针（调用者拥有所有权）。失败时返回 nullptr。
// 如果 outRuntime 非空，则将反序列化所用的 IRuntime 也返回给调用者管理生命周期。
nvinfer1::ICudaEngine* buildEngine(const BuildConfig& cfg,
                                   nvinfer1::IRuntime** outRuntime = nullptr);

#endif // NVINF_H
#endif