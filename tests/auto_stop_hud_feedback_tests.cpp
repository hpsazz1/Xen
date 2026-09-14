#include "auto_stop_probe/hud_feedback_internal.h"
#include <iostream>
using namespace auto_stop_probe_detail;
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    Json a{{"timings",Json::array()},{"shots",Json::array()}};
    auto s=summarize_hud_feedback(a,{});
    check(s["timing"]["habit"]=="暂无" && s["shooting"]["stable_percent"].is_null(),"空数据不显示正常或100%稳定");
    for(double d:{-12.,-6.,0.,6.})a["timings"].push_back({{"delta_ms",d}});
    a["total_timing_count"]=400;
    a["shots"].push_back({{"samples",Json::array({Json{{"valid",true},{"speed_ratio",.8}},Json{{"valid",true},{"speed_ratio",1.25}}})}});
    a["shots"].push_back({{"samples",Json::array({Json{{"valid",false},{"speed_ratio",0}}})}});
    s=summarize_hud_feedback(a,{});
    check(s["timing"]["mean_ms"]==-3 && s["timing"]["habit"]=="正常" && s["timing"]["count"]==400,"平均与总数口径");
    check(std::abs(s["timing"]["stddev_ms"].get<double>()-std::sqrt(45.))<1e-8 && s["timing"]["excellent_percent"]==75,"总体波动和优秀率");
    check(s["shooting"]["stable_percent"]==50 && s["shooting"]["mean_error"]==.25 && s["shooting"]["sample_count"]==3,"全部有效采样汇总且排除无效");
    check(s["shooting"]["latest_ratio"].is_null(),"最后无效样本不能沿用上枪结果");
    a["timings"]=Json::array({Json{{"delta_ms",-5.1}}});
    check(summarize_hud_feedback(a,{})["timing"]["habit"]=="偏早","习惯偏早边界");
    a["timings"]=Json::array({Json{{"delta_ms",5.0}}});
    check(summarize_hud_feedback(a,{})["timing"]["habit"]=="正常","习惯5毫秒仍正常");
    a["timings"]=Json::array({Json{{"delta_ms",5.1}},Json{{"delta_ms",0},{"atomic_ambiguous",true}}});
    check(summarize_hud_feedback(a,{})["timing"]["habit"]=="偏晚","歧义不稀释习惯");
    SamplingSettings custom;custom.clean_shot_speed_ratio=.5;
    s=summarize_hud_feedback(a,custom);
    check(std::abs(s["baseline"]["default_threshold_q"].get<double>()-.68)<1e-8 && s["baseline"]["active_threshold_q"]==1,"默认阈比与当前阈值分别显示");
    a["timings"]=Json::array();for(int i=0;i<301;++i)a["timings"].push_back({{"delta_ms",i ? 0 : 100}});
    check(summarize_hud_feedback(a,{})["timing"]["mean_ms"]==0,"统计保留300条而非图形32条");
    std::cout<<"HUD参考统计专项通过\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
