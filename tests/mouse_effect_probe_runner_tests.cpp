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

void test_parser_recognizes_physical_b_holdout_authority() {
    std::string error;
    auto arguments = common_arguments();
    arguments[1] = L"physical-b";
    arguments.push_back(L"--allow-physical-output");
    arguments.push_back(L"--confirm-physical-output");
    arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT");
    arguments.push_back(L"--safety-ledger");
    arguments.push_back(L"E:\\run\\safety-ledger.json");

    MouseEffectProbeRunOptions options;
    expect(parse_mouse_effect_probe_options(arguments, options, error) ==
               MouseEffectProbeParseStatus::READY &&
               options.dispatch_mode ==
                   mouse_effect_probe::ProbeDispatchMode::PHYSICAL_B &&
               options.allow_physical_output &&
               options.physical_output_confirmed,
           "physical-b 必须识别独立 Holdout 确认令牌: " + error);
}

void test_physical_b_authority_is_bound_to_sequence_profile() {
    std::string error;
    auto primary_arguments = common_arguments();
    primary_arguments[1] = L"physical-b";
    primary_arguments.push_back(L"--allow-physical-output");
    primary_arguments.push_back(L"--confirm-physical-output");
    primary_arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT");
    primary_arguments.push_back(L"--safety-ledger");
    primary_arguments.push_back(L"E:\\run\\safety-ledger.json");
    MouseEffectProbeRunOptions primary_options;
    expect(parse_mouse_effect_probe_options(
               primary_arguments, primary_options, error) ==
               MouseEffectProbeParseStatus::READY,
           "Primary B 参数必须先通过 parser: " + error);

    auto holdout_arguments = primary_arguments;
    holdout_arguments[holdout_arguments.size() - 3U] =
        L"XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT";
    MouseEffectProbeRunOptions holdout_options;
    expect(parse_mouse_effect_probe_options(
               holdout_arguments, holdout_options, error) ==
               MouseEffectProbeParseStatus::READY,
           "Holdout B 参数必须先通过 parser: " + error);

    mouse_effect_probe::MouseEffectProbeSequence primary_sequence;
    primary_sequence.schema = 5U;
    primary_sequence.profile = "physical_b_prbs_primary";
    mouse_effect_probe::MouseEffectProbeSequence holdout_sequence;
    holdout_sequence.schema = 5U;
    holdout_sequence.profile = "physical_b_prbs_holdout";

    expect(validate_mouse_effect_probe_sequence_authorization(
               primary_options, primary_sequence, error),
           "Primary token 必须只授权 Primary sequence: " + error);
    expect(!validate_mouse_effect_probe_sequence_authorization(
               primary_options, holdout_sequence, error),
           "Primary token 不得授权 Holdout sequence");
    expect(validate_mouse_effect_probe_sequence_authorization(
               holdout_options, holdout_sequence, error),
           "Holdout token 必须只授权 Holdout sequence: " + error);
    expect(!validate_mouse_effect_probe_sequence_authorization(
               holdout_options, primary_sequence, error),
           "Holdout token 不得授权 Primary sequence");
}

void test_physical_b_magnitude_authority_is_isolated_by_run_role() {
    std::string error;
    auto primary_arguments = common_arguments();
    primary_arguments[1] = L"physical-b";
    primary_arguments.push_back(L"--allow-physical-output");
    primary_arguments.push_back(L"--confirm-physical-output");
    primary_arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT");
    primary_arguments.push_back(L"--safety-ledger");
    primary_arguments.push_back(L"E:\\run\\safety-ledger.json");
    MouseEffectProbeRunOptions primary_options;
    expect(parse_mouse_effect_probe_options(
               primary_arguments, primary_options, error) ==
               MouseEffectProbeParseStatus::READY,
           "多幅值 Primary 独立令牌必须通过 parser: " + error);

    auto holdout_arguments = primary_arguments;
    holdout_arguments[holdout_arguments.size() - 3U] =
        L"XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_HOLDOUT_SENDS_REAL_KMBOX_INPUT";
    MouseEffectProbeRunOptions holdout_options;
    expect(parse_mouse_effect_probe_options(
               holdout_arguments, holdout_options, error) ==
               MouseEffectProbeParseStatus::READY,
           "多幅值 Holdout 独立令牌必须通过 parser: " + error);

    mouse_effect_probe::MouseEffectProbeSequence primary_sequence;
    primary_sequence.schema = 6U;
    primary_sequence.profile = "physical_b_command_magnitude_primary";
    mouse_effect_probe::MouseEffectProbeSequence holdout_sequence;
    holdout_sequence.schema = 6U;
    holdout_sequence.profile = "physical_b_command_magnitude_holdout";
    mouse_effect_probe::MouseEffectProbeSequence prbs_sequence;
    prbs_sequence.schema = 5U;
    prbs_sequence.profile = "physical_b_prbs_primary";

    expect(validate_mouse_effect_probe_sequence_authorization(
               primary_options, primary_sequence, error),
           "多幅值 Primary token 必须授权且只授权 Primary profile: " + error);
    expect(!validate_mouse_effect_probe_sequence_authorization(
               primary_options, holdout_sequence, error) &&
           !validate_mouse_effect_probe_sequence_authorization(
               primary_options, prbs_sequence, error),
           "多幅值 Primary token 不得扩权到 Holdout 或旧 PRBS");
    expect(validate_mouse_effect_probe_sequence_authorization(
               holdout_options, holdout_sequence, error) &&
           !validate_mouse_effect_probe_sequence_authorization(
               holdout_options, primary_sequence, error),
           "多幅值 Holdout token 必须与 Primary 隔离");
}

void test_composite_phase_authority_and_deadline_are_isolated() {
    std::string error;
    auto arguments = common_arguments();
    arguments[1] = L"physical-b";
    arguments.push_back(L"--allow-physical-output");
    arguments.push_back(L"--confirm-physical-output");
    arguments.push_back(
        L"XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT");
    arguments.push_back(L"--safety-ledger");
    arguments.push_back(L"E:\\run\\safety-ledger.json");
    arguments.push_back(L"--composite-plan");
    arguments.push_back(L"E:\\run\\composite-phase-plan.json");
    arguments.push_back(L"--composite-plan-sha256");
    arguments.push_back(
        L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    arguments.push_back(L"--composite-schedule-ledger");
    arguments.push_back(L"E:\\run\\composite-schedule-ledger.json");
    MouseEffectProbeRunOptions options;
    expect(parse_mouse_effect_probe_options(arguments, options, error) ==
               MouseEffectProbeParseStatus::READY &&
               options.physical_authorization ==
                   MouseEffectProbePhysicalAuthorization::
                       PHYSICAL_B_COMPOSITE_PHASE_CALIBRATION &&
               options.expected_composite_plan_sha256 ==
                   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
           "composite-phase 必须使用独立 token 与 plan/schedule-ledger 参数: " +
               error);

    mouse_effect_probe::MouseEffectProbeSequence composite;
    composite.schema = 7;
    composite.profile = "physical_b_composite_phase_calibration";
    mouse_effect_probe::MouseEffectProbeSequence magnitude;
    magnitude.schema = 6;
    magnitude.profile = "physical_b_command_magnitude_primary";
    expect(validate_mouse_effect_probe_sequence_authorization(
               options, composite, error) &&
               !validate_mouse_effect_probe_sequence_authorization(
                   options, magnitude, error),
           "composite token 必须且只能授权 schema 7 calibration profile");

    CompositePhaseDeadline deadline;
    const CompositePhaseDeadlineRequest early{
        .predictor_source_time_at_steady_ns = 1'000'000'000,
        .source_period_ns = 4'000'000,
        .phase_numerator = 1,
        .phase_denominator = 8,
        .issue_lead_ns = 400'000,
    };
    expect(calculate_composite_phase_deadline(early, deadline, error) &&
               deadline.predicted_next_boundary_steady_ns == 1'004'000'000 &&
               deadline.target_completion_steady_ns == 1'004'500'000 &&
               deadline.issue_deadline_steady_ns == 1'004'100'000,
           "P1/8 deadline 必须由 predictor 的下一 boundary 绝对推导: " + error);
    auto control = early;
    control.command_dispatch = false;
    expect(calculate_composite_phase_deadline(control, deadline, error) &&
               deadline.target_completion_steady_ns == 1'004'500'000 &&
               deadline.issue_deadline_steady_ns == 1'004'500'000,
           "零命令 control marker 必须落在 phase center，不能扣除 KMBOX lead");
    auto late = early;
    late.phase_numerator = 7;
    expect(calculate_composite_phase_deadline(late, deadline, error) &&
               deadline.target_completion_steady_ns == 1'007'500'000 &&
               deadline.issue_deadline_steady_ns == 1'007'100'000,
           "P7/8 deadline 不得从前一实际 dispatch 累加");
    auto invalid = early;
    invalid.phase_numerator = 2;
    expect(!calculate_composite_phase_deadline(invalid, deadline, error),
           "非预注册 phase numerator 必须 fail closed");

    mouse_effect_probe::ProbeSourceFrameEvent previous;
    previous.source_timestamp = 10'000'000;
    previous.source_time_at_steady_ns = 2'000'000'000;
    previous.source_clock_rate = 1.0;
    previous.source_clock_uncertainty_ms = 0.18;
    auto following = previous;
    following.source_timestamp += 80'000;
    following.source_time_at_steady_ns += 8'000'000;
    std::uint64_t phase_lower = 0;
    std::uint64_t phase_upper = 0;
    expect(calculate_composite_phase_interval_q32(
               previous, following, 2'007'000'000, 10'000'000,
               phase_lower, phase_upper, error) &&
               phase_lower >= 13U * (std::uint64_t{1} << 32U) / 16U &&
               phase_upper <= 15U * (std::uint64_t{1} << 32U) / 16U,
           "同 mapping segment 的 P7/8 phase 不得把公共 offset 不确定度重复计入 source period: " +
               error);
}

void test_bounded_composite_auto_arm_parser() {
    auto arguments = common_arguments();
    arguments[1] = L"physical-b";
    const std::vector<std::wstring_view> extra{
        L"--allow-physical-output", L"--confirm-physical-output",
        L"XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT",
        L"--safety-ledger", L"E:\\run\\safety.json",
        L"--composite-plan", L"E:\\run\\plan.json",
        L"--composite-plan-sha256",
        L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        L"--composite-schedule-ledger", L"E:\\run\\schedule.json",
        L"--bounded-composite-auto-arm"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    MouseEffectProbeRunOptions options;
    std::string error;
    expect(parse_mouse_effect_probe_options(arguments, options, error) ==
               MouseEffectProbeParseStatus::READY,
           "已授权且15秒内的有限composite应接受显式自动武装: " + error);
    const auto authorized = options;
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    expect(mouse_effect_probe::make_composite_phase_calibration_sequence(sequence, error) &&
               validate_mouse_effect_probe_sequence_authorization(authorized, sequence, error),
           "自动武装必须通过公开生成器的完整固定序列验证: " + error);
    auto altered = sequence;
    altered.samples.front().dx_counts = 2;
    expect(!validate_mouse_effect_probe_sequence_authorization(authorized, altered, error),
           "自动武装不能只看profile放行超过单计数的序列");
    altered = sequence;
    altered.samples.front().dy_counts = 1;
    expect(!validate_mouse_effect_probe_sequence_authorization(authorized, altered, error),
           "自动武装不得带入Y命令");
    auto invalid_authority = authorized;
    invalid_authority.physical_authorization =
        MouseEffectProbePhysicalAuthorization::PHYSICAL_B_MAGNITUDE_PRIMARY;
    expect(!validate_mouse_effect_probe_sequence_authorization(invalid_authority, sequence, error),
           "直接调用也必须拒绝多幅值授权借用自动武装");
    invalid_authority = authorized;
    invalid_authority.max_seconds = 16;
    expect(!validate_mouse_effect_probe_sequence_authorization(invalid_authority, sequence, error),
           "直接调用也必须拒绝超15秒自动武装");
    invalid_authority = authorized;
    invalid_authority.allow_physical_output = false;
    expect(!validate_mouse_effect_probe_sequence_authorization(invalid_authority, sequence, error),
           "自动武装不得替代既有明确输出授权");
    auto duplicate = arguments;
    duplicate.push_back(L"--bounded-composite-auto-arm");
    expect(parse_mouse_effect_probe_options(duplicate, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "自动武装参数重复必须拒绝");
    auto overlong = arguments;
    for (std::size_t index = 0; index + 1 < overlong.size(); ++index)
        if (overlong[index] == L"--max-seconds") overlong[index + 1] = L"16";
    expect(parse_mouse_effect_probe_options(overlong, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "自动武装不得扩展到超过15秒的运行");
    auto output_off = common_arguments();
    output_off.push_back(L"--bounded-composite-auto-arm");
    expect(parse_mouse_effect_probe_options(output_off, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "自动武装必须拒绝output-off及无授权调用");
}

std::vector<std::wstring_view> event_monitor_arguments() {
    auto arguments = common_arguments();
    arguments[1] = L"physical-b";
    const std::vector<std::wstring_view> extra{
        L"--allow-physical-output", L"--confirm-physical-output",
        L"XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT",
        L"--safety-ledger", L"E:\\run\\safety.json",
        L"--composite-plan", L"E:\\run\\plan.json",
        L"--composite-plan-sha256",
        L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        L"--composite-schedule-ledger", L"E:\\run\\schedule.json",
        L"--bounded-composite-auto-arm", L"--bounded-composite-event-monitor"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

void test_bounded_composite_event_monitor_parser() {
    MouseEffectProbeRunOptions options;
    std::string error;
    const auto arguments = event_monitor_arguments();
    expect(parse_mouse_effect_probe_options(arguments, options, error) ==
               MouseEffectProbeParseStatus::READY,
           "显式有限composite事件订阅模式必须被公开CLI接受: " + error);
    expect(options.bounded_composite_auto_arm && options.bounded_composite_event_monitor,
           "事件订阅模式必须保留两个独立显式选择");
    const auto authorized = options;
    for (const auto policy : {mouse_effect_probe::CompositePhaseSchedulerPolicy::LEGACY,
                             mouse_effect_probe::CompositePhaseSchedulerPolicy::ACTIVE_1MS_V1}) {
        mouse_effect_probe::MouseEffectProbeSequence sequence;
        expect(mouse_effect_probe::make_composite_phase_calibration_sequence(
                   policy, sequence, error) &&
                   validate_mouse_effect_probe_sequence_authorization(authorized, sequence, error),
               "事件订阅仅接受完整固定composite序列: " + error);
        auto altered = sequence;
        altered.samples.front().dx_counts = 2;
        expect(!validate_mouse_effect_probe_sequence_authorization(authorized, altered, error),
               "事件订阅不得借profile放行额外脉冲");
        altered = sequence;
        altered.samples.front().dy_counts = 1;
        expect(!validate_mouse_effect_probe_sequence_authorization(authorized, altered, error),
               "事件订阅不得带入Y命令");
        for (int invalid_case = 0; invalid_case < 4; ++invalid_case) {
            auto invalid = authorized;
            if (invalid_case == 0) invalid.bounded_composite_auto_arm = false;
            if (invalid_case == 1) invalid.max_seconds = 16;
            if (invalid_case == 2) invalid.physical_output_confirmed = false;
            if (invalid_case == 3) invalid.physical_authorization =
                MouseEffectProbePhysicalAuthorization::PHYSICAL_B_PRIMARY;
            expect(!validate_mouse_effect_probe_sequence_authorization(invalid, sequence, error),
                   "直接调用事件订阅仍须满足auto/token/时长/输出授权");
        }
    }
    auto missing_auto = arguments;
    std::erase(missing_auto, L"--bounded-composite-auto-arm");
    expect(parse_mouse_effect_probe_options(missing_auto, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "事件订阅不能单独选择");
    auto duplicate = arguments;
    duplicate.push_back(L"--bounded-composite-event-monitor");
    expect(parse_mouse_effect_probe_options(duplicate, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "事件订阅重复参数必须拒绝");
    auto overlong = arguments;
    for (std::size_t index = 0; index + 1 < overlong.size(); ++index)
        if (overlong[index] == L"--max-seconds") overlong[index + 1] = L"16";
    expect(parse_mouse_effect_probe_options(overlong, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "事件订阅不得扩展到16秒");
    auto wrong_authority = arguments;
    for (std::size_t index = 0; index + 1 < wrong_authority.size(); ++index)
        if (wrong_authority[index] == L"--confirm-physical-output")
            wrong_authority[index + 1] = L"XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT";
    expect(parse_mouse_effect_probe_options(wrong_authority, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "非composite令牌不得启用事件订阅");
    auto output_off = common_arguments();
    output_off.push_back(L"--bounded-composite-event-monitor");
    expect(parse_mouse_effect_probe_options(output_off, options, error) ==
               MouseEffectProbeParseStatus::INVALID,
           "无输出模式不得携带事件订阅参数");
}

void test_bounded_composite_event_monitor_preserves_unknown_state() {
    const auto fresh_ledger = [] {
        MouseEffectProbeSafetyLedger ledger;
        ledger.bounded_composite_auto_arm = true;
        ledger.bounded_composite_event_monitor = true;
        return ledger;
    };
    InputSnapshot waiting;
    waiting.status = InputMonitorStatus::WAITING;
    auto ledger = fresh_ledger();
    for (const auto phase : {MouseEffectProbeSafetyPhase::ARMING,
                             MouseEffectProbeSafetyPhase::ACTIVE}) {
        expect(record_mouse_effect_probe_safety_observation(phase, true, waiting, ledger) ==
                   MouseEffectProbeSafetyDecision::BOUNDED_READY_WITHOUT_INPUT_STATE &&
                   ledger.observations.back().monitor_status == InputMonitorStatus::WAITING &&
                   !ledger.observations.back().state_valid &&
                   ledger.observations.back().monitor_sequence == 0 &&
                   !ledger.observations.back().right_button_pressed &&
                   !ledger.input_state_ever_observed,
               "订阅模式须以独立决策开始且如实保留未知首态，不能伪造READY");
    }
    const auto path = std::filesystem::temp_directory_path() /
        ("xen-event-monitor-ledger-" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()) + ".json");
    std::string sha256, error;
    expect(write_mouse_effect_probe_safety_ledger(path,
               "11111111-2222-4333-8444-555555555555",
               mouse_effect_probe::ProbeStopReason::NORMAL_COMPLETION,
               ledger, sha256, error), "事件订阅实际未知首态账本应能发布: " + error);
    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    expect(content.find("\"arming_policy\": \"BOUNDED_COMPOSITE_SUBSCRIBED_EVENT_MONITOR\"") !=
               std::string::npos &&
               content.find("\"decision\": \"bounded_ready_without_input_state\"") !=
               std::string::npos &&
               content.find("\"state_valid\": false") != std::string::npos &&
               content.find("\"monitor_packets\": []") != std::string::npos,
           "事件订阅账本必须保留独立策略、决策、无首态和零包事实");
    auto without_auto = fresh_ledger();
    without_auto.bounded_composite_auto_arm = false;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING, true, waiting, without_auto) ==
               MouseEffectProbeSafetyDecision::WAITING,
           "孤立事件订阅ledger标志不得放行");
    for (const auto status : {InputMonitorStatus::READY, InputMonitorStatus::UNVERIFIED,
                              InputMonitorStatus::STALE, InputMonitorStatus::FAILURE,
                              InputMonitorStatus::CLOSED}) {
        auto invalid = waiting;
        invalid.status = status;
        auto candidate = fresh_ledger();
        const auto decision = record_mouse_effect_probe_safety_observation(
            MouseEffectProbeSafetyPhase::ACTIVE, true, invalid, candidate);
        expect(decision == (status == InputMonitorStatus::FAILURE ||
                            status == InputMonitorStatus::CLOSED
                                ? MouseEffectProbeSafetyDecision::FAILURE
                                : MouseEffectProbeSafetyDecision::WAITING),
               "新模式只能接纳首态WAITING，不得提升其他无效monitor状态");
    }
    auto candidate = fresh_ledger();
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, false, waiting, candidate) ==
               MouseEffectProbeSafetyDecision::FAILURE,
           "事件订阅不能覆盖poll失败");
    auto inconsistent = waiting;
    inconsistent.sequence = 1;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, true, inconsistent, candidate) ==
               MouseEffectProbeSafetyDecision::WAITING,
           "未知首态放行必须严格要求sequence零");
    for (const auto key : {0x23, 0x77}) {
        auto stop = waiting;
        stop.virtual_keys[key] = true;
        auto stop_ledger = fresh_ledger();
        expect(record_mouse_effect_probe_safety_observation(
                   MouseEffectProbeSafetyPhase::ACTIVE, true, stop, stop_ledger) ==
                   MouseEffectProbeSafetyDecision::USER_STOP,
               "事件订阅无首态决策也不得覆盖End/F8");
    }
    auto valid = waiting;
    valid.status = InputMonitorStatus::READY;
    valid.state_valid = true;
    valid.sequence = 1;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, true, valid, ledger) ==
               MouseEffectProbeSafetyDecision::READY && ledger.input_state_ever_observed &&
               ledger.observations.back().state_valid &&
               !ledger.observations.back().right_button_pressed,
           "首个真实有效快照后须正常记录有效状态和未按右键事实");
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, true, waiting, ledger) ==
               MouseEffectProbeSafetyDecision::WAITING && ledger.input_state_ever_observed,
           "曾有真实快照后不得退回未知首态继续输出");
    for (const auto key : {0x23, 0x77}) {
        auto stop = valid;
        stop.virtual_keys[key] = true;
        expect(record_mouse_effect_probe_safety_observation(
                   MouseEffectProbeSafetyPhase::ACTIVE, true, stop, ledger) ==
                   MouseEffectProbeSafetyDecision::USER_STOP,
               "真实首态后的End/F8仍立即终止");
    }
}

void test_bounded_composite_auto_arm_preserves_monitor_and_stop() {
    MouseEffectProbeSafetyLedger ledger;
    ledger.bounded_composite_auto_arm = true;
    InputSnapshot snapshot;
    snapshot.status = InputMonitorStatus::WAITING;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING, true, snapshot, ledger) ==
               MouseEffectProbeSafetyDecision::WAITING,
           "自动武装仍需等待真实有效monitor");
    snapshot.status = InputMonitorStatus::READY;
    snapshot.state_valid = true;
    snapshot.sequence = 1;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ARMING, true, snapshot, ledger) ==
               MouseEffectProbeSafetyDecision::READY &&
               !ledger.observations.back().right_button_pressed,
           "有限自动武装应允许无右键，账本不得伪造按键");
    for (const auto key : {0x23, 0x77}) {
        snapshot.virtual_keys[key] = true;
        expect(record_mouse_effect_probe_safety_observation(
                   MouseEffectProbeSafetyPhase::ACTIVE, true, snapshot, ledger) ==
                   MouseEffectProbeSafetyDecision::USER_STOP,
               "自动武装后End/F8任一急停必须优先于READY");
        snapshot.virtual_keys[key] = false;
    }
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, false, snapshot, ledger) ==
               MouseEffectProbeSafetyDecision::FAILURE,
           "自动武装后poll失败仍立即失败");
    snapshot.status = InputMonitorStatus::FAILURE;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, true, snapshot, ledger) ==
               MouseEffectProbeSafetyDecision::FAILURE,
           "自动武装不能覆盖设备失败");
    snapshot.status = InputMonitorStatus::READY;
    snapshot.state_valid = false;
    expect(record_mouse_effect_probe_safety_observation(
               MouseEffectProbeSafetyPhase::ACTIVE, true, snapshot, ledger) ==
               MouseEffectProbeSafetyDecision::WAITING,
           "自动武装不能用无效快照继续输出");
    const auto path = std::filesystem::temp_directory_path() /
        ("xen-auto-arm-ledger-" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()) + ".json");
    std::string sha256, error;
    expect(write_mouse_effect_probe_safety_ledger(path,
               "11111111-2222-4333-8444-555555555555",
               mouse_effect_probe::ProbeStopReason::USER_STOP,
               ledger, sha256, error),
           "自动武装实际键态账本应能发布: " + error);
    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    expect(content.find("\"arming_policy\": \"BOUNDED_COMPOSITE_AUTO_ARM\"") !=
               std::string::npos &&
               content.find("\"right_button_pressed\": false") != std::string::npos,
           "账本必须明确标识自动武装并保留真实键态");
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
    timing.source_clock_rate = 1.000001;
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
               event.source_clock_rate == 1.000001 &&
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
    test_parser_recognizes_physical_b_holdout_authority();
    test_physical_b_authority_is_bound_to_sequence_profile();
    test_physical_b_magnitude_authority_is_isolated_by_run_role();
    test_composite_phase_authority_and_deadline_are_isolated();
    test_bounded_composite_auto_arm_parser();
    test_bounded_composite_event_monitor_parser();
    test_bounded_composite_event_monitor_preserves_unknown_state();
    test_bounded_composite_auto_arm_preserves_monitor_and_stop();
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
