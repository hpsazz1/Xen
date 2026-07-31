#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "log/log.h"
#include "sender/sender.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include <opencv2/core.hpp>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

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

unsigned short reserve_loopback_port() noexcept {
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

bool wait_for_capture_frame(ICapture& capture, std::uint64_t after_sequence,
                            CapturedFrame& frame) noexcept {
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

CapturedFrame make_frame(const cv::Scalar& color) {
    CapturedFrame frame;
    frame.bgr = cv::Mat(320, 320, CV_8UC3, color);
    frame.roi_x = 1120.0;
    frame.roi_y = 560.0;
    frame.source_width = 2560;
    frame.source_height = 1440;
    frame.encoded_width = 2560;
    frame.encoded_height = 1440;
    frame.source_pixels_per_pixel_x = 1.0;
    frame.source_pixels_per_pixel_y = 1.0;
    return frame;
}

void test_invalid_sender_config() {
    XudpSender sender;
    XudpSenderConfig config;
    config.destination_url = "udp://0.0.0.0:5000";
    expect(!sender.open(config) &&
               sender.status() == XudpSenderStatus::INVALID_CONFIG,
           "XUDP Sender 不得把监听通配地址作为目的地址");

    config.destination_url = "udp://127.0.0.1:5000";
    config.jpeg_quality = 0;
    expect(!sender.open(config) &&
               sender.status() == XudpSenderStatus::INVALID_CONFIG,
           "XUDP Sender 必须拒绝非法 JPEG 质量");

    config.jpeg_quality = 85;
    config.max_datagram_bytes = 124;
    expect(!sender.open(config) &&
               sender.status() == XudpSenderStatus::INVALID_CONFIG,
           "XUDP Sender 数据报必须能容纳固定头和至少一个 payload 字节");
}

void test_production_sender_to_capture_loopback() {
    WinsockSession winsock;
    expect(winsock.ready(), "Winsock 必须可用于生产 XUDP Sender 回环测试");
    if (!winsock.ready()) return;
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "生产 XUDP Sender 回环测试必须取得临时端口");
    if (port == 0) return;

    CaptureConfig capture_config;
    capture_config.backend = CaptureBackend::XUDP_JPEG;
    capture_config.udp_url =
        "udp://127.0.0.1:" + std::to_string(port);
    capture_config.udp_read_timeout_ms = 20;
    capture_config.udp_disconnect_timeout_ms = 1000;
    capture_config.roi_width = 320;
    capture_config.roi_height = 320;
    capture_config.acquire_timeout_ms = 20;
    auto capture = create_capture(capture_config);
    const bool capture_opened = capture && capture->open();
    expect(capture_opened,
           "生产 XUDP Capture 必须能打开 Sender 回环端口" +
               (capture ? ": " + capture->last_error() : ""));
    if (!capture_opened) return;

    XudpSenderConfig sender_config;
    sender_config.destination_url =
        "udp://127.0.0.1:" + std::to_string(port);
    sender_config.jpeg_quality = 85;
    sender_config.max_datagram_bytes = 1400;
    sender_config.frame_rate_n = 240;
    sender_config.frame_rate_d = 1;
    XudpSender sender;
    const bool sender_opened = sender.open(sender_config);
    expect(sender_opened,
           "生产 XUDP Sender 必须能连接 Capture 回环端口: " +
               sender.last_error());
    if (!sender_opened) {
        capture->close();
        return;
    }

    CapturedFrame invalid_fractional = make_frame(cv::Scalar(16, 80, 220));
    invalid_fractional.roi_x = 1120.5;
    expect(!sender.send_frame(invalid_fractional) &&
               sender.status() == XudpSenderStatus::READY,
           "XUDP Sender 必须拒绝协议无法精确表达的小数主机 ROI");
    CapturedFrame invalid_range = make_frame(cv::Scalar(16, 80, 220));
    invalid_range.roi_x = 2401.0;
    expect(!sender.send_frame(invalid_range) &&
               sender.status() == XudpSenderStatus::READY,
           "XUDP Sender 必须拒绝超出主机 FOV 的 ROI");

    CapturedFrame first_source = make_frame(cv::Scalar(24, 96, 208));
    CapturedFrame second_source = make_frame(cv::Scalar(208, 48, 20));
    expect(sender.send_frame(first_source),
           "生产 XUDP Sender 必须发送第一张真实 JPEG 帧: " +
               sender.last_error());
    CapturedFrame received;
    const bool received_first = wait_for_capture_frame(*capture, 0, received);
    const std::uint64_t first_sequence = received.timing.sequence;
    const cv::Scalar first_mean = received_first
        ? cv::mean(received.bgr) : cv::Scalar{};
    expect(received_first && received.bgr.cols == 320 &&
               received.bgr.rows == 320 && received.source_width == 2560 &&
               received.source_height == 1440 && received.roi_x == 1120.0 &&
               received.roi_y == 560.0 &&
               received.source_pixels_per_pixel_x == 1.0 &&
               received.source_pixels_per_pixel_y == 1.0,
           "生产 Sender/Capture 必须保留 2560x1440 主机中心 ROI (1120,560)");
    expect(received_first && received.timing.source_sequence_valid &&
               received.timing.source_sequence == 1 &&
               std::abs(received.timing.source_fps - 240.0) < 0.001,
           "生产 Sender 首帧必须发布 frame_id=1 和 240 FPS 契约");

    expect(sender.send_frame(second_source),
           "生产 XUDP Sender 必须发送第二张变化 JPEG 帧: " +
               sender.last_error());
    const bool received_second = wait_for_capture_frame(
        *capture, first_sequence, received);
    const cv::Scalar second_mean = received_second
        ? cv::mean(received.bgr) : cv::Scalar{};
    expect(received_second && received.timing.source_sequence == 2 &&
               std::abs(first_mean[0] - second_mean[0]) > 100.0,
           "生产 Sender 连续变化帧不得重放首次内容");

    const XudpSenderStats successful_stats = sender.stats();
    expect(successful_stats.datagrams_sent >= 2 &&
               successful_stats.last_frame_datagrams >= 1 &&
               successful_stats.largest_datagram_bytes <= 1400 &&
               successful_stats.jpeg_bytes_sent > 0 &&
               successful_stats.wire_bytes_sent >
                   successful_stats.jpeg_bytes_sent,
           "生产 Sender 必须统计分片、JPEG 和不超过 1400 字节的线速数据报");

    expect(!sender.send_frame(invalid_fractional),
           "生产 Sender 成功发送后仍必须拒绝小数主机 ROI");

    const XudpSenderStats stats = sender.stats();
    expect(stats.stream_id != 0 && stats.last_frame_id == 2 &&
               stats.frames_sent == 2 && stats.frames_failed == 3,
           "前置失败不得清空生产 Sender 最后已分配的 frame_id");

    sender.close();
    capture->close();
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_invalid_sender_config();
    test_production_sender_to_capture_loopback();
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "发送端测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "发送端测试全部通过。\n";
    return 0;
}
