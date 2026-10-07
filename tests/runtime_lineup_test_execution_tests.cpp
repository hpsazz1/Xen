#include "runtime/lineup_test_execution_internal.h"
#include "runtime/lineup_bridge_internal.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
using namespace lineup;
using namespace lineup::detail;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
control::Snapshot request(Clock::time_point now, std::uint64_t trigger = 11) {
    control::Snapshot s;
    s.connected = s.available = true; s.connection_epoch = 9;
    s.message_sequence = s.locate_sequence = 30; s.received_at = now; s.valid_until = now + 350ms;
    s.request.mode = control::Mode::LOCATE; s.request.reference_version = 3; s.request.trigger_sequence = trigger;
    auto &o = s.request.observation;
    o.identity = {"recipe", "reference", "synthetic", "source-clock-fixture", 2, 4,
        {320,320,1920,1080,320,320,800,380,1,1,true}};
    o.sequence = 1; o.valid = true; o.captured_at = now; o.source_at = now; o.source_uncertainty = 0ms;
    s.request.throw_action = {{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
        {{"buttons",nlohmann::json::array({"left"})},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",30}},
        {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",0}}
    })}};
    return s;
}
control::Snapshot before_press(Clock::time_point now) {
    auto s = request(now, 5); s.message_sequence = s.locate_sequence = 20;
    return s;
}
class FakeMouse final : public IMouseController {
public:
    Clock::time_point now = Clock::now();
    int opens = 0, moves = 0;
    std::vector<bool> buttons;
    bool open() noexcept override { ++opens; return false; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot &) noexcept override { return false; }
    bool supports_left_button() const noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand &) noexcept override {
        ++moves; MouseMoveReceipt r; r.succeeded = r.protocol_ack_received = true; r.backend_completed_at = now; return r;
    }
    ButtonReceipt set_left_button(bool held) noexcept override {
        buttons.push_back(held); ButtonReceipt r; r.disposition = ButtonDisposition::ACKNOWLEDGED;
        r.datagram_sent = true; r.backend_completed_at = now; return r;
    }
};
void test_single_aligned_throw() {
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    ExecutionPermit permit{true,true,true,true,true}; permit.input_state_valid = true;
    bool active = false;
    runtime::detail::LineupExecutionSession session(mouse, arbiter, [&] { return permit; },
        [&](bool value) { active = value; }, [] { return true; }, [] {}, {12,40,3000ms,100ms,2,6});
    runtime::detail::LineupTestExecution test;
    const auto began = mouse->now;
    auto old = before_press(began);
    check(test.begin(11, old, ExecutionState::ALIGNED, began), "F9 新边沿可替代旧定位");
    check(!test.begin(12, old, ExecutionState::ALIGNED, began), "等待期间重复 F9 不排队");
    check(!test.take_throw(old, ExecutionState::ALIGNED, began), "旧已对齐快照不能直接投掷");
    old.message_sequence += 1; old.request.mode = control::Mode::OBSERVATION;
    test.track(old, began + 1ms);
    check(test.pending(), "已在途旧观察只等待，不绑定或续期");
    auto s = request(began + 2ms);
    test.track(s, began + 2ms);
    check(!test.take_throw(s, ExecutionState::ALIGNED, began + 2ms), "匹配事件尚未开始本次定位，旧会话对齐仍不能投");
    auto &o = s.request.observation;
    ExecutionCalibration calibration{"fake-only", "synthetic", o.identity.geometry, true, .5, .5, 1ms};
    mouse->now += 2ms;
    check(session.locate(o, calibration, mouse->now), "本次定位通过现有 Session");
    test.locate_started(s, true, mouse->now);
    for (int i = 0; i < 4; ++i) {
        mouse->now += 3ms; ++o.sequence; ++s.message_sequence;
        o.captured_at = mouse->now; o.source_at = mouse->now;
        o.error_x = i == 0 ? 12 : 0; s.request.mode = control::Mode::OBSERVATION;
        s.valid_until = mouse->now + 350ms;
        session.observe(o, mouse->now);
        const bool fire = test.take_throw(s, session.snapshot().state, mouse->now);
        check(fire == (i == 3), "仅本次回准后三个新帧确认时消费投掷");
        if (fire) {
            ExecutionThrowPlan plan; plan.id = "recipe:2"; plan.identity = o.identity;
            plan.action = s.request.throw_action; plan.validated = true;
            check(session.throw_edge(o, plan, mouse->now), "单次意图继续经过原投掷门禁");
        }
    }
    check(mouse->moves == 1 && mouse->buttons == std::vector<bool>{true}, "fake 链先移动再按下投掷");
    check(!test.rejects_trigger(11), "投掷执行期间保留会话，不提前释放");
    check(!test.begin(12, s, session.snapshot().state, mouse->now), "holding 时新 F9 不排队");
    check(!test.locate_only(session.snapshot().state) && mouse->buttons == std::vector<bool>{true},
        "holding 时 F8 拒绝而非提前释放投掷按钮");
    check(!test.take_throw(s, ExecutionState::ALIGNED, mouse->now), "同一次对齐不会再次投掷");
    mouse->now += 31ms; session.tick(mouse->now); test.settle(session.snapshot().state);
    check(!active && mouse->buttons == std::vector<bool>({true,false}) && mouse->opens == 0,
        "按计划一次释放，未打开真实设备");
    check(test.rejects_trigger(11), "完成后同一旧事件不得重放");
    s = request(mouse->now, 12);
    check(session.locate(s.request.observation, calibration, mouse->now), "独立 F8 可开始下一次定位");
    for (int i = 0; i < 3; ++i) {
        mouse->now += 3ms; ++s.request.observation.sequence;
        s.request.observation.captured_at = mouse->now; s.request.observation.source_at = mouse->now;
        session.observe(s.request.observation, mouse->now);
    }
    check(test.locate_only_aligned(12, session.snapshot().state), "F8 三帧对齐可立即归还会话");
    session.cancel(mouse->now);
    check(!active && mouse->buttons.size() == 2, "F8 完成归还输出且未新增投掷");
}
void test_cancel_and_timeout() {
    const auto now = Clock::now();
    auto s = request(now);
    runtime::detail::LineupTestExecution f8;
    check(!f8.take_throw(s, ExecutionState::ALIGNED, now), "F8 只有定位，不产生待投意图");
    check(f8.begin(11, before_press(now), ExecutionState::IDLE, now), "开始 F9 测试");
    check(f8.locate_only(ExecutionState::ALIGNING), "尚未投掷时 F8 可取消待投"); f8.track(s, now);
    check(!f8.take_throw(s, ExecutionState::ALIGNED, now) && f8.rejects_trigger(11), "F8 取消待投且迟到关联不恢复");
    for (int failure = 0; failure < 9; ++failure) {
        runtime::detail::LineupTestExecution test;
        check(test.begin(11, before_press(now), ExecutionState::IDLE, now), "异常夹具开始");
        auto changed = s; test.track(changed, now); test.locate_started(changed, true, now);
        auto checked_at = now + 1ms;
        if (failure == 0) changed.connected = false;
        if (failure == 1) ++changed.connection_epoch;
        if (failure == 2) ++changed.request.observation.identity.recipe_version;
        if (failure == 3) ++changed.request.reference_version;
        if (failure == 4) ++changed.request.trigger_sequence;
        if (failure == 5) changed.request.mode = control::Mode::CANCEL;
        if (failure == 6) changed.request.throw_action["phases"][0]["duration_ms"] = 31;
        if (failure == 7) changed.valid_until = now;
        if (failure == 8) { checked_at = now + 4000ms; changed.valid_until = checked_at + 350ms; }
        check(!test.take_throw(changed, ExecutionState::ALIGNED, checked_at) && !test.pending(),
            "断连、重连、版本、序号、取消、动作、快照过期和总超时均清除待投");
        check(!test.take_throw(s, ExecutionState::ALIGNED, now + 2ms), "失败后旧有效快照不能恢复");
    }
}
void test_binding_and_failure() {
    const auto now = Clock::now(); auto s = request(now);
    control::Snapshot idle; idle.connected = true; idle.connection_epoch = 9; idle.message_sequence = 20;
    runtime::detail::LineupTestExecution waiting;
    check(waiting.begin(11, idle, ExecutionState::IDLE, now), "空闲取消基线允许新的 F9 意图");
    waiting.track(idle, now + 100ms);
    check(waiting.pending() && !waiting.take_throw(idle, ExecutionState::ALIGNED, now + 100ms),
        "同一个空闲取消基线只等待，不能借旧已对齐状态投掷");
    auto cancelled = idle; ++cancelled.message_sequence;
    waiting.track(cancelled, now + 101ms);
    check(!waiting.pending(), "真正新的取消消息清除等待意图");
    for (int failure = 0; failure < 4; ++failure) {
        runtime::detail::LineupTestExecution test;
        check(test.begin(11, before_press(now), ExecutionState::IDLE, now), "绑定夹具开始");
        auto changed = s;
        if (failure == 0) changed.request.trigger_sequence = 10;
        if (failure == 1) changed.request.trigger_sequence = 0;
        if (failure == 2) changed.request.throw_action["phases"][0]["duration_ms"] = nullptr;
        if (failure == 3) changed.request.observation.valid = false;
        test.track(changed, now);
        check(!test.pending() && !test.take_throw(s, ExecutionState::ALIGNED, now),
            "旧序号、网页无绑定、时序不完整和无效观察均拒绝");
    }
    runtime::detail::LineupTestExecution test;
    auto disconnected = before_press(now); disconnected.connected = false;
    check(!test.begin(11, disconnected, ExecutionState::IDLE, now), "离线按键不排队");
    check(!test.begin(11, s, ExecutionState::HOLDING, now), "已有持键时不能创建新意图");
    check(test.begin(11, before_press(now), ExecutionState::IDLE, now), "失败定位夹具开始");
    test.track(s, now); test.locate_started(s, false, now);
    check(!test.pending() && !test.take_throw(s, ExecutionState::ALIGNED, now), "标定或输出门禁拒绝定位后不投掷");
}
}
int main() {
    try {
        test_single_aligned_throw(); test_cancel_and_timeout(); test_binding_and_failure();
        std::cout << "runtime_lineup_test_execution_tests passed (software-only, no device)\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
