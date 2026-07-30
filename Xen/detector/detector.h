#ifndef DETECTOR_H
#define DETECTOR_H

#include <string>
#include <vector>
#include <memory>
#include <opencv2/core.hpp>

// ============================================================
// 推理后端
// ============================================================
enum class BackendType {
    CUDA,       ///< ONNX Runtime CUDA EP
    TENSORRT,   ///< ONNX Runtime TensorRT EP
    DIRECTML,   ///< ONNX Runtime DirectML EP（AMD/Intel/NVIDIA 通用）
    CPU,        ///< CPU 兜底
};

/// 后端可读名称
const char* BackendName(BackendType bt) noexcept;

// ============================================================
// 模型输出契约
// ============================================================
// 不按 YOLO 版本号分支，而按 ONNX 实际输出布局解码。只要后续模型继续使用
// 其中一种稳定契约，就无需修改 Detector 主流程。
enum class OutputFormat {
    AUTO,                         ///< 根据 metadata 与张量形状严格识别
    CHANNEL_FIRST,                ///< [B, 4+C, A]，cxcywh + 类别概率
    ANCHOR_FIRST_OBJECTNESS,      ///< [B, A, 5+C]，cxcywh + obj + 类别概率
    END_TO_END,                   ///< [B, N, 6]，xyxy + score + class，无需 NMS
};

// ============================================================
// 检测结果
// ============================================================
struct Detection {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0; ///< 原始图像像素坐标
    float confidence = 0;
    int   class_id   = 0;
};

// ============================================================
// 模型配置
// ============================================================
struct DetectorConfig {
    // ── 模型与后端 ──
    std::string  model_path;           ///< ONNX 模型文件路径
    BackendType  backend = BackendType::CUDA;
    int          device_id = 0;        ///< GPU 设备索引

    // ── 输入尺寸（Detector 会根据 ONNX 输入自动读取，通常无需手动设） ──
    int          input_width  = 0;     ///< 0 = 自动从模型读取
    int          input_height = 0;

    // ── 检测阈值 ──
    float        conf_threshold  = 0.25f;
    float        nms_threshold   = 0.45f;
    int          top_k           = 300;

    // ── 输出契约 ──
    // AUTO 适用于带标准输出或 Ultralytics metadata 的模型；第三方导出结果
    // 存在歧义时必须显式指定，Detector 不会猜测未知布局。
    OutputFormat output_format = OutputFormat::AUTO;

    // ── 优化选项 ──
    bool         enable_graph_opt = true;
    bool         enable_fp16      = false;
    bool         enable_trt_engine_cache = true;
    bool         enable_trt_timing_cache = true;
    // TensorRT 首次构建后在此保存 engine/profile/timing 文件。模型、ORT、
    // TensorRT 版本或精度配置变化时必须清理旧缓存。
    std::string  trt_cache_path = "cache/tensorrt";
    int          intra_threads    = 0; ///< 0 = 默认
    int          inter_threads    = 0;
};

// ============================================================
// 单次推理耗时统计
// ============================================================
struct InferenceProfile {
    double preprocess_ms  = 0;
    double inference_ms   = 0;
    double postprocess_ms = 0;
    double total_ms       = 0;
};

// ============================================================
// 检测器主类
// ============================================================
class Detector {
public:
    explicit Detector(const DetectorConfig& cfg);
    ~Detector();

    // 禁止拷贝，允许移动
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;
    Detector(Detector&&) = default;
    Detector& operator=(Detector&&) = default;

    /// 加载模型（应在构造后调用一次，构造+load 更灵活）
    bool load();

    /// 是否已加载
    bool loaded() const noexcept;

    /// 释放模型资源
    void reset();

    /// 在单张 BGR 图像上运行检测；同一实例不可并发调用，多线程请使用 clone()
    std::vector<Detection> detect(const cv::Mat& bgr_image);

    /// 批量检测
    std::vector<std::vector<Detection>> detect_batch(
        const std::vector<cv::Mat>& bgr_images);

    /// 创建一个独立副本（多线程安全：每个线程持有自己的 clone）
    std::unique_ptr<Detector> clone() const;

    /// 最近一次推理的耗时统计
    const InferenceProfile& profile() const noexcept;

    /// 返回当前后端名称
    std::string backend_name() const;

    /// 获取模型实际输入尺寸（load() 后有效）
    int input_width()  const noexcept;
    int input_height() const noexcept;

    /// 列出当前 ONNX Runtime 支持的所有 Execution Provider
    static std::vector<std::string> available_providers();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    DetectorConfig       config_;
    InferenceProfile     profile_;
};

#endif // DETECTOR_H
