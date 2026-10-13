#include "movement/recording.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using Event = MovementTraceEvent;
using Kind = Event::Kind;
using namespace movement;
constexpr std::int64_t kBegin = 1000000000LL;
std::int64_t stamp(int ms) { return kBegin + ms * 1000000LL; }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Event input(int ms, int wasd = 0, bool baseline = false) {
    Event e; e.kind = Kind::INPUT; e.at_ns = stamp(ms); e.baseline = baseline;
    e.input.state_valid = true; e.input.wasd_mask = static_cast<std::uint8_t>(wasd); return e;
}
Event software(Kind kind, int ms) {
    Event e; e.kind = kind; e.at_ns = e.completed_ns = stamp(ms); e.acknowledged = true; return e;
}
Event mask(int ms, int bits, bool enabled) {
    auto e = software(Kind::MASK, ms); e.wasd = static_cast<std::uint8_t>(bits); e.masked = enabled; return e;
}
const timeline::Action& action(const timeline::Plan& plan, timeline::ActionKind kind,
        timeline::Key key, bool physical = false) {
    auto found = std::find_if(plan.actions.begin(), plan.actions.end(), [&](const auto& a) {
        return a.kind == kind && a.key == key && a.physical == physical;
    });
    require(found != plan.actions.end(), "预期录制轨道缺失"); return *found;
}
void longjump_tracks() {
    std::vector<Event> events{input(0, 1, true)};
    auto down = software(Kind::KEYBOARD, 10); down.wasd = 2; down.space = true;
    events.push_back(down); events.push_back(mask(12, 1, true)); events.push_back(mask(14, 2, true));
    auto move = software(Kind::MOVE, 15); move.dx = -12; move.dy = 7; events.push_back(move);
    auto wheel = software(Kind::WHEEL, 16); wheel.wheel = -2; events.push_back(wheel);
    down = software(Kind::KEYBOARD, 20); down.wasd = 8; down.ctrl = true; events.push_back(down);
    events.push_back(mask(25, 1, false)); events.push_back(mask(28, 2, false));
    events.push_back(software(Kind::KEYBOARD, 30));
    auto physical = input(5, 1); physical.input.keyboard_usages[0] = 0x2c;
    physical.input.raw_x = 3; physical.input.raw_y = -2; physical.input.raw_wheel = 1;
    events.push_back(physical); events.push_back(input(18, 1));
    // 特意打乱输入顺序，派生以报告/回执完成时刻稳定排序。
    std::reverse(events.begin(), events.end());
    auto result = recording::derive(events, kBegin, stamp(40), false, 1);
    require(result.complete && !result.plan.mask.enabled, "Long Jump轨迹未派生或附加了默认屏蔽");
    using A = timeline::ActionKind; using K = timeline::Key;
    const auto& w = action(result.plan, A::KEY, K::W, true);
    require(w.start_ms == 0 && w.duration_ms == 40, "贯穿Long Jump的物理W丢失");
    const auto& a = action(result.plan, A::KEY, K::A);
    const auto& d = action(result.plan, A::KEY, K::D);
    require(a.start_ms == 10 && a.duration_ms == 10 && d.start_ms == 20 && d.duration_ms == 10, "软件AD轨道时间错误");
    require(action(result.plan, A::KEY, K::CTRL).duration_ms == 10, "软件蹲轨道缺失");
    require(action(result.plan, A::KEY, K::SPACE).duration_ms == 10, "软件跳跃轨道缺失");
    require(action(result.plan, A::KEY, K::SPACE, true).duration_ms == 13, "物理跳跃时长错误");
    const auto& mw = action(result.plan, A::MASK, K::W); const auto& ma = action(result.plan, A::MASK, K::A);
    require(mw.start_ms == 12 && mw.duration_ms == 13 && ma.start_ms == 14 && ma.duration_ms == 14, "逐键屏蔽区间合并错误");
    int x = 0, y = 0, wheels = 0;
    for (const auto& item : result.plan.actions) {
        if (item.kind == A::MOVE) { x += item.dx; y += item.dy; }
        if (item.kind == A::WHEEL) wheels += item.wheel;
    }
    require(x == -9 && y == 5 && wheels == -1, "物理与软件XY或滚轮丢失");
}
void cumulative_wrap() {
    auto first = input(0, 0, true); first.input.raw_x = 32766; first.input.raw_y = -32767; first.input.raw_wheel = 32767;
    auto next = input(2); next.input.raw_x = -32766; next.input.raw_y = 32766; next.input.raw_wheel = -32768;
    auto result = recording::derive({first, next}, kBegin, stamp(20), false, 2);
    require(result.complete, "累计报告未生成回放");
    int x = 0, y = 0, wheel = 0;
    for (const auto& a : result.plan.actions) { x += a.dx; y += a.dy; wheel += a.wheel; }
    require(x == 4 && y == -3 && wheel == 1, "累计16位报告回绕错误");
}
void incomplete_rejected() {
    const std::vector<Event> valid{input(0, 1, true)};
    require(!recording::derive(valid, kBegin, stamp(20), true, 1).complete, "缺口轨迹仍生成回放");
    require(!recording::derive(valid, kBegin, stamp(20), false, 0).complete, "仅原始模式生成了回放");
    auto invalid = valid; invalid.push_back(input(10)); invalid.back().input.state_valid = false;
    require(!recording::derive(invalid, kBegin, stamp(20), false, 1).complete, "无效物理状态仍生成回放");
    auto unknown = valid; unknown.push_back(software(Kind::MOVE, 10)); unknown.back().acknowledged = false;
    require(!recording::derive(unknown, kBegin, stamp(20), false, 1).complete, "未知ACK仍生成回放");
    auto held = valid; held.push_back(software(Kind::KEYBOARD, 10)); held.back().ctrl = true;
    require(!recording::derive(held, kBegin, stamp(20), false, 1).complete, "软件键未释放仍生成回放");
    auto masked = valid; masked.push_back(mask(10, 8, true));
    require(!recording::derive(masked, kBegin, stamp(20), false, 1).complete, "屏蔽未恢复仍生成回放");
    auto missing = input(0); missing.input.raw_x = 1;
    require(!recording::derive({missing}, kBegin, stamp(20), false, 1).complete, "有位移而缺基线仍生成回放");
    auto pending = software(Kind::MOVE, 19); pending.completed_ns = stamp(21); pending.dx = 1;
    require(!recording::derive({valid[0], pending}, kBegin, stamp(20), false, 1).complete,
        "录制边界外完成的在途命令被静默忽略");
    auto released = input(20, 0);
    const auto ended = recording::derive({valid[0], released}, kBegin, stamp(20), false, 1);
    require(ended.complete && action(ended.plan, timeline::ActionKind::KEY, timeline::Key::W, true).duration_ms == 20,
        "停止边界上的释放被提前截短");
}
void scoped_raw_trace() {
    auto e = input(0, 1, true); e.input.mouse_buttons = 31; e.input.keyboard_usages[0] = 0x04;
    e.input.keyboard_modifiers = 2; e.input.raw_report.fill(0xff);
    auto move = software(Kind::MOVE, 2); move.completed_ns = stamp(3); move.dx = 4; move.dy = -1;
    const auto trace = recording::trace_json({e, move}, kBegin, stamp(20), true, 0);
    require(trace.at("motion_interpretation") == "raw_only" && trace.at("gap") == true &&
        trace.at("game_validation_passed") == false, "原始记录证据边界丢失");
    require(trace.at("events")[0].at("keys") == 1 && !trace.at("events")[0].contains("mouse_buttons") &&
        !trace.at("events")[0].contains("keyboard_usages") && !trace.at("events")[0].contains("raw_report"),
        "记录了非身法按键或鼠标按钮");
    require(trace.at("events")[1].at("request_ns") == 2000000 && trace.at("events")[1].at("completed_ns") == 3000000,
        "软件请求与回执时间未分别保留");
    auto result = recording::derive({e}, kBegin, stamp(20), false, 1);
    require(result.complete && result.plan.actions.size() == 1 && result.plan.actions[0].key == timeline::Key::W,
        "非身法键或按钮被派生为动作");
}
void stop_boundary() {
    auto down = software(Kind::KEYBOARD, 10); down.wasd = 2;
    auto released = software(Kind::KEYBOARD, 20);
    auto result = recording::derive({input(0, 1, true), down, released}, kBegin, stamp(20), false, 1);
    require(result.complete, "停止时已完成的释放未被包含");
    const auto& key = action(result.plan, timeline::ActionKind::KEY, timeline::Key::A);
    require(key.start_ms == 10 && key.duration_ms == 10, "停止边界释放被截短1毫秒");
    auto pending = software(Kind::MOVE, 19); pending.completed_ns = stamp(21); pending.dx = 7;
    result = recording::derive({input(0, 1, true), pending}, kBegin, stamp(20), false, 1);
    require(!result.complete && !result.error.empty(), "停止时仍在途的软件命令被忽略并生成回放");
}
}
int main() {
    try {
        longjump_tracks(); cumulative_wrap(); incomplete_rejected(); scoped_raw_trace(); stop_boundary();
        std::cout << "身法录制派生专项通过\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
