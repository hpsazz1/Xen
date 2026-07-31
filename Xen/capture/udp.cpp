#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/udp_internal.h"

#include "log/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace capture::detail {

UdpLatestFramePool::UdpLatestFramePool() {
    for (auto& slot : pool_) {
        slot = std::make_shared<UdpDecodedFrame>();
    }
}

std::shared_ptr<UdpDecodedFrame>
UdpLatestFramePool::acquire_write() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& slot : pool_) {
            // pool_ 自身持有一个引用；其他引用来自解码线程或 CapturedFrame。
            // 最新帧即使尚未被消费也不能覆写，否则 grab() 会读到撕裂内容。
            if (slot != latest_ && slot.use_count() == 1) return slot;
        }
    } catch (...) {
    }
    return nullptr;
}

void UdpLatestFramePool::publish(
        const std::shared_ptr<UdpDecodedFrame>& frame) noexcept {
    if (!frame || frame->bgr.empty() || frame->timing.sequence == 0) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ && latest_->timing.sequence != consumed_sequence_) {
            ++dropped_frames_;
        }
        frame->timing.source_dropped_frames = dropped_frames_;
        latest_ = frame;
    } catch (...) {
    }
}

bool UdpLatestFramePool::take_latest(
        std::uint64_t last_sequence,
        CapturedFrame& frame) noexcept {
    try {
        std::shared_ptr<UdpDecodedFrame> latest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latest_ || latest_->timing.sequence == last_sequence) {
                return false;
            }
            latest_->timing.source_dropped_frames = dropped_frames_;
            latest = latest_;
            consumed_sequence_ = latest_->timing.sequence;
        }

        // 先释放旧 Mat 视图，再归还其 storage 引用，避免解码线程过早复用旧槽。
        frame.bgr.release();
        frame.bgr_storage.reset();
        frame.bgr_storage = std::shared_ptr<const cv::Mat>(
            latest, &latest->bgr);
        frame.bgr = *frame.bgr_storage;
        frame.timing = latest->timing;
        frame.roi_x = latest->roi_x;
        frame.roi_y = latest->roi_y;
        frame.source_width = latest->source_width;
        frame.source_height = latest->source_height;
        frame.encoded_width = latest->encoded_width;
        frame.encoded_height = latest->encoded_height;
        frame.source_pixels_per_pixel_x =
            latest->source_pixels_per_pixel_x;
        frame.source_pixels_per_pixel_y =
            latest->source_pixels_per_pixel_y;
        return true;
    } catch (...) {
        return false;
    }
}

void UdpLatestFramePool::record_drop() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_frames_;
    } catch (...) {
    }
}

std::uint64_t UdpLatestFramePool::dropped_frames() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_frames_;
    } catch (...) {
        return 0;
    }
}

void UdpLatestFramePool::reset() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset();
        consumed_sequence_ = 0;
        dropped_frames_ = 0;
        for (const auto& slot : pool_) {
            if (slot.use_count() != 1) continue;
            slot->timing = {};
            slot->roi_x = 0;
            slot->roi_y = 0;
            slot->source_width = 0;
            slot->source_height = 0;
            slot->encoded_width = 0;
            slot->encoded_height = 0;
            slot->source_pixels_per_pixel_x = 1.0;
            slot->source_pixels_per_pixel_y = 1.0;
            // 保留 Mat 容量，固定 ROI 下后续会话不重新申请大缓冲。
        }
    } catch (...) {
    }
}

bool parse_udp_url(const std::string& url,
                   std::string& bind_address,
                   std::uint16_t& port) noexcept {
    try {
        constexpr std::string_view kPrefix = "udp://";
        if (!std::string_view(url).starts_with(kPrefix)) return false;
        const std::size_t authority_begin = kPrefix.size();
        const std::size_t query = url.find('?', authority_begin);
        const std::size_t slash = url.find('/', authority_begin);
        if (slash != std::string::npos &&
            (query == std::string::npos || slash < query)) {
            return false;
        }
        const std::size_t authority_end = query;
        const std::string authority = url.substr(
            authority_begin,
            authority_end == std::string::npos
                ? std::string::npos
                : authority_end - authority_begin);
        if (authority.empty() || authority.front() == '[') return false;
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos || colon + 1 >= authority.size()) {
            return false;
        }

        std::string parsed_address = authority.substr(0, colon);
        if (!parsed_address.empty() && parsed_address.front() == '@') {
            parsed_address.erase(parsed_address.begin());
        }
        if (parsed_address.empty()) parsed_address = "0.0.0.0";

        unsigned int parsed_port = 0;
        const std::string_view port_text(authority.data() + colon + 1,
                                         authority.size() - colon - 1);
        const auto result = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(),
            parsed_port);
        if (result.ec != std::errc{} ||
            result.ptr != port_text.data() + port_text.size() ||
            parsed_port == 0 || parsed_port > 65535) {
            return false;
        }
        bind_address = std::move(parsed_address);
        port = static_cast<std::uint16_t>(parsed_port);
        return true;
    } catch (...) {
        return false;
    }
}

bool resolve_udp_frame_geometry(const CaptureConfig& config,
                                int encoded_width,
                                int encoded_height,
                                UdpFrameGeometry& geometry) noexcept {
    try {
        if (encoded_width <= 0 || encoded_height <= 0 ||
            config.roi_width <= 0 || config.roi_height <= 0) {
            return false;
        }

        UdpFrameGeometry resolved;
        resolved.encoded_width = encoded_width;
        resolved.encoded_height = encoded_height;
        if (config.center_roi) {
            resolved.decoded_roi_width =
                std::min(config.roi_width, encoded_width);
            resolved.decoded_roi_height =
                std::min(config.roi_height, encoded_height);
            resolved.decoded_roi_x =
                (encoded_width - resolved.decoded_roi_width) / 2;
            resolved.decoded_roi_y =
                (encoded_height - resolved.decoded_roi_height) / 2;
        } else {
            resolved.decoded_roi_x = config.roi_x;
            resolved.decoded_roi_y = config.roi_y;
            resolved.decoded_roi_width = config.roi_width;
            resolved.decoded_roi_height = config.roi_height;
        }
        if (resolved.decoded_roi_x < 0 || resolved.decoded_roi_y < 0 ||
            resolved.decoded_roi_width <= 0 ||
            resolved.decoded_roi_height <= 0 ||
            resolved.decoded_roi_x + resolved.decoded_roi_width >
                encoded_width ||
            resolved.decoded_roi_y + resolved.decoded_roi_height >
                encoded_height) {
            return false;
        }

        switch (config.udp_frame_layout) {
            case UdpFrameLayout::FULL_FRAME_1_TO_1:
                resolved.source_width = encoded_width;
                resolved.source_height = encoded_height;
                resolved.source_roi_x = resolved.decoded_roi_x;
                resolved.source_roi_y = resolved.decoded_roi_y;
                break;
            case UdpFrameLayout::FULL_FRAME_SCALED:
                if (config.udp_source_width <= 0 ||
                    config.udp_source_height <= 0) {
                    return false;
                }
                resolved.source_width = config.udp_source_width;
                resolved.source_height = config.udp_source_height;
                resolved.source_pixels_per_pixel_x =
                    static_cast<double>(resolved.source_width) /
                    static_cast<double>(encoded_width);
                resolved.source_pixels_per_pixel_y =
                    static_cast<double>(resolved.source_height) /
                    static_cast<double>(encoded_height);
                resolved.source_roi_x =
                    resolved.decoded_roi_x *
                    resolved.source_pixels_per_pixel_x;
                resolved.source_roi_y =
                    resolved.decoded_roi_y *
                    resolved.source_pixels_per_pixel_y;
                break;
            case UdpFrameLayout::CENTER_CROP_1_TO_1:
                if (config.udp_source_width < encoded_width ||
                    config.udp_source_height < encoded_height) {
                    return false;
                }
                resolved.source_width = config.udp_source_width;
                resolved.source_height = config.udp_source_height;
                // 与 Desktop Duplication 的中心 ROI 一致，奇数余量留在右/下侧，
                // 保证 1:1 模式的主机 ROI 原点始终落在整数像素。
                resolved.source_roi_x =
                    (resolved.source_width - encoded_width) / 2 +
                    resolved.decoded_roi_x;
                resolved.source_roi_y =
                    (resolved.source_height - encoded_height) / 2 +
                    resolved.decoded_roi_y;
                break;
            default:
                return false;
        }

        geometry = resolved;
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

constexpr std::size_t kMaxUdpPacketBytes = 65507;
constexpr std::size_t kMaxJpegFrameBytes = 8 * 1024 * 1024;
constexpr int kSocketReceiveBufferBytes = 1024 * 1024;
constexpr int kMaxSourceDimension = 16384;

bool terminal_status(CaptureStatus status) noexcept {
    return status == CaptureStatus::CLOSED ||
           status == CaptureStatus::ACCESS_LOST ||
           status == CaptureStatus::INVALID_CONFIG ||
           status == CaptureStatus::UNSUPPORTED ||
           status == CaptureStatus::FAILURE;
}

class UdpMjpegCapture final : public ICapture {
public:
    explicit UdpMjpegCapture(CaptureConfig config)
        : config_(std::move(config)) {
        jpeg_bytes_.reserve(kMaxJpegFrameBytes);
    }

    ~UdpMjpegCapture() override { close(); }

    bool open() noexcept override {
        close();
        try {
            Log::register_module("capture", LogLevel::INFO);
            if (!valid_config()) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "UDP MJPEG Capture 配置非法");
            }
            std::string bind_address;
            std::uint16_t port = 0;
            if (!parse_udp_url(config_.udp_url, bind_address, port)) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "UDP MJPEG 地址格式非法，要求 udp://地址:端口");
            }
            if (!open_socket(bind_address, port)) return false;

            frames_.reset();
            jpeg_bytes_.clear();
            collecting_jpeg_ = false;
            pending_ff_ = false;
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
            LOG_INFO(
                "capture",
                "UDP MJPEG 已监听: {}:{}, layout={}, source={}x{}, roi={}x{}",
                bind_address, port,
                UdpFrameLayoutName(config_.udp_frame_layout),
                config_.udp_source_width, config_.udp_source_height,
                config_.roi_width, config_.roi_height);
            return true;
        } catch (...) {
            release_socket();
            return fail(CaptureStatus::FAILURE,
                        "打开 UDP MJPEG Capture 时发生未知异常");
        }
    }

    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        try {
            CaptureStatus current =
                status_.load(std::memory_order_acquire);
            if (terminal_status(current)) return current;
            if (frames_.take_latest(last_delivered_sequence_, frame)) {
                current = status_.load(std::memory_order_acquire);
                if (terminal_status(current)) return current;
                last_delivered_sequence_ = frame.timing.sequence;
                return CaptureStatus::FRAME;
            }

            std::unique_lock<std::mutex> lock(condition_mutex_);
            frame_condition_.wait_for(
                lock,
                std::chrono::milliseconds(config_.acquire_timeout_ms),
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
                 "读取 UDP MJPEG 最新帧时发生未知异常");
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
        jpeg_bytes_.clear();
        collecting_jpeg_ = false;
        pending_ff_ = false;
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
            return "读取 UDP MJPEG 错误信息失败";
        }
    }

private:
    bool valid_config() const noexcept {
        const bool source_pair = config_.udp_source_width > 0 &&
                                 config_.udp_source_height > 0 &&
                                 config_.udp_source_width <=
                                     kMaxSourceDimension &&
                                 config_.udp_source_height <=
                                     kMaxSourceDimension;
        const bool layout_valid =
            (config_.udp_frame_layout ==
                 UdpFrameLayout::FULL_FRAME_1_TO_1 &&
             config_.udp_source_width == 0 &&
             config_.udp_source_height == 0) ||
            ((config_.udp_frame_layout ==
                  UdpFrameLayout::FULL_FRAME_SCALED ||
              config_.udp_frame_layout ==
                  UdpFrameLayout::CENTER_CROP_1_TO_1) &&
             source_pair);
        return config_.backend == CaptureBackend::UDP_MJPEG && layout_valid &&
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
            return fail(CaptureStatus::FAILURE,
                        "初始化 Winsock 失败");
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
                        "无法解析 UDP 监听地址: " + bind_address);
        }

        for (const addrinfo* address = addresses;
             address != nullptr;
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
                        "绑定 UDP MJPEG 监听端口失败");
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

    bool decode_and_publish(
            std::chrono::steady_clock::time_point started) noexcept {
        try {
            const cv::Mat decoded = cv::imdecode(
                jpeg_bytes_, cv::IMREAD_COLOR, &decoded_frame_);
            if (decoded.empty() || decoded.type() != CV_8UC3) {
                frames_.record_drop();
                return false;
            }

            UdpFrameGeometry geometry;
            if (!resolve_udp_frame_geometry(
                    config_, decoded.cols, decoded.rows, geometry)) {
                fail(CaptureStatus::INVALID_CONFIG,
                     "UDP MJPEG 解码尺寸与几何配置不兼容");
                frame_condition_.notify_all();
                return false;
            }

            auto write_slot = frames_.acquire_write();
            if (!write_slot) {
                // 池满时丢弃当前完整解码帧，继续排空 UDP；不能等待 Runtime，
                // 否则会把网络积压转换为瞄准输入延迟。
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
                    finished - started).count();
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
            frames_.record_drop();
            return false;
        }
    }

    bool consume_packet(
            const unsigned char* data,
            std::size_t size,
            std::chrono::steady_clock::time_point received_at) noexcept {
        bool decoded_valid_frame = false;
        for (std::size_t index = 0; index < size; ++index) {
            const unsigned char byte = data[index];
            if (!collecting_jpeg_) {
                if (pending_ff_ && byte == 0xd8) {
                    jpeg_bytes_.clear();
                    jpeg_bytes_.push_back(0xff);
                    jpeg_bytes_.push_back(0xd8);
                    collecting_jpeg_ = true;
                    pending_ff_ = false;
                    jpeg_started_at_ = received_at;
                } else {
                    pending_ff_ = byte == 0xff;
                }
                continue;
            }

            const bool previous_was_ff =
                !jpeg_bytes_.empty() && jpeg_bytes_.back() == 0xff;
            if (previous_was_ff && byte == 0xd8) {
                // 上一帧缺失 EOI 时以新的 SOI 重新同步，旧半帧单独计数。
                frames_.record_drop();
                jpeg_bytes_.clear();
                jpeg_bytes_.push_back(0xff);
                jpeg_bytes_.push_back(0xd8);
                jpeg_started_at_ = received_at;
                continue;
            }

            jpeg_bytes_.push_back(byte);
            if (jpeg_bytes_.size() > kMaxJpegFrameBytes) {
                frames_.record_drop();
                jpeg_bytes_.clear();
                collecting_jpeg_ = false;
                pending_ff_ = byte == 0xff;
                continue;
            }
            if (previous_was_ff && byte == 0xd9) {
                decoded_valid_frame =
                    decode_and_publish(jpeg_started_at_) ||
                    decoded_valid_frame;
                jpeg_bytes_.clear();
                collecting_jpeg_ = false;
                pending_ff_ = false;
                if (terminal_status(
                        status_.load(std::memory_order_acquire))) {
                    return decoded_valid_frame;
                }
            }
        }
        return decoded_valid_frame;
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
                             "UDP MJPEG 接收失败，Winsock 错误码: " +
                                 std::to_string(error));
                        frame_condition_.notify_all();
                        return;
                    }
                    const auto silent_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            received_at - last_valid_frame).count();
                    if (silent_ms >= config_.udp_disconnect_timeout_ms) {
                        fail(CaptureStatus::ACCESS_LOST,
                             "UDP MJPEG 数据流超时，采集会话已失效");
                        frame_condition_.notify_all();
                        return;
                    }
                    status_.store(CaptureStatus::NO_FRAME,
                                  std::memory_order_release);
                    continue;
                }
                if (received <= 0) continue;

                const bool valid_frame = consume_packet(
                        packet_.data(), static_cast<std::size_t>(received),
                        received_at);
                if (valid_frame) {
                    last_valid_frame = received_at;
                } else {
                    const auto silent_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            received_at - last_valid_frame).count();
                    if (silent_ms >= config_.udp_disconnect_timeout_ms) {
                        fail(CaptureStatus::ACCESS_LOST,
                             "UDP MJPEG 长时间未收到可解码帧，采集会话已失效");
                        frame_condition_.notify_all();
                        return;
                    }
                }
                if (terminal_status(
                        status_.load(std::memory_order_acquire))) {
                    return;
                }
            }
        } catch (...) {
            fail(CaptureStatus::FAILURE,
                 "UDP MJPEG 接收线程发生未知异常");
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
    std::array<unsigned char, kMaxUdpPacketBytes> packet_{};
    std::vector<unsigned char> jpeg_bytes_;
    cv::Mat decoded_frame_;
    UdpLatestFramePool frames_;
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
    bool collecting_jpeg_ = false;
    bool pending_ff_ = false;
    std::chrono::steady_clock::time_point jpeg_started_at_{};
};

} // namespace

std::unique_ptr<ICapture> create_udp_mjpeg_capture(
        const CaptureConfig& config) noexcept {
    try {
        return std::make_unique<UdpMjpegCapture>(config);
    } catch (...) {
        LOG_ERROR("capture", "创建 UDP MJPEG Capture 实例失败");
        return nullptr;
    }
}

} // namespace capture::detail
