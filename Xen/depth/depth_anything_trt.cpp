#ifdef USE_CUDA

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "depth_anything_trt.h"

#include <NvOnnxParser.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include "other_tools.h"
#include "tensorrt/nvinf.h"
#include "tensorrt/trt_monitor.h"

namespace depth_anything
{
    namespace
    {
        // 是否启用 FP16 精度构建引擎
        constexpr bool kEnableFp16 = true;
        // 输入图像的最小边长
        constexpr int kMinInputSize = 160;
        // 输入图像的最大边长
        constexpr int kMaxInputSize = 640;
        // 构建引擎时的最优输入尺寸
        constexpr int kOptInputSize = 224;

        // 安全地将 int64_t 转换为 int，溢出时返回 false
        bool TensorDimToInt(int64_t value, int& out)
        {
            if (value <= 0 || value > static_cast<int64_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            out = static_cast<int>(value);
            return true;
        }

        // 比较两个路径组件是否相等
        // Windows 下不区分大小写，Linux 下区分大小写
        bool PathComponentEquals(const std::filesystem::path& left, const std::filesystem::path& right)
        {
#ifdef _WIN32
            const std::string leftStr = left.string();
            const std::string rightStr = right.string();
            return OtherTools::ToLowerAscii(leftStr) == OtherTools::ToLowerAscii(rightStr);
#else
            return left == right;
#endif
        }

        // 检查 path 是否以 prefix 为前缀（逐组件比较）
        bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& prefix)
        {
            auto pathIt = path.begin();
            auto prefixIt = prefix.begin();
            for (; prefixIt != prefix.end(); ++prefixIt, ++pathIt)
            {
                if (pathIt == path.end())
                {
                    return false;
                }
                if (!PathComponentEquals(*pathIt, *prefixIt))
                {
                    return false;
                }
            }
            return true;
        }

        // 检查路径中是否包含 ".." 路径遍历攻击
        bool ContainsParentTraversal(const std::filesystem::path& path)
        {
            for (const auto& part : path)
            {
                if (part == "..")
                {
                    return true;
                }
            }
            return false;
        }

        // 根据 ONNX 路径生成对应的 engine 文件路径（替换扩展名为 .engine）
        std::string MakeEnginePathFromOnnx(const std::string& onnxPath)
        {
            std::filesystem::path enginePath(onnxPath);
            enginePath.replace_extension(".engine");
            return enginePath.string();
        }

        // 解析深度模型的路径，确保路径安全（限制在 depth_models 目录内）
        // 返回解析后的绝对路径，若路径无效则返回错误描述
        bool ResolveDepthModelPath(const std::string& modelPath, std::string& resolvedPath, std::string& error)
        {
            if (modelPath.empty())
            {
                error = "Depth model path is empty.";
                return false;
            }

            std::filesystem::path requested(modelPath);
            if (ContainsParentTraversal(requested))
            {
                error = "Depth model path must stay inside depth_models.";
                return false;
            }

            const std::filesystem::path base = std::filesystem::path("depth_models").lexically_normal();
            std::filesystem::path resolved = requested;
            if (!requested.is_absolute() && !requested.has_root_name() && !requested.has_root_directory())
            {
                if (!PathStartsWith(requested, base))
                {
                    resolved = base / requested;
                }
            }
            else
            {
                resolved = requested;
            }

            resolved = resolved.lexically_normal();
            const std::filesystem::path baseAbs = std::filesystem::absolute(base).lexically_normal();
            const std::filesystem::path resolvedAbs = std::filesystem::absolute(resolved).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::exists(baseAbs, ec) || ec || !std::filesystem::is_directory(baseAbs, ec) || ec)
            {
                error = "Depth model directory not found: " + baseAbs.string();
                return false;
            }
            if (!PathStartsWith(resolvedAbs, baseAbs))
            {
                error = "Depth model path must stay inside depth_models.";
                return false;
            }

            resolvedPath = resolvedAbs.string();
            return true;
        }

        // 检查 CUDA 操作是否成功，失败时记录错误信息
        bool CheckCuda(cudaError_t status, const char* action, std::string& last_error)
        {
            if (status == cudaSuccess)
            {
                return true;
            }
            last_error = std::string(action) + ": " + cudaGetErrorString(status);
            return false;
        }
    }

    // DepthAnythingTrt 构造函数
    // 初始化所有成员变量为默认值：
    //   - 输入尺寸：224x224
    //   - 归一化参数：ImageNet 均值/标准差（123.675, 116.28, 103.53 / 58.395, 57.12, 57.375）
    //   - 颜色映射类型：TWILIGHT
    //   - TensorRT/CUDA 指针初始化为空
    DepthAnythingTrt::DepthAnythingTrt()
        : input_w(kOptInputSize)
        , input_h(kOptInputSize)
        , min_input_size(kMinInputSize)
        , max_input_size(kMaxInputSize)
        , dynamic_input(false)
        , mean{ 123.675f, 116.28f, 103.53f }
        , stddev{ 58.395f, 57.12f, 57.375f }
        , colormap_type(COLORMAP_TWILIGHT)
        , runtime(nullptr)
        , engine(nullptr)
        , context(nullptr)
        , input_name()
        , output_name()
        , input_buffer(nullptr)
        , output_buffer(nullptr)
        , input_capacity(0)
        , output_capacity(0)
        , output_w(0)
        , output_h(0)
        , stream(nullptr)
        , initialized(false)
    {
    }

    // DepthAnythingTrt 析构函数，调用 reset() 释放资源
    DepthAnythingTrt::~DepthAnythingTrt()
    {
        reset();
    }

    // 检查模型是否已初始化完成
    bool DepthAnythingTrt::ready() const
    {
        return initialized;
    }

    // 获取最后一次错误信息
    const std::string& DepthAnythingTrt::lastError() const
    {
        return last_error;
    }

    // 设置输出深度图的颜色映射类型
    // type 必须是 OpenCV 支持的颜色映射枚举值，否则使用默认的 TWILIGHT
    void DepthAnythingTrt::setColormap(int type)
    {
        if (type < COLORMAP_AUTUMN || type > COLORMAP_DEEPGREEN)
        {
            colormap_type = COLORMAP_TWILIGHT;
            return;
        }
        colormap_type = type;
    }

    // 获取当前的颜铯映射类型
    int DepthAnythingTrt::colormapType() const
    {
        return colormap_type;
    }

    // 重置所有资源
    // 销毁 CUDA 流和设备缓冲区，释放 TensorRT runtime/engine/context
    void DepthAnythingTrt::reset()
    {
        initialized = false;
        last_error.clear();

        if (stream)
        {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }

        if (input_buffer)
        {
            cudaFree(input_buffer);
            input_buffer = nullptr;
        }
        if (output_buffer)
        {
            cudaFree(output_buffer);
            output_buffer = nullptr;
        }
        input_name.clear();
        output_name.clear();
        input_capacity = 0;
        output_capacity = 0;
        output_w = 0;
        output_h = 0;

        depth_data.clear();
        context.reset();
        engine.reset();
        runtime.reset();
    }

    // 初始化深度估计模型
    // 完整的初始化流程：
    //   1. 解析模型路径（安全检查）
    //   2. 加载 TensorRT engine（或从 ONNX 构建）
    //   3. 枚举 I/O tensor，验证输入/输出各只有一个
    //   4. 读取输入维度，检测动态形状
    //   5. 创建 CUDA 流
    //   6. 分配输入/输出缓冲区
    //   7. 绑定 tensor 地址
    bool DepthAnythingTrt::initialize(const std::string& modelPath, nvinfer1::ILogger& logger)
    {
        reset();
        dynamic_input = false;
        min_input_size = kMinInputSize;
        max_input_size = kMaxInputSize;

        std::string resolvedPath;
        if (!ResolveDepthModelPath(modelPath, resolvedPath, last_error))
        {
            return false;
        }

        if (!std::filesystem::exists(resolvedPath))
        {
            last_error = "Depth model file not found: " + resolvedPath;
            return false;
        }

        if (!loadEngine(resolvedPath, logger))
        {
            if (last_error.empty())
            {
                last_error = "Failed to load depth model: " + resolvedPath;
            }
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        const int nb_io = engine->getNbIOTensors();
        if (nb_io <= 0)
        {
            last_error = "Depth engine has no I/O tensors.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        int input_count = 0;
        int output_count = 0;
        for (int i = 0; i < nb_io; i++)
        {
            const char* name = engine->getIOTensorName(i);
            if (!name)
            {
                last_error = "Depth engine has invalid tensor name.";
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }

            if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
            {
                input_count++;
                if (input_name.empty())
                {
                    input_name = name;
                }
            }
            else
            {
                output_count++;
                if (output_name.empty())
                {
                    output_name = name;
                }
            }
        }

        if (input_count != 1 || output_count != 1 || input_name.empty() || output_name.empty())
        {
            last_error = "Depth engine must have exactly 1 input and 1 output; got " + std::to_string(input_count) + " inputs and " + std::to_string(output_count) + " outputs.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        auto input_dims = engine->getTensorShape(input_name.c_str());
        if (input_dims.nbDims < 3)
        {
            last_error = "Depth input dimensions are invalid.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }
        bool has_dynamic = false;
        for (int i = 0; i < input_dims.nbDims; i++)
        {
            if (input_dims.d[i] == -1)
            {
                has_dynamic = true;
                break;
            }
        }

        if (input_dims.nbDims >= 3 && input_dims.d[input_dims.nbDims - 3] > 0 && input_dims.d[input_dims.nbDims - 3] != 3)
        {
            last_error = "Depth input must have 3 channels.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        if (has_dynamic)
        {
            dynamic_input = true;
            min_input_size = kMinInputSize;
            max_input_size = kMaxInputSize;
            input_h = max_input_size;
            input_w = max_input_size;
            if (!setInputShape(input_w, input_h))
            {
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }
        }
        else
        {
            if (!TensorDimToInt(input_dims.d[input_dims.nbDims - 2], input_h) ||
                !TensorDimToInt(input_dims.d[input_dims.nbDims - 1], input_w))
            {
                last_error = "Depth input dimensions are invalid.";
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }
            min_input_size = input_w;
            max_input_size = input_w;
        }

        if (!CheckCuda(cudaStreamCreate(&stream), "cudaStreamCreate", last_error))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        const size_t input_elements = static_cast<size_t>(3) * static_cast<size_t>(input_h) * static_cast<size_t>(input_w);
        if (!ensureInputCapacity(input_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        int out_h = 0;
        int out_w = 0;
        size_t output_elements = 0;
        if (!getOutputShape(out_h, out_w, output_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }
        if (!ensureOutputCapacity(output_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        if (!setTensorAddresses())
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        initialized = true;
        return true;
    }

    // 从 ONNX 模型导出 TensorRT engine
    // 构建引擎并保存到 .engine 文件，可选通过 enginePath 返回路径
    bool DepthAnythingTrt::exportEngine(const std::string& modelPath, nvinfer1::ILogger& logger, std::string* enginePath)
    {
        reset();

        std::string resolvedPath;
        if (!ResolveDepthModelPath(modelPath, resolvedPath, last_error))
        {
            return false;
        }

        if (!OtherTools::HasExtensionCaseInsensitive(resolvedPath, ".onnx"))
        {
            last_error = "Depth export expects an .onnx model path.";
            return false;
        }

        if (!std::filesystem::exists(resolvedPath))
        {
            last_error = "Depth model file not found: " + resolvedPath;
            return false;
        }

        if (!buildEngine(resolvedPath, logger))
        {
            if (last_error.empty())
            {
                last_error = "Failed to build depth engine from ONNX: " + resolvedPath;
            }
            return false;
        }

        // Engine file already written by unified buildEngine
        if (enginePath)
            *enginePath = MakeEnginePathFromOnnx(resolvedPath);
        return true;
    }

    // 预处理输入图像
    // 流程：通道转换 → 尺寸缩放 → CHW 格式排列 → 归一化 (pixel - mean) / stddev
    // 支持 1/3/4 通道输入，统一转为 3 通道 BGR
    bool DepthAnythingTrt::preprocess(const cv::Mat& image, std::vector<float>& input_tensor)
    {
        if (image.empty())
        {
            last_error = "Depth input image is empty.";
            return false;
        }

        cv::Mat input_image;
        if (image.channels() == 3)
        {
            input_image = image;
        }
        else if (image.channels() == 4)
        {
            cv::cvtColor(image, input_image, cv::COLOR_BGRA2BGR);
        }
        else if (image.channels() == 1)
        {
            cv::cvtColor(image, input_image, cv::COLOR_GRAY2BGR);
        }
        else
        {
            last_error = "Depth input image must have 1, 3, or 4 channels.";
            return false;
        }

        if (input_image.depth() != CV_8U)
        {
            cv::Mat converted;
            input_image.convertTo(converted, CV_8U);
            input_image = converted;
        }

        auto resized = resize_depth(input_image, input_w, input_h);
        cv::Mat resized_image = std::get<0>(resized);
        if (resized_image.empty())
        {
            last_error = "Failed to resize depth input image.";
            return false;
        }
        if (resized_image.rows != input_h || resized_image.cols != input_w)
        {
            last_error = "Depth input resize produced unexpected dimensions.";
            return false;
        }

        input_tensor.resize(static_cast<size_t>(3) * static_cast<size_t>(input_h) * static_cast<size_t>(input_w));
        size_t idx = 0;
        for (int k = 0; k < 3; k++)
        {
            for (int i = 0; i < resized_image.rows; i++)
            {
                const cv::Vec3b* row = resized_image.ptr<cv::Vec3b>(i);
                for (int j = 0; j < resized_image.cols; j++)
                {
                    input_tensor[idx++] = (static_cast<float>(row[j][k]) - mean[k]) / stddev[k];
                }
            }
        }
        return true;
    }

    // 运行推理
    // 完整推理流程：
    //   1. 选择输入尺寸（动态形状支持）
    //   2. 预处理图像
    //   3. 确保输入/输出缓冲区容量
    //   4. 绑定 tensor 地址
    //   5. CPU → GPU 拷贝输入数据
    //   6. 执行 TensorRT 推理 (enqueueV3)
    //   7. GPU → CPU 拷贝输出深度数据
    //   8. 同步 CUDA 流
    //   9. 将深度图归一化到 [0, 255]
    bool DepthAnythingTrt::runInference(const cv::Mat& image, cv::Mat& depth_norm)
    {
        if (!initialized || image.empty())
        {
            return false;
        }

        int target_size = selectInputSize(image);
        if (dynamic_input && (target_size != input_w || target_size != input_h))
        {
            if (!setInputShape(target_size, target_size))
            {
                return false;
            }
        }

        std::vector<float> input;
        if (!preprocess(image, input))
        {
            return false;
        }
        if (!ensureInputCapacity(input.size()))
        {
            return false;
        }

        int out_h = 0;
        int out_w = 0;
        size_t output_elements = 0;
        if (!getOutputShape(out_h, out_w, output_elements))
        {
            return false;
        }
        if (!ensureOutputCapacity(output_elements))
        {
            return false;
        }

        if (!setTensorAddresses())
        {
            return false;
        }

        const size_t input_bytes = input.size() * sizeof(float);
        if (!CheckCuda(cudaMemcpyAsync(input_buffer, input.data(), input_bytes, cudaMemcpyHostToDevice, stream), "cudaMemcpyAsync input", last_error))
        {
            return false;
        }

        if (!context->enqueueV3(stream))
        {
            last_error = "Failed to execute depth inference.";
            return false;
        }

        const size_t output_bytes = output_elements * sizeof(float);
        if (!CheckCuda(cudaMemcpyAsync(depth_data.data(), output_buffer, output_bytes, cudaMemcpyDeviceToHost, stream), "cudaMemcpyAsync output", last_error))
        {
            return false;
        }
        if (!CheckCuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize", last_error))
        {
            return false;
        }

        cv::Mat depth_mat(out_h, out_w, CV_32FC1, depth_data.data());
        cv::normalize(depth_mat, depth_norm, 0, 255, cv::NORM_MINMAX, CV_8U);
        return true;
    }

    // 预测深度图并应用颜色映射
    // 返回与原始图像尺寸相同的伪彩色深度图
    cv::Mat DepthAnythingTrt::predict(const cv::Mat& image)
    {
        cv::Mat depth_norm;
        if (!runInference(image, depth_norm))
        {
            return {};
        }

        cv::Mat colormap;
        cv::applyColorMap(depth_norm, colormap, colormap_type);

        cv::Mat output;
        cv::resize(colormap, output, image.size());
        return output;
    }

    // 预测原始深度图（无颜色映射）
    // 返回归一化到 [0, 255] 的单通道深度图，缩放到原始图像尺寸
    cv::Mat DepthAnythingTrt::predictDepth(const cv::Mat& image)
    {
        cv::Mat depth_norm;
        if (!runInference(image, depth_norm))
        {
            return {};
        }

        cv::Mat output;
        cv::resize(depth_norm, output, image.size(), 0.0, 0.0, cv::INTER_LINEAR);
        return output;
    }

    // 根据输入图像选择推理输入尺寸
    // 以图像较长边为基准，限制在 [min_input_size, max_input_size] 范围内
    int DepthAnythingTrt::selectInputSize(const cv::Mat& image) const
    {
        if (min_input_size <= 0 || max_input_size <= 0)
        {
            return input_w;
        }

        int long_side = std::max(image.cols, image.rows);
        return std::clamp(long_side, min_input_size, max_input_size);
    }

    // 设置动态输入形状（NCHW 格式）
    bool DepthAnythingTrt::setInputShape(int w, int h)
    {
        if (input_name.empty())
        {
            last_error = "Invalid depth input tensor name.";
            return false;
        }
        if (!context->setInputShape(input_name.c_str(), nvinfer1::Dims4{ 1, 3, h, w }))
        {
            last_error = "Failed to set depth input shape.";
            return false;
        }

        input_w = w;
        input_h = h;
        return true;
    }

    // 加载 TensorRT engine
    // 如果输入是 .onnx 文件，则先构建 engine 再保存
    // 如果输入是 .engine 文件，则直接反序列化
    bool DepthAnythingTrt::loadEngine(const std::string& modelPath, nvinfer1::ILogger& logger)
    {
        if (OtherTools::HasExtensionCaseInsensitive(modelPath, ".onnx"))
        {
            // buildEngine (via unified buildEngine) already writes the .engine file
            if (!buildEngine(modelPath, logger))
            {
                return false;
            }
            return true;
        }

        std::ifstream engineStream(modelPath, std::ios::binary);
        if (!engineStream.is_open())
        {
            last_error = "Unable to open depth engine: " + modelPath;
            return false;
        }

        engineStream.seekg(0, std::ios::end);
        const std::streampos endPos = engineStream.tellg();
        if (endPos <= 0)
        {
            last_error = "Depth engine file is empty: " + modelPath;
            return false;
        }
        const size_t modelSize = static_cast<size_t>(endPos);
        engineStream.seekg(0, std::ios::beg);
        std::vector<char> engineData(modelSize);
        engineStream.read(engineData.data(), modelSize);
        const bool read_ok = engineStream.good() || engineStream.eof();
        engineStream.close();
        if (!read_ok)
        {
            last_error = "Failed to read depth engine: " + modelPath;
            return false;
        }

        runtime.reset(nvinfer1::createInferRuntime(logger));
        if (!runtime)
        {
            last_error = "Failed to create depth runtime.";
            return false;
        }
        engine.reset(runtime->deserializeCudaEngine(engineData.data(), modelSize));
        if (!engine)
        {
            last_error = "Failed to deserialize depth engine: " + modelPath;
            return false;
        }
        context.reset(engine->createExecutionContext());
        if (!context)
        {
            last_error = "Failed to create depth execution context.";
            return false;
        }
        return true;
    }

    // 从 ONNX 文件构建 TensorRT engine（委托给统一构建函数）
    bool DepthAnythingTrt::buildEngine(const std::string& onnxPath, nvinfer1::ILogger& logger)
    {
        // ---- 探测模型输入维度以判断动态/静态 ----
        std::string inName;
        nvinfer1::Dims inDims{};
        bool has_dynamic = true;  // 安全默认值：假定为动态

        if (peekOnnxInputInfo(onnxPath, logger, inName, inDims))
        {
            has_dynamic = false;
            for (int i = 0; i < inDims.nbDims; i++)
            {
                if (inDims.d[i] == -1)
                {
                    has_dynamic = true;
                    break;
                }
            }
        }

        // ---- 填充 BuildConfig ----
        BuildConfig cfg;
        cfg.onnxPath       = onnxPath;
        cfg.enginePath     = MakeEnginePathFromOnnx(onnxPath);
        cfg.logger         = &logger;
        cfg.inputName      = inName;
        cfg.enableFp16     = kEnableFp16;
        cfg.parserSeverity = nvinfer1::ILogger::Severity::kINFO;

        if (has_dynamic)
        {
            int opt_size = std::clamp(kOptInputSize, kMinInputSize, kMaxInputSize);
            cfg.inputDimsMin = nvinfer1::Dims4{ 1, 3, kMinInputSize, kMinInputSize };
            cfg.inputDimsOpt = nvinfer1::Dims4{ 1, 3, opt_size, opt_size };
            cfg.inputDimsMax = nvinfer1::Dims4{ 1, 3, kMaxInputSize, kMaxInputSize };
        }
        // 静态模型：维度保持为零（哨兵值），跳过优化配置文件 —— 与原始行为一致

        // ---- 委托给统一构建函数 ----
        nvinfer1::IRuntime* rawRuntime = nullptr;
        nvinfer1::ICudaEngine* rawEngine = ::buildEngine(cfg, &rawRuntime);
        if (!rawEngine)
        {
            if (gTrtExportCancelRequested.load())
                last_error = "Depth export canceled.";
            else if (last_error.empty())
                last_error = "Failed to build depth engine from ONNX.";
            return false;
        }

        engine.reset(rawEngine);
        runtime.reset(rawRuntime);
        context.reset(engine->createExecutionContext());

        if (!engine || !context)
        {
            last_error = "Failed to build depth engine from ONNX.";
            return false;
        }

        return true;
    }

    // 序列化 engine 并保存到文件
    // 将 engine 写入与 ONNX 同目录的 .engine 文件中
    bool DepthAnythingTrt::saveEngine(const std::string& onnxPath, std::string* enginePath)
    {
        if (!engine)
        {
            last_error = "Depth engine is not available for serialization.";
            return false;
        }

        std::string engine_path = MakeEnginePathFromOnnx(onnxPath);
        if (engine_path.empty() || engine_path == onnxPath)
        {
            last_error = "Failed to resolve depth engine output path.";
            return false;
        }

        nvinfer1::IHostMemory* data = engine->serialize();
        if (!data)
        {
            last_error = "Failed to serialize depth engine.";
            return false;
        }
        std::ofstream file(engine_path, std::ios::binary | std::ios::out);
        if (!file.is_open())
        {
            last_error = "Unable to write depth engine: " + engine_path;
            delete data;
            return false;
        }

        file.write(reinterpret_cast<const char*>(data->data()), data->size());
        if (!file.good())
        {
            last_error = "Failed to write depth engine: " + engine_path;
            file.close();
            delete data;
            return false;
        }
        file.close();
        delete data;

        if (enginePath)
        {
            *enginePath = engine_path;
        }
        last_error.clear();
        return true;
    }

    // 获取输出 tensor 的形状和元素数
    // 验证输出为单通道（空间维度 == 总元素数）
    bool DepthAnythingTrt::getOutputShape(int& out_h, int& out_w, size_t& out_elements)
    {
        if (output_name.empty())
        {
            last_error = "Invalid depth output tensor name.";
            return false;
        }

        auto output_dims = context->getTensorShape(output_name.c_str());
        if (output_dims.nbDims < 2)
        {
            last_error = "Depth output dimensions are invalid.";
            return false;
        }

        size_t elements = 1;
        for (int i = 0; i < output_dims.nbDims; i++)
        {
            if (output_dims.d[i] <= 0)
            {
                last_error = "Depth output dimensions are not fully specified.";
                return false;
            }
            elements *= static_cast<size_t>(output_dims.d[i]);
        }

        if (!TensorDimToInt(output_dims.d[output_dims.nbDims - 2], out_h) ||
            !TensorDimToInt(output_dims.d[output_dims.nbDims - 1], out_w))
        {
            last_error = "Depth output dimensions are invalid.";
            return false;
        }

        const size_t spatial = static_cast<size_t>(out_h) * static_cast<size_t>(out_w);
        if (spatial == 0 || elements % spatial != 0 || (elements / spatial) != 1)
        {
            last_error = "Depth output must be single-channel.";
            return false;
        }

        out_elements = elements;
        output_h = out_h;
        output_w = out_w;
        return true;
    }

    // 确保输入缓冲区有足够容量，不足时重新分配 GPU 显存
    bool DepthAnythingTrt::ensureInputCapacity(size_t elements)
    {
        if (elements == 0)
        {
            last_error = "Depth input size is zero.";
            return false;
        }
        if (elements <= input_capacity && input_buffer)
        {
            return true;
        }
        if (input_buffer)
        {
            cudaFree(input_buffer);
            input_buffer = nullptr;
        }
        if (!CheckCuda(cudaMalloc(&input_buffer, elements * sizeof(float)), "cudaMalloc input", last_error))
        {
            return false;
        }
        input_capacity = elements;
        return true;
    }

    // 确保输出缓冲区有足够容量，同时调整主机端 depth_data 大小
    bool DepthAnythingTrt::ensureOutputCapacity(size_t elements)
    {
        if (elements == 0)
        {
            last_error = "Depth output size is zero.";
            return false;
        }
        if (elements <= output_capacity && output_buffer)
        {
            if (depth_data.size() < output_capacity)
            {
                depth_data.resize(output_capacity);
            }
            return true;
        }
        if (output_buffer)
        {
            cudaFree(output_buffer);
            output_buffer = nullptr;
        }
        if (!CheckCuda(cudaMalloc(&output_buffer, elements * sizeof(float)), "cudaMalloc output", last_error))
        {
            return false;
        }
        output_capacity = elements;
        if (depth_data.size() < output_capacity)
        {
            depth_data.resize(output_capacity);
        }
        return true;
    }

    // 绑定输入/输出 tensor 的 GPU 内存地址到执行上下文
    bool DepthAnythingTrt::setTensorAddresses()
    {
        if (input_name.empty() || output_name.empty())
        {
            last_error = "Depth tensor names are not set.";
            return false;
        }
        if (!input_buffer || !output_buffer)
        {
            last_error = "Depth tensor buffers are not allocated.";
            return false;
        }
        if (!context->setTensorAddress(input_name.c_str(), input_buffer))
        {
            last_error = "Failed to set depth input tensor address.";
            return false;
        }
        if (!context->setTensorAddress(output_name.c_str(), output_buffer))
        {
            last_error = "Failed to set depth output tensor address.";
            return false;
        }
        return true;
    }
}

#endif
