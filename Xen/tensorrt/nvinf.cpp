#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include "nvinf.h"
#include "Xen.h"
#include "trt_monitor.h"

Logger gLogger;

// TensorRT日志记录器，仅过滤WARNING及以上级别，特殊处理序列化断言错误
void Logger::log(nvinfer1::ILogger::Severity severity, const char* msg) noexcept
{
    if (severity <= nvinfer1::ILogger::Severity::kWARNING)
    {
        std::string devMsg = msg;

        std::string magicTag = "Serialization assertion plan->header.magicTag == rt::kPLAN_MAGIC_TAG failed.";
        std::string old_deserialization = "Using old deserialization call on a weight-separated plan file.";
        if (devMsg.find(magicTag) != std::string::npos || devMsg.find(old_deserialization) != std::string::npos)
        {
            std::cout << "[TensorRT] ERROR: This engine model is not suitable for execution. Please delete this engine model and set the ONNX version of this model in the settings. The program will export the model automatically." << std::endl;
        }
        else
        {
            std::cout << "[TensorRT] " << severityLevelName(severity) << ": " << msg << std::endl;
        }
    }
}

// 将严重级别枚举转换为字符串
const char* Logger::severityLevelName(nvinfer1::ILogger::Severity severity)
{
    switch (severity)
    {
        case nvinfer1::ILogger::Severity::kINTERNAL_ERROR: return "INTERNAL_ERROR";
        case nvinfer1::ILogger::Severity::kERROR:          return "ERROR";
        case nvinfer1::ILogger::Severity::kWARNING:        return "WARNING";
        case nvinfer1::ILogger::Severity::kINFO:           return "INFO";
        case nvinfer1::ILogger::Severity::kVERBOSE:        return "VERBOSE";
        default:                                           return "UNKNOWN";
    }
}

// 创建TensorRT构建器
nvinfer1::IBuilder* createInferBuilder()
{
    return nvinfer1::createInferBuilder(gLogger);
}

// 创建TensorRT网络定义（显式批处理模式）
nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder)
{
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    return builder->createNetworkV2(explicitBatch);
}

// 创建TensorRT构建器配置
nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder)
{
    return builder->createBuilderConfig();
}

// 从文件读取引擎数据并通过IRuntime反序列化
nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime)
{
    std::ifstream file(engineFile, std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] Error opening the engine file: " << engineFile << std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    file.read(engineData.data(), size);
    file.close();

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(engineData.data(), size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from file: " << engineFile << std::endl;
        return nullptr;
    }

    if (config.verbose)
    {
        std::cout << "[TensorRT] The engine was successfully loaded from the file: " << engineFile << std::endl;
    }
    return engine;
}

	// ========================================================================
	// peekOnnxInputInfo — 轻量级探测 ONNX 第一个输入的名称和维度
	// ========================================================================
	bool peekOnnxInputInfo(const std::string& onnxFile, nvinfer1::ILogger& logger,
	                       std::string& outName, nvinfer1::Dims& outDims)
	{
	    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(logger);
	    if (!builder)
	        return false;

	    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
	    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBatch);
	    nvinfer1::IBuilderConfig* bcfg = builder->createBuilderConfig();
	    if (!network || !bcfg)
	    {
	        delete bcfg;
	        delete network;
	        delete builder;
	        return false;
	    }

	    nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, logger);
	    if (!parser)
	    {
	        delete bcfg;
	        delete network;
	        delete builder;
	        return false;
	    }

	    if (!parser->parseFromFile(onnxFile.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
	    {
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return false;
	    }

	    nvinfer1::ITensor* inputTensor = network->getInput(0);
	    if (!inputTensor)
	    {
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return false;
	    }

	    outName = inputTensor->getName();
	    outDims = inputTensor->getDimensions();

	    delete parser;
	    delete bcfg;
	    delete network;
	    delete builder;
	    return true;
	}

	// ========================================================================
	// buildEngine — 统一引擎构建函数
	// ========================================================================
	nvinfer1::ICudaEngine* buildEngine(const BuildConfig& cfg, nvinfer1::IRuntime** outRuntime)
	{
	    if (!cfg.logger)
	    {
	        std::cerr << "[TensorRT] ERROR: Logger is required for engine build." << std::endl;
	        return nullptr;
	    }

	    // ---- 创建 builder / network / config ----
	    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(*cfg.logger);
	    if (!builder)
	    {
	        std::cerr << "[TensorRT] ERROR: Failed to create TensorRT builder." << std::endl;
	        return nullptr;
	    }

	    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
	    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBatch);
	    nvinfer1::IBuilderConfig* bcfg = builder->createBuilderConfig();
	    if (!network || !bcfg)
	    {
	        std::cerr << "[TensorRT] ERROR: Failed to create network or builder config." << std::endl;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }

	    // ---- 创建 ONNX 解析器 ----
	    nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, *cfg.logger);
	    if (!parser)
	    {
	        std::cerr << "[TensorRT] ERROR: Failed to create ONNX parser." << std::endl;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }

	    // ---- 进度监视器 ----
	    ImGuiProgressMonitor defaultMonitor;
	    nvinfer1::IProgressMonitor* monitor = cfg.progressMonitor ? cfg.progressMonitor : &defaultMonitor;
	    bcfg->setProgressMonitor(monitor);
	    TrtExportResetState();
	    gIsTrtExporting = true;
	    struct ScopedExportState
	    {
	        ~ScopedExportState()
	        {
	            std::lock_guard<std::mutex> lock(gProgressMutex);
	            gProgressPhases.clear();
	            gIsTrtExporting = false;
	            gTrtExportCancelRequested = false;
	            gTrtExportLastUpdateMs = TrtNowMs();
	        }
	    } exportState;

	    // ---- 解析 ONNX ----
	    if (!parser->parseFromFile(cfg.onnxPath.c_str(), static_cast<int>(cfg.parserSeverity)))
	    {
	        std::cerr << "[TensorRT] ERROR: Error parsing the ONNX file: " << cfg.onnxPath << std::endl;
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }

	    // ---- 确定输入张量名称 ----
	    std::string inName = cfg.inputName;
	    if (inName.empty())
	    {
	        nvinfer1::ITensor* inputTensor = network->getInput(0);
	        if (inputTensor)
	            inName = inputTensor->getName();
	    }

	    // ---- 优化配置文件（仅当维度已指定时） ----
	    if (cfg.inputDimsMin.d[2] > 0 || cfg.inputDimsMin.d[3] > 0)
	    {
	        nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
	        profile->setDimensions(inName.c_str(), nvinfer1::OptProfileSelector::kMIN, cfg.inputDimsMin);
	        profile->setDimensions(inName.c_str(), nvinfer1::OptProfileSelector::kOPT, cfg.inputDimsOpt);
	        profile->setDimensions(inName.c_str(), nvinfer1::OptProfileSelector::kMAX, cfg.inputDimsMax);
	        bcfg->addOptimizationProfile(profile);

	        if (cfg.verbose)
	        {
	            bool isStatic = (cfg.inputDimsMin.d[2] == cfg.inputDimsMax.d[2] &&
	                             cfg.inputDimsMin.d[3] == cfg.inputDimsMax.d[3]);
	            if (isStatic)
	                std::cout << "[TensorRT] Static profile " << cfg.inputDimsMin.d[2] << "x" << cfg.inputDimsMin.d[3] << std::endl;
	            else
	                std::cout << "[TensorRT] Dynamic profile "
	                          << cfg.inputDimsMin.d[2] << "/" << cfg.inputDimsOpt.d[2] << "/" << cfg.inputDimsMax.d[2] << std::endl;
	        }
	    }

	    // ---- 精度标志 ----
	    if (cfg.enableFp16)
	    {
	        if (cfg.verbose)
	            std::cout << "[TensorRT] Set FP16" << std::endl;
	        bcfg->setFlag(nvinfer1::BuilderFlag::kFP16);
	    }
	    if (cfg.enableFp8)
	    {
	        if (cfg.verbose)
	            std::cout << "[TensorRT] Set FP8" << std::endl;
	        bcfg->setFlag(nvinfer1::BuilderFlag::kFP8);
	    }

	    // ---- 工作空间（仅当显式指定时） ----
	    if (cfg.workspaceSize > 0)
	    {
	        bcfg->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, cfg.workspaceSize);
	    }

	    // ---- 可选 CUDA 流 ----
	    cudaStream_t stream = nullptr;
	    if (cfg.syncStream)
	        cudaStreamCreate(&stream);

	    std::cout << "[TensorRT] Building engine (this may take several minutes)..." << std::endl;

	    // ---- 构建序列化网络 ----
	    nvinfer1::IHostMemory* plan = builder->buildSerializedNetwork(*network, *bcfg);
	    if (!plan)
	    {
	        std::cerr << "[TensorRT] ERROR: Could not build the engine" << std::endl;
	        if (cfg.syncStream)
	            cudaStreamDestroy(stream);
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }

	    // ---- 反序列化引擎 ----
	    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(*cfg.logger);
	    if (!runtime)
	    {
	        std::cerr << "[TensorRT] ERROR: Could not create inference runtime" << std::endl;
	        if (cfg.syncStream)
	        {
	            cudaStreamSynchronize(stream);
	            cudaStreamDestroy(stream);
	        }
	        delete plan;
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }
	    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(plan->data(), plan->size());

	    if (cfg.syncStream)
	    {
	        cudaStreamSynchronize(stream);
	        cudaStreamDestroy(stream);
	    }

	    if (!engine)
	    {
	        std::cerr << "[TensorRT] ERROR: Could not create engine" << std::endl;
	        delete runtime;
	        delete plan;
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }

	    // ---- 保存引擎文件 ----
	    std::string engineFile = cfg.enginePath;
	    if (engineFile.empty())
	    {
	        engineFile = cfg.onnxPath.substr(0, cfg.onnxPath.find_last_of('.')) + ".engine";
	    }

	    std::ofstream p(engineFile, std::ios::binary);
	    if (!p)
	    {
	        std::cerr << "[TensorRT] ERROR: Could not open file to write: " << engineFile << std::endl;
	        delete engine;
	        delete runtime;
	        delete plan;
	        delete parser;
	        delete bcfg;
	        delete network;
	        delete builder;
	        return nullptr;
	    }
	    p.write(static_cast<const char*>(plan->data()), plan->size());
	    p.close();

	    if (outRuntime)
	        *outRuntime = runtime;
	    // NOTE: do NOT delete runtime — ICudaEngine in TensorRT 10 may internally
	    // reference runtime resources. Caller assumes ownership via engine (and
	    // optionally via outRuntime).
	    delete plan;
	    delete parser;
	    delete bcfg;
	    delete network;
	    delete builder;

	    if (cfg.verbose)
	        std::cout << "[TensorRT] The engine was built and saved to the file: " << engineFile << std::endl;
	    return engine;
	}

	// ========================================================================
	// buildEngineFromOnnx — 薄封装，保持与原有调用者的兼容性
	// ========================================================================
	nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger)
	{
	    // ---- 探测模型输入维度以决定静态/动态配置 ----
	    std::string inName;
	    nvinfer1::Dims inDims{};
	    int H = -1, W = -1;

	    if (peekOnnxInputInfo(onnxFile, logger, inName, inDims))
	    {
	        if (inDims.nbDims >= 4)
	        {
	            H = (inDims.d[2] > 0) ? static_cast<int>(inDims.d[2]) : -1;
	            W = (inDims.d[3] > 0) ? static_cast<int>(inDims.d[3]) : -1;
	        }
	    }

	    bool fixedByModel  = (H > 0 && W > 0);
	    bool fixedByConfig = config.fixed_input_size;
	    bool makeStatic    = fixedByModel || fixedByConfig;

	    if (fixedByConfig && (H <= 0 || W <= 0))
	        H = W = config.detection_resolution;

	    // ---- 填充 BuildConfig ----
	    BuildConfig bcfg;
	    bcfg.onnxPath       = onnxFile;
	    bcfg.logger         = &logger;
	    bcfg.inputName      = inName;
	    bcfg.enableFp16     = config.export_enable_fp16;
	    bcfg.enableFp8      = config.export_enable_fp8;
	    bcfg.syncStream     = true;
	    bcfg.verbose        = config.verbose;

	    if (makeStatic)
	    {
	        bcfg.inputDimsMin = nvinfer1::Dims4{ 1, 3, H, W };
	        bcfg.inputDimsOpt = nvinfer1::Dims4{ 1, 3, H, W };
	        bcfg.inputDimsMax = nvinfer1::Dims4{ 1, 3, H, W };
	    }
	    else
	    {
	        bcfg.inputDimsMin = nvinfer1::Dims4{ 1, 3, 160, 160 };
	        bcfg.inputDimsOpt = nvinfer1::Dims4{ 1, 3, 320, 320 };
	        bcfg.inputDimsMax = nvinfer1::Dims4{ 1, 3, 640, 640 };
	    }

	    // ---- 委托给统一构建函数 ----
	    return buildEngine(bcfg, nullptr);
	}
#endif
