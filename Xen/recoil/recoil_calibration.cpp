#include "recoil/recoil_calibration.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
bool hash_valid(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool bounded(const std::string& value, std::size_t size = 1024) {
    return !value.empty() && value.size() <= size && value.find_first_of("\r\n\0", 0, 3) == std::string::npos;
}
}
std::string recoil_calibration_sha256(std::string_view bytes) {
    if(bytes.size() > 16 * 1024 * 1024) throw std::runtime_error("摘要输入超过16MiB");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("无法初始化SHA256");
    const bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if(hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if(!ok) throw std::runtime_error("SHA256计算失败");
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for(auto byte : digest) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}
std::string recoil_calibration_environment_fingerprint(const RecoilCalibrationEnvironment& e) {
    return recoil_calibration_sha256(nlohmann::json{{"weapon_id",e.weapon_id},{"game_build",e.game_build},
        {"input_path",e.input_path},{"conditions",e.conditions},{"sensitivity",e.sensitivity}}.dump());
}
bool validate_recoil_calibration_manifest(const RecoilCalibrationManifest& m, const RecoilProfile& p, std::string& error) noexcept {
    try {
        if(!validate_recoil_profile(p,error))return false;
        if(p.state != RecoilProfileState::SCHEMA_VALID || !p.calibration.evidence.empty())
            throw std::runtime_error("校准会话只接受无校准证据声明的SCHEMA_VALID候选");
        if(m.schema_version != 1 || !bounded(m.session_id,128) ||
            m.session_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            throw std::runtime_error("校准会话身份无效");
        if(!hash_valid(m.profile_file_sha256) || !hash_valid(m.profile_semantic_sha256) || !hash_valid(m.config_binding_sha256) ||
            m.profile_semantic_sha256 != recoil_calibration_sha256(serialize_recoil_profile(p)))
            throw std::runtime_error("校准候选或配置摘要无效");
        const auto& e=m.environment;
        if(e.weapon_id != p.weapon_id || !bounded(e.game_build) || !bounded(e.conditions) || e.input_path != "kmbox_net" ||
            !std::isfinite(e.sensitivity) || e.sensitivity <= 0 ||
            m.environment_fingerprint != recoil_calibration_environment_fingerprint(e))
            throw std::runtime_error("校准环境缺失或绑定不符");
        const auto& l=m.limits;
        // 资源上界，不声称为武器物理安全阈值；用户仍须显式填写更小额度。
        if(l.max_firing_sessions != 1 || l.max_session_duration_ms <= 0 || l.max_session_duration_ms > 600000 ||
            l.max_firing_duration_ms <= 0 || l.max_firing_duration_ms > 60000 || l.max_firing_duration_ms > l.max_session_duration_ms ||
            l.max_sent_l1_counts == 0 || l.max_sent_l1_counts > 1000000000 || l.max_command_l1_counts <= 0 ||
            l.max_command_l1_counts > 65534 || static_cast<std::uint64_t>(l.max_command_l1_counts) > l.max_sent_l1_counts ||
            l.rolling_window_ms <= 0 || l.rolling_window_ms > 1000 || !std::isfinite(l.rolling_window_counts) ||
            l.rolling_window_counts <= 0 || l.rolling_window_counts > 65534 || !std::isfinite(l.command_phase_budget_ms) ||
            l.command_phase_budget_ms <= 0 || l.command_phase_budget_ms > 1000)
            throw std::runtime_error("校准次数、时间或counts预算无效");
        const auto valid_key=[](int key){return key > 6 && key <= 255 && key != 0x57 && key != 0x41 && key != 0x53 && key != 0x44;};
        if(!valid_key(m.hold_virtual_key) || !valid_key(m.cancel_virtual_key) || m.hold_virtual_key==m.cancel_virtual_key)
            throw std::runtime_error("校准保持键与取消键必须独立且不能使用鼠标或WASD");
        error.clear();return true;
    } catch(const std::exception& e) {error=e.what();return false;} catch(...) {error="校准契约校验异常";return false;}
}
RecoilCalibrationPermit::RecoilCalibrationPermit(RecoilCalibrationManifest m,std::shared_ptr<const RecoilProfile> p,RecoilTime at)
    :manifest_(std::move(m)),profile_(std::move(p)),armed_at_(at){}
RecoilTime RecoilCalibrationPermit::expires_at() const noexcept {
    return armed_at_ + std::chrono::milliseconds(manifest_.limits.max_session_duration_ms);
}
std::shared_ptr<const RecoilCalibrationPermit> authorize_recoil_calibration(const RecoilCalibrationManifest& m,
    std::shared_ptr<const RecoilProfile> p,RecoilTime at,std::string& error) noexcept {
    try {
        if(!p || !validate_recoil_calibration_manifest(m,*p,error))return {};
        if(at==RecoilTime{} || at > RecoilTime::max()-std::chrono::milliseconds(m.limits.max_session_duration_ms)) {
            error="校准授权时间无效";return {};
        }
        return std::shared_ptr<const RecoilCalibrationPermit>(new RecoilCalibrationPermit(m,std::move(p),at));
    }catch(...){error="校准授权创建失败";return {};}
}
const char* RecoilCalibrationEndName(RecoilCalibrationEnd value) noexcept {
    switch(value) {
    case RecoilCalibrationEnd::NONE:return "NONE";case RecoilCalibrationEnd::COMPLETED:return "COMPLETED";
    case RecoilCalibrationEnd::CANCELED:return "CANCELED";case RecoilCalibrationEnd::TIME_LIMIT:return "TIME_LIMIT";
    case RecoilCalibrationEnd::COUNT_LIMIT:return "COUNT_LIMIT";case RecoilCalibrationEnd::INVALID_TIME:return "INVALID_TIME";
    case RecoilCalibrationEnd::CONTEXT:return "CONTEXT";case RecoilCalibrationEnd::UNKNOWN_RECEIPT:return "UNKNOWN_RECEIPT";
    case RecoilCalibrationEnd::NOT_SENT:return "NOT_SENT";
    }return "UNKNOWN";
}
RecoilCalibrationBudget::RecoilCalibrationBudget(std::shared_ptr<const RecoilCalibrationPermit> p):permit_(std::move(p)) {
    if(!permit_)finish(RecoilCalibrationEnd::CONTEXT);
}
void RecoilCalibrationBudget::finish(RecoilCalibrationEnd end) noexcept {
    if(!state_.terminal){state_.terminal=true;state_.end=end==RecoilCalibrationEnd::NONE?RecoilCalibrationEnd::CANCELED:end;}
}
bool RecoilCalibrationBudget::check_time(RecoilTime now) noexcept {
    if(state_.terminal)return false;
    if(now==RecoilTime{} || now<permit_->armed_at() || now<last_now_) {finish(RecoilCalibrationEnd::INVALID_TIME);return false;}
    last_now_=now;
    if(now>=permit_->expires_at() || (state_.firing_sessions && now-fired_at_>=std::chrono::milliseconds(permit_->limits().max_firing_duration_ms))) {
        finish(RecoilCalibrationEnd::TIME_LIMIT);return false;
    }
    return true;
}
bool RecoilCalibrationBudget::begin_firing(RecoilTime now) noexcept {
    if(!check_time(now))return false;
    if(state_.firing_sessions) {finish(RecoilCalibrationEnd::COUNT_LIMIT);return false;}
    state_.firing_sessions=1;fired_at_=now;return true;
}
bool RecoilCalibrationBudget::reserve(int dx,int dy,RecoilTime now) noexcept {
    if(!check_time(now))return false;
    if(!state_.firing_sessions) {finish(RecoilCalibrationEnd::CONTEXT);return false;}
    const auto magnitude=[](int value){const auto wide=static_cast<std::int64_t>(value);return static_cast<std::uint64_t>(wide<0?-wide:wide);};
    const auto count=magnitude(dx)+magnitude(dy);
    const auto& limits=permit_->limits();
    if(count > static_cast<std::uint64_t>(limits.max_command_l1_counts) || count>limits.max_sent_l1_counts-state_.sent_l1_counts) {
        finish(RecoilCalibrationEnd::COUNT_LIMIT);return false;
    }
    state_.sent_l1_counts+=count;return true;
}
