#ifndef MOUSE_EFFECT_PROBE_RUNNER_H
#define MOUSE_EFFECT_PROBE_RUNNER_H

#include "capture/capture.h"
#include "mouse_effect_probe/mouse_effect_probe.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class MouseEffectProbeParseStatus {
    READY,
    HELP,
    INVALID,
};

enum class MouseEffectProbeSafetyPhase {
    ARMING,
    ACTIVE,
};

enum class MouseEffectProbeSafetyDecision {
    READY,
    WAITING,
    RELEASED,
    USER_STOP,
    FAILURE,
};

struct MouseEffectProbeSafetyObservation {
    std::int64_t observed_at_steady_ns = 0;
    MouseEffectProbeSafetyPhase phase =
        MouseEffectProbeSafetyPhase::ARMING;
    bool poll_succeeded = false;
    InputMonitorStatus monitor_status = InputMonitorStatus::CLOSED;
    bool state_valid = false;
    std::uint64_t monitor_sequence = 0;
    bool right_button_pressed = false;
    bool end_pressed = false;
    bool f8_pressed = false;
    MouseEffectProbeSafetyDecision decision =
        MouseEffectProbeSafetyDecision::FAILURE;
};

struct MouseEffectProbeSafetyLedger {
    std::vector<MouseEffectProbeSafetyObservation> observations;
    std::uint64_t dropped_observation_count = 0;
    bool recording_failed = false;
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
    std::filesystem::path safety_ledger_path;
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
    MouseEffectProbeSafetyLedger safety_ledger;
    std::string safety_ledger_sha256;
};

MouseEffectProbeParseStatus parse_mouse_effect_probe_options(
    std::span<const std::wstring_view> arguments,
    MouseEffectProbeRunOptions& options,
    std::string& error) noexcept;

std::string mouse_effect_probe_usage();

// KMBOX monitor 仅在物理输入变化时发布新事实；因此 Physical A 必须
// 在 monitor 打开后提示用户产生一次新的右键按下，不能要求提前按住。
std::string_view mouse_effect_probe_deadman_arming_prompt() noexcept;

// 将原始 monitor 快照与安全判定共同记录；只压缩完全相同的高频 poll，
// 不改变 deadman 的 fail-closed 决策。
MouseEffectProbeSafetyDecision record_mouse_effect_probe_safety_observation(
    MouseEffectProbeSafetyPhase phase,
    bool poll_succeeded,
    const InputSnapshot& snapshot,
    MouseEffectProbeSafetyLedger& ledger) noexcept;

// 账本不具物理输出能力；拒绝覆盖并在同目录原子发布。
bool write_mouse_effect_probe_safety_ledger(
    const std::filesystem::path& path,
    std::string_view run_uuid,
    mouse_effect_probe::ProbeStopReason stop_reason,
    const MouseEffectProbeSafetyLedger& ledger,
    std::string& file_sha256,
    std::string& error) noexcept;

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
