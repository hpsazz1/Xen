#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "capture/ndi_internal.h"

#include "log/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <opencv2/imgproc.hpp>

#if XEN_HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

namespace capture::detail {

namespace {

bool terminal_status(CaptureStatus status) noexcept {
    return status == CaptureStatus::CLOSED ||
           status == CaptureStatus::ACCESS_LOST ||
           status == CaptureStatus::INVALID_CONFIG ||
           status == CaptureStatus::UNSUPPORTED ||
           status == CaptureStatus::FAILURE;
}

constexpr int kMaxSourceDimension = 16384;

NetworkGeometryConfig geometry_config(const CaptureConfig& config) noexcept {
    NetworkGeometryConfig result;
    result.layout = config.ndi_frame_layout;
    result.source_width = config.ndi_source_width;
    result.source_height = config.ndi_source_height;
    result.roi_width = config.roi_width;
    result.roi_height = config.roi_height;
    result.center_roi = config.center_roi;
    result.roi_x = config.roi_x;
    result.roi_y = config.roi_y;
    return result;
}

} // namespace

#if XEN_HAS_NDI

namespace {

std::mutex g_ndi_library_mutex;
int g_ndi_library_users = 0;
bool g_ndi_library_initialized = false;

bool acquire_ndi_library() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_ndi_library_mutex);
        if (g_ndi_library_users == 0) {
            if (!NDIlib_initialize()) return false;
            g_ndi_library_initialized = true;
        }
        ++g_ndi_library_users;
        return true;
    } catch (...) {
        return false;
    }
}

void release_ndi_library() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_ndi_library_mutex);
        if (g_ndi_library_users > 0) --g_ndi_library_users;
        if (g_ndi_library_users == 0 && g_ndi_library_initialized) {
            NDIlib_destroy();
            g_ndi_library_initialized = false;
        }
    } catch (...) {
    }
}

class NdiLibraryLease final {
public:
    NdiLibraryLease() noexcept : acquired_(acquire_ndi_library()) {}
    ~NdiLibraryLease() { if (acquired_) release_ndi_library(); }
    NdiLibraryLease(const NdiLibraryLease&) = delete;
    NdiLibraryLease& operator=(const NdiLibraryLease&) = delete;
    bool acquired() const noexcept { return acquired_; }

private:
    bool acquired_ = false;
};

} // namespace

class NdiCapture final : public ICapture {
public:
    explicit NdiCapture(CaptureConfig config)
        : config_(std::move(config)) {}

    ~NdiCapture() override { close(); }

    bool open() noexcept override {
        close();
        try {
            Log::register_module("capture", LogLevel::INFO);
            if (!valid_config()) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "NDI Capture 配置非法");
            }
            library_ = std::make_unique<NdiLibraryLease>();
            if (!library_ || !library_->acquired()) {
                library_.reset();
                return fail(CaptureStatus::FAILURE,
                            "NDI Runtime 初始化失败");
            }
            frames_.reset();
            last_metadata_.reset();
            source_name_.clear();
            source_url_.clear();
            sequence_ = 0;
            performance_sample_counter_ = 0;
            source_received_frames_ = 0;
            transport_dropped_frames_ = 0;
            last_delivered_sequence_ = 0;
            published_sequence_.store(0, std::memory_order_release);
            stop_requested_.store(false, std::memory_order_release);
            ever_connected_ = false;
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                last_error_.clear();
            }
            status_.store(CaptureStatus::READY, std::memory_order_release);
            worker_ = std::thread([this] { receive_loop(); });
            LOG_INFO("capture", "NDI Capture 已启动: source={}, layout={}",
                     config_.ndi_source_name,
                     NetworkFrameLayoutName(config_.ndi_frame_layout));
            return true;
        } catch (...) {
            return fail(CaptureStatus::FAILURE,
                        "打开 NDI Capture 时发生未知异常");
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
            return terminal_status(current) ? current : CaptureStatus::NO_FRAME;
        } catch (...) {
            fail(CaptureStatus::FAILURE, "读取 NDI 最新帧时发生未知异常");
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
        destroy_receiver();
        destroy_finder();
        frames_.reset();
        last_metadata_.reset();
        source_name_.clear();
        source_url_.clear();
        published_sequence_.store(0, std::memory_order_release);
        status_.store(CaptureStatus::CLOSED, std::memory_order_release);
        library_.reset();
    }

    CaptureStatus status() const noexcept override {
        return status_.load(std::memory_order_acquire);
    }

    std::string last_error() const override {
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            return last_error_;
        } catch (...) {
            return "读取 NDI 错误信息失败";
        }
    }

private:
    bool valid_config() const noexcept {
        const bool source_pair = config_.ndi_source_width > 0 &&
            config_.ndi_source_height > 0 &&
            config_.ndi_source_width <= kMaxSourceDimension &&
            config_.ndi_source_height <= kMaxSourceDimension;
        const bool layout_valid =
            (config_.ndi_frame_layout == NetworkFrameLayout::FULL_FRAME_1_TO_1 &&
             config_.ndi_source_width == 0 && config_.ndi_source_height == 0) ||
            ((config_.ndi_frame_layout == NetworkFrameLayout::FULL_FRAME_SCALED ||
              config_.ndi_frame_layout == NetworkFrameLayout::CENTER_CROP_1_TO_1) &&
             (source_pair || config_.ndi_require_frame_metadata));
        return config_.backend == CaptureBackend::NDI &&
            !config_.ndi_source_name.empty() &&
            config_.ndi_discovery_timeout_ms > 0 &&
            config_.ndi_discovery_timeout_ms <= 60000 &&
            config_.ndi_receive_timeout_ms > 0 &&
            config_.ndi_receive_timeout_ms <= 1000 &&
            config_.ndi_disconnect_timeout_ms >=
                config_.ndi_receive_timeout_ms &&
            config_.ndi_disconnect_timeout_ms <= 60000 &&
            config_.roi_width > 0 && config_.roi_height > 0 &&
            (config_.center_roi ||
             (config_.roi_x >= 0 && config_.roi_y >= 0)) &&
            config_.acquire_timeout_ms >= 0 && layout_valid;
    }

    bool fail(CaptureStatus status, const std::string& message) noexcept {
        status_.store(status, std::memory_order_release);
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
        LOG_ERROR("capture", "{}", message);
        frame_condition_.notify_all();
        return false;
    }

    void destroy_receiver() noexcept {
        if (receiver_) {
            NDIlib_recv_destroy(receiver_);
            receiver_ = nullptr;
        }
    }

    void destroy_finder() noexcept {
        if (finder_) {
            NDIlib_find_destroy(finder_);
            finder_ = nullptr;
        }
    }

    bool connect_source() noexcept {
        if (receiver_ || !finder_) return receiver_ != nullptr;
        const std::uint32_t wait_ms = static_cast<std::uint32_t>(
            std::min(config_.ndi_discovery_timeout_ms, 250));
        NDIlib_find_wait_for_sources(finder_, wait_ms);
        std::uint32_t count = 0;
        const NDIlib_source_t* sources =
            NDIlib_find_get_current_sources(finder_, &count);
        if (!sources || count == 0) return false;

        const NDIlib_source_t* selected = nullptr;
        if (config_.ndi_source_name == "Auto" ||
            config_.ndi_source_name == "auto") {
            if (count == 1) selected = &sources[0];
        } else {
            for (std::uint32_t index = 0; index < count; ++index) {
                if (sources[index].p_ndi_name &&
                    config_.ndi_source_name == sources[index].p_ndi_name) {
                    selected = &sources[index];
                    break;
                }
            }
        }
        if (!selected || !selected->p_ndi_name) return false;

        source_name_ = selected->p_ndi_name;
        source_url_ = selected->p_url_address ? selected->p_url_address : "";
        NDIlib_source_t source{};
        source.p_ndi_name = source_name_.c_str();
        source.p_url_address = source_url_.empty() ? nullptr : source_url_.c_str();
        NDIlib_recv_create_v3_t settings{};
        settings.source_to_connect_to = source;
        settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        settings.bandwidth = NDIlib_recv_bandwidth_highest;
        settings.allow_video_fields = false;
        settings.p_ndi_recv_name = "Xen NDI Capture";
        receiver_ = NDIlib_recv_create_v3(&settings);
        if (!receiver_) {
            source_name_.clear();
            source_url_.clear();
            return false;
        }
        performance_sample_counter_ = 0;
        source_received_frames_ = 0;
        transport_dropped_frames_ = 0;
        ever_connected_ = true;
        LOG_INFO("capture", "NDI 已连接源: {}", source_name_);
        return true;
    }

    bool publish_video(const NDIlib_video_frame_v2_t& video,
                       std::chrono::steady_clock::time_point received_at,
                       const XenFrameMetadata* metadata) noexcept {
        if (!video.p_data || video.xres <= 0 || video.yres <= 0 ||
            video.xres > kMaxSourceDimension ||
            video.yres > kMaxSourceDimension ||
            (video.FourCC != NDIlib_FourCC_type_BGRX &&
             video.FourCC != NDIlib_FourCC_type_BGRA)) {
            frames_.record_drop();
            return false;
        }
        const int stride = video.line_stride_in_bytes > 0
            ? video.line_stride_in_bytes : video.xres * 4;
        if (stride < video.xres * 4) {
            frames_.record_drop();
            return false;
        }

        try {
            NetworkFrameGeometry geometry;
            const NetworkGeometryConfig config = geometry_config(config_);
            const bool metadata_geometry = metadata &&
                resolve_network_frame_geometry(
                    config, video.xres, video.yres, geometry, metadata);
            if (!metadata_geometry && config_.ndi_require_frame_metadata) {
                fail(CaptureStatus::INVALID_CONFIG,
                     "NDI 帧缺少合法 Xen 主机坐标 metadata");
                frames_.record_drop();
                return false;
            }
            if (!metadata_geometry && !resolve_network_frame_geometry(
                    config, video.xres, video.yres, geometry)) {
                fail(CaptureStatus::INVALID_CONFIG,
                     "NDI 解码尺寸与主机 FOV 几何配置不兼容");
                frames_.record_drop();
                return false;
            }

            auto write_slot = frames_.acquire_write();
            if (!write_slot) {
                frames_.record_drop();
                return true;
            }
            cv::Mat bgra(video.yres, video.xres, CV_8UC4,
                         video.p_data, static_cast<std::size_t>(stride));
            const cv::Mat bgra_roi = bgra(cv::Rect(
                geometry.decoded_roi_x, geometry.decoded_roi_y,
                geometry.decoded_roi_width, geometry.decoded_roi_height));
            cv::cvtColor(bgra_roi, write_slot->bgr, cv::COLOR_BGRA2BGR);
            const auto finished = std::chrono::steady_clock::now();
            write_slot->timing.sequence = ++sequence_;
            write_slot->timing.captured_at = finished;
            write_slot->timing.capture_ms =
                std::chrono::duration<double, std::milli>(
                    finished - received_at).count();
            if (video.frame_rate_N > 0 && video.frame_rate_D > 0) {
                write_slot->timing.source_fps =
                    static_cast<double>(video.frame_rate_N) /
                    static_cast<double>(video.frame_rate_D);
            }
            write_slot->timing.source_timecode = video.timecode;
            write_slot->timing.source_timecode_valid =
                video.timecode != NDIlib_send_timecode_synthesize;
            write_slot->timing.source_timestamp = video.timestamp;
            write_slot->timing.source_timestamp_valid =
                video.timestamp != NDIlib_recv_timestamp_undefined;
            if (performance_sample_counter_++ % 30 == 0) {
                NDIlib_recv_performance_t total{};
                NDIlib_recv_performance_t dropped{};
                NDIlib_recv_get_performance(receiver_, &total, &dropped);
                source_received_frames_ = total.video_frames > 0
                    ? static_cast<std::uint64_t>(total.video_frames) : 0;
                transport_dropped_frames_ = dropped.video_frames > 0
                    ? static_cast<std::uint64_t>(dropped.video_frames) : 0;
            }
            write_slot->timing.source_received_frames =
                source_received_frames_;
            write_slot->timing.transport_dropped_frames =
                transport_dropped_frames_;
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
            published_sequence_.store(
                sequence_, std::memory_order_release);
            status_.store(CaptureStatus::FRAME, std::memory_order_release);
            frame_condition_.notify_one();
            return true;
        } catch (...) {
            frames_.record_drop();
            return false;
        }
    }

    void receive_loop() noexcept {
        try {
            receive_loop_impl();
        } catch (...) {
            fail(CaptureStatus::FAILURE, "NDI 接收线程发生未知异常");
        }
    }

    void receive_loop_impl() {
        const auto discovery_started = std::chrono::steady_clock::now();
        auto last_valid_frame = discovery_started;
        NDIlib_find_create_t finder_settings{};
        finder_settings.show_local_sources = true;
        finder_ = NDIlib_find_create_v2(&finder_settings);
        if (!finder_) {
            fail(CaptureStatus::FAILURE, "创建 NDI mDNS 发现器失败");
            return;
        }
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (!connect_source()) {
                const auto silent_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_valid_frame)
                        .count();
                if (!ever_connected_ &&
                    silent_ms >= config_.ndi_discovery_timeout_ms) {
                    fail(CaptureStatus::ACCESS_LOST,
                         "NDI 发现超时，未找到唯一匹配的源");
                    return;
                }
                if (ever_connected_ &&
                    silent_ms >= config_.ndi_disconnect_timeout_ms) {
                    fail(CaptureStatus::ACCESS_LOST,
                         "NDI 源长时间无可用视频帧，采集会话已失效");
                    return;
                }
                status_.store(CaptureStatus::NO_FRAME,
                              std::memory_order_release);
                continue;
            }

            NDIlib_video_frame_v2_t video{};
            NDIlib_metadata_frame_t metadata_frame{};
            const auto received_at = std::chrono::steady_clock::now();
            const NDIlib_frame_type_e type = NDIlib_recv_capture_v2(
                receiver_, &video, nullptr, &metadata_frame,
                static_cast<std::uint32_t>(config_.ndi_receive_timeout_ms));

            XenFrameMetadata parsed_metadata;
            bool metadata_valid = false;
            if (type == NDIlib_frame_type_metadata) {
                if (metadata_frame.p_data) {
                    const std::size_t length = metadata_frame.length > 0
                        ? static_cast<std::size_t>(metadata_frame.length)
                        : std::strlen(metadata_frame.p_data);
                    const std::size_t text_length =
                        length > 0 && metadata_frame.p_data[length - 1] == '\0'
                            ? length - 1 : length;
                    metadata_valid = parse_xen_frame_metadata(
                        std::string_view(metadata_frame.p_data, text_length),
                        parsed_metadata);
                    if (metadata_valid) last_metadata_ = parsed_metadata;
                }
                NDIlib_recv_free_metadata(receiver_, &metadata_frame);
            }

            if (type == NDIlib_frame_type_video) {
                if (video.p_metadata) {
                    metadata_valid = parse_xen_frame_metadata(
                        std::string_view(video.p_metadata), parsed_metadata);
                    if (metadata_valid) last_metadata_ = parsed_metadata;
                } else if (last_metadata_.has_value()) {
                    parsed_metadata = *last_metadata_;
                    metadata_valid = true;
                }
                const bool published = publish_video(
                    video, received_at,
                    metadata_valid ? &parsed_metadata : nullptr);
                NDIlib_recv_free_video_v2(receiver_, &video);
                if (terminal_status(status_.load(std::memory_order_acquire))) {
                    return;
                }
                if (published) last_valid_frame = std::chrono::steady_clock::now();
                continue;
            }
            if (type == NDIlib_frame_type_error) {
                destroy_receiver();
                last_metadata_.reset();
                continue;
            }
            if (type == NDIlib_frame_type_none) {
                status_.store(CaptureStatus::NO_FRAME,
                              std::memory_order_release);
            }
            const auto silent_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_valid_frame)
                    .count();
            if (ever_connected_ &&
                silent_ms >= config_.ndi_disconnect_timeout_ms) {
                fail(CaptureStatus::ACCESS_LOST,
                     "NDI 源长时间无可用视频帧，采集会话已失效");
                return;
            }
        }
    }

    CaptureConfig config_;
    std::unique_ptr<NdiLibraryLease> library_;
    NDIlib_find_instance_t finder_ = nullptr;
    NDIlib_recv_instance_t receiver_ = nullptr;
    std::string source_name_;
    std::string source_url_;
    std::optional<XenFrameMetadata> last_metadata_;
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
    std::uint64_t performance_sample_counter_ = 0;
    std::uint64_t source_received_frames_ = 0;
    std::uint64_t transport_dropped_frames_ = 0;
    std::uint64_t last_delivered_sequence_ = 0;
    bool ever_connected_ = false;
};

#else

class NdiCapture final : public ICapture {
public:
    explicit NdiCapture(CaptureConfig config) : config_(std::move(config)) {}
    bool open() noexcept override {
        status_ = CaptureStatus::UNSUPPORTED;
        error_ = "当前构建未发现 NDI 6 SDK，NDI 后端不可用";
        LOG_ERROR("capture", "{}", error_);
        return false;
    }
    CaptureStatus grab(CapturedFrame&) noexcept override {
        return status_;
    }
    void close() noexcept override { status_ = CaptureStatus::CLOSED; }
    CaptureStatus status() const noexcept override { return status_; }
    std::string last_error() const override { return error_; }

private:
    CaptureConfig config_;
    CaptureStatus status_ = CaptureStatus::CLOSED;
    std::string error_;
};

#endif

std::unique_ptr<ICapture> create_ndi_capture(
    const CaptureConfig& config) noexcept {
    try {
        return std::make_unique<NdiCapture>(config);
    } catch (...) {
        LOG_ERROR("capture", "创建 NDI Capture 实例失败");
        return nullptr;
    }
}

} // namespace capture::detail
