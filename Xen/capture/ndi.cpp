#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "capture/ndi_internal.h"

#include "clock_sync/clock_sync.h"
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
#include <vector>

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
constexpr auto kQueueProbeInterval = std::chrono::milliseconds(1000);
constexpr auto kConnectionProbeInterval = std::chrono::milliseconds(250);

SourceClockStatus source_clock_status(
        clock_sync::MappingStatus status) noexcept {
    switch (status) {
        case clock_sync::MappingStatus::UNSYNCHRONIZED:
            return SourceClockStatus::UNSYNCHRONIZED;
        case clock_sync::MappingStatus::WARMING:
            return SourceClockStatus::WARMING;
        case clock_sync::MappingStatus::VALID:
            return SourceClockStatus::VALID;
        case clock_sync::MappingStatus::STALE:
            return SourceClockStatus::STALE;
        case clock_sync::MappingStatus::INVALID:
            return SourceClockStatus::INVALID;
    }
    return SourceClockStatus::INVALID;
}

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

void NdiSessionState::reset(std::string configured_source_name) {
    configured_source_name_ = std::move(configured_source_name);
    snapshot_ = {};
}

bool NdiSessionState::observe_sources(
        std::span<const NdiSourceView> sources) {
    NdiSessionSnapshot next = snapshot_;
    next.source_snapshot_observed = true;
    next.source_count = sources.size();
    next.candidate_source_names.clear();
    for (const auto& source : sources) {
        if (!source.name.empty()) {
            next.candidate_source_names.emplace_back(source.name);
        }
    }
    std::sort(next.candidate_source_names.begin(),
              next.candidate_source_names.end());

    const NdiSourceView* selected = nullptr;
    const bool automatic = configured_source_name_ == "Auto" ||
                           configured_source_name_ == "auto";
    if (automatic && sources.size() == 1U) {
        if (!sources.front().name.empty()) selected = &sources.front();
    } else {
        for (const auto& source : sources) {
            if (source.name == configured_source_name_) {
                selected = &source;
                break;
            }
        }
    }
    next.selected_source.reset();
    if (selected) {
        next.selection_reason = NdiSessionFailureReason::NONE;
        next.stage = NdiSessionStage::SOURCE_SELECTED;
        next.selected_source = NdiOwnedSource{
            std::string(selected->name), std::string(selected->url)};
    } else {
        next.stage = NdiSessionStage::DISCOVERING;
        if (sources.empty()) {
            next.selection_reason = NdiSessionFailureReason::ZERO_SOURCES;
        } else if (automatic && sources.size() > 1U) {
            next.selection_reason =
                NdiSessionFailureReason::SOURCE_SELECTION_AMBIGUOUS;
        } else {
            next.selection_reason =
                NdiSessionFailureReason::SOURCE_NAME_NOT_FOUND;
        }
    }

    const bool changed =
        next.stage != snapshot_.stage ||
        next.selection_reason != snapshot_.selection_reason ||
        next.source_snapshot_observed !=
            snapshot_.source_snapshot_observed ||
        next.source_count != snapshot_.source_count ||
        next.candidate_source_names != snapshot_.candidate_source_names ||
        next.receiver_create_failed != snapshot_.receiver_create_failed ||
        next.selected_source.has_value() !=
            snapshot_.selected_source.has_value() ||
        (next.selected_source &&
         (next.selected_source->name != snapshot_.selected_source->name ||
          next.selected_source->url != snapshot_.selected_source->url));
    snapshot_ = std::move(next);
    return changed;
}

bool NdiSessionState::record_receiver_created(bool created) noexcept {
    const NdiSessionStage next_stage = snapshot_.selected_source
        ? NdiSessionStage::SOURCE_SELECTED
        : NdiSessionStage::DISCOVERING;
    const bool changed =
        snapshot_.receiver_create_failed != !created ||
        snapshot_.receiver_instance_created != created ||
        snapshot_.receiver_active_connections != 0 ||
        snapshot_.stage != (created
            ? NdiSessionStage::RECEIVER_CREATED : next_stage);
    snapshot_.receiver_create_failed = !created;
    snapshot_.receiver_instance_created = created;
    snapshot_.receiver_active_connections = 0;
    snapshot_.stage = created
        ? NdiSessionStage::RECEIVER_CREATED : next_stage;
    return changed;
}

bool NdiSessionState::record_active_connections(int connections) noexcept {
    connections = std::max(0, connections);
    const bool activates = connections > 0 &&
        snapshot_.stage != NdiSessionStage::FIRST_VIDEO_FRAME;
    const bool changed =
        snapshot_.receiver_active_connections != connections ||
        (activates && snapshot_.stage != NdiSessionStage::ACTIVE_CONNECTION);
    snapshot_.receiver_active_connections = connections;
    if (connections > 0) {
        snapshot_.receiver_ever_connected = true;
        if (snapshot_.stage != NdiSessionStage::FIRST_VIDEO_FRAME) {
            snapshot_.stage = NdiSessionStage::ACTIVE_CONNECTION;
        }
    }
    return changed;
}

bool NdiSessionState::record_valid_frame() noexcept {
    const bool changed = !snapshot_.first_video_frame_received ||
        snapshot_.stage != NdiSessionStage::FIRST_VIDEO_FRAME ||
        snapshot_.receiver_connection_lost;
    snapshot_.receiver_instance_created = true;
    snapshot_.receiver_create_failed = false;
    snapshot_.receiver_active_connections =
        std::max(1, snapshot_.receiver_active_connections);
    snapshot_.receiver_ever_connected = true;
    snapshot_.first_video_frame_received = true;
    snapshot_.receiver_connection_lost = false;
    snapshot_.stage = NdiSessionStage::FIRST_VIDEO_FRAME;
    return changed;
}

bool NdiSessionState::record_receiver_error() noexcept {
    const bool changed = snapshot_.receiver_instance_created ||
        snapshot_.receiver_active_connections != 0 ||
        !snapshot_.receiver_connection_lost;
    snapshot_.receiver_instance_created = false;
    snapshot_.receiver_create_failed = false;
    snapshot_.receiver_active_connections = 0;
    snapshot_.receiver_connection_lost =
        snapshot_.first_video_frame_received;
    snapshot_.stage = snapshot_.selected_source
        ? NdiSessionStage::SOURCE_SELECTED
        : NdiSessionStage::DISCOVERING;
    return changed;
}

NdiSessionFailureReason NdiSessionState::terminal_reason() const noexcept {
    if (snapshot_.receiver_connection_lost ||
        snapshot_.first_video_frame_received) {
        return NdiSessionFailureReason::RECEIVER_CONNECTION_LOST;
    }
    if (snapshot_.receiver_create_failed) {
        return NdiSessionFailureReason::RECEIVER_CREATE_FAILED;
    }
    if (snapshot_.receiver_ever_connected) {
        return NdiSessionFailureReason::FIRST_FRAME_TIMEOUT;
    }
    if (snapshot_.receiver_instance_created) {
        return NdiSessionFailureReason::RECEIVER_CONNECT_TIMEOUT;
    }
    return snapshot_.selection_reason;
}

NdiReceiveLoopDecision advance_ndi_receive_loop(
        NdiReceiveLoopEvent event,
        NdiSessionState& session_state,
        const NdiSilenceWatchdog& silence_watchdog,
        NdiSilenceWatchdog::Clock::time_point now,
        int discovery_timeout_ms,
        int disconnect_timeout_ms) noexcept {
    const NdiSessionFailureReason terminal_reason =
        session_state.terminal_reason();
    if (event == NdiReceiveLoopEvent::RECEIVER_ERROR) {
        session_state.record_receiver_error();
    }
    if (silence_watchdog.expired(
            now, discovery_timeout_ms, disconnect_timeout_ms)) {
        return {NdiReceiveLoopAction::ACCESS_LOST, terminal_reason};
    }
    if (event == NdiReceiveLoopEvent::RECEIVER_ERROR) {
        return {
            NdiReceiveLoopAction::RECONNECT_RECEIVER,
            terminal_reason};
    }
    return {NdiReceiveLoopAction::KEEP_RECEIVING, terminal_reason};
}

namespace {

std::string candidate_names_text(
        const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) {
        if (!result.empty()) result += ", ";
        result += name;
    }
    return result.empty() ? "<none>" : result;
}

std::string ndi_terminal_message(
        NdiSessionFailureReason reason,
        std::string_view configured_source_name,
        const NdiSessionSnapshot& snapshot) {
    const std::string candidates =
        candidate_names_text(snapshot.candidate_source_names);
    switch (reason) {
        case NdiSessionFailureReason::ZERO_SOURCES:
            return "NDI 发现超时：未观察到候选源";
        case NdiSessionFailureReason::SOURCE_NAME_NOT_FOUND:
            return "NDI 发现超时：配置源未精确匹配；source=" +
                std::string(configured_source_name) +
                "，candidates=" + candidates;
        case NdiSessionFailureReason::SOURCE_SELECTION_AMBIGUOUS:
            return "NDI 发现超时：Auto 模式候选源不唯一；candidates=" +
                candidates;
        case NdiSessionFailureReason::RECEIVER_CREATE_FAILED:
            return "NDI Receiver 实例创建失败；source=" +
                (snapshot.selected_source
                    ? snapshot.selected_source->name : "<none>");
        case NdiSessionFailureReason::RECEIVER_CONNECT_TIMEOUT:
            return "NDI Receiver 活动连接超时；source=" +
                (snapshot.selected_source
                    ? snapshot.selected_source->name : "<none>");
        case NdiSessionFailureReason::FIRST_FRAME_TIMEOUT:
            return "NDI Receiver 已有活动连接但未交付首个有效视频帧；source=" +
                (snapshot.selected_source
                    ? snapshot.selected_source->name : "<none>");
        case NdiSessionFailureReason::RECEIVER_CONNECTION_LOST:
            return "NDI 源长时间无可用视频帧，采集会话已失效";
        case NdiSessionFailureReason::NONE:
            break;
    }
    return "NDI 会话在未知阶段超时";
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
            clock_sync::ClientConfig clock_config;
            clock_config.source_url = config_.ndi_clock_sync_url;
            clock_config.exchange_interval_ms =
                config_.ndi_clock_sync_interval_ms;
            clock_config.response_timeout_ms =
                config_.ndi_clock_sync_timeout_ms;
            clock_config.maximum_mapping_age_ms =
                config_.ndi_clock_mapping_max_age_ms;
            if (!clock_client_.open(clock_config)) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            clock_client_.last_error());
            }
            library_ = std::make_unique<NdiLibraryLease>();
            if (!library_ || !library_->acquired()) {
                library_.reset();
                clock_client_.close();
                return fail(CaptureStatus::FAILURE,
                            "NDI Runtime 初始化失败");
            }
            frames_.reset();
            last_metadata_.reset();
            session_state_.reset(config_.ndi_source_name);
            sequence_ = 0;
            performance_sample_counter_ = 0;
            source_received_frames_ = 0;
            transport_dropped_frames_ = 0;
            last_queue_probe_at_ = {};
            last_connection_probe_at_ = {};
            last_delivered_sequence_ = 0;
            published_sequence_.store(0, std::memory_order_release);
            stop_requested_.store(false, std::memory_order_release);
            silence_watchdog_.reset(std::chrono::steady_clock::now());
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
            clock_client_.close();
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
        published_sequence_.store(0, std::memory_order_release);
        status_.store(CaptureStatus::CLOSED, std::memory_order_release);
        library_.reset();
        clock_client_.close();
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
            (config_.ndi_clock_sync_url.empty() ||
             (config_.ndi_clock_sync_interval_ms >= 50 &&
              config_.ndi_clock_sync_interval_ms <= 10000 &&
              config_.ndi_clock_sync_timeout_ms >= 10 &&
              config_.ndi_clock_sync_timeout_ms <= 5000 &&
              config_.ndi_clock_mapping_max_age_ms >=
                  config_.ndi_clock_sync_interval_ms &&
              config_.ndi_clock_mapping_max_age_ms <= 60000)) &&
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

    bool connect_source() {
        if (receiver_ || !finder_) return receiver_ != nullptr;
        const std::uint32_t wait_ms = static_cast<std::uint32_t>(
            std::min(config_.ndi_discovery_timeout_ms, 250));
        NDIlib_find_wait_for_sources(finder_, wait_ms);
        std::uint32_t count = 0;
        const NDIlib_source_t* sources =
            NDIlib_find_get_current_sources(finder_, &count);
        std::vector<NdiSourceView> source_views;
        if (sources) {
            source_views.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                source_views.push_back({
                    sources[index].p_ndi_name
                        ? std::string_view(sources[index].p_ndi_name)
                        : std::string_view{},
                    sources[index].p_url_address
                        ? std::string_view(sources[index].p_url_address)
                        : std::string_view{}});
            }
        }
        session_state_.observe_sources(source_views);
        const auto& selected = session_state_.snapshot().selected_source;
        if (!selected) return false;

        NDIlib_source_t source{};
        source.p_ndi_name = selected->name.c_str();
        source.p_url_address = selected->url.empty()
            ? nullptr : selected->url.c_str();
        NDIlib_recv_create_v3_t settings{};
        settings.source_to_connect_to = source;
        settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        settings.bandwidth = NDIlib_recv_bandwidth_highest;
        settings.allow_video_fields = false;
        settings.p_ndi_recv_name = "Xen NDI Capture";
        receiver_ = NDIlib_recv_create_v3(&settings);
        session_state_.record_receiver_created(receiver_ != nullptr);
        if (!receiver_) {
            return false;
        }
        performance_sample_counter_ = 0;
        source_received_frames_ = 0;
        transport_dropped_frames_ = 0;
        last_queue_probe_at_ = {};
        last_connection_probe_at_ = {};
        LOG_INFO("capture", "NDI Receiver 实例已创建: source={}",
                 selected->name);
        return true;
    }

    void observe_active_connections(
            std::chrono::steady_clock::time_point now) noexcept {
        if (!receiver_ ||
            (last_connection_probe_at_ !=
                 std::chrono::steady_clock::time_point{} &&
             now - last_connection_probe_at_ < kConnectionProbeInterval)) {
            return;
        }
        last_connection_probe_at_ = now;
        const int previous =
            session_state_.snapshot().receiver_active_connections;
        const int current = NDIlib_recv_get_no_connections(receiver_);
        if (!session_state_.record_active_connections(current)) return;
        if (previous <= 0 && current > 0) {
            LOG_INFO("capture", "NDI 活动连接已建立: source={}",
                     session_state_.snapshot().selected_source
                         ? session_state_.snapshot().selected_source->name
                         : "<none>");
        } else {
            LOG_TRACE("capture", "NDI 活动连接数变化: previous={}, current={}",
                      previous, std::max(0, current));
        }
    }

    bool publish_video(const NDIlib_video_frame_v2_t& video,
                       std::chrono::steady_clock::time_point received_at,
                       const XenFrameMetadata* metadata,
                       CaptureStageTiming capture_stages) noexcept {
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
            const auto geometry_started = capture_stages.ndi_valid
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
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
            if (capture_stages.ndi_valid) {
                capture_stages.geometry_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        geometry_started).count();
            }

            const auto pool_acquire_started = capture_stages.ndi_valid
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            auto write_slot = frames_.acquire_write();
            if (capture_stages.ndi_valid) {
                capture_stages.pool_acquire_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        pool_acquire_started).count();
            }
            if (!write_slot) {
                frames_.record_drop();
                return true;
            }
            cv::Mat bgra(video.yres, video.xres, CV_8UC4,
                         video.p_data, static_cast<std::size_t>(stride));
            const cv::Mat bgra_roi = bgra(cv::Rect(
                geometry.decoded_roi_x, geometry.decoded_roi_y,
                geometry.decoded_roi_width, geometry.decoded_roi_height));
            const auto convert_started = capture_stages.ndi_valid
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            cv::cvtColor(bgra_roi, write_slot->bgr, cv::COLOR_BGRA2BGR);
            const auto finished = std::chrono::steady_clock::now();
            if (capture_stages.ndi_valid) {
                capture_stages.color_convert_ms =
                    std::chrono::duration<double, std::milli>(
                        finished - convert_started).count();
            }
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
            if (write_slot->timing.source_timestamp_valid) {
                write_slot->timing.source_time_basis =
                    SourceTimeBasis::NDI_SDK_SUBMISSION;
                const auto mapped = clock_client_.map_utc_100ns(
                    video.timestamp, finished);
                write_slot->timing.source_clock_status =
                    source_clock_status(mapped.status);
                write_slot->timing.source_clock_uncertainty_ms =
                    mapped.uncertainty_ms;
                write_slot->timing.source_clock_round_trip_ms =
                    mapped.round_trip_ms;
                write_slot->timing.source_clock_rate = mapped.clock_rate;
                write_slot->timing.source_clock_mapping_age_ms =
                    mapped.mapping_age_ms;
                write_slot->timing.source_clock_sample_count =
                    mapped.sample_count;
                write_slot->timing.source_clock_session_id =
                    mapped.source_session_id;
                // NDI submission 必须发生在辅机完成颜色转换之前；若映射给出
                // 未来时刻，保持 INVALID，不能把负 age 钳成零。
                if (mapped.valid && mapped.local_time <= finished) {
                    write_slot->timing.source_time_timing_valid = true;
                    write_slot->timing.source_time_at = mapped.local_time;
                } else if (mapped.valid) {
                    write_slot->timing.source_clock_status =
                        SourceClockStatus::INVALID;
                }
            }
            if (performance_sample_counter_++ % 30 == 0) {
                NDIlib_recv_performance_t total{};
                NDIlib_recv_performance_t dropped{};
                const auto query_started = capture_stages.ndi_valid
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                NDIlib_recv_get_performance(receiver_, &total, &dropped);
                if (capture_stages.ndi_valid) {
                    capture_stages.performance_query_sampled = true;
                    capture_stages.performance_query_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            query_started).count();
                }
                source_received_frames_ = total.video_frames > 0
                    ? static_cast<std::uint64_t>(total.video_frames) : 0;
                transport_dropped_frames_ = dropped.video_frames > 0
                    ? static_cast<std::uint64_t>(dropped.video_frames) : 0;
            }
            if (capture_stages.ndi_valid) {
                const auto probe_started = std::chrono::steady_clock::now();
                if (last_queue_probe_at_.time_since_epoch().count() == 0 ||
                    probe_started - last_queue_probe_at_ >=
                        kQueueProbeInterval) {
                    NDIlib_recv_queue_t queued{};
                    NDIlib_recv_get_queue(receiver_, &queued);
                    capture_stages.queue_depth_sampled = true;
                    capture_stages.queue_query_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            probe_started).count();
                    capture_stages.queued_video_frames =
                        std::max(queued.video_frames, 0);
                    capture_stages.queued_audio_frames =
                        std::max(queued.audio_frames, 0);
                    capture_stages.queued_metadata_frames =
                        std::max(queued.metadata_frames, 0);
                    last_queue_probe_at_ = probe_started;
                }
            }
            write_slot->timing.source_received_frames =
                source_received_frames_;
            write_slot->timing.transport_dropped_frames =
                transport_dropped_frames_;
            write_slot->timing.capture_stages = capture_stages;
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
        NDIlib_find_create_t finder_settings{};
        finder_settings.show_local_sources = true;
        finder_ = NDIlib_find_create_v2(&finder_settings);
        if (!finder_) {
            fail(CaptureStatus::FAILURE, "创建 NDI mDNS 发现器失败");
            return;
        }
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (!connect_source()) {
                if (silence_watchdog_.expired(
                        std::chrono::steady_clock::now(),
                        config_.ndi_discovery_timeout_ms,
                        config_.ndi_disconnect_timeout_ms)) {
                    fail(CaptureStatus::ACCESS_LOST,
                         ndi_terminal_message(
                             session_state_.terminal_reason(),
                             config_.ndi_source_name,
                             session_state_.snapshot()));
                    return;
                }
                status_.store(CaptureStatus::NO_FRAME,
                              std::memory_order_release);
                continue;
            }

            observe_active_connections(std::chrono::steady_clock::now());
            NDIlib_video_frame_v2_t video{};
            NDIlib_metadata_frame_t metadata_frame{};
            const auto received_at = std::chrono::steady_clock::now();
            const NDIlib_frame_type_e type = NDIlib_recv_capture_v2(
                receiver_, &video, nullptr, &metadata_frame,
                static_cast<std::uint32_t>(config_.ndi_receive_timeout_ms));
            CaptureStageTiming capture_stages;
            capture_stages.ndi_valid = config_.enable_performance_probes;
            if (capture_stages.ndi_valid) {
                capture_stages.receive_call_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - received_at).count();
            }

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
                const auto metadata_started = capture_stages.ndi_valid
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                if (video.p_metadata) {
                    metadata_valid = parse_xen_frame_metadata(
                        std::string_view(video.p_metadata), parsed_metadata);
                    if (metadata_valid) last_metadata_ = parsed_metadata;
                } else if (last_metadata_.has_value()) {
                    parsed_metadata = *last_metadata_;
                    metadata_valid = true;
                }
                if (capture_stages.ndi_valid) {
                    capture_stages.metadata_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            metadata_started).count();
                }
                const bool published = publish_video(
                    video, received_at,
                    metadata_valid ? &parsed_metadata : nullptr,
                    capture_stages);
                NDIlib_recv_free_video_v2(receiver_, &video);
                if (terminal_status(status_.load(std::memory_order_acquire))) {
                    return;
                }
                if (published) {
                    silence_watchdog_.record_valid_frame(
                        std::chrono::steady_clock::now());
                    const bool recovering = session_state_.snapshot().
                        receiver_connection_lost;
                    if (session_state_.record_valid_frame()) {
                        if (recovering) {
                            LOG_INFO("capture",
                                "NDI 有效视频帧已恢复: source={}",
                                session_state_.snapshot().selected_source
                                    ? session_state_.snapshot().
                                        selected_source->name
                                    : "<none>");
                        } else {
                            LOG_INFO("capture",
                                "NDI 首个有效视频帧已收到: source={}",
                                session_state_.snapshot().selected_source
                                    ? session_state_.snapshot().
                                        selected_source->name
                                    : "<none>");
                        }
                    }
                }
                continue;
            }
            const NdiReceiveLoopDecision loop_decision =
                advance_ndi_receive_loop(
                    type == NDIlib_frame_type_error
                        ? NdiReceiveLoopEvent::RECEIVER_ERROR
                        : NdiReceiveLoopEvent::NON_VIDEO_FRAME,
                    session_state_, silence_watchdog_,
                    std::chrono::steady_clock::now(),
                    config_.ndi_discovery_timeout_ms,
                    config_.ndi_disconnect_timeout_ms);
            if (type == NDIlib_frame_type_error) {
                last_connection_probe_at_ = {};
                destroy_receiver();
                last_metadata_.reset();
            }
            if (loop_decision.action ==
                    NdiReceiveLoopAction::ACCESS_LOST) {
                fail(CaptureStatus::ACCESS_LOST,
                     ndi_terminal_message(
                         loop_decision.terminal_reason,
                         config_.ndi_source_name,
                         session_state_.snapshot()));
                return;
            }
            if (loop_decision.action ==
                    NdiReceiveLoopAction::RECONNECT_RECEIVER) {
                continue;
            }
            if (type == NDIlib_frame_type_none) {
                status_.store(CaptureStatus::NO_FRAME,
                              std::memory_order_release);
            }
        }
    }

    CaptureConfig config_;
    std::unique_ptr<NdiLibraryLease> library_;
    clock_sync::Client clock_client_;
    NDIlib_find_instance_t finder_ = nullptr;
    NDIlib_recv_instance_t receiver_ = nullptr;
    NdiSessionState session_state_;
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
    std::chrono::steady_clock::time_point last_queue_probe_at_{};
    std::chrono::steady_clock::time_point last_connection_probe_at_{};
    std::uint64_t last_delivered_sequence_ = 0;
    NdiSilenceWatchdog silence_watchdog_;
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
