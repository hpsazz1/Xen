#ifndef CAPTURE_NETWORK_INTERNAL_H
#define CAPTURE_NETWORK_INTERNAL_H

#include "capture/capture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace capture::detail {

struct NetworkDecodedFrame {
    cv::Mat bgr;
    FrameTiming timing;
    double roi_x = 0.0;
    double roi_y = 0.0;
    int source_width = 0;
    int source_height = 0;
    int encoded_width = 0;
    int encoded_height = 0;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
};

struct NetworkFrameGeometry {
    int encoded_width = 0;
    int encoded_height = 0;
    int decoded_roi_x = 0;
    int decoded_roi_y = 0;
    int decoded_roi_width = 0;
    int decoded_roi_height = 0;
    double source_roi_x = 0.0;
    double source_roi_y = 0.0;
    int source_width = 0;
    int source_height = 0;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
    // 仅表示本次采用 Xen XML metadata 支路，配置 fallback 成功不会置位。
    bool metadata_applied = false;
};

struct NetworkGeometryConfig {
    NetworkFrameLayout layout = NetworkFrameLayout::FULL_FRAME_1_TO_1;
    int source_width = 0;
    int source_height = 0;
    int roi_width = 320;
    int roi_height = 320;
    bool center_roi = true;
    int roi_x = 0;
    int roi_y = 0;
};

struct XenFrameMetadata {
    int source_width = 0;
    int source_height = 0;
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
};

// 网络采集线程与 Runtime 之间只保留最新帧，固定槽位避免热路径无限增长。
class NetworkLatestFramePool {
public:
    static constexpr std::size_t kSlotCount = 5;

    NetworkLatestFramePool();

    NetworkLatestFramePool(const NetworkLatestFramePool&) = delete;
    NetworkLatestFramePool& operator=(const NetworkLatestFramePool&) = delete;

    std::shared_ptr<NetworkDecodedFrame> acquire_write() noexcept;
    void publish(const std::shared_ptr<NetworkDecodedFrame>& frame) noexcept;
    bool take_latest(std::uint64_t last_sequence,
                     CapturedFrame& frame) noexcept;
    void record_drop() noexcept;
    std::uint64_t dropped_frames() const noexcept;
    void reset() noexcept;

private:
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<NetworkDecodedFrame>, kSlotCount> pool_;
    std::shared_ptr<NetworkDecodedFrame> latest_;
    std::uint64_t consumed_sequence_ = 0;
    std::uint64_t dropped_frames_ = 0;
};

bool parse_xen_frame_metadata(std::string_view metadata,
                              XenFrameMetadata& parsed) noexcept;

bool resolve_network_frame_geometry(
    const NetworkGeometryConfig& config,
    int encoded_width,
    int encoded_height,
    NetworkFrameGeometry& geometry,
    const XenFrameMetadata* metadata = nullptr) noexcept;

} // namespace capture::detail

#endif // CAPTURE_NETWORK_INTERNAL_H
