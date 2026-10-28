#include "grenade_alignment/grenade_alignment.h"
#include "grenade_alignment/grenade_alignment_internal.h"
#include "movement/timeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {
using namespace std::chrono_literals;
using Clock=grenade_alignment::detail::Clock;
using Json=nlohmann::json;
int failures=0;
void expect(bool value,const std::string& message) {
    if (!value) {++failures;std::cerr<<"[失败] "<<message<<'\n';}
}
cv::Mat texture(std::uint64_t seed=1347) {
    cv::Mat result(320,320,CV_8UC3);
    cv::RNG random(seed);random.fill(result,cv::RNG::UNIFORM,0,255);
    cv::GaussianBlur(result,result,{3,3},0.65);
    return result;
}
cv::Mat translated(const cv::Mat& value,double x,double y) {
    cv::Mat result;
    const cv::Mat warp=(cv::Mat_<double>(2,3)<<1,0,x,0,1,y);
    cv::warpAffine(value,result,warp,value.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
    return result;
}
struct Scene {
    Clock::time_point now=Clock::time_point(10s);
    cv::Mat original=texture();
    cv::Mat observed;
    double x=0,y=0;
    // 分开表达配置换算与实际鼠标响应，覆盖实机末段的细步进和不可达量化点。
    double response_counts_per_pixel_x=0.4,response_counts_per_pixel_y=0.4;
    double configured_counts_per_pixel_x=0.4,configured_counts_per_pixel_y=0.4;
    std::uint64_t sequence=0;
    bool stale=false,repeated=false,geometry_change=false,receiver_change=false;
    bool no_frame=false,no_response=false,unrelated=false,network=false,source_clock_missing=false;
    bool focus=true,moved=false,read_motion=false,command_failed=false,clock_warming=false;
    bool emergency=false,local_cancel=false,compressed=false;
    std::atomic_bool canceled{false};
    int moves=0,opened=0,closed=0;
    std::chrono::milliseconds close_delay{0};
    Clock::time_point first_frame_at{};
    int stop_after_frames=0;
    std::vector<MouseMoveCommand> commands;
    std::function<void(FrameTiming&)> transform_timing;
    bool last_clock_trusted=true;
    int untrusted_clock_moves=0;
};
class Device final : public IMouseController {
public:
    explicit Device(Scene& scene):scene_(scene) {}
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand& value) noexcept override {
        ++scene_.moves;scene_.commands.push_back(value);
        if (scene_.network&&!scene_.last_clock_trusted) ++scene_.untrusted_clock_moves;
        if (!scene_.no_response && !scene_.command_failed) {
            scene_.x-=value.dx_counts/scene_.response_counts_per_pixel_x;
            scene_.y-=value.dy_counts/scene_.response_counts_per_pixel_y;
        }
        return {!scene_.command_failed,scene_.now,true,scene_.now,false,{}};
    }
    bool poll_input(InputSnapshot& value) noexcept override {
        value={};value.state_valid=true;value.status=InputMonitorStatus::READY;
        value.virtual_keys[0x57]=scene_.moved;
        value.virtual_keys[0x23]=scene_.emergency;value.virtual_keys[0x25]=scene_.local_cancel;
        return true;
    }
    bool output_owner_exclusive() const noexcept override {return true;}
    bool read_movement_reports(InputReportCursor& cursor,InputReportBatch& batch) noexcept override {
        batch={};batch.subscribed=scene_.read_motion;batch.epoch=1;batch.final_sequence=scene_.sequence;
        if (scene_.read_motion && cursor.sequence<scene_.sequence && scene_.sequence>1) {
            batch.count=1;auto& event=batch.events[0];event.state_valid=true;event.physical_source_verified=true;
            event.motion_semantics=InputMotionSemantics::RELATIVE_COUNTS;event.raw_x=1;
        }
        cursor={1,scene_.sequence};return true;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override {return MouseStatus::READY;}
    std::string last_error() const override {return {};}
private:Scene& scene_;
};
class Capture final : public ICapture {
public:
    explicit Capture(Scene& scene):scene_(scene) {}
    bool open() noexcept override {++scene_.opened;return true;}
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        scene_.now+=30ms;
        if (scene_.no_frame || scene_.now<scene_.first_frame_at ||
            (scene_.stop_after_frames && scene_.sequence>=static_cast<std::uint64_t>(scene_.stop_after_frames)))
            return CaptureStatus::NO_FRAME;
        frame={};frame.width=frame.height=frame.source_width=frame.source_height=320;
        frame.encoded_width=frame.encoded_height=320;
        frame.bgr=scene_.unrelated?texture(7788):translated(
            scene_.observed.empty()?scene_.original:scene_.observed,scene_.x,scene_.y);
        if(scene_.compressed) {
            std::vector<unsigned char> bytes;
            cv::imencode(".jpg",frame.bgr,bytes,{cv::IMWRITE_JPEG_QUALITY,90});
            frame.bgr=cv::imdecode(bytes,cv::IMREAD_COLOR);
        }
        // 与场景无关的屏幕准星和底边改变不能产生假匹配或强迫零位移。
        cv::rectangle(frame.bgr,{146,146,28,28},{0,255,255},-1);
        cv::rectangle(frame.bgr,{0,282,320,38},{255,0,0},-1);
        ++scene_.sequence;frame.timing.sequence=scene_.repeated?1:scene_.sequence;
        frame.timing.captured_at=scene_.now-(scene_.stale?500ms:1ms);
        frame.timing.receiver_generation=scene_.receiver_change&&scene_.sequence>1?2:1;
        if (scene_.geometry_change&&scene_.sequence>1) frame.roi_x=1;
        if (scene_.network) {
            frame.timing.source_time_timing_valid=!scene_.source_clock_missing;
            frame.timing.source_clock_status=SourceClockStatus::VALID;
            if (scene_.clock_warming&&scene_.sequence<=3) {
                frame.timing.source_time_timing_valid=false;
                frame.timing.source_clock_status=SourceClockStatus::WARMING;
            }
            frame.timing.source_timestamp_valid=true;
            frame.timing.source_timestamp=static_cast<std::int64_t>(scene_.sequence);
            frame.timing.source_clock_session_id=1;
            frame.timing.source_time_at=scene_.now-5ms;
        }
        if (scene_.transform_timing) scene_.transform_timing(frame.timing);
        const auto& timing=frame.timing;
        scene_.last_clock_trusted=timing.source_time_timing_valid&&
            timing.source_clock_status==SourceClockStatus::VALID&&timing.source_timestamp_valid&&
            timing.source_clock_session_id!=0&&std::isfinite(timing.source_clock_uncertainty_ms)&&
            timing.source_clock_uncertainty_ms>=0&&timing.source_clock_uncertainty_ms<=50;
        return CaptureStatus::FRAME;
    }
    void close() noexcept override {++scene_.closed;scene_.now+=scene_.close_delay;}
    CaptureStatus status() const noexcept override {return CaptureStatus::READY;}
    std::string last_error() const override {return {};}
private:Scene& scene_;
};
grenade_alignment::Reference reference(const Scene& scene) {
    grenade_alignment::Reference result;
    result.aim_image=scene.original.clone();result.standing_image=scene.original.clone();
    result.geometry={320,320,0,0,320,320,1,1};result.revision="synthetic-reference";
    return result;
}
AppConfig config(const Scene& scene) {
    AppConfig result;result.capture.backend=scene.network?CaptureBackend::NDI:CaptureBackend::DESKTOP_DUPLICATION;
    if (scene.network) result.capture.ndi_clock_sync_url="udp://127.0.0.1:5011";
    result.mouse.backend=MouseBackend::KMBOX_NET;result.mouse.allow_send_input=true;
    result.keyboard.grenade_assist_enabled=scene.local_cancel;
    result.aim.counts_per_pixel_x=static_cast<float>(scene.configured_counts_per_pixel_x);
    result.aim.counts_per_pixel_y=static_cast<float>(scene.configured_counts_per_pixel_y);
    result.aim.max_counts_per_frame=4;result.aim.control_delay_ms=15;
    return result;
}
grenade_alignment::Result run(Scene& scene,bool verify=false,std::function<bool()> permission={}) {
    grenade_alignment::detail::Dependencies dependencies{
        [&](const CaptureConfig&){return std::make_unique<Capture>(scene);},[&]{return scene.now;},
        [&](std::chrono::milliseconds duration){scene.now+=duration;},
        [&]{return source_context::SourceContextSnapshot{true,scene.focus,1,scene.sequence,0,1};}};
    if (!permission) permission=[] {return true;};
    return grenade_alignment::detail::run_with_dependencies(reference(scene),config(scene),
        std::make_shared<Device>(scene),scene.canceled,verify,std::move(permission),dependencies);
}
void aligned_and_control() {
    for (const auto offset:std::array<cv::Point2d,6>{{{0,0},{24,-17},{-13,29},{1.1,-1.2},{1.5,0.5},{-0.5,1.5}}}) {
        Scene scene;scene.x=offset.x;scene.y=offset.y;
        scene.response_counts_per_pixel_x=scene.response_counts_per_pixel_y=2.5;
        scene.configured_counts_per_pixel_x=scene.configured_counts_per_pixel_y=2.5;
        const auto result=run(scene);
        expect(result.aligned&&!result.canceled,"局部平移必须通过真实匹配及闭环收敛：起点="+
            std::to_string(offset.x)+","+std::to_string(offset.y)+" 最终="+std::to_string(scene.x)+","+
            std::to_string(scene.y)+" commands="+std::to_string(scene.moves)+" "+result.error);
        expect(std::max(std::abs(scene.x),std::abs(scene.y))<=0.4,
            "成功必须实际接近目标，不能只检查aligned标志");
        expect(scene.opened==1&&scene.closed==1,"采集器须成对关闭");
        for (const auto& command:scene.commands) expect(std::hypot(command.dx_counts,command.dy_counts)<=4,
            "每步输出不得超过现有向量上限");
        expect(result.evidence.value("frames",0)>=3,"对齐结果必须包含多个新鲜帧");
        expect(result.evidence.value("elapsed_ms",9999.0)<=5000,"合成时钟任务不得超过五秒");
    }
    Scene scene;const auto result=run(scene,true);
    expect(result.aligned&&scene.moves==0,"下键复核不得发鼠标命令");
    Scene delayed;delayed.network=true;delayed.close_delay=150ms;
    const auto delayed_result=run(delayed,true);
    expect(delayed_result.aligned&&delayed_result.verified_at==delayed.now-150ms-5ms,
        "验证时间须绑定实际源帧，采集关闭耗时不得刷新其年龄");
    Scene shifted;shifted.x=10;const auto rejected=run(shifted,true);
    expect(!rejected.aligned&&shifted.moves==0,"下键复核偏移应拒绝投掷且不自动对齐");
}
Json precision_control(const cv::Mat& original=texture()) {
    Json evidence=Json::array();
    // 来自Run20/25/29的末端误差；实际原帧未保存，此处以相同残差构造图像输入。
    for (const auto offset:std::array<cv::Point2d,4>{{{-1.3812,0.2678},{-1.4888,-1.1924},
                                                    {-1.0364,-0.6857},{0.8,0}}}) {
        Scene verify;verify.original=original;verify.x=offset.x;verify.y=offset.y;
        const auto before=run(verify,true);
        expect(!before.aligned&&verify.moves==0,"下键不能把一像素以上的剩余偏差当作精确对齐");
        Scene scene;scene.original=original;scene.x=offset.x;scene.y=offset.y;
        scene.response_counts_per_pixel_x=1/0.38;scene.response_counts_per_pixel_y=1/0.55;
        scene.configured_counts_per_pixel_x=0.425;scene.configured_counts_per_pixel_y=0.4;
        const auto result=run(scene);
        expect(result.aligned&&scene.moves>0&&std::max(std::abs(scene.x),std::abs(scene.y))<=0.4,
            "上键须继续消除已记录末段残差："+result.error);
        const auto move_count=scene.moves;
        const auto after=run(scene,true);
        expect(after.aligned&&scene.moves==move_count,"完成细调后下键复核仍为零鼠标输出");
        evidence.push_back({{"initial_x",offset.x},{"initial_y",offset.y},
            {"final_x",scene.x},{"final_y",scene.y},{"moves",move_count},
            {"old_residual_verified",before.aligned},{"alignment",result.evidence},
            {"prethrow",after.evidence}});
    }
    Scene coarse;coarse.original=original;coarse.x=1.1;coarse.y=-1.2;
    const auto unreachable=run(coarse);
    expect(!unreachable.aligned&&coarse.moves>0&&coarse.moves<=48,
        "单count为2.5像素的不可达目标须有界结束，不能虚报精确完成");
    evidence.push_back({{"quantized_unreachable",unreachable.evidence}});
    return evidence;
}
void coupled_subpixel_precision() {
    Scene scene;
    const cv::Mat diagonal=(cv::Mat_<float>(3,3)<<1.f/3,0,0,0,1.f/3,0,0,0,1.f/3);
    cv::filter2D(scene.original,scene.original,-1,diagonal);
    scene.x=-0.4;scene.y=-0.25;
    scene.response_counts_per_pixel_x=1/0.38;scene.response_counts_per_pixel_y=1/0.55;
    scene.configured_counts_per_pixel_x=0.425;scene.configured_counts_per_pixel_y=0.4;
    expect(!run(scene,true).aligned&&scene.moves==0,
        "斜向纹理的相关峰不能低估两轴残差并提前通过");
    const auto aligned=run(scene);
    expect(aligned.aligned&&scene.moves>0&&std::abs(scene.x)<0.1&&std::abs(scene.y)<=0.35,
        "精修须消除可达的水平残差，保持原量化容差："+aligned.error);
    const auto moves=scene.moves;
    expect(run(scene,true).aligned&&scene.moves==moves,"精细对齐后复核不得再发鼠标命令");
}
Json image_coverage(const cv::Mat& original) {
    Json evidence=Json::array();
    for (const bool compressed:{false,true})
        for (const auto offset:std::array<cv::Point2d,7>{{{0,0},{0.8,0},{-1.4,0.3},
                                                     {12,-8},{-12,8},{24,-18},{-24,18}}}) {
            Scene scene;scene.original=original;scene.x=offset.x;scene.y=offset.y;
            scene.compressed=compressed;
            scene.response_counts_per_pixel_x=1/0.38;scene.response_counts_per_pixel_y=1/0.55;
            scene.configured_counts_per_pixel_x=0.425;scene.configured_counts_per_pixel_y=0.4;
            const auto started=Clock::now();
            const auto result=run(scene);
            const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
            expect(result.aligned&&std::max(std::abs(scene.x),std::abs(scene.y))<=0.4,
                "已采图局部偏移须精确收敛 x="+std::to_string(offset.x)+" y="+std::to_string(offset.y)+
                " compressed="+std::to_string(compressed)+" final="+std::to_string(scene.x)+","+
                std::to_string(scene.y)+" "+result.error);
            const auto moves=scene.moves;
            const auto checked=run(scene,true);
            expect(checked.aligned&&scene.moves==moves,"已采图收敛后应通过零位移复核");
            evidence.push_back({{"initial_x",offset.x},{"initial_y",offset.y},{"compressed",compressed},
                {"final_x",scene.x},{"final_y",scene.y},{"wall_ms",elapsed},
                {"alignment",result.evidence},{"prethrow",checked.evidence}});
        }
    return evidence;
}
Json paired_precision(const cv::Mat& original,const cv::Mat& observed) {
    Scene scene;scene.original=original;scene.observed=observed;
    scene.response_counts_per_pixel_x=1/0.38;scene.response_counts_per_pixel_y=1/0.55;
    scene.configured_counts_per_pixel_x=0.425;scene.configured_counts_per_pixel_y=0.4;
    const auto before=run(scene,true);
    expect(!before.aligned&&scene.moves==0,"T4实际停住画面仍有约0.4像素残差，不能提前通过");
    const auto result=run(scene);
    expect(result.aligned&&scene.moves==1&&scene.commands.front().dx_counts==-1&&
        scene.commands.front().dy_counts==0,"实际停点须继续一格水平细调，不能改变垂直轴："+result.error);
    const auto after=run(scene,true);
    expect(after.aligned&&scene.moves==1,"实际停点细调后须通过零输出复核");
    return {{"before",before.evidence},{"alignment",result.evidence},{"prethrow",after.evidence},
        {"simulated_correction_x",scene.x},{"simulated_correction_y",scene.y}};
}
void image_failures() {
    Scene flat;flat.original.setTo(cv::Scalar::all(100));
    expect(!run(flat).aligned&&flat.moves==0,"低纹理画面不能冒充对齐");
    Scene foreign;foreign.unrelated=true;
    expect(!run(foreign).aligned&&foreign.moves==0,"无共同纹理不得输出");
    Scene ambiguous;
    const auto tile=texture()(cv::Rect(0,0,8,8)).clone();
    cv::repeat(tile,40,40,ambiguous.original);
    expect(!run(ambiguous).aligned&&ambiguous.moves==0,"重复纹理的多个相关峰不得被平滑或亚像素处理掩盖");
    Scene outside;outside.x=100;
    expect(!run(outside).aligned&&outside.moves==0,"大幅超出局部搜索范围应失败");
    Scene stuck;stuck.x=20;stuck.no_response=true;
    const auto result=run(stuck);
    expect(!result.aligned&&stuck.moves>0&&stuck.moves<=48,"没有视角响应不得无限输出");
    Scene failure;failure.x=20;failure.command_failed=true;
    expect(!run(failure).aligned&&failure.moves==1,"设备命令失败不得重试");
}
void sparse_texture_coverage() {
    // 两处地图细节都落在原六块固定取样之间；模拟天空中天线/屋檐等稀疏纹理。
    Scene scene;scene.original.setTo(cv::Scalar::all(90));
    const auto detail=texture(219);
    detail(cv::Rect(0,0,24,24)).copyTo(scene.original(cv::Rect(100,220,24,24)));
    detail(cv::Rect(32,0,24,24)).copyTo(scene.original(cv::Rect(224,232,24,24)));
    scene.x=8.4;scene.y=-6.4;
    scene.response_counts_per_pixel_x=scene.response_counts_per_pixel_y=2.5;
    scene.configured_counts_per_pixel_x=scene.configured_counts_per_pixel_y=2.5;
    const auto result=run(scene);
    expect(result.aligned&&scene.moves>0&&std::max(std::abs(scene.x),std::abs(scene.y))<=0.4,
        "稀疏有效纹理落在固定取样之间仍须找点收敛："+result.error);
    const auto moves=scene.moves;
    expect(run(scene,true).aligned&&scene.moves==moves,"稀疏纹理完成后复核不产生位移");
    Scene edge;
    for(int y=0;y<320;++y) for(int x=0;x<320;++x)
        edge.original.at<cv::Vec3b>(y,x)=cv::Vec3b::all(x<110?60:180);
    expect(!run(edge).aligned&&edge.moves==0,"只有一条直边不能提供二维对齐证据");
}
Json clock_recovery() {
    Json evidence=Json::array();
    const auto record=[&](const char* name,const Scene& scene,const grenade_alignment::Result& result) {
        evidence.push_back({{"case",name},{"result",result.evidence},{"grabbed_frames",scene.sequence},
            {"untrusted_clock_moves",scene.untrusted_clock_moves}});
        expect(scene.untrusted_clock_moves==0,std::string(name)+"：无效时钟帧不得发出鼠标命令");
    };
    const auto invalidate=[](FrameTiming& timing,SourceClockStatus status=SourceClockStatus::INVALID) {
        timing.source_time_timing_valid=false;timing.source_clock_status=status;
        timing.source_clock_mapping_age_ms=1234.5;timing.source_clock_uncertainty_ms=7.25;
        timing.source_clock_round_trip_ms=3.5;timing.source_clock_sample_count=8;
    };
    Scene initial;initial.network=true;initial.x=8.4;initial.y=-6.4;
    initial.response_counts_per_pixel_x=initial.response_counts_per_pixel_y=2.5;
    initial.configured_counts_per_pixel_x=initial.configured_counts_per_pixel_y=2.5;
    initial.transform_timing=[&](FrameTiming& timing) {if(initial.sequence<=2) invalidate(timing);};
    const auto initial_result=run(initial);
    expect(initial_result.aligned&&initial.moves>0,"首帧INVALID后恢复须继续找点："+initial_result.error);
    expect(initial_result.evidence.value("clock_rejections",0)==2,"首帧坏时钟计数须保留原始拒绝次数");
    record("initial_invalid_recovery",initial,initial_result);

    Scene moving;moving.network=true;moving.x=20;
    moving.response_counts_per_pixel_x=moving.response_counts_per_pixel_y=2.5;
    moving.configured_counts_per_pixel_x=moving.configured_counts_per_pixel_y=2.5;
    int moves_before_bad=-1,moves_after_bad=-1;
    moving.transform_timing=[&](FrameTiming& timing) {
        if(moving.sequence==3) moves_before_bad=moving.moves;
        if(moving.sequence>=3&&moving.sequence<=5) invalidate(timing,SourceClockStatus::STALE);
        if(moving.sequence==6) moves_after_bad=moving.moves;
    };
    const auto moving_result=run(moving);
    expect(moving_result.aligned&&moves_before_bad>0&&moves_after_bad==moves_before_bad,
        "中途STALE期间必须暂停输出，恢复后继续同次找点："+moving_result.error);
    record("midrun_stale_recovery",moving,moving_result);

    Scene stable;stable.network=true;Clock::time_point recovered_at{};
    stable.transform_timing=[&](FrameTiming& timing) {
        if(stable.sequence==3) invalidate(timing);
        if(stable.sequence==4) recovered_at=timing.source_time_at;
    };
    const auto stable_result=run(stable,true);
    expect(stable_result.aligned&&stable.moves==0&&recovered_at!=Clock::time_point{}&&
        stable_result.verified_at-recovered_at>=80ms&&stable.sequence>=7,
        "坏帧必须清空稳定累计，恢复后重新满足三帧与80ms");
    record("stable_window_reset",stable,stable_result);

    for(const bool after_valid:{false,true}) {
        Scene persistent;persistent.network=true;
        persistent.transform_timing=[&](FrameTiming& timing) {
            if(!after_valid||persistent.sequence>2) invalidate(timing);
        };
        const auto result=run(persistent);
        expect(!result.aligned&&persistent.moves==0&&result.evidence.value("elapsed_ms",9999.0)>=350&&
            result.evidence.value("elapsed_ms",9999.0)<500,
            "持续INVALID须沿350ms无有效帧期限结束，不能立即失败或刷新预算");
        const auto latest=result.evidence.value("last_clock_rejection",Json::object());
        expect(result.evidence.value("clock_rejections",0)>1&&
            latest.value("source_clock_status",-1)==static_cast<int>(SourceClockStatus::INVALID)&&
            latest.value("source_clock_mapping_age_ms",0.0)==1234.5&&
            latest.value("source_clock_uncertainty_ms",0.0)==7.25&&
            latest.value("source_clock_round_trip_ms",0.0)==3.5&&
            latest.value("source_clock_sample_count",0)==8,
            "最终证据须保留最近坏帧的原始时钟质量字段");
        record(after_valid?"persistent_after_valid":"persistent_initial",persistent,result);
    }

    Scene session;session.network=true;
    for(const bool after_valid:{false,true}) {
        Scene late;late.network=true;
        late.transform_timing=[&](FrameTiming& timing) {
            if(late.sequence<=11&&(!after_valid||late.sequence>1)) invalidate(timing);
        };
        const auto result=run(late);
        expect(!result.aligned&&late.moves==0&&late.sequence==12,
            "首张恢复帧超过既有350ms期限也不能重新开始输出");
    }
    session.transform_timing=[&](FrameTiming& timing) {
        if(session.sequence==2) {invalidate(timing,SourceClockStatus::WARMING);timing.source_clock_session_id=2;}
    };
    const auto session_result=run(session);
    expect(!session_result.aligned&&session.sequence==2&&session_result.error=="画面源时钟会话已变化",
        "已知非零源会话变化须立即停止，WARMING不能掩盖换代");
    record("session_change_during_warming",session,session_result);

    Scene missing_session;missing_session.network=true;
    missing_session.transform_timing=[&](FrameTiming& timing) {
        if(missing_session.sequence==2) {invalidate(timing);timing.source_clock_session_id=0;}
        if(missing_session.sequence>2) timing.source_clock_session_id=2;
    };
    const auto missing_result=run(missing_session);
    expect(!missing_result.aligned&&missing_session.sequence==3&&missing_result.error=="画面源时钟会话已变化",
        "未知会话0不得覆盖已确认会话，恢复时仍须识别换代");
    record("unknown_session_preserves_baseline",missing_session,missing_result);

    for(const bool future:{false,true}) {
        Scene age;age.network=true;age.x=20;
        age.transform_timing=[&](FrameTiming& timing) {
            if(age.sequence==1) invalidate(timing);
            else timing.source_time_at=age.now+(future?1ms:-500ms);
        };
        const auto result=run(age);
        expect(!result.aligned&&age.sequence==2&&age.moves==0&&result.error=="道具画面已陈旧，不能用于鼠标输出",
            "时钟状态恢复不能放过未来或陈旧源帧");
        record(future?"recovery_future_frame":"recovery_stale_frame",age,result);
    }
    return evidence;
}
void timing_geometry_and_cancellation() {
    Scene cold;cold.network=true;cold.clock_warming=true;cold.first_frame_at=cold.now+2500ms;
    const auto cold_result=run(cold);
    expect(cold_result.aligned&&cold.moves==0,"NDI冷启动首帧晚于两秒后仍可暖机对齐："+cold_result.error);
    Scene interrupted;interrupted.stop_after_frames=1;
    const auto interrupted_result=run(interrupted);
    expect(!interrupted_result.aligned&&interrupted.moves==0&&
        interrupted_result.evidence.value("elapsed_ms",9999.0)<600,
        "已出帧后的断流仍应在原期限内停止");
    Scene stale;stale.stale=true;
    expect(!run(stale).aligned&&stale.moves==0,"陈旧帧不得输出");
    Scene repeated;repeated.repeated=true;
    expect(!run(repeated).aligned&&repeated.moves==0,"重复帧不能累计稳定帧数");
    Scene geometry;geometry.geometry_change=true;
    expect(!run(geometry).aligned&&geometry.moves==0,"几何变化必须终止");
    Scene receiver;receiver.receiver_change=true;
    expect(!run(receiver).aligned,"采集器换代必须终止");
    Scene network;network.network=true;
    expect(run(network).aligned,"可信网络源时钟可用于复核");
    Scene warming;warming.network=true;warming.clock_warming=true;
    expect(run(warming).aligned&&warming.moves==0,"新NDI采集器可有界等待三次时钟交换且不提前输出");
    Scene clock;clock.network=true;clock.source_clock_missing=true;
    expect(!run(clock).aligned&&clock.moves==0,"缺失网络源时间不得用本机接收序号代替");
    Scene no_frame;no_frame.no_frame=true;
    expect(!run(no_frame).aligned&&no_frame.moves==0,"断流应有界结束");
    Scene canceled;canceled.canceled=true;
    expect(run(canceled).canceled&&canceled.moves==0,"显式取消不得输出");
    Scene emergency;emergency.emergency=true;
    const auto emergency_result=run(emergency);
    expect(emergency_result.canceled&&emergency_result.emergency_stop&&emergency.moves==0&&
        emergency_result.evidence.value("emergency_stop",false),"End须返回全局急停标志且零输出");
    Scene local_cancel;local_cancel.local_cancel=true;
    const auto local_result=run(local_cancel);
    expect(local_result.canceled&&!local_result.emergency_stop&&local_cancel.moves==0&&
        !local_result.evidence.value("emergency_stop",true),"方向左只取消本次操作，不冒充全局急停");
    Scene revoked;revoked.x=20;
    expect(run(revoked,false,[]{return false;}).canceled&&revoked.moves==0,"已撤回许可不得输出");
    Scene focus;focus.focus=false;
    expect(!run(focus).aligned&&focus.moves==0,"失焦不得输出");
    Scene moved;moved.moved=true;
    expect(run(moved).canceled&&moved.moves==0,"手动WASD必须取消");
    Scene physical;physical.read_motion=true;
    expect(run(physical).canceled&&physical.moves==0,"可信物理鼠标流必须取消");
}
std::string digest(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE handle=nullptr;BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    std::array<unsigned char,32> value{};
    BCryptHash(handle,nullptr,0,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),value.data(),32);
    BCryptCloseAlgorithmProvider(handle,0);
    std::ostringstream out;out<<std::hex<<std::setfill('0');
    for (auto byte:value) out<<std::setw(2)<<static_cast<unsigned>(byte);
    return out.str();
}
void save(const std::filesystem::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary);out<<text;
}
void reference_resources(const std::filesystem::path& root) {
    const auto plans=root/"plans", directory=root/"references"/"sample";
    std::filesystem::create_directories(plans);std::filesystem::create_directories(directory);
    movement::timeline::Plan plan;plan.name="已验证烟雾";plan.map="de_mirage";
    movement::timeline::Action action;action.kind=movement::timeline::ActionKind::BUTTON;
    plan.actions.push_back(action);
    const auto expected=movement::timeline::plan_json(plan);
    save(plans/"sample.json",expected.dump());
    std::vector<unsigned char> image;cv::imencode(".png",texture(),image);
    for (const auto name:{"standing.png","aim.png"}) {
        std::ofstream output(directory/name,std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),image.size());
    }
    Json manifest={{"schema_version",1},{"plan",expected},{"manual_throw_verified",true},
        {"standing_image","standing.png"},{"aim_image","aim.png"},
        {"geometry",{{"source_width",320},{"source_height",320},{"roi_width",320},{"roi_height",320},
            {"roi_x",0},{"roi_y",0},{"scale_x",1},{"scale_y",1}}},
        {"image_sha256",{{"standing_image",digest(image)},{"aim_image",digest(image)}}}};
    save(directory/"reference.json",manifest.dump());
    const auto loaded=grenade_alignment::load_reference(plans/"sample.json",expected);
    expect(loaded.revision.size()==64&&loaded.target.x==160&&loaded.aim_image.cols==320,
        "真实资源加载须规范化方案并计算原图摘要");
    const auto rejects=[&](const std::string& message) {
        bool rejected=false;try {grenade_alignment::load_reference(plans/"sample.json",expected);}
        catch (const std::exception&) {rejected=true;}expect(rejected,message);
    };
    std::vector<unsigned char> shared_standing;
    cv::imencode(".png",cv::Mat(360,640,CV_8UC3,cv::Scalar(30,60,90)),shared_standing);
    {
        std::ofstream output(directory/"standing.png",std::ios::binary);
        output.write(reinterpret_cast<const char*>(shared_standing.data()),shared_standing.size());
    }
    manifest["image_sha256"]["standing_image"]=digest(shared_standing);
    manifest["standing_marker"]={{"x",0.25},{"y",0.5},{"label","T1"}};
    save(directory/"reference.json",manifest.dump());
    const auto shared_loaded=grenade_alignment::load_reference(plans/"sample.json",expected);
    expect(shared_loaded.standing_image.size()==cv::Size(640,360)&&shared_loaded.aim_image.size()==cv::Size(320,320),
        "共用站位图可独立分辨率，瞄点图仍绑定采集几何");
    auto single = manifest;
    single.erase("standing_image"); single.erase("standing_marker"); single["image_sha256"].erase("standing_image");
    single["capture_complete"] = false;
    save(directory/"reference.json",single.dump());
    const auto aim_only = grenade_alignment::load_reference(plans/"sample.json",expected);
    expect(aim_only.standing_image.empty() && aim_only.aim_image.cols == 320,
        "F10独立首采不要求站位图，残留未声明PNG不能被拼入");
    single = manifest;
    single.erase("aim_image"); single.erase("geometry"); single["image_sha256"].erase("aim_image");
    single["capture_complete"] = false; single["manual_throw_verified"] = false;
    save(directory/"reference.json",single.dump());
    const auto standing_only = grenade_alignment::load_reference(plans/"sample.json",expected,true);
    expect(standing_only.standing_image.cols == 640 && standing_only.aim_image.empty(),
        "F9独立首采可用于参考展示，不伪造瞄点和试投确认");
    rejects("站位图不能被当作瞄点执行自动找点");
    single["image_sha256"]["standing_image"] = std::string(64,'0');
    save(directory/"reference.json",single.dump());
    bool partial_rejected = false;
    try { grenade_alignment::load_reference(plans/"sample.json",expected,true); }
    catch (const std::exception&) { partial_rejected = true; }
    expect(partial_rejected,"独立站位展示同样核对原图摘要");
    single = manifest; single["aim_plan"] = expected;
    single["aim_plan"]["name"] = "旧瞄点所属方案";
    save(directory/"reference.json",single.dump());
    const auto prior_aim = grenade_alignment::load_reference(plans/"sample.json",expected,true);
    expect(!prior_aim.aim_image.empty() && !prior_aim.standing_image.empty(),
        "F9保留旧方案瞄点图仍可展示两张原图");
    rejects("F9不能把保留的旧瞄点重新绑定为当前方案");
    single["aim_plan"] = expected; save(directory/"reference.json",single.dump());
    expect(!grenade_alignment::load_reference(plans/"sample.json",expected).aim_image.empty(),
        "F10更新瞄点与所属方案后恢复用于找点");
    auto changed=manifest;changed["standing_marker"]["x"]=-0.01;save(directory/"reference.json",changed.dump());
    rejects("站位标记越界必须拒绝");
    changed=manifest;changed["standing_marker"]["x"]=true;save(directory/"reference.json",changed.dump());
    rejects("站位标记不能把布尔值当坐标");
    changed=manifest;changed["standing_marker"]["label"]="T1\n";save(directory/"reference.json",changed.dump());
    rejects("站位标记名称不能包含控制字符");
    {
        std::ofstream output(directory/"aim.png",std::ios::binary);
        output.write(reinterpret_cast<const char*>(shared_standing.data()),shared_standing.size());
    }
    changed=manifest;changed["image_sha256"]["aim_image"]=digest(shared_standing);
    save(directory/"reference.json",changed.dump());
    rejects("允许站位图尺寸独立不能放宽瞄点图几何约束");
    {
        std::ofstream output(directory/"aim.png",std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),image.size());
    }
    changed=manifest;changed["manual_throw_verified"]=false;save(directory/"reference.json",changed.dump());
    rejects("未人工通过的资源不得加载");
    changed=manifest;changed["plan"]["name"]="另一方案";save(directory/"reference.json",changed.dump());
    rejects("参考资源不能绑定到不同方案");
    changed=manifest;changed["image_sha256"]["aim_image"]=std::string(64,'0');save(directory/"reference.json",changed.dump());
    rejects("图像字节变化必须拒绝");
    changed=manifest;changed["aim_image"]="../aim.png";save(directory/"reference.json",changed.dump());
    rejects("参考资源不得逃出固定目录");
    changed=manifest;changed["geometry"]["scale_x"]=2;save(directory/"reference.json",changed.dump());
    rejects("资源几何越界必须拒绝");
    save(directory/"reference.json",manifest.dump());
    auto disk=expected;disk["actions"][0]["duration_ms"]=123;save(plans/"sample.json",disk.dump());
    rejects("磁盘方案变化不能沿用旧资源");
}
}

int main(int argc,char** argv) {
    try {
        if (argc==7&&std::string(argv[1])=="--reference-image"&&
            std::string(argv[3])=="--observed-image"&&std::string(argv[5])=="--report") {
            const auto roi=[](const char* path) {
                const auto image=cv::imread(path);
                if(image.size()==cv::Size(320,320)) return image;
                if(image.size()==cv::Size(2560,1440)) return image(cv::Rect(1120,560,320,320)).clone();
                throw std::runtime_error("实际停点回放要求320 ROI或2560×1440原图");
            };
            save(argv[6],paired_precision(roi(argv[2]),roi(argv[4])).dump(2));
            return failures?1:0;
        }
        if (argc==5&&std::string(argv[1])=="--precision-image"&&std::string(argv[3])=="--report") {
            const auto image=cv::imread(argv[2]);
            if (image.cols!=2560||image.rows!=1440) throw std::runtime_error("实图精度回放要求本次2560×1440原图");
            const auto report=precision_control(image(cv::Rect(1120,560,320,320)).clone());
            save(argv[4],report.dump(2));
            return failures?1:0;
        }
        if (argc==5&&std::string(argv[1])=="--coverage-image"&&std::string(argv[3])=="--report") {
            const auto image=cv::imread(argv[2]);
            if (image.cols!=2560||image.rows!=1440) throw std::runtime_error("实图覆盖回放要求本次2560×1440原图");
            save(argv[4],image_coverage(image(cv::Rect(1120,560,320,320)).clone()).dump(2));
            return failures?1:0;
        }
        aligned_and_control();precision_control();coupled_subpixel_precision();image_failures();sparse_texture_coverage();timing_geometry_and_cancellation();
        const auto clock_evidence=clock_recovery();
        const auto root=argc==2?std::filesystem::path(argv[1]):
            argc==3&&std::string(argv[1])=="--output-root"?std::filesystem::path(argv[2]):
            std::filesystem::path("temp/XEN-GRENADE-ALIGN-20261026-001/reference-tests");
        reference_resources(root);
        save(root/"clock-recovery.json",clock_evidence.dump(2));
    } catch (const std::exception& error) {++failures;std::cerr<<"[异常] "<<error.what()<<'\n';}
    if (!failures) std::cout<<"grenade alignment tests passed\n";
    return failures?1:0;
}
