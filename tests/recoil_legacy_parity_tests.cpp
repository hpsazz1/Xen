#include "recoil/recoil_store.h"
#include "weapon/weapon_catalog.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// 外部黄金与曲线只在用户本地传入；本目标不包含第三方数据，不连接设备。
namespace {
using Json=nlohmann::json;
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
Json read(const std::filesystem::path& file){std::ifstream input(file);require(bool(input),"无法读取输入文件");return Json::parse(input);}
}
int main(int argc,char** argv){
    try {
        require(argc==4,"用法：recoil_legacy_parity_tests <golden.json> <profiles目录> <legacy_manifest.json>");
        const auto golden=read(std::filesystem::u8path(argv[1]));
        const auto manifest=read(std::filesystem::u8path(argv[3]));
        RecoilStore store(std::filesystem::u8path(argv[2]));std::string error;
        std::vector<RecoilStoredProfile> stored;require(store.list(stored,error),error);
        Json report={{"profiles",Json::array()},{"physical_output",false}};
        std::size_t total=0;
        for(const auto& entry:manifest.at("profiles")) {
            const auto id=entry.at("id").get<std::string>();
            const auto canonical=weapon::normalize_weapon_id(entry.at("canonical_weapon_id").get<std::string>());
            std::shared_ptr<const RecoilProfile> profile;
            for(const auto& item:stored)if(item.profile->schema_version==3&&item.profile->weapon_id==canonical) {
                require(!profile,"同武器存在多个离散版本");profile=item.profile;
            }
            require(bool(profile),id+"缺少离散曲线");
            require(profile->calibration.sensitivity==golden.at("sensitivity").get<double>(),id+"灵敏度不一致");
            const auto& expected=golden.at("profiles").at(id);
            const auto& steps=expected.at("steps");require(profile->events.size()==steps.size(),id+"事件数不一致");
            for(bool delayed:{false,true}) {
                RecoilController controller;RecoilInput input;
                input.enabled=input.healthy=input.focused=input.permission=input.profile_conditions_match=true;
                input.weapon_generation=input.device_epoch=1;input.profile=profile;
                const auto begin=RecoilTime{}+std::chrono::seconds(1);
                controller.advance(input,begin);input.held=true;input.firing_started_at=begin+std::chrono::milliseconds(1);
                auto decision=controller.advance(input,input.firing_started_at);
                auto previous_now=input.firing_started_at;
                long long confirmed_x=0,confirmed_y=0;
                for(std::size_t index=0;index<steps.size();++index) {
                    const auto label=id+" step "+std::to_string(index);
                    const auto& step=steps[index];const auto& event=profile->events[index];
                    require(event.time_ms==step.at("planned_time_ms").get<double>()&&event.x_counts==step.at("requested_x").get<double>()&&
                        event.y_counts==step.at("requested_y").get<double>(),label+"浮点事件不同");
                    const auto due=input.firing_started_at+std::chrono::nanoseconds(step.at("planned_time_ns").get<long long>());
                    require(decision.next_deadline==due,label+"计划纳秒不同");
                    const auto now=delayed?std::max(previous_now+std::chrono::milliseconds(1),due+std::chrono::milliseconds(125)):due;
                    decision=controller.advance(input,now);
                    const int dx=step.at("integer_x"),dy=step.at("integer_y");
                    require(decision.has_intent==(dx!=0||dy!=0),label+"零事件处理不同");
                    if(decision.has_intent) {
                        require(decision.intent.dx_counts==dx&&decision.intent.dy_counts==dy&&decision.intent.planned_at==due,
                            label+"逐步整数或绝对计划不同");
                        const auto ack_at=delayed?now+std::chrono::milliseconds(60):now;
                        decision=controller.acknowledge({decision.intent.command_id,RecoilReceiptStatus::ACKNOWLEDGED,ack_at},ack_at);
                        previous_now=ack_at;
                    } else previous_now=now;
                    confirmed_x+=dx;confirmed_y+=dy;
                    require(decision.snapshot.confirmed_x==confirmed_x&&decision.snapshot.confirmed_y==confirmed_y,
                        label+"回执累计量不同");
                }
                require(decision.snapshot.phase==RecoilPhase::EXHAUSTED&&!decision.snapshot.pending&&!decision.snapshot.faulted,id+"未耗尽");
                require(confirmed_x==expected.at("total_x").get<long long>()&&confirmed_y==expected.at("total_y").get<long long>(),id+"总量不同");
            }
            total+=steps.size();report["profiles"].push_back({{"id",id},{"steps",steps.size()},{"on_time_and_delayed_ack",true}});
        }
        report["step_count"]=total;report["profile_count"]=report["profiles"].size();
        std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
