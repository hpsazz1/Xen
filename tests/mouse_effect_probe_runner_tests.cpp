#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

std::vector<std::wstring_view> common_arguments() {
    return {
        L"--mode", L"output-off-rehearsal",
        L"--config", L"E:\\run\\config.ini",
        L"--sequence", L"E:\\run\\sequence.json",
        L"--binding", L"E:\\run\\probe-binding.json",
        L"--binding-sha256",
        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        L"--sidecar-pid", L"4321",
        L"--sidecar-incoming", L"E:\\run\\pixel.incoming-4321",
        L"--report", L"E:\\run\\command-report.json",
        L"--run-uuid", L"11111111-2222-4333-8444-555555555555",
        L"--activation-epoch", L"7",
        L"--max-seconds", L"15",
    };
}

void test_parser_separates_output_off_and_physical_authority() {
    MouseEffectProbeRunOptions output_off;
    std::string error;
    auto arguments = common_arguments();
    expect(parse_mouse_effect_probe_options(
               arguments, output_off, error) ==
               MouseEffectProbeParseStatus::READY &&
               output_off.dispatch_mode ==
                   mouse_effect_probe::ProbeDispatchMode::
                       OUTPUT_OFF_REHEARSAL &&
               !output_off.allow_physical_output &&
               !output_off.physical_output_confirmed &&
               output_off.sidecar_pid == 4321 &&
               output_off.activation_epoch == 7 &&
               output_off.max_seconds == 15,
           "完整 output-off 参数应解析成功: " + error);

    auto physical_arguments = common_arguments();
    physical_arguments[1] = L"physical-a";
    MouseEffectProbeRunOptions missing_authority;
    expect(parse_mouse_effect_probe_options(
               physical_arguments, missing_authority, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "physical A 缺少双重授权必须在解析期拒绝");
    physical_arguments.push_back(L"--allow-physical-output");
    physical_arguments.push_back(L"--confirm-physical-output");
    physical_arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT");
    MouseEffectProbeRunOptions physical;
    expect(parse_mouse_effect_probe_options(
               physical_arguments, physical, error) ==
               MouseEffectProbeParseStatus::READY &&
               physical.dispatch_mode ==
                   mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A &&
               physical.allow_physical_output &&
               physical.physical_output_confirmed,
           "带固定令牌的 physical A 参数应解析成功: " + error);

    auto forbidden = common_arguments();
    forbidden.push_back(L"--allow-physical-output");
    MouseEffectProbeRunOptions forbidden_options;
    expect(parse_mouse_effect_probe_options(
               forbidden, forbidden_options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "output-off 模式携带任何物理授权必须拒绝");
}

void test_parser_rejects_missing_duplicate_and_invalid_identity() {
    std::string error;
    MouseEffectProbeRunOptions options;
    expect(parse_mouse_effect_probe_options(
               {}, options, error) == MouseEffectProbeParseStatus::INVALID,
           "空参数不得猜测 probe 身份");
    const std::vector<std::wstring_view> help{L"--help"};
    expect(parse_mouse_effect_probe_options(
               help, options, error) == MouseEffectProbeParseStatus::HELP &&
               mouse_effect_probe_usage().find("output-off-rehearsal") !=
                   std::string::npos,
           "--help 必须返回无物理执行的 usage");

    auto duplicate = common_arguments();
    duplicate.push_back(L"--report");
    duplicate.push_back(L"E:\\run\\other.json");
    expect(parse_mouse_effect_probe_options(
               duplicate, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "重复关键参数必须拒绝");

    auto invalid_hash = common_arguments();
    invalid_hash[9] = L"ABC";
    expect(parse_mouse_effect_probe_options(
               invalid_hash, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "binding SHA 必须是 64 位小写十六进制");
}

void test_frame_mapping_preserves_source_identity_and_quality() {
    FrameTiming timing;
    timing.sequence = 42;
    timing.source_timestamp = 123456789;
    timing.source_timestamp_valid = true;
    timing.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
    timing.source_clock_status = SourceClockStatus::VALID;
    timing.source_time_timing_valid = true;
    timing.source_time_at = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(987654321));
    timing.source_clock_uncertainty_ms = 0.15;
    timing.source_clock_round_trip_ms = 0.25;
    timing.source_clock_mapping_age_ms = 0.35;
    timing.source_clock_sample_count = 9;
    timing.source_clock_session_id = 77;
    mouse_effect_probe::ProbeSourceFrameEvent event;
    std::string error;
    expect(make_mouse_effect_probe_source_frame_event(
               timing, true, false, event, error) &&
               event.source_frame_sequence == 42 &&
               event.source_timestamp == 123456789 &&
               event.source_timestamp_valid &&
               event.source_time_at_steady_ns == 987654321 &&
               event.source_time_basis == "NDI_SDK_SUBMISSION" &&
               event.source_clock_status == "VALID" &&
               event.source_clock_session_id == "77" &&
               event.source_clock_uncertainty_ms == 0.15 &&
               event.source_clock_rtt_ms == 0.25 &&
               event.source_clock_mapping_age_ms == 0.35 &&
               event.source_clock_sample_count == 9 &&
               event.sidecar_recording && !event.safety_allowed,
           "FrameTiming 映射必须保留跨 sidecar 对齐所需原始身份与质量: " +
               error);

    timing.source_clock_status = SourceClockStatus::WARMING;
    expect(!make_mouse_effect_probe_source_frame_event(
               timing, true, false, event, error),
           "非 VALID source timing 不得进入 probe executor");
}

} // namespace

int main() {
    test_parser_separates_output_off_and_physical_authority();
    test_parser_rejects_missing_duplicate_and_invalid_identity();
    test_frame_mapping_preserves_source_identity_and_quality();
    if (failures != 0) {
        std::cerr << "Mouse Effect Probe Runner 测试失败数: "
                  << failures << '\n';
        return 1;
    }
    std::cout << "Mouse Effect Probe Runner 测试全部通过。\n";
    return 0;
}
