#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
    MouseEffectProbeRunOptions missing_ledger;
    expect(parse_mouse_effect_probe_options(
               physical_arguments, missing_ledger, error) ==
               MouseEffectProbeParseStatus::INVALID &&
               error.find("safety ledger") != std::string::npos,
           "physical A 必须独立报告缺失的只读 safety ledger 路径");
    physical_arguments.push_back(L"--safety-ledger");
    physical_arguments.push_back(L"E:\\run\\safety-ledger.json");
    MouseEffectProbeRunOptions physical;
    expect(parse_mouse_effect_probe_options(
               physical_arguments, physical, error) ==
               MouseEffectProbeParseStatus::READY &&
               physical.dispatch_mode ==
                   mouse_effect_probe::ProbeDispatchMode::PHYSICAL_A &&
               physical.allow_physical_output &&
               physical.physical_output_confirmed &&
               physical.safety_ledger_path ==
                   std::filesystem::path(L"E:\\run\\safety-ledger.json"),
           "带固定令牌的 physical A 参数应解析成功: " + error);

    auto forbidden = common_arguments();
    forbidden.push_back(L"--allow-physical-output");
    MouseEffectProbeRunOptions forbidden_options;
    expect(parse_mouse_effect_probe_options(
               forbidden, forbidden_options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "output-off 模式携带任何物理授权必须拒绝");
}

void test_parser_isolates_physical_b_authority_from_physical_a() {
    std::string error;
    auto physical_b_arguments = common_arguments();
    physical_b_arguments[1] = L"physical-b";
    physical_b_arguments.push_back(L"--allow-physical-output");
    physical_b_arguments.push_back(L"--confirm-physical-output");
    physical_b_arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT");
    physical_b_arguments.push_back(L"--safety-ledger");
    physical_b_arguments.push_back(L"E:\\run\\safety-ledger.json");
    MouseEffectProbeRunOptions physical_b;
    expect(parse_mouse_effect_probe_options(
               physical_b_arguments, physical_b, error) ==
               MouseEffectProbeParseStatus::READY &&
               physical_b.dispatch_mode ==
                   mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
               physical_b.allow_physical_output &&
               physical_b.physical_output_confirmed,
           "B 专用令牌的 physical-b 参数应解析成功: " + error);

    auto wrong_b = physical_b_arguments;
    wrong_b[wrong_b.size() - 3U] =
        L"XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT";
    MouseEffectProbeRunOptions wrong_b_options;
    expect(parse_mouse_effect_probe_options(
               wrong_b, wrong_b_options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "physical-b 必须拒绝 Physical A 确认令牌");

    auto wrong_a = physical_b_arguments;
    wrong_a[1] = L"physical-a";
    MouseEffectProbeRunOptions wrong_a_options;
    expect(parse_mouse_effect_probe_options(
               wrong_a, wrong_a_options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "physical-a 必须拒绝 Physical B 确认令牌");
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

void test_physical_deadman_prompt_contract() {
    const std::string_view prompt =
        mouse_effect_probe_deadman_arming_prompt();
    expect(prompt.find("monitor 已就绪") != std::string_view::npos &&
               prompt.find("5 秒内") != std::string_view::npos &&
               prompt.find("按住右键") != std::string_view::npos &&
               prompt.find("不要提前按住") != std::string_view::npos &&
               prompt.find("时间线完成") != std::string_view::npos &&
               prompt.find("未正常完成") != std::string_view::npos &&
               prompt.find("sidecar") != std::string_view::npos,
           "Physical A 必须说明新鲜右键按下与唯一松键终局，不能把 sidecar publishing 当松键信号");
}

void test_physical_safety_ledger_distinguishes_explicit_release() {
    MouseEffectProbeSafetyLedger ledger;

    InputSnapshot waiting;
    waiting.status = InputMonitorStatus::WAITING;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING,
               true, waiting, ledger) ==
               MouseEffectProbeSafetyDecision::WAITING &&
               ledger.observations.size() == 1U &&
               !ledger.observations.back().state_valid,
           "账本必须把 monitor 尚无有效事实记录为 WAITING，而不是明确 release");

    InputSnapshot pressed;
    pressed.status = InputMonitorStatus::READY;
    pressed.state_valid = true;
    pressed.virtual_keys[0x02] = true;
    pressed.sequence = 41;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING,
               true, pressed, ledger) ==
               MouseEffectProbeSafetyDecision::READY &&
               ledger.observations.size() == 2U &&
               ledger.observations.back().right_button_pressed &&
               ledger.observations.back().monitor_sequence == 41U,
           "账本必须保留完成武装的右键按下事实与 monitor sequence");
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING,
               true, pressed, ledger) ==
               MouseEffectProbeSafetyDecision::READY &&
               ledger.observations.size() == 2U,
           "相同 phase/sequence/键态的高频 poll 不得膨胀账本");

    InputSnapshot released = pressed;
    released.virtual_keys[0x02] = false;
    released.sequence = 42;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE,
               true, released, ledger) ==
               MouseEffectProbeSafetyDecision::RELEASED &&
               ledger.observations.size() == 3U &&
               ledger.observations.back().state_valid &&
               !ledger.observations.back().right_button_pressed &&
               ledger.observations.back().monitor_sequence == 42U,
           "账本必须把 READY 有效快照中的右键清零与 WAITING 分开记录");

    mouse::detail::KmboxMonitorPacketObservation packet;
    packet.received_at_steady_ns = 123456789;
    packet.datagram_size = 20U;
    packet.source_address_size = 16;
    packet.source_family = 2;
    packet.source_endpoint_valid = true;
    packet.source_ipv4 = {127U, 0U, 0U, 1U};
    packet.source_port = 12345U;
    packet.monitor_local_port = 54321U;
    packet.configured_device_ipv4 = {127U, 0U, 0U, 1U};
    packet.configured_device_port = 12345U;
    packet.source_ip_matches_configured_device = true;
    packet.source_port_matches_configured_device = true;
    packet.exact_monitor_packet_size = true;
    packet.mouse_report_id_present = true;
    packet.mouse_report_id = 1U;
    packet.mouse_buttons_present = true;
    packet.mouse_buttons = 2U;
    packet.keyboard_report_id_present = true;
    packet.keyboard_report_id = 2U;
    packet.keyboard_modifiers_present = true;
    packet.keyboard_modifiers = 4U;
    packet.accepted_as_monitor_state = true;
    packet.monitor_sequence_before = 41U;
    packet.monitor_sequence_after = 42U;
    packet.monitor_sequence = 42U;
    std::vector<std::uint8_t> payload(20U, 0U);
    payload[0] = 1U;
    payload[1] = 2U;
    payload[8] = 2U;
    payload[9] = 4U;
    payload[10] = 0x4dU;
    expect(record_mouse_effect_probe_monitor_packet_identity(
               packet, payload, ledger) &&
               ledger.monitor_packets.size() == 1U &&
               ledger.monitor_packets.back().payload_sha256 ==
                   "5522f093a208b9fc9b9808d561537c279f4c1b1a811711a54132f0dad8798b37" &&
               ledger.monitor_packets.back().configured_device_port ==
                   12345U &&
               ledger.monitor_packets.back().keyboard_modifiers == 4U &&
               ledger.monitor_packets.back().monitor_sequence_before == 41U &&
               ledger.monitor_packets.back().monitor_sequence_after == 42U,
           "账本必须以 SHA-256 绑定 monitor 原始 payload，并保留 packet identity");

    const auto root = std::filesystem::temp_directory_path() /
        ("xen-mouse-effect-probe-safety-ledger-test-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    const auto path = root / "safety-ledger.json";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    std::string sha256;
    std::string error;
    expect(write_mouse_effect_probe_safety_ledger(
               path, "11111111-2222-4333-8444-555555555555",
               mouse_effect_probe::ProbeStopReason::SAFETY_RELEASED,
               ledger, sha256, error) && sha256.size() == 64U,
           "只读 safety ledger 必须原子发布并返回文件 SHA-256: " + error);
    std::ifstream input(path, std::ios::binary);
    const std::string content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    expect(content.find("\"schema_version\": 2") !=
               std::string::npos &&
               content.find("\"physical_output_capability\": false") !=
               std::string::npos &&
               content.find("\"monitor_sequence\": 42") !=
                   std::string::npos &&
               content.find("\"right_button_pressed\": false") !=
                   std::string::npos &&
               content.find("\"decision\": \"released\"") !=
                   std::string::npos &&
               content.find("\"monitor_packets\"") !=
                   std::string::npos &&
               content.find("\"datagram_size\": 20") !=
                   std::string::npos &&
               content.find("\"source_ipv4\": \"127.0.0.1\"") !=
                   std::string::npos &&
               content.find("\"source_port\": 12345") !=
                   std::string::npos &&
               content.find("\"monitor_local_port\": 54321") !=
                   std::string::npos &&
               content.find("\"configured_device_ipv4\": \"127.0.0.1\"") !=
                   std::string::npos &&
               content.find("\"configured_device_port\": 12345") !=
                   std::string::npos &&
               content.find("\"mouse_report_id\": 1") !=
                   std::string::npos &&
               content.find("\"keyboard_report_id\": 2") !=
                   std::string::npos &&
               content.find("\"keyboard_modifiers\": 4") !=
                   std::string::npos &&
               content.find("\"monitor_sequence_before\": 41") !=
                   std::string::npos &&
               content.find("\"monitor_sequence_after\": 42") !=
                   std::string::npos &&
               content.find("\"payload_sha256\": "
                            "\"5522f093a208b9fc9b9808d561537c27"
                            "9f4c1b1a811711a54132f0dad8798b37\"") !=
                   std::string::npos &&
               content.find("\"payload\"") == std::string::npos &&
               content.find("keyboard_data") == std::string::npos,
           "持久账本必须同时保存 explicit release 与可独立判别的原始 packet identity");
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    test_parser_separates_output_off_and_physical_authority();
    test_parser_isolates_physical_b_authority_from_physical_a();
    test_parser_rejects_missing_duplicate_and_invalid_identity();
    test_frame_mapping_preserves_source_identity_and_quality();
    test_physical_deadman_prompt_contract();
    test_physical_safety_ledger_distinguishes_explicit_release();
    if (failures != 0) {
        std::cerr << "Mouse Effect Probe Runner 测试失败数: "
                  << failures << '\n';
        return 1;
    }
    std::cout << "Mouse Effect Probe Runner 测试全部通过。\n";
    return 0;
}
