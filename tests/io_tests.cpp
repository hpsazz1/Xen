#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "capture/udp_internal.h"
#include "capture/network_internal.h"
#include "keyboard/keyboard.h"
#include "log/log.h"
#include "mouse/mouse.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#if XEN_HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void test_mouse_disabled_by_default() {
    MouseConfig config;
    auto mouse = MouseDeviceFactory::create(config);
    expect(mouse && mouse->open(), "Win32 Mouse 后端应可初始化");
    expect(mouse && mouse->status() == MouseStatus::DISABLED,
           "物理输出默认必须为 DISABLED");
    expect(mouse && !mouse->move({1, 1}),
           "未显式允许 SendInput 时不得提交命令");
}

void test_invalid_keyboard_config() {
    KeyboardConfig config;
    config.aim_hold_virtual_key = 0;
    KeyboardListener keyboard(config);
    expect(!keyboard.open() && keyboard.status() == KeyboardStatus::FAILURE,
           "非法虚拟键配置必须失败关闭");
}

void test_invalid_capture_config() {
    CaptureConfig config;
    config.roi_width = 0;
    auto capture = create_capture(config);
    expect(capture && !capture->open(),
           "非法 ROI 必须在访问 DXGI 设备前被拒绝");
    expect(capture && capture->status() == CaptureStatus::INVALID_CONFIG,
           "非法 Capture 配置应返回明确状态");
}

void fill_udp_slot(
        const std::shared_ptr<capture::detail::UdpDecodedFrame>& slot,
        std::uint64_t sequence,
        unsigned char marker) {
    expect(slot != nullptr, "UDP 解码池应提供可写槽");
    if (!slot) return;
    slot->bgr.create(2, 2, CV_8UC3);
    slot->bgr.setTo(cv::Scalar(marker, marker, marker));
    slot->timing.sequence = sequence;
    slot->timing.captured_at = std::chrono::steady_clock::now();
    slot->source_width = 2;
    slot->source_height = 2;
}

void test_udp_latest_frame_pool() {
    capture::detail::UdpLatestFramePool pool;
    auto first = pool.acquire_write();
    fill_udp_slot(first, 1, 10);
    pool.publish(first);
    auto second = pool.acquire_write();
    fill_udp_slot(second, 2, 20);
    pool.publish(second);

    CapturedFrame output;
    expect(pool.take_latest(0, output) && output.timing.sequence == 2,
           "UDP 队列必须跳过未消费旧帧并返回最新帧");
    expect(output.timing.source_dropped_frames == 1,
           "覆盖未消费 UDP 帧时必须累计源端丢帧");
    expect(output.bgr_storage &&
           output.bgr.at<cv::Vec3b>(0, 0)[0] == 20,
           "UDP 输出必须通过 storage 所有权保持解码槽有效");
    expect(!pool.take_latest(2, output),
           "同一 UDP 帧不得被重复交付");

    first.reset();
    second.reset();
    auto third = pool.acquire_write();
    fill_udp_slot(third, 3, 30);
    pool.publish(third);
    expect(output.bgr.at<cv::Vec3b>(0, 0)[0] == 20,
           "发布新帧不得覆写消费者仍持有的 UDP 缓冲");
}

void test_udp_pool_is_bounded() {
    capture::detail::UdpLatestFramePool pool;
    std::array<std::shared_ptr<capture::detail::UdpDecodedFrame>,
               capture::detail::UdpLatestFramePool::kSlotCount> writers;
    for (auto& writer : writers) writer = pool.acquire_write();
    expect(!pool.acquire_write(),
           "UDP 解码池满载时必须拒绝写槽而不是扩容");
    pool.record_drop();
    expect(pool.dropped_frames() == 1,
           "UDP 解码池满载丢弃必须可观测");
    writers.front().reset();
    expect(pool.acquire_write() != nullptr,
           "释放 UDP 槽后必须能够继续接收新帧");
}

void test_udp_frame_geometry() {
    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.roi_width = 320;
    config.roi_height = 320;

    capture::detail::UdpFrameGeometry geometry;
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 2560;
    config.udp_source_height = 1440;
    expect(capture::detail::resolve_udp_frame_geometry(
               config, 320, 320, geometry),
           "主机中心 1:1 预裁剪几何必须可解析");
    expect(geometry.source_width == 2560 &&
               geometry.source_height == 1440 &&
               geometry.source_roi_x == 1120.0 &&
               geometry.source_roi_y == 560.0 &&
               geometry.decoded_roi_width == 320 &&
               geometry.decoded_roi_height == 320 &&
               geometry.source_pixels_per_pixel_x == 1.0 &&
               geometry.source_pixels_per_pixel_y == 1.0,
           "2560x1440 主机中心 320x320 ROI 必须映射到 (1120,560)");

    config.udp_frame_layout = UdpFrameLayout::FULL_FRAME_SCALED;
    expect(capture::detail::resolve_udp_frame_geometry(
               config, 640, 360, geometry),
           "缩放完整帧几何必须可解析");
    expect(geometry.source_roi_x == 640.0 &&
               geometry.source_roi_y == 80.0 &&
               geometry.source_pixels_per_pixel_x == 4.0 &&
               geometry.source_pixels_per_pixel_y == 4.0,
           "640x360 完整帧中心 320x320 ROI 必须按主机像素比例映射");

    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 300;
    expect(!capture::detail::resolve_udp_frame_geometry(
               config, 320, 320, geometry),
           "完整 FOV 小于 OBS 预裁剪帧时必须安全失败");
}

void test_xen_metadata_geometry() {
    capture::detail::XenFrameMetadata metadata;
    expect(capture::detail::parse_xen_frame_metadata(
               "<xen version=\"1\" source_width=\"2560\" "
               "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
               "roi_width=\"320\" roi_height=\"320\"/>", metadata),
           "NDI Xen metadata 必须能被严格解析");
    expect(metadata.source_width == 2560 && metadata.source_height == 1440 &&
               metadata.roi_x == 1120 && metadata.roi_y == 560 &&
               metadata.roi_width == 320 && metadata.roi_height == 320,
           "NDI metadata 的主机 FOV 与 ROI 坐标必须保持原值");
    capture::detail::NetworkGeometryConfig config;
    config.layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.roi_width = 160;
    config.roi_height = 160;
    capture::detail::NetworkFrameGeometry geometry;
    expect(!capture::detail::resolve_network_frame_geometry(
               config, 320, 320, geometry, &metadata),
           "metadata ROI 与本地 Detector ROI 尺寸不一致时必须拒绝");
    expect(!capture::detail::parse_xen_frame_metadata(
               "<xen version=\"1\" source_width=\"2560\" "
               "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
               "roi_width=\"320\" roi_height=\"320\" extra=\"1\"/>",
               metadata),
           "NDI metadata 出现未知属性时必须拒绝而不是猜测");
}

void test_invalid_udp_capture_config() {
    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.udp_url.clear();
    auto capture = create_capture(config);
    expect(capture && !capture->open(),
           "空 UDP 地址必须在访问网络前被拒绝");
    expect(capture && capture->status() == CaptureStatus::INVALID_CONFIG,
           "非法 UDP Capture 配置应返回明确状态");
    expect(std::string(CaptureBackendName(config.backend)) == "UDP_MJPEG",
           "UDP Capture 后端名称必须稳定可观测");

    config.udp_url = "udp://127.0.0.1:5000";
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    auto missing_source = create_capture(config);
    expect(missing_source && !missing_source->open() &&
               missing_source->status() == CaptureStatus::INVALID_CONFIG,
           "中心预裁剪模式缺少主机完整 FOV 时必须在监听前失败");
}

#if XEN_HAS_NDI

bool wait_for_capture_frame(ICapture& capture,
                            std::uint64_t after_sequence,
                            CapturedFrame& frame);

void send_ndi_test_frame(NDIlib_send_instance_t sender,
                         cv::Scalar color,
                         const std::string& metadata) {
    cv::Mat frame(320, 320, CV_8UC4,
                  cv::Scalar(color[0], color[1], color[2], 255.0));
    NDIlib_video_frame_v2_t video{};
    video.xres = frame.cols;
    video.yres = frame.rows;
    video.FourCC = NDIlib_FourCC_type_BGRX;
    video.frame_rate_N = 60;
    video.frame_rate_D = 1;
    video.frame_format_type = NDIlib_frame_format_type_progressive;
    video.timecode = NDIlib_send_timecode_synthesize;
    video.p_data = frame.data;
    video.line_stride_in_bytes = static_cast<int>(frame.step);
    video.p_metadata = metadata.c_str();
    NDIlib_send_send_video_v2(sender, &video);
}

void test_ndi_loopback() {
    CaptureConfig config;
    config.backend = CaptureBackend::NDI;
    config.ndi_source_name = "Auto";
    config.ndi_discovery_timeout_ms = 500;
    config.ndi_receive_timeout_ms = 50;
    config.ndi_disconnect_timeout_ms = 500;
    config.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.ndi_require_frame_metadata = true;
    config.roi_width = 320;
    config.roi_height = 320;
    config.acquire_timeout_ms = 20;

    auto capture = create_capture(config);
    expect(capture && capture->open(),
           "NDI Capture 必须能在真实 SDK 下打开");
    if (!capture || capture->status() == CaptureStatus::UNSUPPORTED) return;

    NDIlib_send_create_t sender_settings{};
    sender_settings.p_ndi_name = "Xen NDI Loopback Sender";
    sender_settings.clock_video = false;
    sender_settings.clock_audio = false;
    NDIlib_send_instance_t sender = NDIlib_send_create(&sender_settings);
    expect(sender != nullptr, "NDI 回环测试发送端必须创建成功");
    if (!sender) {
        capture->close();
        return;
    }
    const std::string metadata =
        "<xen version=\"1\" source_width=\"2560\" "
        "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
        "roi_width=\"320\" roi_height=\"320\"/>";
    for (int index = 0; index < 20; ++index) {
        send_ndi_test_frame(
            sender, cv::Scalar(24.0, 96.0, 208.0), metadata);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CapturedFrame frame;
    const bool received_first = wait_for_capture_frame(*capture, 0, frame);
    expect(received_first && frame.bgr.cols == 320 &&
               frame.bgr.rows == 320 && frame.bgr.type() == CV_8UC3,
           "NDI BGRX 回环必须输出 320x320 BGR ROI");
    expect(received_first && frame.source_width == 2560 &&
               frame.source_height == 1440 && frame.roi_x == 1120.0 &&
               frame.roi_y == 560.0 &&
               frame.source_pixels_per_pixel_x == 1.0 &&
               frame.source_pixels_per_pixel_y == 1.0,
           "NDI metadata 必须将辅机接收帧映射到主机 (1120,560)");
    expect(received_first && frame.timing.source_fps == 60.0 &&
               frame.timing.source_received_frames > 0,
           "NDI 帧必须携带 SDK 源帧率与接收统计");
    const cv::Scalar first_mean = received_first
        ? cv::mean(frame.bgr) : cv::Scalar{};

    for (int index = 0; index < 20; ++index) {
        send_ndi_test_frame(
            sender, cv::Scalar(208.0, 48.0, 20.0), metadata);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool received_second = wait_for_capture_frame(
        *capture, frame.timing.sequence, frame);
    const cv::Scalar second_mean = received_second
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_second &&
               std::abs(first_mean[0] - second_mean[0]) > 100.0,
           "NDI 连续变化帧不得重放首次输入");

    NDIlib_send_destroy(sender);
    bool access_lost = false;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < disconnect_deadline) {
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::ACCESS_LOST) {
            access_lost = true;
            break;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            break;
        }
    }
    expect(access_lost,
           "NDI 源断开超过阈值后必须进入 ACCESS_LOST");
    capture->close();
}

#else

void test_ndi_unsupported_without_sdk() {
    CaptureConfig config;
    config.backend = CaptureBackend::NDI;
    auto capture = create_capture(config);
    expect(capture && !capture->open() &&
               capture->status() == CaptureStatus::UNSUPPORTED,
           "未安装 NDI SDK 时选择 NDI 必须明确返回 UNSUPPORTED");
}

#endif

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockSession() {
        if (ready_) WSACleanup();
    }

    bool ready() const noexcept { return ready_; }

private:
    bool ready_ = false;
};

unsigned short reserve_loopback_port() {
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    int address_size = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address),
                    &address_size) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    closesocket(socket_handle);
    return ntohs(address.sin_port);
}

bool send_fragmented_jpeg(SOCKET socket_handle,
                          const sockaddr_in& destination,
                          const std::vector<unsigned char>& jpeg) {
    if (jpeg.size() < 4) return false;
    const std::array<std::pair<std::size_t, std::size_t>, 3> fragments{{
        {0, 1},
        {1, jpeg.size() - 2},
        {jpeg.size() - 1, 1},
    }};
    for (const auto [offset, size] : fragments) {
        const int sent = sendto(
            socket_handle,
            reinterpret_cast<const char*>(jpeg.data() + offset),
            static_cast<int>(size), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent != static_cast<int>(size)) return false;
    }
    return true;
}

bool wait_for_capture_frame(ICapture& capture,
                            std::uint64_t after_sequence,
                            CapturedFrame& frame) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const CaptureStatus status = capture.grab(frame);
        if (status == CaptureStatus::FRAME &&
            frame.timing.sequence > after_sequence) {
            return true;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            return false;
        }
    }
    return false;
}

void test_udp_mjpeg_loopback() {
    WinsockSession winsock;
    expect(winsock.ready(), "Winsock 必须可用于 UDP 回环测试");
    if (!winsock.ready()) return;
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "UDP 回环测试必须取得临时端口");
    if (port == 0) return;

    cv::Mat first_source(320, 320, CV_8UC3, cv::Scalar(24, 96, 208));
    cv::Mat second_source(320, 320, CV_8UC3, cv::Scalar(208, 48, 20));
    std::vector<unsigned char> first_jpeg;
    std::vector<unsigned char> second_jpeg;
    expect(cv::imencode(".jpg", first_source, first_jpeg) &&
               cv::imencode(".jpg", second_source, second_jpeg) &&
               !first_jpeg.empty() && !second_jpeg.empty(),
           "UDP 回环测试必须生成两张内容不同的真实 JPEG 帧");
    if (first_jpeg.empty() || second_jpeg.empty()) return;

    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.udp_url = "udp://127.0.0.1:" + std::to_string(port);
    config.udp_read_timeout_ms = 50;
    config.udp_disconnect_timeout_ms = 300;
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 2560;
    config.udp_source_height = 1440;
    config.roi_width = 320;
    config.roi_height = 320;
    config.acquire_timeout_ms = 20;

    auto capture = create_capture(config);
    const bool opened = capture && capture->open();
    expect(opened, "生产 UDP Capture 必须能打开本机 MJPEG 数据流" +
                       (capture ? ": " + capture->last_error() : ""));
    if (!opened) return;

    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    expect(sender != INVALID_SOCKET, "UDP 回环发送套接字必须创建成功");
    if (sender == INVALID_SOCKET) {
        capture->close();
        return;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);

    expect(send_fragmented_jpeg(sender, destination, first_jpeg),
           "第一张 JPEG 的 SOI/EOI 跨数据报发送必须成功");
    CapturedFrame frame;
    const bool received_first = wait_for_capture_frame(*capture, 0, frame);
    const std::uint64_t first_sequence = frame.timing.sequence;
    const cv::Scalar first_mean = received_first
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_first && !frame.bgr.empty() &&
               frame.bgr.type() == CV_8UC3 && frame.bgr.cols == 320 &&
               frame.bgr.rows == 320,
           "UDP Capture 必须重组跨数据报标记并解码 320x320 BGR 帧");
    expect(received_first && frame.source_width == 2560 &&
               frame.source_height == 1440 && frame.encoded_width == 320 &&
               frame.encoded_height == 320 && frame.roi_x == 1120.0 &&
               frame.roi_y == 560.0 &&
               frame.source_pixels_per_pixel_x == 1.0 &&
               frame.source_pixels_per_pixel_y == 1.0,
           "辅机收到的 320x320 帧必须保留主机 2560x1440 中心 ROI 语义");

    expect(send_fragmented_jpeg(sender, destination, second_jpeg),
           "第二张变化 JPEG 的跨数据报发送必须成功");
    const bool received_second = wait_for_capture_frame(
        *capture, first_sequence, frame);
    const cv::Scalar second_mean = received_second
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_second && frame.timing.sequence > first_sequence &&
               std::abs(first_mean[0] - second_mean[0]) > 100.0,
           "连续 UDP 帧内容变化时输出必须随输入变化，不能重放旧帧");

    closesocket(sender);
    bool access_lost = false;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < disconnect_deadline) {
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::ACCESS_LOST) {
            access_lost = true;
            break;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            break;
        }
    }
    expect(access_lost,
           "UDP 长时间无可解码帧时必须进入 ACCESS_LOST 而非无限等待");
    capture->close();
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_mouse_disabled_by_default();
    test_invalid_keyboard_config();
    test_invalid_capture_config();
    test_udp_latest_frame_pool();
    test_udp_pool_is_bounded();
    test_udp_frame_geometry();
    test_xen_metadata_geometry();
    test_invalid_udp_capture_config();
#if XEN_HAS_NDI
    test_ndi_loopback();
#else
    test_ndi_unsupported_without_sdk();
#endif
    test_udp_mjpeg_loopback();
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "输入输出测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "输入输出测试全部通过。\n";
    return 0;
}
