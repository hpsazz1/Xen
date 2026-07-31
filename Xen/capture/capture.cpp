#include "capture/capture.h"

#include "capture/udp_internal.h"

#include "log/log.h"

#include <memory>

namespace capture::detail {

std::unique_ptr<ICapture> create_desktop_duplication_capture(
    const CaptureConfig& config) noexcept;

} // namespace capture::detail

const char* CaptureStatusName(CaptureStatus status) noexcept {
    switch (status) {
        case CaptureStatus::CLOSED: return "CLOSED";
        case CaptureStatus::READY: return "READY";
        case CaptureStatus::FRAME: return "FRAME";
        case CaptureStatus::NO_FRAME: return "NO_FRAME";
        case CaptureStatus::ACCESS_LOST: return "ACCESS_LOST";
        case CaptureStatus::INVALID_CONFIG: return "INVALID_CONFIG";
        case CaptureStatus::UNSUPPORTED: return "UNSUPPORTED";
        case CaptureStatus::FAILURE: return "FAILURE";
    }
    return "UNKNOWN";
}

const char* CaptureBackendName(CaptureBackend backend) noexcept {
    switch (backend) {
        case CaptureBackend::DESKTOP_DUPLICATION:
            return "DESKTOP_DUPLICATION";
        case CaptureBackend::UDP_MJPEG:
            return "UDP_MJPEG";
    }
    return "UNKNOWN";
}

const char* UdpFrameLayoutName(UdpFrameLayout layout) noexcept {
    switch (layout) {
        case UdpFrameLayout::FULL_FRAME_1_TO_1:
            return "FULL_FRAME_1_TO_1";
        case UdpFrameLayout::FULL_FRAME_SCALED:
            return "FULL_FRAME_SCALED";
        case UdpFrameLayout::CENTER_CROP_1_TO_1:
            return "CENTER_CROP_1_TO_1";
    }
    return "UNKNOWN";
}

std::unique_ptr<ICapture> create_capture(
        const CaptureConfig& config) noexcept {
    try {
        switch (config.backend) {
            case CaptureBackend::DESKTOP_DUPLICATION:
                return capture::detail::create_desktop_duplication_capture(
                    config);
            case CaptureBackend::UDP_MJPEG:
                return capture::detail::create_udp_mjpeg_capture(config);
        }
    } catch (...) {
        LOG_ERROR("capture", "创建采集后端时发生未知异常");
    }
    return nullptr;
}
