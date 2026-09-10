#include "debug/recoil_report.h"
#include "recoil_tuner/recoil_tuner.h"
#include <nlohmann/json.hpp>
#include <set>

std::string recoil_metadata_json(const RecoilConfig& config, const RuntimeSnapshot& snapshot) {
    using Json = nlohmann::json;
    Json result = {{"schema",1}, {"physical_acceptance",nullptr}, {"config",{
        {"enabled",config.enabled},{"mixed_aim",config.mixed_aim},{"hold_virtual_key",config.hold_virtual_key},
        {"game_build",config.game_build},{"conditions",config.conditions},{"input_path",config.input_path},
        {"sensitivity",config.sensitivity},{"fire_mode",config.fire_mode},{"use_trial",config.use_trial},
        {"trial_file",config.trial_file},{"budget_window_ms",config.budget_window_ms},
        {"max_observation_age_ms",config.max_observation_age_ms}}}};
    result["final"] = nullptr; result["execution"] = nullptr;
    if (snapshot.recoil_telemetry_available) {
        const auto& state = snapshot.recoil;
        result["final"] = {{"phase",static_cast<int>(state.phase)}, {"reason",RecoilReasonName(state.reason)},
            {"session_id",state.session_id ? Json(state.session_id) : Json(nullptr)}, {"faulted",state.faulted},
            {"pending",state.pending},{"planned",{state.planned_x,state.planned_y}},
            {"confirmed",{state.confirmed_x,state.confirmed_y}}, {"discarded",{state.discarded_x,state.discarded_y}},
            {"unknown",{state.unknown_x,state.unknown_y}}, {"profile_status",snapshot.recoil_profile_status}};
        Json records = Json::array(), profiles = Json::array();
        std::set<std::string> seen;
        const auto stamp = [](RecoilTime time) -> Json {
            if (time == RecoilTime{}) return nullptr;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        };
        for (const auto& record : snapshot.recoil_execution_log.records) {
            const auto& intent = record.intent;
            const std::string profile_key = record.profile ? record.profile->id + ":" + std::to_string(record.profile->revision) : "";
            if (record.profile && seen.insert(profile_key).second) {
                auto profile = Json::parse(serialize_recoil_profile(*record.profile));
                profile["environment_fingerprint"] = recoil_tuner::environment_fingerprint(*record.profile);
                profiles.push_back(std::move(profile));
            }
            records.push_back({{"command_id",intent.command_id},{"firing_id",intent.session_id},
                {"profile",profile_key.empty() ? Json(nullptr) : Json(profile_key)}, {"weapon_generation",intent.weapon_generation},
                {"device_epoch",intent.device_epoch},{"planned_at_steady_ns",stamp(intent.planned_at)},
                {"firing_started_at_steady_ns",stamp(record.firing_started_at)},
                {"firing_source",RecoilFiringSourceName(record.firing_source)},
                {"expires_at_steady_ns",stamp(intent.expires_at)}, {"requested_counts",{intent.dx_counts,intent.dy_counts}},
                {"backend_called",record.backend_called},{"receipt",record.receipt.status == RecoilReceiptStatus::ACKNOWLEDGED ? "ACKNOWLEDGED" :
                    record.receipt.status == RecoilReceiptStatus::NOT_SENT ? "NOT_SENT" : "UNKNOWN"},
                {"completed_at_steady_ns",stamp(record.receipt.completed_at)}, {"physical_effect_observed",nullptr}});
        }
        result["execution"] = {{"clock_domain","local_steady"},{"dropped_count",snapshot.recoil_execution_log.dropped_count},
            {"records",records},{"profiles",profiles}};
    }
    const auto& weapon = snapshot.weapon_snapshot;
    result["weapon"] = {{"status",weapon::status_name(weapon.status)}, {"valid",weapon.valid},
        {"id",weapon.canonical_id.empty() ? Json(nullptr) : Json(weapon.canonical_id)},
        {"source_epoch",weapon.source_epoch ? Json(weapon.source_epoch) : Json(nullptr)},
        {"source_order_verified",weapon.source_order_verified}};
    return result.dump();
}
