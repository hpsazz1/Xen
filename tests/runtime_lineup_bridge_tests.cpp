#include "runtime/lineup_bridge_internal.h"
#include <iostream>
#include <vector>
#include <thread>
using namespace std::chrono_literals;
using namespace lineup::detail;
namespace {
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
class FakeMouse final : public IMouseController {
public:
    lineup::Clock::time_point now = lineup::Clock::now();
    bool unknown_up = false;
    int moves = 0;
    std::vector<std::pair<std::string,bool>> events;
    bool open() noexcept override { return false; } // 测试绝不打开设备。
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves; MouseMoveReceipt r; r.succeeded = true; r.protocol_ack_received = true;
        r.backend_completed_at = now; return r;
    }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_left_button() const noexcept override { return true; }
    bool supports_lineup_inputs() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    ButtonReceipt set_left_button(bool down) noexcept override {
        events.emplace_back("left",down); ButtonReceipt r;
        r.disposition = !down && unknown_up ? ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED;
        r.datagram_sent = true; r.backend_completed_at = now; return r;
    }
    ButtonReceipt set_right_button(bool down) noexcept override { auto r=set_left_button(down); events.back().first="right"; return r; }
    KeyboardReceipt set_space_key(bool down) noexcept override {
        events.emplace_back("space",down); KeyboardReceipt r; r.disposition=KeyboardDisposition::ACKNOWLEDGED; r.backend_completed_at=now; return r;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        events.emplace_back("wasd",mask!=0); KeyboardReceipt r; r.disposition=KeyboardDisposition::ACKNOWLEDGED; r.backend_completed_at=now; return r;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
};
ExecutionObservation observation(lineup::Clock::time_point now, std::uint64_t sequence) {
    ExecutionObservation o;
    o.identity = {"recipe","reference","source","session",1,1,{320,320,2560,1440,320,320,1120,560,1,1,false}};
    o.sequence = sequence; o.captured_at=now; o.source_at=now; o.source_uncertainty=0ms; o.valid=true;
    return o;
}
ExecutionCalibration calibration(const ExecutionObservation& o) {
    return {"fake-unit-test-only", "source", o.identity.geometry, true, .5, .5, 1ms};
}
void run_hold(bool revoke, bool unknown) {
    auto mouse=std::make_shared<FakeMouse>(); auto arbiter=std::make_shared<AutoStopOutputArbiter>();
    bool active=false, focused=true; int faults=0;
    ExecutionPermit permit{true,true,true,true,true}; permit.input_state_valid=true;
    runtime::detail::LineupExecutionSession session(mouse,arbiter,[&]{auto p=permit;p.focused=focused;return p;},
        [&](bool value){active=value;},[]{return true;},[&]{++faults;}, {12,10,1000ms,100ms,2,6});
    auto o=observation(mouse->now,1); auto c=calibration(o);
    expect(session.locate(o,c,mouse->now), "显式定位必须获得现有仲裁器");
    for (int i=0;i<3;++i) { mouse->now+=1ms; o=observation(mouse->now,2+i); session.observe(o,mouse->now); }
    expect(session.snapshot().state==ExecutionState::ALIGNED, "三个新帧可到对齐状态");
    ExecutionThrowPlan plan; plan.id="plan";plan.validated=true;plan.identity=o.identity;
    plan.action={{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
        {{"buttons",nlohmann::json::array({"left","right"})},{"movement",nlohmann::json::array()},{"jump",true},{"duration_ms",30}},
        {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",0}}
    })}};
    const auto design = inspect_action(plan.action);
    expect(design.valid && design.configured && design.timing_complete,
           "完整组合夹具必须显式包含最终全释放阶段");
    mouse->now+=1ms; o=observation(mouse->now,5);
    const bool submitted = session.throw_edge(o,plan,mouse->now);
    expect(submitted, "完整组合必须可提交到fake sink");
    if (!submitted) {
        std::cerr << "throw rejection reason: " << session.snapshot().reason << '\n';
        return;
    }
    expect(active && faults==0 && session.snapshot().state==ExecutionState::HOLDING,
           "正常持有按钮的cleanup债务不是故障，不得提前撤销");
    bool competitor_entered = false;
    std::thread competitor([&] { auto lock = arbiter->try_enter_aim(); competitor_entered = lock.owns_lock(); });
    competitor.join();
    expect(!competitor_entered, "道具持有期间竞争Aim不能进入同一arbiter");
    expect(mouse->events.size()==3 && mouse->events[0].second && mouse->events[1].second && mouse->events[2].second,
           "阶段开始只Down已计划的三个控制");
    mouse->unknown_up=unknown;
    if (revoke) focused=false;
    mouse->now+=31ms;session.tick(mouse->now);
    expect(mouse->events.size()==6, "结束或撤销仅释放三个owned控制");
    if (unknown) expect(active && faults>0 && session.snapshot().state==ExecutionState::UNKNOWN,
                        "未知UP必须保持竞争输出阻断并上报故障");
    else {
        expect(!active && faults==0, "确认归还后释放独占会话");
        expect(session.snapshot().state==(revoke?ExecutionState::CANCELLED:ExecutionState::COMPLETED), "正常完成与撤销准确区分");
        auto resumed=arbiter->try_enter_aim(); expect(resumed.owns_lock(), "归还后原Aim仲裁可恢复");
    }
}
void test_missing_calibration() {
    auto mouse=std::make_shared<FakeMouse>();auto arbiter=std::make_shared<AutoStopOutputArbiter>();bool active=false;
    ExecutionPermit permit{true,true,true,true,true};permit.input_state_valid=true;
    runtime::detail::LineupExecutionSession session(mouse,arbiter,[&]{return permit;},[&](bool v){active=v;},[]{return true;},[]{}, {12,10,1000ms,100ms,2,6});
    auto o=observation(mouse->now,1);o.error_x=12;
    expect(!session.locate(o,ExecutionCalibration{},mouse->now), "缺实测标定不得开始对齐");
    expect(mouse->moves==0&&mouse->events.empty()&&!active, "缺标定零输出且不占住竞争功能");
}
}
int main() {run_hold(false,false);run_hold(true,false);run_hold(true,true);test_missing_calibration();
    if(failures)return 1;std::cout<<"runtime_lineup_bridge_tests passed\n";return 0;}
