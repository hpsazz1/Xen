#ifndef CAPTURE_NDI_INTERNAL_H
#define CAPTURE_NDI_INTERNAL_H

#include "capture/capture.h"
#include "capture/network_internal.h"

#include <memory>

namespace capture::detail {

std::unique_ptr<ICapture> create_ndi_capture(
    const CaptureConfig& config) noexcept;

} // namespace capture::detail

#endif // CAPTURE_NDI_INTERNAL_H
