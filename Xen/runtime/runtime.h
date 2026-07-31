#ifndef RUNTIME_H
#define RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>

#include "aim/aim.h"
#include "capture/capture.h"
#include "config/config.h"
#include "detector/detector.h"
#include "mouse/mouse.h"

enum class RuntimeState {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING,
    FAILED,
};

const char* RuntimeStateName(RuntimeState state) noexcept;

enum class RuntimeIntentType {
    START,
    STOP,
    ARM_OUTPUT,
    DISARM_OUTPUT,
    AIM_HOLD_CHANGED,
    EMERGENCY_STOP,
    RESET_EMERGENCY,
};

struct RuntimeIntent {
    RuntimeIntentType type = RuntimeIntentType::DISARM_OUTPUT;
    bool active = false;
};

struct PipelineProfile {
    double capture_ms = 0.0;
    double queue_ms = 0.0;
    InferenceProfile detector;
    AimProfile aim;
    double mouse_ms = 0.0;
    double total_ms = 0.0;
};

struct RuntimeSnapshot {
    RuntimeState state = RuntimeState::STOPPED;
    CaptureStatus capture_status = CaptureStatus::CLOSED;
    DetectionStatus detection_status = DetectionStatus::NOT_RUN;
    AimStatus aim_status = AimStatus::NOT_RUN;
    MouseStatus mouse_status = MouseStatus::CLOSED;
    std::string provider;
    std::string last_error;
    std::uint64_t captured_frames = 0;
    std::uint64_t processed_frames = 0;
    std::uint64_t failed_frames = 0;
    std::uint64_t overwritten_frames = 0;
    std::uint64_t mouse_commands = 0;
    std::uint64_t last_sequence = 0;
    double capture_fps = 0.0;
    double pipeline_p50_ms = 0.0;
    double pipeline_p95_ms = 0.0;
    PipelineProfile last_profile;
    bool output_allowed_by_config = false;
    bool output_armed = false;
    bool aim_hold_active = false;
    bool emergency_stopped = false;
    AimResult last_aim;
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start(const AppConfig& config) noexcept;
    void stop() noexcept;
    bool post_intent(const RuntimeIntent& intent) noexcept;
    void poll_keyboard() noexcept;
    RuntimeSnapshot snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // RUNTIME_H
