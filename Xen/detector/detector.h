#ifndef DETECTOR_H
#define DETECTOR_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
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
    OPENVINO,   ///< ONNX Runtime OpenVINO EP（Intel CPU/GPU/NPU）
    CPU,        ///< CPU 兜底
};

/// 后端可读名称
const char* BackendName(BackendType bt) noexcept;

enum class OpenVinoDevice {
    GPU,        ///< Intel 集成或独立 GPU；device_id>0 时选择 GPU.<id>
    CPU,        ///< OpenVINO CPU plugin；device_id 必须为 0
    NPU,        ///< Intel NPU；device_id 必须为 0
};

/// OpenVINO device_type 的稳定基础名称。
const char* OpenVinoDeviceName(OpenVinoDevice device) noexcept;

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
    UNSUPPORTED_INPUT,   ///< 输入本身有效，但当前 Session 不支持其内存域/格式
    UNSUPPORTED_TASK,    ///< 调用的任务入口与已加载模型任务不一致
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
        case DetectionStatus::UNSUPPORTED_INPUT: return "UNSUPPORTED_INPUT";
        case DetectionStatus::UNSUPPORTED_TASK: return "UNSUPPORTED_TASK";
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
// 实例分割结果
// ============================================================
// 掩码不嵌入 Detection，避免 Aim、Runtime 预览和队列复制检测框时连带复制像素。
// 同一帧的所有掩码紧凑存放在 SegmentationResult::mask_pixels 中；描述符只保存
// 原图 ROI 和偏移。复制 SegmentationResult 时像素缓冲通过 shared_ptr 共享，移动
// 结果则不复制像素。
struct InstanceMask {
    int x = 0;                         ///< ROI 左上角，原图像素坐标
    int y = 0;
    int width = 0;
    int height = 0;
    std::size_t data_offset = 0;       ///< mask_pixels 中首字节偏移
    std::size_t row_stride = 0;        ///< 每行字节数，当前等于 width
};

struct SegmentationResult {
    // detections[index] 与 masks[index] 始终属于同一实例；即使阈值后二值掩码
    // 全零也保留该索引，确保 detect() 与 segment() 的框结果一致。
    std::vector<Detection> detections;
    std::vector<InstanceMask> masks;
    // 像素值只使用 0/1。无实例或调用 detect() 只取框时允许为空。
    std::shared_ptr<const std::vector<std::uint8_t>> mask_pixels;

    /// 返回指定掩码行首地址；索引、行号或缓冲契约非法时返回 nullptr。
    const std::uint8_t* mask_row(std::size_t mask_index,
                                 int row) const noexcept {
        if (!mask_pixels || mask_index >= masks.size()) return nullptr;
        const InstanceMask& mask = masks[mask_index];
        if (row < 0 || row >= mask.height || mask.width <= 0 ||
            mask.row_stride < static_cast<std::size_t>(mask.width)) {
            return nullptr;
        }
        const std::size_t row_index = static_cast<std::size_t>(row);
        if (row_index >
            (std::numeric_limits<std::size_t>::max() - mask.data_offset) /
                mask.row_stride) {
            return nullptr;
        }
        const std::size_t offset =
            mask.data_offset + row_index * mask.row_stride;
        if (offset > mask_pixels->size() ||
            static_cast<std::size_t>(mask.width) >
                mask_pixels->size() - offset) {
            return nullptr;
        }
        return mask_pixels->data() + offset;
    }
};

// ============================================================
// 姿态估计结果
// ============================================================
// 关键点坐标使用原图像素坐标；confidence 是模型给出的可见性/置信度。
// 二维关键点模型没有第三维时统一写为 1.0。关键点按检测实例连续存放，
// detections[index] 对应 keypoints 中第 index 行。
struct PoseKeypoint {
    float x = 0.0f;
    float y = 0.0f;
    float confidence = 1.0f;
};

struct PoseResult {
    std::vector<Detection> detections;
    std::vector<PoseKeypoint> keypoints;
    std::size_t keypoints_per_detection = 0;
    int keypoint_dimensions = 0; ///< 原模型维度，只允许 2 或 3

    /// 返回指定实例的首个关键点；索引或连续布局契约非法时返回 nullptr。
    const PoseKeypoint* keypoint_row(
            std::size_t detection_index) const noexcept {
        if (keypoints_per_detection == 0 ||
            detection_index >= detections.size() ||
            detection_index >
                std::numeric_limits<std::size_t>::max() /
                    keypoints_per_detection) {
            return nullptr;
        }
        const std::size_t offset =
            detection_index * keypoints_per_detection;
        if (offset > keypoints.size() ||
            keypoints_per_detection > keypoints.size() - offset) {
            return nullptr;
        }
        return keypoints.data() + offset;
    }

    /// 返回单个关键点；任一索引越界时返回 nullptr。
    const PoseKeypoint* keypoint(
            std::size_t detection_index,
            std::size_t keypoint_index) const noexcept {
        if (keypoint_index >= keypoints_per_detection) return nullptr;
        const PoseKeypoint* row = keypoint_row(detection_index);
        return row ? row + keypoint_index : nullptr;
    }
};

// ============================================================
// 旋转目标检测结果
// ============================================================
// OBB 使用原图坐标系的 xywhr：中心、宽高和弧度角。角度保持模型原始
// 表示，不强制转换为 [0,pi/2)；宽高也不因角度归一化而互换。
struct OrientedDetection {
    float center_x = 0.0f;
    float center_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float angle_radians = 0.0f;
    float confidence = 0.0f;
    int class_id = 0;
};

struct ObbResult {
    // detections 是每个旋转框四角的原图轴对齐包围盒，供现有 Aim 等
    // 轻量消费者复用；与 oriented_detections 始终一一对应。
    std::vector<Detection> detections;
    std::vector<OrientedDetection> oriented_detections;
};

// D3D11 纹理输入的无 Windows 头公有描述符。resource.get() 必须指向
// ID3D11Texture2D，格式为 DXGI_FORMAT_B8G8R8A8_UNORM；共享所有权保证
// GPU 消费完成前纹理不会被释放。synchronization 必须非空，且 Capture 的
// D3D copy/Flush 与 Detector 的跨 API 提交必须持有同一把锁。DirectML 路径
// 还要求 shared_fence.get() 是 HANDLE，fence_value 是该帧 copy 后的 Signal 值。
struct D3D11TextureFrame {
    std::shared_ptr<void> resource;
    std::shared_ptr<std::mutex> synchronization;
    std::shared_ptr<void> shared_fence;
    std::uint64_t fence_value = 0;
    int width = 0;
    int height = 0;
};

// ============================================================
// 模型配置
// ============================================================
struct DetectorConfig {
    // ── 模型与后端 ──
    std::string  model_path;           ///< ONNX 模型文件路径
    BackendType  backend = BackendType::CUDA;
    int          device_id = 0;        ///< GPU 设备索引；OpenVINO CPU/NPU 仅允许 0
    OpenVinoDevice openvino_device = OpenVinoDevice::GPU;

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
    // 仅用于独立 Provider 诊断 Session。性能 Session 必须保持关闭，前缀不
    // 写入常规 config.ini，避免生产运行意外产生大体积 ORT trace。
    bool         enable_ort_profiling = false;
    std::string  ort_profile_prefix;
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
    // D3D11 map、跨 API 同步和 BGRA array→CUDA 线性缓冲的设备内复制。
    // 互操作路径没有 H2D，h2d_ms 与 input_upload_bytes 必须保持为 0。
    double d3d11_to_cuda_ms = 0;
    // D3D12 queue 等待 D3D11 fence 并直接执行 BGRA8→CHW shader 的 GPU 时间。
    // 该路径不经过主机上传或中间设备复制。
    double d3d11_to_directml_ms = 0;
    double gpu_preprocess_ms = 0;
    double execution_ms   = 0;
    double d2h_ms         = 0;
    bool   explicit_device_copy = false;
    bool   gpu_preprocess = false;
    bool   d3d11_cuda_interop = false;
    bool   d3d11_directml_interop = false;
    std::uint64_t input_upload_bytes = 0;
    std::uint64_t input_device_copy_bytes = 0;
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

    /// 在单张 BGR 图像上运行检测；segment/pose/OBB 模型仅返回轴对齐框，
    /// 不生成掩码、关键点或公有旋转框数组，可直接服务 Aim 热路径。
    /// 同一实例不可并发调用，多线程请使用 clone()。
    std::vector<Detection> detect(const cv::Mat& bgr_image);

    /// 执行 YOLOv8 兼容的实例分割；仅支持标准双输出 raw head：
    /// [1,4+C+M,A] 检测/系数张量与 [1,M,H,W] 原型张量。
    SegmentationResult segment(const cv::Mat& bgr_image);

    /// 从 D3D11 BGRA8 纹理执行推理；仅 TensorRT CUDA Graph 或严格 DirectML
    /// 固定 shape 支持。
    /// 该入口不静默回退 CPU，资源、格式、尺寸或设备不符均返回明确失败状态。
    std::vector<Detection> detect_d3d11(
        const D3D11TextureFrame& frame);

    /// 从 D3D11 BGRA8 纹理执行实例分割；输入互操作约束与 detect_d3d11() 相同。
    SegmentationResult segment_d3d11(
        const D3D11TextureFrame& frame);

    /// 当前已加载模型是否满足受支持的实例分割输出契约。
    bool segmentation_supported() const noexcept;

    /// 执行 YOLOv8 兼容姿态估计；仅支持 metadata 明确声明 task=pose、
    /// kpt_shape=[K,2|3] 的 channel-first raw head。
    PoseResult pose(const cv::Mat& bgr_image);

    /// 从 D3D11 BGRA8 纹理执行姿态估计；输入约束与 detect_d3d11() 相同。
    PoseResult pose_d3d11(const D3D11TextureFrame& frame);

    /// 当前已加载模型是否满足受支持的姿态输出契约。
    bool pose_supported() const noexcept;

    /// 执行 YOLOv8 兼容旋转目标检测；返回旋转框及其轴对齐包围盒。
    ObbResult obb(const cv::Mat& bgr_image);

    /// 从 D3D11 BGRA8 纹理执行旋转目标检测。
    ObbResult obb_d3d11(const D3D11TextureFrame& frame);

    /// 当前已加载模型是否满足受支持的 OBB 输出契约。
    bool obb_supported() const noexcept;

    /// 当前已加载 Session 是否具备对应 Provider 的 D3D11 GPU 互操作能力。
    bool d3d11_interop_supported() const noexcept;

    /// 在任何 D3D11 copy 前预注册固定纹理；成功后该资源可跨帧复用。
    bool prepare_d3d11(const D3D11TextureFrame& frame) noexcept;

    /// 批量检测
    std::vector<std::vector<Detection>> detect_batch(
        const std::vector<cv::Mat>& bgr_images);

    /// 创建完全独立的副本。每个 clone 都会新建 ORT Session、线程池、
    /// TensorRT 执行上下文和设备缓冲；默认实时链路应优先使用单推理线程单实例，
    /// 仅独立视频流确需并行推理时才为各线程创建 clone。
    std::unique_ptr<Detector> clone() const;

    /// 最近一次推理的线程安全快照；可与 detect() 并发读取
    InferenceProfile profile() const noexcept;

    // 显式结束 ORT profiling 并返回实际生成的 JSON 路径。调用后不得继续
    // 使用当前 Session 推理；未启用 profiling、未加载或重复调用均返回 false。
    bool end_profiling(std::string& profile_path) noexcept;

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
