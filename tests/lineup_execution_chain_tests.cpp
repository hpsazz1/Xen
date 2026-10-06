#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "lineup/control_ipc.h"
#include "runtime/lineup_bridge_internal.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using namespace lineup;
using namespace lineup::detail;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> bool until(F f) {
    const auto end = Clock::now() + 3s;
    while (Clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(5ms); }
    return false;
}
// 仅 Session 回调可写 fake 日志；不创建或打开任何设备。
struct Event { std::string control; bool held; bool same_session; };
class FakeMouse final : public IMouseController {
public:
    Clock::time_point now = Clock::now();
    bool same_session = false;
    int moves = 0, opens = 0;
    std::vector<Event> events;
    bool open() noexcept override { ++opens; return false; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_left_button() const noexcept override { return true; }
    bool supports_lineup_inputs() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves; events.push_back({"move",false,same_session});
        MouseMoveReceipt r; r.succeeded=true; r.protocol_ack_received=true; r.backend_completed_at=now; return r;
    }
    ButtonReceipt button(const char* name, bool held) noexcept {
        events.push_back({name,held,same_session}); ButtonReceipt r;
        r.disposition=ButtonDisposition::ACKNOWLEDGED; r.datagram_sent=true; r.backend_completed_at=now; return r;
    }
    ButtonReceipt set_left_button(bool held) noexcept override { return button("left",held); }
    ButtonReceipt set_right_button(bool held) noexcept override { return button("right",held); }
    KeyboardReceipt key(const char* name,bool held) noexcept {
        events.push_back({name,held,same_session}); KeyboardReceipt r;
        r.disposition=KeyboardDisposition::ACKNOWLEDGED; r.backend_completed_at=now; return r;
    }
    KeyboardReceipt set_space_key(bool held) noexcept override { return key("Space",held); }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override { return key("WASD",mask!=0); }
};
struct Fixture {
    control::Server server;
    control::Client client;
    std::shared_ptr<FakeMouse> mouse = std::make_shared<FakeMouse>();
    std::shared_ptr<AutoStopOutputArbiter> arbiter = std::make_shared<AutoStopOutputArbiter>();
    ExecutionPermit permit{true,true,true,true,true};
    bool paused=false;
    int faults=0;
    std::unique_ptr<runtime::detail::LineupExecutionSession> session;
    std::uint64_t epoch=0, locate_sequence=0, sequence=0;
    std::string key;
    control::Request request;
    Fixture() {
        static unsigned serial=0;
        key="fake-chain-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(++serial);
        permit.input_state_valid=true;
        session=std::make_unique<runtime::detail::LineupExecutionSession>(mouse,arbiter,[&]{return permit;},
            [&](bool v){paused=v;},[]{return true;},[&]{++faults;},ExecutionLimits{12,10,2000ms,100ms,2,6});
        check(server.start(key)&&client.start(key),"start actual IPC");
        check(until([&]{return client.connected();}),"actual IPC connect");
        request.reference_version=1;
        request.observation.identity={"recipe","reference","synthetic-source","source-clock-fixture",2,1,
            {320,320,1920,1080,320,320,800,380,1,1,true}};
        // 第一阶段双键，第二阶段保持双键并按 Space，最后全释放。
        request.throw_action={{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
            {{"buttons",{"left","right"}},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",40}},
            {{"buttons",{"left","right"}},{"movement",nlohmann::json::array()},{"jump",true},{"duration_ms",40}},
            {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",0}}
        })}};
    }
    ~Fixture() {
        mouse->same_session=true; mouse->now=Clock::now(); session->cancel(mouse->now); session.reset();
        mouse->same_session=false; client.stop(); server.stop();
    }
    template<class F> auto same_session(F f, Clock::time_point at=Clock::now()) {
        struct Guard { FakeMouse& mouse; ~Guard(){mouse.same_session=false;} } guard{*mouse};
        mouse->now=at; mouse->same_session=true; return f();
    }
    void publish(control::Mode mode,double error=0) {
        request.mode=mode;
        auto& o=request.observation; o.sequence=++sequence; o.valid=true;
        o.captured_at=Clock::now()-1ms; o.source_at=o.captured_at; o.source_uncertainty=0ms;
        o.error_x=error; o.error_y=0;
        check(client.publish(request),"IPC publish");
        check(until([&]{const auto s=server.snapshot();return s.available&&s.request.observation.sequence==sequence;}),"IPC observation accepted");
    }
    // 与 Runtime 同样只由新的 locate_sequence 建立会话；无可用消息立即取消。
    // 此测试不覆盖 Runtime 自身 source-context / 捕获几何校验器。
    void route(bool throw_pressed=false) {
        auto received=server.snapshot(); const auto now=Clock::now();
        same_session([&]{
            if(!received.connected||!received.available||received.valid_until<=now||received.request.mode==control::Mode::CANCEL) {
                session->cancel(now); return;
            }
            if(epoch!=received.connection_epoch) {session->cancel(now);epoch=received.connection_epoch;locate_sequence=0;}
            const auto& o=received.request.observation;
            if(received.locate_sequence!=locate_sequence) {
                session->cancel(now);locate_sequence=received.locate_sequence;
                ExecutionCalibration calibration{"synthetic-chain-only",o.identity.source_id,o.identity.geometry,true,.5,.5,1ms};
                check(session->locate(o,calibration,now),"IPC locate enters actual bridge");
            } else if(session->active()) {
                session->observe(o,now);
                if(throw_pressed) {
                    ExecutionThrowPlan plan;plan.id="fixture-plan";plan.identity=o.identity;plan.action=received.request.throw_action;
                    const auto design=inspect_action(plan.action);plan.validated=design.valid&&design.configured&&design.timing_complete;
                    session->throw_edge(o,plan,now);
                }
                session->tick(now);
            }
        },now);
    }
    void align() {
        publish(control::Mode::LOCATE,8);route();route();
        check(mouse->moves==1,"fresh IPC error produces one bounded move");
        route();check(mouse->moves==1,"same frame never repeats move");
        for(int i=0;i<3;++i) {
            std::this_thread::sleep_for(15ms);publish(control::Mode::OBSERVATION);route();
        }
        check(session->snapshot().state==ExecutionState::ALIGNED,"three fresh IPC frames establish alignment");
    }
    Clock::time_point begin_throw() {
        route(true);check(session->snapshot().state==ExecutionState::HOLDING,"local throw edge starts bound IPC action");
        check(mouse->events.size()==3,"move then exactly two mouse downs before Space phase");
        auto lock=arbiter->try_enter_aim();check(!lock.owns_lock(),"same arbiter excludes competing aim");
        return mouse->now;
    }
    void assert_outputs() {
        check(mouse->opens==0&&faults==0,"no device opened and no fake output faults");
        for(const auto& event:mouse->events) check(event.same_session,"all fake outputs called by same Session scope");
        check(session->snapshot().owned_controls.empty()&&!paused,"owned controls returned and arbiter released");
        auto lock=arbiter->try_enter_aim();check(lock.owns_lock(),"aim can resume after cleanup");
    }
};
void success() {
    Fixture f;f.align();const auto start=f.begin_throw();
    f.route(true);check(f.mouse->events.size()==3,"duplicate throw edge has no outputs");
    f.same_session([&]{f.session->tick(start+40ms);},start+40ms);
    check(f.mouse->events.size()==4&&f.mouse->events.back().control=="Space"&&f.mouse->events.back().held,"Space pressed in second phase");
    f.same_session([&]{f.session->tick(start+80ms);},start+80ms);
    check(f.session->snapshot().state==ExecutionState::COMPLETED,"combined phases complete");
    check(f.mouse->events.size()==7,"exactly three owned controls released once");
    for(const char* control:{"left","right","Space"}) {
        int downs=0,ups=0;for(const auto& e:f.mouse->events)if(e.control==control)(e.held?downs:ups)++;
        check(downs==1&&ups==1,"each control has one Down and one Up");
    }
    f.assert_outputs();
    f.server.publish_status({{"chain_state","completed"},{"owned_count",0}});
    check(until([&]{return f.client.status().value("chain_state","")=="completed"&&f.client.status().value("owned_count",-1)==0;}),"actual reverse IPC reports completion");
    f.route(true);check(f.mouse->events.size()==7,"completed action cannot replay");
}
enum class Stop { CANCEL, EXPIRE, SESSION_CHANGE, DISCONNECT };
void interrupted(Stop stop) {
    Fixture f;f.align();f.begin_throw();
    const auto previous_epoch=f.server.snapshot().connection_epoch;
    if(stop==Stop::CANCEL) {control::Request cancel;check(f.client.publish(cancel),"publish cancel");}
    if(stop==Stop::SESSION_CHANGE) {
        f.request.observation.identity.session_id="source-clock-replaced";
        f.request.observation.sequence=++f.sequence;
        check(f.client.publish(f.request),"publish changed source session");
    }
    if(stop==Stop::DISCONNECT) f.client.stop();
    check(until([&]{return !f.server.snapshot().available;}),"cancel/stale/source change/disconnect revokes IPC");
    f.route();
    check(f.session->snapshot().state==ExecutionState::CANCELLED,"revoked IPC cancels actual bridge");
    check(f.mouse->events.size()==5,"only two owned mouse ups; later Space never pressed");
    check(f.session->snapshot().release_may_throw,"cleanup release may throw is preserved");
    f.assert_outputs();
    if(stop==Stop::DISCONNECT) {
        check(f.client.start(f.key),"restart client");
        check(until([&]{return f.client.connected()&&f.server.snapshot().connection_epoch!=previous_epoch;}),"reconnect new epoch");
    }
    f.route(true);check(f.mouse->events.size()==5,"revocation/reconnect cannot replay throw edge");
    f.request.mode=control::Mode::OBSERVATION;f.request.observation.sequence=++f.sequence;
    f.request.observation.captured_at=Clock::now();f.request.observation.source_at=Clock::now();
    check(f.client.publish(f.request),"post-revocation observation publish");
    std::this_thread::sleep_for(40ms);f.route(true);
    check(!f.server.snapshot().available&&f.mouse->events.size()==5,"observation alone cannot revive cancelled selection");
}
}
int main() {
    try {success();interrupted(Stop::CANCEL);interrupted(Stop::EXPIRE);interrupted(Stop::SESSION_CHANGE);interrupted(Stop::DISCONNECT);
        std::cout<<"lineup_execution_chain_tests passed: actual IPC + Session + fake same-session mouse; no devices\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
