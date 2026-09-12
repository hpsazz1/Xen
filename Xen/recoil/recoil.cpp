#include "recoil/recoil.h"
#include "recoil/recoil_calibration.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
constexpr int kCommandAxisLimitCounts = 32767;
constexpr std::uint64_t kReplayAdvanceLimit = 500000;
bool finite(double x) { return std::isfinite(x); }
bool hash(const std::string& value) {
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c) {
        return c>='0'&&c<='9' || c>='a'&&c<='f' || c>='A'&&c<='F';
    });
}
const char* stage(RecoilProfileState state) {
    switch(state) {
    case RecoilProfileState::IMPORTED:return "IMPORTED";
    case RecoilProfileState::SCHEMA_VALID:return "SCHEMA_VALID";
    case RecoilProfileState::CALIBRATED:return "CALIBRATED";
    case RecoilProfileState::ACCEPTED:return "ACCEPTED";
    } return "INVALID";
}
std::optional<double> number(const Json& json,const char* key) {
    if (!json.contains(key) || json.at(key).is_null()) return {};
    return json.at(key).get<double>();
}
Json optional(std::optional<double> value) { return value ? Json(*value) : Json(nullptr); }
bool execution_profile(const RecoilProfile& profile) {
    return (profile.state==RecoilProfileState::CALIBRATED || profile.state==RecoilProfileState::ACCEPTED) &&
        profile.phase_tolerance_ms.has_value();
}
RecoilTime after(RecoilTime time,double milliseconds) {
    return time + std::chrono::duration_cast<RecoilClock::duration>(std::chrono::duration<double,std::milli>(milliseconds));
}
double elapsed(RecoilTime now,RecoilTime before) {
    return std::chrono::duration<double,std::milli>(now-before).count();
}
}

bool validate_recoil_profile(const RecoilProfile& p,std::string& error) noexcept {
    try {
        auto reject=[&](const char* reason){error=reason;return false;};
        if(p.schema_version!=1 || p.id.empty() || p.id.size()>128 || p.weapon_id.empty() || p.weapon_id.size()>128 || !p.revision)
            return reject("压枪schema或版本身份无效");
        if(p.unit!="device_counts" || p.sample_semantics!="cumulative" || p.fire_mode!="automatic")
            return reject("只支持显式累计device_counts及automatic曲线");
        if(std::string_view(stage(p.state))=="INVALID")return reject("曲线状态无效");
        if(p.points.size()<2 || p.points.size()>100000)return reject("曲线节点数量无效");
        if(p.points.front().time_ms!=0 || p.points.front().x_counts!=0 || p.points.front().y_counts!=0)
            return reject("曲线必须显式从(0,0,0)开始");
        double previous=-1;
        for(const auto& point:p.points) {
            if(!finite(point.time_ms)||!finite(point.x_counts)||!finite(point.y_counts)||point.time_ms<=previous ||
                point.time_ms>60000 || std::abs(point.x_counts)>1e7 || std::abs(point.y_counts)>1e7)
                return reject("节点必须有限、时间严格递增且在有界范围内");
            previous=point.time_ms;
        }
        if(p.phase_tolerance_ms && (!finite(*p.phase_tolerance_ms)||*p.phase_tolerance_ms<=0||*p.phase_tolerance_ms>1000))
            return reject("相位容差无效");
        if(p.recovery_ms && (!finite(*p.recovery_ms)||*p.recovery_ms<0||*p.recovery_ms>60000))
            return reject("恢复时间无效");
        if(p.calibration.sensitivity && (!finite(*p.calibration.sensitivity)||*p.calibration.sensitivity<=0))
            return reject("校准灵敏度无效");
        if(!p.source.sha256.empty() && !hash(p.source.sha256))return reject("来源SHA256无效");
        if(p.state==RecoilProfileState::CALIBRATED || p.state==RecoilProfileState::ACCEPTED) {
            if(!p.phase_tolerance_ms || !p.calibration.sensitivity || p.calibration.game_build.empty() ||
                p.calibration.input_path.empty() || p.calibration.conditions.empty() || p.calibration.evidence.empty() ||
                !hash(p.source.sha256)) return reject("已校准状态缺少条件、相位或证据来源");
        }
        error.clear();return true;
    } catch(...) {return false;}
}
bool load_recoil_profile(std::string_view input,RecoilProfile& output,std::string& error) noexcept {
    try {
        if(input.size()>16*1024*1024) {error="曲线JSON超过16MiB";return false;}
        auto json=Json::parse(input.begin(),input.end(),[&](int depth,Json::parse_event_t,Json&){
            if(depth>16)throw std::runtime_error("曲线JSON嵌套过深");return true;
        });
        RecoilProfile p;
        if(json.at("schema_version")!=1||!json.at("revision").is_number_unsigned())
            throw std::runtime_error("曲线schema或revision类型无效");
        p.schema_version=json.at("schema_version").get<std::uint32_t>();
        p.id=json.at("id").get<std::string>();p.weapon_id=json.at("weapon_id").get<std::string>();
        p.revision=json.at("revision").get<std::uint64_t>();
        p.unit=json.at("unit").get<std::string>();p.sample_semantics=json.at("sample_semantics").get<std::string>();
        p.fire_mode=json.at("fire_mode").get<std::string>();
        auto state=json.at("state").get<std::string>();
        if(state=="IMPORTED")p.state=RecoilProfileState::IMPORTED;
        else if(state=="SCHEMA_VALID")p.state=RecoilProfileState::SCHEMA_VALID;
        else if(state=="CALIBRATED")p.state=RecoilProfileState::CALIBRATED;
        else if(state=="ACCEPTED")p.state=RecoilProfileState::ACCEPTED;
        else throw std::runtime_error("未知曲线状态");
        const auto& s=json.at("source");
        p.source={s.value("repository",""),s.value("commit",""),s.value("sha256",""),s.value("license",""),
            s.value("source_unit",""),s.value("conversion_revision",""),s.value("redistribution_verified",false)};
        const auto& c=json.at("calibration");
        p.calibration={c.value("game_build",""),c.value("input_path",""),c.value("conditions",""),c.value("evidence",""),number(c,"sensitivity")};
        p.phase_tolerance_ms=number(json,"phase_tolerance_ms");p.recovery_ms=number(json,"recovery_ms");
        const auto& points=json.at("points");
        if(!points.is_array()||points.size()>100000)throw std::runtime_error("曲线节点数量无效");
        for(const auto& point:points) {
            if(!point.is_array()||point.size()!=3)throw std::runtime_error("节点必须为[time_ms,x_counts,y_counts]");
            p.points.push_back({point[0].get<double>(),point[1].get<double>(),point[2].get<double>()});
        }
        if(!validate_recoil_profile(p,error))return false;
        output=std::move(p);return true;
    } catch(const std::exception& e) {try{error=e.what();}catch(...){}return false;}
      catch(...) {return false;}
}
std::string serialize_recoil_profile(const RecoilProfile& p) {
    std::string error;if(!validate_recoil_profile(p,error))throw std::invalid_argument(error);
    Json points=Json::array();for(const auto& point:p.points)points.push_back({point.time_ms,point.x_counts,point.y_counts});
    Json json={{"schema_version",p.schema_version},{"id",p.id},{"revision",p.revision},{"weapon_id",p.weapon_id},
        {"unit",p.unit},{"sample_semantics",p.sample_semantics},{"fire_mode",p.fire_mode},{"state",stage(p.state)},
        {"source",{{"repository",p.source.repository},{"commit",p.source.commit},{"sha256",p.source.sha256},
            {"license",p.source.license},{"source_unit",p.source.source_unit},{"conversion_revision",p.source.conversion_revision},
            {"redistribution_verified",p.source.redistribution_verified}}},
        {"calibration",{{"game_build",p.calibration.game_build},{"input_path",p.calibration.input_path},
            {"conditions",p.calibration.conditions},{"evidence",p.calibration.evidence},{"sensitivity",optional(p.calibration.sensitivity)}}},
        {"phase_tolerance_ms",optional(p.phase_tolerance_ms)},{"recovery_ms",optional(p.recovery_ms)},{"points",points}};
    return json.dump(2);
}
RecoilPoint sample_recoil_profile(const RecoilProfile& p,double time) noexcept {
    if(p.points.empty()||!finite(time)||time<=0)return {};
    if(time>=p.points.back().time_ms)return p.points.back();
    auto right=std::upper_bound(p.points.begin(),p.points.end(),time,[](double t,const auto& point){return t<point.time_ms;});
    if(right==p.points.begin())return {};
    const auto& a=*(right-1);const auto& b=*right;
    const double fraction=(time-a.time_ms)/(b.time_ms-a.time_ms);
    return {time,a.x_counts+(b.x_counts-a.x_counts)*fraction,a.y_counts+(b.y_counts-a.y_counts)*fraction};
}
bool compile_recoil_profile(const RecoilProfile& base,const RecoilTuning& tuning,RecoilProfile& output,std::string& error) noexcept {
    try {
        if(!validate_recoil_profile(base,error))return false;
        if(!finite(tuning.x_strength)||!finite(tuning.y_strength)||!finite(tuning.start_offset_ms)||!finite(tuning.time_scale)||
            tuning.x_strength<0||tuning.x_strength>2||tuning.y_strength<0||tuning.y_strength>2||tuning.time_scale<0.1||tuning.time_scale>10) {
            error="强度须0..200%，时序比例须0.1..10且参数有限";return false;
        }
        RecoilProfile compiled=base;
        if(tuning.x_strength==1&&tuning.y_strength==1&&tuning.start_offset_ms==0&&tuning.time_scale==1) {output=std::move(compiled);return true;}
        if(base.revision==std::numeric_limits<std::uint64_t>::max()){error="曲线版本溢出";return false;}
        auto first=std::find_if(base.points.begin(),base.points.end(),[](const auto& p){return p.x_counts!=0||p.y_counts!=0;});
        const double anchor=first==base.points.end()?base.points.back().time_ms:first->time_ms;
        const double onset=first==base.points.end()?base.points.back().time_ms:(first-1)->time_ms;
        if(onset+tuning.start_offset_ms<0){error="开始时刻不能早于射击起点";return false;}
        for(std::size_t i=1;i<compiled.points.size();++i) {
            auto& p=compiled.points[i];
            p.time_ms=tuning.start_offset_ms+(p.time_ms<=anchor?p.time_ms:anchor+(p.time_ms-anchor)*tuning.time_scale);
            p.x_counts*=tuning.x_strength;p.y_counts*=tuning.y_strength;
        }
        // 正偏移必须保留显式零平台，不能把延迟误变成首段减速。
        if(tuning.start_offset_ms>0)compiled.points.insert(compiled.points.begin()+1,{tuning.start_offset_ms,0,0});
        else if(tuning.start_offset_ms<0) {
            // 只裁掉平移后位于起点前的零平台，禁止丢弃非零位移。
            compiled.points.erase(std::remove_if(compiled.points.begin()+1,compiled.points.end(),[](const auto& p){
                return p.time_ms<=0&&p.x_counts==0&&p.y_counts==0;
            }),compiled.points.end());
        }
        ++compiled.revision;compiled.state=RecoilProfileState::SCHEMA_VALID;
        compiled.calibration.evidence.clear();compiled.phase_tolerance_ms.reset();compiled.recovery_ms.reset();
        if(!validate_recoil_profile(compiled,error))return false;
        output=std::move(compiled);return true;
    }catch(...){return false;}
}
const char* RecoilReasonName(RecoilReason r) noexcept {
    switch(r) {
    case RecoilReason::NONE:return "none";case RecoilReason::DISABLED:return "disabled";
    case RecoilReason::INVALID_PROFILE:return "invalid_profile";case RecoilReason::UNCALIBRATED:return "uncalibrated";
    case RecoilReason::CONTEXT:return "context";case RecoilReason::WAIT_RELEASE:return "wait_release";
    case RecoilReason::RESET_UNVERIFIED:return "reset_unverified";case RecoilReason::PROFILE_CHANGED:return "profile_changed";
    case RecoilReason::RELEASED:return "released";case RecoilReason::CANCELED:return "canceled";
    case RecoilReason::LATE:return "late";case RecoilReason::EXHAUSTED:return "exhausted";
    case RecoilReason::NOT_SENT:return "not_sent";case RecoilReason::UNKNOWN_RECEIPT:return "unknown_receipt";
    case RecoilReason::COMMAND_PENDING:return "command_pending";case RecoilReason::INVALID_TIME:return "invalid_time";
    case RecoilReason::LIMIT:return "limit";
    }return "invalid";
}
RecoilController::RecoilController(std::shared_ptr<const RecoilCalibrationPermit> permit)
    : calibration_permit_(std::move(permit)), calibration_claimed_(calibration_permit_&&calibration_permit_->try_claim_controller()) {}
RecoilController::RecoilController(OfflineReplayTag,double phase_budget_ms) noexcept
    : offline_phase_budget_ms_(phase_budget_ms) {}
double RecoilController::phase_budget_ms() const noexcept {
    if(offline_phase_budget_ms_>0)return offline_phase_budget_ms_;
    if(calibration_permit_)return calibration_permit_->limits().command_phase_budget_ms;
    return profile_ ? profile_->phase_tolerance_ms.value_or(0) : 0;
}
RecoilDecision RecoilController::result() const noexcept {
    RecoilDecision decision;decision.snapshot=state_;
    if(active_&&profile_&&phase_budget_ms()>0)
        decision.next_deadline=after(last_sample_at_,phase_budget_ms());
    return decision;
}
RecoilDecision RecoilController::cancel(RecoilReason reason,RecoilTime now) noexcept {
    if(state_.pending) {
        state_.discarded_x+=pending_remainder_x_;state_.discarded_y+=pending_remainder_y_;
        pending_remainder_x_=pending_remainder_y_=0;
    }
    active_=false;release_seen_=false;
    state_.discarded_x+=state_.remainder_x;state_.discarded_y+=state_.remainder_y;
    state_.remainder_x=state_.remainder_y=0;
    state_.reason=reason;state_.phase=state_.faulted?RecoilPhase::FAULT:RecoilPhase::WAIT_RELEASE;
    last_now_=std::max(last_now_,now);
    return result();
}
RecoilDecision RecoilController::advance(const RecoilInput& input,RecoilTime now) noexcept {
    if(now==RecoilTime{}||now<last_now_)return cancel(RecoilReason::INVALID_TIME,last_now_);
    last_now_=now;
    if(!input.enabled){cancel(RecoilReason::DISABLED,now);state_.phase=RecoilPhase::DISABLED;return result();}
    const bool changed=input.profile!=profile_||input.weapon_generation!=weapon_generation_||input.device_epoch!=device_epoch_;
    if(changed) {
        cancel(RecoilReason::PROFILE_CHANGED,now);
        profile_=input.profile;weapon_generation_=input.weapon_generation;device_epoch_=input.device_epoch;
        std::string error;
        profile_valid_=profile_&&validate_recoil_profile(*profile_,error);
    }
    if(!profile_valid_)return cancel(RecoilReason::INVALID_PROFILE,now);
    if(calibration_permit_) {
        if(!calibration_claimed_ || !calibration_permit_->matches(profile_) || now<calibration_permit_->armed_at() || now>=calibration_permit_->expires_at())
            return cancel(RecoilReason::UNCALIBRATED,now);
        // 一次授权只允许一个弹序；不得借释放或recovery_qualified再次起压。
        if(has_fired_&&!active_)return cancel(RecoilReason::EXHAUSTED,now);
        if(active_&&elapsed(now,started_at_)>=calibration_permit_->limits().max_firing_duration_ms)
            return cancel(RecoilReason::LIMIT,now);
    } else if(offline_phase_budget_ms_>0) {
        if(profile_->state!=RecoilProfileState::SCHEMA_VALID || !profile_->calibration.evidence.empty() ||
            profile_->phase_tolerance_ms || profile_->recovery_ms)return cancel(RecoilReason::UNCALIBRATED,now);
    } else if(!execution_profile(*profile_))return cancel(RecoilReason::UNCALIBRATED,now);
    if(!input.healthy||!input.focused||!input.permission||!input.profile_conditions_match||!weapon_generation_||!device_epoch_)
        return cancel(RecoilReason::CONTEXT,now);
    if(state_.faulted)return result();
    if(!input.held) {
        if(active_||(!released_since_firing_&&has_fired_)){released_at_=now;released_since_firing_=true;}
        cancel(RecoilReason::RELEASED,now);release_seen_=true;
        state_.phase=RecoilPhase::READY;return result();
    }
    if(state_.pending) {
        if(now>pending_.expires_at)return cancel(RecoilReason::LATE,now);
        state_.reason=RecoilReason::COMMAND_PENDING;return result();
    }
    if(!active_) {
        if(!release_seen_){state_.reason=RecoilReason::WAIT_RELEASE;return result();}
        const bool recovered=!has_fired_||input.recovery_qualified || (released_since_firing_&&profile_->recovery_ms &&
            elapsed(now,released_at_)>=*profile_->recovery_ms);
        if(!recovered)return cancel(RecoilReason::RESET_UNVERIFIED,now);
        const auto start=input.firing_started_at==RecoilTime{}?now:input.firing_started_at;
        if(start>now||elapsed(now,start)>phase_budget_ms())return cancel(RecoilReason::LATE,now);
        if(state_.session_id==std::numeric_limits<std::uint64_t>::max())return cancel(RecoilReason::LIMIT,now);
        ++state_.session_id;active_=has_fired_=true;released_since_firing_=false;release_seen_=false;
        sampled_={};started_at_=last_sample_at_=start;
        state_.phase=RecoilPhase::FIRING;state_.reason=RecoilReason::NONE;
        if(now==start)return result();
    }
    if(elapsed(now,last_sample_at_)>phase_budget_ms())return cancel(RecoilReason::LATE,now);
    auto sample=sample_recoil_profile(*profile_,elapsed(now,started_at_));
    const double x=sample.x_counts-sampled_.x_counts+state_.remainder_x;
    const double y=sample.y_counts-sampled_.y_counts+state_.remainder_y;
    if(!finite(x)||!finite(y)||std::abs(x)>kCommandAxisLimitCounts||std::abs(y)>kCommandAxisLimitCounts)return cancel(RecoilReason::LIMIT,now);
    const int dx=static_cast<int>(x),dy=static_cast<int>(y);
    state_.planned_x+=sample.x_counts-sampled_.x_counts;state_.planned_y+=sample.y_counts-sampled_.y_counts;
    if(!dx&&!dy) {
        sampled_=sample;last_sample_at_=now;state_.remainder_x=x;state_.remainder_y=y;
        if(elapsed(now,started_at_)>=profile_->points.back().time_ms) {
            cancel(RecoilReason::EXHAUSTED,now);state_.phase=RecoilPhase::EXHAUSTED;
        }
        return result();
    }
    if(next_command_id_==std::numeric_limits<std::uint64_t>::max())return cancel(RecoilReason::LIMIT,now);
    pending_={++next_command_id_,state_.session_id,profile_->revision,weapon_generation_,device_epoch_,
        now,after(now,phase_budget_ms()),dx,dy};
    pending_sample_=sample;pending_remainder_x_=x-dx;pending_remainder_y_=y-dy;
    state_.remainder_x=state_.remainder_y=0;
    state_.command_id=pending_.command_id;state_.pending=true;state_.phase=RecoilPhase::PENDING;
    auto decision=result();decision.has_intent=true;decision.intent=pending_;return decision;
}
RecoilDecision RecoilController::acknowledge(const RecoilReceipt& receipt,RecoilTime now) noexcept {
    if(!state_.pending||receipt.command_id!=pending_.command_id)return result();
    const bool valid=now>=last_now_&&receipt.completed_at>=pending_.planned_at&&receipt.completed_at<=now;
    state_.pending=false;last_now_=std::max(now,last_now_);
    if(!valid||receipt.status==RecoilReceiptStatus::UNKNOWN) {
        state_.unknown_x+=pending_.dx_counts;state_.unknown_y+=pending_.dy_counts;
        state_.discarded_x+=pending_remainder_x_;state_.discarded_y+=pending_remainder_y_;
        pending_remainder_x_=pending_remainder_y_=0;
        state_.faulted=true;return cancel(RecoilReason::UNKNOWN_RECEIPT,now);
    }
    if(receipt.status==RecoilReceiptStatus::NOT_SENT) {
        state_.discarded_x+=pending_.dx_counts;state_.discarded_y+=pending_.dy_counts;
        state_.discarded_x+=pending_remainder_x_;state_.discarded_y+=pending_remainder_y_;
        pending_remainder_x_=pending_remainder_y_=0;
        return cancel(RecoilReason::NOT_SENT,now);
    }
    state_.confirmed_x+=pending_.dx_counts;state_.confirmed_y+=pending_.dy_counts;
    if(!active_)return result();
    if(receipt.completed_at>pending_.expires_at) {
        state_.discarded_x+=pending_remainder_x_;state_.discarded_y+=pending_remainder_y_;
        pending_remainder_x_=pending_remainder_y_=0;
        return cancel(RecoilReason::LATE,now);
    }
    sampled_=pending_sample_;last_sample_at_=pending_.planned_at;
    state_.remainder_x=pending_remainder_x_;state_.remainder_y=pending_remainder_y_;
    state_.phase=RecoilPhase::FIRING;state_.reason=RecoilReason::NONE;
    if(sampled_.time_ms>=profile_->points.back().time_ms) {
        cancel(RecoilReason::EXHAUSTED,now);state_.phase=RecoilPhase::EXHAUSTED;
    }
    return result();
}
bool validate_recoil_candidate_execution(const RecoilProfile& candidate,
    const RecoilCandidateReplayOptions& options,RecoilCandidateReplayReport& report,std::string& error) noexcept {
    report={};
    try {
        const auto require=[](bool condition,const char* message) {
            if(!condition)throw std::runtime_error(message);
        };
        report.advance_limit=kReplayAdvanceLimit;
        report.command_axis_limit_counts=kCommandAxisLimitCounts;
        require(candidate.state==RecoilProfileState::SCHEMA_VALID && candidate.calibration.evidence.empty() &&
            !candidate.phase_tolerance_ms && !candidate.recovery_ms,"回放只接受无校准声明的SCHEMA_VALID候选");
        RecoilProfile compiled;
        if(!compile_recoil_profile(candidate,{},compiled,error))return false;
        require(finite(options.step_ms)&&finite(options.phase_budget_ms)&&options.step_ms>0&&
            options.step_ms<=options.phase_budget_ms&&options.phase_budget_ms<=1000,"软件步长或相位预算无效");
        using Duration=RecoilClock::duration;
        using Milliseconds=std::chrono::duration<double,std::milli>;
        const auto step=std::chrono::duration_cast<Duration>(Milliseconds(options.step_ms));
        const auto phase=std::chrono::duration_cast<Duration>(Milliseconds(options.phase_budget_ms));
        const auto tick=Duration{1};
        require(step>=tick&&phase>=step,"软件步长或相位预算小于时钟粒度");
        report.step_ms=Milliseconds(step).count();report.phase_budget_ms=Milliseconds(phase).count();
        report.duration_ms=compiled.points.back().time_ms;
        const auto end=std::chrono::ceil<Duration>(Milliseconds(report.duration_ms));
        const auto iterations=[&](Duration stride) {
            return static_cast<std::uint64_t>((end.count()-1)/stride.count()+1);
        };
        // 正常、边缘和固定数量的故障场景共用额度；极小步长在进入循环前拒绝。
        const auto normal_steps=iterations(step),edge_steps=iterations(phase);
        require(normal_steps<=(kReplayAdvanceLimit-64)/6 &&
            edge_steps<=kReplayAdvanceLimit-64-normal_steps*6,"候选回放超过有界工作量");
        auto profile=std::make_shared<const RecoilProfile>(std::move(compiled));
        const auto begin=RecoilTime{}+std::chrono::seconds(1);
        const auto start=begin+tick;
        const auto input_for=[&] {
            RecoilInput input;
            // 这些仅是纯计算夹具，不是设备输入或可外传的校准许可。
            input.enabled=input.healthy=input.focused=input.permission=input.profile_conditions_match=true;
            input.weapon_generation=input.device_epoch=1;input.profile=profile;input.firing_started_at=start;
            return input;
        };
        const auto advance=[&](RecoilController& controller,const RecoilInput& input,RecoilTime now) {
            require(report.advance_calls<kReplayAdvanceLimit,"候选回放超过有界工作量");
            ++report.advance_calls;return controller.advance(input,now);
        };
        const auto arm=[&](RecoilController& controller,RecoilInput& input) {
            auto released=advance(controller,input,begin);
            require(!released.has_intent&&released.snapshot.phase==RecoilPhase::READY,"候选回放无法建立释放边沿");
            input.held=true;
            auto started=advance(controller,input,start);
            require(!started.has_intent&&started.snapshot.phase==RecoilPhase::FIRING&&started.snapshot.session_id==1,
                "候选回放无法建立唯一弹序");
        };
        const auto near=[](double actual,double expected) {
            return finite(actual)&&finite(expected)&&std::abs(actual-expected)<=1e-9*(1+std::abs(expected));
        };
        const auto ensure_running=[&](const RecoilDecision& decision) {
            if(decision.snapshot.faulted || (decision.snapshot.phase!=RecoilPhase::FIRING&&
                decision.snapshot.phase!=RecoilPhase::PENDING&&decision.snapshot.phase!=RecoilPhase::EXHAUSTED))
                throw std::runtime_error(std::string("软件回放被生产执行器拒绝：")+RecoilReasonName(decision.snapshot.reason));
        };
        const auto check_intent=[&](const RecoilDecision& decision,const RecoilSnapshot& before,RecoilTime now) {
            const auto& intent=decision.intent;
            const auto axis=std::max(std::abs(static_cast<std::int64_t>(intent.dx_counts)),
                std::abs(static_cast<std::int64_t>(intent.dy_counts)));
            require(axis>0&&axis<=kCommandAxisLimitCounts,"回放命令超出生产单轴范围");
            require(decision.snapshot.pending && decision.snapshot.confirmed_x==before.confirmed_x&&
                decision.snapshot.confirmed_y==before.confirmed_y,"回放在ACK前提前记入成功量");
            require(intent.session_id==1&&intent.profile_revision==profile->revision&&intent.weapon_generation==1&&
                intent.device_epoch==1&&intent.planned_at==now&&intent.expires_at>now,"回放意图身份或时限无效");
            report.max_abs_command_axis_counts=std::max(report.max_abs_command_axis_counts,static_cast<int>(axis));
        };
        const auto acknowledge=[&](RecoilController& controller,const RecoilDecision& decision,
            const RecoilSnapshot& before,RecoilTime now,bool primary) {
            const auto& intent=decision.intent;
            const RecoilReceipt receipt{intent.command_id,RecoilReceiptStatus::ACKNOWLEDGED,now};
            auto done=controller.acknowledge(receipt,now);
            require(!done.snapshot.pending&&!done.snapshot.faulted&&
                done.snapshot.confirmed_x==before.confirmed_x+intent.dx_counts&&
                done.snapshot.confirmed_y==before.confirmed_y+intent.dy_counts,"回放ACK未按实际整数命令记账");
            require(done.snapshot.unknown_x==0&&done.snapshot.unknown_y==0,"正常回放出现未知执行量");
            auto repeated=controller.acknowledge(receipt,now);
            require(repeated.snapshot.confirmed_x==done.snapshot.confirmed_x&&
                repeated.snapshot.confirmed_y==done.snapshot.confirmed_y,"重复ACK重复记账");
            if(primary) {
                ++report.acknowledged_commands;
                report.acknowledged_l1_counts+=static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(intent.dx_counts)))+
                    static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(intent.dy_counts)));
            }
        };
        const auto run_schedule=[&](Duration stride,bool primary) {
            RecoilController controller(RecoilController::OfflineReplayTag{},report.phase_budget_ms);
            auto input=input_for();arm(controller,input);
            for(auto offset=std::min(stride,end);;offset=std::min(offset+stride,end)) {
                const auto now=start+offset;
                const auto before=controller.snapshot();
                auto decision=advance(controller,input,now);ensure_running(decision);
                if(decision.has_intent) {
                    check_intent(decision,before,now);acknowledge(controller,decision,before,now,primary);
                }
                if(offset==end)break;
            }
            const auto terminal=controller.snapshot();
            require(terminal.phase==RecoilPhase::EXHAUSTED&&!terminal.pending&&!terminal.faulted,"候选尾部未结束唯一弹序");
            const auto& tail=profile->points.back();
            require(near(terminal.planned_x,tail.x_counts)&&near(terminal.planned_y,tail.y_counts)&&
                near(terminal.confirmed_x+terminal.discarded_x,terminal.planned_x)&&
                near(terminal.confirmed_y+terminal.discarded_y,terminal.planned_y),"候选量化或尾部账本不守恒");
            require(std::abs(terminal.discarded_x)<1&&std::abs(terminal.discarded_y)<1&&
                terminal.remainder_x==0&&terminal.remainder_y==0,"候选结束时遗留或丢弃整count");
            const auto held=advance(controller,input,start+end+phase);
            require(!held.has_intent&&held.snapshot.session_id==1&&held.snapshot.confirmed_x==terminal.confirmed_x&&
                held.snapshot.confirmed_y==terminal.confirmed_y,"候选耗尽后长按复活或重复尾部");
            if(primary)report.terminal=terminal;
        };
        run_schedule(step,true);run_schedule(phase,false);
        report.phase_edge_checked=report.tail_checked=true;
        {
            RecoilController edge(RecoilController::OfflineReplayTag{},report.phase_budget_ms);
            auto input=input_for();arm(edge,input);
            ensure_running(advance(edge,input,start+phase));
            RecoilController late(RecoilController::OfflineReplayTag{},report.phase_budget_ms);
            input=input_for();arm(late,input);
            const auto rejected=advance(late,input,start+phase+tick);
            require(!rejected.has_intent&&rejected.snapshot.reason==RecoilReason::LATE,
                "超过软件相位时限一个tick仍生成意图");
            require(!advance(late,input,start+phase+tick+tick).has_intent,"迟到取消后仍追赶补发");
        }
        if(!report.acknowledged_commands) {
            // 零意图曲线仍验证取消；没有待确认命令时不捏造回执故障覆盖。
            RecoilController controller(RecoilController::OfflineReplayTag{},report.phase_budget_ms);
            auto input=input_for();arm(controller,input);
            controller.cancel(RecoilReason::CANCELED,start);
            require(!advance(controller,input,start+tick).has_intent&&controller.snapshot().session_id==1,
                "零意图曲线取消后仍重新开始");
            report.cancellation_checked=true;
        } else {
            enum class Probe { CANCELED, UNKNOWN, NOT_SENT, ACK_AT_LIMIT, ACK_LATE };
            // 每个场景最多重放一次到首个非零意图，共五次；不按节点重复回放。
            for(const auto probe : {Probe::CANCELED,Probe::UNKNOWN,Probe::NOT_SENT,Probe::ACK_AT_LIMIT,Probe::ACK_LATE}) {
                RecoilController controller(RecoilController::OfflineReplayTag{},report.phase_budget_ms);
                auto input=input_for();arm(controller,input);
                bool found=false;
                for(auto offset=std::min(step,end);;offset=std::min(offset+step,end)) {
                    const auto now=start+offset;
                    const auto before=controller.snapshot();
                    auto decision=advance(controller,input,now);ensure_running(decision);
                    if(decision.has_intent) {
                        found=true;check_intent(decision,before,now);
                        const auto& intent=decision.intent;
                        const auto pending=advance(controller,input,now);
                        require(!pending.has_intent&&pending.snapshot.pending&&pending.snapshot.command_id==intent.command_id,
                            "未决意图产生重复发送");
                        const auto foreign=controller.acknowledge({intent.command_id+1,RecoilReceiptStatus::ACKNOWLEDGED,now},now);
                        require(foreign.snapshot.pending&&foreign.snapshot.confirmed_x==before.confirmed_x&&
                            foreign.snapshot.confirmed_y==before.confirmed_y,"其他命令ACK污染当前意图");
                        if(probe==Probe::CANCELED)controller.cancel(RecoilReason::CANCELED,now);
                        const auto completed=(probe==Probe::ACK_AT_LIMIT)?intent.expires_at:
                            (probe==Probe::ACK_LATE||probe==Probe::CANCELED)?intent.expires_at+tick:now;
                        const auto status=probe==Probe::UNKNOWN?RecoilReceiptStatus::UNKNOWN:
                            probe==Probe::NOT_SENT?RecoilReceiptStatus::NOT_SENT:RecoilReceiptStatus::ACKNOWLEDGED;
                        const auto done=controller.acknowledge({intent.command_id,status,completed},completed);
                        require(!done.snapshot.pending,"回执后意图仍未决");
                        if(probe==Probe::UNKNOWN) {
                            require(done.snapshot.faulted&&done.snapshot.reason==RecoilReason::UNKNOWN_RECEIPT&&
                                done.snapshot.confirmed_x==before.confirmed_x&&done.snapshot.confirmed_y==before.confirmed_y&&
                                done.snapshot.unknown_x==intent.dx_counts&&done.snapshot.unknown_y==intent.dy_counts,
                                "UNKNOWN回执被记成成功或没有锁存故障");
                            input.held=false;advance(controller,input,completed+tick);
                            input.held=true;
                            require(!advance(controller,input,completed+tick+tick).has_intent&&controller.snapshot().faulted,
                                "UNKNOWN故障被新的按键边沿清除");
                            report.unknown_receipt_checked=true;
                        } else if(probe==Probe::NOT_SENT) {
                            require(!done.snapshot.faulted&&done.snapshot.reason==RecoilReason::NOT_SENT&&
                                done.snapshot.confirmed_x==before.confirmed_x&&done.snapshot.confirmed_y==before.confirmed_y&&
                                done.snapshot.unknown_x==0&&done.snapshot.unknown_y==0,
                                "NOT_SENT回执污染成功量或未知量");
                            require(!advance(controller,input,completed+tick).has_intent,"NOT_SENT后仍补发旧意图");
                            report.not_sent_checked=true;
                        } else {
                            require(!done.snapshot.faulted&&done.snapshot.confirmed_x==before.confirmed_x+intent.dx_counts&&
                                done.snapshot.confirmed_y==before.confirmed_y+intent.dy_counts&&
                                done.snapshot.unknown_x==0&&done.snapshot.unknown_y==0,"ACK边界丢失已确认整数命令");
                            if(probe==Probe::ACK_AT_LIMIT) {
                                require(done.snapshot.reason!=RecoilReason::LATE,"恰好到期的ACK被错误拒绝");
                            } else {
                                require(done.snapshot.reason==(probe==Probe::CANCELED?RecoilReason::CANCELED:RecoilReason::LATE),
                                    "取消或过期ACK没有保持终止原因");
                                const auto held=advance(controller,input,completed+tick);
                                require(!held.has_intent&&held.snapshot.session_id==1&&
                                    held.snapshot.confirmed_x==done.snapshot.confirmed_x&&held.snapshot.confirmed_y==done.snapshot.confirmed_y,
                                    "取消或迟到ACK复活旧弹序");
                                if(probe==Probe::CANCELED)report.cancellation_checked=true;
                            }
                        }
                        break;
                    }
                    if(offset==end)break;
                }
                require(found,"故障回放无法重现正常弹序的首条意图");
            }
        }
        report.deadline_checked=true;
        report.validated=true;error.clear();return true;
    } catch(const std::exception& exception) {
        try {error=exception.what();}catch(...){}
    } catch(...) {try {error="候选执行回放异常";}catch(...){}}
    return false;
}
