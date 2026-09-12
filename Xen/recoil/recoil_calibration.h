#ifndef RECOIL_CALIBRATION_H
#define RECOIL_CALIBRATION_H
#include "recoil/recoil.h"
#include <atomic>

struct RecoilCalibrationEnvironment {
    std::string weapon_id, game_build, input_path, conditions;
    double sensitivity = 0;
};
struct RecoilCalibrationLimits {
    std::uint32_t max_firing_sessions = 1;
    int max_session_duration_ms = 0, max_firing_duration_ms = 0;
    std::uint64_t max_sent_l1_counts = 0;
    int max_command_l1_counts = 0, rolling_window_ms = 0;
    double rolling_window_counts = 0, command_phase_budget_ms = 0;
};
struct RecoilCalibrationManifest {
    std::uint32_t schema_version = 1;
    std::string session_id, profile_file_sha256, profile_semantic_sha256;
    std::string environment_fingerprint, config_binding_sha256;
    RecoilCalibrationEnvironment environment;
    RecoilCalibrationLimits limits;
    int hold_virtual_key = 0, cancel_virtual_key = 0;
};
// 摘要不包含秘密明文；调用失败抛异常，由离线入口统一收敛。
std::string recoil_calibration_sha256(std::string_view bytes);
std::string recoil_calibration_environment_fingerprint(const RecoilCalibrationEnvironment&);
bool validate_recoil_calibration_manifest(const RecoilCalibrationManifest&,
    const RecoilProfile&, std::string& error) noexcept;

class RecoilCalibrationPermit final {
public:
    const std::shared_ptr<const RecoilProfile>& profile() const noexcept { return profile_; }
    const RecoilCalibrationLimits& limits() const noexcept { return manifest_.limits; }
    const RecoilCalibrationManifest& manifest() const noexcept { return manifest_; }
    const std::string& session_id() const noexcept { return manifest_.session_id; }
    RecoilTime armed_at() const noexcept { return armed_at_; }
    RecoilTime expires_at() const noexcept;
    bool matches(const std::shared_ptr<const RecoilProfile>& profile) const noexcept { return profile == profile_; }
    // Controller独占消费同一许可对象；新Worker不能重用已领取的permit。
    bool try_claim_controller() const noexcept { bool expected=false;return claimed_.compare_exchange_strong(expected,true); }
private:
    friend std::shared_ptr<const RecoilCalibrationPermit> authorize_recoil_calibration(
        const RecoilCalibrationManifest&, std::shared_ptr<const RecoilProfile>, RecoilTime, std::string&) noexcept;
    RecoilCalibrationPermit(RecoilCalibrationManifest, std::shared_ptr<const RecoilProfile>, RecoilTime);
    RecoilCalibrationManifest manifest_;
    std::shared_ptr<const RecoilProfile> profile_;
    RecoilTime armed_at_{};
    mutable std::atomic<bool> claimed_{false};
};
std::shared_ptr<const RecoilCalibrationPermit> authorize_recoil_calibration(
    const RecoilCalibrationManifest&, std::shared_ptr<const RecoilProfile>,
    RecoilTime armed_at, std::string& error) noexcept;

enum class RecoilCalibrationEnd { NONE, COMPLETED, CANCELED, TIME_LIMIT, COUNT_LIMIT,
    INVALID_TIME, CONTEXT, UNKNOWN_RECEIPT, NOT_SENT };
const char* RecoilCalibrationEndName(RecoilCalibrationEnd) noexcept;
struct RecoilCalibrationBudgetSnapshot {
    std::uint32_t firing_sessions = 0;
    std::uint64_t sent_l1_counts = 0;
    bool terminal = false;
    RecoilCalibrationEnd end = RecoilCalibrationEnd::NONE;
};
// 单线程纯账本。reserve只在紧邻后端调用前使用；已预留量永不退款。
class RecoilCalibrationBudget {
public:
    explicit RecoilCalibrationBudget(std::shared_ptr<const RecoilCalibrationPermit>);
    bool check_time(RecoilTime now) noexcept;
    bool begin_firing(RecoilTime now) noexcept;
    bool reserve(int dx, int dy, RecoilTime now) noexcept;
    void finish(RecoilCalibrationEnd) noexcept;
    RecoilCalibrationBudgetSnapshot snapshot() const noexcept { return state_; }
private:
    std::shared_ptr<const RecoilCalibrationPermit> permit_;
    RecoilCalibrationBudgetSnapshot state_;
    RecoilTime fired_at_{}, last_now_{};
};
#endif
