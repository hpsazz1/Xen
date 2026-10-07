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
        const auto capture_root = root / "capture-form";
        const Json capture_plan = {{"schema", 1}, {"type", "phases"},
            {"movement_profile", "stationary"}, {"phases", Json::array({
                {{"buttons", Json::array({"left"})}, {"movement", Json::array()},
                 {"jump", true}, {"duration_ms", nullptr}},
                {{"buttons", Json::array()}, {"movement", Json::array()},
                 {"jump", false}, {"duration_ms", nullptr}}})}};
        const auto check_capture_recipe = [&](const Json &recipe) {
            check(recipe.at("map") == "de_mirage" && recipe.at("team") == "CT",
                  "采集表单地图和阵营不被当前上下文覆盖");
            check(recipe.at("throw_action") == capture_plan &&
                  recipe.at("throw_instructions") == "原地跳投，左键",
                  "采集时的投掷方式与说明完整保存");
        };
        {
            Service s(capture_root, ReferenceMode::ROI, "shared-320-source");
            send(s, {{"action", "start"}});
            send(s, {{"action", "mode"}, {"value", "capture"}});
            send(s, {{"action", "context"}, {"mode", "manual"},
                     {"map", "de_dust2"}, {"team", "T"}});
            send(s, {{"action", "capture"}, {"map", "de_mirage"}, {"team", "CT"},
                     {"throw_action", capture_plan}, {"throw_instructions", "原地跳投，左键"}});
            s.submit(frame(1), "capture-form-source");
            const auto state = wait(s);
            check_capture_recipe(state["recipes"][0]);
            check(state["context"]["map"] == "de_dust2" && state["context"]["team"] == "T",
                  "采集表单不修改当前游戏上下文");
            check(!state["recipes"][0]["action_status"]["design"]["timing_complete"].get<bool>() &&
                  s.control_request().mode == control::Mode::CANCEL,
                  "未填写时序的采集预设不产生执行请求");
            s.close();
        }
        {
            Json catalog;
            std::ifstream saved(capture_root / "catalog.json");
            saved >> catalog;
            saved.close();
            check_capture_recipe(catalog.at(0));
            Service s(capture_root, ReferenceMode::ROI, "shared-320-source");
            check_capture_recipe(s.state()["recipes"].at(0));
            send(s, {{"action", "start"}});
            send(s, {{"action", "mode"}, {"value", "capture"}});
            send(s, {{"action", "context"}, {"mode", "manual"},
                     {"map", "de_dust2"}, {"team", "T"}});
            int invalid_request = 0;
            auto invalid_plan = capture_plan;
            invalid_plan["schema"] = 2;
            for (auto fields : {Json{{"team", "SPECTATOR"}}, Json{{"team", 3}},
                                Json{{"throw_action", invalid_plan}}, Json{{"throw_action", 3}},
                                Json{{"throw_instructions", false}}, Json{{"map", 3}}}) {
                const auto before = s.state();
                fields["action"] = "capture";
                fields["epoch"] = before["epoch"];
                fields["revision"] = before["revision"];
                fields["request_id"] = "invalid-capture-" + std::to_string(++invalid_request);
                const auto rejected = s.command(fields);
                check(rejected.status == 400 && rejected.json["revision"] == before["revision"] &&
                      rejected.json["capture_status"] == before["capture_status"] &&
                      rejected.json["recipes"].size() == 1,
                      "无效采集字段同步拒绝且不进入采集队列");
            }
            send(s, {{"action", "capture"}});
            s.submit(frame(1), "capture-form-reload");
            auto state = wait(s);
            check(state["recipes"].size() == 2 && state["recipes"][1]["map"] == "de_dust2" &&
                  state["recipes"][1]["team"] == "T" &&
                  !state["recipes"][1].contains("throw_action") &&
                  state["recipes"][1]["throw_instructions"] == "",
                  "旧采集调用仍采用上下文且不生成投掷动作");
            send(s, {{"action", "capture"}, {"map", "旧自定义地图"}});
            s.submit(frame(2), "capture-form-reload");
            state = wait(s);
            check(state["recipes"].size() == 3 && state["recipes"][2]["map"] == "旧自定义地图" &&
                  state["recipes"][2]["team"] == "T",
                  "地图原有自由文本兼容且未填写阵营继续回退");
            send(s, {{"action", "capture"}, {"map", ""}});
            s.submit(frame(3), "capture-form-reload");
            state = wait(s);
            check(state["recipes"].size() == 4 && state["recipes"][3]["map"] == "de_dust2" &&
                  state["recipes"][3]["team"] == "T",
                  "旧网页未填写地图时仍采用当前上下文");
            std::size_t reference_count = 0;
            for (const auto &entry : std::filesystem::directory_iterator(capture_root))
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "raw.png"))
                    ++reference_count;
            check(reference_count == 4, "被拒绝的采集没有写出额外参考图");
        }
        std::cout<<"increment ROI trigger, geometry, source, practice persistence passed\n"; return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<" evidence="<<root<<'\n'; return 1; }
}
