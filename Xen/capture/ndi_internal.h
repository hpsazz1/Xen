#ifndef CAPTURE_NDI_INTERNAL_H
#define CAPTURE_NDI_INTERNAL_H

#include "capture/capture.h"
#include "capture/network_internal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if XEN_HAS_NDI
#include "clock_sync/clock_sync.h"
#include <Processing.NDI.Lib.h>
#endif

namespace capture::detail {

// receiver 句柄创建成功只代表已发起连接，不能推进此状态机。
// 首帧前使用完整发现预算；收到有效帧后才切换到更短的断流预算。
class NdiSilenceWatchdog final {
public:
    using Clock = std::chrono::steady_clock;

    void reset(Clock::time_point started_at) noexcept {
        received_valid_frame_ = false;
        last_valid_frame_at_ = started_at;
    }

    void record_valid_frame(Clock::time_point received_at) noexcept {
        received_valid_frame_ = true;
        last_valid_frame_at_ = received_at;
    }

    bool received_valid_frame() const noexcept {
        return received_valid_frame_;
    }

    int timeout_ms(int discovery_timeout_ms,
                   int disconnect_timeout_ms) const noexcept {
        return received_valid_frame_
            ? disconnect_timeout_ms
            : discovery_timeout_ms;
    }

    bool expired(Clock::time_point now,
                 int discovery_timeout_ms,
                 int disconnect_timeout_ms) const noexcept {
        const auto silent_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_valid_frame_at_).count();
        return silent_ms >= timeout_ms(
            discovery_timeout_ms, disconnect_timeout_ms);
    }

private:
    bool received_valid_frame_ = false;
    Clock::time_point last_valid_frame_at_{};
};

enum class NdiSessionStage : std::uint8_t {
    DISCOVERING,
    SOURCE_SELECTED,
    RECEIVER_CREATED,
    ACTIVE_CONNECTION,
    FIRST_VIDEO_FRAME,
};

enum class NdiSessionFailureReason : std::uint8_t {
    NONE,
    ZERO_SOURCES,
    SOURCE_NAME_NOT_FOUND,
    SOURCE_SELECTION_AMBIGUOUS,
    RECEIVER_CREATE_FAILED,
    RECEIVER_CONNECT_TIMEOUT,
    FIRST_FRAME_TIMEOUT,
    RECEIVER_CONNECTION_LOST,
};

struct NdiSourceView {
    std::string_view name;
    std::string_view url;
};

struct NdiOwnedSource {
    std::string name;
    std::string url;
};

struct NdiSessionSnapshot {
    NdiSessionStage stage = NdiSessionStage::DISCOVERING;
    NdiSessionFailureReason selection_reason =
        NdiSessionFailureReason::ZERO_SOURCES;
    bool source_snapshot_observed = false;
    std::size_t source_count = 0;
    std::vector<std::string> candidate_source_names;
    std::optional<NdiOwnedSource> selected_source;
    bool receiver_create_failed = false;
    bool receiver_instance_created = false;
    int receiver_active_connections = 0;
    bool receiver_ever_connected = false;
    bool first_video_frame_received = false;
    bool receiver_connection_lost = false;
};

// Finder/Receiver 只提供借用 SDK 快照；本状态 owner 在一次调用内复制字符串，
// 并把发现、实例、活动连接和首帧保持为互不冒充的证据层。
class NdiSessionState final {
public:
    void reset(std::string configured_source_name);
    bool observe_sources(std::span<const NdiSourceView> sources);
    bool record_receiver_created(bool created) noexcept;
    bool record_active_connections(int connections) noexcept;
    bool record_valid_frame() noexcept;
    bool record_receiver_error() noexcept;

    NdiSessionFailureReason terminal_reason() const noexcept;
    const NdiSessionSnapshot& snapshot() const noexcept { return snapshot_; }

private:
    std::string configured_source_name_;
    NdiSessionSnapshot snapshot_;
};

enum class NdiReceiveLoopEvent : std::uint8_t {
    NON_VIDEO_FRAME,
    RECEIVER_ERROR,
};

enum class NdiReceiveLoopAction : std::uint8_t {
    KEEP_RECEIVING,
    RECONNECT_RECEIVER,
    ACCESS_LOST,
};

struct NdiReceiveLoopDecision {
    NdiReceiveLoopAction action = NdiReceiveLoopAction::KEEP_RECEIVING;
    NdiSessionFailureReason terminal_reason = NdiSessionFailureReason::NONE;
};

// SDK frame 资源仍由 NdiCapture 释放；这里只推进接收循环的状态与有界静默判定，
// 使真实 error/none 分支和可注入时钟测试共用同一条决策路径。
NdiReceiveLoopDecision advance_ndi_receive_loop(
    NdiReceiveLoopEvent event,
    NdiSessionState& session_state,
    const NdiSilenceWatchdog& silence_watchdog,
    NdiSilenceWatchdog::Clock::time_point now,
    int discovery_timeout_ms,
    int disconnect_timeout_ms) noexcept;

std::unique_ptr<ICapture> create_ndi_capture(
    const CaptureConfig& config) noexcept;

#if XEN_HAS_NDI
// 只隔离外部 SDK 与源机时钟输入；帧解析、发布和消费仍属于真实 NdiCapture。
// 帧借用到对应 free；源字符串借用到下次 source 快照或 Finder destroy。
class INdiCaptureSystem {
public:
    virtual ~INdiCaptureSystem() = default;
    virtual CaptureStatus open(const clock_sync::ClientConfig& config,
                               std::string& error) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual NDIlib_find_instance_t find_create(
        const NDIlib_find_create_t* settings) noexcept = 0;
    virtual void find_destroy(NDIlib_find_instance_t finder) noexcept = 0;
    virtual bool find_wait(NDIlib_find_instance_t finder,
                           std::uint32_t timeout_ms) noexcept = 0;
    virtual const NDIlib_source_t* find_sources(
        NDIlib_find_instance_t finder, std::uint32_t* count) noexcept = 0;
    virtual NDIlib_recv_instance_t recv_create(
        const NDIlib_recv_create_v3_t* settings) noexcept = 0;
    virtual void recv_destroy(NDIlib_recv_instance_t receiver) noexcept = 0;
    virtual int recv_connections(NDIlib_recv_instance_t receiver) noexcept = 0;
    virtual NDIlib_frame_type_e recv_capture(
        NDIlib_recv_instance_t receiver, NDIlib_video_frame_v2_t* video,
        NDIlib_metadata_frame_t* metadata, std::uint32_t timeout_ms) noexcept = 0;
    virtual void recv_free_video(NDIlib_recv_instance_t receiver,
                                 NDIlib_video_frame_v2_t* video) noexcept = 0;
    virtual void recv_free_metadata(NDIlib_recv_instance_t receiver,
                                    NDIlib_metadata_frame_t* metadata) noexcept = 0;
    virtual void recv_performance(
        NDIlib_recv_instance_t receiver, NDIlib_recv_performance_t* total,
        NDIlib_recv_performance_t* dropped) noexcept = 0;
    virtual void recv_queue(NDIlib_recv_instance_t receiver,
                             NDIlib_recv_queue_t* queue) noexcept = 0;
    virtual clock_sync::MappingResult map_source_timestamp(
        std::int64_t timestamp,
        std::chrono::steady_clock::time_point now) const noexcept = 0;
};

std::unique_ptr<ICapture> create_ndi_capture(
    const CaptureConfig& config,
    std::unique_ptr<INdiCaptureSystem> system) noexcept;
#endif

} // namespace capture::detail

#endif // CAPTURE_NDI_INTERNAL_H
