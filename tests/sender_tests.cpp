#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "log/log.h"
#include "sender/report.h"
#include "sender/sender.h"
#include "sender/sender_internal.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

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

void test_sender_frame_pacer_uses_absolute_deadline() {
    using namespace std::chrono_literals;
    const auto origin = std::chrono::steady_clock::time_point{};
    sender::detail::SenderFramePacer pacer(10ms);

    expect(pacer.due(origin), "首帧必须立即允许发送");
    pacer.record_sent(origin);
    expect(!pacer.due(origin + 9ms) && pacer.due(origin + 10ms),
           "首帧成功后必须按绝对帧间隔限速");

    // 11 ms 才完成下一次采样时，截止点仍应从 10 ms 推进到 20 ms；
    // 不能改成 21 ms，否则每帧 1 ms 的处理抖动会永久累积。
    pacer.record_sent(origin + 11ms);
    expect(pacer.next_send_at() == origin + 20ms,
           "处理耗时不得漂移后续绝对截止时间");

    // 落后多个周期时只发送当前最新帧，并把截止点推进到 now 之后；
    // 禁止连续补发已经过时的历史周期。
    pacer.record_sent(origin + 45ms);
    expect(pacer.next_send_at() == origin + 50ms &&
               !pacer.due(origin + 45ms),
           "落后时必须跳过历史周期且不得突发补发旧帧");
}

void test_sender_frame_pacer_does_not_advance_on_failure() {
    using namespace std::chrono_literals;
    const auto origin = std::chrono::steady_clock::time_point{};
    sender::detail::SenderFramePacer pacer(10ms);
    pacer.record_sent(origin);

    // due() 是只读判断。发送失败时调用方不执行 record_sent()，下一张
    // 新采集帧仍可立即重试，而不是被一次失败消耗整个发送周期。
    expect(pacer.due(origin + 12ms) && pacer.due(origin + 13ms) &&
               pacer.next_send_at() == origin + 10ms,
           "发送失败不得推进 pacing 截止时间");
}

sender::detail::SenderRunReport make_valid_report() {
    sender::detail::SenderRunReport report;
    report.destination_url = "udp://127.0.0.1:5000";
    report.stop_reason = "frame_limit";
    report.jpeg_quality = 85;
    report.max_datagram_bytes = 1400;
    report.fps = 240;
    report.maximum_frames = 2;
    report.elapsed_seconds = 1.25;
    report.geometry.source_width = 2560;
    report.geometry.source_height = 1440;
    report.geometry.encoded_width = 320;
    report.geometry.encoded_height = 320;
    report.geometry.roi_x = 1120;
    report.geometry.roi_y = 560;
    report.geometry.roi_width = 320;
    report.geometry.roi_height = 320;
    report.stats.stream_id = 42;
    report.stats.last_frame_id = 2;
    report.stats.frames_sent = 2;
    report.stats.datagrams_sent = 4;
    report.stats.jpeg_bytes_sent = 4000;
    report.stats.wire_bytes_sent = 4496;
    report.stats.largest_datagram_bytes = 1400;
    report.stats.last_frame_datagrams = 2;
    report.stats.last_frame_jpeg_bytes = 2000;
    report.stats.last_frame_wire_bytes = 2248;
    report.samples = {
        {1, 0.4, 0.2, 0.1, 0.05, 0.35, 2, 2000, 2248},
        {2, 0.6, 0.4, 0.2, 0.10, 0.70, 2, 2000, 2248},
    };
    return report;
}

void test_sender_report_duration_capacity() {
    expect(!sender::detail::sender_report_duration_fits_capacity(240, 0),
           "未声明非零时长上限不得证明报告容量有界");
    expect(sender::detail::sender_report_duration_fits_capacity(240, 833),
           "199920 个理论样本必须保持在报告容量内");
    expect(!sender::detail::sender_report_duration_fits_capacity(240, 834),
           "200160 个理论样本必须在发送前拒绝");
    expect(sender::detail::sender_report_duration_fits_capacity(
               200'000, 1),
           "恰好 200000 个理论样本必须继续允许");
    expect(!sender::detail::sender_report_duration_fits_capacity(
               1'000'000, std::numeric_limits<std::uint64_t>::max()),
           "极大时长必须无乘法溢出地拒绝");
}

void test_sender_report_atomic_publish() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"xen-sender-report-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    expect(!ignored, "发送报告测试目录必须创建成功");
    if (ignored) return;

    const auto report_path = directory / L"sender.json";
    std::string error;
    auto report = make_valid_report();
    expect(sender::detail::write_sender_run_report(
               report_path.string(), report, error),
           "合法发送报告必须原子发布: " + error);
    std::ifstream input(report_path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    expect(input.good() || input.eof(), "发送报告必须可完整读取");
    expect(text.str().find("\"schema\": 1") != std::string::npos &&
               text.str().find("\"capture_to_send\"") !=
                   std::string::npos &&
               text.str().find("\"frame_id\": 2") !=
                   std::string::npos &&
               text.str().find("\"roi_x\": 1120") !=
                   std::string::npos &&
               text.str().find("\"p50_ms\": 0.3") !=
                   std::string::npos,
           "发送报告必须包含 schema、逐帧计时和主机 ROI");

    expect(!sender::detail::write_sender_run_report(
               report_path.string(), report, error),
           "发送报告不得覆盖既有目标");
    std::size_t pending_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().wstring().find(L".pending-") !=
            std::wstring::npos) {
            ++pending_files;
        }
    }
    expect(pending_files == 0, "发送报告成功或拒绝覆盖后不得遗留 pending");

    auto failed_report = make_valid_report();
    failed_report.stats.frames_failed = 1;
    const auto failed_path = directory / L"failed.json";
    expect(!sender::detail::write_sender_run_report(
               failed_path.string(), failed_report, error) &&
               !std::filesystem::exists(failed_path),
           "包含发送失败的运行不得发布 complete 报告");
    auto dropped_report = make_valid_report();
    dropped_report.samples.pop_back();
    dropped_report.samples_dropped = 1;
    const auto dropped_path = directory / L"dropped.json";
    expect(!sender::detail::write_sender_run_report(
               dropped_path.string(), dropped_report, error) &&
               !std::filesystem::exists(dropped_path),
           "发送样本丢弃时不得发布 complete 报告");
    auto inconsistent_report = make_valid_report();
    ++inconsistent_report.stats.wire_bytes_sent;
    const auto inconsistent_path = directory / L"inconsistent.json";
    expect(!sender::detail::write_sender_run_report(
               inconsistent_path.string(), inconsistent_report, error) &&
               !std::filesystem::exists(inconsistent_path),
           "逐帧字节数与累计统计不一致时不得发布 complete 报告");

    input.close();
    std::filesystem::remove(report_path, ignored);
    ignored.clear();
    std::filesystem::remove(directory, ignored);
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
               successful_stats.last_frame_wire_bytes >
                   successful_stats.last_frame_jpeg_bytes &&
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

int main(int argc, char* argv[]) {
    if (argc > 2 ||
        (argc == 2 && std::string_view(argv[1]) != "--report-only")) {
        std::cerr << "发送端测试未知参数。\n";
        return 2;
    }
    const bool report_only = argc == 2;
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    if (!report_only) {
        test_invalid_sender_config();
        test_sender_frame_pacer_uses_absolute_deadline();
        test_sender_frame_pacer_does_not_advance_on_failure();
    }
    test_sender_report_duration_capacity();
    test_sender_report_atomic_publish();
    if (!report_only) test_production_sender_to_capture_loopback();
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "发送端测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "发送端测试全部通过。\n";
    return 0;
}
