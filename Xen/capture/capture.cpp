#include "capture/capture.h"

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

std::unique_ptr<ICapture> create_capture(
        const CaptureConfig& config) noexcept {
    try {
        if (config.backend == CaptureBackend::DESKTOP_DUPLICATION) {
            return capture::detail::create_desktop_duplication_capture(config);
        }
    } catch (...) {
        LOG_ERROR("capture", "创建采集后端时发生未知异常");
    }
    return nullptr;
}
