#include "capture/capture.h"

#include "capture/ndi_internal.h"
#include "capture/udp_internal.h"
#include "capture/xudp_internal.h"

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
        case CaptureBackend::XUDP_JPEG:
            return "XUDP_JPEG";
        case CaptureBackend::NDI:
            return "NDI";
    }
    return "UNKNOWN";
}

const char* UdpFrameLayoutName(UdpFrameLayout layout) noexcept {
    return NetworkFrameLayoutName(layout);
}

const char* NetworkFrameLayoutName(NetworkFrameLayout layout) noexcept {
    switch (layout) {
        case NetworkFrameLayout::FULL_FRAME_1_TO_1:
            return "FULL_FRAME_1_TO_1";
        case NetworkFrameLayout::FULL_FRAME_SCALED:
            return "FULL_FRAME_SCALED";
        case NetworkFrameLayout::CENTER_CROP_1_TO_1:
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
            case CaptureBackend::XUDP_JPEG:
                return capture::detail::create_xudp_capture(config);
            case CaptureBackend::NDI:
                return capture::detail::create_ndi_capture(config);
        }
    } catch (...) {
        LOG_ERROR("capture", "创建采集后端时发生未知异常");
    }
    return nullptr;
}
