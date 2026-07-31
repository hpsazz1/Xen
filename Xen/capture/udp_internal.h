#ifndef CAPTURE_UDP_INTERNAL_H
#define CAPTURE_UDP_INTERNAL_H

#include "capture/capture.h"
#include "capture/network_internal.h"

#include <cstdint>
#include <memory>
#include <string>

namespace capture::detail {

using UdpDecodedFrame = NetworkDecodedFrame;
using UdpFrameGeometry = NetworkFrameGeometry;
using UdpLatestFramePool = NetworkLatestFramePool;

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
