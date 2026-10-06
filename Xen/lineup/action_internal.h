#ifndef XEN_LINEUP_ACTION_INTERNAL_H
#define XEN_LINEUP_ACTION_INTERNAL_H
// 纯动作设计校验；不解析自由文本、不创建设备、不授予生产输出许可。
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <set>
#include <optional>
#include <string>
namespace lineup::detail {
struct ActionDesign {
    bool valid = false, configured = false, timing_complete = false, left_hold_release = false;
    int hold_ms = 0;
    std::string error;
};
inline ActionDesign inspect_action(const nlohmann::json &action) noexcept {
    ActionDesign result;
    try {
        if (action.is_null()) { result.valid = true; result.error = "unconfigured"; return result; }
        result.configured = true;
        if (!action.is_object() || !action.at("schema").is_number_integer() || action.at("schema") != 1 || action.at("type") != "phases")
            throw std::invalid_argument("unsupported_schema");
        for (auto field = action.begin(); field != action.end(); ++field)
            if (field.key() != "schema" && field.key() != "type" && field.key() != "phases" && field.key() != "game_version" && field.key() != "movement_profile")
                throw std::invalid_argument("unknown_action_field");
        if (action.contains("game_version") && (!action.at("game_version").is_string() || action.at("game_version").get<std::string>().size() > 128))
            throw std::invalid_argument("invalid_game_version");
        if (action.contains("movement_profile")) {
            auto profile = action.at("movement_profile").get<std::string>();
            if (profile != "unspecified" && profile != "stationary" && profile != "step" && profile != "runup" && profile != "already_moving")
                throw std::invalid_argument("invalid_movement_profile");
        }
        const auto &phases = action.at("phases");
        if (!phases.is_array() || phases.empty() || phases.size() > 16) throw std::invalid_argument("invalid_phase_count");
        bool timed = true;
        int total = 0;
        for (const auto &phase : phases) {
            if (!phase.is_object() || !phase.at("jump").is_boolean()) throw std::invalid_argument("invalid_phase");
            for (auto field = phase.begin(); field != phase.end(); ++field)
                if (field.key() != "buttons" && field.key() != "movement" && field.key() != "jump" && field.key() != "duration_ms")
                    throw std::invalid_argument("unknown_phase_field");
            for (auto key : {"buttons", "movement"}) {
                const auto &values = phase.at(key);
                if (!values.is_array()) throw std::invalid_argument("invalid_held_set");
                std::set<std::string> unique;
                for (const auto &value : values) {
                    auto name = value.get<std::string>();
                    const bool accepted = std::string(key) == "buttons" ? (name == "left" || name == "right") :
                        (name == "forward" || name == "back" || name == "left" || name == "right");
                    if (!accepted || !unique.insert(name).second) throw std::invalid_argument("invalid_held_set");
                }
                if (std::string(key) == "movement" && ((unique.contains("forward") && unique.contains("back")) ||
                    (unique.contains("left") && unique.contains("right")))) throw std::invalid_argument("opposed_movement");
            }
            const auto &duration = phase.at("duration_ms");
            if (duration.is_null()) timed = false;
            else {
                if (!duration.is_number_integer()) throw std::invalid_argument("invalid_duration");
                auto value = duration.get<std::int64_t>();
                if (value < 0 || value > 2000) throw std::invalid_argument("invalid_duration");
                total += static_cast<int>(value);
            }
        }
        if (total > 10000) throw std::invalid_argument("duration_limit");
        const auto &last = phases.back();
        if (!last.at("buttons").empty() || !last.at("movement").empty() || last.at("jump").get<bool>())
            throw std::invalid_argument("final_release_required");
        result.valid = true; result.timing_complete = timed;
        if (timed && phases.size() == 2) {
            const auto &first = phases.front();
            result.left_hold_release = first.at("buttons") == nlohmann::json::array({"left"}) &&
                first.at("movement").empty() && !first.at("jump").get<bool>() &&
                first.at("duration_ms").get<int>() > 0 && last.at("duration_ms").get<int>() == 0;
            if (result.left_hold_release) result.hold_ms = first.at("duration_ms").get<int>();
        }
        result.error = !timed ? "timing_incomplete" : "";
    } catch (const std::exception &e) { result.error = e.what(); }
      catch (...) { result.error = "invalid_action"; }
    return result;
}
struct ActionCapabilities {
    bool left = false, right = false, movement = false, jump = false;
};
struct ActionDryRun {
    bool supported = false;
    std::string reason;
    nlohmann::json events = nlohmann::json::array();
};
// 纯 dry-run：先检查整条设计所需能力，全部满足才构造按/放事件，不触及输出设备。
inline ActionDryRun dry_run_action(const nlohmann::json &action, const ActionCapabilities &capabilities) {
    ActionDryRun result;
    const auto design = inspect_action(action);
    if (!design.valid || !design.configured || !design.timing_complete) { result.reason = design.error; return result; }
    const auto &phases = action.at("phases");
    for (const auto &phase : phases) {
        for (const auto &button : phase.at("buttons"))
            if ((button == "left" && !capabilities.left) || (button == "right" && !capabilities.right)) { result.reason = "unsupported_button"; return result; }
        if (!phase.at("movement").empty() && !capabilities.movement) { result.reason = "unsupported_movement"; return result; }
        if (phase.at("jump").get<bool>() && !capabilities.jump) { result.reason = "unsupported_jump"; return result; }
    }
    std::set<std::string> previous;
    std::int64_t time = 0;
    for (const auto &phase : phases) {
        std::set<std::string> next;
        for (const auto &button : phase.at("buttons")) next.insert("button:" + button.get<std::string>());
        for (const auto &direction : phase.at("movement")) next.insert("movement:" + direction.get<std::string>());
        if (phase.at("jump").get<bool>()) next.insert("jump");
        // 同阶段先释放不再持有的集合，再按下新集合；同一阶段内没有伪造毫秒间隔。
        for (const auto &key : previous) if (!next.contains(key)) result.events.push_back({{"at_ms",time},{"control",key},{"held",false}});
        for (const auto &key : next) if (!previous.contains(key)) result.events.push_back({{"at_ms",time},{"control",key},{"held",true}});
        previous = std::move(next); time += phase.at("duration_ms").get<int>();
    }
    result.supported = true;
    return result;
}
// hardware_caps 必须来自运行时后端能力；设计文档/浏览器不能提供验证标签。
inline nlohmann::json action_status(const nlohmann::json &action,
        std::optional<ActionCapabilities> hardware_caps = std::nullopt) {
    auto design = inspect_action(action);
    const auto simulation = dry_run_action(action, {true, true, true, true});
    const auto hardware = hardware_caps ? dry_run_action(action, *hardware_caps) : ActionDryRun{};
    return {{"design", {{"valid", design.valid}, {"configured", design.configured}, {"timing_complete", design.timing_complete}, {"reason", design.error}}},
            {"simulation", {{"supported", simulation.supported}, {"reason", simulation.reason}, {"meaning", "phase_dry_run_capability"}}},
            {"hardware", {{"available", hardware_caps.has_value()}, {"supported", hardware_caps.has_value() && hardware.supported}, {"reason", hardware_caps ? hardware.reason : "runtime_capabilities_unavailable"}}},
            {"manual", {{"status", "unverified"}}}};
}
} // namespace lineup::detail
#endif
