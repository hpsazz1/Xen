#include "mouse_effect_probe/mouse_effect_probe.h"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("xen-mouse-effect-probe-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class FakeMouseController final : public IMouseController {
public:
    explicit FakeMouseController(std::uint64_t fail_move_index = 0,
                                 std::uint64_t missing_ack_index = 0)
        : fail_move_index_(fail_move_index),
          missing_ack_index_(missing_ack_index) {}

    bool open() noexcept override {
        status_ = MouseStatus::READY;
        return true;
    }

    MouseMoveReceipt move(const MouseMoveCommand& command) noexcept override {
        commands_.push_back(command);
        ++move_count_;
        if (move_count_ == fail_move_index_) {
            status_ = MouseStatus::RESPONSE_TIMEOUT;
            return {};
        }
        const auto acknowledged = std::chrono::steady_clock::now();
        MouseMoveReceipt receipt;
        receipt.succeeded = true;
        receipt.protocol_ack_received = move_count_ != missing_ack_index_;
        if (receipt.protocol_ack_received) {
            receipt.protocol_ack_received_at = acknowledged;
        }
        receipt.backend_completed_at = std::chrono::steady_clock::now();
        return receipt;
    }

    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {};
        snapshot.status = InputMonitorStatus::READY;
        snapshot.state_valid = true;
        return true;
    }

    bool output_owner_exclusive() const noexcept override { return true; }

    void close() noexcept override { status_ = MouseStatus::CLOSED; }
    MouseStatus status() const noexcept override { return status_; }
    std::string last_error() const override { return "fake failure"; }

    const std::vector<MouseMoveCommand>& commands() const noexcept {
        return commands_;
    }

private:
    std::uint64_t fail_move_index_ = 0;
    std::uint64_t missing_ack_index_ = 0;
    std::uint64_t move_count_ = 0;
    MouseStatus status_ = MouseStatus::CLOSED;
    std::vector<MouseMoveCommand> commands_;
};

mouse_effect_probe::ProbeSourceFrameEvent source_frame(
        std::uint64_t sequence) {
    mouse_effect_probe::ProbeSourceFrameEvent frame;
    frame.source_frame_sequence = sequence;
    frame.source_timestamp =
        10'000'000 + static_cast<std::int64_t>(sequence) * 40'000;
    frame.source_timestamp_valid = true;
    frame.source_time_at_steady_ns =
        1'000'000 + static_cast<std::int64_t>(sequence) * 1'000;
    frame.source_time_basis = "NDI_SDK_SUBMISSION";
    frame.source_clock_status = "VALID";
    frame.source_clock_session_id = "test-clock-session";
    frame.source_clock_uncertainty_ms = 0.1;
    frame.source_clock_rtt_ms = 0.2;
    frame.source_clock_mapping_age_ms = 0.3;
    frame.source_clock_sample_count = 8;
    frame.source_timing_valid = true;
    frame.sidecar_recording = true;
    frame.safety_allowed = true;
    return frame;
}

mouse_effect_probe::ProbeExecutionOptions execution_options() {
    mouse_effect_probe::ProbeExecutionOptions options;
    options.run_uuid = "11111111-2222-4333-8444-555555555555";
    options.activation_epoch = 7;
    options.allow_physical_output = true;
    options.physical_output_confirmed = true;
    options.require_protocol_ack = true;
    return options;
}

void test_sparse_pulse_sequence_is_x_only_balanced_and_order_swapped() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    const mouse_effect_probe::SparsePulseSequenceRequest request{
        .baseline_sample_count = 2,
        .response_sample_count = 3,
        .guard_sample_count = 2,
    };
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               request, sequence, error),
           "A 级稀疏序列应生成成功: " + error);
    expect(mouse_effect_probe::validate_mouse_effect_probe_sequence(
               sequence, error),
           "生成结果必须通过同一公有合同校验: " + error);
    expect(sequence.schema == 1 &&
               sequence.profile == "sparse_pulse_a" &&
               sequence.samples.size() == 24 &&
               sequence.blocks.size() == 2,
           "序列必须固定为 baseline、两个互换方向块和块间 guard");
    expect(sequence.net_x_counts == 0 &&
               sequence.max_abs_prefix_x_counts == 1 &&
               sequence.sequence_sha256.size() == 64,
           "A 级序列必须净零、前缀最多 1 count 且有规范 SHA-256");

    std::uint64_t nonzero_count = 0;
    std::int64_t running_x = 0;
    std::uint64_t max_abs_prefix = 0;
    for (std::size_t index = 0; index < sequence.samples.size(); ++index) {
        const auto& sample = sequence.samples[index];
        expect(sample.sample_index == index && sample.dy_counts == 0 &&
                   sample.dx_counts >= -1 && sample.dx_counts <= 1,
               "每个 sample 必须连续编号、Y=0 且 X 仅为 -1/0/+1");
        if (sample.dx_counts != 0) ++nonzero_count;
        running_x += sample.dx_counts;
        const auto absolute = running_x < 0
            ? static_cast<std::uint64_t>(-running_x)
            : static_cast<std::uint64_t>(running_x);
        if (absolute > max_abs_prefix) max_abs_prefix = absolute;
    }
    expect(nonzero_count == 4 && running_x == 0 && max_abs_prefix == 1,
           "只能有四个单 count 脉冲，正常完成净零且前缀有界");
    expect(sequence.blocks[0].first_pulse_dx_counts == 1 &&
               sequence.blocks[0].second_pulse_dx_counts == -1 &&
               sequence.blocks[1].first_pulse_dx_counts == -1 &&
               sequence.blocks[1].second_pulse_dx_counts == 1,
           "第二块必须交换脉冲方向顺序以隔离场景单调漂移");
}

void test_a2_dependency_calibration_sequences_are_balanced_and_independent() {
    using mouse_effect_probe::DependencyCalibrationRunRole;

    const mouse_effect_probe::DependencyCalibrationSequenceRequest primary_request{
        .baseline_sample_count = 2,
        .response_sample_count = 3,
        .guard_sample_count = 2,
        .block_count = 4,
        .run_role = DependencyCalibrationRunRole::P_CAL,
    };
    auto holdout_request = primary_request;
    holdout_request.run_role = DependencyCalibrationRunRole::P_HOLDOUT;

    mouse_effect_probe::MouseEffectProbeSequence primary;
    mouse_effect_probe::MouseEffectProbeSequence holdout;
    std::string error;
    expect(mouse_effect_probe::make_dependency_calibration_sequence(
               primary_request, primary, error),
           "A2 P-CAL 序列应生成成功: " + error);
    expect(mouse_effect_probe::make_dependency_calibration_sequence(
               holdout_request, holdout, error),
           "A2 P-HOLDOUT 序列应生成成功: " + error);
    expect(mouse_effect_probe::validate_mouse_effect_probe_sequence(
               primary, error) &&
               mouse_effect_probe::validate_mouse_effect_probe_sequence(
                   holdout, error),
           "A2 两个角色必须通过同一生产 reader/validator: " + error);

    expect(primary.schema == 2 &&
               primary.profile == "dependency_calibration_a2_p_cal" &&
               holdout.schema == 2 &&
               holdout.profile ==
                   "dependency_calibration_a2_p_holdout" &&
               primary.sequence_sha256 != holdout.sequence_sha256,
           "P-CAL/P-HOLDOUT 必须有显式角色、schema 与不同语义 SHA");
    expect(primary.blocks.size() == 4 && holdout.blocks.size() == 4 &&
               primary.samples.size() == 50 &&
               holdout.samples.size() == 50,
           "每个 A2 block 必须有独立 pre/post guard、两个 transition 和完整 hold");

    const auto pulse_directions = [](const auto& sequence) {
        std::vector<int> result;
        for (const auto& sample : sequence.samples) {
            if (sample.dx_counts != 0) result.push_back(sample.dx_counts);
            expect(sample.dy_counts == 0 && sample.dx_counts >= -1 &&
                       sample.dx_counts <= 1,
                   "A2 sample 必须保持 X-only 且仅为 -1/0/+1");
        }
        return result;
    };
    expect(pulse_directions(primary) ==
               std::vector<int>({1, -1, -1, 1, -1, 1, 1, -1}),
           "P-CAL 必须使用预注册 ABBA 平衡方向顺序");
    expect(pulse_directions(holdout) ==
               std::vector<int>({-1, 1, 1, -1, 1, -1, -1, 1}),
           "P-HOLDOUT 必须使用与 P-CAL 相反且未参与调参的顺序");
    expect(primary.net_x_counts == 0 && holdout.net_x_counts == 0 &&
               primary.max_abs_prefix_x_counts == 1 &&
               holdout.max_abs_prefix_x_counts == 1,
           "A2 正常完成必须逐 block 回 anchor、全局净零且任意前缀不超过 1 count");

    TemporaryDirectory temporary;
    for (const auto* original : {&primary, &holdout}) {
        const auto path = temporary.path() /
            (original->profile + ".json");
        expect(mouse_effect_probe::write_mouse_effect_probe_sequence(
                   path, *original, error),
               "A2 序列文件应原子写入成功: " + error);
        mouse_effect_probe::MouseEffectProbeSequence round_trip;
        expect(mouse_effect_probe::read_mouse_effect_probe_sequence(
                   path, round_trip, error),
               "A2 序列必须通过生产 reader round-trip: " + error);
        expect(round_trip.profile == original->profile &&
                   round_trip.sequence_sha256 == original->sequence_sha256 &&
                   round_trip.blocks.size() == original->blocks.size() &&
                   round_trip.samples.size() == original->samples.size(),
               "A2 round-trip 必须保持 role/profile/SHA/block/sample 身份");
    }
}

void test_a2_s1_liveness_sequences_bracket_an_exact_zero_baseline() {
    using mouse_effect_probe::S1LivenessRunRole;

    const mouse_effect_probe::S1LivenessSequenceRequest primary_request{
        .challenge_pulse_count = 2,
        .challenge_stride_sample_count = 3,
        .peak_hold_sample_count = 5,
        .settle_sample_count = 4,
        .baseline_sample_count = 8,
        .run_role = S1LivenessRunRole::PRIMARY,
    };
    auto validation_request = primary_request;
    validation_request.run_role = S1LivenessRunRole::VALIDATION;

    mouse_effect_probe::MouseEffectProbeSequence primary;
    mouse_effect_probe::MouseEffectProbeSequence validation;
    std::string error;
    expect(mouse_effect_probe::make_s1_liveness_sequence(
               primary_request, primary, error) &&
               mouse_effect_probe::make_s1_liveness_sequence(
                   validation_request, validation, error),
           "A2 S1 primary/validation 活性序列应生成成功: " + error);
    expect(mouse_effect_probe::validate_mouse_effect_probe_sequence(
               primary, error) &&
               mouse_effect_probe::validate_mouse_effect_probe_sequence(
                   validation, error),
           "A2 S1 活性序列必须通过生产 reader/validator: " + error);
    expect(primary.schema == 3 &&
               primary.profile == "dependency_calibration_a2_s1_primary" &&
               validation.schema == 3 &&
               validation.profile ==
                   "dependency_calibration_a2_s1_validation" &&
               primary.sequence_sha256 != validation.sequence_sha256,
           "A2 S1 两个角色必须有独立 profile 与规范语义 SHA");
    expect(primary.samples.size() == 46 && primary.blocks.size() == 2 &&
               primary.net_x_counts == 0 &&
               primary.max_abs_prefix_x_counts == 2,
           "A2 S1 序列必须在两段回锚挑战峰值插入精确零命令停留");
    auto legacy_request = primary_request;
    legacy_request.peak_hold_sample_count = 0;
    mouse_effect_probe::MouseEffectProbeSequence legacy;
    expect(mouse_effect_probe::make_s1_liveness_sequence(
               legacy_request, legacy, error) &&
               legacy.samples.size() == 36,
           "没有 peak hold 的历史 schema 3 序列必须继续可生成和验证: " +
               error);

    std::vector<int> primary_pulses;
    std::vector<int> validation_pulses;
    for (const auto& sample : primary.samples) {
        if (sample.dx_counts != 0) primary_pulses.push_back(sample.dx_counts);
        expect(sample.dy_counts == 0 && sample.dx_counts >= -1 &&
                   sample.dx_counts <= 1,
               "A2 S1 只能发送 X-only 单 count pulse");
    }
    for (const auto& sample : validation.samples) {
        if (sample.dx_counts != 0) {
            validation_pulses.push_back(sample.dx_counts);
        }
    }
    expect(primary_pulses ==
               std::vector<int>({1, 1, -1, -1, -1, -1, 1, 1}) &&
               validation_pulses ==
               std::vector<int>({-1, -1, 1, 1, 1, 1, -1, -1}),
           "validation 必须镜像 primary，且每段挑战正常完成都回到 anchor");
    for (std::size_t index = 6; index < 11; ++index) {
        expect(primary.samples[index].phase ==
                   mouse_effect_probe::ProbeSamplePhase::HOLD &&
                   primary.samples[index].dx_counts == 0,
               "pre challenge 达峰后必须保持预注册零命令平台");
    }
    for (std::size_t index = 35; index < 40; ++index) {
        expect(primary.samples[index].phase ==
                   mouse_effect_probe::ProbeSamplePhase::HOLD &&
                   primary.samples[index].dx_counts == 0,
               "post challenge 达峰后必须保持预注册零命令平台");
    }
    for (std::size_t index = 17; index < 21; ++index) {
        expect(primary.samples[index].phase ==
                   mouse_effect_probe::ProbeSamplePhase::GUARD &&
                   primary.samples[index].dx_counts == 0,
               "pre challenge 后必须有预注册零命令 settle");
    }
    for (std::size_t index = 21; index < 29; ++index) {
        expect(primary.samples[index].phase ==
                   mouse_effect_probe::ProbeSamplePhase::BASELINE &&
                   primary.samples[index].dx_counts == 0,
               "baseline 必须是独立连续的精确零命令窗口");
    }

    TemporaryDirectory temporary;
    const auto path = temporary.path() / "s1-liveness-primary.json";
    expect(mouse_effect_probe::write_mouse_effect_probe_sequence(
               path, primary, error),
           "A2 S1 活性序列应原子发布: " + error);
    mouse_effect_probe::MouseEffectProbeSequence round_trip;
    expect(mouse_effect_probe::read_mouse_effect_probe_sequence(
               path, round_trip, error) &&
               round_trip.sequence_sha256 == primary.sequence_sha256 &&
               round_trip.s1_liveness_request.challenge_pulse_count == 2 &&
               round_trip.s1_liveness_request.peak_hold_sample_count == 5 &&
               round_trip.s1_liveness_request.baseline_sample_count == 8,
           "A2 S1 活性序列必须经生产 reader 精确 round-trip: " + error);
    const auto legacy_path = temporary.path() / "s1-liveness-legacy.json";
    expect(mouse_effect_probe::write_mouse_effect_probe_sequence(
               legacy_path, legacy, error),
           "历史 A2 S1 序列应继续原子发布: " + error);
    std::ifstream legacy_file(legacy_path, std::ios::binary);
    const std::string legacy_text{
        std::istreambuf_iterator<char>(legacy_file),
        std::istreambuf_iterator<char>()};
    mouse_effect_probe::MouseEffectProbeSequence legacy_round_trip;
    expect(legacy_text.find("peak_hold_sample_count") == std::string::npos &&
               mouse_effect_probe::read_mouse_effect_probe_sequence(
                   legacy_path, legacy_round_trip, error) &&
               legacy_round_trip.samples.size() == 36,
           "历史 schema 3 request 必须保持无新增字段并可由生产 reader 读取: " +
               error);

    auto mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(
               execution_options(), round_trip, mouse, error),
           "A2 S1 活性序列应通过同一物理 executor 启动: " + error);
    for (std::size_t index = 0; index < round_trip.samples.size(); ++index) {
        expect(executor.consume_source_frame(
                   source_frame(1'000 + index), error),
               "A2 S1 executor 应逐 source frame 消费且不得追发: " + error);
    }
    expect(executor.result().complete && mouse->commands().size() == 8 &&
               executor.result().cumulative_requested_x_counts == 0 &&
               executor.result().cumulative_backend_completed_x_counts == 0,
           "A2 S1 正常完成必须有完整 ACK 命令账本并净 X=0");
    for (std::size_t index = 21; index < 29; ++index) {
        const auto& event = executor.result().events[index];
        expect(event.nominal_dx_counts == 0 && !event.dispatch_attempted &&
                   event.requested_dx_counts == 0 &&
                   !event.backend_succeeded &&
                   !event.protocol_ack_received,
               "A2 S1 baseline event 必须证明 Probe 未发送命令");
    }
    for (const auto index : {6U, 7U, 8U, 9U, 10U,
                             35U, 36U, 37U, 38U, 39U}) {
        const auto& event = executor.result().events[index];
        expect(event.nominal_dx_counts == 0 && !event.dispatch_attempted &&
                   event.requested_dx_counts == 0 &&
                   !event.backend_succeeded &&
                   !event.protocol_ack_received,
               "A2 S1 peak hold event 必须证明零命令且未触发 backend/ACK");
    }
}

void test_sequence_file_round_trip_rejects_overwrite_and_tampering() {
    TemporaryDirectory temporary;
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 2,
                .response_sample_count = 3,
                .guard_sample_count = 2},
               sequence, error),
           "文件测试序列应生成成功: " + error);
    const auto path = temporary.path() / "sparse-pulse-a.json";
    expect(mouse_effect_probe::write_mouse_effect_probe_sequence(
               path, sequence, error),
           "合法序列应原子发布: " + error);
    expect(!mouse_effect_probe::write_mouse_effect_probe_sequence(
               path, sequence, error),
           "既有序列目标必须拒绝覆盖");

    mouse_effect_probe::MouseEffectProbeSequence loaded;
    expect(mouse_effect_probe::read_mouse_effect_probe_sequence(
               path, loaded, error),
           "发布序列必须能经公有 reader 回读: " + error);
    expect(loaded.sequence_sha256 == sequence.sequence_sha256 &&
               loaded.samples.size() == sequence.samples.size() &&
               loaded.blocks.size() == sequence.blocks.size(),
           "回读结果必须保留规范 SHA、sample 和 block");

    std::ifstream input(path, std::ios::binary);
    std::string tampered((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    input.close();
    const auto pulse = tampered.find("\"dx_counts\": 1");
    expect(pulse != std::string::npos,
           "测试夹具必须找到正向 pulse");
    if (pulse != std::string::npos) {
        tampered[pulse + std::string("\"dx_counts\": ").size()] = '0';
    }
    const auto tampered_path = temporary.path() / "tampered.json";
    std::ofstream output(tampered_path, std::ios::binary | std::ios::trunc);
    output << tampered;
    output.close();
    expect(!mouse_effect_probe::read_mouse_effect_probe_sequence(
               tampered_path, loaded, error),
           "sample 被篡改后必须因结构或 SHA 不一致而拒绝");
}

void test_executor_consumes_one_sample_per_frame_and_never_catches_up() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "frame-driven 测试序列应生成成功: " + error);
    auto mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(execution_options(), sequence, mouse, error),
           "fake executor 应启动成功: " + error);
    expect(executor.consume_source_frame(source_frame(100), error) &&
               mouse->commands().empty(),
           "baseline frame 只能消费一个零 sample，不得发 Mouse");
    expect(executor.consume_source_frame(source_frame(101), error) &&
               mouse->commands().size() == 1 &&
               mouse->commands()[0].dx_counts == 1 &&
               mouse->commands()[0].dy_counts == 0,
           "下一个连续 frame 只能发送对应 +1 X pulse");
    expect(executor.consume_source_frame(source_frame(102), error) &&
               mouse->commands().size() == 1,
           "response 零 sample 不得伪造成 Mouse 命令");
    expect(!executor.consume_source_frame(source_frame(104), error) &&
               executor.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SOURCE_FRAME_GAP &&
               executor.result().consumed_sample_count == 3 &&
               mouse->commands().size() == 1,
           "source frame gap 必须立即停止，不追发缺失 sample");
    expect(!executor.consume_source_frame(source_frame(105), error) &&
               mouse->commands().size() == 1,
           "停止后任何新 frame 都不得恢复或补发");
}

void test_executor_failure_stops_without_compensation() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "失败语义测试序列应生成成功: " + error);
    auto mouse = std::make_shared<FakeMouseController>(1);
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(execution_options(), sequence, mouse, error),
           "失败语义 fake executor 应启动成功: " + error);
    expect(executor.consume_source_frame(source_frame(200), error),
           "首个 baseline 应正常消费: " + error);
    expect(!executor.consume_source_frame(source_frame(201), error) &&
               executor.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::MOUSE_FAILURE &&
               executor.result().cumulative_requested_x_counts == 1 &&
               executor.result().cumulative_backend_completed_x_counts == 0 &&
               mouse->commands().size() == 1 &&
               mouse->commands()[0].dx_counts == 1,
           "首个 pulse 失败必须保留 residual 并立即停发");
    for (std::uint64_t frame = 202; frame < 220; ++frame) {
        expect(!executor.consume_source_frame(source_frame(frame), error),
               "失败后 executor 必须保持停止");
    }
    expect(mouse->commands().size() == 1,
           "失败或急停后禁止发送自动反向补偿");
}

void test_executor_complete_and_user_stop_contracts() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "完整执行测试序列应生成成功: " + error);
    auto mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(execution_options(), sequence, mouse, error),
           "完整执行 fake executor 应启动成功: " + error);
    for (std::uint64_t index = 0; index < sequence.samples.size(); ++index) {
        expect(executor.consume_source_frame(source_frame(300 + index), error),
               "每个连续 source frame 应恰好消费一个 sample: " + error);
    }
    const std::vector<int> expected_directions{1, -1, -1, 1};
    expect(executor.result().state ==
               mouse_effect_probe::ProbeExecutionState::COMPLETED &&
               executor.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::NORMAL_COMPLETION &&
               executor.result().complete &&
               executor.result().events.size() == sequence.samples.size() &&
               executor.result().cumulative_requested_x_counts == 0 &&
               executor.result().cumulative_backend_completed_x_counts == 0 &&
               mouse->commands().size() == expected_directions.size(),
           "正常完成必须逐 sample 留痕且 requested/completed 都净零");
    for (std::size_t index = 0; index < mouse->commands().size(); ++index) {
        expect(mouse->commands()[index].dx_counts ==
                   expected_directions[index] &&
                   mouse->commands()[index].dy_counts == 0,
               "完整 fake 执行必须保持 +1/-1、-1/+1 的 X-only 顺序");
    }
    for (const auto& event : executor.result().events) {
        if (!event.dispatch_attempted) {
            expect(event.issued_at_steady_ns == 0 &&
                       !event.backend_succeeded &&
                       !event.protocol_ack_received,
                   "零 sample 必须显式记录为未 dispatch");
            continue;
        }
        expect(event.issued_at_steady_ns > 0 &&
                   event.protocol_ack_received_at_steady_ns >=
                       event.issued_at_steady_ns &&
                   event.backend_completed_at_steady_ns >=
                       event.protocol_ack_received_at_steady_ns &&
                   event.returned_at_steady_ns >=
                       event.backend_completed_at_steady_ns,
               "pulse event 必须分列 issue、ACK、completion、return 时刻");
    }

    auto stopped_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor stopped;
    expect(stopped.start(
               execution_options(), sequence, stopped_mouse, error) &&
               stopped.consume_source_frame(source_frame(500), error) &&
               stopped.consume_source_frame(source_frame(501), error),
           "人工停止前应完成 baseline 和首 pulse: " + error);
    stopped.request_stop();
    expect(stopped.result().stop_reason ==
               mouse_effect_probe::ProbeStopReason::USER_STOP &&
               stopped.result().cumulative_requested_x_counts == 1 &&
               stopped.result().cumulative_backend_completed_x_counts == 1 &&
               stopped_mouse->commands().size() == 1,
           "人工急停必须保留 residual 并禁止净零补偿");
}

void test_executor_rejects_missing_authority_and_ack() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "授权测试序列应生成成功: " + error);

    auto unauthorized_mouse = std::make_shared<FakeMouseController>();
    auto unauthorized = execution_options();
    unauthorized.physical_output_confirmed = false;
    mouse_effect_probe::MouseEffectProbeExecutor refused;
    expect(!refused.start(
               unauthorized, sequence, unauthorized_mouse, error) &&
               refused.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::AUTHORIZATION_MISSING &&
               unauthorized_mouse->status() == MouseStatus::CLOSED,
           "缺少双重授权必须在打开 Mouse 前拒绝");

    auto ack_mouse = std::make_shared<FakeMouseController>(0, 1);
    mouse_effect_probe::MouseEffectProbeExecutor missing_ack;
    expect(missing_ack.start(
               execution_options(), sequence, ack_mouse, error) &&
               missing_ack.consume_source_frame(source_frame(600), error),
           "ACK 测试 baseline 应正常消费: " + error);
    expect(!missing_ack.consume_source_frame(source_frame(601), error) &&
               missing_ack.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::PROTOCOL_ACK_MISSING &&
               missing_ack.result().cumulative_requested_x_counts == 1 &&
               missing_ack.result().cumulative_backend_completed_x_counts == 1 &&
               ack_mouse->commands().size() == 1,
           "ACK 缺失必须立即停止且不得发送补偿");
}

void test_executor_requires_real_exclusive_owner_lease() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "owner 冲突测试序列应生成成功: " + error);
    MouseOutputOwnerLease blocker;
    expect(blocker.acquire(MouseOutputOwnerScope::CURRENT_PROCESS_TEST,
                           "probe-test-blocker", error),
           "owner 冲突测试必须先持有隔离 lease: " + error);
    MouseConfig mouse_config;
    mouse_config.backend = MouseBackend::WIN32_SEND_INPUT;
    mouse_config.allow_send_input = false;
    auto owned_mouse = MouseDeviceFactory::create(
        mouse_config, MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
    auto mouse = std::shared_ptr<IMouseController>(std::move(owned_mouse));
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(!executor.start(execution_options(), sequence, mouse, error) &&
               executor.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::
                       EXCLUSIVE_OWNER_MISSING &&
               mouse->status() == MouseStatus::CLOSED,
            "probe 必须由 factory lease 提供真实独占事实");
    blocker.release();
}

void test_output_off_rehearsal_records_nominal_timeline_without_mouse() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "output-off rehearsal 测试序列应生成成功: " + error);
    auto options = execution_options();
    options.dispatch_mode =
        mouse_effect_probe::ProbeDispatchMode::OUTPUT_OFF_REHEARSAL;
    options.allow_physical_output = false;
    options.physical_output_confirmed = false;
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(options, sequence, nullptr, error),
           "output-off rehearsal 不应需要 Mouse 或物理授权: " + error);
    for (std::uint64_t index = 0; index < sequence.samples.size(); ++index) {
        auto frame = source_frame(500 + index);
        frame.safety_allowed = false;
        expect(executor.consume_source_frame(frame, error),
               "output-off rehearsal 必须逐 source frame 推进: " + error);
    }
    const auto& result = executor.result();
    expect(result.dispatch_mode ==
               mouse_effect_probe::ProbeDispatchMode::OUTPUT_OFF_REHEARSAL &&
               result.state ==
                   mouse_effect_probe::ProbeExecutionState::COMPLETED &&
               result.complete &&
               result.events.size() == sequence.samples.size() &&
               result.cumulative_requested_x_counts == 0 &&
               result.cumulative_backend_completed_x_counts == 0,
           "rehearsal 完成只能证明 source timeline，实际输入累计必须为零");
    std::size_t nominal_pulse_count = 0;
    for (const auto& event : result.events) {
        if (event.nominal_dx_counts != 0) ++nominal_pulse_count;
        expect(event.nominal_dy_counts == 0 &&
                   !event.dispatch_attempted &&
                   event.requested_dx_counts == 0 &&
                   event.requested_dy_counts == 0 &&
                   !event.backend_succeeded &&
                   !event.protocol_ack_received &&
                   event.source_time_basis == "NDI_SDK_SUBMISSION" &&
                   event.source_clock_status == "VALID" &&
                   event.source_clock_mapping_age_ms == 0.3 &&
                   event.source_clock_sample_count == 8,
               "rehearsal 行必须区分 nominal 与未 dispatch 的实际命令");
    }
    expect(nominal_pulse_count == 4,
           "rehearsal 必须保留四个稀疏名义 pulse 的 source-frame tag");
}

void test_report_is_atomic_bound_and_tamper_evident() {
    TemporaryDirectory temporary;
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "report 测试序列应生成成功: " + error);
    auto options = execution_options();
    options.dispatch_mode =
        mouse_effect_probe::ProbeDispatchMode::OUTPUT_OFF_REHEARSAL;
    options.allow_physical_output = false;
    options.physical_output_confirmed = false;
    mouse_effect_probe::MouseEffectProbeExecutor executor;
    expect(executor.start(options, sequence, nullptr, error),
           "report rehearsal 应启动成功: " + error);
    for (std::uint64_t index = 0; index < sequence.samples.size(); ++index) {
        auto frame = source_frame(700 + index);
        frame.safety_allowed = false;
        expect(executor.consume_source_frame(frame, error),
               "report rehearsal frame 应被消费: " + error);
    }
    mouse_effect_probe::ProbeEvidenceBinding binding;
    binding.probe_binding_sha256 = std::string(64, 'a');
    binding.sidecar_run_uuid = options.run_uuid;
    binding.capture_source_name = "HPSAZZ (Xen-ROI-320)";
    const auto report_path = temporary.path() / "command-report.json";
    std::string report_sha256;
    expect(mouse_effect_probe::write_mouse_effect_probe_report(
               report_path, options, sequence, binding, executor.result(),
               report_sha256, error) &&
               report_sha256.size() == 64 &&
               std::filesystem::is_regular_file(report_path),
           "完整 report 必须原子发布并返回语义 SHA: " + error);
    expect(mouse_effect_probe::verify_mouse_effect_probe_report(
               report_path, error),
           "新发布 report 必须通过独立完整性校验: " + error);
    std::string duplicate_sha;
    expect(!mouse_effect_probe::write_mouse_effect_probe_report(
               report_path, options, sequence, binding, executor.result(),
               duplicate_sha, error),
           "report 发布必须拒绝覆盖已有证据");

    auto invalid_result = executor.result();
    ++invalid_result.cumulative_requested_x_counts;
    const auto invalid_path = temporary.path() / "invalid-report.json";
    expect(!mouse_effect_probe::write_mouse_effect_probe_report(
               invalid_path, options, sequence, binding, invalid_result,
               duplicate_sha, error) &&
               !std::filesystem::exists(invalid_path),
           "累计不守恒的 report 必须在落盘前拒绝");

    std::fstream report(report_path,
                        std::ios::binary | std::ios::in | std::ios::out);
    const std::string content((std::istreambuf_iterator<char>(report)),
                              std::istreambuf_iterator<char>());
    const auto marker = content.find("HPSAZZ");
    expect(marker != std::string::npos,
           "report 必须保留绑定的 capture source name");
    if (marker != std::string::npos) {
        report.clear();
        report.seekp(static_cast<std::streamoff>(marker));
        report.put('J');
        report.flush();
    }
    report.close();
    expect(!mouse_effect_probe::verify_mouse_effect_probe_report(
               report_path, error),
           "report 任一绑定字段被篡改都必须因语义 SHA 失败");
}

void test_file_sha256_uses_exact_bytes() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "binding.json";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "abc";
    }
    std::string digest;
    std::string error;
    expect(mouse_effect_probe::calculate_mouse_effect_probe_file_sha256(
               path, digest, error) &&
               digest ==
                   "ba7816bf8f01cfea414140de5dae2223"
                   "b00361a396177a9cb410ff61f20015ad",
           "binding 文件 SHA 必须覆盖精确字节: " + error);
}

void test_source_sidecar_safety_and_external_stop_are_fail_closed() {
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    expect(mouse_effect_probe::make_sparse_pulse_sequence(
               {.baseline_sample_count = 1,
                .response_sample_count = 1,
                .guard_sample_count = 1},
               sequence, error),
           "stop reason 测试序列应生成成功: " + error);

    auto invalid_timing_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor invalid_timing;
    expect(invalid_timing.start(
               execution_options(), sequence, invalid_timing_mouse, error),
           "invalid timing executor 应启动成功: " + error);
    auto invalid_timing_frame = source_frame(800);
    invalid_timing_frame.source_clock_status = "WARMING";
    expect(!invalid_timing.consume_source_frame(
               invalid_timing_frame, error) &&
               invalid_timing.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SOURCE_TIMING_INVALID,
           "非 VALID source clock 必须立即停发");

    auto sidecar_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor sidecar_lost;
    expect(sidecar_lost.start(
               execution_options(), sequence, sidecar_mouse, error),
           "sidecar stop executor 应启动成功: " + error);
    auto missing_sidecar = source_frame(810);
    missing_sidecar.sidecar_recording = false;
    expect(!sidecar_lost.consume_source_frame(missing_sidecar, error) &&
               sidecar_lost.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SIDECAR_UNAVAILABLE,
           "sidecar 未记录必须立即停发");

    auto safety_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor safety_released;
    expect(safety_released.start(
               execution_options(), sequence, safety_mouse, error),
           "safety stop executor 应启动成功: " + error);
    auto unsafe = source_frame(820);
    unsafe.safety_allowed = false;
    expect(!safety_released.consume_source_frame(unsafe, error) &&
               safety_released.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SAFETY_RELEASED,
           "physical A deadman 释放必须立即停发");

    auto session_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor session_changed;
    expect(session_changed.start(
               execution_options(), sequence, session_mouse, error) &&
               session_changed.consume_source_frame(
                   source_frame(830), error),
           "session change baseline 应先正常消费: " + error);
    auto changed_session = source_frame(831);
    changed_session.source_clock_session_id = "other-clock-session";
    expect(!session_changed.consume_source_frame(changed_session, error) &&
               session_changed.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::
                       SOURCE_SESSION_CHANGED &&
               session_mouse->commands().empty(),
           "source session 改变必须在 pulse 前停发");

    auto dropped_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor dropped_frame;
    expect(dropped_frame.start(
               execution_options(), sequence, dropped_mouse, error),
           "drop stop executor 应启动成功: " + error);
    auto dropped = source_frame(840);
    dropped.transport_dropped_frames = 1;
    expect(!dropped_frame.consume_source_frame(dropped, error) &&
               dropped_frame.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SOURCE_FRAME_GAP,
           "任一 capture/transport drop 事实必须使整个 block 停止");

    auto external_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor external_stop;
    expect(external_stop.start(
               execution_options(), sequence, external_mouse, error),
           "external stop executor 应启动成功: " + error);
    expect(!external_stop.request_stop(
               mouse_effect_probe::ProbeStopReason::NORMAL_COMPLETION,
               error) &&
               external_stop.result().state ==
                   mouse_effect_probe::ProbeExecutionState::RUNNING,
           "外部停止接口不得伪造正常完成");
    expect(external_stop.request_stop(
               mouse_effect_probe::ProbeStopReason::SIDECAR_UNAVAILABLE,
               error) &&
               external_stop.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::SIDECAR_UNAVAILABLE &&
               external_mouse->status() == MouseStatus::CLOSED,
           "sidecar 监督器必须能提交明确 stop reason 并关闭 Mouse");

    auto no_ack = execution_options();
    no_ack.require_protocol_ack = false;
    auto no_ack_mouse = std::make_shared<FakeMouseController>();
    mouse_effect_probe::MouseEffectProbeExecutor no_ack_executor;
    expect(!no_ack_executor.start(
               no_ack, sequence, no_ack_mouse, error) &&
               no_ack_executor.result().stop_reason ==
                   mouse_effect_probe::ProbeStopReason::
                       AUTHORIZATION_MISSING,
           "physical A 不得关闭 protocol ACK 门禁");
}

} // namespace

int main() {
    test_sparse_pulse_sequence_is_x_only_balanced_and_order_swapped();
    test_a2_dependency_calibration_sequences_are_balanced_and_independent();
    test_a2_s1_liveness_sequences_bracket_an_exact_zero_baseline();
    test_sequence_file_round_trip_rejects_overwrite_and_tampering();
    test_executor_consumes_one_sample_per_frame_and_never_catches_up();
    test_executor_failure_stops_without_compensation();
    test_executor_complete_and_user_stop_contracts();
    test_executor_rejects_missing_authority_and_ack();
    test_executor_requires_real_exclusive_owner_lease();
    test_output_off_rehearsal_records_nominal_timeline_without_mouse();
    test_report_is_atomic_bound_and_tamper_evident();
    test_file_sha256_uses_exact_bytes();
    test_source_sidecar_safety_and_external_stop_are_fail_closed();
    if (failures != 0) {
        std::cerr << "Mouse Effect Probe 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Mouse Effect Probe 测试全部通过。\n";
    return 0;
}
