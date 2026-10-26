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
    double x=0,y=0;
    std::uint64_t sequence=0;
    bool stale=false,repeated=false,geometry_change=false,receiver_change=false;
    bool no_frame=false,no_response=false,unrelated=false,network=false,source_clock_missing=false;
    bool focus=true,moved=false,read_motion=false,command_failed=false,clock_warming=false;
    bool emergency=false,local_cancel=false;
    std::atomic_bool canceled{false};
    int moves=0,opened=0,closed=0;
    std::chrono::milliseconds close_delay{0};
    Clock::time_point first_frame_at{};
    int stop_after_frames=0;
    std::vector<MouseMoveCommand> commands;
};
class Device final : public IMouseController {
public:
    explicit Device(Scene& scene):scene_(scene) {}
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand& value) noexcept override {
        ++scene_.moves;scene_.commands.push_back(value);
        if (!scene_.no_response && !scene_.command_failed) {scene_.x-=value.dx_counts/0.4;scene_.y-=value.dy_counts/0.4;}
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
        frame.bgr=scene_.unrelated?texture(7788):translated(scene_.original,scene_.x,scene_.y);
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
    result.aim.counts_per_pixel_x=result.aim.counts_per_pixel_y=0.4f;
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
        const auto result=run(scene);
        expect(result.aligned&&!result.canceled,"局部平移必须通过真实匹配及闭环收敛：起点="+
            std::to_string(offset.x)+","+std::to_string(offset.y)+" 最终="+std::to_string(scene.x)+","+
            std::to_string(scene.y)+" commands="+std::to_string(scene.moves)+" "+result.error);
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
        aligned_and_control();image_failures();timing_geometry_and_cancellation();
        const auto root=argc==2?std::filesystem::path(argv[1]):
            argc==3&&std::string(argv[1])=="--output-root"?std::filesystem::path(argv[2]):
            std::filesystem::path("temp/XEN-GRENADE-ALIGN-20261026-001/reference-tests");
        reference_resources(root);
    } catch (const std::exception& error) {++failures;std::cerr<<"[异常] "<<error.what()<<'\n';}
    if (!failures) std::cout<<"grenade alignment tests passed\n";
    return failures?1:0;
}
