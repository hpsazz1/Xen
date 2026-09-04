#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "mouse_effect_probe/mouse_effect_probe.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    if (input.empty() || input.size() > static_cast<std::size_t>(
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
}

bool parse_u64(std::wstring_view input, std::uint64_t& output) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8)) return false;
    std::uint64_t candidate = 0;
    const auto [end, result] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), candidate);
    if (result != std::errc{} || end != utf8.data() + utf8.size() ||
        candidate == 0) {
        return false;
    }
    output = candidate;
    return true;
}

void print_usage() {
    std::cout
        << "XenMouseEffectProbeSequence 只生成离线 A/A2 X-only 序列，"
           "不打开 Capture 或 Mouse。\n\n"
        << "用法:\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--baseline-samples <n> --response-samples <n> "
           "--guard-samples <n> "
           "[--profile sparse-pulse-a]\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--baseline-samples <n> --response-samples <n> "
           "--guard-samples <n> --profile dependency-calibration-a2 "
           "--blocks <multiple-of-4> --run-role <p-cal|p-holdout>\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--profile s1-liveness-a2 --run-role <primary|validation> "
           "--challenge-pulses <n> --challenge-stride-samples <n> "
           "[--peak-hold-samples <n>] --settle-samples <n> "
           "--baseline-samples <n>\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--profile physical-b-primary --guard-samples <n> "
           "--lfsr-order <n> --feedback-mask <decimal> --seed <n> "
           "--phase <n> --offline-sequence-semantic-sha256 <sha256>\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--profile physical-b-holdout --guard-samples <n> "
           "--lfsr-order <n> --feedback-mask <decimal> --seed <n> "
           "--phase <n> --offline-sequence-semantic-sha256 <sha256>\n"
        << "  XenMouseEffectProbeSequence --output <new-json> "
           "--profile physical-b-command-magnitude "
           "--run-role <primary|holdout> --baseline-samples 64 "
           "--response-samples 48 --guard-samples 32\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 &&
        (std::wstring_view(argv[1]) == L"--help" ||
         std::wstring_view(argv[1]) == L"-h")) {
        print_usage();
        return 0;
    }
    std::filesystem::path output_path;
    mouse_effect_probe::SparsePulseSequenceRequest request;
    mouse_effect_probe::DependencyCalibrationSequenceRequest
        dependency_request;
    mouse_effect_probe::S1LivenessSequenceRequest s1_liveness_request;
    mouse_effect_probe::PhysicalBPrimarySequenceRequest
        physical_b_primary_request;
    std::wstring profile = L"sparse-pulse-a";
    std::wstring run_role;
    bool seen_output = false;
    bool seen_baseline = false;
    bool seen_response = false;
    bool seen_guard = false;
    bool seen_profile = false;
    bool seen_blocks = false;
    bool seen_run_role = false;
    bool seen_challenge_pulses = false;
    bool seen_challenge_stride = false;
    bool seen_peak_hold = false;
    bool seen_settle = false;
    bool seen_lfsr_order = false;
    bool seen_feedback_mask = false;
    bool seen_seed = false;
    bool seen_phase = false;
    bool seen_offline_sequence_sha = false;
    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) {
            std::cerr << "参数缺少值。\n";
            print_usage();
            return 2;
        }
        const std::wstring_view argument = argv[index];
        const std::wstring_view value = argv[++index];
        if (argument == L"--output" && !seen_output) {
            output_path = std::filesystem::path(value);
            seen_output = true;
        } else if (argument == L"--baseline-samples" && !seen_baseline &&
                   parse_u64(value, request.baseline_sample_count)) {
            seen_baseline = true;
        } else if (argument == L"--response-samples" && !seen_response &&
                   parse_u64(value, request.response_sample_count)) {
            seen_response = true;
        } else if (argument == L"--guard-samples" && !seen_guard &&
                   parse_u64(value, request.guard_sample_count)) {
            seen_guard = true;
        } else if (argument == L"--profile" && !seen_profile &&
                   (value == L"sparse-pulse-a" ||
                     value == L"dependency-calibration-a2" ||
                     value == L"s1-liveness-a2" ||
                     value == L"physical-b-primary" ||
                     value == L"physical-b-holdout" ||
                     value == L"physical-b-command-magnitude")) {
            profile.assign(value);
            seen_profile = true;
        } else if (argument == L"--blocks" && !seen_blocks &&
                   parse_u64(value, dependency_request.block_count)) {
            seen_blocks = true;
        } else if (argument == L"--run-role" && !seen_run_role &&
                   (value == L"p-cal" || value == L"p-holdout" ||
                    value == L"primary" || value == L"validation" ||
                    value == L"holdout")) {
            run_role.assign(value);
            seen_run_role = true;
        } else if (argument == L"--challenge-pulses" &&
                   !seen_challenge_pulses &&
                   parse_u64(value,
                       s1_liveness_request.challenge_pulse_count)) {
            seen_challenge_pulses = true;
        } else if (argument == L"--challenge-stride-samples" &&
                   !seen_challenge_stride &&
                   parse_u64(value,
                       s1_liveness_request.challenge_stride_sample_count)) {
            seen_challenge_stride = true;
        } else if (argument == L"--peak-hold-samples" &&
                   !seen_peak_hold &&
                   parse_u64(value,
                       s1_liveness_request.peak_hold_sample_count)) {
            seen_peak_hold = true;
        } else if (argument == L"--settle-samples" && !seen_settle &&
                   parse_u64(value,
                       s1_liveness_request.settle_sample_count)) {
            seen_settle = true;
        } else if (argument == L"--lfsr-order" && !seen_lfsr_order) {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, parsed) ||
                parsed > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "lfsr-order 非法。\n";
                return 2;
            }
            physical_b_primary_request.lfsr_order =
                static_cast<std::uint32_t>(parsed);
            seen_lfsr_order = true;
        } else if (argument == L"--feedback-mask" && !seen_feedback_mask) {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, parsed) ||
                parsed > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "feedback-mask 非法。\n";
                return 2;
            }
            physical_b_primary_request.feedback_mask =
                static_cast<std::uint32_t>(parsed);
            seen_feedback_mask = true;
        } else if (argument == L"--seed" && !seen_seed &&
                   parse_u64(value, physical_b_primary_request.seed)) {
            seen_seed = true;
        } else if (argument == L"--phase" && !seen_phase &&
                   parse_u64(value, physical_b_primary_request.phase)) {
            seen_phase = true;
        } else if (argument == L"--offline-sequence-semantic-sha256" &&
                   !seen_offline_sequence_sha &&
                   wide_to_utf8(value, physical_b_primary_request.
                       offline_sequence_semantic_sha256)) {
            seen_offline_sequence_sha = true;
        } else {
            std::cerr << "未知、重复或非法参数。\n";
            print_usage();
            return 2;
        }
    }
    const bool dependency_profile =
        profile == L"dependency-calibration-a2";
    const bool s1_liveness_profile = profile == L"s1-liveness-a2";
    const bool physical_b_primary_profile =
        profile == L"physical-b-primary";
    const bool physical_b_holdout_profile =
        profile == L"physical-b-holdout";
    const bool physical_b_profile =
        physical_b_primary_profile || physical_b_holdout_profile;
    const bool command_magnitude_profile =
        profile == L"physical-b-command-magnitude";
    const bool common_valid = seen_output &&
        !output_path.empty() && output_path.is_absolute();
    const bool sparse_valid = !dependency_profile && !s1_liveness_profile &&
        !physical_b_profile && !command_magnitude_profile && seen_baseline &&
        seen_response && seen_guard && !seen_blocks && !seen_run_role &&
        !seen_challenge_pulses && !seen_challenge_stride &&
        !seen_peak_hold && !seen_settle && !seen_lfsr_order &&
        !seen_feedback_mask && !seen_seed && !seen_phase &&
        !seen_offline_sequence_sha;
    const bool dependency_valid = dependency_profile && seen_response &&
        seen_baseline && seen_guard && seen_blocks && seen_run_role &&
        (run_role == L"p-cal" || run_role == L"p-holdout") &&
        !seen_challenge_pulses && !seen_challenge_stride &&
        !seen_peak_hold && !seen_settle && !seen_lfsr_order &&
        !seen_feedback_mask && !seen_seed && !seen_phase &&
        !seen_offline_sequence_sha;
    const bool s1_liveness_valid = s1_liveness_profile &&
        seen_baseline &&
        !seen_response && !seen_guard && !seen_blocks && seen_run_role &&
        (run_role == L"primary" || run_role == L"validation") &&
        seen_challenge_pulses && seen_challenge_stride && seen_settle &&
        !seen_lfsr_order && !seen_feedback_mask && !seen_seed &&
        !seen_phase && !seen_offline_sequence_sha;
    const bool physical_b_valid = physical_b_profile &&
        !seen_baseline && !seen_response && seen_guard && !seen_blocks &&
        !seen_run_role && !seen_challenge_pulses &&
        !seen_challenge_stride && !seen_peak_hold && !seen_settle &&
        seen_lfsr_order && seen_feedback_mask && seen_seed && seen_phase &&
        seen_offline_sequence_sha;
    const bool command_magnitude_valid = command_magnitude_profile &&
        seen_baseline && seen_response && seen_guard && !seen_blocks &&
        seen_run_role && (run_role == L"primary" || run_role == L"holdout") &&
        !seen_challenge_pulses && !seen_challenge_stride &&
        !seen_peak_hold && !seen_settle && !seen_lfsr_order &&
        !seen_feedback_mask && !seen_seed && !seen_phase &&
        !seen_offline_sequence_sha;
    if (!common_valid ||
        (!sparse_valid && !dependency_valid && !s1_liveness_valid &&
         !physical_b_valid && !command_magnitude_valid)) {
        std::cerr << "缺少必填参数，且 output 必须是绝对路径。\n";
        print_usage();
        return 2;
    }

    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    bool generated = false;
    if (physical_b_profile) {
        physical_b_primary_request.guard_sample_count =
            request.guard_sample_count;
        if (physical_b_primary_profile) {
            generated = mouse_effect_probe::make_physical_b_primary_sequence(
                physical_b_primary_request, sequence, error);
        } else {
            mouse_effect_probe::PhysicalBHoldoutSequenceRequest
                holdout_request;
            holdout_request.guard_sample_count =
                physical_b_primary_request.guard_sample_count;
            holdout_request.lfsr_order =
                physical_b_primary_request.lfsr_order;
            holdout_request.feedback_mask =
                physical_b_primary_request.feedback_mask;
            holdout_request.seed = physical_b_primary_request.seed;
            holdout_request.phase = physical_b_primary_request.phase;
            holdout_request.offline_sequence_semantic_sha256 =
                physical_b_primary_request.offline_sequence_semantic_sha256;
            generated = mouse_effect_probe::make_physical_b_holdout_sequence(
                holdout_request, sequence, error);
        }
    } else if (command_magnitude_profile) {
        mouse_effect_probe::CommandMagnitudeSequenceRequest
            magnitude_request;
        magnitude_request.baseline_sample_count =
            request.baseline_sample_count;
        magnitude_request.response_sample_count =
            request.response_sample_count;
        magnitude_request.guard_sample_count = request.guard_sample_count;
        magnitude_request.run_role = run_role == L"primary"
            ? mouse_effect_probe::CommandMagnitudeRunRole::PRIMARY
            : mouse_effect_probe::CommandMagnitudeRunRole::HOLDOUT;
        generated = mouse_effect_probe::make_command_magnitude_sequence(
            magnitude_request, sequence, error);
    } else if (dependency_profile) {
        dependency_request.run_role = run_role == L"p-cal"
            ? mouse_effect_probe::DependencyCalibrationRunRole::P_CAL
            : mouse_effect_probe::DependencyCalibrationRunRole::P_HOLDOUT;
        dependency_request.baseline_sample_count =
            request.baseline_sample_count;
        dependency_request.response_sample_count =
            request.response_sample_count;
        dependency_request.guard_sample_count = request.guard_sample_count;
        generated = mouse_effect_probe::make_dependency_calibration_sequence(
            dependency_request, sequence, error);
    } else if (s1_liveness_profile) {
        s1_liveness_request.baseline_sample_count =
            request.baseline_sample_count;
        s1_liveness_request.run_role = run_role == L"primary"
            ? mouse_effect_probe::S1LivenessRunRole::PRIMARY
            : mouse_effect_probe::S1LivenessRunRole::VALIDATION;
        generated = mouse_effect_probe::make_s1_liveness_sequence(
            s1_liveness_request, sequence, error);
    } else {
        generated = mouse_effect_probe::make_sparse_pulse_sequence(
            request, sequence, error);
    }
    if (!generated ||
        !mouse_effect_probe::write_mouse_effect_probe_sequence(
            output_path, sequence, error)) {
        std::cerr << "生成 Mouse Effect Probe 序列失败: " << error << '\n';
        return 1;
    }
    std::cout << "序列已原子发布: path=" << output_path
              << ", profile=" << sequence.profile
              << ", samples=" << sequence.samples.size()
              << ", net_x_counts=" << sequence.net_x_counts
              << ", max_abs_prefix_x_counts="
              << sequence.max_abs_prefix_x_counts
              << ", sequence_sha256=" << sequence.sequence_sha256 << '\n';
    return 0;
}
