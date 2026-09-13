#include "auto_stop_probe/counterpulse_internal.h"
#include <iostream>
#include <utility>

namespace {
using namespace auto_stop_probe_detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Fake final : public IMouseController {
public:
    Clock::time_point time{std::chrono::seconds(10)};
    int latency_ms = 1, downs = 0, unknown_down = 0, keyboard_calls = 0, cleanup_calls = 0;
    int physical_at_ms = -1;
    bool healthy = true, exclusive = true, left = false;
    bool events_available = false, event_gap = false;
    int epoch_change_at_ms = -1;
    std::uint8_t held = 0;
    std::vector<int> keyboards;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool output_owner_exclusive() const noexcept override { return exclusive; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.state_valid = healthy; input.status = InputMonitorStatus::READY; input.sequence = 1;
        if (physical_at_ms >= 0 && time >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(physical_at_ms))
            input.virtual_keys['A'] = true;
        return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        batch = {}; if (!events_available) return false;
        batch.subscribed = true; batch.gap = event_gap; cursor.epoch = 1;
        if (epoch_change_at_ms >= 0 && time >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(epoch_change_at_ms))
            cursor.epoch = 2;
        return true;
    }
    ButtonReceipt set_left_button(bool value) noexcept override {
        left = value; if (value) ++downs;
        time += std::chrono::milliseconds(latency_ms);
        ButtonReceipt r; r.datagram_sent = true;
        r.disposition = value && downs == unknown_down ? ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED;
        r.backend_completed_at = r.protocol_ack_received_at = time; return r;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t value) noexcept override {
        held = value; ++keyboard_calls; keyboards.push_back(value); return keyboard_ack();
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++cleanup_calls; held = 0; return keyboard_ack(); }
    KeyboardReceipt keyboard_ack() noexcept {
        time += std::chrono::milliseconds(latency_ms);
        KeyboardReceipt r; r.disposition = KeyboardDisposition::ACKNOWLEDGED; r.datagram_sent = true;
        r.backend_completed_at = r.protocol_ack_received_at = time; return r;
    }
    CounterpulseClock clock() {
        return {[this] { return time; }, [this](auto deadline) { time = std::max(time, deadline); }};
    }
};
void successful_and_baselines() {
    for (const auto* baseline : {"counter", "stationary", "no_counter"}) {
        Fake mouse; CounterpulsePlan p; p.baseline = baseline;
        const auto report = execute_counterpulse(mouse, p, {}, mouse.clock());
        require(report["success"], "有界计划应完成");
        require(mouse.downs == 8 && !mouse.left && mouse.held == 0 && mouse.cleanup_calls == 1, "八发预算及最终归零");
        std::vector<std::int64_t> shots;
        for (const auto& command : report["commands"])
            if (command["kind"] == "left_button" && command["value"] == 1) shots.push_back(command["submit_ns"]);
        for (std::size_t i = 1; i < shots.size(); ++i)
            require(shots[i] - shots[i - 1] == (p.baseline == "stationary" ? 280000000 : 282000000),
                "静止组固定280ms，移动组须包含已记录的两次1ms键命令ACK");
        if (p.baseline == "stationary") require(mouse.keyboards.size() == 1, "静止基线不移动");
        if (p.baseline == "counter") require(mouse.keyboards.size() == 29 && mouse.keyboards[1] == 2 && mouse.keyboards[3] == 8, "反冲严格正向释放再反向");
    }
    Fake mouse; CounterpulsePlan p; p.shots = 7; p.direction = 8;
    const auto report = execute_counterpulse(mouse, p, {}, mouse.clock());
    require(report["success"] && mouse.downs == 7 && mouse.keyboards[1] == 8 && mouse.keyboards[3] == 2, "七发和D方向应使用对向A");
}
void configurable_stationary_intervals() {
    for (const auto [shots, interval] : {std::pair{7, 600}, std::pair{7, 650}, std::pair{8, 500}, std::pair{20, 650}}) {
        Fake mouse; CounterpulsePlan p;
        p.baseline = "stationary"; p.shots = shots; p.shot_interval_ms = interval;
        const auto parsed = parse_counterpulse_plan(counterpulse_plan_json(p));
        require(parsed.shots == shots && parsed.shot_interval_ms == interval, "较慢静止基线必须保留指定发数和间隔");
        const auto report = execute_counterpulse(mouse, p, {}, mouse.clock());
        require(report["success"] && report["shot_down_attempts"] == shots && mouse.downs == shots,
            "较慢基线必须完整执行规定枪次，不能被旧三秒期限截断");
        require(!mouse.left && !mouse.held && mouse.cleanup_calls == 1, "较慢基线结束仍须按键归零并清理");
        require(mouse.keyboards.size() == 1 && mouse.keyboards.front() == 0,
            "静止组仅初始化软件零，不得产生任何WASD移动");
        std::vector<std::int64_t> submitted, planned;
        int previous_index = 0;
        for (const auto& command : report["commands"]) {
            if (command["kind"] != "left_button" || command["value"] != 1) continue;
            submitted.push_back(command["submit_ns"]);
            planned.push_back(command["planned_ns"]);
            require(command["shot_index"] == ++previous_index, "枪次索引必须逐次唯一且不补射");
            require(submitted.back() == planned.back(), "确定性fake提交必须遵守目标时刻");
        }
        require(submitted.size() == static_cast<std::size_t>(shots), "命令证据必须记录每一发且仅记录规定发数");
        for (std::size_t i = 1; i < submitted.size(); ++i) {
            require(submitted[i] - submitted[i-1] == static_cast<std::int64_t>(interval)*1000000,
                "实际提交间隔必须等于用户指定值");
            require(planned[i] - planned[i-1] == static_cast<std::int64_t>(interval)*1000000,
                "目标时间轴必须采用用户指定间隔");
        }
        require(submitted.back() - submitted.front() == static_cast<std::int64_t>((shots-1)*interval)*1000000,
            "整组首末枪跨度必须与计划一致");
    }
    require(parse_counterpulse_plan(Json::object()).shot_interval_ms == 280, "未指定间隔时兼容旧计划280ms");
    require(parse_counterpulse_plan(Json{{"shots", 8}, {"shot_interval_ms", 514}}).shot_interval_ms == 514,
        "八发3598ms跨度边界应接受");
    require(parse_counterpulse_plan(Json{{"shots", 8}, {"shot_interval_ms", 557}}).shot_interval_ms == 557,
        "八发3899ms跨度应接受");
}
void matched_brake_window() {
    for (const auto* mode : {"no_counter", "counter"}) for (const int latency : {1, 2, 3}) {
        Fake mouse; CounterpulsePlan p;
        mouse.latency_ms = latency;
        p.baseline = mode; p.shots = 20; p.shot_interval_ms = 650;
        const auto r = execute_counterpulse(mouse, p, {}, mouse.clock());
        require(r["success"], "移动时序应在有界窗口完成");
        for (int shot = 2; shot <= 20; ++shot) {
            std::int64_t moved_up = 0, fired = 0;
            bool moving = false;
            for (const auto& c : r["commands"]) {
                if (c["shot_index"] != shot) continue;
                if (c["kind"] == "wasd" && c["value"] == p.direction) moving = true;
                else if (moving && !moved_up && c["kind"] == "wasd" && c["value"] == 0)
                    moved_up = c["ack_received_ns"];
                if (c["kind"] == "left_button" && c["value"] == 1) fired = c["submit_ns"];
            }
            require(moved_up && fired - moved_up == 60000000,
                "无反向和反向组必须共用移动UP ACK后60ms窗口，不得长等到650ms末端");
        }
    }
    { Fake m; CounterpulsePlan p; p.counter_hold_ms = 59;
      const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "ACTION_BUDGET_EXCEEDED" && m.downs == 1 && !m.held && !m.left,
          "反向UP ACK越过观察deadline应取消而非顺延射击");
      require(r["failure_context"]["site"] == "REVERSE_FINISHED" &&
          r["failure_context"]["observed_ns"].get<std::int64_t>() > r["failure_context"]["limit_ns"].get<std::int64_t>(),
          "报告须保留反向完成超窗的决策现场"); }
    { Fake m; m.latency_ms = 3; CounterpulsePlan p; p.baseline = "no_counter";
      const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["success"] && m.downs == p.shots,
          "ACK耗时应计入恢复间隔，不占用松键后射击的迟到预算"); }
    for (const int delay_ms : {4, 6}) { Fake m; CounterpulsePlan p; p.baseline = "no_counter"; p.shots = 7; p.shot_interval_ms = 650;
      auto clock = m.clock(); bool injected = false;
      clock.sleep_until = [&](auto target) {
          m.time = std::max(m.time, target);
          if (!injected && target >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(654)) {
              m.time += std::chrono::milliseconds(delay_ms); injected = true;
          }
      };
      const auto r = execute_counterpulse(m, p, {}, clock);
      if (delay_ms == 4) require(r["success"] && m.downs == 7 && !m.held && !m.left,
          "恢复超过655ms但松键后射击仅迟4ms应完成，不能重复扣除ACK预算");
      else require(r["failure"] == "SHOT_DEADLINE_MISSED" && m.downs == 1 && !m.held && !m.left,
          "松键后射击迟到超过5ms仍须拒绝第二发并清理"); }
}
void shot_after_direction_release() {
    for (const auto* mode : {"counter", "no_counter"}) for (int hold : {15, 25, 35, 80})
        for (int latency : {1, 3}) {
        Fake m; m.latency_ms = latency;
        const auto p = parse_counterpulse_plan(Json{{"baseline", mode}, {"shots", 20},
            {"shot_interval_ms", 650}, {"counter_hold_ms", hold}, {"shot_after_release_ms", 5}});
        const auto r = execute_counterpulse(m, p, {}, m.clock());
        require(r["success"] && m.downs == 20 && !m.left && !m.held, "释放后单发应完成二十发并清理");
        require(r["timing_model"] == "DIRECTION_UP_ACK_DELAY", "新时序必须区别固定移动松键窗");
        std::int64_t previous_shot = 0;
        for (int shot = 1; shot <= 20; ++shot) {
            std::int64_t last_key_up = 0, fired = 0, fire_ack = 0, fire_up = 0;
            std::int64_t reverse_down_ack = 0, reverse_up_submit = 0;
            for (const auto& c : r["commands"]) {
                if (c["shot_index"] != shot) continue;
                if (c["kind"] == "wasd" && c["value"] != 0 && c["value"] != p.direction)
                    reverse_down_ack = c["ack_received_ns"];
                if (c["kind"] == "wasd" && c["value"] == 0) {
                    last_key_up = c["ack_received_ns"];
                    if (reverse_down_ack) reverse_up_submit = c["submit_ns"];
                }
                if (c["kind"] == "left_button" && c["value"] == 1) {
                    fired = c["submit_ns"]; fire_ack = c["ack_received_ns"];
                }
                if (c["kind"] == "left_button" && c["value"] == 0) fire_up = c["submit_ns"];
            }
            require(fire_up - fire_ack == 5000000, "单发左键ACK后保持5ms再松开");
            if (shot > 1) {
                if (p.baseline == "counter") require(reverse_up_submit - reverse_down_ack == hold * 1000000LL,
                    "每个候选h必须实际用于反向DOWN ACK至UP提交，不能只改变报告值");
                require(last_key_up && fired - last_key_up == 5000000,
                    "无论反向保持多长，必须在最后方向键UP ACK后5ms开枪");
                require(fired - previous_shot >= 650000000, "恢复等待应在移动前，枪间不能短于650ms");
                require(r["cycles"][shot - 2]["shot_anchor_ack_ns"] == last_key_up, "报告必须绑定真正的射击锚点");
            }
            previous_shot = fired;
        }
    }
    { Fake m; const auto p = parse_counterpulse_plan(Json{{"shots", 7}, {"shot_interval_ms", 650},
          {"counter_hold_ms", 15}, {"shot_after_release_ms", 5}});
      auto clock = m.clock(); bool injected = false;
      clock.sleep_until = [&](auto target) {
          m.time = std::max(m.time, target);
          if (!injected && target >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(656)) {
              m.time += std::chrono::milliseconds(6); injected = true;
          }
      };
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(r["failure"] == "SHOT_DEADLINE_MISSED" && m.downs == 1 && !m.left && !m.held,
          "释放后5ms的新射击目标迟到超过5ms仍须取消，不自动补射"); }
}
void failures_stop_and_cleanup() {
    CounterpulsePlan p;
    { Fake m; m.unknown_down = 2; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(!r["success"].get<bool>() && m.downs == 2 && !m.left && !m.held, "未知射击ACK不退款不重试且清理"); }
    { Fake m; m.latency_ms = 40; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "ACTION_BUDGET_EXCEEDED" && m.downs == 1 && !m.left && !m.held, "动作超预算不得补射"); }
    { Fake m; m.physical_at_ms = 40; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "PHYSICAL_INPUT_CANCELED" && m.downs == 1 && !m.held, "真实移动打断动作并清理"); }
    { Fake m; m.healthy = false; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "MONITOR_INVALID" && m.downs == 0, "非有效READY禁止首枪"); }
    { Fake m; m.events_available = true; m.epoch_change_at_ms = 40; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "INPUT_EPOCH_CHANGED" && m.downs == 1 && !m.held, "监听代际变化必须取消"); }
    { Fake m; m.events_available = true; m.event_gap = true; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "INPUT_EVENT_GAP" && m.downs == 0, "真实输入缺口禁止首枪"); }
    { Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock(), true);
      require(r["failure"] == "INPUT_HISTORY_UNAVAILABLE" && m.downs == 0, "要求历史时不能跳过订阅证据"); }
    { Fake m; auto clock = m.clock(); clock.sleep_until = [&m](auto deadline) { m.time = deadline + std::chrono::milliseconds(50); };
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(r["failure"] == "RELEASE_DEADLINE_MISSED" && m.downs == 1 && !m.left && !m.held, "释放晚50毫秒立即归零但整组无效"); }
    { Fake m; auto clock = m.clock(); clock.sleep_until = [](auto) {};
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(r["failure"] == "CLOCK_NOT_ADVANCING" && m.downs == 1 && !m.left, "时钟不推进不能无限等待"); }
    { Fake m; auto clock = m.clock(); clock.sleep_until = [&m](auto) { m.time -= std::chrono::milliseconds(1); };
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(!r["success"].get<bool>() && !m.left && !m.held, "时钟倒退必须取消"); }
    { Fake m; auto clock = m.clock(); clock.sleep_until = [&m](auto) { m.time += std::chrono::seconds(10); };
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(r["failure"] == "RUN_DEADLINE_EXCEEDED" && !m.left && !m.held, "全组超过硬期限必须取消并清理"); }
    { Fake m; m.exclusive = false; const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(!r["success"].get<bool>() && m.downs == 0 && m.cleanup_calls == 0, "独占不足不得接触设备"); }
    { Fake m; const auto r = execute_counterpulse(m, p, [] { return "FOCUS_LOST"; }, m.clock());
      require(r["failure"] == "FOCUS_LOST" && m.downs == 0, "外部取消在每次提交前生效"); }
    { Fake m; int calls = 0; const auto r = execute_counterpulse(m, p, [&]() -> std::string {
          if (++calls > 20) throw std::runtime_error("测试异常"); return {}; }, m.clock());
      require(r["failure"] == "EXECUTION_EXCEPTION" && !m.left && !m.held && m.cleanup_calls == 1, "异常也必须归零"); }
}
void invalid_plans() {
    for (const auto& json : {Json{{"shots", 21}}, Json{{"direction", 258}}, Json{{"late_tolerance_ms", 11}},
        Json{{"shot_after_release_ms", 21}}, Json{{"shot_after_release_ms", -1}},
        Json{{"shot_after_release_ms", 5}, {"move_ms", 250}, {"counter_hold_ms", 200}},
        Json{{"brake_window_ms", 0}}, Json{{"counter_hold_ms", 60}},
        Json{{"shot_interval_ms", 279}}, Json{{"shots", 7}, {"shot_interval_ms", 651}},
        Json{{"shots", 6}},
        Json{{"shots", 7}, {"shot_interval_ms", 500.5}},
        Json{{"move_ms", 250}, {"counter_hold_ms", 100}}, Json{{"shots", 7.5}}}) {
        bool rejected = false; try { parse_counterpulse_plan(json); } catch (...) { rejected = true; }
        require(rejected, "非法计划应拒绝");
    }
}
}
int main() {
    try { shot_after_direction_release(); matched_brake_window(); successful_and_baselines(); configurable_stationary_intervals(); failures_stop_and_cleanup(); invalid_plans(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "反冲纯fake专项通过：时序、基线、预算、取消、未知ACK、清理及参数拒绝\n";
}
