#ifndef MOUSE_EFFECT_PROBE_RUNNER_H
#define MOUSE_EFFECT_PROBE_RUNNER_H

#include "capture/capture.h"
#include "mouse_effect_probe/mouse_effect_probe.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

enum class MouseEffectProbeParseStatus {
    READY,
    HELP,
    INVALID,
};

struct MouseEffectProbeRunOptions {
    mouse_effect_probe::ProbeDispatchMode dispatch_mode =
        mouse_effect_probe::ProbeDispatchMode::OUTPUT_OFF_REHEARSAL;
    std::filesystem::path config_path;
    std::filesystem::path sequence_path;
    std::filesystem::path binding_path;
    std::string expected_binding_sha256;
    std::uint32_t sidecar_pid = 0;
    std::filesystem::path sidecar_incoming_directory;
    std::filesystem::path report_path;
    std::string run_uuid;
    std::uint64_t activation_epoch = 0;
    std::uint64_t max_seconds = 15;
    bool allow_physical_output = false;
    bool physical_output_confirmed = false;
    MouseOutputOwnerScope owner_scope = MouseOutputOwnerScope::PRODUCTION;
};

struct MouseEffectProbeRunResult {
    mouse_effect_probe::ProbeExecutionResult execution;
    std::string report_sha256;
};

MouseEffectProbeParseStatus parse_mouse_effect_probe_options(
    std::span<const std::wstring_view> arguments,
    MouseEffectProbeRunOptions& options,
    std::string& error) noexcept;

std::string mouse_effect_probe_usage();

// KMBOX monitor 仅在物理输入变化时发布新事实；因此 Physical A 必须
// 在 monitor 打开后提示用户产生一次新的右键按下，不能要求提前按住。
std::string_view mouse_effect_probe_deadman_arming_prompt() noexcept;

bool make_mouse_effect_probe_source_frame_event(
    const FrameTiming& timing,
    bool sidecar_recording,
    bool safety_allowed,
    mouse_effect_probe::ProbeSourceFrameEvent& event,
    std::string& error) noexcept;

bool run_mouse_effect_probe(
    const MouseEffectProbeRunOptions& options,
    MouseEffectProbeRunResult& result,
    std::string& error) noexcept;

void request_mouse_effect_probe_stop() noexcept;

#endif // MOUSE_EFFECT_PROBE_RUNNER_H
