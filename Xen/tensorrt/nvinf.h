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

#endif // NVINF_H
#endif