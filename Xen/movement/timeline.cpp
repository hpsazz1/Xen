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
bool ack(const ButtonReceipt& r) { return r.disposition == ButtonDisposition::ACKNOWLEDGED; }
void fields(const nlohmann::json& j, std::initializer_list<const char*> allowed) {
    if (!j.is_object() || j.size() != allowed.size()) throw std::runtime_error("方案字段缺失或包含未知字段");
    for (auto name : allowed) if (!j.contains(name)) throw std::runtime_error("方案缺少必要字段");
}
int integer(const nlohmann::json& j) {
    if (!j.is_number_integer() || j < -1 || j > kMaxTime) throw std::runtime_error("毫秒数或整数参数超出范围");
    return j.get<int>();
}
int counts(const nlohmann::json& j) {
    if (!j.is_number_integer() || j < -1000000 || j > 1000000) throw std::runtime_error("位移或滚轮参数超过1000000 counts");
    return j.get<int>();
}
int mask_start(const Plan& p) {
    if (p.mask.start_ms >= 0) return p.mask.start_ms;
    int start = kMaxTime;
    for (const auto& a : p.actions)
        if (a.kind == ActionKind::KEY && a.key == Key::SPACE) start = std::min(start, a.start_ms);
    return start;
}
bool fully_masked(const Plan& p, const Action& key) {
    if (!key.physical || static_cast<unsigned>(key.key) >= 4) return false;
    std::vector<std::pair<int, int>> intervals;
    const int bit = 1 << static_cast<unsigned>(key.key);
    if (p.mask.enabled && (p.mask.wasd & bit)) {
        const int begin = mask_start(p);
        intervals.emplace_back(begin, p.mask.duration_ms ? begin + p.mask.duration_ms : duration_ms(p));
    }
    for (const auto& a : p.actions)
        if (a.kind == ActionKind::MASK && a.key == key.key) intervals.emplace_back(a.start_ms, a.start_ms + a.duration_ms);
    std::sort(intervals.begin(), intervals.end());
    int covered = key.start_ms;
    for (const auto& interval : intervals) {
        if (interval.first > covered) break;
        covered = std::max(covered, interval.second);
        if (covered >= key.start_ms + key.duration_ms) return true;
    }
    return false;
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
        if (p.name.empty() || p.name.size() > 256 || p.map.size() > 256 || p.location.size() > 256 ||
            p.voice_command.size() > 256 || p.notes.size() > 4096)
            throw std::runtime_error("名称不能为空，名称/地图/点位/口令最多256字节，说明最多4096字节");
        if (p.actions.empty() || p.actions.size() > 16384) throw std::runtime_error("方案需要1至16384个动作");
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
            } else if (a.kind == ActionKind::MASK) {
                if (static_cast<unsigned>(a.key) >= 4) throw std::runtime_error("屏蔽动作只支持WASD");
            } else if (a.kind == ActionKind::MOVE) {
                if (std::abs(static_cast<long long>(a.dx)) > 1000000 || std::abs(static_cast<long long>(a.dy)) > 1000000)
                    throw std::runtime_error("单次位移超过1000000 counts");
            } else if (a.kind == ActionKind::WHEEL) {
                if (!a.wheel || a.wheel < -127 || a.wheel > 127) throw std::runtime_error("单次滚轮必须为-127至127个刻度且非零");
            } else if (a.kind == ActionKind::BUTTON) {
                if (a.button != Button::LEFT && a.button != Button::RIGHT)
                    throw std::runtime_error("按钮只支持鼠标左键和右键");
                if (a.physical) throw std::runtime_error("鼠标按钮暂不支持物理录制轨道");
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
        const int version = integer(j.at("schema_version"));
        if (version != 1 && version != 2 && version != 3) throw std::runtime_error("不支持的方案版本");
        if (version == 3)
            fields(j, {"schema_version", "name", "map", "location", "notes", "voice_command", "actions", "mask"});
        else fields(j, {"schema_version", "name", "map", "location", "notes", "actions", "mask"});
        Plan p;
        p.name = j.at("name").get<std::string>(); p.map = j.at("map").get<std::string>();
        if (version == 3) p.voice_command = j.at("voice_command").get<std::string>();
        p.location = j.at("location").get<std::string>(); p.notes = j.at("notes").get<std::string>();
        const auto& list = j.at("actions");
        if (!list.is_array() || list.size() > 16384) throw std::runtime_error("动作必须是最多16384项的数组");
        for (const auto& item : list) {
            Action a;
            const auto type = item.at("type").get<std::string>();
            if (type == "key" || (version >= 2 && type == "mask")) {
                if (version >= 2 && type == "key" && item.contains("physical")) {
                    fields(item, {"type", "key", "start_ms", "duration_ms", "physical"});
                    a.physical = item.at("physical").get<bool>();
                } else fields(item, {"type", "key", "start_ms", "duration_ms"});
                if (type == "mask") a.kind = ActionKind::MASK;
                const auto key = item.at("key").get<std::string>();
                const auto found = std::find(kKeys.begin(), kKeys.end(), key);
                if (found == kKeys.end()) throw std::runtime_error("不支持的按键名称");
                a.key = static_cast<Key>(found - kKeys.begin());
            } else if (type == "turn") {
                fields(item, {"type", "angle_degrees", "start_ms", "duration_ms"});
                a.kind = ActionKind::TURN;
                if (!item.at("angle_degrees").is_number()) throw std::runtime_error("角度必须是数字");
                a.angle_degrees = item.at("angle_degrees").get<double>();
            } else if (version >= 2 && type == "move") {
                fields(item, {"type", "dx", "dy", "start_ms", "duration_ms"});
                a.kind = ActionKind::MOVE; a.dx = counts(item.at("dx")); a.dy = counts(item.at("dy"));
            } else if (version >= 2 && type == "wheel") {
                fields(item, {"type", "wheel", "start_ms", "duration_ms"});
                a.kind = ActionKind::WHEEL; a.wheel = counts(item.at("wheel"));
            } else if (version >= 3 && type == "button") {
                fields(item, {"type", "button", "start_ms", "duration_ms"});
                const auto button = item.at("button").get<std::string>();
                if (button != "left" && button != "right") throw std::runtime_error("不支持的鼠标按钮");
                a.kind = ActionKind::BUTTON; a.button = button == "left" ? Button::LEFT : Button::RIGHT;
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
        if (a.kind == ActionKind::KEY || a.kind == ActionKind::MASK) {
            item["type"] = a.kind == ActionKind::KEY ? "key" : "mask"; item["key"] = kKeys[static_cast<unsigned>(a.key)];
            if (a.kind == ActionKind::KEY && a.physical) item["physical"] = true;
        } else if (a.kind == ActionKind::TURN) { item["type"] = "turn"; item["angle_degrees"] = a.angle_degrees; }
        else if (a.kind == ActionKind::MOVE) { item["type"] = "move"; item["dx"] = a.dx; item["dy"] = a.dy; }
        else if (a.kind == ActionKind::BUTTON) { item["type"] = "button"; item["button"] = a.button == Button::LEFT ? "left" : "right"; }
        else { item["type"] = "wheel"; item["wheel"] = a.wheel; }
        actions.push_back(std::move(item));
    }
    return {{"schema_version", 3}, {"name", p.name}, {"map", p.map}, {"location", p.location}, {"notes", p.notes},
        {"voice_command", p.voice_command},
        {"actions", actions}, {"mask", {{"enabled", p.mask.enabled}, {"wasd", p.mask.wasd},
            {"start_ms", p.mask.start_ms}, {"duration_ms", p.mask.duration_ms}}}};
}

Result run_plan(IMouseController& device, const Plan& p, const movement::Config& scale, const Callbacks& callbacks) noexcept {
    Result result;
    bool debt = false;
    std::array<bool, 2> button_debt{};
    const auto run_begin = std::chrono::steady_clock::now();
    try {
        if (!validate_plan(p, result.error)) return result;
        const double degrees_per_count = scale.sensitivity * scale.yaw_degrees_per_count;
        if (!std::isfinite(degrees_per_count) || degrees_per_count <= 0 ||
            !std::isfinite(scale.sensitivity) || scale.sensitivity <= 0 || scale.yaw_degrees_per_count <= 0)
            throw std::runtime_error("灵敏度和每count角度必须为有限正数");
        if (!device.supports_wasd_keyboard()) throw std::runtime_error("设备不支持身法键盘输出");
        std::vector<int> sent(p.actions.size(), 0);
        std::vector<int> sent_y(p.actions.size(), 0);
        std::vector<bool> seen(p.actions.size(), false);
        for (const auto& a : p.actions) {
            if (a.kind == ActionKind::TURN && std::abs(a.angle_degrees / degrees_per_count) > 1000000)
                throw std::runtime_error("单次旋转超过1000000 counts，请检查灵敏度");
            if (a.kind == ActionKind::KEY && a.key == Key::CTRL && !device.supports_left_ctrl_key())
                throw std::runtime_error("设备不支持Left Ctrl");
            if (a.kind == ActionKind::BUTTON && !device.supports_left_button())
                throw std::runtime_error("设备不支持鼠标按钮输出");
        }
        const auto allowed = [&] {
            return callbacks.permitted && callbacks.permitted() && (!callbacks.canceled || !callbacks.canceled());
        };
        const auto command = [&](auto operation) {
            if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
            debt = true;
            if (!ack(operation())) throw std::runtime_error("身法键盘操作未确认");
        };
        const auto button_command = [&](unsigned button, bool down) {
            if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
            // 未确认的按下同样承担释放责任，且左右按钮各自清理。
            button_debt[button] = true;
            const auto receipt = button == 0 ? device.set_left_button(down) : device.set_right_button(down);
            if (!ack(receipt)) throw std::runtime_error("鼠标按钮操作未确认");
        };
        using Clock = std::chrono::steady_clock;
        const auto begin = Clock::now();
        const int total = duration_ms(p), mask_begin = mask_start(p);
        const int mask_end = p.mask.duration_ms ? mask_begin + p.mask.duration_ms : total;
        int keys = 0, masked = 0; bool space = false, ctrl = false;
        std::array<bool, 2> buttons{};
        for (;;) {
            if (!allowed()) { result.outcome = Outcome::CANCELED; result.error = "身法执行已取消或输出许可撤回"; break; }
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            int want_mask = p.mask.enabled && elapsed >= mask_begin && elapsed < mask_end ? p.mask.wasd : 0;
            for (const auto& a : p.actions)
                if (a.kind == ActionKind::MASK && elapsed >= a.start_ms && elapsed < a.start_ms + a.duration_ms)
                    want_mask |= 1 << static_cast<unsigned>(a.key);
            if ((want_mask & ~masked) && elapsed < total) {
                for (int bit = 1; bit <= 8; bit <<= 1) if (want_mask & ~masked & bit)
                    command([&] { return device.set_wasd_mask(static_cast<std::uint8_t>(bit), true); });
                masked |= want_mask;
            }
            int want_keys = 0; bool want_space = false, want_ctrl = false;
            for (std::size_t i = 0; i < p.actions.size(); ++i) {
                const auto& a = p.actions[i];
                if (a.kind != ActionKind::KEY) continue;
                const bool already_held = a.key == Key::SPACE ? space : a.key == Key::CTRL ? ctrl :
                    (keys & (1 << static_cast<unsigned>(a.key))) != 0;
                // 完整落在其他同键持有段内的短动作已由并集满足，不制造误报。
                if (elapsed >= a.start_ms + a.duration_ms && already_held) seen[i] = true;
                // 原始物理轨道可完整处于屏蔽内，此时没有任何应发出的按下区间。
                if (elapsed >= a.start_ms + a.duration_ms && !seen[i] && fully_masked(p, a)) seen[i] = true;
                if (elapsed >= a.start_ms + a.duration_ms && !seen[i] && !already_held)
                    throw std::runtime_error("调度或设备确认延迟导致整个按键区间被错过，已停止");
                if (elapsed < a.start_ms || elapsed >= a.start_ms + a.duration_ms) continue;
                seen[i] = true;
                if (a.physical && static_cast<unsigned>(a.key) < 4 && (want_mask & (1 << static_cast<unsigned>(a.key)))) continue;
                if (a.key == Key::SPACE) want_space = true;
                else if (a.key == Key::CTRL) want_ctrl = true;
                else want_keys |= 1 << static_cast<unsigned>(a.key);
            }
            if (keys != want_keys) { command([&] { return device.set_wasd_keyboard(static_cast<std::uint8_t>(want_keys)); }); keys = want_keys; }
            if (space != want_space) { command([&] { return device.set_space_key(want_space); }); space = want_space; }
            if (ctrl != want_ctrl) { command([&] { return device.set_left_ctrl_key(want_ctrl); }); ctrl = want_ctrl; }
            // 跳跃与蹲姿先更新，再处理同一时刻的松鼠标；不把双键投掷顺序交给列表排列。
            std::array<bool, 2> want_buttons{};
            const double button_elapsed = elapsed;
            for (std::size_t i = 0; i < p.actions.size(); ++i) {
                const auto& a = p.actions[i];
                if (a.kind != ActionKind::BUTTON) continue;
                const auto button = static_cast<unsigned>(a.button);
                if (button_elapsed >= a.start_ms + a.duration_ms && buttons[button]) seen[i] = true;
                if (button_elapsed >= a.start_ms + a.duration_ms && !seen[i])
                    throw std::runtime_error("调度或设备确认延迟导致整个鼠标按钮区间被错过，已停止");
                if (button_elapsed < a.start_ms || button_elapsed >= a.start_ms + a.duration_ms) continue;
                seen[i] = true; want_buttons[button] = true;
            }
            std::array<unsigned, 2> button_order{0, 1};
            const auto release_at = [&](unsigned button) {
                if (!buttons[button] || want_buttons[button]) return kMaxTime + 1;
                int end = 0;
                for (const auto& a : p.actions)
                    if (a.kind == ActionKind::BUTTON && static_cast<unsigned>(a.button) == button &&
                        a.start_ms + a.duration_ms <= elapsed)
                        end = std::max(end, a.start_ms + a.duration_ms);
                return end;
            };
            // 一轮可能跨过两个释放时刻；按配方结束时间释放，不能按按钮编号颠倒先后。
            std::stable_sort(button_order.begin(), button_order.end(), [&](unsigned a, unsigned b) {
                return release_at(a) < release_at(b);
            });
            for (unsigned button : button_order)
                if (buttons[button] != want_buttons[button]) {
                    if (want_buttons[button]) {
                        const double now = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
                        const bool still_active = std::any_of(p.actions.begin(), p.actions.end(), [&](const Action& a) {
                            return a.kind == ActionKind::BUTTON && static_cast<unsigned>(a.button) == button &&
                                now >= a.start_ms && now < a.start_ms + a.duration_ms;
                        });
                        if (!still_active) throw std::runtime_error("设备确认延迟导致鼠标按钮区间被错过，已停止");
                    }
                    button_command(button, want_buttons[button]); buttons[button] = want_buttons[button];
                }
            // 先更新/释放这一时刻的软件键，再归还物理直通；不制造已结束方向的短暂叠加。
            if ((masked & ~want_mask) && elapsed < total) {
                for (int bit = 1; bit <= 8; bit <<= 1) if (masked & ~want_mask & bit)
                    command([&] { return device.set_wasd_mask(static_cast<std::uint8_t>(bit), false); });
                masked = want_mask;
                command([&] { return device.set_wasd_keyboard(static_cast<std::uint8_t>(keys)); });
            }
            bool turns_done = true;
            for (std::size_t i = 0; i < p.actions.size(); ++i) {
                const auto& a = p.actions[i];
                if (a.kind == ActionKind::WHEEL) {
                    if (!seen[i] && elapsed >= a.start_ms) {
                        if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
                        if (!device.scroll_wheel(a.wheel)) throw std::runtime_error("身法滚轮发送失败");
                        seen[i] = true;
                    }
                    turns_done = turns_done && seen[i];
                    continue;
                }
                if (a.kind != ActionKind::TURN && a.kind != ActionKind::MOVE) continue;
                const double fraction = std::clamp((elapsed - a.start_ms) / a.duration_ms, 0.0, 1.0);
                const double full_x = a.kind == ActionKind::TURN ? a.angle_degrees / degrees_per_count : a.dx;
                const int full_y = a.kind == ActionKind::MOVE ? a.dy : 0;
                const int target = static_cast<int>(std::llround(full_x * fraction));
                const int target_y = static_cast<int>(std::llround(full_y * fraction));
                const int delta = std::clamp(target - sent[i], -32767, 32767);
                const int delta_y = std::clamp(target_y - sent_y[i], -32767, 32767);
                if (delta || delta_y) {
                    if (!allowed()) { result.outcome = Outcome::CANCELED; throw std::runtime_error("身法执行已取消或输出许可撤回"); }
                    if (!device.move({delta, delta_y})) throw std::runtime_error("身法鼠标发送失败");
                    sent[i] += delta; sent_y[i] += delta_y; result.sent_dx += delta;
                }
                turns_done = turns_done && sent[i] == static_cast<int>(std::llround(full_x)) && sent_y[i] == full_y;
            }
            if (callbacks.progress) callbacks.progress({static_cast<int>(std::min(elapsed, static_cast<double>(total))), total, result.sent_dx});
            if (elapsed >= total && turns_done) { result.outcome = Outcome::COMPLETED; break; }
            // 单调时钟上的累计目标避免逐动作sleep漂移；调度精度不是游戏实际施加精度。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) { result.error = e.what(); }
      catch (...) { result.error = "身法执行发生未知异常"; }
    // 即使取消或一侧释放失败，也尝试另一侧；不再查询已撤销的输出许可。
    for (unsigned button = 0; button < button_debt.size(); ++button)
        if (button_debt[button]) {
            const bool released = ack(button == 0 ? device.set_left_button(false) : device.set_right_button(false));
            result.cleanup_known = released && result.cleanup_known;
        }
    if (button_debt[0] || button_debt[1])
        result.cleanup_known = result.cleanup_known && !device.left_button_cleanup_required() && !device.left_button_faulted();
    if (debt) result.cleanup_known = ack(device.cleanup_wasd_keyboard()) && result.cleanup_known;
    if (!result.cleanup_known) { result.outcome = Outcome::FAILED; result.error = "身法清理未确认，禁止再次输出"; }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run_begin).count();
    return result;
}
}
