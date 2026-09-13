#include "auto_stop_probe/preroll_internal.h"
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
using namespace auto_stop_probe_detail;
constexpr std::int64_t ms = 1000000;
void expect(PrerollDecision d, PrerollState state, const char* reason) {
    if (d.state != state || std::string_view(d.reason) != reason) throw std::runtime_error("预采集状态或原因不符");
}
void matrix() {
    // 实机capture-check轨迹：首帧延迟463902700ns，旧300ms窗口只有0帧。
    constexpr std::int64_t started = 1;
    constexpr std::int64_t first_delay = 463902700;
    CounterpulsePrerollGate observed(started);
    for (int elapsed : {100, 200, 300, 400})
        expect(observed.evaluate(started+elapsed*ms,0,0,0,false,false),PrerollState::WAIT,"WAIT_FIRST_FRAME");
    expect(observed.evaluate(started+first_delay,started+first_delay,started+first_delay,1,false,false),
        PrerollState::WAIT,"CAPTURE_OBSERVING");
    expect(observed.evaluate(started+first_delay+300*ms,started+first_delay,started+first_delay+300*ms,16,false,false),
        PrerollState::READY,"CAPTURE_READY");
    CounterpulsePrerollGate gate(ms);
    expect(gate.evaluate(301*ms,0,0,0,false,false),PrerollState::WAIT,"WAIT_FIRST_FRAME");
    expect(gate.evaluate(1001*ms,1001*ms,1001*ms,1,false,false),PrerollState::WAIT,"CAPTURE_OBSERVING");
    expect(gate.evaluate(1201*ms,1001*ms,1201*ms,3,false,false),PrerollState::WAIT,"CAPTURE_OBSERVING");
    expect(gate.evaluate(1301*ms,1001*ms,1201*ms,3,false,false),PrerollState::READY,"CAPTURE_READY");
    expect(gate.evaluate(1302*ms,1001*ms,1201*ms,3,false,false),PrerollState::FAILED,"CAPTURE_STALE");
    expect(gate.evaluate(1303*ms,1001*ms,1303*ms,4,false,false),PrerollState::FAILED,"CAPTURE_STALE");
    CounterpulsePrerollGate timeout(ms);
    expect(timeout.evaluate(5001*ms,0,0,0,false,false),PrerollState::FAILED,"CAPTURE_FIRST_FRAME_TIMEOUT");
    expect(timeout.evaluate(5002*ms,5002*ms,5002*ms,1,false,false),PrerollState::FAILED,"CAPTURE_FIRST_FRAME_TIMEOUT");
    CounterpulsePrerollGate boundary(ms);
    expect(boundary.evaluate(5001*ms,5001*ms,5001*ms,1,false,false),PrerollState::WAIT,"CAPTURE_OBSERVING");
    expect(boundary.evaluate(5301*ms,5001*ms,5301*ms,3,false,false),PrerollState::READY,"CAPTURE_READY");
    CounterpulsePrerollGate late(ms);
    expect(late.evaluate(5002*ms,5002*ms,5002*ms,1,false,false),PrerollState::FAILED,"CAPTURE_FIRST_FRAME_TIMEOUT");
    CounterpulsePrerollGate stale(ms);
    expect(stale.evaluate(102*ms,ms,ms,1,false,false),PrerollState::FAILED,"CAPTURE_STALE");
    CounterpulsePrerollGate count(ms);
    expect(count.evaluate(301*ms,ms,301*ms,2,false,false),PrerollState::FAILED,"CAPTURE_INSUFFICIENT_FRAMES");
    CounterpulsePrerollGate terminal(ms);
    expect(terminal.evaluate(ms,0,0,0,true,false),PrerollState::FAILED,"CAPTURE_FAILED");
    expect(terminal.evaluate(ms,0,0,0,true,true),PrerollState::FAILED,"USER_CANCELLED");
    CounterpulsePrerollGate clock(ms);
    expect(clock.evaluate(0,0,0,0,false,false),PrerollState::FAILED,"CAPTURE_CLOCK_REGRESSION");
    CounterpulsePrerollGate future(ms);
    expect(future.evaluate(ms,2*ms,2*ms,1,false,false),PrerollState::FAILED,"CAPTURE_TIMING_INVALID");
    CounterpulsePrerollGate regression(ms);
    regression.evaluate(100*ms,ms,100*ms,2,false,false);
    expect(regression.evaluate(101*ms,2*ms,101*ms,3,false,false),PrerollState::FAILED,"CAPTURE_EVIDENCE_REGRESSION");
}
}
int main() {
    try { matrix(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "预采集纯状态专项通过：异步首帧、有界观察、帧龄、失败锁存及取消\n";
}
