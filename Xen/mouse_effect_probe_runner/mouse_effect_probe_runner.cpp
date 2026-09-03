#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr std::wstring_view kPhysicalAConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT";
constexpr std::size_t kMaximumSafetyObservationCount = 8192U;
constexpr std::size_t kMaximumMonitorPacketCount = 8192U;
std::atomic<bool> stop_requested{false};

bool is_physical_dispatch(
        mouse_effect_probe::ProbeDispatchMode mode) noexcept {
    return mode == mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A ||
           mode == mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B;
}

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

bool sha256_payload(std::span<const std::uint8_t> payload,
                    std::string& output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        if (payload.size() > std::numeric_limits<ULONG>::max()) return false;
        DWORD object_bytes = 0;
        DWORD digest_bytes = 0;
        DWORD copied = 0;
        const auto succeeded = [](NTSTATUS status) noexcept {
            return status >= 0;
        };
        if (!succeeded(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !succeeded(BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes),
                sizeof(object_bytes), &copied, 0)) ||
            copied != sizeof(object_bytes) || object_bytes == 0 ||
            !succeeded(BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_bytes),
                sizeof(digest_bytes), &copied, 0)) ||
            copied != sizeof(digest_bytes) || digest_bytes != 32U) {
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        std::vector<std::uint8_t> object(object_bytes);
        std::array<std::uint8_t, 32> digest{};
        if (!succeeded(BCryptCreateHash(
                algorithm, &hash, object.data(), object_bytes,
                nullptr, 0, 0)) ||
            (!payload.empty() && !succeeded(BCryptHashData(
                hash, const_cast<PUCHAR>(payload.data()),
                static_cast<ULONG>(payload.size()), 0))) ||
            !succeeded(BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        constexpr char hexadecimal[] = "0123456789abcdef";
        output.clear();
        output.reserve(digest.size() * 2U);
        for (const auto byte : digest) {
            output.push_back(hexadecimal[byte >> 4U]);
            output.push_back(hexadecimal[byte & 0x0fU]);
        }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return true;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        output.clear();
        return false;
    }
}

const char* input_monitor_status_name(InputMonitorStatus status) noexcept {
    switch (status) {
    case InputMonitorStatus::CLOSED: return "CLOSED";
    case InputMonitorStatus::UNVERIFIED: return "UNVERIFIED";
    case InputMonitorStatus::WAITING: return "WAITING";
    case InputMonitorStatus::READY: return "READY";
    case InputMonitorStatus::STALE: return "STALE";
    case InputMonitorStatus::FAILURE: return "FAILURE";
    }
    return "UNKNOWN";
}

const char* safety_phase_name(
        MouseEffectProbeSafetyPhase phase) noexcept {
    switch (phase) {
    case MouseEffectProbeSafetyPhase::ARMING: return "arming";
    case MouseEffectProbeSafetyPhase::ACTIVE: return "active";
    }
    return "unknown";
}

const char* safety_decision_name(
        MouseEffectProbeSafetyDecision decision) noexcept {
    switch (decision) {
    case MouseEffectProbeSafetyDecision::READY: return "ready";
    case MouseEffectProbeSafetyDecision::WAITING: return "waiting";
    case MouseEffectProbeSafetyDecision::RELEASED: return "released";
    case MouseEffectProbeSafetyDecision::USER_STOP: return "user_stop";
    case MouseEffectProbeSafetyDecision::FAILURE: return "failure";
    }
    return "unknown";
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

class PhysicalKmboxMonitorPacketObserver final
        : public mouse::detail::IKmboxMonitorPacketObserver {
public:
    PhysicalKmboxMonitorPacketObserver() {
        packet_records_.reserve(kMaximumMonitorPacketCount);
    }

    void observe_kmbox_monitor_packet(
            const mouse::detail::KmboxMonitorPacketObservation& observation,
            std::span<const std::uint8_t> payload) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (observation.datagram_size != payload.size() ||
            payload.size() > BufferedPacketRecord::kMaximumPayloadBytes) {
            recording_failed_ = true;
            return;
        }
        BufferedPacketRecord record;
        record.observation = observation;
        record.payload_size = payload.size();
        std::copy(payload.begin(), payload.end(), record.payload.begin());
        if (packet_records_.size() < kMaximumMonitorPacketCount) {
            packet_records_.push_back(std::move(record));
        } else {
            ++dropped_packet_count_;
            packet_records_.back() = std::move(record);
        }
    }

    void copy_to(MouseEffectProbeSafetyLedger& ledger) noexcept {
        try {
            std::vector<BufferedPacketRecord> packet_records;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                packet_records = std::move(packet_records_);
                ledger.dropped_monitor_packet_count =
                    dropped_packet_count_;
                ledger.monitor_packet_recording_failed = recording_failed_;
            }
            ledger.monitor_packets.clear();
            ledger.monitor_packets.reserve(packet_records.size());
            for (const auto& record : packet_records) {
                if (!record_mouse_effect_probe_monitor_packet_identity(
                        record.observation,
                        std::span<const std::uint8_t>(
                            record.payload.data(), record.payload_size),
                        ledger)) {
                    ledger.monitor_packet_recording_failed = true;
                    break;
                }
            }
        } catch (...) {
            ledger.monitor_packet_recording_failed = true;
        }
    }

private:
    struct BufferedPacketRecord {
        static constexpr std::size_t kMaximumPayloadBytes = 1024U;
        mouse::detail::KmboxMonitorPacketObservation observation;
        std::array<std::uint8_t, kMaximumPayloadBytes> payload{};
        std::size_t payload_size = 0;
    };

    std::mutex mutex_;
    std::vector<BufferedPacketRecord> packet_records_;
    std::uint64_t dropped_packet_count_ = 0;
    bool recording_failed_ = false;
};

MouseEffectProbeSafetyDecision poll_physical_safety(
        const std::shared_ptr<IMouseController>& mouse,
        MouseEffectProbeSafetyPhase phase,
        MouseEffectProbeSafetyLedger& ledger) noexcept {
    InputSnapshot snapshot;
    const bool poll_succeeded = mouse && mouse->poll_input(snapshot);
    return record_mouse_effect_probe_safety_observation(
        phase, poll_succeeded, snapshot, ledger);
}

} // namespace

std::string_view mouse_effect_probe_deadman_arming_prompt() noexcept {
    return "KMBOX monitor 已就绪；不要提前按住。请在 5 秒内按住右键并"
           "持续保持，直到出现“Mouse Effect Probe 时间线完成”或"
           "“Mouse Effect Probe 未正常完成”后再松开；sidecar publishing "
           "不是松键信号。";
}

MouseEffectProbeSafetyDecision record_mouse_effect_probe_safety_observation(
        MouseEffectProbeSafetyPhase phase,
        bool poll_succeeded,
        const InputSnapshot& snapshot,
        MouseEffectProbeSafetyLedger& ledger) noexcept {
    MouseEffectProbeSafetyDecision decision =
        MouseEffectProbeSafetyDecision::FAILURE;
    if (poll_succeeded &&
        snapshot.status != InputMonitorStatus::FAILURE &&
        snapshot.status != InputMonitorStatus::CLOSED) {
        if (!snapshot.state_valid ||
            snapshot.status != InputMonitorStatus::READY) {
            decision = MouseEffectProbeSafetyDecision::WAITING;
        } else if (snapshot.virtual_keys[0x23] ||
                   snapshot.virtual_keys[0x77]) {
            decision = MouseEffectProbeSafetyDecision::USER_STOP;
        } else {
            decision = snapshot.virtual_keys[0x02]
                ? MouseEffectProbeSafetyDecision::READY
                : MouseEffectProbeSafetyDecision::RELEASED;
        }
    }

    try {
        MouseEffectProbeSafetyObservation observation;
        observation.observed_at_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        observation.phase = phase;
        observation.poll_succeeded = poll_succeeded;
        observation.monitor_status = snapshot.status;
        observation.state_valid = snapshot.state_valid;
        observation.monitor_sequence = snapshot.sequence;
        observation.right_button_pressed = snapshot.virtual_keys[0x02];
        observation.end_pressed = snapshot.virtual_keys[0x23];
        observation.f8_pressed = snapshot.virtual_keys[0x77];
        observation.decision = decision;

        const auto same_as_last = [&](const auto& previous) {
            return previous.phase == observation.phase &&
                   previous.poll_succeeded == observation.poll_succeeded &&
                   previous.monitor_status == observation.monitor_status &&
                   previous.state_valid == observation.state_valid &&
                   previous.monitor_sequence == observation.monitor_sequence &&
                   previous.right_button_pressed ==
                       observation.right_button_pressed &&
                   previous.end_pressed == observation.end_pressed &&
                   previous.f8_pressed == observation.f8_pressed &&
                   previous.decision == observation.decision;
        };
        if (!ledger.observations.empty() &&
            same_as_last(ledger.observations.back())) {
            return decision;
        }
        if (ledger.observations.size() < kMaximumSafetyObservationCount) {
            ledger.observations.push_back(observation);
        } else {
            ++ledger.dropped_observation_count;
            ledger.observations.back() = observation;
        }
    } catch (...) {
        ledger.recording_failed = true;
    }
    return decision;
}

bool record_mouse_effect_probe_monitor_packet_identity(
        const mouse::detail::KmboxMonitorPacketObservation& observation,
        std::span<const std::uint8_t> payload,
        MouseEffectProbeSafetyLedger& ledger) noexcept {
    try {
        if (observation.datagram_size != payload.size()) {
            ledger.monitor_packet_recording_failed = true;
            return false;
        }
        MouseEffectProbeMonitorPacketIdentity identity;
        identity.received_at_steady_ns = observation.received_at_steady_ns;
        identity.datagram_size = observation.datagram_size;
        identity.source_address_size = observation.source_address_size;
        identity.source_family = observation.source_family;
        identity.source_endpoint_valid = observation.source_endpoint_valid;
        identity.source_ipv4 = observation.source_ipv4;
        identity.source_port = observation.source_port;
        identity.monitor_local_port = observation.monitor_local_port;
        identity.configured_device_ipv4 =
            observation.configured_device_ipv4;
        identity.configured_device_port =
            observation.configured_device_port;
        identity.source_ip_matches_configured_device =
            observation.source_ip_matches_configured_device;
        identity.source_port_matches_configured_device =
            observation.source_port_matches_configured_device;
        identity.exact_monitor_packet_size =
            observation.exact_monitor_packet_size;
        identity.mouse_report_id_present =
            observation.mouse_report_id_present;
        identity.mouse_report_id = observation.mouse_report_id;
        identity.mouse_buttons_present = observation.mouse_buttons_present;
        identity.mouse_buttons = observation.mouse_buttons;
        identity.keyboard_report_id_present =
            observation.keyboard_report_id_present;
        identity.keyboard_report_id = observation.keyboard_report_id;
        identity.keyboard_modifiers_present =
            observation.keyboard_modifiers_present;
        identity.keyboard_modifiers = observation.keyboard_modifiers;
        identity.accepted_as_monitor_state =
            observation.accepted_as_monitor_state;
        identity.monitor_sequence_before =
            observation.monitor_sequence_before;
        identity.monitor_sequence_after =
            observation.monitor_sequence_after;
        identity.monitor_sequence = observation.monitor_sequence;
        if (!sha256_payload(payload, identity.payload_sha256)) {
            ledger.monitor_packet_recording_failed = true;
            return false;
        }
        if (ledger.monitor_packets.size() < kMaximumMonitorPacketCount) {
            ledger.monitor_packets.push_back(std::move(identity));
        } else {
            ++ledger.dropped_monitor_packet_count;
            ledger.monitor_packets.back() = std::move(identity);
        }
        return true;
    } catch (...) {
        ledger.monitor_packet_recording_failed = true;
        return false;
    }
}

bool write_mouse_effect_probe_safety_ledger(
        const std::filesystem::path& path,
        std::string_view run_uuid,
        mouse_effect_probe::ProbeStopReason stop_reason,
        const MouseEffectProbeSafetyLedger& ledger,
        std::string& file_sha256,
        std::string& error) noexcept {
    file_sha256.clear();
    std::filesystem::path temporary_path;
    try {
        if (path.empty() || !path.is_absolute() ||
            !valid_uuid(run_uuid)) {
            set_error(error, "safety ledger 路径或 Run UUID 非法");
            return false;
        }
        const auto final_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(final_path)) {
            set_error(error, "safety ledger 发布目标已存在，拒绝覆盖");
            return false;
        }
        std::error_code directory_error;
        std::filesystem::create_directories(
            final_path.parent_path(), directory_error);
        if (directory_error || !std::filesystem::is_directory(
                                   final_path.parent_path())) {
            set_error(error, "safety ledger 发布目录创建失败");
            return false;
        }

        nlohmann::ordered_json observations =
            nlohmann::ordered_json::array();
        for (const auto& observation : ledger.observations) {
            observations.push_back({
                {"observed_at_steady_ns",
                 observation.observed_at_steady_ns},
                {"phase", safety_phase_name(observation.phase)},
                {"poll_succeeded", observation.poll_succeeded},
                {"monitor_status",
                 input_monitor_status_name(observation.monitor_status)},
                {"state_valid", observation.state_valid},
                {"monitor_sequence", observation.monitor_sequence},
                {"right_button_pressed",
                 observation.right_button_pressed},
                {"end_pressed", observation.end_pressed},
                {"f8_pressed", observation.f8_pressed},
                {"decision", safety_decision_name(observation.decision)},
            });
        }
        nlohmann::ordered_json monitor_packets =
            nlohmann::ordered_json::array();
        for (const auto& packet : ledger.monitor_packets) {
            const std::string source_ipv4 =
                std::to_string(packet.source_ipv4[0]) + "." +
                std::to_string(packet.source_ipv4[1]) + "." +
                std::to_string(packet.source_ipv4[2]) + "." +
                std::to_string(packet.source_ipv4[3]);
            const std::string configured_device_ipv4 =
                std::to_string(packet.configured_device_ipv4[0]) + "." +
                std::to_string(packet.configured_device_ipv4[1]) + "." +
                std::to_string(packet.configured_device_ipv4[2]) + "." +
                std::to_string(packet.configured_device_ipv4[3]);
            nlohmann::ordered_json packet_json = {
                {"received_at_steady_ns", packet.received_at_steady_ns},
                {"datagram_size", packet.datagram_size},
                {"source_address_size", packet.source_address_size},
                {"source_family", packet.source_family},
                {"source_endpoint_valid", packet.source_endpoint_valid},
                {"source_ipv4", source_ipv4},
                {"source_port", packet.source_port},
                {"monitor_local_port", packet.monitor_local_port},
                {"configured_device_ipv4", configured_device_ipv4},
                {"configured_device_port", packet.configured_device_port},
                {"source_ip_matches_configured_device",
                 packet.source_ip_matches_configured_device},
                {"source_port_matches_configured_device",
                 packet.source_port_matches_configured_device},
                {"exact_monitor_packet_size",
                 packet.exact_monitor_packet_size},
                {"mouse_report_id_present",
                 packet.mouse_report_id_present},
                {"mouse_report_id", packet.mouse_report_id_present
                    ? nlohmann::ordered_json(packet.mouse_report_id)
                    : nlohmann::ordered_json(nullptr)},
                {"mouse_buttons_present", packet.mouse_buttons_present},
                {"mouse_buttons", packet.mouse_buttons_present
                    ? nlohmann::ordered_json(packet.mouse_buttons)
                    : nlohmann::ordered_json(nullptr)},
                {"keyboard_report_id_present",
                 packet.keyboard_report_id_present},
                {"keyboard_report_id", packet.keyboard_report_id_present
                    ? nlohmann::ordered_json(packet.keyboard_report_id)
                    : nlohmann::ordered_json(nullptr)},
                {"keyboard_modifiers_present",
                 packet.keyboard_modifiers_present},
                {"keyboard_modifiers", packet.keyboard_modifiers_present
                    ? nlohmann::ordered_json(packet.keyboard_modifiers)
                    : nlohmann::ordered_json(nullptr)},
                {"accepted_as_monitor_state",
                 packet.accepted_as_monitor_state},
                {"monitor_sequence_before",
                 packet.monitor_sequence_before},
                {"monitor_sequence_after",
                 packet.monitor_sequence_after},
                {"monitor_sequence", packet.monitor_sequence},
                {"payload_sha256", packet.payload_sha256},
            };
            monitor_packets.push_back(std::move(packet_json));
        }
        const auto terminal_decision = ledger.observations.empty()
            ? "none"
            : safety_decision_name(ledger.observations.back().decision);
        const nlohmann::ordered_json document = {
            {"schema_version", 2},
            {"evidence_type", "mouse_effect_probe_safety_monitor_ledger"},
            {"physical_output_capability", false},
            {"run_uuid", run_uuid},
            {"input_backend", "kmbox_net"},
            {"timebase", {
                {"name", "steady_clock_nanoseconds_since_epoch"},
                {"ticks_per_second", 1'000'000'000ULL},
            }},
            {"probe_stop_reason",
             mouse_effect_probe::probe_stop_reason_name(stop_reason)},
            {"terminal_decision", terminal_decision},
            {"recording_failed", ledger.recording_failed},
            {"dropped_observation_count",
             ledger.dropped_observation_count},
            {"observations", std::move(observations)},
            {"monitor_packet_recording_failed",
             ledger.monitor_packet_recording_failed},
            {"dropped_monitor_packet_count",
             ledger.dropped_monitor_packet_count},
            {"monitor_packets", std::move(monitor_packets)},
        };
        const std::string content = document.dump(2) + '\n';
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        if (std::filesystem::exists(temporary_path)) {
            set_error(error,
                "safety ledger 临时发布目标已存在，拒绝覆盖");
            temporary_path.clear();
            return false;
        }
        std::ofstream output(
            temporary_path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(
            content.size()));
        output.flush();
        const bool written = output.good();
        output.close();
        if (!written) {
            set_error(error, "safety ledger 临时文件写入失败");
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            const auto win32_error = GetLastError();
            set_error(error, "safety ledger 原子发布失败，Win32Error=" +
                             std::to_string(win32_error));
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        temporary_path.clear();
        if (!mouse_effect_probe::calculate_mouse_effect_probe_file_sha256(
                final_path, file_sha256, error)) {
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("发布 safety ledger 异常: ") +
                         exception.what());
    } catch (...) {
        set_error(error, "发布 safety ledger 时发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    file_sha256.clear();
    return false;
}

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
        bool seen_safety_ledger = false;
        bool seen_run_uuid = false;
        bool seen_activation_epoch = false;
        bool seen_max_seconds = false;
        bool seen_allow_physical = false;
        bool seen_confirmation = false;
        std::wstring physical_confirmation;

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
                } else if (value == L"physical-b") {
                    options.dispatch_mode =
                        mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B;
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
            } else if (argument == L"--safety-ledger") {
                if (duplicate(seen_safety_ledger,
                              "--safety-ledger")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.safety_ledger_path =
                    std::filesystem::path(value);
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
                              "--confirm-physical-output")) {
                    if (error.empty()) {
                        set_error(error, "物理输出确认令牌重复");
                    }
                    return MouseEffectProbeParseStatus::INVALID;
                }
                physical_confirmation.assign(value);
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
        if (is_physical_dispatch(options.dispatch_mode)) {
            const auto expected_confirmation = options.dispatch_mode ==
                    mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A
                ? kPhysicalAConfirmation : kPhysicalBConfirmation;
            if (!options.allow_physical_output ||
                !seen_confirmation ||
                physical_confirmation != expected_confirmation) {
                set_error(error,
                    "physical mode 缺少双重授权或确认令牌不匹配");
                return MouseEffectProbeParseStatus::INVALID;
            }
            options.physical_output_confirmed = true;
            if (!seen_safety_ledger ||
                options.safety_ledger_path.empty() ||
                !options.safety_ledger_path.is_absolute()) {
                set_error(error,
                    "physical mode 缺少绝对 safety ledger 发布路径");
                return MouseEffectProbeParseStatus::INVALID;
            }
        } else if (seen_allow_physical || seen_confirmation ||
                   seen_safety_ledger) {
            set_error(error,
                "output-off rehearsal 禁止物理输出授权或 safety ledger 参数");
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
        "--safety-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT\n"
        "physical B 额外要求:\n"
        "  --mode physical-b --allow-physical-output "
        "--safety-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT\n"
        "physical A/B 会发送真实 KMBOX X 输入；只能由用户前台启动。\n";
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
        if (std::filesystem::exists(options.report_path) ||
            (is_physical_dispatch(options.dispatch_mode) &&
             std::filesystem::exists(options.safety_ledger_path))) {
            set_error(error,
                "command report 或 safety ledger 已存在，拒绝开始 probe");
            return false;
        }
        SidecarWitness sidecar;
        if (!sidecar.open(options, error)) return false;

        mouse_effect_probe::MouseEffectProbeSequence sequence;
        if (!mouse_effect_probe::read_mouse_effect_probe_sequence(
                options.sequence_path, sequence, error)) {
            return false;
        }
        const bool physical_b_sequence =
            sequence.schema == 5U &&
            sequence.profile == "physical_b_prbs_primary";
        if ((options.dispatch_mode ==
                 mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
             !physical_b_sequence) ||
            (options.dispatch_mode ==
                 mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A &&
             physical_b_sequence)) {
            set_error(error,
                "physical dispatch mode 与 sequence schema/profile 不一致");
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

        MouseEffectProbeSafetyLedger safety_ledger;
        std::shared_ptr<PhysicalKmboxMonitorPacketObserver>
            monitor_packet_observer;
        MouseOutputOwnerLease rehearsal_owner_guard;
        std::shared_ptr<IMouseController> mouse;
        if (is_physical_dispatch(options.dispatch_mode)) {
            if (app_config.mouse.backend != MouseBackend::KMBOX_NET) {
                set_error(error,
                    "physical mode 当前只接受 KMBOX NET backend");
                return false;
            }
            monitor_packet_observer =
                std::make_shared<PhysicalKmboxMonitorPacketObserver>();
            if (!mouse::detail::install_kmbox_monitor_packet_observer(
                    monitor_packet_observer)) {
                set_error(error,
                    "physical mode 无法独占 KMBOX monitor packet observer");
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
            if (monitor_packet_observer) {
                monitor_packet_observer->copy_to(safety_ledger);
            }
            result.execution = executor.result();
            result.safety_ledger = safety_ledger;
            bool safety_ledger_published = true;
            if (is_physical_dispatch(options.dispatch_mode)) {
                std::string safety_ledger_error;
                safety_ledger_published =
                    write_mouse_effect_probe_safety_ledger(
                        options.safety_ledger_path, options.run_uuid,
                        result.execution.stop_reason, safety_ledger,
                        result.safety_ledger_sha256,
                        safety_ledger_error);
                if (!safety_ledger_published) {
                    if (execution_error.empty()) {
                        execution_error = "safety ledger 发布失败: " +
                            safety_ledger_error;
                    } else {
                        execution_error += "; safety ledger 发布失败: " +
                            safety_ledger_error;
                    }
                }
            }
            std::string report_error;
            const bool report_published =
                mouse_effect_probe::write_mouse_effect_probe_report(
                    options.report_path, execution_options, sequence,
                    report_binding, result.execution,
                    result.report_sha256, report_error);
            if (!report_published) {
                if (execution_error.empty()) {
                    execution_error = "command report 发布失败: " +
                        report_error;
                } else {
                    execution_error += "; command report 发布失败: " +
                        report_error;
                }
            }
            return safety_ledger_published && report_published;
        };
        if (!started) {
            publish_report();
            error = execution_error;
            return false;
        }

        if (is_physical_dispatch(options.dispatch_mode)) {
            std::cout << mouse_effect_probe_deadman_arming_prompt() << '\n'
                      << std::flush;
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
                const auto safety = poll_physical_safety(
                    mouse, MouseEffectProbeSafetyPhase::ARMING,
                    safety_ledger);
                if (safety == MouseEffectProbeSafetyDecision::READY) {
                    armed = true;
                    break;
                }
                if (safety == MouseEffectProbeSafetyDecision::USER_STOP) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::USER_STOP,
                        execution_error);
                    break;
                }
                if (safety == MouseEffectProbeSafetyDecision::FAILURE) {
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
                    ? "physical mode 未完成 deadman 武装" : execution_error;
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
            if (is_physical_dispatch(options.dispatch_mode)) {
                const auto safety = poll_physical_safety(
                    mouse, MouseEffectProbeSafetyPhase::ACTIVE,
                    safety_ledger);
                if (safety == MouseEffectProbeSafetyDecision::USER_STOP) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::USER_STOP,
                        execution_error);
                    break;
                }
                if (safety == MouseEffectProbeSafetyDecision::RELEASED ||
                    safety == MouseEffectProbeSafetyDecision::WAITING) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::SAFETY_RELEASED,
                        execution_error);
                    break;
                }
                if (safety == MouseEffectProbeSafetyDecision::FAILURE) {
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
