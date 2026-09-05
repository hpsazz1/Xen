#ifndef NDI_CAPTURE_TEST_SYSTEM_H
#define NDI_CAPTURE_TEST_SYSTEM_H

#include "capture/ndi_internal.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndi_test {

enum class MappingMode { VALID, STALE, FUTURE };

struct VideoInput {
    int width = 4;
    int height = 4;
    std::uint8_t blue = 17;
    std::int64_t timestamp = 1000000;
    std::int64_t timecode = 1234;
    int frame_rate_n = 240;
    int frame_rate_d = 1;
    MappingMode mapping = MappingMode::VALID;
    std::optional<std::string> metadata;
};

// 外部输入只提供 SDK bytes 和源机时钟映射结果，不组装 CapturedFrame，
// 不替代 XML/几何/转换/帧池等生产逻辑，也不调用任何真实 SDK 函数。
class CaptureSystem final : public capture::detail::INdiCaptureSystem {
public:
    void send_video(VideoInput input) {
        auto frame = std::make_unique<QueuedFrame>();
        frame->input = std::move(input);
        frame->pixels.assign(
            static_cast<std::size_t>(frame->input.width) *
                frame->input.height * 4U, frame->input.blue);
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(std::move(frame));
        }
        condition_.notify_one();
    }

    CaptureStatus open(const clock_sync::ClientConfig&,
                                std::string& error) noexcept override {
        error.clear();
        return CaptureStatus::READY;
    }
    void close() noexcept override {}

    NDIlib_find_instance_t find_create(
            const NDIlib_find_create_t*) noexcept override {
        return reinterpret_cast<NDIlib_find_instance_t>(this);
    }
    void find_destroy(NDIlib_find_instance_t) noexcept override {}
    bool find_wait(NDIlib_find_instance_t, std::uint32_t) noexcept override {
        return true;
    }
    const NDIlib_source_t* find_sources(
            NDIlib_find_instance_t, std::uint32_t* count) noexcept override {
        *count = 1;
        return &source_;
    }
    NDIlib_recv_instance_t recv_create(
            const NDIlib_recv_create_v3_t*) noexcept override {
        return reinterpret_cast<NDIlib_recv_instance_t>(this);
    }
    void recv_destroy(NDIlib_recv_instance_t) noexcept override {}
    int recv_connections(NDIlib_recv_instance_t) noexcept override { return 1; }

    NDIlib_frame_type_e recv_capture(
            NDIlib_recv_instance_t, NDIlib_video_frame_v2_t* video,
            NDIlib_metadata_frame_t*, std::uint32_t timeout_ms) noexcept override {
        try {
            // 不允许下一次接收覆盖尚未归还的 SDK 借用帧。
            if (active_) return NDIlib_frame_type_error;
            std::unique_lock lock(mutex_);
            if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [this] { return !pending_.empty(); })) {
                return NDIlib_frame_type_none;
            }
            active_ = std::move(pending_.front());
            pending_.pop_front();
            lock.unlock();
            const auto& input = active_->input;
            *video = {};
            video->xres = input.width;
            video->yres = input.height;
            video->FourCC = NDIlib_FourCC_type_BGRA;
            video->p_data = active_->pixels.data();
            video->line_stride_in_bytes = input.width * 4;
            video->timestamp = input.timestamp;
            video->timecode = input.timecode;
            video->frame_rate_N = input.frame_rate_n;
            video->frame_rate_D = input.frame_rate_d;
            video->p_metadata = input.metadata ? input.metadata->c_str() : nullptr;
            ++received_frames_;
            return NDIlib_frame_type_video;
        } catch (...) {
            return NDIlib_frame_type_error;
        }
    }

    void recv_free_video(NDIlib_recv_instance_t,
                         NDIlib_video_frame_v2_t*) noexcept override {
        active_.reset();
    }
    void recv_free_metadata(NDIlib_recv_instance_t,
                            NDIlib_metadata_frame_t*) noexcept override {}
    void recv_performance(
            NDIlib_recv_instance_t, NDIlib_recv_performance_t* total,
            NDIlib_recv_performance_t* dropped) noexcept override {
        *total = {};
        total->video_frames = received_frames_;
        *dropped = {};
    }
    void recv_queue(NDIlib_recv_instance_t,
                    NDIlib_recv_queue_t* queue) noexcept override {
        *queue = {};
    }
    clock_sync::MappingResult map_source_timestamp(
            std::int64_t,
            std::chrono::steady_clock::time_point now) const noexcept override {
        clock_sync::MappingResult result;
        result.status = active_->input.mapping == MappingMode::STALE
            ? clock_sync::MappingStatus::STALE : clock_sync::MappingStatus::VALID;
        result.valid = active_->input.mapping != MappingMode::STALE;
        result.local_time = active_->input.mapping == MappingMode::FUTURE
            ? now + std::chrono::milliseconds(5)
            : now - std::chrono::milliseconds(5);
        result.uncertainty_ms = 0.25;
        result.round_trip_ms = 0.5;
        result.clock_rate = 1.0001;
        result.mapping_age_ms = 2.0;
        result.sample_count = 8;
        result.source_session_id = 73;
        return result;
    }

private:
    struct QueuedFrame {
        VideoInput input;
        std::vector<std::uint8_t> pixels;
    };
    NDIlib_source_t source_{"Xen contract fixture", "fixture://ndi"};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::unique_ptr<QueuedFrame>> pending_;
    std::unique_ptr<QueuedFrame> active_;
    std::int64_t received_frames_ = 0;
};

inline CaptureConfig config() {
    CaptureConfig result;
    result.backend = CaptureBackend::NDI;
    result.roi_width = 4;
    result.roi_height = 4;
    result.ndi_receive_timeout_ms = 10;
    result.acquire_timeout_ms = 100;
    return result;
}

inline bool receive(ICapture& capture,
                     CapturedFrame& frame) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = capture.grab(frame);
        if (status == CaptureStatus::FRAME) return true;
        if (status != CaptureStatus::NO_FRAME &&
            status != CaptureStatus::READY) return false;
    }
    return false;
}

inline void release(CapturedFrame& frame) {
    frame.bgr.release();
    frame.bgr_storage.reset();
}

} // namespace ndi_test

#endif // NDI_CAPTURE_TEST_SYSTEM_H
