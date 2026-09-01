#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <thread>

namespace {

constexpr std::wstring_view kPhysicalConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT";
std::atomic<bool> stop_requested{false};

void set_error(std::string& output, std::string_view value) noexcept {
    try {
        output.assign(value);
    } catch (...) {
    }
}

bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    try {
        if (input.empty()) {
            output.clear();
            return true;
        }
        if (input.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
            return false;
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0) return false;
        output.resize(static_cast<std::size_t>(required));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                   static_cast<int>(input.size()), output.data(), required,
                   nullptr, nullptr) == required;
    } catch (...) {
        return false;
    }
}

template<typename Integer>
bool parse_integer(std::wstring_view input, Integer& output) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8) || utf8.empty()) return false;
    Integer candidate{};
    const auto [end, result] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), candidate);
    if (result != std::errc{} || end != utf8.data() + utf8.size()) {
        return false;
    }
    output = candidate;
    return true;
}

bool valid_uuid(std::string_view value) noexcept {
    if (value.size() != 36U) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return false;
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool valid_sha256(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool path_to_utf8(const std::filesystem::path& path,
                  std::string& output) noexcept {
    return wide_to_utf8(path.native(), output);
}

class SidecarWitness {
public:
    ~SidecarWitness() {
        if (process_) CloseHandle(process_);
    }

    bool open(const MouseEffectProbeRunOptions& options,
              std::string& error) noexcept {
        try {
            std::string local_hash;
            if (!mouse_effect_probe::
                    calculate_mouse_effect_probe_file_sha256(
                        options.binding_path, local_hash, error) ||
                local_hash != options.expected_binding_sha256) {
                if (error.empty()) {
                    set_error(error, "本地 probe binding SHA 不匹配");
                }
                return false;
            }
            std::error_code filesystem_error;
            if (!std::filesystem::is_directory(
                    options.sidecar_incoming_directory,
                    filesystem_error) || filesystem_error ||
                !std::filesystem::is_directory(
                    options.sidecar_incoming_directory / "frames",
                    filesystem_error) || filesystem_error) {
                set_error(error, "sidecar incoming/frames 尚未就绪");
                return false;
            }
            const auto expected_name_fragment =
                L".incoming-" + std::to_wstring(options.sidecar_pid) + L"-";
            if (options.sidecar_incoming_directory.filename().wstring().find(
                    expected_name_fragment) == std::wstring::npos) {
                set_error(error, "sidecar incoming 目录与声明 PID 不绑定");
                return false;
            }
            const auto copied_binding =
                options.sidecar_incoming_directory / "source-binding.json";
            std::string copied_hash;
            if (!mouse_effect_probe::
                    calculate_mouse_effect_probe_file_sha256(
                        copied_binding, copied_hash, error) ||
                copied_hash != options.expected_binding_sha256) {
                if (error.empty()) {
                    set_error(error, "sidecar probe binding SHA 不匹配");
                }
                return false;
            }
            process_ = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, options.sidecar_pid);
            if (!process_) {
                set_error(error, "无法打开声明的 sidecar PID，Win32Error=" +
                                 std::to_string(GetLastError()));
                return false;
            }
            incoming_directory_ = options.sidecar_incoming_directory;
            copied_binding_path_ = copied_binding;
            if (!recording(error)) return false;
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            set_error(error, std::string("验证 sidecar witness 异常: ") +
                             exception.what());
            return false;
        } catch (...) {
            set_error(error, "验证 sidecar witness 时发生未知异常");
            return false;
        }
    }

    bool recording(std::string& error) const noexcept {
        try {
            if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
                set_error(error, "sidecar 进程已退出或状态不可验证");
                return false;
            }
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(process_, &exit_code) ||
                exit_code != STILL_ACTIVE) {
                set_error(error, "sidecar PID 不再处于运行态");
                return false;
            }
            std::error_code filesystem_error;
            if (!std::filesystem::is_directory(
                    incoming_directory_, filesystem_error) ||
                filesystem_error ||
                !std::filesystem::is_regular_file(
                    copied_binding_path_, filesystem_error) ||
                filesystem_error) {
                set_error(error, "sidecar incoming/binding 在 block 中消失");
                return false;
            }
            error.clear();
            return true;
        } catch (...) {
            set_error(error, "检查 sidecar 运行态时发生未知异常");
            return false;
        }
    }

private:
    HANDLE process_ = nullptr;
    std::filesystem::path incoming_directory_;
    std::filesystem::path copied_binding_path_;
};

enum class SafetyPollResult {
    READY,
    WAITING,
    RELEASED,
    USER_STOP,
    FAILURE,
};

SafetyPollResult poll_physical_safety(
        const std::shared_ptr<IMouseController>& mouse) noexcept {
    if (!mouse) return SafetyPollResult::FAILURE;
    InputSnapshot snapshot;
    if (!mouse->poll_input(snapshot) ||
        snapshot.status == InputMonitorStatus::FAILURE ||
        snapshot.status == InputMonitorStatus::CLOSED) {
        return SafetyPollResult::FAILURE;
    }
    if (!snapshot.state_valid ||
        snapshot.status != InputMonitorStatus::READY) {
        return SafetyPollResult::WAITING;
    }
    if (snapshot.virtual_keys[0x23] || snapshot.virtual_keys[0x77]) {
        return SafetyPollResult::USER_STOP;
    }
    return snapshot.virtual_keys[0x02]
        ? SafetyPollResult::READY : SafetyPollResult::RELEASED;
}

} // namespace

MouseEffectProbeParseStatus parse_mouse_effect_probe_options(
        std::span<const std::wstring_view> arguments,
        MouseEffectProbeRunOptions& options,
        std::string& error) noexcept {
    options = {};
    try {
        bool seen_mode = false;
        bool seen_config = false;
        bool seen_sequence = false;
        bool seen_binding = false;
        bool seen_binding_sha = false;
        bool seen_sidecar_pid = false;
        bool seen_sidecar_incoming = false;
        bool seen_report = false;
        bool seen_run_uuid = false;
        bool seen_activation_epoch = false;
        bool seen_max_seconds = false;
        bool seen_allow_physical = false;
        bool seen_confirmation = false;

        const auto duplicate = [&](bool& seen, std::string_view name) {
            if (seen) {
                set_error(error, std::string(name) + " 重复");
                return true;
            }
            seen = true;
            return false;
        };
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto argument = arguments[index];
            if (argument == L"--help" || argument == L"-h") {
                error.clear();
                return MouseEffectProbeParseStatus::HELP;
            }
            if (argument == L"--allow-physical-output") {
                if (duplicate(seen_allow_physical,
                              "--allow-physical-output")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.allow_physical_output = true;
                continue;
            }
            if (index + 1U >= arguments.size()) {
                set_error(error, "参数缺少值");
                return MouseEffectProbeParseStatus::INVALID;
            }
            const auto value = arguments[++index];
            if (argument == L"--mode") {
                if (duplicate(seen_mode, "--mode")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                if (value == L"output-off-rehearsal") {
                    options.dispatch_mode = mouse_effect_probe::
                        ProbeDispatchMode::OUTPUT_OFF_REHEARSAL;
                } else if (value == L"physical-a") {
                    options.dispatch_mode =
                        mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A;
                } else {
                    set_error(error, "--mode 非法");
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--config") {
                if (duplicate(seen_config, "--config")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.config_path = std::filesystem::path(value);
            } else if (argument == L"--sequence") {
                if (duplicate(seen_sequence, "--sequence")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.sequence_path = std::filesystem::path(value);
            } else if (argument == L"--binding") {
                if (duplicate(seen_binding, "--binding")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.binding_path = std::filesystem::path(value);
            } else if (argument == L"--binding-sha256") {
                if (duplicate(seen_binding_sha, "--binding-sha256") ||
                    !wide_to_utf8(value,
                                  options.expected_binding_sha256) ||
                    !valid_sha256(options.expected_binding_sha256)) {
                    if (error.empty()) {
                        set_error(error, "--binding-sha256 非法");
                    }
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--sidecar-pid") {
                if (duplicate(seen_sidecar_pid, "--sidecar-pid") ||
                    !parse_integer(value, options.sidecar_pid) ||
                    options.sidecar_pid == 0) {
                    if (error.empty()) set_error(error, "--sidecar-pid 非法");
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--sidecar-incoming") {
                if (duplicate(seen_sidecar_incoming,
                              "--sidecar-incoming")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.sidecar_incoming_directory =
                    std::filesystem::path(value);
            } else if (argument == L"--report") {
                if (duplicate(seen_report, "--report")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.report_path = std::filesystem::path(value);
            } else if (argument == L"--run-uuid") {
                if (duplicate(seen_run_uuid, "--run-uuid") ||
                    !wide_to_utf8(value, options.run_uuid) ||
                    !valid_uuid(options.run_uuid)) {
                    if (error.empty()) set_error(error, "--run-uuid 非法");
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--activation-epoch") {
                if (duplicate(seen_activation_epoch,
                              "--activation-epoch") ||
                    !parse_integer(value, options.activation_epoch) ||
                    options.activation_epoch == 0) {
                    if (error.empty()) {
                        set_error(error, "--activation-epoch 非法");
                    }
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--max-seconds") {
                if (duplicate(seen_max_seconds, "--max-seconds") ||
                    !parse_integer(value, options.max_seconds) ||
                    options.max_seconds == 0 ||
                    options.max_seconds > 60) {
                    if (error.empty()) set_error(error, "--max-seconds 非法");
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--confirm-physical-output") {
                if (duplicate(seen_confirmation,
                              "--confirm-physical-output") ||
                    value != kPhysicalConfirmation) {
                    if (error.empty()) {
                        set_error(error, "物理输出确认令牌不匹配");
                    }
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.physical_output_confirmed = true;
            } else {
                set_error(error, "未知参数");
                return MouseEffectProbeParseStatus::INVALID;
            }
        }

        if (!seen_mode || !seen_config || !seen_sequence ||
            !seen_binding || !seen_binding_sha || !seen_sidecar_pid ||
            !seen_sidecar_incoming || !seen_report || !seen_run_uuid ||
            !seen_activation_epoch || options.config_path.empty() ||
            options.sequence_path.empty() || options.binding_path.empty() ||
            options.sidecar_incoming_directory.empty() ||
            options.report_path.empty() ||
            !options.config_path.is_absolute() ||
            !options.sequence_path.is_absolute() ||
            !options.binding_path.is_absolute() ||
            !options.sidecar_incoming_directory.is_absolute() ||
            !options.report_path.is_absolute()) {
            set_error(error, "缺少必填参数或路径不是绝对路径");
            return MouseEffectProbeParseStatus::INVALID;
        }
        if (options.dispatch_mode ==
                mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A) {
            if (!options.allow_physical_output ||
                !options.physical_output_confirmed) {
                set_error(error, "physical A 缺少双重物理输出授权");
                return MouseEffectProbeParseStatus::INVALID;
            }
        } else if (seen_allow_physical || seen_confirmation) {
            set_error(error, "output-off rehearsal 禁止物理输出授权参数");
            return MouseEffectProbeParseStatus::INVALID;
        }
        stop_requested.store(false, std::memory_order_release);
        error.clear();
        return MouseEffectProbeParseStatus::READY;
    } catch (const std::exception& exception) {
        set_error(error, std::string("解析 Mouse Effect Probe 参数异常: ") +
                         exception.what());
        return MouseEffectProbeParseStatus::INVALID;
    } catch (...) {
        set_error(error, "解析 Mouse Effect Probe 参数时发生未知异常");
        return MouseEffectProbeParseStatus::INVALID;
    }
}

std::string mouse_effect_probe_usage() {
    return
        "XenMouseEffectProbe 建立 backend-completed command → visible "
        "background response 证据；正常 Aim 输出必须关闭。\n\n"
        "output-off rehearsal 用法:\n"
        "  XenMouseEffectProbe --mode output-off-rehearsal --config <ini> "
        "--sequence <json> --binding <json> --binding-sha256 <sha256> "
        "--sidecar-pid <pid> --sidecar-incoming <dir> --report <new-json> "
        "--run-uuid <uuid> --activation-epoch <n> [--max-seconds <1..60>]\n\n"
        "physical A 额外要求:\n"
        "  --mode physical-a --allow-physical-output "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT\n"
        "physical A 会发送真实 KMBOX ±1 X 输入；只能由用户前台启动。\n";
}

bool make_mouse_effect_probe_source_frame_event(
        const FrameTiming& timing,
        bool sidecar_recording,
        bool safety_allowed,
        mouse_effect_probe::ProbeSourceFrameEvent& event,
        std::string& error) noexcept {
    event = {};
    try {
        if (timing.sequence == 0 || !timing.source_timestamp_valid ||
            timing.source_timestamp <= 0 ||
            timing.source_time_basis != SourceTimeBasis::NDI_SDK_SUBMISSION ||
            timing.source_clock_status != SourceClockStatus::VALID ||
            !timing.source_time_timing_valid ||
            timing.source_time_at.time_since_epoch().count() <= 0 ||
            !finite_nonnegative(timing.source_clock_uncertainty_ms) ||
            !finite_nonnegative(timing.source_clock_round_trip_ms) ||
            !finite_nonnegative(timing.source_clock_mapping_age_ms) ||
            timing.source_clock_sample_count == 0 ||
            timing.source_clock_session_id == 0) {
            set_error(error, "Capture frame 缺少 VALID NDI source timing");
            return false;
        }
        event.source_frame_sequence = timing.sequence;
        event.source_timestamp = timing.source_timestamp;
        event.source_timestamp_valid = timing.source_timestamp_valid;
        event.source_time_at_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                timing.source_time_at.time_since_epoch()).count();
        event.source_time_basis = SourceTimeBasisName(
            timing.source_time_basis);
        event.source_clock_status = SourceClockStatusName(
            timing.source_clock_status);
        event.source_clock_session_id = std::to_string(
            timing.source_clock_session_id);
        event.source_clock_uncertainty_ms =
            timing.source_clock_uncertainty_ms;
        event.source_clock_rtt_ms = timing.source_clock_round_trip_ms;
        event.source_clock_mapping_age_ms =
            timing.source_clock_mapping_age_ms;
        event.source_clock_sample_count =
            timing.source_clock_sample_count;
        event.source_dropped_frames = timing.source_dropped_frames;
        event.transport_dropped_frames =
            timing.transport_dropped_frames;
        event.transport_invalid_packets =
            timing.transport_invalid_packets;
        event.source_timing_valid = true;
        event.sidecar_recording = sidecar_recording;
        event.safety_allowed = safety_allowed;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        event = {};
        set_error(error, std::string("映射 probe source frame 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        event = {};
        set_error(error, "映射 probe source frame 时发生未知异常");
        return false;
    }
}

void request_mouse_effect_probe_stop() noexcept {
    stop_requested.store(true, std::memory_order_release);
}

bool run_mouse_effect_probe(
        const MouseEffectProbeRunOptions& options,
        MouseEffectProbeRunResult& result,
        std::string& error) noexcept {
    result = {};
    stop_requested.store(false, std::memory_order_release);
    try {
        if (std::filesystem::exists(options.report_path)) {
            set_error(error, "command report 已存在，拒绝开始 probe");
            return false;
        }
        SidecarWitness sidecar;
        if (!sidecar.open(options, error)) return false;

        mouse_effect_probe::MouseEffectProbeSequence sequence;
        if (!mouse_effect_probe::read_mouse_effect_probe_sequence(
                options.sequence_path, sequence, error)) {
            return false;
        }
        AppConfig app_config;
        std::string config_path;
        if (!path_to_utf8(options.config_path, config_path) ||
            !load_app_config(config_path, app_config, error)) {
            if (error.empty()) set_error(error, "无法读取 probe config.ini");
            return false;
        }
        if (app_config.capture.backend != CaptureBackend::NDI ||
            app_config.capture.ndi_source_name.empty() ||
            app_config.capture.ndi_source_name == "Auto" ||
            app_config.capture.ndi_clock_sync_url.empty()) {
            set_error(error,
                "probe 只接受精确 NDI source 与独立 clock sync 配置");
            return false;
        }

        mouse_effect_probe::ProbeExecutionOptions execution_options;
        execution_options.run_uuid = options.run_uuid;
        execution_options.activation_epoch = options.activation_epoch;
        execution_options.dispatch_mode = options.dispatch_mode;
        execution_options.allow_physical_output =
            options.allow_physical_output;
        execution_options.physical_output_confirmed =
            options.physical_output_confirmed;
        execution_options.require_protocol_ack = true;

        MouseOutputOwnerLease rehearsal_owner_guard;
        std::shared_ptr<IMouseController> mouse;
        if (options.dispatch_mode ==
                mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A) {
            if (app_config.mouse.backend != MouseBackend::KMBOX_NET) {
                set_error(error, "physical A 当前只接受 KMBOX NET backend");
                return false;
            }
            app_config.mouse.allow_send_input = true;
            auto owned_mouse = MouseDeviceFactory::create(
                app_config.mouse, options.owner_scope);
            mouse = std::shared_ptr<IMouseController>(
                std::move(owned_mouse));
        } else if (!rehearsal_owner_guard.acquire(
                       options.owner_scope,
                       "mouse-effect-probe-output-off", error)) {
            set_error(error,
                "output-off rehearsal 无法证明正常 Aim/Mouse owner 已关闭");
            return false;
        }

        mouse_effect_probe::MouseEffectProbeExecutor executor;
        std::string execution_error;
        const bool started = executor.start(
            execution_options, sequence, mouse, execution_error);

        const mouse_effect_probe::ProbeEvidenceBinding report_binding{
            options.expected_binding_sha256,
            options.run_uuid,
            app_config.capture.ndi_source_name,
        };
        const auto publish_report = [&]() {
            result.execution = executor.result();
            std::string report_error;
            if (!mouse_effect_probe::write_mouse_effect_probe_report(
                    options.report_path, execution_options, sequence,
                    report_binding, result.execution,
                    result.report_sha256, report_error)) {
                if (execution_error.empty()) {
                    execution_error = "command report 发布失败: " +
                        report_error;
                } else {
                    execution_error += "; command report 发布失败: " +
                        report_error;
                }
                return false;
            }
            return true;
        };
        if (!started) {
            publish_report();
            error = execution_error;
            return false;
        }

        if (options.dispatch_mode ==
                mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A) {
            const auto arming_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            bool armed = false;
            while (std::chrono::steady_clock::now() < arming_deadline) {
                if (stop_requested.load(std::memory_order_acquire)) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::USER_STOP,
                        execution_error);
                    break;
                }
                if (!sidecar.recording(execution_error)) {
                    const std::string sidecar_error = execution_error;
                    std::string stop_error;
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::
                            SIDECAR_UNAVAILABLE,
                        stop_error);
                    execution_error = sidecar_error;
                    if (!stop_error.empty()) {
                        execution_error += "; stop 失败: " + stop_error;
                    }
                    break;
                }
                const auto safety = poll_physical_safety(mouse);
                if (safety == SafetyPollResult::READY) {
                    armed = true;
                    break;
                }
                if (safety == SafetyPollResult::USER_STOP) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::USER_STOP,
                        execution_error);
                    break;
                }
                if (safety == SafetyPollResult::FAILURE) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::MOUSE_FAILURE,
                        execution_error);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!armed && executor.result().state ==
                    mouse_effect_probe::ProbeExecutionState::RUNNING) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::SAFETY_RELEASED,
                    execution_error);
                if (execution_error.empty()) {
                    execution_error = "deadman 未在有界武装窗内进入 READY";
                }
            }
            if (!armed) {
                publish_report();
                error = execution_error.empty()
                    ? "physical A 未完成 deadman 武装" : execution_error;
                return false;
            }
        }

        auto capture_config = app_config.capture;
        capture_config.acquire_timeout_ms = 2;
        auto capture = create_capture(capture_config);
        if (!capture || !capture->open()) {
            executor.request_stop(
                mouse_effect_probe::ProbeStopReason::SOURCE_TIMING_INVALID,
                execution_error);
            if (execution_error.empty()) {
                execution_error = capture
                    ? "probe Capture 打开失败: " + capture->last_error()
                    : "probe Capture factory 返回空";
            }
            publish_report();
            error = execution_error;
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(options.max_seconds);
        while (executor.result().state ==
                   mouse_effect_probe::ProbeExecutionState::RUNNING) {
            if (stop_requested.load(std::memory_order_acquire)) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::USER_STOP,
                    execution_error);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::RUN_TIMEOUT,
                    execution_error);
                execution_error = "probe 超过预注册 max-seconds";
                break;
            }
            if (!sidecar.recording(execution_error)) {
                const std::string sidecar_error = execution_error;
                std::string stop_error;
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::SIDECAR_UNAVAILABLE,
                    stop_error);
                execution_error = sidecar_error;
                if (!stop_error.empty()) {
                    execution_error += "; stop 失败: " + stop_error;
                }
                break;
            }

            bool safety_allowed = false;
            if (options.dispatch_mode ==
                    mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A) {
                const auto safety = poll_physical_safety(mouse);
                if (safety == SafetyPollResult::USER_STOP) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::USER_STOP,
                        execution_error);
                    break;
                }
                if (safety == SafetyPollResult::RELEASED ||
                    safety == SafetyPollResult::WAITING) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::SAFETY_RELEASED,
                        execution_error);
                    break;
                }
                if (safety == SafetyPollResult::FAILURE) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::MOUSE_FAILURE,
                        execution_error);
                    break;
                }
                safety_allowed = true;
            }

            CapturedFrame frame;
            const auto capture_status = capture->grab(frame);
            if (capture_status == CaptureStatus::NO_FRAME ||
                capture_status == CaptureStatus::READY) {
                continue;
            }
            if (capture_status != CaptureStatus::FRAME) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::
                        SOURCE_TIMING_INVALID,
                    execution_error);
                execution_error = "probe Capture 终止: " +
                    std::string(CaptureStatusName(capture_status)) + ": " +
                    capture->last_error();
                break;
            }

            mouse_effect_probe::ProbeSourceFrameEvent source_event;
            std::string frame_error;
            if (!make_mouse_effect_probe_source_frame_event(
                    frame.timing, true, safety_allowed,
                    source_event, frame_error)) {
                // Probe 尚未消费任何 sample 时只等待 source clock READY；
                // 一旦 block 开始，任何失效都立即终止整个 block。
                if (executor.result().consumed_sample_count == 0) continue;
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::
                        SOURCE_TIMING_INVALID,
                    execution_error);
                execution_error = frame_error;
                break;
            }
            if (!executor.consume_source_frame(source_event, frame_error)) {
                execution_error = frame_error;
                break;
            }
        }
        capture->close();
        const bool report_published = publish_report();
        const bool completed = result.execution.state ==
            mouse_effect_probe::ProbeExecutionState::COMPLETED;
        if (completed && report_published) {
            error.clear();
            return true;
        }
        if (execution_error.empty()) {
            execution_error = "probe 未正常完成: " + std::string(
                mouse_effect_probe::probe_stop_reason_name(
                    result.execution.stop_reason));
        }
        error = execution_error;
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("运行 Mouse Effect Probe 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        set_error(error, "运行 Mouse Effect Probe 时发生未知异常");
        return false;
    }
}
