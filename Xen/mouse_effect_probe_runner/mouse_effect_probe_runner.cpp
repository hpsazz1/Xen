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

#include <algorithm>
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
#include <optional>
#include <thread>
#include <utility>

namespace {

constexpr std::wstring_view kPhysicalAConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBHoldoutConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBMagnitudePrimaryConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBMagnitudeHoldoutConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_HOLDOUT_SENDS_REAL_KMBOX_INPUT";
constexpr std::wstring_view kPhysicalBCompositePhaseConfirmation =
    L"XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT";
constexpr std::size_t kMaximumSafetyObservationCount = 8192U;
constexpr std::size_t kMaximumMonitorPacketCount = 8192U;
std::atomic<bool> stop_requested{false};
std::mutex stop_event_mutex;
HANDLE active_stop_event = nullptr;

struct CompositeScheduleState {
    bool enabled = false;
    std::string plan_sha256;
    std::string qpc_clock_session_id;
    std::int64_t qpc_frequency = 0;
    std::int64_t plan_accepted_qpc = 0;
    std::int64_t acquisition_started_qpc = 0;
    std::int64_t acquisition_finished_qpc = 0;
    std::uint64_t active_wait_total_ns = 0;
    nlohmann::ordered_json events = nlohmann::ordered_json::array();
    std::optional<mouse_effect_probe::ProbeSourceFrameEvent>
        first_response_boundary;
    std::optional<std::size_t> pending_event_index;
};

bool query_qpc(std::int64_t& value) noexcept {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart <= 0) {
        value = 0;
        return false;
    }
    value = counter.QuadPart;
    return true;
}

bool nanoseconds_to_qpc_ticks(std::uint64_t nanoseconds,
                              std::int64_t frequency,
                              std::int64_t& ticks) noexcept {
    if (frequency <= 0) return false;
    const auto unsigned_frequency = static_cast<std::uint64_t>(frequency);
    const auto whole_seconds = nanoseconds / 1'000'000'000ULL;
    const auto remainder = nanoseconds % 1'000'000'000ULL;
    if (whole_seconds > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) /
            unsigned_frequency ||
        remainder > std::numeric_limits<std::uint64_t>::max() /
            unsigned_frequency) {
        return false;
    }
    const auto whole_ticks = whole_seconds * unsigned_frequency;
    const auto partial_product = remainder * unsigned_frequency;
    const auto partial_ticks = partial_product / 1'000'000'000ULL +
        (partial_product % 1'000'000'000ULL == 0 ? 0U : 1U);
    if (whole_ticks > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) - partial_ticks) {
        return false;
    }
    ticks = static_cast<std::int64_t>(whole_ticks + partial_ticks);
    return true;
}

std::uint64_t qpc_ticks_to_nanoseconds_ceil(
        std::int64_t ticks, std::int64_t frequency) noexcept {
    if (ticks <= 0 || frequency <= 0) return 0;
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto divisor = static_cast<std::uint64_t>(frequency);
    const auto whole = value / divisor;
    const auto remainder = value % divisor;
    if (whole > std::numeric_limits<std::uint64_t>::max() /
            1'000'000'000ULL ||
        remainder > std::numeric_limits<std::uint64_t>::max() /
            1'000'000'000ULL) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto partial_product = remainder * 1'000'000'000ULL;
    return whole * 1'000'000'000ULL + partial_product / divisor +
        (partial_product % divisor == 0 ? 0U : 1U);
}

std::uint32_t phase_numerator(std::string_view phase_cell) noexcept {
    if (phase_cell == "P1_8") return 1U;
    if (phase_cell == "P3_8") return 3U;
    if (phase_cell == "P5_8") return 5U;
    if (phase_cell == "P7_8") return 7U;
    return 0U;
}

std::int64_t steady_now_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool qpc_ticks_to_relative_due_time(std::int64_t ticks,
                                    std::int64_t frequency,
                                    LARGE_INTEGER& due_time) noexcept {
    if (ticks <= 0 || frequency <= 0) return false;
    const auto nanoseconds = qpc_ticks_to_nanoseconds_ceil(
        ticks, frequency);
    if (nanoseconds == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto units_100ns = nanoseconds / 100U +
        (nanoseconds % 100U == 0 ? 0U : 1U);
    if (units_100ns == 0 ||
        units_100ns > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    due_time.QuadPart = -static_cast<std::int64_t>(units_100ns);
    return true;
}

const mouse_effect_probe::CompositePhaseWindow*
find_composite_window_for_sample(
        const mouse_effect_probe::MouseEffectProbeSequence& sequence,
        std::uint64_t sample_index) noexcept {
    for (const auto& window : sequence.composite_phase_windows) {
        if (sample_index >= window.first_sample_index &&
            sample_index < window.first_sample_index + window.sample_count) {
            return &window;
        }
    }
    return nullptr;
}

bool calculate_actual_phase_interval_q32_impl(
        const mouse_effect_probe::ProbeSourceFrameEvent& first_boundary,
        const mouse_effect_probe::ProbeSourceFrameEvent& second_boundary,
        std::int64_t event_time_steady_ns,
        std::int64_t qpc_frequency,
        std::uint64_t& lower,
        std::uint64_t& upper) noexcept {
    lower = 0;
    upper = 0;
    if (event_time_steady_ns <= 0 || qpc_frequency <= 0 ||
        first_boundary.source_timestamp <= 0 ||
        second_boundary.source_timestamp <=
            first_boundary.source_timestamp ||
        !std::isfinite(first_boundary.source_clock_rate) ||
        !std::isfinite(second_boundary.source_clock_rate) ||
        first_boundary.source_clock_rate < 0.99 ||
        first_boundary.source_clock_rate > 1.01 ||
        second_boundary.source_clock_rate < 0.99 ||
        second_boundary.source_clock_rate > 1.01 ||
        second_boundary.source_time_at_steady_ns <=
            first_boundary.source_time_at_steady_ns) {
        return false;
    }
    const auto uncertainty_ns = [](double milliseconds,
                                   std::int64_t& output) noexcept {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
            milliseconds > static_cast<double>(
                std::numeric_limits<std::int64_t>::max()) / 1'000'000.0) {
            return false;
        }
        output = static_cast<std::int64_t>(
            std::ceil(milliseconds * 1'000'000.0));
        return true;
    };
    std::int64_t first_uncertainty = 0;
    std::int64_t second_uncertainty = 0;
    if (!uncertainty_ns(first_boundary.source_clock_uncertainty_ms,
                        first_uncertainty) ||
        !uncertainty_ns(second_boundary.source_clock_uncertainty_ms,
                        second_uncertainty)) {
        return false;
    }
    const auto event_uncertainty = static_cast<std::int64_t>(
        qpc_ticks_to_nanoseconds_ceil(1, qpc_frequency));
    const auto first_lower = first_boundary.source_time_at_steady_ns -
        first_uncertainty;
    const auto first_upper = first_boundary.source_time_at_steady_ns +
        first_uncertainty;
    const auto second_lower = second_boundary.source_time_at_steady_ns -
        second_uncertainty;
    const auto second_upper = second_boundary.source_time_at_steady_ns +
        second_uncertainty;
    const auto event_lower = event_time_steady_ns - event_uncertainty;
    const auto event_upper = event_time_steady_ns + event_uncertainty;
    const auto numerator_lower = event_lower - first_upper;
    const auto numerator_upper = event_upper - first_lower;
    const auto mapped_period_lower = second_lower - first_upper;
    const auto mapped_period_upper = second_upper - first_lower;
    const auto source_delta_100ns = static_cast<std::uint64_t>(
        second_boundary.source_timestamp - first_boundary.source_timestamp);
    if (source_delta_100ns >
            std::numeric_limits<std::uint64_t>::max() / 100U) {
        return false;
    }
    const auto source_period_ns = source_delta_100ns * 100U;
    const auto rate_lower = std::min(
        first_boundary.source_clock_rate,
        second_boundary.source_clock_rate);
    const auto rate_upper = std::max(
        first_boundary.source_clock_rate,
        second_boundary.source_clock_rate);
    const auto period_lower_value = std::floor(
        static_cast<long double>(source_period_ns) * rate_lower);
    const auto period_upper_value = std::ceil(
        static_cast<long double>(source_period_ns) * rate_upper);
    if (!std::isfinite(period_lower_value) ||
        !std::isfinite(period_upper_value) || period_lower_value <= 0.0L ||
        period_upper_value > static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max())) {
        return false;
    }
    const auto period_lower = static_cast<std::uint64_t>(
        period_lower_value);
    const auto period_upper = static_cast<std::uint64_t>(
        period_upper_value);
    constexpr std::uint64_t kQ32 = std::uint64_t{1} << 32U;
    if (numerator_lower <= 0 || mapped_period_lower <= 0 ||
        mapped_period_upper <= 0 || period_lower == 0 ||
        period_upper < period_lower ||
        period_upper < static_cast<std::uint64_t>(mapped_period_lower) ||
        period_lower > static_cast<std::uint64_t>(mapped_period_upper) ||
        static_cast<std::uint64_t>(numerator_upper) >= period_lower ||
        static_cast<std::uint64_t>(numerator_lower) >
            std::numeric_limits<std::uint64_t>::max() / kQ32 ||
        static_cast<std::uint64_t>(numerator_upper) >
            std::numeric_limits<std::uint64_t>::max() / kQ32) {
        return false;
    }
    const auto lower_product =
        static_cast<std::uint64_t>(numerator_lower) * kQ32;
    const auto upper_product =
        static_cast<std::uint64_t>(numerator_upper) * kQ32;
    lower = lower_product / period_upper;
    const auto denominator = period_lower;
    upper = upper_product / denominator +
        (upper_product % denominator == 0 ? 0U : 1U);
    return lower <= upper && upper < kQ32;
}

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

bool write_composite_schedule_ledger(
        const MouseEffectProbeRunOptions& options,
        const mouse_effect_probe::MouseEffectProbeSequence& sequence,
        const mouse_effect_probe::ProbeExecutionResult& execution,
        const CompositeScheduleState& state,
        std::string_view command_report_sha256,
        std::string_view safety_ledger_sha256,
        std::string& file_sha256,
        std::string& error) noexcept {
    std::filesystem::path temporary_path;
    try {
        if (!state.enabled || state.qpc_frequency <= 0 ||
            state.plan_accepted_qpc <= 0 ||
            state.acquisition_started_qpc <= state.plan_accepted_qpc ||
            state.acquisition_finished_qpc < state.acquisition_started_qpc ||
            options.composite_schedule_ledger_path.empty() ||
            !options.composite_schedule_ledger_path.is_absolute() ||
            !valid_sha256(state.plan_sha256) ||
            !valid_sha256(command_report_sha256) ||
            !valid_sha256(safety_ledger_sha256)) {
            set_error(error, "composite schedule ledger 终态身份无效");
            return false;
        }
        nlohmann::ordered_json document = {
            {"schema_version", 1},
            {"evidence_type",
             "mouse_effect_probe_b_composite_phase_raw_schedule_ledger"},
            {"status", execution.complete
                ? "ACQUISITION_COMPLETE" : "ACQUISITION_STOPPED"},
            {"ledger_physical_output_capability", false},
            {"ledger_physical_dispatch_count", 0},
            {"run_uuid", options.run_uuid},
            {"activation_epoch", options.activation_epoch},
            {"composite_plan_file_sha256", state.plan_sha256},
            {"probe_binding_sha256", options.expected_binding_sha256},
            {"sequence_semantic_sha256", sequence.sequence_sha256},
            {"command_report_file_sha256", command_report_sha256},
            {"safety_ledger_file_sha256", safety_ledger_sha256},
            {"scheduler_clock", {
                {"clock_kind", "WINDOWS_QPC"},
                {"clock_session_id", state.qpc_clock_session_id},
                {"frequency_hz", state.qpc_frequency},
                {"producer_process_id", GetCurrentProcessId()},
            }},
            {"timer_mode", sequence.composite_phase_request.timer_mode},
            {"plan_accepted_at_qpc", state.plan_accepted_qpc},
            {"acquisition_started_at_qpc",
             state.acquisition_started_qpc},
            {"acquisition_finished_at_qpc",
             state.acquisition_finished_qpc},
            {"revealed_at_qpc", state.acquisition_finished_qpc},
            {"active_wait_total_ns", state.active_wait_total_ns},
            {"source_dispatch_count",
             std::count_if(execution.events.begin(), execution.events.end(),
                [](const auto& event) { return event.dispatch_attempted; })},
            {"events", state.events},
        };
        const nlohmann::json canonical_document = document;
        const auto semantic_input = canonical_document.dump();
        std::string semantic_sha256;
        if (!sha256_payload(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(semantic_input.data()),
                semantic_input.size()), semantic_sha256)) {
            set_error(error, "composite schedule semantic SHA-256 失败");
            return false;
        }
        document["ledger_semantic_sha256"] = semantic_sha256;
        const std::string content = document.dump(2) + "\n";
        const auto final_path = options.composite_schedule_ledger_path;
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId());
        if (std::filesystem::exists(final_path) ||
            std::filesystem::exists(temporary_path)) {
            set_error(error,
                "composite schedule ledger 已存在，拒绝覆盖");
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
        if (!written || !MoveFileExW(
                temporary_path.c_str(), final_path.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            const auto code = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            set_error(error,
                "composite schedule ledger 原子发布失败，Win32Error=" +
                std::to_string(code));
            return false;
        }
        temporary_path.clear();
        return mouse_effect_probe::calculate_mouse_effect_probe_file_sha256(
            final_path, file_sha256, error);
    } catch (const std::exception& exception) {
        set_error(error,
            std::string("发布 composite schedule ledger 异常: ") +
            exception.what());
    } catch (...) {
        set_error(error,
            "发布 composite schedule ledger 时发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    file_sha256.clear();
    return false;
}

bool validate_composite_plan(
        const MouseEffectProbeRunOptions& options,
        const mouse_effect_probe::MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        std::error_code filesystem_error;
        const auto byte_count = std::filesystem::file_size(
            options.composite_plan_path, filesystem_error);
        if (filesystem_error || byte_count == 0 || byte_count > 1'048'576U) {
            set_error(error, "composite plan 大小合同无效");
            return false;
        }
        std::ifstream input(options.composite_plan_path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        if ((!input.good() && !input.eof()) || content.empty()) {
            set_error(error, "composite plan 无法完整读取");
            return false;
        }
        auto document = nlohmann::json::parse(content);
        if (!document.is_object() ||
            document.value("schema_version", 0) != 1 ||
            document.value("evidence_type", std::string{}) !=
                "mouse_effect_probe_b_composite_phase_calibration_plan" ||
            document.value("status", std::string{}) !=
                "FROZEN_BEFORE_CAPTURE" ||
            document.value("run_uuid", std::string{}) != options.run_uuid ||
            document.value("activation_epoch", std::uint64_t{0}) !=
                options.activation_epoch ||
            document.value("physical_output_capability", true) ||
            document.value("physical_dispatch_count", -1) != 0) {
            set_error(error, "composite plan header/Run/output-off 合同无效");
            return false;
        }
        const auto claimed_semantic = document.value(
            "plan_semantic_sha256", std::string{});
        document.erase("plan_semantic_sha256");
        const auto semantic_payload = document.dump();
        std::string actual_semantic;
        if (!valid_sha256(claimed_semantic) ||
            !sha256_payload(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(
                    semantic_payload.data()), semantic_payload.size()),
                actual_semantic) ||
            actual_semantic != claimed_semantic) {
            set_error(error, "composite plan semantic SHA-256 漂移");
            return false;
        }
        const auto& binding = document.at("sequence_binding");
        std::string sequence_file_sha256;
        if (!binding.is_object() ||
            binding.value("sequence_schema", 0) != 7 ||
            binding.value("sequence_profile", std::string{}) !=
                sequence.profile ||
            binding.value("sequence_semantic_sha256", std::string{}) !=
                sequence.sequence_sha256 ||
            binding.value("sample_count", std::uint64_t{0}) !=
                sequence.samples.size() ||
            binding.value("window_count", std::uint64_t{0}) !=
                sequence.composite_phase_windows.size() ||
            !mouse_effect_probe::calculate_mouse_effect_probe_file_sha256(
                options.sequence_path, sequence_file_sha256, error) ||
            binding.value("sequence_file_sha256", std::string{}) !=
                sequence_file_sha256) {
            if (error.empty()) {
                set_error(error, "composite plan sequence binding 漂移");
            }
            return false;
        }
        std::vector<std::string> expected_window_order;
        expected_window_order.reserve(
            sequence.composite_phase_windows.size());
        for (const auto& window : sequence.composite_phase_windows) {
            expected_window_order.push_back(window.window_id);
        }
        if (binding.value("window_order", std::vector<std::string>{}) !=
                expected_window_order) {
            set_error(error, "composite plan window order 与 sequence 漂移");
            return false;
        }
        const auto& policy = document.at("scheduler_policy");
        const auto& request = sequence.composite_phase_request;
        if (!policy.is_object() ||
            policy.value("clock_kind", std::string{}) != "WINDOWS_QPC" ||
            policy.value("timer_mode", std::string{}) != request.timer_mode ||
            policy.value("deadline_basis", std::string{}) !=
                "PREDICTOR_NEXT_NDI_SUBMISSION_BOUNDARY" ||
            policy.value("issue_lead_ns", std::int64_t{0}) !=
                request.issue_lead_ns ||
            policy.value("issue_lead_applies_to", std::string{}) !=
                "NONZERO_PULSE_ONLY" ||
            policy.value("negative_control_marker_lead_ns",
                         std::int64_t{-1}) != 0 ||
            policy.value("target_tolerance_q32", std::uint64_t{0}) !=
                request.target_tolerance_q32 ||
            policy.value("active_guard_ns", std::uint64_t{0}) !=
                request.active_guard_ns ||
            policy.value("max_wake_lateness_ns", std::uint64_t{0}) !=
                request.max_wake_lateness_ns ||
            policy.value("max_event_interval_width_ns", std::uint64_t{0}) !=
                request.max_event_interval_width_ns ||
            policy.value("max_active_wait_ns_per_event", std::uint64_t{0}) !=
                request.max_active_wait_ns_per_event ||
            policy.value("max_active_wait_ns_total", std::uint64_t{0}) !=
                request.max_active_wait_ns_total ||
            !policy.value("preflight_required", false) ||
            !valid_sha256(policy.value(
                "preflight_file_sha256", std::string{})) ||
            policy.value("per_event_tuning_allowed", true) ||
            policy.value("process_priority", std::string{}) != "NORMAL" ||
            policy.value("thread_priority", std::string{}) != "NORMAL" ||
            policy.value("cpu_affinity_used", true) ||
            policy.value("time_begin_period_used", true) ||
            policy.value("periodic_timer_used", true)) {
            set_error(error, "composite scheduler policy 与冻结 sequence 漂移");
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("验证 composite plan 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        set_error(error, "验证 composite plan 时发生未知异常");
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

class RegisteredStopEvent {
public:
    bool open(std::string& error) noexcept {
        handle_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!handle_) {
            set_error(error, "无法创建 composite scheduler stop event");
            return false;
        }
        std::lock_guard lock(stop_event_mutex);
        if (active_stop_event != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
            set_error(error, "已有 composite scheduler stop event");
            return false;
        }
        active_stop_event = handle_;
        if (stop_requested.load(std::memory_order_acquire)) {
            SetEvent(handle_);
        }
        error.clear();
        return true;
    }

    ~RegisteredStopEvent() {
        if (!handle_) return;
        {
            std::lock_guard lock(stop_event_mutex);
            if (active_stop_event == handle_) active_stop_event = nullptr;
        }
        CloseHandle(handle_);
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~OwnedHandle() { if (handle_) CloseHandle(handle_); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this == &other) return *this;
        if (handle_) CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

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
        bool seen_composite_plan = false;
        bool seen_composite_plan_sha = false;
        bool seen_composite_schedule_ledger = false;
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
            } else if (argument == L"--composite-plan") {
                if (duplicate(seen_composite_plan, "--composite-plan")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.composite_plan_path = std::filesystem::path(value);
            } else if (argument == L"--composite-plan-sha256") {
                if (duplicate(seen_composite_plan_sha,
                              "--composite-plan-sha256") ||
                    !wide_to_utf8(
                        value, options.expected_composite_plan_sha256) ||
                    !valid_sha256(
                        options.expected_composite_plan_sha256)) {
                    if (error.empty()) {
                        set_error(error, "--composite-plan-sha256 非法");
                    }
                    return MouseEffectProbeParseStatus::INVALID;
                }
            } else if (argument == L"--composite-schedule-ledger") {
                if (duplicate(seen_composite_schedule_ledger,
                              "--composite-schedule-ledger")) {
                    return MouseEffectProbeParseStatus::INVALID;
                }
                options.composite_schedule_ledger_path =
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
            MouseEffectProbePhysicalAuthorization authorization =
                MouseEffectProbePhysicalAuthorization::NONE;
            if (options.dispatch_mode ==
                    mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A &&
                physical_confirmation == kPhysicalAConfirmation) {
                authorization =
                    MouseEffectProbePhysicalAuthorization::PHYSICAL_A;
            } else if (options.dispatch_mode ==
                           mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
                       physical_confirmation == kPhysicalBConfirmation) {
                authorization = MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_PRIMARY;
            } else if (options.dispatch_mode ==
                           mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
                       physical_confirmation ==
                           kPhysicalBHoldoutConfirmation) {
                authorization = MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_HOLDOUT;
            } else if (options.dispatch_mode ==
                           mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
                       physical_confirmation ==
                           kPhysicalBMagnitudePrimaryConfirmation) {
                authorization = MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_MAGNITUDE_PRIMARY;
            } else if (options.dispatch_mode ==
                           mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
                       physical_confirmation ==
                           kPhysicalBMagnitudeHoldoutConfirmation) {
                authorization = MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_MAGNITUDE_HOLDOUT;
            } else if (options.dispatch_mode ==
                           mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
                       physical_confirmation ==
                           kPhysicalBCompositePhaseConfirmation) {
                authorization = MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_COMPOSITE_PHASE_CALIBRATION;
            }
            if (!options.allow_physical_output ||
                !seen_confirmation ||
                authorization ==
                    MouseEffectProbePhysicalAuthorization::NONE) {
                set_error(error,
                    "physical mode 缺少双重授权或确认令牌不匹配");
                return MouseEffectProbeParseStatus::INVALID;
            }
            options.physical_output_confirmed = true;
            options.physical_authorization = authorization;
            if (!seen_safety_ledger ||
                options.safety_ledger_path.empty() ||
                !options.safety_ledger_path.is_absolute()) {
                set_error(error,
                    "physical mode 缺少绝对 safety ledger 发布路径");
                return MouseEffectProbeParseStatus::INVALID;
            }
            const bool composite_authority = authorization ==
                MouseEffectProbePhysicalAuthorization::
                    PHYSICAL_B_COMPOSITE_PHASE_CALIBRATION;
            const bool complete_composite_paths =
                seen_composite_plan && seen_composite_plan_sha &&
                seen_composite_schedule_ledger &&
                !options.composite_plan_path.empty() &&
                options.composite_plan_path.is_absolute() &&
                !options.composite_schedule_ledger_path.empty() &&
                options.composite_schedule_ledger_path.is_absolute();
            if (composite_authority != complete_composite_paths) {
                set_error(error,
                    "composite-phase token 必须且只能携带绝对 plan/schedule-ledger 路径与 SHA");
                return MouseEffectProbeParseStatus::INVALID;
            }
        } else if (seen_allow_physical || seen_confirmation ||
                   seen_safety_ledger || seen_composite_plan ||
                   seen_composite_plan_sha || seen_composite_schedule_ledger) {
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

bool validate_mouse_effect_probe_sequence_authorization(
        const MouseEffectProbeRunOptions& options,
        const mouse_effect_probe::MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        if (!is_physical_dispatch(options.dispatch_mode)) {
            error.clear();
            return true;
        }
        if (!options.allow_physical_output ||
            !options.physical_output_confirmed) {
            set_error(error, "physical sequence 缺少已确认的物理输出授权");
            return false;
        }

        const bool physical_b_primary_sequence =
            sequence.schema == 5U &&
            sequence.profile == "physical_b_prbs_primary";
        const bool physical_b_holdout_sequence =
            sequence.schema == 5U &&
            sequence.profile == "physical_b_prbs_holdout";
        const bool physical_b_magnitude_primary_sequence =
            sequence.schema == 6U &&
            sequence.profile == "physical_b_command_magnitude_primary";
        const bool physical_b_magnitude_holdout_sequence =
            sequence.schema == 6U &&
            sequence.profile == "physical_b_command_magnitude_holdout";
        const bool physical_b_composite_phase_sequence =
            sequence.schema == 7U &&
            sequence.profile == "physical_b_composite_phase_calibration";
        if (options.dispatch_mode ==
                mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A) {
            if (options.physical_authorization !=
                    MouseEffectProbePhysicalAuthorization::PHYSICAL_A ||
                physical_b_primary_sequence ||
                physical_b_holdout_sequence ||
                physical_b_magnitude_primary_sequence ||
                physical_b_magnitude_holdout_sequence ||
                physical_b_composite_phase_sequence) {
                set_error(error,
                    "Physical A 授权与 sequence schema/profile 不一致");
                return false;
            }
            error.clear();
            return true;
        }

        const auto expected_authorization = physical_b_primary_sequence
            ? MouseEffectProbePhysicalAuthorization::PHYSICAL_B_PRIMARY
            : physical_b_holdout_sequence
                ? MouseEffectProbePhysicalAuthorization::PHYSICAL_B_HOLDOUT
                : physical_b_magnitude_primary_sequence
                    ? MouseEffectProbePhysicalAuthorization::
                        PHYSICAL_B_MAGNITUDE_PRIMARY
                    : physical_b_magnitude_holdout_sequence
                        ? MouseEffectProbePhysicalAuthorization::
                            PHYSICAL_B_MAGNITUDE_HOLDOUT
                    : physical_b_composite_phase_sequence
                        ? MouseEffectProbePhysicalAuthorization::
                            PHYSICAL_B_COMPOSITE_PHASE_CALIBRATION
                : MouseEffectProbePhysicalAuthorization::NONE;
        if (expected_authorization ==
                MouseEffectProbePhysicalAuthorization::NONE ||
            options.physical_authorization != expected_authorization) {
            set_error(error,
                "Physical B 确认令牌与 sequence profile 不一致");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "验证 physical sequence 授权时发生未知异常");
        return false;
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
        "physical B cross-Run Holdout 使用独立确认令牌:\n"
        "  --mode physical-b --allow-physical-output "
        "--safety-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT\n"
        "physical B command-magnitude Primary 使用独立确认令牌:\n"
        "  --mode physical-b --allow-physical-output "
        "--safety-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT\n"
        "physical B command-magnitude Holdout 使用独立确认令牌:\n"
        "  --mode physical-b --allow-physical-output "
        "--safety-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_HOLDOUT_SENDS_REAL_KMBOX_INPUT\n"
        "physical B composite-phase calibration 使用独立确认令牌与 sealed plan:\n"
        "  --mode physical-b --allow-physical-output "
        "--safety-ledger <new-json> --composite-plan <sealed-json> "
        "--composite-plan-sha256 <sha256> "
        "--composite-schedule-ledger <new-json> "
        "--confirm-physical-output "
        "XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT\n"
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
             !std::isfinite(timing.source_clock_rate) ||
             timing.source_clock_rate <= 0.0 ||
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
        event.source_clock_rate = timing.source_clock_rate;
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

bool calculate_composite_phase_deadline(
        const CompositePhaseDeadlineRequest& request,
        CompositePhaseDeadline& deadline,
        std::string& error) noexcept {
    deadline = {};
    try {
        const bool known_phase = request.phase_numerator == 1U ||
            request.phase_numerator == 3U ||
            request.phase_numerator == 5U ||
            request.phase_numerator == 7U;
        if (request.predictor_source_time_at_steady_ns <= 0 ||
            request.source_period_ns <= 0 || !known_phase ||
            request.phase_denominator != 8U || request.issue_lead_ns <= 0 ||
            request.issue_lead_ns >= request.source_period_ns ||
            request.source_period_ns >
                std::numeric_limits<std::int64_t>::max() /
                    static_cast<std::int64_t>(request.phase_numerator)) {
            set_error(error,
                "composite-phase deadline 输入不满足冻结整数合同");
            return false;
        }
        const auto phase_offset =
            request.source_period_ns *
                static_cast<std::int64_t>(request.phase_numerator) /
            static_cast<std::int64_t>(request.phase_denominator);
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (request.predictor_source_time_at_steady_ns >
                maximum - request.source_period_ns) {
            set_error(error, "composite-phase next boundary 溢出");
            return false;
        }
        deadline.predicted_next_boundary_steady_ns =
            request.predictor_source_time_at_steady_ns +
            request.source_period_ns;
        if (deadline.predicted_next_boundary_steady_ns >
                maximum - phase_offset) {
            deadline = {};
            set_error(error, "composite-phase completion deadline 溢出");
            return false;
        }
        deadline.target_completion_steady_ns =
            deadline.predicted_next_boundary_steady_ns + phase_offset;
        deadline.issue_deadline_steady_ns = request.command_dispatch
            ? deadline.target_completion_steady_ns - request.issue_lead_ns
            : deadline.target_completion_steady_ns;
        if (deadline.issue_deadline_steady_ns <=
                request.predictor_source_time_at_steady_ns) {
            deadline = {};
            set_error(error,
                "composite-phase issue deadline 未晚于 predictor");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        deadline = {};
        set_error(error, "计算 composite-phase deadline 时发生未知异常");
        return false;
    }
}

bool calculate_composite_phase_interval_q32(
        const mouse_effect_probe::ProbeSourceFrameEvent& previous_boundary,
        const mouse_effect_probe::ProbeSourceFrameEvent& following_boundary,
        std::int64_t event_time_steady_ns,
        std::int64_t qpc_frequency,
        std::uint64_t& lower,
        std::uint64_t& upper,
        std::string& error) noexcept {
    if (!calculate_actual_phase_interval_q32_impl(
            previous_boundary, following_boundary,
            event_time_steady_ns, qpc_frequency, lower, upper)) {
        set_error(error,
            "composite-phase source timestamp/rate/boundary interval 无效");
        return false;
    }
    error.clear();
    return true;
}

void request_mouse_effect_probe_stop() noexcept {
    stop_requested.store(true, std::memory_order_release);
    std::lock_guard lock(stop_event_mutex);
    if (active_stop_event) SetEvent(active_stop_event);
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
        if (!validate_mouse_effect_probe_sequence_authorization(
                options, sequence, error)) return false;
        const bool composite_phase = sequence.schema == 7U &&
            sequence.profile == "physical_b_composite_phase_calibration";
        CompositeScheduleState composite_schedule;
        RegisteredStopEvent scheduler_stop_event;
        OwnedHandle scheduler_timer;
        if (composite_phase) {
            if (std::filesystem::exists(
                    options.composite_schedule_ledger_path)) {
                set_error(error,
                    "composite schedule ledger 已存在，拒绝开始 probe");
                return false;
            }
            std::string actual_plan_sha256;
            if (!std::filesystem::is_regular_file(
                    options.composite_plan_path) ||
                !mouse_effect_probe::
                    calculate_mouse_effect_probe_file_sha256(
                        options.composite_plan_path, actual_plan_sha256,
                        error) ||
                actual_plan_sha256 !=
                    options.expected_composite_plan_sha256) {
                if (error.empty()) {
                    set_error(error,
                        "composite plan 文件缺失或 SHA-256 漂移");
                }
                return false;
            }
            if (!validate_composite_plan(options, sequence, error)) {
                return false;
            }
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) ||
                frequency.QuadPart <= 0 ||
                !scheduler_stop_event.open(error)) {
                if (error.empty()) {
                    set_error(error, "composite scheduler QPC 初始化失败");
                }
                return false;
            }
            scheduler_timer = OwnedHandle(CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS));
            if (!scheduler_timer) {
                set_error(error,
                    "composite scheduler 高分辨率 one-shot timer 不可用");
                return false;
            }
            composite_schedule.enabled = true;
            composite_schedule.plan_sha256 = actual_plan_sha256;
            composite_schedule.qpc_frequency = frequency.QuadPart;
            composite_schedule.qpc_clock_session_id =
                options.run_uuid + "-qpc";
            if (!query_qpc(composite_schedule.plan_accepted_qpc)) {
                set_error(error, "composite plan accept QPC 读取失败");
                return false;
            }
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
            bool schedule_ledger_published = true;
            if (composite_schedule.enabled &&
                composite_schedule.acquisition_started_qpc > 0) {
                if (composite_schedule.acquisition_finished_qpc <=
                        composite_schedule.acquisition_started_qpc) {
                    query_qpc(
                        composite_schedule.acquisition_finished_qpc);
                }
                std::string schedule_error;
                schedule_ledger_published = report_published &&
                    safety_ledger_published &&
                    write_composite_schedule_ledger(
                        options, sequence, result.execution,
                        composite_schedule, result.report_sha256,
                        result.safety_ledger_sha256,
                        result.composite_schedule_ledger_sha256,
                        schedule_error);
                if (!schedule_ledger_published) {
                    if (schedule_error.empty()) {
                        schedule_error =
                            "command/safety ledger 未完整发布";
                    }
                    if (execution_error.empty()) {
                        execution_error =
                            "composite schedule ledger 发布失败: " +
                            schedule_error;
                    } else {
                        execution_error +=
                            "; composite schedule ledger 发布失败: " +
                            schedule_error;
                    }
                }
            }
            return safety_ledger_published && report_published &&
                schedule_ledger_published;
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
        if (composite_phase &&
            (!query_qpc(composite_schedule.acquisition_started_qpc) ||
             composite_schedule.acquisition_started_qpc <=
                 composite_schedule.plan_accepted_qpc)) {
            executor.request_stop(
                mouse_effect_probe::ProbeStopReason::
                    SCHEDULER_TIMING_INVALID,
                execution_error);
            execution_error = "composite acquisition start QPC 无效";
            capture->close();
            publish_report();
            error = execution_error;
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(options.max_seconds);
        std::optional<mouse_effect_probe::ProbeSourceFrameEvent>
            previous_source_event;
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
            const auto sample_index =
                executor.result().consumed_sample_count;
            const auto* composite_window = composite_phase
                ? find_composite_window_for_sample(sequence, sample_index)
                : nullptr;
            if (composite_phase && sample_index != 0U &&
                !composite_window) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::
                        SCHEDULER_TIMING_INVALID,
                    execution_error);
                execution_error =
                    "composite sample 未归属冻结 window";
                break;
            }

            bool consumed = false;
            if (composite_window &&
                sample_index == composite_window->first_sample_index) {
                nlohmann::ordered_json schedule_event = {
                    {"window_ordinal", composite_window->window_ordinal},
                    {"window_id", composite_window->window_id},
                    {"phase_cell", composite_window->phase_cell},
                    {"negative_control",
                     composite_window->negative_control},
                    {"predictor_sample_index", sample_index},
                    {"predictor_source_frame_sequence",
                     source_event.source_frame_sequence},
                    {"predictor_source_timestamp",
                     source_event.source_timestamp},
                    {"predictor_source_time_at_steady_ns",
                     source_event.source_time_at_steady_ns},
                    {"dispatch_attempted", false},
                    {"status", "PENDING"},
                };
                const auto stop_before_dispatch = [&]
                        (mouse_effect_probe::ProbeStopReason reason,
                         std::string_view message) {
                    schedule_event["status"] =
                        "REJECTED_BEFORE_DISPATCH";
                    schedule_event["failure_reason"] = message;
                    composite_schedule.events.push_back(schedule_event);
                    std::string stop_error;
                    executor.request_stop(reason, stop_error);
                    execution_error.assign(message);
                    if (!stop_error.empty()) {
                        execution_error += "; stop 失败: " + stop_error;
                    }
                };
                if (!previous_source_event.has_value()) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite predictor 缺少前一 source boundary");
                    break;
                }
                const auto source_period_ns =
                    source_event.source_time_at_steady_ns -
                    previous_source_event->source_time_at_steady_ns;
                const auto numerator = phase_numerator(
                    composite_window->phase_cell);
                CompositePhaseDeadline phase_deadline;
                const CompositePhaseDeadlineRequest deadline_request{
                    .predictor_source_time_at_steady_ns =
                        source_event.source_time_at_steady_ns,
                    .source_period_ns = source_period_ns,
                    .phase_numerator = numerator,
                    .phase_denominator = 8U,
                    .issue_lead_ns = sequence.composite_phase_request.
                        issue_lead_ns,
                    .command_dispatch =
                        !composite_window->negative_control,
                };
                if (!calculate_composite_phase_deadline(
                        deadline_request, phase_deadline, frame_error)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        frame_error);
                    break;
                }
                schedule_event["source_period_ns"] = source_period_ns;
                schedule_event["source_timestamp_delta_100ns"] =
                    source_event.source_timestamp -
                    previous_source_event->source_timestamp;
                schedule_event["predicted_next_boundary_steady_ns"] =
                    phase_deadline.predicted_next_boundary_steady_ns;
                schedule_event["target_completion_steady_ns"] =
                    phase_deadline.target_completion_steady_ns;
                schedule_event["issue_deadline_steady_ns"] =
                    phase_deadline.issue_deadline_steady_ns;

                const auto steady_anchor = steady_now_nanoseconds();
                std::int64_t qpc_anchor = 0;
                const auto steady_delta =
                    phase_deadline.issue_deadline_steady_ns -
                    steady_anchor;
                std::int64_t deadline_delta_ticks = 0;
                if (steady_delta <= 0 || !query_qpc(qpc_anchor) ||
                    !nanoseconds_to_qpc_ticks(
                        static_cast<std::uint64_t>(steady_delta),
                        composite_schedule.qpc_frequency,
                        deadline_delta_ticks) ||
                    qpc_anchor > std::numeric_limits<std::int64_t>::max() -
                        deadline_delta_ticks) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite absolute deadline 已迟到或溢出");
                    break;
                }
                const auto deadline_qpc = qpc_anchor +
                    deadline_delta_ticks;
                std::int64_t active_guard_ticks = 0;
                if (!nanoseconds_to_qpc_ticks(
                        sequence.composite_phase_request.active_guard_ns,
                        composite_schedule.qpc_frequency,
                        active_guard_ticks) ||
                    active_guard_ticks <= 0 ||
                    deadline_qpc <= active_guard_ticks) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite active guard 转换失败");
                    break;
                }
                const auto active_start_target_qpc =
                    deadline_qpc - active_guard_ticks;
                std::int64_t now_qpc = 0;
                if (!query_qpc(now_qpc)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite coarse-wait QPC 读取失败");
                    break;
                }
                schedule_event["deadline_qpc"] = deadline_qpc;
                schedule_event["active_start_target_qpc"] =
                    active_start_target_qpc;
                schedule_event["coarse_wait_started_qpc"] = now_qpc;
                bool coarse_wait_used = false;
                if (now_qpc < active_start_target_qpc) {
                    LARGE_INTEGER due_time{};
                    if (!qpc_ticks_to_relative_due_time(
                            active_start_target_qpc - now_qpc,
                            composite_schedule.qpc_frequency, due_time) ||
                        !SetWaitableTimer(scheduler_timer.get(), &due_time,
                                          0, nullptr, nullptr, FALSE)) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::
                                SCHEDULER_TIMING_INVALID,
                            "composite high-resolution one-shot 设置失败");
                        break;
                    }
                    coarse_wait_used = true;
                    const HANDLE handles[]{scheduler_stop_event.get(),
                                           scheduler_timer.get()};
                    const auto wait = WaitForMultipleObjects(
                        2U, handles, FALSE, INFINITE);
                    if (wait == WAIT_OBJECT_0) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::USER_STOP,
                            "composite scheduler 收到用户停止");
                        break;
                    }
                    if (wait != WAIT_OBJECT_0 + 1U) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::
                                SCHEDULER_TIMING_INVALID,
                            "composite high-resolution one-shot wait 失败");
                        break;
                    }
                }
                if (!query_qpc(now_qpc)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite coarse wake QPC 读取失败");
                    break;
                }
                schedule_event["coarse_wait_used"] = coarse_wait_used;
                schedule_event["coarse_wake_qpc"] = now_qpc;
                const auto active_enter_qpc = now_qpc;
                std::uint64_t active_read_count = 0;
                while (now_qpc < deadline_qpc) {
                    ++active_read_count;
                    if (stop_requested.load(std::memory_order_acquire)) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::USER_STOP,
                            "composite active wait 收到用户停止");
                        break;
                    }
                    const auto active_safety = poll_physical_safety(
                        mouse, MouseEffectProbeSafetyPhase::ACTIVE,
                        safety_ledger);
                    if (active_safety !=
                            MouseEffectProbeSafetyDecision::READY) {
                        stop_before_dispatch(
                            active_safety ==
                                MouseEffectProbeSafetyDecision::USER_STOP
                                ? mouse_effect_probe::ProbeStopReason::
                                      USER_STOP
                                : active_safety ==
                                      MouseEffectProbeSafetyDecision::FAILURE
                                    ? mouse_effect_probe::ProbeStopReason::
                                          MOUSE_FAILURE
                                    : mouse_effect_probe::ProbeStopReason::
                                          SAFETY_RELEASED,
                            "composite active wait 安全门已释放或失效");
                        break;
                    }
                    if (!query_qpc(now_qpc)) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::
                                SCHEDULER_TIMING_INVALID,
                            "composite active wait QPC 读取失败");
                        break;
                    }
                    const auto active_ns = qpc_ticks_to_nanoseconds_ceil(
                        now_qpc - active_enter_qpc,
                        composite_schedule.qpc_frequency);
                    if (active_ns > sequence.composite_phase_request.
                            max_active_wait_ns_per_event ||
                        composite_schedule.active_wait_total_ns >
                            sequence.composite_phase_request.
                                max_active_wait_ns_total -
                            std::min(active_ns,
                                sequence.composite_phase_request.
                                    max_active_wait_ns_total)) {
                        stop_before_dispatch(
                            mouse_effect_probe::ProbeStopReason::
                                SCHEDULER_TIMING_INVALID,
                            "composite active wait budget 超限");
                        break;
                    }
                }
                if (executor.result().state !=
                        mouse_effect_probe::ProbeExecutionState::RUNNING) {
                    break;
                }
                const auto final_safety = poll_physical_safety(
                    mouse, MouseEffectProbeSafetyPhase::ACTIVE,
                    safety_ledger);
                if (final_safety != MouseEffectProbeSafetyDecision::READY) {
                    stop_before_dispatch(
                        final_safety ==
                            MouseEffectProbeSafetyDecision::USER_STOP
                            ? mouse_effect_probe::ProbeStopReason::USER_STOP
                            : final_safety ==
                                  MouseEffectProbeSafetyDecision::FAILURE
                                ? mouse_effect_probe::ProbeStopReason::
                                      MOUSE_FAILURE
                                : mouse_effect_probe::ProbeStopReason::
                                      SAFETY_RELEASED,
                        "composite dispatch 前安全门已释放或失效");
                    break;
                }
                std::string sidecar_error;
                if (!sidecar.recording(sidecar_error)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SIDECAR_UNAVAILABLE,
                        sidecar_error.empty()
                            ? "composite dispatch 前 sidecar 已退出"
                            : sidecar_error);
                    break;
                }
                std::int64_t marker_before_qpc = 0;
                std::int64_t marker_after_qpc = 0;
                if (!query_qpc(marker_before_qpc)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite marker 起点 QPC 读取失败");
                    break;
                }
                const auto marker_steady_ns = steady_now_nanoseconds();
                if (!query_qpc(marker_after_qpc) ||
                    marker_after_qpc < marker_before_qpc) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite marker 终点 QPC 读取失败");
                    break;
                }
                const auto marker_width_ns = qpc_ticks_to_nanoseconds_ceil(
                    marker_after_qpc - marker_before_qpc,
                    composite_schedule.qpc_frequency);
                const auto lateness_ns = marker_before_qpc > deadline_qpc
                    ? qpc_ticks_to_nanoseconds_ceil(
                          marker_before_qpc - deadline_qpc,
                          composite_schedule.qpc_frequency)
                    : 0U;
                const auto active_wait_ns =
                    marker_before_qpc > active_enter_qpc
                    ? qpc_ticks_to_nanoseconds_ceil(
                          marker_before_qpc - active_enter_qpc,
                          composite_schedule.qpc_frequency)
                    : 0U;
                if (marker_width_ns > sequence.composite_phase_request.
                        max_event_interval_width_ns ||
                    lateness_ns > sequence.composite_phase_request.
                        max_wake_lateness_ns ||
                    active_wait_ns > sequence.composite_phase_request.
                        max_active_wait_ns_per_event ||
                    composite_schedule.active_wait_total_ns >
                        sequence.composite_phase_request.
                            max_active_wait_ns_total -
                        std::min(active_wait_ns,
                            sequence.composite_phase_request.
                                max_active_wait_ns_total)) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite marker lateness/width/active budget 超限");
                    break;
                }
                composite_schedule.active_wait_total_ns += active_wait_ns;
                schedule_event["active_enter_qpc"] = active_enter_qpc;
                schedule_event["active_read_count"] = active_read_count;
                schedule_event["active_wait_ns"] = active_wait_ns;
                schedule_event["marker_before_qpc"] = marker_before_qpc;
                schedule_event["marker_after_qpc"] = marker_after_qpc;
                schedule_event["marker_width_ns"] = marker_width_ns;
                schedule_event["deadline_lateness_ns"] = lateness_ns;
                schedule_event["marker_time_steady_ns"] =
                    marker_steady_ns;

                source_event.safety_allowed = true;
                const auto report_event_index =
                    executor.result().events.size();
                const bool consume_succeeded =
                    executor.consume_source_frame(source_event, frame_error);
                const auto& execution_after = executor.result();
                if (report_event_index >= execution_after.events.size()) {
                    stop_before_dispatch(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        "composite predictor 未生成 command report event");
                    break;
                }
                const auto& report_event =
                    execution_after.events[report_event_index];
                const auto event_time_steady_ns =
                    composite_window->negative_control
                    ? marker_steady_ns
                    : report_event.backend_completed_at_steady_ns;
                schedule_event["report_event_index"] = report_event_index;
                schedule_event["report_sample_index"] =
                    report_event.sample_index;
                schedule_event["nominal_dx_counts"] =
                    report_event.nominal_dx_counts;
                schedule_event["nominal_dy_counts"] =
                    report_event.nominal_dy_counts;
                schedule_event["dispatch_attempted"] =
                    report_event.dispatch_attempted;
                schedule_event["backend_succeeded"] =
                    report_event.backend_succeeded;
                schedule_event["backend_completed_at_steady_ns"] =
                    report_event.backend_completed_at_steady_ns;
                schedule_event["protocol_ack_received"] =
                    report_event.protocol_ack_received;
                schedule_event["protocol_ack_received_at_steady_ns"] =
                    report_event.protocol_ack_received_at_steady_ns;
                schedule_event["actual_event_time_steady_ns"] =
                    event_time_steady_ns;
                schedule_event["status"] = consume_succeeded
                    ? "DISPATCHED_AWAITING_PHASE_CHECK"
                    : "DISPATCH_FAILED";
                composite_schedule.events.push_back(
                    std::move(schedule_event));
                if (!consume_succeeded) {
                    execution_error = frame_error;
                    break;
                }
                composite_schedule.pending_event_index =
                    composite_schedule.events.size() - 1U;
                composite_schedule.first_response_boundary.reset();
                consumed = true;
            }

            if (!consumed && !executor.consume_source_frame(
                    source_event, frame_error)) {
                execution_error = frame_error;
                break;
            }

            if (composite_window &&
                sample_index == composite_window->first_sample_index + 1U) {
                composite_schedule.first_response_boundary = source_event;
            } else if (composite_window &&
                       sample_index ==
                           composite_window->first_sample_index + 2U) {
                if (!composite_schedule.first_response_boundary.has_value() ||
                    !composite_schedule.pending_event_index.has_value() ||
                    *composite_schedule.pending_event_index >=
                        composite_schedule.events.size()) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        execution_error);
                    execution_error =
                        "composite phase check 缺少 event/boundary";
                    break;
                }
                auto& schedule_event = composite_schedule.events[
                    *composite_schedule.pending_event_index];
                std::uint64_t phase_lower = 0;
                std::uint64_t phase_upper = 0;
                const auto event_time = schedule_event.value(
                    "actual_event_time_steady_ns", std::int64_t{0});
                const auto center = static_cast<std::uint64_t>(
                    phase_numerator(composite_window->phase_cell)) *
                    (std::uint64_t{1} << 32U) / 8U;
                const auto tolerance = sequence.composite_phase_request.
                    target_tolerance_q32;
                const bool interval_valid =
                    calculate_composite_phase_interval_q32(
                        *composite_schedule.first_response_boundary,
                        source_event, event_time,
                        composite_schedule.qpc_frequency,
                        phase_lower, phase_upper, frame_error) &&
                    phase_lower >= center - tolerance &&
                    phase_upper <= center + tolerance;
                schedule_event["first_response_source_frame_sequence"] =
                    composite_schedule.first_response_boundary->
                        source_frame_sequence;
                schedule_event["second_response_source_frame_sequence"] =
                    source_event.source_frame_sequence;
                schedule_event["actual_phase_interval_q32"] = {
                    {"lower_closed", phase_lower},
                    {"upper_closed", phase_upper},
                };
                schedule_event["status"] = interval_valid
                    ? "PHASE_CONFIRMED" : "PHASE_REJECTED";
                composite_schedule.first_response_boundary.reset();
                composite_schedule.pending_event_index.reset();
                if (!interval_valid) {
                    executor.request_stop(
                        mouse_effect_probe::ProbeStopReason::
                            SCHEDULER_TIMING_INVALID,
                        execution_error);
                    execution_error =
                        "composite actual completion/marker phase 超出冻结 cell";
                    break;
                }
            }
            previous_source_event = source_event;
        }
        capture->close();
        if (composite_phase &&
            !query_qpc(composite_schedule.acquisition_finished_qpc)) {
            if (executor.result().state ==
                    mouse_effect_probe::ProbeExecutionState::RUNNING) {
                executor.request_stop(
                    mouse_effect_probe::ProbeStopReason::
                        SCHEDULER_TIMING_INVALID,
                    execution_error);
            }
            if (execution_error.empty()) {
                execution_error =
                    "composite acquisition finish QPC 读取失败";
            }
        }
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
