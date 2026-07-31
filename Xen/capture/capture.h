#ifndef CAPTURE_H
#define CAPTURE_H

#include <chrono>
#include <cstdint>
#include <memory>
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

const char* CaptureStatusName(CaptureStatus status) noexcept;
const char* CaptureBackendName(CaptureBackend backend) noexcept;
const char* UdpFrameLayoutName(UdpFrameLayout layout) noexcept;
const char* NetworkFrameLayoutName(NetworkFrameLayout layout) noexcept;

struct CaptureConfig {
    CaptureBackend backend = CaptureBackend::DESKTOP_DUPLICATION;
    int adapter_index = 0;
    int output_index = 0;
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

struct FrameTiming {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    double capture_ms = 0.0;
    // Capture 后端内部有界队列累计丢弃的源帧数，不包含 Runtime 队列覆盖。
    std::uint64_t source_dropped_frames = 0;
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
};

struct CapturedFrame {
    // 网络等异步后端可用别名 shared_ptr 延长复用缓冲区寿命；bgr 始终是消费入口。
    // 本成员必须声明在 bgr 之前，使析构时先释放 Mat 视图再归还后端缓冲槽。
    std::shared_ptr<const cv::Mat> bgr_storage;
    cv::Mat bgr;
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
    virtual CaptureStatus grab(CapturedFrame& frame) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual CaptureStatus status() const noexcept = 0;
    virtual std::string last_error() const = 0;

protected:
    ICapture() = default;
};

std::unique_ptr<ICapture> create_capture(const CaptureConfig& config) noexcept;

#endif // CAPTURE_H
