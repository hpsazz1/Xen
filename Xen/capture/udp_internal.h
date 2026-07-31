#ifndef CAPTURE_UDP_INTERNAL_H
#define CAPTURE_UDP_INTERNAL_H

#include "capture/capture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace capture::detail {

struct UdpDecodedFrame {
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

struct UdpFrameGeometry {
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
};

// UDP 解码线程与 Runtime Capture 线程之间只保留最新帧。五个槽覆盖
// Runtime 最多持有的三个旧帧、一个已发布最新帧和一个正在写入帧。
class UdpLatestFramePool {
public:
    static constexpr std::size_t kSlotCount = 5;

    UdpLatestFramePool();

    UdpLatestFramePool(const UdpLatestFramePool&) = delete;
    UdpLatestFramePool& operator=(const UdpLatestFramePool&) = delete;

    std::shared_ptr<UdpDecodedFrame> acquire_write() noexcept;
    void publish(const std::shared_ptr<UdpDecodedFrame>& frame) noexcept;
    bool take_latest(std::uint64_t last_sequence,
                     CapturedFrame& frame) noexcept;
    void record_drop() noexcept;
    std::uint64_t dropped_frames() const noexcept;
    void reset() noexcept;

private:
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<UdpDecodedFrame>, kSlotCount> pool_;
    std::shared_ptr<UdpDecodedFrame> latest_;
    std::uint64_t consumed_sequence_ = 0;
    std::uint64_t dropped_frames_ = 0;
};

bool parse_udp_url(const std::string& url,
                   std::string& bind_address,
                   std::uint16_t& port) noexcept;

bool resolve_udp_frame_geometry(const CaptureConfig& config,
                                int encoded_width,
                                int encoded_height,
                                UdpFrameGeometry& geometry) noexcept;

std::unique_ptr<ICapture> create_udp_mjpeg_capture(
    const CaptureConfig& config) noexcept;

} // namespace capture::detail

#endif // CAPTURE_UDP_INTERNAL_H
