#include "lineup/execution_internal.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace lineup;
using namespace lineup::detail;
using namespace std::chrono_literals;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    Clock::time_point now = Clock::time_point{} + 10s;
    ExecutionObservation observation;
    ExecutionCalibration calibration;
    ExecutionPermit permit{true, true, true, true, true};
    ExecutionLimits limits{4, 5, 1000ms, 100ms, 1.0, .5};
    std::vector<std::pair<int, int>> moves;
    std::vector<bool> buttons;
    ExecutionDelivery move_result = ExecutionDelivery::ACCEPTED, down_result = ExecutionDelivery::ACCEPTED, up_result = ExecutionDelivery::ACCEPTED;
    Fixture() {
        permit.input_state_valid = true;
        observation.identity = {"recipe", "reference", "source", "session", 3, 7,
            {320, 320, 2560, 1440, 320, 320, 1120, 560, 1, 1, true}};
        observation.sequence = 1; observation.captured_at = now; observation.source_at = now; observation.source_uncertainty = 0ms;
        observation.error_x = 8; observation.error_y = -4; observation.valid = true;
        calibration = {"measured-fixture-only", "source", observation.identity.geometry, true, .5, .5, 20ms};
    }
    ExecutionSink sink() {
        return {[this](int x, int y) { moves.emplace_back(x, y); return ExecutionReceipt{move_result, now}; },
                [this](bool down) { buttons.push_back(down); return ExecutionReceipt{down ? down_result : up_result, now}; }};
    }
    ExecutionController controller() { return {sink(), {true, true}, limits}; }
    void next(double x, double y, std::chrono::milliseconds advance = 30ms) {
        now += advance; ++observation.sequence; observation.captured_at = now; observation.source_at = now;
        observation.error_x = x; observation.error_y = y;
    }
    void align(ExecutionController &core) {
        observation.error_x = observation.error_y = 0;
        check(core.locate_edge(observation, calibration, permit, now), "mock start");
        for (int i = 0; i < 3; ++i) { if (i) next(0, 0); core.observe(observation, permit, now); }
        check(core.snapshot().state == ExecutionState::ALIGNED, "three fresh aligned frames");
        check(moves.empty(), "already aligned no moves");
    }
};
int main() {
    try {
        {
            Fixture f; auto core = f.controller(); f.calibration.validated = false;
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now) && f.moves.empty(), "unvalidated zero output");
            f.calibration.validated = true; f.calibration.source_id = "different";
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "source calibration mismatch");
            f.calibration.source_id = "source"; f.calibration.geometry.roi_x += 1;
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "geometry calibration mismatch");
            f.calibration.geometry = f.observation.identity.geometry; f.observation.source_at.reset();
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "unknown source timing no output");
            check(!core.snapshot().production_available, "mock core not production available");
        }
        {
            Fixture f; auto core = f.controller();
            check(core.locate_edge(f.observation, f.calibration, f.permit, f.now), "calibrated mock start");
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "duplicate locate edge does not reset session");
            core.observe(f.observation, f.permit, f.now);
            check(f.moves.size() == 1 && f.moves[0] == std::pair<int,int>{4,-2}, "bounded signed calibrated step");
            core.observe(f.observation, f.permit, f.now);
            check(f.moves.size() == 1, "same frame cannot repeat command");
            f.next(7, -3, 10ms); core.observe(f.observation, f.permit, f.now);
            check(f.moves.size() == 1 && core.snapshot().waiting_feedback, "pre-effect frame cannot schedule");
            f.next(4, -2, 11ms); core.observe(f.observation, f.permit, f.now);
            check(f.moves.size() == 2, "post completion plus delay frame may step");
            f.next(0, 0); core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state != ExecutionState::ALIGNED, "one observed aligned frame insufficient");
            f.next(0, 0); core.observe(f.observation, f.permit, f.now);
            f.next(0, 0); core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state == ExecutionState::ALIGNED, "feedback confirms alignment");
        }
        {
            Fixture f; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            core.observe(f.observation, f.permit, f.now); f.next(20, -4); core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state == ExecutionState::CANCELLED && f.moves.size() == 1, "divergence cancels");
        }
        {
            Fixture f; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            ++f.observation.identity.recipe_version; core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state == ExecutionState::CANCELLED && f.moves.empty(), "recipe revision invalidates session");
        }
        {
            Fixture f; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            f.permit.exclusive = false; core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state == ExecutionState::CANCELLED && f.moves.empty(), "permit revoked no output");
        }
        {
            Fixture f; f.limits.max_steps = 1; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            core.observe(f.observation, f.permit, f.now); f.next(4, -2); core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().reason == "step_limit" && f.moves.size() == 1, "finite step budget");
        }
        {
            Fixture f; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            core.tick(f.permit, f.now + 1001ms);
            check(core.snapshot().reason == "execution_timeout" && f.moves.empty(), "finite time budget");
        }
        {
            Fixture f; f.move_result = ExecutionDelivery::UNKNOWN; auto core = f.controller();
            core.locate_edge(f.observation, f.calibration, f.permit, f.now); core.observe(f.observation, f.permit, f.now);
            f.next(8, -4); core.observe(f.observation, f.permit, f.now);
            check(core.snapshot().state == ExecutionState::UNKNOWN && f.moves.size() == 1, "unknown move never retried");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core);
            ExecutionThrowPlan plan{"action-v1", "right_hold_release", true, 20, f.observation.identity};
            check(!core.throw_edge(f.observation, f.permit, plan, f.now) && f.buttons.empty(), "unsupported combination not downgraded");
            plan.mode = "left_hold_release"; plan.hold_ms = 2001;
            check(!core.throw_edge(f.observation, f.permit, plan, f.now) && f.buttons.empty(), "hold duration upper bound");
            plan.hold_ms = 20;
            check(core.throw_edge(f.observation, f.permit, plan, f.now), "explicit throw edge");
            check(!core.throw_edge(f.observation, f.permit, plan, f.now), "throw edge one shot");
            f.now += 19ms; core.tick(f.permit, f.now); check(f.buttons == std::vector<bool>{true}, "hold maintained until deadline");
            f.now += 1ms; core.tick(f.permit, f.now);
            check(f.buttons == std::vector<bool>({true, false}) && core.snapshot().state == ExecutionState::COMPLETED &&
                  core.snapshot().release_may_throw && !core.snapshot().cleanup_required, "single bounded release not game effect proof");
            core.tick(f.permit, f.now + 50ms); check(f.buttons.size() == 2, "no duplicate release");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core); f.down_result = ExecutionDelivery::UNKNOWN;
            check(!core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now), "unknown down not success");
            check(f.buttons == std::vector<bool>({true, false}) && core.snapshot().state == ExecutionState::UNKNOWN &&
                  core.snapshot().release_may_throw, "unknown down cleanup up only");
            core.tick(f.permit, f.now + 50ms); check(f.buttons.size() == 2, "unknown down no retry");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core);
            core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now);
            f.permit.armed = false; f.up_result = ExecutionDelivery::UNKNOWN; core.tick(f.permit, f.now);
            check(f.buttons == std::vector<bool>({true, false}) && core.snapshot().state == ExecutionState::UNKNOWN &&
                  core.snapshot().cleanup_required && core.snapshot().release_may_throw, "cancel release debt explicit");
            core.cancel(f.now);
            check(core.snapshot().state == ExecutionState::UNKNOWN && core.snapshot().cleanup_required, "cancel preserves unknown cleanup fact");
            f.permit.armed = true;
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "cleanup debt prevents new session");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core); f.now += 101ms;
            check(!core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now) && f.buttons.empty(), "stale alignment never throws");
        }
        {
            Fixture f; auto core = f.controller(); f.calibration.observation_delay.reset();
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now) && f.moves.empty(), "unknown delay is not zero");
            f.calibration.observation_delay = 20ms; f.observation.source_uncertainty.reset();
            check(!core.locate_edge(f.observation, f.calibration, f.permit, f.now), "unknown timing uncertainty rejected");
        }
        {
            Fixture f; ExecutionController core(f.sink(), {true, false}, f.limits); f.align(core);
            check(!core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now) && f.buttons.empty(),
                  "missing release capability rejects entire action before down");
        }
        {
            Fixture f; auto core = f.controller(); core.locate_edge(f.observation, f.calibration, f.permit, f.now);
            core.cancel(f.now); core.observe(f.observation, f.permit, f.now);
            check(f.moves.empty() && core.snapshot().state == ExecutionState::CANCELLED, "cancel before step");
            check(core.locate_edge(f.observation, f.calibration, f.permit, f.now), "explicit new edge after cancel");
            core.observe(f.observation, f.permit, f.now); core.cancel(f.now);
            f.next(0, 0); core.observe(f.observation, f.permit, f.now);
            check(f.moves.size() == 1 && !core.snapshot().waiting_feedback, "cancel while awaiting feedback");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core); core.cancel(f.now);
            check(!core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now) && f.buttons.empty(), "cancel aligned prevents throw");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core);
            core.throw_edge(f.observation, f.permit, {"action", "left_hold_release", true, 20, f.observation.identity}, f.now);
            core.cancel(f.now);
            check(core.snapshot().state == ExecutionState::CANCELLED && core.snapshot().release_may_throw &&
                  f.buttons == std::vector<bool>({true, false}), "cancel held left may throw upon release");
        }
        {
            Fixture f; auto core = f.controller(); f.align(core);
            ExecutionThrowPlan wrong{"action", "left_hold_release", true, 20, f.observation.identity};
            wrong.identity.reference_id = "another-reference";
            check(!core.throw_edge(f.observation, f.permit, wrong, f.now) && f.buttons.empty(), "foreign validated action identity rejected");
            wrong.identity = f.observation.identity; ++wrong.identity.recipe_version;
            check(!core.throw_edge(f.observation, f.permit, wrong, f.now) && f.buttons.empty(), "new action version not silently substituted");
        }
        {
            // 三种鼠标模式 x 静止/前/后/侧四类跳投，全部走真实核心调度与fake Sink。
            for (const auto buttons : {nlohmann::json::array({"left"}), nlohmann::json::array({"right"}), nlohmann::json::array({"left", "right"})}) {
                for (const auto direction : {"", "forward", "back", "left"}) {
                    Fixture f; std::vector<std::pair<std::string,bool>> events;
                    auto sink = f.sink(); sink.control = [&](const std::string &name, bool down) { events.emplace_back(name, down); return ExecutionReceipt{ExecutionDelivery::ACCEPTED, f.now}; };
                    ExecutionController core(sink, {true,true,true,true,true}, f.limits); f.align(core);
                    auto movement = direction[0] ? nlohmann::json::array({direction}) : nlohmann::json::array();
                    auto phase = [&](nlohmann::json held, nlohmann::json move, bool jump, int ms) { return nlohmann::json{{"buttons",held},{"movement",move},{"jump",jump},{"duration_ms",ms}}; };
                    nlohmann::json action = {{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
                        phase(buttons,movement,false,10), phase(buttons,movement,true,10),
                        phase(nlohmann::json::array(),movement,true,10), phase(nlohmann::json::array(),nlohmann::json::array(),false,0)})}};
                    ExecutionThrowPlan plan{"twelve-combinations", "phases", true, 0, f.observation.identity, action};
                    check(core.throw_edge(f.observation,f.permit,plan,f.now), "combo starts");
                    check(!core.throw_edge(f.observation,f.permit,plan,f.now), "combo duplicate edge rejected");
                    for (int i=0;i<3;++i) { f.now += 10ms; core.tick(f.permit,f.now); }
                    check(core.snapshot().state == ExecutionState::COMPLETED && core.snapshot().owned_controls.empty(), "combo completes releases all owned");
                    const auto design_events = dry_run_action(action,{true,true,true,true}).events;
                    check(events.size() == design_events.size(), "combo event count");
                    for (std::size_t i=0;i<events.size();++i) check(events[i].first == design_events[i]["control"].get<std::string>() && events[i].second == design_events[i]["held"].get<bool>(), "combo exact ordered events");
                    check(std::count(events.begin(),events.end(),std::pair<std::string,bool>{"jump",true}) == 1, "jump is one Space control edge");
                }
            }
        }
        {
            // 人工右键会改变左键配方，人工移动会改变静止配方；整条拒绝且不释放人工输入。
            for (const auto held : {"button:left", "button:right", "jump", "movement:forward", "movement:back", "movement:left", "movement:right"}) {
                Fixture held_fixture; auto held_core=held_fixture.controller(); held_fixture.align(held_core);
                held_fixture.permit.user_held.insert(held);
                check(!held_core.throw_edge(held_fixture.observation,held_fixture.permit,
                    {"stationary-left", "left_hold_release",true,20,held_fixture.observation.identity},held_fixture.now) &&
                    held_fixture.buttons.empty(), "any physical lineup input rejects stationary left plan without output");
            }
            Fixture f; auto core=f.controller(); f.align(core); f.permit.user_held.insert("button:left");
            check(!core.throw_edge(f.observation,f.permit,{"held", "left_hold_release",true,20,f.observation.identity},f.now) && f.buttons.empty(), "user held control never touched");
            f.permit.user_held.clear(); f.limits.max_duration=70ms;
            ExecutionController budget(f.sink(),{true,true},f.limits); f.align(budget);
            check(!budget.throw_edge(f.observation,f.permit,{"budget", "left_hold_release",true,20,f.observation.identity},f.now) && f.buttons.empty(), "insufficient remaining session budget rejects before down");
        }
        {
            Fixture f; std::vector<std::pair<std::string,bool>> calls;
            auto sink=f.sink(); sink.control=[&](const std::string &name,bool down) { calls.emplace_back(name,down); return ExecutionReceipt{ExecutionDelivery::ACCEPTED,f.now}; };
            const auto phase=[](bool left,bool jump,int ms) { return nlohmann::json{{"buttons",left?nlohmann::json::array({"left"}):nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",jump},{"duration_ms",ms}}; };
            nlohmann::json action={{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({phase(true,false,10),phase(true,true,10),phase(false,false,0)})}};
            ExecutionController missing(sink,{true,true,false,false,false},f.limits); f.align(missing);
            check(!missing.throw_edge(f.observation,f.permit,{"capability","phases",true,0,f.observation.identity,action},f.now) && calls.empty(), "later unsupported Space rejects entire sequence");
            for(int cancel_stage=0;cancel_stage<2;++cancel_stage) {
                calls.clear(); ExecutionController core(sink,{true,true,false,false,true},f.limits); f.align(core);
                check(core.throw_edge(f.observation,f.permit,{"cancel-stage","phases",true,0,f.observation.identity,action},f.now), "stage setup");
                if(cancel_stage) { f.now+=10ms; core.tick(f.permit,f.now); }
                f.permit.focused=false; core.tick(f.permit,f.now); f.permit.focused=true;
                auto count=calls.size(); core.tick(f.permit,f.now+100ms);
                check(core.snapshot().state==ExecutionState::CANCELLED && core.snapshot().owned_controls.empty() && calls.size()==count, "phase abort cleans owned and stops future edges");
            }
            calls.clear(); ExecutionController user_takeover(sink,{true,true,false,false,true},f.limits); f.align(user_takeover);
            user_takeover.throw_edge(f.observation,f.permit,{"takeover","phases",true,0,f.observation.identity,action},f.now);
            f.permit.user_held.insert("jump"); f.now+=10ms; user_takeover.tick(f.permit,f.now);
            check(user_takeover.snapshot().state==ExecutionState::CANCELLED && calls.size()==2 &&
                  calls[0]==std::pair<std::string,bool>{"button:left",true} && calls[1]==std::pair<std::string,bool>{"button:left",false},
                  "manual Space before later phase prevents SpaceDown and releases only software-owned left");
            f.permit.user_held.clear();
            for (const auto held : {"button:right", "movement:forward"}) {
                calls.clear(); ExecutionController external(sink,{true,true,false,false,true},f.limits); f.align(external);
                check(external.throw_edge(f.observation,f.permit,{"external-takeover","phases",true,0,f.observation.identity,action},f.now), "external takeover setup");
                f.permit.user_held.insert(held); f.now+=10ms; external.tick(f.permit,f.now);
                check(external.snapshot().state==ExecutionState::CANCELLED && calls.size()==2 &&
                      calls[0]==std::pair<std::string,bool>{"button:left",true} && calls[1]==std::pair<std::string,bool>{"button:left",false},
                      "unrequested physical right or W aborts later Space and releases software left only");
                f.permit.user_held.clear();
            }
            calls.clear(); ExecutionController late(sink,{true,true,false,false,true},f.limits); f.align(late);
            late.throw_edge(f.observation,f.permit,{"late","phases",true,0,f.observation.identity,action},f.now);
            f.now+=25ms; late.tick(f.permit,f.now);
            check(late.snapshot().state==ExecutionState::CANCELLED && calls.size()==2 && calls.back()==std::pair<std::string,bool>{"button:left",false}, "missed phases cleaned not burst replayed");
            calls.clear(); ExecutionController unknown(sink,{true,true,false,false,true},f.limits); f.align(unknown);
            action["phases"][1]["duration_ms"]=nullptr;
            check(!unknown.throw_edge(f.observation,f.permit,{"unknown-duration","phases",true,0,f.observation.identity,action},f.now) && calls.empty(), "unknown phase duration not zero");
        }
        {
            Fixture f; std::vector<std::pair<std::string,bool>> calls;
            auto sink=f.sink(); sink.control=[&](const std::string &name,bool down) { calls.emplace_back(name,down); if(down) f.now+=30ms; return ExecutionReceipt{ExecutionDelivery::ACCEPTED,f.now}; };
            ExecutionController core(sink,{true,true,true,false,false},f.limits); f.align(core);
            nlohmann::json action={{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
                {{"buttons",{"left","right"}},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",10}},
                {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",0}}})}};
            check(!core.throw_edge(f.observation,f.permit,{"slow-backend","phases",true,0,f.observation.identity,action},f.now), "slow synchronous backend aborts missed phase");
            check(calls.size()==2 && calls.front()==std::pair<std::string,bool>{"button:left",true} && calls.back()==std::pair<std::string,bool>{"button:left",false},
                  "slow backend never sends second stale Down");
        }
        {
            Fixture f; auto core=f.controller(); f.align(core);
            nlohmann::json action={{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
                {{"buttons",{"left"}},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",10}},
                {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",20}}})}};
            core.throw_edge(f.observation,f.permit,{"final-dwell","phases",true,0,f.observation.identity,action},f.now);
            f.now+=10ms; core.tick(f.permit,f.now);
            check(core.snapshot().state==ExecutionState::HOLDING && core.snapshot().owned_controls.empty(), "explicit final released duration retained");
            f.now+=20ms; core.tick(f.permit,f.now);
            check(core.snapshot().state==ExecutionState::COMPLETED && f.buttons.size()==2, "final dwell adds no inputs");
        }
        std::cout << "lineup_execution_tests: mock_core only; no devices, no calibration claim\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
