#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace movement::timeline {
namespace {
constexpr int kMaxTime = 60000;
constexpr std::array<const char*, 6> kKeys{"W", "A", "S", "D", "SPACE", "CTRL"};
bool ack(const KeyboardReceipt& r) { return r.disposition == KeyboardDisposition::ACKNOWLEDGED; }
void fields(const nlohmann::json& j, std::initializer_list<const char*> allowed) {
    if (!j.is_object() || j.size() != allowed.size()) throw std::runtime_error("方案字段缺失或包含未知字段");
    for (auto name : allowed) if (!j.contains(name)) throw std::runtime_error("方案缺少必要字段");
}
int integer(const nlohmann::json& j) {
    if (!j.is_number_integer() || j < -1 || j > kMaxTime) throw std::runtime_error("毫秒数或整数参数超出范围");
    return j.get<int>();
}
int mask_start(const Plan& p) {
    if (p.mask.start_ms >= 0) return p.mask.start_ms;
    int start = kMaxTime;
    for (const auto& a : p.actions)
        if (a.kind == ActionKind::KEY && a.key == Key::SPACE) start = std::min(start, a.start_ms);
    return start;
}
}
int duration_ms(const Plan& p) noexcept {
    int end = 0;
    for (const auto& a : p.actions)
        if (a.start_ms >= 0 && a.start_ms <= kMaxTime && a.duration_ms > 0 && a.duration_ms <= kMaxTime)
            end = std::max(end, a.start_ms + a.duration_ms);
    return end;
}
bool validate_plan(const Plan& p, std::string& error) noexcept {
    try {
        error.clear();
        if (p.name.empty() || p.name.size() > 256 || p.map.size() > 256 || p.location.size() > 256 || p.notes.size() > 4096)
            throw std::runtime_error("名称不能为空，名称/地图/点位最多256字节，说明最多4096字节");
        if (p.actions.empty() || p.actions.size() > 256) throw std::runtime_error("方案需要1至256个动作");
        for (std::size_t i = 0; i < p.actions.size(); ++i) {
            const auto& a = p.actions[i];
            if (a.start_ms < 0 || a.duration_ms < 1 || a.start_ms > kMaxTime || a.duration_ms > kMaxTime - a.start_ms)
                throw std::runtime_error("动作时长必须为正整数，整段最多60000毫秒");
            if (a.kind == ActionKind::KEY) {
                if (static_cast<unsigned>(a.key) >= kKeys.size()) throw std::runtime_error("不支持的按键");
            } else if (a.kind == ActionKind::TURN) {
                if (!std::isfinite(a.angle_degrees) || a.angle_degrees == 0 || std::abs(a.angle_degrees) > 360)
                    throw std::runtime_error("旋转角度必须在-360至360度内且不能为零");
                for (std::size_t n = 0; n < i; ++n) {
                    const auto& b = p.actions[n];
                    if (b.kind == ActionKind::TURN && a.start_ms < b.start_ms + b.duration_ms && b.start_ms < a.start_ms + a.duration_ms)
                        throw std::runtime_error("鼠标旋转动作不能重叠");
                }
            } else throw std::runtime_error("不支持的动作类型");
        }
        if (p.mask.wasd < 0 || p.mask.wasd > 15 || p.mask.start_ms < -1 || p.mask.start_ms > kMaxTime ||
            p.mask.duration_ms < 0 || p.mask.duration_ms > kMaxTime)
            throw std::runtime_error("物理按键屏蔽参数超出范围");
        if (p.mask.enabled && p.mask.wasd && p.mask.start_ms >= 0 && p.mask.start_ms >= duration_ms(p))
            throw std::runtime_error("屏蔽起点必须位于动作范围内");
        if (p.mask.enabled && p.mask.wasd && p.mask.duration_ms && mask_start(p) + p.mask.duration_ms > duration_ms(p))
            throw std::runtime_error("屏蔽区间不能超过动作结束时间");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool parse_plan(const nlohmann::json& j, Plan& out, std::string& error) noexcept {
    try {
        fields(j, {"schema_version", "name", "map", "location", "notes", "actions", "mask"});
        if (integer(j.at("schema_version")) != 1) throw std::runtime_error("不支持的方案版本");
        Plan p;
        p.name = j.at("name").get<std::string>(); p.map = j.at("map").get<std::string>();
        p.location = j.at("location").get<std::string>(); p.notes = j.at("notes").get<std::string>();
        const auto& list = j.at("actions");
        if (!list.is_array() || list.size() > 256) throw std::runtime_error("动作必须是最多256项的数组");
        for (const auto& item : list) {
            Action a;
            const auto type = item.at("type").get<std::string>();
            if (type == "key") {
                fields(item, {"type", "key", "start_ms", "duration_ms"});
                const auto key = item.at("key").get<std::string>();
                const auto found = std::find(kKeys.begin(), kKeys.end(), key);
                if (found == kKeys.end()) throw std::runtime_error("不支持的按键名称");
                a.key = static_cast<Key>(found - kKeys.begin());
            } else if (type == "turn") {
                fields(item, {"type", "angle_degrees", "start_ms", "duration_ms"});
                a.kind = ActionKind::TURN;
                if (!item.at("angle_degrees").is_number()) throw std::runtime_error("角度必须是数字");
                a.angle_degrees = item.at("angle_degrees").get<double>();
            } else throw std::runtime_error("不支持的动作类型");
            a.start_ms = integer(item.at("start_ms")); a.duration_ms = integer(item.at("duration_ms"));
            p.actions.push_back(a);
        }
        const auto& m = j.at("mask"); fields(m, {"enabled", "wasd", "start_ms", "duration_ms"});
        p.mask.enabled = m.at("enabled").get<bool>(); p.mask.wasd = integer(m.at("wasd"));
        p.mask.start_ms = integer(m.at("start_ms")); p.mask.duration_ms = integer(m.at("duration_ms"));
        if (!validate_plan(p, error)) return false;
        out = std::move(p); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
nlohmann::json plan_json(const Plan& p) {
    std::string error;
    if (!validate_plan(p, error)) throw std::invalid_argument(error);
    auto actions = nlohmann::json::array();
    for (const auto& a : p.actions) {
        nlohmann::json item{{"start_ms", a.start_ms}, {"duration_ms", a.duration_ms}};
        if (a.kind == ActionKind::KEY) { item["type"] = "key"; item["key"] = kKeys[static_cast<unsigned>(a.key)]; }
        else { item["type"] = "turn"; item["angle_degrees"] = a.angle_degrees; }
        actions.push_back(std::move(item));
    }
    return {{"schema_version", 1}, {"name", p.name}, {"map", p.map}, {"location", p.location}, {"notes", p.notes},
        {"actions", actions}, {"mask", {{"enabled", p.mask.enabled}, {"wasd", p.mask.wasd},
            {"start_ms", p.mask.start_ms}, {"duration_ms", p.mask.duration_ms}}}};
}

Result run_plan(IMouseController& device, const Plan& p, const movement::Config& scale, const Callbacks& callbacks) noexcept {
    Result result;
    bool debt = false;
    const auto run_begin = std::chrono::steady_clock::now();
    try {
        if (!validate_plan(p, result.error)) return result;
        const double degrees_per_count = scale.sensitivity * scale.yaw_degrees_per_count;
        if (!std::isfinite(degrees_per_count) || degrees_per_count <= 0 ||
            !std::isfinite(scale.sensitivity) || scale.sensitivity <= 0 || scale.yaw_degrees_per_count <= 0)
            throw std::runtime_error("灵敏度和每count角度必须为有限正数");
        if (!device.supports_wasd_keyboard()) throw std::runtime_error("设备不支持身法键盘输出");
        std::vector<int> sent(p.actions.size(), 0);
        std::vector<bool> seen(p.actions.size(), false);
        for (const auto& a : p.actions) {
            if (a.kind == ActionKind::TURN && std::abs(a.angle_degrees / degrees_per_count) > 1000000)
                throw std::runtime_error("单次旋转超过1000000 counts，请检查灵敏度");
            if (a.kind == ActionKind::KEY && a.key == Key::CTRL && !device.supports_left_ctrl_key())
                throw std::runtime_error("设备不支持Left Ctrl");
        }
        const auto allowed = [&] {
            return callbacks.permitted && callbacks.permitted() && (!callbacks.canceled || !callbacks.canceled());
        };
        const auto command = [&](auto operation) {
            if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
            debt = true;
            if (!ack(operation())) throw std::runtime_error("身法键盘操作未确认");
        };
        using Clock = std::chrono::steady_clock;
        const auto begin = Clock::now();
        const int total = duration_ms(p), mask_begin = mask_start(p);
        const int mask_end = p.mask.duration_ms ? mask_begin + p.mask.duration_ms : total;
        int keys = 0; bool space = false, ctrl = false, masked = false;
        for (;;) {
            if (!allowed()) { result.outcome = Outcome::CANCELED; result.error = "身法执行已取消或输出许可撤回"; break; }
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            const bool want_mask = p.mask.enabled && p.mask.wasd && elapsed >= mask_begin && elapsed < mask_end;
            if (!masked && want_mask && elapsed < total) {
                for (int bit = 1; bit <= 8; bit <<= 1) if (p.mask.wasd & bit)
                    command([&] { return device.set_wasd_mask(static_cast<std::uint8_t>(bit), true); });
                masked = true;
            }
            int want_keys = 0; bool want_space = false, want_ctrl = false;
            for (std::size_t i = 0; i < p.actions.size(); ++i) {
                const auto& a = p.actions[i];
                if (a.kind != ActionKind::KEY) continue;
                const bool already_held = a.key == Key::SPACE ? space : a.key == Key::CTRL ? ctrl :
                    (keys & (1 << static_cast<unsigned>(a.key))) != 0;
                // 完整落在其他同键持有段内的短动作已由并集满足，不制造误报。
                if (elapsed >= a.start_ms + a.duration_ms && already_held) seen[i] = true;
                if (elapsed >= a.start_ms + a.duration_ms && !seen[i] && !already_held)
                    throw std::runtime_error("调度或设备确认延迟导致整个按键区间被错过，已停止");
                if (elapsed < a.start_ms || elapsed >= a.start_ms + a.duration_ms) continue;
                seen[i] = true;
                if (a.key == Key::SPACE) want_space = true;
                else if (a.key == Key::CTRL) want_ctrl = true;
                else want_keys |= 1 << static_cast<unsigned>(a.key);
            }
            if (keys != want_keys) { command([&] { return device.set_wasd_keyboard(static_cast<std::uint8_t>(want_keys)); }); keys = want_keys; }
            if (space != want_space) { command([&] { return device.set_space_key(want_space); }); space = want_space; }
            if (ctrl != want_ctrl) { command([&] { return device.set_left_ctrl_key(want_ctrl); }); ctrl = want_ctrl; }
            // 先更新/释放这一时刻的软件键，再归还物理直通；不制造已结束方向的短暂叠加。
            if (masked && !want_mask && elapsed < total) {
                for (int bit = 1; bit <= 8; bit <<= 1) if (p.mask.wasd & bit)
                    command([&] { return device.set_wasd_mask(static_cast<std::uint8_t>(bit), false); });
                masked = false;
                command([&] { return device.set_wasd_keyboard(static_cast<std::uint8_t>(keys)); });
            }
            bool turns_done = true;
            for (std::size_t i = 0; i < p.actions.size(); ++i) {
                const auto& a = p.actions[i];
                if (a.kind != ActionKind::TURN) continue;
                const double fraction = std::clamp((elapsed - a.start_ms) / a.duration_ms, 0.0, 1.0);
                const int target = static_cast<int>(std::llround(a.angle_degrees / degrees_per_count * fraction));
                const int delta = std::clamp(target - sent[i], -32767, 32767);
                if (delta) {
                    if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
                    if (!device.move({delta, 0})) throw std::runtime_error("身法鼠标发送失败");
                    sent[i] += delta; result.sent_dx += delta;
                }
                turns_done = turns_done && sent[i] == static_cast<int>(std::llround(a.angle_degrees / degrees_per_count));
            }
            if (callbacks.progress) callbacks.progress({static_cast<int>(std::min(elapsed, static_cast<double>(total))), total, result.sent_dx});
            if (elapsed >= total && turns_done) { result.outcome = Outcome::COMPLETED; break; }
            // 单调时钟上的累计目标避免逐动作sleep漂移；调度精度不是游戏实际施加精度。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) { result.error = e.what(); }
      catch (...) { result.error = "身法执行发生未知异常"; }
    if (debt) {
        result.cleanup_known = ack(device.cleanup_wasd_keyboard());
        if (!result.cleanup_known) { result.outcome = Outcome::FAILED; result.error = "身法清理未确认，禁止再次输出"; }
    }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run_begin).count();
    return result;
}
}
