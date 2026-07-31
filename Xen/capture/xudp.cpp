#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/xudp_internal.h"

#include "capture/udp_internal.h"
#include "log/log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <opencv2/imgcodecs.hpp>

namespace capture::detail {

namespace {

constexpr std::size_t kMaxUdpPacketBytes = 65507;
constexpr int kSocketReceiveBufferBytes = 4 * 1024 * 1024;

bool terminal_status(CaptureStatus status) noexcept {
    return status == CaptureStatus::CLOSED ||
           status == CaptureStatus::ACCESS_LOST ||
           status == CaptureStatus::INVALID_CONFIG ||
           status == CaptureStatus::UNSUPPORTED ||
           status == CaptureStatus::FAILURE;
}

class XudpCapture final : public ICapture {
public:
    explicit XudpCapture(CaptureConfig config)
        : config_(std::move(config)) {}

    ~XudpCapture() override { close(); }

    bool open() noexcept override {
        close();
        try {
            Log::register_module("capture", LogLevel::INFO);
            if (!valid_config()) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "XUDP JPEG Capture 配置非法");
            }
            std::string bind_address;
            std::uint16_t port = 0;
            if (!parse_udp_url(config_.udp_url, bind_address, port)) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "XUDP 地址格式非法，要求 udp://地址:端口");
            }
            if (!open_socket(bind_address, port)) return false;

            frames_.reset();
            assembler_.reset();
            decoded_frame_.release();
            sequence_ = 0;
            last_delivered_sequence_ = 0;
            published_sequence_.store(0, std::memory_order_release);
            stop_requested_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                last_error_.clear();
            }
            status_.store(CaptureStatus::READY, std::memory_order_release);
            worker_ = std::thread([this] { receive_loop(); });
            LOG_INFO("capture",
                     "XUDP JPEG 已监听: {}:{}, roi={}x{}",
                     bind_address, port, config_.roi_width,
                     config_.roi_height);
            return true;
        } catch (...) {
            release_socket();
            return fail(CaptureStatus::FAILURE,
                        "打开 XUDP JPEG Capture 时发生未知异常");
        }
    }

    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        try {
            CaptureStatus current = status_.load(std::memory_order_acquire);
            if (terminal_status(current)) return current;
            if (frames_.take_latest(last_delivered_sequence_, frame)) {
                current = status_.load(std::memory_order_acquire);
                if (terminal_status(current)) return current;
                last_delivered_sequence_ = frame.timing.sequence;
                return CaptureStatus::FRAME;
            }

            std::unique_lock<std::mutex> lock(condition_mutex_);
            frame_condition_.wait_for(
                lock, std::chrono::milliseconds(config_.acquire_timeout_ms),
                [this] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           published_sequence_.load(
                               std::memory_order_acquire) !=
                               last_delivered_sequence_ ||
                           terminal_status(
                               status_.load(std::memory_order_acquire));
                });
            lock.unlock();

            current = status_.load(std::memory_order_acquire);
            if (terminal_status(current)) return current;
            if (frames_.take_latest(last_delivered_sequence_, frame)) {
                current = status_.load(std::memory_order_acquire);
                if (terminal_status(current)) return current;
                last_delivered_sequence_ = frame.timing.sequence;
                return CaptureStatus::FRAME;
            }
            current = status_.load(std::memory_order_acquire);
            return terminal_status(current) ? current
                                            : CaptureStatus::NO_FRAME;
        } catch (...) {
            fail(CaptureStatus::FAILURE,
                 "读取 XUDP JPEG 最新帧时发生未知异常");
            return CaptureStatus::FAILURE;
        }
    }

    void close() noexcept override {
        stop_requested_.store(true, std::memory_order_release);
        frame_condition_.notify_all();
        try {
            if (worker_.joinable()) worker_.join();
        } catch (...) {
        }
        release_socket();
        frames_.reset();
        assembler_.reset();
        decoded_frame_.release();
        published_sequence_.store(0, std::memory_order_release);
        status_.store(CaptureStatus::CLOSED, std::memory_order_release);
    }

    CaptureStatus status() const noexcept override {
        return status_.load(std::memory_order_acquire);
    }

    std::string last_error() const override {
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            return last_error_;
        } catch (...) {
            return "读取 XUDP JPEG 错误信息失败";
        }
    }

private:
    bool valid_config() const noexcept {
        return config_.backend == CaptureBackend::XUDP_JPEG &&
               !config_.udp_url.empty() && config_.roi_width > 0 &&
               config_.roi_height > 0 && config_.roi_x >= 0 &&
               config_.roi_y >= 0 && config_.acquire_timeout_ms >= 0 &&
               config_.udp_read_timeout_ms > 0 &&
               config_.udp_read_timeout_ms <= 1000 &&
               config_.udp_disconnect_timeout_ms >=
                   config_.udp_read_timeout_ms &&
               config_.udp_disconnect_timeout_ms <= 60000;
    }

    bool open_socket(const std::string& bind_address,
                     std::uint16_t port) noexcept {
        WSADATA winsock_data{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            return fail(CaptureStatus::FAILURE, "初始化 Winsock 失败");
        }
        winsock_ready_ = true;

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* addresses = nullptr;
        const std::string port_text = std::to_string(port);
        const int resolve_result = getaddrinfo(
            bind_address.c_str(), port_text.c_str(), &hints, &addresses);
        if (resolve_result != 0 || !addresses) {
            release_socket();
            return fail(CaptureStatus::INVALID_CONFIG,
                        "无法解析 XUDP 监听地址: " + bind_address);
        }

        for (const addrinfo* address = addresses; address != nullptr;
             address = address->ai_next) {
            const SOCKET candidate = socket(
                address->ai_family, address->ai_socktype,
                address->ai_protocol);
            if (candidate == INVALID_SOCKET) continue;
            const DWORD timeout =
                static_cast<DWORD>(config_.udp_read_timeout_ms);
            const int receive_buffer = kSocketReceiveBufferBytes;
            const bool configured =
                setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout),
                           sizeof(timeout)) != SOCKET_ERROR &&
                setsockopt(candidate, SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char*>(&receive_buffer),
                           sizeof(receive_buffer)) != SOCKET_ERROR;
            if (configured &&
                bind(candidate, address->ai_addr,
                     static_cast<int>(address->ai_addrlen)) != SOCKET_ERROR) {
                socket_ = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket_ == INVALID_SOCKET) {
            release_socket();
            return fail(CaptureStatus::FAILURE,
                        "绑定 XUDP JPEG 监听端口失败");
        }
        return true;
    }

    void release_socket() noexcept {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (winsock_ready_) {
            WSACleanup();
            winsock_ready_ = false;
        }
    }

    bool decode_and_publish(const XudpCompletedFrame& completed) noexcept {
        try {
            if (completed.jpeg.empty() ||
                completed.jpeg.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<int>::max())) {
                assembler_.record_invalid_frame();
                return false;
            }
            // imdecode 只在调用期间读取该 Mat；底层分片槽在下一包前保持有效。
            const cv::Mat encoded(
                1, static_cast<int>(completed.jpeg.size()), CV_8UC1,
                const_cast<std::uint8_t*>(completed.jpeg.data()));
            const cv::Mat decoded = cv::imdecode(
                encoded, cv::IMREAD_COLOR, &decoded_frame_);
            if (decoded.empty() || decoded.type() != CV_8UC3 ||
                decoded.cols !=
                    static_cast<int>(completed.descriptor.encoded_width) ||
                decoded.rows !=
                    static_cast<int>(completed.descriptor.encoded_height)) {
                assembler_.record_invalid_frame();
                return false;
            }

            NetworkFrameGeometry geometry;
            if (!resolve_xudp_frame_geometry(
                    config_, completed.descriptor, geometry)) {
                assembler_.record_invalid_frame();
                return false;
            }

            auto write_slot = frames_.acquire_write();
            if (!write_slot) {
                // 消费者持有全部槽时丢弃已解码帧，继续排空网络以保持低延迟。
                frames_.record_drop();
                return true;
            }
            decoded(cv::Rect(
                geometry.decoded_roi_x, geometry.decoded_roi_y,
                geometry.decoded_roi_width, geometry.decoded_roi_height))
                .copyTo(write_slot->bgr);
            const auto finished = std::chrono::steady_clock::now();
            write_slot->timing.sequence = ++sequence_;
            write_slot->timing.captured_at = finished;
            write_slot->timing.capture_ms =
                std::chrono::duration<double, std::milli>(
                    finished - completed.started_at).count();
            write_slot->timing.transport_dropped_frames =
                completed.transport_dropped_frames;
            write_slot->timing.transport_invalid_packets =
                assembler_.transport_invalid_packets();
            write_slot->timing.source_received_frames =
                completed.source_received_frames;
            write_slot->timing.source_sequence =
                completed.descriptor.frame_id;
            write_slot->timing.source_sequence_valid = true;
            write_slot->timing.source_fps =
                static_cast<double>(completed.descriptor.frame_rate_n) /
                completed.descriptor.frame_rate_d;
            write_slot->timing.source_timestamp = static_cast<std::int64_t>(
                completed.descriptor.sent_timestamp_ns);
            write_slot->timing.source_timestamp_valid = true;
            write_slot->roi_x = geometry.source_roi_x;
            write_slot->roi_y = geometry.source_roi_y;
            write_slot->source_width = geometry.source_width;
            write_slot->source_height = geometry.source_height;
            write_slot->encoded_width = geometry.encoded_width;
            write_slot->encoded_height = geometry.encoded_height;
            write_slot->source_pixels_per_pixel_x =
                geometry.source_pixels_per_pixel_x;
            write_slot->source_pixels_per_pixel_y =
                geometry.source_pixels_per_pixel_y;
            frames_.publish(write_slot);
            published_sequence_.store(sequence_, std::memory_order_release);
            status_.store(CaptureStatus::FRAME, std::memory_order_release);
            frame_condition_.notify_one();
            return true;
        } catch (...) {
            assembler_.record_invalid_frame();
            return false;
        }
    }

    void receive_loop() noexcept {
        try {
            auto last_valid_frame = std::chrono::steady_clock::now();
            while (!stop_requested_.load(std::memory_order_acquire)) {
                const int received = recvfrom(
                    socket_, reinterpret_cast<char*>(packet_.data()),
                    static_cast<int>(packet_.size()), 0, nullptr, nullptr);
                const auto received_at = std::chrono::steady_clock::now();
                if (stop_requested_.load(std::memory_order_acquire)) break;

                if (received == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK) {
                        fail(CaptureStatus::FAILURE,
                             "XUDP JPEG 接收失败，Winsock 错误码: " +
                                 std::to_string(error));
                        frame_condition_.notify_all();
                        return;
                    }
                    const auto silent_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            received_at - last_valid_frame).count();
                    if (silent_ms >= config_.udp_disconnect_timeout_ms) {
                        fail(CaptureStatus::ACCESS_LOST,
                             "XUDP JPEG 数据流超时，采集会话已失效");
                        frame_condition_.notify_all();
                        return;
                    }
                    status_.store(CaptureStatus::NO_FRAME,
                                  std::memory_order_release);
                    continue;
                }
                if (received <= 0) continue;

                XudpCompletedFrame completed;
                const XudpConsumeResult result = assembler_.consume_packet(
                    std::span<const std::uint8_t>(
                        packet_.data(), static_cast<std::size_t>(received)),
                    received_at, completed);
                bool valid_frame = false;
                if (result == XudpConsumeResult::FRAME) {
                    valid_frame = decode_and_publish(completed);
                    if (terminal_status(
                            status_.load(std::memory_order_acquire))) {
                        return;
                    }
                }
                if (valid_frame) {
                    last_valid_frame = received_at;
                } else {
                    const auto silent_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            received_at - last_valid_frame).count();
                    if (silent_ms >= config_.udp_disconnect_timeout_ms) {
                        fail(CaptureStatus::ACCESS_LOST,
                             "XUDP JPEG 长时间未收到完整有效帧，采集会话已失效");
                        frame_condition_.notify_all();
                        return;
                    }
                }
            }
        } catch (...) {
            fail(CaptureStatus::FAILURE,
                 "XUDP JPEG 接收线程发生未知异常");
            frame_condition_.notify_all();
        }
    }

    bool fail(CaptureStatus status, const std::string& message) noexcept {
        status_.store(status, std::memory_order_release);
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
        LOG_ERROR("capture", "{}", message);
        return false;
    }

    CaptureConfig config_;
    SOCKET socket_ = INVALID_SOCKET;
    bool winsock_ready_ = false;
    std::array<std::uint8_t, kMaxUdpPacketBytes> packet_{};
    XudpFrameAssembler assembler_;
    cv::Mat decoded_frame_;
    NetworkLatestFramePool frames_;
    std::thread worker_;
    mutable std::mutex error_mutex_;
    std::mutex condition_mutex_;
    std::condition_variable frame_condition_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<CaptureStatus> status_{CaptureStatus::CLOSED};
    std::atomic<std::uint64_t> published_sequence_{0};
    std::string last_error_;
    std::uint64_t sequence_ = 0;
    std::uint64_t last_delivered_sequence_ = 0;
};

} // namespace

std::unique_ptr<ICapture> create_xudp_capture(
        const CaptureConfig& config) noexcept {
    try {
        return std::make_unique<XudpCapture>(config);
    } catch (...) {
        LOG_ERROR("capture", "创建 XUDP JPEG Capture 实例失败");
        return nullptr;
    }
}

} // namespace capture::detail
