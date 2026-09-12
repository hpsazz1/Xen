#ifndef RECOIL_H
#define RECOIL_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using RecoilClock = std::chrono::steady_clock;
using RecoilTime = RecoilClock::time_point;
enum class RecoilProfileState { IMPORTED, SCHEMA_VALID, CALIBRATED, ACCEPTED };
struct RecoilPoint {
    double time_ms = 0, x_counts = 0, y_counts = 0;
};
struct RecoilSource {
    std::string repository, commit, sha256, license, source_unit, conversion_revision;
    bool redistribution_verified = false;
};
struct RecoilCalibration {
    std::string game_build, input_path, conditions, evidence;
    std::optional<double> sensitivity;
};
struct RecoilProfile {
    std::uint32_t schema_version = 1;
    std::string id, weapon_id;
    std::uint64_t revision = 1;
    std::string unit = "device_counts", sample_semantics = "cumulative", fire_mode = "automatic";
    RecoilProfileState state = RecoilProfileState::IMPORTED;
    RecoilSource source;
    RecoilCalibration calibration;
    // 缺失表示未经验证，不能自行补常量获得执行资格。
    std::optional<double> phase_tolerance_ms, recovery_ms;
    std::vector<RecoilPoint> points;
};
struct RecoilTuning {
    double x_strength = 1, y_strength = 1;
    // 起压之前保持零；有效时长只伸缩首个非零节点之后，不改变总位移。
    double start_offset_ms = 0, time_scale = 1;
};
bool validate_recoil_profile(const RecoilProfile& profile, std::string& error) noexcept;
bool load_recoil_profile(std::string_view json, RecoilProfile& profile, std::string& error) noexcept;
std::string serialize_recoil_profile(const RecoilProfile& profile);
bool compile_recoil_profile(const RecoilProfile& base, const RecoilTuning& tuning,
    RecoilProfile& output, std::string& error) noexcept;
// 生产与预览共用分段线性累计求值器；时域外钳制，不循环或外推尾斜率。
RecoilPoint sample_recoil_profile(const RecoilProfile& profile, double elapsed_ms) noexcept;

enum class RecoilPhase { DISABLED, WAIT_CONTEXT, WAIT_RELEASE, READY, FIRING, PENDING, EXHAUSTED, FAULT };
enum class RecoilReason {
    NONE, DISABLED, INVALID_PROFILE, UNCALIBRATED, CONTEXT, WAIT_RELEASE,
    RESET_UNVERIFIED, PROFILE_CHANGED, RELEASED, CANCELED, LATE,
    EXHAUSTED, NOT_SENT, UNKNOWN_RECEIPT, COMMAND_PENDING, INVALID_TIME, LIMIT
};
const char* RecoilReasonName(RecoilReason reason) noexcept;
struct RecoilInput {
    bool enabled = false, held = false, healthy = false, focused = false, permission = false;
    bool profile_conditions_match = false, recovery_qualified = false;
    std::uint64_t weapon_generation = 0, device_epoch = 0;
    RecoilTime firing_started_at{};
    std::shared_ptr<const RecoilProfile> profile;
};
struct RecoilIntent {
    std::uint64_t command_id = 0, session_id = 0;
    std::uint64_t profile_revision = 0, weapon_generation = 0, device_epoch = 0;
    RecoilTime planned_at{}, expires_at{};
    int dx_counts = 0, dy_counts = 0;
};
struct RecoilSnapshot {
    RecoilPhase phase = RecoilPhase::DISABLED;
    RecoilReason reason = RecoilReason::DISABLED;
    std::uint64_t session_id = 0, command_id = 0;
    bool faulted = false, pending = false;
    double planned_x = 0, planned_y = 0, confirmed_x = 0, confirmed_y = 0;
    double discarded_x = 0, discarded_y = 0, unknown_x = 0, unknown_y = 0;
    double remainder_x = 0, remainder_y = 0;
};
struct RecoilDecision {
    bool has_intent = false;
    RecoilIntent intent;
    RecoilTime next_deadline{};
    RecoilSnapshot snapshot;
};
enum class RecoilReceiptStatus { ACKNOWLEDGED, NOT_SENT, UNKNOWN };
struct RecoilReceipt {
    std::uint64_t command_id = 0;
    RecoilReceiptStatus status = RecoilReceiptStatus::UNKNOWN;
    RecoilTime completed_at{};
};
// 固定的软件调度条件，不是实测相位或物理许可，不写回候选profile。
struct RecoilCandidateReplayOptions {
    double step_ms = 1, phase_budget_ms = 20;
};
struct RecoilCandidateReplayReport {
    bool validated = false;
    double step_ms = 0, phase_budget_ms = 0, duration_ms = 0;
    std::uint64_t advance_calls = 0, advance_limit = 0;
    // ACK统计与terminal只来自固定步长的正常回放；故障注入不混入成功量。
    std::uint64_t acknowledged_commands = 0, acknowledged_l1_counts = 0;
    int max_abs_command_axis_counts = 0, command_axis_limit_counts = 0;
    RecoilSnapshot terminal;
    bool phase_edge_checked = false, tail_checked = false, deadline_checked = false;
    bool cancellation_checked = false, unknown_receipt_checked = false, not_sent_checked = false;
};
// 仅接受无校准声明的SCHEMA_VALID候选。内部复用真实Controller与模拟ACK；
// 不返回Controller/Permit，不连接Worker，不验证Worker累计或滚动预算。
bool validate_recoil_candidate_execution(const RecoilProfile& candidate,
    const RecoilCandidateReplayOptions& options, RecoilCandidateReplayReport& report,
    std::string& error) noexcept;
// 单线程纯计算；没有设备、线程或隐式全局时钟。每次只有一个未决意图。
class RecoilCalibrationPermit;
class RecoilController {
public:
    RecoilController() = default;
    explicit RecoilController(std::shared_ptr<const RecoilCalibrationPermit> permit);
    RecoilController(const RecoilController&) = delete;
    RecoilController& operator=(const RecoilController&) = delete;
    RecoilController(RecoilController&&) noexcept = default;
    RecoilController& operator=(RecoilController&&) noexcept = default;
    RecoilDecision advance(const RecoilInput& input, RecoilTime now) noexcept;
    RecoilDecision acknowledge(const RecoilReceipt& receipt, RecoilTime now) noexcept;
    RecoilDecision cancel(RecoilReason reason, RecoilTime now) noexcept;
    RecoilSnapshot snapshot() const noexcept { return state_; }
private:
    struct OfflineReplayTag {};
    RecoilController(OfflineReplayTag, double phase_budget_ms) noexcept;
    friend bool validate_recoil_candidate_execution(const RecoilProfile&,
        const RecoilCandidateReplayOptions&, RecoilCandidateReplayReport&, std::string&) noexcept;
    double phase_budget_ms() const noexcept;
    double offline_phase_budget_ms_ = 0;
    std::shared_ptr<const RecoilCalibrationPermit> calibration_permit_;
    bool calibration_claimed_ = false;
    RecoilDecision result() const noexcept;
    RecoilSnapshot state_;
    std::shared_ptr<const RecoilProfile> profile_;
    RecoilIntent pending_;
    RecoilPoint sampled_{};
    RecoilPoint pending_sample_{};
    double pending_remainder_x_ = 0, pending_remainder_y_ = 0;
    RecoilTime started_at_{}, last_sample_at_{}, last_now_{}, released_at_{};
    std::uint64_t weapon_generation_ = 0, device_epoch_ = 0, next_command_id_ = 0;
    bool release_seen_ = false, active_ = false, has_fired_ = false, released_since_firing_ = false;
    bool profile_valid_ = false;
};
#endif
