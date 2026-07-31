#ifndef DETECTOR_H
#define DETECTOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
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
// 单帧执行状态
// ============================================================
enum class DetectionStatus {
    NOT_RUN,             ///< 尚未执行 detect()
    SUCCESS,             ///< pipeline 完整执行；检测结果允许为空
    NOT_LOADED,          ///< 模型尚未加载
    INVALID_INPUT,       ///< 输入不是非空 CV_8UC3 图像
    PREPROCESS_FAILED,   ///< LetterBox 或张量填充失败
    INFERENCE_FAILED,    ///< ORT 执行或设备复制失败
    INVALID_OUTPUT,      ///< 模型输出类型、shape 或契约不受支持
    POSTPROCESS_FAILED,  ///< NMS、top_k 或坐标还原失败
};

inline const char* DetectionStatusName(DetectionStatus status) noexcept {
    switch (status) {
        case DetectionStatus::NOT_RUN: return "NOT_RUN";
        case DetectionStatus::SUCCESS: return "SUCCESS";
        case DetectionStatus::NOT_LOADED: return "NOT_LOADED";
        case DetectionStatus::INVALID_INPUT: return "INVALID_INPUT";
        case DetectionStatus::PREPROCESS_FAILED: return "PREPROCESS_FAILED";
        case DetectionStatus::INFERENCE_FAILED: return "INFERENCE_FAILED";
        case DetectionStatus::INVALID_OUTPUT: return "INVALID_OUTPUT";
        case DetectionStatus::POSTPROCESS_FAILED: return "POSTPROCESS_FAILED";
    }
    return "UNKNOWN";
}

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
    // 固定 shape 实时推理默认使用 CUDA Graph 降低 kernel launch 开销。
    // 动态 shape 或需要排查图捕获问题时可显式关闭。
    bool         enable_trt_cuda_graph = true;
    // 仅在 TensorRT CUDA Graph 固定设备 I/O 路径生效。OpenCV 仍负责 resize 与
    // LetterBox 几何，CUDA kernel 负责 BGR→RGB、uint8→float 和 HWC→CHW。
    bool         enable_gpu_preprocess = true;
    // 仅用于回归诊断：计算原始输出字节指纹会额外遍历整个输出张量，性能
    // 基准和正式运行必须保持关闭。
    bool         enable_output_fingerprint = false;
    // TensorRT 首次构建后在此保存 engine/profile/timing 文件。模型、ORT、
    // TensorRT 版本或精度配置变化时必须清理旧缓存。
    std::string  trt_cache_path = "cache/tensorrt";
    int          intra_threads    = 0; ///< 0 = ORT 默认；GPU 单实例建议实测 1
    // 为保持已有配置源码兼容而保留；当前 Session 始终顺序执行，此值被忽略。
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
    // TensorRT CUDA Graph 使用显式设备复制时可进一步拆分；其他后端的
    // execution_ms 包含 ORT 内部可能发生的隐式复制，h2d/d2h 保持为 0。
    double h2d_ms         = 0;
    double gpu_preprocess_ms = 0;
    double execution_ms   = 0;
    double d2h_ms         = 0;
    bool   explicit_device_copy = false;
    bool   gpu_preprocess = false;
    std::uint64_t input_upload_bytes = 0;
    std::uint64_t output_fingerprint = 0;
    DetectionStatus status = DetectionStatus::NOT_RUN;
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

    /// 创建完全独立的副本。每个 clone 都会新建 ORT Session、线程池、
    /// TensorRT 执行上下文和设备缓冲；默认实时链路应优先使用单推理线程单实例，
    /// 仅独立视频流确需并行推理时才为各线程创建 clone。
    std::unique_ptr<Detector> clone() const;

    /// 最近一次推理的线程安全快照；可与 detect() 并发读取
    InferenceProfile profile() const noexcept;

    /// 返回已注册的最高优先级 Provider；CUDA/TensorRT 允许节点级后备
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
};

#endif // DETECTOR_H
