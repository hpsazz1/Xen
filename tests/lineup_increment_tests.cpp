#include "lineup/service.h"
#include <iostream>
#include <fstream>
#include <thread>
using namespace lineup;
void check(bool v, const char *why) { if (!v) throw std::runtime_error(why); }
Json send(Service &s, Json c) {
    auto state=s.state(); c["epoch"]=state["epoch"]; c["revision"]=state["revision"];
    static int n=0; if (!c.contains("request_id")) c["request_id"]="increment-"+std::to_string(++n);
    auto r=s.command(c); check(r.status==200,"command failed"); return r.json;
}
Json wait(Service &s, bool preview=false) {
    for(int i=0;i<300;++i) { auto state=s.state();
        if (state["capture_status"]=="error") throw std::runtime_error(state["error"].get<std::string>());
        if (preview ? !state["preview"].empty() : (!state["busy"].get<bool>() && state["capture_status"]=="saved")) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } throw std::runtime_error("worker timeout");
}
CapturedFrame frame(int sequence) {
    CapturedFrame f; f.width=f.height=f.encoded_width=f.encoded_height=320;
    f.source_width=1920; f.source_height=1080; f.roi_x=800; f.roi_y=380;
    f.bgr=cv::Mat(320,320,CV_8UC3); cv::RNG r(42); r.fill(f.bgr,cv::RNG::UNIFORM,0,255);
    f.timing.sequence=sequence; f.timing.captured_at=Clock::now(); return f;
}
int main() {
    auto root=std::filesystem::temp_directory_path()/("xen-increment-test-"+std::to_string(Clock::now().time_since_epoch().count()));
    try {
        std::string id;
        {
            Service s(root,ReferenceMode::ROI,"shared-320-source");
            send(s,{{"action","start"}}); check(!s.state()["searching"].get<bool>(),"ROI does not claim library scan"); send(s,{{"action","mode"},{"value","capture"}});
            send(s,{{"action","capture"},{"map","fixture"},{"region","A"},{"instructions","人工靠墙"},{"standpoint_id","shared"}});
            s.submit(frame(1),"receiver-1"); auto state=wait(s); id=state["recipes"][0]["id"];
            check(state["recipes"][0]["frame_mode"]=="roi" && state["recipes"][0]["source_id"]=="shared-320-source","ROI catalog identity");
            send(s,{{"action","update"},{"items",Json::array({{{"id",id},{"name","局部"},{"target","A"},{"grenade","smoke"}}})}}); wait(s);
            send(s,{{"action","mode"},{"value","browse"}}); send(s,{{"action","lock"},{"id",id}});
            s.submit(frame(2),"receiver-1"); std::this_thread::sleep_for(std::chrono::milliseconds(80));
            check(s.state()["preview"].empty() && !s.state()["locating"].get<bool>(),"ROI requires explicit locate");
            send(s,{{"action","locate"}}); s.submit(frame(3),"receiver-1"); state=wait(s,true);
            check(state["preview"]["status"]=="valid" && state["preview"]["source_mapping_verified"]==false,"local-only valid ROI");
            auto request=s.control_request();
            check(request.mode==control::Mode::LOCATE && request.observation.identity.recipe_id==id && !request.observation.valid && !request.observation.source_at,"unknown mapping and source timing never become executable");
            check(s.control_request().mode==control::Mode::OBSERVATION,"locate control edge consumed once");
            send(s,{{"action","favorite"},{"id",id},{"enabled",true}});
            send(s,{{"action","practice_queue"},{"id",id},{"enabled",true}});
            Json record={{"action","practice_record"},{"id",id},{"outcome","success"},{"conditions","手动记录"},{"request_id","durable-attempt"}};
            send(s,record); state=send(s,record);
            check(state["practice"]["history_count"]==1 && state["locked_id"]==id,"history dedupe keeps lock");
            check(state["practice"]["history"][0]["recipe"]["recipe_version"]==2,"history revision identity");
            check(state["recipes"][0]["action_status"]["hardware"]["supported"]==false,"no production output claim");
            Json plan={{"schema",1},{"type","phases"},{"phases",Json::array({
                {{"buttons",Json::array({"left"})},{"movement",Json::array()},{"jump",false},{"duration_ms",nullptr}},
                {{"buttons",Json::array()},{"movement",Json::array()},{"jump",false},{"duration_ms",nullptr}}})}};
            send(s,{{"action","update"},{"items",Json::array({{{"id",id},{"throw_action",plan}}})}}); state=wait(s);
            check(state["recipes"][0]["throw_action"]==plan && state["recipes"][0]["recipe_version"]==3,"action design version persisted");
            check(state["practice"]["history"][0]["recipe"]["recipe_version"]==2,"action revision preserves historical identity");
            auto moved=frame(4); moved.roi_x=801; s.submit(moved,"receiver-1");
            check(s.state()["preview"].empty() && !s.state()["locating"].get<bool>(),"geometry change revokes locate");
            check(s.control_request().mode==control::Mode::CANCEL,"geometry change cancels runtime request");
            s.disconnect("fixture disconnect"); s.submit(frame(5),"receiver-2");
            check(s.state()["preview"].empty(),"reconnect never rearms locate"); s.close();
        }
        {
            Service s(root,ReferenceMode::ROI,"shared-320-source"); auto state=s.state();
            check(state["practice"]["history_count"]==1 && state["practice"]["favorites"].size()==1 && state["practice"]["queue"].size()==1,"persistent practice state");
            send(s,{{"action","practice_record"},{"id",id},{"outcome","success"},{"conditions","手动记录"},{"request_id","durable-attempt"}});
            check(state["recipes"][0].contains("throw_action") && state["recipes"][0]["recipe_version"]==3,"action design restart recovery");
            check(s.state()["practice"]["history_count"]==1,"restart persistent dedupe"); s.close();
        }
        { Service other(root,ReferenceMode::ROI,"different-source"); check(other.state()["recipes"][0]["compatible"]==false,"source change cannot reuse ROI"); }
        { Service full(root); check(full.state()["recipes"][0]["compatible"]==false,"full and ROI modes isolated"); }
        for (const auto mode : {ReferenceMode::ROI, ReferenceMode::FULL_FRAME}) {
            Service other(root,mode,"different-source");
            send(other,{{"action","start"}}); send(other,{{"action","mode"},{"value","capture"}});
            send(other,{{"action","capture"},{"map","fixture"},{"region","A"},{"standpoint_id","shared"}});
            auto f=frame(1);
            if (mode==ReferenceMode::FULL_FRAME) { f.source_width=f.source_height=320; f.roi_x=f.roi_y=0; }
            other.submit(f,"new-source"); auto state=wait(other);
            check(state["recipes"].back()["standpoint_reference_id"]==state["recipes"].back()["reference_id"],"mode or source cannot share standpoint");
        }
        std::cout<<"increment ROI trigger, geometry, source, practice persistence passed\n"; return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<" evidence="<<root<<'\n'; return 1; }
}
