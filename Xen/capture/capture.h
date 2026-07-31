#ifndef CAPTURE_H
#define CAPTURE_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

enum class CaptureBackend {
    DESKTOP_DUPLICATION,
};

enum class CaptureStatus {
    CLOSED,
    READY,
    FRAME,
    NO_FRAME,
    ACCESS_LOST,
    INVALID_CONFIG,
    UNSUPPORTED,
    FAILURE,
};

const char* CaptureStatusName(CaptureStatus status) noexcept;

struct CaptureConfig {
    CaptureBackend backend = CaptureBackend::DESKTOP_DUPLICATION;
    int adapter_index = 0;
    int output_index = 0;
    int roi_width = 320;
    int roi_height = 320;
    bool center_roi = true;
    int roi_x = 0;
    int roi_y = 0;
    int acquire_timeout_ms = 16;
};

struct FrameTiming {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    double capture_ms = 0.0;
};

struct CapturedFrame {
    cv::Mat bgr;
    FrameTiming timing;
    int roi_x = 0;
    int roi_y = 0;
    int source_width = 0;
    int source_height = 0;
};

class ICapture {
public:
    virtual ~ICapture() = default;

    ICapture(const ICapture&) = delete;
    ICapture& operator=(const ICapture&) = delete;

    virtual bool open() noexcept = 0;
    virtual CaptureStatus grab(CapturedFrame& frame) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual CaptureStatus status() const noexcept = 0;
    virtual std::string last_error() const = 0;

protected:
    ICapture() = default;
};

std::unique_ptr<ICapture> create_capture(const CaptureConfig& config) noexcept;

#endif // CAPTURE_H
