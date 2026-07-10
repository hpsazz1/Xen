#pragma once
#ifdef USE_CUDA

// Depth Anything 深度估计模型的 TensorRT 推理封装
// 使用 TensorRT 加速运行 Depth Anything 模型，从单张图像生成深度图

#include <NvInfer.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <memory>

#include "depth_utils.h"

namespace depth_anything
{
    class DepthAnythingTrt
    {
    public:
        DepthAnythingTrt();
        ~DepthAnythingTrt();

        // 初始化：加载 TensorRT 引擎
        bool initialize(const std::string& modelPath, nvinfer1::ILogger& logger);
        // 从 ONNX 导出 TensorRT 引擎
        bool exportEngine(const std::string& modelPath, nvinfer1::ILogger& logger, std::string* enginePath = nullptr);
        // 预测深度图（带可选上色）
        cv::Mat predict(const cv::Mat& image);
        // 预测原始深度图（无上色）
        cv::Mat predictDepth(const cv::Mat& image);
        // 设置深度色图类型
        void setColormap(int type);
        int colormapType() const;
        // 模型是否就绪
        bool ready() const;
        const std::string& lastError() const;
        // 重置模型状态
        void reset();

    private:
        // 模型输入参数
        int input_w;           // 输入宽度
        int input_h;           // 输入高度
        int min_input_size;    // 最小输入尺寸
        int max_input_size;    // 最大输入尺寸
        bool dynamic_input;    // 是否支持动态输入尺寸
        float mean[3];         // 归一化均值
        float stddev[3];       // 归一化标准差
        int colormap_type;     // 色图类型

        // TensorRT 资源
        std::unique_ptr<nvinfer1::IRuntime> runtime;
        std::unique_ptr<nvinfer1::ICudaEngine> engine;
        std::unique_ptr<nvinfer1::IExecutionContext> context;

        // 输入输出绑定
        std::string input_name;
        std::string output_name;
        void* input_buffer;
        void* output_buffer;
        size_t input_capacity;
        size_t output_capacity;
        int output_w;                     // 输出宽度
        int output_h;                     // 输出高度
        std::vector<float> depth_data;    // 深度数据缓存
        cudaStream_t stream;              // CUDA 流

        bool initialized;                 // 初始化标志
        std::string last_error;           // 最后错误信息

        // 预处理：将 OpenCV 图像转换为模型输入张量
        bool preprocess(const cv::Mat& image, std::vector<float>& input_tensor);
        // 获取输出张量形状
        bool getOutputShape(int& out_h, int& out_w, size_t& out_elements);
        bool ensureInputCapacity(size_t elements);
        bool ensureOutputCapacity(size_t elements);
        // 设置 TensorRT 张量地址
        bool setTensorAddresses();
        // 根据图像尺寸选择最佳输入尺寸
        int selectInputSize(const cv::Mat& image) const;
        // 设置动态输入形状
        bool setInputShape(int w, int h);
        // 运行推理并返回归一化深度图
        bool runInference(const cv::Mat& image, cv::Mat& depth_norm);
        // 从文件加载序列化引擎
        bool loadEngine(const std::string& modelPath, nvinfer1::ILogger& logger);
        // 从 ONNX 构建引擎
        bool buildEngine(const std::string& onnxPath, nvinfer1::ILogger& logger);
        // 保存引擎到文件
        bool saveEngine(const std::string& onnxPath, std::string* enginePath = nullptr);
    };
}

#endif
