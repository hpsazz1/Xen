#ifndef MOUSE_EFFECT_PROBE_H
#define MOUSE_EFFECT_PROBE_H

#include "mouse/mouse.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mouse_effect_probe {

enum class ProbeSamplePhase {
    BASELINE,
    PULSE,
    RESPONSE,
    HOLD,
    GUARD,
    PERIOD,
    INVERTED_PERIOD,
    RETURN_TO_ZERO,
};

struct SparsePulseSequenceRequest {
    std::uint64_t baseline_sample_count = 0;
    std::uint64_t response_sample_count = 0;
    std::uint64_t guard_sample_count = 0;
};

enum class DependencyCalibrationRunRole {
    P_CAL,
    P_HOLDOUT,
};

struct DependencyCalibrationSequenceRequest {
    std::uint64_t baseline_sample_count = 0;
    std::uint64_t response_sample_count = 0;
    std::uint64_t guard_sample_count = 0;
    std::uint64_t block_count = 0;
    DependencyCalibrationRunRole run_role =
        DependencyCalibrationRunRole::P_CAL;
};

enum class S1LivenessRunRole {
    PRIMARY,
    VALIDATION,
};

struct S1LivenessSequenceRequest {
    std::uint64_t challenge_pulse_count = 0;
    std::uint64_t challenge_stride_sample_count = 0;
    std::uint64_t peak_hold_sample_count = 0;
    std::uint64_t settle_sample_count = 0;
    std::uint64_t baseline_sample_count = 0;
    S1LivenessRunRole run_role = S1LivenessRunRole::PRIMARY;
};

struct PhysicalBPrimarySequenceRequest {
    std::uint64_t guard_sample_count = 0;
    std::uint32_t lfsr_order = 0;
    std::uint32_t feedback_mask = 0;
    std::uint64_t seed = 0;
    std::uint64_t phase = 0;
    std::string offline_sequence_semantic_sha256;
};

struct ProbeSequenceSample {
    std::uint64_t sample_index = 0;
    std::uint64_t block_id = 0;
    ProbeSamplePhase phase = ProbeSamplePhase::GUARD;
    int dx_counts = 0;
    int dy_counts = 0;
};

enum class ProbeSequenceBlockRole {
    UNSPECIFIED,
    ESTIMATION,
    WITHIN_RUN_VALIDATION,
};

enum class ProbeSequenceBlockPolarity {
    UNSPECIFIED,
    NORMAL,
    INVERTED,
};

struct ProbeSequenceBlock {
    std::uint64_t block_id = 0;
    std::uint64_t pair_index = 0;
    ProbeSequenceBlockRole role = ProbeSequenceBlockRole::UNSPECIFIED;
    ProbeSequenceBlockPolarity polarity =
        ProbeSequenceBlockPolarity::UNSPECIFIED;
    std::uint64_t first_sample_index = 0;
    std::uint64_t period_sample_count = 0;
    std::uint64_t sample_count = 0;
    int first_pulse_dx_counts = 0;
    int second_pulse_dx_counts = 0;
};

struct MouseEffectProbeSequence {
    std::uint32_t schema = 0;
    std::string profile;
    SparsePulseSequenceRequest request;
    DependencyCalibrationSequenceRequest dependency_calibration_request;
    S1LivenessSequenceRequest s1_liveness_request;
    PhysicalBPrimarySequenceRequest physical_b_primary_request;
    std::vector<ProbeSequenceBlock> blocks;
    std::vector<ProbeSequenceSample> samples;
    std::int64_t net_x_counts = 0;
    std::uint64_t max_abs_prefix_x_counts = 0;
    std::string sequence_sha256;
};

// 生成固定的 +1/-1、-1/+1 两个稀疏块；sample 时刻由后续 source-frame
// executor 决定，本模块不拥有 Mouse、时钟或物理输出。
bool make_sparse_pulse_sequence(
    const SparsePulseSequenceRequest& request,
    MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

// A2 依赖校准只复用 A 级安全骨架。每个完整 block 都包含独立 pre/post
// zero guard，并在 0 与单 count 位置间往返；P-CAL 与 P-HOLDOUT 使用相反
// 的预注册平衡顺序和不同语义 SHA，不在执行期随机化或自动追加。
bool make_dependency_calibration_sequence(
    const DependencyCalibrationSequenceRequest& request,
    MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

// S1 活性序列只为静态数字 baseline 提供前后正控制。两段挑战以固定
// source-frame cadence 发送 X-only 单 count；可在原幅度峰值插入不 dispatch
// 的零命令 hold，随后各自回锚。hold/settle/baseline 全零，challenge、hold
// 与 settle 永远不得进入零扰动或分辨率估计。
bool make_s1_liveness_sequence(
    const S1LivenessSequenceRequest& request,
    MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

// Physical B Primary 只生成 F0 已冻结的 cumulative-position m-sequence：
// 两个完整 normal/inverted pair，实际差分命令为 X-only {-1,0,+1}，
// 每个 block 显式回零且 guard 独立；当前接口不提供 cross-Run holdout。
bool make_physical_b_primary_sequence(
    const PhysicalBPrimarySequenceRequest& request,
    MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

bool validate_mouse_effect_probe_sequence(
    const MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

// 文件发布拒绝覆盖；sequence_sha256 绑定规范化语义载荷，不冒充文件哈希。
bool write_mouse_effect_probe_sequence(
    const std::filesystem::path& path,
    const MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

bool read_mouse_effect_probe_sequence(
    const std::filesystem::path& path,
    MouseEffectProbeSequence& sequence,
    std::string& error) noexcept;

const char* probe_sample_phase_name(ProbeSamplePhase phase) noexcept;
const char* dependency_calibration_run_role_name(
    DependencyCalibrationRunRole role) noexcept;
const char* s1_liveness_run_role_name(S1LivenessRunRole role) noexcept;
const char* probe_sequence_block_role_name(
    ProbeSequenceBlockRole role) noexcept;
const char* probe_sequence_block_polarity_name(
    ProbeSequenceBlockPolarity polarity) noexcept;

enum class ProbeExecutionState {
    IDLE,
    RUNNING,
    COMPLETED,
    STOPPED,
};

enum class ProbeDispatchMode {
    // 只验证真实 source-frame/sidecar 时间线；不得打开 Mouse，也不产生请求。
    OUTPUT_OFF_REHEARSAL,
    // A 级稀疏脉冲物理执行；仍需独占 owner、双重授权和安全门。
    PHYSICAL_A,
    // B 级冻结 PRBS Primary；使用独立确认令牌，不与 A 级权限复用。
    PHYSICAL_B,
};

enum class ProbeStopReason {
    NONE,
    NORMAL_COMPLETION,
    AUTHORIZATION_MISSING,
    EXCLUSIVE_OWNER_MISSING,
    SOURCE_TIMING_INVALID,
    SOURCE_SESSION_CHANGED,
    SOURCE_FRAME_GAP,
    SIDECAR_UNAVAILABLE,
    SAFETY_RELEASED,
    MOUSE_FAILURE,
    PROTOCOL_ACK_MISSING,
    RUN_TIMEOUT,
    USER_STOP,
};

struct ProbeExecutionOptions {
    std::string run_uuid;
    std::uint64_t activation_epoch = 0;
    ProbeDispatchMode dispatch_mode = ProbeDispatchMode::PHYSICAL_A;
    bool allow_physical_output = false;
    bool physical_output_confirmed = false;
    bool require_protocol_ack = true;
};

struct ProbeSourceFrameEvent {
    std::uint64_t source_frame_sequence = 0;
    std::int64_t source_timestamp = 0;
    bool source_timestamp_valid = false;
    std::int64_t source_time_at_steady_ns = 0;
    std::string source_time_basis;
    std::string source_clock_status;
    std::string source_clock_session_id;
    double source_clock_uncertainty_ms = 0.0;
    double source_clock_rtt_ms = 0.0;
    double source_clock_mapping_age_ms = 0.0;
    std::uint64_t source_clock_sample_count = 0;
    std::uint64_t source_dropped_frames = 0;
    std::uint64_t transport_dropped_frames = 0;
    std::uint64_t transport_invalid_packets = 0;
    bool source_timing_valid = false;
    bool sidecar_recording = false;
    bool safety_allowed = false;
};

struct ProbeCommandEvent {
    std::string run_uuid;
    std::uint64_t activation_epoch = 0;
    std::uint64_t block_id = 0;
    std::string sequence_sha256;
    std::uint64_t sample_index = 0;
    std::uint64_t source_frame_sequence = 0;
    std::int64_t source_timestamp = 0;
    bool source_timestamp_valid = false;
    std::int64_t source_time_at_steady_ns = 0;
    std::string source_time_basis;
    std::string source_clock_status;
    std::string source_clock_session_id;
    double source_clock_uncertainty_ms = 0.0;
    double source_clock_rtt_ms = 0.0;
    double source_clock_mapping_age_ms = 0.0;
    std::uint64_t source_clock_sample_count = 0;
    std::uint64_t source_dropped_frames = 0;
    std::uint64_t transport_dropped_frames = 0;
    std::uint64_t transport_invalid_packets = 0;
    std::int64_t scheduled_at_steady_ns = 0;
    std::int64_t issued_at_steady_ns = 0;
    int nominal_dx_counts = 0;
    int nominal_dy_counts = 0;
    int requested_dx_counts = 0;
    int requested_dy_counts = 0;
    bool safety_allowed = false;
    bool dispatch_attempted = false;
    bool backend_succeeded = false;
    std::int64_t backend_completed_at_steady_ns = 0;
    bool protocol_ack_received = false;
    std::int64_t protocol_ack_received_at_steady_ns = 0;
    std::int64_t returned_at_steady_ns = 0;
    MouseStatus mouse_status = MouseStatus::CLOSED;
    std::int64_t cumulative_requested_x_counts = 0;
    std::int64_t cumulative_backend_completed_x_counts = 0;
    ProbeStopReason stop_reason = ProbeStopReason::NONE;
};

struct ProbeExecutionResult {
    ProbeDispatchMode dispatch_mode = ProbeDispatchMode::PHYSICAL_A;
    ProbeExecutionState state = ProbeExecutionState::IDLE;
    ProbeStopReason stop_reason = ProbeStopReason::NONE;
    bool complete = false;
    std::uint64_t consumed_sample_count = 0;
    std::int64_t cumulative_requested_x_counts = 0;
    std::int64_t cumulative_backend_completed_x_counts = 0;
    std::vector<ProbeCommandEvent> events;
};

struct ProbeEvidenceBinding {
    // 与 command report 和像素 sidecar 共同引用的 probe binding 文件哈希。
    std::string probe_binding_sha256;
    std::string sidecar_run_uuid;
    std::string capture_source_name;
};

class MouseEffectProbeExecutor {
public:
    MouseEffectProbeExecutor() noexcept;
    ~MouseEffectProbeExecutor();

    MouseEffectProbeExecutor(const MouseEffectProbeExecutor&) = delete;
    MouseEffectProbeExecutor& operator=(const MouseEffectProbeExecutor&) =
        delete;

    bool start(const ProbeExecutionOptions& options,
               const MouseEffectProbeSequence& sequence,
               std::shared_ptr<IMouseController> mouse,
               std::string& error) noexcept;
    bool consume_source_frame(const ProbeSourceFrameEvent& frame,
                              std::string& error) noexcept;
    void request_stop() noexcept;
    bool request_stop(ProbeStopReason reason,
                      std::string& error) noexcept;
    const ProbeExecutionResult& result() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 只接受已终止或正常完成的执行结果；拒绝覆盖并通过同目录临时文件原子发布。
bool write_mouse_effect_probe_report(
    const std::filesystem::path& path,
    const ProbeExecutionOptions& options,
    const MouseEffectProbeSequence& sequence,
    const ProbeEvidenceBinding& binding,
    const ProbeExecutionResult& result,
    std::string& report_sha256,
    std::string& error) noexcept;

// 校验报告 schema 与规范化语义 SHA；不把哈希解释为签名或物理效果证明。
bool verify_mouse_effect_probe_report(
    const std::filesystem::path& path,
    std::string& error) noexcept;

bool calculate_mouse_effect_probe_file_sha256(
    const std::filesystem::path& path,
    std::string& sha256,
    std::string& error) noexcept;

const char* probe_execution_state_name(ProbeExecutionState state) noexcept;
const char* probe_dispatch_mode_name(ProbeDispatchMode mode) noexcept;
const char* probe_stop_reason_name(ProbeStopReason reason) noexcept;

} // namespace mouse_effect_probe

#endif // MOUSE_EFFECT_PROBE_H
