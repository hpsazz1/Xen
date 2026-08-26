#ifndef CAPTURE_H
#define CAPTURE_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>

enum class CaptureBackend {
    DESKTOP_DUPLICATION,
    UDP_MJPEG,
    XUDP_JPEG,
    NDI,
};

enum class NetworkFrameLayout {
    // 解码帧与主机完整 FOV 逐像素一致。
    FULL_FRAME_1_TO_1,
    // 解码帧覆盖完整 FOV，但已由 OBS 等发送端缩放。
    FULL_FRAME_SCALED,
    // 解码帧是主机完整 FOV 的 1:1 像素中心裁剪。
    CENTER_CROP_1_TO_1,
};

// UDP 与 NDI 必须共享同一套网络画面语义；保留旧名称以兼容已有配置和测试。
using UdpFrameLayout = NetworkFrameLayout;

enum class CaptureStatus {
    CLOSED,
    READY,
    FRAME,
    NO_FRAME,
    ACCESS_LOST,
    INVALID_CONFIG,
    UNSUPPORTED,
    FAILURE,
};

// Capture 的像素载荷内存域。CPU_BGR 是所有网络后端和默认 DXGI 路径；
// 两种 D3D11_BGRA8 值只由对应的显式 GPU 互操作路径发布。
enum class CapturedFrameStorage {
    CPU_BGR,
    D3D11_BGRA8,
    D3D11_BGRA8_DIRECTML,
};

enum class SourceTimeBasis {
    UNAVAILABLE,
    // NDI SDK 在发送端收到该帧的提交时刻；不是相机曝光或桌面采集时刻。
    NDI_SDK_SUBMISSION,
};

enum class SourceClockStatus {
    UNSYNCHRONIZED,
    WARMING,
    VALID,
    STALE,
    INVALID,
};

inline const char* SourceTimeBasisName(SourceTimeBasis basis) noexcept {
    switch (basis) {
        case SourceTimeBasis::UNAVAILABLE: return "UNAVAILABLE";
        case SourceTimeBasis::NDI_SDK_SUBMISSION:
            return "NDI_SDK_SUBMISSION";
    }
    return "UNKNOWN";
}

inline const char* SourceClockStatusName(SourceClockStatus status) noexcept {
    switch (status) {
        case SourceClockStatus::UNSYNCHRONIZED: return "UNSYNCHRONIZED";
        case SourceClockStatus::WARMING: return "WARMING";
        case SourceClockStatus::VALID: return "VALID";
        case SourceClockStatus::STALE: return "STALE";
        case SourceClockStatus::INVALID: return "INVALID";
    }
    return "UNKNOWN";
}

const char* CaptureStatusName(CaptureStatus status) noexcept;
const char* CaptureBackendName(CaptureBackend backend) noexcept;
const char* UdpFrameLayoutName(UdpFrameLayout layout) noexcept;
const char* NetworkFrameLayoutName(NetworkFrameLayout layout) noexcept;

struct CaptureConfig {
    CaptureBackend backend = CaptureBackend::DESKTOP_DUPLICATION;
    int adapter_index = 0;
    int output_index = 0;
    // 默认关闭。开启后 Desktop Duplication 只发布三槽 D3D11 BGRA8 纹理，
    // 不执行逐帧 GPU→CPU readback；Runtime 会严格核对 TensorRT CUDA Graph。
    bool enable_d3d11_cuda_interop = false;
    // 默认关闭。开启后 Capture 发布可由 D3D12 打开的 shared NT handle 纹理，
    // 并用共享 fence 把 D3D11 copy 完成点交给严格 DirectML Session。
    bool enable_d3d11_directml_interop = false;
    // 裸 UDP 与 XUDP 共用 IPv4 监听地址；协议几何始终由 XUDP 帧头声明。
    std::string udp_url = "udp://0.0.0.0:5000";
    // recvfrom 单次阻塞上限，同时决定 close() 回收接收线程的最坏等待时间。
    int udp_read_timeout_ms = 250;
    int udp_disconnect_timeout_ms = 2000;
    UdpFrameLayout udp_frame_layout = UdpFrameLayout::FULL_FRAME_1_TO_1;
    // 主机完整游戏 FOV，不是辅机显示器或 OBS 输出分辨率。
    // FULL_FRAME_SCALED 与 CENTER_CROP_1_TO_1 必须成对显式配置。
    int udp_source_width = 0;
    int udp_source_height = 0;
    // NDI 源名称使用 UTF-8；Auto 表示先发现后按唯一源连接。
    std::string ndi_source_name = "Auto";
    int ndi_discovery_timeout_ms = 5000;
    int ndi_receive_timeout_ms = 50;
    int ndi_disconnect_timeout_ms = 2000;
    // NDI timestamp 使用源机 UTC；Windows Time 未锁定时必须通过该独立
    // 四时间戳旁路映射到辅机 steady_clock。空值保持 source age unknown。
    std::string ndi_clock_sync_url;
    int ndi_clock_sync_interval_ms = 250;
    int ndi_clock_sync_timeout_ms = 200;
    int ndi_clock_mapping_max_age_ms = 1000;
    // 仅由正式性能入口临时开启，不持久化到用户配置。开启后记录 NDI
    // 分段耗时，并低频查询 SDK 队列深度；正常运行保持关闭以避免诊断扰动。
    bool enable_performance_probes = false;
    NetworkFrameLayout ndi_frame_layout =
        NetworkFrameLayout::FULL_FRAME_1_TO_1;
    // NDI metadata 缺失或不符合 Xen 契约时使用的主机完整 FOV。
    int ndi_source_width = 0;
    int ndi_source_height = 0;
    // 开启后，NDI 帧必须携带合法 Xen metadata，否则会明确失败。
    bool ndi_require_frame_metadata = false;
    int roi_width = 320;
    int roi_height = 320;
    bool center_roi = true;
    int roi_x = 0;
    int roi_y = 0;
    int acquire_timeout_ms = 16;
};

// Capture 热路径的可选分段诊断。所有耗时单位均为毫秒；ndi_valid=false
// 表示本帧没有启用 NDI 探针，不能把默认零值解释为真实零耗时。
struct CaptureStageTiming {
    bool ndi_valid = false;
    bool runtime_handoff_valid = false;
    // NDIlib_recv_capture_v2 整次阻塞调用，包含等帧和 SDK 内部接收/解码，
    // 不能解释为纯网络或纯解码耗时。
    double receive_call_ms = 0.0;
    // metadata 仅统计当前视频帧携带的文本解析或最近有效 metadata 复制；
    // geometry 只统计坐标契约解析，不包含五槽池申请。
    double metadata_ms = 0.0;
    double geometry_ms = 0.0;
    double pool_acquire_ms = 0.0;
    // 仅包围 BGRA/BGRX 到 BGR 的 cv::cvtColor。
    double color_convert_ms = 0.0;
    // 两类 SDK 查询均以 sampled 区分“本帧未查询”和“真实零耗时”。
    bool performance_query_sampled = false;
    double performance_query_ms = 0.0;
    bool queue_depth_sampled = false;
    double queue_query_ms = 0.0;
    // 三项队列深度只在 queue_depth_sampled=true 时有效。
    int queued_video_frames = 0;
    int queued_audio_frames = 0;
    int queued_metadata_frames = 0;
    // pool_publish_ms 在 NetworkLatestFramePool 发布锁内固化；最后两项由
    // Runtime 后续交接阶段填写，Capture 不得提前标记为有效。
    double pool_publish_ms = 0.0;
    double runtime_capture_grab_ms = 0.0;
    double runtime_queue_publish_ms = 0.0;
};

struct FrameTiming {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    double capture_ms = 0.0;
    CaptureStageTiming capture_stages;
    // Capture 后端内部有界队列累计丢弃的源帧数，不包含 Runtime 队列覆盖。
    std::uint64_t source_dropped_frames = 0;
    // Desktop Duplication 在 D3D11 设备仍有效时成功重建的会话数。
    // 该数值是异常环境证据，不能与丢帧或失败帧混合。
    std::uint64_t duplication_recoveries = 0;
    // NDI SDK 报告或 XUDP 帧序号推导的传输丢帧数。
    std::uint64_t transport_dropped_frames = 0;
    // 协议解析、分片冲突、覆盖不完整或 SHA-256 失败的累计数量。
    std::uint64_t transport_invalid_packets = 0;
    // NDI/XUDP 报告的源端接收帧总数；没有该统计的后端保持为 0。
    std::uint64_t source_received_frames = 0;
    std::uint64_t source_sequence = 0;
    bool source_sequence_valid = false;
    double source_fps = 0.0;
    std::int64_t source_timecode = 0;
    bool source_timecode_valid = false;
    std::int64_t source_timestamp = 0;
    bool source_timestamp_valid = false;
    SourceTimeBasis source_time_basis = SourceTimeBasis::UNAVAILABLE;
    SourceClockStatus source_clock_status =
        SourceClockStatus::UNSYNCHRONIZED;
    // 映射后的源时刻与本地 steady_clock 同域。valid=false 时 time_point
    // 和所有质量标量均不得解释为真实零值。
    bool source_time_timing_valid = false;
    std::chrono::steady_clock::time_point source_time_at{};
    double source_clock_uncertainty_ms = 0.0;
    double source_clock_round_trip_ms = 0.0;
    double source_clock_rate = 1.0;
    double source_clock_mapping_age_ms = 0.0;
    std::uint64_t source_clock_sample_count = 0;
    std::uint64_t source_clock_session_id = 0;
};

struct CapturedFrame {
    // 网络等异步后端可用别名 shared_ptr 延长复用缓冲区寿命；bgr 始终是消费入口。
    // 本成员必须声明在 bgr 之前，使析构时先释放 Mat 视图再归还后端缓冲槽。
    std::shared_ptr<const cv::Mat> bgr_storage;
    cv::Mat bgr;
    // 公有契约不暴露 Windows 头。两种 D3D11_BGRA8 时 native_storage.get()
    // 是 ID3D11Texture2D*，shared_ptr 的自定义 deleter 负责 COM Release。
    std::shared_ptr<void> native_storage;
    // 同一 D3D11 immediate context 的 copy/Flush 与 CUDA map/unmap 不能
    // 跨线程交错提交；三个 GPU 帧槽共享同一个互操作提交锁。
    std::shared_ptr<std::mutex> native_synchronization;
    // DirectML 路径中 native_fence.get() 是可由 D3D12 OpenSharedHandle 打开的
    // fence HANDLE；自定义 deleter 负责 CloseHandle。CUDA 路径保持为空。
    std::shared_ptr<void> native_fence;
    // Capture 在同一 immediate context 的 copy 之后 Signal 的单调 fence 值。
    std::uint64_t native_fence_value = 0;
    CapturedFrameStorage storage = CapturedFrameStorage::CPU_BGR;
    // 像素载荷尺寸不能从 cv::Mat 推导：GPU-only 帧的 bgr 必须为空。
    int width = 0;
    int height = 0;
    FrameTiming timing;
    // bgr 左上角在主机完整 FOV 中的坐标；缩放完整帧时允许为小数。
    double roi_x = 0.0;
    double roi_y = 0.0;
    int source_width = 0;
    int source_height = 0;
    int encoded_width = 0;
    int encoded_height = 0;
    // 一个 bgr 像素对应的主机 FOV 像素数，用于控制量换算。
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
};

class ICapture {
public:
    virtual ~ICapture() = default;

    ICapture(const ICapture&) = delete;
    ICapture& operator=(const ICapture&) = delete;

    virtual bool open() noexcept = 0;
    // Runtime 启动线程可在业务线程创建前预备固定帧槽。普通 CPU/网络后端
    // 无需额外资源；D3D11 GPU 互操作后端借此预创建可注册/共享纹理。
    virtual bool prepare_frame(CapturedFrame& frame) noexcept {
        (void)frame;
        return true;
    }
    virtual CaptureStatus grab(CapturedFrame& frame) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual CaptureStatus status() const noexcept = 0;
    virtual std::string last_error() const = 0;

protected:
    ICapture() = default;
};

std::unique_ptr<ICapture> create_capture(const CaptureConfig& config) noexcept;

#endif // CAPTURE_H
