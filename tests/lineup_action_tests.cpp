#include "lineup/action_internal.h"
#include <iostream>
using Json=nlohmann::json;
void check(bool value,const char *why) { if(!value) throw std::runtime_error(why); }
int main() {
 try {
    using lineup::detail::inspect_action;
    Json release={{"buttons",Json::array()},{"movement",Json::array()},{"jump",false},{"duration_ms",0}};
    auto hold=release; hold["buttons"]={"left"}; hold["duration_ms"]=40;
    Json action={{"schema",1},{"type","phases"},{"phases",Json::array({hold,release})}};
    check(inspect_action(nullptr).valid && !inspect_action(nullptr).configured,"legacy has no executable action");
    check(inspect_action(action).left_hold_release && inspect_action(action).hold_ms==40,"explicit mock duration");
    for (auto buttons : {Json::array({"right"}),Json::array({"left","right"})}) {
        auto other=action; other["phases"][0]["buttons"]=buttons;
        check(inspect_action(other).valid && !inspect_action(other).left_hold_release,"right and dual never downgraded");
    }
    for (auto direction : {"forward","back","left","right"}) {
        auto other=action; other["phases"][0]["movement"]={direction}; other["phases"][0]["jump"]=true;
        check(inspect_action(other).valid && !inspect_action(other).left_hold_release,"direction jump design only");
    }
    auto incomplete=action; incomplete["phases"][0]["duration_ms"]=nullptr;
    check(inspect_action(incomplete).valid && !inspect_action(incomplete).timing_complete && !inspect_action(incomplete).left_hold_release,"no invented timing");
    auto staged=action; staged["phases"][0]["buttons"]={"left","right"}; staged["phases"].insert(staged["phases"].begin()+1,hold);
    check(inspect_action(staged).valid && !inspect_action(staged).left_hold_release,"staged release design");
    for(auto duration : {Json(-1),Json(2001),Json(1.5),Json("40"),Json(true)}) { auto bad=action; bad["phases"][0]["duration_ms"]=duration; check(!inspect_action(bad).valid,"bad duration"); }
    auto bad=action; bad["phases"][1]["buttons"]={"left"}; check(!inspect_action(bad).valid,"terminal release required");
    bad=action; bad["phases"][0]["buttons"]={"left","left"}; check(!inspect_action(bad).valid,"duplicate button rejected");
    bad=action; bad["phases"][0]["movement"]={"left","right"}; check(!inspect_action(bad).valid,"opposed movement rejected");
    bad=action; bad["schema"]=2; check(!inspect_action(bad).valid,"schema version rejected");
    // 十二种设计矩阵；固定毫秒仅是软件fixture，不是实战跳投时序。
    int matrix=0;
    for(auto direction : {"", "forward", "left", "right"}) for(auto buttons : {Json::array({"left"}),Json::array({"right"}),Json::array({"left","right"})}) {
        auto jump_action=action; auto &phase=jump_action["phases"][0]; phase["buttons"]=buttons;
        phase["movement"]=std::string(direction).empty()?Json::array():Json::array({direction});phase["jump"]=true;
        jump_action["game_version"]="";jump_action["movement_profile"]=std::string(direction).empty()?"stationary":"runup";
        check(inspect_action(jump_action).valid && !inspect_action(jump_action).left_hold_release,"12 design matrix not executable on left executor");
        auto trace=lineup::detail::dry_run_action(jump_action,{true,true,true,true});
        check(trace.supported && !trace.events.empty(),"all-capability virtual dryrun");
        std::set<std::string> held;
        for(const auto &event:trace.events) {auto key=event["control"].get<std::string>(); if(event["held"].get<bool>())held.insert(key);else held.erase(key);}
        check(held.empty(),"matrix final release complete");
        auto rejected=lineup::detail::dry_run_action(jump_action,{true,false,false,false});
        check(!rejected.supported && rejected.events.empty(),"unsupported plan rejected before first output");++matrix;
    }
    check(matrix==12,"12 direction-strength designs");
    auto transitions=lineup::detail::dry_run_action(staged,{true,true,false,false});
    check(transitions.supported,"staged release trace");
    bool right_released=false,left_still_held=false;
    for(const auto &event:transitions.events) {if(event["at_ms"]==40 && event["control"]=="button:right" && event["held"]==false)right_released=true; if(event["at_ms"]==40 && event["control"]=="button:left" && event["held"]==false)left_still_held=true;}
    check(right_released && !left_still_held,"dual to left releases only right at phase boundary");
    check(!lineup::detail::dry_run_action(incomplete,{true,true,true,true}).supported,"unknown relative timing not zero");
    for(auto field:{"buttons","movement"}){bad=action;bad["phases"][0][field]={"invalid_mapping"};check(!inspect_action(bad).valid && lineup::detail::dry_run_action(bad,{true,true,true,true}).events.empty(),"illegal mapping rejected");}
    bad=action;bad["game_version"]=42;check(!inspect_action(bad).valid,"invalid game version");
    auto status=lineup::detail::action_status(action); check(status["simulation"]["supported"]==true && status["hardware"]["supported"]==false && status["manual"]["status"]=="unverified","evidence layers distinct");
    auto production=lineup::detail::action_status(staged,lineup::detail::ActionCapabilities{true,true,false,false});
    check(production["hardware"]["available"]==true && production["hardware"]["supported"]==true && production["manual"]["status"]=="unverified","backend capability not real verification");
    auto limited=lineup::detail::action_status(staged,lineup::detail::ActionCapabilities{true,false,false,false});
    check(limited["simulation"]["supported"]==true && limited["hardware"]["supported"]==false,"simulation not backend support");
    std::cout<<"action schema, incomplete timing, capability and releases passed\n"; return 0;
 } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
