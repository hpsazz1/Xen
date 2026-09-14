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
    int physical_at_ms = -1, unknown_keyboard_call = -1;
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
        held = value; ++keyboard_calls; keyboards.push_back(value);
        auto r = keyboard_ack();
        if (keyboard_calls == unknown_keyboard_call) r.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        return r;
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
void immediate_movement_cycles() {
    const auto p = parse_counterpulse_plan(Json{{"shots", 20}, {"shot_interval_ms", 0},
        {"move_ms", 500}, {"counter_hold_ms", 15}, {"shot_after_release_ms", 5}, {"capture_enabled", false}});
    { Fake slow; slow.latency_ms = 3;
      const auto stopped = execute_counterpulse(slow, p, {}, slow.clock());
      require(!stopped["success"].get<bool>() && slow.downs < 20 && !slow.left && !slow.held && slow.cleanup_calls == 1,
          "每次ACK3ms累计超过全程余量时须停止并清理，不压缩动作凑满20发");
      require(stopped["failure"] == "RUN_DEADLINE_EXCEEDED" || stopped["failure"] == "ACTION_BUDGET_EXCEEDED",
          "累计超时须归因全程预算而非反向保持参数或已取消的650ms下限"); }
    Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
    require(r["success"] && m.downs == 20 && !m.left && !m.held, "无额外恢复等待的500ms移动循环须完成二十发");
    require(r["plan"]["capture_enabled"] == false && r["plan"]["shot_interval_ms"] == 0,
        "报告应保留不采集及取消最小枪间隔的实际计划");
    std::int64_t previous_fire_up_return = 0, previous_fire = 0;
    for (int shot = 1; shot <= 20; ++shot) {
        std::int64_t move_down = 0, move_ack = 0, move_up = 0, reverse_ack = 0, reverse_up = 0, key_up_ack = 0;
        for (const auto& c : r["commands"]) {
            if (c["shot_index"] != shot) continue;
            if (c["kind"] == "wasd") {
                if (c["value"] == 2) { move_down = c["submit_ns"]; move_ack = c["ack_received_ns"]; }
                else if (c["value"] == 8) reverse_ack = c["ack_received_ns"];
                else if (reverse_ack) { reverse_up = c["submit_ns"]; key_up_ack = c["ack_received_ns"]; }
                else move_up = c["submit_ns"];
            } else if (c["value"] == 1) {
                const auto fired = c["submit_ns"].get<std::int64_t>();
                if (shot > 1) {
                    require(move_down == previous_fire_up_return, "上一枪左键释放完成后立即移动，不保留650ms等待");
                    require(move_up - move_ack == 500000000 && reverse_up - reverse_ack == 15000000,
                        "实际移动500ms且反向15ms，不得压缩动作");
                    require(fired - key_up_ack == 5000000, "反向松键ACK后5ms开枪");
                    require(fired - previous_fire == 531000000, "1ms ACK假设备周期为525ms动作加6次ACK，而非650ms");
                }
                previous_fire = fired;
            } else previous_fire_up_return = c["returned_ns"];
        }
    }
}
void delayed_reverse_tap_then_fire() {
    const auto p = parse_counterpulse_plan(Json{{"shots", 20}, {"capture_enabled", false}, {"shot_interval_ms", 0},
        {"fire_delay_ms", 300}, {"move_ms", 300}, {"counter_delay_ms", 50},
        {"counter_hold_ms", 5}, {"shot_after_release_ms", 0}});
    Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
    require(r["success"] && m.downs == 20 && !m.held && !m.left, "松A等50ms轻点D并立即开枪应完整执行");
    for (int shot = 2; shot <= 20; ++shot) {
        std::int64_t a_up = 0, d_down_ack = 0, d_up_ack = 0;
        int state = 0;
        for (const auto& c : r["commands"]) {
            if (c["shot_index"] != shot) continue;
            const auto submit = c["submit_ns"].get<std::int64_t>();
            if (c["kind"] == "wasd") {
                if (c["value"] == 2) state = 2;
                else if (c["value"] == 8) {
                    require(state == 0 && submit - a_up == 50000000, "50ms是A释放到D按下的间隔，不能当D保持时长");
                    d_down_ack = c["ack_received_ns"]; state = 8;
                } else if (state == 2) { a_up = c["ack_received_ns"]; state = 0; }
                else {
                    require(submit - d_down_ack == 5000000, "D仅轻点5ms，不保持50ms");
                    d_up_ack = c["ack_received_ns"]; state = 0;
                }
            } else if (c["value"] == 1) require(state == 0 && submit == d_up_ack,
                "D释放确认后立即开枪，不附加旧5ms或60ms等待");
        }
    }
    { Fake late; auto clock = late.clock(); bool injected = false;
      clock.sleep_until = [&](auto target) {
          late.time = std::max(late.time, target);
          if (!injected && target >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(361)) {
              late.time += std::chrono::milliseconds(6); injected = true;
          }
      };
      const auto stopped = execute_counterpulse(late, p, {}, clock);
      require(stopped["failure"] == "COMMAND_DEADLINE_MISSED" && late.downs == 1 && !late.held && !late.left,
          "松A后50ms等待迟到超过5ms时停止并归零");
      require(std::find(late.keyboards.begin(), late.keyboards.end(), 8) == late.keyboards.end(),
          "等待超期不能重设计划时刻继续发送D"); }
}
void fire_delay_while_moving() {
    for (const int count : {1, 3, 20, 30}) for (const int delay : {100, 300, 600}) {
        const auto p = parse_counterpulse_plan(Json{{"shots", count}, {"capture_enabled", false},
            {"shot_interval_ms", 0}, {"fire_delay_ms", delay}, {"move_ms", 300},
            {"counter_hold_ms", 50}, {"shot_after_release_ms", 5}});
        Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
        require(r["success"] && m.downs == count && !m.held && !m.left, "配置子弹数和并行射击间隔必须生效且完成清理");
        std::int64_t shot_up_ack = 0, shot_up_return = 0, move_ack = 0, move_up_ack = 0, reverse_up_ack = 0;
        int state = 0;
        for (const auto& c : r["commands"]) {
            if (c["shot_index"] == 0) continue;
            const auto submitted = c["submit_ns"].get<std::int64_t>();
            if (c["kind"] == "left_button") {
                if (c["value"] == 0) { shot_up_ack = c["ack_received_ns"]; shot_up_return = c["returned_ns"]; }
                else if (c["shot_index"] != 1) require(state == 0 && submitted - reverse_up_ack == 5000000,
                    "下一枪必须在所有方向键松开后，且反向UP ACK后5ms");
            } else if (c["value"] == 2) {
                require(state == 0 && submitted == shot_up_return, "单发左键UP完成后立刻开始A，不空等射击间隔");
                state = 2; move_ack = c["ack_received_ns"];
            } else if (c["value"] == 8) {
                require(state == 0 && move_up_ack && submitted >= move_up_ack, "按D之前必须已松A并收到ACK");
                state = 8;
            } else if (state == 2) {
                require(submitted == std::max(move_ack + 300000000, shot_up_ack + delay * 1000000LL),
                    "A一直保持到移动时长与射击间隔均满足；两者并行不串加");
                move_up_ack = c["ack_received_ns"]; state = 0;
            } else { reverse_up_ack = c["ack_received_ns"]; state = 0; }
        }
        require(r["cycles"].size() == static_cast<std::size_t>(count - 1), "周期数量应与配置子弹数一致");
    }
    { Fake m; m.unknown_keyboard_call = 3;
      const auto p = parse_counterpulse_plan(Json{{"shots", 3}, {"capture_enabled", false}, {"shot_interval_ms", 0},
          {"fire_delay_ms", 300}, {"move_ms", 300}, {"counter_hold_ms", 50}, {"shot_after_release_ms", 5}});
      const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "COMMAND_NOT_ACKNOWLEDGED" && m.downs == 1 && !m.held && !m.left,
          "A释放ACK未知时立即清理，不能继续反向或开枪");
      require(std::find(m.keyboards.begin(), m.keyboards.end(), 8) == m.keyboards.end(), "A释放未确认绝不发送D"); }
}
void fire_delay_before_moving() {
    require(parse_counterpulse_plan(Json::object()).move_during_fire_delay,
        "缺省开关须保留射后等待期间并行移动的旧行为");
    for (const auto* baseline : {"counter", "no_counter"}) for (const int direction : {2, 8})
        for (const int delay : {0, 100, 600}) {
        const auto p = parse_counterpulse_plan(Json{{"baseline", baseline}, {"direction", direction},
            {"shots", 20}, {"capture_enabled", false}, {"shot_interval_ms", 0},
            {"fire_delay_ms", delay}, {"move_during_fire_delay", false}, {"move_ms", 300},
            {"counter_hold_ms", 5}, {"shot_after_release_ms", 5}});
        Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
        require(r["success"] && m.downs == 20 && !m.held && !m.left,
            "先等射后间隔再移动须完成配置发数并归零");
        require(r["plan"]["move_during_fire_delay"] == false,
            "实际执行报告须保留关闭等待期间移动的布尔配置");
        require(r["timing_model"] == (delay > 0 ? "WAIT_FIRE_DELAY_THEN_MOVE" : "DIRECTION_UP_ACK_DELAY"),
            "串行等待必须在报告中与并行模式区分，零等待沿用释放锚点模式");
        std::int64_t shot_up_ack = 0, move_ack = 0;
        bool moving = false;
        for (const auto& c : r["commands"]) {
            if (c["shot_index"] == 0) continue;
            if (c["kind"] == "left_button" && c["value"] == 0) shot_up_ack = c["ack_received_ns"];
            if (c["kind"] != "wasd") continue;
            const auto submitted = c["submit_ns"].get<std::int64_t>();
            if (c["value"] == direction) {
                require(submitted == shot_up_ack + delay * 1000000LL,
                    "关闭后须从上一枪左键UP ACK等满射后间隔才提交正向DOWN");
                move_ack = c["ack_received_ns"]; moving = true;
            } else if (moving && c["value"] == 0) {
                require(submitted - move_ack == 300000000,
                    "串行模式的移动保持从正向DOWN ACK独立计满move_ms");
                moving = false;
            }
        }
        require(p.cycle_budget_ms() == p.shot_hold_ms + delay + p.move_ms + p.move_tail_ms(),
            "串行等待预算须计入射后等待加移动，不得沿用两者最大值");
    }
    const auto p = parse_counterpulse_plan(Json{{"shots", 3}, {"capture_enabled", false},
        {"shot_interval_ms", 0}, {"fire_delay_ms", 300}, {"move_during_fire_delay", false},
        {"move_ms", 300}, {"counter_hold_ms", 5}, {"shot_after_release_ms", 5}});
    { Fake m; m.physical_at_ms = 100;
      const auto r = execute_counterpulse(m, p, {}, m.clock());
      require(r["failure"] == "PHYSICAL_INPUT_CANCELED" && m.downs == 1 && !m.held && !m.left && m.cleanup_calls == 1,
          "射后静置等待期间仍须检查真实输入并立即清理");
      require(m.keyboards == std::vector<int>{0}, "静置等待取消后不得发出任何移动按键"); }
    { Fake m; auto clock = m.clock(); bool injected = false;
      clock.sleep_until = [&](auto target) {
          m.time = std::max(m.time, target);
          if (!injected && target >= Clock::time_point(std::chrono::seconds(10)) + std::chrono::milliseconds(309)) {
              m.time += std::chrono::milliseconds(6); injected = true;
          }
      };
      const auto r = execute_counterpulse(m, p, {}, clock);
      require(r["failure"] == "COMMAND_DEADLINE_MISSED" && m.downs == 1 && !m.held && !m.left && m.cleanup_calls == 1,
          "射后等待越过计划移动时刻超过容差时应取消，不重设时刻补发");
      require(m.keyboards == std::vector<int>{0}, "等待超期后不得补发正向按键"); }
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
void schema2_release_timing() {
    for (const auto* baseline : {"counter", "no_counter"}) for (const bool parallel : {false, true})
        for (const int delay : {0, 300}) {
        const auto p = parse_counterpulse_plan(Json{{"schema_version", 2}, {"baseline", baseline},
            {"shots", 3}, {"fire_delay_ms", delay}, {"move_during_fire_delay", parallel},
            {"counter_delay_ms", 0}, {"shot_after_release_ms", 0}});
        require(p.uses_release_timing() && !p.capture_enabled && p.shot_interval_ms == 0,
            "版本2即使全部等待为零也必须使用释放锚点且默认不采集");
        const auto serialized = counterpulse_plan_json(p);
        require(serialized["schema_version"] == 2 && !serialized.contains("shot_interval_ms") &&
            !serialized.contains("brake_window_ms"), "版本2序列化必须移除旧参数");
        require(parse_counterpulse_plan(serialized).cycle_budget_ms() == p.cycle_budget_ms(),
            "版本2序列化往返不能改变时序预算");
        Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
        require(r["success"] && m.downs == 3 && !m.left && !m.held && m.cleanup_calls == 1,
            "版本2有界移动射击须完整完成并清理");
        for (const auto& cycle : r["cycles"]) {
            require(!cycle.contains("brake_window_ms") && cycle["shot_anchor_ack_ns"] == cycle["shot_deadline_ns"],
                "零等待必须立即使用最后松键ACK，不暴露或使用旧刹车窗");
            for (const auto& c : r["commands"])
                if (c["shot_index"] == cycle["shot_index"] && c["kind"] == "left_button" && c["value"] == 1)
                    require(c["submit_ns"] == cycle["shot_anchor_ack_ns"], "零等待的实际单发提交不能回退固定窗");
        }
    }
    const auto stationary = parse_counterpulse_plan(Json{{"schema_version", 2}, {"baseline", "stationary"},
        {"shots", 3}, {"fire_delay_ms", 300}});
    Fake m; const auto r = execute_counterpulse(m, stationary, {}, m.clock());
    require(r["success"] && m.downs == 3 && m.keyboards == std::vector<int>{0} &&
        r["timing_model"] == "STATIONARY_UP_ACK_DELAY", "版本2静止基线只按左键释放ACK等待，不发移动");
    std::int64_t previous_up = 0;
    for (const auto& c : r["commands"]) {
        if (c["kind"] != "left_button" || c["shot_index"] == 0) continue;
        if (c["value"] == 0) previous_up = c["ack_received_ns"];
        else if (c["shot_index"] != 1)
            require(c["submit_ns"].get<std::int64_t>() - previous_up == 300000000,
                "静止下一发从实际左键UP ACK等满指定间隔");
    }
    require(stationary.cycle_budget_ms() == 305, "静止预算只含左键保持与射后等待");
    { Fake canceled; canceled.physical_at_ms = 100;
      const auto stopped = execute_counterpulse(canceled, stationary, {}, canceled.clock());
      require(stopped["failure"] == "PHYSICAL_INPUT_CANCELED" && canceled.downs == 1 &&
          !canceled.left && !canceled.held && canceled.cleanup_calls == 1,
          "版本2静止等待期间真实输入仍须及时取消并清理"); }
    { Fake unknown; unknown.unknown_keyboard_call = 3;
      const auto zero = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 3}});
      const auto stopped = execute_counterpulse(unknown, zero, {}, unknown.clock());
      require(stopped["failure"] == "COMMAND_NOT_ACKNOWLEDGED" && unknown.downs == 1 &&
          !unknown.left && !unknown.held && unknown.cleanup_calls == 1,
          "版本2零等待不能绕过正向UP未知ACK，必须停止并归零");
      require(std::find(unknown.keyboards.begin(), unknown.keyboards.end(), 8) == unknown.keyboards.end(),
          "版本2正向释放未确认不得发送反向按键"); }
    const auto bounded = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 30},
        {"fire_delay_ms", 1000}, {"move_ms", 500}, {"counter_hold_ms", 200}, {"shot_after_release_ms", 20}});
    require(bounded.cycle_budget_ms() == 1225, "并行等待预算取最大值且完整计入反向和释放后等待");
    Fake slow; slow.latency_ms = 40;
    const auto stopped = execute_counterpulse(slow, bounded, {}, slow.clock());
    require(!stopped["success"].get<bool>() && slow.downs < 30 && !slow.left && !slow.held && slow.cleanup_calls == 1,
        "版本2累计ACK超预算仍必须取消清理，不能挤压时序或补射");
    for (const auto& patch : {Json{{"schema_version", 1}}, Json{{"schema_version", 3}}, Json{{"schema_version", 2.5}},
        Json{{"shot_interval_ms", 0}}, Json{{"brake_window_ms", 60}}, Json{{"capture_enabled", true}},
        Json{{"baseline", "stationary"}, {"fire_delay_ms", 0}}, Json{{"fire_delay_ms", 2001}},
        Json{{"shots", 30}, {"fire_delay_ms", 1000}, {"move_ms", 500}, {"move_during_fire_delay", false}},
        Json{{"shots", 30}, {"fire_delay_ms", 2000}}, Json{{"shots", 31}}, Json{{"shot_hold_ms", 0}},
        Json{{"move_ms", 0}}, Json{{"counter_hold_ms", 0}}, Json{{"counter_delay_ms", 201}},
        Json{{"baseline", "no_counter"}, {"counter_delay_ms", 1}}, Json{{"direction", 3}},
        Json{{"shot_after_release_ms", 21}}, Json{{"late_tolerance_ms", 11}}, Json{{"unknown", 1}}}) {
        Json invalid{{"schema_version", 2}}; invalid.update(patch);
        bool rejected = false; try { parse_counterpulse_plan(invalid); } catch (...) { rejected = true; }
        require(rejected, "版本2必须拒绝旧参数、版本错误和超范围计划");
    }
}
void schema2_weapon_hold_and_interval() {
    for (const auto* baseline : {"counter", "no_counter", "stationary"})
        for (const int interval : {0, 500, 1500, 5000}) {
        const auto p = parse_counterpulse_plan(Json{{"schema_version", 2}, {"baseline", baseline},
            {"shots", 3}, {"shot_hold_ms", 1000}, {"fire_interval_ms", interval}, {"fire_delay_ms", 100}});
        require(counterpulse_plan_json(p)["fire_interval_ms"] == interval,
            "武器DOWN最小间隔必须写入正式计划并在执行中保留");
        Fake m; const auto r = execute_counterpulse(m, p, {}, m.clock());
        require(r["success"] && m.downs == 3 && !m.left && !m.held && m.cleanup_calls == 1,
            "一秒开火保持必须完成全部周期并纳入最后一发的全程预算");
        std::int64_t previous_down = 0, previous_up = 0, down_ack = 0;
        for (const auto& c : r["commands"]) {
            if (c["shot_index"] == 0) continue;
            const auto submit = c["submit_ns"].get<std::int64_t>();
            if (c["kind"] == "wasd" && c["value"] == p.direction)
                require(submit == std::max(previous_up, previous_down + interval * 1000000LL),
                    "移动组必须先等到前DOWN的最小间隔，再开始完整移动");
            if (c["kind"] != "left_button") continue;
            if (c["value"] == 1) {
                if (previous_down) {
                    require(submit - previous_down >= interval * 1000000LL,
                        "相邻DOWN实际提交不能早于武器最小间隔");
                    if (p.baseline == "stationary")
                        require(submit == std::max(previous_up + 100000000, previous_down + interval * 1000000LL),
                            "静止组取UP射后等待与DOWN最小间隔的较晚时刻");
                }
                previous_down = submit; down_ack = c["ack_received_ns"];
            } else {
                require(submit - down_ack == 1000000000,
                    "每次左键从DOWN ACK完整保持一秒，不能仅改报告值");
                previous_up = c["ack_received_ns"];
            }
        }
        if (p.baseline != "stationary") for (const auto& cycle : r["cycles"])
            require(cycle["shot_deadline_ns"] == cycle["shot_anchor_ack_ns"],
                "武器冷却等待不得追加在最后方向键松开之后");
    }
    const auto long_hold = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 3}, {"shot_hold_ms", 2000}});
    { Fake m; const auto r = execute_counterpulse(m, long_hold, {}, m.clock());
      require(r["success"] && m.downs == 3 && !m.left && !m.held,
          "两秒保持上限仍须包括末枪保持，不能被旧固定三秒预算截断"); }
    { Fake m; m.physical_at_ms = 500;
      const auto r = execute_counterpulse(m, long_hold, {}, m.clock());
      require(r["failure"] == "PHYSICAL_INPUT_CANCELED" && m.downs == 1 && !m.left && !m.held && m.cleanup_calls == 1,
          "长开火保持期间真实输入取消仍须立即松左键和方向键");
      require(m.time < Clock::time_point(std::chrono::seconds(11)), "取消不能等到两秒保持结束才清理"); }
    require(parse_counterpulse_plan(Json{{"schema_version", 2}, {"baseline", "stationary"},
        {"shots", 20}, {"shot_hold_ms", 1999}, {"fire_delay_ms", 1}}).shot_hold_ms == 1999,
        "接近40秒但仍含末枪保持的合法计划应可解析");
    for (const auto& invalid : {Json{{"schema_version", 2}, {"shot_hold_ms", 2001}},
        Json{{"schema_version", 2}, {"fire_interval_ms", 5001}}, Json{{"schema_version", 2}, {"fire_interval_ms", -1}},
        Json{{"schema_version", 2}, {"shots", 30}, {"fire_interval_ms", 2000}},
        Json{{"schema_version", 2}, {"baseline", "stationary"}, {"shots", 20}, {"shot_hold_ms", 2000}, {"fire_delay_ms", 1}},
        Json{{"shot_hold_ms", 21}}, Json{{"fire_interval_ms", 0}}}) {
        bool rejected = false; try { parse_counterpulse_plan(invalid); } catch (...) { rejected = true; }
        require(rejected, "必须拒绝武器参数越界、含末枪超40秒以及旧版本新增参数或超过20ms保持");
    }
}
void wait_deadline_reached_during_check() {
    Fake m; const auto p = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 1}});
    bool crossed = false;
    const auto r = execute_counterpulse(m, p, [&]() -> std::string {
        // 模拟全松/焦点检查消耗最后等待时间；随后sleep_until过去时刻立即返回。
        if (m.left && !crossed) { m.time += std::chrono::milliseconds(5); crossed = true; }
        return {};
    }, m.clock());
    require(crossed && r["success"] && m.downs == 1 && !m.left && !m.held && m.cleanup_calls == 1,
        "检查期间已到deadline且同tick返回不能误判CLOCK_NOT_ADVANCING");
}
void wait_transient_equal_clock_ticks() {
    Fake m; const auto p = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 1}});
    auto clock = m.clock(); int equal_ticks = 0;
    clock.sleep_until = [&](auto target) {
        if (equal_ticks < 3) { ++equal_ticks; return; }
        m.time = std::max(m.time, target);
    };
    const auto r = execute_counterpulse(m, p, {}, clock);
    require(equal_ticks == 3 && r["success"] && m.downs == 1 && !m.left && !m.held && m.cleanup_calls == 1,
        "有限同tick读取后恢复推进不等于冻结时钟，不得截断动作");
}
void command_observer_failure_keeps_cleanup() {
    Fake m; const auto p = parse_counterpulse_plan(Json{{"schema_version", 2}, {"shots", 3}});
    int callbacks = 0;
    const auto r = execute_counterpulse(m, p, {}, m.clock(), false, [&](const Json& command) {
        require(command.contains("ack_received_ns") && command.contains("returned_ns"),
            "观察器只能消费完整记录的命令回执");
        ++callbacks;
        throw std::runtime_error("测试HUD观察器失败");
    });
    require(r["success"] && m.downs == 3 && !m.left && !m.held && m.cleanup_calls == 1,
        "HUD观察器异常不得跳过开火释放或最终清理");
    require(callbacks == static_cast<int>(r["commands"].size()) && r["observer_failures"] == callbacks,
        "每条已记录回执恰好通知一次，观察器失败须显式计数而非冒充正常");
}
void invalid_plans() {
    for (const auto& json : {Json{{"counter_delay_ms", 201}}, Json{{"baseline", "stationary"}, {"counter_delay_ms", 50}}, Json{{"fire_delay_ms", 300}}, Json{{"fire_delay_ms", 2001}},
        Json{{"move_during_fire_delay", 0}}, Json{{"move_during_fire_delay", "false"}}, Json{{"move_during_fire_delay", nullptr}},
        Json{{"shots", 30}, {"capture_enabled", false}, {"shot_interval_ms", 0}, {"fire_delay_ms", 1000},
            {"move_ms", 500}, {"move_during_fire_delay", false}, {"shot_after_release_ms", 5}},
        Json{{"capture_enabled", false}, {"shots", 30}, {"shot_interval_ms", 0}, {"fire_delay_ms", 2000}, {"shot_after_release_ms", 5}},
        Json{{"shots", 30}, {"shot_interval_ms", 650}}, Json{{"shot_interval_ms", 0}},
        Json{{"baseline", "stationary"}, {"shot_interval_ms", 0}, {"shot_after_release_ms", 5}},
        Json{{"shot_interval_ms", 0}, {"move_ms", 500}, {"counter_hold_ms", 200}, {"shot_after_release_ms", 5}},
        Json{{"move_ms", 501}}, Json{{"capture_enabled", 0}}, Json{{"shots", 31}}, Json{{"direction", 258}}, Json{{"late_tolerance_ms", 11}},
        Json{{"shot_after_release_ms", 21}}, Json{{"shot_after_release_ms", -1}},
        Json{{"shot_after_release_ms", 5}, {"move_ms", 250}, {"counter_hold_ms", 200}},
        Json{{"brake_window_ms", 0}}, Json{{"counter_hold_ms", 60}},
        Json{{"shot_interval_ms", 279}}, Json{{"shots", 7}, {"shot_interval_ms", 651}},
        Json{{"shots", 0}},
        Json{{"shots", 7}, {"shot_interval_ms", 500.5}},
        Json{{"move_ms", 250}, {"counter_hold_ms", 100}}, Json{{"shots", 7.5}}}) {
        bool rejected = false; try { parse_counterpulse_plan(json); } catch (...) { rejected = true; }
        require(rejected, "非法计划应拒绝");
    }
}
}
int main() {
    try { command_observer_failure_keeps_cleanup(); wait_deadline_reached_during_check(); wait_transient_equal_clock_ticks(); schema2_weapon_hold_and_interval(); schema2_release_timing(); fire_delay_before_moving(); delayed_reverse_tap_then_fire(); fire_delay_while_moving(); immediate_movement_cycles(); shot_after_direction_release(); matched_brake_window(); successful_and_baselines(); configurable_stationary_intervals(); failures_stop_and_cleanup(); invalid_plans(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "反冲纯fake专项通过：时序、基线、预算、取消、未知ACK、清理及参数拒绝\n";
}
